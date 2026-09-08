#include "turbo_rtsp.h"

#ifdef TURBO_MEDIA_HAS_RTSP

#include "turbo_rtsp_parser.h"
#include "turbo_rtsp_rtp.h"
#include "turbo_rtsp_sdp.h"
#include "base64_utils.h"
#include "disruptor.h"
#include "platform.h"

#include <chttp/chttp.h>
#include <salts/clock.h>
#include <salts/error_codes.h>
#include <salts/thread.h>
#include <openssl/evp.h>

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#endif

#define TURBO_RTSP_DEFAULT_HOST "0.0.0.0"
#define TURBO_RTSP_DEFAULT_PORT 554
#define TURBO_RTSP_DEFAULT_TIMEOUT_MS 30000
#define TURBO_RTSP_DEFAULT_SERVER_NAME "TurboMedia RTSP"
#define TURBO_RTSP_DEFAULT_USER_AGENT "TurboMedia RTSP"
#define TURBO_RTSP_DEFAULT_PUBLIC "OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, ANNOUNCE, RECORD, GET_PARAMETER, SET_PARAMETER, REDIRECT"
#define TURBO_RTSP_RESPONSE_BUFFER_SIZE 65536
#define TURBO_RTSP_REQUEST_BUFFER_SIZE 65536
#define TURBO_RTSP_CLIENT_RECV_BUFFER_SIZE (TURBO_RTSP_INTERLEAVED_HEADER_SIZE + UINT16_MAX)
#define TURBO_RTSP_SERVER_MAX_PENDING_BYTES (TURBO_RTSP_INTERLEAVED_HEADER_SIZE + UINT16_MAX)
#define TURBO_RTSP_CLIENT_FRAME_QUEUE_CAPACITY 64
#define TURBO_RTSP_AUTH_VALUE_BUFFER_SIZE 1024
#define TURBO_RTSP_MD5_HEX_LEN 32
#define TURBO_RTSP_MD5_DIGEST_LEN 16
#define TURBO_RTSP_CLIENT_H264_RTP_MAX_PAYLOAD 1200u
#define TURBO_RTSP_CLIENT_H264_RTP_PACKET_SIZE \
    (TURBO_RTSP_RTP_HEADER_SIZE + TURBO_RTSP_CLIENT_H264_RTP_MAX_PAYLOAD)
#define TURBO_RTSP_CLIENT_H264_RTP_INITIAL_SSRC 0x54525000u
#define TURBO_RTSP_CLIENT_H264_RTP_INITIAL_SEQ 1u
#define TURBO_RTSP_CLIENT_H264_RTP_INITIAL_TIMESTAMP 0u
#define TURBO_RTSP_CLIENT_RTCP_COMPOUND_BUFFER_SIZE 1500u
#define TURBO_RTSP_DIGEST_CNONCE_BYTES 16u
#define TURBO_RTSP_DEFAULT_CONNECTION_CAPACITY 128u
#define TURBO_RTSP_DEFAULT_SEND_QUEUE_CAPACITY 256u
#define TURBO_RTSP_DEFAULT_SEND_QUEUE_BYTES (8u * 1024u * 1024u)
#define TURBO_RTSP_SERVER_POLL_SLICE_MS 10u
#define TURBO_RTSP_SERVER_STOP_TIMEOUT_MS 10000u
#define TURBO_RTSP_SERVER_MIN_QUEUE_CAPACITY 8u
#define TURBO_RTSP_SERVER_WS_WIRE_OVERHEAD_BYTES 64u
#define TURBO_RTSP_SERVER_WS_HEADER_COUNT 16u
#define TURBO_RTSP_SERVER_WS_HEADER_BYTES (16u * 1024u)
#define TURBO_RTSP_SERVER_WS_TARGET_BYTES 1024u
#define TURBO_RTSP_SERVER_WS_BODY_BYTES 256u

typedef enum {
    TURBO_RTSP_IO_NONE = 0,
    TURBO_RTSP_IO_STREAM,
    TURBO_RTSP_IO_PACKET,
    TURBO_RTSP_IO_WEBSOCKET
} turbo_rtsp_io_kind_t;

