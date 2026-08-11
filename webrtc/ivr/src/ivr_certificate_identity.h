#ifndef TURBO_MEDIA_IVR_CERTIFICATE_IDENTITY_H
#define TURBO_MEDIA_IVR_CERTIFICATE_IDENTITY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IVR_CERTIFICATE_IDENTITY_MAX_ENTRIES 256u
#define IVR_CERTIFICATE_SHA256_TEXT_SIZE 72u
#define IVR_CERTIFICATE_IDENTITY_MAX_WORKER_ID 127u

typedef uint64_t (*ivr_certificate_identity_clock_fn)(void *context);

typedef struct {
    const char *worker_id;
    const char *active_certificate_sha256;
    const char *previous_certificate_sha256;
    /* Unix epoch milliseconds. The injectable clock must use the same epoch. */
    uint64_t previous_expires_at_ms;
    uint64_t generation;
} ivr_certificate_identity_entry_t;

typedef struct {
    size_t size;
    const ivr_certificate_identity_entry_t *entries;
    size_t entry_count;
    ivr_certificate_identity_clock_fn clock;
    void *clock_context;
} ivr_certificate_identity_config_t;

#define IVR_CERTIFICATE_IDENTITY_CONFIG_INIT \
    {sizeof(ivr_certificate_identity_config_t), NULL, 0u, NULL, NULL}

typedef struct ivr_certificate_identity_s ivr_certificate_identity_t;

int ivr_certificate_identity_create(
    const ivr_certificate_identity_config_t *config,
    ivr_certificate_identity_t **out_identity);
void ivr_certificate_identity_destroy(ivr_certificate_identity_t *identity);

/* FlowMQ BIND-side callback. Context is the owner returned by create(). */
int ivr_certificate_identity_verify(void *context,
                                    const char *certificate_sha256,
                                    const char *claimed_identity);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_MEDIA_IVR_CERTIFICATE_IDENTITY_H */
