/**
 * @file turbo_peer_connection.c
 * @brief High-level WebRTC Peer Connection Implementation
 */

#include "turbo_peer_connection.h"
#include "turbo_rtp.h"
#include "turbo_sdp.h"
#include "ice/turbo_ice.h"
#include "tlog.h"
#include <platform.h>
#include <turbo_coro_context.h>
#include <salts_str.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TURBO_TRANSPORT_CC_URI "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01"
#define TURBO_STUN_SCHEME "stun:"
#define TURBO_TURN_SCHEME "turn:"
#define TURBO_ICE_SDPFRAG_MAX_LENGTH 65536u
#define TURBO_ICE_SDPFRAG_CANDIDATE_CAPACITY 512u

struct turbo_peer_connection_s {
    turbo_peer_config_t config;

    turbo_peer_callbacks_t callbacks;
    
    turbo_ice_agent_t *ice_agent;
    coro_context_t *ice_ctx;
    turbo_dc_context_t *dc_ctx;
    turbo_dc_peer_t *dc_peer;
    turbo_media_context_t *media_ctx;
    
    turbo_peer_state_t state;
    
    /* Local credentials */
    tstr local_ufrag;
    tstr local_pwd;
    
    /* Remote info */
    tstr remote_fingerprint;
    tstr remote_ufrag;
    tstr remote_pwd;
    sdp_session_t remote_sdp;
    int have_remote_sdp;
    
    /* Internal */
    int ice_connected;
    int dtls_connected;
    int dtls_connect_pending;
    int gathering_start_pending;
    int checks_start_pending;
    int remote_candidate_count;
    int have_ice_role;
    int have_dtls_role;
    int ice_controlling;
    int dtls_server;
    int local_ice_restart_pending;
    turbo_media_track_t *notified_remote_tracks[TURBO_MEDIA_MAX_TRACKS];
    int notified_remote_track_count;
    int destroying;
};

static int copy_ice_server_url(char *destination, size_t destination_size,
                               const char *source, const char *scheme) {
    size_t length;

    if (!destination || destination_size == 0 || !source || !scheme ||
        strncmp(source, scheme, strlen(scheme)) != 0) {
        return -1;
    }
    length = strlen(source);
    if (length == 0 || length >= destination_size) {
        return -1;
    }
    memcpy(destination, source, length + 1);
    return 0;
}

static int configure_turn_server(ice_server_t *destination, const char *source) {
    const char *username;
    const char *username_end;
    const char *password;
    const char *password_end;
    const char *host;
    size_t username_length;
    size_t password_length;
    int written;

    if (!destination || !source ||
        strncmp(source, TURBO_TURN_SCHEME, strlen(TURBO_TURN_SCHEME)) != 0) {
        return -1;
    }

    username = source + strlen(TURBO_TURN_SCHEME);
    username_end = strchr(username, ':');
    if (!username_end || username_end == username) {
        return -1;
    }
    password = username_end + 1;
    password_end = strchr(password, '@');
    if (!password_end || password_end == password || password_end[1] == '\0') {
        return -1;
    }
    host = password_end + 1;
    username_length = (size_t)(username_end - username);
    password_length = (size_t)(password_end - password);
    if (username_length >= sizeof(destination->username) ||
        password_length >= sizeof(destination->credential)) {
        return -1;
    }

    written = snprintf(destination->url, sizeof(destination->url),
                       "%s%s", TURBO_TURN_SCHEME, host);
    if (written < 0 || (size_t)written >= sizeof(destination->url)) {
        return -1;
    }
    memcpy(destination->username, username, username_length);
    destination->username[username_length] = '\0';
    memcpy(destination->credential, password, password_length);
    destination->credential[password_length] = '\0';
    return 0;
}

static int configure_ice_servers(const turbo_peer_config_t *source,
                                 ice_config_t *destination) {
    int i;

    if (!source || !destination ||
        source->stun_server_count < 0 ||
        source->stun_server_count > ICE_MAX_STUN_SERVERS ||
        source->turn_server_count < 0 ||
        source->turn_server_count > ICE_MAX_TURN_SERVERS ||
        (source->stun_server_count > 0 && !source->stun_servers) ||
        (source->turn_server_count > 0 && !source->turn_servers)) {
        return -1;
    }

    for (i = 0; i < source->stun_server_count; ++i) {
        if (copy_ice_server_url(destination->stun_servers[i].url,
                                sizeof(destination->stun_servers[i].url),
                                source->stun_servers[i],
                                TURBO_STUN_SCHEME) != 0) {
            return -1;
        }
        destination->stun_server_count++;
    }
    for (i = 0; i < source->turn_server_count; ++i) {
        if (configure_turn_server(&destination->turn_servers[i],
                                  source->turn_servers[i]) != 0) {
            return -1;
        }
        destination->turn_server_count++;
    }
    return 0;
}

static int refresh_local_ice_credentials(turbo_peer_connection_t *pc) {
    char ufrag[32];
    char pwd[64];
    tstr next_ufrag;
    tstr next_pwd;

    if (!pc || !pc->ice_agent) {
        return -1;
    }

    ice_agent_get_local_credentials(
        pc->ice_agent, ufrag, sizeof(ufrag), pwd, sizeof(pwd));
    if (!ufrag[0] || !pwd[0]) {
        return -1;
    }

    next_ufrag = tstr_dup(ufrag);
    next_pwd = tstr_dup(pwd);
    if (!next_ufrag || !next_pwd) {
        tstr_free(next_ufrag);
        tstr_free(next_pwd);
        return -1;
    }

    tstr_free(pc->local_ufrag);
    tstr_free(pc->local_pwd);
    pc->local_ufrag = next_ufrag;
    pc->local_pwd = next_pwd;
    return 0;
}

/* ============================================================================
 * Internal Callbacks
 * ============================================================================ */

static void on_ice_candidate(turbo_ice_agent_t *agent, const ice_candidate_t *candidate, void *user_data) {
    turbo_peer_connection_t *pc = (turbo_peer_connection_t *)user_data;
    (void)agent;

    if (!pc || pc->destroying) {
        return;
    }

    if (pc->callbacks.on_ice_candidate) {
        char sdp_line[512];
        ice_candidate_to_sdp(candidate, sdp_line, sizeof(sdp_line));
        pc->callbacks.on_ice_candidate(pc, sdp_line, pc->config.user_data);
    }
}

static void notify_state_change(turbo_peer_connection_t *pc, turbo_peer_state_t state) {
    if (!pc || pc->destroying) {
        return;
    }

    if (pc->state == state) {
        return;
    }
    pc->state = state;
    if (pc->callbacks.on_state_change) {
        pc->callbacks.on_state_change(pc, state, pc->config.user_data);
    }
}

static int notify_remote_track_once(turbo_peer_connection_t *pc,
                                    turbo_media_track_t *track) {
    if (!pc || !track || pc->destroying) {
        return -1;
    }
    for (int i = 0; i < pc->notified_remote_track_count; ++i) {
        if (pc->notified_remote_tracks[i] == track) {
            return 0;
        }
    }
    if (pc->notified_remote_track_count >= TURBO_MEDIA_MAX_TRACKS) {
        return -1;
    }

    pc->notified_remote_tracks[pc->notified_remote_track_count++] = track;
    if (pc->callbacks.on_track) {
        pc->callbacks.on_track(pc, track, pc->config.user_data);
    }
    return 0;
}

static void pump_ice_context(turbo_peer_connection_t *pc) {
    if (pc && pc->ice_ctx) {
        coro_context_run(pc->ice_ctx, TURBO_RUN_NOWAIT);
    }
}

static void drain_ice_coroutines(turbo_peer_connection_t *pc) {
    if (!pc || !pc->ice_ctx) {
        return;
    }

    /* The ICE agent is owned by its coroutines until every task has unwound. */
    while (coro_context_coro_count(pc->ice_ctx) > 0) {
        coro_context_run(pc->ice_ctx, TURBO_RUN_ONCE);
    }
}

static void start_gathering_task(coro_t *co, void *arg) {
    turbo_peer_connection_t *pc = (turbo_peer_connection_t *)arg;
    int rc;
    (void)co;

    if (!pc || pc->destroying || !pc->ice_agent) {
        if (pc) {
            pc->gathering_start_pending = 0;
        }
        return;
    }

    rc = ice_agent_gather_candidates(pc->ice_agent);
    pc->gathering_start_pending = 0;
    if (rc != 0 && !pc->destroying) {
        notify_state_change(pc, TURBO_PEER_STATE_FAILED);
    }
}

