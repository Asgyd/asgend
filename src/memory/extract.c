#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L
#include "asgend_memory.h"
#include "internal.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <openssl/evp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define EXTRACT_READ_BUFFER (64U * 1024U)
#define EXTRACT_BATCH_SIZE 64U
#define EXTRACT_MAX_TEXT_LIMIT (64ULL * 1024ULL * 1024ULL)
#define EXTRACTOR_VERSION "utf8-text-okf-v2-source-offsets"

typedef struct {
    int64_t file_id;
    char *relative_path;
    char *extension;
    char *mime_type;
    int64_t file_size;
    int64_t mtime_ns;
    char *sha256;
    char *previous_sha256;
    char *previous_status;
    char *previous_extractor_id;
} ExtractFile;

typedef struct {
    char *text;
    size_t text_len;
    uint64_t source_bytes;
    char sha256[65];
    bool truncated;
    bool valid_utf8_text;
} ReadResult;

typedef struct {
    unsigned char *data;
    size_t length;
    uint64_t source_bytes;
    char sha256[65];
} BinaryReadResult;

typedef struct {
    uint32_t codepoint;
    uint32_t minimum;
    unsigned int remaining;
    bool invalid;
} Utf8State;

typedef struct {
    char *text;
    size_t indent;
} YamlLine;

static void mem_error(char *error, size_t size, const char *format, ...) {
    if (!error || size == 0) return;
    va_list args;
    va_start(args, format);
    (void)vsnprintf(error, size, format, args);
    va_end(args);
}

static int64_t mtime_ns(const struct stat *st) {
    return (int64_t)st->st_mtim.tv_sec * INT64_C(1000000000) + (int64_t)st->st_mtim.tv_nsec;
}

static char *trim_in_place(char *text) {
    if (!text) return text;
    while (*text && isspace((unsigned char)*text)) ++text;
    size_t length = strlen(text);
    while (length > 0 && isspace((unsigned char)text[length - 1])) text[--length] = '\0';
    return text;
}

static char *duplicate_range(const char *text, size_t length) {
    char *copy = malloc(length + 1U);
    if (!copy) return NULL;
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

static bool is_textual_mime(const char *mime) {
    if (!mime) return false;
    if (!strncasecmp(mime, "text/", 5)) return true;
    return !strcasecmp(mime, "application/json") ||
           !strcasecmp(mime, "application/xml") ||
           !strcasecmp(mime, "application/javascript") ||
           !strcasecmp(mime, "application/x-javascript") ||
           !strcasecmp(mime, "application/x-yaml") ||
           !strcasecmp(mime, "application/yaml") ||
           !strcasecmp(mime, "application/toml") ||
           !strcasecmp(mime, "application/sql") ||
           !strcasecmp(mime, "image/svg+xml");
}

static bool is_known_binary_mime(const char *mime) {
    if (!mime) return false;
    return !strcasecmp(mime, "application/pdf") ||
           !strcasecmp(mime, "application/zip") ||
           !strcasecmp(mime, "application/gzip") ||
           !strcasecmp(mime, "application/x-executable") ||
           !strcasecmp(mime, "application/octet-stream+binary") ||
           (!strncasecmp(mime, "image/", 6) && strcasecmp(mime, "image/svg+xml") != 0) ||
           !strncasecmp(mime, "audio/", 6) || !strncasecmp(mime, "video/", 6) ||
           !strncasecmp(mime, "font/", 5);
}

static bool utf8_feed(Utf8State *state, const unsigned char *bytes, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        unsigned char byte = bytes[i];
        if (state->remaining == 0) {
            if (byte <= 0x7f) {
                if (byte == 0 || (byte < 0x20 && byte != '\n' && byte != '\r' && byte != '\t' && byte != '\f')) {
                    state->invalid = true;
                    return false;
                }
                continue;
            }
            if (byte >= 0xc2 && byte <= 0xdf) {
                state->codepoint = (uint32_t)(byte & 0x1fU);
                state->minimum = 0x80U;
                state->remaining = 1;
            } else if (byte >= 0xe0 && byte <= 0xef) {
                state->codepoint = (uint32_t)(byte & 0x0fU);
                state->minimum = 0x800U;
                state->remaining = 2;
            } else if (byte >= 0xf0 && byte <= 0xf4) {
                state->codepoint = (uint32_t)(byte & 0x07U);
                state->minimum = 0x10000U;
                state->remaining = 3;
            } else {
                state->invalid = true;
                return false;
            }
        } else {
            if ((byte & 0xc0U) != 0x80U) {
                state->invalid = true;
                return false;
            }
            state->codepoint = (state->codepoint << 6U) | (uint32_t)(byte & 0x3fU);
            --state->remaining;
            if (state->remaining == 0 &&
                (state->codepoint < state->minimum || state->codepoint > 0x10ffffU ||
                 (state->codepoint >= 0xd800U && state->codepoint <= 0xdfffU))) {
                state->invalid = true;
                return false;
            }
        }
    }
    return true;
}

static size_t complete_utf8_prefix(const unsigned char *text, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        unsigned char first = text[offset];
        size_t need = first < 0x80 ? 1U :
                      (first >= 0xc2 && first <= 0xdf) ? 2U :
                      (first >= 0xe0 && first <= 0xef) ? 3U :
                      (first >= 0xf0 && first <= 0xf4) ? 4U : 1U;
        if (offset + need > length) break;
        offset += need;
    }
    return offset;
}

static int hash_to_hex(EVP_MD_CTX *digest, char output[65]) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    if (EVP_DigestFinal_ex(digest, hash, &hash_len) != 1 || hash_len != 32) return -1;
    static const char digits[] = "0123456789abcdef";
    for (unsigned int i = 0; i < hash_len; ++i) {
        output[i * 2U] = digits[hash[i] >> 4U];
        output[i * 2U + 1U] = digits[hash[i] & 0x0fU];
    }
    output[64] = '\0';
    return 0;
}

/* Opens each path component without following symlinks, including after the scan. */
static int open_relative_regular(int root_fd, const char *relative_path,
                                 struct stat *st, char *error, size_t error_size) {
    if (!relative_path || !relative_path[0] || relative_path[0] == '/') {
        mem_error(error, error_size, "invalid relative path");
        return -1;
    }
    char *path_copy = strdup(relative_path);
    if (!path_copy) {
        mem_error(error, error_size, "out of memory copying relative path");
        return -1;
    }
    int current_fd = dup(root_fd);
    if (current_fd < 0) {
        mem_error(error, error_size, "cannot duplicate root directory handle: %s", strerror(errno));
        free(path_copy);
        return -1;
    }
    char *save = NULL;
    char *component = strtok_r(path_copy, "/", &save);
    int result_fd = -1;
    while (component) {
        if (!strcmp(component, ".") || !strcmp(component, "..") || component[0] == '\0') {
            mem_error(error, error_size, "invalid path component in indexed path");
            goto done;
        }
        char *next = strtok_r(NULL, "/", &save);
        int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
        if (next) flags |= O_DIRECTORY;
        int child_fd = openat(current_fd, component, flags);
        if (child_fd < 0) {
            mem_error(error, error_size, "cannot open indexed path '%s': %s", relative_path, strerror(errno));
            goto done;
        }
        close(current_fd);
        current_fd = child_fd;
        component = next;
    }
    if (fstat(current_fd, st) != 0 || !S_ISREG(st->st_mode)) {
        mem_error(error, error_size, "indexed path is no longer a regular file");
        goto done;
    }
    result_fd = current_fd;
    current_fd = -1;
done:
    if (current_fd >= 0) close(current_fd);
    free(path_copy);
    return result_fd;
}

