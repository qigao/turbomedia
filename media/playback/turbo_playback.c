/**
 * TurboNet Audio Playback Implementation
 *
 * Uses miniaudio for cross-platform audio output
 */
#include "miniaudio.h"

#include "turbo_playback.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =============================================================================
 * Internal Structures
 * ============================================================================= */

typedef enum {
  PLAYBACK_MODE_STREAMING, /* Callback-based or push-based */
  PLAYBACK_MODE_FILE       /* File decoder */
} playback_mode_t;

typedef struct {
  uint8_t *buffer;
  size_t capacity;
  size_t read_pos;
  size_t write_pos;
  size_t count;
} ring_buffer_t;

/* Queue entry for playlist */
#define TURBO_PLAYBACK_MAX_QUEUE 64

typedef struct {
  char *filepaths[TURBO_PLAYBACK_MAX_QUEUE];
  int count;
  int current;
  int loop_queue;
} playback_queue_t;

struct turbo_playback_s {
  ma_device device;
  ma_device_config device_config;
  ma_decoder decoder; /* For file playback */

  playback_mode_t mode;
  turbo_playback_state_t state;
  turbo_playback_config_t config;

  /* Callbacks */
  turbo_playback_data_cb data_cb;
  turbo_playback_state_cb state_cb;
  turbo_playback_complete_cb complete_cb;
  void *user_data;

  /* Ring buffer for push-based streaming */
  ring_buffer_t ring;
  ma_mutex ring_mutex;

  /* Volume */
  float volume;
  int looping;

  /* File info */
  uint64_t total_frames;
  uint32_t sample_rate;
  uint32_t channels;

  /* Playlist queue */
  playback_queue_t queue;
  volatile int switching_file; /* Flag to indicate file switch in progress */
};

/* =============================================================================
 * Ring Buffer
 * ============================================================================= */

static int ring_buffer_init(ring_buffer_t *rb, size_t capacity) {
  rb->buffer = (uint8_t *)malloc(capacity);
  if (!rb->buffer)
    return -1;
  rb->capacity = capacity;
  rb->read_pos = 0;
  rb->write_pos = 0;
  rb->count = 0;
  return 0;
}

static void ring_buffer_free(ring_buffer_t *rb) {
  if (rb->buffer) {
    free(rb->buffer);
    rb->buffer = NULL;
  }
}

static size_t ring_buffer_write(ring_buffer_t *rb, const void *data, size_t len) {
  if (!rb->buffer || !data || len == 0)
    return 0;

  size_t available = rb->capacity - rb->count;
  size_t to_write = (len < available) ? len : available;

  const uint8_t *src = (const uint8_t *)data;
  size_t first_chunk = rb->capacity - rb->write_pos;

  if (to_write <= first_chunk) {
    memcpy(rb->buffer + rb->write_pos, src, to_write);
  } else {
    memcpy(rb->buffer + rb->write_pos, src, first_chunk);
    memcpy(rb->buffer, src + first_chunk, to_write - first_chunk);
  }

  rb->write_pos = (rb->write_pos + to_write) % rb->capacity;
  rb->count += to_write;
  return to_write;
}

static size_t ring_buffer_read(ring_buffer_t *rb, void *data, size_t len) {
  if (!rb->buffer || !data || len == 0)
    return 0;

  size_t to_read = (len < rb->count) ? len : rb->count;
  uint8_t *dst = (uint8_t *)data;
  size_t first_chunk = rb->capacity - rb->read_pos;

  if (to_read <= first_chunk) {
    memcpy(dst, rb->buffer + rb->read_pos, to_read);
  } else {
    memcpy(dst, rb->buffer + rb->read_pos, first_chunk);
    memcpy(dst + first_chunk, rb->buffer, to_read - first_chunk);
  }

  rb->read_pos = (rb->read_pos + to_read) % rb->capacity;
  rb->count -= to_read;
  return to_read;
}

/* =============================================================================
 * Queue Helper - Switch to next file
 * ============================================================================= */

