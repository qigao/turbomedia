#include "turbo_media_rtc.h"
#include "rtc_peer_backend.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define TURBO_MEDIA_RTC_MAX_TRACKS 8
#define TURBO_MEDIA_RTC_RTP_HEADER_SIZE 12

typedef struct {
    int track_id;
    turbo_media_rtc_backend_track_t *track;
    char codec_name[TURBO_MEDIA_MAX_CODEC_NAME_LEN];
} turbo_media_rtc_track_binding_t;

struct turbo_media_rtc_session_s {
    turbo_media_server_runtime_t *runtime;
    turbo_media_rtc_role_t role;
    turbo_media_source_key_t key;
    const turbo_media_rtc_backend_ops_t *backend;
    turbo_media_rtc_backend_peer_t *peer;
    turbo_media_protocol_session_t *protocol_session;
    turbo_media_rtc_track_binding_t tracks[TURBO_MEDIA_RTC_MAX_TRACKS];
    size_t track_count;
    turbo_media_rtc_peer_state_t state;
    int last_error;
    int replay_cached;
    int remove_source_on_close;
    int closing;
    turbo_media_rtc_ice_candidate_cb on_ice_candidate;
    turbo_media_rtc_state_cb on_state;
    void *user_data;
};

static int rtc_backend_valid(const turbo_media_rtc_backend_ops_t *backend) {
    return backend && backend->create && backend->destroy &&
           backend->add_send_track && backend->set_remote_offer &&
           backend->create_answer && backend->add_ice_candidate &&
           backend->start_track && backend->send_rtp && backend->pump;
}

static int rtc_role_valid(turbo_media_rtc_role_t role) {
    return role == TURBO_MEDIA_RTC_ROLE_PUBLISHER ||
           role == TURBO_MEDIA_RTC_ROLE_PLAYER;
}

static int rtc_codec_equal(const char *lhs, const char *rhs) {
    unsigned char a;
    unsigned char b;

    if (!lhs || !rhs) return 0;
    while (*lhs && *rhs) {
        a = (unsigned char)*lhs++;
        b = (unsigned char)*rhs++;
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
        if (a != b) return 0;
    }
    return *lhs == '\0' && *rhs == '\0';
}

static const uint8_t *rtc_rtp_payload(const uint8_t *packet,
                                      size_t packet_len,
                                      size_t *payload_len) {
    size_t offset;
    size_t extension_words;

    if (!packet || packet_len < TURBO_MEDIA_RTC_RTP_HEADER_SIZE ||
        (packet[0] >> 6) != 2) {
        return NULL;
    }

    offset = TURBO_MEDIA_RTC_RTP_HEADER_SIZE + (size_t)(packet[0] & 0x0F) * 4;
    if (offset > packet_len) return NULL;

    if ((packet[0] & 0x10) != 0) {
        if (packet_len - offset < 4) return NULL;
        extension_words = ((size_t)packet[offset + 2] << 8) | packet[offset + 3];
        if (extension_words > (SIZE_MAX - offset - 4) / 4) return NULL;
        offset += 4 + extension_words * 4;
        if (offset > packet_len) return NULL;
    }

    *payload_len = packet_len - offset;
    return packet + offset;
}

static int rtc_h264_keyframe(const uint8_t *payload, size_t payload_len) {
    uint8_t nal_type;
    size_t offset;

    if (!payload || payload_len == 0) return 0;
    nal_type = (uint8_t)(payload[0] & 0x1F);
    if (nal_type == 5) return 1;
    if (nal_type == 28 && payload_len > 1) {
        return (payload[1] & 0x80) != 0 && (payload[1] & 0x1F) == 5;
    }
    if (nal_type != 24) return 0;

    offset = 1;
    while (payload_len - offset >= 2) {
        size_t nal_size = ((size_t)payload[offset] << 8) | payload[offset + 1];
        offset += 2;
        if (nal_size > payload_len - offset) return 0;
        if (nal_size > 0 && (payload[offset] & 0x1F) == 5) return 1;
        offset += nal_size;
    }
    return 0;
}

