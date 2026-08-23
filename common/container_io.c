#include "container_io.h"
#include "stl_status.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <turbo_error.h>

static int container_io_fail(turbo_container_io_t *io, int error) {
    if (io && io->error == TURBO_OK) io->error = error;
    return error;
}

static int container_io_file_read(turbo_container_io_t *io, void *data,
                                  size_t bytes) {
    uint8_t *cursor = (uint8_t *)data;
    size_t remaining = bytes;

    while (remaining > 0) {
        size_t chunk = remaining > INT_MAX ? INT_MAX : remaining;
        int result = turbo_fs_read(io->file, (char *)cursor, chunk);
        if (result < 0) return container_io_fail(io, result);
        if (result == 0) return container_io_fail(io, TURBO_EOF);
        cursor += (size_t)result;
        remaining -= (size_t)result;
    }
    return TURBO_OK;
}

static int container_io_file_write(turbo_container_io_t *io, const void *data,
                                   size_t bytes) {
    const uint8_t *cursor = (const uint8_t *)data;
    size_t remaining = bytes;

    while (remaining > 0) {
        size_t chunk = remaining > INT_MAX ? INT_MAX : remaining;
        int result = turbo_fs_write(io->file, (const char *)cursor, chunk);
        if (result < 0) return container_io_fail(io, result);
        if (result == 0) return container_io_fail(io, TURBO_EIO);
        cursor += (size_t)result;
        remaining -= (size_t)result;
    }
    return TURBO_OK;
}

int turbo_container_io_open_reader(turbo_container_io_t *io, const char *path,
                                   const uint8_t *data, size_t size) {
    if (!io || (!!path == !!data) || (!path && size == 0)) return TURBO_EINVAL;

    memset(io, 0, sizeof(*io));
    io->file = TURBO_INVALID_FILE;
    io->error = TURBO_OK;
    if (path) {
        io->file = turbo_fs_open(path, TURBO_FS_O_RDONLY, 0);
        if (io->file == TURBO_INVALID_FILE) return TURBO_EIO;
        io->mode = TURBO_CONTAINER_IO_FILE;
    } else {
        io->mode = TURBO_CONTAINER_IO_MEMORY_READ;
        io->input = data;
        io->input_size = size;
    }
    return TURBO_OK;
}

int turbo_container_io_open_writer(turbo_container_io_t *io, const char *path) {
    int result;

    if (!io) return TURBO_EINVAL;
    memset(io, 0, sizeof(*io));
    io->file = TURBO_INVALID_FILE;
    io->error = TURBO_OK;
    if (path) {
        io->file = turbo_fs_open(path, TURBO_FS_O_RDWR | TURBO_FS_O_CREAT |
                                           TURBO_FS_O_TRUNC,
                                 TURBO_FS_DEFAULT_MODE);
        if (io->file == TURBO_INVALID_FILE) return TURBO_EIO;
        io->mode = TURBO_CONTAINER_IO_FILE;
        return TURBO_OK;
    }

    result = turbo_media_stl_status_to_error(vec_init_bytes(
        &io->output, sizeof(uint8_t), CMETA_ALIGNOF(uint8_t), SIZE_MAX));
    if (result != TURBO_OK) return result;
    io->mode = TURBO_CONTAINER_IO_MEMORY_WRITE;
    return TURBO_OK;
}

void turbo_container_io_close(turbo_container_io_t *io) {
    if (!io) return;
    if (io->file != TURBO_INVALID_FILE) {
        (void)turbo_fs_close(io->file);
    }
    if (io->mode == TURBO_CONTAINER_IO_MEMORY_WRITE) {
        vec_destroy(&io->output);
    }
    memset(io, 0, sizeof(*io));
    io->file = TURBO_INVALID_FILE;
}

