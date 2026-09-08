#ifndef TURBO_MEDIA_SIP_STRING_VIEW_H
#define TURBO_MEDIA_SIP_STRING_VIEW_H

#include "salts_vstr.h"

#include <stdlib.h>
#include <string.h>

static inline int sip_sv_valid(const vstr *value) {
  return value != NULL && value->data != NULL && value->len > 0U;
}

static inline const char *sip_sv_find_char(const vstr *value, int needle) {
  size_t offset;

  if (!value) return NULL;
  offset = vstr_find_char(*value, (char)needle);
  return offset == VSTR_NPOS ? NULL : value->data + offset;
}

static inline size_t sip_sv_copy(const vstr *value, char *output, size_t capacity) {
  size_t count;

  if (!value || !output) return value ? value->len : 0U;
  count = value->len < capacity ? value->len : capacity;
  if (count > 0U) memcpy(output, value->data, count);
  if (count < capacity) output[count] = '\0';
  return value->len;
}

static inline void sip_sv_trim(vstr *value, const char *characters) {
  if (value) *value = vstr_trim(*value, characters);
}

static inline int sip_sv_compare_cstr(const vstr *value, const char *text) {
  return value && vstr_eq(*value, vstr_from_cstr(text)) ? 0 : -1;
}

static inline int sip_sv_equal(const vstr *left, const vstr *right) {
  return left && right ? vstr_eq(*left, *right) : 0;
}

static inline int sip_sv_compare_cstr_ci(const vstr *value, const char *text) {
  return value && vstr_ieq(*value, vstr_from_cstr(text)) ? 0 : -1;
}

static inline int sip_cstr_casecmp(const char *left, const char *right) {
  return left && right && vstr_ieq(vstr_from_cstr(left), vstr_from_cstr(right)) ? 0 : -1;
}

static inline int sip_mem_casecmp(const char *left, const char *right, size_t length) {
  if (length == 0U) return 0;
  return left && right &&
                 vstr_ieq(vstr_from_buf(left, length), vstr_from_buf(right, length))
             ? 0
             : -1;
}

static inline int sip_sv_starts_with(const vstr *value, const char *prefix) {
  return value ? vstr_starts_with(*value, vstr_from_cstr(prefix)) : 0;
}

static inline long sip_sv_to_long(const vstr *value, char **endptr, int base) {
  char *copy;
  char *end;
  long result;

  if (!value || !value->data || value->len == 0U) {
    if (endptr) *endptr = value ? (char *)value->data : NULL;
    return 0L;
  }
  copy = vstr_to_cstr(*value);
  if (!copy) {
    if (endptr) *endptr = (char *)value->data;
    return 0L;
  }
  result = strtol(copy, &end, base);
  if (endptr) *endptr = (char *)value->data + (end - copy);
  free(copy);
  return result;
}

static inline long long sip_sv_to_long_long(const vstr *value, char **endptr, int base) {
  char *copy;
  char *end;
  long long result;

  if (!value || !value->data || value->len == 0U) {
    if (endptr) *endptr = value ? (char *)value->data : NULL;
    return 0LL;
  }
  copy = vstr_to_cstr(*value);
  if (!copy) {
    if (endptr) *endptr = (char *)value->data;
    return 0LL;
  }
  result = strtoll(copy, &end, base);
  if (endptr) *endptr = (char *)value->data + (end - copy);
  free(copy);
  return result;
}

static inline double sip_sv_to_double(const vstr *value, char **endptr) {
  char *copy;
  char *end;
  double result;

  if (!value || !value->data || value->len == 0U) {
    if (endptr) *endptr = value ? (char *)value->data : NULL;
    return 0.0;
  }
  copy = vstr_to_cstr(*value);
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
