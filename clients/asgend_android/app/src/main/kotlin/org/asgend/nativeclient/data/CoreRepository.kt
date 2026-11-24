package org.asgend.nativeclient.data

import android.util.Base64
import org.json.JSONArray
import org.json.JSONObject

data class CoreBinaryChunk(val bytes: ByteArray, val size: Long, val nextOffset: Long, val eof: Boolean)

/** Typed operations over the existing C core JSON-lines RPC contract. */
class CoreRepository(private val client: CoreRpcTransport) {
    suspend fun listTasks(status: String? = null, limit: Int = 100): List<JSONObject> {
        val request = JSONObject().put("op", "list_tasks").put("limit", limit.coerceIn(1, 200))
        if (!status.isNullOrBlank() && status != "all") request.put("status", status)
        return jsonArrayObjects(client.call(request).optJSONArray("tasks"))
    }

    suspend fun submitTask(text: String, priority: Int, idempotencyKey: String, sessionId: String? = null): JSONObject {
        require(text.isNotBlank()) { "Task description must not be empty." }
        require(text.toByteArray(Charsets.UTF_8).size <= 100_000) { "Task description is larger than the core API limit." }
        val request = JSONObject()
            .put("op", "submit")
            .put("task", text.trim())
            .put("priority", priority.coerceIn(0, 9))
            .put("idempotency_key", idempotencyKey)
        if (!sessionId.isNullOrBlank()) request.put("session_id", sessionId)
        return client.call(request)
    }

    suspend fun taskStatus(taskId: String): JSONObject = client.call(JSONObject().put("op", "status").put("task_id", taskId))

    suspend fun taskEvents(taskId: String, afterId: Long, limit: Int = 100): List<JSONObject> =
        jsonArrayObjects(client.call(JSONObject().put("op", "events").put("task_id", taskId).put("after_id", afterId).put("limit", limit.coerceIn(1, 500))).optJSONArray("events"))

    suspend fun answerTask(taskId: String, answer: String, optionId: String? = null): JSONObject {
        require(answer.isNotBlank()) { "An answer is required." }
        val request = JSONObject().put("op", "answer").put("task_id", taskId).put("answer", answer.trim())
        if (!optionId.isNullOrBlank()) request.put("option_id", optionId)
        return client.call(request)
    }

    suspend fun cancelTask(taskId: String): JSONObject = client.call(JSONObject().put("op", "cancel").put("task_id", taskId))
    suspend fun retryTask(taskId: String): JSONObject = client.call(JSONObject().put("op", "retry_task").put("task_id", taskId))

    suspend fun deadLetters(limit: Int = 50): List<JSONObject> =
        jsonArrayObjects(client.call(JSONObject().put("op", "dead_letters").put("limit", limit.coerceIn(1, 200))).optJSONArray("dead_letters"))

    suspend fun planWait(revision: Long, timeoutMs: Int = 15_000): JSONObject {
        require(revision >= 0L) { "Plan revision must not be negative." }
        require(timeoutMs in 0..30_000) { "Plan wait timeout is out of range." }
        return client.call(JSONObject().put("op", "plan_wait").put("revision", revision).put("timeout_ms", timeoutMs), timeoutMs = timeoutMs + 5_000)
    }

    suspend fun planList(scopeId: String, kind: String? = null): List<JSONObject> {
        val request = JSONObject().put("op", "plan_list").put("scope_id", scopeId.ifBlank { "global" })
        if (!kind.isNullOrBlank() && kind != "all") request.put("kind", kind)
        return jsonArrayObjects(client.call(request).optJSONArray("items"))
    }

    suspend fun planAdd(scopeId: String, kind: String, title: String, description: String): JSONObject =
        client.call(JSONObject().put("op", "plan_add").put("scope_id", scopeId.ifBlank { "global" }).put("kind", kind).put("title", title.trim()).put("description", description.trim()))

    suspend fun planUpdate(scopeId: String, itemId: Long, title: String? = null, description: String? = null, status: String? = null): JSONObject {
        val request = JSONObject().put("op", "plan_update").put("scope_id", scopeId.ifBlank { "global" }).put("item_id", itemId)
        if (title != null) request.put("title", title.trim())
        if (description != null) request.put("description", description.trim())
        if (status != null) request.put("status", status)
        return client.call(request)
    }

    suspend fun planDelete(scopeId: String, itemId: Long): JSONObject =
        client.call(JSONObject().put("op", "plan_delete").put("scope_id", scopeId.ifBlank { "global" }).put("item_id", itemId))

    suspend fun workspaceList(taskId: String, path: String): JSONObject =
        client.call(JSONObject().put("op", "workspace_list").put("task_id", taskId).put("path", path))

    suspend fun workspaceRead(taskId: String, path: String): JSONObject =
        client.call(JSONObject().put("op", "workspace_read").put("task_id", taskId).put("path", path), timeoutMs = 15_000)

    suspend fun workspaceReadChunk(taskId: String, path: String, offset: Long = 0L, limit: Int = 32 * 1024): JSONObject =
        client.call(JSONObject().put("op", "workspace_read_chunk").put("task_id", taskId).put("path", path).put("offset", offset).put("limit", limit.coerceIn(1, 65_536)), timeoutMs = 15_000)

