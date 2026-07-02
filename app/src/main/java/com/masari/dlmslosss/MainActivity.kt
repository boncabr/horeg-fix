package com.masari.dlmslosss

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Bundle
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import com.masari.dlmslosss.databinding.ActivityMainBinding

/**
 * MainActivity — entry point for dlms losss.
 *
 * Hosts:
 *   - App branding header (title + owner credit)
 *   - Engine start/stop toggle
 *   - 6 channel strips (Low L/R, Mid L/R, High L/R)
 *   - Crossover frequency controls
 *   - Tuning menu with .dwp import button
 *
 * dlms losss — mas ari
 */
class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private val viewModel: MainViewModel by viewModels()

    // ── File picker launcher (Storage Access Framework) ───────────────────────
    private val dwpPickerLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode == Activity.RESULT_OK) {
            result.data?.data?.let { uri ->
                // Persist permission so app can read the file later
                contentResolver.takePersistableUriPermission(
                    uri, Intent.FLAG_GRANT_READ_URI_PERMISSION
                )
                viewModel.loadDwpPreset(uri)
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        setupObservers()
        setupControls()
        requestAudioPermission()
    }

    // ── LiveData observers ────────────────────────────────────────────────────
    private fun setupObservers() {
        viewModel.isRunning.observe(this) { running ->
            binding.btnToggleEngine.text = if (running) "■ Stop Engine" else "▶ Start Engine"
        }

        viewModel.statusMsg.observe(this) { msg ->
            binding.tvStatus.text = msg
        }

        viewModel.presetName.observe(this) { name ->
            name?.let { binding.tvPresetName.text = "Preset: $it" }
        }

        viewModel.channels.observe(this) { channels ->
            // Update each channel strip
            val strips = listOf(
                binding.channelLowL,  binding.channelLowR,
                binding.channelMidL,  binding.channelMidR,
                binding.channelHighL, binding.channelHighR
            )
            strips.forEachIndexed { idx, strip ->
                val state = channels[idx]
                // Avoid infinite loops: only update if value differs
                if (strip.sliderGain.value != state.gainDb)
                    strip.sliderGain.value = state.gainDb
                if (strip.switchMute.isChecked != state.mute)
                    strip.switchMute.isChecked = state.mute
                if (strip.switchPhase.isChecked != state.phaseInvert)
                    strip.switchPhase.isChecked = state.phaseInvert
                if (strip.sliderDelay.value != state.delayMs)
                    strip.sliderDelay.value = state.delayMs
            }
        }
    }

    // ── Control wiring ────────────────────────────────────────────────────────
    private fun setupControls() {
        // Engine toggle
        binding.btnToggleEngine.setOnClickListener {
            if (viewModel.isRunning.value == true)
                viewModel.stopEngine()
            else
                viewModel.startEngine()
        }

        // Channel strips
        val strips = listOf(
            binding.channelLowL,  binding.channelLowR,
            binding.channelMidL,  binding.channelMidR,
            binding.channelHighL, binding.channelHighR
        )
        strips.forEachIndexed { idx, strip ->
            strip.sliderGain.addOnChangeListener  { _, v, _ -> viewModel.setChannelGain(idx, v) }
            strip.sliderDelay.addOnChangeListener { _, v, _ -> viewModel.setChannelDelay(idx, v) }
            strip.switchMute.setOnCheckedChangeListener  { _, c -> viewModel.setChannelMute(idx, c) }
            strip.switchPhase.setOnCheckedChangeListener { _, c -> viewModel.setChannelPhase(idx, c) }
        }

        // Crossover sliders
        binding.sliderLowMid.addOnChangeListener { _, v, _ ->
            viewModel.setCrossover(v, binding.sliderMidHigh.value)
        }
        binding.sliderMidHigh.addOnChangeListener { _, v, _ ->
            viewModel.setCrossover(binding.sliderLowMid.value, v)
        }

        // .dwp import button
        binding.btnImportDwp.setOnClickListener {
            openDwpFilePicker()
        }
    }

    // ── SAF file picker ───────────────────────────────────────────────────────
    private fun openDwpFilePicker() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"  // SAF doesn't have a MIME type for .dwp
            putExtra(Intent.EXTRA_MIME_TYPES, arrayOf("application/octet-stream", "*/*"))
        }
        dwpPickerLauncher.launch(intent)
    }

    // ── Permissions ───────────────────────────────────────────────────────────
    private fun requestAudioPermission() {
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO)
            != PackageManager.PERMISSION_GRANTED
        ) {
            ActivityCompat.requestPermissions(
                this,
                arrayOf(Manifest.permission.RECORD_AUDIO),
                RC_AUDIO_PERMISSION
            )
        }
    }

    override fun onRequestPermissionsResult(
        requestCode: Int, permissions: Array<String>, grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == RC_AUDIO_PERMISSION) {
            if (grantResults.firstOrNull() != PackageManager.PERMISSION_GRANTED) {
                Toast.makeText(this,
                    "RECORD_AUDIO permission required for audio input",
                    Toast.LENGTH_LONG).show()
            }
        }
    }

    companion object {
        private const val RC_AUDIO_PERMISSION = 1001
    }
}
