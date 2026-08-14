#include "ivr_http_media_client.h"
#include "ivr_worker_health.h"
#include "ivr_worker_http.h"
#include "ivr_worker_metrics.h"
#include "tinytest_compat.h"

#include <string.h>

enum {
    IVR_WORKER_HTTP_TEST_PORT_FIRST = 24810,
    IVR_WORKER_HTTP_TEST_PORT_LAST = 24841
};

static int start_on_available_port(ivr_worker_http_t *server) {
    int port;
    for (port = IVR_WORKER_HTTP_TEST_PORT_FIRST;
         port <= IVR_WORKER_HTTP_TEST_PORT_LAST; ++port) {
        if (ivr_worker_http_start(server, "127.0.0.1", port) == 0) {
            return port;
        }
    }
    return -1;
}

static int get_path(int port, const char *path,
                    ivr_http_media_response_t *response) {
    return ivr_http_media_request("127.0.0.1", port, "GET", path, NULL,
                                  NULL, NULL, NULL, response);
}

static int post_path(int port, const char *path,
                     ivr_http_media_response_t *response) {
    return ivr_http_media_request("127.0.0.1", port, "POST", path, NULL,
                                  NULL, NULL, NULL, response);
}

static int request_drain(void *context) {
    int *calls = (int *)context;
    ++*calls;
    return 0;
}

