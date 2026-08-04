/**
 * SFU (Selective Forwarding Unit) Implementation
 * 
 * Routes media streams between multiple participants in a conference
 * Essential for scalable multi-party video conferencing
 * 
 * Features:
 * - Multi-participant support (up to 50 participants)
 * - Simulcast layer selection per receiver
 * - Bandwidth-aware forwarding
 * - Automatic layer switching
 * - Statistics per participant
 * - NACK forwarding
 * - Keyframe request handling
 */
#include "turbo_sfu.h"
#include "turbo_simulcast.h"
#include "turbo_nack.h"
#include "turbo_rtp.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

/* =============================================================================
 * Configuration
 * ============================================================================= */

#define SFU_MAX_PARTICIPANTS 50
#define SFU_MAX_STREAMS_PER_PARTICIPANT 4
#define SFU_LAYER_SWITCH_HYSTERESIS_MS 2000  /* Wait 2s before switching up */

/* =============================================================================
 * Stream Context
 * ============================================================================= */

typedef struct {
    uint32_t ssrc;
    int active;
    
    /* Simulcast layers */
    uint32_t layer_ssrcs[3];  /* Low, Medium, High */
    int layer_count;
    
    /* Current state is tracked per receiver so one constrained subscriber
     * does not force every other subscriber onto the same layer. */
    simulcast_layer_t current_layer_by_receiver[SFU_MAX_PARTICIPANTS];
    int64_t last_layer_switch_us_by_receiver[SFU_MAX_PARTICIPANTS];
    int enabled_by_receiver[SFU_MAX_PARTICIPANTS];
    simulcast_layer_t max_layer_by_receiver[SFU_MAX_PARTICIPANTS];
    
    /* Statistics */
    int64_t packets_received;
    int64_t bytes_received;
    int64_t packets_forwarded;
    int64_t bytes_forwarded;
} sfu_stream_t;

/* =============================================================================
 * Participant Context
 * ============================================================================= */

typedef struct {
    char id[64];
    int active;
    
    /* Streams */
    sfu_stream_t streams[SFU_MAX_STREAMS_PER_PARTICIPANT];
    int stream_count;
    
    /* Bandwidth */
    int available_bandwidth;
    int64_t last_bandwidth_update;
    
    /* Statistics */
    int64_t total_packets_sent;
    int64_t total_bytes_sent;
    int64_t total_packets_received;
    int64_t total_bytes_received;
    
    /* Callbacks */
    void (*on_packet)(void *user_data, const uint8_t *packet, size_t len);
    void (*on_keyframe_request)(void *user_data, uint32_t ssrc);
    void *packet_user_data;
    void *keyframe_user_data;
} sfu_participant_t;

/* =============================================================================
 * SFU Context
 * ============================================================================= */

struct sfu_context_t {
    /* Participants */
    sfu_participant_t participants[SFU_MAX_PARTICIPANTS];
    int participant_count;
    
    /* Configuration */
    int max_participants;
    int enable_simulcast;
    int enable_bandwidth_adaptation;
    
    /* Statistics */
    int64_t total_packets_routed;
    int64_t total_bytes_routed;
    int64_t total_layer_switches;
    
    /* Callbacks */
    void (*on_participant_joined)(void *user_data, const char *participant_id);
    void (*on_participant_left)(void *user_data, const char *participant_id);
    void *user_data;
};

static sfu_stream_t *find_stream_by_ssrc(sfu_participant_t *p, uint32_t ssrc);

static int64_t sfu_get_time_us(void) {
#ifdef _WIN32
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;

    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
    }

    QueryPerformanceCounter(&counter);
    return (int64_t)((counter.QuadPart * 1000000LL) / frequency.QuadPart);
#else
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + (ts.tv_nsec / 1000);
#endif
}

/* =============================================================================
 * SFU Management
 * ============================================================================= */

sfu_context_t *sfu_create(int max_participants) {
    sfu_context_t *sfu = (sfu_context_t *)calloc(1, sizeof(sfu_context_t));
    if (!sfu) return NULL;
    
    sfu->max_participants = max_participants > 0 && max_participants <= SFU_MAX_PARTICIPANTS
                           ? max_participants : SFU_MAX_PARTICIPANTS;
    sfu->enable_simulcast = 1;
    sfu->enable_bandwidth_adaptation = 1;
    
    return sfu;
}

