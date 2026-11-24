#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L
#include "asgend_memory.h"
#include "internal.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

#define CHUNK_DEFAULT_TOKENS 400U
#define CHUNK_MAX_TOKENS 8192U

static const char *chunker_version(unsigned int tokens, char buffer[64]) {
    (void)snprintf(buffer, 64, "utf8-paragraph-v1-t%u", tokens);
    return buffer;
}

static char *column_dup(sqlite3_stmt *stmt, int column) {
    const unsigned char *value = sqlite3_column_text(stmt, column);
    return strdup(value ? (const char *)value : "");
}

static int build_line_offsets(const char *text, size_t length, size_t **offsets_out, size_t *count_out) {
    *offsets_out = NULL; *count_out = 0;
    size_t lines = 1;
    for (size_t i = 0; i < length; ++i) if (text[i] == '\n') ++lines;
    if (lines > SIZE_MAX / sizeof(size_t)) return -1;
    size_t *offsets = malloc(lines * sizeof(*offsets));
    if (!offsets) return -1;
    size_t index = 0;
    offsets[index++] = 0;
    for (size_t i = 0; i < length; ++i) if (text[i] == '\n' && i + 1U <= length) offsets[index++] = i + 1U;
    *offsets_out = offsets; *count_out = index;
    return 0;
}

static uint64_t line_at(const size_t *offsets, size_t count, size_t byte_offset) {
    size_t low = 0, high = count;
    while (low + 1U < high) {
        size_t mid = low + (high - low) / 2U;
        if (offsets[mid] <= byte_offset) low = mid;
        else high = mid;
    }
    return (uint64_t)low + 1U;
}

static size_t utf8_back_boundary(const char *text, size_t start, size_t pos) {
    while (pos > start && (((unsigned char)text[pos] & 0xc0U) == 0x80U)) --pos;
    return pos;
}

static size_t utf8_forward_boundary(const char *text, size_t length, size_t pos) {
    while (pos < length && (((unsigned char)text[pos] & 0xc0U) == 0x80U)) ++pos;
    return pos;
}

static int insert_chunk(sqlite3 *db, sqlite3_stmt *stmt, int64_t file_id,
                        const char *source_hash, unsigned int index,
                        uint64_t source_start, uint64_t source_line,
                        size_t start, size_t end, const size_t *line_offsets, size_t line_count,
                        const char *text, const char *path, const char *title,
                        const char *description, const char *metadata_text,
                        const char *version) {
    (void)db;
    sqlite3_reset(stmt); sqlite3_clear_bindings(stmt);
    sqlite3_bind_int64(stmt, 1, file_id);
    sqlite3_bind_text(stmt, 2, source_hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, (int)index);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)(source_start + start));
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)(source_start + end));
    sqlite3_bind_int64(stmt, 6, (sqlite3_int64)(source_line + line_at(line_offsets, line_count, start) - 1U));
    sqlite3_bind_int64(stmt, 7, (sqlite3_int64)(source_line + line_at(line_offsets, line_count, end > start ? end - 1U : end) - 1U));
    sqlite3_bind_text(stmt, 8, text + start, (int)(end - start), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 11, description, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 12, metadata_text, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 13, version, -1, SQLITE_TRANSIENT);
    return sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
}

