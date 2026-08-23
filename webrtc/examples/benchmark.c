/**
 * benchmark.c - WebRTC DataChannel performance benchmark
 *
 * Tests:
 * 1. Throughput - How many MB/s can we push through?
 * 2. Latency - Round-trip time for small messages
 *
 * Usage:
 *   Terminal 1: ./benchmark server 127.0.0.1 5000
 *   Terminal 2: ./benchmark client 127.0.0.1 5000
 */

#include "turbo_datachannel.h"
#include "turbo_datachannel_errors.h"
#include "tlog.h"
#include <platform.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#define TEST_DURATION_SEC 10
#define PING_COUNT 1000
#define THROUGHPUT_CHUNK_SIZE (64 * 1024)  /* 64KB chunks */

typedef enum {
    TEST_LATENCY,
    TEST_THROUGHPUT
} test_type_t;

typedef struct {
    turbo_dc_context_t *ctx;
    turbo_dc_peer_t *peer;
    turbo_dc_channel_t *channel;
    int is_server;

    /* Test state */
    test_type_t current_test;
    int test_running;

    /* Latency test */
    int ping_count;
    int pongs_received;
    uint64_t latency_sum_us;
    uint64_t ping_start_time;

    /* Throughput test */
    size_t bytes_sent;
    size_t bytes_received;
    uint64_t throughput_start_time;
    uint64_t last_stats_print_time;
    uint64_t throughput_resume_time;
    int throughput_started;
} benchmark_state_t;

/* ============================================================================
 * Utilities
 * ============================================================================ */

static uint64_t get_time_us(void) {
    return turbo_monotonic_ms() * 1000ULL;
}

static void print_stats(benchmark_state_t *bench) {
    uint64_t elapsed_us = get_time_us() - bench->throughput_start_time;
    double elapsed_sec = elapsed_us / 1000000.0;

    if (bench->is_server) {
        double mbps = (bench->bytes_received * 8.0) / (elapsed_sec * 1000000.0);
        /* Keep printf for dynamic progress updates as log_info doesn't handle \r well */
        printf("\r[SERVER] Received: %.2f MB, Rate: %.2f Mbps",
               bench->bytes_received / (1024.0 * 1024.0), mbps);
    } else {
        double mbps = (bench->bytes_sent * 8.0) / (elapsed_sec * 1000000.0);
        printf("\r[CLIENT] Sent: %.2f MB, Rate: %.2f Mbps",
               bench->bytes_sent / (1024.0 * 1024.0), mbps);
    }
    fflush(stdout);
}

/* ============================================================================
 * Test: Latency
 * ============================================================================ */

static void send_ping(benchmark_state_t *bench) {
    if (bench->ping_count >= PING_COUNT) {
        /* Test complete */
        bench->test_running = 0;
        bench->throughput_resume_time = get_time_us() + 2000000ULL;

        double avg_latency_ms = (bench->latency_sum_us / (double)bench->pongs_received) / 1000.0;

        TLOG_INFO("=== Latency Test Results ===");
        TLOG_INFOF("Pings sent: {}", bench->ping_count);
        TLOG_INFOF("Pongs received: {}", bench->pongs_received);
        TLOG_INFOF("Average RTT: {} ms", avg_latency_ms);

        return;
    }

    bench->ping_start_time = get_time_us();
    const char *msg = "PING";
    turbo_dc_channel_send(bench->channel, msg, strlen(msg), 0);
    bench->ping_count++;
}

static void handle_latency_message(benchmark_state_t *bench, const void *data, size_t len) {
    if (bench->is_server) {
        /* Server echoes back */
        turbo_dc_channel_send(bench->channel, data, len, 0);
    } else {
        /* Client measures RTT */
        if (len == 4 && memcmp(data, "PING", 4) == 0) {
            uint64_t rtt_us = get_time_us() - bench->ping_start_time;
            bench->latency_sum_us += rtt_us;
            bench->pongs_received++;

            /* Send next ping */
            send_ping(bench);
        }
    }
}

/* ============================================================================
 * Test: Throughput
 * ============================================================================ */

