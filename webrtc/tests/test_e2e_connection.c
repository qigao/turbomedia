/**
 * test_e2e_connection.c - End-to-end P2P connection test
 *
 * Tests complete WebRTC connection flow:
 * 1. ICE gathering
 * 2. Candidate exchange
 * 3. DTLS handshake
 * 4. SCTP association
 * 5. DataChannel messaging
 */

#include "tinytest.h"
#include "turbo_datachannel.h"
#include "turbo_media_engine.h"
#include "ice_integration.h"
#include <turbo_coro_context.h>
#include <string.h>
#include <stdio.h>

#define TEST_TIMEOUT_MS 30000
#define MESSAGE_COUNT 10

typedef struct {
    turbo_loop_t *loop;
    salts_timer_t *timeout_timer;
    
    /* Peer A (offerer) */
    turbo_dc_context_t *ctx_a;
    turbo_dc_peer_t *peer_a;
    ice_integration_ctx_t *ice_a;
    turbo_dc_channel_t *channel_a;
    turbo_media_context_t *media_a;
    
    /* Peer B (answerer) */
    turbo_dc_context_t *ctx_b;
    turbo_dc_peer_t *peer_b;
    ice_integration_ctx_t *ice_b;
    turbo_dc_channel_t *channel_b;
    
    /* Credentials exchange */
    char ufrag_a[32], pwd_a[64];
    char ufrag_b[32], pwd_b[64];
    
    /* State */
    int gathering_complete_a;
    int gathering_complete_b;
    int ice_connected_a;
    int ice_connected_b;
    int dtls_connected_a;
    int dtls_connected_b;
    int channel_open_a;
    int channel_open_b;
    
    /* Messaging */
    int messages_sent_a;
    int messages_received_b;
    int messages_sent_b;
    int messages_received_a;
    
    /* Test result */
    int test_complete;
    int test_passed;
    char error_message[256];
} e2e_test_ctx_t;

static e2e_test_ctx_t g_ctx;

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static void fail_test(const char *reason) {
    snprintf(g_ctx.error_message, sizeof(g_ctx.error_message), "%s", reason);
    g_ctx.test_complete = 1;
    g_ctx.test_passed = 0;
}

static void check_test_completion(void) {
    /* Test passes when both peers exchange messages */
    if (g_ctx.messages_received_a >= MESSAGE_COUNT &&
        g_ctx.messages_received_b >= MESSAGE_COUNT) {
        g_ctx.test_complete = 1;
        g_ctx.test_passed = 1;
    }
}

static void on_timeout(salts_timer_t *timer) {
    (void)timer;
    fail_test("Test timeout");
}

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */
static void on_channel_open_a(turbo_dc_channel_t *channel, void *user_data);
static void on_message_a(turbo_dc_channel_t *channel, const void *data, size_t len, int is_binary, void *user_data);
static void on_message_b(turbo_dc_channel_t *channel, const void *data, size_t len, int is_binary, void *user_data);

/* ============================================================================
 * ICE Callbacks
 * ============================================================================ */

static void on_candidate_a(const char *candidate_sdp, void *user_data) {
    (void)user_data;
    
    printf("Peer A candidate: %s\n", candidate_sdp);
    
    /* Trickle to peer B */
    if (g_ctx.ice_b) {
        ice_integration_add_remote_candidate(g_ctx.ice_b, candidate_sdp);
    }
}

static void on_candidate_b(const char *candidate_sdp, void *user_data) {
    (void)user_data;
    
    printf("Peer B candidate: %s\n", candidate_sdp);
    
    /* Trickle to peer A */
    if (g_ctx.ice_a) {
        ice_integration_add_remote_candidate(g_ctx.ice_a, candidate_sdp);
    }
}

static void on_ice_state_a(ice_state_t state, void *user_data) {
    (void)user_data;
    
    printf("Peer A ICE state: %d\n", state);
    
    if (state == ICE_STATE_CONNECTED || state == ICE_STATE_COMPLETED) {
        g_ctx.ice_connected_a = 1;
        printf("Peer A ICE CONNECTED\n");
    } else if (state == ICE_STATE_FAILED) {
        printf("Peer A ICE FAILED\n");
        fail_test("ICE failed on peer A");
    }
}

