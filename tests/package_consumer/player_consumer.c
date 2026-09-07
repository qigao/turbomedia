#include <salts_playback.h>
#include <turbo_player.h>

int main(void) {
    salts_playback_device_t device = {0};
    turbo_player_config_t config = {0};

    config.audio_device = &device;
    return config.audio_device == &device &&
                   turbo_player_get_duration_ms(NULL) == 0
               ? 0
               : 1;
}
