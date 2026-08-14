#ifndef TURBO_MEDIA_IRIS_RESOURCE_OBSERVER_H
#define TURBO_MEDIA_IRIS_RESOURCE_OBSERVER_H

#include "iris_command_ledger_port.h"

typedef enum iris_resource_observation_state_e {
    IRIS_RESOURCE_OBSERVATION_UNKNOWN = 0,
    IRIS_RESOURCE_OBSERVATION_ACTIVE,
    IRIS_RESOURCE_OBSERVATION_ABSENT
} iris_resource_observation_state_t;

/* Owning synchronous observation copied while the resource owner holds its
   own lock. Callers may retain the value after the callback returns. */
typedef struct iris_resource_observation_s {
    iris_resource_observation_state_t state;
    char provider_resource_id[IRIS_COMMAND_RESOURCE_ID_CAPACITY];
} iris_resource_observation_t;

#endif
