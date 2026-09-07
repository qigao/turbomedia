#include "ivr_whip_transport.h"
#include "ivr_http_media_client.h"
#include "ivr_internal.h"
#include "ivr_thread.h"
#include "turbo_peer_connection.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IVR_WHIP_DEFAULT_SAMPLE_RATE 16000u

/* ------------------------------------------------------------------ */
/* transport state                                                     */
/* ------------------------------------------------------------------ */

struct ivr_whip_transport_s {
    ivr_whip_transport_config_t config;
    ivr_http_media_client_t *http_client;
    ivr_mutex_t lock;
    ivr_mutex_t lifecycle_lock;
    int lifecycle_lock_initialized;
    int destroying;
    /* active call */
    int active;
    int terminal;
    ivr_str_t tenant_id;
    ivr_str_t provider_session_id;
    ivr_str_t dialog_id;
    ivr_str_t room_id;
    ivr_str_t call_id;
    uint64_t call_generation;
    uint64_t expected_room_version;
    uint64_t attempt_generation;
    ivr_media_state_fn on_state;
    void *state_context;
    /* media */
    turbo_peer_connection_t *pc;
    turbo_media_track_t *audio_track;
    int connected;
    uint64_t frames_sent;
    char session_location[512];
    char session_etag[128];
    /* poll thread */
    ivr_thread_t poll_thread;
    int poll_stop;
    int poll_started;
};

typedef struct {
    ivr_call_ref_t view;
    char tenant_id[IVR_MEDIA_ID_CAPACITY];
    char provider_session_id[IVR_MEDIA_ID_CAPACITY];
    char dialog_id[IVR_MEDIA_ID_CAPACITY];
    char room_id[IVR_MEDIA_ID_CAPACITY];
    char call_id[IVR_MEDIA_ID_CAPACITY];
} whip_call_copy_t;

static int call_view_valid(const ivr_bytes_view_t *view) {
    return view && view->data && view->size > 0u &&
           view->size < IVR_MEDIA_ID_CAPACITY;
}

static int call_view_matches(const ivr_bytes_view_t *view,
                             const ivr_str_t *owned) {
    return view && owned && view->size == owned->size &&
           (view->size == 0u ||
            (view->data && owned->data &&
             memcmp(view->data, owned->data, view->size) == 0));
}

/* Caller holds lock or has crossed the lifecycle barrier and is quiescent. */
static void whip_clear_call(ivr_whip_transport_t *transport) {
    ivr_str_free(&transport->tenant_id);
    ivr_str_free(&transport->provider_session_id);
    ivr_str_free(&transport->dialog_id);
    ivr_str_free(&transport->room_id);
    ivr_str_free(&transport->call_id);
    transport->call_generation = 0u;
    transport->expected_room_version = 0u;
}

static int whip_call_matches_locked(const ivr_whip_transport_t *transport,
                                    const ivr_call_ref_t *call) {
    return transport->active && call &&
           call->call_generation == transport->call_generation &&
           call_view_matches(&call->tenant_id, &transport->tenant_id) &&
           call_view_matches(&call->provider_session_id,
                             &transport->provider_session_id) &&
           call_view_matches(&call->dialog_id, &transport->dialog_id) &&
           call_view_matches(&call->room_id, &transport->room_id) &&
           call_view_matches(&call->call_id, &transport->call_id);
}