void sfu_destroy(sfu_context_t *sfu) {
    free(sfu);
}

/* =============================================================================
 * Participant Management
 * ============================================================================= */

static sfu_participant_t *find_participant(sfu_context_t *sfu, const char *participant_id) {
    for (int i = 0; i < sfu->participant_count; i++) {
        if (sfu->participants[i].active &&
            strcmp(sfu->participants[i].id, participant_id) == 0) {
            return &sfu->participants[i];
        }
    }
    return NULL;
}

static int get_participant_index(sfu_context_t *sfu, const char *participant_id) {
    if (!sfu || !participant_id) {
        return -1;
    }

    for (int i = 0; i < SFU_MAX_PARTICIPANTS; i++) {
        if (sfu->participants[i].active &&
            strcmp(sfu->participants[i].id, participant_id) == 0) {
            return i;
        }
    }

    return -1;
}

int sfu_add_participant(sfu_context_t *sfu, const char *participant_id) {
    if (!sfu || !participant_id) return -1;
    
    /* Check if already exists */
    if (find_participant(sfu, participant_id)) {
        return -1;  /* Already exists */
    }
    
    /* Check capacity */
    if (sfu->participant_count >= sfu->max_participants) {
        return -1;  /* Full */
    }
    
    /* Find free slot */
    sfu_participant_t *p = NULL;
    for (int i = 0; i < SFU_MAX_PARTICIPANTS; i++) {
        if (!sfu->participants[i].active) {
            p = &sfu->participants[i];
            break;
        }
    }
    
    if (!p) return -1;
    
    /* Initialize participant */
    memset(p, 0, sizeof(sfu_participant_t));
    strncpy(p->id, participant_id, sizeof(p->id) - 1);
    p->active = 1;
    p->available_bandwidth = 2500000;  /* Start with 2.5 Mbps */
    
    sfu->participant_count++;
    
    /* Notify */
    if (sfu->on_participant_joined) {
        sfu->on_participant_joined(sfu->user_data, participant_id);
    }
    
    return 0;
}

int sfu_remove_participant(sfu_context_t *sfu, const char *participant_id) {
    if (!sfu || !participant_id) return -1;
    
    sfu_participant_t *p = find_participant(sfu, participant_id);
    if (!p) return -1;
    
    p->active = 0;
    sfu->participant_count--;
    
    /* Notify */
    if (sfu->on_participant_left) {
        sfu->on_participant_left(sfu->user_data, participant_id);
    }
    
    return 0;
}

void sfu_set_participant_bandwidth(sfu_context_t *sfu, const char *participant_id,
                                   int bandwidth_bps) {
    if (!sfu || !participant_id) return;
    
    sfu_participant_t *p = find_participant(sfu, participant_id);
    if (!p) return;
    
    p->available_bandwidth = bandwidth_bps;
    p->last_bandwidth_update = sfu_get_time_us();
}

void sfu_set_participant_callback(sfu_context_t *sfu, const char *participant_id,
                                  void (*on_packet)(void *user_data,
                                                   const uint8_t *packet, size_t len),
                                  void *user_data) {
    if (!sfu || !participant_id) return;
    
    sfu_participant_t *p = find_participant(sfu, participant_id);
    if (!p) return;
    
    p->on_packet = on_packet;
    p->packet_user_data = user_data;
}

void sfu_set_participant_keyframe_callback(
    sfu_context_t *sfu, const char *participant_id,
    void (*on_keyframe_request)(void *user_data, uint32_t ssrc),
    void *user_data) {
    if (!sfu || !participant_id) return;

    sfu_participant_t *p = find_participant(sfu, participant_id);
    if (!p) return;

    p->on_keyframe_request = on_keyframe_request;
    p->keyframe_user_data = user_data;
}

/* =============================================================================
 * Stream Management
 * ============================================================================= */

