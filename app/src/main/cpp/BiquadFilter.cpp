#include "BiquadFilter.h"
#include <cmath>

/**
 * BiquadFilter coefficient computation.
 *
 * References:
 *   - Audio EQ Cookbook — Robert Bristow-Johnson
 *   - Linkwitz-Riley filters — composed as two cascaded Butterworth stages
 *
 * dlms losss — mas ari
 */

static constexpr float kPi  = static_cast<float>(M_PI);
static constexpr float kTwoPi = 2.0f * kPi;

void BiquadFilter::setParameters(FilterType type, float sampleRate,
                                  float freqHz, float qFactor, float gainDb)
{
    // Clamp to safe ranges to avoid NaN / infinity in DSP chain
    freqHz   = std::clamp(freqHz,   20.0f,  sampleRate * 0.499f);
    qFactor  = std::clamp(qFactor,  0.1f,   100.0f);

    const float A  = std::pow(10.0f, gainDb / 40.0f); // sqrt(10^(dB/20))
    const float w0 = kTwoPi * freqHz / sampleRate;
    const float cosW0 = std::cos(w0);
    const float sinW0 = std::sin(w0);
    const float alpha = sinW0 / (2.0f * qFactor);

    float b0, b1, b2, a0, a1, a2;

    switch (type) {

    // ── Parametric / Peaking EQ ─────────────────────────────────────────
    case FilterType::PEAKING:
        b0 =  1.0f + alpha * A;
        b1 = -2.0f * cosW0;
        b2 =  1.0f - alpha * A;
        a0 =  1.0f + alpha / A;
        a1 = -2.0f * cosW0;
        a2 =  1.0f - alpha / A;
        break;

    // ── Low Shelf (shelf slope S=1) ─────────────────────────────────────
    case FilterType::LOW_SHELF: {
        const float sqrtA = std::sqrt(A);
        const float alphaS = sinW0 / 2.0f * std::sqrt((A + 1.0f / A) * (1.0f / 1.0f - 1.0f) + 2.0f);
        b0 =     A * ((A + 1.0f) - (A - 1.0f) * cosW0 + 2.0f * sqrtA * alphaS);
        b1 = 2.0f*A * ((A - 1.0f) - (A + 1.0f) * cosW0);
        b2 =     A * ((A + 1.0f) - (A - 1.0f) * cosW0 - 2.0f * sqrtA * alphaS);
        a0 =          (A + 1.0f) + (A - 1.0f) * cosW0 + 2.0f * sqrtA * alphaS;
        a1 =   -2.0f * ((A - 1.0f) + (A + 1.0f) * cosW0);
        a2 =          (A + 1.0f) + (A - 1.0f) * cosW0 - 2.0f * sqrtA * alphaS;
        break;
    }

    // ── High Shelf (shelf slope S=1) ────────────────────────────────────
    case FilterType::HIGH_SHELF: {
        const float sqrtA = std::sqrt(A);
        const float alphaS = sinW0 / 2.0f * std::sqrt((A + 1.0f / A) * (1.0f / 1.0f - 1.0f) + 2.0f);
        b0 =      A * ((A + 1.0f) + (A - 1.0f) * cosW0 + 2.0f * sqrtA * alphaS);
        b1 = -2.0f*A * ((A - 1.0f) + (A + 1.0f) * cosW0);
        b2 =      A * ((A + 1.0f) + (A - 1.0f) * cosW0 - 2.0f * sqrtA * alphaS);
        a0 =           (A + 1.0f) - (A - 1.0f) * cosW0 + 2.0f * sqrtA * alphaS;
        a1 =    2.0f * ((A - 1.0f) - (A + 1.0f) * cosW0);
        a2 =           (A + 1.0f) - (A - 1.0f) * cosW0 - 2.0f * sqrtA * alphaS;
        break;
    }

    // ── 2nd-order Low-Pass (Butterworth, used inside LR4) ───────────────
    case FilterType::LOW_PASS:
        b0 = (1.0f - cosW0) / 2.0f;
        b1 =  1.0f - cosW0;
        b2 = (1.0f - cosW0) / 2.0f;
        a0 =  1.0f + alpha;
        a1 = -2.0f * cosW0;
        a2 =  1.0f - alpha;
        break;

    // ── 2nd-order High-Pass (Butterworth, used inside LR4) ──────────────
    case FilterType::HIGH_PASS:
        b0 =  (1.0f + cosW0) / 2.0f;
        b1 = -(1.0f + cosW0);
        b2 =  (1.0f + cosW0) / 2.0f;
        a0 =   1.0f + alpha;
        a1 =  -2.0f * cosW0;
        a2 =   1.0f - alpha;
        break;
    }

    // Normalise by a0
    b0_ = b0 / a0;
    b1_ = b1 / a0;
    b2_ = b2 / a0;
    a1_ = a1 / a0;
    a2_ = a2 / a0;

    // Reset state to avoid transients when parameters change mid-stream
    reset();
}