typedef struct {
    char realm[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
    char nonce[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
    char opaque[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
    char algorithm[32];
    int has_qop;
    int qop_auth;
} turbo_rtsp_digest_challenge_t;

typedef struct {
    uint8_t channel;
    uint8_t *payload;
    size_t payload_len;
} turbo_rtsp_client_frame_event_t;

typedef struct {
    turbo_rtsp_rtp_stream_t stream;
    uint8_t initialized;
    uint8_t rtp_channel;
    uint8_t rtcp_channel;
    uint8_t payload_type;
    uint32_t clock_rate;
    uint32_t ssrc;
    uint16_t initial_sequence_number;
    uint32_t initial_timestamp;
} turbo_rtsp_client_interleaved_track_state_t;

struct turbo_rtsp_server_s {
    turbo_rtsp_server_handlers_t handlers;
    void *user_data;
    char bind_host[64];
    char server_name[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
    char public_methods[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
    char ws_path[TURBO_RTSP_MAX_URI_LEN];
    char ws_subprotocol[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
    turbo_rtsp_control_transport_t control_transport;
    turbo_rtsp_kcp_config_t kcp_config;
    cnet_tls_server_config tls_config;
    char *tls_cert_file;
    char *tls_key_file;
    char *tls_key_password;
    char *tls_ca_file;
    char *tls_ca_path;
    int port;
    uint64_t client_timeout_ms;
    size_t connection_capacity;
    size_t send_queue_capacity;
    size_t send_queue_bytes;
    struct turbo_rtsp_session_s *sessions;
    struct turbo_rtsp_server_send_s *send_queue;
    size_t send_head;
    size_t send_count;
    size_t send_bytes;
    cnet_listener listener;
    cnet_client network;
    cnet_packet_endpoint packet_endpoint;
    chttp_server http;
    salts_mutex_t mutex;
    salts_cond_t state_changed;
    salts_thread_t worker;
    int sync_initialized;
    int listener_initialized;
    int network_initialized;
    int packet_initialized;
    int http_initialized;
    int worker_started;
    int start_finished;
    int start_status;
    int started;
    int stopping;
};

struct turbo_rtsp_session_s {
    turbo_rtsp_server_t *server;
    turbo_rtsp_io_kind_t io_kind;
    cnet_connection connection;
    cnet_packet_session packet_session;
    chttp_server_websocket_session websocket_session;
    uint32_t generation;
    int active;
    int close_after_flush;
    char *pending;
    size_t pending_len;
    size_t pending_cap;
    turbo_rtsp_request_t last_request;
    turbo_rtsp_message_t last_message;
    turbo_rtsp_rtp_udp_pair_t *udp_pair;
    turbo_rtsp_transport_spec_t udp_transport;
    int has_udp_transport;
    char udp_transport_header[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
};

typedef struct turbo_rtsp_server_send_s {
    size_t session_index;
    uint32_t session_generation;
    uint8_t *data;
    size_t size;
    int close_after;
} turbo_rtsp_server_send_t;

struct turbo_rtsp_client_s {
    char host[256];
    int port;
    uint64_t timeout_ms;
    char user_agent[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
    turbo_rtsp_control_transport_t control_transport;
    turbo_rtsp_kcp_config_t kcp_config;
    char ws_path[TURBO_RTSP_MAX_URI_LEN];
    char ws_subprotocol[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
    cnet_client network;
    cnet_connection connection;
    cnet_packet_endpoint packet_endpoint;
    cnet_packet_session packet_session;
    chttp_websocket_client websocket;
    chttp_tls_profile tls_profile;
    int network_initialized;
    int packet_initialized;
    int websocket_initialized;
    int tls_initialized;
    int connect_finished;
    int connect_status;
    int send_finished;
    int receive_finished;
    int receive_status;
    uint32_t h264_rtp_ssrc_seed;
    uint16_t h264_rtp_initial_sequence;
    uint32_t h264_rtp_initial_timestamp;
    char auth_username[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
    char auth_password[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
    turbo_rtsp_auth_scheme_t auth_preferred;
    char digest_nonce[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
    uint32_t digest_nonce_count;
    char presentation_uri[TURBO_RTSP_MAX_URI_LEN];
    char session_id[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
    turbo_rtsp_session_header_t session;
    uint32_t next_cseq;
    int connected;
    int announced;
    int setup_done;
    int recording;
    int playing;
    char recv_buffer[TURBO_RTSP_CLIENT_RECV_BUFFER_SIZE];
    size_t recv_len;
    turbo_rtsp_client_response_t last_response;
    turbo_rtsp_transport_spec_t last_setup_transport;
    int has_last_setup_transport;
    turbo_rtsp_client_media_track_t media_tracks[TURBO_RTSP_MAX_MEDIA_TRACKS];
    turbo_rtsp_client_interleaved_track_state_t interleaved_tracks[TURBO_RTSP_MAX_MEDIA_TRACKS];
    size_t media_track_count;
    char last_content_type[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
    char *last_header_names[TURBO_RTSP_MAX_MESSAGE_HEADERS];
    char *last_header_values[TURBO_RTSP_MAX_MESSAGE_HEADERS];
    char *last_body;
    size_t last_body_len;
    disruptor_t *frame_queue;
    disruptor_consumer_t frame_consumer;
    uint64_t frame_next_sequence;
    int frame_consumer_registered;
};

static void turbo_rtsp_safe_copy(char *dst, size_t dst_size, const char *src) {
    size_t len = 0;

    if (!dst || dst_size == 0) {
        return;
    }

    if (!src) {
        dst[0] = '\0';
        return;
    }

    len = strlen(src);
    if (len >= dst_size) {
        len = dst_size - 1;
    }

    memcpy(dst, src, len);
    dst[len] = '\0';
}

static int turbo_rtsp_copy_range(
    char *dst,
    size_t dst_size,
    const char *start,
    const char *end) {
    size_t len = 0;

    if (!dst || dst_size == 0 || !start || !end || end < start) {
        return -1;
    }

    len = (size_t)(end - start);
    if (len == 0 || len >= dst_size) {
        return -1;
    }

    memcpy(dst, start, len);
    dst[len] = '\0';
    return 0;
}

static int turbo_rtsp_parse_port(
    const char *start,
    const char *end,
    int *port) {
    int value = 0;
    const char *cursor = start;

    if (!start || !end || start >= end || !port) {
        return -1;
    }

    while (cursor < end) {
        if (!isdigit((unsigned char)*cursor)) {
            return -1;
        }
        value = value * 10 + (*cursor - '0');
        if (value > 65535) {
            return -1;
        }
        ++cursor;
    }

    if (value <= 0) {
        return -1;
    }

    *port = value;
    return 0;
}

int turbo_rtsp_url_parse(
    const char *url,
    turbo_rtsp_url_t *parsed) {
    static const char scheme[] = "rtsp://";
    const char *authority_start = NULL;
    const char *authority_end = NULL;
    const char *host_start = NULL;
    const char *host_end = NULL;
    const char *port_start = NULL;
    const char *cursor = NULL;
    const char *last_at = NULL;

    if (!url || !parsed || strncmp(url, scheme, sizeof(scheme) - 1) != 0) {
        return -1;
    }

    memset(parsed, 0, sizeof(*parsed));
    parsed->port = TURBO_RTSP_DEFAULT_PORT;

    authority_start = url + sizeof(scheme) - 1;
    authority_end = strchr(authority_start, '/');
    if (!authority_end || authority_end == authority_start) {
        return -1;
    }

    for (cursor = authority_start; cursor < authority_end; ++cursor) {
        if (*cursor == '@') {
            last_at = cursor;
        }
    }
    host_start = last_at ? last_at + 1 : authority_start;
    if (host_start >= authority_end) {
        return -1;
    }

    if (*host_start == '[') {
        host_start++;
        host_end = host_start;
        while (host_end < authority_end && *host_end != ']') {
            ++host_end;
        }
        if (host_end == host_start || host_end >= authority_end || *host_end != ']') {
            return -1;
        }
        cursor = host_end + 1;
        if (cursor < authority_end) {
            if (*cursor != ':') {
                return -1;
            }
            port_start = cursor + 1;
        }
    } else {
        host_end = host_start;
        while (host_end < authority_end && *host_end != ':') {
            ++host_end;
        }
        if (host_end == host_start) {
            return -1;
        }
        if (host_end < authority_end) {
            port_start = host_end + 1;
        }
    }

    if (turbo_rtsp_copy_range(
            parsed->host,
            sizeof(parsed->host),
            host_start,
            host_end) != 0) {
        return -1;
    }

    if (port_start &&
        turbo_rtsp_parse_port(port_start, authority_end, &parsed->port) != 0) {
        return -1;
    }

    if (turbo_rtsp_copy_range(
            parsed->path,
            sizeof(parsed->path),
            authority_end,
            url + strlen(url)) != 0) {
        return -1;
    }

    return 0;
}

static int turbo_rtsp_ascii_ieq_n(
    const char *left,
    const char *right,
    size_t len) {
    size_t i = 0;

    if (!left || !right) {
        return 0;
    }

    for (i = 0; i < len; ++i) {
        if (tolower((unsigned char)left[i]) !=
            tolower((unsigned char)right[i])) {
            return 0;
        }
    }
    return 1;
}

static int turbo_rtsp_control_transport_is_ws(turbo_rtsp_control_transport_t transport) {
    return transport == TURBO_RTSP_CONTROL_TRANSPORT_WS ||
           transport == TURBO_RTSP_CONTROL_TRANSPORT_WSS;
}

static int turbo_rtsp_control_transport_is_tls(turbo_rtsp_control_transport_t transport) {
    return transport == TURBO_RTSP_CONTROL_TRANSPORT_WSS;
}

static native_io_backend_kind turbo_rtsp_backend(void) {
#ifdef _WIN32
    return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
    return NATIVE_IO_BACKEND_IO_URING;
#else
    return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static void turbo_rtsp_secure_wipe(void *value, size_t size) {
    volatile unsigned char *cursor = (volatile unsigned char *)value;
    while (cursor && size > 0) {
        *cursor++ = 0;
        --size;
    }
}

void turbo_rtsp_kcp_config_init(turbo_rtsp_kcp_config_t *config) {
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->transport = (cnet_kcp_config)CNET_KCP_CONFIG_INIT;
    config->transport.stream_mode = false;
    config->security = (cnet_kcp_security_config)CNET_KCP_SECURITY_CONFIG_INIT;
    config->security.mode = CNET_KCP_SECURITY_PSK_V1;
    config->transport.mtu =
        config->security.fec.max_payload_bytes -
        CNET_KCP_SECURE_RECORD_OVERHEAD;
}

void turbo_rtsp_kcp_config_wipe(turbo_rtsp_kcp_config_t *config) {
    if (config) {
        turbo_rtsp_secure_wipe(config, sizeof(*config));
    }
}

static int turbo_rtsp_kcp_config_valid(const turbo_rtsp_kcp_config_t *config) {
    size_t i;
    int has_key = 0;

    if (!config || config->transport.size != sizeof(config->transport) ||
        config->security.size != sizeof(config->security) ||
        config->security.mode != CNET_KCP_SECURITY_PSK_V1 ||
        config->transport.conversation != 0 || config->transport.stream_mode ||
        config->transport.observer.output || config->transport.observer.on_receive ||
        config->transport.observer.user) {
        return 0;
    }
    for (i = 0; i < CNET_KCP_PSK_BYTES; ++i) {
        has_key |= config->security.pre_shared_key[i] != 0;
    }
    return has_key;
}

static char *turbo_rtsp_strdup(const char *value) {
    size_t size;
    char *copy;
    if (!value) {
        return NULL;
    }
    size = strlen(value) + 1u;
    copy = (char *)malloc(size);
    if (copy) {
        memcpy(copy, value, size);
    }
    return copy;
}

static int turbo_rtsp_server_enqueue(
    turbo_rtsp_session_t *session,
    const void *data,
    size_t size,
    int close_after) {
    turbo_rtsp_server_t *server;
    turbo_rtsp_server_send_t *command;
    uint8_t *copy;
    size_t index;

    if (!session || !session->server || !data || size == 0) {
        return -1;
    }
    server = session->server;
    if (session->io_kind == TURBO_RTSP_IO_WEBSOCKET) {
        int status = chttp_server_websocket_send_binary(
            &session->websocket_session, data, size);
        if (status == SALTS_OK && close_after) {
            status = chttp_server_websocket_close(
                &session->websocket_session, 1002u, NULL, 0u);
        }
        return status == SALTS_OK ? 0 : -1;
    }

    copy = (uint8_t *)malloc(size);
    if (!copy) {
        return -1;
    }
    memcpy(copy, data, size);

    salts_mutex_lock(&server->mutex);
    if (!session->active || server->stopping ||
        server->send_count == server->send_queue_capacity ||
        size > server->send_queue_bytes - server->send_bytes) {
        salts_mutex_unlock(&server->mutex);
        free(copy);
        return -1;
    }
    index = (server->send_head + server->send_count) % server->send_queue_capacity;
    command = &server->send_queue[index];
    command->session_index = (size_t)(session - server->sessions);
    command->session_generation = session->generation;
    command->data = copy;
    command->size = size;
    command->close_after = close_after;
    server->send_count++;
    server->send_bytes += size;
    if (close_after) {
        session->close_after_flush = 1;
    }
    salts_mutex_unlock(&server->mutex);

    if (session->io_kind == TURBO_RTSP_IO_STREAM && server->network_initialized) {
        (void)cnet_client_wake(&server->network);
    } else if (session->io_kind == TURBO_RTSP_IO_PACKET && server->packet_initialized) {
        (void)cnet_packet_wake(&server->packet_endpoint);
    }
    return 0;
}

static void turbo_rtsp_session_clear_udp(turbo_rtsp_session_t *session) {
    if (!session) {
        return;
    }

    if (session->udp_pair) {
        turbo_rtsp_rtp_udp_pair_destroy(session->udp_pair);
        session->udp_pair = NULL;
    }
    session->has_udp_transport = 0;
    memset(&session->udp_transport, 0, sizeof(session->udp_transport));
    session->udp_transport_header[0] = '\0';
}

static int turbo_rtsp_response_ok(turbo_rtsp_response_t *response) {
    if (!response) {
        return -1;
    }
    memset(response, 0, sizeof(*response));
    response->status_code = 200;
    return 0;
}

static const char *turbo_rtsp_redirect_reason(int status_code) {
    switch (status_code) {
    case 300:
        return "Multiple Choices";
    case 301:
        return "Moved Permanently";
    case 302:
        return "Moved Temporarily";
    case 303:
        return "See Other";
    case 305:
        return "Use Proxy";
    case 307:
        return "Temporary Redirect";
    case 308:
        return "Permanent Redirect";
    default:
        return "Redirect";
    }
}

static int turbo_rtsp_send_interleaved_frame(
    turbo_rtsp_session_t *session,
    uint8_t channel,
    const uint8_t *payload,
    size_t payload_len) {
    uint8_t *frame = NULL;
    int rc = 0;

    if (!session || (!payload && payload_len > 0) || payload_len > UINT16_MAX) {
        return -1;
    }

    frame = (uint8_t *)malloc(TURBO_RTSP_INTERLEAVED_HEADER_SIZE + payload_len);
    if (!frame) {
        return -1;
    }
    if (turbo_rtsp_interleaved_write_header(
            frame,
            TURBO_RTSP_INTERLEAVED_HEADER_SIZE,
            channel,
            (uint16_t)payload_len) != TURBO_RTSP_INTERLEAVED_HEADER_SIZE) {
        free(frame);
        return -1;
    }
    if (payload_len > 0) {
        memcpy(frame + TURBO_RTSP_INTERLEAVED_HEADER_SIZE, payload, payload_len);
    }

    rc = turbo_rtsp_server_enqueue(
        session,
        frame,
        TURBO_RTSP_INTERLEAVED_HEADER_SIZE + payload_len,
        0);
    free(frame);
    return rc == 0 ? 0 : -1;
}

int turbo_rtsp_response_options(
    turbo_rtsp_response_t *response,
    const char *public_methods) {
    if (turbo_rtsp_response_ok(response) != 0) {
        return -1;
    }
    response->public_methods = public_methods;
    return 0;
}

int turbo_rtsp_response_status(
    turbo_rtsp_response_t *response,
    int status_code,
    const char *reason) {
    if (!response || status_code <= 0) {
        return -1;
    }
    memset(response, 0, sizeof(*response));
    response->status_code = status_code;
    response->reason = reason;
    return 0;
}

int turbo_rtsp_response_redirect(
    turbo_rtsp_response_t *response,
    int status_code,
    const char *location) {
    if (!response || status_code < 300 || status_code >= 400 ||
        !location || location[0] == '\0') {
        return -1;
    }

    memset(response, 0, sizeof(*response));
    response->status_code = status_code;
    response->reason = turbo_rtsp_redirect_reason(status_code);
    response->location_header.name = "Location";
    response->location_header.value = location;
    response->headers = &response->location_header;
    response->header_count = 1;
    return 0;
}

int turbo_rtsp_response_describe(
    turbo_rtsp_response_t *response,
    const char *sdp,
    size_t sdp_len) {
    if (!sdp || turbo_rtsp_response_ok(response) != 0) {
        return -1;
    }
    response->content_type = "application/sdp";
    response->body = sdp;
    response->body_len = sdp_len;
    return 0;
}

int turbo_rtsp_response_setup(
    turbo_rtsp_response_t *response,
    const char *session_id,
    const char *transport) {
    if (!session_id || !transport || turbo_rtsp_response_ok(response) != 0) {
        return -1;
    }
    response->session_id = session_id;
    response->transport = transport;
    return 0;
}

int turbo_rtsp_response_play(
    turbo_rtsp_response_t *response,
    const char *range,
    const char *rtp_info) {
    if (turbo_rtsp_response_ok(response) != 0) {
        return -1;
    }
    response->range = range;
    response->rtp_info = rtp_info;
    return 0;
}

int turbo_rtsp_response_pause(turbo_rtsp_response_t *response) {
    return turbo_rtsp_response_ok(response);
}

int turbo_rtsp_response_teardown(turbo_rtsp_response_t *response) {
    return turbo_rtsp_response_ok(response);
}

int turbo_rtsp_response_announce(turbo_rtsp_response_t *response) {
    return turbo_rtsp_response_ok(response);
}

int turbo_rtsp_response_record(
    turbo_rtsp_response_t *response,
    const char *range) {
    if (turbo_rtsp_response_ok(response) != 0) {
        return -1;
    }
    response->range = range;
    return 0;
}

int turbo_rtsp_response_get_parameter(
    turbo_rtsp_response_t *response,
    const char *content_type,
    const char *body,
    size_t body_len) {
    if (turbo_rtsp_response_ok(response) != 0) {
        return -1;
    }
    response->content_type = content_type;
    response->body = body;
    response->body_len = body_len;
    return 0;
}

int turbo_rtsp_response_set_parameter(turbo_rtsp_response_t *response) {
    return turbo_rtsp_response_ok(response);
}

static const char *turbo_rtsp_client_method_name(turbo_rtsp_method_t method) {
    switch (method) {
    case TURBO_RTSP_METHOD_OPTIONS:
        return "OPTIONS";
    case TURBO_RTSP_METHOD_DESCRIBE:
        return "DESCRIBE";
    case TURBO_RTSP_METHOD_SETUP:
        return "SETUP";
    case TURBO_RTSP_METHOD_PLAY:
        return "PLAY";
    case TURBO_RTSP_METHOD_TEARDOWN:
        return "TEARDOWN";
    case TURBO_RTSP_METHOD_PAUSE:
        return "PAUSE";
    case TURBO_RTSP_METHOD_ANNOUNCE:
        return "ANNOUNCE";
    case TURBO_RTSP_METHOD_RECORD:
        return "RECORD";
    case TURBO_RTSP_METHOD_GET_PARAMETER:
        return "GET_PARAMETER";
    case TURBO_RTSP_METHOD_SET_PARAMETER:
        return "SET_PARAMETER";
    case TURBO_RTSP_METHOD_REDIRECT:
        return "REDIRECT";
    case TURBO_RTSP_METHOD_UNKNOWN:
    default:
        return NULL;
    }
}

static turbo_rtsp_request_cb turbo_rtsp_pick_handler(
    turbo_rtsp_server_t *server,
    turbo_rtsp_method_t method) {
    switch (method) {
    case TURBO_RTSP_METHOD_OPTIONS:
        return server->handlers.on_options;
    case TURBO_RTSP_METHOD_DESCRIBE:
        return server->handlers.on_describe;
    case TURBO_RTSP_METHOD_SETUP:
        return server->handlers.on_setup;
    case TURBO_RTSP_METHOD_PLAY:
        return server->handlers.on_play;
    case TURBO_RTSP_METHOD_TEARDOWN:
        return server->handlers.on_teardown;
    case TURBO_RTSP_METHOD_PAUSE:
        return server->handlers.on_pause;
    case TURBO_RTSP_METHOD_ANNOUNCE:
        return server->handlers.on_announce;
    case TURBO_RTSP_METHOD_RECORD:
        return server->handlers.on_record;
    case TURBO_RTSP_METHOD_GET_PARAMETER:
        return server->handlers.on_get_parameter;
    case TURBO_RTSP_METHOD_SET_PARAMETER:
        return server->handlers.on_set_parameter;
    case TURBO_RTSP_METHOD_REDIRECT:
        return server->handlers.on_redirect;
    case TURBO_RTSP_METHOD_UNKNOWN:
    default:
        return server->handlers.on_unknown;
    }
}

static void turbo_rtsp_init_response(
    turbo_rtsp_server_t *server,
    const turbo_rtsp_request_t *request,
    turbo_rtsp_response_t *response) {
    memset(response, 0, sizeof(*response));

    if (request->method == TURBO_RTSP_METHOD_OPTIONS) {
        response->status_code = 200;
        response->public_methods = server->public_methods;
        return;
    }

    if (request->method == TURBO_RTSP_METHOD_UNKNOWN) {
        response->status_code = 501;
        return;
    }

    response->status_code = 501;
}

static int turbo_rtsp_write_response(
    turbo_rtsp_session_t *session,
    uint32_t cseq,
    const turbo_rtsp_response_t *response) {
    char *buffer = NULL;
    int len = 0;
    int rc = 0;

    if (!session || !session->active || !response) {
        return -1;
    }

    buffer = (char *)malloc(TURBO_RTSP_RESPONSE_BUFFER_SIZE);
    if (!buffer) {
        return -1;
    }

    len = turbo_rtsp_format_response(
        buffer,
        TURBO_RTSP_RESPONSE_BUFFER_SIZE,
        cseq,
        session->server->server_name,
        response);
    if (len < 0) {
        free(buffer);
        return -1;
    }

    rc = turbo_rtsp_server_enqueue(session, buffer, (size_t)len, 0);
    free(buffer);
    return rc == 0 ? 0 : -1;
}

static int turbo_rtsp_respond_parse_error(turbo_rtsp_session_t *session) {
    turbo_rtsp_response_t response;
    const char *body = "Malformed RTSP request";

    memset(&response, 0, sizeof(response));
    response.status_code = 400;
    response.reason = "Bad Request";
    response.content_type = "text/plain";
    response.body = body;
    response.body_len = strlen(body);
    return turbo_rtsp_write_response(session, 0, &response);
}

static int turbo_rtsp_parse_one(
    turbo_rtsp_session_t *session,
    char *buffer,
    size_t buffer_len,
    size_t *consumed) {
    turbo_rtsp_request_cb handler = NULL;
    turbo_rtsp_response_t response;
    int callback_rc = 0;
    int parse_rc = 0;

    *consumed = 0;

    if (buffer_len > 0 && (unsigned char)buffer[0] == '$') {
        turbo_rtsp_interleaved_frame_t frame;
        int frame_rc = turbo_rtsp_interleaved_parse(
            (const uint8_t *)buffer,
            buffer_len,
            &frame,
            consumed);
        if (frame_rc == TURBO_RTSP_FRAME_PARTIAL) {
            return 1;
        }
        if (frame_rc != TURBO_RTSP_FRAME_OK) {
            return -1;
        }
        if (session->server->handlers.on_interleaved_frame &&
            session->server->handlers.on_interleaved_frame(
                session,
                frame.channel,
                frame.payload,
                frame.payload_len,
                session->server->user_data) != 0) {
            return -1;
        }
        return 0;
    }

    parse_rc = turbo_rtsp_parse_message(
        buffer,
        buffer_len,
        consumed,
        &session->last_message);
    if (parse_rc == TURBO_RTSP_PARSE_PARTIAL) {
        return 1;
    }
    if (parse_rc != TURBO_RTSP_PARSE_OK) {
        turbo_rtsp_respond_parse_error(session);
        return -1;
    }
    session->last_request = session->last_message.request;

    turbo_rtsp_init_response(session->server, &session->last_request, &response);
    handler = turbo_rtsp_pick_handler(session->server, session->last_request.method);
    if (handler) {
        callback_rc = handler(
            session,
            &session->last_request,
            &response,
            session->server->user_data);
        if (callback_rc != 0) {
            memset(&response, 0, sizeof(response));
            response.status_code = 500;
            response.reason = "Internal Server Error";
        }
    }

    if (turbo_rtsp_write_response(session, session->last_request.cseq, &response) != 0) {
        return -1;
    }

    return 0;
}

static void turbo_rtsp_notify_session_close(turbo_rtsp_session_t *session) {
    if (session && session->server && session->server->handlers.on_session_close) {
        session->server->handlers.on_session_close(
            session,
            session->server->user_data);
    }
}

static int turbo_rtsp_next_power_of_two(size_t minimum, size_t *out) {
    size_t value = 1u;
    if (!out || minimum == 0) {
        return -1;
    }
    while (value < minimum) {
        if (value > SIZE_MAX / 2u) {
            return -1;
        }
        value *= 2u;
    }
    *out = value;
    return 0;
}

static turbo_rtsp_session_t *turbo_rtsp_server_session_allocate(
    turbo_rtsp_server_t *server,
    turbo_rtsp_io_kind_t io_kind) {
    size_t i;
    turbo_rtsp_session_t *session = NULL;

    salts_mutex_lock(&server->mutex);
    for (i = 0; i < server->connection_capacity; ++i) {
        if (!server->sessions[i].active) {
            uint32_t generation = server->sessions[i].generation + 1u;
            if (generation == 0) {
                generation = 1u;
            }
            memset(&server->sessions[i], 0, sizeof(server->sessions[i]));
            session = &server->sessions[i];
            session->server = server;
            session->io_kind = io_kind;
            session->generation = generation;
            session->active = 1;
            break;
        }
    }
    salts_mutex_unlock(&server->mutex);
    return session;
}

static void turbo_rtsp_server_session_release(
    turbo_rtsp_session_t *session,
    int notify) {
    turbo_rtsp_server_t *server;
    uint32_t generation;

    if (!session || !session->server) {
        return;
    }
    server = session->server;
    salts_mutex_lock(&server->mutex);
    if (!session->active) {
        salts_mutex_unlock(&server->mutex);
        return;
    }
    session->active = 0;
    generation = session->generation;
    salts_mutex_unlock(&server->mutex);

    if (notify) {
        turbo_rtsp_notify_session_close(session);
    }
    turbo_rtsp_session_clear_udp(session);
    free(session->pending);
    memset(session, 0, sizeof(*session));
    session->server = server;
    session->generation = generation;
}

static int turbo_rtsp_server_session_append(
    turbo_rtsp_session_t *session,
    const void *data,
    size_t size) {
    size_t offset = 0;

    if (!session || !session->active || !data || size == 0 ||
        size > TURBO_RTSP_SERVER_MAX_PENDING_BYTES - session->pending_len) {
        return -1;
    }
    if (session->pending_cap - session->pending_len < size) {
        size_t capacity = session->pending_cap ? session->pending_cap : 2048u;
        char *resized;
        while (capacity - session->pending_len < size) {
            if (capacity >= TURBO_RTSP_SERVER_MAX_PENDING_BYTES) {
                return -1;
            }
            capacity = capacity > TURBO_RTSP_SERVER_MAX_PENDING_BYTES / 2u
                           ? TURBO_RTSP_SERVER_MAX_PENDING_BYTES
                           : capacity * 2u;
        }
        resized = (char *)realloc(session->pending, capacity);
        if (!resized) {
            return -1;
        }
        session->pending = resized;
        session->pending_cap = capacity;
    }
    memcpy(session->pending + session->pending_len, data, size);
    session->pending_len += size;

    while (offset < session->pending_len) {
        size_t consumed = 0;
        int status = turbo_rtsp_parse_one(
            session,
            session->pending + offset,
            session->pending_len - offset,
            &consumed);
        if (status > 0) {
            break;
        }
        if (status < 0 || consumed == 0) {
            session->close_after_flush = 1;
            return -1;
        }
        offset += consumed;
    }
    if (offset > 0) {
        size_t remaining = session->pending_len - offset;
        if (remaining > 0) {
            memmove(session->pending, session->pending + offset, remaining);
        }
        session->pending_len = remaining;
    }
    return 0;
}

static turbo_rtsp_session_t *turbo_rtsp_server_find_stream_session(
    turbo_rtsp_server_t *server,
    cnet_connection connection) {
    size_t i;
    for (i = 0; i < server->connection_capacity; ++i) {
        turbo_rtsp_session_t *session = &server->sessions[i];
        if (session->active && session->io_kind == TURBO_RTSP_IO_STREAM &&
            session->connection.slot == connection.slot &&
            session->connection.generation == connection.generation) {
            return session;
        }
    }
    return NULL;
}

static turbo_rtsp_session_t *turbo_rtsp_server_find_packet_session(
    turbo_rtsp_server_t *server,
    cnet_packet_session packet) {
    size_t i;
    for (i = 0; i < server->connection_capacity; ++i) {
        turbo_rtsp_session_t *session = &server->sessions[i];
        if (session->active && session->io_kind == TURBO_RTSP_IO_PACKET &&
            session->packet_session.slot == packet.slot &&
            session->packet_session.generation == packet.generation) {
            return session;
        }
    }
    return NULL;
}

static turbo_rtsp_session_t *turbo_rtsp_server_find_websocket_session(
    turbo_rtsp_server_t *server,
    const chttp_server_websocket_session *websocket) {
    size_t i;
    for (i = 0; i < server->connection_capacity; ++i) {
        turbo_rtsp_session_t *session = &server->sessions[i];
        if (session->active && session->io_kind == TURBO_RTSP_IO_WEBSOCKET &&
            session->websocket_session.connection_slot == websocket->connection_slot &&
            session->websocket_session.connection_generation == websocket->connection_generation &&
            session->websocket_session.stream_id == websocket->stream_id) {
            return session;
        }
    }
    return NULL;
}

static void turbo_rtsp_server_pop_send(turbo_rtsp_server_t *server) {
    turbo_rtsp_server_send_t *command;
    salts_mutex_lock(&server->mutex);
    if (server->send_count == 0) {
        salts_mutex_unlock(&server->mutex);
        return;
    }
    command = &server->send_queue[server->send_head];
    server->send_bytes -= command->size;
    free(command->data);
    memset(command, 0, sizeof(*command));
    server->send_head = (server->send_head + 1u) % server->send_queue_capacity;
    server->send_count--;
    salts_mutex_unlock(&server->mutex);
}

static int turbo_rtsp_server_drain_sends(turbo_rtsp_server_t *server) {
    for (;;) {
        turbo_rtsp_server_send_t command;
        turbo_rtsp_session_t *session;
        int status;

        salts_mutex_lock(&server->mutex);
        if (server->send_count == 0) {
            salts_mutex_unlock(&server->mutex);
            return 0;
        }
        command = server->send_queue[server->send_head];
        if (command.session_index >= server->connection_capacity) {
            salts_mutex_unlock(&server->mutex);
            turbo_rtsp_server_pop_send(server);
            continue;
        }
        session = &server->sessions[command.session_index];
        if (!session->active || session->generation != command.session_generation) {
            salts_mutex_unlock(&server->mutex);
            turbo_rtsp_server_pop_send(server);
            continue;
        }
        salts_mutex_unlock(&server->mutex);

        if (session->io_kind == TURBO_RTSP_IO_STREAM) {
            status = (command.close_after || session->close_after_flush)
                         ? cnet_send_and_close(
                               &server->network, session->connection,
                               command.data, command.size)
                         : cnet_send(
                               &server->network, session->connection,
                               command.data, command.size);
        } else if (session->io_kind == TURBO_RTSP_IO_PACKET) {
            status = cnet_packet_send(
                &server->packet_endpoint, session->packet_session,
                command.data, command.size);
            if (status == SALTS_OK &&
                (command.close_after || session->close_after_flush)) {
                (void)cnet_packet_session_close(
                    &server->packet_endpoint, session->packet_session);
            }
        } else {
            status = SALTS_ENOENT;
        }

        if (status == SALTS_EBUSY || status == SALTS_ENOBUFS) {
            return 0;
        }
        turbo_rtsp_server_pop_send(server);
        if (status != SALTS_OK) {
            if (session->io_kind == TURBO_RTSP_IO_STREAM) {
                (void)cnet_close(&server->network, session->connection);
            } else if (session->io_kind == TURBO_RTSP_IO_PACKET) {
                (void)cnet_packet_session_close(
                    &server->packet_endpoint, session->packet_session);
            }
            return -1;
        }
    }
}

static void turbo_rtsp_server_stream_state(
    void *user,
    cnet_connection connection,
    cnet_connection_state state,
    const cnet_error *error) {
    turbo_rtsp_session_t *session = (turbo_rtsp_session_t *)user;
    (void)error;
    if (!session || !session->server ||
        session->connection.slot != connection.slot ||
        session->connection.generation != connection.generation) {
        return;
    }
    if (state == CNET_CONNECTION_CONNECTED) {
        if (cnet_receive(&session->server->network, connection, 1u) != SALTS_OK) {
            (void)cnet_close(&session->server->network, connection);
        }
    } else if (state == CNET_CONNECTION_CLOSED || state == CNET_CONNECTION_FAILED) {
        turbo_rtsp_server_session_release(session, 1);
    }
}

static void turbo_rtsp_server_stream_receive(
    void *user,
    cnet_connection connection,
    const cnet_receive_view *view) {
    turbo_rtsp_session_t *session = (turbo_rtsp_session_t *)user;
    if (!session || !session->active || !view ||
        turbo_rtsp_server_session_append(session, view->data, view->size) != 0) {
        if (session && session->active && !session->close_after_flush) {
            (void)cnet_close(&session->server->network, connection);
        }
        return;
    }
    if (!session->close_after_flush &&
        cnet_receive(&session->server->network, connection, 1u) != SALTS_OK) {
        (void)cnet_close(&session->server->network, connection);
    }
}

static void turbo_rtsp_server_stream_send(
    void *user,
    cnet_connection connection,
    size_t size) {
    turbo_rtsp_session_t *session = (turbo_rtsp_session_t *)user;
    (void)connection;
    (void)size;
    if (session && session->server) {
        (void)turbo_rtsp_server_drain_sends(session->server);
    }
}

static int turbo_rtsp_server_packet_admit(
    void *user,
    cnet_packet_endpoint *endpoint,
    cnet_packet_protocol protocol,
    const cnet_datagram_peer *peer,
    uint32_t conversation) {
    turbo_rtsp_server_t *server = (turbo_rtsp_server_t *)user;
    size_t i;
    (void)endpoint;
    (void)peer;
    (void)conversation;
    if (!server || protocol != CNET_PACKET_KCP) {
        return SALTS_EINVAL;
    }
    for (i = 0; i < server->connection_capacity; ++i) {
        if (!server->sessions[i].active) {
            return SALTS_OK;
        }
    }
    return SALTS_ENOBUFS;
}

static void turbo_rtsp_server_packet_state(
    void *user,
    cnet_packet_endpoint *endpoint,
    cnet_packet_session packet,
    cnet_packet_session_state state,
    const cnet_datagram_peer *peer,
    uint32_t conversation) {
    turbo_rtsp_server_t *server = (turbo_rtsp_server_t *)user;
    turbo_rtsp_session_t *session;
    (void)endpoint;
    (void)peer;
    (void)conversation;
    if (!server) {
        return;
    }
    session = turbo_rtsp_server_find_packet_session(server, packet);
    if ((state == CNET_PACKET_SESSION_CONNECTING || state == CNET_PACKET_SESSION_OPEN) &&
        !session) {
        session = turbo_rtsp_server_session_allocate(server, TURBO_RTSP_IO_PACKET);
        if (session) {
            session->packet_session = packet;
        }
    } else if (state == CNET_PACKET_SESSION_CLOSED && session) {
        turbo_rtsp_server_session_release(session, 1);
    }
}

static void turbo_rtsp_server_packet_receive(
    void *user,
    cnet_packet_endpoint *endpoint,
    cnet_packet_session packet,
    const cnet_receive_view *view) {
    turbo_rtsp_server_t *server = (turbo_rtsp_server_t *)user;
    turbo_rtsp_session_t *session =
        server ? turbo_rtsp_server_find_packet_session(server, packet) : NULL;
    (void)endpoint;
    if (!session || !view ||
        turbo_rtsp_server_session_append(session, view->data, view->size) != 0) {
        if (session && !session->close_after_flush) {
            (void)cnet_packet_session_close(&server->packet_endpoint, packet);
        }
    }
}

static void turbo_rtsp_server_packet_error(
    void *user,
    cnet_packet_endpoint *endpoint,
    cnet_packet_session packet,
    int status) {
    turbo_rtsp_server_t *server = (turbo_rtsp_server_t *)user;
    (void)endpoint;
    (void)status;
    if (server) {
        (void)cnet_packet_session_close(&server->packet_endpoint, packet);
    }
}

static int turbo_rtsp_server_websocket_open(
    void *user,
    chttp_websocket *websocket,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
    turbo_rtsp_server_t *server = (turbo_rtsp_server_t *)user;
    turbo_rtsp_session_t *session;
    if (!server || server->stopping) {
        return SALTS_ESHUTDOWN;
    }
    session = turbo_rtsp_server_session_allocate(server, TURBO_RTSP_IO_WEBSOCKET);
    if (!session) {
        return SALTS_ENOBUFS;
    }
    if (chttp_server_websocket_session_capture(
            websocket, &session->websocket_session) != SALTS_OK ||
        (server->ws_subprotocol[0] &&
         chttp_server_response_select_websocket_subprotocol(
             response, request, server->ws_subprotocol) != SALTS_OK)) {
        turbo_rtsp_server_session_release(session, 0);
        return SALTS_EINVAL;
    }
    return SALTS_OK;
}

static void turbo_rtsp_server_websocket_event(
    void *user,
    chttp_websocket *websocket,
    const chttp_websocket_event *event) {
    turbo_rtsp_server_t *server = (turbo_rtsp_server_t *)user;
    chttp_server_websocket_session captured = {0};
    turbo_rtsp_session_t *session;
    if (!server || !event ||
        chttp_server_websocket_session_capture(websocket, &captured) != SALTS_OK) {
        return;
    }
    session = turbo_rtsp_server_find_websocket_session(server, &captured);
    if (!session) {
        return;
    }
    if (event->kind == CHTTP_WEBSOCKET_EVENT_MESSAGE) {
        if (turbo_rtsp_server_session_append(session, event->data, event->size) != 0) {
            (void)chttp_websocket_close(websocket, 1002u, NULL, 0u);
        }
    } else if (event->kind == CHTTP_WEBSOCKET_EVENT_CLOSE) {
        turbo_rtsp_server_session_release(session, 1);
    }
}

static int turbo_rtsp_server_stream_init(turbo_rtsp_server_t *server) {
    cnet_client_config network = {0};
    cnet_listener_config listener = {0};
    size_t queue_capacity;
    size_t event_capacity;

    if (server->connection_capacity > SIZE_MAX / 2u ||
        turbo_rtsp_next_power_of_two(
            server->connection_capacity * 2u > TURBO_RTSP_SERVER_MIN_QUEUE_CAPACITY
                ? server->connection_capacity * 2u
                : TURBO_RTSP_SERVER_MIN_QUEUE_CAPACITY,
            &queue_capacity) != 0 ||
        turbo_rtsp_next_power_of_two(server->connection_capacity * 2u, &event_capacity) != 0) {
        return SALTS_ERANGE;
    }
    network.backend = turbo_rtsp_backend();
    network.connection_capacity = server->connection_capacity;
    network.command_capacity = queue_capacity;
    network.request_capacity = server->connection_capacity * 2u;
    network.completion_batch_capacity =
        network.request_capacity < 256u ? network.request_capacity : 256u;
    network.event_capacity = event_capacity;
    network.max_send_bytes = TURBO_RTSP_SERVER_MAX_PENDING_BYTES;
    network.receive_buffer_bytes = TURBO_RTSP_SERVER_MAX_PENDING_BYTES;
    network.read_timeout_ms = (uint32_t)server->client_timeout_ms;
    network.write_timeout_ms = (uint32_t)server->client_timeout_ms;
    if (cnet_client_init(&server->network, &network) != SALTS_OK) {
        return SALTS_EIO;
    }
    server->network_initialized = 1;

    listener.backend = turbo_rtsp_backend();
    listener.host = server->bind_host;
    listener.port = (uint16_t)server->port;
    listener.backlog = server->connection_capacity;
    if (cnet_listener_init(&server->listener, &listener) != SALTS_OK) {
        return SALTS_EIO;
    }
    server->listener_initialized = 1;
    {
        uint16_t bound_port = 0;
        int status = cnet_listener_port(&server->listener, &bound_port);
        if (status == SALTS_OK) {
            server->port = (int)bound_port;
        }
        return status;
    }
}

static int turbo_rtsp_server_packet_init(turbo_rtsp_server_t *server) {
    cnet_packet_endpoint_config config = CNET_PACKET_ENDPOINT_CONFIG_INIT;
    size_t send_capacity;
    size_t wire_bytes;

    if (server->connection_capacity > (SIZE_MAX - 1u) / 2u) {
        return SALTS_ERANGE;
    }
    send_capacity = server->connection_capacity * 2u;
    if (send_capacity < TURBO_RTSP_SERVER_MIN_QUEUE_CAPACITY) {
        send_capacity = TURBO_RTSP_SERVER_MIN_QUEUE_CAPACITY;
    }
    wire_bytes = server->kcp_config.security.fec.max_payload_bytes +
                 CNET_KCP_SECURE_RECORD_OVERHEAD;
    if (wire_bytes > CNET_DATAGRAM_MAX_PAYLOAD_BYTES) {
        return SALTS_ERANGE;
    }
    config.protocol = CNET_PACKET_KCP;
    config.session_capacity = server->connection_capacity;
    config.datagram.backend = turbo_rtsp_backend();
    config.datagram.host = server->bind_host;
    config.datagram.port = (uint16_t)server->port;
    config.datagram.send_capacity = send_capacity;
    config.datagram.request_capacity = send_capacity + 1u;
    config.datagram.completion_batch_capacity =
        config.datagram.request_capacity < 256u ? config.datagram.request_capacity : 256u;
    config.datagram.max_datagram_bytes = wire_bytes;
    config.datagram.receive_buffer_bytes = wire_bytes;
    config.kcp = server->kcp_config.transport;
    config.security = server->kcp_config.security;
    config.observer.on_admit = turbo_rtsp_server_packet_admit;
    config.observer.on_state = turbo_rtsp_server_packet_state;
    config.observer.on_receive = turbo_rtsp_server_packet_receive;
    config.observer.on_error = turbo_rtsp_server_packet_error;
    config.observer.user = server;
    if (cnet_packet_endpoint_init(&server->packet_endpoint, &config) != SALTS_OK) {
        return SALTS_EIO;
    }
    server->packet_initialized = 1;
    {
        uint16_t bound_port = 0;
        int status = cnet_packet_endpoint_port(&server->packet_endpoint, &bound_port);
        if (status == SALTS_OK) {
            server->port = (int)bound_port;
        }
        return status;
    }
}

static void turbo_rtsp_server_stream_accept(turbo_rtsp_server_t *server) {
    int ready = 0;
    while (!server->stopping &&
           cnet_listener_wait(&server->listener, 0u, &ready) == SALTS_OK && ready) {
        turbo_rtsp_session_t *session =
            turbo_rtsp_server_session_allocate(server, TURBO_RTSP_IO_STREAM);
        cnet_observer observer;
        cnet_stream_peer peer;
        int status;
        if (!session) {
            return;
        }
        memset(&observer, 0, sizeof(observer));
        observer.on_state = turbo_rtsp_server_stream_state;
        observer.on_receive = turbo_rtsp_server_stream_receive;
        observer.on_send = turbo_rtsp_server_stream_send;
        observer.user = session;
        status = cnet_listener_accept_peer(
            &server->listener, &server->network, &observer,
            &session->connection, &peer);
        if (status != SALTS_OK) {
            turbo_rtsp_server_session_release(session, 0);
            return;
        }
        ready = 0;
    }
}

static void turbo_rtsp_server_worker(void *arg) {
    turbo_rtsp_server_t *server = (turbo_rtsp_server_t *)arg;
    int status = server->control_transport == TURBO_RTSP_CONTROL_TRANSPORT_KCP
                     ? turbo_rtsp_server_packet_init(server)
                     : turbo_rtsp_server_stream_init(server);

    salts_mutex_lock(&server->mutex);
    server->start_status = status;
    server->start_finished = 1;
    server->started = status == SALTS_OK;
    salts_cond_broadcast(&server->state_changed);
    salts_mutex_unlock(&server->mutex);

    while (status == SALTS_OK) {
        size_t events = 0;
        salts_mutex_lock(&server->mutex);
        if (server->stopping) {
            salts_mutex_unlock(&server->mutex);
            break;
        }
        salts_mutex_unlock(&server->mutex);
        (void)turbo_rtsp_server_drain_sends(server);
        if (server->control_transport == TURBO_RTSP_CONTROL_TRANSPORT_KCP) {
            status = cnet_packet_poll(
                &server->packet_endpoint, TURBO_RTSP_SERVER_POLL_SLICE_MS, &events);
        } else {
            turbo_rtsp_server_stream_accept(server);
            status = cnet_client_poll(
                &server->network, TURBO_RTSP_SERVER_POLL_SLICE_MS, &events);
        }
        if (status == SALTS_ESHUTDOWN) {
            status = SALTS_OK;
            break;
        }
    }

    if (server->listener_initialized) {
        (void)cnet_listener_close(&server->listener);
    }
    if (server->network_initialized) {
        (void)cnet_client_stop(&server->network, TURBO_RTSP_SERVER_STOP_TIMEOUT_MS);
        (void)cnet_client_destroy(&server->network);
        server->network_initialized = 0;
    }
    if (server->listener_initialized) {
        (void)cnet_listener_destroy(&server->listener);
        server->listener_initialized = 0;
    }
    if (server->packet_initialized) {
        (void)cnet_packet_endpoint_stop(
            &server->packet_endpoint, TURBO_RTSP_SERVER_STOP_TIMEOUT_MS);
        (void)cnet_packet_endpoint_destroy(&server->packet_endpoint);
        server->packet_initialized = 0;
    }
}

static int turbo_rtsp_server_http_init(turbo_rtsp_server_t *server) {
    chttp_server_config config = {0};
    chttp_server_websocket_options websocket = {0};
    size_t command_capacity;
    size_t event_capacity;
    uint16_t port = 0;

    if (server->connection_capacity > SIZE_MAX / 2u ||
        turbo_rtsp_next_power_of_two(
            server->connection_capacity * 2u, &command_capacity) != 0 ||
        turbo_rtsp_next_power_of_two(
            server->connection_capacity * 2u, &event_capacity) != 0) {
        return SALTS_ERANGE;
    }
    config.host = server->bind_host;
    config.port = (uint16_t)server->port;
    config.backlog = server->connection_capacity;
    config.network.backend = turbo_rtsp_backend();
    config.network.connection_capacity = server->connection_capacity;
    config.network.command_capacity = command_capacity;
    config.network.request_capacity = server->connection_capacity * 2u;
    config.network.completion_batch_capacity =
        config.network.request_capacity < 256u ? config.network.request_capacity : 256u;
    config.network.event_capacity = event_capacity;
    config.network.max_send_bytes =
        TURBO_RTSP_RESPONSE_BUFFER_SIZE + TURBO_RTSP_SERVER_WS_WIRE_OVERHEAD_BYTES;
    config.network.receive_buffer_bytes = TURBO_RTSP_SERVER_MAX_PENDING_BYTES;
    config.network.read_timeout_ms = (uint32_t)server->client_timeout_ms;
    config.network.write_timeout_ms = (uint32_t)server->client_timeout_ms;
    config.network.tls_io_buffer_bytes =
        turbo_rtsp_control_transport_is_tls(server->control_transport)
            ? CNET_TLS_MIN_IO_BUFFER_BYTES
            : 0u;
    config.network.tls_handshake_timeout_ms =
        turbo_rtsp_control_transport_is_tls(server->control_transport)
            ? (uint32_t)server->client_timeout_ms
            : 0u;
    config.route_capacity = 1u;
    config.max_target_bytes = TURBO_RTSP_SERVER_WS_TARGET_BYTES;
    config.max_header_count = TURBO_RTSP_SERVER_WS_HEADER_COUNT;
    config.max_header_bytes = TURBO_RTSP_SERVER_WS_HEADER_BYTES;
    config.max_request_body_bytes = TURBO_RTSP_SERVER_WS_BODY_BYTES;
    config.max_response_header_count = TURBO_RTSP_SERVER_WS_HEADER_COUNT;
    config.max_response_header_bytes = TURBO_RTSP_SERVER_WS_HEADER_BYTES;
    config.max_response_body_bytes = TURBO_RTSP_SERVER_WS_BODY_BYTES;
    config.poll_slice_ms = TURBO_RTSP_SERVER_POLL_SLICE_MS;
    config.tls = turbo_rtsp_control_transport_is_tls(server->control_transport)
                     ? &server->tls_config
                     : NULL;
    config.buffer_capacity_bytes = server->send_queue_bytes;

    if (chttp_server_init(&server->http, &config) != SALTS_OK) {
        return SALTS_EIO;
    }
    server->http_initialized = 1;
    websocket.size = sizeof(websocket);
    websocket.path = server->ws_path;
    websocket.max_frame_bytes = TURBO_RTSP_SERVER_MAX_PENDING_BYTES;
    websocket.max_message_bytes = TURBO_RTSP_SERVER_MAX_PENDING_BYTES;
    websocket.max_buffered_input_bytes =
        TURBO_RTSP_SERVER_MAX_PENDING_BYTES +
        TURBO_RTSP_SERVER_WS_WIRE_OVERHEAD_BYTES;
    websocket.on_open = turbo_rtsp_server_websocket_open;
    websocket.on_event = turbo_rtsp_server_websocket_event;
    websocket.user = server;
    if (chttp_server_websocket_with(&server->http, &websocket) != SALTS_OK ||
        chttp_server_start(&server->http) != SALTS_OK ||
        chttp_server_port(&server->http, &port) != SALTS_OK) {
        (void)chttp_server_destroy(&server->http);
        server->http_initialized = 0;
        return SALTS_EIO;
    }
    server->port = (int)port;
    return SALTS_OK;
}

turbo_rtsp_server_t *turbo_rtsp_server_create(
    const turbo_rtsp_server_config_t *config,
    const turbo_rtsp_server_handlers_t *handlers,
    void *user_data) {
    turbo_rtsp_server_t *server;
    turbo_rtsp_control_transport_t transport =
        config ? config->control_transport : TURBO_RTSP_CONTROL_TRANSPORT_TCP;
    size_t connection_capacity =
        config && config->connection_capacity
            ? config->connection_capacity
            : TURBO_RTSP_DEFAULT_CONNECTION_CAPACITY;
    size_t send_queue_capacity =
        config && config->send_queue_capacity
            ? config->send_queue_capacity
            : TURBO_RTSP_DEFAULT_SEND_QUEUE_CAPACITY;
    size_t send_queue_bytes =
        config && config->send_queue_bytes
            ? config->send_queue_bytes
            : TURBO_RTSP_DEFAULT_SEND_QUEUE_BYTES;

    if (transport < TURBO_RTSP_CONTROL_TRANSPORT_TCP ||
        transport > TURBO_RTSP_CONTROL_TRANSPORT_WSS ||
        connection_capacity == 0 || send_queue_capacity == 0 ||
        send_queue_bytes < TURBO_RTSP_SERVER_MAX_PENDING_BYTES ||
        (transport == TURBO_RTSP_CONTROL_TRANSPORT_KCP &&
         (!config || !turbo_rtsp_kcp_config_valid(config->kcp_config))) ||
        (transport == TURBO_RTSP_CONTROL_TRANSPORT_WSS &&
         (!config || !config->tls || !config->tls->cert_file ||
          !config->tls->key_file || config->tls->alpn_protocol_count != 0))) {
        return NULL;
    }
    server = (turbo_rtsp_server_t *)calloc(1, sizeof(*server));
    if (!server) {
        return NULL;
    }
    server->sessions = (turbo_rtsp_session_t *)calloc(
        connection_capacity, sizeof(*server->sessions));
    server->send_queue = (turbo_rtsp_server_send_t *)calloc(
        send_queue_capacity, sizeof(*server->send_queue));
    if (!server->sessions || !server->send_queue) {
        turbo_rtsp_server_destroy(server);
        return NULL;
    }

    server->user_data = user_data;
    server->port = config && config->port > 0 ? config->port : TURBO_RTSP_DEFAULT_PORT;
    server->control_transport = transport;
    server->connection_capacity = connection_capacity;
    server->send_queue_capacity = send_queue_capacity;
    server->send_queue_bytes = send_queue_bytes;
    server->client_timeout_ms =
        config && config->client_timeout_ms
            ? config->client_timeout_ms
            : TURBO_RTSP_DEFAULT_TIMEOUT_MS;
    if (server->client_timeout_ms > UINT32_MAX) {
        turbo_rtsp_server_destroy(server);
        return NULL;
    }
    turbo_rtsp_safe_copy(
        server->bind_host, sizeof(server->bind_host),
        config && config->bind_host ? config->bind_host : TURBO_RTSP_DEFAULT_HOST);
    turbo_rtsp_safe_copy(
        server->server_name, sizeof(server->server_name),
        config && config->server_name ? config->server_name : TURBO_RTSP_DEFAULT_SERVER_NAME);
    turbo_rtsp_safe_copy(
        server->public_methods, sizeof(server->public_methods),
        config && config->public_methods ? config->public_methods : TURBO_RTSP_DEFAULT_PUBLIC);
    turbo_rtsp_safe_copy(
        server->ws_path, sizeof(server->ws_path),
        config && config->ws_path ? config->ws_path : "/");
    turbo_rtsp_safe_copy(
        server->ws_subprotocol, sizeof(server->ws_subprotocol),
        config && config->ws_subprotocol ? config->ws_subprotocol : "");
    if (handlers) {
        server->handlers = *handlers;
    }
    if (transport == TURBO_RTSP_CONTROL_TRANSPORT_KCP) {
        server->kcp_config = *config->kcp_config;
    }
    if (transport == TURBO_RTSP_CONTROL_TRANSPORT_WSS) {
        server->tls_config = *config->tls;
        server->tls_cert_file = turbo_rtsp_strdup(config->tls->cert_file);
        server->tls_key_file = turbo_rtsp_strdup(config->tls->key_file);
        server->tls_key_password = turbo_rtsp_strdup(config->tls->key_password);
        server->tls_ca_file = turbo_rtsp_strdup(config->tls->ca_file);
        server->tls_ca_path = turbo_rtsp_strdup(config->tls->ca_path);
        if (!server->tls_cert_file || !server->tls_key_file) {
            turbo_rtsp_server_destroy(server);
            return NULL;
        }
        server->tls_config.cert_file = server->tls_cert_file;
        server->tls_config.key_file = server->tls_key_file;
        server->tls_config.key_password = server->tls_key_password;
        server->tls_config.ca_file = server->tls_ca_file;
        server->tls_config.ca_path = server->tls_ca_path;
    }
    salts_mutex_init(&server->mutex);
    salts_cond_init(&server->state_changed);
    server->sync_initialized = 1;
    return server;
}

int turbo_rtsp_server_start(turbo_rtsp_server_t *server) {
    int status;
    if (!server || !server->sync_initialized) {
        return -1;
    }
    salts_mutex_lock(&server->mutex);
    if (server->started) {
        salts_mutex_unlock(&server->mutex);
        return 0;
    }
    server->stopping = 0;
    server->start_finished = 0;
    server->start_status = SALTS_EIO;
    salts_mutex_unlock(&server->mutex);

    if (turbo_rtsp_control_transport_is_ws(server->control_transport)) {
        status = turbo_rtsp_server_http_init(server);
        salts_mutex_lock(&server->mutex);
        server->started = status == SALTS_OK;
        salts_mutex_unlock(&server->mutex);
        return status == SALTS_OK ? 0 : -1;
    }
    status = salts_thread_create(&server->worker, turbo_rtsp_server_worker, server);
    if (status != SALTS_OK) {
        return -1;
    }
    server->worker_started = 1;
    salts_mutex_lock(&server->mutex);
    while (!server->start_finished) {
        salts_cond_wait(&server->state_changed, &server->mutex);
    }
    status = server->start_status;
    salts_mutex_unlock(&server->mutex);
    if (status != SALTS_OK) {
        (void)salts_thread_join(&server->worker);
        salts_thread_destroy(&server->worker);
        server->worker_started = 0;
    }
    return status == SALTS_OK ? 0 : -1;
}

void turbo_rtsp_server_stop(turbo_rtsp_server_t *server) {
    if (!server || !server->sync_initialized) {
        return;
    }
    salts_mutex_lock(&server->mutex);
    server->stopping = 1;
    server->started = 0;
    salts_mutex_unlock(&server->mutex);
    if (server->network_initialized) {
        (void)cnet_client_wake(&server->network);
    }
    if (server->packet_initialized) {
        (void)cnet_packet_wake(&server->packet_endpoint);
    }
    if (server->worker_started) {
        (void)salts_thread_join(&server->worker);
        salts_thread_destroy(&server->worker);
        server->worker_started = 0;
    }
    if (server->http_initialized) {
        (void)chttp_server_stop(&server->http, TURBO_RTSP_SERVER_STOP_TIMEOUT_MS);
        (void)chttp_server_destroy(&server->http);
        server->http_initialized = 0;
    }
}

void turbo_rtsp_server_destroy(turbo_rtsp_server_t *server) {
    size_t i;
    if (!server) {
        return;
    }
    turbo_rtsp_server_stop(server);
    if (server->sessions) {
        for (i = 0; i < server->connection_capacity; ++i) {
            if (server->sessions[i].active) {
                turbo_rtsp_server_session_release(&server->sessions[i], 1);
            } else {
                free(server->sessions[i].pending);
            }
        }
    }
    if (server->send_queue) {
        for (i = 0; i < server->send_queue_capacity; ++i) {
            free(server->send_queue[i].data);
        }
    }
    if (server->sync_initialized) {
        salts_cond_destroy(&server->state_changed);
        salts_mutex_destroy(&server->mutex);
    }
    turbo_rtsp_kcp_config_wipe(&server->kcp_config);
    free(server->tls_cert_file);
    free(server->tls_key_file);
    free(server->tls_key_password);
    free(server->tls_ca_file);
    free(server->tls_ca_path);
    free(server->send_queue);
    free(server->sessions);
    free(server);
}

const turbo_rtsp_request_t *turbo_rtsp_session_get_last_request(
    const turbo_rtsp_session_t *session) {
    return session ? &session->last_request : NULL;
}

const turbo_rtsp_message_t *turbo_rtsp_session_get_last_message(
    const turbo_rtsp_session_t *session) {
    return session ? &session->last_message : NULL;
}

int turbo_rtsp_session_send_interleaved_frame(
    turbo_rtsp_session_t *session,
    uint8_t channel,
    const uint8_t *payload,
    size_t payload_len) {
    return turbo_rtsp_send_interleaved_frame(
        session,
        channel,
        payload,
        payload_len);
}

int turbo_rtsp_session_setup_udp_transport(
    turbo_rtsp_session_t *session,
    turbo_rtsp_response_t *response,
    const char *session_id,
    const char *local_host,
    const char *peer_host) {
    turbo_rtsp_rtp_udp_pair_config_t config;
    const turbo_rtsp_request_t *request = NULL;
    int server_rtp_port = 0;
    int server_rtcp_port = 0;
    const char *effective_peer_host = NULL;
    const char *mode_param = "";

    if (!session || !response || !session_id) {
        return -1;
    }

    request = &session->last_request;
    if (request->method != TURBO_RTSP_METHOD_SETUP ||
        request->transport_spec.kind != TURBO_RTSP_TRANSPORT_RTP_AVP_UDP ||
        request->transport_spec.delivery != TURBO_RTSP_TRANSPORT_DELIVERY_UNICAST ||
        request->transport_spec.client_rtp_port <= 0 ||
        request->transport_spec.client_rtcp_port <= 0) {
        return -1;
    }

    memset(&config, 0, sizeof(config));
    config.local_host = local_host ? local_host : session->server->bind_host;
    config.timeout_ms = session->server->client_timeout_ms;

    turbo_rtsp_session_clear_udp(session);
    session->udp_pair = turbo_rtsp_rtp_udp_pair_create(&config);
    if (!session->udp_pair ||
        turbo_rtsp_rtp_udp_pair_get_local_ports(
            session->udp_pair,
            &server_rtp_port,
            &server_rtcp_port) != 0) {
        turbo_rtsp_session_clear_udp(session);
        return -1;
    }

    effective_peer_host = peer_host && peer_host[0] != '\0'
                              ? peer_host
                              : request->transport_spec.destination;
    if (!effective_peer_host || effective_peer_host[0] == '\0') {
        turbo_rtsp_session_clear_udp(session);
        return -1;
    }

    if (turbo_rtsp_rtp_udp_pair_set_peer(
            session->udp_pair,
            effective_peer_host,
            request->transport_spec.client_rtp_port,
            request->transport_spec.client_rtcp_port) != 0) {
        turbo_rtsp_session_clear_udp(session);
        return -1;
    }

    if (request->transport_spec.mode == TURBO_RTSP_TRANSPORT_MODE_RECORD) {
        mode_param = ";mode=RECORD";
    } else if (request->transport_spec.mode == TURBO_RTSP_TRANSPORT_MODE_PLAY) {
        mode_param = ";mode=PLAY";
    }

    if (snprintf(
            session->udp_transport_header,
            sizeof(session->udp_transport_header),
            "RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d%s",
            request->transport_spec.client_rtp_port,
            request->transport_spec.client_rtcp_port,
            server_rtp_port,
            server_rtcp_port,
            mode_param) < 0) {
        turbo_rtsp_session_clear_udp(session);
        return -1;
    }

    if (turbo_rtsp_response_setup(response, session_id, session->udp_transport_header) != 0 ||
        turbo_rtsp_parse_transport(
            session->udp_transport_header,
            strlen(session->udp_transport_header),
            &session->udp_transport) != 0) {
        turbo_rtsp_session_clear_udp(session);
        return -1;
    }

    session->has_udp_transport = 1;
    return 0;
}

int turbo_rtsp_session_send_rtp_udp(
    turbo_rtsp_session_t *session,
    const uint8_t *packet,
    size_t packet_len) {
    if (!session || !session->udp_pair) {
        return -1;
    }
    return turbo_rtsp_rtp_udp_pair_send_rtp(session->udp_pair, packet, packet_len);
}

int turbo_rtsp_session_send_rtcp_udp(
    turbo_rtsp_session_t *session,
    const uint8_t *packet,
    size_t packet_len) {
    if (!session || !session->udp_pair) {
        return -1;
    }
    return turbo_rtsp_rtp_udp_pair_send_rtcp(session->udp_pair, packet, packet_len);
}

int turbo_rtsp_session_recv_rtp_udp(
    turbo_rtsp_session_t *session,
    uint8_t *buffer,
    size_t buffer_size,
    size_t *packet_len) {
    if (!session || !session->udp_pair) {
        return -1;
    }
    return turbo_rtsp_rtp_udp_pair_recv_rtp(session->udp_pair, buffer, buffer_size, packet_len);
}

int turbo_rtsp_session_recv_rtcp_udp(
    turbo_rtsp_session_t *session,
    uint8_t *buffer,
    size_t buffer_size,
    size_t *packet_len) {
    if (!session || !session->udp_pair) {
        return -1;
    }
    return turbo_rtsp_rtp_udp_pair_recv_rtcp(session->udp_pair, buffer, buffer_size, packet_len);
}

static int turbo_rtsp_client_copy_session(
    turbo_rtsp_client_t *client,
    const turbo_rtsp_response_view_t *response) {
    const turbo_rtsp_header_view_t *session = NULL;
    const char *value_end = NULL;
    const char *id_end = NULL;
    const char *param = NULL;
    size_t len = 0;

    if (!client || !response) {
        return -1;
    }

    session = turbo_rtsp_response_find_header(response, "Session");
    if (!session || !session->value || session->value_len == 0) {
        return 0;
    }

    value_end = session->value + session->value_len;
    id_end = memchr(session->value, ';', session->value_len);
    if (!id_end) {
        id_end = value_end;
    }

    while (id_end > session->value &&
           (id_end[-1] == ' ' || id_end[-1] == '\t')) {
        --id_end;
    }
    len = (size_t)(id_end - session->value);
    if (len >= sizeof(client->session_id)) {
        return -1;
    }

    memcpy(client->session_id, session->value, len);
    client->session_id[len] = '\0';
    memset(&client->session, 0, sizeof(client->session));
    turbo_rtsp_safe_copy(client->session.id, sizeof(client->session.id), client->session_id);

    param = id_end;
    while (param < value_end) {
        const char *param_end = NULL;

        while (param < value_end && (*param == ';' || *param == ' ' || *param == '\t')) {
            ++param;
        }
        if (param >= value_end) {
            break;
        }

        param_end = param;
        while (param_end < value_end && *param_end != ';') {
            ++param_end;
        }
        if ((size_t)(param_end - param) > strlen("timeout=") &&
            turbo_rtsp_ascii_ieq_n(param, "timeout=", strlen("timeout="))) {
            const char *start = param + strlen("timeout=");
            uint64_t timeout = 0;
            const char *cursor = start;
            while (cursor < param_end) {
                unsigned char c = (unsigned char)*cursor;
                if (c < '0' || c > '9' ||
                    timeout > ((uint64_t)INT_MAX - (uint64_t)(c - '0')) / 10u) {
                    return -1;
                }
                timeout = timeout * 10u + (uint64_t)(c - '0');
                ++cursor;
            }
            if (cursor == start) {
                return -1;
            }
            client->session.timeout_seconds = (int)timeout;
            client->session.has_timeout = 1;
        }
        param = param_end < value_end ? param_end + 1 : param_end;
    }
    return 0;
}

static void turbo_rtsp_client_clear_last_response(turbo_rtsp_client_t *client) {
    size_t i = 0;

    if (!client) {
        return;
    }

    free(client->last_body);
    client->last_body = NULL;
    client->last_body_len = 0;
    for (i = 0; i < TURBO_RTSP_MAX_MESSAGE_HEADERS; ++i) {
        free(client->last_header_names[i]);
        free(client->last_header_values[i]);
        client->last_header_names[i] = NULL;
        client->last_header_values[i] = NULL;
    }
    client->last_content_type[0] = '\0';
    memset(&client->last_response, 0, sizeof(client->last_response));
}

static char *turbo_rtsp_client_copy_view(
    const char *value,
    size_t value_len) {
    char *copy = NULL;

    if (!value && value_len > 0) {
        return NULL;
    }

    copy = (char *)malloc(value_len + 1);
    if (!copy) {
        return NULL;
    }
    if (value_len > 0) {
        memcpy(copy, value, value_len);
    }
    copy[value_len] = '\0';
    return copy;
}

static int turbo_rtsp_client_content_type_is_sdp(const turbo_rtsp_client_t *client) {
    const char *content_type = client ? client->last_response.content_type : NULL;

    if (!content_type) {
        return 0;
    }

    return strstr(content_type, "application/sdp") != NULL ||
           strstr(content_type, "Application/SDP") != NULL;
}

static int turbo_rtsp_client_response_header_to_buffer(
    const turbo_rtsp_client_response_t *response,
    const char *name,
    char *buffer,
    size_t buffer_size) {
    const turbo_rtsp_header_view_t *header = NULL;

    if (!response || !name || !buffer || buffer_size == 0) {
        return -1;
    }

    buffer[0] = '\0';
    header = turbo_rtsp_client_response_find_header(response, name);
    if (!header || !header->value || header->value_len == 0) {
        return 1;
    }
    if (header->value_len >= buffer_size) {
        return -1;
    }

    memcpy(buffer, header->value, header->value_len);
    buffer[header->value_len] = '\0';
    return 0;
}

static int turbo_rtsp_client_resolve_control_uri(
    const turbo_rtsp_client_t *client,
    const char *session_control,
    const char *media_control,
    char *uri,
    size_t uri_size) {
    char base[TURBO_RTSP_MAX_URI_LEN];
    const char *control = NULL;
    size_t base_len = 0;
    size_t control_len = 0;
    int header_rc = 0;

    if (!client || !uri || uri_size == 0) {
        return -1;
    }

    uri[0] = '\0';
    control = (media_control && media_control[0] != '\0')
                  ? media_control
                  : (session_control && session_control[0] != '\0' ? session_control : "");

    if (strcmp(control, "*") == 0) {
        control = "";
    }
    if (strncmp(control, "rtsp://", 7) == 0) {
        turbo_rtsp_safe_copy(uri, uri_size, control);
        return uri[0] != '\0' ? 0 : -1;
    }

    header_rc = turbo_rtsp_client_response_header_to_buffer(
        &client->last_response,
        "Content-Base",
        base,
        sizeof(base));
    if (header_rc < 0) {
        return -1;
    }
    if (header_rc > 0 || base[0] == '\0') {
        turbo_rtsp_safe_copy(base, sizeof(base), client->presentation_uri);
    }

    if (control[0] == '\0') {
        turbo_rtsp_safe_copy(uri, uri_size, base);
        return uri[0] != '\0' ? 0 : -1;
    }

    base_len = strlen(base);
    control_len = strlen(control);
    if (base_len == 0 || base_len + control_len + 2 > uri_size) {
        return -1;
    }

    memcpy(uri, base, base_len);
    if (base[base_len - 1] != '/' && control[0] != '/') {
        uri[base_len++] = '/';
    } else if (base[base_len - 1] == '/' && control[0] == '/') {
        ++control;
        --control_len;
    }
    memcpy(uri + base_len, control, control_len);
    uri[base_len + control_len] = '\0';
    return 0;
}

static int turbo_rtsp_client_store_sdp_tracks_from_body(
    turbo_rtsp_client_t *client,
    const char *sdp,
    size_t sdp_len) {
    turbo_rtsp_sdp_description_t description;
    size_t i = 0;

    if (!client) {
        return -1;
    }

    client->media_track_count = 0;
    memset(client->media_tracks, 0, sizeof(client->media_tracks));

    if (!sdp || sdp_len == 0) {
        return 0;
    }

    if (turbo_rtsp_sdp_parse(sdp, sdp_len, &description) != 0) {
        return -1;
    }

    for (i = 0; i < description.media_count && i < TURBO_RTSP_MAX_MEDIA_TRACKS; ++i) {
        const turbo_rtsp_sdp_media_desc_t *src = &description.media[i];
        turbo_rtsp_client_media_track_t *dst = &client->media_tracks[client->media_track_count];

        if (turbo_rtsp_client_resolve_control_uri(
                client,
                description.control,
                src->control,
                dst->control_uri,
                sizeof(dst->control_uri)) != 0) {
            return -1;
        }

        turbo_rtsp_safe_copy(dst->media, sizeof(dst->media), src->media);
        turbo_rtsp_safe_copy(dst->proto, sizeof(dst->proto), src->proto);
        turbo_rtsp_safe_copy(dst->encoding_name, sizeof(dst->encoding_name), src->encoding_name);
        turbo_rtsp_safe_copy(dst->fmtp, sizeof(dst->fmtp), src->fmtp);
        turbo_rtsp_safe_copy(
            dst->range,
            sizeof(dst->range),
            src->range[0] != '\0' ? src->range : description.range);
        turbo_rtsp_safe_copy(
            dst->direction,
            sizeof(dst->direction),
            src->direction[0] != '\0' ? src->direction : description.direction);
        turbo_rtsp_safe_copy(
            dst->connection_address,
            sizeof(dst->connection_address),
            src->connection_address[0] != '\0'
                ? src->connection_address
                : description.connection_address);
        dst->payload_type = src->payload_type;
        dst->clock_rate = src->clock_rate;
        ++client->media_track_count;
    }

    return 0;
}

static int turbo_rtsp_client_store_sdp_tracks(turbo_rtsp_client_t *client) {
    if (!client) {
        return -1;
    }
    if (!turbo_rtsp_client_content_type_is_sdp(client)) {
        client->media_track_count = 0;
        memset(client->media_tracks, 0, sizeof(client->media_tracks));
        return 0;
    }
    return turbo_rtsp_client_store_sdp_tracks_from_body(
        client,
        client->last_response.body,
        client->last_response.body_len);
}

static void turbo_rtsp_client_try_store_announced_sdp_tracks(
    turbo_rtsp_client_t *client,
    const turbo_rtsp_push_announce_t *announce) {
    size_t sdp_len = 0;

    if (!client || !announce || !announce->sdp) {
        return;
    }

    sdp_len = announce->sdp_len != 0 ? announce->sdp_len : strlen(announce->sdp);
    if (turbo_rtsp_client_store_sdp_tracks_from_body(client, announce->sdp, sdp_len) != 0) {
        client->media_track_count = 0;
        memset(client->media_tracks, 0, sizeof(client->media_tracks));
    }
}

static int turbo_rtsp_client_copy_last_response(
    turbo_rtsp_client_t *client,
    const turbo_rtsp_response_view_t *response) {
    const turbo_rtsp_header_view_t *content_type = NULL;
    size_t i = 0;

    if (!client || !response) {
        return -1;
    }

    turbo_rtsp_client_clear_last_response(client);
    client->last_response.status_code = response->status_code;

    if (response->header_count > TURBO_RTSP_MAX_MESSAGE_HEADERS) {
        return -1;
    }
    for (i = 0; i < response->header_count; ++i) {
        const turbo_rtsp_header_view_t *src = &response->headers[i];
        turbo_rtsp_header_view_t *dst = &client->last_response.headers[i];

        client->last_header_names[i] = turbo_rtsp_client_copy_view(src->name, src->name_len);
        client->last_header_values[i] = turbo_rtsp_client_copy_view(src->value, src->value_len);
        if (!client->last_header_names[i] || !client->last_header_values[i]) {
            return -1;
        }

        dst->name = client->last_header_names[i];
        dst->name_len = src->name_len;
        dst->value = client->last_header_values[i];
        dst->value_len = src->value_len;
    }
    client->last_response.header_count = response->header_count;

    content_type = turbo_rtsp_response_find_header(response, "Content-Type");
    if (content_type && content_type->value) {
        if (content_type->value_len >= sizeof(client->last_content_type)) {
            return -1;
        }
        memcpy(client->last_content_type, content_type->value, content_type->value_len);
        client->last_content_type[content_type->value_len] = '\0';
        client->last_response.content_type = client->last_content_type;
    }

    if (response->body_len > 0) {
        client->last_body = (char *)malloc(response->body_len + 1);
        if (!client->last_body) {
            return -1;
        }
        memcpy(client->last_body, response->body, response->body_len);
        client->last_body[response->body_len] = '\0';
        client->last_body_len = response->body_len;
        client->last_response.body = client->last_body;
        client->last_response.body_len = response->body_len;
    }

    return 0;
}

static int turbo_rtsp_client_parse_u32_view(
    const char *value,
    size_t value_len,
    uint32_t *parsed) {
    size_t i = 0;
    uint32_t result = 0;

    if (!value || value_len == 0 || !parsed) {
        return -1;
    }

    for (i = 0; i < value_len; ++i) {
        const unsigned char c = (unsigned char)value[i];
        const uint32_t digit = (uint32_t)(c - '0');

        if (c < '0' || c > '9') {
            return -1;
        }
        if (result > (UINT32_MAX - digit) / 10u) {
            return -1;
        }
        result = result * 10u + digit;
    }

    *parsed = result;
    return 0;
}

static int turbo_rtsp_client_response_matches_cseq(
    const turbo_rtsp_response_view_t *response,
    uint32_t expected_cseq) {
    const turbo_rtsp_header_view_t *cseq = NULL;
    uint32_t parsed = 0;

    if (!response || expected_cseq == 0) {
        return -1;
    }

    cseq = turbo_rtsp_response_find_header(response, "CSeq");
    if (!cseq ||
        turbo_rtsp_client_parse_u32_view(cseq->value, cseq->value_len, &parsed) != 0) {
        return -1;
    }

    return parsed == expected_cseq ? 0 : -1;
}

static int turbo_rtsp_client_auth_configured(const turbo_rtsp_client_t *client) {
    return client && client->auth_username[0] != '\0';
}

static int turbo_rtsp_md5_hex(const char *value, char hex[TURBO_RTSP_MD5_HEX_LEN + 1]) {
    uint8_t digest[TURBO_RTSP_MD5_DIGEST_LEN];
    unsigned int digest_len = 0;
    static const char digits[] = "0123456789abcdef";
    size_t i = 0;

    if (!value || !hex) {
        return -1;
    }

    if (EVP_Digest(
            value,
            strlen(value),
            digest,
            &digest_len,
            EVP_md5(),
            NULL) != 1 ||
        digest_len != TURBO_RTSP_MD5_DIGEST_LEN) {
        return -1;
    }

    for (i = 0; i < TURBO_RTSP_MD5_DIGEST_LEN; ++i) {
        hex[i * 2] = digits[(digest[i] >> 4) & 0x0f];
        hex[i * 2 + 1] = digits[digest[i] & 0x0f];
    }
    hex[TURBO_RTSP_MD5_HEX_LEN] = '\0';
    return 0;
}

static int turbo_rtsp_client_response_find_auth_challenge(
    const turbo_rtsp_client_response_t *response,
    const char *scheme,
    const char **challenge,
    size_t *challenge_len) {
    size_t i = 0;
    const size_t scheme_len = scheme ? strlen(scheme) : 0;

    if (!response || !scheme || scheme_len == 0 || !challenge || !challenge_len) {
        return 0;
    }

    *challenge = NULL;
    *challenge_len = 0;

    for (i = 0; i < response->header_count; ++i) {
        const turbo_rtsp_header_view_t *header = &response->headers[i];
        const char *cursor = NULL;
        const char *end = NULL;

        if (!header->name || !header->value ||
            header->name_len != strlen("WWW-Authenticate") ||
            !turbo_rtsp_ascii_ieq_n(
                header->name,
                "WWW-Authenticate",
                strlen("WWW-Authenticate")) ||
            header->value_len == 0) {
            continue;
        }

        cursor = header->value;
        end = header->value + header->value_len;
        while (cursor < end) {
            while (cursor < end &&
                   (*cursor == ' ' || *cursor == '\t' || *cursor == ',')) {
                ++cursor;
            }
            if ((size_t)(end - cursor) >= scheme_len &&
                turbo_rtsp_ascii_ieq_n(cursor, scheme, scheme_len)) {
                const char *after = cursor + scheme_len;
                if (after == end || *after == ' ' || *after == '\t' || *after == ',') {
                    while (after < end && (*after == ' ' || *after == '\t')) {
                        ++after;
                    }
                    *challenge = after;
                    *challenge_len = (size_t)(end - after);
                    return 1;
                }
            }
            while (cursor < end && *cursor != ',') {
                ++cursor;
            }
        }
    }

    return 0;
}

static int turbo_rtsp_client_response_has_basic_challenge(
    const turbo_rtsp_client_response_t *response) {
    const char *challenge = NULL;
    size_t challenge_len = 0;

    return turbo_rtsp_client_response_find_auth_challenge(
        response,
        "Basic",
        &challenge,
        &challenge_len);
}

static int turbo_rtsp_digest_qop_has_auth(const char *value) {
    const char *cursor = value;

    if (!value) {
        return 0;
    }

    while (*cursor != '\0') {
        const char *start = cursor;
        size_t len = 0;

        while (*start == ' ' || *start == '\t' || *start == ',') {
            ++start;
        }
        cursor = start;
        while (*cursor != '\0' && *cursor != ',') {
            ++cursor;
        }
        len = (size_t)(cursor - start);
        while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t')) {
            --len;
        }
        if (len == strlen("auth") && turbo_rtsp_ascii_ieq_n(start, "auth", len)) {
            return 1;
        }
    }

    return 0;
}

static int turbo_rtsp_digest_copy_param(
    char *dst,
    size_t dst_size,
    const char *value,
    size_t value_len) {
    if (!dst || dst_size == 0 || !value || value_len >= dst_size) {
        return -1;
    }

    memcpy(dst, value, value_len);
    dst[value_len] = '\0';
    return 0;
}

static int turbo_rtsp_client_parse_digest_challenge(
    const turbo_rtsp_client_response_t *response,
    turbo_rtsp_digest_challenge_t *challenge) {
    const char *cursor = NULL;
    const char *end = NULL;
    size_t challenge_len = 0;

    if (!challenge ||
        !turbo_rtsp_client_response_find_auth_challenge(
            response,
            "Digest",
            &cursor,
            &challenge_len)) {
        return -1;
    }

    memset(challenge, 0, sizeof(*challenge));
    end = cursor + challenge_len;

    while (cursor < end) {
        const char *name_start = NULL;
        const char *name_end = NULL;
        const char *value_start = NULL;
        char value[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
        size_t value_len = 0;

        while (cursor < end &&
               (*cursor == ' ' || *cursor == '\t' || *cursor == ',')) {
            ++cursor;
        }
        if (cursor >= end) {
            break;
        }

        name_start = cursor;
        while (cursor < end &&
               *cursor != '=' &&
               *cursor != ',' &&
               *cursor != ' ' &&
               *cursor != '\t') {
            ++cursor;
        }
        name_end = cursor;
        while (cursor < end && (*cursor == ' ' || *cursor == '\t')) {
            ++cursor;
        }
        if (cursor >= end || *cursor != '=') {
            while (cursor < end && *cursor != ',') {
                ++cursor;
            }
            continue;
        }
        ++cursor;
        while (cursor < end && (*cursor == ' ' || *cursor == '\t')) {
            ++cursor;
        }

        if (cursor < end && *cursor == '"') {
            ++cursor;
            while (cursor < end && *cursor != '"') {
                if (*cursor == '\\' && cursor + 1 < end) {
                    ++cursor;
                }
                if (value_len + 1 >= sizeof(value)) {
                    return -1;
                }
                value[value_len++] = *cursor++;
            }
            if (cursor >= end || *cursor != '"') {
                return -1;
            }
            ++cursor;
        } else {
            value_start = cursor;
            while (cursor < end && *cursor != ',' && *cursor != ' ' && *cursor != '\t') {
                ++cursor;
            }
            value_len = (size_t)(cursor - value_start);
            if (value_len >= sizeof(value)) {
                return -1;
            }
            memcpy(value, value_start, value_len);
        }
        value[value_len] = '\0';

        if ((size_t)(name_end - name_start) == strlen("realm") &&
            turbo_rtsp_ascii_ieq_n(name_start, "realm", strlen("realm"))) {
            if (turbo_rtsp_digest_copy_param(
                    challenge->realm,
                    sizeof(challenge->realm),
                    value,
                    value_len) != 0) {
                return -1;
            }
        } else if ((size_t)(name_end - name_start) == strlen("nonce") &&
                   turbo_rtsp_ascii_ieq_n(name_start, "nonce", strlen("nonce"))) {
            if (turbo_rtsp_digest_copy_param(
                    challenge->nonce,
                    sizeof(challenge->nonce),
                    value,
                    value_len) != 0) {
                return -1;
            }
        } else if ((size_t)(name_end - name_start) == strlen("opaque") &&
                   turbo_rtsp_ascii_ieq_n(name_start, "opaque", strlen("opaque"))) {
            if (turbo_rtsp_digest_copy_param(
                    challenge->opaque,
                    sizeof(challenge->opaque),
                    value,
                    value_len) != 0) {
                return -1;
            }
        } else if ((size_t)(name_end - name_start) == strlen("algorithm") &&
                   turbo_rtsp_ascii_ieq_n(name_start, "algorithm", strlen("algorithm"))) {
            if (turbo_rtsp_digest_copy_param(
                    challenge->algorithm,
                    sizeof(challenge->algorithm),
                    value,
                    value_len) != 0) {
                return -1;
            }
        } else if ((size_t)(name_end - name_start) == strlen("qop") &&
                   turbo_rtsp_ascii_ieq_n(name_start, "qop", strlen("qop"))) {
            challenge->has_qop = 1;
            challenge->qop_auth = turbo_rtsp_digest_qop_has_auth(value);
        }

        while (cursor < end && *cursor != ',') {
            ++cursor;
        }
    }

    if (challenge->realm[0] == '\0' ||
        challenge->nonce[0] == '\0' ||
        (challenge->algorithm[0] != '\0' &&
         (strlen(challenge->algorithm) != strlen("MD5") ||
          !turbo_rtsp_ascii_ieq_n(challenge->algorithm, "MD5", strlen("MD5")))) ||
        (challenge->has_qop && !challenge->qop_auth)) {
        return -1;
    }

    return 0;
}

static int turbo_rtsp_client_build_basic_auth(
    const turbo_rtsp_client_t *client,
    char *value,
    size_t value_size) {
    char userpass[(TURBO_RTSP_MAX_HEADER_VALUE_LEN * 2) + 2];
    char encoded[TURBO_RTSP_AUTH_VALUE_BUFFER_SIZE];
    size_t username_len = 0;
    size_t password_len = 0;
    size_t userpass_len = 0;
    int len = 0;

    if (!turbo_rtsp_client_auth_configured(client) || !value || value_size == 0) {
        return -1;
    }

    username_len = strlen(client->auth_username);
    password_len = strlen(client->auth_password);
    if (username_len + 1 + password_len >= sizeof(userpass)) {
        return -1;
    }

    memcpy(userpass, client->auth_username, username_len);
    userpass[username_len] = ':';
    memcpy(userpass + username_len + 1, client->auth_password, password_len);
    userpass_len = username_len + 1 + password_len;

    if (tn_base64_encode_buf(
            (const uint8_t *)userpass,
            userpass_len,
            encoded,
            sizeof(encoded)) != 0) {
        return -1;
    }

    len = snprintf(value, value_size, "Basic %s", encoded);
    if (len < 0 || (size_t)len >= value_size) {
        return -1;
    }
    return 0;
}

static int turbo_rtsp_append_digest_part(
    char *value,
    size_t value_size,
    size_t *offset,
    const char *format,
    const char *part) {
    int len = 0;

    if (!value || !offset || !format || !part || *offset >= value_size) {
        return -1;
    }

    len = snprintf(value + *offset, value_size - *offset, format, part);
    if (len < 0 || (size_t)len >= value_size - *offset) {
        return -1;
    }
    *offset += (size_t)len;
    return 0;
}

static int turbo_rtsp_client_build_digest_auth(
    turbo_rtsp_client_t *client,
    turbo_rtsp_method_t method,
    const char *uri,
    const turbo_rtsp_digest_challenge_t *challenge,
    char *value,
    size_t value_size) {
    const char *method_name = turbo_rtsp_client_method_name(method);
    unsigned char cnonce_bytes[TURBO_RTSP_DIGEST_CNONCE_BYTES];
    char nc[9];
    char ha1_input[TURBO_RTSP_AUTH_VALUE_BUFFER_SIZE];
    char ha2_input[TURBO_RTSP_AUTH_VALUE_BUFFER_SIZE];
    char response_input[TURBO_RTSP_AUTH_VALUE_BUFFER_SIZE];
    char ha1[TURBO_RTSP_MD5_HEX_LEN + 1];
    char ha2[TURBO_RTSP_MD5_HEX_LEN + 1];
    char response[TURBO_RTSP_MD5_HEX_LEN + 1];
    char cnonce[TURBO_RTSP_MD5_HEX_LEN + 1];
    size_t offset = 0;
    size_t i = 0;
    int len = 0;

    if (!turbo_rtsp_client_auth_configured(client) ||
        !method_name ||
        !uri ||
        !challenge ||
        !value ||
        value_size == 0) {
        return -1;
    }

    if (strcmp(client->digest_nonce, challenge->nonce) != 0) {
        turbo_rtsp_safe_copy(
            client->digest_nonce,
            sizeof(client->digest_nonce),
            challenge->nonce);
        client->digest_nonce_count = 0;
    }
    if (client->digest_nonce_count == UINT32_MAX) {
        return -1;
    }
    client->digest_nonce_count++;
    len = snprintf(nc, sizeof(nc), "%08x", client->digest_nonce_count);
    if (len != 8) {
        return -1;
    }

    len = snprintf(
        ha1_input,
        sizeof(ha1_input),
        "%s:%s:%s",
        client->auth_username,
        challenge->realm,
        client->auth_password);
    if (len < 0 || (size_t)len >= sizeof(ha1_input) ||
        turbo_rtsp_md5_hex(ha1_input, ha1) != 0) {
        return -1;
    }

    len = snprintf(ha2_input, sizeof(ha2_input), "%s:%s", method_name, uri);
    if (len < 0 || (size_t)len >= sizeof(ha2_input) ||
        turbo_rtsp_md5_hex(ha2_input, ha2) != 0) {
        return -1;
    }

    if (salts_secure_random(cnonce_bytes, sizeof(cnonce_bytes)) != 0) {
        return -1;
    }
    for (i = 0; i < sizeof(cnonce_bytes); ++i) {
        static const char digits[] = "0123456789abcdef";
        cnonce[i * 2u] = digits[(cnonce_bytes[i] >> 4) & 0x0f];
        cnonce[i * 2u + 1u] = digits[cnonce_bytes[i] & 0x0f];
    }
    cnonce[sizeof(cnonce_bytes) * 2u] = '\0';

    if (challenge->has_qop) {
        len = snprintf(
            response_input,
            sizeof(response_input),
            "%s:%s:%s:%s:auth:%s",
            ha1,
            challenge->nonce,
            nc,
            cnonce,
            ha2);
    } else {
        len = snprintf(
            response_input,
            sizeof(response_input),
            "%s:%s:%s",
            ha1,
            challenge->nonce,
            ha2);
    }
    if (len < 0 || (size_t)len >= sizeof(response_input) ||
        turbo_rtsp_md5_hex(response_input, response) != 0) {
        return -1;
    }

    len = snprintf(value, value_size, "Digest ");
    if (len < 0 || (size_t)len >= value_size) {
        return -1;
    }
    offset = (size_t)len;

    if (turbo_rtsp_append_digest_part(value, value_size, &offset, "username=\"%s\"", client->auth_username) != 0 ||
        turbo_rtsp_append_digest_part(value, value_size, &offset, ", realm=\"%s\"", challenge->realm) != 0 ||
        turbo_rtsp_append_digest_part(value, value_size, &offset, ", nonce=\"%s\"", challenge->nonce) != 0 ||
        turbo_rtsp_append_digest_part(value, value_size, &offset, ", uri=\"%s\"", uri) != 0 ||
        turbo_rtsp_append_digest_part(value, value_size, &offset, ", response=\"%s\"", response) != 0 ||
        turbo_rtsp_append_digest_part(value, value_size, &offset, ", algorithm=%s", "MD5") != 0) {
        return -1;
    }

    if (challenge->has_qop &&
        (turbo_rtsp_append_digest_part(value, value_size, &offset, ", qop=%s", "auth") != 0 ||
         turbo_rtsp_append_digest_part(value, value_size, &offset, ", nc=%s", nc) != 0 ||
         turbo_rtsp_append_digest_part(value, value_size, &offset, ", cnonce=\"%s\"", cnonce) != 0)) {
        return -1;
    }

    if (challenge->opaque[0] != '\0' &&
        turbo_rtsp_append_digest_part(value, value_size, &offset, ", opaque=\"%s\"", challenge->opaque) != 0) {
        return -1;
    }

    return 0;
}

static cnet_client_config turbo_rtsp_client_network_config(
    const turbo_rtsp_client_t *client,
    int tls) {
    cnet_client_config config;
    memset(&config, 0, sizeof(config));
    config.backend = turbo_rtsp_backend();
    config.connection_capacity = 1u;
    config.command_capacity = TURBO_RTSP_SERVER_MIN_QUEUE_CAPACITY;
    config.request_capacity = TURBO_RTSP_SERVER_MIN_QUEUE_CAPACITY;
    config.completion_batch_capacity = TURBO_RTSP_SERVER_MIN_QUEUE_CAPACITY;
    config.event_capacity = TURBO_RTSP_SERVER_MIN_QUEUE_CAPACITY;
    config.max_send_bytes = tls
                                ? TURBO_RTSP_SERVER_MAX_PENDING_BYTES +
                                      TURBO_RTSP_SERVER_WS_WIRE_OVERHEAD_BYTES
                                : TURBO_RTSP_SERVER_MAX_PENDING_BYTES;
    config.receive_buffer_bytes = TURBO_RTSP_CLIENT_RECV_BUFFER_SIZE;
    config.connect_timeout_ms = (uint32_t)client->timeout_ms;
    config.read_timeout_ms = (uint32_t)client->timeout_ms;
    config.write_timeout_ms = (uint32_t)client->timeout_ms;
    config.tls_io_buffer_bytes = tls ? CNET_TLS_MIN_IO_BUFFER_BYTES : 0u;
    config.tls_handshake_timeout_ms = tls ? (uint32_t)client->timeout_ms : 0u;
    return config;
}

static void turbo_rtsp_client_store_received(
    turbo_rtsp_client_t *client,
    const cnet_receive_view *view) {
    if (!client || !view ||
        view->size > sizeof(client->recv_buffer) - client->recv_len) {
        if (client) {
            client->receive_status = SALTS_EMSGSIZE;
            client->receive_finished = 1;
        }
        return;
    }
    if (view->size > 0) {
        memcpy(client->recv_buffer + client->recv_len, view->data, view->size);
        client->recv_len += view->size;
    }
    client->receive_status = SALTS_OK;
    client->receive_finished = 1;
}

static void turbo_rtsp_client_stream_state(
    void *user,
    cnet_connection connection,
    cnet_connection_state state,
    const cnet_error *error) {
    turbo_rtsp_client_t *client = (turbo_rtsp_client_t *)user;
    (void)connection;
    if (!client) {
        return;
    }
    if (state == CNET_CONNECTION_CONNECTED) {
        client->connect_status = SALTS_OK;
        client->connect_finished = 1;
    } else if (state == CNET_CONNECTION_FAILED || state == CNET_CONNECTION_CLOSED) {
        client->connect_status = error ? error->status : SALTS_EIO;
        client->connect_finished = 1;
        client->connected = 0;
    }
}

static void turbo_rtsp_client_stream_receive(
    void *user,
    cnet_connection connection,
    const cnet_receive_view *view) {
    (void)connection;
    turbo_rtsp_client_store_received((turbo_rtsp_client_t *)user, view);
}

static void turbo_rtsp_client_stream_send(
    void *user,
    cnet_connection connection,
    size_t size) {
    turbo_rtsp_client_t *client = (turbo_rtsp_client_t *)user;
    (void)connection;
    (void)size;
    if (client) {
        client->send_finished = 1;
    }
}

static void turbo_rtsp_client_packet_state(
    void *user,
    cnet_packet_endpoint *endpoint,
    cnet_packet_session packet,
    cnet_packet_session_state state,
    const cnet_datagram_peer *peer,
    uint32_t conversation) {
    turbo_rtsp_client_t *client = (turbo_rtsp_client_t *)user;
    (void)endpoint;
    (void)peer;
    (void)conversation;
    if (!client || client->packet_session.slot != packet.slot ||
        client->packet_session.generation != packet.generation) {
        return;
    }
    if (state == CNET_PACKET_SESSION_OPEN) {
        client->connect_status = SALTS_OK;
        client->connect_finished = 1;
    } else if (state == CNET_PACKET_SESSION_CLOSED) {
        client->connect_status = SALTS_EIO;
        client->connect_finished = 1;
        client->connected = 0;
    }
}

static void turbo_rtsp_client_packet_receive(
    void *user,
    cnet_packet_endpoint *endpoint,
    cnet_packet_session packet,
    const cnet_receive_view *view) {
    turbo_rtsp_client_t *client = (turbo_rtsp_client_t *)user;
    (void)endpoint;
    if (client && client->packet_session.slot == packet.slot &&
        client->packet_session.generation == packet.generation) {
        turbo_rtsp_client_store_received(client, view);
    }
}

static void turbo_rtsp_client_packet_error(
    void *user,
    cnet_packet_endpoint *endpoint,
    cnet_packet_session packet,
    int status) {
    turbo_rtsp_client_t *client = (turbo_rtsp_client_t *)user;
    (void)endpoint;
    (void)packet;
    if (client) {
        client->receive_status = status;
        client->receive_finished = 1;
        client->connect_status = status;
        client->connect_finished = 1;
    }
}

static int turbo_rtsp_client_poll_until(
    turbo_rtsp_client_t *client,
    int *finished) {
    const uint64_t started_ms = salts_monotonic_ms();
    int status = SALTS_OK;
    while (!*finished) {
        uint64_t elapsed_ms = salts_monotonic_ms() - started_ms;
        uint32_t wait_ms;
        size_t events = 0;
        if (elapsed_ms >= client->timeout_ms) {
            return SALTS_ETIMEDOUT;
        }
        wait_ms = (uint32_t)(client->timeout_ms - elapsed_ms);
        status = client->packet_initialized
                     ? cnet_packet_poll(&client->packet_endpoint, wait_ms, &events)
                     : cnet_client_poll(&client->network, wait_ms, &events);
        if (status != SALTS_OK) {
            return status;
        }
    }
    return SALTS_OK;
}

static int turbo_rtsp_client_recv_more(turbo_rtsp_client_t *client) {
    int status;
    if (!client || !client->connected ||
        client->recv_len == sizeof(client->recv_buffer)) {
        return -1;
    }
    if (client->websocket_initialized) {
        for (;;) {
            chttp_websocket_event event;
            memset(&event, 0, sizeof(event));
            status = chttp_websocket_client_receive(
                &client->websocket, (uint32_t)client->timeout_ms, &event);
            if (status != SALTS_OK) {
                return -1;
            }
            if (event.kind == CHTTP_WEBSOCKET_EVENT_CLOSE) {
                client->connected = 0;
                return -1;
            }
            if (event.kind == CHTTP_WEBSOCKET_EVENT_MESSAGE) {
                cnet_receive_view view;
                view.data = event.data;
                view.size = event.size;
                view.kind = CNET_MESSAGE_BYTES;
                turbo_rtsp_client_store_received(client, &view);
                return client->receive_status == SALTS_OK ? 0 : -1;
            }
        }
    }

    client->receive_finished = 0;
    client->receive_status = SALTS_EIO;
    status = client->packet_initialized
                 ? turbo_rtsp_client_poll_until(client, &client->receive_finished)
                 : cnet_receive(&client->network, client->connection, 1u);
    if (!client->packet_initialized && status == SALTS_OK) {
        status = turbo_rtsp_client_poll_until(client, &client->receive_finished);
    }
    return status == SALTS_OK && client->receive_status == SALTS_OK ? 0 : -1;
}

static int turbo_rtsp_client_send_bytes(
    turbo_rtsp_client_t *client,
    const void *data,
    size_t size) {
    int status;
    if (!client || !client->connected || !data || size == 0) {
        return -1;
    }
    if (client->websocket_initialized) {
        status = chttp_websocket_client_send_binary(
            &client->websocket, data, size, (uint32_t)client->timeout_ms);
        return status == SALTS_OK ? 0 : -1;
    }
    if (client->packet_initialized) {
        status = cnet_packet_send(
            &client->packet_endpoint, client->packet_session, data, size);
        return status == SALTS_OK ? 0 : -1;
    }
    client->send_finished = 0;
    status = cnet_send(&client->network, client->connection, data, size);
    if (status == SALTS_OK) {
        status = turbo_rtsp_client_poll_until(client, &client->send_finished);
    }
    return status == SALTS_OK ? 0 : -1;
}

static int turbo_rtsp_client_frame_queue_init(turbo_rtsp_client_t *client) {
    disruptor_config_t config;

    if (!client) {
        return -1;
    }

    memset(&config, 0, sizeof(config));
    config.entry_size = sizeof(turbo_rtsp_client_frame_event_t);
    config.capacity = TURBO_RTSP_CLIENT_FRAME_QUEUE_CAPACITY;
    config.consumer_capacity = 1;

    client->frame_queue = disruptor_create(&config);
    if (!client->frame_queue) {
        return -1;
    }
    if (!disruptor_consumer_try_register(
            client->frame_queue,
            &client->frame_consumer,
            &client->frame_next_sequence)) {
        disruptor_destroy(client->frame_queue);
        client->frame_queue = NULL;
        return -1;
    }

    client->frame_consumer_registered = 1;
    return 0;
}

static int turbo_rtsp_client_frame_queue_push(
    turbo_rtsp_client_t *client,
    uint8_t channel,
    const uint8_t *payload,
    size_t payload_len) {
    disruptor_cursor_t cursor;
    turbo_rtsp_client_frame_event_t *event = NULL;
    uint8_t *copy = NULL;

    if (!client || !client->frame_queue || (!payload && payload_len > 0)) {
        return -1;
    }

    if (payload_len > 0) {
        copy = (uint8_t *)malloc(payload_len);
        if (!copy) {
            return -1;
        }
        memcpy(copy, payload, payload_len);
    }

    if (!disruptor_publisher_try_claim(client->frame_queue, &cursor)) {
        free(copy);
        return -1;
    }

    event = (turbo_rtsp_client_frame_event_t *)disruptor_acquire_entry(
        client->frame_queue,
        &cursor);
    if (!event) {
        free(copy);
        return -1;
    }

    event->channel = channel;
    event->payload = copy;
    event->payload_len = payload_len;

    if (!disruptor_publisher_publish(client->frame_queue, &cursor)) {
        free(copy);
        event->payload = NULL;
        event->payload_len = 0;
        return -1;
    }

    return 0;
}

static int turbo_rtsp_client_frame_queue_pop(
    turbo_rtsp_client_t *client,
    uint8_t *channel,
    uint8_t *payload,
    size_t payload_capacity,
    size_t *payload_len) {
    disruptor_cursor_t available_cursor;
    disruptor_cursor_t read_cursor;
    turbo_rtsp_client_frame_event_t *event = NULL;

    if (!client || !client->frame_queue || !channel || !payload_len) {
        return 0;
    }

    available_cursor.sequence = client->frame_next_sequence;
    if (!disruptor_consumer_wait_for_nonblocking(client->frame_queue, &available_cursor)) {
        return 0;
    }

    read_cursor.sequence = client->frame_next_sequence;
    event = (turbo_rtsp_client_frame_event_t *)disruptor_acquire_entry(
        client->frame_queue,
        &read_cursor);
    if (!event) {
        return -1;
    }

    if (event->payload_len > payload_capacity) {
        *payload_len = event->payload_len;
        return -1;
    }

    if (event->payload_len > 0) {
        memcpy(payload, event->payload, event->payload_len);
    }
    *channel = event->channel;
    *payload_len = event->payload_len;

    free(event->payload);
    event->payload = NULL;
    event->payload_len = 0;
    disruptor_consumer_release_entry(
        client->frame_queue,
        &client->frame_consumer,
        &read_cursor);
    ++client->frame_next_sequence;
    return 1;
}

static void turbo_rtsp_client_frame_queue_clear(turbo_rtsp_client_t *client) {
    if (!client || !client->frame_queue) {
        return;
    }

    for (;;) {
        disruptor_cursor_t available_cursor;
        disruptor_cursor_t read_cursor;
        turbo_rtsp_client_frame_event_t *event = NULL;

        available_cursor.sequence = client->frame_next_sequence;
        if (!disruptor_consumer_wait_for_nonblocking(client->frame_queue, &available_cursor)) {
            break;
        }

        read_cursor.sequence = client->frame_next_sequence;
        event = (turbo_rtsp_client_frame_event_t *)disruptor_acquire_entry(
            client->frame_queue,
            &read_cursor);
        if (event) {
            free(event->payload);
            event->payload = NULL;
            event->payload_len = 0;
        }
        disruptor_consumer_release_entry(
            client->frame_queue,
            &client->frame_consumer,
            &read_cursor);
        ++client->frame_next_sequence;
    }
}

static void turbo_rtsp_client_frame_queue_destroy(turbo_rtsp_client_t *client) {
    if (!client || !client->frame_queue) {
        return;
    }

    turbo_rtsp_client_frame_queue_clear(client);
    if (client->frame_consumer_registered) {
        disruptor_consumer_unregister(client->frame_queue, &client->frame_consumer);
        client->frame_consumer_registered = 0;
    }
    disruptor_destroy(client->frame_queue);
    client->frame_queue = NULL;
    client->frame_next_sequence = 0;
}

static int turbo_rtsp_client_recv_response(
    turbo_rtsp_client_t *client,
    uint32_t expected_cseq) {
    for (;;) {
        turbo_rtsp_response_view_t response;
        size_t consumed = 0;
        int rc = 0;

        if (client->recv_len > 0 && (unsigned char)client->recv_buffer[0] == '$') {
            turbo_rtsp_interleaved_frame_t frame;
            rc = turbo_rtsp_interleaved_parse(
                (const uint8_t *)client->recv_buffer,
                client->recv_len,
                &frame,
                &consumed);
            if (rc == TURBO_RTSP_FRAME_OK) {
                const size_t remaining = client->recv_len - consumed;
                if (turbo_rtsp_client_frame_queue_push(
                        client,
                        frame.channel,
                        frame.payload,
                        frame.payload_len) != 0) {
                    return -1;
                }
                if (remaining > 0) {
                    memmove(client->recv_buffer, client->recv_buffer + consumed, remaining);
                }
                client->recv_len = remaining;
                continue;
            }
            if (rc != TURBO_RTSP_FRAME_PARTIAL) {
                return -1;
            }
        } else {
            rc = turbo_rtsp_parse_response(
                client->recv_buffer,
                client->recv_len,
                &consumed,
                &response);
            if (rc == TURBO_RTSP_PARSE_OK) {
                const int success = response.status_code >= 200 && response.status_code < 300;
                const size_t remaining = client->recv_len - consumed;

                if (turbo_rtsp_client_response_matches_cseq(&response, expected_cseq) != 0 ||
                    turbo_rtsp_client_copy_last_response(client, &response) != 0) {
                    return -1;
                }
                if (success && turbo_rtsp_client_copy_session(client, &response) != 0) {
                    return -1;
                }
                if (remaining > 0) {
                    memmove(client->recv_buffer, client->recv_buffer + consumed, remaining);
                }
                client->recv_len = remaining;
                return success ? 0 : -1;
            }
            if (rc != TURBO_RTSP_PARSE_PARTIAL) {
                return -1;
            }
        }
        if (client->recv_len == sizeof(client->recv_buffer)) {
            return -1;
        }

        if (turbo_rtsp_client_recv_more(client) != 0) {
            return -1;
        }
    }
}

static int turbo_rtsp_client_send_request_once(
    turbo_rtsp_client_t *client,
    turbo_rtsp_method_t method,
    const char *uri,
    const char *transport,
    const char *content_type,
    const char *range,
    const turbo_rtsp_header_t *headers,
    size_t header_count,
    const char *body,
    size_t body_len,
    uint32_t *sent_cseq) {
    turbo_rtsp_request_message_t request;
    char *buffer = NULL;
    uint32_t cseq = 0;
    int len = 0;
    int rc = 0;

    if (!client || !client->connected || !uri || uri[0] == '\0' ||
        (header_count > 0 && !headers)) {
        return -1;
    }

    turbo_rtsp_client_clear_last_response(client);

    buffer = (char *)malloc(TURBO_RTSP_REQUEST_BUFFER_SIZE);
    if (!buffer) {
        return -1;
    }

    memset(&request, 0, sizeof(request));
    request.method = method;
    request.uri = uri;
    cseq = client->next_cseq++;
    request.cseq = cseq;
    if (sent_cseq) {
        *sent_cseq = cseq;
    }
    request.user_agent = client->user_agent;
    request.session_id = client->session_id[0] != '\0' ? client->session_id : NULL;
    request.transport = transport;
    request.content_type = content_type;
    request.range = range;
    request.headers = headers;
    request.header_count = header_count;
    request.body = body;
    request.body_len = body_len;

    len = turbo_rtsp_format_request(buffer, TURBO_RTSP_REQUEST_BUFFER_SIZE, &request);
    if (len < 0) {
        free(buffer);
        return -1;
    }

    rc = turbo_rtsp_client_send_bytes(client, buffer, (size_t)len);
    free(buffer);
    if (rc != 0) {
        return -1;
    }

    return turbo_rtsp_client_recv_response(client, cseq);
}

static int turbo_rtsp_client_send_request(
    turbo_rtsp_client_t *client,
    turbo_rtsp_method_t method,
    const char *uri,
    const char *transport,
    const char *content_type,
    const char *range,
    const turbo_rtsp_header_t *headers,
    size_t header_count,
    const char *body,
    size_t body_len) {
    turbo_rtsp_header_t retry_headers[TURBO_RTSP_MAX_MESSAGE_HEADERS + 1];
    char auth_value[TURBO_RTSP_AUTH_VALUE_BUFFER_SIZE];
    turbo_rtsp_digest_challenge_t digest_challenge;
    size_t i = 0;
    int can_use_basic = 0;
    int can_use_digest = 0;

    if (header_count > 0 && !headers) {
        return -1;
    }

    if (turbo_rtsp_client_send_request_once(
            client,
            method,
            uri,
            transport,
            content_type,
            range,
            headers,
            header_count,
            body,
            body_len,
            NULL) == 0) {
        return 0;
    }

    if (!turbo_rtsp_client_auth_configured(client) ||
        client->last_response.status_code != 401 ||
        header_count > TURBO_RTSP_MAX_MESSAGE_HEADERS) {
        return -1;
    }

    can_use_basic =
        client->auth_preferred != TURBO_RTSP_AUTH_DIGEST &&
        turbo_rtsp_client_response_has_basic_challenge(&client->last_response);
    can_use_digest =
        client->auth_preferred != TURBO_RTSP_AUTH_BASIC &&
        turbo_rtsp_client_parse_digest_challenge(&client->last_response, &digest_challenge) == 0;

    if (can_use_digest) {
        if (turbo_rtsp_client_build_digest_auth(
                client,
                method,
                uri,
                &digest_challenge,
                auth_value,
                sizeof(auth_value)) != 0) {
            return -1;
        }
    } else if (can_use_basic) {
        if (turbo_rtsp_client_build_basic_auth(client, auth_value, sizeof(auth_value)) != 0) {
            return -1;
        }
    } else {
        return -1;
    }

    for (i = 0; i < header_count; ++i) {
        retry_headers[i] = headers[i];
    }
    retry_headers[header_count].name = "Authorization";
    retry_headers[header_count].value = auth_value;

    return turbo_rtsp_client_send_request_once(
        client,
        method,
        uri,
        transport,
        content_type,
        range,
        retry_headers,
        header_count + 1,
        body,
        body_len,
        NULL);
}

static int turbo_rtsp_client_store_last_setup_transport(turbo_rtsp_client_t *client) {
    const turbo_rtsp_header_view_t *transport = NULL;
    turbo_rtsp_transport_spec_t spec;

    if (!client) {
        return -1;
    }

    client->has_last_setup_transport = 0;
    memset(&client->last_setup_transport, 0, sizeof(client->last_setup_transport));

    transport = turbo_rtsp_client_response_find_header(&client->last_response, "Transport");
    if (!transport || !transport->value || transport->value_len == 0) {
        return -1;
    }

    if (turbo_rtsp_parse_transport(transport->value, transport->value_len, &spec) != 0) {
        return -1;
    }

    client->last_setup_transport = spec;
    client->has_last_setup_transport = 1;
    return 0;
}

static int turbo_rtsp_client_track_is_h264(const turbo_rtsp_client_media_track_t *track) {
    return track && strcmp(track->encoding_name, "H264") == 0 &&
           track->payload_type >= 0 && track->payload_type <= 127 &&
           track->clock_rate > 0;
}

static void turbo_rtsp_client_init_interleaved_track_state(
    turbo_rtsp_client_t *client,
    size_t index,
    uint8_t rtp_channel,
    uint8_t rtcp_channel) {
    turbo_rtsp_client_interleaved_track_state_t *state = NULL;
    const turbo_rtsp_client_media_track_t *media_track = NULL;
    uint32_t ssrc = 0;

    if (!client || index >= client->media_track_count ||
        index >= TURBO_RTSP_MAX_MEDIA_TRACKS) {
        return;
    }

    media_track = &client->media_tracks[index];
    state = &client->interleaved_tracks[index];
    ssrc = client->h264_rtp_ssrc_seed + (uint32_t)index;

    memset(state, 0, sizeof(*state));
    state->initialized = 1;
    state->rtp_channel = rtp_channel;
    state->rtcp_channel = rtcp_channel;
    state->payload_type = (uint8_t)media_track->payload_type;
    state->clock_rate = (uint32_t)media_track->clock_rate;
    state->ssrc = ssrc;
    state->initial_sequence_number = client->h264_rtp_initial_sequence;
    state->initial_timestamp = client->h264_rtp_initial_timestamp;
    turbo_rtsp_rtp_stream_init(
        &state->stream,
        state->payload_type,
        state->ssrc,
        state->initial_sequence_number,
        state->initial_timestamp,
        state->clock_rate);
}

static int turbo_rtsp_client_setup_interleaved_mode(
    turbo_rtsp_client_t *client,
    const turbo_rtsp_interleaved_track_t *track,
    turbo_rtsp_transport_mode_t mode) {
    char transport[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
    const char *mode_name = mode == TURBO_RTSP_TRANSPORT_MODE_RECORD ? "RECORD" : "PLAY";

    if (!client || !client->connected || !track || !track->control_uri) {
        return -1;
    }

    if (mode == TURBO_RTSP_TRANSPORT_MODE_RECORD && !client->announced) {
        return -1;
    }
    if (mode == TURBO_RTSP_TRANSPORT_MODE_PLAY && client->presentation_uri[0] == '\0') {
        return -1;
    }

    if (snprintf(
            transport,
            sizeof(transport),
            "RTP/AVP/TCP;unicast;interleaved=%u-%u;mode=%s",
            (unsigned)track->rtp_channel,
            (unsigned)track->rtcp_channel,
            mode_name) < 0) {
        return -1;
    }

    if (turbo_rtsp_client_send_request(
            client,
            TURBO_RTSP_METHOD_SETUP,
            track->control_uri,
            transport,
            NULL,
            NULL,
            NULL,
            0,
            NULL,
            0) != 0) {
        return -1;
    }
    if (turbo_rtsp_client_store_last_setup_transport(client) != 0 ||
        client->last_setup_transport.kind != TURBO_RTSP_TRANSPORT_RTP_AVP_TCP ||
        client->last_setup_transport.delivery != TURBO_RTSP_TRANSPORT_DELIVERY_UNICAST ||
        client->last_setup_transport.interleaved_rtp_channel < 0 ||
        client->last_setup_transport.interleaved_rtp_channel > 255 ||
        client->last_setup_transport.interleaved_rtcp_channel < 0 ||
        client->last_setup_transport.interleaved_rtcp_channel > 255) {
        return -1;
    }

    client->setup_done = 1;
    client->recording = 0;
    client->playing = 0;
    return 0;
}

static int turbo_rtsp_client_setup_udp_mode(
    turbo_rtsp_client_t *client,
    const turbo_rtsp_udp_track_t *track,
    turbo_rtsp_transport_mode_t mode) {
    char transport[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
    const char *mode_name = mode == TURBO_RTSP_TRANSPORT_MODE_RECORD ? "RECORD" : "PLAY";
    int rtp_port = 0;
    int rtcp_port = 0;

    if (!client || !client->connected || !track || !track->control_uri ||
        (!track->udp_pair && (track->client_rtp_port <= 0 || track->client_rtp_port > 65535))) {
        return -1;
    }

    if (track->udp_pair) {
        if (turbo_rtsp_rtp_udp_pair_get_local_ports(track->udp_pair, &rtp_port, &rtcp_port) != 0) {
            return -1;
        }
    } else {
        rtp_port = track->client_rtp_port;
        rtcp_port = track->client_rtcp_port > 0
                        ? track->client_rtcp_port
                        : track->client_rtp_port + 1;
    }

    if (rtp_port <= 0 || rtp_port > 65535 ||
        rtcp_port <= 0 || rtcp_port > 65535) {
        return -1;
    }

    if (mode == TURBO_RTSP_TRANSPORT_MODE_RECORD && !client->announced) {
        return -1;
    }
    if (mode == TURBO_RTSP_TRANSPORT_MODE_PLAY && client->presentation_uri[0] == '\0') {
        return -1;
    }

    if (snprintf(
            transport,
            sizeof(transport),
            "RTP/AVP;unicast;client_port=%d-%d;mode=%s",
            rtp_port,
            rtcp_port,
            mode_name) < 0) {
        return -1;
    }

    if (turbo_rtsp_client_send_request(
            client,
            TURBO_RTSP_METHOD_SETUP,
            track->control_uri,
            transport,
            NULL,
            NULL,
            NULL,
            0,
            NULL,
            0) != 0) {
        return -1;
    }
    if (turbo_rtsp_client_store_last_setup_transport(client) != 0 ||
        client->last_setup_transport.kind != TURBO_RTSP_TRANSPORT_RTP_AVP_UDP ||
        client->last_setup_transport.delivery != TURBO_RTSP_TRANSPORT_DELIVERY_UNICAST ||
        client->last_setup_transport.server_rtp_port <= 0 ||
        client->last_setup_transport.server_rtcp_port <= 0) {
        return -1;
    }
    if (track->udp_pair) {
        const char *peer_host = client->last_setup_transport.source[0] != '\0'
                                    ? client->last_setup_transport.source
                                    : client->host;
        if (turbo_rtsp_rtp_udp_pair_set_peer(
                track->udp_pair,
                peer_host,
                client->last_setup_transport.server_rtp_port,
                client->last_setup_transport.server_rtcp_port) != 0) {
            return -1;
        }
    }

    client->setup_done = 1;
    client->recording = 0;
    client->playing = 0;
    return 0;
}

turbo_rtsp_client_t *turbo_rtsp_client_create(
    const turbo_rtsp_client_config_t *config) {
    turbo_rtsp_client_t *client = NULL;
    turbo_rtsp_control_transport_t transport =
        config ? config->control_transport : TURBO_RTSP_CONTROL_TRANSPORT_TCP;
    uint64_t timeout_ms =
        config && config->timeout_ms ? config->timeout_ms : TURBO_RTSP_DEFAULT_TIMEOUT_MS;

    if (transport < TURBO_RTSP_CONTROL_TRANSPORT_TCP ||
        transport > TURBO_RTSP_CONTROL_TRANSPORT_WSS ||
        timeout_ms > UINT32_MAX ||
        (transport == TURBO_RTSP_CONTROL_TRANSPORT_KCP &&
         (!config || !turbo_rtsp_kcp_config_valid(config->kcp_config)))) {
        return NULL;
    }

    client = (turbo_rtsp_client_t *)calloc(1, sizeof(*client));
    if (!client) {
        return NULL;
    }

    client->port = (config && config->port > 0) ? config->port : TURBO_RTSP_DEFAULT_PORT;
    client->timeout_ms = timeout_ms;
    client->control_transport = transport;
    if (client->control_transport == TURBO_RTSP_CONTROL_TRANSPORT_KCP) {
        client->kcp_config = *config->kcp_config;
    }
    client->next_cseq = 1;
    client->h264_rtp_ssrc_seed =
        (config && config->has_h264_rtp_ssrc_seed)
            ? config->h264_rtp_ssrc_seed
            : TURBO_RTSP_CLIENT_H264_RTP_INITIAL_SSRC;
    client->h264_rtp_initial_sequence =
        (config && config->has_h264_rtp_initial_sequence)
            ? config->h264_rtp_initial_sequence
            : TURBO_RTSP_CLIENT_H264_RTP_INITIAL_SEQ;
    client->h264_rtp_initial_timestamp =
        (config && config->has_h264_rtp_initial_timestamp)
            ? config->h264_rtp_initial_timestamp
            : TURBO_RTSP_CLIENT_H264_RTP_INITIAL_TIMESTAMP;
    turbo_rtsp_safe_copy(
        client->host,
        sizeof(client->host),
        (config && config->host) ? config->host : "127.0.0.1");
    turbo_rtsp_safe_copy(
        client->user_agent,
        sizeof(client->user_agent),
        (config && config->user_agent) ? config->user_agent : TURBO_RTSP_DEFAULT_USER_AGENT);
    turbo_rtsp_safe_copy(
        client->ws_path,
        sizeof(client->ws_path),
        (config && config->ws_path) ? config->ws_path : "/");
    turbo_rtsp_safe_copy(
        client->ws_subprotocol,
        sizeof(client->ws_subprotocol),
        (config && config->ws_subprotocol) ? config->ws_subprotocol : "");

    if (transport == TURBO_RTSP_CONTROL_TRANSPORT_WSS && config && config->tls &&
        chttp_tls_profile_init(&client->tls_profile, config->tls) != SALTS_OK) {
        turbo_rtsp_kcp_config_wipe(&client->kcp_config);
        free(client);
        return NULL;
    }
    client->tls_initialized =
        transport == TURBO_RTSP_CONTROL_TRANSPORT_WSS && config && config->tls;

    if (turbo_rtsp_client_frame_queue_init(client) != 0) {
        if (client->tls_initialized) {
            (void)chttp_tls_profile_destroy(&client->tls_profile);
        }
        turbo_rtsp_kcp_config_wipe(&client->kcp_config);
        free(client);
        return NULL;
    }

    return client;
}

static int turbo_rtsp_resolve_packet_peer(
    const char *host,
    int port,
    cnet_datagram_peer *peer,
    const char **bind_host) {
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    char port_text[16];
    int status;

    if (!host || port <= 0 || port > 65535 || !peer || !bind_host) {
        return -1;
    }
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    if (snprintf(port_text, sizeof(port_text), "%d", port) < 0) {
        return -1;
    }
    status = getaddrinfo(host, port_text, &hints, &result);
    if (status != 0 || !result || !result->ai_addr) {
        if (result) {
            freeaddrinfo(result);
        }
        return -1;
    }
    memset(peer, 0, sizeof(*peer));
    if (result->ai_family == AF_INET) {
        const struct sockaddr_in *address =
            (const struct sockaddr_in *)result->ai_addr;
        peer->family = CNET_DATAGRAM_ADDRESS_IPV4;
        peer->port = ntohs(address->sin_port);
        memcpy(peer->address, &address->sin_addr, sizeof(address->sin_addr));
        *bind_host = "0.0.0.0";
    } else if (result->ai_family == AF_INET6) {
        const struct sockaddr_in6 *address =
            (const struct sockaddr_in6 *)result->ai_addr;
        peer->family = CNET_DATAGRAM_ADDRESS_IPV6;
        peer->port = ntohs(address->sin6_port);
        peer->scope_id = address->sin6_scope_id;
        memcpy(peer->address, &address->sin6_addr, sizeof(address->sin6_addr));
        *bind_host = "::";
    } else {
        freeaddrinfo(result);
        return -1;
    }
    freeaddrinfo(result);
    return 0;
}

static int turbo_rtsp_client_connect_stream(turbo_rtsp_client_t *client) {
    cnet_client_config config = turbo_rtsp_client_network_config(client, 0);
    cnet_connect_options options;
    char uri[TURBO_RTSP_MAX_URI_LEN];
    size_t events = 0;
    int status;

    if (snprintf(uri, sizeof(uri), "tcp://%s:%d", client->host, client->port) < 0 ||
        cnet_client_init(&client->network, &config) != SALTS_OK) {
        return -1;
    }
    client->network_initialized = 1;
    memset(&options, 0, sizeof(options));
    options.uri = uri;
    options.observer.on_state = turbo_rtsp_client_stream_state;
    options.observer.on_receive = turbo_rtsp_client_stream_receive;
    options.observer.on_send = turbo_rtsp_client_stream_send;
    options.observer.user = client;
    client->connect_finished = 0;
    client->connect_status = SALTS_EIO;
    status = cnet_connect(&client->network, &options, &client->connection);
    while (status == SALTS_OK && !client->connect_finished) {
        status = cnet_client_poll(
            &client->network, (uint32_t)client->timeout_ms, &events);
    }
    return status == SALTS_OK && client->connect_status == SALTS_OK ? 0 : -1;
}

static int turbo_rtsp_client_connect_packet(turbo_rtsp_client_t *client) {
    cnet_packet_endpoint_config config = CNET_PACKET_ENDPOINT_CONFIG_INIT;
    cnet_datagram_peer peer;
    const char *bind_host = NULL;
    size_t wire_bytes =
        client->kcp_config.security.fec.max_payload_bytes +
        CNET_KCP_SECURE_RECORD_OVERHEAD;
    int status;

    if (wire_bytes > CNET_DATAGRAM_MAX_PAYLOAD_BYTES ||
        turbo_rtsp_resolve_packet_peer(
            client->host, client->port, &peer, &bind_host) != 0) {
        return -1;
    }
    config.protocol = CNET_PACKET_KCP;
    config.session_capacity = 1u;
    config.datagram.backend = turbo_rtsp_backend();
    config.datagram.host = bind_host;
    config.datagram.port = 0u;
    config.datagram.send_capacity = TURBO_RTSP_SERVER_MIN_QUEUE_CAPACITY;
    config.datagram.request_capacity = TURBO_RTSP_SERVER_MIN_QUEUE_CAPACITY + 1u;
    config.datagram.completion_batch_capacity = TURBO_RTSP_SERVER_MIN_QUEUE_CAPACITY;
    config.datagram.max_datagram_bytes = wire_bytes;
    config.datagram.receive_buffer_bytes = wire_bytes;
    config.kcp = client->kcp_config.transport;
    config.security = client->kcp_config.security;
    config.observer.on_state = turbo_rtsp_client_packet_state;
    config.observer.on_receive = turbo_rtsp_client_packet_receive;
    config.observer.on_error = turbo_rtsp_client_packet_error;
    config.observer.user = client;
    if (cnet_packet_endpoint_init(&client->packet_endpoint, &config) != SALTS_OK) {
        return -1;
    }
    client->packet_initialized = 1;
    client->connect_finished = 0;
    client->connect_status = SALTS_EIO;
    status = cnet_packet_session_open(
        &client->packet_endpoint, &peer, 0u, &client->packet_session);
    if (status == SALTS_OK) {
        status = turbo_rtsp_client_poll_until(client, &client->connect_finished);
    }
    return status == SALTS_OK && client->connect_status == SALTS_OK ? 0 : -1;
}

static int turbo_rtsp_client_connect_websocket(turbo_rtsp_client_t *client) {
    chttp_websocket_client_config config;
    chttp_websocket_connect_options options;
    char uri[TURBO_RTSP_MAX_URI_LEN + 64u];
    unsigned int http_status = 0;
    int status;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.network = turbo_rtsp_client_network_config(
        client, turbo_rtsp_control_transport_is_tls(client->control_transport));
    config.network.max_send_bytes =
        TURBO_RTSP_SERVER_MAX_PENDING_BYTES +
        TURBO_RTSP_SERVER_WS_WIRE_OVERHEAD_BYTES;
    config.max_frame_bytes = TURBO_RTSP_SERVER_MAX_PENDING_BYTES;
    config.max_message_bytes = TURBO_RTSP_SERVER_MAX_PENDING_BYTES;
    config.max_buffered_input_bytes =
        TURBO_RTSP_SERVER_MAX_PENDING_BYTES +
        TURBO_RTSP_SERVER_WS_WIRE_OVERHEAD_BYTES;
    config.max_handshake_header_bytes = TURBO_RTSP_SERVER_WS_HEADER_BYTES;
    config.event_capacity = TURBO_RTSP_SERVER_MIN_QUEUE_CAPACITY;
    if (chttp_websocket_client_init(&client->websocket, &config) != SALTS_OK) {
        return -1;
    }
    client->websocket_initialized = 1;
    if (snprintf(
            uri, sizeof(uri), "%s://%s:%d%s",
            turbo_rtsp_control_transport_is_tls(client->control_transport) ? "wss" : "ws",
            client->host, client->port,
            client->ws_path[0] ? client->ws_path : "/") < 0) {
        return -1;
    }
    memset(&options, 0, sizeof(options));
    options.size = sizeof(options);
    options.uri = uri;
    options.tls = client->tls_initialized ? &client->tls_profile : NULL;
    options.timeout_ms = (uint32_t)client->timeout_ms;
    options.subprotocol = client->ws_subprotocol[0] ? client->ws_subprotocol : NULL;
    status = chttp_websocket_client_connect(
        &client->websocket, &options, &http_status);
    return status == SALTS_OK ? 0 : -1;
}

int turbo_rtsp_client_connect(turbo_rtsp_client_t *client) {
    int status;
    if (!client) {
        return -1;
    }
    if (client->connected) {
        return 0;
    }
    if (turbo_rtsp_control_transport_is_ws(client->control_transport)) {
        status = turbo_rtsp_client_connect_websocket(client);
    } else if (client->control_transport == TURBO_RTSP_CONTROL_TRANSPORT_KCP) {
        status = turbo_rtsp_client_connect_packet(client);
    } else {
        status = turbo_rtsp_client_connect_stream(client);
    }
    if (status != 0) {
        turbo_rtsp_client_close(client);
        return -1;
    }
    client->connected = 1;
    return 0;
}

turbo_rtsp_client_t *turbo_rtsp_client_open_url(
    const char *url,
    const turbo_rtsp_client_config_t *config) {
    turbo_rtsp_url_t parsed;
    turbo_rtsp_client_config_t resolved;
    turbo_rtsp_client_t *client = NULL;

    if (turbo_rtsp_url_parse(url, &parsed) != 0) {
        return NULL;
    }

    if (config) {
        resolved = *config;
    } else {
        memset(&resolved, 0, sizeof(resolved));
    }
    resolved.host = parsed.host;
    resolved.port = parsed.port;

    client = turbo_rtsp_client_create(&resolved);
    if (!client) {
        return NULL;
    }

    if (turbo_rtsp_client_connect(client) != 0) {
        turbo_rtsp_client_destroy(client);
        return NULL;
    }

    return client;
}

void turbo_rtsp_client_close(turbo_rtsp_client_t *client) {
    if (!client) {
        return;
    }
    if (client->websocket_initialized) {
        if (client->connected) {
            (void)chttp_websocket_client_close(
                &client->websocket, 1000u, NULL, 0u, (uint32_t)client->timeout_ms);
        }
        (void)chttp_websocket_client_destroy(
            &client->websocket, (uint32_t)client->timeout_ms);
        client->websocket_initialized = 0;
    }
    if (client->packet_initialized) {
        if (cnet_packet_session_valid(client->packet_session)) {
            (void)cnet_packet_session_close(
                &client->packet_endpoint, client->packet_session);
        }
        (void)cnet_packet_endpoint_stop(
            &client->packet_endpoint, (uint32_t)client->timeout_ms);
        (void)cnet_packet_endpoint_destroy(&client->packet_endpoint);
        client->packet_initialized = 0;
    }
    if (client->network_initialized) {
        (void)cnet_close(&client->network, client->connection);
        (void)cnet_client_stop(&client->network, (uint32_t)client->timeout_ms);
        (void)cnet_client_destroy(&client->network);
        client->network_initialized = 0;
    }
    memset(&client->connection, 0, sizeof(client->connection));
    memset(&client->packet_session, 0, sizeof(client->packet_session));
    client->connected = 0;
    client->announced = 0;
    client->setup_done = 0;
    client->recording = 0;
    client->playing = 0;
    client->session_id[0] = '\0';
    client->digest_nonce[0] = '\0';
    client->digest_nonce_count = 0;
    memset(&client->session, 0, sizeof(client->session));
    client->presentation_uri[0] = '\0';
    client->recv_len = 0;
    client->has_last_setup_transport = 0;
    memset(&client->last_setup_transport, 0, sizeof(client->last_setup_transport));
    client->media_track_count = 0;
    memset(client->media_tracks, 0, sizeof(client->media_tracks));
    turbo_rtsp_client_clear_last_response(client);
    turbo_rtsp_client_frame_queue_clear(client);
}

void turbo_rtsp_client_destroy(turbo_rtsp_client_t *client) {
    if (!client) {
        return;
    }
    turbo_rtsp_client_close(client);
    turbo_rtsp_client_clear_last_response(client);
    turbo_rtsp_client_frame_queue_destroy(client);
    if (client->tls_initialized) {
        (void)chttp_tls_profile_destroy(&client->tls_profile);
    }
    turbo_rtsp_kcp_config_wipe(&client->kcp_config);
    free(client);
}

const turbo_rtsp_client_response_t *turbo_rtsp_client_get_last_response(
    const turbo_rtsp_client_t *client) {
    return client ? &client->last_response : NULL;
}

int turbo_rtsp_client_set_auth(
    turbo_rtsp_client_t *client,
    const char *username,
    const char *password,
    turbo_rtsp_auth_scheme_t preferred) {
    if (!client || !username || !password ||
        preferred < TURBO_RTSP_AUTH_AUTO ||
        preferred > TURBO_RTSP_AUTH_DIGEST ||
        strlen(username) >= sizeof(client->auth_username) ||
        strlen(password) >= sizeof(client->auth_password)) {
        return -1;
    }

    turbo_rtsp_safe_copy(client->auth_username, sizeof(client->auth_username), username);
    turbo_rtsp_safe_copy(client->auth_password, sizeof(client->auth_password), password);
    client->auth_preferred = preferred;
    return 0;
}

int turbo_rtsp_client_get_last_setup_transport(
    const turbo_rtsp_client_t *client,
    turbo_rtsp_transport_spec_t *transport) {
    if (!client || !transport || !client->has_last_setup_transport) {
        return -1;
    }

    *transport = client->last_setup_transport;
    return 0;
}

int turbo_rtsp_client_get_session(
    const turbo_rtsp_client_t *client,
    turbo_rtsp_session_header_t *session) {
    if (!client || !session || client->session.id[0] == '\0') {
        return -1;
    }

    *session = client->session;
    return 0;
}

size_t turbo_rtsp_client_get_media_track_count(
    const turbo_rtsp_client_t *client) {
    return client ? client->media_track_count : 0;
}

const turbo_rtsp_client_media_track_t *turbo_rtsp_client_get_media_track(
    const turbo_rtsp_client_t *client,
    size_t index) {
    if (!client || index >= client->media_track_count) {
        return NULL;
    }

    return &client->media_tracks[index];
}

const turbo_rtsp_header_view_t *turbo_rtsp_client_response_find_header(
    const turbo_rtsp_client_response_t *response,
    const char *name) {
    size_t i = 0;

    if (!response || !name) {
        return NULL;
    }

    for (i = 0; i < response->header_count; ++i) {
        const turbo_rtsp_header_view_t *header = &response->headers[i];
        size_t j = 0;
        const size_t name_len = strlen(name);

        if (!header->name || header->name_len != name_len) {
            continue;
        }
        for (j = 0; j < name_len; ++j) {
            if (tolower((unsigned char)header->name[j]) !=
                tolower((unsigned char)name[j])) {
                break;
            }
        }
        if (j == name_len) {
            return header;
        }
    }

    return NULL;
}

int turbo_rtsp_client_options(
    turbo_rtsp_client_t *client,
    const char *uri) {
    return turbo_rtsp_client_options_ex(client, uri, NULL, 0);
}

int turbo_rtsp_client_options_ex(
    turbo_rtsp_client_t *client,
    const char *uri,
    const turbo_rtsp_header_t *headers,
    size_t header_count) {
    if (!client || !client->connected || !uri) {
        return -1;
    }

    return turbo_rtsp_client_send_request(
        client,
        TURBO_RTSP_METHOD_OPTIONS,
        uri,
        NULL,
        NULL,
        NULL,
        headers,
        header_count,
        NULL,
        0);
}

int turbo_rtsp_client_redirect(
    turbo_rtsp_client_t *client,
    const char *uri) {
    if (!client || !client->connected || !uri) {
        return -1;
    }

    return turbo_rtsp_client_send_request(
        client,
        TURBO_RTSP_METHOD_REDIRECT,
        uri,
        NULL,
        NULL,
        NULL,
        NULL,
        0,
        NULL,
        0);
}

int turbo_rtsp_client_describe(
    turbo_rtsp_client_t *client,
    const char *uri) {
    return turbo_rtsp_client_describe_ex(client, uri, NULL, 0);
}

int turbo_rtsp_client_describe_ex(
    turbo_rtsp_client_t *client,
    const char *uri,
    const turbo_rtsp_header_t *headers,
    size_t header_count) {
    turbo_rtsp_header_t combined_headers[TURBO_RTSP_MAX_MESSAGE_HEADERS];
    turbo_rtsp_header_t header;
    size_t i = 0;

    if (!client || !client->connected || !uri ||
        header_count >= TURBO_RTSP_MAX_MESSAGE_HEADERS ||
        (header_count > 0 && !headers)) {
        return -1;
    }

    memset(&header, 0, sizeof(header));
    header.name = "Accept";
    header.value = "application/sdp";
    combined_headers[0] = header;
    for (i = 0; i < header_count; ++i) {
        combined_headers[i + 1] = headers[i];
    }

    if (turbo_rtsp_client_send_request(
            client,
            TURBO_RTSP_METHOD_DESCRIBE,
            uri,
            NULL,
            NULL,
            NULL,
            combined_headers,
            header_count + 1,
            NULL,
            0) != 0) {
        return -1;
    }

    turbo_rtsp_safe_copy(client->presentation_uri, sizeof(client->presentation_uri), uri);
    if (turbo_rtsp_client_store_sdp_tracks(client) != 0) {
        return -1;
    }
    client->announced = 0;
    client->setup_done = 0;
    client->recording = 0;
    client->playing = 0;
    return 0;
}

int turbo_rtsp_client_announce(
    turbo_rtsp_client_t *client,
    const turbo_rtsp_push_announce_t *announce) {
    size_t sdp_len = 0;
    int rc = 0;

    if (!client || !client->connected || !announce || !announce->uri || !announce->sdp) {
        return -1;
    }

    sdp_len = announce->sdp_len != 0 ? announce->sdp_len : strlen(announce->sdp);
    rc = turbo_rtsp_client_send_request(
        client,
        TURBO_RTSP_METHOD_ANNOUNCE,
        announce->uri,
        NULL,
        "application/sdp",
        NULL,
        NULL,
        0,
        announce->sdp,
        sdp_len);
    if (rc == 0) {
        turbo_rtsp_safe_copy(client->presentation_uri, sizeof(client->presentation_uri), announce->uri);
        turbo_rtsp_client_try_store_announced_sdp_tracks(client, announce);
        client->announced = 1;
        client->setup_done = 0;
        client->recording = 0;
        client->playing = 0;
    }
    return rc;
}

int turbo_rtsp_client_setup_interleaved(
    turbo_rtsp_client_t *client,
    const turbo_rtsp_push_track_t *track) {
    turbo_rtsp_interleaved_track_t interleaved_track;

    if (!track) {
        return -1;
    }

    memset(&interleaved_track, 0, sizeof(interleaved_track));
    interleaved_track.control_uri = track->control_uri;
    interleaved_track.rtp_channel = track->rtp_channel;
    interleaved_track.rtcp_channel = track->rtcp_channel;

    return turbo_rtsp_client_setup_interleaved_track(client, &interleaved_track);
}

int turbo_rtsp_client_setup_interleaved_track(
    turbo_rtsp_client_t *client,
    const turbo_rtsp_interleaved_track_t *track) {
    return turbo_rtsp_client_setup_interleaved_mode(
        client,
        track,
        TURBO_RTSP_TRANSPORT_MODE_RECORD);
}

int turbo_rtsp_client_setup_play_interleaved(
    turbo_rtsp_client_t *client,
    const turbo_rtsp_interleaved_track_t *track) {
    return turbo_rtsp_client_setup_interleaved_mode(
        client,
        track,
        TURBO_RTSP_TRANSPORT_MODE_PLAY);
}

static int turbo_rtsp_client_setup_interleaved_track_index_mode(
    turbo_rtsp_client_t *client,
    size_t index,
    uint8_t rtp_channel,
    uint8_t rtcp_channel,
    turbo_rtsp_transport_mode_t mode) {
    turbo_rtsp_interleaved_track_t track;
    const turbo_rtsp_client_media_track_t *media_track = NULL;
    uint8_t negotiated_rtp_channel = rtp_channel;
    uint8_t negotiated_rtcp_channel = rtcp_channel;

    media_track = turbo_rtsp_client_get_media_track(client, index);
    if (!media_track || media_track->control_uri[0] == '\0') {
        return -1;
    }

    memset(&track, 0, sizeof(track));
    track.control_uri = media_track->control_uri;
    track.rtp_channel = rtp_channel;
    track.rtcp_channel = rtcp_channel;
    if (turbo_rtsp_client_setup_interleaved_mode(client, &track, mode) != 0) {
        return -1;
    }
    if (client->has_last_setup_transport &&
        client->last_setup_transport.interleaved_rtp_channel >= 0 &&
        client->last_setup_transport.interleaved_rtp_channel <= 255 &&
        client->last_setup_transport.interleaved_rtcp_channel >= 0 &&
        client->last_setup_transport.interleaved_rtcp_channel <= 255) {
        negotiated_rtp_channel = (uint8_t)client->last_setup_transport.interleaved_rtp_channel;
        negotiated_rtcp_channel = (uint8_t)client->last_setup_transport.interleaved_rtcp_channel;
    }
    turbo_rtsp_client_init_interleaved_track_state(
        client,
        index,
        negotiated_rtp_channel,
        negotiated_rtcp_channel);
    return 0;
}

int turbo_rtsp_client_setup_record_interleaved_track_index(
    turbo_rtsp_client_t *client,
    size_t index,
    uint8_t rtp_channel,
    uint8_t rtcp_channel) {
    return turbo_rtsp_client_setup_interleaved_track_index_mode(
        client,
        index,
        rtp_channel,
        rtcp_channel,
        TURBO_RTSP_TRANSPORT_MODE_RECORD);
}

int turbo_rtsp_client_setup_play_interleaved_track_index(
    turbo_rtsp_client_t *client,
    size_t index,
    uint8_t rtp_channel,
    uint8_t rtcp_channel) {
    return turbo_rtsp_client_setup_interleaved_track_index_mode(
        client,
        index,
        rtp_channel,
        rtcp_channel,
        TURBO_RTSP_TRANSPORT_MODE_PLAY);
}

int turbo_rtsp_client_setup_record_udp(
    turbo_rtsp_client_t *client,
    const turbo_rtsp_udp_track_t *track) {
    return turbo_rtsp_client_setup_udp_mode(
        client,
        track,
        TURBO_RTSP_TRANSPORT_MODE_RECORD);
}

int turbo_rtsp_client_setup_play_udp(
    turbo_rtsp_client_t *client,
    const turbo_rtsp_udp_track_t *track) {
    return turbo_rtsp_client_setup_udp_mode(
        client,
        track,
        TURBO_RTSP_TRANSPORT_MODE_PLAY);
}

static int turbo_rtsp_client_setup_udp_track_index_mode(
    turbo_rtsp_client_t *client,
    size_t index,
    turbo_rtsp_rtp_udp_pair_t *udp_pair,
    turbo_rtsp_transport_mode_t mode) {
    turbo_rtsp_udp_track_t track;
    const turbo_rtsp_client_media_track_t *media_track = NULL;

    media_track = turbo_rtsp_client_get_media_track(client, index);
    if (!media_track || media_track->control_uri[0] == '\0' || !udp_pair) {
        return -1;
    }

    memset(&track, 0, sizeof(track));
    track.control_uri = media_track->control_uri;
    track.udp_pair = udp_pair;
    return turbo_rtsp_client_setup_udp_mode(client, &track, mode);
}

static int turbo_rtsp_client_setup_udp_ports_track_index_mode(
    turbo_rtsp_client_t *client,
    size_t index,
    int client_rtp_port,
    int client_rtcp_port,
    turbo_rtsp_transport_mode_t mode) {
    turbo_rtsp_udp_track_t track;
    const turbo_rtsp_client_media_track_t *media_track = NULL;

    media_track = turbo_rtsp_client_get_media_track(client, index);
    if (!media_track || media_track->control_uri[0] == '\0') {
        return -1;
    }

    memset(&track, 0, sizeof(track));
    track.control_uri = media_track->control_uri;
    track.client_rtp_port = client_rtp_port;
    track.client_rtcp_port = client_rtcp_port;
    return turbo_rtsp_client_setup_udp_mode(client, &track, mode);
}

int turbo_rtsp_client_setup_record_udp_track_index(
    turbo_rtsp_client_t *client,
    size_t index,
    turbo_rtsp_rtp_udp_pair_t *udp_pair) {
    return turbo_rtsp_client_setup_udp_track_index_mode(
        client,
        index,
        udp_pair,
        TURBO_RTSP_TRANSPORT_MODE_RECORD);
}

int turbo_rtsp_client_setup_play_udp_track_index(
    turbo_rtsp_client_t *client,
    size_t index,
    turbo_rtsp_rtp_udp_pair_t *udp_pair) {
    return turbo_rtsp_client_setup_udp_track_index_mode(
        client,
        index,
        udp_pair,
        TURBO_RTSP_TRANSPORT_MODE_PLAY);
}

int turbo_rtsp_client_setup_record_udp_ports_track_index(
    turbo_rtsp_client_t *client,
    size_t index,
    int client_rtp_port,
    int client_rtcp_port) {
    return turbo_rtsp_client_setup_udp_ports_track_index_mode(
        client,
        index,
        client_rtp_port,
        client_rtcp_port,
        TURBO_RTSP_TRANSPORT_MODE_RECORD);
}

int turbo_rtsp_client_setup_play_udp_ports_track_index(
    turbo_rtsp_client_t *client,
    size_t index,
    int client_rtp_port,
    int client_rtcp_port) {
    return turbo_rtsp_client_setup_udp_ports_track_index_mode(
        client,
        index,
        client_rtp_port,
        client_rtcp_port,
        TURBO_RTSP_TRANSPORT_MODE_PLAY);
}

int turbo_rtsp_client_play(
    turbo_rtsp_client_t *client,
    const char *range) {
    return turbo_rtsp_client_play_ex(client, range, NULL, 0);
}

int turbo_rtsp_client_play_ex(
    turbo_rtsp_client_t *client,
    const char *range,
    const turbo_rtsp_header_t *headers,
    size_t header_count) {
    if (!client || !client->connected || !client->setup_done ||
        client->presentation_uri[0] == '\0' || client->session_id[0] == '\0') {
        return -1;
    }

    if (turbo_rtsp_client_send_request(
            client,
            TURBO_RTSP_METHOD_PLAY,
            client->presentation_uri,
            NULL,
            NULL,
            range ? range : "npt=0-",
            headers,
            header_count,
            NULL,
            0) != 0) {
        return -1;
    }

    client->recording = 0;
    client->playing = 1;
    return 0;
}

int turbo_rtsp_client_pause(turbo_rtsp_client_t *client) {
    if (!client || !client->connected || !client->playing ||
        client->presentation_uri[0] == '\0' || client->session_id[0] == '\0') {
        return -1;
    }

    if (turbo_rtsp_client_send_request(
            client,
            TURBO_RTSP_METHOD_PAUSE,
            client->presentation_uri,
            NULL,
            NULL,
            NULL,
            NULL,
            0,
            NULL,
            0) != 0) {
        return -1;
    }

    client->playing = 0;
    return 0;
}

int turbo_rtsp_client_record(turbo_rtsp_client_t *client) {
    if (!client || !client->connected || !client->setup_done ||
        client->presentation_uri[0] == '\0' || client->session_id[0] == '\0') {
        return -1;
    }

    if (turbo_rtsp_client_send_request(
            client,
            TURBO_RTSP_METHOD_RECORD,
            client->presentation_uri,
            NULL,
            NULL,
            "npt=0-",
            NULL,
            0,
            NULL,
            0) != 0) {
        return -1;
    }

    client->recording = 1;
    return 0;
}

static int turbo_rtsp_client_parameter(
    turbo_rtsp_client_t *client,
    turbo_rtsp_method_t method,
    const char *content_type,
    const turbo_rtsp_header_t *headers,
    size_t header_count,
    const char *body,
    size_t body_len) {
    if (!client || !client->connected ||
        client->presentation_uri[0] == '\0' || client->session_id[0] == '\0') {
        return -1;
    }

    return turbo_rtsp_client_send_request(
        client,
        method,
        client->presentation_uri,
        NULL,
        content_type,
        NULL,
        headers,
        header_count,
        body,
        body_len);
}

int turbo_rtsp_client_get_parameter(
    turbo_rtsp_client_t *client,
    const char *content_type,
    const char *body,
    size_t body_len) {
    return turbo_rtsp_client_get_parameter_ex(
        client,
        content_type,
        NULL,
        0,
        body,
        body_len);
}

int turbo_rtsp_client_get_parameter_ex(
    turbo_rtsp_client_t *client,
    const char *content_type,
    const turbo_rtsp_header_t *headers,
    size_t header_count,
    const char *body,
    size_t body_len) {
    return turbo_rtsp_client_parameter(
        client,
        TURBO_RTSP_METHOD_GET_PARAMETER,
        content_type,
        headers,
        header_count,
        body,
        body_len);
}

int turbo_rtsp_client_set_parameter(
    turbo_rtsp_client_t *client,
    const char *content_type,
    const char *body,
    size_t body_len) {
    return turbo_rtsp_client_set_parameter_ex(
        client,
        content_type,
        NULL,
        0,
        body,
        body_len);
}

int turbo_rtsp_client_set_parameter_ex(
    turbo_rtsp_client_t *client,
    const char *content_type,
    const turbo_rtsp_header_t *headers,
    size_t header_count,
    const char *body,
    size_t body_len) {
    return turbo_rtsp_client_parameter(
        client,
        TURBO_RTSP_METHOD_SET_PARAMETER,
        content_type,
        headers,
        header_count,
        body,
        body_len);
}

int turbo_rtsp_client_teardown(turbo_rtsp_client_t *client) {
    if (!client || !client->connected ||
        client->presentation_uri[0] == '\0' || client->session_id[0] == '\0') {
        return -1;
    }

    if (turbo_rtsp_client_send_request(
            client,
            TURBO_RTSP_METHOD_TEARDOWN,
            client->presentation_uri,
            NULL,
            NULL,
            NULL,
            NULL,
            0,
            NULL,
            0) != 0) {
        return -1;
    }

    client->announced = 0;
    client->setup_done = 0;
    client->recording = 0;
    client->playing = 0;
    client->session_id[0] = '\0';
    memset(&client->session, 0, sizeof(client->session));
    client->presentation_uri[0] = '\0';
    return 0;
}

int turbo_rtsp_client_send_interleaved_frame(
    turbo_rtsp_client_t *client,
    uint8_t channel,
    const uint8_t *payload,
    size_t payload_len) {
    if (!client || !client->connected || (!client->recording && !client->playing) ||
        (!payload && payload_len > 0) || payload_len > UINT16_MAX) {
        return -1;
    }

    {
        uint8_t *frame = (uint8_t *)malloc(
            TURBO_RTSP_INTERLEAVED_HEADER_SIZE + payload_len);
        int status;
        if (!frame) {
            return -1;
        }
        if (turbo_rtsp_interleaved_write_header(
                frame, TURBO_RTSP_INTERLEAVED_HEADER_SIZE,
                channel, (uint16_t)payload_len) !=
            TURBO_RTSP_INTERLEAVED_HEADER_SIZE) {
            free(frame);
            return -1;
        }
        if (payload_len > 0) {
            memcpy(frame + TURBO_RTSP_INTERLEAVED_HEADER_SIZE, payload, payload_len);
        }
        status = turbo_rtsp_client_send_bytes(
            client, frame, TURBO_RTSP_INTERLEAVED_HEADER_SIZE + payload_len);
        free(frame);
        return status;
    }
}

int turbo_rtsp_client_send_h264_nal_interleaved_track_index(
    turbo_rtsp_client_t *client,
    size_t index,
    const uint8_t *nal,
    size_t nal_len,
    uint32_t timestamp_increment,
    uint32_t *rtp_timestamp_or_null) {
    return turbo_rtsp_client_send_h264_nal_interleaved_track_index_ex(
        client,
        index,
        nal,
        nal_len,
        1,
        timestamp_increment,
        rtp_timestamp_or_null);
}

int turbo_rtsp_client_send_h264_nal_interleaved_track_index_ex(
    turbo_rtsp_client_t *client,
    size_t index,
    const uint8_t *nal,
    size_t nal_len,
    int marker,
    uint32_t timestamp_increment,
    uint32_t *rtp_timestamp_or_null) {
    const turbo_rtsp_client_media_track_t *media_track = NULL;
    turbo_rtsp_client_interleaved_track_state_t *track = NULL;
    turbo_rtsp_rtp_stream_t next_stream;
    turbo_rtsp_rtp_stream_packet_t *packets = NULL;
    uint8_t *packet_buffer = NULL;
    uint8_t payload_scratch[TURBO_RTSP_CLIENT_H264_RTP_MAX_PAYLOAD];
    size_t packet_capacity = 0;
    size_t packet_count = 0;
    size_t i = 0;
    uint32_t rtp_timestamp = 0;

    if (!client || !client->connected || (!client->recording && !client->playing) ||
        !nal || nal_len == 0 || index >= client->media_track_count ||
        index >= TURBO_RTSP_MAX_MEDIA_TRACKS) {
        return -1;
    }

    media_track = &client->media_tracks[index];
    track = &client->interleaved_tracks[index];
    if (!track->initialized || !turbo_rtsp_client_track_is_h264(media_track)) {
        return -1;
    }

    packet_capacity = nal_len <= TURBO_RTSP_CLIENT_H264_RTP_MAX_PAYLOAD
                          ? 1u
                          : (nal_len - 1u + (TURBO_RTSP_CLIENT_H264_RTP_MAX_PAYLOAD - 3u)) /
                                (TURBO_RTSP_CLIENT_H264_RTP_MAX_PAYLOAD - 2u);
    if (packet_capacity == 0 ||
        packet_capacity > SIZE_MAX / sizeof(*packets) ||
        packet_capacity > SIZE_MAX / TURBO_RTSP_CLIENT_H264_RTP_PACKET_SIZE) {
        return -1;
    }

    packets = (turbo_rtsp_rtp_stream_packet_t *)calloc(packet_capacity, sizeof(*packets));
    packet_buffer = (uint8_t *)malloc(packet_capacity * TURBO_RTSP_CLIENT_H264_RTP_PACKET_SIZE);
    if (!packets || !packet_buffer) {
        free(packet_buffer);
        free(packets);
        return -1;
    }

    next_stream = track->stream;
    rtp_timestamp = next_stream.timestamp;
    if (turbo_rtsp_rtp_stream_write_h264_nal_ex(
            &next_stream,
            nal,
            nal_len,
            marker,
            timestamp_increment,
            TURBO_RTSP_CLIENT_H264_RTP_MAX_PAYLOAD,
            packet_buffer,
            TURBO_RTSP_CLIENT_H264_RTP_PACKET_SIZE,
            payload_scratch,
            sizeof(payload_scratch),
            packets,
            packet_capacity,
            &packet_count) != 0) {
        free(packet_buffer);
        free(packets);
        return -1;
    }

    for (i = 0; i < packet_count; ++i) {
        if (packets[i].packet_len > UINT16_MAX ||
            turbo_rtsp_client_send_interleaved_frame(
                client,
                track->rtp_channel,
                packets[i].packet,
                packets[i].packet_len) != 0) {
            /* A socket failure after earlier packets were sent cannot roll back
             * those external bytes; keep local RTP state unchanged and report failure. */
            free(packet_buffer);
            free(packets);
            return -1;
        }
    }

    track->stream = next_stream;
    if (rtp_timestamp_or_null) {
        *rtp_timestamp_or_null = rtp_timestamp;
    }
    free(packet_buffer);
    free(packets);
    return 0;
}

int turbo_rtsp_client_send_sender_rtcp_compound_interleaved_track_index(
    turbo_rtsp_client_t *client,
    size_t index,
    uint64_t ntp_timestamp,
    uint32_t rtp_timestamp,
    const char *cname,
    size_t cname_len) {
    turbo_rtsp_client_interleaved_track_state_t *track = NULL;
    uint8_t packet[TURBO_RTSP_CLIENT_RTCP_COMPOUND_BUFFER_SIZE];
    int packet_len = 0;

    if (!client || !client->connected || (!client->recording && !client->playing) ||
        index >= client->media_track_count || index >= TURBO_RTSP_MAX_MEDIA_TRACKS) {
        return -1;
    }

    track = &client->interleaved_tracks[index];
    if (!track->initialized) {
        return -1;
    }

    packet_len = turbo_rtsp_rtp_sender_write_rtcp_compound(
        &track->stream.sender,
        packet,
        sizeof(packet),
        ntp_timestamp,
        rtp_timestamp,
        NULL,
        0,
        cname,
        cname_len);
    if (packet_len <= 0) {
        return -1;
    }

    return turbo_rtsp_client_send_interleaved_frame(
        client,
        track->rtcp_channel,
        packet,
        (size_t)packet_len);
}

int turbo_rtsp_client_recv_interleaved_frame(
    turbo_rtsp_client_t *client,
    uint8_t *channel,
    uint8_t *payload,
    size_t payload_capacity,
    size_t *payload_len) {
    int queued_rc = 0;

    if (!client || !client->connected || !channel || !payload_len ||
        (!payload && payload_capacity > 0)) {
        return -1;
    }

    queued_rc = turbo_rtsp_client_frame_queue_pop(
        client,
        channel,
        payload,
        payload_capacity,
        payload_len);
    if (queued_rc != 0) {
        return queued_rc > 0 ? 0 : -1;
    }

    for (;;) {
        size_t consumed = 0;

        if (client->recv_len > 0 && (unsigned char)client->recv_buffer[0] == '$') {
            turbo_rtsp_interleaved_frame_t frame;
            int rc = turbo_rtsp_interleaved_parse(
                (const uint8_t *)client->recv_buffer,
                client->recv_len,
                &frame,
                &consumed);
            if (rc == TURBO_RTSP_FRAME_OK) {
                if (frame.payload_len > payload_capacity) {
                    *payload_len = frame.payload_len;
                    return -1;
                }
                if (frame.payload_len > 0) {
                    memcpy(payload, frame.payload, frame.payload_len);
                }
                *channel = frame.channel;
                *payload_len = frame.payload_len;

                if (client->recv_len > consumed) {
                    memmove(
                        client->recv_buffer,
                        client->recv_buffer + consumed,
                        client->recv_len - consumed);
                }
                client->recv_len -= consumed;
                return 0;
            }
            if (rc != TURBO_RTSP_FRAME_PARTIAL) {
                return -1;
            }
        } else if (client->recv_len > 0) {
            turbo_rtsp_response_view_t response;
            int rc = turbo_rtsp_parse_response(
                client->recv_buffer,
                client->recv_len,
                &consumed,
                &response);
            if (rc == TURBO_RTSP_PARSE_OK) {
                if (turbo_rtsp_client_copy_last_response(client, &response) != 0 ||
                    turbo_rtsp_client_copy_session(client, &response) != 0) {
                    return -1;
                }
                if (client->recv_len > consumed) {
                    memmove(
                        client->recv_buffer,
                        client->recv_buffer + consumed,
                        client->recv_len - consumed);
                }
                client->recv_len -= consumed;
                continue;
            }
            if (rc != TURBO_RTSP_PARSE_PARTIAL) {
                return -1;
            }
        }

        if (turbo_rtsp_client_recv_more(client) != 0) {
            return -1;
        }
    }
}

#endif /* TURBO_MEDIA_HAS_RTSP */