int sfu_add_stream(sfu_context_t *sfu, const char *participant_id, uint32_t ssrc,
                   const uint32_t *layer_ssrcs, int layer_count) {
    if (!sfu || !participant_id) return -1;
    
    sfu_participant_t *p = find_participant(sfu, participant_id);
    if (!p) return -1;
    
    /* Check capacity */
    if (p->stream_count >= SFU_MAX_STREAMS_PER_PARTICIPANT) {
        return -1;
    }
    
    /* Add stream */
    sfu_stream_t *s = &p->streams[p->stream_count];
    memset(s, 0, sizeof(sfu_stream_t));
    
    s->ssrc = ssrc;
    s->active = 1;
    for (int i = 0; i < SFU_MAX_PARTICIPANTS; i++) {
        s->current_layer_by_receiver[i] = SIMULCAST_LAYER_HIGH;
        s->last_layer_switch_us_by_receiver[i] = 0;
        s->enabled_by_receiver[i] = 1;
        s->max_layer_by_receiver[i] = SIMULCAST_LAYER_HIGH;
    }
    
    /* Set simulcast layers */
    if (layer_ssrcs && layer_count > 0 && layer_count <= 3) {
        s->layer_count = layer_count;
        for (int i = 0; i < layer_count; i++) {
            s->layer_ssrcs[i] = layer_ssrcs[i];
        }
    } else {
        s->layer_count = 1;
        s->layer_ssrcs[0] = ssrc;
    }
    
    p->stream_count++;
    
    return 0;
}

/* =============================================================================
 * Layer Selection
 * ============================================================================= */

static simulcast_layer_t select_layer_for_bandwidth(int bandwidth_bps) {
    if (bandwidth_bps >= 2000000) {
        return SIMULCAST_LAYER_HIGH;
    } else if (bandwidth_bps >= 1000000) {
        return SIMULCAST_LAYER_MEDIUM;
    } else if (bandwidth_bps >= 300000) {
        return SIMULCAST_LAYER_LOW;
    } else {
        return SIMULCAST_LAYER_LOW;  /* Always send at least low quality */
    }
}

static int should_switch_layer(sfu_stream_t *stream, int receiver_index,
                               simulcast_layer_t new_layer, int64_t now_us) {
    simulcast_layer_t current_layer = stream->current_layer_by_receiver[receiver_index];
    int64_t last_switch_us = stream->last_layer_switch_us_by_receiver[receiver_index];

    if (new_layer == current_layer) {
        return 0;  /* No change */
    }
    
    /* Switching down: immediate */
    if (new_layer < current_layer) {
        return 1;
    }
    
    /* Switching up: wait for hysteresis */
    if (now_us - last_switch_us < SFU_LAYER_SWITCH_HYSTERESIS_MS * 1000) {
        return 0;  /* Too soon */
    }
    
    return 1;
}

int sfu_set_receiver_stream_policy(sfu_context_t *sfu, const char *receiver_id,
                                   const char *sender_id, uint32_t stream_ssrc,
                                   int enabled, int max_layer) {
    sfu_participant_t *sender;
    sfu_stream_t *stream;
    int receiver_index;

    if (!sfu || !receiver_id || !sender_id || max_layer < 0 || max_layer > 2) {
        return -1;
    }

    receiver_index = get_participant_index(sfu, receiver_id);
    sender = find_participant(sfu, sender_id);
    if (receiver_index < 0 || !sender) {
        return -1;
    }

    stream = find_stream_by_ssrc(sender, stream_ssrc);
    if (!stream) {
        return -1;
    }

    stream->enabled_by_receiver[receiver_index] = enabled ? 1 : 0;
    stream->max_layer_by_receiver[receiver_index] = (simulcast_layer_t)max_layer;
    return 0;
}

/* =============================================================================
 * Packet Forwarding
 * ============================================================================= */

static int parse_rtp_ssrc(const uint8_t *packet, size_t len, uint32_t *ssrc) {
    if (len < 12) return -1;
    
    /* Check RTP version */
    if ((packet[0] & 0xC0) != 0x80) return -1;
    
    /* Extract SSRC */
    *ssrc = ((uint32_t)packet[8] << 24) | ((uint32_t)packet[9] << 16) |
            ((uint32_t)packet[10] << 8) | packet[11];
    
    return 0;
}

static sfu_stream_t *find_stream_by_ssrc(sfu_participant_t *p, uint32_t ssrc) {
    for (int i = 0; i < p->stream_count; i++) {
        sfu_stream_t *s = &p->streams[i];
        if (!s->active) continue;
        
        /* Check main SSRC */
        if (s->ssrc == ssrc) return s;
        
        /* Check layer SSRCs */
        for (int j = 0; j < s->layer_count; j++) {
            if (s->layer_ssrcs[j] == ssrc) return s;
        }
    }
    return NULL;
}