static int read_and_hash_file(int fd, uint64_t max_file_bytes, uint64_t max_text_bytes,
                              ReadResult *result, char *error, size_t error_size) {
    memset(result, 0, sizeof(*result));
    if (max_text_bytes > SIZE_MAX - 1U) {
        mem_error(error, error_size, "max extracted text size is outside this platform's address range");
        return -1;
    }
    result->text = malloc((size_t)max_text_bytes + 1U);
    if (!result->text) {
        mem_error(error, error_size, "cannot allocate extracted text buffer");
        return -1;
    }
    EVP_MD_CTX *digest = EVP_MD_CTX_new();
    if (!digest || EVP_DigestInit_ex(digest, EVP_sha256(), NULL) != 1) {
        mem_error(error, error_size, "cannot initialise SHA-256 context");
        EVP_MD_CTX_free(digest);
        free(result->text); result->text = NULL;
        return -1;
    }
    Utf8State utf8 = {0};
    bool ok = true;
    unsigned char buffer[EXTRACT_READ_BUFFER];
    for (;;) {
        ssize_t got = read(fd, buffer, sizeof(buffer));
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) {
            mem_error(error, error_size, "file read failed: %s", strerror(errno));
            ok = false; break;
        }
        if (got == 0) break;
        if ((uint64_t)got > max_file_bytes - result->source_bytes) {
            mem_error(error, error_size, "file grew beyond configured maximum size while extracting");
            ok = false; break;
        }
        if (EVP_DigestUpdate(digest, buffer, (size_t)got) != 1) {
            mem_error(error, error_size, "SHA-256 update failed during extraction");
            ok = false; break;
        }
        (void)utf8_feed(&utf8, buffer, (size_t)got);
        uint64_t remain = max_text_bytes - (uint64_t)result->text_len;
        size_t copy_len = (uint64_t)got < remain ? (size_t)got : (size_t)remain;
        if (copy_len > 0) {
            memcpy(result->text + result->text_len, buffer, copy_len);
            result->text_len += copy_len;
        }
        if (copy_len < (size_t)got) result->truncated = true;
        result->source_bytes += (uint64_t)got;
    }
    if (utf8.remaining != 0) utf8.invalid = true;
    if (ok && hash_to_hex(digest, result->sha256) != 0) {
        mem_error(error, error_size, "SHA-256 finalization failed during extraction");
        ok = false;
    }
    EVP_MD_CTX_free(digest);
    if (!ok) {
        free(result->text); result->text = NULL;
        return -1;
    }
    result->text_len = complete_utf8_prefix((const unsigned char *)result->text, result->text_len);
    result->text[result->text_len] = '\0';
    result->valid_utf8_text = !utf8.invalid;
    return 0;
}

/* Binary document extractors need random access to the complete source, but are kept to a
   64 MiB input budget independently of the output-text limit. The caller verifies fstat/hash
   stability after this routine returns. */
static int read_and_hash_binary_file(int fd, size_t max_input_bytes, BinaryReadResult *result,
                                     char *error, size_t error_size) {
    memset(result, 0, sizeof(*result));
    size_t capacity = 4096U;
    if (capacity > max_input_bytes) capacity = max_input_bytes;
    if (capacity == 0U) {
        mem_error(error, error_size, "binary document input limit is zero");
        return -1;
    }
    result->data = malloc(capacity + 1U);
    if (!result->data) {
        mem_error(error, error_size, "cannot allocate binary document buffer");
        return -1;
    }
    EVP_MD_CTX *digest = EVP_MD_CTX_new();
    if (!digest || EVP_DigestInit_ex(digest, EVP_sha256(), NULL) != 1) {
        mem_error(error, error_size, "cannot initialise SHA-256 context for binary document");
        EVP_MD_CTX_free(digest);
        free(result->data); result->data = NULL;
        return -1;
    }
    unsigned char block[EXTRACT_READ_BUFFER];
    bool ok = true;
    for (;;) {
        ssize_t got = read(fd, block, sizeof(block));
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) {
            mem_error(error, error_size, "binary document read failed: %s", strerror(errno));
            ok = false; break;
        }
        if (got == 0) break;
        size_t count = (size_t)got;
        if (count > max_input_bytes - (size_t)result->source_bytes) {
            mem_error(error, error_size, "binary document exceeds built-in input limit of %zu bytes", max_input_bytes);
            ok = false; break;
        }
        if (EVP_DigestUpdate(digest, block, count) != 1) {
            mem_error(error, error_size, "SHA-256 update failed for binary document");
            ok = false; break;
        }
        size_t needed = (size_t)result->source_bytes + count;
        if (needed > capacity) {
            size_t next = capacity;
            while (next < needed) {
                if (next > max_input_bytes / 2U) { next = max_input_bytes; break; }
                next *= 2U;
            }
            if (next < needed || next > max_input_bytes) {
                mem_error(error, error_size, "binary document exceeds built-in input limit");
                ok = false; break;
            }
            unsigned char *grown = realloc(result->data, next + 1U);
            if (!grown) {
                mem_error(error, error_size, "cannot grow binary document buffer");
                ok = false; break;
            }
            result->data = grown;
            capacity = next;
        }
        memcpy(result->data + (size_t)result->source_bytes, block, count);
        result->source_bytes += (uint64_t)count;
    }
    if (ok && hash_to_hex(digest, result->sha256) != 0) {
        mem_error(error, error_size, "SHA-256 finalization failed for binary document");
        ok = false;
    }
    EVP_MD_CTX_free(digest);
    if (!ok) {
        free(result->data); result->data = NULL;
        result->length = 0;
        return -1;
    }
    result->length = (size_t)result->source_bytes;
    result->data[result->length] = 0U;
    return 0;
}

static bool is_pdf_file(const ExtractFile *file) {
    return file && ((file->extension && !strcasecmp(file->extension, "pdf")) ||
                    (file->mime_type && !strcasecmp(file->mime_type, "application/pdf")));
}

static bool is_ooxml_file(const ExtractFile *file) {
    if (!file) return false;
    if (file->extension && (!strcasecmp(file->extension, "docx") ||
                            !strcasecmp(file->extension, "xlsx") ||
                            !strcasecmp(file->extension, "pptx"))) return true;
    if (file->mime_type && (!strcasecmp(file->mime_type, "application/vnd.openxmlformats-officedocument.wordprocessingml.document") ||
                            !strcasecmp(file->mime_type, "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet") ||
                            !strcasecmp(file->mime_type, "application/vnd.openxmlformats-officedocument.presentationml.presentation"))) return true;
    return false;
}

