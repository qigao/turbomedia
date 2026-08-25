/* test_ivr_whip_transport.c - Real WHIP/WHEP transports against a live SFU.
 *
 * Spawns the real sfu_node, attaches a room, then ivr_whip_transport opens a
 * WebRTC HTTP Ingestion publish connection: offer -> answer -> ICE trickle ->
 * DTLS/SRTP connected. Once connected, TTS PCM frames are pushed through
 * turbo_media_track_send_speech_frame. The WHEP half uses the publisher's
 * real SSRC and an authoritative desired subscription, then verifies that
 * routed PCM is decoded and delivered through the receive callback. */
#include "ivr_thread.h"
#include "ivr_dtmf_rtp.h"
#include "ivr_whep_transport.h"
#include "ivr_whip_transport.h"
#include "tinytest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TEST_BIN_DIR
#define TEST_BIN_DIR "."
#endif
#ifndef SFU_NODE_BIN
#define SFU_NODE_BIN "sfu_node"
#endif

#define TEST_SFU_PORT 17932
#define TEST_CTRL_TOKEN "sfu-control-token"
#define TEST_MEDIA_TOKEN "sfu-media-token"
#define TEST_MEDIA_DISCONNECT_TIMEOUT_MS 40000u
#define TEST_MEDIA_STATE_POLL_MS 25u

#ifdef _WIN32
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
typedef HANDLE proc_handle_t;
static void proc_sleep(unsigned int ms) { Sleep(ms); }
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
typedef pid_t proc_handle_t;
static void proc_sleep(unsigned int ms) { usleep(ms * 1000); }
#endif

typedef struct {
    proc_handle_t handle;
    char *stdout_path;
} child_t;

static int spawn_with_stdout(const char *exe, const char *const *args,
                             const char *stdout_path, child_t *out) {
#ifdef _WIN32
    char cmdline[4096];
    size_t off = 0;
    off += (size_t)snprintf(cmdline + off, sizeof(cmdline) - off, "\"%s\"",
                            exe);
    for (int i = 0; args[i] && off < sizeof(cmdline); i++) {
        off += (size_t)snprintf(cmdline + off, sizeof(cmdline) - off, " \"%s\"",
                                args[i]);
    }
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE out_file = CreateFileA(stdout_path, GENERIC_WRITE, FILE_SHARE_READ,
                                  &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                  NULL);
    if (out_file == INVALID_HANDLE_VALUE) {
        return -1;
    }
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out_file;
    si.hStdError = out_file;
    BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL,
                             &si, &pi);
    CloseHandle(out_file);
    if (!ok) {
        return -1;
    }
    CloseHandle(pi.hThread);
    out->handle = pi.hProcess;
    out->stdout_path = _strdup(stdout_path);
    return 0;
#else
    (void)exe;
    (void)args;
    (void)stdout_path;
    (void)out;
    return -1;
#endif
}

static void kill_child(child_t *child) {
    if (!child || child->handle == NULL) {
        return;
    }
#ifdef _WIN32
    TerminateProcess(child->handle, 0);
    WaitForSingleObject(child->handle, 5000);
    CloseHandle(child->handle);
#else
    kill(child->handle, SIGKILL);
#endif
    child->handle = NULL;
    free(child->stdout_path);
    child->stdout_path = NULL;
}

static void print_child_output(const char *path) {
    FILE *stream;
    char line[512];
    if (!path) {
        return;
    }
    stream = fopen(path, "rb");
    if (!stream) {
        return;
    }
    while (fgets(line, sizeof(line), stream)) {
        fputs(line, stderr);
    }
    fclose(stream);
}

static int http_post(const char *path, const char *token, const char *body,
                     char *resp, size_t resp_cap) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        WSACleanup();
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)TEST_SFU_PORT);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(sock);
        WSACleanup();
        return -1;
    }
#else
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)TEST_SFU_PORT);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sock);
        return -1;
    }
#endif
    char req[2048];
    int n = snprintf(req, sizeof(req),
                     "POST %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n"
                     "Authorization: Bearer %s\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                     path, TEST_SFU_PORT, token, strlen(body), body);
#ifdef _WIN32
    send(sock, req, (int)n, 0);
    size_t total = 0;
    while (total + 1 < resp_cap) {
        int r = recv(sock, resp + total, (int)(resp_cap - 1 - total), 0);
        if (r <= 0) {
            break;
        }
        total += (size_t)r;
    }
    resp[total] = '\0';
    closesocket(sock);
    WSACleanup();
#else
    write(sock, req, (size_t)n);
    size_t total = 0;
    while (total + 1 < resp_cap) {
        ssize_t r = read(sock, resp + total, resp_cap - 1 - total);
        if (r <= 0) {
            break;
        }
        total += (size_t)r;
    }
    resp[total] = '\0';
    close(sock);
#endif
    return 0;
}

