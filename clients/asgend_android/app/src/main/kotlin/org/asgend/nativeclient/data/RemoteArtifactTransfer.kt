package org.asgend.nativeclient.data

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.ByteArrayInputStream
import java.io.Closeable
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.io.InputStream
import java.net.URI
import java.security.MessageDigest
import java.util.Locale
import java.util.UUID
import javax.net.ssl.HttpsURLConnection

/** Metadata returned by GET /v1/tasks/{task}/artifacts. */
data class ControlPlaneArtifact(
    val artifactId: String,
    val taskId: String,
    val workspaceId: String,
    val nodeId: String,
    val name: String,
    val mime: String,
    val size: Long,
    val sha256: String,
    val state: String,
    val createdAt: String,
    val expiresAt: String?,
) {
    val available: Boolean get() = state == "available"

    override fun toString(): String =
        "ControlPlaneArtifact(artifactId=$artifactId, taskId=$taskId, name=[REDACTED], size=$size, state=$state, sha256=[REDACTED])"

    companion object {
        private val ARTIFACT_ID = Regex("art_[0-9A-HJKMNP-TV-Za-hjkmnp-tv-z]{26}")
        private val TASK_ID = Regex("rt_[0-9A-HJKMNP-TV-Za-hjkmnp-tv-z]{26}")
        private val WORKSPACE_ID = Regex("ws_[0-9A-HJKMNP-TV-Za-hjkmnp-tv-z]{26}")
        private val NODE_ID = Regex("node_[0-9A-HJKMNP-TV-Za-hjkmnp-tv-z]{26}")
        private val SHA256 = Regex("[0-9a-f]{64}")
        private val TIMESTAMP = Regex("[0-9]{4}-[0-9]{2}-[0-9]{2}[Tt][0-9]{2}:[0-9]{2}:[0-9]{2}(\\.[0-9]{1,9})?([Zz]|[+-][0-9]{2}:[0-9]{2})")
        private val MIME = Regex("[A-Za-z0-9][A-Za-z0-9!#$&^_.+-]{0,126}/[A-Za-z0-9][A-Za-z0-9!#$&^_.+-]{0,126}( *; *[A-Za-z0-9!#$&^_.+-]+=(\"[^\"\\u0000-\\u001f]*\"|[A-Za-z0-9!#$&^_.+-]+))*")
        private val STATES = setOf("uploading", "available", "rejected", "expired")
        const val MAX_SIZE_BYTES = 268_435_456L

        fun fromJson(json: JSONObject, expectedTaskId: String): ControlPlaneArtifact {
            val artifactId = json.optString("artifact_id")
            val taskId = json.optString("task_id")
            val name = json.optString("name")
            val mime = json.optString("mime")
            val state = json.optString("state")
            val sha = json.optString("sha256")
            val sizeValue = json.opt("size")
            val createdAt = json.optString("created_at")
            val expires = json.opt("expires_at")
            val workspaceId = json.optString("workspace_id")
            val nodeId = json.optString("node_id")
            if (!ARTIFACT_ID.matches(artifactId) || !TASK_ID.matches(taskId) || taskId != expectedTaskId ||
                name.isBlank() || name.length > 255 || name == "." || name == ".." ||
                name.any { it == '/' || it == '\\' || it.code < 0x20 || it.code == 0x7f } ||
                !MIME.matches(mime) || sizeValue !is Number ||
                sizeValue.toDouble() % 1.0 != 0.0 || sizeValue.toLong() !in 0L..MAX_SIZE_BYTES ||
                !SHA256.matches(sha) || state !in STATES || !TIMESTAMP.matches(createdAt) ||
                !WORKSPACE_ID.matches(workspaceId) || !NODE_ID.matches(nodeId) || !json.has("expires_at") ||
                (expires != JSONObject.NULL && (expires !is String || expires.isBlank() || !TIMESTAMP.matches(expires)))) {
                throw ControlPlaneException(null, "bad_response", "The Control Plane returned invalid artifact metadata.")
            }
            val expiry = (expires as? String)?.takeIf { it.isNotBlank() }
            return ControlPlaneArtifact(
                artifactId, taskId, workspaceId, nodeId, name, mime, sizeValue.toLong(), sha,
                state, createdAt, expiry,
            )
        }
    }
}

data class ControlPlaneDownloadTicket(
    val ticket: String,
    val url: String,
    val expiresAt: String,
) {
    override fun toString(): String = "ControlPlaneDownloadTicket([REDACTED], expiresAt=$expiresAt)"
}

