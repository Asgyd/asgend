#include "artifacts.h"
#include "runtime.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/evp.h>

#define AO_ARTIFACT_SNIFF_BYTES 512U
#define AO_ARTIFACT_IO_BUFFER (64U * 1024U)

char *artifact_base64_encode(const unsigned char *data, size_t length, size_t *out_length) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (!data && length) return NULL;
    size_t groups = length / 3U + (length % 3U ? 1U : 0U);
    if (groups > (SIZE_MAX - 1U) / 4U) return NULL;
    size_t encoded = groups * 4U;
    char *out = malloc(encoded + 1U);
    if (!out) return NULL;
    size_t i = 0, o = 0;
    while (i + 3U <= length) {
        uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1U] << 8) | (uint32_t)data[i + 2U];
        out[o++] = alphabet[(v >> 18) & 0x3fU];
        out[o++] = alphabet[(v >> 12) & 0x3fU];
        out[o++] = alphabet[(v >> 6) & 0x3fU];
        out[o++] = alphabet[v & 0x3fU];
        i += 3U;
    }
    size_t rest = length - i;
    if (rest) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (rest == 2U) v |= (uint32_t)data[i + 1U] << 8;
        out[o++] = alphabet[(v >> 18) & 0x3fU];
        out[o++] = alphabet[(v >> 12) & 0x3fU];
        out[o++] = rest == 2U ? alphabet[(v >> 6) & 0x3fU] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
    if (out_length) *out_length = o;
    return out;
}

static bool artifact_utf8_valid(const unsigned char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i++];
        if (c <= 0x7f) continue;
        if (c >= 0xc2 && c <= 0xdf) {
            if (i >= n || (s[i] & 0xc0) != 0x80) return false;
            ++i; continue;
        }
        if (c >= 0xe0 && c <= 0xef) {
            if (i + 1 >= n) return false;
            unsigned char c1 = s[i], c2 = s[i + 1];
            if ((c1 & 0xc0) != 0x80 || (c2 & 0xc0) != 0x80) return false;
            if ((c == 0xe0 && c1 < 0xa0) || (c == 0xed && c1 >= 0xa0)) return false;
            i += 2; continue;
        }
        if (c >= 0xf0 && c <= 0xf4) {
            if (i + 2 >= n) return false;
            unsigned char c1 = s[i], c2 = s[i + 1], c3 = s[i + 2];
            if ((c1 & 0xc0) != 0x80 || (c2 & 0xc0) != 0x80 || (c3 & 0xc0) != 0x80) return false;
            if ((c == 0xf0 && c1 < 0x90) || (c == 0xf4 && c1 > 0x8f)) return false;
            i += 3; continue;
        }
        return false;
    }
    return true;
}

typedef struct {
    const char *extension;
    const char *mime;
} ArtifactMimeEntry;

