package com.displaymaster.client

import android.annotation.SuppressLint
import android.content.Context
import android.os.Build
import android.view.InputDevice
import android.view.MotionEvent
import android.view.PointerIcon
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView

/**
 * Shows the decoded desktop and captures pen/touch/mouse input on top of it. Sized by
 * the caller to the video's aspect ratio, so view coordinates map 1:1 onto the video.
 */
@SuppressLint("ViewConstructor")
class StreamSurfaceView(context: Context, private val client: NativeClient) :
    SurfaceView(context), SurfaceHolder.Callback {

    val router = InputRouter(client, ::normalize, resources.displayMetrics.density)
    private var fps = 60

    init {
        holder.addCallback(this)
        isFocusable = true
        isFocusableInTouchMode = true
    }

    fun setVideoFps(value: Int) {
        if (value == fps) return
        fps = value
        applyFrameRate()
    }

    private fun normalize(x: Float, y: Float): Pair<Float, Float> =
        (x / width.coerceAtLeast(1)) to (y / height.coerceAtLeast(1))

    private fun applyFrameRate() {
        // Ask the panel for the stream's rate (120 Hz on the Tab S7+ / Fold 7).
        if (Build.VERSION.SDK_INT >= 30 && holder.surface.isValid) {
            holder.surface.setFrameRate(fps.toFloat(), Surface.FRAME_RATE_COMPATIBILITY_DEFAULT)
        }
    }

    override fun surfaceCreated(h: SurfaceHolder) {
        client.setSurface(h.surface)
        applyFrameRate()
    }

    override fun surfaceChanged(h: SurfaceHolder, format: Int, width: Int, height: Int) = Unit

    override fun surfaceDestroyed(h: SurfaceHolder) = client.setSurface(null)

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        // Deliver pointer events as they arrive instead of once per vsync (lower pen latency).
        if (Build.VERSION.SDK_INT >= 30) requestUnbufferedDispatch(InputDevice.SOURCE_CLASS_POINTER)
        // Windows draws its own cursor into the video; hide Android's pointer on top of it.
        pointerIcon = PointerIcon.getSystemIcon(context, PointerIcon.TYPE_NULL)
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        if (e.actionMasked == MotionEvent.ACTION_DOWN) requestUnbufferedDispatch(e)
        return router.onTouchEvent(e)
    }

    override fun onHoverEvent(e: MotionEvent): Boolean = router.onGenericMotionEvent(e) || super.onHoverEvent(e)

    override fun onGenericMotionEvent(e: MotionEvent): Boolean =
        router.onGenericMotionEvent(e) || super.onGenericMotionEvent(e)
}
