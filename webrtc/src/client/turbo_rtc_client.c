#include "turbo_rtc_client.h"

#include "turbo_http_media_client.h"
#include "turbo_peer_connection.h"

#include <salts/clock.h>

#include <limits.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct turbo_rtc_client_s {
    turbo_rtc_client_state_t state;

    char *whip_base_url;
    char *whip_path;
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
    uint32_t bitrate;
    uint32_t frame_size_ms;
    int enable_fec;
    int enable_dtx;

    turbo_http_media_client_t *http;
    turbo_peer_connection_t *pc;
    turbo_media_track_t *audio_track;

    char session_location[512];
    char session_etag[128];
    int last_http_status;
    uint64_t frames_sent;
    uint64_t connect_started_ms;
    _Atomic int peer_state;
};

static char *turbo_rtc_client_strdup(const char *value) {
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

static int turbo_rtc_client_bool_valid(int value) {
    return value == 0 || value == 1;
}

static int turbo_rtc_client_sample_rate_valid(uint32_t sample_rate) {
    return sample_rate == 8000u || sample_rate == 16000u ||
           sample_rate == 24000u || sample_rate == 48000u;
}

static int turbo_rtc_client_frame_size_valid(uint32_t frame_size_ms) {
    return frame_size_ms == 10u || frame_size_ms == 20u ||
           frame_size_ms == 40u || frame_size_ms == 60u;
}

static int turbo_rtc_client_string_present(const char *value) {
    return value && value[0] != '\0';
}

static int turbo_rtc_client_config_valid(
    const turbo_rtc_client_config_t *config) {
    size_t i;
    if (!config || config->size < sizeof(*config) ||
        !turbo_rtc_client_string_present(config->whip_base_url) ||
        !turbo_rtc_client_string_present(config->whip_path) ||
        config->whip_path[0] != '/' ||
        config->request_timeout_ms == 0u ||
        config->connect_timeout_ms == 0u ||
        !turbo_rtc_client_bool_valid(config->allow_plaintext_loopback) ||
        !turbo_rtc_client_bool_valid(config->allow_loopback) ||
        !turbo_rtc_client_sample_rate_valid(config->sample_rate) ||
        config->bitrate > (uint32_t)INT_MAX ||
        (config->channels != 1u && config->channels != 2u) ||
        !turbo_rtc_client_frame_size_valid(config->frame_size_ms) ||
        !turbo_rtc_client_bool_valid(config->enable_fec) ||
        !turbo_rtc_client_bool_valid(config->enable_dtx) ||
        config->stun_server_count > TURBO_RTC_CLIENT_MAX_ICE_SERVERS ||
        config->turn_server_count > TURBO_RTC_CLIENT_MAX_ICE_SERVERS ||
        (config->stun_server_count != 0u && !config->stun_servers) ||
        (config->turn_server_count != 0u && !config->turn_servers)) {
        return 0;
    }
    for (i = 0u; i < config->stun_server_count; ++i) {
        if (!turbo_rtc_client_string_present(config->stun_servers[i])) {
            return 0;
        }
    }
    for (i = 0u; i < config->turn_server_count; ++i) {
        if (!turbo_rtc_client_string_present(config->turn_servers[i])) {
            return 0;
        }
    }
    return 1;
}

static void turbo_rtc_client_free_string_array(char **items, size_t count) {
    size_t i;
    if (!items) {
        return;
    }
    for (i = 0u; i < count; ++i) {
        free(items[i]);
    }
    free(items);
}

static char **turbo_rtc_client_copy_string_array(
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
        copy[i] = turbo_rtc_client_strdup(items[i]);
        if (!copy[i]) {
            turbo_rtc_client_free_string_array(copy, count);
            return NULL;
        }
    }
    return copy;
}

static void turbo_rtc_client_clear_peer(turbo_rtc_client_t *client) {
    if (!client) {
        return;
    }
    if (client->audio_track) {
        turbo_media_track_stop(client->audio_track);
        client->audio_track = NULL;
    }
    if (client->pc) {
        turbo_peer_connection_destroy(client->pc);
        client->pc = NULL;
    }
}

static void turbo_rtc_client_state_change(
    turbo_peer_connection_t *pc, turbo_peer_state_t state, void *user_data) {
    turbo_rtc_client_t *client = (turbo_rtc_client_t *)user_data;
    (void)pc;
    if (!client) {
        return;
    }
    if (state == TURBO_PEER_STATE_CONNECTED) {
        if (!client->audio_track ||
            turbo_media_track_start(client->audio_track) != 0) {
            atomic_store_explicit(
                &client->peer_state, TURBO_PEER_STATE_FAILED,
                memory_order_release);
            return;
        }
    }
    atomic_store_explicit(
        &client->peer_state, (int)state, memory_order_release);
}

