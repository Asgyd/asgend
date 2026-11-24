#ifndef AO_ARTIFACTS_H
#define AO_ARTIFACTS_H

/* Task-workspace artifact helpers used by the core ops `artifact_list` and
 * `workspace_read_bytes` (contracts/v1/CONTRACT_SKETCH.md section 8). The
 * caller opens the task workspace securely and passes its directory fd; every
 * further lookup is relative to that fd with O_NOFOLLOW. */

#include <json-c/json.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AO_ARTIFACT_READ_DEFAULT (256U * 1024U)
#define AO_ARTIFACT_READ_MAX (1024U * 1024U)
#define AO_ARTIFACT_LIST_MAX 1000U
#define AO_ARTIFACT_MAX_DEPTH 16
#define AO_ARTIFACT_FILE_MAX ((int64_t)256 * 1024 * 1024)
/* Bounds the work of one listing request: directory entries inspected and
 * total bytes hashed. Hitting either sets `truncated`. */
#define AO_ARTIFACT_SCAN_MAX 100000U
#define AO_ARTIFACT_HASH_BUDGET ((uint64_t)4 * 1024 * 1024 * 1024)

/* Standard base64 (RFC 4648 alphabet, '=' padding, no line breaks). Returns a
 * malloc'ed NUL-terminated string, or NULL on allocation/overflow failure. */
char *artifact_base64_encode(const unsigned char *data, size_t length, size_t *out_length);

/* MIME type by file extension, then by magic bytes of `head`; `head_complete`
 * tells whether `head` is the whole file. Never returns NULL. */
const char *artifact_mime_type(const char *name, const unsigned char *head, size_t head_length, bool head_complete);

/* Recursively lists regular files below `workspace_fd` and returns a reply
 * object (`ok`, `artifacts`, `truncated`, skip counters) or NULL with `err`. */
json_object *artifact_list_json(int workspace_fd, char *err, size_t err_size);

#endif
