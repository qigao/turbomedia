#include "turbo_rtc_subscriber.h"

#include "turbo_http_media_client.h"
#include "turbo_peer_connection.h"

#include <salts/clock.h>
#include <salts/thread.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TURBO_RTC_SUBSCRIBER_DEFAULT_QUEUE 8u
#define TURBO_RTC_SUBSCRIBER_DEFAULT_MAX_FRAME_BYTES 11520u

struct turbo_rtc_subscriber_s {
    turbo_rtc_client_state_t state;

    char *whep_base_url;
    char *whep_path;
    char *bearer_token;
    char *ca_file;
    char *cert_file;
    char *key_file;
    char *key_password;
    char *server_name;

    char **stun_servers;
    size_t stun_server_count;
    char **turn_servers;
    size_t turn_server_count;

    uint32_t request_timeout_ms;
    uint32_t connect_timeout_ms;
    int allow_plaintext_loopback;
    int allow_loopback;

    uint32_t sample_rate;
    uint32_t channels;
    uint32_t frame_queue_capacity;
    uint32_t max_frame_bytes;

    turbo_rtc_subscriber_audio_cb on_audio;
    void *audio_context;

    turbo_http_media_client_t *http;
    turbo_peer_connection_t *pc;
    turbo_media_track_t *audio_track;

    char session_location[512];
    char session_etag[128];
    int last_http_status;
    uint64_t connect_started_ms;

    salts_mutex_t lock;
    int lock_initialized;
    turbo_peer_state_t peer_state;
    int accept_frames;
    int stopping;
    int queue_overflowed;
    int frame_error;

    uint8_t *frame_storage;
    uint8_t *delivery_buffer;
    size_t *frame_lengths;
    uint64_t *frame_timestamps;
    size_t queue_head;
    size_t queue_tail;
    size_t queue_count;
    size_t queue_bytes;
    size_t queue_high_water;
    size_t queue_bytes_high_water;

    uint64_t frames_received;
    uint64_t frames_delivered;
    uint64_t frames_rejected;
};

