#include "DlmsEngine.h"
#include "DwpParser.h"
#include <cmath>
#include <android/log.h>

#define LOG_TAG "DlmsEngine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)

/**
 * DlmsEngine — main DSP chain.
 *
 * PEQ thread-safety (snapshot pattern):
 *   - UI/JNI thread writes peqParams_ + peqDirty_ under peqMutex_.
 *   - Audio thread acquires peqMutex_ once per block to copy dirty rows into
 *     peqSnapshot_, then releases the lock and recomputes coefficients from the
 *     snapshot. The lock is never held during DSP math.
 *
 * dlms losss — mas ari
 */

DlmsEngine::DlmsEngine(float sampleRate)
    : limiters_{LookAheadLimiter(sampleRate), LookAheadLimiter(sampleRate),
                LookAheadLimiter(sampleRate), LookAheadLimiter(sampleRate),
                LookAheadLimiter(sampleRate), LookAheadLimiter(sampleRate)}
{
    for (int ch = 0; ch < kNumOutputChannels; ++ch)
        limiters_[ch].setParameters(limiterParams_[ch]);

    // Initialise snapshot to match defaults
    peqSnapshot_ = peqParams_;

    prepare(sampleRate);
}

void DlmsEngine::prepare(float sampleRate) {
    sampleRate_ = sampleRate;
    prevLowMidHz_  = -1.0f;
    prevMidHighHz_ = -1.0f;
    rebuildCrossover();

    for (int ch = 0; ch < kNumOutputChannels; ++ch)
        for (int b = 0; b < kNumPEQBands; ++b)
            refreshPEQBand(ch, b);

    for (int ch = 0; ch < kNumOutputChannels; ++ch)
        delays_[ch].setDelay(channelParams_[ch].delayMs.load(), sampleRate_);

    for (int ch = 0; ch < kNumOutputChannels; ++ch) {
        limiters_[ch] = LookAheadLimiter(sampleRate_);
        limiters_[ch].setParameters(limiterParams_[ch]);
    }

    reset();
}

void DlmsEngine::reset() {
    for (auto& f : xoLowLpf1_)  f.reset(); for (auto& f : xoLowLpf2_)  f.reset();
    for (auto& f : xoMidHpf1_)  f.reset(); for (auto& f : xoMidHpf2_)  f.reset();
    for (auto& f : xoMidLpf1_)  f.reset(); for (auto& f : xoMidLpf2_)  f.reset();
    for (auto& f : xoHighHpf1_) f.reset(); for (auto& f : xoHighHpf2_) f.reset();
    for (auto& ch : peqFilters_) for (auto& f : ch) f.reset();
    for (auto& d : delays_)   d.reset();
    for (auto& l : limiters_) l.reset();
}

// ── Crossover rebuild ─────────────────────────────────────────────────────────
void DlmsEngine::rebuildCrossover() {
    const float lm = crossoverParams_.lowMidHz .load(std::memory_order_relaxed);
    const float mh = crossoverParams_.midHighHz.load(std::memory_order_relaxed);
    if (lm == prevLowMidHz_ && mh == prevMidHighHz_) return;
    prevLowMidHz_  = lm;
    prevMidHighHz_ = mh;

    const float q = 0.70710678118f; // Butterworth Q for flat LR4 sum
    for (int s = 0; s < 2; ++s) {
        xoLowLpf1_ [s].setParameters(FilterType::LOW_PASS,  sampleRate_, lm, q, 0.f);
        xoLowLpf2_ [s].setParameters(FilterType::LOW_PASS,  sampleRate_, lm, q, 0.f);
        xoMidHpf1_ [s].setParameters(FilterType::HIGH_PASS, sampleRate_, lm, q, 0.f);
        xoMidHpf2_ [s].setParameters(FilterType::HIGH_PASS, sampleRate_, lm, q, 0.f);
        xoMidLpf1_ [s].setParameters(FilterType::LOW_PASS,  sampleRate_, mh, q, 0.f);
        xoMidLpf2_ [s].setParameters(FilterType::LOW_PASS,  sampleRate_, mh, q, 0.f);
        xoHighHpf1_[s].setParameters(FilterType::HIGH_PASS, sampleRate_, mh, q, 0.f);
        xoHighHpf2_[s].setParameters(FilterType::HIGH_PASS, sampleRate_, mh, q, 0.f);
    }
    LOGI("Crossover: lm=%.1f mh=%.1f Hz", lm, mh);
}

// ── PEQ ───────────────────────────────────────────────────────────────────────
void DlmsEngine::refreshPEQBand(int ch, int band) {
    // Called from audio thread using peqSnapshot_ (already copied under lock)
    const PEQBandParams& p = peqSnapshot_[ch][band];
    peqFilters_[ch][band].setParameters(p.type, sampleRate_, p.freqHz, p.q, p.gainDb);
    peqDirty_[ch][band] = false;
}

void DlmsEngine::setPEQBand(int ch, int band, const PEQBandParams& p) {
    // Called from UI/JNI thread
    std::lock_guard<std::mutex> lock(peqMutex_);
    peqParams_[ch][band] = p;
    peqDirty_ [ch][band] = true;
}

