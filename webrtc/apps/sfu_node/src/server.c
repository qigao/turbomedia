#include "sfu_node/server.h"
#include "sfu_node/http_api.h"
#include "turbo_recorder.h"
#include "turbo_sfu_node.h"
#include "turbo_peer_connection.h"
#include "turbo_rtp.h"
#ifdef ENABLE_RTC_SCXML_WORKFLOW
#include "rtc_workflow_adapter.h"
#endif
#include <platform.h>
#include <turbo_thread.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
static void sfu_node_sleep_ms(unsigned int ms) { Sleep(ms); }
#define sfu_strdup _strdup
#define sfu_stricmp _stricmp
#else
#include <unistd.h>
#include <strings.h>
static void sfu_node_sleep_ms(unsigned int ms) { usleep(ms * 1000); }
#define sfu_strdup strdup
#define sfu_stricmp strcasecmp
#endif

#define SFU_NODE_LOCAL_SDP_CAPACITY 16384
#define SFU_NODE_RECORDING_ID_CAPACITY 128
#define SFU_NODE_RECORDING_MODE_CAPACITY 32
#define SFU_NODE_RECORDING_PATH_CAPACITY 512

typedef struct {
    char track_id[TURBO_TRACK_ID_MAX];
    turbo_media_track_t *track;
    uint32_t source_main_ssrc;
    uint32_t source_layer_ssrcs[3];
    int source_layer_count;
} sfu_node_relay_track_t;

typedef struct {
    char room_id[TURBO_ROOM_ID_MAX];
    char participant_id[TURBO_PARTICIPANT_ID_MAX];
    char track_id[TURBO_TRACK_ID_MAX];
    uint32_t main_ssrc;
    uint32_t layer_ssrcs[3];
    int layer_count;
    turbo_room_track_kind_t kind;
    turbo_codec_type_t codec;
    uint8_t payload_type;
    int metadata_ready;
} sfu_node_published_track_t;

typedef struct {
    char room_id[TURBO_ROOM_ID_MAX];
    char receiver_participant_id[TURBO_PARTICIPANT_ID_MAX];
    char track_id[TURBO_TRACK_ID_MAX];
    int enabled;
    int priority;
    turbo_room_video_layer_t preferred_layer;
    turbo_room_video_layer_t target_layer;
    int muted;
    char policy_source[TURBO_POLICY_SOURCE_MAX];
    turbo_room_video_layer_t max_layer;
} sfu_node_desired_subscription_t;

typedef struct {
    char participant_id[TURBO_PARTICIPANT_ID_MAX];
    char track_id[TURBO_TRACK_ID_MAX];
    uint32_t main_ssrc;
    uint32_t layer_ssrcs[3];
    int layer_count;
    turbo_room_track_kind_t kind;
    turbo_codec_type_t codec;
    int recorder_track_id;
    rtp_recorder_ctx_t *rtp_ctx;
    int64_t packet_count;
} sfu_node_recording_track_t;

typedef struct {
    char room_id[TURBO_ROOM_ID_MAX];
    char recording_id[SFU_NODE_RECORDING_ID_CAPACITY];
    char mode[SFU_NODE_RECORDING_MODE_CAPACITY];
    char output_path[SFU_NODE_RECORDING_PATH_CAPACITY];
    turbo_recorder_t *recorder;
    sfu_node_recording_track_t *tracks;
    int track_count;
    int track_capacity;
    int active;
    int64_t packet_count;
    int64_t bytes_written;
    int64_t duration_us;
} sfu_node_room_recording_t;

typedef struct {
    char room_id[TURBO_ROOM_ID_MAX];
    char participant_id[TURBO_PARTICIPANT_ID_MAX];
    char session_id[TURBO_PARTICIPANT_ID_MAX];
    struct sfu_node_app_server_s *server;
    turbo_peer_connection_t *pc;
    turbo_mutex_t mutex;
    turbo_peer_state_t state;
    int remote_description_set;
    int remote_track_count;
    int remote_frame_count;
    char *local_answer;
    char **local_candidates;
    int local_candidate_count;
    int local_candidate_capacity;
    uint64_t resource_version;
    int owns_node_session;
    sfu_node_relay_track_t *relay_tracks;
    int relay_track_count;
    int relay_track_capacity;
#ifdef ENABLE_RTC_SCXML_WORKFLOW
    rtc_workflow_context_t *workflow_ctx;
#endif
} sfu_node_webrtc_session_t;

typedef enum {
    SFU_NODE_WEBRTC_COMMAND_CREATE = 1,
    SFU_NODE_WEBRTC_COMMAND_CREATE_OWNED,
    SFU_NODE_WEBRTC_COMMAND_REMOVE,
    SFU_NODE_WEBRTC_COMMAND_DISCONNECT_MEDIA_PARTICIPANT,
    SFU_NODE_WEBRTC_COMMAND_SET_OFFER,
    SFU_NODE_WEBRTC_COMMAND_ADD_CANDIDATE,
    SFU_NODE_WEBRTC_COMMAND_APPLY_SDPFRAG,
    SFU_NODE_WEBRTC_COMMAND_CLEAR
} sfu_node_webrtc_command_type_t;

typedef struct sfu_node_webrtc_command_s {
    sfu_node_webrtc_command_type_t type;
    const char *room_id;
    const char *participant_id;
    const char *session_id;
    const char *text;
    size_t text_len;
    uint64_t expected_version;
    char *output;
    size_t output_capacity;
    uint64_t *version_out;
    int result;
    int done;
    struct sfu_node_webrtc_command_s *next;
} sfu_node_webrtc_command_t;

struct sfu_node_app_server_s {
    sfu_node_app_config_t config;
    turbo_sfu_node_t *node;
    sfu_node_http_api_t *http_api;
    int running;
    int draining;
    int webrtc_running;
    int webrtc_thread_started;
    turbo_thread_t webrtc_thread;
    turbo_mutex_t webrtc_command_mutex;
    turbo_cond_t webrtc_command_cond;
    sfu_node_webrtc_command_t *webrtc_command_head;
    sfu_node_webrtc_command_t *webrtc_command_tail;
    turbo_mutex_t mutex;
    sfu_node_webrtc_session_t **webrtc_sessions;
    int webrtc_session_count;
    int webrtc_session_capacity;
    sfu_node_published_track_t *published_tracks;
    int published_track_count;
    int published_track_capacity;
    sfu_node_desired_subscription_t *subscriptions;
    int subscription_count;
    int subscription_capacity;
    turbo_mutex_t recording_mutex;
    sfu_node_room_recording_t *room_recordings;
    int room_recording_count;
    int room_recording_capacity;
};

static int create_webrtc_session_impl(sfu_node_app_server_t *server,
                                      const char *room_id,
                                      const char *participant_id,
                                      const char *session_id);
static int create_owned_media_session_impl(sfu_node_app_server_t *server,
                                           const char *room_id,
                                           const char *participant_id,
                                           const char *session_id);
static int remove_webrtc_session_impl(sfu_node_app_server_t *server,
                                      const char *room_id,
                                      const char *session_id);
static int disconnect_media_participant_impl(
    sfu_node_app_server_t *server, const char *room_id,
    const char *participant_id);
static int set_remote_offer_impl(sfu_node_app_server_t *server,
                                 const char *room_id,
                                 const char *session_id,
                                 const char *sdp);
static int add_remote_ice_candidate_impl(sfu_node_app_server_t *server,
                                         const char *room_id,
                                         const char *session_id,
                                         const char *candidate);
static int apply_remote_ice_sdpfrag_impl(
    sfu_node_app_server_t *server, const char *room_id, const char *participant_id,
    const char *session_id, uint64_t expected_version, const char *sdpfrag,
    size_t sdpfrag_len, char *local_sdpfrag_out,
    size_t local_sdpfrag_capacity, uint64_t *version_out);

static void copy_string(char *dest, size_t dest_size, const char *src) {
    if (!dest || dest_size == 0) {
        return;
    }

    if (!src) {
        dest[0] = '\0';
        return;
    }

    strncpy(dest, src, dest_size - 1);
    dest[dest_size - 1] = '\0';
}

static int ensure_capacity(void **items, int *capacity, size_t item_size,
                           int count_needed) {
    void *new_items;
    int new_capacity;

    if (!items || !capacity || item_size == 0 || count_needed <= *capacity) {
        return 0;
    }

    new_capacity = (*capacity > 0) ? *capacity : 4;
    while (new_capacity < count_needed) {
        new_capacity *= 2;
    }

    new_items = realloc(*items, item_size * (size_t)new_capacity);
    if (!new_items) {
        return -1;
    }

    *items = new_items;
    *capacity = new_capacity;
    return 0;
}

static turbo_codec_type_t codec_from_name(const char *codec_name) {
    if (!codec_name) {
        return 0;
    }
    if (sfu_stricmp(codec_name, "opus") == 0) {
        return TURBO_CODEC_OPUS;
    }
    if (sfu_stricmp(codec_name, "pcmu") == 0) {
        return TURBO_CODEC_PCMU;
    }
    if (sfu_stricmp(codec_name, "pcma") == 0) {
        return TURBO_CODEC_PCMA;
    }
    if (sfu_stricmp(codec_name, "vp8") == 0) {
        return TURBO_CODEC_VP8;
    }
    if (sfu_stricmp(codec_name, "vp9") == 0) {
        return TURBO_CODEC_VP9;
    }
    if (sfu_stricmp(codec_name, "h264") == 0) {
        return TURBO_CODEC_H264;
    }
    if (sfu_stricmp(codec_name, "h265") == 0 || sfu_stricmp(codec_name, "hevc") == 0) {
        return TURBO_CODEC_H265;
    }
    return 0;
}

static uint8_t payload_type_for_codec(turbo_codec_type_t codec) {
    switch (codec) {
        case TURBO_CODEC_PCMU: return 0;
        case TURBO_CODEC_PCMA: return 8;
        case TURBO_CODEC_OPUS: return 111;
        case TURBO_CODEC_VP8: return 96;
        case TURBO_CODEC_VP9: return 98;
        case TURBO_CODEC_H264: return 102;
        case TURBO_CODEC_H265: return 103;
        default: return 0;
    }
}

static turbo_recorder_codec_t recorder_codec_from_track(
    const sfu_node_published_track_t *track) {
    if (!track) {
        return (turbo_recorder_codec_t)-1;
    }

    switch (track->codec) {
        case TURBO_CODEC_OPUS: return TURBO_RECORDER_CODEC_OPUS;
        case TURBO_CODEC_PCMU: return TURBO_RECORDER_CODEC_PCMU;
        case TURBO_CODEC_PCMA: return TURBO_RECORDER_CODEC_PCMA;
        case TURBO_CODEC_VP8: return TURBO_RECORDER_CODEC_VP8;
        case TURBO_CODEC_VP9: return TURBO_RECORDER_CODEC_VP9;
        case TURBO_CODEC_H264: return TURBO_RECORDER_CODEC_H264;
        case TURBO_CODEC_H265: return TURBO_RECORDER_CODEC_H265;
        default: return (turbo_recorder_codec_t)-1;
    }
}

static int recording_format_from_mode(const char *mode, turbo_recorder_format_t *format,
                                      const char **extension) {
    if (!mode || !format || !extension) {
        return -1;
    }

    if (strcmp(mode, "archive") == 0 || strcmp(mode, "mkv") == 0) {
        *format = TURBO_RECORDER_FORMAT_MKV;
        *extension = "mkv";
        return 0;
    }
    if (strcmp(mode, "webm") == 0) {
        *format = TURBO_RECORDER_FORMAT_WEBM;
        *extension = "webm";
        return 0;
    }
    if (strcmp(mode, "mp4") == 0) {
        *format = TURBO_RECORDER_FORMAT_MP4;
        *extension = "mp4";
        return 0;
    }

    return -1;
}

