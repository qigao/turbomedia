#include "salts_ice_owner.h"
#include "tinytest.h"

#include <stdint.h>
#include <string.h>

suite("SaltsNet ICE owner") {
  group("lifecycle") {
    it("creates credentials and reaches terminal close") {
      ice_config_t config = ice_default_config();
      turbo_ice_owner_t *owner = turbo_ice_owner_create(&config, NULL);
      char ufrag[32] = {0};
      char pwd[64] = {0};

      check_not_null(owner);
      check_equal(turbo_ice_owner_get_state(owner), ICE_STATE_NEW);
      check_equal(
          turbo_ice_owner_get_local_credentials(
              owner, ufrag, sizeof(ufrag), pwd, sizeof(pwd)),
          0);
      check(strlen(ufrag) >= ICE_UFRAG_LEN);
      check(strlen(pwd) >= ICE_PWD_LEN);

      turbo_ice_owner_close(owner);
      check_equal(turbo_ice_owner_get_state(owner), ICE_STATE_CLOSED);
      check_equal(turbo_ice_owner_end_of_candidates(owner),
                  ICE_AGENT_ERROR_CLOSED);
      check_equal(turbo_ice_owner_set_allow_loopback(owner, 1),
                  ICE_AGENT_ERROR_CLOSED);
      check_equal(turbo_ice_owner_set_role(owner, 1),
                  ICE_AGENT_ERROR_CLOSED);
      turbo_ice_owner_destroy(owner);
    }

    it("executes restart and role changes on its owner thread") {
      ice_config_t config = ice_default_config();
      turbo_ice_owner_t *owner = turbo_ice_owner_create(&config, NULL);
      ice_restart_options_t options = ice_restart_options_default();
      char first_ufrag[32] = {0};
      char first_pwd[64] = {0};
      char next_ufrag[32] = {0};
      char next_pwd[64] = {0};

      check_not_null(owner);
      check_equal(turbo_ice_owner_set_role(owner, 0), 0);
      check_equal(turbo_ice_owner_set_allow_loopback(owner, 1), 0);
      check_equal(
          turbo_ice_owner_get_local_credentials(
              owner, first_ufrag, sizeof(first_ufrag),
              first_pwd, sizeof(first_pwd)),
          0);
      check_equal(turbo_ice_owner_restart(owner, &options), 0);
      check_equal(
          turbo_ice_owner_get_local_credentials(
              owner, next_ufrag, sizeof(next_ufrag),
              next_pwd, sizeof(next_pwd)),
          0);
      check_not_equal(first_ufrag, next_ufrag);
      check_not_equal(first_pwd, next_pwd);
      turbo_ice_owner_destroy(owner);
    }

    it("rejects an unsupported restart options version") {
      ice_config_t config = ice_default_config();
      turbo_ice_owner_t *owner = turbo_ice_owner_create(&config, NULL);
      ice_restart_options_t options = ice_restart_options_default();

      check_not_null(owner);
      options.version++;
      check_equal(
          turbo_ice_owner_restart(owner, &options),
          ICE_AGENT_ERROR_INVALID_OPTIONS);
      turbo_ice_owner_destroy(owner);
    }

    it("bounds asynchronous datagrams and rejects sends after close") {
      ice_config_t config = ice_default_config();
      turbo_ice_owner_t *owner = turbo_ice_owner_create(&config, NULL);
      uint8_t datagram[TURBO_ICE_OWNER_MAX_DATAGRAM_BYTES] = {0};
      uint8_t too_large[TURBO_ICE_OWNER_MAX_DATAGRAM_BYTES + 1] = {0};

      check_not_null(owner);
      check_equal(turbo_ice_owner_send_async(owner, NULL, 1), -1);
      check_equal(turbo_ice_owner_send_async(owner, datagram, 0), -1);
      check_equal(
          turbo_ice_owner_send_async(owner, too_large, sizeof(too_large)), -1);
      check_equal(
          turbo_ice_owner_send_async(owner, datagram, sizeof(datagram)), 0);

      turbo_ice_owner_close(owner);
      check_equal(turbo_ice_owner_send_async(owner, datagram, 1), -1);
      turbo_ice_owner_destroy(owner);
    }
  }
}
