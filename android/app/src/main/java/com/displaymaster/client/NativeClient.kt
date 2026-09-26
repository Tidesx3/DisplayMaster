package com.displaymaster.client

import android.view.Surface

/** Wire-protocol enums, mirrored from protocol/include/dm/protocol.h. */
object Proto {
    const val DEFAULT_PORT = 47800

    const val CODEC_H264 = 1
    const val CODEC_HEVC = 2
    const val CODEC_AV1 = 3
    fun codecBit(codec: Int) = 1 shl codec

    const val TRANSPORT_USB_ADB = 1
    const val TRANSPORT_WIFI = 3

    const val CAP_TOUCH = 1 shl 0
    const val CAP_PEN = 1 shl 1
    const val CAP_PEN_TILT = 1 shl 2
    const val CAP_PEN_HOVER = 1 shl 3
    const val CAP_PEN_ERASER = 1 shl 4
    const val CAP_KEYBOARD = 1 shl 5

    const val PEN_IN_RANGE = 1 shl 0
    const val PEN_CONTACT = 1 shl 1
    const val PEN_BARREL = 1 shl 2
    const val PEN_ERASER = 1 shl 3
    const val PEN_INVERTED = 1 shl 4
    const val PEN_BARREL2 = 1 shl 5

    const val ACTION_DOWN = 0
    const val ACTION_MOVE = 1
    const val ACTION_UP = 2
    const val ACTION_CANCEL = 3

    const val MOUSE_MOVE_ABS = 0
    const val MOUSE_MOVE_REL = 1
    const val MOUSE_BUTTON = 2
    const val MOUSE_WHEEL = 3
    const val BUTTON_LEFT = 0
    const val BUTTON_RIGHT = 1
    const val BUTTON_MIDDLE = 2

    const val POSTURE_UNKNOWN = 0
    const val POSTURE_FLAT = 1
    const val POSTURE_HALF_OPENED = 2
    const val POSTURE_FOLDED = 3
}

enum class DisplayMode(val wire: Int) { Extend(0), Mirror(1), Tablet(2) }

enum class TouchMode(val wire: Int) { Touch(0), Mouse(1), Trackpad(2) }

data class VideoConfig(
    val codec: Int,
    val width: Int,
    val height: Int,
    val fps: Int,
    val bitrateKbps: Int,
    val contentX: Float,
    val contentY: Float,
    val contentW: Float,
    val contentH: Float,
    val mode: DisplayMode,
) {
    val codecName: String
        get() = when (codec) {
            Proto.CODEC_H264 -> "H.264"
            Proto.CODEC_HEVC -> "HEVC"
            Proto.CODEC_AV1 -> "AV1"
            else -> "?"
        }
}

data class StreamStats(val fps: Float, val mbps: Float, val rttMs: Float, val decodeMs: Float, val dropped: Int)

/**
 * Thin JNI wrapper over the C++ client (android/app/src/main/cpp). Callbacks arrive on
 * native threads; [Listener] implementations must hop to the main thread themselves.
 */
class NativeClient(private val listener: Listener) : AutoCloseable {
    interface Listener {
        fun onState(state: State, message: String)
        fun onVideoConfig(config: VideoConfig)
        fun onStats(stats: StreamStats)
        /** Wi-Fi: has this device paired with the PC that has this public key (hex)? */
        fun isKnownPc(pcKey: String): Boolean
        /** A PC this device doesn't know: show [code]; answer with [confirmPairing]. */
        fun onPairing(code: String, pcKey: String)
    }

    enum class State { Connecting, Connected, Streaming, Disconnected, Error }

    private var handle: Long = nativeCreate()

    /** [identity]: this device's key; non-null encrypts the connection (always over Wi-Fi). */
    fun connect(host: String, port: Int, info: DeviceInfo.Hello, settings: Settings, identity: ByteArray?) = nativeConnect(
        handle, host, port, info.deviceId, info.name, info.model, info.sdk,
        info.geometry.width, info.geometry.height, info.geometry.dpi, info.geometry.refreshMhz,
        info.geometry.rotation, info.geometry.posture, info.codecMask, info.inputCaps, info.transport,
        settings.displayMode.wire, settings.touchMode.wire, settings.maxFps, settings.preferredCodec, identity,
    )

    fun confirmPairing(codesMatch: Boolean) = nativeConfirmPairing(handle, codesMatch)

    fun disconnect() = nativeDisconnect(handle)
    fun setSurface(surface: Surface?) = nativeSetSurface(handle, surface)