static int disconnect_media_participant(const char *participant_id) {
    char command[512];
    char response[4096];
    int command_length;

    if (!participant_id) {
        return -1;
    }
    command_length = snprintf(
        command, sizeof(command),
        "{\"type\":\"disconnect_media_participant\","
        "\"room_id\":\"room-42\",\"participant_id\":\"%s\"}",
        participant_id);
    if (command_length <= 0 || (size_t)command_length >= sizeof(command)) {
        return -1;
    }
    memset(response, 0, sizeof(response));
    if (http_post("/api/v1/commands", TEST_CTRL_TOKEN, command, response,
                  sizeof(response)) != 0) {
        return -1;
    }
    return strstr(response, " 200 ") ? 0 : -1;
}

static child_t g_sfu;
static char g_cfg_path[1024];
static char g_out_path[1024];
static int g_seq = 0;
static ivr_whip_transport_t *g_transport = NULL;
static ivr_whep_transport_t *g_whep_transport = NULL;
static uint64_t g_whep_audio_frames = 0;
static ivr_dtmf_ingress_t *g_dtmf_ingress = NULL;
static ivr_mutex_t g_dtmf_lock;
static ivr_mutex_t g_media_state_lock;
static ivr_dtmf_input_t g_dtmf_input;
static unsigned g_dtmf_finals = 0;

typedef struct {
    unsigned connecting;
    unsigned connected;
    unsigned disconnected;
    unsigned failed;
    unsigned closed;
    unsigned input_stalled;
    unsigned invalid_identity;
    uint64_t last_generation;
} media_state_counts_t;
static media_state_counts_t g_whip_states;
static media_state_counts_t g_whep_states;

static const ivr_call_ref_t g_call = {
    .tenant_id = {"tenant-42", 9},
    .provider_session_id = {"session-42", 10},
    .dialog_id = {"dialog-42", 9},
    .room_id = {"room-42", 7},
    .call_id = {"call-42", 7},
    .call_generation = 1,
    .expected_room_version = 1};

static int view_equal(const ivr_bytes_view_t *left,
                      const ivr_bytes_view_t *right) {
    return left && right && left->size == right->size &&
           (left->size == 0u ||
            (left->data && right->data &&
             memcmp(left->data, right->data, left->size) == 0));
}

static int call_identity_matches_fixture(const ivr_call_ref_t *call) {
    return call && call->call_generation == g_call.call_generation &&
           call->expected_room_version == g_call.expected_room_version &&
           view_equal(&call->tenant_id, &g_call.tenant_id) &&
           view_equal(&call->provider_session_id,
                      &g_call.provider_session_id) &&
           view_equal(&call->dialog_id, &g_call.dialog_id) &&
           view_equal(&call->room_id, &g_call.room_id) &&
           view_equal(&call->call_id, &g_call.call_id);
}

static int spawn_sfu(void) {
    char bin[1024];
    const char *args[] = {"--config", g_cfg_path, NULL};

    if (g_sfu.handle != NULL) {
        return 0;
    }
    snprintf(bin, sizeof(bin), "%s/%s", TEST_BIN_DIR, SFU_NODE_BIN);
    return spawn_with_stdout(bin, args, g_out_path, &g_sfu) == 0;
}

static int provision_room(void) {
    char response[4096];

    for (int attempt = 0; attempt < 10; ++attempt) {
        memset(response, 0, sizeof(response));
        if (http_post("/api/v1/commands", TEST_CTRL_TOKEN,
                      "{\"type\":\"attach_room\",\"room_id\":\"room-42\","
                      "\"max_participants\":8}",
                      response, sizeof(response)) == 0 &&
            strstr(response, " 200 ")) {
            return 1;
        }
        proc_sleep(500);
    }
    fprintf(stderr, "last attach_room response:\n%s\n", response);
    return 0;
}

static int wait_whip_connected(void) {
    for (int attempt = 0; attempt < 800; ++attempt) {
        if (ivr_whip_transport_connected(g_transport)) {
            return 1;
        }
        proc_sleep(TEST_MEDIA_STATE_POLL_MS);
    }
    return 0;
}

