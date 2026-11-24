package org.asgend.nativeclient.data

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URI
import java.net.URLEncoder
import java.nio.charset.StandardCharsets
import java.util.Locale
import java.util.UUID
import javax.net.ssl.HttpsURLConnection

/** A deliberately separate HTTPS client. It must never send local core TCP payloads. */
data class ControlPlaneRequest(
    val method: String,
    val uri: URI,
    val headers: Map<String, String>,
    val body: ByteArray?,
    val timeoutMs: Int,
)

data class ControlPlaneResponse(
    val statusCode: Int,
    val headers: Map<String, String>,
    val body: ByteArray,
)

fun interface ControlPlaneTransport {
    fun execute(request: ControlPlaneRequest): ControlPlaneResponse
}

/** Uses platform TLS validation, disables redirects, and caps JSON response bodies. */
class UrlConnectionControlPlaneTransport : ControlPlaneTransport {
    override fun execute(request: ControlPlaneRequest): ControlPlaneResponse {
        require(request.uri.scheme.equals("https", ignoreCase = true)) { "Control Plane requires HTTPS." }
        val connection = request.uri.toURL().openConnection() as? HttpsURLConnection
            ?: throw IOException("Control Plane requires HTTPS.")
        connection.instanceFollowRedirects = false
        connection.connectTimeout = request.timeoutMs.coerceIn(1_000, 60_000)
        connection.readTimeout = request.timeoutMs.coerceIn(1_000, 60_000)
        connection.requestMethod = request.method
        connection.useCaches = false
        connection.doInput = true
        request.headers.forEach { (key, value) -> connection.setRequestProperty(key, value) }
        try {
            request.body?.let { bytes ->
                connection.doOutput = true
                connection.setFixedLengthStreamingMode(bytes.size)
                connection.outputStream.use { it.write(bytes) }
            }
            val status = connection.responseCode
            val stream = if (status in 200..299) connection.inputStream else connection.errorStream
            val bytes = stream?.use { it.readBounded(MAX_JSON_RESPONSE_BYTES) } ?: ByteArray(0)
            val headers = mutableMapOf<String, String>()
            connection.headerFields.forEach { (key, values) ->
                if (key != null && !values.isNullOrEmpty()) headers[key.lowercase(Locale.ROOT)] = values.first()
            }
            return ControlPlaneResponse(status, headers, bytes)
        } finally {
            connection.disconnect()
        }
    }

    private fun java.io.InputStream.readBounded(maximum: Int): ByteArray {
        val output = ByteArrayOutputStream(minOf(maximum, 8 * 1024))
        val buffer = ByteArray(8 * 1024)
        var total = 0
        while (true) {
            val read = read(buffer)
            if (read < 0) break
            total += read
            if (total > maximum) throw IOException("Control Plane response exceeds the size limit.")
            output.write(buffer, 0, read)
        }
        return output.toByteArray()
    }

    companion object { const val MAX_JSON_RESPONSE_BYTES = 2 * 1024 * 1024 }
}

data class ControlPlanePairingResult(
    val token: String,
    val userId: String,
    val workspaceId: String,
    val role: String,
) {
    override fun toString(): String = "ControlPlanePairingResult(userId=$userId, workspaceId=$workspaceId, role=$role, token=[REDACTED])"
}

data class ControlPlaneWorkspace(val workspaceId: String, val name: String, val role: String)
data class ControlPlaneIdentity(
    val userId: String,
    val displayName: String,
    val deviceName: String,
    val workspaces: List<ControlPlaneWorkspace>,
)
data class ControlPlaneNode(
    val nodeId: String,
    val name: String,
    val status: String,
    val coreVersion: String,
    val protocolVersion: Int,
)

data class ControlPlaneTask(
    val taskId: String,
    val workspaceId: String,
    val nodeId: String,
    val title: String,
    val state: String,
    val lastEventId: Long,
    val stale: Boolean,
    val question: ControlPlaneQuestion?,
    val resultSummary: String,
    val failed: Boolean,
)

