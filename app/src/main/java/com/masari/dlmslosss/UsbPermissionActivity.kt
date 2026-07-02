package com.masari.dlmslosss

import android.app.Activity
import android.os.Bundle

/**
 * UsbPermissionActivity — placeholder activity for USB device attachment intent-filter.
 *
 * When a USB audio interface is plugged in via OTG, Android routes the
 * USB_DEVICE_ATTACHED intent here. The actual USB stream is opened by Oboe
 * inside AudioEngine. This activity just dismisses itself immediately; Oboe
 * handles device enumeration internally.
 *
 * dlms losss — mas ari
 */
class UsbPermissionActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Oboe handles USB audio routing automatically via AudioManager.
        // No action needed here — finish transparently.
        finish()
    }
}