int turbo_container_io_flush(turbo_container_io_t *io) {
    int result;

    if (!io) return TURBO_EINVAL;
    if (io->error != TURBO_OK) return io->error;
    if (io->mode != TURBO_CONTAINER_IO_FILE) return TURBO_OK;
    result = turbo_fs_fsync(io->file);
    return result == TURBO_OK ? TURBO_OK : container_io_fail(io, result);
}

int turbo_container_io_read(void *param, void *data, uint64_t bytes) {
    turbo_container_io_t *io = (turbo_container_io_t *)param;
    size_t count;

    if (!io || (!data && bytes > 0) || bytes > SIZE_MAX)
        return container_io_fail(io, TURBO_EINVAL);
    count = (size_t)bytes;
    if (io->mode == TURBO_CONTAINER_IO_FILE)
        return container_io_file_read(io, data, count);

    if (io->mode == TURBO_CONTAINER_IO_MEMORY_READ) {
        if (io->position > io->input_size)
            return container_io_fail(io, TURBO_ERANGE);
        if (count > io->input_size - io->position)
            return container_io_fail(io, TURBO_EOF);
        memcpy(data, io->input + io->position, count);
        io->position += count;
        return TURBO_OK;
    }

    if (io->mode == TURBO_CONTAINER_IO_MEMORY_WRITE) {
        size_t size = vec_size(&io->output);
        if (io->position > size || count > size - io->position)
            return container_io_fail(io, TURBO_EOF);
        memcpy(data, (const uint8_t *)vec_data_const(&io->output) + io->position,
               count);
        io->position += count;
        return TURBO_OK;
    }
    return container_io_fail(io, TURBO_EINVAL);
}

int turbo_container_io_read_some(turbo_container_io_t *io, void *data,
                                 size_t capacity, size_t *bytes_read) {
    size_t available;
    size_t count;

    if (!io || (!data && capacity > 0) || !bytes_read)
        return container_io_fail(io, TURBO_EINVAL);
    *bytes_read = 0;
    if (capacity == 0) return TURBO_OK;

    if (io->mode == TURBO_CONTAINER_IO_FILE) {
        size_t chunk = capacity > INT_MAX ? INT_MAX : capacity;
        int result = turbo_fs_read(io->file, (char *)data, chunk);
        if (result < 0) return container_io_fail(io, result);
        *bytes_read = (size_t)result;
        return TURBO_OK;
    }

    if (io->mode == TURBO_CONTAINER_IO_MEMORY_READ) {
        if (io->position > io->input_size)
            return container_io_fail(io, TURBO_ERANGE);
        available = io->input_size - io->position;
        count = capacity < available ? capacity : available;
        if (count > 0) memcpy(data, io->input + io->position, count);
    } else if (io->mode == TURBO_CONTAINER_IO_MEMORY_WRITE) {
        size_t size = vec_size(&io->output);
        if (io->position > size) return container_io_fail(io, TURBO_ERANGE);
        available = size - io->position;
        count = capacity < available ? capacity : available;
        if (count > 0) {
            memcpy(data,
                   (const uint8_t *)vec_data_const(&io->output) + io->position,
                   count);
        }
    } else {
        return container_io_fail(io, TURBO_EINVAL);
    }

    io->position += count;
    *bytes_read = count;
    return TURBO_OK;
}

int turbo_container_io_write(void *param, const void *data, uint64_t bytes) {
    turbo_container_io_t *io = (turbo_container_io_t *)param;
    size_t count;
    size_t required;
    int result;

    if (!io || (!data && bytes > 0) || bytes > SIZE_MAX)
        return container_io_fail(io, TURBO_EINVAL);
    count = (size_t)bytes;
    if (io->mode == TURBO_CONTAINER_IO_FILE)
        return container_io_file_write(io, data, count);
    if (io->mode != TURBO_CONTAINER_IO_MEMORY_WRITE)
        return container_io_fail(io, TURBO_EPERM);
    if (count > SIZE_MAX - io->position)
        return container_io_fail(io, TURBO_EFBIG);

    required = io->position + count;
    if (required > vec_size(&io->output)) {
        result = turbo_media_stl_status_to_error(vec_resize(&io->output, required));
        if (result != TURBO_OK) return container_io_fail(io, result);
    }
    memcpy((uint8_t *)vec_data(&io->output) + io->position, data, count);
    io->position = required;
    return TURBO_OK;
}