static int wait_whep_connected(void) {
    for (int attempt = 0; attempt < 800; ++attempt) {
        if (ivr_whep_transport_connected(g_whep_transport)) {
            return 1;
        }
        proc_sleep(TEST_MEDIA_STATE_POLL_MS);
    }
    return 0;
}

static int configure_audio_route(uint32_t publisher_ssrc) {
    char command[1024];
    char response[4096];
    int command_length = snprintf(
        command, sizeof(command),
        "{\"type\":\"register_published_track\","
        "\"room_id\":\"room-42\",\"participant_id\":\"call-42\","
        "\"track_id\":\"caller-audio-42\",\"main_ssrc\":%u,"
        "\"layer_ssrcs\":[%u],\"kind\":\"audio\","
        "\"codec_name\":\"opus\"}",
        publisher_ssrc, publisher_ssrc);

    if (command_length <= 0 ||
        (size_t)command_length >= sizeof(command)) {
        return 0;
    }
    memset(response, 0, sizeof(response));
    if (http_post("/api/v1/commands", TEST_CTRL_TOKEN, command, response,
                  sizeof(response)) != 0 ||
        !strstr(response, " 200 ")) {
        return 0;
    }
    memset(response, 0, sizeof(response));
    return http_post(
               "/api/v1/commands", TEST_CTRL_TOKEN,
               "{\"type\":\"set_track_subscription\","
               "\"room_id\":\"room-42\","
               "\"receiver_participant_id\":\"call-42-rx\","
               "\"track_id\":\"caller-audio-42\",\"enabled\":true,"
               "\"muted\":false,\"policy_source\":\"ivr\"}",
               response, sizeof(response)) == 0 &&
           strstr(response, " 200 ") != NULL;
}

static int send_audio_frames(unsigned frame_count) {
    ivr_media_transport_t publisher;
    static int16_t samples[320];

    ivr_whip_transport_get_transport(g_transport, &publisher);
    for (int index = 0; index < 320; ++index) {
        samples[index] = (int16_t)(800 + (index % 11) * 100);
    }
    for (unsigned frame = 0; frame < frame_count; ++frame) {
        if (publisher.play_audio(
                publisher.context, &g_call, (const uint8_t *)samples,
                sizeof(samples), 16000) != 0) {
            return 0;
        }
        proc_sleep(20);
    }
    return 1;
}

static int wait_whep_frames_greater_than(uint64_t baseline) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (g_whep_audio_frames > baseline) {
            return 1;
        }
        proc_sleep(TEST_MEDIA_STATE_POLL_MS);
    }
    return 0;
}

static int wait_sfu_room_empty(void) {
    char response[4096];

    for (int attempt = 0; attempt < 40; ++attempt) {
        memset(response, 0, sizeof(response));
        if (http_post("/api/v1/commands", TEST_CTRL_TOKEN,
                      "{\"type\":\"get_room_stats\","
                      "\"room_id\":\"room-42\"}",
                      response, sizeof(response)) == 0 &&
            strstr(response, " 200 ") &&
            strstr(response, "\"session_count\":0") &&
            strstr(response, "\"participant_count\":0")) {
            return 1;
        }
        proc_sleep(100);
    }
    return 0;
}

static unsigned media_terminal_count(const media_state_counts_t *counts) {
    unsigned total = 0;

    ivr_mutex_lock(&g_media_state_lock);
    if (counts) {
        total = counts->disconnected + counts->failed + counts->closed;
    }
    ivr_mutex_unlock(&g_media_state_lock);
    return total;
}

static media_state_counts_t media_state_snapshot(
    const media_state_counts_t *counts) {
    media_state_counts_t snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    ivr_mutex_lock(&g_media_state_lock);
    if (counts) {
        snapshot = *counts;
    }
    ivr_mutex_unlock(&g_media_state_lock);
    return snapshot;
}

static int wait_for_media_terminal(const media_state_counts_t *counts,
                                   unsigned baseline) {
    unsigned waited = 0;

    while (waited < TEST_MEDIA_DISCONNECT_TIMEOUT_MS) {
        if (media_terminal_count(counts) > baseline) {
            return 1;
        }
        proc_sleep(TEST_MEDIA_STATE_POLL_MS);
        waited += TEST_MEDIA_STATE_POLL_MS;
    }
    return 0;
}

static int on_whep_audio(void *context, const ivr_call_ref_t *call,
                         const uint8_t *pcm, size_t length,
                         uint32_t sample_rate, uint64_t rtp_timestamp) {
    uint64_t *frames = (uint64_t *)context;
    (void)rtp_timestamp;
    if (!frames || !call_identity_matches_fixture(call) || !pcm ||
        length == 0 || sample_rate == 0) {
        return -1;
    }
    (*frames)++;
    return 0;
}

