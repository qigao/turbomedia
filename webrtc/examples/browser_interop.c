/**
 * browser_interop.c - Browser interoperability test
 *
 * Tests WebRTC DataChannel with browser peers (Chrome/Firefox)
 *
 * Usage:
 *   ./browser_interop [--port 8080]
 *
 * Then open browser_interop.html in your browser
 */

#include "turbo_datachannel.h"
#include "ice_integration.h"
#include "turbo_sdp.h"
#include <CoroNet/turbo_coro_context.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

static int g_running = 1;
static turbo_dc_peer_t *g_peer = NULL;
static ice_integration_ctx_t *g_ice = NULL;

#define MAX_LOCAL_CANDIDATES 32
static char g_local_candidates[MAX_LOCAL_CANDIDATES][512];
static int g_local_candidate_count = 0;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ============================================================================
 * Callbacks
 * ============================================================================ */

static void on_ice_candidate(const char *candidate_sdp, void *user_data) {
    (void)user_data;

    if (g_local_candidate_count < MAX_LOCAL_CANDIDATES) {
        snprintf(g_local_candidates[g_local_candidate_count],
                 sizeof(g_local_candidates[g_local_candidate_count]), "%s", candidate_sdp);
        g_local_candidate_count++;
    }
    
    printf("[ICE] Local candidate: %s\n", candidate_sdp);
    
    /* TODO: Send to browser via signaling */
    /* For now, print for manual copy-paste */
    printf(">>> Send this candidate to browser:\n%s\n", candidate_sdp);
}

static void on_ice_state(ice_state_t state, void *user_data) {
    (void)user_data;
    
    const char *state_names[] = {
        "NEW", "GATHERING", "CONNECTING", "CONNECTED",
        "COMPLETED", "FAILED", "DISCONNECTED", "CLOSED"
    };
    
    printf("[ICE] State: %s\n", state_names[state]);
}

static void on_peer_state(turbo_dc_peer_t *peer, turbo_dc_state_t old_state,
                           turbo_dc_state_t new_state, void *user_data) {
    (void)peer;
    (void)old_state;
    (void)user_data;
    
    const char *state_names[] = {
        "NEW", "CONNECTING", "CONNECTED", "DISCONNECTING", "CLOSED", "FAILED"
    };
    
    printf("[DataChannel] State: %s\n", state_names[new_state]);
    
    if (new_state == TURBO_DC_STATE_CONNECTED) {
        printf("[DataChannel] ✓ Connected! DTLS handshake complete\n");
    }
}

static void on_channel_open(turbo_dc_channel_t *channel, void *user_data) {
    (void)user_data;
    
    printf("[Channel] ✓ Opened: %s\n", turbo_dc_channel_get_label(channel));
    
    /* Send test message */
    const char *msg = "Hello from TurboNet!";
    turbo_dc_channel_send(channel, msg, strlen(msg), 0);
    printf("[Channel] Sent: %s\n", msg);
}

static void on_channel_message(turbo_dc_channel_t *channel, const void *data,
                                size_t len, int is_binary, void *user_data) {
    (void)channel;
    (void)user_data;
    
    if (is_binary) {
        printf("[Channel] Received binary: %zu bytes\n", len);
    } else {
        printf("[Channel] Received text: %.*s\n", (int)len, (const char *)data);
        
        /* Echo back */
        turbo_dc_channel_send(channel, data, len, is_binary);
    }
}

static void on_incoming_channel(turbo_dc_peer_t *peer, turbo_dc_channel_t *channel,
                                 void *user_data) {
    (void)peer;
    (void)user_data;
    
    printf("[Channel] Incoming channel: %s\n", turbo_dc_channel_get_label(channel));
    
    turbo_dc_channel_on_open(channel, on_channel_open);
    turbo_dc_channel_on_message(channel, on_channel_message);
}