static void start_checks_task(coro_t *co, void *arg) {
    turbo_peer_connection_t *pc = (turbo_peer_connection_t *)arg;
    int rc;
    (void)co;

    if (!pc || pc->destroying || !pc->ice_agent) {
        if (pc) {
            pc->checks_start_pending = 0;
        }
        return;
    }

    rc = ice_agent_start_checks(pc->ice_agent);
    pc->checks_start_pending = 0;
    if (rc != 0 && !pc->destroying && pc->state != TURBO_PEER_STATE_CLOSED) {
        notify_state_change(pc, TURBO_PEER_STATE_FAILED);
    }
}

static int sdp_candidate_to_ice_line(const sdp_candidate_t *candidate, char *buffer, size_t buffer_len) {
    int written;

    if (!candidate || !buffer || buffer_len == 0) {
        return -1;
    }

    written = snprintf(buffer, buffer_len, "candidate:%s %d %s %u %s %u typ %s",
                       candidate->foundation,
                       candidate->component,
                       candidate->transport[0] ? candidate->transport : "udp",
                       candidate->priority,
                       candidate->address,
                       candidate->port,
                       candidate->type);
    if (written < 0 || (size_t)written >= buffer_len) {
        return -1;
    }

    if (candidate->rel_addr[0] && candidate->rel_port != 0) {
        int extra = snprintf(buffer + written, buffer_len - (size_t)written, " raddr %s rport %u",
                             candidate->rel_addr, candidate->rel_port);
        if (extra < 0 || (size_t)extra >= buffer_len - (size_t)written) {
            return -1;
        }
        written += extra;
    }

    return written;
}

static int import_remote_candidates(turbo_peer_connection_t *pc, const sdp_session_t *remote_sdp) {
    int added = 0;

    if (!pc || !pc->ice_agent || !remote_sdp) {
        return 0;
    }

    for (int i = 0; i < remote_sdp->media_count; i++) {
        const sdp_media_t *media = &remote_sdp->media[i];

        for (int j = 0; j < media->candidate_count; j++) {
            char candidate_line[256];

            if (sdp_candidate_to_ice_line(&media->candidates[j], candidate_line, sizeof(candidate_line)) < 0) {
                continue;
            }
            if (ice_agent_add_remote_candidate(pc->ice_agent, candidate_line) == 0) {
                added++;
            }
        }
    }

    pc->remote_candidate_count += added;
    return added;
}

static int maybe_start_checks(turbo_peer_connection_t *pc) {
    ice_state_t ice_state;

    if (!pc || pc->destroying || !pc->ice_agent) {
        return -1;
    }

    if (!pc->remote_ufrag || !pc->remote_ufrag[0] || pc->remote_candidate_count == 0) {
        return 0;
    }

    if (ice_agent_get_gathering_state(pc->ice_agent) != ICE_GATHERING_COMPLETE) {
        return 0;
    }

    ice_state = ice_agent_get_state(pc->ice_agent);
    if (pc->checks_start_pending ||
        ice_state == ICE_STATE_CONNECTING ||
        ice_state == ICE_STATE_CONNECTED ||
        ice_state == ICE_STATE_COMPLETED) {
        return 0;
    }

    notify_state_change(pc, TURBO_PEER_STATE_CONNECTING);
    pc->checks_start_pending = 1;
    if (coro_context_spawn(pc->ice_ctx, start_checks_task, pc) != 0) {
        pc->checks_start_pending = 0;
        return -1;
    }

    pump_ice_context(pc);
    return 0;
}

static void maybe_start_dtls(turbo_peer_connection_t *pc) {
    if (!pc || pc->destroying || !pc->dtls_connect_pending || !pc->dc_peer) {
        return;
    }

    pc->dtls_connect_pending = 0;
    if (turbo_dc_peer_connect(pc->dc_peer) != 0) {
        notify_state_change(pc, TURBO_PEER_STATE_FAILED);
    }
}

static void send_dc_transport(void *transport, const void *data, size_t len) {
    turbo_peer_connection_t *pc = (turbo_peer_connection_t *)transport;

    if (!pc || pc->destroying || !pc->ice_agent) {
        return;
    }
    if (ice_agent_send(pc->ice_agent, data, len) != 0) {
        ice_state_t ice_state = ice_agent_get_state(pc->ice_agent);
        pc->ice_connected = 0;
        notify_state_change(
            pc, ice_state == ICE_STATE_DISCONNECTED
                    ? TURBO_PEER_STATE_DISCONNECTED
                    : TURBO_PEER_STATE_FAILED);
    }
}



static void on_ice_state_change(turbo_ice_agent_t *agent, ice_state_t old_state,
                                ice_state_t new_state, void *user_data) {
    turbo_peer_connection_t *pc = (turbo_peer_connection_t *)user_data;
    (void)agent; (void)old_state;

    if (!pc || pc->destroying) {
        return;
    }

    if (new_state == ICE_STATE_CONNECTED || new_state == ICE_STATE_COMPLETED) {
        if (!pc->ice_connected) {
            pc->ice_connected = 1;
            /* Bridge ICE and DataChannel if ready */
            if (pc->dc_peer) {
                if (turbo_dc_peer_set_external_transport(
                        pc->dc_peer, pc, send_dc_transport) != 0) {
                    pc->ice_connected = 0;
                    notify_state_change(pc, TURBO_PEER_STATE_FAILED);
                    return;
                }
                if (pc->dtls_connected) {
                    notify_state_change(pc, TURBO_PEER_STATE_CONNECTED);
                } else {
                    pc->dtls_connect_pending = 1;
                }
            }
        }
    } else if (new_state == ICE_STATE_DISCONNECTED) {
        pc->ice_connected = 0;
        pc->dtls_connect_pending = 0;
        notify_state_change(pc, TURBO_PEER_STATE_DISCONNECTED);
    } else if (new_state == ICE_STATE_FAILED) {
        pc->ice_connected = 0;
        pc->dtls_connect_pending = 0;
        notify_state_change(pc, TURBO_PEER_STATE_FAILED);
    }

}

static void on_ice_data(turbo_ice_agent_t *agent, const void *data, size_t len, void *user_data) {
    turbo_peer_connection_t *pc = (turbo_peer_connection_t *)user_data;
    (void)agent;

    if (!pc || pc->destroying) {
        return;
    }

    if (len == 0) return;
    uint8_t first_byte = ((const uint8_t *)data)[0];

    if (first_byte >= 128 && first_byte <= 191) {
        /* RTP/RTCP */
        if (pc->media_ctx) {
            turbo_media_feed_data(pc->media_ctx, data, len);
        }
    } else if (first_byte >= 20 && first_byte <= 63) {
        /* DTLS */
        if (pc->dc_peer) {
            turbo_dc_peer_feed_transport_data(pc->dc_peer, data, len);
        }
    }
}

static void on_dc_state(turbo_dc_peer_t *peer, turbo_dc_state_t old_state,
                        turbo_dc_state_t new_state, void *user_data) {
    turbo_peer_connection_t *pc = (turbo_peer_connection_t *)user_data;
    int track_count;
    (void)peer; (void)old_state;

    if (!pc || pc->destroying) {
        return;
    }

    if (new_state == TURBO_DC_STATE_CONNECTED) {
        pc->dtls_connected = 1;
        /* Initialize SRTP for media */
        if (pc->media_ctx) {
             if (turbo_media_setup_srtp(pc->media_ctx) != 0) {
                 TLOG_ERROR("PeerConnection SRTP setup failed");
                 notify_state_change(pc, TURBO_PEER_STATE_FAILED);
                 return;
             }

             track_count = turbo_media_get_track_count(pc->media_ctx);
             for (int i = 0; i < track_count; i++) {
                 turbo_media_track_t *track = turbo_media_get_track(pc->media_ctx, i);
                 if (!track) {
                     continue;
                 }
                 /* Remote tracks are safe to activate after SRTP keys exist.
                  * Send tracks may own cameras/screens and remain app-controlled. */
                 if (turbo_media_track_get_direction(track) & TURBO_MEDIA_DIRECTION_RECVONLY) {
                     if (turbo_media_track_get_state(track) != TURBO_MEDIA_STATE_ACTIVE &&
                         turbo_media_track_start(track) != 0) {
                         TLOG_ERRORF("PeerConnection failed to start recv track[{}]", i);
                         notify_state_change(pc, TURBO_PEER_STATE_FAILED);
                         return;
                     }
                 }
             }
         }

        notify_state_change(pc, TURBO_PEER_STATE_CONNECTED);
    } else if (new_state == TURBO_DC_STATE_FAILED) {
        pc->dtls_connected = 0;
        notify_state_change(pc, TURBO_PEER_STATE_FAILED);
    } else if (new_state == TURBO_DC_STATE_CLOSED) {
        pc->dtls_connected = 0;
        notify_state_change(pc, TURBO_PEER_STATE_DISCONNECTED);
    }

}