int sfu_forward_packet(sfu_context_t *sfu, const char *sender_id,
                       const uint8_t *packet, size_t len) {
    if (!sfu || !sender_id || !packet || len < 12) return -1;
    
    /* Find sender */
    sfu_participant_t *sender = find_participant(sfu, sender_id);
    if (!sender) return -1;
    
    /* Parse packet SSRC */
    uint32_t ssrc;
    if (parse_rtp_ssrc(packet, len, &ssrc) != 0) {
        return -1;
    }
    
    /* Find stream */
    sfu_stream_t *stream = find_stream_by_ssrc(sender, ssrc);
    if (!stream) return -1;
    
    /* Update sender statistics */
    sender->total_packets_received++;
    sender->total_bytes_received += len;
    stream->packets_received++;
    stream->bytes_received += len;
    
    /* Forward to all other participants */
    int forwarded = 0;
    int64_t now_us = sfu_get_time_us();
    
    for (int i = 0; i < SFU_MAX_PARTICIPANTS; i++) {
        sfu_participant_t *receiver = &sfu->participants[i];
        
        /* Skip inactive, sender, or participants without callback */
        if (!receiver->active || receiver == sender || !receiver->on_packet) {
            continue;
        }
        if (!stream->enabled_by_receiver[i]) {
            continue;
        }
        
        /* Select appropriate layer based on receiver bandwidth */
        simulcast_layer_t target_layer = SIMULCAST_LAYER_HIGH;
        
        if (sfu->enable_bandwidth_adaptation && stream->layer_count > 1) {
            target_layer = select_layer_for_bandwidth(receiver->available_bandwidth);
        }
        if (stream->layer_count > 1) {
            simulcast_layer_t max_allowed_layer = stream->max_layer_by_receiver[i];
            simulcast_layer_t highest_stream_layer =
                (simulcast_layer_t)(stream->layer_count - 1);

            if (max_allowed_layer > highest_stream_layer) {
                max_allowed_layer = highest_stream_layer;
            }
            if (target_layer > max_allowed_layer) {
                target_layer = max_allowed_layer;
            }

            /* Check if we should switch layers */
            if (should_switch_layer(stream, i, target_layer, now_us)) {
                stream->current_layer_by_receiver[i] = target_layer;
                stream->last_layer_switch_us_by_receiver[i] = now_us;
                sfu->total_layer_switches++;

                /* Request keyframe from the sender so the newly selected layer
                 * can be decoded immediately by downstream receivers. */
                if (sender->on_keyframe_request) {
                    sender->on_keyframe_request(sender->keyframe_user_data,
                                                stream->layer_ssrcs[target_layer]);
                }
            }
        }
        
        /* Check if this packet is from the selected layer */
        int should_forward = 0;
        if (stream->layer_count == 1) {
            should_forward = 1;  /* No simulcast, forward everything */
        } else {
            /* Check if packet is from current layer */
            if (ssrc == stream->layer_ssrcs[stream->current_layer_by_receiver[i]]) {
                should_forward = 1;
            }
        }
        
        if (should_forward) {
            /* Forward packet */
            receiver->on_packet(receiver->packet_user_data, packet, len);
            
            /* Update statistics */
            receiver->total_packets_sent++;
            receiver->total_bytes_sent += len;
            stream->packets_forwarded++;
            stream->bytes_forwarded += len;
            forwarded++;
        }
    }
    
    sfu->total_packets_routed++;
    sfu->total_bytes_routed += len;
    
    return forwarded;
}

/* =============================================================================
 * NACK Forwarding
 * ============================================================================= */

int sfu_forward_nack(sfu_context_t *sfu, const char *receiver_id,
                     const char *sender_id, const uint16_t *seq_nums, int count) {
    if (!sfu || !receiver_id || !sender_id || !seq_nums || count == 0) {
        return -1;
    }

    sfu_participant_t *receiver = find_participant(sfu, receiver_id);
    sfu_participant_t *sender = find_participant(sfu, sender_id);
    uint8_t rtcp_buf[RTP_MAX_PACKET];
    size_t rtcp_len = sizeof(rtcp_buf);
    uint32_t receiver_ssrc = 0;

    if (!receiver || !sender || !sender->on_packet) {
        return -1;
    }

    if (receiver->stream_count > 0) {
        receiver_ssrc = receiver->streams[0].ssrc;
    }

    if (nack_build_rtcp(receiver_ssrc, 0, seq_nums, count, rtcp_buf, &rtcp_len) != 0) {
        return -1;
    }

    sender->on_packet(sender->packet_user_data, rtcp_buf, rtcp_len);
    return 0;
}

