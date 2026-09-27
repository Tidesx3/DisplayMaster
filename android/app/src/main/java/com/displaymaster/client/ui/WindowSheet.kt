package com.displaymaster.client.ui

import android.graphics.Bitmap
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.Input
import androidx.compose.material.icons.rounded.CheckCircle
import androidx.compose.material.icons.rounded.Info
import androidx.compose.material.icons.rounded.Keyboard
import androidx.compose.material.icons.rounded.WebAsset
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.displaymaster.client.MoveNotice
import com.displaymaster.client.PcWindow
import com.displaymaster.client.Proto
import com.displaymaster.client.R
import com.displaymaster.client.WindowSheet
import kotlinx.coroutines.delay

/** What the stream screen can do with the PC's windows (see ConnectionViewModel). */
class WindowActions(
    val pull: (id: Long) -> Unit,      // id 0: the window used last on the other screens
    val sendBack: (id: Long) -> Unit,
    val open: () -> Unit,
    val close: () -> Unit,
    val noticeShown: (MoveNotice) -> Unit,
)

/** Picker for the PC's windows: tap one to bring it here, or send one of this screen's back. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun WindowPicker(sheet: WindowSheet, actions: WindowActions) {
    ModalBottomSheet(
        onDismissRequest = actions.close,
        sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true),
    ) {
        Column(Modifier.padding(horizontal = 24.dp).padding(bottom = 16.dp)) {
            Text(stringResource(R.string.windows_title), style = MaterialTheme.typography.titleLarge)
            Spacer(Modifier.height(4.dp))
            Text(
                stringResource(R.string.windows_hint),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Spacer(Modifier.height(12.dp))
            if (sheet.loading) {
                Box(Modifier.fillMaxWidth().height(160.dp), contentAlignment = Alignment.Center) {
                    CircularProgressIndicator()
                }
            } else {
                val elsewhere = sheet.windows.filter { !it.here }
                val here = sheet.windows.filter { it.here }
                LazyColumn(Modifier.weight(1f, fill = false)) {
                    item { SectionLabel(stringResource(R.string.windows_elsewhere)) }
                    if (elsewhere.isEmpty()) {
                        item {
                            Text(
                                stringResource(R.string.windows_none),
                                style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                                modifier = Modifier.padding(horizontal = 8.dp, vertical = 12.dp),
                            )
                        }
                    }
                    items(elsewhere, key = { it.id }) { w ->
                        WindowRow(w, onClick = {
                            actions.pull(w.id)
                            actions.close()
                        }) {
                            Icon(
                                Icons.AutoMirrored.Rounded.Input,
                                contentDescription = stringResource(R.string.pull_window),
                                tint = MaterialTheme.colorScheme.primary,
                                modifier = Modifier.padding(horizontal = 12.dp),
                            )
                        }
                    }
                    if (here.isNotEmpty()) {
                        item { SectionLabel(stringResource(R.string.windows_here)) }
                        items(here, key = { it.id }) { w ->
                            WindowRow(w, onClick = { actions.sendBack(w.id) }) {
                                TextButton(onClick = { actions.sendBack(w.id) }) { Text(stringResource(R.string.send_back)) }
                            }
                        }
                    }
                    item {
                        Row(Modifier.padding(horizontal = 8.dp, vertical = 16.dp), verticalAlignment = Alignment.CenterVertically) {
                            Icon(
                                Icons.Rounded.Keyboard, null, Modifier.size(18.dp),
                                tint = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                            Spacer(Modifier.size(8.dp))
                            Text(
                                stringResource(R.string.windows_tip),
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun SectionLabel(text: String) = Text(
    text,
    style = MaterialTheme.typography.labelLarge,
    color = MaterialTheme.colorScheme.primary,
    modifier = Modifier.padding(start = 8.dp, top = 12.dp, bottom = 4.dp),
)

@Composable
private fun WindowRow(w: PcWindow, onClick: () -> Unit, trailing: @Composable () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(16.dp))
            .clickable(onClick = onClick)
            .padding(horizontal = 8.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        WindowIcon(w.icon)
        Spacer(Modifier.size(16.dp))
        Column(Modifier.weight(1f)) {
            Text(w.title, style = MaterialTheme.typography.bodyLarge, maxLines = 1, overflow = TextOverflow.Ellipsis)
            val details = listOfNotNull(w.app.ifBlank { null }, if (w.minimized) stringResource(R.string.window_minimized) else null)
            if (details.isNotEmpty()) {
                Text(
                    details.joinToString(" · "),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
        trailing()
    }
}

@Composable
private fun WindowIcon(icon: Bitmap?) {
    Box(
        Modifier.size(44.dp).background(MaterialTheme.colorScheme.surfaceContainerHighest, RoundedCornerShape(12.dp)),
        contentAlignment = Alignment.Center,
    ) {
        if (icon != null) {
            val image = remember(icon) { icon.asImageBitmap() }
            Image(image, contentDescription = null, modifier = Modifier.size(32.dp))
        } else {
            Icon(Icons.Rounded.WebAsset, null, tint = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

/** Short confirmation (or reason) after a window move, at the top of the stream. */
@Composable
fun MoveNoticePill(notice: MoveNotice?, onShown: (MoveNotice) -> Unit, modifier: Modifier = Modifier) {
    var last by remember { mutableStateOf(notice) }  // keeps the text during the exit animation
    LaunchedEffect(notice) {
        if (notice != null) {
            last = notice
            delay(2600)
            onShown(notice)
        }
    }
    AnimatedVisibility(
        visible = notice != null,
        enter = fadeIn() + slideInVertically { -it },
        exit = fadeOut() + slideOutVertically { -it },
        modifier = modifier,
    ) {
        val n = notice ?: last ?: return@AnimatedVisibility
        val ok = n.result.result == Proto.MOVE_MOVED
        Surface(
            color = MaterialTheme.colorScheme.inverseSurface,
            contentColor = MaterialTheme.colorScheme.inverseOnSurface,
            shape = CircleShape,
            shadowElevation = 6.dp,
        ) {
            Row(
                Modifier.padding(horizontal = 18.dp, vertical = 10.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                Icon(if (ok) Icons.Rounded.CheckCircle else Icons.Rounded.Info, null, Modifier.size(18.dp))
                Text(moveMessage(n), style = MaterialTheme.typography.bodyMedium, maxLines = 2)
            }
        }
    }
}

@Composable
private fun moveMessage(n: MoveNotice): String {
    val r = n.result
    val title = r.title.let { if (it.length > 48) it.take(47).trimEnd() + "…" else it }
    return when (r.result) {
        Proto.MOVE_MOVED ->
            if (r.target == Proto.TARGET_BACK) stringResource(R.string.move_back, title) else stringResource(R.string.move_here, title)
        Proto.MOVE_NOTHING -> stringResource(R.string.move_nothing)
        Proto.MOVE_GONE -> stringResource(R.string.move_gone)
        Proto.MOVE_DENIED -> stringResource(R.string.move_denied)
        Proto.MOVE_NOT_RESPONDING -> stringResource(R.string.move_not_responding, title)
        Proto.MOVE_NOT_EXTENDED -> stringResource(R.string.move_not_extended)
        else -> stringResource(R.string.move_failed)
    }
}