static void on_dc_channel(turbo_dc_peer_t *peer, turbo_dc_channel_t *channel, void *user_data) {
    turbo_peer_connection_t *pc = (turbo_peer_connection_t *)user_data;
    (void)peer;

    if (!pc || pc->destroying) {
        return;
    }

    if (pc->callbacks.on_datachannel) {
        pc->callbacks.on_datachannel(pc, channel, pc->config.user_data);
    }
}

static int configure_negotiated_transport_roles(
    turbo_peer_connection_t *pc,
    int ice_controlling,
    int dtls_server,
    int dtls_role_is_final
) {
    int normalized_ice_role;
    int normalized_dtls_role;

    if (!pc) {
        return -1;
    }

    normalized_ice_role = ice_controlling ? 1 : 0;
    normalized_dtls_role = dtls_server ? 1 : 0;

    if (pc->have_dtls_role && dtls_role_is_final &&
        pc->dtls_server != normalized_dtls_role &&
        turbo_dc_peer_get_state(pc->dc_peer) != TURBO_DC_STATE_NEW) {
        return -1;
    }
    if (pc->have_ice_role && pc->ice_controlling != normalized_ice_role) {
        if (!pc->ice_agent ||
            ice_agent_set_role(pc->ice_agent, normalized_ice_role) != 0) {
            return -1;
        }
        pc->ice_controlling = normalized_ice_role;
    } else if (!pc->have_ice_role && pc->ice_agent &&
        ice_agent_set_role(pc->ice_agent, normalized_ice_role) != 0) {
        return -1;
    }
    pc->have_ice_role = 1;
    pc->ice_controlling = normalized_ice_role;

    if ((!pc->have_dtls_role ||
         (dtls_role_is_final && pc->dtls_server != normalized_dtls_role)) &&
        pc->dc_peer &&
        turbo_dc_peer_set_dtls_role(pc->dc_peer, normalized_dtls_role) != 0) {
        return -1;
    }
    if (!pc->have_dtls_role || dtls_role_is_final) {
        pc->have_dtls_role = 1;
        pc->dtls_server = normalized_dtls_role;
    }
    return 0;
}

static int restart_ice_generation(turbo_peer_connection_t *pc,
                                  int locally_initiated) {
    ice_restart_options_t options;

    if (!pc || pc->destroying || !pc->ice_agent ||
        pc->state == TURBO_PEER_STATE_CLOSED ||
        pc->checks_start_pending) {
        return -1;
    }

    pump_ice_context(pc);
    options = ice_restart_options_default();
    {
        int restart_result = ice_agent_restart(pc->ice_agent, &options);
        if (restart_result != 0) {
            return -1;
        }
    }
    if (refresh_local_ice_credentials(pc) != 0) {
        pc->ice_connected = 0;
        pc->dtls_connect_pending = 0;
        notify_state_change(pc, TURBO_PEER_STATE_FAILED);
        return -1;
    }

    pc->ice_connected = 0;
    pc->dtls_connect_pending = 0;
    pc->remote_candidate_count = 0;
    pc->local_ice_restart_pending = locally_initiated ? 1 : 0;
    notify_state_change(pc, TURBO_PEER_STATE_CONNECTING);
    return 0;
}

static sdp_direction_t media_direction_to_sdp(turbo_media_direction_t direction) {
    switch (direction) {
        case TURBO_MEDIA_DIRECTION_SENDONLY:
            return SDP_DIRECTION_SENDONLY;
        case TURBO_MEDIA_DIRECTION_RECVONLY:
            return SDP_DIRECTION_RECVONLY;
        case TURBO_MEDIA_DIRECTION_SENDRECV:
        default:
            return SDP_DIRECTION_SENDRECV;
    }
}

static int peer_ascii_equal_ignore_case(const char *left, const char *right) {
    unsigned char a;
    unsigned char b;

    if (!left || !right) return 0;
    while (*left && *right) {
        a = (unsigned char)*left++;
        b = (unsigned char)*right++;
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
        if (a != b) return 0;
    }
    return *left == '\0' && *right == '\0';
}

static int sdp_codec_to_media_codec(
    const sdp_codec_t *codec,
    turbo_rtc_media_track_type_t type,
    turbo_codec_type_t *codec_type
) {
    if (!codec || !codec_type) return -1;
    if (type == TURBO_RTC_MEDIA_TRACK_AUDIO) {
        if (peer_ascii_equal_ignore_case(codec->name, "opus")) {
            *codec_type = TURBO_CODEC_OPUS;
            return 0;
        }
        if (peer_ascii_equal_ignore_case(codec->name, "PCMU")) {
            *codec_type = TURBO_CODEC_PCMU;
            return 0;
        }
        if (peer_ascii_equal_ignore_case(codec->name, "PCMA")) {
            *codec_type = TURBO_CODEC_PCMA;
            return 0;
        }
        return -1;
    }

    if (peer_ascii_equal_ignore_case(codec->name, "VP8")) {
        *codec_type = TURBO_CODEC_VP8;
        return 0;
    }
    if (peer_ascii_equal_ignore_case(codec->name, "VP9")) {
        *codec_type = TURBO_CODEC_VP9;
        return 0;
    }
    if (peer_ascii_equal_ignore_case(codec->name, "H264")) {
        *codec_type = TURBO_CODEC_H264;
        return 0;
    }
    if (peer_ascii_equal_ignore_case(codec->name, "H265") ||
        peer_ascii_equal_ignore_case(codec->name, "HEVC")) {
        *codec_type = TURBO_CODEC_H265;
        return 0;
    }

    return -1;
}

static int fill_remote_track_config(
    const sdp_media_t *media,
    const sdp_codec_t *codec,
    turbo_media_track_config_t *config
) {
    if (!media || !codec || !config) {
        return -1;
    }

    memset(config, 0, sizeof(*config));
    config->type = media->type == SDP_MEDIA_AUDIO
        ? TURBO_RTC_MEDIA_TRACK_AUDIO
        : TURBO_RTC_MEDIA_TRACK_VIDEO;
    config->direction = TURBO_MEDIA_DIRECTION_RECVONLY;
    if (sdp_codec_to_media_codec(codec, config->type, &config->codec) != 0) {
        return -1;
    }

    if (config->type == TURBO_RTC_MEDIA_TRACK_AUDIO) {
        config->audio.sample_rate = codec->clock_rate > 0 ? codec->clock_rate : 48000;
        config->audio.channels = codec->channels > 0 ? codec->channels : 2;
        config->audio.bitrate = 64000;
        config->audio.frame_size_ms = 20;
    } else {
        config->video.width = 1280;
        config->video.height = 720;
        config->video.framerate = 30;
        config->video.bitrate = 1500000;
        config->video.keyframe_interval = 30;
    }
    return 0;
}

static int fill_sdp_codec(turbo_codec_type_t codec_type, sdp_codec_t *codec) {
    if (!codec) {
        return -1;
    }

    memset(codec, 0, sizeof(*codec));
    codec->payload_type = (int)codec_type;
    codec->supports_nack = 1;
    codec->supports_transport_cc = 1;

    switch (codec_type) {
        case TURBO_CODEC_OPUS:
            strcpy(codec->name, "opus");
            codec->clock_rate = 48000;
            codec->channels = 2;
            strcpy(codec->fmtp, "minptime=10;useinbandfec=1");
            return 0;
        case TURBO_CODEC_PCMU:
            strcpy(codec->name, "PCMU");
            codec->clock_rate = 8000;
            codec->channels = 1;
            return 0;
        case TURBO_CODEC_PCMA:
            strcpy(codec->name, "PCMA");
            codec->clock_rate = 8000;
            codec->channels = 1;
            return 0;
        case TURBO_CODEC_VP8:
            strcpy(codec->name, "VP8");
            codec->clock_rate = 90000;
            codec->supports_pli = 1;
            codec->supports_fir = 1;
            codec->supports_remb = 1;
            return 0;
        case TURBO_CODEC_VP9:
            strcpy(codec->name, "VP9");
            codec->clock_rate = 90000;
            codec->supports_pli = 1;
            codec->supports_fir = 1;
            codec->supports_remb = 1;
            return 0;
        case TURBO_CODEC_H264:
            strcpy(codec->name, "H264");
            codec->clock_rate = 90000;
            codec->supports_pli = 1;
            codec->supports_fir = 1;
            codec->supports_remb = 1;
            strcpy(codec->fmtp, "packetization-mode=1;profile-level-id=42e01f;level-asymmetry-allowed=1");
            return 0;
        case TURBO_CODEC_H265:
            strcpy(codec->name, "H265");
            codec->clock_rate = 90000;
            codec->supports_pli = 1;
            codec->supports_fir = 1;
            codec->supports_remb = 1;
            return 0;
        default:
            return -1;
    }
}

