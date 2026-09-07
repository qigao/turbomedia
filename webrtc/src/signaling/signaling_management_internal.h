#ifndef WEBRTC_SIGNALING_MANAGEMENT_INTERNAL_H
#define WEBRTC_SIGNALING_MANAGEMENT_INTERNAL_H

typedef struct webrtc_signaling_server_s webrtc_signaling_server_t;

int webrtc_signaling_broadcast(webrtc_signaling_server_t *server,
                               const char *room, const char *from,
                               const char *message);
char *webrtc_signaling_get_rooms_json(webrtc_signaling_server_t *server);
char *webrtc_signaling_get_room_peers_json(
    webrtc_signaling_server_t *server, const char *room_id);
int webrtc_signaling_kick_peer(webrtc_signaling_server_t *server,
                               const char *room_id, const char *peer_id,
                               const char *reason);
char *webrtc_signaling_get_status_json(webrtc_signaling_server_t *server);

#endif /* WEBRTC_SIGNALING_MANAGEMENT_INTERNAL_H */
