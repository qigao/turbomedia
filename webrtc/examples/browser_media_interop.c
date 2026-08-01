/**
 * browser_media_interop.c - Browser audio/video interoperability test
 *
 * Native audio/video sender/receiver that answers a browser media offer.
 *
 * Usage:
 *   ./browser_media_interop [--no-stun]
 */

#include "turbo_datachannel.h"
#include "ice_integration.h"
#include "turbo_sdp.h"
#include "turbo_media_engine.h"
#include <CoroNet/turbo_coro_context.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <strings.h>
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#endif

#define MAX_LOCAL_CANDIDATES 32
#define FRAME_WIDTH 320
#define FRAME_HEIGHT 240
#define FRAME_RATE 30
#define AUDIO_SAMPLE_RATE 48000
#define AUDIO_CHANNELS 2
#define AUDIO_FRAME_MS 20
#define AUDIO_SAMPLES_PER_CH ((AUDIO_SAMPLE_RATE * AUDIO_FRAME_MS) / 1000)
#define H264_PROFILE_LEVEL_ID "42e01f"
#define TRANSPORT_CC_URI "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01"

static int g_running = 1;
static int g_connected = 0;
static int g_srtp_ready_logged = 0;
static uint64_t g_video_frames_sent = 0;
static uint64_t g_video_frames_received = 0;
static uint64_t g_audio_frames_sent = 0;
static uint64_t g_audio_frames_received = 0;
static turbo_codec_type_t g_selected_video_codec = TURBO_CODEC_VP8;
static int g_receive_mode = 0;

static turbo_dc_context_t *g_dc_ctx = NULL;
static turbo_dc_peer_t *g_peer = NULL;
static turbo_loop_t *g_loop = NULL;
static ice_integration_ctx_t *g_ice = NULL;
static turbo_media_context_t *g_media = NULL;
static turbo_media_track_t *g_audio_track = NULL;
static turbo_media_track_t *g_video_track = NULL;

typedef struct {
    const sdp_media_t *audio_media;
    const sdp_media_t *video_media;
    const sdp_codec_t *audio_codec;
    const sdp_codec_t *video_codec;
    const sdp_media_t *transport_media;
} remote_offer_info_t;

static char g_local_candidates[MAX_LOCAL_CANDIDATES][512];
static int g_local_candidate_count = 0;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static void on_ice_candidate(const char *candidate_sdp, void *user_data) {
    (void)user_data;

    if (g_local_candidate_count < MAX_LOCAL_CANDIDATES) {
        snprintf(g_local_candidates[g_local_candidate_count],
                 sizeof(g_local_candidates[g_local_candidate_count]), "%s", candidate_sdp);
        g_local_candidate_count++;
    }

    printf("[ICE] Local candidate: %s\n", candidate_sdp);
}

static void on_ice_state(ice_state_t state, void *user_data) {
    static const char *state_names[] = {
        "NEW", "GATHERING", "CONNECTING", "CONNECTED",
        "COMPLETED", "FAILED", "DISCONNECTED", "CLOSED"
    };
    (void)user_data;

    if (state >= 0 && state < (int)(sizeof(state_names) / sizeof(state_names[0]))) {
        printf("[ICE] State: %s\n", state_names[state]);
    } else {
        printf("[ICE] State: %d\n", (int)state);
    }
}

static void on_peer_state(turbo_dc_peer_t *peer, turbo_dc_state_t old_state,
                          turbo_dc_state_t new_state, void *user_data) {
    static const char *state_names[] = {
        "NEW", "CONNECTING", "CONNECTED", "DISCONNECTING", "CLOSED", "FAILED"
    };
    (void)peer;
    (void)old_state;
    (void)user_data;

    printf("[DTLS] State: %s\n", state_names[new_state]);

    if (new_state == TURBO_DC_STATE_FAILED) {
        fprintf(stderr, "[DTLS] Connection failed\n");
        g_running = 0;
    }
}

static void try_start_media(void) {
    uint8_t keying_material[128];
    uint16_t srtp_profile;

    if (g_connected || !g_media || (!g_audio_track && !g_video_track)) {
        return;
    }

    srtp_profile = turbo_dc_peer_get_srtp_keys(g_peer, keying_material);
    if (srtp_profile == 0) {
        return;
    }

    if (!g_srtp_ready_logged) {
        g_srtp_ready_logged = 1;
        printf("[Media] SRTP profile negotiated: 0x%04x\n", srtp_profile);
    }

    if (turbo_media_setup_srtp(g_media) != 0) {
        fprintf(stderr, "[Media] Failed to create SRTP sessions\n");
        g_running = 0;
        return;
    }

    if (g_audio_track && turbo_media_track_start(g_audio_track) != 0) {
        fprintf(stderr, "[Media] Failed to start audio track after DTLS keying\n");
        g_running = 0;
        return;
    }

    if (g_video_track && turbo_media_track_start(g_video_track) != 0) {
        fprintf(stderr, "[Media] Failed to start video track after DTLS keying\n");
        g_running = 0;
        return;
    }

    g_connected = 1;
    if (g_audio_track) {
        printf("[Media] Audio %s active\n", g_receive_mode ? "receiver" : "sender");
    }
    if (g_video_track) {
        printf("[Media] Video %s active\n", g_receive_mode ? "receiver" : "sender");
    }
}

