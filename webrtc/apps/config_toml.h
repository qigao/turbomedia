#ifndef TURBO_MEDIA_RTC_APP_CONFIG_TOML_H
#define TURBO_MEDIA_RTC_APP_CONFIG_TOML_H

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <tlog.h>
#include <salts_fs.h>
#include <toml.h>

enum { RTC_APP_TOML_ERROR_SIZE = 200 };

typedef struct rtc_app_config_storage_s {
    size_t count;
    char *values[];
} rtc_app_config_storage_t;

typedef struct rtc_app_toml_document_s {
    salts_fs_buf_t file;
    toml_table_t *root;
} rtc_app_toml_document_t;

static inline rtc_app_config_storage_t *rtc_app_config_storage_create(size_t count) {
    rtc_app_config_storage_t *storage;

    if (count > (SIZE_MAX - sizeof(*storage)) / sizeof(storage->values[0])) {
        return NULL;
    }
    storage = (rtc_app_config_storage_t *)calloc(
        1, sizeof(*storage) + count * sizeof(storage->values[0]));
    if (storage) {
        storage->count = count;
    }
    return storage;
}

static inline void rtc_app_config_storage_destroy(rtc_app_config_storage_t *storage) {
    size_t index;

    if (!storage) {
        return;
    }
    for (index = 0; index < storage->count; ++index) {
        free(storage->values[index]);
    }
    free(storage);
}

static inline char *rtc_app_config_string_duplicate(const char *value) {
    size_t size;
    char *copy;

    if (!value) {
        return NULL;
    }
    size = strlen(value) + 1;
    copy = (char *)malloc(size);
    if (copy) {
        memcpy(copy, value, size);
    }
    return copy;
}

static inline int rtc_app_config_storage_replace(
    rtc_app_config_storage_t *storage,
    size_t index,
    char *value,
    const char **target) {
    if (!storage || index >= storage->count || !target) {
        free(value);
        return -1;
    }
    free(storage->values[index]);
    storage->values[index] = value;
    *target = value;
    return 0;
}

static inline int rtc_app_config_storage_copy(
    rtc_app_config_storage_t *storage,
    size_t index,
    const char *value,
    const char **target) {
    char *copy = rtc_app_config_string_duplicate(value);

    if (value && !copy) {
        return -1;
    }
    return rtc_app_config_storage_replace(storage, index, copy, target);
}

static inline int rtc_app_toml_key_equals(
    const char *key,
    int key_length,
    const char *expected) {
    size_t expected_length;

    if (!key || key_length < 0 || !expected) {
        return 0;
    }
    expected_length = strlen(expected);
    return expected_length == (size_t)key_length &&
           memcmp(key, expected, expected_length) == 0;
}

static inline int rtc_app_toml_table_has_key(
    const toml_table_t *table,
    const char *expected) {
    int index;
    int count = toml_table_len(table);

    for (index = 0; index < count; ++index) {
        int key_length = 0;
        const char *key = toml_table_key(table, index, &key_length);
        if (rtc_app_toml_key_equals(key, key_length, expected)) {
            return 1;
        }
    }
    return 0;
}

static inline int rtc_app_toml_table_keys_valid(
    const toml_table_t *table,
    const char *section,
    const char *const *allowed,
    size_t allowed_count) {
    int index;
    int count;

    if (!table) {
        return -1;
    }
    count = toml_table_len(table);
    for (index = 0; index < count; ++index) {
        int key_length = 0;
        const char *key = toml_table_key(table, index, &key_length);
        size_t allowed_index;
        int found = 0;

        for (allowed_index = 0; allowed_index < allowed_count; ++allowed_index) {
            if (rtc_app_toml_key_equals(key, key_length, allowed[allowed_index])) {
                found = 1;
                break;
            }
        }
        if (!found) {
            TLOG_ERRORF("Unknown TOML key in [{}]", section);
            return -1;
        }
    }
    return 0;
}

static inline int rtc_app_toml_get_optional_table(
    const toml_table_t *root,
    const char *name,
    toml_table_t **table) {
    *table = NULL;
    if (!rtc_app_toml_table_has_key(root, name)) {
        return 0;
    }
    *table = toml_table_table(root, name);
    if (!*table) {
        TLOG_ERRORF("TOML root key '{}' must be a table", name);
        return -1;
    }
    return 0;
}

