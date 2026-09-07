#include <turbo_transport.h>

#include <stdlib.h>

int main(void) {
    turbo_transport_config_t config;
    int result = turbo_transport_parse_url("https://media.example/live", &config);

    if (result != 0 || config.port != 443) return 1;
    free((void *)config.host);
    free((void *)config.path);
    return 0;
}
