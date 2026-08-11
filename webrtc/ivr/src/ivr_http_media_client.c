#include "ivr_http_media_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET ivr_http_media_socket_t;
#define IVR_HTTP_MEDIA_INVALID_SOCKET INVALID_SOCKET
#define ivr_http_media_strncasecmp _strnicmp
static void ivr_http_media_close(ivr_http_media_socket_t socket_handle) {
    closesocket(socket_handle);
}
static void ivr_http_media_init(void) {
    WSADATA data;
    (void)WSAStartup(MAKEWORD(2, 2), &data);
}
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int ivr_http_media_socket_t;
#define IVR_HTTP_MEDIA_INVALID_SOCKET (-1)
#define ivr_http_media_strncasecmp strncasecmp
static void ivr_http_media_close(ivr_http_media_socket_t socket_handle) {
    close(socket_handle);
}
static void ivr_http_media_init(void) {}
#endif

char *ivr_http_media_strdup(const char *value) {
    size_t length;
    char *copy;

    if (!value) {
        return NULL;
    }
    length = strlen(value);
    copy = (char *)malloc(length + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, value, length + 1);
    return copy;
}

static int ivr_http_media_send_all(ivr_http_media_socket_t socket_handle,
                                   const char *data, size_t length) {
    size_t offset = 0;

    while (offset < length) {
#ifdef _WIN32
        int written = send(socket_handle, data + offset,
                           (int)(length - offset), 0);
#else
        ssize_t written = send(socket_handle, data + offset,
                               length - offset, 0);
#endif
        if (written <= 0) {
            return -1;
        }
        offset += (size_t)written;
    }
    return 0;
}

static void ivr_http_media_header_value(const char *headers,
                                        const char *name, size_t name_length,
                                        char *out, size_t out_capacity) {
    const char *line = headers;

    if (!headers || !name || !out || out_capacity == 0) {
        return;
    }
    while (*line) {
        const char *end = strstr(line, "\r\n");
        if (!end) {
            break;
        }
        if ((size_t)(end - line) > name_length &&
            ivr_http_media_strncasecmp(line, name, name_length) == 0 &&
            line[name_length] == ':') {
            const char *value = line + name_length + 1;
            size_t value_length;
            while (*value == ' ') {
                value++;
            }
            value_length = (size_t)(end - value);
            if (value_length >= out_capacity) {
                value_length = out_capacity - 1;
            }
            memcpy(out, value, value_length);
            out[value_length] = '\0';
            return;
        }
        line = end + 2;
    }
    out[0] = '\0';
}

int ivr_http_media_request(const char *host, int port, const char *method,
                           const char *path, const char *token,
                           const char *content_type, const char *if_match,
                           const char *body,
                           ivr_http_media_response_t *response) {
    ivr_http_media_socket_t socket_handle;
    struct sockaddr_in address;
    char request[IVR_HTTP_MEDIA_MAX_RESPONSE + 512];
    char raw[IVR_HTTP_MEDIA_MAX_RESPONSE + 1024];
    char headers[4096];
    size_t total = 0;
    size_t header_length;
    const char *status_separator;
    const char *header_end;
    int request_length;

    if (!host || port <= 0 || !method || !path || !response) {
        return -1;
    }
    memset(response, 0, sizeof(*response));
    ivr_http_media_init();
    socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == IVR_HTTP_MEDIA_INVALID_SOCKET) {
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)port);
    address.sin_addr.s_addr = inet_addr(host);
    if (address.sin_addr.s_addr == INADDR_NONE ||
        connect(socket_handle, (struct sockaddr *)&address,
                sizeof(address)) != 0) {
        ivr_http_media_close(socket_handle);
        return -1;
    }
    request_length = snprintf(
        request, sizeof(request),
        "%s %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Authorization: Bearer %s\r\n"
        "%s%s%s"
        "%s%s%s"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n%s",
        method, path, host, port, token ? token : "",
        content_type ? "Content-Type: " : "", content_type ? content_type : "",
        content_type ? "\r\n" : "", if_match ? "If-Match: " : "",
        if_match ? if_match : "", if_match ? "\r\n" : "",
        body ? strlen(body) : 0, body ? body : "");
    if (request_length < 0 || (size_t)request_length >= sizeof(request)) {
        ivr_http_media_close(socket_handle);
        return -1;
    }