static void send_throughput_data(benchmark_state_t *bench) {
    uint8_t buffer[THROUGHPUT_CHUNK_SIZE];
    memset(buffer, 0xAB, sizeof(buffer));

    if (turbo_dc_channel_send(bench->channel, buffer, sizeof(buffer), 1) == 0) {
        bench->bytes_sent += sizeof(buffer);
    }
}

static void update_throughput_test(benchmark_state_t *bench, uint64_t now_us) {
    if (!bench->test_running) {
        return;
    }

    if (!bench->throughput_started) {
        return;
    }

    if (!bench->is_server && now_us >= bench->throughput_resume_time) {
        send_throughput_data(bench);
    }

    if (now_us - bench->last_stats_print_time >= 1000000ULL) {
        bench->last_stats_print_time = now_us;
        print_stats(bench);
    }

    if (now_us - bench->throughput_start_time <= TEST_DURATION_SEC * 1000000ULL) {
        return;
    }

    bench->test_running = 0;
    print_stats(bench);

    TLOG_INFO("=== Throughput Test Results ===");
    if (bench->is_server) {
        double mbps = (bench->bytes_received * 8.0) / (TEST_DURATION_SEC * 1000000.0);
        TLOG_INFOF("Total received: {} MB", bench->bytes_received / (1024.0 * 1024.0));
        TLOG_INFOF("Throughput: {} Mbps", mbps);
    } else {
        double mbps = (bench->bytes_sent * 8.0) / (TEST_DURATION_SEC * 1000000.0);
        TLOG_INFOF("Total sent: {} MB", bench->bytes_sent / (1024.0 * 1024.0));
        TLOG_INFOF("Throughput: {} Mbps", mbps);
    }

    turbo_dc_peer_close(bench->peer);
}

static void start_throughput_test(benchmark_state_t *bench) {
    bench->bytes_sent = 0;
    bench->bytes_received = 0;
    bench->throughput_start_time = get_time_us();
    bench->last_stats_print_time = bench->throughput_start_time;
    bench->test_running = 1;
    bench->throughput_started = 1;

    TLOG_INFOF("=== Starting Throughput Test ({} seconds) ===", TEST_DURATION_SEC);
}

static void handle_throughput_message(benchmark_state_t *bench, const void *data, size_t len) {
    (void)data;
    if (bench->is_server) {
        bench->bytes_received += len;
    }
}

/* ============================================================================
 * Callbacks
 * ============================================================================ */

static void on_channel_message(turbo_dc_channel_t *channel, const void *data,
                               size_t len, int is_binary, void *user_data) {
    benchmark_state_t *bench = (benchmark_state_t *)user_data;
    (void)channel;
    (void)is_binary;

    switch (bench->current_test) {
        case TEST_LATENCY:
            handle_latency_message(bench, data, len);
            break;
        case TEST_THROUGHPUT:
            handle_throughput_message(bench, data, len);
            break;
    }
}

static void on_channel_open(turbo_dc_channel_t *channel, void *user_data) {
    benchmark_state_t *bench = (benchmark_state_t *)user_data;

    TLOG_INFOF("Channel '{}' opened! ({})",
           turbo_dc_channel_get_label(channel),
           bench->is_server ? "SERVER" : "CLIENT");

    if (!bench->is_server) {
        TLOG_INFO("=== Starting Benchmark ===");

        /* Test 1: Latency */
        bench->current_test = TEST_LATENCY;
        bench->test_running = 1;
        bench->ping_count = 0;
        bench->pongs_received = 0;
        bench->latency_sum_us = 0;

        TLOG_INFOF("=== Starting Latency Test ({} pings) ===", PING_COUNT);
        send_ping(bench);

    } else {
        TLOG_INFO("Ready for benchmarks...");
    }
}

static void on_peer_channel(turbo_dc_peer_t *peer, turbo_dc_channel_t *channel,
                            void *user_data) {
    benchmark_state_t *bench = (benchmark_state_t *)user_data;
    (void)peer;

    TLOG_INFOF("Incoming channel: {}", turbo_dc_channel_get_label(channel));

    bench->channel = channel;
    turbo_dc_channel_on_message(channel, on_channel_message);
}

