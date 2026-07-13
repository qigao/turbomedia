#include "turbo_media_server.h"

#ifdef TURBO_MEDIA_HAS_RTSP

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TURBO_MEDIA_RTSP_ADAPTER_MAX_SESSIONS 64
#define TURBO_MEDIA_RTSP_ADAPTER_DEFAULT_SESSION_ID "00000001"
#define TURBO_MEDIA_RTSP_ADAPTER_DEFAULT_CHANNELS 2
#define TURBO_MEDIA_RTSP_ADAPTER_MAX_SDP 2048

typedef struct {
    int active;
    turbo_rtsp_session_t *rtsp_session;
    char uri[TURBO_RTSP_MAX_URI_LEN];
    char transport[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
    char sdp[TURBO_MEDIA_RTSP_ADAPTER_MAX_SDP];
    size_t channel_count;
    turbo_media_protocol_session_t *publisher;
    turbo_media_protocol_session_t *player;
} turbo_media_rtsp_adapter_session_t;

struct turbo_media_rtsp_server_adapter_s {
    turbo_media_server_runtime_t *runtime;
    struct coro_context_s *coro_context;
    turbo_rtsp_server_t *rtsp_server;
    turbo_rtsp_server_handlers_t handlers;
    turbo_media_rtsp_server_adapter_config_t config;
    char vhost[TURBO_MEDIA_MAX_VHOST_LEN];
    char session_id[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
    turbo_media_rtsp_adapter_session_t sessions[TURBO_MEDIA_RTSP_ADAPTER_MAX_SESSIONS];
};

static const char *turbo_media_rtsp_adapter_vhost(
    const turbo_media_rtsp_server_adapter_t *adapter) {
    return adapter->vhost[0] ? adapter->vhost : NULL;
}

static const char *turbo_media_rtsp_adapter_session_id(
    const turbo_media_rtsp_server_adapter_t *adapter) {
    return adapter->session_id[0]
               ? adapter->session_id
               : TURBO_MEDIA_RTSP_ADAPTER_DEFAULT_SESSION_ID;
}

static int turbo_media_rtsp_adapter_status_from_result(int rc) {
    switch (rc) {
    case TURBO_MEDIA_OK:
        return 200;
    case TURBO_MEDIA_ERR_NOT_FOUND:
        return 404;
    case TURBO_MEDIA_ERR_FULL:
        return 453;
    case TURBO_MEDIA_ERR_STATE:
        return 455;
    case TURBO_MEDIA_ERR_NOMEM:
        return 500;
    case TURBO_MEDIA_ERR_INVALID:
    default:
        return 400;
    }
}

static void turbo_media_rtsp_adapter_close_binding(
    turbo_media_rtsp_adapter_session_t *binding) {
    if (!binding) return;

    if (binding->player) {
        turbo_media_server_protocol_session_close(binding->player);
        binding->player = NULL;
    }
    if (binding->publisher) {
        turbo_media_server_protocol_session_close(binding->publisher);
        binding->publisher = NULL;
    }
    memset(binding, 0, sizeof(*binding));
}

static turbo_media_rtsp_adapter_session_t *turbo_media_rtsp_adapter_find_binding(
    turbo_media_rtsp_server_adapter_t *adapter,
    turbo_rtsp_session_t *session) {
    size_t i;

    if (!adapter || !session) return NULL;

    for (i = 0; i < TURBO_MEDIA_RTSP_ADAPTER_MAX_SESSIONS; ++i) {
        turbo_media_rtsp_adapter_session_t *binding = &adapter->sessions[i];
        if (binding->active && binding->rtsp_session == session) {
            return binding;
        }
    }

    return NULL;
}

static turbo_media_rtsp_adapter_session_t *turbo_media_rtsp_adapter_get_binding(
    turbo_media_rtsp_server_adapter_t *adapter,
    turbo_rtsp_session_t *session,
    const char *uri) {
    size_t i;
    turbo_media_rtsp_adapter_session_t *binding;

    binding = turbo_media_rtsp_adapter_find_binding(adapter, session);
    if (binding) {
        if (uri && uri[0]) {
            snprintf(binding->uri, sizeof(binding->uri), "%s", uri);
        }
        return binding;
    }

    for (i = 0; i < TURBO_MEDIA_RTSP_ADAPTER_MAX_SESSIONS; ++i) {
        binding = &adapter->sessions[i];
        if (!binding->active) {
            memset(binding, 0, sizeof(*binding));
            binding->active = 1;
            binding->rtsp_session = session;
            binding->channel_count =
                adapter->config.default_rtp_channel_count > 0
                    ? adapter->config.default_rtp_channel_count
                    : TURBO_MEDIA_RTSP_ADAPTER_DEFAULT_CHANNELS;
            if (uri && uri[0]) {
                snprintf(binding->uri, sizeof(binding->uri), "%s", uri);
            }
            return binding;
        }
    }

    return NULL;
}

static void turbo_media_rtsp_adapter_note_channels(
    turbo_media_rtsp_adapter_session_t *binding,
    const turbo_rtsp_request_t *request) {
    size_t needed = 0;

    if (!binding || !request) return;

    if (request->transport_spec.interleaved_rtp_channel >= 0) {
        needed = (size_t)request->transport_spec.interleaved_rtp_channel + 1u;
        if (needed > binding->channel_count) binding->channel_count = needed;
    }
    if (request->transport_spec.interleaved_rtcp_channel >= 0) {
        needed = (size_t)request->transport_spec.interleaved_rtcp_channel + 1u;
        if (needed > binding->channel_count) binding->channel_count = needed;
    }
}

static int turbo_media_rtsp_adapter_format_transport(
    turbo_media_rtsp_adapter_session_t *binding,
    const turbo_rtsp_request_t *request,
    turbo_rtsp_transport_mode_t mode) {
    const char *mode_name = mode == TURBO_RTSP_TRANSPORT_MODE_RECORD ? "RECORD" : "PLAY";
    int rtp_channel;
    int rtcp_channel;
    int written;

    if (!binding || !request ||
        request->transport_spec.kind != TURBO_RTSP_TRANSPORT_RTP_AVP_TCP) {
        return TURBO_MEDIA_ERR_INVALID;
    }

    rtp_channel = request->transport_spec.interleaved_rtp_channel >= 0
                      ? request->transport_spec.interleaved_rtp_channel
                      : 0;
    rtcp_channel = request->transport_spec.interleaved_rtcp_channel >= 0
                       ? request->transport_spec.interleaved_rtcp_channel
                       : rtp_channel + 1;

    written = snprintf(
        binding->transport,
        sizeof(binding->transport),
        "RTP/AVP/TCP;unicast;interleaved=%d-%d;mode=%s",
        rtp_channel,
        rtcp_channel,
        mode_name);
    if (written < 0 || (size_t)written >= sizeof(binding->transport)) {
        return TURBO_MEDIA_ERR_INVALID;
    }

    turbo_media_rtsp_adapter_note_channels(binding, request);
    return TURBO_MEDIA_OK;
}

static int turbo_media_rtsp_adapter_frame_to_rtsp(
    turbo_media_source_t *source,
    const turbo_media_frame_t *frame,
    void *user_data) {
    turbo_media_rtsp_adapter_session_t *binding =
        (turbo_media_rtsp_adapter_session_t *)user_data;
    (void)source;

    if (!binding || !binding->active || !binding->rtsp_session || !frame) {
        return TURBO_MEDIA_ERR_INVALID;
    }

    return turbo_rtsp_session_send_interleaved_frame(
               binding->rtsp_session,
               (uint8_t)frame->track_id,
               frame->data,
               frame->size) == 0
               ? TURBO_MEDIA_OK
               : TURBO_MEDIA_ERR_STATE;
}

static int turbo_media_rtsp_adapter_write_sdp(
    turbo_media_rtsp_server_adapter_t *adapter,
    turbo_media_rtsp_adapter_session_t *binding,
    const turbo_media_source_key_t *key) {
    turbo_media_source_t *source = NULL;
    size_t offset = 0;
    size_t track_count;
    size_t i;
    int rc;

    rc = turbo_media_server_runtime_find_source(adapter->runtime, key, &source);
    if (rc != TURBO_MEDIA_OK) return rc;

#define APPEND_SDP(...)                                                         \
    do {                                                                       \
        int written__ = snprintf(binding->sdp + offset,                         \
                                 sizeof(binding->sdp) - offset,                 \
                                 __VA_ARGS__);                                  \
        if (written__ < 0 || (size_t)written__ >= sizeof(binding->sdp) - offset) \
            return TURBO_MEDIA_ERR_FULL;                                        \
        offset += (size_t)written__;                                            \
    } while (0)

    APPEND_SDP("v=0\r\n");
    APPEND_SDP("o=- 0 0 IN IP4 127.0.0.1\r\n");
    APPEND_SDP("s=TurboMedia\r\n");
    APPEND_SDP("t=0 0\r\n");
    APPEND_SDP("a=control:*\r\n");

    track_count = turbo_media_source_track_count(source);
    for (i = 0; i < track_count; ++i) {
        turbo_media_track_info_t track;
        const char *media = "application";
        const char *codec = "rtp";
        int payload_type = 96;
        int clock_rate = 90000;

        if (turbo_media_source_get_track(source, (int)i, &track) != TURBO_MEDIA_OK) {
            continue;
        }
        if (track.type == TURBO_MEDIA_TRACK_AUDIO) {
            media = "audio";
        } else if (track.type == TURBO_MEDIA_TRACK_VIDEO) {
            media = "video";
        }
        if (track.codec_name[0]) codec = track.codec_name;
        if (track.payload_type >= 0) payload_type = track.payload_type;
        if (track.clock_rate > 0) clock_rate = track.clock_rate;

        APPEND_SDP("m=%s 0 RTP/AVP %d\r\n", media, payload_type);
        APPEND_SDP("a=rtpmap:%d %s/%d\r\n", payload_type, codec, clock_rate);
        APPEND_SDP("a=control:trackID=%d\r\n", track.track_id);
    }

#undef APPEND_SDP
    return TURBO_MEDIA_OK;
}

static int turbo_media_rtsp_adapter_on_options(
    turbo_rtsp_session_t *session,
    const turbo_rtsp_request_t *request,
    turbo_rtsp_response_t *response,
    void *user_data) {
    (void)session;
    (void)request;
    (void)user_data;
    return turbo_rtsp_response_options(response, NULL);
}

static int turbo_media_rtsp_adapter_on_announce(
    turbo_rtsp_session_t *session,
    const turbo_rtsp_request_t *request,
    turbo_rtsp_response_t *response,
    void *user_data) {
    turbo_media_rtsp_server_adapter_t *adapter =
        (turbo_media_rtsp_server_adapter_t *)user_data;
    turbo_media_rtsp_adapter_session_t *binding;

    binding = turbo_media_rtsp_adapter_get_binding(adapter, session, request->uri);
    if (!binding) {
        return turbo_rtsp_response_status(response, 453, "Not Enough Bandwidth");
    }

    return turbo_rtsp_response_announce(response);
}

static int turbo_media_rtsp_adapter_on_setup(
    turbo_rtsp_session_t *session,
    const turbo_rtsp_request_t *request,
    turbo_rtsp_response_t *response,
    void *user_data) {
    turbo_media_rtsp_server_adapter_t *adapter =
        (turbo_media_rtsp_server_adapter_t *)user_data;
    turbo_media_rtsp_adapter_session_t *binding;
    turbo_rtsp_transport_mode_t mode;
    int rc;

    binding = turbo_media_rtsp_adapter_get_binding(adapter, session, request->uri);
    if (!binding) {
        return turbo_rtsp_response_status(response, 453, "Not Enough Bandwidth");
    }

    mode = request->transport_spec.mode == TURBO_RTSP_TRANSPORT_MODE_RECORD
               ? TURBO_RTSP_TRANSPORT_MODE_RECORD
               : TURBO_RTSP_TRANSPORT_MODE_PLAY;
    rc = turbo_media_rtsp_adapter_format_transport(binding, request, mode);
    if (rc != TURBO_MEDIA_OK) {
        return turbo_rtsp_response_status(response, 461, "Unsupported Transport");
    }

    return turbo_rtsp_response_setup(
        response,
        turbo_media_rtsp_adapter_session_id(adapter),
        binding->transport);
}

static int turbo_media_rtsp_adapter_on_record(
    turbo_rtsp_session_t *session,
    const turbo_rtsp_request_t *request,
    turbo_rtsp_response_t *response,
    void *user_data) {
    turbo_media_rtsp_server_adapter_t *adapter =
        (turbo_media_rtsp_server_adapter_t *)user_data;
    turbo_media_rtsp_adapter_session_t *binding;
    int rc;

    binding = turbo_media_rtsp_adapter_get_binding(adapter, session, request->uri);
    if (!binding) {
        return turbo_rtsp_response_status(response, 453, "Not Enough Bandwidth");
    }
    if (!binding->uri[0]) {
        snprintf(binding->uri, sizeof(binding->uri), "%s", request->uri);
    }

    if (!binding->publisher) {
        rc = turbo_media_server_rtsp_open_record(
            adapter->runtime,
            turbo_media_rtsp_adapter_vhost(adapter),
            binding->uri,
            binding->channel_count,
            adapter->config.remove_source_on_close,
            &binding->publisher);
        if (rc != TURBO_MEDIA_OK) {
            return turbo_rtsp_response_status(
                response,
                turbo_media_rtsp_adapter_status_from_result(rc),
                NULL);
        }
    }

    return turbo_rtsp_response_record(response, request->range[0] ? request->range : NULL);
}

static int turbo_media_rtsp_adapter_on_describe(
    turbo_rtsp_session_t *session,
    const turbo_rtsp_request_t *request,
    turbo_rtsp_response_t *response,
    void *user_data) {
    turbo_media_rtsp_server_adapter_t *adapter =
        (turbo_media_rtsp_server_adapter_t *)user_data;
    turbo_media_rtsp_adapter_session_t *binding;
    turbo_media_source_key_t key;
    int rc;

    binding = turbo_media_rtsp_adapter_get_binding(adapter, session, request->uri);
    if (!binding) {
        return turbo_rtsp_response_status(response, 453, "Not Enough Bandwidth");
    }

    rc = turbo_media_server_rtsp_source_key(
        turbo_media_rtsp_adapter_vhost(adapter),
        request->uri,
        &key);
    if (rc == TURBO_MEDIA_OK) {
        rc = turbo_media_rtsp_adapter_write_sdp(adapter, binding, &key);
    }
    if (rc != TURBO_MEDIA_OK) {
        return turbo_rtsp_response_status(
            response,
            turbo_media_rtsp_adapter_status_from_result(rc),
            NULL);
    }

    return turbo_rtsp_response_describe(response, binding->sdp, strlen(binding->sdp));
}

static int turbo_media_rtsp_adapter_on_play(
    turbo_rtsp_session_t *session,
    const turbo_rtsp_request_t *request,
    turbo_rtsp_response_t *response,
    void *user_data) {
    turbo_media_rtsp_server_adapter_t *adapter =
        (turbo_media_rtsp_server_adapter_t *)user_data;
    turbo_media_rtsp_adapter_session_t *binding;
    int rc;

    binding = turbo_media_rtsp_adapter_get_binding(adapter, session, request->uri);
    if (!binding) {
        return turbo_rtsp_response_status(response, 453, "Not Enough Bandwidth");
    }
    if (!binding->uri[0]) {
        snprintf(binding->uri, sizeof(binding->uri), "%s", request->uri);
    }

    if (!binding->player) {
        rc = turbo_media_server_rtsp_open_play(
            adapter->runtime,
            turbo_media_rtsp_adapter_vhost(adapter),
            binding->uri,
            turbo_media_rtsp_adapter_frame_to_rtsp,
            binding,
            adapter->config.replay_cached,
            &binding->player);
        if (rc != TURBO_MEDIA_OK) {
            return turbo_rtsp_response_status(
                response,
                turbo_media_rtsp_adapter_status_from_result(rc),
                NULL);
        }
    }

    return turbo_rtsp_response_play(response, request->range[0] ? request->range : NULL, NULL);
}

static int turbo_media_rtsp_adapter_on_teardown(
    turbo_rtsp_session_t *session,
    const turbo_rtsp_request_t *request,
    turbo_rtsp_response_t *response,
    void *user_data) {
    turbo_media_rtsp_server_adapter_t *adapter =
        (turbo_media_rtsp_server_adapter_t *)user_data;
    turbo_media_rtsp_adapter_session_t *binding =
        turbo_media_rtsp_adapter_find_binding(adapter, session);
    (void)request;

    turbo_media_rtsp_adapter_close_binding(binding);
    return turbo_rtsp_response_teardown(response);
}

static int turbo_media_rtsp_adapter_on_interleaved(
    turbo_rtsp_session_t *session,
    uint8_t channel,
    const uint8_t *payload,
    size_t payload_len,
    void *user_data) {
    turbo_media_rtsp_server_adapter_t *adapter =
        (turbo_media_rtsp_server_adapter_t *)user_data;
    turbo_media_rtsp_adapter_session_t *binding =
        turbo_media_rtsp_adapter_find_binding(adapter, session);

    if (!binding || !binding->publisher) return -1;
    return turbo_media_server_rtsp_publish_interleaved(
        binding->publisher,
        channel,
        payload,
        payload_len,
        0);
}

static void turbo_media_rtsp_adapter_on_session_close(
    turbo_rtsp_session_t *session,
    void *user_data) {
    turbo_media_rtsp_server_adapter_t *adapter =
        (turbo_media_rtsp_server_adapter_t *)user_data;
    turbo_media_rtsp_adapter_session_t *binding =
        turbo_media_rtsp_adapter_find_binding(adapter, session);

    turbo_media_rtsp_adapter_close_binding(binding);
}

turbo_media_rtsp_server_adapter_t *turbo_media_server_rtsp_adapter_create(
    turbo_media_server_runtime_t *runtime,
    struct coro_context_s *coro_context,
    const turbo_rtsp_server_config_t *rtsp_config,
    const turbo_media_rtsp_server_adapter_config_t *adapter_config) {
    turbo_media_rtsp_server_adapter_t *adapter;

    if (!runtime || !coro_context) return NULL;

    adapter = (turbo_media_rtsp_server_adapter_t *)calloc(1, sizeof(*adapter));
    if (!adapter) return NULL;

    adapter->runtime = runtime;
    adapter->coro_context = coro_context;
    if (adapter_config) adapter->config = *adapter_config;
    if (adapter->config.vhost) {
        snprintf(adapter->vhost, sizeof(adapter->vhost), "%s", adapter->config.vhost);
    }
    if (adapter->config.session_id) {
        snprintf(adapter->session_id, sizeof(adapter->session_id), "%s", adapter->config.session_id);
    }

    adapter->handlers.on_options = turbo_media_rtsp_adapter_on_options;
    adapter->handlers.on_describe = turbo_media_rtsp_adapter_on_describe;
    adapter->handlers.on_setup = turbo_media_rtsp_adapter_on_setup;
    adapter->handlers.on_play = turbo_media_rtsp_adapter_on_play;
    adapter->handlers.on_teardown = turbo_media_rtsp_adapter_on_teardown;
    adapter->handlers.on_announce = turbo_media_rtsp_adapter_on_announce;
    adapter->handlers.on_record = turbo_media_rtsp_adapter_on_record;
    adapter->handlers.on_interleaved_frame = turbo_media_rtsp_adapter_on_interleaved;
    adapter->handlers.on_session_close = turbo_media_rtsp_adapter_on_session_close;

    adapter->rtsp_server = turbo_rtsp_server_create(
        (coro_context_t *)coro_context,
        rtsp_config,
        &adapter->handlers,
        adapter);
    if (!adapter->rtsp_server) {
        free(adapter);
        return NULL;
    }

    return adapter;
}

void turbo_media_server_rtsp_adapter_destroy(
    turbo_media_rtsp_server_adapter_t *adapter) {
    size_t i;

    if (!adapter) return;

    turbo_media_server_rtsp_adapter_stop(adapter);
    for (i = 0; i < TURBO_MEDIA_RTSP_ADAPTER_MAX_SESSIONS; ++i) {
        turbo_media_rtsp_adapter_close_binding(&adapter->sessions[i]);
    }
    turbo_rtsp_server_destroy(adapter->rtsp_server);
    free(adapter);
}

int turbo_media_server_rtsp_adapter_start(
    turbo_media_rtsp_server_adapter_t *adapter) {
    if (!adapter || !adapter->rtsp_server) return TURBO_MEDIA_ERR_INVALID;
    return turbo_rtsp_server_start(adapter->rtsp_server) == 0
               ? TURBO_MEDIA_OK
               : TURBO_MEDIA_ERR_STATE;
}

int turbo_media_server_rtsp_adapter_stop(
    turbo_media_rtsp_server_adapter_t *adapter) {
    if (!adapter || !adapter->rtsp_server) return TURBO_MEDIA_ERR_INVALID;
    turbo_rtsp_server_stop(adapter->rtsp_server);
    return TURBO_MEDIA_OK;
}

turbo_rtsp_server_t *turbo_media_server_rtsp_adapter_rtsp_server(
    turbo_media_rtsp_server_adapter_t *adapter) {
    return adapter ? adapter->rtsp_server : NULL;
}

#endif /* TURBO_MEDIA_HAS_RTSP */
