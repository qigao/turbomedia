#include "tinytest.h"
#include "../src/signaling/source_identity.h"

#include <stdio.h>
#include <string.h>

static void peer_from_key(
    const signaling_source_key_t *key, cnet_stream_peer *peer) {
  memset(peer, 0, sizeof(*peer));
  if (key->family == SIGNALING_SOURCE_FAMILY_IPV4) {
    peer->family = CNET_DATAGRAM_ADDRESS_IPV4;
    memcpy(peer->address, key->address, 4U);
  } else {
    peer->family = CNET_DATAGRAM_ADDRESS_IPV6;
    memcpy(peer->address, key->address, 16U);
    peer->scope_id = key->scope_id;
  }
}

void test_source_identity_ignores_spoofed_forwarding_from_untrusted_peer(void) {
  signaling_trusted_proxy_set_t proxies;
  signaling_source_key_t direct;
  signaling_source_key_t spoofed;
  signaling_source_key_t resolved;
  cnet_stream_peer peer;

  check_equal(
      signaling_trusted_proxy_set_parse(
          "10.0.0.10,2001:db8::10", &proxies), 0);
  check_equal(
      signaling_source_key_from_text("198.51.100.20", &direct), 0);
  check_equal(
      signaling_source_key_from_text("203.0.113.99", &spoofed), 0);
  peer_from_key(&direct, &peer);

  check_equal(
      signaling_source_identity_resolve(
          &proxies, &peer, "203.0.113.99", &resolved), 0);
  check_true(signaling_source_key_equal(&resolved, &direct));
  check_false(signaling_source_key_equal(&resolved, &spoofed));
}

void test_source_identity_accepts_only_one_ip_from_trusted_proxy(void) {
  signaling_trusted_proxy_set_t proxies;
  signaling_source_key_t proxy;
  signaling_source_key_t client;
  signaling_source_key_t resolved;
  cnet_stream_peer peer;

  check_equal(
      signaling_trusted_proxy_set_parse("10.0.0.10", &proxies), 0);
  check_equal(signaling_source_key_from_text("10.0.0.10", &proxy), 0);
  check_equal(
      signaling_source_key_from_text("203.0.113.7", &client), 0);
  peer_from_key(&proxy, &peer);

  check_equal(
      signaling_source_identity_resolve(
          &proxies, &peer, "203.0.113.7", &resolved), 0);
  check_true(signaling_source_key_equal(&resolved, &client));

  check_equal(
      signaling_source_identity_resolve(
          &proxies, &peer, NULL, &resolved), -1);
  check_equal(
      signaling_source_identity_resolve(
          &proxies, &peer,
          "203.0.113.7, 198.51.100.1", &resolved), -1);
  check_equal(
      signaling_source_identity_resolve(
          &proxies, &peer, "client.example", &resolved), -1);
  check_equal(
      signaling_source_identity_resolve(
          &proxies, &peer, "[2001:db8::1]", &resolved), -1);
  check_equal(
      signaling_source_identity_resolve(
          &proxies, &peer, "2001:db8::1%eth0", &resolved), -1);
}

void test_source_identity_allowlist_is_exact_normalized_and_bounded(void) {
  signaling_trusted_proxy_set_t proxies;
  signaling_source_key_t resolved;
  cnet_stream_peer mapped_peer = {0};
  uint8_t mapped[16] = {
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff,
      10, 0, 0, 10};
  char too_many[640];
  size_t used = 0U;

  check_equal(
      signaling_trusted_proxy_set_parse("10.0.0.10", &proxies), 0);
  mapped_peer.family = CNET_DATAGRAM_ADDRESS_IPV6;
  memcpy(mapped_peer.address, mapped, sizeof(mapped));
  check_equal(
      signaling_source_identity_resolve(
          &proxies, &mapped_peer, "2001:db8::55", &resolved), 0);
  check_equal((int)resolved.family, SIGNALING_SOURCE_FAMILY_IPV6);

  check_equal(
      signaling_trusted_proxy_set_parse(
          "10.0.0.10,10.0.0.10", &proxies), -1);
  check_equal(
      signaling_trusted_proxy_set_parse("10.0.0.10/32", &proxies), -1);

  memset(too_many, 0, sizeof(too_many));
  for (int index = 1; index <= SIGNALING_MAX_TRUSTED_PROXIES + 1;
       ++index) {
    int written = snprintf(
        too_many + used, sizeof(too_many) - used,
        "%s10.0.0.%d", index == 1 ? "" : ",", index);
    check_true(written > 0);
    used += (size_t)written;
  }
  check_equal(
      signaling_trusted_proxy_set_parse(too_many, &proxies), -1);
}

spec("test_signaling_source_identity") {
  it("test_source_identity_ignores_spoofed_forwarding_from_untrusted_peer") {
    test_source_identity_ignores_spoofed_forwarding_from_untrusted_peer();
  };
  it("test_source_identity_accepts_only_one_ip_from_trusted_proxy") {
    test_source_identity_accepts_only_one_ip_from_trusted_proxy();
  };
  it("test_source_identity_allowlist_is_exact_normalized_and_bounded") {
    test_source_identity_allowlist_is_exact_normalized_and_bounded();
  };
}
