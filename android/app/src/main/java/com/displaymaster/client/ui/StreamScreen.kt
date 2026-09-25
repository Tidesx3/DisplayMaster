package com.displaymaster.client.ui

import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.isImeVisible
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.Gesture
import androidx.compose.material.icons.rounded.Keyboard
import androidx.compose.material.icons.rounded.LinkOff
import androidx.compose.material.icons.rounded.Mouse
import androidx.compose.material.icons.rounded.OpenInFull
import androidx.compose.material.icons.automirrored.rounded.ScreenShare
import androidx.compose.material.icons.rounded.TouchApp
import androidx.compose.material.icons.rounded.Usb
import androidx.compose.material.icons.rounded.Wifi
import androidx.compose.material3.AssistChip
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.text.TextRange
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import com.displaymaster.client.DisplayMode
import com.displaymaster.client.KeyMapper
import com.displaymaster.client.NativeClient
import com.displaymaster.client.Settings
import com.displaymaster.client.StreamSurfaceView
import com.displaymaster.client.TouchMode
import com.displaymaster.client.UiState
import com.displaymaster.client.ui.theme.LocalStatusColors
import com.displaymaster.client.ui.theme.StatsTextStyle
import kotlinx.coroutines.delay

@Composable
fun StreamScreen(
    state: UiState,
    settings: Settings,
    client: NativeClient,
    onSettings: ((Settings) -> Settings) -> Unit,
    onDisconnect: () -> Unit,
) {
    val video = state.video ?: return
    var panelOpen by remember { mutableStateOf(false) }
    var keyboardOn by remember { mutableStateOf(false) }
    // Edge swipes happen by accident while drawing: back toggles the panel instead of leaving.
    BackHandler { panelOpen = !panelOpen }
    // Auto-hide keeps the desktop unobstructed; any settings change restarts the timer.
    LaunchedEffect(panelOpen, settings) {
        if (panelOpen) {
            delay(8000)
            panelOpen = false
        }
    }

    Box(Modifier.fillMaxSize().background(Color.Black)) {
        AndroidView(
            factory = { ctx -> StreamSurfaceView(ctx, client) },
            update = { view ->
                view.router.touchMode = settings.touchMode
                view.setVideoFps(video.fps)
            },
            modifier = Modifier.align(Alignment.Center).aspectRatio(video.width.toFloat() / video.height),
        )

        if (settings.showStats) StatsPill(state, Modifier.align(Alignment.TopStart).padding(12.dp))

        EdgeHandle(visible = !panelOpen, onOpen = { panelOpen = true }, modifier = Modifier.align(Alignment.CenterEnd))

        // Tap outside the panel closes it.
        if (panelOpen) {
            Box(
                Modifier.fillMaxSize().clickable(
                    interactionSource = remember { MutableInteractionSource() },
                    indication = null,
                ) { panelOpen = false },
            )
        }
        AnimatedVisibility(
            visible = panelOpen,
            enter = slideInHorizontally { it } + fadeIn(),
            exit = slideOutHorizontally { it } + fadeOut(),
            modifier = Modifier.align(Alignment.CenterEnd),
        ) {
            QuickPanel(
                state = state,
                settings = settings,
                keyboardOn = keyboardOn,
                onSettings = onSettings,
                onKeyboard = { keyboardOn = !keyboardOn; panelOpen = false },
                onDisconnect = onDisconnect,
            )
        }

        if (keyboardOn) KeyboardBridge(client, onClose = { keyboardOn = false })
    }
}

/** A slim grab handle on the right edge; tap or swipe left to open the quick panel. */
@Composable
private fun EdgeHandle(visible: Boolean, onOpen: () -> Unit, modifier: Modifier = Modifier) {
    val alpha by animateFloatAsState(if (visible) 1f else 0f, label = "handle")
    Box(
        modifier
            .alpha(alpha)
            .width(22.dp)
            .height(96.dp)
            .pointerInput(Unit) { detectHorizontalDragGestures { _, drag -> if (drag < -8) onOpen() } }
            .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null, onClick = onOpen),
        contentAlignment = Alignment.CenterEnd,
    ) {
        Box(
            Modifier
                .padding(end = 4.dp)
                .width(5.dp)
                .height(56.dp)
                .background(Color.White.copy(alpha = 0.28f), CircleShape),
        )
    }
}

