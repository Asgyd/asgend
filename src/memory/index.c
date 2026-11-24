#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L
#include "asgend_memory.h"
#include "internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <limits.h>
#include <openssl/evp.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MEMORY_SCHEMA_VERSION 9
#define HASH_BUFFER_SIZE (64U * 1024U)
#define DEFAULT_EVENT_LIMIT 30U
#define MAX_EVENT_LIMIT 1000U

typedef struct {
    sqlite3 *db;
    sqlite3_stmt *seen_insert;
    sqlite3_stmt *lookup_path;
    const char *root_path;
    dev_t root_dev;
    uint64_t max_file_size;
    bool verify_hashes;
    bool exclude_resource_directories;
    int64_t scan_id;
    AsgendMemoryScanResult result;
    char *error;
    size_t error_size;
} ScanContext;

static void set_error(char *buffer, size_t size, const char *format, ...) {
    if (!buffer || size == 0) return;
    va_list args;
    va_start(args, format);
    (void)vsnprintf(buffer, size, format, args);
    va_end(args);
}

static int sqlite_error(sqlite3 *db, char *error, size_t error_size, const char *operation) {
    set_error(error, error_size, "%s: %s", operation, db ? sqlite3_errmsg(db) : "SQLite unavailable");
    return -1;
}

static int exec_sql(sqlite3 *db, const char *sql, char *error, size_t error_size) {
    char *message = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &message);
    if (rc == SQLITE_OK) return 0;
    set_error(error, error_size, "SQLite error: %s", message ? message : sqlite3_errmsg(db));
    sqlite3_free(message);
    return -1;
}

static char *path_join(const char *left, const char *right) {
    size_t left_len = strlen(left), right_len = strlen(right);
    bool slash = left_len > 0 && left[left_len - 1] != '/';
    if (left_len > SIZE_MAX - right_len - (slash ? 2U : 1U)) return NULL;
    size_t size = left_len + right_len + (slash ? 2U : 1U);
    char *joined = malloc(size);
    if (!joined) return NULL;
    memcpy(joined, left, left_len);
    size_t pos = left_len;
    if (slash) joined[pos++] = '/';
    memcpy(joined + pos, right, right_len);
    joined[pos + right_len] = '\0';
    return joined;
}

static char *relative_join(const char *prefix, const char *name) {
    if (!prefix || !prefix[0]) return strdup(name);
    return path_join(prefix, name);
}

static int64_t stat_mtime_ns(const struct stat *st) {
    return (int64_t)st->st_mtim.tv_sec * INT64_C(1000000000) + (int64_t)st->st_mtim.tv_nsec;
}

/* Linux mount IDs distinguish bind mounts even when st_dev is identical.
   On kernels/filesystems without STATX_MNT_ID, callers fall back to st_dev. */
static bool mount_id_for_fd(int fd, uint64_t *mount_id) {
#ifdef STATX_MNT_ID
    struct statx info;
    memset(&info, 0, sizeof(info));
    if (statx(fd, "", AT_EMPTY_PATH, STATX_MNT_ID, &info) == 0 && (info.stx_mask & STATX_MNT_ID)) {
        *mount_id = (uint64_t)info.stx_mnt_id;
        return true;
    }
#else
    (void)fd; (void)mount_id;
#endif
    return false;
}

static bool mount_id_at(int dirfd, const char *name, uint64_t *mount_id) {
#ifdef STATX_MNT_ID
    struct statx info;
    memset(&info, 0, sizeof(info));
    if (statx(dirfd, name, AT_SYMLINK_NOFOLLOW, STATX_MNT_ID, &info) == 0 && (info.stx_mask & STATX_MNT_ID)) {
        *mount_id = (uint64_t)info.stx_mnt_id;
        return true;
    }
#else
    (void)dirfd; (void)name; (void)mount_id;
#endif
    return false;
}

static const char *extension_for(const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base || !dot[1]) return "";
    return dot + 1;
}

static char *lower_extension(const char *path) {
    const char *extension = extension_for(path);
    size_t len = strlen(extension);
    if (len > 32) len = 32;
    char *value = malloc(len + 1);
    if (!value) return NULL;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)extension[i];
        value[i] = (char)((c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c);
    }
    value[len] = '\0';
    return value;
}

static const char *mime_for_extension(const char *extension) {
    if (!extension || !extension[0]) return "application/octet-stream";
    if (!strcmp(extension, "md") || !strcmp(extension, "markdown")) return "text/markdown";
    if (!strcmp(extension, "txt") || !strcmp(extension, "log")) return "text/plain";
    if (!strcmp(extension, "c") || !strcmp(extension, "h") || !strcmp(extension, "cc") || !strcmp(extension, "cpp") || !strcmp(extension, "hpp") || !strcmp(extension, "rs") || !strcmp(extension, "go") || !strcmp(extension, "java") || !strcmp(extension, "js") || !strcmp(extension, "ts") || !strcmp(extension, "py") || !strcmp(extension, "sh") || !strcmp(extension, "sql") || !strcmp(extension, "toml") || !strcmp(extension, "yaml") || !strcmp(extension, "yml")) return "text/plain";
    if (!strcmp(extension, "json")) return "application/json";
    if (!strcmp(extension, "xml") || !strcmp(extension, "html") || !strcmp(extension, "htm")) return "text/html";
    if (!strcmp(extension, "pdf")) return "application/pdf";
    if (!strcmp(extension, "png")) return "image/png";
    if (!strcmp(extension, "jpg") || !strcmp(extension, "jpeg")) return "image/jpeg";
    if (!strcmp(extension, "gif")) return "image/gif";
    if (!strcmp(extension, "svg")) return "image/svg+xml";
    if (!strcmp(extension, "zip")) return "application/zip";
    return "application/octet-stream";
}

static bool table_has_column(sqlite3 *db, const char *table, const char *column) {
    char sql[128];
    int n = snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
    if (n < 0 || (size_t)n >= sizeof(sql)) return false;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(stmt, 1);
        if (name && strcmp((const char *)name, column) == 0) { found = true; break; }
    }
    sqlite3_finalize(stmt);
    return found;
}

