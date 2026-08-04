#ifndef TURBO_MEDIA_SIP_STRING_VIEW_H
#define TURBO_MEDIA_SIP_STRING_VIEW_H

#include "turbo_str_view.h"

#include <stdlib.h>
#include <string.h>

static inline int sip_sv_valid(const tstr_v *value) {
  return value != NULL && value->data != NULL && value->len > 0U;
}

static inline const char *sip_sv_find_char(const tstr_v *value, int needle) {
  size_t offset;

  if (!value) return NULL;
  offset = tstr_v_find_char(*value, (char)needle);
  return offset == TSTR_V_NPOS ? NULL : value->data + offset;
}

static inline size_t sip_sv_copy(const tstr_v *value, char *output, size_t capacity) {
  size_t count;

  if (!value || !output) return value ? value->len : 0U;
  count = value->len < capacity ? value->len : capacity;
  if (count > 0U) memcpy(output, value->data, count);
  if (count < capacity) output[count] = '\0';
  return value->len;
}

static inline void sip_sv_trim(tstr_v *value, const char *characters) {
  if (value) *value = tstr_v_trim(*value, characters);
}

static inline int sip_sv_compare_cstr(const tstr_v *value, const char *text) {
  return value && tstr_v_eq(*value, tstr_v_from_cstr(text)) ? 0 : -1;
}

static inline int sip_sv_equal(const tstr_v *left, const tstr_v *right) {
  return left && right ? tstr_v_eq(*left, *right) : 0;
}

static inline int sip_sv_compare_cstr_ci(const tstr_v *value, const char *text) {
  return value && tstr_v_ieq(*value, tstr_v_from_cstr(text)) ? 0 : -1;
}

static inline int sip_cstr_casecmp(const char *left, const char *right) {
  return left && right && tstr_v_ieq(tstr_v_from_cstr(left), tstr_v_from_cstr(right)) ? 0 : -1;
}

static inline int sip_mem_casecmp(const char *left, const char *right, size_t length) {
  if (length == 0U) return 0;
  return left && right &&
                 tstr_v_ieq(tstr_v_from_buf(left, length), tstr_v_from_buf(right, length))
             ? 0
             : -1;
}

static inline int sip_sv_starts_with(const tstr_v *value, const char *prefix) {
  return value ? tstr_v_starts_with(*value, tstr_v_from_cstr(prefix)) : 0;
}

static inline long sip_sv_to_long(const tstr_v *value, char **endptr, int base) {
  char *copy;
  char *end;
  long result;

  if (!value || !value->data || value->len == 0U) {
    if (endptr) *endptr = value ? (char *)value->data : NULL;
    return 0L;
  }
  copy = tstr_v_to_cstr(*value);
  if (!copy) {
    if (endptr) *endptr = (char *)value->data;
    return 0L;
  }
  result = strtol(copy, &end, base);
  if (endptr) *endptr = (char *)value->data + (end - copy);
  free(copy);
  return result;
}

static inline long long sip_sv_to_long_long(const tstr_v *value, char **endptr, int base) {
  char *copy;
  char *end;
  long long result;

  if (!value || !value->data || value->len == 0U) {
    if (endptr) *endptr = value ? (char *)value->data : NULL;
    return 0LL;
  }
  copy = tstr_v_to_cstr(*value);
  if (!copy) {
    if (endptr) *endptr = (char *)value->data;
    return 0LL;
  }
  result = strtoll(copy, &end, base);
  if (endptr) *endptr = (char *)value->data + (end - copy);
  free(copy);
  return result;
}

static inline double sip_sv_to_double(const tstr_v *value, char **endptr) {
  char *copy;
  char *end;
  double result;

  if (!value || !value->data || value->len == 0U) {
    if (endptr) *endptr = value ? (char *)value->data : NULL;
    return 0.0;
  }
  copy = tstr_v_to_cstr(*value);
  if (!copy) {
    if (endptr) *endptr = (char *)value->data;
    return 0.0;
  }
  result = strtod(copy, &end);
  if (endptr) *endptr = (char *)value->data + (end - copy);
  free(copy);
  return result;
}

#endif