static inline int rtc_app_toml_apply_string(
    const toml_table_t *table,
    const char *section,
    const char *key,
    rtc_app_config_storage_t *storage,
    size_t index,
    const char **target) {
    toml_value_t value;

    if (!rtc_app_toml_table_has_key(table, key)) {
        return 0;
    }
    value = toml_table_string(table, key);
    if (!value.ok || !value.u.s || value.u.sl < 0 ||
        strlen(value.u.s) != (size_t)value.u.sl) {
        free(value.ok ? value.u.s : NULL);
        TLOG_ERRORF("TOML key [{}].{} must be a string without embedded NUL",
                   section, key);
        return -1;
    }
    return rtc_app_config_storage_replace(storage, index, value.u.s, target);
}

static inline int rtc_app_toml_apply_string_array(
    const toml_table_t *table,
    const char *section,
    const char *key,
    rtc_app_config_storage_t *storage,
    size_t storage_base,
    const char **targets,
    int target_capacity,
    int *target_count) {
    toml_array_t *array;
    int count;
    int index;

    if (!table || !section || !key || !storage || !targets ||
        target_capacity < 0 || !target_count) {
        return -1;
    }
    if (!rtc_app_toml_table_has_key(table, key)) {
        return 0;
    }
    array = toml_table_array(table, key);
    if (!array) {
        TLOG_ERRORF("TOML key [{}].{} must be an array of strings",
                   section, key);
        return -1;
    }
    count = toml_array_len(array);
    if (count < 0 || count > target_capacity) {
        TLOG_ERRORF("TOML key [{}].{} exceeds the maximum of {} entries",
                   section, key, target_capacity);
        return -1;
    }

    for (index = 0; index < target_capacity; ++index) {
        if (rtc_app_config_storage_replace(
                storage, storage_base + (size_t)index, NULL,
                &targets[index]) != 0) {
            return -1;
        }
    }
    for (index = 0; index < count; ++index) {
        toml_value_t value = toml_array_string(array, index);
        if (!value.ok || !value.u.s || value.u.sl <= 0 ||
            strlen(value.u.s) != (size_t)value.u.sl) {
            free(value.ok ? value.u.s : NULL);
            TLOG_ERRORF(
                "TOML key [{}].{}[{}] must be a non-empty string without embedded NUL",
                section, key, index);
            return -1;
        }
        if (rtc_app_config_storage_replace(
                storage, storage_base + (size_t)index, value.u.s,
                &targets[index]) != 0) {
            return -1;
        }
    }
    *target_count = count;
    return 0;
}

static inline int rtc_app_toml_apply_bool(
    const toml_table_t *table,
    const char *section,
    const char *key,
    int *target) {
    toml_value_t value;

    if (!rtc_app_toml_table_has_key(table, key)) {
        return 0;
    }
    value = toml_table_bool(table, key);
    if (!value.ok) {
        TLOG_ERRORF("TOML key [{}].{} must be a boolean", section, key);
        return -1;
    }
    *target = value.u.b ? 1 : 0;
    return 0;
}

static inline int rtc_app_toml_apply_int(
    const toml_table_t *table,
    const char *section,
    const char *key,
    int *target) {
    toml_value_t value;

    if (!rtc_app_toml_table_has_key(table, key)) {
        return 0;
    }
    value = toml_table_int(table, key);
    if (!value.ok || value.u.i < INT_MIN || value.u.i > INT_MAX) {
        TLOG_ERRORF("TOML key [{}].{} must be a 32-bit integer", section, key);
        return -1;
    }
    *target = (int)value.u.i;
    return 0;
}

static inline int rtc_app_toml_document_open(
    rtc_app_toml_document_t *document,
    const char *filename) {
    char parse_error[RTC_APP_TOML_ERROR_SIZE] = {0};

    memset(document, 0, sizeof(*document));
    if (salts_fs_read_file(filename, &document->file) != 0) {
        TLOG_ERRORF("Failed to open configuration file: {}", filename);
        return -1;
    }
    document->root = toml_parse(document->file.base, parse_error,
                                (int)sizeof(parse_error));
    if (!document->root) {
        TLOG_ERRORF("Failed to parse TOML configuration file {}: {}", filename,
                    parse_error);
        salts_fs_buf_free(&document->file);
        return -1;
    }
    return 0;
}

static inline void rtc_app_toml_document_close(
    rtc_app_toml_document_t *document) {
    if (!document) {
        return;
    }
    toml_free(document->root);
    document->root = NULL;
    salts_fs_buf_free(&document->file);
}

static inline int rtc_app_config_string_in_set(
    const char *value,
    const char *const *allowed,
    size_t allowed_count) {
    size_t index;

    if (!value) {
        return 0;
    }
    for (index = 0; index < allowed_count; ++index) {
        if (strcmp(value, allowed[index]) == 0) {
            return 1;
        }
    }
    return 0;
}

#endif /* TURBO_MEDIA_RTC_APP_CONFIG_TOML_H */
