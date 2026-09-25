package com.displaymaster.client

import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import kotlin.math.abs
import kotlin.math.hypot

/**
 * Turns Android input events into protocol messages. Coordinates are normalized to the
 * video rectangle supplied by [mapper] (the host maps them onto its monitor).
 */
class InputRouter(
    private val client: NativeClient,
    private val mapper: (x: Float, y: Float) -> Pair<Float, Float>,
    private val pixelsPerDp: Float,
) {
    var touchMode: TouchMode = TouchMode.Touch
    var trackpadSensitivity = 1.6f

    private var penInRange = false
    private val trackpad = TrackpadGestures()
    private val mouseEmu = MouseEmulation()

    // Reused buffers for touch frames (up to 10 contacts).
    private val ids = IntArray(MAX_POINTERS)
    private val xs = FloatArray(MAX_POINTERS)
    private val ys = FloatArray(MAX_POINTERS)
    private val pressures = FloatArray(MAX_POINTERS)
    private val majors = FloatArray(MAX_POINTERS)

    fun onTouchEvent(e: MotionEvent): Boolean {
        val tool = e.getToolType(e.actionIndex)
        return when {
            tool == MotionEvent.TOOL_TYPE_STYLUS || tool == MotionEvent.TOOL_TYPE_ERASER -> { pen(e); true }
            e.isFromSource(InputDevice.SOURCE_MOUSE) -> { hardwareMouse(e); true }
            penInRange -> true  // palm rejection: ignore fingers while the pen is near
            else -> when (touchMode) {
                TouchMode.Touch -> { touch(e); true }
                TouchMode.Mouse -> mouseEmu.onEvent(e)
                TouchMode.Trackpad -> trackpad.onEvent(e)
            }
        }
    }

    /** Hover and scroll events (pen hovering, mouse moving without buttons, wheel). */
    fun onGenericMotionEvent(e: MotionEvent): Boolean {
        val tool = e.getToolType(0)
        if (tool == MotionEvent.TOOL_TYPE_STYLUS || tool == MotionEvent.TOOL_TYPE_ERASER) {
            pen(e)
            return true
        }
        if (e.isFromSource(InputDevice.SOURCE_MOUSE) || e.isFromSource(InputDevice.SOURCE_TOUCHPAD)) {
            hardwareMouse(e)
            return true
        }
        return false
    }

    // ------------------------------------------------------------------ pen

    private fun pen(e: MotionEvent) {
        val action = e.actionMasked
        val eraser = e.getToolType(0) == MotionEvent.TOOL_TYPE_ERASER
        val buttons = e.buttonState
        var flags = Proto.PEN_IN_RANGE
        val contact = when (action) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_MOVE -> true
            else -> false
        }
        if (contact) flags = flags or Proto.PEN_CONTACT
        if (buttons and MotionEvent.BUTTON_STYLUS_PRIMARY != 0) flags = flags or Proto.PEN_BARREL
        if (buttons and MotionEvent.BUTTON_STYLUS_SECONDARY != 0) flags = flags or Proto.PEN_BARREL2
        if (eraser) flags = flags or Proto.PEN_ERASER
        if (action == MotionEvent.ACTION_HOVER_EXIT || action == MotionEvent.ACTION_CANCEL) {
            flags = 0  // out of range
        }
        penInRange = flags and Proto.PEN_IN_RANGE != 0

        // Batched samples first: keeps fast strokes smooth at the pen's full report rate.
        if (contact || action == MotionEvent.ACTION_HOVER_MOVE) {
            for (h in 0 until e.historySize) {
                val (x, y) = mapper(e.getHistoricalX(0, h), e.getHistoricalY(0, h))
                client.sendPen(
                    flags, x, y, e.getHistoricalPressure(0, h),
                    e.getHistoricalAxisValue(MotionEvent.AXIS_TILT, 0, h),
                    e.getHistoricalAxisValue(MotionEvent.AXIS_ORIENTATION, 0, h),
                    e.getHistoricalEventTime(h) * 1000,
                )
            }
        }
        val (x, y) = mapper(e.x, e.y)
        client.sendPen(
            flags, x, y, if (contact) e.pressure else 0f,
            e.getAxisValue(MotionEvent.AXIS_TILT), e.getAxisValue(MotionEvent.AXIS_ORIENTATION),
            e.eventTime * 1000,
        )
    }

    // ------------------------------------------------------------------ native touch

    private fun touch(e: MotionEvent) {
        val action = when (e.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> Proto.ACTION_DOWN
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> Proto.ACTION_UP
            MotionEvent.ACTION_CANCEL -> Proto.ACTION_CANCEL
            else -> Proto.ACTION_MOVE
        }
        val count = minOf(e.pointerCount, MAX_POINTERS)
        for (i in 0 until count) {
            val (x, y) = mapper(e.getX(i), e.getY(i))
            ids[i] = e.getPointerId(i)
            xs[i] = x
            ys[i] = y
            pressures[i] = e.getPressure(i).coerceIn(0f, 1f)
            // Contact size in the same normalized units as x.
            majors[i] = (mapper(e.getX(i) + e.getTouchMajor(i), e.getY(i)).first - x).coerceAtLeast(0f)
        }
        client.sendTouch(action, e.getPointerId(e.actionIndex), ids, xs, ys, pressures, majors, count, e.eventTime * 1000)
    }

    // ------------------------------------------------------------------ hardware mouse

    private fun hardwareMouse(e: MotionEvent) {
        when (e.actionMasked) {
            MotionEvent.ACTION_SCROLL -> client.sendMouse(
                Proto.MOUSE_WHEEL, e.getAxisValue(MotionEvent.AXIS_HSCROLL), e.getAxisValue(MotionEvent.AXIS_VSCROLL),
            )
            MotionEvent.ACTION_BUTTON_PRESS, MotionEvent.ACTION_BUTTON_RELEASE -> {
                val down = e.actionMasked == MotionEvent.ACTION_BUTTON_PRESS
                val button = when (e.actionButton) {
                    MotionEvent.BUTTON_SECONDARY -> Proto.BUTTON_RIGHT
                    MotionEvent.BUTTON_TERTIARY -> Proto.BUTTON_MIDDLE
                    else -> Proto.BUTTON_LEFT
                }
                moveAbs(e.x, e.y)
                client.sendMouse(Proto.MOUSE_BUTTON, 0f, 0f, button, down)
            }
            else -> moveAbs(e.x, e.y)
        }
    }

    private fun moveAbs(viewX: Float, viewY: Float) {
        val (x, y) = mapper(viewX, viewY)
        client.sendMouse(Proto.MOUSE_MOVE_ABS, x, y)
    }

    private fun click(button: Int) {
        client.sendMouse(Proto.MOUSE_BUTTON, 0f, 0f, button, true)
        client.sendMouse(Proto.MOUSE_BUTTON, 0f, 0f, button, false)
    }

    // ------------------------------------------------------------------ gesture translators

    /** Direct pointing: the cursor jumps to the finger. Tap = click, two-finger tap = right click. */
    private inner class MouseEmulation {
        private var downTime = 0L
        private var secondFinger = false
        private var dragging = false
        private var startX = 0f
        private var startY = 0f
        private var lastScrollY = 0f

        fun onEvent(e: MotionEvent): Boolean {
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    downTime = e.eventTime; secondFinger = false; dragging = false
                    startX = e.x; startY = e.y
                    moveAbs(e.x, e.y)
                }
                MotionEvent.ACTION_POINTER_DOWN -> {
                    secondFinger = true
                    lastScrollY = e.getY(0)
                    if (dragging) { client.sendMouse(Proto.MOUSE_BUTTON, 0f, 0f, Proto.BUTTON_LEFT, false); dragging = false }
                }
                MotionEvent.ACTION_MOVE -> {
                    if (secondFinger && e.pointerCount >= 2) {
                        scroll(e.getY(0) - lastScrollY); lastScrollY = e.getY(0)
                    } else if (!secondFinger) {
                        if (!dragging && hypot(e.x - startX, e.y - startY) > TAP_SLOP_DP * pixelsPerDp) {
                            dragging = true
                            client.sendMouse(Proto.MOUSE_BUTTON, 0f, 0f, Proto.BUTTON_LEFT, true)
                        }
                        moveAbs(e.x, e.y)
                    }
                }
                MotionEvent.ACTION_UP -> {
                    if (dragging) client.sendMouse(Proto.MOUSE_BUTTON, 0f, 0f, Proto.BUTTON_LEFT, false)
                    else if (!secondFinger) click(Proto.BUTTON_LEFT)
                    else if (e.eventTime - downTime < TAP_TIMEOUT_MS) click(Proto.BUTTON_RIGHT)
                    dragging = false
                }
                MotionEvent.ACTION_CANCEL -> {
                    if (dragging) client.sendMouse(Proto.MOUSE_BUTTON, 0f, 0f, Proto.BUTTON_LEFT, false)
                    dragging = false
                }
            }
            return true
        }

        private var scrollAccum = 0f
        private fun scroll(dy: Float) {
            scrollAccum += dy / (SCROLL_DP_PER_NOTCH * pixelsPerDp)
            if (abs(scrollAccum) >= 0.25f) {
                client.sendMouse(Proto.MOUSE_WHEEL, 0f, scrollAccum)
                scrollAccum = 0f
            }
        }
    }

    /**
     * Laptop-style trackpad: relative motion, tap = click, two-finger tap = right click,
     * two-finger drag = scroll, double-tap-and-hold = drag.
     */
    private inner class TrackpadGestures {
        private var lastX = 0f
        private var lastY = 0f
        private var downTime = 0L
        private var moved = 0f
        private var fingers = 0
        private var maxFingers = 0
        private var lastTapUp = 0L
        private var dragging = false
        private var scrollAccumX = 0f
        private var scrollAccumY = 0f

        fun onEvent(e: MotionEvent): Boolean {
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    lastX = e.x; lastY = e.y; downTime = e.eventTime; moved = 0f
                    fingers = 1; maxFingers = 1
                    // Second tap shortly after a tap: press and drag.
                    if (e.eventTime - lastTapUp < DOUBLE_TAP_MS) {
                        dragging = true
                        client.sendMouse(Proto.MOUSE_BUTTON, 0f, 0f, Proto.BUTTON_LEFT, true)
                    }
                }
                MotionEvent.ACTION_POINTER_DOWN -> {
                    fingers = e.pointerCount; maxFingers = maxOf(maxFingers, fingers)
                    lastX = centroidX(e); lastY = centroidY(e)
                }
                MotionEvent.ACTION_POINTER_UP -> {
                    fingers = e.pointerCount - 1
                    // Re-anchor on the remaining finger(s) to avoid a jump.
                    val remaining = (0 until e.pointerCount).filter { it != e.actionIndex }
                    lastX = remaining.map { e.getX(it) }.average().toFloat()
                    lastY = remaining.map { e.getY(it) }.average().toFloat()
                }
                MotionEvent.ACTION_MOVE -> {
                    val cx = centroidX(e)
                    val cy = centroidY(e)
                    val dx = cx - lastX
                    val dy = cy - lastY
                    lastX = cx; lastY = cy
                    moved += hypot(dx, dy)
                    if (e.pointerCount >= 2) {
                        scrollAccumX += -dx / (SCROLL_DP_PER_NOTCH * pixelsPerDp)
                        scrollAccumY += dy / (SCROLL_DP_PER_NOTCH * pixelsPerDp)
                        if (abs(scrollAccumX) >= 0.25f || abs(scrollAccumY) >= 0.25f) {
                            client.sendMouse(Proto.MOUSE_WHEEL, scrollAccumX, scrollAccumY)
                            scrollAccumX = 0f; scrollAccumY = 0f
                        }
                    } else {
                        // Mild acceleration: fast flicks travel further.
                        val speed = hypot(dx, dy) / pixelsPerDp
                        val gain = trackpadSensitivity * (1f + (speed / 20f).coerceAtMost(1.5f))
                        client.sendMouse(Proto.MOUSE_MOVE_REL, dx * gain, dy * gain)
                    }
                }
                MotionEvent.ACTION_UP -> {
                    val isTap = e.eventTime - downTime < TAP_TIMEOUT_MS && moved < TAP_SLOP_DP * pixelsPerDp
                    when {
                        dragging -> {
                            client.sendMouse(Proto.MOUSE_BUTTON, 0f, 0f, Proto.BUTTON_LEFT, false)
                            dragging = false
                        }
                        isTap && maxFingers == 1 -> { click(Proto.BUTTON_LEFT); lastTapUp = e.eventTime }
                        isTap && maxFingers == 2 -> click(Proto.BUTTON_RIGHT)
                        isTap && maxFingers >= 3 -> click(Proto.BUTTON_MIDDLE)
                    }
                    fingers = 0
                }
                MotionEvent.ACTION_CANCEL -> {
                    if (dragging) client.sendMouse(Proto.MOUSE_BUTTON, 0f, 0f, Proto.BUTTON_LEFT, false)
                    dragging = false
                }
            }
            return true
        }

        private fun centroidX(e: MotionEvent) = (0 until e.pointerCount).map { e.getX(it) }.average().toFloat()
        private fun centroidY(e: MotionEvent) = (0 until e.pointerCount).map { e.getY(it) }.average().toFloat()
    }

    companion object {
        private const val MAX_POINTERS = 10
        private const val TAP_TIMEOUT_MS = 250L
        private const val DOUBLE_TAP_MS = 300L
        private const val TAP_SLOP_DP = 8f
        private const val SCROLL_DP_PER_NOTCH = 40f
    }
}

