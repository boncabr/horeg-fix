#pragma once
#include <vector>
#include <cstdint>
#include <string>
#include "BiquadFilter.h"
#include "DlmsEngine.h"

/**
 * DwpParser — dbx DLMS .dwp Preset File Parser
 *
 * The .dwp binary format is a proprietary dbx binary container. This parser
 * implements a best-effort reverse-engineered layout based on field analysis of
 * publicly captured .dwp files. Unknown fields are skipped with a warning log.
 *
 * Parsed data is returned as a DlmsPreset struct that the DlmsEngine can apply
 * directly via DlmsEngine::applyPreset().
 *
 * dlms losss — mas ari
 */

// ─── Preset data structures ──────────────────────────────────────────────────

struct DwpPEQBand {
    float freqHz  {1000.0f};
    float q       {0.707f};
    float gainDb  {0.0f};
    uint8_t type  {0};       // 0=Peaking, 1=LowShelf, 2=HighShelf
    bool enabled  {true};
};

struct DwpChannelPreset {
    float gainDb          {0.0f};
    float delayMs         {0.0f};
    bool  mute            {false};
    bool  phaseInvert     {false};
    float limiterThreshDb {-3.0f};
    float limiterAttackMs {0.5f};
    float limiterRelMs    {50.0f};
    DwpPEQBand peq[8];
};

struct DlmsPreset {
    char  presetName[64]         {};
    float crossoverLowMidHz      {200.0f};
    float crossoverMidHighHz     {2000.0f};
    DwpChannelPreset channels[6]; // Low L, Low R, Mid L, Mid R, High L, High R
};

// ─── Parser ──────────────────────────────────────────────────────────────────

class DwpParser {
public:
    /**
     * Parse a .dwp file byte buffer.
     *
     * @param data    Raw bytes of the .dwp file (passed via JNI from Android SAF)
     * @param length  Number of bytes in the buffer
     * @param out     Output preset struct (populated on success)
     * @return true on success, false with error details in lastError()
     */
    bool parse(const uint8_t* data, size_t length, DlmsPreset& out);

    /** Human-readable description of the last parse error. */
    const std::string& lastError() const { return lastError_; }

private:
    std::string lastError_;

    // ── Known .dwp magic bytes ───────────────────────────────────────────
    static constexpr uint32_t kMagicDWP = 0x44575000; // 'DWP\0'

    // ── Little-endian read helpers ───────────────────────────────────────
    static uint16_t readU16(const uint8_t* p) {
        return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
    }
    static uint32_t readU32(const uint8_t* p) {
        return static_cast<uint32_t>(p[0])        |
               (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16)|
               (static_cast<uint32_t>(p[3]) << 24);
    }
    static float readF32(const uint8_t* p) {
        uint32_t bits = readU32(p);
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    }
};
