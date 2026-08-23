#include <signaling_server/config.h>
#include <tinytest.h>

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define signaling_config_test_strdup _strdup
#else
#define signaling_config_test_strdup strdup
#endif

#ifndef SIGNALING_CONFIG_EXAMPLE_PATH
#error "SIGNALING_CONFIG_EXAMPLE_PATH must identify the shipped TOML example"
#endif

static void signaling_config_test_set_env(const char *name,
                                          const char *value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
}

static char *signaling_config_test_save_env(const char *name) {
    const char *value = getenv(name);
    return value ? signaling_config_test_strdup(value) : NULL;
}

static void signaling_config_test_restore_env(const char *name,
                                              char *saved_value) {
    signaling_config_test_set_env(name, saved_value);
    free(saved_value);
}

static char *write_toml(const char *content) {
    char *path = tt_make_temp_file("tmcfg", ".toml");

    check_not_null(path);
    if (!path) {
        return NULL;
    }
    if (tt_write_file(path, content, strlen(content)) != 0) {
        check_true(0);
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

spec("signaling TOML configuration") {
    it("loads every supported section with typed values") {
        static const char toml[] =
            "[server]\n"
            "node_id = \"edge-1\"\n"
            "host = \"::\"\n"
            "port = 9443\n"
            "use_tls = true\n"
            "cert_file = \"signaling-chain.pem\"\n"
            "key_file = \"signaling-key.pem\"\n"
            "\n"
            "[http_api]\n"
            "enabled = false\n"
            "host = \"127.0.0.1\"\n"
            "port = 9001\n"
            "use_tls = false\n"
            "auth_enabled = true\n"
            "\n"
            "[http_auth]\n"
            "issuer = \"management-issuer\"\n"
            "active_key_id = \"management-key-2026-07\"\n"
            "active_secret = \"management-active-secret-at-least-32-bytes\"\n"
            "previous_key_id = \"management-key-2026-06\"\n"
            "previous_secret = \"management-previous-secret-at-least-32-bytes\"\n"
            "revoked_token_sha256 = \"0000000000000000000000000000000000000000000000000000000000000000\"\n"
            "clock_skew_seconds = 15\n"
            "max_ttl_seconds = 900\n"
            "\n"
            "[limits]\n"
            "max_peers = 2048\n"
            "max_rooms = 256\n"
            "peer_timeout_ms = 45000\n"
            "join_timeout_ms = 12000\n"
            "max_message_size = 131072\n"
            "messages_per_second = 250\n"
            "message_burst = 500\n"
            "max_outbox_messages = 512\n"
            "max_outbox_bytes = 2097152\n"
            "max_connections_per_source = 150\n"
            "source_admissions_per_second = 30\n"
            "source_admission_burst = 75\n"
            "max_source_states = 8192\n"
            "source_state_ttl_ms = 240000\n"
            "\n"
            "[auth]\n"
            "enabled = false\n"
            "issuer = \"peer-issuer\"\n"
            "active_key_id = \"peer-key-2026-07\"\n"
            "secret = \"peer-active-secret-at-least-32-bytes\"\n"
            "previous_key_id = \"peer-key-2026-06\"\n"
            "previous_secret = \"peer-previous-secret-at-least-32-bytes\"\n"
            "revoked_token_sha256 = \"1111111111111111111111111111111111111111111111111111111111111111\"\n"
            "clock_skew_seconds = 20\n"
            "ttl_seconds = 7200\n"
            "algorithm = \"HS256\"\n"
            "\n"
            "[redis]\n"
            "enabled = false\n"
            "host = \"redis.internal\"\n"
            "port = 6380\n"
            "password = \"unused-password\"\n"
            "db = 2\n"
            "use_streams = false\n"
            "stream_read_interval_ms = 250\n"
            "stream_max_len = 4096\n"
            "\n"
            "[logging]\n"
            "level = \"debug\"\n"
            "format = \"text\"\n"
            "output = \"stdout\"\n";
        signaling_server_config_t config;
        char *path = write_toml(toml);

        signaling_server_config_init(&config);
        if (path) {
            check_equal(signaling_server_config_load(&config, path), 0);
            check_equal(config.config_file, path);
            check_equal(config.node_id, "edge-1");
            check_equal(config.ws_host, "::");
            check_equal(config.ws_port, 9443);
            check_true(config.ws_use_tls);
            check_equal(config.ws_cert_file, "signaling-chain.pem");
            check_equal(config.ws_key_file, "signaling-key.pem");
            check_false(config.http_enabled);
            check_equal(config.http_host, "127.0.0.1");
            check_equal(config.http_port, 9001);
            check_true(config.http_auth_enabled);
            check_equal(config.http_auth_issuer, "management-issuer");
            check_equal(config.http_auth_active_key_id,
                         "management-key-2026-07");
            check_equal(config.http_auth_previous_key_id,
                         "management-key-2026-06");
            check_equal(
                config.http_auth_revoked_token_sha256,
                "0000000000000000000000000000000000000000000000000000000000000000");
            check_equal(config.http_auth_clock_skew_seconds, 15);
            check_equal(config.http_auth_max_ttl_seconds, 900);
            check_equal(config.max_peers, 2048);
            check_equal(config.max_rooms, 256);
            check_equal(config.peer_timeout_ms, 45000);
            check_equal(config.join_timeout_ms, 12000);
            check_equal(config.max_message_size, 131072);
            check_equal(config.messages_per_second, 250);
            check_equal(config.message_burst, 500);
            check_equal(config.max_outbox_messages, 512);
            check_equal(config.max_outbox_bytes, 2097152);
            check_equal(config.max_connections_per_source, 150);
            check_equal(config.source_admissions_per_second, 30);
            check_equal(config.source_admission_burst, 75);
            check_equal(config.max_source_states, 8192);
            check_equal(config.source_state_ttl_ms, 240000);
            check_equal(
                config.jwt_revoked_token_sha256,
                "1111111111111111111111111111111111111111111111111111111111111111");
            check_equal(config.jwt_ttl_seconds, 7200);
            check_equal(config.jwt_issuer, "peer-issuer");
            check_equal(config.jwt_active_key_id, "peer-key-2026-07");
            check_equal(config.jwt_secret,
                         "peer-active-secret-at-least-32-bytes");
            check_equal(config.jwt_previous_key_id, "peer-key-2026-06");
            check_equal(config.jwt_clock_skew_seconds, 20);
            check_equal(config.redis_host, "redis.internal");
            check_equal(config.redis_port, 6380);
            check_equal(config.redis_db, 2);
            check_false(config.redis_use_streams);
            check_equal(config.log_level, "debug");
            check_not_null(config.private_data);
        }
        signaling_server_config_cleanup(&config);
        remove_toml(path);
    }

    it("loads the shipped example") {
        signaling_server_config_t config;

        signaling_server_config_init(&config);
        check_equal(
            signaling_server_config_load(&config, SIGNALING_CONFIG_EXAMPLE_PATH), 0);
        check_equal(signaling_server_config_validate(&config), 0);
        signaling_server_config_cleanup(&config);
    }

    it("rejects unknown keys without changing the current configuration") {
        static const char field_toml[] =
            "[server]\n"
            "port = 9000\n"
            "unexpected = true\n";
        static const char section_toml[] =
            "[unexpected]\n"
            "enabled = true\n";
        signaling_server_config_t config;
        char *field_path = write_toml(field_toml);
        char *section_path = write_toml(section_toml);

        signaling_server_config_init(&config);
        config.ws_port = 7777;
        if (field_path && section_path) {
            check_equal(signaling_server_config_load(&config, field_path), -1);
            check_equal(signaling_server_config_load(&config, section_path), -1);
            check_equal(config.ws_port, 7777);
            check_null(config.config_file);
            check_null(config.private_data);
        }
        signaling_server_config_cleanup(&config);
        remove_toml(field_path);
        remove_toml(section_path);
    }

    it("rejects values with the wrong TOML type") {
        static const char toml[] =
            "[limits]\n"
            "max_peers = \"many\"\n";
        signaling_server_config_t config;
        char *path = write_toml(toml);

        signaling_server_config_init(&config);
        if (path) {
            check_equal(signaling_server_config_load(&config, path), -1);
            check_equal(config.max_peers, 1000);
        }
        signaling_server_config_cleanup(&config);
        remove_toml(path);
    }

    it("rejects malformed TOML without changing the current configuration") {
        static const char toml[] =
            "[server\n"
            "port = 9000\n";
        signaling_server_config_t config;
        char *path = write_toml(toml);

        signaling_server_config_init(&config);
        config.ws_port = 7777;
        if (path) {
            check_equal(signaling_server_config_load(&config, path), -1);
            check_equal(config.ws_port, 7777);
            check_null(config.private_data);
        }
        signaling_server_config_cleanup(&config);
        remove_toml(path);
    }

    it("rejects a missing configuration file") {
        signaling_server_config_t config;
        char *path = tt_make_temp_file("tmcfg_missing", ".toml");

        signaling_server_config_init(&config);
        check_not_null(path);
        if (path) {
            check_equal(tt_remove_file(path), 0);
            check_equal(signaling_server_config_load(&config, path), -1);
            check_null(config.config_file);
            check_null(config.private_data);
            free(path);
        }
        signaling_server_config_cleanup(&config);
    }

    it("rejects semantically invalid values") {
        static const char toml[] =
            "[limits]\n"
            "peer_timeout_ms = 0\n";
        signaling_server_config_t config;
        char *path = write_toml(toml);

        signaling_server_config_init(&config);
        if (path) {
            check_equal(signaling_server_config_load(&config, path), -1);
            check_equal(config.peer_timeout_ms, 60000);
        }
        signaling_server_config_cleanup(&config);
        remove_toml(path);
    }

    it("rejects inconsistent signaling resource bounds transactionally") {
        static const char toml[] =
            "[limits]\n"
            "max_message_size = 131072\n"
            "max_outbox_bytes = 65536\n";
        signaling_server_config_t config;
        char *path = write_toml(toml);

        signaling_server_config_init(&config);
        if (path) {
            check_equal(signaling_server_config_load(&config, path), -1);
            check_equal(config.max_message_size, 65536);
            check_equal(config.max_outbox_bytes, 1048576);
        }
        signaling_server_config_cleanup(&config);
        remove_toml(path);
    }

    it("rejects an unbounded source admission state lifetime") {
        static const char toml[] =
            "[limits]\n"
            "source_state_ttl_ms = 0\n";
        signaling_server_config_t config;
        char *path = write_toml(toml);

        signaling_server_config_init(&config);
        if (path) {
            check_equal(signaling_server_config_load(&config, path), -1);
            check_equal(config.source_state_ttl_ms, 300000);
            check_equal(config.max_source_states, 4096);
        }
        signaling_server_config_cleanup(&config);
        remove_toml(path);
    }

    it("allows source admission policy to be disabled explicitly") {
        signaling_server_config_t config;

        signaling_server_config_init(&config);
        config.max_connections_per_source = 0;
        config.source_admissions_per_second = 0;
        config.source_admission_burst = 0;
        config.max_source_states = 0;
        config.source_state_ttl_ms = 0;
        check_equal(signaling_server_config_validate(&config), 0);
        signaling_server_config_cleanup(&config);
    }

    it("preserves owned values across a partial reload") {
        static const char first_toml[] =
            "[server]\n"
            "node_id = \"persistent-node\"\n";
        static const char second_toml[] =
            "[limits]\n"
            "max_rooms = 321\n";
        signaling_server_config_t config;
        char *first_path = write_toml(first_toml);
        char *second_path = write_toml(second_toml);

        signaling_server_config_init(&config);
        if (first_path && second_path) {
            check_equal(signaling_server_config_load(&config, first_path), 0);
            check_equal(signaling_server_config_load(&config, second_path), 0);
            check_equal(config.node_id, "persistent-node");
            check_equal(config.max_rooms, 321);
            check_equal(config.config_file, second_path);
        }
        signaling_server_config_cleanup(&config);
        remove_toml(first_path);
        remove_toml(second_path);
    }

    it("accepts complete peer admission auth and rejects incomplete keys") {
        static const char valid_auth_toml[] =
            "[auth]\n"
            "enabled = true\n"
            "issuer = \"turbomedia\"\n"
            "active_key_id = \"peer-key-2026-07\"\n"
            "secret = \"peer-active-secret-at-least-32-bytes\"\n"
            "ttl_seconds = 3600\n"
            "algorithm = \"HS256\"\n";
        static const char invalid_auth_toml[] =
            "[auth]\n"
            "enabled = true\n"
            "issuer = \"turbomedia\"\n"
            "active_key_id = \"peer-key-2026-07\"\n"
            "secret = \"too-short\"\n";
        signaling_server_config_t config;
        char *valid_path = write_toml(valid_auth_toml);
        char *invalid_path = write_toml(invalid_auth_toml);

        signaling_server_config_init(&config);
        if (valid_path && invalid_path) {
            check_equal(signaling_server_config_load(&config, valid_path), 0);
            signaling_server_config_cleanup(&config);
            signaling_server_config_init(&config);
            check_equal(signaling_server_config_load(&config, invalid_path), -1);
        }
        signaling_server_config_cleanup(&config);
        remove_toml(valid_path);
        remove_toml(invalid_path);
    }

    it("fails fast when reserved Redis integration is enabled") {
        static const char redis_toml[] =
            "[redis]\n"
            "enabled = true\n";
        signaling_server_config_t config;
        char *redis_path = write_toml(redis_toml);

        signaling_server_config_init(&config);
        if (redis_path) {
            check_equal(signaling_server_config_load(&config, redis_path), -1);
        }
        signaling_server_config_cleanup(&config);
        remove_toml(redis_path);
    }

    it("accepts complete TLS identities and rejects incomplete ones") {
        static const char valid[] =
            "[server]\n"
            "use_tls = true\n"
            "cert_file = \"server.crt\"\n"
            "key_file = \"server.key\"\n"
            "[http_api]\n"
            "enabled = true\n"
            "host = \"127.0.0.1\"\n"
            "port = 9444\n"
            "use_tls = true\n"
            "cert_file = \"management.crt\"\n"
            "key_file = \"management.key\"\n"
            "auth_enabled = true\n";
        static const char invalid_ws[] =
            "[server]\n"
            "use_tls = true\n"
            "cert_file = \"server.crt\"\n";
        static const char invalid_http[] =
            "[http_api]\n"
            "enabled = true\n"
            "use_tls = true\n"
            "cert_file = \"management.crt\"\n"
            "auth_enabled = true\n";
        signaling_server_config_t config;
        char *valid_path = write_toml(valid);
        char *invalid_ws_path = write_toml(invalid_ws);
        char *invalid_http_path = write_toml(invalid_http);

        signaling_server_config_init(&config);
        config.http_admin_token = "test-admin-token";
        if (valid_path && invalid_ws_path && invalid_http_path) {
            check_equal(
                signaling_server_config_load(&config, valid_path), 0);
            signaling_server_config_cleanup(&config);
            signaling_server_config_init(&config);
            config.http_admin_token = "test-admin-token";
            check_equal(
                signaling_server_config_load(&config, invalid_ws_path), -1);
            check_equal(
                signaling_server_config_load(&config, invalid_http_path), -1);
        }
        signaling_server_config_cleanup(&config);
        remove_toml(valid_path);
        remove_toml(invalid_ws_path);
        remove_toml(invalid_http_path);
    }

    it("requires complete static or scoped management authentication") {
        signaling_server_config_t config;

        signaling_server_config_init(&config);
        config.http_enabled = 1;
        config.http_host = "0.0.0.0";
        config.http_auth_enabled = 1;
        config.http_admin_token = "test-admin-token";
        check_equal(signaling_server_config_validate(&config), 0);

        config.http_admin_token = NULL;
        check_equal(signaling_server_config_validate(&config), -1);
        config.http_auth_active_key_id = "management-key";
        config.http_auth_active_secret =
            "management-active-secret-at-least-32-bytes";
        check_equal(signaling_server_config_validate(&config), 0);
        config.http_auth_active_secret = "too-short";
        check_equal(signaling_server_config_validate(&config), -1);
        config.http_auth_active_key_id = NULL;
        config.http_auth_active_secret = NULL;
        config.http_admin_token = "test-admin-token";
        config.http_auth_enabled = 0;
        check_equal(signaling_server_config_validate(&config), -1);
        signaling_server_config_cleanup(&config);
    }

    it("applies TLS environment overrides and rejects invalid booleans") {
        static const char *const names[] = {
            "TURBO_SIGNALING_ADMIN_TOKEN",
            "TURBO_SIGNALING_USE_TLS",
            "TURBO_SIGNALING_TLS_CERT_FILE",
            "TURBO_SIGNALING_TLS_KEY_FILE",
            "TURBO_SIGNALING_HTTP_USE_TLS",
            "TURBO_SIGNALING_HTTP_TLS_CERT_FILE",
            "TURBO_SIGNALING_HTTP_TLS_KEY_FILE",
            "TURBO_SIGNALING_HTTP_AUTH_ACTIVE_KEY_ID",
            "TURBO_SIGNALING_HTTP_AUTH_ACTIVE_SECRET",
            "TURBO_SIGNALING_HTTP_AUTH_MAX_TTL_SECONDS"
        };
        char *saved[sizeof(names) / sizeof(names[0])] = {0};
        signaling_server_config_t config;

        for (size_t index = 0; index < sizeof(names) / sizeof(names[0]);
             ++index) {
            saved[index] = signaling_config_test_save_env(names[index]);
        }
        signaling_config_test_set_env(names[0], "env-admin-token");
        signaling_config_test_set_env(names[1], "true");
        signaling_config_test_set_env(names[2], "signaling-chain.pem");
        signaling_config_test_set_env(names[3], "signaling-key.pem");
        signaling_config_test_set_env(names[4], "1");
        signaling_config_test_set_env(names[5], "management-chain.pem");
        signaling_config_test_set_env(names[6], "management-key.pem");
        signaling_config_test_set_env(names[7], "env-management-key");
        signaling_config_test_set_env(
            names[8], "env-management-secret-at-least-32-bytes");
        signaling_config_test_set_env(names[9], "900");

        signaling_server_config_init(&config);
        config.http_enabled = 1;
        signaling_server_config_apply_environment(&config);
        check_true(config.ws_use_tls);
        check_equal(config.ws_cert_file, "signaling-chain.pem");
        check_equal(config.ws_key_file, "signaling-key.pem");
        check_true(config.http_use_tls);
        check_equal(config.http_cert_file, "management-chain.pem");
        check_equal(config.http_key_file, "management-key.pem");
        check_equal(config.http_admin_token, "env-admin-token");
        check_equal(config.http_auth_active_key_id, "env-management-key");
        check_equal(config.http_auth_active_secret,
                     "env-management-secret-at-least-32-bytes");
        check_equal(config.http_auth_max_ttl_seconds, 900);
        check_equal(signaling_server_config_validate(&config), 0);

        signaling_config_test_set_env(names[1], "not-a-boolean");
        signaling_server_config_apply_environment(&config);
        check_equal(signaling_server_config_validate(&config), -1);
        signaling_server_config_cleanup(&config);

        for (size_t index = 0; index < sizeof(names) / sizeof(names[0]);
             ++index) {
            signaling_config_test_restore_env(names[index], saved[index]);
        }
    }
}
