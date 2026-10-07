#include "tinytest.h"
#include "turbo_media_tenant_quota.h"
#include "turbo_media_tenant_quota_wire.h"

#include <stdlib.h>
#include <string.h>

static const char SNAPSHOT[] =
    "{"
    "\"schema_version\":1,"
    "\"epoch\":4294967296,"
    "\"sequence\":7,"
    "\"node_id\":\"node-a\","
    "\"leases\":["
      "{"
        "\"tenant_id\":\"tenant-a\","
        "\"expires_at_unix_ms\":9007199254740993,"
        "\"limits\":{"
          "\"signaling_connections\":10,"
          "\"rooms\":2,"
          "\"participants\":20,"
          "\"media_sessions\":8,"
          "\"published_tracks\":16"
        "}"
      "},"
      "{"
        "\"tenant_id\":\"tenant-b\","
        "\"expires_at_unix_ms\":9007199254740994,"
        "\"limits\":{"
          "\"signaling_connections\":3,"
          "\"rooms\":1,"
          "\"participants\":6,"
          "\"media_sessions\":2,"
          "\"published_tracks\":4"
        "}"
      "}"
    "]"
    "}";

static const char UPDATE[] =
    "{"
    "\"schema_version\":1,"
    "\"epoch\":4294967296,"
    "\"sequence\":8,"
    "\"node_id\":\"node-a\","
    "\"lease\":{"
      "\"tenant_id\":\"tenant-a\","
      "\"expires_at_unix_ms\":9007199254740995,"
      "\"limits\":{"
        "\"signaling_connections\":11,"
        "\"rooms\":3,"
        "\"participants\":22,"
        "\"media_sessions\":9,"
        "\"published_tracks\":18"
      "}"
    "}"
    "}";

void test_tenant_quota_wire_preserves_exact_versions_and_limits(void) {
    turbo_media_tenant_quota_wire_snapshot_t *snapshot =
        turbo_media_tenant_quota_wire_parse_snapshot(
            SNAPSHOT, strlen(SNAPSHOT));
    const turbo_media_tenant_quota_lease_t *leases;

    check_not_null(snapshot);
    check_equal(
        (uint64_t)turbo_media_tenant_quota_wire_snapshot_epoch(snapshot),
        UINT64_C(4294967296));
    check_equal(
        (uint64_t)turbo_media_tenant_quota_wire_snapshot_sequence(snapshot),
        UINT64_C(7));
    check_equal(
        turbo_media_tenant_quota_wire_snapshot_node_id(snapshot),
        "node-a");
    check_equal(
        (size_t)turbo_media_tenant_quota_wire_snapshot_count(snapshot),
        (size_t)2);

    leases = turbo_media_tenant_quota_wire_snapshot_leases(snapshot);
    check_not_null(leases);
    check_equal(leases[0].tenant_id, "tenant-a");
    check_equal(leases[0].node_id, "node-a");
    check_equal(
        (uint64_t)leases[0].expires_at_unix_ms,
        UINT64_C(9007199254740993));
    check_equal(
        (uint32_t)leases[0].limits[
            TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS],
        (uint32_t)10);
    check_equal(
        (uint32_t)leases[0].limits[TURBO_MEDIA_TENANT_QUOTA_ROOMS],
        (uint32_t)2);
    check_equal(
        (uint32_t)leases[1].limits[
            TURBO_MEDIA_TENANT_QUOTA_PUBLISHED_TRACKS],
        (uint32_t)4);

    turbo_media_tenant_quota_wire_snapshot_destroy(snapshot);
}

void test_tenant_quota_wire_parses_exact_next_update(void) {
    turbo_media_tenant_quota_wire_update_t update;

    check_equal(
        turbo_media_tenant_quota_wire_parse_update(
            UPDATE, strlen(UPDATE), &update),
        0);
    check_equal((uint64_t)update.epoch, UINT64_C(4294967296));
    check_equal((uint64_t)update.sequence, UINT64_C(8));
    check_equal(update.node_id, "node-a");
    check_equal(update.tenant_id, "tenant-a");
    check_equal(update.lease.node_id, "node-a");
    check_equal(update.lease.tenant_id, "tenant-a");
    check_equal(
        (uint64_t)update.lease.expires_at_unix_ms,
        UINT64_C(9007199254740995));
    check_equal(
        (uint32_t)update.lease.limits[
            TURBO_MEDIA_TENANT_QUOTA_MEDIA_SESSIONS],
        (uint32_t)9);
}