data class ControlPlaneDownloadResponse(
    val statusCode: Int,
    val headers: Map<String, String>,
    val body: InputStream,
    private val closeAction: () -> Unit = {},
) : Closeable {
    fun header(name: String): String? = headers.entries.firstOrNull { it.key.equals(name, ignoreCase = true) }?.value

    override fun close() {
        try { body.close() } finally { closeAction() }
    }
}

/** Separate raw-byte transport: deliberately has no API for bearer credentials. */
fun interface ControlPlaneArtifactDownloadTransport {
    fun open(uri: URI, timeoutMs: Int, requestId: String): ControlPlaneDownloadResponse
}

/** TLS-validating ticket transport. It never attaches the user's Authorization bearer token. */
class UrlConnectionControlPlaneArtifactDownloadTransport : ControlPlaneArtifactDownloadTransport {
    override fun open(uri: URI, timeoutMs: Int, requestId: String): ControlPlaneDownloadResponse {
        require(uri.scheme.equals("https", ignoreCase = true)) { "Artifact download requires HTTPS." }
        val connection = uri.toURL().openConnection() as? HttpsURLConnection
            ?: throw IOException("Artifact download requires HTTPS.")
        connection.instanceFollowRedirects = false
        connection.connectTimeout = timeoutMs.coerceIn(1_000, 60_000)
        connection.readTimeout = timeoutMs.coerceIn(1_000, 60_000)
        connection.requestMethod = "GET"
        connection.useCaches = false
        connection.doInput = true
        connection.setRequestProperty("Accept", "application/octet-stream")
        connection.setRequestProperty("Accept-Encoding", "identity")
        connection.setRequestProperty("User-Agent", "asgend-android/1.0")
        connection.setRequestProperty("X-Request-ID", requestId)
        return try {
            val status = connection.responseCode
            val headers = mutableMapOf<String, String>()
            connection.headerFields.forEach { (key, values) ->
                if (key != null && !values.isNullOrEmpty()) headers[key.lowercase(Locale.ROOT)] = values.first()
            }
            val stream = if (status in 200..299) connection.inputStream else connection.errorStream
            ControlPlaneDownloadResponse(
                statusCode = status,
                headers = headers,
                body = stream ?: ByteArrayInputStream(ByteArray(0)),
                closeAction = { connection.disconnect() },
            )
        } catch (failure: Throwable) {
            connection.disconnect()
            throw failure
        }
    }
}

data class VerifiedRemoteArtifactFile(
    val file: File,
    val artifact: ControlPlaneArtifact,
    val sha256: String,
) {
    override fun toString(): String = "VerifiedRemoteArtifactFile(name=[REDACTED], size=${artifact.size}, sha256=[REDACTED])"
}