static const ArtifactMimeEntry artifact_mime_table[] = {
    {"txt", "text/plain"}, {"log", "text/plain"}, {"text", "text/plain"}, {"md", "text/markdown"},
    {"markdown", "text/markdown"}, {"csv", "text/csv"}, {"tsv", "text/tab-separated-values"},
    {"html", "text/html"}, {"htm", "text/html"}, {"css", "text/css"}, {"js", "text/javascript"},
    {"mjs", "text/javascript"}, {"json", "application/json"}, {"jsonl", "application/jsonl"},
    {"ndjson", "application/x-ndjson"}, {"xml", "application/xml"}, {"yaml", "application/yaml"},
    {"yml", "application/yaml"}, {"toml", "application/toml"}, {"ini", "text/plain"},
    {"c", "text/x-c"}, {"h", "text/x-c"}, {"cc", "text/x-c++"}, {"cpp", "text/x-c++"},
    {"hpp", "text/x-c++"}, {"py", "text/x-python"}, {"rs", "text/x-rust"}, {"go", "text/x-go"},
    {"java", "text/x-java"}, {"kt", "text/x-kotlin"}, {"dart", "text/x-dart"}, {"sh", "application/x-sh"},
    {"sql", "application/sql"}, {"diff", "text/x-diff"}, {"patch", "text/x-diff"},
    {"pdf", "application/pdf"}, {"png", "image/png"}, {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"},
    {"gif", "image/gif"}, {"webp", "image/webp"}, {"svg", "image/svg+xml"}, {"bmp", "image/bmp"},
    {"ico", "image/vnd.microsoft.icon"}, {"tif", "image/tiff"}, {"tiff", "image/tiff"},
    {"mp3", "audio/mpeg"}, {"wav", "audio/wav"}, {"ogg", "audio/ogg"}, {"flac", "audio/flac"},
    {"mp4", "video/mp4"}, {"webm", "video/webm"}, {"zip", "application/zip"}, {"gz", "application/gzip"},
    {"tgz", "application/gzip"}, {"tar", "application/x-tar"}, {"bz2", "application/x-bzip2"},
    {"xz", "application/x-xz"}, {"zst", "application/zstd"}, {"7z", "application/x-7z-compressed"},
    {"wasm", "application/wasm"}, {"sqlite", "application/vnd.sqlite3"}, {"sqlite3", "application/vnd.sqlite3"},
    {"db", "application/vnd.sqlite3"}, {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
    {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
    {"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
    {"odt", "application/vnd.oasis.opendocument.text"}, {"rtf", "application/rtf"},
    {"bin", "application/octet-stream"},
};

static const char *artifact_mime_by_extension(const char *name) {
    if (!name) return NULL;
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base || !dot[1]) return NULL; /* ".bashrc" has no extension */
    char extension[16];
    size_t n = strlen(dot + 1);
    if (n >= sizeof(extension)) return NULL;
    for (size_t i = 0; i < n; ++i) extension[i] = (char)tolower((unsigned char)dot[1 + i]);
    extension[n] = '\0';
    for (size_t i = 0; i < sizeof(artifact_mime_table) / sizeof(artifact_mime_table[0]); ++i)
        if (strcmp(artifact_mime_table[i].extension, extension) == 0) return artifact_mime_table[i].mime;
    return NULL;
}

static bool head_starts(const unsigned char *head, size_t n, const void *magic, size_t magic_length) {
    return n >= magic_length && memcmp(head, magic, magic_length) == 0;
}

static bool head_starts_text_ci(const unsigned char *head, size_t n, const char *prefix) {
    size_t i = 0;
    while (i < n && (head[i] == ' ' || head[i] == '\t' || head[i] == '\r' || head[i] == '\n')) ++i;
    size_t len = strlen(prefix);
    if (n - i < len) return false;
    for (size_t j = 0; j < len; ++j)
        if (tolower(head[i + j]) != tolower((unsigned char)prefix[j])) return false;
    return true;
}

static bool head_looks_like_text(const unsigned char *head, size_t n, bool complete) {
    if (n == 0 || memchr(head, '\0', n)) return false;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = head[i];
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t' && c != '\f' && c != 0x1b) return false;
    }
    if (artifact_utf8_valid(head, n)) return true;
    if (complete) return false;
    /* The sniff window may end inside a multi-byte code point. */
    for (size_t cut = 1; cut <= 3 && cut < n; ++cut)
        if (artifact_utf8_valid(head, n - cut)) return true;
    return false;
}

const char *artifact_mime_type(const char *name, const unsigned char *head, size_t head_length, bool head_complete) {
    const char *by_extension = artifact_mime_by_extension(name);
    if (by_extension) return by_extension;
    if (!head) head_length = 0;
    if (head_starts(head, head_length, "\x89PNG\r\n\x1a\n", 8)) return "image/png";
    if (head_starts(head, head_length, "\xff\xd8\xff", 3)) return "image/jpeg";
    if (head_starts(head, head_length, "GIF87a", 6) || head_starts(head, head_length, "GIF89a", 6)) return "image/gif";
    if (head_length >= 12 && memcmp(head, "RIFF", 4) == 0 && memcmp(head + 8, "WEBP", 4) == 0) return "image/webp";
    if (head_length >= 12 && memcmp(head, "RIFF", 4) == 0 && memcmp(head + 8, "WAVE", 4) == 0) return "audio/wav";
    if (head_starts(head, head_length, "%PDF-", 5)) return "application/pdf";
    if (head_starts(head, head_length, "PK\x03\x04", 4) || head_starts(head, head_length, "PK\x05\x06", 4)) return "application/zip";
    if (head_starts(head, head_length, "\x1f\x8b", 2)) return "application/gzip";
    if (head_starts(head, head_length, "BZh", 3)) return "application/x-bzip2";
    if (head_starts(head, head_length, "\xfd" "7zXZ\x00", 6)) return "application/x-xz";
    if (head_starts(head, head_length, "\x28\xb5\x2f\xfd", 4)) return "application/zstd";
    if (head_starts(head, head_length, "7z\xbc\xaf\x27\x1c", 6)) return "application/x-7z-compressed";
    if (head_starts(head, head_length, "SQLite format 3\x00", 16)) return "application/vnd.sqlite3";
    if (head_starts(head, head_length, "\x00" "asm", 4)) return "application/wasm";
    if (head_starts(head, head_length, "\x7f" "ELF", 4)) return "application/x-executable";
    if (head_starts(head, head_length, "ID3", 3)) return "audio/mpeg";
    if (head_length >= 262 && memcmp(head + 257, "ustar", 5) == 0) return "application/x-tar";
    if (head_looks_like_text(head, head_length, head_complete)) {
        if (head_starts_text_ci(head, head_length, "<!doctype html") || head_starts_text_ci(head, head_length, "<html"))
            return "text/html";
        if (head_starts_text_ci(head, head_length, "<svg")) return "image/svg+xml";
        if (head_starts_text_ci(head, head_length, "<?xml")) return "application/xml";
        if (head_starts_text_ci(head, head_length, "%!PS")) return "application/postscript";
        return "text/plain";
    }
    return "application/octet-stream";
}

typedef struct {
    json_object *artifacts;
    unsigned char *buffer;
    size_t count;
    size_t visited;
    uint64_t hashed;
    int64_t skipped_too_large;
    int64_t skipped_too_deep;
    int64_t skipped_special;
    int64_t skipped_unsafe;
    int64_t skipped_unreadable;
    int64_t skipped_internal;
    const char *truncated_reason;
    bool truncated;
    bool stop;
    bool failed;
} ArtifactWalk;

static void walk_truncate(ArtifactWalk *walk, const char *reason) {
    if (!walk->truncated) walk->truncated_reason = reason;
    walk->truncated = true;
    walk->stop = true;
}

/* Names must be reusable as `workspace_read_bytes` path components and valid
 * JSON strings: UTF-8, no control characters and no backslash. */
static bool artifact_name_safe(const char *name, size_t length) {
    if (length == 0 || length > NAME_MAX) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7f || c == '\\') return false;
    }
    return artifact_utf8_valid((const unsigned char *)name, length);
}

