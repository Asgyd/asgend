package org.asgend.nativeclient.data

import kotlinx.coroutines.runBlocking
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.nio.file.Files
import java.security.MessageDigest
import java.net.URI

class RemoteArtifactTransferContractTest {
    private class JsonTransport(vararg replies: ControlPlaneResponse) : ControlPlaneTransport {
        val requests = mutableListOf<ControlPlaneRequest>()
        private val queue = replies.toList()
        private var index = 0
        override fun execute(request: ControlPlaneRequest): ControlPlaneResponse {
            requests += request
            return queue.getOrElse(index++) { error("Unexpected Control Plane request") }
        }
    }

    private class ByteTransport(vararg responses: ControlPlaneDownloadResponse) : ControlPlaneArtifactDownloadTransport {
        val uris = mutableListOf<URI>()
        private val queue = responses.toList()
        private var index = 0
        override fun open(uri: URI, timeoutMs: Int, requestId: String): ControlPlaneDownloadResponse {
            uris += uri
            return queue.getOrElse(index++) { error("Unexpected artifact download") }
        }
    }

    private fun json(json: JSONObject, status: Int = 200): ControlPlaneResponse {
        val body = JSONObject(json.toString())
        if (!body.has("api_version")) body.put("api_version", 1)
        return ControlPlaneResponse(status, mapOf("x-asgend-api-version" to "1"), body.toString().toByteArray())
    }

    private fun ticketReply(id: String = artifactId, suffix: String = "a"): ControlPlaneResponse {
        val ticket = "dlt_" + suffix.repeat(32)
        return json(JSONObject().put("ticket", ticket).put("url", "/v1/downloads/$ticket")
            .put("expires_at", "2026-10-25T12:00:00Z").put("artifact_id", id), 201)
    }

    private fun artifact(bytes: ByteArray, state: String = "available") = ControlPlaneArtifact(
        artifactId = artifactId, taskId = taskId, workspaceId = workspaceId, nodeId = nodeId,
        name = "result.pdf", mime = "application/pdf", size = bytes.size.toLong(),
        sha256 = sha(bytes), state = state, createdAt = "2026-10-25T10:00:00Z", expiresAt = null,
    )

    private fun response(bytes: ByteArray, checksum: String = sha(bytes), status: Int = 200,
                         contentLength: Long = bytes.size.toLong(), version: String? = "1"): ControlPlaneDownloadResponse {
        val headers = mutableMapOf("content-length" to contentLength.toString(), "x-checksum-sha256" to checksum)
        if (version != null) headers["x-asgend-api-version"] = version
        return ControlPlaneDownloadResponse(status, headers, ByteArrayInputStream(bytes))
    }

    private fun sha(bytes: ByteArray): String = MessageDigest.getInstance("SHA-256").digest(bytes)
        .joinToString("") { "%02x".format(it.toInt() and 0xff) }