static const sdp_codec_t *find_offered_codec(
    const sdp_media_t *media,
    turbo_codec_type_t codec_type
) {
    sdp_codec_t local_codec;

    if (!media || fill_sdp_codec(codec_type, &local_codec) != 0) {
        return NULL;
    }
    for (int i = 0; i < media->codec_count; ++i) {
        if (peer_ascii_equal_ignore_case(
                media->codecs[i].name, local_codec.name)) {
            return &media->codecs[i];
        }
    }
    return NULL;
}

static const sdp_codec_t *find_first_supported_codec(
    const sdp_media_t *media,
    turbo_rtc_media_track_type_t type
) {
    turbo_codec_type_t codec_type;

    if (!media) return NULL;
    for (int i = 0; i < media->codec_count; ++i) {
        if (sdp_codec_to_media_codec(
                &media->codecs[i], type, &codec_type) == 0) {
            return &media->codecs[i];
        }
    }
    return NULL;
}

static int sdp_media_transport_cc_ext_id(const sdp_media_t *media) {
    if (!media) {
        return 0;
    }

    for (int i = 0; i < media->extension_count; i++) {
        const sdp_extension_t *ext = &media->extensions[i];
        if (ext->id >= 1 && ext->id <= 14 && strstr(ext->uri, "transport-wide-cc")) {
            return ext->id;
        }
    }

    return 0;
}

static void add_common_media_security(turbo_peer_connection_t *pc, sdp_media_t *media,
                                      sdp_setup_role_t setup_role) {
    char hash[16];
    char fp[128];

    if (!pc || !media) {
        return;
    }

    sdp_media_set_ice(media, pc->local_ufrag, pc->local_pwd);
    media->setup = setup_role;

    if (turbo_dc_context_get_local_fingerprint(pc->dc_ctx, hash, sizeof(hash), fp, sizeof(fp)) == 0) {
        sdp_media_set_fingerprint(media, hash, fp);
    }
}

static void add_local_candidates_to_media(turbo_peer_connection_t *pc, sdp_media_t *media) {
    int count;

    if (!pc || !pc->ice_agent || !media) {
        return;
    }

    count = ice_agent_get_local_candidate_count(pc->ice_agent);
    for (int i = 0; i < count; i++) {
        ice_candidate_t candidate;
        sdp_candidate_t sdp_candidate;

        if (ice_agent_get_local_candidate(pc->ice_agent, i, &candidate) != 0) {
            continue;
        }
        memset(&sdp_candidate, 0, sizeof(sdp_candidate));
        strncpy(sdp_candidate.foundation, candidate.foundation, sizeof(sdp_candidate.foundation) - 1);
        sdp_candidate.component = candidate.component_id;
        strncpy(sdp_candidate.transport,
                candidate.transport == ICE_TRANSPORT_TCP ? "tcp" : "udp",
                sizeof(sdp_candidate.transport) - 1);
        sdp_candidate.priority = candidate.priority;
        strncpy(sdp_candidate.address, candidate.ip, sizeof(sdp_candidate.address) - 1);
        sdp_candidate.port = candidate.port;
        strncpy(sdp_candidate.type, ice_candidate_type_name(candidate.type), sizeof(sdp_candidate.type) - 1);
        strncpy(sdp_candidate.rel_addr, candidate.related_ip, sizeof(sdp_candidate.rel_addr) - 1);
        sdp_candidate.rel_port = candidate.related_port;
        sdp_media_add_candidate(media, &sdp_candidate);
    }
}

static sdp_direction_t negotiate_answer_direction(turbo_media_direction_t local_direction,
                                                  sdp_direction_t remote_direction) {
    int local_can_send = (local_direction & TURBO_MEDIA_DIRECTION_SENDONLY) != 0;
    int local_can_recv = (local_direction & TURBO_MEDIA_DIRECTION_RECVONLY) != 0;
    int remote_can_send = (remote_direction == SDP_DIRECTION_SENDRECV ||
                           remote_direction == SDP_DIRECTION_SENDONLY);
    int remote_can_recv = (remote_direction == SDP_DIRECTION_SENDRECV ||
                           remote_direction == SDP_DIRECTION_RECVONLY);
    int answer_can_send = local_can_send && remote_can_recv;
    int answer_can_recv = local_can_recv && remote_can_send;

    if (answer_can_send && answer_can_recv) {
        return SDP_DIRECTION_SENDRECV;
    }
    if (answer_can_send) {
        return SDP_DIRECTION_SENDONLY;
    }
    if (answer_can_recv) {
        return SDP_DIRECTION_RECVONLY;
    }
    return SDP_DIRECTION_INACTIVE;
}

static int sdp_direction_can_send(sdp_direction_t direction) {
    return direction == SDP_DIRECTION_SENDRECV || direction == SDP_DIRECTION_SENDONLY;
}

static int sdp_direction_can_recv(sdp_direction_t direction) {
    return direction == SDP_DIRECTION_SENDRECV || direction == SDP_DIRECTION_RECVONLY;
}

static turbo_media_track_t *find_track_by_type(turbo_media_context_t *ctx, turbo_rtc_media_track_type_t type) {
    int count = turbo_media_get_track_count(ctx);

    for (int i = 0; i < count; i++) {
        turbo_media_track_t *track = turbo_media_get_track(ctx, i);
        if (track && turbo_media_track_get_type(track) == type) {
            return track;
        }
    }

    return NULL;
}

static int track_is_claimed(turbo_media_track_t *track,
                            turbo_media_track_t *const *claimed_tracks,
                            int claimed_track_count) {
    for (int i = 0; i < claimed_track_count; ++i) {
        if (claimed_tracks[i] == track) {
            return 1;
        }
    }
    return 0;
}

static turbo_media_track_t *find_track_for_remote_media(
    turbo_media_context_t *ctx,
    const sdp_media_t *remote_media,
    turbo_media_track_t *const *claimed_tracks,
    int claimed_track_count,
    const sdp_codec_t **negotiated_codec_out) {
    int count = turbo_media_get_track_count(ctx);
    turbo_rtc_media_track_type_t type;
    int need_send;
    int need_recv;

    if (negotiated_codec_out) {
        *negotiated_codec_out = NULL;
    }
    if (!ctx || !remote_media) {
        return NULL;
    }
    if (remote_media->type == SDP_MEDIA_AUDIO) {
        type = TURBO_RTC_MEDIA_TRACK_AUDIO;
    } else if (remote_media->type == SDP_MEDIA_VIDEO) {
        type = TURBO_RTC_MEDIA_TRACK_VIDEO;
    } else {
        return NULL;
    }

    need_send = sdp_direction_can_recv(remote_media->direction);
    need_recv = sdp_direction_can_send(remote_media->direction);

    if (!need_send && !need_recv) {
        return NULL;
    }

    for (int i = 0; i < count; i++) {
        turbo_media_track_t *track = turbo_media_get_track(ctx, i);
        turbo_media_direction_t direction;

        const sdp_codec_t *negotiated_codec;

        if (!track || turbo_media_track_get_type(track) != type ||
            track_is_claimed(track, claimed_tracks, claimed_track_count)) {
            continue;
        }

        direction = turbo_media_track_get_direction(track);
        if (need_send && !(direction & TURBO_MEDIA_DIRECTION_SENDONLY)) {
            continue;
        }
        if (need_recv && !(direction & TURBO_MEDIA_DIRECTION_RECVONLY)) {
            continue;
        }

        negotiated_codec = find_offered_codec(
            remote_media, turbo_media_track_get_codec(track));
        if (!negotiated_codec) {
            continue;
        }
        if (negotiated_codec_out) {
            *negotiated_codec_out = negotiated_codec;
        }
        return track;
    }

    return NULL;
}

static const sdp_media_t *find_media_by_mid(const sdp_session_t *sdp,
                                            const char *mid) {
    if (!sdp || !mid || !mid[0]) {
        return NULL;
    }
    for (int i = 0; i < sdp->media_count; ++i) {
        if (strcmp(sdp->media[i].mid, mid) == 0) {
            return &sdp->media[i];
        }
    }
    return NULL;
}

static int media_is_in_bundle(const sdp_session_t *sdp,
                              const sdp_media_t *media) {
    if (!sdp || !media || !media->mid[0]) {
        return 0;
    }
    for (int i = 0; i < sdp->bundle_count; ++i) {
        if (strcmp(sdp->bundle_mids[i], media->mid) == 0) {
            return 1;
        }
    }
    return 0;
}

static const sdp_media_t *select_remote_transport_media(
    const sdp_session_t *sdp) {
    const sdp_media_t *first_active = NULL;
    const sdp_media_t *bundle_tag = NULL;
    int active_count = 0;

    if (!sdp) {
        return NULL;
    }

    for (int i = 0; i < sdp->media_count; ++i) {
        const sdp_media_t *media = &sdp->media[i];
        if (media->port == 0) {
            continue;
        }
        if (!first_active) {
            first_active = media;
        }
        ++active_count;
    }
    if (active_count == 0) {
        return NULL;
    }
    if (sdp->bundle_count == 0) {
        return active_count == 1 ? first_active : NULL;
    }

    bundle_tag = find_media_by_mid(sdp, sdp->bundle_mids[0]);
    if (!bundle_tag || bundle_tag->port == 0) {
        return NULL;
    }
    for (int i = 0; i < sdp->media_count; ++i) {
        const sdp_media_t *media = &sdp->media[i];
        if (media->port != 0 && !media_is_in_bundle(sdp, media)) {
            return NULL;
        }
    }
    return bundle_tag;
}

