#ifndef TM_TEST_REAL_COMPONENT_PROVIDER_H
#define TM_TEST_REAL_COMPONENT_PROVIDER_H

#include <salts/component_plugin_abi.h>

/* Test-owned typed ABI consumed from the *installed* Salts SDK.
 * No copied Salts fixture or private Plugin descriptor. */
#if defined(TM_TEST_FFMPEG_DSO)
#include <stddef.h>
#include <stdint.h>
/* A borrowed compressed packet and caller-owned luma output, both bounded.
 * Never expose FFmpeg AVFrame/AVPacket or own those pointers across returns. */
#define TM_TEST_PROVIDER_FFMPEG_METHOD(X, I) \
    X(I, R4, int, decode_mjpeg, const uint8_t *, packet, size_t, packet_size, \
      uint8_t *, out_luma, size_t, luma_capacity)
#else
#define TM_TEST_PROVIDER_FFMPEG_METHOD(X, I)
#endif

#define TM_TEST_PROVIDER_METHODS(X, I) \
    X(I, R0, int, get, _) \
    X(I, R0, int, entered, _) \
    X(I, R0, int, release, _) \
    TM_TEST_PROVIDER_FFMPEG_METHOD(X, I)

CMETA_INTERFACE(tm_test_provider, TM_TEST_PROVIDER_METHODS);
CMETA_OBJECT_INTERFACE_ADAPTER(tm_test_provider);

#define TM_TEST_PROVIDER_EXPORT_ID "tm.streamgraph.test.provider"
#endif
