#ifndef TURBOMEDIA_TEST_IRIS_CONTROL_PROCESS_PEER_H
#define TURBOMEDIA_TEST_IRIS_CONTROL_PROCESS_PEER_H

#include "turbomedia_iris_provider_v1.h"

#include <stddef.h>
#include <stdint.h>

typedef struct test_iris_control_peer_s test_iris_control_peer_t;

typedef int (*test_iris_control_query_fn)(void *context,
                                         char *payload_json,
                                         size_t payload_capacity,
                                         int *available);
typedef void (*test_iris_control_completion_fn)(
    void *context, const ProviderCompletionV1_t *completion);
typedef void (*test_iris_control_event_fn)(
    void *context, const ProviderEventV1_t *event);

typedef struct test_iris_control_peer_config_s {
    uint16_t port;
    test_iris_control_query_fn query;
    test_iris_control_completion_fn completion;
    test_iris_control_event_fn event;
    void *context;
} test_iris_control_peer_config_t;

typedef struct test_iris_control_receipt_s {
    int disposition;
    int status_code;
    char command_id[128];
    char worker_id[128];
    char dispatch_epoch[32];
    char error_code[128];
    char error_message[512];
} test_iris_control_receipt_t;

int test_iris_control_peer_start(
    const test_iris_control_peer_config_t *config,
    test_iris_control_peer_t **out_peer);
void test_iris_control_peer_stop(test_iris_control_peer_t *peer);
int test_iris_control_peer_send_command(
    test_iris_control_peer_t *peer, const char *idempotency_key,
    const char *bridge_json, uint64_t timeout_ms,
    test_iris_control_receipt_t *out_receipt);

#endif