static char *subscriber_strdup(const char *value) {
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

static int subscriber_bool_valid(int value) {
    return value == 0 || value == 1;
}

static int subscriber_sample_rate_valid(uint32_t sample_rate) {
    return sample_rate == 8000u || sample_rate == 16000u ||
           sample_rate == 24000u || sample_rate == 48000u;
}

static int subscriber_string_present(const char *value) {
    return value && value[0] != '\0';
}

static void subscriber_free_string_array(char **items, size_t count) {
    size_t i;
    if (!items) {
        return;
    }
    for (i = 0u; i < count; ++i) {
        free(items[i]);
    }
    free(items);
}

static char **subscriber_copy_string_array(
    const char *const *items, size_t count) {
    char **copy;
    size_t i;
    if (count == 0u) {
        return NULL;
    }
    copy = (char **)calloc(count, sizeof(*copy));
    if (!copy) {
        return NULL;
    }
    for (i = 0u; i < count; ++i) {
        copy[i] = subscriber_strdup(items[i]);
        if (!copy[i]) {
            subscriber_free_string_array(copy, count);
            return NULL;
        }
    }
    return copy;
}

static int subscriber_optional_copy(
    const char *source, char **destination) {
    if (!source) {
        *destination = NULL;
        return 1;
    }
    *destination = subscriber_strdup(source);
    return *destination != NULL;
}

static int subscriber_config_valid(
    const turbo_rtc_subscriber_config_t *config) {
    size_t i;
    size_t frame_alignment;
    if (!config || config->size < sizeof(*config) ||
        !subscriber_string_present(config->whep_base_url) ||
        !subscriber_string_present(config->whep_path) ||
        config->whep_path[0] != '/' ||
        config->request_timeout_ms == 0u ||
        config->connect_timeout_ms == 0u ||
        !subscriber_bool_valid(config->allow_plaintext_loopback) ||
        !subscriber_bool_valid(config->allow_loopback) ||
        !subscriber_sample_rate_valid(config->sample_rate) ||
        (config->channels != 1u && config->channels != 2u) ||
        config->frame_queue_capacity == 0u ||
        config->frame_queue_capacity > TURBO_RTC_SUBSCRIBER_MAX_FRAME_QUEUE ||
        config->max_frame_bytes == 0u ||
        config->max_frame_bytes > TURBO_RTC_SUBSCRIBER_MAX_FRAME_BYTES ||
        !config->on_audio ||
        config->stun_server_count > TURBO_RTC_SUBSCRIBER_MAX_ICE_SERVERS ||
        config->turn_server_count > TURBO_RTC_SUBSCRIBER_MAX_ICE_SERVERS ||
        (config->stun_server_count != 0u && !config->stun_servers) ||
        (config->turn_server_count != 0u && !config->turn_servers)) {
        return 0;
    }
    frame_alignment = (size_t)config->channels * 2u;
    if (frame_alignment == 0u ||
        config->max_frame_bytes < frame_alignment ||
        (config->max_frame_bytes % frame_alignment) != 0u) {
        return 0;
    }
    for (i = 0u; i < config->stun_server_count; ++i) {
        if (!subscriber_string_present(config->stun_servers[i])) {
            return 0;
        }
    }
    for (i = 0u; i < config->turn_server_count; ++i) {
        if (!subscriber_string_present(config->turn_servers[i])) {
            return 0;
        }
    }
    return 1;
}

static void subscriber_reset_queue_locked(
    turbo_rtc_subscriber_t *subscriber) {
    subscriber->queue_head = 0u;
    subscriber->queue_tail = 0u;
    subscriber->queue_count = 0u;
    subscriber->queue_bytes = 0u;
    subscriber->queue_overflowed = 0;
    subscriber->frame_error = 0;
}

static void subscriber_clear_peer(turbo_rtc_subscriber_t *subscriber) {
    turbo_media_track_t *track = NULL;
    turbo_peer_connection_t *pc = NULL;
    if (!subscriber) {
        return;
    }

    if (subscriber->lock_initialized) {
        salts_mutex_lock(&subscriber->lock);
        subscriber->accept_frames = 0;
        subscriber->stopping = 1;
        track = subscriber->audio_track;
        pc = subscriber->pc;
        subscriber->audio_track = NULL;
        subscriber->pc = NULL;
        salts_mutex_unlock(&subscriber->lock);
    } else {
        subscriber->audio_track = NULL;
        subscriber->pc = NULL;
    }

    if (track) {
        turbo_media_track_stop(track);
    }
    if (pc) {
        turbo_peer_connection_destroy(pc);
    }

    if (subscriber->lock_initialized) {
        salts_mutex_lock(&subscriber->lock);
        subscriber->peer_state = TURBO_PEER_STATE_CLOSED;
        subscriber_reset_queue_locked(subscriber);
        salts_mutex_unlock(&subscriber->lock);
    }
}

static turbo_rtc_client_status_t subscriber_delete_remote(
    turbo_rtc_subscriber_t *subscriber, const char *location) {
    turbo_http_media_response_t response;
    int status;
    if (!subscriber || !location || location[0] == '\0') {
        return TURBO_RTC_CLIENT_OK;
    }
    memset(&response, 0, sizeof(response));
    status = turbo_http_media_request(
        subscriber->http, "DELETE", location, NULL, NULL, NULL, &response);
    subscriber->last_http_status = response.status;
    if (status != 0 ||
        (response.status != 200 && response.status != 204 &&
         response.status != 404)) {
        return TURBO_RTC_CLIENT_EHTTP;
    }
    return TURBO_RTC_CLIENT_OK;
}

static void subscriber_on_frame(
    turbo_media_track_t *track,
    const uint8_t *data,
    size_t length,
    uint64_t timestamp,
    void *user_data) {
    turbo_rtc_subscriber_t *subscriber =
        (turbo_rtc_subscriber_t *)user_data;
    size_t alignment;
    size_t slot_offset;

    (void)track;
    if (!subscriber || !data || length == 0u ||
        !subscriber->lock_initialized) {
        return;
    }

    salts_mutex_lock(&subscriber->lock);
    if (!subscriber->accept_frames) {
        salts_mutex_unlock(&subscriber->lock);
        return;
    }

    subscriber->frames_received++;
    alignment = (size_t)subscriber->channels * 2u;
    if (alignment == 0u || length > subscriber->max_frame_bytes ||
        (length % alignment) != 0u) {
        subscriber->frames_rejected++;
        subscriber->frame_error = 1;
        salts_mutex_unlock(&subscriber->lock);
        return;
    }

    if (subscriber->queue_count >= subscriber->frame_queue_capacity) {
        subscriber->frames_rejected++;
        subscriber->queue_overflowed = 1;
        salts_mutex_unlock(&subscriber->lock);
        return;
    }

    slot_offset = subscriber->queue_tail * subscriber->max_frame_bytes;
    memcpy(subscriber->frame_storage + slot_offset, data, length);
    subscriber->frame_lengths[subscriber->queue_tail] = length;
    subscriber->frame_timestamps[subscriber->queue_tail] = timestamp;
    subscriber->queue_tail =
        (subscriber->queue_tail + 1u) % subscriber->frame_queue_capacity;
    subscriber->queue_count++;
    subscriber->queue_bytes += length;
    if (subscriber->queue_count > subscriber->queue_high_water) {
        subscriber->queue_high_water = subscriber->queue_count;
    }
    if (subscriber->queue_bytes > subscriber->queue_bytes_high_water) {
        subscriber->queue_bytes_high_water = subscriber->queue_bytes;
    }
    salts_mutex_unlock(&subscriber->lock);
}

static void subscriber_on_track(
    turbo_peer_connection_t *pc,
    turbo_media_track_t *track,
    void *user_data) {
    turbo_rtc_subscriber_t *subscriber =
        (turbo_rtc_subscriber_t *)user_data;
    (void)pc;
    if (!subscriber || !track ||
        turbo_media_track_get_type(track) != TURBO_RTC_MEDIA_TRACK_AUDIO ||
        !(turbo_media_track_get_direction(track) &
          TURBO_MEDIA_DIRECTION_RECVONLY)) {
        return;
    }
    turbo_media_track_set_user_data(track, subscriber);
    turbo_media_track_on_frame(track, subscriber_on_frame);
    if (subscriber->lock_initialized) {
        salts_mutex_lock(&subscriber->lock);
        if (!subscriber->stopping) {
            subscriber->audio_track = track;
        }
        salts_mutex_unlock(&subscriber->lock);
    }
}

static void subscriber_on_state_change(
    turbo_peer_connection_t *pc,
    turbo_peer_state_t state,
    void *user_data) {
    turbo_rtc_subscriber_t *subscriber =
        (turbo_rtc_subscriber_t *)user_data;
    (void)pc;
    if (!subscriber || !subscriber->lock_initialized) {
        return;
    }
    salts_mutex_lock(&subscriber->lock);
    subscriber->peer_state = state;
    salts_mutex_unlock(&subscriber->lock);
}

static void subscriber_free(turbo_rtc_subscriber_t *subscriber) {
    if (!subscriber) {
        return;
    }
    subscriber_clear_peer(subscriber);
    turbo_http_media_client_destroy(subscriber->http);
    subscriber->http = NULL;

    if (subscriber->lock_initialized) {
        salts_mutex_destroy(&subscriber->lock);
        subscriber->lock_initialized = 0;
    }

    free(subscriber->frame_timestamps);
    free(subscriber->frame_lengths);
    free(subscriber->delivery_buffer);
    free(subscriber->frame_storage);

    subscriber_free_string_array(
        subscriber->stun_servers, subscriber->stun_server_count);
    subscriber_free_string_array(
        subscriber->turn_servers, subscriber->turn_server_count);

    free(subscriber->server_name);
    free(subscriber->key_password);
    free(subscriber->key_file);
    free(subscriber->cert_file);
    free(subscriber->ca_file);
    free(subscriber->bearer_token);
    free(subscriber->whep_path);
    free(subscriber->whep_base_url);
    free(subscriber);
}

void turbo_rtc_subscriber_config_init(
    turbo_rtc_subscriber_config_t *config) {
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->size = sizeof(*config);
    config->request_timeout_ms = 5000u;
    config->connect_timeout_ms = 10000u;
    config->sample_rate = 48000u;
    config->channels = 1u;
    config->frame_queue_capacity = TURBO_RTC_SUBSCRIBER_DEFAULT_QUEUE;
    config->max_frame_bytes =
        TURBO_RTC_SUBSCRIBER_DEFAULT_MAX_FRAME_BYTES;
}

void turbo_rtc_subscriber_snapshot_init(
    turbo_rtc_subscriber_snapshot_t *snapshot) {
    if (!snapshot) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->size = sizeof(*snapshot);
}

turbo_rtc_client_status_t turbo_rtc_subscriber_create(
    const turbo_rtc_subscriber_config_t *config,
    turbo_rtc_subscriber_t **out_subscriber) {
    turbo_rtc_subscriber_t *subscriber;
    size_t storage_bytes;

    if (!out_subscriber) {
        return TURBO_RTC_CLIENT_EINVAL;
    }
    *out_subscriber = NULL;
    if (!subscriber_config_valid(config)) {
        return TURBO_RTC_CLIENT_EINVAL;
    }

    storage_bytes =
        (size_t)config->frame_queue_capacity * config->max_frame_bytes;
    if (config->max_frame_bytes != 0u &&
        storage_bytes / config->max_frame_bytes !=
            config->frame_queue_capacity) {
        return TURBO_RTC_CLIENT_EINVAL;
    }

    subscriber =
        (turbo_rtc_subscriber_t *)calloc(1, sizeof(*subscriber));
    if (!subscriber) {
        return TURBO_RTC_CLIENT_ENOMEM;
    }

    subscriber->stun_server_count = config->stun_server_count;
    subscriber->turn_server_count = config->turn_server_count;

    subscriber->whep_base_url = subscriber_strdup(config->whep_base_url);
    subscriber->whep_path = subscriber_strdup(config->whep_path);
    if (!subscriber->whep_base_url || !subscriber->whep_path ||
        !subscriber_optional_copy(
            config->bearer_token, &subscriber->bearer_token) ||
        !subscriber_optional_copy(config->ca_file, &subscriber->ca_file) ||
        !subscriber_optional_copy(
            config->cert_file, &subscriber->cert_file) ||
        !subscriber_optional_copy(
            config->key_file, &subscriber->key_file) ||
        !subscriber_optional_copy(
            config->key_password, &subscriber->key_password) ||
        !subscriber_optional_copy(
            config->server_name, &subscriber->server_name)) {
        subscriber_free(subscriber);
        return TURBO_RTC_CLIENT_ENOMEM;
    }

    subscriber->stun_servers = subscriber_copy_string_array(
        config->stun_servers, config->stun_server_count);
    subscriber->turn_servers = subscriber_copy_string_array(
        config->turn_servers, config->turn_server_count);
    if ((config->stun_server_count != 0u && !subscriber->stun_servers) ||
        (config->turn_server_count != 0u && !subscriber->turn_servers)) {
        subscriber_free(subscriber);
        return TURBO_RTC_CLIENT_ENOMEM;
    }

    subscriber->frame_storage = (uint8_t *)malloc(storage_bytes);
    subscriber->delivery_buffer =
        (uint8_t *)malloc(config->max_frame_bytes);
    subscriber->frame_lengths = (size_t *)calloc(
        config->frame_queue_capacity, sizeof(*subscriber->frame_lengths));
    subscriber->frame_timestamps = (uint64_t *)calloc(
        config->frame_queue_capacity, sizeof(*subscriber->frame_timestamps));
    if (!subscriber->frame_storage || !subscriber->delivery_buffer ||
        !subscriber->frame_lengths || !subscriber->frame_timestamps) {
        subscriber_free(subscriber);
        return TURBO_RTC_CLIENT_ENOMEM;
    }

    subscriber->request_timeout_ms = config->request_timeout_ms;
    subscriber->connect_timeout_ms = config->connect_timeout_ms;
    subscriber->allow_plaintext_loopback =
        config->allow_plaintext_loopback;
    subscriber->allow_loopback = config->allow_loopback;
    subscriber->sample_rate = config->sample_rate;
    subscriber->channels = config->channels;
    subscriber->frame_queue_capacity = config->frame_queue_capacity;
    subscriber->max_frame_bytes = config->max_frame_bytes;
    subscriber->on_audio = config->on_audio;
    subscriber->audio_context = config->audio_context;
    subscriber->state = TURBO_RTC_CLIENT_CREATED;
    subscriber->peer_state = TURBO_PEER_STATE_NEW;

    salts_mutex_init(&subscriber->lock);
    subscriber->lock_initialized = 1;

    *out_subscriber = subscriber;
    return TURBO_RTC_CLIENT_OK;
}

turbo_rtc_client_status_t turbo_rtc_subscriber_prepare(
    turbo_rtc_subscriber_t *subscriber) {
    turbo_http_media_client_config_t http_config =
        TURBO_HTTP_MEDIA_CLIENT_CONFIG_INIT;
    turbo_peer_config_t peer_config;
    turbo_peer_callbacks_t callbacks;
    turbo_media_track_config_t track_config;

    if (!subscriber) {
        return TURBO_RTC_CLIENT_EINVAL;
    }
    if (subscriber->state != TURBO_RTC_CLIENT_CREATED) {
        return TURBO_RTC_CLIENT_ESTATE;
    }

    http_config.base_url = subscriber->whep_base_url;
    http_config.media_token = subscriber->bearer_token;
    http_config.ca_file = subscriber->ca_file;
    http_config.cert_file = subscriber->cert_file;
    http_config.key_file = subscriber->key_file;
    http_config.key_password = subscriber->key_password;
    http_config.server_name = subscriber->server_name;
    http_config.timeout_ms = subscriber->request_timeout_ms;
    http_config.allow_plaintext_loopback =
        subscriber->allow_plaintext_loopback;
    if (turbo_http_media_client_create(
            &http_config, &subscriber->http) != 0) {
        subscriber->state = TURBO_RTC_CLIENT_FAILED;
        return TURBO_RTC_CLIENT_EHTTP;
    }

    memset(&peer_config, 0, sizeof(peer_config));
    peer_config.stun_servers =
        (const char **)subscriber->stun_servers;
    peer_config.stun_server_count = (int)subscriber->stun_server_count;
    peer_config.turn_servers =
        (const char **)subscriber->turn_servers;
    peer_config.turn_server_count = (int)subscriber->turn_server_count;
    peer_config.allow_loopback = subscriber->allow_loopback;
    peer_config.disable_datachannel = 1;
    peer_config.user_data = subscriber;

    salts_mutex_lock(&subscriber->lock);
    subscriber->stopping = 0;
    subscriber->accept_frames = 0;
    subscriber->peer_state = TURBO_PEER_STATE_NEW;
    subscriber_reset_queue_locked(subscriber);
    salts_mutex_unlock(&subscriber->lock);

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.on_state_change = subscriber_on_state_change;
    callbacks.on_track = subscriber_on_track;
    subscriber->pc =
        turbo_peer_connection_create(&peer_config, &callbacks);
    if (!subscriber->pc) {
        subscriber->state = TURBO_RTC_CLIENT_FAILED;
        return TURBO_RTC_CLIENT_EPEER;
    }

    memset(&track_config, 0, sizeof(track_config));
    track_config.type = TURBO_RTC_MEDIA_TRACK_AUDIO;
    track_config.direction = TURBO_MEDIA_DIRECTION_RECVONLY;
    track_config.codec = TURBO_CODEC_OPUS;
    track_config.audio.sample_rate = (int)subscriber->sample_rate;
    track_config.audio.channels = (int)subscriber->channels;
    track_config.audio.frame_size_ms = 20;

    subscriber->audio_track =
        turbo_peer_connection_add_track_ex(
            subscriber->pc, &track_config);
    if (!subscriber->audio_track) {
        subscriber->state = TURBO_RTC_CLIENT_FAILED;
        return TURBO_RTC_CLIENT_EPEER;
    }
    turbo_media_track_set_user_data(
        subscriber->audio_track, subscriber);
    turbo_media_track_on_frame(
        subscriber->audio_track, subscriber_on_frame);

    subscriber->state = TURBO_RTC_CLIENT_PREPARED;
    return TURBO_RTC_CLIENT_OK;
}

turbo_rtc_client_status_t turbo_rtc_subscriber_start(
    turbo_rtc_subscriber_t *subscriber) {
    char offer[TURBO_HTTP_MEDIA_MAX_SDP];
    char minimal_offer[TURBO_HTTP_MEDIA_MAX_SDP];
    char fragment[4096];
    char ufrag[64];
    char password[96];
    turbo_http_media_response_t response;
    turbo_http_media_response_t trickle;
    size_t used;
    int prefix_length;
    int status;
    turbo_peer_state_t peer_state;

    if (!subscriber) {
        return TURBO_RTC_CLIENT_EINVAL;
    }
    if (subscriber->state != TURBO_RTC_CLIENT_PREPARED ||
        !subscriber->http || !subscriber->pc ||
        !subscriber->audio_track) {
        return TURBO_RTC_CLIENT_ESTATE;
    }

    subscriber->state = TURBO_RTC_CLIENT_NEGOTIATING;
    if (turbo_peer_connection_create_offer(
            subscriber->pc, offer, sizeof(offer)) <= 0 ||
        !turbo_http_media_sdp_attr_value(
            offer, "a=ice-ufrag:", ufrag, sizeof(ufrag)) ||
        !turbo_http_media_sdp_attr_value(
            offer, "a=ice-pwd:", password, sizeof(password))) {
        subscriber->state = TURBO_RTC_CLIENT_FAILED;
        return TURBO_RTC_CLIENT_ESDP;
    }

    turbo_http_media_sdp_build_minimal_audio_offer(
        offer, minimal_offer, sizeof(minimal_offer),
        (int)subscriber->sample_rate, "recvonly");
    if (minimal_offer[0] == '\0') {
        subscriber->state = TURBO_RTC_CLIENT_FAILED;
        return TURBO_RTC_CLIENT_ESDP;
    }

    memset(&response, 0, sizeof(response));
    status = turbo_http_media_request(
        subscriber->http, "POST", subscriber->whep_path,
        "application/sdp", NULL, minimal_offer, &response);
    subscriber->last_http_status = response.status;
    if (status != 0 || response.status != 201 ||
        response.location[0] == '\0' ||
        response.etag[0] == '\0' ||
        response.body[0] == '\0') {
        subscriber->state = TURBO_RTC_CLIENT_FAILED;
        return TURBO_RTC_CLIENT_EHTTP;
    }

    if (turbo_peer_connection_set_remote_description(
            subscriber->pc, "answer", response.body) != 0) {
        (void)subscriber_delete_remote(
            subscriber, response.location);
        subscriber->state = TURBO_RTC_CLIENT_FAILED;
        return TURBO_RTC_CLIENT_ESDP;
    }

    prefix_length = snprintf(
        fragment, sizeof(fragment),
        "a=ice-ufrag:%s\r\na=ice-pwd:%s\r\n",
        ufrag, password);
    if (prefix_length < 0 ||
        (size_t)prefix_length >= sizeof(fragment)) {
        (void)subscriber_delete_remote(
            subscriber, response.location);
        subscriber->state = TURBO_RTC_CLIENT_FAILED;
        return TURBO_RTC_CLIENT_ESDP;
    }
    used = (size_t)prefix_length;
    turbo_http_media_sdp_append_candidates(
        offer, fragment + used, sizeof(fragment) - used);

    memset(&trickle, 0, sizeof(trickle));
    status = turbo_http_media_request(
        subscriber->http, "PATCH", response.location,
        "application/trickle-ice-sdpfrag", response.etag,
        fragment, &trickle);
    subscriber->last_http_status = trickle.status;
    if (status != 0 ||
        (trickle.status != 200 && trickle.status != 204)) {
        (void)subscriber_delete_remote(
            subscriber, response.location);
        subscriber->state = TURBO_RTC_CLIENT_FAILED;
        return TURBO_RTC_CLIENT_EHTTP;
    }
    if (trickle.body[0] != '\0' &&
        turbo_peer_connection_apply_remote_ice_sdpfrag(
            subscriber->pc, trickle.body,
            strlen(trickle.body)) < 0) {
        (void)subscriber_delete_remote(
            subscriber, response.location);
        subscriber->state = TURBO_RTC_CLIENT_FAILED;
        return TURBO_RTC_CLIENT_ESDP;
    }

    memcpy(subscriber->session_location, response.location,
           strlen(response.location) + 1u);
    memcpy(subscriber->session_etag, response.etag,
           strlen(response.etag) + 1u);
    subscriber->connect_started_ms = salts_monotonic_ms();

    salts_mutex_lock(&subscriber->lock);
    subscriber->accept_frames = 1;
    subscriber->queue_overflowed = 0;
    subscriber->frame_error = 0;
    peer_state = subscriber->peer_state;
    salts_mutex_unlock(&subscriber->lock);

    if (peer_state == TURBO_PEER_STATE_FAILED ||
        peer_state == TURBO_PEER_STATE_DISCONNECTED ||
        peer_state == TURBO_PEER_STATE_CLOSED) {
        (void)subscriber_delete_remote(
            subscriber, response.location);
        subscriber->state = TURBO_RTC_CLIENT_FAILED;
        return TURBO_RTC_CLIENT_EPEER;
    }
    subscriber->state =
        peer_state == TURBO_PEER_STATE_CONNECTED
            ? TURBO_RTC_CLIENT_CONNECTED
            : TURBO_RTC_CLIENT_CONNECTING;
    return TURBO_RTC_CLIENT_OK;
}

static turbo_rtc_client_status_t subscriber_drain_audio(
    turbo_rtc_subscriber_t *subscriber) {
    for (;;) {
        size_t length;
        uint64_t timestamp;
        size_t slot_offset;
        int callback_status;

        salts_mutex_lock(&subscriber->lock);
        if (subscriber->queue_count == 0u) {
            salts_mutex_unlock(&subscriber->lock);
            return TURBO_RTC_CLIENT_OK;
        }

        length = subscriber->frame_lengths[subscriber->queue_head];
        timestamp =
            subscriber->frame_timestamps[subscriber->queue_head];
        slot_offset =
            subscriber->queue_head * subscriber->max_frame_bytes;
        memcpy(subscriber->delivery_buffer,
               subscriber->frame_storage + slot_offset, length);

        subscriber->queue_head =
            (subscriber->queue_head + 1u) %
            subscriber->frame_queue_capacity;
        subscriber->queue_count--;
        subscriber->queue_bytes -= length;
        salts_mutex_unlock(&subscriber->lock);

        callback_status = subscriber->on_audio(
            subscriber->audio_context,
            subscriber->delivery_buffer,
            length,
            subscriber->sample_rate,
            subscriber->channels,
            timestamp);

        salts_mutex_lock(&subscriber->lock);
        if (callback_status == 0) {
            subscriber->frames_delivered++;
        } else {
            subscriber->frames_rejected++;
        }
        salts_mutex_unlock(&subscriber->lock);

        if (callback_status != 0) {
            salts_mutex_lock(&subscriber->lock);
            subscriber->accept_frames = 0;
            salts_mutex_unlock(&subscriber->lock);
            subscriber->state = TURBO_RTC_CLIENT_FAILED;
            return TURBO_RTC_CLIENT_EIO;
        }
    }
}

turbo_rtc_client_status_t turbo_rtc_subscriber_poll(
    turbo_rtc_subscriber_t *subscriber) {
    turbo_peer_state_t peer_state;
    int queue_overflowed;
    int frame_error;
    uint64_t now;

    if (!subscriber) {
        return TURBO_RTC_CLIENT_EINVAL;
    }
    if ((subscriber->state != TURBO_RTC_CLIENT_CONNECTING &&
         subscriber->state != TURBO_RTC_CLIENT_CONNECTED) ||
        !subscriber->pc) {
        return TURBO_RTC_CLIENT_ESTATE;
    }

    turbo_peer_connection_poll(subscriber->pc);

    salts_mutex_lock(&subscriber->lock);
    peer_state = subscriber->peer_state;
    queue_overflowed = subscriber->queue_overflowed;
    frame_error = subscriber->frame_error;
    salts_mutex_unlock(&subscriber->lock);

    if (queue_overflowed || frame_error ||
        peer_state == TURBO_PEER_STATE_FAILED ||
        peer_state == TURBO_PEER_STATE_DISCONNECTED ||
        peer_state == TURBO_PEER_STATE_CLOSED) {
        turbo_rtc_client_status_t failure_status =
            queue_overflowed
                ? TURBO_RTC_CLIENT_EQUEUE
                : (frame_error ? TURBO_RTC_CLIENT_EIO
                               : TURBO_RTC_CLIENT_EPEER);
        salts_mutex_lock(&subscriber->lock);
        subscriber->accept_frames = 0;
        salts_mutex_unlock(&subscriber->lock);
        subscriber->state = TURBO_RTC_CLIENT_FAILED;
        return failure_status;
    }
    if (peer_state == TURBO_PEER_STATE_CONNECTED) {
        subscriber->state = TURBO_RTC_CLIENT_CONNECTED;
    }

    if (subscriber->state == TURBO_RTC_CLIENT_CONNECTING) {
        now = salts_monotonic_ms();
        if (now - subscriber->connect_started_ms >=
            subscriber->connect_timeout_ms) {
            salts_mutex_lock(&subscriber->lock);
            subscriber->accept_frames = 0;
            salts_mutex_unlock(&subscriber->lock);
            subscriber->state = TURBO_RTC_CLIENT_FAILED;
            return TURBO_RTC_CLIENT_ETIMEDOUT;
        }
        return TURBO_RTC_CLIENT_OK;
    }

    return subscriber_drain_audio(subscriber);
}

turbo_rtc_client_status_t turbo_rtc_subscriber_stop(
    turbo_rtc_subscriber_t *subscriber) {
    turbo_rtc_client_status_t remote_status =
        TURBO_RTC_CLIENT_OK;

    if (!subscriber) {
        return TURBO_RTC_CLIENT_EINVAL;
    }
    if (subscriber->state != TURBO_RTC_CLIENT_PREPARED &&
        subscriber->state != TURBO_RTC_CLIENT_NEGOTIATING &&
        subscriber->state != TURBO_RTC_CLIENT_CONNECTING &&
        subscriber->state != TURBO_RTC_CLIENT_CONNECTED &&
        subscriber->state != TURBO_RTC_CLIENT_FAILED) {
        return TURBO_RTC_CLIENT_ESTATE;
    }

    subscriber->state = TURBO_RTC_CLIENT_DRAINING;
    salts_mutex_lock(&subscriber->lock);
    subscriber->accept_frames = 0;
    salts_mutex_unlock(&subscriber->lock);

    if (subscriber->session_location[0] != '\0') {
        remote_status = subscriber_delete_remote(
            subscriber, subscriber->session_location);
    }

    subscriber_clear_peer(subscriber);
    if (remote_status != TURBO_RTC_CLIENT_OK) {
        subscriber->state = TURBO_RTC_CLIENT_FAILED;
        return remote_status;
    }

    subscriber->session_location[0] = '\0';
    subscriber->session_etag[0] = '\0';
    subscriber->state = TURBO_RTC_CLIENT_CLOSED;
    return TURBO_RTC_CLIENT_OK;
}

turbo_rtc_client_status_t turbo_rtc_subscriber_snapshot(
    const turbo_rtc_subscriber_t *subscriber,
    turbo_rtc_subscriber_snapshot_t *snapshot) {
    turbo_rtc_subscriber_t *mutable_subscriber;
    if (!subscriber || !snapshot ||
        snapshot->size < sizeof(*snapshot)) {
        return TURBO_RTC_CLIENT_EINVAL;
    }

    mutable_subscriber =
        (turbo_rtc_subscriber_t *)subscriber;
    snapshot->state = subscriber->state;
    snapshot->connected =
        subscriber->state == TURBO_RTC_CLIENT_CONNECTED ? 1 : 0;
    snapshot->session_active =
        subscriber->session_location[0] != '\0' ? 1 : 0;
    snapshot->last_http_status = subscriber->last_http_status;
    snapshot->connect_started_ms =
        subscriber->connect_started_ms;

    salts_mutex_lock(&mutable_subscriber->lock);
    snapshot->frames_received =
        subscriber->frames_received;
    snapshot->frames_delivered =
        subscriber->frames_delivered;
    snapshot->frames_rejected =
        subscriber->frames_rejected;
    snapshot->queue_items =
        (uint32_t)subscriber->queue_count;
    snapshot->queue_high_water =
        (uint32_t)subscriber->queue_high_water;
    snapshot->queue_bytes =
        (uint64_t)subscriber->queue_bytes;
    snapshot->queue_bytes_high_water =
        (uint64_t)subscriber->queue_bytes_high_water;
    salts_mutex_unlock(&mutable_subscriber->lock);
    return TURBO_RTC_CLIENT_OK;
}

turbo_rtc_client_status_t turbo_rtc_subscriber_destroy(
    turbo_rtc_subscriber_t *subscriber) {
    if (!subscriber) {
        return TURBO_RTC_CLIENT_EINVAL;
    }
    if (subscriber->state == TURBO_RTC_CLIENT_NEGOTIATING ||
        subscriber->state == TURBO_RTC_CLIENT_CONNECTING ||
        subscriber->state == TURBO_RTC_CLIENT_CONNECTED ||
        subscriber->state == TURBO_RTC_CLIENT_DRAINING ||
        subscriber->session_location[0] != '\0') {
        return TURBO_RTC_CLIENT_ESTATE;
    }
    subscriber_free(subscriber);
    return TURBO_RTC_CLIENT_OK;
}
