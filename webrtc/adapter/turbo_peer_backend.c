#include "turbo_media_webrtc_backend.h"

#include "turbo_peer_connection.h"
#include "turbo_sdp.h"

#include <tlog.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum {
    RTC_BACKEND_MAX_TRACKS = SDP_MAX_MEDIA_SECTIONS,
    RTC_BACKEND_DEFAULT_EVENT_CAPACITY = 256,
    RTC_BACKEND_MAX_EVENT_CAPACITY = 4096,
    RTC_BACKEND_DEFAULT_QUEUED_BYTES = 4 * 1024 * 1024,
    RTC_BACKEND_MAX_QUEUED_BYTES = 64 * 1024 * 1024,
    RTC_BACKEND_DEFAULT_RTP_PACKET_SIZE = 65535,
    RTC_BACKEND_MAX_RTP_PACKET_SIZE = 65535,
    RTC_BACKEND_DEFAULT_AUDIO_BITRATE = 64000,
    RTC_BACKEND_DEFAULT_AUDIO_FRAME_MS = 20,
    RTC_BACKEND_DEFAULT_VIDEO_WIDTH = 1280,
    RTC_BACKEND_DEFAULT_VIDEO_HEIGHT = 720,
    RTC_BACKEND_DEFAULT_VIDEO_FRAMERATE = 30,
    RTC_BACKEND_DEFAULT_VIDEO_BITRATE = 1500000,
    RTC_BACKEND_DEFAULT_KEYFRAME_INTERVAL = 30
};

struct turbo_media_webrtc_backend_track_s {
    turbo_media_webrtc_backend_peer_t *peer;
    turbo_media_track_t *media_track;
    turbo_media_webrtc_backend_track_info_t info;
    int remote;
    int active;
};

struct turbo_media_webrtc_backend_peer_s {
    turbo_media_webrtc_backend_config_t config;
    turbo_peer_connection_t *peer_connection;
    sdp_session_t remote_sdp;
    int remote_offer_set;
    int failed;
    int remote_media_claimed[SDP_MAX_MEDIA_SECTIONS];
    int remote_media_reported[SDP_MAX_MEDIA_SECTIONS];
    turbo_media_webrtc_backend_track_t tracks[RTC_BACKEND_MAX_TRACKS];
    size_t track_count;
    size_t max_rtp_packet_bytes;
};

