#pragma once
#include <array>
#include <atomic>
#include <vector>
#include <cstdint>
#include "BiquadFilter.h"

/**
 * DlmsEngine — Digital Loudspeaker Management System DSP Core
 *
 * Architecture:
 *   Stereo input → Linkwitz-Riley 3-way crossover
 *              → 6 mono output channels (Low L/R, Mid L/R, High L/R)
 *              → Per-channel: Gain → Phase → PEQ (8 bands) → Delay → Limiter
 *
 * Thread safety:
 *   processBlock() runs on the real-time audio thread.
 *   All parameter setters use std::atomic stores (relaxed), which are safe to
 *   call from any thread without locking the audio thread.
 *
 * dlms losss — mas ari
 */

// ── Constants ───────────────────────────────────────────────────────────────
static constexpr int   kNumOutputChannels  = 6;   // Low L, Low R, Mid L, Mid R, High L, High R
static constexpr int   kNumPEQBands        = 8;   // PEQ bands per output channel
static constexpr float kMaxDelayMs         = 20.0f;
static constexpr int   kMaxDelaySamples    = static_cast<int>(kMaxDelayMs / 1000.0f * 96000.0f + 1);

// ── Crossover band indices ───────────────────────────────────────────────────
enum CrossoverBand { LOW = 0, MID = 1, HIGH = 2 };

// ── Per-channel PEQ band parameters ─────────────────────────────────────────
struct PEQBandParams {
    float    freqHz  {1000.0f};
    float    q       {0.707f};
    float    gainDb  {0.0f};
    FilterType type  {FilterType::PEAKING};
    bool     enabled {true};
};

// ── Per-output-channel DSP parameters (all atomics for lock-free UI updates) ─
struct ChannelParams {
    std::atomic<float> gainDb{0.0f};         // Channel gain in dB
    std::atomic<bool>  mute{false};           // Mute switch
    std::atomic<bool>  phaseInvert{false};    // Phase invert (180°)
    std::atomic<float> delayMs{0.0f};         // Alignment delay 0–20 ms
};

// ── Crossover parameters ─────────────────────────────────────────────────────
struct CrossoverParams {
    std::atomic<float> lowMidHz{200.0f};   // Low / Mid crossover frequency
    std::atomic<float> midHighHz{2000.0f}; // Mid / High crossover frequency
};

// ── Limiter parameters (per channel) ────────────────────────────────────────
struct LimiterParams {
    std::atomic<float> thresholdDb{-3.0f};  // Brickwall threshold
    std::atomic<float> attackMs   {0.5f};   // Attack time
    std::atomic<float> releaseMs  {50.0f};  // Release time
    // Look-ahead delay line length (fixed at 1 ms for zero-clip guarantee)
    static constexpr float kLookAheadMs = 1.0f;
};

// ── Ring-buffer delay (per channel) ─────────────────────────────────────────
class AlignmentDelay {
public:
    AlignmentDelay() : buffer_(kMaxDelaySamples, 0.0f), writeIndex_(0), delaySamples_(0) {}

    void setDelay(float ms, float sampleRate) {
        int samples = static_cast<int>(ms / 1000.0f * sampleRate);
        samples = std::clamp(samples, 0, kMaxDelaySamples - 1);
        delaySamples_ = samples;
    }

    inline float process(float input) {
        buffer_[writeIndex_] = input;
        int readIndex = (writeIndex_ - delaySamples_ + kMaxDelaySamples) % kMaxDelaySamples;
        writeIndex_ = (writeIndex_ + 1) % kMaxDelaySamples;
        return buffer_[readIndex];
    }

    void reset() { std::fill(buffer_.begin(), buffer_.end(), 0.0f); writeIndex_ = 0; }

private:
    std::vector<float> buffer_;
    int writeIndex_;
    int delaySamples_;
};

// ── Look-Ahead Limiter ────────────────────────────────────────────────────────
class LookAheadLimiter {
public:
    explicit LookAheadLimiter(float sampleRate)
        : sampleRate_(sampleRate), envelope_(0.0f) {
        const int lookAheadSamples = static_cast<int>(
            LimiterParams::kLookAheadMs / 1000.0f * sampleRate);
        lookAheadBuffer_.assign(lookAheadSamples, 0.0f);
        writePos_ = 0;
    }

    void setParameters(const LimiterParams& p) { params_ = &p; }

