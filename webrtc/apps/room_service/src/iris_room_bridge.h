#ifndef TURBO_ROOM_SERVICE_IRIS_ROOM_BRIDGE_H
#define TURBO_ROOM_SERVICE_IRIS_ROOM_BRIDGE_H

#include "iris_command_ledger_port.h"
#include "iris_resource_observer.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct iris_room_bridge_s iris_room_bridge_t;

typedef enum iris_room_command_kind_e {
    IRIS_ROOM_COMMAND_CREATE = 1,
    IRIS_ROOM_COMMAND_DESTROY,
    IRIS_ROOM_COMMAND_JOIN,
    IRIS_ROOM_COMMAND_UNJOIN
} iris_room_command_kind_t;

typedef struct iris_room_command_s {
    iris_room_command_kind_t kind;
    char command_id[128];
    char tenant_id[128];
    char provider_session_id[128];
    char correlation_id[128];
    char causation_id[128];
    char room_id[64];
    uint64_t room_generation;
    char call_id[64];
    uint64_t call_generation;
    char role[32];
    char user_id[64];
    char display_name[128];
} iris_room_command_t;

typedef enum iris_room_terminal_status_e {
    IRIS_ROOM_TERMINAL_SUCCEEDED = 1,
    IRIS_ROOM_TERMINAL_FAILED
} iris_room_terminal_status_t;

typedef struct iris_room_execution_s {
    iris_room_terminal_status_t terminal_status;
    int resource_absent;
    char event_type[96];
    char data[1024];
} iris_room_execution_t;

typedef enum iris_room_bridge_status_e {
    IRIS_ROOM_BRIDGE_TERMINAL = 0,
    IRIS_ROOM_BRIDGE_DUPLICATE,
    IRIS_ROOM_BRIDGE_IN_PROGRESS,
    IRIS_ROOM_BRIDGE_INVALID,
    IRIS_ROOM_BRIDGE_CONFLICT,
    IRIS_ROOM_BRIDGE_EXPIRED,
    IRIS_ROOM_BRIDGE_FULL,
    IRIS_ROOM_BRIDGE_UNAVAILABLE,
    IRIS_ROOM_BRIDGE_INTERNAL
} iris_room_bridge_status_t;

typedef struct iris_room_bridge_result_s {
    iris_room_bridge_status_t status;
    char command_id[128];
    iris_room_terminal_status_t terminal_status;
    char event_type[96];
    char data[1024];
    const char *error_code;
    const char *error_message;
} iris_room_bridge_result_t;

typedef int (*iris_room_bridge_execute_fn)(
    void *context, const iris_room_command_t *command,
    iris_room_execution_t *result);
typedef ivr_status_t (*iris_room_bridge_observe_fn)(
    void *context, const iris_room_command_t *command,
    iris_resource_observation_t *observation);
typedef uint64_t (*iris_room_bridge_realtime_ms_fn)(void *context);

typedef struct iris_room_bridge_config_s {
    size_t capacity;
    iris_room_bridge_execute_fn execute;
    void *execute_context;
    iris_room_bridge_observe_fn observe;
    void *observe_context;
    iris_room_bridge_realtime_ms_fn realtime_ms;
    void *realtime_context;
    iris_command_ledger_port_t ledger;
} iris_room_bridge_config_t;

iris_room_bridge_t *iris_room_bridge_create(
    const iris_room_bridge_config_t *config);
void iris_room_bridge_destroy(iris_room_bridge_t *bridge);

/* The request body and idempotency key are borrowed only for this call. The
   returned result owns all successful terminal data. */
iris_room_bridge_result_t iris_room_bridge_dispatch_json(
    iris_room_bridge_t *bridge, const char *idempotency_key,
    const char *body, size_t body_size);

#ifdef __cplusplus
}
#endif

#endif