int sfu_remove_stream(sfu_context_t *sfu, const char *participant_id, uint32_t ssrc) {
    sfu_participant_t *p;
    int i;

    if (!sfu || !participant_id) return -1;

    p = find_participant(sfu, participant_id);
    if (!p) return -1;

    for (i = 0; i < p->stream_count; i++) {
        sfu_stream_t *stream = &p->streams[i];
        if (!stream->active || stream->ssrc != ssrc) {
            continue;
        }

        if (i + 1 < p->stream_count) {
            memmove(&p->streams[i], &p->streams[i + 1],
                    (size_t)(p->stream_count - i - 1) * sizeof(sfu_stream_t));
        }
        memset(&p->streams[p->stream_count - 1], 0, sizeof(sfu_stream_t));
        p->stream_count--;
        return 0;
    }

    return -1;
}

/* =============================================================================
 * Keyframe Requests
 * ============================================================================= */

int sfu_request_keyframe(sfu_context_t *sfu, const char *participant_id, uint32_t ssrc) {
    if (!sfu || !participant_id) return -1;
    
    sfu_participant_t *p = find_participant(sfu, participant_id);
    if (!p || !p->on_keyframe_request) return -1;
    
    p->on_keyframe_request(p->keyframe_user_data, ssrc);
    
    return 0;
}

/* =============================================================================
 * Statistics
 * ============================================================================= */

void sfu_get_stats(sfu_context_t *sfu, sfu_stats_t *stats) {
    if (!sfu || !stats) return;
    
    memset(stats, 0, sizeof(sfu_stats_t));
    
    stats->participant_count = sfu->participant_count;
    stats->total_packets_routed = sfu->total_packets_routed;
    stats->total_bytes_routed = sfu->total_bytes_routed;
    stats->total_layer_switches = sfu->total_layer_switches;
}

void sfu_get_participant_stats(sfu_context_t *sfu, const char *participant_id,
                               sfu_participant_stats_t *stats) {
    if (!sfu || !participant_id || !stats) return;
    
    sfu_participant_t *p = find_participant(sfu, participant_id);
    if (!p) return;
    
    memset(stats, 0, sizeof(sfu_participant_stats_t));
    
    strncpy(stats->participant_id, p->id, sizeof(stats->participant_id) - 1);
    stats->stream_count = p->stream_count;
    stats->available_bandwidth = p->available_bandwidth;
    stats->packets_sent = p->total_packets_sent;
    stats->bytes_sent = p->total_bytes_sent;
    stats->packets_received = p->total_packets_received;
    stats->bytes_received = p->total_bytes_received;
}

int sfu_get_participant_count(sfu_context_t *sfu) {
    return sfu ? sfu->participant_count : 0;
}

int sfu_get_participant_list(sfu_context_t *sfu, char **participant_ids, int max_count) {
    if (!sfu || !participant_ids || max_count == 0) return 0;
    
    int count = 0;
    for (int i = 0; i < SFU_MAX_PARTICIPANTS && count < max_count; i++) {
        if (sfu->participants[i].active) {
            participant_ids[count++] = sfu->participants[i].id;
        }
    }
    
    return count;
}

/* =============================================================================
 * Configuration
 * ============================================================================= */

void sfu_set_simulcast_enabled(sfu_context_t *sfu, int enabled) {
    if (sfu) sfu->enable_simulcast = enabled;
}

void sfu_set_bandwidth_adaptation_enabled(sfu_context_t *sfu, int enabled) {
    if (sfu) sfu->enable_bandwidth_adaptation = enabled;
}

void sfu_set_callbacks(sfu_context_t *sfu,
                       void (*on_participant_joined)(void *user_data, const char *id),
                       void (*on_participant_left)(void *user_data, const char *id),
                       void *user_data) {
    if (!sfu) return;
    
    sfu->on_participant_joined = on_participant_joined;
    sfu->on_participant_left = on_participant_left;
    sfu->user_data = user_data;
}
