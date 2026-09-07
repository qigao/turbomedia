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
#include "salts_ice_owner.h"
#include "turbo_datachannel.h"
#include "ice/salts_ice.h"
#include "tlog.h"
#include <salts/clock.h>
#include <salts/thread.h>

#include <stdlib.h>
#include <string.h>

enum {
    ICE_WORKER_POLL_INTERVAL_MS = 10
};

#define ICE_WORKER_POLL_INTERVAL_NS \
    ((uint64_t)ICE_WORKER_POLL_INTERVAL_MS * UINT64_C(1000000))

/* ============================================================================
 * ICE Integration Context
 * ============================================================================ */

struct ice_integration_ctx_s {
    turbo_dc_peer_t *peer;
    turbo_ice_owner_t *ice_owner;
    salts_thread_t worker;
    salts_mutex_t worker_mutex;
    salts_cond_t worker_cond;
    int worker_sync_initialized;
    int worker_started;
    int stop_requested;
    int gather_requested;
    int checks_requested;

    /* Callbacks */
    void (*on_ice_candidate)(const char *candidate_sdp, void *user_data);
    void (*on_ice_state_change)(ice_state_t state, void *user_data);
    void *candidate_user_data;
    void *state_user_data;

    /* Connection management */
    salts_timer_t *connection_timer;
    salts_timer_t *reconnect_timer;

    int connection_timeout_ms;
    int reconnect_attempts;
    int max_reconnect_attempts;

    /* State */
    int gathering_complete;
    int gathering_start_pending;
    int remote_candidate_count;
    int remote_candidates_complete;
    int checks_started;
    int checks_start_pending;
};

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

static void on_ice_state_changed(salts_ice_agent_t *agent, ice_state_t old_state,
                                  ice_state_t new_state, void *user_data);
static void on_ice_gathering_changed(salts_ice_agent_t *agent,
                                      ice_gathering_state_t state, void *user_data);
static void on_ice_candidate_discovered(salts_ice_agent_t *agent,
                                         const ice_candidate_t *candidate, void *user_data);
static void on_ice_data_received(salts_ice_agent_t *agent, const void *data,
                                  size_t len, void *user_data);
static void on_connection_timeout(salts_timer_t *timer);
static void on_reconnect_timer(salts_timer_t *timer);
static void ice_worker_main(void *arg);
static void send_ice_datagram(void *transport, const void *data, size_t len);

static int start_connectivity_checks_if_ready(ice_integration_ctx_t *ctx);
static void stop_and_destroy_timer(salts_timer_t **timer_ptr);

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
    (void)loop;
    ctx->connection_timeout_ms = 30000;  /* 30 seconds */
    ctx->max_reconnect_attempts = 5;
    salts_mutex_init(&ctx->worker_mutex);
    salts_cond_init(&ctx->worker_cond);
    ctx->worker_sync_initialized = 1;
    
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
    
    ice_callbacks_t callbacks = {
        .on_state_change = on_ice_state_changed,
        .on_gathering_change = on_ice_gathering_changed,
        .on_candidate = on_ice_candidate_discovered,
        .on_data = on_ice_data_received,
        .user_data = ctx
    };

    ctx->ice_owner = turbo_ice_owner_create(&ice_cfg, &callbacks);
    if (!ctx->ice_owner) {
        TLOG_ERROR("Failed to create SaltsNet ICE owner");
        salts_cond_destroy(&ctx->worker_cond);
        salts_mutex_destroy(&ctx->worker_mutex);
        free(ctx);
        return NULL;
    }
    
    /* Create connection timeout timer */
    ctx->connection_timer = salts_timer_create(NULL);
    if (ctx->connection_timer) {
        salts_timer_set_data(ctx->connection_timer, ctx);
    }

    
    /* Create reconnect timer */
    ctx->reconnect_timer = salts_timer_create(NULL);
    if (ctx->reconnect_timer) {
        salts_timer_set_data(ctx->reconnect_timer, ctx);
    }

    
    if (turbo_dc_peer_set_external_transport(
            peer, ctx->ice_owner, send_ice_datagram) != 0) {
        stop_and_destroy_timer(&ctx->connection_timer);
        stop_and_destroy_timer(&ctx->reconnect_timer);
        turbo_ice_owner_destroy(ctx->ice_owner);
        salts_cond_destroy(&ctx->worker_cond);
        salts_mutex_destroy(&ctx->worker_mutex);
        free(ctx);
        return NULL;
    }

    if (salts_thread_create(&ctx->worker, ice_worker_main, ctx) != 0) {
        turbo_dc_peer_set_external_transport(peer, NULL, NULL);
        stop_and_destroy_timer(&ctx->connection_timer);
        stop_and_destroy_timer(&ctx->reconnect_timer);
        turbo_ice_owner_destroy(ctx->ice_owner);
        salts_cond_destroy(&ctx->worker_cond);
        salts_mutex_destroy(&ctx->worker_mutex);
        free(ctx);
        return NULL;
    }
    ctx->worker_started = 1;
    
    return ctx;
}

