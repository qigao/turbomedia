#ifndef TURBO_MEDIA_IVR_ACL_H
#define TURBO_MEDIA_IVR_ACL_H

/**
 * @file ivr_acl.h
 * @brief Shared tenant/room/call scope and content-capability ACL helpers.
 *
 * These helpers are used by both the RoomService adapter (command
 * authorization) and the IVR worker (dispatch admission) so the two sides
 * agree on the exact scope matching rules. A scope value is either "*" (any)
 * or a comma-separated list of exact identifiers. The tenant rule is a
 * prefix convention: a room_id belongs to a tenant when it equals the tenant
 * id or starts with "<tenant>/".
 */

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when `scope` is "*" or a non-empty comma-separated list of non-empty,
   comma-free tokens with no leading/trailing whitespace. Empty string is
   invalid. */
int ivr_acl_scope_valid(const char *scope);

/* 1 when `value` is allowed by `scope` ("*" or exact match in the list). A
   NULL or empty value is never allowed unless scope is "*" and value is
   non-NULL. */
int ivr_acl_scope_allows(const char *scope, const char *value);

/* 1 when `room_id` belongs to `tenant_id` (room == tenant or
   room == "<tenant>/..."). A NULL/empty tenant grants everything; a NULL/empty
   room_id is denied. */
int ivr_acl_tenant_allows(const char *tenant_id, const char *room_id);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_MEDIA_IVR_ACL_H */