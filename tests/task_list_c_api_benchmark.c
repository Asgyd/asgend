#include "runtime.h"
#include "db_perf_timing.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WARMUP_CALLS 3

typedef struct {
    uint64_t api_ns;
    uint64_t mutex_wait_ns;
    uint64_t prepare_ns;
    uint64_t sqlite_step_ns;
    uint64_t json_mapping_ns;
    uint64_t serialization_ns;
    size_t serialized_bytes;
    size_t rows;
    char *serialized;
} Sample;

typedef struct {
    Runtime *runtime;
    int worker_id;
    int samples_per_worker;
    int limit;
    bool candidate;
    Sample *samples;
    uint64_t initial_api_ns;
    uint64_t initial_serialization_ns;
    uint64_t initial_mutex_wait_ns;
    uint64_t initial_prepare_ns;
    uint64_t initial_sqlite_step_ns;
    uint64_t initial_json_mapping_ns;
    size_t initial_rows;
    char *initial_serialized;
    char error[512];
    pthread_mutex_t *gate_mutex;
    pthread_cond_t *gate_cond;
    int *ready_count;
    bool *start_flag;
} Worker;

static uint64_t monotonic_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static int copy_arg(char *dst, size_t size, const char *src) {
    if (!dst || !size || !src) return -1;
    int n = snprintf(dst, size, "%s", src);
    return n >= 0 && (size_t)n < size ? 0 : -1;
}

static json_object *list_tasks(Runtime *rt, bool candidate, int limit, char *err, size_t err_size) {
    return candidate ? db_list_tasks_candidate(rt, NULL, limit, err, err_size)
                     : db_list_tasks(rt, NULL, limit, err, err_size);
}

static int measured_call(Worker *worker, Sample *sample, bool capture_json) {
    char error[512] = {0};
    uint64_t start = monotonic_ns();
    json_object *rows = list_tasks(worker->runtime, worker->candidate, worker->limit,
                                   error, sizeof(error));
    uint64_t end = monotonic_ns();
    if (!rows) {
        (void)snprintf(worker->error, sizeof(worker->error), "%s",
                       error[0] ? error : "db_list_tasks returned null");
        return -1;
    }
    DbPerfTiming timing = {0};
    db_perf_timing_last(&timing);
    uint64_t serialization_start = monotonic_ns();
    const char *json = json_object_to_json_string_ext(rows, JSON_C_TO_STRING_PLAIN);
    uint64_t serialization_end = monotonic_ns();
    if (!json) {
        json_object_put(rows);
        (void)snprintf(worker->error, sizeof(worker->error), "json serialization returned null");
        return -1;
    }
    size_t bytes = strlen(json);
    char *copy = capture_json ? strdup(json) : NULL;
    if (capture_json && !copy) {
        json_object_put(rows);
        (void)snprintf(worker->error, sizeof(worker->error), "out of memory copying JSON sample");
        return -1;
    }
    if (sample) {
        sample->api_ns = end >= start ? end - start : 0;
        sample->mutex_wait_ns = timing.mutex_wait_ns;
        sample->prepare_ns = timing.prepare_ns;
        sample->sqlite_step_ns = timing.sqlite_step_ns;
        sample->json_mapping_ns = timing.json_mapping_ns;
        sample->serialization_ns = serialization_end >= serialization_start ? serialization_end - serialization_start : 0;
        sample->serialized_bytes = bytes;
        sample->rows = json_object_array_length(rows);
        sample->serialized = copy;
    }
    json_object_put(rows);
    return 0;
}

