#include "sctp_c11_contract.h"

#include <limits.h>
#include <string.h>

/* RFC 8831 encodes an empty message using PPID 56/57 and a single zero octet.
 * Static storage survives a synchronous borrowed send-admission call.
 * The engine must separately own queued bytes after that call returns. */
static const uint8_t tm_sc_empty_wire_octet = 0u;

static tm_sc_status tm_sc_reset_send(tm_sc_send_v1 *out) {
    if (!out) return TM_SC_EINVAL;
    if (out->version != TM_SC_ABI_V1 || out->size != sizeof(*out))
        return TM_SC_EVERSION;
    *out = (tm_sc_send_v1)TM_SC_SEND_V1_INIT;
    return TM_SC_OK;
}

static tm_sc_status tm_sc_reset_receive(tm_sc_receive_v1 *out) {
    if (!out) return TM_SC_EINVAL;
    if (out->version != TM_SC_ABI_V1 || out->size != sizeof(*out))
        return TM_SC_EVERSION;
    *out = (tm_sc_receive_v1)TM_SC_RECEIVE_V1_INIT;
    return TM_SC_OK;
}

tm_sc_status tm_sc_make_user_send(const turbo_dc_channel_config_t *config,
                                  uint64_t peer_generation, uint16_t sid,
                                  const void *data, size_t app_bytes,
                                  int is_binary, size_t budget,
                                  tm_sc_send_v1 *out) {
    tm_sc_status rc = tm_sc_reset_send(out);
    size_t wire_bytes;
    if (rc != TM_SC_OK) return rc;
    /* Preserve existing turbo_dc_channel_send's non-null data requirement,
     * even when the caller requests an empty application message. */
    if (!config || !data || !peer_generation || !budget ||
        (is_binary != 0 && is_binary != 1) ||
        config->max_retransmits < 0 || config->max_lifetime_ms < 0 ||
        (config->max_retransmits > 0 && config->max_lifetime_ms > 0))
        return TM_SC_EINVAL;
    /* DCEP OPEN currently transports these values as uint16; reject values
     * which could be admitted for a message but cannot open its channel. */
    if ((unsigned int)config->max_retransmits > UINT16_MAX ||
        (unsigned int)config->max_lifetime_ms > UINT16_MAX)
        return TM_SC_EINVAL;
    wire_bytes = app_bytes ? app_bytes : 1u;
    if (wire_bytes > budget) return TM_SC_ECAPACITY;

    out->peer_generation = peer_generation;
    out->sid = sid;
    out->ppid = app_bytes
        ? (is_binary ? TM_SC_PPID_BINARY : TM_SC_PPID_STRING)
        : (is_binary ? TM_SC_PPID_BINARY_EMPTY : TM_SC_PPID_STRING_EMPTY);
    out->data = app_bytes ? data : &tm_sc_empty_wire_octet;
    out->wire_bytes = wire_bytes;
    out->app_bytes = app_bytes;
    out->unordered = config->ordered ? 0u : 1u;
    if (config->max_retransmits > 0) {
        out->policy = TM_SC_MAX_RETRANSMITS;
        out->policy_value = (uint32_t)config->max_retransmits;
    } else if (config->max_lifetime_ms > 0) {
        out->policy = TM_SC_MAX_LIFETIME_MS;
        out->policy_value = (uint32_t)config->max_lifetime_ms;
    } else {
        out->policy = TM_SC_RELIABLE;
    }
    return TM_SC_OK;
}

tm_sc_status tm_sc_make_dcep_send(uint64_t peer_generation, uint16_t sid,
                                  const void *dcep_bytes, size_t length,
                                  size_t budget, tm_sc_send_v1 *out) {
    const uint8_t *bytes = (const uint8_t *)dcep_bytes;
    tm_sc_status rc = tm_sc_reset_send(out);
    if (rc != TM_SC_OK) return rc;
    if (!peer_generation || !bytes || !length || !budget)
        return TM_SC_EINVAL;
    /* DCEP frame validation, channel-ID allocation and parity remain in
     * TurboMedia. This only checks the unambiguous OPEN/ACK envelope. */
    if (!((bytes[0] == 0x02u && length == 1u) ||
          (bytes[0] == 0x03u && length >= 12u)))
        return TM_SC_EINVAL;
    if (length > budget) return TM_SC_ECAPACITY;
    out->peer_generation = peer_generation;
    out->sid = sid;
    out->ppid = TM_SC_PPID_DCEP;
    out->data = bytes;
    out->wire_bytes = length;
    out->app_bytes = 0u;
    out->unordered = 0u;
    out->policy = TM_SC_RELIABLE;
    return TM_SC_OK;
}

tm_sc_status tm_sc_classify_received(uint64_t peer_generation, uint16_t sid,
                                     uint32_t ppid, const void *wire_data,
                                     size_t wire_bytes, tm_sc_receive_v1 *out) {
    const uint8_t *bytes = (const uint8_t *)wire_data;
    tm_sc_status rc = tm_sc_reset_receive(out);
    if (rc != TM_SC_OK) return rc;
    if (!peer_generation || !bytes || !wire_bytes)
        return TM_SC_EINVAL;
    switch (ppid) {
    case TM_SC_PPID_DCEP:
        out->kind = TM_SC_DCEP;
        break;
    case TM_SC_PPID_STRING:
        out->kind = TM_SC_TEXT;
        break;
    case TM_SC_PPID_BINARY:
        out->kind = TM_SC_BINARY;
        break;
    case TM_SC_PPID_STRING_EMPTY:
    case TM_SC_PPID_BINARY_EMPTY:
        if (wire_bytes != 1u || bytes[0] != 0u)
            return TM_SC_EINVAL;
        out->kind = (ppid == TM_SC_PPID_BINARY_EMPTY)
            ? TM_SC_BINARY : TM_SC_TEXT;
        break;
    default:
        return TM_SC_EUNSUPPORTED;
    }
    out->peer_generation = peer_generation;
    out->sid = sid;
    out->ppid = ppid;
    if (ppid != TM_SC_PPID_STRING_EMPTY &&
        ppid != TM_SC_PPID_BINARY_EMPTY) {
        out->data = wire_data;
        out->app_bytes = wire_bytes;
    }
    return TM_SC_OK;
}
