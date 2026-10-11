#include "gmssl_dtls.h"
#include "dtls_test_identity.h"
#include <tinytest.hpp>
#include <cmeta_crypto.h>
#include <cstring>

namespace {
uint64_t clock_ms;
bool contains_finished_record(const void *data, size_t size) {
    const auto *p = static_cast<const uint8_t *>(data);
    while (size >= 13) {
        size_t record_size = 13 + (static_cast<size_t>(p[11]) << 8) + p[12];
        if (record_size > size) return false;
        if (p[0] == 22 && p[3] == 0 && p[4] == 1) return true;
        p += record_size; size -= record_size;
    }
    return false;
}

struct Fixture {
    turbo_gdtls_context *reference_context = nullptr, *context = nullptr;
    turbo_gdtls *reference = nullptr, *session = nullptr;
    tstr certificate = nullptr, private_key = nullptr;
    tstr reference_certificate = nullptr, reference_private_key = nullptr;
    tstr owned_packet = nullptr, held_packet = nullptr, reference_held_packet = nullptr;
    bool server = false;
    bool drop_first = false, drop_final = false, reorder = false, dropped = false;
    bool reorder_reference = false, early_application = false, early_sent = false;
    int gm_error = 0, reference_error = 0;
    uint8_t reference_plain[16384]{};
    size_t reference_plain_size = 0;