/**
 * Destroy ICE integration context
 */
void ice_integration_destroy(ice_integration_ctx_t *ctx) {
    if (!ctx) return;

    stop_and_destroy_timer(&ctx->connection_timer);
    stop_and_destroy_timer(&ctx->reconnect_timer);

    if (ctx->peer) {
        turbo_dc_peer_set_external_transport(ctx->peer, NULL, NULL);
    }

    if (ctx->worker_sync_initialized) {
        salts_mutex_lock(&ctx->worker_mutex);
        ctx->stop_requested = 1;
        ctx->on_ice_candidate = NULL;
        ctx->on_ice_state_change = NULL;
        ctx->candidate_user_data = NULL;
        ctx->state_user_data = NULL;
        salts_cond_broadcast(&ctx->worker_cond);
        salts_mutex_unlock(&ctx->worker_mutex);
    }
    
    if (ctx->ice_owner) {
        turbo_ice_owner_close(ctx->ice_owner);
    }
    if (ctx->worker_started) {
        salts_thread_join(&ctx->worker);
        salts_thread_destroy(&ctx->worker);
        ctx->worker_started = 0;
    }
    if (ctx->ice_owner) {
        turbo_ice_owner_destroy(ctx->ice_owner);
        ctx->ice_owner = NULL;
    }
    if (ctx->worker_sync_initialized) {
        salts_cond_destroy(&ctx->worker_cond);
        salts_mutex_destroy(&ctx->worker_mutex);
        ctx->worker_sync_initialized = 0;
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
        salts_mutex_lock(&ctx->worker_mutex);
        ctx->on_ice_candidate = callback;
        ctx->candidate_user_data = user_data;
        salts_mutex_unlock(&ctx->worker_mutex);
    }
}

/**
 * Set callback for ICE state changes
 */
void ice_integration_on_state_change(ice_integration_ctx_t *ctx,
                                      void (*callback)(ice_state_t state, void *user_data),
                                      void *user_data) {
    if (ctx) {
        salts_mutex_lock(&ctx->worker_mutex);
        ctx->on_ice_state_change = callback;
        ctx->state_user_data = user_data;
        salts_mutex_unlock(&ctx->worker_mutex);
    }
}

/**
 * Get local ICE credentials for SDP
 */
int ice_integration_get_local_credentials(ice_integration_ctx_t *ctx,
                                           char *ufrag, size_t ufrag_len,
                                           char *pwd, size_t pwd_len) {
    if (!ctx || !ctx->ice_owner) return -1;
    
    return turbo_ice_owner_get_local_credentials(
        ctx->ice_owner, ufrag, ufrag_len, pwd, pwd_len);
}

/**
 * Set remote ICE credentials from SDP
 */
int ice_integration_set_remote_credentials(ice_integration_ctx_t *ctx,
                                            const char *ufrag,
                                            const char *pwd) {
    if (!ctx || !ctx->ice_owner) return -1;
    
    return turbo_ice_owner_set_remote_credentials(ctx->ice_owner, ufrag, pwd);
}

/**
 * Start ICE gathering
 */
