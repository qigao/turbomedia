#ifndef TURBO_NETWORK_PORT_INTERNAL_H
#define TURBO_NETWORK_PORT_INTERNAL_H

#include <stdint.h>

/*
 * Socket sockaddr_in/sockaddr_in6 ports are two octets in network byte
 * order. Accessing their object representations as unsigned bytes is valid
 * in C11 and independent of host endianness and platform-specific feature
 * macros exposing ntohs. This internal helper does not own a socket or
 * introduce a platform compatibility path.
 */
static inline uint16_t turbo_media_socket_port_host(const uint16_t *network_port) {
    const unsigned char *octets=(const unsigned char *)(const void *)network_port;
    return (uint16_t)(((uint16_t)octets[0]<<8u)|(uint16_t)octets[1]);
}

#endif /* TURBO_NETWORK_PORT_INTERNAL_H */