    @Test fun artifactListValidatesRequiredMetadataAndTaskBinding() = runBlocking {
        val artifact = JSONObject().put("artifact_id", artifactId).put("task_id", taskId)
            .put("workspace_id", workspaceId).put("node_id", nodeId).put("name", "result.pdf")
            .put("mime", "application/pdf").put("size", 12).put("sha256", "a".repeat(64))
            .put("state", "available").put("created_at", "2026-10-25T10:00:00Z").put("expires_at", JSONObject.NULL)
        val json = JSONObject().put("artifacts", JSONArray(listOf(artifact)))
        val transport = JsonTransport(json(json))
        val result = ControlPlaneClient.forEndpoint("https://control.example", token, transport).listTaskArtifacts(taskId)
        assertEquals(1, result.size)
        assertEquals("application/pdf", result.single().mime)
        assertTrue(result.single().available)
        assertFalse(result.single().toString().contains("result.pdf"))
        assertEquals(null, result.single().expiresAt)
        assertFalse(transport.requests.single().headers["Authorization"].isNullOrBlank())

        val missingExpiry = JSONObject().put("artifact_id", artifactId).put("task_id", taskId)
            .put("workspace_id", workspaceId).put("node_id", nodeId).put("name", "result.pdf")
            .put("mime", "application/pdf").put("size", 12).put("sha256", "a".repeat(64))
            .put("state", "available").put("created_at", "2026-10-25T10:00:00Z")
        val missingExpiryClient = ControlPlaneClient.forEndpoint("https://control.example", token,
            JsonTransport(json(JSONObject().put("artifacts", JSONArray(listOf(missingExpiry))))))
        assertThrows(ControlPlaneException::class.java) { runBlocking { missingExpiryClient.listTaskArtifacts(taskId) } }

        val wrongExpiry = JSONObject().put("artifact_id", artifactId).put("task_id", taskId)
            .put("workspace_id", workspaceId).put("node_id", nodeId).put("name", "result.pdf")
            .put("mime", "application/pdf").put("size", 12).put("sha256", "a".repeat(64))
            .put("state", "available").put("created_at", "2026-10-25T10:00:00Z").put("expires_at", 12)
        val wrongExpiryClient = ControlPlaneClient.forEndpoint("https://control.example", token,
            JsonTransport(json(JSONObject().put("artifacts", JSONArray(listOf(wrongExpiry))))))
        assertThrows(ControlPlaneException::class.java) { runBlocking { wrongExpiryClient.listTaskArtifacts(taskId) } }

        val wrongTask = JsonTransport(json(JSONObject().put("artifacts", JSONArray(listOf(artifact.put("task_id", otherTaskId))))))
        assertThrows(ControlPlaneException::class.java) {
            runBlocking { ControlPlaneClient.forEndpoint("https://control.example", token, wrongTask).listTaskArtifacts(taskId) }
        }
    }

    @Test fun downloadTicketUsesAuthenticatedJsonButTicketGetIsSameOriginAndHasNoBearerApi() = runBlocking {
        val bytes = "hello artifact".toByteArray()
        val ticketJson = JsonTransport(ticketReply())
        val raw = ByteTransport(response(bytes))
        val client = ControlPlaneClient.forEndpoint("https://control.example/prefix", token, ticketJson, artifactDownloadTransport = raw)
        val ticket = client.createArtifactDownloadTicket(artifactId)
        assertEquals("POST", ticketJson.requests.single().method)
        assertTrue(ticketJson.requests.single().uri.path.endsWith("/v1/artifacts/$artifactId/download-ticket"))
        assertEquals("Bearer $token", ticketJson.requests.single().headers["Authorization"])
        client.openArtifactDownload(ticket).close()
        assertEquals(URI("https://control.example/v1/downloads/${ticket.ticket}"), raw.uris.single())
    }

    @Test fun externalOrMismatchedTicketUrlIsRejectedBeforeNetwork() = runBlocking {
        val fake = ByteTransport(response(byteArrayOf()))
        val client = ControlPlaneClient.forEndpoint("https://control.example", token, JsonTransport(), artifactDownloadTransport = fake)
        assertThrows(ControlPlaneException::class.java) {
            runBlocking { client.openArtifactDownload(ControlPlaneDownloadTicket("dlt_${"a".repeat(32)}", "https://evil.example/v1/downloads/dlt_${"a".repeat(32)}", "2026-10-25T12:00:00Z")) }
        }
        assertTrue(fake.uris.isEmpty())
    }

    @Test fun validDownloadIsWrittenOnlyAfterLengthAndDigestVerification() = runBlocking {
        val bytes = "verified artifact bytes".toByteArray()
        val jsonTransport = JsonTransport(ticketReply())
        val byteTransport = ByteTransport(response(bytes))
        val dir = Files.createTempDirectory("asgend-remote-artifact-test").toFile()
        val result = RemoteArtifactDownloader(ControlPlaneClient.forEndpoint("https://control.example", token, jsonTransport, artifactDownloadTransport = byteTransport))
            .download(artifact(bytes), dir)
        assertEquals(bytes.size.toLong(), result.file.length())
        assertEquals(sha(bytes), result.sha256)
        assertFalse(result.toString().contains("result.pdf"))
        assertEquals(bytes.toList(), result.file.readBytes().toList())
        assertTrue(dir.listFiles().orEmpty().none { it.name.endsWith(".part") })
        result.file.delete(); dir.delete()
    }

