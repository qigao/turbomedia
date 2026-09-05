#include "ivr_whep_transport.h"

#include "ivr_http_media_client.h"
#include "ivr_internal.h"
#include "ivr_thread.h"
#include "turbo_peer_connection.h"
#include "turbo_rtp.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IVR_WHEP_DEFAULT_SAMPLE_RATE 16000u
#define IVR_WHEP_PCM_BITS_PER_SAMPLE 16u
#define IVR_WHEP_MAX_AUDIO_FRAME_BYTES (48000u * 2u * 2u * 60u / 1000u)
#define IVR_WHEP_DEFAULT_TELEPHONE_EVENT_PT 126u

struct ivr_whep_transport_s {
    ivr_whep_transport_config_t config;
    ivr_http_media_client_t *http_client;
    ivr_mutex_t lock;
    ivr_mutex_t lifecycle_lock;
    int lock_initialized;
    int lifecycle_lock_initialized;
    int destroying;
    int active;
    int connected;
    ivr_str_t tenant_id;
    ivr_str_t provider_session_id;
    ivr_str_t dialog_id;
    ivr_str_t room_id;
    ivr_str_t call_id;
    ivr_str_t participant_id;
    uint64_t call_generation;
    uint64_t expected_room_version;
    uint64_t attempt_generation;
    ivr_media_state_fn on_state;
    void *state_context;
    turbo_peer_connection_t *pc;
    turbo_media_track_t *audio_track;
    char session_location[512];
    char session_etag[128];
    ivr_thread_t poll_thread;
    int poll_stop;
    int poll_started;
    uint64_t frames_received;
    uint64_t frames_rejected;
    uint64_t last_frame_ms;
    int input_stalled;
    uint8_t telephone_event_payload_type;
};

typedef struct {
    ivr_call_ref_t view;
    char tenant_id[IVR_MEDIA_ID_CAPACITY];
    char provider_session_id[IVR_MEDIA_ID_CAPACITY];
    char dialog_id[IVR_MEDIA_ID_CAPACITY];
    char room_id[IVR_MEDIA_ID_CAPACITY];
    char call_id[IVR_MEDIA_ID_CAPACITY];
} whep_call_copy_t;

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
static void whep_clear_call(ivr_whep_transport_t *transport) {
    ivr_str_free(&transport->tenant_id);
    ivr_str_free(&transport->provider_session_id);
    ivr_str_free(&transport->dialog_id);
    ivr_str_free(&transport->room_id);
    ivr_str_free(&transport->call_id);
    transport->call_generation = 0u;
    transport->expected_room_version = 0u;
}

