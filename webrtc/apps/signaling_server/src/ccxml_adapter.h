#ifndef SIGNALING_SERVER_CCXML_ADAPTER_H
#define SIGNALING_SERVER_CCXML_ADAPTER_H

#include "signaling_server/server.h"

typedef struct ccxml_adapter_s ccxml_adapter_t;

ccxml_adapter_t *ccxml_adapter_create(signaling_server_t *server, const char *xml);
void ccxml_adapter_destroy(ccxml_adapter_t *adapter);
int ccxml_adapter_receive_event(ccxml_adapter_t *adapter, const char *event_json);
void ccxml_adapter_step(ccxml_adapter_t *adapter);

#endif