static sdp_media_t *add_track_media_section(turbo_peer_connection_t *pc, sdp_session_t *sdp,
                                            turbo_media_track_t *track, const char *mid,
                                            sdp_direction_t direction,
                                            sdp_setup_role_t setup_role,
                                            const sdp_codec_t *negotiated_codec) {
    sdp_codec_t codec;
    sdp_media_t *media;
    turbo_rtc_media_track_type_t track_type;
    turbo_codec_type_t codec_type;
    uint32_t ssrc;

    if (!pc || !sdp || !track) {
        return NULL;
    }

    track_type = turbo_media_track_get_type(track);
    codec_type = turbo_media_track_get_codec(track);
    if (negotiated_codec) {
        codec = *negotiated_codec;
    } else {
        if (fill_sdp_codec(codec_type, &codec) != 0) {
            return NULL;
        }
    }

    media = (track_type == TURBO_RTC_MEDIA_TRACK_AUDIO)
        ? sdp_add_audio(sdp, mid, direction)
        : sdp_add_video(sdp, mid, direction);
    if (!media) {
        return NULL;
    }

    add_common_media_security(pc, media, setup_role);
    add_local_candidates_to_media(pc, media);
    if (turbo_media_track_get_transport_cc_ext_id(track) > 0) {
        sdp_media_add_extension(media, turbo_media_track_get_transport_cc_ext_id(track),
                                TURBO_TRANSPORT_CC_URI);
    }
    sdp_media_add_codec(media, &codec);

    ssrc = turbo_media_track_get_ssrc(track);
    if (ssrc != 0) {
        sdp_media_add_ssrc(media, ssrc, "turbonet", track_type == TURBO_RTC_MEDIA_TRACK_AUDIO ? "audio" : "video");
    }

    return media;
}

static sdp_media_t *add_rejected_media_section(
    sdp_session_t *sdp,
    const sdp_media_t *remote_media,
    const char *mid
) {
    sdp_media_t *media;

    if (!sdp || !remote_media || !mid) return NULL;
    if (remote_media->type == SDP_MEDIA_AUDIO) {
        media = sdp_add_audio(sdp, mid, SDP_DIRECTION_INACTIVE);
    } else if (remote_media->type == SDP_MEDIA_VIDEO) {
        media = sdp_add_video(sdp, mid, SDP_DIRECTION_INACTIVE);
    } else if (remote_media->type == SDP_MEDIA_APPLICATION) {
        media = sdp_add_datachannel(
            sdp, mid, remote_media->sctp_port > 0
                          ? remote_media->sctp_port
                          : 5000);
    } else {
        return NULL;
    }
    if (!media) return NULL;

    media->port = 0;
    media->direction = SDP_DIRECTION_INACTIVE;
    media->setup = SDP_ROLE_PASSIVE;
    if (remote_media->protocol[0]) {
        strncpy(media->protocol, remote_media->protocol,
                sizeof(media->protocol) - 1);
    }
    for (int i = 0; i < remote_media->codec_count; ++i) {
        if (sdp_media_add_codec(media, &remote_media->codecs[i]) != 0) {
            return NULL;
        }
    }
    return media;
}


/* ============================================================================
 * Public API
 * ============================================================================ */

turbo_peer_connection_t *turbo_peer_connection_create(
    const turbo_peer_config_t *config,
    const turbo_peer_callbacks_t *callbacks
) {

    turbo_peer_connection_t *pc = calloc(1, sizeof(turbo_peer_connection_t));
    if (!pc) return NULL;
    
    if (config) pc->config = *config;

    if (callbacks) pc->callbacks = *callbacks;
    pc->state = TURBO_PEER_STATE_NEW;
    
    /* 1. Create ICE Agent */
    ice_config_t ice_cfg = ice_default_config();
    /* ice_cfg.loop is no longer needed in config */
    ice_cfg.is_controlling = 1; /* Default, will update based on role */
    ice_cfg.allow_loopback = pc->config.allow_loopback ? 1 : 0;


    
    if (configure_ice_servers(&pc->config, &ice_cfg) != 0) {
        free(pc);
        return NULL;
    }
    
    pc->ice_ctx = coro_context_create(NULL);
    if (!pc->ice_ctx) {
        free(pc);
        return NULL;
    }
#if defined(__linux__) && !defined(__ANDROID__)
    coro_context_set_udp_backend(pc->ice_ctx, TURBO_UDP_BACKEND_EPOLL);
#elif defined(_WIN32)
    coro_context_set_udp_backend(pc->ice_ctx, TURBO_UDP_BACKEND_IOCP);
#endif

    pc->ice_agent = ice_agent_create(pc->ice_ctx, &ice_cfg);
    if (!pc->ice_agent) {
        coro_context_destroy(pc->ice_ctx);
        free(pc);
        return NULL;
    }
    
    ice_callbacks_t ice_cb = {
        .on_state_change = on_ice_state_change,
        .on_candidate = on_ice_candidate,
        .on_data = on_ice_data,
        .user_data = pc
    };
    ice_agent_set_callbacks(pc->ice_agent, &ice_cb);
    
    if (refresh_local_ice_credentials(pc) != 0) {
        ice_agent_destroy(pc->ice_agent);
        coro_context_destroy(pc->ice_ctx);
        free(pc);
        return NULL;
    }
                                    
    /* 2. Create DataChannel Context */
    turbo_dc_config_t dc_cfg = {
        .transport = TURBO_DC_TRANSPORT_ICE, 
        .is_server = 0, /* Will be updated */
        .disable_sctp = pc->config.disable_datachannel ? 1 : 0
    };
    pc->dc_ctx = turbo_dc_context_create(&dc_cfg);
    if (!pc->dc_ctx) {
        ice_agent_destroy(pc->ice_agent);
        coro_context_destroy(pc->ice_ctx);
        tstr_free(pc->local_ufrag);
        tstr_free(pc->local_pwd);
        free(pc);
        return NULL;
    }
    
    /* 3. Create DataChannel Peer */
    pc->dc_peer = turbo_dc_peer_create(pc->dc_ctx, NULL, 0, pc);
    if (!pc->dc_peer) {
        turbo_dc_context_destroy(pc->dc_ctx);
        ice_agent_destroy(pc->ice_agent);
        coro_context_destroy(pc->ice_ctx);
        tstr_free(pc->local_ufrag);
        tstr_free(pc->local_pwd);
        free(pc);
        return NULL;
    }
    
    turbo_dc_peer_on_state(pc->dc_peer, on_dc_state);
    turbo_dc_peer_on_channel(pc->dc_peer, on_dc_channel);
    
    /* 4. Create Media Context */
    pc->media_ctx = turbo_media_create(pc->dc_peer, pc);
    if (!pc->media_ctx) {
        turbo_dc_peer_destroy(pc->dc_peer);
        turbo_dc_context_destroy(pc->dc_ctx);
        ice_agent_destroy(pc->ice_agent);
        coro_context_destroy(pc->ice_ctx);
        tstr_free(pc->local_ufrag);
        tstr_free(pc->local_pwd);
        free(pc);
        return NULL;
    }
    
    /* Start gathering candidates immediately */
    pc->gathering_start_pending = 1;
    if (coro_context_spawn(pc->ice_ctx, start_gathering_task, pc) != 0) {
        pc->gathering_start_pending = 0;
        turbo_media_destroy(pc->media_ctx);
        turbo_dc_peer_destroy(pc->dc_peer);
        turbo_dc_context_destroy(pc->dc_ctx);
        ice_agent_destroy(pc->ice_agent);
        coro_context_destroy(pc->ice_ctx);
        tstr_free(pc->local_ufrag);
        tstr_free(pc->local_pwd);
        free(pc);
        return NULL;
    }
    pump_ice_context(pc);
    
    return pc;
}