static void on_audio_frame(turbo_media_track_t *track, const uint8_t *data, size_t len,
                           uint64_t timestamp, void *user_data) {
    (void)track;
    (void)data;
    (void)user_data;

    g_audio_frames_received++;
    if (g_audio_frames_received == 1 || (g_audio_frames_received % 50) == 0) {
        printf("[Media] Received %llu audio frames last_len=%zu timestamp=%llu\n",
               (unsigned long long)g_audio_frames_received, len, (unsigned long long)timestamp);
    }
}

static void on_video_frame(turbo_media_track_t *track, const uint8_t *data, size_t len,
                           uint64_t timestamp, void *user_data) {
    (void)track;
    (void)data;
    (void)user_data;

    g_video_frames_received++;
    if (g_video_frames_received == 1 || (g_video_frames_received % FRAME_RATE) == 0) {
        printf("[Media] Received %llu video frames last_len=%zu timestamp=%llu\n",
               (unsigned long long)g_video_frames_received, len, (unsigned long long)timestamp);
    }
}

static int sdp_candidate_to_string(const sdp_candidate_t *candidate, char *buffer,
                                   size_t buffer_size) {
    int len = snprintf(buffer, buffer_size, "candidate:%s %d %s %u %s %u typ %s",
                       candidate->foundation, candidate->component, candidate->transport,
                       candidate->priority, candidate->address, candidate->port,
                       candidate->type);
    if (len < 0 || (size_t)len >= buffer_size) {
        return -1;
    }

    if (candidate->rel_addr[0] != '\0' && candidate->rel_port != 0) {
        int extra = snprintf(buffer + len, buffer_size - (size_t)len, " raddr %s rport %u",
                             candidate->rel_addr, candidate->rel_port);
        if (extra < 0 || (size_t)extra >= buffer_size - (size_t)len) {
            return -1;
        }
    }

    return 0;
}

static int add_candidate_to_sdp_media(sdp_media_t *media, const char *candidate_sdp) {
    ice_candidate_t ice_candidate;
    sdp_candidate_t sdp_candidate;

    if (!media || !candidate_sdp || ice_candidate_parse(candidate_sdp, &ice_candidate) != 0) {
        return -1;
    }

    memset(&sdp_candidate, 0, sizeof(sdp_candidate));
    strncpy(sdp_candidate.foundation, ice_candidate.foundation, sizeof(sdp_candidate.foundation) - 1);
    sdp_candidate.component = ice_candidate.component_id;
    strncpy(sdp_candidate.transport,
            ice_candidate.transport == ICE_TRANSPORT_TCP ? "tcp" : "udp",
            sizeof(sdp_candidate.transport) - 1);
    sdp_candidate.priority = ice_candidate.priority;
    strncpy(sdp_candidate.address, ice_candidate.ip, sizeof(sdp_candidate.address) - 1);
    sdp_candidate.port = ice_candidate.port;
    strncpy(sdp_candidate.type, ice_candidate_type_name(ice_candidate.type),
            sizeof(sdp_candidate.type) - 1);
    strncpy(sdp_candidate.rel_addr, ice_candidate.related_ip, sizeof(sdp_candidate.rel_addr) - 1);
    sdp_candidate.rel_port = ice_candidate.related_port;

    return sdp_media_add_candidate(media, &sdp_candidate);
}

static void print_sdp_for_copy_paste(const char *sdp) {
    const char *line = sdp;
    int emitted_candidate = 0;

    if (!sdp) {
        return;
    }

    while (*line) {
        const char *next = strchr(line, '\n');
        size_t len = next ? (size_t)(next - line) : strlen(line);

        while (len > 0 && line[len - 1] == '\r') {
            --len;
        }

        fwrite(line, 1, len, stdout);
        fputc('\n', stdout);

        if (len > 12 && strncmp(line, "a=candidate:", 12) == 0) {
            emitted_candidate = 1;
        }

        if (len > 15 && strncmp(line, "a=group:BUNDLE", 14) == 0) {
            fputs("a=extmap-allow-mixed\n", stdout);
            fputs("a=msid-semantic: WMS turbonet\n", stdout);
        } else if (len > 10 && strncmp(line, "a=ice-pwd:", 10) == 0) {
            fputs("a=ice-options:trickle\n", stdout);
        }

        if (!next) {
            break;
        }
        line = next + 1;
    }

    if (emitted_candidate) {
        fputs("a=end-of-candidates\n", stdout);
    }
}

