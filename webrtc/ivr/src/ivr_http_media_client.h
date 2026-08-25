#ifndef TURBO_MEDIA_IVR_HTTP_MEDIA_CLIENT_H
#define TURBO_MEDIA_IVR_HTTP_MEDIA_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#define IVR_HTTP_MEDIA_MAX_SDP 16384u
#define IVR_HTTP_MEDIA_MAX_RESPONSE 32768u

typedef struct {
    int status;
    char location[512];
    char etag[128];
    char body[IVR_HTTP_MEDIA_MAX_RESPONSE];
} ivr_http_media_response_t;

typedef struct ivr_http_media_client_s ivr_http_media_client_t;

typedef struct {
    size_t size;
    const char *base_url;
    const char *media_token;
    const char *ca_file;
    const char *cert_file;
    const char *key_file;
    const char *key_password;
    uint64_t timeout_ms;
    int allow_plaintext_loopback;
} ivr_http_media_client_config_t;

#define IVR_HTTP_MEDIA_CLIENT_CONFIG_INIT                                  \
    {                                                                      \
        sizeof(ivr_http_media_client_config_t), NULL, NULL, NULL, NULL,    \
            NULL, NULL, 0u, 0                                              \
    }

char *ivr_http_media_strdup(const char *value);

/* Returns a caller-owned RFC 3986 path segment. Control bytes are rejected. */
char *ivr_http_media_encode_path_segment(const char *value);

int ivr_http_media_client_create(
    const ivr_http_media_client_config_t *config,
    ivr_http_media_client_t **out_client);
void ivr_http_media_client_destroy(ivr_http_media_client_t *client);

int ivr_http_media_request(ivr_http_media_client_t *client,
                           const char *method, const char *path,
                           const char *content_type, const char *if_match,
                           const char *body,
                           ivr_http_media_response_t *response);

int ivr_sdp_attr_value(const char *sdp, const char *attribute, char *out,
                       size_t capacity);

void ivr_sdp_build_minimal_audio_offer(const char *source, char *out,
                                       size_t capacity, int sample_rate,
                                       const char *direction);

void ivr_sdp_append_candidates(const char *offer, char *out, size_t capacity);

#endif /* TURBO_MEDIA_IVR_HTTP_MEDIA_CLIENT_H */
