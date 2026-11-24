package org.asgend.nativeclient.data

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class RuntimeConfigDraftTest {
    @Test
    fun parsesOnlyApprovedSettingsAndPreservesHiddenFieldsOnUpdate() {
        val main = JSONObject(
            """{
              "server":{"auth_token":"server-secret","max_connections":128,"request_timeout_seconds":30},
              "local_llm":{"api_key":"provider-secret","temperature":0.4},
              "paths":{"skills_dir":"/private/skills"},
              "runtime":{"workers":2,"queue_max_pending":1000,"max_retries":2,"priority_aging_seconds":300},
              "sandbox":{"enabled":true,"require_landlock":true,"allow_network":false,"cpu_seconds":120,"memory_mb":512,"file_size_mb":64,"open_files":64}
            }""".trimIndent(),
        )
        val draft = RuntimeConfigDraft.fromMain(main).copy(workers = 4, allowSandboxNetwork = true)
        assertEquals(4, draft.workers)
        assertEquals("server-secret", main.getJSONObject("server").getString("auth_token"))
        assertEquals("provider-secret", main.getJSONObject("local_llm").getString("api_key"))

        draft.applyTo(main)
        assertEquals(4, main.getJSONObject("runtime").getInt("workers"))
        assertEquals(true, main.getJSONObject("sandbox").getBoolean("allow_network"))
        assertEquals("server-secret", main.getJSONObject("server").getString("auth_token"))
        assertEquals("provider-secret", main.getJSONObject("local_llm").getString("api_key"))
        assertEquals("/private/skills", main.getJSONObject("paths").getString("skills_dir"))
    }

    @Test
    fun serializesOnlyAllowListedFieldsForNonDestructiveCoreUpdate() {
        val settings = RuntimeConfigDraft(workers = 4, allowSandboxNetwork = true).toSettingsJson()
        assertEquals(setOf("server", "runtime", "sandbox"), settings.keys().asSequence().toSet())
        assertEquals(setOf("max_connections", "request_timeout_seconds"),
            settings.getJSONObject("server").keys().asSequence().toSet())
        assertEquals(setOf("workers", "queue_max_pending", "max_retries", "priority_aging_seconds"),
            settings.getJSONObject("runtime").keys().asSequence().toSet())
        assertEquals(setOf("enabled", "require_landlock", "allow_network", "cpu_seconds", "memory_mb", "file_size_mb", "open_files"),
            settings.getJSONObject("sandbox").keys().asSequence().toSet())
        assertEquals(4, settings.getJSONObject("runtime").getInt("workers"))
        assertEquals(true, settings.getJSONObject("sandbox").getBoolean("allow_network"))
        assertFalse(settings.toString().contains("auth_token"))
        assertFalse(settings.toString().contains("api_key"))
        assertFalse(settings.toString().contains("models"))
        assertFalse(settings.toString().contains("agents"))
    }

    @Test
    fun validatesCoreRangesBeforeAnySave() {
        assertNull(RuntimeConfigDraft().validationError())
        assertNull(RuntimeConfigDraft(workers = 64, queueMaxPending = 1_000_000, maxRetries = 10).validationError())
        assertNotNull(RuntimeConfigDraft(workers = 0).validationError())
        assertNotNull(RuntimeConfigDraft(maxRetries = 11).validationError())
        assertNotNull(RuntimeConfigDraft(sandboxMemoryMb = 63).validationError())
        assertNotNull(RuntimeConfigDraft(sandboxOpenFiles = 15).validationError())
    }

    @Test
    fun preservesBaselineSandboxDenyNetworkDefaults() {
        val draft = RuntimeConfigDraft.fromMain(
            JSONObject("""{"sandbox":{"enabled":true,"require_landlock":true,"allow_network":false}}"""),
        )
        assertTrue(draft.sandboxEnabled)
        assertTrue(draft.requireLandlock)
        assertEquals(false, draft.allowSandboxNetwork)
    }
}