static const char *extractor_for_file(const ExtractFile *file) {
    if (is_pdf_file(file)) return ASGEND_MEMORY_PDF_EXTRACTOR_VERSION;
    if (is_ooxml_file(file)) return ASGEND_MEMORY_OOXML_EXTRACTOR_VERSION;
    return EXTRACTOR_VERSION;
}

static bool is_md_extension(const char *extension) {
    return extension && (!strcasecmp(extension, "md") || !strcasecmp(extension, "markdown") ||
                         !strcasecmp(extension, "mdown") || !strcasecmp(extension, "mkd") ||
                         !strcasecmp(extension, "mkdn"));
}

static bool known_text_extension(const char *extension) {
    static const char *const extensions[] = {
        "txt", "text", "log", "rst", "adoc", "asciidoc", "md", "markdown", "mdown", "mkd", "mkdn",
        "c", "h", "cc", "hh", "cpp", "hpp", "hxx", "cxx", "rs", "go", "java", "kt", "kts", "swift",
        "js", "mjs", "cjs", "jsx", "ts", "tsx", "py", "pyi", "rb", "php", "pl", "pm", "sh", "bash",
        "zsh", "fish", "sql", "json", "jsonc", "jsonl", "yaml", "yml", "toml", "ini", "conf", "cfg",
        "xml", "html", "htm", "css", "scss", "less", "svg", "make", "mk", "cmake", "dockerfile", "gitignore",
        "env", "properties", "gradle", "proto", "graphql", "gql", "vue", "svelte", "r", "jl", "ex", "exs",
        "erl", "hrl", "hs", "lhs", "lua", "vim", "tf", "hcl", "lock", "csv", "tsv", "patch", "diff", NULL
    };
    for (size_t i = 0; extensions[i]; ++i) if (extension && !strcasecmp(extension, extensions[i])) return true;
    return false;
}

static char *strip_yaml_comment(const char *value) {
    size_t n = strlen(value);
    char quote = '\0';
    bool escaped = false;
    for (size_t i = 0; i < n; ++i) {
        char c = value[i];
        if (quote == '"' && c == '\\' && !escaped) { escaped = true; continue; }
        if ((c == '\'' || c == '"') && (!quote || quote == c) && !escaped) quote = quote ? '\0' : c;
        if (c == '#' && !quote && (i == 0 || isspace((unsigned char)value[i - 1]))) return duplicate_range(value, i);
        escaped = false;
    }
    return strdup(value);
}

static const char *json_type_label(enum json_type type) {
    switch (type) {
        case json_type_null: return "null";
        case json_type_boolean: return "boolean";
        case json_type_double: return "number";
        case json_type_int: return "number";
        case json_type_object: return "object";
        case json_type_array: return "array";
        case json_type_string: return "string";
        default: return "unknown";
    }
}

static json_object *yaml_value(const char *raw, bool *partial, unsigned int depth);

static bool find_yaml_separator(const char *text, size_t *position) {
    char quote = '\0';
    bool escaped = false;
    unsigned int nesting = 0;
    for (size_t i = 0; text[i]; ++i) {
        char c = text[i];
        if (quote == '"' && c == '\\' && !escaped) { escaped = true; continue; }
        if ((c == '\'' || c == '"') && (!quote || quote == c) && !escaped) quote = quote ? '\0' : c;
        if (!quote) {
            if (c == '[' || c == '{') ++nesting;
            else if ((c == ']' || c == '}') && nesting > 0) --nesting;
            else if (c == ':' && nesting == 0 && (text[i + 1] == '\0' || isspace((unsigned char)text[i + 1]))) {
                *position = i;
                return true;
            }
        }
        escaped = false;
    }
    return false;
}

static json_object *yaml_quoted_value(const char *value, char quote, bool *partial) {
    size_t len = strlen(value);
    if (len < 2 || value[len - 1] != quote) {
        *partial = true;
        return json_object_new_string(value);
    }
    if (quote == '"') {
        json_tokener *tokener = json_tokener_new();
        if (tokener) {
            json_object *parsed = json_tokener_parse_ex(tokener, value, (int)len);
            enum json_tokener_error err = json_tokener_get_error(tokener);
            json_tokener_free(tokener);
            if (err == json_tokener_success && parsed && json_object_is_type(parsed, json_type_string)) return parsed;
            if (parsed) json_object_put(parsed);
        }
        *partial = true;
        return json_object_new_string_len(value + 1, (int)(len - 2));
    }
    char *copy = duplicate_range(value + 1, len - 2);
    if (!copy) return NULL;
    size_t out = 0;
    for (size_t i = 0; copy[i]; ++i) {
        copy[out++] = copy[i];
        if (copy[i] == '\'' && copy[i + 1] == '\'') ++i;
    }
    copy[out] = '\0';
    json_object *obj = json_object_new_string(copy);
    free(copy);
    return obj;
}

static json_object *yaml_value(const char *raw, bool *partial, unsigned int depth) {
    if (depth > 16) { *partial = true; return json_object_new_string(raw ? raw : ""); }
    char *copy = strip_yaml_comment(raw ? raw : "");
    if (!copy) return NULL;
    char *value = trim_in_place(copy);
    size_t len = strlen(value);
    json_object *result = NULL;
    if (len == 0 || !strcmp(value, "~") || !strcasecmp(value, "null") || !strcasecmp(value, "Null") || !strcasecmp(value, "NULL")) {
        result = json_object_new_null();
    } else if (!strcasecmp(value, "true") || !strcasecmp(value, "false")) {
        result = json_object_new_boolean(!strcasecmp(value, "true"));
    } else if ((value[0] == '"' || value[0] == '\'') && len >= 2) {
        result = yaml_quoted_value(value, value[0], partial);
    } else if (value[0] == '[' && len >= 2 && value[len - 1] == ']') {
        result = json_object_new_array();
        size_t start = 1;
        char quote = '\0'; bool escaped = false; unsigned int nesting = 0;
        for (size_t i = 1; i < len; ++i) {
            char c = value[i];
            if (quote == '"' && c == '\\' && !escaped) { escaped = true; continue; }
            if ((c == '\'' || c == '"') && (!quote || quote == c) && !escaped) quote = quote ? '\0' : c;
            if (!quote) {
                if (c == '[' || c == '{') ++nesting;
                else if ((c == ']' || c == '}') && nesting > 0) --nesting;
                else if (c == ',' && nesting == 0) {
                    char *item = duplicate_range(value + start, i - start);
                    if (!item) { json_object_put(result); result = NULL; break; }
                    char *trimmed = trim_in_place(item);
                    if (*trimmed) json_object_array_add(result, yaml_value(trimmed, partial, depth + 1));
                    free(item); start = i + 1;
                }
            }
            escaped = false;
        }
        if (result) {
            char *item = duplicate_range(value + start, len - start - 1);
            if (!item) { json_object_put(result); result = NULL; }
            else {
                char *trimmed = trim_in_place(item);
                if (*trimmed) json_object_array_add(result, yaml_value(trimmed, partial, depth + 1));
                free(item);
            }
        }
    } else if (!strcmp(value, "|") || !strcmp(value, ">") || !strcmp(value, "|-") || !strcmp(value, ">-")) {
        *partial = true;
        result = json_object_new_string(value);
    } else {
        errno = 0;
        char *end = NULL;
        long long int_value = strtoll(value, &end, 10);
        if (errno == 0 && end && *end == '\0' && end != value) result = json_object_new_int64((int64_t)int_value);
        else {
            errno = 0; end = NULL;
            double double_value = strtod(value, &end);
            if (errno == 0 && end && *end == '\0' && end != value && strpbrk(value, ".eE")) result = json_object_new_double(double_value);
            else result = json_object_new_string(value);
        }
    }
    free(copy);
    return result;
}

