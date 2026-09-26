package com.displaymaster.client

import android.app.Application
import android.content.Context
import androidx.lifecycle.AndroidViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update

enum class Phase { Idle, Connecting, Streaming, Error }

data class UiState(
    val phase: Phase = Phase.Idle,
    val target: String = "",          // "USB" or an IP
    val hostName: String = "",        // PC name from Welcome
    val message: String = "",         // error text
    val video: VideoConfig? = null,
    val stats: StreamStats? = null,
    val usb: Boolean = false,
    val awaitingApproval: Boolean = false,  // PC is asking its user to allow this device
    val pairingCode: String = "",           // Wi-Fi: same code on both screens
    val confirmOnDevice: Boolean = false,   // new PC: the user must confirm the code here too
)

data class Settings(
    val displayMode: DisplayMode = DisplayMode.Extend,
    val touchMode: TouchMode = TouchMode.Touch,
    val maxFps: Int = 120,
    val showStats: Boolean = false,
    val preferredCodec: Int = Proto.CODEC_HEVC,
)

data class RecentHost(val address: String, val name: String)

class ConnectionViewModel(app: Application) : AndroidViewModel(app), NativeClient.Listener {
    private val prefs = app.getSharedPreferences("settings", Context.MODE_PRIVATE)

    private val _state = MutableStateFlow(UiState())
    val state: StateFlow<UiState> = _state.asStateFlow()

    private val _settings = MutableStateFlow(loadSettings())
    val settings: StateFlow<Settings> = _settings.asStateFlow()

    private val _recents = MutableStateFlow(loadRecents())
    val recents: StateFlow<List<RecentHost>> = _recents.asStateFlow()

    val client = NativeClient(this)
    val discovery = PcDiscovery(app)
    private val pairing = Pairing(app)
    @Volatile private var pendingPcKey: String? = null

    /** Address, port and transport of the last attempt, for "Retry". */
    var lastTarget: Triple<String, Int, Int>? = null
        private set

    fun connect(address: String, port: Int, hello: DeviceInfo.Hello) {
        lastTarget = Triple(address, port, hello.transport)
        val usb = hello.transport == Proto.TRANSPORT_USB_ADB
        _state.value = UiState(phase = Phase.Connecting, target = if (usb) "USB" else address, usb = usb)
        // Wi-Fi is always encrypted; USB goes through adb's authorized link.
        client.connect(address, port, hello, _settings.value, if (usb) null else pairing.identity)
    }

    /** The user compared the code with the PC's. A match pins the PC's key for next time. */
    fun confirmPairing(codesMatch: Boolean) {
        if (codesMatch) pendingPcKey?.let(pairing::remember)
        pendingPcKey = null
        _state.update { it.copy(confirmOnDevice = false) }
        client.confirmPairing(codesMatch)
    }

    fun disconnect() {
        client.disconnect()
        _state.value = UiState()
    }

    fun dismissError() = _state.update { UiState() }

    fun updateSettings(transform: (Settings) -> Settings) {
        val s = transform(_settings.value)
        _settings.value = s
        prefs.edit()
            .putString("displayMode", s.displayMode.name)
            .putString("touchMode", s.touchMode.name)
            .putInt("maxFps", s.maxFps)
            .putBoolean("showStats", s.showStats)
            .putInt("codec", s.preferredCodec)
            .apply()
        if (_state.value.phase == Phase.Streaming) pushSettings()
    }

    private fun pushSettings() {
        val s = _settings.value
        client.sendSettings(s.displayMode, s.touchMode, s.maxFps, 0, s.preferredCodec)
    }

    // --- NativeClient.Listener (native threads; StateFlow is thread-safe) ---

    override fun onState(state: NativeClient.State, message: String) {
        when (state) {
            NativeClient.State.Connecting ->
                if (message.startsWith("approval:")) {
                    _state.update { it.copy(awaitingApproval = true, pairingCode = message.removePrefix("approval:")) }
                }
            NativeClient.State.Connected -> {
                _state.update { it.copy(hostName = message) }
                if (!_state.value.usb) rememberHost(_state.value.target, message)
            }
            NativeClient.State.Streaming -> _state.update { it.copy(phase = Phase.Streaming) }
            NativeClient.State.Disconnected -> _state.value = UiState()
            NativeClient.State.Error -> _state.update { it.copy(phase = Phase.Error, message = message, video = null) }
        }
    }

    override fun onVideoConfig(config: VideoConfig) = _state.update { it.copy(video = config) }

    override fun onStats(stats: StreamStats) = _state.update { it.copy(stats = stats) }

    override fun isKnownPc(pcKey: String): Boolean = pairing.isKnown(pcKey)

    override fun onPairing(code: String, pcKey: String) {
        pendingPcKey = pcKey
        _state.update { it.copy(pairingCode = code, confirmOnDevice = true) }
    }

    override fun onCleared() {
        discovery.stop()
        client.close()
    }

    // --- persistence ---

    private fun loadSettings() = Settings(
        displayMode = runCatching { DisplayMode.valueOf(prefs.getString("displayMode", null)!!) }.getOrDefault(DisplayMode.Extend),
        touchMode = runCatching { TouchMode.valueOf(prefs.getString("touchMode", null)!!) }.getOrDefault(TouchMode.Touch),
        maxFps = prefs.getInt("maxFps", 120),
        showStats = prefs.getBoolean("showStats", false),
        preferredCodec = prefs.getInt("codec", Proto.CODEC_HEVC),
    )

    private fun loadRecents(): List<RecentHost> =
        prefs.getString("recents", "")!!.split('\n').filter { it.contains('\t') }.map {
            val (addr, name) = it.split('\t', limit = 2)
            RecentHost(addr, name)
        }

    private fun rememberHost(address: String, name: String) {
        val list = (listOf(RecentHost(address, name)) + _recents.value.filter { it.address != address }).take(5)
        _recents.value = list
        prefs.edit().putString("recents", list.joinToString("\n") { "${it.address}\t${it.name}" }).apply()
    }

    fun forgetHost(address: String) {
        val list = _recents.value.filter { it.address != address }
        _recents.value = list
        prefs.edit().putString("recents", list.joinToString("\n") { "${it.address}\t${it.name}" }).apply()
    }
}
