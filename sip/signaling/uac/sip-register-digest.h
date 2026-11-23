#ifndef TURBO_SIP_REGISTER_DIGEST_H
#define TURBO_SIP_REGISTER_DIGEST_H

#include "sip-message.h"

enum sip_digest_algorithm { SIP_DIGEST_NONE, SIP_DIGEST_MD5, SIP_DIGEST_SHA256 };

struct sip_register_digest {
    enum sip_digest_algorithm algorithm;
    char realm[256];
    char nonce[512];
    char opaque[256];
    uint32_t nonce_count;
    int qop;
    int stale;
};

int sip_register_digest_challenge(const struct sip_message_t* msg, const char* name,
                                  struct sip_register_digest* best);
int sip_digest_write(const struct sip_register_digest* digest, const char* username, vstr method,
                              const char* password, const char* uri, char* header, size_t capacity);

#endif
