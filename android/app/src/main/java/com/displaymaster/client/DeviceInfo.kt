package com.displaymaster.client

import android.app.Activity
import android.content.Context
import android.content.pm.PackageManager
import android.media.MediaCodecList
import android.os.Build
import android.provider.Settings
import android.view.InputDevice
import androidx.window.layout.WindowMetricsCalculator
import java.util.UUID
import kotlin.math.roundToInt

/** Everything the host needs to know about this device, gathered for the Hello message. */
object DeviceInfo {
    data class Geometry(
        val width: Int,
        val height: Int,
        val dpi: Int,
        val refreshMhz: Int,
        val rotation: Int,
        val posture: Int,
    )

    data class Hello(
        val deviceId: String,
        val name: String,
        val model: String,
        val sdk: Int,
        val geometry: Geometry,
        val codecMask: Int,
        val inputCaps: Int,
        val transport: Int,
    )

    fun hello(activity: Activity, transport: Int, posture: Int): Hello = Hello(
        deviceId = deviceId(activity),
        name = deviceName(activity),
        model = "${Build.MANUFACTURER} ${Build.MODEL}",
        sdk = Build.VERSION.SDK_INT,
        geometry = geometry(activity, posture),
        codecMask = decodableCodecs(),
        inputCaps = inputCaps(activity),
        transport = transport,
    )

    /** Full window size in physical pixels (the stream activity runs edge-to-edge, fullscreen). */
    fun geometry(activity: Activity, posture: Int): Geometry {
        val bounds = WindowMetricsCalculator.getOrCreate().computeCurrentWindowMetrics(activity).bounds
        @Suppress("DEPRECATION")
        val display = if (Build.VERSION.SDK_INT >= 30) activity.display else activity.windowManager.defaultDisplay
        // Highest refresh rate the panel offers at its current resolution (e.g. 120 Hz).
        val refresh = display?.supportedModes
            ?.filter { it.physicalWidth == display.mode.physicalWidth && it.physicalHeight == display.mode.physicalHeight }
            ?.maxOfOrNull { it.refreshRate } ?: display?.refreshRate ?: 60f
        return Geometry(
            width = bounds.width(),
            height = bounds.height(),
            dpi = activity.resources.displayMetrics.densityDpi,
            refreshMhz = (refresh * 1000).roundToInt(),
            rotation = display?.rotation ?: 0,
            posture = posture,
        )
    }

    /** Hardware decoders only: software decoding can't keep up with a 120 Hz desktop. */
    fun decodableCodecs(): Int {
        var mask = 0
        val infos = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.filter { !it.isEncoder }
        fun hw(info: android.media.MediaCodecInfo): Boolean =
            if (Build.VERSION.SDK_INT >= 29) info.isHardwareAccelerated
            else !info.name.startsWith("OMX.google.") && !info.name.startsWith("c2.android.")
        for ((mime, codec) in listOf(
            "video/avc" to Proto.CODEC_H264,
            "video/hevc" to Proto.CODEC_HEVC,
            "video/av01" to Proto.CODEC_AV1,
        )) {
            if (infos.any { info -> hw(info) && info.supportedTypes.any { it.equals(mime, ignoreCase = true) } }) {
                mask = mask or Proto.codecBit(codec)
            }
        }
        // H.264 is always decodable, even if only in software.
        return mask or Proto.codecBit(Proto.CODEC_H264)
    }

    fun inputCaps(context: Context): Int {
        var caps = Proto.CAP_TOUCH or Proto.CAP_KEYBOARD
        val hasStylus = InputDevice.getDeviceIds().any { id ->
            InputDevice.getDevice(id)?.supportsSource(InputDevice.SOURCE_STYLUS) == true
        } || context.packageManager.hasSystemFeature("com.sec.feature.spen_usp")
        if (hasStylus) {
            caps = caps or Proto.CAP_PEN or Proto.CAP_PEN_TILT or Proto.CAP_PEN_HOVER or Proto.CAP_PEN_ERASER
        }
        return caps
    }

    fun deviceName(context: Context): String =
        Settings.Global.getString(context.contentResolver, Settings.Global.DEVICE_NAME)
            ?: "${Build.MANUFACTURER} ${Build.MODEL}"

    private fun deviceId(context: Context): String {
        val prefs = context.getSharedPreferences("device", Context.MODE_PRIVATE)
        return prefs.getString("id", null) ?: UUID.randomUUID().toString().also {
            prefs.edit().putString("id", it).apply()
        }
    }

    fun isTouchscreen(context: Context) = context.packageManager.hasSystemFeature(PackageManager.FEATURE_TOUCHSCREEN)
}