    void clear() {
        turbo_gdtls_destroy(session); session = nullptr;
        turbo_gdtls_destroy(reference); reference = nullptr;
        turbo_gdtls_context_destroy(context); context = nullptr;
        turbo_gdtls_context_destroy(reference_context); reference_context = nullptr;
        if (private_key) cmeta_crypto_clear(private_key, tstr_len(private_key));
        if (reference_private_key) cmeta_crypto_clear(reference_private_key, tstr_len(reference_private_key));
        tstr_free(private_key); private_key = nullptr;
        tstr_free(reference_private_key); reference_private_key = nullptr;
        tstr_free(certificate); certificate = nullptr;
        tstr_free(reference_certificate); reference_certificate = nullptr;
        tstr_free(owned_packet); owned_packet = nullptr;
        tstr_free(held_packet); held_packet = nullptr;
        tstr_free(reference_held_packet); reference_held_packet = nullptr;
    }
    bool init(bool server_role, uint16_t profile, bool aes256 = false, bool wrong_pin = false,
              bool engine_rsa = false, bool reference_rsa = false, unsigned reference_bits = 0) {
        server = server_role; clock_ms = 1000000;
        if (dtls_test_identity(reference_rsa, &reference_certificate, &reference_private_key) ||
            dtls_test_identity(engine_rsa, &certificate, &private_key)) return false;
        reference_context = turbo_gdtls_context_create(reference_certificate, tstr_len(reference_certificate),
            reference_private_key, tstr_len(reference_private_key));
        context = turbo_gdtls_context_create(certificate, tstr_len(certificate), private_key, tstr_len(private_key));
        if (!context || !reference_context) return false;
        uint8_t expected[32];
        if (turbo_gdtls_context_fingerprint(reference_context, expected)) return false;
        if (wrong_pin) expected[0] ^= 1;
        session = turbo_gdtls_create(context, server ? 1 : 0, 300, expected, &profile, 1, aes256 ? 256 : 128);
        if (turbo_gdtls_context_fingerprint(context, expected)) return false;
        reference = turbo_gdtls_create(reference_context, server ? 0 : 1, 300, expected, &profile, 1,
            reference_bits ? reference_bits : aes256 ? 256 : 128);
        return session && reference && turbo_gdtls_start(session, clock_ms) == 0 &&
            turbo_gdtls_start(reference, clock_ms) == 0;
    }
    bool transfer_to_reference(tstr packet) {
        reference_error = turbo_gdtls_receive(reference, packet, tstr_len(packet), clock_ms);
        return reference_error == 0 || reference_error == TURBO_GDTLS_CLOSED;
    }
    bool step() {
        clock_ms += 20;
        gm_error = turbo_gdtls_poll(session, clock_ms);
        if (gm_error) return false;
        while ((owned_packet = turbo_gdtls_take_datagram(session)) != nullptr) {
            bool encrypted_handshake = contains_finished_record(owned_packet, tstr_len(owned_packet));
            if (!dropped && (drop_first || (drop_final && server && encrypted_handshake))) dropped = true;
            else if (reorder && !held_packet) { held_packet = owned_packet; owned_packet = nullptr; }
            else {
                if (!transfer_to_reference(owned_packet)) return false;
                if (held_packet) {
                    if (!transfer_to_reference(held_packet)) return false;
                    tstr_free(held_packet); held_packet = nullptr;
                }
            }
            tstr_free(owned_packet); owned_packet = nullptr;
        }
        if (held_packet) {
            if (!transfer_to_reference(held_packet)) return false;
            tstr_free(held_packet); held_packet = nullptr;
        }
        reference_error = turbo_gdtls_poll(reference, clock_ms);
        if (reference_error) return false;
        while ((owned_packet = turbo_gdtls_take_plaintext(reference)) != nullptr) {
            reference_plain_size = tstr_len(owned_packet);
            if (reference_plain_size > sizeof(reference_plain)) return false;
            memcpy(reference_plain, owned_packet, reference_plain_size);
            tstr_free(owned_packet); owned_packet = nullptr;
        }
        if (early_application && !early_sent && turbo_gdtls_ready(reference)) {
            if (turbo_gdtls_write(reference, "early", 5)) return false;
            early_sent = true;
        }
        while ((owned_packet = turbo_gdtls_take_datagram(reference)) != nullptr) {
            bool encrypted_handshake = contains_finished_record(owned_packet, tstr_len(owned_packet));
            if (!dropped && drop_final && !server && encrypted_handshake) dropped = true;
            else if (reorder_reference && !reference_held_packet) {
                reference_held_packet = owned_packet; owned_packet = nullptr;
            } else {
                gm_error = turbo_gdtls_receive(session, owned_packet, tstr_len(owned_packet), clock_ms);
                if (gm_error) return false;
                if (reference_held_packet) {
                    gm_error = turbo_gdtls_receive(session, reference_held_packet, tstr_len(reference_held_packet), clock_ms);
                    tstr_free(reference_held_packet); reference_held_packet = nullptr;
                    if (gm_error) return false;
                }
            }
            tstr_free(owned_packet); owned_packet = nullptr;
        }
        if (reference_held_packet) {
            gm_error = turbo_gdtls_receive(session, reference_held_packet, tstr_len(reference_held_packet), clock_ms);
            tstr_free(reference_held_packet); reference_held_packet = nullptr;
            if (gm_error) return false;
        }
        return true;
    }
    bool handshake() {
        for (unsigned i = 0; i < 2000; ++i) {
            if (!step()) return false;
            if (turbo_gdtls_ready(session) && turbo_gdtls_ready(reference)) return true;
        }
        return false;
    }
    bool capture_reference_record() {
        if (owned_packet) return false;
        constexpr char payload[] = "authenticated datagram";
        if (turbo_gdtls_write(reference, payload, sizeof(payload))) return false;
        owned_packet = turbo_gdtls_take_datagram(reference);
        return owned_packet != nullptr;
    }
    int receive_owned() {
        return turbo_gdtls_receive(session, owned_packet, tstr_len(owned_packet), clock_ms);
    }
};
Fixture fixture;
void check_pair(uint16_t profile) {
    bool connected = fixture.handshake();
    capture(fixture.gm_error, "%d"); capture(fixture.reference_error, "%d");
    check_true(connected);
    check_equal(turbo_gdtls_srtp_profile(fixture.session), profile);
    check_equal(turbo_gdtls_srtp_profile(fixture.reference), profile);
    uint8_t actual[88], expected[88]; size_t size = 0, expected_size = 0;
    check_equal(turbo_gdtls_export_srtp(fixture.session, actual, sizeof(actual), &size), 0);
    check_equal(turbo_gdtls_export_srtp(fixture.reference, expected, sizeof(expected), &expected_size), 0);
    check_equal(size, expected_size);
    check_equal(actual, expected, size);
    cmeta_crypto_clear(actual, sizeof(actual));
    cmeta_crypto_clear(expected, sizeof(expected));

    constexpr char payload[] = "one complete SCTP datagram";
    check_equal(turbo_gdtls_write(fixture.session, payload, sizeof(payload)), 0);
    check_true(fixture.step());
    check_equal(fixture.reference_plain_size, sizeof(payload));
    check_equal(fixture.reference_plain, payload, sizeof(payload));
    check_equal(turbo_gdtls_write(fixture.reference, payload, sizeof(payload)), 0);
    check_true(fixture.step());
    fixture.owned_packet = turbo_gdtls_take_plaintext(fixture.session);
    check_not_null(fixture.owned_packet);
    check_equal(tstr_len(fixture.owned_packet), sizeof(payload));
    check_equal(fixture.owned_packet, payload, sizeof(payload));
}
}