int ice_integration_start_gathering(ice_integration_ctx_t *ctx) {
    if (!ctx || !ctx->ice_owner) return -1;

    salts_mutex_lock(&ctx->worker_mutex);
    if (ctx->stop_requested || ctx->gathering_complete ||
        ctx->gathering_start_pending) {
        salts_mutex_unlock(&ctx->worker_mutex);
        return ctx->stop_requested ? -1 : 0;
    }
    ctx->gathering_start_pending = 1;
    ctx->gather_requested = 1;
    salts_cond_signal(&ctx->worker_cond);
    salts_mutex_unlock(&ctx->worker_mutex);

    TLOG_INFO("Starting candidate gathering...");

    /* Start connection timeout */
    if (ctx->connection_timer) {
        salts_timer_start(ctx->connection_timer, on_connection_timeout,
                        ctx->connection_timeout_ms, 0);
    }

    return 0;
}

/**
 * Add remote ICE candidate (trickle ICE)
 */
int ice_integration_add_remote_candidate(ice_integration_ctx_t *ctx,
                                          const char *candidate_sdp) {
    if (!ctx || !ctx->ice_owner || !candidate_sdp) return -1;
    
    TLOG_DEBUGF("Adding remote candidate: {}", candidate_sdp);
    
    int result = turbo_ice_owner_add_remote_candidate(
        ctx->ice_owner, candidate_sdp);
    if (result == 0) {
        salts_mutex_lock(&ctx->worker_mutex);
        ctx->remote_candidate_count++;
        salts_mutex_unlock(&ctx->worker_mutex);
    }
    
    /* Try to start checks if ready */
    start_connectivity_checks_if_ready(ctx);
    
    return result;
}

/**
 * Signal end of remote candidates
 */
void ice_integration_end_of_candidates(ice_integration_ctx_t *ctx) {
    if (!ctx || !ctx->ice_owner) return;
    
    TLOG_INFO("End of remote candidates signaled");
    
    salts_mutex_lock(&ctx->worker_mutex);
    ctx->remote_candidates_complete = 1;
    salts_mutex_unlock(&ctx->worker_mutex);
    (void)turbo_ice_owner_end_of_candidates(ctx->ice_owner);
    
    /* Try to start checks if ready */
    start_connectivity_checks_if_ready(ctx);
}

void ice_integration_poll(ice_integration_ctx_t *ctx) {
    if (!ctx || !ctx->worker_sync_initialized) {
        return;
    }
    salts_mutex_lock(&ctx->worker_mutex);
    salts_cond_signal(&ctx->worker_cond);
    salts_mutex_unlock(&ctx->worker_mutex);
}

int ice_integration_is_gathering_complete(ice_integration_ctx_t *ctx) {
    if (!ctx) {
        return 0;
    }

    int complete;
    salts_mutex_lock(&ctx->worker_mutex);
    complete = ctx->gathering_complete;
    salts_mutex_unlock(&ctx->worker_mutex);
    return complete ? 1 : 0;
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
    if (!ctx || !ctx->ice_owner) return -1;
    
    int delay_ms = 2000; /* Default */

    /* Algorithmic Backoff for ICE */
    if (ctx->reconnect_attempts <= 5) {
        /* Phase 1: Aggressive (Fast) */
        delay_ms = 2000;
        TLOG_DEBUGF("ICE Reconnect [Aggressive]: attempt {}/{}",
                   ctx->reconnect_attempts + 1, ctx->max_reconnect_attempts);
    } else if (ctx->reconnect_attempts <= 15) {
        /* Phase 2: Backoff (Jittered) */
        unsigned int seed = (unsigned int)salts_hrtime();
        delay_ms = 10000 + (int)(seed % 20000); /* 10-30s */
        if (ctx->reconnect_attempts % 5 == 0) {
            TLOG_DEBUGF("ICE Reconnect [Backoff]: attempt {}/{}",
                       ctx->reconnect_attempts + 1, ctx->max_reconnect_attempts);
        }
    } else {
        /* Phase 3: Standby (Long) */
        unsigned int seed = (unsigned int)salts_hrtime();
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
        salts_timer_start(ctx->reconnect_timer, on_reconnect_timer, delay_ms, 0);
    }

    return 0;
}

