#include "runtime.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define AO_LSP_MESSAGE_MAX (4U * 1024U * 1024U)
#define AO_LSP_HEADER_MAX 8192U
#define AO_LSP_FILE_MAX (5U * 1024U * 1024U)
#define AO_LSP_URI_MAX (AO_MAX_PATH * 3U + 32U)

typedef struct LSPDocument {
    char uri[AO_LSP_URI_MAX];
    int version;
    struct LSPDocument *next;
} LSPDocument;

static void set_error(char *dst, size_t cap, const char *message) {
    if (dst && cap) (void)snprintf(dst, cap, "%s", message ? message : "LSP error");
}

static int remaining_ms(const struct timespec *deadline) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    long long ns = ((long long)deadline->tv_sec - now.tv_sec) * 1000000000LL +
                   (long long)deadline->tv_nsec - now.tv_nsec;
    if (ns <= 0) return 0;
    long long ms = (ns + 999999LL) / 1000000LL;
    return ms > INT_MAX ? INT_MAX : (int)ms;
}

static int grow_buffer(LSPServer *server, size_t need) {
    if (need > AO_LSP_MESSAGE_MAX + AO_LSP_HEADER_MAX + 1U) return -1;
    if (need <= server->read_capacity) return 0;
    size_t cap = server->read_capacity ? server->read_capacity : 8192;
    while (cap < need) {
        if (cap > (AO_LSP_MESSAGE_MAX + AO_LSP_HEADER_MAX) / 2U) {
            cap = AO_LSP_MESSAGE_MAX + AO_LSP_HEADER_MAX + 1U;
            break;
        }
        cap *= 2U;
    }
    char *next = realloc(server->read_buffer, cap);
    if (!next) return -1;
    server->read_buffer = next;
    server->read_capacity = cap;
    return 0;
}

static int write_bytes_deadline(int fd, const char *data, size_t len, const struct timespec *deadline) {
    size_t pos = 0;
    while (pos < len) {
        int wait_ms = remaining_ms(deadline);
        if (wait_ms <= 0) { errno = ETIMEDOUT; return -1; }
        struct pollfd pfd = {.fd = fd, .events = POLLOUT};
        int rc = poll(&pfd, 1, wait_ms);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) { if (rc == 0) errno = ETIMEDOUT; return -1; }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) { errno = EPIPE; return -1; }
        ssize_t n = write(fd, data + pos, len - pos);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) return -1;
        pos += (size_t)n;
    }
    return 0;
}

static int lsp_send(LSPServer *server, json_object *message, char *err, size_t err_size) {
    const char *body = json_object_to_json_string_ext(message, JSON_C_TO_STRING_PLAIN);
    if (!body) { set_error(err, err_size, "cannot serialize LSP JSON-RPC message"); return -1; }
    size_t length = strlen(body);
    if (length > AO_LSP_MESSAGE_MAX) { set_error(err, err_size, "LSP message exceeds 4 MiB limit"); return -1; }
    size_t cap = length + 64;
    char *framed = malloc(cap);
    if (!framed) { set_error(err, err_size, "out of memory framing LSP message"); return -1; }
    int n = snprintf(framed, cap, "Content-Length: %zu\r\n\r\n%s", length, body);
    if (n < 0 || (size_t)n >= cap) { free(framed); set_error(err, err_size, "LSP framing overflow"); return -1; }
    struct timespec deadline;
    (void)clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += server->config.timeout_seconds > 0 ? server->config.timeout_seconds : 10;
    int rc = write_bytes_deadline(server->to_child, framed, (size_t)n, &deadline);
    if (rc != 0 && err && err_size)
        (void)snprintf(err, err_size, "write to LSP '%s' failed: %s", server->config.language, strerror(errno));
    free(framed);
    return rc;
}

static int find_header_length(const char *buf, size_t len, size_t *header_len) {
    if (len > AO_LSP_HEADER_MAX) return -1;
    for (size_t i = 0; i + 3 < len; ++i) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            *header_len = i + 4;
            return 1;
        }
    }
    return 0;
}