static void on_ice_state_b(ice_state_t state, void *user_data) {
    (void)user_data;
    
    printf("Peer B ICE state: %d\n", state);
    
    if (state == ICE_STATE_CONNECTED || state == ICE_STATE_COMPLETED) {
        g_ctx.ice_connected_b = 1;
        printf("Peer B ICE CONNECTED\n");
    } else if (state == ICE_STATE_FAILED) {
        printf("Peer B ICE FAILED\n");
        fail_test("ICE failed on peer B");
    }
}

static void on_gathering_complete_a(turbo_ice_agent_t *agent,
                                     ice_gathering_state_t state, void *user_data) {
    (void)agent;
    (void)user_data;
    
    if (state == ICE_GATHERING_COMPLETE) {
        g_ctx.gathering_complete_a = 1;
        
        /* Signal end of candidates to peer B */
        if (g_ctx.ice_b) {
            ice_integration_end_of_candidates(g_ctx.ice_b);
        }
    }
}

static void on_gathering_complete_b(turbo_ice_agent_t *agent,
                                     ice_gathering_state_t state, void *user_data) {
    (void)agent;
    (void)user_data;
    
    if (state == ICE_GATHERING_COMPLETE) {
        g_ctx.gathering_complete_b = 1;
        
        /* Signal end of candidates to peer A */
        if (g_ctx.ice_a) {
            ice_integration_end_of_candidates(g_ctx.ice_a);
        }
    }
}

/* ============================================================================
 * DataChannel Callbacks
 * ============================================================================ */

static void on_peer_state_a(turbo_dc_peer_t *peer, turbo_dc_state_t old_state,
                             turbo_dc_state_t new_state, void *user_data) {
    (void)peer;
    (void)old_state;
    (void)user_data;
    
    if (new_state == TURBO_DC_STATE_CONNECTED) {
        g_ctx.dtls_connected_a = 1;
        
        /* Create and open channel */
        g_ctx.channel_a = turbo_dc_channel_create(g_ctx.peer_a, "test-channel", NULL);
        if (g_ctx.channel_a) {
            turbo_dc_channel_on_open(g_ctx.channel_a, on_channel_open_a);
            turbo_dc_channel_on_message(g_ctx.channel_a, on_message_a);
            turbo_dc_channel_open(g_ctx.channel_a);
        }
    } else if (new_state == TURBO_DC_STATE_FAILED) {
        fail_test("DTLS failed on peer A");
    }
}

static void on_peer_state_b(turbo_dc_peer_t *peer, turbo_dc_state_t old_state,
                             turbo_dc_state_t new_state, void *user_data) {
    (void)peer;
    (void)old_state;
    (void)user_data;
    
    if (new_state == TURBO_DC_STATE_CONNECTED) {
        g_ctx.dtls_connected_b = 1;
    } else if (new_state == TURBO_DC_STATE_FAILED) {
        fail_test("DTLS failed on peer B");
    }
}

static void on_channel_open_a(turbo_dc_channel_t *channel, void *user_data) {
    (void)user_data;
    
    g_ctx.channel_open_a = 1;
    
    /* Start sending messages */
    for (int i = 0; i < MESSAGE_COUNT; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Hello from A #%d", i);
        turbo_dc_channel_send(channel, msg, strlen(msg), 0);
        g_ctx.messages_sent_a++;
    }
}

static void on_channel_open_b(turbo_dc_channel_t *channel, void *user_data) {
    (void)user_data;
    
    g_ctx.channel_open_b = 1;
    
    /* Start sending messages */
    for (int i = 0; i < MESSAGE_COUNT; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Hello from B #%d", i);
        turbo_dc_channel_send(channel, msg, strlen(msg), 0);
        g_ctx.messages_sent_b++;
    }
}

