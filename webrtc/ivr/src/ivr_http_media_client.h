#ifndef TURBO_MEDIA_IVR_HTTP_MEDIA_CLIENT_H
#define TURBO_MEDIA_IVR_HTTP_MEDIA_CLIENT_H

#include <stddef.h>

#define IVR_HTTP_MEDIA_MAX_SDP 16384u
#define IVR_HTTP_MEDIA_MAX_RESPONSE 32768u

typedef struct {
    int status;
    char location[512];
    char etag[128];
    char body[IVR_HTTP_MEDIA_MAX_RESPONSE];
} ivr_http_media_response_t;

char *ivr_http_media_strdup(const char *value);

int ivr_http_media_request(const char *host, int port, const char *method,
                           const char *path, const char *token,
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
