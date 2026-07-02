#include "DlmsEngine.h"
#include <cmath>
#include <android/log.h>

#define LOG_TAG "DlmsEngine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)

/**
 * DlmsEngine — main DSP processing loop.
 * dlms losss — mas ari
 */

DlmsEngine::DlmsEngine(float sampleRate)
    : limiters_{LookAheadLimiter(sampleRate), LookAheadLimiter(sampleRate),
                LookAheadLimiter(sampleRate), LookAheadLimiter(sampleRate),
                LookAheadLimiter(sampleRate), LookAheadLimiter(sampleRate)}
{
    // Wire limiter parameter pointers
    for (int ch = 0; ch < kNumOutputChannels; ++ch) {
        limiters_[ch].setParameters(limiterParams_[ch]);
    }

    // Mark all PEQ bands as dirty so coefficients are computed on first block
    for (int ch = 0; ch < kNumOutputChannels; ++ch)
        for (int b = 0; b < kNumPEQBands; ++b)
            peqDirty_[ch][b].store(true, std::memory_order_relaxed);

    prepare(sampleRate);
}

void DlmsEngine::prepare(float sampleRate) {
    sampleRate_ = sampleRate;
    LOGI("prepare: sampleRate=%.0f", sampleRate);

    // Force crossover rebuild
    prevLowMidHz_  = -1.0f;
    prevMidHighHz_ = -1.0f;
    rebuildCrossover();

    // Rebuild all PEQ bands
    for (int ch = 0; ch < kNumOutputChannels; ++ch)
        for (int b = 0; b < kNumPEQBands; ++b)
            refreshPEQBand(ch, b);

    // Update delay sample counts
    for (int ch = 0; ch < kNumOutputChannels; ++ch)
        delays_[ch].setDelay(channelParams_[ch].delayMs.load(), sampleRate_);

    // Recreate limiters with new sample rate
    for (int ch = 0; ch < kNumOutputChannels; ++ch) {
        limiters_[ch] = LookAheadLimiter(sampleRate_);
        limiters_[ch].setParameters(limiterParams_[ch]);
    }

    reset();
}

void DlmsEngine::reset() {
    for (auto& f : xoLowLpf1_)  f.reset();
    for (auto& f : xoLowLpf2_)  f.reset();
    for (auto& f : xoMidHpf1_)  f.reset();
    for (auto& f : xoMidHpf2_)  f.reset();
    for (auto& f : xoMidLpf1_)  f.reset();
    for (auto& f : xoMidLpf2_)  f.reset();
    for (auto& f : xoHighHpf1_) f.reset();
    for (auto& f : xoHighHpf2_) f.reset();
    for (auto& ch : peqFilters_) for (auto& f : ch) f.reset();
    for (auto& d : delays_)  d.reset();
    for (auto& l : limiters_) l.reset();
}

// ── Crossover rebuild ─────────────────────────────────────────────────────────
void DlmsEngine::rebuildCrossover() {
    const float lowMidHz  = crossoverParams_.lowMidHz .load(std::memory_order_relaxed);
    const float midHighHz = crossoverParams_.midHighHz.load(std::memory_order_relaxed);

    if (lowMidHz == prevLowMidHz_ && midHighHz == prevMidHighHz_) return;
    prevLowMidHz_  = lowMidHz;
    prevMidHighHz_ = midHighHz;

    // Linkwitz-Riley 4th-order = two cascaded 2nd-order Butterworth (Q = 0.7071)
    const float sqrtHalf = 0.70710678118f; // 1/sqrt(2) — Butterworth Q for flat summing
    for (int s = 0; s < 2; ++s) { // s=0 → Left, s=1 → Right
        // Low path: two cascaded LPF at lowMidHz
        xoLowLpf1_[s].setParameters(FilterType::LOW_PASS,  sampleRate_, lowMidHz,  sqrtHalf, 0.0f);
        xoLowLpf2_[s].setParameters(FilterType::LOW_PASS,  sampleRate_, lowMidHz,  sqrtHalf, 0.0f);
        // Mid path HPF side
        xoMidHpf1_[s].setParameters(FilterType::HIGH_PASS, sampleRate_, lowMidHz,  sqrtHalf, 0.0f);
        xoMidHpf2_[s].setParameters(FilterType::HIGH_PASS, sampleRate_, lowMidHz,  sqrtHalf, 0.0f);
        // Mid path LPF side
        xoMidLpf1_[s].setParameters(FilterType::LOW_PASS,  sampleRate_, midHighHz, sqrtHalf, 0.0f);
        xoMidLpf2_[s].setParameters(FilterType::LOW_PASS,  sampleRate_, midHighHz, sqrtHalf, 0.0f);
        // High path HPF
        xoHighHpf1_[s].setParameters(FilterType::HIGH_PASS, sampleRate_, midHighHz, sqrtHalf, 0.0f);
        xoHighHpf2_[s].setParameters(FilterType::HIGH_PASS, sampleRate_, midHighHz, sqrtHalf, 0.0f);
    }
    LOGI("Crossover rebuilt: lowMid=%.1f Hz, midHigh=%.1f Hz", lowMidHz, midHighHz);
}

