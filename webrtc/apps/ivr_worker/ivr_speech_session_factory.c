#include "ivr_speech_session_factory.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct ivr_speech_session_s {
    ivr_openai_speech_factory_t *factory;
    ivr_openai_tts_t *tts;
    ivr_openai_asr_t *asr;
    turbo_tts_provider_t tts_provider;
    turbo_asr_provider_t asr_provider;
};

static int ivr_default_tts_create(void *context,
                                  const ivr_openai_config_t *config,
                                  ivr_openai_tts_t **out_tts) {
    (void)context;
    return ivr_openai_tts_create(config, out_tts);
}

static int ivr_default_asr_create(void *context,
                                  const ivr_openai_config_t *config,
                                  ivr_openai_asr_t **out_asr) {
    (void)context;
    return ivr_openai_asr_create(config, out_asr);
}

static int ivr_openai_speech_factory_copy_config(
    ivr_openai_speech_factory_t *factory,
    const ivr_openai_config_t *source) {
    const char *values[] = {
        source->base_url, source->api_key, source->tts_path,
        source->tts_model, source->tts_voice, source->asr_path,
        source->asr_model, source->asr_language, source->ca_file,
        source->server_name, source->user_agent};
    const char **destinations[] = {
        &factory->config.base_url, &factory->config.api_key,
        &factory->config.tts_path, &factory->config.tts_model,
        &factory->config.tts_voice, &factory->config.asr_path,
        &factory->config.asr_model, &factory->config.asr_language,
        &factory->config.ca_file, &factory->config.server_name,
        &factory->config.user_agent};
    size_t total = 0;
    size_t i;
    char *cursor;

    factory->config = *source;
    for (i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        size_t length;
        if (!values[i]) {
            continue;
        }
        length = strlen(values[i]) + 1u;
        if (length == 0 || total > SIZE_MAX - length) {
            return -1;
        }
        total += length;
    }
    factory->config_storage = (char *)malloc(total);
    if (!factory->config_storage) {
        return -1;
    }
    cursor = factory->config_storage;
    for (i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        size_t length;
        if (!values[i]) {
            *destinations[i] = NULL;
            continue;
        }
        length = strlen(values[i]) + 1u;
        memcpy(cursor, values[i], length);
        *destinations[i] = cursor;
        cursor += length;
    }
    return 0;
}

static void ivr_openai_speech_session_destroy(
    void *context, ivr_speech_session_t *session) {
    (void)context;
    if (!session) {
        return;
    }
    ivr_openai_asr_free(session->asr);
    session->asr = NULL;
    ivr_openai_tts_free(session->tts);
    session->tts = NULL;
    if (session->factory) {
        atomic_fetch_sub_explicit(&session->factory->active_sessions, 1u,
                                  memory_order_relaxed);
    }
    free(session);
}

static ivr_status_t ivr_openai_speech_session_create(
    void *context, const ivr_call_ref_t *call,
    ivr_speech_session_t **out_session) {
    ivr_openai_speech_factory_t *factory =
        (ivr_openai_speech_factory_t *)context;
    ivr_speech_session_t *session;
    if (!factory || !call || !out_session || !factory->config.base_url ||
        factory->config.base_url[0] == '\0') {
        return IVR_EINVAL;
    }
    *out_session = NULL;
    session = (ivr_speech_session_t *)calloc(1, sizeof(*session));
    if (!session) {
        return IVR_ENOSPC;
    }
    if (factory->provider_create.create_tts(
            factory->provider_create.context, &factory->config,
            &session->tts) != 0) {
        ivr_openai_speech_session_destroy(factory, session);
        return IVR_ESTATE;
    }
    if (factory->provider_create.create_asr(
            factory->provider_create.context, &factory->config,
            &session->asr) != 0) {
        ivr_openai_speech_session_destroy(factory, session);
        return IVR_ESTATE;
    }
    ivr_openai_tts_get_provider(session->tts, &session->tts_provider);
    ivr_openai_asr_get_provider(session->asr, &session->asr_provider);
    session->factory = factory;
    atomic_fetch_add_explicit(&factory->active_sessions, 1u,
                              memory_order_relaxed);
    *out_session = session;
    return IVR_OK;
}

static ivr_status_t ivr_openai_speech_factory_probe(
    void *context, char *error, size_t error_capacity) {
    static const char probe_room[] = "health";
    static const char probe_call[] = "speech-provider";
    ivr_call_ref_t call;
    ivr_speech_session_t *session = NULL;
    memset(&call, 0, sizeof(call));
    call.room_id.data = probe_room;
    call.room_id.size = sizeof(probe_room) - 1u;
    call.call_id.data = probe_call;
    call.call_id.size = sizeof(probe_call) - 1u;
    call.call_generation = 1;
    ivr_status_t rc =
        ivr_openai_speech_session_create(context, &call, &session);
    if (rc != IVR_OK && error && error_capacity > 0) {
        snprintf(error, error_capacity,
                 "could not create isolated TTS/ASR provider threads");
    }
    ivr_openai_speech_session_destroy(context, session);
    return rc;
}

ivr_status_t ivr_openai_speech_factory_init(
    ivr_openai_speech_factory_t *factory,
    const ivr_openai_config_t *immutable_config,
    const ivr_speech_provider_create_ops_t *provider_create,
    ivr_speech_session_factory_ops_t *out_ops) {
    if (!factory || !immutable_config || !immutable_config->base_url ||
        immutable_config->base_url[0] == '\0' || !out_ops) {
        return IVR_EINVAL;
    }
    memset(factory, 0, sizeof(*factory));
    if (ivr_openai_speech_factory_copy_config(factory, immutable_config) != 0) {
        memset(factory, 0, sizeof(*factory));
        return IVR_ENOSPC;
    }
    if (provider_create) {
        factory->provider_create = *provider_create;
    }
    if (!factory->provider_create.create_tts) {
        factory->provider_create.create_tts = ivr_default_tts_create;
    }
    if (!factory->provider_create.create_asr) {
        factory->provider_create.create_asr = ivr_default_asr_create;
    }
    atomic_init(&factory->active_sessions, 0u);
    memset(out_ops, 0, sizeof(*out_ops));
    out_ops->context = factory;
    out_ops->create = ivr_openai_speech_session_create;
    out_ops->destroy = ivr_openai_speech_session_destroy;
    out_ops->probe = ivr_openai_speech_factory_probe;
    return IVR_OK;
}

ivr_status_t ivr_openai_speech_factory_deinit(
    ivr_openai_speech_factory_t *factory) {
    if (!factory) {
        return IVR_EINVAL;
    }
    if (atomic_load_explicit(&factory->active_sessions,
                             memory_order_acquire) != 0) {
        return IVR_ESTATE;
    }
    free(factory->config_storage);
    memset(factory, 0, sizeof(*factory));
    return IVR_OK;
}

void ivr_openai_speech_factory_get_resource_snapshot(
    const ivr_openai_speech_factory_t *factory,
    ivr_speech_resource_snapshot_t *out_snapshot) {
    if (!out_snapshot) {
        return;
    }
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    if (factory) {
        out_snapshot->active_sessions = atomic_load_explicit(
            &factory->active_sessions, memory_order_acquire);
    }
    ivr_openai_get_resource_snapshot(&out_snapshot->provider);
}

const turbo_tts_provider_t *ivr_speech_session_tts(
    const ivr_speech_session_t *session) {
    return session ? &session->tts_provider : NULL;
}

const turbo_asr_provider_t *ivr_speech_session_asr(
    const ivr_speech_session_t *session) {
    return session ? &session->asr_provider : NULL;
}