void test_management_endpoints_follow_health_snapshot(void) {
    ivr_worker_health_t health;
    ivr_worker_health_snapshot_t snapshot;
    ivr_worker_metrics_t metrics;
    ivr_worker_http_t *server = NULL;
    ivr_http_media_response_t response;
    int drain_calls = 0;
    int port;

    TEST_ASSERT_EQUAL_INT(0, ivr_worker_health_init(&health, 8));
    ivr_worker_metrics_init(&metrics);
    TEST_ASSERT_EQUAL_INT(0, ivr_worker_http_create(&health, &server));
    TEST_ASSERT_NOT_NULL(server);
    TEST_ASSERT_EQUAL_INT(0, ivr_worker_http_set_metrics(server, &metrics));
    TEST_ASSERT_EQUAL_INT(
        0, ivr_worker_http_set_drain_handler(server, request_drain,
                                             &drain_calls));
    port = start_on_available_port(server);
    TEST_ASSERT_GREATER_THAN(0, port);

    TEST_ASSERT_EQUAL_INT(0, get_path(port, "/live", &response));
    TEST_ASSERT_EQUAL_INT(200, response.status);
    TEST_ASSERT_NOT_NULL(strstr(response.body, "\"live\":true"));

    TEST_ASSERT_EQUAL_INT(0, get_path(port, "/ready", &response));
    TEST_ASSERT_EQUAL_INT(503, response.status);
    TEST_ASSERT_NOT_NULL(strstr(response.body, "\"ready\":false"));

    TEST_ASSERT_EQUAL_INT(0, ivr_worker_health_snapshot(&health, &snapshot));
    snapshot.ready = 1;
    snapshot.content_ready = 1;
    snapshot.schema_ready = 1;
    snapshot.command_channel_ready = 1;
    snapshot.event_channel_ready = 1;
    snapshot.sync_ready = 1;
    snapshot.speech_ready = 1;
    snapshot.sfu_ready = 1;
    snapshot.active_sessions = 3;
    strcpy(snapshot.capabilities, "turboxml,flowmq,health.ready");
    strcpy(snapshot.reason, "ready");
    TEST_ASSERT_EQUAL_INT(0, ivr_worker_health_update(&health, &snapshot));

    TEST_ASSERT_EQUAL_INT(0, get_path(port, "/ready", &response));
    TEST_ASSERT_EQUAL_INT(200, response.status);
    TEST_ASSERT_NOT_NULL(strstr(response.body, "\"generation\":2"));
    TEST_ASSERT_NOT_NULL(strstr(response.body, "\"active_sessions\":3"));
    TEST_ASSERT_NOT_NULL(strstr(response.body, "\"max_sessions\":8"));

    TEST_ASSERT_EQUAL_INT(0, get_path(port, "/health", &response));
    TEST_ASSERT_EQUAL_INT(200, response.status);
    TEST_ASSERT_NOT_NULL(strstr(response.body, "\"reason\":\"ready\""));

    ivr_worker_metrics_inc(&metrics, IVR_WORKER_METRIC_ASSIGN_ACCEPTED);
    ivr_worker_metrics_inc(&metrics, IVR_WORKER_METRIC_PROVIDER_ERROR);
    ivr_worker_metrics_add(&metrics, IVR_WORKER_METRIC_DRAIN_TIMEOUT, 2);
    ivr_worker_metrics_set_gauge(&metrics,
                                 IVR_WORKER_GAUGE_REPLY_QUEUE_ITEMS, 3);
    uint64_t gauge_value = 0;
    TEST_ASSERT_EQUAL_INT(
        0, ivr_worker_metrics_add_gauge(
               &metrics, IVR_WORKER_GAUGE_REPLY_QUEUE_BYTES, 120,
               &gauge_value));
    TEST_ASSERT_EQUAL_UINT64(120u, gauge_value);
    TEST_ASSERT_EQUAL_INT(
        0, ivr_worker_metrics_sub_gauge(
               &metrics, IVR_WORKER_GAUGE_REPLY_QUEUE_BYTES, 20,
               &gauge_value));
    TEST_ASSERT_EQUAL_UINT64(100u, gauge_value);
    TEST_ASSERT_EQUAL_INT(
        -1, ivr_worker_metrics_sub_gauge(
                &metrics, IVR_WORKER_GAUGE_REPLY_QUEUE_BYTES, 101,
                &gauge_value));
    ivr_worker_metrics_set_gauge_max(
        &metrics, IVR_WORKER_GAUGE_REPLY_QUEUE_ITEMS_HIGH_WATER, 3);
    ivr_worker_metrics_set_gauge_max(
        &metrics, IVR_WORKER_GAUGE_REPLY_QUEUE_ITEMS_HIGH_WATER, 2);
    ivr_worker_metrics_set_gauge(&metrics, IVR_WORKER_GAUGE_MEDIA_PEERS, 6);
    ivr_worker_metrics_set_gauge(
        &metrics, IVR_WORKER_GAUGE_MEDIA_LINKS_CONNECTED, 4);
    ivr_worker_metrics_observe_ms(&metrics, IVR_WORKER_HISTOGRAM_DISPATCH, 0);
    ivr_worker_metrics_observe_ms(&metrics, IVR_WORKER_HISTOGRAM_DISPATCH, 25);
    ivr_worker_metrics_observe_ms(&metrics, IVR_WORKER_HISTOGRAM_DISPATCH,
                                  30001);
    TEST_ASSERT_EQUAL_INT(0, get_path(port, "/metrics", &response));
    TEST_ASSERT_EQUAL_INT(200, response.status);
    TEST_ASSERT_NOT_NULL(strstr(response.body,
                                "turbo_ivr_worker_ready 1\n"));
    TEST_ASSERT_NOT_NULL(strstr(
        response.body, "turbo_ivr_worker_active_sessions 3\n"));
    TEST_ASSERT_NOT_NULL(strstr(
        response.body, "turbo_ivr_worker_assign_accepted_total 1\n"));
    TEST_ASSERT_NOT_NULL(strstr(
        response.body, "turbo_ivr_worker_provider_error_total 1\n"));
    TEST_ASSERT_NOT_NULL(strstr(
        response.body, "turbo_ivr_worker_drain_timeout_total 2\n"));
    TEST_ASSERT_NOT_NULL(strstr(
        response.body, "turbo_ivr_worker_reply_queue_items 3\n"));
    TEST_ASSERT_NOT_NULL(strstr(
        response.body, "turbo_ivr_worker_reply_queue_bytes 100\n"));
    TEST_ASSERT_NOT_NULL(strstr(
        response.body,
        "turbo_ivr_worker_reply_queue_items_high_water 3\n"));
    TEST_ASSERT_NOT_NULL(strstr(
        response.body, "turbo_ivr_worker_media_peers 6\n"));
    TEST_ASSERT_NOT_NULL(strstr(
        response.body, "turbo_ivr_worker_media_links_connected 4\n"));
    TEST_ASSERT_NOT_NULL(strstr(
        response.body,
        "turbo_ivr_worker_dispatch_duration_seconds_bucket{le=\"0.001\"} 1\n"));
    TEST_ASSERT_NOT_NULL(strstr(
        response.body,
        "turbo_ivr_worker_dispatch_duration_seconds_bucket{le=\"0.025\"} 2\n"));
    TEST_ASSERT_NOT_NULL(strstr(
        response.body,
        "turbo_ivr_worker_dispatch_duration_seconds_bucket{le=\"+Inf\"} 3\n"));
    TEST_ASSERT_NOT_NULL(strstr(
        response.body, "turbo_ivr_worker_dispatch_duration_seconds_sum 30.026\n"));
    TEST_ASSERT_NOT_NULL(strstr(
        response.body, "turbo_ivr_worker_dispatch_duration_seconds_count 3\n"));
    TEST_ASSERT_NULL(strstr(response.body, "room-42"));
    TEST_ASSERT_NULL(strstr(response.body, "call-42"));
    TEST_ASSERT_NULL(strstr(response.body, "message_id"));

    TEST_ASSERT_EQUAL_INT(0, post_path(port, "/drain", &response));
    TEST_ASSERT_EQUAL_INT(202, response.status);
    TEST_ASSERT_NOT_NULL(strstr(response.body, "\"accepted\":true"));
    TEST_ASSERT_EQUAL_INT(1, drain_calls);
    TEST_ASSERT_EQUAL_INT(
        -1, ivr_worker_http_set_drain_handler(server, request_drain,
                                              &drain_calls));

    TEST_ASSERT_EQUAL_INT(0, ivr_worker_health_snapshot(&health, &snapshot));
    snapshot.draining = 1;
    snapshot.ready = 0;
    strcpy(snapshot.reason, "draining");
    TEST_ASSERT_EQUAL_INT(0, ivr_worker_health_update(&health, &snapshot));
    TEST_ASSERT_EQUAL_INT(0, get_path(port, "/ready", &response));
    TEST_ASSERT_EQUAL_INT(503, response.status);
    TEST_ASSERT_NOT_NULL(strstr(response.body, "\"draining\":true"));

    ivr_worker_http_stop(server);
    TEST_ASSERT_EQUAL_INT(-1, get_path(port, "/live", &response));
    ivr_worker_http_destroy(server);
    ivr_worker_health_destroy(&health);
}

