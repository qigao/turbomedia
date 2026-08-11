#include "ivr_fmq_security.h"
#include "ivr_flowmq_gateway.h"
#include "ivr_flowmq_subscriber.h"
#include "ivr_room_bridge.h"
#include "ivr_thread.h"
#include "tinytest.h"
#include "turbo_error.h"
#include "turbo_flow_fmq.h"

#include <stdatomic.h>
#include <string.h>

#ifndef IVR_FMQ_TEST_TLS_CERT_PATH
#error "IVR_FMQ_TEST_TLS_CERT_PATH must identify the TLS fixture"
#endif
#ifndef IVR_FMQ_TEST_TLS_KEY_PATH
#error "IVR_FMQ_TEST_TLS_KEY_PATH must identify the TLS fixture key"
#endif

#define IVR_FMQ_MTLS_ROUTER_PORT 17725
#define IVR_FMQ_MTLS_PUB_PORT 17726
#define IVR_FMQ_MTLS_WAIT_MS 5000

static const char kSecret[] =
    "0123456789abcdef0123456789abcdef";
static const char kActiveFingerprint[] =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";
static const char kFixtureFingerprint[] =
    "sha256:ebd76f304bc43bc2be697fca2f054206978c0558931529a7c1b2bb7d82a7a3c4";

static uint64_t test_clock(void *context) {
    return *(const uint64_t *)context;
}

static uint64_t room_version(void *context, const char *room_id) {
    (void)context;
    (void)room_id;
    return 1u;
}

static ivr_status_t apply_command(void *context,
                                  const ivr_room_command_t *command,
                                  ivr_room_command_result_t *result) {
    (void)context;
    (void)command;
    if (!result) return IVR_EINVAL;
    result->status_code = IVR_OK;
    result->room_version = 1u;
    return IVR_OK;
}

static void connection_changed(void *context, int connected) {
    atomic_store_explicit((_Atomic int *)context, connected,
                          memory_order_release);
}

static int wait_for_both(_Atomic int *command_connected,
                         _Atomic int *event_connected, int expected) {
    int attempts = IVR_FMQ_MTLS_WAIT_MS / 20;
    while (attempts-- > 0) {
        int command = atomic_load_explicit(command_connected,
                                           memory_order_acquire);
        int event = atomic_load_explicit(event_connected,
                                         memory_order_acquire);
        if (command == expected && event == expected) return 1;
        ivr_thread_sleep_ms(20u);
    }
    return 0;
}

static int start_clients(
    const char *identity, const turbo_flow_fmq_tls_config_t *tls,
    const turbo_flow_fmq_security_binding_t *security,
    _Atomic int *command_connected, _Atomic int *event_connected,
    ivr_flowmq_gateway_t **out_gateway,
    ivr_flowmq_subscriber_t **out_subscriber) {
    ivr_flowmq_gateway_config_t gateway_config;
    ivr_flowmq_subscriber_config_t subscriber_config;
    ivr_command_gateway_ops_t gateway_ops;
    ivr_flowmq_gateway_t *gateway = NULL;
    ivr_flowmq_subscriber_t *subscriber = NULL;

    *out_gateway = NULL;
    *out_subscriber = NULL;
    memset(&gateway_config, 0, sizeof(gateway_config));
    memset(&subscriber_config, 0, sizeof(subscriber_config));
    memset(&gateway_ops, 0, sizeof(gateway_ops));
    atomic_store_explicit(command_connected, 0, memory_order_release);
    atomic_store_explicit(event_connected, 0, memory_order_release);

    gateway_config.worker_id = identity;
    gateway_config.host = "localhost";
    gateway_config.port = IVR_FMQ_MTLS_ROUTER_PORT;
    gateway_config.transport = TURBO_FLOW_FMQ_TLS;
    gateway_config.timeout_ms = 1000u;
    gateway_config.tls = tls;
    gateway_config.security = security;
    gateway_config.on_connection = connection_changed;
    gateway_config.connection_ctx = command_connected;
    if (ivr_flowmq_gateway_create(&gateway_config, &gateway_ops, &gateway) !=
        IVR_OK) {
        return 0;
    }

    subscriber_config.identity = identity;
    subscriber_config.host = "localhost";
    subscriber_config.port = IVR_FMQ_MTLS_PUB_PORT;
    subscriber_config.transport = TURBO_FLOW_FMQ_TLS;
    subscriber_config.topic = "room.events";
    subscriber_config.timeout_ms = 1000u;
    subscriber_config.tls = tls;
    subscriber_config.security = security;
    subscriber_config.on_connection = connection_changed;
    subscriber_config.connection_ctx = event_connected;
    if (ivr_flowmq_subscriber_create(&subscriber_config, &subscriber) !=
        IVR_OK) {
        ivr_flowmq_gateway_destroy(gateway);
        return 0;
    }
    if (ivr_flowmq_gateway_start(gateway) != IVR_OK ||
        ivr_flowmq_subscriber_start(subscriber) != IVR_OK) {
        ivr_flowmq_subscriber_destroy(subscriber);
        ivr_flowmq_gateway_destroy(gateway);
        return 0;
    }
    *out_gateway = gateway;
    *out_subscriber = subscriber;
    return 1;
}

static void stop_clients(ivr_flowmq_gateway_t **gateway,
                         ivr_flowmq_subscriber_t **subscriber) {
    ivr_flowmq_subscriber_destroy(*subscriber);
    *subscriber = NULL;
    ivr_flowmq_gateway_destroy(*gateway);
    *gateway = NULL;
}

