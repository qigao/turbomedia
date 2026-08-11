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
    /* Owned copies of the config strings (sfu_host / media_token). The
       transport keeps them for its whole lifetime so callers may release
       their config after create(); config.sfu_host / config.media_token point
       at these buffers. */
    char *sfu_host_owned;
    char *media_token_owned;
    ivr_mutex_t lock;
    ivr_mutex_t lifecycle_lock;
    int lifecycle_lock_initialized;
    int destroying;
    /* active call */
    int active;
    ivr_str_t room_id;
    ivr_str_t call_id;
    uint64_t call_generation;
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

static void whip_emit_state(ivr_whip_transport_t *t,
                            ivr_media_link_state_t state, int error_code) {
    ivr_media_state_fn callback;
    void *context;
    ivr_call_ref_t call;
    char room[256];
    char call_id[256];
    uint64_t generation;
    if (!t) {
        return;
    }
    memset(&call, 0, sizeof(call));
    memset(room, 0, sizeof(room));
    memset(call_id, 0, sizeof(call_id));
    ivr_mutex_lock(&t->lock);
    callback = t->on_state;
    context = t->state_context;
    generation = t->attempt_generation;
    if (t->room_id.data && t->room_id.size < sizeof(room) &&
        t->call_id.data && t->call_id.size < sizeof(call_id)) {
        memcpy(room, t->room_id.data, t->room_id.size);
        memcpy(call_id, t->call_id.data, t->call_id.size);
        call.room_id.data = room;
        call.room_id.size = t->room_id.size;
        call.call_id.data = call_id;
        call.call_id.size = t->call_id.size;
        call.call_generation = t->call_generation;
    }
    ivr_mutex_unlock(&t->lock);
    if (callback) {
        callback(context, &call, generation, state, error_code);
    }
}

static void whip_delete_session(ivr_whip_transport_t *t,
                                 const char *location) {
    ivr_http_media_response_t resp;

    if (!t || !location || location[0] == '\0') {
        return;
    }
    memset(&resp, 0, sizeof(resp));
    (void)ivr_http_media_request(t->config.sfu_host, t->config.sfu_port,
                                 "DELETE", location, t->config.media_token,
                                 NULL, NULL, NULL, &resp);
}

