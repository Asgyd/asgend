package org.asgend.nativeclient.data

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlinx.coroutines.runBlocking
import java.net.URI

class ControlPlaneClientContractTest {
    private class FakeTransport(vararg replies: ControlPlaneResponse) : ControlPlaneTransport {
        val requests = mutableListOf<ControlPlaneRequest>()
        private val queuedReplies = replies.toList()
        private var index = 0
        override fun execute(request: ControlPlaneRequest): ControlPlaneResponse {
            requests += request
            return queuedReplies.getOrElse(index++) { error("Unexpected extra request") }
        }
    }

    private fun reply(
        json: JSONObject,
        status: Int = 200,
        headers: Map<String, String> = emptyMap(),
        addApiVersion: Boolean = true,
    ): ControlPlaneResponse {
        val body = JSONObject(json.toString())
        if (addApiVersion && !body.has("api_version")) body.put("api_version", 1)
        return ControlPlaneResponse(status, headers, body.toString().toByteArray(Charsets.UTF_8))
    }

    @Test fun endpointNormalizationRequiresHttpsAndRemovesV1Suffix() {
        assertEquals(URI("https://example.test/control"), ControlPlaneClient.normalizeEndpoint(" HTTPS://Example.Test:443/control/v1/ "))
        assertThrows(ControlPlaneException::class.java) { ControlPlaneClient.normalizeEndpoint("http://control.example") }
        assertThrows(ControlPlaneException::class.java) { ControlPlaneClient.normalizeEndpoint("https://user:pass@control.example") }
        assertThrows(ControlPlaneException::class.java) { ControlPlaneClient.normalizeEndpoint("https://control.example/?token=secret") }
        assertThrows(ControlPlaneException::class.java) { ControlPlaneClient.normalizeEndpoint("https://control.example/a/../b") }
    }

    @Test fun pairingUsesUnauthenticatedHttpsPostAndDoesNotPutCodeInUrl() = runBlocking {
        val transport = FakeTransport(reply(JSONObject().put("token", "ucp_abcdefghijklmnopqrstuvwxyzABCDEF123456").put("user_id", "user-1").put("workspace_id", "ws-1").put("role", "operator"), 201))
        val result = ControlPlaneClient.forEndpoint("https://control.example/root", transport = transport)
            .redeemPairing("abcde 12345", "phone")
        val request = transport.requests.single()
        assertEquals("POST", request.method)
        assertTrue(request.uri.path.endsWith("/v1/pairing/redeem"))
        assertNull(request.headers["Authorization"])
        assertFalse(request.uri.toString().contains("abcde%2012345"))
        val sent = JSONObject(String(request.body!!, Charsets.UTF_8))
        assertEquals("ABCDE 12345", sent.optString("code"))
        assertEquals("phone", sent.optString("device_name"))
        assertEquals("operator", result.role)
    }

    @Test fun authenticatedIdentityUsesBearerHeaderAndParsesMembership() = runBlocking {
        val user = JSONObject().put("user_id", "user-1").put("display_name", "Owner")
        val workspaces = JSONArray(listOf(JSONObject().put("workspace_id", "ws-1").put("name", "Main").put("role", "owner")))
        val transport = FakeTransport(reply(JSONObject().put("user", user).put("workspaces", workspaces), headers = mapOf("x-asgend-api-version" to "1")))
        val identity = ControlPlaneClient.forEndpoint("https://control.example", token = "ucp_abcdefghijklmnopqrstuvwxyzABCDEF123456", transport = transport).me()
        val request = transport.requests.single()
        assertEquals("Bearer ucp_abcdefghijklmnopqrstuvwxyzABCDEF123456", request.headers["Authorization"])
        assertFalse(request.uri.toString().contains("ucp_abcdefghijklmnopqrstuvwxyzABCDEF123456"))
        assertEquals("user-1", identity.userId)
        assertEquals("owner", identity.workspaces.single().role)
    }

    @Test fun rejectsMalformedPairingTokenAgainstOpenApiPattern() = runBlocking {
        val transport = FakeTransport(reply(JSONObject().put("token", "ucp_short").put("user_id", "user-1").put("workspace_id", "ws-1").put("role", "owner"), 201))
        assertThrows(ControlPlaneException::class.java) {
            runBlocking { ControlPlaneClient.forEndpoint("https://control.example", transport = transport).redeemPairing("abcde-12345", "phone") }
        }
    }

