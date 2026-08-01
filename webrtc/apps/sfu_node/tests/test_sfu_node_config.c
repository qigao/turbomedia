#include "sfu_node/config.h"
#include <tinytest.h>

#include <stdlib.h>
#include <string.h>

#ifndef SFU_NODE_CONFIG_EXAMPLE_PATH
#error "SFU_NODE_CONFIG_EXAMPLE_PATH must identify the shipped TOML example"
#endif

static char *write_toml(const char *content) {
    char *path = tt_make_temp_file("sfu_cfg", ".toml");
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

spec("SFU node TOML configuration") {
    it("loads every supported section with typed values") {
        static const char toml[] =
            "[server]\n"
            "host = \"127.0.0.1\"\n"
            "port = 19190\n"
            "use_tls = true\n"
            "cert_file = \"sfu-chain.pem\"\n"
            "key_file = \"sfu-key.pem\"\n"
            "node_id = \"sfu-edge-1\"\n"
            "[capacity]\n"
            "max_rooms = 64\n"
            "default_room_capacity = 24\n"
            "[control]\n"
            "token = \"sfu-token\"\n"
            "[media]\n"
            "access_token = \"media-token\"\n"
            "[auth]\n"
            "issuer = \"sfu-issuer\"\n"
            "active_key_id = \"sfu-key-2026-07\"\n"
            "active_secret = \"sfu-active-secret-at-least-32-bytes\"\n"
            "revoked_token_sha256 = \"0000000000000000000000000000000000000000000000000000000000000000\"\n"
            "[ice]\n"
            "stun_servers = [\"stun:stun-1.example.com:3478\", "
            "\"stun:stun-2.example.com:3478\"]\n"
            "turn_servers = [\"turn:user:secret@turn.example.com:3478\"]\n"
            "allow_loopback = true\n"
            "[runtime]\n"
            "dry_run = true\n"
            "[logging]\n"
            "level = \"debug\"\n";
        sfu_node_app_config_t config;
        char *path = write_toml(toml);

        sfu_node_app_config_init(&config);
        if (path) {
            check_int_eq(sfu_node_app_config_load(&config, path), 0);
            check_str_eq(config.config_file, path);
            check_str_eq(config.bind_host, "127.0.0.1");
            check_int_eq(config.bind_port, 19190);
            check_true(config.use_tls);
            check_str_eq(config.tls_cert_file, "sfu-chain.pem");
            check_str_eq(config.tls_key_file, "sfu-key.pem");
            check_str_eq(config.node_id, "sfu-edge-1");
            check_int_eq(config.max_rooms, 64);
            check_int_eq(config.default_room_capacity, 24);
            check_str_eq(config.control_token, "sfu-token");
            check_str_eq(config.media_access_token, "media-token");
            check_str_eq(
                config.auth_revoked_token_sha256,
                "0000000000000000000000000000000000000000000000000000000000000000");
            check_int_eq(config.stun_server_count, 2);
            check_str_eq(config.stun_servers[0],
                         "stun:stun-1.example.com:3478");
            check_str_eq(config.stun_servers[1],
                         "stun:stun-2.example.com:3478");
            check_int_eq(config.turn_server_count, 1);
            check_str_eq(config.turn_servers[0],
                         "turn:user:secret@turn.example.com:3478");
            check_true(config.ice_allow_loopback);
            check_true(config.dry_run);
            check_str_eq(config.log_level, "debug");
            check_not_null(config.private_data);
        }
        sfu_node_app_config_cleanup(&config);
        remove_toml(path);
    }

    it("loads the shipped example") {
        sfu_node_app_config_t config;

        sfu_node_app_config_init(&config);
        check_int_eq(
            sfu_node_app_config_load(&config, SFU_NODE_CONFIG_EXAMPLE_PATH), 0);
        check_int_eq(sfu_node_app_config_validate(&config), 0);
        sfu_node_app_config_cleanup(&config);
    }

    it("rejects unknown keys without changing the current configuration") {
        static const char toml[] =
            "[server]\n"
            "port = 19000\n"
            "unexpected = true\n";
        sfu_node_app_config_t config;
        char *path = write_toml(toml);

        sfu_node_app_config_init(&config);
        config.bind_port = 17777;
        if (path) {
            check_int_eq(sfu_node_app_config_load(&config, path), -1);
            check_int_eq(config.bind_port, 17777);
            check_null(config.config_file);
            check_null(config.private_data);
        }
        sfu_node_app_config_cleanup(&config);
        remove_toml(path);
    }

    it("rejects malformed, mistyped, and semantically invalid values") {
        static const char malformed[] = "[server\nport = 9190\n";
        static const char mistyped[] = "[capacity]\nmax_rooms = \"many\"\n";
        static const char invalid[] = "[server]\nport = 70000\n";
        sfu_node_app_config_t config;
        char *malformed_path = write_toml(malformed);
        char *mistyped_path = write_toml(mistyped);
        char *invalid_path = write_toml(invalid);

        sfu_node_app_config_init(&config);
        if (malformed_path && mistyped_path && invalid_path) {
            check_int_eq(sfu_node_app_config_load(&config, malformed_path), -1);
            check_int_eq(sfu_node_app_config_load(&config, mistyped_path), -1);
            check_int_eq(sfu_node_app_config_load(&config, invalid_path), -1);
            check_int_eq(config.bind_port, 9190);
            check_int_eq(config.max_rooms, 128);
            check_null(config.private_data);
        }
        sfu_node_app_config_cleanup(&config);
        remove_toml(malformed_path);
        remove_toml(mistyped_path);
        remove_toml(invalid_path);
    }

    it("requires a complete certificate identity when TLS is enabled") {
        static const char missing_key[] =
            "[server]\n"
            "use_tls = true\n"
            "cert_file = \"sfu-chain.pem\"\n";
        static const char missing_cert[] =
            "[server]\n"
            "use_tls = true\n"
            "key_file = \"sfu-key.pem\"\n";
        sfu_node_app_config_t config;
        char *missing_key_path = write_toml(missing_key);
        char *missing_cert_path = write_toml(missing_cert);

        sfu_node_app_config_init(&config);
        if (missing_key_path && missing_cert_path) {
            check_int_eq(
                sfu_node_app_config_load(&config, missing_key_path), -1);
            check_int_eq(
                sfu_node_app_config_load(&config, missing_cert_path), -1);
            check_false(config.use_tls);
            check_null(config.private_data);
        }
        sfu_node_app_config_cleanup(&config);
        remove_toml(missing_key_path);
        remove_toml(missing_cert_path);
    }

    it("rejects invalid ICE server lists transactionally") {
        static const char too_many[] =
            "[ice]\n"
            "stun_servers = [\"stun:a\", \"stun:b\", \"stun:c\", "
            "\"stun:d\", \"stun:e\"]\n";
        static const char wrong_type[] =
            "[ice]\nturn_servers = \"turn:user:secret@turn.example:3478\"\n";
        static const char invalid_url[] =
            "[ice]\nturn_servers = [\"turn:missing-credentials.example:3478\"]\n";
        sfu_node_app_config_t config;
        char *too_many_path = write_toml(too_many);
        char *wrong_type_path = write_toml(wrong_type);
        char *invalid_url_path = write_toml(invalid_url);

        sfu_node_app_config_init(&config);
        if (too_many_path && wrong_type_path && invalid_url_path) {
            check_int_eq(sfu_node_app_config_load(&config, too_many_path), -1);
            check_int_eq(sfu_node_app_config_load(&config, wrong_type_path), -1);
            check_int_eq(sfu_node_app_config_load(&config, invalid_url_path), -1);
            check_int_eq(config.stun_server_count, 0);
            check_int_eq(config.turn_server_count, 0);
            check_null(config.private_data);
        }
        sfu_node_app_config_cleanup(&config);
        remove_toml(too_many_path);
        remove_toml(wrong_type_path);
        remove_toml(invalid_url_path);
    }

    it("preserves owned strings across a partial reload") {
        static const char first[] = "[server]\nnode_id = \"persistent-sfu\"\n";
        static const char second[] = "[capacity]\nmax_rooms = 321\n";
        sfu_node_app_config_t config;
        char *first_path = write_toml(first);
        char *second_path = write_toml(second);

        sfu_node_app_config_init(&config);
        if (first_path && second_path) {
            check_int_eq(sfu_node_app_config_load(&config, first_path), 0);
            check_int_eq(sfu_node_app_config_load(&config, second_path), 0);
            check_str_eq(config.node_id, "persistent-sfu");
            check_int_eq(config.max_rooms, 321);
            check_str_eq(config.config_file, second_path);
        }
        sfu_node_app_config_cleanup(&config);
        remove_toml(first_path);
        remove_toml(second_path);
    }

    it("copies server configuration with independent string ownership") {
        static const char toml[] =
            "[server]\nnode_id = \"owned-node\"\n"
            "[ice]\n"
            "stun_servers = [\"stun:owned.example:3478\"]\n"
            "turn_servers = [\"turn:user:secret@owned.example:3478\"]\n";
        sfu_node_app_config_t source;
        sfu_node_app_config_t copy;
        char *path = write_toml(toml);

        sfu_node_app_config_init(&source);
        sfu_node_app_config_init(&copy);
        if (path) {
            check_int_eq(sfu_node_app_config_load(&source, path), 0);
            check_int_eq(sfu_node_app_config_copy(&copy, &source), 0);
            sfu_node_app_config_cleanup(&source);
            check_str_eq(copy.node_id, "owned-node");
            check_int_eq(copy.stun_server_count, 1);
            check_str_eq(copy.stun_servers[0], "stun:owned.example:3478");
            check_int_eq(copy.turn_server_count, 1);
            check_str_eq(copy.turn_servers[0],
                         "turn:user:secret@owned.example:3478");
        }
        sfu_node_app_config_cleanup(&source);
        sfu_node_app_config_cleanup(&copy);
        remove_toml(path);
    }
}
