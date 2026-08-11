#ifndef TURBO_MEDIA_IVR_CONTENT_H
#define TURBO_MEDIA_IVR_CONTENT_H

/**
 * @file ivr_content.h
 * @brief Versioned IVR content package loading and validation.
 *
 * A content package maps one versioned IVR behavior to a manifest plus its
 * XML documents (rtc_session.scxml, call_control.ccxml, dialog VXML) and
 * trace files. The worker validates the manifest and every XML document
 * before creating a session; arbitrary network URIs are never accepted.
 */

#include "ivr_internal.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    ivr_str_t src; /* dialog source name, e.g. "conference_menu.vxml" */
    ivr_str_t xml; /* full VXML document */
} ivr_content_dialog_t;

typedef struct {
    ivr_str_t input;  /* recognized input, e.g. "1" */
    ivr_str_t command; /* business command, e.g. "conference.join" */
} ivr_content_cmdmap_entry_t;

typedef struct {
    ivr_str_t name; /* package name */
    ivr_str_t version;
    size_t max_xml_bytes;
    size_t max_json_bytes;
    uint64_t default_timeout_ms; /* input window timeout */

    ivr_str_t *allowed_commands;
    size_t allowed_command_count;
    ivr_str_t *allowed_events;
    size_t allowed_event_count;
    ivr_str_t *allowed_uris; /* ivr://command/... allowlist */
    size_t allowed_uri_count;
    ivr_str_t *required_capabilities;
    size_t required_capability_count;

    ivr_str_t rtc_scxml; /* canonical RTC control spec */
    ivr_str_t ccxml;     /* call control document */
    ivr_content_dialog_t *dialogs;
    size_t dialog_count;
    ivr_content_cmdmap_entry_t *command_map; /* input -> business command */
    size_t command_map_count;
} ivr_content_package_t;

/* Load and validate a content package. Returns IVR_OK or a distinct error:
   IVR_EINVAL (missing/invalid manifest), IVR_ENOSPC (oversized), IVR_ESTATE
   (unexpected document/field). */
int ivr_content_package_load(const char *content_root,
                             const char *package_name,
                             ivr_content_package_t *out);

void ivr_content_package_free(ivr_content_package_t *pkg);

int ivr_content_command_allowed(const ivr_content_package_t *pkg,
                                const char *command);
int ivr_content_event_allowed(const ivr_content_package_t *pkg,
                              const char *event_name);
int ivr_content_uri_allowed(const ivr_content_package_t *pkg,
                            const char *uri);
int ivr_content_capabilities_satisfied(const ivr_content_package_t *pkg,
                                       const char *available_csv);
const ivr_content_dialog_t *ivr_content_find_dialog(
    const ivr_content_package_t *pkg, const char *src);
/* Look up the business command for a recognized input; NULL when unmapped. */
const char *ivr_content_command_for_input(const ivr_content_package_t *pkg,
                                          const char *input);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_MEDIA_IVR_CONTENT_H */

