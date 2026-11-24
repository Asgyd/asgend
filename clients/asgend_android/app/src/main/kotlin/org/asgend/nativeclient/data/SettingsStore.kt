package org.asgend.nativeclient.data

import android.content.Context
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.security.MessageDigest

internal class SettingsStore(context: Context) {
    private val prefs = context.getSharedPreferences("asgend_settings", Context.MODE_PRIVATE)
    private val secureTokens = SecureTokenStore(context.applicationContext)
    private val operationPrefs = context.getSharedPreferences("asgend_remote_operations", Context.MODE_PRIVATE)
    private val operationLedger = RemoteOperationLedger(object : RemoteOperationPersistence {
        override fun read(): List<PendingRemoteOperation> {
            val encoded = operationPrefs.getString("ledger_v1", null) ?: return emptyList()
            val array = try { JSONArray(encoded) } catch (_: Exception) {
                error("The pending remote-operation ledger is unreadable; refusing to issue a new mutation key.")
            }
            check(array.length() <= RemoteOperationLedger.MAX_RECORDS) {
                "The pending remote-operation ledger exceeds its supported size; refusing new mutations."
            }
            return buildList {
                for (index in 0 until array.length()) {
                    val item = array.optJSONObject(index)
                        ?: error("A pending remote-operation ledger entry is corrupt; refusing new mutations.")
                    val slot = item.optString("slot")
                    val digest = item.optString("digest")
                    val key = item.optString("key")
                    val created = item.optLong("created_at", -1L)
                    check(slot.isNotBlank() && slot.length <= RemoteOperationLedger.MAX_SLOT_CHARS &&
                        digest.matches(Regex("[0-9a-f]{64}")) && key.matches(Regex("[A-Za-z0-9._:-]{8,128}")) && created >= 0L) {
                        "A pending remote-operation ledger entry is invalid; refusing new mutations."
                    }
                    add(PendingRemoteOperation(slot, digest, key, created))
                }
            }
        }
        override fun write(records: List<PendingRemoteOperation>): Boolean {
            if (records.size > RemoteOperationLedger.MAX_RECORDS) return false
            val array = JSONArray()
            records.forEach { record ->
                array.put(JSONObject().put("slot", record.slot).put("digest", record.digest)
                    .put("key", record.key).put("created_at", record.createdAtMillis))
            }
            return operationPrefs.edit().putString("ledger_v1", array.toString()).commit()
        }
    })

    suspend fun load(): CoreSettings = withContext(Dispatchers.IO) {
        val token = runCatching { secureTokens.load() }.getOrElse {
            // Never fall back to a plaintext copy if Android Keystore cannot decrypt.
            ""
        }
        CoreSettings(
            host = prefs.getString("core_host", "") ?: "",
            port = prefs.getInt("core_port", 8765),
            token = token,
            vpnTunnelAcknowledged = prefs.getBoolean("tcp_vpn_ack", false),
        )
    }

    suspend fun save(settings: CoreSettings) = withContext(Dispatchers.IO) {
        secureTokens.save(settings.token)
        check(
            prefs.edit()
                .putString("core_host", settings.host.trim())
                .putInt("core_port", settings.port)
                .putBoolean("tcp_vpn_ack", settings.vpnTunnelAcknowledged)
                .commit(),
        ) { "Could not persist connection settings." }
    }

    suspend fun clearToken() = withContext(Dispatchers.IO) { secureTokens.clear() }

    suspend fun loadControlPlaneSettings(): ControlPlaneSettings = withContext(Dispatchers.IO) {
        val token = runCatching { secureTokens.loadRemote() }.getOrElse { "" }
        ControlPlaneSettings(
            endpoint = prefs.getString("cp_endpoint", "") ?: "",
            workspaceId = prefs.getString("cp_workspace_id", "") ?: "",
            userId = prefs.getString("cp_user_id", "") ?: "",
            role = prefs.getString("cp_role", "unknown") ?: "unknown",
            deviceName = prefs.getString("cp_device_name", "ASGEND Android") ?: "ASGEND Android",
            token = token,
        )
    }

    suspend fun saveControlPlaneSettings(settings: ControlPlaneSettings) = withContext(Dispatchers.IO) {
        require(settings.token.matches(Regex("ucp_[A-Za-z0-9_-]{32,256}"))) { "A valid Control Plane user token is required." }
        require(settings.endpoint.startsWith("https://")) { "The Control Plane endpoint must use HTTPS." }
        secureTokens.saveRemote(settings.token)
        val stored = prefs.edit()
            .putString("cp_endpoint", settings.endpoint)
            .putString("cp_workspace_id", settings.workspaceId)
            .putString("cp_user_id", settings.userId)
            .putString("cp_role", settings.role)
            .putString("cp_device_name", settings.deviceName)
            .commit()
        if (!stored) {
            runCatching { secureTokens.clearRemote() }
            error("Could not persist Control Plane settings.")
        }
    }

    suspend fun saveActiveControlPlaneWorkspace(workspaceId: String, role: String) = withContext(Dispatchers.IO) {
        require(workspaceId.isNotBlank() && workspaceId.length <= 256) { "A valid workspace is required." }
        require(role in setOf("owner", "operator", "viewer")) { "A valid workspace role is required." }
        check(prefs.edit().putString("cp_workspace_id", workspaceId).putString("cp_role", role).commit()) {
            "Could not persist the selected Control Plane workspace."
        }
    }


