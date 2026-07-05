#pragma once
#include <aaudio/AAudio.h>
#include <cstdint>

namespace aasdk_android {

enum class AudioChannel { MEDIA, SPEECH, SYSTEM };

// Plays PCM audio received from AA audio channels via AAudio.
// One instance per channel (media=stereo 48kHz, speech/system=mono 16kHz).
class AndroidAudioOutput {
public:
    explicit AndroidAudioOutput(AudioChannel channel);
    ~AndroidAudioOutput();

    bool init();
    // data: interleaved 16-bit PCM samples
    bool write(const int16_t* data, size_t frameCount);
    void stop();

private:
    AudioChannel channel_;
    AAudioStream* stream_{nullptr};
};

} // namespace aasdk_android