static void sanitize_filename_component(const char *src, char *dest, size_t dest_size,
                                        const char *fallback) {
    size_t written = 0;

    if (!dest || dest_size == 0) {
        return;
    }

    dest[0] = '\0';
    if (!src || src[0] == '\0') {
        copy_string(dest, dest_size, fallback);
        return;
    }

    while (*src != '\0' && written + 1 < dest_size) {
        unsigned char ch = (unsigned char)*src++;

        if (isalnum(ch) || ch == '-' || ch == '_') {
            dest[written++] = (char)ch;
        } else {
            dest[written++] = '_';
        }
    }

    dest[written] = '\0';
    if (written == 0) {
        copy_string(dest, dest_size, fallback);
    }
}

static void build_recording_output_path(const char *room_id, const char *recording_id,
                                        const char *extension, char *output_path,
                                        size_t output_path_size) {
    char safe_room[TURBO_ROOM_ID_MAX];
    char safe_recording[SFU_NODE_RECORDING_ID_CAPACITY];

    sanitize_filename_component(room_id, safe_room, sizeof(safe_room), "room");
    sanitize_filename_component(recording_id, safe_recording, sizeof(safe_recording),
                                "recording");
    snprintf(output_path, output_path_size, "recording_%s_%s.%s",
             safe_room, safe_recording, extension ? extension : "mkv");
}

static void fill_recorder_track_config(const sfu_node_published_track_t *published_track,
                                       turbo_recorder_track_config_t *config) {
    memset(config, 0, sizeof(*config));
    config->type = (published_track->kind == TURBO_ROOM_TRACK_AUDIO)
                       ? TURBO_RECORDER_TRACK_AUDIO
                       : TURBO_RECORDER_TRACK_VIDEO;
    config->codec = recorder_codec_from_track(published_track);
    config->rtp_payload_type = published_track->payload_type;

    if (config->type == TURBO_RECORDER_TRACK_AUDIO) {
        if (published_track->codec == TURBO_CODEC_PCMU ||
            published_track->codec == TURBO_CODEC_PCMA) {
            config->sample_rate = 8000;
            config->channels = 1;
        } else {
            config->sample_rate = 48000;
            config->channels = 2;
        }
    } else {
        config->width = 1280;
        config->height = 720;
        config->framerate = 30;
    }
}

static sfu_node_published_track_t *find_published_track_locked(
    sfu_node_app_server_t *server, const char *room_id, const char *track_id) {
    int i;

    if (!server || !room_id || !track_id) {
        return NULL;
    }

    for (i = 0; i < server->published_track_count; ++i) {
        sfu_node_published_track_t *track = &server->published_tracks[i];
        if (strcmp(track->room_id, room_id) == 0 && strcmp(track->track_id, track_id) == 0) {
            return track;
        }
    }

    return NULL;
}

static sfu_node_published_track_t *find_published_track_by_ssrc_locked(
    sfu_node_app_server_t *server, const char *room_id, uint32_t ssrc) {
    int i;
    int j;

    if (!server || !room_id || ssrc == 0) {
        return NULL;
    }

    for (i = 0; i < server->published_track_count; ++i) {
        sfu_node_published_track_t *track = &server->published_tracks[i];
        if (strcmp(track->room_id, room_id) != 0) {
            continue;
        }
        if (track->main_ssrc == ssrc) {
            return track;
        }
        for (j = 0; j < track->layer_count && j < 3; ++j) {
            if (track->layer_ssrcs[j] == ssrc) {
                return track;
            }
        }
    }

    return NULL;
}

static sfu_node_desired_subscription_t *find_subscription_locked(
    sfu_node_app_server_t *server, const char *room_id,
    const char *receiver_participant_id, const char *track_id) {
    int i;

    if (!server || !room_id || !receiver_participant_id || !track_id) {
        return NULL;
    }

    for (i = 0; i < server->subscription_count; ++i) {
        sfu_node_desired_subscription_t *subscription = &server->subscriptions[i];
        if (strcmp(subscription->room_id, room_id) == 0 &&
            strcmp(subscription->receiver_participant_id, receiver_participant_id) == 0 &&
            strcmp(subscription->track_id, track_id) == 0) {
            return subscription;
        }
    }

    return NULL;
}

static sfu_node_room_recording_t *find_room_recording_locked(
    sfu_node_app_server_t *server, const char *room_id) {
    int i;

    if (!server || !room_id) {
        return NULL;
    }

    for (i = 0; i < server->room_recording_count; ++i) {
        sfu_node_room_recording_t *recording = &server->room_recordings[i];
        if (strcmp(recording->room_id, room_id) == 0) {
            return recording;
        }
    }

    return NULL;
}

static sfu_node_recording_track_t *find_recording_track_by_ssrc_locked(
    sfu_node_room_recording_t *recording, const char *participant_id, uint32_t ssrc) {
    int i;
    int j;

    if (!recording || !participant_id || ssrc == 0) {
        return NULL;
    }

    for (i = 0; i < recording->track_count; ++i) {
        sfu_node_recording_track_t *track = &recording->tracks[i];

        if (strcmp(track->participant_id, participant_id) != 0) {
            continue;
        }
        if (track->main_ssrc == ssrc) {
            return track;
        }
        for (j = 0; j < track->layer_count && j < 3; ++j) {
            if (track->layer_ssrcs[j] == ssrc) {
                return track;
            }
        }
    }

    return NULL;
}

static void refresh_room_recording_stats_locked(sfu_node_room_recording_t *recording) {
    int track_count = 0;

    if (!recording || !recording->recorder) {
        return;
    }

    turbo_recorder_get_stats(recording->recorder,
                             &recording->duration_us,
                             &recording->bytes_written,
                             &track_count);
}

static void destroy_room_recording_rtp_contexts_locked(
    sfu_node_room_recording_t *recording) {
    int i;

    if (!recording) {
        return;
    }

    for (i = 0; i < recording->track_count; ++i) {
        if (recording->tracks[i].rtp_ctx) {
            turbo_recorder_destroy_rtp_context(recording->tracks[i].rtp_ctx);
            recording->tracks[i].rtp_ctx = NULL;
        }
    }
}

static void destroy_room_recording_locked(sfu_node_room_recording_t *recording) {
    if (!recording) {
        return;
    }

    destroy_room_recording_rtp_contexts_locked(recording);
    if (recording->recorder) {
        turbo_recorder_destroy(recording->recorder);
        recording->recorder = NULL;
    }
    free(recording->tracks);
    recording->tracks = NULL;
    recording->track_count = 0;
    recording->track_capacity = 0;
}

static void remove_room_recording_locked(sfu_node_app_server_t *server, const char *room_id) {
    int i;

    if (!server || !room_id) {
        return;
    }

    for (i = 0; i < server->room_recording_count; ++i) {
        if (strcmp(server->room_recordings[i].room_id, room_id) != 0) {
            continue;
        }

        destroy_room_recording_locked(&server->room_recordings[i]);
        if (i + 1 < server->room_recording_count) {
            memmove(&server->room_recordings[i], &server->room_recordings[i + 1],
                    (size_t)(server->room_recording_count - i - 1) *
                        sizeof(sfu_node_room_recording_t));
        }
        server->room_recording_count--;
        return;
    }
}

static sfu_node_relay_track_t *find_relay_track_locked(sfu_node_webrtc_session_t *session,
                                                       const char *track_id) {
    int i;

    if (!session || !track_id) {
        return NULL;
    }

    for (i = 0; i < session->relay_track_count; ++i) {
        if (strcmp(session->relay_tracks[i].track_id, track_id) == 0) {
            return &session->relay_tracks[i];
        }
    }

    return NULL;
}

static void bind_relay_track_source_locked(sfu_node_relay_track_t *relay_track,
                                           const sfu_node_published_track_t *published_track) {
    int i;

    if (!relay_track || !published_track) {
        return;
    }

    relay_track->source_main_ssrc = published_track->main_ssrc;
    relay_track->source_layer_count =
        (published_track->layer_count > 0 && published_track->layer_count <= 3)
            ? published_track->layer_count
            : 1;
    for (i = 0; i < relay_track->source_layer_count; ++i) {
        relay_track->source_layer_ssrcs[i] = published_track->layer_ssrcs[i];
    }
}

static sfu_node_relay_track_t *find_relay_track_by_source_ssrc_locked(
    sfu_node_webrtc_session_t *session, uint32_t ssrc) {
    int i;
    int j;

    if (!session || ssrc == 0) {
        return NULL;
    }

    for (i = 0; i < session->relay_track_count; ++i) {
        sfu_node_relay_track_t *relay_track = &session->relay_tracks[i];

        if (relay_track->source_main_ssrc == ssrc) {
            return relay_track;
        }
        for (j = 0; j < relay_track->source_layer_count && j < 3; ++j) {
            if (relay_track->source_layer_ssrcs[j] == ssrc) {
                return relay_track;
            }
        }
    }

    return NULL;
}

static void clear_room_runtime_state_locked(sfu_node_app_server_t *server, const char *room_id) {
    int i;

    if (!server || !room_id) {
        return;
    }

    for (i = server->published_track_count - 1; i >= 0; --i) {
        if (strcmp(server->published_tracks[i].room_id, room_id) != 0) {
            continue;
        }
        if (i + 1 < server->published_track_count) {
            memmove(&server->published_tracks[i], &server->published_tracks[i + 1],
                    (size_t)(server->published_track_count - i - 1) *
                        sizeof(sfu_node_published_track_t));
        }
        server->published_track_count--;
    }

    for (i = server->subscription_count - 1; i >= 0; --i) {
        if (strcmp(server->subscriptions[i].room_id, room_id) != 0) {
            continue;
        }
        if (i + 1 < server->subscription_count) {
            memmove(&server->subscriptions[i], &server->subscriptions[i + 1],
                    (size_t)(server->subscription_count - i - 1) *
                        sizeof(sfu_node_desired_subscription_t));
        }
        server->subscription_count--;
    }
}

static void fill_relay_track_config(const sfu_node_published_track_t *published_track,
                                    turbo_media_track_config_t *config) {
    memset(config, 0, sizeof(*config));
    config->type = (published_track->kind == TURBO_ROOM_TRACK_AUDIO)
                       ? TURBO_RTC_MEDIA_TRACK_AUDIO
                       : TURBO_RTC_MEDIA_TRACK_VIDEO;
    config->direction = TURBO_MEDIA_DIRECTION_SENDONLY;
    config->codec = published_track->codec;

    if (config->type == TURBO_RTC_MEDIA_TRACK_AUDIO) {
        if (published_track->codec == TURBO_CODEC_PCMU || published_track->codec == TURBO_CODEC_PCMA) {
            config->audio.sample_rate = 8000;
            config->audio.channels = 1;
        } else {
            config->audio.sample_rate = 48000;
            config->audio.channels = 2;
        }
        config->audio.bitrate = 64000;
        config->audio.frame_size_ms = 20;
    } else {
        config->video.width = 1280;
        config->video.height = 720;
        config->video.framerate = 30;
        config->video.bitrate = 1500000;
        config->video.keyframe_interval = 30;
    }
}

static const char *peer_state_name(turbo_peer_state_t state) {
    switch (state) {
        case TURBO_PEER_STATE_NEW: return "new";
        case TURBO_PEER_STATE_CONNECTING: return "connecting";
        case TURBO_PEER_STATE_CONNECTED: return "connected";
        case TURBO_PEER_STATE_DISCONNECTED: return "disconnected";
        case TURBO_PEER_STATE_FAILED: return "failed";
        case TURBO_PEER_STATE_CLOSED: return "closed";
        default: return "unknown";
    }
}