    @Test fun checksumAndSizeMismatchDeletePartialFile() = runBlocking {
        val bytes = "not the expected digest".toByteArray()
        val dir = Files.createTempDirectory("asgend-remote-artifact-bad").toFile()
        val badDigestClient = ControlPlaneClient.forEndpoint("https://control.example", token, JsonTransport(ticketReply()),
            artifactDownloadTransport = ByteTransport(response(bytes, checksum = "0".repeat(64))))
        assertThrows(ControlPlaneException::class.java) { runBlocking { RemoteArtifactDownloader(badDigestClient).download(artifact(bytes), dir) } }
        assertEquals(0, dir.listFiles().orEmpty().size)

        val badLengthClient = ControlPlaneClient.forEndpoint("https://control.example", token, JsonTransport(ticketReply("$artifactId")),
            artifactDownloadTransport = ByteTransport(response(bytes, contentLength = bytes.size + 1L)))
        assertThrows(ControlPlaneException::class.java) { runBlocking { RemoteArtifactDownloader(badLengthClient).download(artifact(bytes), dir) } }
        assertEquals(0, dir.listFiles().orEmpty().size)
        dir.delete()
    }

    @Test fun expiredTicketIsRefreshedOnceAndRedirectIsRejected() = runBlocking {
        val bytes = "artifact".toByteArray()
        val tickets = JsonTransport(ticketReply(suffix = "a"), ticketReply(suffix = "b"))
        val expired = ControlPlaneDownloadResponse(410, emptyMap(), ByteArrayInputStream(byteArrayOf()))
        val bytesTransport = ByteTransport(expired, response(bytes))
        val dir = Files.createTempDirectory("asgend-remote-artifact-expired").toFile()
        val verified = RemoteArtifactDownloader(ControlPlaneClient.forEndpoint("https://control.example", token, tickets, artifactDownloadTransport = bytesTransport))
            .download(artifact(bytes), dir)
        assertEquals(2, tickets.requests.size)
        assertEquals(2, bytesTransport.uris.size)
        verified.file.delete(); dir.delete()

        val redirectClient = ControlPlaneClient.forEndpoint("https://control.example", token, JsonTransport(ticketReply()),
            artifactDownloadTransport = ByteTransport(response(bytes, status = 302)))
        assertThrows(ControlPlaneException::class.java) {
            runBlocking { RemoteArtifactDownloader(redirectClient).download(artifact(bytes), Files.createTempDirectory("asgend-remote-redirect").toFile()) }
        }
    }

    @Test fun zeroByteArtifactIsVerifiedAndMissingVersionHeaderIsRejected() = runBlocking {
        val bytes = byteArrayOf()
        val zeroArtifact = artifact(bytes)
        val zeroDir = Files.createTempDirectory("asgend-remote-zero").toFile()
        val zeroClient = ControlPlaneClient.forEndpoint("https://control.example", token, JsonTransport(ticketReply()),
            artifactDownloadTransport = ByteTransport(response(bytes)))
        val verified = RemoteArtifactDownloader(zeroClient).download(zeroArtifact, zeroDir)
        assertEquals(0L, verified.file.length())
        assertEquals(sha(bytes), verified.sha256)
        verified.file.delete(); zeroDir.delete()

        val payload = "missing version header".toByteArray()
        val errorDir = Files.createTempDirectory("asgend-remote-missing-version").toFile()
        val missingVersionClient = ControlPlaneClient.forEndpoint("https://control.example", token, JsonTransport(ticketReply()),
            artifactDownloadTransport = ByteTransport(response(payload, version = null)))
        assertThrows(ControlPlaneException::class.java) { runBlocking { RemoteArtifactDownloader(missingVersionClient).download(artifact(payload), errorDir) } }
        assertEquals(0, errorDir.listFiles().orEmpty().size)
        errorDir.delete()
    }