static void whip_state_change(turbo_peer_connection_t *pc,
                              turbo_peer_state_t state, void *user_data) {
    ivr_whip_transport_t *t = (ivr_whip_transport_t *)user_data;
    (void)pc;
    if (state == TURBO_PEER_STATE_CONNECTED) {
        ivr_mutex_lock(&t->lock);
        t->connected = 1;
        turbo_media_track_t *track = t->audio_track;
        ivr_mutex_unlock(&t->lock);
        if (track) {
            (void)turbo_media_track_start(track);
        }
        whip_emit_state(t, IVR_MEDIA_LINK_CONNECTED, IVR_MEDIA_ERROR_NONE);
    } else if (state == TURBO_PEER_STATE_DISCONNECTED) {
        ivr_mutex_lock(&t->lock);
        t->connected = 0;
        ivr_mutex_unlock(&t->lock);
        whip_emit_state(t, IVR_MEDIA_LINK_DISCONNECTED, IVR_MEDIA_ERROR_NONE);
    } else if (state == TURBO_PEER_STATE_FAILED) {
        ivr_mutex_lock(&t->lock);
        t->connected = 0;
        ivr_mutex_unlock(&t->lock);
        whip_emit_state(t, IVR_MEDIA_LINK_FAILED, IVR_MEDIA_ERROR_PEER_FAILED);
    } else if (state == TURBO_PEER_STATE_CLOSED) {
        ivr_mutex_lock(&t->lock);
        t->connected = 0;
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
    if (!config || !config->sfu_host || config->sfu_port <= 0 ||
        !out_transport) {
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
    /* Deep-copy the config strings so later start/stop do not dereference
       caller-owned memory that may already have been freed. */
    t->sfu_host_owned = ivr_http_media_strdup(config->sfu_host);
    t->media_token_owned = ivr_http_media_strdup(config->media_token);
    if (!t->sfu_host_owned || (config->media_token && !t->media_token_owned)) {
        free(t->sfu_host_owned);
        free(t->media_token_owned);
        free(t);
        return IVR_ENOSPC;
    }
    t->config.sfu_host = t->sfu_host_owned;
    t->config.media_token = t->media_token_owned;
    t->on_state = config->on_state;
    t->state_context = config->state_context;
    ivr_str_init(&t->room_id);
    ivr_str_init(&t->call_id);
    if (ivr_mutex_init(&t->lock) != 0) {
        free(t->sfu_host_owned);
        free(t->media_token_owned);
        free(t);
        return IVR_ENOSPC;
    }
    if (ivr_mutex_init(&t->lifecycle_lock) != 0) {
        ivr_mutex_destroy(&t->lock);
        free(t->sfu_host_owned);
        free(t->media_token_owned);
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
    int active = t->active &&
                 call->call_generation == t->call_generation &&
                 call->room_id.size == t->room_id.size &&
                 (call->room_id.size == 0 ||
                  memcmp(call->room_id.data, t->room_id.data,
                         call->room_id.size) == 0) &&
                 call->call_id.size == t->call_id.size &&
                 (call->call_id.size == 0 ||
                  memcmp(call->call_id.data, t->call_id.data,
                         call->call_id.size) == 0);
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
    ivr_str_free(&t->room_id);
    ivr_str_free(&t->call_id);
    ivr_mutex_unlock(&t->lock);
    if (t->poll_started) {
        ivr_thread_join(&t->poll_thread);
        t->poll_started = 0;
    }
    whip_delete_session(t, location);
    if (pc) {
        turbo_peer_connection_destroy(pc);
    }
    return 0;
}

static int whip_stop(void *ctx, const ivr_call_ref_t *call) {
    ivr_whip_transport_t *t = (ivr_whip_transport_t *)ctx;
    int rc;

    if (!t || !call) {
        return -1;
    }
    ivr_mutex_lock(&t->lifecycle_lock);
    if (t->destroying) {
        ivr_mutex_unlock(&t->lifecycle_lock);
        return -1;
    }
    rc = whip_stop_impl(t);
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
    if (!t || !call) {
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
    if (ivr_str_assign(&t->room_id, call->room_id.data, call->room_id.size) < 0 ||
        ivr_str_assign(&t->call_id, call->call_id.data, call->call_id.size) < 0) {
        ivr_mutex_unlock(&t->lock);
        ivr_mutex_unlock(&t->lifecycle_lock);
        return IVR_ENOSPC;
    }
    t->call_generation = call->call_generation;
    t->attempt_generation++;
    t->active = 1;
    t->connected = 0;
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
        ivr_mutex_lock(&t->lock);
        t->active = 0;
        ivr_str_free(&t->room_id);
        ivr_str_free(&t->call_id);
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
        turbo_peer_connection_destroy(pc);
        ivr_mutex_lock(&t->lock);
        t->active = 0;
        ivr_str_free(&t->room_id);
        ivr_str_free(&t->call_id);
        ivr_mutex_unlock(&t->lock);
        ivr_mutex_unlock(&t->lifecycle_lock);
        return IVR_ENOSPC;
    }

    char offer[IVR_HTTP_MEDIA_MAX_SDP];
    if (turbo_peer_connection_create_offer(pc, offer, sizeof(offer)) <= 0) {
        turbo_peer_connection_destroy(pc);
        ivr_mutex_lock(&t->lock);
        t->active = 0;
        ivr_str_free(&t->room_id);
        ivr_str_free(&t->call_id);
        ivr_mutex_unlock(&t->lock);
        ivr_mutex_unlock(&t->lifecycle_lock);
        return IVR_ESTATE;
    }

    char path[512];
    snprintf(path, sizeof(path), "/whip/%.*s/%.*s",
             (int)t->room_id.size, t->room_id.data, (int)t->call_id.size,
             t->call_id.data);
    ivr_http_media_response_t resp;
    memset(&resp, 0, sizeof(resp));
    /* Build a minimal WHIP offer from the peer connection ICE credentials and
       fingerprint (candidates go via PATCH; the SFU answers a minimal offer). */
    char offer_min[IVR_HTTP_MEDIA_MAX_SDP];
    ivr_sdp_build_minimal_audio_offer(offer, offer_min, sizeof(offer_min),
                                      (int)t->config.sample_rate, "sendonly");
    int whip_rc = ivr_http_media_request(
        t->config.sfu_host, t->config.sfu_port, "POST", path,
        t->config.media_token, "application/sdp", NULL, offer_min, &resp);
    if (whip_rc != 0 ||
        resp.status != 201 || resp.location[0] == '\0' ||
        resp.etag[0] == '\0' || resp.body[0] == '\0') {
        turbo_peer_connection_destroy(pc);
        ivr_mutex_lock(&t->lock);
        t->active = 0;
        ivr_str_free(&t->room_id);
        ivr_str_free(&t->call_id);
        ivr_mutex_unlock(&t->lock);
        ivr_mutex_unlock(&t->lifecycle_lock);
        return IVR_ESTATE;
    }
    if (turbo_peer_connection_set_remote_description(pc, "answer",
                                                     resp.body) != 0) {
        whip_delete_session(t, resp.location);
        turbo_peer_connection_destroy(pc);
        ivr_mutex_lock(&t->lock);
        t->active = 0;
        ivr_str_free(&t->room_id);
        ivr_str_free(&t->call_id);
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
        if (ivr_http_media_request(
                t->config.sfu_host, t->config.sfu_port, "PATCH",
                resp.location, t->config.media_token,
                "application/trickle-ice-sdpfrag", resp.etag, fragment,
                &trickle) != 0 ||
            (trickle.status != 200 && trickle.status != 204)) {
            whip_delete_session(t, resp.location);
            turbo_peer_connection_destroy(pc);
            ivr_mutex_lock(&t->lock);
            t->active = 0;
            ivr_str_free(&t->room_id);
            ivr_str_free(&t->call_id);
            ivr_mutex_unlock(&t->lock);
            ivr_mutex_unlock(&t->lifecycle_lock);
            return IVR_ESTATE;
        }
        if (trickle.body[0] &&
            turbo_peer_connection_apply_remote_ice_sdpfrag(
                pc, trickle.body, strlen(trickle.body)) != 0) {
            whip_delete_session(t, resp.location);
            turbo_peer_connection_destroy(pc);
            ivr_mutex_lock(&t->lock);
            t->active = 0;
            ivr_str_free(&t->room_id);
            ivr_str_free(&t->call_id);
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
    ivr_str_free(&t->room_id);
    ivr_str_free(&t->call_id);
    if (t->lifecycle_lock_initialized) {
        ivr_mutex_destroy(&t->lifecycle_lock);
        t->lifecycle_lock_initialized = 0;
    }
    ivr_mutex_destroy(&t->lock);
    free(t->sfu_host_owned);
    free(t->media_token_owned);
    free(t);
}
