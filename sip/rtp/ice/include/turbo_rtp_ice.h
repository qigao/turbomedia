#ifndef TURBO_RTP_ICE_H
#define TURBO_RTP_ICE_H

#include <ice/salts_ice.h>
#include <turbo_sdp.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_rtp_ice turbo_rtp_ice_t;

typedef enum {
    TURBO_RTP_ICE_RTP,
    TURBO_RTP_ICE_RTCP
} turbo_rtp_ice_packet_t;

enum { TURBO_RTP_ICE_MAX_PACKET_BYTES = 2048 };

/* Called synchronously during poll. The packet is borrowed until return.
 * Callbacks may observe application state and must not reenter this transport. */
typedef void (*turbo_rtp_ice_receive_fn)(void *context, turbo_rtp_ice_packet_t kind,
                                        const void *data, size_t size);

/* Create a caller-owned full ICE transport for one UDP RTP/RTCP-mux session.
 * Use ice_default_config() for defaults. Set the ICE role before creating the
 * transport. The configuration is copied. Keep the callback context live until
 * destroy succeeds. All operations run on one media owner thread. */
turbo_rtp_ice_t *turbo_rtp_ice_create(const ice_config_t *config,
                                      turbo_rtp_ice_receive_fn on_receive, void *context);

/* Gather a complete, non-trickle candidate set. This is synchronous and uses the
 * SaltsNet gathering and server timeouts. Returns 0 or a negative status. */
int turbo_rtp_ice_gather(turbo_rtp_ice_t *transport);

/* Write gathered credentials, candidates, default endpoint and rtcp-mux to an
 * initialized local audio/video media description. Keep the codec/protocol
 * negotiated by the application. Returns 0 or a negative status. */
int turbo_rtp_ice_write_local(turbo_rtp_ice_t *transport, sdp_media_t *media);

/* Apply one remote audio/video media description after offer/answer. It must
 * accept rtcp-mux and provide a usable UDP component-1 candidate. The operation
 * copies all inputs. Returns 0 or a negative status. If the ICE agent rejects a
 * candidate after admission begins, destroy this transport and create a new one. */
int turbo_rtp_ice_set_remote(turbo_rtp_ice_t *transport, const sdp_media_t *media);

/* Run synchronous connectivity checks. The peer must progress its checks
 * concurrently. The configured connectivity timeout bounds the ICE checks. */
int turbo_rtp_ice_connect(turbo_rtp_ice_t *transport);

/* Drive selected-path receive and consent on the owner after connect.
 * Returns 0 while connected, or a negative status. Zero timeout polls once. */
int turbo_rtp_ice_poll(turbo_rtp_ice_t *transport, uint32_t timeout_ms);

/* Send one RTP or RTCP datagram, borrowing bytes for this call only.
 * Success indicates local send completion. Returns 0 or a negative status. */
int turbo_rtp_ice_send(turbo_rtp_ice_t *transport, turbo_rtp_ice_packet_t kind,
                       const void *data, size_t size);

/* Copy both selected candidates for the connected path. */
int turbo_rtp_ice_get_selected_pair(turbo_rtp_ice_t *transport, ice_candidate_t *local,
                                    ice_candidate_t *remote);

/* Stop and drain with a total cleanup budget. Returns 0 and clears *transport
 * on success. A negative status retains the closing transport for a retry. */
int turbo_rtp_ice_destroy(turbo_rtp_ice_t **transport, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
