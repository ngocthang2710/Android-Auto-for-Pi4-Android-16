#pragma once
#include <media/NdkMediaCodec.h>
#include <android/native_window.h>
#include <cstdint>
#include <vector>

namespace aasdk_android {

// Receives raw Annex-B H.264 frames from the AA video channel and decodes
// them via AMediaCodec directly onto an ANativeWindow (SurfaceView surface).
class AndroidVideoOutput {
public:
    AndroidVideoOutput() = default;
    ~AndroidVideoOutput();

    // One-shot, throwaway c2.v4l2.avc.decoder create/configure/start/stop
    // cycle against no surface. Call once, early, before any real session
    // exists (see aasdk_jni.cpp nativeCreate). Works around a one-time
    // negotiation glitch in the RPi4 bcm2835-codec V4L2 driver where the
    // *first* AVC decoder instance created after boot logs a kernel-level
    // "buffer size mismatch" and silently never produces decodable output
    // (no error surfaces up through AMediaCodec/CCodec) -- absorbing that
    // glitch here means the real session's decoder is never the first one.
    static void warmup();

    // Must be called before the first write().  window is retained.
    bool init(ANativeWindow* window, int width, int height);

    // Feed a complete Annex-B NAL unit (may contain SPS/PPS + IDR on first
    // call).  Returns false on fatal codec error.
    bool write(const uint8_t* data, size_t size);

    void stop();

private:
    AMediaCodec* codec_{nullptr};
    ANativeWindow* window_{nullptr};
    bool started_{false};

    bool queueInput(const uint8_t* data, size_t size);
    void drainOutput();
};

} // namespace aasdk_android
