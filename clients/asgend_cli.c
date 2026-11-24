#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include <json-c/json.h>
#include "asgend_tui.h"
#include <errno.h>
#include <dirent.h>
#include <ctype.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <locale.h>
#include <limits.h>
#include <strings.h>
#include <pthread.h>
#include <stdatomic.h>
#include <wchar.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define RESPONSE_LIMIT (16U * 1024U * 1024U)
#define CLI_WORKSPACE_FILE_MAX (1024U * 1024U)
#define CLI_EDITOR_COMMAND_MAX 1024U
#define CLI_EDITOR_ARGV_MAX 32U
#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 8765
/* Bound line-mode polling latency so streamed text appears while the model is still generating. */
#define CLI_STREAM_POLL_INTERVAL_NS 120000000L

typedef struct {
    const char *host;
    const char *port;
    const char *token;
} Client;

static AsgendTui cli_tui;
static bool cli_tui_live;
static char cli_tui_scope[ASGEND_TUI_KEY_MAX] = "global";
static char cli_tui_files_scope[ASGEND_TUI_KEY_MAX] = "";
static void cli_tui_show_help(void);
static void cli_tui_edit_previewed_file(const Client *client);
static void cli_print_stream_rule(void);
static int follow_task(const Client *client, const char *task_id);
static atomic_bool cli_tui_plan_refresh_pending = ATOMIC_VAR_INIT(false);
static atomic_bool cli_tui_plan_watch_stop = ATOMIC_VAR_INIT(false);
static pthread_t cli_tui_plan_watch_thread;
static bool cli_tui_plan_watch_started;
static int64_t cli_tui_preview_offsets[1024];
static size_t cli_tui_preview_offset_count;
static char *cli_tui_preview_baseline;
static char cli_tui_preview_baseline_path[ASGEND_TUI_PATH_MAX];
static char cli_tui_input_action[24] = "task";
static char cli_tui_requested_session[ASGEND_TUI_KEY_MAX] = "";
static char cli_tui_last_model[160] = "Модель";
static bool cli_tui_valid_utf8(const unsigned char *data, size_t length);

/* Stable model fingerprints keep concurrent streams from different models in
 * separate tiles even when a provider reuses stream IDs. Raw model names are
 * never embedded in keys, so keys remain bounded and safe for terminal state. */
static uint32_t cli_tui_model_fingerprint(const char *model) {
    uint32_t hash = UINT32_C(2166136261);
    const unsigned char *p = (const unsigned char *)(model ? model : "Модель");
    while (*p) { hash ^= *p++; hash *= UINT32_C(16777619); }
    return hash;
}

