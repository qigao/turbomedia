#include "signaling_server/server.h"
#include "capi/uscxml_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    signaling_server_t *server;
    uscxml_ccxml_t *interpreter;
} ccxml_adapter_t;

static void ccxml_on_accept(void *user_data) {
    ccxml_adapter_t *adapter = (ccxml_adapter_t *)user_data;
    printf("[CCXML] Accepting connection...\n");
    // TODO: Signal to SIP gateway
}

static void ccxml_on_disconnect(void *user_data) {
    ccxml_adapter_t *adapter = (ccxml_adapter_t *)user_data;
    printf("[CCXML] Disconnecting...\n");
}

static void ccxml_on_start_dialog(void *user_data, const char *src) {
    ccxml_adapter_t *adapter = (ccxml_adapter_t *)user_data;
    printf("[CCXML] Starting VoiceXML dialog: %s\n", src);
    // TODO: Trigger VXML adapter
}

ccxml_adapter_t *ccxml_adapter_create(signaling_server_t *server, const char *xml) {
    ccxml_adapter_t *adapter = (ccxml_adapter_t *)calloc(1, sizeof(*adapter));
    if (!adapter) {
        return NULL;
    }
    adapter->server = server;
    adapter->interpreter = uscxml_ccxml_from_xml(xml);
    
    if (!adapter->interpreter) {
        free(adapter);
        return NULL;
    }

    uscxml_ccxml_platform_t platform = {0};
    platform.user_data = adapter;
    platform.accept = ccxml_on_accept;
    platform.disconnect = ccxml_on_disconnect;
    platform.start_dialog = ccxml_on_start_dialog;
    
    uscxml_ccxml_set_platform(adapter->interpreter, platform);
    return adapter;
}

void ccxml_adapter_destroy(ccxml_adapter_t *adapter) {
    if (adapter) {
        uscxml_ccxml_destroy(adapter->interpreter);
        free(adapter);
    }
}

int ccxml_adapter_receive_event(ccxml_adapter_t *adapter, const char *event_json) {
    if (!adapter) return -1;
    return uscxml_ccxml_receive_event_json(adapter->interpreter, event_json);
}

void ccxml_adapter_step(ccxml_adapter_t *adapter) {
    if (adapter) {
        uscxml_ccxml_step(adapter->interpreter);
    }
}
