/*
 * TurboMedia #227 — PRIVATE, TEST-ONLY C11 SCTP adapter contract.
 * This is NOT a Salts SCTP API, an installed TurboMedia ABI, or a second engine.
 * The operations table is a compile-only review contract: no implementation
 * or runtime selection is supplied until upstream Salts #1181 is qualified.
 */
#ifndef TM_TEST_SCTP_C11_CONTRACT_H
#define TM_TEST_SCTP_C11_CONTRACT_H

#ifdef __cplusplus
#error "This isolated SCTP adapter contract is pure C11"
#endif

#include <stddef.h>
#include <stdint.h>
#include "turbo_datachannel.h"

#define TM_SC_ABI_V1 1u
#define TM_SC_PPID_DCEP 50u
#define TM_SC_PPID_STRING 51u
#define TM_SC_PPID_BINARY 53u
#define TM_SC_PPID_STRING_EMPTY 56u
#define TM_SC_PPID_BINARY_EMPTY 57u

typedef enum tm_sc_status {
    TM_SC_OK = 0,
    TM_SC_EINVAL = -1,
    TM_SC_EVERSION = -2,
    TM_SC_ECAPACITY = -3,
    TM_SC_EUNSUPPORTED = -4,
    TM_SC_ESTALE = -5,
    TM_SC_EBUSY = -6
} tm_sc_status;

typedef enum tm_sc_policy {
    TM_SC_RELIABLE = 0,
    TM_SC_MAX_RETRANSMITS = 1,
    TM_SC_MAX_LIFETIME_MS = 2
} tm_sc_policy;

typedef enum tm_sc_message_kind {
    TM_SC_DCEP = 1,
    TM_SC_TEXT = 2,
    TM_SC_BINARY = 3
} tm_sc_message_kind;

/* 'data' is borrowed only for the synchronous send admission call. A
 * successful engine admission must take an owned bounded copy or establish a
 * separately charged retained lease. A failure retains nothing.
 * 'wire_bytes' may differ from 'app_bytes' for RFC 8831 empty PPIDs. */
typedef struct tm_sc_send_v1 {
    uint32_t size;
    uint32_t version;
    uint64_t peer_generation;
    const void *data;
    size_t wire_bytes;
    size_t app_bytes;
    uint32_t ppid;
    uint32_t policy_value;
    uint16_t sid;
    uint8_t unordered;
    uint8_t policy;
} tm_sc_send_v1;

#define TM_SC_SEND_V1_INIT {sizeof(tm_sc_send_v1), TM_SC_ABI_V1, 0}

typedef struct tm_sc_receive_v1 {
    uint32_t size;
    uint32_t version;
    uint64_t peer_generation;
    const void *data; /* borrowed until the hosting Owner returns */
    size_t app_bytes;
    uint32_t ppid;
    uint16_t sid;
    uint8_t kind;
} tm_sc_receive_v1;

#define TM_SC_RECEIVE_V1_INIT {sizeof(tm_sc_receive_v1), TM_SC_ABI_V1, 0}

/* A proposal only: Salts #1182 presently exports *Wire*, not this session.
 * No source in the production DataChannel library references these types.
 * Every operation is Owner-serialized; external threads submit bounded
 * copied commands through existing CNet Owner admission, not through ops. */
typedef struct tm_sc_session tm_sc_session;

typedef struct tm_sc_session_config_v1 {
    uint32_t size;
    uint32_t version;
    uint64_t peer_generation;
    uint32_t mtu;
    uint32_t max_streams;
    size_t max_tx_bytes;
    size_t max_rx_bytes;
    size_t max_events;
} tm_sc_session_config_v1;

typedef struct tm_sc_event_v1 {
    uint32_t size;
    uint32_t version;
    uint64_t peer_generation;
    uint32_t kind;
    uint32_t ppid;
    uint16_t sid;
    const void *data; /* valid only during the Owner's current event */
    size_t data_bytes;
} tm_sc_event_v1;

typedef struct tm_sc_owner_ops_v1 {
    uint32_t size;
    uint32_t version;
    tm_sc_status (*create)(void *owner, const tm_sc_session_config_v1 *,
                           tm_sc_session **);
    tm_sc_status (*connect)(tm_sc_session *);
    tm_sc_status (*input)(tm_sc_session *, uint64_t peer_generation,
                          const void *borrowed_plaintext, size_t bytes);
    tm_sc_status (*send_message)(tm_sc_session *, const tm_sc_send_v1 *);
    tm_sc_status (*reset_stream)(tm_sc_session *, uint16_t sid);
    tm_sc_status (*advance)(tm_sc_session *, uint64_t monotonic_now_ms);
    tm_sc_status (*next_deadline)(tm_sc_session *, uint64_t *out_ms);
    tm_sc_status (*poll_event)(tm_sc_session *, tm_sc_event_v1 *);
    tm_sc_status (*pending_bytes)(tm_sc_session *, uint16_t sid,
                                  size_t *out_bytes);
    tm_sc_status (*stop)(tm_sc_session *);
    tm_sc_status (*drained)(tm_sc_session *, int *out_drained);
    tm_sc_status (*destroy)(tm_sc_session **);
} tm_sc_owner_ops_v1;

/* The three functions below ONLY validate/normalize the existing public
 * DataChannel semantics into a borrowed, bounded request or view. They never
 * call an engine or change production behavior. 'budget' is supplied by the
 * caller/Owner; no implicit queue or fallback is constructed here. */
tm_sc_status tm_sc_make_user_send(const turbo_dc_channel_config_t *config,
                                  uint64_t peer_generation, uint16_t sid,
                                  const void *data, size_t app_bytes,
                                  int is_binary, size_t budget,
                                  tm_sc_send_v1 *out);
tm_sc_status tm_sc_make_dcep_send(uint64_t peer_generation, uint16_t sid,
                                  const void *dcep_bytes, size_t length,
                                  size_t budget, tm_sc_send_v1 *out);
tm_sc_status tm_sc_classify_received(uint64_t peer_generation, uint16_t sid,
                                     uint32_t ppid, const void *wire_data,
                                     size_t wire_bytes, tm_sc_receive_v1 *out);

#endif /* TM_TEST_SCTP_C11_CONTRACT_H */