// ── Main processing block ─────────────────────────────────────────────────────
void DlmsEngine::processBlock(const float* inputL, const float* inputR,
                               float* outputs[kNumOutputChannels], int numFrames)
{
    // ── 1. Crossover frequency check (atomic reads, no lock) ───────────────
    rebuildCrossover();

    // ── 2. Snapshot PEQ params that were marked dirty (lock held briefly) ──
    {
        std::lock_guard<std::mutex> lock(peqMutex_);
        for (int ch = 0; ch < kNumOutputChannels; ++ch)
            for (int b = 0; b < kNumPEQBands; ++b)
                if (peqDirty_[ch][b]) {
                    peqSnapshot_[ch][b] = peqParams_[ch][b];
                    // keep peqDirty_ true so refreshPEQBand() rebuilds coeff
                }
    }
    // Rebuild biquad coefficients from snapshot (no lock held)
    for (int ch = 0; ch < kNumOutputChannels; ++ch)
        for (int b = 0; b < kNumPEQBands; ++b)
            if (peqDirty_[ch][b])
                refreshPEQBand(ch, b); // clears peqDirty_[ch][b]

    // ── 3. Update delay tap lengths (atomic reads) ─────────────────────────
    for (int ch = 0; ch < kNumOutputChannels; ++ch)
        delays_[ch].setDelay(channelParams_[ch].delayMs.load(std::memory_order_relaxed),
                             sampleRate_);

    // ── 4. Per-sample DSP ──────────────────────────────────────────────────
    for (int i = 0; i < numFrames; ++i) {
        const float inL = inputL[i];
        const float inR = inputR[i];

        // LR4 crossover → 3 stereo bands
        float lowL  = xoLowLpf2_ [0].process(xoLowLpf1_ [0].process(inL));
        float lowR  = xoLowLpf2_ [1].process(xoLowLpf1_ [1].process(inR));

        float midL  = xoMidLpf2_ [0].process(xoMidLpf1_ [0].process(
                      xoMidHpf2_ [0].process(xoMidHpf1_ [0].process(inL))));
        float midR  = xoMidLpf2_ [1].process(xoMidLpf1_ [1].process(
                      xoMidHpf2_ [1].process(xoMidHpf1_ [1].process(inR))));

        float highL = xoHighHpf2_[0].process(xoHighHpf1_[0].process(inL));
        float highR = xoHighHpf2_[1].process(xoHighHpf1_[1].process(inR));

        // Channel order: 0=LowL, 1=LowR, 2=MidL, 3=MidR, 4=HighL, 5=HighR
        float s[kNumOutputChannels] = { lowL, lowR, midL, midR, highL, highR };

        for (int ch = 0; ch < kNumOutputChannels; ++ch) {
            // Mute
            if (channelParams_[ch].mute.load(std::memory_order_relaxed)) {
                outputs[ch][i] = 0.0f;
                continue;
            }

            float x = s[ch];

            // Phase invert
            if (channelParams_[ch].phaseInvert.load(std::memory_order_relaxed)) x = -x;

            // Gain (dB → linear)
            x *= std::pow(10.0f, channelParams_[ch].gainDb.load(std::memory_order_relaxed) / 20.0f);

            // 8-band PEQ (coefficients already up-to-date from snapshot above)
            for (int b = 0; b < kNumPEQBands; ++b)
                if (peqSnapshot_[ch][b].enabled)
                    x = peqFilters_[ch][b].process(x);

            // Alignment delay
            x = delays_[ch].process(x);

            // Look-ahead limiter
            outputs[ch][i] = limiters_[ch].process(x);
        }
    }
}

// ── Apply preset ──────────────────────────────────────────────────────────────
void DlmsEngine::applyPreset(const DlmsPreset& preset) {
    crossoverParams_.lowMidHz .store(preset.crossoverLowMidHz,  std::memory_order_relaxed);
    crossoverParams_.midHighHz.store(preset.crossoverMidHighHz, std::memory_order_relaxed);

    for (int ch = 0; ch < kNumOutputChannels; ++ch) {
        const DwpChannelPreset& cp = preset.channels[ch];
        channelParams_[ch].gainDb     .store(cp.gainDb,          std::memory_order_relaxed);
        channelParams_[ch].mute       .store(cp.mute,            std::memory_order_relaxed);
        channelParams_[ch].phaseInvert.store(cp.phaseInvert,     std::memory_order_relaxed);
        channelParams_[ch].delayMs    .store(cp.delayMs,         std::memory_order_relaxed);
        limiterParams_[ch].thresholdDb.store(cp.limiterThreshDb, std::memory_order_relaxed);
        limiterParams_[ch].attackMs   .store(cp.limiterAttackMs, std::memory_order_relaxed);
        limiterParams_[ch].releaseMs  .store(cp.limiterRelMs,    std::memory_order_relaxed);

        for (int b = 0; b < kNumPEQBands; ++b) {
            PEQBandParams bp;
            bp.freqHz  = cp.peq[b].freqHz;
            bp.q       = cp.peq[b].q;
            bp.gainDb  = cp.peq[b].gainDb;
            bp.enabled = cp.peq[b].enabled;
            switch (cp.peq[b].type) {
                case 1:  bp.type = FilterType::LOW_SHELF;  break;
                case 2:  bp.type = FilterType::HIGH_SHELF; break;
                default: bp.type = FilterType::PEAKING;    break;
            }
            setPEQBand(ch, b, bp); // thread-safe via mutex
        }
    }
    LOGI("Preset applied: %s", preset.presetName);
}