void test_management_listener_rejects_non_loopback_bind(void) {
    ivr_worker_health_t health;
    ivr_worker_http_t *server = NULL;

    TEST_ASSERT_EQUAL_INT(0, ivr_worker_health_init(&health, 1));
    TEST_ASSERT_EQUAL_INT(0, ivr_worker_http_create(&health, &server));
    TEST_ASSERT_EQUAL_INT(
        -1, ivr_worker_http_start(server, "0.0.0.0",
                                  IVR_WORKER_HTTP_TEST_PORT_FIRST));
    TEST_ASSERT_EQUAL_INT(
        -1, ivr_worker_http_start(server, "192.0.2.1",
                                  IVR_WORKER_HTTP_TEST_PORT_FIRST));
    ivr_worker_http_destroy(server);
    ivr_worker_health_destroy(&health);
}

void test_metrics_dependency_must_be_set_before_start(void) {
    ivr_worker_health_t health;
    ivr_worker_metrics_t metrics;
    ivr_worker_http_t *server = NULL;
    int port;
    ivr_http_media_response_t response;

    TEST_ASSERT_EQUAL_INT(0, ivr_worker_health_init(&health, 1));
    ivr_worker_metrics_init(&metrics);
    TEST_ASSERT_EQUAL_INT(0, ivr_worker_http_create(&health, &server));
    port = start_on_available_port(server);
    TEST_ASSERT_GREATER_THAN(0, port);
    TEST_ASSERT_EQUAL_INT(-1, ivr_worker_http_set_metrics(server, &metrics));
    TEST_ASSERT_EQUAL_INT(0, post_path(port, "/drain", &response));
    TEST_ASSERT_EQUAL_INT(503, response.status);
    TEST_ASSERT_NOT_NULL(strstr(response.body, "\"accepted\":false"));
    ivr_worker_http_stop(server);
    ivr_worker_http_destroy(server);
    ivr_worker_health_destroy(&health);
}

spec("test_ivr_worker_http") {
  TT_TEST(test_management_endpoints_follow_health_snapshot);
  TT_TEST(test_management_listener_rejects_non_loopback_bind);
  TT_TEST(test_metrics_dependency_must_be_set_before_start);
}