static int queue_switch_to_next(turbo_playback_t *playback) {
  if (!playback || playback->queue.count == 0)
    return -1;

  int next_index = playback->queue.current + 1;

  /* Check if we've reached the end */
  if (next_index >= playback->queue.count) {
    if (playback->queue.loop_queue) {
      next_index = 0;
    } else {
      return -1; /* Queue finished */
    }
  }

  /* Close current decoder and open next file */
  ma_decoder_uninit(&playback->decoder);

  ma_decoder_config decoder_config =
      ma_decoder_config_init(playback->device.playback.format, playback->device.playback.channels,
                             playback->device.sampleRate);

  if (ma_decoder_init_file(playback->queue.filepaths[next_index], &decoder_config,
                           &playback->decoder) != MA_SUCCESS) {
    return -1;
  }

  playback->queue.current = next_index;

  /* Update duration */
  ma_uint64 total_frames;
  if (ma_decoder_get_length_in_pcm_frames(&playback->decoder, &total_frames) == MA_SUCCESS) {
    playback->total_frames = total_frames;
  }

  return 0;
}

/* =============================================================================
 * miniaudio Callbacks
 * ============================================================================= */

static void playback_data_callback(ma_device *pDevice, void *pOutput, const void *pInput,
                                   ma_uint32 frameCount) {
  turbo_playback_t *playback = (turbo_playback_t *)pDevice->pUserData;
  (void)pInput;

  if (!playback || playback->state != TURBO_PLAYBACK_STATE_PLAYING) {
    memset(pOutput, 0,
           frameCount *
               ma_get_bytes_per_frame(pDevice->playback.format, pDevice->playback.channels));
    return;
  }

  size_t frames_read = 0;
  size_t bytes_per_frame =
      ma_get_bytes_per_frame(pDevice->playback.format, pDevice->playback.channels);

  if (playback->mode == PLAYBACK_MODE_FILE) {
    /* File playback - read from decoder */
    ma_uint64 frames;
    ma_decoder_read_pcm_frames(&playback->decoder, pOutput, frameCount, &frames);
    frames_read = (size_t)frames;

    /* Handle file completion */
    if (frames_read < frameCount) {
      /* Try to advance to next file in queue */
      if (playback->queue.count > 0 && queue_switch_to_next(playback) == 0) {
        /* Successfully switched to next file, fill remaining frames */
        ma_uint64 extra_frames;
        ma_decoder_read_pcm_frames(&playback->decoder,
                                   (uint8_t *)pOutput + frames_read * bytes_per_frame,
                                   frameCount - frames_read, &extra_frames);
        frames_read += (size_t)extra_frames;
      } else if (playback->looping) {
        /* Single file looping */
        ma_decoder_seek_to_pcm_frame(&playback->decoder, 0);
        ma_uint64 extra_frames;
        ma_decoder_read_pcm_frames(&playback->decoder,
                                   (uint8_t *)pOutput + frames_read * bytes_per_frame,
                                   frameCount - frames_read, &extra_frames);
        frames_read += (size_t)extra_frames;
      } else if (playback->complete_cb) {
        /* All done - notify completion */
        playback->complete_cb(playback, playback->user_data);
      }
    }
  } else {
    /* Streaming playback */
    if (playback->data_cb) {
      /* Callback-based: let user fill the buffer */
      frames_read = playback->data_cb(playback, pOutput, frameCount, playback->user_data);
    } else {
      /* Push-based: read from ring buffer */
      ma_mutex_lock(&playback->ring_mutex);
      size_t bytes_needed = frameCount * bytes_per_frame;
      size_t bytes_read = ring_buffer_read(&playback->ring, pOutput, bytes_needed);
      frames_read = bytes_read / bytes_per_frame;
      ma_mutex_unlock(&playback->ring_mutex);
    }
  }

  /* Zero-fill any remaining frames */
  if (frames_read < frameCount) {
    memset((uint8_t *)pOutput + frames_read * bytes_per_frame, 0,
           (frameCount - frames_read) * bytes_per_frame);
  }

  /* Apply volume */
  if (playback->volume != 1.0f) {
    float *samples = (float *)pOutput;
    size_t sample_count = frameCount * pDevice->playback.channels;
    for (size_t i = 0; i < sample_count; i++) {
      samples[i] *= playback->volume;
    }
  }
}

