#include "ivr_content.h"
#include "ivr/ivr_worker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "turbo_parser.h"
#if !defined(_WIN32)
#include <limits.h> /* PATH_MAX for realpath() */
#endif

/* ------------------------------------------------------------------ */
/* file helpers                                                        */
/* ------------------------------------------------------------------ */

static int ivr_content_read_file(const char *path, size_t max_bytes,
                                 ivr_str_t *out) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    if ((size_t)sz > max_bytes) {
        fclose(f);
        return -2; /* oversized */
    }
    if (ivr_str_assign(out, NULL, (size_t)sz) < 0) {
        fclose(f);
        return -1;
    }
    size_t got = fread(out->data, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        return -1;
    }
    out->size = (size_t)sz;
    out->data[out->size] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ */
/* manifest JSON accessors                                             */
/* ------------------------------------------------------------------ */

static const json_value_t *obj_get(const json_value_t *obj, const char *key) {
    if (!obj || turbo_json_type(obj) != TURBO_JSON_OBJECT) {
        return NULL;
    }
    size_t n = turbo_json_object_size(obj);
    for (size_t i = 0; i < n; i++) {
        const char *k = turbo_json_object_key(obj, i);
        if (k && strcmp(k, key) == 0) {
            return turbo_json_object_value(obj, i);
        }
    }
    return NULL;
}

static int obj_str(const json_value_t *obj, const char *key, ivr_str_t *out) {
    const json_value_t *v = obj_get(obj, key);
    if (!v || turbo_json_type(v) != TURBO_JSON_STRING) {
        return -1;
    }
    const char *s = turbo_json_string(v);
    return ivr_str_assign(out, s, turbo_json_string_len(v));
}

static int obj_u64(const json_value_t *obj, const char *key, uint64_t *out) {
    const json_value_t *v = obj_get(obj, key);
    if (!v || turbo_json_type(v) != TURBO_JSON_NUMBER) {
        return -1;
    }
    double d = turbo_json_number(v);
    if (d < 0 || d > 1e15) {
        return -1;
    }
    *out = (uint64_t)d;
    return 0;
}

static int append_string_array(ivr_str_t **arr, size_t *count,
                               const json_value_t *list) {
    if (!list || turbo_json_type(list) != TURBO_JSON_ARRAY) {
        return list ? -1 : 0;
    }
    size_t n = turbo_json_array_size(list);
    if (n == 0) {
        return 0;
    }
    ivr_str_t *next = (ivr_str_t *)realloc(*arr, (*count + n) * sizeof(ivr_str_t));
    if (!next) {
        return -1;
    }
    *arr = next;
    for (size_t i = 0; i < n; i++) {
        json_value_t *item = turbo_json_array_get(list, i);
        if (!item || turbo_json_type(item) != TURBO_JSON_STRING) {
            return -1;
        }
        ivr_str_init(&(*arr)[*count + i]);
        if (ivr_str_assign(&(*arr)[*count + i], turbo_json_string(item),
                           turbo_json_string_len(item)) < 0) {
            return -1;
        }
    }
    *count += n;
    return 0;
}

/* ------------------------------------------------------------------ */
/* path safety                                                         */
/* ------------------------------------------------------------------ */

/* A package name must be a single path segment: no separators, no "." / "..",
   no control characters, and short enough to fit the path buffers. */
static int ivr_content_package_name_safe(const char *name) {
    size_t len;
    if (!name || name[0] == '\0' || name[0] == '/' || name[0] == '\\') {
        return 0;
    }
    len = strlen(name);
    if (len >= 256 || strchr(name, '/') || strchr(name, '\\') ||
        strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        if ((unsigned char)name[i] < 0x20) {
            return 0;
        }
    }
    return 1;
}

/* A manifest document name is a relative path inside the package directory:
   non-empty, no leading separator, no '\\', no empty/./.. segments. */
static int ivr_content_doc_path_safe(const char *rel) {
    size_t len;
    size_t seg_start;
    if (!rel || rel[0] == '\0' || rel[0] == '/' || rel[0] == '\\') {
        return 0;
    }
    len = strlen(rel);
    if (len >= 512) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)rel[i];
        if (c == '\\' || c < 0x20) {
            return 0;
        }
    }
    seg_start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || rel[i] == '/') {
            size_t seg_len = i - seg_start;
            if (seg_len == 0 ||
                (seg_len == 1 && rel[seg_start] == '.') ||
                (seg_len == 2 && rel[seg_start] == '.' &&
                 rel[seg_start + 1] == '.')) {
                return 0;
            }
            seg_start = i + 1;
        }
    }
    return 1;
}

/* Verifies the resolved absolute form of `path` stays inside the resolved
   absolute package directory `pkg_dir`. This is a second line of defense
   after the lexical segment checks (catches "..", absolute paths and, on
   POSIX, symlink escapes). When the file does not exist yet the lexical
   checks already guarantee containment, so the open error path decides. */
