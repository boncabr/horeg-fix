#pragma once
#include <vector>
#include <cstdint>
#include <string>

/**
 * DwpParser — dbx DLMS .dwp Preset File Parser
 *
 * Parses the real dbx .dwp binary format (reverse-engineered from 115 genuine
 * preset files from the "260 AE tunings" set).
 *
 * File format: Pascal string header + TLV tagged sections with embedded
 * human-readable section names ("ST POST PEQ", "2X4 XOVER", "MONO DLY 0 S").
 *
 * dlms losss — mas ari
 */

// ── Preset data structures ────────────────────────────────────────────────────

struct DwpPEQBand {
    float   freqHz  {1000.0f};
    float   q       {0.707f};
    float   gainDb  {0.0f};
    uint8_t type    {0};       // 0=Peaking, 1=LowShelf, 2=HighShelf
    bool    enabled {true};
};

struct DwpChannelPreset {
    float     gainDb          {0.0f};
    float     delayMs         {0.0f};
    bool      mute            {false};
    bool      phaseInvert     {false};
    float     limiterThreshDb {-3.0f};
    float     limiterAttackMs {0.5f};
    float     limiterRelMs    {50.0f};
    DwpPEQBand peq[8];
};

struct DlmsPreset {
    char  presetName[64]         {};
    float crossoverLowMidHz      {200.0f};
    float crossoverMidHighHz     {2000.0f};
    DwpChannelPreset channels[6]; // LowL, LowR, MidL, MidR, HighL, HighR
};

// ── Parser ────────────────────────────────────────────────────────────────────

class DwpParser {
public:
    /**
     * Parse a .dwp file byte buffer.
     * @param data    Raw bytes (from Android SAF / JNI byte array)
     * @param length  Number of bytes
     * @param out     Populated on success
     * @return true on success
     */
    bool parse(const uint8_t* data, size_t length, DlmsPreset& out);

    const std::string& lastError() const { return lastError_; }

    static constexpr size_t kNotFound = SIZE_MAX;

private:
    std::string lastError_;
};