static char *escape_json_string(const char *str) {
    size_t extra = 0;
    size_t len;
    size_t i;
    size_t j;
    char *escaped;

    if (!str) {
        return NULL;
    }

    len = strlen(str);
    for (i = 0; i < len; ++i) {
        switch (str[i]) {
            case '\"':
            case '\\':
            case '\b':
            case '\f':
            case '\n':
            case '\r':
            case '\t':
                extra++;
                break;
            default:
                break;
        }
    }

    escaped = (char *)malloc(len + extra + 1);
    if (!escaped) {
        return NULL;
    }

    for (i = 0, j = 0; i < len; ++i) {
        switch (str[i]) {
            case '\"': escaped[j++] = '\\'; escaped[j++] = '\"'; break;
            case '\\': escaped[j++] = '\\'; escaped[j++] = '\\'; break;
            case '\b': escaped[j++] = '\\'; escaped[j++] = 'b'; break;
            case '\f': escaped[j++] = '\\'; escaped[j++] = 'f'; break;
            case '\n': escaped[j++] = '\\'; escaped[j++] = 'n'; break;
            case '\r': escaped[j++] = '\\'; escaped[j++] = 'r'; break;
            case '\t': escaped[j++] = '\\'; escaped[j++] = 't'; break;
            default: escaped[j++] = str[i]; break;
        }
    }

    escaped[j] = '\0';
    return escaped;
}

static sfu_node_webrtc_session_t *find_webrtc_session_locked(
    sfu_node_app_server_t *server, const char *room_id, const char *session_id) {
    int i;

    if (!server || !room_id || !session_id) {
        return NULL;
    }

    for (i = 0; i < server->webrtc_session_count; ++i) {
        sfu_node_webrtc_session_t *session = server->webrtc_sessions[i];
        if (strcmp(session->room_id, room_id) == 0 &&
            strcmp(session->session_id, session_id) == 0) {
            return session;
        }
    }

    return NULL;
}

static sfu_node_webrtc_session_t *find_webrtc_session_by_participant_locked(
    sfu_node_app_server_t *server, const char *room_id, const char *participant_id) {
    int i;

    if (!server || !room_id || !participant_id) {
        return NULL;
    }

    for (i = 0; i < server->webrtc_session_count; ++i) {
        sfu_node_webrtc_session_t *session = server->webrtc_sessions[i];
        if (strcmp(session->room_id, room_id) == 0 &&
            strcmp(session->participant_id, participant_id) == 0) {
            return session;
        }
    }

    return NULL;
}

static int ensure_session_relay_track_locked(sfu_node_webrtc_session_t *session,
                                             const sfu_node_published_track_t *published_track) {
    turbo_media_track_config_t config;
    sfu_node_relay_track_t *relay_entry;
    turbo_media_track_t *relay_track;
    turbo_media_context_t *media_ctx;

    if (!session || !session->pc || !published_track || !published_track->metadata_ready) {
        return -1;
    }

    turbo_mutex_lock(&session->mutex);
    relay_entry = find_relay_track_locked(session, published_track->track_id);
    if (relay_entry) {
        bind_relay_track_source_locked(relay_entry, published_track);
        turbo_mutex_unlock(&session->mutex);
        return 0;
    }

    if (ensure_capacity((void **)&session->relay_tracks, &session->relay_track_capacity,
                        sizeof(sfu_node_relay_track_t), session->relay_track_count + 1) != 0) {
        turbo_mutex_unlock(&session->mutex);
        return -1;
    }

    fill_relay_track_config(published_track, &config);
    relay_track = turbo_peer_connection_add_track_ex(session->pc, &config);
    if (!relay_track) {
        turbo_mutex_unlock(&session->mutex);
        return -1;
    }

    turbo_media_track_set_payload_type(relay_track, published_track->payload_type);
    if (turbo_media_track_start(relay_track) != 0) {
        turbo_mutex_unlock(&session->mutex);
        return -1;
    }

    media_ctx = turbo_peer_connection_get_media_context(session->pc);
    if (media_ctx && session->state == TURBO_PEER_STATE_CONNECTED) {
        if (turbo_media_setup_srtp(media_ctx) != 0) {
            turbo_mutex_unlock(&session->mutex);
            return -1;
        }
    }

    relay_entry = &session->relay_tracks[session->relay_track_count];
    memset(relay_entry, 0, sizeof(*relay_entry));
    copy_string(relay_entry->track_id,
                sizeof(relay_entry->track_id),
                published_track->track_id);
    relay_entry->track = relay_track;
    bind_relay_track_source_locked(relay_entry, published_track);
    session->relay_track_count++;
    turbo_mutex_unlock(&session->mutex);
    return 0;
}

static void sync_session_relay_tracks_locked(sfu_node_app_server_t *server,
                                             sfu_node_webrtc_session_t *session) {
    int i;

    if (!server || !session) {
        return;
    }

    for (i = 0; i < server->subscription_count; ++i) {
        sfu_node_desired_subscription_t *subscription = &server->subscriptions[i];
        sfu_node_published_track_t *published_track;

        if (!subscription->enabled ||
            strcmp(subscription->room_id, session->room_id) != 0 ||
            strcmp(subscription->receiver_participant_id, session->participant_id) != 0) {
            continue;
        }

        published_track = find_published_track_locked(server, session->room_id, subscription->track_id);
        if (!published_track || !published_track->metadata_ready) {
            continue;
        }

        ensure_session_relay_track_locked(session, published_track);
    }
}

static void sync_track_to_subscribers_locked(sfu_node_app_server_t *server,
                                             const sfu_node_published_track_t *published_track) {
    int i;

    if (!server || !published_track || !published_track->metadata_ready) {
        return;
    }

    for (i = 0; i < server->subscription_count; ++i) {
        sfu_node_desired_subscription_t *subscription = &server->subscriptions[i];
        sfu_node_webrtc_session_t *session;

        if (!subscription->enabled ||
            strcmp(subscription->room_id, published_track->room_id) != 0 ||
            strcmp(subscription->track_id, published_track->track_id) != 0) {
            continue;
        }

        session = find_webrtc_session_by_participant_locked(server, published_track->room_id,
                                                            subscription->receiver_participant_id);
        if (!session) {
            continue;
        }

        ensure_session_relay_track_locked(session, published_track);
    }
}

/* Desired subscriptions may arrive before the WHEP receiver session. Apply
   them to the core SFU only after both the receiver and published track are
   present, then attach the corresponding relay tracks before SDP answer
   generation. */
static int apply_desired_subscriptions_for_participant(
    sfu_node_app_server_t *server, const char *room_id,
    const char *participant_id) {
    int count;

    if (!server || !room_id || !participant_id) {
        return -1;
    }
    turbo_mutex_lock(&server->mutex);
    count = server->subscription_count;
    turbo_mutex_unlock(&server->mutex);
    for (int i = 0; i < count; ++i) {
        turbo_sfu_node_track_subscription_t config;
        int ready = 0;

        memset(&config, 0, sizeof(config));
        turbo_mutex_lock(&server->mutex);
        if (i < server->subscription_count) {
            const sfu_node_desired_subscription_t *desired =
                &server->subscriptions[i];
            const sfu_node_published_track_t *track =
                find_published_track_locked(server, room_id,
                                            desired->track_id);
            if (strcmp(desired->room_id, room_id) == 0 &&
                strcmp(desired->receiver_participant_id, participant_id) == 0 &&
                track && track->metadata_ready) {
                copy_string(config.receiver_participant_id,
                            sizeof(config.receiver_participant_id),
                            desired->receiver_participant_id);
                copy_string(config.track_id, sizeof(config.track_id),
                            desired->track_id);
                config.enabled = desired->enabled;
                config.priority = desired->priority;
                config.preferred_layer = desired->preferred_layer;
                config.target_layer = desired->target_layer;
                config.muted = desired->muted;
                copy_string(config.policy_source,
                            sizeof(config.policy_source),
                            desired->policy_source);
                config.max_layer = desired->max_layer;
                ready = 1;
            }
        }
        turbo_mutex_unlock(&server->mutex);
        if (ready) {
            /* Core participants/tracks may be provisioned after app metadata.
               A failed apply remains pending and is retried on the next
               participant/session or subscription update. */
            (void)turbo_sfu_node_apply_track_subscription(
                server->node, room_id, &config);
        }
    }

    turbo_mutex_lock(&server->mutex);
    sfu_node_webrtc_session_t *session =
        find_webrtc_session_by_participant_locked(server, room_id,
                                                  participant_id);
    if (session) {
        sync_session_relay_tracks_locked(server, session);
    }
    turbo_mutex_unlock(&server->mutex);
    return 0;
}

static void on_sfu_keyframe_request(void *user_data, uint32_t ssrc) {
    sfu_node_webrtc_session_t *session = (sfu_node_webrtc_session_t *)user_data;
    turbo_media_context_t *media_ctx;

    if (!session || !session->pc) {
        return;
    }

    media_ctx = turbo_peer_connection_get_media_context(session->pc);
    if (!media_ctx) {
        return;
    }

    for (int i = 0; i < turbo_media_get_track_count(media_ctx); ++i) {
        turbo_media_track_t *track = turbo_media_get_track(media_ctx, i);
        if (!track ||
            turbo_media_track_get_type(track) != TURBO_RTC_MEDIA_TRACK_VIDEO ||
            !(turbo_media_track_get_direction(track) & TURBO_MEDIA_DIRECTION_RECVONLY)) {
            continue;
        }
        if (ssrc == 0 || turbo_media_track_get_remote_ssrc(track) == ssrc) {
            turbo_media_track_request_keyframe(track);
            return;
        }
    }
}

static void on_sfu_packet(void *user_data, const uint8_t *packet, size_t len) {
    sfu_node_webrtc_session_t *session = (sfu_node_webrtc_session_t *)user_data;
    rtp_packet_t incoming;
    sfu_node_relay_track_t *relay_track;

    if (!session || !packet || len == 0) {
        return;
    }

    memset(&incoming, 0, sizeof(incoming));
    if (rtp_packet_parse(&incoming, packet, len) != 0) {
        return;
    }

    turbo_mutex_lock(&session->mutex);
    relay_track = find_relay_track_by_source_ssrc_locked(session, incoming.header.ssrc);
    if (!relay_track || !relay_track->track) {
        turbo_mutex_unlock(&session->mutex);
        return;
    }

    turbo_media_track_send_rtp_packet(relay_track->track, packet, len);
    turbo_mutex_unlock(&session->mutex);
}

static void on_session_rtp_packet(turbo_media_track_t *track, const uint8_t *packet,
                                  size_t len, void *user_data) {
    sfu_node_webrtc_session_t *session = (sfu_node_webrtc_session_t *)user_data;
    sfu_node_room_recording_t *recording;
    sfu_node_recording_track_t *recording_track;
    rtp_packet_t incoming;
    (void)track;

    if (!session || !session->server || !session->server->node || !packet || len == 0) {
        return;
    }

    memset(&incoming, 0, sizeof(incoming));
    if (rtp_packet_parse(&incoming, packet, len) == 0 && incoming.payload &&
        incoming.payload_len > 0) {
        turbo_mutex_lock(&session->server->recording_mutex);
        recording = find_room_recording_locked(session->server, session->room_id);
        if (recording && recording->active) {
            recording_track = find_recording_track_by_ssrc_locked(
                recording, session->participant_id, incoming.header.ssrc);
            if (recording_track && recording_track->rtp_ctx &&
                turbo_recorder_write_rtp_packet(recording_track->rtp_ctx,
                                                packet, len) == 0) {
                recording_track->packet_count++;
                recording->packet_count++;
                refresh_room_recording_stats_locked(recording);
            }
        }
        turbo_mutex_unlock(&session->server->recording_mutex);
    }

    turbo_sfu_node_forward_packet(session->server->node, session->room_id,
                                  session->participant_id, packet, len);
}

