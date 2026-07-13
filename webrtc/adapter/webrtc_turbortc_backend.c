#include "rtc_peer_backend.h"

#include <turbo_media_engine.h>
#include <turbo_peer_connection.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define TURBO_MEDIA_RTC_BACKEND_MAX_TRACKS 8

struct turbo_media_rtc_backend_track_s {
    turbo_media_rtc_backend_peer_t *owner;
    turbo_media_track_t *native_track;
    turbo_media_rtc_backend_track_info_t info;
};

struct turbo_media_rtc_backend_peer_s {
    turbo_peer_connection_t *native_peer;
    turbo_media_context_t *media_context;
    turbo_media_rtc_backend_config_t config;
    turbo_media_rtc_backend_track_t tracks[TURBO_MEDIA_RTC_BACKEND_MAX_TRACKS];
    size_t track_count;
    int next_remote_track_id;
    int callback_error;
};

static int turbortc_text_equal(const char *lhs, const char *rhs) {
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

static int turbortc_codec_from_name(const char *name, turbo_codec_type_t *codec) {
    if (!name || !codec) return -1;
    if (turbortc_text_equal(name, "opus")) {
        *codec = TURBO_CODEC_OPUS;
    } else if (turbortc_text_equal(name, "pcmu")) {
        *codec = TURBO_CODEC_PCMU;
    } else if (turbortc_text_equal(name, "pcma")) {
        *codec = TURBO_CODEC_PCMA;
    } else if (turbortc_text_equal(name, "vp8")) {
        *codec = TURBO_CODEC_VP8;
    } else if (turbortc_text_equal(name, "vp9")) {
        *codec = TURBO_CODEC_VP9;
    } else if (turbortc_text_equal(name, "h264")) {
        *codec = TURBO_CODEC_H264;
    } else if (turbortc_text_equal(name, "h265") ||
               turbortc_text_equal(name, "hevc")) {
        *codec = TURBO_CODEC_H265;
    } else {
        return -1;
    }
    return 0;
}

static const char *turbortc_codec_name(turbo_codec_type_t codec) {
    switch (codec) {
        case TURBO_CODEC_OPUS: return "opus";
        case TURBO_CODEC_PCMU: return "pcmu";
        case TURBO_CODEC_PCMA: return "pcma";
        case TURBO_CODEC_VP8: return "vp8";
        case TURBO_CODEC_VP9: return "vp9";
        case TURBO_CODEC_H264: return "h264";
        case TURBO_CODEC_H265: return "h265";
        default: return NULL;
    }
}

static turbo_media_rtc_backend_track_t *turbortc_store_track(
    turbo_media_rtc_backend_peer_t *peer,
    turbo_media_track_t *native_track,
    const turbo_media_rtc_backend_track_info_t *info) {
    turbo_media_rtc_backend_track_t *track;

    if (!peer || !native_track || !info ||
        peer->track_count >= TURBO_MEDIA_RTC_BACKEND_MAX_TRACKS) {
        return NULL;
    }
    track = &peer->tracks[peer->track_count++];
    memset(track, 0, sizeof(*track));
    track->owner = peer;
    track->native_track = native_track;
    track->info = *info;
    return track;
}

static void turbortc_on_state(turbo_peer_connection_t *native_peer,
                              turbo_peer_state_t state,
                              void *user_data) {
    turbo_media_rtc_backend_peer_t *peer =
        (turbo_media_rtc_backend_peer_t *)user_data;

    (void)native_peer;
    if (peer && peer->config.on_state) {
        peer->config.on_state(peer->config.user_data, (int)state);
    }
}

static void turbortc_on_ice_candidate(turbo_peer_connection_t *native_peer,
                                      const char *candidate,
                                      void *user_data) {
    turbo_media_rtc_backend_peer_t *peer =
        (turbo_media_rtc_backend_peer_t *)user_data;

    (void)native_peer;
    if (peer && peer->config.on_ice_candidate && candidate) {
        peer->config.on_ice_candidate(peer->config.user_data, candidate);
    }
}

static void turbortc_on_rtp(turbo_media_track_t *native_track,
                            const uint8_t *packet,
                            size_t packet_len,
                            void *user_data) {
    turbo_media_rtc_backend_track_t *track =
        (turbo_media_rtc_backend_track_t *)user_data;
    turbo_media_rtc_backend_peer_t *peer;
    int rc;

    if (!track || !packet) return;
    peer = track->owner;
    (void)native_track;
    if (!peer || peer->callback_error || !peer->config.on_rtp) return;
    rc = peer->config.on_rtp(
        peer->config.user_data, track, packet, packet_len);
    if (rc != 0) {
        peer->callback_error = rc;
    }
}

static void turbortc_on_track(turbo_peer_connection_t *native_peer,
                              turbo_media_track_t *native_track,
                              void *user_data) {
    turbo_media_rtc_backend_peer_t *peer =
        (turbo_media_rtc_backend_peer_t *)user_data;
    turbo_media_rtc_backend_track_info_t info;
    turbo_media_rtc_backend_track_t *track;
    const char *codec_name;

    (void)native_peer;
    if (!peer || !native_track || peer->callback_error) return;

    memset(&info, 0, sizeof(info));
    info.track_id = peer->next_remote_track_id++;
    if (turbo_media_track_get_type(native_track) == TURBO_MEDIA_TRACK_AUDIO) {
        info.type = TURBO_MEDIA_RTC_BACKEND_TRACK_AUDIO;
        info.clock_rate = 48000;
        info.sample_rate = 48000;
        info.channels = 2;
    } else {
        info.type = TURBO_MEDIA_RTC_BACKEND_TRACK_VIDEO;
        info.clock_rate = 90000;
    }
    codec_name = turbortc_codec_name(turbo_media_track_get_codec(native_track));
    if (!codec_name) {
        peer->callback_error = -1;
        return;
    }
    strncpy(info.codec_name, codec_name, sizeof(info.codec_name) - 1);
    info.payload_type = (int)turbo_media_track_get_payload_type(native_track);

    track = turbortc_store_track(peer, native_track, &info);
    if (!track) {
        peer->callback_error = -1;
        return;
    }
    turbo_media_track_set_user_data(native_track, track);
    turbo_media_track_on_rtp_packet(native_track, turbortc_on_rtp);
    if (peer->config.on_remote_track &&
        peer->config.on_remote_track(
            peer->config.user_data, track, &track->info) != 0) {
        turbo_media_track_on_rtp_packet(native_track, NULL);
        turbo_media_track_set_user_data(native_track, NULL);
        peer->callback_error = -1;
    }
}

static turbo_media_rtc_backend_peer_t *turbortc_create(
    const turbo_media_rtc_backend_config_t *config) {
    turbo_media_rtc_backend_peer_t *peer;
    turbo_peer_callbacks_t callbacks;
    turbo_peer_config_t peer_config;

    if (!config || config->stun_server_count > (size_t)INT_MAX ||
        config->turn_server_count > (size_t)INT_MAX) {
        return NULL;
    }
    peer = (turbo_media_rtc_backend_peer_t *)calloc(1, sizeof(*peer));
    if (!peer) return NULL;
    peer->config = *config;

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.on_state_change = turbortc_on_state;
    callbacks.on_track = turbortc_on_track;
    callbacks.on_ice_candidate = turbortc_on_ice_candidate;

    memset(&peer_config, 0, sizeof(peer_config));
    peer_config.stun_servers = (const char **)config->stun_servers;
    peer_config.stun_server_count = (int)config->stun_server_count;
    peer_config.turn_servers = (const char **)config->turn_servers;
    peer_config.turn_server_count = (int)config->turn_server_count;
    peer_config.allow_loopback = config->allow_loopback;
    peer_config.disable_datachannel = 1;
    peer_config.user_data = peer;

    peer->native_peer = turbo_peer_connection_create(&peer_config, &callbacks);
    if (!peer->native_peer) {
        free(peer);
        return NULL;
    }
    peer->media_context = turbo_peer_connection_get_media_context(peer->native_peer);
    if (!peer->media_context) {
        turbo_peer_connection_destroy(peer->native_peer);
        free(peer);
        return NULL;
    }
    return peer;
}

static void turbortc_destroy(turbo_media_rtc_backend_peer_t *peer) {
    size_t i;

    if (!peer) return;
    for (i = 0; i < peer->track_count; ++i) {
        if (peer->tracks[i].native_track) {
            turbo_media_track_on_rtp_packet(peer->tracks[i].native_track, NULL);
            turbo_media_track_set_user_data(peer->tracks[i].native_track, NULL);
        }
    }
    turbo_peer_connection_destroy(peer->native_peer);
    free(peer);
}

static int turbortc_add_send_track(
    turbo_media_rtc_backend_peer_t *peer,
    const turbo_media_rtc_backend_track_info_t *info,
    turbo_media_rtc_backend_track_t **track) {
    turbo_media_track_config_t config;
    turbo_media_track_t *native_track;
    turbo_media_rtc_backend_track_t *stored;

    if (!peer || !info || !track || !info->codec_name[0]) return -1;
    *track = NULL;
    memset(&config, 0, sizeof(config));
    config.direction = TURBO_MEDIA_DIRECTION_SENDONLY;
    if (turbortc_codec_from_name(info->codec_name, &config.codec) != 0) return -1;

    if (info->type == TURBO_MEDIA_RTC_BACKEND_TRACK_AUDIO) {
        config.type = TURBO_MEDIA_TRACK_AUDIO;
        config.audio.sample_rate = info->sample_rate > 0 ? info->sample_rate : 48000;
        config.audio.channels = info->channels > 0 ? info->channels : 2;
        config.audio.bitrate = 64000;
        config.audio.frame_size_ms = 20;
    } else if (info->type == TURBO_MEDIA_RTC_BACKEND_TRACK_VIDEO) {
        config.type = TURBO_MEDIA_TRACK_VIDEO;
        config.video.width = info->width > 0 ? info->width : 1280;
        config.video.height = info->height > 0 ? info->height : 720;
        config.video.framerate = info->framerate > 0 ? info->framerate : 30;
        config.video.bitrate = 1500000;
        config.video.keyframe_interval = config.video.framerate * 2;
    } else {
        return -1;
    }

    native_track = turbo_peer_connection_add_track_ex(peer->native_peer, &config);
    if (!native_track) return -1;
    if (info->payload_type >= 0 && info->payload_type <= 127) {
        turbo_media_track_set_payload_type(native_track, (uint8_t)info->payload_type);
    }
    stored = turbortc_store_track(peer, native_track, info);
    if (!stored) return -1;
    *track = stored;
    return 0;
}

static int turbortc_set_remote_offer(turbo_media_rtc_backend_peer_t *peer,
                                     const char *offer_sdp) {
    int rc;

    if (!peer || !offer_sdp) return -1;
    peer->callback_error = 0;
    rc = turbo_peer_connection_set_remote_description(
        peer->native_peer, "offer", offer_sdp);
    return rc == 0 && peer->callback_error == 0 ? 0 : -1;
}

static int turbortc_create_answer(turbo_media_rtc_backend_peer_t *peer,
                                  char *answer_sdp,
                                  size_t answer_sdp_capacity,
                                  size_t *answer_sdp_length) {
    int length;

    if (!peer || !answer_sdp || answer_sdp_capacity == 0) return -1;
    length = turbo_peer_connection_create_answer(
        peer->native_peer, answer_sdp, answer_sdp_capacity);
    if (length <= 0 || (size_t)length >= answer_sdp_capacity) return -1;
    if (answer_sdp_length) *answer_sdp_length = (size_t)length;
    return 0;
}

static int turbortc_add_ice_candidate(turbo_media_rtc_backend_peer_t *peer,
                                      const char *candidate) {
    return peer && candidate
               ? turbo_peer_connection_add_ice_candidate(peer->native_peer, candidate)
               : -1;
}

static int turbortc_start_track(turbo_media_rtc_backend_track_t *track) {
    return track && track->native_track
               ? turbo_media_track_start(track->native_track)
               : -1;
}

static int turbortc_send_rtp(turbo_media_rtc_backend_track_t *track,
                             const uint8_t *packet,
                             size_t packet_len) {
    return track && track->native_track && packet
               ? turbo_media_track_send_rtp_packet(
                     track->native_track, packet, packet_len)
               : -1;
}

static int turbortc_pump(turbo_media_rtc_backend_peer_t *peer) {
    if (!peer || !peer->native_peer || !peer->media_context) return -1;
    turbo_peer_connection_poll(peer->native_peer);
    (void)turbo_media_handle_timers(peer->media_context);
    return peer->callback_error == 0 ? 0 : -1;
}

const turbo_media_rtc_backend_ops_t *turbo_media_rtc_turbortc_backend(void) {
    static const turbo_media_rtc_backend_ops_t ops = {
        turbortc_create,
        turbortc_destroy,
        turbortc_add_send_track,
        turbortc_set_remote_offer,
        turbortc_create_answer,
        turbortc_add_ice_candidate,
        turbortc_start_track,
        turbortc_send_rtp,
        turbortc_pump
    };
    return &ops;
}