static int fmtp_contains_token(const char *fmtp, const char *token) {
    size_t token_len;

    if (!fmtp || !token) {
        return 0;
    }

    token_len = strlen(token);
    while (*fmtp) {
        if (_strnicmp(fmtp, token, token_len) == 0) {
            return 1;
        }
        ++fmtp;
    }

    return 0;
}

static turbo_codec_type_t sdp_codec_to_turbo(const sdp_codec_t *codec) {
    if (!codec) {
        return TURBO_CODEC_VP8;
    }

    if (_stricmp(codec->name, "VP8") == 0) {
        return TURBO_CODEC_VP8;
    }
    if (_stricmp(codec->name, "VP9") == 0) {
        return TURBO_CODEC_VP9;
    }
    if (_stricmp(codec->name, "H264") == 0) {
        return TURBO_CODEC_H264;
    }

    return TURBO_CODEC_VP8;
}

static turbo_codec_type_t sdp_audio_codec_to_turbo(const sdp_codec_t *codec) {
    if (!codec) {
        return TURBO_CODEC_OPUS;
    }
    if (_stricmp(codec->name, "opus") == 0) {
        return TURBO_CODEC_OPUS;
    }
    return TURBO_CODEC_OPUS;
}

static const char *turbo_codec_label(turbo_codec_type_t codec) {
    switch (codec) {
        case TURBO_CODEC_OPUS:
            return "OPUS";
        case TURBO_CODEC_VP8:
            return "VP8";
        case TURBO_CODEC_VP9:
            return "VP9";
        case TURBO_CODEC_H264:
            return "H264";
        default:
            return "video";
    }
}

static const sdp_codec_t *choose_remote_video_codec(const sdp_media_t *media) {
    int i;
    const sdp_codec_t *vp8 = NULL;
    const sdp_codec_t *vp9 = NULL;
    const sdp_codec_t *mode1_fallback = NULL;
    const sdp_codec_t *preferred_h264 = NULL;
    const sdp_codec_t *fallback_h264 = NULL;
    const sdp_codec_t *any_h264 = NULL;

    if (!media) {
        return NULL;
    }

    for (i = 0; i < media->codec_count; i++) {
        const sdp_codec_t *codec = &media->codecs[i];

        if (_stricmp(codec->name, "VP8") == 0) {
            if (!vp8) {
                vp8 = codec;
            }
            continue;
        }

        if (_stricmp(codec->name, "VP9") == 0) {
            if (!vp9) {
                vp9 = codec;
            }
            continue;
        }

        if (_stricmp(codec->name, "H264") != 0) {
            continue;
        }

        if (!any_h264) {
            any_h264 = codec;
        }

        if (!fmtp_contains_token(codec->fmtp, "packetization-mode=1")) {
            continue;
        }

        if (!mode1_fallback) {
            mode1_fallback = codec;
        }

        if (fmtp_contains_token(codec->fmtp, "profile-level-id=" H264_PROFILE_LEVEL_ID)) {
            preferred_h264 = codec;
        } else if (fmtp_contains_token(codec->fmtp, "profile-level-id=42c01f")) {
            fallback_h264 = codec;
        }
    }

    if (vp8) {
        return vp8;
    }
    if (preferred_h264) {
        return preferred_h264;
    }
    if (fallback_h264) {
        return fallback_h264;
    }
    if (mode1_fallback) {
        return mode1_fallback;
    }
    if (vp9) {
        return vp9;
    }
    return any_h264;
}

