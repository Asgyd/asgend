#include "runtime.h"
#ifdef ASGEND_DB_PERF_BENCHMARK
#include "db_perf_timing.h"
#endif
#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>


#ifdef ASGEND_DB_PERF_BENCHMARK
static _Thread_local DbPerfTiming db_perf_last_timing;

static uint64_t db_perf_now_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

void db_perf_timing_last(DbPerfTiming *out) {
    if (out) *out = db_perf_last_timing;
}
#define DB_PERF_RESET() do { memset(&db_perf_last_timing, 0, sizeof(db_perf_last_timing)); } while (0)
#define DB_PERF_ADD(field, start_ns) do { uint64_t _end = db_perf_now_ns(); if (_end >= (start_ns)) db_perf_last_timing.field += _end - (start_ns); } while (0)
#define DB_PERF_NOW() db_perf_now_ns()
#else
#define DB_PERF_RESET() ((void)0)
#define DB_PERF_ADD(field, start_ns) ((void)(start_ns))
#define DB_PERF_NOW() UINT64_C(0)
#endif

static void set_error(char *dst, size_t n, const char *msg) {
    if (dst && n) (void)snprintf(dst, n, "%s", msg ? msg : "database error");
}

static int exec_sql(sqlite3 *db, const char *sql, char *err, size_t err_size) {
    char *sqlite_err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &sqlite_err);
    if (rc != SQLITE_OK) {
        if (err && err_size) (void)snprintf(err, err_size, "SQLite: %s", sqlite_err ? sqlite_err : sqlite3_errmsg(db));
        sqlite3_free(sqlite_err);
        return -1;
    }
    sqlite3_free(sqlite_err);
    return 0;
}

static void make_task_id(char *dst, size_t n) {
    static const char alphabet[] = "0123456789abcdefghjkmnpqrstvwxyz";
    unsigned char random_bytes[10];
    if (!dst || n < 11) return;
    sqlite3_randomness((int)sizeof(random_bytes), random_bytes);
    for (size_t i = 0; i < 10; ++i) dst[i] = alphabet[random_bytes[i] & 31U];
    dst[10] = '\0';
}

static int secure_task_directories(const Runtime *rt, const char *task_id,
                                   char *workspace_path, size_t workspace_size) {
    if (!rt || !task_id || !*task_id || !workspace_path || workspace_size == 0 || !rt->config.asgend_home[0]) {
        errno = EINVAL; return -1;
    }
    const char *home_env = getenv("HOME");
    if (strcmp(rt->config.asgend_home, "/") == 0 || strcmp(rt->config.asgend_home, "/tmp") == 0 ||
        (home_env && *home_env && strcmp(rt->config.asgend_home, home_env) == 0) ||
        strcmp(rt->config.asgend_home, rt->config.root_dir) == 0) {
        errno = EINVAL; return -1;
    }
    char tasks_dir[AO_MAX_PATH];
    if (snprintf(tasks_dir, sizeof(tasks_dir), "%s/tasks", rt->config.asgend_home) >= (int)sizeof(tasks_dir)) {
        errno = ENAMETOOLONG; return -1;
    }
    if (path_mkdirs(rt->config.asgend_home, 0700) != 0) return -1;
    struct stat st;
    if (lstat(rt->config.asgend_home, &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 0022) != 0) {
        errno = EPERM; return -1;
    }
    if (path_mkdirs(tasks_dir, 0700) != 0) return -1;
    if (lstat(tasks_dir, &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode) || st.st_uid != geteuid()) {
        errno = EPERM; return -1;
    }
    if (chmod(tasks_dir, 0700) != 0) return -1;
    if (snprintf(workspace_path, workspace_size, "%s/%s", tasks_dir, task_id) >= (int)workspace_size) {
        errno = ENAMETOOLONG; return -1;
    }
    if (mkdir(workspace_path, 0700) != 0) return errno == EEXIST ? -2 : -1;
    if (lstat(workspace_path, &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode) || st.st_uid != geteuid()) {
        int saved_errno = errno ? errno : EPERM;
        (void)rmdir(workspace_path);
        errno = saved_errno;
        return -1;
    }
    if (chmod(workspace_path, 0700) != 0) {
        int saved_errno = errno;
        (void)rmdir(workspace_path);
        errno = saved_errno;
        return -1;
    }
    return 0;
}

