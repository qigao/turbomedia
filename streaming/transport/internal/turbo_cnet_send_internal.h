#ifndef TURBO_CNET_SEND_INTERNAL_H
#define TURBO_CNET_SEND_INTERNAL_H

#include <stddef.h>
#include <cnet/cnet.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Internal copy-send bridge for Salts CNet stream connections.
 *
 * data is borrowed for the duration of this call. The helper copies it into a
 * Salts-owned mem_buffer_t before calling the buffer send API, then releases
 * the caller's buffer reference after CNet has accepted or rejected the send.
 *
 * Completion/polling remains the caller's responsibility.
 */
int turbo_media_cnet_send_copy(
    cnet_client *client,
    cnet_connection connection,
    const void *data,
    size_t size,
    int close_after);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_CNET_SEND_INTERNAL_H */