#ifdef _WIN32
    {
        DWORD timeout_ms = 5000;
        (void)setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO,
                         (const char *)&timeout_ms, sizeof(timeout_ms));
    }
#else
    {
        struct timeval timeout;
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;
        (void)setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                         sizeof(timeout));
    }
#endif
    if (ivr_http_media_send_all(socket_handle, request,
                                (size_t)request_length) != 0) {
        ivr_http_media_close(socket_handle);
        return -1;
    }
    while (total + 1 < sizeof(raw)) {
#ifdef _WIN32
        int received = recv(socket_handle, raw + total,
                            (int)(sizeof(raw) - 1 - total), 0);
#else
        ssize_t received = read(socket_handle, raw + total,
                                sizeof(raw) - 1 - total);
#endif
        if (received <= 0) {
            break;
        }
        total += (size_t)received;
    }
    raw[total] = '\0';
    ivr_http_media_close(socket_handle);

    status_separator = strchr(raw, ' ');
    header_end = strstr(raw, "\r\n\r\n");
    if (!status_separator || !header_end) {
        return -1;
    }
    response->status = atoi(status_separator + 1);
    header_length = (size_t)(header_end - raw);
    if (header_length >= sizeof(headers)) {
        return -1;
    }
    memcpy(headers, raw, header_length);
    headers[header_length] = '\0';
    ivr_http_media_header_value(headers, "Location", 8, response->location,
                                sizeof(response->location));
    ivr_http_media_header_value(headers, "ETag", 4, response->etag,
                                sizeof(response->etag));
    if (strlen(header_end + 4) >= sizeof(response->body)) {
        return -1;
    }
    memcpy(response->body, header_end + 4, strlen(header_end + 4) + 1);
    return 0;
}

int ivr_sdp_attr_value(const char *sdp, const char *attribute, char *out,
                       size_t capacity) {
    const char *value = sdp && attribute ? strstr(sdp, attribute) : NULL;
    size_t length = 0;

    if (!value || !out || capacity == 0) {
        return 0;
    }
    value += strlen(attribute);
    while (value[length] && value[length] != '\r' && value[length] != '\n' &&
           length + 1 < capacity) {
        out[length] = value[length];
        length++;
    }
    out[length] = '\0';
    return length > 0;
}

void ivr_sdp_build_minimal_audio_offer(const char *source, char *out,
                                       size_t capacity, int sample_rate,
                                       const char *direction) {
    char ufrag[64];
    char password[96];
    char fingerprint[256];

    if (!source || !out || capacity == 0 || !direction ||
        !ivr_sdp_attr_value(source, "a=ice-ufrag:", ufrag, sizeof(ufrag)) ||
        !ivr_sdp_attr_value(source, "a=ice-pwd:", password,
                            sizeof(password)) ||
        !ivr_sdp_attr_value(source, "a=fingerprint:sha-256 ", fingerprint,
                            sizeof(fingerprint))) {
        if (source && out && capacity > 0) {
            snprintf(out, capacity, "%s", source);
        }
        return;
    }
    snprintf(out, capacity,
             "v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n"
             "m=audio 9 UDP/TLS/RTP/SAVPF 111 126\r\nc=IN IP4 0.0.0.0\r\n"
             "a=ice-ufrag:%s\r\na=ice-pwd:%s\r\n"
             "a=fingerprint:sha-256 %s\r\n"
             "a=setup:actpass\r\na=mid:0\r\na=%s\r\n"
             "a=rtcp-mux\r\na=rtpmap:111 opus/%d/2\r\n"
             "a=rtpmap:126 telephone-event/8000\r\n"
             "a=fmtp:126 0-16\r\n",
             ufrag, password, fingerprint, direction, sample_rate);
}

void ivr_sdp_append_candidates(const char *offer, char *out, size_t capacity) {
    size_t used = 0;
    const char *line = offer;

    if (!out || capacity == 0) {
        return;
    }
    while (line && *line && used + 1 < capacity) {
        const char *end = strstr(line, "\r\n");
        size_t length = end ? (size_t)(end - line) : strlen(line);
        if (length >= 12 && strncmp(line, "a=candidate:", 12) == 0) {
            if (used + length + 3 > capacity) {
                break;
            }
            memcpy(out + used, line, length);
            used += length;
            memcpy(out + used, "\r\n", 2);
            used += 2;
        }
        if (!end) {
            break;
        }
        line = end + 2;
    }
    out[used] = '\0';
}