static void on_message_a(turbo_dc_channel_t *channel, const void *data,
                         size_t len, int is_binary, void *user_data) {
    (void)channel;
    (void)is_binary;
    (void)user_data;
    
    g_ctx.messages_received_a++;
    
    /* Verify message format */
    char expected[64];
    snprintf(expected, sizeof(expected), "Hello from B #%d", g_ctx.messages_received_a - 1);
    
    if (len != strlen(expected) || memcmp(data, expected, len) != 0) {
        fail_test("Message content mismatch on peer A");
    }
    
    check_test_completion();
}

static void on_message_b(turbo_dc_channel_t *channel, const void *data,
                         size_t len, int is_binary, void *user_data) {
    (void)channel;
    (void)is_binary;
    (void)user_data;
    
    g_ctx.messages_received_b++;
    
    /* Verify message format */
    char expected[64];
    snprintf(expected, sizeof(expected), "Hello from A #%d", g_ctx.messages_received_b - 1);
    
    if (len != strlen(expected) || memcmp(data, expected, len) != 0) {
        fail_test("Message content mismatch on peer B");
    }
    
    check_test_completion();
}

static void on_incoming_channel_b(turbo_dc_peer_t *peer, turbo_dc_channel_t *channel,
                                   void *user_data) {
    (void)peer;
    (void)user_data;
    
    g_ctx.channel_b = channel;
    
    /* Set callbacks */
    turbo_dc_channel_on_open(channel, on_channel_open_b);
    turbo_dc_channel_on_message(channel, on_message_b);
}

/* ============================================================================
 * Test Setup
 * ============================================================================ */

void setUp(void) {
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.loop = turbo_loop_create();
    
    /* Create timeout timer */
    g_ctx.timeout_timer = salts_timer_create(NULL);
    if (g_ctx.timeout_timer) {
        salts_timer_start(g_ctx.timeout_timer, on_timeout, TEST_TIMEOUT_MS, 0);
    }
}

void tearDown(void) {
    /* Stop timeout */
    if (g_ctx.timeout_timer) {
        salts_timer_stop(g_ctx.timeout_timer);
        salts_timer_destroy(g_ctx.timeout_timer);
        g_ctx.timeout_timer = NULL;
    }
    
    /* Run loop to process timer close */
    turbo_loop_poll(g_ctx.loop, 0, 0);
    
    /* Cleanup channels */
    if (g_ctx.channel_a) {
        turbo_dc_channel_close(g_ctx.channel_a);
        g_ctx.channel_a = NULL;
    }
    if (g_ctx.channel_b) {
        turbo_dc_channel_close(g_ctx.channel_b);
        g_ctx.channel_b = NULL;
    }

    if (g_ctx.media_a) {
        turbo_media_destroy(g_ctx.media_a);
        g_ctx.media_a = NULL;
    }

    /* Quiesce DTLS/SCTP before tearing down ICE underneath it. */
    if (g_ctx.peer_a) {
        turbo_dc_peer_close(g_ctx.peer_a);
    }
    if (g_ctx.peer_b) {
        turbo_dc_peer_close(g_ctx.peer_b);
    }
    for (int i = 0; i < 10; i++) {
        turbo_loop_poll(g_ctx.loop, 0, 0);
        if (g_ctx.ice_a) {
            ice_integration_poll(g_ctx.ice_a);
        }
        if (g_ctx.ice_b) {
            ice_integration_poll(g_ctx.ice_b);
        }
    }
    
    /* Clear ICE agent references from peers BEFORE destroying ICE
     * This prevents use-after-free when peer tries to send via destroyed agent */
    if (g_ctx.peer_a) {
        turbo_dc_peer_set_external_transport(g_ctx.peer_a, NULL, NULL);
    }
    if (g_ctx.peer_b) {
        turbo_dc_peer_set_external_transport(g_ctx.peer_b, NULL, NULL);
    }
    
    /* Cleanup ICE */
    if (g_ctx.ice_a) {
        ice_integration_destroy(g_ctx.ice_a);
        g_ctx.ice_a = NULL;
    }
    if (g_ctx.ice_b) {
        ice_integration_destroy(g_ctx.ice_b);
        g_ctx.ice_b = NULL;
    }
    
    /* Run loop multiple times to ensure all ICE handles are closed
     * ICE agent uses deferred cleanup with pending_closes counter */
    for (int i = 0; i < 10; i++) {
        turbo_loop_poll(g_ctx.loop, 0, 0);
    }
    
    /* Cleanup peers */
    if (g_ctx.peer_a) {
        turbo_dc_peer_destroy(g_ctx.peer_a);
        g_ctx.peer_a = NULL;
    }
    if (g_ctx.peer_b) {
        turbo_dc_peer_destroy(g_ctx.peer_b);
        g_ctx.peer_b = NULL;
    }
    
    /* Cleanup contexts */
    if (g_ctx.ctx_a) {
        turbo_dc_context_destroy(g_ctx.ctx_a);
        g_ctx.ctx_a = NULL;
    }
    if (g_ctx.ctx_b) {
        turbo_dc_context_destroy(g_ctx.ctx_b);
        g_ctx.ctx_b = NULL;
    }
    
    /* Final loop run to cleanup any remaining handles */
    turbo_loop_poll(g_ctx.loop, 0, 0);
    if (g_ctx.loop) {
        turbo_loop_destroy(g_ctx.loop);
        g_ctx.loop = NULL;
    }
}

