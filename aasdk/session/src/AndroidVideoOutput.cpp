#include <AndroidVideoOutput.hpp>
#include <android/log.h>
#include <media/NdkMediaFormat.h>
#include <cstring>

#define LOG_TAG "AaSdk_Video"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace aasdk_android {

AndroidVideoOutput::~AndroidVideoOutput() {
    stop();
}

void AndroidVideoOutput::warmup() {
    AMediaCodec* codec = AMediaCodec_createDecoderByType("video/avc");
    if (!codec) {
        LOGE("warmup: failed to create AVC decoder");
        return;
    }

    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, 1920);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, 1080);

    // No surface: this instance only exists to absorb the driver's one-time
    // negotiation glitch, nothing needs to actually render.
    if (AMediaCodec_configure(codec, fmt, nullptr, nullptr, 0) == AMEDIA_OK &&
        AMediaCodec_start(codec) == AMEDIA_OK) {
        AMediaCodec_stop(codec);
        LOGI("warmup: decoder primed");
    } else {
        LOGE("warmup: configure/start failed (non-fatal, just means priming didn't happen)");
    }

    AMediaFormat_delete(fmt);
    AMediaCodec_delete(codec);
}

bool AndroidVideoOutput::init(ANativeWindow* window, int width, int height) {
    if (window == nullptr) {
        // No window yet -- e.g. AVChannelSetupRequest arrived before the
        // Activity's Surface did. The codec gets created once a real window
        // shows up via a later init() call from sessionSetSurface(); until
        // then write() is a safe no-op (started_ stays false).
        return true;
    }

    if (started_) {
        if (window_ == window) return true; // already running against this window
        stop(); // window changed (e.g. Surface recreated) -- tear down and recreate
    }

    window_ = window;
    ANativeWindow_acquire(window_);

    codec_ = AMediaCodec_createDecoderByType("video/avc");
    if (!codec_) {
        LOGE("Failed to create AVC decoder");
        return false;
    }

    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, height);
    // Low-latency mode: output frames as soon as decoded
    AMediaFormat_setInt32(fmt, "low-latency", 1);

    media_status_t status = AMediaCodec_configure(
        codec_, fmt, window_, nullptr, 0);
    AMediaFormat_delete(fmt);

    if (status != AMEDIA_OK) {
        LOGE("AMediaCodec_configure failed: %d", (int)status);
        return false;
    }

    status = AMediaCodec_start(codec_);
    if (status != AMEDIA_OK) {
        LOGE("AMediaCodec_start failed: %d", (int)status);
        return false;
    }

    started_ = true;
    LOGI("Video decoder started %dx%d", width, height);
    return true;
}

bool AndroidVideoOutput::write(const uint8_t* data, size_t size) {
    if (!started_) return false;
    drainOutput();
    return queueInput(data, size);
}

bool AndroidVideoOutput::queueInput(const uint8_t* data, size_t size) {
    ssize_t idx = AMediaCodec_dequeueInputBuffer(codec_, 2000 /*us*/);
    if (idx < 0) {
        // Transient: no input buffer available; drop frame to avoid blocking
        return true;
    }
    size_t bufSize = 0;
    uint8_t* buf = AMediaCodec_getInputBuffer(codec_, (size_t)idx, &bufSize);
    if (!buf) return false;

    size_t copyLen = (size <= bufSize) ? size : bufSize;
    std::memcpy(buf, data, copyLen);

    // Timestamp in microseconds (monotonic)
    int64_t pts = 0;
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    pts = (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;

    media_status_t st = AMediaCodec_queueInputBuffer(
        codec_, (size_t)idx, 0, copyLen, pts, 0);
    return st == AMEDIA_OK;
}

void AndroidVideoOutput::drainOutput() {
    AMediaCodecBufferInfo info{};
    for (;;) {
        ssize_t idx = AMediaCodec_dequeueOutputBuffer(codec_, &info, 0);
        if (idx < 0) break;
        AMediaCodec_releaseOutputBuffer(codec_, (size_t)idx, true /*render*/);
    }
}

void AndroidVideoOutput::stop() {
    if (started_) {
        AMediaCodec_stop(codec_);
        started_ = false;
    }
    if (codec_) {
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
    if (window_) {
        ANativeWindow_release(window_);
        window_ = nullptr;
    }
}

} // namespace aasdk_android
