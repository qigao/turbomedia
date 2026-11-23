#ifndef TURBO_SIP_CLIENT_H
#define TURBO_SIP_CLIENT_H

#include <sip-agent.h>
#include <sip-registration.h>
#include <turbo_sdp.h>

#if defined(_WIN32) && defined(TURBO_SIP_CLIENT_SHARED)
#if defined(TURBO_SIP_CLIENT_EXPORTS)
#define TURBO_SIP_CLIENT_API __declspec(dllexport)
#else
#define TURBO_SIP_CLIENT_API __declspec(dllimport)
#endif
#else
#define TURBO_SIP_CLIENT_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_sip_client turbo_sip_client_t;
typedef struct turbo_sip_call turbo_sip_call_t;

typedef enum turbo_sip_call_state {
    TURBO_SIP_CALL_DIALING,
    TURBO_SIP_CALL_RINGING,
    TURBO_SIP_CALL_INCOMING,
    TURBO_SIP_CALL_ANSWERING,
    TURBO_SIP_CALL_ACTIVE,
    TURBO_SIP_CALL_ENDING,
    TURBO_SIP_CALL_ENDED
} turbo_sip_call_state_t;

typedef enum turbo_sip_event_type {
    TURBO_SIP_REGISTRATION,
    TURBO_SIP_CALL_CHANGED,
    TURBO_SIP_MESSAGE_RECEIVED,
    TURBO_SIP_MESSAGE_RESULT
} turbo_sip_event_type_t;

typedef enum turbo_sip_packet_kind {
    TURBO_SIP_RTP,
    TURBO_SIP_RTCP
} turbo_sip_packet_kind_t;

typedef struct turbo_sip_event {
    turbo_sip_event_type_t type;
    turbo_sip_call_t* call;
    turbo_sip_call_state_t call_state;
    sip_registration_status_t registration;
    int sip_code;
    int error;
    vstr from;
    vstr content_type;
    vstr body;
} turbo_sip_event_t;

typedef struct turbo_sip_signaling {
    struct sip_transport_t request;
    /* Addresses and bytes are borrowed through return. Route replies using
     * the received parameters from the top Via. */
    int (*reply)(void* context, const vstr* protocol, const vstr* sent_by, const vstr* received,
                 int rport, const void* packet, int bytes);
    void* context;
} turbo_sip_signaling_t;

typedef struct turbo_sip_media {
    /* Fill port, connection address and optional ICE attributes in the
     * initialized local RTP/AVP media section. Preserve its codec list.
     * On error clean up allocations made during this callback. */
    int (*prepare)(void* context, turbo_sip_call_t* call, int offerer, sdp_media_t* local);
    /* Apply negotiated endpoints after SIP establishment. Copy fields
     * needed after return. The transport can return -EAGAIN from send
     * while connectivity checks progress. */
    int (*start)(void* context, turbo_sip_call_t* call,
                 const sdp_media_t* local, const sdp_media_t* remote);
    int (*send)(void* context, turbo_sip_call_t* call, turbo_sip_packet_kind_t kind,
                const void* packet, size_t bytes);
    /* Matches each successful prepare, including failed or rejected calls.
     * Stop using the call handle before returning. */
    void (*close)(void* context, turbo_sip_call_t* call);
    void* context;
} turbo_sip_media_t;

typedef struct turbo_sip_client_config {
    const char* address_of_record;
    const char* registrar;    /* Optional for direct calls. */
    const char* contact;      /* Optional explicit local Contact header value. */
    const char* username;     /* Both credentials are optional. */
    const char* password;
    unsigned expires_seconds; /* Default 300 when registrar is set. */
    const char* audio_codec;  /* "pcmu" (default), "pcma" or "opus". */
    turbo_sip_signaling_t signaling;
    turbo_sip_media_t media;  /* May be empty for registration and messaging only. */
    void (*on_event)(void* context, const turbo_sip_event_t* event);
    void (*on_pcm)(void* context, turbo_sip_call_t* call, const int16_t* samples, size_t count);
    void* context;
} turbo_sip_client_config_t;

/* One client owns one account, one current call and bounded SIP transactions.
 * All calls and callbacks run on a single application owner. Callbacks borrow
 * their inputs and must not reenter the client. Configuration strings are
 * copied. Callback contexts are borrowed through destroy. */
TURBO_SIP_CLIENT_API turbo_sip_client_t* turbo_sip_client_create(const turbo_sip_client_config_t* config);
/* Closes media, cancels local timers and releases all handles. Use hangup and
 * unregister first, and poll to their final events, for peer-visible shutdown. */
TURBO_SIP_CLIENT_API void turbo_sip_client_destroy(turbo_sip_client_t* client);

/* Drive timers and registration with nondecreasing monotonic milliseconds.
 * Poll at least once before starting work. Zero means success and negative
 * values describe local errors. SIP outcomes are delivered in events. */
TURBO_SIP_CLIENT_API int turbo_sip_client_poll(turbo_sip_client_t* client, uint64_t now_ms);
/* Accept a complete UDP message from a peer admitted by the application.
 * For requests, peer_address/peer_port supply the observed Via mapping. */
TURBO_SIP_CLIENT_API int turbo_sip_client_input(
    turbo_sip_client_t* client, const void* packet, size_t bytes,
    const char* peer_address, uint16_t peer_port);

TURBO_SIP_CLIENT_API int turbo_sip_client_register(turbo_sip_client_t* client);
TURBO_SIP_CLIENT_API int turbo_sip_client_unregister(turbo_sip_client_t* client);
/* One outbound MESSAGE at a time. The body is copied before return. */
TURBO_SIP_CLIENT_API int turbo_sip_client_message(
    turbo_sip_client_t* client, const char* target,
    const char* content_type, const void* body, size_t bytes);

/* The owner releases each ended handle to admit the next call. Incoming
 * handles are delivered in the INCOMING event. */
TURBO_SIP_CLIENT_API int turbo_sip_client_dial(
    turbo_sip_client_t* client, const char* target, turbo_sip_call_t** out);
TURBO_SIP_CLIENT_API int turbo_sip_call_answer(turbo_sip_call_t* call);
/* Decline an incoming call, cancel an outbound INVITE, or send BYE. */
TURBO_SIP_CLIENT_API int turbo_sip_call_hangup(turbo_sip_call_t* call);
TURBO_SIP_CLIENT_API int turbo_sip_call_release(turbo_sip_call_t* call);

/* One 20-ms interleaved PCM frame. G.711 uses 8000 Hz/mono and Opus uses
 * 48000 Hz/stereo. A frame handed to media.send advances RTP time even
 * if transport admission fails. */
TURBO_SIP_CLIENT_API int turbo_sip_call_send_pcm(
    turbo_sip_call_t* call, const int16_t* samples, size_t count);
/* Deliver RTP or RTCP from the admitted media peer on the owner. */
TURBO_SIP_CLIENT_API int turbo_sip_call_input_media(
    turbo_sip_call_t* call, turbo_sip_packet_kind_t kind, const void* packet, size_t bytes);

#ifdef __cplusplus
}
#endif
#endif
