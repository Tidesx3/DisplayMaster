package com.displaymaster.client.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.expandVertically
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.shrinkVertically
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.ArrowForward
import androidx.compose.material.icons.rounded.Close
import androidx.compose.material.icons.rounded.Computer
import androidx.compose.material.icons.rounded.Draw
import androidx.compose.material.icons.rounded.ErrorOutline
import androidx.compose.material.icons.rounded.Gesture
import androidx.compose.material.icons.rounded.Mouse
import androidx.compose.material.icons.rounded.OpenInFull
import androidx.compose.material.icons.automirrored.rounded.ScreenShare
import androidx.compose.material.icons.rounded.Keyboard
import androidx.compose.material.icons.rounded.Speed
import androidx.compose.material.icons.rounded.TouchApp
import androidx.compose.material.icons.rounded.Tune
import androidx.compose.material.icons.rounded.Usb
import androidx.compose.material.icons.rounded.Wifi
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.sp
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.material.icons.rounded.Lock
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import com.displaymaster.client.DiscoveredPc
import com.displaymaster.client.DisplayMode
import com.displaymaster.client.Phase
import com.displaymaster.client.RecentHost
import com.displaymaster.client.Settings
import com.displaymaster.client.TouchMode
import com.displaymaster.client.UiState
import com.displaymaster.client.ui.theme.LocalStatusColors

@Composable
fun HomeScreen(
    state: UiState,
    settings: Settings,
    recents: List<RecentHost>,
    nearby: List<DiscoveredPc>,
    hasPen: Boolean,
    onConnectUsb: () -> Unit,
    onConnectWifi: (String) -> Unit,
    onConnectNearby: (DiscoveredPc) -> Unit,
    onForget: (String) -> Unit,
    onCancel: () -> Unit,
    onConfirmPairing: (Boolean) -> Unit,
    onDismissError: () -> Unit,
    onRetry: (() -> Unit)?,
    onSettings: ((Settings) -> Settings) -> Unit,
) {
    Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
        BoxWithConstraints(Modifier.fillMaxSize().safeDrawingPadding()) {
            // Two panes on the Tab S7+ and the unfolded Fold 7; one pane on phones / cover screens.
            val wide = maxWidth >= 720.dp
            val scroll = rememberScrollState()
            Column(
                Modifier.fillMaxSize().verticalScroll(scroll).padding(horizontal = if (wide) 40.dp else 20.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Column(Modifier.widthIn(max = 1120.dp).fillMaxWidth()) {
                    Spacer(Modifier.height(if (wide) 40.dp else 24.dp))
                    Header()
                    Spacer(Modifier.height(24.dp))
                    AnimatedVisibility(
                        visible = state.phase == Phase.Error,
                        enter = expandVertically() + fadeIn(),
                        exit = shrinkVertically() + fadeOut(),
                    ) {
                        ErrorBanner(state.message, onDismissError, onRetry, Modifier.padding(bottom = 16.dp))
                    }
                    if (wide) {
                        Row(horizontalArrangement = Arrangement.spacedBy(20.dp)) {
                            Column(Modifier.weight(1.15f), verticalArrangement = Arrangement.spacedBy(16.dp)) {
                                UsbCard(onConnectUsb)
                                WifiCard(nearby, recents, onConnectNearby, onConnectWifi, onForget)
                            }
                            Column(Modifier.weight(1f)) { SettingsCard(settings, hasPen, onSettings) }
                        }
                    } else {
                        Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
                            UsbCard(onConnectUsb)
                            WifiCard(nearby, recents, onConnectNearby, onConnectWifi, onForget)
                            SettingsCard(settings, hasPen, onSettings)
                        }
                    }
                    Spacer(Modifier.height(32.dp))
                }
            }
        }
    }
    if (state.phase == Phase.Connecting) ConnectingDialog(state, onConfirmPairing, onCancel)
}

