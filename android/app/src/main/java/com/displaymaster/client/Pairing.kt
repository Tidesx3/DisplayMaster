package com.displaymaster.client

import android.content.Context
import java.io.File

/**
 * Encrypted Wi-Fi connections (protocol/include/dm/noise.h): this install's long-term key
 * and the PCs the user paired with (by the PC's public key). The key lives in
 * noBackupFilesDir so a backup restored onto another device can't impersonate this one.
 */
class Pairing(context: Context) {
    private val keyFile = File(context.noBackupFilesDir, "identity.key")
    private val prefs = context.getSharedPreferences("pairing", Context.MODE_PRIVATE)

    val identity: ByteArray by lazy {
        keyFile.takeIf { it.isFile }?.readBytes()?.takeIf { it.size == KEY_SIZE }
            ?: NativeClient.nativeGenerateIdentity().also { keyFile.writeBytes(it) }
    }

    fun isKnown(pcKey: String): Boolean = pcKey in prefs.getStringSet(PCS, emptySet())!!

    fun remember(pcKey: String) {
        val pcs = prefs.getStringSet(PCS, emptySet())!! + pcKey
        prefs.edit().putStringSet(PCS, pcs).apply()
    }

    private companion object {
        const val KEY_SIZE = 32
        const val PCS = "pcs"
    }
}
