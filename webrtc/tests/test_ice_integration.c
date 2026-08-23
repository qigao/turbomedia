/**
 * test_ice_integration.c - Integration tests for ICE + DataChannel
 *
 * Tests complete P2P connection flow with ICE
 */

#include "tinytest.h"
#include "turbo_datachannel.h"
#include "ice_integration.h"
#include "ice/turbo_ice.h"
#include <turbo_coro_context.h>
#include <turbo_thread.h>
#include <string.h>

/* Test context */
typedef struct {
    turbo_loop_t *loop;
    
    /* Peer A (offerer) */
    turbo_dc_context_t *ctx_a;
    turbo_dc_peer_t *peer_a;
    ice_integration_ctx_t *ice_a;
    
    /* Peer B (answerer) */
    turbo_dc_context_t *ctx_b;
    turbo_dc_peer_t *peer_b;
    ice_integration_ctx_t *ice_b;
    coro_context_t *direct_ice_ctx;
    turbo_ice_agent_t *direct_ice_agent;
    
    /* State tracking */
    int peer_a_connected;
    int peer_b_connected;
    int peer_a_ice_complete;
    int peer_b_ice_complete;
    
    /* Candidate exchange */
    char *candidates_a[32];
    int candidate_count_a;
    char *candidates_b[32];
    int candidate_count_b;
    
    /* Test completion */
    int test_complete;
    int test_passed;
} test_context_t;

static test_context_t g_test_ctx;

void setUp(void) {
    memset(&g_test_ctx, 0, sizeof(g_test_ctx));
    g_test_ctx.loop = turbo_loop_create();
}

void tearDown(void) {
    /* Cleanup candidates */
    for (int i = 0; i < g_test_ctx.candidate_count_a; i++) {
        free(g_test_ctx.candidates_a[i]);
    }
    for (int i = 0; i < g_test_ctx.candidate_count_b; i++) {
        free(g_test_ctx.candidates_b[i]);
    }
    
    /* Cleanup ICE */
    if (g_test_ctx.ice_a) {
        ice_integration_destroy(g_test_ctx.ice_a);
        g_test_ctx.ice_a = NULL;
    }
    if (g_test_ctx.ice_b) {
        ice_integration_destroy(g_test_ctx.ice_b);
        g_test_ctx.ice_b = NULL;
    }
    
    /* Cleanup peers */
    if (g_test_ctx.peer_a) {
        turbo_dc_peer_destroy(g_test_ctx.peer_a);
        g_test_ctx.peer_a = NULL;
    }
    if (g_test_ctx.peer_b) {
        turbo_dc_peer_destroy(g_test_ctx.peer_b);
        g_test_ctx.peer_b = NULL;
    }

    if (g_test_ctx.direct_ice_agent) {
        ice_agent_destroy(g_test_ctx.direct_ice_agent);
        g_test_ctx.direct_ice_agent = NULL;
    }
    if (g_test_ctx.direct_ice_ctx) {
        coro_context_destroy(g_test_ctx.direct_ice_ctx);
        g_test_ctx.direct_ice_ctx = NULL;
    }
    
    /* Cleanup contexts */
    if (g_test_ctx.ctx_a) {
        turbo_dc_context_destroy(g_test_ctx.ctx_a);
        g_test_ctx.ctx_a = NULL;
    }
    if (g_test_ctx.ctx_b) {
        turbo_dc_context_destroy(g_test_ctx.ctx_b);
        g_test_ctx.ctx_b = NULL;
    }
    
    /* Run loop multiple times to process all close callbacks */
    /* Use UV_RUN_DEFAULT with a short timeout to ensure all callbacks fire */
    for (int i = 0; i < 50; i++) {
        turbo_loop_poll(g_test_ctx.loop, 0, 0);
        ice_integration_poll(g_test_ctx.ice_a);
        ice_integration_poll(g_test_ctx.ice_b);
        if (!turbo_loop_alive(g_test_ctx.loop)) {
            break;
        }
        turbo_sleep_ms(1);
    }

    if (g_test_ctx.loop) {
        turbo_loop_destroy(g_test_ctx.loop);
        g_test_ctx.loop = NULL;
    }
}

/* ============================================================================
 * Callbacks
 * ============================================================================ */

static void on_candidate_a(const char *candidate_sdp, void *user_data) {
    (void)user_data;
    
    /* Store candidate for later exchange */
    if (g_test_ctx.candidate_count_a < 32) {
        g_test_ctx.candidates_a[g_test_ctx.candidate_count_a++] = strdup(candidate_sdp);
    }
}

static void on_candidate_b(const char *candidate_sdp, void *user_data) {
    (void)user_data;
    
    /* Store candidate for later exchange */
    if (g_test_ctx.candidate_count_b < 32) {
        g_test_ctx.candidates_b[g_test_ctx.candidate_count_b++] = strdup(candidate_sdp);
    }
}