static int rtc_h265_keyframe(const uint8_t *payload, size_t payload_len) {
    uint8_t nal_type;

    if (!payload || payload_len < 2) return 0;
    nal_type = (uint8_t)((payload[0] >> 1) & 0x3F);
    if (nal_type >= 19 && nal_type <= 21) return 1;
    if (nal_type == 49 && payload_len > 2) {
        nal_type = (uint8_t)(payload[2] & 0x3F);
        return (payload[2] & 0x80) != 0 && nal_type >= 19 && nal_type <= 21;
    }
    return 0;
}

static int rtc_vp8_keyframe(const uint8_t *payload, size_t payload_len) {
    size_t offset = 1;
    uint8_t descriptor;

    if (!payload || payload_len < 2) return 0;
    descriptor = payload[0];
    if ((descriptor & 0x80) != 0) {
        uint8_t extension;
        if (offset >= payload_len) return 0;
        extension = payload[offset++];
        if ((extension & 0x80) != 0) {
            if (offset >= payload_len) return 0;
            if ((payload[offset++] & 0x80) != 0) {
                if (offset >= payload_len) return 0;
                offset++;
            }
        }
        if ((extension & 0x40) != 0) {
            if (offset >= payload_len) return 0;
            offset++;
        }
        if ((extension & 0x30) != 0) {
            if (offset >= payload_len) return 0;
            offset++;
        }
    }
    if (offset >= payload_len) return 0;
    return (descriptor & 0x10) != 0 && (payload[offset] & 0x01) == 0;
}

static int rtc_packet_is_keyframe(const char *codec_name,
                                  const uint8_t *packet,
                                  size_t packet_len) {
    const uint8_t *payload;
    size_t payload_len;

    payload = rtc_rtp_payload(packet, packet_len, &payload_len);
    if (!payload) return 0;
    if (rtc_codec_equal(codec_name, "h264")) {
        return rtc_h264_keyframe(payload, payload_len);
    }
    if (rtc_codec_equal(codec_name, "h265") ||
        rtc_codec_equal(codec_name, "hevc")) {
        return rtc_h265_keyframe(payload, payload_len);
    }
    if (rtc_codec_equal(codec_name, "vp8")) {
        return rtc_vp8_keyframe(payload, payload_len);
    }
    if (rtc_codec_equal(codec_name, "vp9")) {
        return payload_len > 0 && (payload[0] & 0x48) == 0x08;
    }
    return 0;
}

static turbo_media_rtc_track_binding_t *rtc_find_binding_by_track(
    turbo_media_rtc_session_t *session,
    turbo_media_rtc_backend_track_t *track) {
    size_t i;

    for (i = 0; i < session->track_count; ++i) {
        if (session->tracks[i].track == track) return &session->tracks[i];
    }
    return NULL;
}

static turbo_media_rtc_track_binding_t *rtc_find_binding_by_id(
    turbo_media_rtc_session_t *session,
    int track_id) {
    size_t i;

    for (i = 0; i < session->track_count; ++i) {
        if (session->tracks[i].track_id == track_id) return &session->tracks[i];
    }
    return NULL;
}

static int rtc_store_binding(turbo_media_rtc_session_t *session,
                             int track_id,
                             turbo_media_rtc_backend_track_t *track,
                             const char *codec_name) {
    turbo_media_rtc_track_binding_t *binding;

    if (!session || !track || !codec_name || !codec_name[0]) {
        return TURBO_MEDIA_ERR_INVALID;
    }
    if (session->track_count >= TURBO_MEDIA_RTC_MAX_TRACKS) {
        return TURBO_MEDIA_ERR_FULL;
    }
    if (rtc_find_binding_by_id(session, track_id)) return TURBO_MEDIA_ERR_EXISTS;

    binding = &session->tracks[session->track_count++];
    memset(binding, 0, sizeof(*binding));
    binding->track_id = track_id;
    binding->track = track;
    strncpy(binding->codec_name, codec_name, sizeof(binding->codec_name) - 1);
    return TURBO_MEDIA_OK;
}

