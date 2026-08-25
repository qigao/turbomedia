/* test_sfu_minimal_environment.c - Minimal SFU + signaling environment.
 *
 * Spawns the real sfu_node executable, provisions a room through its control
 * API, then exercises the WHIP (publish) and WHEP (subscribe) media signaling
 * endpoints with minimal audio SDP offers. The SFU must return valid SDP
 * answers and create the media sessions. This verifies the signaling path of
 * the "SFU + signaling" minimal environment that the IVR bot media transport
 * plugs into (actual ICE/DTLS/SRTP media bytes require a second peer and are
 * a follow-up). */
#include "ivr_thread.h"
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

#define TEST_SFU_HTTP_PORT 17931
#define TEST_CTRL_TOKEN "sfu-control-token"
#define TEST_MEDIA_TOKEN "sfu-media-token"

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

/* ------------------------------------------------------------------ */
/* spawn / cleanup                                                     */
/* ------------------------------------------------------------------ */

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
    return -1; /* POSIX spawn not exercised on this host */
#endif
}

static void kill_child(child_t *child) {
    if (!child || child->handle == NULL) {
        return;
    }
#ifdef _WIN32
    TerminateProcess(child->handle, 0);
    CloseHandle(child->handle);
#else
    kill(child->handle, SIGKILL);
#endif
    child->handle = NULL;
    free(child->stdout_path);
    child->stdout_path = NULL;
}

/* ------------------------------------------------------------------ */
/* raw-socket HTTP POST                                                */
/* ------------------------------------------------------------------ */

static int http_post(const char *path, const char *token,
                     const char *content_type, const char *body,
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
    addr.sin_port = htons((unsigned short)TEST_SFU_HTTP_PORT);
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
    addr.sin_port = htons((unsigned short)TEST_SFU_HTTP_PORT);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sock);
        return -1;
    }
#endif
    char req[8192];
    int n = snprintf(
        req, sizeof(req),
        "POST %s HTTP/1.1\r\n"
        "Host: 127.0.0.1:%d\r\n"
        "Authorization: Bearer %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        path, TEST_SFU_HTTP_PORT, token, content_type, strlen(body), body);
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

/* ------------------------------------------------------------------ */
/* fixtures                                                            */
/* ------------------------------------------------------------------ */

static child_t g_sfu;
static char g_cfg_path[1024];
static char g_out_path[1024];
static int g_seq = 0;

static const char *kAudioOfferBase =
    "v=0\r\n"
    "o=- 1 1 IN IP4 0.0.0.0\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=ice-ufrag:testufrag0\r\n"
    "a=ice-pwd:testpassword000000000000\r\n"
    "a=fingerprint:sha-256 "
    "00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:"
    "00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF\r\n"
    "a=setup:actpass\r\n"
    "a=mid:0\r\n"
    "a=rtcp-mux\r\n"
    "a=rtpmap:111 opus/48000/2\r\n";

static char g_offer_buf[1024];

static const char *audio_offer_sendonly(void) {
    snprintf(g_offer_buf, sizeof(g_offer_buf),
             "%sa=sendonly\r\na=ssrc:424242 cname:minimal-whip\r\n",
             kAudioOfferBase);
    return g_offer_buf;
}

static const char *audio_offer_recvonly(void) {
    snprintf(g_offer_buf, sizeof(g_offer_buf), "%sa=recvonly\r\n",
             kAudioOfferBase);
    return g_offer_buf;
}

void setUp(void) {
    memset(&g_sfu, 0, sizeof(g_sfu));
    snprintf(g_cfg_path, sizeof(g_cfg_path), "%s/sfu_min_%d.toml", TEST_BIN_DIR,
             ++g_seq);
    snprintf(g_out_path, sizeof(g_out_path), "%s/sfu_min_%d.out", TEST_BIN_DIR,
             g_seq);
    FILE *cfg = fopen(g_cfg_path, "wb");
    check_not_null(cfg);
    fprintf(cfg,
            "[server]\nhost = \"127.0.0.1\"\nport = %d\nuse_tls = false\n"
            "node_id = \"sfu-min-1\"\n"
            "[capacity]\nmax_rooms = 16\ndefault_room_capacity = 8\n"
            "[control]\ntoken = \"%s\"\n"
            "[media]\naccess_token = \"%s\"\n"
            "[ice]\nallow_loopback = true\n",
            TEST_SFU_HTTP_PORT, TEST_CTRL_TOKEN, TEST_MEDIA_TOKEN);
    fclose(cfg);

    char bin[1024];
    snprintf(bin, sizeof(bin), "%s/%s", TEST_BIN_DIR, SFU_NODE_BIN);
    const char *args[] = {"--config", g_cfg_path, NULL};
    check_equal((int)(spawn_with_stdout(bin, args, g_out_path, &g_sfu)), (int)(0));
    proc_sleep(2000); /* let the SFU bind HTTP + start the WebRTC worker */

    /* provision the room through the control API (retry until up) */
    char resp[4096];
    int ok = 0;
    for (int i = 0; i < 10 && !ok; i++) {
        memset(resp, 0, sizeof(resp));
        http_post("/api/v1/commands", TEST_CTRL_TOKEN, "application/json",
                  "{\"type\":\"attach_room\",\"room_id\":\"room-42\","
                  "\"max_participants\":8}",
                  resp, sizeof(resp));
        if (strstr(resp, " 200 ")) {
            ok = 1;
        }
        if (!ok) {
            proc_sleep(500);
        }
    }
    check_true(ok);
}

void tearDown(void) {
    kill_child(&g_sfu);
    remove(g_cfg_path);
    remove(g_out_path);
}

void test_whip_publish_offer_answer(void) {
    char resp[8192];
    memset(resp, 0, sizeof(resp));
    check_equal((int)(http_post("/whip/room-42/ivr-bot",
                                       TEST_MEDIA_TOKEN, "application/sdp",
                                       audio_offer_sendonly(), resp,
                                       sizeof(resp))), (int)(0));
    if (!strstr(resp, " 201 ")) {
        fprintf(stderr, "WHIP response: %s\n", resp);
    }
    check_not_null(strstr(resp, " 201 "));
    /* the answer must be a valid SDP with an audio media line and DTLS/ICE */
    check_not_null(strstr(resp, "m=audio"));
    check_not_null(strstr(resp, "a=setup:"));
    check_not_null(strstr(resp, "a=ice-ufrag"));
}

void test_whep_subscribe_offer_answer(void) {
    char resp[8192];
    memset(resp, 0, sizeof(resp));
    check_equal((int)(http_post("/whep/room-42/ivr-bot-caller-audio",
                                       TEST_MEDIA_TOKEN, "application/sdp",
                                       audio_offer_recvonly(), resp,
                                       sizeof(resp))), (int)(0));
    check_not_null(strstr(resp, " 201 "));
    check_not_null(strstr(resp, "m=audio"));
    check_not_null(strstr(resp, "a=setup:"));
}

spec("test_sfu_minimal_environment") {
  before_each() { setUp(); }
  after_each() { tearDown(); }

  it("test_whip_publish_offer_answer") { test_whip_publish_offer_answer(); };
  it("test_whep_subscribe_offer_answer") { test_whep_subscribe_offer_answer(); };
}