void turbo_peer_connection_destroy(turbo_peer_connection_t *pc) {
    if (!pc) return;

    pc->destroying = 1;
    pc->state = TURBO_PEER_STATE_CLOSED;

    if (pc->media_ctx) {
        turbo_media_destroy(pc->media_ctx);
        pc->media_ctx = NULL;
    }

    if (pc->dc_peer) {
        turbo_dc_peer_on_state(pc->dc_peer, NULL);
        turbo_dc_peer_on_channel(pc->dc_peer, NULL);
        turbo_dc_peer_on_error(pc->dc_peer, NULL);
        turbo_dc_peer_set_external_transport(pc->dc_peer, NULL, NULL);
        turbo_dc_peer_destroy(pc->dc_peer);
        pc->dc_peer = NULL;
    }
    
    if (pc->ice_agent) {
        ice_callbacks_t callbacks = {0};
        ice_agent_set_callbacks(pc->ice_agent, &callbacks);
        ice_agent_close(pc->ice_agent);
        if (pc->ice_ctx) {
            coro_context_stop(pc->ice_ctx);
            drain_ice_coroutines(pc);
        }
        ice_agent_destroy(pc->ice_agent);
        pc->ice_agent = NULL;
    }
    if (pc->dc_ctx) {
        turbo_dc_context_destroy(pc->dc_ctx);
    }
    if (pc->ice_ctx) {
        drain_ice_coroutines(pc);
        coro_context_destroy(pc->ice_ctx);
        pc->ice_ctx = NULL;
    }
    
    tstr_free(pc->local_ufrag);
    tstr_free(pc->local_pwd);
    tstr_free(pc->remote_fingerprint);
    tstr_free(pc->remote_ufrag);
    tstr_free(pc->remote_pwd);
    
    free(pc);
}

turbo_media_track_t *turbo_peer_connection_add_track(
    turbo_peer_connection_t *pc,
    turbo_rtc_media_track_type_t type,
    turbo_media_direction_t direction
) {
    turbo_media_track_config_t config = {
        .type = type,
        .direction = direction,
        /* Defaults */
        .codec = (type == TURBO_RTC_MEDIA_TRACK_AUDIO) ? TURBO_CODEC_OPUS : TURBO_CODEC_VP8
    };
    
    /* Set sensible defaults */
    if (type == TURBO_RTC_MEDIA_TRACK_AUDIO) {
        config.audio.sample_rate = 48000;
        config.audio.channels = 2;
        config.audio.bitrate = 64000;
        config.audio.frame_size_ms = 20;
    } else {
        config.video.width = 1280;
        config.video.height = 720;
        config.video.framerate = 30;
        config.video.bitrate = 1500000;
    }

    return turbo_peer_connection_add_track_ex(pc, &config);
}

turbo_media_track_t *turbo_peer_connection_add_track_ex(
    turbo_peer_connection_t *pc,
    const turbo_media_track_config_t *config
) {
    if (!pc || !pc->media_ctx || !config) return NULL;
    return turbo_media_add_track(pc->media_ctx, config);
}

turbo_dc_channel_t *turbo_peer_connection_create_datachannel(
    turbo_peer_connection_t *pc,
    const char *label,
    const turbo_dc_channel_config_t *config
) {
    if (!pc || !pc->dc_peer) return NULL;
    return turbo_dc_channel_create(pc->dc_peer, label, config);
}

int turbo_peer_connection_create_offer(
    turbo_peer_connection_t *pc,
    char *sdp_out,
    size_t max_len
) {
    sdp_session_t sdp;
    char mid[16];
    int media_mid = 0;

    if (!pc || !sdp_out || max_len == 0) return -1;
    if (configure_negotiated_transport_roles(pc, 1, 0, 0) != 0) return -1;
    pump_ice_context(pc);
    maybe_start_checks(pc);

    sdp_session_init(&sdp);

    if (pc->media_ctx) {
        int track_count = turbo_media_get_track_count(pc->media_ctx);

        for (int i = 0; i < track_count; i++) {
            turbo_media_track_t *track = turbo_media_get_track(pc->media_ctx, i);
            if (!track) {
                continue;
            }

            snprintf(mid, sizeof(mid), "%d", media_mid++);
            if (turbo_media_track_get_transport_cc_ext_id(track) == 0) {
                turbo_media_track_set_transport_cc_ext_id(track, RTP_EXT_TRANSPORT_CC);
            }
            add_track_media_section(pc, &sdp, track, mid,
                                    media_direction_to_sdp(turbo_media_track_get_direction(track)),
                                    SDP_ROLE_ACTPASS, NULL);
        }
    }

    if (!pc->config.disable_datachannel) {
        sdp_media_t *m;

        snprintf(mid, sizeof(mid), "%d", media_mid++);
        m = sdp_add_datachannel(&sdp, mid, 5000);
        if (m) {
            add_common_media_security(pc, m, SDP_ROLE_ACTPASS);
            add_local_candidates_to_media(pc, m);
            m->max_message_size = 262144;
        }
    }

    return sdp_generate_offer(&sdp, sdp_out, max_len);
}

int turbo_peer_connection_restart_ice(turbo_peer_connection_t *pc) {
    return restart_ice_generation(pc, 1);
}

int turbo_peer_connection_create_answer(
    turbo_peer_connection_t *pc,
    char *sdp_out,
    size_t max_len
) {
    sdp_session_t sdp;
    turbo_media_track_t *claimed_tracks[TURBO_MEDIA_MAX_TRACKS] = {0};
    int claimed_track_count = 0;

    if (!pc || !sdp_out || max_len == 0 || !pc->have_remote_sdp) return -1;
    pump_ice_context(pc);
    maybe_start_checks(pc);

    sdp_session_init(&sdp);

    for (int i = 0; i < pc->remote_sdp.media_count; i++) {
        sdp_media_t *remote_media = &pc->remote_sdp.media[i];
        char mid[16];

        snprintf(mid, sizeof(mid), "%s", remote_media->mid[0] ? remote_media->mid : "0");

        if (remote_media->port == 0) {
            if (!add_rejected_media_section(&sdp, remote_media, mid)) {
                return -1;
            }
            continue;
        }

        if (remote_media->type == SDP_MEDIA_APPLICATION) {
            if (pc->config.disable_datachannel) {
                if (!add_rejected_media_section(&sdp, remote_media, mid)) {
                    return -1;
                }
                continue;
            }
            sdp_media_t *m = sdp_add_datachannel(&sdp, mid,
                                                 remote_media->sctp_port > 0 ? remote_media->sctp_port : 5000);
            if (!m) {
                return -1;
            }
            add_common_media_security(pc, m, SDP_ROLE_PASSIVE);
            add_local_candidates_to_media(pc, m);
            m->max_message_size = remote_media->max_message_size > 0 ? remote_media->max_message_size : 262144;
            continue;
        }

        if (pc->media_ctx) {
            const sdp_codec_t *negotiated_codec = NULL;
            turbo_media_track_t *track = find_track_for_remote_media(
                pc->media_ctx, remote_media, claimed_tracks,
                claimed_track_count, &negotiated_codec);
            if (track) {
                if (claimed_track_count >= TURBO_MEDIA_MAX_TRACKS) {
                    return -1;
                }
                claimed_tracks[claimed_track_count++] = track;
                turbo_media_track_set_transport_cc_ext_id(track,
                    sdp_media_transport_cc_ext_id(remote_media));
                sdp_media_t *local_media = add_track_media_section(
                    pc, &sdp, track, mid,
                    negotiate_answer_direction(turbo_media_track_get_direction(track), remote_media->direction),
                    SDP_ROLE_PASSIVE, negotiated_codec);
                if (!local_media) {
                    return -1;
                }
                continue;
            }
        }
        if (!add_rejected_media_section(&sdp, remote_media, mid)) {
            return -1;
        }
    }

    return sdp_generate_answer(&sdp, &pc->remote_sdp, sdp_out, max_len);
}

