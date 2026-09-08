#include <turbo_rtsp.h>

#include <string.h>

int main(void) {
    turbo_rtsp_client_config_t config;

    memset(&config, 0, sizeof(config));
    config.control_transport = TURBO_RTSP_CONTROL_TRANSPORT_TCP;
    return config.control_transport == TURBO_RTSP_CONTROL_TRANSPORT_TCP ? 0 : 1;
}
