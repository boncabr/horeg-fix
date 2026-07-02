#include <jni.h>
#include <string>
#include <memory>
#include <android/log.h>
#include "AudioEngine.h"
#include "DwpParser.h"

/**
 * native-lib.cpp — JNI bridge (Kotlin ↔ C++ DSP engine).
 *
 * All channel/band index parameters are bounds-checked before touching native
 * memory. Out-of-range calls are logged and silently ignored — they never
 * cause OOB writes.
 *
 * dlms losss — mas ari
 */

#define LOG_TAG "NativeLib"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)

static std::unique_ptr<AudioEngine> gAudioEngine;

static AudioEngine* getEngine() {
    if (!gAudioEngine) { LOGE("getEngine(): engine not initialised"); return nullptr; }
    return gAudioEngine.get();
}

// Validate channel index (0-5) — returns false and logs if out of range
static bool checkChannel(int ch) {
    if (ch < 0 || ch >= kNumOutputChannels) {
        LOGE("Invalid channel index %d (valid: 0-%d)", ch, kNumOutputChannels - 1);
        return false;
    }
    return true;
}

// Validate PEQ band index (0-7)
static bool checkBand(int band) {
    if (band < 0 || band >= kNumPEQBands) {
        LOGE("Invalid PEQ band index %d (valid: 0-%d)", band, kNumPEQBands - 1);
        return false;
    }
    return true;
}

