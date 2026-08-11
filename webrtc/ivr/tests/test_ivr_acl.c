/* test_ivr_acl.c - shared tenant/room/call scope + content capability ACL */
#include "ivr/ivr_acl.h"
#include "tinytest_compat.h"
#include <string.h>

void test_scope_valid(void) {
    TEST_ASSERT_TRUE(ivr_acl_scope_valid("*"));
    TEST_ASSERT_TRUE(ivr_acl_scope_valid("room-1"));
    TEST_ASSERT_TRUE(ivr_acl_scope_valid("room-1,room-2"));
    TEST_ASSERT_TRUE(ivr_acl_scope_valid("acme/room-1,acme/room-2"));
    TEST_ASSERT_FALSE(ivr_acl_scope_valid(NULL));
    TEST_ASSERT_FALSE(ivr_acl_scope_valid(""));
    TEST_ASSERT_FALSE(ivr_acl_scope_valid(","));
    TEST_ASSERT_FALSE(ivr_acl_scope_valid("room-1,"));
    TEST_ASSERT_FALSE(ivr_acl_scope_valid(",room-1"));
    TEST_ASSERT_FALSE(ivr_acl_scope_valid("room-1,,room-2"));
    TEST_ASSERT_FALSE(ivr_acl_scope_valid("room 1"));
    TEST_ASSERT_FALSE(ivr_acl_scope_valid(" room-1"));
}

void test_scope_allows(void) {
    TEST_ASSERT_TRUE(ivr_acl_scope_allows("*", "room-1"));
    TEST_ASSERT_FALSE(ivr_acl_scope_allows("*", NULL));
    TEST_ASSERT_FALSE(ivr_acl_scope_allows("*", ""));
    TEST_ASSERT_TRUE(ivr_acl_scope_allows("room-1,room-2", "room-2"));
    TEST_ASSERT_FALSE(ivr_acl_scope_allows("room-1,room-2", "room-3"));
    TEST_ASSERT_FALSE(ivr_acl_scope_allows("room-1", "room-10"));
    TEST_ASSERT_TRUE(ivr_acl_scope_allows("acme/room-1,acme/room-2",
                                          "acme/room-2"));
    TEST_ASSERT_FALSE(ivr_acl_scope_allows(NULL, "room-1"));
    TEST_ASSERT_FALSE(ivr_acl_scope_allows("", "room-1"));
}

void test_tenant_allows(void) {
    /* no tenant restriction grants everything */
    TEST_ASSERT_TRUE(ivr_acl_tenant_allows(NULL, "room-1"));
    TEST_ASSERT_TRUE(ivr_acl_tenant_allows("", "room-1"));
    /* tenant prefix convention */
    TEST_ASSERT_TRUE(ivr_acl_tenant_allows("acme", "acme"));
    TEST_ASSERT_TRUE(ivr_acl_tenant_allows("acme", "acme/room-1"));
    TEST_ASSERT_FALSE(ivr_acl_tenant_allows("acme", "acme-room"));
    TEST_ASSERT_FALSE(ivr_acl_tenant_allows("acme", "other/room-1"));
    TEST_ASSERT_FALSE(ivr_acl_tenant_allows("acme", NULL));
    TEST_ASSERT_FALSE(ivr_acl_tenant_allows("acme", ""));
}

spec("test_ivr_acl") {
  TT_TEST(test_scope_valid);
  TT_TEST(test_scope_allows);
  TT_TEST(test_tenant_allows);
}