int turbo_peer_connection_set_remote_description(
    turbo_peer_connection_t *pc,
    const char *type,
    const char *sdp_str
) {
    const sdp_media_t *transport_media = NULL;
    tstr new_ufrag = NULL;
    tstr new_pwd = NULL;
    tstr new_fingerprint = NULL;
    int ice_controlling;
    int dtls_server = 0;
    int remote_credentials_changed = 0;
    int remote_restart = 0;
    int completing_local_restart = 0;
    turbo_media_track_t *claimed_tracks[TURBO_MEDIA_MAX_TRACKS] = {0};
    int claimed_track_count = 0;

    if (!pc || !type || !sdp_str) return -1;
    if (strcmp(type, "offer") == 0) {
        ice_controlling = 0;
    } else if (strcmp(type, "answer") == 0) {
        ice_controlling = 1;
    } else {
        return -1;
    }
    
    sdp_session_t remote_sdp;
    if (sdp_parse(sdp_str, strlen(sdp_str), &remote_sdp) < 0) return -1;

    transport_media = select_remote_transport_media(&remote_sdp);
    if (!transport_media) {
        return -1;
    }

    for (int i = 0; i < remote_sdp.media_count; ++i) {
        const sdp_media_t *media = &remote_sdp.media[i];

        if (media->port == 0) {
            continue;
        }

        /*
         * Initial BUNDLE offers carry unique transport attributes on every
         * active m-line. The first BUNDLE mid selects the attributes that
         * become authoritative for the negotiated shared transport.
         */
        if (!ice_controlling &&
            (!media->ice_ufrag[0] || !media->ice_pwd[0] ||
             !media->fingerprint_hash[0] || !media->fingerprint[0] ||
             media->setup != SDP_ROLE_ACTPASS)) {
            return -1;
        }
    }
    if (!transport_media->ice_ufrag[0] || !transport_media->ice_pwd[0] ||
        !transport_media->fingerprint_hash[0] ||
        !transport_media->fingerprint[0] ||
        (ice_controlling && transport_media->setup == SDP_ROLE_ACTPASS)) {
        return -1;
    }
    dtls_server = !ice_controlling ||
                  transport_media->setup == SDP_ROLE_ACTIVE;
    completing_local_restart = pc->local_ice_restart_pending;
    if (pc->remote_ufrag && pc->remote_pwd) {
        remote_credentials_changed =
            strcmp(pc->remote_ufrag, transport_media->ice_ufrag) != 0 ||
            strcmp(pc->remote_pwd, transport_media->ice_pwd) != 0;
        if (completing_local_restart && !remote_credentials_changed) {
            return -1;
        }
    }
    if (pc->dtls_connected && pc->remote_fingerprint &&
        !peer_ascii_equal_ignore_case(
            pc->remote_fingerprint, transport_media->fingerprint)) {
        return -1;
    }
    remote_restart = remote_credentials_changed && !completing_local_restart;

    new_ufrag = tstr_dup(transport_media->ice_ufrag);
    new_pwd = tstr_dup(transport_media->ice_pwd);
    new_fingerprint = tstr_dup(transport_media->fingerprint);
    if (!new_ufrag || !new_pwd || !new_fingerprint) {
        tstr_free(new_ufrag);
        tstr_free(new_pwd);
        tstr_free(new_fingerprint);
        return -1;
    }
    if (configure_negotiated_transport_roles(
            pc, ice_controlling, dtls_server, 1) != 0) {
        tstr_free(new_ufrag);
        tstr_free(new_pwd);
        tstr_free(new_fingerprint);
        return -1;
    }
    if ((!pc->remote_fingerprint ||
         !peer_ascii_equal_ignore_case(
             pc->remote_fingerprint, transport_media->fingerprint)) &&
        turbo_dc_peer_set_remote_fingerprint(
            pc->dc_peer, transport_media->fingerprint_hash,
            transport_media->fingerprint) != 0) {
        tstr_free(new_ufrag);
        tstr_free(new_pwd);
        tstr_free(new_fingerprint);
        return -1;
    }
    if (remote_restart && restart_ice_generation(pc, 0) != 0) {
        tstr_free(new_ufrag);
        tstr_free(new_pwd);
        tstr_free(new_fingerprint);
        return -1;
    }
    if (ice_agent_set_remote_credentials(
            pc->ice_agent, transport_media->ice_ufrag,
            transport_media->ice_pwd) != 0) {
        tstr_free(new_ufrag);
        tstr_free(new_pwd);
        tstr_free(new_fingerprint);
        return -1;
    }

    tstr_free(pc->remote_ufrag);
    tstr_free(pc->remote_pwd);
    tstr_free(pc->remote_fingerprint);
    pc->remote_ufrag = new_ufrag;
    pc->remote_pwd = new_pwd;
    pc->remote_fingerprint = new_fingerprint;
    pc->remote_sdp = remote_sdp;
    pc->have_remote_sdp = 1;
    if (remote_restart || completing_local_restart) {
        pc->remote_candidate_count = 0;
    }
    pc->local_ice_restart_pending = 0;

    /* Negotiate media tracks logic */
    for (int i = 0; i < remote_sdp.media_count; i++) {
         sdp_media_t *rm = &remote_sdp.media[i];
         if (rm->port == 0 || rm->type == SDP_MEDIA_APPLICATION) {
             continue;
         }

         turbo_rtc_media_track_type_t track_type = (rm->type == SDP_MEDIA_AUDIO) ? TURBO_RTC_MEDIA_TRACK_AUDIO : TURBO_RTC_MEDIA_TRACK_VIDEO;
         const sdp_codec_t *preferred =
             find_first_supported_codec(rm, track_type);
         int transport_cc_ext_id = sdp_media_transport_cc_ext_id(rm);
         turbo_media_track_t *local_track = NULL;
         const sdp_codec_t *negotiated_codec = NULL;

         if (pc->media_ctx) {
             local_track = find_track_for_remote_media(
                 pc->media_ctx, rm, claimed_tracks, claimed_track_count,
                 &negotiated_codec);
             if (local_track) {
                 turbo_media_track_set_payload_type(
                     local_track,
                     (uint8_t)negotiated_codec->payload_type);
                 turbo_media_track_set_transport_cc_ext_id(local_track, transport_cc_ext_id);
                 if (claimed_track_count >= TURBO_MEDIA_MAX_TRACKS) {
                     return -1;
                 }
                 claimed_tracks[claimed_track_count++] = local_track;
                 if (sdp_direction_can_send(rm->direction) &&
                     notify_remote_track_once(pc, local_track) != 0) {
                     return -1;
                 }
             }
         }

         turbo_media_track_config_t config;

         if (pc->media_ctx && !local_track && preferred &&
             sdp_direction_can_send(rm->direction) &&
             fill_remote_track_config(rm, preferred, &config) == 0) {
             turbo_media_track_t *track = turbo_media_add_track(pc->media_ctx, &config);
              if (track) {
                  if (claimed_track_count >= TURBO_MEDIA_MAX_TRACKS) {
                      return -1;
                  }
                  claimed_tracks[claimed_track_count++] = track;
                  if (preferred) {
                      turbo_media_track_set_payload_type(track, (uint8_t)preferred->payload_type);
                 }
                 turbo_media_track_set_transport_cc_ext_id(track, transport_cc_ext_id);
                 if (rm->ssrc_count > 0) {
                     turbo_media_track_set_remote_ssrc(track, rm->ssrcs[0].ssrc);
                 }
                 if (notify_remote_track_once(pc, track) != 0) {
                     return -1;
                 }
             }
         }
    }

    import_remote_candidates(pc, &remote_sdp);
    if (maybe_start_checks(pc) != 0) {
        return -1;
    }
    
    return 0;
}

int turbo_peer_connection_add_ice_candidate(
    turbo_peer_connection_t *pc,
    const char *candidate
) {
    if (!pc || !pc->ice_agent) return -1;
    pump_ice_context(pc);
    if (ice_agent_add_remote_candidate(pc->ice_agent, candidate) != 0) {
        return -1;
    }
    pc->remote_candidate_count++;
    return maybe_start_checks(pc);
}

typedef struct turbo_ice_sdpfrag_s {
    char ufrag[32];
    char pwd[64];
    char candidates[SDP_MAX_CANDIDATES][TURBO_ICE_SDPFRAG_CANDIDATE_CAPACITY];
    int candidate_count;
    int have_ufrag;
    int have_pwd;
} turbo_ice_sdpfrag_t;

static int copy_sdpfrag_attribute(char *destination, size_t capacity,
                                  const char *value, size_t value_len) {
    if (!destination || capacity == 0 || !value || value_len == 0 ||
        value_len >= capacity) {
        return -1;
    }
    memcpy(destination, value, value_len);
    destination[value_len] = '\0';
    return 0;
}

