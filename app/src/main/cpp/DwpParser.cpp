#include "DwpParser.h"
#include <cstring>
#include <android/log.h>

#define LOG_TAG "DwpParser"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)

/**
 * DwpParser — reverse-engineered dbx .dwp binary format reader.
 *
 * Known .dwp file layout (version 1, little-endian):
 *   Offset  Size  Field
 *   0x00    4     Magic: 'D','W','P',0x00
 *   0x04    2     Format version (uint16)
 *   0x06    2     Number of channels (uint16)
 *   0x08    64    Preset name (null-padded ASCII)
 *   0x48    4     Crossover Low-Mid frequency (float32, Hz)
 *   0x4C    4     Crossover Mid-High frequency (float32, Hz)
 *   0x50    n     Channel data blocks (see below)
 *
 * Per-channel block (repeated × numChannels):
 *   0x00    4     Gain dB (float32)
 *   0x04    4     Delay ms (float32)
 *   0x08    1     Flags: bit0=mute, bit1=phaseInvert
 *   0x09    4     Limiter threshold dB (float32)
 *   0x0D    4     Limiter attack ms (float32)
 *   0x11    4     Limiter release ms (float32)
 *   0x15    8×(4+4+4+1+1)  PEQ bands: freq, q, gain, type, enabled
 *
 * Total per-channel block size: 3 + 12 + 8*14 = 127 bytes
 * (Plus 2 padding bytes to align to 4 bytes → 128 bytes)
 *
 * dlms losss — mas ari
 */

static constexpr size_t kHeaderSize    = 0x50;
static constexpr size_t kChannelBlock  = 128; // bytes per channel
static constexpr size_t kPEQBandBlock  = 14;  // bytes per PEQ band
static constexpr int    kMaxChannels   = 6;
static constexpr int    kMaxPEQBands   = 8;

bool DwpParser::parse(const uint8_t* data, size_t length, DlmsPreset& out) {
    lastError_.clear();

    // ── Minimum size check ────────────────────────────────────────────────
    const size_t minSize = kHeaderSize + kMaxChannels * kChannelBlock;
    if (length < minSize) {
        lastError_ = "File too small to be a valid .dwp preset (got " +
                     std::to_string(length) + " bytes, need " + std::to_string(minSize) + ")";
        LOGW("%s", lastError_.c_str());
        return false;
    }

    // ── Magic number ─────────────────────────────────────────────────────
    if (data[0] != 'D' || data[1] != 'W' || data[2] != 'P' || data[3] != 0x00) {
        lastError_ = "Invalid .dwp magic bytes — this may not be a dbx DLMS preset file.";
        LOGW("%s", lastError_.c_str());
        return false;
    }

    // ── Format version ────────────────────────────────────────────────────
    const uint16_t version = readU16(data + 0x04);
    if (version > 2) {
        LOGW("Unknown .dwp format version %u — attempting parse anyway", version);
    }

    // ── Channel count ─────────────────────────────────────────────────────
    const uint16_t numChannels = readU16(data + 0x06);
    if (numChannels > kMaxChannels) {
        lastError_ = "Unexpected channel count: " + std::to_string(numChannels);
        return false;
    }

    // ── Preset name (64 bytes, null-terminated ASCII) ─────────────────────
    std::memcpy(out.presetName, data + 0x08, 63);
    out.presetName[63] = '\0';
    LOGI("Parsing preset: \"%s\" (v%u, %u ch)", out.presetName, version, numChannels);

    // ── Crossover frequencies ─────────────────────────────────────────────
    out.crossoverLowMidHz  = readF32(data + 0x48);
    out.crossoverMidHighHz = readF32(data + 0x4C);

    // Sanity-check crossover values
    if (out.crossoverLowMidHz  < 20.0f  || out.crossoverLowMidHz  > 20000.0f) out.crossoverLowMidHz  = 200.0f;
    if (out.crossoverMidHighHz < 200.0f || out.crossoverMidHighHz > 20000.0f) out.crossoverMidHighHz = 2000.0f;

    // ── Per-channel data ──────────────────────────────────────────────────
    for (int ch = 0; ch < static_cast<int>(numChannels); ++ch) {
        const uint8_t* cb = data + kHeaderSize + ch * kChannelBlock;
        DwpChannelPreset& cp = out.channels[ch];

        cp.gainDb       = readF32(cb + 0x00);
        cp.delayMs      = readF32(cb + 0x04);
        const uint8_t flags = cb[0x08];
        cp.mute         = (flags & 0x01) != 0;
        cp.phaseInvert  = (flags & 0x02) != 0;
        cp.limiterThreshDb = readF32(cb + 0x09);
        cp.limiterAttackMs = readF32(cb + 0x0D);
        cp.limiterRelMs    = readF32(cb + 0x11);

        // Sanity checks
        cp.gainDb  = std::clamp(cp.gainDb,  -40.0f,  20.0f);
        cp.delayMs = std::clamp(cp.delayMs,   0.0f,  20.0f);

        // PEQ bands
        for (int b = 0; b < kMaxPEQBands; ++b) {
            const uint8_t* pb = cb + 0x15 + b * kPEQBandBlock;
            DwpPEQBand& band = cp.peq[b];
            band.freqHz  = readF32(pb + 0);
            band.q       = readF32(pb + 4);
            band.gainDb  = readF32(pb + 8);
            band.type    = pb[12];
            band.enabled = pb[13] != 0;

            // Sanity checks
            band.freqHz = std::clamp(band.freqHz, 20.0f, 20000.0f);
            band.q      = std::clamp(band.q,     0.1f,  100.0f);
            band.gainDb = std::clamp(band.gainDb,-24.0f, 24.0f);
            if (band.type > 2) band.type = 0;
        }
    }

    LOGI("Preset parsed OK: xo=%.0f/%.0f Hz",
         out.crossoverLowMidHz, out.crossoverMidHighHz);
    return true;
}
