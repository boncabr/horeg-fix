package com.masari.dlmslosss

/**
 * DlmsNativeInterface — Kotlin bridge to the C++ DSP engine via JNI.
 *
 * All calls dispatch to native-lib.cpp where the actual audio engine lives.
 * Parameter updates are lock-free (atomic stores in C++) and safe to call
 * from the main/UI thread while audio is running.
 *
 * dlms losss — mas ari
 */
object DlmsNativeInterface {

    init {
        System.loadLibrary("dlmslosss")
    }

    // ── Lifecycle ────────────────────────────────────────────────────────────
    external fun nativeInit()
    external fun nativeStart(): Boolean
    external fun nativeStop()
    external fun nativeIsRunning(): Boolean

    // ── Crossover ────────────────────────────────────────────────────────────
    /** Set Linkwitz-Riley 3-way crossover frequencies (Hz). */
    external fun nativeSetCrossover(lowMidHz: Float, midHighHz: Float)

    // ── Channel strip ────────────────────────────────────────────────────────
    /**
     * @param channel  0=LowL, 1=LowR, 2=MidL, 3=MidR, 4=HighL, 5=HighR
     * @param gainDb   Channel gain in dB (range −40 … +20)
     */
    external fun nativeSetChannelGain(channel: Int, gainDb: Float)
    external fun nativeSetChannelMute(channel: Int, mute: Boolean)
    external fun nativeSetChannelPhase(channel: Int, invert: Boolean)
    /** @param delayMs  0 … 20 ms alignment delay */
    external fun nativeSetChannelDelay(channel: Int, delayMs: Float)

    // ── Parametric EQ ────────────────────────────────────────────────────────
    /**
     * @param channel  Output channel index (0-5)
     * @param band     PEQ band index (0-7)
     * @param type     0=Peaking, 1=LowShelf, 2=HighShelf
     */
    external fun nativeSetPEQBand(
        channel: Int, band: Int,
        freqHz: Float, q: Float, gainDb: Float,
        type: Int, enabled: Boolean
    )

    // ── Limiter ──────────────────────────────────────────────────────────────
    external fun nativeSetLimiter(
        channel: Int,
        threshDb: Float,
        attackMs: Float,
        releaseMs: Float
    )

    // ── .dwp preset import ───────────────────────────────────────────────────
    /**
     * Parse and apply a dbx .dwp preset file byte array.
     * @return Preset name on success, or a string starting with "ERROR:" on failure.
     */
    external fun nativeLoadDwpPreset(data: ByteArray): String
}