static int rtc_backend_track_to_media(
    const turbo_media_rtc_backend_track_info_t *backend_track,
    turbo_media_track_info_t *track) {
    if (!backend_track || !track || backend_track->track_id < 0 ||
        !backend_track->codec_name[0]) {
        return TURBO_MEDIA_ERR_INVALID;
    }

    memset(track, 0, sizeof(*track));
    track->track_id = backend_track->track_id;
    if (backend_track->type == TURBO_MEDIA_RTC_BACKEND_TRACK_AUDIO) {
        track->type = TURBO_MEDIA_TRACK_AUDIO;
    } else if (backend_track->type == TURBO_MEDIA_RTC_BACKEND_TRACK_VIDEO) {
        track->type = TURBO_MEDIA_TRACK_VIDEO;
    } else {
        return TURBO_MEDIA_ERR_INVALID;
    }
    strncpy(track->codec_name, backend_track->codec_name,
            sizeof(track->codec_name) - 1);
    track->payload_type = backend_track->payload_type;
    track->clock_rate = backend_track->clock_rate;
    track->width = backend_track->width;
    track->height = backend_track->height;
    track->framerate = backend_track->framerate;
    track->sample_rate = backend_track->sample_rate;
    track->channels = backend_track->channels;
    return TURBO_MEDIA_OK;
}

static int rtc_media_track_to_backend(
    const turbo_media_track_info_t *track,
    turbo_media_rtc_backend_track_info_t *backend_track) {
    if (!track || !backend_track || track->track_id < 0 || !track->codec_name[0]) {
        return TURBO_MEDIA_ERR_INVALID;
    }

    memset(backend_track, 0, sizeof(*backend_track));
    backend_track->track_id = track->track_id;
    if (track->type == TURBO_MEDIA_TRACK_AUDIO) {
        backend_track->type = TURBO_MEDIA_RTC_BACKEND_TRACK_AUDIO;
    } else if (track->type == TURBO_MEDIA_TRACK_VIDEO) {
        backend_track->type = TURBO_MEDIA_RTC_BACKEND_TRACK_VIDEO;
    } else {
        return TURBO_MEDIA_ERR_INVALID;
    }
    strncpy(backend_track->codec_name, track->codec_name,
            sizeof(backend_track->codec_name) - 1);
    backend_track->payload_type = track->payload_type;
    backend_track->clock_rate = track->clock_rate;
    backend_track->width = track->width;
    backend_track->height = track->height;
    backend_track->framerate = track->framerate;
    backend_track->sample_rate = track->sample_rate;
    backend_track->channels = track->channels;
    return TURBO_MEDIA_OK;
}

static int rtc_open_publisher(turbo_media_rtc_session_t *session) {
    turbo_media_protocol_session_config_t config;

    memset(&config, 0, sizeof(config));
    config.protocol = TURBO_MEDIA_PROTOCOL_WEBRTC;
    config.role = TURBO_MEDIA_PROTOCOL_ROLE_PUBLISHER;
    config.key = session->key;
    config.remove_source_on_close = session->remove_source_on_close;
    return turbo_media_server_protocol_session_open(
        session->runtime, &config, &session->protocol_session);
}

static int rtc_player_frame_cb(turbo_media_source_t *source,
                               const turbo_media_frame_t *frame,
                               void *user_data) {
    turbo_media_rtc_session_t *session = (turbo_media_rtc_session_t *)user_data;
    turbo_media_rtc_track_binding_t *binding;
    int rc;

    (void)source;
    if (!session || session->closing || !frame) return TURBO_MEDIA_ERR_STATE;
    binding = rtc_find_binding_by_id(session, frame->track_id);
    if (!binding) return TURBO_MEDIA_ERR_NOT_FOUND;

    rc = session->backend->send_rtp(binding->track, frame->data, frame->size);
    if (rc != 0) {
        session->last_error = TURBO_MEDIA_ERR_STATE;
        return TURBO_MEDIA_ERR_STATE;
    }
    return TURBO_MEDIA_OK;
}