int turbo_container_io_seek(void *param, int64_t offset) {
    turbo_container_io_t *io = (turbo_container_io_t *)param;
    size_t size;
    size_t target;
    int64_t result;

    if (!io) return TURBO_EINVAL;
    if (io->mode == TURBO_CONTAINER_IO_FILE) {
        result = turbo_fs_seek(io->file, offset,
                               offset >= 0 ? SEEK_SET : SEEK_END);
        if (result < 0) return container_io_fail(io, (int)result);
        return TURBO_OK;
    }

    size = io->mode == TURBO_CONTAINER_IO_MEMORY_READ
               ? io->input_size
               : vec_size(&io->output);
    if (offset >= 0) {
        if ((uint64_t)offset > SIZE_MAX)
            return container_io_fail(io, TURBO_EFBIG);
        target = (size_t)offset;
    } else {
        uint64_t distance = (uint64_t)(-(offset + 1)) + 1;
        if (distance > size) return container_io_fail(io, TURBO_ERANGE);
        target = size - (size_t)distance;
    }

    if (io->mode == TURBO_CONTAINER_IO_MEMORY_READ && target > size)
        return container_io_fail(io, TURBO_ERANGE);
    if (io->mode == TURBO_CONTAINER_IO_MEMORY_WRITE && target > size) {
        int resize_result = turbo_media_stl_status_to_error(
            vec_resize(&io->output, target));
        if (resize_result != TURBO_OK)
            return container_io_fail(io, resize_result);
    }
    io->position = target;
    return TURBO_OK;
}

int64_t turbo_container_io_tell(void *param) {
    turbo_container_io_t *io = (turbo_container_io_t *)param;
    int64_t result;

    if (!io) return TURBO_EINVAL;
    if (io->mode != TURBO_CONTAINER_IO_FILE) {
        if (io->position > INT64_MAX) return container_io_fail(io, TURBO_EFBIG);
        return (int64_t)io->position;
    }
    result = turbo_fs_tell(io->file);
    if (result < 0) container_io_fail(io, (int)result);
    return result;
}

int64_t turbo_container_io_size(turbo_container_io_t *io) {
    int64_t current;
    int64_t size;

    if (!io) return TURBO_EINVAL;
    if (io->mode == TURBO_CONTAINER_IO_MEMORY_READ) {
        if (io->input_size > INT64_MAX) return container_io_fail(io, TURBO_EFBIG);
        return (int64_t)io->input_size;
    }
    if (io->mode == TURBO_CONTAINER_IO_MEMORY_WRITE) {
        size_t output_size = vec_size(&io->output);
        if (output_size > INT64_MAX) return container_io_fail(io, TURBO_EFBIG);
        return (int64_t)output_size;
    }
    if (io->mode != TURBO_CONTAINER_IO_FILE) return TURBO_EINVAL;

    current = turbo_fs_tell(io->file);
    if (current < 0) return container_io_fail(io, (int)current);
    size = turbo_fs_seek(io->file, 0, SEEK_END);
    if (size < 0) return container_io_fail(io, (int)size);
    if (turbo_fs_seek(io->file, current, SEEK_SET) < 0)
        return container_io_fail(io, TURBO_EIO);
    return size;
}

int turbo_container_io_get_memory(turbo_container_io_t *io, uint8_t **data,
                                  size_t *size) {
    if (!io || !data || !size || io->mode != TURBO_CONTAINER_IO_MEMORY_WRITE)
        return TURBO_EINVAL;
    if (io->error != TURBO_OK) return io->error;
    *data = (uint8_t *)vec_data(&io->output);
    *size = vec_size(&io->output);
    return TURBO_OK;
}
