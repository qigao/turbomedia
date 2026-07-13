/**
 * @file server.h
 * @brief SFU node application wrapper
 */
#ifndef TURBO_SFU_NODE_APP_SERVER_H
#define TURBO_SFU_NODE_APP_SERVER_H

#include "sfu_node/config.h"
#include "turbo_sfu_node.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sfu_node_app_server_s sfu_node_app_server_t;

sfu_node_app_server_t *sfu_node_app_server_create(const sfu_node_app_config_t *config);
int sfu_node_app_server_start(sfu_node_app_server_t *server);
int sfu_node_app_server_run(sfu_node_app_server_t *server);
void sfu_node_app_server_stop(sfu_node_app_server_t *server);
void sfu_node_app_server_destroy(sfu_node_app_server_t *server);
turbo_sfu_node_t *sfu_node_app_server_get_node(sfu_node_app_server_t *server);
const sfu_node_app_config_t *sfu_node_app_server_get_config(sfu_node_app_server_t *server);
int sfu_node_app_server_set_draining(sfu_node_app_server_t *server, int draining);
int sfu_node_app_server_is_draining(sfu_node_app_server_t *server);
void sfu_node_app_server_poll_webrtc(sfu_node_app_server_t *server);
int sfu_node_app_server_create_webrtc_session(sfu_node_app_server_t *server,
                                              const char *room_id,
                                              const char *participant_id,
                                              const char *session_id);
int sfu_node_app_server_remove_webrtc_session(sfu_node_app_server_t *server,
                                              const char *room_id,
                                              const char *session_id);
int sfu_node_app_server_remove_room_webrtc_sessions(sfu_node_app_server_t *server,
                                                    const char *room_id);
int sfu_node_app_server_set_remote_offer(sfu_node_app_server_t *server,
                                         const char *room_id,
                                         const char *session_id,
                                         const char *sdp);
int sfu_node_app_server_add_remote_ice_candidate(sfu_node_app_server_t *server,
                                                 const char *room_id,
                                                 const char *session_id,
                                                 const char *candidate);
int sfu_node_app_server_register_published_track(sfu_node_app_server_t *server,
                                                 const char *room_id,
                                                 const char *participant_id,
                                                 const char *track_id,
                                                 uint32_t main_ssrc,
                                                 const uint32_t *layer_ssrcs,
                                                 int layer_count,
                                                 turbo_room_track_kind_t kind,
                                                 const char *codec_name);
int sfu_node_app_server_unregister_published_track(sfu_node_app_server_t *server,
                                                   const char *room_id,
                                                   const char *track_id);
int sfu_node_app_server_set_track_subscription(sfu_node_app_server_t *server,
                                               const char *room_id,
                                               const char *receiver_participant_id,
                                               const char *track_id,
                                               int enabled,
                                               turbo_room_video_layer_t max_layer);
int sfu_node_app_server_apply_track_subscription(
    sfu_node_app_server_t *server, const char *room_id,
    const turbo_sfu_node_track_subscription_t *subscription);
int sfu_node_app_server_start_recording(sfu_node_app_server_t *server,
                                        const char *room_id,
                                        const char *recording_id,
                                        const char *mode);
int sfu_node_app_server_stop_recording(sfu_node_app_server_t *server,
                                       const char *room_id);
void sfu_node_app_server_clear_room_runtime(sfu_node_app_server_t *server,
                                            const char *room_id);
char *sfu_node_app_server_build_webrtc_session_json(sfu_node_app_server_t *server,
                                                    const char *room_id,
                                                    const char *session_id);
char *sfu_node_app_server_build_recording_status_json(sfu_node_app_server_t *server,
                                                      const char *room_id);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_SFU_NODE_APP_SERVER_H */
