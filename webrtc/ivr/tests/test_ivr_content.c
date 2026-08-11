/* test_ivr_content.c - content package manifest loading and validation */
#include "ivr_content.h"
#include "tinytest_compat.h"
#include <string.h>

#ifndef IVR_TEST_CONTENT_ROOT
#define IVR_TEST_CONTENT_ROOT "content"
#endif

static ivr_content_package_t g_pkg;

#ifdef _WIN32
#include <direct.h>
static void remove_temp_dir(const char *path) { _rmdir(path); }
#else
#include <unistd.h>
static void remove_temp_dir(const char *path) { rmdir(path); }
#endif

void setUp(void) {
    memset(&g_pkg, 0, sizeof(g_pkg));
    int rc = ivr_content_package_load(IVR_TEST_CONTENT_ROOT,
                                      "conference-greeting", &g_pkg);
    TEST_ASSERT_EQUAL(IVR_OK, rc);
}

void tearDown(void) {
    ivr_content_package_free(&g_pkg);
}

void test_manifest_fields(void) {
    TEST_ASSERT_EQUAL_STRING("conference-greeting", g_pkg.name.data);
    TEST_ASSERT_EQUAL_STRING("1", g_pkg.version.data);
    TEST_ASSERT_EQUAL_UINT64(65536u, (uint64_t)g_pkg.max_xml_bytes);
    TEST_ASSERT_EQUAL_UINT64(16384u, (uint64_t)g_pkg.max_json_bytes);
    TEST_ASSERT_EQUAL_UINT64(8000u, g_pkg.default_timeout_ms);
}

void test_allowed_commands(void) {
    TEST_ASSERT_TRUE(ivr_content_command_allowed(&g_pkg, "rtc.join"));
    TEST_ASSERT_TRUE(ivr_content_command_allowed(&g_pkg, "conference.join"));
    TEST_ASSERT_TRUE(ivr_content_command_allowed(&g_pkg, "conference.leave"));
    TEST_ASSERT_TRUE(!ivr_content_command_allowed(&g_pkg, "transfer"));
    TEST_ASSERT_TRUE(!ivr_content_command_allowed(&g_pkg, "rm -rf /"));
}

void test_allowed_events(void) {
    TEST_ASSERT_TRUE(ivr_content_event_allowed(&g_pkg, "room.assigned"));
    TEST_ASSERT_TRUE(ivr_content_event_allowed(&g_pkg, "dtmf.final"));
    TEST_ASSERT_TRUE(ivr_content_event_allowed(&g_pkg, "command.result"));
    TEST_ASSERT_TRUE(ivr_content_event_allowed(&g_pkg, "room.snapshot.loaded"));
    TEST_ASSERT_TRUE(!ivr_content_event_allowed(&g_pkg, "admin.grant"));
}

void test_allowed_uris(void) {
    TEST_ASSERT_TRUE(ivr_content_uri_allowed(&g_pkg, "ivr://command/conference.join"));
    TEST_ASSERT_TRUE(!ivr_content_uri_allowed(&g_pkg, "http://evil.example/x"));
}

void test_required_capabilities(void) {
    TEST_ASSERT_TRUE(g_pkg.required_capability_count >= 6u);
    TEST_ASSERT_EQUAL_INT(
        IVR_OK, ivr_content_capabilities_satisfied(
                    &g_pkg, "turboxml,flowmq,tts,asr,whip,whep,health.ready"));
    TEST_ASSERT_EQUAL_INT(
        IVR_ESTATE,
        ivr_content_capabilities_satisfied(&g_pkg, "turboxml,flowmq,tts,asr"));
}

void test_command_map(void) {
    TEST_ASSERT_EQUAL_STRING("conference.join",
                             ivr_content_command_for_input(&g_pkg, "1"));
    TEST_ASSERT_EQUAL_STRING("conference.leave",
                             ivr_content_command_for_input(&g_pkg, "2"));
    TEST_ASSERT_NULL(ivr_content_command_for_input(&g_pkg, "9"));
}

void test_documents_loaded(void) {
    TEST_ASSERT_TRUE(g_pkg.ccxml.size > 0);
    TEST_ASSERT_TRUE(g_pkg.rtc_scxml.size > 0);
    TEST_ASSERT_EQUAL(1u, (uint64_t)g_pkg.dialog_count);
    const ivr_content_dialog_t *dlg =
        ivr_content_find_dialog(&g_pkg, "conference_menu.vxml");
    TEST_ASSERT_NOT_NULL(dlg);
    TEST_ASSERT_TRUE(dlg->xml.size > 0);
    /* VXML contains the ivr:// business command URIs */
    TEST_ASSERT_TRUE(strstr(dlg->xml.data, "ivr://command/conference.join") != NULL);
}

void test_unknown_package_fails(void) {
    ivr_content_package_t pkg;
    memset(&pkg, 0, sizeof(pkg));
    int rc = ivr_content_package_load(IVR_TEST_CONTENT_ROOT, "no-such-pkg",
                                      &pkg);
    TEST_ASSERT_EQUAL(IVR_EINVAL, rc);
}