static void cli_tui_stream_key(char *out, size_t out_size, const char *kind,
                               const char *model, const char *stream_id) {
    char id[40];
    size_t used = 0;
    const unsigned char *p = (const unsigned char *)(stream_id && *stream_id ? stream_id : "active");
    while (*p && used + 1 < sizeof(id)) {
        unsigned char c = *p++;
        id[used++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_') ? (char)c : '_';
    }
    id[used] = '\0';
    (void)snprintf(out, out_size, "%s:%08x:%s", kind ? kind : "stream",
                   (unsigned)cli_tui_model_fingerprint(model), id);
}

static void cli_tui_close_model_stream_tools(const char *model, const char *stream_id) {
    if (!cli_tui_live) return;
    char id[40];
    size_t used = 0;
    const unsigned char *p = (const unsigned char *)(stream_id && *stream_id ? stream_id : "active");
    while (*p && used + 1 < sizeof(id)) {
        unsigned char c = *p++;
        id[used++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_') ? (char)c : '_';
    }
    id[used] = '\0';
    char stream_prefix[ASGEND_TUI_KEY_MAX];
    /* call_key is <stream-id>-<index>; include the trailing hyphen so s-1
     * cannot accidentally close s-10 while several streams are active. */
    (void)snprintf(stream_prefix, sizeof(stream_prefix), "tool-form:%08x:%s-",
                   (unsigned)cli_tui_model_fingerprint(model), id);
    size_t n = strlen(stream_prefix);
    for (size_t i = 0; i < cli_tui.tile_count; ++i) {
        AsgendTuiTile *tile = &cli_tui.tiles[i];
        if (strncmp(tile->key, stream_prefix, n) != 0) continue;
        tile->streaming = false;
        char *down = strstr(tile->title, "↓");
        if (down) memcpy(down, "→", strlen("→"));
        if (cli_tui.display_mode == ASGEND_TUI_DISPLAY_DYNAMIC && tile->kind != ASGEND_TUI_TILE_CHAT)
            tile->expanded = false;
    }
}
static _Thread_local int cli_rpc_timeout_seconds = 60;
static _Thread_local bool cli_rpc_quiet;

static json_object *obj_get(json_object *obj, const char *key) {
    json_object *value = NULL;
    return obj && json_object_object_get_ex(obj, key, &value) ? value : NULL;
}

static const char *str_get(json_object *obj, const char *key, const char *fallback) {
    json_object *value = obj_get(obj, key);
    return value && json_object_get_type(value) == json_type_string ? json_object_get_string(value) : fallback;
}

static int send_all(int fd, const char *data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        ssize_t n = send(fd, data + sent, length - sent, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static int connect_with_timeout(int fd, const struct sockaddr *address, socklen_t address_length,
                                int timeout_seconds) {
    int original_flags = fcntl(fd, F_GETFL, 0);
    if (original_flags < 0) return -1;
    if (fcntl(fd, F_SETFL, original_flags | O_NONBLOCK) != 0) return -1;

    int rc = connect(fd, address, address_length);
    if (rc != 0 && errno == EINPROGRESS) {
        long timeout_ms_long = (long)(timeout_seconds > 0 ? timeout_seconds : 1) * 1000L;
        int timeout_ms = timeout_ms_long > INT_MAX ? INT_MAX : (int)timeout_ms_long;
        struct pollfd pfd = {.fd = fd, .events = POLLOUT, .revents = 0};
        int poll_rc;
        do {
            poll_rc = poll(&pfd, 1, timeout_ms);
        } while (poll_rc < 0 && errno == EINTR);
        if (poll_rc == 0) {
            errno = ETIMEDOUT;
            rc = -1;
        } else if (poll_rc < 0) {
            rc = -1;
        } else {
            int socket_error = 0;
            socklen_t error_length = sizeof(socket_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) != 0) {
                rc = -1;
            } else if (socket_error != 0) {
                errno = socket_error;
                rc = -1;
            } else {
                rc = 0;
            }
        }
    }
    int saved_errno = errno;
    if (fcntl(fd, F_SETFL, original_flags) != 0 && rc == 0) {
        saved_errno = errno;
        rc = -1;
    }
    errno = saved_errno;
    return rc;
}

static int connect_core(const Client *client) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *addresses = NULL;
    int rc = getaddrinfo(client->host, client->port, &hints, &addresses);
    if (rc != 0) {
        if (!cli_rpc_quiet) fprintf(stderr, "ASGEND: cannot resolve %s: %s\n", client->host, gai_strerror(rc));
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *it = addresses; it; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) continue;
        struct timeval timeout = {.tv_sec = cli_rpc_timeout_seconds > 0 ? cli_rpc_timeout_seconds : 60, .tv_usec = 0};
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        if (connect_with_timeout(fd, it->ai_addr, it->ai_addrlen, cli_rpc_timeout_seconds) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(addresses);
    if (fd < 0 && !cli_rpc_quiet) fprintf(stderr, "ASGEND: cannot connect to core at %s:%s: %s\n", client->host, client->port, strerror(errno));
    return fd;
}

static json_object *rpc(const Client *client, json_object *request) {
    if (!request) return NULL;
    if (client->token && client->token[0])
        json_object_object_add(request, "auth_token", json_object_new_string(client->token));
    const char *json = json_object_to_json_string_ext(request, JSON_C_TO_STRING_PLAIN);
    if (!json) { if (!cli_rpc_quiet) fprintf(stderr, "ASGEND: cannot serialize request\n"); return NULL; }
    size_t request_len = strlen(json);
    char *wire = malloc(request_len + 2);
    if (!wire) { if (!cli_rpc_quiet) fprintf(stderr, "ASGEND: out of memory\n"); return NULL; }
    memcpy(wire, json, request_len); wire[request_len] = '\n'; wire[request_len + 1] = '\0';
    int fd = connect_core(client);
    if (fd < 0) { free(wire); return NULL; }
    if (send_all(fd, wire, request_len + 1) != 0) {
        if (!cli_rpc_quiet) fprintf(stderr, "ASGEND: writing request failed: %s\n", strerror(errno));
        close(fd); free(wire); return NULL;
    }
    free(wire);
    size_t used = 0, capacity = 4096;
    char *response = malloc(capacity);
    if (!response) { close(fd); if (!cli_rpc_quiet) fprintf(stderr, "ASGEND: out of memory\n"); return NULL; }
    for (;;) {
        if (used + 2048 + 1 > capacity) {
            size_t next_capacity = capacity * 2;
            if (next_capacity > RESPONSE_LIMIT + 1) next_capacity = RESPONSE_LIMIT + 1;
            if (next_capacity <= capacity) { if (!cli_rpc_quiet) fprintf(stderr, "ASGEND: server response exceeded 16 MiB\n"); free(response); close(fd); return NULL; }
            char *next = realloc(response, next_capacity);
            if (!next) { free(response); close(fd); if (!cli_rpc_quiet) fprintf(stderr, "ASGEND: out of memory\n"); return NULL; }
            response = next; capacity = next_capacity;
        }
        ssize_t n = recv(fd, response + used, capacity - used - 1, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { if (!cli_rpc_quiet) fprintf(stderr, "ASGEND: reading response failed: %s\n", strerror(errno)); free(response); close(fd); return NULL; }
        if (n == 0) break;
        used += (size_t)n; response[used] = '\0';
        if (memchr(response, '\n', used)) break;
        if (used >= RESPONSE_LIMIT) { if (!cli_rpc_quiet) fprintf(stderr, "ASGEND: server response exceeded 16 MiB\n"); free(response); close(fd); return NULL; }
    }
    close(fd);
    if (!used) { free(response); if (!cli_rpc_quiet) fprintf(stderr, "ASGEND: core closed connection without a response\n"); return NULL; }
    char *line_end = memchr(response, '\n', used);
    size_t line_len = line_end ? (size_t)(line_end - response) : used;
    json_tokener *tok = json_tokener_new();
    if (!tok) { free(response); return NULL; }
    /* json-c's STRICT flag rejects valid raw UTF-8 in strings in the supported
       library version. Validate UTF-8 explicitly, then enforce one complete JSON
       object and whitespace-only trailing bytes ourselves. */
    json_tokener_set_flags(tok, JSON_TOKENER_VALIDATE_UTF8);
    json_object *reply = json_tokener_parse_ex(tok, response, (int)line_len);
    enum json_tokener_error parse_error = json_tokener_get_error(tok);
    size_t parse_end = json_tokener_get_parse_end(tok);
    bool trailing_ok = true;
    for (size_t i = parse_end; i < line_len; ++i)
        if (response[i] != ' ' && response[i] != '\t' && response[i] != '\r' && response[i] != '\n') { trailing_ok = false; break; }
    bool valid = reply && parse_error == json_tokener_success && trailing_ok &&
                 json_object_get_type(reply) == json_type_object;
    json_tokener_free(tok);
    if (!valid) {
        if (reply) json_object_put(reply);
        /* Do not echo the response body: it can contain private task/model data. */
        if (!cli_rpc_quiet) fprintf(stderr, "ASGEND: core returned invalid JSON (%zu bytes; parse_end=%zu; error=%s)\n",
                line_len, parse_end, json_tokener_error_desc(parse_error));
        free(response);
        return NULL;
    }
    free(response);
    json_object *ok = obj_get(reply, "ok");
    if (ok && json_object_get_type(ok) == json_type_boolean && !json_object_get_boolean(ok)) {
        if (!cli_rpc_quiet) fprintf(stderr, "ASGEND: %s\n", str_get(reply, "error", "request failed"));
        json_object_put(reply); return NULL;
    }
    return reply;
}

static json_object *request_op(const char *op) {
    json_object *request = json_object_new_object();
    if (request) json_object_object_add(request, "op", json_object_new_string(op));
    return request;
}

static void *cli_tui_plan_watch_main(void *opaque) {
    Client client = *(const Client *)opaque;
    free(opaque);
    /* plan_wait is a long poll, but the socket timeout must remain bounded too:
     * stop_plan_watch() joins this thread synchronously during TUI shutdown. */
    cli_rpc_timeout_seconds = 2;
    cli_rpc_quiet = true;
    int64_t revision = 0;
    while (!atomic_load_explicit(&cli_tui_plan_watch_stop, memory_order_relaxed)) {
        json_object *request = request_op("plan_wait");
        if (!request) break;
        json_object_object_add(request, "revision", json_object_new_int64(revision));
        /* Long-poll gives prompt delivery while keeping TUI shutdown responsive. */
        json_object_object_add(request, "timeout_ms", json_object_new_int(750));
        json_object *reply = rpc(&client, request);
        json_object_put(request);
        if (!reply) {
            if (atomic_load_explicit(&cli_tui_plan_watch_stop, memory_order_relaxed)) break;
            struct timespec pause = {.tv_sec = 0, .tv_nsec = 250000000L};
            (void)nanosleep(&pause, NULL);
            continue;
        }
        json_object *revision_value = obj_get(reply, "revision");
        if (revision_value && json_object_get_type(revision_value) == json_type_int)
            revision = json_object_get_int64(revision_value);
        bool changed = json_object_get_boolean(obj_get(reply, "changed"));
        bool stopping = json_object_get_boolean(obj_get(reply, "stopping"));
        json_object_put(reply);
        if (changed) atomic_store_explicit(&cli_tui_plan_refresh_pending, true, memory_order_release);
        if (stopping) break;
    }
    cli_rpc_quiet = false;
    cli_rpc_timeout_seconds = 60;
    return NULL;
}

static void cli_tui_start_plan_watch(const Client *client) {
    if (cli_tui_plan_watch_started || !client) return;
    Client *copy = malloc(sizeof(*copy));
    if (!copy) return;
    *copy = *client;
    atomic_store_explicit(&cli_tui_plan_refresh_pending, false, memory_order_release);
    atomic_store_explicit(&cli_tui_plan_watch_stop, false, memory_order_release);
    int rc = pthread_create(&cli_tui_plan_watch_thread, NULL, cli_tui_plan_watch_main, copy);
    if (rc != 0) {
        free(copy);
        return;
    }
    cli_tui_plan_watch_started = true;
}

static void cli_tui_stop_plan_watch(void) {
    if (!cli_tui_plan_watch_started) return;
    atomic_store_explicit(&cli_tui_plan_watch_stop, true, memory_order_release);
    (void)pthread_join(cli_tui_plan_watch_thread, NULL);
    cli_tui_plan_watch_started = false;
}

static json_object *request_task_op(const char *op, const char *task_id) {
    json_object *request = request_op(op);
    if (request) json_object_object_add(request, "task_id", json_object_new_string(task_id));
    return request;
}


#define CLI_TUI_EVENT_QUEUE_CAPACITY 256
#define CLI_TUI_EVENT_BATCH_MAX 64
#define CLI_TUI_EVENT_PAGE_LIMIT 100

typedef struct {
    Client client;
    char task_id[ASGEND_TUI_KEY_MAX];
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t event_space;
    atomic_bool stop;
    json_object *events[CLI_TUI_EVENT_QUEUE_CAPACITY];
    size_t event_head;
    size_t event_count;
    json_object *latest_status;
    uint64_t status_generation;
    bool finished;
    int result;
} CliTuiTaskWatch;

static void cli_tui_watch_queue_event(CliTuiTaskWatch *watch, json_object *event) {
    if (!watch || !event) return;
    pthread_mutex_lock(&watch->mutex);
    /* Never drop streamed text: apply backpressure to the watcher while the
     * render thread drains the bounded queue. The database remains the source
     * of truth, but this also preserves order within the fetched event batch. */
    while (watch->event_count == CLI_TUI_EVENT_QUEUE_CAPACITY &&
           !atomic_load_explicit(&watch->stop, memory_order_acquire)) {
        pthread_cond_wait(&watch->event_space, &watch->mutex);
    }
    if (atomic_load_explicit(&watch->stop, memory_order_acquire)) {
        pthread_mutex_unlock(&watch->mutex);
        return;
    }
    size_t at = (watch->event_head + watch->event_count) % CLI_TUI_EVENT_QUEUE_CAPACITY;
    watch->events[at] = json_object_get(event);
    ++watch->event_count;
    pthread_mutex_unlock(&watch->mutex);
}

static void cli_tui_watch_publish_status(CliTuiTaskWatch *watch, json_object *status) {
    if (!watch || !status) return;
    pthread_mutex_lock(&watch->mutex);
    if (watch->latest_status) json_object_put(watch->latest_status);
    watch->latest_status = json_object_get(status);
    ++watch->status_generation;
    const char *state = str_get(status, "status", "unknown");
    if (strcmp(state, "completed") == 0 || strcmp(state, "completed_without_model") == 0 ||
        strcmp(state, "failed") == 0 || strcmp(state, "cancelled") == 0) {
        /* Terminal status is not equivalent to a drained event stream. The
         * watcher marks finished only after it has polled a post-terminal page. */
        watch->result = strcmp(state, "failed") == 0 ? 1 : 0;
    }
    pthread_mutex_unlock(&watch->mutex);
}

static bool cli_tui_status_is_terminal(const char *state) {
    return state && (strcmp(state, "completed") == 0 ||
                     strcmp(state, "completed_without_model") == 0 ||
                     strcmp(state, "failed") == 0 ||
                     strcmp(state, "cancelled") == 0);
}

static void cli_tui_watch_mark_finished(CliTuiTaskWatch *watch) {
    if (!watch) return;
    pthread_mutex_lock(&watch->mutex);
    watch->finished = true;
    pthread_mutex_unlock(&watch->mutex);
}

static void *cli_tui_task_watch_main(void *opaque) {
    CliTuiTaskWatch *watch = opaque;
    int64_t last_event_id = 0;
    bool terminal_status_seen = false;
    cli_rpc_timeout_seconds = 2;
    cli_rpc_quiet = true;
    while (!atomic_load_explicit(&watch->stop, memory_order_acquire)) {
        /* If terminal status was seen on the previous iteration, this page is
         * the required post-terminal drain. Never stop on status alone: an
         * events page can race with the final event being committed. */
        bool terminal_before_page = terminal_status_seen;
        bool events_fetch_ok = false;
        size_t fetched_count = 0;
        json_object *events_request = request_task_op("events", watch->task_id);
        if (events_request) {
            json_object_object_add(events_request, "after_id", json_object_new_int64(last_event_id));
            json_object_object_add(events_request, "limit", json_object_new_int(CLI_TUI_EVENT_PAGE_LIMIT));
            json_object *events_reply = rpc(&watch->client, events_request);
            json_object_put(events_request);
            if (events_reply) {
                json_object *events = obj_get(events_reply, "events");
                if (events && json_object_get_type(events) == json_type_array) {
                    events_fetch_ok = true;
                    fetched_count = json_object_array_length(events);
                    for (size_t i = 0; i < fetched_count; ++i) {
                        json_object *event = json_object_array_get_idx(events, i);
                        int64_t id = json_object_get_int64(obj_get(event, "event_id"));
                        if (id > last_event_id) last_event_id = id;
                        cli_tui_watch_queue_event(watch, event);
                        if (atomic_load_explicit(&watch->stop, memory_order_acquire)) break;
                    }
                }
                json_object_put(events_reply);
            }
        }
        if (atomic_load_explicit(&watch->stop, memory_order_acquire)) break;

        json_object *status_request = request_task_op("status", watch->task_id);
        json_object *status = status_request ? rpc(&watch->client, status_request) : NULL;
        if (status_request) json_object_put(status_request);
        if (status) {
            const char *state = str_get(status, "status", "unknown");
            if (cli_tui_status_is_terminal(state)) terminal_status_seen = true;
            cli_tui_watch_publish_status(watch, status);
            json_object_put(status);
        }

        /* Do at least one event request after terminal status has been seen.
         * Continue across full pages, and retry on transient RPC failure. A
         * short/empty page after terminal status means all committed events
         * are now queued; only then may the UI see `finished`. */
        if (terminal_before_page && terminal_status_seen && events_fetch_ok &&
            fetched_count < CLI_TUI_EVENT_PAGE_LIMIT) {
            cli_tui_watch_mark_finished(watch);
            break;
        }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = terminal_status_seen ? 20000000L : 110000000L};
        (void)nanosleep(&pause, NULL);
    }
    cli_rpc_quiet = false;
    cli_rpc_timeout_seconds = 60;
    return NULL;
}

static json_object *cli_tui_watch_pop_event(CliTuiTaskWatch *watch) {
    if (!watch) return NULL;
    pthread_mutex_lock(&watch->mutex);
    json_object *event = NULL;
    if (watch->event_count) {
        event = watch->events[watch->event_head];
        watch->events[watch->event_head] = NULL;
        watch->event_head = (watch->event_head + 1) % CLI_TUI_EVENT_QUEUE_CAPACITY;
        --watch->event_count;
        pthread_cond_signal(&watch->event_space);
    }
    pthread_mutex_unlock(&watch->mutex);
    return event;
}

static json_object *cli_tui_watch_copy_status(CliTuiTaskWatch *watch, uint64_t *generation) {
    if (!watch) return NULL;
    pthread_mutex_lock(&watch->mutex);
    json_object *status = watch->latest_status ? json_object_get(watch->latest_status) : NULL;
    if (generation) *generation = watch->status_generation;
    pthread_mutex_unlock(&watch->mutex);
    return status;
}

static bool cli_tui_watch_finished(CliTuiTaskWatch *watch, int *result) {
    if (!watch) return true;
    pthread_mutex_lock(&watch->mutex);
    bool finished = watch->finished;
    if (result) *result = watch->result;
    pthread_mutex_unlock(&watch->mutex);
    return finished;
}

static void cli_tui_watch_stop(CliTuiTaskWatch *watch) {
    if (!watch) return;
    atomic_store_explicit(&watch->stop, true, memory_order_release);
    pthread_mutex_lock(&watch->mutex);
    pthread_cond_broadcast(&watch->event_space);
    pthread_mutex_unlock(&watch->mutex);
    (void)pthread_join(watch->thread, NULL);
    for (size_t i = 0; i < CLI_TUI_EVENT_QUEUE_CAPACITY; ++i) {
        if (watch->events[i]) json_object_put(watch->events[i]);
    }
    if (watch->latest_status) json_object_put(watch->latest_status);
    pthread_cond_destroy(&watch->event_space);
    pthread_mutex_destroy(&watch->mutex);
}

static void print_readable_value(json_object *value, const char *label, unsigned depth);

static void print_json(json_object *obj) {
    if (obj) print_readable_value(obj, NULL, 0);
}


static const char *human_key(const char *key) {
    static const struct { const char *key; const char *label; } names[] = {
        {"content", "Сообщение"}, {"summary", "Итог"}, {"answer", "Ответ"},
        {"question", "Вопрос"}, {"status", "Статус"}, {"reason", "Причина"},
        {"assumptions", "Допущения"}, {"known_information", "Известная информация"},
        {"constraints", "Ограничения"}, {"issues", "Замечания"}, {"defects", "Недочёты"},
        {"unverified_claims", "Не удалось проверить"}, {"changes", "Изменения"},
        {"score", "Оценка качества"}, {"output", "Результат"}, {"error", "Ошибка"},
        {"tool", "Инструмент"}, {"skills", "Навыки"}, {"system_prompts", "Инструкции"},
        {"tools", "Инструменты"}, {"knowledge", "Знания"}, {"model", "Модель"},
        {"title", "Этап"}, {"instruction", "Инструкция"}, {"acceptance_criteria", "Критерии готовности"},
        {"verification", "Проверка"}, {"review", "Ревью"}, {"defects", "Недочёты"},
        {"rationale", "Обоснование"}, {"fallback", "Резервный режим"}
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (strcmp(key, names[i].key) == 0) return names[i].label;
    return key;
}

static bool ignored_human_key(const char *key) {
    return strcmp(key, "id") == 0 || strcmp(key, "object") == 0 ||
           strcmp(key, "index") == 0 || strcmp(key, "finish_reason") == 0 ||
           strcmp(key, "subtask_id") == 0 || strcmp(key, "task_id") == 0 ||
           strcmp(key, "model_used") == 0 || strcmp(key, "agent_id") == 0;
}

static void print_readable_value(json_object *value, const char *label, unsigned depth);

static void print_readable_text(const char *text, const char *label, unsigned depth) {
    if (!text) return;
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') ++text;
    size_t n = strlen(text);
    if (n > 1 && (text[0] == '{' || text[0] == '[')) {
        json_object *parsed = json_tokener_parse(text);
        if (parsed && (json_object_get_type(parsed) == json_type_object || json_object_get_type(parsed) == json_type_array)) {
            json_object *type = obj_get(parsed, "type");
            json_object *needs_input = obj_get(parsed, "need_user_input");
            bool is_question = (type && json_object_get_type(type) == json_type_string &&
                                strcmp(json_object_get_string(type), "question") == 0) ||
                               (needs_input && json_object_get_type(needs_input) == json_type_boolean && json_object_get_boolean(needs_input));
            if (!is_question) print_readable_value(parsed, label, depth);
            json_object_put(parsed);
            return;
        }
        if (parsed) json_object_put(parsed);
    }
    for (unsigned i = 0; i < depth; ++i) fputs("  ", stdout);
    if (label && *label) printf("%s: ", label);
    puts(text);
}

static void print_readable_value(json_object *value, const char *label, unsigned depth) {
    if (!value || depth > 5) return;
    enum json_type type = json_object_get_type(value);
    if (type == json_type_string) {
        const char *text = json_object_get_string(value);
        if (depth < 5 && text && (*text == '{' || *text == '[')) {
            print_readable_text(text, label, depth);
            return;
        }
        for (unsigned i = 0; i < depth; ++i) fputs("  ", stdout);
        if (label && *label) printf("%s: ", label);
        if (text && *text) puts(text);
        else puts("—");
        return;
    }
    if (type == json_type_int || type == json_type_double || type == json_type_boolean) {
        for (unsigned i = 0; i < depth; ++i) fputs("  ", stdout);
        if (label && *label) printf("%s: ", label);
        if (type == json_type_boolean) puts(json_object_get_boolean(value) ? "Да" : "Нет");
        else puts(json_object_to_json_string_ext(value, JSON_C_TO_STRING_PLAIN));
        return;
    }
    if (type == json_type_array) {
        size_t count = json_object_array_length(value);
        if (!count) return;
        if (label && *label) {
            for (unsigned i = 0; i < depth; ++i) fputs("  ", stdout);
            printf("%s:\n", label);
        }
        for (size_t i = 0; i < count; ++i) {
            json_object *item = json_object_array_get_idx(value, i);
            if (json_object_get_type(item) == json_type_string) {
                const char *text = json_object_get_string(item);
                for (unsigned j = 0; j < depth + (label && *label ? 1U : 0U); ++j) fputs("  ", stdout);
                printf("• %s\n", text ? text : "");
            } else {
                print_readable_value(item, NULL, depth + (label && *label ? 1U : 0U));
            }
        }
        return;
    }
    if (type == json_type_object) {
        if (label && *label) {
            for (unsigned i = 0; i < depth; ++i) fputs("  ", stdout);
            printf("%s:\n", label);
        }
        json_object_object_foreach(value, key, child) {
            if (ignored_human_key(key)) continue;
            const char *display = human_key(key);
            if (json_object_get_type(child) == json_type_object || json_object_get_type(child) == json_type_array) {
                print_readable_value(child, display, depth + (label && *label ? 1U : 0U));
            } else {
                print_readable_value(child, display, depth + (label && *label ? 1U : 0U));
            }
        }
    }
}

static void print_chat_header(const char *role, const char *timestamp) {
    const char *stamp = timestamp ? strchr(timestamp, 'T') : NULL;
    stamp = stamp ? stamp + 1 : "";
    char time_buf[16] = "";
    if (*stamp) (void)snprintf(time_buf, sizeof(time_buf), "%.5s", stamp);
    if (isatty(STDOUT_FILENO)) printf("\n\033[1;36m%s%s%s\033[0m\n", role, *time_buf ? " · " : "", time_buf);
    else printf("\n%s%s%s\n", role, *time_buf ? " · " : "", time_buf);
}

static void print_chat_bubble(const char *role, const char *timestamp, const char *content) {
    if (!content || !*content) return;
    print_chat_header(role, timestamp);
    const char *line = content;
    while (*line) {
        const char *end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line);
        fputs("  ", stdout);
        if (length) fwrite(line, 1, length, stdout);
        putchar('\n');
        if (!end) break;
        line = end + 1;
    }
    putchar('\n');
}

static const char *event_title(const char *type) {
    static const struct { const char *type; const char *title; } titles[] = {
        {"orchestration_started", "Оркестратор начал выполнение"}, {"agent_selected", "Профиль исполнения выбран"},
        {"analysis_started", "Анализ задачи"}, {"decomposition_completed", "План проверен"},
        {"subtask_started", "Этап работы"}, {"resources_selected", "Ресурсы подобраны"},
        {"agent_started", "Выполнение этапа началось"}, {"agent_completed", "Этап завершён"},
        {"agent_llm_request", "Запрос к модели"}, {"model_selected", "Выбрана модель"},
        {"model_released", "Модель выгружена из памяти"}, {"model_release_warning", "Предупреждение выгрузки модели"},
        {"orchestrator_waiting_input", "Ожидается ваш ответ"}, {"task_resumed", "Продолжаю выполнение"},
        {"task_completed", "Задача завершена"}, {"task_failed", "Ошибка задачи"},
        {"task_cancel_requested", "Запрошена отмена"}, {"task_cancelled", "Задача отменена"}
    };
    for (size_t i = 0; i < sizeof(titles) / sizeof(titles[0]); ++i)
        if (strcmp(type, titles[i].type) == 0) return titles[i].title;
    return "Событие";
}

#define CLI_STREAM_STATE_CAPACITY 64

typedef struct {
    bool used;
    bool seen;
    bool line_open;
    bool completed;
    char stream_id[64];
    char model_name[96];
} CliThinkingOutputState;

typedef struct {
    bool used;
    bool completed;
    bool header_printed;
    char call_key[96];
    char stream_id[64];
    char model_name[96];
    char tool_name[96];
} CliToolOutputState;

static char cli_stream_id[64];
static bool cli_stream_open;
static bool cli_progress_visible;
static CliThinkingOutputState cli_thinking_outputs[CLI_STREAM_STATE_CAPACITY];
static CliThinkingOutputState *cli_current_thinking_output;
static CliToolOutputState cli_tool_outputs[CLI_STREAM_STATE_CAPACITY];
static int cli_stream_columns = 80;
static int cli_stream_column = 0;

static CliThinkingOutputState *cli_thinking_state(const char *stream_id, const char *model_name, bool create) {
    const char *sid = stream_id ? stream_id : "";
    const char *model = model_name && *model_name ? model_name : "Модель";
    CliThinkingOutputState *reusable = NULL;
    for (size_t i = 0; i < CLI_STREAM_STATE_CAPACITY; ++i) {
        CliThinkingOutputState *state = &cli_thinking_outputs[i];
        if (state->used && strcmp(state->stream_id, sid) == 0 && strcmp(state->model_name, model) == 0) {
            /* Compatibility producers may reuse a fallback stream ID such as
             * "active". A completed slot must start a fresh lifecycle when a
             * new delta arrives, otherwise subsequent text is silently ignored. */
            if (create && state->completed) {
                memset(state, 0, sizeof(*state));
                state->used = true;
                (void)snprintf(state->stream_id, sizeof(state->stream_id), "%s", sid);
                (void)snprintf(state->model_name, sizeof(state->model_name), "%s", model);
            }
            return state;
        }
        if (!reusable && (!state->used || state->completed)) reusable = state;
    }
    if (!create || !reusable) return NULL;
    memset(reusable, 0, sizeof(*reusable));
    reusable->used = true;
    (void)snprintf(reusable->stream_id, sizeof(reusable->stream_id), "%s", sid);
    (void)snprintf(reusable->model_name, sizeof(reusable->model_name), "%s", model);
    return reusable;
}

static void cli_thinking_break_current_line(void) {
    if (cli_current_thinking_output && cli_current_thinking_output->line_open) {
        putchar('\n');
        cli_current_thinking_output->line_open = false;
        cli_stream_column = 0;
    }
    cli_current_thinking_output = NULL;
}

static void cli_thinking_finish(const char *stream_id, const char *model_name, bool failed) {
    CliThinkingOutputState *state = cli_thinking_state(stream_id, model_name, false);
    if (!state || state->completed) return;
    if (cli_current_thinking_output == state) cli_thinking_break_current_line();
    if (state->seen) {
        printf("%s • Thinking %s\n", state->model_name, failed ? "×" : "→");
        cli_stream_column = 0;
    }
    state->completed = true;
}

static void cli_thinking_finish_all(bool failed) {
    for (size_t i = 0; i < CLI_STREAM_STATE_CAPACITY; ++i) {
        CliThinkingOutputState *state = &cli_thinking_outputs[i];
        if (state->used && !state->completed)
            cli_thinking_finish(state->stream_id, state->model_name, failed);
    }
}

static CliToolOutputState *cli_tool_state(const char *call_key, const char *stream_id,
                                           const char *model_name, const char *tool_name, bool create) {
    const char *key = call_key ? call_key : "";
    const char *sid = stream_id ? stream_id : "";
    const char *model = model_name && *model_name ? model_name : "Модель";
    const char *tool = tool_name && *tool_name ? tool_name : "инструмент";
    CliToolOutputState *reusable = NULL;
    for (size_t i = 0; i < CLI_STREAM_STATE_CAPACITY; ++i) {
        CliToolOutputState *state = &cli_tool_outputs[i];
        if (state->used && strcmp(state->call_key, key) == 0 &&
            strcmp(state->stream_id, sid) == 0 && strcmp(state->model_name, model) == 0) {
            /* Start a new lifecycle if the provider reuses a call identity. */
            if (create && state->completed) {
                memset(state, 0, sizeof(*state));
                state->used = true;
                (void)snprintf(state->call_key, sizeof(state->call_key), "%s", key);
                (void)snprintf(state->stream_id, sizeof(state->stream_id), "%s", sid);
                (void)snprintf(state->model_name, sizeof(state->model_name), "%s", model);
                (void)snprintf(state->tool_name, sizeof(state->tool_name), "%s", tool);
            }
            return state;
        }
        if (!reusable && (!state->used || state->completed)) reusable = state;
    }
    if (!create || !reusable) return NULL;
    memset(reusable, 0, sizeof(*reusable));
    reusable->used = true;
    (void)snprintf(reusable->call_key, sizeof(reusable->call_key), "%s", key);
    (void)snprintf(reusable->stream_id, sizeof(reusable->stream_id), "%s", sid);
    (void)snprintf(reusable->model_name, sizeof(reusable->model_name), "%s", model);
    (void)snprintf(reusable->tool_name, sizeof(reusable->tool_name), "%s", tool);
    return reusable;
}

static void cli_tool_finish_state(CliToolOutputState *state, bool failed) {
    if (!state || state->completed) return;
    printf("\n%s • Tool call %s · %s\n", state->model_name,
           failed ? "×" : "→", state->tool_name);
    state->completed = true;
    cli_stream_column = 0;
}

static void cli_tool_finish_stream(const char *stream_id, const char *model_name, bool failed) {
    const char *sid = stream_id ? stream_id : "";
    const char *model = model_name && *model_name ? model_name : "Модель";
    for (size_t i = 0; i < CLI_STREAM_STATE_CAPACITY; ++i) {
        CliToolOutputState *state = &cli_tool_outputs[i];
        if (state->used && !state->completed && strcmp(state->stream_id, sid) == 0 &&
            strcmp(state->model_name, model) == 0)
            cli_tool_finish_state(state, failed);
    }
}

static void cli_tool_finish_all(bool failed) {
    for (size_t i = 0; i < CLI_STREAM_STATE_CAPACITY; ++i)
        if (cli_tool_outputs[i].used && !cli_tool_outputs[i].completed)
            cli_tool_finish_state(&cli_tool_outputs[i], failed);
}

static void cli_progress_clear(void) {
    if (cli_progress_visible && isatty(STDOUT_FILENO)) {
        fputs("\r\033[2K", stdout);
        fflush(stdout);
    }
    cli_progress_visible = false;
}

static void cli_stream_close(void) {
    if (cli_stream_open) {
        cli_print_stream_rule();
        cli_stream_open = false;
        cli_stream_id[0] = '\0';
        cli_stream_column = 0;
    }
}

static int cli_terminal_columns(void) {
    if (!isatty(STDOUT_FILENO)) return 120;
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return (int)ws.ws_col;
    return 80;
}

static void cli_print_stream_rule(void) {
    int columns = isatty(STDOUT_FILENO) ? cli_terminal_columns() : 52;
    if (columns < 2) columns = 2;
    if (columns > 512) columns = 512;
    (void)columns;
    putchar('\n');
}

static void cli_write_stream_delta(const char *delta) {
    if (!delta) return;
    /* A terminal can be resized while a model is streaming. Refresh the width
     * before every delta rather than keeping the startup dimensions forever. */
    cli_stream_columns = cli_terminal_columns();
    if (cli_stream_columns < 1) cli_stream_columns = 1;
    const char *p = delta;
    mbstate_t state;
    memset(&state, 0, sizeof(state));
    while (*p) {
        if (*p == '\n') {
            putchar('\n'); cli_stream_column = 0; ++p;
            memset(&state, 0, sizeof(state));
            continue;
        }
        wchar_t wc = 0;
        size_t n = mbrtowc(&wc, p, MB_CUR_MAX, &state);
        if (n == (size_t)-1 || n == (size_t)-2 || n == 0) {
            /* Preserve an incomplete UTF-8 byte sequence rather than losing output. */
            memset(&state, 0, sizeof(state)); wc = (unsigned char)*p; n = 1;
        }
        int width = wcwidth(wc); if (width < 0) width = 1;
        if (cli_stream_column > 0 && cli_stream_column + width > cli_stream_columns) {
            putchar('\n'); cli_stream_column = 0;
            if (wc == L' ') { p += n; continue; }
        }
        fwrite(p, 1, n, stdout);
        cli_stream_column += width;
        p += n;
    }
}

static char *read_line(const char *prompt) {
    char *line = NULL;
    size_t capacity = 0;
    printf("%s", prompt ? prompt : "");
    fflush(stdout);
    ssize_t n = getline(&line, &capacity, stdin);
    if (n < 0) { free(line); return NULL; }
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
    return line;
}

static char *read_secret_line(const char *prompt) {
    if (!isatty(STDIN_FILENO)) return read_line(prompt);
    struct termios original, hidden;
    if (tcgetattr(STDIN_FILENO, &original) != 0) {
        fprintf(stderr, "ASGEND: cannot read terminal settings for hidden token input\n");
        return NULL;
    }
    hidden = original;
    hidden.c_lflag &= (tcflag_t)~ECHO;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden) != 0) {
        fprintf(stderr, "ASGEND: cannot disable terminal echo for token input\n");
        return NULL;
    }

    char *line = NULL;
    size_t capacity = 0;
    printf("%s", prompt ? prompt : "");
    fflush(stdout);
    ssize_t n = getline(&line, &capacity, stdin);
    int read_errno = errno;
    /* Always restore terminal echo before printing errors or returning. */
    (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
    putchar('\n');
    if (n < 0) {
        free(line);
        errno = read_errno;
        return NULL;
    }
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
    return line;
}

static void print_task_list(const Client *client) {
    json_object *request = request_op("list_tasks");
    if (!request) return;
    json_object_object_add(request, "limit", json_object_new_int(50));
    json_object *reply = rpc(client, request); json_object_put(request);
    if (!reply) return;
    json_object *tasks = obj_get(reply, "tasks");
    if (!tasks || json_object_get_type(tasks) != json_type_array || json_object_array_length(tasks) == 0) {
        puts("Задач пока нет."); json_object_put(reply); return;
    }
    puts("\nID                                      STATUS             PRIORITY  CREATED");
    puts("--------------------------------------------------------------------------------");
    for (size_t i = 0; i < json_object_array_length(tasks); ++i) {
        json_object *task = json_object_array_get_idx(tasks, i);
        printf("%-39.39s %-18.18s %-8d %s\n",
            str_get(task, "task_id", "—"), str_get(task, "status", "unknown"),
            json_object_get_int(obj_get(task, "priority")), str_get(task, "created_at", ""));
    }
    json_object_put(reply);
}

static void cancel_task(const Client *client, const char *task_id) {
    json_object *request = request_task_op("cancel", task_id);
    json_object *reply = rpc(client, request);
    if (request) json_object_put(request);
    if (reply) { printf("Отмена запрошена для %s.\n", task_id); json_object_put(reply); }
}

static bool prompt_one_question(json_object *question, size_t question_number,
                                const char *task_id, const Client *client,
                                char **answer_out, bool *cancelled) {
    *answer_out = NULL;
    printf("\nУточнение %zu\n%s\n",
           question_number, str_get(question, "question", "Агенту нужна дополнительная информация."));
    json_object *options = obj_get(question, "options");
    size_t option_count = options && json_object_get_type(options) == json_type_array ? json_object_array_length(options) : 0;
    for (size_t i = 0; i < option_count; ++i) {
        json_object *option = json_object_array_get_idx(options, i);
        if (json_object_get_type(option) == json_type_object)
            printf("  %zu) %s\n", i + 1, str_get(option, "label", str_get(option, "id", "Вариант")));
        else if (json_object_get_type(option) == json_type_string)
            printf("  %zu) %s\n", i + 1, json_object_get_string(option));
    }
    puts("Введите номер варианта или свободный ответ. Команда /cancel отменяет задачу.");
    for (;;) {
        char *line = read_line("Ответ> ");
        if (!line) { puts("Ввод закрыт; задача остаётся в waiting_input."); return false; }
        size_t length = strlen(line);
        while (length && (line[length - 1] == ' ' || line[length - 1] == '\t')) line[--length] = '\0';
        char *start = line; while (*start == ' ' || *start == '\t') ++start;
        if (strcmp(start, "/cancel") == 0) { cancel_task(client, task_id); if (cancelled) *cancelled = true; free(line); return true; }
        if (!*start) { puts("Ответ не должен быть пустым."); free(line); continue; }
        const char *answer = start;
        char *end = NULL; errno = 0; long picked = strtol(start, &end, 10);
        if (!errno && end && *end == '\0' && picked >= 1 && (size_t)picked <= option_count) {
            json_object *option = json_object_array_get_idx(options, (size_t)picked - 1);
            if (json_object_get_type(option) == json_type_object)
                answer = str_get(option, "label", str_get(option, "id", start));
            else if (json_object_get_type(option) == json_type_string) answer = json_object_get_string(option);
        }
        *answer_out = strdup(answer ? answer : start);
        free(line);
        return *answer_out != NULL;
    }
}

static bool show_question_and_answer(const Client *client, const char *task_id,
                                     json_object *question_payload, bool *cancelled) {
    if (cancelled) *cancelled = false;
    json_object *questions = obj_get(question_payload, "questions");
    bool array_form = questions && json_object_get_type(questions) == json_type_array;
    size_t count = array_form ? json_object_array_length(questions) : 1;
    if (count == 0) {
        json_object *single = obj_get(question_payload, "question");
        if (single && json_object_get_type(single) == json_type_object) { question_payload = single; count = 1; array_form = false; }
        else { count = 1; array_form = false; }
    }
    if (count > 8) count = 8;
    char *answers[8] = {0};
    const char *questions_text[8] = {0};
    bool cancelled_local = false;
    for (size_t i = 0; i < count; ++i) {
        json_object *question = array_form ? json_object_array_get_idx(questions, i) : question_payload;
        questions_text[i] = str_get(question, "question", "Уточните недостающие сведения.");
        bool okay = prompt_one_question(question, i + 1, task_id, client, &answers[i], &cancelled_local);
        if (cancelled_local) { if (cancelled) *cancelled = true; for (size_t j=0;j<count;j++) free(answers[j]); return true; }
        if (!okay) { for (size_t j=0;j<count;j++) free(answers[j]); return false; }
    }
    size_t cap = 10000;
    for (size_t i=0;i<count;i++) cap += strlen(answers[i] ? answers[i] : "") + strlen(questions_text[i] ? questions_text[i] : "") + 32;
    if (cap > 100000) cap = 100000;
    char *combined = calloc(1, cap);
    if (!combined) { for (size_t i=0;i<count;i++) free(answers[i]); return false; }
    size_t used = 0;
    if (count == 1) used = (size_t)snprintf(combined, cap, "%s", answers[0] ? answers[0] : "");
    else {
        used = (size_t)snprintf(combined, cap, "Ответы на уточняющие вопросы:");
        for (size_t i=0;i<count && used<cap-1;i++) {
            int n = snprintf(combined+used,cap-used,"\n%zu. %s — %s",i+1,questions_text[i]?questions_text[i]:"Вопрос",answers[i]?answers[i]:"");
            if(n<0||(size_t)n>=cap-used){used=cap-1;break;}used+=(size_t)n;
        }
    }
    json_object *request = request_task_op("answer", task_id);
    if (!request) { free(combined); for(size_t i=0;i<count;i++)free(answers[i]); return false; }
    json_object_object_add(request, "answer", json_object_new_string_len(combined, (int)strlen(combined)));
    json_object *reply = rpc(client, request);
    json_object_put(request); free(combined); for(size_t i=0;i<count;i++)free(answers[i]);
    if (!reply) return false;
    printf("Ответ%s отправлен; задача продолжится.\n\n", count > 1 ? "ы" : "");
    json_object_put(reply); return true;
}

static const char *status_label(const char *status);

static void cli_tui_upsert(const char *key, AsgendTuiTileKind kind,
                           const char *title, const char *body,
                           bool append, bool streaming, bool expanded) {
    if (cli_tui_live)
        (void)asgend_tui_upsert_tile(&cli_tui, key, kind, title, body,
                                    append, streaming, expanded);
}

static void cli_tui_close_key(const char *key) {
    if (!cli_tui_live || !key || !*key) return;
    for (size_t i = 0; i < cli_tui.tile_count; ++i) {
        AsgendTuiTile *tile = &cli_tui.tiles[i];
        if (strcmp(tile->key, key) != 0) continue;
        tile->streaming = false;
        char *down = strstr(tile->title, "↓");
        if (down) memcpy(down, "→", strlen("→"));
        if (cli_tui.display_mode == ASGEND_TUI_DISPLAY_DYNAMIC && tile->kind != ASGEND_TUI_TILE_CHAT)
            tile->expanded = false;
        break;
    }
    asgend_tui_render(&cli_tui);
}

static const char *cli_tui_status_label(const char *value) {
    return status_label(value);
}

typedef struct {
    const char *text;
    size_t length;
} CliTuiLineSlice;

static void cli_tui_close_preview(void) {
    free(cli_tui_preview_baseline);
    cli_tui_preview_baseline = NULL;
    cli_tui_preview_baseline_path[0] = '\0';
    asgend_tui_close_file_preview(&cli_tui);
}

static CliTuiLineSlice *cli_tui_split_lines(const char *text, size_t length, size_t *count_out) {
    size_t capacity = 1;
    for (size_t i = 0; i < length; ++i) if (text[i] == '\n') ++capacity;
    CliTuiLineSlice *lines = calloc(capacity, sizeof(*lines));
    if (!lines) return NULL;
    size_t count = 0, start = 0;
    for (size_t i = 0; i < length; ++i) {
        if (text[i] != '\n') continue;
        size_t end = i;
        if (end > start && text[end - 1] == '\r') --end;
        lines[count++] = (CliTuiLineSlice){.text = text + start, .length = end - start};
        start = i + 1;
    }
    if (start < length || count == 0) {
        size_t end = length;
        if (end > start && text[end - 1] == '\r') --end;
        lines[count++] = (CliTuiLineSlice){.text = text + start, .length = end - start};
    }
    *count_out = count;
    return lines;
}

static bool cli_tui_line_equal(CliTuiLineSlice a, CliTuiLineSlice b) {
    return a.length == b.length && memcmp(a.text, b.text, a.length) == 0;
}

static void cli_tui_diff_append(char *out, size_t cap, size_t *used, const char *prefix, CliTuiLineSlice line) {
    if (*used >= cap - 1) return;
    int n = snprintf(out + *used, cap - *used, "%s%.*s\n", prefix,
                     (int)(line.length > INT_MAX ? INT_MAX : line.length), line.text);
    if (n < 0) return;
    if ((size_t)n >= cap - *used) *used = cap - 1;
    else *used += (size_t)n;
}

static char *cli_tui_build_diff(const char *before, const char *after, size_t *length_out) {
    size_t before_len = strlen(before), after_len = strlen(after);
    size_t a_count = 0, b_count = 0;
    CliTuiLineSlice *a = cli_tui_split_lines(before, before_len, &a_count);
    CliTuiLineSlice *b = cli_tui_split_lines(after, after_len, &b_count);
    if (!a || !b) { free(a); free(b); return NULL; }
    size_t prefix = 0;
    while (prefix < a_count && prefix < b_count && cli_tui_line_equal(a[prefix], b[prefix])) ++prefix;
    size_t suffix = 0;
    while (suffix < a_count - prefix && suffix < b_count - prefix &&
           cli_tui_line_equal(a[a_count - suffix - 1], b[b_count - suffix - 1])) ++suffix;
    if (prefix == a_count && prefix == b_count) { free(a); free(b); return NULL; }
    size_t context_before = prefix < 3 ? prefix : 3;
    size_t context_after = suffix < 3 ? suffix : 3;
    size_t cap = before_len + after_len + 512;
    char *out = calloc(1, cap);
    if (!out) { free(a); free(b); return NULL; }
    size_t used = (size_t)snprintf(out, cap, "Изменения файла\n--- до изменений\n+++ после изменений\n@@ строки %zu, -%zu +%zu @@\n",
                                  prefix + 1, a_count - prefix - suffix, b_count - prefix - suffix);
    for (size_t i = prefix - context_before; i < prefix; ++i) cli_tui_diff_append(out, cap, &used, "  ", a[i]);
    for (size_t i = prefix; i < a_count - suffix; ++i) cli_tui_diff_append(out, cap, &used, "- ", a[i]);
    for (size_t i = prefix; i < b_count - suffix; ++i) cli_tui_diff_append(out, cap, &used, "+ ", b[i]);
    for (size_t i = a_count - suffix; i < a_count - suffix + context_after; ++i) {
        if (i < a_count) cli_tui_diff_append(out, cap, &used, "  ", a[i]);
    }
    free(a); free(b);
    if (length_out) *length_out = strlen(out);
    return out;
}

static void cli_tui_preview_content(const char *path, const char *content) {
    if (!path || !content) return;
    if (strcmp(cli_tui_preview_baseline_path, path) != 0 || !cli_tui_preview_baseline) {
        free(cli_tui_preview_baseline);
        cli_tui_preview_baseline = strdup(content);
        (void)snprintf(cli_tui_preview_baseline_path, sizeof(cli_tui_preview_baseline_path), "%s", path);
    }
    char *diff = NULL;
    if (cli_tui_preview_baseline && strcmp(cli_tui_preview_baseline, content) != 0)
        diff = cli_tui_build_diff(cli_tui_preview_baseline, content, NULL);
    asgend_tui_show_file_preview(&cli_tui, path, diff ? diff : content);
    free(diff);
}

static void cli_tui_refresh_mcp(const Client *client) {
    json_object *request = request_op("mcp_status");
    json_object *reply = request ? rpc(client, request) : NULL;
    if (request) json_object_put(request);
    if (!reply) return;
    AsgendTuiMcpItem items[ASGEND_TUI_MCP_MAX];
    memset(items, 0, sizeof(items));
    size_t count = 0;
    json_object *servers = obj_get(reply, "servers");
    if (servers && json_object_get_type(servers) == json_type_array) {
        size_t n = json_object_array_length(servers);
        if (n > ASGEND_TUI_MCP_MAX) n = ASGEND_TUI_MCP_MAX;
        for (size_t i = 0; i < n; ++i) {
            json_object *row = json_object_array_get_idx(servers, i);
            (void)snprintf(items[count].name, sizeof(items[count].name), "%s", str_get(row, "name", "MCP"));
            (void)snprintf(items[count].transport, sizeof(items[count].transport), "%s", str_get(row, "transport", ""));
            (void)snprintf(items[count].status, sizeof(items[count].status), "%s", str_get(row, "status", "unknown"));
            items[count].connected = json_object_get_boolean(obj_get(row, "connected"));
            ++count;
        }
    }
    asgend_tui_set_mcp_servers(&cli_tui, items, count);
    json_object_put(reply);
}

static void cli_tui_refresh_files(const Client *client, const char *scope_id) {
    if (!cli_tui_live) return;
    if (!scope_id || !*scope_id || strcmp(scope_id, "global") == 0) {
        if (cli_tui_files_scope[0] || cli_tui.preview_path[0]) cli_tui_close_preview();
        cli_tui_files_scope[0] = '\0';
        asgend_tui_set_workspace_path(&cli_tui, "");
        asgend_tui_set_project_path(&cli_tui, "Выберите задачу");
        asgend_tui_set_files(&cli_tui, NULL, 0);
        return;
    }
    if (strcmp(cli_tui_files_scope, scope_id) != 0) {
        (void)snprintf(cli_tui_files_scope, sizeof(cli_tui_files_scope), "%s", scope_id);
        cli_tui_close_preview();
        asgend_tui_set_workspace_path(&cli_tui, "");
        cli_tui.selected_file_index = 0;
        cli_tui.file_scroll_offset = 0;
    }
    json_object *request = request_op("workspace_list");
    if (!request) return;
    json_object_object_add(request, "task_id", json_object_new_string(scope_id));
    json_object_object_add(request, "path", json_object_new_string(asgend_tui_workspace_path(&cli_tui)));
    json_object *reply = rpc(client, request);
    json_object_put(request);
    if (!reply) return;
    AsgendTuiFileItem files[ASGEND_TUI_ITEMS_MAX];
    size_t count = 0;
    json_object *entries = obj_get(reply, "entries");
    if (entries && json_object_get_type(entries) == json_type_array) {
        size_t n = json_object_array_length(entries);
        if (n > ASGEND_TUI_ITEMS_MAX) n = ASGEND_TUI_ITEMS_MAX;
        for (size_t i = 0; i < n; ++i) {
            json_object *entry = json_object_array_get_idx(entries, i);
            (void)snprintf(files[count].path, sizeof(files[count].path), "%s", str_get(entry, "name", ""));
            (void)snprintf(files[count].kind, sizeof(files[count].kind), "%s", str_get(entry, "kind", "file"));
            files[count].size = json_object_get_int64(obj_get(entry, "size"));
            ++count;
        }
    }
    asgend_tui_set_files(&cli_tui, files, count);
    json_object_put(reply);
}

static bool cli_tui_show_workspace_chunk(const Client *client, const char *path,
                                         int64_t offset, bool start_at_end) {
    if (!client || !path || !*path || !cli_tui_files_scope[0]) return false;
    json_object *request = request_op("workspace_read_chunk");
    if (!request) return false;
    json_object_object_add(request, "task_id", json_object_new_string(cli_tui_files_scope));
    json_object_object_add(request, "path", json_object_new_string(path));
    json_object_object_add(request, "offset", json_object_new_int64(offset));
    json_object_object_add(request, "limit", json_object_new_int(32768));
    json_object *reply = rpc(client, request);
    json_object_put(request);
    if (!reply) {
        asgend_tui_show_file_preview(&cli_tui, path, "Не удалось прочитать текстовый фрагмент workspace.");
        return false;
    }
    json_object *content = obj_get(reply, "content");
    bool ok = json_object_get_boolean(obj_get(reply, "ok")) && content &&
              json_object_get_type(content) == json_type_string;
    if (ok) {
        asgend_tui_show_file_preview_chunk(&cli_tui, path, json_object_get_string(content),
            json_object_get_int64(obj_get(reply, "size")),
            json_object_get_int64(obj_get(reply, "offset")),
            json_object_get_int64(obj_get(reply, "next_offset")),
            json_object_get_boolean(obj_get(reply, "eof")), start_at_end);
    } else {
        char message[1024];
        (void)snprintf(message, sizeof(message), "Файл не открыт: %s",
                       str_get(reply, "error", "ответ ядра не содержит текст"));
        asgend_tui_show_file_preview(&cli_tui, path, message);
    }
    json_object_put(reply);
    return ok;
}

static bool cli_tui_show_workspace_file(const Client *client, const char *path) {
    cli_tui_preview_offset_count = 0;
    if (!client || !path || !*path || !cli_tui_files_scope[0]) return false;
    json_object *request = request_op("workspace_read");
    if (request) {
        json_object_object_add(request, "task_id", json_object_new_string(cli_tui_files_scope));
        json_object_object_add(request, "path", json_object_new_string(path));
    }
    json_object *reply = request ? rpc(client, request) : NULL;
    if (request) json_object_put(request);
    json_object *content = obj_get(reply, "content");
    bool ok = reply && json_object_get_boolean(obj_get(reply, "ok")) && content &&
              json_object_get_type(content) == json_type_string;
    if (ok) {
        const char *text = json_object_get_string(content);
        int64_t size = json_object_get_int64(obj_get(reply, "size"));
        if (size >= 0 && size <= (int64_t)CLI_WORKSPACE_FILE_MAX) {
            cli_tui_preview_content(path, text);
            json_object_put(reply);
            return true;
        }
    }
    if (reply) json_object_put(reply);
    return cli_tui_show_workspace_chunk(client, path, 0, false);
}

static void cli_tui_refresh_open_preview(const Client *client) {
    if (!cli_tui.preview_path[0]) return;
    char path[ASGEND_TUI_PATH_MAX];
    (void)snprintf(path, sizeof(path), "%s", cli_tui.preview_path);
    if (!cli_tui_files_scope[0]) {
        cli_tui_close_preview();
        return;
    }

    json_object *request = request_op("workspace_read");
    if (!request) return;
    json_object_object_add(request, "task_id", json_object_new_string(cli_tui_files_scope));
    json_object_object_add(request, "path", json_object_new_string(path));
    json_object *reply = rpc(client, request);
    json_object_put(request);
    json_object *content = obj_get(reply, "content");
    bool ok = reply && json_object_get_boolean(obj_get(reply, "ok")) && content &&
              json_object_get_type(content) == json_type_string;
    int64_t size = reply ? json_object_get_int64(obj_get(reply, "size")) : -1;
    if (ok && size >= 0 && size <= (int64_t)CLI_WORKSPACE_FILE_MAX) {
        cli_tui_preview_content(path, json_object_get_string(content));
        json_object_put(reply);
        return;
    }
    if (reply) json_object_put(reply);

    /* Large files are opened through chunk reads; refresh the same viewport
     * instead of leaving stale contents when a tool modifies the file. */
    int64_t offset = cli_tui.preview_chunk_offset;
    if (offset < 0) offset = 0;
    (void)cli_tui_show_workspace_chunk(client, path, offset, false);
}

static void cli_tui_maybe_stream_preview(const Client *client) {
    int action = asgend_tui_preview_stream_action(&cli_tui);
    if (action > 0 && cli_tui.preview_content && !cli_tui.preview_eof &&
        cli_tui.preview_next_offset > cli_tui.preview_chunk_offset) {
        if (cli_tui_preview_offset_count >= sizeof(cli_tui_preview_offsets) / sizeof(cli_tui_preview_offsets[0])) {
            memmove(cli_tui_preview_offsets, cli_tui_preview_offsets + 1,
                    (cli_tui_preview_offset_count - 1) * sizeof(cli_tui_preview_offsets[0]));
            --cli_tui_preview_offset_count;
        }
        cli_tui_preview_offsets[cli_tui_preview_offset_count++] = cli_tui.preview_chunk_offset;
        char path[ASGEND_TUI_PATH_MAX];
        (void)snprintf(path, sizeof(path), "%s", cli_tui.preview_path);
        (void)cli_tui_show_workspace_chunk(client, path, cli_tui.preview_next_offset, false);
    } else if (action < 0 && cli_tui_preview_offset_count > 0 && cli_tui.preview_path[0]) {
        int64_t previous = cli_tui_preview_offsets[--cli_tui_preview_offset_count];
        char path[ASGEND_TUI_PATH_MAX];
        (void)snprintf(path, sizeof(path), "%s", cli_tui.preview_path);
        (void)cli_tui_show_workspace_chunk(client, path, previous, true);
    }
}

static void cli_tui_open_selected_file(const Client *client) {
    if (!cli_tui_live || cli_tui.focused_pane != 0) return;
    AsgendTuiFileItem file;
    if (!asgend_tui_selected_file(&cli_tui, &file)) return;
    const char *scope = cli_tui_files_scope[0] ? cli_tui_files_scope : "global";
    if (strcmp(file.kind, "directory") == 0) {
        if (asgend_tui_enter_selected_directory(&cli_tui)) cli_tui_refresh_files(client, scope);
        return;
    }
    if (strcmp(file.kind, "file") != 0) return;
    if (cli_tui_files_scope[0]) (void)cli_tui_show_workspace_file(client, file.path);
}

static void cli_tui_open_selected_chat_file(const Client *client) {
    if (!cli_tui_live || cli_tui.focused_pane != 1 || cli_tui.selected_tile >= cli_tui.tile_count) return;
    AsgendTuiTile *tile = &cli_tui.tiles[cli_tui.selected_tile];
    if (tile->kind == ASGEND_TUI_TILE_FILE) {
        const char *path = tile->title;
        while (*path == ' ') ++path;
        if (!*path) path = tile->body;
        if (!path || !*path) return;
        if (cli_tui_files_scope[0]) (void)cli_tui_show_workspace_file(client, path);
    } else {
        asgend_tui_toggle_selected(&cli_tui);
    }
}

static bool cli_tui_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Split an editor command without a shell: quoting and backslash escapes work,
 * but substitutions, pipes and command execution are intentionally unsupported. */
static int cli_tui_parse_editor_command(char *storage, char **argv, size_t argv_capacity) {
    if (!storage || !argv || argv_capacity < 2) return -1;
    char *readp = storage;
    char *writep = storage;
    size_t argc = 0;
    while (*readp) {
        while (cli_tui_is_space(*readp)) ++readp;
        if (!*readp) break;
        if (argc + 1 >= argv_capacity) return -1;
        argv[argc++] = writep;
        char quote = '\0';
        bool escaped = false;
        while (*readp) {
            char c = *readp++;
            if (escaped) {
                *writep++ = c;
                escaped = false;
                continue;
            }
            if (c == '\\' && quote != '\'') {
                escaped = true;
                continue;
            }
            if (quote) {
                if (c == quote) quote = '\0';
                else *writep++ = c;
                continue;
            }
            if (c == '\'' || c == '"') {
                quote = c;
                continue;
            }
            if (cli_tui_is_space(c)) break;
            *writep++ = c;
        }
        if (quote || escaped) return -1;
        *writep++ = '\0';
    }
    argv[argc] = NULL;
    return argc ? (int)argc : -1;
}

static int cli_tui_write_all(int fd, const char *data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        ssize_t n = write(fd, data + offset, size - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        offset += (size_t)n;
    }
    return 0;
}

static bool cli_tui_valid_utf8(const unsigned char *data, size_t length) {
    size_t i = 0;
    while (i < length) {
        unsigned char c = data[i++];
        if (c <= 0x7f) continue;
        if (c >= 0xc2 && c <= 0xdf) {
            if (i >= length || (data[i++] & 0xc0) != 0x80) return false;
        } else if (c == 0xe0) {
            if (i + 1 >= length || data[i] < 0xa0 || data[i] > 0xbf ||
                (data[i + 1] & 0xc0) != 0x80) return false;
            i += 2;
        } else if ((c >= 0xe1 && c <= 0xec) || (c >= 0xee && c <= 0xef)) {
            if (i + 1 >= length || (data[i] & 0xc0) != 0x80 ||
                (data[i + 1] & 0xc0) != 0x80) return false;
            i += 2;
        } else if (c == 0xed) {
            if (i + 1 >= length || data[i] < 0x80 || data[i] > 0x9f ||
                (data[i + 1] & 0xc0) != 0x80) return false;
            i += 2;
        } else if (c == 0xf0) {
            if (i + 2 >= length || data[i] < 0x90 || data[i] > 0xbf ||
                (data[i + 1] & 0xc0) != 0x80 || (data[i + 2] & 0xc0) != 0x80) return false;
            i += 3;
        } else if (c >= 0xf1 && c <= 0xf3) {
            if (i + 2 >= length || (data[i] & 0xc0) != 0x80 ||
                (data[i + 1] & 0xc0) != 0x80 || (data[i + 2] & 0xc0) != 0x80) return false;
            i += 3;
        } else if (c == 0xf4) {
            if (i + 2 >= length || data[i] < 0x80 || data[i] > 0x8f ||
                (data[i + 1] & 0xc0) != 0x80 || (data[i + 2] & 0xc0) != 0x80) return false;
            i += 3;
        } else return false;
    }
    return true;
}

static bool cli_tui_read_edited_file(const char *path, char **content_out, size_t *length_out) {
    if (!path || !content_out || !length_out) return false;
    *content_out = NULL;
    *length_out = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size > CLI_WORKSPACE_FILE_MAX) {
        close(fd);
        return false;
    }
    /* Restore private permissions in case the editor replaced the temp file. */
    if (fchmod(fd, 0600) != 0) { close(fd); return false; }
    size_t length = (size_t)st.st_size;
    char *content = malloc(length + 1);
    if (!content) { close(fd); return false; }
    size_t used = 0;
    while (used < length) {
        ssize_t n = read(fd, content + used, length - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { free(content); close(fd); return false; }
        used += (size_t)n;
    }
    close(fd);
    content[length] = '\0';
    if (memchr(content, '\0', length) || !cli_tui_valid_utf8((const unsigned char *)content, length)) {
        free(content);
        return false;
    }
    *content_out = content;
    *length_out = length;
    return true;
}

static void cli_tui_cleanup_editor_temp(const char *directory, const char *path) {
    if (path && *path) (void)unlink(path);
    if (directory && *directory) (void)rmdir(directory);
}

static void cli_tui_editor_status(const char *message) {
    (void)snprintf(cli_tui.status, sizeof(cli_tui.status), "%s", message ? message : "");
    asgend_tui_render(&cli_tui);
}

static void cli_tui_edit_previewed_file(const Client *client) {
    if (!cli_tui_live || cli_tui.suspended || !cli_tui.preview_content ||
        !cli_tui.preview_path[0] || !cli_tui_files_scope[0]) {
        cli_tui_editor_status("Откройте файл в панели задачи перед редактированием");
        return;
    }

    char path[ASGEND_TUI_PATH_MAX];
    (void)snprintf(path, sizeof(path), "%s", cli_tui.preview_path);
    if (cli_tui.preview_file_size > (int64_t)CLI_WORKSPACE_FILE_MAX) {
        cli_tui_editor_status("Потоковый просмотр доступен, редактирование ограничено 1 MiB");
        return;
    }
    json_object *read_request = request_op("workspace_read");
    if (!read_request) { cli_tui_editor_status("Не удалось подготовить чтение файла"); return; }
    json_object_object_add(read_request, "task_id", json_object_new_string(cli_tui_files_scope));
    json_object_object_add(read_request, "path", json_object_new_string(path));
    json_object *read_reply = rpc(client, read_request);
    json_object_put(read_request);
    json_object *original_value = obj_get(read_reply, "content");
    if (!read_reply || !json_object_get_boolean(obj_get(read_reply, "ok")) || !original_value ||
        json_object_get_type(original_value) != json_type_string) {
        if (read_reply) json_object_put(read_reply);
        cli_tui_editor_status("Не удалось загрузить исходный текст; файл не изменён");
        return;
    }
    size_t original_length = (size_t)json_object_get_string_len(original_value);
    if (original_length > CLI_WORKSPACE_FILE_MAX) {
        json_object_put(read_reply);
        cli_tui_editor_status("Редактор поддерживает текстовые файлы до 1 MiB");
        return;
    }
    char *original = malloc(original_length + 1);
    if (!original) {
        json_object_put(read_reply);
        cli_tui_editor_status("Недостаточно памяти для редактирования файла");
        return;
    }
    memcpy(original, json_object_get_string(original_value), original_length);
    original[original_length] = '\0';
    json_object_put(read_reply);

    char temporary_dir[] = "/tmp/asgend-edit-XXXXXX";
    if (!mkdtemp(temporary_dir)) {
        free(original);
        cli_tui_editor_status("Не удалось создать временный файл для редактора");
        return;
    }
    char temporary[sizeof(temporary_dir) + 16];
    int temporary_length = snprintf(temporary, sizeof(temporary), "%s/content", temporary_dir);
    if (temporary_length < 0 || (size_t)temporary_length >= sizeof(temporary)) {
        cli_tui_cleanup_editor_temp(temporary_dir, NULL);
        free(original);
        cli_tui_editor_status("Не удалось подготовить путь временного файла");
        return;
    }
    int temp_fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (temp_fd < 0) {
        cli_tui_cleanup_editor_temp(temporary_dir, NULL);
        free(original);
        cli_tui_editor_status("Не удалось создать временный файл для редактора");
        return;
    }
    bool temp_ok = fchmod(temp_fd, 0600) == 0 &&
                   cli_tui_write_all(temp_fd, original, original_length) == 0 && fsync(temp_fd) == 0;
    if (close(temp_fd) != 0) temp_ok = false;
    if (!temp_ok) {
        cli_tui_cleanup_editor_temp(temporary_dir, temporary);
        free(original);
        cli_tui_editor_status("Не удалось подготовить временную копию; файл не изменён");
        return;
    }

    const char *configured_editor = getenv("VISUAL");
    if (!configured_editor || !*configured_editor) configured_editor = getenv("EDITOR");
    if (!configured_editor || !*configured_editor) configured_editor = "vi";
    char command[CLI_EDITOR_COMMAND_MAX];
    if (strlen(configured_editor) >= sizeof(command)) {
        cli_tui_cleanup_editor_temp(temporary_dir, temporary);
        free(original);
        cli_tui_editor_status("Команда редактора слишком длинная");
        return;
    }
    (void)snprintf(command, sizeof(command), "%s", configured_editor);
    char *editor_argv[CLI_EDITOR_ARGV_MAX];
    int editor_argc = cli_tui_parse_editor_command(command, editor_argv,
                                                   sizeof(editor_argv) / sizeof(editor_argv[0]));
    if (editor_argc < 0 || (size_t)editor_argc + 1 >= CLI_EDITOR_ARGV_MAX) {
        cli_tui_cleanup_editor_temp(temporary_dir, temporary);
        free(original);
        cli_tui_editor_status("Некорректная команда редактора; используйте VISUAL или EDITOR без shell-конструкций");
        return;
    }
    editor_argv[editor_argc] = temporary;
    editor_argv[editor_argc + 1] = NULL;

    asgend_tui_suspend(&cli_tui);
    pid_t child = fork();
    if (child == 0) {
        execvp(editor_argv[0], editor_argv);
        dprintf(STDERR_FILENO, "ASGEND: не удалось запустить редактор '%s': %s\n",
                editor_argv[0], strerror(errno));
        _exit(127);
    }
    int child_status = -1;
    if (child > 0) {
        while (waitpid(child, &child_status, 0) < 0) {
            if (errno == EINTR) continue;
            child_status = -1;
            break;
        }
    }
    if (cli_tui.suspended) asgend_tui_resume(&cli_tui);

    if (child <= 0 || !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        cli_tui_cleanup_editor_temp(temporary_dir, temporary);
        free(original);
        cli_tui_editor_status("Редактор завершился с ошибкой; исходный файл сохранён без изменений");
        (void)cli_tui_show_workspace_file(client, path);
        return;
    }

    char *edited = NULL;
    size_t edited_length = 0;
    bool edited_ok = cli_tui_read_edited_file(temporary, &edited, &edited_length);
    cli_tui_cleanup_editor_temp(temporary_dir, temporary);
    if (!edited_ok) {
        free(original);
        cli_tui_editor_status("Результат редактора не является UTF-8 текстом до 1 MiB; файл не изменён");
        (void)cli_tui_show_workspace_file(client, path);
        return;
    }
    if (edited_length == original_length && memcmp(edited, original, original_length) == 0) {
        free(edited);
        free(original);
        cli_tui_editor_status("Изменений нет");
        (void)cli_tui_show_workspace_file(client, path);
        return;
    }
    free(original);

    json_object *write_request = request_op("workspace_write");
    if (write_request) {
        json_object_object_add(write_request, "task_id", json_object_new_string(cli_tui_files_scope));
        json_object_object_add(write_request, "path", json_object_new_string(path));
        json_object_object_add(write_request, "content", json_object_new_string_len(edited, (int)edited_length));
    }
    free(edited);
    json_object *write_reply = write_request ? rpc(client, write_request) : NULL;
    if (write_request) json_object_put(write_request);
    if (!write_reply || !json_object_get_boolean(obj_get(write_reply, "ok"))) {
        if (write_reply) json_object_put(write_reply);
        cli_tui_editor_status("Не удалось сохранить файл; проверьте сообщение ядра");
        (void)cli_tui_show_workspace_file(client, path);
        return;
    }
    json_object_put(write_reply);
    cli_tui_refresh_files(client, cli_tui_files_scope);
    (void)cli_tui_show_workspace_file(client, path);
    cli_tui_editor_status("Сохранено");
}

static void cli_tui_go_parent_directory(const Client *client) {
    if (!cli_tui_live || cli_tui.focused_pane != 0) return;
    if (asgend_tui_parent_directory(&cli_tui))
        cli_tui_refresh_files(client, cli_tui_files_scope[0] ? cli_tui_files_scope : "global");
}

static void cli_tui_refresh_data(const Client *client, const char *scope_id) {
    if (!cli_tui_live) return;
    AsgendTuiTaskItem tasks[ASGEND_TUI_ITEMS_MAX];
    size_t task_count = 0;
    json_object *request = request_op("list_tasks");
    if (request) {
        json_object_object_add(request, "limit", json_object_new_int(ASGEND_TUI_ITEMS_MAX));
        json_object *reply = rpc(client, request);
        json_object_put(request);
        if (reply) {
            json_object *array = obj_get(reply, "tasks");
            if (array && json_object_get_type(array) == json_type_array) {
                size_t n = json_object_array_length(array);
                if (n > ASGEND_TUI_ITEMS_MAX) n = ASGEND_TUI_ITEMS_MAX;
                for (size_t i = 0; i < n; ++i) {
                    json_object *row = json_object_array_get_idx(array, i);
                    (void)snprintf(tasks[task_count].id, sizeof(tasks[task_count].id), "%s", str_get(row, "task_id", ""));
                    (void)snprintf(tasks[task_count].title, sizeof(tasks[task_count].title), "%s", str_get(row, "title", str_get(row, "task_id", "Задача")));
                    (void)snprintf(tasks[task_count].status, sizeof(tasks[task_count].status), "%s", cli_tui_status_label(str_get(row, "status", "unknown")));
                    (void)snprintf(tasks[task_count].created_at, sizeof(tasks[task_count].created_at), "%s", str_get(row, "created_at", ""));
                    ++task_count;
                }
            }
            json_object_put(reply);
        }
    }
    asgend_tui_set_tasks(&cli_tui, tasks, task_count);

    json_object *plan_request = request_op("plan_list");
    if (plan_request) {
        json_object_object_add(plan_request, "scope_id", json_object_new_string(scope_id && *scope_id ? scope_id : "global"));
        json_object *reply = rpc(client, plan_request);
        json_object_put(plan_request);
        if (reply) {
            AsgendTuiPlanItem items[ASGEND_TUI_ITEMS_MAX];
            size_t count = 0;
            json_object *array = obj_get(reply, "items");
            if (array && json_object_get_type(array) == json_type_array) {
                size_t n = json_object_array_length(array);
                if (n > ASGEND_TUI_ITEMS_MAX) n = ASGEND_TUI_ITEMS_MAX;
                for (size_t i = 0; i < n; ++i) {
                    json_object *row = json_object_array_get_idx(array, i);
                    items[count].id = json_object_get_int64(obj_get(row, "item_id"));
                    (void)snprintf(items[count].kind, sizeof(items[count].kind), "%s", str_get(row, "kind", "todo"));
                    (void)snprintf(items[count].title, sizeof(items[count].title), "%s", str_get(row, "title", ""));
                    (void)snprintf(items[count].status, sizeof(items[count].status), "%s", str_get(row, "status", "open"));
                    ++count;
                }
            }
            asgend_tui_set_plan(&cli_tui, items, count);
            json_object_put(reply);
        }
    }
    if (!scope_id || !*scope_id || strcmp(scope_id, "global") == 0)
        cli_tui_refresh_files(client, "global");
    else
        cli_tui_refresh_files(client, scope_id);
    cli_tui_refresh_mcp(client);
    asgend_tui_set_task(&cli_tui, strcmp(scope_id ? scope_id : "global", "global") == 0 ? "" : scope_id,
                        "Готов к работе");
}

static void cli_tui_apply_plan_notification(const Client *client) {
    if (!atomic_exchange_explicit(&cli_tui_plan_refresh_pending, false, memory_order_acq_rel)) return;
    if (!cli_tui_live) return;
    if (cli_tui.suspended) {
        atomic_store_explicit(&cli_tui_plan_refresh_pending, true, memory_order_release);
        return;
    }
    cli_tui_refresh_data(client, cli_tui_scope);
}

static void cli_tui_add_plan(const Client *client, const char *kind) {
    char scope[ASGEND_TUI_KEY_MAX];
    (void)snprintf(scope, sizeof(scope), "%s", cli_tui_scope);
    asgend_tui_suspend(&cli_tui);
    char prompt[128];
    (void)snprintf(prompt, sizeof(prompt), "%s: ", strcmp(kind, "goal") == 0 ? "Новая цель" : "Новый пункт To-do");
    char *title = read_line(prompt);
    if (title && *title) {
        json_object *request = request_op("plan_add");
        if (request) {
            json_object_object_add(request, "scope_id", json_object_new_string(scope));
            json_object_object_add(request, "kind", json_object_new_string(kind));
            json_object_object_add(request, "title", json_object_new_string(title));
            json_object *reply = rpc(client, request);
            json_object_put(request);
            if (reply) json_object_put(reply);
        }
    }
    free(title);
    asgend_tui_resume(&cli_tui);
    cli_tui_refresh_data(client, scope);
}

static void cli_tui_toggle_plan(const Client *client) {
    AsgendTuiPlanItem item;
    if (!asgend_tui_selected_plan(&cli_tui, &item)) {
        (void)asgend_tui_upsert_tile(&cli_tui, "notice:plan", ASGEND_TUI_TILE_ACTION,
                                    "План", "Сначала выберите To-do или Goal стрелками и фокусом Tab.",
                                    false, false, true);
        return;
    }
    json_object *request = request_op("plan_update");
    if (!request) return;
    json_object_object_add(request, "scope_id", json_object_new_string(cli_tui_scope));
    json_object_object_add(request, "item_id", json_object_new_int64(item.id));
    json_object_object_add(request, "status", json_object_new_string(strcmp(item.status, "done") == 0 ? "open" : "done"));
    json_object *reply = rpc(client, request);
    json_object_put(request);
    if (reply) json_object_put(reply);
    cli_tui_refresh_data(client, cli_tui_scope);
}

static void cli_tui_tool_output_summary(const char *output, char *dst, size_t dst_size) {
    if (!dst || dst_size == 0) return;
    dst[0] = '\0';
    if (!output || !*output) {
        (void)snprintf(dst, dst_size, "Результат готов.");
        return;
    }
    const char *trim = output;
    while (*trim == ' ' || *trim == '\t' || *trim == '\r' || *trim == '\n') ++trim;
    size_t length = strlen(trim);
    if (*trim == '{' || *trim == '[') {
        json_object *parsed = json_tokener_parse(trim);
        const char *human = NULL;
        if (parsed && json_object_get_type(parsed) == json_type_object) {
            static const char *const fields[] = {"summary", "message", "content", "result_text", "human_summary", NULL};
            for (size_t i = 0; fields[i]; ++i) {
                json_object *value = NULL;
                if (json_object_object_get_ex(parsed, fields[i], &value) && value &&
                    json_object_get_type(value) == json_type_string && *json_object_get_string(value)) {
                    human = json_object_get_string(value);
                    break;
                }
            }
        }
        if (human) (void)snprintf(dst, dst_size, "%s", human);
        else (void)snprintf(dst, dst_size, "Структурированный результат скрыт (%zu байт JSON).", length);
        if (parsed) json_object_put(parsed);
        return;
    }
    size_t cap = dst_size - 1;
    if (cap > 640) cap = 640;
    size_t n = strlen(trim);
    if (n > cap) {
        n = cap;
        while (n > 0 && (((unsigned char)trim[n] & 0xC0) == 0x80)) --n;
    }
    memcpy(dst, trim, n);
    dst[n] = '\0';
    while (n > 0 && (dst[n - 1] == '\n' || dst[n - 1] == '\r' || dst[n - 1] == ' ')) dst[--n] = '\0';
    if (strlen(trim) > n && n + 4 < dst_size) strcat(dst, "…");
}

static bool cli_tui_is_orchestrator_group_event(const char *type) {
    if (!type) return false;
    static const char *const grouped[] = {
        "orchestration_started", "agent_selected", "profile_selected", "analysis_started",
        "decomposition_completed", "resources_selected", "model_selected", "task_resumed",
        "orchestrator_waiting_input", NULL
    };
    for (size_t i = 0; grouped[i]; ++i)
        if (strcmp(type, grouped[i]) == 0) return true;
    return false;
}

static bool cli_tui_tile_exists(const char *key) {
    if (!key) return false;
    for (size_t i = 0; i < cli_tui.tile_count; ++i)
        if (strcmp(cli_tui.tiles[i].key, key) == 0) return true;
    return false;
}

static bool cli_tui_render_event(json_object *event) {
    const char *type = str_get(event, "type", "event");
    const char *message = str_get(event, "message", "");
    const char *timestamp = str_get(event, "created_at", "");
    json_object *payload = obj_get(event, "payload");
    const char *model_in_event = str_get(payload, "model_name", "");
    if (model_in_event[0]) (void)snprintf(cli_tui_last_model, sizeof(cli_tui_last_model), "%.159s", model_in_event);
    if (strcmp(type, "task_completed") == 0 || strcmp(type, "task_failed") == 0 ||
        strcmp(type, "task_cancelled") == 0) {
        cli_tui_close_key("orchestrator:summary");
    }
    if (strcmp(type, "assistant_delta") == 0) {
        const char *stream_id = str_get(payload, "stream_id", "active");
        const char *model = str_get(payload, "model_name", "Модель");
        const char *delta = str_get(payload, "delta", "");
        char key[ASGEND_TUI_KEY_MAX], title[ASGEND_TUI_TITLE_MAX];
        cli_tui_stream_key(key, sizeof(key), "assistant", model, stream_id);
        (void)snprintf(title, sizeof(title), "%.112s • Streaming ↓", model);
        cli_tui_upsert(key, ASGEND_TUI_TILE_CHAT, title, delta, true, true, true);
        return delta[0] != '\0';
    }
    if (strcmp(type, "llm_thinking_delta") == 0) {
        const char *stream_id = str_get(payload, "stream_id", "active");
        const char *model = str_get(payload, "model_name", "Модель");
        const char *delta = str_get(payload, "delta", "");
        char key[ASGEND_TUI_KEY_MAX], title[ASGEND_TUI_TITLE_MAX];
        cli_tui_stream_key(key, sizeof(key), "thinking", model, stream_id);
        (void)snprintf(title, sizeof(title), "%.112s • Thinking ↓", model);
        bool exists = cli_tui_tile_exists(key);
        const char *body = delta[0] ? delta : "Обрабатывает токены рассуждения…";
        if (exists && !delta[0]) {
            for (size_t i = 0; i < cli_tui.tile_count; ++i) {
                if (strcmp(cli_tui.tiles[i].key, key) == 0) { body = cli_tui.tiles[i].body; break; }
            }
        }
        cli_tui_upsert(key, ASGEND_TUI_TILE_THINKING, title, body,
                       exists && delta[0] != '\0', true, true);
        return false;
    }
    if (strcmp(type, "tool_call_stream_delta") == 0) {
        const char *call_key = str_get(payload, "call_key", str_get(payload, "stream_id", "active"));
        const char *name = str_get(payload, "tool_name", "инструмент");
        const char *model = str_get(payload, "model_name", "Модель");
        const char *argument_delta = str_get(payload, "argument_delta", "");
        const char *phase = str_get(payload, "phase", "writing");
        char key[ASGEND_TUI_KEY_MAX], title[ASGEND_TUI_TITLE_MAX], body[256];
        cli_tui_stream_key(key, sizeof(key), "tool-form", model, call_key);
        (void)snprintf(title, sizeof(title), "%.82s • Tool call ↓ · %.42s", model, name);
        (void)snprintf(body, sizeof(body), "Формирование вызова инструмента · %s", phase);
        bool exists = cli_tui_tile_exists(key);
        /* Keep the live tile empty until its first argument fragment, so the
         * actual streamed JSON starts at byte zero rather than after a placeholder. */
        const char *tile_body = argument_delta[0] ? argument_delta : "";
        if (exists && !argument_delta[0]) {
            for (size_t i = 0; i < cli_tui.tile_count; ++i)
                if (strcmp(cli_tui.tiles[i].key, key) == 0) { tile_body = cli_tui.tiles[i].body; break; }
        }
        cli_tui_upsert(key, ASGEND_TUI_TILE_TOOL, title, tile_body,
                       exists && argument_delta[0] != '\0', true, true);
        return false;
    }
    if (strcmp(type, "orchestrator_model_delta") == 0) {
        const char *model = str_get(payload, "model_name", "Модель");
        const char *stream_id = str_get(payload, "stream_id", "active");
        char key[ASGEND_TUI_KEY_MAX], title[ASGEND_TUI_TITLE_MAX];
        cli_tui_stream_key(key, sizeof(key), "orchestrator-stream", model, stream_id);
        (void)snprintf(title, sizeof(title), "Оркестратор · %.96s ↓", model);
        cli_tui_upsert(key, ASGEND_TUI_TILE_ACTION, title,
                       "Оркестратор обрабатывает внутренний этап…", false, true, true);
        return false;
    }
    if (strcmp(type, "llm_status") == 0) {
        const char *state = str_get(payload, "status", "unknown");
        const char *stream_id = str_get(payload, "stream_id", "active");
        const char *stream_kind = str_get(payload, "stream_kind", "assistant");
        const char *model = str_get(payload, "model_name", cli_tui_last_model);
        char body[512], key[ASGEND_TUI_KEY_MAX], title[ASGEND_TUI_TITLE_MAX];
        (void)snprintf(body, sizeof(body), "%s%s%s", state, message[0] ? " — " : "", message);
        if (strcmp(state, "completed") == 0 || strcmp(state, "cancelled") == 0) {
            const char *key_kind = strcmp(stream_kind, "orchestrator") == 0 ? "orchestrator-stream" :
                                   strcmp(stream_kind, "thinking") == 0 ? "thinking" : "assistant";
            /* A stream can emit both reasoning text and visible answer/planner text;
             * close each matching tile so no spinner survives completion. */
            cli_tui_stream_key(key, sizeof(key), "thinking", model, stream_id);
            cli_tui_close_key(key);
            cli_tui_stream_key(key, sizeof(key), key_kind, model, stream_id);
            cli_tui_close_key(key);
            cli_tui_close_model_stream_tools(model, stream_id);
        } else if (strcmp(state, "HTTP-error") == 0 || strcmp(state, "stalled") == 0) {
            cli_tui_stream_key(key, sizeof(key), "llm-status", model, stream_id);
            (void)snprintf(title, sizeof(title), "%.100s • %s", model, state);
            cli_tui_upsert(key, ASGEND_TUI_TILE_PROGRESS, title, body, false, false, true);
        } else {
            cli_tui_stream_key(key, sizeof(key), "llm-status", model, stream_id);
            (void)snprintf(title, sizeof(title), "%.100s • %s", model, state);
            cli_tui_upsert(key, ASGEND_TUI_TILE_PROGRESS, title, body, false, true, false);
        }
        return false;
    }
    if (strcmp(type, "task_created") == 0 || strcmp(type, "user_answer") == 0) {
        const char *content = strcmp(type, "user_answer") == 0 ? str_get(payload, "answer", message) : message;
        char key[ASGEND_TUI_KEY_MAX]; (void)snprintf(key, sizeof(key), "user:%s:%zu", timestamp, strlen(content));
        cli_tui_upsert(key, ASGEND_TUI_TILE_CHAT, "Вы", content, false, false, true);
        return false;
    }
    if (strcmp(type, "agent_message") == 0) {
        const char *content = str_get(payload, "content", message);
        const char *trim = content; while (*trim == ' ' || *trim == '\t' || *trim == '\n' || *trim == '\r') ++trim;
        json_object *parsed = (*trim == '{' || *trim == '[') ? json_tokener_parse(trim) : NULL;
        if (parsed) {
            const char *kind = str_get(parsed, "type", "");
            json_object *need = obj_get(parsed, "need_user_input");
            if (strcmp(kind, "question") == 0 || (need && json_object_get_type(need) == json_type_boolean && json_object_get_boolean(need))) {
                json_object_put(parsed); return false;
            }
            const char *human = str_get(parsed, "summary", str_get(parsed, "answer", str_get(parsed, "content", str_get(parsed, "output", NULL))));
            if (human && *human) cli_tui_upsert("assistant:structured", ASGEND_TUI_TILE_CHAT, cli_tui_last_model, human, false, false, true);
            else cli_tui_upsert("assistant:structured", ASGEND_TUI_TILE_ACTION, "Структурированный результат", "Получен структурированный результат; сырой JSON в чате скрыт.", false, false, false);
            json_object_put(parsed); return human && *human;
        }
        char key[ASGEND_TUI_KEY_MAX]; (void)snprintf(key, sizeof(key), "agent-message:%llu", (unsigned long long)json_object_get_int64(obj_get(event, "event_id")));
        cli_tui_upsert(key, ASGEND_TUI_TILE_CHAT, cli_tui_last_model, content, false, false, true);
        return content && *content;
    }
    if (strcmp(type, "tool_started") == 0) {
        const char *tool = str_get(payload, "tool", message[0] ? message : "инструмент");
        const char *model = str_get(payload, "model_name", cli_tui_last_model);
        const char *call_key = str_get(payload, "call_key", str_get(payload, "stream_id", tool));
        char source_key[ASGEND_TUI_KEY_MAX], key[ASGEND_TUI_KEY_MAX], title[ASGEND_TUI_TITLE_MAX];
        cli_tui_stream_key(source_key, sizeof(source_key), "tool-form", model, call_key);
        cli_tui_stream_key(key, sizeof(key), "tool-running", model, call_key);
        (void)snprintf(title, sizeof(title), "%.76s • Tool call ↓ %.48s", model, tool);
        bool reused = false;
        for (size_t i = 0; i < cli_tui.tile_count; ++i) {
            AsgendTuiTile *tile = &cli_tui.tiles[i];
            if (strcmp(tile->key, source_key) != 0) continue;
            (void)snprintf(tile->key, sizeof(tile->key), "%s", key);
            (void)snprintf(tile->title, sizeof(tile->title), "%.72s • Tool call ↓ %.54s", model, tool);
            free(tile->body);
            tile->body = strdup("Выполнение инструмента…");
            tile->streaming = true;
            tile->kind = ASGEND_TUI_TILE_TOOL;
            tile->expanded = true;
            reused = true;
            break;
        }
        if (!reused)
            cli_tui_upsert(key, ASGEND_TUI_TILE_TOOL, title,
                           "Выполнение инструмента…", false, true, true);
        else asgend_tui_render(&cli_tui);
        return false;
    }
    if (strcmp(type, "tool_completed") == 0) {
        const char *tool = str_get(payload, "tool", "инструмент");
        const char *error = str_get(payload, "error", "");
        const char *output = str_get(payload, "output", "");
        const char *model = str_get(payload, "model_name", cli_tui_last_model);
        const char *call_key = str_get(payload, "call_key", "");
        char key[ASGEND_TUI_KEY_MAX], title[ASGEND_TUI_TITLE_MAX], body[1024];
        if (call_key[0]) cli_tui_stream_key(key, sizeof(key), "tool-running", model, call_key);
        else (void)snprintf(key, sizeof(key), "tool-running:%08x:%s",
                            (unsigned)cli_tui_model_fingerprint(model), tool);
        (void)snprintf(title, sizeof(title), "%.76s • Tool call → %.48s", model, tool);
        char output_summary[760];
        cli_tui_tool_output_summary(output, output_summary, sizeof(output_summary));
        (void)snprintf(body, sizeof(body), "%s%s", error[0] ? "Ошибка: " : "Завершено. ", error[0] ? error : output_summary);
        cli_tui_upsert(key, ASGEND_TUI_TILE_TOOL, title, body, false, false, false);
        return false;
    }
    if (strstr(type, "file") || strstr(type, "workspace") || strstr(type, "artifact")) {
        char key[ASGEND_TUI_KEY_MAX], title[ASGEND_TUI_TITLE_MAX], body[1024];
        const char *path = str_get(payload, "path", str_get(payload, "file_path", str_get(payload, "relative_path", "")));
        (void)snprintf(key, sizeof(key), "file:%s:%llu", type, (unsigned long long)json_object_get_int64(obj_get(event, "event_id")));
        (void)snprintf(title, sizeof(title), "%s", path[0] ? path : event_title(type));
        (void)snprintf(body, sizeof(body), "%s%s%s", message, path[0] ? " · " : "", path);
        cli_tui_upsert(key, ASGEND_TUI_TILE_FILE, title, body, false, false, true);
        return false;
    }
    if (cli_tui_is_orchestrator_group_event(type)) {
        const char *key = "orchestrator:summary";
        bool append = cli_tui_tile_exists(key);
        bool active = strcmp(type, "orchestrator_waiting_input") != 0;
        char body[1024];
        (void)snprintf(body, sizeof(body), "%s• %s%s%s%s%s",
                       append ? "\n" : "", event_title(type), message[0] ? " — " : "", message,
                       timestamp[0] ? " · " : "", timestamp);
        /* Every lifecycle/profile/analysis record is a child row in one
         * subdued orchestrator surface. Preserve history across a question
         * pause; do not spin while waiting for the user, resume on task_resumed. */
        cli_tui_upsert(key, ASGEND_TUI_TILE_ACTION,
                       active ? "Оркестратор ↓" : "Оркестратор →", body,
                       append, active, active);
        return false;
    }
    const char *kind = (strstr(type, "progress") || strstr(type, "status") || strstr(type, "waiting")) ? "progress" : "action";
    char key[ASGEND_TUI_KEY_MAX], title[ASGEND_TUI_TITLE_MAX], body[1024];
    (void)snprintf(key, sizeof(key), "event:%llu", (unsigned long long)json_object_get_int64(obj_get(event, "event_id")));
    (void)snprintf(title, sizeof(title), "%s", event_title(type));
    (void)snprintf(body, sizeof(body), "%s%s%s", message, timestamp[0] ? " · " : "", timestamp);
    cli_tui_upsert(key, strcmp(kind, "progress") == 0 ? ASGEND_TUI_TILE_PROGRESS : ASGEND_TUI_TILE_ACTION,
                   title, body, false, false, false);
    return false;
}

static bool print_event(json_object *event) {
    if (cli_tui_live) return cli_tui_render_event(event);
    cli_progress_clear();
    const char *type = str_get(event, "type", "event");
    const char *message = str_get(event, "message", "");
    const char *timestamp = str_get(event, "created_at", "");
    json_object *payload = obj_get(event, "payload");
    /* cli_stream_open tracks visible assistant text only; tool-call deltas are
     * a separate stream and must not remain attached to the answer line. */
    bool continuing_stream = strcmp(type, "assistant_delta") == 0;
    if (cli_stream_open && !continuing_stream) cli_stream_close();
    /* End the current physical line when another event is interleaved, but
     * keep that stream active until its own llm_status completion event. */
    if (strcmp(type, "llm_thinking_delta") != 0) cli_thinking_break_current_line();
    if (strcmp(type, "orchestrator_model_delta") == 0) {
        return false; /* Never leak planner/routing JSON into the user-facing chat. */
    }
    if (strcmp(type, "llm_status") == 0) {
        const char *status = str_get(payload, "status", "unknown");
        const char *stream_id = str_get(payload, "stream_id", "");
        const char *stream_kind = str_get(payload, "stream_kind", "assistant");
        const char *model_name = str_get(payload, "model_name", "Модель");
        if (strcmp(status, "connecting") == 0) {
            cli_stream_close();
            printf("  ◦ Подключаюсь к модели%s%s\n", message[0] ? ": " : "", message);
        } else if (strcmp(status, "waiting-first-token") == 0) {
            puts("  ◦ Модель подключена; ожидаю первый токен…");
        } else if (strcmp(status, "stalled") == 0) {
            cli_thinking_finish(stream_id, model_name, true);
            cli_tool_finish_stream(stream_id, model_name, true);
            printf("  ! Поток модели приостановился: %s\n", message[0] ? message : "нет новых токенов");
        } else if (strcmp(status, "HTTP-error") == 0 || strcmp(status, "cancelled") == 0) {
            cli_thinking_finish(stream_id, model_name, true);
            cli_tool_finish_stream(stream_id, model_name, true);
            cli_stream_close();
            fprintf(stderr, "  ! Ошибка запроса модели: %s\n", message[0] ? message : "неизвестная ошибка");
        } else if (strcmp(status, "completed") == 0) {
            cli_thinking_finish(stream_id, model_name, false);
            cli_tool_finish_stream(stream_id, model_name, false);
            if (strcmp(stream_kind, "orchestrator") == 0) {
                cli_stream_close();
                puts("  ◦ Оркестратор →");
                return false;
            }
            if (cli_stream_open && (!stream_id[0] || strcmp(cli_stream_id, stream_id) == 0)) cli_stream_close();
            else printf("  ✓ %s\n", message[0] ? message : "Ответ модели получен");
            json_object *token_count = obj_get(payload, "completion_tokens");
            if (token_count && json_object_get_type(token_count) == json_type_int && json_object_get_int(token_count) >= 0)
                printf("    Использовано токенов ответа: %d\n", json_object_get_int(token_count));
        }
        return false;
    }
    if (strcmp(type, "task_completed") == 0 || strcmp(type, "task_failed") == 0 ||
        strcmp(type, "task_cancelled") == 0) {
        bool failed = strcmp(type, "task_completed") != 0;
        cli_thinking_finish_all(failed);
        cli_tool_finish_all(failed);
    }
    if (strcmp(type, "llm_thinking_delta") == 0) {
        const char *delta = str_get(payload, "delta", "");
        const char *stream_id = str_get(payload, "stream_id", "");
        const char *model_name = str_get(payload, "model_name", str_get(payload, "model", "Модель"));
        json_object *delta_bytes_value = obj_get(payload, "delta_bytes");
        size_t delta_bytes = delta_bytes_value && json_object_get_type(delta_bytes_value) == json_type_int &&
                             json_object_get_int64(delta_bytes_value) > 0
                             ? (size_t)json_object_get_int64(delta_bytes_value) : strlen(delta);
        if (delta_bytes == 0) return false;
        CliThinkingOutputState *state = cli_thinking_state(stream_id, model_name, true);
        if (!state || state->completed) return false;
        if (cli_current_thinking_output != state) {
            cli_thinking_break_current_line();
            if (isatty(STDOUT_FILENO)) printf("\n\033[1;35m%s • Thinking ↓%s\033[0m\n", model_name,
                                               state->seen ? " (продолжение)" : "");
            else printf("\n%s • Thinking ↓%s\n", model_name, state->seen ? " (продолжение)" : "");
            state->seen = true;
            state->line_open = true;
            cli_current_thinking_output = state;
            cli_stream_column = 0;
        }
        if (delta[0]) {
            cli_write_stream_delta(delta);
        } else if (isatty(STDOUT_FILENO)) {
            printf("◌ ожидание текстового фрагмента…");
        }
        fflush(stdout);
        return false;
    }
    if (strcmp(type, "tool_call_stream_delta") == 0) {
        const char *key = str_get(payload, "call_key", "");
        const char *stream_id = str_get(payload, "stream_id", "");
        const char *name = str_get(payload, "tool_name", "инструмент");
        const char *model = str_get(payload, "model_name", "Модель");
        const char *argument_delta = str_get(payload, "argument_delta", "");
        CliToolOutputState *state = cli_tool_state(key, stream_id, model, name, true);
        if (!state || state->completed) return false;
        if (state->call_key[0] && state->stream_id[0] && !state->completed &&
            state->tool_name[0] && strcmp(state->call_key, key) == 0 &&
            strcmp(state->stream_id, stream_id) == 0 && strcmp(state->model_name, model) == 0 &&
            !state->header_printed) {
            /* header_printed is stored on first sight of this call, not per delta. */
            state->header_printed = true;
            printf("\n%s • Tool call ↓ · %s\n", model, name[0] ? name : "инструмент");
        }
        if (argument_delta[0]) {
            /* Label each fragment so interleaved calls from parallel models remain distinct. */
            printf("  [%s · %s] ", model, name[0] ? name : "инструмент");
            cli_stream_column = 0;
            cli_write_stream_delta(argument_delta);
            putchar('\n');
            cli_stream_column = 0;
            fflush(stdout);
        }
        return false;
    }
    if (strcmp(type, "assistant_delta") == 0) {
        const char *delta = str_get(payload, "delta", ""); /* compatibility with older servers */
        const char *stream_id = str_get(payload, "stream_id", "");
        json_object *delta_bytes_value = obj_get(payload, "delta_bytes");
        size_t delta_bytes = delta_bytes_value && json_object_get_type(delta_bytes_value) == json_type_int &&
                             json_object_get_int64(delta_bytes_value) > 0
                             ? (size_t)json_object_get_int64(delta_bytes_value) : strlen(delta);
        if (delta_bytes == 0) return false;
        if (!cli_stream_open || strcmp(cli_stream_id, stream_id) != 0) {
            const char *model_name = str_get(payload, "model_name", str_get(payload, "model", NULL));
            if (!model_name || !*model_name) {
                /* Older event producers may lack model metadata. Avoid showing
                 * the generic "Generated response text" event message as a model name. */
                model_name = (message[0] && strcmp(message, "Generated response text") != 0) ? message : "Модель";
            }
            cli_stream_close();
            (void)snprintf(cli_stream_id, sizeof(cli_stream_id), "%s", stream_id);
            printf("\n\033[1;36m%s • Streaming ↓%s%s\033[0m\n", model_name, *timestamp ? " · " : "", *timestamp ? timestamp : "");
            cli_stream_column = 0;
            cli_stream_open = true;
        }
        cli_write_stream_delta(delta);
        fflush(stdout);
        return true;
    }
    if (strcmp(type, "task_created") == 0) {
        print_chat_bubble("Вы", timestamp, message);
        return false;
    }
    if (strcmp(type, "user_answer") == 0) {
        const char *answer = str_get(payload, "answer", message);
        print_chat_bubble("Вы", timestamp, answer);
        return false;
    }
    if (strcmp(type, "agent_message") == 0) {
        const char *content = str_get(payload, "content", message);
        json_object *parsed = NULL;
        const char *trim = content;
        while (*trim == ' ' || *trim == '\t' || *trim == '\n' || *trim == '\r') ++trim;
        if (*trim == '{' || *trim == '[') parsed = json_tokener_parse(trim);
        bool is_question = false;
        if (parsed && json_object_get_type(parsed) == json_type_object) {
            const char *kind = str_get(parsed, "type", "");
            json_object *need = obj_get(parsed, "need_user_input");
            is_question = strcmp(kind, "question") == 0 ||
                (need && json_object_get_type(need) == json_type_boolean && json_object_get_boolean(need));
        }
        if (is_question) { if (parsed) json_object_put(parsed); return false; }
        if (parsed) {
            print_chat_header("ASGEND", timestamp);
            print_readable_value(parsed, NULL, 0);
            putchar('\n');
            json_object_put(parsed);
        } else {
            print_chat_bubble("ASGEND", timestamp, content);
        }
        return content && *content;
    }
    if (strcmp(type, "agent_question") == 0 || strcmp(type, "question") == 0) {
        puts("\n  ? Уточнение от агента: оркестратор ждёт ответа на уточняющий вопрос.");
        return false;
    }
    if (strcmp(type, "tool_started") == 0) {
        printf("\n  ↳ Инструмент: %s\n", message[0] ? message : "выполняется вызов");
        return false;
    }
    if (strcmp(type, "tool_completed") == 0) {
        const char *tool = str_get(payload, "tool", "инструмент");
        const char *error = str_get(payload, "error", "");
        const char *output = str_get(payload, "output", "");
        const char *call_key = str_get(payload, "call_key", "");
        const char *stream_id = str_get(payload, "stream_id", "");
        const char *model = str_get(payload, "model_name", "Модель");
        CliToolOutputState *tool_state = cli_tool_state(call_key, stream_id, model, tool, false);
        if (tool_state) cli_tool_finish_state(tool_state, error[0] != '\0');
        printf("  ✓ %s: %s\n", tool, error[0] ? error : "готово");
        if (error[0]) print_readable_text(error, NULL, 1);
        else if (output[0]) print_readable_text(output, "Результат инструмента", 1);
        return false;
    }
    if (strcmp(type, "resources_selected") == 0) {
        printf("  ◦ %s · подобраны навыки, инструкции, инструменты и знания\n", event_title(type));
        return false;
    }
    printf("  ◦ %s", event_title(type));
    if (message[0]) printf(" — %s", message);
    putchar('\n');
    return false;
}

static void print_task_result_fallback(json_object *result) {
    if (!result) return;
    json_object *outputs = obj_get(result, "agent_results");
    if (outputs && json_object_get_type(outputs) == json_type_array) {
        /* On a reconnect where events were pruned, show a human-readable result,
           preferring a concrete output over an internal verification object. */
        for (size_t i = 0; i < json_object_array_length(outputs); ++i) {
            json_object *item = json_object_array_get_idx(outputs, i);
            json_object *output = obj_get(item, "output");
            if (!output) continue;
            json_object *verification = NULL;
            bool only_review = json_object_get_type(output) == json_type_object &&
                               json_object_object_get_ex(output, "verification", &verification);
            if (only_review && i + 1 < json_object_array_length(outputs)) continue;
            puts("\nРезультат:");
            print_readable_value(output, NULL, 0);
            return;
        }
    }
    const char *error = str_get(result, "error", "");
    if (error[0]) fprintf(stderr, "Ошибка: %s\n", error);
}

static const char *status_label(const char *status) {
    if (strcmp(status, "queued") == 0) return "в очереди";
    if (strcmp(status, "running") == 0) return "выполняется";
    if (strcmp(status, "waiting_input") == 0) return "ожидает ответа";
    if (strcmp(status, "completed") == 0) return "завершена";
    if (strcmp(status, "completed_without_model") == 0) return "завершена без LLM";
    if (strcmp(status, "failed") == 0) return "ошибка";
    if (strcmp(status, "cancelled") == 0) return "отменена";
    return status;
}


typedef struct {
    json_object *payload;
    size_t question_count;
    size_t question_index;
    char *answers[ASGEND_TUI_QUESTION_OPTIONS_MAX];
    char question_texts[ASGEND_TUI_QUESTION_OPTIONS_MAX][512];
    bool visible;
} CliTuiQuestionState;

static void cli_tui_question_reset(CliTuiQuestionState *question) {
    if (!question) return;
    for (size_t i = 0; i < ASGEND_TUI_QUESTION_OPTIONS_MAX; ++i) {
        free(question->answers[i]);
        question->answers[i] = NULL;
    }
    if (question->payload) json_object_put(question->payload);
    memset(question, 0, sizeof(*question));
    asgend_tui_clear_question(&cli_tui);
}

static json_object *cli_tui_question_at(CliTuiQuestionState *state, size_t index) {
    if (!state || !state->payload) return NULL;
    json_object *batch = obj_get(state->payload, "questions");
    if (batch && json_object_get_type(batch) == json_type_array)
        return index < json_object_array_length(batch) ? json_object_array_get_idx(batch, index) : NULL;
    json_object *nested = obj_get(state->payload, "question");
    if (nested && json_object_get_type(nested) == json_type_object) return nested;
    return state->payload;
}

static bool cli_tui_question_is_valid(json_object *payload) {
    if (!payload || json_object_get_type(payload) != json_type_object) return false;
    json_object *batch = obj_get(payload, "questions");
    if (batch && json_object_get_type(batch) == json_type_array && json_object_array_length(batch) > 0) return true;
    json_object *q = obj_get(payload, "question");
    return (q && (json_object_get_type(q) == json_type_string || json_object_get_type(q) == json_type_object)) ||
           obj_get(payload, "options") || obj_get(payload, "choices");
}

static void cli_tui_question_show(CliTuiQuestionState *state) {
    if (!state || !state->payload) return;
    json_object *current = cli_tui_question_at(state, state->question_index);
    if (!current) return;
    const char *prompt = str_get(current, "question", str_get(current, "text", "Уточните недостающие сведения."));
    json_object *options = obj_get(current, "options");
    if (!options || json_object_get_type(options) != json_type_array) options = obj_get(current, "choices");
    char labels[ASGEND_TUI_QUESTION_OPTIONS_MAX][ASGEND_TUI_TITLE_MAX];
    const char *label_ptrs[ASGEND_TUI_QUESTION_OPTIONS_MAX];
    size_t count = options && json_object_get_type(options) == json_type_array ? json_object_array_length(options) : 0;
    if (count > ASGEND_TUI_QUESTION_OPTIONS_MAX) count = ASGEND_TUI_QUESTION_OPTIONS_MAX;
    for (size_t i = 0; i < count; ++i) {
        json_object *option = json_object_array_get_idx(options, i);
        const char *label = json_object_get_type(option) == json_type_string ? json_object_get_string(option) :
                            str_get(option, "label", str_get(option, "title", str_get(option, "value", str_get(option, "id", "Вариант"))));
        (void)snprintf(labels[i], sizeof(labels[i]), "%s", label ? label : "Вариант");
        label_ptrs[i] = labels[i];
    }
    (void)snprintf(state->question_texts[state->question_index], sizeof(state->question_texts[state->question_index]), "%s", prompt);
    char modal_title[160];
    (void)snprintf(modal_title, sizeof(modal_title), "%.120s • Ask User →", cli_tui_last_model);
    asgend_tui_batch_begin(&cli_tui);
    asgend_tui_set_question(&cli_tui, prompt, label_ptrs, count);
    asgend_tui_set_question_title(&cli_tui, modal_title);
    asgend_tui_batch_end(&cli_tui);
    state->visible = true;
}

static void cli_tui_question_set(CliTuiQuestionState *state, json_object *payload) {
    if (!state || !cli_tui_question_is_valid(payload)) return;
    cli_tui_question_reset(state);
    state->payload = json_object_get(payload);
    json_object *batch = obj_get(payload, "questions");
    state->question_count = batch && json_object_get_type(batch) == json_type_array
                            ? json_object_array_length(batch) : 1;
    if (state->question_count > ASGEND_TUI_QUESTION_OPTIONS_MAX)
        state->question_count = ASGEND_TUI_QUESTION_OPTIONS_MAX;
    if (state->question_count == 0) state->question_count = 1;
    cli_tui_question_show(state);
}

static char *cli_tui_question_option_value(CliTuiQuestionState *state) {
    json_object *current = cli_tui_question_at(state, state->question_index);
    json_object *options = obj_get(current, "options");
    if (!options || json_object_get_type(options) != json_type_array) options = obj_get(current, "choices");
    if (!options || json_object_get_type(options) != json_type_array || cli_tui.question_selected >= json_object_array_length(options))
        return strdup(asgend_tui_question_answer(&cli_tui));
    json_object *option = json_object_array_get_idx(options, cli_tui.question_selected);
    const char *value = json_object_get_type(option) == json_type_string ? json_object_get_string(option) :
                        str_get(option, "value", str_get(option, "id", str_get(option, "label", str_get(option, "title", ""))));
    return strdup(value ? value : "");
}

static bool cli_tui_question_accept(const Client *client, const char *task_id, CliTuiQuestionState *state) {
    if (!state || !state->payload) return false;
    char *answer = NULL;
    if (cli_tui.input_mode) answer = asgend_tui_input_take(&cli_tui);
    else answer = cli_tui_question_option_value(state);
    if (!answer) answer = strdup("");
    if (!answer || !answer[0]) {
        free(answer);
        cli_tui_question_show(state);
        return false;
    }
    free(state->answers[state->question_index]);
    state->answers[state->question_index] = answer;
    if (state->question_index + 1 < state->question_count) {
        ++state->question_index;
        cli_tui_question_show(state);
        return true;
    }
    size_t cap = 1024;
    for (size_t i = 0; i < state->question_count; ++i)
        cap += strlen(state->answers[i] ? state->answers[i] : "") + strlen(state->question_texts[i]) + 32;
    if (cap > 100000) cap = 100000;
    char *combined = calloc(1, cap);
    if (!combined) { cli_tui_question_show(state); return false; }
    size_t used = 0;
    if (state->question_count == 1) {
        (void)snprintf(combined, cap, "%s", state->answers[0] ? state->answers[0] : "");
    } else {
        used = (size_t)snprintf(combined, cap, "Ответы на уточняющие вопросы:");
        for (size_t i = 0; i < state->question_count && used + 1 < cap; ++i) {
            int n = snprintf(combined + used, cap - used, "\n%zu. %s — %s", i + 1,
                             state->question_texts[i], state->answers[i] ? state->answers[i] : "");
            if (n < 0 || (size_t)n >= cap - used) break;
            used += (size_t)n;
        }
    }
    json_object *request = request_task_op("answer", task_id);
    if (request) json_object_object_add(request, "answer", json_object_new_string(combined));
    json_object *reply = request ? rpc(client, request) : NULL;
    if (request) json_object_put(request);
    free(combined);
    if (!reply) {
        cli_tui_question_show(state);
        return false;
    }
    json_object_put(reply);
    (void)asgend_tui_upsert_tile(&cli_tui, "user:question-answer", ASGEND_TUI_TILE_CHAT, "Вы · ответ", answer, false, false, true);
    cli_tui_question_reset(state);
    return true;
}

static void cli_tui_begin_input(const char *action, const char *prompt) {
    (void)snprintf(cli_tui_input_action, sizeof(cli_tui_input_action), "%s", action ? action : "task");
    cli_tui.input_line[0] = '\0';
    cli_tui.input_length = 0;
    (void)snprintf(cli_tui.input_prompt, sizeof(cli_tui.input_prompt), "%s", prompt ? prompt : "Введите текст · Enter принять");
    asgend_tui_input_begin(&cli_tui);
}

/* Completes a value entered in the bottom prompt. Task creation returns a task ID
 * to let the caller hand off to the non-blocking task watcher. */
static bool cli_tui_finish_input(const Client *client, const char *action, const char *value,
                                 char *next_task_id, size_t next_task_id_size) {
    if (!value || !*value) return false;
    if (strcmp(action, "task") == 0 && strncmp(value, "/session", 8) == 0 &&
        (value[8] == '\0' || cli_tui_is_space(value[8]))) {
        const char *query = value + 8;
        while (*query && cli_tui_is_space(*query)) ++query;
        asgend_tui_open_session_picker(&cli_tui, query);
        return true;
    }
    if (strcmp(action, "open") == 0) {
        (void)snprintf(next_task_id, next_task_id_size, "%s", value);
        return true;
    }
    if (strcmp(action, "task") == 0) {
        json_object *request = request_op("submit");
        if (!request) return false;
        json_object_object_add(request, "task", json_object_new_string(value));
        json_object_object_add(request, "priority", json_object_new_int(5));
        json_object *reply = rpc(client, request);
        json_object_put(request);
        if (!reply) return false;
        const char *id = str_get(reply, "task_id", "");
        if (!id[0]) { json_object_put(reply); return false; }
        (void)snprintf(next_task_id, next_task_id_size, "%s", id);
        char key[ASGEND_TUI_KEY_MAX];
        (void)snprintf(key, sizeof(key), "user:prompt:%lld", (long long)time(NULL));
        (void)asgend_tui_upsert_tile(&cli_tui, key, ASGEND_TUI_TILE_CHAT, "Вы", value, false, false, true);
        json_object_put(reply);
        return true;
    }
    if (strcmp(action, "todo") == 0 || strcmp(action, "goal") == 0) {
        json_object *request = request_op("plan_add");
        if (!request) return false;
        json_object_object_add(request, "scope_id", json_object_new_string(cli_tui_scope));
        json_object_object_add(request, "kind", json_object_new_string(action));
        json_object_object_add(request, "title", json_object_new_string(value));
        json_object *reply = rpc(client, request);
        json_object_put(request);
        if (reply) json_object_put(reply);
        cli_tui_refresh_data(client, cli_tui_scope);
        return reply != NULL;
    }
    return false;
}

static int cli_tui_handle_session_key(int key, char *selected_id, size_t selected_id_size) {
    if (!asgend_tui_session_picker_active(&cli_tui)) return 0;
    if (key == ASGEND_TUI_KEY_ESCAPE) {
        asgend_tui_close_session_picker(&cli_tui);
        return 1;
    }
    if (key == 127 || key == 8) {
        asgend_tui_session_query_backspace(&cli_tui);
        return 1;
    }
    if (key == '\n') {
        AsgendTuiTaskItem selected;
        if (asgend_tui_selected_session(&cli_tui, &selected)) {
            if (selected_id && selected_id_size)
                (void)snprintf(selected_id, selected_id_size, "%s", selected.id);
            asgend_tui_close_session_picker(&cli_tui);
            return 2;
        }
        return 1;
    }
    if (key == ASGEND_TUI_KEY_NAVIGATION || key == ASGEND_TUI_KEY_MOUSE ||
        key == ASGEND_TUI_KEY_RESIZE || key == ASGEND_TUI_KEY_PAGE_UP || key == ASGEND_TUI_KEY_PAGE_DOWN ||
        key == '\t' || key == 0) return 1;
    if (key >= 0x20 && key <= 0xff) {
        char byte[2] = {(char)key, '\0'};
        (void)asgend_tui_session_query_append(&cli_tui, byte);
    }
    return 1;
}

static void cli_tui_handle_composer_key(const Client *client, int key, bool *quit,
                                        char *next_task_id, size_t next_task_id_size) {
    if (!cli_tui.input_mode) return;
    if (key == ASGEND_TUI_KEY_ESCAPE) {
        asgend_tui_input_cancel(&cli_tui);
        return;
    }
    if (key == 127 || key == 8) {
        asgend_tui_input_backspace(&cli_tui);
        return;
    }
    if (key != '\n') {
        if (key >= 0x20 && key <= 0x7e) {
            char byte[2] = {(char)key, '\0'};
            (void)asgend_tui_input_append(&cli_tui, byte);
        } else if (key >= 0x80 && key <= 0xff) {
            char byte[2] = {(char)key, '\0'};
            (void)asgend_tui_input_append(&cli_tui, byte);
        }
        return;
    }
    char action[sizeof(cli_tui_input_action)];
    (void)snprintf(action, sizeof(action), "%s", cli_tui_input_action);
    char *value = asgend_tui_input_take(&cli_tui);
    if (!value) return;
    bool okay = cli_tui_finish_input(client, action, value, next_task_id, next_task_id_size);
    if (!okay && value[0]) (void)asgend_tui_upsert_tile(&cli_tui, "notice:input", ASGEND_TUI_TILE_ACTION,
        "Не удалось выполнить ввод", "Проверьте соединение с ядром и повторите запрос.", false, false, true);
    if (okay && next_task_id && next_task_id[0]) *quit = true;
    free(value);
}

static int cli_tui_follow_task_async(const Client *client, const char *task_id) {
    CliTuiTaskWatch watch;
    memset(&watch, 0, sizeof(watch));
    watch.client = *client;
    (void)snprintf(watch.task_id, sizeof(watch.task_id), "%s", task_id);
    atomic_init(&watch.stop, false);
    if (pthread_mutex_init(&watch.mutex, NULL) != 0) return 1;
    if (pthread_cond_init(&watch.event_space, NULL) != 0) {
        pthread_mutex_destroy(&watch.mutex);
        return 1;
    }
    (void)snprintf(cli_tui_scope, sizeof(cli_tui_scope), "%s", task_id);
    asgend_tui_set_welcome(&cli_tui, false);
    cli_tui.focused_pane = 1;
    asgend_tui_clear_tiles(&cli_tui);
    asgend_tui_set_header(&cli_tui, "ASGEND · Чат задачи");
    asgend_tui_set_task(&cli_tui, task_id, "Подключение");
    cli_tui_refresh_data(client, task_id);
    cli_tui_show_help();
    if (pthread_create(&watch.thread, NULL, cli_tui_task_watch_main, &watch) != 0) {
        pthread_cond_destroy(&watch.event_space);
        pthread_mutex_destroy(&watch.mutex);
        (void)asgend_tui_upsert_tile(&cli_tui, "watch:error", ASGEND_TUI_TILE_PROGRESS,
                                    "Ошибка наблюдения", "Не удалось запустить поток событий задачи.", false, false, true);
        return 1;
    }
    CliTuiQuestionState question = {0};
    uint64_t seen_status_generation = 0;
    bool leave = false;
    bool refresh_files = false;
    bool watch_finished_drained = false;
    char next_task_id[ASGEND_TUI_KEY_MAX] = "";
    int final_result = 0;
    while (!leave) {
        asgend_tui_batch_begin(&cli_tui);
        json_object *event = NULL;
        size_t processed_events = 0;
        refresh_files = false;
        while (processed_events < CLI_TUI_EVENT_BATCH_MAX &&
               (event = cli_tui_watch_pop_event(&watch)) != NULL) {
            ++processed_events;
            (void)print_event(event);
            const char *type = str_get(event, "type", "");
            json_object *payload = obj_get(event, "payload");
            if (strstr(type, "file") || strstr(type, "workspace") || strstr(type, "artifact") ||
                strcmp(type, "tool_completed") == 0) refresh_files = true;
            if (!question.payload && (strcmp(type, "question") == 0 || strcmp(type, "agent_question") == 0 ||
                strcmp(type, "orchestrator_waiting_input") == 0) && cli_tui_question_is_valid(payload))
                cli_tui_question_set(&question, payload);
            json_object_put(event);
        }
        uint64_t current_status_generation = 0;
        json_object *status = cli_tui_watch_copy_status(&watch, &current_status_generation);
        if (status && current_status_generation != seen_status_generation) {
            seen_status_generation = current_status_generation;
            const char *state = str_get(status, "status", "unknown");
            asgend_tui_set_task(&cli_tui, task_id, status_label(state));
            if (!question.payload && strcmp(state, "waiting_input") == 0) {
                json_object *result = obj_get(status, "result");
                if (cli_tui_question_is_valid(result)) cli_tui_question_set(&question, result);
                else {
                    json_object *nested = obj_get(result, "question");
                    if (cli_tui_question_is_valid(nested)) cli_tui_question_set(&question, nested);
                }
            }
        }
        if (status) json_object_put(status);
        if (refresh_files) {
            cli_tui_refresh_files(client, task_id);
            cli_tui_refresh_open_preview(client);
        }
        asgend_tui_batch_end(&cli_tui);

        int key = asgend_tui_poll_key(&cli_tui);
        if (asgend_tui_session_picker_active(&cli_tui)) {
            char selected_session[ASGEND_TUI_KEY_MAX] = "";
            int handled = cli_tui_handle_session_key(key, selected_session, sizeof(selected_session));
            if (handled == 2 && selected_session[0] && strcmp(selected_session, task_id) != 0) {
                (void)snprintf(cli_tui_requested_session, sizeof(cli_tui_requested_session), "%s", selected_session);
                leave = true;
            }
        } else if (cli_tui.input_mode && !cli_tui.question_active) {
            bool input_quit = false;
            cli_tui_handle_composer_key(client, key, &input_quit, next_task_id, sizeof(next_task_id));
            if (input_quit) leave = true;
        } else if (question.payload && question.visible) {
            if (key == ASGEND_TUI_KEY_ESCAPE) {
                asgend_tui_clear_question(&cli_tui);
                question.visible = false;
            } else if ((key == 127 || key == 8) && cli_tui.input_mode) {
                asgend_tui_input_backspace(&cli_tui);
            } else if (key == '\n') {
                (void)cli_tui_question_accept(client, task_id, &question);
            } else if (key >= 0x20 && key <= 0xff && key != '\t') {
                char byte[2] = {(char)key, '\0'};
                        (void)snprintf(cli_tui.input_prompt, sizeof(cli_tui.input_prompt), "%s", "Ответ · Enter отправить · Esc отменить");
                asgend_tui_input_begin(&cli_tui);
                (void)asgend_tui_input_append(&cli_tui, byte);
            }
        } else if (key == ASGEND_TUI_KEY_ESCAPE) {
            if (cli_tui.preview_content) cli_tui_close_preview();
        } else if (key == ASGEND_TUI_KEY_NAVIGATION || key == ASGEND_TUI_KEY_MOUSE ||
                   key == ASGEND_TUI_KEY_RESIZE || key == ASGEND_TUI_KEY_PAGE_UP || key == ASGEND_TUI_KEY_PAGE_DOWN) {
            if (key == ASGEND_TUI_KEY_NAVIGATION || key == ASGEND_TUI_KEY_MOUSE ||
                key == ASGEND_TUI_KEY_PAGE_UP || key == ASGEND_TUI_KEY_PAGE_DOWN) cli_tui_maybe_stream_preview(client);
        } else if (key == '\n') {
            if (cli_tui.focused_pane == 0) cli_tui_open_selected_file(client);
            else if (cli_tui.focused_pane == 1) cli_tui_open_selected_chat_file(client);
            else if (cli_tui.focused_pane == 2 && cli_tui.preview_content) cli_tui_edit_previewed_file(client);
            else cli_tui_toggle_plan(client);
        } else if (key == 'q' || key == 'Q') {
            leave = true;
        } else if (key == '/' || key == 'n' || key == 'N') {
            cli_tui_begin_input("task", "Новая задача · Enter отправить · Esc отменить");
        } else if (key == 'o' || key == 'O') {
            cli_tui_begin_input("open", "ID задачи · Enter открыть · Esc отменить");
        } else if (key == 's' || key == 'S') {
            asgend_tui_open_session_picker(&cli_tui, "");
        } else if (key == 't' || key == 'T') {
            cli_tui_begin_input("todo", "Новый пункт To-do · Enter добавить");
        } else if (key == 'g' || key == 'G') {
            cli_tui_begin_input("goal", "Новая Goal · Enter добавить");
        } else if (key == 'd' || key == 'D') {
            cli_tui_toggle_plan(client);
        } else if (key == 'r' || key == 'R') {
            cli_tui_refresh_data(client, task_id);
        } else if (key == 'c' || key == 'C') {
            cancel_task(client, task_id);
        } else if (key == 'e' || key == 'E') {
            if (cli_tui.preview_content) cli_tui_edit_previewed_file(client);
        } else if (key == 'b' || key == 127 || key == 8) {
            if (cli_tui.focused_pane == 0) cli_tui_go_parent_directory(client);
            else if (cli_tui.preview_content) cli_tui_close_preview();
        } else if (key == '?' && question.payload && !question.visible) {
            cli_tui_question_show(&question);
        } else if (key == '?') {
            cli_tui_show_help();
        } else if (key == '1') asgend_tui_set_display_mode(&cli_tui, ASGEND_TUI_DISPLAY_ALL_EXPANDED);
        else if (key == '2') asgend_tui_set_display_mode(&cli_tui, ASGEND_TUI_DISPLAY_ALL_COLLAPSED);
        else if (key == '3') asgend_tui_set_display_mode(&cli_tui, ASGEND_TUI_DISPLAY_DYNAMIC);

        cli_tui_apply_plan_notification(client);
        if (!watch_finished_drained && cli_tui_watch_finished(&watch, &final_result)) {
            /* Drain queued events once more so the final answer is not lost.
             * A terminal task is still a browsable session: stop consuming
             * network events, but keep the TUI alive until the user presses q
             * or switches to another task. */
            asgend_tui_batch_begin(&cli_tui);
            while ((event = cli_tui_watch_pop_event(&watch)) != NULL) {
                (void)print_event(event);
                json_object_put(event);
            }
            asgend_tui_batch_end(&cli_tui);
            watch_finished_drained = true;
            json_object *finished_status = cli_tui_watch_copy_status(&watch, NULL);
            const char *state = str_get(finished_status, "status", "completed");
            char body[256];
            (void)snprintf(body, sizeof(body), "%s", status_label(state));
            (void)asgend_tui_upsert_tile(&cli_tui, "task-terminal-status", ASGEND_TUI_TILE_PROGRESS,
                strcmp(state, "failed") == 0 ? "Задача завершилась с ошибкой" :
                strcmp(state, "cancelled") == 0 ? "Задача отменена" : "Задача завершена",
                body, false, false, true);
            if (finished_status) json_object_put(finished_status);
        }
        if (!key) {
            asgend_tui_tick(&cli_tui);
            struct timespec pause = {.tv_sec = 0, .tv_nsec = 35000000L};
            (void)nanosleep(&pause, NULL);
        }
    }
    cli_tui_question_reset(&question);
    cli_tui_watch_stop(&watch);
    if (next_task_id[0]) {
        (void)snprintf(cli_tui_scope, sizeof(cli_tui_scope), "%s", next_task_id);
        return follow_task(client, next_task_id);
    }
    if (!leave && final_result != 0) {
        (void)asgend_tui_upsert_tile(&cli_tui, "task-terminal-status", ASGEND_TUI_TILE_PROGRESS,
            "Задача завершилась с ошибкой", "Проверьте события выше; задача больше не выполняется.", false, false, true);
    }
    asgend_tui_set_header(&cli_tui, "ASGEND · Рабочее пространство");
    if (leave) {
        (void)snprintf(cli_tui_scope, sizeof(cli_tui_scope), "global");
        cli_tui_refresh_data(client, "global");
    }
    return final_result;
}

static int follow_task(const Client *client, const char *task_id) {
    if (cli_tui_live) return cli_tui_follow_task_async(client, task_id);
    int last_event_id = 0, handled_question_event = -1;
    bool saw_assistant_message = false;
    char last_reported_state[64] = "";
    unsigned spinner_frame = 0;
    unsigned status_rpc_failures = 0;
    static const char *const spinner[] = {"|", "/", "-", "\\"};
    char handled_question_text[2048] = "";
    if (!cli_tui_live) {
        puts("\nЧАТ ЗАДАЧИ");
        puts("Сообщения и действия оркестратора появляются по мере работы");
        puts("/cancel — отменить задачу");
    }
    for (;;) {
        json_object *events_request = request_task_op("events", task_id);
        json_object_object_add(events_request, "after_id", json_object_new_int(last_event_id));
        json_object_object_add(events_request, "limit", json_object_new_int(100));
        json_object *events_reply = rpc(client, events_request); json_object_put(events_request);
        json_object *pending_question = NULL;
        int pending_question_event = -1;
        if (events_reply) {
            json_object *events = obj_get(events_reply, "events");
            if (events && json_object_get_type(events) == json_type_array) {
                bool refresh_files = false;
                for (size_t i = 0; i < json_object_array_length(events); ++i) {
                    json_object *event = json_object_array_get_idx(events, i);
                    int event_id = json_object_get_int(obj_get(event, "event_id"));
                    if (event_id > last_event_id) last_event_id = event_id;
                    if (print_event(event)) saw_assistant_message = true;
                    const char *type = str_get(event, "type", "");
                    if (strstr(type, "file") || strstr(type, "workspace") || strstr(type, "artifact") || strcmp(type, "tool_completed") == 0)
                        refresh_files = true;
                    if ((strcmp(type, "question") == 0 || strcmp(type, "agent_question") == 0 || strcmp(type, "orchestrator_waiting_input") == 0) && obj_get(event, "payload")) {
                        json_object *payload = obj_get(event, "payload");
                        json_object *questions = obj_get(payload, "questions");
                        json_object *question = obj_get(payload, "question");
                        if (questions && json_object_get_type(questions) == json_type_array && json_object_array_length(questions) > 0)
                            question = payload; /* Keep the full batch of clarification questions. */
                        else if (!question || json_object_get_type(question) != json_type_object) question = payload;
                        if (obj_get(question, "question") || obj_get(question, "questions")) { if (pending_question) json_object_put(pending_question); pending_question = json_object_get(question); pending_question_event = event_id; }
                    }
                }
                if (refresh_files && cli_tui_live) cli_tui_refresh_files(client, task_id);
            }
            json_object_put(events_reply);
        }
        json_object *status_request = request_task_op("status", task_id);
        json_object *status = rpc(client, status_request); json_object_put(status_request);
        if (!status) {
            status_rpc_failures++;
            if (pending_question) json_object_put(pending_question);
            if (status_rpc_failures >= 5) {
                fputs("\nASGEND CLI: не удалось получить статус задачи после 5 попыток; чат остановлен, сама задача не отменена.\n", stderr);
                return 1;
            }
            struct timespec retry_pause = {.tv_sec = 0, .tv_nsec = 400000000L};
            (void)nanosleep(&retry_pause, NULL);
            continue;
        }
        status_rpc_failures = 0;
        /* str_get() returns storage owned by the JSON status object.  Keep a
         * local copy because terminal handling releases that object below. */
        char state[64];
        (void)snprintf(state, sizeof(state), "%s", str_get(status, "status", "unknown"));
        if (cli_tui_live) {
            asgend_tui_set_task(&cli_tui, task_id, status_label(state));
        } else if (isatty(STDOUT_FILENO)) {
            printf("\r\033[2K%s Статус: %-22s · событий: %d",
                   spinner[spinner_frame++ % (sizeof(spinner) / sizeof(spinner[0]))],
                   status_label(state), last_event_id);
            fflush(stdout);
            cli_progress_visible = true;
        } else if (strcmp(last_reported_state, state) != 0) {
            printf("Статус: %s\n", status_label(state));
            (void)snprintf(last_reported_state, sizeof(last_reported_state), "%s", state);
        }
        if (strcmp(state, "waiting_input") == 0) {
            json_object *result = obj_get(status, "result");
            json_object *question = obj_get(result, "question");
            json_object *questions = obj_get(result, "questions");
            if (questions && json_object_get_type(questions) == json_type_array && json_object_array_length(questions) > 0) {
                if (!pending_question) { pending_question = json_object_get(result); pending_question_event = last_event_id; }
            } else if (question && json_object_get_type(question) == json_type_object && !pending_question) {
                pending_question = json_object_get(question); pending_question_event = last_event_id;
            }
            if (pending_question) {
                const char *question_text = str_get(pending_question, "question", "");
                bool repeated = pending_question_event >= 0 ? pending_question_event == handled_question_event :
                                (question_text[0] && strcmp(question_text, handled_question_text) == 0);
                if (!repeated) {
                    bool cancelled = false;
                    if (cli_tui_live) asgend_tui_suspend(&cli_tui);
                    bool answered = show_question_and_answer(client, task_id, pending_question, &cancelled);
                    if (cli_tui_live && cli_tui.suspended) asgend_tui_resume(&cli_tui);
                    if (answered) {
                        handled_question_event = pending_question_event;
                        (void)snprintf(handled_question_text, sizeof(handled_question_text), "%s", question_text);
                    }
                    if (cancelled) { json_object_put(status); if (pending_question) json_object_put(pending_question); continue; }
                    if (!answered && feof(stdin)) {
                        if (cli_tui_live) (void)asgend_tui_upsert_tile(&cli_tui, "waiting-input", ASGEND_TUI_TILE_PROGRESS, "Ожидается ответ", "Задача остаётся в waiting_input; откройте её позже.", false, false, true);
                        else puts("\nВвод завершён; задача остаётся в waiting_input. Откройте чат позже, чтобы продолжить.");
                        json_object_put(status); if (pending_question) json_object_put(pending_question); return 0;
                    }
                } else {
                    struct timespec pause = {.tv_sec = 0, .tv_nsec = 500000000L}; (void)nanosleep(&pause, NULL);
                }
            } else {
                struct timespec pause = {.tv_sec = 0, .tv_nsec = 500000000L}; (void)nanosleep(&pause, NULL);
            }
        } else if (strcmp(state, "completed") == 0 || strcmp(state, "completed_without_model") == 0 ||
                   strcmp(state, "failed") == 0 || strcmp(state, "cancelled") == 0) {
            if (!cli_tui_live) {
                cli_progress_clear();
                if (isatty(STDOUT_FILENO)) putchar('\n');
                cli_stream_close();
                puts("\n");
            }
            json_object *result = obj_get(status, "result");
            if (!cli_tui_live && !saw_assistant_message && result) print_task_result_fallback(result);
            if (cli_tui_live) {
                char body[512];
                const char *err = str_get(status, "error", "");
                (void)snprintf(body, sizeof(body), "%s%s%s", status_label(state), err[0] ? " — " : "", err);
                (void)asgend_tui_upsert_tile(&cli_tui, "task-terminal-status", ASGEND_TUI_TILE_PROGRESS,
                    strcmp(state, "failed") == 0 ? "Задача завершилась с ошибкой" : strcmp(state, "cancelled") == 0 ? "Задача отменена" : "Задача завершена",
                    body, false, false, true);
                asgend_tui_set_task(&cli_tui, task_id, status_label(state));
            } else {
                if (strcmp(state, "completed") == 0 || strcmp(state, "completed_without_model") == 0) {
                    puts("Результат: сообщения и проверочные действия показаны в чате выше.");
                    puts("✓ Задача завершена.");
                }
                if (strcmp(state, "failed") == 0) fprintf(stderr, "Задача завершилась с ошибкой: %s\n", str_get(status, "error", "unknown error"));
            }
            if (!cli_tui_live) {
                json_object_put(status); if (pending_question) json_object_put(pending_question);
                return strcmp(state, "failed") == 0 ? 1 : 0;
            }
            /* A completed task remains browsable in fullscreen TUI so users can
             * inspect its workspace and plan. The same terminal state is still
             * returned immediately in line mode. */
        } else {
            /* This path is used by line-mode CLI. Keep the event polling cadence
             * short enough to render each persisted content delta during generation. */
            struct timespec pause = {.tv_sec = 0, .tv_nsec = CLI_STREAM_POLL_INTERVAL_NS};
            (void)nanosleep(&pause, NULL);
        }
        if (cli_tui_live) {
            cli_tui_apply_plan_notification(client);
            int key = asgend_tui_poll_key(&cli_tui);
            if (key == 'q' || key == 'Q') {
                json_object_put(status);
                if (pending_question) json_object_put(pending_question);
                asgend_tui_set_header(&cli_tui, "Рабочее пространство");
                return 0; /* Leave the task running; return to the task overview. */
            }
            if (key == 't' || key == 'T') cli_tui_add_plan(client, "todo");
            else if (key == 'g' || key == 'G') cli_tui_add_plan(client, "goal");
            else if (key == 'd' || key == 'D') cli_tui_toggle_plan(client);
            else if (key == 'r' || key == 'R') cli_tui_refresh_data(client, task_id);
            else if (key == 'c' || key == 'C') cancel_task(client, task_id);
            else if (key == '?') cli_tui_show_help();
        }
        json_object_put(status);
        if (pending_question) json_object_put(pending_question);
    }
}

static void submit_task(const Client *client) {
    char *task = read_line("Опишите задачу: ");
    if (!task) return;
    if (!*task) { puts("Пустая задача отменена."); free(task); return; }
    char *priority_line = read_line("Приоритет 0–9 [5]: ");
    int priority = 5;
    if (priority_line && *priority_line) {
        char *end = NULL; long value = strtol(priority_line, &end, 10);
        if (end && *end == '\0' && value >= 0 && value <= 9) priority = (int)value;
        else puts("Некорректный приоритет; используется 5.");
    }
    free(priority_line);
    json_object *request = request_op("submit");
    json_object_object_add(request, "task", json_object_new_string(task));
    json_object_object_add(request, "priority", json_object_new_int(priority));
    json_object *reply = rpc(client, request); json_object_put(request); free(task);
    if (!reply) return;
    const char *id = str_get(reply, "task_id", "");
    if (*id) { char task_id[128]; (void)snprintf(task_id, sizeof(task_id), "%s", id); printf("Задача создана: %s\n", task_id); json_object_put(reply); (void)follow_task(client, task_id); }
    else { puts("Сервер не вернул ID задачи."); print_readable_value(reply, NULL, 0); json_object_put(reply); }
}

static char *read_task_id_line(const Client *client, const char *prompt) {
    if (!isatty(STDIN_FILENO)) return read_line(prompt);
    struct termios original, editing;
    if (tcgetattr(STDIN_FILENO, &original) != 0) return read_line(prompt);
    editing = original;
    editing.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    editing.c_cc[VMIN] = 1;
    editing.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &editing) != 0) return read_line(prompt);

    char buffer[128] = {0};
    size_t used = 0;
    bool interrupted = false;
    printf("%s", prompt ? prompt : "");
    fflush(stdout);
    for (;;) {
        unsigned char ch = 0;
        ssize_t n = read(STDIN_FILENO, &ch, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || ch == 4 || ch == 3) { interrupted = true; break; }
        if (ch == '\r' || ch == '\n') { putchar('\n'); break; }
        if (ch == 127 || ch == '\b') {
            if (used) { --used; buffer[used] = '\0'; fputs("\b \b", stdout); fflush(stdout); }
            continue;
        }
        if (ch == '\t') {
            json_object *request = request_op("list_tasks");
            if (request) json_object_object_add(request, "limit", json_object_new_int(200));
            json_object *reply = request ? rpc(client, request) : NULL;
            if (request) json_object_put(request);
            json_object *tasks = obj_get(reply, "tasks");
            const char *candidate = NULL;
            size_t matches = 0;
            if (tasks && json_object_get_type(tasks) == json_type_array) {
                for (size_t i = 0; i < json_object_array_length(tasks); ++i) {
                    json_object *row = json_object_array_get_idx(tasks, i);
                    const char *id = str_get(row, "task_id", "");
                    if (*id && strlen(id) >= used && strncmp(id, buffer, used) == 0) {
                        candidate = id;
                        ++matches;
                    }
                }
            }
            if (matches == 1 && candidate) {
                size_t full_len = strlen(candidate);
                while (used < full_len && used + 1 < sizeof(buffer)) {
                    buffer[used] = candidate[used];
                    putchar(buffer[used]);
                    ++used;
                }
                buffer[used] = '\0';
                fflush(stdout);
            } else if (matches > 1 && tasks) {
                printf("\nСовпадения:");
                size_t shown = 0;
                for (size_t i = 0; i < json_object_array_length(tasks) && shown < 12; ++i) {
                    json_object *row = json_object_array_get_idx(tasks, i);
                    const char *id = str_get(row, "task_id", "");
                    if (*id && strlen(id) >= used && strncmp(id, buffer, used) == 0) {
                        printf(" %s", id);
                        ++shown;
                    }
                }
                printf("\n%s%s", prompt ? prompt : "", buffer);
                fflush(stdout);
            }
            if (reply) json_object_put(reply);
            continue;
        }
        if (ch >= 32 && ch < 127 && used + 1 < sizeof(buffer)) {
            buffer[used++] = (char)ch;
            buffer[used] = '\0';
            putchar((char)ch);
            fflush(stdout);
        }
    }
    (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
    if (interrupted) { putchar('\n'); return NULL; }
    return strdup(buffer);
}

static void open_task_chat(const Client *client) {
    char *task_id = read_task_id_line(client, "ID задачи (Tab — дополнить): ");
    if (!task_id) return;
    if (*task_id) (void)follow_task(client, task_id);
    free(task_id);
}

static void show_health(const Client *client) {
    json_object *request = request_op("health"); json_object *reply = rpc(client, request);
    if (request) json_object_put(request);
    if (reply) { print_json(reply); json_object_put(reply); }
}

static void show_models(const Client *client) {
    json_object *request = request_op("models"); json_object *reply = rpc(client, request);
    if (request) json_object_put(request);
    if (reply) { print_json(reply); json_object_put(reply); }
}

static void menu(void) {
    int columns = cli_terminal_columns();
    if (isatty(STDOUT_FILENO) && columns < 44) {
        puts("\nASGEND");
        if (columns < 32) {
            puts("1 Задачи");
            puts("2 Новая задача");
            puts("3 Чат по ID");
            puts("4 Состояние");
            puts("5 Модели");
            puts("6 Отменить");
        } else {
            puts("1  Список задач");
            puts("2  Создать задачу и открыть чат");
            puts("3  Открыть чат по ID задачи");
            puts("4  Состояние ядра");
            puts("5  Модели и узлы");
            puts("6  Отменить задачу");
        }
        puts("q  Выход");
        return;
    }
    puts("\nASGEND");
    puts("1  Список задач");
    puts("2  Создать задачу и открыть чат");
    puts("3  Открыть чат по ID задачи");
    puts("4  Состояние ядра");
    puts("5  Модели и узлы");
    puts("6  Отменить задачу");
    puts("q  Выход");
}

static void cli_tui_show_help(void) {
    const char *help =
        "Стрелки / j,k — перемещение; Tab — панель; Enter — открыть задачу, каталог или файл.\n"
        "n — новая задача; o — открыть задачу по ID; /session или s — выбрать активную задачу; c — отмена; r — обновить.\n"
        "t — добавить To-do; g — добавить Goal; d — отметить выбранный пункт готовым/открытым.\n"
        "В панели файлов: b/Backspace — родительский каталог; p — переключить Files/Goals.\n"
        "В предпросмотре: e — редактировать через VISUAL/EDITOR (по умолчанию vi); после выхода изменения сохраняются в workspace.\n"
        "↑/↓, PgUp/PgDn или колесо мыши — прокрутка; горизонтальное колесо — панорамирование без переноса; Esc — закрыть.\n"
        "Левый клик и drag выбирают строки; лента событий прокручивается по визуальным строкам. Просмотр файлов потоковый, внешний редактор ограничен 1 MiB.\n"
        "1 — раскрыть все плитки; 2 — свернуть все; 3 — динамика; h — состояние ядра; m — модели; q — выход.\n"
        "Thinking отображается как активный процесс; приватный текст reasoning и аргументы tool call не публикуются ядром.\n";
    (void)asgend_tui_upsert_tile(&cli_tui, "help:keys", ASGEND_TUI_TILE_ACTION,
                                "Справка · клавиши", help, false, false, false);
}

static int cli_tui_loop(const Client *client) {
    int previous_rpc_timeout = cli_rpc_timeout_seconds;
    bool previous_rpc_quiet = cli_rpc_quiet;
    /* Foreground TUI RPCs must not inherit the line-mode 60s timeout. This is
     * a bounded fallback until all slow operations use the worker-result queue. */
    cli_rpc_timeout_seconds = 3;
    cli_tui_live = true;
    cli_tui.focused_pane = 1;
    (void)snprintf(cli_tui_scope, sizeof(cli_tui_scope), "global");
    asgend_tui_set_welcome(&cli_tui, true);
    cli_tui_refresh_data(client, cli_tui_scope);
    cli_tui_start_plan_watch(client);
    cli_tui_show_help();
    bool quit = false;
    while (!quit) {
        int key = asgend_tui_poll_key(&cli_tui);
        cli_tui_apply_plan_notification(client);
        bool input_quit = false;
        char next_task_id[ASGEND_TUI_KEY_MAX] = "";
        if (asgend_tui_session_picker_active(&cli_tui)) {
            int handled = cli_tui_handle_session_key(key, next_task_id, sizeof(next_task_id));
            if (handled == 2 && next_task_id[0]) {
                input_quit = true;
                asgend_tui_set_welcome(&cli_tui, false);
            }
        } else if (asgend_tui_welcome_active(&cli_tui) && !cli_tui.input_mode && key == '\n') {
            AsgendTuiTaskItem selected;
            if (asgend_tui_selected_session(&cli_tui, &selected)) {
                (void)snprintf(next_task_id, sizeof(next_task_id), "%s", selected.id);
                input_quit = true;
                asgend_tui_set_welcome(&cli_tui, false);
            }
        } else if (cli_tui.input_mode) {
            cli_tui_handle_composer_key(client, key, &input_quit, next_task_id, sizeof(next_task_id));
        }
        if (input_quit && next_task_id[0]) {
            char active_id[ASGEND_TUI_KEY_MAX];
            (void)snprintf(active_id, sizeof(active_id), "%s", next_task_id);
            do {
                cli_tui_requested_session[0] = '\0';
                (void)snprintf(cli_tui_scope, sizeof(cli_tui_scope), "%s", active_id);
                (void)follow_task(client, active_id);
                if (!cli_tui_requested_session[0] || strcmp(cli_tui_requested_session, active_id) == 0) break;
                (void)snprintf(active_id, sizeof(active_id), "%s", cli_tui_requested_session);
            } while (active_id[0]);
            cli_tui_requested_session[0] = '\0';
            (void)snprintf(cli_tui_scope, sizeof(cli_tui_scope), "global");
            asgend_tui_set_header(&cli_tui, "ASGEND · Рабочее пространство");
            asgend_tui_set_welcome(&cli_tui, true);
            cli_tui_refresh_data(client, "global");
            continue;
        }
        if (cli_tui.input_mode || asgend_tui_session_picker_active(&cli_tui)) {
            if (!key) {
                asgend_tui_tick(&cli_tui);
                struct timespec pause = {.tv_sec = 0, .tv_nsec = 30000000L};
                (void)nanosleep(&pause, NULL);
            }
            continue;
        }
        if (!key) {
            asgend_tui_tick(&cli_tui);
            struct timespec pause = {.tv_sec = 0, .tv_nsec = 30000000L};
            (void)nanosleep(&pause, NULL);
            continue;
        }
        if (key == ASGEND_TUI_KEY_NAVIGATION || key == ASGEND_TUI_KEY_MOUSE ||
            key == ASGEND_TUI_KEY_PAGE_UP || key == ASGEND_TUI_KEY_PAGE_DOWN) {
            cli_tui_maybe_stream_preview(client);
        } else if (key == ASGEND_TUI_KEY_ESCAPE) {
            if (cli_tui.preview_content) cli_tui_close_preview();
        } else if (key == ASGEND_TUI_KEY_RESIZE) {
            /* Layout changes are applied by the library's resize handler. */
        } else if (key == '\n') {
            if (cli_tui.focused_pane == 0) cli_tui_open_selected_file(client);
            else if (cli_tui.focused_pane == 1) cli_tui_open_selected_chat_file(client);
            else if (cli_tui.focused_pane == 2 && cli_tui.preview_content) cli_tui_edit_previewed_file(client);
            else if (cli_tui.focused_pane == 2) cli_tui_toggle_plan(client);
        } else if (key == 'q' || key == 'Q') {
            quit = true;
        } else if (key == '/' || key == 'n' || key == 'N') {
            cli_tui_begin_input("task", "Новая задача · Enter отправить · Esc отменить");
        } else if (key == 'o' || key == 'O') {
            cli_tui_begin_input("open", "ID задачи · Enter открыть · Esc отменить");
        } else if (key == 's' || key == 'S') {
            asgend_tui_open_session_picker(&cli_tui, "");
        } else if (key == 't' || key == 'T') {
            cli_tui_begin_input("todo", "Новый пункт To-do · Enter добавить");
        } else if (key == 'g' || key == 'G') {
            cli_tui_begin_input("goal", "Новая Goal · Enter добавить");
        } else if (key == 'b' || key == 127 || key == 8) {
            if (cli_tui.focused_pane == 0) cli_tui_go_parent_directory(client);
            else if (cli_tui.preview_content) cli_tui_close_preview();
        } else if (key == 'r' || key == 'R') {
            cli_tui_refresh_data(client, cli_tui_scope);
        } else if (key == 'd' || key == 'D') {
            cli_tui_toggle_plan(client);
        } else if (key == 'c' || key == 'C') {
            if (strcmp(cli_tui_scope, "global") != 0) cancel_task(client, cli_tui_scope);
            else (void)asgend_tui_upsert_tile(&cli_tui, "notice:cancel", ASGEND_TUI_TILE_ACTION,
                                              "Отмена задачи", "Откройте задачу клавишей o и укажите её ID, затем нажмите c.", false, false, true);
        } else if ((key == 'e' || key == 'E') && cli_tui.preview_content) {
            cli_tui_edit_previewed_file(client);
        } else if (key == 'h' || key == 'H' || key == 'm' || key == 'M') {
            asgend_tui_suspend(&cli_tui);
            if (key == 'h' || key == 'H') show_health(client); else show_models(client);
            (void)read_line("Enter — вернуться в TUI: ");
            if (cli_tui.suspended) asgend_tui_resume(&cli_tui);
        } else if (key == '?') {
            cli_tui_show_help();
        } else if (key == '\t') {
            /* Focus change is applied by the TUI library. */
        }
    }
    cli_tui_stop_plan_watch();
    cli_tui_live = false;
    cli_rpc_quiet = previous_rpc_quiet;
    cli_rpc_timeout_seconds = previous_rpc_timeout;
    return 0;
}

static void usage(const char *program) {
    printf("%s [--host HOST] [--port PORT] [--token TOKEN] [--no-tui]\n", program);
    puts("TTY starts the full-screen TUI; --no-tui keeps the line-oriented client.");
    puts("For secrets, prefer ASGEND_TOKEN or the hidden interactive prompt; --token may be visible in process listings.");
}

int main(int argc, char **argv) {
    (void)setlocale(LC_CTYPE, "");
    cli_stream_columns = cli_terminal_columns();
    const char *host = getenv("ASGEND_HOST"); if (!host || !*host) host = DEFAULT_HOST;
    const char *token = getenv("ASGEND_TOKEN"); if (!token) token = "";
    char port_buf[16]; (void)snprintf(port_buf, sizeof(port_buf), "%d", DEFAULT_PORT);
    const char *port = getenv("ASGEND_PORT"); if (!port || !*port) port = port_buf;
    bool force_no_tui = false;
    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "--host") == 0 || strcmp(argv[i], "--port") == 0 || strcmp(argv[i], "--token") == 0) && i + 1 < argc) {
            const char *flag = argv[i++];
            if (strcmp(flag, "--host") == 0) host = argv[i];
            else if (strcmp(flag, "--port") == 0) port = argv[i];
            else token = argv[i];
        } else if (strcmp(argv[i], "--no-tui") == 0) { force_no_tui = true; }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) { usage(argv[0]); return 0; }
        else { usage(argv[0]); return 2; }
    }
    char *owned_token = NULL;
    if (!token[0] && isatty(STDIN_FILENO)) {
        owned_token = read_secret_line("Core token (hidden; ASGEND_TOKEN may be used instead): ");
        if (owned_token) token = owned_token;
    }
    Client client = {.host = host, .port = port, .token = token};
    (void)signal(SIGPIPE, SIG_IGN);
    if (!force_no_tui && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
        int tui_status = asgend_tui_init(&cli_tui, "Рабочее пространство");
        if (tui_status > 0) {
            cli_tui_live = true;
            int result = cli_tui_loop(&client);
            cli_tui_live = false;
            asgend_tui_shutdown(&cli_tui);
            free(owned_token);
            return result;
        }
        asgend_tui_shutdown(&cli_tui);
    }
    if (argc == 1) puts("ASGEND core client · interactive task/chat mode");
    for (;;) {
        menu();
        char *choice = read_line("Выбор> ");
        if (!choice) { puts("\nВыход."); break; }
        if (strcmp(choice, "1") == 0) print_task_list(&client);
        else if (strcmp(choice, "2") == 0) submit_task(&client);
        else if (strcmp(choice, "3") == 0) open_task_chat(&client);
        else if (strcmp(choice, "4") == 0) show_health(&client);
        else if (strcmp(choice, "5") == 0) show_models(&client);
        else if (strcmp(choice, "6") == 0) {
            char *id = read_task_id_line(&client, "ID задачи для отмены (Tab — дополнить): ");
            if (id) { if (*id) cancel_task(&client, id); free(id); }
        } else if (strcmp(choice, "q") == 0 || strcmp(choice, "Q") == 0 || strcmp(choice, "0") == 0) { free(choice); break; }
        else puts("Неизвестный пункт меню.");
        free(choice);
    }
    free(owned_token);
    return 0;
}
