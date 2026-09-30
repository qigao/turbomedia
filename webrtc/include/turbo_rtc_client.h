#ifndef TURBO_RTC_CLIENT_H
#define TURBO_RTC_CLIENT_H

#include <stddef.h>
#include <stdint.h>
#include <turbo_export.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_RTC_CLIENT_MAX_ICE_SERVERS 8u

typedef struct turbo_rtc_client_s turbo_rtc_client_t;

typedef enum turbo_rtc_client_status_t {
    TURBO_RTC_CLIENT_OK = 0,
    TURBO_RTC_CLIENT_EINVAL = -1,
    TURBO_RTC_CLIENT_ENOMEM = -2,
    TURBO_RTC_CLIENT_ESTATE = -3,
    TURBO_RTC_CLIENT_EHTTP = -4,
    TURBO_RTC_CLIENT_ESDP = -5,
    TURBO_RTC_CLIENT_EPEER = -6,
    TURBO_RTC_CLIENT_ETIMEDOUT = -7,
    TURBO_RTC_CLIENT_EIO = -8,
    TURBO_RTC_CLIENT_EQUEUE = -9
} turbo_rtc_client_status_t;

typedef enum turbo_rtc_client_state_t {
    TURBO_RTC_CLIENT_CREATED = 0,
    TURBO_RTC_CLIENT_PREPARED,
    TURBO_RTC_CLIENT_NEGOTIATING,
    TURBO_RTC_CLIENT_CONNECTING,
    TURBO_RTC_CLIENT_CONNECTED,
    TURBO_RTC_CLIENT_DRAINING,
    TURBO_RTC_CLIENT_CLOSED,
    TURBO_RTC_CLIENT_FAILED
} turbo_rtc_client_state_t;

/*
 * WHIP audio publisher configuration.
 *
 * create() copies every string and ICE-server entry. No pointer in this
 * structure is retained after create() returns.
 */
typedef struct turbo_rtc_client_config_t {
    size_t size;

    const char *whip_base_url;
    const char *whip_path;
    const char *bearer_token;

    const char *ca_file;
    const char *cert_file;
    const char *key_file;
    const char *key_password;
    const char *server_name;

    uint32_t request_timeout_ms;
    uint32_t connect_timeout_ms;
    int allow_plaintext_loopback;
    int allow_loopback;

    const char *const *stun_servers;
    size_t stun_server_count;
    const char *const *turn_servers;
    size_t turn_server_count;

    uint32_t sample_rate;
    uint32_t channels;
    uint32_t bitrate;
    uint32_t frame_size_ms;
    int enable_fec;
    int enable_dtx;
} turbo_rtc_client_config_t;

typedef struct turbo_rtc_client_snapshot_t {
    size_t size;
    turbo_rtc_client_state_t state;
    int connected;
    int session_active;
    int last_http_status;
    uint64_t frames_sent;
    uint64_t connect_started_ms;
} turbo_rtc_client_snapshot_t;

TURBO_MEDIA_C_API void turbo_rtc_client_config_init(
    turbo_rtc_client_config_t *config);

TURBO_MEDIA_C_API void turbo_rtc_client_snapshot_init(
    turbo_rtc_client_snapshot_t *snapshot);

/*
 * Creates one owner-driven RTC client. This call performs no signaling
 * request. All configured strings and ICE server entries are copied.
 */
TURBO_MEDIA_C_API turbo_rtc_client_status_t turbo_rtc_client_create(
    const turbo_rtc_client_config_t *config,
    turbo_rtc_client_t **out_client);

/*
 * Creates the bounded CHTTP client, PeerConnection, and one send-only Opus
 * audio track. No WHIP POST is sent by prepare().
 */
TURBO_MEDIA_C_API turbo_rtc_client_status_t turbo_rtc_client_prepare(
    turbo_rtc_client_t *client);

/*
 * Performs the WHIP POST + answer application + trickle ICE PATCH
 * synchronously on the owner thread. Success enters CONNECTING; poll() drives
 * ICE/DTLS/SRTP until CONNECTED.
 */
TURBO_MEDIA_C_API turbo_rtc_client_status_t turbo_rtc_client_start(
    turbo_rtc_client_t *client);

/*
 * Performs an in-place WHIP ICE restart for an existing live resource.
 *
 * Valid from CONNECTED or from FAILED while a remote session still exists.
 * The client rotates local ICE credentials, PATCHes the current resource with
 * its strong ETag, requires a 200 response with a new ETag and remote restart
 * SDP fragment, applies that fragment, then enters CONNECTING.
 *
 * There is no session recreation or stale-ETag fallback. Any signaling or
 * restart-fragment failure moves the client to FAILED; stop() remains the
 * cleanup path for the existing remote resource.
 */
TURBO_MEDIA_C_API turbo_rtc_client_status_t turbo_rtc_client_restart_ice(
    turbo_rtc_client_t *client);

/*
 * Advances PeerConnection once. There is no hidden poll thread.
 * A connect deadline failure moves the client to FAILED.
 */
TURBO_MEDIA_C_API turbo_rtc_client_status_t turbo_rtc_client_poll(
    turbo_rtc_client_t *client);

/*
 * Sends exactly one configured Opus PCM frame through the connected track.
 *
 * The payload is borrowed interleaved signed-16 PCM and must contain exactly
 * sample_rate * frame_size_ms / 1000 samples per channel. RTP timestamp
 * ownership remains inside the track; callers do not project device/wall-clock
 * timestamps into RTP time.
 */
TURBO_MEDIA_C_API turbo_rtc_client_status_t turbo_rtc_client_send_audio(
    turbo_rtc_client_t *client,
    const void *pcm,
    size_t len);

/*
 * Stops local media/PeerConnection ownership and DELETEs the WHIP resource.
 * DELETE accepts 200/204/404. A remote cleanup failure is surfaced as EHTTP;
 * no signaling/backend fallback is attempted.
 */
TURBO_MEDIA_C_API turbo_rtc_client_status_t turbo_rtc_client_stop(
    turbo_rtc_client_t *client);

TURBO_MEDIA_C_API turbo_rtc_client_status_t turbo_rtc_client_snapshot(
    const turbo_rtc_client_t *client,
    turbo_rtc_client_snapshot_t *snapshot);

/*
 * Destroy is valid only when no remote session is live and no PeerConnection
 * callback can still run: CREATED, PREPARED, CLOSED, or locally cleaned FAILED.
 */
TURBO_MEDIA_C_API turbo_rtc_client_status_t turbo_rtc_client_destroy(
    turbo_rtc_client_t *client);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_RTC_CLIENT_H */
