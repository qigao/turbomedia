#ifndef TURBO_ROOM_SERVICE_MEDIA_H
#define TURBO_ROOM_SERVICE_MEDIA_H

#include "room_service/server.h"
#include "ivr_control_gateway.h"

ivr_status_t room_service_app_server_send_ivr_media_command(
    room_service_app_server_t *server, const ivr_media_command_t *command,
    char *out_worker_id, size_t out_worker_id_capacity);


#endif