static int whip_copy_call_locked(const ivr_whip_transport_t *transport,
                                 whip_call_copy_t *copy) {
    if (!transport || !copy || !transport->tenant_id.data ||
        !transport->provider_session_id.data ||
        !transport->dialog_id.data || !transport->room_id.data ||
        !transport->call_id.data ||
        transport->tenant_id.size >= sizeof(copy->tenant_id) ||
        transport->provider_session_id.size >=
            sizeof(copy->provider_session_id) ||
        transport->dialog_id.size >= sizeof(copy->dialog_id) ||
        transport->room_id.size >= sizeof(copy->room_id) ||
        transport->call_id.size >= sizeof(copy->call_id)) {
        return 0;
    }
    memset(copy, 0, sizeof(*copy));
    memcpy(copy->tenant_id, transport->tenant_id.data,
           transport->tenant_id.size);
    memcpy(copy->provider_session_id, transport->provider_session_id.data,
           transport->provider_session_id.size);
    memcpy(copy->dialog_id, transport->dialog_id.data,
           transport->dialog_id.size);
    memcpy(copy->room_id, transport->room_id.data, transport->room_id.size);
    memcpy(copy->call_id, transport->call_id.data, transport->call_id.size);
    copy->view.tenant_id.data = copy->tenant_id;
    copy->view.tenant_id.size = transport->tenant_id.size;
    copy->view.provider_session_id.data = copy->provider_session_id;
    copy->view.provider_session_id.size = transport->provider_session_id.size;
    copy->view.dialog_id.data = copy->dialog_id;
    copy->view.dialog_id.size = transport->dialog_id.size;
    copy->view.room_id.data = copy->room_id;
    copy->view.room_id.size = transport->room_id.size;
    copy->view.call_id.data = copy->call_id;
    copy->view.call_id.size = transport->call_id.size;
    copy->view.call_generation = transport->call_generation;
    copy->view.expected_room_version = transport->expected_room_version;
    return 1;
}

static void whip_emit_state(ivr_whip_transport_t *t,
                            ivr_media_link_state_t state, int error_code) {
    ivr_media_state_fn callback;
    void *context;
    whip_call_copy_t call;
    uint64_t generation;
    int have_call;
    if (!t) {
        return;
    }
    ivr_mutex_lock(&t->lock);
    callback = t->on_state;
    context = t->state_context;
    generation = t->attempt_generation;
    have_call = whip_copy_call_locked(t, &call);
    ivr_mutex_unlock(&t->lock);
    if (callback && have_call) {
        callback(context, &call.view, generation, state, error_code);
    }
}

static int whip_delete_session(ivr_whip_transport_t *t,
                               const char *location) {
    ivr_http_media_response_t resp;
    int request_status;

    if (!t || !location || location[0] == '\0') {
        return 0;
    }
    memset(&resp, 0, sizeof(resp));
    request_status = ivr_http_media_request(
        t->http_client, "DELETE", location, NULL, NULL, NULL, &resp);
    if (request_status != 0 ||
        (resp.status != 200 && resp.status != 204 && resp.status != 404)) {
        fprintf(stderr,
                "[ivr_whip] delete session rc=%d status=%d location=%s\n",
                request_status, resp.status, location);
        return -1;
    }
    return 0;
}

static void whip_state_change(turbo_peer_connection_t *pc,
                              turbo_peer_state_t state, void *user_data) {
    ivr_whip_transport_t *t = (ivr_whip_transport_t *)user_data;
    turbo_media_track_t *track = NULL;
    int emit = 1;
    (void)pc;
    if (state == TURBO_PEER_STATE_CONNECTED) {
        ivr_mutex_lock(&t->lock);
        if (t->terminal) {
            emit = 0;
        } else {
            t->connected = 1;
            track = t->audio_track;
        }
        ivr_mutex_unlock(&t->lock);
        if (track) {
            (void)turbo_media_track_start(track);
        }
        if (emit) {
            whip_emit_state(t, IVR_MEDIA_LINK_CONNECTED,
                            IVR_MEDIA_ERROR_NONE);
        }
    } else if (state == TURBO_PEER_STATE_DISCONNECTED) {
        ivr_mutex_lock(&t->lock);
        t->connected = 0;
        t->terminal = 1;
        ivr_mutex_unlock(&t->lock);
        whip_emit_state(t, IVR_MEDIA_LINK_DISCONNECTED, IVR_MEDIA_ERROR_NONE);
    } else if (state == TURBO_PEER_STATE_FAILED) {
        ivr_mutex_lock(&t->lock);
        t->connected = 0;
        t->terminal = 1;
        ivr_mutex_unlock(&t->lock);
        whip_emit_state(t, IVR_MEDIA_LINK_FAILED, IVR_MEDIA_ERROR_PEER_FAILED);
    } else if (state == TURBO_PEER_STATE_CLOSED) {
        ivr_mutex_lock(&t->lock);
        t->connected = 0;
        t->terminal = 1;
        ivr_mutex_unlock(&t->lock);
        whip_emit_state(t, IVR_MEDIA_LINK_CLOSED, IVR_MEDIA_ERROR_STOPPED);
    }
}