static int rtc_open_player(turbo_media_rtc_session_t *session) {
    turbo_media_protocol_session_config_t config;

    if (session->protocol_session) return TURBO_MEDIA_OK;
    memset(&config, 0, sizeof(config));
    config.protocol = TURBO_MEDIA_PROTOCOL_WEBRTC;
    config.role = TURBO_MEDIA_PROTOCOL_ROLE_PLAYER;
    config.key = session->key;
    config.callback = rtc_player_frame_cb;
    config.callback_user_data = session;
    config.replay_cached = session->replay_cached;
    return turbo_media_server_protocol_session_open(
        session->runtime, &config, &session->protocol_session);
}

static void rtc_backend_state(void *user_data, int state_value) {
    turbo_media_rtc_session_t *session = (turbo_media_rtc_session_t *)user_data;
    turbo_media_rtc_peer_state_t state;
    size_t i;
    int rc = TURBO_MEDIA_OK;

    if (!session || session->closing || state_value < TURBO_MEDIA_RTC_PEER_NEW ||
        state_value > TURBO_MEDIA_RTC_PEER_CLOSED) {
        return;
    }

    state = (turbo_media_rtc_peer_state_t)state_value;
    if (state == TURBO_MEDIA_RTC_PEER_CONNECTED &&
        session->role == TURBO_MEDIA_RTC_ROLE_PLAYER) {
        for (i = 0; i < session->track_count; ++i) {
            if (session->backend->start_track(session->tracks[i].track) != 0) {
                rc = TURBO_MEDIA_ERR_STATE;
                break;
            }
        }
        if (rc == TURBO_MEDIA_OK) rc = rtc_open_player(session);
        if (rc != TURBO_MEDIA_OK) {
            session->last_error = rc;
            state = TURBO_MEDIA_RTC_PEER_FAILED;
        }
    }

    session->state = state;
    if (session->on_state) session->on_state(session, state, session->user_data);
}

static void rtc_backend_ice_candidate(void *user_data, const char *candidate) {
    turbo_media_rtc_session_t *session = (turbo_media_rtc_session_t *)user_data;

    if (!session || session->closing || !candidate) return;
    if (session->on_ice_candidate) {
        session->on_ice_candidate(session, candidate, session->user_data);
    }
}

static int rtc_backend_remote_track(
    void *user_data,
    turbo_media_rtc_backend_track_t *backend_track,
    const turbo_media_rtc_backend_track_info_t *backend_info) {
    turbo_media_rtc_session_t *session = (turbo_media_rtc_session_t *)user_data;
    turbo_media_track_info_t track;
    int rc;

    if (!session || session->closing ||
        session->role != TURBO_MEDIA_RTC_ROLE_PUBLISHER) {
        return TURBO_MEDIA_ERR_STATE;
    }

    rc = rtc_backend_track_to_media(backend_info, &track);
    if (rc == TURBO_MEDIA_OK) {
        rc = turbo_media_server_runtime_add_track(
            session->runtime, &session->key, &track, NULL);
    }
    if (rc == TURBO_MEDIA_OK) {
        rc = rtc_store_binding(
            session, track.track_id, backend_track, track.codec_name);
    }
    if (rc != TURBO_MEDIA_OK) session->last_error = rc;
    return rc;
}

static int rtc_backend_rtp(void *user_data,
                           turbo_media_rtc_backend_track_t *backend_track,
                           const uint8_t *packet,
                           size_t packet_len) {
    turbo_media_rtc_session_t *session = (turbo_media_rtc_session_t *)user_data;
    turbo_media_rtc_track_binding_t *binding;
    turbo_media_frame_t frame;
    uint32_t timestamp;
    int rc;

    if (!session || session->closing || !session->protocol_session ||
        !packet || packet_len < TURBO_MEDIA_RTC_RTP_HEADER_SIZE) {
        return TURBO_MEDIA_ERR_INVALID;
    }
    binding = rtc_find_binding_by_track(session, backend_track);
    if (!binding) return TURBO_MEDIA_ERR_NOT_FOUND;

    timestamp = ((uint32_t)packet[4] << 24) |
                ((uint32_t)packet[5] << 16) |
                ((uint32_t)packet[6] << 8) |
                packet[7];
    memset(&frame, 0, sizeof(frame));
    frame.track_id = binding->track_id;
    frame.data = packet;
    frame.size = packet_len;
    frame.pts = timestamp;
    frame.dts = timestamp;
    frame.is_keyframe = rtc_packet_is_keyframe(
        binding->codec_name, packet, packet_len);

    rc = turbo_media_server_protocol_session_publish(
        session->protocol_session, &frame);
    if (rc != TURBO_MEDIA_OK) session->last_error = rc;
    return rc;
}