    fun sendPen(flags: Int, x: Float, y: Float, pressure: Float, tiltRad: Float, orientationRad: Float, timeUs: Long) =
        nativeSendPen(handle, flags, x, y, pressure, tiltRad, orientationRad, timeUs)

    fun sendTouch(
        action: Int, actionId: Int, ids: IntArray, xs: FloatArray, ys: FloatArray,
        pressures: FloatArray, majors: FloatArray, count: Int, timeUs: Long,
    ) = nativeSendTouch(handle, action, actionId, ids, xs, ys, pressures, majors, count, timeUs)

    fun sendMouse(kind: Int, x: Float, y: Float, button: Int = 0, down: Boolean = false) =
        nativeSendMouse(handle, kind, x, y, button, down)

    fun sendKey(evdevScan: Int, set1: Int, extended: Boolean, down: Boolean, unicode: Int) =
        nativeSendKey(handle, evdevScan, set1, extended, down, unicode)

    fun sendGeometry(g: DeviceInfo.Geometry) =
        nativeSendGeometry(handle, g.width, g.height, g.dpi, g.refreshMhz, g.rotation, g.posture)

    fun sendSettings(mode: DisplayMode, touchMode: TouchMode, maxFps: Int, bitrateKbps: Int, codec: Int) =
        nativeSendSettings(handle, mode.wire, touchMode.wire, maxFps, bitrateKbps, codec)

    override fun close() {
        if (handle != 0L) {
            nativeDestroy(handle)
            handle = 0
        }
    }

    // --- called from native code ---
    @Suppress("unused")
    private fun onNativeState(state: Int, message: String) =
        listener.onState(State.entries.getOrElse(state) { State.Error }, message)

    @Suppress("unused")
    private fun onNativeVideoConfig(
        codec: Int, w: Int, h: Int, fps: Int, kbps: Int, cx: Float, cy: Float, cw: Float, ch: Float, mode: Int,
    ) = listener.onVideoConfig(
        VideoConfig(codec, w, h, fps, kbps, cx, cy, cw, ch, DisplayMode.entries.getOrElse(mode) { DisplayMode.Extend }),
    )

    @Suppress("unused")
    private fun onNativeStats(fps: Float, mbps: Float, rttMs: Float, decodeMs: Float, dropped: Int) =
        listener.onStats(StreamStats(fps, mbps, rttMs, decodeMs, dropped))

    @Suppress("unused")
    private fun isNativeKnownPc(pcKey: String): Boolean = listener.isKnownPc(pcKey)

    @Suppress("unused")
    private fun onNativePairing(code: String, pcKey: String) = listener.onPairing(code, pcKey)

    private external fun nativeCreate(): Long
    private external fun nativeDestroy(handle: Long)
    private external fun nativeConnect(
        handle: Long, host: String, port: Int, deviceId: String, name: String, model: String, sdk: Int,
        width: Int, height: Int, dpi: Int, refreshMhz: Int, rotation: Int, posture: Int,
        codecs: Int, inputCaps: Int, transport: Int, mode: Int, touchMode: Int, maxFps: Int, codec: Int,
        identity: ByteArray?,
    )
    private external fun nativeDisconnect(handle: Long)
    private external fun nativeConfirmPairing(handle: Long, codesMatch: Boolean)
    private external fun nativeSetSurface(handle: Long, surface: Surface?)
    private external fun nativeSendPen(
        handle: Long, flags: Int, x: Float, y: Float, pressure: Float, tiltRad: Float, orientationRad: Float, timeUs: Long,
    )
    private external fun nativeSendTouch(
        handle: Long, action: Int, actionId: Int, ids: IntArray, xs: FloatArray, ys: FloatArray,
        pressures: FloatArray, majors: FloatArray, count: Int, timeUs: Long,
    )
    private external fun nativeSendMouse(handle: Long, kind: Int, x: Float, y: Float, button: Int, down: Boolean)
    private external fun nativeSendKey(handle: Long, evdev: Int, set1: Int, extended: Boolean, down: Boolean, unicode: Int)
    private external fun nativeSendGeometry(
        handle: Long, width: Int, height: Int, dpi: Int, refreshMhz: Int, rotation: Int, posture: Int,
    )
    private external fun nativeSendSettings(
        handle: Long, mode: Int, touchMode: Int, maxFps: Int, bitrateKbps: Int, codec: Int,
    )

    companion object {
        init {
            System.loadLibrary("dmclient")
        }

        /** A new long-term key (32 bytes) from the OS random generator. */
        @JvmStatic
        external fun nativeGenerateIdentity(): ByteArray
    }
}