@Composable
private fun QuickPanel(
    state: UiState,
    settings: Settings,
    keyboardOn: Boolean,
    onSettings: ((Settings) -> Settings) -> Unit,
    onKeyboard: () -> Unit,
    onDisconnect: () -> Unit,
) {
    Surface(
        shape = RoundedCornerShape(topStart = 28.dp, bottomStart = 28.dp),
        color = MaterialTheme.colorScheme.surfaceContainerHigh.copy(alpha = 0.96f),
        tonalElevation = 6.dp,
        shadowElevation = 12.dp,
        modifier = Modifier.width(320.dp).fillMaxHeight(0.92f),
    ) {
        Column(
            Modifier.padding(20.dp).verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(18.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                AppLogo(size = 32.dp)
                Spacer(Modifier.size(12.dp))
                Column(Modifier.weight(1f)) {
                    Text(state.hostName.ifBlank { "Your PC" }, style = MaterialTheme.typography.titleMedium, maxLines = 1)
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Box(Modifier.size(8.dp).background(LocalStatusColors.current.connected, CircleShape))
                        Spacer(Modifier.size(6.dp))
                        Text("Connected", style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                }
            }
            state.video?.let { v ->
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    AssistChip(
                        onClick = {},
                        label = { Text(if (state.usb) "USB" else "Wi-Fi") },
                        leadingIcon = { Icon(if (state.usb) Icons.Rounded.Usb else Icons.Rounded.Wifi, null, Modifier.size(18.dp)) },
                    )
                    AssistChip(onClick = {}, label = { Text("${v.codecName} · ${v.fps} Hz") })
                }
            }
            ChoiceRow(
                label = "Display",
                options = listOf(DisplayMode.Extend to "Extend", DisplayMode.Mirror to "Mirror"),
                selected = if (settings.displayMode == DisplayMode.Tablet) DisplayMode.Mirror else settings.displayMode,
                onSelect = { m -> onSettings { it.copy(displayMode = m) } },
                icons = listOf(Icons.Rounded.OpenInFull, Icons.AutoMirrored.Rounded.ScreenShare),
            )
            ChoiceRow(
                label = "Finger input",
                options = listOf(TouchMode.Touch to "Touch", TouchMode.Mouse to "Mouse", TouchMode.Trackpad to "Pad"),
                selected = settings.touchMode,
                onSelect = { m -> onSettings { it.copy(touchMode = m) } },
                icons = listOf(Icons.Rounded.TouchApp, Icons.Rounded.Mouse, Icons.Rounded.Gesture),
            )
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Performance overlay", style = MaterialTheme.typography.bodyLarge, modifier = Modifier.weight(1f))
                Switch(checked = settings.showStats, onCheckedChange = { v -> onSettings { it.copy(showStats = v) } })
            }
            FilledTonalButton(onClick = onKeyboard, modifier = Modifier.fillMaxWidth()) {
                Icon(Icons.Rounded.Keyboard, null)
                Spacer(Modifier.size(8.dp))
                Text(if (keyboardOn) "Hide keyboard" else "Show keyboard")
            }
            Button(
                onClick = onDisconnect,
                colors = ButtonDefaults.buttonColors(
                    containerColor = MaterialTheme.colorScheme.errorContainer,
                    contentColor = MaterialTheme.colorScheme.onErrorContainer,
                ),
                modifier = Modifier.fillMaxWidth(),
            ) {
                Icon(Icons.Rounded.LinkOff, null)
                Spacer(Modifier.size(8.dp))
                Text("Disconnect")
            }
        }
    }
}

@Composable
private fun StatsPill(state: UiState, modifier: Modifier = Modifier) {
    val s = state.stats ?: return
    val v = state.video
    Surface(color = Color.Black.copy(alpha = 0.55f), contentColor = Color.White, shape = CircleShape, modifier = modifier) {
        Text(
            buildString {
                if (v != null) append("${v.codecName} ${v.width}×${v.height}  ·  ")
                append("%.0f fps  ·  %.1f Mbps  ·  RTT %.1f ms  ·  decode %.1f ms".format(s.fps, s.mbps, s.rttMs, s.decodeMs))
                if (s.dropped > 0) append("  ·  ${s.dropped} dropped")
            },
            style = StatsTextStyle,
            modifier = Modifier.padding(horizontal = 12.dp, vertical = 6.dp),
        )
    }
}

/**
 * Invisible text field that brings up the soft keyboard and forwards what's typed.
 * Hardware keyboards bypass this (MainActivity forwards their scancodes directly).
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun KeyboardBridge(client: NativeClient, onClose: () -> Unit) {
    val sentinel = " "  // lets us see backspace on an otherwise empty field
    var value by remember { mutableStateOf(TextFieldValue(sentinel, TextRange(1))) }
    val focus = remember { FocusRequester() }
    val keyboard = LocalSoftwareKeyboardController.current
    BasicTextField(
        value = value,
        onValueChange = { new ->
            val text = new.text
            when {
                text.length < sentinel.length -> KeyMapper.backspace(client)
                text.startsWith(sentinel) && text.length > sentinel.length -> KeyMapper.typeText(client, text.substring(1))
            }
            value = TextFieldValue(sentinel, TextRange(1))
        },
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Ascii, imeAction = ImeAction.Send, autoCorrectEnabled = false),
        keyboardActions = KeyboardActions(onSend = { KeyMapper.typeText(client, "\n") }),
        modifier = Modifier.size(1.dp).alpha(0f).focusRequester(focus),
    )
    LaunchedEffect(Unit) {
        delay(50)
        focus.requestFocus()
        keyboard?.show()
    }
    // Dismissing the IME (back gesture / hide key) ends keyboard mode.
    val imeVisible = WindowInsets.isImeVisible
    var wasVisible by remember { mutableStateOf(false) }
    LaunchedEffect(imeVisible) {
        if (imeVisible) wasVisible = true else if (wasVisible) onClose()
    }
}