data class ControlPlaneQuestion(val text: String, val options: List<String>)
data class ControlPlaneTaskPage(val tasks: List<ControlPlaneTask>, val nextCursor: String?)
data class ControlPlaneEvent(val eventId: Long, val type: String, val occurredAt: String, val safeSummary: String)
data class ControlPlaneEventPage(val events: List<ControlPlaneEvent>, val lastEventId: Long, val hasMore: Boolean)

class ControlPlaneException(
    val statusCode: Int?,
    val errorCode: String,
    override val message: String,
) : IOException(message) {
    override fun toString(): String = "ControlPlaneException(status=$statusCode, code=$errorCode)"
}

class ControlPlaneClient private constructor(
    val endpoint: URI,
    private val token: String?,
    private val transport: ControlPlaneTransport,
    private val requestTimeoutMs: Int,
    private val artifactDownloadTransport: ControlPlaneArtifactDownloadTransport,
) {
    val hasToken: Boolean get() = token?.matches(USER_TOKEN_PATTERN) == true

    suspend fun health(): Boolean {
        val response = request("GET", "/v1/health", authenticated = false)
        return response.optString("status") == "ok"
    }

    suspend fun redeemPairing(code: String, deviceName: String): ControlPlanePairingResult {
        require(!hasToken) { "Pairing must use an unauthenticated client." }
        val normalizedCode = code.trim().uppercase(Locale.ROOT)
        val normalizedName = deviceName.trim().ifBlank { "ASGEND Android" }.take(100)
        require(normalizedCode.length in 8..64 && normalizedCode.matches(Regex("[0-9A-Z][0-9A-Z -]{6,62}[0-9A-Z]"))) {
            "Enter the one-time pairing code shown by the Control Plane."
        }
        require(normalizedName.isNotBlank() && normalizedName.none { it.code < 0x20 || it.code == 0x7f }) { "Device name must be a single line." }
        val json = request(
            "POST", "/v1/pairing/redeem", authenticated = false,
            body = JSONObject().put("code", normalizedCode).put("device_name", normalizedName),
        )
        val result = ControlPlanePairingResult(
            token = json.optString("token"),
            userId = json.optString("user_id"),
            workspaceId = json.optString("workspace_id"),
            role = json.optString("role", "unknown"),
        )
        if (!result.token.matches(USER_TOKEN_PATTERN) || result.userId.isBlank() || result.workspaceId.isBlank() || result.role !in ALLOWED_ROLES) {
            throw ControlPlaneException(null, "bad_response", "Pairing response is incomplete or incompatible.")
        }
        return result
    }

    suspend fun me(): ControlPlaneIdentity {
        val json = request("GET", "/v1/me")
        val user = json.optJSONObject("user")
        val workspaces = mutableListOf<ControlPlaneWorkspace>()
        json.optJSONArray("workspaces").objects().forEach { item ->
            val id = item.optString("workspace_id")
            val role = item.optString("role", "unknown")
            if (id.isNotBlank() && role in ALLOWED_ROLES) {
                workspaces += ControlPlaneWorkspace(id, item.optString("name", id), role)
            }
        }
        return ControlPlaneIdentity(
            userId = user?.optString("user_id", json.optString("user_id")).orEmpty().ifBlank { json.optString("user_id") },
            displayName = user?.optString("display_name", user.optString("name")).orEmpty(),
            deviceName = json.optString("device_name"),
            workspaces = workspaces,
        ).also {
            if (it.userId.isBlank()) throw ControlPlaneException(null, "bad_response", "Control Plane identity response is incomplete.")
        }
    }

    suspend fun listNodes(workspaceId: String): List<ControlPlaneNode> {
        val json = request("GET", "/v1/workspaces/${segment(workspaceId)}/nodes")
        val rows = json.optJSONArray("nodes") ?: json.optJSONArray("items")
        return rows.objects().mapNotNull { item ->
            val id = item.optString("node_id")
            if (id.isBlank()) null else ControlPlaneNode(
                nodeId = id,
                name = item.optString("name", id),
                status = item.optString("status", "offline"),
                coreVersion = item.optString("core_version"),
                protocolVersion = item.optInt("protocol_version", 1),
            )
        }
    }

    suspend fun listTasks(
        workspaceId: String,
        state: String? = null,
        nodeId: String? = null,
        limit: Int = 50,
        cursor: String? = null,
    ): ControlPlaneTaskPage {
        require(limit in 1..200) { "Task page limit must be between 1 and 200." }
        require(state == null || state in TASK_STATES) { "Unsupported remote task state filter." }
        require(nodeId == null || nodeId.isBlank().not()) { "Node filter cannot be blank." }
        require(cursor == null || (cursor.length <= 2048 && cursor.none { it.code < 0x20 || it.code == 0x7f })) { "Task cursor is invalid." }
        val query = linkedMapOf("limit" to limit.toString())
        if (!state.isNullOrBlank()) query["state"] = state
        if (!nodeId.isNullOrBlank()) query["node_id"] = nodeId
        if (!cursor.isNullOrBlank()) query["cursor"] = cursor
        val json = request("GET", "/v1/workspaces/${segment(workspaceId)}/tasks", query = query)
        val tasks = json.optJSONArray("tasks").objects().map { parseTask(it) }
        val nextValue = json.opt("next_cursor")
        val next = if (nextValue == null || nextValue == JSONObject.NULL) null else nextValue.toString().takeIf { it.isNotBlank() }
        return ControlPlaneTaskPage(tasks, next)
    }

    suspend fun createTask(
        workspaceId: String,
        nodeId: String,
        task: String,
        idempotencyKey: String,
        agent: String? = null,
        sessionId: String? = null,
        priority: Int = 5,
        ttlSeconds: Int? = null,
    ): ControlPlaneTask {
        require(workspaceId.isNotBlank()) { "A workspace is required." }
        require(nodeId.isNotBlank()) { "An execution node is required." }
        require(task.isNotBlank() && task.length <= 262_144) { "Task text must contain 1..262144 characters." }
        require(priority in 0..9) { "Priority must be between 0 and 9." }
        requireIdempotencyKey(idempotencyKey)
        if (ttlSeconds != null) require(ttlSeconds in 10..86_400) { "Delivery TTL must be between 10 seconds and 24 hours." }
        val body = JSONObject().put("node_id", nodeId).put("task", task).put("idempotency_key", idempotencyKey).put("priority", priority)
        if (agent != null) body.put("agent", agent)
        if (sessionId != null) body.put("session_id", sessionId)
        if (ttlSeconds != null) body.put("ttl_seconds", ttlSeconds)
        return parseTask(request("POST", "/v1/workspaces/${segment(workspaceId)}/tasks", body = body))
    }

    suspend fun getTask(taskId: String): ControlPlaneTask = parseTask(request("GET", "/v1/tasks/${segment(taskId)}"))

    suspend fun taskEvents(
        taskId: String,
        afterEventId: Long,
        limit: Int = 100,
        waitMs: Int = 25_000,
    ): ControlPlaneEventPage {
        require(afterEventId >= 0) { "Event cursor cannot be negative." }
        require(limit in 1..500) { "Event page limit must be between 1 and 500." }
        require(waitMs in 0..30_000) { "Event long-poll wait must be between 0 and 30000 ms." }
        val json = request(
            "GET", "/v1/tasks/${segment(taskId)}/events",
            query = mapOf("after_event_id" to afterEventId.toString(), "limit" to limit.toString(), "wait_ms" to waitMs.toString()),
            timeoutMs = (waitMs + 20_000).coerceIn(1_000, 60_000),
        )
        val events = json.optJSONArray("events").objects().mapNotNull { item ->
            val eventId = item.optLong("event_id", -1L)
            val type = item.optString("type")
            if (eventId <= 0L || type.isBlank()) null else ControlPlaneEvent(
                eventId = eventId,
                type = type,
                occurredAt = item.optString("occurred_at"),
                safeSummary = safeEventSummary(type, item.optJSONObject("payload") ?: JSONObject()),
            )
        }.sortedBy { it.eventId }
        val last = json.optLong("last_event_id", afterEventId)
        return ControlPlaneEventPage(events, last, json.optBoolean("has_more", false))
    }

    suspend fun answerTask(taskId: String, answer: String, idempotencyKey: String): JSONObject {
        require(answer.isNotBlank() && answer.length <= 65_536) { "Answer must contain 1..65536 characters." }
        requireIdempotencyKey(idempotencyKey)
        return request("POST", "/v1/tasks/${segment(taskId)}/answer", body = JSONObject().put("answer", answer).put("idempotency_key", idempotencyKey))
    }

    suspend fun cancelTask(taskId: String, idempotencyKey: String): JSONObject {
        requireIdempotencyKey(idempotencyKey)
        return request("POST", "/v1/tasks/${segment(taskId)}/cancel", body = JSONObject().put("idempotency_key", idempotencyKey))
    }

    suspend fun retryTask(taskId: String, idempotencyKey: String): JSONObject {
        requireIdempotencyKey(idempotencyKey)
        return request("POST", "/v1/tasks/${segment(taskId)}/retry", body = JSONObject().put("idempotency_key", idempotencyKey))
    }

    suspend fun listTaskArtifacts(taskId: String): List<ControlPlaneArtifact> {
        require(REMOTE_TASK_ID_PATTERN.matches(taskId)) { "A valid remote task identifier is required." }
        val json = request("GET", "/v1/tasks/${segment(taskId)}/artifacts")
        val rows = json.optJSONArray("artifacts")
            ?: throw ControlPlaneException(null, "bad_response", "The Control Plane artifact list is incomplete.")
        if (rows.length() > MAX_ARTIFACTS_PER_TASK) {
            throw ControlPlaneException(null, "bad_response", "The Control Plane artifact list exceeds the supported limit.")
        }
        return (0 until rows.length()).map { index ->
            val item = rows.optJSONObject(index)
                ?: throw ControlPlaneException(null, "bad_response", "The Control Plane artifact list contains an invalid row.")
            ControlPlaneArtifact.fromJson(item, taskId)
        }
    }

    suspend fun createArtifactDownloadTicket(artifactId: String): ControlPlaneDownloadTicket {
        require(ARTIFACT_ID_PATTERN.matches(artifactId)) { "A valid artifact identifier is required." }
        val json = request(
            "POST", "/v1/artifacts/${segment(artifactId)}/download-ticket", body = JSONObject(),
        )
        val ticket = json.optString("ticket")
        val url = json.optString("url")
        val expiresAt = json.optString("expires_at")
        if (!DOWNLOAD_TICKET_PATTERN.matches(ticket) || url != "/v1/downloads/$ticket" || !TIMESTAMP_PATTERN.matches(expiresAt)) {
            throw ControlPlaneException(null, "bad_response", "The Control Plane download ticket response is invalid.")
        }
        val responseArtifactId = json.optString("artifact_id")
        if (responseArtifactId.isNotBlank() && responseArtifactId != artifactId) {
            throw ControlPlaneException(null, "bad_response", "The download ticket is bound to a different artifact.")
        }
        return ControlPlaneDownloadTicket(ticket, url, expiresAt)
    }

    /**
     * Opens a capability-ticket download without attaching the Control Plane bearer token.
     * Only the exact same-origin /v1/downloads/{ticket} path from the ticket schema is accepted.
     */
    suspend fun openArtifactDownload(ticket: ControlPlaneDownloadTicket): ControlPlaneDownloadResponse = withContext(Dispatchers.IO) {
        if (!DOWNLOAD_TICKET_PATTERN.matches(ticket.ticket) || ticket.url != "/v1/downloads/${ticket.ticket}") {
            throw ControlPlaneException(null, "bad_response", "The artifact download URL is invalid.")
        }
        val uri = try {
            URI(endpoint.scheme, null, endpoint.host, endpoint.port, ticket.url, null, null)
        } catch (_: Exception) {
            throw ControlPlaneException(null, "bad_response", "The artifact download URL is invalid.")
        }
        if (!sameOrigin(endpoint, uri) || uri.rawQuery != null || uri.rawFragment != null ||
            !DOWNLOAD_PATH_PATTERN.matches(uri.rawPath.orEmpty())) {
            throw ControlPlaneException(null, "bad_response", "The artifact download URL is outside the configured Control Plane origin.")
        }
        val response = try {
            artifactDownloadTransport.open(uri, requestTimeoutMs, UUID.randomUUID().toString().replace("-", ""))
        } catch (_: Exception) {
            throw ControlPlaneException(null, "network", "Could not reach the artifact download endpoint.")
        }
        if (response.statusCode == 200) return@withContext response
        val status = response.statusCode
        runCatching { response.close() }
        throw when (status) {
            in 300..399 -> ControlPlaneException(status, "redirect", "Artifact redirects are not followed for security.")
            401 -> ControlPlaneException(401, "unauthorized", "The Control Plane session expired or was revoked. Pair this device again.")
            403 -> ControlPlaneException(403, "forbidden", "Your Control Plane role does not permit this download.")
            404 -> ControlPlaneException(404, "not_found", "The artifact or download ticket was not found.")
            409 -> ControlPlaneException(409, "conflict", "The artifact is not available for download.")
            410 -> ControlPlaneException(410, "gone", "The artifact download ticket expired. A fresh ticket may be requested once.")
            416 -> ControlPlaneException(416, "range", "The artifact download range is not valid.")
            429 -> ControlPlaneException(429, "rate_limited", "The Control Plane is rate limiting artifact downloads. Try again shortly.")
            in 500..599 -> ControlPlaneException(status, "unavailable", "The artifact download service is temporarily unavailable.")
            else -> ControlPlaneException(status, "bad_response", "The artifact download returned an unexpected status.")
        }
    }

    private fun parseTask(json: JSONObject): ControlPlaneTask {
        val taskId = json.optString("task_id")
        val state = json.optString("state")
        if (taskId.isBlank() || state !in TASK_STATES) {
            throw ControlPlaneException(null, "bad_response", "The Control Plane task response is incomplete or incompatible.")
        }
        val questionJson = json.optJSONObject("question")
        val question = questionJson?.let { value ->
            val text = value.optString("text").take(4_000)
            if (text.isBlank()) null else {
                val optionsJson = value.optJSONArray("options")
                val options = if (optionsJson == null) emptyList() else (0 until minOf(optionsJson.length(), 16))
                    .mapNotNull { index -> optionsJson.optString(index).takeIf { it.isNotBlank() }?.take(500) }
                ControlPlaneQuestion(text, options)
            }
        }
        return ControlPlaneTask(
            taskId = taskId,
            workspaceId = json.optString("workspace_id"),
            nodeId = json.optString("node_id"),
            title = json.optString("title").ifBlank { "Untitled task" }.take(200),
            state = state,
            lastEventId = json.optLong("last_event_id", 0L).coerceAtLeast(0L),
            stale = json.optBoolean("stale", false),
            question = question,
            resultSummary = json.optString("result_summary").take(4_000).takeIf { it != "null" }.orEmpty(),
            failed = json.opt("error") != null && json.opt("error") != JSONObject.NULL,
        )
    }

    private fun safeEventSummary(type: String, payload: JSONObject): String = when (type) {
        "state_changed" -> {
            val state = payload.optString("state").takeIf { it in TASK_STATES } ?: "updated"
            "Task state changed · $state"
        }
        "question" -> "The agent is waiting for an answer"
        "answer_submitted" -> "Answer submitted"
        "core_event" -> "Execution progress updated"
        "artifact_available" -> "An artifact is available"
        "command_status" -> "Command delivery status changed"
        "delivery_failed" -> "Delivery failed · retry may be available"
        else -> "Task event"
    }

    private fun requireIdempotencyKey(key: String) {
        require(key.matches(IDEMPOTENCY_KEY_PATTERN)) { "Idempotency key must be 8..128 safe characters." }
    }

    suspend fun logout() {
        if (hasToken) request("POST", "/v1/auth/logout", body = JSONObject())
    }

    private suspend fun request(
        method: String,
        path: String,
        authenticated: Boolean = true,
        body: JSONObject? = null,
        query: Map<String, String> = emptyMap(),
        timeoutMs: Int = requestTimeoutMs,
    ): JSONObject = withContext(Dispatchers.IO) {
        if (authenticated && token.isNullOrBlank()) {
            throw ControlPlaneException(401, "unauthorized", "This device is not paired with the Control Plane.")
        }
        val uri = buildUri(path, query)
        val headers = linkedMapOf(
            "Accept" to "application/json",
            "User-Agent" to "asgend-android/1.0",
            "X-Request-ID" to UUID.randomUUID().toString().replace("-", ""),
        )
        if (authenticated) headers["Authorization"] = "Bearer $token"
        val bodyBytes = body?.toString()?.toByteArray(StandardCharsets.UTF_8)
        if (bodyBytes != null) headers["Content-Type"] = "application/json; charset=utf-8"
        val response = try {
            transport.execute(ControlPlaneRequest(method, uri, headers, bodyBytes, timeoutMs.coerceIn(1_000, 60_000)))
        } catch (e: ControlPlaneException) {
            throw e
        } catch (_: Exception) {
            throw ControlPlaneException(null, "network", "Could not reach the Control Plane. Check the HTTPS endpoint and connection.")
        }
        if (response.statusCode !in 200..299) {
            throw when (response.statusCode) {
                301, 302, 303, 307, 308 -> ControlPlaneException(response.statusCode, "redirect", "The Control Plane endpoint returned a redirect; redirects are not followed for security.")
                401 -> ControlPlaneException(401, "unauthorized", "The Control Plane session expired or was revoked. Pair this device again.")
                403 -> ControlPlaneException(403, "forbidden", "Your Control Plane role does not permit this operation.")
                404 -> ControlPlaneException(404, "not_found", "The requested Control Plane resource was not found.")
                409 -> ControlPlaneException(409, "conflict", "The Control Plane rejected this operation because the current state conflicts.")
                422 -> ControlPlaneException(422, "invalid", "The Control Plane rejected the request fields.")
                429 -> ControlPlaneException(429, "rate_limited", "The Control Plane is rate limiting requests. Try again shortly.")
                in 500..599 -> ControlPlaneException(response.statusCode, "unavailable", "The Control Plane is temporarily unavailable.")
                else -> ControlPlaneException(response.statusCode, "http_error", "The Control Plane request failed (HTTP ${response.statusCode}).")
            }
        }
        val version = response.headers.entries.firstOrNull { it.key.equals("x-asgend-api-version", true) }?.value
        if (!version.isNullOrBlank() && version.trim() != "1") {
            throw ControlPlaneException(null, "version", "The Control Plane API version is incompatible with this client.")
        }
        if (response.body.size > UrlConnectionControlPlaneTransport.MAX_JSON_RESPONSE_BYTES) {
            throw ControlPlaneException(null, "response_too_large", "The Control Plane response exceeds the supported size limit.")
        }
        val json = try {
            if (response.body.isEmpty()) throw IllegalArgumentException("Empty JSON response")
            JSONObject(response.body.toString(StandardCharsets.UTF_8))
        } catch (_: Exception) {
            throw ControlPlaneException(null, "bad_response", "The Control Plane returned an invalid JSON response.")
        }
        val bodyVersion = json.opt("api_version")
        val bodyVersionIsOne = bodyVersion is Number && bodyVersion.toDouble() == 1.0
        if (!bodyVersionIsOne) {
            throw ControlPlaneException(null, "version", "The Control Plane response body has a missing or incompatible API version.")
        }
        json
    }

    private fun buildUri(path: String, query: Map<String, String>): URI {
        require(path.startsWith("/") && !path.startsWith("//") && !path.contains("..") && !path.contains('\\')) {
            "Control Plane path must be relative to the validated API base."
        }
        val raw = endpoint.toASCIIString().trimEnd('/') + path + if (query.isEmpty()) "" else query.entries.joinToString("&", prefix = "?") {
            "${encode(it.key)}=${encode(it.value)}"
        }
        return URI(raw)
    }

    private fun segment(value: String): String {
        require(value.isNotBlank() && value.length <= 256 && value.none { it.code < 0x20 || it.code == 0x7f }) {
            "A valid resource identifier is required."
        }
        return encode(value)
    }

    companion object {
        private const val DEFAULT_TIMEOUT_MS = 20_000
        private val ALLOWED_ROLES = setOf("owner", "operator", "viewer")
        private val USER_TOKEN_PATTERN = Regex("ucp_[A-Za-z0-9_-]{32,256}")
        private val IDEMPOTENCY_KEY_PATTERN = Regex("[A-Za-z0-9._:-]{8,128}")
        private val TASK_STATES = setOf("pending_delivery", "delivered", "queued", "running", "waiting_input", "completed", "completed_without_model", "failed", "cancelled", "delivery_failed")
        private val ARTIFACT_ID_PATTERN = Regex("art_[0-9A-HJKMNP-TV-Za-hjkmnp-tv-z]{26}")
        private val REMOTE_TASK_ID_PATTERN = Regex("rt_[0-9A-HJKMNP-TV-Za-hjkmnp-tv-z]{26}")
        private val TIMESTAMP_PATTERN = Regex("[0-9]{4}-[0-9]{2}-[0-9]{2}[Tt][0-9]{2}:[0-9]{2}:[0-9]{2}(\\.[0-9]{1,9})?([Zz]|[+-][0-9]{2}:[0-9]{2})")
        private val DOWNLOAD_TICKET_PATTERN = Regex("dlt_[A-Za-z0-9_-]{32,256}")
        private val DOWNLOAD_PATH_PATTERN = Regex("/v1/downloads/dlt_[A-Za-z0-9_-]{32,256}")
        private const val MAX_ARTIFACTS_PER_TASK = 1_000

        private fun sameOrigin(left: URI, right: URI): Boolean =
            left.scheme.equals(right.scheme, true) && left.host.equals(right.host, true) &&
                effectivePort(left) == effectivePort(right)

        private fun effectivePort(uri: URI): Int = when {
            uri.port != -1 -> uri.port
            uri.scheme.equals("https", true) -> 443
            else -> -1
        }

        fun forEndpoint(
            endpoint: String,
            token: String? = null,
            transport: ControlPlaneTransport = UrlConnectionControlPlaneTransport(),
            requestTimeoutMs: Int = DEFAULT_TIMEOUT_MS,
            artifactDownloadTransport: ControlPlaneArtifactDownloadTransport = UrlConnectionControlPlaneArtifactDownloadTransport(),
        ): ControlPlaneClient = ControlPlaneClient(
            normalizeEndpoint(endpoint), token, transport, requestTimeoutMs.coerceIn(1_000, 60_000), artifactDownloadTransport,
        )

        fun normalizeEndpoint(raw: String): URI {
            val parsed = try { URI(raw.trim()) } catch (_: Exception) {
                throw ControlPlaneException(null, "endpoint", "Enter a valid HTTPS Control Plane endpoint.")
            }
            if (!parsed.scheme.equals("https", true) || parsed.host.isNullOrBlank() || parsed.rawUserInfo != null || parsed.rawQuery != null || parsed.rawFragment != null) {
                throw ControlPlaneException(null, "endpoint", "The Control Plane endpoint must use https://host[:port] without credentials, query or fragment.")
            }
            if (parsed.port !in -1..65535 || parsed.port == 0 || parsed.rawPath?.contains('\\') == true || parsed.normalize().rawPath != parsed.rawPath) {
                throw ControlPlaneException(null, "endpoint", "The Control Plane endpoint path or port is invalid.")
            }
            var basePath = parsed.path.orEmpty().trimEnd('/')
            if (basePath.endsWith("/v1")) basePath = basePath.dropLast(3).trimEnd('/')
            val port = if (parsed.port == 443) -1 else parsed.port
            return try { URI("https", null, parsed.host.lowercase(Locale.ROOT), port, basePath, null, null) }
            catch (_: Exception) { throw ControlPlaneException(null, "endpoint", "The Control Plane endpoint is invalid.") }
        }

        private fun encode(value: String): String = URLEncoder.encode(value, StandardCharsets.UTF_8)
            .replace("+", "%20").replace("%21", "!").replace("%27", "'").replace("%28", "(").replace("%29", ")").replace("%7E", "~")
    }
}

private fun JSONArray?.objects(): List<JSONObject> {
    if (this == null) return emptyList()
    return (0 until length()).mapNotNull { optJSONObject(it) }
}
