/* test_ivr_openai_provider.c - Remote TTS/ASR providers (OpenAI-compatible
 * protocol) against an in-process mock HTTP server.
 *
 * The mock server speaks the real wire protocol (JSON POST for TTS, raw PCM
 * reply; multipart/form-data POST for ASR, JSON {"text":...} reply), so the
 * tests exercise the actual http_client path, resampling, WAV packaging,
 * multipart encoding, JSON parsing, cancellation quiescence and error paths
 * without a live OpenAI endpoint or API key. */
#include "ivr_openai_provider.h"
#include "ivr_speech_session_factory.h"
#include "ivr_thread.h"
#include "tinytest.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#define mock_sleep_ms(ms) Sleep(ms)
typedef SOCKET mock_sock_t;
#define MOCK_INVALID_SOCKET INVALID_SOCKET
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#define mock_sleep_ms(ms) usleep((ms)*1000)
typedef int mock_sock_t;
#define MOCK_INVALID_SOCKET (-1)
#endif

#define MOCK_TTS_PATH "/v1/audio/speech"
#define MOCK_ASR_PATH "/v1/audio/transcriptions"
#define MOCK_API_KEY "test-openai-key"
#define MOCK_PCM_BYTES (24000u * 2u) /* 1 second at 24 kHz 16-bit mono */

static int fail_asr_create(void *context,
                           const ivr_openai_config_t *config,
                           ivr_openai_asr_t **out_asr) {
    int *calls = (int *)context;
    (void)config;
    if (calls) {
        (*calls)++;
    }
    if (out_asr) {
        *out_asr = NULL;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* in-process mock HTTP server                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    ivr_thread_t thread;
    mock_sock_t listen_sock;
    volatile int running;
    int port;
    /* request log */
    volatile int request_count;
    volatile int tts_requests;
    volatile int asr_requests;
    char last_method[32];
    char last_path[512];
    char last_auth[512];
    char last_content_type[256];
    uint8_t last_body[1 << 16];
    size_t last_body_len;
    /* response modes */
    int tts_status;
    int asr_status;
    const uint8_t *tts_pcm;
    size_t tts_pcm_len;
    const char *asr_json;
    const uint8_t *asr_match;
    size_t asr_match_len;
    const char *asr_match_json;
    size_t tts_frame_delay_ms; /* optional slow write to exercise cancel */
    size_t asr_delay_ms;        /* optional delay before the ASR reply */
} mock_server_t;

static int mock_bytes_contains(const uint8_t *haystack, size_t haystack_len,
                               const uint8_t *needle, size_t needle_len) {
    size_t i;
    if (!haystack || !needle || needle_len == 0 || needle_len > haystack_len) {
        return 0;
    }
    for (i = 0; i <= haystack_len - needle_len; ++i) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            return 1;
        }
    }
    return 0;
}

static void mock_server_init(mock_server_t *s) {
    memset(s, 0, sizeof(*s));
    s->listen_sock = MOCK_INVALID_SOCKET;
    s->tts_status = 200;
    s->asr_status = 200;
    s->asr_json = "{\"text\":\"hello remote world\"}";
    s->tts_pcm = NULL;
    s->tts_pcm_len = 0;
}

static int mock_net_init(void) {
#ifdef _WIN32
    static volatile int inited = 0;
    static WSADATA wsa;
    if (!inited) {
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            return -1;
        }
        inited = 1;
    }
    return 0;
#else
    return 0;
#endif
}