static turbo_rtc_client_status_t turbo_rtc_client_delete_remote(
    turbo_rtc_client_t *client, const char *location) {
    turbo_http_media_response_t response;
    int status;
    if (!client || !location || location[0] == '\0') {
        return TURBO_RTC_CLIENT_OK;
    }
    memset(&response, 0, sizeof(response));
    status = turbo_http_media_request(
        client->http, "DELETE", location, NULL, NULL, NULL, &response);
    client->last_http_status = response.status;
    if (status != 0 ||
        (response.status != 200 && response.status != 204 &&
         response.status != 404)) {
        return TURBO_RTC_CLIENT_EHTTP;
    }
    return TURBO_RTC_CLIENT_OK;
}

static void turbo_rtc_client_free(turbo_rtc_client_t *client) {
    if (!client) {
        return;
    }
    turbo_rtc_client_clear_peer(client);
    turbo_http_media_client_destroy(client->http);
    client->http = NULL;
    turbo_rtc_client_free_string_array(
        client->stun_servers, client->stun_server_count);
    turbo_rtc_client_free_string_array(
        client->turn_servers, client->turn_server_count);
    free(client->server_name);
    free(client->key_password);
    free(client->key_file);
    free(client->cert_file);
    free(client->ca_file);
    free(client->bearer_token);
    free(client->whip_path);
    free(client->whip_base_url);
    free(client);
}

void turbo_rtc_client_config_init(turbo_rtc_client_config_t *config) {
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->size = sizeof(*config);
    config->request_timeout_ms = 5000u;
    config->connect_timeout_ms = 10000u;
    config->sample_rate = 48000u;
    config->channels = 1u;
    config->frame_size_ms = 20u;
    config->enable_fec = 1;
}

void turbo_rtc_client_snapshot_init(turbo_rtc_client_snapshot_t *snapshot) {
    if (!snapshot) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->size = sizeof(*snapshot);
}

turbo_rtc_client_status_t turbo_rtc_client_create(
    const turbo_rtc_client_config_t *config,
    turbo_rtc_client_t **out_client) {
    turbo_rtc_client_t *client;
    if (!out_client) {
        return TURBO_RTC_CLIENT_EINVAL;
    }
    *out_client = NULL;
    if (!turbo_rtc_client_config_valid(config)) {
        return TURBO_RTC_CLIENT_EINVAL;
    }

    client = (turbo_rtc_client_t *)calloc(1u, sizeof(*client));
    if (!client) {
        return TURBO_RTC_CLIENT_ENOMEM;
    }
    client->whip_base_url = turbo_rtc_client_strdup(config->whip_base_url);
    client->whip_path = turbo_rtc_client_strdup(config->whip_path);
    client->bearer_token = turbo_rtc_client_strdup(config->bearer_token);
    client->ca_file = turbo_rtc_client_strdup(config->ca_file);
    client->cert_file = turbo_rtc_client_strdup(config->cert_file);
    client->key_file = turbo_rtc_client_strdup(config->key_file);
    client->key_password = turbo_rtc_client_strdup(config->key_password);
    client->server_name = turbo_rtc_client_strdup(config->server_name);
    client->stun_servers = turbo_rtc_client_copy_string_array(
        config->stun_servers, config->stun_server_count);
    client->turn_servers = turbo_rtc_client_copy_string_array(
        config->turn_servers, config->turn_server_count);
    if (!client->whip_base_url || !client->whip_path ||
        (config->bearer_token && !client->bearer_token) ||
        (config->ca_file && !client->ca_file) ||
        (config->cert_file && !client->cert_file) ||
        (config->key_file && !client->key_file) ||
        (config->key_password && !client->key_password) ||
        (config->server_name && !client->server_name) ||
        (config->stun_server_count && !client->stun_servers) ||
        (config->turn_server_count && !client->turn_servers)) {
        turbo_rtc_client_free(client);
        return TURBO_RTC_CLIENT_ENOMEM;
    }

    client->stun_server_count = config->stun_server_count;
    client->turn_server_count = config->turn_server_count;
    client->request_timeout_ms = config->request_timeout_ms;
    client->connect_timeout_ms = config->connect_timeout_ms;
    client->allow_plaintext_loopback = config->allow_plaintext_loopback;
    client->allow_loopback = config->allow_loopback;
    client->sample_rate = config->sample_rate;
    client->channels = config->channels;
    client->bitrate = config->bitrate;
    client->frame_size_ms = config->frame_size_ms;
    client->enable_fec = config->enable_fec;
    client->enable_dtx = config->enable_dtx;
    client->state = TURBO_RTC_CLIENT_CREATED;
    *out_client = client;
    return TURBO_RTC_CLIENT_OK;
}

