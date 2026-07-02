#pragma once
#include <array>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstdint>
#include "BiquadFilter.h"

/**
 * DlmsEngine — Digital Loudspeaker Management System DSP Core
 *
 * Thread safety model:
 *   - processBlock() runs on the real-time audio thread.
 *   - Scalar channel parameters (gain, mute, phase, delay) are std::atomic —
 *     safe to write from UI/JNI thread with no locking.
 *   - PEQ band structs (multi-field, non-atomic) are protected by peqMutex_.
 *     The audio thread copies the struct under the lock at the start of each
 *     block (snapshot pattern) — the lock is held only for a memcpy, so the
 *     lock is never held during the actual DSP math.
 *
 * dlms losss — mas ari
 */

static constexpr int   kNumOutputChannels  = 6;
static constexpr int   kNumPEQBands        = 8;
static constexpr float kMaxDelayMs         = 20.0f;
static constexpr int   kMaxDelaySamples    = static_cast<int>(kMaxDelayMs / 1000.0f * 96000.0f + 1);

enum CrossoverBand { LOW = 0, MID = 1, HIGH = 2 };

// ── PEQ band parameter struct (written from UI, snapshotted to audio thread) ──
struct PEQBandParams {
    float      freqHz  {1000.0f};
    float      q       {0.707f};
    float      gainDb  {0.0f};
    FilterType type    {FilterType::PEAKING};
    bool       enabled {true};
};

// ── Atomic channel strip parameters ──────────────────────────────────────────
struct ChannelParams {
    std::atomic<float> gainDb{0.0f};
    std::atomic<bool>  mute{false};
    std::atomic<bool>  phaseInvert{false};
    std::atomic<float> delayMs{0.0f};
};

// ── Crossover parameters ──────────────────────────────────────────────────────
struct CrossoverParams {
    std::atomic<float> lowMidHz{200.0f};
    std::atomic<float> midHighHz{2000.0f};
};

// ── Limiter parameters (all atomic) ──────────────────────────────────────────
struct LimiterParams {
    std::atomic<float> thresholdDb{-3.0f};
    std::atomic<float> attackMs   {0.5f};
    std::atomic<float> releaseMs  {50.0f};
    static constexpr float kLookAheadMs = 1.0f;
};

// ── Alignment delay (ring buffer) ─────────────────────────────────────────────
class AlignmentDelay {
public:
    AlignmentDelay() : buffer_(kMaxDelaySamples, 0.0f) {}

    void setDelay(float ms, float sampleRate) {
        int s = static_cast<int>(ms / 1000.0f * sampleRate);
        delaySamples_ = std::clamp(s, 0, kMaxDelaySamples - 1);
    }

    inline float process(float in) {
        buffer_[writeIndex_] = in;
        int ri = (writeIndex_ - delaySamples_ + kMaxDelaySamples) % kMaxDelaySamples;
        writeIndex_ = (writeIndex_ + 1) % kMaxDelaySamples;
        return buffer_[ri];
    }

    void reset() { std::fill(buffer_.begin(), buffer_.end(), 0.0f); writeIndex_ = 0; }

private:
    std::vector<float> buffer_;
    int writeIndex_    {0};
    int delaySamples_  {0};
};

// ── Look-ahead limiter ────────────────────────────────────────────────────────
class LookAheadLimiter {
public:
    explicit LookAheadLimiter(float sr) : sampleRate_(sr), envelope_(1.0f) {
        int la = static_cast<int>(LimiterParams::kLookAheadMs / 1000.0f * sr);
        lookAheadBuffer_.assign(std::max(la, 1), 0.0f);
    }

    void setParameters(const LimiterParams& p) { params_ = &p; }