static int configure_db(sqlite3 *db, char *error, size_t error_size) {
    sqlite3_busy_timeout(db, 5000);
    if (exec_sql(db,
        "PRAGMA foreign_keys=ON;"
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=FULL;"
        "PRAGMA trusted_schema=OFF;",
        error, error_size) != 0) return -1;

    sqlite3_stmt *version_stmt = NULL;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &version_stmt, NULL) != SQLITE_OK)
        return sqlite_error(db, error, error_size, "read schema version");
    int rc = sqlite3_step(version_stmt);
    int version = rc == SQLITE_ROW ? sqlite3_column_int(version_stmt, 0) : -1;
    sqlite3_finalize(version_stmt);
    if (version < 0) return sqlite_error(db, error, error_size, "read schema version");
    if (version > MEMORY_SCHEMA_VERSION) {
        set_error(error, error_size, "memory database schema %d is newer than supported schema %d", version, MEMORY_SCHEMA_VERSION);
        return -1;
    }
    if (version == MEMORY_SCHEMA_VERSION) return 0;

    if (version == 0) {
    static const char schema[] =
        "BEGIN IMMEDIATE;"
        "CREATE TABLE IF NOT EXISTS scan_runs ("
        " scan_id INTEGER PRIMARY KEY AUTOINCREMENT, root_path TEXT NOT NULL,"
        " started_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),"
        " completed_at TEXT, status TEXT NOT NULL CHECK(status IN ('running','complete','partial','failed')),"
        " verify_mode INTEGER NOT NULL DEFAULT 0, max_file_size_bytes INTEGER NOT NULL,"
        " files_seen INTEGER NOT NULL DEFAULT 0, added_count INTEGER NOT NULL DEFAULT 0,"
        " modified_count INTEGER NOT NULL DEFAULT 0, deleted_count INTEGER NOT NULL DEFAULT 0,"
        " renamed_count INTEGER NOT NULL DEFAULT 0, restored_count INTEGER NOT NULL DEFAULT 0,"
        " file_errors INTEGER NOT NULL DEFAULT 0, walk_errors INTEGER NOT NULL DEFAULT 0,"
        " skipped_symlinks INTEGER NOT NULL DEFAULT 0, skipped_mountpoints INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS files ("
        " file_id INTEGER PRIMARY KEY AUTOINCREMENT, relative_path TEXT NOT NULL UNIQUE,"
        " extension TEXT NOT NULL DEFAULT '', mime_type TEXT NOT NULL DEFAULT 'application/octet-stream',"
        " file_size INTEGER NOT NULL, mtime_ns INTEGER NOT NULL, sha256 TEXT,"
        " status TEXT NOT NULL, last_seen_scan_id INTEGER NOT NULL DEFAULT 0,"
        " first_seen_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),"
        " updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),"
        " deleted_at TEXT, error_text TEXT);"
        "CREATE INDEX IF NOT EXISTS idx_files_sha_active ON files(sha256) WHERE deleted_at IS NULL;"
        "CREATE INDEX IF NOT EXISTS idx_files_scan_status ON files(last_seen_scan_id,status);"
        "CREATE INDEX IF NOT EXISTS idx_files_deleted ON files(deleted_at);"
        "CREATE TABLE IF NOT EXISTS file_snapshots ("
        " snapshot_id INTEGER PRIMARY KEY AUTOINCREMENT, scan_id INTEGER NOT NULL REFERENCES scan_runs(scan_id),"
        " file_id INTEGER NOT NULL REFERENCES files(file_id), relative_path TEXT NOT NULL,"
        " file_size INTEGER NOT NULL, mtime_ns INTEGER NOT NULL, sha256 TEXT, status TEXT NOT NULL,"
        " captured_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),"
        " UNIQUE(scan_id,relative_path));"
        "CREATE INDEX IF NOT EXISTS idx_snapshots_path_scan ON file_snapshots(relative_path,scan_id);"
        "CREATE TABLE IF NOT EXISTS file_events ("
        " event_id INTEGER PRIMARY KEY AUTOINCREMENT, scan_id INTEGER NOT NULL REFERENCES scan_runs(scan_id),"
        " file_id INTEGER REFERENCES files(file_id), event_type TEXT NOT NULL, old_path TEXT, new_path TEXT,"
        " sha256 TEXT, details TEXT, created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));"
        "CREATE INDEX IF NOT EXISTS idx_events_scan ON file_events(scan_id,event_id);"
        "CREATE INDEX IF NOT EXISTS idx_events_type ON file_events(event_type,created_at);"
        "CREATE TABLE IF NOT EXISTS schema_migrations (version INTEGER PRIMARY KEY, applied_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')), description TEXT NOT NULL);"
        "INSERT OR IGNORE INTO schema_migrations(version,description) VALUES(1,'M1 recursive file scan and hash snapshots');"
        "PRAGMA user_version=1;"
        "COMMIT;";
    if (exec_sql(db, schema, error, error_size) != 0) {
        (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    version = 1;
    }

    /* Migrate v1 scan/state databases without discarding existing file IDs/events. */
    if (version < 2) {
    static const char migration_v2[] =
        "BEGIN IMMEDIATE;"
        "CREATE TABLE IF NOT EXISTS file_contents ("
        " file_id INTEGER PRIMARY KEY REFERENCES files(file_id) ON DELETE CASCADE,"
        " source_sha256 TEXT NOT NULL, content_text TEXT, frontmatter_raw TEXT,"
        " extractor_id TEXT NOT NULL, extract_status TEXT NOT NULL,"
        " metadata_status TEXT NOT NULL DEFAULT 'not_applicable',"
        " source_bytes INTEGER NOT NULL DEFAULT 0, extracted_bytes INTEGER NOT NULL DEFAULT 0,"
        " extracted_chars INTEGER NOT NULL DEFAULT 0, error_text TEXT,"
        " extracted_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));"
        "CREATE INDEX IF NOT EXISTS idx_file_contents_status ON file_contents(extract_status,metadata_status);"
        "CREATE INDEX IF NOT EXISTS idx_file_contents_hash ON file_contents(source_sha256);"
        "CREATE TABLE IF NOT EXISTS file_metadata ("
        " file_id INTEGER NOT NULL REFERENCES files(file_id) ON DELETE CASCADE,"
        " metadata_key TEXT NOT NULL, value_json TEXT NOT NULL, value_type TEXT NOT NULL,"
        " source TEXT NOT NULL DEFAULT 'okf_frontmatter', source_sha256 TEXT NOT NULL,"
        " updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')) ,"
        " PRIMARY KEY(file_id,metadata_key));"
        "CREATE INDEX IF NOT EXISTS idx_file_metadata_key ON file_metadata(metadata_key);"
        "INSERT OR IGNORE INTO schema_migrations(version,description) VALUES(2,'M2 safe text extraction and read-only OKF frontmatter indexing');"
        "PRAGMA user_version=2;"
        "COMMIT;";
    if (exec_sql(db, migration_v2, error, error_size) != 0) {
        (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    version = 2;
    }

    /* M3: durable, model-specific pipeline jobs and immutable model outputs. */
    if (version < 3) {
    static const char migration_v3[] =
        "BEGIN IMMEDIATE;"
        "CREATE TABLE IF NOT EXISTS pipeline_jobs ("
        " job_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " file_id INTEGER NOT NULL REFERENCES files(file_id) ON DELETE CASCADE,"
        " stage TEXT NOT NULL CHECK(stage IN ('decision','describe','embedding')) ,"
        " source_sha256 TEXT NOT NULL, model_id TEXT NOT NULL, prompt_version TEXT NOT NULL,"
        " state TEXT NOT NULL CHECK(state IN ('queued','running','succeeded','failed','stale')) ,"
        " attempt_count INTEGER NOT NULL DEFAULT 0, next_attempt_at TEXT, error_text TEXT,"
        " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')) ,"
        " started_at TEXT, finished_at TEXT,"
        " updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')) ,"
        " UNIQUE(file_id,stage,source_sha256,model_id,prompt_version));"
        "CREATE INDEX IF NOT EXISTS idx_pipeline_jobs_state ON pipeline_jobs(stage,state,next_attempt_at);"
        "CREATE INDEX IF NOT EXISTS idx_pipeline_jobs_file ON pipeline_jobs(file_id,stage,source_sha256);"
        "CREATE TABLE IF NOT EXISTS model_metadata ("
        " metadata_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " file_id INTEGER NOT NULL REFERENCES files(file_id) ON DELETE CASCADE,"
        " source_sha256 TEXT NOT NULL, model_id TEXT NOT NULL, prompt_version TEXT NOT NULL,"
        " metadata_json TEXT NOT NULL CHECK(json_valid(metadata_json)),"
        " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')) ,"
        " UNIQUE(file_id,source_sha256,model_id,prompt_version));"
        "CREATE INDEX IF NOT EXISTS idx_model_metadata_hash ON model_metadata(file_id,source_sha256);"
        "CREATE TABLE IF NOT EXISTS descriptions ("
        " description_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " file_id INTEGER NOT NULL REFERENCES files(file_id) ON DELETE CASCADE,"
        " source_sha256 TEXT NOT NULL, model_id TEXT NOT NULL, prompt_version TEXT NOT NULL,"
        " title TEXT NOT NULL, description TEXT NOT NULL, status TEXT NOT NULL DEFAULT 'complete' CHECK(status IN ('complete','stale')),"
        " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')) ,"
        " UNIQUE(file_id,source_sha256,model_id,prompt_version));"
        "CREATE INDEX IF NOT EXISTS idx_descriptions_hash ON descriptions(file_id,source_sha256);"
        "INSERT OR IGNORE INTO schema_migrations(version,description) VALUES(3,'Resumable metadata/title/description model pipeline');"
        "PRAGMA user_version=3;"
        "COMMIT;";
    if (exec_sql(db, migration_v3, error, error_size) != 0) {
        (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    version = 3;
    }
    /* M3: persistent vectors and FTS5 BM25. Keep each SQL batch below ISO C's minimum supported literal size. */
    if (version < 4) {
        static const char migration_v4_tables[] =
            "CREATE TABLE IF NOT EXISTS embeddings ("
            " embedding_id INTEGER PRIMARY KEY AUTOINCREMENT,"
            " description_id INTEGER NOT NULL REFERENCES descriptions(description_id) ON DELETE CASCADE,"
            " file_id INTEGER NOT NULL REFERENCES files(file_id) ON DELETE CASCADE,"
            " input_sha256 TEXT NOT NULL, model_id TEXT NOT NULL, dimensions INTEGER NOT NULL CHECK(dimensions>0 AND dimensions<=65536),"
            " vector_json TEXT NOT NULL CHECK(json_valid(vector_json)),"
            " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')) ,"
            " UNIQUE(description_id,input_sha256,model_id));"
            "CREATE INDEX IF NOT EXISTS idx_embeddings_model_file ON embeddings(model_id,file_id);"
            "CREATE INDEX IF NOT EXISTS idx_embeddings_desc ON embeddings(description_id,input_sha256);"
            "CREATE TABLE IF NOT EXISTS memory_search_docs ("
            " file_id INTEGER PRIMARY KEY REFERENCES files(file_id) ON DELETE CASCADE,"
            " relative_path TEXT NOT NULL, title TEXT NOT NULL DEFAULT '', description TEXT NOT NULL DEFAULT '',"
            " content_text TEXT NOT NULL DEFAULT '', metadata_text TEXT NOT NULL DEFAULT '');";
        static const char migration_v4_fts[] =
            "CREATE VIRTUAL TABLE IF NOT EXISTS memory_fts USING fts5(relative_path,title,description,content_text,metadata_text,"
            " content='memory_search_docs',content_rowid='file_id',tokenize='unicode61 remove_diacritics 2');"
            "CREATE TRIGGER IF NOT EXISTS memory_search_docs_ai AFTER INSERT ON memory_search_docs BEGIN "
            " INSERT INTO memory_fts(rowid,relative_path,title,description,content_text,metadata_text) VALUES(NEW.file_id,NEW.relative_path,NEW.title,NEW.description,NEW.content_text,NEW.metadata_text); END;"
            "CREATE TRIGGER IF NOT EXISTS memory_search_docs_ad AFTER DELETE ON memory_search_docs BEGIN "
            " INSERT INTO memory_fts(memory_fts,rowid,relative_path,title,description,content_text,metadata_text) VALUES('delete',OLD.file_id,OLD.relative_path,OLD.title,OLD.description,OLD.content_text,OLD.metadata_text); END;"
            "CREATE TRIGGER IF NOT EXISTS memory_search_docs_au AFTER UPDATE ON memory_search_docs BEGIN "
            " INSERT INTO memory_fts(memory_fts,rowid,relative_path,title,description,content_text,metadata_text) VALUES('delete',OLD.file_id,OLD.relative_path,OLD.title,OLD.description,OLD.content_text,OLD.metadata_text);"
            " INSERT INTO memory_fts(rowid,relative_path,title,description,content_text,metadata_text) VALUES(NEW.file_id,NEW.relative_path,NEW.title,NEW.description,NEW.content_text,NEW.metadata_text); END;";
        static const char migration_v4_view[] =
            "CREATE VIEW IF NOT EXISTS memory_search_source AS SELECT f.file_id,f.relative_path,"
            "COALESCE((SELECT d.title FROM descriptions d WHERE d.file_id=f.file_id AND d.status='complete' AND d.source_sha256=f.sha256 ORDER BY d.created_at DESC,d.description_id DESC LIMIT 1),'') AS title,"
            "COALESCE((SELECT d.description FROM descriptions d WHERE d.file_id=f.file_id AND d.status='complete' AND d.source_sha256=f.sha256 ORDER BY d.created_at DESC,d.description_id DESC LIMIT 1),'') AS description,"
            "COALESCE((SELECT c.content_text FROM file_contents c WHERE c.file_id=f.file_id AND c.source_sha256=f.sha256 LIMIT 1),'') AS content_text,"
            "COALESCE((SELECT group_concat(m.metadata_key||' '||m.value_json,' ') FROM file_metadata m WHERE m.file_id=f.file_id AND m.source_sha256=f.sha256),'') AS metadata_text FROM files f;";
        static const char migration_v4_sync_files[] =
            "CREATE TRIGGER IF NOT EXISTS files_search_ai AFTER INSERT ON files BEGIN "
            "INSERT INTO memory_search_docs(file_id,relative_path,title,description,content_text,metadata_text) SELECT * FROM memory_search_source WHERE file_id=NEW.file_id "
            "ON CONFLICT(file_id) DO UPDATE SET relative_path=excluded.relative_path,title=excluded.title,description=excluded.description,content_text=excluded.content_text,metadata_text=excluded.metadata_text; END;"
            "CREATE TRIGGER IF NOT EXISTS files_search_au AFTER UPDATE OF relative_path,sha256,deleted_at ON files BEGIN "
            "INSERT INTO memory_search_docs(file_id,relative_path,title,description,content_text,metadata_text) SELECT * FROM memory_search_source WHERE file_id=NEW.file_id "
            "ON CONFLICT(file_id) DO UPDATE SET relative_path=excluded.relative_path,title=excluded.title,description=excluded.description,content_text=excluded.content_text,metadata_text=excluded.metadata_text; END;";
        static const char migration_v4_sync_contents[] =
            "CREATE TRIGGER IF NOT EXISTS contents_search_ai AFTER INSERT ON file_contents BEGIN "
            "INSERT INTO memory_search_docs(file_id,relative_path,title,description,content_text,metadata_text) SELECT * FROM memory_search_source WHERE file_id=NEW.file_id "
            "ON CONFLICT(file_id) DO UPDATE SET relative_path=excluded.relative_path,title=excluded.title,description=excluded.description,content_text=excluded.content_text,metadata_text=excluded.metadata_text; END;"
            "CREATE TRIGGER IF NOT EXISTS contents_search_au AFTER UPDATE ON file_contents BEGIN "
            "INSERT INTO memory_search_docs(file_id,relative_path,title,description,content_text,metadata_text) SELECT * FROM memory_search_source WHERE file_id=NEW.file_id "
            "ON CONFLICT(file_id) DO UPDATE SET relative_path=excluded.relative_path,title=excluded.title,description=excluded.description,content_text=excluded.content_text,metadata_text=excluded.metadata_text; END;";
        static const char migration_v4_sync_metadata[] =
            "CREATE TRIGGER IF NOT EXISTS metadata_search_ai AFTER INSERT ON file_metadata BEGIN "
            "INSERT INTO memory_search_docs(file_id,relative_path,title,description,content_text,metadata_text) SELECT * FROM memory_search_source WHERE file_id=NEW.file_id "
            "ON CONFLICT(file_id) DO UPDATE SET relative_path=excluded.relative_path,title=excluded.title,description=excluded.description,content_text=excluded.content_text,metadata_text=excluded.metadata_text; END;"
            "CREATE TRIGGER IF NOT EXISTS metadata_search_au AFTER UPDATE ON file_metadata BEGIN "
            "INSERT INTO memory_search_docs(file_id,relative_path,title,description,content_text,metadata_text) SELECT * FROM memory_search_source WHERE file_id=NEW.file_id "
            "ON CONFLICT(file_id) DO UPDATE SET relative_path=excluded.relative_path,title=excluded.title,description=excluded.description,content_text=excluded.content_text,metadata_text=excluded.metadata_text; END;"
            "CREATE TRIGGER IF NOT EXISTS metadata_search_ad AFTER DELETE ON file_metadata BEGIN "
            "INSERT INTO memory_search_docs(file_id,relative_path,title,description,content_text,metadata_text) SELECT * FROM memory_search_source WHERE file_id=OLD.file_id "
            "ON CONFLICT(file_id) DO UPDATE SET relative_path=excluded.relative_path,title=excluded.title,description=excluded.description,content_text=excluded.content_text,metadata_text=excluded.metadata_text; END;";
        static const char migration_v4_sync_descriptions[] =
            "CREATE TRIGGER IF NOT EXISTS descriptions_search_ai AFTER INSERT ON descriptions BEGIN "
            "INSERT INTO memory_search_docs(file_id,relative_path,title,description,content_text,metadata_text) SELECT * FROM memory_search_source WHERE file_id=NEW.file_id "
            "ON CONFLICT(file_id) DO UPDATE SET relative_path=excluded.relative_path,title=excluded.title,description=excluded.description,content_text=excluded.content_text,metadata_text=excluded.metadata_text; END;"
            "CREATE TRIGGER IF NOT EXISTS descriptions_search_au AFTER UPDATE ON descriptions BEGIN "
            "INSERT INTO memory_search_docs(file_id,relative_path,title,description,content_text,metadata_text) SELECT * FROM memory_search_source WHERE file_id=NEW.file_id "
            "ON CONFLICT(file_id) DO UPDATE SET relative_path=excluded.relative_path,title=excluded.title,description=excluded.description,content_text=excluded.content_text,metadata_text=excluded.metadata_text; END;";
        static const char migration_v4_populate[] =
            "INSERT INTO memory_search_docs(file_id,relative_path,title,description,content_text,metadata_text) "
            "SELECT * FROM memory_search_source WHERE 1 ON CONFLICT(file_id) DO UPDATE SET relative_path=excluded.relative_path,title=excluded.title,description=excluded.description,content_text=excluded.content_text,metadata_text=excluded.metadata_text;"
            "INSERT INTO memory_fts(memory_fts) VALUES('rebuild');"
            "INSERT OR IGNORE INTO schema_migrations(version,description) VALUES(4,'Embedding vectors, indexed project-memory BM25 and hybrid query');"
            "PRAGMA user_version=4;";
        if (exec_sql(db, "BEGIN IMMEDIATE", error, error_size) != 0) return -1;
        if (exec_sql(db, migration_v4_tables, error, error_size) != 0 ||
            exec_sql(db, migration_v4_fts, error, error_size) != 0 ||
            exec_sql(db, migration_v4_view, error, error_size) != 0 ||
            exec_sql(db, migration_v4_sync_files, error, error_size) != 0 ||
            exec_sql(db, migration_v4_sync_contents, error, error_size) != 0 ||
            exec_sql(db, migration_v4_sync_metadata, error, error_size) != 0 ||
            exec_sql(db, migration_v4_sync_descriptions, error, error_size) != 0 ||
            exec_sql(db, migration_v4_populate, error, error_size) != 0 ||
            exec_sql(db, "COMMIT", error, error_size) != 0) {
            (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            return -1;
        }
        version = 4;
    }
    /* M4: persistent dependency graph between indexed local files. */
    if (version < 5) {
        static const char migration_v5[] =
            "BEGIN IMMEDIATE;"
            "CREATE TABLE IF NOT EXISTS file_edges ("
            " edge_id INTEGER PRIMARY KEY AUTOINCREMENT,"
            " source_file_id INTEGER NOT NULL REFERENCES files(file_id) ON DELETE CASCADE,"
            " target_file_id INTEGER NOT NULL REFERENCES files(file_id) ON DELETE CASCADE,"
            " edge_type TEXT NOT NULL CHECK(edge_type IN ('markdown_link','wiki_link','source_include','source_import')),"
            " reference_text TEXT NOT NULL, source_sha256 TEXT NOT NULL,"
            " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),"
            " UNIQUE(source_file_id,target_file_id,edge_type,reference_text));"
            "CREATE INDEX IF NOT EXISTS idx_file_edges_source ON file_edges(source_file_id,edge_type);"
            "CREATE INDEX IF NOT EXISTS idx_file_edges_target ON file_edges(target_file_id,edge_type);"
            "INSERT OR IGNORE INTO schema_migrations(version,description) VALUES(5,'Local dependency graph for Markdown links, includes and relative imports');"
            "PRAGMA user_version=5;"
            "COMMIT;";
        if (exec_sql(db, migration_v5, error, error_size) != 0) {
            (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            return -1;
        }
        version = 5;
    }
    /* M5: source-aware chunks and an FTS index for fragment-level retrieval. */
    if (version < 6) {
        static const char migration_v6_tables[] =
            "BEGIN IMMEDIATE;"
            "CREATE TABLE IF NOT EXISTS file_chunks ("
            " chunk_id INTEGER PRIMARY KEY AUTOINCREMENT,"
            " file_id INTEGER NOT NULL REFERENCES files(file_id) ON DELETE CASCADE,"
            " source_sha256 TEXT NOT NULL, chunk_index INTEGER NOT NULL,"
            " start_byte INTEGER NOT NULL, end_byte INTEGER NOT NULL,"
            " start_line INTEGER NOT NULL, end_line INTEGER NOT NULL,"
            " chunk_text TEXT NOT NULL, relative_path TEXT NOT NULL, title TEXT NOT NULL DEFAULT '',"
            " description TEXT NOT NULL DEFAULT '', metadata_text TEXT NOT NULL DEFAULT '',"
            " chunker_version TEXT NOT NULL,"
            " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')) ,"
            " UNIQUE(file_id,source_sha256,chunker_version,chunk_index));"
            "CREATE INDEX IF NOT EXISTS idx_file_chunks_source ON file_chunks(file_id,source_sha256);"
            "CREATE TABLE IF NOT EXISTS file_chunk_state ("
            " file_id INTEGER PRIMARY KEY REFERENCES files(file_id) ON DELETE CASCADE,"
            " source_sha256 TEXT NOT NULL, chunker_version TEXT NOT NULL,"
            " status TEXT NOT NULL CHECK(status IN ('complete','empty','failed')),"
            " chunk_count INTEGER NOT NULL DEFAULT 0, error_text TEXT,"
            " updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));"
            "CREATE INDEX IF NOT EXISTS idx_chunk_state_source ON file_chunk_state(source_sha256,chunker_version);"
            "CREATE VIRTUAL TABLE IF NOT EXISTS memory_chunk_fts USING fts5(relative_path,title,description,chunk_text,metadata_text,"
            " content='file_chunks',content_rowid='chunk_id',tokenize='unicode61 remove_diacritics 2');";
        static const char migration_v6_triggers[] =
            "CREATE TRIGGER IF NOT EXISTS file_chunks_ai AFTER INSERT ON file_chunks BEGIN "
            " INSERT INTO memory_chunk_fts(rowid,relative_path,title,description,chunk_text,metadata_text) "
            " VALUES(NEW.chunk_id,NEW.relative_path,NEW.title,NEW.description,NEW.chunk_text,NEW.metadata_text); END;"
            "CREATE TRIGGER IF NOT EXISTS file_chunks_ad AFTER DELETE ON file_chunks BEGIN "
            " INSERT INTO memory_chunk_fts(memory_chunk_fts,rowid,relative_path,title,description,chunk_text,metadata_text) "
            " VALUES('delete',OLD.chunk_id,OLD.relative_path,OLD.title,OLD.description,OLD.chunk_text,OLD.metadata_text); END;"
            "CREATE TRIGGER IF NOT EXISTS file_chunks_au AFTER UPDATE ON file_chunks BEGIN "
            " INSERT INTO memory_chunk_fts(memory_chunk_fts,rowid,relative_path,title,description,chunk_text,metadata_text) "
            " VALUES('delete',OLD.chunk_id,OLD.relative_path,OLD.title,OLD.description,OLD.chunk_text,OLD.metadata_text);"
            " INSERT INTO memory_chunk_fts(rowid,relative_path,title,description,chunk_text,metadata_text) "
            " VALUES(NEW.chunk_id,NEW.relative_path,NEW.title,NEW.description,NEW.chunk_text,NEW.metadata_text); END;"
            "CREATE TRIGGER IF NOT EXISTS search_docs_chunks_ai AFTER INSERT ON memory_search_docs BEGIN "
            " UPDATE file_chunks SET relative_path=NEW.relative_path,title=NEW.title,description=NEW.description,metadata_text=NEW.metadata_text WHERE file_id=NEW.file_id; END;"
            "CREATE TRIGGER IF NOT EXISTS search_docs_chunks_au AFTER UPDATE ON memory_search_docs BEGIN "
            " UPDATE file_chunks SET relative_path=NEW.relative_path,title=NEW.title,description=NEW.description,metadata_text=NEW.metadata_text WHERE file_id=NEW.file_id; END;"
            "CREATE TRIGGER IF NOT EXISTS search_docs_chunks_ad AFTER DELETE ON memory_search_docs BEGIN "
            " DELETE FROM file_chunks WHERE file_id=OLD.file_id; DELETE FROM file_chunk_state WHERE file_id=OLD.file_id; END;"
            "INSERT OR IGNORE INTO schema_migrations(version,description) VALUES(6,'Source-aware document chunks, line/byte coordinates and chunk-level FTS5');"
            "PRAGMA user_version=6;"
            "COMMIT;";
        if (exec_sql(db, migration_v6_tables, error, error_size) != 0 ||
            exec_sql(db, migration_v6_triggers, error, error_size) != 0) {
            (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            return -1;
        }
        version = 6;
    }
    /* M6: preserve body offsets so fragment coordinates map back to the source file. */
    if (version < 7) {
        static const char migration_v7[] =
            "BEGIN IMMEDIATE;"
            "ALTER TABLE file_contents ADD COLUMN body_start_byte INTEGER NOT NULL DEFAULT 0;"
            "ALTER TABLE file_contents ADD COLUMN body_start_line INTEGER NOT NULL DEFAULT 1;"
            "INSERT OR IGNORE INTO schema_migrations(version,description) VALUES(7,'Original source offsets for extracted Markdown bodies and source-aware chunks');"
            "PRAGMA user_version=7;"
            "COMMIT;";
        if (exec_sql(db, migration_v7, error, error_size) != 0) {
            (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            return -1;
        }
        version = 7;
    }
    /* M7: per-chunk vector embeddings for source-aware semantic retrieval. */
    if (version < 8) {
        static const char migration_v8[] =
            "BEGIN IMMEDIATE;"
            "CREATE TABLE IF NOT EXISTS chunk_embeddings ("
            " embedding_id INTEGER PRIMARY KEY AUTOINCREMENT,"
            " chunk_id INTEGER NOT NULL REFERENCES file_chunks(chunk_id) ON DELETE CASCADE,"
            " file_id INTEGER NOT NULL REFERENCES files(file_id) ON DELETE CASCADE,"
            " source_sha256 TEXT NOT NULL, input_sha256 TEXT NOT NULL, model_id TEXT NOT NULL,"
            " dimensions INTEGER NOT NULL CHECK(dimensions BETWEEN 1 AND 65536),"
            " vector_json TEXT NOT NULL CHECK(json_valid(vector_json)),"
            " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')) ,"
            " UNIQUE(chunk_id,input_sha256,model_id));"
            "CREATE INDEX IF NOT EXISTS idx_chunk_embeddings_model ON chunk_embeddings(model_id,file_id,chunk_id);"
            "CREATE INDEX IF NOT EXISTS idx_chunk_embeddings_source ON chunk_embeddings(chunk_id,source_sha256,input_sha256);"
            "INSERT OR IGNORE INTO schema_migrations(version,description) VALUES(8,'Chunk-level embeddings for source-aware semantic retrieval');"
            "PRAGMA user_version=8;"
            "COMMIT;";
        if (exec_sql(db, migration_v8, error, error_size) != 0) {
            (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            return -1;
        }
        version = 8;
    }
    /* M8: persistent random-hyperplane LSH signatures for approximate nearest-neighbour retrieval. */
    if (version < 9) {
        if (exec_sql(db, "BEGIN IMMEDIATE", error, error_size) != 0) return -1;
        if (!table_has_column(db, "embeddings", "ann_signature") &&
            exec_sql(db, "ALTER TABLE embeddings ADD COLUMN ann_signature TEXT", error, error_size) != 0) {
            (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL); return -1;
        }
        if (!table_has_column(db, "chunk_embeddings", "ann_signature") &&
            exec_sql(db, "ALTER TABLE chunk_embeddings ADD COLUMN ann_signature TEXT", error, error_size) != 0) {
            (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL); return -1;
        }
        static const char migration_v9_tail[] =
            "CREATE INDEX IF NOT EXISTS idx_embeddings_ann_model_signature ON embeddings(model_id,ann_signature);"
            "CREATE INDEX IF NOT EXISTS idx_chunk_embeddings_ann_model_signature ON chunk_embeddings(model_id,ann_signature);"
            "INSERT OR IGNORE INTO schema_migrations(version,description) VALUES(9,'Approximate vector retrieval using persistent random-hyperplane LSH signatures');"
            "PRAGMA user_version=9; COMMIT;";
        if (exec_sql(db, migration_v9_tail, error, error_size) != 0) {
            (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL); return -1;
        }
        version = 9;
    }
    return version == MEMORY_SCHEMA_VERSION ? 0 : -1;
}

static int open_index(const char *root, char **canonical_out, sqlite3 **db_out,
                      char **index_file_out, char *error, size_t error_size) {
    if (!root || !root[0]) {
        set_error(error, error_size, "root path is required");
        return -1;
    }
    char *canonical = realpath(root, NULL);
    if (!canonical) {
        set_error(error, error_size, "cannot resolve root '%s': %s", root, strerror(errno));
        return -1;
    }
    struct stat root_st;
    if (stat(canonical, &root_st) != 0 || !S_ISDIR(root_st.st_mode)) {
        set_error(error, error_size, "root must be an existing directory: %s", canonical);
        free(canonical);
        return -1;
    }
    char *mem_dir = path_join(canonical, ".asgend_mem");
    if (!mem_dir) {
        set_error(error, error_size, "out of memory while resolving memory directory");
        free(canonical);
        return -1;
    }
    struct stat mem_st;
    if (lstat(mem_dir, &mem_st) != 0) {
        if (errno != ENOENT || mkdir(mem_dir, 0700) != 0) {
            set_error(error, error_size, "cannot create memory directory '%s': %s", mem_dir, strerror(errno));
            free(mem_dir); free(canonical); return -1;
        }
        if (lstat(mem_dir, &mem_st) != 0) {
            set_error(error, error_size, "cannot inspect memory directory '%s': %s", mem_dir, strerror(errno));
            free(mem_dir); free(canonical); return -1;
        }
    }
    if (!S_ISDIR(mem_st.st_mode) || S_ISLNK(mem_st.st_mode)) {
        set_error(error, error_size, "memory path must be a real directory, not a symlink: %s", mem_dir);
        free(mem_dir); free(canonical); return -1;
    }
    (void)chmod(mem_dir, 0700);
    char *index_file = path_join(mem_dir, "index.sqlite3");
    free(mem_dir);
    if (!index_file) {
        set_error(error, error_size, "out of memory while resolving database path");
        free(canonical); return -1;
    }
    struct stat db_st;
    int db_lstat = lstat(index_file, &db_st);
    if (db_lstat == 0 && S_ISLNK(db_st.st_mode)) {
        set_error(error, error_size, "memory database path must not be a symlink: %s", index_file);
        free(index_file); free(canonical); return -1;
    }
    if (db_lstat == 0 && !S_ISREG(db_st.st_mode)) {
        set_error(error, error_size, "memory database path must be a regular file: %s", index_file);
        free(index_file); free(canonical); return -1;
    }
    if (db_lstat != 0 && errno != ENOENT) {
        set_error(error, error_size, "cannot inspect memory database '%s': %s", index_file, strerror(errno));
        free(index_file); free(canonical); return -1;
    }
    sqlite3 *db = NULL;
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    int rc = sqlite3_open_v2(index_file, &db, flags, NULL);
    if (rc != SQLITE_OK) {
        set_error(error, error_size, "cannot open memory database '%s': %s", index_file, db ? sqlite3_errmsg(db) : "SQLite open failed");
        if (db) sqlite3_close(db);
        free(index_file); free(canonical); return -1;
    }
    (void)chmod(index_file, 0600);
    if (configure_db(db, error, error_size) != 0) {
        sqlite3_close(db); free(index_file); free(canonical); return -1;
    }
    *canonical_out = canonical;
    *db_out = db;
    *index_file_out = index_file;
    return 0;
}

int asgend_memory_internal_open_index(const char *root, char **canonical_out,
                                      sqlite3 **db_out, char **index_file_out,
                                      char *error, size_t error_size) {
    return open_index(root, canonical_out, db_out, index_file_out, error, error_size);
}

void asgend_memory_internal_set_error(char *buffer, size_t size,
                                      const char *format, ...) {
    if (!buffer || size == 0) return;
    va_list args;
    va_start(args, format);
    (void)vsnprintf(buffer, size, format, args);
    va_end(args);
}

int asgend_memory_internal_sqlite_error(sqlite3 *db, char *error,
                                        size_t error_size, const char *operation) {
    return sqlite_error(db, error, error_size, operation);
}

int asgend_memory_internal_exec_sql(sqlite3 *db, const char *sql,
                                    char *error, size_t error_size) {
    return exec_sql(db, sql, error, error_size);
}

static int hash_open_file(int dirfd, const char *name, const struct stat *initial,
                          uint64_t max_file_size, char output[65], char *error, size_t error_size) {
    int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        set_error(error, error_size, "open failed: %s", strerror(errno));
        return -1;
    }
    struct stat before;
    if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) || before.st_dev != initial->st_dev || before.st_ino != initial->st_ino) {
        set_error(error, error_size, "file changed while opening or is not a regular file");
        close(fd); return -1;
    }
    if (before.st_size != initial->st_size || stat_mtime_ns(&before) != stat_mtime_ns(initial)) {
        set_error(error, error_size, "file changed while scanning; retry on next scan");
        close(fd); return -1;
    }
    if (before.st_size < 0 || (uint64_t)before.st_size > max_file_size) {
        set_error(error, error_size, "file exceeds configured size limit");
        close(fd); return -2;
    }
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        set_error(error, error_size, "cannot allocate SHA-256 context");
        close(fd); return -1;
    }
    int ok = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1;
    unsigned char buffer[HASH_BUFFER_SIZE];
    uint64_t total = 0;
    while (ok) {
        ssize_t got = read(fd, buffer, sizeof(buffer));
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) {
            set_error(error, error_size, "read failed: %s", strerror(errno));
            ok = 0; break;
        }
        if (got == 0) break;
        if ((uint64_t)got > max_file_size - total) {
            set_error(error, error_size, "file grew beyond configured size limit while hashing");
            close(fd); EVP_MD_CTX_free(ctx); return -2;
        }
        total += (uint64_t)got;
        if (EVP_DigestUpdate(ctx, buffer, (size_t)got) != 1) {
            set_error(error, error_size, "SHA-256 update failed");
            ok = 0; break;
        }
    }
    struct stat after;
    if (ok && (fstat(fd, &after) != 0 || after.st_size != before.st_size || stat_mtime_ns(&after) != stat_mtime_ns(&before) || (uint64_t)after.st_size != total)) {
        set_error(error, error_size, "file changed while hashing; retry on next scan");
        ok = 0;
    }
    close(fd);
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (ok && EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1) {
        set_error(error, error_size, "SHA-256 finalization failed");
        ok = 0;
    }
    EVP_MD_CTX_free(ctx);
    if (!ok || digest_len != 32) return -1;
    static const char hex[] = "0123456789abcdef";
    for (unsigned int i = 0; i < digest_len; ++i) {
        output[i * 2] = hex[digest[i] >> 4];
        output[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    output[64] = '\0';
    return 0;
}

static int lookup_file(ScanContext *ctx, const char *relative_path,
                       bool *exists, bool *active, int64_t *file_id,
                       int64_t *size, int64_t *mtime_ns, char hash[65],
                       bool *hash_valid, char old_status[24]) {
    sqlite3_stmt *stmt = ctx->lookup_path;
    sqlite3_reset(stmt); sqlite3_clear_bindings(stmt);
    sqlite3_bind_text(stmt, 1, relative_path, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    *exists = false; *active = false; *file_id = 0; *size = 0; *mtime_ns = 0;
    *hash_valid = false; hash[0] = '\0'; old_status[0] = '\0';
    if (rc == SQLITE_ROW) {
        *exists = true;
        *file_id = sqlite3_column_int64(stmt, 0);
        *size = sqlite3_column_int64(stmt, 1);
        *mtime_ns = sqlite3_column_int64(stmt, 2);
        const unsigned char *hash_text = sqlite3_column_text(stmt, 3);
        const unsigned char *status_text = sqlite3_column_text(stmt, 4);
        const unsigned char *deleted_text = sqlite3_column_text(stmt, 5);
        if (hash_text && strlen((const char *)hash_text) == 64) {
            snprintf(hash, 65, "%s", (const char *)hash_text); *hash_valid = true;
        }
        if (status_text) snprintf(old_status, 24, "%s", (const char *)status_text);
        *active = deleted_text == NULL;
    } else if (rc != SQLITE_DONE) {
        sqlite3_reset(stmt);
        return sqlite_error(ctx->db, ctx->error, ctx->error_size, "lookup indexed path");
    }
    sqlite3_reset(stmt);
    return 0;
}

static int stage_file(ScanContext *ctx, const char *relative_path, const char *extension,
                      const char *mime_type, const struct stat *st, int dirfd, const char *name) {
    bool existed = false, active = false, hash_valid = false;
    int64_t old_file_id = 0, old_size = 0, old_mtime = 0;
    char old_hash[65], old_status[24];
    if (lookup_file(ctx, relative_path, &existed, &active, &old_file_id, &old_size,
                    &old_mtime, old_hash, &hash_valid, old_status) != 0) return -1;
    (void)active; (void)old_file_id;

    const int64_t size = st->st_size < 0 ? 0 : (int64_t)st->st_size;
    const int64_t mtime_ns = stat_mtime_ns(st);
    const char *status = "indexed";
    const char *error_text = NULL;
    char hash[65] = {0};
    bool hash_ready = false;

    if (st->st_size < 0 || (uint64_t)st->st_size > ctx->max_file_size) {
        status = "too_large";
        error_text = "file exceeds configured size limit";
    } else if (!ctx->verify_hashes && existed && old_size == size && old_mtime == mtime_ns && hash_valid &&
               strcmp(old_status, "read_error") != 0) {
        /* Fast path uses size/mtime as hints. `--verify` forces a full digest pass. */
        memcpy(hash, old_hash, sizeof(hash));
        hash_ready = true;
        if (!strcmp(old_status, "too_large")) status = "too_large";
    } else {
        char hash_error[ASGEND_MEMORY_ERROR_MAX] = {0};
        int rc = hash_open_file(dirfd, name, st, ctx->max_file_size, hash, hash_error, sizeof(hash_error));
        if (rc == 0) hash_ready = true;
        else if (rc == -2) { status = "too_large"; error_text = hash_error[0] ? hash_error : "file exceeds configured size limit"; }
        else { status = "read_error"; error_text = hash_error[0] ? hash_error : "unable to hash file"; }
    }
    if (hash_ready && !hash[0]) hash_ready = false;

    sqlite3_stmt *stmt = ctx->seen_insert;
    sqlite3_reset(stmt); sqlite3_clear_bindings(stmt);
    sqlite3_bind_text(stmt, 1, relative_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, extension, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, mime_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, size);
    sqlite3_bind_int64(stmt, 5, mtime_ns);
    if (hash_ready) sqlite3_bind_text(stmt, 6, hash, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(stmt, 6);
    sqlite3_bind_text(stmt, 7, status, -1, SQLITE_TRANSIENT);
    if (error_text) sqlite3_bind_text(stmt, 8, error_text, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(stmt, 8);
    sqlite3_bind_int(stmt, 9, existed ? 1 : 0);
    int rc = sqlite3_step(stmt);
    sqlite3_reset(stmt);
    if (rc != SQLITE_DONE) return sqlite_error(ctx->db, ctx->error, ctx->error_size, "stage scanned file");
    ctx->result.files_seen++;
    if (strcmp(status, "read_error") == 0 || strcmp(status, "too_large") == 0) ctx->result.file_errors++;
    return 0;
}

static int scan_directory(ScanContext *ctx, int dirfd, const char *relative_prefix, uint64_t current_mount_id, bool has_current_mount_id) {
    int duplicate = dup(dirfd);
    if (duplicate < 0) {
        ctx->result.walk_errors++;
        return 0;
    }
    DIR *dir = fdopendir(duplicate);
    if (!dir) {
        close(duplicate);
        ctx->result.walk_errors++;
        return 0;
    }
    struct dirent *entry;
    int read_error = 0;
    for (;;) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) { read_error = errno; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        struct stat st;
        if (fstatat(dirfd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            ctx->result.walk_errors++;
            continue;
        }
        if (S_ISLNK(st.st_mode)) {
            ctx->result.skipped_symlinks++;
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!strcmp(entry->d_name, ".asgend_mem")) continue;
            if (ctx->exclude_resource_directories && !relative_prefix[0] &&
                (!strcmp(entry->d_name, "skills") || !strcmp(entry->d_name, "knowledge") ||
                 !strcmp(entry->d_name, "tools") || !strcmp(entry->d_name, "system_prompts") ||
                 !strcmp(entry->d_name, "instructions"))) continue;
            uint64_t child_mount_id = 0;
            bool has_child_mount_id = mount_id_at(dirfd, entry->d_name, &child_mount_id);
            bool mounted = has_current_mount_id && has_child_mount_id && child_mount_id != current_mount_id;
            if (mounted || st.st_dev != ctx->root_dev) {
                ctx->result.skipped_mountpoints++;
                continue;
            }
            int child = openat(dirfd, entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (child < 0) {
                ctx->result.walk_errors++;
                continue;
            }
            struct stat opened;
            if (fstat(child, &opened) != 0 || !S_ISDIR(opened.st_mode) || opened.st_dev != st.st_dev || opened.st_ino != st.st_ino) {
                close(child); ctx->result.walk_errors++; continue;
            }
            char *child_rel = relative_join(relative_prefix, entry->d_name);
            if (!child_rel) { close(child); closedir(dir); set_error(ctx->error, ctx->error_size, "out of memory building relative path"); return -1; }
            if (!has_child_mount_id) has_child_mount_id = mount_id_for_fd(child, &child_mount_id);
            int rc = scan_directory(ctx, child, child_rel, child_mount_id, has_child_mount_id);
            free(child_rel); close(child);
            if (rc != 0) { closedir(dir); return -1; }
            continue;
        }
        if (!S_ISREG(st.st_mode)) continue;
        char *relative_path = relative_join(relative_prefix, entry->d_name);
        char *extension = relative_path ? lower_extension(relative_path) : NULL;
        if (!relative_path || !extension) {
            free(relative_path); free(extension); closedir(dir);
            set_error(ctx->error, ctx->error_size, "out of memory building file record");
            return -1;
        }
        const char *mime = mime_for_extension(extension);
        int rc = stage_file(ctx, relative_path, extension, mime, &st, dirfd, entry->d_name);
        free(relative_path); free(extension);
        if (rc != 0) { closedir(dir); return -1; }
    }
    closedir(dir);
    if (read_error != 0) ctx->result.walk_errors++;
    return 0;
}

static int insert_event(sqlite3 *db, int64_t scan_id, int64_t file_id,
                        const char *event_type, const char *old_path,
                        const char *new_path, const char *sha256, const char *details,
                        char *error, size_t error_size) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO file_events(scan_id,file_id,event_type,old_path,new_path,sha256,details) VALUES(?1,?2,?3,?4,?5,?6,?7)";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return sqlite_error(db, error, error_size, "prepare event insert");
    sqlite3_bind_int64(stmt, 1, scan_id);
    if (file_id > 0) sqlite3_bind_int64(stmt, 2, file_id); else sqlite3_bind_null(stmt, 2);
    sqlite3_bind_text(stmt, 3, event_type, -1, SQLITE_TRANSIENT);
    if (old_path) sqlite3_bind_text(stmt, 4, old_path, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(stmt, 4);
    if (new_path) sqlite3_bind_text(stmt, 5, new_path, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(stmt, 5);
    if (sha256) sqlite3_bind_text(stmt, 6, sha256, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(stmt, 6);
    if (details) sqlite3_bind_text(stmt, 7, details, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(stmt, 7);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return sqlite_error(db, error, error_size, "insert file event");
    return 0;
}

static int insert_snapshot(sqlite3 *db, int64_t scan_id, int64_t file_id,
                           const char *path, int64_t size, int64_t mtime_ns,
                           const char *hash, const char *status, char *error, size_t error_size) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT OR REPLACE INTO file_snapshots(scan_id,file_id,relative_path,file_size,mtime_ns,sha256,status) VALUES(?1,?2,?3,?4,?5,?6,?7)";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return sqlite_error(db, error, error_size, "prepare snapshot insert");
    sqlite3_bind_int64(stmt, 1, scan_id); sqlite3_bind_int64(stmt, 2, file_id);
    sqlite3_bind_text(stmt, 3, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, size); sqlite3_bind_int64(stmt, 5, mtime_ns);
    if (hash) sqlite3_bind_text(stmt, 6, hash, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(stmt, 6);
    sqlite3_bind_text(stmt, 7, status, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt); sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return sqlite_error(db, error, error_size, "insert snapshot");
    return 0;
}

static int try_find_rename(ScanContext *ctx, const char *path, const char *hash,
                           int64_t *file_id, char **old_path) {
    *file_id = 0; *old_path = NULL;
    if (!hash || !hash[0]) return 0;
    sqlite3_stmt *count_stmt = NULL;
    const char *count_sql = "SELECT COUNT(*) FROM scan_seen WHERE sha256=?1 AND path_existed=0";
    if (sqlite3_prepare_v2(ctx->db, count_sql, -1, &count_stmt, NULL) != SQLITE_OK)
        return sqlite_error(ctx->db, ctx->error, ctx->error_size, "prepare rename candidate count");
    sqlite3_bind_text(count_stmt, 1, hash, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(count_stmt);
    int incoming = rc == SQLITE_ROW ? sqlite3_column_int(count_stmt, 0) : -1;
    sqlite3_finalize(count_stmt);
    if (incoming != 1) return incoming < 0 ? sqlite_error(ctx->db, ctx->error, ctx->error_size, "count rename candidates") : 0;

    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT f.file_id,f.relative_path FROM files f "
        "WHERE f.sha256=?1 AND f.deleted_at IS NULL AND f.last_seen_scan_id<>?2 "
        "AND f.relative_path NOT IN (SELECT relative_path FROM scan_seen) "
        "AND NOT EXISTS (SELECT 1 FROM files at_new_path WHERE at_new_path.relative_path=?3) "
        "ORDER BY f.updated_at DESC LIMIT 2";
    if (sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return sqlite_error(ctx->db, ctx->error, ctx->error_size, "prepare rename lookup");
    sqlite3_bind_text(stmt, 1, hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, ctx->scan_id);
    sqlite3_bind_text(stmt, 3, path, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *file_id = sqlite3_column_int64(stmt, 0);
        const unsigned char *p = sqlite3_column_text(stmt, 1);
        *old_path = p ? strdup((const char *)p) : NULL;
        if (p && !*old_path) { sqlite3_finalize(stmt); set_error(ctx->error, ctx->error_size, "out of memory copying rename path"); return -1; }
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) { *file_id = 0; free(*old_path); *old_path = NULL; }
        else if (rc != SQLITE_DONE) { sqlite3_finalize(stmt); return sqlite_error(ctx->db, ctx->error, ctx->error_size, "finish rename lookup"); }
    } else if (rc != SQLITE_DONE) { sqlite3_finalize(stmt); return sqlite_error(ctx->db, ctx->error, ctx->error_size, "lookup rename candidate"); }
    sqlite3_finalize(stmt);
    return 0;
}

static int process_seen_files(ScanContext *ctx) {
    static const char select_sql[] = "SELECT relative_path,extension,mime_type,file_size,mtime_ns,sha256,status,error_text,path_existed FROM scan_seen ORDER BY relative_path";
    sqlite3_stmt *seen = NULL;
    if (sqlite3_prepare_v2(ctx->db, select_sql, -1, &seen, NULL) != SQLITE_OK)
        return sqlite_error(ctx->db, ctx->error, ctx->error_size, "prepare staged file walk");
    int seen_rc = SQLITE_OK;
    while ((seen_rc = sqlite3_step(seen)) == SQLITE_ROW) {
        const char *path = (const char *)sqlite3_column_text(seen, 0);
        const char *extension = (const char *)sqlite3_column_text(seen, 1);
        const char *mime = (const char *)sqlite3_column_text(seen, 2);
        int64_t size = sqlite3_column_int64(seen, 3);
        int64_t mtime_ns = sqlite3_column_int64(seen, 4);
        const char *hash = sqlite3_column_type(seen, 5) == SQLITE_NULL ? NULL : (const char *)sqlite3_column_text(seen, 5);
        const char *status = (const char *)sqlite3_column_text(seen, 6);
        const char *details = sqlite3_column_type(seen, 7) == SQLITE_NULL ? NULL : (const char *)sqlite3_column_text(seen, 7);
        bool path_existed = sqlite3_column_int(seen, 8) != 0;
        if (!path || !extension || !mime || !status) { sqlite3_finalize(seen); set_error(ctx->error, ctx->error_size, "invalid staged file record"); return -1; }

        bool exists = false, active = false, old_hash_valid = false;
        int64_t file_id = 0, old_size = 0, old_mtime = 0;
        char old_hash[65], old_status[24];
        if (lookup_file(ctx, path, &exists, &active, &file_id, &old_size, &old_mtime, old_hash, &old_hash_valid, old_status) != 0) { sqlite3_finalize(seen); return -1; }
        bool restored = exists && !active;
        bool hashes_comparable = old_hash_valid && hash != NULL;
        bool content_changed = (old_hash_valid && (!hash || strcmp(old_hash, hash) != 0)) || (!old_hash_valid && hash != NULL);
        bool metadata_changed_without_hash = (!hashes_comparable) && (old_size != size || old_mtime != mtime_ns);
        bool modified = exists && active && (content_changed || strcmp(old_status, status) != 0 || metadata_changed_without_hash);
        char *old_path_for_event = NULL;
        const char *event_type = NULL;

        if (!exists) {
            int64_t rename_id = 0;
            if (try_find_rename(ctx, path, hash, &rename_id, &old_path_for_event) != 0) { sqlite3_finalize(seen); return -1; }
            if (rename_id > 0 && old_path_for_event) {
                sqlite3_stmt *update = NULL;
                const char *sql = "UPDATE files SET relative_path=?1,extension=?2,mime_type=?3,file_size=?4,mtime_ns=?5,sha256=?6,status=?7,last_seen_scan_id=?8,updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now'),deleted_at=NULL,error_text=?9 WHERE file_id=?10";
                if (sqlite3_prepare_v2(ctx->db, sql, -1, &update, NULL) != SQLITE_OK) { free(old_path_for_event); sqlite3_finalize(seen); return sqlite_error(ctx->db, ctx->error, ctx->error_size, "prepare rename update"); }
                sqlite3_bind_text(update, 1, path, -1, SQLITE_TRANSIENT); sqlite3_bind_text(update, 2, extension, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(update, 3, mime, -1, SQLITE_TRANSIENT); sqlite3_bind_int64(update, 4, size); sqlite3_bind_int64(update, 5, mtime_ns);
                if (hash) sqlite3_bind_text(update, 6, hash, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(update, 6);
                sqlite3_bind_text(update, 7, status, -1, SQLITE_TRANSIENT); sqlite3_bind_int64(update, 8, ctx->scan_id);
                if (details) sqlite3_bind_text(update, 9, details, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(update, 9);
                sqlite3_bind_int64(update, 10, rename_id);
                int rc = sqlite3_step(update); sqlite3_finalize(update);
                if (rc != SQLITE_DONE) { free(old_path_for_event); sqlite3_finalize(seen); return sqlite_error(ctx->db, ctx->error, ctx->error_size, "update renamed file"); }
                file_id = rename_id; event_type = "renamed"; ctx->result.renamed++;
                if (insert_event(ctx->db, ctx->scan_id, file_id, event_type, old_path_for_event, path, hash, "same unique hash moved to a new path", ctx->error, ctx->error_size) != 0) { free(old_path_for_event); sqlite3_finalize(seen); return -1; }
            } else {
                sqlite3_stmt *insert = NULL;
                const char *sql = "INSERT INTO files(relative_path,extension,mime_type,file_size,mtime_ns,sha256,status,last_seen_scan_id,error_text) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)";
                if (sqlite3_prepare_v2(ctx->db, sql, -1, &insert, NULL) != SQLITE_OK) { free(old_path_for_event); sqlite3_finalize(seen); return sqlite_error(ctx->db, ctx->error, ctx->error_size, "prepare new file"); }
                sqlite3_bind_text(insert, 1, path, -1, SQLITE_TRANSIENT); sqlite3_bind_text(insert, 2, extension, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insert, 3, mime, -1, SQLITE_TRANSIENT); sqlite3_bind_int64(insert, 4, size); sqlite3_bind_int64(insert, 5, mtime_ns);
                if (hash) sqlite3_bind_text(insert, 6, hash, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(insert, 6);
                sqlite3_bind_text(insert, 7, status, -1, SQLITE_TRANSIENT); sqlite3_bind_int64(insert, 8, ctx->scan_id);
                if (details) sqlite3_bind_text(insert, 9, details, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(insert, 9);
                int rc = sqlite3_step(insert); sqlite3_finalize(insert);
                if (rc != SQLITE_DONE) { free(old_path_for_event); sqlite3_finalize(seen); return sqlite_error(ctx->db, ctx->error, ctx->error_size, "insert new file"); }
                file_id = sqlite3_last_insert_rowid(ctx->db); event_type = "added"; ctx->result.added++;
                if (insert_event(ctx->db, ctx->scan_id, file_id, event_type, NULL, path, hash, details, ctx->error, ctx->error_size) != 0) { free(old_path_for_event); sqlite3_finalize(seen); return -1; }
            }
        } else {
            sqlite3_stmt *update = NULL;
            const char *sql = "UPDATE files SET extension=?1,mime_type=?2,file_size=?3,mtime_ns=?4,sha256=?5,status=?6,last_seen_scan_id=?7,updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now'),deleted_at=NULL,error_text=?8 WHERE file_id=?9";
            if (sqlite3_prepare_v2(ctx->db, sql, -1, &update, NULL) != SQLITE_OK) { sqlite3_finalize(seen); return sqlite_error(ctx->db, ctx->error, ctx->error_size, "prepare file update"); }
            sqlite3_bind_text(update, 1, extension, -1, SQLITE_TRANSIENT); sqlite3_bind_text(update, 2, mime, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(update, 3, size); sqlite3_bind_int64(update, 4, mtime_ns);
            if (hash) sqlite3_bind_text(update, 5, hash, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(update, 5);
            sqlite3_bind_text(update, 6, status, -1, SQLITE_TRANSIENT); sqlite3_bind_int64(update, 7, ctx->scan_id);
            if (details) sqlite3_bind_text(update, 8, details, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(update, 8);
            sqlite3_bind_int64(update, 9, file_id);
            int rc = sqlite3_step(update); sqlite3_finalize(update);
            if (rc != SQLITE_DONE) { sqlite3_finalize(seen); return sqlite_error(ctx->db, ctx->error, ctx->error_size, "update file"); }
            if (restored) { event_type = "restored"; ctx->result.restored++; }
            else if (modified && (strcmp(status, "read_error") == 0 || strcmp(status, "too_large") == 0)) event_type = status;
            else if (modified) { event_type = "modified"; ctx->result.modified++; }
            if (event_type && insert_event(ctx->db, ctx->scan_id, file_id, event_type, NULL, path, hash, details, ctx->error, ctx->error_size) != 0) { sqlite3_finalize(seen); return -1; }
        }
        if (insert_snapshot(ctx->db, ctx->scan_id, file_id, path, size, mtime_ns, hash, status, ctx->error, ctx->error_size) != 0) { free(old_path_for_event); sqlite3_finalize(seen); return -1; }
        free(old_path_for_event);
        (void)path_existed;
    }
    sqlite3_finalize(seen);
    if (seen_rc != SQLITE_DONE) return sqlite_error(ctx->db, ctx->error, ctx->error_size, "iterate staged files");
    return 0;
}

static int reconcile_scan(ScanContext *ctx) {
    if (exec_sql(ctx->db, "BEGIN IMMEDIATE", ctx->error, ctx->error_size) != 0) return -1;
    if (process_seen_files(ctx) != 0) goto fail;

    /* A directory traversal error makes deletions ambiguous, so preserve unseen paths. */
    if (ctx->result.walk_errors == 0) {
        sqlite3_stmt *missing = NULL;
        const char *select_sql = "SELECT file_id,relative_path,file_size,mtime_ns,sha256 FROM files WHERE deleted_at IS NULL AND last_seen_scan_id<>?1";
        if (sqlite3_prepare_v2(ctx->db, select_sql, -1, &missing, NULL) != SQLITE_OK) { sqlite_error(ctx->db, ctx->error, ctx->error_size, "prepare missing files"); goto fail; }
        sqlite3_bind_int64(missing, 1, ctx->scan_id);
        typedef struct { int64_t id,size,mtime; char *path; char hash[65]; bool has_hash; } MissingFile;
        MissingFile *items = NULL; size_t count = 0, capacity = 0;
        int rc;
        while ((rc = sqlite3_step(missing)) == SQLITE_ROW) {
            if (count == capacity) {
                size_t next = capacity ? capacity * 2 : 32;
                if (next < capacity || next > SIZE_MAX / sizeof(*items)) { set_error(ctx->error, ctx->error_size, "too many missing files"); free(items); sqlite3_finalize(missing); goto fail; }
                MissingFile *grown = realloc(items, next * sizeof(*items));
                if (!grown) { set_error(ctx->error, ctx->error_size, "out of memory collecting deleted paths"); free(items); sqlite3_finalize(missing); goto fail; }
                items = grown; capacity = next;
            }
            MissingFile *item = &items[count++]; memset(item, 0, sizeof(*item));
            item->id = sqlite3_column_int64(missing, 0); item->path = strdup((const char *)sqlite3_column_text(missing, 1));
            item->size = sqlite3_column_int64(missing, 2); item->mtime = sqlite3_column_int64(missing, 3);
            const unsigned char *hash = sqlite3_column_text(missing, 4);
            if (hash && strlen((const char *)hash) == 64) { snprintf(item->hash, sizeof(item->hash), "%s", (const char *)hash); item->has_hash = true; }
            if (!item->path) { set_error(ctx->error, ctx->error_size, "out of memory copying deleted path"); for(size_t i=0;i<count;i++) free(items[i].path); free(items); sqlite3_finalize(missing); goto fail; }
        }
        sqlite3_finalize(missing);
        if (rc != SQLITE_DONE) { for(size_t i=0;i<count;i++) free(items[i].path); free(items); sqlite_error(ctx->db, ctx->error, ctx->error_size, "read deleted paths"); goto fail; }
        for (size_t i = 0; i < count; ++i) {
            sqlite3_stmt *update = NULL;
            if (sqlite3_prepare_v2(ctx->db, "UPDATE files SET status='deleted',deleted_at=strftime('%Y-%m-%dT%H:%M:%fZ','now'),updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE file_id=?1", -1, &update, NULL) != SQLITE_OK) { for(size_t j=i;j<count;j++) free(items[j].path); for(size_t j=0;j<i;j++) free(items[j].path); free(items); sqlite_error(ctx->db, ctx->error, ctx->error_size, "prepare delete update"); goto fail; }
            sqlite3_bind_int64(update, 1, items[i].id);
            int update_rc = sqlite3_step(update); sqlite3_finalize(update);
            if (update_rc != SQLITE_DONE || insert_event(ctx->db, ctx->scan_id, items[i].id, "deleted", items[i].path, NULL, items[i].has_hash ? items[i].hash : NULL, "path not present in complete scan", ctx->error, ctx->error_size) != 0 || insert_snapshot(ctx->db, ctx->scan_id, items[i].id, items[i].path, items[i].size, items[i].mtime, items[i].has_hash ? items[i].hash : NULL, "deleted", ctx->error, ctx->error_size) != 0) {
                for (size_t j = 0; j < count; ++j) free(items[j].path);
                free(items);
                if (update_rc != SQLITE_DONE) sqlite_error(ctx->db, ctx->error, ctx->error_size, "mark deleted file");
                goto fail;
            }
            ctx->result.deleted++;
        }
        for (size_t i = 0; i < count; ++i) free(items[i].path);
        free(items);
    }

    sqlite3_stmt *finish = NULL;
    const char *finish_sql = "UPDATE scan_runs SET completed_at=strftime('%Y-%m-%dT%H:%M:%fZ','now'),status=?1,files_seen=?2,added_count=?3,modified_count=?4,deleted_count=?5,renamed_count=?6,restored_count=?7,file_errors=?8,walk_errors=?9,skipped_symlinks=?10,skipped_mountpoints=?11 WHERE scan_id=?12";
    if (sqlite3_prepare_v2(ctx->db, finish_sql, -1, &finish, NULL) != SQLITE_OK) { sqlite_error(ctx->db, ctx->error, ctx->error_size, "prepare scan completion"); goto fail; }
    sqlite3_bind_text(finish, 1, ctx->result.walk_errors ? "partial" : "complete", -1, SQLITE_STATIC);
    sqlite3_bind_int64(finish, 2, (sqlite3_int64)ctx->result.files_seen); sqlite3_bind_int64(finish, 3, (sqlite3_int64)ctx->result.added);
    sqlite3_bind_int64(finish, 4, (sqlite3_int64)ctx->result.modified); sqlite3_bind_int64(finish, 5, (sqlite3_int64)ctx->result.deleted);
    sqlite3_bind_int64(finish, 6, (sqlite3_int64)ctx->result.renamed); sqlite3_bind_int64(finish, 7, (sqlite3_int64)ctx->result.restored);
    sqlite3_bind_int64(finish, 8, (sqlite3_int64)ctx->result.file_errors); sqlite3_bind_int64(finish, 9, (sqlite3_int64)ctx->result.walk_errors);
    sqlite3_bind_int64(finish, 10, (sqlite3_int64)ctx->result.skipped_symlinks); sqlite3_bind_int64(finish, 11, (sqlite3_int64)ctx->result.skipped_mountpoints);
    sqlite3_bind_int64(finish, 12, ctx->scan_id);
    int rc = sqlite3_step(finish); sqlite3_finalize(finish);
    if (rc != SQLITE_DONE) { sqlite_error(ctx->db, ctx->error, ctx->error_size, "finish scan run"); goto fail; }
    if (exec_sql(ctx->db, "COMMIT", ctx->error, ctx->error_size) != 0) goto fail_no_rollback;
    ctx->result.complete = ctx->result.walk_errors == 0;
    return 0;
fail:
    (void)sqlite3_exec(ctx->db, "ROLLBACK", NULL, NULL, NULL);
    return -1;
fail_no_rollback:
    (void)sqlite3_exec(ctx->db, "ROLLBACK", NULL, NULL, NULL);
    return -1;
}

int asgend_memory_scan(const char *root, const AsgendMemoryScanOptions *options,
                       AsgendMemoryScanResult *result, char *error, size_t error_size) {
    if (error && error_size) error[0] = '\0';
    if (result) memset(result, 0, sizeof(*result));
    char *canonical = NULL, *db_path = NULL;
    sqlite3 *db = NULL;
    if (open_index(root, &canonical, &db, &db_path, error, error_size) != 0) return -1;
    uint64_t max_file_size = options && options->max_file_size_bytes ? options->max_file_size_bytes : ASGEND_MEMORY_DEFAULT_MAX_FILE_SIZE;
    if (max_file_size > (uint64_t)INT64_MAX) {
        set_error(error, error_size, "max_file_size_bytes exceeds SQLite's signed 64-bit integer range");
        sqlite3_close(db); free(db_path); free(canonical); return -1;
    }
    bool verify = options && options->verify_hashes;
    struct stat root_st;
    int root_fd = open(canonical, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root_fd < 0 || fstat(root_fd, &root_st) != 0 || !S_ISDIR(root_st.st_mode)) {
        set_error(error, error_size, "cannot open scan root '%s': %s", canonical, strerror(errno));
        if (root_fd >= 0) close(root_fd);
        sqlite3_close(db); free(db_path); free(canonical); return -1;
    }
    ScanContext ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.db = db; ctx.root_path = canonical; ctx.root_dev = root_st.st_dev; ctx.max_file_size = max_file_size; ctx.verify_hashes = verify;
    ctx.exclude_resource_directories = options && options->exclude_resource_directories;
    ctx.error = error; ctx.error_size = error_size;
    ctx.result.complete = true;

    if (exec_sql(db, "CREATE TEMP TABLE scan_seen(relative_path TEXT PRIMARY KEY,extension TEXT NOT NULL,mime_type TEXT NOT NULL,file_size INTEGER NOT NULL,mtime_ns INTEGER NOT NULL,sha256 TEXT,status TEXT NOT NULL,error_text TEXT,path_existed INTEGER NOT NULL);", error, error_size) != 0) goto fail;
    if (exec_sql(db, "UPDATE scan_runs SET status='failed',completed_at=COALESCE(completed_at,strftime('%Y-%m-%dT%H:%M:%fZ','now')) WHERE status='running'", error, error_size) != 0) goto fail;
    if (sqlite3_prepare_v2(db, "INSERT INTO scan_runs(root_path,status,verify_mode,max_file_size_bytes) VALUES(?1,'running',?2,?3)", -1, &ctx.lookup_path, NULL) != SQLITE_OK) { sqlite_error(db, error, error_size, "prepare scan run"); goto fail; }
    sqlite3_bind_text(ctx.lookup_path, 1, canonical, -1, SQLITE_TRANSIENT); sqlite3_bind_int(ctx.lookup_path, 2, verify ? 1 : 0); sqlite3_bind_int64(ctx.lookup_path, 3, (sqlite3_int64)max_file_size);
    int rc = sqlite3_step(ctx.lookup_path); sqlite3_finalize(ctx.lookup_path); ctx.lookup_path = NULL;
    if (rc != SQLITE_DONE) { sqlite_error(db, error, error_size, "create scan run"); goto fail; }
    ctx.scan_id = sqlite3_last_insert_rowid(db); ctx.result.scan_id = ctx.scan_id;
    if (exec_sql(db, "CREATE TEMP TABLE IF NOT EXISTS scan_seen(relative_path TEXT PRIMARY KEY,extension TEXT NOT NULL,mime_type TEXT NOT NULL,file_size INTEGER NOT NULL,mtime_ns INTEGER NOT NULL,sha256 TEXT,status TEXT NOT NULL,error_text TEXT,path_existed INTEGER NOT NULL);", error, error_size) != 0) goto fail;
    if (sqlite3_prepare_v2(db, "INSERT INTO scan_seen(relative_path,extension,mime_type,file_size,mtime_ns,sha256,status,error_text,path_existed) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)", -1, &ctx.seen_insert, NULL) != SQLITE_OK) { sqlite_error(db, error, error_size, "prepare staged insert"); goto fail; }
    if (sqlite3_prepare_v2(db, "SELECT file_id,file_size,mtime_ns,sha256,status,deleted_at FROM files WHERE relative_path=?1", -1, &ctx.lookup_path, NULL) != SQLITE_OK) { sqlite_error(db, error, error_size, "prepare path lookup"); goto fail; }

    uint64_t root_mount_id = 0;
    bool has_root_mount_id = mount_id_for_fd(root_fd, &root_mount_id);
    if (scan_directory(&ctx, root_fd, "", root_mount_id, has_root_mount_id) != 0) goto fail;
    if (reconcile_scan(&ctx) != 0) goto fail;
    sqlite3_finalize(ctx.seen_insert); sqlite3_finalize(ctx.lookup_path);
    close(root_fd); sqlite3_close(db); free(db_path); free(canonical);
    if (result) *result = ctx.result;
    return 0;
fail:
    if (ctx.seen_insert) sqlite3_finalize(ctx.seen_insert);
    if (ctx.lookup_path) sqlite3_finalize(ctx.lookup_path);
    if (ctx.scan_id > 0) {
        sqlite3_stmt *failed = NULL;
        if (sqlite3_prepare_v2(db, "UPDATE scan_runs SET completed_at=strftime('%Y-%m-%dT%H:%M:%fZ','now'),status='failed' WHERE scan_id=?1", -1, &failed, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(failed, 1, ctx.scan_id); (void)sqlite3_step(failed); sqlite3_finalize(failed);
        }
    }
    if (root_fd >= 0) close(root_fd);
    sqlite3_close(db); free(db_path); free(canonical);
    return -1;
}

static int query_one(sqlite3 *db, const char *sql, sqlite3_stmt **stmt, char *error, size_t error_size) {
    if (sqlite3_prepare_v2(db, sql, -1, stmt, NULL) != SQLITE_OK) return sqlite_error(db, error, error_size, "prepare status query");
    return 0;
}

int asgend_memory_status(const char *root, FILE *out, char *error, size_t error_size) {
    if (error && error_size) error[0] = '\0';
    if (!out) { set_error(error, error_size, "output stream is required"); return -1; }
    char *canonical = NULL, *db_path = NULL; sqlite3 *db = NULL;
    if (open_index(root, &canonical, &db, &db_path, error, error_size) != 0) return -1;
    sqlite3_stmt *stmt = NULL;
    if (query_one(db, "SELECT COUNT(*),SUM(CASE WHEN deleted_at IS NULL THEN 1 ELSE 0 END),SUM(CASE WHEN deleted_at IS NOT NULL THEN 1 ELSE 0 END) FROM files", &stmt, error, error_size) != 0) goto fail;
    int aggregate_rc = sqlite3_step(stmt);
    if (aggregate_rc != SQLITE_ROW) { sqlite_error(db, error, error_size, "read file counts"); goto fail; }
    fprintf(out, "Root: %s\nDatabase: %s\n", canonical, db_path);
    fprintf(out, "Files: total=%lld active=%lld deleted=%lld\n",
        (long long)sqlite3_column_int64(stmt, 0), (long long)sqlite3_column_int64(stmt, 1), (long long)sqlite3_column_int64(stmt, 2));
    sqlite3_finalize(stmt); stmt = NULL;
    if (query_one(db, "SELECT COUNT(*),SUM(CASE WHEN c.extract_status IN ('extracted','truncated','parsed_partial') THEN 1 ELSE 0 END),SUM(CASE WHEN c.extract_status IN ('unsupported','binary','too_large') THEN 1 ELSE 0 END),SUM(CASE WHEN c.extract_status IN ('read_error','stale_source') THEN 1 ELSE 0 END) FROM files f LEFT JOIN file_contents c ON c.file_id=f.file_id WHERE f.deleted_at IS NULL", &stmt, error, error_size) != 0) goto fail;
    int extraction_rc = sqlite3_step(stmt);
    if (extraction_rc != SQLITE_ROW) { sqlite_error(db, error, error_size, "read extraction counts"); goto fail; }
    fprintf(out, "Extraction: active=%lld text=%lld unsupported_or_binary=%lld errors=%lld\n",
        (long long)sqlite3_column_int64(stmt, 0), (long long)sqlite3_column_int64(stmt, 1),
        (long long)sqlite3_column_int64(stmt, 2), (long long)sqlite3_column_int64(stmt, 3));
    sqlite3_finalize(stmt); stmt = NULL;
    if (query_one(db, "SELECT (SELECT COUNT(*) FROM model_metadata mm JOIN files f ON f.file_id=mm.file_id AND f.sha256=mm.source_sha256 WHERE f.deleted_at IS NULL),(SELECT COUNT(*) FROM descriptions d JOIN files f ON f.file_id=d.file_id AND f.sha256=d.source_sha256 WHERE f.deleted_at IS NULL AND d.status='complete'),(SELECT COUNT(*) FROM embeddings e JOIN files f ON f.file_id=e.file_id WHERE f.deleted_at IS NULL),(SELECT COUNT(*) FROM pipeline_jobs WHERE state='failed'),(SELECT COUNT(*) FROM pipeline_jobs WHERE state='running'),(SELECT COUNT(*) FROM pipeline_jobs WHERE state='succeeded')", &stmt, error, error_size) != 0) goto fail;
    int pipeline_rc = sqlite3_step(stmt);
    if (pipeline_rc != SQLITE_ROW) { sqlite_error(db, error, error_size, "read model pipeline counts"); goto fail; }
    fprintf(out, "Model pipeline: metadata=%lld descriptions=%lld embedding_vectors=%lld jobs_succeeded=%lld jobs_running=%lld jobs_failed=%lld\n",
        (long long)sqlite3_column_int64(stmt, 0), (long long)sqlite3_column_int64(stmt, 1),
        (long long)sqlite3_column_int64(stmt, 2), (long long)sqlite3_column_int64(stmt, 5),
        (long long)sqlite3_column_int64(stmt, 4), (long long)sqlite3_column_int64(stmt, 3));
    sqlite3_finalize(stmt); stmt = NULL;
    if (query_one(db, "SELECT scan_id,status,started_at,completed_at,files_seen,added_count,modified_count,deleted_count,renamed_count,restored_count,file_errors,walk_errors FROM scan_runs ORDER BY scan_id DESC LIMIT 1", &stmt, error, error_size) != 0) goto fail;
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        fprintf(out, "Last scan: id=%lld status=%s started=%s completed=%s files=%lld added=%lld modified=%lld deleted=%lld renamed=%lld restored=%lld file_errors=%lld walk_errors=%lld\n",
            (long long)sqlite3_column_int64(stmt, 0), sqlite3_column_text(stmt, 1), sqlite3_column_text(stmt, 2),
            sqlite3_column_type(stmt, 3) == SQLITE_NULL ? "(running)" : (const char *)sqlite3_column_text(stmt, 3),
            (long long)sqlite3_column_int64(stmt, 4), (long long)sqlite3_column_int64(stmt, 5),
            (long long)sqlite3_column_int64(stmt, 6), (long long)sqlite3_column_int64(stmt, 7),
            (long long)sqlite3_column_int64(stmt, 8), (long long)sqlite3_column_int64(stmt, 9),
            (long long)sqlite3_column_int64(stmt, 10), (long long)sqlite3_column_int64(stmt, 11));
    } else if (rc == SQLITE_DONE) fprintf(out, "Last scan: none\n");
    else { sqlite_error(db, error, error_size, "read last scan"); goto fail; }
    sqlite3_finalize(stmt); sqlite3_close(db); free(db_path); free(canonical); return 0;
fail:
    if (stmt) sqlite3_finalize(stmt);
    sqlite3_close(db); free(db_path); free(canonical); return -1;
}

static int memory_scalar_i64(sqlite3 *db, const char *sql, sqlite3_int64 *value,
                             char *error, size_t error_size) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return sqlite_error(db, error, error_size, "prepare memory statistic");
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return sqlite_error(db, error, error_size, "read memory statistic");
    }
    *value = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return 0;
}

static int memory_stats_add_count(sqlite3 *db, struct json_object *object,
                                  const char *key, const char *sql,
                                  char *error, size_t error_size) {
    sqlite3_int64 value = 0;
    if (memory_scalar_i64(db, sql, &value, error, error_size) != 0) return -1;
    json_object_object_add(object, key, json_object_new_int64(value));
    return 0;
}

static int64_t memory_stat_file_size(const char *path) {
    struct stat st;
    if (!path || stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) return 0;
    return (int64_t)st.st_size;
}

static int memory_stats_add_last_scan(sqlite3 *db, struct json_object *top,
                                      char *error, size_t error_size) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT scan_id,status,started_at,completed_at,files_seen,added_count,modified_count,deleted_count,renamed_count,restored_count,file_errors,walk_errors FROM scan_runs ORDER BY scan_id DESC LIMIT 1";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return sqlite_error(db, error, error_size, "prepare last scan statistics");
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        json_object_object_add(top, "last_scan", json_object_new_null());
        sqlite3_finalize(stmt);
        return 0;
    }
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return sqlite_error(db, error, error_size, "read last scan statistics");
    }
    struct json_object *scan = json_object_new_object();
    if (!scan) {
        sqlite3_finalize(stmt);
        set_error(error, error_size, "out of memory while serializing last scan statistics");
        return -1;
    }
    json_object_object_add(scan, "id", json_object_new_int64(sqlite3_column_int64(stmt, 0)));
    const unsigned char *status = sqlite3_column_text(stmt, 1);
    const unsigned char *started = sqlite3_column_text(stmt, 2);
    json_object_object_add(scan, "status", status ? json_object_new_string((const char *)status) : json_object_new_null());
    json_object_object_add(scan, "started_at", started ? json_object_new_string((const char *)started) : json_object_new_null());
    const unsigned char *completed = sqlite3_column_text(stmt, 3);
    json_object_object_add(scan, "completed_at", completed ? json_object_new_string((const char *)completed) : json_object_new_null());
    static const char *keys[] = {"files_seen", "added", "modified", "deleted", "renamed", "restored", "file_errors", "walk_errors"};
    for (int i = 0; i < 8; ++i)
        json_object_object_add(scan, keys[i], json_object_new_int64(sqlite3_column_int64(stmt, i + 4)));
    json_object_object_add(top, "last_scan", scan);
    sqlite3_finalize(stmt);
    return 0;
}

int asgend_memory_stats(const char *root, FILE *out, char *error, size_t error_size) {
    if (error && error_size) error[0] = '\0';
    if (!out) { set_error(error, error_size, "output stream is required"); return -1; }
    char *canonical = NULL, *db_path = NULL;
    sqlite3 *db = NULL;
    struct json_object *top = NULL;
    struct json_object *sizes = NULL, *files = NULL, *extraction = NULL;
    struct json_object *chunks = NULL, *vectors = NULL, *jobs = NULL;
    char *wal_path = NULL, *shm_path = NULL;
    if (open_index(root, &canonical, &db, &db_path, error, error_size) != 0) return -1;
    top = json_object_new_object();
    sizes = json_object_new_object(); files = json_object_new_object();
    extraction = json_object_new_object(); chunks = json_object_new_object();
    vectors = json_object_new_object(); jobs = json_object_new_object();
    if (!top || !sizes || !files || !extraction || !chunks || !vectors || !jobs) {
        set_error(error, error_size, "out of memory while collecting memory statistics");
        goto fail;
    }
    size_t db_path_len = strlen(db_path);
    if (db_path_len > SIZE_MAX - 5) { set_error(error, error_size, "database path is too long"); goto fail; }
    wal_path = malloc(db_path_len + 5);
    shm_path = malloc(db_path_len + 5);
    if (!wal_path || !shm_path) { set_error(error, error_size, "out of memory while reading database file sizes"); goto fail; }
    (void)snprintf(wal_path, db_path_len + 5, "%s-wal", db_path);
    (void)snprintf(shm_path, db_path_len + 5, "%s-shm", db_path);
    int64_t db_bytes = memory_stat_file_size(db_path);
    int64_t wal_bytes = memory_stat_file_size(wal_path);
    int64_t shm_bytes = memory_stat_file_size(shm_path);
    json_object_object_add(top, "root", json_object_new_string(canonical));
    json_object_object_add(top, "database", json_object_new_string(db_path));
    sqlite3_int64 schema_version = 0;
    if (memory_scalar_i64(db, "PRAGMA user_version", &schema_version, error, error_size) != 0) goto fail;
    json_object_object_add(top, "schema_version", json_object_new_int64(schema_version));
    json_object_object_add(sizes, "database", json_object_new_int64(db_bytes));
    json_object_object_add(sizes, "wal", json_object_new_int64(wal_bytes));
    json_object_object_add(sizes, "shm", json_object_new_int64(shm_bytes));
    json_object_object_add(top, "storage_files_bytes", sizes); sizes = NULL;
    json_object_object_add(top, "storage_bytes", json_object_new_int64(db_bytes + wal_bytes + shm_bytes));

    if (memory_stats_add_count(db, files, "total", "SELECT COUNT(*) FROM files", error, error_size) != 0 ||
        memory_stats_add_count(db, files, "active", "SELECT COUNT(*) FROM files WHERE deleted_at IS NULL", error, error_size) != 0 ||
        memory_stats_add_count(db, files, "deleted", "SELECT COUNT(*) FROM files WHERE deleted_at IS NOT NULL", error, error_size) != 0) goto fail;
    json_object_object_add(top, "files", files); files = NULL;
    if (memory_stats_add_count(db, extraction, "current_text", "SELECT COUNT(*) FROM file_contents c JOIN files f ON f.file_id=c.file_id AND f.deleted_at IS NULL AND f.sha256=c.source_sha256 WHERE c.extract_status IN ('extracted','truncated','parsed_partial')", error, error_size) != 0 ||
        memory_stats_add_count(db, extraction, "unsupported_or_binary", "SELECT COUNT(*) FROM file_contents c JOIN files f ON f.file_id=c.file_id AND f.deleted_at IS NULL AND f.sha256=c.source_sha256 WHERE c.extract_status IN ('unsupported','binary','too_large')", error, error_size) != 0 ||
        memory_stats_add_count(db, extraction, "errors", "SELECT COUNT(*) FROM file_contents c JOIN files f ON f.file_id=c.file_id AND f.deleted_at IS NULL AND f.sha256=c.source_sha256 WHERE c.extract_status IN ('read_error','stale_source')", error, error_size) != 0) goto fail;
    json_object_object_add(top, "extraction", extraction); extraction = NULL;
    if (memory_stats_add_count(db, chunks, "current", "SELECT COUNT(*) FROM file_chunks c JOIN files f ON f.file_id=c.file_id AND f.deleted_at IS NULL AND f.sha256=c.source_sha256", error, error_size) != 0 ||
        memory_stats_add_count(db, chunks, "stale", "SELECT COUNT(*) FROM file_chunks c WHERE NOT EXISTS (SELECT 1 FROM files f WHERE f.file_id=c.file_id AND f.deleted_at IS NULL AND f.sha256=c.source_sha256)", error, error_size) != 0 ||
        memory_stats_add_count(db, chunks, "chunk_states", "SELECT COUNT(*) FROM file_chunk_state", error, error_size) != 0) goto fail;
    json_object_object_add(top, "chunks", chunks); chunks = NULL;
    if (memory_stats_add_count(db, vectors, "document_current", "SELECT COUNT(*) FROM embeddings e JOIN descriptions d ON d.description_id=e.description_id AND d.file_id=e.file_id AND d.status='complete' JOIN files f ON f.file_id=e.file_id AND f.deleted_at IS NULL AND f.sha256=d.source_sha256", error, error_size) != 0 ||
        memory_stats_add_count(db, vectors, "document_stale", "SELECT COUNT(*) FROM embeddings e WHERE NOT EXISTS (SELECT 1 FROM descriptions d JOIN files f ON f.file_id=d.file_id WHERE d.description_id=e.description_id AND d.file_id=e.file_id AND d.status='complete' AND f.deleted_at IS NULL AND f.sha256=d.source_sha256)", error, error_size) != 0 ||
        memory_stats_add_count(db, vectors, "chunk_current", "SELECT COUNT(*) FROM chunk_embeddings e JOIN file_chunks c ON c.chunk_id=e.chunk_id AND c.source_sha256=e.source_sha256 JOIN files f ON f.file_id=e.file_id AND f.deleted_at IS NULL AND f.sha256=c.source_sha256", error, error_size) != 0 ||
        memory_stats_add_count(db, vectors, "chunk_stale", "SELECT COUNT(*) FROM chunk_embeddings e WHERE NOT EXISTS (SELECT 1 FROM file_chunks c JOIN files f ON f.file_id=c.file_id WHERE c.chunk_id=e.chunk_id AND c.file_id=e.file_id AND c.source_sha256=e.source_sha256 AND f.deleted_at IS NULL AND f.sha256=c.source_sha256)", error, error_size) != 0) goto fail;
    json_object_object_add(top, "vectors", vectors); vectors = NULL;
    if (memory_stats_add_count(db, top, "current_graph_edges", "SELECT COUNT(*) FROM file_edges e JOIN files s ON s.file_id=e.source_file_id AND s.deleted_at IS NULL AND s.sha256=e.source_sha256 JOIN files t ON t.file_id=e.target_file_id AND t.deleted_at IS NULL", error, error_size) != 0 ||
        memory_stats_add_count(db, top, "stale_graph_edges", "SELECT COUNT(*) FROM file_edges e WHERE NOT EXISTS (SELECT 1 FROM files s JOIN files t ON t.file_id=e.target_file_id WHERE s.file_id=e.source_file_id AND s.deleted_at IS NULL AND t.deleted_at IS NULL AND s.sha256=e.source_sha256)", error, error_size) != 0) goto fail;

    sqlite3_stmt *jobs_stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT stage,state,COUNT(*) FROM pipeline_jobs GROUP BY stage,state ORDER BY stage,state", -1, &jobs_stmt, NULL) != SQLITE_OK) {
        sqlite_error(db, error, error_size, "prepare pipeline job statistics"); goto fail;
    }
    int jobs_rc;
    while ((jobs_rc = sqlite3_step(jobs_stmt)) == SQLITE_ROW) {
        const char *stage = (const char *)sqlite3_column_text(jobs_stmt, 0);
        const char *state = (const char *)sqlite3_column_text(jobs_stmt, 1);
        sqlite3_int64 count = sqlite3_column_int64(jobs_stmt, 2);
        struct json_object *stage_obj = NULL;
        if (!stage || !state) continue;
        if (!json_object_object_get_ex(jobs, stage, &stage_obj)) {
            stage_obj = json_object_new_object();
            if (!stage_obj) { sqlite3_finalize(jobs_stmt); set_error(error, error_size, "out of memory while serializing pipeline statistics"); goto fail; }
            json_object_object_add(jobs, stage, stage_obj);
        }
        json_object_object_add(stage_obj, state, json_object_new_int64(count));
    }
    sqlite3_finalize(jobs_stmt);
    if (jobs_rc != SQLITE_DONE) { sqlite_error(db, error, error_size, "read pipeline job statistics"); goto fail; }
    json_object_object_add(top, "pipeline_jobs", jobs); jobs = NULL;
    if (memory_stats_add_last_scan(db, top, error, error_size) != 0) goto fail;
    fputs(json_object_to_json_string_ext(top, JSON_C_TO_STRING_PRETTY), out);
    fputc('\n', out);
    json_object_put(top);
    sqlite3_close(db); free(db_path); free(canonical); free(wal_path); free(shm_path);
    return ferror(out) ? -1 : 0;
fail:
    if (top) json_object_put(top);
    if (sizes) json_object_put(sizes);
    if (files) json_object_put(files);
    if (extraction) json_object_put(extraction);
    if (chunks) json_object_put(chunks);
    if (vectors) json_object_put(vectors);
    if (jobs) json_object_put(jobs);
    if (db) sqlite3_close(db);
    free(db_path); free(canonical); free(wal_path); free(shm_path);
    return -1;
}

static int memory_gc_delete(sqlite3 *db, const char *sql, const char *operation,
                            int64_t *removed, char *error, size_t error_size) {
    if (exec_sql(db, sql, error, error_size) != 0) {
        if (error && error_size && error[0]) {
            char detail[ASGEND_MEMORY_ERROR_MAX];
            (void)snprintf(detail, sizeof(detail), "%s", error);
            set_error(error, error_size, "%s: %s", operation, detail);
        }
        return -1;
    }
    *removed = (int64_t)sqlite3_changes(db);
    return 0;
}

int asgend_memory_gc(const char *root, bool vacuum, FILE *out,
                     char *error, size_t error_size) {
    if (error && error_size) error[0] = '\0';
    if (!out) { set_error(error, error_size, "output stream is required"); return -1; }
    char *canonical = NULL, *db_path = NULL;
    sqlite3 *db = NULL;
    int64_t stale_chunk_vectors = 0, stale_chunks = 0, stale_chunk_states = 0;
    int64_t stale_document_vectors = 0, stale_jobs = 0, stale_edges = 0, stale_docs = 0;
    if (open_index(root, &canonical, &db, &db_path, error, error_size) != 0) return -1;
    if (exec_sql(db, "BEGIN IMMEDIATE", error, error_size) != 0) goto fail;
    if (memory_gc_delete(db,
        "DELETE FROM chunk_embeddings WHERE NOT EXISTS (SELECT 1 FROM file_chunks c JOIN files f ON f.file_id=c.file_id WHERE c.chunk_id=chunk_embeddings.chunk_id AND c.file_id=chunk_embeddings.file_id AND f.deleted_at IS NULL AND f.sha256=c.source_sha256 AND chunk_embeddings.source_sha256=c.source_sha256)",
        "remove stale chunk vectors", &stale_chunk_vectors, error, error_size) != 0 ||
        memory_gc_delete(db,
        "DELETE FROM file_chunks WHERE NOT EXISTS (SELECT 1 FROM files f WHERE f.file_id=file_chunks.file_id AND f.deleted_at IS NULL AND f.sha256=file_chunks.source_sha256)",
        "remove stale chunks", &stale_chunks, error, error_size) != 0 ||
        memory_gc_delete(db,
        "DELETE FROM file_chunk_state WHERE NOT EXISTS (SELECT 1 FROM files f WHERE f.file_id=file_chunk_state.file_id AND f.deleted_at IS NULL AND f.sha256=file_chunk_state.source_sha256)",
        "remove stale chunk states", &stale_chunk_states, error, error_size) != 0 ||
        memory_gc_delete(db,
        "DELETE FROM embeddings WHERE NOT EXISTS (SELECT 1 FROM descriptions d JOIN files f ON f.file_id=d.file_id WHERE d.description_id=embeddings.description_id AND d.file_id=embeddings.file_id AND d.status='complete' AND f.deleted_at IS NULL AND f.sha256=d.source_sha256)",
        "remove stale document vectors", &stale_document_vectors, error, error_size) != 0 ||
        memory_gc_delete(db,
        "DELETE FROM pipeline_jobs WHERE NOT EXISTS (SELECT 1 FROM files f WHERE f.file_id=pipeline_jobs.file_id AND f.deleted_at IS NULL AND f.sha256=pipeline_jobs.source_sha256)",
        "remove stale pipeline jobs", &stale_jobs, error, error_size) != 0 ||
        memory_gc_delete(db,
        "DELETE FROM file_edges WHERE NOT EXISTS (SELECT 1 FROM files s JOIN files t ON t.file_id=file_edges.target_file_id WHERE s.file_id=file_edges.source_file_id AND s.deleted_at IS NULL AND t.deleted_at IS NULL AND s.sha256=file_edges.source_sha256)",
        "remove stale graph edges", &stale_edges, error, error_size) != 0 ||
        memory_gc_delete(db,
        "DELETE FROM memory_search_docs WHERE NOT EXISTS (SELECT 1 FROM files f WHERE f.file_id=memory_search_docs.file_id AND f.deleted_at IS NULL)",
        "remove stale search documents", &stale_docs, error, error_size) != 0) {
        (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        goto fail;
    }
    if (exec_sql(db, "COMMIT", error, error_size) != 0) {
        (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        goto fail;
    }
    /* FTS5 optimization only consolidates index segments; it does not change source rows. */
    if (exec_sql(db, "INSERT INTO memory_fts(memory_fts) VALUES('optimize'); INSERT INTO memory_chunk_fts(memory_chunk_fts) VALUES('optimize');", error, error_size) != 0) goto fail;
    if (vacuum) {
        if (exec_sql(db, "PRAGMA wal_checkpoint(TRUNCATE); VACUUM;", error, error_size) != 0) goto fail;
    }
    fprintf(out,
        "{\"status\":\"ok\",\"stale_search_docs_removed\":%lld,\"stale_chunks_removed\":%lld,\"stale_chunk_states_removed\":%lld,\"stale_chunk_vectors_removed\":%lld,\"stale_document_vectors_removed\":%lld,\"stale_pipeline_jobs_removed\":%lld,\"stale_graph_edges_removed\":%lld,\"vacuum\":%s}\n",
        (long long)stale_docs, (long long)stale_chunks, (long long)stale_chunk_states,
        (long long)stale_chunk_vectors, (long long)stale_document_vectors,
        (long long)stale_jobs, (long long)stale_edges, vacuum ? "true" : "false");
    sqlite3_close(db); free(db_path); free(canonical);
    return ferror(out) ? -1 : 0;
fail:
    if (db) sqlite3_close(db);
    free(db_path); free(canonical);
    return -1;
}

int asgend_memory_events(const char *root, unsigned int limit, FILE *out, char *error, size_t error_size) {
    if (error && error_size) error[0] = '\0';
    if (!out) { set_error(error, error_size, "output stream is required"); return -1; }
    if (limit == 0) limit = DEFAULT_EVENT_LIMIT;
    if (limit > MAX_EVENT_LIMIT) limit = MAX_EVENT_LIMIT;
    char *canonical = NULL, *db_path = NULL; sqlite3 *db = NULL;
    if (open_index(root, &canonical, &db, &db_path, error, error_size) != 0) return -1;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT e.event_id,e.scan_id,e.event_type,COALESCE(e.old_path,''),COALESCE(e.new_path,''),COALESCE(e.details,'') FROM file_events e ORDER BY e.event_id DESC LIMIT ?1";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) { sqlite_error(db, error, error_size, "prepare events query"); goto fail; }
    sqlite3_bind_int(stmt, 1, (int)limit);
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        fprintf(out, "event=%lld scan=%lld type=%s old=%s new=%s detail=%s\n",
            (long long)sqlite3_column_int64(stmt, 0), (long long)sqlite3_column_int64(stmt, 1),
            sqlite3_column_text(stmt, 2), sqlite3_column_text(stmt, 3), sqlite3_column_text(stmt, 4), sqlite3_column_text(stmt, 5));
    }
    if (rc != SQLITE_DONE) { sqlite3_finalize(stmt); sqlite_error(db, error, error_size, "read events"); goto fail; }
    sqlite3_finalize(stmt); sqlite3_close(db); free(db_path); free(canonical); return 0;
fail:
    if (stmt) sqlite3_finalize(stmt);
    sqlite3_close(db); free(db_path); free(canonical); return -1;
}
