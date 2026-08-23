#ifndef TURBO_MEDIA_STL_STATUS_H
#define TURBO_MEDIA_STL_STATUS_H

#include <turbo_error.h>
#include <turbostl/status.h>

static inline int turbo_media_stl_status_to_error(stl_status status) {
    switch (status) {
        case STL_OK:
            return TURBO_OK;
        case STL_OUT_OF_MEMORY:
            return TURBO_ENOMEM;
        case STL_CAPACITY_EXCEEDED:
            return TURBO_EFBIG;
        case STL_EMPTY:
        case STL_NOT_FOUND:
            return TURBO_ENOENT;
        case STL_INVALID_ARGUMENT:
        case STL_TYPE_MISMATCH:
        case STL_TRAIT_MISSING:
        default:
            return TURBO_EINVAL;
    }
}

#endif
