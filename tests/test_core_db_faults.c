#include "runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int deny_commit_authorizer(void *opaque, int action, const char *arg1,
                                 const char *arg2, const char *database_name,
                                 const char *trigger_name) {
    (void)opaque;
    (void)arg2;
    (void)database_name;
    (void)trigger_name;
    if (action == SQLITE_TRANSACTION && arg1 && strcmp(arg1, "COMMIT") == 0)
        return SQLITE_DENY;
    return SQLITE_OK;
}

static int fail(const char *message) {
    fprintf(stderr, "core DB fault-injection test failed: %s\n", message);
    return 1;
}

static int set_runtime_path(char *destination, size_t destination_size,
                            const char *root, const char *leaf) {
    int written = snprintf(destination, destination_size, "%s/%s", root, leaf);
    return written >= 0 && (size_t)written < destination_size ? 0 : -1;
}

static int configure_runtime_paths(Runtime *rt, const char *root, const char *db_path) {
    return string_copy(rt->config.root_dir, sizeof(rt->config.root_dir), root) == 0 &&
           string_copy(rt->config.database_path, sizeof(rt->config.database_path), db_path) == 0 &&
           set_runtime_path(rt->config.asgend_home, sizeof(rt->config.asgend_home), root, "asgend-home") == 0
        ? 0 : -1;
}