    @Test fun rejectsApiVersionMismatchAndRedirects() = runBlocking {
        val incompatible = FakeTransport(reply(JSONObject().put("status", "ok"), headers = mapOf("x-asgend-api-version" to "2")))
        assertThrows(ControlPlaneException::class.java) { runBlocking { ControlPlaneClient.forEndpoint("https://control.example", transport = incompatible).health() } }
        val redirect = FakeTransport(reply(JSONObject(), status = 302))
        val failure = assertThrows(ControlPlaneException::class.java) {
            runBlocking { ControlPlaneClient.forEndpoint("https://control.example", token = "ucp_abcdefghijklmnopqrstuvwxyzABCDEF123456", transport = redirect).me() }
        }
        assertEquals("redirect", failure.errorCode)
    }

    @Test fun rejectsMissingOrMismatchedBodyApiVersion() = runBlocking {
        val token = "ucp_abcdefghijklmnopqrstuvwxyzABCDEF123456"
        val missing = FakeTransport(reply(JSONObject().put("status", "ok"), addApiVersion = false))
        val missingFailure = assertThrows(ControlPlaneException::class.java) {
            runBlocking { ControlPlaneClient.forEndpoint("https://control.example", transport = missing).health() }
        }
        assertEquals("version", missingFailure.errorCode)

        val mismatch = FakeTransport(reply(JSONObject().put("api_version", 2).put("user_id", "user-1")))
        val mismatchFailure = assertThrows(ControlPlaneException::class.java) {
            runBlocking { ControlPlaneClient.forEndpoint("https://control.example", token = token, transport = mismatch).me() }
        }
        assertEquals("version", mismatchFailure.errorCode)

        val wrongType = FakeTransport(reply(JSONObject().put("api_version", "1").put("status", "ok")))
        val typeFailure = assertThrows(ControlPlaneException::class.java) {
            runBlocking { ControlPlaneClient.forEndpoint("https://control.example", transport = wrongType).health() }
        }
        assertEquals("version", typeFailure.errorCode)
    }

    @Test fun noTokenFailsBeforeNetworkCall() = runBlocking {
        val transport = FakeTransport()
        val failure = assertThrows(ControlPlaneException::class.java) {
            runBlocking { ControlPlaneClient.forEndpoint("https://control.example", transport = transport).me() }
        }
        assertEquals(401, failure.statusCode)
        assertTrue(transport.requests.isEmpty())
    }

    @Test fun nodeRequestEncodesWorkspaceAsPathSegment() = runBlocking {
        val nodes = JSONArray(listOf(JSONObject().put("node_id", "node-1").put("name", "build node").put("status", "online")))
        val transport = FakeTransport(reply(JSONObject().put("nodes", nodes)))
        val result = ControlPlaneClient.forEndpoint("https://control.example", token = "ucp_abcdefghijklmnopqrstuvwxyzABCDEF123456", transport = transport).listNodes("ws/id")
        assertTrue(transport.requests.single().uri.rawPath.endsWith("/v1/workspaces/ws%2Fid/nodes"))
        assertEquals("node-1", result.single().nodeId)
    }

    @Test fun artifactListRejectsMalformedTaskIdBeforeTransport() = runBlocking {
        val transport = FakeTransport()
        val client = ControlPlaneClient.forEndpoint("https://control.example", token = "ucp_abcdefghijklmnopqrstuvwxyzABCDEF123456", transport = transport)
        assertThrows(IllegalArgumentException::class.java) { runBlocking { client.listTaskArtifacts("rt_bad/../id") } }
        assertTrue(transport.requests.isEmpty())
    }

