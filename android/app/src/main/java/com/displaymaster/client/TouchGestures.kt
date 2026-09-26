package com.displaymaster.client

import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.ln

/**
 * Multi-finger gestures for Touch mode, touchpad style so they work in every Windows app:
 * two fingers scroll (content follows the fingers) or pinch to zoom (Ctrl + wheel), three
 * fingers swipe: up = Task View, down = show desktop, left/right = previous app. One finger
 * stays real Windows touch; the router hands a sequence over here once a second finger lands.
 *
 * Plain logic on positions (no Android types) so it can be unit tested.
 */
class TouchGestures(private val out: Output, private val pixelsPerDp: Float) {
    interface Output {
        /** Lift the native touch contacts of this sequence without a click. */
        fun cancelTouch()
        /** Put the mouse where the gesture happens (wheel events go to the window under it). */
        fun moveMouse(viewX: Float, viewY: Float)
        /** Wheel in notches: +y scrolls up (content moves down), +x scrolls right. */
        fun wheel(x: Float, y: Float)
        fun key(vk: Int, char: Int, down: Boolean)
    }

    enum class Mode { None, Undecided, Scroll, Pinch, Swipe }

    var mode = Mode.None
        private set

    private var startX = 0f
    private var startY = 0f
    private var lastX = 0f
    private var lastY = 0f
    private var startSpan = 0f
    private var lastSpan = 0f
    private var scrollX = 0f
    private var scrollY = 0f
    private var zoom = 0f
    private var ctrlDown = false
    private var swipeFired = false

    val active: Boolean get() = mode != Mode.None

    /** A new finger landed; `xs`/`ys` hold all fingers now down. */
    fun pointerDown(xs: FloatArray, ys: FloatArray, count: Int) {
        if (count < 2) return
        if (mode == Mode.None) out.cancelTouch()
        releaseCtrl()
        mode = if (count >= 3) Mode.Swipe else Mode.Undecided
        swipeFired = false
        anchor(xs, ys, count)
        if (mode == Mode.Undecided) out.moveMouse(startX, startY)
    }

    /** A finger lifted but others remain. */
    fun pointerUp(xs: FloatArray, ys: FloatArray, count: Int) {
        // Re-anchor on the remaining fingers so nothing jumps.
        if (count >= 1) {
            val (cx, cy) = centroid(xs, ys, count)
            lastX = cx
            lastY = cy
            lastSpan = span(xs, ys, count, cx, cy)
        }
    }

    fun move(xs: FloatArray, ys: FloatArray, count: Int) {
        if (!active || count < 2) return
        val (cx, cy) = centroid(xs, ys, count)
        val s = span(xs, ys, count, cx, cy)
        when (mode) {
            Mode.Undecided -> {
                val moved = hypot(cx - startX, cy - startY)
                val stretched = abs(s - startSpan)
                if (stretched > DECIDE_DP * pixelsPerDp && stretched > moved) {
                    mode = Mode.Pinch
                    out.key(VK_CONTROL, 0, true)
                    ctrlDown = true
                } else if (moved > DECIDE_DP * pixelsPerDp) {
                    mode = Mode.Scroll
                }
            }
            Mode.Scroll -> {
                scrollX += -(cx - lastX) / (SCROLL_DP_PER_NOTCH * pixelsPerDp)
                scrollY += (cy - lastY) / (SCROLL_DP_PER_NOTCH * pixelsPerDp)
                if (abs(scrollX) >= 0.25f || abs(scrollY) >= 0.25f) {
                    out.wheel(scrollX, scrollY)
                    scrollX = 0f
                    scrollY = 0f
                }
            }
            Mode.Pinch -> {
                if (lastSpan > 0f && s > 0f) zoom += ln(s / lastSpan) / ZOOM_PER_NOTCH
                while (abs(zoom) >= 1f) {
                    val step = if (zoom > 0) 1f else -1f
                    out.wheel(0f, step)  // Ctrl held: spreading fingers zooms in
                    zoom -= step
                }
            }
            Mode.Swipe -> {
                val dx = cx - startX
                val dy = cy - startY
                if (!swipeFired && hypot(dx, dy) > SWIPE_DP * pixelsPerDp) {
                    swipeFired = true
                    swipe(dx, dy)
                }
            }
            Mode.None -> Unit
        }
        lastX = cx
        lastY = cy
        lastSpan = s
    }

    /** Every finger lifted (or the sequence was cancelled). */
    fun end() {
        releaseCtrl()
        mode = Mode.None
        scrollX = 0f
        scrollY = 0f
        zoom = 0f
    }

    private fun swipe(dx: Float, dy: Float) {
        if (abs(dy) > abs(dx)) {
            if (dy < 0) combo(VK_LWIN, VK_TAB, 0) else combo(VK_LWIN, 0, 'd'.code)  // Task View / desktop
        } else {
            combo(VK_MENU, VK_TAB, 0)  // Alt+Tab: back to the previous app
        }
    }

    private fun combo(modifier: Int, vk: Int, char: Int) {
        out.key(modifier, 0, true)
        out.key(vk, char, true)
        out.key(vk, char, false)
        out.key(modifier, 0, false)
    }

    private fun releaseCtrl() {
        if (ctrlDown) out.key(VK_CONTROL, 0, false)
        ctrlDown = false
    }

    private fun anchor(xs: FloatArray, ys: FloatArray, count: Int) {
        val (cx, cy) = centroid(xs, ys, count)
        startX = cx
        startY = cy
        lastX = cx
        lastY = cy
        startSpan = span(xs, ys, count, cx, cy)
        lastSpan = startSpan
        scrollX = 0f
        scrollY = 0f
        zoom = 0f
    }

    private fun centroid(xs: FloatArray, ys: FloatArray, count: Int): Pair<Float, Float> {
        var x = 0f
        var y = 0f
        for (i in 0 until count) {
            x += xs[i]
            y += ys[i]
        }
        return x / count to y / count
    }

    private fun span(xs: FloatArray, ys: FloatArray, count: Int, cx: Float, cy: Float): Float {
        var d = 0f
        for (i in 0 until count) d += hypot(xs[i] - cx, ys[i] - cy)
        return d / count
    }

    companion object {
        const val VK_CONTROL = 0xA2
        const val VK_MENU = 0xA4
        const val VK_LWIN = 0x5B
        const val VK_TAB = 0x09
        private const val DECIDE_DP = 12f           // movement before scroll vs. pinch is decided
        private const val SCROLL_DP_PER_NOTCH = 40f
        private val ZOOM_PER_NOTCH = ln(1.12f)      // 12 % spread = one zoom step
        private const val SWIPE_DP = 60f
    }
}
