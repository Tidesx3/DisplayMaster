package com.displaymaster.client.ui.theme

import android.os.Build
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.ColorScheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/**
 * DisplayMaster design tokens. The Windows app (windows/ui) uses the same accent
 * (#3B6BFF) and status colors so both halves feel like one product.
 */
object Brand {
    val Accent = Color(0xFF3B6BFF)
    val AccentLight = Color(0xFFB4C5FF)
    val Connected = Color(0xFF00BFA5)
    val Pen = Color(0xFFFF7A59)
    val Warning = Color(0xFFFFB020)

    val Ink = Color(0xFF0B0D12)
    val Night = Color(0xFF11141B)
    val NightHigh = Color(0xFF1A1F2A)
    val Paper = Color(0xFFF7F8FC)
}

/** Status colors that aren't part of the Material scheme. */
@Immutable
data class StatusColors(val connected: Color, val pen: Color, val warning: Color)

val LocalStatusColors = staticCompositionLocalOf { StatusColors(Brand.Connected, Brand.Pen, Brand.Warning) }

private val DarkBrand = darkColorScheme(
    primary = Brand.AccentLight,
    onPrimary = Color(0xFF00257A),
    primaryContainer = Color(0xFF1F45C2),
    onPrimaryContainer = Color(0xFFDCE1FF),
    secondary = Color(0xFF5FE0C9),
    onSecondary = Color(0xFF00382F),
    tertiary = Color(0xFFFFB59F),
    background = Brand.Ink,
    onBackground = Color(0xFFE3E5EE),
    surface = Brand.Ink,
    onSurface = Color(0xFFE3E5EE),
    surfaceVariant = Brand.NightHigh,
    onSurfaceVariant = Color(0xFFB9BECC),
    surfaceContainerLowest = Color(0xFF07080C),
    surfaceContainerLow = Color(0xFF0F1218),
    surfaceContainer = Brand.Night,
    surfaceContainerHigh = Brand.NightHigh,
    surfaceContainerHighest = Color(0xFF232937),
    outline = Color(0xFF454B5A),
    outlineVariant = Color(0xFF2C3240),
)

private val LightBrand = lightColorScheme(
    primary = Brand.Accent,
    onPrimary = Color.White,
    primaryContainer = Color(0xFFDCE1FF),
    onPrimaryContainer = Color(0xFF00174B),
    secondary = Color(0xFF006B5C),
    tertiary = Color(0xFFA63E1E),
    background = Brand.Paper,
    surface = Brand.Paper,
    surfaceContainerLowest = Color.White,
    surfaceContainerLow = Color(0xFFF1F3FA),
    surfaceContainer = Color(0xFFEBEEF6),
    surfaceContainerHigh = Color(0xFFE5E8F1),
    surfaceContainerHighest = Color(0xFFDFE2EC),
    outlineVariant = Color(0xFFD3D7E3),
)

private val AppTypography = Typography().run {
    copy(
        displaySmall = displaySmall.copy(fontWeight = FontWeight.SemiBold, letterSpacing = (-0.5).sp),
        headlineMedium = headlineMedium.copy(fontWeight = FontWeight.SemiBold, letterSpacing = (-0.25).sp),
        headlineSmall = headlineSmall.copy(fontWeight = FontWeight.SemiBold),
        titleLarge = titleLarge.copy(fontWeight = FontWeight.SemiBold),
        titleMedium = titleMedium.copy(fontWeight = FontWeight.SemiBold),
        labelLarge = labelLarge.copy(fontWeight = FontWeight.SemiBold),
    )
}

private val AppShapes = Shapes(
    extraSmall = RoundedCornerShape(8.dp),
    small = RoundedCornerShape(12.dp),
    medium = RoundedCornerShape(16.dp),
    large = RoundedCornerShape(24.dp),
    extraLarge = RoundedCornerShape(32.dp),
)

/** Monospace-ish numerals for live stats. */
val StatsTextStyle = TextStyle(fontSize = 12.sp, fontWeight = FontWeight.Medium, letterSpacing = 0.2.sp)

@Composable
fun DisplayMasterTheme(
    darkTheme: Boolean = isSystemInDarkTheme(),
    dynamicColor: Boolean = true,
    content: @Composable () -> Unit,
) {
    val context = LocalContext.current
    val scheme: ColorScheme = when {
        dynamicColor && Build.VERSION.SDK_INT >= Build.VERSION_CODES.S ->
            if (darkTheme) dynamicDarkColorScheme(context).withBrandSurfaces() else dynamicLightColorScheme(context)
        darkTheme -> DarkBrand
        else -> LightBrand
    }
    MaterialTheme(colorScheme = scheme, typography = AppTypography, shapes = AppShapes) {
        androidx.compose.runtime.CompositionLocalProvider(
            LocalStatusColors provides StatusColors(Brand.Connected, Brand.Pen, Brand.Warning),
            content = content,
        )
    }
}

/** Dynamic dark schemes can be muddy; keep our deep ink background for contrast with video. */
private fun ColorScheme.withBrandSurfaces() = copy(background = Brand.Ink, surface = Brand.Ink)
