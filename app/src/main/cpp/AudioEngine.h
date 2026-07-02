#pragma once
#include <oboe/Oboe.h>
#include <memory>
#include <atomic>
#include "DlmsEngine.h"

/**
 * AudioEngine — Real-time full-duplex audio using Google Oboe.
 *
 * Architecture:
 *   - Output stream uses callback mode (onAudioReady) — drives the audio clock.
 *   - Input stream uses NON-callback mode (read() called from output callback)
 *     so there is exactly ONE active callback thread. This avoids the dual-
 *     callback concurrency issue and is the pattern recommended by Oboe for
 *     full-duplex on Android.
 *
 * dlms losss — mas ari
 */
class AudioEngine : public oboe::AudioStreamCallback {
public:
    AudioEngine();
    ~AudioEngine();

    bool start();
    void stop();

    DlmsEngine& dspEngine() { return *dspEngine_; }
    bool isRunning() const  { return running_.load(); }

    // oboe::AudioStreamCallback — output stream only
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* stream,
                                          void* audioData,
                                          int32_t numFrames) override;
    void onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) override;

private:
    std::shared_ptr<DlmsEngine> dspEngine_;

    std::shared_ptr<oboe::AudioStream> inputStream_;   // non-callback (blocking read)
    std::shared_ptr<oboe::AudioStream> outputStream_;  // callback driven

    std::atomic<bool> running_{false};

    static constexpr int kMaxFrames     = 2048;
    static constexpr int kInputChannels = 2;

    // Separate interleaved buffer for raw stereo input (size = frames × channels)
    float inputInterleaved_[kMaxFrames * kInputChannels]{};

    // De-interleaved mono channels for DSP
    float inputBufL_[kMaxFrames]{};
    float inputBufR_[kMaxFrames]{};

    // 6 output channel buffers + pointer array
    float  outputBufs_[kNumOutputChannels][kMaxFrames]{};
    float* outputPtrs_[kNumOutputChannels]{};

    bool openInputStream();
    bool openOutputStream();
};