    @Test fun remoteTaskListUsesContractFiltersAndOpaqueCursor() = runBlocking {
        val row = JSONObject().put("task_id", "rt_123").put("workspace_id", "ws-1").put("node_id", "node-1")
            .put("title", "Build app").put("state", "running").put("last_event_id", 7).put("stale", false)
        val transport = FakeTransport(reply(JSONObject().put("tasks", JSONArray(listOf(row))).put("next_cursor", "opaque+/cursor")))
        val page = ControlPlaneClient.forEndpoint("https://control.example", token = "ucp_abcdefghijklmnopqrstuvwxyzABCDEF123456", transport = transport)
            .listTasks("ws-1", state = "running", nodeId = "node-1", limit = 25, cursor = "cursor+/opaque")
        val request = transport.requests.single()
        assertTrue(request.uri.rawPath.endsWith("/v1/workspaces/ws-1/tasks"))
        assertTrue(request.uri.rawQuery!!.contains("state=running"))
        assertTrue(request.uri.rawQuery!!.contains("node_id=node-1"))
        assertTrue(request.uri.rawQuery!!.contains("limit=25"))
        assertTrue(request.uri.rawQuery!!.contains("cursor=cursor%2B%2Fopaque"))
        assertEquals("opaque+/cursor", page.nextCursor)
        assertEquals("rt_123", page.tasks.single().taskId)
    }

    @Test fun taskSubmissionCarriesIdempotencyKeyAndValidatedFields() = runBlocking {
        val task = JSONObject().put("task_id", "rt_123").put("workspace_id", "ws-1").put("node_id", "node-1").put("title", "Build app").put("state", "pending_delivery").put("last_event_id", 0)
        val transport = FakeTransport(reply(task, 201))
        ControlPlaneClient.forEndpoint("https://control.example", token = "ucp_abcdefghijklmnopqrstuvwxyzABCDEF123456", transport = transport)
            .createTask("ws-1", "node-1", "Build the app", "android-create:abc123", priority = 7, ttlSeconds = 120)
        val request = transport.requests.single()
        assertEquals("POST", request.method)
        val body = JSONObject(String(request.body!!, Charsets.UTF_8))
        assertEquals("node-1", body.optString("node_id"))
        assertEquals("Build the app", body.optString("task"))
        assertEquals("android-create:abc123", body.optString("idempotency_key"))
        assertEquals(7, body.optInt("priority"))
        assertEquals(120, body.optInt("ttl_seconds"))
        assertThrows(IllegalArgumentException::class.java) {
            runBlocking { ControlPlaneClient.forEndpoint("https://control.example", token = "ucp_abcdefghijklmnopqrstuvwxyzABCDEF123456", transport = FakeTransport()).createTask("ws-1", "node-1", "x", "bad", priority = 99) }
        }
    }

    @Test fun eventLongPollUsesMonotonicCursorAndExtraTransportDeadline() = runBlocking {
        val event = JSONObject().put("event_id", 42).put("type", "state_changed").put("occurred_at", "2026-10-25T10:00:00Z").put("payload", JSONObject().put("state", "running"))
        val transport = FakeTransport(reply(JSONObject().put("events", JSONArray(listOf(event))).put("last_event_id", 42).put("has_more", false)))
        val page = ControlPlaneClient.forEndpoint("https://control.example", token = "ucp_abcdefghijklmnopqrstuvwxyzABCDEF123456", transport = transport)
            .taskEvents("rt_123", afterEventId = 41, limit = 10, waitMs = 25_000)
        val request = transport.requests.single()
        assertTrue(request.uri.rawQuery!!.contains("after_event_id=41"))
        assertTrue(request.uri.rawQuery!!.contains("wait_ms=25000"))
        assertEquals(45_000, request.timeoutMs)
        assertEquals(42L, page.lastEventId)
        assertEquals(42L, page.events.single().eventId)
        assertThrows(IllegalArgumentException::class.java) {
            runBlocking { ControlPlaneClient.forEndpoint("https://control.example", token = "ucp_abcdefghijklmnopqrstuvwxyzABCDEF123456", transport = FakeTransport()).taskEvents("rt_123", -1) }
        }
    }