static int on_whep_rtp(void *context, const ivr_call_ref_t *call,
                       uint8_t payload_type, uint32_t rtp_timestamp,
                       const uint8_t *payload, size_t payload_length,
                       uint64_t source_generation) {
    ivr_dtmf_input_t input;
    ivr_dtmf_ingress_t *ingress = (ivr_dtmf_ingress_t *)context;
    if (!ingress || !call_identity_matches_fixture(call) ||
        payload_type != 126u ||
        ivr_dtmf_ingress_submit_rtp(
            ingress, call, source_generation, rtp_timestamp, payload,
            payload_length, &input) != IVR_OK) {
        return -1;
    }
    ivr_mutex_lock(&g_dtmf_lock);
    g_dtmf_input = input;
    g_dtmf_finals++;
    ivr_mutex_unlock(&g_dtmf_lock);
    return 0;
}

static void on_media_state(void *context, const ivr_call_ref_t *call,
                           uint64_t attempt_generation,
                           ivr_media_link_state_t state, int error_code) {
    media_state_counts_t *counts = (media_state_counts_t *)context;
    (void)call;
    (void)error_code;
    if (!counts) {
        return;
    }
    ivr_mutex_lock(&g_media_state_lock);
    if (!call_identity_matches_fixture(call)) {
        counts->invalid_identity++;
        ivr_mutex_unlock(&g_media_state_lock);
        return;
    }
    counts->last_generation = attempt_generation;
    if (error_code == IVR_MEDIA_ERROR_INPUT_STALLED) {
        counts->input_stalled++;
    } else {
        switch (state) {
            case IVR_MEDIA_LINK_CONNECTING: counts->connecting++; break;
            case IVR_MEDIA_LINK_CONNECTED: counts->connected++; break;
            case IVR_MEDIA_LINK_DISCONNECTED: counts->disconnected++; break;
            case IVR_MEDIA_LINK_FAILED: counts->failed++; break;
            case IVR_MEDIA_LINK_CLOSED: counts->closed++; break;
            default: break;
        }
    }
    ivr_mutex_unlock(&g_media_state_lock);
}

void setUp(void) {
    memset(&g_sfu, 0, sizeof(g_sfu));
    memset(&g_whip_states, 0, sizeof(g_whip_states));
    memset(&g_whep_states, 0, sizeof(g_whep_states));
    memset(&g_dtmf_input, 0, sizeof(g_dtmf_input));
    g_dtmf_finals = 0;
    check_equal((int)(ivr_mutex_init(&g_dtmf_lock)), (int)(0));
    check_equal((int)(ivr_mutex_init(&g_media_state_lock)), (int)(0));
    ivr_dtmf_ingress_config_t dtmf_config;
    memset(&dtmf_config, 0, sizeof(dtmf_config));
    dtmf_config.window_capacity = 1;
    check_equal(ivr_dtmf_ingress_create(&dtmf_config,
                                              &g_dtmf_ingress), IVR_OK);
    check_equal(ivr_dtmf_ingress_begin_input(g_dtmf_ingress, &g_call,
                                                   "w1", 1), IVR_OK);
    snprintf(g_cfg_path, sizeof(g_cfg_path), "%s/whip_%d.toml", TEST_BIN_DIR,
             ++g_seq);
    snprintf(g_out_path, sizeof(g_out_path), "%s/whip_%d.out", TEST_BIN_DIR,
             g_seq);
    FILE *cfg = fopen(g_cfg_path, "wb");
    check_not_null(cfg);
    fprintf(cfg,
            "[server]\nhost = \"127.0.0.1\"\nport = %d\nuse_tls = false\n"
            "node_id = \"sfu-whip-1\"\n"
            "[capacity]\nmax_rooms = 16\ndefault_room_capacity = 8\n"
            "[control]\ntoken = \"%s\"\n"
            "[media]\naccess_token = \"%s\"\n"
            "[ice]\nallow_loopback = true\n"
            "[logging]\nlevel = \"trace\"\n",
            TEST_SFU_PORT, TEST_CTRL_TOKEN, TEST_MEDIA_TOKEN);
    fclose(cfg);

    check_true(spawn_sfu());
    proc_sleep(2000);

    int room_ready = provision_room();
    if (!room_ready) {
        kill_child(&g_sfu);
        print_child_output(g_out_path);
    }
    check_true(room_ready);

    char sfu_base_url[64];
    snprintf(sfu_base_url, sizeof(sfu_base_url), "http://127.0.0.1:%d",
             TEST_SFU_PORT);
    ivr_whip_transport_config_t tcfg;
    memset(&tcfg, 0, sizeof(tcfg));
    tcfg.sfu_base_url = sfu_base_url;
    tcfg.media_token = TEST_MEDIA_TOKEN;
    tcfg.allow_plaintext_loopback = 1;
    tcfg.allow_loopback = 1;
    tcfg.sample_rate = 16000;
    tcfg.connect_timeout_ms = 15000;
    tcfg.on_state = on_media_state;
    tcfg.state_context = &g_whip_states;
    check_equal(ivr_whip_transport_create(&tcfg, &g_transport), IVR_OK);
    check_not_null(g_transport);

    ivr_whep_transport_config_t wcfg;
    memset(&wcfg, 0, sizeof(wcfg));
    wcfg.sfu_base_url = sfu_base_url;
    wcfg.media_token = TEST_MEDIA_TOKEN;
    wcfg.allow_plaintext_loopback = 1;
    wcfg.allow_loopback = 1;
    wcfg.sample_rate = 16000;
    wcfg.connect_timeout_ms = 15000;
    wcfg.input_inactivity_timeout_ms = 250;
    wcfg.on_state = on_media_state;
    wcfg.state_context = &g_whep_states;
    wcfg.on_audio = on_whep_audio;
    wcfg.audio_context = &g_whep_audio_frames;
    wcfg.telephone_event_payload_type = 126u;
    wcfg.on_rtp = on_whep_rtp;
    wcfg.rtp_context = g_dtmf_ingress;
    g_whep_audio_frames = 0;
    check_equal(ivr_whep_transport_create(&wcfg, &g_whep_transport), IVR_OK);
    check_not_null(g_whep_transport);
}

