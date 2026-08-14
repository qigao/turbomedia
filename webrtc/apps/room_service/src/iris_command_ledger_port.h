#ifndef TURBO_ROOM_SERVICE_IRIS_COMMAND_LEDGER_PORT_H
#define TURBO_ROOM_SERVICE_IRIS_COMMAND_LEDGER_PORT_H

#include "ivr/ivr_worker.h"

#include <stdint.h>

#define IRIS_COMMAND_ID_CAPACITY 128u
#define IRIS_COMMAND_FINGERPRINT_CAPACITY 65u
#define IRIS_COMMAND_SESSION_ID_CAPACITY 128u
#define IRIS_COMMAND_TYPE_CAPACITY 96u
#define IRIS_COMMAND_RESOURCE_ID_CAPACITY 128u
#define IRIS_COMMAND_EVENT_TYPE_CAPACITY 96u
#define IRIS_COMMAND_RESULT_JSON_CAPACITY 2048u

typedef struct iris_command_identity_s {
    char command_id[IRIS_COMMAND_ID_CAPACITY];
    char semantic_fingerprint[IRIS_COMMAND_FINGERPRINT_CAPACITY];
    char provider_session_id[IRIS_COMMAND_SESSION_ID_CAPACITY];
    char command_type[IRIS_COMMAND_TYPE_CAPACITY];
    char resource_scope_id[IRIS_COMMAND_RESOURCE_ID_CAPACITY];
    uint64_t resource_scope_generation;
    char resource_id[IRIS_COMMAND_RESOURCE_ID_CAPACITY];
    uint64_t resource_generation;
} iris_command_identity_t;

typedef enum iris_command_claim_disposition_e {
    IRIS_COMMAND_CLAIM_EXECUTE = 1,
    IRIS_COMMAND_CLAIM_IN_PROGRESS,
    IRIS_COMMAND_CLAIM_REPLAY_ACCEPTED,
    IRIS_COMMAND_CLAIM_REPLAY_TERMINAL,
    IRIS_COMMAND_CLAIM_OUTCOME_UNKNOWN,
    IRIS_COMMAND_CLAIM_CONFLICT
} iris_command_claim_disposition_t;

typedef struct iris_command_claim_result_s {
    iris_command_claim_disposition_t disposition;
    uint64_t store_revision;
    char provider_resource_id[IRIS_COMMAND_RESOURCE_ID_CAPACITY];
    char terminal_status[16];
    char event_type[IRIS_COMMAND_EVENT_TYPE_CAPACITY];
    char result_json[IRIS_COMMAND_RESULT_JSON_CAPACITY];
} iris_command_claim_result_t;

typedef struct iris_command_terminal_outcome_s {
    char terminal_status[16]; /* succeeded | failed */
    char event_type[IRIS_COMMAND_EVENT_TYPE_CAPACITY];
    char result_json[IRIS_COMMAND_RESULT_JSON_CAPACITY];
} iris_command_terminal_outcome_t;

typedef struct iris_command_ledger_port_s {
    void *context;
    ivr_status_t (*claim)(void *context,
                          const iris_command_identity_t *identity,
                          iris_command_claim_result_t *result);
    ivr_status_t (*commit_accepted)(
        void *context, const iris_command_identity_t *identity,
        const char *provider_resource_id);
    ivr_status_t (*commit_terminal)(
        void *context, const iris_command_identity_t *identity,
        const iris_command_terminal_outcome_t *outcome);
    ivr_status_t (*mark_unknown)(
        void *context, const iris_command_identity_t *identity);
    ivr_status_t (*resource_seen)(
        void *context, const iris_command_identity_t *identity,
        int *seen);
    ivr_status_t (*abort_intent)(
        void *context, const iris_command_identity_t *identity);
} iris_command_ledger_port_t;

#endif
