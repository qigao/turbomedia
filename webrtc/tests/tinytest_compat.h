#ifndef TURBORTC_TINYTEST_COMPAT_H
#define TURBORTC_TINYTEST_COMPAT_H

#include "tinytest.h"

#include <stdint.h>
#include <stddef.h>

#define TEST_PASS() check_true(1)
#define TEST_FAIL() __BDD_CHECK__(0, "forced failure")
#define TEST_FAIL_MESSAGE(msg) __BDD_CHECK__(0, "%s", (msg))
#define TEST_IGNORE() tt_skip("Test was skipped")
#define TEST_IGNORE_MESSAGE(msg) tt_skip(msg)

#define TEST_ASSERT_TRUE(actual) check_true(actual)
#define TEST_ASSERT_FALSE(actual) check_false(actual)
#define TEST_ASSERT_NULL(ptr) check_null(ptr)
#define TEST_ASSERT_NOT_NULL(ptr) check_not_null(ptr)

#define TEST_ASSERT_EQUAL(expected, actual) check((actual) == (expected))
#define TEST_ASSERT_NOT_EQUAL(expected, actual) check((actual) != (expected))

#define TEST_ASSERT_EQUAL_INT(expected, actual) check_int_eq((actual), (expected))
#define TEST_ASSERT_EQUAL_INT64(expected, actual) check((long long)(actual) == (long long)(expected))

#define TEST_ASSERT_EQUAL_UINT8(expected, actual) check((uint8_t)(actual) == (uint8_t)(expected))
#define TEST_ASSERT_EQUAL_UINT16(expected, actual) check((uint16_t)(actual) == (uint16_t)(expected))
#define TEST_ASSERT_EQUAL_UINT32(expected, actual) check((uint32_t)(actual) == (uint32_t)(expected))
#define TEST_ASSERT_EQUAL_UINT64(expected, actual) check((uint64_t)(actual) == (uint64_t)(expected))

#define TEST_ASSERT_EQUAL_size_t(expected, actual) check_size_eq((actual), (expected))
#define TEST_ASSERT_EQUAL_STRING(expected, actual) check_str_eq((actual), (expected))
#define TEST_ASSERT_EQUAL_MEMORY(expected, actual, len) check_mem_eq((actual), (expected), (len))

#define TEST_ASSERT_GREATER_THAN(threshold, actual) check((actual) > (threshold))
#define TEST_ASSERT_LESS_THAN(threshold, actual) check((actual) < (threshold))
#define TEST_ASSERT_GREATER_OR_EQUAL(threshold, actual) check((actual) >= (threshold))

#define TT_TEST(fn) it(#fn) { fn(); }

#endif