void tearDown(void) {
    if (g_whep_transport) {
        ivr_whep_transport_destroy(g_whep_transport);
        g_whep_transport = NULL;
    }
    if (g_transport) {
        ivr_whip_transport_destroy(g_transport);
        g_transport = NULL;
    }
    ivr_dtmf_ingress_destroy(g_dtmf_ingress);
    g_dtmf_ingress = NULL;
    ivr_mutex_destroy(&g_dtmf_lock);
    ivr_mutex_destroy(&g_media_state_lock);
    kill_child(&g_sfu);
    remove(g_cfg_path);
    remove(g_out_path);
}

void test_whip_publish_connect_and_send_audio(void) {
    check_equal(ivr_whip_transport_start(g_transport, &g_call), IVR_OK);
    /* wait for ICE/DTLS to complete against the SFU */
    int connected = 0;
    for (int i = 0; i < 800 && !connected; i++) {
        connected = ivr_whip_transport_connected(g_transport);
        if (!connected) {
            proc_sleep(25);
        }
    }
    check_true(connected);
    media_state_counts_t whip_states = media_state_snapshot(&g_whip_states);
    check_true(whip_states.connecting >= 1);
    check_true(whip_states.connected >= 1);
    check_equal((uint64_t)(whip_states.invalid_identity), (uint64_t)(0u));

    /* push TTS PCM frames through the audio send track (via the bot's
       transport interface) */
    ivr_media_transport_t transport;
    ivr_whip_transport_get_transport(g_transport, &transport);
    static uint16_t samples[320];
    ivr_call_ref_t wrong_dialog = g_call;
    wrong_dialog.dialog_id.data = "dialog-other";
    wrong_dialog.dialog_id.size = strlen("dialog-other");
    for (int i = 0; i < 320; i++) {
        samples[i] = (uint16_t)(1000 + (i % 7) * 100);
    }
    check_equal((int)(transport.play_audio(transport.context, &wrong_dialog,
                                 (const uint8_t *)samples,
                                 sizeof(samples), 16000)), (int)(-1));
    check_equal((int)(transport.stop(transport.context, &wrong_dialog)), (int)(-1));
    check_true(ivr_whip_transport_connected(g_transport));
    for (int f = 0; f < 8; f++) {
        check_equal((int)(transport.play_audio(
                                     transport.context, &g_call,
                                     (const uint8_t *)samples,
                                     sizeof(samples), 16000)), (int)(0));
    }
    check_greater_equal(ivr_whip_transport_frames_sent(g_transport), 8u);

    /* the transport owns the WHIP session; tearDown stops + DELETEs it */
}