// ── PEQ band refresh ──────────────────────────────────────────────────────────
void DlmsEngine::refreshPEQBand(int ch, int band) {
    const PEQBandParams& p = peqParams_[ch][band];
    peqFilters_[ch][band].setParameters(p.type, sampleRate_, p.freqHz, p.q, p.gainDb);
    peqDirty_[ch][band].store(false, std::memory_order_relaxed);
}

void DlmsEngine::setPEQBand(int ch, int band, const PEQBandParams& p) {
    peqParams_[ch][band] = p;
    peqDirty_[ch][band].store(true, std::memory_order_relaxed);
}

// ── Main audio block processing ───────────────────────────────────────────────
void DlmsEngine::processBlock(const float* inputL, const float* inputR,
                               float* outputs[kNumOutputChannels], int numFrames)
{
    // Check if crossover needs rebuild (frequency changed from UI thread)
    rebuildCrossover();

    // Check if delay settings changed
    for (int ch = 0; ch < kNumOutputChannels; ++ch) {
        delays_[ch].setDelay(channelParams_[ch].delayMs.load(std::memory_order_relaxed), sampleRate_);
    }

    // Process sample by sample (scalar; NEON vectorisation handled by compiler -O2 -ffast-math)
    for (int i = 0; i < numFrames; ++i) {
        const float inL = inputL[i];
        const float inR = inputR[i];

        // ── 3-way Linkwitz-Riley crossover ────────────────────────────────
        // Low path (LR4 LPF)
        float lowL = xoLowLpf2_[0].process(xoLowLpf1_[0].process(inL));
        float lowR = xoLowLpf2_[1].process(xoLowLpf1_[1].process(inR));

        // Mid path: HPF then LPF (band-pass via crossover)
        float midL = xoMidLpf2_[0].process(xoMidLpf1_[0].process(
                     xoMidHpf2_[0].process(xoMidHpf1_[0].process(inL))));
        float midR = xoMidLpf2_[1].process(xoMidLpf1_[1].process(
                     xoMidHpf2_[1].process(xoMidHpf1_[1].process(inR))));

        // High path (LR4 HPF)
        float highL = xoHighHpf2_[0].process(xoHighHpf1_[0].process(inL));
        float highR = xoHighHpf2_[1].process(xoHighHpf1_[1].process(inR));

        // Map to channel order: 0=LowL, 1=LowR, 2=MidL, 3=MidR, 4=HighL, 5=HighR
        float chSample[kNumOutputChannels] = { lowL, lowR, midL, midR, highL, highR };

        // ── Per-channel processing ─────────────────────────────────────────
        for (int ch = 0; ch < kNumOutputChannels; ++ch) {
            float s = chSample[ch];

            // Mute
            if (channelParams_[ch].mute.load(std::memory_order_relaxed)) {
                outputs[ch][i] = 0.0f;
                continue;
            }

            // Phase invert
            if (channelParams_[ch].phaseInvert.load(std::memory_order_relaxed)) s = -s;

            // Gain (dB → linear)
            const float gainDb = channelParams_[ch].gainDb.load(std::memory_order_relaxed);
            s *= std::pow(10.0f, gainDb / 20.0f);

            // PEQ — 8 bands
            for (int b = 0; b < kNumPEQBands; ++b) {
                if (peqDirty_[ch][b].load(std::memory_order_relaxed))
                    refreshPEQBand(ch, b);
                if (peqParams_[ch][b].enabled)
                    s = peqFilters_[ch][b].process(s);
            }

            // Alignment delay
            s = delays_[ch].process(s);

            // Look-ahead limiter
            s = limiters_[ch].process(s);

            outputs[ch][i] = s;
        }
    }
}

// ── Apply loaded preset ───────────────────────────────────────────────────────
void DlmsEngine::applyPreset(const DlmsPreset& preset) {
    crossoverParams_.lowMidHz .store(preset.crossoverLowMidHz,  std::memory_order_relaxed);
    crossoverParams_.midHighHz.store(preset.crossoverMidHighHz, std::memory_order_relaxed);

    for (int ch = 0; ch < kNumOutputChannels; ++ch) {
        const DwpChannelPreset& cp = preset.channels[ch];
        channelParams_[ch].gainDb      .store(cp.gainDb,       std::memory_order_relaxed);
        channelParams_[ch].mute        .store(cp.mute,         std::memory_order_relaxed);
        channelParams_[ch].phaseInvert .store(cp.phaseInvert,  std::memory_order_relaxed);
        channelParams_[ch].delayMs     .store(cp.delayMs,      std::memory_order_relaxed);
        limiterParams_[ch].thresholdDb .store(cp.limiterThreshDb, std::memory_order_relaxed);
        limiterParams_[ch].attackMs    .store(cp.limiterAttackMs, std::memory_order_relaxed);
        limiterParams_[ch].releaseMs   .store(cp.limiterRelMs,    std::memory_order_relaxed);
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
            setPEQBand(ch, b, bp);
        }
    }
    LOGI("Preset applied: %s", preset.presetName);
}