    inline float process(float input) {
        if (!params_) return input;

        const float threshold = std::pow(10.0f, params_->thresholdDb.load(std::memory_order_relaxed) / 20.0f);
        const float attackCoeff  = std::exp(-1.0f / (params_->attackMs .load(std::memory_order_relaxed) / 1000.0f * sampleRate_));
        const float releaseCoeff = std::exp(-1.0f / (params_->releaseMs.load(std::memory_order_relaxed) / 1000.0f * sampleRate_));

        // Write into look-ahead ring buffer
        lookAheadBuffer_[writePos_] = input;
        int readPos = (writePos_ + 1) % static_cast<int>(lookAheadBuffer_.size());
        float delayed = lookAheadBuffer_[readPos];
        writePos_ = (writePos_ + 1) % static_cast<int>(lookAheadBuffer_.size());

        // Gain computer: detect peak level of incoming sample
        float level = std::fabs(input);
        float targetGain = (level > threshold && level > 1e-9f) ? threshold / level : 1.0f;

        // Smooth envelope (attack faster when gain needs to drop)
        if (targetGain < envelope_)
            envelope_ = attackCoeff  * envelope_ + (1.0f - attackCoeff)  * targetGain;
        else
            envelope_ = releaseCoeff * envelope_ + (1.0f - releaseCoeff) * targetGain;

        return delayed * envelope_;
    }

    void reset() {
        std::fill(lookAheadBuffer_.begin(), lookAheadBuffer_.end(), 0.0f);
        envelope_ = 1.0f;
        writePos_ = 0;
    }

private:
    float sampleRate_;
    std::vector<float> lookAheadBuffer_;
    int writePos_;
    float envelope_;
    const LimiterParams* params_{nullptr};
};

// ── Main DSP Engine ──────────────────────────────────────────────────────────
class DlmsEngine {
public:
    explicit DlmsEngine(float sampleRate = 48000.0f);

    /** Call when the audio stream is opened / sample rate changes. */
    void prepare(float sampleRate);

    /**
     * Process one stereo input block, producing 6 mono output channels.
     *
     * @param inputL    Left input buffer  [numFrames]
     * @param inputR    Right input buffer [numFrames]
     * @param outputs   6 output channel pointers, each [numFrames]
     * @param numFrames Number of audio frames per block
     */
    void processBlock(const float* inputL, const float* inputR,
                      float* outputs[kNumOutputChannels], int numFrames);

    // ── Parameter accessors (called from JNI / UI thread) ─────────────────
    ChannelParams&    channelParams(int ch)  { return channelParams_[ch]; }
    CrossoverParams&  crossoverParams()      { return crossoverParams_; }
    LimiterParams&    limiterParams(int ch)  { return limiterParams_[ch]; }

    /** Update PEQ band on specified output channel. Safe to call from UI thread. */
    void setPEQBand(int channel, int band, const PEQBandParams& p);

    /** Apply all DSP parameters from a loaded .dwp preset. */
    void applyPreset(const struct DlmsPreset& preset);

    void reset();

private:
    float sampleRate_{48000.0f};

    // ── Crossover filters (LR4 = two cascaded 2nd-order Butterworth) ──────
    // Low-Mid split: LPF stage for low, HPF stage for mid/high
    // Mid-High split: LPF stage for mid, HPF stage for high
    // Stereo, so 2 sets per split
    std::array<BiquadFilter, 2> xoLowLpf1_, xoLowLpf2_;   // Low LR4 LPF  (L, R)
    std::array<BiquadFilter, 2> xoMidHpf1_, xoMidHpf2_;   // Mid LR4 HPF  (L, R)
    std::array<BiquadFilter, 2> xoMidLpf1_, xoMidLpf2_;   // Mid LR4 LPF  (L, R)
    std::array<BiquadFilter, 2> xoHighHpf1_, xoHighHpf2_; // High LR4 HPF (L, R)

    // ── Per-channel PEQ (8 bands × 6 channels) ────────────────────────────
    std::array<std::array<BiquadFilter, kNumPEQBands>, kNumOutputChannels> peqFilters_;
    std::array<std::array<PEQBandParams, kNumPEQBands>, kNumOutputChannels> peqParams_;
    // Dirty flag: re-compute coefficients only when parameters change
    std::array<std::array<std::atomic<bool>, kNumPEQBands>, kNumOutputChannels> peqDirty_;

    // ── Per-channel alignment delay ────────────────────────────────────────
    std::array<AlignmentDelay, kNumOutputChannels> delays_;

    // ── Per-channel look-ahead limiter ────────────────────────────────────
    std::array<LookAheadLimiter, kNumOutputChannels> limiters_;

    // ── Shared parameter structs ───────────────────────────────────────────
    std::array<ChannelParams, kNumOutputChannels> channelParams_;
    CrossoverParams crossoverParams_;
    std::array<LimiterParams, kNumOutputChannels> limiterParams_;

    // Previous crossover frequencies to detect changes
    float prevLowMidHz_  {-1.0f};
    float prevMidHighHz_ {-1.0f};

    // ── Private helpers ────────────────────────────────────────────────────
    void rebuildCrossover();
    void refreshPEQBand(int channel, int band);
};
