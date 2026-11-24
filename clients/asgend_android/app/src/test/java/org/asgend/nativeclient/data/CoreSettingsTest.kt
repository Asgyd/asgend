package org.asgend.nativeclient.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class CoreSettingsTest {
    @Test
    fun rejectsUrlSchemesAndPathsInHost() {
        assertEquals(
            "Host must be a hostname or IP address, without a URL scheme or path.",
            CoreSettings(host = "https://core.example", vpnTunnelAcknowledged = true).validationError(),
        )
        assertEquals(
            "Host must be a hostname or IP address, without a URL scheme or path.",
            CoreSettings(host = "core.example/api", vpnTunnelAcknowledged = true).validationError(),
        )
    }

    @Test
    fun requiresExplicitTunnelAcknowledgement() {
        assertEquals(
            "The local-core TCP protocol is unencrypted. Confirm that the connection uses a trusted VPN or SSH tunnel.",
            CoreSettings(host = "10.10.0.4", port = 8765).validationError(),
        )
    }

    @Test
    fun rejectsDeviceLoopbackAddresses() {
        val message = "On Android, localhost points to the phone itself. Use the core's LAN/VPN address or the HTTPS Control Plane."
        assertEquals(message, CoreSettings(host = "localhost", vpnTunnelAcknowledged = true).validationError())
        assertEquals(message, CoreSettings(host = "127.0.0.1", vpnTunnelAcknowledged = true).validationError())
        assertEquals(message, CoreSettings(host = "[::1]", vpnTunnelAcknowledged = true).validationError())
    }

    @Test
    fun acceptsValidVpnAddressAndPort() {
        assertNull(CoreSettings(host = "10.10.0.4", port = 8765, vpnTunnelAcknowledged = true).validationError())
    }

    @Test
    fun redactsSecretFromErrorText() {
        assertEquals("request [redacted] failed", safeErrorText("request super-secret failed", "super-secret"))
    }
}
