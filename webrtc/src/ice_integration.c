/**
 * ice_integration.c - Complete ICE integration for WebRTC DataChannel
 *
 * Implements:
 * - ICE candidate trickle
 * - STUN/TURN server support
 * - Connection timeout handling
 * - Retry accounting and scheduling (not a complete ICE restart)
 */

#include "ice_integration.h"
#include "turbo_datachannel.h"
#include "ice/turbo_ice.h"
#include "tlog.h"

#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * ICE Integration Context
 * ============================================================================ */

struct ice_integration_ctx_s {
    turbo_dc_peer_t *peer;
    turbo_ice_agent_t *ice_agent;
    coro_context_t *ice_ctx;
    void *loop;

    /* Callbacks */
    void (*on_ice_candidate)(const char *candidate_sdp, void *user_data);
    void (*on_ice_state_change)(ice_state_t state, void *user_data);
    void *candidate_user_data;
    void *state_user_data;

    /* Connection management */
    turbo_timer_t *connection_timer;
    turbo_timer_t *reconnect_timer;

    int connection_timeout_ms;
    int reconnect_attempts;
    int max_reconnect_attempts;

    /* State */
    int gathering_complete;
    int gathering_start_pending;
    int remote_candidates_complete;
    int checks_started;
    int checks_start_pending;
    int peer_connect_pending;
};

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

static void on_ice_state_changed(turbo_ice_agent_t *agent, ice_state_t old_state, 
                                  ice_state_t new_state, void *user_data);
static void on_ice_gathering_changed(turbo_ice_agent_t *agent, 
                                      ice_gathering_state_t state, void *user_data);
static void on_ice_candidate_discovered(turbo_ice_agent_t *agent,
                                         const ice_candidate_t *candidate, void *user_data);
static void on_ice_data_received(turbo_ice_agent_t *agent, const void *data,
                                  size_t len, void *user_data);
static void on_connection_timeout(turbo_timer_t *timer);
static void on_reconnect_timer(turbo_timer_t *timer);
static void on_ice_connected_post(void *arg1, void *arg2);
static void start_gathering_task(coro_t *co, void *arg);
static void start_connectivity_checks_task(coro_t *co, void *arg);
static void send_ice_datagram(void *transport, const void *data, size_t len);

static int start_connectivity_checks_if_ready(ice_integration_ctx_t *ctx);
static void stop_and_destroy_timer(turbo_timer_t **timer_ptr);
static void drain_ice_context(ice_integration_ctx_t *ctx, int wait_for_coroutines_only);

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * Create ICE integration context with STUN/TURN support
 */
