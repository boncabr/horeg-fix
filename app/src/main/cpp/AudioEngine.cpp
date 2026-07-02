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
    // Oboe fluent setters return AudioStreamBuilder* — use -> for chaining
    oboe::Result result = builder
        .setDirection(oboe::Direction::Input)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setSharingMode(oboe::SharingMode::Exclusive)
        ->setFormat(oboe::AudioFormat::Float)
        ->setChannelCount(kInputChannels)
        ->setSampleRate(48000)
        ->setCallback(nullptr)   // non-callback: we poll with read() from output thread
        ->openStream(inputStream_);

    if (result != oboe::Result::OK) {
        LOGW("Input stream open failed (%s) — output-only mode",
             oboe::convertToText(result));
        return false;
    }
    LOGI("Input stream: sr=%d ch=%d",
         inputStream_->getSampleRate(), inputStream_->getChannelCount());
    return true;
}

// ── Open OUTPUT stream (callback driven, 6-ch) ────────────────────────────────
bool AudioEngine::openOutputStream() {
    oboe::AudioStreamBuilder builder;
    oboe::Result result = builder
        .setDirection(oboe::Direction::Output)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setSharingMode(oboe::SharingMode::Exclusive)
        ->setFormat(oboe::AudioFormat::Float)
        ->setChannelCount(kNumOutputChannels)
        ->setSampleRate(48000)
        ->setCallback(this)
        ->openStream(outputStream_);

    if (result != oboe::Result::OK) {
        LOGW("Exclusive 6-ch output failed (%s) — retrying Shared",
             oboe::convertToText(result));
        oboe::AudioStreamBuilder builder2;
        result = builder2
            .setDirection(oboe::Direction::Output)
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setSharingMode(oboe::SharingMode::Shared)
            ->setFormat(oboe::AudioFormat::Float)
            ->setChannelCount(kNumOutputChannels)
            ->setSampleRate(48000)
            ->setCallback(this)
            ->openStream(outputStream_);

        if (result != oboe::Result::OK) {
            LOGE("Output stream open failed: %s", oboe::convertToText(result));
            return false;
        }
    }

    const float sr = static_cast<float>(outputStream_->getSampleRate());
    dspEngine_->prepare(sr);
    LOGI("Output stream: sr=%.0f ch=%d", sr, outputStream_->getChannelCount());
    return true;
}

bool AudioEngine::start() {
    if (running_.load()) return true;
    if (!openOutputStream()) { stop(); return false; }
    openInputStream(); // failure tolerated — runs on silence

    if (inputStream_)  inputStream_->requestStart();
    auto outResult = outputStream_->requestStart();
    if (outResult != oboe::Result::OK) {
        LOGE("Output start failed: %s", oboe::convertToText(outResult));
        stop(); return false;
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
    if (numFrames > kMaxFrames) numFrames = kMaxFrames;

    if (inputStream_) {
        auto readResult = inputStream_->read(
            inputInterleaved_, numFrames, /*timeoutNanoseconds=*/0);
        if (readResult && readResult.value() > 0) {
            const int n = readResult.value();
            for (int i = 0; i < n; ++i) {
                inputBufL_[i] = inputInterleaved_[i * kInputChannels + 0];
                inputBufR_[i] = inputInterleaved_[i * kInputChannels + 1];
            }
            if (n < numFrames) {
                std::fill(inputBufL_ + n, inputBufL_ + numFrames, 0.0f);
                std::fill(inputBufR_ + n, inputBufR_ + numFrames, 0.0f);
            }
        } else {
            std::fill(inputBufL_, inputBufL_ + numFrames, 0.0f);
            std::fill(inputBufR_, inputBufR_ + numFrames, 0.0f);
        }
    } else {
        std::fill(inputBufL_, inputBufL_ + numFrames, 0.0f);
        std::fill(inputBufR_, inputBufR_ + numFrames, 0.0f);
    }

    dspEngine_->processBlock(inputBufL_, inputBufR_, outputPtrs_, numFrames);

    float* out = reinterpret_cast<float*>(audioData);
    for (int i = 0; i < numFrames; ++i)
        for (int ch = 0; ch < kNumOutputChannels; ++ch)
            out[i * kNumOutputChannels + ch] = outputBufs_[ch][i];

    return oboe::DataCallbackResult::Continue;
}

void AudioEngine::onErrorAfterClose(oboe::AudioStream* /*stream*/, oboe::Result error) {
    LOGW("Stream error: %s — restarting", oboe::convertToText(error));
    running_.store(false);
    start();
}