static int create_chunks_for_file(sqlite3 *db, int64_t file_id, const char *hash,
                                  const char *text, size_t text_length,
                                  uint64_t body_start_byte, uint64_t body_start_line,
                                  const char *path, const char *title, const char *description,
                                  const char *metadata_text, unsigned int max_tokens,
                                  const char *version, uint64_t *count_out,
                                  char *error, size_t error_size) {
    *count_out = 0;
    size_t *line_offsets = NULL, line_count = 0;
    if (build_line_offsets(text, text_length, &line_offsets, &line_count) != 0) {
        asgend_memory_internal_set_error(error, error_size, "out of memory preparing source line offsets");
        return -1;
    }
    const char *insert_sql =
        "INSERT INTO file_chunks(file_id,source_sha256,chunk_index,start_byte,end_byte,start_line,end_line,chunk_text,relative_path,title,description,metadata_text,chunker_version) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13)";
    sqlite3_stmt *insert = NULL;
    if (sqlite3_prepare_v2(db, insert_sql, -1, &insert, NULL) != SQLITE_OK) {
        free(line_offsets);
        return asgend_memory_internal_sqlite_error(db, error, error_size, "prepare source chunk insert");
    }
    /* 3 bytes/token is a conservative UTF-8 sizing approximation, not model tokenizer output. */
    size_t target_bytes = (size_t)max_tokens * 3U;
    if (target_bytes < 96U) target_bytes = 96U;
    size_t overlap = target_bytes / 8U;
    if (overlap > 512U) overlap = 512U;
    if (overlap < 24U) overlap = 24U;
    size_t start = 0;
    unsigned int chunk_index = 0;
    while (start < text_length) {
        size_t end = text_length - start > target_bytes ? start + target_bytes : text_length;
        if (end < text_length) {
            size_t lower = start + target_bytes / 2U;
            for (size_t p = end; p > lower; --p) {
                if (text[p - 1U] == '\n') { end = p; break; }
            }
        }
        end = utf8_back_boundary(text, start, end);
        if (end <= start) {
            end = start + target_bytes;
            if (end > text_length) end = text_length;
            end = utf8_forward_boundary(text, text_length, end);
            if (end <= start) { sqlite3_finalize(insert); free(line_offsets); asgend_memory_internal_set_error(error, error_size, "cannot find a valid UTF-8 chunk boundary"); return -1; }
        }
        if (insert_chunk(db, insert, file_id, hash, chunk_index, body_start_byte, body_start_line,
                         start, end, line_offsets, line_count, text, path, title, description,
                         metadata_text, version) != 0) {
            sqlite3_finalize(insert); free(line_offsets);
            return asgend_memory_internal_sqlite_error(db, error, error_size, "insert source chunk");
        }
        ++chunk_index;
        if (end >= text_length) break;
        size_t next_start = end > overlap ? end - overlap : end;
        next_start = utf8_forward_boundary(text, text_length, next_start);
        if (next_start <= start || next_start >= end) next_start = end;
        start = next_start;
    }
    sqlite3_finalize(insert);
    free(line_offsets);
    *count_out = chunk_index;
    return 0;
}

