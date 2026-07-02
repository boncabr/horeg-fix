#include "AudioEngine.h"
#include <android/log.h>
#include <cassert>

#define LOG_TAG "AudioEngine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)

/**
 * AudioEngine — Oboe full-duplex audio with 6-channel output.
 * dlms losss — mas ari
 */

AudioEngine::AudioEngine() {
    dspEngine_ = std::make_shared<DlmsEngine>(48000.0f);
    for (int ch = 0; ch < kNumOutputChannels; ++ch)
        outputPtrs_[ch] = outputBufs_[ch];
}

AudioEngine::~AudioEngine() { stop(); }

// ── Open input (stereo mic / USB) ─────────────────────────────────────────────
bool AudioEngine::openInputStream() {
    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Input)
           .setPerformanceMode(oboe::PerformanceMode::LowLatency)
           .setSharingMode(oboe::SharingMode::Exclusive)
           .setFormat(oboe::AudioFormat::Float)
           .setChannelCount(2)                   // Stereo input
           .setSampleRate(48000)
           .setCallback(this);

    oboe::Result result = builder.openStream(inputStream_);
    if (result != oboe::Result::OK) {
        LOGE("Failed to open input stream: %s", oboe::convertToText(result));
        return false;
    }
    LOGI("Input stream opened: sr=%d, channels=%d",
         inputStream_->getSampleRate(), inputStream_->getChannelCount());
    return true;
}

// ── Open output (6-channel USB audio interface) ───────────────────────────────
bool AudioEngine::openOutputStream() {
    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output)
           .setPerformanceMode(oboe::PerformanceMode::LowLatency)
           .setSharingMode(oboe::SharingMode::Exclusive)
           .setFormat(oboe::AudioFormat::Float)
           .setChannelCount(kNumOutputChannels)  // 6 channels for USB interface
           .setSampleRate(48000)
           .setCallback(this);

    oboe::Result result = builder.openStream(outputStream_);
    if (result != oboe::Result::OK) {
        LOGW("Exclusive 6-ch output failed (%s), retrying Shared mode...",
             oboe::convertToText(result));
        // Fallback: shared mode (e.g. emulator or devices without multi-ch support)
        builder.setSharingMode(oboe::SharingMode::Shared);
        result = builder.openStream(outputStream_);
        if (result != oboe::Result::OK) {
            LOGE("Failed to open output stream: %s", oboe::convertToText(result));
            return false;
        }
    }

    // Update DSP engine with actual sample rate
    const float sr = static_cast<float>(outputStream_->getSampleRate());
    dspEngine_->prepare(sr);

    LOGI("Output stream opened: sr=%.0f, channels=%d",
         sr, outputStream_->getChannelCount());
    return true;
}

bool AudioEngine::start() {
    if (running_.load()) return true;

    if (!openInputStream() || !openOutputStream()) {
        stop();
        return false;
    }

    auto inResult  = inputStream_->requestStart();
    auto outResult = outputStream_->requestStart();

    if (inResult != oboe::Result::OK || outResult != oboe::Result::OK) {
        LOGE("Stream start failed: in=%s out=%s",
             oboe::convertToText(inResult), oboe::convertToText(outResult));
        stop();
        return false;
    }

    running_.store(true);
    LOGI("AudioEngine started");
    return true;
}

void AudioEngine::stop() {
    if (inputStream_) {
        inputStream_->requestStop();
        inputStream_->close();
        inputStream_.reset();
    }
    if (outputStream_) {
        outputStream_->requestStop();
        outputStream_->close();
        outputStream_.reset();
    }
    running_.store(false);
    LOGI("AudioEngine stopped");
}

// ── Real-time audio callback ──────────────────────────────────────────────────
oboe::DataCallbackResult AudioEngine::onAudioReady(oboe::AudioStream* stream,
                                                    void* audioData,
                                                    int32_t numFrames)
{
    assert(numFrames <= kMaxFrames);

    if (stream == outputStream_.get()) {
        // Try to read available input samples (non-blocking)
        if (inputStream_) {
            auto readResult = inputStream_->read(
                inputBufL_, numFrames, 0 /* timeout ns */);
            if (readResult) {
                // De-interleave stereo: inputBufL_ holds interleaved L/R
                for (int i = 0; i < numFrames; ++i) {
                    const float* src = reinterpret_cast<const float*>(inputBufL_) + i * 2;
                    inputBufL_[i] = src[0];
                    inputBufR_[i] = src[1];
                }
            } else {
                // Silence on read error
                std::fill(inputBufL_, inputBufL_ + numFrames, 0.0f);
                std::fill(inputBufR_, inputBufR_ + numFrames, 0.0f);
            }
        } else {
            std::fill(inputBufL_, inputBufL_ + numFrames, 0.0f);
            std::fill(inputBufR_, inputBufR_ + numFrames, 0.0f);
        }

        // Run DSP engine → 6 output channels
        dspEngine_->processBlock(inputBufL_, inputBufR_, outputPtrs_, numFrames);

        // Interleave 6 channels into Oboe output buffer
        float* out = reinterpret_cast<float*>(audioData);
        for (int i = 0; i < numFrames; ++i) {
            for (int ch = 0; ch < kNumOutputChannels; ++ch) {
                out[i * kNumOutputChannels + ch] = outputBufs_[ch][i];
            }
        }
    }

    return oboe::DataCallbackResult::Continue;
}

void AudioEngine::onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) {
    LOGW("Stream error after close: %s — attempting restart", oboe::convertToText(error));
    running_.store(false);
    // Restart on audio thread error (e.g. USB disconnect/reconnect)
    start();
}
