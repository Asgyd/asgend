package org.asgend.nativeclient.data

import kotlinx.coroutines.runBlocking
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class CoreRepositoryContractTest {
    private class RecordingTransport : CoreRpcTransport {
        var lastRequest: JSONObject? = null
        var timeout: Int = 0
        override suspend fun call(payload: JSONObject, timeoutMs: Int): JSONObject {
            lastRequest = JSONObject(payload.toString())
            timeout = timeoutMs
            return when (payload.optString("op")) {
                "list_tasks" -> JSONObject().put("ok", true).put("tasks", org.json.JSONArray())
                "events" -> JSONObject().put("ok", true).put("events", org.json.JSONArray())
                "plan_list" -> JSONObject().put("ok", true).put("items", org.json.JSONArray())
                "plan_wait" -> JSONObject().put("ok", true).put("revision", payload.optLong("revision") + 1).put("changed", true).put("stopping", false)
                "workspace_list" -> JSONObject().put("ok", true).put("entries", org.json.JSONArray())
                "mcp_status" -> JSONObject().put("ok", true).put("servers", org.json.JSONArray())
                "dead_letters" -> JSONObject().put("ok", true).put("dead_letters", org.json.JSONArray())
                "artifact_list" -> JSONObject().put("ok", true).put("artifacts", org.json.JSONArray()).put("count", 0)
                "workspace_read_bytes" -> JSONObject().put("ok", true).put("path", payload.optString("path"))
                    .put("offset", payload.optLong("offset")).put("next_offset", payload.optLong("offset"))
                    .put("size", 0).put("data_b64", "").put("eof", true)
                "lsp_diagnostics" -> JSONObject().put("ok", true).put("diagnostics", "No diagnostics")
                else -> JSONObject().put("ok", true).put("task_id", "task-test")
            }
        }
    }

    @Test
    fun submitSendsCoreSupportedFieldsAndStableIdempotencyKey() = runBlocking {
        val transport = RecordingTransport()
        val repo = CoreRepository(transport)
        repo.submitTask("  inspect project  ", 8, "idempotent-test", "session-1")
        val request = transport.lastRequest!!
        assertEquals("submit", request.optString("op"))
        assertEquals("inspect project", request.optString("task"))
        assertEquals(8, request.optInt("priority"))
        assertEquals("idempotent-test", request.optString("idempotency_key"))
        assertEquals("session-1", request.optString("session_id"))
    }

    @Test
    fun eventsUseAfterIdCursorAndBoundedLimit() = runBlocking {
        val transport = RecordingTransport()
        CoreRepository(transport).taskEvents("task-123", 77L, 40)
        assertEquals("events", transport.lastRequest!!.optString("op"))
        assertEquals("task-123", transport.lastRequest!!.optString("task_id"))
        assertEquals(77L, transport.lastRequest!!.optLong("after_id"))
        assertEquals(40, transport.lastRequest!!.optInt("limit"))
    }


    @Test
    fun planNotificationsUseRevisionLongPollAndExtraClientDeadline() = runBlocking {
        val transport = RecordingTransport()
        CoreRepository(transport).planWait(12L, 15_000)
        assertEquals("plan_wait", transport.lastRequest!!.optString("op"))
        assertEquals(12L, transport.lastRequest!!.optLong("revision"))
        assertEquals(15_000, transport.lastRequest!!.optInt("timeout_ms"))
        assertEquals(20_000, transport.timeout)
    }

    @Test
    fun planWaitRejectsInvalidRevisionAndTimeoutBeforeTransport() = runBlocking {
        val transport = RecordingTransport()
        val repo = CoreRepository(transport)
        assertThrows(IllegalArgumentException::class.java) { runBlocking { repo.planWait(-1L, 1000) } }
        assertThrows(IllegalArgumentException::class.java) { runBlocking { repo.planWait(0L, 30_001) } }
        assertEquals(null, transport.lastRequest)
    }

    @Test
    fun planAndWorkspaceCallsUseSharedCoreOperations() = runBlocking {
        val transport = RecordingTransport()
        val repo = CoreRepository(transport)
        repo.planAdd("global", "goal", "Ship Android", "native client")
        assertEquals("plan_add", transport.lastRequest!!.optString("op"))
        assertEquals("goal", transport.lastRequest!!.optString("kind"))
        assertEquals("global", transport.lastRequest!!.optString("scope_id"))
        repo.workspaceList("task-123", "docs")
        assertEquals("workspace_list", transport.lastRequest!!.optString("op"))
        assertEquals("docs", transport.lastRequest!!.optString("path"))
    }

    @Test
    fun modelControlUsesValidatedCoreOperationAndLongTimeout() = runBlocking {
        val transport = RecordingTransport()
        val repo = CoreRepository(transport)
        repo.modelAction("model_start", "local-fast", "llm")
        val request = transport.lastRequest!!
        assertEquals("model_start", request.optString("op"))
        assertEquals("local-fast", request.optString("model_id"))
        assertEquals("llm", request.optString("kind"))
        assertEquals(180_000, transport.timeout)
    }

    @Test
    fun modelControlRejectsUnknownOperationAndKindBeforeTransport() = runBlocking {
        val transport = RecordingTransport()
        val repo = CoreRepository(transport)
        assertThrows(IllegalArgumentException::class.java) {
            runBlocking { repo.modelAction("config_save", "local-fast", "llm") }
        }
        assertThrows(IllegalArgumentException::class.java) {
            runBlocking { repo.modelAction("model_start", "local-fast", "secret") }
        }
        assertEquals(null, transport.lastRequest)
    }


    @Test
    fun deadLettersUseBoundedCoreOperation() = runBlocking {
        val transport = RecordingTransport()
        CoreRepository(transport).deadLetters(500)
        assertEquals("dead_letters", transport.lastRequest!!.optString("op"))
        assertEquals(200, transport.lastRequest!!.optInt("limit"))
    }

    @Test
    fun artifactsAreRequestedForSelectedTaskOnly() = runBlocking {
        val transport = RecordingTransport()
        CoreRepository(transport).artifactList("task-123")
        assertEquals("artifact_list", transport.lastRequest!!.optString("op"))
        assertEquals("task-123", transport.lastRequest!!.optString("task_id"))
    }

    @Test
    fun lspDiagnosticsRejectAbsoluteAndTraversalPathsBeforeNetwork() = runBlocking {
        val transport = RecordingTransport()
        val repo = CoreRepository(transport)
        assertThrows(IllegalArgumentException::class.java) { runBlocking { repo.lspDiagnostics("/etc/passwd") } }
        assertThrows(IllegalArgumentException::class.java) { runBlocking { repo.lspDiagnostics("src/../../etc/passwd") } }
        assertEquals(null, transport.lastRequest)
    }

    @Test
    fun lspDiagnosticsUseRelativePathAndBoundedResponse() = runBlocking {
        val transport = RecordingTransport()
        CoreRepository(transport).lspDiagnostics("src/main.c")
        assertEquals("lsp_diagnostics", transport.lastRequest!!.optString("op"))
        assertEquals("src/main.c", transport.lastRequest!!.optString("file_path"))
        assertEquals(30_000, transport.timeout)
    }


    @Test
    fun binaryReadUsesBoundedChunksAndValidatesResponseEnvelope() = runBlocking {
        val transport = RecordingTransport()
        val chunk = CoreRepository(transport).workspaceReadBytes("task-123", "out/report.pdf", 0L, 256 * 1024)
        val request = transport.lastRequest!!
        assertEquals("workspace_read_bytes", request.optString("op"))
        assertEquals("task-123", request.optString("task_id"))
        assertEquals("out/report.pdf", request.optString("path"))
        assertEquals(256 * 1024, request.optInt("limit"))
        assertEquals(0L, chunk.size)
        assertTrue(chunk.eof)
        assertEquals(0, chunk.bytes.size)
    }

    @Test
    fun binaryReadRejectsUnsafePathBeforeTransport() = runBlocking {
        val transport = RecordingTransport()
        val repo = CoreRepository(transport)
        assertThrows(IllegalArgumentException::class.java) { runBlocking { repo.workspaceReadBytes("task-123", "../secrets.txt", 0L) } }
        assertThrows(IllegalArgumentException::class.java) { runBlocking { repo.workspaceReadBytes("task-123", "/etc/passwd", 0L) } }
        assertEquals(null, transport.lastRequest)
    }

    @Test
    fun oversizedTextWriteIsRejectedBeforeTransportCall() = runBlocking {
        val transport = RecordingTransport()
        val repo = CoreRepository(transport)
        val failure = assertThrows(IllegalArgumentException::class.java) {
            runBlocking { repo.workspaceWrite("task-123", "large.txt", "x".repeat(1024 * 1024 + 1)) }
        }
        assertTrue(failure.message.orEmpty().contains("1 MiB"))
        assertEquals(null, transport.lastRequest)
    }
}
