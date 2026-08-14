#ifndef TURBO_ROOM_SERVICE_MEDIA_H
#define TURBO_ROOM_SERVICE_MEDIA_H

#include "room_service/server.h"
#include "iris_event_outbox.h"
#include "iris_media_bridge.h"
#include "iris_room_bridge.h"
#include "ivr_flowmq_gateway.h"

ivr_status_t room_service_app_server_send_ivr_media_command(
    room_service_app_server_t *server, const ivr_media_command_t *command,
    char *out_worker_id, size_t out_worker_id_capacity);

iris_media_bridge_result_t room_service_app_server_dispatch_iris_media_command(
    room_service_app_server_t *server, const char *idempotency_key,
    const char *body, size_t body_size);

iris_room_bridge_result_t room_service_app_server_dispatch_iris_room_command(
    room_service_app_server_t *server, const char *idempotency_key,
    const char *body, size_t body_size);

ivr_status_t room_service_app_server_replay_iris_event(
    room_service_app_server_t *server, const char *event_id);

ivr_status_t room_service_app_server_replay_iris_dead_letters(
    room_service_app_server_t *server, size_t limit,
    iris_event_replay_batch_result_t *result);

ivr_status_t room_service_app_server_list_iris_dead_letters(
    room_service_app_server_t *server, iris_event_dead_letter_t *items,
    size_t capacity, size_t *count, size_t *total);
ivr_status_t room_service_app_server_list_iris_archived_events(
    room_service_app_server_t *server, iris_event_archive_t *items,
    size_t capacity, size_t *count, size_t *total);
ivr_status_t room_service_app_server_run_iris_event_retention(
    room_service_app_server_t *server,
    iris_event_retention_result_t *result);

#endif
