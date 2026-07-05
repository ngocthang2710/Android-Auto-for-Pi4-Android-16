#include <AndroidAudioOutput.hpp>
#include <android/log.h>

#define LOG_TAG "AaSdk_Audio"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace aasdk_android {

AndroidAudioOutput::AndroidAudioOutput(AudioChannel channel) : channel_(channel) {}

AndroidAudioOutput::~AndroidAudioOutput() {
    stop();
}

bool AndroidAudioOutput::init() {
    AAudioStreamBuilder* builder;
    aaudio_result_t r = AAudio_createStreamBuilder(&builder);
    if (r != AAUDIO_OK) return false;

    // Channel config
    if (channel_ == AudioChannel::MEDIA) {
        AAudioStreamBuilder_setSampleRate(builder, 48000);
        AAudioStreamBuilder_setChannelCount(builder, 2);
    } else {
        // SPEECH and SYSTEM: mono 16 kHz
        AAudioStreamBuilder_setSampleRate(builder, 16000);
        AAudioStreamBuilder_setChannelCount(builder, 1);
    }

    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);

    r = AAudioStreamBuilder_openStream(builder, &stream_);
    AAudioStreamBuilder_delete(builder);

    if (r != AAUDIO_OK) {
        LOGE("AAudio open failed: %s", AAudio_convertResultToText(r));
        stream_ = nullptr;
        return false;
    }

    r = AAudioStream_requestStart(stream_);
    if (r != AAUDIO_OK) {
        LOGE("AAudio start failed: %s", AAudio_convertResultToText(r));
        return false;
    }

    LOGI("Audio stream started (channel=%d)", (int)channel_);
    return true;
}

bool AndroidAudioOutput::write(const int16_t* data, size_t frameCount) {
    if (!stream_) return false;
    aaudio_result_t r = AAudioStream_write(
        stream_, data, (int32_t)frameCount, 100000000LL /*100ms timeout*/);
    return r > 0;
}

void AndroidAudioOutput::stop() {
    if (stream_) {
        AAudioStream_requestStop(stream_);
        AAudioStream_close(stream_);
        stream_ = nullptr;
    }
}

} // namespace aasdk_android