void test_create_copies_config_and_null_token(void) {
    /* config strings are deep-copied and a NULL media_token is allowed:
       short-lived caller buffers and the NULL bearer must not crash
       create/start/destroy (the HTTP builder treats a NULL token as an empty
       bearer value instead of hitting %s(NULL) undefined behavior). */
    ivr_whip_transport_t *t = NULL;
    char base_url[] = "http://127.0.0.1:17932";
    char token[] = "ignored";
    ivr_whip_transport_config_t tcfg;
    memset(&tcfg, 0, sizeof(tcfg));
    tcfg.sfu_base_url = base_url;
    tcfg.media_token = NULL; /* allowed: no media token configured */
    tcfg.allow_plaintext_loopback = 1;
    tcfg.allow_loopback = 1;
    tcfg.sample_rate = 16000;
    tcfg.connect_timeout_ms = 5000;
    check_equal(ivr_whip_transport_create(&tcfg, &t), IVR_OK);
    check_not_null(t);
    /* release the caller buffers to prove the transport owns its copies */
    base_url[0] = '\0';
    token[0] = '\0';
    /* start reaches the HTTP layer with a NULL media_token: it must not crash
       (the SFU rejects the empty bearer, so start fails without connecting) */
    (void)ivr_whip_transport_start(t, &g_call);
    ivr_whip_transport_destroy(t);
}