static int whep_copy_call_locked(const ivr_whep_transport_t *transport,
                                 whep_call_copy_t *copy) {
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

static void whep_emit_state(ivr_whep_transport_t *transport,
                            ivr_media_link_state_t state, int error_code) {
    ivr_media_state_fn callback;
    void *context;
    whep_call_copy_t call;
    uint64_t generation;
    int have_call;
    if (!transport) {
        return;
    }
    ivr_mutex_lock(&transport->lock);
    callback = transport->on_state;
    context = transport->state_context;
    generation = transport->attempt_generation;
    have_call = whep_copy_call_locked(transport, &call);
    ivr_mutex_unlock(&transport->lock);
    if (callback && have_call) {
        callback(context, &call.view, generation, state, error_code);
    }
}

static int whep_call_matches_locked(const ivr_whep_transport_t *transport,
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

static void whep_delete_session(ivr_whep_transport_t *transport,
                                const char *location) {
    ivr_http_media_response_t response;

    if (!transport || !location || location[0] == '\0') {
        return;
    }
    (void)ivr_http_media_request(transport->http_client, "DELETE", location,
                                 NULL, NULL, NULL, &response);
}

static void whep_on_frame(turbo_media_track_t *track, const uint8_t *data,
                          size_t length, uint64_t timestamp,
                          void *user_data) {
    ivr_whep_transport_t *transport =
        (ivr_whep_transport_t *)user_data;
    ivr_whep_audio_cb callback = NULL;
    void *callback_context = NULL;
    whep_call_copy_t call;
    uint32_t sample_rate = 0;
    int accepted = 0;
    int have_call = 0;

    (void)track;
    if (!transport || !data || length == 0 ||
        length > IVR_WHEP_MAX_AUDIO_FRAME_BYTES ||
        (length % (IVR_WHEP_PCM_BITS_PER_SAMPLE / 8u)) != 0) {
        return;
    }
    ivr_mutex_lock(&transport->lock);
    if (transport->active && !transport->poll_stop) {
        transport->last_frame_ms = salts_monotonic_ms();
        transport->input_stalled = 0;
        have_call = whep_copy_call_locked(transport, &call);
        callback = transport->config.on_audio;
        callback_context = transport->config.audio_context;
        sample_rate = transport->config.sample_rate;
    }
    ivr_mutex_unlock(&transport->lock);
    if (callback && have_call) {
        accepted = callback(callback_context, &call.view, data, length,
                            sample_rate, timestamp) == 0;
    }
    ivr_mutex_lock(&transport->lock);
    if (accepted) {
        transport->frames_received++;
    } else {
        transport->frames_rejected++;
    }
    ivr_mutex_unlock(&transport->lock);
}

static void whep_on_rtp(turbo_media_track_t *track, const uint8_t *packet,
                        size_t length, void *user_data) {
    ivr_whep_transport_t *transport = (ivr_whep_transport_t *)user_data;
    ivr_whep_rtp_cb callback = NULL;
    void *callback_context = NULL;
    whep_call_copy_t call;
    rtp_packet_t parsed;
    uint8_t telephone_event_pt;
    uint64_t source_generation = 0;
    int have_call = 0;

    (void)track;
    if (!transport || !packet || length < 12u ||
        rtp_packet_parse(&parsed, packet, length) != 0) {
        return;
    }
    ivr_mutex_lock(&transport->lock);
    telephone_event_pt = transport->telephone_event_payload_type;
    if (transport->active && !transport->poll_stop &&
        parsed.header.payload_type == telephone_event_pt) {
        have_call = whep_copy_call_locked(transport, &call);
        callback = transport->config.on_rtp;
        callback_context = transport->config.rtp_context;
        source_generation = transport->attempt_generation;
    }
    ivr_mutex_unlock(&transport->lock);
    if (callback && have_call) {
        (void)callback(callback_context, &call.view, parsed.header.payload_type,
                       parsed.header.timestamp, parsed.payload,
                       parsed.payload_len, source_generation);
    }
}

static void whep_on_track(turbo_peer_connection_t *pc,
                          turbo_media_track_t *track, void *user_data) {
    ivr_whep_transport_t *transport =
        (ivr_whep_transport_t *)user_data;
    int connected;

    (void)pc;
    if (!transport || !track ||
        turbo_media_track_get_type(track) != TURBO_RTC_MEDIA_TRACK_AUDIO) {
        return;
    }
    turbo_media_track_set_user_data(track, transport);
    turbo_media_track_on_frame(track, whep_on_frame);
    turbo_media_track_on_rtp_packet(track, whep_on_rtp);
    ivr_mutex_lock(&transport->lock);
    transport->audio_track = track;
    connected = transport->connected;
    ivr_mutex_unlock(&transport->lock);
    if (connected) {
        (void)turbo_media_track_start(track);
    }
}

static void whep_on_state_change(turbo_peer_connection_t *pc,
                                 turbo_peer_state_t state, void *user_data) {
    ivr_whep_transport_t *transport =
        (ivr_whep_transport_t *)user_data;
    turbo_media_track_t *track;

    (void)pc;
    if (!transport) {
        return;
    }
    ivr_mutex_lock(&transport->lock);
    if (state == TURBO_PEER_STATE_CONNECTED) {
        transport->connected = 1;
        transport->last_frame_ms = salts_monotonic_ms();
        transport->input_stalled = 0;
    } else if (state == TURBO_PEER_STATE_DISCONNECTED ||
               state == TURBO_PEER_STATE_FAILED ||
               state == TURBO_PEER_STATE_CLOSED) {
        transport->connected = 0;
    }
    track = transport->audio_track;
    ivr_mutex_unlock(&transport->lock);
    if (state == TURBO_PEER_STATE_CONNECTED && track) {
        (void)turbo_media_track_start(track);
    }
    if (state == TURBO_PEER_STATE_CONNECTED) {
        whep_emit_state(transport, IVR_MEDIA_LINK_CONNECTED,
                        IVR_MEDIA_ERROR_NONE);
    } else if (state == TURBO_PEER_STATE_DISCONNECTED) {
        whep_emit_state(transport, IVR_MEDIA_LINK_DISCONNECTED,
                        IVR_MEDIA_ERROR_NONE);
    } else if (state == TURBO_PEER_STATE_FAILED) {
        whep_emit_state(transport, IVR_MEDIA_LINK_FAILED,
                        IVR_MEDIA_ERROR_PEER_FAILED);
    } else if (state == TURBO_PEER_STATE_CLOSED) {
        whep_emit_state(transport, IVR_MEDIA_LINK_CLOSED,
                        IVR_MEDIA_ERROR_STOPPED);
    }
}

static void *whep_poll_thread_main(void *opaque) {
    ivr_whep_transport_t *transport = (ivr_whep_transport_t *)opaque;
    uint64_t timeout = transport->config.connect_timeout_ms
                           ? transport->config.connect_timeout_ms
                           : 10000u;
    uint64_t inactivity = transport->config.input_inactivity_timeout_ms
                              ? transport->config.input_inactivity_timeout_ms
                              : 5000u;
    uint64_t waited = 0;
    int connected_once = 0;

    for (;;) {
        turbo_peer_connection_t *pc;
        int stop;
        int connected;
        int stalled = 0;
        uint64_t now;

        ivr_mutex_lock(&transport->lock);
        stop = transport->poll_stop;
        connected = transport->connected;
        pc = transport->pc;
        now = salts_monotonic_ms();
        if (!stop && transport->active && connected &&
            !transport->input_stalled && transport->last_frame_ms != 0 &&
            now >= transport->last_frame_ms &&
            now - transport->last_frame_ms >= inactivity) {
            transport->input_stalled = 1;
            stalled = 1;
        }
        ivr_mutex_unlock(&transport->lock);
        if (stop || !pc) {
            break;
        }
        if (stalled) {
            whep_emit_state(transport, IVR_MEDIA_LINK_DISCONNECTED,
                            IVR_MEDIA_ERROR_INPUT_STALLED);
        }
        if (connected) {
            connected_once = 1;
        }
        turbo_peer_connection_poll(pc);
        ivr_thread_sleep_ms(10);
        if (!connected && !connected_once) {
            waited += 10;
            if (waited >= timeout) {
                break;
            }
        }
    }
    ivr_mutex_lock(&transport->lock);
    int timed_out = !transport->poll_stop && transport->active &&
                    !transport->connected;
    ivr_mutex_unlock(&transport->lock);
    if (timed_out) {
        whep_emit_state(transport, IVR_MEDIA_LINK_FAILED,
                        IVR_MEDIA_ERROR_CONNECT_TIMEOUT);
    }
    return NULL;
}

ivr_status_t ivr_whep_transport_create(
    const ivr_whep_transport_config_t *config,
    ivr_whep_transport_t **out_transport) {
    ivr_http_media_client_config_t http_config =
        IVR_HTTP_MEDIA_CLIENT_CONFIG_INIT;
    ivr_whep_transport_t *transport;

    if (!config || !config->sfu_base_url ||
        !config->on_audio || !out_transport) {
        return IVR_EINVAL;
    }
    transport = (ivr_whep_transport_t *)calloc(1, sizeof(*transport));
    if (!transport) {
        return IVR_ENOSPC;
    }
    transport->config = *config;
    transport->config.sample_rate = config->sample_rate
                                        ? config->sample_rate
                                        : IVR_WHEP_DEFAULT_SAMPLE_RATE;
    transport->telephone_event_payload_type =
        config->telephone_event_payload_type
            ? config->telephone_event_payload_type
            : IVR_WHEP_DEFAULT_TELEPHONE_EVENT_PT;
    transport->config.sfu_base_url = NULL;
    transport->config.media_token = NULL;
    transport->config.ca_file = NULL;
    transport->config.cert_file = NULL;
    transport->config.key_file = NULL;
    transport->config.key_password = NULL;
    http_config.base_url = config->sfu_base_url;
    http_config.media_token = config->media_token;
    http_config.ca_file = config->ca_file;
    http_config.cert_file = config->cert_file;
    http_config.key_file = config->key_file;
    http_config.key_password = config->key_password;
    http_config.timeout_ms = config->http_timeout_ms;
    http_config.allow_plaintext_loopback = config->allow_plaintext_loopback;
    if (ivr_http_media_client_create(&http_config,
                                     &transport->http_client) != 0) {
        free(transport);
        return IVR_EINVAL;
    }
    transport->on_state = config->on_state;
    transport->state_context = config->state_context;
    ivr_str_init(&transport->tenant_id);
    ivr_str_init(&transport->provider_session_id);
    ivr_str_init(&transport->dialog_id);
    ivr_str_init(&transport->room_id);
    ivr_str_init(&transport->call_id);
    ivr_str_init(&transport->participant_id);
    if (ivr_mutex_init(&transport->lock) != 0) {
        ivr_whep_transport_destroy(transport);
        return IVR_ENOSPC;
    }
    transport->lock_initialized = 1;
    if (ivr_mutex_init(&transport->lifecycle_lock) != 0) {
        ivr_whep_transport_destroy(transport);
        return IVR_ENOSPC;
    }
    transport->lifecycle_lock_initialized = 1;
    *out_transport = transport;
    return IVR_OK;
}

static int whep_stop_impl(ivr_whep_transport_t *transport) {
    turbo_peer_connection_t *pc;
    char location[512];

    ivr_mutex_lock(&transport->lock);
    if (!transport->active) {
        ivr_mutex_unlock(&transport->lock);
        return 0;
    }
    memcpy(location, transport->session_location,
           sizeof(transport->session_location));
    location[sizeof(location) - 1] = '\0';
    transport->active = 0;
    transport->connected = 0;
    transport->poll_stop = 1;
    transport->last_frame_ms = 0;
    transport->input_stalled = 0;
    pc = transport->pc;
    transport->pc = NULL;
    transport->audio_track = NULL;
    ivr_mutex_unlock(&transport->lock);
    if (transport->poll_started) {
        ivr_thread_join(&transport->poll_thread);
        transport->poll_started = 0;
    }
    whep_delete_session(transport, location);
    if (pc) {
        turbo_peer_connection_destroy(pc);
    }
    ivr_mutex_lock(&transport->lock);
    whep_clear_call(transport);
    ivr_str_free(&transport->participant_id);
    transport->session_location[0] = '\0';
    transport->session_etag[0] = '\0';
    ivr_mutex_unlock(&transport->lock);
    return 0;
}

int ivr_whep_transport_stop(ivr_whep_transport_t *transport,
                            const ivr_call_ref_t *call) {
    int matches;
    int result;

    if (!transport || !call) {
        return -1;
    }
    ivr_mutex_lock(&transport->lifecycle_lock);
    if (transport->destroying) {
        ivr_mutex_unlock(&transport->lifecycle_lock);
        return -1;
    }
    ivr_mutex_lock(&transport->lock);
    matches = whep_call_matches_locked(transport, call);
    ivr_mutex_unlock(&transport->lock);
    result = matches ? whep_stop_impl(transport) : -1;
    ivr_mutex_unlock(&transport->lifecycle_lock);
    return result;
}

static ivr_status_t whep_start_fail(ivr_whep_transport_t *transport,
                                    turbo_peer_connection_t *pc,
                                    const char *location,
                                    ivr_status_t status) {
    if (location && location[0]) {
        whep_delete_session(transport, location);
    }
    if (pc) {
        turbo_peer_connection_destroy(pc);
    }
    ivr_mutex_lock(&transport->lock);
    transport->active = 0;
    transport->connected = 0;
    whep_clear_call(transport);
    ivr_str_free(&transport->participant_id);
    ivr_mutex_unlock(&transport->lock);
    ivr_mutex_unlock(&transport->lifecycle_lock);
    return status;
}

ivr_status_t ivr_whep_transport_start(ivr_whep_transport_t *transport,
                                      const ivr_call_ref_t *call,
                                      const char *participant_id) {
    turbo_peer_config_t peer_config;
    turbo_peer_callbacks_t callbacks;
    turbo_media_track_config_t track_config;
    turbo_peer_connection_t *pc;
    turbo_media_track_t *track;
    ivr_http_media_response_t response;
    char offer[IVR_HTTP_MEDIA_MAX_SDP];
    char minimal_offer[IVR_HTTP_MEDIA_MAX_SDP];
    char path[IVR_MEDIA_ID_CAPACITY * 6u + 16u];
    char fragment[2048];
    char ufrag[64];
    char password[96];
    int path_length;

    if (!transport || !call || !participant_id || participant_id[0] == '\0' ||
        strlen(participant_id) >= IVR_MEDIA_ID_CAPACITY ||
        !call_view_valid(&call->tenant_id) ||
        !call_view_valid(&call->provider_session_id) ||
        !call_view_valid(&call->dialog_id) ||
        !call_view_valid(&call->room_id) ||
        !call_view_valid(&call->call_id) || call->call_generation == 0u) {
        return IVR_EINVAL;
    }
    ivr_mutex_lock(&transport->lifecycle_lock);
    if (transport->destroying) {
        ivr_mutex_unlock(&transport->lifecycle_lock);
        return IVR_ESTATE;
    }
    ivr_mutex_lock(&transport->lock);
    if (transport->active ||
        ivr_str_assign(&transport->tenant_id, call->tenant_id.data,
                       call->tenant_id.size) < 0 ||
        ivr_str_assign(&transport->provider_session_id,
                       call->provider_session_id.data,
                       call->provider_session_id.size) < 0 ||
        ivr_str_assign(&transport->dialog_id, call->dialog_id.data,
                       call->dialog_id.size) < 0 ||
        ivr_str_assign(&transport->room_id, call->room_id.data,
                       call->room_id.size) < 0 ||
        ivr_str_assign(&transport->call_id, call->call_id.data,
                       call->call_id.size) < 0 ||
        ivr_str_assign(&transport->participant_id, participant_id,
                       strlen(participant_id)) < 0) {
        if (!transport->active) {
            whep_clear_call(transport);
            ivr_str_free(&transport->participant_id);
        }
        ivr_mutex_unlock(&transport->lock);
        ivr_mutex_unlock(&transport->lifecycle_lock);
        return transport->active ? IVR_ESTATE : IVR_ENOSPC;
    }
    transport->call_generation = call->call_generation;
    transport->expected_room_version = call->expected_room_version;
    transport->attempt_generation++;
    transport->active = 1;
    transport->poll_stop = 0;
    transport->connected = 0;
    transport->last_frame_ms = 0;
    transport->input_stalled = 0;
    ivr_mutex_unlock(&transport->lock);
    whep_emit_state(transport, IVR_MEDIA_LINK_CONNECTING,
                    IVR_MEDIA_ERROR_NONE);

    memset(&peer_config, 0, sizeof(peer_config));
    peer_config.allow_loopback = transport->config.allow_loopback;
    peer_config.disable_datachannel = 1;
    peer_config.user_data = transport;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.on_state_change = whep_on_state_change;
    callbacks.on_track = whep_on_track;
    pc = turbo_peer_connection_create(&peer_config, &callbacks);
    if (!pc) {
        return whep_start_fail(transport, NULL, NULL, IVR_ENOSPC);
    }
    memset(&track_config, 0, sizeof(track_config));
    track_config.type = TURBO_RTC_MEDIA_TRACK_AUDIO;
    track_config.direction = TURBO_MEDIA_DIRECTION_RECVONLY;
    track_config.codec = TURBO_CODEC_OPUS;
    track_config.audio.sample_rate = (int)transport->config.sample_rate;
    track_config.audio.channels = 1;
    track_config.audio.frame_size_ms = 20;
    track = turbo_peer_connection_add_track_ex(pc, &track_config);
    if (!track) {
        return whep_start_fail(transport, pc, NULL, IVR_ENOSPC);
    }
    turbo_media_track_set_user_data(track, transport);
    turbo_media_track_on_frame(track, whep_on_frame);
    turbo_media_track_on_rtp_packet(track, whep_on_rtp);
    ivr_mutex_lock(&transport->lock);
    transport->audio_track = track;
    ivr_mutex_unlock(&transport->lock);
    if (turbo_peer_connection_create_offer(pc, offer, sizeof(offer)) <= 0) {
        return whep_start_fail(transport, pc, NULL, IVR_ESTATE);
    }
    ivr_sdp_build_minimal_audio_offer(
        offer, minimal_offer, sizeof(minimal_offer),
        (int)transport->config.sample_rate, "recvonly");
    char *room_segment =
        ivr_http_media_encode_path_segment(transport->room_id.data);
    char *participant_segment =
        ivr_http_media_encode_path_segment(transport->participant_id.data);
    if (!room_segment || !participant_segment) {
        free(room_segment);
        free(participant_segment);
        return whep_start_fail(transport, pc, NULL, IVR_EINVAL);
    }
    path_length = snprintf(path, sizeof(path), "/whep/%s/%s", room_segment,
                           participant_segment);
    free(room_segment);
    free(participant_segment);
    if (path_length < 0 || (size_t)path_length >= sizeof(path)) {
        fprintf(stderr, "[ivr_whep] start failed: session path too long\n");
        return whep_start_fail(transport, pc, NULL, IVR_EINVAL);
    }
    if (ivr_http_media_request(transport->http_client, "POST", path,
                               "application/sdp", NULL, minimal_offer,
                               &response) != 0) {
        fprintf(stderr, "[ivr_whep] start failed: POST request failed\n");
        return whep_start_fail(transport, pc, NULL, IVR_ESTATE);
    }
    if (response.status != 201 || response.location[0] == '\0' ||
        response.etag[0] == '\0' || response.body[0] == '\0') {
        fprintf(stderr,
                "[ivr_whep] start failed: POST status=%d location=%s etag=%s\n",
                response.status, response.location[0] ? "yes" : "no",
                response.etag[0] ? "yes" : "no");
        return whep_start_fail(transport, pc, NULL, IVR_ESTATE);
    }
    if (turbo_peer_connection_set_remote_description(pc, "answer",
                                                     response.body) != 0) {
        fprintf(stderr,
                "[ivr_whep] start failed: invalid SDP answer\n");
        return whep_start_fail(transport, pc, response.location, IVR_ESTATE);
    }
    if (ivr_sdp_attr_value(offer, "a=ice-ufrag:", ufrag, sizeof(ufrag)) &&
        ivr_sdp_attr_value(offer, "a=ice-pwd:", password,
                           sizeof(password))) {
        ivr_http_media_response_t trickle;
        memset(&trickle, 0, sizeof(trickle));
        int prefix_length = snprintf(
            fragment, sizeof(fragment),
            "a=ice-ufrag:%s\r\na=ice-pwd:%s\r\n", ufrag, password);
        if (prefix_length < 0 || (size_t)prefix_length >= sizeof(fragment)) {
            fprintf(stderr,
                    "[ivr_whep] start failed: ICE fragment too large\n");
            return whep_start_fail(transport, pc, response.location,
                                   IVR_ESTATE);
        }
        ivr_sdp_append_candidates(offer, fragment + prefix_length,
                                  sizeof(fragment) - (size_t)prefix_length);
        if (ivr_http_media_request(
                transport->http_client, "PATCH", response.location,
                "application/trickle-ice-sdpfrag", response.etag, fragment,
                &trickle) != 0 ||
            (trickle.status != 200 && trickle.status != 204) ||
            (trickle.body[0] &&
             turbo_peer_connection_apply_remote_ice_sdpfrag(
                 pc, trickle.body, strlen(trickle.body)) < 0)) {
            fprintf(stderr,
                    "[ivr_whep] start failed: ICE PATCH status=%d\n",
                    trickle.status);
            return whep_start_fail(transport, pc, response.location,
                                   IVR_ESTATE);
        }
    }
    ivr_mutex_lock(&transport->lock);
    transport->pc = pc;
    memcpy(transport->session_location, response.location,
           strlen(response.location) + 1);
    memcpy(transport->session_etag, response.etag,
           strlen(response.etag) + 1);
    ivr_mutex_unlock(&transport->lock);
    if (ivr_thread_create(&transport->poll_thread, whep_poll_thread_main,
                          transport) != 0) {
        (void)whep_stop_impl(transport);
        ivr_mutex_unlock(&transport->lifecycle_lock);
        return IVR_ENOSPC;
    }
    transport->poll_started = 1;
    ivr_mutex_unlock(&transport->lifecycle_lock);
    return IVR_OK;
}

int ivr_whep_transport_connected(const ivr_whep_transport_t *transport) {
    int connected;

    if (!transport) {
        return 0;
    }
    ivr_mutex_lock((ivr_mutex_t *)&transport->lock);
    connected = transport->connected;
    ivr_mutex_unlock((ivr_mutex_t *)&transport->lock);
    return connected;
}

uint64_t ivr_whep_transport_frames_received(
    const ivr_whep_transport_t *transport) {
    uint64_t frames;

    if (!transport) {
        return 0;
    }
    ivr_mutex_lock((ivr_mutex_t *)&transport->lock);
    frames = transport->frames_received;
    ivr_mutex_unlock((ivr_mutex_t *)&transport->lock);
    return frames;
}

uint64_t ivr_whep_transport_frames_rejected(
    const ivr_whep_transport_t *transport) {
    uint64_t frames;

    if (!transport) {
        return 0;
    }
    ivr_mutex_lock((ivr_mutex_t *)&transport->lock);
    frames = transport->frames_rejected;
    ivr_mutex_unlock((ivr_mutex_t *)&transport->lock);
    return frames;
}

void ivr_whep_transport_destroy(ivr_whep_transport_t *transport) {
    if (!transport) {
        return;
    }
    if (transport->lifecycle_lock_initialized) {
        ivr_mutex_lock(&transport->lifecycle_lock);
        transport->destroying = 1;
        (void)whep_stop_impl(transport);
        ivr_mutex_unlock(&transport->lifecycle_lock);
    }
    whep_clear_call(transport);
    ivr_str_free(&transport->participant_id);
    if (transport->lifecycle_lock_initialized) {
        ivr_mutex_destroy(&transport->lifecycle_lock);
    }
    if (transport->lock_initialized) {
        ivr_mutex_destroy(&transport->lock);
    }
    ivr_http_media_client_destroy(transport->http_client);
    free(transport);
}