static bool line_is_sequence_item(const char *text) {
    return text[0] == '-' && (text[1] == '\0' || isspace((unsigned char)text[1]));
}

static json_object *parse_yaml_block(YamlLine *lines, size_t line_count, size_t *index,
                                     size_t indent, bool *partial, unsigned int depth);

static json_object *yaml_mapping_value(YamlLine *lines, size_t line_count, size_t *index,
                                       size_t parent_indent, const char *inline_value,
                                       bool *partial, unsigned int depth) {
    char *value = strdup(inline_value ? inline_value : "");
    if (!value) return NULL;
    char *trimmed = trim_in_place(value);
    json_object *result;
    if (*trimmed) result = yaml_value(trimmed, partial, depth + 1);
    else if (*index < line_count && lines[*index].indent > parent_indent) {
        result = parse_yaml_block(lines, line_count, index, lines[*index].indent, partial, depth + 1);
    } else result = json_object_new_null();
    free(value);
    return result;
}

static json_object *parse_mapping_line(char *text, YamlLine *lines, size_t line_count,
                                       size_t *index, size_t parent_indent,
                                       bool *partial, unsigned int depth) {
    size_t separator = 0;
    if (!find_yaml_separator(text, &separator)) {
        *partial = true;
        return NULL;
    }
    char *key = duplicate_range(text, separator);
    if (!key) return NULL;
    char *key_trim = trim_in_place(key);
    if ((*key_trim == '\'' || *key_trim == '"') && strlen(key_trim) >= 2) {
        char quote = *key_trim;
        size_t klen = strlen(key_trim);
        if (key_trim[klen - 1] == quote) {
            memmove(key_trim, key_trim + 1, klen - 2);
            key_trim[klen - 2] = '\0';
        }
    }
    if (!key_trim[0] || strlen(key_trim) > 128) {
        free(key);
        *partial = true;
        return NULL;
    }
    for (const unsigned char *it = (const unsigned char *)key_trim; *it; ++it) {
        if (!(isalnum(*it) || *it == '_' || *it == '-' || *it == '.' || *it >= 0x80)) {
            *partial = true;
            free(key);
            return NULL;
        }
    }
    char *value_copy = strdup(text + separator + 1);
    if (!value_copy) { free(key); return NULL; }
    json_object *value = yaml_mapping_value(lines, line_count, index, parent_indent, value_copy, partial, depth);
    free(value_copy);
    /* Return {key:value}; caller merges it into its containing mapping. */
    json_object *pair = json_object_new_object();
    if (!pair) { free(key); if (value) json_object_put(value); return NULL; }
    json_object_object_add(pair, key_trim, value ? value : json_object_new_null());
    free(key);
    return pair;
}

static void merge_json_object(json_object *destination, json_object *source) {
    if (!destination || !source || !json_object_is_type(destination, json_type_object) ||
        !json_object_is_type(source, json_type_object)) return;
    json_object_object_foreach(source, key, value) {
        json_object_get(value);
        json_object_object_add(destination, key, value);
    }
}

static json_object *parse_yaml_block(YamlLine *lines, size_t line_count, size_t *index,
                                     size_t indent, bool *partial, unsigned int depth) {
    if (depth > 16) {
        *partial = true;
        if (*index < line_count) ++*index;
        return json_object_new_object();
    }
    if (*index >= line_count) return json_object_new_object();
    while (*index < line_count && !lines[*index].text[0]) ++*index;
    if (*index >= line_count) return json_object_new_object();
    bool sequence = lines[*index].indent == indent && line_is_sequence_item(lines[*index].text);
    json_object *container = sequence ? json_object_new_array() : json_object_new_object();
    if (!container) return NULL;
    while (*index < line_count) {
        if (!lines[*index].text[0]) { ++*index; continue; }
        if (lines[*index].indent < indent) break;
        if (lines[*index].indent > indent) {
            *partial = true;
            ++*index;
            continue;
        }
        char *text = lines[*index].text;
        if (sequence) {
            if (!line_is_sequence_item(text)) break;
            char *item_text = trim_in_place(text + 1);
            ++*index;
            if (!*item_text) {
                json_object *item = NULL;
                if (*index < line_count && lines[*index].indent > indent)
                    item = parse_yaml_block(lines, line_count, index, lines[*index].indent, partial, depth + 1);
                else item = json_object_new_null();
                json_object_array_add(container, item ? item : json_object_new_null());
                continue;
            }
            size_t separator = 0;
            if (find_yaml_separator(item_text, &separator)) {
                json_object *item_obj = json_object_new_object();
                if (!item_obj) { json_object_put(container); return NULL; }
                json_object *pair = parse_mapping_line(item_text, lines, line_count, index, indent, partial, depth + 1);
                if (pair) { merge_json_object(item_obj, pair); json_object_put(pair); }
                while (*index < line_count && lines[*index].text[0] && lines[*index].indent > indent) {
                    size_t child_indent = lines[*index].indent;
                    json_object *continuation = parse_yaml_block(lines, line_count, index, child_indent, partial, depth + 1);
                    if (continuation && json_object_is_type(continuation, json_type_object)) merge_json_object(item_obj, continuation);
                    else if (continuation) *partial = true;
                    if (continuation) json_object_put(continuation);
                }
                json_object_array_add(container, item_obj);
            } else {
                json_object_array_add(container, yaml_value(item_text, partial, depth + 1));
                /* Any indented children on a scalar sequence item are unsupported YAML. */
                while (*index < line_count && lines[*index].text[0] && lines[*index].indent > indent) {
                    *partial = true;
                    ++*index;
                }
            }
        } else {
            if (line_is_sequence_item(text)) break;
            size_t separator = 0;
            if (!find_yaml_separator(text, &separator)) { *partial = true; ++*index; continue; }
            ++*index;
            json_object *pair = parse_mapping_line(text, lines, line_count, index, indent, partial, depth + 1);
            if (pair) { merge_json_object(container, pair); json_object_put(pair); }
        }
    }
    return container;
}

