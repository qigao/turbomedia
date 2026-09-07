#ifndef TURBO_MEDIA_CONTAINER_IO_H
#define TURBO_MEDIA_CONTAINER_IO_H

#include <stddef.h>
#include <stdint.h>

#include <salts_fs.h>
#include <cstl/vec.h>

typedef enum {
    TURBO_CONTAINER_IO_NONE = 0,
    TURBO_CONTAINER_IO_FILE,
    TURBO_CONTAINER_IO_MEMORY_READ,
    TURBO_CONTAINER_IO_MEMORY_WRITE
} turbo_container_io_mode_t;

typedef struct {
    turbo_container_io_mode_t mode;
    salts_file_t file;
    const uint8_t *input;
    size_t input_size;
    vec_t output;
    size_t position;
    int error;
} turbo_container_io_t;

int turbo_container_io_open_reader(turbo_container_io_t *io, const char *path,
                                   const uint8_t *data, size_t size);
int turbo_container_io_open_writer(turbo_container_io_t *io, const char *path);
void turbo_container_io_close(turbo_container_io_t *io);
int turbo_container_io_flush(turbo_container_io_t *io);

int turbo_container_io_read(void *param, void *data, uint64_t bytes);
int turbo_container_io_read_some(turbo_container_io_t *io, void *data,
                                 size_t capacity, size_t *bytes_read);
int turbo_container_io_write(void *param, const void *data, uint64_t bytes);
int turbo_container_io_seek(void *param, int64_t offset);
int64_t turbo_container_io_tell(void *param);
int64_t turbo_container_io_size(turbo_container_io_t *io);

int turbo_container_io_get_memory(turbo_container_io_t *io, uint8_t **data,
                                  size_t *size);

#endif