suite("GmSSL DTLS-SRTP paired protocol regression") {
    after_each() { fixture.clear(); fixture = Fixture{}; }
    for (int server = 0; server < 2; ++server) {
        it("role=%d rejects disjoint record cipher policies", server) {
            check_true(fixture.init(server != 0, 1, false, false, false, false, 256));
            check_false(fixture.handshake());
            check_equal(server ? fixture.gm_error : fixture.reference_error, TURBO_GDTLS_PROTOCOL);
            check_false(turbo_gdtls_ready(fixture.session));
            check_false(turbo_gdtls_ready(fixture.reference));
        }
        for (int engine_rsa = 0; engine_rsa < 2; ++engine_rsa) {
            for (int reference_rsa = 0; reference_rsa < 2; ++reference_rsa) {
                if (!engine_rsa && !reference_rsa) continue;
                it("role=%d engine RSA=%d reference RSA=%d, mutual identity and exporter", server,
                   engine_rsa, reference_rsa) {
                    check_true(fixture.init(server != 0, 8, true, false, engine_rsa != 0, reference_rsa != 0));
                    check_pair(8);
                }
            }
        }
        for (uint16_t profile : {uint16_t{1}, uint16_t{2}, uint16_t{7}, uint16_t{8}}) {
            for (int aes256 = 0; aes256 < 2; ++aes256) {
                it("role=%d profile=%u AES-%d, fragmented mutual handshake and payload", server,
                   static_cast<unsigned>(profile), aes256 ? 256 : 128) {
                    check_true(fixture.init(server != 0, profile, aes256 != 0));
                    check_pair(profile);
                }
            }
        }
        it("role=%d recovers ordinary first-flight datagram loss", server) {
            check_true(fixture.init(server != 0, 1));
            fixture.drop_first = true;
            check_pair(1); check_true(fixture.dropped);
        }
        it("role=%d recovers a lost final server Finished", server) {
            check_true(fixture.init(server != 0, 7));
            fixture.drop_final = true;
            check_pair(7); check_true(fixture.dropped);
        }
        it("role=%d reassembles reordered flight fragments", server) {
            check_true(fixture.init(server != 0, 8));
            fixture.reorder = true;
            check_pair(8);
        }
        it("role=%d reassembles independently reordered reference flight fragments", server) {
            check_true(fixture.init(server != 0, 8));
            fixture.reorder_reference = true;
            check_pair(8);
        }
        it("role=%d rejects an incorrect peer fingerprint before key export", server) {
            check_true(fixture.init(server != 0, 1, false, true));
            check_false(fixture.handshake());
            check_equal(fixture.gm_error, TURBO_GDTLS_AUTH);
            check_false(turbo_gdtls_ready(fixture.session));
            uint8_t keys[88]; size_t size = 123;
            check_equal(turbo_gdtls_export_srtp(fixture.session, keys, sizeof(keys), &size), TURBO_GDTLS_AUTH);
            check_equal(size, size_t{0});
        }
        it("role=%d discards damaged and repeated records without losing the association", server) {
            check_true(fixture.init(server != 0, 1));
            check_true(fixture.handshake());
            check_true(fixture.capture_reference_record());
            size_t last = tstr_len(fixture.owned_packet) - 1;
            fixture.owned_packet[last] ^= 1;
            check_equal(fixture.receive_owned(), 0);
            check_null(turbo_gdtls_take_plaintext(fixture.session));
            fixture.owned_packet[last] ^= 1;
            check_equal(fixture.receive_owned(), 0);
            fixture.held_packet = turbo_gdtls_take_plaintext(fixture.session);
            check_not_null(fixture.held_packet);
            check_equal(fixture.held_packet, "authenticated datagram");
            check_equal(fixture.receive_owned(), 0);
            check_null(turbo_gdtls_take_plaintext(fixture.session));
            check_true(turbo_gdtls_ready(fixture.session));
        }
        it("role=%d enforces export readiness and capacity without partial keys", server) {
            check_true(fixture.init(server != 0, 8));
            uint8_t keys[88]; memset(keys, 0xa5, sizeof(keys));
            size_t size = 123;
            check_equal(turbo_gdtls_export_srtp(fixture.session, keys, sizeof(keys), &size), TURBO_GDTLS_AUTH);
            check_equal(size, size_t{0});
            check_true(fixture.handshake());
            check_equal(turbo_gdtls_export_srtp(fixture.session, keys, sizeof(keys) - 1, &size), TURBO_GDTLS_CAPACITY);
            check_equal(size, size_t{0});
            for (uint8_t byte : keys) check_equal(byte, uint8_t{0xa5});
            check_equal(turbo_gdtls_export_srtp(fixture.session, keys, sizeof(keys), &size), 0);
            check_equal(size, sizeof(keys));
        }
        it("role=%d exposes the handshake lifetime even with no incoming flight", server) {
            check_true(fixture.init(server != 0, 1));
            check_equal(turbo_gdtls_deadline(fixture.session), clock_ms + (server ? 60000u : 1000u));
            clock_ms += 60000;
            check_equal(turbo_gdtls_poll(fixture.session, clock_ms), TURBO_GDTLS_TIMEOUT);
            check_equal(turbo_gdtls_deadline(fixture.session), uint64_t{0});
            check_null(turbo_gdtls_take_datagram(fixture.session));
            check_false(turbo_gdtls_ready(fixture.session));
        }
        it("role=%d cannot extend the handshake lifetime by receiving without polling", server) {
            check_true(fixture.init(server != 0, 1));
            clock_ms += 60000;
            check_equal(turbo_gdtls_receive(fixture.session, nullptr, 0, clock_ms), TURBO_GDTLS_TIMEOUT);
            check_equal(turbo_gdtls_deadline(fixture.session), uint64_t{0});
        }
        it("role=%d fails explicitly when the admitted output packet budget is exhausted", server) {
            check_true(fixture.init(server != 0, 1));
            check_true(fixture.handshake());
            for (unsigned i = 0; i < 256; ++i)
                check_equal(turbo_gdtls_write(fixture.session, "x", 1), 0);
            check_equal(turbo_gdtls_write(fixture.session, "x", 1), TURBO_GDTLS_CAPACITY);
            check_false(turbo_gdtls_ready(fixture.session));
            check_equal(turbo_gdtls_poll(fixture.session, clock_ms), TURBO_GDTLS_CAPACITY);
            check_null(turbo_gdtls_take_datagram(fixture.session));
        }
        it("role=%d drains local close_notify after revoking readiness and exporter access", server) {
            check_true(fixture.init(server != 0, 1));
            check_true(fixture.handshake());
            check_equal(turbo_gdtls_close(fixture.session), 0);
            check_false(turbo_gdtls_ready(fixture.session));
            check_equal(turbo_gdtls_close(fixture.session), TURBO_GDTLS_CLOSED);
            uint8_t keys[88]; size_t size = 123;
            check_equal(turbo_gdtls_export_srtp(fixture.session, keys, sizeof(keys), &size), TURBO_GDTLS_AUTH);
            check_equal(size, size_t{0});
            fixture.owned_packet = turbo_gdtls_take_datagram(fixture.session);
            check_not_null(fixture.owned_packet);
            check_true(fixture.transfer_to_reference(fixture.owned_packet));
            check_equal(fixture.reference_error, TURBO_GDTLS_CLOSED);
            check_false(turbo_gdtls_ready(fixture.reference));
        }
        it("role=%d closes on authenticated close_notify and returns the closing alert", server) {
            check_true(fixture.init(server != 0, 1));
            check_true(fixture.handshake());
            check_equal(turbo_gdtls_close(fixture.reference), 0);
            fixture.held_packet = turbo_gdtls_take_datagram(fixture.reference);
            check_not_null(fixture.held_packet);
            check_equal(turbo_gdtls_receive(fixture.session, fixture.held_packet,
                tstr_len(fixture.held_packet), clock_ms), TURBO_GDTLS_CLOSED);
            check_false(turbo_gdtls_ready(fixture.session));
            check_equal(turbo_gdtls_deadline(fixture.session), uint64_t{0});
            fixture.owned_packet = turbo_gdtls_take_datagram(fixture.session);
            check_not_null(fixture.owned_packet);
            check_true(fixture.transfer_to_reference(fixture.owned_packet));
            check_equal(fixture.reference_error, TURBO_GDTLS_CLOSED);
            check_equal(turbo_gdtls_poll(fixture.session, clock_ms), TURBO_GDTLS_CLOSED);
            check_equal(turbo_gdtls_write(fixture.session, "x", 1), TURBO_GDTLS_CLOSED);
        }
        it("role=%d preserves authenticated trailing data coalesced with close_notify", server) {
            check_true(fixture.init(server != 0, 1));
            check_true(fixture.handshake());
            check_true(fixture.capture_reference_record());
            check_equal(turbo_gdtls_close(fixture.reference), 0);
            fixture.reference_held_packet = turbo_gdtls_take_datagram(fixture.reference);
            check_not_null(fixture.reference_held_packet);
            size_t data_size = tstr_len(fixture.owned_packet);
            size_t close_size = tstr_len(fixture.reference_held_packet);
            fixture.held_packet = tstr_new_len(nullptr, data_size + close_size);
            check_not_null(fixture.held_packet);
            memcpy(fixture.held_packet, fixture.owned_packet, data_size);
            memcpy(fixture.held_packet + data_size, fixture.reference_held_packet, close_size);
            check_equal(turbo_gdtls_receive(fixture.session, fixture.held_packet,
                tstr_len(fixture.held_packet), clock_ms), TURBO_GDTLS_CLOSED);
            check_false(turbo_gdtls_ready(fixture.session));
            tstr_free(fixture.owned_packet);
            fixture.owned_packet = turbo_gdtls_take_plaintext(fixture.session);
            check_not_null(fixture.owned_packet);
            check_equal(fixture.owned_packet, "authenticated datagram");
            check_null(turbo_gdtls_take_plaintext(fixture.session));
            check_equal(turbo_gdtls_poll(fixture.session, clock_ms), TURBO_GDTLS_CLOSED);
        }
    }
    it("accepts Finished when UDP delivers authenticated application data ahead of it") {
        check_true(fixture.init(false, 1));
        fixture.reorder_reference = true;
        fixture.early_application = true;
        check_pair(1);
        check_true(fixture.early_sent);
    }
}