static int sdp_candidate_to_string(const sdp_candidate_t *candidate, char *buffer,
                                   size_t buffer_size) {
    int len = snprintf(buffer, buffer_size, "candidate:%s %d %s %u %s %u typ %s",
                       candidate->foundation, candidate->component, candidate->transport,
                       candidate->priority, candidate->address, candidate->port,
                       candidate->type);
    if (len < 0 || (size_t)len >= buffer_size) {
        return -1;
    }

    if (candidate->rel_addr[0] != '\0' && candidate->rel_port != 0) {
        int extra = snprintf(buffer + len, buffer_size - (size_t)len, " raddr %s rport %u",
                             candidate->rel_addr, candidate->rel_port);
        if (extra < 0 || (size_t)extra >= buffer_size - (size_t)len) {
            return -1;
        }
    }

    return 0;
}

static int add_candidate_to_sdp_media(sdp_media_t *media, const char *candidate_sdp) {
    ice_candidate_t ice_candidate;
    sdp_candidate_t sdp_candidate;

    if (!media || !candidate_sdp || ice_candidate_parse(candidate_sdp, &ice_candidate) != 0) {
        return -1;
    }

    memset(&sdp_candidate, 0, sizeof(sdp_candidate));
    strncpy(sdp_candidate.foundation, ice_candidate.foundation, sizeof(sdp_candidate.foundation) - 1);
    sdp_candidate.component = ice_candidate.component_id;
    strncpy(sdp_candidate.transport,
            ice_candidate.transport == ICE_TRANSPORT_TCP ? "tcp" : "udp",
            sizeof(sdp_candidate.transport) - 1);
    sdp_candidate.priority = ice_candidate.priority;
    strncpy(sdp_candidate.address, ice_candidate.ip, sizeof(sdp_candidate.address) - 1);
    sdp_candidate.port = ice_candidate.port;
    strncpy(sdp_candidate.type, ice_candidate_type_name(ice_candidate.type),
            sizeof(sdp_candidate.type) - 1);
    strncpy(sdp_candidate.rel_addr, ice_candidate.related_ip, sizeof(sdp_candidate.rel_addr) - 1);
    sdp_candidate.rel_port = ice_candidate.related_port;

    return sdp_media_add_candidate(media, &sdp_candidate);
}

static void print_sdp_for_copy_paste(const char *sdp) {
    if (!sdp) {
        return;
    }

    const char *line = sdp;
    int emitted_candidate = 0;
    while (*line) {
        const char *next = strchr(line, '\n');
        size_t len = next ? (size_t)(next - line) : strlen(line);

        while (len > 0 && line[len - 1] == '\r') {
            --len;
        }

        if (!(len == 10 && strncmp(line, "a=sendrecv", len) == 0)) {
            fwrite(line, 1, len, stdout);
            fputc('\n', stdout);
        }

        if (len > 12 && strncmp(line, "a=candidate:", 12) == 0) {
            emitted_candidate = 1;
        }

        if (len > 15 && strncmp(line, "a=group:BUNDLE", 14) == 0) {
            fputs("a=extmap-allow-mixed\n", stdout);
            fputs("a=msid-semantic: WMS\n", stdout);
        } else if (len > 10 && strncmp(line, "a=ice-pwd:", 10) == 0) {
            fputs("a=ice-options:trickle\n", stdout);
        }

        if (!next) {
            break;
        }
        line = next + 1;
    }

    if (emitted_candidate) {
        fputs("a=end-of-candidates\n", stdout);
    }

    if (sdp[0] != '\0' && sdp[strlen(sdp) - 1] != '\n') {
        fputc('\n', stdout);
    }
}

/* ============================================================================
 * SDP Exchange
 * ============================================================================ */

