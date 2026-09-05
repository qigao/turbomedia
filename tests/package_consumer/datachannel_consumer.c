#include <turbo_dc_msg.h>

int main(void) {
    ltv_message_t *message = NULL;
    static const unsigned char wire[] = {1, 7};
    int result = turbo_dc_parse_ltv(wire, sizeof(wire), &message);

    if (result != LTV_PARSE_OK || !message || message->type != 7) return 1;
    turbo_dc_ltv_free(&message);
    return message == NULL ? 0 : 2;
}