/**
 * Keyboard forwarding. Hardware keys go as scancodes (the PC's layout decides the
 * character); soft-keyboard keys without a scancode use [set1ForKeyCode] or Unicode.
 */
object KeyMapper {
    private val SYSTEM_KEYS = setOf(
        KeyEvent.KEYCODE_BACK, KeyEvent.KEYCODE_VOLUME_UP, KeyEvent.KEYCODE_VOLUME_DOWN,
        KeyEvent.KEYCODE_POWER, KeyEvent.KEYCODE_HOME, KeyEvent.KEYCODE_APP_SWITCH,
    )

    /** Returns true if the event was sent to the PC. */
    fun forward(client: NativeClient, e: KeyEvent): Boolean {
        if (e.action != KeyEvent.ACTION_DOWN && e.action != KeyEvent.ACTION_UP) return false
        // Volume/back/home stay on the device unless they come from an external keyboard.
        if (e.keyCode in SYSTEM_KEYS) {
            val external = android.os.Build.VERSION.SDK_INT >= 29 && e.device?.isExternal == true
            if (!external) return false
        }
        val down = e.action == KeyEvent.ACTION_DOWN
        val direct = set1ForKeyCode(e.keyCode)
        val unicode = if (down) e.getUnicodeChar(e.metaState) else 0
        client.sendKey(e.scanCode, direct?.first ?: 0, direct?.second ?: false, down, unicode)
        return true
    }

