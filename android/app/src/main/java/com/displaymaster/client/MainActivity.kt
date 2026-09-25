package com.displaymaster.client

import android.content.Intent
import android.content.res.Configuration
import android.os.Build
import android.os.Bundle
import android.view.KeyEvent
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.viewModels
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import androidx.window.layout.FoldingFeature
import androidx.window.layout.WindowInfoTracker
import com.displaymaster.client.ui.HomeScreen
import com.displaymaster.client.ui.StreamScreen
import com.displaymaster.client.ui.theme.DisplayMasterTheme
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

class MainActivity : ComponentActivity() {
    private val vm: ConnectionViewModel by viewModels()
    private var posture = Proto.POSTURE_UNKNOWN
    private var geometryJob: Job? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        // Use the full panel, including the area around cameras / cutouts.
        if (Build.VERSION.SDK_INT >= 30) {
            window.attributes.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
        } else if (Build.VERSION.SDK_INT >= 28) {
            window.attributes.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
        }
        trackFoldPosture()
        val hasPen = DeviceInfo.inputCaps(this) and Proto.CAP_PEN != 0

        setContent {
            DisplayMasterTheme {
                val state by vm.state.collectAsStateWithLifecycle()
                val settings by vm.settings.collectAsStateWithLifecycle()
                val recents by vm.recents.collectAsStateWithLifecycle()
                val nearby by vm.discovery.pcs.collectAsStateWithLifecycle()
                val streaming = state.phase == Phase.Streaming && state.video != null

                DisposableEffect(streaming) {
                    applyImmersive(streaming)
                    onDispose { }
                }

                if (streaming) {
                    StreamScreen(
                        state = state,
                        settings = settings,
                        client = vm.client,
                        onSettings = vm::updateSettings,
                        onDisconnect = vm::disconnect,
                    )
                } else {
                    HomeScreen(
                        state = state,
                        settings = settings,
                        recents = recents,
                        nearby = nearby,
                        hasPen = hasPen,
                        onConnectUsb = { connect("127.0.0.1", Proto.DEFAULT_PORT, Proto.TRANSPORT_USB_ADB) },
                        onConnectWifi = { connectTyped(it) },
                        onConnectNearby = { pc -> connect(pc.address, pc.port, Proto.TRANSPORT_WIFI) },
                        onForget = vm::forgetHost,
                        onCancel = vm::disconnect,
                        onDismissError = vm::dismissError,
                        onRetry = vm.lastTarget?.let { (address, port, transport) -> { connect(address, port, transport) } },
                        onSettings = vm::updateSettings,
                    )
                }
            }
        }
        handleLaunchIntent(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleLaunchIntent(intent)
    }

    /**
     * USB_CONNECT: the PC host launches us over adb when a USB device is set up.
     * CONNECT_ADDRESS: connect over Wi-Fi to "host[:port]" (automation, future QR pairing).
     */
    private fun handleLaunchIntent(intent: Intent?) {
        if (intent == null) return
        val idle = vm.state.value.phase == Phase.Idle || vm.state.value.phase == Phase.Error
        if (intent.getBooleanExtra(EXTRA_USB_CONNECT, false)) {
            intent.removeExtra(EXTRA_USB_CONNECT)
            if (idle) connect("127.0.0.1", Proto.DEFAULT_PORT, Proto.TRANSPORT_USB_ADB)
        }
        intent.getStringExtra(EXTRA_CONNECT_ADDRESS)?.let { address ->
            intent.removeExtra(EXTRA_CONNECT_ADDRESS)
            if (idle) connectTyped(address)
        }
    }

    private fun connect(address: String, port: Int, transport: Int) {
        vm.connect(address, port, DeviceInfo.hello(this, transport, posture))
    }

    /** "192.168.1.20" or "192.168.1.20:47801" typed by the user. */
    private fun connectTyped(text: String) {
        val port = text.substringAfterLast(':', "").toIntOrNull()
        val host = if (port != null) text.substringBeforeLast(':') else text
        connect(host, port ?: Proto.DEFAULT_PORT, Proto.TRANSPORT_WIFI)
    }

    // Look for PCs on the network while the app is visible.
    override fun onStart() {
        super.onStart()
        vm.discovery.start()
    }

    override fun onStop() {
        vm.discovery.stop()
        super.onStop()
    }

    private fun applyImmersive(on: Boolean) {
        val controller = WindowCompat.getInsetsController(window, window.decorView)
        if (on) {
            controller.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            controller.hide(WindowInsetsCompat.Type.systemBars())
            window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        } else {
            controller.show(WindowInsetsCompat.Type.systemBars())
            window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        }
    }

    // Rotation, fold/unfold and window resizes arrive here (see configChanges in the manifest).
    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        scheduleGeometryUpdate()
    }

    /** Debounced: folding produces a burst of configuration changes. */
    private fun scheduleGeometryUpdate() {
        if (vm.state.value.phase != Phase.Streaming) return
        geometryJob?.cancel()
        geometryJob = lifecycleScope.launch {
            delay(350)
            vm.client.sendGeometry(DeviceInfo.geometry(this@MainActivity, posture))
        }
    }

    private fun trackFoldPosture() {
        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                WindowInfoTracker.getOrCreate(this@MainActivity).windowLayoutInfo(this@MainActivity).collect { info ->
                    val fold = info.displayFeatures.filterIsInstance<FoldingFeature>().firstOrNull()
                    val newPosture = when (fold?.state) {
                        FoldingFeature.State.HALF_OPENED -> Proto.POSTURE_HALF_OPENED
                        FoldingFeature.State.FLAT -> Proto.POSTURE_FLAT
                        else -> Proto.POSTURE_UNKNOWN
                    }
                    if (newPosture != posture) {
                        posture = newPosture
                        scheduleGeometryUpdate()
                    }
                }
            }
        }
    }

    // Hardware keyboards: forward scancodes to the PC while streaming.
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (vm.state.value.phase == Phase.Streaming && event.device?.isVirtual == false &&
            KeyMapper.forward(vm.client, event)
        ) {
            return true
        }
        return super.dispatchKeyEvent(event)
    }

    companion object {
        const val EXTRA_USB_CONNECT = "com.displaymaster.extra.USB_CONNECT"
        const val EXTRA_CONNECT_ADDRESS = "com.displaymaster.extra.CONNECT_ADDRESS"
    }
}
