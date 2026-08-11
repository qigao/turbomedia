#include "ivr/ivr_acl.h"

#include <string.h>

int ivr_acl_scope_valid(const char *scope) {
    const char *p;
    if (!scope || scope[0] == '\0') {
        return 0;
    }
    if (strcmp(scope, "*") == 0) {
        return 1;
    }
    p = scope;
    while (*p) {
        /* A token is one or more non-comma characters; spaces/tabs are never
           allowed anywhere in a scope value. */
        if (*p == ',' || *p == ' ' || *p == '\t' || *p == '\r' ||
            *p == '\n') {
            return 0;
        }
        while (*p && *p != ',' && *p != ' ' && *p != '\t' && *p != '\r' &&
               *p != '\n') {
            ++p;
        }
        if (*p != '\0' && *p != ',') {
            return 0; /* whitespace inside a token */
        }
        if (*p == ',') {
            ++p;
            if (*p == '\0' || *p == ',') {
                return 0;
            }
        }
    }
    return 1;
}

int ivr_acl_scope_allows(const char *scope, const char *value) {
    const char *p;
    size_t value_len;
    if (!scope || scope[0] == '\0' || !value || value[0] == '\0') {
        return 0;
    }
    if (strcmp(scope, "*") == 0) {
        return 1;
    }
    value_len = strlen(value);
    p = scope;
    while (*p) {
        const char *end = strchr(p, ',');
        size_t token_len = end ? (size_t)(end - p) : strlen(p);
        if (token_len == value_len && memcmp(p, value, value_len) == 0) {
            return 1;
        }
        if (!end) {
            break;
        }
        p = end + 1;
    }
    return 0;
}

int ivr_acl_tenant_allows(const char *tenant_id, const char *room_id) {
    size_t tenant_len;
    if (!room_id || room_id[0] == '\0') {
        return 0;
    }
    if (!tenant_id || tenant_id[0] == '\0') {
        return 1;
    }
    tenant_len = strlen(tenant_id);
    if (strncmp(room_id, tenant_id, tenant_len) != 0) {
        return 0;
    }
    if (room_id[tenant_len] == '\0' || room_id[tenant_len] == '/') {
        return 1;
    }
    return 0;
}