#ifndef TURBO_GMSSL_DTLS_H
#define TURBO_GMSSL_DTLS_H

#include <stddef.h>
#include <stdint.h>
#include <tstr.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Private, externally serialized DTLS 1.2 engine. Not an installed API.
 * The context owns its DER chain/key. Sessions borrow it until destroyed.
 * Inputs are borrowed for the call; take_* transfers a caller-owned tstr.
 * No callbacks or socket/thread operations occur inside the engine.
 * now_ms is a monotonic clock supplied by the existing transport owner. */
typedef struct turbo_gdtls_context turbo_gdtls_context;
typedef struct turbo_gdtls turbo_gdtls;

enum {
    TURBO_GDTLS_OK = 0,
    TURBO_GDTLS_INVALID = -1,
    TURBO_GDTLS_NOMEM = -2,
    TURBO_GDTLS_PROTOCOL = -3,
    TURBO_GDTLS_AUTH = -4,
    TURBO_GDTLS_CAPACITY = -5,
    TURBO_GDTLS_CLOSED = -6,
    TURBO_GDTLS_TIMEOUT = -7
};

/* DER chain is concatenated certificates, leaf first; key is PKCS#8 DER. */
turbo_gdtls_context *turbo_gdtls_context_create(const void *chain, size_t chain_len,
                                              const void *key, size_t key_len);
void turbo_gdtls_context_destroy(turbo_gdtls_context *ctx);
int turbo_gdtls_context_fingerprint(const turbo_gdtls_context *ctx, uint8_t out[32]);

/* Expected SHA-256 leaf fingerprint is mandatory. Profiles are RFC 5764/7714
 * IDs, in preference order, at most four; MTU is the complete UDP payload. */
turbo_gdtls *turbo_gdtls_create(const turbo_gdtls_context *ctx, int server,
                              size_t mtu, const uint8_t expected_sha256[32],
                              const uint16_t *profiles, size_t profile_count);
void turbo_gdtls_destroy(turbo_gdtls *session);
int turbo_gdtls_start(turbo_gdtls *session, uint64_t now_ms);
int turbo_gdtls_receive(turbo_gdtls *session, const void *packet, size_t size,
                       uint64_t now_ms);
int turbo_gdtls_poll(turbo_gdtls *session, uint64_t now_ms);
/* Next retransmission, handshake expiry or final-flight retirement, whichever
 * comes first. 0 means no pending timer, including before start/after close. */
uint64_t turbo_gdtls_deadline(const turbo_gdtls *session);
tstr turbo_gdtls_take_datagram(turbo_gdtls *session);
tstr turbo_gdtls_take_plaintext(turbo_gdtls *session);
int turbo_gdtls_ready(const turbo_gdtls *session);
int turbo_gdtls_write(turbo_gdtls *session, const void *data, size_t size);
/* Local close returns OK once, then CLOSED. Authenticated peer close returns
 * CLOSED from receive; in both cases drain take_datagram for close_notify.
 * Closing/failure revokes key export and clears secrets. Clean close preserves
 * already authenticated plaintext for take_plaintext; destroy clears leftovers.
 * Capacity failures during admitted processing are terminal and discard queues. */
int turbo_gdtls_close(turbo_gdtls *session);
uint16_t turbo_gdtls_srtp_profile(const turbo_gdtls *session);
int turbo_gdtls_export_srtp(const turbo_gdtls *session, uint8_t *out, size_t capacity,
                           size_t *size);

#ifdef __cplusplus
}
#endif
#endif