    /** The key is durably committed before the caller sends a mutating network request. */
    suspend fun pendingRemoteOperation(slot: String, digest: String): PendingRemoteOperation = withContext(Dispatchers.IO) {
        operationLedger.getOrCreate(slot, digest)
    }

    /** Remove only after a definite accepted response; ambiguous failures retain the key. */
    suspend fun confirmRemoteOperation(slot: String, key: String) = withContext(Dispatchers.IO) {
        check(operationLedger.remove(slot, key)) { "Could not clear the confirmed remote operation." }
    }

    /** User-reviewed reconciliation only; never used automatically when the retry TTL expires. */
    suspend fun discardExpiredRemoteOperation(slot: String, digest: String, key: String) = withContext(Dispatchers.IO) {
        check(operationLedger.discardExpired(slot, digest, key)) { "Could not clear the reviewed expired operation." }
    }

    suspend fun loadRemoteUiState(userId: String, workspaceId: String): SavedRemoteUiState? = withContext(Dispatchers.IO) {
        val key = remoteUiKey(userId, workspaceId)
        val encoded = prefs.getString(key, null) ?: return@withContext null
        try {
            val json = JSONObject(encoded)
            val eventsJson = json.optJSONArray("events") ?: JSONArray()
            val events = buildList {
                for (index in maxOf(0, eventsJson.length() - MAX_SAVED_EVENTS) until eventsJson.length()) {
                    val item = eventsJson.optJSONObject(index) ?: continue
                    val id = item.optLong("event_id", 0L)
                    val type = item.optString("type")
                    if (id > 0 && type in SAFE_PERSISTED_EVENT_TYPES) {
                        add(ControlPlaneEvent(id, type, item.optString("occurred_at").take(64), item.optString("summary").take(256)))
                    }
                }
            }
            SavedRemoteUiState(
                selectedTaskId = json.optString("selected_task_id").takeIf { it.isNotBlank() },
                taskFilter = json.optString("task_filter", "all").takeIf { it in REMOTE_TASK_FILTERS } ?: "all",
                eventCursor = json.optLong("event_cursor", 0L).coerceAtLeast(0L),
                events = events,
                revision = json.optLong("revision", 0L).coerceAtLeast(0L),
            )
        } catch (_: Exception) { null }
    }

    suspend fun saveRemoteUiState(userId: String, workspaceId: String, state: SavedRemoteUiState) = withContext(Dispatchers.IO) {
        if (userId.isBlank() || workspaceId.isBlank()) return@withContext
        val events = JSONArray()
        state.events.takeLast(MAX_SAVED_EVENTS).forEach { event ->
            if (event.eventId > 0 && event.type in SAFE_PERSISTED_EVENT_TYPES) {
                events.put(JSONObject().put("event_id", event.eventId).put("type", event.type)
                    .put("occurred_at", event.occurredAt.take(64)).put("summary", event.safeSummary.take(256)))
            }
        }
        val key = remoteUiKey(userId, workspaceId)
        val currentRevision = prefs.getString(key, null)?.let { current ->
            runCatching { JSONObject(current).optLong("revision", 0L).coerceAtLeast(0L) }.getOrDefault(0L)
        } ?: 0L
        // Async saves can complete out of order; never let an older snapshot replace newer state.
        if (currentRevision > state.revision) return@withContext
        val json = JSONObject()
            .put("selected_task_id", state.selectedTaskId.orEmpty().take(128))
            .put("task_filter", state.taskFilter.takeIf { it in REMOTE_TASK_FILTERS } ?: "all")
            .put("event_cursor", state.eventCursor.coerceAtLeast(0L))
            .put("revision", state.revision.coerceAtLeast(0L))
            .put("events", events)
        check(prefs.edit().putString(key, json.toString()).commit()) {
            "Could not save remote workspace recovery state."
        }
    }

    private fun remoteUiKey(userId: String, workspaceId: String): String {
        val digest = MessageDigest.getInstance("SHA-256")
            .digest("$userId|$workspaceId".toByteArray(Charsets.UTF_8))
            .joinToString("") { "%02x".format(it.toInt() and 0xff) }.take(24)
        return "remote_ui_v1_$digest"
    }

    companion object {
        private const val MAX_SAVED_EVENTS = 100
        private val REMOTE_TASK_FILTERS = setOf("all", "pending_delivery", "delivered", "queued", "running", "waiting_input", "completed", "completed_without_model", "failed", "cancelled", "delivery_failed")
        private val SAFE_PERSISTED_EVENT_TYPES = setOf("state_changed", "question", "answer_submitted", "core_event", "artifact_available", "command_status", "delivery_failed")
    }

    /** Remove bearer credential and account binding; retain endpoint/device name for re-pair. */
    suspend fun clearControlPlaneSettings() = withContext(Dispatchers.IO) {
        var clearFailure: Throwable? = null
        try { secureTokens.clearRemote() } catch (failure: Throwable) { clearFailure = failure }
        val stored = prefs.edit().remove("cp_workspace_id").remove("cp_user_id").remove("cp_role").commit()
        if (!stored) error("Could not clear Control Plane account metadata.")
        clearFailure?.let { throw it }
    }
}


data class SavedRemoteUiState(
    val selectedTaskId: String? = null,
    val taskFilter: String = "all",
    val eventCursor: Long = 0L,
    val events: List<ControlPlaneEvent> = emptyList(),
    val revision: Long = 0L,
)