/* =============================================================================
 * Format Conversion
 * ============================================================================= */

static ma_format playback_format_to_ma(turbo_playback_format_t fmt) {
  switch (fmt) {
  case TURBO_PLAYBACK_FORMAT_S16:
    return ma_format_s16;
  case TURBO_PLAYBACK_FORMAT_S32:
    return ma_format_s32;
  case TURBO_PLAYBACK_FORMAT_F32:
    return ma_format_f32;
  default:
    return ma_format_f32;
  }
}

static int playback_is_valid_sample_rate(int sample_rate) {
  return sample_rate == 8000 || sample_rate == 16000 || sample_rate == 24000 ||
         sample_rate == 48000;
}

static int playback_is_valid_format(turbo_playback_format_t format) {
  return format == TURBO_PLAYBACK_FORMAT_S16 || format == TURBO_PLAYBACK_FORMAT_S32 ||
         format == TURBO_PLAYBACK_FORMAT_F32;
}

static int playback_is_valid_config(const turbo_playback_config_t *config) {
  if (!config)
    return 1;

  return playback_is_valid_sample_rate(config->sample_rate) &&
         (config->channels == 1 || config->channels == 2) &&
         playback_is_valid_format(config->format) &&
         config->buffer_size_ms > 0;
}

/* =============================================================================
 * Device Enumeration
 * ============================================================================= */

int turbo_playback_list_devices(turbo_playback_device_t *devices, int max_count) {
  if (!devices || max_count <= 0)
    return -1;

  ma_context context;
  if (ma_context_init(NULL, 0, NULL, &context) != MA_SUCCESS) {
    return TURBO_PLAYBACK_ERR_DEVICE;
  }

  ma_device_info *playback_infos;
  ma_uint32 playback_count;
  ma_device_info *capture_infos;
  ma_uint32 capture_count;

  if (ma_context_get_devices(&context, &playback_infos, &playback_count, &capture_infos,
                             &capture_count) != MA_SUCCESS) {
    ma_context_uninit(&context);
    return TURBO_PLAYBACK_ERR_DEVICE;
  }

  int count = 0;
  for (ma_uint32 i = 0; i < playback_count && count < max_count; i++) {
    devices[count].index = count;
    strncpy(devices[count].name, playback_infos[i].name, sizeof(devices[count].name) - 1);
    devices[count].name[sizeof(devices[count].name) - 1] = '\0';

    /* Use index as ID for simplicity */
    snprintf(devices[count].id, sizeof(devices[count].id), "%u", (unsigned int)i);
    devices[count].is_default = playback_infos[i].isDefault ? 1 : 0;
    count++;
  }

  ma_context_uninit(&context);
  return count;
}

int turbo_playback_get_default_device(turbo_playback_device_t *device) {
  if (!device)
    return -1;

  turbo_playback_device_t devices[TURBO_PLAYBACK_MAX_DEVICES];
  int count = turbo_playback_list_devices(devices, TURBO_PLAYBACK_MAX_DEVICES);

  for (int i = 0; i < count; i++) {
    if (devices[i].is_default) {
      *device = devices[i];
      return 0;
    }
  }

  /* No default found, use first device */
  if (count > 0) {
    *device = devices[0];
    return 0;
  }

  return TURBO_PLAYBACK_ERR_DEVICE;
}

/* =============================================================================
 * Playback Instance Creation
 * ============================================================================= */