/** Bounded stream-to-file download. A file becomes visible to UI only after size and SHA-256 match. */
class RemoteArtifactDownloader(
    private val client: ControlPlaneClient,
    private val maxBytes: Long = ControlPlaneArtifact.MAX_SIZE_BYTES,
) {
    suspend fun download(
        artifact: ControlPlaneArtifact,
        targetDir: File,
        onProgress: (received: Long, total: Long) -> Unit = { _, _ -> },
    ): VerifiedRemoteArtifactFile = withContext(Dispatchers.IO) {
        if (!artifact.available) throw ControlPlaneException(409, "conflict", "This artifact is not available for download.")
        if (!isValidArtifactName(artifact.name)) {
            throw ControlPlaneException(null, "bad_response", "The artifact name is invalid; refusing to create a download ticket.")
        }
        if (artifact.size !in 0L..maxBytes || !artifact.sha256.matches(Regex("[0-9a-f]{64}"))) {
            throw ControlPlaneException(413, "payload_too_large", "Artifact metadata is invalid or exceeds the client download limit.")
        }
        if (!targetDir.exists() && !targetDir.mkdirs()) throw IOException("Private artifact cache is unavailable.")
        if (!targetDir.isDirectory) throw IOException("Private artifact cache is unavailable.")

        var partial: File? = null
        val startedAtNanos = System.nanoTime()
        try {
            var ticket = client.createArtifactDownloadTicket(artifact.artifactId)
            val openedResponse = try {
                client.openArtifactDownload(ticket)
            } catch (expired: ControlPlaneException) {
                if (expired.statusCode != 410 && expired.errorCode != "gone") throw expired
                ticket = client.createArtifactDownloadTicket(artifact.artifactId)
                client.openArtifactDownload(ticket)
            }
            openedResponse.use { streamResponse ->
                if (streamResponse.statusCode != 200) {
                    throw ControlPlaneException(streamResponse.statusCode, "bad_response", "The artifact download did not return a complete file response.")
                }
                val version = streamResponse.header("x-asgend-api-version")
                if (version != "1") throw ControlPlaneException(null, "version", "The artifact download API version is missing or incompatible.")
                val headerSha = streamResponse.header("x-checksum-sha256")?.lowercase(Locale.ROOT)
                if (headerSha == null || !headerSha.matches(Regex("[0-9a-f]{64}")) || headerSha != artifact.sha256) {
                    throw ControlPlaneException(null, "checksum_mismatch", "The artifact checksum header does not match the inventory.")
                }
                val contentLength = streamResponse.header("content-length")?.toLongOrNull()
                    ?: throw ControlPlaneException(null, "bad_response", "The artifact download omitted Content-Length.")
                if (contentLength != artifact.size || contentLength > maxBytes) {
                    throw ControlPlaneException(null, "checksum_mismatch", "The artifact size does not match its inventory metadata.")
                }

                partial = File.createTempFile("asgend-remote-", ".part", targetDir)
                val digest = MessageDigest.getInstance("SHA-256")
                var received = 0L
                var lastProgressByte = -1L
                FileOutputStream(partial!!).use { output ->
                    val buffer = ByteArray(BUFFER_BYTES)
                    while (true) {
                        currentCoroutineContext().ensureActive()
                        if (System.nanoTime() - startedAtNanos > MAX_DOWNLOAD_DURATION_NANOS) {
                            throw ControlPlaneException(null, "timeout", "The artifact download exceeded the absolute time limit.")
                        }
                        val count = streamResponse.body.read(buffer)
                        if (System.nanoTime() - startedAtNanos > MAX_DOWNLOAD_DURATION_NANOS) {
                            throw ControlPlaneException(null, "timeout", "The artifact download exceeded the absolute time limit.")
                        }
                        if (count < 0) break
                        if (count == 0) continue
                        received += count
                        if (received > maxBytes || received > artifact.size) {
                            throw ControlPlaneException(null, "checksum_mismatch", "The artifact stream exceeded its declared size.")
                        }
                        output.write(buffer, 0, count)
                        digest.update(buffer, 0, count)
                        if (received == artifact.size || received - lastProgressByte >= PROGRESS_INTERVAL_BYTES) {
                            lastProgressByte = received
                            onProgress(received, artifact.size)
                        }
                    }
                    output.flush()
                    output.fd.sync()
                }
                val actualSha = digest.digest().joinToString("") { "%02x".format(it.toInt() and 0xff) }
                if (received != artifact.size || actualSha != artifact.sha256) {
                    throw ControlPlaneException(null, "checksum_mismatch", "The downloaded file failed size/SHA-256 verification.")
                }
                val safeName = sanitizeArtifactFileName(artifact.name)
                val finalFile = uniqueTarget(targetDir, "${artifact.artifactId.takeLast(8)}-$safeName")
                if (!partial!!.renameTo(finalFile)) throw IOException("Could not finalize the verified artifact.")
                partial = null
                onProgress(received, artifact.size)
                VerifiedRemoteArtifactFile(finalFile, artifact, actualSha)
            }
        } catch (cancelled: kotlinx.coroutines.CancellationException) {
            throw cancelled
        } finally {
            // use{} owns response closure; this block only removes an unverified partial file.
            partial?.let { runCatching { it.delete() } }
        }
    }

    private fun uniqueTarget(dir: File, filename: String): File {
        val base = filename.take(220).ifBlank { "artifact.bin" }
        var candidate = File(dir, base)
        var suffix = 1
        while (candidate.exists()) {
            val dot = base.lastIndexOf('.')
            val stem = if (dot > 0) base.substring(0, dot) else base
            val ext = if (dot > 0) base.substring(dot) else ""
            candidate = File(dir, "$stem ($suffix)$ext")
            suffix++
        }
        return candidate
    }

    companion object {
        const val BUFFER_BYTES = 64 * 1024
        const val PROGRESS_INTERVAL_BYTES = 1024 * 1024
        const val MAX_DOWNLOAD_DURATION_MILLIS = 30L * 60L * 1000L
        private const val MAX_DOWNLOAD_DURATION_NANOS = MAX_DOWNLOAD_DURATION_MILLIS * 1_000_000L
    }
}

fun isValidArtifactName(value: String): Boolean =
    value.isNotBlank() && value.length <= 255 && value != "." && value != ".." &&
        value.none { it == '/' || it == '\\' || it.code < 0x20 || it.code == 0x7f }

fun sanitizeArtifactFileName(raw: String): String {
    var value = raw.replace(Regex("[\\x00-\\x1f\\x7f<>:\"|?*\\\\/]"), "_").trim()
    while (value.startsWith('.')) value = value.drop(1)
    if (value == "." || value == "..") value = ""
    value = value.take(255)
    return value.ifBlank { "artifact.bin" }
}