static void destroy_webrtc_session(sfu_node_app_server_t *server,
                                   sfu_node_webrtc_session_t *session) {
    int i;

    if (!session) {
        return;
    }

    if (server && server->node) {
        turbo_sfu_node_set_participant_packet_callback(server->node, session->room_id,
                                                       session->participant_id, NULL, NULL);
        turbo_sfu_node_set_participant_keyframe_callback(server->node, session->room_id,
                                                         session->participant_id, NULL, NULL);
        turbo_sfu_node_bind_session_pc(server->node, session->room_id, session->participant_id,
                                       session->session_id, NULL);
        if (session->owns_node_session) {
            turbo_sfu_node_remove_session(
                server->node, session->room_id, session->session_id);
        }
    }

    if (session->pc) {
        turbo_peer_connection_destroy(session->pc);
        session->pc = NULL;
    }

#ifdef ENABLE_RTC_SCXML_WORKFLOW
    if (session->workflow_ctx) {
        sfu_node_workflow_destroy(session->workflow_ctx);
        session->workflow_ctx = NULL;
    }
#endif

    for (i = 0; i < session->local_candidate_count; ++i) {
        free(session->local_candidates[i]);
    }
    free(session->local_candidates);
    session->local_candidates = NULL;
    session->local_candidate_count = 0;
    session->local_candidate_capacity = 0;

    free(session->local_answer);
    session->local_answer = NULL;
    free(session->relay_tracks);
    session->relay_tracks = NULL;
    session->relay_track_count = 0;
    session->relay_track_capacity = 0;

    turbo_mutex_destroy(&session->mutex);
    free(session);
}

static void on_session_frame(turbo_media_track_t *track, const uint8_t *data,
                             size_t len, uint64_t timestamp, void *user_data) {
    sfu_node_webrtc_session_t *session = (sfu_node_webrtc_session_t *)user_data;
    (void)track;
    (void)data;
    (void)len;
    (void)timestamp;

    if (!session) {
        return;
    }

    turbo_mutex_lock(&session->mutex);
    session->remote_frame_count++;
    turbo_mutex_unlock(&session->mutex);
}

static void on_session_state_change(turbo_peer_connection_t *pc,
                                    turbo_peer_state_t state, void *user_data) {
    sfu_node_webrtc_session_t *session = (sfu_node_webrtc_session_t *)user_data;
    (void)pc;

    if (!session) {
        return;
    }

    turbo_mutex_lock(&session->mutex);
    session->state = state;
    turbo_mutex_unlock(&session->mutex);

#ifdef ENABLE_RTC_SCXML_WORKFLOW
    if (session->workflow_ctx) {
        const char *event_name = NULL;
        switch (state) {
            case TURBO_PEER_STATE_CONNECTED: event_name = "rtc.peer.connected"; break;
            case TURBO_PEER_STATE_DISCONNECTED: event_name = "rtc.peer.disconnected"; break;
            case TURBO_PEER_STATE_FAILED: event_name = "rtc.peer.failed"; break;
            case TURBO_PEER_STATE_CLOSED: event_name = "rtc.session.closed"; break;
            default: break;
        }
        if (event_name) {
            sfu_node_workflow_receive(session->workflow_ctx, event_name, NULL, 0);
        }
    }
#endif
}

static void on_session_track(turbo_peer_connection_t *pc, turbo_media_track_t *track,
                             void *user_data) {
    sfu_node_webrtc_session_t *session = (sfu_node_webrtc_session_t *)user_data;
    (void)pc;

    if (!session || !track) {
        return;
    }

    turbo_media_track_set_user_data(track, session);
    turbo_media_track_on_frame(track, on_session_frame);
    turbo_media_track_on_rtp_packet(track, on_session_rtp_packet);

    turbo_mutex_lock(&session->mutex);
    session->remote_track_count++;
    turbo_mutex_unlock(&session->mutex);
}

static void on_session_ice_candidate(turbo_peer_connection_t *pc,
                                     const char *candidate, void *user_data) {
    sfu_node_webrtc_session_t *session = (sfu_node_webrtc_session_t *)user_data;
    int i;

    (void)pc;
    if (!session || !candidate || candidate[0] == '\0') {
        return;
    }

    turbo_mutex_lock(&session->mutex);
    for (i = 0; i < session->local_candidate_count; ++i) {
        if (strcmp(session->local_candidates[i], candidate) == 0) {
            turbo_mutex_unlock(&session->mutex);
            return;
        }
    }

    if (ensure_capacity((void **)&session->local_candidates,
                        &session->local_candidate_capacity,
                        sizeof(char *),
                        session->local_candidate_count + 1) == 0) {
        session->local_candidates[session->local_candidate_count] = sfu_strdup(candidate);
        if (session->local_candidates[session->local_candidate_count]) {
            session->local_candidate_count++;
        }
    }
    turbo_mutex_unlock(&session->mutex);
}

static void sfu_node_webrtc_thread(void *arg) {
    sfu_node_app_server_t *server = (sfu_node_app_server_t *)arg;

    if (!server) {
        return;
    }

    while (server->webrtc_running) {
        for (;;) {
            sfu_node_webrtc_command_t *command;

            turbo_mutex_lock(&server->webrtc_command_mutex);
            command = server->webrtc_command_head;
            if (command) {
                server->webrtc_command_head = command->next;
                if (!server->webrtc_command_head) {
                    server->webrtc_command_tail = NULL;
                }
            }
            turbo_mutex_unlock(&server->webrtc_command_mutex);
            if (!command) {
                break;
            }

            switch (command->type) {
                case SFU_NODE_WEBRTC_COMMAND_CREATE:
                    command->result = create_webrtc_session_impl(
                        server, command->room_id, command->participant_id,
                        command->session_id);
                    break;
                case SFU_NODE_WEBRTC_COMMAND_CREATE_OWNED:
                    command->result = create_owned_media_session_impl(
                        server, command->room_id, command->participant_id,
                        command->session_id);
                    break;
                case SFU_NODE_WEBRTC_COMMAND_REMOVE:
                    command->result = remove_webrtc_session_impl(
                        server, command->room_id, command->session_id);
                    break;
                case SFU_NODE_WEBRTC_COMMAND_DISCONNECT_MEDIA_PARTICIPANT:
                    command->result = disconnect_media_participant_impl(
                        server, command->room_id, command->participant_id);
                    break;
                case SFU_NODE_WEBRTC_COMMAND_SET_OFFER:
                    command->result = set_remote_offer_impl(
                        server, command->room_id, command->session_id,
                        command->text);
                    break;
                case SFU_NODE_WEBRTC_COMMAND_ADD_CANDIDATE:
                    command->result = add_remote_ice_candidate_impl(
                        server, command->room_id, command->session_id,
                        command->text);
                    break;
                case SFU_NODE_WEBRTC_COMMAND_APPLY_SDPFRAG:
                    command->result = apply_remote_ice_sdpfrag_impl(
                        server, command->room_id, command->participant_id,
                        command->session_id, command->expected_version,
                        command->text, command->text_len, command->output,
                        command->output_capacity, command->version_out);
                    break;
                case SFU_NODE_WEBRTC_COMMAND_CLEAR:
                    turbo_mutex_lock(&server->mutex);
                    while (server->webrtc_session_count > 0) {
                        int last = server->webrtc_session_count - 1;
                        destroy_webrtc_session(
                            server, server->webrtc_sessions[last]);
                        server->webrtc_session_count--;
                    }
                    turbo_mutex_unlock(&server->mutex);
                    command->result = 0;
                    break;
                default:
                    command->result = -1;
                    break;
            }

            turbo_mutex_lock(&server->webrtc_command_mutex);
            command->done = 1;
            turbo_cond_broadcast(&server->webrtc_command_cond);
            turbo_mutex_unlock(&server->webrtc_command_mutex);
        }
        sfu_node_app_server_poll_webrtc(server);
        sfu_node_sleep_ms(5);
    }
}

static int submit_webrtc_command(sfu_node_app_server_t *server,
                                 sfu_node_webrtc_command_t *command) {
    if (!server || !command || !server->webrtc_running ||
        !server->webrtc_thread_started) {
        return -1;
    }
    command->done = 0;
    command->next = NULL;

    turbo_mutex_lock(&server->webrtc_command_mutex);
    if (server->webrtc_command_tail) {
        server->webrtc_command_tail->next = command;
    } else {
        server->webrtc_command_head = command;
    }
    server->webrtc_command_tail = command;
    while (!command->done && server->webrtc_running) {
        turbo_cond_wait(
            &server->webrtc_command_cond, &server->webrtc_command_mutex);
    }
    turbo_mutex_unlock(&server->webrtc_command_mutex);
    return command->done ? command->result : -1;
}

void sfu_node_app_server_poll_webrtc(sfu_node_app_server_t *server) {
    if (!server) {
        return;
    }

    turbo_mutex_lock(&server->mutex);
    for (int i = 0; i < server->webrtc_session_count; ++i) {
        sfu_node_webrtc_session_t *session = server->webrtc_sessions[i];

        if (!session->pc) {
            continue;
        }

        turbo_peer_connection_poll(session->pc);
    }
    turbo_mutex_unlock(&server->mutex);
}

sfu_node_app_server_t *sfu_node_app_server_create(const sfu_node_app_config_t *config) {
    sfu_node_app_server_t *server;
    turbo_sfu_node_config_t node_config;

    if (!config || sfu_node_app_config_validate(config) != 0) {
        return NULL;
    }

    server = (sfu_node_app_server_t *)calloc(1, sizeof(*server));
    if (!server) {
        return NULL;
    }

    if (sfu_node_app_config_copy(&server->config, config) != 0) {
        free(server);
        return NULL;
    }
    memset(&node_config, 0, sizeof(node_config));
    node_config.node_id = server->config.node_id;
    node_config.max_rooms = server->config.max_rooms;
    node_config.default_room_capacity = server->config.default_room_capacity;
    server->node = turbo_sfu_node_create(&node_config);
    if (!server->node) {
        sfu_node_app_config_cleanup(&server->config);
        free(server);
        return NULL;
    }

    turbo_mutex_init(&server->mutex);
    turbo_mutex_init(&server->recording_mutex);
    turbo_mutex_init(&server->webrtc_command_mutex);
    turbo_cond_init(&server->webrtc_command_cond);

    server->http_api = sfu_node_http_api_create(server);
    if (!server->http_api) {
        turbo_cond_destroy(&server->webrtc_command_cond);
        turbo_mutex_destroy(&server->webrtc_command_mutex);
        turbo_mutex_destroy(&server->recording_mutex);
        turbo_mutex_destroy(&server->mutex);
        turbo_sfu_node_destroy(server->node);
        sfu_node_app_config_cleanup(&server->config);
        free(server);
        return NULL;
    }

    return server;
}

int sfu_node_app_server_start(sfu_node_app_server_t *server) {
    if (!server || !server->node) {
        return -1;
    }

    if (sfu_node_app_server_ensure_webrtc_worker(server) != 0) {
        return -1;
    }
    if (server->http_api &&
        sfu_node_http_api_start(server->http_api, server->config.bind_host,
                                server->config.bind_port) != 0) {
        server->webrtc_running = 0;
        turbo_mutex_lock(&server->webrtc_command_mutex);
        turbo_cond_broadcast(&server->webrtc_command_cond);
        turbo_mutex_unlock(&server->webrtc_command_mutex);
        turbo_thread_join(&server->webrtc_thread);
        server->webrtc_thread_started = 0;
        return -1;
    }

    server->running = 1;
    return 0;
}

