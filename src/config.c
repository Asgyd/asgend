#include "runtime.h"

#include <ctype.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <unistd.h>

static void set_error(char *dst, size_t n, const char *message) {
    if (dst && n) (void)snprintf(dst, n, "%s", message ? message : "unknown error");
}

int string_copy(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0 || !src) return -1;
    size_t len = strlen(src);
    if (len >= dst_size) return -1;
    memcpy(dst, src, len + 1);
    return 0;
}

char *string_duplicate(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

int json_get_string(json_object *obj, const char *key, const char **out) {
    json_object *value = NULL;
    if (!obj || !key || !out || !json_object_object_get_ex(obj, key, &value) ||
        !value || json_object_get_type(value) != json_type_string) return -1;
    *out = json_object_get_string(value);
    return *out ? 0 : -1;
}

int json_to_string(json_object *obj, char **out) {
    if (!obj || !out) return -1;
    const char *s = json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN);
    if (!s) return -1;
    *out = string_duplicate(s);
    return *out ? 0 : -1;
}

int path_mkdirs(const char *path, unsigned mode) {
    if (!path || !*path) return -1;
    char tmp[AO_MAX_PATH];
    if (string_copy(tmp, sizeof(tmp), path) != 0) return -1;
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') tmp[--len] = '\0';
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, (mode_t)mode) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, (mode_t)mode) != 0 && errno != EEXIST) return -1;
    struct stat st;
    if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
    return 0;
}

