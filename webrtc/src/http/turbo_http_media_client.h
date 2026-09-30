#ifndef TURBO_MEDIA_HTTP_MEDIA_CLIENT_H
#define TURBO_MEDIA_HTTP_MEDIA_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#define TURBO_HTTP_MEDIA_MAX_SDP 16384u
#define TURBO_HTTP_MEDIA_MAX_RESPONSE 32768u

typedef struct {
    int status;
    char location[512];
    char etag[128];
    char content_type[128];
    char body[TURBO_HTTP_MEDIA_MAX_RESPONSE];
} turbo_http_media_response_t;

typedef struct turbo_http_media_client_s turbo_http_media_client_t;

typedef struct {
    size_t size;
    const char *base_url;
    const char *media_token;
    const char *ca_file;
    const char *cert_file;
    const char *key_file;
    const char *key_password;
    const char *server_name;
    uint64_t timeout_ms;
    int allow_plaintext_loopback;
} turbo_http_media_client_config_t;

#define TURBO_HTTP_MEDIA_CLIENT_CONFIG_INIT                                  \
    {                                                                      \
        sizeof(turbo_http_media_client_config_t), NULL, NULL, NULL, NULL,    \
            NULL, NULL, NULL, 0u, 0                                        \
    }

char *turbo_http_media_strdup(const char *value);

/* Returns a caller-owned RFC 3986 path segment. Control bytes are rejected. */
char *turbo_http_media_encode_path_segment(const char *value);

int turbo_http_media_client_create(
    const turbo_http_media_client_config_t *config,
    turbo_http_media_client_t **out_client);
void turbo_http_media_client_destroy(turbo_http_media_client_t *client);

int turbo_http_media_request(turbo_http_media_client_t *client,
                           const char *method, const char *path,
                           const char *content_type, const char *if_match,
                           const char *body,
                           turbo_http_media_response_t *response);

int turbo_http_media_sdp_attr_value(const char *sdp, const char *attribute, char *out,
                       size_t capacity);

void turbo_http_media_sdp_build_minimal_audio_offer(const char *source, char *out,
                                       size_t capacity, int sample_rate,
                                       const char *direction);

void turbo_http_media_sdp_append_candidates(const char *offer, char *out, size_t capacity);

#endif /* TURBO_MEDIA_HTTP_MEDIA_CLIENT_H */