ice_integration_ctx_t *ice_integration_create(
    turbo_dc_peer_t *peer,
    void *loop,
    const char **stun_servers,
    int stun_count,
    const char **turn_servers,
    const char **turn_usernames,
    const char **turn_credentials,
    int turn_count
) {
    if (!peer || stun_count < 0 || turn_count < 0 ||
        (stun_count > 0 && !stun_servers) ||
        (turn_count > 0 && !turn_servers)) {
        return NULL;
    }

    
    ice_integration_ctx_t *ctx = calloc(1, sizeof(ice_integration_ctx_t));
    if (!ctx) return NULL;
    
    ctx->peer = peer;
    ctx->loop = loop;
    ctx->connection_timeout_ms = 30000;  /* 30 seconds */
    ctx->max_reconnect_attempts = 5;
    
    /* Configure ICE agent */
    ice_config_t ice_cfg = ice_default_config();
    ice_cfg.is_controlling = turbo_dc_peer_is_dtls_server(peer) ? 0 : 1;

    ice_cfg.aggressive_nomination = 1;
    ice_cfg.use_mdns_candidates = 0;  /* Disable mDNS until resolution is implemented */
    
    /* Add STUN servers */
    for (int i = 0; i < stun_count && i < ICE_MAX_STUN_SERVERS; i++) {
        strncpy(ice_cfg.stun_servers[i].url, stun_servers[i], 
                sizeof(ice_cfg.stun_servers[i].url) - 1);
        ice_cfg.stun_server_count++;
    }
    
    /* Add TURN servers */
    for (int i = 0; i < turn_count && i < ICE_MAX_TURN_SERVERS; i++) {
        strncpy(ice_cfg.turn_servers[i].url, turn_servers[i], 
                sizeof(ice_cfg.turn_servers[i].url) - 1);
        if (turn_usernames && turn_usernames[i]) {
            strncpy(ice_cfg.turn_servers[i].username, turn_usernames[i],
                    sizeof(ice_cfg.turn_servers[i].username) - 1);
        }
        if (turn_credentials && turn_credentials[i]) {
            strncpy(ice_cfg.turn_servers[i].credential, turn_credentials[i],
                    sizeof(ice_cfg.turn_servers[i].credential) - 1);
        }
        ice_cfg.turn_server_count++;
    }
    
    ctx->ice_ctx = coro_context_create(loop);
    if (!ctx->ice_ctx) {
        TLOG_ERROR("Failed to create ICE context");
        free(ctx);
        return NULL;
    }
#if defined(__linux__) && !defined(__ANDROID__)
    /* The io_uring UDP path is still flaky for local ICE checks on this repo's
     * Linux setup. Force epoll for ICE sockets until the lower-layer backend is
     * fixed. */
    coro_context_set_udp_backend(ctx->ice_ctx, TURBO_UDP_BACKEND_EPOLL);
#elif defined(_WIN32)
    /* The generic Windows UDP selection is currently unreliable for local ICE
     * checks in this repo. Force IOCP for ICE sockets. */
    coro_context_set_udp_backend(ctx->ice_ctx, TURBO_UDP_BACKEND_IOCP);
#endif

    /* Create ICE agent */
    ctx->ice_agent = ice_agent_create(ctx->ice_ctx, &ice_cfg);
    if (!ctx->ice_agent) {
        TLOG_ERROR("Failed to create ICE agent");
        coro_context_destroy(ctx->ice_ctx);
        free(ctx);
        return NULL;
    }
    
    /* Set ICE callbacks */
    ice_callbacks_t callbacks = {
        .on_state_change = on_ice_state_changed,
        .on_gathering_change = on_ice_gathering_changed,
        .on_candidate = on_ice_candidate_discovered,
        .on_data = on_ice_data_received,
        .user_data = ctx
    };
    ice_agent_set_callbacks(ctx->ice_agent, &callbacks);
    
    /* Create connection timeout timer */
    ctx->connection_timer = turbo_timer_create(ctx->loop);
    if (ctx->connection_timer) {
        turbo_timer_set_data(ctx->connection_timer, ctx);
    }

    
    /* Create reconnect timer */
    ctx->reconnect_timer = turbo_timer_create(ctx->loop);
    if (ctx->reconnect_timer) {
        turbo_timer_set_data(ctx->reconnect_timer, ctx);
    }

    
    if (turbo_dc_peer_set_external_transport(
            peer, ctx->ice_agent, send_ice_datagram) != 0) {
        ice_callbacks_t empty_callbacks = {0};
        ice_agent_set_callbacks(ctx->ice_agent, &empty_callbacks);
        stop_and_destroy_timer(&ctx->connection_timer);
        stop_and_destroy_timer(&ctx->reconnect_timer);
        ice_agent_destroy(ctx->ice_agent);
        coro_context_destroy(ctx->ice_ctx);
        free(ctx);
        return NULL;
    }
    
    return ctx;
}

/**
 * Destroy ICE integration context
 */