int db_init(Runtime *rt, char *err, size_t err_size) {
    if (path_mkdirs(rt->config.root_dir, 0750) != 0) {
        set_error(err, err_size, "cannot create project root directory"); return -1;
    }
    char db_dir[AO_MAX_PATH];
    if (string_copy(db_dir, sizeof(db_dir), rt->config.database_path) != 0) {
        set_error(err, err_size, "database path too long"); return -1;
    }
    char *slash = strrchr(db_dir, '/');
    if (slash) {
        *slash = '\0';
        if (*db_dir && path_mkdirs(db_dir, 0750) != 0) {
            if (err && err_size) (void)snprintf(err, err_size, "cannot create database directory %s: %s", db_dir, strerror(errno));
            return -1;
        }
    }
    int rc = sqlite3_open_v2(rt->config.database_path, &rt->db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc != SQLITE_OK) {
        if (err && err_size) (void)snprintf(err, err_size, "cannot open SQLite database: %s", rt->db ? sqlite3_errmsg(rt->db) : "out of memory");
        if (rt->db) sqlite3_close(rt->db);
        rt->db = NULL;
        return -1;
    }
    sqlite3_busy_timeout(rt->db, 5000);
    pthread_mutex_lock(&rt->db_mutex);
    int result = 0;
    if (exec_sql(rt->db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON;", err, err_size) != 0) result = -1;
    const char *schema =
        "CREATE TABLE IF NOT EXISTS tasks ("
        " id TEXT PRIMARY KEY, idempotency_key TEXT UNIQUE, task_json TEXT NOT NULL, session_id TEXT, workspace_path TEXT NOT NULL DEFAULT '', title TEXT NOT NULL DEFAULT '',"
        " status TEXT NOT NULL CHECK(status IN ('queued','running','waiting_input','completed','completed_without_model','failed','cancelled')) ,"
        " attempts INTEGER NOT NULL DEFAULT 0, max_attempts INTEGER NOT NULL, cancel_requested INTEGER NOT NULL DEFAULT 0, priority INTEGER NOT NULL DEFAULT 0 CHECK(priority BETWEEN 0 AND 9),"
        " result_json TEXT, error TEXT, next_attempt_at TEXT, created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')) ,"
        " updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')) );"
        "CREATE INDEX IF NOT EXISTS idx_tasks_status_created ON tasks(status, created_at);"
        "CREATE TABLE IF NOT EXISTS sessions (session_id TEXT PRIMARY KEY, updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));"
        "CREATE TABLE IF NOT EXISTS session_messages (message_id INTEGER PRIMARY KEY AUTOINCREMENT, session_id TEXT NOT NULL,"
        " task_id TEXT, role TEXT NOT NULL, content TEXT NOT NULL, created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),"
        " FOREIGN KEY(session_id) REFERENCES sessions(session_id) ON DELETE CASCADE);"
        "CREATE INDEX IF NOT EXISTS idx_session_messages ON session_messages(session_id, message_id);"
        "CREATE TABLE IF NOT EXISTS task_events (event_id INTEGER PRIMARY KEY AUTOINCREMENT, task_id TEXT NOT NULL,"
        " event_type TEXT NOT NULL, actor TEXT NOT NULL, message TEXT NOT NULL, payload_json TEXT,"
        " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));"
        "CREATE INDEX IF NOT EXISTS idx_task_events_task ON task_events(task_id,event_id);"
        "CREATE TABLE IF NOT EXISTS plan_items (item_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " scope_id TEXT NOT NULL DEFAULT 'global', kind TEXT NOT NULL CHECK(kind IN ('todo','goal')),"
        " title TEXT NOT NULL CHECK(length(title)>0 AND length(title)<=160), description TEXT NOT NULL DEFAULT '',"
        " status TEXT NOT NULL DEFAULT 'open' CHECK(status IN ('open','done')), sort_order INTEGER NOT NULL DEFAULT 0,"
        " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),"
        " updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));"
        "CREATE INDEX IF NOT EXISTS idx_plan_scope_kind ON plan_items(scope_id,kind,status,sort_order,item_id);";
    if (result == 0 && exec_sql(rt->db, schema, err, err_size) != 0) result = -1;
    /* Ensure the priority column exists before a possible table rebuild (pre-priority DBs). */
    if (result == 0) {
        bool has_priority_early = false, has_workspace_early = false, has_title_early = false, has_next_attempt_early = false;
        sqlite3_stmt *early_columns = NULL;
        if (sqlite3_prepare_v2(rt->db, "PRAGMA table_info(tasks)", -1, &early_columns, NULL) != SQLITE_OK) result = -1;
        else {
            while (sqlite3_step(early_columns) == SQLITE_ROW) {
                const char *column_name = (const char *)sqlite3_column_text(early_columns, 1);
                if (column_name && strcmp(column_name, "priority") == 0) has_priority_early = true;
                if (column_name && strcmp(column_name, "workspace_path") == 0) has_workspace_early = true;
                if (column_name && strcmp(column_name, "title") == 0) has_title_early = true;
                if (column_name && strcmp(column_name, "next_attempt_at") == 0) has_next_attempt_early = true;
            }
            sqlite3_finalize(early_columns);
        }
        if (result == 0 && !has_priority_early && exec_sql(rt->db,
            "ALTER TABLE tasks ADD COLUMN priority INTEGER NOT NULL DEFAULT 0 CHECK(priority BETWEEN 0 AND 9)", err, err_size) != 0)
            result = -1;
        if (result == 0 && !has_workspace_early && exec_sql(rt->db,
            "ALTER TABLE tasks ADD COLUMN workspace_path TEXT NOT NULL DEFAULT ''", err, err_size) != 0)
            result = -1;
        if (result == 0 && !has_title_early && exec_sql(rt->db,
            "ALTER TABLE tasks ADD COLUMN title TEXT NOT NULL DEFAULT ''", err, err_size) != 0)
            result = -1;
        if (result == 0 && !has_next_attempt_early && exec_sql(rt->db,
            "ALTER TABLE tasks ADD COLUMN next_attempt_at TEXT", err, err_size) != 0)
            result = -1;
    }
    /* Old releases constrained task status without waiting_input. Rebuild the table so
       an interactive clarification pause survives restarts on existing installations. */
    if (result == 0) {
        sqlite3_stmt *schema_stmt = NULL;
        bool supports_waiting_input = false;
        if (sqlite3_prepare_v2(rt->db, "SELECT sql FROM sqlite_master WHERE type='table' AND name='tasks'", -1, &schema_stmt, NULL) != SQLITE_OK) result = -1;
        else {
            if (sqlite3_step(schema_stmt) == SQLITE_ROW) {
                const char *sql = (const char *)sqlite3_column_text(schema_stmt, 0);
                supports_waiting_input = sql && strstr(sql, "waiting_input") != NULL;
            }
            sqlite3_finalize(schema_stmt);
        }
        if (result == 0 && !supports_waiting_input) {
            const char *migration =
                "BEGIN IMMEDIATE;"
                "DROP INDEX IF EXISTS idx_tasks_status_created;"
                "DROP INDEX IF EXISTS idx_tasks_queue_priority;"
                "CREATE TABLE tasks_new ("
                " id TEXT PRIMARY KEY, idempotency_key TEXT UNIQUE, task_json TEXT NOT NULL, session_id TEXT, workspace_path TEXT NOT NULL DEFAULT '', title TEXT NOT NULL DEFAULT '',"
                " status TEXT NOT NULL CHECK(status IN ('queued','running','waiting_input','completed','completed_without_model','failed','cancelled')) ,"
                " attempts INTEGER NOT NULL DEFAULT 0, max_attempts INTEGER NOT NULL, cancel_requested INTEGER NOT NULL DEFAULT 0, priority INTEGER NOT NULL DEFAULT 0 CHECK(priority BETWEEN 0 AND 9),"
                " result_json TEXT, error TEXT, next_attempt_at TEXT, created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')) ,"
                " updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));"
                "INSERT INTO tasks_new(id,idempotency_key,task_json,session_id,workspace_path,title,status,attempts,max_attempts,cancel_requested,priority,result_json,error,next_attempt_at,created_at,updated_at)"
                " SELECT id,idempotency_key,task_json,session_id,workspace_path,title,status,attempts,max_attempts,cancel_requested,priority,result_json,error,next_attempt_at,created_at,updated_at FROM tasks;"
                "DROP TABLE tasks; ALTER TABLE tasks_new RENAME TO tasks;"
                "CREATE INDEX idx_tasks_status_created ON tasks(status,created_at);"
                "CREATE INDEX idx_tasks_queue_priority ON tasks(status,priority DESC,created_at);"
                "COMMIT;";
            if (exec_sql(rt->db, migration, err, err_size) != 0) {
                (void)sqlite3_exec(rt->db, "ROLLBACK", NULL, NULL, NULL);
                result = -1;
            }
        }
    }
    /* Additive migration for databases created by prior releases. */
    if (result == 0) {
        bool has_priority = false;
        sqlite3_stmt *columns = NULL;
        if (sqlite3_prepare_v2(rt->db, "PRAGMA table_info(tasks)", -1, &columns, NULL) != SQLITE_OK) result = -1;
        else {
            while (sqlite3_step(columns) == SQLITE_ROW) {
                const char *column_name = (const char *)sqlite3_column_text(columns, 1);
                if (column_name && strcmp(column_name, "priority") == 0) has_priority = true;
            }
            sqlite3_finalize(columns);
        }
        if (result == 0 && !has_priority && exec_sql(rt->db,
            "ALTER TABLE tasks ADD COLUMN priority INTEGER NOT NULL DEFAULT 0 CHECK(priority BETWEEN 0 AND 9)", err, err_size) != 0)
            result = -1;
        if (result == 0 && exec_sql(rt->db,
            "CREATE INDEX IF NOT EXISTS idx_tasks_queue_priority ON tasks(status, priority DESC, created_at)", err, err_size) != 0)
            result = -1;
    }
    pthread_mutex_unlock(&rt->db_mutex);
    return result;
}

void db_close(Runtime *rt) {
    if (!rt) return;
    pthread_mutex_lock(&rt->db_mutex);
    sqlite3 *db = rt->db;
    if (!db) {
        pthread_mutex_unlock(&rt->db_mutex);
        return;
    }

    int rc = sqlite3_close(db);
    if (rc == SQLITE_OK) {
        rt->db = NULL;
        pthread_mutex_unlock(&rt->db_mutex);
        return;
    }

    /* sqlite3_close() leaves the handle open on SQLITE_BUSY. Report this
     * lifecycle bug and ask SQLite to defer destruction until outstanding
     * statements/backups are finalized instead of losing the only handle. */
    fprintf(stderr, "SQLite close reported outstanding resources (code %d); deferring close\n", rc);
    int deferred_rc = sqlite3_close_v2(db);
    if (deferred_rc == SQLITE_OK) {
        rt->db = NULL;
    } else {
        fprintf(stderr, "SQLite deferred close failed (code %d)\n", deferred_rc);
    }
    pthread_mutex_unlock(&rt->db_mutex);
}

int db_recover_running(Runtime *rt, char *err, size_t err_size) {
    const char *sql =
        "UPDATE tasks SET status=CASE WHEN cancel_requested=1 THEN 'cancelled' WHEN attempts < max_attempts THEN 'queued' ELSE 'failed' END,"
        " error=CASE WHEN cancel_requested=1 THEN 'cancelled during previous process lifetime' WHEN attempts < max_attempts THEN 'recovered after server restart' ELSE 'worker stopped during final attempt; retries exhausted' END,"
        " next_attempt_at=CASE WHEN cancel_requested=0 AND attempts < max_attempts THEN strftime('%Y-%m-%dT%H:%M:%fZ','now','+' || (CASE WHEN attempts<=1 THEN 1 WHEN attempts=2 THEN 2 WHEN attempts=3 THEN 4 WHEN attempts=4 THEN 8 WHEN attempts=5 THEN 16 ELSE 30 END) || ' seconds') ELSE NULL END,"
        " updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE status='running'";
    pthread_mutex_lock(&rt->db_mutex);
    int rc = exec_sql(rt->db, sql, err, err_size);
    pthread_mutex_unlock(&rt->db_mutex);
    return rc;
}

static void make_fallback_title(const char *task_json, char *dst, size_t dst_size) {
    if (!dst || dst_size == 0) return;
    (void)snprintf(dst, dst_size, "%s", "Новая задача");
    json_object *obj = task_json ? json_tokener_parse(task_json) : NULL;
    if (!obj || json_object_get_type(obj) != json_type_object) { if (obj) json_object_put(obj); return; }
    const char *task = NULL;
    if (json_get_string(obj, "task", &task) != 0 && json_get_string(obj, "prompt", &task) != 0) task = NULL;
    if (!task || !*task) { json_object_put(obj); return; }
    size_t used = 0;
    bool in_space = true;
    for (const unsigned char *p = (const unsigned char *)task; *p && *p != '\n' && *p != '\r'; ++p) {
        if (isspace(*p)) {
            if (!in_space && used + 1 < dst_size) dst[used++] = ' ';
            in_space = true;
            continue;
        }
        if (used + 1 >= dst_size) break;
        dst[used++] = (char)*p;
        in_space = false;
    }
    while (used && dst[used - 1] == ' ') used--;
    /* If the byte cap cut a multibyte UTF-8 character, remove that partial character. */
    if (used + 1 >= dst_size) {
        size_t lead = used;
        while (lead > 0 && (((unsigned char)dst[lead - 1]) & 0xc0) == 0x80) lead--;
        if (lead > 0) {
            unsigned char c = (unsigned char)dst[lead - 1];
            size_t expected = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc2 ? 2 : 1;
            size_t present = used - (lead - 1);
            if (expected > present) used = lead - 1;
        } else if (used && (((unsigned char)dst[used - 1]) & 0xc0) == 0x80) {
            used = 0;
        }
    }
    if (used == 0) (void)snprintf(dst, dst_size, "%s", "Новая задача");
    else dst[used] = '\0';
    json_object_put(obj);
}

int db_enqueue(Runtime *rt, const char *task_json, const char *session_id,
               const char *idempotency_key, int priority, char *task_id, size_t task_id_size,
               bool *duplicate, char *err, size_t err_size) {
    if (!rt || !task_json || !task_id || !task_id_size) {
        set_error(err, err_size, "invalid enqueue arguments");
        return -1;
    }
    if (err && err_size) err[0] = '\0';
    if (duplicate) *duplicate = false;

    char workspace_path[AO_MAX_PATH] = {0};
    bool workspace_created = false;
    bool transaction_open = false;
    int failure_rc = -1;
    sqlite3_stmt *stmt = NULL;
    int rc;

    pthread_mutex_lock(&rt->db_mutex);
    rc = sqlite3_exec(rt->db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        if (err && err_size) (void)snprintf(err, err_size, "SQLite enqueue begin: %s", sqlite3_errmsg(rt->db));
        pthread_mutex_unlock(&rt->db_mutex);
        return -1;
    }
    transaction_open = true;

    /* Serialize idempotency and capacity checks with the insert, including when
       more than one core process shares this database. */
    if (idempotency_key && *idempotency_key) {
        rc = sqlite3_prepare_v2(rt->db, "SELECT id FROM tasks WHERE idempotency_key=?1", -1, &stmt, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_bind_text(stmt, 1, idempotency_key, -1, SQLITE_TRANSIENT);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            const unsigned char *existing = sqlite3_column_text(stmt, 0);
            if (!existing || string_copy(task_id, task_id_size, (const char *)existing) != 0) {
                set_error(err, err_size, "stored task ID too long");
                goto fail;
            }
            sqlite3_finalize(stmt);
            stmt = NULL;
            rc = sqlite3_exec(rt->db, "COMMIT", NULL, NULL, NULL);
            if (rc != SQLITE_OK) goto fail;
            transaction_open = false;
            if (duplicate) *duplicate = true;
            pthread_mutex_unlock(&rt->db_mutex);
            return 0;
        }
        sqlite3_finalize(stmt);
        stmt = NULL;
        if (rc != SQLITE_DONE) goto fail;
    }

    rc = sqlite3_prepare_v2(rt->db,
        "SELECT COUNT(*) FROM tasks WHERE status IN ('queued','running')", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto fail;
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto fail;
    sqlite3_int64 pending = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (pending >= rt->config.queue_max_pending) {
        set_error(err, err_size, "task queue is full");
        failure_rc = -2;
        goto fail;
    }

    for (int attempt = 0; attempt < 32; ++attempt) {
        make_task_id(task_id, task_id_size);
        if (!task_id[0]) {
            set_error(err, err_size, "task ID buffer is too small");
            goto fail;
        }
        rc = sqlite3_prepare_v2(rt->db, "SELECT 1 FROM tasks WHERE id=?1", -1, &stmt, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
        if (rc != SQLITE_OK) goto fail;
        int exists_rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        stmt = NULL;
        if (exists_rc == SQLITE_ROW) continue;
        if (exists_rc != SQLITE_DONE) { rc = exists_rc; goto fail; }

        int workspace_rc = secure_task_directories(rt, task_id, workspace_path, sizeof(workspace_path));
        if (workspace_rc == -2) continue;
        if (workspace_rc != 0) {
            if (err && err_size) (void)snprintf(err, err_size,
                "cannot securely create task workspace: %s", strerror(errno));
            goto fail;
        }
        workspace_created = true;
        break;
    }
    if (!workspace_created) {
        set_error(err, err_size, "could not allocate a unique task ID after 32 attempts");
        goto fail;
    }

    char task_title[160];
    make_fallback_title(task_json, task_title, sizeof(task_title));
    rc = sqlite3_prepare_v2(rt->db,
        "INSERT INTO tasks(id,idempotency_key,task_json,session_id,workspace_path,title,status,attempts,max_attempts,priority) "
        "VALUES(?1,?2,?3,?4,?5,?6,'queued',0,?7,?8)", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto fail;
    rc = sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) {
        if (idempotency_key && *idempotency_key)
            rc = sqlite3_bind_text(stmt, 2, idempotency_key, -1, SQLITE_TRANSIENT);
        else rc = sqlite3_bind_null(stmt, 2);
    }
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 3, task_json, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) {
        if (session_id && *session_id) rc = sqlite3_bind_text(stmt, 4, session_id, -1, SQLITE_TRANSIENT);
        else rc = sqlite3_bind_null(stmt, 4);
    }
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 5, workspace_path, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 6, task_title, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 7, rt->config.max_retries + 1);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 8, priority < 0 ? 0 : (priority > 9 ? 9 : priority));
    if (rc != SQLITE_OK) goto fail;
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (rc != SQLITE_DONE) goto fail;

    if (session_id && *session_id) {
        rc = sqlite3_prepare_v2(rt->db,
            "INSERT INTO sessions(session_id) VALUES(?1) ON CONFLICT(session_id) DO UPDATE SET updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now')",
            -1, &stmt, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        stmt = NULL;
        if (rc != SQLITE_DONE) goto fail;
    }

    rc = sqlite3_exec(rt->db, "COMMIT", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    transaction_open = false;
    pthread_mutex_unlock(&rt->db_mutex);
    return 0;

fail:
    if (stmt) {
        sqlite3_finalize(stmt);
        stmt = NULL;
    }
    if (err && err_size && !err[0])
        (void)snprintf(err, err_size, "SQLite enqueue: %s", sqlite3_errmsg(rt->db));
    if (transaction_open) {
        (void)sqlite3_exec(rt->db, "ROLLBACK", NULL, NULL, NULL);
        transaction_open = false;
    }
    if (workspace_created && workspace_path[0]) (void)rmdir(workspace_path);
    pthread_mutex_unlock(&rt->db_mutex);
    return failure_rc;
}

int db_claim_next(Runtime *rt, ClaimedTask *claim, char *err, size_t err_size) {
    if (!claim) { set_error(err, err_size, "claim output is required"); return -1; }
    memset(claim, 0, sizeof(*claim));
    pthread_mutex_lock(&rt->db_mutex);
    int rc = sqlite3_exec(rt->db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(rt->db,
        "SELECT id,task_json,attempts FROM tasks WHERE status='queued' AND cancel_requested=0 "
        "AND (next_attempt_at IS NULL OR next_attempt_at <= strftime('%Y-%m-%dT%H:%M:%fZ','now')) "
        "ORDER BY MIN(9, priority + CAST(MAX(0, (julianday('now')-julianday(created_at))*86400 / ?1) AS INTEGER)) DESC, created_at ASC, rowid ASC LIMIT 1",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto rollback;
    sqlite3_bind_int(stmt, 1, rt->config.priority_aging_seconds);
    if (rc != SQLITE_OK) goto rollback;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        sqlite3_finalize(stmt);
        stmt = NULL;
        rc = sqlite3_exec(rt->db, "COMMIT", NULL, NULL, NULL);
        if (rc != SQLITE_OK) {
            if (err && err_size) (void)snprintf(err, err_size, "SQLite commit empty claim: %s", sqlite3_errmsg(rt->db));
            (void)sqlite3_exec(rt->db, "ROLLBACK", NULL, NULL, NULL);
            pthread_mutex_unlock(&rt->db_mutex);
            return -1;
        }
        pthread_mutex_unlock(&rt->db_mutex);
        return 0;
    }
    if (rc != SQLITE_ROW) goto rollback;
    const char *id = (const char *)sqlite3_column_text(stmt, 0);
    const char *raw = (const char *)sqlite3_column_text(stmt, 1);
    int previous_attempts = sqlite3_column_int(stmt, 2);
    claim->id = string_duplicate(id ? id : "");
    claim->task_json = string_duplicate(raw ? raw : "{}");
    claim->attempt = previous_attempts + 1;
    sqlite3_finalize(stmt); stmt = NULL;
    if (!claim->id || !claim->task_json) { rc = SQLITE_NOMEM; goto rollback; }
    rc = sqlite3_prepare_v2(rt->db, "UPDATE tasks SET status='running',attempts=attempts+1,updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now'),error=NULL WHERE id=?1 AND status='queued'", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto rollback;
    sqlite3_bind_text(stmt, 1, claim->id, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE || sqlite3_changes(rt->db) != 1) goto rollback;
    sqlite3_finalize(stmt); stmt = NULL;
    rc = sqlite3_exec(rt->db, "COMMIT", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail_after_claim;
    pthread_mutex_unlock(&rt->db_mutex);
    return 1;
rollback:
    if (stmt) sqlite3_finalize(stmt);
    (void)sqlite3_exec(rt->db, "ROLLBACK", NULL, NULL, NULL);
    free(claim->id); free(claim->task_json); memset(claim, 0, sizeof(*claim));
fail:
    if (err && err_size) (void)snprintf(err, err_size, "SQLite claim: %s", sqlite3_errmsg(rt->db));
    pthread_mutex_unlock(&rt->db_mutex);
    return -1;
fail_after_claim:
    /* A failed COMMIT can leave the connection inside a transaction; release it before reuse. */
    (void)sqlite3_exec(rt->db, "ROLLBACK", NULL, NULL, NULL);
    free(claim->id); free(claim->task_json); memset(claim, 0, sizeof(*claim));
    if (err && err_size) (void)snprintf(err, err_size, "SQLite commit claim: %s", sqlite3_errmsg(rt->db));
    pthread_mutex_unlock(&rt->db_mutex);
    return -1;
}

void db_claim_free(ClaimedTask *claim) {
    if (!claim) return;
    free(claim->id); free(claim->task_json); memset(claim, 0, sizeof(*claim));
}

int db_task_cancelled(Runtime *rt, const char *task_id) {
    if (!rt || !task_id) return 1;
    pthread_mutex_lock(&rt->db_mutex);
    sqlite3_stmt *stmt = NULL;
    int cancelled = 1;
    if (sqlite3_prepare_v2(rt->db, "SELECT cancel_requested,status FROM tasks WHERE id=?1", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *status = (const char *)sqlite3_column_text(stmt, 1);
            cancelled = sqlite3_column_int(stmt, 0) != 0 || !status || strcmp(status, "running") != 0;
        }
        sqlite3_finalize(stmt);
    }
    pthread_mutex_unlock(&rt->db_mutex);
    return cancelled;
}

int db_finish(Runtime *rt, const char *task_id, int attempt, bool success,
              const char *result_json, const char *error, bool *retry,
              char *err, size_t err_size) {
    if (retry) *retry = false;
    pthread_mutex_lock(&rt->db_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(rt->db, "SELECT cancel_requested,max_attempts FROM tasks WHERE id=?1", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto fail;
    sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) { sqlite3_finalize(stmt); stmt = NULL; goto fail; }
    bool cancelled = sqlite3_column_int(stmt, 0) != 0;
    int max_attempts = sqlite3_column_int(stmt, 1);
    sqlite3_finalize(stmt); stmt = NULL;

    const char *status = "failed";
    const char *error_to_store = error;
    if (cancelled) { status = "cancelled"; error_to_store = "cancel requested"; }
    else if (success) {
        json_object *res = result_json ? json_tokener_parse(result_json) : NULL;
        const char *res_status = NULL;
        if (res && json_get_string(res, "status", &res_status) == 0) {
            if (strcmp(res_status, "waiting_input") == 0) status = "waiting_input";
            else if (strcmp(res_status, "completed_without_model") == 0) status = "completed_without_model";
            else status = "completed";
        } else status = "completed";
        if (res) json_object_put(res);
        error_to_store = strcmp(status, "waiting_input") == 0 ? "Awaiting user input" : NULL;
    } else if (attempt < max_attempts) {
        status = "queued";
        if (retry) *retry = true;
    }
    int retry_delay = attempt <= 1 ? 1 : attempt == 2 ? 2 : attempt == 3 ? 4 : attempt == 4 ? 8 : attempt == 5 ? 16 : 30;
    rc = sqlite3_prepare_v2(rt->db,
        "UPDATE tasks SET status=?2,result_json=?3,error=?4,"
        "next_attempt_at=CASE WHEN ?5=1 THEN strftime('%Y-%m-%dT%H:%M:%fZ','now','+' || ?6 || ' seconds') ELSE NULL END,"
        "updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE id=?1", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto fail;
    sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, status, -1, SQLITE_TRANSIENT);
    if (result_json) sqlite3_bind_text(stmt, 3, result_json, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(stmt, 3);
    if (error_to_store) sqlite3_bind_text(stmt, 4, error_to_store, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(stmt, 4);
    sqlite3_bind_int(stmt, 5, strcmp(status, "queued") == 0 && !cancelled && !success ? 1 : 0);
    sqlite3_bind_int(stmt, 6, retry_delay);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) goto fail;
    pthread_mutex_unlock(&rt->db_mutex);
    if (strcmp(status, "waiting_input") == 0) {
        json_object *payload = result_json ? json_tokener_parse(result_json) : NULL;
        const char *question_text = NULL;
        json_object *question = NULL;
        if (payload && json_object_object_get_ex(payload, "question", &question) && question)
            (void)json_get_string(question, "question", &question_text);
        (void)db_append_task_event(rt, task_id, "question", "orchestrator",
                                   question_text ? question_text : "Clarification is required before continuing.", payload);
        if (payload) json_object_put(payload);
    } else if (strcmp(status, "queued") == 0) {
        json_object *payload = json_object_new_object();
        json_object_object_add(payload, "attempt", json_object_new_int(attempt));
        json_object_object_add(payload, "retry_delay_seconds", json_object_new_int(retry_delay));
        (void)db_append_task_event(rt, task_id, "task_retry_scheduled", "orchestrator",
                                   error ? error : "Task failed; retry scheduled with backoff.", payload);
        if (payload) json_object_put(payload);
    } else {
        const char *event_type = strcmp(status, "cancelled") == 0 ? "task_cancelled" :
                                 success ? "task_completed" : "task_failed";
        const char *message = strcmp(status, "cancelled") == 0 ? "Task cancellation completed." :
                              success ? "Task execution finished." : (error ? error : "Task execution failed.");
        (void)db_append_task_event(rt, task_id, event_type, "orchestrator", message, NULL);
    }
    return 0;
fail:
    if (stmt) sqlite3_finalize(stmt);
    if (err && err_size) (void)snprintf(err, err_size, "SQLite finish: %s", sqlite3_errmsg(rt->db));
    pthread_mutex_unlock(&rt->db_mutex);
    return -1;
}

int db_release_claim(Runtime *rt, const char *task_id, int attempt, char *err, size_t err_size) {
    if (!rt || !task_id || !*task_id || attempt < 1) {
        set_error(err, err_size, "invalid claim-release arguments");
        return -1;
    }
    pthread_mutex_lock(&rt->db_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(rt->db,
        "SELECT cancel_requested FROM tasks WHERE id=?1 AND status='running' AND attempts=?2",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, attempt);
        rc = sqlite3_step(stmt);
    }
    if (rc == SQLITE_DONE) {
        if (stmt) sqlite3_finalize(stmt);
        pthread_mutex_unlock(&rt->db_mutex);
        return 0;
    }
    if (rc != SQLITE_ROW) {
        if (stmt) sqlite3_finalize(stmt);
        if (err && err_size) (void)snprintf(err, err_size, "SQLite inspect claim: %s", sqlite3_errmsg(rt->db));
        pthread_mutex_unlock(&rt->db_mutex);
        return -1;
    }
    bool was_cancelled = sqlite3_column_int(stmt, 0) != 0;
    sqlite3_finalize(stmt); stmt = NULL;
    rc = sqlite3_prepare_v2(rt->db,
        "UPDATE tasks SET status=CASE WHEN cancel_requested=1 THEN 'cancelled' ELSE 'queued' END,"
        "attempts=MAX(0,attempts-1),next_attempt_at=NULL,"
        "error=CASE WHEN cancel_requested=1 THEN 'cancelled before execution during shutdown' ELSE NULL END,"
        "updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE id=?1 AND status='running' AND attempts=?2",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, attempt);
        rc = sqlite3_step(stmt);
    }
    int changed = rc == SQLITE_DONE ? sqlite3_changes(rt->db) : 0;
    if (stmt) sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        if (err && err_size) (void)snprintf(err, err_size, "SQLite release claim: %s", sqlite3_errmsg(rt->db));
        pthread_mutex_unlock(&rt->db_mutex);
        return -1;
    }
    pthread_mutex_unlock(&rt->db_mutex);
    if (changed) (void)db_append_task_event(rt, task_id,
        was_cancelled ? "task_cancelled" : "task_deferred_shutdown", "orchestrator",
        was_cancelled ? "Task cancelled before execution while shutdown began." :
        "Worker shutdown started before execution; claim released without consuming an attempt.", NULL);
    return changed;
}

int db_retry_failed_task(Runtime *rt, const char *task_id, char *err, size_t err_size) {
    if (!rt || !task_id || !*task_id) {
        set_error(err, err_size, "task_id is required");
        return -1;
    }
    pthread_mutex_lock(&rt->db_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(rt->db,
        "UPDATE tasks SET status='queued',max_attempts=attempts+1,"
        "cancel_requested=0,result_json=NULL,error=NULL,next_attempt_at=NULL,"
        "updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE id=?1 AND status='failed'",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
    }
    int changed = rc == SQLITE_DONE ? sqlite3_changes(rt->db) : 0;
    if (stmt) sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        if (err && err_size) (void)snprintf(err, err_size, "SQLite retry failed task: %s", sqlite3_errmsg(rt->db));
        pthread_mutex_unlock(&rt->db_mutex);
        return -1;
    }
    pthread_mutex_unlock(&rt->db_mutex);
    if (changed) (void)db_append_task_event(rt, task_id, "task_redriven", "operator",
                                            "Dead-letter task manually queued for one additional attempt.", NULL);
    return changed;
}

int db_cancel_task(Runtime *rt, const char *task_id, char *err, size_t err_size) {
    pthread_mutex_lock(&rt->db_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(rt->db,
        "UPDATE tasks SET status=CASE WHEN status='queued' THEN 'cancelled' ELSE status END,"
        " cancel_requested=1,updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE id=?1 AND status IN ('queued','running')",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto fail;
    sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) goto fail;
    int changed = sqlite3_changes(rt->db);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&rt->db_mutex);
    return changed > 0 ? 1 : 0;
fail:
    if (stmt) sqlite3_finalize(stmt);
    if (err && err_size) (void)snprintf(err, err_size, "SQLite cancel: %s", sqlite3_errmsg(rt->db));
    pthread_mutex_unlock(&rt->db_mutex);
    return -1;
}

int db_task_set_title(Runtime *rt, const char *task_id, const char *title, char *err, size_t err_size) {
    if (!rt || !task_id || !*task_id || !title || !*title || strlen(title) > 150) {
        set_error(err, err_size, "invalid task title"); return -1;
    }
    pthread_mutex_lock(&rt->db_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(rt->db, "UPDATE tasks SET title=?1,updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE id=?2", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto title_fail;
    sqlite3_bind_text(stmt, 1, title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, task_id, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) goto title_fail;
    int changed = sqlite3_changes(rt->db);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&rt->db_mutex);
    return changed == 1 ? 0 : -1;
title_fail:
    if (stmt) sqlite3_finalize(stmt);
    if (err && err_size) (void)snprintf(err, err_size, "SQLite task title: %s", sqlite3_errmsg(rt->db));
    pthread_mutex_unlock(&rt->db_mutex);
    return -1;
}

json_object *db_task_status(Runtime *rt, const char *task_id, char *err, size_t err_size) {
    if (!rt || !task_id) return NULL;
    pthread_mutex_lock(&rt->db_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(rt->db,
        "SELECT id,status,attempts,max_attempts,cancel_requested,result_json,error,created_at,updated_at,session_id,priority,task_json,workspace_path,title,next_attempt_at FROM tasks WHERE id=?1", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto fail;
    sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) { sqlite3_finalize(stmt); pthread_mutex_unlock(&rt->db_mutex); return NULL; }
    if (rc != SQLITE_ROW) goto fail;
    json_object *out = json_object_new_object();
    if (!out) { sqlite3_finalize(stmt); pthread_mutex_unlock(&rt->db_mutex); return NULL; }
    const char *id = (const char *)sqlite3_column_text(stmt, 0);
    const char *status = (const char *)sqlite3_column_text(stmt, 1);
    const char *result = (const char *)sqlite3_column_text(stmt, 5);
    const char *error = (const char *)sqlite3_column_text(stmt, 6);
    const char *created = (const char *)sqlite3_column_text(stmt, 7);
    const char *updated = (const char *)sqlite3_column_text(stmt, 8);
    const char *session = (const char *)sqlite3_column_text(stmt, 9);
    const char *task_json = (const char *)sqlite3_column_text(stmt, 11);
    const char *workspace_path = (const char *)sqlite3_column_text(stmt, 12);
    const char *title = (const char *)sqlite3_column_text(stmt, 13);
    const char *next_attempt_at = (const char *)sqlite3_column_text(stmt, 14);
    if (title && *title) json_object_object_add(out, "title", json_object_new_string(title));
    else if (task_json) {
        char fallback_title[160]; make_fallback_title(task_json, fallback_title, sizeof(fallback_title));
        if (*fallback_title) json_object_object_add(out, "title", json_object_new_string(fallback_title));
    }
    if (task_json) {
        json_object *task_request = json_tokener_parse(task_json);
        if (task_request) {
            json_object *task_value = NULL;
            if ((json_object_object_get_ex(task_request, "task", &task_value) ||
                 json_object_object_get_ex(task_request, "prompt", &task_value)) &&
                task_value && json_object_get_type(task_value) == json_type_string) {
                json_object_object_add(out, "task", json_object_new_string(json_object_get_string(task_value)));
            }
            json_object_put(task_request);
        }
    }
    json_object_object_add(out, "task_id", json_object_new_string(id ? id : ""));
    json_object_object_add(out, "status", json_object_new_string(status ? status : "unknown"));
    json_object_object_add(out, "attempts", json_object_new_int(sqlite3_column_int(stmt, 2)));
    json_object_object_add(out, "priority", json_object_new_int(sqlite3_column_int(stmt, 10)));
    json_object_object_add(out, "max_attempts", json_object_new_int(sqlite3_column_int(stmt, 3)));
    json_object_object_add(out, "cancel_requested", json_object_new_boolean(sqlite3_column_int(stmt, 4) != 0));
    if (workspace_path && *workspace_path) json_object_object_add(out, "workspace_path", json_object_new_string(workspace_path));
    if (created) json_object_object_add(out, "created_at", json_object_new_string(created));
    if (updated) json_object_object_add(out, "updated_at", json_object_new_string(updated));
    if (next_attempt_at) json_object_object_add(out, "next_attempt_at", json_object_new_string(next_attempt_at));
    if (session) json_object_object_add(out, "session_id", json_object_new_string(session));
    if (error) json_object_object_add(out, "error", json_object_new_string(error));
    if (result) {
        json_object *parsed = json_tokener_parse(result);
        if (parsed) json_object_object_add(out, "result", parsed);
        else json_object_object_add(out, "result_raw", json_object_new_string(result));
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&rt->db_mutex);
    return out;
fail:
    if (stmt) sqlite3_finalize(stmt);
    if (err && err_size) (void)snprintf(err, err_size, "SQLite status: %s", sqlite3_errmsg(rt->db));
    pthread_mutex_unlock(&rt->db_mutex);
    return NULL;
}

static json_object *db_task_row_from_stmt(sqlite3_stmt *stmt) {
    json_object *row = json_object_new_object();
    if (!row) return NULL;
    const char *id = (const char *)sqlite3_column_text(stmt, 0);
    const char *status = (const char *)sqlite3_column_text(stmt, 1);
    const char *created = (const char *)sqlite3_column_text(stmt, 6);
    const char *updated = (const char *)sqlite3_column_text(stmt, 7);
    const char *session = (const char *)sqlite3_column_text(stmt, 8);
    const char *error = (const char *)sqlite3_column_text(stmt, 9);
    json_object_object_add(row, "task_id", json_object_new_string(id ? id : ""));
    json_object_object_add(row, "status", json_object_new_string(status ? status : "unknown"));
    json_object_object_add(row, "attempts", json_object_new_int(sqlite3_column_int(stmt, 2)));
    json_object_object_add(row, "max_attempts", json_object_new_int(sqlite3_column_int(stmt, 3)));
    json_object_object_add(row, "priority", json_object_new_int(sqlite3_column_int(stmt, 4)));
    json_object_object_add(row, "cancel_requested", json_object_new_boolean(sqlite3_column_int(stmt, 5) != 0));
    if (created) json_object_object_add(row, "created_at", json_object_new_string(created));
    if (updated) json_object_object_add(row, "updated_at", json_object_new_string(updated));
    if (session) json_object_object_add(row, "session_id", json_object_new_string(session));
    if (error) json_object_object_add(row, "error", json_object_new_string(error));
    json_object_object_add(row, "effective_priority", json_object_new_int(sqlite3_column_int(stmt, 10)));
    const char *workspace_path = (const char *)sqlite3_column_text(stmt, 11);
    const char *title = (const char *)sqlite3_column_text(stmt, 12);
    const char *task_json = (const char *)sqlite3_column_text(stmt, 13);
    const char *next_attempt_at = (const char *)sqlite3_column_text(stmt, 14);
    if (next_attempt_at) json_object_object_add(row, "next_attempt_at", json_object_new_string(next_attempt_at));
    if (workspace_path && *workspace_path) json_object_object_add(row, "workspace_path", json_object_new_string(workspace_path));
    if (title && *title) json_object_object_add(row, "title", json_object_new_string(title));
    else if (task_json) {
        char fallback_title[160];
        make_fallback_title(task_json, fallback_title, sizeof(fallback_title));
        if (*fallback_title) json_object_object_add(row, "title", json_object_new_string(fallback_title));
    }
    return row;
}

static int db_task_list_append_rows(sqlite3_stmt *stmt, json_object *arr,
                                    char *err, size_t err_size, sqlite3 *db) {
    int rc;
    for (;;) {
        uint64_t step_started = DB_PERF_NOW();
        rc = sqlite3_step(stmt);
        DB_PERF_ADD(sqlite_step_ns, step_started);
        if (rc != SQLITE_ROW) break;
        uint64_t mapping_started = DB_PERF_NOW();
        json_object *row = db_task_row_from_stmt(stmt);
        if (!row) {
            DB_PERF_ADD(json_mapping_ns, mapping_started);
            set_error(err, err_size, "out of memory building task list");
            return -1;
        }
        json_object_array_add(arr, row);
        DB_PERF_ADD(json_mapping_ns, mapping_started);
    }
    if (rc != SQLITE_DONE) {
        if (err && err_size) (void)snprintf(err, err_size, "SQLite list tasks: %s", sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

#ifdef ASGEND_DB_PERF_BENCHMARK
static int db_task_list_run_candidate_band(Runtime *rt, json_object *arr, const char *sql,
                                           int bind_kind, int limit, char *err, size_t err_size) {
    sqlite3_stmt *stmt = NULL;
    uint64_t prepare_started = DB_PERF_NOW();
    int rc = sqlite3_prepare_v2(rt->db, sql, -1, &stmt, NULL);
    DB_PERF_ADD(prepare_ns, prepare_started);
    if (rc != SQLITE_OK) {
        if (err && err_size) (void)snprintf(err, err_size, "SQLite candidate prepare: %s", sqlite3_errmsg(rt->db));
        if (stmt) sqlite3_finalize(stmt);
        return -1;
    }
    if (bind_kind == 1) {
        sqlite3_bind_int(stmt, 1, rt->config.priority_aging_seconds);
        sqlite3_bind_int(stmt, 2, limit);
    } else {
        sqlite3_bind_int(stmt, 1, limit);
    }
    int result = db_task_list_append_rows(stmt, arr, err, err_size, rt->db);
    sqlite3_finalize(stmt);
    return result;
}

#endif

static json_object *db_list_tasks_impl(Runtime *rt, const char *status_filter, int limit,
                                       char *err, size_t err_size, bool candidate) {
    if (!rt) return NULL;
    DB_PERF_RESET();
    if (limit < 1) limit = 50;
    if (limit > 200) limit = 200;
    static const char *allowed[] = {"queued", "running", "waiting_input", "completed", "completed_without_model", "failed", "cancelled"};
    if (status_filter && *status_filter) {
        bool found = false;
        for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i)
            if (strcmp(status_filter, allowed[i]) == 0) found = true;
        if (!found) { set_error(err, err_size, "invalid status filter"); return NULL; }
    }
    uint64_t lock_started = DB_PERF_NOW();
    pthread_mutex_lock(&rt->db_mutex);
#ifdef ASGEND_DB_PERF_BENCHMARK
    DB_PERF_ADD(mutex_wait_ns, lock_started);
#else
    (void)lock_started;
#endif
    json_object *arr = json_object_new_array();
    if (!arr) goto fail;

    if (candidate && !(status_filter && *status_filter)) {
#ifdef ASGEND_DB_PERF_BENCHMARK
        /* Experimental three-band path. It is compiled only into the benchmark helper. */
        const char *running_sql =
            "SELECT id,status,attempts,max_attempts,priority,cancel_requested,created_at,updated_at,session_id,error,"
            "priority AS effective_priority,workspace_path,title,task_json,next_attempt_at FROM tasks INDEXED BY idx_ab_running_order "
            "WHERE status='running' ORDER BY priority DESC,created_at ASC,id ASC LIMIT ?1";
        const char *queued_sql =
            "SELECT id,status,attempts,max_attempts,priority,cancel_requested,created_at,updated_at,session_id,error,"
            "CASE WHEN status='queued' THEN MIN(9,priority+CAST(MAX(0,(julianday('now')-julianday(created_at))*86400/?1) AS INTEGER)) ELSE priority END,"
            "workspace_path,title,task_json,next_attempt_at FROM tasks WHERE status='queued' "
            "ORDER BY CASE WHEN status='queued' THEN MIN(9,priority+CAST(MAX(0,(julianday('now')-julianday(created_at))*86400/?1) AS INTEGER)) ELSE priority END DESC,"
            "created_at ASC,id ASC LIMIT ?2";
        const char *other_sql =
            "SELECT id,status,attempts,max_attempts,priority,cancel_requested,created_at,updated_at,session_id,error,"
            "priority AS effective_priority,workspace_path,title,task_json,next_attempt_at FROM tasks INDEXED BY idx_ab_other_order "
            "WHERE status NOT IN ('running','queued') ORDER BY priority DESC,created_at ASC,id ASC LIMIT ?1";
        int remaining = limit;
        if (db_task_list_run_candidate_band(rt, arr, running_sql, 0, remaining, err, err_size) != 0) goto fail;
        remaining = limit - (int)json_object_array_length(arr);
        if (remaining > 0 && db_task_list_run_candidate_band(rt, arr, queued_sql, 1, remaining, err, err_size) != 0) goto fail;
        remaining = limit - (int)json_object_array_length(arr);
        if (remaining > 0 && db_task_list_run_candidate_band(rt, arr, other_sql, 0, remaining, err, err_size) != 0) goto fail;
#else
        set_error(err, err_size, "candidate path is available only in benchmark build");
        goto fail;
#endif
    } else {
        sqlite3_stmt *stmt = NULL;
        const char *sql_all = "SELECT id,status,attempts,max_attempts,priority,cancel_requested,created_at,updated_at,session_id,error,CASE WHEN status='queued' THEN MIN(9,priority+CAST(MAX(0,(julianday('now')-julianday(created_at))*86400/?1) AS INTEGER)) ELSE priority END,workspace_path,title,task_json,next_attempt_at FROM tasks ORDER BY CASE status WHEN 'running' THEN 0 WHEN 'queued' THEN 1 ELSE 2 END, CASE WHEN status='queued' THEN MIN(9,priority+CAST(MAX(0,(julianday('now')-julianday(created_at))*86400/?1) AS INTEGER)) ELSE priority END DESC, created_at ASC, id ASC LIMIT ?2";
        const char *sql_filter = "SELECT id,status,attempts,max_attempts,priority,cancel_requested,created_at,updated_at,session_id,error,CASE WHEN status='queued' THEN MIN(9,priority+CAST(MAX(0,(julianday('now')-julianday(created_at))*86400/?2) AS INTEGER)) ELSE priority END,workspace_path,title,task_json,next_attempt_at FROM tasks WHERE status=?1 ORDER BY CASE WHEN status='queued' THEN MIN(9,priority+CAST(MAX(0,(julianday('now')-julianday(created_at))*86400/?2) AS INTEGER)) ELSE priority END DESC, created_at ASC, id ASC LIMIT ?3";
        const char *sql_candidate_filter_queued = "SELECT id,status,attempts,max_attempts,priority,cancel_requested,created_at,updated_at,session_id,error,MIN(9,priority+CAST(MAX(0,(julianday('now')-julianday(created_at))*86400/?1) AS INTEGER)),workspace_path,title,task_json,next_attempt_at FROM tasks WHERE status='queued' ORDER BY MIN(9,priority+CAST(MAX(0,(julianday('now')-julianday(created_at))*86400/?1) AS INTEGER)) DESC,created_at ASC,id ASC LIMIT ?2";
        const char *sql_candidate_filter_other = "SELECT id,status,attempts,max_attempts,priority,cancel_requested,created_at,updated_at,session_id,error,priority AS effective_priority,workspace_path,title,task_json,next_attempt_at FROM tasks WHERE status=?1 ORDER BY priority DESC,created_at ASC,id ASC LIMIT ?2";
        const char *sql = candidate && strcmp(status_filter, "queued") == 0 ? sql_candidate_filter_queued :
                          candidate ? sql_candidate_filter_other :
                          ((status_filter && *status_filter) ? sql_filter : sql_all);
        uint64_t prepare_started = DB_PERF_NOW();
        int rc = sqlite3_prepare_v2(rt->db, sql, -1, &stmt, NULL);
        DB_PERF_ADD(prepare_ns, prepare_started);
        if (rc != SQLITE_OK) goto fail;
        if (candidate) {
            if (strcmp(status_filter, "queued") == 0) {
                sqlite3_bind_int(stmt, 1, rt->config.priority_aging_seconds);
                sqlite3_bind_int(stmt, 2, limit);
            } else {
                sqlite3_bind_text(stmt, 1, status_filter, -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(stmt, 2, limit);
            }
        } else if (status_filter && *status_filter) {
            sqlite3_bind_text(stmt, 1, status_filter, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 2, rt->config.priority_aging_seconds);
            sqlite3_bind_int(stmt, 3, limit);
        } else {
            sqlite3_bind_int(stmt, 1, rt->config.priority_aging_seconds);
            sqlite3_bind_int(stmt, 2, limit);
        }
        int append_result = db_task_list_append_rows(stmt, arr, err, err_size, rt->db);
        sqlite3_finalize(stmt);
        if (append_result != 0) goto fail;
    }
    pthread_mutex_unlock(&rt->db_mutex);
    return arr;
fail:
    if (arr) json_object_put(arr);
    if (err && err_size && !*err) (void)snprintf(err, err_size, "SQLite list tasks: %s", rt->db ? sqlite3_errmsg(rt->db) : "database unavailable");
    pthread_mutex_unlock(&rt->db_mutex);
    return NULL;
}

json_object *db_list_tasks(Runtime *rt, const char *status_filter, int limit, char *err, size_t err_size) {
    return db_list_tasks_impl(rt, status_filter, limit, err, err_size, false);
}

#ifdef ASGEND_DB_PERF_BENCHMARK
json_object *db_list_tasks_candidate(Runtime *rt, const char *status_filter, int limit,
                                    char *err, size_t err_size) {
    return db_list_tasks_impl(rt, status_filter, limit, err, err_size, true);
}
#endif

int db_resolve_task_id(Runtime *rt, const char *prefix, char *resolved, size_t resolved_size,
                       char *err, size_t err_size) {
    if (!rt || !prefix || !*prefix || !resolved || resolved_size == 0) {
        set_error(err, err_size, "task ID is required"); return -1;
    }
    pthread_mutex_lock(&rt->db_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(rt->db, "SELECT id FROM tasks WHERE id=?1", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto error;
    sqlite3_bind_text(stmt, 1, prefix, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const char *id = (const char *)sqlite3_column_text(stmt, 0);
        int result = id && string_copy(resolved, resolved_size, id) == 0 ? 0 : -1;
        sqlite3_finalize(stmt); pthread_mutex_unlock(&rt->db_mutex);
        if (result != 0) set_error(err, err_size, "task ID buffer is too small");
        return result;
    }
    sqlite3_finalize(stmt); stmt = NULL;
    if (rc != SQLITE_DONE) goto error;
    rc = sqlite3_prepare_v2(rt->db, "SELECT id FROM tasks WHERE substr(id,1,length(?1))=?1 LIMIT 2", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto error;
    sqlite3_bind_text(stmt, 1, prefix, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) { sqlite3_finalize(stmt); pthread_mutex_unlock(&rt->db_mutex); set_error(err, err_size, "no task matches that ID prefix"); return -1; }
    if (rc != SQLITE_ROW) goto error;
    const char *first = (const char *)sqlite3_column_text(stmt, 0);
    char candidate[AO_MAX_ID];
    if (!first || string_copy(candidate, sizeof(candidate), first) != 0) { sqlite3_finalize(stmt); pthread_mutex_unlock(&rt->db_mutex); set_error(err, err_size, "stored task ID is invalid"); return -1; }
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt); stmt = NULL;
    if (rc == SQLITE_ROW) { pthread_mutex_unlock(&rt->db_mutex); set_error(err, err_size, "ambiguous task ID prefix; enter more characters"); return -2; }
    if (rc != SQLITE_DONE) goto error;
    if (string_copy(resolved, resolved_size, candidate) != 0) { pthread_mutex_unlock(&rt->db_mutex); set_error(err, err_size, "task ID buffer is too small"); return -1; }
    pthread_mutex_unlock(&rt->db_mutex);
    return 0;
error:
    if (stmt) sqlite3_finalize(stmt);
    if (err && err_size) (void)snprintf(err, err_size, "SQLite task ID lookup: %s", sqlite3_errmsg(rt->db));
    pthread_mutex_unlock(&rt->db_mutex);
    return -1;
}

int db_task_workspace_path(Runtime *rt, const char *task_id, char *path, size_t path_size) {
    if (!rt || !task_id || !path || path_size == 0) return -1;
    pthread_mutex_lock(&rt->db_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(rt->db, "SELECT workspace_path FROM tasks WHERE id=?1", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
    }
    int result = -1;
    if (rc == SQLITE_ROW) {
        const char *value = (const char *)sqlite3_column_text(stmt, 0);
        result = value && *value ? string_copy(path, path_size, value) : -1;
    }
    if (stmt) sqlite3_finalize(stmt);
    pthread_mutex_unlock(&rt->db_mutex);
    return result;
}

json_object *db_queue_health(Runtime *rt, char *err, size_t err_size) {
    pthread_mutex_lock(&rt->db_mutex);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT status,COUNT(*) FROM tasks GROUP BY status";
    if (sqlite3_prepare_v2(rt->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        if (err && err_size) (void)snprintf(err, err_size, "SQLite health: %s", sqlite3_errmsg(rt->db));
        pthread_mutex_unlock(&rt->db_mutex); return NULL;
    }
    json_object *out = json_object_new_object();
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *status = (const char *)sqlite3_column_text(stmt, 0);
        if (status) json_object_object_add(out, status, json_object_new_int64(sqlite3_column_int64(stmt, 1)));
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&rt->db_mutex);
    return out;
}

int db_append_session_message(Runtime *rt, const char *session_id, const char *task_id,
                              const char *role, const char *content) {
    if (!rt || !session_id || !*session_id || !role || !content) return 0;
    /* Persist bounded compaction: individual messages are capped at 16 KiB, and only
       the latest 20 messages are retained per session. */
    char bounded[16385];
    size_t n = strlen(content);
    if (n > sizeof(bounded) - 1) n = sizeof(bounded) - 1;
    memcpy(bounded, content, n); bounded[n] = '\0';
    pthread_mutex_lock(&rt->db_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(rt->db, "INSERT INTO sessions(session_id) VALUES(?1) ON CONFLICT(session_id) DO UPDATE SET updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now')", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt); stmt = NULL;
    }
    if (rc == SQLITE_DONE) rc = sqlite3_prepare_v2(rt->db, "INSERT INTO session_messages(session_id,task_id,role,content) VALUES(?1,?2,?3,?4)", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
        if (task_id) sqlite3_bind_text(stmt, 2, task_id, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(stmt, 2);
        sqlite3_bind_text(stmt, 3, role, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, bounded, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt); stmt = NULL;
    }
    if (rc == SQLITE_DONE) rc = sqlite3_prepare_v2(rt->db,
        "DELETE FROM session_messages WHERE session_id=?1 AND message_id NOT IN (SELECT message_id FROM session_messages WHERE session_id=?1 ORDER BY message_id DESC LIMIT 20)", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt); stmt = NULL;
    }
    pthread_mutex_unlock(&rt->db_mutex);
    return rc == SQLITE_DONE ? 0 : -1;
}


json_object *db_session_context(Runtime *rt, const char *session_id, const char *exclude_task_id) {
    json_object *messages = json_object_new_array();
    if (!messages || !rt || !session_id || !*session_id) return messages;
    pthread_mutex_lock(&rt->db_mutex);
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT role,content FROM ("
        " SELECT message_id,role,content FROM session_messages"
        " WHERE session_id=?1 AND (task_id IS NULL OR task_id<>?2)"
        " ORDER BY message_id DESC LIMIT 6) ORDER BY message_id ASC";
    int rc = sqlite3_prepare_v2(rt->db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, exclude_task_id ? exclude_task_id : "", -1, SQLITE_TRANSIENT);
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            const char *role = (const char *)sqlite3_column_text(stmt, 0);
            const char *content = (const char *)sqlite3_column_text(stmt, 1);
            if (!role || !content || (strcmp(role, "user") != 0 && strcmp(role, "assistant") != 0)) continue;
            size_t len = strlen(content);
            if (len > 4096) len = 4096;
            json_object *message = json_object_new_object();
            if (!message) break;
            json_object_object_add(message, "role", json_object_new_string(role));
            json_object_object_add(message, "content", json_object_new_string_len(content, (int)len));
            json_object_array_add(messages, message);
        }
        sqlite3_finalize(stmt);
    }
    pthread_mutex_unlock(&rt->db_mutex);
    return messages;
}

int db_append_task_event(Runtime *rt, const char *task_id, const char *event_type,
                         const char *actor, const char *message, json_object *payload) {
    if (!rt || !rt->db || !task_id || !*task_id || !event_type || !*event_type) return -1;
    const char *payload_text = payload ? json_object_to_json_string_ext(payload, JSON_C_TO_STRING_PLAIN) : NULL;
    pthread_mutex_lock(&rt->db_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(rt->db,
        "INSERT INTO task_events(task_id,event_type,actor,message,payload_json) VALUES(?1,?2,?3,?4,?5)",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, event_type, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, actor && *actor ? actor : "system", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, message ? message : "", -1, SQLITE_TRANSIENT);
        if (payload_text) sqlite3_bind_text(stmt, 5, payload_text, -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(stmt, 5);
        rc = sqlite3_step(stmt);
    }
    if (stmt) sqlite3_finalize(stmt);
    pthread_mutex_unlock(&rt->db_mutex);
    return rc == SQLITE_DONE ? 0 : -1;
}

json_object *db_task_events(Runtime *rt, const char *task_id, int after_id, int limit,
                            char *err, size_t err_size) {
    if (!rt || !task_id || !*task_id) return NULL;
    if (after_id < 0) after_id = 0;
    if (limit < 1) limit = 100;
    if (limit > 500) limit = 500;
    json_object *events = json_object_new_array();
    if (!events) return NULL;
    pthread_mutex_lock(&rt->db_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(rt->db,
        "SELECT event_id,event_type,actor,message,payload_json,created_at FROM task_events WHERE task_id=?1 AND event_id>?2 ORDER BY event_id ASC LIMIT ?3",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto events_fail;
    sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, after_id);
    sqlite3_bind_int(stmt, 3, limit);
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        json_object *row = json_object_new_object();
        if (!row) { rc = SQLITE_NOMEM; break; }
        sqlite3_int64 event_id = sqlite3_column_int64(stmt, 0);
        const char *type = (const char *)sqlite3_column_text(stmt, 1);
        const char *actor = (const char *)sqlite3_column_text(stmt, 2);
        const char *message = (const char *)sqlite3_column_text(stmt, 3);
        const char *payload = (const char *)sqlite3_column_text(stmt, 4);
        const char *created = (const char *)sqlite3_column_text(stmt, 5);
        json_object_object_add(row, "event_id", json_object_new_int64(event_id));
        json_object_object_add(row, "type", json_object_new_string(type ? type : "event"));
        json_object_object_add(row, "actor", json_object_new_string(actor ? actor : "system"));
        json_object_object_add(row, "message", json_object_new_string(message ? message : ""));
        if (created) json_object_object_add(row, "created_at", json_object_new_string(created));
        if (payload) {
            json_object *parsed = json_tokener_parse(payload);
            if (parsed) json_object_object_add(row, "payload", parsed);
            else json_object_object_add(row, "payload_raw", json_object_new_string(payload));
        }
        json_object_array_add(events, row);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&rt->db_mutex);
    if (rc != SQLITE_DONE) { json_object_put(events); return NULL; }
    return events;
events_fail:
    if (stmt) sqlite3_finalize(stmt);
    if (err && err_size) (void)snprintf(err, err_size, "SQLite task events: %s", sqlite3_errmsg(rt->db));
    pthread_mutex_unlock(&rt->db_mutex);
    json_object_put(events);
    return NULL;
}

int db_answer_task(Runtime *rt, const char *task_id, const char *answer, const char *option_id,
                   char *err, size_t err_size) {
    if (!rt || !task_id || !*task_id || !answer || !*answer || strlen(answer) > 10000) {
        set_error(err, err_size, "a non-empty answer up to 10000 bytes is required");
        return -1;
    }
    char session_id[129] = {0};
    pthread_mutex_lock(&rt->db_mutex);
    if (sqlite3_exec(rt->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) goto answer_fail;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(rt->db, "SELECT task_json,session_id,status,result_json FROM tasks WHERE id=?1", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto answer_fail_stmt;
    sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt); stmt = NULL;
        (void)sqlite3_exec(rt->db, "ROLLBACK", NULL, NULL, NULL);
        pthread_mutex_unlock(&rt->db_mutex);
        set_error(err, err_size, "task not found");
        return -1;
    }
    const char *raw = (const char *)sqlite3_column_text(stmt, 0);
    const char *sess = (const char *)sqlite3_column_text(stmt, 1);
    const char *state = (const char *)sqlite3_column_text(stmt, 2);
    const char *stored_result = (const char *)sqlite3_column_text(stmt, 3);
    char *raw_copy = raw ? strdup(raw) : NULL;
    char *result_copy = stored_result ? strdup(stored_result) : NULL;
    bool waiting = state && strcmp(state, "waiting_input") == 0;
    if (sess) (void)snprintf(session_id, sizeof(session_id), "%s", sess);
    sqlite3_finalize(stmt); stmt = NULL;
    if (!waiting || !raw_copy) {
        free(raw_copy); free(result_copy);
        (void)sqlite3_exec(rt->db, "ROLLBACK", NULL, NULL, NULL);
        pthread_mutex_unlock(&rt->db_mutex);
        set_error(err, err_size, waiting ? "could not read queued task" : "task is not waiting for input");
        return -1;
    }
    json_object *request = json_tokener_parse(raw_copy);
    free(raw_copy);
    if (!request || json_object_get_type(request) != json_type_object) {
        if (request) json_object_put(request);
        free(result_copy);
        (void)sqlite3_exec(rt->db, "ROLLBACK", NULL, NULL, NULL);
        pthread_mutex_unlock(&rt->db_mutex);
        set_error(err, err_size, "stored task request is invalid JSON");
        return -1;
    }
    /* Agent-originated questions carry a checkpoint so prior subtasks are not replayed. */
    if (result_copy) {
        json_object *stored = json_tokener_parse(result_copy);
        free(result_copy); result_copy = NULL;
        json_object *checkpoint = NULL;
        if (stored && json_object_get_type(stored) == json_type_object &&
            json_object_object_get_ex(stored, "checkpoint", &checkpoint) && checkpoint &&
            json_object_get_type(checkpoint) == json_type_object) {
            json_object_object_add(request, "resume_checkpoint", json_object_get(checkpoint));
        }
        if (stored) json_object_put(stored);
    }
    json_object *answers = NULL;
    if (!json_object_object_get_ex(request, "clarification_answers", &answers) || !answers || json_object_get_type(answers) != json_type_array) {
        answers = json_object_new_array();
        if (!answers) { json_object_put(request); (void)sqlite3_exec(rt->db, "ROLLBACK", NULL, NULL, NULL); pthread_mutex_unlock(&rt->db_mutex); set_error(err, err_size, "out of memory saving answer"); return -1; }
        json_object_object_add(request, "clarification_answers", answers);
    }
    if (json_object_array_length(answers) >= 3) {
        json_object_put(request);
        (void)sqlite3_exec(rt->db, "ROLLBACK", NULL, NULL, NULL);
        pthread_mutex_unlock(&rt->db_mutex);
        set_error(err, err_size, "the task has reached the maximum of three clarification turns");
        return -1;
    }
    json_object *entry = json_object_new_object();
    if (!entry) { json_object_put(request); (void)sqlite3_exec(rt->db, "ROLLBACK", NULL, NULL, NULL); pthread_mutex_unlock(&rt->db_mutex); set_error(err, err_size, "out of memory saving answer"); return -1; }
    json_object_object_add(entry, "answer", json_object_new_string(answer));
    if (option_id && *option_id) json_object_object_add(entry, "option_id", json_object_new_string(option_id));
    json_object_array_add(answers, entry);
    const char *updated_json = json_object_to_json_string_ext(request, JSON_C_TO_STRING_PLAIN);
    rc = sqlite3_prepare_v2(rt->db,
        "UPDATE tasks SET task_json=?2,status='queued',attempts=MAX(0,attempts-1),result_json=NULL,error=NULL,updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE id=?1 AND status='waiting_input'",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, updated_json, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
    }
    if (stmt) { sqlite3_finalize(stmt); stmt = NULL; }
    json_object_put(request);
    if (rc != SQLITE_DONE || sqlite3_changes(rt->db) != 1 || sqlite3_exec(rt->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        (void)sqlite3_exec(rt->db, "ROLLBACK", NULL, NULL, NULL);
        pthread_mutex_unlock(&rt->db_mutex);
        set_error(err, err_size, "could not persist answer and resume task");
        return -1;
    }
    pthread_mutex_unlock(&rt->db_mutex);
    char event_message[10500];
    (void)snprintf(event_message, sizeof(event_message), "User answered: %.10000s", answer);
    json_object *payload = json_object_new_object();
    if (payload) {
        json_object_object_add(payload, "answer", json_object_new_string(answer));
        if (option_id && *option_id) json_object_object_add(payload, "option_id", json_object_new_string(option_id));
    }
    (void)db_append_task_event(rt, task_id, "user_answer", "user", event_message, payload);
    if (payload) json_object_put(payload);
    if (session_id[0]) (void)db_append_session_message(rt, session_id, task_id, "user", event_message);
    return 0;
answer_fail_stmt:
    if (stmt) sqlite3_finalize(stmt);
answer_fail:
    (void)sqlite3_exec(rt->db, "ROLLBACK", NULL, NULL, NULL);
    if (err && err_size) (void)snprintf(err, err_size, "SQLite answer: %s", sqlite3_errmsg(rt->db));
    pthread_mutex_unlock(&rt->db_mutex);
    return -1;
}

/* Shared To-do/Goal plan storage, used by the Web UI and C TUI over the same API. */
json_object *db_plan_list(Runtime *rt, const char *scope_id, const char *kind,
                          char *err, size_t err_size) {
    if (!rt || !rt->db) return NULL;
    const char *scope = scope_id && *scope_id ? scope_id : "global";
    if (strcmp(scope, "global") != 0 && strlen(scope) >= AO_MAX_ID) {
        set_error(err, err_size, "plan scope ID is too long"); return NULL;
    }
    if (kind && *kind && strcmp(kind, "todo") != 0 && strcmp(kind, "goal") != 0) {
        set_error(err, err_size, "plan kind must be todo or goal"); return NULL;
    }
    pthread_mutex_lock(&rt->db_mutex);
    sqlite3_stmt *stmt = NULL;
    const char *sql = (kind && *kind)
        ? "SELECT item_id,kind,title,description,status,created_at,updated_at FROM plan_items WHERE scope_id=?1 AND kind=?2 ORDER BY status,sort_order,item_id"
        : "SELECT item_id,kind,title,description,status,created_at,updated_at FROM plan_items WHERE scope_id=?1 ORDER BY kind,status,sort_order,item_id";
    int rc = sqlite3_prepare_v2(rt->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto plan_list_fail;
    sqlite3_bind_text(stmt, 1, scope, -1, SQLITE_TRANSIENT);
    if (kind && *kind) sqlite3_bind_text(stmt, 2, kind, -1, SQLITE_TRANSIENT);
    json_object *items = json_object_new_array();
    if (!items) goto plan_list_fail;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        json_object *item = json_object_new_object();
        if (!item) { json_object_put(items); items = NULL; goto plan_list_fail; }
        json_object_object_add(item, "item_id", json_object_new_int64(sqlite3_column_int64(stmt, 0)));
        const char *item_kind = (const char *)sqlite3_column_text(stmt, 1);
        const char *title = (const char *)sqlite3_column_text(stmt, 2);
        const char *desc = (const char *)sqlite3_column_text(stmt, 3);
        const char *status = (const char *)sqlite3_column_text(stmt, 4);
        const char *created = (const char *)sqlite3_column_text(stmt, 5);
        const char *updated = (const char *)sqlite3_column_text(stmt, 6);
        json_object_object_add(item, "kind", json_object_new_string(item_kind ? item_kind : "todo"));
        json_object_object_add(item, "title", json_object_new_string(title ? title : ""));
        json_object_object_add(item, "description", json_object_new_string(desc ? desc : ""));
        json_object_object_add(item, "status", json_object_new_string(status ? status : "open"));
        if (created) json_object_object_add(item, "created_at", json_object_new_string(created));
        if (updated) json_object_object_add(item, "updated_at", json_object_new_string(updated));
        json_object_array_add(items, item);
    }
    if (rc != SQLITE_DONE) { json_object_put(items); goto plan_list_fail; }
    sqlite3_finalize(stmt); pthread_mutex_unlock(&rt->db_mutex); return items;
plan_list_fail:
    if (stmt) sqlite3_finalize(stmt);
    if (err && err_size) (void)snprintf(err, err_size, "SQLite plan list: %s", sqlite3_errmsg(rt->db));
    pthread_mutex_unlock(&rt->db_mutex); return NULL;
}

int db_plan_add(Runtime *rt, const char *scope_id, const char *kind, const char *title,
                const char *description, int64_t *item_id, char *err, size_t err_size) {
    if (!rt || !rt->db || !kind || (strcmp(kind,"todo")!=0 && strcmp(kind,"goal")!=0) ||
        !title || !*title || strlen(title)>160 || (description && strlen(description)>4096)) {
        set_error(err, err_size, "invalid plan item (kind todo/goal, title <=160 characters, description <=4096)"); return -1;
    }
    const char *scope = scope_id && *scope_id ? scope_id : "global";
    pthread_mutex_lock(&rt->db_mutex); sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(rt->db,
        "INSERT INTO plan_items(scope_id,kind,title,description,status,sort_order) VALUES(?1,?2,?3,?4,'open',COALESCE((SELECT MAX(sort_order)+1 FROM plan_items WHERE scope_id=?1 AND kind=?2),0))",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto plan_add_fail;
    sqlite3_bind_text(stmt,1,scope,-1,SQLITE_TRANSIENT); sqlite3_bind_text(stmt,2,kind,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,3,title,-1,SQLITE_TRANSIENT); sqlite3_bind_text(stmt,4,description ? description : "",-1,SQLITE_TRANSIENT);
    rc=sqlite3_step(stmt); if (rc != SQLITE_DONE) goto plan_add_fail;
    if (item_id) *item_id = sqlite3_last_insert_rowid(rt->db);
    sqlite3_finalize(stmt); pthread_mutex_unlock(&rt->db_mutex); return 0;
plan_add_fail:
    if (stmt) sqlite3_finalize(stmt);
    if (err && err_size) (void)snprintf(err,err_size,"SQLite plan add: %s",sqlite3_errmsg(rt->db));
    pthread_mutex_unlock(&rt->db_mutex); return -1;
}

int db_plan_update(Runtime *rt, const char *scope_id, int64_t item_id, const char *title,
                   const char *description, const char *status, char *err, size_t err_size) {
    if (!rt || !rt->db || item_id <= 0 ||
        (title && (!*title || strlen(title)>160)) || (description && strlen(description)>4096) ||
        (status && strcmp(status,"open")!=0 && strcmp(status,"done")!=0)) {
        set_error(err,err_size,"invalid plan item update"); return -1;
    }
    const char *scope=scope_id&&*scope_id?scope_id:"global";
    pthread_mutex_lock(&rt->db_mutex); sqlite3_stmt *stmt=NULL;
    int rc=sqlite3_prepare_v2(rt->db,
       "UPDATE plan_items SET title=COALESCE(?1,title),description=COALESCE(?2,description),status=COALESCE(?3,status),updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE item_id=?4 AND scope_id=?5",
       -1,&stmt,NULL);
    if(rc!=SQLITE_OK)goto plan_update_fail;
    if(title)sqlite3_bind_text(stmt,1,title,-1,SQLITE_TRANSIENT);else sqlite3_bind_null(stmt,1);
    if(description)sqlite3_bind_text(stmt,2,description,-1,SQLITE_TRANSIENT);else sqlite3_bind_null(stmt,2);
    if(status)sqlite3_bind_text(stmt,3,status,-1,SQLITE_TRANSIENT);else sqlite3_bind_null(stmt,3);
    sqlite3_bind_int64(stmt,4,item_id);sqlite3_bind_text(stmt,5,scope,-1,SQLITE_TRANSIENT);
    rc=sqlite3_step(stmt);if(rc!=SQLITE_DONE)goto plan_update_fail;
    int changed=sqlite3_changes(rt->db);sqlite3_finalize(stmt);pthread_mutex_unlock(&rt->db_mutex);
    if(changed!=1){set_error(err,err_size,"plan item not found in this scope");return -1;}return 0;
plan_update_fail:
    if (stmt) sqlite3_finalize(stmt);
    if (err && err_size) (void)snprintf(err, err_size, "SQLite plan update: %s", sqlite3_errmsg(rt->db));
    pthread_mutex_unlock(&rt->db_mutex);
    return -1;
}

int db_plan_delete(Runtime *rt, const char *scope_id, int64_t item_id, char *err, size_t err_size) {
    if (!rt || !rt->db || item_id <= 0) { set_error(err,err_size,"invalid plan item ID"); return -1; }
    const char *scope=scope_id&&*scope_id?scope_id:"global";
    pthread_mutex_lock(&rt->db_mutex);sqlite3_stmt *stmt=NULL;
    int rc=sqlite3_prepare_v2(rt->db,"DELETE FROM plan_items WHERE item_id=?1 AND scope_id=?2",-1,&stmt,NULL);
    if(rc!=SQLITE_OK)goto plan_delete_fail;
    sqlite3_bind_int64(stmt,1,item_id);sqlite3_bind_text(stmt,2,scope,-1,SQLITE_TRANSIENT);rc=sqlite3_step(stmt);
    if(rc!=SQLITE_DONE)goto plan_delete_fail;
    int changed=sqlite3_changes(rt->db);sqlite3_finalize(stmt);pthread_mutex_unlock(&rt->db_mutex);
    if(changed!=1){set_error(err,err_size,"plan item not found in this scope");return -1;}return 0;
plan_delete_fail:
    if (stmt) sqlite3_finalize(stmt);
    if (err && err_size) (void)snprintf(err, err_size, "SQLite plan delete: %s", sqlite3_errmsg(rt->db));
    pthread_mutex_unlock(&rt->db_mutex);
    return -1;
}
