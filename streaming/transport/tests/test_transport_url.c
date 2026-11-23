#include "turbo_transport.h"
#include <tinytest.h>

#include <stdlib.h>
#include <string.h>

static void cleanup_config(turbo_transport_config_t *config) {
    free((void *)config->host);
    free((void *)config->path);
    memset(config, 0, sizeof(*config));
}

static void check_rejected(const char *url) {
    turbo_transport_config_t config;

    memset(&config, 0x5a, sizeof(config));
    check_equal(turbo_transport_parse_url(url, &config), -1);
    check_null(config.host);
    check_null(config.path);
    check_equal(config.port, 0);
}

spec("transport URL parsing") {
    describe("valid endpoints") {
        it("applies defaults and preserves path plus query") {
            turbo_transport_config_t config;

            check_equal(turbo_transport_parse_url(
                            "https://media.example/live?id=7", &config),
                        0);
            check_equal(config.type, TURBO_TRANSPORT_HTTP);
            check_equal(config.use_tls, 1);
            check_equal(config.port, 443);
            check(strcmp(config.host, "media.example") == 0);
            check(strcmp(config.path, "/live?id=7") == 0);
            cleanup_config(&config);
        }

        it("accepts the largest valid explicit port") {
            turbo_transport_config_t config;

            check_equal(turbo_transport_parse_url(
                            "tcp://media.example:65535", &config),
                        0);
            check_equal(config.port, 65535);
            cleanup_config(&config);
        }
    }

    describe("invalid endpoints") {
        it("rejects empty, zero, out-of-range, and overflowing ports") {
            check_rejected("tcp://media.example:");
            check_rejected("tcp://media.example:0");
            check_rejected("tcp://media.example:65536");
            check_rejected("tcp://media.example:999999999999999999999999");
        }

        it("rejects truncated components and unsupported authorities") {
            static const char oversized_host[] =
                "https://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.example/";
            static const char oversized_path[] =
                "https://media.example/"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

            check_rejected(oversized_host);
            check_rejected(oversized_path);
            check_rejected("smtp://media.example:25");
            check_rejected("https:///missing-host");
        }
    }
}
