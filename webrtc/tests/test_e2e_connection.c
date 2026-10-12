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

/* The Windows E2E fixture records a sanitized crash signature only.
 * The handler returns CONTINUE_SEARCH: crashes must still FAIL CTest. */
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif
#include "tinytest.h"
#include "turbo_datachannel.h"
#include "turbo_media_engine.h"
#include "ice_integration.h"
#include <salts/clock.h>
#include <salts/thread.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#ifdef _WIN32
static volatile LONG g_e2e_diag_phase = 0;
static volatile LONG g_e2e_last_main_phase = 0;
static volatile LONG g_e2e_poll_iteration = 0;
static DWORD g_e2e_main_thread_id = 0;
static volatile LONG g_e2e_fault_logged = 0;

/* Return only a sanitized module basename and module-relative offset.
 * Never emit absolute addresses, stack bytes, SDP, tokens or media. */
static void e2e_module_rva(const void *pc, char module_name[64],
                           unsigned long long *rva) {
    HMODULE module = NULL;
    strcpy(module_name, "unknown");
    *rva = 0;
    if (!pc || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        (LPCSTR)pc, &module))
        return;

    char file[MAX_PATH];
    DWORD len = GetModuleFileNameA(module, file, (DWORD)sizeof(file));
    if (len > 0 && len < (DWORD)sizeof(file)) {
        DWORD base = 0;
        for (DWORD i = 0; i < len; ++i)
            if (file[i] == '\\' || file[i] == '/')
                base = i + 1;
        size_t n = 0;
        for (DWORD i = base; i < len && n < 63; ++i) {
            const char c = file[i];
            module_name[n++] =
                (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'
                    ? c : '_';
        }
        module_name[n] = '\0';
    }
    *rva = (unsigned long long)((uintptr_t)pc - (uintptr_t)module);
}

/* Walk the *faulting* CONTEXT, not the filter's own call stack. This avoids
 * full memory dumps and symbol servers while retaining module/RVA evidence.
 * RtlVirtualUnwind is supported on the Windows x64 MSVC CI target. */
static void e2e_emit_fault_frames(const EXCEPTION_POINTERS *ep, HANDLE err) {
#if defined(_M_X64)
    if (!ep || !ep->ContextRecord || !err || err == INVALID_HANDLE_VALUE)
        return;
    CONTEXT context = *ep->ContextRecord;
    for (unsigned depth = 0; depth < 12 && context.Rip; ++depth) {
        const DWORD64 pc = context.Rip;
        const DWORD64 sp = context.Rsp;
        char module_name[64];
        unsigned long long rva = 0;
        e2e_module_rva((const void *)(uintptr_t)pc, module_name, &rva);
        char line[176];
        int len = snprintf(line, sizeof(line),
                           "[webrtc-e2e-frame] depth=%u module=%s rva=%llu\n",
                           depth, module_name, rva);
        if (len > 0 && len < (int)sizeof(line)) {
            DWORD written = 0;
            (void)WriteFile(err, line, (DWORD)len, &written, NULL);
        }

        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION function =
            RtlLookupFunctionEntry(pc, &image_base, NULL);
        if (function) {
            PVOID handler_data = NULL;
            DWORD64 establisher_frame = 0;
            (void)RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, pc,
                                   function, &context, &handler_data,
                                   &establisher_frame, NULL);
        } else {
            DWORD64 caller = 0;
            SIZE_T bytes = 0;
            if (sp < 65536u ||
                !ReadProcessMemory(GetCurrentProcess(),
                                   (const void *)(uintptr_t)sp,
                                   &caller, sizeof(caller), &bytes) ||
                bytes != sizeof(caller))
                break;
            context.Rip = caller;
            context.Rsp = sp + sizeof(caller);
        }
        if ((context.Rip == pc && context.Rsp <= sp) ||
            context.Rsp <= sp)
            break;
    }
#else
    (void)ep;
    (void)err;
