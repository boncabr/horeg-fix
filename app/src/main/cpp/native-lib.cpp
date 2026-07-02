#include <jni.h>
#include <string>
#include <memory>
#include <android/log.h>
#include "AudioEngine.h"
#include "DwpParser.h"

/**
 * native-lib.cpp — JNI bridge between Kotlin UI and the C++ DSP engine.
 *
 * All JNI functions run on a Binder/UI thread.  Parameter writes to
 * the DSP engine use std::atomic stores (relaxed), ensuring the real-time
 * audio callback thread sees consistent values without any mutex lock.
 *
 * JNI naming convention:
 *   Java_<package_underscored>_<class>_<method>
 *   package: com.masari.dlmslosss → com_masari_dlmslosss
 *
 * dlms losss — mas ari
 */

#define LOG_TAG "NativeLib"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ── Singleton audio engine (lives for the app lifetime) ──────────────────────
static std::unique_ptr<AudioEngine> gAudioEngine;

static AudioEngine* getEngine() {
    if (!gAudioEngine) {
        LOGE("getEngine() called before init — returning nullptr");
        return nullptr;
    }
    return gAudioEngine.get();
}

extern "C" {

// ────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ────────────────────────────────────────────────────────────────────────────

JNIEXPORT void JNICALL
Java_com_masari_dlmslosss_DlmsNativeInterface_nativeInit(JNIEnv*, jobject) {
    if (!gAudioEngine) {
        gAudioEngine = std::make_unique<AudioEngine>();
        LOGI("DlmsEngine created");
    }
}

JNIEXPORT jboolean JNICALL
Java_com_masari_dlmslosss_DlmsNativeInterface_nativeStart(JNIEnv*, jobject) {
    AudioEngine* eng = getEngine();
    if (!eng) return JNI_FALSE;
    return eng->start() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_masari_dlmslosss_DlmsNativeInterface_nativeStop(JNIEnv*, jobject) {
    if (gAudioEngine) gAudioEngine->stop();
}

JNIEXPORT jboolean JNICALL
Java_com_masari_dlmslosss_DlmsNativeInterface_nativeIsRunning(JNIEnv*, jobject) {
    AudioEngine* eng = getEngine();
    return (eng && eng->isRunning()) ? JNI_TRUE : JNI_FALSE;
}

// ────────────────────────────────────────────────────────────────────────────
// Crossover
// ────────────────────────────────────────────────────────────────────────────

JNIEXPORT void JNICALL
Java_com_masari_dlmslosss_DlmsNativeInterface_nativeSetCrossover(
        JNIEnv*, jobject, jfloat lowMidHz, jfloat midHighHz)
{
    AudioEngine* eng = getEngine(); if (!eng) return;
    eng->dspEngine().crossoverParams().lowMidHz .store(lowMidHz,  std::memory_order_relaxed);
    eng->dspEngine().crossoverParams().midHighHz.store(midHighHz, std::memory_order_relaxed);
}

// ────────────────────────────────────────────────────────────────────────────
// Channel strip: gain, mute, phase, delay
// ────────────────────────────────────────────────────────────────────────────

JNIEXPORT void JNICALL
Java_com_masari_dlmslosss_DlmsNativeInterface_nativeSetChannelGain(
        JNIEnv*, jobject, jint channel, jfloat gainDb)
{
    AudioEngine* eng = getEngine(); if (!eng) return;
    eng->dspEngine().channelParams(channel).gainDb.store(gainDb, std::memory_order_relaxed);
}

JNIEXPORT void JNICALL
Java_com_masari_dlmslosss_DlmsNativeInterface_nativeSetChannelMute(
        JNIEnv*, jobject, jint channel, jboolean mute)
{
    AudioEngine* eng = getEngine(); if (!eng) return;
    eng->dspEngine().channelParams(channel).mute.store(mute, std::memory_order_relaxed);
}

JNIEXPORT void JNICALL
Java_com_masari_dlmslosss_DlmsNativeInterface_nativeSetChannelPhase(
        JNIEnv*, jobject, jint channel, jboolean invert)
{
    AudioEngine* eng = getEngine(); if (!eng) return;
    eng->dspEngine().channelParams(channel).phaseInvert.store(invert, std::memory_order_relaxed);
}

JNIEXPORT void JNICALL
Java_com_masari_dlmslosss_DlmsNativeInterface_nativeSetChannelDelay(
        JNIEnv*, jobject, jint channel, jfloat delayMs)
{
    AudioEngine* eng = getEngine(); if (!eng) return;
    eng->dspEngine().channelParams(channel).delayMs.store(delayMs, std::memory_order_relaxed);
}

// ────────────────────────────────────────────────────────────────────────────
// PEQ
// ────────────────────────────────────────────────────────────────────────────

JNIEXPORT void JNICALL
Java_com_masari_dlmslosss_DlmsNativeInterface_nativeSetPEQBand(
        JNIEnv*, jobject,
        jint channel, jint band,
        jfloat freqHz, jfloat q, jfloat gainDb, jint type, jboolean enabled)
{
    AudioEngine* eng = getEngine(); if (!eng) return;
    PEQBandParams p;
    p.freqHz  = freqHz;
    p.q       = q;
    p.gainDb  = gainDb;
    p.enabled = enabled;
    switch (type) {
        case 1:  p.type = FilterType::LOW_SHELF;  break;
        case 2:  p.type = FilterType::HIGH_SHELF; break;
        default: p.type = FilterType::PEAKING;    break;
    }
    eng->dspEngine().setPEQBand(channel, band, p);
}

// ────────────────────────────────────────────────────────────────────────────
// Limiter
// ────────────────────────────────────────────────────────────────────────────

JNIEXPORT void JNICALL
Java_com_masari_dlmslosss_DlmsNativeInterface_nativeSetLimiter(
        JNIEnv*, jobject,
        jint channel, jfloat threshDb, jfloat attackMs, jfloat releaseMs)
{
    AudioEngine* eng = getEngine(); if (!eng) return;
    eng->dspEngine().limiterParams(channel).thresholdDb.store(threshDb,  std::memory_order_relaxed);
    eng->dspEngine().limiterParams(channel).attackMs   .store(attackMs,  std::memory_order_relaxed);
    eng->dspEngine().limiterParams(channel).releaseMs  .store(releaseMs, std::memory_order_relaxed);
}

// ────────────────────────────────────────────────────────────────────────────
// .dwp preset import (byte array from Android SAF)
// ────────────────────────────────────────────────────────────────────────────

JNIEXPORT jstring JNICALL
Java_com_masari_dlmslosss_DlmsNativeInterface_nativeLoadDwpPreset(
        JNIEnv* env, jobject, jbyteArray data)
{
    AudioEngine* eng = getEngine();
    if (!eng) return env->NewStringUTF("ERROR: Engine not initialised");

    const jsize length = env->GetArrayLength(data);
    jbyte* bytes = env->GetByteArrayElements(data, nullptr);

    DwpParser parser;
    DlmsPreset preset;
    bool ok = parser.parse(reinterpret_cast<const uint8_t*>(bytes), length, preset);
    env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);

    if (!ok) {
        return env->NewStringUTF(("ERROR: " + parser.lastError()).c_str());
    }

    eng->dspEngine().applyPreset(preset);
    return env->NewStringUTF(preset.presetName);
}

} // extern "C"