    suspend fun artifactList(taskId: String): JSONObject {
        require(taskId.isNotBlank() && taskId.length <= 128) { "A valid task identifier is required." }
        return client.call(JSONObject().put("op", "artifact_list").put("task_id", taskId), timeoutMs = 30_000)
    }

    suspend fun workspaceReadBytes(taskId: String, path: String, offset: Long, limit: Int = 256 * 1024): CoreBinaryChunk {
        require(taskId.isNotBlank() && taskId.length <= 128) { "A valid task identifier is required." }
        require(isSafeRelativePath(path)) { "File path must be a normalized relative workspace path." }
        require(offset >= 0L) { "Offset must not be negative." }
        require(limit in 1..(1024 * 1024)) { "Binary chunk limit is out of range." }
        val reply = client.call(
            JSONObject().put("op", "workspace_read_bytes").put("task_id", taskId)
                .put("path", path).put("offset", offset).put("limit", limit),
            timeoutMs = 20_000,
        )
        require(reply.optString("path") == path && reply.optLong("offset", -1L) == offset) { "Core returned an inconsistent binary chunk." }
        val size = reply.optLong("size", -1L)
        val nextOffset = reply.optLong("next_offset", -1L)
        require(size >= 0L && nextOffset >= offset && nextOffset <= size) { "Core returned invalid binary chunk offsets." }
        val encoded = reply.optString("data_b64")
        val bytes = try { Base64.decode(encoded, Base64.DEFAULT) } catch (_: IllegalArgumentException) {
            throw IllegalStateException("Core returned invalid binary data encoding.")
        }
        require(nextOffset == offset + bytes.size && bytes.size <= limit) { "Core returned an invalid binary chunk length." }
        return CoreBinaryChunk(bytes, size, nextOffset, reply.optBoolean("eof", false))
    }

    private fun isSafeRelativePath(path: String): Boolean =
        path.isNotBlank() && path.toByteArray(Charsets.UTF_8).size < 1024 && !path.startsWith('/') && !path.startsWith('\\') &&
            !path.contains('\\') && path.none { it.code < 0x20 || it.code == 0x7f } &&
            path.split('/').all { it.isNotBlank() && it != "." && it != ".." }

    suspend fun lspDiagnostics(relativePath: String): String {
        require(isSafeRelativePath(relativePath) && relativePath.length <= 1024) {
            "Use a normalized relative path inside the configured project root."
        }
        return client.call(JSONObject().put("op", "lsp_diagnostics").put("file_path", relativePath), timeoutMs = 30_000)
            .optString("diagnostics").take(32_000)
    }

    suspend fun workspaceWrite(taskId: String, path: String, content: String): JSONObject {
        require(content.toByteArray(Charsets.UTF_8).size <= 1024 * 1024) { "Text files are limited to 1 MiB." }
        return client.call(JSONObject().put("op", "workspace_write").put("task_id", taskId).put("path", path).put("content", content), timeoutMs = 15_000)
    }

    suspend fun health(): JSONObject = client.call(JSONObject().put("op", "health"))
    suspend fun models(): JSONObject = client.call(JSONObject().put("op", "models"))
    suspend fun agents(): List<JSONObject> = jsonArrayObjects(client.call(JSONObject().put("op", "list_agents")).optJSONArray("agents"))
    suspend fun resources(): JSONObject = client.call(JSONObject().put("op", "resources"))
    suspend fun mcpStatus(): List<JSONObject> = jsonArrayObjects(client.call(JSONObject().put("op", "mcp_status")).optJSONArray("servers"))
    suspend fun modelAction(operation: String, modelId: String, kind: String): JSONObject {
        require(operation in setOf("model_enable", "model_disable", "model_start", "model_stop")) { "Unsupported model action." }
        require(kind == "llm" || kind == "decision") { "Model kind must be llm or decision." }
        return client.call(JSONObject().put("op", operation).put("model_id", modelId).put("kind", kind), timeoutMs = 180_000)
    }
    suspend fun configGet(): JSONObject = client.call(JSONObject().put("op", "config_get"))
    suspend fun configSave(config: JSONObject): JSONObject = client.call(JSONObject(config.toString()).put("op", "config_save"), timeoutMs = 15_000)
    suspend fun configUpdateSettings(settings: JSONObject): JSONObject = client.call(
        JSONObject().put("op", "config_update_settings").put("settings", JSONObject(settings.toString())),
        timeoutMs = 15_000,
    )

    companion object {
        fun parseQuestionFromEvent(event: JSONObject): JSONObject? {
            val payload = event.optJSONObject("payload") ?: return null
            val nested = payload.optJSONObject("question")
            if (nested != null) return nested
            return if (payload.optString("question").isNotBlank()) payload else null
        }

        fun parseQuestionFromStatus(status: JSONObject): JSONObject? = status.optJSONObject("result")?.optJSONObject("question")

        fun eventId(event: JSONObject): Long = event.optLong("event_id", 0L)

        fun questionOptions(question: JSONObject): List<JSONObject> = jsonArrayObjects(question.optJSONArray("options"))
    }
}