int sfu_node_app_server_ensure_webrtc_worker(sfu_node_app_server_t *server) {
    if (!server || !server->node) {
        return -1;
    }
    if (server->webrtc_thread_started) {
        return 0;
    }
    server->webrtc_running = 1;
    if (turbo_thread_create(&server->webrtc_thread, sfu_node_webrtc_thread, server) != 0) {
        server->webrtc_running = 0;
        return -1;
    }
    server->webrtc_thread_started = 1;
    return 0;
}

int sfu_node_app_server_run(sfu_node_app_server_t *server) {
    if (!server || !server->running) {
        return -1;
    }

    if (server->config.dry_run) {
        printf("sfu_node: dry-run complete\n");
        return 0;
    }

    printf("sfu_node: running on %s:%d with node_id=%s\n",
           server->config.bind_host,
           server->config.bind_port,
           server->config.node_id);

    while (server->running) {
        sfu_node_sleep_ms(100);
    }

    return 0;
}

void sfu_node_app_server_stop(sfu_node_app_server_t *server) {
    if (!server) {
        return;
    }

    server->running = 0;
    if (server->http_api) {
        sfu_node_http_api_stop(server->http_api);
    }
    if (server->webrtc_thread_started && server->webrtc_running) {
        sfu_node_webrtc_command_t clear_command = {
            .type = SFU_NODE_WEBRTC_COMMAND_CLEAR
        };
        submit_webrtc_command(server, &clear_command);
    }
    server->webrtc_running = 0;
    turbo_mutex_lock(&server->webrtc_command_mutex);
    turbo_cond_broadcast(&server->webrtc_command_cond);
    turbo_mutex_unlock(&server->webrtc_command_mutex);
    if (server->webrtc_thread_started) {
        turbo_thread_join(&server->webrtc_thread);
        server->webrtc_thread_started = 0;
    }
}

void sfu_node_app_server_destroy(sfu_node_app_server_t *server) {
    int i;

    if (!server) {
        return;
    }

    if (server->webrtc_thread_started || server->running) {
        sfu_node_app_server_stop(server);
    }

    if (server->http_api) {
        sfu_node_http_api_destroy(server->http_api);
    }

    for (i = 0; i < server->webrtc_session_count; ++i) {
        destroy_webrtc_session(server, server->webrtc_sessions[i]);
    }
    free(server->webrtc_sessions);
    free(server->published_tracks);
    free(server->subscriptions);

    turbo_mutex_lock(&server->recording_mutex);
    for (i = 0; i < server->room_recording_count; ++i) {
        destroy_room_recording_locked(&server->room_recordings[i]);
    }
    free(server->room_recordings);
    turbo_mutex_unlock(&server->recording_mutex);
    turbo_mutex_destroy(&server->recording_mutex);
    turbo_mutex_destroy(&server->mutex);
    turbo_cond_destroy(&server->webrtc_command_cond);
    turbo_mutex_destroy(&server->webrtc_command_mutex);

    if (server->node) {
        turbo_sfu_node_destroy(server->node);
    }
    sfu_node_app_config_cleanup(&server->config);
    free(server);
}

turbo_sfu_node_t *sfu_node_app_server_get_node(sfu_node_app_server_t *server) {
    return server ? server->node : NULL;
}

const sfu_node_app_config_t *sfu_node_app_server_get_config(sfu_node_app_server_t *server) {
    return server ? &server->config : NULL;
}

int sfu_node_app_server_set_draining(sfu_node_app_server_t *server, int draining) {
    if (!server) {
        return -1;
    }

    turbo_mutex_lock(&server->mutex);
    server->draining = draining ? 1 : 0;
    turbo_mutex_unlock(&server->mutex);
    return 0;
}

int sfu_node_app_server_is_draining(sfu_node_app_server_t *server) {
    int draining;

    if (!server) {
        return 0;
    }

    turbo_mutex_lock(&server->mutex);
    draining = server->draining;
    turbo_mutex_unlock(&server->mutex);
    return draining;
}

static int create_webrtc_session_impl(sfu_node_app_server_t *server,
                                      const char *room_id,
                                      const char *participant_id,
                                      const char *session_id) {
    sfu_node_webrtc_session_t *session;
    turbo_peer_config_t peer_config;
    turbo_peer_callbacks_t callbacks;
    turbo_sfu_node_room_stats_t room_stats;
    turbo_sfu_node_participant_stats_t participant_stats;

    if (!server || !room_id || !participant_id || !session_id) {
        return -1;
    }

    memset(&room_stats, 0, sizeof(room_stats));
    if (turbo_sfu_node_get_room_stats(server->node, room_id, &room_stats) != 0) {
        return -1;
    }

    memset(&participant_stats, 0, sizeof(participant_stats));
    if (turbo_sfu_node_get_participant_stats(server->node, room_id, participant_id,
                                             &participant_stats) != 0) {
        return -1;
    }

    session = (sfu_node_webrtc_session_t *)calloc(1, sizeof(*session));
    if (!session) {
        return -1;
    }
    session->server = server;
    copy_string(session->room_id, sizeof(session->room_id), room_id);
    copy_string(session->participant_id, sizeof(session->participant_id), participant_id);
    copy_string(session->session_id, sizeof(session->session_id), session_id);
    session->state = TURBO_PEER_STATE_NEW;
    session->resource_version = 1;
    turbo_mutex_init(&session->mutex);

    memset(&peer_config, 0, sizeof(peer_config));
    peer_config.stun_servers = (const char **)server->config.stun_servers;
    peer_config.stun_server_count = server->config.stun_server_count;
    peer_config.turn_servers = (const char **)server->config.turn_servers;
    peer_config.turn_server_count = server->config.turn_server_count;
    peer_config.allow_loopback = server->config.ice_allow_loopback;
    peer_config.disable_datachannel = 1;
    peer_config.user_data = session;

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.on_state_change = on_session_state_change;
    callbacks.on_track = on_session_track;
    callbacks.on_ice_candidate = on_session_ice_candidate;

    session->pc = turbo_peer_connection_create(&peer_config, &callbacks);
    if (!session->pc) {
        turbo_mutex_destroy(&session->mutex);
        free(session);
        return -1;
    }

    turbo_mutex_lock(&server->mutex);
    if (find_webrtc_session_locked(server, room_id, session_id) ||
        find_webrtc_session_by_participant_locked(server, room_id, participant_id) ||
        ensure_capacity((void **)&server->webrtc_sessions, &server->webrtc_session_capacity,
                        sizeof(sfu_node_webrtc_session_t *),
                        server->webrtc_session_count + 1) != 0) {
        turbo_mutex_unlock(&server->mutex);
        destroy_webrtc_session(server, session);
        return -1;
    }

    if (turbo_sfu_node_bind_session_pc(server->node, room_id, participant_id, session_id,
                                       session->pc) != 0) {
        turbo_mutex_unlock(&server->mutex);
        destroy_webrtc_session(server, session);
        return -1;
    }

    if (turbo_sfu_node_set_participant_keyframe_callback(server->node, room_id, participant_id,
                                                         on_sfu_keyframe_request, session) != 0) {
        turbo_sfu_node_bind_session_pc(server->node, room_id, participant_id, session_id, NULL);
        turbo_mutex_unlock(&server->mutex);
        destroy_webrtc_session(server, session);
        return -1;
    }

    if (turbo_sfu_node_set_participant_packet_callback(server->node, room_id, participant_id,
                                                       on_sfu_packet, session) != 0) {
        turbo_sfu_node_set_participant_keyframe_callback(server->node, room_id, participant_id,
                                                         NULL, NULL);
        turbo_sfu_node_bind_session_pc(server->node, room_id, participant_id, session_id, NULL);
        turbo_mutex_unlock(&server->mutex);
        destroy_webrtc_session(server, session);
        return -1;
    }

    sync_session_relay_tracks_locked(server, session);

#ifdef ENABLE_RTC_SCXML_WORKFLOW
    sfu_node_workflow_init(server, session, &session->workflow_ctx);
#endif

    server->webrtc_sessions[server->webrtc_session_count++] = session;
    turbo_mutex_unlock(&server->mutex);

    if (apply_desired_subscriptions_for_participant(
            server, room_id, participant_id) != 0) {
        remove_webrtc_session_impl(server, room_id, session_id);
        return -1;
    }

#ifdef ENABLE_RTC_SCXML_WORKFLOW
    if (session->workflow_ctx) {
        turbo_rtc_workflow_param_t params[] = {
            {"room_id", session->room_id},
            {"participant_id", session->participant_id},
            {"session_id", session->session_id}
        };
        sfu_node_workflow_receive(session->workflow_ctx, "rtc.session.create", params, 3);
    }
#endif
    return 0;
}

int sfu_node_app_server_create_webrtc_session(sfu_node_app_server_t *server,
                                              const char *room_id,
                                              const char *participant_id,
                                              const char *session_id) {
    sfu_node_webrtc_command_t command = {
        .type = SFU_NODE_WEBRTC_COMMAND_CREATE,
        .room_id = room_id,
        .participant_id = participant_id,
        .session_id = session_id
    };

    if (server && server->webrtc_thread_started) {
        return submit_webrtc_command(server, &command);
    }
    return create_webrtc_session_impl(
        server, room_id, participant_id, session_id);
}

static int create_owned_media_session_impl(
    sfu_node_app_server_t *server, const char *room_id, const char *participant_id,
    const char *session_id) {
    sfu_node_webrtc_session_t *session;

    if (!server || !server->node || !room_id || !participant_id || !session_id) {
        return -1;
    }
    if (turbo_sfu_node_add_session(
            server->node, room_id, participant_id, session_id, NULL) != 0) {
        return -1;
    }
    if (create_webrtc_session_impl(
            server, room_id, participant_id, session_id) != 0) {
        turbo_sfu_node_remove_session(server->node, room_id, session_id);
        return -1;
    }

    turbo_mutex_lock(&server->mutex);
    session = find_webrtc_session_locked(server, room_id, session_id);
    if (!session) {
        turbo_mutex_unlock(&server->mutex);
        remove_webrtc_session_impl(server, room_id, session_id);
        turbo_sfu_node_remove_session(server->node, room_id, session_id);
        return -1;
    }
    session->owns_node_session = 1;
    turbo_mutex_unlock(&server->mutex);
    return 0;
}

int sfu_node_app_server_create_owned_media_session(
    sfu_node_app_server_t *server, const char *room_id, const char *participant_id,
    const char *session_id) {
    sfu_node_webrtc_command_t command = {
        .type = SFU_NODE_WEBRTC_COMMAND_CREATE_OWNED,
        .room_id = room_id,
        .participant_id = participant_id,
        .session_id = session_id
    };

    if (server && server->webrtc_thread_started) {
        return submit_webrtc_command(server, &command);
    }
    return create_owned_media_session_impl(
        server, room_id, participant_id, session_id);
}

static int remove_webrtc_session_impl(sfu_node_app_server_t *server,
                                      const char *room_id,
                                      const char *session_id) {
    int i;

    if (!server || !room_id || !session_id) {
        return -1;
    }

    turbo_mutex_lock(&server->mutex);
    for (i = 0; i < server->webrtc_session_count; ++i) {
        sfu_node_webrtc_session_t *session = server->webrtc_sessions[i];
        if (strcmp(session->room_id, room_id) == 0 &&
            strcmp(session->session_id, session_id) == 0) {
            destroy_webrtc_session(server, session);
            if (i + 1 < server->webrtc_session_count) {
                memmove(&server->webrtc_sessions[i], &server->webrtc_sessions[i + 1],
                        (size_t)(server->webrtc_session_count - i - 1) *
                            sizeof(sfu_node_webrtc_session_t *));
            }
            server->webrtc_session_count--;
            turbo_mutex_unlock(&server->mutex);
            return 0;
        }
    }
    turbo_mutex_unlock(&server->mutex);
    return -1;
}