void test_whep_subscribe_connect_and_stop(void) {
    char response[4096];
    char command[1024];
    uint32_t publisher_ssrc;

    check_equal(ivr_whip_transport_start(g_transport, &g_call), IVR_OK);
    int publisher_connected = 0;
    for (int i = 0; i < 800 && !publisher_connected; i++) {
        publisher_connected = ivr_whip_transport_connected(g_transport);
        if (!publisher_connected) {
            proc_sleep(25);
        }
    }
    check_true(publisher_connected);
    publisher_ssrc = ivr_whip_transport_ssrc(g_transport);
    check_true(publisher_ssrc != 0);

    memset(response, 0, sizeof(response));
    int command_length = snprintf(
        command, sizeof(command),
        "{\"type\":\"register_published_track\","
        "\"room_id\":\"room-42\",\"participant_id\":\"call-42\","
        "\"track_id\":\"caller-audio-42\",\"main_ssrc\":%u,"
        "\"layer_ssrcs\":[%u],\"kind\":\"audio\","
        "\"codec_name\":\"opus\"}",
        publisher_ssrc, publisher_ssrc);
    check_true(command_length > 0 &&
                     (size_t)command_length < sizeof(command));
    check_equal((int)(http_post("/api/v1/commands", TEST_CTRL_TOKEN, command, response,
                     sizeof(response))), (int)(0));
    check_not_null(strstr(response, " 200 "));

    /* Desired subscription is accepted before the WHEP participant exists;
       sfu_node applies it atomically when the receiver session is created. */
    memset(response, 0, sizeof(response));
    check_equal((int)(http_post(
               "/api/v1/commands", TEST_CTRL_TOKEN,
               "{\"type\":\"set_track_subscription\","
               "\"room_id\":\"room-42\","
               "\"receiver_participant_id\":\"call-42-rx\","
               "\"track_id\":\"caller-audio-42\",\"enabled\":true,"
               "\"muted\":false,\"policy_source\":\"ivr\"}",
               response, sizeof(response))), (int)(0));
    check_not_null(strstr(response, " 200 "));

    check_equal(ivr_whep_transport_start(g_whep_transport, &g_call,
                                               "call-42-rx"), IVR_OK);
    int connected = 0;
    for (int i = 0; i < 800 && !connected; i++) {
        connected = ivr_whep_transport_connected(g_whep_transport);
        if (!connected) {
            proc_sleep(25);
        }
    }
    check_true(connected);
    media_state_counts_t whep_states = media_state_snapshot(&g_whep_states);
    check_true(whep_states.connecting >= 1);
    check_true(whep_states.connected >= 1);
    check_equal((uint64_t)(whep_states.invalid_identity), (uint64_t)(0u));

    /* The peer stays connected while caller media is silent. The WHEP poll
       loop must report that distinct failure without waiting for shutdown. */
    for (int i = 0; i < 80; ++i) {
        whep_states = media_state_snapshot(&g_whep_states);
        if (whep_states.input_stalled != 0) {
            break;
        }
        proc_sleep(25);
    }
    whep_states = media_state_snapshot(&g_whep_states);
    check_true(whep_states.input_stalled >= 1);
    check_true(ivr_whep_transport_connected(g_whep_transport));

    ivr_media_transport_t publisher;
    ivr_whip_transport_get_transport(g_transport, &publisher);
    static int16_t samples[320];
    for (int i = 0; i < 320; ++i) {
        samples[i] = (int16_t)(800 + (i % 11) * 100);
    }
    for (int i = 0; i < 30; ++i) {
        check_equal((int)(publisher.play_audio(publisher.context, &g_call,
                                    (const uint8_t *)samples,
                                    sizeof(samples), 16000)), (int)(0));
        proc_sleep(20);
    }
    for (int i = 0; i < 200 && g_whep_audio_frames == 0; ++i) {
        proc_sleep(25);
    }
    check_true(g_whep_audio_frames > 0);
    check_true(ivr_whep_transport_frames_received(g_whep_transport) > 0);
    whep_states = media_state_snapshot(&g_whep_states);
    check_equal((uint64_t)(whep_states.input_stalled), (uint64_t)(1u));
    check_equal((uint64_t)(ivr_whep_transport_frames_rejected(g_whep_transport)), (uint64_t)(0u));

    /* RFC 4733 final packets traverse the real WHIP -> SFU -> WHEP RTP
       path. Repeated end packets are normal on the wire but produce one
       canonical input for the active window. */
    static const uint8_t telephone_event[] = {
        0x80, 0x7e, 0x00, 0x01, 0x00, 0x01, 0x86, 0xa0,
        0x12, 0x34, 0x56, 0x78, 0x01, 0x80, 0x00, 0xa0};
    for (int i = 0; i < 3; ++i) {
        check_equal((int)(ivr_whip_transport_send_rtp_packet(
                   g_transport, telephone_event, sizeof(telephone_event))), (int)(0));
        proc_sleep(20);
    }
    for (int i = 0; i < 200; ++i) {
        ivr_mutex_lock(&g_dtmf_lock);
        unsigned finals = g_dtmf_finals;
        ivr_mutex_unlock(&g_dtmf_lock);
        if (finals > 0) {
            break;
        }
        proc_sleep(25);
    }
    ivr_mutex_lock(&g_dtmf_lock);
    check_equal((uint64_t)(g_dtmf_finals), (uint64_t)(1u));
    check_equal((int)(g_dtmf_input.digit), (int)('1'));
    check_equal(g_dtmf_input.input_id, "w1");
    check_equal((uint64_t)(g_dtmf_input.input_generation), (uint64_t)(1u));
    ivr_mutex_unlock(&g_dtmf_lock);

    /* Delete only the WHEP participant's owned session. Consent freshness
       must turn the remote loss into a terminal state without dropping WHIP. */
    unsigned whep_terminal_before = media_terminal_count(&g_whep_states);
    check_equal((int)(disconnect_media_participant("call-42-rx")), (int)(0));
    check_true(wait_for_media_terminal(&g_whep_states, whep_terminal_before));
    check_false(ivr_whep_transport_connected(g_whep_transport));
    check_true(ivr_whip_transport_connected(g_transport));

    check_equal((int)(ivr_whep_transport_stop(g_whep_transport, &g_call)), (int)(0));
    whep_states = media_state_snapshot(&g_whep_states);
    uint64_t whep_generation_before = whep_states.last_generation;
    check_equal(ivr_whep_transport_start(g_whep_transport, &g_call,
                                         "call-42-rx"), IVR_OK);
    connected = 0;
    for (int i = 0; i < 800 && !connected; ++i) {
        connected = ivr_whep_transport_connected(g_whep_transport);
        if (!connected) {
            proc_sleep(TEST_MEDIA_STATE_POLL_MS);
        }
    }
    check_true(connected);
    whep_states = media_state_snapshot(&g_whep_states);
    check_true(whep_states.last_generation > whep_generation_before);

    /* Repeat in the opposite direction. The WHEP peer is independently
       owned and must remain connected when the WHIP participant is removed. */
    unsigned whip_terminal_before = media_terminal_count(&g_whip_states);
    check_equal((int)(disconnect_media_participant("call-42")), (int)(0));
    check_true(wait_for_media_terminal(&g_whip_states, whip_terminal_before));
    check_false(ivr_whip_transport_connected(g_transport));
    check_true(ivr_whep_transport_connected(g_whep_transport));

    ivr_call_ref_t wrong_call = g_call;
    wrong_call.call_generation++;
    check_equal((int)(ivr_whep_transport_stop(g_whep_transport, &wrong_call)), (int)(-1));
    check_true(ivr_whep_transport_connected(g_whep_transport));
    check_equal((int)(ivr_whep_transport_stop(g_whep_transport, &g_call)), (int)(0));
    check_false(ivr_whep_transport_connected(g_whep_transport));
}