static int print_local_answer(turbo_dc_context_t *ctx, ice_integration_ctx_t *ice,
                              const sdp_session_t *remote_sdp) {
    sdp_session_t sdp;
    sdp_session_init(&sdp);
    
    /* Add application media (DataChannel) */
    sdp_media_t *media = sdp_add_datachannel(&sdp, "0", 5000);
    if (!media) {
        fprintf(stderr, "Failed to add application media\n");
        return -1;
    }
    
    /* Set ICE credentials */
    char ufrag[32], pwd[64];
    ice_integration_get_local_credentials(ice, ufrag, sizeof(ufrag), pwd, sizeof(pwd));
    sdp_media_set_ice(media, ufrag, pwd);
    
    /* Set DTLS fingerprint */
    char fp_hash[32], fp[128];
    turbo_dc_context_get_local_fingerprint(ctx, fp_hash, sizeof(fp_hash), fp, sizeof(fp));
    for (char *p = fp; *p; ++p) {
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            memmove(p, p + 1, strlen(p));
            --p;
        }
    }
    sdp_media_set_fingerprint(media, fp_hash, fp);
    
    /* Match Chrome's DataChannel answer shape for SDP parser compatibility. */
    media->setup = SDP_ROLE_ACTIVE;
    
    /* Set SCTP port */
    media->sctp_port = 5000;
    media->max_message_size = 0;

    for (int i = 0; i < g_local_candidate_count; ++i) {
        if (add_candidate_to_sdp_media(media, g_local_candidates[i]) != 0) {
            fprintf(stderr, "Failed to add local candidate to SDP answer: %s\n",
                    g_local_candidates[i]);
        }
    }
    
    /* Generate SDP string */
    char sdp_buf[4096];
    int len = sdp_generate_answer(&sdp, remote_sdp, sdp_buf, sizeof(sdp_buf));
    
    if (len > 0) {
        printf("\n=== Local SDP Answer (copy to browser) ===\n");
        printf("-----BEGIN SDP ANSWER-----\n");
        print_sdp_for_copy_paste(sdp_buf);
        printf("-----END SDP ANSWER-----\n");
        printf("========================================\n\n");
        return 0;
    }

    fprintf(stderr, "Failed to generate local SDP answer\n");
    return -1;
}