int sfu_node_app_server_remove_webrtc_session(sfu_node_app_server_t *server,
                                              const char *room_id,
                                              const char *session_id) {
    sfu_node_webrtc_command_t command = {
        .type = SFU_NODE_WEBRTC_COMMAND_REMOVE,
        .room_id = room_id,
        .session_id = session_id
    };

    if (server && server->webrtc_thread_started) {
        return submit_webrtc_command(server, &command);
    }
    return remove_webrtc_session_impl(server, room_id, session_id);
}

static int disconnect_media_participant_impl(
    sfu_node_app_server_t *server, const char *room_id,
    const char *participant_id) {
    int i;

    if (!server || !room_id || !participant_id) {
        return -1;
    }

    turbo_mutex_lock(&server->mutex);
    for (i = 0; i < server->webrtc_session_count; ++i) {
        sfu_node_webrtc_session_t *session = server->webrtc_sessions[i];
        if (strcmp(session->room_id, room_id) == 0 &&
            strcmp(session->participant_id, participant_id) == 0) {
            if (!session->owns_node_session) {
                turbo_mutex_unlock(&server->mutex);
                return -1;
            }
            destroy_webrtc_session(server, session);
            if (i + 1 < server->webrtc_session_count) {
                memmove(&server->webrtc_sessions[i],
                        &server->webrtc_sessions[i + 1],
                        (size_t)(server->webrtc_session_count - i - 1) *
                            sizeof(sfu_node_webrtc_session_t *));
            }
            server->webrtc_session_count--;
            turbo_mutex_unlock(&server->mutex);
            return 0;
        }
    }
    turbo_mutex_unlock(&server->mutex);
    return -1;
}

int sfu_node_app_server_disconnect_media_participant(
    sfu_node_app_server_t *server, const char *room_id,
    const char *participant_id) {
    sfu_node_webrtc_command_t command = {
        .type = SFU_NODE_WEBRTC_COMMAND_DISCONNECT_MEDIA_PARTICIPANT,
        .room_id = room_id,
        .participant_id = participant_id
    };

    if (server && server->webrtc_thread_started) {
        return submit_webrtc_command(server, &command);
    }
    return disconnect_media_participant_impl(server, room_id, participant_id);
}

int sfu_node_app_server_remove_room_webrtc_sessions(sfu_node_app_server_t *server,
                                                    const char *room_id) {
    int removed = 0;

    if (!server || !room_id) {
        return -1;
    }

    turbo_mutex_lock(&server->mutex);
    for (int i = server->webrtc_session_count - 1; i >= 0; --i) {
        sfu_node_webrtc_session_t *session = server->webrtc_sessions[i];
        if (strcmp(session->room_id, room_id) != 0) {
            continue;
        }

        destroy_webrtc_session(server, session);
        if (i + 1 < server->webrtc_session_count) {
            memmove(&server->webrtc_sessions[i], &server->webrtc_sessions[i + 1],
                    (size_t)(server->webrtc_session_count - i - 1) *
                        sizeof(sfu_node_webrtc_session_t *));
        }
        server->webrtc_session_count--;
        removed++;
    }
    turbo_mutex_unlock(&server->mutex);
    return removed;
}

static int set_remote_offer_impl(sfu_node_app_server_t *server,
                                 const char *room_id,
                                 const char *session_id,
                                 const char *sdp) {
    sfu_node_webrtc_session_t *session;
    char answer[SFU_NODE_LOCAL_SDP_CAPACITY];
    char *stored_answer;

    if (!server || !room_id || !session_id || !sdp) {
        return -1;
    }

    turbo_mutex_lock(&server->mutex);
    session = find_webrtc_session_locked(server, room_id, session_id);
    if (!session || !session->pc) {
        turbo_mutex_unlock(&server->mutex);
        return -1;
    }

    if (turbo_peer_connection_set_remote_description(session->pc, "offer", sdp) != 0) {
        turbo_mutex_unlock(&server->mutex);
        return -1;
    }

    sync_session_relay_tracks_locked(server, session);
    if (turbo_peer_connection_create_answer(session->pc, answer, sizeof(answer)) <= 0) {
        turbo_mutex_unlock(&server->mutex);
        return -1;
    }

    stored_answer = sfu_strdup(answer);
    if (!stored_answer) {
        turbo_mutex_unlock(&server->mutex);
        return -1;
    }

    turbo_mutex_lock(&session->mutex);
    free(session->local_answer);
    session->local_answer = stored_answer;
    session->remote_description_set = 1;
    turbo_mutex_unlock(&session->mutex);

    turbo_mutex_unlock(&server->mutex);

#ifdef ENABLE_RTC_SCXML_WORKFLOW
    if (session->workflow_ctx) {
        turbo_rtc_workflow_param_t params[] = {
            {"sdp", sdp}
        };
        sfu_node_workflow_receive(session->workflow_ctx, "rtc.offer.received", params, 1);
    }
#endif

    return 0;
}

int sfu_node_app_server_set_remote_offer(sfu_node_app_server_t *server,
                                         const char *room_id,
                                         const char *session_id,
                                         const char *sdp) {
    sfu_node_webrtc_command_t command = {
        .type = SFU_NODE_WEBRTC_COMMAND_SET_OFFER,
        .room_id = room_id,
        .session_id = session_id,
        .text = sdp
    };

    if (server && server->webrtc_thread_started) {
        return submit_webrtc_command(server, &command);
    }
    return set_remote_offer_impl(server, room_id, session_id, sdp);
}

static int add_remote_ice_candidate_impl(sfu_node_app_server_t *server,
                                         const char *room_id,
                                         const char *session_id,
                                         const char *candidate) {
    sfu_node_webrtc_session_t *session;
    int rc;

    if (!server || !room_id || !session_id || !candidate) {
        return -1;
    }

    turbo_mutex_lock(&server->mutex);
    session = find_webrtc_session_locked(server, room_id, session_id);
    if (!session || !session->pc) {
        turbo_mutex_unlock(&server->mutex);
        return -1;
    }

    rc = turbo_peer_connection_add_ice_candidate(session->pc, candidate);

    turbo_mutex_unlock(&server->mutex);

#ifdef ENABLE_RTC_SCXML_WORKFLOW
    if (rc == 0 && session->workflow_ctx) {
        turbo_rtc_workflow_param_t params[] = {
            {"candidate", candidate}
        };
        sfu_node_workflow_receive(session->workflow_ctx, "rtc.ice.candidate.received", params, 1);
    }
#endif

    return rc;
}

int sfu_node_app_server_add_remote_ice_candidate(sfu_node_app_server_t *server,
                                                 const char *room_id,
                                                 const char *session_id,
                                                 const char *candidate) {
    sfu_node_webrtc_command_t command = {
        .type = SFU_NODE_WEBRTC_COMMAND_ADD_CANDIDATE,
        .room_id = room_id,
        .session_id = session_id,
        .text = candidate
    };

    if (server && server->webrtc_thread_started) {
        return submit_webrtc_command(server, &command);
    }
    return add_remote_ice_candidate_impl(
        server, room_id, session_id, candidate);
}

char *sfu_node_app_server_copy_local_answer(sfu_node_app_server_t *server,
                                            const char *room_id,
                                            const char *participant_id,
                                            const char *session_id) {
    sfu_node_webrtc_session_t *session;
    char *answer = NULL;

    if (!server || !room_id || !participant_id || !session_id) {
        return NULL;
    }
    turbo_mutex_lock(&server->mutex);
    session = find_webrtc_session_locked(server, room_id, session_id);
    if (session && strcmp(session->participant_id, participant_id) == 0) {
        turbo_mutex_lock(&session->mutex);
        if (session->local_answer) {
            answer = sfu_strdup(session->local_answer);
        }
        turbo_mutex_unlock(&session->mutex);
    }
    turbo_mutex_unlock(&server->mutex);
    return answer;
}

int sfu_node_app_server_get_media_session_version(
    sfu_node_app_server_t *server, const char *room_id, const char *participant_id,
    const char *session_id, uint64_t *version_out) {
    sfu_node_webrtc_session_t *session;

    if (!server || !room_id || !participant_id || !session_id || !version_out) {
        return -1;
    }
    turbo_mutex_lock(&server->mutex);
    session = find_webrtc_session_locked(server, room_id, session_id);
    if (!session || strcmp(session->participant_id, participant_id) != 0) {
        turbo_mutex_unlock(&server->mutex);
        return SFU_NODE_MEDIA_SESSION_NOT_FOUND;
    }
    turbo_mutex_lock(&session->mutex);
    *version_out = session->resource_version;
    turbo_mutex_unlock(&session->mutex);
    turbo_mutex_unlock(&server->mutex);
    return 0;
}

static int apply_remote_ice_sdpfrag_impl(
    sfu_node_app_server_t *server, const char *room_id, const char *participant_id,
    const char *session_id, uint64_t expected_version, const char *sdpfrag,
    size_t sdpfrag_len, char *local_sdpfrag_out,
    size_t local_sdpfrag_capacity, uint64_t *version_out) {
    sfu_node_webrtc_session_t *session;
    int result;

    if (!server || !room_id || !participant_id || !session_id || !sdpfrag ||
        sdpfrag_len == 0 || !version_out) {
        return -1;
    }
    turbo_mutex_lock(&server->mutex);
    session = find_webrtc_session_locked(server, room_id, session_id);
    if (!session || !session->pc ||
        strcmp(session->participant_id, participant_id) != 0) {
        turbo_mutex_unlock(&server->mutex);
        return SFU_NODE_MEDIA_SESSION_NOT_FOUND;
    }
    turbo_mutex_lock(&session->mutex);
    if (session->resource_version != expected_version) {
        turbo_mutex_unlock(&session->mutex);
        turbo_mutex_unlock(&server->mutex);
        return SFU_NODE_MEDIA_SESSION_PRECONDITION_FAILED;
    }
    turbo_mutex_unlock(&session->mutex);

    result = turbo_peer_connection_apply_remote_ice_sdpfrag(
        session->pc, sdpfrag, sdpfrag_len);
    if (result < 0) {
        turbo_mutex_unlock(&server->mutex);
        return -1;
    }
    if (result > 0) {
        if (!local_sdpfrag_out || local_sdpfrag_capacity == 0 ||
            turbo_peer_connection_create_local_ice_sdpfrag(
                session->pc, local_sdpfrag_out,
                local_sdpfrag_capacity) < 0) {
            turbo_mutex_unlock(&server->mutex);
            return -1;
        }
        turbo_mutex_lock(&session->mutex);
        session->resource_version++;
        *version_out = session->resource_version;
        turbo_mutex_unlock(&session->mutex);
    } else {
        turbo_mutex_lock(&session->mutex);
        *version_out = session->resource_version;
        turbo_mutex_unlock(&session->mutex);
    }
    turbo_mutex_unlock(&server->mutex);
    return result;
}

int sfu_node_app_server_apply_remote_ice_sdpfrag(
    sfu_node_app_server_t *server, const char *room_id, const char *participant_id,
    const char *session_id, uint64_t expected_version, const char *sdpfrag,
    size_t sdpfrag_len, char *local_sdpfrag_out,
    size_t local_sdpfrag_capacity, uint64_t *version_out) {
    sfu_node_webrtc_command_t command = {
        .type = SFU_NODE_WEBRTC_COMMAND_APPLY_SDPFRAG,
        .room_id = room_id,
        .participant_id = participant_id,
        .session_id = session_id,
        .text = sdpfrag,
        .text_len = sdpfrag_len,
        .expected_version = expected_version,
        .output = local_sdpfrag_out,
        .output_capacity = local_sdpfrag_capacity,
        .version_out = version_out
    };

    if (server && server->webrtc_thread_started) {
        return submit_webrtc_command(server, &command);
    }
    return apply_remote_ice_sdpfrag_impl(
        server, room_id, participant_id, session_id, expected_version,
        sdpfrag, sdpfrag_len, local_sdpfrag_out,
        local_sdpfrag_capacity, version_out);
}

