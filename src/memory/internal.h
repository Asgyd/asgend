#ifndef ASGEND_MEMORY_INTERNAL_H
#define ASGEND_MEMORY_INTERNAL_H

#include <sqlite3.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int asgend_memory_internal_open_index(const char *root, char **canonical_out,
                                      sqlite3 **db_out, char **index_file_out,
                                      char *error, size_t error_size);
void asgend_memory_internal_set_error(char *buffer, size_t size,
                                      const char *format, ...);
int asgend_memory_internal_sqlite_error(sqlite3 *db, char *error,
                                        size_t error_size, const char *operation);
int asgend_memory_internal_exec_sql(sqlite3 *db, const char *sql,
                                    char *error, size_t error_size);
uint16_t asgend_memory_ann_signature(const double *vector, size_t dimensions);
void asgend_memory_ann_signature_hex(uint16_t signature, char out[5]);
int asgend_memory_ann_prepare_probes(sqlite3 *db, const double *vector, size_t dimensions,
                                     char *error, size_t error_size);

int asgend_memory_internal_sync_chunks(sqlite3 *db, unsigned int max_chunk_tokens,
                                       uint64_t *files_seen, uint64_t *files_chunked,
                                       uint64_t *chunks_created, uint64_t *skipped,
                                       char *error, size_t error_size);

/* ---- Document (PDF / OOXML) text extraction, shared helpers in doc_common.c ---- */

#define ASGEND_MEMORY_PDF_EXTRACTOR_VERSION "pdf-text-v1"
#define ASGEND_MEMORY_OOXML_EXTRACTOR_VERSION "ooxml-text-v1"
/* Whole-file input cap for binary document extractors (they need random access). */
#define ASGEND_MEMORY_DOC_MAX_INPUT (64ULL * 1024ULL * 1024ULL)

/* Bounded UTF-8 output buffer. Only valid, non-control Unicode scalar values are kept. */
typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    size_t limit;          /* Maximum stored bytes (excluding the NUL terminator). */
    unsigned int pending;  /* 0 none, 1 space, 2 newline, 3 paragraph break (lazy separators). */
    bool truncated;
    bool oom;
} AsgendMemoryTextBuffer;

typedef struct {
    char *text;            /* NUL-terminated valid UTF-8 owned by the caller (may be empty). */
    size_t text_len;
    bool truncated;        /* Output reached the configured text limit. */
    bool partial;          /* Some structure could not be decoded; text is incomplete. */
    bool unsupported;      /* Nothing could be extracted for a structural/security reason. */
    char reason[200];      /* First partial/unsupported reason, e.g. "encrypted". */
} AsgendMemoryDocText;

int asgend_memory_text_init(AsgendMemoryTextBuffer *buffer, size_t limit);
void asgend_memory_text_free(AsgendMemoryTextBuffer *buffer);
/* Appends one code point; drops NUL/control/surrogate/out-of-range values. */
void asgend_memory_text_codepoint(AsgendMemoryTextBuffer *buffer, uint32_t codepoint);
/* Decodes UTF-8 leniently: malformed sequences are skipped byte-by-byte. */
void asgend_memory_text_utf8(AsgendMemoryTextBuffer *buffer, const char *bytes, size_t length);
/* Requests a separator (1 space, 2 newline, 3 blank line) emitted before the next text. */
void asgend_memory_text_separator(AsgendMemoryTextBuffer *buffer, unsigned int kind);
/* Moves the buffer into a DocText result (always NUL-terminated on success). */
int asgend_memory_text_finish(AsgendMemoryTextBuffer *buffer, AsgendMemoryDocText *out);
void asgend_memory_doc_reason(AsgendMemoryDocText *out, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

/* Inflates zlib (raw=false) or raw deflate (raw=true) data. Never writes more than max_output
   bytes; *limit_hit is set when the stream would have produced more. Returns 0 when at least the
   bounded prefix was decoded, -1 on corrupt input before any output, -2 on out-of-memory. */
int asgend_memory_inflate_bounded(const unsigned char *input, size_t input_len, bool raw,
                                  size_t max_output, unsigned char **output, size_t *output_len,
                                  bool *limit_hit, bool *corrupt);

/* Both extractors return 0 when out is filled (inspect unsupported/partial/truncated) and -1 only
   on out-of-memory. Malformed input never crashes; it yields partial/unsupported results. */
int asgend_memory_internal_extract_pdf(const unsigned char *data, size_t length,
                                       size_t max_text_bytes, AsgendMemoryDocText *out);
int asgend_memory_internal_extract_ooxml(const unsigned char *data, size_t length,
                                         size_t max_text_bytes, AsgendMemoryDocText *out);

/* ---- Decision-model query planning, see plan.c ---- */

#define ASGEND_MEMORY_PLAN_MAX_REWRITES 3U
#define ASGEND_MEMORY_PLAN_MAX_QUERY 256U

typedef struct {
    bool requested;
    bool used;
    char model_id[256];
    char fallback_reason[256];
    char intent[32];
    char reason[256];
    char rewritten[ASGEND_MEMORY_PLAN_MAX_REWRITES][ASGEND_MEMORY_PLAN_MAX_QUERY + 1U];
    size_t rewritten_count;
    char filter_type[129];
    char filter_tag[129];
    char filter_path_prefix[257];
} AsgendMemoryQueryPlan;

/* Sends the query plus a fixed intent allowlist (never retrieved document content) to an
   OpenAI-compatible Chat Completions planner and validates strict JSON. Never fails hard:
   on any error plan->used=false with fallback_reason. */
void asgend_memory_internal_plan_query(const char *query, const char *base_url,
                                       const char *model_id, const char *api_key,
                                       unsigned int timeout_seconds,
                                       const char *const *allowed_intents, size_t intent_count,
                                       AsgendMemoryQueryPlan *plan);

#endif
