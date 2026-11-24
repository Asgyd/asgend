#ifndef ASGEND_MEMORY_H
#define ASGEND_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASGEND_MEMORY_DEFAULT_MAX_FILE_SIZE (256ULL * 1024ULL * 1024ULL)
#define ASGEND_MEMORY_DEFAULT_MAX_TEXT_SIZE (4ULL * 1024ULL * 1024ULL)
#define ASGEND_MEMORY_ERROR_MAX 512

typedef struct {
    bool verify_hashes;          /* Re-hash files even when size/mtime are unchanged. */
    uint64_t max_file_size_bytes; /* Zero selects ASGEND_MEMORY_DEFAULT_MAX_FILE_SIZE. */
    bool exclude_resource_directories; /* Exclude top-level skills/knowledge/tools/system_prompts/instructions from project memory. */
} AsgendMemoryScanOptions;

typedef struct {
    uint64_t max_file_size_bytes; /* Zero selects ASGEND_MEMORY_DEFAULT_MAX_FILE_SIZE. */
    uint64_t max_text_bytes;      /* Zero selects ASGEND_MEMORY_DEFAULT_MAX_TEXT_SIZE. */
    bool retry_failed;            /* Re-run failed/stale extractors even when hash is unchanged. */
} AsgendMemoryExtractOptions;

typedef struct {
    uint64_t files_seen;
    uint64_t extracted;
    uint64_t truncated;
    uint64_t unsupported;
    uint64_t skipped;
    uint64_t file_errors;
    uint64_t stale_sources;
    bool complete;
} AsgendMemoryExtractResult;


typedef struct {
    const char *decision_base_url;  /* OpenAI-compatible /chat/completions endpoint or base URL. */
    const char *decision_model_id;
    const char *decision_api_key;   /* Read from environment by CLI; never stored in .asgend_mem. */
    const char *generation_base_url;
    const char *generation_model_id;
    const char *generation_api_key;
    unsigned int workers;           /* 1..16; zero selects 2. */
    unsigned int timeout_seconds;   /* Zero selects 120 seconds. */
    uint64_t max_input_chars;        /* Zero selects 24000 Unicode code points. */
    bool retry_failed;               /* Ignore retry backoff for failed jobs. */
} AsgendMemoryEnrichOptions;

typedef struct {
    uint64_t files_seen;
    uint64_t metadata_generated;
    uint64_t descriptions_generated;
    uint64_t skipped;
    uint64_t failed;
    uint64_t deferred;
    bool complete;
} AsgendMemoryEnrichResult;

typedef struct {
    const char *base_url;          /* OpenAI-compatible /embeddings endpoint or base URL. */
    const char *model_id;
    const char *api_key;           /* Read from environment by CLI; never stored in the index. */
    unsigned int workers;           /* 1..16; zero selects 2. */
    unsigned int timeout_seconds;   /* Zero selects 120 seconds. */
    bool retry_failed;              /* Ignore persisted retry backoff for failed jobs. */
    bool embed_chunks;               /* Also embed source-aware chunks for precise semantic retrieval. */
    unsigned int max_chunk_tokens;    /* Chunking size when embed_chunks is true; zero selects 400. */
} AsgendMemoryEmbedOptions;

typedef struct {
    uint64_t files_seen;
    uint64_t embedded;
    uint64_t skipped;
    uint64_t failed;
    uint64_t deferred;
    uint64_t chunks_seen;
    uint64_t chunks_embedded;
    uint64_t chunks_skipped;
    uint64_t chunks_failed;
    bool complete;
} AsgendMemoryEmbedResult;