static int ivr_content_path_within_root(const char *path, const char *pkg_dir) {
#if defined(_WIN32)
    char full[_MAX_PATH];
    char base[_MAX_PATH];
    size_t blen;
    if (!_fullpath(full, path, sizeof(full)) ||
        !_fullpath(base, pkg_dir, sizeof(base))) {
        return 0;
    }
    blen = strlen(base);
    if (_strnicmp(full, base, blen) != 0) {
        return 0;
    }
    return full[blen] == '\0' || full[blen] == '\\' || full[blen] == '/';
#else
    char full[PATH_MAX];
    char base[PATH_MAX];
    size_t blen;
    if (!realpath(path, full)) {
        return 1; /* not on disk yet: the lexical checks already bound it */
    }
    if (!realpath(pkg_dir, base)) {
        return 0;
    }
    blen = strlen(base);
    if (strncmp(full, base, blen) != 0) {
        return 0;
    }
    return full[blen] == '\0' || full[blen] == '/';
#endif
}

/* ------------------------------------------------------------------ */
/* load                                                                */
/* ------------------------------------------------------------------ */

int ivr_content_package_load(const char *content_root,
                             const char *package_name,
                             ivr_content_package_t *out) {
    if (!content_root || !package_name || !out) {
        return IVR_EINVAL;
    }
    if (!ivr_content_package_name_safe(package_name)) {
        /* content_root must stay the boundary: reject traversal names */
        return IVR_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    ivr_str_init(&out->name);
    ivr_str_init(&out->version);
    ivr_str_init(&out->rtc_scxml);
    ivr_str_init(&out->ccxml);

    char pkg_dir[1024];
    int pn = snprintf(pkg_dir, sizeof(pkg_dir), "%s/%s", content_root,
                      package_name);
    if (pn < 0 || (size_t)pn >= sizeof(pkg_dir)) {
        return IVR_EINVAL;
    }
    char manifest_path[1024];
    pn = snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json",
                  pkg_dir);
    if (pn < 0 || (size_t)pn >= sizeof(manifest_path)) {
        return IVR_EINVAL;
    }
    if (!ivr_content_path_within_root(manifest_path, pkg_dir)) {
        return IVR_EINVAL; /* resolved manifest path escapes content_root */
    }
    ivr_str_t manifest_text;
    ivr_str_init(&manifest_text);
    int rc = ivr_content_read_file(manifest_path, 64 * 1024, &manifest_text);
    if (rc == -2) {
        ivr_str_free(&manifest_text);
        return IVR_ENOSPC;
    }
    if (rc < 0) {
        ivr_str_free(&manifest_text);
        return IVR_EINVAL;
    }

    turbo_json_doc_t *doc = NULL;
    if (turbo_parse_json((const uint8_t *)manifest_text.data,
                         manifest_text.size, &doc) != 0) {
        ivr_str_free(&manifest_text);
        return IVR_EINVAL;
    }
    const json_value_t *root = (const json_value_t *)doc;
    int result = IVR_EINVAL;

    if (obj_str(root, "package", &out->name) < 0 ||
        obj_str(root, "version", &out->version) < 0) {
        goto done;
    }
    if (strcmp(out->name.data, package_name) != 0) {
        result = IVR_ESTATE;
        goto done;
    }
    uint64_t v = 0;
    if (obj_u64(root, "max_xml_bytes", &v) == 0 && v > 0) {
        out->max_xml_bytes = (size_t)v;
    } else {
        out->max_xml_bytes = 64 * 1024;
    }
    if (obj_u64(root, "max_json_bytes", &v) == 0 && v > 0) {
        out->max_json_bytes = (size_t)v;
    } else {
        out->max_json_bytes = 16 * 1024;
    }
    if (obj_u64(root, "default_timeout_ms", &v) == 0 && v > 0) {
        out->default_timeout_ms = v;
    } else {
        out->default_timeout_ms = 8000;
    }

    if (append_string_array(&out->allowed_commands, &out->allowed_command_count,
                            obj_get(root, "allowed_commands")) < 0 ||
        append_string_array(&out->allowed_events, &out->allowed_event_count,
                            obj_get(root, "allowed_events")) < 0 ||
        append_string_array(&out->allowed_uris, &out->allowed_uri_count,
                            obj_get(root, "allowed_uris")) < 0 ||
        append_string_array(&out->required_capabilities,
                            &out->required_capability_count,
                            obj_get(root, "required_capabilities")) < 0) {
        goto done;
    }

    /* input -> business command map */
    const json_value_t *cmap = obj_get(root, "command_map");
    if (cmap && turbo_json_type(cmap) == TURBO_JSON_OBJECT) {
        size_t n = turbo_json_object_size(cmap);
        out->command_map = (ivr_content_cmdmap_entry_t *)calloc(
            n ? n : 1, sizeof(*out->command_map));
        if (!out->command_map) {
            result = IVR_ENOSPC;
            goto done;
        }
        for (size_t i = 0; i < n; i++) {
            const char *input = turbo_json_object_key(cmap, i);
            json_value_t *cmd = turbo_json_object_value(cmap, i);
            ivr_str_init(&out->command_map[i].input);
            ivr_str_init(&out->command_map[i].command);
            if (!input || !cmd ||
                turbo_json_type(cmd) != TURBO_JSON_STRING ||
                ivr_str_assign(&out->command_map[i].input, input,
                               strlen(input)) < 0 ||
                ivr_str_assign(&out->command_map[i].command,
                               turbo_json_string(cmd),
                               turbo_json_string_len(cmd)) < 0) {
                result = IVR_ESTATE;
                goto done;
            }
            out->command_map_count++;
        }
    }

    /* documents */
    const json_value_t *docs = obj_get(root, "documents");
    if (docs && turbo_json_type(docs) == TURBO_JSON_OBJECT) {
        if (obj_str(docs, "rtc_session.scxml", &out->rtc_scxml) < 0 ||
            obj_str(docs, "call_control.ccxml", &out->ccxml) < 0) {
            goto done;
        }
    }
    /* dialogs */
    const json_value_t *dlg = obj_get(root, "dialogs");
    if (dlg && turbo_json_type(dlg) == TURBO_JSON_OBJECT) {
        size_t n = turbo_json_object_size(dlg);
        out->dialogs = (ivr_content_dialog_t *)calloc(n ? n : 1, sizeof(*out->dialogs));
        if (!out->dialogs) {
            result = IVR_ENOSPC;
            goto done;
        }
        for (size_t i = 0; i < n; i++) {
            const char *src = turbo_json_object_key(dlg, i);
            json_value_t *file = turbo_json_object_value(dlg, i);
            ivr_str_init(&out->dialogs[i].src);
            ivr_str_init(&out->dialogs[i].xml);
            if (!src || !file ||
                turbo_json_type(file) != TURBO_JSON_STRING ||
                ivr_str_assign(&out->dialogs[i].src, src, strlen(src)) < 0) {
                result = IVR_ESTATE;
                goto done;
            }
            out->dialog_count++;
        }
    }

    /* Document names come from the manifest and are joined into filesystem
       paths: reject names that could escape the package directory before any
       file is opened. */
    if ((out->rtc_scxml.size > 0 &&
         !ivr_content_doc_path_safe(out->rtc_scxml.data)) ||
        (out->ccxml.size > 0 &&
         !ivr_content_doc_path_safe(out->ccxml.data))) {
        result = IVR_EINVAL;
        goto done;
    }
    for (size_t i = 0; i < out->dialog_count; i++) {
        if (!ivr_content_doc_path_safe(out->dialogs[i].src.data)) {
            result = IVR_EINVAL;
            goto done;
        }
    }

    /* Load documents from disk with size caps. */
    char file_path[1024];
    if (out->rtc_scxml.size > 0) {
        snprintf(file_path, sizeof(file_path), "%s/%s", pkg_dir,
                 out->rtc_scxml.data);
        if (!ivr_content_path_within_root(file_path, pkg_dir)) {
            result = IVR_EINVAL;
            goto done;
        }
        rc = ivr_content_read_file(file_path, out->max_xml_bytes, &out->rtc_scxml);
        if (rc == -2) {
            result = IVR_ENOSPC;
            goto done;
        }
        if (rc < 0) {
            goto done;
        }
    }
    if (out->ccxml.size > 0) {
        snprintf(file_path, sizeof(file_path), "%s/%s", pkg_dir,
                 out->ccxml.data);
        if (!ivr_content_path_within_root(file_path, pkg_dir)) {
            result = IVR_EINVAL;
            goto done;
        }
        rc = ivr_content_read_file(file_path, out->max_xml_bytes, &out->ccxml);
        if (rc == -2) {
            result = IVR_ENOSPC;
            goto done;
        }
        if (rc < 0) {
            goto done;
        }
    }
    for (size_t i = 0; i < out->dialog_count; i++) {
        snprintf(file_path, sizeof(file_path), "%s/%s", pkg_dir,
                 out->dialogs[i].src.data);
        if (!ivr_content_path_within_root(file_path, pkg_dir)) {
            result = IVR_EINVAL;
            goto done;
        }
        rc = ivr_content_read_file(file_path, out->max_xml_bytes,
                                   &out->dialogs[i].xml);
        if (rc == -2) {
            result = IVR_ENOSPC;
            goto done;
        }
        if (rc < 0) {
            result = IVR_ESTATE;
            goto done;
        }
    }
    result = IVR_OK;

done:
    turbo_free_json(&doc);
    ivr_str_free(&manifest_text);
    if (result != IVR_OK) {
        ivr_content_package_free(out);
    }
    return result;
}