void test_tenant_quota_wire_rejects_structural_and_numeric_ambiguity(void) {
    static const char extra_key[] =
        "{"
        "\"schema_version\":1,\"epoch\":1,\"sequence\":0,"
        "\"node_id\":\"node-a\",\"leases\":[],\"extra\":true"
        "}";
    static const char fractional[] =
        "{"
        "\"schema_version\":1,\"epoch\":1.5,\"sequence\":0,"
        "\"node_id\":\"node-a\",\"leases\":[]"
        "}";
    static const char negative[] =
        "{"
        "\"schema_version\":1,\"epoch\":-1,\"sequence\":0,"
        "\"node_id\":\"node-a\",\"leases\":[]"
        "}";
    static const char nul_node[] =
        "{"
        "\"schema_version\":1,\"epoch\":1,\"sequence\":0,"
        "\"node_id\":\"node-a\\u0000evil\",\"leases\":[]"
        "}";
    static const char nul_tenant_update[] =
        "{"
        "\"schema_version\":1,\"epoch\":1,\"sequence\":1,"
        "\"node_id\":\"node-a\","
        "\"lease\":{"
          "\"tenant_id\":\"tenant-a\\u0000evil\","
          "\"expires_at_unix_ms\":1000,"
          "\"limits\":{"
            "\"signaling_connections\":1,\"rooms\":0,"
            "\"participants\":0,\"media_sessions\":0,"
            "\"published_tracks\":0"
          "}"
        "}"
        "}";
    static const char bad_limit[] =
        "{"
        "\"schema_version\":1,\"epoch\":1,\"sequence\":0,"
        "\"node_id\":\"node-a\","
        "\"leases\":[{"
          "\"tenant_id\":\"tenant-a\","
          "\"expires_at_unix_ms\":1000,"
          "\"limits\":{"
            "\"signaling_connections\":4294967296,"
            "\"rooms\":0,\"participants\":0,"
            "\"media_sessions\":0,\"published_tracks\":0"
          "}"
        "}]"
        "}";
    turbo_media_tenant_quota_wire_update_t update;

    check_null(turbo_media_tenant_quota_wire_parse_snapshot(
        extra_key, strlen(extra_key)));
    check_null(turbo_media_tenant_quota_wire_parse_snapshot(
        fractional, strlen(fractional)));
    check_null(turbo_media_tenant_quota_wire_parse_snapshot(
        negative, strlen(negative)));
    check_null(turbo_media_tenant_quota_wire_parse_snapshot(
        bad_limit, strlen(bad_limit)));
    check_null(turbo_media_tenant_quota_wire_parse_snapshot(
        nul_node, strlen(nul_node)));
    check_equal(
        turbo_media_tenant_quota_wire_parse_update(
            nul_tenant_update, strlen(nul_tenant_update), &update),
        -1);

    check_equal(
        turbo_media_tenant_quota_wire_parse_update(
            "{\"schema_version\":1,\"epoch\":1,\"sequence\":1,"
            "\"node_id\":\"node-a\","
            "\"lease\":{"
              "\"tenant_id\":\"\","
              "\"expires_at_unix_ms\":1000,"
              "\"limits\":{"
                "\"signaling_connections\":0,\"rooms\":0,"
                "\"participants\":0,\"media_sessions\":0,"
                "\"published_tracks\":0"
              "}"
            "}}",
            strlen(
                "{\"schema_version\":1,\"epoch\":1,\"sequence\":1,"
                "\"node_id\":\"node-a\","
                "\"lease\":{"
                  "\"tenant_id\":\"\","
                  "\"expires_at_unix_ms\":1000,"
                  "\"limits\":{"
                    "\"signaling_connections\":0,\"rooms\":0,"
                    "\"participants\":0,\"media_sessions\":0,"
                    "\"published_tracks\":0"
                  "}"
                "}}"),
            &update),
        -1);
}

void test_tenant_quota_wire_keeps_semantic_failure_in_projection(void) {
    static const char invalid_update[] =
        "{"
        "\"schema_version\":1,\"epoch\":1,\"sequence\":1,"
        "\"node_id\":\"node-a\","
        "\"lease\":{"
          "\"tenant_id\":\"tenant/a\","
          "\"expires_at_unix_ms\":5000,"
          "\"limits\":{"
            "\"signaling_connections\":1,\"rooms\":0,"
            "\"participants\":0,\"media_sessions\":0,"
            "\"published_tracks\":0"
          "}"
        "}"
        "}";
    turbo_media_tenant_quota_projection_t *projection =
        turbo_media_tenant_quota_projection_create("node-a", 4U);
    turbo_media_tenant_quota_wire_update_t update;
    int synchronized = 0;
    uint64_t epoch = 0U;
    uint64_t sequence = 0U;
    size_t count = 0U;

    check_not_null(projection);
    check_equal(
        (int)turbo_media_tenant_quota_apply_snapshot(
            projection, 1U, 0U, NULL, 0U),
        (int)TURBO_MEDIA_TENANT_QUOTA_APPLY_APPLIED);
    check_equal(
        turbo_media_tenant_quota_wire_parse_update(
            invalid_update, strlen(invalid_update), &update),
        0);
    check_equal(
        (int)turbo_media_tenant_quota_apply_update(
            projection, update.epoch, update.sequence, &update.lease),
        (int)TURBO_MEDIA_TENANT_QUOTA_APPLY_ERROR);
    check_equal(
        turbo_media_tenant_quota_status(
            projection, &synchronized, &epoch, &sequence, &count),
        0);
    check_false(synchronized);
    check_equal((uint64_t)epoch, UINT64_C(1));
    check_equal((uint64_t)sequence, UINT64_C(0));

    turbo_media_tenant_quota_projection_destroy(projection);
}

void test_tenant_quota_wire_enforces_body_bound(void) {
    size_t size = TURBO_MEDIA_TENANT_QUOTA_WIRE_MAX_BODY_BYTES + 1U;
    char *body = (char *)malloc(size);

    check_not_null(body);
    if (body) {
        memset(body, ' ', size);
        check_null(turbo_media_tenant_quota_wire_parse_snapshot(
            body, size));
        free(body);
    }
}

spec("test_tenant_quota_wire") {
    it("test_tenant_quota_wire_preserves_exact_versions_and_limits") {
        test_tenant_quota_wire_preserves_exact_versions_and_limits();
    };
    it("test_tenant_quota_wire_parses_exact_next_update") {
        test_tenant_quota_wire_parses_exact_next_update();
    };
    it("test_tenant_quota_wire_rejects_structural_and_numeric_ambiguity") {
        test_tenant_quota_wire_rejects_structural_and_numeric_ambiguity();
    };
    it("test_tenant_quota_wire_keeps_semantic_failure_in_projection") {
        test_tenant_quota_wire_keeps_semantic_failure_in_projection();
    };
    it("test_tenant_quota_wire_enforces_body_bound") {
        test_tenant_quota_wire_enforces_body_bound();
    };
}