static json_object *parse_yaml_frontmatter(const char *raw, size_t length, bool *partial) {
    json_object *empty = json_object_new_object();
    if (!empty) return NULL;
    YamlLine *lines = NULL;
    size_t count = 0, capacity = 0, offset = 0;
    while (offset < length) {
        size_t end = offset;
        while (end < length && raw[end] != '\n') ++end;
        size_t line_len = end - offset;
        if (line_len && raw[offset + line_len - 1] == '\r') --line_len;
        char *line = duplicate_range(raw + offset, line_len);
        if (!line) goto fail;
        char *trimmed = trim_in_place(line);
        if (*trimmed == '#') *trimmed = '\0';
        size_t indent = 0;
        while (line[indent] == ' ' || line[indent] == '\t') {
            if (line[indent] == '\t') *partial = true;
            ++indent;
        }
        char *content = line + indent;
        char *comment_removed = strip_yaml_comment(content);
        if (!comment_removed) { free(line); goto fail; }
        char *effective = trim_in_place(comment_removed);
        char *stored = strdup(effective);
        free(comment_removed); free(line);
        if (!stored) goto fail;
        if (count == capacity) {
            size_t next = capacity ? capacity * 2U : 16U;
            if (next > SIZE_MAX / sizeof(*lines)) { free(stored); goto fail; }
            YamlLine *grown = realloc(lines, next * sizeof(*lines));
            if (!grown) { free(stored); goto fail; }
            lines = grown; capacity = next;
        }
        lines[count].text = stored;
        lines[count].indent = indent;
        ++count;
        offset = end < length ? end + 1U : end;
    }
    if (count > 0) {
        size_t index = 0;
        json_object *parsed = parse_yaml_block(lines, count, &index, lines[0].indent, partial, 0);
        if (parsed && json_object_is_type(parsed, json_type_object)) {
            json_object_put(empty);
            empty = parsed;
        } else {
            if (parsed) json_object_put(parsed);
            *partial = true;
        }
    }
    for (size_t i = 0; i < count; ++i) free(lines[i].text);
    free(lines);
    return empty;
fail:
    if (lines) { for (size_t i = 0; i < count; ++i) free(lines[i].text); free(lines); }
    json_object_put(empty);
    return NULL;
}

static bool markdown_line_equals(const char *text, size_t length, const char *wanted) {
    while (length && (text[length - 1] == '\r' || text[length - 1] == ' ' || text[length - 1] == '\t')) --length;
    while (length && (*text == ' ' || *text == '\t')) { ++text; --length; }
    return strlen(wanted) == length && memcmp(text, wanted, length) == 0;
}

typedef struct {
    char *body;
    size_t body_len;
    size_t body_start_byte;
    uint64_t body_start_line;
    char *raw_frontmatter;
    json_object *metadata;
    const char *metadata_status;
} MarkdownParts;

static int split_markdown_frontmatter(const char *text, size_t length, bool input_truncated,
                                      MarkdownParts *parts) {
    memset(parts, 0, sizeof(*parts));
    size_t offset = 0;
    if (length >= 3 && (unsigned char)text[0] == 0xef &&
        (unsigned char)text[1] == 0xbb && (unsigned char)text[2] == 0xbf) offset = 3;
    size_t first_end = offset;
    while (first_end < length && text[first_end] != '\n') ++first_end;
    if (!markdown_line_equals(text + offset, first_end - offset, "---")) {
        parts->body = duplicate_range(text, length);
        parts->body_len = length;
        parts->body_start_byte = 0;
        parts->body_start_line = 1;
        parts->metadata_status = "none";
        return parts->body ? 0 : -1;
    }
    size_t yaml_start = first_end < length ? first_end + 1U : first_end;
    size_t line_start = yaml_start;
    size_t close_start = SIZE_MAX, body_start = SIZE_MAX;
    while (line_start <= length) {
        size_t line_end = line_start;
        while (line_end < length && text[line_end] != '\n') ++line_end;
        if (markdown_line_equals(text + line_start, line_end - line_start, "---") ||
            markdown_line_equals(text + line_start, line_end - line_start, "...")) {
            close_start = line_start;
            body_start = line_end < length ? line_end + 1U : line_end;
            break;
        }
        if (line_end == length) break;
        line_start = line_end + 1U;
    }
    if (close_start == SIZE_MAX) {
        parts->body = duplicate_range(text, length);
        parts->raw_frontmatter = duplicate_range(text + yaml_start, length - yaml_start);
        parts->body_len = length;
        parts->body_start_byte = 0;
        parts->body_start_line = 1;
        parts->metadata_status = input_truncated ? "truncated_header" : "invalid_frontmatter";
        return parts->body && parts->raw_frontmatter ? 0 : -1;
    }
    size_t yaml_end = close_start;
    if (yaml_end > yaml_start && text[yaml_end - 1] == '\n') --yaml_end;
    if (yaml_end > yaml_start && text[yaml_end - 1] == '\r') --yaml_end;
    parts->raw_frontmatter = duplicate_range(text + yaml_start, yaml_end - yaml_start);
    parts->body = duplicate_range(text + body_start, length - body_start);
    parts->body_len = length - body_start;
    parts->body_start_byte = body_start;
    parts->body_start_line = 1;
    for (size_t line_i = 0; line_i < body_start; ++line_i) if (text[line_i] == '\n') parts->body_start_line++;
    if (!parts->raw_frontmatter || !parts->body) return -1;
    bool partial = false;
    parts->metadata = parse_yaml_frontmatter(parts->raw_frontmatter, strlen(parts->raw_frontmatter), &partial);
    if (!parts->metadata) return -1;
    parts->metadata_status = partial ? "parsed_partial" : "parsed";
    return 0;
}

static size_t utf8_char_count(const char *text, size_t len) {
    size_t count = 0;
    for (size_t i = 0; i < len; ++i) if (((unsigned char)text[i] & 0xc0U) != 0x80U) ++count;
    return count;
}

static const char *extract_status_text(json_object *value) {
    if (!value) return "unknown";
    return json_type_label(json_object_get_type(value));
}

