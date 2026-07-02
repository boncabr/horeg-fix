#pragma once
#include <cmath>
#include <array>

/**
 * BiquadFilter — Second-order IIR filter (Direct Form II Transposed)
 *
 * Supports:
 *   - Parametric EQ  : Peaking, Low-Shelf, High-Shelf
 *   - Crossover      : Linkwitz-Riley 4th-order (LPF / HPF pair)
 *
 * Thread safety: NOT safe for concurrent access from multiple threads.
 * Call from the audio thread only.
 *
 * dlms losss — mas ari
 */

enum class FilterType {
    PEAKING,
    LOW_SHELF,
    HIGH_SHELF,
    LOW_PASS,   // 2nd-order Butterworth (used inside LR4)
    HIGH_PASS   // 2nd-order Butterworth (used inside LR4)
};

class BiquadFilter {
public:
    BiquadFilter() { reset(); }

    /**
     * Compute biquad coefficients from human-readable parameters.
     *
     * @param type      Filter type (see FilterType enum)
     * @param sampleRate  Host sample rate in Hz
     * @param freqHz    Centre / corner frequency in Hz
     * @param qFactor   Q / bandwidth factor (ignored for shelves: uses shelf slope S=1)
     * @param gainDb    Gain in dB — used by Peaking and Shelves (pass 0 for LPF/HPF)
     */
    void setParameters(FilterType type, float sampleRate,
                       float freqHz, float qFactor, float gainDb);

    /**
     * Process a single audio sample in-place (mono).
     * Returns the filtered sample.
     */
    inline float process(float x) {
        // Direct Form II transposed
        float y = b0_ * x + s1_;
        s1_ = b1_ * x - a1_ * y + s2_;
        s2_ = b2_ * x - a2_ * y;
        return y;
    }

    /** Hard-reset filter state (call on stream open / seek). */
    void reset() { s1_ = s2_ = 0.0f; }

private:
    // Normalised coefficients (a0 divided out)
    float b0_{1.0f}, b1_{0.0f}, b2_{0.0f};
    float        a1_{0.0f}, a2_{0.0f};

    // Filter state (transposed direct form II delay elements)
    float s1_{0.0f}, s2_{0.0f};
};