    @Test fun answerCancelAndRetryUseDistinctContractPathsAndIdempotencyKeys() = runBlocking {
        val transport = FakeTransport(reply(JSONObject().put("command_id", "cmd-1"), 202), reply(JSONObject().put("command_id", "cmd-2"), 202), reply(JSONObject().put("command_id", "cmd-3"), 202))
        val client = ControlPlaneClient.forEndpoint("https://control.example", token = "ucp_abcdefghijklmnopqrstuvwxyzABCDEF123456", transport = transport)
        client.answerTask("rt_123", "Option A", "answer-key:0001")
        client.cancelTask("rt_123", "cancel-key:0001")
        client.retryTask("rt_123", "retry-key:0001")
        assertTrue(transport.requests[0].uri.path.endsWith("/v1/tasks/rt_123/answer"))
        assertTrue(transport.requests[1].uri.path.endsWith("/v1/tasks/rt_123/cancel"))
        assertTrue(transport.requests[2].uri.path.endsWith("/v1/tasks/rt_123/retry"))
        assertEquals("answer-key:0001", JSONObject(String(transport.requests[0].body!!, Charsets.UTF_8)).optString("idempotency_key"))
        assertEquals("cancel-key:0001", JSONObject(String(transport.requests[1].body!!, Charsets.UTF_8)).optString("idempotency_key"))
        assertEquals("retry-key:0001", JSONObject(String(transport.requests[2].body!!, Charsets.UTF_8)).optString("idempotency_key"))
    }


    @Test fun eventSummaryNeverProjectsUntrustedPayloadText() = runBlocking {
        val payload = JSONObject().put("state", "running").put("message", "secret provider reasoning must not leak")
        val event = JSONObject().put("event_id", 9).put("type", "state_changed").put("occurred_at", "now").put("payload", payload)
        val transport = FakeTransport(reply(JSONObject().put("events", JSONArray(listOf(event))).put("last_event_id", 9)))
        val page = ControlPlaneClient.forEndpoint("https://control.example", token = "ucp_abcdefghijklmnopqrstuvwxyzABCDEF123456", transport = transport)
            .taskEvents("rt_123", 0, waitMs = 0)
        assertEquals("Task state changed · running", page.events.single().safeSummary)
        assertFalse(page.events.single().safeSummary.contains("secret"))
    }

    @Test fun unknownEventTypeAdvancesPageWithoutPayloadProjection() = runBlocking {
        val payload = JSONObject().put("message", "untrusted payload")
        val event = JSONObject().put("event_id", 17).put("type", "future_private_type").put("occurred_at", "now").put("payload", payload)
        val transport = FakeTransport(reply(JSONObject().put("events", JSONArray(listOf(event))).put("last_event_id", 17)))
        val page = ControlPlaneClient.forEndpoint("https://control.example", token = "ucp_abcdefghijklmnopqrstuvwxyzABCDEF123456", transport = transport)
            .taskEvents("rt_123", 16, waitMs = 0)
        assertEquals(17L, page.lastEventId)
        assertEquals("Task event", page.events.single().safeSummary)
    }

    @Test fun taskFiltersAndCommandKeysRejectInvalidValuesBeforeTransport() = runBlocking {
        val transport = FakeTransport()
        val client = ControlPlaneClient.forEndpoint("https://control.example", token = "ucp_abcdefghijklmnopqrstuvwxyzABCDEF123456", transport = transport)
        assertThrows(IllegalArgumentException::class.java) { runBlocking { client.listTasks("ws-1", state = "imaginary_state") } }
        assertThrows(IllegalArgumentException::class.java) { runBlocking { client.listTasks("ws-1", limit = 201) } }
        assertThrows(IllegalArgumentException::class.java) { runBlocking { client.cancelTask("rt_123", "short") } }
        assertThrows(IllegalArgumentException::class.java) { runBlocking { client.answerTask("rt_123", " ", "answer-key:0001") } }
        assertTrue(transport.requests.isEmpty())
    }


    @Test fun questionResponseKeepsOnlyBoundedTextAndOptionLabels() = runBlocking {
        val question = JSONObject().put("text", "Where to deploy?").put("options", JSONArray(listOf("Local", "Remote"))).put("private_debug", "must not enter UI model")
        val task = JSONObject().put("task_id", "rt_123").put("workspace_id", "ws-1").put("node_id", "node-1")
            .put("title", "Task").put("state", "waiting_input").put("last_event_id", 1).put("question", question)
        val transport = FakeTransport(reply(task))
        val parsed = ControlPlaneClient.forEndpoint("https://control.example", token = "ucp_abcdefghijklmnopqrstuvwxyzABCDEF123456", transport = transport).getTask("rt_123")
        assertEquals("Where to deploy?", parsed.question?.text)
        assertEquals(listOf("Local", "Remote"), parsed.question?.options)
        assertFalse(parsed.question.toString().contains("private_debug"))
    }

}
