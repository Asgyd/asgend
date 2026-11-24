#define _POSIX_C_SOURCE 200809L
#include "asgend_memory.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *program) {
    fprintf(stderr,
        "Usage:\n"
        "  %s scan ROOT [--verify] [--max-file-size BYTES]\n"
        "  %s verify ROOT [--max-file-size BYTES]\n"
        "  %s extract ROOT [--max-file-size BYTES] [--max-text-bytes BYTES] [--retry]\n"
        "  %s enrich ROOT [--workers N] [--timeout-seconds N] [--max-input-chars N] [--retry]\n"
        "     env: ASGEND_MEMORY_DECISION_BASE_URL, ASGEND_MEMORY_DECISION_MODEL, ASGEND_MEMORY_GENERATION_BASE_URL, ASGEND_MEMORY_GENERATION_MODEL\n"
        "  %s embed ROOT [--workers N] [--timeout-seconds N] [--retry] [--chunks] [--chunk-tokens N]\n"
        "     env: ASGEND_MEMORY_EMBEDDING_BASE_URL, ASGEND_MEMORY_EMBEDDING_MODEL, ASGEND_MEMORY_EMBEDDING_API_KEY\n     --chunks additionally embeds source-aware chunks for chunk-level semantic search\n"
        "  %s chunk ROOT [--max-chunk-tokens N]\n"
        "  %s query ROOT QUERY [--limit N] [--context-tokens N] [--chunk-tokens N] [--type VALUE] [--tag VALUE] [--path-prefix PATH] [--metadata KEY=VALUE] [--timeout-seconds N] [--lexical-only] [--exact-vectors] [--plan] [--rerank [--rerank-candidates N]]\n"
        "     BM25 is local; embedding config enables semantic fusion. --plan opts in to constrained query planning; --rerank sends top-K candidates to a decision model.\n"
        "     --plan and --rerank are EXPERIMENTAL in 1.0: their quality has not been evaluated on a human-judged corpus.\n"
        "     env: ASGEND_MEMORY_DECISION_BASE_URL, ASGEND_MEMORY_DECISION_MODEL, ASGEND_MEMORY_DECISION_API_KEY (for --plan)\n"
        "     env: ASGEND_MEMORY_RERANK_BASE_URL, ASGEND_MEMORY_RERANK_MODEL, ASGEND_MEMORY_RERANK_API_KEY\n"
        "  %s graph ROOT\n"
        "  %s ann-index ROOT\n"
        "  %s status ROOT\n"
        "  %s stats ROOT\n"
        "  %s gc ROOT [--vacuum]\n"
        "  %s events ROOT [LIMIT]\n",
        program, program, program, program, program, program, program, program, program, program, program, program, program);
}

