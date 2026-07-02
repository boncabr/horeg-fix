#include "DwpParser.h"
#include <cstring>
#include <algorithm>
#include <android/log.h>

#define LOG_TAG "DwpParser"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)

/**
 * DwpParser — Real binary format of dbx DLMS .dwp preset files.
 *
 * Reverse-engineered from 115 genuine .dwp files (260 AE tuning set).
 *
 * ── File Layout ──────────────────────────────────────────────────────────────
 * Byte 0       : Preset name length (uint8, Pascal string)
 * Bytes 1..N   : Preset name (ASCII, no null terminator)
 * Bytes N+1..  : Optional 0x00 padding, then channel name Pascal strings
 *                (first byte = length, may be 0 for "no name")
 * Rest of file : Tagged binary sections separated by TLV markers (0x03 0x80)
 *                Section names are Pascal strings embedded after 0x01 0x80 or
 *                0x03 0x80 type bytes. Recognized section names:
 *                  "ST POST PEQ"  — per-channel parametric EQ (6 instances)
 *                  "PRE PEQ"      — pre-crossover EQ (ignored)
 *                  "MONO DLY 0 S" — per-channel alignment delay
 *                  "2X4 XOVER"    — crossover block
 *
 * ── Numeric Encoding ─────────────────────────────────────────────────────────
 * Parameters within sections are stored as TLV records:
 *   [0x03 0x80] [type_id: u8] [0x00] [len: u8] [param_id_str…] [value: u16 LE] [padding…]
 *
 * Known param_id_str tags found in files:
 *   "F1".."F8"  — EQ band frequencies (u16, Hz × 10 or index — see below)
 *   "G"         — EQ band gain        (i16, unit = 0.5 dB, centered at 0x18=24→0dB)
 *   "Q"         — EQ band Q factor    (u16, unit = 0.1, e.g. 0x19=25→Q2.5)
 *   "LPFc"      — LPF crossover freq  (u16, frequency index, see freqFromIndex())
 *   "HPFc"      — HPF crossover freq  (u16, same encoding)
 *   "Delay"     — Alignment delay     (u16, milliseconds × 100)
 *   "Slope"     — crossover slope     (ignored)
 *   "Gain:"     — channel gain        (i16, unit = 0.5 dB, centered at 0)
 *
 * ── Frequency Index Table ─────────────────────────────────────────────────────
 * dbx uses a 1/24-octave logarithmic index table starting from 20 Hz.
 * f(idx) = 20 × 2^(idx / 24)   (confirmed: idx=0xf1=241 → ~13 kHz crossover)
 *
 * dlms losss — mas ari
 */

// ── Frequency decode (1/24-octave index → Hz) ─────────────────────────────────
static float freqFromIndex(uint16_t idx) {
    // f = 20 × 2^(idx/24), clamped to audio range
    float hz = 20.0f * std::pow(2.0f, static_cast<float>(idx) / 24.0f);
    return std::clamp(hz, 20.0f, 20000.0f);
}

// ── Gain decode (u16 value, center=0x18 → 0dB, step = 0.5 dB) ────────────────
static float gainFromU16(uint16_t raw) {
    const int16_t signed_val = static_cast<int16_t>(raw);
    // Observed: 0x18 (24) → 0 dB; scale = 0.5 dB/step
    float dB = (static_cast<float>(signed_val) - 24.0f) * 0.5f;
    return std::clamp(dB, -24.0f, 24.0f);
}

// ── Q decode (u16 value, step = 0.1) ─────────────────────────────────────────
static float qFromU16(uint16_t raw) {
    float q = static_cast<float>(raw) * 0.1f;
    return std::clamp(q, 0.1f, 100.0f);
}

