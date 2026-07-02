package com.masari.dlmslosss

import android.app.Application
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.LiveData
import androidx.lifecycle.MutableLiveData
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * MainViewModel — owns the DSP engine lifetime and exposes UI state via LiveData.
 *
 * dlms losss — mas ari
 */
class MainViewModel(app: Application) : AndroidViewModel(app) {

    // ── LiveData ─────────────────────────────────────────────────────────────
    private val _isRunning   = MutableLiveData(false)
    val isRunning: LiveData<Boolean> = _isRunning

    private val _statusMsg   = MutableLiveData("Ready")
    val statusMsg: LiveData<String> = _statusMsg

    private val _presetName  = MutableLiveData<String?>(null)
    val presetName: LiveData<String?> = _presetName

    // Channel UI state (6 channels)
    data class ChannelState(
        val gainDb: Float      = 0f,
        val mute: Boolean      = false,
        val phaseInvert: Boolean = false,
        val delayMs: Float     = 0f
    )

    private val _channels = MutableLiveData(Array(6) { ChannelState() })
    val channels: LiveData<Array<ChannelState>> = _channels

    // Crossover state
    private val _crossoverLowMid  = MutableLiveData(200f)
    private val _crossoverMidHigh = MutableLiveData(2000f)
    val crossoverLowMid:  LiveData<Float> = _crossoverLowMid
    val crossoverMidHigh: LiveData<Float> = _crossoverMidHigh

    // ── Init ─────────────────────────────────────────────────────────────────
    init {
        DlmsNativeInterface.nativeInit()
    }

    // ── Engine control ────────────────────────────────────────────────────────
    fun startEngine() {
        viewModelScope.launch(Dispatchers.IO) {
            val ok = DlmsNativeInterface.nativeStart()
            withContext(Dispatchers.Main) {
                _isRunning.value = ok
                _statusMsg.value = if (ok) "▶ Engine running" else "⚠ Failed to start"
            }
        }
    }

    fun stopEngine() {
        DlmsNativeInterface.nativeStop()
        _isRunning.value = false
        _statusMsg.value = "■ Engine stopped"
    }

    // ── Channel strip ─────────────────────────────────────────────────────────
    fun setChannelGain(ch: Int, gainDb: Float) {
        DlmsNativeInterface.nativeSetChannelGain(ch, gainDb)
        updateChannel(ch) { it.copy(gainDb = gainDb) }
    }

    fun setChannelMute(ch: Int, mute: Boolean) {
        DlmsNativeInterface.nativeSetChannelMute(ch, mute)
        updateChannel(ch) { it.copy(mute = mute) }
    }

    fun setChannelPhase(ch: Int, invert: Boolean) {
        DlmsNativeInterface.nativeSetChannelPhase(ch, invert)
        updateChannel(ch) { it.copy(phaseInvert = invert) }
    }

    fun setChannelDelay(ch: Int, delayMs: Float) {
        DlmsNativeInterface.nativeSetChannelDelay(ch, delayMs)
        updateChannel(ch) { it.copy(delayMs = delayMs) }
    }

    private fun updateChannel(ch: Int, transform: (ChannelState) -> ChannelState) {
        val arr = _channels.value!!.copyOf()
        arr[ch] = transform(arr[ch])
        _channels.value = arr
    }

    // ── Crossover ─────────────────────────────────────────────────────────────
    fun setCrossover(lowMidHz: Float, midHighHz: Float) {
        DlmsNativeInterface.nativeSetCrossover(lowMidHz, midHighHz)
        _crossoverLowMid.value  = lowMidHz
        _crossoverMidHigh.value = midHighHz
    }

    // ── .dwp preset import ────────────────────────────────────────────────────
    fun loadDwpPreset(uri: Uri) {
        viewModelScope.launch(Dispatchers.IO) {
            try {
                val bytes = getApplication<Application>().contentResolver
                    .openInputStream(uri)?.readBytes()
                    ?: throw IllegalArgumentException("Cannot read file")

                val result = DlmsNativeInterface.nativeLoadDwpPreset(bytes)
                withContext(Dispatchers.Main) {
                    if (result.startsWith("ERROR:")) {
                        _statusMsg.value = result
                    } else {
                        _presetName.value = result
                        _statusMsg.value  = "Preset loaded: $result"
                    }
                }
            } catch (e: Exception) {
                withContext(Dispatchers.Main) {
                    _statusMsg.value = "Import failed: ${e.message}"
                }
            }
        }
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    override fun onCleared() {
        DlmsNativeInterface.nativeStop()
        super.onCleared()
    }
}