static void *task_list_c_api_worker_main(void *opaque) {
    Worker *worker = (Worker *)opaque;
    Sample initial = {0};
    if (measured_call(worker, &initial, true) == 0) {
        worker->initial_api_ns = initial.api_ns;
        worker->initial_serialization_ns = initial.serialization_ns;
        worker->initial_mutex_wait_ns = initial.mutex_wait_ns;
        worker->initial_prepare_ns = initial.prepare_ns;
        worker->initial_sqlite_step_ns = initial.sqlite_step_ns;
        worker->initial_json_mapping_ns = initial.json_mapping_ns;
        worker->initial_rows = initial.rows;
        worker->initial_serialized = initial.serialized;
    }
    for (int i = 0; i < WARMUP_CALLS && !worker->error[0]; ++i) {
        if (measured_call(worker, NULL, false) != 0) break;
    }

    /* Even a failed worker must join the gate, so the main thread cannot hang. */
    (void)pthread_mutex_lock(worker->gate_mutex);
    (*worker->ready_count)++;
    (void)pthread_cond_broadcast(worker->gate_cond);
    while (!*worker->start_flag) (void)pthread_cond_wait(worker->gate_cond, worker->gate_mutex);
    (void)pthread_mutex_unlock(worker->gate_mutex);

    if (!worker->error[0]) {
        for (int i = 0; i < worker->samples_per_worker; ++i) {
            if (measured_call(worker, &worker->samples[i], worker->worker_id == 0 && i == 0) != 0) break;
        }
    }
    return NULL;
}