static const sdp_codec_t *choose_remote_audio_codec(const sdp_media_t *media) {
    if (!media) {
        return NULL;
    }

    for (int i = 0; i < media->codec_count; i++) {
        const sdp_codec_t *codec = &media->codecs[i];
        if (_stricmp(codec->name, "opus") == 0) {
            return codec;
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

static int is_preferred_host_candidate(const sdp_candidate_t *candidate) {
    if (!candidate) {
        return 0;
    }
    return _stricmp(candidate->type, "host") == 0;
}

static void configure_answer_media_common(sdp_media_t *media) {
    char ufrag[32];
    char pwd[64];
    char fp_hash[32];
    char fingerprint[128];

    if (!media) {
        return;
    }

    ice_integration_get_local_credentials(g_ice, ufrag, sizeof(ufrag), pwd, sizeof(pwd));
    sdp_media_set_ice(media, ufrag, pwd);

    turbo_dc_context_get_local_fingerprint(g_dc_ctx, fp_hash, sizeof(fp_hash),
                                           fingerprint, sizeof(fingerprint));
    for (char *p = fingerprint; *p; ++p) {
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            memmove(p, p + 1, strlen(p));
            --p;
        }
    }
    sdp_media_set_fingerprint(media, fp_hash, fingerprint);
    media->setup = SDP_ROLE_ACTIVE;

    for (int i = 0; i < g_local_candidate_count; i++) {
        add_candidate_to_sdp_media(media, g_local_candidates[i]);
    }
}

static int add_answer_audio_section(sdp_session_t *local_sdp, const sdp_media_t *remote_audio,
                                    const sdp_codec_t *remote_codec) {
    sdp_media_t *media;
    sdp_codec_t codec;

    if (!local_sdp || !remote_audio || !remote_codec || !g_audio_track) {
        return 0;
    }

    media = sdp_add_audio(local_sdp, remote_audio->mid[0] ? remote_audio->mid : "0",
                          g_receive_mode ? SDP_DIRECTION_RECVONLY : SDP_DIRECTION_SENDONLY);
    if (!media) {
        return -1;
    }

    codec = *remote_codec;
    codec.channels = codec.channels > 0 ? codec.channels : AUDIO_CHANNELS;
    codec.rtx_payload_type = 0;
    codec.supports_nack = 1;
    codec.supports_pli = 0;
    codec.supports_fir = 0;
    codec.supports_remb = 0;
    codec.supports_transport_cc = sdp_media_transport_cc_ext_id(remote_audio) > 0 ? 1 : 0;
    if (codec.supports_transport_cc) {
        int ext_id = sdp_media_transport_cc_ext_id(remote_audio);
        sdp_media_add_extension(media, ext_id, TRANSPORT_CC_URI);
        turbo_media_track_set_transport_cc_ext_id(g_audio_track, ext_id);
    } else {
        turbo_media_track_set_transport_cc_ext_id(g_audio_track, 0);
    }
    sdp_media_add_codec(media, &codec);
    configure_answer_media_common(media);

    if (!g_receive_mode && turbo_media_track_get_ssrc(g_audio_track) != 0) {
        sdp_media_add_ssrc(media, turbo_media_track_get_ssrc(g_audio_track), "turbonet", "turbonet-stream");
        if (media->ssrc_count > 0) {
            strncpy(media->ssrcs[media->ssrc_count - 1].track_id, "audio-track",
                    sizeof(media->ssrcs[media->ssrc_count - 1].track_id) - 1);
        }
    }

    return 0;
}

static int print_local_answer(const sdp_session_t *remote_sdp, const remote_offer_info_t *remote_offer) {
    sdp_session_t local_sdp;
    sdp_media_t *media;
    sdp_codec_t codec;
    char sdp_buf[8192];
    int len;

    if (!remote_sdp || !remote_offer || !remote_offer->transport_media ||
        (!g_audio_track && !g_video_track)) {
        return -1;
    }

    sdp_session_init(&local_sdp);
    if (remote_offer->audio_media && remote_offer->audio_codec && g_audio_track) {
        if (add_answer_audio_section(&local_sdp, remote_offer->audio_media, remote_offer->audio_codec) != 0) {
            return -1;
        }
    }

    if (remote_offer->video_media && remote_offer->video_codec && g_video_track) {
        media = sdp_add_video(&local_sdp,
                              remote_offer->video_media->mid[0] ? remote_offer->video_media->mid : "1",
                              g_receive_mode ? SDP_DIRECTION_RECVONLY : SDP_DIRECTION_SENDONLY);
        if (!media) {
            return -1;
        }

        codec = *remote_offer->video_codec;
        codec.channels = 0;
        codec.rtx_payload_type = 0;
        codec.supports_nack = 1;
        codec.supports_pli = 1;
        codec.supports_fir = 1;
        codec.supports_remb = 1;
        codec.supports_transport_cc = sdp_media_transport_cc_ext_id(remote_offer->video_media) > 0 ? 1 : 0;
        if (codec.supports_transport_cc) {
            int ext_id = sdp_media_transport_cc_ext_id(remote_offer->video_media);
            sdp_media_add_extension(media, ext_id, TRANSPORT_CC_URI);
            turbo_media_track_set_transport_cc_ext_id(g_video_track, ext_id);
        } else {
            turbo_media_track_set_transport_cc_ext_id(g_video_track, 0);
        }
        sdp_media_add_codec(media, &codec);
        configure_answer_media_common(media);

        if (!g_receive_mode && turbo_media_track_get_ssrc(g_video_track) != 0) {
            sdp_media_add_ssrc(media, turbo_media_track_get_ssrc(g_video_track), "turbonet", "turbonet-stream");
            if (media->ssrc_count > 0) {
                strncpy(media->ssrcs[media->ssrc_count - 1].track_id, "video-track",
                        sizeof(media->ssrcs[media->ssrc_count - 1].track_id) - 1);
            }
        }
    }

    len = sdp_generate_answer(&local_sdp, remote_sdp, sdp_buf, sizeof(sdp_buf));
    if (len <= 0) {
        return -1;
    }

    printf("\n=== Local SDP Answer (copy to browser) ===\n");
    printf("-----BEGIN SDP ANSWER-----\n");
    print_sdp_for_copy_paste(sdp_buf);
    printf("-----END SDP ANSWER-----\n");
    printf("========================================\n\n");
    return 0;
}

static int parse_remote_offer(const char *sdp_str, sdp_session_t *remote_sdp,
                              remote_offer_info_t *offer_info) {
    sdp_media_t *audio_media;
    sdp_media_t *video_media;
    const sdp_media_t *transport_media;
    const sdp_codec_t *selected_audio_codec;
    const sdp_codec_t *selected_codec;
    int candidate_total = 0;

    if (!sdp_str || !remote_sdp || !offer_info) {
        return -1;
    }

    if (sdp_parse(sdp_str, strlen(sdp_str), remote_sdp) != 0) {
        fprintf(stderr, "Failed to parse SDP offer\n");
        return -1;
    }

    memset(offer_info, 0, sizeof(*offer_info));
    audio_media = sdp_find_media_by_type(remote_sdp, SDP_MEDIA_AUDIO);
    video_media = sdp_find_media_by_type(remote_sdp, SDP_MEDIA_VIDEO);
    if (!audio_media && !video_media) {
        fprintf(stderr, "No audio/video media in remote SDP\n");
        return -1;
    }

    selected_audio_codec = choose_remote_audio_codec(audio_media);
    selected_codec = choose_remote_video_codec(video_media);
    if (audio_media && !selected_audio_codec) {
        fprintf(stderr, "No compatible audio codec in offer\n");
        return -1;
    }
    if (video_media && !selected_codec) {
        fprintf(stderr, "No compatible video codec in offer\n");
        return -1;
    }

    transport_media = audio_media ? (const sdp_media_t *)audio_media : (const sdp_media_t *)video_media;
    ice_integration_set_remote_credentials(g_ice, transport_media->ice_ufrag, transport_media->ice_pwd);
    turbo_dc_peer_set_remote_fingerprint(g_peer, transport_media->fingerprint_hash, transport_media->fingerprint);

    if (transport_media) {
        int host_candidate_count = 0;
        for (int i = 0; i < transport_media->candidate_count; i++) {
            if (is_preferred_host_candidate(&transport_media->candidates[i])) {
                host_candidate_count++;
            }
        }
        for (int i = 0; i < transport_media->candidate_count; i++) {
            const sdp_candidate_t *candidate = &transport_media->candidates[i];
            char candidate_str[512];
            if (host_candidate_count > 0 && !is_preferred_host_candidate(candidate)) {
                continue;
            }
            if (sdp_candidate_to_string(candidate, candidate_str,
                                        sizeof(candidate_str)) == 0) {
                ice_integration_add_remote_candidate(g_ice, candidate_str);
                candidate_total++;
            }
        }
    }

    offer_info->audio_media = audio_media;
    offer_info->video_media = video_media;
    offer_info->audio_codec = selected_audio_codec;
    offer_info->video_codec = selected_codec;
    offer_info->transport_media = transport_media;

    if (audio_media) {
        printf("[SDP] Remote audio codecs: %d\n", audio_media->codec_count);
        printf("[SDP] Selected audio codec: %s (PT=%d)\n",
               selected_audio_codec->name, selected_audio_codec->payload_type);
    }
    if (video_media) {
        printf("[SDP] Remote video codecs: %d\n", video_media->codec_count);
        printf("[SDP] Selected video codec: %s (PT=%d)\n",
               selected_codec->name, selected_codec->payload_type);
    }
    printf("[SDP] Remote candidates in SDP: %d\n", candidate_total);
    return 0;
}

static void generate_test_frame(uint8_t *frame, int width, int height, uint64_t frame_index) {
    size_t y_size = (size_t)width * (size_t)height;
    size_t uv_size = y_size / 4;
    int x;
    int y;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            frame[(size_t)y * (size_t)width + (size_t)x] =
                (uint8_t)((x + (int)(frame_index * 3) + (y / 2)) & 0xFF);
        }
    }

    for (y = 0; y < height / 2; y++) {
        for (x = 0; x < width / 2; x++) {
            frame[y_size + (size_t)y * (size_t)(width / 2) + (size_t)x] =
                (uint8_t)(96 + ((y + (int)frame_index) % 96));
            frame[y_size + uv_size + (size_t)y * (size_t)(width / 2) + (size_t)x] =
                (uint8_t)(160 + ((x + (int)(frame_index * 2)) % 64));
        }
    }
}