static int rtc_add_player_tracks(turbo_media_rtc_session_t *session) {
    turbo_media_source_t *source;
    size_t track_count;
    size_t i;
    int added = 0;
    int rc;

    rc = turbo_media_server_runtime_find_source(
        session->runtime, &session->key, &source);
    if (rc != TURBO_MEDIA_OK) return rc;

    track_count = turbo_media_source_track_count(source);
    for (i = 0; i < track_count; ++i) {
        turbo_media_track_info_t track;
        turbo_media_rtc_backend_track_info_t backend_info;
        turbo_media_rtc_backend_track_t *backend_track = NULL;

        rc = turbo_media_source_get_track_at(source, i, &track);
        if (rc != TURBO_MEDIA_OK) return rc;
        rc = rtc_media_track_to_backend(&track, &backend_info);
        if (rc == TURBO_MEDIA_ERR_INVALID && track.type == TURBO_MEDIA_TRACK_DATA) {
            continue;
        }
        if (rc != TURBO_MEDIA_OK) return rc;
        if (session->backend->add_send_track(
                session->peer, &backend_info, &backend_track) != 0 ||
            !backend_track) {
            return TURBO_MEDIA_ERR_STATE;
        }
        rc = rtc_store_binding(
            session, track.track_id, backend_track, track.codec_name);
        if (rc != TURBO_MEDIA_OK) return rc;
        added++;
    }
    return added > 0 ? TURBO_MEDIA_OK : TURBO_MEDIA_ERR_NOT_FOUND;
}

int turbo_media_rtc_session_create_with_backend(
    const turbo_media_rtc_session_config_t *config,
    const turbo_media_rtc_backend_ops_t *backend,
    char *answer_sdp,
    size_t answer_sdp_capacity,
    size_t *answer_sdp_length,
    turbo_media_rtc_session_t **session) {
    turbo_media_rtc_backend_config_t backend_config;
    turbo_media_rtc_session_t *created;
    int rc;

    if (!config || !config->runtime || !rtc_role_valid(config->role) ||
        !config->remote_offer_sdp || !config->remote_offer_sdp[0] ||
        !answer_sdp || answer_sdp_capacity == 0 || !session ||
        !rtc_backend_valid(backend) ||
        config->stun_server_count > (size_t)INT_MAX ||
        config->turn_server_count > (size_t)INT_MAX) {
        return TURBO_MEDIA_ERR_INVALID;
    }

    *session = NULL;
    if (answer_sdp_length) *answer_sdp_length = 0;
    created = (turbo_media_rtc_session_t *)calloc(1, sizeof(*created));
    if (!created) return TURBO_MEDIA_ERR_NOMEM;

    created->runtime = config->runtime;
    created->role = config->role;
    created->backend = backend;
    created->state = TURBO_MEDIA_RTC_PEER_NEW;
    created->replay_cached = config->replay_cached;
    created->remove_source_on_close = config->remove_source_on_close;
    created->on_ice_candidate = config->on_ice_candidate;
    created->on_state = config->on_state;
    created->user_data = config->user_data;

    rc = turbo_media_rtc_source_key(config->default_vhost,
                                    config->resource_path,
                                    config->query,
                                    &created->key);
    if (rc != TURBO_MEDIA_OK) goto cleanup;

    memset(&backend_config, 0, sizeof(backend_config));
    backend_config.stun_servers = config->stun_servers;
    backend_config.stun_server_count = config->stun_server_count;
    backend_config.turn_servers = config->turn_servers;
    backend_config.turn_server_count = config->turn_server_count;
    backend_config.allow_loopback = config->allow_loopback;
    backend_config.on_state = rtc_backend_state;
    backend_config.on_ice_candidate = rtc_backend_ice_candidate;
    backend_config.on_remote_track = rtc_backend_remote_track;
    backend_config.on_rtp = rtc_backend_rtp;
    backend_config.user_data = created;

    created->peer = backend->create(&backend_config);
    if (!created->peer) {
        rc = TURBO_MEDIA_ERR_STATE;
        goto cleanup;
    }

    if (created->role == TURBO_MEDIA_RTC_ROLE_PUBLISHER) {
        rc = rtc_open_publisher(created);
    } else {
        rc = rtc_add_player_tracks(created);
    }
    if (rc != TURBO_MEDIA_OK) goto cleanup;

    if (backend->set_remote_offer(created->peer, config->remote_offer_sdp) != 0) {
        rc = created->last_error != TURBO_MEDIA_OK
                 ? created->last_error
                 : TURBO_MEDIA_ERR_INVALID;
        goto cleanup;
    }
    if (backend->create_answer(created->peer,
                               answer_sdp,
                               answer_sdp_capacity,
                               answer_sdp_length) != 0) {
        rc = TURBO_MEDIA_ERR_STATE;
        goto cleanup;
    }

    *session = created;
    return TURBO_MEDIA_OK;

cleanup:
    created->last_error = rc;
    turbo_media_rtc_session_destroy(created);
    return rc;
}

