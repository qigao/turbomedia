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
    check_int_eq(result, 0);
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
    check_int_eq(tt_remove_file(path), 0);
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
            "level = \"warn\"\n";
        room_service_app_config_t config;
        char *path = write_toml(toml);

        room_service_app_config_init(&config);
        if (path) {
            check_int_eq(room_service_app_config_load(&config, path), 0);
            check_str_eq(config.config_file, path);
            check_str_eq(config.bind_host, "127.0.0.1");
            check_int_eq(config.bind_port, 19090);
            check_true(config.use_tls);
            check_str_eq(config.tls_cert_file, "room-chain.pem");
            check_str_eq(config.tls_key_file, "room-key.pem");
            check_str_eq(config.node_id, "room-control-1");
            check_str_eq(config.control_token, "room-token");
            check_str_eq(config.auth_issuer, "room-issuer");
            check_str_eq(config.auth_active_key_id, "room-key-2026-07");
            check_str_eq(config.auth_previous_key_id, "room-key-2026-06");
            check_str_eq(
                config.auth_revoked_token_sha256,
                "0000000000000000000000000000000000000000000000000000000000000000");
            check_int_eq(config.auth_clock_skew_seconds, 15);
            check_int_eq(config.auth_max_ttl_seconds, 900);
            check_str_eq(config.sfu_control_url, "https://sfu-a.internal:19190");
            check_str_eq(
                config.sfu_nodes,
                "sfu-a=https://sfu-a.internal:19190,sfu-b=https://sfu-b.internal:19191");
            check_str_eq(config.sfu_control_token, "sfu-token");
            check_str_eq(config.sfu_ca_file, "internal-ca.pem");
            check_str_eq(config.sfu_auth_issuer, "sfu-command-issuer");
            check_str_eq(config.sfu_auth_key_id, "sfu-key-2026-07");
            check_int_eq(config.sfu_auth_ttl_seconds, 45);
            check_int_eq(config.max_rooms, 512);
            check_false(config.auto_create_rooms);
            check_true(config.dry_run);
            check_str_eq(config.log_level, "warn");
            check_not_null(config.private_data);
        }
        room_service_app_config_cleanup(&config);
        remove_toml(path);
    }

    it("loads the shipped example") {
        room_service_app_config_t config;

        room_service_app_config_init(&config);
        check_int_eq(
            room_service_app_config_load(
                &config, ROOM_SERVICE_CONFIG_EXAMPLE_PATH),
            0);
        check_int_eq(room_service_app_config_validate(&config), 0);
        room_service_app_config_cleanup(&config);
    }

    it("rejects unknown sections without changing the current configuration") {
        static const char toml[] = "[unexpected]\nenabled = true\n";
        room_service_app_config_t config;
        char *path = write_toml(toml);

        room_service_app_config_init(&config);
        config.bind_port = 17777;
        if (path) {
            check_int_eq(room_service_app_config_load(&config, path), -1);
            check_int_eq(config.bind_port, 17777);
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
            check_int_eq(room_service_app_config_load(&config, malformed_path), -1);
            check_int_eq(room_service_app_config_load(&config, mistyped_path), -1);
            check_int_eq(room_service_app_config_load(&config, invalid_path), -1);
            check_int_eq(config.bind_port, 9090);
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
            check_int_eq(room_service_app_config_load(&config, path), -1);
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
            check_int_eq(
                room_service_app_config_load(&config, inbound_path), -1);
            check_int_eq(
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
            check_int_eq(room_service_app_config_load(&config, first_path), 0);
            check_int_eq(room_service_app_config_load(&config, second_path), 0);
            check_str_eq(config.node_id, "persistent-room-service");
            check_int_eq(config.max_rooms, 2048);
            check_str_eq(config.config_file, second_path);
        }
        room_service_app_config_cleanup(&config);
        remove_toml(first_path);
        remove_toml(second_path);
    }
}
