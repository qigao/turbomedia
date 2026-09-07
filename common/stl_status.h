#ifndef TURBO_MEDIA_STL_STATUS_H
#define TURBO_MEDIA_STL_STATUS_H

#include <salts_error.h>
#include <cstl/status.h>

static inline int turbo_media_stl_status_to_error(stl_status status) {
    switch (status) {
        case STL_OK:
            return SALTS_OK;
        case STL_OUT_OF_MEMORY:
            return SALTS_ENOMEM;
        case STL_CAPACITY_EXCEEDED:
            return SALTS_EFBIG;
        case STL_EMPTY:
        case STL_NOT_FOUND:
            return SALTS_ENOENT;
        case STL_INVALID_ARGUMENT:
        case STL_TYPE_MISMATCH:
        case STL_TRAIT_MISSING:
        default:
            return SALTS_EINVAL;
    }
}

#endif