static void enqueue_stress_child(const char *root, const char *db_path, int worker_index) {
    Runtime *child = calloc(1, sizeof(*child));
    if (!child) _exit(31);
    if (pthread_mutex_init(&child->db_mutex, NULL) != 0 ||
        configure_runtime_paths(child, root, db_path) != 0) _exit(32);
    child->config.queue_max_pending = 500;
    child->config.max_retries = 2;
    child->config.priority_aging_seconds = 2;
    if (sqlite3_open_v2(db_path, &child->db,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) _exit(33);
    sqlite3_busy_timeout(child->db, 5000);
    if (sqlite3_exec(child->db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL) != SQLITE_OK) _exit(34);

    for (int item = 0; item < 20; ++item) {
        char task[160], key[128], id[AO_MAX_ID] = {0}, err[512] = {0};
        (void)snprintf(task, sizeof(task), "{\"task\":\"process-%d-item-%d\"}", worker_index, item);
        (void)snprintf(key, sizeof(key), "mp-worker-%d-item-%d", worker_index, item);
        bool duplicate = false;
        if (db_enqueue(child, task, NULL, key, item % 10, id, sizeof(id),
                       &duplicate, err, sizeof(err)) != 0 || duplicate || !id[0]) {
            (void)dprintf(STDERR_FILENO, "worker %d item %d failed: %s\n", worker_index, item, err);
            _exit(35);
        }
    }
    char shared_id[AO_MAX_ID] = {0}, shared_err[512] = {0};
    bool shared_duplicate = false;
    if (db_enqueue(child, "{\"task\":\"shared-race\"}", NULL,
                   "shared-cross-process-idempotency", 0, shared_id, sizeof(shared_id),
                   &shared_duplicate, shared_err, sizeof(shared_err)) != 0 || !shared_id[0]) {
        (void)dprintf(STDERR_FILENO, "worker %d shared key failed: %s\n", worker_index, shared_err);
        _exit(36);
    }
    db_close(child);
    pthread_mutex_destroy(&child->db_mutex);
    free(child);
    _exit(0);
}

static int run_multiprocess_enqueue_stress(const char *root, const char *db_path) {
    enum { WORKERS = 4 };
    pid_t children[WORKERS] = {-1, -1, -1, -1};
    for (int i = 0; i < WORKERS; ++i) {
        pid_t child = fork();
        if (child < 0) {
            for (int j = 0; j < i; ++j) if (children[j] > 0) (void)kill(children[j], SIGKILL);
            for (int j = 0; j < i; ++j) if (children[j] > 0) (void)waitpid(children[j], NULL, 0);
            return -1;
        }
        if (child == 0) enqueue_stress_child(root, db_path, i);
        children[i] = child;
    }
    for (int i = 0; i < WORKERS; ++i) {
        int status = 0;
        pid_t waited;
        do { waited = waitpid(children[i], &status, 0); } while (waited < 0 && errno == EINTR);
        if (waited != children[i] || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    }
    return 0;
}

int main(void) {
    char root_template[] = "/tmp/asgend-db-faults-XXXXXX";
    char *root = mkdtemp(root_template);
    if (!root) return fail("mkdtemp failed");

    Runtime *rt = calloc(1, sizeof(*rt));
    if (!rt) {
        (void)rmdir(root);
        return fail("runtime allocation failed");
    }
    if (pthread_mutex_init(&rt->db_mutex, NULL) != 0) {
        free(rt);
        (void)rmdir(root);
        return fail("database mutex initialization failed");
    }
    if (configure_runtime_paths(rt, root, "") != 0 ||
        set_runtime_path(rt->config.database_path, sizeof(rt->config.database_path),
                         root, "core.sqlite3") != 0) {
        pthread_mutex_destroy(&rt->db_mutex);
        free(rt);
        (void)rmdir(root);
        return fail("database paths do not fit configuration buffers");
    }
    rt->config.priority_aging_seconds = 2;
    rt->config.queue_max_pending = 500;
    rt->config.max_retries = 2;

    char error[512] = {0};
    if (db_init(rt, error, sizeof(error)) != 0) {
        fprintf(stderr, "db_init failed: %s\n", error);
        pthread_mutex_destroy(&rt->db_mutex);
        free(rt);
        return 1;
    }

    /* Force SQLite itself to deny COMMIT while allowing rollback. This tests
       the real empty-queue commit error branch without a production test hook. */
    if (sqlite3_set_authorizer(rt->db, deny_commit_authorizer, NULL) != SQLITE_OK)
        return fail("could not install COMMIT-denying authorizer");
    ClaimedTask claim = {0};
    int claim_rc = db_claim_next(rt, &claim, error, sizeof(error));
    if (claim_rc != -1 || !strstr(error, "SQLite commit empty claim") ||
        !strstr(error, "not authorized")) {
        sqlite3_set_authorizer(rt->db, NULL, NULL);
        db_close(rt);
        pthread_mutex_destroy(&rt->db_mutex);
        free(rt);
        return fail("empty-queue COMMIT denial was not reported as a claim error");
    }
    if (claim.id || claim.task_json) {
        sqlite3_set_authorizer(rt->db, NULL, NULL);
        db_claim_free(&claim);
        db_close(rt);
        pthread_mutex_destroy(&rt->db_mutex);
        free(rt);
        return fail("empty-queue COMMIT failure returned a non-empty claim");
    }
    if (sqlite3_set_authorizer(rt->db, NULL, NULL) != SQLITE_OK) {
        db_close(rt);
        pthread_mutex_destroy(&rt->db_mutex);
        free(rt);
        return fail("could not remove COMMIT-denying authorizer");
    }
    memset(error, 0, sizeof(error));
    claim_rc = db_claim_next(rt, &claim, error, sizeof(error));
    if (claim_rc != 0 || claim.id || claim.task_json) {
        db_claim_free(&claim);
        db_close(rt);
        pthread_mutex_destroy(&rt->db_mutex);
        free(rt);
        return fail("connection was not reusable after empty-queue COMMIT rollback");
    }
    char *sqlite_error = NULL;
    if (sqlite3_exec(rt->db, "BEGIN IMMEDIATE; ROLLBACK;", NULL, NULL, &sqlite_error) != SQLITE_OK) {
        if (sqlite_error) sqlite3_free(sqlite_error);
        db_close(rt);
        pthread_mutex_destroy(&rt->db_mutex);
        free(rt);
        return fail("transaction state was not cleared after COMMIT failure");
    }

    /* Exercise enqueue serialization across independent processes, not just
       threads/one FULLMUTEX connection. Close the parent's DB handle before
       fork so child processes never use an inherited SQLite connection. */
    char shared_db_path[AO_MAX_PATH];
    if (string_copy(shared_db_path, sizeof(shared_db_path), rt->config.database_path) != 0) {
        db_close(rt);
        pthread_mutex_destroy(&rt->db_mutex);
        free(rt);
        return fail("could not snapshot DB path for multi-process stress");
    }
    db_close(rt);
    if (run_multiprocess_enqueue_stress(root, shared_db_path) != 0) {
        pthread_mutex_destroy(&rt->db_mutex);
        free(rt);
        return fail("multi-process task enqueue/idempotency stress failed");
    }
    if (sqlite3_open_v2(shared_db_path, &rt->db,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK || !rt->db) {
        db_close(rt);
        pthread_mutex_destroy(&rt->db_mutex);
        free(rt);
        return fail("could not reopen database after multi-process stress");
    }
    sqlite3_busy_timeout(rt->db, 5000);
    sqlite3_stmt *count_stmt = NULL;
    if (sqlite3_prepare_v2(rt->db, "SELECT COUNT(*), SUM(idempotency_key='shared-cross-process-idempotency') FROM tasks",
                           -1, &count_stmt, NULL) != SQLITE_OK ||
        sqlite3_step(count_stmt) != SQLITE_ROW) {
        if (count_stmt) sqlite3_finalize(count_stmt);
        db_close(rt);
        pthread_mutex_destroy(&rt->db_mutex);
        free(rt);
        return fail("could not inspect task counts after multi-process stress");
    }
    int total_tasks = sqlite3_column_int(count_stmt, 0);
    int shared_key_rows = sqlite3_column_int(count_stmt, 1);
    sqlite3_finalize(count_stmt);
    if (total_tasks != 81 || shared_key_rows != 1) {
        db_close(rt);
        pthread_mutex_destroy(&rt->db_mutex);
        free(rt);
        return fail("concurrent processes did not preserve task/idempotency uniqueness");
    }

    /* An outstanding statement deterministically makes sqlite3_close() return
       SQLITE_BUSY. db_close must log it, use close_v2, and clear rt->db. */
    sqlite3_stmt *outstanding = NULL;
    if (sqlite3_prepare_v2(rt->db, "SELECT 1", -1, &outstanding, NULL) != SQLITE_OK || !outstanding) {
        db_close(rt);
        pthread_mutex_destroy(&rt->db_mutex);
        free(rt);
        return fail("could not prepare outstanding statement for SQLITE_BUSY fixture");
    }
    FILE *log_file = tmpfile();
    if (!log_file) {
        sqlite3_finalize(outstanding);
        db_close(rt);
        pthread_mutex_destroy(&rt->db_mutex);
        free(rt);
        return fail("could not create temporary diagnostics capture");
    }
    (void)fflush(stderr);
    int saved_stderr = dup(STDERR_FILENO);
    if (saved_stderr < 0 || dup2(fileno(log_file), STDERR_FILENO) < 0) {
        if (saved_stderr >= 0) close(saved_stderr);
        fclose(log_file);
        sqlite3_finalize(outstanding);
        db_close(rt);
        pthread_mutex_destroy(&rt->db_mutex);
        free(rt);
        return fail("could not redirect diagnostics for close test");
    }
    db_close(rt);
    (void)fflush(stderr);
    (void)dup2(saved_stderr, STDERR_FILENO);
    close(saved_stderr);

    char diagnostics[1024] = {0};
    rewind(log_file);
    (void)fread(diagnostics, 1, sizeof(diagnostics) - 1, log_file);
    fclose(log_file);
    if (rt->db != NULL ||
        !strstr(diagnostics, "SQLite close reported outstanding resources")) {
        (void)sqlite3_finalize(outstanding);
        pthread_mutex_destroy(&rt->db_mutex);
        free(rt);
        return fail("SQLITE_BUSY close was not logged and deferred");
    }
    if (sqlite3_finalize(outstanding) != SQLITE_OK) {
        pthread_mutex_destroy(&rt->db_mutex);
        free(rt);
        return fail("outstanding statement did not finalize after close_v2");
    }

    char db_path[AO_MAX_PATH];
    if (string_copy(db_path, sizeof(db_path), rt->config.database_path) != 0) {
        pthread_mutex_destroy(&rt->db_mutex);
        free(rt);
        return fail("database path copy failed during cleanup");
    }
    pthread_mutex_destroy(&rt->db_mutex);
    free(rt);

    (void)unlink(db_path);
    char sidecar[AO_MAX_PATH];
    if (snprintf(sidecar, sizeof(sidecar), "%s-wal", db_path) < (int)sizeof(sidecar)) (void)unlink(sidecar);
    if (snprintf(sidecar, sizeof(sidecar), "%s-shm", db_path) < (int)sizeof(sidecar)) (void)unlink(sidecar);
    (void)rmdir(root);
    puts("core DB fault-injection tests passed");
    return 0;
}
