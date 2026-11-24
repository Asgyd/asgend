#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int copy_path(char *dst, size_t size, const char *src) {
    if (!dst || !size || !src) return -1;
    int count = snprintf(dst, size, "%s", src);
    return count >= 0 && (size_t)count < size ? 0 : -1;
}

static int fail(Runtime *rt, const char *message, int close_db) {
    fprintf(stderr, "task-list C reference failed: %s\n", message);
    if (rt) {
        if (close_db) db_close(rt);
        if (rt->models_mutex_initialized) pthread_mutex_destroy(&rt->models_mutex);
        if (rt->model_scheduler_mutex_initialized) pthread_mutex_destroy(&rt->model_scheduler_mutex);
        pthread_mutex_destroy(&rt->db_mutex);
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s DATABASE ROOT_DIR AGING_SECONDS LIMITS_CSV\n", argv[0]);
        return 2;
    }
    char *end = NULL;
    long aging = strtol(argv[3], &end, 10);
    if (!end || *end || aging < 1 || aging > 86400) return 2;

    Runtime runtime;
    memset(&runtime, 0, sizeof(runtime));
    if (pthread_mutex_init(&runtime.db_mutex, NULL) != 0) return 3;
    if (copy_path(runtime.config.database_path, sizeof(runtime.config.database_path), argv[1]) != 0 ||
        copy_path(runtime.config.root_dir, sizeof(runtime.config.root_dir), argv[2]) != 0) {
        pthread_mutex_destroy(&runtime.db_mutex);
        return 2;
    }
    char asgend_home[AO_MAX_PATH];
    if (snprintf(asgend_home, sizeof(asgend_home), "%s/home", argv[2]) >= (int)sizeof(asgend_home) ||
        copy_path(runtime.config.asgend_home, sizeof(runtime.config.asgend_home), asgend_home) != 0) {
        pthread_mutex_destroy(&runtime.db_mutex);
        return 2;
    }
    runtime.config.priority_aging_seconds = (int)aging;
    char error[512] = {0};
    if (db_init(&runtime, error, sizeof(error)) != 0) return fail(&runtime, error, runtime.db != NULL);

    json_object *report = json_object_new_object();
    if (!report) return fail(&runtime, "out of memory", 1);
    const char *statuses[] = {NULL, "queued", "running", "waiting_input", "completed",
                              "completed_without_model", "failed", "cancelled"};
    char *limits_copy = strdup(argv[4]);
    if (!limits_copy) { json_object_put(report); return fail(&runtime, "out of memory", 1); }
    char *save = NULL;
    for (char *token = strtok_r(limits_copy, ",", &save); token;
         token = strtok_r(NULL, ",", &save)) {
        char *limit_end = NULL;
        long limit_value = strtol(token, &limit_end, 10);
        if (!limit_end || *limit_end || limit_value < 1 || limit_value > 200) {
            free(limits_copy); json_object_put(report); return fail(&runtime, "invalid limit", 1);
        }
        for (size_t s = 0; s < sizeof(statuses) / sizeof(statuses[0]); ++s) {
            char key[80];
            (void)snprintf(key, sizeof(key), "%s_%ld", statuses[s] ? statuses[s] : "all", limit_value);
            json_object *rows = db_list_tasks(&runtime, statuses[s], (int)limit_value, error, sizeof(error));
            if (!rows) {
                free(limits_copy); json_object_put(report);
                return fail(&runtime, error[0] ? error : "db_list_tasks returned null", 1);
            }
            json_object_object_add(report, key, rows);
        }
    }
    free(limits_copy);
    db_close(&runtime);
    pthread_mutex_destroy(&runtime.db_mutex);
    const char *serialized = json_object_to_json_string_ext(report, JSON_C_TO_STRING_PLAIN);
    if (!serialized) { json_object_put(report); return 1; }
    puts(serialized);
    json_object_put(report);
    return 0;
}