turbo_playback_t *turbo_playback_create(const char *device_id,
                                        const turbo_playback_config_t *config) {
  if (!playback_is_valid_config(config))
    return NULL;

  turbo_playback_t *playback = (turbo_playback_t *)calloc(1, sizeof(turbo_playback_t));
  if (!playback)
    return NULL;

  playback->mode = PLAYBACK_MODE_STREAMING;
  playback->state = TURBO_PLAYBACK_STATE_STOPPED;
  playback->volume = 1.0f;

  /* Store config */
  if (config) {
    playback->config = *config;
  } else {
    playback->config.sample_rate = 48000;
    playback->config.channels = 2;
    playback->config.format = TURBO_PLAYBACK_FORMAT_F32;
    playback->config.buffer_size_ms = 50;
  }

  playback->sample_rate = playback->config.sample_rate;
  playback->channels = playback->config.channels;

  /* Initialize ring buffer for push-based streaming */
  size_t buffer_frames = (playback->config.sample_rate * playback->config.buffer_size_ms) / 1000;
  size_t bytes_per_frame = playback->config.channels * 4; /* Assuming F32 */
  if (ring_buffer_init(&playback->ring, buffer_frames * bytes_per_frame * 4) != 0) {
    free(playback);
    return NULL;
  }

  if (ma_mutex_init(&playback->ring_mutex) != MA_SUCCESS) {
    ring_buffer_free(&playback->ring);
    free(playback);
    return NULL;
  }

  /* Configure device */
  playback->device_config = ma_device_config_init(ma_device_type_playback);
  playback->device_config.playback.format = playback_format_to_ma(playback->config.format);
  playback->device_config.playback.channels = playback->config.channels;
  playback->device_config.sampleRate = playback->config.sample_rate;
  playback->device_config.dataCallback = playback_data_callback;
  playback->device_config.pUserData = playback;

  /* TODO: Handle device_id selection */
  (void)device_id;

  if (ma_device_init(NULL, &playback->device_config, &playback->device) != MA_SUCCESS) {
    ma_mutex_uninit(&playback->ring_mutex);
    ring_buffer_free(&playback->ring);
    free(playback);
    return NULL;
  }

  return playback;
}

turbo_playback_t *turbo_playback_create_file(const char *device_id, const char *filepath) {
  if (!filepath)
    return NULL;

  turbo_playback_t *playback = (turbo_playback_t *)calloc(1, sizeof(turbo_playback_t));
  if (!playback)
    return NULL;

  playback->mode = PLAYBACK_MODE_FILE;
  playback->state = TURBO_PLAYBACK_STATE_STOPPED;
  playback->volume = 1.0f;

  /* Initialize decoder */
  ma_decoder_config decoder_config = ma_decoder_config_init(ma_format_f32, 2, 48000);
  if (ma_decoder_init_file(filepath, &decoder_config, &playback->decoder) != MA_SUCCESS) {
    free(playback);
    return NULL;
  }

  /* Get file info */
  playback->sample_rate = playback->decoder.outputSampleRate;
  playback->channels = playback->decoder.outputChannels;

  ma_uint64 total_frames;
  if (ma_decoder_get_length_in_pcm_frames(&playback->decoder, &total_frames) == MA_SUCCESS) {
    playback->total_frames = total_frames;
  }

  /* Configure device to match decoder output */
  playback->device_config = ma_device_config_init(ma_device_type_playback);
  playback->device_config.playback.format = playback->decoder.outputFormat;
  playback->device_config.playback.channels = playback->decoder.outputChannels;
  playback->device_config.sampleRate = playback->decoder.outputSampleRate;
  playback->device_config.dataCallback = playback_data_callback;
  playback->device_config.pUserData = playback;

  (void)device_id;

  if (ma_device_init(NULL, &playback->device_config, &playback->device) != MA_SUCCESS) {
    ma_decoder_uninit(&playback->decoder);
    free(playback);
    return NULL;
  }

  return playback;
}

void turbo_playback_destroy(turbo_playback_t *playback) {
  if (!playback)
    return;

  turbo_playback_stop(playback);

  ma_device_uninit(&playback->device);

  if (playback->mode == PLAYBACK_MODE_FILE) {
    ma_decoder_uninit(&playback->decoder);
    /* Clean up queue */
    turbo_playback_queue_clear(playback);
  } else {
    ma_mutex_uninit(&playback->ring_mutex);
    ring_buffer_free(&playback->ring);
  }

  free(playback);
}

/* =============================================================================
 * Callbacks
 * ============================================================================= */

void turbo_playback_set_data_callback(turbo_playback_t *playback, turbo_playback_data_cb cb,
                                      void *user_data) {
  if (!playback)
    return;
  playback->data_cb = cb;
  playback->user_data = user_data;
}