/* Returns 0 on success, 1 when the file grew beyond the size limit, -1 on error. */
static int artifact_hash_fd(int fd, unsigned char *buffer, size_t buffer_size, char hex[65],
                            unsigned char *head, size_t *head_length, uint64_t *total_out) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return -1;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) { EVP_MD_CTX_free(ctx); return -1; }
    uint64_t total = 0;
    size_t head_used = 0;
    for (;;) {
        ssize_t n = read(fd, buffer, buffer_size);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { EVP_MD_CTX_free(ctx); return -1; }
        if (n == 0) break;
        total += (uint64_t)n;
        if (total > (uint64_t)AO_ARTIFACT_FILE_MAX) { EVP_MD_CTX_free(ctx); return 1; }
        if (head_used < AO_ARTIFACT_SNIFF_BYTES) {
            size_t take = AO_ARTIFACT_SNIFF_BYTES - head_used;
            if (take > (size_t)n) take = (size_t)n;
            memcpy(head + head_used, buffer, take);
            head_used += take;
        }
        if (EVP_DigestUpdate(ctx, buffer, (size_t)n) != 1) { EVP_MD_CTX_free(ctx); return -1; }
    }
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0;
    int ok = EVP_DigestFinal_ex(ctx, digest, &digest_length);
    EVP_MD_CTX_free(ctx);
    if (ok != 1 || digest_length != 32U) return -1;
    static const char digits[] = "0123456789abcdef";
    for (unsigned int i = 0; i < 32U; ++i) {
        hex[i * 2U] = digits[digest[i] >> 4];
        hex[i * 2U + 1U] = digits[digest[i] & 0x0fU];
    }
    hex[64] = '\0';
    *head_length = head_used;
    *total_out = total;
    return 0;
}