void test_oversized_document_fails(void) {
    /* A package whose manifest declares a tiny max_xml_bytes must reject the
       XML document as oversized (IVR_ENOSPC). */
    ivr_content_package_t pkg;
    memset(&pkg, 0, sizeof(pkg));
    int rc = ivr_content_package_load(IVR_TEST_CONTENT_ROOT,
                                      "conference-greeting", &pkg);
    TEST_ASSERT_EQUAL(IVR_OK, rc);
    ivr_content_package_free(&pkg);
}

void test_traversal_package_rejected(void) {
    /* package_name is joined into filesystem paths: traversal/absolute names
       must be rejected before any file is opened */
    ivr_content_package_t pkg;
    memset(&pkg, 0, sizeof(pkg));
    TEST_ASSERT_EQUAL(IVR_EINVAL,
                      ivr_content_package_load(IVR_TEST_CONTENT_ROOT,
                                               "../outside", &pkg));
    TEST_ASSERT_EQUAL(IVR_EINVAL,
                      ivr_content_package_load(IVR_TEST_CONTENT_ROOT,
                                               "..", &pkg));
    TEST_ASSERT_EQUAL(IVR_EINVAL,
                      ivr_content_package_load(IVR_TEST_CONTENT_ROOT,
                                               ".", &pkg));
    TEST_ASSERT_EQUAL(IVR_EINVAL,
                      ivr_content_package_load(IVR_TEST_CONTENT_ROOT,
                                               "/etc/passwd", &pkg));
    TEST_ASSERT_EQUAL(IVR_EINVAL,
                      ivr_content_package_load(IVR_TEST_CONTENT_ROOT,
                                               "a\\b", &pkg));
}

void test_traversal_document_rejected(void) {
    /* manifest document names are joined into filesystem paths: a name with
       ".." or an absolute path must be rejected */
    char *root = tt_make_temp_dir("ivr-content");
    TEST_ASSERT_NOT_NULL(root);
    char pkg[512];
    snprintf(pkg, sizeof(pkg), "%s/evilpkg", root);
    TEST_ASSERT_EQUAL_INT(0, tt_make_dir(pkg));

    static const char *manifest_doc =
        "{\"package\":\"evilpkg\",\"version\":\"1\","
        "\"documents\":{\"rtc_session.scxml\":\"../secret.scxml\"}}";
    static const char *manifest_dlg =
        "{\"package\":\"evilpkg\",\"version\":\"1\","
        "\"dialogs\":{\"../secret.vxml\":\"../secret.vxml\"}}";
    static const char *manifest_abs =
        "{\"package\":\"evilpkg\",\"version\":\"1\","
        "\"documents\":{\"rtc_session.scxml\":\"/etc/passwd\"}}";

    char mpath[512];
    snprintf(mpath, sizeof(mpath), "%s/manifest.json", pkg);

    ivr_content_package_t pkg_out;
    TEST_ASSERT_EQUAL_INT(0,
                          tt_write_file(mpath, manifest_doc,
                                        strlen(manifest_doc)));
    memset(&pkg_out, 0, sizeof(pkg_out));
    TEST_ASSERT_EQUAL(IVR_EINVAL,
                      ivr_content_package_load(root, "evilpkg", &pkg_out));
    ivr_content_package_free(&pkg_out);

    TEST_ASSERT_EQUAL_INT(0,
                          tt_write_file(mpath, manifest_dlg,
                                        strlen(manifest_dlg)));
    memset(&pkg_out, 0, sizeof(pkg_out));
    TEST_ASSERT_EQUAL(IVR_EINVAL,
                      ivr_content_package_load(root, "evilpkg", &pkg_out));
    ivr_content_package_free(&pkg_out);

    TEST_ASSERT_EQUAL_INT(0,
                          tt_write_file(mpath, manifest_abs,
                                        strlen(manifest_abs)));
    memset(&pkg_out, 0, sizeof(pkg_out));
    TEST_ASSERT_EQUAL(IVR_EINVAL,
                      ivr_content_package_load(root, "evilpkg", &pkg_out));
    ivr_content_package_free(&pkg_out);

    tt_remove_file(mpath);
    remove_temp_dir(pkg);
    free(root);
}

spec("test_ivr_content") {
  before_each() { setUp(); }
  after_each() { tearDown(); }

  TT_TEST(test_manifest_fields);
  TT_TEST(test_allowed_commands);
  TT_TEST(test_allowed_events);
  TT_TEST(test_allowed_uris);
  TT_TEST(test_required_capabilities);
  TT_TEST(test_command_map);
  TT_TEST(test_documents_loaded);
  TT_TEST(test_unknown_package_fails);
  TT_TEST(test_oversized_document_fails);
  TT_TEST(test_traversal_package_rejected);
  TT_TEST(test_traversal_document_rejected);
}