static bool path_is_within(const char *base, const char *path) {
    size_t n = strlen(base);
    return strncmp(base, path, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

/* Resolve the two explicit MCP path tokens. $ASGEND_TASKS is the dedicated
 * parent of per-task workspaces; it is created only when write access is
 * explicitly configured. No '..' or symlink component is accepted. */
static int resolve_mcp_policy_path(const Config *cfg, const char *raw,
                                   char *out, size_t out_size, bool create_directory) {
    if (!cfg || !raw || !out || out_size == 0) return -1;
    const char *base = NULL;
    const char *suffix = NULL;
    char tasks_base[AO_MAX_PATH];
    if (strncmp(raw, "$PROJECT_ROOT", 13) == 0 && (raw[13] == '\0' || raw[13] == '/')) {
        base = cfg->project_root;
        suffix = raw + 13;
    } else if (strncmp(raw, "$ASGEND_TASKS", 13) == 0 && (raw[13] == '\0' || raw[13] == '/')) {
        if (snprintf(tasks_base, sizeof(tasks_base), "%s/tasks", cfg->asgend_home) >= (int)sizeof(tasks_base)) return -1;
        base = tasks_base;
        suffix = raw + 13;
    } else if (raw[0] == '/' && !create_directory) {
        char resolved[PATH_MAX];
        struct stat st, lst;
        if (lstat(raw, &lst) != 0 || S_ISLNK(lst.st_mode) || !realpath(raw, resolved) ||
            stat(resolved, &st) != 0 || !(S_ISDIR(st.st_mode) || S_ISREG(st.st_mode))) return -1;
        return string_copy(out, out_size, resolved);
    } else {
        return -1;
    }

    struct stat base_lstat;
    if (create_directory && lstat(base, &base_lstat) != 0 && errno == ENOENT) {
        /* Only the explicit ASGEND home/tasks parent may be created here. */
        if (path_mkdirs(cfg->asgend_home, 0700) != 0) return -1;
        if (strcmp(base, tasks_base) == 0 && mkdir(base, 0700) != 0 && errno != EEXIST) return -1;
    }
    char real_base[PATH_MAX];
    if (lstat(base, &base_lstat) != 0 || S_ISLNK(base_lstat.st_mode) ||
        !S_ISDIR(base_lstat.st_mode) || !realpath(base, real_base)) return -1;

    if (strlen(real_base) >= out_size || string_copy(out, out_size, real_base) != 0) return -1;
    if (!suffix || !*suffix) return 0;
    if (*suffix != '/') return -1;
    const char *part = suffix + 1;
    while (*part) {
        const char *slash = strchr(part, '/');
        size_t part_len = slash ? (size_t)(slash - part) : strlen(part);
        if (part_len == 0 || (part_len == 1 && part[0] == '.') ||
            (part_len == 2 && part[0] == '.' && part[1] == '.') || part_len >= AO_MAX_NAME) return -1;
        char name[AO_MAX_NAME];
        memcpy(name, part, part_len); name[part_len] = '\0';
        size_t used = strlen(out);
        if (used + 1 + part_len >= out_size) return -1;
        out[used] = '/'; memcpy(out + used + 1, name, part_len + 1);
        struct stat component;
        if (lstat(out, &component) != 0) {
            if (!create_directory || errno != ENOENT || mkdir(out, 0700) != 0 || lstat(out, &component) != 0) return -1;
        }
        if (S_ISLNK(component.st_mode)) return -1;
        if (slash && !S_ISDIR(component.st_mode)) return -1;
        if (create_directory && !S_ISDIR(component.st_mode)) return -1;
        if (!slash && !(S_ISDIR(component.st_mode) || (!create_directory && S_ISREG(component.st_mode)))) return -1;
        if (!slash) break;
        part = slash + 1;
    }
    char resolved[PATH_MAX];
    struct stat final_st;
    if (!realpath(out, resolved) || lstat(out, &final_st) != 0 || S_ISLNK(final_st.st_mode) ||
        (create_directory ? !S_ISDIR(final_st.st_mode) : !(S_ISDIR(final_st.st_mode) || S_ISREG(final_st.st_mode))) ||
        !path_is_within(real_base, resolved)) return -1;
    return string_copy(out, out_size, resolved);
}

static int expand_mcp_argument(const Config *cfg, const char *raw,
                               char *out, size_t out_size) {
    if (!raw || !out || out_size == 0) return -1;
    if (strncmp(raw, "$PROJECT_ROOT", 13) == 0 || strncmp(raw, "$ASGEND_TASKS", 13) == 0)
        return resolve_mcp_policy_path(cfg, raw, out, out_size, strncmp(raw, "$ASGEND_TASKS", 13) == 0);
    return string_copy(out, out_size, raw);
}

static json_object *get_object(json_object *root, const char *key) {
    json_object *obj = NULL;
    if (root && json_object_object_get_ex(root, key, &obj) && obj &&
        json_object_get_type(obj) == json_type_object) return obj;
    return NULL;
}

static long get_long(json_object *obj, const char *key, long fallback) {
    json_object *v = NULL;
    if (!obj || !json_object_object_get_ex(obj, key, &v) || !v) return fallback;
    if (json_object_get_type(v) == json_type_int) return json_object_get_int64(v);
    return fallback;
}

static double get_double(json_object *obj, const char *key, double fallback) {
    json_object *v = NULL;
    if (!obj || !json_object_object_get_ex(obj, key, &v) || !v) return fallback;
    if (json_object_get_type(v) == json_type_double || json_object_get_type(v) == json_type_int)
        return json_object_get_double(v);
    return fallback;
}

static bool get_bool(json_object *obj, const char *key, bool fallback) {
    json_object *v = NULL;
    if (!obj || !json_object_object_get_ex(obj, key, &v) || !v) return fallback;
    if (json_object_get_type(v) == json_type_boolean) return json_object_get_boolean(v) != 0;
    return fallback;
}

static _Thread_local char config_invalid_string_field[128];

static void get_string_or(json_object *obj, const char *key, const char *field_label,
                          char *dst, size_t cap, const char *fallback) {
    json_object *value = NULL;
    bool present = obj && json_object_object_get_ex(obj, key, &value);
    if (!present) {
        (void)snprintf(dst, cap, "%s", fallback ? fallback : "");
        return;
    }
    const char *text = value && json_object_get_type(value) == json_type_string
        ? json_object_get_string(value) : NULL;
    if (!text || strlen(text) >= cap) {
        /* Record a safe field name, never the value (which may be a secret). */
        if (!config_invalid_string_field[0])
            (void)snprintf(config_invalid_string_field, sizeof(config_invalid_string_field), "%s",
                           field_label ? field_label : key);
        (void)snprintf(dst, cap, "%s", fallback ? fallback : "");
        return;
    }
    (void)snprintf(dst, cap, "%s", text);
}
static bool env_name_valid(const char *name) {
    if (!name) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        if (!( (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
               (*p >= '0' && *p <= '9') || *p == '_' )) return false;
    }
    return true;
}

static bool mcp_url_is_loopback_http(const char *url) {
    if (!url || strncmp(url, "http://", 7) != 0) return false;
    const char *host = url + 7;
    if (*host == '[') {
        const char *close = strchr(host, ']');
        return close && (size_t)(close - host - 1) == 3 && strncmp(host + 1, "::1", 3) == 0;
    }
    const char *end = host + strcspn(host, "/?#:");
    size_t length = (size_t)(end - host);
    return (length == 9 && strncasecmp(host, "localhost", length) == 0) ||
           (length == 9 && strncmp(host, "127.0.0.1", length) == 0);
}

static int resolve_path(char *dst, size_t dst_size, const char *root, const char *p) {
    if (!p || !*p) return -1;
    if (p[0] == '/') return string_copy(dst, dst_size, p);
    if (p[0] == '~' && p[1] == '/') {
        const char *home = getenv("HOME");
        if (!home || !*home) return -1;
        int hn = snprintf(dst, dst_size, "%s/%s", home, p + 2);
        return hn > 0 && (size_t)hn < dst_size ? 0 : -1;
    }
    int n = snprintf(dst, dst_size, "%s/%s", root, p);
    return n > 0 && (size_t)n < dst_size ? 0 : -1;
}

static bool lsp_language_id_valid(const char *language) {
    if (!language || !*language || strlen(language) >= 32) return false;
    for (const unsigned char *p = (const unsigned char *)language; *p; ++p)
        if (!(isalnum(*p) || *p == '_' || *p == '-')) return false;
    return true;
}

static void lsp_default_extensions(LSPServerConfig *server) {
    const char *defaults_c[] = {".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".hh", ".hxx"};
    const char *defaults_py[] = {".py", ".pyi"};
    const char *defaults_rs[] = {".rs"};
    const char *defaults_go[] = {".go"};
    const char *defaults_js[] = {".js", ".jsx", ".mjs", ".cjs"};
    const char *defaults_ts[] = {".ts", ".tsx", ".mts", ".cts"};
    const char *defaults_java[] = {".java"};
    const char *defaults_ruby[] = {".rb"};
    const char *defaults_shell[] = {".sh", ".bash"};
    const char **source = NULL; size_t count = 0;
    if (strcmp(server->language, "c") == 0 || strcmp(server->language, "cpp") == 0 || strcmp(server->language, "csharp") == 0) { source = defaults_c; count = sizeof(defaults_c)/sizeof(defaults_c[0]); }
    else if (strcmp(server->language, "python") == 0) { source = defaults_py; count = sizeof(defaults_py)/sizeof(defaults_py[0]); }
    else if (strcmp(server->language, "rust") == 0) { source = defaults_rs; count = sizeof(defaults_rs)/sizeof(defaults_rs[0]); }
    else if (strcmp(server->language, "go") == 0) { source = defaults_go; count = sizeof(defaults_go)/sizeof(defaults_go[0]); }
    else if (strcmp(server->language, "javascript") == 0 || strcmp(server->language, "javascriptreact") == 0) { source = defaults_js; count = sizeof(defaults_js)/sizeof(defaults_js[0]); }
    else if (strcmp(server->language, "typescript") == 0 || strcmp(server->language, "typescriptreact") == 0) { source = defaults_ts; count = sizeof(defaults_ts)/sizeof(defaults_ts[0]); }
    else if (strcmp(server->language, "java") == 0) { source = defaults_java; count = sizeof(defaults_java)/sizeof(defaults_java[0]); }
    else if (strcmp(server->language, "ruby") == 0) { source = defaults_ruby; count = sizeof(defaults_ruby)/sizeof(defaults_ruby[0]); }
    else if (strcmp(server->language, "shellscript") == 0) { source = defaults_shell; count = sizeof(defaults_shell)/sizeof(defaults_shell[0]); }
    for (size_t i = 0; source && i < count && i < AO_MAX_LSP_EXTENSIONS; ++i)
        (void)snprintf(server->extensions[server->extension_count++], sizeof(server->extensions[0]), "%s", source[i]);
}

int config_load(Config *cfg, const char *path, char *err, size_t err_size) {
    if (!cfg || !path) { set_error(err, err_size, "config path is required"); return -1; }
    config_invalid_string_field[0] = '\0';
    memset(cfg, 0, sizeof(*cfg));
    json_object *root_json = json_object_from_file(path);
    if (!root_json || json_object_get_type(root_json) != json_type_object) {
        set_error(err, err_size, "cannot read config JSON object");
        if (root_json) json_object_put(root_json);
        return -1;
    }

    char absolute[AO_MAX_PATH];
    if (!realpath(path, absolute)) {
        if (path[0] == '/') {
            if (string_copy(absolute, sizeof(absolute), path) != 0) {
                json_object_put(root_json); set_error(err, err_size, "config path too long"); return -1;
            }
        } else {
            if (snprintf(absolute, sizeof(absolute), "%s/%s", ".", path) >= (int)sizeof(absolute)) {
                json_object_put(root_json); set_error(err, err_size, "config path too long"); return -1;
            }
        }
    }
    char absolute_copy[AO_MAX_PATH];
    (void)snprintf(absolute_copy, sizeof(absolute_copy), "%s", absolute);
    char *config_dir = dirname(absolute_copy);
    char config_dir_copy[AO_MAX_PATH];
    (void)snprintf(config_dir_copy, sizeof(config_dir_copy), "%s", config_dir);
    char *leaf = strrchr(config_dir_copy, '/');
    const char *last = leaf ? leaf + 1 : config_dir_copy;
    if (strcmp(last, "config") == 0) {
        if (leaf) *leaf = '\0';
        if (!*config_dir_copy) (void)snprintf(cfg->root_dir, sizeof(cfg->root_dir), "/");
        else (void)snprintf(cfg->root_dir, sizeof(cfg->root_dir), "%s", config_dir_copy);
    } else {
        (void)snprintf(cfg->root_dir, sizeof(cfg->root_dir), "%s", config_dir_copy);
    }

    if (string_copy(cfg->main_config_path, sizeof(cfg->main_config_path), absolute) != 0) {
        json_object_put(root_json); set_error(err, err_size, "config file path too long"); return -1;
    }

    json_object *server = get_object(root_json, "server");
    json_object *runtime = get_object(root_json, "runtime");
    json_object *llm = get_object(root_json, "local_llm");
    json_object *defaults = get_object(root_json, "defaults");
    json_object *paths = get_object(root_json, "paths");
    json_object *mcp = get_object(root_json, "mcp");
    json_object *lsp = get_object(root_json, "lsp");
    json_object *unix_socket = get_object(server, "unix_socket");
    json_object *ui = get_object(root_json, "ui");
    json_object *sandbox = get_object(root_json, "sandbox");

    get_string_or(server, "host", "server.host", cfg->host, sizeof(cfg->host), "127.0.0.1");
    cfg->port = (int)get_long(server, "port", 8765);
    get_string_or(server, "auth_token", "server.auth_token", cfg->auth_token, sizeof(cfg->auth_token), "");
    cfg->max_request_bytes = (size_t)get_long(server, "max_request_bytes", AO_DEFAULT_REQ_MAX);
    cfg->max_connections = (int)get_long(server, "max_connections", 128);
    cfg->request_timeout_seconds = (int)get_long(server, "request_timeout_seconds", 30);

    cfg->workers = (int)get_long(runtime, "workers", 2);
    cfg->queue_max_pending = (int)get_long(runtime, "queue_max_pending", 1000);
    cfg->max_retries = (int)get_long(runtime, "max_retries", 2);
    cfg->priority_aging_seconds = (int)get_long(runtime, "priority_aging_seconds", 300);

    cfg->unix_socket_enabled = get_bool(unix_socket, "enabled", true);
    char unix_path[AO_MAX_PATH];
    get_string_or(unix_socket, "path", "server.unix_socket.path", unix_path, sizeof(unix_path), "data/agent-orchestrator.sock");
    if (resolve_path(cfg->unix_socket_path, sizeof(cfg->unix_socket_path), cfg->root_dir, unix_path) != 0) {
        json_object_put(root_json); set_error(err, err_size, "unix socket path is too long"); return -1;
    }

    cfg->ui_enabled = get_bool(ui, "enabled", false);
    cfg->ui_port = (int)get_long(ui, "port", 8766);
    get_string_or(ui, "auth_token", "ui.auth_token", cfg->ui_auth_token, sizeof(cfg->ui_auth_token), "");
    char ui_path[AO_MAX_PATH];
    get_string_or(ui, "directory", "ui.directory", ui_path, sizeof(ui_path), "ui");
    if (resolve_path(cfg->ui_dir, sizeof(cfg->ui_dir), cfg->root_dir, ui_path) != 0) {
        json_object_put(root_json); set_error(err, err_size, "UI directory path is too long"); return -1;
    }

    cfg->sandbox_enabled = get_bool(sandbox, "enabled", true);
    cfg->sandbox_require_landlock = get_bool(sandbox, "require_landlock", true);
    cfg->sandbox_allow_network = get_bool(sandbox, "allow_network", false);
    cfg->sandbox_cpu_seconds = (int)get_long(sandbox, "cpu_seconds", 120);
    cfg->sandbox_memory_mb = (int)get_long(sandbox, "memory_mb", 512);
    cfg->sandbox_file_size_mb = (int)get_long(sandbox, "file_size_mb", 64);
    cfg->sandbox_open_files = (int)get_long(sandbox, "open_files", 64);

    get_string_or(llm, "base_url", "local_llm.base_url", cfg->llm_base_url, sizeof(cfg->llm_base_url), "http://127.0.0.1:11434/v1");
    get_string_or(llm, "model", "local_llm.model", cfg->llm_model, sizeof(cfg->llm_model), "qwen2.5:7b");
    get_string_or(llm, "api_key", "local_llm.api_key", cfg->llm_api_key, sizeof(cfg->llm_api_key), "");
    cfg->llm_enabled = get_bool(llm, "enabled", false);
    cfg->llm_timeout_seconds = (int)get_long(llm, "timeout_seconds", 120);
    cfg->llm_temperature = get_double(llm, "temperature", 0.2);

    char db_path[AO_MAX_PATH];
    char asgend_home_path[AO_MAX_PATH];
    char models_cfg_path[AO_MAX_PATH];
    char agents_path[AO_MAX_PATH];
    char skills_path[AO_MAX_PATH];
    char knowledge_path[AO_MAX_PATH];
    char system_prompts_path[AO_MAX_PATH];
    char tools_path[AO_MAX_PATH];
    char instructions_path[AO_MAX_PATH];
    char project_root_path[AO_MAX_PATH];
    get_string_or(runtime, "database_path", "runtime.database_path", db_path, sizeof(db_path), "data/tasks.sqlite3");
    const char *home_override = getenv("ASGEND_HOME");
    char home_default[AO_MAX_PATH];
    if (home_override && *home_override) {
        if (string_copy(home_default, sizeof(home_default), home_override) != 0) {
            json_object_put(root_json); set_error(err, err_size, "ASGEND_HOME is too long"); return -1;
        }
    } else {
        const char *user_home = getenv("HOME");
        if (!user_home || !*user_home) user_home = cfg->root_dir;
        if (snprintf(home_default, sizeof(home_default), "%s/.asgend", user_home) >= (int)sizeof(home_default)) {
            json_object_put(root_json); set_error(err, err_size, "default ASGEND home path is too long"); return -1;
        }
    }
    get_string_or(paths, "asgend_home", "paths.asgend_home", asgend_home_path, sizeof(asgend_home_path), home_default);
    get_string_or(paths, "agents_dir", "paths.agents_dir", agents_path, sizeof(agents_path), "config/agents");
    get_string_or(paths, "models_config", "paths.models_config", models_cfg_path, sizeof(models_cfg_path), "");
    get_string_or(defaults, "model_id", "defaults.model_id", cfg->default_model_id, sizeof(cfg->default_model_id), "");
    get_string_or(defaults, "decision_model_id", "defaults.decision_model_id", cfg->decision_model_id, sizeof(cfg->decision_model_id), "");
    get_string_or(defaults, "analysis_model_id", "defaults.analysis_model_id", cfg->analysis_model_id, sizeof(cfg->analysis_model_id), "");
    get_string_or(paths, "skills_dir", "paths.skills_dir", skills_path, sizeof(skills_path), "skills");
    get_string_or(paths, "knowledge_dir", "paths.knowledge_dir", knowledge_path, sizeof(knowledge_path), "knowledge");
    get_string_or(paths, "system_prompts_dir", "paths.system_prompts_dir", system_prompts_path, sizeof(system_prompts_path), "system_prompts");
    get_string_or(paths, "tools_dir", "paths.tools_dir", tools_path, sizeof(tools_path), "tools");
    get_string_or(paths, "instructions_dir", "paths.instructions_dir", instructions_path, sizeof(instructions_path), "~/.config/asgend/instructions");
    get_string_or(paths, "project_root", "paths.project_root", project_root_path, sizeof(project_root_path), cfg->root_dir);
    if (resolve_path(cfg->database_path, sizeof(cfg->database_path), cfg->root_dir, db_path) != 0 ||
        resolve_path(cfg->asgend_home, sizeof(cfg->asgend_home), cfg->root_dir, asgend_home_path) != 0 ||
        resolve_path(cfg->agents_dir, sizeof(cfg->agents_dir), cfg->root_dir, agents_path) != 0 ||
        (models_cfg_path[0] && resolve_path(cfg->models_config_path, sizeof(cfg->models_config_path), cfg->root_dir, models_cfg_path) != 0) ||
        resolve_path(cfg->skills_dir, sizeof(cfg->skills_dir), cfg->root_dir, skills_path) != 0 ||
        resolve_path(cfg->knowledge_dir, sizeof(cfg->knowledge_dir), cfg->root_dir, knowledge_path) != 0 ||
        resolve_path(cfg->system_prompts_dir, sizeof(cfg->system_prompts_dir), cfg->root_dir, system_prompts_path) != 0 ||
        resolve_path(cfg->tools_dir, sizeof(cfg->tools_dir), cfg->root_dir, tools_path) != 0 ||
        resolve_path(cfg->instructions_dir, sizeof(cfg->instructions_dir), cfg->root_dir, instructions_path) != 0 ||
        resolve_path(cfg->project_root, sizeof(cfg->project_root), cfg->root_dir, project_root_path) != 0) {
        json_object_put(root_json); set_error(err, err_size, "configured path is too long"); return -1;
    }
    struct stat project_root_st;
    if (lstat(cfg->project_root, &project_root_st) != 0 ||
        !S_ISDIR(project_root_st.st_mode) || S_ISLNK(project_root_st.st_mode)) {
        json_object_put(root_json);
        set_error(err, err_size, "paths.project_root must be an existing non-symlink directory");
        return -1;
    }

    json_object *mcp_servers = NULL;
    if (mcp && json_object_object_get_ex(mcp, "servers", &mcp_servers) &&
        mcp_servers && json_object_get_type(mcp_servers) == json_type_array) {
        size_t count = json_object_array_length(mcp_servers);
        if (count > AO_MAX_MCP_SERVERS) {
            json_object_put(root_json); set_error(err, err_size, "mcp.servers exceeds supported limit"); return -1;
        }
        for (size_t i = 0; i < count; ++i) {
            json_object *entry = json_object_array_get_idx(mcp_servers, i);
            if (!entry || json_object_get_type(entry) != json_type_object) {
                json_object_put(root_json); set_error(err, err_size, "each mcp.servers entry must be an object"); return -1;
            }
            json_object *enabled_obj = NULL;
            bool enabled = true;
            if (json_object_object_get_ex(entry, "enabled", &enabled_obj) && enabled_obj && json_object_get_type(enabled_obj) == json_type_boolean)
                enabled = json_object_get_boolean(enabled_obj) != 0;
            if (!enabled) continue;
            MCPServerConfig *mc = &cfg->mcp_servers[cfg->mcp_server_count];
            memset(mc, 0, sizeof(*mc));
            get_string_or(entry, "name", "mcp.servers[].name", mc->name, sizeof(mc->name), "");
            get_string_or(entry, "transport", "mcp.servers[].transport", mc->transport, sizeof(mc->transport), "stdio");
            get_string_or(entry, "command", "mcp.servers[].command", mc->command, sizeof(mc->command), "");
            get_string_or(entry, "url", "mcp.servers[].url", mc->url, sizeof(mc->url), "");
            get_string_or(entry, "bearer_token_env", "mcp.servers[].bearer_token_env", mc->bearer_token_env, sizeof(mc->bearer_token_env), "");
            mc->allow_insecure_http = get_bool(entry, "allow_insecure_http", false);
            mc->timeout_seconds = (int)get_long(entry, "timeout_seconds", 15);
            bool stdio_transport = strcmp(mc->transport, "stdio") == 0;
            bool http_transport = strcmp(mc->transport, "streamable_http") == 0;
            if (!mc->name[0] || (!stdio_transport && !http_transport)) {
                json_object_put(root_json); set_error(err, err_size, "enabled MCP server needs a name and transport stdio or streamable_http"); return -1;
            }
            if (stdio_transport && !mc->command[0]) {
                json_object_put(root_json); set_error(err, err_size, "stdio MCP server requires command"); return -1;
            }
            if (http_transport && !mc->url[0]) {
                json_object_put(root_json); set_error(err, err_size, "streamable_http MCP server requires url"); return -1;
            }
            if (http_transport && strncmp(mc->url, "https://", 8) != 0 &&
                !(mc->allow_insecure_http && strncmp(mc->url, "http://", 7) == 0) &&
                !mcp_url_is_loopback_http(mc->url)) {
                json_object_put(root_json); set_error(err, err_size, "remote Streamable HTTP MCP endpoints must use HTTPS; plain HTTP is allowed only for loopback or explicit allow_insecure_http"); return -1;
            }
            if (http_transport && strncmp(mc->url, "http://", 7) != 0 && strncmp(mc->url, "https://", 8) != 0) {
                json_object_put(root_json); set_error(err, err_size, "Streamable HTTP MCP url must begin with http:// or https://"); return -1;
            }
            if (!env_name_valid(mc->bearer_token_env)) {
                json_object_put(root_json); set_error(err, err_size, "MCP bearer_token_env may contain only letters, digits and underscore"); return -1;
            }
            for (const char *p = mc->name; *p; ++p) {
                if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                       (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' )) {
                    json_object_put(root_json); set_error(err, err_size, "MCP server names may contain only letters, digits, underscore and hyphen"); return -1;
                }
            }
            for (size_t prev = 0; prev < cfg->mcp_server_count; ++prev) {
                if (strcmp(cfg->mcp_servers[prev].name, mc->name) == 0) {
                    json_object_put(root_json); set_error(err, err_size, "duplicate MCP server name"); return -1;
                }
            }
            if (mc->timeout_seconds < 1 || mc->timeout_seconds > 300) {
                json_object_put(root_json); set_error(err, err_size, "MCP timeout_seconds must be in 1..300"); return -1;
            }
            json_object *args = NULL;
            if (json_object_object_get_ex(entry, "args", &args) && args && json_object_get_type(args) == json_type_array) {
                size_t argc = json_object_array_length(args);
                if (argc > AO_MAX_MCP_ARGS) { json_object_put(root_json); set_error(err, err_size, "too many arguments for MCP server"); return -1; }
                for (size_t j = 0; j < argc; ++j) {
                    json_object *av = json_object_array_get_idx(args, j);
                    if (!av || json_object_get_type(av) != json_type_string) { json_object_put(root_json); set_error(err, err_size, "MCP args must be strings"); return -1; }
                    const char *as = json_object_get_string(av);
                    if (!as || strlen(as) >= AO_MAX_PATH ||
                        expand_mcp_argument(cfg, as, mc->args[mc->arg_count], AO_MAX_PATH) != 0) {
                        json_object_put(root_json); set_error(err, err_size, "MCP argument is too long or an unsafe project path token"); return -1;
                    }
                    mc->arg_count++;
                }
            } else if (args && json_object_get_type(args) != json_type_array) {
                json_object_put(root_json); set_error(err, err_size, "MCP args must be an array of strings"); return -1;
            }
            json_object *env_obj = NULL;
            if (json_object_object_get_ex(entry, "env", &env_obj)) {
                if (http_transport) {
                    json_object_put(root_json); set_error(err, err_size, "mcp env is supported only for stdio servers"); return -1;
                }
                if (!env_obj || json_object_get_type(env_obj) != json_type_object ||
                    json_object_object_length(env_obj) > AO_MAX_MCP_ENV_VARS) {
                    json_object_put(root_json); set_error(err, err_size, "mcp env must be an object with at most 16 string values"); return -1;
                }
                json_object_object_foreach(env_obj, env_key, env_value) {
                    if (!env_name_valid(env_key) || !env_key[0] ||
                        !((env_key[0] >= 'A' && env_key[0] <= 'Z') ||
                          (env_key[0] >= 'a' && env_key[0] <= 'z') || env_key[0] == '_')) {
                        json_object_put(root_json); set_error(err, err_size, "MCP env names must begin with a letter or underscore and contain only letters, digits and underscore"); return -1;
                    }
                    if (strcmp(env_key, "PATH") == 0 || strcmp(env_key, "HOME") == 0 ||
                        strcmp(env_key, "TMPDIR") == 0 || strncmp(env_key, "LD_", 3) == 0 ||
                        strncmp(env_key, "DYLD_", 5) == 0) {
                        json_object_put(root_json); set_error(err, err_size, "MCP env cannot override PATH, HOME, TMPDIR, or dynamic-loader variables"); return -1;
                    }
                    if (!env_value || json_object_get_type(env_value) != json_type_string) {
                        json_object_put(root_json); set_error(err, err_size, "MCP env values must be strings"); return -1;
                    }
                    const char *value = json_object_get_string(env_value);
                    if (!value || strlen(env_key) >= AO_MAX_MCP_ENV_NAME || strlen(value) >= AO_MAX_PATH) {
                        json_object_put(root_json); set_error(err, err_size, "MCP env name or value exceeds the supported length"); return -1;
                    }
                    MCPEnvironmentVariable *var = &mc->env[mc->env_count++];
                    (void)snprintf(var->name, sizeof(var->name), "%s", env_key);
                    if (expand_mcp_argument(cfg, value, var->value, sizeof(var->value)) != 0) {
                        json_object_put(root_json); set_error(err, err_size, "MCP env contains an unsafe project path token"); return -1;
                    }
                }
            }
            json_object *read_paths = NULL;
            if (json_object_object_get_ex(entry, "read_only_paths", &read_paths)) {
                if (!read_paths || json_object_get_type(read_paths) != json_type_array || json_object_array_length(read_paths) > AO_MAX_MCP_READ_PATHS) {
                    json_object_put(root_json); set_error(err, err_size, "mcp read_only_paths must be an array with at most 16 absolute paths"); return -1;
                }
                for (size_t k = 0; k < json_object_array_length(read_paths); ++k) {
                    json_object *rv = json_object_array_get_idx(read_paths, k);
                    if (!rv || json_object_get_type(rv) != json_type_string) {
                        json_object_put(root_json); set_error(err, err_size, "mcp read_only_paths items must be strings"); return -1;
                    }
                    const char *rp = json_object_get_string(rv);
                    char resolved_read_path[AO_MAX_PATH];
                    if (!rp || strlen(rp) >= AO_MAX_PATH ||
                        resolve_mcp_policy_path(cfg, rp, resolved_read_path, sizeof(resolved_read_path), false) != 0) {
                        json_object_put(root_json); set_error(err, err_size, "mcp read_only_paths must be existing absolute paths or safe $PROJECT_ROOT/$ASGEND_TASKS tokens"); return -1;
                    }
                    struct stat rp_st;
                    if (stat(resolved_read_path, &rp_st) != 0 || !(S_ISDIR(rp_st.st_mode) || S_ISREG(rp_st.st_mode))) {
                        json_object_put(root_json); set_error(err, err_size, "MCP read_only_path must name a regular file or directory"); return -1;
                    }
                    (void)snprintf(mc->read_only_paths[mc->read_only_path_count++], AO_MAX_PATH, "%s", resolved_read_path);
                }
            }
            json_object *write_paths = NULL;
            if (json_object_object_get_ex(entry, "write_paths", &write_paths)) {
                if (!write_paths || json_object_get_type(write_paths) != json_type_array ||
                    json_object_array_length(write_paths) > AO_MAX_MCP_WRITE_PATHS) {
                    json_object_put(root_json); set_error(err, err_size, "mcp write_paths must be an array with at most 8 dedicated writable directories"); return -1;
                }
                for (size_t k = 0; k < json_object_array_length(write_paths); ++k) {
                    json_object *wv = json_object_array_get_idx(write_paths, k);
                    if (!wv || json_object_get_type(wv) != json_type_string) {
                        json_object_put(root_json); set_error(err, err_size, "mcp write_paths items must be strings"); return -1;
                    }
                    const char *wp = json_object_get_string(wv);
                    char resolved_write_path[AO_MAX_PATH];
                    if (!wp || strlen(wp) >= AO_MAX_PATH ||
                        resolve_mcp_policy_path(cfg, wp, resolved_write_path, sizeof(resolved_write_path), true) != 0) {
                        json_object_put(root_json); set_error(err, err_size, "mcp write_paths must use safe $PROJECT_ROOT or $ASGEND_TASKS directory tokens"); return -1;
                    }
                    (void)snprintf(mc->write_paths[mc->write_path_count++], AO_MAX_PATH, "%s", resolved_write_path);
                }
            }
            cfg->mcp_server_count++;
        }
    } else if (mcp_servers && json_object_get_type(mcp_servers) != json_type_array) {
        json_object_put(root_json); set_error(err, err_size, "mcp.servers must be an array"); return -1;
    }

    json_object *lsp_servers = NULL;
    if (lsp && json_object_object_get_ex(lsp, "servers", &lsp_servers)) {
        if (!lsp_servers || json_object_get_type(lsp_servers) != json_type_array ||
            json_object_array_length(lsp_servers) > AO_MAX_LSP_SERVERS) {
            json_object_put(root_json); set_error(err, err_size, "lsp.servers must be an array within the supported limit"); return -1;
        }
        for (size_t i = 0; i < json_object_array_length(lsp_servers); ++i) {
            json_object *entry = json_object_array_get_idx(lsp_servers, i);
            if (!entry || json_object_get_type(entry) != json_type_object) {
                json_object_put(root_json); set_error(err, err_size, "each lsp.servers entry must be an object"); return -1;
            }
            json_object *enabled_obj = NULL;
            bool enabled = true;
            if (json_object_object_get_ex(entry, "enabled", &enabled_obj)) {
                if (!enabled_obj || json_object_get_type(enabled_obj) != json_type_boolean) {
                    json_object_put(root_json); set_error(err, err_size, "LSP enabled must be a boolean"); return -1;
                }
                enabled = json_object_get_boolean(enabled_obj) != 0;
            }
            if (!enabled) continue;
            if (cfg->lsp_server_count >= AO_MAX_LSP_SERVERS) {
                json_object_put(root_json); set_error(err, err_size, "too many enabled LSP servers"); return -1;
            }
            LSPServerConfig *lsp_cfg = &cfg->lsp_servers[cfg->lsp_server_count];
            memset(lsp_cfg, 0, sizeof(*lsp_cfg));
            get_string_or(entry, "language", "lsp.servers[].language", lsp_cfg->language, sizeof(lsp_cfg->language), "");
            get_string_or(entry, "command", "lsp.servers[].command", lsp_cfg->command, sizeof(lsp_cfg->command), "");
            lsp_cfg->timeout_seconds = (int)get_long(entry, "timeout_seconds", 10);
            lsp_cfg->enabled = true;
            if (!lsp_language_id_valid(lsp_cfg->language) || !lsp_cfg->command[0]) {
                json_object_put(root_json); set_error(err, err_size, "enabled LSP server needs a valid language id and command"); return -1;
            }
            if (lsp_cfg->timeout_seconds < 1 || lsp_cfg->timeout_seconds > 60) {
                json_object_put(root_json); set_error(err, err_size, "LSP timeout_seconds must be in 1..60"); return -1;
            }
            char root_dir[AO_MAX_PATH];
            get_string_or(entry, "root_dir", "lsp.servers[].root_dir", root_dir, sizeof(root_dir), cfg->root_dir);
            if (resolve_path(lsp_cfg->root_dir, sizeof(lsp_cfg->root_dir), cfg->root_dir, root_dir) != 0) {
                json_object_put(root_json); set_error(err, err_size, "LSP root_dir is too long"); return -1;
            }
            struct stat root_st;
            if (stat(lsp_cfg->root_dir, &root_st) != 0 || !S_ISDIR(root_st.st_mode)) {
                json_object_put(root_json); set_error(err, err_size, "LSP root_dir must exist and be a directory"); return -1;
            }
            json_object *args = NULL;
            if (json_object_object_get_ex(entry, "args", &args)) {
                if (!args || json_object_get_type(args) != json_type_array || json_object_array_length(args) > AO_MAX_LSP_ARGS) {
                    json_object_put(root_json); set_error(err, err_size, "LSP args must be an array within the supported limit"); return -1;
                }
                for (size_t j = 0; j < json_object_array_length(args); ++j) {
                    json_object *arg = json_object_array_get_idx(args, j);
                    if (!arg || json_object_get_type(arg) != json_type_string || !json_object_get_string(arg) ||
                        strlen(json_object_get_string(arg)) >= AO_MAX_PATH) {
                        json_object_put(root_json); set_error(err, err_size, "LSP args must contain bounded strings"); return -1;
                    }
                    (void)snprintf(lsp_cfg->args[lsp_cfg->arg_count++], AO_MAX_PATH, "%s", json_object_get_string(arg));
                }
            }
            json_object *extensions = NULL;
            if (json_object_object_get_ex(entry, "extensions", &extensions)) {
                if (!extensions || json_object_get_type(extensions) != json_type_array ||
                    json_object_array_length(extensions) > AO_MAX_LSP_EXTENSIONS) {
                    json_object_put(root_json); set_error(err, err_size, "LSP extensions must be an array within the supported limit"); return -1;
                }
                for (size_t j = 0; j < json_object_array_length(extensions); ++j) {
                    json_object *extension = json_object_array_get_idx(extensions, j);
                    if (!extension || json_object_get_type(extension) != json_type_string) {
                        json_object_put(root_json); set_error(err, err_size, "LSP file extensions must be strings"); return -1;
                    }
                    const char *ext = json_object_get_string(extension);
                    if (!ext || ext[0] != '.' || strlen(ext) < 2 || strlen(ext) >= sizeof(lsp_cfg->extensions[0])) {
                        json_object_put(root_json); set_error(err, err_size, "LSP file extension must start with '.' and be shorter than 24 bytes"); return -1;
                    }
                    (void)snprintf(lsp_cfg->extensions[lsp_cfg->extension_count++], sizeof(lsp_cfg->extensions[0]), "%s", ext);
                }
            } else {
                lsp_default_extensions(lsp_cfg);
            }
            if (lsp_cfg->extension_count == 0) {
                json_object_put(root_json); set_error(err, err_size, "LSP server must provide at least one file extension"); return -1;
            }
            for (size_t prev = 0; prev < cfg->lsp_server_count; ++prev) {
                for (size_t x = 0; x < lsp_cfg->extension_count; ++x) {
                    for (size_t y = 0; y < cfg->lsp_servers[prev].extension_count; ++y) {
                        if (strcmp(lsp_cfg->extensions[x], cfg->lsp_servers[prev].extensions[y]) == 0) {
                            json_object_put(root_json); set_error(err, err_size, "LSP file extensions must not map to multiple enabled servers"); return -1;
                        }
                    }
                }
            }
            ++cfg->lsp_server_count;
        }
    }

    if (config_invalid_string_field[0]) {
        char field[sizeof(config_invalid_string_field)];
        (void)snprintf(field, sizeof(field), "%s", config_invalid_string_field);
        json_object_put(root_json);
        if (err && err_size)
            (void)snprintf(err, err_size, "configuration field '%s' must be a string within its configured length limit", field);
        return -1;
    }
    json_object_put(root_json);
    if (cfg->port < 1 || cfg->port > 65535) { set_error(err, err_size, "server.port must be in 1..65535"); return -1; }
    if (cfg->max_request_bytes < 1024 || cfg->max_request_bytes > AO_MAX_REQ_MAX) { set_error(err, err_size, "server.max_request_bytes must be in 1024..16777216"); return -1; }
    if (cfg->max_connections < 1 || cfg->max_connections > 4096) { set_error(err, err_size, "server.max_connections must be in 1..4096"); return -1; }
    if (cfg->request_timeout_seconds < 1 || cfg->request_timeout_seconds > 600) { set_error(err, err_size, "server.request_timeout_seconds must be in 1..600"); return -1; }
    if (cfg->workers < 1 || cfg->workers > 64) { set_error(err, err_size, "runtime.workers must be in 1..64"); return -1; }
    if (cfg->queue_max_pending < 1 || cfg->queue_max_pending > 1000000) { set_error(err, err_size, "runtime.queue_max_pending must be in 1..1000000"); return -1; }
    if (cfg->max_retries < 0 || cfg->max_retries > 10) { set_error(err, err_size, "runtime.max_retries must be in 0..10"); return -1; }
    if (cfg->priority_aging_seconds < 1 || cfg->priority_aging_seconds > 86400) { set_error(err, err_size, "runtime.priority_aging_seconds must be in 1..86400"); return -1; }
    if (cfg->ui_port < 1 || cfg->ui_port > 65535) { set_error(err, err_size, "ui.port must be in 1..65535"); return -1; }
    if (cfg->ui_enabled && strlen(cfg->ui_auth_token) < 32) { set_error(err, err_size, "ui.auth_token must contain at least 32 characters when UI is enabled"); return -1; }
    if (cfg->sandbox_cpu_seconds < 1 || cfg->sandbox_cpu_seconds > 86400) { set_error(err, err_size, "sandbox.cpu_seconds must be in 1..86400"); return -1; }
    if (cfg->sandbox_memory_mb < 64 || cfg->sandbox_memory_mb > 65536) { set_error(err, err_size, "sandbox.memory_mb must be in 64..65536"); return -1; }
    if (cfg->sandbox_file_size_mb < 1 || cfg->sandbox_file_size_mb > 4096) { set_error(err, err_size, "sandbox.file_size_mb must be in 1..4096"); return -1; }
    if (cfg->sandbox_open_files < 16 || cfg->sandbox_open_files > 4096) { set_error(err, err_size, "sandbox.open_files must be in 16..4096"); return -1; }
    if (cfg->llm_timeout_seconds < 1 || cfg->llm_timeout_seconds > 900) { set_error(err, err_size, "local_llm.timeout_seconds must be in 1..900"); return -1; }
    if (cfg->llm_temperature < 0.0 || cfg->llm_temperature > 2.0) { set_error(err, err_size, "local_llm.temperature must be in 0..2"); return -1; }
    if ((strcmp(cfg->host, "127.0.0.1") != 0 && strcmp(cfg->host, "::1") != 0 &&
         strcmp(cfg->host, "localhost") != 0) && cfg->auth_token[0] == '\0') {
        set_error(err, err_size, "server.auth_token is required when binding to a non-loopback host"); return -1;
    }
    return 0;
}