void ice_integration_destroy(ice_integration_ctx_t *ctx) {
    if (!ctx) return;

    ctx->on_ice_candidate = NULL;
    ctx->on_ice_state_change = NULL;
    ctx->candidate_user_data = NULL;
    ctx->state_user_data = NULL;
    ctx->peer_connect_pending = 0;

    stop_and_destroy_timer(&ctx->connection_timer);
    stop_and_destroy_timer(&ctx->reconnect_timer);

    if (ctx->peer) {
        turbo_dc_peer_set_external_transport(ctx->peer, NULL, NULL);
    }
    
    if (ctx->ice_agent) {
        ice_callbacks_t callbacks = {0};
        ice_agent_set_callbacks(ctx->ice_agent, &callbacks);
        ice_agent_close(ctx->ice_agent);
        drain_ice_context(ctx, 1);
        ice_agent_destroy(ctx->ice_agent);
        ctx->ice_agent = NULL;
        coro_context_stop(ctx->ice_ctx);
        drain_ice_context(ctx, 0);
    }
    if (ctx->ice_ctx) {
        coro_context_destroy(ctx->ice_ctx);
        ctx->ice_ctx = NULL;
    }

    
    free(ctx);
}

/**
 * Set callback for ICE candidate trickle
 */
void ice_integration_on_candidate(ice_integration_ctx_t *ctx,
                                   void (*callback)(const char *candidate_sdp, void *user_data),
                                   void *user_data) {
    if (ctx) {
        ctx->on_ice_candidate = callback;
        ctx->candidate_user_data = user_data;
    }
}

/**
 * Set callback for ICE state changes
 */
void ice_integration_on_state_change(ice_integration_ctx_t *ctx,
                                      void (*callback)(ice_state_t state, void *user_data),
                                      void *user_data) {
    if (ctx) {
        ctx->on_ice_state_change = callback;
        ctx->state_user_data = user_data;
    }
}

/**
 * Get local ICE credentials for SDP
 */
int ice_integration_get_local_credentials(ice_integration_ctx_t *ctx,
                                           char *ufrag, size_t ufrag_len,
                                           char *pwd, size_t pwd_len) {
    if (!ctx || !ctx->ice_agent) return -1;
    
    ice_agent_get_local_credentials(ctx->ice_agent, ufrag, ufrag_len, pwd, pwd_len);
    return 0;
}

/**
 * Set remote ICE credentials from SDP
 */
int ice_integration_set_remote_credentials(ice_integration_ctx_t *ctx,
                                            const char *ufrag,
                                            const char *pwd) {
    if (!ctx || !ctx->ice_agent) return -1;
    
    return ice_agent_set_remote_credentials(ctx->ice_agent, ufrag, pwd);
}

/**
 * Start ICE gathering
 */
int ice_integration_start_gathering(ice_integration_ctx_t *ctx) {
    if (!ctx || !ctx->ice_agent) return -1;
    if (ctx->gathering_complete || ctx->gathering_start_pending) return 0;
    
    TLOG_INFO("Starting candidate gathering...");
    
    /* Start connection timeout */
    if (ctx->connection_timer) {
        turbo_timer_start(ctx->connection_timer, on_connection_timeout,
                        ctx->connection_timeout_ms, 0);
    }

    ctx->gathering_start_pending = 1;
    if (coro_context_spawn(ctx->ice_ctx, start_gathering_task, ctx) != 0) {
        ctx->gathering_start_pending = 0;
        return -1;
    }

    return 0;
}

/**
 * Add remote ICE candidate (trickle ICE)
 */
int ice_integration_add_remote_candidate(ice_integration_ctx_t *ctx,
                                          const char *candidate_sdp) {
    if (!ctx || !ctx->ice_agent || !candidate_sdp) return -1;
    
    TLOG_DEBUGF("Adding remote candidate: {}", candidate_sdp);
    
    int result = ice_agent_add_remote_candidate(ctx->ice_agent, candidate_sdp);
    
    /* Try to start checks if ready */
    start_connectivity_checks_if_ready(ctx);
    
    return result;
}

/**
 * Signal end of remote candidates
 */
void ice_integration_end_of_candidates(ice_integration_ctx_t *ctx) {
    if (!ctx || !ctx->ice_agent) return;
    
    TLOG_INFO("End of remote candidates signaled");
    
    ctx->remote_candidates_complete = 1;
    ice_agent_end_of_candidates(ctx->ice_agent);
    
    /* Try to start checks if ready */
    start_connectivity_checks_if_ready(ctx);
}