// ── Helper: find the next occurrence of a Pascal-string label in data ─────────
// Searches for a label string (e.g. "F1", "Delay") after startPos.
// Returns the position of the FIRST byte of the 2-byte value that follows the label.
// Returns DwpParser::kNotFound if not found before endPos.
static size_t findLabel(const uint8_t* data, size_t endPos,
                         size_t startPos, const char* label)
{
    const size_t labelLen = std::strlen(label);
    if (startPos + labelLen >= endPos) return SIZE_MAX;

    for (size_t i = startPos; i + labelLen < endPos; ++i) {
        if (std::memcmp(data + i, label, labelLen) == 0) {
            // Value is 2 bytes after the label string (skip label bytes)
            size_t valuePos = i + labelLen;
            if (valuePos + 2 <= endPos) return valuePos;
        }
    }
    return SIZE_MAX;
}

// ── Helper: find a named section and return its start offset ──────────────────
static size_t findSection(const uint8_t* data, size_t dataLen,
                           size_t startPos, const char* sectionName)
{
    const size_t len = std::strlen(sectionName);
    for (size_t i = startPos; i + len < dataLen; ++i) {
        if (std::memcmp(data + i, sectionName, len) == 0)
            return i;
    }
    return SIZE_MAX;
}

// ── Helper: read uint16 little-endian ─────────────────────────────────────────
static uint16_t u16le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

// ── Parse EQ bands from "ST POST PEQ" section ────────────────────────────────
static void parsePEQSection(const uint8_t* data, size_t secStart, size_t secEnd,
                              DwpChannelPreset& out)
{
    // EQ band labels: F1..F8 (frequency), G (gain), Q (q factor)
    // Each band has one F, one G, one Q in order
    static const char* freqLabels[] = {"F1","F2","F3","F4","F5","F6","F7","F8"};
    static const char* gainLabel  = "G";
    static const char* qLabel     = "Q";

    size_t searchPos = secStart;
    for (int band = 0; band < 8; ++band) {
        // Find Fn label
        size_t fPos = findLabel(data, secEnd, searchPos, freqLabels[band]);
        if (fPos == SIZE_MAX) break;
        out.peq[band].freqHz  = freqFromIndex(u16le(data + fPos));

        // G and Q should follow closely after the freq
        size_t gPos = findLabel(data, std::min(secEnd, fPos + 64), fPos, gainLabel);
        if (gPos != SIZE_MAX)
            out.peq[band].gainDb = gainFromU16(u16le(data + gPos));

        size_t qPos = findLabel(data, std::min(secEnd, fPos + 128), fPos, qLabel);
        if (qPos != SIZE_MAX)
            out.peq[band].q = qFromU16(u16le(data + qPos));

        out.peq[band].type    = 0; // Peaking (dbx DLMS default for mid bands)
        out.peq[band].enabled = true;
        searchPos = fPos + 2;
    }
}

// ── Parse delay from "MONO DLY 0 S" section ──────────────────────────────────
static float parseDelay(const uint8_t* data, size_t secStart, size_t secEnd) {
    size_t pos = findLabel(data, secEnd, secStart, "Delay");
    if (pos == SIZE_MAX) return 0.0f;
    uint16_t raw = u16le(data + pos);
    // Unit: 1/100 ms (i.e. raw=100 → 1ms). Clamp to 0–20ms.
    return std::clamp(static_cast<float>(raw) / 100.0f, 0.0f, 20.0f);
}

// ── Parse crossover from "2X4 XOVER" section ─────────────────────────────────
static void parseCrossover(const uint8_t* data, size_t secStart, size_t secEnd,
                             DlmsPreset& out)
{
    size_t lpfPos = findLabel(data, secEnd, secStart, "LPFc");
    size_t hpfPos = findLabel(data, secEnd, secStart, "HPFc");
    if (lpfPos != SIZE_MAX) out.crossoverLowMidHz  = freqFromIndex(u16le(data + lpfPos));
    if (hpfPos != SIZE_MAX) out.crossoverMidHighHz = freqFromIndex(u16le(data + hpfPos));
}

