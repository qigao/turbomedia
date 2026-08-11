#ifndef IVR_WORKER_SPEECH_SESSION_FACTORY_H
#define IVR_WORKER_SPEECH_SESSION_FACTORY_H

#include "ivr/ivr_worker.h"
#include "ivr_openai_provider.h"
#include <stdatomic.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ivr_speech_session_s ivr_speech_session_t;

/* App-internal factory contract. The factory context is borrowed and immutable
   for the worker lifetime. A successful create returns one owned, per-call
   session; destroy is NULL-safe and joins provider threads before returning. */
typedef struct {
    void *context;
    ivr_status_t (*create)(void *context, const ivr_call_ref_t *call,
                           ivr_speech_session_t **out_session);
    void (*destroy)(void *context, ivr_speech_session_t *session);
    ivr_status_t (*probe)(void *context, char *error, size_t error_capacity);
} ivr_speech_session_factory_ops_t;

/* Injectable creation strategy used to verify partial construction rollback.
   Implementations must return genuine OpenAI wrapper objects because the
   factory releases successful partial results with ivr_openai_*_free(). */
typedef struct {
    void *context;
    int (*create_tts)(void *context, const ivr_openai_config_t *config,
                      ivr_openai_tts_t **out_tts);
    int (*create_asr)(void *context, const ivr_openai_config_t *config,
                      ivr_openai_asr_t **out_asr);
} ivr_speech_provider_create_ops_t;

typedef struct {
    ivr_openai_config_t config;
    char *config_storage;
    ivr_speech_provider_create_ops_t provider_create;
    atomic_uint_least64_t active_sessions;
} ivr_openai_speech_factory_t;

/* immutable_config strings are copied into factory-owned storage. Optional
   provider_create callbacks are copied; NULL callbacks select the production
   OpenAI constructors. The factory must outlive all created sessions. */
ivr_status_t ivr_openai_speech_factory_init(
    ivr_openai_speech_factory_t *factory,
    const ivr_openai_config_t *immutable_config,
    const ivr_speech_provider_create_ops_t *provider_create,
    ivr_speech_session_factory_ops_t *out_ops);

/* Fails while sessions are active. On success, releases the owned immutable
   config and invalidates the factory. */
ivr_status_t ivr_openai_speech_factory_deinit(
    ivr_openai_speech_factory_t *factory);

typedef struct {
    uint64_t active_sessions;
    ivr_openai_resource_snapshot_t provider;
} ivr_speech_resource_snapshot_t;

void ivr_openai_speech_factory_get_resource_snapshot(
    const ivr_openai_speech_factory_t *factory,
    ivr_speech_resource_snapshot_t *out_snapshot);

const turbo_tts_provider_t *ivr_speech_session_tts(
    const ivr_speech_session_t *session);
const turbo_asr_provider_t *ivr_speech_session_asr(
    const ivr_speech_session_t *session);

#ifdef __cplusplus
}
#endif

#endif /* IVR_WORKER_SPEECH_SESSION_FACTORY_H */
