#include "AudioEngine.h"
#include <android/log.h>
#include <cassert>
#include <algorithm>

#define LOG_TAG "AudioEngine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)

/**
 * AudioEngine — Oboe full-duplex, output-callback driven.
 * dlms losss — mas ari
 */

AudioEngine::AudioEngine() {
    dspEngine_ = std::make_shared<DlmsEngine>(48000.0f);
    for (int ch = 0; ch < kNumOutputChannels; ++ch)
        outputPtrs_[ch] = outputBufs_[ch];
}

AudioEngine::~AudioEngine() { stop(); }

// ── Open INPUT stream (non-callback, blocking read) ───────────────────────────
bool AudioEngine::openInputStream() {
    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Input)
           .setPerformanceMode(oboe::PerformanceMode::LowLatency)
           .setSharingMode(oboe::SharingMode::Exclusive)
           .setFormat(oboe::AudioFormat::Float)
           .setChannelCount(kInputChannels)   // stereo
           .setSampleRate(48000)
           // NO callback — we poll with read() from the output callback thread
           .setCallback(nullptr);

    oboe::Result result = builder.openStream(inputStream_);
    if (result != oboe::Result::OK) {
        LOGW("Input stream open failed (%s) — running output-only",
             oboe::convertToText(result));
        return false; // non-fatal: DSP runs on silence
    }
    LOGI("Input stream opened: sr=%d ch=%d",
         inputStream_->getSampleRate(), inputStream_->getChannelCount());
    return true;
}

// ── Open OUTPUT stream (callback driven, 6-ch) ────────────────────────────────
bool AudioEngine::openOutputStream() {
    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output)
           .setPerformanceMode(oboe::PerformanceMode::LowLatency)
           .setSharingMode(oboe::SharingMode::Exclusive)
           .setFormat(oboe::AudioFormat::Float)
           .setChannelCount(kNumOutputChannels)
           .setSampleRate(48000)
           .setCallback(this);

    oboe::Result result = builder.openStream(outputStream_);
    if (result != oboe::Result::OK) {
        LOGW("Exclusive 6-ch output failed (%s) — retrying Shared",
             oboe::convertToText(result));
        builder.setSharingMode(oboe::SharingMode::Shared);
        result = builder.openStream(outputStream_);
        if (result != oboe::Result::OK) {
            LOGE("Output stream open failed: %s", oboe::convertToText(result));
            return false;
        }
    }

    const float sr = static_cast<float>(outputStream_->getSampleRate());
    dspEngine_->prepare(sr);
    LOGI("Output stream opened: sr=%.0f ch=%d", sr, outputStream_->getChannelCount());
    return true;
}

bool AudioEngine::start() {
    if (running_.load()) return true;

    // Output stream is required; input is optional (graceful silent fallback)
    if (!openOutputStream()) { stop(); return false; }
    openInputStream(); // failure tolerated

    if (inputStream_) inputStream_->requestStart();
    auto outResult = outputStream_->requestStart();
    if (outResult != oboe::Result::OK) {
        LOGE("Output start failed: %s", oboe::convertToText(outResult));
        stop();
        return false;
    }

    running_.store(true);
    LOGI("AudioEngine started");
    return true;
}

void AudioEngine::stop() {
    if (inputStream_)  { inputStream_->requestStop();  inputStream_->close();  inputStream_.reset(); }
    if (outputStream_) { outputStream_->requestStop(); outputStream_->close(); outputStream_.reset(); }
    running_.store(false);
    LOGI("AudioEngine stopped");
}

// ── Real-time output callback ─────────────────────────────────────────────────
oboe::DataCallbackResult AudioEngine::onAudioReady(oboe::AudioStream* /*stream*/,
                                                    void* audioData,
                                                    int32_t numFrames)
{
    // Safety: never exceed pre-allocated scratch buffers
    if (numFrames > kMaxFrames) numFrames = kMaxFrames;

    // ── Read stereo input (non-blocking, 0 ns timeout) ─────────────────────
    if (inputStream_) {
        // inputInterleaved_ is sized kMaxFrames * kInputChannels — safe
        auto readResult = inputStream_->read(
            inputInterleaved_, numFrames, /*timeoutNanoseconds=*/0);

        if (readResult && readResult.value() > 0) {
            const int framesRead = readResult.value();
            // De-interleave LRLRLR… → separate L and R buffers
            for (int i = 0; i < framesRead; ++i) {
                inputBufL_[i] = inputInterleaved_[i * kInputChannels + 0];
                inputBufR_[i] = inputInterleaved_[i * kInputChannels + 1];
            }
            // Zero any unread frames (underrun safety)
            if (framesRead < numFrames) {
                std::fill(inputBufL_ + framesRead, inputBufL_ + numFrames, 0.0f);
                std::fill(inputBufR_ + framesRead, inputBufR_ + numFrames, 0.0f);
            }
        } else {
            // Underrun or error — pass silence to DSP
            std::fill(inputBufL_, inputBufL_ + numFrames, 0.0f);
            std::fill(inputBufR_, inputBufR_ + numFrames, 0.0f);
        }
    } else {
        std::fill(inputBufL_, inputBufL_ + numFrames, 0.0f);
        std::fill(inputBufR_, inputBufR_ + numFrames, 0.0f);
    }

    // ── DSP: stereo → 6-channel ────────────────────────────────────────────
    dspEngine_->processBlock(inputBufL_, inputBufR_, outputPtrs_, numFrames);

    // ── Interleave 6 channels → Oboe output buffer ─────────────────────────
    float* out = reinterpret_cast<float*>(audioData);
    for (int i = 0; i < numFrames; ++i)
        for (int ch = 0; ch < kNumOutputChannels; ++ch)
            out[i * kNumOutputChannels + ch] = outputBufs_[ch][i];

    return oboe::DataCallbackResult::Continue;
}

void AudioEngine::onErrorAfterClose(oboe::AudioStream* /*stream*/, oboe::Result error) {
    LOGW("Stream error: %s — attempting restart", oboe::convertToText(error));
    running_.store(false);
    start(); // Oboe calls this on a non-audio thread — safe to restart here
}