suite("GmSSL DTLS-SRTP paired owner contracts") {
    static turbo_gdtls_context *contexts[2];
    static turbo_gdtls *sessions[2];
    static tstr certificate, key, packet, repeated_finished;
    before_each() {
        clock_ms = 1000000;
        for (auto &context : contexts) {
            check_equal(turbo_dc_generate_certificate_der(&certificate, &key), 0);
            context = turbo_gdtls_context_create(certificate, tstr_len(certificate), key, tstr_len(key));
            cmeta_crypto_clear(key, tstr_len(key));
            tstr_free(key); key = nullptr;
            tstr_free(certificate); certificate = nullptr;
            check_not_null(context);
        }
    }
    after_each() {
        for (auto &session : sessions) { turbo_gdtls_destroy(session); session = nullptr; }
        for (auto &context : contexts) { turbo_gdtls_context_destroy(context); context = nullptr; }
        if (key) cmeta_crypto_clear(key, tstr_len(key));
        tstr_free(key); key = nullptr;
        tstr_free(certificate); certificate = nullptr;
        tstr_free(packet); packet = nullptr;
        tstr_free(repeated_finished); repeated_finished = nullptr;
    }
    it("recovers the first final-flight retry despite transit delay and limits later replays") {
        constexpr uint16_t profile = 1;
        for (int i = 0; i < 2; ++i) {
            uint8_t pin[32];
            check_equal(turbo_gdtls_context_fingerprint(contexts[1 - i], pin), 0);
            sessions[i] = turbo_gdtls_create(contexts[i], i, 300, pin, &profile, 1, 0);
            check_not_null(sessions[i]);
            check_equal(turbo_gdtls_start(sessions[i], clock_ms), 0);
        }
        uint64_t client_retry = 0, server_ready_at = 0;
        for (unsigned step = 0; step < 20 && !turbo_gdtls_ready(sessions[1]); ++step) {
            clock_ms += 20;
            while ((packet = turbo_gdtls_take_datagram(sessions[0])) != nullptr) {
                if (static_cast<uint8_t>(packet[0]) == 22 && packet[4] == 1) {
                    client_retry = turbo_gdtls_deadline(sessions[0]);
                    clock_ms += 50; // Client's RTO is already running in transit.
                }
                check_equal(turbo_gdtls_receive(sessions[1], packet, tstr_len(packet), clock_ms), 0);
                tstr_free(packet); packet = nullptr;
            }
            if (turbo_gdtls_ready(sessions[1])) server_ready_at = clock_ms;
            while ((packet = turbo_gdtls_take_datagram(sessions[1])) != nullptr) {
                // Drop the entire final server flight, including CCS.
                if (!server_ready_at)
                    check_equal(turbo_gdtls_receive(sessions[0], packet, tstr_len(packet), clock_ms), 0);
                tstr_free(packet); packet = nullptr;
            }
        }
        check_true(turbo_gdtls_ready(sessions[1]));
        check_false(turbo_gdtls_ready(sessions[0]));
        check_greater(client_retry, server_ready_at);
        check_less(client_retry - server_ready_at, uint64_t{1000});
        clock_ms = client_retry;
        check_equal(turbo_gdtls_poll(sessions[0], clock_ms), 0);
        while ((packet = turbo_gdtls_take_datagram(sessions[0])) != nullptr) {
            if (static_cast<uint8_t>(packet[0]) == 22 && packet[4] == 1) {
                repeated_finished = tstr_new_len(packet, tstr_len(packet));
                check_not_null(repeated_finished);
            }
            check_equal(turbo_gdtls_receive(sessions[1], packet, tstr_len(packet), clock_ms), 0);
            tstr_free(packet); packet = nullptr;
        }
        check_not_null(repeated_finished);
        unsigned replies = 0;
        while ((packet = turbo_gdtls_take_datagram(sessions[1])) != nullptr) {
            ++replies;
            check_equal(turbo_gdtls_receive(sessions[0], packet, tstr_len(packet), clock_ms), 0);
            tstr_free(packet); packet = nullptr;
        }
        check_greater(replies, 0u);
        check_true(turbo_gdtls_ready(sessions[0]));
        for (unsigned i = 0; i < 10; ++i)
            check_equal(turbo_gdtls_receive(sessions[1], repeated_finished,
                tstr_len(repeated_finished), clock_ms), 0);
        check_null(turbo_gdtls_take_datagram(sessions[1]));
        clock_ms += 999;
        check_equal(turbo_gdtls_receive(sessions[1], repeated_finished,
            tstr_len(repeated_finished), clock_ms), 0);
        check_null(turbo_gdtls_take_datagram(sessions[1]));
        ++clock_ms;
        check_equal(turbo_gdtls_receive(sessions[1], repeated_finished,
            tstr_len(repeated_finished), clock_ms), 0);
        packet = turbo_gdtls_take_datagram(sessions[1]);
        check_not_null(packet);
    }
    it("negotiates server preference, exports matching keys and retires the retained final flight") {
        constexpr uint16_t profiles[2][4] = {{1, 2, 7, 8}, {8, 7, 2, 1}};
        for (int i = 0; i < 2; ++i) {
            uint8_t pin[32];
            check_equal(turbo_gdtls_context_fingerprint(contexts[1 - i], pin), 0);
            sessions[i] = turbo_gdtls_create(contexts[i], i, 300, pin, profiles[i], 4, 0);
            check_not_null(sessions[i]);
            check_equal(turbo_gdtls_start(sessions[i], clock_ms), 0);
        }
        for (unsigned step = 0; step < 2000; ++step) {
            clock_ms += 20;
            for (int sender = 0; sender < 2; ++sender) {
                check_equal(turbo_gdtls_poll(sessions[sender], clock_ms), 0);
                while ((packet = turbo_gdtls_take_datagram(sessions[sender])) != nullptr) {
                    check_equal(turbo_gdtls_receive(sessions[1 - sender], packet, tstr_len(packet), clock_ms), 0);
                    tstr_free(packet); packet = nullptr;
                }
            }
            if (turbo_gdtls_ready(sessions[0]) && turbo_gdtls_ready(sessions[1])) break;
        }
        uint8_t keys[2][88]; size_t sizes[2];
        for (int i = 0; i < 2; ++i) {
            check_true(turbo_gdtls_ready(sessions[i]));
            check_equal(turbo_gdtls_srtp_profile(sessions[i]), uint16_t{8});
            check_equal(turbo_gdtls_export_srtp(sessions[i], keys[i], sizeof(keys[i]), &sizes[i]), 0);
            check_equal(sizes[i], sizeof(keys[i]));
        }
        check_equal(keys[0], keys[1], sizeof(keys[0]));
        check_equal(turbo_gdtls_deadline(sessions[0]), uint64_t{0});
        uint64_t retires = turbo_gdtls_deadline(sessions[1]);
        check_greater(retires, clock_ms);
        check_equal(turbo_gdtls_poll(sessions[1], retires), 0);
        check_equal(turbo_gdtls_deadline(sessions[1]), uint64_t{0});
        check_null(turbo_gdtls_take_datagram(sessions[1]));
        check_true(turbo_gdtls_ready(sessions[1]));
        check_equal(turbo_gdtls_export_srtp(sessions[1], keys[1], sizeof(keys[1]), &sizes[1]), 0);
        check_equal(keys[0], keys[1], sizeof(keys[0]));
    }
}
