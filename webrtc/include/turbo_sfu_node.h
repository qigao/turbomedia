/**
 * SFU node runtime skeleton for conference orchestration.
 */
#ifndef TURBO_SFU_NODE_H
#define TURBO_SFU_NODE_H

#include "turbo_room_service.h"
#include <turbo_export.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_peer_connection_s turbo_peer_connection_t;
typedef struct turbo_sfu_node_s turbo_sfu_node_t;

typedef struct {
    const char *node_id;
    int max_rooms;
    int default_room_capacity;
} turbo_sfu_node_config_t;

typedef struct {
    char node_id[TURBO_NODE_ID_MAX];
    int room_count;
    int session_count;
    int published_track_count;
    int64_t total_packets_routed;
    int64_t total_bytes_routed;
    int64_t total_layer_switches;
} turbo_sfu_node_stats_t;

typedef struct {
    char room_id[TURBO_ROOM_ID_MAX];
    int session_count;
    int published_track_count;
    int participant_count;
    int64_t total_packets_routed;
    int64_t total_bytes_routed;
    int64_t total_layer_switches;
} turbo_sfu_node_room_stats_t;

typedef struct {
    char participant_id[TURBO_PARTICIPANT_ID_MAX];
    int stream_count;
    int available_bandwidth;
    int64_t packets_sent;
    int64_t bytes_sent;
    int64_t packets_received;
    int64_t bytes_received;
} turbo_sfu_node_participant_stats_t;

typedef struct {
    char receiver_participant_id[TURBO_PARTICIPANT_ID_MAX];
    char sender_participant_id[TURBO_PARTICIPANT_ID_MAX];
    char track_id[TURBO_TRACK_ID_MAX];
    int enabled;
    int priority;
    turbo_room_video_layer_t preferred_layer;
    turbo_room_video_layer_t target_layer;
    int muted;
    char policy_source[TURBO_POLICY_SOURCE_MAX];
    turbo_room_video_layer_t max_layer;
} turbo_sfu_node_track_subscription_t;

typedef void (*turbo_sfu_node_packet_cb)(void *user_data, const uint8_t *packet, size_t len);
typedef void (*turbo_sfu_node_keyframe_request_cb)(void *user_data, uint32_t ssrc);

CXX_C_API turbo_sfu_node_t *turbo_sfu_node_create(
    const turbo_sfu_node_config_t *config);
CXX_C_API void turbo_sfu_node_destroy(turbo_sfu_node_t *node);

CXX_C_API int turbo_sfu_node_attach_room(turbo_sfu_node_t *node, const char *room_id,
                                         int max_participants);
CXX_C_API int turbo_sfu_node_detach_room(turbo_sfu_node_t *node, const char *room_id);
CXX_C_API int turbo_sfu_node_force_close_room(turbo_sfu_node_t *node, const char *room_id);

CXX_C_API int turbo_sfu_node_add_session(turbo_sfu_node_t *node, const char *room_id,
                                         const char *participant_id,
                                         const char *session_id,
                                         turbo_peer_connection_t *pc);
CXX_C_API int turbo_sfu_node_bind_session_pc(turbo_sfu_node_t *node,
                                             const char *room_id,
                                             const char *participant_id,
                                             const char *session_id,
                                             turbo_peer_connection_t *pc);
CXX_C_API int turbo_sfu_node_remove_session(turbo_sfu_node_t *node, const char *room_id,
                                            const char *session_id);

CXX_C_API int turbo_sfu_node_register_published_track(
    turbo_sfu_node_t *node, const char *room_id, const char *participant_id,
    const char *track_id, uint32_t main_ssrc, const uint32_t *layer_ssrcs,
    int layer_count);
CXX_C_API int turbo_sfu_node_unregister_published_track(
    turbo_sfu_node_t *node, const char *room_id, const char *track_id);
CXX_C_API int turbo_sfu_node_set_receiver_bandwidth(turbo_sfu_node_t *node,
                                                    const char *room_id,
                                                    const char *participant_id,
                                                    int bandwidth_bps);
CXX_C_API int turbo_sfu_node_set_track_subscription(
    turbo_sfu_node_t *node, const char *room_id, const char *receiver_participant_id,
    const char *track_id, int enabled, turbo_room_video_layer_t max_layer);
CXX_C_API int turbo_sfu_node_apply_track_subscription(
    turbo_sfu_node_t *node, const char *room_id,
    const turbo_sfu_node_track_subscription_t *subscription);
CXX_C_API int turbo_sfu_node_set_participant_packet_callback(
    turbo_sfu_node_t *node, const char *room_id, const char *participant_id,
    turbo_sfu_node_packet_cb on_packet, void *user_data);
CXX_C_API int turbo_sfu_node_set_participant_keyframe_callback(
    turbo_sfu_node_t *node, const char *room_id, const char *participant_id,
    turbo_sfu_node_keyframe_request_cb on_keyframe_request, void *user_data);
CXX_C_API int turbo_sfu_node_forward_packet(turbo_sfu_node_t *node, const char *room_id,
                                            const char *sender_participant_id,
                                            const uint8_t *packet, size_t len);

CXX_C_API int turbo_sfu_node_get_room_stats(turbo_sfu_node_t *node,
                                            const char *room_id,
                                            turbo_sfu_node_room_stats_t *stats);
CXX_C_API int turbo_sfu_node_get_participant_stats(
    turbo_sfu_node_t *node, const char *room_id, const char *participant_id,
    turbo_sfu_node_participant_stats_t *stats);
CXX_C_API int turbo_sfu_node_get_track_subscription(
    turbo_sfu_node_t *node, const char *room_id, const char *receiver_participant_id,
    const char *track_id, turbo_sfu_node_track_subscription_t *subscription);
CXX_C_API void turbo_sfu_node_get_stats(turbo_sfu_node_t *node,
                                        turbo_sfu_node_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_SFU_NODE_H */
