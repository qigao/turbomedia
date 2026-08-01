/**
 * TurboMedia RTSP control-plane support
 *
 * RTSP parser/server layer.
 */
#ifndef TURBO_RTSP_H
#define TURBO_RTSP_H

#include <stddef.h>
#include <stdint.h>
#include <turbo_export.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef TURBO_MEDIA_HAS_RTSP

#include <CoroNet.h>

#define TURBO_RTSP_MAX_URI_LEN            512
#define TURBO_RTSP_MAX_HEADER_VALUE_LEN   256
#define TURBO_RTSP_MAX_MESSAGE_HEADERS    32
#define TURBO_RTSP_MAX_MEDIA_TRACKS       8
#define TURBO_RTSP_MAX_SDP_FMTP_LEN       512

typedef enum {
    TURBO_RTSP_METHOD_UNKNOWN = 0,
    TURBO_RTSP_METHOD_OPTIONS,
    TURBO_RTSP_METHOD_DESCRIBE,
    TURBO_RTSP_METHOD_SETUP,
    TURBO_RTSP_METHOD_PLAY,
    TURBO_RTSP_METHOD_TEARDOWN,
    TURBO_RTSP_METHOD_PAUSE,
    TURBO_RTSP_METHOD_ANNOUNCE,
    TURBO_RTSP_METHOD_RECORD,
    TURBO_RTSP_METHOD_GET_PARAMETER,
    TURBO_RTSP_METHOD_SET_PARAMETER,
    TURBO_RTSP_METHOD_REDIRECT
} turbo_rtsp_method_t;

typedef enum {
    TURBO_RTSP_TRANSPORT_UNSPECIFIED = 0,
    TURBO_RTSP_TRANSPORT_RTP_AVP_UDP,
    TURBO_RTSP_TRANSPORT_RTP_AVP_TCP
} turbo_rtsp_transport_t;

typedef enum {
    TURBO_RTSP_CONTROL_TRANSPORT_TCP = 0,
    TURBO_RTSP_CONTROL_TRANSPORT_KCP,
    TURBO_RTSP_CONTROL_TRANSPORT_WS,
    TURBO_RTSP_CONTROL_TRANSPORT_WSS
} turbo_rtsp_control_transport_t;

typedef enum {
    TURBO_RTSP_AUTH_AUTO = 0,
    TURBO_RTSP_AUTH_BASIC,
    TURBO_RTSP_AUTH_DIGEST
} turbo_rtsp_auth_scheme_t;

typedef enum {
    TURBO_RTSP_TRANSPORT_DELIVERY_UNSPECIFIED = 0,
    TURBO_RTSP_TRANSPORT_DELIVERY_UNICAST,
    TURBO_RTSP_TRANSPORT_DELIVERY_MULTICAST
} turbo_rtsp_transport_delivery_t;

typedef enum {
    TURBO_RTSP_TRANSPORT_MODE_UNSPECIFIED = 0,
    TURBO_RTSP_TRANSPORT_MODE_PLAY,
    TURBO_RTSP_TRANSPORT_MODE_RECORD
} turbo_rtsp_transport_mode_t;

typedef enum {
    TURBO_RTSP_RANGE_UNSPECIFIED = 0,
    TURBO_RTSP_RANGE_NPT,
    TURBO_RTSP_RANGE_SMPTE,
    TURBO_RTSP_RANGE_SMPTE_30_DROP,
    TURBO_RTSP_RANGE_SMPTE_25,
    TURBO_RTSP_RANGE_CLOCK
} turbo_rtsp_range_t;

typedef struct {
    const char *name;
    const char *value;
} turbo_rtsp_header_t;

typedef struct {
    char host[256];
    int port;
    char path[TURBO_RTSP_MAX_URI_LEN];
} turbo_rtsp_url_t;