typedef struct {
    const char *base_url;            /* Optional: enables semantic/hybrid search when configured. */
    const char *model_id;
    const char *api_key;
    unsigned int timeout_seconds;    /* Zero selects 120 seconds. */
    unsigned int limit;              /* 1..100; zero selects 10. */
    bool lexical_only;               /* Do not invoke embedding API for the query. */
    const char *metadata_type;        /* Optional exact OKF/model metadata field type filter. */
    const char *tag;                  /* Optional exact tag membership filter. */
    const char *path_prefix;          /* Optional root-relative path prefix filter. */
    const char *metadata_key;         /* Optional arbitrary metadata key for an exact string match. */
    const char *metadata_value;       /* Value paired with metadata_key. */
    unsigned int context_token_budget;/* Conservative output budget; zero selects 2000. */
    unsigned int max_chunk_tokens;    /* Chunk size estimate; zero selects 400. */
    bool decision_rerank;              /* Explicitly enable constrained top-K decision-model reranking. */
    bool exact_vector_search;           /* Disable ANN and compare/use full exact cosine scan. */
    const char *rerank_base_url;       /* OpenAI-compatible Chat Completions base URL. */
    const char *rerank_model_id;
    const char *rerank_api_key;        /* Environment-sourced; never stored in the index. */
    unsigned int rerank_candidates;    /* 1..20; zero selects 12 when reranking is enabled. */
    bool query_plan;                   /* Opt-in decision-model query planning; falls back locally on any planner error. */
    const char *decision_base_url;     /* OpenAI-compatible Chat Completions endpoint/base URL for --plan. */
    const char *decision_model_id;
    const char *decision_api_key;      /* Environment-sourced; never stored in the index. */
} AsgendMemoryQueryOptions;

typedef struct {
    uint64_t files_seen;
    uint64_t files_chunked;
    uint64_t chunks_created;
    uint64_t skipped;
    bool complete;
} AsgendMemoryChunkResult;

typedef struct {
    int64_t scan_id;
    uint64_t files_seen;
    uint64_t added;
    uint64_t modified;
    uint64_t deleted;
    uint64_t renamed;
    uint64_t restored;
    uint64_t file_errors;
    uint64_t walk_errors;
    uint64_t skipped_symlinks;
    uint64_t skipped_mountpoints;
    bool complete;
} AsgendMemoryScanResult;

/* Creates ROOT/.asgend_mem/index.sqlite3 as needed. Source files are never modified. */
int asgend_memory_scan(const char *root, const AsgendMemoryScanOptions *options,
                       AsgendMemoryScanResult *result,
                       char *error, size_t error_size);

/* Extracts UTF-8 text and existing Markdown OKF frontmatter into .asgend_mem. */
int asgend_memory_extract(const char *root, const AsgendMemoryExtractOptions *options,
                          AsgendMemoryExtractResult *result,
                          char *error, size_t error_size);

/* Runs resumable decision-metadata and title/description stages against OpenAI-compatible APIs. */
int asgend_memory_enrich(const char *root, const AsgendMemoryEnrichOptions *options,
                         AsgendMemoryEnrichResult *result,
                         char *error, size_t error_size);

/* Generates and persists description embeddings. The source tree remains read-only. */
int asgend_memory_embed(const char *root, const AsgendMemoryEmbedOptions *options,
                        AsgendMemoryEmbedResult *result,
                        char *error, size_t error_size);

/* Builds resumable source-aware chunks. Coordinates refer to original source-file byte offsets and lines. */
int asgend_memory_chunk(const char *root, unsigned int max_chunk_tokens,
                        AsgendMemoryChunkResult *result, FILE *out,
                        char *error, size_t error_size);

/* Queries the indexed corpus with fragment BM25, optional vector fusion, filters and a context budget. */
int asgend_memory_query(const char *root, const char *query,
                        const AsgendMemoryQueryOptions *options, FILE *out,
                        char *error, size_t error_size);

/* Backfills ANN signatures for existing document/chunk embeddings. */
int asgend_memory_ann_index(const char *root, FILE *out, char *error, size_t error_size);

/* Rebuilds local file dependency edges from Markdown links and source imports. */
int asgend_memory_graph(const char *root, FILE *out,
                        char *error, size_t error_size);

int asgend_memory_status(const char *root, FILE *out,
                         char *error, size_t error_size);
/* Emits machine-readable observability JSON: storage sizes, stage counts, and current/stale vectors. */
int asgend_memory_stats(const char *root, FILE *out,
                        char *error, size_t error_size);
/* Removes stale retrieval rows while preserving file identity, scan history, and file events. */
int asgend_memory_gc(const char *root, bool vacuum, FILE *out,
                     char *error, size_t error_size);
int asgend_memory_events(const char *root, unsigned int limit, FILE *out,
                         char *error, size_t error_size);

#ifdef __cplusplus
}
#endif
#endif