turbo_rtc_client_status_t turbo_rtc_client_prepare(
    turbo_rtc_client_t *client) {
    turbo_http_media_client_config_t http_config =
        TURBO_HTTP_MEDIA_CLIENT_CONFIG_INIT;
    turbo_peer_config_t peer_config;
    turbo_peer_callbacks_t callbacks;
    turbo_media_track_config_t track_config;

    if (!client) {
        return TURBO_RTC_CLIENT_EINVAL;
    }
    if (client->state != TURBO_RTC_CLIENT_CREATED) {
        return TURBO_RTC_CLIENT_ESTATE;
    }

    http_config.base_url = client->whip_base_url;
    http_config.media_token = client->bearer_token;
    http_config.ca_file = client->ca_file;
    http_config.cert_file = client->cert_file;
    http_config.key_file = client->key_file;
    http_config.key_password = client->key_password;
    http_config.server_name = client->server_name;
    http_config.timeout_ms = client->request_timeout_ms;
    http_config.allow_plaintext_loopback = client->allow_plaintext_loopback;
    if (turbo_http_media_client_create(&http_config, &client->http) != 0) {
        client->state = TURBO_RTC_CLIENT_FAILED;
        return TURBO_RTC_CLIENT_EHTTP;
    }

    memset(&peer_config, 0, sizeof(peer_config));
    peer_config.stun_servers = (const char **)client->stun_servers;
    peer_config.stun_server_count = (int)client->stun_server_count;
    peer_config.turn_servers = (const char **)client->turn_servers;
    peer_config.turn_server_count = (int)client->turn_server_count;
    peer_config.allow_loopback = client->allow_loopback;
    peer_config.disable_datachannel = 1;
    peer_config.user_data = client;

    atomic_store_explicit(
        &client->peer_state, TURBO_PEER_STATE_NEW, memory_order_release);

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.on_state_change = turbo_rtc_client_state_change;
    client->pc = turbo_peer_connection_create(&peer_config, &callbacks);
    if (!client->pc) {
        client->state = TURBO_RTC_CLIENT_FAILED;
        return TURBO_RTC_CLIENT_EPEER;
    }

    memset(&track_config, 0, sizeof(track_config));
    track_config.type = TURBO_RTC_MEDIA_TRACK_AUDIO;
    track_config.direction = TURBO_MEDIA_DIRECTION_SENDONLY;
    track_config.codec = TURBO_CODEC_OPUS;
    track_config.audio.sample_rate = (int)client->sample_rate;
    track_config.audio.channels = (int)client->channels;
    track_config.audio.bitrate = (int)client->bitrate;
    track_config.audio.frame_size_ms = (int)client->frame_size_ms;
    track_config.audio.enable_fec = client->enable_fec;
    track_config.audio.enable_dtx = client->enable_dtx;
    client->audio_track =
        turbo_peer_connection_add_track_ex(client->pc, &track_config);
    if (!client->audio_track) {
        client->state = TURBO_RTC_CLIENT_FAILED;
        return TURBO_RTC_CLIENT_EPEER;
    }

    client->state = TURBO_RTC_CLIENT_PREPARED;
    return TURBO_RTC_CLIENT_OK;
}