static void generate_test_audio_frame(int16_t *samples, uint64_t frame_index) {
    for (int i = 0; i < AUDIO_SAMPLES_PER_CH; i++) {
        double phase = (double)((frame_index * AUDIO_SAMPLES_PER_CH) + (uint64_t)i);
        int16_t sample = (int16_t)(phase * 17.0);
        for (int ch = 0; ch < AUDIO_CHANNELS; ch++) {
            samples[(i * AUDIO_CHANNELS) + ch] = sample;
        }
    }
}

int main(int argc, char **argv) {
    const char *stun_servers[] = {
        "stun:stun.l.google.com:19302",
        "stun:stun1.l.google.com:19302"
    };
    int use_stun = 1;
    char sdp_buf[16384] = {0};
    char line[1024];
    size_t sdp_len = 0;
    uint8_t *frame = NULL;
    uint64_t last_video_frame_ms = 0;
    uint64_t last_audio_frame_ms = 0;
    uint64_t last_recv_stats_ms = 0;
    uint64_t last_reported_audio_packets_recv = 0;
    uint64_t last_reported_audio_packets_lost = 0;
    uint64_t last_reported_packets_recv = 0;
    uint64_t last_reported_packets_lost = 0;
    sdp_session_t remote_sdp;
    remote_offer_info_t remote_offer = {0};
    turbo_media_track_config_t track_config;
    int16_t *audio_frame = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-stun") == 0) {
            use_stun = 0;
        } else if (strcmp(argv[i], "--receive") == 0) {
            g_receive_mode = 1;
        }
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("=== TurboNet Browser Media Interop (Native A/V %s) ===\n\n",
           g_receive_mode ? "Receiver" : "Sender");

    g_loop = turbo_loop_create();
    if (!g_loop) {
        fprintf(stderr, "Failed to create event loop\n");
        return 1;
    }

    {
        turbo_dc_config_t dc_config = {
            .is_server = 1,
            .transport = TURBO_DC_TRANSPORT_ICE
        };

        g_dc_ctx = turbo_dc_context_create(&dc_config);
    }
    if (!g_dc_ctx) {
        fprintf(stderr, "Failed to create DataChannel context\n");
        turbo_loop_destroy(g_loop);
        return 1;
    }

    g_peer = turbo_dc_peer_create(g_dc_ctx, NULL, 0, NULL);
    if (!g_peer) {
        fprintf(stderr, "Failed to create peer\n");
        turbo_dc_context_destroy(g_dc_ctx);
        turbo_loop_destroy(g_loop);
        return 1;
    }

    turbo_dc_peer_on_state(g_peer, on_peer_state);
    turbo_dc_peer_set_dtls_role(g_peer, 0);

    g_ice = ice_integration_create(
        g_peer, g_loop,
        use_stun ? stun_servers : NULL, use_stun ? 2 : 0,
        NULL, NULL, NULL, 0);
    if (!g_ice) {
        fprintf(stderr, "Failed to create ICE integration\n");
        turbo_dc_peer_destroy(g_peer);
        turbo_dc_context_destroy(g_dc_ctx);
        turbo_loop_destroy(g_loop);
        return 1;
    }

    ice_integration_on_candidate(g_ice, on_ice_candidate, NULL);
    ice_integration_on_state_change(g_ice, on_ice_state, NULL);

    g_media = turbo_media_create(g_peer, NULL);
    if (!g_media) {
        fprintf(stderr, "Failed to create media context\n");
        ice_integration_destroy(g_ice);
        turbo_dc_peer_destroy(g_peer);
        turbo_dc_context_destroy(g_dc_ctx);
        turbo_loop_destroy(g_loop);
        return 1;
    }

    if (!g_receive_mode) {
        frame = (uint8_t *)malloc((FRAME_WIDTH * FRAME_HEIGHT * 3) / 2);
        audio_frame = (int16_t *)malloc(sizeof(int16_t) * AUDIO_SAMPLES_PER_CH * AUDIO_CHANNELS);
        if (!frame || !audio_frame) {
            fprintf(stderr, "Failed to allocate frame buffer\n");
            free(audio_frame);
            free(frame);
            turbo_media_destroy(g_media);
            ice_integration_destroy(g_ice);
            turbo_dc_peer_destroy(g_peer);
            turbo_dc_context_destroy(g_dc_ctx);
            turbo_loop_destroy(g_loop);
            return 1;
        }
    }

    printf("[ICE] Starting candidate gathering...\n");
    if (ice_integration_start_gathering(g_ice) != 0) {
        fprintf(stderr, "Failed to start ICE gathering\n");
        free(frame);
        turbo_media_destroy(g_media);
        ice_integration_destroy(g_ice);
        turbo_dc_peer_destroy(g_peer);
        turbo_dc_context_destroy(g_dc_ctx);
        turbo_loop_destroy(g_loop);
        return 1;
    }

    printf("[ICE] Waiting for candidate gathering to complete...\n");
    while (g_running && !ice_integration_is_gathering_complete(g_ice)) {
        turbo_loop_poll(g_loop, 50, 1);
        ice_integration_poll(g_ice);
    }

    printf("\nPaste browser SDP offer after ICE gathering completes (end with empty line):\n");
    while (fgets(line, sizeof(line), stdin)) {
        if (line[0] == '\n' || line[0] == '\r') {
            break;
        }

        if (sdp_len + strlen(line) + 1 < sizeof(sdp_buf)) {
            memcpy(sdp_buf + sdp_len, line, strlen(line));
            sdp_len += strlen(line);
        }
    }

    if (sdp_len == 0 || parse_remote_offer(sdp_buf, &remote_sdp, &remote_offer) != 0) {
        free(audio_frame);
        free(frame);
        turbo_media_destroy(g_media);
        ice_integration_destroy(g_ice);
        turbo_dc_peer_destroy(g_peer);
        turbo_dc_context_destroy(g_dc_ctx);
        turbo_loop_destroy(g_loop);
        return 1;
    }

    if (remote_offer.audio_media && remote_offer.audio_codec) {
        memset(&track_config, 0, sizeof(track_config));
        track_config.type = TURBO_RTC_MEDIA_TRACK_AUDIO;
        track_config.direction = g_receive_mode ? TURBO_MEDIA_DIRECTION_RECVONLY : TURBO_MEDIA_DIRECTION_SENDONLY;
        track_config.codec = sdp_audio_codec_to_turbo(remote_offer.audio_codec);
        track_config.audio.sample_rate = AUDIO_SAMPLE_RATE;
        track_config.audio.channels = AUDIO_CHANNELS;
        track_config.audio.bitrate = 64000;
        track_config.audio.frame_size_ms = AUDIO_FRAME_MS;
        track_config.audio.enable_fec = 1;
        track_config.audio.enable_dtx = 0;

        g_audio_track = turbo_media_add_track(g_media, &track_config);
        if (!g_audio_track) {
            fprintf(stderr, "Failed to create audio track\n");
            free(audio_frame);
            free(frame);
            turbo_media_destroy(g_media);
            ice_integration_destroy(g_ice);
            turbo_dc_peer_destroy(g_peer);
            turbo_dc_context_destroy(g_dc_ctx);
            turbo_loop_destroy(g_loop);
            return 1;
        }
        turbo_media_track_set_payload_type(g_audio_track, (uint8_t)remote_offer.audio_codec->payload_type);
        if (g_receive_mode) {
            turbo_media_track_on_frame(g_audio_track, on_audio_frame);
        }
        printf("[Media] Configured native %s audio codec: %s\n",
               g_receive_mode ? "receiver" : "sender",
               turbo_codec_label(sdp_audio_codec_to_turbo(remote_offer.audio_codec)));
    }

    if (remote_offer.video_media && remote_offer.video_codec) {
        g_selected_video_codec = sdp_codec_to_turbo(remote_offer.video_codec);

        memset(&track_config, 0, sizeof(track_config));
        track_config.type = TURBO_RTC_MEDIA_TRACK_VIDEO;
        track_config.direction = g_receive_mode ? TURBO_MEDIA_DIRECTION_RECVONLY : TURBO_MEDIA_DIRECTION_SENDONLY;
        track_config.codec = g_selected_video_codec;
        track_config.video.width = FRAME_WIDTH;
        track_config.video.height = FRAME_HEIGHT;
        track_config.video.framerate = FRAME_RATE;
        track_config.video.bitrate = 400000;
        track_config.video.keyframe_interval = FRAME_RATE;

        g_video_track = turbo_media_add_track(g_media, &track_config);
        if (!g_video_track) {
            fprintf(stderr, "Failed to create %s track\n", turbo_codec_label(g_selected_video_codec));
            free(audio_frame);
            free(frame);
            turbo_media_destroy(g_media);
            ice_integration_destroy(g_ice);
            turbo_dc_peer_destroy(g_peer);
            turbo_dc_context_destroy(g_dc_ctx);
            turbo_loop_destroy(g_loop);
            return 1;
        }
        turbo_media_track_set_payload_type(g_video_track, (uint8_t)remote_offer.video_codec->payload_type);
        if (g_receive_mode) {
            turbo_media_track_on_frame(g_video_track, on_video_frame);
        }
        printf("[Media] Configured native %s video codec: %s\n",
               g_receive_mode ? "receiver" : "sender", turbo_codec_label(g_selected_video_codec));
    }

    if (print_local_answer(&remote_sdp, &remote_offer) != 0) {
        fprintf(stderr, "Failed to generate local SDP answer\n");
        free(audio_frame);
        free(frame);
        turbo_media_destroy(g_media);
        ice_integration_destroy(g_ice);
        turbo_dc_peer_destroy(g_peer);
        turbo_dc_context_destroy(g_dc_ctx);
        turbo_loop_destroy(g_loop);
        return 1;
    }

    ice_integration_end_of_candidates(g_ice);
    printf("[Status] Waiting for browser connection...\n");

    while (g_running) {
        uint64_t now_ms = turbo_monotonic_ms();

        turbo_loop_poll(g_loop, 10, 1);
        ice_integration_poll(g_ice);
        try_start_media();
        if (g_media) {
            turbo_media_handle_timers(g_media);
        }

        if (g_receive_mode && g_connected &&
            now_ms - last_recv_stats_ms >= 1000) {
            if (g_audio_track) {
                turbo_media_stats_t stats;
                turbo_media_track_get_stats(g_audio_track, &stats);
                if (stats.packets_recv != last_reported_audio_packets_recv ||
                    stats.packets_lost != last_reported_audio_packets_lost) {
                    printf("[Media] Audio receive stats packets=%llu frames=%llu bytes=%llu drops=%llu\n",
                           (unsigned long long)stats.packets_recv,
                           (unsigned long long)stats.frames_recv,
                           (unsigned long long)stats.bytes_recv,
                           (unsigned long long)stats.packets_lost);
                    last_reported_audio_packets_recv = stats.packets_recv;
                    last_reported_audio_packets_lost = stats.packets_lost;
                }
            }
            if (g_video_track) {
                turbo_media_stats_t stats;
                turbo_media_track_get_stats(g_video_track, &stats);
                if (stats.packets_recv != last_reported_packets_recv ||
                    stats.packets_lost != last_reported_packets_lost) {
                    printf("[Media] Video receive stats packets=%llu frames=%llu bytes=%llu drops=%llu\n",
                           (unsigned long long)stats.packets_recv,
                           (unsigned long long)stats.frames_recv,
                           (unsigned long long)stats.bytes_recv,
                           (unsigned long long)stats.packets_lost);
                    last_reported_packets_recv = stats.packets_recv;
                    last_reported_packets_lost = stats.packets_lost;
                }
            }
            last_recv_stats_ms = now_ms;
        }

        if (!g_receive_mode && g_connected && g_audio_track &&
            turbo_media_track_get_state(g_audio_track) == TURBO_MEDIA_STATE_ACTIVE &&
            now_ms - last_audio_frame_ms >= AUDIO_FRAME_MS) {
            generate_test_audio_frame(audio_frame, g_audio_frames_sent);
            if (turbo_media_track_send_frame(g_audio_track, (const uint8_t *)audio_frame,
                                             sizeof(int16_t) * AUDIO_SAMPLES_PER_CH * AUDIO_CHANNELS,
                                             g_audio_frames_sent * AUDIO_SAMPLES_PER_CH) == 0) {
                g_audio_frames_sent++;
                last_audio_frame_ms = now_ms;
                if ((g_audio_frames_sent % 50) == 0) {
                    printf("[Media] Sent %llu audio frames\n", (unsigned long long)g_audio_frames_sent);
                }
            }
        }

        if (!g_receive_mode && g_connected && g_video_track &&
            turbo_media_track_get_state(g_video_track) == TURBO_MEDIA_STATE_ACTIVE &&
            now_ms - last_video_frame_ms >= (1000 / FRAME_RATE)) {
            generate_test_frame(frame, FRAME_WIDTH, FRAME_HEIGHT, g_video_frames_sent);
            if (turbo_media_track_send_frame(g_video_track, frame,
                                             (FRAME_WIDTH * FRAME_HEIGHT * 3) / 2, 0) == 0) {
                g_video_frames_sent++;
                last_video_frame_ms = now_ms;
                if ((g_video_frames_sent % FRAME_RATE) == 0) {
                    printf("[Media] Sent %llu video frames\n", (unsigned long long)g_video_frames_sent);
                }
            }
        }
    }

    printf("\n[Cleanup] Shutting down...\n");
    free(audio_frame);
    free(frame);
    turbo_media_destroy(g_media);
    ice_integration_destroy(g_ice);
    turbo_dc_peer_destroy(g_peer);
    turbo_dc_context_destroy(g_dc_ctx);
    turbo_loop_destroy(g_loop);
    return 0;
}