static int parse_remote_sdp(const char *sdp_str, ice_integration_ctx_t *ice,
                             turbo_dc_peer_t *peer, sdp_session_t *sdp) {
    
    if (sdp_parse(sdp_str, strlen(sdp_str), sdp) != 0) {
        fprintf(stderr, "Failed to parse SDP\n");
        return -1;
    }
    
    /* Find application media */
    sdp_media_t *media = sdp_find_media_by_type(sdp, SDP_MEDIA_APPLICATION);
    if (!media) {
        fprintf(stderr, "No application media in SDP\n");
        return -1;
    }
    
    /* Set remote ICE credentials */
    ice_integration_set_remote_credentials(ice, media->ice_ufrag, media->ice_pwd);
    
    /* Set remote DTLS fingerprint */
    turbo_dc_peer_set_remote_fingerprint(peer, media->fingerprint_hash, media->fingerprint);
    
    /* Add remote ICE candidates */
    for (int i = 0; i < media->candidate_count; i++) {
        char candidate_str[512];
        if (sdp_candidate_to_string(&media->candidates[i], candidate_str,
                                    sizeof(candidate_str)) == 0) {
            ice_integration_add_remote_candidate(ice, candidate_str);
        }
    }
    
    printf("[SDP] Parsed remote SDP successfully\n");
    printf("[SDP] Remote ICE ufrag: %s\n", media->ice_ufrag);
    printf("[SDP] Remote fingerprint: %s %s\n", media->fingerprint_hash, media->fingerprint);
    printf("[SDP] Remote candidates in SDP: %d\n", media->candidate_count);
    
    return 0;
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char **argv) {
    int use_stun = 1;

    if (argc > 1 &&
        (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        printf("Usage: %s [--no-stun]\n", argv[0]);
        printf("Starts a browser-interoperable WebRTC DataChannel answerer.\n");
        return 0;
    }

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--no-stun") == 0) {
            use_stun = 0;
        }
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    printf("=== TurboNet WebRTC Browser Interoperability Test ===\n\n");
    
    /* Create event loop */
    turbo_loop_t *loop = turbo_loop_create();
    if (!loop) {
        fprintf(stderr, "Failed to create event loop\n");
        return 1;
    }
    
    /* Create DataChannel context (answerer mode for browser compatibility) */
    turbo_dc_config_t dc_config = {
        .is_server = 1,  /* Answerer */
        .transport = TURBO_DC_TRANSPORT_ICE
    };
    
    turbo_dc_context_t *ctx = turbo_dc_context_create(&dc_config);
    if (!ctx) {
        fprintf(stderr, "Failed to create DataChannel context\n");
        return 1;
    }
    
    /* Create peer */
    g_peer = turbo_dc_peer_create(ctx, NULL, 0, NULL);
    if (!g_peer) {
        fprintf(stderr, "Failed to create peer\n");
        turbo_dc_context_destroy(ctx);
        return 1;
    }
    
    turbo_dc_peer_on_state(g_peer, on_peer_state);
    turbo_dc_peer_on_channel(g_peer, on_incoming_channel);
    turbo_dc_peer_set_dtls_role(g_peer, 0);
    
    /* Create ICE integration with public STUN servers */
    const char *stun_servers[] = {
        "stun:stun.l.google.com:19302",
        "stun:stun1.l.google.com:19302"
    };
    
    g_ice = ice_integration_create(
        g_peer, loop,
        use_stun ? stun_servers : NULL, use_stun ? 2 : 0,
        NULL, NULL, NULL, 0
    );
    
    if (!g_ice) {
        fprintf(stderr, "Failed to create ICE integration\n");
        turbo_dc_peer_destroy(g_peer);
        turbo_dc_context_destroy(ctx);
        return 1;
    }
    
    ice_integration_on_candidate(g_ice, on_ice_candidate, NULL);
    ice_integration_on_state_change(g_ice, on_ice_state, NULL);
    
    /* Start ICE gathering */
    printf("[ICE] Starting candidate gathering...\n");
    if (ice_integration_start_gathering(g_ice) != 0) {
        fprintf(stderr, "Failed to start ICE gathering\n");
        ice_integration_destroy(g_ice);
        turbo_dc_peer_destroy(g_peer);
        turbo_dc_context_destroy(ctx);
        turbo_loop_destroy(loop);
        return 1;
    }

    printf("[ICE] Waiting for candidate gathering to complete...\n");
    while (g_running && !ice_integration_is_gathering_complete(g_ice)) {
        turbo_loop_poll(loop, 50, 1);
        ice_integration_poll(g_ice);
    }
    
    /* Wait for user to paste remote browser offer */
    printf("\nPaste browser SDP offer after ICE gathering completes (end with empty line):\n");
    
    char sdp_buf[8192] = {0};
    char line[512];
    size_t sdp_len = 0;
    
    while (fgets(line, sizeof(line), stdin)) {
        if (line[0] == '\n' || line[0] == '\r') {
            break;  /* Empty line signals end */
        }
        
        size_t line_len = strlen(line);
        if (sdp_len + line_len < sizeof(sdp_buf)) {
            memcpy(sdp_buf + sdp_len, line, line_len);
            sdp_len += line_len;
        }
    }
    
    if (sdp_len > 0) {
        sdp_session_t remote_sdp;

        printf("\n[SDP] Received %zu bytes\n", sdp_len);
        
        if (parse_remote_sdp(sdp_buf, g_ice, g_peer, &remote_sdp) == 0) {
            printf("[SDP] ✓ Remote SDP parsed successfully\n");

            if (print_local_answer(ctx, g_ice, &remote_sdp) != 0) {
                ice_integration_destroy(g_ice);
                turbo_dc_peer_destroy(g_peer);
                turbo_dc_context_destroy(ctx);
                turbo_loop_destroy(loop);
                return 1;
            }
            
            /* Signal end of candidates */
            ice_integration_end_of_candidates(g_ice);
            
            printf("\n[Status] Waiting for connection...\n");
            printf("Press Ctrl+C to exit\n\n");
            
            /* Run event loop */
            while (g_running) {
                turbo_loop_poll(loop, 50, 1);
                ice_integration_poll(g_ice);
            }
        }
    }
    
    /* Cleanup */
    printf("\n[Cleanup] Shutting down...\n");
    
    ice_integration_destroy(g_ice);
    turbo_dc_peer_destroy(g_peer);
    turbo_dc_context_destroy(ctx);
    
    turbo_loop_destroy(loop);
    
    printf("[Cleanup] ✓ Done\n");
    
    return 0;
}