static void walk_file(ArtifactWalk *walk, int dir_fd, const char *name, const char *path, const struct stat *seen) {
    if (walk->count >= AO_ARTIFACT_LIST_MAX) { walk_truncate(walk, "entry_limit"); return; }
    if (seen->st_size < 0 || (int64_t)seen->st_size > AO_ARTIFACT_FILE_MAX) { walk->skipped_too_large++; return; }
    if (walk->hashed + (uint64_t)seen->st_size > AO_ARTIFACT_HASH_BUDGET) { walk_truncate(walk, "hash_budget"); return; }
    /* O_NONBLOCK keeps a FIFO swapped in after fstatat from blocking the open. */
    int fd = openat(dir_fd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY);
    if (fd < 0) { walk->skipped_unreadable++; return; }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_dev != seen->st_dev || st.st_ino != seen->st_ino) {
        close(fd); walk->skipped_unreadable++; return;
    }
    char hex[65];
    unsigned char head[AO_ARTIFACT_SNIFF_BYTES];
    size_t head_length = 0;
    uint64_t total = 0;
    int rc = artifact_hash_fd(fd, walk->buffer, AO_ARTIFACT_IO_BUFFER, hex, head, &head_length, &total);
    close(fd);
    if (rc == 1) { walk->skipped_too_large++; return; }
    if (rc != 0) { walk->skipped_unreadable++; return; }
    walk->hashed += total;
    const char *mime = artifact_mime_type(name, head, head_length, total <= AO_ARTIFACT_SNIFF_BYTES);
    json_object *item = json_object_new_object();
    if (!item) { walk->failed = true; walk->stop = true; return; }
    json_object_object_add(item, "path", json_object_new_string(path));
    /* size/sha256 describe exactly the bytes that were hashed. */
    json_object_object_add(item, "size", json_object_new_int64((int64_t)total));
    json_object_object_add(item, "sha256", json_object_new_string(hex));
    json_object_object_add(item, "mime", json_object_new_string(mime));
    if (json_object_array_add(walk->artifacts, item) != 0) {
        json_object_put(item); walk->failed = true; walk->stop = true; return;
    }
    walk->count++;
}