static int parse_u64(const char *text, uint64_t *value) {
    if (!text || !text[0] || text[0] == '-') return -1;
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed == 0 || parsed > (unsigned long long)INT64_MAX) return -1;
    *value = (uint64_t)parsed;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(argv[0]); return 2; }
    const char *command = argv[1];
    const char *root = argv[2];
    char error[ASGEND_MEMORY_ERROR_MAX] = {0};
    if (!strcmp(command, "chunk")) {
        unsigned int max_tokens = 400;
        int i = 3;
        while (i < argc) {
            uint64_t parsed = 0;
            if (!strcmp(argv[i], "--max-chunk-tokens") && i + 1 < argc) {
                if (parse_u64(argv[i + 1], &parsed) != 0 || parsed > 8192) {
                    fprintf(stderr, "asgend-memory: --max-chunk-tokens must be between 1 and 8192\n"); return 2;
                }
                max_tokens = (unsigned int)parsed; i += 2; continue;
            }
            usage(argv[0]); return 2;
        }
        AsgendMemoryChunkResult result;
        if (asgend_memory_chunk(root, max_tokens, &result, stdout, error, sizeof(error)) != 0) {
            fprintf(stderr, "asgend-memory: %s\n", error[0] ? error : "chunking failed"); return 1;
        }
        return 0;
    }
    if (!strcmp(command, "graph")) {
        if (argc != 3) { usage(argv[0]); return 2; }
        if (asgend_memory_graph(root, stdout, error, sizeof(error)) != 0) {
            fprintf(stderr, "asgend-memory: %s\n", error[0] ? error : "graph build failed"); return 1;
        }
        return 0;
    }
    if (!strcmp(command, "ann-index")) {
        if (argc != 3) { usage(argv[0]); return 2; }
        if (asgend_memory_ann_index(root, stdout, error, sizeof(error)) != 0) {
            fprintf(stderr, "asgend-memory: %s\n", error[0] ? error : "ANN indexing failed"); return 1;
        }
        return 0;
    }
    if (!strcmp(command, "status")) {
        if (argc != 3) { usage(argv[0]); return 2; }
        if (asgend_memory_status(root, stdout, error, sizeof(error)) != 0) { fprintf(stderr, "asgend-memory: %s\n", error); return 1; }
        return 0;
    }
    if (!strcmp(command, "stats")) {
        if (argc != 3) { usage(argv[0]); return 2; }
        if (asgend_memory_stats(root, stdout, error, sizeof(error)) != 0) {
            fprintf(stderr, "asgend-memory: %s\n", error[0] ? error : "stats failed"); return 1;
        }
        return 0;
    }
    if (!strcmp(command, "gc")) {
        bool vacuum = false;
        if (argc == 4 && !strcmp(argv[3], "--vacuum")) vacuum = true;
        else if (argc != 3) { usage(argv[0]); return 2; }
        if (asgend_memory_gc(root, vacuum, stdout, error, sizeof(error)) != 0) {
            fprintf(stderr, "asgend-memory: %s\n", error[0] ? error : "garbage collection failed"); return 1;
        }
        return 0;
    }
    if (!strcmp(command, "events")) {
        uint64_t limit = 30;
        if (argc > 4 || (argc == 4 && parse_u64(argv[3], &limit) != 0)) { usage(argv[0]); return 2; }
        if (limit > 1000) limit = 1000;
        if (asgend_memory_events(root, (unsigned int)limit, stdout, error, sizeof(error)) != 0) { fprintf(stderr, "asgend-memory: %s\n", error); return 1; }
        return 0;
    }
    if (!strcmp(command, "extract")) {
        AsgendMemoryExtractOptions options = {0};
        options.max_file_size_bytes = ASGEND_MEMORY_DEFAULT_MAX_FILE_SIZE;
        options.max_text_bytes = ASGEND_MEMORY_DEFAULT_MAX_TEXT_SIZE;
        int i = 3;
        while (i < argc) {
            if (!strcmp(argv[i], "--retry")) { options.retry_failed = true; ++i; continue; }
            if (!strcmp(argv[i], "--max-file-size") && i + 1 < argc) {
                if (parse_u64(argv[i + 1], &options.max_file_size_bytes) != 0) {
                    fprintf(stderr, "asgend-memory: --max-file-size requires a positive integer number of bytes\n"); return 2;
                }
                i += 2; continue;
            }
            if (!strcmp(argv[i], "--max-text-bytes") && i + 1 < argc) {
                if (parse_u64(argv[i + 1], &options.max_text_bytes) != 0) {
                    fprintf(stderr, "asgend-memory: --max-text-bytes requires a positive integer number of bytes\n"); return 2;
                }
                i += 2; continue;
            }
            usage(argv[0]); return 2;
        }
        AsgendMemoryExtractResult result;
        if (asgend_memory_extract(root, &options, &result, error, sizeof(error)) != 0) {
            fprintf(stderr, "asgend-memory: %s\n", error[0] ? error : "extract failed"); return 1;
        }
        printf("status=%s files_seen=%" PRIu64 " extracted=%" PRIu64 " truncated=%" PRIu64
               " unsupported=%" PRIu64 " skipped=%" PRIu64 " file_errors=%" PRIu64 " stale_sources=%" PRIu64 "\n",
               result.complete ? "complete" : "partial", result.files_seen, result.extracted, result.truncated,
               result.unsupported, result.skipped, result.file_errors, result.stale_sources);
        return 0;
    }
    if (!strcmp(command, "enrich")) {
        AsgendMemoryEnrichOptions options = {0};
        options.decision_base_url = getenv("ASGEND_MEMORY_DECISION_BASE_URL");
        options.decision_model_id = getenv("ASGEND_MEMORY_DECISION_MODEL");
        options.decision_api_key = getenv("ASGEND_MEMORY_DECISION_API_KEY");
        options.generation_base_url = getenv("ASGEND_MEMORY_GENERATION_BASE_URL");
        options.generation_model_id = getenv("ASGEND_MEMORY_GENERATION_MODEL");
        options.generation_api_key = getenv("ASGEND_MEMORY_GENERATION_API_KEY");
        options.workers = 2;
        options.timeout_seconds = 120;
        options.max_input_chars = 24000;
        int i = 3;
        while (i < argc) {
            uint64_t parsed = 0;
            if (!strcmp(argv[i], "--retry")) { options.retry_failed = true; ++i; continue; }
            if (!strcmp(argv[i], "--workers") && i + 1 < argc) {
                if (parse_u64(argv[i + 1], &parsed) != 0 || parsed > 16) {
                    fprintf(stderr, "asgend-memory: --workers must be between 1 and 16\n"); return 2;
                }
                options.workers = (unsigned int)parsed; i += 2; continue;
            }
            if (!strcmp(argv[i], "--timeout-seconds") && i + 1 < argc) {
                if (parse_u64(argv[i + 1], &parsed) != 0 || parsed > 3600) {
                    fprintf(stderr, "asgend-memory: --timeout-seconds must be between 1 and 3600\n"); return 2;
                }
                options.timeout_seconds = (unsigned int)parsed; i += 2; continue;
            }
            if (!strcmp(argv[i], "--max-input-chars") && i + 1 < argc) {
                if (parse_u64(argv[i + 1], &parsed) != 0 || parsed > 1000000) {
                    fprintf(stderr, "asgend-memory: --max-input-chars must be between 1 and 1000000\n"); return 2;
                }
                options.max_input_chars = parsed; i += 2; continue;
            }
            usage(argv[0]); return 2;
        }
        AsgendMemoryEnrichResult result;
        if (asgend_memory_enrich(root, &options, &result, error, sizeof(error)) != 0) {
            fprintf(stderr, "asgend-memory: %s\n", error[0] ? error : "enrich failed"); return 1;
        }
        printf("status=%s files_seen=%" PRIu64 " metadata_generated=%" PRIu64
               " descriptions_generated=%" PRIu64 " skipped=%" PRIu64 " deferred=%" PRIu64 " failed=%" PRIu64 "\n",
               result.complete ? "complete" : "partial", result.files_seen, result.metadata_generated,
               result.descriptions_generated, result.skipped, result.deferred, result.failed);
        if (!result.complete && error[0]) fprintf(stderr, "asgend-memory: first pipeline error: %s\n", error);
        return result.complete ? 0 : 3;
    }
    if (!strcmp(command, "embed")) {
        AsgendMemoryEmbedOptions options = {0};
        options.base_url = getenv("ASGEND_MEMORY_EMBEDDING_BASE_URL");
        options.model_id = getenv("ASGEND_MEMORY_EMBEDDING_MODEL");
        options.api_key = getenv("ASGEND_MEMORY_EMBEDDING_API_KEY");
        options.workers = 2;
        options.timeout_seconds = 120;
        int i = 3;
        while (i < argc) {
            uint64_t parsed = 0;
            if (!strcmp(argv[i], "--retry")) { options.retry_failed = true; ++i; continue; }
            if (!strcmp(argv[i], "--chunks")) { options.embed_chunks = true; ++i; continue; }
            if (!strcmp(argv[i], "--chunk-tokens") && i + 1 < argc) {
                if (parse_u64(argv[i + 1], &parsed) != 0 || parsed > 8192) {
                    fprintf(stderr, "asgend-memory: --chunk-tokens must be between 1 and 8192\n"); return 2;
                }
                options.max_chunk_tokens = (unsigned int)parsed; i += 2; continue;
            }
            if (!strcmp(argv[i], "--workers") && i + 1 < argc) {
                if (parse_u64(argv[i + 1], &parsed) != 0 || parsed > 16) {
                    fprintf(stderr, "asgend-memory: --workers must be between 1 and 16\n"); return 2;
                }
                options.workers = (unsigned int)parsed; i += 2; continue;
            }
            if (!strcmp(argv[i], "--timeout-seconds") && i + 1 < argc) {
                if (parse_u64(argv[i + 1], &parsed) != 0 || parsed > 3600) {
                    fprintf(stderr, "asgend-memory: --timeout-seconds must be between 1 and 3600\n"); return 2;
                }
                options.timeout_seconds = (unsigned int)parsed; i += 2; continue;
            }
            usage(argv[0]); return 2;
        }
        AsgendMemoryEmbedResult result;
        if (asgend_memory_embed(root, &options, &result, error, sizeof(error)) != 0) {
            fprintf(stderr, "asgend-memory: %s\n", error[0] ? error : "embed failed"); return 1;
        }
        printf("status=%s files_seen=%" PRIu64 " embedded=%" PRIu64 " skipped=%" PRIu64 " deferred=%" PRIu64 " failed=%" PRIu64
               " chunks_seen=%" PRIu64 " chunks_embedded=%" PRIu64 " chunks_skipped=%" PRIu64 " chunks_failed=%" PRIu64 "\n",
               result.complete ? "complete" : "partial", result.files_seen, result.embedded, result.skipped, result.deferred, result.failed,
               result.chunks_seen, result.chunks_embedded, result.chunks_skipped, result.chunks_failed);
        if (!result.complete && error[0]) fprintf(stderr, "asgend-memory: first embedding error: %s\n", error);
        return result.complete ? 0 : 3;
    }
    if (!strcmp(command, "query")) {
        if (argc < 4) { usage(argv[0]); return 2; }
        AsgendMemoryQueryOptions options = {0};
        options.base_url = getenv("ASGEND_MEMORY_EMBEDDING_BASE_URL");
        options.model_id = getenv("ASGEND_MEMORY_EMBEDDING_MODEL");
        options.api_key = getenv("ASGEND_MEMORY_EMBEDDING_API_KEY");
        options.rerank_base_url = getenv("ASGEND_MEMORY_RERANK_BASE_URL");
        options.rerank_model_id = getenv("ASGEND_MEMORY_RERANK_MODEL");
        options.rerank_api_key = getenv("ASGEND_MEMORY_RERANK_API_KEY");
        options.decision_base_url = getenv("ASGEND_MEMORY_DECISION_BASE_URL");
        options.decision_model_id = getenv("ASGEND_MEMORY_DECISION_MODEL");
        options.decision_api_key = getenv("ASGEND_MEMORY_DECISION_API_KEY");
        options.limit = 10;
        options.timeout_seconds = 120;
        int i = 4;
        while (i < argc) {
            uint64_t parsed = 0;
            if (!strcmp(argv[i], "--lexical-only")) { options.lexical_only = true; ++i; continue; }
            if (!strcmp(argv[i], "--exact-vectors")) { options.exact_vector_search = true; ++i; continue; }
            if (!strcmp(argv[i], "--plan")) { options.query_plan = true; ++i; continue; }
            if (!strcmp(argv[i], "--rerank")) { options.decision_rerank = true; ++i; continue; }
            if (!strcmp(argv[i], "--rerank-candidates") && i + 1 < argc) {
                if (parse_u64(argv[i + 1], &parsed) != 0 || parsed > 20) {
                    fprintf(stderr, "asgend-memory: --rerank-candidates must be between 1 and 20\n"); return 2;
                }
                options.rerank_candidates = (unsigned int)parsed; i += 2; continue;
            }
            if ((!strcmp(argv[i], "--type") || !strcmp(argv[i], "--tag") || !strcmp(argv[i], "--path-prefix")) && i + 1 < argc) {
                if (!argv[i + 1][0]) { fprintf(stderr, "asgend-memory: filter values cannot be empty\n"); return 2; }
                if (!strcmp(argv[i], "--type")) options.metadata_type = argv[i + 1];
                else if (!strcmp(argv[i], "--tag")) options.tag = argv[i + 1];
                else options.path_prefix = argv[i + 1];
                i += 2; continue;
            }
            if (!strcmp(argv[i], "--metadata") && i + 1 < argc) {
                char *equal = strchr(argv[i + 1], '=');
                if (!equal || equal == argv[i + 1] || !equal[1]) { fprintf(stderr, "asgend-memory: --metadata requires KEY=VALUE\n"); return 2; }
                options.metadata_key = argv[i + 1];
                options.metadata_value = equal + 1;
                /* Keep key as a separate owned string to avoid passing the '=VALUE' suffix. */
                static char metadata_key[256];
                size_t key_len = (size_t)(equal - argv[i + 1]);
                if (key_len >= sizeof(metadata_key)) { fprintf(stderr, "asgend-memory: metadata key is too long\n"); return 2; }
                memcpy(metadata_key, argv[i + 1], key_len); metadata_key[key_len] = '\0';
                options.metadata_key = metadata_key;
                i += 2; continue;
            }
            if ((!strcmp(argv[i], "--context-tokens") || !strcmp(argv[i], "--chunk-tokens")) && i + 1 < argc) {
                if (parse_u64(argv[i + 1], &parsed) != 0) { fprintf(stderr, "asgend-memory: token limit requires a positive integer\n"); return 2; }
                if (!strcmp(argv[i], "--context-tokens")) {
                    if (parsed > 100000) { fprintf(stderr, "asgend-memory: --context-tokens must be <= 100000\n"); return 2; }
                    options.context_token_budget = (unsigned int)parsed;
                } else {
                    if (parsed > 8192) { fprintf(stderr, "asgend-memory: --chunk-tokens must be <= 8192\n"); return 2; }
                    options.max_chunk_tokens = (unsigned int)parsed;
                }
                i += 2; continue;
            }
            if (!strcmp(argv[i], "--limit") && i + 1 < argc) {
                if (parse_u64(argv[i + 1], &parsed) != 0 || parsed > 100) {
                    fprintf(stderr, "asgend-memory: --limit must be between 1 and 100\n"); return 2;
                }
                options.limit = (unsigned int)parsed; i += 2; continue;
            }
            if (!strcmp(argv[i], "--timeout-seconds") && i + 1 < argc) {
                if (parse_u64(argv[i + 1], &parsed) != 0 || parsed > 3600) {
                    fprintf(stderr, "asgend-memory: --timeout-seconds must be between 1 and 3600\n"); return 2;
                }
                options.timeout_seconds = (unsigned int)parsed; i += 2; continue;
            }
            usage(argv[0]); return 2;
        }
        if (asgend_memory_query(root, argv[3], &options, stdout, error, sizeof(error)) != 0) {
            fprintf(stderr, "asgend-memory: %s\n", error[0] ? error : "query failed"); return 1;
        }
        return 0;
    }
    if (!strcmp(command, "scan") || !strcmp(command, "verify")) {
        AsgendMemoryScanOptions options = {0};
        options.verify_hashes = !strcmp(command, "verify");
        options.max_file_size_bytes = ASGEND_MEMORY_DEFAULT_MAX_FILE_SIZE;
        int i = 3;
        while (i < argc) {
            if (!strcmp(argv[i], "--verify") && !options.verify_hashes) { options.verify_hashes = true; ++i; continue; }
            if (!strcmp(argv[i], "--max-file-size") && i + 1 < argc) {
                if (parse_u64(argv[i + 1], &options.max_file_size_bytes) != 0) {
                    fprintf(stderr, "asgend-memory: --max-file-size requires a positive integer number of bytes\n"); return 2;
                }
                i += 2; continue;
            }
            usage(argv[0]); return 2;
        }
        AsgendMemoryScanResult result;
        if (asgend_memory_scan(root, &options, &result, error, sizeof(error)) != 0) {
            fprintf(stderr, "asgend-memory: %s\n", error[0] ? error : "scan failed"); return 1;
        }
        printf("scan_id=%" PRId64 " status=%s files=%" PRIu64 " added=%" PRIu64 " modified=%" PRIu64
               " deleted=%" PRIu64 " renamed=%" PRIu64 " restored=%" PRIu64 " file_errors=%" PRIu64
               " walk_errors=%" PRIu64 " skipped_symlinks=%" PRIu64 " skipped_mountpoints=%" PRIu64 "\n",
               result.scan_id, result.complete ? "complete" : "partial", result.files_seen, result.added,
               result.modified, result.deleted, result.renamed, result.restored, result.file_errors,
               result.walk_errors, result.skipped_symlinks, result.skipped_mountpoints);
        return 0;
    }
    usage(argv[0]);
    return 2;
}