void ivr_content_package_free(ivr_content_package_t *pkg) {
    if (!pkg) {
        return;
    }
    ivr_str_free(&pkg->name);
    ivr_str_free(&pkg->version);
    ivr_str_free(&pkg->rtc_scxml);
    ivr_str_free(&pkg->ccxml);
    for (size_t i = 0; i < pkg->allowed_command_count; i++) {
        ivr_str_free(&pkg->allowed_commands[i]);
    }
    free(pkg->allowed_commands);
    for (size_t i = 0; i < pkg->allowed_event_count; i++) {
        ivr_str_free(&pkg->allowed_events[i]);
    }
    free(pkg->allowed_events);
    for (size_t i = 0; i < pkg->allowed_uri_count; i++) {
        ivr_str_free(&pkg->allowed_uris[i]);
    }
    free(pkg->allowed_uris);
    for (size_t i = 0; i < pkg->required_capability_count; i++) {
        ivr_str_free(&pkg->required_capabilities[i]);
    }
    free(pkg->required_capabilities);
    for (size_t i = 0; i < pkg->dialog_count; i++) {
        ivr_str_free(&pkg->dialogs[i].src);
        ivr_str_free(&pkg->dialogs[i].xml);
    }
    free(pkg->dialogs);
    for (size_t i = 0; i < pkg->command_map_count; i++) {
        ivr_str_free(&pkg->command_map[i].input);
        ivr_str_free(&pkg->command_map[i].command);
    }
    free(pkg->command_map);
    memset(pkg, 0, sizeof(*pkg));
}