// ── Main parse entry point ─────────────────────────────────────────────────────
bool DwpParser::parse(const uint8_t* data, size_t length, DlmsPreset& out) {
    lastError_.clear();

    // Minimum sanity check
    if (length < 16) {
        lastError_ = "File too small to be a .dwp preset";
        return false;
    }

    // ── Preset name (Pascal string: 1-byte length + ASCII) ───────────────────
    const uint8_t nameLen = data[0];
    if (nameLen == 0 || nameLen > 63 || static_cast<size_t>(nameLen) + 1 > length) {
        lastError_ = "Invalid preset name length byte: " + std::to_string(nameLen);
        return false;
    }
    std::memcpy(out.presetName, data + 1, nameLen);
    out.presetName[nameLen] = '\0';
    LOGI("Parsing preset: \"%s\" (%zu bytes)", out.presetName, length);

    // ── Default crossover values ───────────────────────────────────────────────
    out.crossoverLowMidHz  = 200.0f;
    out.crossoverMidHighHz = 2000.0f;

    // ── Default channel values ─────────────────────────────────────────────────
    for (int ch = 0; ch < 6; ++ch) {
        out.channels[ch] = DwpChannelPreset{};
        for (int b = 0; b < 8; ++b) {
            out.channels[ch].peq[b].freqHz  = 1000.0f;
            out.channels[ch].peq[b].q       = 0.707f;
            out.channels[ch].peq[b].gainDb  = 0.0f;
            out.channels[ch].peq[b].type    = 0;
            out.channels[ch].peq[b].enabled = true;
        }
    }

    // ── Crossover ─────────────────────────────────────────────────────────────
    size_t xoStart = findSection(data, length, 0, "2X4 XOVER");
    if (xoStart != SIZE_MAX) {
        size_t xoEnd = std::min(length, xoStart + 512);
        parseCrossover(data, xoStart, xoEnd, out);
        LOGI("Crossover: lo/mid=%.0f Hz, mid/hi=%.0f Hz",
             out.crossoverLowMidHz, out.crossoverMidHighHz);
    } else {
        LOGW("2X4 XOVER section not found — using defaults");
    }

    // ── Per-channel EQ (ST POST PEQ × 6) ─────────────────────────────────────
    // Channel mapping in 221200Bi files:
    //   occurrence 0 → Left High  (ch 4)
    //   occurrence 1 → Right High (ch 5)
    //   occurrence 2 → Left Low   (ch 0)
    //   occurrence 3 → Right Low  (ch 1)
    //   occurrences 4,5 → Mid channels (ch 2, ch 3) — may be "No Name"
    // We map occurrences 0..5 → channels 4,5,0,1,2,3 to match dbx wiring order
    static const int kPeqChOrder[] = {4, 5, 0, 1, 2, 3};

    size_t peqSearch = 0;
    for (int occ = 0; occ < 6; ++occ) {
        size_t secStart = findSection(data, length, peqSearch, "ST POST PEQ");
        if (secStart == SIZE_MAX) break;
        // Section ends at the next ST POST PEQ occurrence or 1024 bytes later
        size_t secEnd = findSection(data, length, secStart + 11, "ST POST PEQ");
        if (secEnd == SIZE_MAX) secEnd = std::min(length, secStart + 1024);

        int ch = kPeqChOrder[occ];
        parsePEQSection(data, secStart + 11, secEnd, out.channels[ch]);
        peqSearch = secStart + 11;
    }

    // ── Per-channel delay (MONO DLY 0 S × 2, repeated for L/R pairs) ─────────
    // Two MONO DLY sections found: one for odd channels, one for even
    static const int kDlyChOrder[] = {0, 1, 2, 3, 4, 5};
    size_t dlySearch = 0;
    for (int occ = 0; occ < 6; ++occ) {
        size_t secStart = findSection(data, length, dlySearch, "MONO DLY");
        if (secStart == SIZE_MAX) break;
        size_t secEnd = std::min(length, secStart + 256);
        int ch = kDlyChOrder[occ];
        out.channels[ch].delayMs = parseDelay(data, secStart, secEnd);
        dlySearch = secStart + 8;
    }

    LOGI("Preset parse OK: xo=%.0f/%.0f Hz",
         out.crossoverLowMidHz, out.crossoverMidHighHz);
    return true;
}
