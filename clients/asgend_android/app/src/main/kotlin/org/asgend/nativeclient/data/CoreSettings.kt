package org.asgend.nativeclient.data

data class CoreSettings(
    val host: String = "",
    val port: Int = 8765,
    val token: String = "",
    val vpnTunnelAcknowledged: Boolean = false,
) {
    val configured: Boolean get() = host.isNotBlank() && port in 1..65535

    fun validationError(): String? {
        val trimmed = host.trim()
        if (trimmed.isEmpty()) return "Enter the host or VPN/LAN address of the ASGEND core."
        if (trimmed.any { it.isWhitespace() || it == '/' || it == '\\' }) return "Host must be a hostname or IP address, without a URL scheme or path."
        if (isDeviceLoopback(trimmed)) return "On Android, localhost points to the phone itself. Use the core's LAN/VPN address or the HTTPS Control Plane."
        if (port !in 1..65535) return "TCP port must be between 1 and 65535."
        if (!vpnTunnelAcknowledged) return "The local-core TCP protocol is unencrypted. Confirm that the connection uses a trusted VPN or SSH tunnel."
        return null
    }

    private fun isDeviceLoopback(host: String): Boolean {
        val normalized = host.lowercase().removePrefix("[").removeSuffix("]").trimEnd('.')
        return normalized == "localhost" || normalized.endsWith(".localhost") || normalized == "::1" ||
            normalized == "0:0:0:0:0:0:0:1" || normalized.matches(Regex("127(?:\\.[0-9]{1,3}){3}"))
    }
}

internal fun safeErrorText(message: String, secret: String): String {
    if (secret.isBlank()) return message.take(500)
    return message.replace(secret, "[redacted]").take(500)
}