/* ============================================================================
 * Tests
 * ============================================================================ */

void test_e2e_p2p_connection(void) {
    /* Create peer A (offerer) */
    turbo_dc_config_t config_a = {
        .is_server = 0,
        .transport = TURBO_DC_TRANSPORT_ICE
    };
    g_ctx.ctx_a = turbo_dc_context_create(&config_a);
    check_not_null(g_ctx.ctx_a);
    
    g_ctx.peer_a = turbo_dc_peer_create(g_ctx.ctx_a, NULL, 0, NULL);
    check_not_null(g_ctx.peer_a);
    
    turbo_dc_peer_on_state(g_ctx.peer_a, on_peer_state_a);
    
    /* Create peer B (answerer) */
    turbo_dc_config_t config_b = {
        .is_server = 1,
        .transport = TURBO_DC_TRANSPORT_ICE
    };
    g_ctx.ctx_b = turbo_dc_context_create(&config_b);
    check_not_null(g_ctx.ctx_b);
    
    g_ctx.peer_b = turbo_dc_peer_create(g_ctx.ctx_b, NULL, 0, NULL);
    check_not_null(g_ctx.peer_b);
    
    turbo_dc_peer_on_state(g_ctx.peer_b, on_peer_state_b);
    turbo_dc_peer_on_channel(g_ctx.peer_b, on_incoming_channel_b);
    
    /* Create ICE integration (no STUN servers for local testing) */
    g_ctx.ice_a = ice_integration_create(
        g_ctx.peer_a, g_ctx.loop,
        NULL, 0,  /* No STUN servers */
        NULL, NULL, NULL, 0
    );
    check_not_null(g_ctx.ice_a);
    
    g_ctx.ice_b = ice_integration_create(
        g_ctx.peer_b, g_ctx.loop,
        NULL, 0,  /* No STUN servers */
        NULL, NULL, NULL, 0
    );
    check_not_null(g_ctx.ice_b);
    
    /* Enable loopback candidates for local testing */
    ice_integration_set_allow_loopback(g_ctx.ice_a, 1);
    ice_integration_set_allow_loopback(g_ctx.ice_b, 1);
    
    /* Set ICE callbacks */
    ice_integration_on_candidate(g_ctx.ice_a, on_candidate_a, NULL);
    ice_integration_on_candidate(g_ctx.ice_b, on_candidate_b, NULL);
    ice_integration_on_state_change(g_ctx.ice_a, on_ice_state_a, NULL);
    ice_integration_on_state_change(g_ctx.ice_b, on_ice_state_b, NULL);
    
    /* Exchange credentials */
    ice_integration_get_local_credentials(g_ctx.ice_a, g_ctx.ufrag_a, 
                                          sizeof(g_ctx.ufrag_a), g_ctx.pwd_a, sizeof(g_ctx.pwd_a));
    ice_integration_get_local_credentials(g_ctx.ice_b, g_ctx.ufrag_b,
                                          sizeof(g_ctx.ufrag_b), g_ctx.pwd_b, sizeof(g_ctx.pwd_b));
    
    ice_integration_set_remote_credentials(g_ctx.ice_a, g_ctx.ufrag_b, g_ctx.pwd_b);
    ice_integration_set_remote_credentials(g_ctx.ice_b, g_ctx.ufrag_a, g_ctx.pwd_a);
    
    /* Exchange fingerprints */
    char fp_hash_a[32], fp_a[128];
    char fp_hash_b[32], fp_b[128];
    turbo_dc_context_get_local_fingerprint(g_ctx.ctx_a, fp_hash_a, sizeof(fp_hash_a), 
                                           fp_a, sizeof(fp_a));
    turbo_dc_context_get_local_fingerprint(g_ctx.ctx_b, fp_hash_b, sizeof(fp_hash_b),
                                           fp_b, sizeof(fp_b));
    
    turbo_dc_peer_set_remote_fingerprint(g_ctx.peer_a, fp_hash_b, fp_b);
    turbo_dc_peer_set_remote_fingerprint(g_ctx.peer_b, fp_hash_a, fp_a);
    
    /* Start ICE gathering */
    ice_integration_start_gathering(g_ctx.ice_a);
    ice_integration_start_gathering(g_ctx.ice_b);
    
    /* Set channel callbacks */
    /* Run event loop until test completes */
    while (!g_ctx.test_complete) {
        turbo_loop_poll(g_ctx.loop, 10, 1);
        ice_integration_poll(g_ctx.ice_a);
        ice_integration_poll(g_ctx.ice_b);
    }
    
    /* Verify test passed */
    if (!g_ctx.test_passed) {
        check(0, "%s", (g_ctx.error_message));
    }
    
    check_true(g_ctx.ice_connected_a);
    check_true(g_ctx.ice_connected_b);
    check_true(g_ctx.dtls_connected_a);
    check_true(g_ctx.dtls_connected_b);
    check_true(g_ctx.channel_open_a);
    check_true(g_ctx.channel_open_b);
    check_equal(g_ctx.messages_sent_a, MESSAGE_COUNT);
    check_equal(g_ctx.messages_sent_b, MESSAGE_COUNT);
    check_equal(g_ctx.messages_received_a, MESSAGE_COUNT);
    check_equal(g_ctx.messages_received_b, MESSAGE_COUNT);

    {
        turbo_media_track_config_t track_config = {
            .type = TURBO_RTC_MEDIA_TRACK_AUDIO,
            .direction = TURBO_MEDIA_DIRECTION_SENDONLY,
            .codec = TURBO_CODEC_PCMU,
            .audio = {
                .sample_rate = 8000,
                .channels = 1,
                .bitrate = 64000,
                .frame_size_ms = 20,
            },
        };
        static const uint8_t rtp_packet[] = {
            0x80, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
            0x12, 0x34, 0x56, 0x78, 0x7f,
        };
        turbo_media_track_t *track;

        g_ctx.media_a = turbo_media_create(g_ctx.peer_a, NULL);
        check_not_null(g_ctx.media_a);
        check_equal((int)(turbo_media_setup_srtp(g_ctx.media_a)), (int)(0));
        track = turbo_media_add_track(g_ctx.media_a, &track_config);
        check_not_null(track);
        check_equal((int)(turbo_media_track_start(track)), (int)(0));
        check_equal((int)(turbo_media_track_send_rtp_packet(track, rtp_packet, sizeof(rtp_packet))), (int)(0));
    }
}

/* ============================================================================
 * Main
 * ============================================================================ */

spec("test_e2e_connection") {
  before_each() { setUp(); }
  after_each() { tearDown(); }
  it("test_e2e_p2p_connection") { test_e2e_p2p_connection(); };
}