/**
 * Enable/disable loopback candidate gathering
 */
void ice_integration_set_allow_loopback(ice_integration_ctx_t *ctx, int allow) {
    if (ctx && ctx->ice_owner) {
        (void)turbo_ice_owner_set_allow_loopback(ctx->ice_owner, allow);
    }
}

/* ============================================================================
 * Internal Callbacks
 * ============================================================================ */

static void on_ice_state_changed(salts_ice_agent_t *agent, ice_state_t old_state,
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
                salts_timer_stop(ctx->connection_timer);
            }

            
            /* Reset reconnect attempts */
            ctx->reconnect_attempts = 0;
            
            if (ctx->peer &&
                turbo_dc_peer_get_state(ctx->peer) == TURBO_DC_STATE_NEW) {
                if (turbo_dc_peer_connect(ctx->peer) != 0) {
                    TLOG_WARN("Failed to connect DataChannel peer after ICE connected");
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
    void (*state_callback)(ice_state_t, void *) = NULL;
    void *state_user_data = NULL;
    salts_mutex_lock(&ctx->worker_mutex);
    state_callback = ctx->on_ice_state_change;
    state_user_data = ctx->state_user_data;
    salts_mutex_unlock(&ctx->worker_mutex);
    if (state_callback) {
        state_callback(new_state, state_user_data);
    }
}

static void on_ice_gathering_changed(salts_ice_agent_t *agent,
                                      ice_gathering_state_t state, void *user_data) {
    (void)agent;
    ice_integration_ctx_t *ctx = (ice_integration_ctx_t *)user_data;
    
    const char *state_names[] = {"NEW", "GATHERING", "COMPLETE"};
    TLOG_INFOF("ICE gathering state: {}", state_names[state]);
    
    if (state == ICE_GATHERING_COMPLETE) {
        salts_mutex_lock(&ctx->worker_mutex);
        ctx->gathering_complete = 1;
        ctx->gathering_start_pending = 0;
        salts_mutex_unlock(&ctx->worker_mutex);
        
        /* Try to start checks if ready */
        start_connectivity_checks_if_ready(ctx);
    }
}

static void on_ice_candidate_discovered(salts_ice_agent_t *agent,
                                         const ice_candidate_t *candidate, void *user_data) {
    (void)agent;
    ice_integration_ctx_t *ctx = (ice_integration_ctx_t *)user_data;
    
    /* Convert to SDP format */
    char candidate_sdp[512];
    if (ice_candidate_to_sdp(candidate, candidate_sdp, sizeof(candidate_sdp)) > 0) {
        void (*candidate_callback)(const char *, void *) = NULL;
        void *candidate_user_data = NULL;
        TLOG_DEBUGF("Local candidate: {}", candidate_sdp);

        salts_mutex_lock(&ctx->worker_mutex);
        candidate_callback = ctx->on_ice_candidate;
        candidate_user_data = ctx->candidate_user_data;
        salts_mutex_unlock(&ctx->worker_mutex);

        /* Trickle to application for signaling */
        if (candidate_callback) {
            candidate_callback(candidate_sdp, candidate_user_data);
        }
    }
}

static void on_ice_data_received(salts_ice_agent_t *agent, const void *data,
                                  size_t len, void *user_data) {
    (void)agent;
    ice_integration_ctx_t *ctx = (ice_integration_ctx_t *)user_data;
    
    turbo_dc_peer_feed_transport_data(ctx->peer, data, len);
}

static void send_ice_datagram(void *transport, const void *data, size_t len) {
    (void)turbo_ice_owner_send_async(
        (turbo_ice_owner_t *)transport, data, len);
}

static void on_connection_timeout(salts_timer_t *timer) {
    if (!timer) return;
    
    ice_integration_ctx_t *ctx = (ice_integration_ctx_t *)salts_timer_get_data(timer);

    
    /* Check if context was destroyed (data set to NULL during cleanup) */
    if (!ctx || !ctx->ice_owner) {
        return;
    }
    
    ice_state_t state = turbo_ice_owner_get_state(ctx->ice_owner);
    
    if (state != ICE_STATE_CONNECTED && state != ICE_STATE_COMPLETED) {
        TLOG_DEBUGF("Connection timeout after {} ms", ctx->connection_timeout_ms);
        
        /* Attempt reconnection */
        ice_integration_reconnect(ctx);
    }
}

static void on_reconnect_timer(salts_timer_t *timer) {
    if (!timer) return;
    
    ice_integration_ctx_t *ctx = (ice_integration_ctx_t *)salts_timer_get_data(timer);

    
    /* Check if context was destroyed (data set to NULL during cleanup) */
    if (!ctx || !ctx->ice_owner) {
        return;
    }

    ice_integration_reconnect(ctx);
}

static void ice_worker_main(void *arg) {
    ice_integration_ctx_t *ctx = (ice_integration_ctx_t *)arg;
    if (!ctx) {
        return;
    }

    for (;;) {
        int gather_requested;
        int checks_requested;
        int rc;

        salts_mutex_lock(&ctx->worker_mutex);
        if (!ctx->stop_requested && !ctx->gather_requested &&
            !ctx->checks_requested) {
            (void)salts_cond_timedwait(&ctx->worker_cond, &ctx->worker_mutex,
                                       ICE_WORKER_POLL_INTERVAL_NS);
        }
        if (ctx->stop_requested) {
            salts_mutex_unlock(&ctx->worker_mutex);
            break;
        }
        gather_requested = ctx->gather_requested;
        checks_requested = ctx->checks_requested;
        ctx->gather_requested = 0;
        ctx->checks_requested = 0;
        salts_mutex_unlock(&ctx->worker_mutex);

        if (gather_requested) {
            rc = turbo_ice_owner_gather_candidates(ctx->ice_owner);
            if (rc != 0) {
                salts_mutex_lock(&ctx->worker_mutex);
                ctx->gathering_start_pending = 0;
                salts_mutex_unlock(&ctx->worker_mutex);
                TLOG_WARNF("Failed to start ICE candidate gathering: {}", rc);
            }
        }

        if (checks_requested) {
            rc = turbo_ice_owner_start_checks(ctx->ice_owner);
            salts_mutex_lock(&ctx->worker_mutex);
            ctx->checks_start_pending = 0;
            if (rc != 0) {
                ctx->checks_started = 0;
            }
            salts_mutex_unlock(&ctx->worker_mutex);
            if (rc != 0) {
                TLOG_WARNF("Failed to start ICE connectivity checks: {}", rc);
            }
        }

    }
}

static int start_connectivity_checks_if_ready(ice_integration_ctx_t *ctx) {
    int result = 0;

    salts_mutex_lock(&ctx->worker_mutex);
    if (ctx->checks_started || ctx->checks_start_pending) {
        salts_mutex_unlock(&ctx->worker_mutex);
        return 0;
    }

    /* Connectivity checks start only after the local set and the signaled
     * remote set are complete. This keeps both peers able to service checks. */
    if (!ctx->gathering_complete || ctx->remote_candidate_count == 0 ||
        !ctx->remote_candidates_complete) {
        salts_mutex_unlock(&ctx->worker_mutex);
        return 0;
    }

    if (ctx->stop_requested) {
        salts_mutex_unlock(&ctx->worker_mutex);
        return -1;
    }

    ctx->checks_started = 1;
    ctx->checks_start_pending = 1;
    ctx->checks_requested = 1;
    salts_cond_signal(&ctx->worker_cond);
    salts_mutex_unlock(&ctx->worker_mutex);

    TLOG_INFO("Starting connectivity checks");
    return result;
}

static void stop_and_destroy_timer(salts_timer_t **timer_ptr) {
    if (!timer_ptr || !*timer_ptr) {
        return;
    }

    salts_timer_stop(*timer_ptr);
    salts_timer_set_data(*timer_ptr, NULL);
    salts_timer_destroy(*timer_ptr);
    *timer_ptr = NULL;
}