@Composable
private fun Header() {
    Row(verticalAlignment = Alignment.CenterVertically) {
        AppLogo(size = 48.dp)
        Spacer(Modifier.size(16.dp))
        Column {
            Text("DisplayMaster", style = MaterialTheme.typography.headlineMedium)
            Text(
                "Use this device as a second screen for your PC",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun UsbCard(onConnect: () -> Unit) {
    Surface(
        shape = MaterialTheme.shapes.large,
        color = MaterialTheme.colorScheme.primaryContainer,
        modifier = Modifier.fillMaxWidth(),
    ) {
        Row(Modifier.padding(20.dp), verticalAlignment = Alignment.CenterVertically) {
            PulsingIcon(Icons.Rounded.Usb, MaterialTheme.colorScheme.onPrimaryContainer, size = 52.dp)
            Spacer(Modifier.size(12.dp))
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text("USB", style = MaterialTheme.typography.titleLarge, color = MaterialTheme.colorScheme.onPrimaryContainer)
                Text(
                    "Plug into your PC. With USB debugging on, the PC starts the stream by itself.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onPrimaryContainer.copy(alpha = 0.8f),
                )
                Spacer(Modifier.height(8.dp))
                Button(onClick = onConnect) {
                    Text("Connect via USB")
                    Spacer(Modifier.size(8.dp))
                    Icon(Icons.AutoMirrored.Rounded.ArrowForward, null, Modifier.size(18.dp))
                }
            }
        }
    }
}

@Composable
private fun WifiCard(
    nearby: List<DiscoveredPc>,
    recents: List<RecentHost>,
    onConnectNearby: (DiscoveredPc) -> Unit,
    onConnect: (String) -> Unit,
    onForget: (String) -> Unit,
) {
    var address by rememberSaveable { mutableStateOf("") }
    val valid = address.isNotBlank()
    SectionCard(title = "Wi-Fi", icon = Icons.Rounded.Wifi) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            OutlinedTextField(
                value = address,
                onValueChange = { address = it.trim() },
                label = { Text("PC address") },
                placeholder = { Text("192.168.1.20") },
                singleLine = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri, imeAction = ImeAction.Go),
                keyboardActions = KeyboardActions(onGo = { if (valid) onConnect(address) }),
                modifier = Modifier.weight(1f),
            )
            Spacer(Modifier.size(12.dp))
            FilledTonalButton(onClick = { onConnect(address) }, enabled = valid, modifier = Modifier.height(56.dp)) {
                Text("Connect")
            }
        }
        if (nearby.isNotEmpty()) {
            Text("On this network", style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Column {
                nearby.forEachIndexed { i, pc ->
                    if (i > 0) HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                    ListItem(
                        headlineContent = { Text(pc.name) },
                        supportingContent = { Text(pc.address) },
                        leadingContent = { Icon(Icons.Rounded.Computer, null, tint = LocalStatusColors.current.connected) },
                        trailingContent = { Icon(Icons.AutoMirrored.Rounded.ArrowForward, "Connect") },
                        colors = ListItemDefaults.colors(containerColor = MaterialTheme.colorScheme.surfaceContainer),
                        modifier = Modifier.fillMaxWidth().clickable { onConnectNearby(pc) },
                    )
                }
            }
        }
        if (recents.isNotEmpty()) {
            Text("Recent", style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Column {
                recents.forEachIndexed { i, host ->
                    if (i > 0) HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                    ListItem(
                        headlineContent = { Text(host.name.ifBlank { host.address }) },
                        supportingContent = { Text(host.address) },
                        leadingContent = { Icon(Icons.Rounded.Computer, null) },
                        trailingContent = {
                            IconButton(onClick = { onForget(host.address) }) {
                                Icon(Icons.Rounded.Close, "Forget ${host.name}")
                            }
                        },
                        colors = ListItemDefaults.colors(containerColor = MaterialTheme.colorScheme.surfaceContainer),
                        modifier = Modifier.fillMaxWidth().clickable { onConnect(host.address) },
                    )
                }
            }
        } else if (nearby.isEmpty()) {
            Text(
                "Turn on Wi-Fi connections in DisplayMaster on your PC. It shows up here automatically, or enter the address it shows.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun SwitchRow(
    icon: androidx.compose.ui.graphics.vector.ImageVector,
    title: String,
    subtitle: String,
    checked: Boolean,
    onChange: (Boolean) -> Unit,
) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Icon(icon, null, tint = MaterialTheme.colorScheme.onSurfaceVariant)
        Spacer(Modifier.size(12.dp))
        Column(Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.bodyLarge)
            Text(subtitle, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        Switch(checked = checked, onCheckedChange = onChange)
    }
}

@Composable
private fun SettingsCard(settings: Settings, hasPen: Boolean, onSettings: ((Settings) -> Settings) -> Unit) {
    SectionCard(title = "Preferences", icon = Icons.Rounded.Tune) {
        ChoiceRow(
            label = "Display",
            options = listOf(DisplayMode.Extend to "Extend", DisplayMode.Mirror to "Mirror"),
            selected = if (settings.displayMode == DisplayMode.Tablet) DisplayMode.Mirror else settings.displayMode,
            onSelect = { m -> onSettings { it.copy(displayMode = m) } },
            icons = listOf(Icons.Rounded.OpenInFull, Icons.AutoMirrored.Rounded.ScreenShare),
        )
        ChoiceRow(
            label = "Finger input",
            options = listOf(TouchMode.Touch to "Touch", TouchMode.Mouse to "Mouse", TouchMode.Trackpad to "Trackpad"),
            selected = settings.touchMode,
            onSelect = { m -> onSettings { it.copy(touchMode = m) } },
            icons = listOf(Icons.Rounded.TouchApp, Icons.Rounded.Mouse, Icons.Rounded.Gesture),
        )
        ChoiceRow(
            label = "Frame rate",
            options = listOf(60 to "60 Hz", 90 to "90 Hz", 120 to "120 Hz"),
            selected = settings.maxFps,
            onSelect = { f -> onSettings { it.copy(maxFps = f) } },
        )
        SwitchRow(
            icon = Icons.Rounded.Speed,
            title = "Performance overlay",
            subtitle = "Frame rate, bitrate and latency while streaming",
            checked = settings.showStats,
            onChange = { v -> onSettings { it.copy(showStats = v) } },
        )
        SwitchRow(
            icon = Icons.Rounded.Wifi,
            title = "Connect automatically",
            subtitle = "When the app is open and a PC you've connected to before is on this Wi-Fi",
            checked = settings.autoConnect,
            onChange = { v -> onSettings { it.copy(autoConnect = v) } },
        )
        SwitchRow(
            icon = Icons.Rounded.Keyboard,
            title = "Shortcut bar",
            subtitle = "Undo, Ctrl, Space, brush size… on the left edge while streaming",
            checked = settings.showShortcuts,
            onChange = { v -> onSettings { it.copy(showShortcuts = v) } },
        )
        if (hasPen) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(Icons.Rounded.Draw, null, tint = LocalStatusColors.current.pen)
                Spacer(Modifier.size(12.dp))
                Text(
                    "Pen detected: pressure, tilt, hover and the side button work in Windows Ink apps.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

@Composable
private fun ErrorBanner(message: String, onDismiss: () -> Unit, onRetry: (() -> Unit)?, modifier: Modifier = Modifier) {
    Surface(color = MaterialTheme.colorScheme.errorContainer, shape = MaterialTheme.shapes.medium, modifier = modifier.fillMaxWidth()) {
        Row(Modifier.padding(start = 16.dp, top = 8.dp, bottom = 8.dp, end = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            Icon(Icons.Rounded.ErrorOutline, null, tint = MaterialTheme.colorScheme.onErrorContainer)
            Spacer(Modifier.size(12.dp))
            Text(
                message.ifBlank { "Connection lost" },
                color = MaterialTheme.colorScheme.onErrorContainer,
                style = MaterialTheme.typography.bodyMedium,
                modifier = Modifier.weight(1f),
            )
            TextButton(onClick = onDismiss) { Text("Dismiss", color = MaterialTheme.colorScheme.onErrorContainer) }
            if (onRetry != null) {
                TextButton(onClick = onRetry) { Text("Retry", color = MaterialTheme.colorScheme.onErrorContainer) }
            }
        }
    }
}

@Composable
private fun ConnectingDialog(state: UiState, onConfirmPairing: (Boolean) -> Unit, onCancel: () -> Unit) {
    val code = state.pairingCode
    Dialog(onDismissRequest = onCancel) {
        Surface(shape = MaterialTheme.shapes.extraLarge, color = MaterialTheme.colorScheme.surfaceContainerHigh) {
            Column(
                Modifier.padding(horizontal = 32.dp, vertical = 28.dp).widthIn(min = 240.dp, max = 360.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(16.dp),
            ) {
                if (state.confirmOnDevice) {
                    Icon(Icons.Rounded.Lock, null, Modifier.size(40.dp), tint = MaterialTheme.colorScheme.primary)
                } else {
                    Box(contentAlignment = Alignment.Center) {
                        CircularProgressIndicator(Modifier.size(56.dp), strokeWidth = 4.dp)
                        AppLogo(size = 26.dp)
                    }
                }
                Text(
                    when {
                        state.confirmOnDevice -> "Pair with your PC"
                        state.awaitingApproval -> "Confirm on your PC"
                        state.reconnecting > 0 -> "Reconnecting"
                        else -> "Connecting"
                    },
                    style = MaterialTheme.typography.titleLarge,
                )
                Text(
                    when {
                        state.confirmOnDevice -> "Check that DisplayMaster on the PC shows the same code. It keeps the connection private."
                        state.awaitingApproval && code.isNotEmpty() -> "Click Allow on the PC if it shows the same code."
                        state.awaitingApproval -> "The first time you connect over Wi-Fi, DisplayMaster on the PC asks to allow this device."
                        state.reconnecting > 0 ->
                            "The connection to ${state.hostName.ifBlank { "your PC" }} dropped. Trying again (${state.reconnecting})…"
                        state.target == "USB" -> "Reaching your PC over USB…"
                        else -> "Reaching ${state.target}…"
                    },
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    textAlign = TextAlign.Center,
                )
                if (code.isNotEmpty() && (state.confirmOnDevice || state.awaitingApproval)) {
                    Text(
                        code,
                        style = MaterialTheme.typography.displaySmall,
                        fontFamily = FontFamily.Monospace,
                        fontWeight = FontWeight.SemiBold,
                        letterSpacing = 4.sp,
                    )
                }
                if (state.confirmOnDevice) {
                    Button(onClick = { onConfirmPairing(true) }, modifier = Modifier.fillMaxWidth()) { Text("Codes match") }
                    TextButton(onClick = { onConfirmPairing(false) }) { Text("They're different") }
                } else {
                    TextButton(onClick = onCancel, colors = ButtonDefaults.textButtonColors()) { Text("Cancel") }
                }
            }
        }
    }
}