static int rtc_backend_ascii_equal(const char *left, const char *right) {
    unsigned char a;
    unsigned char b;

    if (!left || !right) return 0;
    while (*left && *right) {
        a = (unsigned char)*left++;
        b = (unsigned char)*right++;
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return *left == '\0' && *right == '\0';
}

static int rtc_backend_codec_type(const char *name,
                                  turbo_codec_type_t *codec) {
    if (!name || !codec) return -1;
    if (rtc_backend_ascii_equal(name, "opus")) {
        *codec = TURBO_CODEC_OPUS;
    } else if (rtc_backend_ascii_equal(name, "pcmu")) {
        *codec = TURBO_CODEC_PCMU;
    } else if (rtc_backend_ascii_equal(name, "pcma")) {
        *codec = TURBO_CODEC_PCMA;
    } else if (rtc_backend_ascii_equal(name, "vp8")) {
        *codec = TURBO_CODEC_VP8;
    } else if (rtc_backend_ascii_equal(name, "vp9")) {
        *codec = TURBO_CODEC_VP9;
    } else if (rtc_backend_ascii_equal(name, "h264")) {
        *codec = TURBO_CODEC_H264;
    } else if (rtc_backend_ascii_equal(name, "h265") ||
               rtc_backend_ascii_equal(name, "hevc")) {
        *codec = TURBO_CODEC_H265;
    } else {
        return -1;
    }
    return 0;
}

static const char *rtc_backend_codec_name(turbo_codec_type_t codec) {
    switch (codec) {
        case TURBO_CODEC_OPUS:
            return "opus";
        case TURBO_CODEC_PCMU:
            return "PCMU";
        case TURBO_CODEC_PCMA:
            return "PCMA";
        case TURBO_CODEC_VP8:
            return "VP8";
        case TURBO_CODEC_VP9:
            return "VP9";
        case TURBO_CODEC_H264:
            return "H264";
        case TURBO_CODEC_H265:
            return "H265";
        default:
            return NULL;
    }
}

static turbo_media_webrtc_backend_track_t *rtc_backend_reserve_track(
    turbo_media_webrtc_backend_peer_t *peer) {
    turbo_media_webrtc_backend_track_t *track;

    if (!peer || peer->track_count >= RTC_BACKEND_MAX_TRACKS) return NULL;
    track = &peer->tracks[peer->track_count++];
    memset(track, 0, sizeof(*track));
    track->peer = peer;
    track->active = 1;
    return track;
}

static void rtc_backend_fail(turbo_media_webrtc_backend_peer_t *peer) {
    if (!peer || peer->failed) return;
    peer->failed = 1;
    peer->config.on_state(
        peer->config.user_data, (int)TURBO_PEER_STATE_FAILED);
}

static const sdp_codec_t *rtc_backend_remote_codec(
    turbo_media_webrtc_backend_peer_t *peer,
    turbo_media_track_t *track,
    int *media_index) {
    turbo_rtc_media_track_type_t track_type;
    int payload_type;
    int i;
    int j;

    if (!peer || !track) return NULL;
    track_type = turbo_media_track_get_type(track);
    payload_type = (int)turbo_media_track_get_payload_type(track);
    for (i = 0; i < peer->remote_sdp.media_count; ++i) {
        const sdp_media_t *media = &peer->remote_sdp.media[i];
        if (peer->remote_media_reported[i] ||
            (media->direction != SDP_DIRECTION_SENDONLY &&
             media->direction != SDP_DIRECTION_SENDRECV) ||
            (track_type == TURBO_RTC_MEDIA_TRACK_AUDIO &&
             media->type != SDP_MEDIA_AUDIO) ||
            (track_type == TURBO_RTC_MEDIA_TRACK_VIDEO &&
             media->type != SDP_MEDIA_VIDEO)) {
            continue;
        }
        for (j = 0; j < media->codec_count; ++j) {
            turbo_codec_type_t codec;
            if (media->codecs[j].payload_type == payload_type &&
                rtc_backend_codec_type(media->codecs[j].name, &codec) == 0) {
                if (media_index) *media_index = i;
                return &media->codecs[j];
            }
        }
    }
    return NULL;
}

static void rtc_backend_rtp_packet(turbo_media_track_t *media_track,
                                   const uint8_t *packet,
                                   size_t packet_len,
                                   void *user_data) {
    turbo_media_webrtc_backend_track_t *track =
        (turbo_media_webrtc_backend_track_t *)user_data;

    (void)media_track;
    if (!track || !track->active || !packet ||
        packet_len > track->peer->max_rtp_packet_bytes) {
        if (track && packet_len > track->peer->max_rtp_packet_bytes) {
            rtc_backend_fail(track->peer);
        }
        return;
    }
    if (track->peer->config.on_rtp(
            track->peer->config.user_data, track, packet, packet_len) != 0) {
        rtc_backend_fail(track->peer);
    }
}

static void rtc_backend_state_changed(turbo_peer_connection_t *connection,
                                      turbo_peer_state_t state,
                                      void *user_data) {
    turbo_media_webrtc_backend_peer_t *peer =
        (turbo_media_webrtc_backend_peer_t *)user_data;

    (void)connection;
    if (!peer || state < TURBO_PEER_STATE_NEW ||
        state > TURBO_PEER_STATE_CLOSED) {
        return;
    }
    if (state == TURBO_PEER_STATE_FAILED) peer->failed = 1;
    peer->config.on_state(peer->config.user_data, (int)state);
}

static void rtc_backend_ice_candidate(turbo_peer_connection_t *connection,
                                      const char *candidate,
                                      void *user_data) {
    turbo_media_webrtc_backend_peer_t *peer =
        (turbo_media_webrtc_backend_peer_t *)user_data;

    (void)connection;
    if (!peer || !candidate) return;
    peer->config.on_ice_candidate(peer->config.user_data, candidate);
}

static void rtc_backend_remote_track(turbo_peer_connection_t *connection,
                                     turbo_media_track_t *media_track,
                                     void *user_data) {
    turbo_media_webrtc_backend_peer_t *peer =
        (turbo_media_webrtc_backend_peer_t *)user_data;
    turbo_media_webrtc_backend_track_t *track;
    const sdp_codec_t *codec;
    turbo_rtc_media_track_type_t type;
    int media_index = -1;

    (void)connection;
    if (!peer || !media_track || !peer->config.accept_remote_tracks) return;
    codec = rtc_backend_remote_codec(peer, media_track, &media_index);
    if (!codec || media_index < 0) {
        rtc_backend_fail(peer);
        return;
    }
    track = rtc_backend_reserve_track(peer);
    if (!track) {
        rtc_backend_fail(peer);
        return;
    }

    type = turbo_media_track_get_type(media_track);
    track->media_track = media_track;
    track->remote = 1;
    track->info.track_id = (int)(peer->track_count - 1);
    track->info.type =
        type == TURBO_RTC_MEDIA_TRACK_AUDIO
            ? TURBO_MEDIA_WEBRTC_BACKEND_TRACK_AUDIO
            : TURBO_MEDIA_WEBRTC_BACKEND_TRACK_VIDEO;
    strncpy(track->info.codec_name, codec->name,
            sizeof(track->info.codec_name) - 1);
    track->info.payload_type = codec->payload_type;
    track->info.clock_rate = codec->clock_rate;
    if (type == TURBO_RTC_MEDIA_TRACK_AUDIO) {
        track->info.sample_rate = codec->clock_rate;
        track->info.channels = codec->channels > 0 ? codec->channels : 1;
    }
    peer->remote_media_reported[media_index] = 1;

    turbo_media_track_set_user_data(media_track, track);
    turbo_media_track_on_rtp_packet(media_track, rtc_backend_rtp_packet);
    if (peer->config.on_remote_track(
            peer->config.user_data, track, &track->info) != 0) {
        rtc_backend_fail(peer);
    }
}

static turbo_media_webrtc_backend_peer_t *rtc_backend_create(
    const turbo_media_webrtc_backend_config_t *config) {
    turbo_media_webrtc_backend_peer_t *peer;
    turbo_peer_config_t peer_config;
    turbo_peer_callbacks_t callbacks;
    size_t event_capacity;
    size_t queued_bytes;
    size_t max_rtp_packet_bytes;
    size_t i;

    if (!config || !config->on_state || !config->on_ice_candidate ||
        !config->on_remote_track || !config->on_rtp ||
        config->stun_server_count > (size_t)INT_MAX ||
        config->turn_server_count > (size_t)INT_MAX ||
        (config->stun_server_count > 0 && !config->stun_servers) ||
        (config->turn_server_count > 0 && !config->turn_servers)) {
        return NULL;
    }
    for (i = 0; i < config->stun_server_count; ++i) {
        if (!config->stun_servers[i]) return NULL;
    }
    for (i = 0; i < config->turn_server_count; ++i) {
        if (!config->turn_servers[i]) return NULL;
    }

    event_capacity = config->event_queue_capacity
                         ? config->event_queue_capacity
                         : RTC_BACKEND_DEFAULT_EVENT_CAPACITY;
    queued_bytes = config->event_queue_max_bytes
                       ? config->event_queue_max_bytes
                       : RTC_BACKEND_DEFAULT_QUEUED_BYTES;
    max_rtp_packet_bytes = config->max_rtp_packet_bytes
                               ? config->max_rtp_packet_bytes
                               : RTC_BACKEND_DEFAULT_RTP_PACKET_SIZE;
    if (event_capacity > RTC_BACKEND_MAX_EVENT_CAPACITY ||
        queued_bytes > RTC_BACKEND_MAX_QUEUED_BYTES ||
        max_rtp_packet_bytes > RTC_BACKEND_MAX_RTP_PACKET_SIZE) {
        return NULL;
    }

    peer = (turbo_media_webrtc_backend_peer_t *)calloc(1, sizeof(*peer));
    if (!peer) return NULL;
    peer->config = *config;
    peer->max_rtp_packet_bytes = max_rtp_packet_bytes;

    memset(&peer_config, 0, sizeof(peer_config));
    peer_config.stun_servers = (const char **)config->stun_servers;
    peer_config.stun_server_count = (int)config->stun_server_count;
    peer_config.turn_servers = (const char **)config->turn_servers;
    peer_config.turn_server_count = (int)config->turn_server_count;
    peer_config.allow_loopback = config->allow_loopback;
    peer_config.disable_datachannel = 1;
    peer_config.user_data = peer;

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.on_state_change = rtc_backend_state_changed;
    callbacks.on_track = rtc_backend_remote_track;
    callbacks.on_ice_candidate = rtc_backend_ice_candidate;
    peer->peer_connection =
        turbo_peer_connection_create(&peer_config, &callbacks);
    if (!peer->peer_connection) {
        TLOG_ERROR("WebRTC internal backend failed to create PeerConnection");
        free(peer);
        return NULL;
    }
    return peer;
}

static void rtc_backend_destroy(turbo_media_webrtc_backend_peer_t *peer) {
    size_t i;

    if (!peer) return;
    for (i = 0; i < peer->track_count; ++i) {
        peer->tracks[i].active = 0;
    }
    turbo_peer_connection_destroy(peer->peer_connection);
    free(peer);
}

static sdp_media_t *rtc_backend_claim_send_media(
    turbo_media_webrtc_backend_peer_t *peer,
    const turbo_media_webrtc_backend_track_info_t *info,
    const sdp_codec_t **negotiated_codec,
    int *media_index) {
    sdp_media_type_t type;
    int i;
    int j;

    type = info->type == TURBO_MEDIA_WEBRTC_BACKEND_TRACK_AUDIO
               ? SDP_MEDIA_AUDIO
               : SDP_MEDIA_VIDEO;
    for (i = 0; i < peer->remote_sdp.media_count; ++i) {
        sdp_media_t *media = &peer->remote_sdp.media[i];
        if (peer->remote_media_claimed[i] || media->type != type ||
            (media->direction != SDP_DIRECTION_RECVONLY &&
             media->direction != SDP_DIRECTION_SENDRECV)) {
            continue;
        }
        for (j = 0; j < media->codec_count; ++j) {
            turbo_codec_type_t codec;
            if (rtc_backend_ascii_equal(
                    media->codecs[j].name, info->codec_name) &&
                rtc_backend_codec_type(media->codecs[j].name, &codec) == 0) {
                *negotiated_codec = &media->codecs[j];
                *media_index = i;
                return media;
            }
        }
    }
    return NULL;
}

static int rtc_backend_add_send_track(
    turbo_media_webrtc_backend_peer_t *peer,
    const turbo_media_webrtc_backend_track_info_t *info,
    turbo_media_webrtc_backend_track_t **track_out) {
    turbo_media_webrtc_backend_track_t *track;
    turbo_media_track_config_t track_config;
    turbo_media_track_t *media_track;
    const sdp_codec_t *negotiated_codec = NULL;
    sdp_media_t *media;
    turbo_codec_type_t codec;
    int media_index = -1;

    if (!peer || !info || !track_out || !peer->remote_offer_set ||
        info->track_id < 0 || info->payload_type < 0 ||
        info->payload_type > 127 ||
        (info->type != TURBO_MEDIA_WEBRTC_BACKEND_TRACK_AUDIO &&
         info->type != TURBO_MEDIA_WEBRTC_BACKEND_TRACK_VIDEO) ||
        rtc_backend_codec_type(info->codec_name, &codec) != 0 ||
        peer->track_count >= RTC_BACKEND_MAX_TRACKS) {
        return -1;
    }
    media = rtc_backend_claim_send_media(
        peer, info, &negotiated_codec, &media_index);
    if (!media || !negotiated_codec || media_index < 0 ||
        negotiated_codec->payload_type < 0 ||
        negotiated_codec->payload_type > 127) {
        return -1;
    }

    memset(&track_config, 0, sizeof(track_config));
    track_config.type =
        info->type == TURBO_MEDIA_WEBRTC_BACKEND_TRACK_AUDIO
            ? TURBO_RTC_MEDIA_TRACK_AUDIO
            : TURBO_RTC_MEDIA_TRACK_VIDEO;
    track_config.direction = TURBO_MEDIA_DIRECTION_SENDONLY;
    track_config.codec = codec;
    if (track_config.type == TURBO_RTC_MEDIA_TRACK_AUDIO) {
        track_config.audio.sample_rate =
            negotiated_codec->clock_rate > 0
                ? negotiated_codec->clock_rate
                : (info->sample_rate > 0 ? info->sample_rate : 48000);
        track_config.audio.channels =
            negotiated_codec->channels > 0
                ? negotiated_codec->channels
                : (info->channels > 0 ? info->channels : 2);
        track_config.audio.bitrate = RTC_BACKEND_DEFAULT_AUDIO_BITRATE;
        track_config.audio.frame_size_ms = RTC_BACKEND_DEFAULT_AUDIO_FRAME_MS;
    } else {
        track_config.video.width =
            info->width > 0 ? info->width : RTC_BACKEND_DEFAULT_VIDEO_WIDTH;
        track_config.video.height =
            info->height > 0 ? info->height : RTC_BACKEND_DEFAULT_VIDEO_HEIGHT;
        track_config.video.framerate =
            info->framerate > 0
                ? info->framerate
                : RTC_BACKEND_DEFAULT_VIDEO_FRAMERATE;
        track_config.video.bitrate = RTC_BACKEND_DEFAULT_VIDEO_BITRATE;
        track_config.video.keyframe_interval =
            RTC_BACKEND_DEFAULT_KEYFRAME_INTERVAL;
    }

    media_track = turbo_peer_connection_add_track_ex(
        peer->peer_connection, &track_config);
    if (!media_track) {
        TLOG_ERROR("WebRTC internal backend failed to add send track");
        return -1;
    }
    turbo_media_track_set_payload_type(
        media_track, (uint8_t)negotiated_codec->payload_type);
    track = rtc_backend_reserve_track(peer);
    if (!track) return -1;
    track->media_track = media_track;
    track->info = *info;
    track->info.payload_type = negotiated_codec->payload_type;
    track->info.clock_rate = negotiated_codec->clock_rate;
    strncpy(track->info.codec_name,
            rtc_backend_codec_name(codec),
            sizeof(track->info.codec_name) - 1);
    peer->remote_media_claimed[media_index] = 1;
    *track_out = track;
    return 0;
}

static int rtc_backend_set_remote_offer(
    turbo_media_webrtc_backend_peer_t *peer,
    const char *offer_sdp) {
    size_t length;

    if (!peer || !offer_sdp || !offer_sdp[0] ||
        peer->remote_offer_set) {
        return -1;
    }
    length = strlen(offer_sdp);
    if (sdp_parse(offer_sdp, length, &peer->remote_sdp) != 0 ||
        peer->remote_sdp.media_count <= 0) {
        return -1;
    }
    peer->remote_offer_set = 1;
    if (turbo_peer_connection_set_remote_description(
            peer->peer_connection, "offer", offer_sdp) != 0 ||
        peer->failed) {
        TLOG_ERRORF("WebRTC internal backend rejected remote offer failed={}",
                   peer->failed);
        return -1;
    }
    return 0;
}

static int rtc_backend_create_answer(
    turbo_media_webrtc_backend_peer_t *peer,
    char *answer_sdp,
    size_t answer_sdp_capacity,
    size_t *answer_sdp_length) {
    int answer_length;

    if (!peer || !peer->remote_offer_set || !answer_sdp ||
        answer_sdp_capacity == 0 || peer->failed) {
        return -1;
    }
    answer_length = turbo_peer_connection_create_answer(
        peer->peer_connection, answer_sdp, answer_sdp_capacity);
    if (answer_length <= 0) {
        TLOG_ERROR("WebRTC internal backend failed to create SDP answer");
        return -1;
    }
    if (answer_sdp_length) *answer_sdp_length = (size_t)answer_length;
    return 0;
}

static int rtc_backend_add_ice_candidate(
    turbo_media_webrtc_backend_peer_t *peer,
    const char *candidate) {
    if (!peer || !peer->remote_offer_set || !candidate || !candidate[0] ||
        peer->failed) {
        return -1;
    }
    return turbo_peer_connection_add_ice_candidate(
        peer->peer_connection, candidate);
}

static int rtc_backend_start_track(
    turbo_media_webrtc_backend_track_t *track) {
    if (!track || !track->active || track->remote || !track->media_track) {
        return -1;
    }
    return turbo_media_track_start(track->media_track);
}

static int rtc_backend_send_rtp(
    turbo_media_webrtc_backend_track_t *track,
    const uint8_t *packet,
    size_t packet_len) {
    if (!track || !track->active || track->remote || !track->media_track ||
        !packet || packet_len > track->peer->max_rtp_packet_bytes ||
        track->peer->failed) {
        return -1;
    }
    return turbo_media_track_send_rtp_packet(
        track->media_track, packet, packet_len);
}

static int rtc_backend_pump(turbo_media_webrtc_backend_peer_t *peer) {
    if (!peer || peer->failed) return -1;
    turbo_peer_connection_poll(peer->peer_connection);
    return peer->failed ? -1 : 0;
}

static const turbo_media_webrtc_backend_ops_t RTC_BACKEND_OPS = {
    rtc_backend_create,
    rtc_backend_destroy,
    rtc_backend_add_send_track,
    rtc_backend_set_remote_offer,
    rtc_backend_create_answer,
    rtc_backend_add_ice_candidate,
    rtc_backend_start_track,
    rtc_backend_send_rtp,
    rtc_backend_pump
};

const turbo_media_webrtc_backend_ops_t *
turbo_media_webrtc_internal_backend(void) {
    return &RTC_BACKEND_OPS;
}