int sfu_node_app_server_remove_media_session(
    sfu_node_app_server_t *server, const char *room_id, const char *participant_id,
    const char *session_id) {
    sfu_node_webrtc_session_t *session;

    if (!server || !room_id || !participant_id || !session_id) {
        return -1;
    }
    turbo_mutex_lock(&server->mutex);
    session = find_webrtc_session_locked(server, room_id, session_id);
    if (!session || strcmp(session->participant_id, participant_id) != 0) {
        turbo_mutex_unlock(&server->mutex);
        return SFU_NODE_MEDIA_SESSION_NOT_FOUND;
    }
    turbo_mutex_unlock(&server->mutex);
    return sfu_node_app_server_remove_webrtc_session(server, room_id, session_id);
}

int sfu_node_app_server_register_published_track(sfu_node_app_server_t *server,
                                                 const char *room_id,
                                                 const char *participant_id,
                                                 const char *track_id,
                                                 uint32_t main_ssrc,
                                                 const uint32_t *layer_ssrcs,
                                                 int layer_count,
                                                 turbo_room_track_kind_t kind,
                                                 const char *codec_name) {
    sfu_node_published_track_t *published_track;
    turbo_codec_type_t codec = codec_from_name(codec_name);
    int i;
#ifdef ENABLE_RTC_SCXML_WORKFLOW
    sfu_node_webrtc_session_t *session = NULL;
#endif

    if (!server || !room_id || !participant_id || !track_id || main_ssrc == 0) {
        return -1;
    }

    turbo_mutex_lock(&server->mutex);
    published_track = find_published_track_locked(server, room_id, track_id);
    if (!published_track) {
        if (ensure_capacity((void **)&server->published_tracks, &server->published_track_capacity,
                            sizeof(sfu_node_published_track_t),
                            server->published_track_count + 1) != 0) {
            turbo_mutex_unlock(&server->mutex);
            return -1;
        }
        published_track = &server->published_tracks[server->published_track_count++];
        memset(published_track, 0, sizeof(*published_track));
        copy_string(published_track->room_id, sizeof(published_track->room_id), room_id);
        copy_string(published_track->participant_id, sizeof(published_track->participant_id),
                    participant_id);
        copy_string(published_track->track_id, sizeof(published_track->track_id), track_id);
    }

    published_track->main_ssrc = main_ssrc;
    published_track->layer_count = (layer_count > 0 && layer_count <= 3) ? layer_count : 1;
    for (i = 0; i < published_track->layer_count; ++i) {
        published_track->layer_ssrcs[i] =
            (layer_ssrcs && layer_count > 0) ? layer_ssrcs[i] : main_ssrc;
    }

    if (codec != 0 && kind != 0) {
        published_track->kind = kind;
        published_track->codec = codec;
        published_track->payload_type = payload_type_for_codec(codec);
        published_track->metadata_ready = 1;
        sync_track_to_subscribers_locked(server, published_track);

#ifdef ENABLE_RTC_SCXML_WORKFLOW
        session = find_webrtc_session_by_participant_locked(
            server, published_track->room_id, published_track->participant_id);
#endif
    }

    turbo_mutex_unlock(&server->mutex);

#ifdef ENABLE_RTC_SCXML_WORKFLOW
    if (session && session->workflow_ctx) {
        turbo_rtc_workflow_param_t params[] = {
            {"track_id", track_id},
            {"kind", (kind == TURBO_ROOM_TRACK_AUDIO) ? "audio" : "video"},
            {"codec", codec_name}
        };
        sfu_node_workflow_receive(session->workflow_ctx, "rtc.track.published", params, 3);
    }
#endif

    return 0;
}

int sfu_node_app_server_unregister_published_track(sfu_node_app_server_t *server,
                                                   const char *room_id,
                                                   const char *track_id) {
    int i;

    if (!server || !room_id || !track_id) {
        return -1;
    }

    turbo_mutex_lock(&server->mutex);
    for (i = 0; i < server->published_track_count; ++i) {
        if (strcmp(server->published_tracks[i].room_id, room_id) != 0 ||
            strcmp(server->published_tracks[i].track_id, track_id) != 0) {
            continue;
        }

#ifdef ENABLE_RTC_SCXML_WORKFLOW
        sfu_node_webrtc_session_t *session = NULL;
        session = find_webrtc_session_by_participant_locked(
            server, room_id, server->published_tracks[i].participant_id);
#endif

        if (i + 1 < server->published_track_count) {
            memmove(&server->published_tracks[i], &server->published_tracks[i + 1],
                    (size_t)(server->published_track_count - i - 1) *
                        sizeof(sfu_node_published_track_t));
        }
        server->published_track_count--;

        turbo_mutex_unlock(&server->mutex);

#ifdef ENABLE_RTC_SCXML_WORKFLOW
        if (session && session->workflow_ctx) {
            turbo_rtc_workflow_param_t params[] = {
                {"track_id", track_id}
            };
            sfu_node_workflow_receive(session->workflow_ctx, "rtc.track.unpublished", params, 1);
        }
#endif
        return 0;
    }

    turbo_mutex_unlock(&server->mutex);
    return -1;
}

int sfu_node_app_server_set_track_subscription(sfu_node_app_server_t *server,
                                               const char *room_id,
                                               const char *receiver_participant_id,
                                               const char *track_id,
                                               int enabled,
                                               turbo_room_video_layer_t max_layer) {
    turbo_sfu_node_track_subscription_t subscription;

    memset(&subscription, 0, sizeof(subscription));
    copy_string(subscription.receiver_participant_id,
                sizeof(subscription.receiver_participant_id),
                receiver_participant_id);
    copy_string(subscription.track_id, sizeof(subscription.track_id), track_id);
    subscription.enabled = enabled ? 1 : 0;
    subscription.preferred_layer = max_layer;
    subscription.target_layer = max_layer;
    subscription.max_layer = max_layer;
    subscription.muted = enabled ? 0 : 1;
    return sfu_node_app_server_apply_track_subscription(server, room_id, &subscription);
}

int sfu_node_app_server_apply_track_subscription(
    sfu_node_app_server_t *server, const char *room_id,
    const turbo_sfu_node_track_subscription_t *subscription_config) {
    sfu_node_desired_subscription_t *subscription;
    int receiver_ready;

    if (!server || !room_id || !subscription_config ||
        !subscription_config->receiver_participant_id[0] ||
        !subscription_config->track_id[0]) {
        return -1;
    }

    turbo_mutex_lock(&server->mutex);
    subscription = find_subscription_locked(server, room_id,
                                            subscription_config->receiver_participant_id,
                                            subscription_config->track_id);
    if (!subscription) {
        if (ensure_capacity((void **)&server->subscriptions, &server->subscription_capacity,
                            sizeof(sfu_node_desired_subscription_t),
                            server->subscription_count + 1) != 0) {
            turbo_mutex_unlock(&server->mutex);
            return -1;
        }
        subscription = &server->subscriptions[server->subscription_count++];
        memset(subscription, 0, sizeof(*subscription));
        copy_string(subscription->room_id, sizeof(subscription->room_id), room_id);
        copy_string(subscription->receiver_participant_id,
                    sizeof(subscription->receiver_participant_id),
                    subscription_config->receiver_participant_id);
        copy_string(subscription->track_id, sizeof(subscription->track_id),
                    subscription_config->track_id);
    }

    subscription->enabled = subscription_config->enabled ? 1 : 0;
    subscription->priority = subscription_config->priority;
    subscription->preferred_layer = subscription_config->preferred_layer;
    subscription->target_layer = subscription_config->target_layer;
    subscription->muted = subscription_config->muted ? 1 : 0;
    copy_string(subscription->policy_source, sizeof(subscription->policy_source),
                subscription_config->policy_source);
    subscription->max_layer = subscription_config->max_layer;

    receiver_ready = find_webrtc_session_by_participant_locked(
                         server, room_id,
                         subscription->receiver_participant_id) != NULL;
    turbo_mutex_unlock(&server->mutex);
    /* This succeeds immediately when core sender/receiver state is ready;
       otherwise the desired state remains pending for WHEP session creation. */
    (void)turbo_sfu_node_apply_track_subscription(
        server->node, room_id, subscription_config);
    if (!receiver_ready) {
        return 0;
    }
    return apply_desired_subscriptions_for_participant(
        server, room_id, subscription_config->receiver_participant_id);
}