static int get_content_length(const char *header, size_t length, size_t *out) {
    const char *p = header;
    const char *end = header + length;
    bool found = false;
    size_t value = 0;
    while (p < end) {
        const char *line_end = NULL;
        for (const char *q = p; q + 1 < end; ++q) if (q[0] == '\r' && q[1] == '\n') { line_end = q; break; }
        if (!line_end) break;
        if ((size_t)(line_end - p) >= 15 && strncasecmp(p, "Content-Length:", 15) == 0) {
            const char *v = p + 15;
            while (v < line_end && (*v == ' ' || *v == '\t')) ++v;
            if (v == line_end) return -1;
            size_t parsed = 0;
            for (; v < line_end; ++v) {
                if (!isdigit((unsigned char)*v)) return -1;
                if (parsed > (AO_LSP_MESSAGE_MAX - (size_t)(*v - '0')) / 10U) return -1;
                parsed = parsed * 10U + (size_t)(*v - '0');
            }
            value = parsed; found = true;
        }
        p = line_end + 2;
    }
    if (!found || value > AO_LSP_MESSAGE_MAX) return -1;
    *out = value;
    return 0;
}

static json_object *lsp_read_one(LSPServer *server, const struct timespec *deadline, char *err, size_t err_size) {
    for (;;) {
        size_t header_len = 0;
        int hdr = find_header_length(server->read_buffer ? server->read_buffer : "", server->read_length, &header_len);
        if (hdr < 0) { set_error(err, err_size, "LSP header exceeds size limit"); return NULL; }
        if (hdr > 0) {
            size_t body_len = 0;
            if (get_content_length(server->read_buffer, header_len, &body_len) != 0) {
                set_error(err, err_size, "LSP header has invalid/missing Content-Length"); return NULL;
            }
            if (server->read_length >= header_len + body_len) {
                json_tokener *tok = json_tokener_new();
                json_object *obj = NULL;
                if (tok) {
                    obj = json_tokener_parse_ex(tok, server->read_buffer + header_len, (int)body_len);
                    if (json_tokener_get_error(tok) != json_tokener_success || !obj ||
                        json_object_get_type(obj) != json_type_object ||
                        (size_t)json_tokener_get_parse_end(tok) != body_len) {
                        if (obj) json_object_put(obj);
                        obj = NULL;
                    }
                    json_tokener_free(tok);
                }
                size_t consumed = header_len + body_len;
                memmove(server->read_buffer, server->read_buffer + consumed, server->read_length - consumed);
                server->read_length -= consumed;
                if (server->read_buffer) server->read_buffer[server->read_length] = '\0';
                if (!obj) set_error(err, err_size, "LSP server sent malformed JSON-RPC body");
                return obj;
            }
        }
        int wait_ms = remaining_ms(deadline);
        if (wait_ms <= 0) { errno = ETIMEDOUT; set_error(err, err_size, "timed out waiting for LSP response"); return NULL; }
        struct pollfd pfd = {.fd = server->from_child, .events = POLLIN};
        int pr = poll(&pfd, 1, wait_ms);
        if (pr < 0 && errno == EINTR) continue;
        if (pr == 0) { errno = ETIMEDOUT; set_error(err, err_size, "timed out waiting for LSP response"); return NULL; }
        if (pr < 0 || (pfd.revents & (POLLERR | POLLNVAL))) { set_error(err, err_size, "LSP server pipe failed"); return NULL; }
        char chunk[8192];
        ssize_t got = read(server->from_child, chunk, sizeof(chunk));
        if (got < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (got <= 0) { set_error(err, err_size, got == 0 ? "LSP server closed stdout" : strerror(errno)); return NULL; }
        if (grow_buffer(server, server->read_length + (size_t)got + 1) != 0) {
            set_error(err, err_size, "cannot grow LSP response buffer"); return NULL;
        }
        memcpy(server->read_buffer + server->read_length, chunk, (size_t)got);
        server->read_length += (size_t)got;
        server->read_buffer[server->read_length] = '\0';
    }
}

static bool json_int_equal(json_object *obj, const char *key, int expected) {
    json_object *value = NULL;
    return obj && json_object_object_get_ex(obj, key, &value) && value &&
           json_object_get_type(value) == json_type_int && json_object_get_int(value) == expected;
}

static int lsp_send_response(LSPServer *server, json_object *id, json_object *result, json_object *error) {
    json_object *message = json_object_new_object();
    if (!message) return -1;
    json_object_object_add(message, "jsonrpc", json_object_new_string("2.0"));
    json_object_object_add(message, "id", id ? json_object_get(id) : json_object_new_null());
    if (error) json_object_object_add(message, "error", json_object_get(error));
    else json_object_object_add(message, "result", result ? json_object_get(result) : json_object_new_null());
    int rc = lsp_send(server, message, NULL, 0);
    json_object_put(message);
    return rc;
}

static void lsp_handle_server_request(LSPServer *server, json_object *message) {
    json_object *method_obj = NULL, *id_obj = NULL, *params = NULL;
    if (!json_object_object_get_ex(message, "method", &method_obj) || !method_obj ||
        json_object_get_type(method_obj) != json_type_string ||
        !json_object_object_get_ex(message, "id", &id_obj) || !id_obj) return;
    const char *method = json_object_get_string(method_obj);
    (void)json_object_object_get_ex(message, "params", &params);
    json_object *result = NULL;
    if (strcmp(method, "workspace/configuration") == 0) {
        result = json_object_new_array();
        json_object *items = NULL;
        if (params && json_object_object_get_ex(params, "items", &items) && items && json_object_get_type(items) == json_type_array)
            for (size_t i = 0; i < json_object_array_length(items); ++i) json_object_array_add(result, json_object_new_null());
    } else if (strcmp(method, "workspace/workspaceFolders") == 0) {
        result = json_object_new_array();
        char uri[AO_LSP_URI_MAX];
        size_t used = 0;
        const char *root = server->config.root_dir;
        used += (size_t)snprintf(uri + used, sizeof(uri) - used, "file://");
        for (const unsigned char *p = (const unsigned char *)root; *p && used + 4 < sizeof(uri); ++p) {
            if (isalnum(*p) || *p == '/' || *p == '-' || *p == '_' || *p == '.' || *p == '~') uri[used++] = (char)*p;
            else { int n = snprintf(uri + used, sizeof(uri) - used, "%%%02X", *p); if (n > 0) used += (size_t)n; }
        }
        uri[used] = '\0';
        json_object *folder = json_object_new_object();
        json_object_object_add(folder, "uri", json_object_new_string(uri));
        json_object_object_add(folder, "name", json_object_new_string("ASGEND workspace"));
        json_object_array_add(result, folder);
    } else if (strcmp(method, "client/registerCapability") == 0 ||
               strcmp(method, "client/unregisterCapability") == 0 ||
               strcmp(method, "window/workDoneProgress/create") == 0 ||
               strcmp(method, "window/showMessageRequest") == 0) {
        result = json_object_new_null();
    } else {
        json_object *error = json_object_new_object();
        json_object_object_add(error, "code", json_object_new_int(-32601));
        json_object_object_add(error, "message", json_object_new_string("Method not supported by ASGEND LSP client"));
        (void)lsp_send_response(server, id_obj, NULL, error);
        json_object_put(error);
        return;
    }
    if (result) { (void)lsp_send_response(server, id_obj, result, NULL); json_object_put(result); }
}

static json_object *lsp_wait_id(LSPServer *server, int id, int timeout_seconds, char *err, size_t err_size) {
    struct timespec deadline;
    (void)clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_seconds > 0 ? timeout_seconds : 10;
    for (;;) {
        json_object *msg = lsp_read_one(server, &deadline, err, err_size);
        if (!msg) return NULL;
        json_object *method = NULL, *msg_id = NULL;
        (void)json_object_object_get_ex(msg, "method", &method);
        (void)json_object_object_get_ex(msg, "id", &msg_id);
        if (method && json_object_get_type(method) == json_type_string && msg_id) {
            lsp_handle_server_request(server, msg);
            json_object_put(msg);
            continue;
        }
        if (!json_int_equal(msg, "id", id)) { json_object_put(msg); continue; }
        json_object *error_obj = NULL, *result = NULL;
        if (json_object_object_get_ex(msg, "error", &error_obj) && error_obj) {
            json_object *message = NULL;
            const char *text = "LSP JSON-RPC request failed";
            if (json_object_object_get_ex(error_obj, "message", &message) && message && json_object_get_type(message) == json_type_string) text = json_object_get_string(message);
            if (err && err_size) (void)snprintf(err, err_size, "LSP request error: %.400s", text);
            json_object_put(msg); return NULL;
        }
        if (json_object_object_get_ex(msg, "result", &result) && result) json_object_get(result);
        json_object_put(msg);
        return result;
    }
}

static int lsp_request(LSPServer *server, const char *method, json_object *params,
                       json_object **result, char *err, size_t err_size) {
    int id = ++server->next_request_id;
    if (server->next_request_id < 1) server->next_request_id = id = 1;
    json_object *request = json_object_new_object();
    if (!request) { set_error(err, err_size, "out of memory creating LSP request"); return -1; }
    json_object_object_add(request, "jsonrpc", json_object_new_string("2.0"));
    json_object_object_add(request, "id", json_object_new_int(id));
    json_object_object_add(request, "method", json_object_new_string(method));
    if (params) json_object_object_add(request, "params", json_object_get(params));
    int rc = lsp_send(server, request, err, err_size);
    json_object_put(request);
    if (rc != 0) return -1;
    json_object *reply = lsp_wait_id(server, id, server->config.timeout_seconds, err, err_size);
    if (!reply) return -1;
    if (result) *result = reply; else json_object_put(reply);
    return 0;
}

static int lsp_notify(LSPServer *server, const char *method, json_object *params, char *err, size_t err_size) {
    json_object *message = json_object_new_object();
    if (!message) { set_error(err, err_size, "out of memory creating LSP notification"); return -1; }
    json_object_object_add(message, "jsonrpc", json_object_new_string("2.0"));
    json_object_object_add(message, "method", json_object_new_string(method));
    if (params) json_object_object_add(message, "params", json_object_get(params));
    int rc = lsp_send(server, message, err, err_size);
    json_object_put(message);
    return rc;
}

static int start_lsp(LSPServer *server, char *err, size_t err_size) {
    if (server->started && server->pid > 0 && kill(server->pid, 0) == 0) return 0;
    int to_child[2] = {-1, -1}, from_child[2] = {-1, -1};
    if (pipe2(to_child, O_CLOEXEC) != 0 || pipe2(from_child, O_CLOEXEC) != 0) {
        if (to_child[0] >= 0) { close(to_child[0]); close(to_child[1]); }
        if (from_child[0] >= 0) { close(from_child[0]); close(from_child[1]); }
        if (err && err_size) (void)snprintf(err, err_size, "cannot create LSP pipes: %s", strerror(errno));
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        if (err && err_size) (void)snprintf(err, err_size, "cannot fork LSP server: %s", strerror(errno));
        close(to_child[0]); close(to_child[1]); close(from_child[0]); close(from_child[1]); return -1;
    }
    if (pid == 0) {
        (void)setpgid(0, 0);
        (void)dup2(to_child[0], STDIN_FILENO); (void)dup2(from_child[1], STDOUT_FILENO);
        close(to_child[0]); close(to_child[1]); close(from_child[0]); close(from_child[1]);
        if (server->config.root_dir[0]) (void)chdir(server->config.root_dir);
        char *argv[AO_MAX_LSP_ARGS + 2];
        argv[0] = server->config.command;
        for (size_t i = 0; i < server->config.arg_count; ++i) argv[i + 1] = server->config.args[i];
        argv[server->config.arg_count + 1] = NULL;
        execvp(argv[0], argv);
        dprintf(STDERR_FILENO, "ASGEND: cannot exec LSP '%s': %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    (void)setpgid(pid, pid);
    close(to_child[0]); close(from_child[1]);
    server->pid = pid; server->to_child = to_child[1]; server->from_child = from_child[0];
    int flags = fcntl(server->to_child, F_GETFL, 0); if (flags >= 0) (void)fcntl(server->to_child, F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(server->from_child, F_GETFL, 0); if (flags >= 0) (void)fcntl(server->from_child, F_SETFL, flags | O_NONBLOCK);
    server->started = true;

    json_object *params = json_object_new_object();
    json_object *caps = json_object_new_object(), *text_doc = json_object_new_object(), *publish = json_object_new_object();
    json_object *client_info = json_object_new_object();
    json_object_object_add(publish, "relatedInformation", json_object_new_boolean(true));
    json_object_object_add(text_doc, "publishDiagnostics", publish);
    json_object_object_add(caps, "textDocument", text_doc);
    json_object_object_add(params, "processId", json_object_new_int((int)getpid()));
    json_object_object_add(params, "capabilities", caps);
    json_object_object_add(client_info, "name", json_object_new_string("ASGEND"));
    json_object_object_add(client_info, "version", json_object_new_string(AGEND_VERSION));
    json_object_object_add(params, "clientInfo", client_info);
    json_object *folder = json_object_new_object();
    char root_uri[AO_LSP_URI_MAX];
    size_t used = (size_t)snprintf(root_uri, sizeof(root_uri), "file://");
    for (const unsigned char *p = (const unsigned char *)server->config.root_dir; *p && used + 4 < sizeof(root_uri); ++p) {
        if (isalnum(*p) || *p == '/' || *p == '-' || *p == '_' || *p == '.' || *p == '~') root_uri[used++] = (char)*p;
        else { int n = snprintf(root_uri + used, sizeof(root_uri) - used, "%%%02X", *p); if (n > 0) used += (size_t)n; }
    }
    root_uri[used] = '\0';
    json_object_object_add(params, "rootUri", json_object_new_string(root_uri));
    json_object_object_add(folder, "uri", json_object_new_string(root_uri));
    json_object_object_add(folder, "name", json_object_new_string("ASGEND workspace"));
    json_object *folders = json_object_new_array(); json_object_array_add(folders, folder);
    json_object_object_add(params, "workspaceFolders", folders);
    json_object *init_result = NULL;
    int rc = lsp_request(server, "initialize", params, &init_result, err, err_size);
    json_object_put(params);
    if (init_result) json_object_put(init_result);
    if (rc == 0) {
        json_object *initialized_params = json_object_new_object();
        if (!initialized_params) { set_error(err, err_size, "out of memory creating LSP initialized notification"); rc = -1; }
        else { rc = lsp_notify(server, "initialized", initialized_params, err, err_size); json_object_put(initialized_params); }
    }
    if (rc != 0) {
        (void)kill(-pid, SIGTERM); (void)kill(pid, SIGTERM);
        (void)close(server->to_child); (void)close(server->from_child);
        server->to_child = server->from_child = -1;
        int status = 0; for (int i = 0; i < 10; ++i) { pid_t w = waitpid(pid, &status, WNOHANG); if (w == pid || (w < 0 && errno == ECHILD)) break; struct timespec pause = {.tv_sec=0,.tv_nsec=100000000L}; nanosleep(&pause,NULL); }
        (void)kill(-pid, SIGKILL); (void)waitpid(pid, &status, WNOHANG); server->pid = -1; server->started = false;
        return -1;
    }
    return 0;
}

static bool extension_matches(const LSPServerConfig *cfg, const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return false;
    for (size_t i = 0; i < cfg->extension_count; ++i)
        if (strcasecmp(ext, cfg->extensions[i]) == 0) return true;
    return false;
}

static bool path_is_inside(const char *root, const char *path) {
    if (!root || !path) return false;
    size_t n = strlen(root);
    if (!n || strncmp(root, path, n) != 0) return false;
    return path[n] == '\0' || root[n - 1] == '/' || path[n] == '/';
}

static int file_uri(const char *path, char *out, size_t cap) {
    size_t used = 0;
    int n = snprintf(out, cap, "file://");
    if (n < 0 || (size_t)n >= cap) return -1;
    used = (size_t)n;
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p) {
        if (isalnum(*p) || *p == '/' || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            if (used + 2 > cap) return -1;
            out[used++] = (char)*p;
        } else {
            if (used + 4 > cap) return -1;
            n = snprintf(out + used, cap - used, "%%%02X", *p);
            if (n != 3) return -1;
            used += (size_t)n;
        }
    }
    out[used] = '\0';
    return 0;
}

static LSPDocument *document_find(LSPServer *server, const char *uri) {
    for (LSPDocument *d = (LSPDocument *)server->documents; d; d = d->next) if (strcmp(d->uri, uri) == 0) return d;
    return NULL;
}

static int send_document(LSPServer *server, const char *uri, const char *language, const char *text, char *err, size_t err_size) {
    LSPDocument *doc = document_find(server, uri);
    json_object *params = json_object_new_object(), *text_doc = json_object_new_object();
    if (!params || !text_doc) { if (params) json_object_put(params); if (text_doc) json_object_put(text_doc); set_error(err, err_size, "out of memory preparing LSP document"); return -1; }
    if (!doc) {
        doc = calloc(1, sizeof(*doc));
        if (!doc || strlen(uri) >= sizeof(doc->uri)) { free(doc); json_object_put(params); json_object_put(text_doc); set_error(err, err_size, "LSP document URI too long"); return -1; }
        (void)snprintf(doc->uri, sizeof(doc->uri), "%s", uri); doc->version = 1;
        doc->next = (LSPDocument *)server->documents; server->documents = (void *)doc;
        json_object_object_add(text_doc, "uri", json_object_new_string(uri));
        json_object_object_add(text_doc, "languageId", json_object_new_string(language));
        json_object_object_add(text_doc, "version", json_object_new_int(doc->version));
        json_object_object_add(text_doc, "text", json_object_new_string(text));
        json_object_object_add(params, "textDocument", text_doc);
        int rc = lsp_notify(server, "textDocument/didOpen", params, err, err_size);
        json_object_put(params);
        return rc;
    }
    ++doc->version;
    json_object_object_add(text_doc, "uri", json_object_new_string(uri));
    json_object_object_add(text_doc, "version", json_object_new_int(doc->version));
    json_object_object_add(params, "textDocument", text_doc);
    json_object *content = json_object_new_array(), *change = json_object_new_object();
    json_object_object_add(change, "text", json_object_new_string(text)); json_object_array_add(content, change);
    json_object_object_add(params, "contentChanges", content);
    int rc = lsp_notify(server, "textDocument/didChange", params, err, err_size);
    json_object_put(params);
    return rc;
}

static char *format_diagnostics(json_object *diagnostics, const char *path) {
    char *out = calloc(1, 32768);
    if (!out) return NULL;
    size_t used = (size_t)snprintf(out, 32768, "LSP diagnostics for %s:\n", path);
    if (!diagnostics || json_object_get_type(diagnostics) != json_type_array) {
        (void)snprintf(out + used, 32768 - used, "No diagnostics payload was returned. Do not treat this as proof that the file is valid.");
        return out;
    }
    size_t n = json_object_array_length(diagnostics);
    if (n == 0) { (void)snprintf(out + used, 32768 - used, "No issues reported by the language server."); return out; }
    for (size_t i = 0; i < n && used + 128 < 32768; ++i) {
        json_object *d = json_object_array_get_idx(diagnostics, i), *range = NULL, *start = NULL, *line = NULL,
                    *character = NULL, *message = NULL, *severity = NULL, *code = NULL;
        if (!d || json_object_get_type(d) != json_type_object) continue;
        (void)json_object_object_get_ex(d, "range", &range); if (range) (void)json_object_object_get_ex(range, "start", &start);
        if (start) { (void)json_object_object_get_ex(start, "line", &line); (void)json_object_object_get_ex(start, "character", &character); }
        (void)json_object_object_get_ex(d, "message", &message); (void)json_object_object_get_ex(d, "severity", &severity); (void)json_object_object_get_ex(d, "code", &code);
        int sev = severity && json_object_get_type(severity) == json_type_int ? json_object_get_int(severity) : 3;
        const char *kind = sev == 1 ? "ERROR" : sev == 2 ? "WARNING" : sev == 3 ? "INFO" : "HINT";
        const char *txt = message && json_object_get_type(message) == json_type_string ? json_object_get_string(message) : "unspecified diagnostic";
        int l = line && json_object_get_type(line) == json_type_int ? json_object_get_int(line) + 1 : 1;
        int c = character && json_object_get_type(character) == json_type_int ? json_object_get_int(character) + 1 : 1;
        int w = snprintf(out + used, 32768 - used, "%s:%d:%d: %s: %.1200s", path, l, c, kind, txt);
        if (w < 0) break;
        used += (size_t)w;
        if (code) { const char *cs = json_object_get_type(code) == json_type_string ? json_object_get_string(code) : json_object_to_json_string(code); int cw = snprintf(out + used, 32768 - used, " [code %s]", cs ? cs : "?"); if (cw > 0) used += (size_t)cw; }
        if (used + 2 < 32768) { out[used++] = '\n'; out[used] = '\0'; }
    }
    return out;
}

void lsp_stop_all(Runtime *rt) {
    if (!rt) return;
    for (size_t i = 0; i < rt->lsp_server_count; ++i) {
        LSPServer *s = &rt->lsp_servers[i];
        if (s->mutex_initialized) pthread_mutex_lock(&s->mutex);
        if (s->pid > 0) { (void)kill(-s->pid, SIGTERM); (void)kill(s->pid, SIGTERM); }
        if (s->to_child >= 0) { close(s->to_child); s->to_child = -1; }
        if (s->from_child >= 0) { close(s->from_child); s->from_child = -1; }
        if (s->pid > 0) {
            int status = 0; bool exited = false;
            for (int iwait = 0; iwait < 20; ++iwait) {
                pid_t rc = waitpid(s->pid, &status, WNOHANG);
                if (rc == s->pid || (rc < 0 && errno == ECHILD)) { exited = true; break; }
                struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000L}; (void)nanosleep(&pause, NULL);
            }
            if (!exited) { (void)kill(-s->pid, SIGKILL); (void)kill(s->pid, SIGKILL); while (waitpid(s->pid, &status, 0) < 0 && errno == EINTR) {} }
            s->pid = -1;
        }
        free(s->read_buffer); s->read_buffer = NULL;
        LSPDocument *doc = (LSPDocument *)s->documents;
        while (doc) { LSPDocument *next = doc->next; free(doc); doc = next; }
        s->documents = NULL; s->started = false;
        if (s->mutex_initialized) { pthread_mutex_unlock(&s->mutex); pthread_mutex_destroy(&s->mutex); s->mutex_initialized = false; }
    }
    rt->lsp_server_count = 0;
}

int lsp_init_all(Runtime *rt, char *err, size_t err_size) {
    if (!rt) { set_error(err, err_size, "runtime is required"); return -1; }
    rt->lsp_server_count = rt->config.lsp_server_count;
    for (size_t i = 0; i < rt->lsp_server_count; ++i) {
        LSPServer *s = &rt->lsp_servers[i];
        memset(s, 0, sizeof(*s));
        s->config = rt->config.lsp_servers[i];
        s->pid = -1; s->to_child = -1; s->from_child = -1;
        if (pthread_mutex_init(&s->mutex, NULL) != 0) {
            set_error(err, err_size, "cannot initialize LSP session mutex");
            rt->lsp_server_count = i;
            lsp_stop_all(rt);
            return -1;
        }
        s->mutex_initialized = true;
    }
    return 0;
}

char *lsp_run_diagnostics(Runtime *rt, const char *file_path, char *err, size_t err_size) {
    if (!rt || !file_path || !*file_path) { set_error(err, err_size, "file_path is required"); return NULL; }
    char absolute[AO_MAX_PATH], resolved[AO_MAX_PATH];
    if (file_path[0] == '/') {
        if (string_copy(absolute, sizeof(absolute), file_path) != 0) { set_error(err, err_size, "file path too long"); return NULL; }
    } else if (snprintf(absolute, sizeof(absolute), "%s/%s", rt->config.root_dir, file_path) >= (int)sizeof(absolute)) {
        set_error(err, err_size, "file path too long"); return NULL;
    }
    char *resolved_alloc = realpath(absolute, NULL);
    if (!resolved_alloc) { if (err && err_size) (void)snprintf(err, err_size, "cannot resolve LSP file: %s", strerror(errno)); return NULL; }
    if (strlen(resolved_alloc) >= sizeof(resolved)) { free(resolved_alloc); set_error(err, err_size, "resolved LSP file path is too long"); return NULL; }
    (void)snprintf(resolved, sizeof(resolved), "%s", resolved_alloc); free(resolved_alloc);
    struct stat st;
    if (stat(resolved, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size > AO_LSP_FILE_MAX) {
        set_error(err, err_size, "LSP diagnostics accepts regular files no larger than 5 MiB"); return NULL;
    }
    char *root_alloc = realpath(rt->config.root_dir, NULL);
    if (!root_alloc) { set_error(err, err_size, "configured workspace root cannot be resolved"); return NULL; }
    bool in_workspace = path_is_inside(root_alloc, resolved);
    free(root_alloc);
    if (!in_workspace) { set_error(err, err_size, "LSP file must be inside the configured workspace root"); return NULL; }
    LSPServer *server = NULL;
    for (size_t i = 0; i < rt->lsp_server_count; ++i) {
        LSPServer *candidate = &rt->lsp_servers[i];
        if (candidate->config.enabled && extension_matches(&candidate->config, resolved)) { server = candidate; break; }
    }
    if (!server) { if (err && err_size) (void)snprintf(err, err_size, "no enabled LSP server configured for file extension '%s'", strrchr(resolved, '.') ? strrchr(resolved, '.') : "(none)"); return NULL; }
    FILE *f = fopen(resolved, "rb");
    if (!f) { if (err && err_size) (void)snprintf(err, err_size, "cannot read LSP file: %s", strerror(errno)); return NULL; }
    char *text = malloc((size_t)st.st_size + 1U);
    if (!text) { fclose(f); set_error(err, err_size, "out of memory reading LSP file"); return NULL; }
    size_t got = fread(text, 1, (size_t)st.st_size, f); fclose(f);
    if (got != (size_t)st.st_size || memchr(text, '\0', got)) { free(text); set_error(err, err_size, "cannot read LSP file or file contains NUL bytes"); return NULL; }
    text[got] = '\0';
    char uri[AO_LSP_URI_MAX];
    if (file_uri(resolved, uri, sizeof(uri)) != 0) { free(text); set_error(err, err_size, "cannot encode file URI for LSP"); return NULL; }
    const char *language = server->config.language[0] ? server->config.language : "plaintext";
    const char *ext = strrchr(resolved, '.');
    if (ext && !server->config.language[0]) {
        if (strcasecmp(ext, ".c") == 0 || strcasecmp(ext, ".h") == 0) language = "c";
        else if (strcasecmp(ext, ".cc") == 0 || strcasecmp(ext, ".cpp") == 0 || strcasecmp(ext, ".hpp") == 0) language = "cpp";
        else if (strcasecmp(ext, ".py") == 0) language = "python";
        else if (strcasecmp(ext, ".rs") == 0) language = "rust";
        else if (strcasecmp(ext, ".js") == 0 || strcasecmp(ext, ".jsx") == 0) language = "javascript";
        else if (strcasecmp(ext, ".ts") == 0 || strcasecmp(ext, ".tsx") == 0) language = "typescript";
        else if (strcasecmp(ext, ".go") == 0) language = "go";
    }
    pthread_mutex_lock(&server->mutex);
    int rc = start_lsp(server, err, err_size);
    if (rc == 0) rc = send_document(server, uri, language, text, err, err_size);
    free(text);
    json_object *diagnostics = NULL;
    bool received = false;
    struct timespec deadline;
    (void)clock_gettime(CLOCK_MONOTONIC, &deadline);
    int wait_seconds = server->config.timeout_seconds > 0 ? server->config.timeout_seconds : 10;
    if (wait_seconds > 5) wait_seconds = 5;
    deadline.tv_sec += wait_seconds;
    while (rc == 0 && remaining_ms(&deadline) > 0) {
        json_object *message = lsp_read_one(server, &deadline, err, err_size);
        if (!message) break;
        json_object *method = NULL, *params = NULL;
        if (json_object_object_get_ex(message, "method", &method) && method && json_object_get_type(method) == json_type_string &&
            strcmp(json_object_get_string(method), "textDocument/publishDiagnostics") == 0 &&
            json_object_object_get_ex(message, "params", &params) && params && json_object_get_type(params) == json_type_object) {
            json_object *uri_obj = NULL, *items = NULL;
            if (json_object_object_get_ex(params, "uri", &uri_obj) && uri_obj && json_object_get_type(uri_obj) == json_type_string &&
                strcmp(json_object_get_string(uri_obj), uri) == 0 && json_object_object_get_ex(params, "diagnostics", &items) && items &&
                json_object_get_type(items) == json_type_array) {
                if (diagnostics) json_object_put(diagnostics);
                diagnostics = json_object_get(items); received = true;
                json_object_put(message); break;
            }
        } else {
            json_object *id = NULL;
            if (json_object_object_get_ex(message, "id", &id) && id) lsp_handle_server_request(server, message);
        }
        json_object_put(message);
    }
    pthread_mutex_unlock(&server->mutex);
    char *result = NULL;
    if (!received && rc == 0 && err && err_size) err[0] = '\0';
    if (rc == 0) {
        if (received) result = format_diagnostics(diagnostics, resolved);
        else {
            result = calloc(1, 2048);
            if (result) (void)snprintf(result, 2048, "LSP status for %s: no publishDiagnostics notification received within %d seconds. This is not proof that the file is valid; check server startup, workspace setup, and whether diagnostics are push- or pull-based.", resolved, wait_seconds);
        }
    }
    if (diagnostics) json_object_put(diagnostics);
    if (rc != 0 && server->pid > 0) { (void)kill(-server->pid, SIGTERM); }
    return result;
}
