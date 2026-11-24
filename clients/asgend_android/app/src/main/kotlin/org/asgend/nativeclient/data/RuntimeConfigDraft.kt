package org.asgend.nativeclient.data

import org.json.JSONObject

/** Only non-secret, explicitly supported settings exposed to the Android form. */
data class RuntimeConfigDraft(
    val maxConnections: Int = 128,
    val requestTimeoutSeconds: Int = 30,
    val workers: Int = 2,
    val queueMaxPending: Int = 1000,
    val maxRetries: Int = 2,
    val priorityAgingSeconds: Int = 300,
    val sandboxEnabled: Boolean = true,
    val requireLandlock: Boolean = true,
    val allowSandboxNetwork: Boolean = false,
    val sandboxCpuSeconds: Int = 120,
    val sandboxMemoryMb: Int = 512,
    val sandboxFileSizeMb: Int = 64,
    val sandboxOpenFiles: Int = 64,
) {
    fun validationError(): String? = when {
        maxConnections !in 1..4096 -> "Max connections must be in 1..4096."
        requestTimeoutSeconds !in 1..600 -> "Request timeout must be in 1..600 seconds."
        workers !in 1..64 -> "Workers must be in 1..64."
        queueMaxPending !in 1..1_000_000 -> "Pending queue limit must be in 1..1000000."
        maxRetries !in 0..10 -> "Max retries must be in 0..10."
        priorityAgingSeconds !in 1..86400 -> "Priority aging must be in 1..86400 seconds."
        sandboxCpuSeconds !in 1..86400 -> "Sandbox CPU limit must be in 1..86400 seconds."
        sandboxMemoryMb !in 64..65536 -> "Sandbox memory must be in 64..65536 MiB."
        sandboxFileSizeMb !in 1..4096 -> "Sandbox file size must be in 1..4096 MiB."
        sandboxOpenFiles !in 16..4096 -> "Sandbox open-file limit must be in 16..4096."
        else -> null
    }

    /** Minimal payload for the core's non-destructive config_update_settings operation. */
    fun toSettingsJson(): JSONObject = JSONObject()
        .put("server", JSONObject()
            .put("max_connections", maxConnections)
            .put("request_timeout_seconds", requestTimeoutSeconds))
        .put("runtime", JSONObject()
            .put("workers", workers)
            .put("queue_max_pending", queueMaxPending)
            .put("max_retries", maxRetries)
            .put("priority_aging_seconds", priorityAgingSeconds))
        .put("sandbox", JSONObject()
            .put("enabled", sandboxEnabled)
            .put("require_landlock", requireLandlock)
            .put("allow_network", allowSandboxNetwork)
            .put("cpu_seconds", sandboxCpuSeconds)
            .put("memory_mb", sandboxMemoryMb)
            .put("file_size_mb", sandboxFileSizeMb)
            .put("open_files", sandboxOpenFiles))

    /** Update an in-memory private snapshot after the server confirms the safe partial update. */
    fun applyTo(main: JSONObject) {
        val server = main.optJSONObject("server") ?: JSONObject().also { main.put("server", it) }
        val runtime = main.optJSONObject("runtime") ?: JSONObject().also { main.put("runtime", it) }
        val sandbox = main.optJSONObject("sandbox") ?: JSONObject().also { main.put("sandbox", it) }
        server.put("max_connections", maxConnections)
        server.put("request_timeout_seconds", requestTimeoutSeconds)
        runtime.put("workers", workers)
        runtime.put("queue_max_pending", queueMaxPending)
        runtime.put("max_retries", maxRetries)
        runtime.put("priority_aging_seconds", priorityAgingSeconds)
        sandbox.put("enabled", sandboxEnabled)
        sandbox.put("require_landlock", requireLandlock)
        sandbox.put("allow_network", allowSandboxNetwork)
        sandbox.put("cpu_seconds", sandboxCpuSeconds)
        sandbox.put("memory_mb", sandboxMemoryMb)
        sandbox.put("file_size_mb", sandboxFileSizeMb)
        sandbox.put("open_files", sandboxOpenFiles)
    }

    companion object {
        fun fromMain(main: JSONObject): RuntimeConfigDraft {
            val server = main.optJSONObject("server") ?: JSONObject()
            val runtime = main.optJSONObject("runtime") ?: JSONObject()
            val sandbox = main.optJSONObject("sandbox") ?: JSONObject()
            return RuntimeConfigDraft(
                maxConnections = server.optInt("max_connections", 128),
                requestTimeoutSeconds = server.optInt("request_timeout_seconds", 30),
                workers = runtime.optInt("workers", 2),
                queueMaxPending = runtime.optInt("queue_max_pending", 1000),
                maxRetries = runtime.optInt("max_retries", 2),
                priorityAgingSeconds = runtime.optInt("priority_aging_seconds", 300),
                sandboxEnabled = sandbox.optBoolean("enabled", true),
                requireLandlock = sandbox.optBoolean("require_landlock", true),
                allowSandboxNetwork = sandbox.optBoolean("allow_network", false),
                sandboxCpuSeconds = sandbox.optInt("cpu_seconds", 120),
                sandboxMemoryMb = sandbox.optInt("memory_mb", 512),
                sandboxFileSizeMb = sandbox.optInt("file_size_mb", 64),
                sandboxOpenFiles = sandbox.optInt("open_files", 64),
            )
        }
    }
}