turbo_rtc_client_status_t turbo_rtc_client_start(
    turbo_rtc_client_t *client) {
    char offer[TURBO_HTTP_MEDIA_MAX_SDP];
    char minimal_offer[TURBO_HTTP_MEDIA_MAX_SDP];
    char fragment[4096];
    char ufrag[64];
    char password[96];
    char fingerprint[256];
    turbo_http_media_response_t response;
    turbo_http_media_response_t trickle;
    size_t used;
    int status;
    int fragment_length;

    if (!client) {
        return TURBO_RTC_CLIENT_EINVAL;
    }
    if (client->state != TURBO_RTC_CLIENT_PREPARED ||
        !client->http || !client->pc || !client->audio_track) {
        return TURBO_RTC_CLIENT_ESTATE;
    }

    client->state = TURBO_RTC_CLIENT_NEGOTIATING;
    if (turbo_peer_connection_create_offer(
            client->pc, offer, sizeof(offer)) <= 0 ||
        !turbo_http_media_sdp_attr_value(
            offer, "a=ice-ufrag:", ufrag, sizeof(ufrag)) ||
        !turbo_http_media_sdp_attr_value(
            offer, "a=ice-pwd:", password, sizeof(password)) ||
        !turbo_http_media_sdp_attr_value(
            offer, "a=fingerprint:sha-256 ", fingerprint,
            sizeof(fingerprint))) {
        client->state = TURBO_RTC_CLIENT_FAILED;
        return TURBO_RTC_CLIENT_ESDP;
    }

    turbo_http_media_sdp_build_minimal_audio_offer(
        offer, minimal_offer, sizeof(minimal_offer),
        (int)client->sample_rate, "sendonly");
    if (minimal_offer[0] == '\0') {
        client->state = TURBO_RTC_CLIENT_FAILED;
        return TURBO_RTC_CLIENT_ESDP;
    }

    memset(&response, 0, sizeof(response));
    status = turbo_http_media_request(
        client->http, "POST", client->whip_path, "application/sdp", NULL,
        minimal_offer, &response);
    client->last_http_status = response.status;
    if (status != 0 || response.status != 201 ||
        response.location[0] == '\0' || response.etag[0] == '\0' ||
        response.body[0] == '\0') {
        client->state = TURBO_RTC_CLIENT_FAILED;
        return TURBO_RTC_CLIENT_EHTTP;
    }

    if (turbo_peer_connection_set_remote_description(
            client->pc, "answer", response.body) != 0) {
        (void)turbo_rtc_client_delete_remote(client, response.location);
        client->state = TURBO_RTC_CLIENT_FAILED;
        return TURBO_RTC_CLIENT_ESDP;
    }

    fragment_length = snprintf(
        fragment, sizeof(fragment),
        "a=ice-ufrag:%s\r\na=ice-pwd:%s\r\n", ufrag, password);
    if (fragment_length < 0 ||
        (size_t)fragment_length >= sizeof(fragment)) {
        (void)turbo_rtc_client_delete_remote(client, response.location);
        client->state = TURBO_RTC_CLIENT_FAILED;
        return TURBO_RTC_CLIENT_ESDP;
    }
    used = (size_t)fragment_length;
    turbo_http_media_sdp_append_candidates(
        offer, fragment + used, sizeof(fragment) - used);

    memset(&trickle, 0, sizeof(trickle));
    status = turbo_http_media_request(
        client->http, "PATCH", response.location,
        "application/trickle-ice-sdpfrag", response.etag, fragment,
        &trickle);
    client->last_http_status = trickle.status;
    if (status != 0 || (trickle.status != 200 && trickle.status != 204)) {
        (void)turbo_rtc_client_delete_remote(client, response.location);
        client->state = TURBO_RTC_CLIENT_FAILED;
        return TURBO_RTC_CLIENT_EHTTP;
    }
    if (trickle.body[0] != '\0' &&
        turbo_peer_connection_apply_remote_ice_sdpfrag(
            client->pc, trickle.body, strlen(trickle.body)) < 0) {
        (void)turbo_rtc_client_delete_remote(client, response.location);
        client->state = TURBO_RTC_CLIENT_FAILED;
        return TURBO_RTC_CLIENT_ESDP;
    }

    memcpy(client->session_location, response.location,
           strlen(response.location) + 1u);
    memcpy(client->session_etag, response.etag,
           strlen(response.etag) + 1u);
    client->connect_started_ms = salts_monotonic_ms();
    {
        turbo_peer_state_t peer_state = (turbo_peer_state_t)
            atomic_load_explicit(
                &client->peer_state, memory_order_acquire);
        if (peer_state == TURBO_PEER_STATE_FAILED ||
            peer_state == TURBO_PEER_STATE_DISCONNECTED ||
            peer_state == TURBO_PEER_STATE_CLOSED) {
            (void)turbo_rtc_client_delete_remote(client, response.location);
            client->state = TURBO_RTC_CLIENT_FAILED;
            return TURBO_RTC_CLIENT_EPEER;
        }
        client->state =
            peer_state == TURBO_PEER_STATE_CONNECTED
                ? TURBO_RTC_CLIENT_CONNECTED
                : TURBO_RTC_CLIENT_CONNECTING;
    }
    return TURBO_RTC_CLIENT_OK;
}