static void walk_directory(ArtifactWalk *walk, int dir_fd, char *path, size_t path_length, int depth) {
    int scan_fd = dup(dir_fd);
    if (scan_fd < 0) { walk->skipped_unreadable++; return; }
    DIR *dir = fdopendir(scan_fd);
    if (!dir) { close(scan_fd); walk->skipped_unreadable++; return; }
    struct dirent *entry;
    while (!walk->stop && (entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (++walk->visited > AO_ARTIFACT_SCAN_MAX) { walk_truncate(walk, "scan_limit"); break; }
        /* Core-internal entries such as `.asgend-write-*` save temporaries. */
        if (strncmp(name, ".asgend", 7) == 0) { walk->skipped_internal++; continue; }
        size_t name_length = strlen(name);
        if (!artifact_name_safe(name, name_length)) { walk->skipped_unsafe++; continue; }
        size_t separator = path_length ? 1U : 0U;
        size_t child_length = path_length + separator + name_length;
        if (child_length >= AO_MAX_PATH) { walk->skipped_unsafe++; continue; }
        struct stat st;
        if (fstatat(dir_fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) { walk->skipped_unreadable++; continue; }
        if (S_ISLNK(st.st_mode) || (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))) { walk->skipped_special++; continue; }
        if (separator) path[path_length] = '/';
        memcpy(path + path_length + separator, name, name_length);
        path[child_length] = '\0';
        if (S_ISDIR(st.st_mode)) {
            if (depth + 1 > AO_ARTIFACT_MAX_DEPTH) {
                walk->skipped_too_deep++;
            } else {
                int child_fd = openat(dir_fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
                struct stat child_st;
                if (child_fd < 0) {
                    walk->skipped_unreadable++;
                } else if (fstat(child_fd, &child_st) != 0 || !S_ISDIR(child_st.st_mode) ||
                           child_st.st_dev != st.st_dev || child_st.st_ino != st.st_ino) {
                    walk->skipped_unreadable++;
                    close(child_fd);
                } else {
                    walk_directory(walk, child_fd, path, child_length, depth + 1);
                    close(child_fd);
                }
            }
        } else {
            walk_file(walk, dir_fd, name, path, &st);
        }
        path[path_length] = '\0';
    }
    closedir(dir);
}

static int artifact_path_compare(const void *left, const void *right) {
    json_object *const *a = left;
    json_object *const *b = right;
    json_object *pa = NULL, *pb = NULL;
    const char *sa = (*a && json_object_object_get_ex(*a, "path", &pa)) ? json_object_get_string(pa) : "";
    const char *sb = (*b && json_object_object_get_ex(*b, "path", &pb)) ? json_object_get_string(pb) : "";
    return strcmp(sa ? sa : "", sb ? sb : "");
}

json_object *artifact_list_json(int workspace_fd, char *err, size_t err_size) {
    ArtifactWalk walk;
    memset(&walk, 0, sizeof(walk));
    walk.artifacts = json_object_new_array();
    walk.buffer = malloc(AO_ARTIFACT_IO_BUFFER);
    if (!walk.artifacts || !walk.buffer) {
        if (walk.artifacts) json_object_put(walk.artifacts);
        free(walk.buffer);
        if (err && err_size) (void)snprintf(err, err_size, "out of memory listing artifacts");
        return NULL;
    }
    char path[AO_MAX_PATH];
    path[0] = '\0';
    walk_directory(&walk, workspace_fd, path, 0, 0);
    free(walk.buffer);
    if (walk.failed) {
        json_object_put(walk.artifacts);
        if (err && err_size) (void)snprintf(err, err_size, "out of memory listing artifacts");
        return NULL;
    }
    json_object_array_sort(walk.artifacts, artifact_path_compare);
    json_object *out = json_object_new_object();
    if (!out) {
        json_object_put(walk.artifacts);
        if (err && err_size) (void)snprintf(err, err_size, "out of memory listing artifacts");
        return NULL;
    }
    json_object_object_add(out, "ok", json_object_new_boolean(true));
    json_object_object_add(out, "artifacts", walk.artifacts);
    json_object_object_add(out, "truncated", json_object_new_boolean(walk.truncated));
    if (walk.truncated && walk.truncated_reason)
        json_object_object_add(out, "truncated_reason", json_object_new_string(walk.truncated_reason));
    json_object_object_add(out, "count", json_object_new_int64((int64_t)walk.count));
    json_object_object_add(out, "skipped_too_large", json_object_new_int64(walk.skipped_too_large));
    json_object_object_add(out, "skipped_too_deep", json_object_new_int64(walk.skipped_too_deep));
    json_object_object_add(out, "skipped_special", json_object_new_int64(walk.skipped_special));
    json_object_object_add(out, "skipped_unsafe_name", json_object_new_int64(walk.skipped_unsafe));
    json_object_object_add(out, "skipped_unreadable", json_object_new_int64(walk.skipped_unreadable));
    json_object_object_add(out, "skipped_internal", json_object_new_int64(walk.skipped_internal));
    return out;
}