void ice_integration_poll(ice_integration_ctx_t *ctx) {
    if (!ctx || !ctx->ice_ctx) {
        return;
    }

    coro_context_run(ctx->ice_ctx, TURBO_RUN_NOWAIT);
}

int ice_integration_is_gathering_complete(ice_integration_ctx_t *ctx) {
    if (!ctx) {
        return 0;
    }

    return ctx->gathering_complete ? 1 : 0;
}

/**
 * Set connection timeout (milliseconds)
 */
void ice_integration_set_connection_timeout(ice_integration_ctx_t *ctx, int timeout_ms) {
    if (ctx) {
        ctx->connection_timeout_ms = timeout_ms;
    }
}

/**
 * Set max reconnection attempts
 */
void ice_integration_set_max_reconnect_attempts(ice_integration_ctx_t *ctx, int max_attempts) {
    if (ctx) {
        ctx->max_reconnect_attempts = max_attempts;
    }
}

/**
 * Trigger manual reconnection
 */
int ice_integration_reconnect(ice_integration_ctx_t *ctx) {
    if (!ctx || !ctx->ice_agent) return -1;
    
    int delay_ms = 2000; /* Default */

    /* Algorithmic Backoff for ICE */
    if (ctx->reconnect_attempts <= 5) {
        /* Phase 1: Aggressive (Fast) */
        delay_ms = 2000;
        TLOG_DEBUGF("ICE Reconnect [Aggressive]: attempt {}/{}",
                   ctx->reconnect_attempts + 1, ctx->max_reconnect_attempts);
    } else if (ctx->reconnect_attempts <= 15) {
        /* Phase 2: Backoff (Jittered) */
        unsigned int seed = (unsigned int)turbo_hrtime();
        delay_ms = 10000 + (int)(seed % 20000); /* 10-30s */
        if (ctx->reconnect_attempts % 5 == 0) {
            TLOG_DEBUGF("ICE Reconnect [Backoff]: attempt {}/{}",
                       ctx->reconnect_attempts + 1, ctx->max_reconnect_attempts);
        }
    } else {
        /* Phase 3: Standby (Long) */
        unsigned int seed = (unsigned int)turbo_hrtime();
        delay_ms = 60000 + (int)(seed % 60000); /* 1-2m */
        if (ctx->reconnect_attempts % 10 == 0) {
            TLOG_DEBUGF("ICE Reconnect [Standby]: attempt {}/{}",
                       ctx->reconnect_attempts + 1, ctx->max_reconnect_attempts);
        }
    }

    ctx->reconnect_attempts++;
    
    if (ctx->reconnect_attempts > ctx->max_reconnect_attempts && ctx->max_reconnect_attempts > 0) {
        TLOG_DEBUG("Max reconnection attempts reached");
        return -1;
    }
    
    /* Scheduling reconnection */
    if (ctx->reconnect_timer) {
        turbo_timer_start(ctx->reconnect_timer, on_reconnect_timer, delay_ms, 0);
    }

    return 0;
}

/**
 * Enable/disable loopback candidate gathering
 */
void ice_integration_set_allow_loopback(ice_integration_ctx_t *ctx, int allow) {
    if (ctx && ctx->ice_agent) {
        ice_agent_set_allow_loopback(ctx->ice_agent, allow);
    }
}

/* ============================================================================
 * Internal Callbacks
 * ============================================================================ */

