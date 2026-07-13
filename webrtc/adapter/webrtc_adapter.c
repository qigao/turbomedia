#include "turbo_media_rtc.h"

#include <string.h>

#define TURBO_MEDIA_RTC_DEFAULT_VHOST "default"

static const char *rtc_nonempty_or_default(const char *value, const char *fallback) {
    return value && value[0] ? value : fallback;
}

static int rtc_copy_token(char *dst, size_t dst_size, const char *start, const char *end) {
    size_t len;

    if (!dst || dst_size == 0 || !start || !end || end < start) {
        return TURBO_MEDIA_ERR_INVALID;
    }

    len = (size_t)(end - start);
    if (len == 0 || len >= dst_size) {
        return TURBO_MEDIA_ERR_INVALID;
    }

    memcpy(dst, start, len);
    dst[len] = '\0';
    return TURBO_MEDIA_OK;
}

static int rtc_query_param_matches(
    const char *name_start,
    const char *name_end,
    const char *name) {
    size_t name_len;

    if (!name_start || !name_end || !name) return 0;
    name_len = strlen(name);
    return (size_t)(name_end - name_start) == name_len &&
           memcmp(name_start, name, name_len) == 0;
}

static int rtc_copy_query_param(
    const char *query,
    const char *name,
    char *dst,
    size_t dst_size,
    int *found) {
    const char *cursor = query;

    if (found) *found = 0;
    if (!query || !name || !dst || dst_size == 0) return TURBO_MEDIA_ERR_INVALID;

    while (*cursor) {
        const char *name_start = cursor;
        const char *name_end;
        const char *value_start;
        const char *value_end;

        while (*cursor && *cursor != '=' && *cursor != '&') cursor++;
        name_end = cursor;
        if (*cursor != '=') {
            while (*cursor && *cursor != '&') cursor++;
            if (*cursor == '&') cursor++;
            continue;
        }

        cursor++;
        value_start = cursor;
        while (*cursor && *cursor != '&') cursor++;
        value_end = cursor;

        if (rtc_query_param_matches(name_start, name_end, name)) {
            if (found) *found = 1;
            return rtc_copy_token(dst, dst_size, value_start, value_end);
        }

        if (*cursor == '&') cursor++;
    }

    dst[0] = '\0';
    return TURBO_MEDIA_OK;
}

static int rtc_source_key_from_query(
    const char *default_vhost,
    const char *query,
    turbo_media_source_key_t *key,
    int *used_query) {
    char vhost[TURBO_MEDIA_MAX_VHOST_LEN];
    char app[TURBO_MEDIA_MAX_APP_LEN];
    char stream[TURBO_MEDIA_MAX_STREAM_LEN];
    int found_vhost = 0;
    int found_app = 0;
    int found_stream = 0;
    int rc;

    if (used_query) *used_query = 0;
    if (!query || !query[0]) return TURBO_MEDIA_OK;

    rc = rtc_copy_query_param(query, "vhost", vhost, sizeof(vhost), &found_vhost);
    if (rc != TURBO_MEDIA_OK) return rc;
    rc = rtc_copy_query_param(query, "app", app, sizeof(app), &found_app);
    if (rc != TURBO_MEDIA_OK) return rc;
    rc = rtc_copy_query_param(query, "stream", stream, sizeof(stream), &found_stream);
    if (rc != TURBO_MEDIA_OK) return rc;

    if (!found_app && !found_stream && !found_vhost) {
        return TURBO_MEDIA_OK;
    }
    if (!found_app || !found_stream) {
        return TURBO_MEDIA_ERR_INVALID;
    }

    if (used_query) *used_query = 1;
    return turbo_media_source_key_init(
        key,
        found_vhost ? vhost : rtc_nonempty_or_default(default_vhost, TURBO_MEDIA_RTC_DEFAULT_VHOST),
        app,
        stream);
}

static int rtc_source_key_from_path(
    const char *default_vhost,
    const char *resource_path,
    turbo_media_source_key_t *key) {
    const char *path;
    const char *app_start;
    const char *app_end;
    const char *stream_start;
    const char *stream_end;
    char app[TURBO_MEDIA_MAX_APP_LEN];
    char stream[TURBO_MEDIA_MAX_STREAM_LEN];
    int rc;

    if (!resource_path || !resource_path[0]) {
        return TURBO_MEDIA_ERR_INVALID;
    }

    path = resource_path;
    while (*path == '/') path++;
    app_start = path;
    while (*path && *path != '/' && *path != '?') path++;
    app_end = path;
    if (*path != '/') return TURBO_MEDIA_ERR_INVALID;

    path++;
    stream_start = path;
    while (*path && *path != '/' && *path != '?') path++;
    stream_end = path;

    rc = rtc_copy_token(app, sizeof(app), app_start, app_end);
    if (rc != TURBO_MEDIA_OK) return rc;
    rc = rtc_copy_token(stream, sizeof(stream), stream_start, stream_end);
    if (rc != TURBO_MEDIA_OK) return rc;

    return turbo_media_source_key_init(
        key,
        rtc_nonempty_or_default(default_vhost, TURBO_MEDIA_RTC_DEFAULT_VHOST),
        app,
        stream);
}

int turbo_media_rtc_source_key(
    const char *default_vhost,
    const char *resource_path,
    const char *query,
    turbo_media_source_key_t *key) {
    int used_query = 0;
    int rc;

    if (!key) return TURBO_MEDIA_ERR_INVALID;

    rc = rtc_source_key_from_query(default_vhost, query, key, &used_query);
    if (rc != TURBO_MEDIA_OK) return rc;
    if (used_query) return TURBO_MEDIA_OK;

    return rtc_source_key_from_path(default_vhost, resource_path, key);
}