void test_sfu_restart_reconnects_same_call_and_resumes_rtp(void) {
    ivr_media_transport_t publisher;
    media_state_counts_t whip_before;
    media_state_counts_t whep_before;
    media_state_counts_t whip_after;
    media_state_counts_t whep_after;
    unsigned whip_terminal_before;
    unsigned whep_terminal_before;
    uint64_t frames_before_restart;
    uint64_t frames_after_reconnect;
    uint32_t publisher_ssrc;

    check_equal(ivr_whip_transport_start(g_transport, &g_call), IVR_OK);
    check_true(wait_whip_connected());
    publisher_ssrc = ivr_whip_transport_ssrc(g_transport);
    check_true(publisher_ssrc != 0u);
    check_true(configure_audio_route(publisher_ssrc));
    check_equal(ivr_whep_transport_start(g_whep_transport, &g_call,
                                         "call-42-rx"), IVR_OK);
    check_true(wait_whep_connected());
    check_true(send_audio_frames(30u));
    check_true(wait_whep_frames_greater_than(0u));

    whip_before = media_state_snapshot(&g_whip_states);
    whep_before = media_state_snapshot(&g_whep_states);
    whip_terminal_before = media_terminal_count(&g_whip_states);
    whep_terminal_before = media_terminal_count(&g_whep_states);
    frames_before_restart = g_whep_audio_frames;

    kill_child(&g_sfu);
    check_true(wait_for_media_terminal(&g_whip_states, whip_terminal_before));
    check_true(wait_for_media_terminal(&g_whep_states, whep_terminal_before));
    check_false(ivr_whip_transport_connected(g_transport));
    check_false(ivr_whep_transport_connected(g_whep_transport));

    check_equal((int)(ivr_whep_transport_stop(g_whep_transport, &g_call)), (int)(0));
    ivr_whip_transport_get_transport(g_transport, &publisher);
    check_not_null(publisher.stop);
    check_equal((int)(publisher.stop(publisher.context, &g_call)), (int)(0));

    check_true(spawn_sfu());
    check_true(provision_room());
    check_equal(ivr_whip_transport_start(g_transport, &g_call), IVR_OK);
    check_true(wait_whip_connected());
    publisher_ssrc = ivr_whip_transport_ssrc(g_transport);
    check_true(publisher_ssrc != 0u);
    check_true(configure_audio_route(publisher_ssrc));
    check_equal(ivr_whep_transport_start(g_whep_transport, &g_call,
                                         "call-42-rx"), IVR_OK);
    check_true(wait_whep_connected());

    whip_after = media_state_snapshot(&g_whip_states);
    whep_after = media_state_snapshot(&g_whep_states);
    check_true(whip_after.last_generation >
                     whip_before.last_generation);
    check_true(whep_after.last_generation >
                     whep_before.last_generation);
    check_equal((uint64_t)(whip_after.invalid_identity), (uint64_t)(0u));
    check_equal((uint64_t)(whep_after.invalid_identity), (uint64_t)(0u));

    frames_after_reconnect = g_whep_audio_frames;
    check_true(frames_after_reconnect >= frames_before_restart);
    check_true(send_audio_frames(30u));
    check_true(wait_whep_frames_greater_than(frames_after_reconnect));
    check_true(ivr_whip_transport_connected(g_transport));
    check_true(ivr_whep_transport_connected(g_whep_transport));

    check_equal((int)(ivr_whep_transport_stop(g_whep_transport, &g_call)), (int)(0));
    ivr_whip_transport_get_transport(g_transport, &publisher);
    check_equal((int)(publisher.stop(publisher.context, &g_call)), (int)(0));
    check_true(wait_sfu_room_empty());
}

spec("test_ivr_whip_transport") {
  before_each() { setUp(); }
  after_each() { tearDown(); }

  it("test_whip_publish_connect_and_send_audio") { test_whip_publish_connect_and_send_audio(); };
  it("test_create_copies_config_and_null_token") { test_create_copies_config_and_null_token(); };
  it("test_whep_subscribe_connect_and_stop") { test_whep_subscribe_connect_and_stop(); };
  it("test_sfu_restart_reconnects_same_call_and_resumes_rtp") { test_sfu_restart_reconnects_same_call_and_resumes_rtp(); };
}
