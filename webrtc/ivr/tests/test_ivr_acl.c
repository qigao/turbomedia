/* test_ivr_acl.c - shared tenant/room/call scope + content capability ACL */
#include "ivr/ivr_acl.h"
#include "tinytest.h"
#include <string.h>

void test_scope_valid(void) {
    check_true(ivr_acl_scope_valid("*"));
    check_true(ivr_acl_scope_valid("room-1"));
    check_true(ivr_acl_scope_valid("room-1,room-2"));
    check_true(ivr_acl_scope_valid("acme/room-1,acme/room-2"));
    check_false(ivr_acl_scope_valid(NULL));
    check_false(ivr_acl_scope_valid(""));
    check_false(ivr_acl_scope_valid(","));
    check_false(ivr_acl_scope_valid("room-1,"));
    check_false(ivr_acl_scope_valid(",room-1"));
    check_false(ivr_acl_scope_valid("room-1,,room-2"));
    check_false(ivr_acl_scope_valid("room 1"));
    check_false(ivr_acl_scope_valid(" room-1"));
}

void test_scope_allows(void) {
    check_true(ivr_acl_scope_allows("*", "room-1"));
    check_false(ivr_acl_scope_allows("*", NULL));
    check_false(ivr_acl_scope_allows("*", ""));
    check_true(ivr_acl_scope_allows("room-1,room-2", "room-2"));
    check_false(ivr_acl_scope_allows("room-1,room-2", "room-3"));
    check_false(ivr_acl_scope_allows("room-1", "room-10"));
    check_true(ivr_acl_scope_allows("acme/room-1,acme/room-2",
                                          "acme/room-2"));
    check_false(ivr_acl_scope_allows(NULL, "room-1"));
    check_false(ivr_acl_scope_allows("", "room-1"));
}

void test_tenant_allows(void) {
    /* no tenant restriction grants everything */
    check_true(ivr_acl_tenant_allows(NULL, "room-1"));
    check_true(ivr_acl_tenant_allows("", "room-1"));
    /* tenant prefix convention */
    check_true(ivr_acl_tenant_allows("acme", "acme"));
    check_true(ivr_acl_tenant_allows("acme", "acme/room-1"));
    check_false(ivr_acl_tenant_allows("acme", "acme-room"));
    check_false(ivr_acl_tenant_allows("acme", "other/room-1"));
    check_false(ivr_acl_tenant_allows("acme", NULL));
    check_false(ivr_acl_tenant_allows("acme", ""));
}

spec("test_ivr_acl") {
  it("test_scope_valid") { test_scope_valid(); };
  it("test_scope_allows") { test_scope_allows(); };
  it("test_tenant_allows") { test_tenant_allows(); };
}
