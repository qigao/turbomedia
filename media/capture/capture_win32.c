/**
 * Windows Capture Dispatcher
 *
 * Centralizes capture start/stop/destroy for all types on Windows
 */
#ifdef _WIN32

  #ifndef NOMINMAX
    #define NOMINMAX
  #endif

  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <objbase.h>
  #include <rpc.h>
  #include <rpcndr.h>
  #include <stdlib.h>
  #include <windows.h>


#endif /* _WIN32 */

#include "turbo_capture.h"

#ifdef _WIN32

/* Internal hooks from specific implementations */
/* Audio: uses miniaudio (cross-platform) */
extern int miniaudio_audio_start(turbo_capture_t *capture);
extern void miniaudio_audio_stop(turbo_capture_t *capture);
extern void miniaudio_audio_destroy(turbo_capture_t *capture);

/* Video: uses Media Foundation */
extern int mf_video_start(turbo_capture_t *capture);
extern void mf_video_stop(turbo_capture_t *capture);
extern void mf_video_destroy(turbo_capture_t *capture);

/* Screen: uses DXGI Desktop Duplication */
extern int dxgi_screen_start(turbo_capture_t *capture);
extern void dxgi_screen_stop(turbo_capture_t *capture);
extern void dxgi_screen_destroy(turbo_capture_t *capture);

int turbo_capture_start(turbo_capture_t *capture) {
  if (!capture) return -1;
  if (capture->state == TURBO_CAPTURE_STATE_RUNNING) return 0;

  capture->state = TURBO_CAPTURE_STATE_STARTING;

  int res = -1;
  switch (capture->type) {
  case TURBO_CAPTURE_TYPE_AUDIO:
    res = miniaudio_audio_start(capture);
    break;
  case TURBO_CAPTURE_TYPE_VIDEO:
    res = mf_video_start(capture);
    break;
  case TURBO_CAPTURE_TYPE_SCREEN:
    res = dxgi_screen_start(capture);
    break;
  default:
    return -1;
  }

  if (res == 0) {
    capture->state = TURBO_CAPTURE_STATE_RUNNING;
    if (capture->state_cb) {
      capture->state_cb(capture, capture->state, capture->user_data);
    }
  } else {
    capture->state = TURBO_CAPTURE_STATE_ERROR;
  }

  return res;
}

void turbo_capture_stop(turbo_capture_t *capture) {
  if (!capture || capture->state == TURBO_CAPTURE_STATE_STOPPED) return;

  capture->state = TURBO_CAPTURE_STATE_STOPPING;

  switch (capture->type) {
  case TURBO_CAPTURE_TYPE_AUDIO:
    miniaudio_audio_stop(capture);
    break;
  case TURBO_CAPTURE_TYPE_VIDEO:
    mf_video_stop(capture);
    break;
  case TURBO_CAPTURE_TYPE_SCREEN:
    dxgi_screen_stop(capture);
    break;
  }

  capture->state = TURBO_CAPTURE_STATE_STOPPED;
  if (capture->state_cb) {
    capture->state_cb(capture, capture->state, capture->user_data);
  }
}

void turbo_capture_destroy(turbo_capture_t *capture) {
  if (!capture) return;

  switch (capture->type) {
  case TURBO_CAPTURE_TYPE_AUDIO:
    miniaudio_audio_destroy(capture);
    break;
  case TURBO_CAPTURE_TYPE_VIDEO:
    mf_video_destroy(capture);
    break;
  case TURBO_CAPTURE_TYPE_SCREEN:
    dxgi_screen_destroy(capture);
    break;
  }
}

turbo_capture_state_t turbo_capture_get_state(turbo_capture_t *capture) {
  return capture ? capture->state : TURBO_CAPTURE_STATE_STOPPED;
}

void turbo_capture_on_state(turbo_capture_t *capture, turbo_capture_state_cb cb) {
  if (capture) capture->state_cb = cb;
}

#endif /* _WIN32 */