void turbo_playback_on_state(turbo_playback_t *playback, turbo_playback_state_cb cb) {
  if (!playback)
    return;
  playback->state_cb = cb;
}

void turbo_playback_on_complete(turbo_playback_t *playback, turbo_playback_complete_cb cb) {
  if (!playback)
    return;
  playback->complete_cb = cb;
}

/* =============================================================================
 * Playback Control
 * ============================================================================= */

static void set_state(turbo_playback_t *playback, turbo_playback_state_t state) {
  if (playback->state != state) {
    playback->state = state;
    if (playback->state_cb) {
      playback->state_cb(playback, state, playback->user_data);
    }
  }
}

int turbo_playback_start(turbo_playback_t *playback) {
  if (!playback)
    return TURBO_PLAYBACK_ERR_DEVICE;

  if (playback->state == TURBO_PLAYBACK_STATE_PLAYING) {
    return TURBO_PLAYBACK_OK;
  }

  set_state(playback, TURBO_PLAYBACK_STATE_STARTING);

  if (ma_device_start(&playback->device) != MA_SUCCESS) {
    set_state(playback, TURBO_PLAYBACK_STATE_ERROR);
    return TURBO_PLAYBACK_ERR_DEVICE;
  }

  set_state(playback, TURBO_PLAYBACK_STATE_PLAYING);
  return TURBO_PLAYBACK_OK;
}

void turbo_playback_stop(turbo_playback_t *playback) {
  if (!playback)
    return;

  if (playback->state == TURBO_PLAYBACK_STATE_STOPPED)
    return;

  set_state(playback, TURBO_PLAYBACK_STATE_STOPPING);
  ma_device_stop(&playback->device);
  set_state(playback, TURBO_PLAYBACK_STATE_STOPPED);

  /* Reset decoder position for file playback */
  if (playback->mode == PLAYBACK_MODE_FILE) {
    ma_decoder_seek_to_pcm_frame(&playback->decoder, 0);
  }
}

void turbo_playback_pause(turbo_playback_t *playback) {
  if (!playback || playback->state != TURBO_PLAYBACK_STATE_PLAYING)
    return;

  ma_device_stop(&playback->device);
  set_state(playback, TURBO_PLAYBACK_STATE_PAUSED);
}

void turbo_playback_resume(turbo_playback_t *playback) {
  if (!playback || playback->state != TURBO_PLAYBACK_STATE_PAUSED)
    return;

  if (ma_device_start(&playback->device) == MA_SUCCESS) {
    set_state(playback, TURBO_PLAYBACK_STATE_PLAYING);
  }
}

turbo_playback_state_t turbo_playback_get_state(turbo_playback_t *playback) {
  return playback ? playback->state : TURBO_PLAYBACK_STATE_STOPPED;
}

/* =============================================================================
 * Volume and Position
 * ============================================================================= */

void turbo_playback_set_volume(turbo_playback_t *playback, float volume) {
  if (!playback)
    return;
  playback->volume = (volume < 0.0f) ? 0.0f : volume;
}

float turbo_playback_get_volume(turbo_playback_t *playback) {
  return playback ? playback->volume : 0.0f;
}

int turbo_playback_seek(turbo_playback_t *playback, uint64_t position_ms) {
  if (!playback || playback->mode != PLAYBACK_MODE_FILE) {
    return TURBO_PLAYBACK_ERR_UNSUPPORTED;
  }

  uint64_t frame = (position_ms * playback->sample_rate) / 1000;
  if (ma_decoder_seek_to_pcm_frame(&playback->decoder, frame) != MA_SUCCESS) {
    return TURBO_PLAYBACK_ERR_FILE;
  }

  return TURBO_PLAYBACK_OK;
}

uint64_t turbo_playback_get_position(turbo_playback_t *playback) {
  if (!playback || playback->mode != PLAYBACK_MODE_FILE)
    return 0;

  ma_uint64 cursor;
  if (ma_decoder_get_cursor_in_pcm_frames(&playback->decoder, &cursor) != MA_SUCCESS) {
    return 0;
  }

  return (cursor * 1000) / playback->sample_rate;
}

