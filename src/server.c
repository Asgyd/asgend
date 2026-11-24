#include "runtime.h"
#include "artifacts.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <ctype.h>
#include <sys/time.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define AO_RESPONSE_MAX (4U * 1024U * 1024U)

static void json_add_string(json_object *obj, const char *key, const char *value) {
    json_object_object_add(obj, key, json_object_new_string(value ? value : ""));
}

static json_object *make_reply(bool ok, const char *error) {
    json_object *obj = json_object_new_object();
    json_object_object_add(obj, "ok", json_object_new_boolean(ok));
    if (error) json_add_string(obj, "error", error);
    return obj;
}

static int send_all(int fd, const char *data, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        ssize_t sent = send(fd, data + offset, len - offset, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return -1;
        offset += (size_t)sent;
    }
    return 0;
}

static int send_json(int fd, json_object *obj) {
    const char *s = obj ? json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN) : NULL;
    if (!s || strlen(s) > AO_RESPONSE_MAX - 2) return -1;
    if (send_all(fd, s, strlen(s)) != 0 || send_all(fd, "\n", 1) != 0) return -1;
    return 0;
}

static int milliseconds_left(const struct timespec *deadline) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    long long ns = ((long long)deadline->tv_sec - now.tv_sec) * 1000000000LL +
                   (long long)deadline->tv_nsec - now.tv_nsec;
    if (ns <= 0) return 0;
    long long ms = (ns + 999999LL) / 1000000LL;
    return ms > 2147483647LL ? 2147483647 : (int)ms;
}

static char *read_request_line(Runtime *rt, int fd) {
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    struct timespec deadline;
    (void)clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += rt->config.request_timeout_seconds;
    for (;;) {
        int left = milliseconds_left(&deadline);
        if (left <= 0) { free(buf); return NULL; }
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int pr = poll(&pfd, 1, left);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0 || (pfd.revents & (POLLERR | POLLNVAL))) { free(buf); return NULL; }
        char chunk[4096];
        ssize_t got = recv(fd, chunk, sizeof(chunk), 0);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { free(buf); return NULL; }
        /* Raw NUL bytes are not valid JSON text. Reject instead of allowing strlen() to truncate a valid prefix. */
        if (memchr(chunk, '\0', (size_t)got) != NULL) { free(buf); return NULL; }
        size_t available = (size_t)got;
        char *nl = memchr(chunk, '\n', available);
        if (nl) available = (size_t)(nl - chunk);
        if (available > rt->config.max_request_bytes - len) { free(buf); return NULL; }
        if (len + available + 1 > cap) {
            size_t needed = len + available + 1;
            size_t next_cap = cap;
            while (next_cap < needed) {
                if (next_cap > rt->config.max_request_bytes / 2) { next_cap = rt->config.max_request_bytes + 1; break; }
                next_cap *= 2;
            }
            if (next_cap > rt->config.max_request_bytes + 1) { free(buf); return NULL; }
            char *next = realloc(buf, next_cap);
            if (!next) { free(buf); return NULL; }
            buf = next; cap = next_cap;
        }
        memcpy(buf + len, chunk, available); len += available; buf[len] = '\0';
        if (nl) break;
        if (len >= rt->config.max_request_bytes) { free(buf); return NULL; }
    }
    if (len == 0) { free(buf); return NULL; }
    return buf;
}