turbo_rtc_client_status_t turbo_rtc_client_restart_ice(
    turbo_rtc_client_t *client) {
    char fragment[TURBO_HTTP_MEDIA_MAX_SDP];
    turbo_http_media_response_t response;
    turbo_peer_state_t peer_state;
    int fragment_length;
    int status;
    int apply_result;

    if (!client) {
        return TURBO_RTC_CLIENT_EINVAL;
    }
    if ((client->state != TURBO_RTC_CLIENT_CONNECTED &&
         client->state != TURBO_RTC_CLIENT_FAILED) ||
        !client->http || !client->pc ||
        client->session_location[0] == '\0' ||
        client->session_etag[0] == '\0') {
        return TURBO_RTC_CLIENT_ESTATE;
    }

    peer_state = (turbo_peer_state_t)atomic_load_explicit(
        &client->peer_state, memory_order_acquire);
    if (peer_state == TURBO_PEER_STATE_CLOSED) {
        return TURBO_RTC_CLIENT_ESTATE;
    }

    client->state = TURBO_RTC_CLIENT_NEGOTIATING;
    if (turbo_peer_connection_restart_ice(client->pc) != 0) {
        client->state = TURBO_RTC_CLIENT_FAILED;
        return TURBO_RTC_CLIENT_EPEER;
    }

    fragment_length = turbo_peer_connection_create_local_ice_sdpfrag(
        client->pc, fragment, sizeof(fragment));
    if (fragment_length <= 0) {
        client->state = TURBO_RTC_CLIENT_FAILED;
        return TURBO_RTC_CLIENT_ESDP;
    }

    memset(&response, 0, sizeof(response));
    status = turbo_http_media_request(
        client->http, "PATCH", client->session_location,
        "application/trickle-ice-sdpfrag", client->session_etag,
        fragment, &response);
    client->last_http_status = response.status;
    if (status != 0 || response.status != 200 ||
        response.body[0] == '\0' || response.etag[0] == '\0' ||
        strcmp(response.etag, client->session_etag) == 0) {
        client->state = TURBO_RTC_CLIENT_FAILED;
        return TURBO_RTC_CLIENT_EHTTP;
    }

    apply_result = turbo_peer_connection_apply_remote_ice_sdpfrag(
        client->pc, response.body, strlen(response.body));
    if (apply_result != 1) {
        client->state = TURBO_RTC_CLIENT_FAILED;
        return TURBO_RTC_CLIENT_ESDP;
    }

    memcpy(client->session_etag, response.etag,
           strlen(response.etag) + 1u);
    client->connect_started_ms = salts_monotonic_ms();

    peer_state = (turbo_peer_state_t)atomic_load_explicit(
        &client->peer_state, memory_order_acquire);
    if (peer_state == TURBO_PEER_STATE_FAILED ||
        peer_state == TURBO_PEER_STATE_DISCONNECTED ||
        peer_state == TURBO_PEER_STATE_CLOSED) {
        client->state = TURBO_RTC_CLIENT_FAILED;
        return TURBO_RTC_CLIENT_EPEER;
    }
    client->state =
        peer_state == TURBO_PEER_STATE_CONNECTED
            ? TURBO_RTC_CLIENT_CONNECTED
            : TURBO_RTC_CLIENT_CONNECTING;
    return TURBO_RTC_CLIENT_OK;
}

turbo_rtc_client_status_t turbo_rtc_client_poll(
    turbo_rtc_client_t *client) {
    turbo_peer_state_t peer_state;
    uint64_t now;
    if (!client) {
        return TURBO_RTC_CLIENT_EINVAL;
    }
    if ((client->state != TURBO_RTC_CLIENT_CONNECTING &&
         client->state != TURBO_RTC_CLIENT_CONNECTED) ||
        !client->pc) {
        return TURBO_RTC_CLIENT_ESTATE;
    }

    turbo_peer_connection_poll(client->pc);
    peer_state = (turbo_peer_state_t)atomic_load_explicit(
        &client->peer_state, memory_order_acquire);
    if (peer_state == TURBO_PEER_STATE_FAILED ||
        peer_state == TURBO_PEER_STATE_DISCONNECTED ||
        peer_state == TURBO_PEER_STATE_CLOSED) {
        client->state = TURBO_RTC_CLIENT_FAILED;
        return TURBO_RTC_CLIENT_EPEER;
    }
    if (peer_state == TURBO_PEER_STATE_CONNECTED) {
        client->state = TURBO_RTC_CLIENT_CONNECTED;
    }
    if (client->state == TURBO_RTC_CLIENT_CONNECTING) {
        now = salts_monotonic_ms();
        if (now - client->connect_started_ms >= client->connect_timeout_ms) {
            client->state = TURBO_RTC_CLIENT_FAILED;
            return TURBO_RTC_CLIENT_ETIMEDOUT;
        }
    }
    return TURBO_RTC_CLIENT_OK;
}

