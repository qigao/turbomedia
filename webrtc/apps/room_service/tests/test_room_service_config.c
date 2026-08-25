#include "room_service/config.h"
#include <tinytest.h>

#include <stdlib.h>
#include <string.h>

#ifndef ROOM_SERVICE_CONFIG_EXAMPLE_PATH
#error "ROOM_SERVICE_CONFIG_EXAMPLE_PATH must identify the shipped TOML example"
#endif

static char *write_toml(const char *content) {
    char *path = tt_make_temp_file("room_cfg", ".toml");
    int result;

    check_not_null(path);
    if (!path) {
        return NULL;
    }
    result = tt_write_file(path, content, strlen(content));
    check_equal(result, 0);
    if (result != 0) {
        tt_remove_file(path);
        free(path);
        return NULL;
    }
    return path;
}

static void remove_toml(char *path) {
    if (!path) {
        return;
    }
    check_equal(tt_remove_file(path), 0);
    free(path);
}

spec("room service TOML configuration") {
    it("loads every supported section with typed values") {
        static const char toml[] =
            "[server]\n"
            "host = \"127.0.0.1\"\n"
            "port = 19090\n"
            "use_tls = true\n"
            "cert_file = \"room-chain.pem\"\n"
            "key_file = \"room-key.pem\"\n"
            "node_id = \"room-control-1\"\n"
            "[control]\n"
            "token = \"room-token\"\n"
            "[auth]\n"
            "issuer = \"room-issuer\"\n"
            "active_key_id = \"room-key-2026-07\"\n"
            "active_secret = \"room-active-secret-at-least-32-bytes\"\n"
            "previous_key_id = \"room-key-2026-06\"\n"
            "previous_secret = \"room-previous-secret-at-least-32-bytes\"\n"
            "revoked_token_sha256 = \"0000000000000000000000000000000000000000000000000000000000000000\"\n"
            "clock_skew_seconds = 15\n"
            "max_ttl_seconds = 900\n"
            "[sfu]\n"
            "control_url = \"https://sfu-a.internal:19190\"\n"
            "nodes = \"sfu-a=https://sfu-a.internal:19190,sfu-b=https://sfu-b.internal:19191\"\n"
            "control_token = \"sfu-token\"\n"
            "ca_file = \"internal-ca.pem\"\n"
            "[sfu_auth]\n"
            "issuer = \"sfu-command-issuer\"\n"
            "key_id = \"sfu-key-2026-07\"\n"
            "secret = \"sfu-command-secret-at-least-32-bytes\"\n"
            "ttl_seconds = 45\n"
            "[capacity]\n"
            "max_rooms = 512\n"
            "[rooms]\n"
            "auto_create = false\n"
            "[runtime]\n"
            "dry_run = true\n"
            "[logging]\n"
            "level = \"warn\"\n"
            "[iris_provider]\n"
            "flowmq_host = \"127.0.0.1\"\n"
            "flowmq_port = 17715\n"
            "flowmq_topic = \"media-provider-v1\"\n"
            "provider_instance_id = \"room-control-1\"\n"
            "iris_identity = \"iris-router-1\"\n"
            "flowmq_use_tls = false\n"
            "flowmq_allow_insecure_loopback = true\n"
            "event_store_config = \"room-flowstore.yaml\"\n"
            "event_store_channel = \"iris.media_events\"\n"
            "command_ledger_channel = \"iris.provider_commands\"\n"
            "allow_development_sqlite = true\n"
            "correlation_capacity = 2048\n"
             "completion_queue_capacity = 512\n"
             "reconcile_inventory_queue_capacity = 4\n"
            "outbox_request_queue_capacity = 256\n"
            "command_ledger_queue_capacity = 128\n"
            "command_terminal_retention_seconds = 7200\n"
            "command_retention_batch_size = 32\n"
            "dead_retention_seconds = 3600\n"
            "archive_retention_seconds = 604800\n"
            "retention_sweep_interval_ms = 30000\n"
            "retention_sweep_batch_size = 64\n"
            "retry_max_attempts = 6\n"
            "retry_backoff_ms = 125\n"
            "ack_timeout_ms = 4000\n"
            "drain_timeout_ms = 20000\n"
            "[fmq]\n"
            "bind_host = \"127.0.0.1\"\n"
            "bind_port = 17713\n"
            "worker_heartbeat_ms = 4000\n"
            "worker_lease_ms = 12000\n"
            "dispatch_deadline_ms = 3000\n"
            "dialog_capacity = 768\n"
            "use_tls = true\n"
            "ca_file = \"flowmq-ca.pem\"\n"
            "cert_file = \"flowmq-room-chain.pem\"\n"
            "key_file = \"flowmq-room-key.pem\"\n"
            "key_password = \"test-key-password\"\n"
            "[[fmq.workers]]\n"
            "worker_id = \"ivr-worker-a\"\n"
            "active_certificate_sha256 = \"sha256:0000000000000000000000000000000000000000000000000000000000000000\"\n"
            "generation = 7\n"
            "tenant_id = \"acme\"\n"
            "room_scope = \"acme/room-1,acme/room-2\"\n"
            "call_scope = \"call-1,call-2\"\n"
            "content_capabilities = \"conference-greeting\"\n";
        room_service_app_config_t config;
        char *path = write_toml(toml);

        room_service_app_config_init(&config);
        if (path) {
            check_equal(room_service_app_config_load(&config, path), 0);
            check_equal(config.config_file, path);
            check_equal(config.bind_host, "127.0.0.1");
            check_equal(config.bind_port, 19090);
            check_true(config.use_tls);
            check_equal(config.tls_cert_file, "room-chain.pem");
            check_equal(config.tls_key_file, "room-key.pem");
            check_equal(config.node_id, "room-control-1");
            check_equal(config.control_token, "room-token");
            check_equal(config.auth_issuer, "room-issuer");
            check_equal(config.auth_active_key_id, "room-key-2026-07");
            check_equal(config.auth_previous_key_id, "room-key-2026-06");
            check_equal(
                config.auth_revoked_token_sha256,
                "0000000000000000000000000000000000000000000000000000000000000000");
            check_equal(config.auth_clock_skew_seconds, 15);
            check_equal(config.auth_max_ttl_seconds, 900);
            check_equal(config.sfu_control_url, "https://sfu-a.internal:19190");
            check_equal(
                config.sfu_nodes,
                "sfu-a=https://sfu-a.internal:19190,sfu-b=https://sfu-b.internal:19191");
            check_equal(config.sfu_control_token, "sfu-token");
            check_equal(config.sfu_ca_file, "internal-ca.pem");
            check_equal(config.sfu_auth_issuer, "sfu-command-issuer");
            check_equal(config.sfu_auth_key_id, "sfu-key-2026-07");
            check_equal(config.sfu_auth_ttl_seconds, 45);
            check_equal(config.max_rooms, 512);
            check_false(config.auto_create_rooms);
            check_true(config.dry_run);
            check_equal(config.log_level, "warn");
            check_equal(config.iris_flowmq_host, "127.0.0.1");
            check_equal(config.iris_flowmq_port, 17715);
            check_equal(config.iris_flowmq_topic, "media-provider-v1");
            check_equal(config.iris_provider_instance_id,
                         "room-control-1");
            check_equal(config.iris_identity, "iris-router-1");
            check_false(config.iris_flowmq_use_tls);
            check_true(config.iris_flowmq_allow_insecure_loopback);
            check_equal(config.iris_event_store_config,
                         "room-flowstore.yaml");
            check_equal(config.iris_event_store_channel,
                         "iris.media_events");
            check_equal(config.iris_command_ledger_channel,
                         "iris.provider_commands");
            check_true(config.iris_allow_development_sqlite);
            check_equal(config.iris_correlation_capacity, 2048);
            check_equal(config.iris_completion_queue_capacity, 512);
            check_equal(config.iris_reconcile_inventory_queue_capacity, 4);
            check_equal(config.iris_outbox_request_queue_capacity, 256);
            check_equal(config.iris_command_ledger_queue_capacity, 128);
            check_equal(config.iris_command_terminal_retention_seconds,
                         7200);
            check_equal(config.iris_command_retention_batch_size, 32);
            check_equal(config.iris_dead_retention_seconds, 3600);
            check_equal(config.iris_archive_retention_seconds, 604800);
            check_equal(config.iris_retention_sweep_interval_ms, 30000);
            check_equal(config.iris_retention_sweep_batch_size, 64);
            check_equal(config.iris_retry_max_attempts, 6);
            check_equal(config.iris_retry_backoff_ms, 125);
            check_equal(config.iris_ack_timeout_ms, 4000);
            check_equal(config.iris_drain_timeout_ms, 20000);
            check_equal(config.fmq_bind_host, "127.0.0.1");
            check_equal(config.fmq_bind_port, 17713);
            check_equal(config.fmq_worker_heartbeat_ms, 4000);
            check_equal(config.fmq_worker_lease_ms, 12000);
            check_equal(config.fmq_dispatch_deadline_ms, 3000);
            check_equal(config.fmq_dialog_capacity, 768);
            check_true(config.fmq_use_tls);
            check_false(config.fmq_allow_insecure_loopback);
            check_equal(config.fmq_ca_file, "flowmq-ca.pem");
            check_equal(config.fmq_cert_file, "flowmq-room-chain.pem");
            check_equal(config.fmq_key_file, "flowmq-room-key.pem");
            check_equal(config.fmq_key_password, "test-key-password");
            check_equal(config.fmq_worker_identity_count, 1);
            check_equal(config.fmq_worker_identities[0].worker_id,
                         "ivr-worker-a");
            check_equal(
                config.fmq_worker_identities[0].active_certificate_sha256,
                "sha256:0000000000000000000000000000000000000000000000000000000000000000");
            check_equal(config.fmq_worker_identities[0].generation, 7);
            check_equal(config.fmq_worker_identities[0].tenant_id, "acme");
            check_equal(config.fmq_worker_identities[0].room_scope,
                         "acme/room-1,acme/room-2");
            check_equal(config.fmq_worker_identities[0].call_scope,
                         "call-1,call-2");
            check_equal(config.fmq_worker_identities[0].content_capabilities,
                         "conference-greeting");
            check_not_null(config.private_data);
        }
        room_service_app_config_cleanup(&config);
        remove_toml(path);
    }

    it("loads the shipped example") {
        room_service_app_config_t config;

        room_service_app_config_init(&config);
        check_equal(
            room_service_app_config_load(
                &config, ROOM_SERVICE_CONFIG_EXAMPLE_PATH),
            0);
        check_equal(room_service_app_config_validate(&config), 0);
        room_service_app_config_cleanup(&config);
    }

    it("uses one typed FlowMQ endpoint") {
        room_service_app_config_t config;

        room_service_app_config_init(&config);
        config.fmq_allow_insecure_loopback = 1;
        config.fmq_bind_port = 17713;
        check_equal(room_service_app_config_validate(&config), 0);
        config.fmq_bind_port = 0;
        check_equal(room_service_app_config_validate(&config), 0);
        room_service_app_config_cleanup(&config);
    }

    it("allows plaintext FlowMQ only on explicitly trusted loopback") {
        room_service_app_config_t config;

        room_service_app_config_init(&config);
        config.fmq_bind_port = 17713;
        check_equal(room_service_app_config_validate(&config), -1);
        config.fmq_allow_insecure_loopback = 1;
        config.fmq_bind_host = "0.0.0.0";
        check_equal(room_service_app_config_validate(&config), -1);
        config.fmq_bind_host = "::1";
        check_equal(room_service_app_config_validate(&config), 0);
        room_service_app_config_cleanup(&config);
    }

    it("requires complete mutually exclusive FlowMQ mTLS identity") {
        static const char fingerprint[] =
            "sha256:0000000000000000000000000000000000000000000000000000000000000000";
        room_service_app_config_t config;

        room_service_app_config_init(&config);
        config.fmq_bind_port = 17713;
        config.fmq_use_tls = 1;
        config.fmq_ca_file = "flowmq-ca.pem";
        config.fmq_cert_file = "flowmq-room-chain.pem";
        config.fmq_key_file = "flowmq-room-key.pem";
        config.fmq_worker_identity_count = 1;
        config.fmq_worker_identities[0].worker_id = "ivr-worker-a";
        config.fmq_worker_identities[0].active_certificate_sha256 = fingerprint;
        config.fmq_worker_identities[0].generation = 1u;
        check_equal(room_service_app_config_validate(&config), 0);
        config.fmq_allow_insecure_loopback = 1;
        check_equal(room_service_app_config_validate(&config), -1);
        config.fmq_allow_insecure_loopback = 0;
        config.fmq_key_file = NULL;
        check_equal(room_service_app_config_validate(&config), -1);
        config.fmq_key_file = "flowmq-room-key.pem";
        config.fmq_worker_identities[0].active_certificate_sha256 = "invalid";
        check_equal(room_service_app_config_validate(&config), -1);
        room_service_app_config_cleanup(&config);
    }

    it("validates FlowMQ heartbeat lease and dispatch timing") {
        room_service_app_config_t config;

        room_service_app_config_init(&config);
        check_equal(room_service_app_config_validate(&config), 0);
        config.fmq_worker_lease_ms = 14999;
        check_equal(room_service_app_config_validate(&config), -1);
        config.fmq_worker_lease_ms = 15000;
        config.fmq_dispatch_deadline_ms = 15000;
        check_equal(room_service_app_config_validate(&config), -1);
        config.fmq_dispatch_deadline_ms = 4999;
        check_equal(room_service_app_config_validate(&config), 0);
        config.fmq_dialog_capacity = 0;
        check_equal(room_service_app_config_validate(&config), -1);
        config.fmq_dialog_capacity = 65537;
        check_equal(room_service_app_config_validate(&config), -1);
        config.fmq_dialog_capacity = 256;
        check_equal(room_service_app_config_validate(&config), 0);
        room_service_app_config_cleanup(&config);
    }

    it("requires a complete bounded Iris provider configuration") {
        room_service_app_config_t config;

        room_service_app_config_init(&config);
        config.fmq_allow_insecure_loopback = 1;
        config.fmq_bind_port = 17713;
        config.iris_flowmq_use_tls = 0;
        config.iris_flowmq_allow_insecure_loopback = 1;
        config.iris_flowmq_host = "127.0.0.1";
        check_equal(room_service_app_config_validate(&config), -1);
        config.iris_flowmq_port = 17715;
        config.iris_provider_instance_id = "room-service-1";
        config.iris_identity = "iris-router-1";
        check_equal(room_service_app_config_validate(&config), -1);
        config.iris_event_store_config = "room-flowstore.yaml";
        check_equal(room_service_app_config_validate(&config), -1);
        config.iris_event_store_channel = "iris.media_events";
        check_equal(room_service_app_config_validate(&config), -1);
        config.iris_command_ledger_channel = "iris.provider_commands";
        check_equal(room_service_app_config_validate(&config), 0);
        config.iris_completion_queue_capacity = 0;
        check_equal(room_service_app_config_validate(&config), -1);
        config.iris_completion_queue_capacity = 1024;
        config.iris_reconcile_inventory_queue_capacity = 0;
        check_equal(room_service_app_config_validate(&config), -1);
        config.iris_reconcile_inventory_queue_capacity = 8;
        config.iris_retention_sweep_batch_size = 257;
        check_equal(room_service_app_config_validate(&config), -1);
        config.iris_retention_sweep_batch_size = 128;
        config.iris_archive_retention_seconds = 0;
        check_equal(room_service_app_config_validate(&config), -1);
        config.iris_archive_retention_seconds = 2592000;
        config.iris_retention_sweep_interval_ms = 999;
        check_equal(room_service_app_config_validate(&config), -1);
        config.iris_retention_sweep_interval_ms = 60000;
        config.iris_drain_timeout_ms = config.iris_ack_timeout_ms - 1;
        check_equal(room_service_app_config_validate(&config), -1);
        config.iris_drain_timeout_ms = 30000;
        config.iris_flowmq_host = "iris.internal";
        check_equal(room_service_app_config_validate(&config), -1);
        config.iris_flowmq_host = "127.0.0.1";
        check_equal(room_service_app_config_validate(&config), 0);
        config.iris_flowmq_host = "iris.internal";
        config.iris_flowmq_use_tls = 1;
        config.iris_flowmq_allow_insecure_loopback = 0;
        check_equal(room_service_app_config_validate(&config), -1);
        config.iris_certificate_sha256 =
            "sha256:0000000000000000000000000000000000000000000000000000000000000000";
        config.iris_flowmq_ca_file = "flowmq-ca.pem";
        config.iris_flowmq_cert_file = "room-chain.pem";
        config.iris_flowmq_key_file = "room-key.pem";
        config.iris_flowmq_server_name = "iris.internal";
        check_equal(room_service_app_config_validate(&config), 0);
        config.iris_certificate_sha256 = "sha256:bad";
        check_equal(room_service_app_config_validate(&config), -1);
        room_service_app_config_cleanup(&config);
    }

    it("rejects unknown sections without changing the current configuration") {
        static const char toml[] = "[unexpected]\nenabled = true\n";
        room_service_app_config_t config;
        char *path = write_toml(toml);

        room_service_app_config_init(&config);
        config.bind_port = 17777;
        if (path) {
            check_equal(room_service_app_config_load(&config, path), -1);
            check_equal(config.bind_port, 17777);
            check_null(config.config_file);
            check_null(config.private_data);
        }
        room_service_app_config_cleanup(&config);
        remove_toml(path);
    }

    it("rejects malformed, mistyped, and semantically invalid values") {
        static const char malformed[] = "[server\nport = 9090\n";
        static const char mistyped[] = "[rooms]\nauto_create = \"yes\"\n";
        static const char invalid[] = "[sfu]\nnodes = \"missing-url\"\n";
        room_service_app_config_t config;
        char *malformed_path = write_toml(malformed);
        char *mistyped_path = write_toml(mistyped);
        char *invalid_path = write_toml(invalid);

        room_service_app_config_init(&config);
        if (malformed_path && mistyped_path && invalid_path) {
            check_equal(room_service_app_config_load(&config, malformed_path), -1);
            check_equal(room_service_app_config_load(&config, mistyped_path), -1);
            check_equal(room_service_app_config_load(&config, invalid_path), -1);
            check_equal(config.bind_port, 9090);
            check_true(config.auto_create_rooms);
            check_null(config.private_data);
        }
        room_service_app_config_cleanup(&config);
        remove_toml(malformed_path);
        remove_toml(mistyped_path);
        remove_toml(invalid_path);
    }

    it("requires a complete certificate identity when TLS is enabled") {
        static const char missing_key[] =
            "[server]\n"
            "use_tls = true\n"
            "cert_file = \"room-chain.pem\"\n";
        room_service_app_config_t config;
        char *path = write_toml(missing_key);

        room_service_app_config_init(&config);
        if (path) {
            check_equal(room_service_app_config_load(&config, path), -1);
            check_false(config.use_tls);
            check_null(config.private_data);
        }
        room_service_app_config_cleanup(&config);
        remove_toml(path);
    }

    it("rejects incomplete or weak signed-token key configuration") {
        static const char inbound_missing_secret[] =
            "[auth]\n"
            "active_key_id = \"room-key\"\n";
        static const char outbound_weak_secret[] =
            "[sfu_auth]\n"
            "key_id = \"sfu-key\"\n"
            "secret = \"too-short\"\n";
        room_service_app_config_t config;
        char *inbound_path = write_toml(inbound_missing_secret);
        char *outbound_path = write_toml(outbound_weak_secret);

        room_service_app_config_init(&config);
        if (inbound_path && outbound_path) {
            check_equal(
                room_service_app_config_load(&config, inbound_path), -1);
            check_equal(
                room_service_app_config_load(&config, outbound_path), -1);
            check_null(config.private_data);
        }
        room_service_app_config_cleanup(&config);
        remove_toml(inbound_path);
        remove_toml(outbound_path);
    }

    it("preserves owned strings across a partial reload") {
        static const char first[] =
            "[server]\nnode_id = \"persistent-room-service\"\n";
        static const char second[] = "[capacity]\nmax_rooms = 2048\n";
        room_service_app_config_t config;
        char *first_path = write_toml(first);
        char *second_path = write_toml(second);

        room_service_app_config_init(&config);
        if (first_path && second_path) {
            check_equal(room_service_app_config_load(&config, first_path), 0);
            check_equal(room_service_app_config_load(&config, second_path), 0);
            check_equal(config.node_id, "persistent-room-service");
            check_equal(config.max_rooms, 2048);
            check_equal(config.config_file, second_path);
        }
        room_service_app_config_cleanup(&config);
        remove_toml(first_path);
        remove_toml(second_path);
    }
}