static int parse_ice_sdpfrag(const char *sdpfrag, size_t sdpfrag_len,
                             turbo_ice_sdpfrag_t *parsed) {
    size_t offset = 0;

    if (!sdpfrag || !parsed || sdpfrag_len == 0 ||
        sdpfrag_len > TURBO_ICE_SDPFRAG_MAX_LENGTH) {
        return -1;
    }
    memset(parsed, 0, sizeof(*parsed));

    while (offset < sdpfrag_len) {
        size_t line_start = offset;
        size_t line_len;
        const char *value;
        size_t value_len;

        while (offset < sdpfrag_len &&
               sdpfrag[offset] != '\r' && sdpfrag[offset] != '\n') {
            ++offset;
        }
        line_len = offset - line_start;
        while (offset < sdpfrag_len &&
               (sdpfrag[offset] == '\r' || sdpfrag[offset] == '\n')) {
            ++offset;
        }
        if (line_len == 0) {
            continue;
        }

        if (line_len > strlen("a=ice-ufrag:") &&
            memcmp(sdpfrag + line_start, "a=ice-ufrag:",
                   strlen("a=ice-ufrag:")) == 0) {
            value = sdpfrag + line_start + strlen("a=ice-ufrag:");
            value_len = line_len - strlen("a=ice-ufrag:");
            if ((parsed->have_ufrag &&
                 (strlen(parsed->ufrag) != value_len ||
                  memcmp(parsed->ufrag, value, value_len) != 0)) ||
                copy_sdpfrag_attribute(parsed->ufrag, sizeof(parsed->ufrag),
                                       value, value_len) != 0) {
                return -1;
            }
            parsed->have_ufrag = 1;
            continue;
        }
        if (line_len > strlen("a=ice-pwd:") &&
            memcmp(sdpfrag + line_start, "a=ice-pwd:",
                   strlen("a=ice-pwd:")) == 0) {
            value = sdpfrag + line_start + strlen("a=ice-pwd:");
            value_len = line_len - strlen("a=ice-pwd:");
            if ((parsed->have_pwd &&
                 (strlen(parsed->pwd) != value_len ||
                  memcmp(parsed->pwd, value, value_len) != 0)) ||
                copy_sdpfrag_attribute(parsed->pwd, sizeof(parsed->pwd),
                                       value, value_len) != 0) {
                return -1;
            }
            parsed->have_pwd = 1;
            continue;
        }
        if (line_len > strlen("a=candidate:") &&
            memcmp(sdpfrag + line_start, "a=candidate:",
                   strlen("a=candidate:")) == 0) {
            if (parsed->candidate_count >= SDP_MAX_CANDIDATES ||
                line_len - 2 >= TURBO_ICE_SDPFRAG_CANDIDATE_CAPACITY) {
                return -1;
            }
            memcpy(parsed->candidates[parsed->candidate_count],
                   sdpfrag + line_start + 2, line_len - 2);
            parsed->candidates[parsed->candidate_count][line_len - 2] = '\0';
            parsed->candidate_count++;
            continue;
        }
        if ((line_len == strlen("a=end-of-candidates") &&
             memcmp(sdpfrag + line_start, "a=end-of-candidates", line_len) == 0) ||
            (line_len >= 2 &&
             memcmp(sdpfrag + line_start, "m=", 2) == 0) ||
            (line_len >= 6 &&
             memcmp(sdpfrag + line_start, "a=mid:", 6) == 0)) {
            continue;
        }
        return -1;
    }

    if (parsed->have_ufrag != parsed->have_pwd ||
        (!parsed->have_ufrag && parsed->candidate_count == 0)) {
        return -1;
    }
    return 0;
}

int turbo_peer_connection_apply_remote_ice_sdpfrag(
    turbo_peer_connection_t *pc,
    const char *sdpfrag,
    size_t sdpfrag_len
) {
    turbo_ice_sdpfrag_t parsed;
    tstr next_ufrag = NULL;
    tstr next_pwd = NULL;
    int completing_local_restart;
    int restarted = 0;

    if (!pc || !pc->ice_agent || !pc->have_remote_sdp ||
        parse_ice_sdpfrag(sdpfrag, sdpfrag_len, &parsed) != 0) {
        return -1;
    }
    completing_local_restart = pc->local_ice_restart_pending;

    if (parsed.have_ufrag &&
        (!pc->remote_ufrag || !pc->remote_pwd ||
         strcmp(pc->remote_ufrag, parsed.ufrag) != 0 ||
         strcmp(pc->remote_pwd, parsed.pwd) != 0)) {
        next_ufrag = tstr_dup(parsed.ufrag);
        next_pwd = tstr_dup(parsed.pwd);
        if (!next_ufrag || !next_pwd) {
            tstr_free(next_ufrag);
            tstr_free(next_pwd);
            return -1;
        }
        if (!completing_local_restart) {
            int restart_result = restart_ice_generation(pc, 0);
            if (restart_result != 0) {
                tstr_free(next_ufrag);
                tstr_free(next_pwd);
                return -1;
            }
        }
        {
            int credential_result = ice_agent_set_remote_credentials(
                pc->ice_agent, parsed.ufrag, parsed.pwd);
            if (credential_result != 0) {
                tstr_free(next_ufrag);
                tstr_free(next_pwd);
                return -1;
            }
        }

        tstr_free(pc->remote_ufrag);
        tstr_free(pc->remote_pwd);
        pc->remote_ufrag = next_ufrag;
        pc->remote_pwd = next_pwd;
        pc->remote_candidate_count = 0;
        for (int i = 0; i < pc->remote_sdp.media_count; ++i) {
            if (pc->remote_sdp.media[i].port != 0) {
                sdp_media_set_ice(
                    &pc->remote_sdp.media[i], parsed.ufrag, parsed.pwd);
            }
        }
        pc->local_ice_restart_pending = 0;
        restarted = 1;
    } else if (parsed.have_ufrag && completing_local_restart) {
        return -1;
    }

    for (int i = 0; i < parsed.candidate_count; ++i) {
        if (turbo_peer_connection_add_ice_candidate(
                pc, parsed.candidates[i]) != 0) {
            return -1;
        }
    }
    return restarted;
}

static int append_sdpfrag(char *output, size_t capacity, size_t *length,
                          const char *format, ...) {
    va_list args;
    int written;

    if (!output || !length || *length >= capacity) {
        return -1;
    }
    va_start(args, format);
    written = vsnprintf(output + *length, capacity - *length, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= capacity - *length) {
        return -1;
    }
    *length += (size_t)written;
    return 0;
}

int turbo_peer_connection_create_local_ice_sdpfrag(
    turbo_peer_connection_t *pc,
    char *sdpfrag_out,
    size_t max_len
) {
    const sdp_media_t *media;
    const char *media_name;
    size_t length = 0;
    int candidate_count;

    if (!pc || !pc->ice_agent || !pc->have_remote_sdp ||
        !pc->local_ufrag || !pc->local_pwd || !sdpfrag_out || max_len == 0) {
        return -1;
    }
    media = select_remote_transport_media(&pc->remote_sdp);
    if (!media || media->port == 0) {
        return -1;
    }
    media_name = media->type == SDP_MEDIA_AUDIO ? "audio" :
                 media->type == SDP_MEDIA_VIDEO ? "video" :
                 media->type == SDP_MEDIA_APPLICATION ? "application" : NULL;
    if (!media_name ||
        append_sdpfrag(sdpfrag_out, max_len, &length,
                       "a=ice-ufrag:%s\r\na=ice-pwd:%s\r\n",
                       pc->local_ufrag, pc->local_pwd) != 0 ||
        append_sdpfrag(sdpfrag_out, max_len, &length, "m=%s 9 %s",
                       media_name, media->protocol) != 0) {
        return -1;
    }
    if (media->type == SDP_MEDIA_APPLICATION) {
        if (append_sdpfrag(sdpfrag_out, max_len, &length,
                           " webrtc-datachannel\r\n") != 0) {
            return -1;
        }
    } else {
        if (media->codec_count == 0) {
            return -1;
        }
        for (int i = 0; i < media->codec_count; ++i) {
            if (append_sdpfrag(sdpfrag_out, max_len, &length, " %d",
                               media->codecs[i].payload_type) != 0) {
                return -1;
            }
        }
        if (append_sdpfrag(sdpfrag_out, max_len, &length, "\r\n") != 0) {
            return -1;
        }
    }
    if (append_sdpfrag(sdpfrag_out, max_len, &length, "a=mid:%s\r\n",
                       media->mid[0] ? media->mid : "0") != 0) {
        return -1;
    }

    pump_ice_context(pc);
    candidate_count = ice_agent_get_local_candidate_count(pc->ice_agent);
    for (int i = 0; i < candidate_count; ++i) {
        ice_candidate_t candidate;
        char candidate_line[TURBO_ICE_SDPFRAG_CANDIDATE_CAPACITY];

        if (ice_agent_get_local_candidate(pc->ice_agent, i, &candidate) != 0 ||
            ice_candidate_to_sdp(
                &candidate, candidate_line, sizeof(candidate_line)) < 0 ||
            append_sdpfrag(sdpfrag_out, max_len, &length, "a=%s\r\n",
                           candidate_line) != 0) {
            return -1;
        }
    }
    if (append_sdpfrag(sdpfrag_out, max_len, &length,
                       "a=end-of-candidates\r\n") != 0) {
        return -1;
    }
    return (int)length;
}

void turbo_peer_connection_poll(turbo_peer_connection_t *pc) {
    if (!pc) {
        return;
    }

    pump_ice_context(pc);
    maybe_start_dtls(pc);
    if (pc->dc_peer) {
        turbo_dc_peer_poll(pc->dc_peer);
    }
    if (pc->media_ctx) {
        turbo_media_handle_timers(pc->media_ctx);
    }
    turbo_dc_handle_timers();
    maybe_start_checks(pc);
}

turbo_media_context_t *turbo_peer_connection_get_media_context(turbo_peer_connection_t *pc) {
    return pc ? pc->media_ctx : NULL;
}

turbo_dc_peer_t *turbo_peer_connection_get_dc_peer(turbo_peer_connection_t *pc) {
    return pc ? pc->dc_peer : NULL;
}
