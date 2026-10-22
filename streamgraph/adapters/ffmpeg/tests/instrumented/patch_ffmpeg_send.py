#!/usr/bin/env python3
"""Patch the pinned FFmpeg 6.1.1 source ONLY for a Linux conformance build.

This inserts a test gate AFTER FFmpeg references the packet inside avcodec_send_packet(),
not in TurboMedia before or after the library call. Never distribute this
patched library as a production codec or link it into the normal SDK.
"""
from pathlib import Path
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: patch_ffmpeg_send.py <FFmpeg/libavcodec/decode.c>")
path = Path(sys.argv[1])
original = path.read_text(encoding="utf-8")
signature = (
    "int attribute_align_arg avcodec_send_packet(AVCodecContext *avctx, "
    "const AVPacket *avpkt)\n{\n"
    "    AVCodecInternal *avci = avctx->internal;\n"
    "    DecodeContext     *dc = decode_ctx(avci);\n"
    "    int ret;\n"
)
if original.count(signature) != 1:
    raise SystemExit("unsupported FFmpeg version: expected one exact 6.1.1 signature")
if "avcodec_tm_test_arm_send_packet" in original or "avcodec_tm_test_arm_flush" in original:
    raise SystemExit("FFmpeg source already instrumented")
gate = """
/* TurboMedia test-only probe. THIS CODE RUNS INSIDE LIBAVCODEC.
 * 0=idle, 1=armed, 2=worker stopped inside avcodec_send_packet,
 * 3=owner released it, 4=worker exited the test gate.
 * No third-party Codec memory is freed or accessed by the cancelling owner.
 */
#include <stdatomic.h>
static atomic_uint tm_ffmpeg_send_gate = ATOMIC_VAR_INIT(0u);
int avcodec_tm_test_arm_send_packet(void);
unsigned avcodec_tm_test_send_packet_state(void);
int avcodec_tm_test_release_send_packet(void);
int avcodec_tm_test_arm_send_packet(void) {
    unsigned expected = 0u;
    return atomic_compare_exchange_strong_explicit(&tm_ffmpeg_send_gate,
        &expected, 1u, memory_order_acq_rel, memory_order_acquire) ? 0 : -1;
}
unsigned avcodec_tm_test_send_packet_state(void) {
    return atomic_load_explicit(&tm_ffmpeg_send_gate, memory_order_acquire);
}
int avcodec_tm_test_release_send_packet(void) {
    unsigned expected = 2u;
    return atomic_compare_exchange_strong_explicit(&tm_ffmpeg_send_gate,
        &expected, 3u, memory_order_acq_rel, memory_order_acquire) ? 0 : -1;
}
/* Separate arm for avcodec_send_packet(NULL). State is independent of the
 * normal packet gate so this cannot break M15b's packet cancellation test. */
static atomic_uint tm_ffmpeg_flush_gate = ATOMIC_VAR_INIT(0u);
int avcodec_tm_test_arm_flush(void);
unsigned avcodec_tm_test_flush_state(void);
int avcodec_tm_test_release_flush(void);
int avcodec_tm_test_arm_flush(void) {
    unsigned expected = 0u;
    return atomic_compare_exchange_strong_explicit(&tm_ffmpeg_flush_gate,
        &expected, 1u, memory_order_acq_rel, memory_order_acquire) ? 0 : -1;
}
unsigned avcodec_tm_test_flush_state(void) {
    return atomic_load_explicit(&tm_ffmpeg_flush_gate, memory_order_acquire);
}
int avcodec_tm_test_release_flush(void) {
    unsigned expected = 2u;
    return atomic_compare_exchange_strong_explicit(&tm_ffmpeg_flush_gate,
        &expected, 3u, memory_order_acq_rel, memory_order_acquire) ? 0 : -1;
}
"""
pause = """
    /* Instrumented-only: a successful 1->2 transition can only happen from
     * within this very avcodec_send_packet() call. The next Codec can still
     * enter independently because the state is now 2 rather than 1. */
    if (avpkt && avpkt->data && avpkt->size > 0) {
        unsigned expected = 1u;
        if (atomic_compare_exchange_strong_explicit(&tm_ffmpeg_send_gate,
                &expected, 2u, memory_order_acq_rel, memory_order_acquire)) {
            while (atomic_load_explicit(&tm_ffmpeg_send_gate,
                                        memory_order_acquire) != 3u) {
                /* Conformance watchdog is enforced by the CTest timeout. */
            }
            atomic_store_explicit(&tm_ffmpeg_send_gate, 4u,
                                  memory_order_release);
        }
    }
    /* NULL packet has already set FFmpeg's draining_started state here.
     * This is INSIDE libavcodec, not the TurboMedia call-site wrapper. */
    if (avpkt == NULL) {
        unsigned expected = 1u;
        if (atomic_compare_exchange_strong_explicit(&tm_ffmpeg_flush_gate,
                &expected, 2u, memory_order_acq_rel, memory_order_acquire)) {
            while (atomic_load_explicit(&tm_ffmpeg_flush_gate,
                                        memory_order_acquire) != 3u) {
                /* The test runner enforces a finite CTest watchdog. */
            }
            atomic_store_explicit(&tm_ffmpeg_flush_gate, 4u,
                                  memory_order_release);
        }
    }
"""
# Place the pause only AFTER FFmpeg validated the decoder packet and took
# its own av_packet_ref, but BEFORE its internal decode attempt. This is
# materially stronger than a wrapper or a probe at the function entrance.
after_packet_ref = (
    "    } else\n"
    "        dc->draining_started = 1;\n\n"
    "    if (!avci->buffer_frame->buf[0] && !dc->draining_started) {"
)
if original.count(after_packet_ref) != 1:
    raise SystemExit("unsupported FFmpeg version: exact packet ownership site missing")
patched = original.replace(signature, gate + "\n" + signature)
patched = patched.replace(
    after_packet_ref,
    after_packet_ref.replace(
        "    if (!avci->buffer_frame", pause + "    if (!avci->buffer_frame"
    ),
)
path.write_text(patched, encoding="utf-8")
print(f"Instrumented avcodec_send_packet AFTER av_packet_ref and inside NULL flush in {path}")