    inline float process(float in) {
        if (!params_) return in;

        const float thresh   = std::pow(10.0f, params_->thresholdDb.load(std::memory_order_relaxed) / 20.0f);
        // Clamp times to avoid exp(-inf) or exp(0)
        const float atkMs    = std::max(params_->attackMs .load(std::memory_order_relaxed), 0.01f);
        const float relMs    = std::max(params_->releaseMs.load(std::memory_order_relaxed), 1.0f);
        const float atkC     = std::exp(-1.0f / (atkMs  / 1000.0f * sampleRate_));
        const float relC     = std::exp(-1.0f / (relMs  / 1000.0f * sampleRate_));

        const int sz = static_cast<int>(lookAheadBuffer_.size());
        lookAheadBuffer_[writePos_] = in;
        int readPos = (writePos_ + 1) % sz;
        float delayed = lookAheadBuffer_[readPos];
        writePos_ = (writePos_ + 1) % sz;

        float level  = std::fabs(in);
        float target = (level > thresh && level > 1e-9f) ? thresh / level : 1.0f;

        envelope_ = (target < envelope_)
            ? atkC * envelope_ + (1.0f - atkC) * target
            : relC * envelope_ + (1.0f - relC) * target;

        return delayed * envelope_;
    }

    void reset() {
        std::fill(lookAheadBuffer_.begin(), lookAheadBuffer_.end(), 0.0f);
        envelope_ = 1.0f; writePos_ = 0;
    }

private:
    float              sampleRate_;
    std::vector<float> lookAheadBuffer_;
    int                writePos_{0};
    float              envelope_;
    const LimiterParams* params_{nullptr};
};

// ── DlmsPreset (forward declared for applyPreset) ─────────────────────────────
struct DwpPEQBand;
struct DwpChannelPreset;
struct DlmsPreset;

// ── Main DSP Engine ───────────────────────────────────────────────────────────
class DlmsEngine {
public:
    explicit DlmsEngine(float sampleRate = 48000.0f);

    void prepare(float sampleRate);

    void processBlock(const float* inputL, const float* inputR,
                      float* outputs[kNumOutputChannels], int numFrames);

    ChannelParams&   channelParams(int ch) { return channelParams_[ch]; }
    CrossoverParams& crossoverParams()     { return crossoverParams_; }
    LimiterParams&   limiterParams(int ch) { return limiterParams_[ch]; }

    /**
     * Thread-safe PEQ band update.
     * Acquires peqMutex_ briefly to write the struct, sets dirty flag.
     */
    void setPEQBand(int channel, int band, const PEQBandParams& p);

    void applyPreset(const DlmsPreset& preset);
    void reset();

private:
    float sampleRate_{48000.0f};

    // Crossover (LR4 = two cascaded 2nd-order Butterworth per side)
    std::array<BiquadFilter, 2> xoLowLpf1_,  xoLowLpf2_;
    std::array<BiquadFilter, 2> xoMidHpf1_,  xoMidHpf2_;
    std::array<BiquadFilter, 2> xoMidLpf1_,  xoMidLpf2_;
    std::array<BiquadFilter, 2> xoHighHpf1_, xoHighHpf2_;

    // PEQ biquad state (audio thread only — no locking needed for filter state)
    std::array<std::array<BiquadFilter,    kNumPEQBands>, kNumOutputChannels> peqFilters_;

    // PEQ parameters — shared between UI and audio threads.
    // Written under peqMutex_; audio thread snapshots under peqMutex_ once per block.
    std::mutex peqMutex_;
    std::array<std::array<PEQBandParams, kNumPEQBands>, kNumOutputChannels> peqParams_;
    std::array<std::array<bool,          kNumPEQBands>, kNumOutputChannels> peqDirty_{};

    // Local snapshot (audio thread only — no locking after copy)
    std::array<std::array<PEQBandParams, kNumPEQBands>, kNumOutputChannels> peqSnapshot_;

    std::array<AlignmentDelay,    kNumOutputChannels> delays_;
    std::array<LookAheadLimiter,  kNumOutputChannels> limiters_;

    std::array<ChannelParams, kNumOutputChannels> channelParams_;
    CrossoverParams  crossoverParams_;
    std::array<LimiterParams, kNumOutputChannels> limiterParams_;

    float prevLowMidHz_  {-1.0f};
    float prevMidHighHz_ {-1.0f};

    void rebuildCrossover();
    void refreshPEQBand(int ch, int band);
};
