#ifndef TURBO_MEDIA_SIP_REGISTRATION_H
#define TURBO_MEDIA_SIP_REGISTRATION_H

#include <stdint.h>
#include "sip-transport.h"

#ifdef __cplusplus
extern "C" {
#endif

struct sip_agent_t;
typedef struct sip_registration sip_registration_t;

typedef struct sip_registration_config {
    struct sip_agent_t* agent;
    const struct sip_transport_t* transport;
    void* transport_context;
    const char* address_of_record;
    const char* registrar;
    const char* contact;   /* Optional Contact field value. NULL uses the transport address. */
    const char* username;  /* Both credentials may be NULL for unauthenticated registrars. */
    const char* password;
    unsigned expires_seconds;
} sip_registration_config_t;

typedef enum sip_registration_state {
    SIP_REGISTRATION_PENDING,
    SIP_REGISTRATION_ACTIVE,
    SIP_REGISTRATION_FAILED,
    SIP_REGISTRATION_INACTIVE
} sip_registration_state_t;

typedef struct sip_registration_status {
    sip_registration_state_t state;
    int sip_code;            /* Last final SIP response, 408 on transaction timeout, or 0. */
    int error;               /* Negative local error or 0. */
    uint64_t expires_at_ms;  /* Conservative deadline for the last confirmed binding, or 0. */
    uint64_t refresh_at_ms;  /* Next scheduled refresh, or 0 while processing a REGISTER. */
} sip_registration_status_t;

/* Copies configuration strings and the transport callbacks. Keep the agent and
 * transport context valid until destroy returns. Receive on the selected peer
 * and deliver replies to the agent. Transport callbacks must not reenter the agent. */
sip_registration_t* sip_registration_create(const sip_registration_config_t* config);

/* Drive initial registration, challenge retries and refresh from one owner.
 * now_ms comes from a nondecreasing monotonic clock. Returns 0 on a valid
 * call with the current status, or -EINVAL for invalid arguments/time.
 * On SIP rejection or a local error the state is terminal. */
int sip_registration_poll(sip_registration_t* registration, uint64_t now_ms,
                          sip_registration_status_t* status);

/* Schedule an authenticated REGISTER with Expires: 0 on the next owner poll.
 * The state becomes INACTIVE after a 2xx, or FAILED on rejection. */
int sip_registration_unregister(sip_registration_t* registration);

/* Stop local registration and drain its active transaction's transport usage.
 * The registrar binding expires at its negotiated deadline. NULL is accepted. */
void sip_registration_destroy(sip_registration_t* registration);

#ifdef __cplusplus
}
#endif

#endif
