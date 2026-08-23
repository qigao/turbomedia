/**
 * SFU (Selective Forwarding Unit) API
 *
 * Multi-party video conferencing server
 */
#ifndef TURBO_SFU_H
#define TURBO_SFU_H

#include <turbo_export.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct sfu_context_t sfu_context_t;

/* =============================================================================
 * Statistics Structures
 * ============================================================================= */

typedef struct {
    int participant_count;
    int64_t total_packets_routed;
    int64_t total_bytes_routed;
    int64_t total_layer_switches;
} sfu_stats_t;

typedef struct {
    char participant_id[64];
    int stream_count;
    int available_bandwidth;
    int64_t packets_sent;
    int64_t bytes_sent;
    int64_t packets_received;
    int64_t bytes_received;
} sfu_participant_stats_t;

/* =============================================================================
 * SFU Management
 * ============================================================================= */

/**
 * Create SFU context
 *
 * @param max_participants Maximum number of participants (0 for default: 50)
 * @return SFU context or NULL on error
 */
TURBO_MEDIA_API sfu_context_t *sfu_create(int max_participants);

/**
 * Destroy SFU context
 */
TURBO_MEDIA_API void sfu_destroy(sfu_context_t *sfu);

/* =============================================================================
 * Participant Management
 * ============================================================================= */

/**
 * Add participant to conference
 *
 * @param sfu SFU context
 * @param participant_id Unique participant identifier
 * @return 0 on success, -1 on error
 */
TURBO_MEDIA_API int sfu_add_participant(sfu_context_t *sfu, const char *participant_id);

/**
 * Remove participant from conference
 *
 * @param sfu SFU context
 * @param participant_id Participant identifier
 * @return 0 on success, -1 on error
 */
TURBO_MEDIA_API int sfu_remove_participant(sfu_context_t *sfu, const char *participant_id);

/**
 * Set participant bandwidth
 *
 * Used for adaptive layer selection
 *
 * @param sfu SFU context
 * @param participant_id Participant identifier
 * @param bandwidth_bps Available bandwidth in bits per second
 */
TURBO_MEDIA_API void sfu_set_participant_bandwidth(sfu_context_t *sfu, const char *participant_id,
                                   int bandwidth_bps);

/**
 * Set per-receiver stream policy.
 *
 * This controls whether a given receiver is allowed to receive a sender stream
 * at all, and if so the highest simulcast layer the SFU may forward.
 *
 * @param sfu SFU context
 * @param receiver_id Receiver participant identifier
 * @param sender_id Sender participant identifier
 * @param stream_ssrc Main SSRC for the sender stream
 * @param enabled 1 to allow forwarding, 0 to block forwarding
 * @param max_layer Maximum allowed layer: 0=low, 1=medium, 2=high
 * @return 0 on success, -1 on error
 */
TURBO_MEDIA_API int sfu_set_receiver_stream_policy(sfu_context_t *sfu, const char *receiver_id,
                                             const char *sender_id, uint32_t stream_ssrc,
                                             int enabled, int max_layer);

/**
 * Set participant packet callback
 *
 * Called when packets should be sent to this participant
 *
 * @param sfu SFU context
 * @param participant_id Participant identifier
 * @param on_packet Callback function
 * @param user_data User data passed to callback
 */
TURBO_MEDIA_API void sfu_set_participant_callback(sfu_context_t *sfu, const char *participant_id,
                                  void (*on_packet)(void *user_data,
                                                   const uint8_t *packet, size_t len),
                                  void *user_data);

/**
 * Set participant keyframe request callback
 *
 * Called when the SFU needs this participant to generate a keyframe for a
 * specific SSRC, for example after a simulcast layer switch.
 *
 * @param sfu SFU context
 * @param participant_id Participant identifier
 * @param on_keyframe_request Callback function
 * @param user_data User data passed to callback
 */
TURBO_MEDIA_API void sfu_set_participant_keyframe_callback(
    sfu_context_t *sfu, const char *participant_id,
    void (*on_keyframe_request)(void *user_data, uint32_t ssrc),
    void *user_data);

/* =============================================================================
 * Stream Management
 * ============================================================================= */

/**
 * Add stream for participant
 *
 * @param sfu SFU context
 * @param participant_id Participant identifier
 * @param ssrc Main SSRC for the stream
 * @param layer_ssrcs Array of SSRCs for simulcast layers (can be NULL)
 * @param layer_count Number of simulcast layers (0 for no simulcast)
 * @return 0 on success, -1 on error
 */