static void on_ice_state_changed(turbo_ice_agent_t *agent, ice_state_t old_state,
                                  ice_state_t new_state, void *user_data) {
    (void)agent;
    ice_integration_ctx_t *ctx = (ice_integration_ctx_t *)user_data;
    
    const char *state_names[] = {
        "NEW", "GATHERING", "CONNECTING", "CONNECTED",
        "COMPLETED", "FAILED", "DISCONNECTED", "CLOSED"
    };
    
    TLOG_INFOF("ICE state changed: {} -> {}",
              state_names[old_state], state_names[new_state]);
    
    switch (new_state) {
        case ICE_STATE_CONNECTED:
        case ICE_STATE_COMPLETED:
            /* Stop connection timeout */
            if (ctx->connection_timer) {
                turbo_timer_stop(ctx->connection_timer);
            }

            
            /* Reset reconnect attempts */
            ctx->reconnect_attempts = 0;
            
            if (ctx->peer && !ctx->peer_connect_pending) {
                ctx->peer_connect_pending = 1;
                if (coro_post(ctx->ice_ctx, on_ice_connected_post, ctx, NULL) != 0) {
                    ctx->peer_connect_pending = 0;
                    TLOG_WARN("Failed to defer ICE peer connect");
                }
            }
            break;
            
        case ICE_STATE_FAILED:
            /* Use ice_integration_reconnect to manage the retry logic and backoff */
            ice_integration_reconnect(ctx);
            break;
            
        case ICE_STATE_DISCONNECTED:
            TLOG_DEBUG("ICE connection lost, scheduling reconnection...");
            
            /* Immediate reconnection attempt for first drop */
            ice_integration_reconnect(ctx);
            break;
            
        default:
            break;
    }
    
    /* Notify application */
    if (ctx->on_ice_state_change) {
        ctx->on_ice_state_change(new_state, ctx->state_user_data);
    }
}

static void on_ice_connected_post(void *arg1, void *arg2) {
    ice_integration_ctx_t *ctx = (ice_integration_ctx_t *)arg1;
    turbo_dc_state_t peer_state;
    (void)arg2;

    if (!ctx) {
        return;
    }

    ctx->peer_connect_pending = 0;
    if (!ctx->peer || !ctx->ice_agent) {
        return;
    }

    peer_state = turbo_dc_peer_get_state(ctx->peer);
    if (peer_state == TURBO_DC_STATE_NEW) {
        turbo_dc_peer_connect(ctx->peer);
    }
}

static void on_ice_gathering_changed(turbo_ice_agent_t *agent,
                                      ice_gathering_state_t state, void *user_data) {
    (void)agent;
    ice_integration_ctx_t *ctx = (ice_integration_ctx_t *)user_data;
    
    const char *state_names[] = {"NEW", "GATHERING", "COMPLETE"};
    TLOG_INFOF("ICE gathering state: {}", state_names[state]);
    
    if (state == ICE_GATHERING_COMPLETE) {
        ctx->gathering_complete = 1;
        ctx->gathering_start_pending = 0;
        
        /* Try to start checks if ready */
        start_connectivity_checks_if_ready(ctx);
    }
}

static void on_ice_candidate_discovered(turbo_ice_agent_t *agent,
                                         const ice_candidate_t *candidate, void *user_data) {
    (void)agent;
    ice_integration_ctx_t *ctx = (ice_integration_ctx_t *)user_data;
    
    /* Convert to SDP format */
    char candidate_sdp[512];
    if (ice_candidate_to_sdp(candidate, candidate_sdp, sizeof(candidate_sdp)) > 0) {
        TLOG_DEBUGF("Local candidate: {}", candidate_sdp);
        
        /* Trickle to application for signaling */
        if (ctx->on_ice_candidate) {
            ctx->on_ice_candidate(candidate_sdp, ctx->candidate_user_data);
        }
    }
}

static void on_ice_data_received(turbo_ice_agent_t *agent, const void *data,
                                  size_t len, void *user_data) {
    (void)agent;
    ice_integration_ctx_t *ctx = (ice_integration_ctx_t *)user_data;
    
    turbo_dc_peer_feed_transport_data(ctx->peer, data, len);
}

static void send_ice_datagram(void *transport, const void *data, size_t len) {
    (void)ice_agent_send((turbo_ice_agent_t *)transport, data, len);
}