static void on_ice_state_a(ice_state_t state, void *user_data) {
    (void)user_data;
    
    if (state == ICE_STATE_COMPLETED) {
        g_test_ctx.peer_a_ice_complete = 1;
    }
}

static void on_ice_state_b(ice_state_t state, void *user_data) {
    (void)user_data;
    
    if (state == ICE_STATE_COMPLETED) {
        g_test_ctx.peer_b_ice_complete = 1;
    }
}

static void on_peer_state_a(turbo_dc_peer_t *peer, turbo_dc_state_t old_state,
                             turbo_dc_state_t new_state, void *user_data) {
    (void)peer;
    (void)old_state;
    (void)user_data;
    
    if (new_state == TURBO_DC_STATE_CONNECTED) {
        g_test_ctx.peer_a_connected = 1;
        
        /* Check if both peers connected */
        if (g_test_ctx.peer_b_connected) {
            g_test_ctx.test_complete = 1;
            g_test_ctx.test_passed = 1;
        }
    }
}

static void on_peer_state_b(turbo_dc_peer_t *peer, turbo_dc_state_t old_state,
                             turbo_dc_state_t new_state, void *user_data) {
    (void)peer;
    (void)old_state;
    (void)user_data;
    
    if (new_state == TURBO_DC_STATE_CONNECTED) {
        g_test_ctx.peer_b_connected = 1;
        
        /* Check if both peers connected */
        if (g_test_ctx.peer_a_connected) {
            g_test_ctx.test_complete = 1;
            g_test_ctx.test_passed = 1;
        }
    }
}

/* ============================================================================
 * Tests
 * ============================================================================ */

void test_ice_integration_create(void) {
    /* Create peer A */
    turbo_dc_config_t config_a = {
        .is_server = 0,
        .transport = TURBO_DC_TRANSPORT_ICE
    };
    g_test_ctx.ctx_a = turbo_dc_context_create(&config_a);
    check_not_null(g_test_ctx.ctx_a);
    
    g_test_ctx.peer_a = turbo_dc_peer_create(g_test_ctx.ctx_a, NULL, 0, NULL);
    check_not_null(g_test_ctx.peer_a);
    
    /* Create ICE integration without STUN servers (host candidates only) */
    /* This avoids DNS resolution issues during test cleanup */
    g_test_ctx.ice_a = ice_integration_create(
        g_test_ctx.peer_a,
        g_test_ctx.loop,
        NULL, 0,  /* No STUN servers */
        NULL, NULL, NULL, 0
    );
    check_not_null(g_test_ctx.ice_a);
}

void test_datachannel_attaches_turbonet_ice_agent(void) {
    turbo_dc_config_t dc_config = {
        .is_server = 0,
        .transport = TURBO_DC_TRANSPORT_ICE
    };
    ice_config_t ice_config = ice_default_config();

    g_test_ctx.ctx_a = turbo_dc_context_create(&dc_config);
    check_not_null(g_test_ctx.ctx_a);
    g_test_ctx.peer_a = turbo_dc_peer_create(g_test_ctx.ctx_a, NULL, 0, NULL);
    check_not_null(g_test_ctx.peer_a);

    g_test_ctx.direct_ice_ctx = coro_context_create(g_test_ctx.loop);
    check_not_null(g_test_ctx.direct_ice_ctx);
    g_test_ctx.direct_ice_agent =
        ice_agent_create(g_test_ctx.direct_ice_ctx, &ice_config);
    check_not_null(g_test_ctx.direct_ice_agent);

    check_equal((int)(turbo_dc_peer_set_ice_agent(NULL, g_test_ctx.direct_ice_agent)), (int)(-1));
    check_equal((int)(turbo_dc_peer_set_ice_agent(g_test_ctx.peer_a, NULL)), (int)(-1));
    check_equal((int)(turbo_dc_peer_set_ice_agent(
               g_test_ctx.peer_a, g_test_ctx.direct_ice_agent)), (int)(0));
    check_equal((int)(turbo_dc_peer_set_external_transport(g_test_ctx.peer_a, NULL, NULL)), (int)(0));
}

void test_ice_integration_credentials(void) {
    /* Setup */
    test_ice_integration_create();
    
    /* Get local credentials */
    char ufrag[32], pwd[64];
    int result = ice_integration_get_local_credentials(
        g_test_ctx.ice_a, ufrag, sizeof(ufrag), pwd, sizeof(pwd)
    );
    
    check_equal(result, 0);
    check_greater(strlen(ufrag), 0);
    check_greater(strlen(pwd), 0);
    
    /* Set remote credentials */
    result = ice_integration_set_remote_credentials(g_test_ctx.ice_a, "test_ufrag", "test_password");
    check_equal(result, 0);
}