int sfu_node_app_server_start_recording(sfu_node_app_server_t *server,
                                        const char *room_id,
                                        const char *recording_id,
                                        const char *mode) {
    sfu_node_published_track_t *published_tracks = NULL;
    sfu_node_room_recording_t *recording;
    turbo_sfu_node_room_stats_t room_stats;
    turbo_recorder_config_t recorder_config;
    turbo_recorder_format_t format;
    const char *extension = NULL;
    int published_track_count = 0;
    int recordable_track_count = 0;
    int i;

    if (!server || !room_id || !recording_id || !mode) {
        return -1;
    }

    memset(&room_stats, 0, sizeof(room_stats));
    if (turbo_sfu_node_get_room_stats(server->node, room_id, &room_stats) != 0 ||
        recording_format_from_mode(mode, &format, &extension) != 0) {
        return -1;
    }

    turbo_mutex_lock(&server->mutex);
    for (i = 0; i < server->published_track_count; ++i) {
        const sfu_node_published_track_t *track = &server->published_tracks[i];

        if (strcmp(track->room_id, room_id) != 0 || !track->metadata_ready ||
            recorder_codec_from_track(track) == (turbo_recorder_codec_t)-1) {
            continue;
        }
        published_track_count++;
    }

    if (published_track_count > 0) {
        published_tracks = (sfu_node_published_track_t *)calloc(
            (size_t)published_track_count, sizeof(*published_tracks));
    }
    if (published_track_count > 0 && !published_tracks) {
        turbo_mutex_unlock(&server->mutex);
        return -1;
    }

    for (i = 0; i < server->published_track_count; ++i) {
        const sfu_node_published_track_t *track = &server->published_tracks[i];

        if (strcmp(track->room_id, room_id) != 0 || !track->metadata_ready ||
            recorder_codec_from_track(track) == (turbo_recorder_codec_t)-1) {
            continue;
        }
        memcpy(&published_tracks[recordable_track_count++], track, sizeof(*track));
    }
    turbo_mutex_unlock(&server->mutex);

    if (recordable_track_count == 0) {
        free(published_tracks);
        return -1;
    }

    turbo_mutex_lock(&server->recording_mutex);
    recording = find_room_recording_locked(server, room_id);
    if (recording && recording->active) {
        turbo_mutex_unlock(&server->recording_mutex);
        free(published_tracks);
        return -1;
    }
    if (recording) {
        remove_room_recording_locked(server, room_id);
    }

    if (ensure_capacity((void **)&server->room_recordings, &server->room_recording_capacity,
                        sizeof(sfu_node_room_recording_t),
                        server->room_recording_count + 1) != 0) {
        turbo_mutex_unlock(&server->recording_mutex);
        free(published_tracks);
        return -1;
    }

    recording = &server->room_recordings[server->room_recording_count++];
    memset(recording, 0, sizeof(*recording));
    copy_string(recording->room_id, sizeof(recording->room_id), room_id);
    copy_string(recording->recording_id, sizeof(recording->recording_id), recording_id);
    copy_string(recording->mode, sizeof(recording->mode), mode);
    build_recording_output_path(room_id, recording_id, extension,
                                recording->output_path, sizeof(recording->output_path));

    memset(&recorder_config, 0, sizeof(recorder_config));
    recorder_config.format = format;
    recorder_config.filename = recording->output_path;
    recorder_config.title = recording_id;
    recorder_config.comment = room_id;
    recording->recorder = turbo_recorder_create(&recorder_config);
    if (!recording->recorder) {
        remove_room_recording_locked(server, room_id);
        turbo_mutex_unlock(&server->recording_mutex);
        free(published_tracks);
        return -1;
    }

    for (i = 0; i < recordable_track_count; ++i) {
        turbo_recorder_track_config_t track_config;
        sfu_node_recording_track_t *recording_track;
        int recorder_track_id;
        int layer_index;

        if (ensure_capacity((void **)&recording->tracks, &recording->track_capacity,
                            sizeof(sfu_node_recording_track_t),
                            recording->track_count + 1) != 0) {
            remove_room_recording_locked(server, room_id);
            turbo_mutex_unlock(&server->recording_mutex);
            free(published_tracks);
            return -1;
        }

        fill_recorder_track_config(&published_tracks[i], &track_config);
        recorder_track_id = turbo_recorder_add_track(recording->recorder, &track_config);
        if (recorder_track_id < 0) {
            remove_room_recording_locked(server, room_id);
            turbo_mutex_unlock(&server->recording_mutex);
            free(published_tracks);
            return -1;
        }

        recording_track = &recording->tracks[recording->track_count++];
        memset(recording_track, 0, sizeof(*recording_track));
        copy_string(recording_track->participant_id, sizeof(recording_track->participant_id),
                    published_tracks[i].participant_id);
        copy_string(recording_track->track_id, sizeof(recording_track->track_id),
                    published_tracks[i].track_id);
        recording_track->main_ssrc = published_tracks[i].main_ssrc;
        recording_track->layer_count =
            (published_tracks[i].layer_count > 0 && published_tracks[i].layer_count <= 3)
                ? published_tracks[i].layer_count
                : 1;
        for (layer_index = 0; layer_index < recording_track->layer_count; ++layer_index) {
            recording_track->layer_ssrcs[layer_index] = published_tracks[i].layer_ssrcs[layer_index];
        }
        recording_track->kind = published_tracks[i].kind;
        recording_track->codec = published_tracks[i].codec;
        recording_track->recorder_track_id = recorder_track_id;
    }

    if (turbo_recorder_start(recording->recorder) != 0) {
        remove_room_recording_locked(server, room_id);
        turbo_mutex_unlock(&server->recording_mutex);
        free(published_tracks);
        return -1;
    }

    for (i = 0; i < recording->track_count; ++i) {
        recording->tracks[i].rtp_ctx = turbo_recorder_create_rtp_context(
            recording->recorder, recording->tracks[i].recorder_track_id);
        if (!recording->tracks[i].rtp_ctx) {
            remove_room_recording_locked(server, room_id);
            turbo_mutex_unlock(&server->recording_mutex);
            free(published_tracks);
            return -1;
        }
    }

    recording->active = 1;
    refresh_room_recording_stats_locked(recording);
    turbo_mutex_unlock(&server->recording_mutex);
    free(published_tracks);
    return 0;
}

int sfu_node_app_server_stop_recording(sfu_node_app_server_t *server,
                                       const char *room_id) {
    sfu_node_room_recording_t *recording;
    int stop_rc;

    if (!server || !room_id) {
        return -1;
    }

    turbo_mutex_lock(&server->recording_mutex);
    recording = find_room_recording_locked(server, room_id);
    if (!recording || !recording->recorder || !recording->active) {
        turbo_mutex_unlock(&server->recording_mutex);
        return -1;
    }

    stop_rc = turbo_recorder_stop(recording->recorder);
    recording->active = 0;
    destroy_room_recording_rtp_contexts_locked(recording);
    refresh_room_recording_stats_locked(recording);
    turbo_mutex_unlock(&server->recording_mutex);
    return stop_rc;
}

void sfu_node_app_server_clear_room_runtime(sfu_node_app_server_t *server,
                                            const char *room_id) {
    if (!server || !room_id) {
        return;
    }

    turbo_mutex_lock(&server->mutex);
    clear_room_runtime_state_locked(server, room_id);
    turbo_mutex_unlock(&server->mutex);

    turbo_mutex_lock(&server->recording_mutex);
    remove_room_recording_locked(server, room_id);
    turbo_mutex_unlock(&server->recording_mutex);
}

char *sfu_node_app_server_build_webrtc_session_json(sfu_node_app_server_t *server,
                                                    const char *room_id,
                                                    const char *session_id) {
    sfu_node_webrtc_session_t *session;
    char room_id_copy[TURBO_ROOM_ID_MAX];
    char participant_id_copy[TURBO_PARTICIPANT_ID_MAX];
    char session_id_copy[TURBO_PARTICIPANT_ID_MAX];
    turbo_peer_state_t state;
    int remote_description_set;
    int remote_track_count;
    int remote_frame_count;
    int relay_track_count;
    int local_candidate_count;
    char *local_answer = NULL;
    char **local_candidates = NULL;
    char *escaped_answer = NULL;
    char *json = NULL;
    size_t json_len = 512;
    size_t written = 0;

    if (!server || !room_id || !session_id) {
        return NULL;
    }

    turbo_mutex_lock(&server->mutex);
    session = find_webrtc_session_locked(server, room_id, session_id);
    if (!session) {
        turbo_mutex_unlock(&server->mutex);
        return NULL;
    }

    turbo_mutex_lock(&session->mutex);
    copy_string(room_id_copy, sizeof(room_id_copy), session->room_id);
    copy_string(participant_id_copy, sizeof(participant_id_copy), session->participant_id);
    copy_string(session_id_copy, sizeof(session_id_copy), session->session_id);
    state = session->state;
    remote_description_set = session->remote_description_set;
    remote_track_count = session->remote_track_count;
    remote_frame_count = session->remote_frame_count;
    relay_track_count = session->relay_track_count;
    local_candidate_count = session->local_candidate_count;

    if (session->local_answer) {
        local_answer = sfu_strdup(session->local_answer);
    }
    if (local_candidate_count > 0) {
        local_candidates = (char **)calloc((size_t)local_candidate_count, sizeof(char *));
        if (!local_candidates) {
            turbo_mutex_unlock(&session->mutex);
            turbo_mutex_unlock(&server->mutex);
            free(local_answer);
            return NULL;
        }
        for (int i = 0; i < local_candidate_count; ++i) {
            local_candidates[i] = sfu_strdup(session->local_candidates[i]);
            if (!local_candidates[i]) {
                for (int j = 0; j < i; ++j) {
                    free(local_candidates[j]);
                }
                free(local_candidates);
                turbo_mutex_unlock(&session->mutex);
                turbo_mutex_unlock(&server->mutex);
                free(local_answer);
                return NULL;
            }
        }
    }
    turbo_mutex_unlock(&session->mutex);
    turbo_mutex_unlock(&server->mutex);

    if (local_answer) {
        escaped_answer = escape_json_string(local_answer);
        if (!escaped_answer) {
            goto cleanup;
        }
        json_len += strlen(escaped_answer);
    }

    for (int i = 0; i < local_candidate_count; ++i) {
        char *escaped_candidate = escape_json_string(local_candidates[i]);
        if (!escaped_candidate) {
            goto cleanup;
        }
        json_len += strlen(escaped_candidate) + 8;
        free(local_candidates[i]);
        local_candidates[i] = escaped_candidate;
    }

    json = (char *)malloc(json_len);
    if (!json) {
        goto cleanup;
    }

    written += (size_t)snprintf(
        json + written, json_len - written,
        "{"
        "\"room_id\":\"%s\","
        "\"participant_id\":\"%s\","
        "\"session_id\":\"%s\","
        "\"state\":\"%s\","
        "\"remote_description_set\":%s,"
        "\"remote_track_count\":%d,"
        "\"remote_frame_count\":%d,"
        "\"relay_track_count\":%d,"
        "\"local_candidate_count\":%d,"
        "\"local_answer\":",
        room_id_copy,
        participant_id_copy,
        session_id_copy,
        peer_state_name(state),
        remote_description_set ? "true" : "false",
        remote_track_count,
        remote_frame_count,
        relay_track_count,
        local_candidate_count);

    if (local_answer) {
        written += (size_t)snprintf(json + written, json_len - written, "\"%s\",",
                                    escaped_answer ? escaped_answer : "");
    } else {
        written += (size_t)snprintf(json + written, json_len - written, "null,");
    }

    written += (size_t)snprintf(json + written, json_len - written,
                                "\"local_ice_candidates\":[");
    for (int i = 0; i < local_candidate_count; ++i) {
        written += (size_t)snprintf(json + written, json_len - written, "%s\"%s\"",
                                    i == 0 ? "" : ",", local_candidates[i]);
    }
    snprintf(json + written, json_len - written, "]}");

cleanup:
    free(local_answer);
    free(escaped_answer);
    if (local_candidates) {
        for (int i = 0; i < local_candidate_count; ++i) {
            free(local_candidates[i]);
        }
        free(local_candidates);
    }

    return json;
}

char *sfu_node_app_server_build_recording_status_json(sfu_node_app_server_t *server,
                                                      const char *room_id) {
    sfu_node_room_recording_t *recording;
    char room_copy[TURBO_ROOM_ID_MAX];
    char recording_id_copy[SFU_NODE_RECORDING_ID_CAPACITY];
    char mode_copy[SFU_NODE_RECORDING_MODE_CAPACITY];
    char output_path_copy[SFU_NODE_RECORDING_PATH_CAPACITY];
    char *escaped_output_path = NULL;
    char *json = NULL;
    int active = 0;
    int track_count = 0;
    int64_t packet_count = 0;
    int64_t bytes_written = 0;
    int64_t duration_us = 0;

    if (!server || !room_id) {
        return NULL;
    }

    turbo_mutex_lock(&server->recording_mutex);
    recording = find_room_recording_locked(server, room_id);
    if (!recording) {
        turbo_mutex_unlock(&server->recording_mutex);
        return NULL;
    }

    refresh_room_recording_stats_locked(recording);
    copy_string(room_copy, sizeof(room_copy), recording->room_id);
    copy_string(recording_id_copy, sizeof(recording_id_copy), recording->recording_id);
    copy_string(mode_copy, sizeof(mode_copy), recording->mode);
    copy_string(output_path_copy, sizeof(output_path_copy), recording->output_path);
    active = recording->active;
    track_count = recording->track_count;
    packet_count = recording->packet_count;
    bytes_written = recording->bytes_written;
    duration_us = recording->duration_us;
    turbo_mutex_unlock(&server->recording_mutex);

    escaped_output_path = escape_json_string(output_path_copy);
    if (!escaped_output_path) {
        return NULL;
    }

    json = (char *)malloc(768 + strlen(escaped_output_path));
    if (!json) {
        free(escaped_output_path);
        return NULL;
    }

    snprintf(json, 768 + strlen(escaped_output_path),
             "{"
             "\"room_id\":\"%s\","
             "\"recording_id\":\"%s\","
             "\"mode\":\"%s\","
             "\"active\":%s,"
             "\"output_path\":\"%s\","
             "\"track_count\":%d,"
             "\"packet_count\":%lld,"
             "\"total_bytes\":%lld,"
             "\"duration_us\":%lld"
             "}",
             room_copy,
             recording_id_copy,
             mode_copy,
             active ? "true" : "false",
             escaped_output_path,
             track_count,
             (long long)packet_count,
             (long long)bytes_written,
             (long long)duration_us);

    free(escaped_output_path);
    return json;
}
