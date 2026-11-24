package org.asgend.nativeclient.data

/** Non-secret Control Plane account metadata plus an in-memory Keystore-decoded token. */
data class ControlPlaneSettings(
    val endpoint: String = "",
    val workspaceId: String = "",
    val userId: String = "",
    val role: String = "unknown",
    val deviceName: String = "ASGEND Android",
    val token: String = "",
) {
    val paired: Boolean get() = token.matches(Regex("ucp_[A-Za-z0-9_-]{32,256}")) && endpoint.isNotBlank() && workspaceId.isNotBlank()
    override fun toString(): String = "ControlPlaneSettings(endpoint=$endpoint, workspaceId=$workspaceId, userId=$userId, role=$role, deviceName=$deviceName, token=${if (token.isBlank()) "empty" else "[REDACTED]"})"
}