uint64_t turbo_playback_get_duration(turbo_playback_t *playback) {
  if (!playback || playback->mode != PLAYBACK_MODE_FILE)
    return 0;

  return (playback->total_frames * 1000) / playback->sample_rate;
}

void turbo_playback_set_looping(turbo_playback_t *playback, int loop) {
  if (playback) {
    playback->looping = loop ? 1 : 0;
  }
}

/* =============================================================================
 * Push-based Streaming
 * ============================================================================= */

size_t turbo_playback_write(turbo_playback_t *playback, const void *samples, size_t len) {
  if (!playback || playback->mode != PLAYBACK_MODE_STREAMING || !samples || len == 0)
    return 0;

  ma_mutex_lock(&playback->ring_mutex);
  size_t written = ring_buffer_write(&playback->ring, samples, len);
  ma_mutex_unlock(&playback->ring_mutex);

  return written;
}

size_t turbo_playback_get_available(turbo_playback_t *playback) {
  if (!playback || playback->mode != PLAYBACK_MODE_STREAMING)
    return 0;

  ma_mutex_lock(&playback->ring_mutex);
  size_t available = playback->ring.capacity - playback->ring.count;
  ma_mutex_unlock(&playback->ring_mutex);

  return available;
}

size_t turbo_playback_get_buffered(turbo_playback_t *playback) {
  if (!playback || playback->mode != PLAYBACK_MODE_STREAMING)
    return 0;

  ma_mutex_lock(&playback->ring_mutex);
  size_t buffered = playback->ring.count;
  ma_mutex_unlock(&playback->ring_mutex);

  return buffered;
}

void turbo_playback_clear(turbo_playback_t *playback) {
  if (!playback || playback->mode != PLAYBACK_MODE_STREAMING)
    return;

  ma_mutex_lock(&playback->ring_mutex);
  playback->ring.read_pos = 0;
  playback->ring.write_pos = 0;
  playback->ring.count = 0;
  ma_mutex_unlock(&playback->ring_mutex);
}

/* =============================================================================
 * Playlist/Queue API
 * ============================================================================= */

int turbo_playback_queue_add(turbo_playback_t *playback, const char *filepath) {
  if (!playback || !filepath || playback->mode != PLAYBACK_MODE_FILE) {
    return TURBO_PLAYBACK_ERR_UNSUPPORTED;
  }

  if (playback->queue.count >= TURBO_PLAYBACK_MAX_QUEUE) {
    return TURBO_PLAYBACK_ERR_NOMEM;
  }

  /* Duplicate the filepath string */
  size_t len = strlen(filepath);
  char *copy = (char *)malloc(len + 1);
  if (!copy)
    return TURBO_PLAYBACK_ERR_NOMEM;

  memcpy(copy, filepath, len + 1);
  playback->queue.filepaths[playback->queue.count] = copy;
  playback->queue.count++;

  return TURBO_PLAYBACK_OK;
}

void turbo_playback_queue_clear(turbo_playback_t *playback) {
  if (!playback)
    return;

  for (int i = 0; i < playback->queue.count; i++) {
    if (playback->queue.filepaths[i]) {
      free(playback->queue.filepaths[i]);
      playback->queue.filepaths[i] = NULL;
    }
  }
  playback->queue.count = 0;
  playback->queue.current = 0;
}

int turbo_playback_queue_count(turbo_playback_t *playback) {
  return playback ? playback->queue.count : 0;
}

int turbo_playback_queue_next(turbo_playback_t *playback) {
  if (!playback || playback->mode != PLAYBACK_MODE_FILE) {
    return TURBO_PLAYBACK_ERR_UNSUPPORTED;
  }

  if (queue_switch_to_next(playback) != 0) {
    return TURBO_PLAYBACK_ERR_FILE;
  }

  return TURBO_PLAYBACK_OK;
}

void turbo_playback_queue_set_looping(turbo_playback_t *playback, int loop) {
  if (playback) {
    playback->queue.loop_queue = loop ? 1 : 0;
  }
}