static int persist_result(sqlite3 *db, int64_t file_id, const char *expected_hash,
                          const char *status, const char *metadata_status,
                          const char *content, size_t content_len,
                          const char *frontmatter_raw, const char *error_text,
                          uint64_t source_bytes, bool truncated,
                          uint64_t body_start_byte, uint64_t body_start_line,
                          json_object *metadata, const char *extractor_id,
                          char *error, size_t error_size) {
    if (asgend_memory_internal_exec_sql(db, "BEGIN IMMEDIATE", error, error_size) != 0) return -1;
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO file_contents(file_id,source_sha256,content_text,frontmatter_raw,extractor_id,extract_status,metadata_status,source_bytes,extracted_bytes,extracted_chars,error_text,extracted_at,body_start_byte,body_start_line) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,strftime('%Y-%m-%dT%H:%M:%fZ','now'),?12,?13) "
        "ON CONFLICT(file_id) DO UPDATE SET source_sha256=excluded.source_sha256,content_text=excluded.content_text,frontmatter_raw=excluded.frontmatter_raw,extractor_id=excluded.extractor_id,extract_status=excluded.extract_status,metadata_status=excluded.metadata_status,source_bytes=excluded.source_bytes,extracted_bytes=excluded.extracted_bytes,extracted_chars=excluded.extracted_chars,error_text=excluded.error_text,extracted_at=excluded.extracted_at,body_start_byte=excluded.body_start_byte,body_start_line=excluded.body_start_line";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) goto db_fail;
    sqlite3_bind_int64(stmt, 1, file_id);
    sqlite3_bind_text(stmt, 2, expected_hash ? expected_hash : "", -1, SQLITE_TRANSIENT);
    if (content && content_len > 0) sqlite3_bind_text(stmt, 3, content, (int)content_len, SQLITE_TRANSIENT);
    else if (content) sqlite3_bind_text(stmt, 3, "", 0, SQLITE_STATIC);
    else sqlite3_bind_null(stmt, 3);
    if (frontmatter_raw) sqlite3_bind_text(stmt, 4, frontmatter_raw, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(stmt, 4);
    sqlite3_bind_text(stmt, 5, extractor_id ? extractor_id : EXTRACTOR_VERSION, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, status, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 7, metadata_status ? metadata_status : "not_applicable", -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 8, (sqlite3_int64)source_bytes);
    sqlite3_bind_int64(stmt, 9, content ? (sqlite3_int64)content_len : 0);
    sqlite3_bind_int64(stmt, 10, content ? (sqlite3_int64)utf8_char_count(content, content_len) : 0);
    if (error_text) sqlite3_bind_text(stmt, 11, error_text, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(stmt, 11);
    sqlite3_bind_int64(stmt, 12, (sqlite3_int64)body_start_byte);
    sqlite3_bind_int64(stmt, 13, (sqlite3_int64)(body_start_line ? body_start_line : 1));
    if (sqlite3_step(stmt) != SQLITE_DONE) { sqlite3_finalize(stmt); stmt = NULL; goto db_fail; }
    sqlite3_finalize(stmt); stmt = NULL;

    if (sqlite3_prepare_v2(db, "DELETE FROM file_metadata WHERE file_id=?1", -1, &stmt, NULL) != SQLITE_OK) goto db_fail;
    sqlite3_bind_int64(stmt, 1, file_id);
    if (sqlite3_step(stmt) != SQLITE_DONE) { sqlite3_finalize(stmt); stmt = NULL; goto db_fail; }
    sqlite3_finalize(stmt); stmt = NULL;

    if (metadata && json_object_is_type(metadata, json_type_object)) {
        const char *insert_sql = "INSERT INTO file_metadata(file_id,metadata_key,value_json,value_type,source,source_sha256) VALUES(?1,?2,?3,?4,'okf_frontmatter',?5)";
        if (sqlite3_prepare_v2(db, insert_sql, -1, &stmt, NULL) != SQLITE_OK) goto db_fail;
        json_object_object_foreach(metadata, key, value) {
            const char *json = json_object_to_json_string_ext(value, JSON_C_TO_STRING_PLAIN);
            sqlite3_reset(stmt); sqlite3_clear_bindings(stmt);
            sqlite3_bind_int64(stmt, 1, file_id);
            sqlite3_bind_text(stmt, 2, key, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, json ? json : "null", -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, extract_status_text(value), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, expected_hash ? expected_hash : "", -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) != SQLITE_DONE) { sqlite3_finalize(stmt); stmt = NULL; goto db_fail; }
        }
        sqlite3_finalize(stmt); stmt = NULL;
    }
    if (asgend_memory_internal_exec_sql(db, "COMMIT", error, error_size) != 0) goto rollback_after_commit_error;
    (void)truncated;
    return 0;

db_fail:
    asgend_memory_internal_sqlite_error(db, error, error_size, "persist extracted content/metadata");
    if (stmt) sqlite3_finalize(stmt);
    (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    return -1;
rollback_after_commit_error:
    (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    return -1;
}

static bool should_skip(const ExtractFile *file, bool retry_failed) {
    if (!file->previous_status || !file->previous_sha256 || !file->previous_extractor_id) return false;
    if (strcmp(file->previous_sha256, file->sha256 ? file->sha256 : "") != 0 ||
        strcmp(file->previous_extractor_id, extractor_for_file(file)) != 0) return false;
    if (!strcmp(file->previous_status, "extracted") ||
        !strcmp(file->previous_status, "unsupported") ||
        !strcmp(file->previous_status, "binary")) return true;
    if (!strcmp(file->previous_status, "truncated") ||
        !strcmp(file->previous_status, "parsed_partial") ||
        !strcmp(file->previous_status, "too_large") ||
        !strcmp(file->previous_status, "read_error") ||
        !strcmp(file->previous_status, "stale_source")) return !retry_failed;
    return false;
}

static void free_extract_file(ExtractFile *file) {
    free(file->relative_path);
    free(file->extension);
    free(file->mime_type);
    free(file->sha256);
    free(file->previous_sha256);
    free(file->previous_status);
    free(file->previous_extractor_id);
    memset(file, 0, sizeof(*file));
}

static int load_extract_batch(sqlite3 *db, int64_t after_id, ExtractFile files[EXTRACT_BATCH_SIZE],
                              size_t *count, char *error, size_t error_size) {
    *count = 0;
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT f.file_id,f.relative_path,f.extension,f.mime_type,f.file_size,f.mtime_ns,COALESCE(f.sha256,''),c.source_sha256,c.extract_status,c.extractor_id "
        "FROM files f LEFT JOIN file_contents c ON c.file_id=f.file_id "
        "WHERE f.deleted_at IS NULL AND f.file_id>?1 ORDER BY f.file_id LIMIT 64";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return asgend_memory_internal_sqlite_error(db, error, error_size, "prepare extraction batch");
    sqlite3_bind_int64(stmt, 1, after_id);
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        ExtractFile *file = &files[*count];
        memset(file, 0, sizeof(*file));
        file->file_id = sqlite3_column_int64(stmt, 0);
        file->relative_path = strdup((const char *)sqlite3_column_text(stmt, 1));
        file->extension = strdup((const char *)sqlite3_column_text(stmt, 2));
        file->mime_type = strdup((const char *)sqlite3_column_text(stmt, 3));
        file->file_size = sqlite3_column_int64(stmt, 4);
        file->mtime_ns = sqlite3_column_int64(stmt, 5);
        file->sha256 = strdup((const char *)sqlite3_column_text(stmt, 6));
        const unsigned char *previous_hash = sqlite3_column_text(stmt, 7);
        const unsigned char *previous_status = sqlite3_column_text(stmt, 8);
        if (previous_hash) file->previous_sha256 = strdup((const char *)previous_hash);
        if (previous_status) file->previous_status = strdup((const char *)previous_status);
        const unsigned char *previous_extractor = sqlite3_column_text(stmt, 9);
        if (previous_extractor) file->previous_extractor_id = strdup((const char *)previous_extractor);
        if (!file->relative_path || !file->extension || !file->mime_type || !file->sha256 ||
            (previous_hash && !file->previous_sha256) || (previous_status && !file->previous_status) ||
            (previous_extractor && !file->previous_extractor_id)) {
            sqlite3_finalize(stmt);
            for (size_t i = 0; i <= *count; ++i) free_extract_file(&files[i]);
            *count = 0;
            mem_error(error, error_size, "out of memory loading extraction batch");
            return -1;
        }
        ++*count;
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return asgend_memory_internal_sqlite_error(db, error, error_size, "read extraction batch");
    return 0;
}

static int persist_simple_status_with_extractor(sqlite3 *db, int64_t file_id, const char *hash,
                                                const char *status, const char *metadata_status,
                                                const char *message, uint64_t source_bytes,
                                                const char *extractor_id,
                                                char *error, size_t error_size) {
    return persist_result(db, file_id, hash, status, metadata_status, NULL, 0,
                          NULL, message, source_bytes, false, 0, 1, NULL,
                          extractor_id, error, error_size);
}

static int persist_simple_status(sqlite3 *db, int64_t file_id, const char *hash,
                                 const char *status, const char *metadata_status,
                                 const char *message, uint64_t source_bytes,
                                 char *error, size_t error_size) {
    return persist_simple_status_with_extractor(db, file_id, hash, status, metadata_status,
                                                message, source_bytes, EXTRACTOR_VERSION,
                                                error, error_size);
}

static int process_extract_file(sqlite3 *db, int root_fd, const ExtractFile *file,
                                uint64_t max_file_size, uint64_t max_text_size,
                                bool retry_failed, AsgendMemoryExtractResult *result,
                                char *error, size_t error_size) {
    result->files_seen++;
    if (should_skip(file, retry_failed)) { result->skipped++; return 0; }
    if (!file->sha256 || !file->sha256[0]) {
        if (persist_simple_status(db, file->file_id, "", "too_large", "not_applicable",
                                  "scan did not produce a hash (file is too large or unreadable)",
                                  file->file_size > 0 ? (uint64_t)file->file_size : 0, error, error_size) != 0) return -1;
        result->unsupported++;
        return 0;
    }
    if (file->file_size < 0 || (uint64_t)file->file_size > max_file_size) {
        if (persist_simple_status(db, file->file_id, file->sha256, "too_large", "not_applicable",
                                  "file exceeds configured extraction size limit", (uint64_t)(file->file_size > 0 ? file->file_size : 0), error, error_size) != 0) return -1;
        result->unsupported++;
        return 0;
    }
    if (is_known_binary_mime(file->mime_type) && !is_pdf_file(file) && !is_ooxml_file(file)) {
        if (persist_simple_status(db, file->file_id, file->sha256, "unsupported", "not_applicable",
                                  "no built-in extractor for this binary format", (uint64_t)file->file_size, error, error_size) != 0) return -1;
        result->unsupported++;
        return 0;
    }
    if ((is_pdf_file(file) || is_ooxml_file(file)) && (uint64_t)file->file_size > ASGEND_MEMORY_DOC_MAX_INPUT) {
        const char *doc_extractor = extractor_for_file(file);
        if (persist_simple_status_with_extractor(db, file->file_id, file->sha256, "too_large", "not_applicable",
                is_pdf_file(file) ? "PDF exceeds built-in 64 MiB random-access input limit" : "OOXML exceeds built-in 64 MiB random-access input limit",
                (uint64_t)file->file_size, doc_extractor, error, error_size) != 0) return -1;
        result->unsupported++;
        return 0;
    }

    struct stat before;
    char open_error[ASGEND_MEMORY_ERROR_MAX] = {0};
    int fd = open_relative_regular(root_fd, file->relative_path, &before, open_error, sizeof(open_error));
    if (fd < 0) {
        if (persist_simple_status(db, file->file_id, file->sha256, "read_error", "not_applicable",
                                  open_error[0] ? open_error : "could not open indexed file", (uint64_t)file->file_size, error, error_size) != 0) return -1;
        result->file_errors++;
        return 0;
    }
    if (before.st_size != file->file_size || mtime_ns(&before) != file->mtime_ns) {
        close(fd);
        if (persist_simple_status(db, file->file_id, file->sha256, "stale_source", "not_applicable",
                                  "file metadata changed since scan; run scan again before extract", (uint64_t)(before.st_size > 0 ? before.st_size : 0), error, error_size) != 0) return -1;
        result->stale_sources++;
        return 0;
    }
    if (is_pdf_file(file) || is_ooxml_file(file)) {
        bool is_pdf = is_pdf_file(file);
        const char *doc_extractor = is_pdf ? ASGEND_MEMORY_PDF_EXTRACTOR_VERSION : ASGEND_MEMORY_OOXML_EXTRACTOR_VERSION;
        const char *format_name = is_pdf ? "PDF" : "OOXML";
        BinaryReadResult binary;
        char read_error[ASGEND_MEMORY_ERROR_MAX] = {0};
        int read_rc = read_and_hash_binary_file(fd, (size_t)ASGEND_MEMORY_DOC_MAX_INPUT,
                                                &binary, read_error, sizeof(read_error));
        struct stat after;
        bool stable_after_read = fstat(fd, &after) == 0 && before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
                                 before.st_size == after.st_size && mtime_ns(&before) == mtime_ns(&after);
        close(fd);
        if (read_rc != 0) {
            if (persist_simple_status_with_extractor(db, file->file_id, file->sha256, "read_error", "not_applicable",
                    read_error[0] ? read_error : "binary document read failed", (uint64_t)file->file_size,
                    doc_extractor, error, error_size) != 0) return -1;
            result->file_errors++;
            return 0;
        }
        if (!stable_after_read || strcmp(binary.sha256, file->sha256) != 0) {
            free(binary.data);
            if (persist_simple_status_with_extractor(db, file->file_id, file->sha256, "stale_source", "not_applicable",
                    "binary document changed during extraction; run scan again", (uint64_t)file->file_size,
                    doc_extractor, error, error_size) != 0) return -1;
            result->stale_sources++;
            return 0;
        }
        AsgendMemoryDocText document;
        int parse_rc = is_pdf
            ? asgend_memory_internal_extract_pdf(binary.data, binary.length, (size_t)max_text_size, &document)
            : asgend_memory_internal_extract_ooxml(binary.data, binary.length, (size_t)max_text_size, &document);
        free(binary.data);
        if (parse_rc != 0) {
            char message[160];
            (void)snprintf(message, sizeof(message), "out of memory extracting %s text", format_name);
            if (persist_simple_status_with_extractor(db, file->file_id, file->sha256, "read_error", "not_applicable",
                    message, (uint64_t)file->file_size, doc_extractor, error, error_size) != 0) return -1;
            result->file_errors++;
            return 0;
        }
        const char *doc_status = document.unsupported ? "unsupported" :
                                 document.truncated ? "truncated" :
                                 document.partial ? "parsed_partial" : "extracted";
        const char *doc_metadata_status = document.partial ? "parsed_partial" : "not_applicable";
        const char *doc_content = document.unsupported ? NULL : (document.text ? document.text : "");
        if (persist_result(db, file->file_id, file->sha256, doc_status, doc_metadata_status,
                           doc_content, document.unsupported ? 0U : document.text_len,
                           NULL, document.reason[0] ? document.reason : NULL,
                           (uint64_t)file->file_size, document.truncated, 0, 1, NULL,
                           doc_extractor, error, error_size) != 0) {
            free(document.text);
            return -1;
        }
        free(document.text);
        if (document.unsupported) result->unsupported++;
        else if (document.truncated) result->truncated++;
        else result->extracted++;
        return 0;
    }

    ReadResult read_result;
    char read_error[ASGEND_MEMORY_ERROR_MAX] = {0};
    int read_rc = read_and_hash_file(fd, max_file_size, max_text_size, &read_result, read_error, sizeof(read_error));
    struct stat after;
    bool stable_after_read = fstat(fd, &after) == 0 && before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
                             before.st_size == after.st_size && mtime_ns(&before) == mtime_ns(&after);
    close(fd);
    if (read_rc != 0) {
        if (persist_simple_status(db, file->file_id, file->sha256, "read_error", "not_applicable",
                                  read_error[0] ? read_error : "read failed", (uint64_t)file->file_size, error, error_size) != 0) return -1;
        result->file_errors++;
        return 0;
    }
    if (!stable_after_read || strcmp(read_result.sha256, file->sha256) != 0) {
        free(read_result.text);
        if (persist_simple_status(db, file->file_id, file->sha256, "stale_source", "not_applicable",
                                  "file content changed since scan; run scan again before extract", (uint64_t)file->file_size, error, error_size) != 0) return -1;
        result->stale_sources++;
        return 0;
    }

    if (!read_result.valid_utf8_text) {
        free(read_result.text);
        if (persist_simple_status(db, file->file_id, file->sha256, "binary", "not_applicable",
                                  "file contains NUL, invalid UTF-8, or binary control bytes", read_result.source_bytes, error, error_size) != 0) return -1;
        result->unsupported++;
        return 0;
    }
    if (!is_textual_mime(file->mime_type) && !known_text_extension(file->extension)) {
        /* Unknown extensions are accepted only when the byte stream passed the strict UTF-8/text checks. */
    }

    char *body = read_result.text;
    size_t body_len = read_result.text_len;
    char *frontmatter = NULL;
    json_object *metadata = NULL;
    const char *metadata_status = is_md_extension(file->extension) ? "none" : "not_applicable";
    uint64_t body_start_byte = 0, body_start_line = 1;
    if (is_md_extension(file->extension)) {
        MarkdownParts parts;
        if (split_markdown_frontmatter(read_result.text, read_result.text_len, read_result.truncated, &parts) != 0) {
            free(read_result.text);
            if (persist_simple_status(db, file->file_id, file->sha256, "read_error", "invalid_frontmatter", "out of memory parsing Markdown frontmatter", read_result.source_bytes, error, error_size) != 0) return -1;
            result->file_errors++;
            return 0;
        }
        free(read_result.text);
        read_result.text = NULL;
        body = parts.body;
        body_len = parts.body_len;
        frontmatter = parts.raw_frontmatter;
        metadata = parts.metadata;
        metadata_status = parts.metadata_status;
        body_start_byte = parts.body_start_byte;
        body_start_line = parts.body_start_line ? parts.body_start_line : 1;
        /* Ownership of body/frontmatter/metadata moves to the persistence call. */
    }
    const char *status = read_result.truncated ? "truncated" : "extracted";
    if (persist_result(db, file->file_id, file->sha256, status, metadata_status,
                       body, body_len, frontmatter, NULL, read_result.source_bytes,
                       read_result.truncated, body_start_byte, body_start_line, metadata,
                       EXTRACTOR_VERSION, error, error_size) != 0) {
        free(body); free(frontmatter); if (metadata) json_object_put(metadata); return -1;
    }
    free(body);
    free(frontmatter);
    if (metadata) json_object_put(metadata);
    if (read_result.truncated) result->truncated++;
    else result->extracted++;
    return 0;
}

int asgend_memory_extract(const char *root, const AsgendMemoryExtractOptions *options,
                          AsgendMemoryExtractResult *result,
                          char *error, size_t error_size) {
    if (error && error_size) error[0] = '\0';
    if (result) memset(result, 0, sizeof(*result));
    char *canonical = NULL, *db_path = NULL;
    sqlite3 *db = NULL;
    if (asgend_memory_internal_open_index(root, &canonical, &db, &db_path, error, error_size) != 0) return -1;
    uint64_t max_file_size = options && options->max_file_size_bytes ? options->max_file_size_bytes : ASGEND_MEMORY_DEFAULT_MAX_FILE_SIZE;
    uint64_t max_text_size = options && options->max_text_bytes ? options->max_text_bytes : ASGEND_MEMORY_DEFAULT_MAX_TEXT_SIZE;
    bool retry_failed = options && options->retry_failed;
    if (max_file_size > (uint64_t)INT64_MAX || max_text_size > EXTRACT_MAX_TEXT_LIMIT) {
        mem_error(error, error_size, "extract limits exceed supported bounds (file <= INT64_MAX, text <= 64 MiB)");
        sqlite3_close(db); free(db_path); free(canonical); return -1;
    }
    int root_fd = open(canonical, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root_fd < 0) {
        mem_error(error, error_size, "cannot open extraction root: %s", strerror(errno));
        sqlite3_close(db); free(db_path); free(canonical); return -1;
    }
    AsgendMemoryExtractResult local_result;
    memset(&local_result, 0, sizeof(local_result));
    local_result.complete = true;
    int64_t after_id = 0;
    for (;;) {
        ExtractFile batch[EXTRACT_BATCH_SIZE];
        memset(batch, 0, sizeof(batch));
        size_t count = 0;
        if (load_extract_batch(db, after_id, batch, &count, error, error_size) != 0) goto fatal;
        if (count == 0) break;
        for (size_t i = 0; i < count; ++i) {
            after_id = batch[i].file_id;
            if (process_extract_file(db, root_fd, &batch[i], max_file_size, max_text_size,
                                     retry_failed, &local_result, error, error_size) != 0) {
                for (size_t j = i; j < count; ++j) free_extract_file(&batch[j]);
                for (size_t j = 0; j < i; ++j) free_extract_file(&batch[j]);
                goto fatal;
            }
            free_extract_file(&batch[i]);
        }
    }
    local_result.complete = local_result.file_errors == 0 && local_result.stale_sources == 0;
    close(root_fd); sqlite3_close(db); free(db_path); free(canonical);
    if (result) *result = local_result;
    return 0;
fatal:
    close(root_fd); sqlite3_close(db); free(db_path); free(canonical);
    return -1;
}