static void on_peer_state(turbo_dc_peer_t *peer, turbo_dc_state_t old_state,
                          turbo_dc_state_t new_state, void *user_data) {
    benchmark_state_t *bench = (benchmark_state_t *)user_data;
    (void)peer;
    (void)old_state;

    if (new_state == TURBO_DC_STATE_CONNECTED) {
        TLOG_INFOF("Peer connected! ({})", bench->is_server ? "SERVER" : "CLIENT");

        if (!bench->is_server) {
            turbo_dc_channel_config_t config = turbo_dc_default_channel_config();
            config.ordered = 0;  /* Unordered for max throughput */

            bench->channel = turbo_dc_channel_create(bench->peer, "benchmark", &config);
            if (!bench->channel) {
                TLOG_ERROR("Failed to create channel");
                return;
            }

            turbo_dc_channel_on_open(bench->channel, on_channel_open);
            turbo_dc_channel_on_message(bench->channel, on_channel_message);

            if (turbo_dc_channel_open(bench->channel) != 0) {
                turbo_dc_error_t err = turbo_dc_peer_get_error(bench->peer);
                TLOG_ERRORF("Failed to open channel: {}", turbo_dc_error_string(err.code));
            }
        }
    }
}

static void on_peer_error(turbo_dc_peer_t *peer, int error_code,
                          const char *error_msg, void *user_data) {
    (void)peer;
    (void)user_data;
    TLOG_ERRORF("Error {}: {}", error_code, error_msg);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char **argv) {
    if (argc != 4) {
        TLOG_ERRORF("Usage: {} <server|client> <host> <port>", argv[0]);
        return 1;
    }

    const char *mode = argv[1];
    const char *host = argv[2];
    int port = atoi(argv[3]);

    int is_server = (strcmp(mode, "server") == 0);

    TLOG_INFO("=== WebRTC DataChannel Benchmark ===");
    TLOG_INFOF("Mode: {}", is_server ? "SERVER" : "CLIENT");
    TLOG_INFOF("Address: {}:{}", host, port);

    benchmark_state_t bench = {0};
    bench.is_server = is_server;
    turbo_dc_config_t config = {
        .is_server = is_server,
        .cert_pem = NULL,
        .key_pem = NULL
    };

    bench.ctx = turbo_dc_context_create(&config);
    if (!bench.ctx) {
        TLOG_ERROR("Failed to create context");
        return 1;
    }

    bench.peer = turbo_dc_peer_create(bench.ctx, host, (uint16_t)port, &bench);
    if (!bench.peer) {
        turbo_dc_error_t err = turbo_dc_context_get_error(bench.ctx);
        TLOG_ERRORF("Failed to create peer: {}", turbo_dc_error_string(err.code));
        turbo_dc_context_destroy(bench.ctx);
        return 1;
    }

    turbo_dc_peer_on_state(bench.peer, on_peer_state);
    turbo_dc_peer_on_channel(bench.peer, on_peer_channel);
    turbo_dc_peer_on_error(bench.peer, on_peer_error);

    if (turbo_dc_peer_connect(bench.peer) != 0) {
        turbo_dc_error_t err = turbo_dc_peer_get_error(bench.peer);
        TLOG_ERRORF("Failed to connect: {}", turbo_dc_error_string(err.code));
        turbo_dc_peer_destroy(bench.peer);
        turbo_dc_context_destroy(bench.ctx);
        return 1;
    }

    TLOG_INFO("Connecting...");

    while (turbo_dc_peer_get_state(bench.peer) != TURBO_DC_STATE_CLOSED &&
           turbo_dc_peer_get_state(bench.peer) != TURBO_DC_STATE_FAILED) {
        uint64_t now_us = get_time_us();

        if (!bench.throughput_started &&
            bench.throughput_resume_time != 0 &&
            now_us >= bench.throughput_resume_time) {
            start_throughput_test(&bench);
        }

        update_throughput_test(&bench, now_us);
        turbo_sleep_ms(1);
    }

    turbo_dc_peer_destroy(bench.peer);
    turbo_dc_context_destroy(bench.ctx);

    TLOG_INFO("Benchmark complete!");

    return 0;
}
