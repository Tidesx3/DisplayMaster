package com.displaymaster.client.ui

import com.displaymaster.client.R
import androidx.compose.ui.res.stringResource
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.expandHorizontally
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.shrinkHorizontally
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.waitForUpOrCancellation
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.Redo
import androidx.compose.material.icons.automirrored.rounded.Undo
import androidx.compose.material.icons.rounded.ChevronLeft
import androidx.compose.material.icons.rounded.ZoomIn
import androidx.compose.material.icons.rounded.ZoomOut
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.displaymaster.client.NativeClient

/**
 * Drawing-app shortcuts on the left edge of the stream. Keys go as "virtual" keys, so the
 * PC's layout decides which key is meant (Ctrl+Z stays undo on QWERTZ / AZERTY).
 * Modifiers work both ways: hold one while drawing with the pen, or tap it to latch it
 * (it stays pressed until tapped again).
 */
@Composable
fun ShortcutBar(client: NativeClient, modifier: Modifier = Modifier) {
    var expanded by rememberSaveable { mutableStateOf(true) }
    val latched = remember { mutableStateListOf<Int>() }
    // Leaving the stream must not leave Ctrl or Space stuck on the PC.
    DisposableEffect(Unit) {
        onDispose { latched.forEach { client.sendShortcutKey(it, 0, false) } }
    }

    Box(modifier) {
        AnimatedVisibility(
            visible = expanded,
            enter = expandHorizontally(expandFrom = Alignment.Start) + fadeIn(),
            exit = shrinkHorizontally(shrinkTowards = Alignment.Start) + fadeOut(),
        ) {
            Surface(
                shape = RoundedCornerShape(topEnd = 20.dp, bottomEnd = 20.dp),
                color = MaterialTheme.colorScheme.surfaceContainerHigh.copy(alpha = 0.94f),
                tonalElevation = 4.dp,
                shadowElevation = 8.dp,
            ) {
                Column(
                    Modifier.padding(vertical = 8.dp, horizontal = 6.dp).verticalScroll(rememberScrollState()),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(4.dp),
                ) {
                    ShortcutButton(icon = Icons.Rounded.ChevronLeft, label = stringResource(R.string.sc_hide)) { expanded = false }
                    Divider()
                    ShortcutButton(icon = Icons.AutoMirrored.Rounded.Undo, label = stringResource(R.string.sc_undo)) { client.combo(VK_CONTROL, 'z') }
                    ShortcutButton(icon = Icons.AutoMirrored.Rounded.Redo, label = stringResource(R.string.sc_redo)) { client.combo(VK_CONTROL, 'y') }
                    Divider()
                    for ((vk, label) in MODIFIERS) ModifierButton(client, vk, stringResource(label), latched)
                    Divider()
                    ShortcutButton(text = "[", label = stringResource(R.string.sc_brush_smaller)) { client.tap(0, '['.code) }
                    ShortcutButton(text = "]", label = stringResource(R.string.sc_brush_bigger)) { client.tap(0, ']'.code) }
                    Divider()
                    ShortcutButton(icon = Icons.Rounded.ZoomIn, label = stringResource(R.string.sc_zoom_in)) { client.combo(VK_CONTROL, vk = VK_OEM_PLUS) }
                    ShortcutButton(icon = Icons.Rounded.ZoomOut, label = stringResource(R.string.sc_zoom_out)) { client.combo(VK_CONTROL, vk = VK_OEM_MINUS) }
                    ShortcutButton(text = "Esc", label = stringResource(R.string.sc_escape)) { client.tap(VK_ESCAPE, 0) }
                }
            }
        }
        if (!expanded) {
            // Slim tab to bring the bar back.
            Box(
                Modifier
                    .align(Alignment.CenterStart)
                    .width(22.dp)
                    .height(96.dp)
                    .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) { expanded = true },
                contentAlignment = Alignment.CenterStart,
            ) {
                Box(Modifier.padding(start = 4.dp).width(5.dp).height(56.dp).background(Color.White.copy(alpha = 0.28f), CircleShape))
            }
        }
    }
}

@Composable
private fun Divider() = HorizontalDivider(Modifier.width(36.dp).padding(vertical = 2.dp))

@Composable
private fun ShortcutButton(
    icon: ImageVector? = null,
    text: String? = null,
    label: String,
    active: Boolean = false,
    modifier: Modifier = Modifier,
    onClick: (() -> Unit)? = null,
) {
    val colors = MaterialTheme.colorScheme
    Column(
        modifier
            .width(56.dp)
            .background(if (active) colors.primary else Color.Transparent, RoundedCornerShape(12.dp))
            .let { m -> if (onClick != null) m.clickable(onClick = onClick) else m }
            .padding(vertical = 6.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        val tint = if (active) colors.onPrimary else colors.onSurface
        if (icon != null) Icon(icon, contentDescription = label, tint = tint, modifier = Modifier.size(24.dp))
        if (text != null) {
            Box(Modifier.height(24.dp), contentAlignment = Alignment.Center) {
                Text(text, color = tint, fontWeight = FontWeight.SemiBold, fontSize = 16.sp)
            }
        }
        if (label.isNotEmpty()) {
            Text(label, color = if (active) colors.onPrimary else colors.onSurfaceVariant, fontSize = 10.sp, maxLines = 1)
        }
    }
}

/** Held while pressed; a quick tap latches it until the next tap. */
@Composable
private fun ModifierButton(client: NativeClient, vk: Int, label: String, latched: MutableList<Int>) {
    var pressed by remember { mutableStateOf(false) }
    val isLatched = vk in latched
    ShortcutButton(
        text = label,
        label = if (isLatched) stringResource(R.string.sc_locked) else "",
        active = pressed || isLatched,
        modifier = Modifier.pointerInput(vk) {
            awaitEachGesture {
                val down = awaitFirstDown()
                down.consume()
                val wasLatched = vk in latched
                if (!wasLatched) client.sendShortcutKey(vk, 0, true)
                pressed = true
                val up = waitForUpOrCancellation()
                pressed = false
                val quickTap = up != null && up.uptimeMillis - down.uptimeMillis < LATCH_TAP_MS
                when {
                    wasLatched -> { latched.remove(vk); client.sendShortcutKey(vk, 0, false) }
                    quickTap -> latched.add(vk)  // stays down
                    else -> client.sendShortcutKey(vk, 0, false)
                }
            }
        },
    )
}

private fun NativeClient.tap(vk: Int, char: Int) {
    sendShortcutKey(vk, char, true)
    sendShortcutKey(vk, char, false)
}

/** Modifier + key, e.g. Ctrl+Z. The key is given by character or by virtual-key code. */
private fun NativeClient.combo(modifier: Int, char: Char? = null, vk: Int = 0) {
    sendShortcutKey(modifier, 0, true)
    tap(vk, char?.code ?: 0)
    sendShortcutKey(modifier, 0, false)
}

// Windows virtual-key codes.
private const val VK_CONTROL = 0xA2  // left Ctrl
private const val VK_SHIFT = 0xA0    // left Shift
private const val VK_MENU = 0xA4     // left Alt
private const val VK_SPACE = 0x20
private const val VK_ESCAPE = 0x1B
private const val VK_OEM_PLUS = 0xBB
private const val VK_OEM_MINUS = 0xBD
private const val LATCH_TAP_MS = 300L

// Space: hold to pan in Photoshop, Krita, Clip Studio...
private val MODIFIERS = listOf(
    VK_CONTROL to R.string.key_ctrl, VK_SHIFT to R.string.key_shift, VK_MENU to R.string.key_alt, VK_SPACE to R.string.key_space,
)