static int parse_positive(const char *text, int min_value, int max_value, int *out) {
    if (!text || !*text || !out) return -1;
    errno = 0;
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (errno || !end || *end || value < min_value || value > max_value) return -1;
    *out = (int)value;
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 8) {
        fprintf(stderr, "usage: %s DATABASE ROOT_DIR AGING_SECONDS LIMIT SAMPLES_PER_WORKER CONCURRENCY baseline|candidate\n", argv[0]);
        return 2;
    }
    int aging = 0, limit = 0, samples_per_worker = 0, concurrency = 0;
    bool candidate = strcmp(argv[7], "candidate") == 0;
    if ((!candidate && strcmp(argv[7], "baseline") != 0) ||
        parse_positive(argv[3], 1, 86400, &aging) != 0 ||
        parse_positive(argv[4], 1, 200, &limit) != 0 ||
        parse_positive(argv[5], 1, 10000, &samples_per_worker) != 0 ||
        parse_positive(argv[6], 1, 64, &concurrency) != 0) return 2;

    Runtime runtime;
    memset(&runtime, 0, sizeof(runtime));
    if (pthread_mutex_init(&runtime.db_mutex, NULL) != 0) return 3;
    char home[AO_MAX_PATH];
    if (snprintf(home, sizeof(home), "%s/asgend-home", argv[2]) >= (int)sizeof(home) ||
        copy_arg(runtime.config.database_path, sizeof(runtime.config.database_path), argv[1]) != 0 ||
        copy_arg(runtime.config.root_dir, sizeof(runtime.config.root_dir), argv[2]) != 0 ||
        copy_arg(runtime.config.asgend_home, sizeof(runtime.config.asgend_home), home) != 0) {
        pthread_mutex_destroy(&runtime.db_mutex);
        return 2;
    }
    runtime.config.priority_aging_seconds = aging;
    char error[512] = {0};
    if (db_init(&runtime, error, sizeof(error)) != 0) {
        fprintf(stderr, "db_init failed: %s\n", error);
        if (runtime.db) db_close(&runtime);
        pthread_mutex_destroy(&runtime.db_mutex);
        return 4;
    }

    Worker *workers = calloc((size_t)concurrency, sizeof(*workers));
    pthread_t *threads = calloc((size_t)concurrency, sizeof(*threads));
    if (!workers || !threads) {
        free(workers); free(threads); db_close(&runtime); pthread_mutex_destroy(&runtime.db_mutex); return 5;
    }
    pthread_mutex_t gate_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
    int ready_count = 0;
    bool start_flag = false;
    int created = 0;
    for (int i = 0; i < concurrency; ++i) {
        workers[i].runtime = &runtime;
        workers[i].worker_id = i;
        workers[i].samples_per_worker = samples_per_worker;
        workers[i].limit = limit;
        workers[i].candidate = candidate;
        workers[i].gate_mutex = &gate_mutex;
        workers[i].gate_cond = &gate_cond;
        workers[i].ready_count = &ready_count;
        workers[i].start_flag = &start_flag;
        workers[i].samples = calloc((size_t)samples_per_worker, sizeof(Sample));
        if (!workers[i].samples || pthread_create(&threads[i], NULL, task_list_c_api_worker_main, &workers[i]) != 0) {
            (void)snprintf(error, sizeof(error), "failed creating benchmark worker %d", i);
            break;
        }
        created++;
    }
    if (created != concurrency) {
        (void)pthread_mutex_lock(&gate_mutex);
        start_flag = true;
        (void)pthread_cond_broadcast(&gate_cond);
        (void)pthread_mutex_unlock(&gate_mutex);
        for (int i = 0; i < created; ++i) (void)pthread_join(threads[i], NULL);
        for (int i = 0; i < concurrency; ++i) {
            if (workers[i].samples) free(workers[i].samples);
            free(workers[i].initial_serialized);
        }
        free(workers); free(threads); db_close(&runtime); pthread_mutex_destroy(&runtime.db_mutex);
        pthread_cond_destroy(&gate_cond); pthread_mutex_destroy(&gate_mutex);
        fprintf(stderr, "%s\n", error);
        return 6;
    }

    (void)pthread_mutex_lock(&gate_mutex);
    while (ready_count < concurrency) (void)pthread_cond_wait(&gate_cond, &gate_mutex);
    uint64_t wall_start = monotonic_ns();
    start_flag = true;
    (void)pthread_cond_broadcast(&gate_cond);
    (void)pthread_mutex_unlock(&gate_mutex);
    for (int i = 0; i < concurrency; ++i) (void)pthread_join(threads[i], NULL);
    uint64_t wall_end = monotonic_ns();

    json_object *report = json_object_new_object();
    json_object_object_add(report, "mode", json_object_new_string(candidate ? "candidate" : "baseline"));
    json_object_object_add(report, "api", json_object_new_string(candidate ? "db_list_tasks_candidate(test-only)" : "db_list_tasks(production)"));
    json_object_object_add(report, "limit", json_object_new_int(limit));
    json_object_object_add(report, "concurrency", json_object_new_int(concurrency));
    json_object_object_add(report, "samples_per_worker", json_object_new_int(samples_per_worker));
    json_object_object_add(report, "warmup_calls_per_worker", json_object_new_int(WARMUP_CALLS));
    json_object_object_add(report, "initial_call_is_os_cold_cache", json_object_new_boolean(false));
    json_object_object_add(report, "first_call_note", json_object_new_string("first API call after db_init in this process; not an OS page-cache cold measurement"));
    json_object_object_add(report, "wall_ns", json_object_new_int64((int64_t)(wall_end >= wall_start ? wall_end - wall_start : 0)));

    json_object *samples = json_object_new_array();
    json_object *initial_calls = json_object_new_array();
    json_object *first_result = NULL;
    uint64_t total_api_ns = 0, total_serialization_ns = 0, total_bytes = 0;
    int total_rows = 0, total_samples = 0, errors = 0;
    for (int i = 0; i < concurrency; ++i) {
        Worker *worker = &workers[i];
        if (worker->error[0]) errors++;
        json_object *initial = json_object_new_object();
        json_object_object_add(initial, "worker", json_object_new_int(i));
        json_object_object_add(initial, "api_ns", json_object_new_int64((int64_t)worker->initial_api_ns));
        json_object_object_add(initial, "serialization_ns", json_object_new_int64((int64_t)worker->initial_serialization_ns));
        json_object_object_add(initial, "mutex_wait_ns", json_object_new_int64((int64_t)worker->initial_mutex_wait_ns));
        json_object_object_add(initial, "prepare_ns", json_object_new_int64((int64_t)worker->initial_prepare_ns));
        json_object_object_add(initial, "sqlite_step_ns", json_object_new_int64((int64_t)worker->initial_sqlite_step_ns));
        json_object_object_add(initial, "json_mapping_ns", json_object_new_int64((int64_t)worker->initial_json_mapping_ns));
        json_object_object_add(initial, "rows", json_object_new_int64((int64_t)worker->initial_rows));
        json_object_array_add(initial_calls, initial);
        if (!first_result && worker->initial_serialized) first_result = json_tokener_parse(worker->initial_serialized);
        for (int j = 0; j < samples_per_worker; ++j) {
            Sample *sample = &worker->samples[j];
            if (!sample->api_ns) continue;
            json_object *item = json_object_new_object();
            json_object_object_add(item, "worker", json_object_new_int(i));
            json_object_object_add(item, "api_ns", json_object_new_int64((int64_t)sample->api_ns));
            json_object_object_add(item, "mutex_wait_ns", json_object_new_int64((int64_t)sample->mutex_wait_ns));
            json_object_object_add(item, "prepare_ns", json_object_new_int64((int64_t)sample->prepare_ns));
            json_object_object_add(item, "sqlite_step_ns", json_object_new_int64((int64_t)sample->sqlite_step_ns));
            json_object_object_add(item, "json_mapping_ns", json_object_new_int64((int64_t)sample->json_mapping_ns));
            json_object_object_add(item, "serialization_ns", json_object_new_int64((int64_t)sample->serialization_ns));
            json_object_object_add(item, "serialized_bytes", json_object_new_int64((int64_t)sample->serialized_bytes));
            json_object_object_add(item, "rows", json_object_new_int64((int64_t)sample->rows));
            json_object_array_add(samples, item);
            total_api_ns += sample->api_ns;
            total_serialization_ns += sample->serialization_ns;
            total_bytes += sample->serialized_bytes;
            total_rows += (int)sample->rows;
            total_samples++;
        }
    }
    json_object_object_add(report, "initial_calls_after_db_init", initial_calls);
    json_object_object_add(report, "samples", samples);
    if (first_result) json_object_object_add(report, "first_result", first_result);
    json_object_object_add(report, "sample_count", json_object_new_int(total_samples));
    json_object_object_add(report, "error_count", json_object_new_int(errors));
    json_object_object_add(report, "total_api_ns", json_object_new_int64((int64_t)total_api_ns));
    json_object_object_add(report, "mean_api_ns", json_object_new_int64(total_samples ? (int64_t)(total_api_ns / (uint64_t)total_samples) : 0));
    json_object_object_add(report, "mean_serialization_ns", json_object_new_int64(total_samples ? (int64_t)(total_serialization_ns / (uint64_t)total_samples) : 0));
    json_object_object_add(report, "total_serialized_bytes", json_object_new_int64((int64_t)total_bytes));
    json_object_object_add(report, "mean_rows_per_call", json_object_new_double(total_samples ? (double)total_rows / (double)total_samples : 0.0));
    double wall_s = wall_end > wall_start ? (double)(wall_end - wall_start) / 1e9 : 0.0;
    json_object_object_add(report, "throughput_calls_per_second", json_object_new_double(wall_s > 0 ? (double)total_samples / wall_s : 0.0));

    int rc = 0;
    for (int i = 0; i < concurrency; ++i) {
        if (workers[i].error[0]) {
            fprintf(stderr, "worker %d failed: %s\n", i, workers[i].error);
            rc = 7;
        }
    }
    json_object_object_add(report, "passed", json_object_new_boolean(rc == 0 && total_samples == concurrency * samples_per_worker));
    const char *serialized_report = json_object_to_json_string_ext(report, JSON_C_TO_STRING_PLAIN);
    if (!serialized_report) rc = 8;
    else puts(serialized_report);
    json_object_put(report);
    for (int i = 0; i < concurrency; ++i) {
        for (int j = 0; j < samples_per_worker; ++j) free(workers[i].samples[j].serialized);
        free(workers[i].samples);
        free(workers[i].initial_serialized);
    }
    free(workers); free(threads);
    db_close(&runtime);
    pthread_mutex_destroy(&runtime.db_mutex);
    pthread_cond_destroy(&gate_cond);
    pthread_mutex_destroy(&gate_mutex);
    return rc;
}