static void on_connection_timeout(turbo_timer_t *timer) {
    if (!timer) return;
    
    ice_integration_ctx_t *ctx = (ice_integration_ctx_t *)turbo_timer_get_data(timer);

    
    /* Check if context was destroyed (data set to NULL during cleanup) */
    if (!ctx || !ctx->ice_agent) {
        return;
    }
    
    ice_state_t state = ice_agent_get_state(ctx->ice_agent);
    
    if (state != ICE_STATE_CONNECTED && state != ICE_STATE_COMPLETED) {
        TLOG_DEBUGF("Connection timeout after {} ms", ctx->connection_timeout_ms);
        
        /* Attempt reconnection */
        ice_integration_reconnect(ctx);
    }
}

static void on_reconnect_timer(turbo_timer_t *timer) {
    if (!timer) return;
    
    ice_integration_ctx_t *ctx = (ice_integration_ctx_t *)turbo_timer_get_data(timer);

    
    /* Check if context was destroyed (data set to NULL during cleanup) */
    if (!ctx || !ctx->ice_agent) {
        return;
    }

    ice_integration_reconnect(ctx);
}

static void start_gathering_task(coro_t *co, void *arg) {
    ice_integration_ctx_t *ctx = (ice_integration_ctx_t *)arg;
    int rc;
    (void)co;

    if (!ctx || !ctx->ice_agent) {
        return;
    }

    rc = ice_agent_gather_candidates(ctx->ice_agent);
    if (rc != 0) {
        ctx->gathering_start_pending = 0;
        TLOG_WARNF("Failed to start ICE candidate gathering: {}", rc);
    }
}

static void start_connectivity_checks_task(coro_t *co, void *arg) {
    ice_integration_ctx_t *ctx = (ice_integration_ctx_t *)arg;
    int rc;
    (void)co;

    if (!ctx || !ctx->ice_agent) {
        return;
    }

    rc = ice_agent_start_checks(ctx->ice_agent);
    ctx->checks_start_pending = 0;
    if (rc != 0) {
        ctx->checks_started = 0;
        TLOG_WARNF("Failed to start ICE connectivity checks: {}", rc);
    }
}

static int start_connectivity_checks_if_ready(ice_integration_ctx_t *ctx) {
    if (ctx->checks_started || ctx->checks_start_pending) {
        return 0;  /* Already started */
    }
    
    /* Need both gathering complete and at least one remote candidate */
    if (!ctx->gathering_complete) {
        TLOG_DEBUG("Not starting checks: gathering not complete");
        return 0;  /* Not ready yet */
    }
    
    /* Check if we have remote credentials */
    /* This is implicit - if we have remote candidates, we should have credentials */
    
    TLOG_INFO("Starting connectivity checks");
    
    ctx->checks_started = 1;
    ctx->checks_start_pending = 1;
    if (coro_context_spawn(ctx->ice_ctx, start_connectivity_checks_task, ctx) != 0) {
        ctx->checks_started = 0;
        ctx->checks_start_pending = 0;
        return -1;
    }
    return 0;
}

static void stop_and_destroy_timer(turbo_timer_t **timer_ptr) {
    if (!timer_ptr || !*timer_ptr) {
        return;
    }

    turbo_timer_stop(*timer_ptr);
    turbo_timer_set_data(*timer_ptr, NULL);
    turbo_timer_destroy(*timer_ptr);
    *timer_ptr = NULL;
}

static void drain_ice_context(ice_integration_ctx_t *ctx, int wait_for_coroutines_only) {
    int spins;

    if (!ctx || !ctx->ice_ctx) {
        return;
    }

    for (spins = 0; spins < 128; ++spins) {
        int coro_count = coro_context_coro_count(ctx->ice_ctx);
        int alive = coro_context_alive(ctx->ice_ctx);

        if (wait_for_coroutines_only) {
            if (coro_count == 0) {
                break;
            }
        } else if (!alive) {
            break;
        }

        coro_context_run(ctx->ice_ctx, TURBO_RUN_ONCE);
    }
}
