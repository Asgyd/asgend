package org.asgend.nativeclient.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.assertThrows
import org.junit.Test

class RemoteOperationLedgerTest {
    private class MemoryPersistence : RemoteOperationPersistence {
        var records: List<PendingRemoteOperation> = emptyList()
        var allowWrite = true
        override fun read() = records.toList()
        override fun write(records: List<PendingRemoteOperation>): Boolean {
            if (!allowWrite) return false
            this.records = records.toList()
            return true
        }
    }

    @Test fun repeatsTheSameKeyForTheExactPendingOperation() {
        val memory = MemoryPersistence()
        var now = 1_000_000L
        var sequence = 0
        val ledger = RemoteOperationLedger(memory, { now }, { "android:key-${++sequence}" })
        val first = ledger.getOrCreate("create:slot", "a".repeat(64))
        val second = ledger.getOrCreate("create:slot", "a".repeat(64))
        assertEquals(first.key, second.key)
        assertEquals(1, memory.records.size)
        now += 1000
        assertEquals(first.key, ledger.getOrCreate("create:slot", "a".repeat(64)).key)
    }

    @Test fun changedPayloadRequiresDifferentSlotAndDigest() {
        val memory = MemoryPersistence()
        var sequence = 0
        val ledger = RemoteOperationLedger(memory, { 1_000_000L }, { "android:key-${++sequence}" })
        val first = ledger.getOrCreate("create:one", "a".repeat(64))
        val second = ledger.getOrCreate("create:two", "b".repeat(64))
        assertNotEquals(first.key, second.key)
        assertEquals(2, memory.records.size)
    }

    @Test fun confirmedOperationIsRemovedOnlyByMatchingSlotAndKey() {
        val memory = MemoryPersistence()
        val ledger = RemoteOperationLedger(memory, { 1_000_000L }, { "android:key-123456" })
        val pending = ledger.getOrCreate("command:slot", "c".repeat(64))
        assertTrue(ledger.remove("command:slot", "wrong-key"))
        assertEquals(1, ledger.count())
        assertTrue(ledger.remove(pending.slot, pending.key))
        assertEquals(0, ledger.count())
    }

    @Test fun expiredAmbiguousOperationFailsClosedWithoutRotatingKey() {
        val memory = MemoryPersistence()
        var now = 1_000_000L
        var sequence = 0
        val ledger = RemoteOperationLedger(memory, { now }, { "android:key-${++sequence}" })
        val original = ledger.getOrCreate("create:slot", "d".repeat(64))
        now += RemoteOperationLedger.RETENTION_MILLIS + 1
        val expired = ledger.getOrCreate("create:slot", "d".repeat(64))
        assertTrue(expired.expired)
        assertEquals(original.key, expired.key)
        assertEquals(1, sequence)
    }

    @Test fun expiredRecordCanOnlyBeDiscardedAfterExplicitReconciliation() {
        val memory = MemoryPersistence()
        var now = 5_000_000L
        val ledger = RemoteOperationLedger(memory, { now }, { "android:expired-review-key" })
        val pending = ledger.getOrCreate("slot", "a".repeat(64))
        assertThrows(IllegalStateException::class.java) {
            ledger.discardExpired(pending.slot, pending.digest, pending.key)
        }
        now += RemoteOperationLedger.RETENTION_MILLIS + 1
        assertTrue(ledger.discardExpired(pending.slot, pending.digest, pending.key))
        assertEquals(0, ledger.count())
    }

    @Test fun fullLedgerAndFailedDurableWriteRejectMutation() {
        val full = MemoryPersistence()
        full.records = (0 until RemoteOperationLedger.MAX_RECORDS).map { i ->
            PendingRemoteOperation("slot-$i", i.toString().padStart(64, '0'), "android:key-$i", 1_000_000L)
        }
        val ledger = RemoteOperationLedger(full, { 1_000_000L }, { "android:new-key-000" })
        assertThrows(IllegalStateException::class.java) { ledger.getOrCreate("new-slot", "e".repeat(64)) }

        val failed = MemoryPersistence().apply { allowWrite = false }
        val failedLedger = RemoteOperationLedger(failed, { 1_000_000L }, { "android:new-key-001" })
        assertThrows(IllegalStateException::class.java) { failedLedger.getOrCreate("new-slot", "f".repeat(64)) }
        assertTrue(failed.records.isEmpty())
    }
}
