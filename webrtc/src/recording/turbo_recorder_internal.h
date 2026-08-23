#ifndef TURBO_RECORDER_INTERNAL_H
#define TURBO_RECORDER_INTERNAL_H

#include "turbo_recorder.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TURBO_RECORDER_TEST_IO_OPEN = 0,
    TURBO_RECORDER_TEST_IO_WRITE = 1,
    TURBO_RECORDER_TEST_IO_FLUSH = 2,
    TURBO_RECORDER_TEST_IO_SEEK = 3,
    TURBO_RECORDER_TEST_IO_TELL = 4,
    TURBO_RECORDER_TEST_IO_CLOSE = 5,
} turbo_recorder_test_io_op_t;

TURBO_MEDIA_API void turbo_recorder_test_reset_io_failures(void);
TURBO_MEDIA_API void turbo_recorder_test_fail_io_once(turbo_recorder_test_io_op_t op, int call_index);

#ifdef __cplusplus
}
#endif

#endif