#ifdef TURBO_MEDIA_HAS_TURBORTC_BACKEND
int turbo_media_rtc_session_create(
    const turbo_media_rtc_session_config_t *config,
    char *answer_sdp,
    size_t answer_sdp_capacity,
    size_t *answer_sdp_length,
    turbo_media_rtc_session_t **session) {
    return turbo_media_rtc_session_create_with_backend(
        config,
        turbo_media_rtc_turbortc_backend(),
        answer_sdp,
        answer_sdp_capacity,
        answer_sdp_length,
        session);
}
#endif

void turbo_media_rtc_session_destroy(turbo_media_rtc_session_t *session) {
    if (!session || session->closing) return;

    session->closing = 1;
    if (session->peer && session->backend && session->backend->destroy) {
        session->backend->destroy(session->peer);
        session->peer = NULL;
    }
    if (session->protocol_session) {
        turbo_media_server_protocol_session_close(session->protocol_session);
        session->protocol_session = NULL;
    }
    session->state = TURBO_MEDIA_RTC_PEER_CLOSED;
    free(session);
}

int turbo_media_rtc_session_add_ice_candidate(
    turbo_media_rtc_session_t *session,
    const char *candidate) {
    if (!session || session->closing || !session->peer ||
        !candidate || !candidate[0]) {
        return TURBO_MEDIA_ERR_INVALID;
    }
    if (session->backend->add_ice_candidate(session->peer, candidate) != 0) {
        session->last_error = TURBO_MEDIA_ERR_INVALID;
        return TURBO_MEDIA_ERR_INVALID;
    }
    return TURBO_MEDIA_OK;
}

int turbo_media_rtc_session_pump(turbo_media_rtc_session_t *session) {
    if (!session || session->closing || !session->peer) {
        return TURBO_MEDIA_ERR_INVALID;
    }
    if (session->last_error != TURBO_MEDIA_OK) return session->last_error;
    if (session->backend->pump(session->peer) != 0) {
        session->last_error = TURBO_MEDIA_ERR_STATE;
        return TURBO_MEDIA_ERR_STATE;
    }
    return session->last_error;
}

turbo_media_rtc_peer_state_t turbo_media_rtc_session_state(
    const turbo_media_rtc_session_t *session) {
    return session ? session->state : TURBO_MEDIA_RTC_PEER_CLOSED;
}

int turbo_media_rtc_session_last_error(
    const turbo_media_rtc_session_t *session) {
    return session ? session->last_error : TURBO_MEDIA_ERR_INVALID;
}

const turbo_media_source_key_t *turbo_media_rtc_session_key(
    const turbo_media_rtc_session_t *session) {
    return session && !session->closing ? &session->key : NULL;
}
