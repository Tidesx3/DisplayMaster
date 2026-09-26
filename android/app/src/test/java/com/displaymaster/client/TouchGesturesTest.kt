package com.displaymaster.client

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class TouchGesturesTest {
    private class Recorder : TouchGestures.Output {
        val events = mutableListOf<String>()
        var wheelX = 0f
        var wheelY = 0f
        override fun cancelTouch() { events += "cancel" }
        override fun moveMouse(viewX: Float, viewY: Float) { events += "move" }
        override fun wheel(x: Float, y: Float) {
            wheelX += x
            wheelY += y
            events += "wheel"
        }
        override fun key(vk: Int, char: Int, down: Boolean) {
            events += "key ${vk.toString(16)}/${char} ${if (down) "down" else "up"}"
        }
    }

    private val out = Recorder()
    private val g = TouchGestures(out, pixelsPerDp = 1f)

    private fun fingers(vararg p: Pair<Float, Float>) =
        Triple(p.map { it.first }.toFloatArray(), p.map { it.second }.toFloatArray(), p.size)

    private fun down(vararg p: Pair<Float, Float>) = fingers(*p).let { (x, y, n) -> g.pointerDown(x, y, n) }
    private fun move(vararg p: Pair<Float, Float>) = fingers(*p).let { (x, y, n) -> g.move(x, y, n) }

    @Test
    fun twoFingersDraggingDownScrollsUp() {
        down(100f to 100f, 200f to 100f)
        assertEquals(listOf("cancel", "move"), out.events)  // native touch lifted, mouse moved there
        for (y in 110..300 step 10) move(100f to y.toFloat(), 200f to y.toFloat())
        assertEquals(TouchGestures.Mode.Scroll, g.mode)
        assertTrue("content follows the fingers: wheel up, got ${out.wheelY}", out.wheelY > 3f)
        assertEquals(0f, out.wheelX, 0.3f)
        g.end()
        assertEquals(TouchGestures.Mode.None, g.mode)
    }

    @Test
    fun spreadingFingersZoomsInWithCtrlHeld() {
        down(150f to 100f, 250f to 100f)
        for (d in 1..20) move((150f - 5 * d) to 100f, (250f + 5 * d) to 100f)
        assertEquals(TouchGestures.Mode.Pinch, g.mode)
        val ctrlDown = out.events.indexOf("key a2/0 down")
        assertTrue(ctrlDown >= 0)
        assertTrue("zoom in = wheel up with Ctrl, got ${out.wheelY}", out.wheelY >= 3f)
        g.end()
        assertEquals("key a2/0 up", out.events.last())  // Ctrl never stays stuck
    }

    @Test
    fun pinchingFingersZoomsOut() {
        down(0f to 100f, 400f to 100f)
        for (d in 1..20) move((0f + 8 * d) to 100f, (400f - 8 * d) to 100f)
        assertTrue(out.wheelY <= -3f)
    }

    @Test
    fun threeFingerSwipes() {
        fun swipe(dx: Float, dy: Float): List<String> {
            out.events.clear()
            down(100f to 300f, 150f to 300f, 200f to 300f)
            for (i in 1..10) move((100f + dx * i) to (300f + dy * i), (150f + dx * i) to (300f + dy * i), (200f + dx * i) to (300f + dy * i))
            g.end()
            return out.events.filter { it.startsWith("key") }
        }
        assertEquals(listOf("key 5b/0 down", "key 9/0 down", "key 9/0 up", "key 5b/0 up"), swipe(0f, -15f))  // Win+Tab
        assertEquals(listOf("key 5b/0 down", "key 0/100 down", "key 0/100 up", "key 5b/0 up"), swipe(0f, 15f))  // Win+D
        assertEquals(listOf("key a4/0 down", "key 9/0 down", "key 9/0 up", "key a4/0 up"), swipe(15f, 0f))  // Alt+Tab
    }

    @Test
    fun swipeFiresOnlyOnce() {
        down(100f to 300f, 150f to 300f, 200f to 300f)
        for (i in 1..40) move(100f to (300f - 10f * i), 150f to (300f - 10f * i), 200f to (300f - 10f * i))
        assertEquals(4, out.events.count { it.startsWith("key") })
    }

    @Test
    fun smallJitterDoesNothing() {
        down(100f to 100f, 200f to 100f)
        move(102f to 101f, 201f to 99f)
        assertEquals(TouchGestures.Mode.Undecided, g.mode)
        g.end()
        assertEquals(listOf("cancel", "move"), out.events)
    }
}