typedef struct {
    turbo_rtsp_transport_t kind;
    turbo_rtsp_transport_delivery_t delivery;
    turbo_rtsp_transport_mode_t mode;
    char destination[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
    char source[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
    int client_rtp_port;
    int client_rtcp_port;
    int server_rtp_port;
    int server_rtcp_port;
    int multicast_rtp_port;
    int multicast_rtcp_port;
    int interleaved_rtp_channel;
    int interleaved_rtcp_channel;
    int ttl;
    int layers;
    int append;
    uint32_t ssrc;
    int has_ssrc;
} turbo_rtsp_transport_spec_t;

typedef struct {
    char id[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
    int timeout_seconds;
    int has_timeout;
} turbo_rtsp_session_header_t;

typedef struct {
    turbo_rtsp_range_t type;
    char value[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
    int64_t start_ms;
    int64_t end_ms;
    int has_start;
    int has_end;
    int start_is_now;
    int64_t time_ms;
    int has_time;
} turbo_rtsp_range_header_t;

typedef struct {
    char url[TURBO_RTSP_MAX_URI_LEN];
    uint32_t seq;
    int has_seq;
    uint32_t rtptime;
    int has_rtptime;
} turbo_rtsp_rtp_info_t;

typedef struct {
    turbo_rtsp_method_t method;
    uint32_t cseq;
    char uri[TURBO_RTSP_MAX_URI_LEN];
    char session_id[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
    char range[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
    char transport[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
    turbo_rtsp_transport_t transport_kind;
    int client_rtp_port;
    int client_rtcp_port;
    int interleaved_rtp_channel;
    int interleaved_rtcp_channel;
    turbo_rtsp_transport_spec_t transport_spec;
    turbo_rtsp_session_header_t session;
    turbo_rtsp_range_header_t range_spec;
} turbo_rtsp_request_t;

typedef struct {
    const char *bind_host;          /* NULL defaults to 0.0.0.0 */
    int port;                       /* 0 defaults to 554 */
    uint64_t client_timeout_ms;     /* 0 defaults to 30000 */
    const char *server_name;        /* NULL defaults to TurboMedia RTSP */
    const char *public_methods;     /* NULL uses every built-in RTSP method handler name */
    turbo_rtsp_control_transport_t control_transport; /* 0 defaults to TCP */
    /**
     * Required when control_transport is KCP. The configuration must contain a
     * non-zero PSK accepted by CoroNet. turbo_rtsp_server_create() copies it.
     */
    const turbo_kcp_config_t *kcp_config;
} turbo_rtsp_server_config_t;

typedef struct {
    int status_code;                /* 0 defaults to 200 */
    const char *reason;             /* NULL uses a built-in phrase for status_code */
    const char *content_type;       /* Optional Content-Type */
    const char *body;               /* Optional response body */
    size_t body_len;                /* 0 uses strlen(body) when body is not NULL */
    const char *session_id;         /* Optional Session header */
    const char *transport;          /* Optional Transport header */
    const char *public_methods;     /* Optional Public header */
    const char *range;              /* Optional Range header */
    const char *rtp_info;           /* Optional RTP-Info header */
    const turbo_rtsp_header_t *headers;
    size_t header_count;
    turbo_rtsp_header_t location_header; /* Storage used by turbo_rtsp_response_redirect */
} turbo_rtsp_response_t;

typedef struct {
    turbo_rtsp_method_t method;
    const char *uri;
    uint32_t cseq;
    const char *user_agent;
    const char *session_id;
    const char *transport;
    const char *content_type;
    const char *range;
    const turbo_rtsp_header_t *headers;
    size_t header_count;
    const char *body;
    size_t body_len;
} turbo_rtsp_request_message_t;

typedef struct {
    const char *name;
    size_t name_len;
    const char *value;
    size_t value_len;
} turbo_rtsp_header_view_t;

typedef struct {
    turbo_rtsp_request_t request;
    turbo_rtsp_header_view_t headers[TURBO_RTSP_MAX_MESSAGE_HEADERS];
    size_t header_count;
    /* Header and body pointers view the session read buffer and are valid only
     * during the active request callback, or until the next session read. */
    const char *body;
    size_t body_len;
} turbo_rtsp_message_t;

typedef struct turbo_rtsp_server_s turbo_rtsp_server_t;
typedef struct turbo_rtsp_session_s turbo_rtsp_session_t;
typedef struct turbo_rtsp_client_s turbo_rtsp_client_t;
#ifndef TURBO_RTSP_RTP_UDP_PAIR_TYPE_DEFINED
#define TURBO_RTSP_RTP_UDP_PAIR_TYPE_DEFINED
typedef struct turbo_rtsp_rtp_udp_pair_s turbo_rtsp_rtp_udp_pair_t;
#endif

typedef int (*turbo_rtsp_request_cb)(turbo_rtsp_session_t *session,
                                     const turbo_rtsp_request_t *request,
                                     turbo_rtsp_response_t *response,
                                     void *user_data);

typedef int (*turbo_rtsp_interleaved_cb)(turbo_rtsp_session_t *session,
                                         uint8_t channel,
                                         const uint8_t *payload,
                                         size_t payload_len,
                                         void *user_data);

typedef void (*turbo_rtsp_session_close_cb)(turbo_rtsp_session_t *session,
                                            void *user_data);

typedef struct {
    turbo_rtsp_request_cb on_options;
    turbo_rtsp_request_cb on_describe;
    turbo_rtsp_request_cb on_setup;
    turbo_rtsp_request_cb on_play;
    turbo_rtsp_request_cb on_teardown;
    turbo_rtsp_request_cb on_pause;
    turbo_rtsp_request_cb on_announce;
    turbo_rtsp_request_cb on_record;
    turbo_rtsp_request_cb on_get_parameter;
    turbo_rtsp_request_cb on_set_parameter;
    turbo_rtsp_request_cb on_redirect;
    turbo_rtsp_request_cb on_unknown;
    turbo_rtsp_interleaved_cb on_interleaved_frame;
    turbo_rtsp_session_close_cb on_session_close;
} turbo_rtsp_server_handlers_t;

typedef struct {
    const char *host;
    int port;
    uint64_t timeout_ms;
    const char *user_agent;
    turbo_rtsp_control_transport_t control_transport; /* 0 defaults to TCP */
    const char *ws_path;            /* NULL defaults to "/" for WS/WSS */
    const char *ws_subprotocol;     /* Optional Sec-WebSocket-Protocol */
    uint32_t h264_rtp_ssrc_seed;
    int has_h264_rtp_ssrc_seed;
    uint16_t h264_rtp_initial_sequence;
    int has_h264_rtp_initial_sequence;
    uint32_t h264_rtp_initial_timestamp;
    int has_h264_rtp_initial_timestamp;
    /**
     * Required when control_transport is KCP. The configuration must match the
     * server configuration. turbo_rtsp_client_create() copies it.
     */
    const turbo_kcp_config_t *kcp_config;
} turbo_rtsp_client_config_t;

typedef struct {
    const char *uri;
    const char *sdp;
    size_t sdp_len;
} turbo_rtsp_push_announce_t;

typedef struct {
    const char *control_uri;
    uint8_t rtp_channel;
    uint8_t rtcp_channel;
} turbo_rtsp_interleaved_track_t;

typedef turbo_rtsp_interleaved_track_t turbo_rtsp_push_track_t;

typedef struct {
    const char *control_uri;
    int client_rtp_port;
    int client_rtcp_port; /* 0 defaults to client_rtp_port + 1 */
    turbo_rtsp_rtp_udp_pair_t *udp_pair; /* Optional: use pair ports and bind peer after SETUP */
} turbo_rtsp_udp_track_t;

typedef struct {
    int status_code;
    const char *content_type;
    turbo_rtsp_header_view_t headers[TURBO_RTSP_MAX_MESSAGE_HEADERS];
    size_t header_count;
    const char *body;
    size_t body_len;
} turbo_rtsp_client_response_t;

typedef struct {
    char control_uri[TURBO_RTSP_MAX_URI_LEN];
    char media[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
    char proto[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
    char encoding_name[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
    int payload_type;
    int clock_rate;
    char fmtp[TURBO_RTSP_MAX_SDP_FMTP_LEN];
    char range[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
    char direction[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
    char connection_address[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
} turbo_rtsp_client_media_track_t;

CXX_C_API int turbo_rtsp_response_options(
    turbo_rtsp_response_t *response,
    const char *public_methods);

CXX_C_API int turbo_rtsp_response_status(
    turbo_rtsp_response_t *response,
    int status_code,
    const char *reason);

CXX_C_API int turbo_rtsp_response_redirect(
    turbo_rtsp_response_t *response,
    int status_code,
    const char *location);

CXX_C_API int turbo_rtsp_response_describe(
    turbo_rtsp_response_t *response,
    const char *sdp,
    size_t sdp_len);

CXX_C_API int turbo_rtsp_response_setup(
    turbo_rtsp_response_t *response,
    const char *session_id,
    const char *transport);

CXX_C_API int turbo_rtsp_response_play(
    turbo_rtsp_response_t *response,
    const char *range,
    const char *rtp_info);

CXX_C_API int turbo_rtsp_response_pause(
    turbo_rtsp_response_t *response);

CXX_C_API int turbo_rtsp_response_teardown(
    turbo_rtsp_response_t *response);

CXX_C_API int turbo_rtsp_response_announce(
    turbo_rtsp_response_t *response);

CXX_C_API int turbo_rtsp_response_record(
    turbo_rtsp_response_t *response,
    const char *range);

CXX_C_API int turbo_rtsp_response_get_parameter(
    turbo_rtsp_response_t *response,
    const char *content_type,
    const char *body,
    size_t body_len);

CXX_C_API int turbo_rtsp_response_set_parameter(
    turbo_rtsp_response_t *response);

/**
 * @brief Create an RTSP server.
 *
 * KCP control transport requires config->kcp_config. The configuration is
 * copied, so the caller may wipe or release it after this function returns.
 * Its contents are validated by CoroNet when turbo_rtsp_server_start() creates
 * the listener; invalid values make start return -1.
 *
 * @param ctx Coroutine context that owns all server sockets.
 * @param config Optional server configuration. KCP requires kcp_config.
 * @param handlers Optional request callbacks copied by the server.
 * @param user_data Opaque callback context retained without ownership transfer.
 * @return A server instance, or NULL when ctx is NULL, required KCP
 *         configuration is absent, or allocation fails.
 */
CXX_C_API turbo_rtsp_server_t *turbo_rtsp_server_create(
    coro_context_t *ctx,
    const turbo_rtsp_server_config_t *config,
    const turbo_rtsp_server_handlers_t *handlers,
    void *user_data);

CXX_C_API int turbo_rtsp_server_start(turbo_rtsp_server_t *server);
/**
 * @brief Begin asynchronous server shutdown.
 *
 * Active connections and pending TCP/TLS/WebSocket admission tasks are
 * cancelled. turbo_rtsp_server_destroy() waits for their completion.
 */
CXX_C_API void turbo_rtsp_server_stop(turbo_rtsp_server_t *server);
CXX_C_API void turbo_rtsp_server_destroy(turbo_rtsp_server_t *server);

CXX_C_API int turbo_rtsp_url_parse(
    const char *url,
    turbo_rtsp_url_t *parsed);

CXX_C_API coro_socket_t *turbo_rtsp_session_get_control_socket(
    turbo_rtsp_session_t *session);

CXX_C_API const turbo_rtsp_request_t *turbo_rtsp_session_get_last_request(
    const turbo_rtsp_session_t *session);

CXX_C_API const turbo_rtsp_message_t *turbo_rtsp_session_get_last_message(
    const turbo_rtsp_session_t *session);

CXX_C_API int turbo_rtsp_session_send_interleaved_frame(
    turbo_rtsp_session_t *session,
    uint8_t channel,
    const uint8_t *payload,
    size_t payload_len);

CXX_C_API int turbo_rtsp_session_setup_udp_transport(
    turbo_rtsp_session_t *session,
    turbo_rtsp_response_t *response,
    const char *session_id,
    const char *local_host,
    const char *peer_host);

CXX_C_API int turbo_rtsp_session_send_rtp_udp(
    turbo_rtsp_session_t *session,
    const uint8_t *packet,
    size_t packet_len);

CXX_C_API int turbo_rtsp_session_send_rtcp_udp(
    turbo_rtsp_session_t *session,
    const uint8_t *packet,
    size_t packet_len);

CXX_C_API int turbo_rtsp_session_recv_rtp_udp(
    turbo_rtsp_session_t *session,
    uint8_t *buffer,
    size_t buffer_size,
    size_t *packet_len);

CXX_C_API int turbo_rtsp_session_recv_rtcp_udp(
    turbo_rtsp_session_t *session,
    uint8_t *buffer,
    size_t buffer_size,
    size_t *packet_len);

/**
 * @brief Create an RTSP client.
 *
 * KCP control transport requires config->kcp_config. The configuration is
 * copied, so the caller may wipe or release it after this function returns.
 * Its contents are validated by CoroNet when turbo_rtsp_client_connect()
 * creates the socket; invalid values make connect return -1.
 *
 * @param ctx Coroutine context that owns the client socket.
 * @param config Optional client configuration. KCP requires kcp_config.
 * @return A client instance, or NULL when ctx is NULL, required KCP
 *         configuration is absent, or allocation fails.
 */
CXX_C_API turbo_rtsp_client_t *turbo_rtsp_client_create(
    coro_context_t *ctx,
    const turbo_rtsp_client_config_t *config);

CXX_C_API int turbo_rtsp_client_connect(
    turbo_rtsp_client_t *client);

CXX_C_API turbo_rtsp_client_t *turbo_rtsp_client_open_url(
    coro_context_t *ctx,
    const char *url,
    const turbo_rtsp_client_config_t *config);

CXX_C_API void turbo_rtsp_client_close(
    turbo_rtsp_client_t *client);

CXX_C_API void turbo_rtsp_client_destroy(
    turbo_rtsp_client_t *client);

CXX_C_API const turbo_rtsp_client_response_t *turbo_rtsp_client_get_last_response(
    const turbo_rtsp_client_t *client);

CXX_C_API int turbo_rtsp_client_set_auth(
    turbo_rtsp_client_t *client,
    const char *username,
    const char *password,
    turbo_rtsp_auth_scheme_t preferred);

CXX_C_API int turbo_rtsp_client_get_last_setup_transport(
    const turbo_rtsp_client_t *client,
    turbo_rtsp_transport_spec_t *transport);

CXX_C_API int turbo_rtsp_client_get_session(
    const turbo_rtsp_client_t *client,
    turbo_rtsp_session_header_t *session);

CXX_C_API size_t turbo_rtsp_client_get_media_track_count(
    const turbo_rtsp_client_t *client);

CXX_C_API const turbo_rtsp_client_media_track_t *turbo_rtsp_client_get_media_track(
    const turbo_rtsp_client_t *client,
    size_t index);

CXX_C_API const turbo_rtsp_header_view_t *turbo_rtsp_client_response_find_header(
    const turbo_rtsp_client_response_t *response,
    const char *name);

CXX_C_API int turbo_rtsp_client_options(
    turbo_rtsp_client_t *client,
    const char *uri);

CXX_C_API int turbo_rtsp_client_options_ex(
    turbo_rtsp_client_t *client,
    const char *uri,
    const turbo_rtsp_header_t *headers,
    size_t header_count);

CXX_C_API int turbo_rtsp_client_redirect(
    turbo_rtsp_client_t *client,
    const char *uri);

CXX_C_API int turbo_rtsp_client_describe(
    turbo_rtsp_client_t *client,
    const char *uri);

CXX_C_API int turbo_rtsp_client_describe_ex(
    turbo_rtsp_client_t *client,
    const char *uri,
    const turbo_rtsp_header_t *headers,
    size_t header_count);

CXX_C_API int turbo_rtsp_client_announce(
    turbo_rtsp_client_t *client,
    const turbo_rtsp_push_announce_t *announce);

CXX_C_API int turbo_rtsp_client_setup_interleaved(
    turbo_rtsp_client_t *client,
    const turbo_rtsp_push_track_t *track);

CXX_C_API int turbo_rtsp_client_setup_interleaved_track(
    turbo_rtsp_client_t *client,
    const turbo_rtsp_interleaved_track_t *track);

CXX_C_API int turbo_rtsp_client_setup_play_interleaved(
    turbo_rtsp_client_t *client,
    const turbo_rtsp_interleaved_track_t *track);

CXX_C_API int turbo_rtsp_client_setup_record_interleaved_track_index(
    turbo_rtsp_client_t *client,
    size_t index,
    uint8_t rtp_channel,
    uint8_t rtcp_channel);

CXX_C_API int turbo_rtsp_client_setup_play_interleaved_track_index(
    turbo_rtsp_client_t *client,
    size_t index,
    uint8_t rtp_channel,
    uint8_t rtcp_channel);

CXX_C_API int turbo_rtsp_client_setup_record_udp(
    turbo_rtsp_client_t *client,
    const turbo_rtsp_udp_track_t *track);

CXX_C_API int turbo_rtsp_client_setup_play_udp(
    turbo_rtsp_client_t *client,
    const turbo_rtsp_udp_track_t *track);

CXX_C_API int turbo_rtsp_client_setup_record_udp_track_index(
    turbo_rtsp_client_t *client,
    size_t index,
    turbo_rtsp_rtp_udp_pair_t *udp_pair);

CXX_C_API int turbo_rtsp_client_setup_play_udp_track_index(
    turbo_rtsp_client_t *client,
    size_t index,
    turbo_rtsp_rtp_udp_pair_t *udp_pair);

CXX_C_API int turbo_rtsp_client_setup_record_udp_ports_track_index(
    turbo_rtsp_client_t *client,
    size_t index,
    int client_rtp_port,
    int client_rtcp_port);

CXX_C_API int turbo_rtsp_client_setup_play_udp_ports_track_index(
    turbo_rtsp_client_t *client,
    size_t index,
    int client_rtp_port,
    int client_rtcp_port);

CXX_C_API int turbo_rtsp_client_play(
    turbo_rtsp_client_t *client,
    const char *range);

CXX_C_API int turbo_rtsp_client_play_ex(
    turbo_rtsp_client_t *client,
    const char *range,
    const turbo_rtsp_header_t *headers,
    size_t header_count);

CXX_C_API int turbo_rtsp_client_pause(
    turbo_rtsp_client_t *client);

CXX_C_API int turbo_rtsp_client_record(
    turbo_rtsp_client_t *client);

CXX_C_API int turbo_rtsp_client_get_parameter(
    turbo_rtsp_client_t *client,
    const char *content_type,
    const char *body,
    size_t body_len);

CXX_C_API int turbo_rtsp_client_get_parameter_ex(
    turbo_rtsp_client_t *client,
    const char *content_type,
    const turbo_rtsp_header_t *headers,
    size_t header_count,
    const char *body,
    size_t body_len);

CXX_C_API int turbo_rtsp_client_set_parameter(
    turbo_rtsp_client_t *client,
    const char *content_type,
    const char *body,
    size_t body_len);

CXX_C_API int turbo_rtsp_client_set_parameter_ex(
    turbo_rtsp_client_t *client,
    const char *content_type,
    const turbo_rtsp_header_t *headers,
    size_t header_count,
    const char *body,
    size_t body_len);

CXX_C_API int turbo_rtsp_client_teardown(
    turbo_rtsp_client_t *client);

CXX_C_API int turbo_rtsp_client_send_interleaved_frame(
    turbo_rtsp_client_t *client,
    uint8_t channel,
    const uint8_t *payload,
    size_t payload_len);

CXX_C_API int turbo_rtsp_client_send_h264_nal_interleaved_track_index(
    turbo_rtsp_client_t *client,
    size_t index,
    const uint8_t *nal,
    size_t nal_len,
    uint32_t timestamp_increment,
    uint32_t *rtp_timestamp_or_null);

CXX_C_API int turbo_rtsp_client_send_h264_nal_interleaved_track_index_ex(
    turbo_rtsp_client_t *client,
    size_t index,
    const uint8_t *nal,
    size_t nal_len,
    int marker,
    uint32_t timestamp_increment,
    uint32_t *rtp_timestamp_or_null);

CXX_C_API int turbo_rtsp_client_send_sender_rtcp_compound_interleaved_track_index(
    turbo_rtsp_client_t *client,
    size_t index,
    uint64_t ntp_timestamp,
    uint32_t rtp_timestamp,
    const char *cname,
    size_t cname_len);

/* Returns -1 with *payload_len set to the required size when payload_capacity
 * is too small. The frame remains pending so callers can retry with a larger
 * buffer. */
CXX_C_API int turbo_rtsp_client_recv_interleaved_frame(
    turbo_rtsp_client_t *client,
    uint8_t *channel,
    uint8_t *payload,
    size_t payload_capacity,
    size_t *payload_len);

CXX_C_API const turbo_rtsp_header_view_t *turbo_rtsp_message_find_header(
    const turbo_rtsp_message_t *message,
    const char *name);

#endif /* TURBO_MEDIA_HAS_RTSP */

#ifdef __cplusplus
}
#endif

#endif /* TURBO_RTSP_H */