#endif
}

/* First-fatal-exception-only diagnostics: never suppress the CTest crash. */
static LONG WINAPI e2e_unhandled_exception(EXCEPTION_POINTERS *p) {
    const EXCEPTION_RECORD *rec = p ? p->ExceptionRecord : NULL;
    if (!rec || InterlockedCompareExchange(&g_e2e_fault_logged, 1, 0) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    char module_name[64];
    unsigned long long rva = 0;
    e2e_module_rva(rec->ExceptionAddress, module_name, &rva);

    const unsigned kind = rec->NumberParameters > 0
                              ? (unsigned)rec->ExceptionInformation[0] : 9u;
    const int near_null = rec->NumberParameters > 1 &&
                          rec->ExceptionInformation[1] < 65536u;
    char line[256];
    int len = snprintf(line, sizeof(line),
                       "[webrtc-e2e-exception] code=%lu phase=%ld main_phase=%ld "
                       "module=%s rva=%llu kind=%u near_null=%d "
                       "main_thread=%d poll_iteration=%ld\n",
                       (unsigned long)rec->ExceptionCode,
                       (long)InterlockedCompareExchange(&g_e2e_diag_phase, 0, 0),
                       (long)InterlockedCompareExchange(&g_e2e_last_main_phase, 0, 0),
                       module_name, rva, kind, near_null,
                       GetCurrentThreadId() == g_e2e_main_thread_id ? 1 : 0,
                       (long)InterlockedCompareExchange(&g_e2e_poll_iteration, 0, 0));
    if (len > 0 && len < (int)sizeof(line)) {
        DWORD written = 0;
        HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
        if (err && err != INVALID_HANDLE_VALUE) {
            (void)WriteFile(err, line, (DWORD)len, &written, NULL);
            e2e_emit_fault_frames(p, err);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#define E2E_DIAG_PHASE(value) \
    ((void)InterlockedExchange(&g_e2e_diag_phase, (LONG)(value)))
#else
#define E2E_DIAG_PHASE(value) ((void)0)
#endif

#define TEST_TIMEOUT_MS 30000
#define MESSAGE_COUNT 10
#define TEST_CANDIDATE_CAPACITY 512

typedef struct {
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
    char candidates_a[ICE_MAX_CANDIDATES][TEST_CANDIDATE_CAPACITY];
    char candidates_b[ICE_MAX_CANDIDATES][TEST_CANDIDATE_CAPACITY];
    int candidate_count_a;
    int candidate_count_b;
    
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
    
    printf("Peer A ICE candidate captured\n");
    
    if (g_ctx.candidate_count_a < ICE_MAX_CANDIDATES) {
        snprintf(g_ctx.candidates_a[g_ctx.candidate_count_a],
                 TEST_CANDIDATE_CAPACITY, "%s", candidate_sdp);
        g_ctx.candidate_count_a++;
    }
}

static void on_candidate_b(const char *candidate_sdp, void *user_data) {
    (void)user_data;
    
    printf("Peer B ICE candidate captured\n");
    
    if (g_ctx.candidate_count_b < ICE_MAX_CANDIDATES) {
        snprintf(g_ctx.candidates_b[g_ctx.candidate_count_b],
                 TEST_CANDIDATE_CAPACITY, "%s", candidate_sdp);
        g_ctx.candidate_count_b++;
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

static void on_gathering_complete_a(salts_ice_agent_t *agent,
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

static void on_gathering_complete_b(salts_ice_agent_t *agent,
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
#ifdef _WIN32
    InterlockedExchange(&g_e2e_fault_logged, 0);
    E2E_DIAG_PHASE(0);
    InterlockedExchange(&g_e2e_last_main_phase, 0);
    InterlockedExchange(&g_e2e_poll_iteration, 0);
    g_e2e_main_thread_id = GetCurrentThreadId();
    (void)SetUnhandledExceptionFilter(e2e_unhandled_exception);
#endif
    memset(&g_ctx, 0, sizeof(g_ctx));
}

void tearDown(void) {
#ifdef _WIN32
    /* Save which test step reached tearDown, even on an early assertion. */
    InterlockedExchange(&g_e2e_last_main_phase,
                        InterlockedCompareExchange(&g_e2e_diag_phase, 0, 0));
#endif
    E2E_DIAG_PHASE(90); /* channel A close */
    /* Cleanup channels */
    if (g_ctx.channel_a) {
        turbo_dc_channel_close(g_ctx.channel_a);
        g_ctx.channel_a = NULL;
    }
    E2E_DIAG_PHASE(91); /* channel B close */
    if (g_ctx.channel_b) {
        turbo_dc_channel_close(g_ctx.channel_b);
        g_ctx.channel_b = NULL;
    }

    E2E_DIAG_PHASE(92); /* media A destroy */
    if (g_ctx.media_a) {
        turbo_media_destroy(g_ctx.media_a);
        g_ctx.media_a = NULL;
    }

    /* Quiesce DTLS/SCTP before tearing down ICE underneath it. */
    E2E_DIAG_PHASE(93); /* peer A close */
    if (g_ctx.peer_a) {
        turbo_dc_peer_close(g_ctx.peer_a);
    }
    E2E_DIAG_PHASE(94); /* peer B close */
    if (g_ctx.peer_b) {
        turbo_dc_peer_close(g_ctx.peer_b);
    }
    E2E_DIAG_PHASE(95); /* post-close peer/ICE poll */
    for (int i = 0; i < 10; i++) {
#ifdef _WIN32
        InterlockedExchange(&g_e2e_poll_iteration, (LONG)i);
#endif
        E2E_DIAG_PHASE(950); /* peer A status poll */
        if (g_ctx.peer_a) turbo_dc_peer_poll(g_ctx.peer_a);
        E2E_DIAG_PHASE(951); /* peer B status poll */
        if (g_ctx.peer_b) turbo_dc_peer_poll(g_ctx.peer_b);
        E2E_DIAG_PHASE(952); /* ICE A wake */
        if (g_ctx.ice_a) {
            ice_integration_poll(g_ctx.ice_a);
        }
        E2E_DIAG_PHASE(953); /* ICE B wake */
        if (g_ctx.ice_b) {
            ice_integration_poll(g_ctx.ice_b);
        }
        E2E_DIAG_PHASE(954); /* owner callback scheduling gap */
        cmeta_sleep_ms(1);
    }
    
    /* Clear ICE agent references from peers BEFORE destroying ICE
     * This prevents use-after-free when peer tries to send via destroyed agent */
    E2E_DIAG_PHASE(96); /* peer A transport detach */
    if (g_ctx.peer_a) {
        turbo_dc_peer_set_external_transport(g_ctx.peer_a, NULL, NULL);
    }
    E2E_DIAG_PHASE(97); /* peer B transport detach */
    if (g_ctx.peer_b) {
        turbo_dc_peer_set_external_transport(g_ctx.peer_b, NULL, NULL);
    }
    
    /* Cleanup ICE */
    E2E_DIAG_PHASE(98); /* ICE A destroy */
    if (g_ctx.ice_a) {
        ice_integration_destroy(g_ctx.ice_a);
        g_ctx.ice_a = NULL;
    }
    E2E_DIAG_PHASE(99); /* ICE B destroy */
    if (g_ctx.ice_b) {
        ice_integration_destroy(g_ctx.ice_b);
        g_ctx.ice_b = NULL;
    }
    
    /* Cleanup peers */
    E2E_DIAG_PHASE(100); /* DC peer A destroy */
    if (g_ctx.peer_a) {
        turbo_dc_peer_destroy(g_ctx.peer_a);
        g_ctx.peer_a = NULL;
    }
    E2E_DIAG_PHASE(101); /* DC peer B destroy */
    if (g_ctx.peer_b) {
        turbo_dc_peer_destroy(g_ctx.peer_b);
        g_ctx.peer_b = NULL;
    }
    
    /* Cleanup contexts */
    E2E_DIAG_PHASE(102); /* DC context A destroy */
    if (g_ctx.ctx_a) {
        turbo_dc_context_destroy(g_ctx.ctx_a);
        g_ctx.ctx_a = NULL;
    }
    E2E_DIAG_PHASE(103); /* DC context B destroy */
    if (g_ctx.ctx_b) {
        turbo_dc_context_destroy(g_ctx.ctx_b);
        g_ctx.ctx_b = NULL;
    }
    E2E_DIAG_PHASE(104); /* cleanup finished */
    
}

/* ============================================================================
 * Tests
 * ============================================================================ */

void test_e2e_p2p_connection(void) {
    E2E_DIAG_PHASE(1); /* peer and context setup */
    uint64_t deadline = cmeta_monotonic_ms() + TEST_TIMEOUT_MS;
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
    
    E2E_DIAG_PHASE(2); /* ICE integration setup */
    /* Create ICE integration (no STUN servers for local testing) */
    g_ctx.ice_a = ice_integration_create(
        g_ctx.peer_a, NULL,
        NULL, 0,  /* No STUN servers */
        NULL, NULL, NULL, 0
    );
    check_not_null(g_ctx.ice_a);
    
    g_ctx.ice_b = ice_integration_create(
        g_ctx.peer_b, NULL,
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
    
    /* Gather sequentially so synchronous candidate callbacks never create a
     * cross-owner AB/BA wait in this single-process test harness. */
    E2E_DIAG_PHASE(3); /* gathering A */
    check_equal(ice_integration_start_gathering(g_ctx.ice_a), 0);
    while (!ice_integration_is_gathering_complete(g_ctx.ice_a) &&
           cmeta_monotonic_ms() < deadline) {
        cmeta_sleep_ms(1);
    }
    check_true(ice_integration_is_gathering_complete(g_ctx.ice_a));

    E2E_DIAG_PHASE(4); /* gathering B */
    check_equal(ice_integration_start_gathering(g_ctx.ice_b), 0);
    while (!ice_integration_is_gathering_complete(g_ctx.ice_b) &&
           cmeta_monotonic_ms() < deadline) {
        cmeta_sleep_ms(1);
    }
    check_true(ice_integration_is_gathering_complete(g_ctx.ice_b));

    E2E_DIAG_PHASE(5); /* remote candidate exchange */
    for (int i = 0; i < g_ctx.candidate_count_a; ++i) {
        check_equal(
            ice_integration_add_remote_candidate(
                g_ctx.ice_b, g_ctx.candidates_a[i]),
            0);
    }
    for (int i = 0; i < g_ctx.candidate_count_b; ++i) {
        check_equal(
            ice_integration_add_remote_candidate(
                g_ctx.ice_a, g_ctx.candidates_b[i]),
            0);
    }
    ice_integration_end_of_candidates(g_ctx.ice_a);
    ice_integration_end_of_candidates(g_ctx.ice_b);
    
    E2E_DIAG_PHASE(6); /* connection and media progress */
    /* Set channel callbacks */
    /* Run event loop until test completes */
    while (!g_ctx.test_complete) {
        turbo_dc_peer_poll(g_ctx.peer_a);
        turbo_dc_peer_poll(g_ctx.peer_b);
        ice_integration_poll(g_ctx.ice_a);
        ice_integration_poll(g_ctx.ice_b);
        turbo_dc_handle_timers();
        if (cmeta_monotonic_ms() >= deadline) {
            fail_test("Test timeout");
            break;
        }
        cmeta_sleep_ms(10);
    }
    
    E2E_DIAG_PHASE(7); /* completed handshake validation */
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

        E2E_DIAG_PHASE(8); /* media setup */
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