static void mock_close_sock(mock_sock_t fd) {
    if (fd == MOCK_INVALID_SOCKET) {
        return;
    }
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

static int mock_read_exact(mock_sock_t fd, uint8_t *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        int n = recv(fd, (char *)buf + off, (int)(len - off), 0);
        if (n <= 0) {
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

/* Read one request (headers + Content-Length body). Returns 0 on success. */
static int mock_read_request(mock_sock_t fd, char *hdr, size_t hdr_cap,
                             size_t *hdr_len, uint8_t *body,
                             size_t body_cap, size_t *body_len) {
    size_t h = 0;
    uint8_t byte;
    size_t content_length = 0;
    int have_cl = 0;

    while (h + 4 < hdr_cap) {
        int n = recv(fd, (char *)&byte, 1, 0);
        if (n <= 0) {
            return -1;
        }
        hdr[h++] = (char)byte;
        if (h >= 4 && memcmp(hdr + h - 4, "\r\n\r\n", 4) == 0) {
            break;
        }
    }
    if (h >= hdr_cap) {
        return -1;
    }
    hdr[h] = '\0';
    {
        char *cl = strstr(hdr, "Content-Length:");
        if (!cl) {
            cl = strstr(hdr, "content-length:");
        }
        if (cl) {
            cl += 15;
            while (*cl == ' ' || *cl == '\t') {
                cl++;
            }
            content_length = (size_t)strtoul(cl, NULL, 10);
            have_cl = 1;
        }
    }
    *hdr_len = h;
    *body_len = 0;
    if (have_cl && content_length > 0) {
        if (content_length > body_cap) {
            content_length = body_cap;
        }
        if (mock_read_exact(fd, body, content_length) != 0) {
            return -1;
        }
        *body_len = content_length;
    } else if (strstr(hdr, "chunked") != NULL) {
        /* Transfer-Encoding: chunked request body */
        size_t w = 0;
        for (;;) {
            char size_line[64];
            size_t sl = 0;
            size_t chunk_size;
            char *endptr;
            /* read a hex size line terminated by CRLF */
            while (sl + 2 < sizeof(size_line)) {
                if (recv(fd, &size_line[sl], 1, 0) <= 0) {
                    return -1;
                }
                sl++;
                if (sl >= 2 && size_line[sl - 2] == '\r' && size_line[sl - 1] == '\n') {
                    break;
                }
            }
            size_line[sl] = '\0';
            chunk_size = (size_t)strtoull(size_line, &endptr, 16);
            if (chunk_size == 0) {
                break; /* final chunk; trailers may follow, ignore */
            }
            if (w + chunk_size > body_cap) {
                return -1;
            }
            if (mock_read_exact(fd, body + w, chunk_size) != 0) {
                return -1;
            }
            w += chunk_size;
            if (mock_read_exact(fd, body + w, 2) != 0) {
                return -1;
            }
            w += 2; /* CRLF after chunk data; included in captured body */
        }
        *body_len = w;
    }
    return 0;
}

static int mock_send_all(mock_sock_t fd, const char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        int n = send(fd, data + off, (int)(len - off), 0);
        if (n <= 0) {
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

/* Sleep in small slices so mock_server_stop() can abort a long handler
   delay (used to keep a request in flight) without waiting for the full
   sleep to elapse. */
static void mock_interruptible_sleep(mock_server_t *s, size_t ms) {
    size_t waited = 0;
    while (waited < ms && s->running) {
        size_t slice = ms - waited;
        if (slice > 10u) {
            slice = 10u;
        }
        mock_sleep_ms(slice);
        waited += slice;
    }
}

static void mock_send_response(mock_server_t *s, mock_sock_t fd, int status,
                               const char *reason, const char *content_type,
                               const uint8_t *body, size_t body_len,
                               size_t chunk_delay_ms) {
    char head[512];
    size_t off = 0;
    int n;

    n = snprintf(head, sizeof(head),
                 "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
                 "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                 status, reason, content_type, body_len);
    if (n > 0) {
        off = (size_t)n;
    }
    mock_send_all(fd, head, off);
    if (body_len > 0) {
        if (chunk_delay_ms == 0) {
            mock_send_all(fd, (const char *)body, body_len);
        } else {
            size_t sent = 0;
            while (sent < body_len && chunk_delay_ms > 0 && s->running) {
                size_t chunk = body_len - sent;
                if (chunk > 1024u) {
                    chunk = 1024u;
                }
                mock_send_all(fd, (const char *)body + sent, chunk);
                sent += chunk;
                mock_interruptible_sleep(s, chunk_delay_ms);
            }
        }
    }
}

static void mock_handle_connection(mock_server_t *s, mock_sock_t fd) {
    char hdr[8192];
    size_t hdr_len = 0;
    uint8_t body[1 << 16];
    size_t body_len = 0;
    char method[16];
    char path[512];
    char *sp;
    char *eol;

    if (mock_read_request(fd, hdr, sizeof(hdr), &hdr_len, body,
                          sizeof(body), &body_len) != 0) {
            mock_close_sock(fd);
        return;
    }
    (void)hdr_len;
    sp = strchr(hdr, ' ');
    if (!sp) {
        mock_close_sock(fd);
        return;
    }
    eol = strchr(sp + 1, ' ');
    if (!eol) {
        mock_close_sock(fd);
        return;
    }
    {
        size_t mlen = (size_t)(sp - hdr);
        if (mlen >= sizeof(method)) {
            mlen = sizeof(method) - 1;
        }
        memcpy(method, hdr, mlen);
        method[mlen] = '\0';
    }
    {
        size_t plen = (size_t)(eol - (sp + 1));
        if (plen >= sizeof(path)) {
            plen = sizeof(path) - 1;
        }
        memcpy(path, sp + 1, plen);
        path[plen] = '\0';
    }

    s->request_count++;
    snprintf(s->last_method, sizeof(s->last_method), "%s", method);
    snprintf(s->last_path, sizeof(s->last_path), "%s", path);
    {
        char *auth = strstr(hdr, "Authorization:");
        if (auth) {
            char *nl = strchr(auth, '\r');
            size_t alen = nl ? (size_t)(nl - auth) : strlen(auth);
            if (alen >= sizeof(s->last_auth)) {
                alen = sizeof(s->last_auth) - 1;
            }
            memcpy(s->last_auth, auth, alen);
            s->last_auth[alen] = '\0';
        } else {
            s->last_auth[0] = '\0';
        }
    }
    {
        char *ct = strstr(hdr, "Content-Type:");
        if (ct) {
            char *nl = strchr(ct, '\r');
            size_t clen = nl ? (size_t)(nl - ct) : strlen(ct);
            if (clen >= sizeof(s->last_content_type)) {
                clen = sizeof(s->last_content_type) - 1;
            }
            memcpy(s->last_content_type, ct, clen);
            s->last_content_type[clen] = '\0';
        } else {
            s->last_content_type[0] = '\0';
        }
    }
    memcpy(s->last_body, body, body_len);
    s->last_body_len = body_len;

    if (strcmp(path, MOCK_TTS_PATH) == 0) {
        s->tts_requests++;
        mock_send_response(s, fd, s->tts_status, "OK",
                           "application/octet-stream", s->tts_pcm,
                           s->tts_pcm_len, s->tts_frame_delay_ms);
    } else if (strcmp(path, MOCK_ASR_PATH) == 0) {
        const char *asr_json = s->asr_json;
        s->asr_requests++;
        if (s->asr_delay_ms > 0) {
            mock_interruptible_sleep(s, s->asr_delay_ms);
        }
        if (s->asr_match_json &&
            mock_bytes_contains(body, body_len, s->asr_match,
                                s->asr_match_len)) {
            asr_json = s->asr_match_json;
        }
        mock_send_response(s, fd, s->asr_status, "OK", "application/json",
                           (const uint8_t *)asr_json,
                           asr_json ? strlen(asr_json) : 0, 0);
    } else {
        mock_send_response(s, fd, 404, "Not Found", "text/plain",
                           (const uint8_t *)"not found", 9, 0);
    }
    mock_close_sock(fd);
}

static void *mock_server_thread(void *opaque) {
    mock_server_t *s = (mock_server_t *)opaque;
    while (s->running) {
        fd_set rfds;
        struct timeval tv;
        int sel;

        /* Poll the listen socket with a bounded timeout so stop() can join
           the thread even when closesocket() does not wake a blocked
           accept() on Windows. */
        FD_ZERO(&rfds);
        FD_SET((mock_sock_t)s->listen_sock, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        sel = select(0, &rfds, NULL, NULL, &tv);
        if (sel > 0) {
            mock_sock_t fd = accept(s->listen_sock, NULL, NULL);
            if (fd != MOCK_INVALID_SOCKET) {
                mock_handle_connection(s, fd);
            }
        } else if (sel < 0) {
            if (!s->running) {
                break;
            }
            mock_sleep_ms(5);
        }
    }
    return NULL;
}

/* Bind to an ephemeral port and start the accept loop. */
static int mock_server_start(mock_server_t *s) {
    struct sockaddr_in addr;
    socklen_t addr_len = (socklen_t)sizeof(addr);

    if (mock_net_init() != 0) {
        return -1;
    }
    s->listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s->listen_sock == MOCK_INVALID_SOCKET) {
        return -1;
    }
#ifdef _WIN32
    {
        BOOL opt = TRUE;
        setsockopt(s->listen_sock, SOL_SOCKET, SO_REUSEADDR,
                   (const char *)&opt, sizeof(opt));
    }
#else
    {
        int opt = 1;
        setsockopt(s->listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    }
#endif
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = 0; /* ephemeral */
    if (bind(s->listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        mock_close_sock(s->listen_sock);
        s->listen_sock = MOCK_INVALID_SOCKET;
        return -1;
    }
    if (getsockname(s->listen_sock, (struct sockaddr *)&addr, &addr_len) != 0) {
        mock_close_sock(s->listen_sock);
        s->listen_sock = MOCK_INVALID_SOCKET;
        return -1;
    }
    s->port = (int)ntohs(addr.sin_port);
    if (listen(s->listen_sock, 8) != 0) {
        mock_close_sock(s->listen_sock);
        s->listen_sock = MOCK_INVALID_SOCKET;
        return -1;
    }
    s->running = 1;
    if (ivr_thread_create(&s->thread, mock_server_thread, s) != 0) {
        s->running = 0;
        mock_close_sock(s->listen_sock);
        s->listen_sock = MOCK_INVALID_SOCKET;
        return -1;
    }
    return 0;
}

static void mock_server_stop(mock_server_t *s) {
    s->running = 0;
    mock_close_sock(s->listen_sock);
    s->listen_sock = MOCK_INVALID_SOCKET;
    ivr_thread_join(&s->thread);
}

static void mock_server_make_base_url(const mock_server_t *s, char *out,
                                      size_t out_cap) {
    snprintf(out, out_cap, "http://127.0.0.1:%d", s->port);
}

/* Binary-safe substring search over the captured request body (the WAV
   payload contains NUL bytes, so strstr() is not usable). */
static int mock_body_contains(const mock_server_t *s, const char *needle) {
    size_t nlen;
    size_t i;

    if (!s || !needle) {
        return 0;
    }
    nlen = strlen(needle);
    if (nlen == 0 || nlen > s->last_body_len) {
        return 0;
    }
    for (i = 0; i + nlen <= s->last_body_len; i++) {
        if (memcmp(s->last_body + i, needle, nlen) == 0) {
            return 1;
        }
    }
    return 0;
}
/* ------------------------------------------------------------------ */
/* observers                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    volatile int audio_count;
    volatile int complete_count;
    volatile int error_count;
    volatile long long total_bytes;
    volatile int first_sample;
    volatile int sink_result;
    volatile int last_sample_rate;
    volatile int audio_delay_ms; /* sleep inside on_audio to hold callbacks in flight */
} tts_observer_t;

typedef struct {
    volatile int result_count;
    volatile int complete_count;
    volatile int error_count;
    char text[256];
    volatile int is_final;
} asr_observer_t;

static int observe_tts_audio(turbo_tts_t *tts, const turbo_speech_audio_frame_t *frame,
                             void *user_data) {
    tts_observer_t *o = (tts_observer_t *)user_data;
    (void)tts;
    o->audio_count++;
    o->total_bytes += (long long)frame->len;
    if (o->audio_count == 1 && frame->len >= 2) {
        o->first_sample = (int)((int16_t)(frame->data[0] | (frame->data[1] << 8)));
    }
    o->last_sample_rate = frame->format.sample_rate;
    if (o->audio_delay_ms > 0) {
        mock_sleep_ms(o->audio_delay_ms);
    }
    return o->sink_result;
}

static void observe_tts_complete(turbo_tts_t *tts, void *user_data) {
    tts_observer_t *o = (tts_observer_t *)user_data;
    (void)tts;
    o->complete_count++;
}

static void observe_tts_error(turbo_tts_t *tts, int error_code, const char *message,
                              void *user_data) {
    tts_observer_t *o = (tts_observer_t *)user_data;
    (void)tts;
    (void)error_code;
    o->error_count++;
}

static void observe_asr_result(turbo_asr_t *asr, const turbo_asr_result_t *result,
                               void *user_data) {
    asr_observer_t *o = (asr_observer_t *)user_data;
    size_t n;
    (void)asr;
    o->result_count++;
    o->is_final = result->is_final;
    n = result->text_len < sizeof(o->text) - 1u ? result->text_len
                                                : sizeof(o->text) - 1u;
    memcpy(o->text, result->text, n);
    o->text[n] = '\0';
}

static void observe_asr_complete(turbo_asr_t *asr, void *user_data) {
    asr_observer_t *o = (asr_observer_t *)user_data;
    (void)asr;
    o->complete_count++;
}

static void observe_asr_error(turbo_asr_t *asr, int error_code, const char *message,
                              void *user_data) {
    asr_observer_t *o = (asr_observer_t *)user_data;
    (void)asr;
    (void)error_code;
    o->error_count++;
}

/* ------------------------------------------------------------------ */
/* config helper                                                       */
/* ------------------------------------------------------------------ */

static int mock_wait_until(int (*cond)(void *), void *arg, int timeout_ms) {
    int waited = 0;
    while (waited < timeout_ms) {
        if (cond(arg)) {
            return 0;
        }
        mock_sleep_ms(5);
        waited += 5;
    }
    return -1;
}

static int tts_done_cond(void *arg) {
    tts_observer_t *o = (tts_observer_t *)arg;
    return o->complete_count > 0 || o->error_count > 0;
}

static int asr_done_cond(void *arg) {
    asr_observer_t *o = (asr_observer_t *)arg;
    return o->complete_count > 0 || o->error_count > 0;
}

static int count_cond(void *arg) {
    return *(const volatile int *)arg > 0;
}

typedef struct {
    atomic_uint_least64_t tts_count;
    atomic_uint_least64_t asr_count;
    atomic_uint_least64_t last_duration_ms;
} request_observer_t;

static void observe_request_complete(void *context,
                                     ivr_openai_request_kind_t kind,
                                     uint64_t duration_ms) {
    request_observer_t *observer = (request_observer_t *)context;
    atomic_store_explicit(&observer->last_duration_ms, duration_ms,
                          memory_order_relaxed);
    if (kind == IVR_OPENAI_REQUEST_TTS) {
        atomic_fetch_add_explicit(&observer->tts_count, 1,
                                  memory_order_release);
    } else if (kind == IVR_OPENAI_REQUEST_ASR) {
        atomic_fetch_add_explicit(&observer->asr_count, 1,
                                  memory_order_release);
    }
}

static int request_observed_cond(void *arg) {
    request_observer_t *observer = (request_observer_t *)arg;
    return atomic_load_explicit(&observer->tts_count, memory_order_acquire) > 0 ||
           atomic_load_explicit(&observer->asr_count, memory_order_acquire) > 0;
}

/* Build a deterministic 24 kHz PCM ramp for the TTS mock body. */
static void mock_fill_pcm(uint8_t *pcm, size_t len) {
    size_t i;
    for (i = 0; i < len / 2u; i++) {
        int16_t v = (int16_t)((int)(i % 1000) - 500);
        pcm[2u * i] = (uint8_t)(v & 0xFFu);
        pcm[2u * i + 1u] = (uint8_t)((v >> 8) & 0xFFu);
    }
}

static void check_tts_error_response(int status, const uint8_t *payload,
                                     size_t payload_len) {
    mock_server_t *server = (mock_server_t *)calloc(1, sizeof(*server));
    char base_url[128];
    ivr_openai_config_t config;
    ivr_openai_tts_t *tts_wrap = NULL;
    turbo_tts_provider_t provider;
    tts_observer_t observer;
    turbo_tts_t *tts = NULL;
    turbo_tts_request_t request;
    turbo_tts_callbacks_t callbacks;

    check_not_null(server);
    mock_server_init(server);
    server->tts_status = status;
    server->tts_pcm = payload;
    server->tts_pcm_len = payload_len;
    check_equal(mock_server_start(server), 0);
    mock_server_make_base_url(server, base_url, sizeof(base_url));
    memset(&config, 0, sizeof(config));
    config.base_url = base_url;
    config.timeout_ms = 5000;
    memset(&observer, 0, sizeof(observer));
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.on_audio = observe_tts_audio;
    callbacks.on_complete = observe_tts_complete;
    callbacks.on_error = observe_tts_error;
    check_equal(ivr_openai_tts_create(&config, &tts_wrap), 0);
    ivr_openai_tts_get_provider(tts_wrap, &provider);
    tts = turbo_tts_create(&provider, &callbacks, &observer);
    check_not_null(tts);
    memset(&request, 0, sizeof(request));
    request.text = "invalid response";
    request.text_len = strlen(request.text);
    request.rate = 1.0f;
    request.pitch = 1.0f;
    check_equal(turbo_tts_synthesize(tts, &request), TURBO_SPEECH_OK);
    check_equal(mock_wait_until(tts_done_cond, &observer, 5000), 0);
    check_equal(observer.error_count, 1);
    check_equal(observer.complete_count, 0);
    check_equal(observer.audio_count, 0);
    check_equal(turbo_tts_get_state(tts), TURBO_SPEECH_STATE_ERROR);
    turbo_tts_destroy(tts);
    ivr_openai_tts_free(tts_wrap);
    mock_server_stop(server);
    free(server);
}

static void check_asr_error_response(int status, const char *json,
                                     size_t response_delay_ms,
                                     int timeout_ms) {
    mock_server_t *server = (mock_server_t *)calloc(1, sizeof(*server));
    uint8_t pcm[3200u] = {0};
    char base_url[128];
    ivr_openai_config_t config;
    ivr_openai_asr_t *asr_wrap = NULL;
    turbo_asr_provider_t provider;
    asr_observer_t observer;
    turbo_asr_t *asr = NULL;
    turbo_asr_config_t asr_config;
    turbo_asr_callbacks_t callbacks;

    check_not_null(server);
    mock_server_init(server);
    server->asr_status = status;
    server->asr_json = json;
    server->asr_delay_ms = response_delay_ms;
    check_equal(mock_server_start(server), 0);
    mock_server_make_base_url(server, base_url, sizeof(base_url));
    memset(&config, 0, sizeof(config));
    config.base_url = base_url;
    config.timeout_ms = timeout_ms;
    config.connect_timeout_ms = timeout_ms;
    memset(&observer, 0, sizeof(observer));
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.on_result = observe_asr_result;
    callbacks.on_complete = observe_asr_complete;
    callbacks.on_error = observe_asr_error;
    check_equal(ivr_openai_asr_create(&config, &asr_wrap), 0);
    ivr_openai_asr_get_provider(asr_wrap, &provider);
    asr = turbo_asr_create(&provider, &callbacks, &observer);
    check_not_null(asr);
    memset(&asr_config, 0, sizeof(asr_config));
    asr_config.format.sample_rate = 16000;
    asr_config.format.channels = 1;
    asr_config.format.bits_per_sample = 16;
    check_equal(turbo_asr_start(asr, &asr_config), TURBO_SPEECH_OK);
    check_equal(turbo_asr_write_pcm(asr, pcm, sizeof(pcm), 0),
                 TURBO_SPEECH_OK);
    check_equal(turbo_asr_finish(asr), TURBO_SPEECH_OK);
    check_equal(mock_wait_until(asr_done_cond, &observer, 5000), 0);
    check_equal(observer.error_count, 1);
    check_equal(observer.result_count, 0);
    check_equal(observer.complete_count, 0);
    check_equal(turbo_asr_get_state(asr), TURBO_SPEECH_STATE_ERROR);
    turbo_asr_destroy(asr);
    ivr_openai_asr_free(asr_wrap);
    mock_server_stop(server);
    free(server);
}
suite("TurboMedia IVR OpenAI provider") {
  group("TTS") {
    it("synthesizes text over HTTP and delivers resampled PCM") {
      mock_server_t server;
      uint8_t pcm[MOCK_PCM_BYTES];
      char base_url[128];
      ivr_openai_config_t config;
      ivr_openai_tts_t *tts_wrap = NULL;
      turbo_tts_provider_t provider;
      tts_observer_t observer;
      turbo_tts_t *tts = NULL;
      turbo_tts_request_t request;
      turbo_tts_callbacks_t callbacks;
      request_observer_t request_observer;

      mock_server_init(&server);
      mock_fill_pcm(pcm, sizeof(pcm));
      server.tts_pcm = pcm;
      server.tts_pcm_len = sizeof(pcm);
      check_equal(mock_server_start(&server), 0);
      mock_server_make_base_url(&server, base_url, sizeof(base_url));

      memset(&config, 0, sizeof(config));
      config.base_url = base_url;
      config.api_key = MOCK_API_KEY;
      config.sample_rate = 16000;
      config.tts_frame_bytes = 1600u;
      config.timeout_ms = 5000;
      atomic_init(&request_observer.tts_count, 0);
      atomic_init(&request_observer.asr_count, 0);
      atomic_init(&request_observer.last_duration_ms, 0);
      config.observer.context = &request_observer;
      config.observer.on_request_complete = observe_request_complete;

      memset(&observer, 0, sizeof(observer));
      memset(&callbacks, 0, sizeof(callbacks));
      callbacks.on_audio = observe_tts_audio;
      callbacks.on_complete = observe_tts_complete;
      callbacks.on_error = observe_tts_error;

      check_equal(ivr_openai_tts_create(&config, &tts_wrap), 0);
      check_not_null(tts_wrap);
      ivr_openai_tts_get_provider(tts_wrap, &provider);
      tts = turbo_tts_create(&provider, &callbacks, &observer);
      check_not_null(tts);

      memset(&request, 0, sizeof(request));
      request.text = "hello ivr";
      request.text_len = 9;
      request.rate = 1.0f;
      request.pitch = 1.0f;
      check_equal(turbo_tts_synthesize(tts, &request), TURBO_SPEECH_OK);
      check_equal(mock_wait_until(tts_done_cond, &observer, 5000), 0);
      check_equal(mock_wait_until(request_observed_cond, &request_observer,
                                   5000), 0);

      check_equal(observer.error_count, 0);
      check_equal(observer.complete_count, 1);
      check_equal(observer.audio_count, 20); /* 32000 / 1600 */
      check_equal((long)observer.total_bytes, 32000L);
      check_equal(observer.last_sample_rate, 16000);
      check_equal((int)observer.first_sample, -500);
      check_equal(turbo_tts_get_state(tts), TURBO_SPEECH_STATE_STOPPED);
      check_equal((unsigned)atomic_load(&request_observer.tts_count), 1u);
      check_equal((unsigned)atomic_load(&request_observer.asr_count), 0u);

      check_equal(server.tts_requests, 1);
      check_equal(server.asr_requests, 0);
      check_equal(server.last_path, MOCK_TTS_PATH);
      check_equal(server.last_method, "POST");
      check_contains(server.last_auth, "Bearer test-openai-key");
      check_contains((const char *)server.last_body, "\"model\":\"tts-1\"");
      check_contains((const char *)server.last_body, "\"voice\":\"alloy\"");
      check_contains((const char *)server.last_body, "\"response_format\":\"pcm\"");
      check_contains((const char *)server.last_body, "hello ivr");

      turbo_tts_destroy(tts);
      ivr_openai_tts_free(tts_wrap);
      mock_server_stop(&server);
    }

    it("reports non-2xx responses through on_error") {
      mock_server_t server;
      uint8_t pcm[MOCK_PCM_BYTES];
      char base_url[128];
      ivr_openai_config_t config;
      ivr_openai_tts_t *tts_wrap = NULL;
      turbo_tts_provider_t provider;
      tts_observer_t observer;
      turbo_tts_t *tts = NULL;
      turbo_tts_request_t request;
      turbo_tts_callbacks_t callbacks;

      mock_server_init(&server);
      mock_fill_pcm(pcm, sizeof(pcm));
      server.tts_pcm = pcm;
      server.tts_pcm_len = sizeof(pcm);
      server.tts_status = 500;
      check_equal(mock_server_start(&server), 0);
      mock_server_make_base_url(&server, base_url, sizeof(base_url));

      memset(&config, 0, sizeof(config));
      config.base_url = base_url;
      config.api_key = MOCK_API_KEY;
      config.sample_rate = 16000;

      memset(&observer, 0, sizeof(observer));
      memset(&callbacks, 0, sizeof(callbacks));
      callbacks.on_audio = observe_tts_audio;
      callbacks.on_complete = observe_tts_complete;
      callbacks.on_error = observe_tts_error;

      check_equal(ivr_openai_tts_create(&config, &tts_wrap), 0);
      ivr_openai_tts_get_provider(tts_wrap, &provider);
      tts = turbo_tts_create(&provider, &callbacks, &observer);
      check_not_null(tts);

      memset(&request, 0, sizeof(request));
      request.text = "boom";
      request.text_len = 4;
      request.rate = 1.0f;
      request.pitch = 1.0f;
      check_equal(turbo_tts_synthesize(tts, &request), TURBO_SPEECH_OK);
      check_equal(mock_wait_until(tts_done_cond, &observer, 5000), 0);

      check_equal(observer.error_count, 1);
      check_equal(observer.complete_count, 0);
      check_equal(observer.audio_count, 0);
      check_equal(turbo_tts_get_state(tts), TURBO_SPEECH_STATE_ERROR);

      turbo_tts_destroy(tts);
      ivr_openai_tts_free(tts_wrap);
      mock_server_stop(&server);
    }
    it("cancel quiesces in-flight callbacks without on_complete") {
      mock_server_t server;
      uint8_t pcm[96000u]; /* 2 seconds at 24 kHz */
      char base_url[128];
      ivr_openai_config_t config;
      ivr_openai_tts_t *tts_wrap = NULL;
      turbo_tts_provider_t provider;
      tts_observer_t observer;
      turbo_tts_t *tts = NULL;
      turbo_tts_request_t request;
      turbo_tts_callbacks_t callbacks;
      int frames_before_cancel;

      mock_server_init(&server);
      mock_fill_pcm(pcm, sizeof(pcm));
      server.tts_pcm = pcm;
      server.tts_pcm_len = sizeof(pcm);
      server.tts_frame_delay_ms = 5u;
      check_equal(mock_server_start(&server), 0);
      mock_server_make_base_url(&server, base_url, sizeof(base_url));

      memset(&config, 0, sizeof(config));
      config.base_url = base_url;
      config.api_key = MOCK_API_KEY;
      config.sample_rate = 16000;
      config.tts_frame_bytes = 320u;
      config.timeout_ms = 2000;

      memset(&observer, 0, sizeof(observer));
      observer.audio_delay_ms = 5; /* keep callbacks in flight */
      memset(&callbacks, 0, sizeof(callbacks));
      callbacks.on_audio = observe_tts_audio;
      callbacks.on_complete = observe_tts_complete;
      callbacks.on_error = observe_tts_error;

      check_equal(ivr_openai_tts_create(&config, &tts_wrap), 0);
      ivr_openai_tts_get_provider(tts_wrap, &provider);
      tts = turbo_tts_create(&provider, &callbacks, &observer);
      check_not_null(tts);

      memset(&request, 0, sizeof(request));
      request.text = "long utterance for cancellation";
      request.text_len = 32;
      request.rate = 1.0f;
      request.pitch = 1.0f;
      check_equal(turbo_tts_synthesize(tts, &request), TURBO_SPEECH_OK);
      /* give the worker a chance to start delivering frames */
      mock_sleep_ms(60);
      frames_before_cancel = observer.audio_count;
      check_equal(turbo_tts_cancel(tts), TURBO_SPEECH_OK);
      (void)frames_before_cancel;
      mock_sleep_ms(100); /* no callback may arrive after cancel() returns */
      check_equal(observer.complete_count, 0);
      check_equal(observer.error_count, 0);
      check_equal(turbo_tts_get_state(tts), TURBO_SPEECH_STATE_STOPPED);

      turbo_tts_destroy(tts);
      ivr_openai_tts_free(tts_wrap);
      mock_server_stop(&server);
    }

    it("rejects an odd-byte PCM response as malformed audio") {
      static const uint8_t malformed_pcm[] = {0x00, 0x01, 0x02};
      check_tts_error_response(200, malformed_pcm, sizeof(malformed_pcm));
    }

    it("reports an audio sink rejection instead of completing playback") {
      mock_server_t server;
      uint8_t pcm[MOCK_PCM_BYTES];
      char base_url[128];
      ivr_openai_config_t config;
      ivr_openai_tts_t *tts_wrap = NULL;
      turbo_tts_provider_t provider;
      tts_observer_t observer;
      turbo_tts_t *tts = NULL;
      turbo_tts_request_t request;
      turbo_tts_callbacks_t callbacks;

      mock_server_init(&server);
      mock_fill_pcm(pcm, sizeof(pcm));
      server.tts_pcm = pcm;
      server.tts_pcm_len = sizeof(pcm);
      check_equal(mock_server_start(&server), 0);
      mock_server_make_base_url(&server, base_url, sizeof(base_url));
      memset(&config, 0, sizeof(config));
      config.base_url = base_url;
      config.sample_rate = 16000;
      config.tts_frame_bytes = 640u;
      config.timeout_ms = 5000;
      memset(&observer, 0, sizeof(observer));
      observer.sink_result = TURBO_SPEECH_ERR_PROVIDER;
      memset(&callbacks, 0, sizeof(callbacks));
      callbacks.on_audio = observe_tts_audio;
      callbacks.on_complete = observe_tts_complete;
      callbacks.on_error = observe_tts_error;
      check_equal(ivr_openai_tts_create(&config, &tts_wrap), 0);
      ivr_openai_tts_get_provider(tts_wrap, &provider);
      tts = turbo_tts_create(&provider, &callbacks, &observer);
      check_not_null(tts);
      memset(&request, 0, sizeof(request));
      request.text = "sink failure";
      request.text_len = strlen(request.text);
      request.rate = 1.0f;
      request.pitch = 1.0f;

      check_equal(turbo_tts_synthesize(tts, &request), TURBO_SPEECH_OK);
      check_equal(mock_wait_until(tts_done_cond, &observer, 5000), 0);
      check_equal(observer.audio_count, 1);
      check_equal(observer.error_count, 1);
      check_equal(observer.complete_count, 0);
      check_equal(turbo_tts_get_state(tts), TURBO_SPEECH_STATE_ERROR);

      turbo_tts_destroy(tts);
      ivr_openai_tts_free(tts_wrap);
      mock_server_stop(&server);
    }

    it("rejects input beyond the configured text cap without retention") {
      ivr_openai_config_t config;
      ivr_openai_tts_t *tts_wrap = NULL;
      turbo_tts_provider_t provider;
      tts_observer_t observer;
      turbo_tts_t *tts;
      turbo_tts_request_t request;
      turbo_tts_callbacks_t callbacks;
      ivr_openai_resource_snapshot_t snapshot;

      memset(&config, 0, sizeof(config));
      config.base_url = "http://127.0.0.1:1";
      config.max_tts_input_bytes = 3;
      memset(&observer, 0, sizeof(observer));
      memset(&callbacks, 0, sizeof(callbacks));
      callbacks.on_audio = observe_tts_audio;
      callbacks.on_complete = observe_tts_complete;
      callbacks.on_error = observe_tts_error;
      check_equal(ivr_openai_tts_create(&config, &tts_wrap), 0);
      ivr_openai_tts_get_provider(tts_wrap, &provider);
      tts = turbo_tts_create(&provider, &callbacks, &observer);
      check_not_null(tts);
      memset(&request, 0, sizeof(request));
      request.text = "four";
      request.text_len = 4;
      request.rate = 1.0f;
      request.pitch = 1.0f;

      check_equal(turbo_tts_synthesize(tts, &request),
                   TURBO_SPEECH_ERR_BUSY);
      turbo_tts_destroy(tts);
      ivr_openai_tts_free(tts_wrap);
      ivr_openai_get_resource_snapshot(&snapshot);
      check(snapshot.retained_input_bytes == 0);
      check(snapshot.active_tts_instances == 0);
      check(snapshot.live_provider_threads == 0);
    }

    it("reports an HTTP response that exceeds the configured cap") {
      mock_server_t *server = (mock_server_t *)calloc(1, sizeof(*server));
      uint8_t *pcm = (uint8_t *)malloc(MOCK_PCM_BYTES);
      char base_url[128];
      ivr_openai_config_t config;
      ivr_openai_tts_t *tts_wrap = NULL;
      turbo_tts_provider_t provider;
      tts_observer_t observer;
      turbo_tts_t *tts;
      turbo_tts_request_t request;
      turbo_tts_callbacks_t callbacks;
      ivr_openai_resource_snapshot_t snapshot;

      check_not_null(server);
      check_not_null(pcm);
      mock_server_init(server);
      mock_fill_pcm(pcm, MOCK_PCM_BYTES);
      server->tts_pcm = pcm;
      server->tts_pcm_len = MOCK_PCM_BYTES;
      check_equal(mock_server_start(server), 0);
      mock_server_make_base_url(server, base_url, sizeof(base_url));
      memset(&config, 0, sizeof(config));
      config.base_url = base_url;
      config.max_response_bytes = 1024;
      memset(&observer, 0, sizeof(observer));
      memset(&callbacks, 0, sizeof(callbacks));
      callbacks.on_audio = observe_tts_audio;
      callbacks.on_complete = observe_tts_complete;
      callbacks.on_error = observe_tts_error;
      check_equal(ivr_openai_tts_create(&config, &tts_wrap), 0);
      ivr_openai_tts_get_provider(tts_wrap, &provider);
      tts = turbo_tts_create(&provider, &callbacks, &observer);
      check_not_null(tts);
      memset(&request, 0, sizeof(request));
      request.text = "bounded";
      request.text_len = 7;
      request.rate = 1.0f;
      request.pitch = 1.0f;

      check_equal(turbo_tts_synthesize(tts, &request), TURBO_SPEECH_OK);
      check_equal(mock_wait_until(tts_done_cond, &observer, 5000), 0);
      check_equal(observer.error_count, 1);
      check_equal(observer.complete_count, 0);
      turbo_tts_destroy(tts);
      ivr_openai_tts_free(tts_wrap);
      mock_server_stop(server);
      free(pcm);
      free(server);
      ivr_openai_get_resource_snapshot(&snapshot);
      check(snapshot.retained_response_bytes == 0);
    }

    it("rejects a second synthesize while one is in flight") {
      mock_server_t server;
      uint8_t pcm[24000u]; /* 0.5 s at 24 kHz */
      char base_url[128];
      ivr_openai_config_t config;
      ivr_openai_tts_t *tts_wrap = NULL;
      turbo_tts_provider_t provider;
      tts_observer_t observer;
      turbo_tts_t *tts = NULL;
      turbo_tts_request_t request;
      turbo_tts_callbacks_t callbacks;

      mock_server_init(&server);
      mock_fill_pcm(pcm, sizeof(pcm));
      server.tts_pcm = pcm;
      server.tts_pcm_len = sizeof(pcm);
      server.tts_frame_delay_ms = 20u; /* keep the first request in flight */
      check_equal(mock_server_start(&server), 0);
      mock_server_make_base_url(&server, base_url, sizeof(base_url));

      memset(&config, 0, sizeof(config));
      config.base_url = base_url;
      config.api_key = MOCK_API_KEY;
      config.sample_rate = 16000;
      config.timeout_ms = 2000;

      memset(&observer, 0, sizeof(observer));
      memset(&callbacks, 0, sizeof(callbacks));
      callbacks.on_audio = observe_tts_audio;
      callbacks.on_complete = observe_tts_complete;
      callbacks.on_error = observe_tts_error;

      check_equal(ivr_openai_tts_create(&config, &tts_wrap), 0);
      ivr_openai_tts_get_provider(tts_wrap, &provider);
      tts = turbo_tts_create(&provider, &callbacks, &observer);
      check_not_null(tts);

      memset(&request, 0, sizeof(request));
      request.text = "first";
      request.text_len = 5;
      request.rate = 1.0f;
      request.pitch = 1.0f;
      check_equal(turbo_tts_synthesize(tts, &request), TURBO_SPEECH_OK);
      mock_sleep_ms(50);
      memset(&request, 0, sizeof(request));
      request.text = "second";
      request.text_len = 6;
      request.rate = 1.0f;
      request.pitch = 1.0f;
      /* the session layer rejects a second synthesize while the first is
         RUNNING (before the provider is consulted) */
      check_equal(turbo_tts_synthesize(tts, &request), TURBO_SPEECH_ERR_STATE);
      check_equal(turbo_tts_cancel(tts), TURBO_SPEECH_OK);
      check_equal(turbo_tts_get_state(tts), TURBO_SPEECH_STATE_STOPPED);

      turbo_tts_destroy(tts);
      ivr_openai_tts_free(tts_wrap);
      mock_server_stop(&server);
    }
  }
  group("ASR") {
    it("transcribes captured PCM via multipart upload") {
      mock_server_t server;
      uint8_t pcm[3200u]; /* 100 ms at 16 kHz */
      char base_url[128];
      ivr_openai_config_t config;
      ivr_openai_asr_t *asr_wrap = NULL;
      turbo_asr_provider_t provider;
      asr_observer_t observer;
      turbo_asr_t *asr = NULL;
      turbo_asr_config_t asr_config;
      turbo_asr_callbacks_t callbacks;
      request_observer_t request_observer;

      mock_server_init(&server);
      check_equal(mock_server_start(&server), 0);
      mock_server_make_base_url(&server, base_url, sizeof(base_url));

      memset(&config, 0, sizeof(config));
      config.base_url = base_url;
      config.api_key = MOCK_API_KEY;
      config.asr_model = "whisper-1";
      config.asr_language = "en-US";
      config.timeout_ms = 5000;
      atomic_init(&request_observer.tts_count, 0);
      atomic_init(&request_observer.asr_count, 0);
      atomic_init(&request_observer.last_duration_ms, 0);
      config.observer.context = &request_observer;
      config.observer.on_request_complete = observe_request_complete;

      memset(&observer, 0, sizeof(observer));
      memset(&callbacks, 0, sizeof(callbacks));
      callbacks.on_result = observe_asr_result;
      callbacks.on_complete = observe_asr_complete;
      callbacks.on_error = observe_asr_error;

      check_equal(ivr_openai_asr_create(&config, &asr_wrap), 0);
      check_not_null(asr_wrap);
      ivr_openai_asr_get_provider(asr_wrap, &provider);
      asr = turbo_asr_create(&provider, &callbacks, &observer);
      check_not_null(asr);

      memset(&asr_config, 0, sizeof(asr_config));
      asr_config.format.sample_rate = 16000;
      asr_config.format.channels = 1;
      asr_config.format.bits_per_sample = 16;
      asr_config.language = "en-US";
      check_equal(turbo_asr_start(asr, &asr_config), TURBO_SPEECH_OK);
      check_equal(turbo_asr_write_pcm(asr, pcm, sizeof(pcm), 0),
                   TURBO_SPEECH_OK);
      check_equal(turbo_asr_finish(asr), TURBO_SPEECH_OK);
      check_equal(mock_wait_until(asr_done_cond, &observer, 5000), 0);
      check_equal(mock_wait_until(request_observed_cond, &request_observer,
                                   5000), 0);

      check_equal(observer.error_count, 0);
      check_equal(observer.result_count, 1);
      check_equal(observer.text, "hello remote world");
      check_equal(observer.is_final, 1);
      check_equal(observer.complete_count, 1);
      check_equal(turbo_asr_get_state(asr), TURBO_SPEECH_STATE_STOPPED);
      check_equal((unsigned)atomic_load(&request_observer.tts_count), 0u);
      check_equal((unsigned)atomic_load(&request_observer.asr_count), 1u);

      check_equal(server.asr_requests, 1);
      check_equal(server.tts_requests, 0);
      check_equal(server.last_path, MOCK_ASR_PATH);
      check_equal(server.last_method, "POST");
      check_contains(server.last_auth, "Bearer test-openai-key");
      check_contains(server.last_content_type, "multipart/form-data");
      /* multipart parts: file + model + language */
      check(mock_body_contains(&server, "name=\"file\""), "multipart file field");
      check(mock_body_contains(&server, "audio.wav"), "multipart wav filename");
      check(mock_body_contains(&server, "name=\"model\""), "multipart model field");
      check(mock_body_contains(&server, "whisper-1"), "multipart model value");
      check(mock_body_contains(&server, "name=\"language\""), "multipart language field");
      check(mock_body_contains(&server, "en-US"), "multipart language value");
      /* the WAV payload rides inside the multipart body */
      check(mock_body_contains(&server, "RIFF"), "wav riff magic");
      check(mock_body_contains(&server, "WAVE"), "wav wave magic");

      turbo_asr_destroy(asr);
      ivr_openai_asr_free(asr_wrap);
      mock_server_stop(&server);
    }

    it("completes locally when no audio was captured") {
      mock_server_t server;
      char base_url[128];
      ivr_openai_config_t config;
      ivr_openai_asr_t *asr_wrap = NULL;
      turbo_asr_provider_t provider;
      asr_observer_t observer;
      turbo_asr_t *asr = NULL;
      turbo_asr_config_t asr_config;
      turbo_asr_callbacks_t callbacks;

      mock_server_init(&server);
      check_equal(mock_server_start(&server), 0);
      mock_server_make_base_url(&server, base_url, sizeof(base_url));

      memset(&config, 0, sizeof(config));
      config.base_url = base_url;
      config.api_key = MOCK_API_KEY;

      memset(&observer, 0, sizeof(observer));
      memset(&callbacks, 0, sizeof(callbacks));
      callbacks.on_result = observe_asr_result;
      callbacks.on_complete = observe_asr_complete;
      callbacks.on_error = observe_asr_error;

      check_equal(ivr_openai_asr_create(&config, &asr_wrap), 0);
      ivr_openai_asr_get_provider(asr_wrap, &provider);
      asr = turbo_asr_create(&provider, &callbacks, &observer);
      check_not_null(asr);

      memset(&asr_config, 0, sizeof(asr_config));
      asr_config.format.sample_rate = 16000;
      asr_config.format.channels = 1;
      asr_config.format.bits_per_sample = 16;
      check_equal(turbo_asr_start(asr, &asr_config), TURBO_SPEECH_OK);
      check_equal(turbo_asr_finish(asr), TURBO_SPEECH_OK);
      check_equal(mock_wait_until(asr_done_cond, &observer, 2000), 0);

      check_equal(observer.complete_count, 1);
      check_equal(observer.result_count, 0);
      check_equal(observer.error_count, 0);
      check_equal(server.request_count, 0); /* no network round trip */
      check_equal(turbo_asr_get_state(asr), TURBO_SPEECH_STATE_STOPPED);

      turbo_asr_destroy(asr);
      ivr_openai_asr_free(asr_wrap);
      mock_server_stop(&server);
    }

    it("reports non-2xx responses through on_error") {
      mock_server_t server;
      uint8_t pcm[3200u];
      char base_url[128];
      ivr_openai_config_t config;
      ivr_openai_asr_t *asr_wrap = NULL;
      turbo_asr_provider_t provider;
      asr_observer_t observer;
      turbo_asr_t *asr = NULL;
      turbo_asr_config_t asr_config;
      turbo_asr_callbacks_t callbacks;

      mock_server_init(&server);
      server.asr_status = 401;
      server.asr_json = "{\"error\":{\"message\":\"invalid api key\"}}";
      check_equal(mock_server_start(&server), 0);
      mock_server_make_base_url(&server, base_url, sizeof(base_url));

      memset(&config, 0, sizeof(config));
      config.base_url = base_url;
      config.api_key = "bad-key";

      memset(&observer, 0, sizeof(observer));
      memset(&callbacks, 0, sizeof(callbacks));
      callbacks.on_result = observe_asr_result;
      callbacks.on_complete = observe_asr_complete;
      callbacks.on_error = observe_asr_error;

      check_equal(ivr_openai_asr_create(&config, &asr_wrap), 0);
      ivr_openai_asr_get_provider(asr_wrap, &provider);
      asr = turbo_asr_create(&provider, &callbacks, &observer);
      check_not_null(asr);

      memset(&asr_config, 0, sizeof(asr_config));
      asr_config.format.sample_rate = 16000;
      asr_config.format.channels = 1;
      asr_config.format.bits_per_sample = 16;
      check_equal(turbo_asr_start(asr, &asr_config), TURBO_SPEECH_OK);
      check_equal(turbo_asr_write_pcm(asr, pcm, sizeof(pcm), 0),
                   TURBO_SPEECH_OK);
      check_equal(turbo_asr_finish(asr), TURBO_SPEECH_OK);
      check_equal(mock_wait_until(asr_done_cond, &observer, 5000), 0);

      check_equal(observer.error_count, 1);
      check_equal(observer.result_count, 0);
      check_equal(observer.complete_count, 0);
      check_equal(turbo_asr_get_state(asr), TURBO_SPEECH_STATE_ERROR);

      turbo_asr_destroy(asr);
      ivr_openai_asr_free(asr_wrap);
      mock_server_stop(&server);
    }

    it("rejects rate limits, timeouts and malformed transcripts per call") {
      check_asr_error_response(
          429, "{\"error\":{\"message\":\"rate limited\"}}", 0, 5000);
      check_asr_error_response(200, "{\"text\":\"too late\"}", 60000u,
                               200);
      check_asr_error_response(200, "{\"unexpected\":true}", 0, 5000);
    }

    it("cancel aborts a queued transcription without results") {
      mock_server_t server;
      uint8_t pcm[3200u];
      char base_url[128];
      ivr_openai_config_t config;
      ivr_openai_asr_t *asr_wrap = NULL;
      turbo_asr_provider_t provider;
      asr_observer_t observer;
      turbo_asr_t *asr = NULL;
      turbo_asr_config_t asr_config;
      turbo_asr_callbacks_t callbacks;

      mock_server_init(&server);
      server.asr_delay_ms = 60000u; /* the server never answers in time: the
                                       client request timeout is what bounds
                                       the in-flight wait */
      check_equal(mock_server_start(&server), 0);
      mock_server_make_base_url(&server, base_url, sizeof(base_url));

      memset(&config, 0, sizeof(config));
      config.base_url = base_url;
      config.api_key = MOCK_API_KEY;
      config.timeout_ms = 800;
      config.connect_timeout_ms = 800;

      memset(&observer, 0, sizeof(observer));
      memset(&callbacks, 0, sizeof(callbacks));
      callbacks.on_result = observe_asr_result;
      callbacks.on_complete = observe_asr_complete;
      callbacks.on_error = observe_asr_error;

      check_equal(ivr_openai_asr_create(&config, &asr_wrap), 0);
      ivr_openai_asr_get_provider(asr_wrap, &provider);
      asr = turbo_asr_create(&provider, &callbacks, &observer);
      check_not_null(asr);

      memset(&asr_config, 0, sizeof(asr_config));
      asr_config.format.sample_rate = 16000;
      asr_config.format.channels = 1;
      asr_config.format.bits_per_sample = 16;
      check_equal(turbo_asr_start(asr, &asr_config), TURBO_SPEECH_OK);
      check_equal(turbo_asr_write_pcm(asr, pcm, sizeof(pcm), 0),
                   TURBO_SPEECH_OK);
      check_equal(turbo_asr_finish(asr), TURBO_SPEECH_OK);
      check_equal(turbo_asr_cancel(asr), TURBO_SPEECH_OK);
      mock_sleep_ms(100);
      check_equal(observer.result_count, 0);
      check_equal(observer.complete_count, 0);
      check_equal(observer.error_count, 0);
      check_equal(turbo_asr_get_state(asr), TURBO_SPEECH_STATE_STOPPED);

      turbo_asr_destroy(asr);
      ivr_openai_asr_free(asr_wrap);
      mock_server_stop(&server);
    }

    it("rejects writes beyond the configured buffer cap") {
      mock_server_t server;
      uint8_t pcm[2048u];
      char base_url[128];
      ivr_openai_config_t config;
      ivr_openai_asr_t *asr_wrap = NULL;
      turbo_asr_provider_t provider;
      asr_observer_t observer;
      turbo_asr_t *asr = NULL;
      turbo_asr_config_t asr_config;
      turbo_asr_callbacks_t callbacks;

      mock_server_init(&server);
      check_equal(mock_server_start(&server), 0);
      mock_server_make_base_url(&server, base_url, sizeof(base_url));

      memset(&config, 0, sizeof(config));
      config.base_url = base_url;
      config.max_asr_buffer_bytes = 4096u; /* holds two 2048-byte frames */

      memset(&observer, 0, sizeof(observer));
      memset(&callbacks, 0, sizeof(callbacks));
      callbacks.on_result = observe_asr_result;
      callbacks.on_complete = observe_asr_complete;
      callbacks.on_error = observe_asr_error;

      check_equal(ivr_openai_asr_create(&config, &asr_wrap), 0);
      ivr_openai_asr_get_provider(asr_wrap, &provider);
      asr = turbo_asr_create(&provider, &callbacks, &observer);
      check_not_null(asr);

      memset(&asr_config, 0, sizeof(asr_config));
      asr_config.format.sample_rate = 16000;
      asr_config.format.channels = 1;
      asr_config.format.bits_per_sample = 16;
      check_equal(turbo_asr_start(asr, &asr_config), TURBO_SPEECH_OK);
      check_equal(turbo_asr_write_pcm(asr, pcm, sizeof(pcm), 0),
                   TURBO_SPEECH_OK);
      check_equal(turbo_asr_write_pcm(asr, pcm, sizeof(pcm), 0),
                   TURBO_SPEECH_OK);
      check_equal(turbo_asr_write_pcm(asr, pcm, sizeof(pcm), 0),
                   TURBO_SPEECH_ERR_BUSY);
      check_equal(turbo_asr_get_rejected_frame_count(asr), 1u);
      check_equal(turbo_asr_cancel(asr), TURBO_SPEECH_OK);

      turbo_asr_destroy(asr);
      ivr_openai_asr_free(asr_wrap);
      mock_server_stop(&server);
    }
  }

  group("per-call factory") {
    it("creates and destroys 100 dual-call provider batches") {
      ivr_openai_config_t config;
      ivr_openai_speech_factory_t factory;
      ivr_speech_session_factory_ops_t ops;
      ivr_call_ref_t call;
      memset(&config, 0, sizeof(config));
      config.base_url = "http://127.0.0.1:1";
      config.max_asr_buffer_bytes = 4096;
      config.max_response_bytes = 4096;
      check_equal(ivr_openai_speech_factory_init(&factory, &config, NULL, &ops),
                   IVR_OK);
      memset(&call, 0, sizeof(call));
      call.room_id.data = "room";
      call.room_id.size = 4;
      call.call_id.data = "call";
      call.call_id.size = 4;
      call.call_generation = 1;
      for (int i = 0; i < 100; i++) {
        ivr_speech_session_t *session_a = NULL;
        ivr_speech_session_t *session_b = NULL;
        check_equal(ops.create(ops.context, &call, &session_a), IVR_OK);
        check_equal(ops.create(ops.context, &call, &session_b), IVR_OK);
        ops.destroy(ops.context, session_a);
        ops.destroy(ops.context, session_b);
      }
      {
        ivr_speech_resource_snapshot_t snapshot;
        ivr_openai_speech_factory_get_resource_snapshot(&factory, &snapshot);
        check(snapshot.active_sessions == 0);
        check(snapshot.provider.active_tts_instances == 0);
        check(snapshot.provider.active_asr_instances == 0);
        check(snapshot.provider.live_provider_threads == 0);
        check(snapshot.provider.retained_input_bytes == 0);
        check(snapshot.provider.retained_response_bytes == 0);
      }
      check_equal(ivr_openai_speech_factory_deinit(&factory), IVR_OK);
    }

    it("rolls back TTS when ASR provider creation fails") {
      ivr_openai_config_t config;
      ivr_openai_speech_factory_t factory;
      ivr_speech_session_factory_ops_t ops;
      ivr_speech_provider_create_ops_t create_ops;
      ivr_speech_session_t *session = (ivr_speech_session_t *)(uintptr_t)1;
      ivr_call_ref_t call;
      ivr_speech_resource_snapshot_t snapshot;
      int asr_create_calls = 0;

      memset(&config, 0, sizeof(config));
      config.base_url = "http://127.0.0.1:1";
      memset(&create_ops, 0, sizeof(create_ops));
      create_ops.context = &asr_create_calls;
      create_ops.create_asr = fail_asr_create;
      check_equal(ivr_openai_speech_factory_init(
                       &factory, &config, &create_ops, &ops),
                   IVR_OK);
      memset(&call, 0, sizeof(call));
      call.room_id.data = "room";
      call.room_id.size = 4;
      call.call_id.data = "partial-create";
      call.call_id.size = 14;
      call.call_generation = 1;

      check_equal(ops.create(ops.context, &call, &session), IVR_ESTATE);
      check_null(session);
      check_equal(asr_create_calls, 1);
      ivr_openai_speech_factory_get_resource_snapshot(&factory, &snapshot);
      check(snapshot.active_sessions == 0);
      check(snapshot.provider.active_tts_instances == 0);
      check(snapshot.provider.active_asr_instances == 0);
      check(snapshot.provider.live_provider_threads == 0);
      check(snapshot.provider.retained_input_bytes == 0);
      check(snapshot.provider.retained_response_bytes == 0);
      check_equal(ivr_openai_speech_factory_deinit(&factory), IVR_OK);
    }

    it("runs two TTS calls concurrently without provider busy leakage") {
      mock_server_t *server = (mock_server_t *)calloc(1, sizeof(*server));
      uint8_t pcm[MOCK_PCM_BYTES];
      char base_url[128];
      ivr_openai_config_t config;
      ivr_openai_speech_factory_t factory;
      ivr_speech_session_factory_ops_t ops;
      ivr_speech_session_t *session_a = NULL;
      ivr_speech_session_t *session_b = NULL;
      ivr_call_ref_t call_a;
      ivr_call_ref_t call_b;
      tts_observer_t observer_a;
      tts_observer_t observer_b;
      turbo_tts_callbacks_t callbacks;
      turbo_tts_request_t request;
      turbo_tts_t *tts_a;
      turbo_tts_t *tts_b;

      check_not_null(server);
      mock_server_init(server);
      mock_fill_pcm(pcm, sizeof(pcm));
      server->tts_pcm = pcm;
      server->tts_pcm_len = sizeof(pcm);
      server->tts_frame_delay_ms = 2;
      check_equal(mock_server_start(server), 0);
      mock_server_make_base_url(server, base_url, sizeof(base_url));
      memset(&config, 0, sizeof(config));
      config.base_url = base_url;
      config.sample_rate = 16000;
      config.timeout_ms = 5000;
      check_equal(ivr_openai_speech_factory_init(&factory, &config, NULL, &ops),
                   IVR_OK);

      memset(&call_a, 0, sizeof(call_a));
      call_a.room_id.data = "room";
      call_a.room_id.size = 4;
      call_a.call_id.data = "call-a";
      call_a.call_id.size = 6;
      call_a.call_generation = 1;
      call_b = call_a;
      call_b.call_id.data = "call-b";
      check_equal(ops.create(ops.context, &call_a, &session_a), IVR_OK);
      check_equal(ops.create(ops.context, &call_b, &session_b), IVR_OK);
      check_true(ivr_speech_session_tts(session_a)->context !=
                 ivr_speech_session_tts(session_b)->context);

      memset(&observer_a, 0, sizeof(observer_a));
      memset(&observer_b, 0, sizeof(observer_b));
      memset(&callbacks, 0, sizeof(callbacks));
      callbacks.on_audio = observe_tts_audio;
      callbacks.on_complete = observe_tts_complete;
      callbacks.on_error = observe_tts_error;
      tts_a = turbo_tts_create(ivr_speech_session_tts(session_a), &callbacks,
                               &observer_a);
      tts_b = turbo_tts_create(ivr_speech_session_tts(session_b), &callbacks,
                               &observer_b);
      check_not_null(tts_a);
      check_not_null(tts_b);
      memset(&request, 0, sizeof(request));
      request.text = "hello";
      request.text_len = 5;
      request.rate = 1.0f;
      request.pitch = 1.0f;
      check_equal(turbo_tts_synthesize(tts_a, &request), TURBO_SPEECH_OK);
      check_equal(turbo_tts_synthesize(tts_b, &request), TURBO_SPEECH_OK);
      check_equal(mock_wait_until(tts_done_cond, &observer_a, 5000), 0);
      check_equal(mock_wait_until(tts_done_cond, &observer_b, 5000), 0);
      check_equal(observer_a.complete_count, 1);
      check_equal(observer_b.complete_count, 1);
      check_equal(observer_a.error_count, 0);
      check_equal(observer_b.error_count, 0);

      turbo_tts_destroy(tts_a);
      turbo_tts_destroy(tts_b);
      ops.destroy(ops.context, session_a);
      ops.destroy(ops.context, session_b);
      check_equal(ivr_openai_speech_factory_deinit(&factory), IVR_OK);
      mock_server_stop(server);
      free(server);
    }

    it("keeps interleaved ASR transcripts and callback contexts per call") {
      static const uint8_t call_b_marker[32] = {
          0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
          0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
          0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
          0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5};
      mock_server_t *server = (mock_server_t *)calloc(1, sizeof(*server));
      char base_url[128];
      ivr_openai_config_t config;
      ivr_openai_speech_factory_t factory;
      ivr_speech_session_factory_ops_t ops;
      ivr_speech_session_t *session_a = NULL;
      ivr_speech_session_t *session_b = NULL;
      ivr_call_ref_t call;
      asr_observer_t observer_a;
      asr_observer_t observer_b;
      turbo_asr_callbacks_t callbacks;
      turbo_asr_config_t asr_config;
      turbo_asr_t *asr_a;
      turbo_asr_t *asr_b;
      uint8_t pcm_a[640] = {0};
      uint8_t pcm_b[640];

      check_not_null(server);
      mock_server_init(server);
      server->asr_json = "{\"text\":\"call-a-transcript\"}";
      server->asr_match = call_b_marker;
      server->asr_match_len = sizeof(call_b_marker);
      server->asr_match_json = "{\"text\":\"call-b-transcript\"}";
      memset(pcm_b, 0xa5, sizeof(pcm_b));
      check_equal(mock_server_start(server), 0);
      mock_server_make_base_url(server, base_url, sizeof(base_url));
      memset(&config, 0, sizeof(config));
      config.base_url = base_url;
      config.timeout_ms = 5000;
      check_equal(ivr_openai_speech_factory_init(&factory, &config, NULL, &ops),
                   IVR_OK);
      memset(&call, 0, sizeof(call));
      call.room_id.data = "room";
      call.room_id.size = 4;
      call.call_id.data = "call";
      call.call_id.size = 4;
      call.call_generation = 1;
      check_equal(ops.create(ops.context, &call, &session_a), IVR_OK);
      check_equal(ops.create(ops.context, &call, &session_b), IVR_OK);
      memset(&observer_a, 0, sizeof(observer_a));
      memset(&observer_b, 0, sizeof(observer_b));
      memset(&callbacks, 0, sizeof(callbacks));
      callbacks.on_result = observe_asr_result;
      callbacks.on_complete = observe_asr_complete;
      callbacks.on_error = observe_asr_error;
      asr_a = turbo_asr_create(ivr_speech_session_asr(session_a), &callbacks,
                               &observer_a);
      asr_b = turbo_asr_create(ivr_speech_session_asr(session_b), &callbacks,
                               &observer_b);
      check_not_null(asr_a);
      check_not_null(asr_b);
      memset(&asr_config, 0, sizeof(asr_config));
      asr_config.format.sample_rate = 16000;
      asr_config.format.channels = 1;
      asr_config.format.bits_per_sample = 16;
      check_equal(turbo_asr_start(asr_a, &asr_config), TURBO_SPEECH_OK);
      check_equal(turbo_asr_start(asr_b, &asr_config), TURBO_SPEECH_OK);
      check_equal(turbo_asr_write_pcm(asr_a, pcm_a, sizeof(pcm_a), 0),
                   TURBO_SPEECH_OK);
      check_equal(turbo_asr_write_pcm(asr_b, pcm_b, sizeof(pcm_b), 0),
                   TURBO_SPEECH_OK);
      check_equal(turbo_asr_finish(asr_a), TURBO_SPEECH_OK);
      check_equal(turbo_asr_finish(asr_b), TURBO_SPEECH_OK);
      check_equal(mock_wait_until(asr_done_cond, &observer_a, 5000), 0);
      check_equal(mock_wait_until(asr_done_cond, &observer_b, 5000), 0);
      check_equal(observer_a.text, "call-a-transcript");
      check_equal(observer_b.text, "call-b-transcript");
      check_equal(observer_a.complete_count, 1);
      check_equal(observer_b.complete_count, 1);
      check_equal(observer_a.error_count, 0);
      check_equal(observer_b.error_count, 0);

      turbo_asr_destroy(asr_a);
      turbo_asr_destroy(asr_b);
      ops.destroy(ops.context, session_a);
      ops.destroy(ops.context, session_b);
      check_equal(ivr_openai_speech_factory_deinit(&factory), IVR_OK);
      mock_server_stop(server);
      free(server);
    }

    it("canceling call A does not cancel call B ASR final") {
      mock_server_t *server = (mock_server_t *)calloc(1, sizeof(*server));
      char base_url[128];
      ivr_openai_config_t config;
      ivr_openai_speech_factory_t factory;
      ivr_speech_session_factory_ops_t ops;
      ivr_speech_session_t *session_a = NULL;
      ivr_speech_session_t *session_b = NULL;
      ivr_call_ref_t call;
      asr_observer_t observer_a;
      asr_observer_t observer_b;
      turbo_asr_callbacks_t callbacks;
      turbo_asr_config_t asr_config;
      turbo_asr_t *asr_a;
      turbo_asr_t *asr_b;
      uint8_t pcm[640] = {0};

      check_not_null(server);
      mock_server_init(server);
      check_equal(mock_server_start(server), 0);
      mock_server_make_base_url(server, base_url, sizeof(base_url));
      memset(&config, 0, sizeof(config));
      config.base_url = base_url;
      config.timeout_ms = 5000;
      check_equal(ivr_openai_speech_factory_init(&factory, &config, NULL, &ops),
                   IVR_OK);
      memset(&call, 0, sizeof(call));
      call.room_id.data = "room";
      call.room_id.size = 4;
      call.call_id.data = "call";
      call.call_id.size = 4;
      call.call_generation = 1;
      check_equal(ops.create(ops.context, &call, &session_a), IVR_OK);
      check_equal(ops.create(ops.context, &call, &session_b), IVR_OK);

      memset(&observer_a, 0, sizeof(observer_a));
      memset(&observer_b, 0, sizeof(observer_b));
      memset(&callbacks, 0, sizeof(callbacks));
      callbacks.on_result = observe_asr_result;
      callbacks.on_complete = observe_asr_complete;
      callbacks.on_error = observe_asr_error;
      asr_a = turbo_asr_create(ivr_speech_session_asr(session_a), &callbacks,
                               &observer_a);
      asr_b = turbo_asr_create(ivr_speech_session_asr(session_b), &callbacks,
                               &observer_b);
      check_not_null(asr_a);
      check_not_null(asr_b);
      memset(&asr_config, 0, sizeof(asr_config));
      asr_config.format.sample_rate = 16000;
      asr_config.format.channels = 1;
      asr_config.format.bits_per_sample = 16;
      check_equal(turbo_asr_start(asr_a, &asr_config), TURBO_SPEECH_OK);
      check_equal(turbo_asr_start(asr_b, &asr_config), TURBO_SPEECH_OK);
      check_equal(turbo_asr_write_pcm(asr_a, pcm, sizeof(pcm), 0),
                   TURBO_SPEECH_OK);
      check_equal(turbo_asr_write_pcm(asr_b, pcm, sizeof(pcm), 0),
                   TURBO_SPEECH_OK);
      check_equal(turbo_asr_cancel(asr_a), TURBO_SPEECH_OK);
      check_equal(turbo_asr_finish(asr_b), TURBO_SPEECH_OK);
      check_equal(mock_wait_until(asr_done_cond, &observer_b, 5000), 0);
      check_equal(observer_a.result_count, 0);
      check_equal(observer_b.result_count, 1);
      check_equal(observer_b.text, "hello remote world");

      turbo_asr_destroy(asr_a);
      turbo_asr_destroy(asr_b);
      ops.destroy(ops.context, session_a);
      ops.destroy(ops.context, session_b);
      check_equal(ivr_openai_speech_factory_deinit(&factory), IVR_OK);
      mock_server_stop(server);
      free(server);
    }
  }
}