    @Test fun sizeLimitAndOverrunAreRejectedWithoutLeavingFiles() = runBlocking {
        val bytes = "body".toByteArray()
        val dir = Files.createTempDirectory("asgend-remote-size").toFile()
        val transport = ByteTransport(response(bytes))
        val client = ControlPlaneClient.forEndpoint("https://control.example", token, JsonTransport(ticketReply()), artifactDownloadTransport = transport)
        assertThrows(ControlPlaneException::class.java) {
            runBlocking { RemoteArtifactDownloader(client, maxBytes = 2L).download(artifact(bytes), dir) }
        }
        assertTrue(transport.uris.isEmpty())
        assertEquals(0, dir.listFiles().orEmpty().size)

        val expected = artifact(byteArrayOf(1, 2, 3))
        val overrunResponse = response(bytes, checksum = expected.sha256, contentLength = expected.size)
        val overrunClient = ControlPlaneClient.forEndpoint("https://control.example", token, JsonTransport(ticketReply()), artifactDownloadTransport = ByteTransport(overrunResponse))
        assertThrows(ControlPlaneException::class.java) { runBlocking { RemoteArtifactDownloader(overrunClient).download(expected, dir) } }
        assertEquals(0, dir.listFiles().orEmpty().size)
        dir.delete()
    }

    @Test fun invalidArtifactNamesAndUnsupportedStatesNeverDownload() = runBlocking {
        val bytes = "abc".toByteArray()
        val dir = Files.createTempDirectory("asgend-remote-artifact-state").toFile()
        val transport = ByteTransport(response(bytes))
        val ticketTransport = JsonTransport(ticketReply())
        val client = ControlPlaneClient.forEndpoint("https://control.example", token, ticketTransport, artifactDownloadTransport = transport)
        val invalidName = artifact(bytes).copy(name = "../payload.bin")
        assertThrows(ControlPlaneException::class.java) { runBlocking { RemoteArtifactDownloader(client).download(invalidName, dir) } }
        assertEquals(0, ticketTransport.requests.size)
        val uploading = artifact(bytes, state = "uploading")
        assertThrows(ControlPlaneException::class.java) { runBlocking { RemoteArtifactDownloader(client).download(uploading, dir) } }
        assertEquals(0, ticketTransport.requests.size)
        assertTrue(transport.uris.isEmpty())
        assertEquals(0, dir.listFiles().orEmpty().size)
        dir.delete()
    }

    @Test fun streamFailureDeletesPartialFileAndDoesNotPublishResult() = runBlocking {
        val payload = "partial-before-I/O-failure".toByteArray()
        val broken = object : InputStream() {
            private var sent = false
            override fun read(): Int = throw IOException("synthetic source failure")
            override fun read(buffer: ByteArray, offset: Int, length: Int): Int {
                if (sent) throw IOException("synthetic source failure")
                sent = true
                val count = minOf(length, payload.size)
                payload.copyInto(buffer, offset, 0, count)
                return count
            }
        }
        val dir = Files.createTempDirectory("asgend-remote-stream-failure").toFile()
        val ticketJson = JsonTransport(ticketReply())
        val raw = ByteTransport(ControlPlaneDownloadResponse(200, mapOf(
            "content-length" to payload.size.toString(),
            "x-checksum-sha256" to sha(payload),
            "x-asgend-api-version" to "1",
        ), broken))
        val client = ControlPlaneClient.forEndpoint("https://control.example", token, ticketJson, artifactDownloadTransport = raw)
        assertThrows(IOException::class.java) { runBlocking { RemoteArtifactDownloader(client).download(artifact(payload), dir) } }
        assertEquals(0, dir.listFiles().orEmpty().size)
        dir.delete()
    }

    companion object {
        private const val token = "ucp_abcdefghijklmnopqrstuvwxyzABCDEF123456"
        private const val taskId = "rt_01ARZ3NDEKTSV4RRFFQ69G5FAV"
        private const val otherTaskId = "rt_01ARZ3NDEKTSV4RRFFQ69G5FAW"
        private const val artifactId = "art_01ARZ3NDEKTSV4RRFFQ69G5FAV"
        private const val workspaceId = "ws_01ARZ3NDEKTSV4RRFFQ69G5FAV"
        private const val nodeId = "node_01ARZ3NDEKTSV4RRFFQ69G5FAV"
    }
}
