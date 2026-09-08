#ifndef TURBO_MEDIA_SALTS_ICE_OWNER_H
#define TURBO_MEDIA_SALTS_ICE_OWNER_H

#include <ice/salts_ice.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_ice_owner_s turbo_ice_owner_t;

enum {
    TURBO_ICE_OWNER_SEND_CAPACITY = 32,
    TURBO_ICE_OWNER_MAX_DATAGRAM_BYTES = 2048
};

turbo_ice_owner_t *turbo_ice_owner_create(
    const ice_config_t *config, const ice_callbacks_t *callbacks);
void turbo_ice_owner_close(turbo_ice_owner_t *owner);
void turbo_ice_owner_destroy(turbo_ice_owner_t *owner);

int turbo_ice_owner_get_local_credentials(
    turbo_ice_owner_t *owner, char *ufrag, size_t ufrag_len,
    char *pwd, size_t pwd_len);
int turbo_ice_owner_set_remote_credentials(
    turbo_ice_owner_t *owner, const char *ufrag, const char *pwd);
int turbo_ice_owner_restart(
    turbo_ice_owner_t *owner, const ice_restart_options_t *options);
int turbo_ice_owner_gather_candidates(turbo_ice_owner_t *owner);
int turbo_ice_owner_add_remote_candidate(
    turbo_ice_owner_t *owner, const char *candidate);
int turbo_ice_owner_end_of_candidates(turbo_ice_owner_t *owner);
int turbo_ice_owner_start_checks(turbo_ice_owner_t *owner);
int turbo_ice_owner_send(
    turbo_ice_owner_t *owner, const void *data, size_t len);
int turbo_ice_owner_send_async(
    turbo_ice_owner_t *owner, const void *data, size_t len);
ice_state_t turbo_ice_owner_get_state(turbo_ice_owner_t *owner);
ice_gathering_state_t turbo_ice_owner_get_gathering_state(
    turbo_ice_owner_t *owner);
int turbo_ice_owner_get_local_candidate_count(turbo_ice_owner_t *owner);
int turbo_ice_owner_get_local_candidate(
    turbo_ice_owner_t *owner, int index, ice_candidate_t *candidate);
int turbo_ice_owner_set_allow_loopback(turbo_ice_owner_t *owner, int allow);
int turbo_ice_owner_set_role(turbo_ice_owner_t *owner, int is_controlling);

#ifdef __cplusplus
}
#endif

#endif
