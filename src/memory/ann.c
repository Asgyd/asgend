#include "internal.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ANN_BITS 16U
#define ANN_MAX_HAMMING 4U

static uint64_t splitmix64(uint64_t value) {
    value += UINT64_C(0x9e3779b97f4a7c15);
    value = (value ^ (value >> 30U)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27U)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31U);
}

uint16_t asgend_memory_ann_signature(const double *vector, size_t dimensions) {
    if (!vector || dimensions == 0U) return 0U;
    uint16_t signature = 0U;
    for (unsigned int plane = 0U; plane < ANN_BITS; ++plane) {
        long double projection = 0.0L;
        for (size_t i = 0U; i < dimensions; ++i) {
            uint64_t key = ((uint64_t)plane << 48U) ^ (uint64_t)i ^ UINT64_C(0x415347454e44414e);
            int coefficient = (splitmix64(key) & UINT64_C(1)) ? 1 : -1;
            projection += (long double)coefficient * (long double)vector[i];
        }
        if (projection >= 0.0L) signature = (uint16_t)(signature | (uint16_t)(1U << plane));
    }
    return signature;
}

void asgend_memory_ann_signature_hex(uint16_t signature, char out[5]) {
    static const char hex[] = "0123456789abcdef";
    out[0] = hex[(signature >> 12U) & 0x0fU];
    out[1] = hex[(signature >> 8U) & 0x0fU];
    out[2] = hex[(signature >> 4U) & 0x0fU];
    out[3] = hex[signature & 0x0fU];
    out[4] = '\0';
}

static int insert_probe(sqlite3_stmt *stmt, uint16_t signature) {
    char hex[5];
    asgend_memory_ann_signature_hex(signature, hex);
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_text(stmt, 1, hex, -1, SQLITE_TRANSIENT);
    return sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
}

int asgend_memory_ann_prepare_probes(sqlite3 *db, const double *vector, size_t dimensions,
                                     char *error, size_t error_size) {
    if (!db || !vector || !dimensions) {
        asgend_memory_internal_set_error(error, error_size, "ANN query vector is empty");
        return -1;
    }
    if (asgend_memory_internal_exec_sql(db,
            "CREATE TEMP TABLE IF NOT EXISTS asgend_ann_probes(signature TEXT PRIMARY KEY) WITHOUT ROWID;"
            "DELETE FROM temp.asgend_ann_probes; BEGIN;", error, error_size) != 0) return -1;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO temp.asgend_ann_probes(signature) VALUES(?1)", -1, &stmt, NULL) != SQLITE_OK) {
        (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return asgend_memory_internal_sqlite_error(db, error, error_size, "prepare ANN probe insert");
    }
    uint16_t base = asgend_memory_ann_signature(vector, dimensions);
    int failed = insert_probe(stmt, base);
    for (unsigned int i = 0U; !failed && i < ANN_BITS; ++i)
        failed = insert_probe(stmt, (uint16_t)(base ^ (uint16_t)(1U << i)));
    for (unsigned int i = 0U; !failed && i < ANN_BITS; ++i)
        for (unsigned int j = i + 1U; !failed && j < ANN_BITS; ++j)
            failed = insert_probe(stmt, (uint16_t)(base ^ (uint16_t)(1U << i) ^ (uint16_t)(1U << j)));
    for (unsigned int i = 0U; !failed && i < ANN_BITS; ++i)
        for (unsigned int j = i + 1U; !failed && j < ANN_BITS; ++j)
            for (unsigned int k = j + 1U; !failed && k < ANN_BITS; ++k)
                failed = insert_probe(stmt, (uint16_t)(base ^ (uint16_t)(1U << i) ^ (uint16_t)(1U << j) ^ (uint16_t)(1U << k)));
    for (unsigned int i = 0U; !failed && i < ANN_BITS; ++i)
        for (unsigned int j = i + 1U; !failed && j < ANN_BITS; ++j)
            for (unsigned int k = j + 1U; !failed && k < ANN_BITS; ++k)
                for (unsigned int m = k + 1U; !failed && m < ANN_BITS; ++m)
                    failed = insert_probe(stmt, (uint16_t)(base ^ (uint16_t)(1U << i) ^ (uint16_t)(1U << j) ^ (uint16_t)(1U << k) ^ (uint16_t)(1U << m)));
    sqlite3_finalize(stmt);
    if (failed) {
        (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return asgend_memory_internal_sqlite_error(db, error, error_size, "populate ANN probes");
    }
    if (asgend_memory_internal_exec_sql(db, "COMMIT", error, error_size) != 0) {
        (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    return 0;
}