turbo_rtc_client_status_t turbo_rtc_client_send_audio(
    turbo_rtc_client_t *client, const void *pcm, size_t len,
    uint64_t timestamp_us) {
    size_t frame_bytes;
    if (!client || !pcm || len == 0u) {
        return TURBO_RTC_CLIENT_EINVAL;
    }
    frame_bytes = (size_t)client->channels * 2u;
    if (frame_bytes == 0u || len % frame_bytes != 0u) {
        return TURBO_RTC_CLIENT_EINVAL;
    }
    if (client->state != TURBO_RTC_CLIENT_CONNECTED ||
        !client->audio_track) {
        return TURBO_RTC_CLIENT_ESTATE;
    }
    if (turbo_media_track_send_frame(
            client->audio_track, (const uint8_t *)pcm, len,
            timestamp_us) != 0) {
        return TURBO_RTC_CLIENT_EIO;
    }
    client->frames_sent++;
    return TURBO_RTC_CLIENT_OK;
}

turbo_rtc_client_status_t turbo_rtc_client_stop(
    turbo_rtc_client_t *client) {
    turbo_rtc_client_status_t remote_status = TURBO_RTC_CLIENT_OK;
    if (!client) {
        return TURBO_RTC_CLIENT_EINVAL;
    }
    if (client->state != TURBO_RTC_CLIENT_PREPARED &&
        client->state != TURBO_RTC_CLIENT_NEGOTIATING &&
        client->state != TURBO_RTC_CLIENT_CONNECTING &&
        client->state != TURBO_RTC_CLIENT_CONNECTED &&
        client->state != TURBO_RTC_CLIENT_FAILED) {
        return TURBO_RTC_CLIENT_ESTATE;
    }

    client->state = TURBO_RTC_CLIENT_DRAINING;
    if (client->session_location[0] != '\0') {
        remote_status = turbo_rtc_client_delete_remote(
            client, client->session_location);
    }
    turbo_rtc_client_clear_peer(client);
    if (remote_status != TURBO_RTC_CLIENT_OK) {
        client->state = TURBO_RTC_CLIENT_FAILED;
        return remote_status;
    }

    client->session_location[0] = '\0';
    client->session_etag[0] = '\0';
    client->state = TURBO_RTC_CLIENT_CLOSED;
    return TURBO_RTC_CLIENT_OK;
}

turbo_rtc_client_status_t turbo_rtc_client_snapshot(
    const turbo_rtc_client_t *client,
    turbo_rtc_client_snapshot_t *snapshot) {
    if (!client || !snapshot || snapshot->size < sizeof(*snapshot)) {
        return TURBO_RTC_CLIENT_EINVAL;
    }
    snapshot->state = client->state;
    snapshot->connected =
        client->state == TURBO_RTC_CLIENT_CONNECTED ? 1 : 0;
    snapshot->session_active =
        client->session_location[0] != '\0' ? 1 : 0;
    snapshot->last_http_status = client->last_http_status;
    snapshot->frames_sent = client->frames_sent;
    snapshot->connect_started_ms = client->connect_started_ms;
    return TURBO_RTC_CLIENT_OK;
}

turbo_rtc_client_status_t turbo_rtc_client_destroy(
    turbo_rtc_client_t *client) {
    if (!client) {
        return TURBO_RTC_CLIENT_EINVAL;
    }
    if (client->state == TURBO_RTC_CLIENT_NEGOTIATING ||
        client->state == TURBO_RTC_CLIENT_CONNECTING ||
        client->state == TURBO_RTC_CLIENT_CONNECTED ||
        client->state == TURBO_RTC_CLIENT_DRAINING ||
        client->session_location[0] != '\0') {
        return TURBO_RTC_CLIENT_ESTATE;
    }
    turbo_rtc_client_free(client);
    return TURBO_RTC_CLIENT_OK;
}
