package com.displaymaster.client.ui

import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.displaymaster.client.ui.theme.Brand

/** Two overlapping screens: the PC display and the device extending it. */
@Composable
fun AppLogo(modifier: Modifier = Modifier, size: Dp = 44.dp) {
    val accent = MaterialTheme.colorScheme.primary
    Canvas(modifier.size(size)) {
        val w = this.size.width
        val stroke = w * 0.075f
        val brush = Brush.linearGradient(listOf(Brand.Accent, Brand.Connected), Offset.Zero, Offset(w, w))
        // Back: laptop screen.
        drawRoundRect(
            color = accent.copy(alpha = 0.35f),
            topLeft = Offset(w * 0.06f, w * 0.14f),
            size = Size(w * 0.62f, w * 0.44f),
            cornerRadius = CornerRadius(w * 0.08f),
            style = Stroke(stroke),
        )
        // Front: tablet, filled with the brand gradient.
        drawRoundRect(
            brush = brush,
            topLeft = Offset(w * 0.36f, w * 0.38f),
            size = Size(w * 0.58f, w * 0.48f),
            cornerRadius = CornerRadius(w * 0.09f),
        )
    }
}

/** Soft expanding rings behind an icon, used to hint "plug in / waiting". */
@Composable
fun PulsingIcon(icon: ImageVector, tint: Color, modifier: Modifier = Modifier, size: Dp = 56.dp) {
    val transition = rememberInfiniteTransition(label = "pulse")
    val t by transition.animateFloat(
        initialValue = 0f,
        targetValue = 1f,
        animationSpec = infiniteRepeatable(tween(1800, easing = LinearEasing), RepeatMode.Restart),
        label = "t",
    )
    Box(modifier.size(size * 1.6f), contentAlignment = Alignment.Center) {
        Canvas(Modifier.size(size * 1.6f)) {
            val base = size.toPx() / 2
            for (k in 0..1) {
                val p = (t + k * 0.5f) % 1f
                drawCircle(color = tint.copy(alpha = 0.22f * (1 - p)), radius = base * (1 + 0.6f * p))
            }
            drawCircle(color = tint.copy(alpha = 0.16f), radius = base)
        }
        Icon(icon, contentDescription = null, tint = tint, modifier = Modifier.size(size * 0.5f))
    }
}

@Composable
fun SectionCard(
    modifier: Modifier = Modifier,
    title: String? = null,
    icon: ImageVector? = null,
    content: @Composable ColumnScope.() -> Unit,
) {
    Surface(
        modifier = modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.large,
        color = MaterialTheme.colorScheme.surfaceContainer,
    ) {
        Column(Modifier.padding(20.dp), verticalArrangement = Arrangement.spacedBy(14.dp)) {
            if (title != null) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    if (icon != null) {
                        Icon(icon, null, tint = MaterialTheme.colorScheme.primary, modifier = Modifier.size(20.dp))
                        Box(Modifier.size(10.dp))
                    }
                    Text(title, style = MaterialTheme.typography.titleMedium)
                }
            }
            content()
        }
    }
}

/** Labelled single-choice segmented control. */
@Composable
fun <T> ChoiceRow(
    label: String,
    options: List<Pair<T, String>>,
    selected: T,
    onSelect: (T) -> Unit,
    icons: List<ImageVector>? = null,
) {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(label, style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.onSurfaceVariant)
        SingleChoiceSegmentedButtonRow(Modifier.fillMaxWidth()) {
            options.forEachIndexed { i, (value, text) ->
                SegmentedButton(
                    selected = value == selected,
                    onClick = { onSelect(value) },
                    shape = SegmentedButtonDefaults.itemShape(i, options.size),
                    icon = {
                        if (icons != null && value == selected) {
                            SegmentedButtonDefaults.Icon(active = true)
                        } else if (icons != null) {
                            Icon(icons[i], null, Modifier.size(18.dp))
                        }
                    },
                ) { Text(text, maxLines = 1) }
            }
        }
    }
}