    /** Text typed on the soft keyboard. */
    fun typeText(client: NativeClient, text: String) {
        var i = 0
        while (i < text.length) {
            val cp = text.codePointAt(i)
            if (cp == '\n'.code) {
                client.sendKey(0, 0x1C, false, true, 0)
                client.sendKey(0, 0x1C, false, false, 0)
            } else {
                client.sendKey(0, 0, false, true, cp)
            }
            i += Character.charCount(cp)
        }
    }

    fun backspace(client: NativeClient) {
        client.sendKey(0, 0x0E, false, true, 0)
        client.sendKey(0, 0x0E, false, false, 0)
    }

    fun set1ForKeyCode(keyCode: Int): Pair<Int, Boolean>? = when (keyCode) {
        KeyEvent.KEYCODE_DEL -> 0x0E to false
        KeyEvent.KEYCODE_FORWARD_DEL -> 0x53 to true
        KeyEvent.KEYCODE_ENTER -> 0x1C to false
        KeyEvent.KEYCODE_NUMPAD_ENTER -> 0x1C to true
        KeyEvent.KEYCODE_TAB -> 0x0F to false
        KeyEvent.KEYCODE_ESCAPE -> 0x01 to false
        KeyEvent.KEYCODE_SPACE -> 0x39 to false
        KeyEvent.KEYCODE_DPAD_UP -> 0x48 to true
        KeyEvent.KEYCODE_DPAD_DOWN -> 0x50 to true
        KeyEvent.KEYCODE_DPAD_LEFT -> 0x4B to true
        KeyEvent.KEYCODE_DPAD_RIGHT -> 0x4D to true
        KeyEvent.KEYCODE_MOVE_HOME -> 0x47 to true
        KeyEvent.KEYCODE_MOVE_END -> 0x4F to true
        KeyEvent.KEYCODE_PAGE_UP -> 0x49 to true
        KeyEvent.KEYCODE_PAGE_DOWN -> 0x51 to true
        KeyEvent.KEYCODE_CTRL_LEFT -> 0x1D to false
        KeyEvent.KEYCODE_SHIFT_LEFT -> 0x2A to false
        KeyEvent.KEYCODE_ALT_LEFT -> 0x38 to false
        KeyEvent.KEYCODE_META_LEFT -> 0x5B to true
        else -> null
    }
}
