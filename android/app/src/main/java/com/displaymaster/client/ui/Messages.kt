package com.displaymaster.client.ui

import androidx.compose.runtime.Composable
import androidx.compose.ui.res.stringResource
import com.displaymaster.client.R

/**
 * Connection messages come from the native client and from the PC in English; show the
 * known ones in the device's language (unknown ones as they are).
 */
@Composable
fun localizedMessage(message: String): String = when {
    message.isBlank() -> stringResource(R.string.connection_lost)
    message.startsWith("Can't reach ") ->
        stringResource(R.string.err_cant_reach, message.removePrefix("Can't reach ").substringBefore(" - "))
    message.startsWith("Can't resolve ") -> stringResource(R.string.err_cant_resolve, message.removePrefix("Can't resolve "))
    message.contains("update DisplayMaster on the PC") -> stringResource(R.string.err_pc_outdated)
    message == "The PC closed the connection" -> stringResource(R.string.err_pc_closed)
    message == "Secure connection to the PC failed" -> stringResource(R.string.err_secure_failed)
    message == "Data from the PC failed verification" -> stringResource(R.string.err_verification)
    message == "Corrupt data from the PC" -> stringResource(R.string.err_corrupt)
    message == "Unexpected reply from the PC" -> stringResource(R.string.err_unexpected)
    message == "This device can't decode the video format" -> stringResource(R.string.err_decode)
    message == "Pairing cancelled" -> stringResource(R.string.err_pairing_cancelled)
    message == "Not approved on the PC" -> stringResource(R.string.err_not_approved)
    message == "Disconnected from the PC" -> stringResource(R.string.err_kicked)
    message.startsWith("Update the DisplayMaster app") -> stringResource(R.string.err_app_outdated)
    message.startsWith("Protocol version mismatch") -> stringResource(R.string.err_version)
    message.startsWith("Could not start video") -> stringResource(R.string.err_no_video)
    else -> message
}
