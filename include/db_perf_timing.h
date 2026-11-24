#ifndef ASGEND_DB_PERF_TIMING_H
#define ASGEND_DB_PERF_TIMING_H

#include <stdint.h>
#include "runtime.h"

typedef struct {
    uint64_t mutex_wait_ns;
    uint64_t prepare_ns;
    uint64_t sqlite_step_ns;
    uint64_t json_mapping_ns;
} DbPerfTiming;

#ifdef ASGEND_DB_PERF_BENCHMARK
void db_perf_timing_last(DbPerfTiming *out);
json_object *db_list_tasks_candidate(Runtime *rt, const char *status_filter, int limit,
                                    char *err, size_t err_size);
#endif

#endif
