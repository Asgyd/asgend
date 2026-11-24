package org.asgend.nativeclient.data

import java.util.UUID

/** Persistable record for a mutation whose server outcome is not yet confirmed. */
data class PendingRemoteOperation(
    val slot: String,
    val digest: String,
    val key: String,
    val createdAtMillis: Long,
    val expired: Boolean = false,
)

/** Storage boundary makes the ledger deterministic and testable without Android APIs. */
interface RemoteOperationPersistence {
    fun read(): List<PendingRemoteOperation>
    fun write(records: List<PendingRemoteOperation>): Boolean
}

/**
 * Bounded, fail-closed idempotency ledger. It deliberately does not discard unresolved records
 * when the retention window expires: automatic replacement could create a duplicate if the
 * server accepted the original request but the response was lost. An expired record therefore
 * requires reconciliation by the UI rather than issuing a new mutation key.
 */
class RemoteOperationLedger(
    private val persistence: RemoteOperationPersistence,
    private val nowMillis: () -> Long = System::currentTimeMillis,
    private val newKey: () -> String = { "android:${UUID.randomUUID()}" },
) {
    @Synchronized
    fun getOrCreate(slot: String, digest: String): PendingRemoteOperation {
        require(slot.isNotBlank() && slot.length <= MAX_SLOT_CHARS && slot.none { it.code < 0x20 || it.code == 0x7f }) {
            "Invalid operation slot."
        }
        require(digest.matches(DIGEST_PATTERN)) { "Operation digest must be a SHA-256 hex string." }
        val records = persistence.read()
        val existing = records.firstOrNull { it.slot == slot && it.digest == digest }
        if (existing != null) {
            return existing.copy(expired = isExpired(existing.createdAtMillis))
        }
        check(records.size < MAX_RECORDS) {
            "Too many unresolved remote operations. Review the remote task list before submitting more commands."
        }
        val key = newKey()
        require(key.matches(KEY_PATTERN)) { "Generated idempotency key is invalid." }
        val record = PendingRemoteOperation(slot, digest, key, nowMillis())
        check(persistence.write(records + record)) { "Could not persist the remote operation key before submission." }
        return record
    }

    @Synchronized
    fun remove(slot: String, key: String): Boolean {
        val records = persistence.read()
        val remaining = records.filterNot { it.slot == slot && it.key == key }
        return if (remaining.size == records.size) true else persistence.write(remaining)
    }

    @Synchronized
    fun discardExpired(slot: String, digest: String, key: String): Boolean {
        val records = persistence.read()
        val record = records.firstOrNull { it.slot == slot && it.digest == digest && it.key == key } ?: return true
        check(isExpired(record.createdAtMillis)) { "Only an expired unresolved operation can be discarded after review." }
        return persistence.write(records.filterNot { it.slot == slot && it.digest == digest && it.key == key })
    }

    @Synchronized
    fun count(): Int = persistence.read().size

    private fun isExpired(createdAt: Long): Boolean {
        val age = nowMillis() - createdAt
        return age < 0L || age > RETENTION_MILLIS
    }

    companion object {
        const val MAX_RECORDS = 128
        const val MAX_SLOT_CHARS = 512
        const val RETENTION_MILLIS = 7L * 24 * 60 * 60 * 1000
        private val DIGEST_PATTERN = Regex("[0-9a-f]{64}")
        private val KEY_PATTERN = Regex("[A-Za-z0-9._:-]{8,128}")
    }
}