spec("IVR FlowMQ mTLS application wiring") {
  it("requires an explicit secure subscriber identity") {
    turbo_flow_fmq_security_binding_t security =
        TURBO_FLOW_FMQ_SECURITY_BINDING_INIT;
    ivr_flowmq_subscriber_config_t config;
    ivr_flowmq_subscriber_t *subscriber = NULL;
    memset(&config, 0, sizeof(config));
    config.host = "127.0.0.1";
    config.port = IVR_FMQ_MTLS_PUB_PORT;
    config.security = &security;
    check_int_eq(ivr_flowmq_subscriber_create(&config, &subscriber),
                 IVR_EINVAL);
    check_null(subscriber);
  }

  it("binds both channels to worker certificate identity and expiry") {
    uint64_t now_ms = 1000u;
    ivr_certificate_identity_entry_t identity = {
        "worker-a", kActiveFingerprint, kFixtureFingerprint, 2000u, 2u};
    ivr_fmq_server_security_config_t server_security_config =
        IVR_FMQ_SERVER_SECURITY_CONFIG_INIT;
    ivr_fmq_client_security_config_t client_security_config =
        IVR_FMQ_CLIENT_SECURITY_CONFIG_INIT;
    ivr_fmq_security_owner_t *server_security = NULL;
    ivr_fmq_security_owner_t *client_security = NULL;
    turbo_flow_fmq_tls_config_t server_tls = TURBO_FLOW_FMQ_TLS_CONFIG_INIT;
    turbo_flow_fmq_tls_config_t client_tls = TURBO_FLOW_FMQ_TLS_CONFIG_INIT;
    ivr_room_bridge_config_t bridge_config;
    ivr_room_bridge_t *bridge = NULL;
    ivr_flowmq_gateway_t *gateway = NULL;
    ivr_flowmq_subscriber_t *subscriber = NULL;
    _Atomic int command_connected = 0;
    _Atomic int event_connected = 0;

    server_security_config.shared_secret = kSecret;
    server_security_config.pub_topic = "room.events";
    server_security_config.identities = &identity;
    server_security_config.identity_count = 1u;
    server_security_config.policy_version = 2u;
    server_security_config.clock = test_clock;
    server_security_config.clock_context = &now_ms;
    check_int_eq(ivr_fmq_server_security_create(&server_security_config,
                                                &server_security),
                 TURBO_OK);
    client_security_config.shared_secret = kSecret;
    check_int_eq(ivr_fmq_client_security_create(&client_security_config,
                                                &client_security),
                 TURBO_OK);

    server_tls.ca_file = IVR_FMQ_TEST_TLS_CERT_PATH;
    server_tls.cert_file = IVR_FMQ_TEST_TLS_CERT_PATH;
    server_tls.key_file = IVR_FMQ_TEST_TLS_KEY_PATH;
    server_tls.verify_peer = 1;
    server_tls.require_client_certificate = 1;
    server_tls.rotation_generation = 2u;
    client_tls.ca_file = IVR_FMQ_TEST_TLS_CERT_PATH;
    client_tls.cert_file = IVR_FMQ_TEST_TLS_CERT_PATH;
    client_tls.key_file = IVR_FMQ_TEST_TLS_KEY_PATH;
    client_tls.server_name = "localhost";
    client_tls.verify_peer = 1;
    client_tls.rotation_generation = 2u;

    memset(&bridge_config, 0, sizeof(bridge_config));
    bridge_config.host = "127.0.0.1";
    bridge_config.port = IVR_FMQ_MTLS_ROUTER_PORT;
    bridge_config.transport = TURBO_FLOW_FMQ_TLS;
    bridge_config.tls = &server_tls;
    bridge_config.security = ivr_fmq_security_binding(server_security);
    bridge_config.pub_host = "127.0.0.1";
    bridge_config.pub_port = IVR_FMQ_MTLS_PUB_PORT;
    bridge_config.pub_transport = TURBO_FLOW_FMQ_TLS;
    bridge_config.pub_tls = &server_tls;
    bridge_config.pub_security = ivr_fmq_security_binding(server_security);
    bridge_config.pub_topic = "room.events";
    bridge_config.timeout_ms = 1000u;
    bridge_config.queue_capacity = 8u;
    bridge_config.dedup_capacity = 8u;
    bridge_config.handler.get_room_version = room_version;
    bridge_config.handler.on_command = apply_command;
    check_int_eq(ivr_room_bridge_create(&bridge_config, &bridge), IVR_OK);
    check_int_eq(ivr_room_bridge_start(bridge), IVR_OK);

    check_true(start_clients("worker-a", &client_tls,
                             ivr_fmq_security_binding(client_security),
                             &command_connected, &event_connected, &gateway,
                             &subscriber));
    check_true(wait_for_both(&command_connected, &event_connected, 1));
    stop_clients(&gateway, &subscriber);

    if (start_clients("worker-b", &client_tls,
                      ivr_fmq_security_binding(client_security),
                      &command_connected, &event_connected, &gateway,
                      &subscriber)) {
        ivr_thread_sleep_ms(1500u);
    }
    check_false(atomic_load_explicit(&command_connected,
                                     memory_order_acquire));
    check_false(atomic_load_explicit(&event_connected,
                                     memory_order_acquire));
    stop_clients(&gateway, &subscriber);

    now_ms = 2000u;
    if (start_clients("worker-a", &client_tls,
                      ivr_fmq_security_binding(client_security),
                      &command_connected, &event_connected, &gateway,
                      &subscriber)) {
        ivr_thread_sleep_ms(1500u);
    }
    check_false(atomic_load_explicit(&command_connected,
                                     memory_order_acquire));
    check_false(atomic_load_explicit(&event_connected,
                                     memory_order_acquire));
    stop_clients(&gateway, &subscriber);

    ivr_room_bridge_stop(bridge);
    ivr_room_bridge_destroy(bridge);
    ivr_fmq_security_destroy(client_security);
    ivr_fmq_security_destroy(server_security);
  }
}
