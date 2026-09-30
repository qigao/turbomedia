#ifndef TURBO_RTC_SUBSCRIBER_H
#define TURBO_RTC_SUBSCRIBER_H

#include <stddef.h>
#include <stdint.h>

#include "turbo_export.h"
#include "turbo_rtc_client.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_RTC_SUBSCRIBER_MAX_ICE_SERVERS 8u
#define TURBO_RTC_SUBSCRIBER_MAX_FRAME_QUEUE 64u
#define TURBO_RTC_SUBSCRIBER_MAX_FRAME_BYTES 65536u

typedef struct turbo_rtc_subscriber_s turbo_rtc_subscriber_t;

typedef enum turbo_rtc_subscriber_audio_result_t {
    TURBO_RTC_SUBSCRIBER_AUDIO_CONSUMED = 0,
    TURBO_RTC_SUBSCRIBER_AUDIO_RETRY = 1,
    TURBO_RTC_SUBSCRIBER_AUDIO_FATAL = -1
} turbo_rtc_subscriber_audio_result_t;

/*
 * Called from turbo_rtc_subscriber_poll() on the owner thread.
 *
 * pcm is borrowed and is valid only for the duration of the callback.
 *
 * CONSUMED removes the queue-head frame. RETRY leaves that exact frame at the
 * queue head and makes poll() return without busy-spinning, so bounded sink
 * backpressure can recover on a later poll. Any other result is fatal.
 *
 * The callback must not re-enter poll(), stop(), or destroy(); those calls
 * return ESTATE while the callback is active.
 */
typedef int (*turbo_rtc_subscriber_audio_cb)(
    void *context,
    const void *pcm,
    size_t bytes,
    uint32_t sample_rate,
    uint32_t channels,
    uint64_t rtp_timestamp);

typedef struct turbo_rtc_subscriber_config_t {
    size_t size;

    const char *whep_base_url;
    const char *whep_path;
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
    uint32_t frame_queue_capacity;
    uint32_t max_frame_bytes;

    turbo_rtc_subscriber_audio_cb on_audio;
    void *audio_context;
} turbo_rtc_subscriber_config_t;

typedef struct turbo_rtc_subscriber_snapshot_t {
    size_t size;
    turbo_rtc_client_state_t state;
    int connected;
    int session_active;
    int last_http_status;

    uint64_t frames_received;
    uint64_t frames_delivered;
    uint64_t frames_deferred;
    uint64_t frames_rejected;

    uint32_t queue_items;
    uint32_t queue_high_water;
    uint64_t queue_bytes;
    uint64_t queue_bytes_high_water;

    uint64_t connect_started_ms;
} turbo_rtc_subscriber_snapshot_t;

TURBO_MEDIA_C_API void turbo_rtc_subscriber_config_init(
    turbo_rtc_subscriber_config_t *config);

TURBO_MEDIA_C_API void turbo_rtc_subscriber_snapshot_init(
    turbo_rtc_subscriber_snapshot_t *snapshot);

/*
 * Creates one owner-driven WHEP subscriber and copies every configured string
 * and ICE-server entry. create() performs no network operation.
 *
 * Except for the internal PeerConnection callbacks, all lifecycle functions
 * and snapshot() are owner-thread APIs.
 */
TURBO_MEDIA_C_API turbo_rtc_client_status_t turbo_rtc_subscriber_create(
    const turbo_rtc_subscriber_config_t *config,
    turbo_rtc_subscriber_t **out_subscriber);

/*
 * Creates the bounded CHTTP client, PeerConnection, and recv-only Opus track.
 * No WHEP POST is sent by prepare().
 */
TURBO_MEDIA_C_API turbo_rtc_client_status_t turbo_rtc_subscriber_prepare(
    turbo_rtc_subscriber_t *subscriber);

/*
 * Performs the WHEP POST + answer application + trickle ICE PATCH
 * synchronously on the owner thread. Success enters CONNECTING unless the peer
 * already reached CONNECTED.
 */
TURBO_MEDIA_C_API turbo_rtc_client_status_t turbo_rtc_subscriber_start(
    turbo_rtc_subscriber_t *subscriber);

/*
 * Advances PeerConnection once and drains the bounded decoded-audio queue.
 * User audio callbacks are invoked only from this function on the owner thread.
 *
 * A full internal queue returns TURBO_RTC_CLIENT_EQUEUE and fails the session;
 * no DROP_OLDEST or unbounded growth is used.
 */
TURBO_MEDIA_C_API turbo_rtc_client_status_t turbo_rtc_subscriber_poll(
    turbo_rtc_subscriber_t *subscriber);

/*
 * Stops local media/PeerConnection ownership and DELETEs the WHEP resource.
 * DELETE accepts 200/204/404. A remote cleanup failure is returned as EHTTP
 * and can be retried by calling stop() again from FAILED.
 */
TURBO_MEDIA_C_API turbo_rtc_client_status_t turbo_rtc_subscriber_stop(
    turbo_rtc_subscriber_t *subscriber);

TURBO_MEDIA_C_API turbo_rtc_client_status_t turbo_rtc_subscriber_snapshot(
    const turbo_rtc_subscriber_t *subscriber,
    turbo_rtc_subscriber_snapshot_t *snapshot);

/*
 * Destroy is rejected while a remote WHEP session is still live.
 */
TURBO_MEDIA_C_API turbo_rtc_client_status_t turbo_rtc_subscriber_destroy(
    turbo_rtc_subscriber_t *subscriber);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_RTC_SUBSCRIBER_H */