/* ------------------------------------------------------------------ */
/* validation helpers                                                  */
/* ------------------------------------------------------------------ */

static int list_has(const ivr_str_t *arr, size_t count, const char *value) {
    if (!value) {
        return 0;
    }
    for (size_t i = 0; i < count; i++) {
        if (arr[i].size == strlen(value) &&
            memcmp(arr[i].data, value, arr[i].size) == 0) {
            return 1;
        }
    }
    return 0;
}

int ivr_content_command_allowed(const ivr_content_package_t *pkg,
                                const char *command) {
    return pkg && list_has(pkg->allowed_commands, pkg->allowed_command_count,
                           command);
}

int ivr_content_event_allowed(const ivr_content_package_t *pkg,
                              const char *event_name) {
    return pkg && list_has(pkg->allowed_events, pkg->allowed_event_count,
                           event_name);
}

int ivr_content_uri_allowed(const ivr_content_package_t *pkg, const char *uri) {
    return pkg && list_has(pkg->allowed_uris, pkg->allowed_uri_count, uri);
}

static int csv_has_capability(const char *csv, const char *capability) {
    const char *p = csv;
    size_t wanted;
    if (!csv || !capability || capability[0] == '\0') {
        return 0;
    }
    wanted = strlen(capability);
    while (*p) {
        const char *end = strchr(p, ',');
        size_t length = end ? (size_t)(end - p) : strlen(p);
        if (length == wanted && memcmp(p, capability, wanted) == 0) {
            return 1;
        }
        if (!end) {
            break;
        }
        p = end + 1;
    }
    return 0;
}

int ivr_content_capabilities_satisfied(const ivr_content_package_t *pkg,
                                       const char *available_csv) {
    if (!pkg || !available_csv) {
        return IVR_EINVAL;
    }
    for (size_t i = 0; i < pkg->required_capability_count; i++) {
        if (!csv_has_capability(available_csv,
                                pkg->required_capabilities[i].data)) {
            return IVR_ESTATE;
        }
    }
    return IVR_OK;
}

const char *ivr_content_command_for_input(const ivr_content_package_t *pkg,
                                          const char *input) {
    if (!pkg || !input) {
        return NULL;
    }
    for (size_t i = 0; i < pkg->command_map_count; i++) {
        if (pkg->command_map[i].input.size == strlen(input) &&
            memcmp(pkg->command_map[i].input.data, input,
                   pkg->command_map[i].input.size) == 0) {
            return pkg->command_map[i].command.data;
        }
    }
    return NULL;
}

const ivr_content_dialog_t *ivr_content_find_dialog(
    const ivr_content_package_t *pkg, const char *src) {
    if (!pkg || !src) {
        return NULL;
    }
    for (size_t i = 0; i < pkg->dialog_count; i++) {
        if (pkg->dialogs[i].src.size == strlen(src) &&
            memcmp(pkg->dialogs[i].src.data, src, pkg->dialogs[i].src.size) ==
                0) {
            return &pkg->dialogs[i];
        }
    }
    return NULL;
}