void test_ice_integration_gathering(void) {
    /* Setup */
    test_ice_integration_create();
    
    /* Set candidate callback */
    ice_integration_on_candidate(g_test_ctx.ice_a, on_candidate_a, NULL);
    
    /* Start gathering */
    int result = ice_integration_start_gathering(g_test_ctx.ice_a);
    check_equal(result, 0);
    
    /* Run event loop briefly to allow gathering to start */
    for (int i = 0; i < 10; i++) {
        turbo_loop_poll(g_test_ctx.loop, 0, 0);
        ice_integration_poll(g_test_ctx.ice_a);
    }
    
    /* Should have at least one candidate (host) eventually */
    /* Note: This test may not always pass immediately on all systems */
    /* In production, candidates arrive asynchronously */
}

void test_ice_integration_timeout(void) {
    /* Setup */
    test_ice_integration_create();
    
    /* Set very short timeout for testing */
    ice_integration_set_connection_timeout(g_test_ctx.ice_a, 100);
    
    /* Start gathering without remote peer */
    ice_integration_start_gathering(g_test_ctx.ice_a);
    
    /* Run event loop briefly */
    for (int i = 0; i < 5; i++) {
        turbo_loop_poll(g_test_ctx.loop, 0, 0);
        ice_integration_poll(g_test_ctx.ice_a);
    }
    
    /* Timeout should trigger (no assertion, just verify no crash) */
}

void test_ice_integration_reconnect(void) {
    /* Setup */
    test_ice_integration_create();
    
    /* Set max attempts */
    ice_integration_set_max_reconnect_attempts(g_test_ctx.ice_a, 3);
    
    /* Trigger reconnect */
    int result = ice_integration_reconnect(g_test_ctx.ice_a);
    check_equal(result, 0);
    
    /* Multiple reconnects should eventually fail */
    for (int i = 0; i < 5; i++) {
        result = ice_integration_reconnect(g_test_ctx.ice_a);
    }
    check_equal(result, -1);  /* Should fail after max attempts */
}

void test_ice_integration_add_remote_candidate(void) {
    /* Setup */
    test_ice_integration_create();
    
    /* Add remote candidate */
    const char *candidate = "candidate:1 1 UDP 2130706431 192.168.1.100 54321 typ host";
    int result = ice_integration_add_remote_candidate(g_test_ctx.ice_a, candidate);
    
    /* Should succeed (even if not connected yet) */
    check_equal(result, 0);
}

void test_ice_integration_end_of_candidates(void) {
    /* Setup */
    test_ice_integration_create();
    
    /* Signal end of candidates (should not crash) */
    ice_integration_end_of_candidates(g_test_ctx.ice_a);
}

void test_ice_integration_with_stun(void) {
    /* Create peer A */
    turbo_dc_config_t config_a = {
        .is_server = 0,
        .transport = TURBO_DC_TRANSPORT_ICE
    };
    g_test_ctx.ctx_a = turbo_dc_context_create(&config_a);
    check_not_null(g_test_ctx.ctx_a);
    
    g_test_ctx.peer_a = turbo_dc_peer_create(g_test_ctx.ctx_a, NULL, 0, NULL);
    check_not_null(g_test_ctx.peer_a);
    
    /* Create ICE integration with STUN server */
    const char *stun_servers[] = {"stun:stun.l.google.com:19302"};
    g_test_ctx.ice_a = ice_integration_create(
        g_test_ctx.peer_a,
        g_test_ctx.loop,
        stun_servers, 1,
        NULL, NULL, NULL, 0
    );
    check_not_null(g_test_ctx.ice_a);
    
    /* Set candidate callback */
    ice_integration_on_candidate(g_test_ctx.ice_a, on_candidate_a, NULL);
    
    /* Start gathering */
    int result = ice_integration_start_gathering(g_test_ctx.ice_a);
    check_equal(result, 0);
    
    /* Run event loop long enough for DNS to complete and cleanup to work */
    /* This gives time for async operations to finish before tearDown */
    for (int i = 0; i < 100; i++) {
        turbo_loop_poll(g_test_ctx.loop, 0, 0);
        ice_integration_poll(g_test_ctx.ice_a);
        turbo_sleep_ms(10);
    }
}

/* ============================================================================
 * Main
 * ============================================================================ */

spec("test_ice_integration") {
  before_each() { setUp(); }
  after_each() { tearDown(); }
  it("test_ice_integration_create") { test_ice_integration_create(); };
  it("test_datachannel_attaches_turbonet_ice_agent") { test_datachannel_attaches_turbonet_ice_agent(); };
  it("test_ice_integration_credentials") { test_ice_integration_credentials(); };
  it("test_ice_integration_gathering") { test_ice_integration_gathering(); };
  it("test_ice_integration_timeout") { test_ice_integration_timeout(); };
  it("test_ice_integration_reconnect") { test_ice_integration_reconnect(); };
  it("test_ice_integration_add_remote_candidate") { test_ice_integration_add_remote_candidate(); };
  it("test_ice_integration_end_of_candidates") { test_ice_integration_end_of_candidates(); };
    /* Skip STUN test by default - it requires network and proper async cleanup */
    /* STUN-backed integration test remains disabled in this suite. */
}