extern "C" {

// ── Lifecycle ─────────────────────────────────────────────────────────────────

JNIEXPORT void JNICALL
Java_com_masari_dlmslosss_DlmsNativeInterface_nativeInit(JNIEnv*, jobject) {
    if (!gAudioEngine) {
        gAudioEngine = std::make_unique<AudioEngine>();
        LOGI("AudioEngine created");
    }
}

JNIEXPORT jboolean JNICALL
Java_com_masari_dlmslosss_DlmsNativeInterface_nativeStart(JNIEnv*, jobject) {
    AudioEngine* e = getEngine();
    return e && e->start() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_masari_dlmslosss_DlmsNativeInterface_nativeStop(JNIEnv*, jobject) {
    if (gAudioEngine) gAudioEngine->stop();
}

JNIEXPORT jboolean JNICALL
Java_com_masari_dlmslosss_DlmsNativeInterface_nativeIsRunning(JNIEnv*, jobject) {
    AudioEngine* e = getEngine();
    return (e && e->isRunning()) ? JNI_TRUE : JNI_FALSE;
}

// ── Crossover ─────────────────────────────────────────────────────────────────

JNIEXPORT void JNICALL
Java_com_masari_dlmslosss_DlmsNativeInterface_nativeSetCrossover(
        JNIEnv*, jobject, jfloat lowMidHz, jfloat midHighHz)
{
    AudioEngine* e = getEngine(); if (!e) return;
    // Clamp to valid audio range
    lowMidHz  = std::clamp(lowMidHz,  20.f, 2000.f);
    midHighHz = std::clamp(midHighHz, 200.f, 20000.f);
    e->dspEngine().crossoverParams().lowMidHz .store(lowMidHz,  std::memory_order_relaxed);
    e->dspEngine().crossoverParams().midHighHz.store(midHighHz, std::memory_order_relaxed);
}

// ── Channel strip ─────────────────────────────────────────────────────────────

JNIEXPORT void JNICALL
Java_com_masari_dlmslosss_DlmsNativeInterface_nativeSetChannelGain(
        JNIEnv*, jobject, jint channel, jfloat gainDb)
{
    AudioEngine* e = getEngine(); if (!e) return;
    if (!checkChannel(channel)) return;
    gainDb = std::clamp(gainDb, -40.f, 20.f);
    e->dspEngine().channelParams(channel).gainDb.store(gainDb, std::memory_order_relaxed);
}

JNIEXPORT void JNICALL
Java_com_masari_dlmslosss_DlmsNativeInterface_nativeSetChannelMute(
        JNIEnv*, jobject, jint channel, jboolean mute)
{
    AudioEngine* e = getEngine(); if (!e) return;
    if (!checkChannel(channel)) return;
    e->dspEngine().channelParams(channel).mute.store(mute, std::memory_order_relaxed);
}

JNIEXPORT void JNICALL
Java_com_masari_dlmslosss_DlmsNativeInterface_nativeSetChannelPhase(
        JNIEnv*, jobject, jint channel, jboolean invert)
{
    AudioEngine* e = getEngine(); if (!e) return;
    if (!checkChannel(channel)) return;
    e->dspEngine().channelParams(channel).phaseInvert.store(invert, std::memory_order_relaxed);
}

JNIEXPORT void JNICALL
Java_com_masari_dlmslosss_DlmsNativeInterface_nativeSetChannelDelay(
        JNIEnv*, jobject, jint channel, jfloat delayMs)
{
    AudioEngine* e = getEngine(); if (!e) return;
    if (!checkChannel(channel)) return;
    delayMs = std::clamp(delayMs, 0.f, 20.f);
    e->dspEngine().channelParams(channel).delayMs.store(delayMs, std::memory_order_relaxed);
}

// ── PEQ ───────────────────────────────────────────────────────────────────────

JNIEXPORT void JNICALL
Java_com_masari_dlmslosss_DlmsNativeInterface_nativeSetPEQBand(
        JNIEnv*, jobject,
        jint channel, jint band,
        jfloat freqHz, jfloat q, jfloat gainDb, jint type, jboolean enabled)
{
    AudioEngine* e = getEngine(); if (!e) return;
    if (!checkChannel(channel) || !checkBand(band)) return;

    PEQBandParams p;
    p.freqHz  = std::clamp(freqHz,  20.f,  20000.f);
    p.q       = std::clamp(q,       0.1f,  100.f);
    p.gainDb  = std::clamp(gainDb, -24.f,  24.f);
    p.enabled = enabled;
    switch (type) {
        case 1:  p.type = FilterType::LOW_SHELF;  break;
        case 2:  p.type = FilterType::HIGH_SHELF; break;
        default: p.type = FilterType::PEAKING;    break;
    }
    e->dspEngine().setPEQBand(channel, band, p);
}

// ── Limiter ───────────────────────────────────────────────────────────────────

JNIEXPORT void JNICALL
Java_com_masari_dlmslosss_DlmsNativeInterface_nativeSetLimiter(
        JNIEnv*, jobject,
        jint channel, jfloat threshDb, jfloat attackMs, jfloat releaseMs)
{
    AudioEngine* e = getEngine(); if (!e) return;
    if (!checkChannel(channel)) return;
    // Enforce sane positive minimums so exp() in limiter never gets 0/negative
    threshDb  = std::clamp(threshDb,  -40.f,  0.f);
    attackMs  = std::max(attackMs,    0.01f);
    releaseMs = std::max(releaseMs,   1.0f);
    e->dspEngine().limiterParams(channel).thresholdDb.store(threshDb,  std::memory_order_relaxed);
    e->dspEngine().limiterParams(channel).attackMs   .store(attackMs,  std::memory_order_relaxed);
    e->dspEngine().limiterParams(channel).releaseMs  .store(releaseMs, std::memory_order_relaxed);
}

// ── .dwp preset import ────────────────────────────────────────────────────────

JNIEXPORT jstring JNICALL
Java_com_masari_dlmslosss_DlmsNativeInterface_nativeLoadDwpPreset(
        JNIEnv* env, jobject, jbyteArray data)
{
    AudioEngine* e = getEngine();
    if (!e) return env->NewStringUTF("ERROR: Engine not initialised");

    const jsize len = env->GetArrayLength(data);
    if (len <= 0) return env->NewStringUTF("ERROR: Empty file");

    jbyte* bytes = env->GetByteArrayElements(data, nullptr);
    if (!bytes)   return env->NewStringUTF("ERROR: Could not read byte array");

    DwpParser parser;
    DlmsPreset preset;
    bool ok = parser.parse(reinterpret_cast<const uint8_t*>(bytes),
                           static_cast<size_t>(len), preset);
    env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);

    if (!ok) return env->NewStringUTF(("ERROR: " + parser.lastError()).c_str());

    e->dspEngine().applyPreset(preset);
    return env->NewStringUTF(preset.presetName);
}

} // extern "C"