TURBO_MEDIA_API int sfu_add_stream(sfu_context_t *sfu, const char *participant_id, uint32_t ssrc,
                   const uint32_t *layer_ssrcs, int layer_count);

/**
 * Remove stream for participant
 *
 * @param sfu SFU context
 * @param participant_id Participant identifier
 * @param ssrc Main SSRC for the stream
 * @return 0 on success, -1 on error
 */
TURBO_MEDIA_API int sfu_remove_stream(sfu_context_t *sfu, const char *participant_id, uint32_t ssrc);

/* =============================================================================
 * Packet Forwarding
 * ============================================================================= */

/**
 * Forward RTP packet from sender to all receivers
 *
 * Automatically selects appropriate simulcast layer per receiver
 *
 * @param sfu SFU context
 * @param sender_id Sender participant identifier
 * @param packet RTP packet data
 * @param len Packet length
 * @return Number of receivers forwarded to, or -1 on error
 */
TURBO_MEDIA_API int sfu_forward_packet(sfu_context_t *sfu, const char *sender_id,
                       const uint8_t *packet, size_t len);

/**
 * Forward NACK request from receiver to sender
 *
 * @param sfu SFU context
 * @param receiver_id Receiver participant identifier
 * @param sender_id Sender participant identifier
 * @param seq_nums Sequence numbers to NACK
 * @param count Number of sequence numbers
 * @return 0 on success, -1 on error
 */
TURBO_MEDIA_API int sfu_forward_nack(sfu_context_t *sfu, const char *receiver_id,
                     const char *sender_id, const uint16_t *seq_nums, int count);

/**
 * Request keyframe from participant
 *
 * @param sfu SFU context
 * @param participant_id Participant identifier
 * @param ssrc SSRC to request keyframe for
 * @return 0 on success, -1 on error
 */
TURBO_MEDIA_API int sfu_request_keyframe(sfu_context_t *sfu, const char *participant_id, uint32_t ssrc);

/* =============================================================================
 * Statistics
 * ============================================================================= */

/**
 * Get SFU statistics
 *
 * @param sfu SFU context
 * @param stats Output statistics structure
 */
TURBO_MEDIA_API void sfu_get_stats(sfu_context_t *sfu, sfu_stats_t *stats);

/**
 * Get participant statistics
 *
 * @param sfu SFU context
 * @param participant_id Participant identifier
 * @param stats Output statistics structure
 */
TURBO_MEDIA_API void sfu_get_participant_stats(sfu_context_t *sfu, const char *participant_id,
                               sfu_participant_stats_t *stats);

/**
 * Get participant count
 *
 * @param sfu SFU context
 * @return Number of active participants
 */
TURBO_MEDIA_API int sfu_get_participant_count(sfu_context_t *sfu);

/**
 * Get list of participant IDs
 *
 * @param sfu SFU context
 * @param participant_ids Output array of participant ID pointers
 * @param max_count Maximum number of IDs to return
 * @return Number of IDs returned
 */
TURBO_MEDIA_API int sfu_get_participant_list(sfu_context_t *sfu, char **participant_ids, int max_count);

/* =============================================================================
 * Configuration
 * ============================================================================= */

/**
 * Enable or disable simulcast support
 *
 * @param sfu SFU context
 * @param enabled 1 to enable, 0 to disable
 */
TURBO_MEDIA_API void sfu_set_simulcast_enabled(sfu_context_t *sfu, int enabled);

/**
 * Enable or disable bandwidth adaptation
 *
 * @param sfu SFU context
 * @param enabled 1 to enable, 0 to disable
 */
TURBO_MEDIA_API void sfu_set_bandwidth_adaptation_enabled(sfu_context_t *sfu, int enabled);

/**
 * Set SFU callbacks
 *
 * @param sfu SFU context
 * @param on_participant_joined Called when participant joins
 * @param on_participant_left Called when participant leaves
 * @param user_data User data passed to callbacks
 */
TURBO_MEDIA_API void sfu_set_callbacks(sfu_context_t *sfu,
                       void (*on_participant_joined)(void *user_data, const char *id),
                       void (*on_participant_left)(void *user_data, const char *id),
                       void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_SFU_H */
