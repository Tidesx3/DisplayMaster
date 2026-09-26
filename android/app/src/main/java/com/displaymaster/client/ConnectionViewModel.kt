package com.displaymaster.client

import android.app.Application
import android.content.Context
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
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
    val reconnecting: Int = 0,              // > 0: the connection dropped, this is the retry number
)

data class Settings(
    val displayMode: DisplayMode = DisplayMode.Extend,
    val touchMode: TouchMode = TouchMode.Touch,
    val maxFps: Int = 120,
    val showStats: Boolean = false,
    val showShortcuts: Boolean = true,  // drawing-app shortcut bar on the stream's left edge
    val autoConnect: Boolean = false,   // connect to a known PC as soon as it shows up on the network
    val touchGestures: Boolean = true,  // Touch mode: 2-finger scroll / pinch-zoom, 3-finger swipes
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

    /** Builds a fresh Hello (current screen size, fold posture); set by the visible activity. */
    @Volatile var helloProvider: ((transport: Int) -> DeviceInfo.Hello)? = null

    // Automatic reconnect after a dropped connection, and optional auto-connect.
    private var reconnectJob: Job? = null
    @Volatile private var autoAttempt = false        // this connection was started by auto-connect
    private var autoConnectPaused = false            // after a manual disconnect or a failed auto attempt

    init {
        viewModelScope.launch { discovery.pcs.collect { maybeAutoConnect(it) } }
    }

    fun connect(address: String, port: Int, hello: DeviceInfo.Hello) = start(address, port, hello, retry = 0)

    private fun start(address: String, port: Int, hello: DeviceInfo.Hello, retry: Int, auto: Boolean = false) {
        if (retry == 0) reconnectJob?.cancel()
        autoAttempt = auto
        lastTarget = Triple(address, port, hello.transport)
        val usb = hello.transport == Proto.TRANSPORT_USB_ADB
        val hostName = if (retry > 0) _state.value.hostName else ""
        _state.value = UiState(phase = Phase.Connecting, target = if (usb) "USB" else address, usb = usb,
            hostName = hostName, reconnecting = retry)
        // Wi-Fi is always encrypted; USB goes through adb's authorized link.
        client.connect(address, port, hello, _settings.value, if (usb) null else pairing.identity)
    }

    /** A live connection broke: try again a few times before showing the error. */
    private fun scheduleReconnect(retry: Int, message: String): Boolean {
        val (address, port, transport) = lastTarget ?: return false
        if (retry > RECONNECT_DELAYS_S.size || !isRetryable(message)) return false
        val delaySeconds = RECONNECT_DELAYS_S[retry - 1]
        _state.update { it.copy(phase = Phase.Connecting, reconnecting = retry, video = null, stats = null, message = message) }
        reconnectJob = viewModelScope.launch {
            delay(delaySeconds * 1000L)
            val hello = helloProvider?.invoke(transport) ?: return@launch
            start(address, port, hello, retry)
        }
        return true
    }

    /** Errors a retry can't fix: the user or the PC decided, or an app needs an update. */
    private fun isRetryable(message: String) = RETRY_NEVER.none { message.contains(it, ignoreCase = true) }

    private fun maybeAutoConnect(pcs: List<DiscoveredPc>) {
        if (!_settings.value.autoConnect || autoConnectPaused || _state.value.phase != Phase.Idle) return
        val known = _recents.value.map { it.name }.toSet()
        val pc = pcs.firstOrNull { it.name in known } ?: return
        val hello = helloProvider?.invoke(Proto.TRANSPORT_WIFI) ?: return
        start(pc.address, pc.port, hello, retry = 0, auto = true)
    }

    /** The user compared the code with the PC's. A match pins the PC's key for next time. */
    fun confirmPairing(codesMatch: Boolean) {
        if (codesMatch) pendingPcKey?.let(pairing::remember)
        pendingPcKey = null
        _state.update { it.copy(confirmOnDevice = false) }
        client.confirmPairing(codesMatch)
    }

    fun disconnect() {
        reconnectJob?.cancel()
        autoConnectPaused = true  // don't reconnect straight away after the user said stop
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
            .putBoolean("showShortcuts", s.showShortcuts)
            .putBoolean("autoConnect", s.autoConnect)
            .putBoolean("touchGestures", s.touchGestures)
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
            NativeClient.State.Streaming -> _state.update { it.copy(phase = Phase.Streaming, reconnecting = 0, message = "") }
            NativeClient.State.Disconnected -> _state.value = UiState()
            NativeClient.State.Error -> {
                val s = _state.value
                when {
                    // Auto-connect found nothing to do (e.g. an unknown PC): stay quiet.
                    autoAttempt -> {
                        autoConnectPaused = true
                        _state.value = UiState()
                    }
                    // Was streaming, or already retrying: try again.
                    (s.phase == Phase.Streaming || s.reconnecting > 0) && scheduleReconnect(s.reconnecting + 1, message) -> Unit
                    else -> _state.update { it.copy(phase = Phase.Error, message = message, video = null, reconnecting = 0) }
                }
            }
        }
    }

    override fun onVideoConfig(config: VideoConfig) = _state.update { it.copy(video = config) }

    override fun onStats(stats: StreamStats) = _state.update { it.copy(stats = stats) }

    override fun isKnownPc(pcKey: String): Boolean = pairing.isKnown(pcKey)

    override fun onPairing(code: String, pcKey: String) {
        // Auto-connect never starts a pairing on its own.
        if (autoAttempt) {
            client.confirmPairing(false)
            return
        }
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
        showShortcuts = prefs.getBoolean("showShortcuts", true),
        autoConnect = prefs.getBoolean("autoConnect", false),
        touchGestures = prefs.getBoolean("touchGestures", true),
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

    private companion object {
        // Seconds before each reconnect attempt: about a minute in total.
        val RECONNECT_DELAYS_S = listOf(1, 2, 3, 5, 5, 5, 5, 5, 5, 5, 5, 10)
        val RETRY_NEVER = listOf(
            "approved", "Pairing cancelled", "version", "Update the DisplayMaster app", "update DisplayMaster",
            "Disconnected from the PC", "can't decode", "Could not start video",
        )
    }

    fun forgetHost(address: String) {
        val list = _recents.value.filter { it.address != address }
        _recents.value = list
        prefs.edit().putString("recents", list.joinToString("\n") { "${it.address}\t${it.name}" }).apply()
    }
}
