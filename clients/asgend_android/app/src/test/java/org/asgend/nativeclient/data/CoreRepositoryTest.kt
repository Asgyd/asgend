package org.asgend.nativeclient.data

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class CoreRepositoryTest {
    @Test
    fun extractsNestedQuestionFromEventWithoutLosingOptions() {
        val event = JSONObject(
            """{"event_id":17,"type":"agent_question","payload":{"question":{"question":"Where should it run?","options":[{"id":"local","label":"Locally"},{"id":"remote","label":"Remote node"}]}}}""",
        )
        val parsed = CoreRepository.parseQuestionFromEvent(event)
        assertEquals("Where should it run?", parsed?.optString("question"))
        val options = CoreRepository.questionOptions(parsed!!)
        assertEquals(2, options.size)
        assertEquals("local", options.first().optString("id"))
        assertEquals("Remote node", options.last().optString("label"))
    }

    @Test
    fun acceptsQuestionPayloadAlreadyAtRoot() {
        val event = JSONObject("""{"type":"question","payload":{"question":"Which mode?","options":[]}}""")
        val parsed = CoreRepository.parseQuestionFromEvent(event)
        assertEquals("Which mode?", parsed?.optString("question"))
        assertTrue(CoreRepository.questionOptions(parsed!!).isEmpty())
    }

    @Test
    fun statusQuestionHasPrecedenceCandidateAndMissingQuestionIsNull() {
        val status = JSONObject("""{"status":"waiting_input","result":{"question":{"question":"Need input","options":[]}}}""")
        assertEquals("Need input", CoreRepository.parseQuestionFromStatus(status)?.optString("question"))
        assertNull(CoreRepository.parseQuestionFromStatus(JSONObject("""{"status":"running"}""")))
    }

    @Test
    fun eventIdsAreMonotonicAndMissingIdDefaultsToZero() {
        assertEquals(42L, CoreRepository.eventId(JSONObject("""{"event_id":42}""")))
        assertEquals(0L, CoreRepository.eventId(JSONObject()))
    }
}
