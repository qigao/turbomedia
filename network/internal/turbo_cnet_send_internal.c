#include "turbo_cnet_send_internal.h"

#include <salts/error_codes.h>
#include <cmeta_buffer.h>

#include <string.h>

int turbo_media_cnet_send_copy(
    cnet_client *client,
    cnet_connection connection,
    const void *data,
    size_t size,
    int close_after) {
    mem_buffer_t *buffer;
    int status;

    if (client == NULL || data == NULL || size == 0u) {
        return SALTS_EINVAL;
    }

    buffer = mem_get_buffer(mem_global(), size);
    if (buffer == NULL) {
        return SALTS_ENOMEM;
    }

    memcpy(mem_buffer_data(buffer), data, size);
    mem_set_used(buffer, size);
    status = close_after
                 ? cnet_send_buffer_and_close(client, connection, buffer)
                 : cnet_send_buffer(client, connection, buffer);
    mem_buffer_release(buffer);
    return status;
}