static void whip_on_track(turbo_peer_connection_t *pc,
                          turbo_media_track_t *track, void *user_data) {
    (void)pc;
    (void)track;
    (void)user_data;
}

static void *whip_poll_thread_main(void *opaque) {
    ivr_whip_transport_t *t = (ivr_whip_transport_t *)opaque;
    uint64_t deadline = t->config.connect_timeout_ms
                            ? t->config.connect_timeout_ms
                            : 10000;
    uint64_t waited = 0;
    while (waited < deadline) {
        ivr_mutex_lock(&t->lock);
        int stop = t->poll_stop;
        int connected = t->connected;
        turbo_peer_connection_t *pc = t->pc;
        ivr_mutex_unlock(&t->lock);
        if (stop || !pc) {
            break;
        }
        turbo_peer_connection_poll(pc);
        ivr_thread_sleep_ms(10);
        if (!connected) {
            waited += 10;
            if (waited >= deadline) {
                break;
            }
        }
    }
    ivr_mutex_lock(&t->lock);
    int timed_out = !t->poll_stop && t->active && !t->connected;
    ivr_mutex_unlock(&t->lock);
    if (timed_out) {
        whip_emit_state(t, IVR_MEDIA_LINK_FAILED,
                        IVR_MEDIA_ERROR_CONNECT_TIMEOUT);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

ivr_status_t ivr_whip_transport_create(const ivr_whip_transport_config_t *config,
                                       ivr_whip_transport_t **out_transport) {
    ivr_http_media_client_config_t http_config =
        IVR_HTTP_MEDIA_CLIENT_CONFIG_INIT;
    if (!config || !config->sfu_base_url || !out_transport) {
        return IVR_EINVAL;
    }
    ivr_whip_transport_t *t =
        (ivr_whip_transport_t *)calloc(1, sizeof(*t));
    if (!t) {
        return IVR_ENOSPC;
    }
    t->config = *config;
    t->config.sample_rate = config->sample_rate
                                ? config->sample_rate
                                : IVR_WHIP_DEFAULT_SAMPLE_RATE;
    t->config.sfu_base_url = NULL;
    t->config.media_token = NULL;
    t->config.ca_file = NULL;
    t->config.cert_file = NULL;
    t->config.key_file = NULL;
    t->config.key_password = NULL;
    http_config.base_url = config->sfu_base_url;
    http_config.media_token = config->media_token;
    http_config.ca_file = config->ca_file;
    http_config.cert_file = config->cert_file;
    http_config.key_file = config->key_file;
    http_config.key_password = config->key_password;
    http_config.timeout_ms = config->http_timeout_ms;
    http_config.allow_plaintext_loopback = config->allow_plaintext_loopback;
    if (ivr_http_media_client_create(&http_config, &t->http_client) != 0) {
        free(t);
        return IVR_EINVAL;
    }
    t->on_state = config->on_state;
    t->state_context = config->state_context;
    ivr_str_init(&t->tenant_id);
    ivr_str_init(&t->provider_session_id);
    ivr_str_init(&t->dialog_id);
    ivr_str_init(&t->room_id);
    ivr_str_init(&t->call_id);
    if (ivr_mutex_init(&t->lock) != 0) {
        ivr_http_media_client_destroy(t->http_client);
        free(t);
        return IVR_ENOSPC;
    }
    if (ivr_mutex_init(&t->lifecycle_lock) != 0) {
        ivr_mutex_destroy(&t->lock);
        ivr_http_media_client_destroy(t->http_client);
        free(t);
        return IVR_ENOSPC;
    }
    t->lifecycle_lock_initialized = 1;
    *out_transport = t;
    return IVR_OK;
}

static int whip_play_audio(void *ctx, const ivr_call_ref_t *call,
                           const uint8_t *pcm, size_t len,
                           uint32_t sample_rate) {
    ivr_whip_transport_t *t = (ivr_whip_transport_t *)ctx;
    if (!t || !call || (!pcm && len > 0)) {
        return -1;
    }
    ivr_mutex_lock(&t->lifecycle_lock);
    if (t->destroying) {
        ivr_mutex_unlock(&t->lifecycle_lock);
        return -1;
    }
    ivr_mutex_lock(&t->lock);
    int active = whip_call_matches_locked(t, call);
    turbo_media_track_t *track = t->audio_track;
    int connected = t->connected;
    ivr_mutex_unlock(&t->lock);
    if (!active || !connected || !track) {
        ivr_mutex_unlock(&t->lifecycle_lock);
        return -1;
    }
    turbo_speech_audio_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.data = pcm;
    frame.len = len;
    frame.format.sample_rate = (int)sample_rate;
    frame.format.channels = 1;
    frame.format.bits_per_sample = 16;
    frame.timestamp_us = 0; /* track owns RTP timestamp */
    if (turbo_media_track_send_speech_frame(track, &frame) != 0) {
        ivr_mutex_unlock(&t->lifecycle_lock);
        return -1;
    }
    ivr_mutex_lock(&t->lock);
    t->frames_sent++;
    ivr_mutex_unlock(&t->lock);
    ivr_mutex_unlock(&t->lifecycle_lock);
    return 0;
}

static int whip_stop_impl(ivr_whip_transport_t *t) {
    char location[512];
    ivr_mutex_lock(&t->lock);
    if (!t->active) {
        ivr_mutex_unlock(&t->lock);
        return 0;
    }
    snprintf(location, sizeof(location), "%s", t->session_location);
    t->active = 0;
    t->connected = 0;
    t->poll_stop = 1;
    turbo_peer_connection_t *pc = t->pc;
    t->pc = NULL;
    t->audio_track = NULL;
    whip_clear_call(t);
    ivr_mutex_unlock(&t->lock);
    if (t->poll_started) {
        ivr_thread_join(&t->poll_thread);
        t->poll_started = 0;
    }
    int delete_status = whip_delete_session(t, location);
    if (pc) {
        turbo_peer_connection_destroy(pc);
    }
    return delete_status;
}

static int whip_stop(void *ctx, const ivr_call_ref_t *call) {
    ivr_whip_transport_t *t = (ivr_whip_transport_t *)ctx;
    int rc;
    int active;
    int matches;

    if (!t || !call) {
        return -1;
    }
    ivr_mutex_lock(&t->lifecycle_lock);
    if (t->destroying) {
        ivr_mutex_unlock(&t->lifecycle_lock);
        return -1;
    }
    ivr_mutex_lock(&t->lock);
    active = t->active;
    matches = whip_call_matches_locked(t, call);
    ivr_mutex_unlock(&t->lock);
    rc = !active ? 0 : (matches ? whip_stop_impl(t) : -1);
    ivr_mutex_unlock(&t->lifecycle_lock);
    return rc;
}

void ivr_whip_transport_get_transport(ivr_whip_transport_t *transport,
                                      ivr_media_transport_t *out) {
    if (!transport || !out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->context = transport;
    out->play_audio = whip_play_audio;
    out->stop = whip_stop;
}

ivr_status_t ivr_whip_transport_start(ivr_whip_transport_t *t,
                                      const ivr_call_ref_t *call) {
    if (!t || !call || !call_view_valid(&call->tenant_id) ||
        !call_view_valid(&call->provider_session_id) ||
        !call_view_valid(&call->dialog_id) ||
        !call_view_valid(&call->room_id) ||
        !call_view_valid(&call->call_id) || call->call_generation == 0u) {
        return IVR_EINVAL;
    }
    ivr_mutex_lock(&t->lifecycle_lock);
    if (t->destroying) {
        ivr_mutex_unlock(&t->lifecycle_lock);
        return IVR_ESTATE;
    }
    ivr_mutex_lock(&t->lock);
    if (t->active) {
        ivr_mutex_unlock(&t->lock);
        ivr_mutex_unlock(&t->lifecycle_lock);
        return IVR_ESTATE;
    }
    if (ivr_str_assign(&t->tenant_id, call->tenant_id.data,
                       call->tenant_id.size) < 0 ||
        ivr_str_assign(&t->provider_session_id,
                       call->provider_session_id.data,
                       call->provider_session_id.size) < 0 ||
        ivr_str_assign(&t->dialog_id, call->dialog_id.data,
                       call->dialog_id.size) < 0 ||
        ivr_str_assign(&t->room_id, call->room_id.data, call->room_id.size) < 0 ||
        ivr_str_assign(&t->call_id, call->call_id.data, call->call_id.size) < 0) {
        whip_clear_call(t);
        ivr_mutex_unlock(&t->lock);
        ivr_mutex_unlock(&t->lifecycle_lock);
        return IVR_ENOSPC;
    }
    t->call_generation = call->call_generation;
    t->expected_room_version = call->expected_room_version;
    t->attempt_generation++;
    t->active = 1;
    t->connected = 0;
    t->terminal = 0;
    t->poll_stop = 0;
    ivr_mutex_unlock(&t->lock);
    whip_emit_state(t, IVR_MEDIA_LINK_CONNECTING, IVR_MEDIA_ERROR_NONE);

    turbo_peer_config_t pc_cfg;
    memset(&pc_cfg, 0, sizeof(pc_cfg));
    pc_cfg.allow_loopback = t->config.allow_loopback;
    pc_cfg.disable_datachannel = 1;
    pc_cfg.user_data = t;
    turbo_peer_callbacks_t cb;
    memset(&cb, 0, sizeof(cb));
    cb.on_state_change = whip_state_change;
    cb.on_track = whip_on_track;
    turbo_peer_connection_t *pc = turbo_peer_connection_create(&pc_cfg, &cb);
    if (!pc) {
        fprintf(stderr, "[ivr_whip] start failed stage=peer_create\n");
        ivr_mutex_lock(&t->lock);
        t->active = 0;
        whip_clear_call(t);
        ivr_mutex_unlock(&t->lock);
        ivr_mutex_unlock(&t->lifecycle_lock);
        return IVR_ENOSPC;
    }
    turbo_media_track_config_t track_cfg;
    memset(&track_cfg, 0, sizeof(track_cfg));
    track_cfg.type = TURBO_RTC_MEDIA_TRACK_AUDIO;
    track_cfg.direction = TURBO_MEDIA_DIRECTION_SENDONLY;
    track_cfg.codec = TURBO_CODEC_OPUS;
    track_cfg.audio.sample_rate = (int)t->config.sample_rate;
    track_cfg.audio.channels = 1;
    track_cfg.audio.frame_size_ms = 20;
    turbo_media_track_t *audio_track =
        turbo_peer_connection_add_track_ex(pc, &track_cfg);
    if (!audio_track) {
        fprintf(stderr, "[ivr_whip] start failed stage=add_audio_track\n");
        turbo_peer_connection_destroy(pc);
        ivr_mutex_lock(&t->lock);
        t->active = 0;
        whip_clear_call(t);
        ivr_mutex_unlock(&t->lock);
        ivr_mutex_unlock(&t->lifecycle_lock);
        return IVR_ENOSPC;
    }

    char offer[IVR_HTTP_MEDIA_MAX_SDP];
    if (turbo_peer_connection_create_offer(pc, offer, sizeof(offer)) <= 0) {
        fprintf(stderr, "[ivr_whip] start failed stage=create_offer\n");
        turbo_peer_connection_destroy(pc);
        ivr_mutex_lock(&t->lock);
        t->active = 0;
        whip_clear_call(t);
        ivr_mutex_unlock(&t->lock);
        ivr_mutex_unlock(&t->lifecycle_lock);
        return IVR_ESTATE;
    }

    char path[IVR_MEDIA_ID_CAPACITY * 6u + 16u];
    char *room_segment = ivr_http_media_encode_path_segment(t->room_id.data);
    char *call_segment = ivr_http_media_encode_path_segment(t->call_id.data);
    int path_length;
    if (!room_segment || !call_segment) {
        free(room_segment);
        free(call_segment);
        turbo_peer_connection_destroy(pc);
        ivr_mutex_lock(&t->lock);
        t->active = 0;
        whip_clear_call(t);
        ivr_mutex_unlock(&t->lock);
        ivr_mutex_unlock(&t->lifecycle_lock);
        return IVR_EINVAL;
    }
    path_length = snprintf(path, sizeof(path), "/whip/%s/%s", room_segment,
                           call_segment);
    free(room_segment);
    free(call_segment);
    if (path_length < 0 || (size_t)path_length >= sizeof(path)) {
        turbo_peer_connection_destroy(pc);
        ivr_mutex_lock(&t->lock);
        t->active = 0;
        whip_clear_call(t);
        ivr_mutex_unlock(&t->lock);
        ivr_mutex_unlock(&t->lifecycle_lock);
        return IVR_EINVAL;
    }
    ivr_http_media_response_t resp;
    memset(&resp, 0, sizeof(resp));
    /* Build a minimal WHIP offer from the peer connection ICE credentials and
       fingerprint (candidates go via PATCH; the SFU answers a minimal offer). */
    char offer_min[IVR_HTTP_MEDIA_MAX_SDP];
    ivr_sdp_build_minimal_audio_offer(offer, offer_min, sizeof(offer_min),
                                      (int)t->config.sample_rate, "sendonly");
    int whip_rc = ivr_http_media_request(t->http_client, "POST", path,
                                         "application/sdp", NULL, offer_min,
                                         &resp);
    if (whip_rc != 0 ||
        resp.status != 201 || resp.location[0] == '\0' ||
        resp.etag[0] == '\0' || resp.body[0] == '\0') {
        fprintf(stderr,
                "[ivr_whip] start failed stage=post rc=%d status=%d "
                "location=%d etag=%d body=%d\n",
                whip_rc, resp.status, resp.location[0] != '\0',
                resp.etag[0] != '\0', resp.body[0] != '\0');
        turbo_peer_connection_destroy(pc);
        ivr_mutex_lock(&t->lock);
        t->active = 0;
        whip_clear_call(t);
        ivr_mutex_unlock(&t->lock);
        ivr_mutex_unlock(&t->lifecycle_lock);
        return IVR_ESTATE;
    }
    if (turbo_peer_connection_set_remote_description(pc, "answer",
                                                     resp.body) != 0) {
        fprintf(stderr, "[ivr_whip] start failed stage=remote_description\n");
        (void)whip_delete_session(t, resp.location);
        turbo_peer_connection_destroy(pc);
        ivr_mutex_lock(&t->lock);
        t->active = 0;
        whip_clear_call(t);
        ivr_mutex_unlock(&t->lock);
        ivr_mutex_unlock(&t->lifecycle_lock);
        return IVR_ESTATE;
    }
    /* trickle the client ICE credentials AND local candidates to the SFU
       (If-Match = ETag); WHIP keeps candidates out of the offer */
    char fragment[2048];
    char ufrag[64];
    char pwd[96];
    if (ivr_sdp_attr_value(offer, "a=ice-ufrag:", ufrag, sizeof(ufrag)) &&
        ivr_sdp_attr_value(offer, "a=ice-pwd:", pwd, sizeof(pwd))) {
        snprintf(fragment, sizeof(fragment),
                 "a=ice-ufrag:%s\r\na=ice-pwd:%s\r\n", ufrag, pwd);
        size_t used = strlen(fragment);
        ivr_sdp_append_candidates(offer, fragment + used,
                                  sizeof(fragment) - used);
        ivr_http_media_response_t trickle;
        memset(&trickle, 0, sizeof(trickle));
        int patch_rc = ivr_http_media_request(
            t->http_client, "PATCH", resp.location,
            "application/trickle-ice-sdpfrag", resp.etag, fragment,
            &trickle);
        if (patch_rc != 0 ||
            (trickle.status != 200 && trickle.status != 204)) {
            fprintf(stderr,
                    "[ivr_whip] start failed stage=trickle_request rc=%d "
                    "status=%d etag=%s\n",
                    patch_rc, trickle.status, resp.etag);
            (void)whip_delete_session(t, resp.location);
            turbo_peer_connection_destroy(pc);
            ivr_mutex_lock(&t->lock);
            t->active = 0;
            whip_clear_call(t);
            ivr_mutex_unlock(&t->lock);
            ivr_mutex_unlock(&t->lifecycle_lock);
            return IVR_ESTATE;
        }
        if (trickle.body[0] &&
            turbo_peer_connection_apply_remote_ice_sdpfrag(
                pc, trickle.body, strlen(trickle.body)) != 0) {
            fprintf(stderr,
                    "[ivr_whip] start failed stage=apply_remote_sdpfrag\n");
            (void)whip_delete_session(t, resp.location);
            turbo_peer_connection_destroy(pc);
            ivr_mutex_lock(&t->lock);
            t->active = 0;
            whip_clear_call(t);
            ivr_mutex_unlock(&t->lock);
            ivr_mutex_unlock(&t->lifecycle_lock);
            return IVR_ESTATE;
        }
    }
    ivr_mutex_lock(&t->lock);
    snprintf(t->session_location, sizeof(t->session_location), "%s",
             resp.location);
    snprintf(t->session_etag, sizeof(t->session_etag), "%s", resp.etag);
    t->pc = pc;
    t->audio_track = audio_track;
    ivr_mutex_unlock(&t->lock);
    if (ivr_thread_create(&t->poll_thread, whip_poll_thread_main, t) != 0) {
        (void)whip_stop_impl(t);
        ivr_mutex_unlock(&t->lifecycle_lock);
        return IVR_ENOSPC;
    }
    t->poll_started = 1;
    ivr_mutex_unlock(&t->lifecycle_lock);
    return IVR_OK;
}

int ivr_whip_transport_connected(const ivr_whip_transport_t *t) {
    if (!t) {
        return 0;
    }
    ivr_mutex_lock((ivr_mutex_t *)&t->lifecycle_lock);
    ivr_mutex_lock((ivr_mutex_t *)&t->lock);
    int connected = t->connected;
    ivr_mutex_unlock((ivr_mutex_t *)&t->lock);
    ivr_mutex_unlock((ivr_mutex_t *)&t->lifecycle_lock);
    return connected;
}

uint32_t ivr_whip_transport_ssrc(const ivr_whip_transport_t *t) {
    uint32_t ssrc = 0;

    if (!t) {
        return 0;
    }
    ivr_mutex_lock((ivr_mutex_t *)&t->lifecycle_lock);
    ivr_mutex_lock((ivr_mutex_t *)&t->lock);
    if (t->audio_track) {
        ssrc = turbo_media_track_get_ssrc(t->audio_track);
    }
    ivr_mutex_unlock((ivr_mutex_t *)&t->lock);
    ivr_mutex_unlock((ivr_mutex_t *)&t->lifecycle_lock);
    return ssrc;
}

uint64_t ivr_whip_transport_frames_sent(const ivr_whip_transport_t *t) {
    if (!t) {
        return 0;
    }
    ivr_mutex_lock((ivr_mutex_t *)&t->lifecycle_lock);
    ivr_mutex_lock((ivr_mutex_t *)&t->lock);
    uint64_t frames = t->frames_sent;
    ivr_mutex_unlock((ivr_mutex_t *)&t->lock);
    ivr_mutex_unlock((ivr_mutex_t *)&t->lifecycle_lock);
    return frames;
}

int ivr_whip_transport_send_rtp_packet(ivr_whip_transport_t *t,
                                       const uint8_t *packet, size_t length) {
    turbo_media_track_t *track;
    int ready;
    int result;
    if (!t || !packet || length < 12u) {
        return -1;
    }
    ivr_mutex_lock(&t->lifecycle_lock);
    if (t->destroying) {
        ivr_mutex_unlock(&t->lifecycle_lock);
        return -1;
    }
    ivr_mutex_lock(&t->lock);
    track = t->audio_track;
    ready = t->active && t->connected && track != NULL;
    ivr_mutex_unlock(&t->lock);
    result = ready ? turbo_media_track_send_rtp_packet(track, packet, length)
                   : -1;
    ivr_mutex_unlock(&t->lifecycle_lock);
    return result;
}

void ivr_whip_transport_destroy(ivr_whip_transport_t *t) {
    if (!t) {
        return;
    }
    ivr_mutex_lock(&t->lifecycle_lock);
    t->destroying = 1;
    (void)whip_stop_impl(t);
    ivr_mutex_unlock(&t->lifecycle_lock);
    whip_clear_call(t);
    if (t->lifecycle_lock_initialized) {
        ivr_mutex_destroy(&t->lifecycle_lock);
        t->lifecycle_lock_initialized = 0;
    }
    ivr_mutex_destroy(&t->lock);
    ivr_http_media_client_destroy(t->http_client);
    free(t);
}