int asgend_memory_internal_sync_chunks(sqlite3 *db, unsigned int max_chunk_tokens,
                                       uint64_t *files_seen, uint64_t *files_chunked,
                                       uint64_t *chunks_created, uint64_t *skipped,
                                       char *error, size_t error_size) {
    if (!db) { asgend_memory_internal_set_error(error, error_size, "SQLite database is required"); return -1; }
    if (max_chunk_tokens == 0) max_chunk_tokens = CHUNK_DEFAULT_TOKENS;
    if (max_chunk_tokens > CHUNK_MAX_TOKENS) { asgend_memory_internal_set_error(error, error_size, "chunk token size must be <= %u", CHUNK_MAX_TOKENS); return -1; }
    if (files_seen) *files_seen = 0;
    if (files_chunked) *files_chunked = 0;
    if (chunks_created) *chunks_created = 0;
    if (skipped) *skipped = 0;
    char version_buffer[64];
    const char *version = chunker_version(max_chunk_tokens, version_buffer);
    if (asgend_memory_internal_exec_sql(db, "BEGIN IMMEDIATE", error, error_size) != 0) return -1;
    const char *cleanup_sql =
        "DELETE FROM file_chunks WHERE NOT EXISTS (SELECT 1 FROM files f JOIN file_contents c ON c.file_id=f.file_id "
        "WHERE f.file_id=file_chunks.file_id AND f.deleted_at IS NULL AND f.sha256=file_chunks.source_sha256 "
        "AND c.source_sha256=f.sha256 AND c.content_text IS NOT NULL AND c.extract_status IN ('extracted','truncated','parsed_partial'));"
        "DELETE FROM file_chunk_state WHERE NOT EXISTS (SELECT 1 FROM files f JOIN file_contents c ON c.file_id=f.file_id "
        "WHERE f.file_id=file_chunk_state.file_id AND f.deleted_at IS NULL AND f.sha256=file_chunk_state.source_sha256 "
        "AND c.source_sha256=f.sha256 AND c.content_text IS NOT NULL AND c.extract_status IN ('extracted','truncated','parsed_partial'));";
    if (asgend_memory_internal_exec_sql(db, cleanup_sql, error, error_size) != 0 ||
        asgend_memory_internal_exec_sql(db, "COMMIT", error, error_size) != 0) {
        (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL); return -1;
    }
    sqlite3_stmt *ids_stmt = NULL;
    const char *ids_sql =
        "SELECT f.file_id FROM files f JOIN file_contents c ON c.file_id=f.file_id AND c.source_sha256=f.sha256 "
        "WHERE f.deleted_at IS NULL AND c.content_text IS NOT NULL AND c.extract_status IN ('extracted','truncated','parsed_partial') ORDER BY f.file_id";
    if (sqlite3_prepare_v2(db, ids_sql, -1, &ids_stmt, NULL) != SQLITE_OK)
        return asgend_memory_internal_sqlite_error(db, error, error_size, "prepare chunk source list");
    int64_t *ids = NULL; size_t id_count = 0, id_capacity = 0;
    int rc;
    while ((rc = sqlite3_step(ids_stmt)) == SQLITE_ROW) {
        if (id_count == id_capacity) {
            size_t new_capacity = id_capacity ? id_capacity * 2U : 128U;
            if (new_capacity > SIZE_MAX / sizeof(*ids)) { sqlite3_finalize(ids_stmt); free(ids); asgend_memory_internal_set_error(error, error_size, "too many files to chunk"); return -1; }
            int64_t *grown = realloc(ids, new_capacity * sizeof(*ids));
            if (!grown) { sqlite3_finalize(ids_stmt); free(ids); asgend_memory_internal_set_error(error, error_size, "out of memory listing files to chunk"); return -1; }
            ids = grown; id_capacity = new_capacity;
        }
        ids[id_count++] = sqlite3_column_int64(ids_stmt, 0);
    }
    sqlite3_finalize(ids_stmt);
    if (rc != SQLITE_DONE) { free(ids); return asgend_memory_internal_sqlite_error(db, error, error_size, "read chunk source list"); }
    sqlite3_stmt *load = NULL, *remove = NULL, *save_state = NULL;
    const char *load_sql =
        "SELECT f.sha256,c.content_text,c.body_start_byte,c.body_start_line,d.relative_path,d.title,d.description,d.metadata_text,"
        "s.source_sha256,s.chunker_version,s.status FROM files f JOIN file_contents c ON c.file_id=f.file_id AND c.source_sha256=f.sha256 "
        "JOIN memory_search_docs d ON d.file_id=f.file_id LEFT JOIN file_chunk_state s ON s.file_id=f.file_id "
        "WHERE f.file_id=?1 AND f.deleted_at IS NULL AND c.content_text IS NOT NULL AND c.extract_status IN ('extracted','truncated','parsed_partial')";
    if (sqlite3_prepare_v2(db, load_sql, -1, &load, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, "DELETE FROM file_chunks WHERE file_id=?1", -1, &remove, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, "INSERT INTO file_chunk_state(file_id,source_sha256,chunker_version,status,chunk_count,error_text,updated_at) VALUES(?1,?2,?3,?4,?5,NULL,strftime('%Y-%m-%dT%H:%M:%fZ','now')) ON CONFLICT(file_id) DO UPDATE SET source_sha256=excluded.source_sha256,chunker_version=excluded.chunker_version,status=excluded.status,chunk_count=excluded.chunk_count,error_text=NULL,updated_at=excluded.updated_at", -1, &save_state, NULL) != SQLITE_OK) {
        if (load) sqlite3_finalize(load);
        if (remove) sqlite3_finalize(remove);
        if (save_state) sqlite3_finalize(save_state);
        free(ids);
        return asgend_memory_internal_sqlite_error(db, error, error_size, "prepare chunk storage statements");
    }
    uint64_t seen = 0, created_files = 0, total_chunks = 0, skipped_count = 0;
    for (size_t i = 0; i < id_count; ++i) {
        sqlite3_reset(load); sqlite3_clear_bindings(load); sqlite3_bind_int64(load, 1, ids[i]);
        rc = sqlite3_step(load);
        if (rc != SQLITE_ROW) { if (rc != SQLITE_DONE) { asgend_memory_internal_sqlite_error(db, error, error_size, "read chunk source"); goto fail; } continue; }
        char *hash = column_dup(load, 0), *text = column_dup(load, 1), *path = column_dup(load, 4), *title = column_dup(load, 5), *description = column_dup(load, 6), *metadata_text = column_dup(load, 7);
        uint64_t source_start_byte = (uint64_t)sqlite3_column_int64(load, 2);
        uint64_t source_start_line = (uint64_t)sqlite3_column_int64(load, 3);
        const unsigned char *old_hash = sqlite3_column_text(load, 8), *old_version = sqlite3_column_text(load, 9), *old_status = sqlite3_column_text(load, 10);
        bool current = old_hash && old_version && old_status && strcmp((const char *)old_hash, hash ? hash : "") == 0 &&
                       strcmp((const char *)old_version, version) == 0 &&
                       (!strcmp((const char *)old_status, "complete") || !strcmp((const char *)old_status, "empty"));
        size_t text_len = text ? strlen(text) : 0;
        sqlite3_reset(load);
        if (!hash || !text || !path || !title || !description || !metadata_text) {
            free(hash); free(text); free(path); free(title); free(description); free(metadata_text);
            asgend_memory_internal_set_error(error, error_size, "out of memory loading chunk input"); goto fail;
        }
        ++seen;
        if (current) { ++skipped_count; free(hash); free(text); free(path); free(title); free(description); free(metadata_text); continue; }
        if (asgend_memory_internal_exec_sql(db, "BEGIN IMMEDIATE", error, error_size) != 0) {
            free(hash); free(text); free(path); free(title); free(description); free(metadata_text); goto fail;
        }
        sqlite3_reset(remove); sqlite3_clear_bindings(remove); sqlite3_bind_int64(remove, 1, ids[i]);
        if (sqlite3_step(remove) != SQLITE_DONE) {
            free(hash); free(text); free(path); free(title); free(description); free(metadata_text);
            asgend_memory_internal_sqlite_error(db, error, error_size, "remove stale chunks"); (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL); goto fail;
        }
        uint64_t file_chunk_count = 0;
        if (text_len && create_chunks_for_file(db, ids[i], hash, text, text_len, source_start_byte, source_start_line,
                                               path, title, description, metadata_text, max_chunk_tokens, version,
                                               &file_chunk_count, error, error_size) != 0) {
            free(hash); free(text); free(path); free(title); free(description); free(metadata_text);
            (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL); goto fail;
        }
        sqlite3_reset(save_state); sqlite3_clear_bindings(save_state);
        sqlite3_bind_int64(save_state, 1, ids[i]); sqlite3_bind_text(save_state, 2, hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(save_state, 3, version, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(save_state, 4, file_chunk_count ? "complete" : "empty", -1, SQLITE_STATIC);
        sqlite3_bind_int64(save_state, 5, (sqlite3_int64)file_chunk_count);
        if (sqlite3_step(save_state) != SQLITE_DONE || asgend_memory_internal_exec_sql(db, "COMMIT", error, error_size) != 0) {
            free(hash); free(text); free(path); free(title); free(description); free(metadata_text);
            (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL); asgend_memory_internal_sqlite_error(db, error, error_size, "persist chunk state"); goto fail;
        }
        ++created_files; total_chunks += file_chunk_count;
        free(hash); free(text); free(path); free(title); free(description); free(metadata_text);
    }
    sqlite3_finalize(load); sqlite3_finalize(remove); sqlite3_finalize(save_state); free(ids);
    if (files_seen) *files_seen = seen;
    if (files_chunked) *files_chunked = created_files;
    if (chunks_created) *chunks_created = total_chunks;
    if (skipped) *skipped = skipped_count;
    return 0;
fail:
    sqlite3_finalize(load); sqlite3_finalize(remove); sqlite3_finalize(save_state); free(ids);
    return -1;
}

int asgend_memory_chunk(const char *root, unsigned int max_chunk_tokens,
                        AsgendMemoryChunkResult *result, FILE *out,
                        char *error, size_t error_size) {
    if (error && error_size) error[0] = '\0';
    if (result) memset(result, 0, sizeof(*result));
    if (!out) { asgend_memory_internal_set_error(error, error_size, "output stream is required"); return -1; }
    if (!max_chunk_tokens) max_chunk_tokens = CHUNK_DEFAULT_TOKENS;
    if (max_chunk_tokens > CHUNK_MAX_TOKENS) { asgend_memory_internal_set_error(error, error_size, "chunk token size must be <= %u", CHUNK_MAX_TOKENS); return -1; }
    char *canonical = NULL, *index_file = NULL; sqlite3 *db = NULL;
    if (asgend_memory_internal_open_index(root, &canonical, &db, &index_file, error, error_size) != 0) return -1;
    free(canonical); free(index_file);
    AsgendMemoryChunkResult local = {0};
    int rc = asgend_memory_internal_sync_chunks(db, max_chunk_tokens, &local.files_seen, &local.files_chunked,
                                                 &local.chunks_created, &local.skipped, error, error_size);
    local.complete = rc == 0;
    if (rc == 0 && fprintf(out, "{\"status\":\"complete\",\"files_seen\":%llu,\"files_chunked\":%llu,\"chunks_created\":%llu,\"skipped\":%llu,\"max_chunk_tokens\":%u}\n",
                           (unsigned long long)local.files_seen, (unsigned long long)local.files_chunked,
                           (unsigned long long)local.chunks_created, (unsigned long long)local.skipped, max_chunk_tokens) < 0) {
        asgend_memory_internal_set_error(error, error_size, "could not write chunking result"); rc = -1;
    }
    sqlite3_close(db);
    if (result) { local.complete = rc == 0; *result = local; }
    return rc;
}