static bool valid_utf8(const unsigned char *s, size_t n) {
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

#define AO_WORKSPACE_FILE_MAX (1024U * 1024U)
#define AO_WORKSPACE_LIST_MAX 500

/* Workspace paths are intentionally relative. Each component is opened with
 * openat(O_NOFOLLOW), so a task-created symlink cannot escape the task root. */
static bool workspace_relative_path_valid(const char *path, bool allow_empty) {
    if (!path) return false;
    size_t length = strlen(path);
    if (length == 0) return allow_empty;
    if (length >= AO_MAX_PATH || path[0] == '/' || path[length - 1] == '/') return false;
    size_t component_start = 0;
    for (size_t i = 0; i <= length; ++i) {
        unsigned char c = (unsigned char)path[i];
        if (c == '\\' || (c < 0x20 && c != '\0')) return false;
        if (c == '/' || c == '\0') {
            size_t component_length = i - component_start;
            if (component_length == 0 || component_length > NAME_MAX) return false;
            if ((component_length == 1 && path[component_start] == '.') ||
                (component_length == 2 && path[component_start] == '.' && path[component_start + 1] == '.'))
                return false;
            component_start = i + 1;
        }
    }
    return true;
}

static int open_task_workspace(Runtime *rt, const char *task_id, char *err, size_t err_size) {
    char stored_path[AO_MAX_PATH] = {0}, tasks_path[AO_MAX_PATH] = {0}, expected_path[AO_MAX_PATH] = {0};
    if (db_task_workspace_path(rt, task_id, stored_path, sizeof(stored_path)) != 0) {
        (void)snprintf(err, err_size, "task workspace is not available"); return -1;
    }
    if (!rt->config.asgend_home[0] ||
        snprintf(tasks_path, sizeof(tasks_path), "%s/tasks", rt->config.asgend_home) >= (int)sizeof(tasks_path) ||
        snprintf(expected_path, sizeof(expected_path), "%s/%s", tasks_path, task_id) >= (int)sizeof(expected_path) ||
        strcmp(stored_path, expected_path) != 0) {
        (void)snprintf(err, err_size, "task workspace path failed its boundary check"); errno = EPERM; return -1;
    }
    int tasks_fd = open(tasks_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (tasks_fd < 0) { (void)snprintf(err, err_size, "cannot open task directory root: %s", strerror(errno)); return -1; }
    struct stat root_stat;
    if (fstat(tasks_fd, &root_stat) != 0 || !S_ISDIR(root_stat.st_mode) || root_stat.st_uid != geteuid() || (root_stat.st_mode & 0022) != 0) {
        close(tasks_fd); errno = EPERM; (void)snprintf(err, err_size, "task directory root is not private and owned by this user"); return -1;
    }
    int workspace_fd = openat(tasks_fd, task_id, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    int saved_errno = errno;
    close(tasks_fd);
    if (workspace_fd < 0) { errno = saved_errno; (void)snprintf(err, err_size, "cannot securely open task workspace: %s", strerror(errno)); return -1; }
    struct stat workspace_stat;
    if (fstat(workspace_fd, &workspace_stat) != 0 || !S_ISDIR(workspace_stat.st_mode) ||
        workspace_stat.st_uid != geteuid() || (workspace_stat.st_mode & 0077) != 0) {
        close(workspace_fd); errno = EPERM; (void)snprintf(err, err_size, "task workspace must be private and owned by this user"); return -1;
    }
    return workspace_fd;
}

static int open_relative_directory(int root_fd, const char *relative_path) {
    if (!workspace_relative_path_valid(relative_path, true)) { errno = EINVAL; return -1; }
    int current = dup(root_fd);
    if (current < 0) return -1;
    if (!*relative_path) return current;
    char copy[AO_MAX_PATH];
    if (string_copy(copy, sizeof(copy), relative_path) != 0) { close(current); errno = ENAMETOOLONG; return -1; }
    char *save = NULL;
    for (char *component = strtok_r(copy, "/", &save); component; component = strtok_r(NULL, "/", &save)) {
        int next = openat(current, component, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        int saved_errno = errno;
        close(current);
        if (next < 0) { errno = saved_errno; return -1; }
        current = next;
    }
    return current;
}

static int open_relative_parent(int root_fd, const char *relative_path, char *basename, size_t basename_size) {
    if (!workspace_relative_path_valid(relative_path, false)) { errno = EINVAL; return -1; }
    char copy[AO_MAX_PATH];
    if (string_copy(copy, sizeof(copy), relative_path) != 0) { errno = ENAMETOOLONG; return -1; }
    char *slash = strrchr(copy, '/');
    const char *leaf = copy;
    int parent_fd = -1;
    if (slash) {
        *slash = '\0';
        leaf = slash + 1;
        parent_fd = open_relative_directory(root_fd, copy);
    } else {
        parent_fd = dup(root_fd);
    }
    if (parent_fd < 0) return -1;
    if (string_copy(basename, basename_size, leaf) != 0) { close(parent_fd); errno = ENAMETOOLONG; return -1; }
    return parent_fd;
}

static json_object *workspace_list_reply(Runtime *rt, const char *task_id, const char *relative_path) {
    char error[512] = {0};
    int root_fd = open_task_workspace(rt, task_id, error, sizeof(error));
    if (root_fd < 0) return make_reply(false, error[0] ? error : "cannot open workspace");
    if (!workspace_relative_path_valid(relative_path, true)) { close(root_fd); return make_reply(false, "path must be a safe relative directory path"); }
    int dir_fd = open_relative_directory(root_fd, relative_path);
    close(root_fd);
    if (dir_fd < 0) return make_reply(false, errno == ENOENT ? "directory does not exist" : "cannot open directory (symlinks and unsafe paths are not allowed)");
    int scan_fd = dup(dir_fd);
    if (scan_fd < 0) { close(dir_fd); return make_reply(false, "cannot read directory"); }
    DIR *dir = fdopendir(scan_fd);
    if (!dir) { close(scan_fd); close(dir_fd); return make_reply(false, "cannot enumerate directory"); }
    json_object *out = make_reply(true, NULL);
    json_object *entries = json_object_new_array();
    if (!out || !entries) { if (out) json_object_put(out); if (entries) json_object_put(entries); closedir(dir); close(dir_fd); return NULL; }
    json_add_string(out, "path", relative_path);
    bool truncated = false;
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        struct stat st;
        if (fstatat(dir_fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
        const char *kind = S_ISDIR(st.st_mode) ? "directory" : S_ISREG(st.st_mode) ? "file" : "unsupported";
        if (S_ISLNK(st.st_mode) || (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))) continue;
        if (count >= AO_WORKSPACE_LIST_MAX) { truncated = true; break; }
        json_object *item = json_object_new_object();
        if (!item) continue;
        json_add_string(item, "name", entry->d_name);
        json_add_string(item, "kind", kind);
        json_object_object_add(item, "size", json_object_new_int64(S_ISREG(st.st_mode) ? (int64_t)st.st_size : 0));
        json_object_array_add(entries, item); count++;
    }
    closedir(dir); close(dir_fd);
    json_object_object_add(out, "entries", entries);
    json_object_object_add(out, "truncated", json_object_new_boolean(truncated));
    return out;
}

static json_object *workspace_read_reply(Runtime *rt, const char *task_id, const char *relative_path) {
    char error[512] = {0};
    int root_fd = open_task_workspace(rt, task_id, error, sizeof(error));
    if (root_fd < 0) return make_reply(false, error[0] ? error : "cannot open workspace");
    char basename[NAME_MAX + 1];
    int parent_fd = open_relative_parent(root_fd, relative_path, basename, sizeof(basename));
    close(root_fd);
    if (parent_fd < 0) return make_reply(false, "file path is invalid, missing, or traverses a symlink");
    int file_fd = openat(parent_fd, basename, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    int saved_errno = errno;
    close(parent_fd);
    if (file_fd < 0) { errno = saved_errno; return make_reply(false, "cannot open file (symlinks are not allowed)"); }
    struct stat st;
    if (fstat(file_fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size > AO_WORKSPACE_FILE_MAX) {
        close(file_fd); return make_reply(false, "only regular text files up to 1 MiB can be opened");
    }
    size_t size = (size_t)st.st_size;
    char *content = malloc(size + 1);
    if (!content) { close(file_fd); return make_reply(false, "out of memory reading file"); }
    size_t used = 0;
    while (used < size) {
        ssize_t n = read(file_fd, content + used, size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { free(content); close(file_fd); return make_reply(false, "file read was incomplete"); }
        used += (size_t)n;
    }
    close(file_fd);
    content[size] = '\0';
    if (memchr(content, '\0', size) || !valid_utf8((const unsigned char *)content, size)) {
        free(content); return make_reply(false, "binary or invalid UTF-8 files are not supported by the text editor");
    }
    json_object *out = make_reply(true, NULL);
    if (out) {
        json_add_string(out, "path", relative_path);
        json_object_object_add(out, "content", json_object_new_string_len(content, (int)size));
        json_object_object_add(out, "size", json_object_new_int64((int64_t)size));
    }
    free(content);
    return out;
}

/* Read one bounded UTF-8 window without loading an entire workspace file.
 * Prefer line-aligned chunks; a single line longer than the window is split at
 * a UTF-8 boundary so callers can still make progress through generated logs. */
#define AO_WORKSPACE_READ_CHUNK_MAX (64U * 1024U)
static json_object *workspace_read_chunk_reply(Runtime *rt, const char *task_id,
                                               const char *relative_path,
                                               int64_t requested_offset, int requested_limit) {
    if (requested_offset < 0) return make_reply(false, "offset must be non-negative");
    if (requested_limit <= 0 || (unsigned int)requested_limit > AO_WORKSPACE_READ_CHUNK_MAX)
        return make_reply(false, "limit must be between 1 and 65536 bytes");

    char error[512] = {0};
    int root_fd = open_task_workspace(rt, task_id, error, sizeof(error));
    if (root_fd < 0) return make_reply(false, error[0] ? error : "cannot open workspace");
    char basename[NAME_MAX + 1];
    int parent_fd = open_relative_parent(root_fd, relative_path, basename, sizeof(basename));
    close(root_fd);
    if (parent_fd < 0) return make_reply(false, "file path is invalid, missing, or traverses a symlink");
    int file_fd = openat(parent_fd, basename, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    int saved_errno = errno;
    close(parent_fd);
    if (file_fd < 0) { errno = saved_errno; return make_reply(false, "cannot open file (symlinks are not allowed)"); }

    struct stat st;
    if (fstat(file_fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        close(file_fd);
        return make_reply(false, "only regular files can be streamed");
    }
    if (requested_offset > (int64_t)st.st_size) {
        close(file_fd);
        return make_reply(false, "offset is past end of file");
    }

    int64_t file_size = (int64_t)st.st_size;
    size_t remaining = (size_t)(file_size - requested_offset);
    size_t wanted = (size_t)requested_limit;
    if (wanted > remaining) wanted = remaining;
    size_t read_capacity = wanted;
    if (wanted < remaining && read_capacity <= SIZE_MAX - 4) read_capacity += 4;
    unsigned char *buffer = malloc(read_capacity + 1);
    if (!buffer) { close(file_fd); return make_reply(false, "out of memory reading file chunk"); }

    size_t used = 0;
    while (used < read_capacity) {
        ssize_t n = pread(file_fd, buffer + used, read_capacity - used,
                          (off_t)(requested_offset + (int64_t)used));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            free(buffer); close(file_fd);
            return make_reply(false, "file read failed");
        }
        if (n == 0) break;
        used += (size_t)n;
    }
    close(file_fd);

    size_t content_length = wanted;
    if (content_length > used) content_length = used;
    if ((int64_t)content_length < file_size - requested_offset) {
        size_t last_newline = SIZE_MAX;
        for (size_t i = 0; i < content_length; ++i)
            if (buffer[i] == '\n') last_newline = i;
        if (last_newline != SIZE_MAX) {
            content_length = last_newline + 1;
        } else {
            /* If this is a very long line, stop before a UTF-8 continuation
             * byte; the following request resumes at that code point. */
            while (content_length > 0 && content_length < used &&
                   (buffer[content_length] & 0xc0U) == 0x80U)
                --content_length;
        }
    }
    if (content_length == 0 && wanted > 0 && used > 0) {
        /* A valid UTF-8 code point cannot exceed four bytes. This fallback
         * prevents a zero-progress loop on unusual files while keeping JSON
         * strings valid. */
        content_length = wanted < used ? wanted : used;
        while (content_length > 0 && !valid_utf8(buffer, content_length)) --content_length;
    }
    if ((content_length > 0 && memchr(buffer, '\0', content_length)) ||
        !valid_utf8(buffer, content_length)) {
        free(buffer);
        return make_reply(false, "file chunk is binary or is not valid UTF-8 text");
    }
    buffer[content_length] = '\0';
    int64_t next_offset = requested_offset + (int64_t)content_length;
    json_object *out = make_reply(true, NULL);
    if (out) {
        json_add_string(out, "path", relative_path);
        json_object_object_add(out, "content", json_object_new_string_len((const char *)buffer, (int)content_length));
        json_object_object_add(out, "size", json_object_new_int64(file_size));
        json_object_object_add(out, "offset", json_object_new_int64(requested_offset));
        json_object_object_add(out, "next_offset", json_object_new_int64(next_offset));
        json_object_object_add(out, "eof", json_object_new_boolean(next_offset >= file_size));
    }
    free(buffer);
    return out;
}

/* Binary-safe bounded read for remote artifact transfer (contract v1 section 8).
 * Memory use is bounded by `limit` (<= 1 MiB) plus its base64 encoding. */
static json_object *workspace_read_bytes_reply(Runtime *rt, const char *task_id,
                                               const char *relative_path,
                                               int64_t requested_offset, size_t requested_limit) {
    if (requested_offset < 0) return make_reply(false, "offset must be non-negative");
    if (requested_limit == 0 || requested_limit > AO_ARTIFACT_READ_MAX)
        return make_reply(false, "limit must be between 1 and 1048576 bytes");

    char error[512] = {0};
    int root_fd = open_task_workspace(rt, task_id, error, sizeof(error));
    if (root_fd < 0) return make_reply(false, error[0] ? error : "cannot open workspace");
    char basename[NAME_MAX + 1];
    int parent_fd = open_relative_parent(root_fd, relative_path, basename, sizeof(basename));
    close(root_fd);
    if (parent_fd < 0) return make_reply(false, "file path is invalid, missing, or traverses a symlink");
    /* O_NONBLOCK: opening a FIFO must not block the connection thread; it is rejected below. */
    int file_fd = openat(parent_fd, basename, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY);
    int saved_errno = errno;
    close(parent_fd);
    if (file_fd < 0) { errno = saved_errno; return make_reply(false, "cannot open file (symlinks are not allowed)"); }

    struct stat st;
    if (fstat(file_fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        close(file_fd);
        return make_reply(false, "only regular files can be read");
    }
    int64_t file_size = (int64_t)st.st_size;
    if (requested_offset > file_size) {
        close(file_fd);
        return make_reply(false, "offset is past end of file");
    }
    uint64_t remaining = (uint64_t)(file_size - requested_offset);
    size_t wanted = remaining < (uint64_t)requested_limit ? (size_t)remaining : requested_limit;
    unsigned char *buffer = malloc(wanted ? wanted : 1U);
    if (!buffer) { close(file_fd); return make_reply(false, "out of memory reading file bytes"); }
    size_t used = 0;
    while (used < wanted) {
        ssize_t n = pread(file_fd, buffer + used, wanted - used, (off_t)(requested_offset + (int64_t)used));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { free(buffer); close(file_fd); return make_reply(false, "file read failed"); }
        if (n == 0) break; /* file shrank concurrently; report what was read */
        used += (size_t)n;
    }
    close(file_fd);

    size_t encoded_length = 0;
    char *encoded = artifact_base64_encode(buffer, used, &encoded_length);
    free(buffer);
    if (!encoded || encoded_length > (size_t)INT_MAX) { free(encoded); return make_reply(false, "out of memory encoding file bytes"); }
    int64_t next_offset = requested_offset + (int64_t)used;
    json_object *out = make_reply(true, NULL);
    if (out) {
        json_add_string(out, "path", relative_path);
        json_object_object_add(out, "data_b64", json_object_new_string_len(encoded, (int)encoded_length));
        json_object_object_add(out, "offset", json_object_new_int64(requested_offset));
        json_object_object_add(out, "next_offset", json_object_new_int64(next_offset));
        json_object_object_add(out, "size", json_object_new_int64(file_size));
        json_object_object_add(out, "eof", json_object_new_boolean(next_offset >= file_size || used < wanted));
    }
    free(encoded);
    return out;
}

static json_object *artifact_list_reply(Runtime *rt, const char *task_id) {
    char error[512] = {0};
    int root_fd = open_task_workspace(rt, task_id, error, sizeof(error));
    if (root_fd < 0) return make_reply(false, error[0] ? error : "cannot open workspace");
    json_object *out = artifact_list_json(root_fd, error, sizeof(error));
    close(root_fd);
    if (!out) return make_reply(false, error[0] ? error : "could not list task artifacts");
    return out;
}

static int write_all_fd(int fd, const char *data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        ssize_t written = write(fd, data + offset, size - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return -1;
        offset += (size_t)written;
    }
    return 0;
}

static json_object *workspace_write_reply(Runtime *rt, const char *task_id, const char *relative_path,
                                          const char *content, size_t content_size) {
    if (content_size > AO_WORKSPACE_FILE_MAX) return make_reply(false, "file content exceeds 1 MiB");
    if (memchr(content, '\0', content_size) || !valid_utf8((const unsigned char *)content, content_size))
        return make_reply(false, "file content must be valid UTF-8 text without NUL bytes");
    char error[512] = {0};
    int root_fd = open_task_workspace(rt, task_id, error, sizeof(error));
    if (root_fd < 0) return make_reply(false, error[0] ? error : "cannot open workspace");
    char basename[NAME_MAX + 1];
    int parent_fd = open_relative_parent(root_fd, relative_path, basename, sizeof(basename));
    close(root_fd);
    if (parent_fd < 0) return make_reply(false, "file path is invalid or parent directory does not exist");
    struct stat existing;
    if (fstatat(parent_fd, basename, &existing, AT_SYMLINK_NOFOLLOW) == 0) {
        if (S_ISLNK(existing.st_mode) || !S_ISREG(existing.st_mode)) { close(parent_fd); return make_reply(false, "only regular files may be overwritten"); }
    } else if (errno != ENOENT) { close(parent_fd); return make_reply(false, "cannot inspect destination file"); }
    char temporary[NAME_MAX + 1];
    int fd = -1;
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        (void)snprintf(temporary, sizeof(temporary), ".asgend-write-%ld-%u-%u", (long)getpid(), (unsigned)time(NULL), attempt);
        fd = openat(parent_fd, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) { close(parent_fd); return make_reply(false, "cannot create temporary file for safe save"); }
    int rc = fchmod(fd, 0600);
    if (rc == 0) rc = write_all_fd(fd, content, content_size);
    if (rc == 0 && fsync(fd) != 0) rc = -1;
    int saved_errno = errno;
    if (close(fd) != 0 && rc == 0) { rc = -1; saved_errno = errno; }
    if (rc == 0 && renameat(parent_fd, temporary, parent_fd, basename) != 0) { rc = -1; saved_errno = errno; }
    if (rc == 0 && fsync(parent_fd) != 0) { rc = -1; saved_errno = errno; }
    if (rc != 0) (void)unlinkat(parent_fd, temporary, 0);
    close(parent_fd);
    if (rc != 0) { errno = saved_errno; return make_reply(false, "safe file save failed"); }
    json_object *out = make_reply(true, NULL);
    if (out) {
        json_add_string(out, "path", relative_path);
        json_object_object_add(out, "size", json_object_new_int64((int64_t)content_size));
    }
    return out;
}

static bool request_string_length(json_object *request, const char *key,
                                  const char **value, size_t *length) {
    json_object *entry = NULL;
    if (!json_object_object_get_ex(request, key, &entry) || !entry || json_object_get_type(entry) != json_type_string) return false;
    const char *s = json_object_get_string(entry);
    int n = json_object_get_string_len(entry);
    if (!s || n < 0 || (size_t)n != strlen(s)) return false; /* reject embedded NUL */
    if (value) *value = s;
    if (length) *length = (size_t)n;
    return true;
}

static bool constant_time_equal(const char *a, const char *b) {
    if (!a || !b) return false;
    size_t na = strlen(a), nb = strlen(b), n = na > nb ? na : nb;
    unsigned char diff = (unsigned char)(na ^ nb);
    for (size_t i = 0; i < n; ++i) {
        unsigned char ca = i < na ? (unsigned char)a[i] : 0;
        unsigned char cb = i < nb ? (unsigned char)b[i] : 0;
        diff |= (unsigned char)(ca ^ cb);
    }
    return diff == 0;
}

static void plan_revision_bump(Runtime *rt) {
    if (!rt || !rt->plan_sync_initialized) return;
    pthread_mutex_lock(&rt->plan_mutex);
    /* Keep the wire representation within signed JSON int64 range. */
    rt->plan_revision = rt->plan_revision >= (uint64_t)INT64_MAX ? 1U : rt->plan_revision + 1U;
    pthread_cond_broadcast(&rt->plan_cond);
    pthread_mutex_unlock(&rt->plan_mutex);
}

static json_object *plan_wait_reply(Runtime *rt, json_object *request) {
    json_object *revision_value = NULL, *timeout_value = NULL;
    int64_t observed_revision = 0;
    int timeout_ms = 15000;
    if (json_object_object_get_ex(request, "revision", &revision_value)) {
        if (!revision_value || json_object_get_type(revision_value) != json_type_int ||
            json_object_get_int64(revision_value) < 0)
            return make_reply(false, "revision must be a non-negative integer");
        observed_revision = json_object_get_int64(revision_value);
    }
    if (json_object_object_get_ex(request, "timeout_ms", &timeout_value)) {
        if (!timeout_value || json_object_get_type(timeout_value) != json_type_int)
            return make_reply(false, "timeout_ms must be an integer");
        int64_t requested_timeout = json_object_get_int64(timeout_value);
        if (requested_timeout < 0 || requested_timeout > 30000)
            return make_reply(false, "timeout_ms must be between 0 and 30000");
        timeout_ms = (int)requested_timeout;
    }

    struct timespec deadline = {0};
    if (timeout_ms > 0) {
        if (clock_gettime(CLOCK_REALTIME, &deadline) != 0)
            return make_reply(false, "could not read system clock");
        deadline.tv_sec += timeout_ms / 1000;
        deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            ++deadline.tv_sec;
            deadline.tv_nsec -= 1000000000L;
        }
    }

    pthread_mutex_lock(&rt->plan_mutex);
    int wait_rc = 0;
    while (!rt->stopping && rt->plan_revision == (uint64_t)observed_revision && timeout_ms > 0) {
        wait_rc = pthread_cond_timedwait(&rt->plan_cond, &rt->plan_mutex, &deadline);
        if (wait_rc == ETIMEDOUT) break;
        if (wait_rc != 0) break;
    }
    uint64_t current_revision = rt->plan_revision;
    bool stopping = rt->stopping != 0;
    pthread_mutex_unlock(&rt->plan_mutex);
    if (wait_rc != 0 && wait_rc != ETIMEDOUT)
        return make_reply(false, "plan notification wait failed");

    json_object *out = make_reply(true, NULL);
    json_object_object_add(out, "revision", json_object_new_int64((int64_t)current_revision));
    json_object_object_add(out, "changed", json_object_new_boolean(current_revision != (uint64_t)observed_revision));
    json_object_object_add(out, "stopping", json_object_new_boolean(stopping));
    return out;
}

json_object *server_handle_request(Runtime *rt, json_object *request) {
    if (!request || json_object_get_type(request) != json_type_object) return make_reply(false, "request must be a JSON object");
    const char *token = NULL;
    (void)json_get_string(request, "auth_token", &token);
    if (rt->config.auth_token[0] && (!token || !constant_time_equal(token, rt->config.auth_token)))
        return make_reply(false, "unauthorized");
    const char *op = NULL;
    if (json_get_string(request, "op", &op) != 0) return make_reply(false, "op is required");
    if (strcmp(op, "status") == 0 || strcmp(op, "events") == 0 ||
        strcmp(op, "cancel") == 0 || strcmp(op, "answer") == 0 ||
        strcmp(op, "retry_task") == 0 ||
        strcmp(op, "workspace_list") == 0 || strcmp(op, "workspace_read") == 0 || strcmp(op, "workspace_write") == 0 ||
        strcmp(op, "workspace_read_bytes") == 0 || strcmp(op, "artifact_list") == 0) {
        const char *requested_id = NULL;
        if (json_get_string(request, "task_id", &requested_id) != 0 || !requested_id || !*requested_id)
            return make_reply(false, "task_id is required");
        char resolved_id[AO_MAX_ID], resolve_error[256] = {0};
        int resolve_rc = db_resolve_task_id(rt, requested_id, resolved_id, sizeof(resolved_id), resolve_error, sizeof(resolve_error));
        if (resolve_rc != 0) return make_reply(false, resolve_error[0] ? resolve_error : "task ID lookup failed");
        json_object_object_add(request, "task_id", json_object_new_string(resolved_id));
    }
    if (strcmp(op, "ping") == 0) {
        json_object *out = make_reply(true, NULL);
        json_add_string(out, "service", "agent-orchestrator-c");
        json_add_string(out, "version", AGEND_VERSION);
        return out;
    }
    if (strcmp(op, "plan_wait") == 0) return plan_wait_reply(rt, request);
    if (strcmp(op, "config_get") == 0) {
        char config_err[1024] = {0};
        json_object *config = config_editor_get(rt, config_err, sizeof(config_err));
        if (!config) return make_reply(false, config_err[0] ? config_err : "could not read configuration");
        return config;
    }
    if (strcmp(op, "config_save") == 0) {
        char config_err[1024] = {0};
        if (config_editor_save(rt, request, config_err, sizeof(config_err)) != 0)
            return make_reply(false, config_err[0] ? config_err : "configuration was rejected");
        json_object *out = make_reply(true, NULL);
        json_object_object_add(out, "restart_required", json_object_new_boolean(true));
        json_object_object_add(out, "message", json_object_new_string("Configuration files were saved. Restart the core to apply structural changes."));
        return out;
    }
    if (strcmp(op, "config_update_settings") == 0) {
        char config_err[1024] = {0};
        if (config_editor_update_settings(rt, request, config_err, sizeof(config_err)) != 0)
            return make_reply(false, config_err[0] ? config_err : "settings update was rejected");
        json_object *out = make_reply(true, NULL);
        json_object_object_add(out, "restart_required", json_object_new_boolean(true));
        json_object_object_add(out, "message", json_object_new_string("Allowed settings were saved atomically. Restart the core to apply changes."));
        return out;
    }
    if (strcmp(op, "models") == 0) {
        json_object *out = make_reply(true, NULL);
        json_object *models = models_status_json(rt);
        json_object *resources = models_resource_snapshot_json(rt);
        if (models) json_object_object_add(out, "model_status", models);
        if (resources) json_object_object_add(out, "resources", resources);
        return out;
    }
    if (strcmp(op, "resources") == 0) {
        json_object *resources = resources_status_json(rt);
        if (!resources) return make_reply(false, "could not read resource catalog");
        return resources;
    }
    if (strcmp(op, "mcp_status") == 0) {
        json_object *out = make_reply(true, NULL);
        json_object *servers = json_object_new_array();
        if (!out || !servers) {
            if (out) json_object_put(out);
            if (servers) json_object_put(servers);
            return make_reply(false, "could not allocate MCP status response");
        }
        for (size_t i = 0; i < rt->mcp_server_count; ++i) {
            const MCPServer *server = &rt->mcp_servers[i];
            bool running = atomic_load_explicit(&server->started, memory_order_acquire);
            json_object *entry = json_object_new_object();
            if (!entry) continue;
            json_add_string(entry, "name", server->config.name);
            json_add_string(entry, "transport", server->config.transport);
            json_add_string(entry, "status", running ? "running" :
                            (server->config.command[0] || server->config.url[0] ? "stopped" : "disabled"));
            json_object_object_add(entry, "connected", json_object_new_boolean(running));
            json_object_array_add(servers, entry);
        }
        json_object_object_add(out, "servers", servers);
        return out;
    }
    if (strcmp(op, "lsp_diagnostics") == 0) {
        const char *file_path = NULL;
        if (json_get_string(request, "file_path", &file_path) != 0 || !file_path || !*file_path)
            return make_reply(false, "file_path is required");
        char lsp_err[1024] = {0};
        char *diagnostics = lsp_run_diagnostics(rt, file_path, lsp_err, sizeof(lsp_err));
        if (!diagnostics) return make_reply(false, lsp_err[0] ? lsp_err : "LSP diagnostics failed");
        json_object *out = make_reply(true, NULL);
        json_add_string(out, "diagnostics", diagnostics);
        free(diagnostics);
        return out;
    }
    if (strcmp(op, "model_enable") == 0 || strcmp(op, "model_disable") == 0 ||
        strcmp(op, "model_start") == 0 || strcmp(op, "model_stop") == 0) {
        const char *model_id = NULL, *kind = NULL;
        if (json_get_string(request, "model_id", &model_id) != 0 || !*model_id)
            return make_reply(false, "model_id is required");
        (void)json_get_string(request, "kind", &kind);
        bool decision = kind && strcmp(kind, "decision") == 0;
        if (kind && strcmp(kind, "decision") != 0 && strcmp(kind, "llm") != 0)
            return make_reply(false, "kind must be llm or decision");
        char model_err[1024] = {0};
        int rc = 0;
        if (strcmp(op, "model_enable") == 0) rc = model_set_enabled(rt, model_id, decision, true, model_err, sizeof(model_err));
        else if (strcmp(op, "model_disable") == 0) rc = model_set_enabled(rt, model_id, decision, false, model_err, sizeof(model_err));
        else if (strcmp(op, "model_start") == 0) rc = model_start(rt, model_id, decision, model_err, sizeof(model_err));
        else rc = model_stop(rt, model_id, decision, model_err, sizeof(model_err));
        if (rc != 0) return make_reply(false, model_err[0] ? model_err : "model operation failed");
        json_object *out = make_reply(true, NULL);
        json_object *models = models_status_json(rt);
        if (models) json_object_object_add(out, "model_status", models);
        return out;
    }
    if (strcmp(op, "list_agents") == 0) {
        json_object *out = make_reply(true, NULL);
        json_object *arr = json_object_new_array();
        for (size_t i = 0; i < rt->agent_count; ++i) {
            json_object *a = json_object_new_object();
            json_add_string(a, "id", rt->agents[i].id);
            json_add_string(a, "name", rt->agents[i].name);
            json_add_string(a, "description", rt->agents[i].description);
            json_add_string(a, "model_id", rt->agents[i].model_id);
            json_object_array_add(arr, a);
        }
        json_object_object_add(out, "agents", arr);
        return out;
    }
    if (strcmp(op, "health") == 0) {
        char db_err[512] = {0};
        json_object *queue = db_queue_health(rt, db_err, sizeof(db_err));
        if (!queue) return make_reply(false, db_err[0] ? db_err : "could not read queue health");
        json_object *out = make_reply(true, NULL);
        json_object_object_add(out, "tasks_by_status", queue);
        pthread_mutex_lock(&rt->connections_mutex);
        int active_connections = rt->active_connections;
        pthread_mutex_unlock(&rt->connections_mutex);
        json_object_object_add(out, "active_connections", json_object_new_int(active_connections));
        json_object_object_add(out, "workers", json_object_new_int(rt->worker_count));
        json_object_object_add(out, "draining", json_object_new_boolean(rt->stopping != 0));
        json_object_object_add(out, "priority_aging_seconds", json_object_new_int(rt->config.priority_aging_seconds));
        json_object *resources = models_resource_snapshot_json(rt);
        if (resources) json_object_object_add(out, "resources", resources);
        json_object_object_add(out, "unix_socket_enabled", json_object_new_boolean(rt->config.unix_socket_enabled));
        json_object_object_add(out, "ui_enabled", json_object_new_boolean(rt->config.ui_enabled));
        json_object_object_add(out, "model_status", models_status_json(rt));
        return out;
    }
    if (strcmp(op, "plan_list") == 0 || strcmp(op, "plan_add") == 0 ||
        strcmp(op, "plan_update") == 0 || strcmp(op, "plan_delete") == 0) {
        const char *scope_arg = NULL, *task_arg = NULL;
        (void)json_get_string(request, "scope_id", &scope_arg);
        (void)json_get_string(request, "task_id", &task_arg);
        const char *scope_source = scope_arg && *scope_arg ? scope_arg : (task_arg && *task_arg ? task_arg : "global");
        char scope[AO_MAX_ID] = "global", scope_err[256] = {0};
        if (strcmp(scope_source, "global") != 0) {
            if (db_resolve_task_id(rt, scope_source, scope, sizeof(scope), scope_err, sizeof(scope_err)) != 0)
                return make_reply(false, scope_err[0] ? scope_err : "unknown plan scope");
        }
        if (strcmp(op, "plan_list") == 0) {
            const char *kind = NULL; (void)json_get_string(request, "kind", &kind);
            json_object *items = db_plan_list(rt, scope, kind, scope_err, sizeof(scope_err));
            if (!items) return make_reply(false, scope_err[0] ? scope_err : "could not read plan items");
            json_object *out = make_reply(true, NULL);
            json_add_string(out, "scope_id", scope);
            json_object_object_add(out, "items", items);
            return out;
        }
        if (strcmp(op, "plan_add") == 0) {
            const char *kind = NULL, *title = NULL, *description = NULL;
            if (json_get_string(request, "kind", &kind) != 0 || json_get_string(request, "title", &title) != 0)
                return make_reply(false, "kind and title are required");
            (void)json_get_string(request, "description", &description);
            int64_t item_id = 0;
            if (db_plan_add(rt, scope, kind, title, description, &item_id, scope_err, sizeof(scope_err)) != 0)
                return make_reply(false, scope_err[0] ? scope_err : "could not add plan item");
            plan_revision_bump(rt);
            json_object *out = make_reply(true, NULL);
            json_object_object_add(out, "item_id", json_object_new_int64(item_id));
            json_add_string(out, "scope_id", scope);
            return out;
        }
        json_object *id_value = NULL;
        if (!json_object_object_get_ex(request, "item_id", &id_value) || !id_value ||
            (json_object_get_type(id_value) != json_type_int && json_object_get_type(id_value) != json_type_string))
            return make_reply(false, "item_id is required");
        int64_t item_id = json_object_get_type(id_value) == json_type_int ? json_object_get_int64(id_value) : strtoll(json_object_get_string(id_value), NULL, 10);
        if (item_id <= 0) return make_reply(false, "item_id must be a positive integer");
        int action_rc;
        if (strcmp(op, "plan_update") == 0) {
            const char *title = NULL, *description = NULL, *status = NULL;
            (void)json_get_string(request, "title", &title);
            (void)json_get_string(request, "description", &description);
            (void)json_get_string(request, "status", &status);
            action_rc = db_plan_update(rt, scope, item_id, title, description, status, scope_err, sizeof(scope_err));
        } else {
            action_rc = db_plan_delete(rt, scope, item_id, scope_err, sizeof(scope_err));
        }
        if (action_rc != 0) return make_reply(false, scope_err[0] ? scope_err : "plan item update failed");
        plan_revision_bump(rt);
        json_object *out = make_reply(true, NULL);
        json_object_object_add(out, "item_id", json_object_new_int64(item_id));
        json_add_string(out, "scope_id", scope);
        return out;
    }
    if (strcmp(op, "workspace_list") == 0) {
        const char *task_id = NULL, *path = "";
        if (json_get_string(request, "task_id", &task_id) != 0 || !task_id || !*task_id)
            return make_reply(false, "task_id is required");
        (void)json_get_string(request, "path", &path);
        return workspace_list_reply(rt, task_id, path ? path : "");
    }
    if (strcmp(op, "workspace_read") == 0) {
        const char *task_id = NULL, *path = NULL;
        if (json_get_string(request, "task_id", &task_id) != 0 || !task_id || !*task_id)
            return make_reply(false, "task_id is required");
        if (json_get_string(request, "path", &path) != 0 || !path || !*path)
            return make_reply(false, "path is required");
        return workspace_read_reply(rt, task_id, path);
    }
    if (strcmp(op, "workspace_read_chunk") == 0) {
        const char *task_id = NULL, *path = NULL;
        int64_t offset = 0;
        int limit = 32768;
        json_object *value = NULL;
        if (json_get_string(request, "task_id", &task_id) != 0 || !task_id || !*task_id)
            return make_reply(false, "task_id is required");
        if (json_get_string(request, "path", &path) != 0 || !path || !*path)
            return make_reply(false, "path is required");
        if (json_object_object_get_ex(request, "offset", &value) && value) {
            if (json_object_get_type(value) != json_type_int)
                return make_reply(false, "offset must be an integer");
            offset = json_object_get_int64(value);
        }
        if (json_object_object_get_ex(request, "limit", &value) && value) {
            if (json_object_get_type(value) != json_type_int)
                return make_reply(false, "limit must be an integer");
            int64_t requested = json_object_get_int64(value);
            if (requested < 1 || requested > (int64_t)AO_WORKSPACE_READ_CHUNK_MAX)
                return make_reply(false, "limit must be between 1 and 65536 bytes");
            limit = (int)requested;
        }
        return workspace_read_chunk_reply(rt, task_id, path, offset, limit);
    }
    if (strcmp(op, "workspace_read_bytes") == 0) {
        const char *task_id = NULL, *path = NULL;
        size_t path_length = 0;
        int64_t offset = 0;
        size_t limit = AO_ARTIFACT_READ_DEFAULT;
        json_object *value = NULL;
        if (json_get_string(request, "task_id", &task_id) != 0 || !task_id || !*task_id)
            return make_reply(false, "task_id is required");
        if (!json_object_object_get_ex(request, "path", &value) || !value)
            return make_reply(false, "path is required");
        /* Reject embedded NUL explicitly: a truncated C string could name another file. */
        if (!request_string_length(request, "path", &path, &path_length))
            return make_reply(false, "path must be a string without embedded NUL bytes");
        if (path_length == 0) return make_reply(false, "path is required");
        if (json_object_object_get_ex(request, "offset", &value) && value) {
            if (json_object_get_type(value) != json_type_int)
                return make_reply(false, "offset must be an integer");
            offset = json_object_get_int64(value);
        }
        if (json_object_object_get_ex(request, "limit", &value) && value) {
            if (json_object_get_type(value) != json_type_int)
                return make_reply(false, "limit must be an integer");
            int64_t requested = json_object_get_int64(value);
            if (requested < 1 || requested > (int64_t)AO_ARTIFACT_READ_MAX)
                return make_reply(false, "limit must be between 1 and 1048576 bytes");
            limit = (size_t)requested;
        }
        return workspace_read_bytes_reply(rt, task_id, path, offset, limit);
    }
    if (strcmp(op, "artifact_list") == 0) {
        const char *task_id = NULL;
        if (json_get_string(request, "task_id", &task_id) != 0 || !task_id || !*task_id)
            return make_reply(false, "task_id is required");
        return artifact_list_reply(rt, task_id);
    }
    if (strcmp(op, "workspace_write") == 0) {
        const char *task_id = NULL, *path = NULL, *content = NULL;
        size_t content_size = 0;
        if (json_get_string(request, "task_id", &task_id) != 0 || !task_id || !*task_id)
            return make_reply(false, "task_id is required");
        if (json_get_string(request, "path", &path) != 0 || !path || !*path)
            return make_reply(false, "path is required");
        if (!request_string_length(request, "content", &content, &content_size))
            return make_reply(false, "content must be a string without embedded NUL bytes");
        return workspace_write_reply(rt, task_id, path, content, content_size);
    }
    if (strcmp(op, "submit") == 0) {
        const char *task = NULL, *session_id = NULL, *idem = NULL, *agent_id = NULL;
        int priority = 5;
        if (json_get_string(request, "task", &task) != 0 && json_get_string(request, "prompt", &task) != 0)
            return make_reply(false, "task is required and must be a string");
        if (!task || !*task) return make_reply(false, "task must not be empty");
        if (strlen(task) > 100000) return make_reply(false, "task exceeds 100000 bytes");
        (void)json_get_string(request, "session_id", &session_id);
        (void)json_get_string(request, "idempotency_key", &idem);
        (void)json_get_string(request, "agent", &agent_id);
        if (session_id && strlen(session_id) > 128) return make_reply(false, "session_id exceeds 128 bytes");
        if (idem && strlen(idem) > 128) return make_reply(false, "idempotency_key exceeds 128 bytes");
        if (agent_id && !agent_find(rt, agent_id)) return make_reply(false, "unknown agent id");
        json_object *priority_obj = NULL;
        if (json_object_object_get_ex(request, "priority", &priority_obj)) {
            if (!priority_obj || json_object_get_type(priority_obj) != json_type_int) return make_reply(false, "priority must be an integer in 0..9");
            priority = json_object_get_int(priority_obj);
            if (priority < 0 || priority > 9) return make_reply(false, "priority must be an integer in 0..9");
        }
        /* Do not persist transport credentials inside the durable task payload. */
        json_object_object_del(request, "auth_token");
        char *raw = NULL;
        if (json_to_string(request, &raw) != 0) return make_reply(false, "could not serialize submitted task");
        char task_id[AO_MAX_ID]; bool duplicate = false; char db_err[512] = {0};
        int rc = db_enqueue(rt, raw, session_id, idem, priority, task_id, sizeof(task_id), &duplicate, db_err, sizeof(db_err));
        free(raw);
        if (rc == -2) {
            json_object *out = make_reply(false, "task queue is full");
            json_object_object_add(out, "retryable", json_object_new_boolean(true));
            return out;
        }
        if (rc != 0) return make_reply(false, db_err[0] ? db_err : "could not enqueue task");
        if (!duplicate) {
            char workspace_path[AO_MAX_PATH] = {0};
            json_object *workspace_payload = json_object_new_object();
            if (db_task_workspace_path(rt, task_id, workspace_path, sizeof(workspace_path)) == 0) {
                json_add_string(workspace_payload, "workspace_path", workspace_path);
                (void)db_append_task_event(rt, task_id, "task_workspace_created", "orchestrator",
                                           "A private working directory was created for this task.", workspace_payload);
            }
            if (workspace_payload) json_object_put(workspace_payload);
            (void)db_append_task_event(rt, task_id, "task_created", "user", task, NULL);
            pthread_mutex_lock(&rt->queue_mutex); pthread_cond_signal(&rt->queue_cond); pthread_mutex_unlock(&rt->queue_mutex);
        }
        json_object *out = make_reply(true, NULL);
        json_object_object_add(out, "accepted", json_object_new_boolean(true));
        json_add_string(out, "task_id", task_id);
        json_add_string(out, "status", duplicate ? "existing" : "queued");
        json_object_object_add(out, "duplicate", json_object_new_boolean(duplicate));
        json_object_object_add(out, "priority", json_object_new_int(priority));
        return out;
    }
    if (strcmp(op, "status") == 0) {
        const char *task_id = NULL;
        if (json_get_string(request, "task_id", &task_id) != 0 || !*task_id) return make_reply(false, "task_id is required");
        char db_err[512] = {0};
        json_object *status = db_task_status(rt, task_id, db_err, sizeof(db_err));
        if (!status) return make_reply(false, "task not found");
        json_object_object_add(status, "ok", json_object_new_boolean(true));
        return status;
    }
    if (strcmp(op, "list_tasks") == 0) {
        int limit = 50;
        const char *status_filter = NULL;
        json_object *limit_obj = NULL;
        if (json_object_object_get_ex(request, "limit", &limit_obj) && limit_obj) {
            if (json_object_get_type(limit_obj) != json_type_int) return make_reply(false, "limit must be an integer");
            limit = json_object_get_int(limit_obj);
        }
        (void)json_get_string(request, "status", &status_filter);
        char db_err[512] = {0};
        json_object *tasks = db_list_tasks(rt, status_filter, limit, db_err, sizeof(db_err));
        if (!tasks) return make_reply(false, db_err[0] ? db_err : "could not list tasks");
        json_object *out = make_reply(true, NULL);
        json_object_object_add(out, "tasks", tasks);
        return out;
    }
    if (strcmp(op, "dead_letters") == 0) {
        int limit = 50;
        json_object *limit_obj = NULL;
        if (json_object_object_get_ex(request, "limit", &limit_obj) && limit_obj) {
            if (json_object_get_type(limit_obj) != json_type_int) return make_reply(false, "limit must be an integer");
            limit = json_object_get_int(limit_obj);
        }
        char db_err[512] = {0};
        json_object *tasks = db_list_tasks(rt, "failed", limit, db_err, sizeof(db_err));
        if (!tasks) return make_reply(false, db_err[0] ? db_err : "could not inspect dead letters");
        json_object *out = make_reply(true, NULL);
        json_object_object_add(out, "dead_letters", tasks);
        return out;
    }
    if (strcmp(op, "retry_task") == 0) {
        const char *task_id = NULL;
        if (json_get_string(request, "task_id", &task_id) != 0 || !task_id || !*task_id)
            return make_reply(false, "task_id is required");
        char db_err[512] = {0};
        int rc = db_retry_failed_task(rt, task_id, db_err, sizeof(db_err));
        if (rc < 0) return make_reply(false, db_err[0] ? db_err : "could not retry failed task");
        if (rc == 0) return make_reply(false, "task is not a failed dead letter");
        pthread_mutex_lock(&rt->queue_mutex);
        pthread_cond_signal(&rt->queue_cond);
        pthread_mutex_unlock(&rt->queue_mutex);
        json_object *out = make_reply(true, NULL);
        json_add_string(out, "task_id", task_id);
        json_add_string(out, "status", "queued");
        json_object_object_add(out, "additional_attempts", json_object_new_int(1));
        return out;
    }
    if (strcmp(op, "cancel") == 0) {
        const char *task_id = NULL;
        if (json_get_string(request, "task_id", &task_id) != 0 || !*task_id) return make_reply(false, "task_id is required");
        char db_err[512] = {0};
        int rc = db_cancel_task(rt, task_id, db_err, sizeof(db_err));
        if (rc < 0) return make_reply(false, db_err[0] ? db_err : "could not cancel task");
        if (rc == 0) {
            json_object *out = make_reply(false, "task does not exist or is already terminal");
            return out;
        }
        (void)db_append_task_event(rt, task_id, "task_cancel_requested", "user", "Cancellation requested.", NULL);
        json_object *out = make_reply(true, NULL);
        json_add_string(out, "task_id", task_id);
        json_add_string(out, "status", "cancellation_requested");
        return out;
    }
    if (strcmp(op, "events") == 0) {
        const char *task_id = NULL;
        if (json_get_string(request, "task_id", &task_id) != 0 || !*task_id) return make_reply(false, "task_id is required");
        int after_id = 0, limit = 100;
        json_object *value = NULL;
        if (json_object_object_get_ex(request, "after_id", &value) && value && json_object_get_type(value) == json_type_int) after_id = json_object_get_int(value);
        if (json_object_object_get_ex(request, "limit", &value) && value && json_object_get_type(value) == json_type_int) limit = json_object_get_int(value);
        char db_err[512] = {0};
        json_object *events = db_task_events(rt, task_id, after_id, limit, db_err, sizeof(db_err));
        if (!events) return make_reply(false, db_err[0] ? db_err : "could not list task events");
        json_object *out = make_reply(true, NULL);
        json_object_object_add(out, "events", events);
        return out;
    }
    if (strcmp(op, "answer") == 0) {
        const char *task_id = NULL, *answer = NULL, *option_id = NULL;
        if (json_get_string(request, "task_id", &task_id) != 0 || !*task_id) return make_reply(false, "task_id is required");
        if (json_get_string(request, "answer", &answer) != 0 || !*answer) return make_reply(false, "answer is required");
        (void)json_get_string(request, "option_id", &option_id);
        char db_err[512] = {0};
        if (db_answer_task(rt, task_id, answer, option_id, db_err, sizeof(db_err)) != 0)
            return make_reply(false, db_err[0] ? db_err : "could not answer task question");
        pthread_mutex_lock(&rt->queue_mutex); pthread_cond_signal(&rt->queue_cond); pthread_mutex_unlock(&rt->queue_mutex);
        json_object *out = make_reply(true, NULL);
        json_add_string(out, "task_id", task_id);
        json_add_string(out, "status", "queued");
        return out;
    }
    return make_reply(false, "unsupported op; use ping, health, list_agents, submit, status, list_tasks, events, answer, cancel, workspace operations, plan_list/plan_add/plan_update/plan_delete/plan_wait, config, models, resources and mcp_status");
}

static void connection_release(Runtime *rt, int fd) {
    pthread_mutex_lock(&rt->connections_mutex);
    for (int i = 0; i < rt->active_connections; ++i) {
        if (rt->connection_fds[i] == fd) {
            rt->connection_fds[i] = rt->connection_fds[rt->active_connections - 1];
            rt->connection_fds[rt->active_connections - 1] = -1;
            --rt->active_connections;
            break;
        }
    }
    pthread_mutex_unlock(&rt->connections_mutex);
}

/* Wake request handlers blocked on partially or not-yet-read requests during
   shutdown. Keep descriptor ownership with connection_main(): shutdown() is
   only used to interrupt I/O, never to close a descriptor behind its owner. */
static void shutdown_active_connections(Runtime *rt) {
    if (!rt) return;
    pthread_mutex_lock(&rt->connections_mutex);
    for (int i = 0; i < rt->active_connections; ++i) {
        int fd = rt->connection_fds[i];
        if (fd >= 0) (void)shutdown(fd, SHUT_RDWR);
    }
    pthread_mutex_unlock(&rt->connections_mutex);
}


static void *connection_main(void *arg);

#define AO_HTTP_HEADER_MAX 16384U
#define AO_HTTP_BODY_MAX (256U * 1024U)

typedef struct {
    char *raw;
    size_t length;
    size_t header_length;
    size_t body_length;
    char method[8];
    char target[2048];
    char host[256];
    char token[512];
    bool has_content_length;
    bool transfer_encoding;
} HttpRequest;

static int send_http_response(int fd, int code, const char *reason, const char *content_type,
                              const char *body, size_t body_length, bool is_static) {
    char header[2048];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Referrer-Policy: no-referrer\r\n"
        "X-Frame-Options: DENY\r\n"
        "Cache-Control: %s\r\n"
        "Content-Security-Policy: default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data: blob:; media-src 'self' blob:; frame-src 'self' blob:; connect-src 'self' http://127.0.0.1:8788 http://localhost:8788; base-uri 'none'; frame-ancestors 'none'; form-action 'self'\r\n"
        "\r\n", code, reason, content_type, body_length, is_static ? "public, max-age=60" : "no-store");
    if (n < 0 || (size_t)n >= sizeof(header)) return -1;
    if (send_all(fd, header, (size_t)n) != 0) return -1;
    return body_length ? send_all(fd, body, body_length) : 0;
}

static int read_http_request(Runtime *rt, int fd, HttpRequest *request) {
    memset(request, 0, sizeof(*request));
    size_t cap = AO_HTTP_HEADER_MAX + AO_HTTP_BODY_MAX + 1;
    request->raw = malloc(cap);
    if (!request->raw) return -1;
    struct timespec deadline;
    (void)clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += rt->config.request_timeout_seconds;
    size_t used = 0, expected_total = 0, header_end = 0;
    bool have_header = false;
    for (;;) {
        if (have_header && used == expected_total) break;
        int left = milliseconds_left(&deadline);
        if (left <= 0) goto bad;
        struct pollfd pfd = {.fd=fd,.events=POLLIN};
        int pr = poll(&pfd,1,left);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0 || (pfd.revents & (POLLERR|POLLNVAL))) goto bad;
        char chunk[4096];
        ssize_t got = recv(fd, chunk, sizeof(chunk), 0);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) goto bad;
        if ((size_t)got > cap - used - 1) goto too_large;
        memcpy(request->raw + used, chunk, (size_t)got);
        used += (size_t)got;
        request->raw[used] = '\0';
        if (!have_header) {
            char *marker = NULL;
            for (size_t i=0; i+3<used; ++i)
                if (request->raw[i]=='\r' && request->raw[i+1]=='\n' && request->raw[i+2]=='\r' && request->raw[i+3]=='\n') { marker=request->raw+i; break; }
            if (!marker) { if (used > AO_HTTP_HEADER_MAX) goto too_large; continue; }
            header_end = (size_t)(marker-request->raw);
            if (header_end > AO_HTTP_HEADER_MAX) goto too_large;
            request->header_length = header_end;
            request->raw[header_end] = '\0';
            char *line_end = strstr(request->raw, "\r\n");
            if (!line_end) goto bad;
            *line_end = '\0';
            char version[16];
            if (sscanf(request->raw, "%7s %2047s %15s", request->method, request->target, version) != 3 ||
                (strcmp(version,"HTTP/1.1") != 0 && strcmp(version,"HTTP/1.0") != 0)) goto bad;
            char *line = line_end + 2;
            bool got_host = false, got_token = false, got_length = false;
            while (line < request->raw + header_end) {
                char *next = strstr(line, "\r\n");
                if (!next) next = request->raw + header_end;
                *next = '\0';
                char *colon = strchr(line, ':');
                if (!colon) goto bad;
                *colon++='\0';
                while (*colon==' ' || *colon=='\t') ++colon;
                char *end = colon + strlen(colon);
                while (end > colon && (end[-1]==' ' || end[-1]=='\t')) *--end='\0';
                if (strcasecmp(line,"Host")==0) {
                    if (got_host || strlen(colon)>=sizeof(request->host)) goto bad;
                    strcpy(request->host,colon); got_host=true;
                } else if (strcasecmp(line,"X-AO-Token")==0) {
                    if (got_token || strlen(colon)>=sizeof(request->token)) goto bad;
                    strcpy(request->token,colon); got_token=true;
                } else if (strcasecmp(line,"Content-Length")==0) {
                    if (got_length || !*colon) goto bad;
                    char *ep=NULL; errno=0; unsigned long long value=strtoull(colon,&ep,10);
                    if (errno || !ep || *ep || value > AO_HTTP_BODY_MAX) goto too_large;
                    request->body_length=(size_t)value; request->has_content_length=true; got_length=true;
                } else if (strcasecmp(line,"Transfer-Encoding")==0) request->transfer_encoding=true;
                line = next + 2;
            }
            if (!got_host || request->transfer_encoding) goto bad;
            size_t body_start = header_end + 4;
            if (request->body_length > cap - body_start - 1) goto too_large;
            expected_total = body_start + request->body_length;
            have_header=true;
            if (used > expected_total) goto bad; /* Reject pipelining/ambiguous request framing. */
            if (!strcmp(request->method,"POST") && !request->has_content_length) goto bad;
        }
        if (have_header && used > expected_total) goto bad;
    }
    request->length = used;
    return 0;
too_large:
    free(request->raw); request->raw=NULL; return -2;
bad:
    free(request->raw); request->raw=NULL; return -1;
}

static char *read_static_file(const char *directory, const char *name, size_t *length) {
    char path[AO_MAX_PATH];
    int n=snprintf(path,sizeof(path),"%s/%s",directory,name);
    if (n<0 || (size_t)n>=sizeof(path)) return NULL;
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0) return NULL;
    struct stat st;
    if(fstat(fd,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size<0 || st.st_size>1024*1024) { close(fd); return NULL; }
    size_t cap=(size_t)st.st_size;
    char *data=malloc(cap+1);
    if(!data){close(fd);return NULL;}
    size_t off=0;
    while(off<cap){ssize_t r=read(fd,data+off,cap-off);if(r<0&&errno==EINTR)continue;if(r<=0){free(data);close(fd);return NULL;}off+=(size_t)r;}
    close(fd); data[cap]='\0'; if(length)*length=cap; return data;
}

static bool host_is_loopback_ui(const char *host, int port) {
    char with_port[64];
    (void)snprintf(with_port,sizeof(with_port),"127.0.0.1:%d",port);
    if (strcasecmp(host,"localhost")==0 || strcmp(host,"127.0.0.1")==0 || strcmp(host,with_port)==0) return true;
    (void)snprintf(with_port,sizeof(with_port),"localhost:%d",port);
    return strcasecmp(host,with_port)==0;
}

static const char *http_reason_phrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 422: return "Unprocessable Content";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default: return "Internal Server Error";
    }
}

static void http_reply_json(int fd, int status, json_object *obj) {
    const char *json=obj?json_object_to_json_string_ext(obj,JSON_C_TO_STRING_PLAIN):"{\"ok\":false,\"error\":\"internal error\"}";
    (void)send_http_response(fd,status,http_reason_phrase(status),
                             "application/json; charset=utf-8",json,strlen(json),false);
}

/* Preserve the existing JSON error envelope while exposing transport-level
 * status classes to HTTP clients. Unknown/internal errors remain 500 rather
 * than being reported as successful HTTP 200 responses. */
static int http_status_for_rpc_result(json_object *result) {
    json_object *ok = NULL;
    if (!result || json_object_get_type(result) != json_type_object) return 500;
    /* An absent or malformed success marker is not a successful RPC result. */
    if (!json_object_object_get_ex(result, "ok", &ok) || !ok ||
        json_object_get_type(ok) != json_type_boolean) return 500;
    if (json_object_get_boolean(ok)) return 200;
    json_object *error_value = NULL;
    if (!json_object_object_get_ex(result, "error", &error_value) ||
        !error_value || json_object_get_type(error_value) != json_type_string) return 500;
    const char *error = json_object_get_string(error_value);
    if (!error) return 500;
    if (strstr(error, "unauthorized") || strstr(error, "authentication required")) return 401;
    if (strstr(error, "already terminal") || strstr(error, "not a failed dead letter")) return 409;
    if (strstr(error, "queue is full") || strstr(error, "queue capacity reached")) return 429;
    if (strstr(error, "not found") || strstr(error, "does not exist") || strstr(error, "no task matches")) return 404;
    if (strstr(error, "required") || strstr(error, "invalid") || strstr(error, "must be") ||
        strstr(error, "must not") || strstr(error, "exceeds ") || strstr(error, "too long") ||
        strstr(error, "unsupported op") || strstr(error, "unknown agent") ||
        strstr(error, "unknown plan scope") || strstr(error, "path is unsafe") ||
        strstr(error, "limit must") || strstr(error, "offset must") || strstr(error, "priority must")) return 400;
    return 500;
}

static json_object *parse_http_json_body(const HttpRequest *req) {
    if (!req || !req->raw || req->body_length == 0 || req->body_length > INT_MAX) return NULL;
    size_t body_offset = req->header_length + 4;
    if (memchr(req->raw + body_offset, '\0', req->body_length) != NULL) return NULL;
    json_tokener *tok = json_tokener_new();
    if (!tok) return NULL;
    json_tokener_set_flags(tok, JSON_TOKENER_STRICT);
    json_object *body = json_tokener_parse_ex(tok, req->raw + body_offset, (int)req->body_length);
    bool valid = body && json_tokener_get_error(tok) == json_tokener_success &&
                 json_object_get_type(body) == json_type_object;
    if (valid) {
        size_t end = (size_t)json_tokener_get_parse_end(tok);
        for (size_t i = end; i < req->body_length; ++i)
            if (!isspace((unsigned char)req->raw[body_offset + i])) valid = false;
    }
    json_tokener_free(tok);
    if (!valid) { if (body) json_object_put(body); return NULL; }
    return body;
}

static bool parse_model_action_path(const char *path, bool *decision, char *id, size_t id_cap, bool *start) {
    const char *prefix = "/api/models/";
    if (!path || strncmp(path, prefix, strlen(prefix)) != 0) return false;
    const char *p = path + strlen(prefix);
    if (strncmp(p, "llm/", 4) == 0) { *decision = false; p += 4; }
    else if (strncmp(p, "decision/", 9) == 0) { *decision = true; p += 9; }
    else return false;
    const char *slash = strchr(p, '/');
    if (!slash || slash == p) return false;
    size_t n = (size_t)(slash - p);
    if (n >= id_cap) return false;
    for (size_t i = 0; i < n; ++i)
        if (!(isalnum((unsigned char)p[i]) || p[i] == '_' || p[i] == '-')) return false;
    memcpy(id, p, n); id[n] = '\0';
    if (strcmp(slash, "/start") == 0) *start = true;
    else if (strcmp(slash, "/stop") == 0) *start = false;
    else return false;
    return true;
}

static bool parse_model_enabled_path(const char *path, bool *decision, char *id, size_t id_cap, bool *enabled) {
    const char *prefix = "/api/models/";
    if (!path || strncmp(path, prefix, strlen(prefix)) != 0) return false;
    const char *p = path + strlen(prefix);
    if (strncmp(p, "llm/", 4) == 0) { *decision = false; p += 4; }
    else if (strncmp(p, "decision/", 9) == 0) { *decision = true; p += 9; }
    else return false;
    const char *slash = strchr(p, '/');
    if (!slash || slash == p) return false;
    size_t n = (size_t)(slash - p);
    if (n >= id_cap) return false;
    for (size_t i = 0; i < n; ++i) if (!(isalnum((unsigned char)p[i]) || p[i] == '_' || p[i] == '-')) return false;
    memcpy(id, p, n); id[n] = '\0';
    if (strcmp(slash, "/enable") == 0) *enabled = true;
    else if (strcmp(slash, "/disable") == 0) *enabled = false;
    else return false;
    return true;
}

static void http_query_param(const char *query, const char *key, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0'; if (!query || !key || !*key) return;
    size_t key_len = strlen(key); const char *p = query;
    while (*p) {
        while (*p == '&' || *p == '?') ++p;
        const char *eq = strchr(p, '='); if (!eq) break;
        const char *end = strchr(p, '&'); if (!end) end = p + strlen(p);
        if ((size_t)(eq - p) == key_len && strncmp(p, key, key_len) == 0) {
            size_t n = (size_t)(end - eq - 1); if (n >= out_size) n = out_size - 1;
            memcpy(out, eq + 1, n); out[n] = '\0'; return;
        }
        p = *end ? end + 1 : end;
    }
}

static void http_handle_connection(Runtime *rt, int fd) {
    HttpRequest req;
    int read_rc=read_http_request(rt,fd,&req);
    if(read_rc!=0){const char *message=read_rc==-2?"{\"ok\":false,\"error\":\"request too large\"}":"{\"ok\":false,\"error\":\"malformed HTTP request\"}";
        (void)send_http_response(fd,read_rc==-2?413:400,read_rc==-2?"Payload Too Large":"Bad Request","application/json; charset=utf-8",message,strlen(message),false);return;}
    const char *query=strchr(req.target,'?');
    size_t path_len=query?(size_t)(query-req.target):strlen(req.target);
    char path[2048]; if(path_len>=sizeof(path)){free(req.raw);return;} memcpy(path,req.target,path_len);path[path_len]='\0';
    if(!host_is_loopback_ui(req.host,rt->config.ui_port)){
        const char *msg="{\"ok\":false,\"error\":\"invalid Host header\"}";
        (void)send_http_response(fd,400,"Bad Request","application/json; charset=utf-8",msg,strlen(msg),false);
        free(req.raw);return;
    }
    if(strcmp(req.method,"GET")==0 && (
        strcmp(path,"/")==0 || strcmp(path,"/index.html")==0 ||
        strcmp(path,"/assets/app.js")==0 || strcmp(path,"/assets/style.css")==0 ||
        strcmp(path,"/manifest.webmanifest")==0 || strcmp(path,"/sw.js")==0 ||
        strcmp(path,"/icon-192.png")==0 || strcmp(path,"/icon-512.png")==0)) {
        const char *file = strcmp(path,"/assets/app.js")==0 ? "app.js" :
                           strcmp(path,"/assets/style.css")==0 ? "style.css" :
                           strcmp(path,"/manifest.webmanifest")==0 ? "manifest.webmanifest" :
                           strcmp(path,"/sw.js")==0 ? "sw.js" :
                           strcmp(path,"/icon-192.png")==0 ? "icon-192.png" :
                           strcmp(path,"/icon-512.png")==0 ? "icon-512.png" : "index.html";
        const char *type = strcmp(file,"app.js")==0 ? "text/javascript; charset=utf-8" :
                           strcmp(file,"style.css")==0 ? "text/css; charset=utf-8" :
                           strcmp(file,"manifest.webmanifest")==0 ? "application/manifest+json; charset=utf-8" :
                           strcmp(file,"sw.js")==0 ? "application/javascript; charset=utf-8" :
                           strcmp(file,"icon-192.png")==0 || strcmp(file,"icon-512.png")==0 ? "image/png" : "text/html; charset=utf-8";
        size_t length=0; char *contents=read_static_file(rt->config.ui_dir,file,&length);
        if(!contents){const char *msg="UI assets unavailable; check ui.directory";(void)send_http_response(fd,404,"Not Found","text/plain; charset=utf-8",msg,strlen(msg),false);}
        else {(void)send_http_response(fd,200,"OK",type,contents,length,false);free(contents);}
        free(req.raw);return;
    }
    if(strcmp(req.target,"/favicon.ico")==0){(void)send_http_response(fd,404,"Not Found","text/plain","",0,false);free(req.raw);return;}
    if(!constant_time_equal(req.token,rt->config.ui_auth_token)) {
        const char *msg="{\"ok\":false,\"error\":\"unauthorized\"}";
        (void)send_http_response(fd,401,"Unauthorized","application/json; charset=utf-8",msg,strlen(msg),false);free(req.raw);return;
    }
    if (strcmp(req.method, "GET") == 0 && strcmp(path, "/api/configs") == 0) {
        char config_err[1024] = {0};
        json_object *result = config_editor_get(rt, config_err, sizeof(config_err));
        if (!result) { json_object *failure = make_reply(false, config_err[0] ? config_err : "cannot read configurations"); http_reply_json(fd, 500, failure); json_object_put(failure); }
        else { http_reply_json(fd, 200, result); json_object_put(result); }
        free(req.raw); return;
    }
    if (strcmp(req.method, "PUT") == 0 && strcmp(path, "/api/configs") == 0) {
        json_object *body = parse_http_json_body(&req);
        char config_err[1024] = {0};
        if (!body) { json_object *failure = make_reply(false, "request body must be a single JSON object"); http_reply_json(fd, 400, failure); json_object_put(failure); }
        else if (config_editor_save(rt, body, config_err, sizeof(config_err)) != 0) {
            json_object *failure = make_reply(false, config_err[0] ? config_err : "configuration validation failed"); http_reply_json(fd, 400, failure); json_object_put(failure); json_object_put(body);
        } else {
            json_object *saved = make_reply(true, NULL);
            uint64_t current_fingerprint = config_editor_fingerprint(rt);
            bool restart_required = rt->active_config_fingerprint == 0 || current_fingerprint == 0 || current_fingerprint != rt->active_config_fingerprint;
            json_object_object_add(saved, "restart_required", json_object_new_boolean(restart_required));
            http_reply_json(fd, 200, saved); json_object_put(saved); json_object_put(body);
        }
        free(req.raw); return;
    }
    if (strcmp(req.method, "GET") == 0 && strcmp(path, "/api/resources") == 0) {
        json_object *result = resources_status_json(rt);
        if (!result) { json_object *failure = make_reply(false, "cannot read resource catalog"); http_reply_json(fd, 500, failure); json_object_put(failure); }
        else { http_reply_json(fd, 200, result); json_object_put(result); }
        free(req.raw); return;
    }
    if (strcmp(req.method, "GET") == 0 && strcmp(path, "/api/models") == 0) {
        json_object *result = models_status_json(rt);
        if (!result) { json_object *failure = make_reply(false, "cannot read model status"); http_reply_json(fd, 500, failure); json_object_put(failure); }
        else { json_object_object_add(result, "ok", json_object_new_boolean(true)); http_reply_json(fd, 200, result); json_object_put(result); }
        free(req.raw); return;
    }
    if (strcmp(req.method, "POST") == 0) {
        bool decision = false, start = false, enabled = false; char model_id[AO_MAX_ID]; char model_err[1024] = {0};
        int action_rc = 1;
        if (parse_model_action_path(path, &decision, model_id, sizeof(model_id), &start))
            action_rc = start ? model_start(rt, model_id, decision, model_err, sizeof(model_err)) : model_stop(rt, model_id, decision, model_err, sizeof(model_err));
        else if (parse_model_enabled_path(path, &decision, model_id, sizeof(model_id), &enabled))
            action_rc = model_set_enabled(rt, model_id, decision, enabled, model_err, sizeof(model_err));
        if (action_rc != 1) {
            if (action_rc != 0) { json_object *failure = make_reply(false, model_err[0] ? model_err : "model action failed"); http_reply_json(fd, 400, failure); json_object_put(failure); }
            else { json_object *ok = make_reply(true, NULL); json_object_object_add(ok, "models", models_status_json(rt)); http_reply_json(fd, 200, ok); json_object_put(ok); }
            free(req.raw); return;
        }
    }
    json_object *command=NULL;
    /* A recognized endpoint with an invalid body/action is a client error,
       not an unknown route. Unknown paths continue to return 404. */
    int route_failure_status = 404;
    if (strcmp(req.method, "GET") == 0 && strcmp(path, "/api/plan") == 0) {
        command = json_object_new_object(); json_object_object_add(command, "op", json_object_new_string("plan_list"));
        char scope[AO_MAX_ID] = "", kind[16] = "";
        http_query_param(query, "scope_id", scope, sizeof(scope));
        http_query_param(query, "kind", kind, sizeof(kind));
        if (scope[0]) json_object_object_add(command, "scope_id", json_object_new_string(scope));
        if (kind[0]) json_object_object_add(command, "kind", json_object_new_string(kind));
    } else if (strcmp(req.method, "POST") == 0 && strcmp(path, "/api/plan") == 0) {
        json_object *body = parse_http_json_body(&req);
        if (body && json_object_get_type(body) == json_type_object) {
            const char *action = NULL; (void)json_get_string(body, "action", &action);
            const char *operation = action && strcmp(action, "add") == 0 ? "plan_add" :
                                   action && strcmp(action, "update") == 0 ? "plan_update" :
                                   action && strcmp(action, "delete") == 0 ? "plan_delete" : NULL;
            if (operation) { command = body; json_object_object_del(command, "action"); json_object_object_add(command, "op", json_object_new_string(operation)); }
            else { json_object_put(body); route_failure_status = 400; }
        } else { if (body) json_object_put(body); route_failure_status = 400; }
    } else if(strcmp(req.method,"GET")==0 && strcmp(path,"/api/health")==0) {
        command=json_object_new_object();json_object_object_add(command,"op",json_object_new_string("health"));
    } else if(strcmp(req.method,"GET")==0 && strcmp(path,"/api/agents")==0) {
        command=json_object_new_object();json_object_object_add(command,"op",json_object_new_string("list_agents"));
    } else if(strcmp(req.method,"GET")==0 && strcmp(path,"/api/tasks")==0) {
        command=json_object_new_object();json_object_object_add(command,"op",json_object_new_string("list_tasks"));
        json_object_object_add(command,"limit",json_object_new_int(100));
        if(query){const char *st=strstr(query,"status=");if(st){st+=7;char value[64];size_t n=0;while(st[n]&&st[n]!='&'&&n<sizeof(value)-1){value[n]=st[n];n++;}value[n]='\0';if(n)json_object_object_add(command,"status",json_object_new_string(value));}}
    } else if(strcmp(req.method,"GET")==0 && strncmp(path,"/api/tasks/",11)==0 && path[11]) {
        const char *tail=path+11; size_t tail_len=strlen(tail); const char events_suffix[]="/events";
        bool events_route=tail_len>sizeof(events_suffix)-1 && strcmp(tail+tail_len-(sizeof(events_suffix)-1),events_suffix)==0;
        size_t id_len=events_route?tail_len-(sizeof(events_suffix)-1):tail_len;
        bool valid=id_len>0 && id_len<AO_MAX_ID;
        for(size_t i=0;valid&&i<id_len;i++)if(!(isalnum((unsigned char)tail[i])||tail[i]=='-'||tail[i]=='_'))valid=false;
        if(valid){
            char task_id[AO_MAX_ID]; memcpy(task_id,tail,id_len); task_id[id_len]='\0';
            command=json_object_new_object();json_object_object_add(command,"op",json_object_new_string(events_route?"events":"status"));json_object_object_add(command,"task_id",json_object_new_string(task_id));
            if(events_route && query){
                const char *after=strstr(query,"after_id="); if(after){after+=8;char value[32];size_t n=0;while(after[n]&&after[n]!='&'&&n<sizeof(value)-1){value[n]=after[n];n++;}value[n]='\0';if(n){char *end=NULL;long parsed=strtol(value,&end,10);if(end&&*end=='\0'&&parsed>=0&&parsed<=INT32_MAX)json_object_object_add(command,"after_id",json_object_new_int((int)parsed));}}
                const char *limit=strstr(query,"limit="); if(limit){limit+=6;char value[32];size_t n=0;while(limit[n]&&limit[n]!='&'&&n<sizeof(value)-1){value[n]=limit[n];n++;}value[n]='\0';if(n){char *end=NULL;long parsed=strtol(value,&end,10);if(end&&*end=='\0'&&parsed>0&&parsed<=500)json_object_object_add(command,"limit",json_object_new_int((int)parsed));}}
            }
        }
    } else if(strcmp(req.method,"POST")==0 && strcmp(path,"/api/tasks")==0) {
        json_tokener *tok=json_tokener_new();
        size_t body_offset=req.header_length+4;
        if(tok)json_tokener_set_flags(tok,JSON_TOKENER_STRICT);
        json_object *body=tok?json_tokener_parse_ex(tok,req.raw+body_offset,(int)req.body_length):NULL;
        bool valid=body && tok && json_tokener_get_error(tok)==json_tokener_success && json_object_get_type(body)==json_type_object;
        if(valid) {
            size_t parse_end=json_tokener_get_parse_end(tok);
            for(size_t i=parse_end;i<req.body_length;i++) if(!isspace((unsigned char)req.raw[body_offset+i])) valid=false;
            if(memchr(req.raw+body_offset,'\0',req.body_length)!=NULL) valid=false;
            if(!valid){json_object_put(body);body=NULL;}
        }
        if(tok)json_tokener_free(tok);
        if(valid) {
            command=body;
            json_object_object_del(command,"op");
            json_object_object_add(command,"op",json_object_new_string("submit"));
        } else { if(body)json_object_put(body); route_failure_status = 400; }
    } else if(strcmp(req.method,"POST")==0 && strncmp(path,"/api/tasks/",11)==0) {
        const char *tail=path+11;size_t n=strlen(tail);const char cancel_suffix[]="/cancel", answer_suffix[]="/answer", files_suffix[]="/files";
        bool answer_route=n>sizeof(answer_suffix)-1 && strcmp(tail+n-(sizeof(answer_suffix)-1),answer_suffix)==0;
        bool cancel_route=n>sizeof(cancel_suffix)-1 && strcmp(tail+n-(sizeof(cancel_suffix)-1),cancel_suffix)==0;
        bool files_route=n>sizeof(files_suffix)-1 && strcmp(tail+n-(sizeof(files_suffix)-1),files_suffix)==0;
        size_t suffix_len=answer_route?sizeof(answer_suffix)-1:(cancel_route?sizeof(cancel_suffix)-1:(files_route?sizeof(files_suffix)-1:0));
        if(suffix_len){
            size_t id_len=n-suffix_len;char task_id[AO_MAX_ID];bool valid=id_len>0&&id_len<sizeof(task_id);
            for(size_t i=0;valid&&i<id_len;i++)if(!(isalnum((unsigned char)tail[i])||tail[i]=='-'||tail[i]=='_'))valid=false;
            if(valid){
                memcpy(task_id,tail,id_len);task_id[id_len]='\0';
                if(answer_route || files_route){
                    json_object *body=parse_http_json_body(&req);
                    if(body&&json_object_get_type(body)==json_type_object){
                        command=body;json_object_object_del(command,"op");json_object_object_add(command,"task_id",json_object_new_string(task_id));
                        if(answer_route) json_object_object_add(command,"op",json_object_new_string("answer"));
                        else {
                            const char *action=NULL;
                            if(json_get_string(command,"action",&action)==0 && action){
                                const char *operation = strcmp(action,"list")==0 ? "workspace_list" :
                                                        strcmp(action,"read")==0 ? "workspace_read" :
                                                        strcmp(action,"write")==0 ? "workspace_write" :
                                                        strcmp(action,"read_bytes")==0 ? "workspace_read_bytes" :
                                                        strcmp(action,"artifact_list")==0 ? "artifact_list" : NULL;
                                if(operation) json_object_object_add(command,"op",json_object_new_string(operation));
                                else { json_object_put(command); command=NULL; }
                            } else { json_object_put(command); command=NULL; }
                        }
                    } else { if(body)json_object_put(body); route_failure_status = 400; }
                }else{command=json_object_new_object();json_object_object_add(command,"op",json_object_new_string("cancel"));json_object_object_add(command,"task_id",json_object_new_string(task_id));}
            }
        }
    }
    if(!command){
        int status = route_failure_status;
        const char *reason = status == 400 ? "Bad Request" : "Not Found";
        const char *msg = status == 400
            ? "{\"ok\":false,\"error\":\"invalid request body or action\"}"
            : "{\"ok\":false,\"error\":\"unknown route\"}";
        (void)send_http_response(fd,status,reason,"application/json; charset=utf-8",msg,strlen(msg),false);
        free(req.raw);return;
    }
    if(rt->config.auth_token[0]) json_object_object_add(command,"auth_token",json_object_new_string(rt->config.auth_token));
    json_object *result=server_handle_request(rt,command);
    json_object_put(command);
    http_reply_json(fd, http_status_for_rpc_result(result), result);
    if(result)json_object_put(result);
    free(req.raw);
}

static int create_unix_listener(Runtime *rt, char *err, size_t err_size) {
    if(!rt->config.unix_socket_enabled)return -1;
    struct sockaddr_un addr;memset(&addr,0,sizeof(addr));addr.sun_family=AF_UNIX;
    if(strlen(rt->config.unix_socket_path)>=sizeof(addr.sun_path)){if(err&&err_size)snprintf(err,err_size,"Unix socket path exceeds sun_path limit");return -1;}
    strcpy(addr.sun_path,rt->config.unix_socket_path);
    char parent[AO_MAX_PATH];
    if(string_copy(parent,sizeof(parent),addr.sun_path)!=0){if(err&&err_size)snprintf(err,err_size,"Unix socket path too long");return -1;}
    char *last_slash=strrchr(parent,'/');
    if(last_slash){if(last_slash==parent)last_slash[1]='\0';else *last_slash='\0';
        if(path_mkdirs(parent,0700)!=0){if(err&&err_size)snprintf(err,err_size,"cannot prepare Unix socket directory: %s",strerror(errno));return -1;}
        struct stat parent_st;
        if(stat(parent,&parent_st)!=0 || !S_ISDIR(parent_st.st_mode) || parent_st.st_uid!=geteuid() || (parent_st.st_mode&0022)!=0){
            if(err&&err_size) snprintf(err,err_size,"Unix socket directory must be owned by this UID and not group/world writable");
            return -1;
        }
    }
    struct stat st;
    if(lstat(addr.sun_path,&st)==0){
        if(!S_ISSOCK(st.st_mode)||st.st_uid!=geteuid()){if(err&&err_size)snprintf(err,err_size,"refusing to replace existing non-owned Unix socket path");return -1;}
        if(unlink(addr.sun_path)!=0){if(err&&err_size)snprintf(err,err_size,"cannot unlink stale Unix socket: %s",strerror(errno));return -1;}
    } else if(errno!=ENOENT){if(err&&err_size)snprintf(err,err_size,"cannot inspect Unix socket path: %s",strerror(errno));return -1;}
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
    if(fd<0){if(err&&err_size)snprintf(err,err_size,"cannot create Unix socket: %s",strerror(errno));return -1;}
    if(bind(fd,(struct sockaddr*)&addr,sizeof(addr))!=0 || chmod(addr.sun_path,0600)!=0 || listen(fd,128)!=0){int saved=errno;close(fd);unlink(addr.sun_path);errno=saved;if(err&&err_size)snprintf(err,err_size,"cannot bind secure Unix socket: %s",strerror(errno));return -1;}
    int flags=fcntl(fd,F_GETFL,0);if(flags>=0)(void)fcntl(fd,F_SETFL,flags|O_NONBLOCK);
    return fd;
}

static int create_ui_listener(Runtime *rt, char *err, size_t err_size) {
    if(!rt->config.ui_enabled)return -1;
    int fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);
    if(fd<0){if(err&&err_size)snprintf(err,err_size,"cannot create UI socket: %s",strerror(errno));return -1;}
    struct sockaddr_in addr;memset(&addr,0,sizeof(addr));addr.sin_family=AF_INET;addr.sin_port=htons((uint16_t)rt->config.ui_port);addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    int one=1;(void)setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    if(bind(fd,(struct sockaddr*)&addr,sizeof(addr))!=0 || listen(fd,64)!=0){int saved=errno;close(fd);errno=saved;if(err&&err_size)snprintf(err,err_size,"cannot bind loopback UI on port %d: %s",rt->config.ui_port,strerror(errno));return -1;}
    int flags=fcntl(fd,F_GETFL,0);if(flags>=0)(void)fcntl(fd,F_SETFL,flags|O_NONBLOCK);
    return fd;
}

static bool unix_peer_is_same_uid(int fd) {
#ifdef SO_PEERCRED
    struct ucred cred;socklen_t n=sizeof(cred);
    return getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&cred,&n)==0 && cred.uid==geteuid();
#else
    (void)fd;return false;
#endif
}

static void accept_ready_connections(Runtime *rt, int listen_fd, bool http_mode, bool unix_mode) {
    for(;;) {
        struct sockaddr_storage peer;socklen_t peer_len=sizeof(peer);
        int client=accept4(listen_fd,(struct sockaddr*)&peer,&peer_len,SOCK_CLOEXEC);
        if(client<0&&errno==EINTR)continue;
        if(client<0&&(errno==EAGAIN||errno==EWOULDBLOCK))break;
        if(client<0){if(errno!=EBADF&&errno!=EINVAL)fprintf(stderr,"accept: %s\n",strerror(errno));break;}
        if(unix_mode&&!unix_peer_is_same_uid(client)){const char *denied="{\"ok\":false,\"error\":\"Unix socket peer UID is not permitted\"}\n";(void)send_all(client,denied,strlen(denied));close(client);continue;}
        pthread_mutex_lock(&rt->connections_mutex);
        bool full=rt->active_connections>=rt->config.max_connections;
        if(!full&&rt->active_connections<(int)(sizeof(rt->connection_fds)/sizeof(rt->connection_fds[0]))){rt->connection_fds[rt->active_connections]=client;++rt->active_connections;}else full=true;
        pthread_mutex_unlock(&rt->connections_mutex);
        if(full){const char *busy="{\"ok\":false,\"error\":\"connection limit reached\"}\n";(void)send_all(client,busy,strlen(busy));close(client);continue;}
        ConnectionArgs *args=malloc(sizeof(*args));
        if(!args){connection_release(rt,client);close(client);continue;}
        args->runtime=rt;args->fd=client;args->http_mode=http_mode;
        pthread_t thread;int rc=pthread_create(&thread,NULL,connection_main,args);
        if(rc!=0){free(args);connection_release(rt,client);close(client);continue;}
        (void)pthread_detach(thread);
    }
}

static void *connection_main(void *arg) {
    ConnectionArgs *args = arg;
    Runtime *rt = args->runtime;
    int fd = args->fd;
    bool http_mode = args->http_mode;
    free(args);
    struct timeval send_timeout = {.tv_sec = 5, .tv_usec = 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
    if(http_mode) {
        http_handle_connection(rt,fd);
        connection_release(rt,fd);shutdown(fd,SHUT_RDWR);close(fd);return NULL;
    }
    char *line = read_request_line(rt, fd);
    json_object *reply = NULL;
    if (!line) {
        reply = make_reply(false, "request timeout, empty request, or request too large");
    } else {
        json_tokener *tok = json_tokener_new();
        size_t line_length = strlen(line);
        json_object *request = tok ? json_tokener_parse_ex(tok, line, (int)line_length) : NULL;
        enum json_tokener_error jerr = tok ? json_tokener_get_error(tok) : json_tokener_error_parse_unexpected;
        size_t parse_end = tok ? json_tokener_get_parse_end(tok) : 0;
        bool trailing_ok = true;
        for (size_t i = parse_end; i < line_length; ++i) if (!isspace((unsigned char)line[i])) { trailing_ok = false; break; }
        bool utf8_ok = valid_utf8((const unsigned char *)line, line_length);
        if (!request || jerr != json_tokener_success || !trailing_ok || !utf8_ok) {
            reply = make_reply(false, "invalid JSON request");
            if (request) json_object_put(request);
        } else {
            reply = server_handle_request(rt, request);
            json_object_put(request);
        }
        if (tok) json_tokener_free(tok);
    }
    if (reply) { (void)send_json(fd, reply); json_object_put(reply); }
    free(line);
    connection_release(rt, fd);
    shutdown(fd, SHUT_RDWR);
    close(fd);
    return NULL;
}

void *worker_main(void *arg) {
    Runtime *rt = arg;
    for (;;) {
        if (rt->stopping) break;
        ClaimedTask claim = {0};
        char db_err[512] = {0};
        int got = db_claim_next(rt, &claim, db_err, sizeof(db_err));
        if (got < 0) {
            fprintf(stderr, "worker: %s\n", db_err);
            struct timespec pause = {.tv_sec = 0, .tv_nsec = 250000000L};
            (void)nanosleep(&pause, NULL);
            continue;
        }
        if (got == 0) {
            pthread_mutex_lock(&rt->queue_mutex);
            if (!rt->stopping) {
                struct timespec until;
                (void)clock_gettime(CLOCK_REALTIME, &until);
                until.tv_sec += 1;
                (void)pthread_cond_timedwait(&rt->queue_cond, &rt->queue_mutex, &until);
            }
            pthread_mutex_unlock(&rt->queue_mutex);
            continue;
        }
        if (rt->stopping) {
            char release_err[512] = {0};
            if (db_release_claim(rt, claim.id, claim.attempt, release_err, sizeof(release_err)) < 0)
                fprintf(stderr, "worker could not release claim for %s during shutdown: %s\n", claim.id, release_err);
            db_claim_free(&claim);
            break;
        }
        if (db_task_cancelled(rt, claim.id)) {
            bool ignored_retry = false;
            char cancelled_err[256] = {0};
            (void)db_finish(rt, claim.id, claim.attempt, false, NULL, "task cancelled before execution",
                            &ignored_retry, cancelled_err, sizeof(cancelled_err));
            db_claim_free(&claim);
            continue;
        }
        char *result_json = NULL;
        char status[64] = {0};
        char task_err[2048] = {0};
        bool success = false;
        int rc = orchestrator_execute(rt, claim.id, claim.task_json, &result_json, &success,
                                      status, sizeof(status), task_err, sizeof(task_err));
        if (rc != 0) success = false;
        /* Cancellation may interrupt an in-flight streamed LLM request before the
           orchestrator has allocated its normal result object. Persist a stable
           cancellation result even in that early-exit path. */
        if (db_task_cancelled(rt, claim.id)) {
            char cancelled_json[256];
            (void)snprintf(cancelled_json, sizeof(cancelled_json),
                           "{\"status\":\"cancelled\",\"task_id\":\"%s\"}", claim.id);
            free(result_json);
            result_json = strdup(cancelled_json);
            success = true;
            status[0] = '\0';
            task_err[0] = '\0';
        }
        if (!success && task_err[0] && !db_task_cancelled(rt, claim.id))
            orchestrator_diagnose_failure(rt, claim.id, claim.task_json, task_err);
        bool retry = false;
        char finish_err[512] = {0};
        if (db_finish(rt, claim.id, claim.attempt, success, result_json,
                      success ? NULL : (task_err[0] ? task_err : "task execution failed"),
                      &retry, finish_err, sizeof(finish_err)) != 0)
            fprintf(stderr, "worker could not persist result for %s: %s\n", claim.id, finish_err);
        if (retry) {
            pthread_mutex_lock(&rt->queue_mutex); pthread_cond_signal(&rt->queue_cond); pthread_mutex_unlock(&rt->queue_mutex);
        }
        if (task_err[0] && !success) fprintf(stderr, "task %s attempt %d failed: %s\n", claim.id, claim.attempt, task_err);
        free(result_json);
        db_claim_free(&claim);
    }
    return NULL;
}

static int create_listener(Runtime *rt, char *err, size_t err_size) {
    char port[16]; (void)snprintf(port, sizeof(port), "%d", rt->config.port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    struct addrinfo *results = NULL;
    int gai = getaddrinfo(rt->config.host, port, &hints, &results);
    if (gai != 0) { if (err && err_size) (void)snprintf(err, err_size, "getaddrinfo: %s", gai_strerror(gai)); return -1; }
    int fd = -1;
    for (struct addrinfo *ai = results; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) continue;
        int one = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && listen(fd, 128) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(results);
    if (fd < 0) { if (err && err_size) (void)snprintf(err, err_size, "cannot bind/listen on %s:%d: %s", rt->config.host, rt->config.port, strerror(errno)); return -1; }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

void server_request_stop(Runtime *rt) { if (rt) rt->stopping = 1; }

int server_run(Runtime *rt, char *err, size_t err_size) {
    if (db_recover_running(rt, err, err_size) != 0) return -1;
    int tcp_fd=create_listener(rt,err,err_size);if(tcp_fd<0)return -1;
    rt->listen_fd=tcp_fd;
    int unix_fd=create_unix_listener(rt,err,err_size);
    if(rt->config.unix_socket_enabled && unix_fd<0){close(tcp_fd);rt->listen_fd=-1;return -1;}
    rt->unix_listen_fd=unix_fd;
    int ui_fd=create_ui_listener(rt,err,err_size);
    if(rt->config.ui_enabled && ui_fd<0){close(tcp_fd);if(unix_fd>=0)close(unix_fd);rt->listen_fd=-1;rt->unix_listen_fd=-1;return -1;}
    rt->ui_listen_fd=ui_fd;
    rt->worker_count=rt->config.workers;
    rt->workers=calloc((size_t)rt->worker_count,sizeof(*rt->workers));
    if(!rt->workers){if(err&&err_size)snprintf(err,err_size,"out of memory creating workers");goto startup_fail;}
    int started=0;
    for(int i=0;i<rt->worker_count;++i){int rc=pthread_create(&rt->workers[i],NULL,worker_main,rt);if(rc!=0){if(err&&err_size)snprintf(err,err_size,"cannot start worker %d: %s",i,strerror(rc));rt->stopping=1;break;}++started;}
    if(started!=rt->worker_count){pthread_mutex_lock(&rt->queue_mutex);pthread_cond_broadcast(&rt->queue_cond);pthread_mutex_unlock(&rt->queue_mutex);for(int i=0;i<started;++i)pthread_join(rt->workers[i],NULL);free(rt->workers);rt->workers=NULL;goto startup_fail;}
    fprintf(stderr,"agent-orchestrator-c TCP %s:%d; workers=%d; priority-aging=%ds\n",rt->config.host,rt->config.port,rt->worker_count,rt->config.priority_aging_seconds);
    if(unix_fd>=0)fprintf(stderr,"secure Unix socket: %s (mode 0600; same-UID peers only)\n",rt->config.unix_socket_path);
    if(ui_fd>=0)fprintf(stderr,"local web UI: http://127.0.0.1:%d/ (token required)\n",rt->config.ui_port);
    if(rt->config.sandbox_enabled)fprintf(stderr,"MCP sandbox: rlimits + no_new_privs + seccomp; Landlock %s; network %s\n",
        rt->config.sandbox_require_landlock?"required":"best-effort",rt->config.sandbox_allow_network?"allowed":"denied");

    while(!rt->stopping){
        struct pollfd fds[3];int map[3];nfds_t count=0;
        if(tcp_fd>=0){fds[count]=(struct pollfd){.fd=tcp_fd,.events=POLLIN};map[count++]=0;}
        if(unix_fd>=0){fds[count]=(struct pollfd){.fd=unix_fd,.events=POLLIN};map[count++]=1;}
        if(ui_fd>=0){fds[count]=(struct pollfd){.fd=ui_fd,.events=POLLIN};map[count++]=2;}
        int pr=poll(fds,count,250);
        if(pr<0&&errno==EINTR)continue;
        if(pr<0){if(err&&err_size)snprintf(err,err_size,"poll listeners: %s",strerror(errno));rt->stopping=1;break;}
        if(pr==0)continue;
        if (rt->stopping) break;
        for(nfds_t i=0;i<count;++i){
            if (rt->stopping) break;
            if(fds[i].revents&(POLLERR|POLLNVAL)){if(err&&err_size)snprintf(err,err_size,"listener poll error");rt->stopping=1;break;}
            if(fds[i].revents&POLLIN)accept_ready_connections(rt,fds[i].fd,map[i]==2,map[i]==1);
        }
    }
    rt->stopping=1;
    fprintf(stderr, "shutdown: draining active workers; queued tasks remain durable for next startup\n");
    shutdown_active_connections(rt);
    pthread_mutex_lock(&rt->queue_mutex);pthread_cond_broadcast(&rt->queue_cond);pthread_mutex_unlock(&rt->queue_mutex);
    if (rt->plan_sync_initialized) {
        pthread_mutex_lock(&rt->plan_mutex);
        pthread_cond_broadcast(&rt->plan_cond);
        pthread_mutex_unlock(&rt->plan_mutex);
    }
    if(tcp_fd>=0){shutdown(tcp_fd,SHUT_RDWR);close(tcp_fd);}if(unix_fd>=0){shutdown(unix_fd,SHUT_RDWR);close(unix_fd);}if(ui_fd>=0){shutdown(ui_fd,SHUT_RDWR);close(ui_fd);}
    rt->listen_fd=-1;rt->unix_listen_fd=-1;rt->ui_listen_fd=-1;
    if(unix_fd>=0){struct stat st;if(lstat(rt->config.unix_socket_path,&st)==0&&S_ISSOCK(st.st_mode)&&st.st_uid==geteuid())(void)unlink(rt->config.unix_socket_path);}
    for(int i=0;i<started;++i)pthread_join(rt->workers[i],NULL);
    free(rt->workers);rt->workers=NULL;rt->worker_count=0;
    for(;;){pthread_mutex_lock(&rt->connections_mutex);int active=rt->active_connections;pthread_mutex_unlock(&rt->connections_mutex);if(active==0)break;struct timespec pause={.tv_sec=0,.tv_nsec=20000000L};(void)nanosleep(&pause,NULL);}
    return 0;
startup_fail:
    if(tcp_fd>=0)close(tcp_fd);
    if(unix_fd>=0)close(unix_fd);
    if(ui_fd>=0)close(ui_fd);
    rt->listen_fd=-1;rt->unix_listen_fd=-1;rt->ui_listen_fd=-1;
    if(unix_fd>=0){struct stat st;if(lstat(rt->config.unix_socket_path,&st)==0&&S_ISSOCK(st.st_mode)&&st.st_uid==geteuid())(void)unlink(rt->config.unix_socket_path);}
    return -1;
}
