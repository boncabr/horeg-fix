#pragma once
#include <oboe/Oboe.h>
#include <memory>
#include <atomic>
#include "DlmsEngine.h"

/**
 * AudioEngine — Real-time full-duplex audio using Google Oboe
 *
 * Routes stereo input through DlmsEngine, producing 6-channel output for a
 * USB multichannel audio interface (via USB OTG).
 *
 * dlms losss — mas ari
 */

class AudioEngine : public oboe::AudioStreamCallback {
public:
    AudioEngine();
    ~AudioEngine();

    /** Open and start both input and output streams. Returns true on success. */
    bool start();

    /** Stop and close both streams gracefully. */
    void stop();

    /** Access the underlying DSP engine to update parameters. */
    DlmsEngine& dspEngine() { return *dspEngine_; }

    /** True while the audio streams are running. */
    bool isRunning() const { return running_.load(); }

    // ── oboe::AudioStreamCallback ─────────────────────────────────────────
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* stream,
                                          void* audioData,
                                          int32_t numFrames) override;

    void onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) override;

private:
    std::shared_ptr<DlmsEngine> dspEngine_;

    // Oboe streams
    std::shared_ptr<oboe::AudioStream> inputStream_;
    std::shared_ptr<oboe::AudioStream> outputStream_;

    std::atomic<bool> running_{false};

    // Scratch buffers (allocated once to avoid per-callback heap alloc)
    static constexpr int kMaxFrames = 2048;

    float inputBufL_[kMaxFrames]{};
    float inputBufR_[kMaxFrames]{};
    float outputBufs_[kNumOutputChannels][kMaxFrames]{};
    float* outputPtrs_[kNumOutputChannels]{};

    bool openInputStream();
    bool openOutputStream();
};
