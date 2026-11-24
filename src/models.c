#include "runtime.h"

#include <curl/curl.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MODEL_MAX_ARGV (AO_MAX_MODEL_ARGS + 2)
#define MODEL_ARG_EXPANSION_MAX (AO_MAX_PATH * 2)

static void set_error(char *dst, size_t cap, const char *message) {
    if (dst && cap) (void)snprintf(dst, cap, "%s", message ? message : "model error");
}

static json_object *obj_get(json_object *obj, const char *key) {
    json_object *value = NULL;
    if (!obj || !json_object_object_get_ex(obj, key, &value)) return NULL;
    return value;
}

static long int_get(json_object *obj, const char *key, long fallback) {
    json_object *value = obj_get(obj, key);
    return value && json_object_get_type(value) == json_type_int ? json_object_get_int64(value) : fallback;
}

static double double_get(json_object *obj, const char *key, double fallback) {
    json_object *value = obj_get(obj, key);
    if (!value) return fallback;
    if (json_object_get_type(value) == json_type_double || json_object_get_type(value) == json_type_int)
        return json_object_get_double(value);
    return fallback;
}

static bool bool_get(json_object *obj, const char *key, bool fallback) {
    json_object *value = obj_get(obj, key);
    return value && json_object_get_type(value) == json_type_boolean ? json_object_get_boolean(value) != 0 : fallback;
}

static void str_get(json_object *obj, const char *key, char *dst, size_t cap, const char *fallback) {
    if (!dst || cap == 0) return;
    const char *value = NULL;
    if (json_get_string(obj, key, &value) == 0 && value && strlen(value) < cap) {
        size_t n = strlen(value);
        memmove(dst, value, n);
        dst[n] = '\0';
    } else {
        const char *source = fallback ? fallback : "";
        size_t n = strlen(source);
        if (n >= cap) n = cap - 1;
        memmove(dst, source, n);
        dst[n] = '\0';
    }
}

static int resolve_root_path(char *dst, size_t cap, const char *root, const char *path) {
    if (!path || !*path) { if (cap) dst[0] = '\0'; return 0; }
    int n = path[0] == '/' ? snprintf(dst, cap, "%s", path) : snprintf(dst, cap, "%s/%s", root, path);
    return n >= 0 && (size_t)n < cap ? 0 : -1;
}

static bool valid_id(const char *id) {
    if (!id || !*id || strlen(id) >= AO_MAX_ID) return false;
    for (const unsigned char *p = (const unsigned char *)id; *p; ++p)
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return false;
    return true;
}

static bool starts_with(const char *value, const char *prefix) {
    return value && prefix && strncmp(value, prefix, strlen(prefix)) == 0;
}

static int validate_nodes_json(json_object *nodes, char *err, size_t err_size) {
    if (!nodes) return 0;
    if (json_object_get_type(nodes) != json_type_array) { set_error(err, err_size, "nodes must be an array"); return -1; }
    size_t count = json_object_array_length(nodes);
    if (count > 64) { set_error(err, err_size, "node registry exceeds 64 entries"); return -1; }
    for (size_t i = 0; i < count; ++i) {
        json_object *node = json_object_array_get_idx(nodes, i);
        json_object *id_obj = obj_get(node, "id"), *name_obj = obj_get(node, "name"), *type_obj = obj_get(node, "type");
        if (!node || json_object_get_type(node) != json_type_object || !id_obj || json_object_get_type(id_obj) != json_type_string ||
            !name_obj || json_object_get_type(name_obj) != json_type_string || !json_object_get_string(name_obj)[0] ||
            !type_obj || json_object_get_type(type_obj) != json_type_string) {
            set_error(err, err_size, "each node requires string id, name and type fields"); return -1;
        }
        const char *id = json_object_get_string(id_obj), *type = json_object_get_string(type_obj);
        if (!valid_id(id)) { set_error(err, err_size, "node id must use letters, digits, underscore or hyphen"); return -1; }
        if (strcmp(type, "local") != 0 && strcmp(type, "lan") != 0 && strcmp(type, "internet") != 0) {
            set_error(err, err_size, "node type must be local, lan or internet"); return -1;
        }
        for (size_t j = 0; j < i; ++j) {
            json_object *prev = json_object_array_get_idx(nodes, j), *prev_id_obj = obj_get(prev, "id");
            if (prev_id_obj && json_object_get_type(prev_id_obj) == json_type_string && strcmp(id, json_object_get_string(prev_id_obj)) == 0) {
                if (err && err_size) snprintf(err, err_size, "duplicate node id '%s'", id);
                return -1;
            }
        }
        json_object *enabled_obj = obj_get(node, "enabled");
        bool enabled = enabled_obj && json_object_get_type(enabled_obj) == json_type_boolean && json_object_get_boolean(enabled_obj);
        json_object *endpoint_obj = obj_get(node, "endpoint"), *health_obj = obj_get(node, "health_url");
        json_object *api_type_obj = obj_get(node, "api_type");
        const char *endpoint = endpoint_obj && json_object_get_type(endpoint_obj) == json_type_string ? json_object_get_string(endpoint_obj) : "";
        const char *health = health_obj && json_object_get_type(health_obj) == json_type_string ? json_object_get_string(health_obj) : "";
        if (endpoint_obj && json_object_get_type(endpoint_obj) != json_type_string) { set_error(err, err_size, "node endpoint must be a string"); return -1; }
        if (health_obj && json_object_get_type(health_obj) != json_type_string) { set_error(err, err_size, "node health_url must be a string"); return -1; }
        if (api_type_obj && (json_object_get_type(api_type_obj) != json_type_string ||
            (strcmp(json_object_get_string(api_type_obj), "openai_compatible") != 0 &&
             strcmp(json_object_get_string(api_type_obj), "systemone") != 0))) {
            set_error(err, err_size, "node api_type must be openai_compatible or systemone"); return -1;
        }
        if (strcmp(type, "internet") == 0) {
            if ((endpoint[0] && !starts_with(endpoint, "https://")) || (health[0] && !starts_with(health, "https://"))) {
                if (err && err_size) snprintf(err, err_size, "internet node '%s' must use HTTPS endpoints", id);
                return -1;
            }
            if (enabled && !endpoint[0]) { if (err && err_size) snprintf(err, err_size, "enabled internet node '%s' requires an HTTPS endpoint", id); return -1; }
        } else if (strcmp(type, "lan") == 0) {
            if ((endpoint[0] && !starts_with(endpoint, "http://") && !starts_with(endpoint, "https://")) ||
                (health[0] && !starts_with(health, "http://") && !starts_with(health, "https://"))) {
                if (err && err_size) snprintf(err, err_size, "LAN node '%s' endpoint must use HTTP or HTTPS", id);
                return -1;
            }
            if (enabled && !endpoint[0]) { if (err && err_size) snprintf(err, err_size, "enabled LAN node '%s' requires an endpoint", id); return -1; }
        }
        json_object *key_env_obj = obj_get(node, "api_key_env");
        if (key_env_obj && json_object_get_type(key_env_obj) != json_type_string) { set_error(err, err_size, "node api_key_env must be a string"); return -1; }
        const char *key_env = key_env_obj ? json_object_get_string(key_env_obj) : "";
        for (const unsigned char *p = (const unsigned char *)key_env; p && *p; ++p) {
            if (!( (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) {
                if (err && err_size) snprintf(err, err_size, "node '%s' has invalid api_key_env name", id);
                return -1;
            }
        }
    }
    return 0;
}

static bool valid_modality_csv(const char *value, const char *const *allowed, size_t allowed_count) {
    if (!value || !*value) return false;
    char copy[256];
    if (strlen(value) >= sizeof(copy)) return false;
    (void)snprintf(copy, sizeof(copy), "%s", value);
    bool seen[8] = {false};
    char *save = NULL;
    for (char *part = strtok_r(copy, ",", &save); part; part = strtok_r(NULL, ",", &save)) {
        if (!*part) return false;
        size_t match = allowed_count;
        for (size_t i = 0; i < allowed_count; ++i) if (strcmp(part, allowed[i]) == 0) { match = i; break; }
        if (match == allowed_count || match >= sizeof(seen) / sizeof(seen[0]) || seen[match]) return false;
        seen[match] = true;
    }
    return true;
}

static int parse_model(Runtime *rt, json_object *item, bool decision,
                       LocalModel *out, char *err, size_t err_size) {
    memset(out, 0, sizeof(*out));
    out->actual_gpu_memory_mb = -1;
    str_get(item, "id", out->id, sizeof(out->id), "");
    str_get(item, "name", out->name, sizeof(out->name), out->id);
    out->enabled = bool_get(item, "enabled", false);
    out->auto_start = bool_get(item, "auto_start", false);
    out->start_at_boot = bool_get(item, "start_at_boot", false);
    out->is_decision = decision;
    (void)snprintf(out->root_dir, sizeof(out->root_dir), "%s", rt->config.root_dir);
    str_get(item, "executable", out->executable, sizeof(out->executable), "");
    str_get(item, "model_path", out->model_path, sizeof(out->model_path), "");
    str_get(item, "working_directory", out->working_directory, sizeof(out->working_directory), rt->config.root_dir);
    str_get(item, "base_url", out->base_url, sizeof(out->base_url), "");
    str_get(item, "api_type", out->api_type, sizeof(out->api_type), "openai_compatible");
    str_get(item, "model_role", out->model_role, sizeof(out->model_role), "general");
    str_get(item, "runtime_adapter", out->runtime_adapter, sizeof(out->runtime_adapter), "openai_compatible");
    str_get(item, "input_modalities", out->input_modalities, sizeof(out->input_modalities), "text");
    str_get(item, "output_modalities", out->output_modalities, sizeof(out->output_modalities), "text");
    str_get(item, "health_url", out->health_url, sizeof(out->health_url), "");
    str_get(item, "model_name", out->model_name, sizeof(out->model_name), out->id);
    str_get(item, "api_key", out->api_key, sizeof(out->api_key), "");
    str_get(item, "node_id", out->node_id, sizeof(out->node_id), "");
    str_get(item, "location_type", out->location_type, sizeof(out->location_type), out->executable[0] ? "local" : "remote");
    str_get(item, "tier", out->tier, sizeof(out->tier), decision ? "decision" : "medium");
    out->ram_estimate_mb = (int)int_get(item, "ram_estimate_mb", 0);
    out->vram_estimate_mb = (int)int_get(item, "vram_estimate_mb", 0);
    out->port = (int)int_get(item, "port", 0);
    out->startup_timeout_seconds = (int)int_get(item, "startup_timeout_seconds", 120);
    out->shutdown_timeout_seconds = (int)int_get(item, "shutdown_timeout_seconds", 10);
    out->request_timeout_seconds = (int)int_get(item, "request_timeout_seconds", 120);
    out->first_token_timeout_seconds = (int)int_get(item, "first_token_timeout_seconds", out->request_timeout_seconds);
    out->stream_idle_timeout_seconds = (int)int_get(item, "stream_idle_timeout_seconds", out->request_timeout_seconds);
    out->total_timeout_seconds = (int)int_get(item, "total_timeout_seconds", 0);
    out->temperature = double_get(item, "temperature", 0.2);

    if (!valid_id(out->id)) { set_error(err, err_size, "model id must use letters, digits, underscore or hyphen"); return -1; }
    if (!out->name[0] || !out->base_url[0] || strlen(out->name) >= sizeof(out->name)) {
        set_error(err, err_size, "model name and base_url are required"); return -1;
    }
    if (strcmp(out->api_type, "openai_compatible") != 0 && strcmp(out->api_type, "systemone") != 0) {
        set_error(err, err_size, "model api_type must be openai_compatible or systemone"); return -1;
    }
    if (strcmp(out->model_role, "general") != 0 && strcmp(out->model_role, "multimodal") != 0 &&
        strcmp(out->model_role, "voice_interpreter") != 0) {
        set_error(err, err_size, "model_role must be general, multimodal or voice_interpreter"); return -1;
    }
    if (strcmp(out->runtime_adapter, "openai_compatible") != 0 &&
        strcmp(out->runtime_adapter, "llama_server_openai_compatible") != 0) {
        set_error(err, err_size, "runtime_adapter must be openai_compatible or llama_server_openai_compatible"); return -1;
    }
    static const char *const input_modalities[] = {"text", "audio", "image", "video"};
    static const char *const output_modalities[] = {"text", "audio"};
    if (!valid_modality_csv(out->input_modalities, input_modalities,
                            sizeof(input_modalities) / sizeof(input_modalities[0])) ||
        !valid_modality_csv(out->output_modalities, output_modalities,
                            sizeof(output_modalities) / sizeof(output_modalities[0]))) {
        set_error(err, err_size, "model input_modalities/output_modalities must be comma-separated supported modality names"); return -1;
    }
    if (strcmp(out->runtime_adapter, "llama_server_openai_compatible") == 0 &&
        (strcmp(out->model_role, "voice_interpreter") != 0 ||
         !strstr(out->input_modalities, "audio") || !strstr(out->output_modalities, "text"))) {
        set_error(err, err_size, "llama_server_openai_compatible requires voice_interpreter role with audio input and text output"); return -1;
    }
    if (out->ram_estimate_mb < 0 || out->ram_estimate_mb > 1048576 || out->vram_estimate_mb < 0 || out->vram_estimate_mb > 1048576) {
        set_error(err, err_size, "model RAM/VRAM estimates must be in 0..1048576 MB"); return -1;
    }
    if (out->port < 0 || out->port > 65535 || out->startup_timeout_seconds < 1 || out->startup_timeout_seconds > 900 ||
        out->shutdown_timeout_seconds < 1 || out->shutdown_timeout_seconds > 60 ||
        out->request_timeout_seconds < 1 || out->request_timeout_seconds > 900 ||
        out->first_token_timeout_seconds < 1 || out->first_token_timeout_seconds > 900 ||
        out->stream_idle_timeout_seconds < 1 || out->stream_idle_timeout_seconds > 900 ||
        out->total_timeout_seconds < 0 || out->total_timeout_seconds > 3600 ||
        out->temperature < 0.0 || out->temperature > 2.0) {
        set_error(err, err_size, "model launch timeout, port or temperature is outside allowed range"); return -1;
    }
    if ((out->auto_start || out->start_at_boot) && out->enabled && (out->port < 1 || !out->executable[0] || out->executable[0] != '/')) {
        set_error(err, err_size, "enabled auto_start model needs an absolute executable path"); return -1;
    }
    char resolved[AO_MAX_PATH];
    if (out->executable[0]) {
        if (resolve_root_path(resolved, sizeof(resolved), rt->config.root_dir, out->executable) != 0 ||
            string_copy(out->executable, sizeof(out->executable), resolved) != 0) {
            set_error(err, err_size, "model executable path is too long"); return -1;
        }
    }
    if (out->model_path[0]) {
        if (resolve_root_path(resolved, sizeof(resolved), rt->config.root_dir, out->model_path) != 0 ||
            string_copy(out->model_path, sizeof(out->model_path), resolved) != 0) {
            set_error(err, err_size, "model weights path is too long"); return -1;
        }
    }
    if (resolve_root_path(resolved, sizeof(resolved), rt->config.root_dir, out->working_directory) != 0 ||
        string_copy(out->working_directory, sizeof(out->working_directory), resolved) != 0) {
        set_error(err, err_size, "model working_directory is too long"); return -1;
    }
    json_object *args = obj_get(item, "args");
    if (args && json_object_get_type(args) != json_type_array) { set_error(err, err_size, "model args must be an array of strings"); return -1; }
    if (args) {
        size_t count = json_object_array_length(args);
        if (count > AO_MAX_MODEL_ARGS) { set_error(err, err_size, "too many model arguments"); return -1; }
        for (size_t i = 0; i < count; ++i) {
            json_object *v = json_object_array_get_idx(args, i);
            if (!v || json_object_get_type(v) != json_type_string || !json_object_get_string(v) ||
                strlen(json_object_get_string(v)) >= AO_MAX_PATH) {
                set_error(err, err_size, "model args must contain short strings"); return -1;
            }
            (void)snprintf(out->args[out->arg_count++], AO_MAX_PATH, "%s", json_object_get_string(v));
        }
    }
    if (out->enabled && !out->executable[0]) {
        /* Connecting to a separately managed inference server is supported. */
        if (!out->base_url[0]) { set_error(err, err_size, "enabled external model requires base_url"); return -1; }
    }
    if (out->enabled && (out->auto_start || out->start_at_boot) && out->model_path[0]) {
        struct stat model_st;
        if (stat(out->model_path, &model_st) != 0 || !(S_ISREG(model_st.st_mode) || S_ISDIR(model_st.st_mode))) {
            if (err && err_size) (void)snprintf(err, err_size, "model_path for '%s' does not exist or is not a regular file/directory", out->id);
            return -1;
        }
    }
    if (out->enabled && (out->auto_start || out->start_at_boot) && access(out->executable, X_OK) != 0) {
        if (err && err_size) (void)snprintf(err, err_size, "auto_start executable for model '%s' is not executable: %s", out->id, out->executable);
        return -1;
    }
    if (out->health_url[0] == '\0') {
        size_t n = strlen(out->base_url);
        const char *suffix = (n >= 3 && strcmp(out->base_url + n - 3, "/v1") == 0) ? "/models" : "/health";
        if (snprintf(out->health_url, sizeof(out->health_url), "%s%s", out->base_url, suffix) >= (int)sizeof(out->health_url)) {
            set_error(err, err_size, "model health_url is too long"); return -1;
        }
    }
    return 0;
}

int models_load(Runtime *rt, char *err, size_t err_size) {
    if (!rt) { set_error(err, err_size, "runtime is required"); return -1; }
    if (!rt->config.models_config_path[0]) return 0; /* Backward-compatible legacy configuration. */
    json_object *root = json_object_from_file(rt->config.models_config_path);
    if (!root || json_object_get_type(root) != json_type_object) {
        if (root) json_object_put(root);
        if (err && err_size) (void)snprintf(err, err_size, "cannot read model registry JSON: %s", rt->config.models_config_path);
        return -1;
    }
    json_object *defaults = obj_get(root, "defaults");
    str_get(defaults, "model_id", rt->config.default_model_id, sizeof(rt->config.default_model_id), rt->config.default_model_id);
    str_get(defaults, "decision_model_id", rt->config.decision_model_id, sizeof(rt->config.decision_model_id), rt->config.decision_model_id);
    str_get(defaults, "analysis_model_id", rt->config.analysis_model_id, sizeof(rt->config.analysis_model_id), rt->config.analysis_model_id);
    json_object *model_array = obj_get(root, "models");
    json_object *decision_array = obj_get(root, "decision_models");
    json_object *nodes_array = obj_get(root, "nodes");
    if (validate_nodes_json(nodes_array, err, err_size) != 0) { json_object_put(root); return -1; }
    if ((model_array && json_object_get_type(model_array) != json_type_array) ||
        (decision_array && json_object_get_type(decision_array) != json_type_array)) {
        json_object_put(root); set_error(err, err_size, "models and decision_models must be arrays"); return -1;
    }
    size_t counts[2] = {model_array ? json_object_array_length(model_array) : 0,
                        decision_array ? json_object_array_length(decision_array) : 0};
    if (counts[0] > AO_MAX_MODELS || counts[1] > AO_MAX_MODELS) {
        json_object_put(root); set_error(err, err_size, "model registry exceeds supported limit"); return -1;
    }
    for (int role = 0; role < 2; ++role) {
        json_object *array = role ? decision_array : model_array;
        for (size_t i = 0; i < counts[role]; ++i) {
            json_object *item = json_object_array_get_idx(array, i);
            if (!item || json_object_get_type(item) != json_type_object) {
                json_object_put(root); set_error(err, err_size, "every model entry must be an object"); return -1;
            }
            json_object *node_id_obj = obj_get(item, "node_id");
            if (node_id_obj && json_object_get_type(node_id_obj) == json_type_string && json_object_get_string(node_id_obj)[0]) {
                const char *node_id = json_object_get_string(node_id_obj);
                json_object *node = NULL;
                for (size_t ni = 0; nodes_array && ni < json_object_array_length(nodes_array); ++ni) {
                    json_object *candidate = json_object_array_get_idx(nodes_array, ni);
                    json_object *candidate_id = obj_get(candidate, "id");
                    if (candidate_id && json_object_get_type(candidate_id) == json_type_string && strcmp(json_object_get_string(candidate_id), node_id) == 0) { node = candidate; break; }
                }
                if (!node) { json_object_put(root); if (err && err_size) snprintf(err, err_size, "model refers to unknown node_id '%s'", node_id); return -1; }
                const char *node_type = "local";
                json_object *type_obj = obj_get(node, "type");
                if (type_obj && json_object_get_type(type_obj) == json_type_string) node_type = json_object_get_string(type_obj);
                if (strcmp(node_type, "local") && strcmp(node_type, "lan") && strcmp(node_type, "internet")) { json_object_put(root); set_error(err, err_size, "node type must be local, lan or internet"); return -1; }
                if (strcmp(node_type, "internet") == 0) {
                    json_object *endpoint = obj_get(node, "endpoint");
                    const char *url = endpoint && json_object_get_type(endpoint) == json_type_string ? json_object_get_string(endpoint) : "";
                    json_object *health_url = obj_get(node, "health_url");
                    const char *health = health_url && json_object_get_type(health_url) == json_type_string ? json_object_get_string(health_url) : "";
                    if (*url && !starts_with(url, "https://")) { json_object_put(root); if (err && err_size) snprintf(err, err_size, "internet node '%s' endpoint must use HTTPS", node_id); return -1; }
                    if (*health && !starts_with(health, "https://")) { json_object_put(root); if (err && err_size) snprintf(err, err_size, "internet node '%s' health_url must use HTTPS", node_id); return -1; }
                }
                json_object_object_add(item, "location_type", json_object_new_string(node_type));
                json_object *node_enabled_obj = obj_get(node, "enabled");
                if (node_enabled_obj && json_object_get_type(node_enabled_obj) == json_type_boolean && !json_object_get_boolean(node_enabled_obj))
                    json_object_object_add(item, "enabled", json_object_new_boolean(false));
                {
                    /* A local node may be a pre-existing localhost endpoint rather than a
                     * managed process. A non-empty endpoint therefore applies regardless
                     * of node type, and its protocol contract follows the same route. */
                    json_object *endpoint = obj_get(node, "endpoint");
                    if (endpoint && json_object_get_type(endpoint) == json_type_string && json_object_get_string(endpoint)[0])
                        json_object_object_add(item, "base_url", json_object_new_string(json_object_get_string(endpoint)));
                    json_object *health = obj_get(node, "health_url");
                    if (health && json_object_get_type(health) == json_type_string && json_object_get_string(health)[0])
                        json_object_object_add(item, "health_url", json_object_new_string(json_object_get_string(health)));
                    json_object *node_api_type = obj_get(node, "api_type");
                    if (node_api_type && json_object_get_type(node_api_type) == json_type_string && json_object_get_string(node_api_type)[0])
                        json_object_object_add(item, "api_type", json_object_new_string(json_object_get_string(node_api_type)));
                }
                {
                    /* A localhost endpoint can still require auth; use node credentials
                     * whenever explicitly configured, independent of where it runs. */
                    json_object *api_key_env = obj_get(node, "api_key_env");
                    const char *env_name = api_key_env && json_object_get_type(api_key_env) == json_type_string ? json_object_get_string(api_key_env) : "";
                    const char *env_value = env_name && *env_name ? getenv(env_name) : NULL;
                    json_object *api_key = obj_get(node, "api_key");
                    if (env_value && *env_value) json_object_object_add(item, "api_key", json_object_new_string(env_value));
                    else if (api_key && json_object_get_type(api_key) == json_type_string && json_object_get_string(api_key)[0])
                        json_object_object_add(item, "api_key", json_object_new_string(json_object_get_string(api_key)));
                }
            }
            LocalModel *dest = role ? &rt->decision_models[rt->decision_model_count] : &rt->models[rt->model_count];
            if (parse_model(rt, item, role != 0, dest, err, err_size) != 0) { json_object_put(root); return -1; }
            for (size_t m = 0; m < rt->model_count; ++m) if (strcmp(rt->models[m].id, dest->id) == 0) {
                json_object_put(root); if (err && err_size) snprintf(err, err_size, "duplicate model id '%s'", dest->id); return -1;
            }
            for (size_t m = 0; m < rt->decision_model_count; ++m) if (strcmp(rt->decision_models[m].id, dest->id) == 0) {
                json_object_put(root); if (err && err_size) snprintf(err, err_size, "duplicate decision model id '%s'", dest->id); return -1;
            }
            if (role) rt->decision_model_count++; else rt->model_count++;
        }
    }
    json_object_put(root);
    if (rt->config.default_model_id[0] && !model_find(rt, rt->config.default_model_id, false)) {
        if (err && err_size) (void)snprintf(err, err_size, "default model_id '%s' is not defined in models.json", rt->config.default_model_id);
        return -1;
    }
    if (rt->config.analysis_model_id[0] && !model_find(rt, rt->config.analysis_model_id, false)) {
        if (err && err_size) (void)snprintf(err, err_size, "analysis_model_id '%s' is not defined in models.json models", rt->config.analysis_model_id);
        return -1;
    }
    if (rt->config.decision_model_id[0] && !model_find(rt, rt->config.decision_model_id, true)) {
        if (err && err_size) (void)snprintf(err, err_size, "decision_model_id '%s' is not defined in models.json decision_models", rt->config.decision_model_id);
        return -1;
    }
    const LocalModel *default_model = rt->config.default_model_id[0] ? model_find(rt, rt->config.default_model_id, false) : NULL;
    if (default_model && strcmp(default_model->model_role, "voice_interpreter") == 0) {
        if (err && err_size) (void)snprintf(err, err_size, "voice_interpreter model '%s' is reserved for the Cowork voice gateway and cannot be the default project model", default_model->id);
        return -1;
    }
    const LocalModel *analysis_model = rt->config.analysis_model_id[0] ? model_find(rt, rt->config.analysis_model_id, false) : NULL;
    if (analysis_model && strcmp(analysis_model->model_role, "voice_interpreter") == 0) {
        if (err && err_size) (void)snprintf(err, err_size, "voice_interpreter model '%s' cannot be assigned to project analysis", analysis_model->id);
        return -1;
    }
    for (size_t i = 0; i < rt->agent_count; ++i) {
        Agent *a = &rt->agents[i];
        const LocalModel *agent_model = a->model_id[0] ? model_find(rt, a->model_id, false) : NULL;
        if (a->model_id[0] && !agent_model) {
            if (err && err_size) (void)snprintf(err, err_size, "agent '%s' refers to undefined model_id '%s'", a->id, a->model_id);
            return -1;
        }
        if (agent_model && strcmp(agent_model->model_role, "voice_interpreter") == 0) {
            if (err && err_size) (void)snprintf(err, err_size, "agent '%s' cannot use voice_interpreter model '%s'; it is reserved for report preparation", a->id, agent_model->id);
            return -1;
        }
    }
    return 0;
}

const LocalModel *model_find(const Runtime *rt, const char *id, bool decision) {
    if (!rt || !id || !*id) return NULL;
    const LocalModel *models = decision ? rt->decision_models : rt->models;
    size_t count = decision ? rt->decision_model_count : rt->model_count;
    for (size_t i = 0; i < count; ++i) if (strcmp(models[i].id, id) == 0) return &models[i];
    return NULL;
}

typedef struct {
    long total_mb;
    long available_mb;
    long cgroup_limit_mb;
    bool cgroup_limited;
} HostMemoryStats;

static bool read_unsigned_file(const char *path, uint64_t *value, bool allow_max) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char text[128];
    bool ok = false;
    if (fgets(text, sizeof(text), f)) {
        char *p = text;
        while (isspace((unsigned char)*p)) ++p;
        if (allow_max && strncmp(p, "max", 3) == 0) {
            *value = UINT64_MAX;
            ok = true;
        } else {
            char *end = NULL;
            errno = 0;
            unsigned long long parsed = strtoull(p, &end, 10);
            while (end && isspace((unsigned char)*end)) ++end;
            if (end && end != p && *end == '\0' && errno == 0) {
                *value = (uint64_t)parsed;
                ok = true;
            }
        }
    }
    fclose(f);
    return ok;
}

static HostMemoryStats system_memory_stats(void) {
    HostMemoryStats stats = {.total_mb = -1, .available_mb = -1, .cgroup_limit_mb = -1, .cgroup_limited = false};
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[256];
        long total_kb = -1, available_kb = -1, free_kb = -1, buffers_kb = 0, cached_kb = 0;
        while (fgets(line, sizeof(line), f)) {
            (void)sscanf(line, "MemTotal: %ld kB", &total_kb);
            (void)sscanf(line, "MemAvailable: %ld kB", &available_kb);
            (void)sscanf(line, "MemFree: %ld kB", &free_kb);
            (void)sscanf(line, "Buffers: %ld kB", &buffers_kb);
            (void)sscanf(line, "Cached: %ld kB", &cached_kb);
        }
        fclose(f);
        if (total_kb > 0) stats.total_mb = total_kb / 1024;
        if (available_kb > 0) stats.available_mb = available_kb / 1024;
        else if (free_kb >= 0) stats.available_mb = (free_kb + buffers_kb + cached_kb) / 1024;
    }

    /* Host MemAvailable is misleading inside a memory-limited container. Respect cgroup v2 first. */
    uint64_t limit = UINT64_MAX, current = 0;
    bool have_limit = read_unsigned_file("/sys/fs/cgroup/memory.max", &limit, true) && limit != UINT64_MAX;
    bool have_current = read_unsigned_file("/sys/fs/cgroup/memory.current", &current, false);
    if (!have_limit || !have_current) {
        uint64_t legacy_limit = UINT64_MAX, legacy_usage = 0;
        have_limit = read_unsigned_file("/sys/fs/cgroup/memory/memory.limit_in_bytes", &legacy_limit, false) &&
                     legacy_limit < UINT64_C(1) << 60;
        have_current = read_unsigned_file("/sys/fs/cgroup/memory/memory.usage_in_bytes", &legacy_usage, false);
        if (have_limit && have_current) { limit = legacy_limit; current = legacy_usage; }
    }
    if (have_limit && have_current && limit > 0) {
        long limit_mb = (long)(limit / (1024U * 1024U));
        long avail_mb = current >= limit ? 0 : (long)((limit - current) / (1024U * 1024U));
        stats.cgroup_limited = true;
        stats.cgroup_limit_mb = limit_mb;
        if (stats.total_mb < 0 || limit_mb < stats.total_mb) stats.total_mb = limit_mb;
        if (stats.available_mb < 0 || avail_mb < stats.available_mb) stats.available_mb = avail_mb;
    }
    return stats;
}

static long system_mem_available_mb(void) {
    return system_memory_stats().available_mb;
}

typedef struct {
    long free_mb;
    long total_mb;
    long used_mb;
    bool shared_system_ram;
    const char *source;
} GPUStats;

static GPUStats system_gpu_stats(void) {
    GPUStats stats = {.free_mb = -1, .total_mb = -1, .used_mb = -1, .shared_system_ram = false, .source = "unavailable"};
    /* NVIDIA reports separately addressable device memory; query free and total in one call. */
    FILE *pipe = popen("nvidia-smi --query-gpu=memory.free,memory.total --format=csv,noheader,nounits 2>/dev/null", "r");
    if (pipe) {
        char line[160];
        if (fgets(line, sizeof(line), pipe)) {
            char *comma = strchr(line, ',');
            if (comma) {
                *comma++ = '\0';
                char *end_free = NULL, *end_total = NULL;
                errno = 0;
                long free_mb = strtol(line, &end_free, 10);
                while (comma && isspace((unsigned char)*comma)) ++comma;
                long total_mb = strtol(comma, &end_total, 10);
                if (!errno && end_free != line && end_total != comma && free_mb >= 0 && total_mb > 0) {
                    stats.free_mb = free_mb;
                    stats.total_mb = total_mb;
                    stats.used_mb = total_mb - free_mb;
                    stats.source = "nvidia-smi";
                    (void)pclose(pipe);
                    return stats;
                }
            }
        }
        (void)pclose(pipe);
    }

    /* DRM counters are useful for AMD GPUs. Small UMA apertures are not a separate
       pool: they borrow system RAM, so account for all weights/KV against MemAvailable. */
    DIR *drm = opendir("/sys/class/drm");
    bool has_dri = false;
    if (drm) {
        struct dirent *entry;
        while ((entry = readdir(drm)) != NULL) {
            if (strncmp(entry->d_name, "card", 4) != 0 || !isdigit((unsigned char)entry->d_name[4])) continue;
            has_dri = true;
            char path_total[512], path_used[512];
            (void)snprintf(path_total, sizeof(path_total), "/sys/class/drm/%s/device/mem_info_vram_total", entry->d_name);
            (void)snprintf(path_used, sizeof(path_used), "/sys/class/drm/%s/device/mem_info_vram_used", entry->d_name);
            uint64_t total = 0, used = 0;
            if (read_unsigned_file(path_total, &total, false) && read_unsigned_file(path_used, &used, false) && total > used) {
                long total_mb = (long)(total / (1024U * 1024U));
                stats.total_mb = total_mb;
                stats.used_mb = (long)(used / (1024U * 1024U));
                stats.free_mb = (long)((total - used) / (1024U * 1024U));
                stats.shared_system_ram = total_mb <= 2048;
                stats.source = stats.shared_system_ram ? "drm-uma" : "drm-sysfs";
                closedir(drm);
                return stats;
            }
        }
        closedir(drm);
    }
    struct stat st;
    if (has_dri || (stat("/dev/dri", &st) == 0 && S_ISDIR(st.st_mode))) {
        stats.shared_system_ram = true;
        stats.source = "drm-shared-system-ram";
    }
    return stats;
}

static long system_vram_free_mb(void) {
    return system_gpu_stats().free_mb;
}

static const char *memory_gpu_mode(GPUStats stats) {
    if (stats.shared_system_ram) return "shared_system_ram";
    if (stats.free_mb >= 0) return "dedicated_vram";
    return "unavailable";
}

typedef struct {
    pid_t pid;
    long memory_mb;
} GPUProcessMemory;

static size_t nvidia_gpu_process_snapshot(GPUProcessMemory *items, size_t capacity,
                                          bool *available) {
    if (available) *available = false;
    if (!items || capacity == 0) return 0;
    FILE *pipe = popen("nvidia-smi --query-compute-apps=pid,used_memory --format=csv,noheader,nounits 2>/dev/null", "r");
    if (!pipe) return 0;
    char line[256];
    size_t count = 0;
    while (fgets(line, sizeof(line), pipe)) {
        char *comma = strchr(line, ',');
        if (!comma) continue;
        *comma++ = '\0';
        char *pid_end = NULL, *memory_end = NULL;
        errno = 0;
        long pid_long = strtol(line, &pid_end, 10);
        while (isspace((unsigned char)*comma)) ++comma;
        long memory_mb = strtol(comma, &memory_end, 10);
        if (errno || pid_end == line || !pid_end || *pid_end || memory_end == comma ||
            pid_long <= 0 || pid_long > INT_MAX || memory_mb < 0) continue;
        size_t i = 0;
        while (i < count && items[i].pid != (pid_t)pid_long) ++i;
        if (i < count) {
            if (LONG_MAX - items[i].memory_mb >= memory_mb) items[i].memory_mb += memory_mb;
        } else if (count < capacity) {
            items[count].pid = (pid_t)pid_long;
            items[count].memory_mb = memory_mb;
            ++count;
        }
    }
    int status = pclose(pipe);
    if (available) *available = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    return count;
}

static long gpu_process_memory_lookup(const GPUProcessMemory *items, size_t count,
                                      bool snapshot_available, pid_t pid) {
    if (!snapshot_available) return -1;
    for (size_t i = 0; i < count; ++i) if (items[i].pid == pid) return items[i].memory_mb;
    return 0;
}

static void process_memory_mb(pid_t pid, long *rss_mb, long *pss_mb) {
    if (rss_mb) *rss_mb = 0;
    if (pss_mb) *pss_mb = 0;
    if (pid <= 0) return;
    char path[128];
    (void)snprintf(path, sizeof(path), "/proc/%ld/smaps_rollup", (long)pid);
    FILE *f = fopen(path, "r");
    long rss_kb = -1, pss_kb = -1;
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            long value = -1;
            if (sscanf(line, "Rss: %ld kB", &value) == 1) rss_kb = value;
            if (sscanf(line, "Pss: %ld kB", &value) == 1) pss_kb = value;
        }
        fclose(f);
    }
    if (rss_kb < 0 || pss_kb < 0) {
        (void)snprintf(path, sizeof(path), "/proc/%ld/status", (long)pid);
        f = fopen(path, "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                long value = -1;
                if (sscanf(line, "VmRSS: %ld kB", &value) == 1) rss_kb = value;
            }
            fclose(f);
        }
        if (pss_kb < 0) pss_kb = rss_kb;
    }
    if (rss_mb && rss_kb >= 0) *rss_mb = rss_kb / 1024;
    if (pss_mb && pss_kb >= 0) *pss_mb = pss_kb / 1024;
}

static bool pid_is_in_list(pid_t pid, const pid_t *pids, size_t count) {
    for (size_t i = 0; i < count; ++i) if (pids[i] == pid) return true;
    return false;
}

static const char *inference_process_label(const char *cmdline) {
    static const struct { const char *marker; const char *label; } known[] = {
        {"llama-server", "llama-server"}, {"llama.cpp", "llama.cpp"}, {"llamafile", "llamafile"},
        {"koboldcpp", "koboldcpp"}, {"ollama", "ollama"}, {"vllm", "vllm"},
        {"text-generation-launcher", "text-generation-inference"}, {"text-generation-inference", "text-generation-inference"},
        {"sglang", "sglang"}, {"lmstudio", "lm-studio"}, {"mlx_lm", "mlx-lm"},
        {"aphrodite", "aphrodite"}, {"exllamav2", "exllamav2"}, {"tabbyapi", "tabbyapi"},
        {"local-ai", "local-ai"}, {"localai", "local-ai"}
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); ++i)
        if (strstr(cmdline, known[i].marker)) return known[i].label;
    return NULL;
}

static void scan_external_inference_processes(json_object *running, pid_t *tracked_pids,
                                              size_t tracked_count, long *total_pss_mb,
                                              const GPUProcessMemory *gpu_processes,
                                              size_t gpu_process_count, bool gpu_process_snapshot_available) {
    DIR *proc = opendir("/proc");
    if (!proc) return;
    struct dirent *entry;
    while ((entry = readdir(proc)) != NULL) {
        if (!isdigit((unsigned char)entry->d_name[0])) continue;
        char *end = NULL;
        long parsed = strtol(entry->d_name, &end, 10);
        if (!end || *end || parsed <= 1 || parsed > INT_MAX) continue;
        pid_t pid = (pid_t)parsed;
        if (pid_is_in_list(pid, tracked_pids, tracked_count)) continue;
        char path[128];
        (void)snprintf(path, sizeof(path), "/proc/%ld/cmdline", parsed);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        char command[4096];
        size_t got = fread(command, 1, sizeof(command) - 1, f);
        fclose(f);
        if (!got) continue;
        for (size_t i = 0; i < got; ++i) {
            unsigned char c = (unsigned char)command[i];
            if (c == '\0') command[i] = ' ';
            else command[i] = (char)tolower(c);
        }
        command[got] = '\0';
        const char *label = inference_process_label(command);
        if (!label) continue;
        long rss_mb = 0, pss_mb = 0;
        process_memory_mb(pid, &rss_mb, &pss_mb);
        if (pss_mb <= 0) pss_mb = rss_mb;
        if (total_pss_mb && pss_mb > 0 && LONG_MAX - *total_pss_mb >= pss_mb) *total_pss_mb += pss_mb;
        json_object *item = json_object_new_object();
        if (!item) continue;
        char id[AO_MAX_ID];
        (void)snprintf(id, sizeof(id), "external:%s:%ld", label, parsed);
        json_object_object_add(item, "id", json_object_new_string(id));
        json_object_object_add(item, "name", json_object_new_string(label));
        json_object_object_add(item, "kind", json_object_new_string("external_inference"));
        json_object_object_add(item, "tier", json_object_new_string("unknown"));
        json_object_object_add(item, "pid", json_object_new_int64(parsed));
        json_object_object_add(item, "actual_rss_mb", json_object_new_int64(rss_mb));
        json_object_object_add(item, "actual_pss_mb", json_object_new_int64(pss_mb));
        json_object_object_add(item, "actual_gpu_memory_mb", json_object_new_int64(
            gpu_process_memory_lookup(gpu_processes, gpu_process_count, gpu_process_snapshot_available, pid)));
        json_object_object_add(item, "source", json_object_new_string("/proc process scan"));
        json_object_object_add(item, "disk_size_bytes", json_object_new_int64(0));
        json_object_object_add(item, "disk_size_mb", json_object_new_int(0));
        json_object_object_add(item, "loaded_context_tokens", json_object_new_int(0));
        json_object_object_add(item, "loaded_parallel", json_object_new_int(0));
        json_object_object_add(item, "active_requests", json_object_new_int(0));
        json_object_array_add(running, item);
    }
    closedir(proc);
}

static uint64_t disk_size_walk(const char *path, unsigned depth) {
    if (!path || depth > 32) return 0;
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISREG(st.st_mode)) return st.st_size > 0 ? (uint64_t)st.st_size : 0;
    if (!S_ISDIR(st.st_mode)) return 0; /* Do not follow symlinks. */
    DIR *dir = opendir(path);
    if (!dir) return 0;
    uint64_t total = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[AO_MAX_PATH];
        int n = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(child)) continue;
        uint64_t size = disk_size_walk(child, depth + 1);
        if (UINT64_MAX - total < size) { total = UINT64_MAX; break; }
        total += size;
    }
    closedir(dir);
    return total;
}

static uint64_t model_disk_size_bytes(LocalModel *model) {
    if (!model || !model->model_path[0]) return 0;
    struct stat st;
    if (stat(model->model_path, &st) != 0) return model->disk_size_bytes_cached;
    int64_t mtime = (int64_t)st.st_mtime;
    int64_t now = (int64_t)time(NULL);
    if (model->disk_cache_checked_at && now - model->disk_cache_checked_at < 300 &&
        model->disk_cache_mtime == mtime) return model->disk_size_bytes_cached;
    uint64_t bytes = disk_size_walk(model->model_path, 0);
    model->disk_size_bytes_cached = bytes;
    model->disk_cache_mtime = mtime;
    model->disk_cache_checked_at = now;
    return bytes;
}

static long model_weight_size_mb(LocalModel *model);

/* GGUF metadata lets us estimate KV memory from layers, KV heads and head size
 * rather than assuming every model has the same cache-to-weights ratio. Values
 * are cached on LocalModel and refreshed periodically. */
static bool file_skip(FILE *f, uint64_t bytes) {
    unsigned char scratch[4096];
    while (bytes) {
        size_t chunk = bytes > sizeof(scratch) ? sizeof(scratch) : (size_t)bytes;
        if (fread(scratch, 1, chunk, f) != chunk) return false;
        bytes -= chunk;
    }
    return true;
}

static bool gguf_u32(FILE *f, uint32_t *value) {
    unsigned char b[4];
    if (fread(b, 1, sizeof(b), f) != sizeof(b)) return false;
    *value = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return true;
}

static bool gguf_u64(FILE *f, uint64_t *value) {
    unsigned char b[8];
    if (fread(b, 1, sizeof(b), f) != sizeof(b)) return false;
    *value = (uint64_t)b[0] | ((uint64_t)b[1] << 8) | ((uint64_t)b[2] << 16) | ((uint64_t)b[3] << 24) |
             ((uint64_t)b[4] << 32) | ((uint64_t)b[5] << 40) | ((uint64_t)b[6] << 48) | ((uint64_t)b[7] << 56);
    return true;
}

static bool gguf_string(FILE *f, char *dst, size_t cap) {
    uint64_t length = 0;
    if (!gguf_u64(f, &length) || length > UINT64_C(64) * 1024 * 1024) return false;
    size_t copy_length = cap ? (length < cap - 1 ? (size_t)length : cap - 1) : 0;
    if (copy_length && fread(dst, 1, copy_length, f) != copy_length) return false;
    if (cap) dst[copy_length] = '\0';
    return file_skip(f, length - copy_length);
}

static bool gguf_skip_value(FILE *f, uint32_t type, unsigned depth) {
    uint32_t subtype = 0;
    uint64_t count = 0;
    switch (type) {
        case 0: case 1: case 7: return file_skip(f, 1);
        case 2: case 3: return file_skip(f, 2);
        case 4: case 5: case 6: return file_skip(f, 4);
        case 10: case 11: case 12: return file_skip(f, 8);
        case 8: {
            uint64_t length = 0;
            return gguf_u64(f, &length) && length <= UINT64_C(64) * 1024 * 1024 && file_skip(f, length);
        }
        case 9:
            if (depth > 1 || !gguf_u32(f, &subtype) || !gguf_u64(f, &count) || count > UINT64_C(10000000)) return false;
            for (uint64_t i = 0; i < count; ++i) if (!gguf_skip_value(f, subtype, depth + 1)) return false;
            return true;
        default: return false;
    }
}

static bool gguf_integer_value(FILE *f, uint32_t type, uint64_t *value) {
    unsigned char b[8];
    size_t n = 0;
    switch (type) {
        case 0: case 1: case 7: n = 1; break;
        case 2: case 3: n = 2; break;
        case 4: case 5: n = 4; break;
        case 10: case 11: n = 8; break;
        default: return false;
    }
    if (fread(b, 1, n, f) != n) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v |= (uint64_t)b[i] << (8U * i);
    *value = v;
    return true;
}

static bool key_ends_with(const char *key, const char *suffix) {
    size_t key_len = strlen(key), suffix_len = strlen(suffix);
    return key_len > suffix_len && key[key_len - suffix_len - 1] == '.' &&
           strcmp(key + key_len - suffix_len, suffix) == 0;
}

static bool gguf_read_kv_bytes_per_token(FILE *f, uint64_t *bytes_per_token) {
    char magic[4];
    uint32_t version = 0;
    uint64_t tensors = 0, kv_count = 0;
    if (fread(magic, 1, sizeof(magic), f) != sizeof(magic) || memcmp(magic, "GGUF", 4) != 0 ||
        !gguf_u32(f, &version) || version < 2 || version > 3 ||
        !gguf_u64(f, &tensors) || !gguf_u64(f, &kv_count) || kv_count > UINT64_C(1000000)) return false;
    (void)tensors;
    char architecture[96] = "", field_prefix[96] = "";
    uint64_t layers = 0, embedding = 0, heads = 0, kv_heads = 0, key_length = 0, value_length = 0;
    for (uint64_t i = 0; i < kv_count; ++i) {
        char key[256] = "";
        uint32_t type = 0;
        if (!gguf_string(f, key, sizeof(key)) || !gguf_u32(f, &type)) return false;
        if (strcmp(key, "general.architecture") == 0 && type == 8) {
            if (!gguf_string(f, architecture, sizeof(architecture))) return false;
            continue;
        }
        const char *suffix = NULL;
        enum { FIELD_NONE, FIELD_LAYERS, FIELD_EMBED, FIELD_HEADS, FIELD_KV_HEADS, FIELD_KEY_LEN, FIELD_VALUE_LEN } field = FIELD_NONE;
        if (key_ends_with(key, "block_count")) { suffix = "block_count"; field = FIELD_LAYERS; }
        else if (key_ends_with(key, "embedding_length")) { suffix = "embedding_length"; field = FIELD_EMBED; }
        else if (key_ends_with(key, "attention.head_count_kv")) { suffix = "attention.head_count_kv"; field = FIELD_KV_HEADS; }
        else if (key_ends_with(key, "attention.head_count")) { suffix = "attention.head_count"; field = FIELD_HEADS; }
        else if (key_ends_with(key, "attention.key_length")) { suffix = "attention.key_length"; field = FIELD_KEY_LEN; }
        else if (key_ends_with(key, "attention.value_length")) { suffix = "attention.value_length"; field = FIELD_VALUE_LEN; }
        if (!suffix) {
            if (!gguf_skip_value(f, type, 0)) return false;
            continue;
        }
        size_t prefix_len = strlen(key) - strlen(suffix) - 1;
        if (prefix_len == 0 || prefix_len >= sizeof(field_prefix)) {
            if (!gguf_skip_value(f, type, 0)) return false;
            continue;
        }
        char prefix[96]; memcpy(prefix, key, prefix_len); prefix[prefix_len] = '\0';
        if (!field_prefix[0]) (void)snprintf(field_prefix, sizeof(field_prefix), "%s", prefix);
        if (strcmp(field_prefix, prefix) != 0) {
            if (!gguf_skip_value(f, type, 0)) return false;
            continue;
        }
        uint64_t value = 0;
        if (gguf_integer_value(f, type, &value)) {
            switch (field) {
                case FIELD_LAYERS: layers = value; break;
                case FIELD_EMBED: embedding = value; break;
                case FIELD_HEADS: heads = value; break;
                case FIELD_KV_HEADS: kv_heads = value; break;
                case FIELD_KEY_LEN: key_length = value; break;
                case FIELD_VALUE_LEN: value_length = value; break;
                default: break;
            }
        } else if (!gguf_skip_value(f, type, 0)) return false;
    }
    if (!architecture[0] || !field_prefix[0] || strcmp(architecture, field_prefix) != 0 ||
        layers == 0 || layers > 1000 || embedding == 0 || embedding > 65536 || heads == 0 || heads > 1024) return false;
    if (kv_heads == 0) kv_heads = heads;
    if (kv_heads > heads || kv_heads > 1024) return false;
    uint64_t key_dim = key_length ? key_length : embedding / heads;
    uint64_t value_dim = value_length ? value_length : key_dim;
    if (key_dim == 0 || value_dim == 0 || key_dim > 65536 || value_dim > 65536) return false;
    /* Default llama.cpp K/V cache type is F16: two bytes for K and two for V. */
    if (layers > UINT64_MAX / kv_heads || layers * kv_heads > UINT64_MAX / key_dim / 4U) return false;
    uint64_t key_bytes = layers * kv_heads * key_dim * 2U;
    if (layers > UINT64_MAX / kv_heads || layers * kv_heads > UINT64_MAX / value_dim / 2U) return false;
    uint64_t value_bytes = layers * kv_heads * value_dim * 2U;
    if (UINT64_MAX - key_bytes < value_bytes) return false;
    *bytes_per_token = key_bytes + value_bytes;
    return *bytes_per_token > 0 && *bytes_per_token <= UINT64_C(64) * 1024 * 1024;
}

static bool find_gguf_path(const char *path, unsigned depth, char *found, size_t found_cap) {
    if (!path || depth > 8) return false;
    struct stat st;
    if (lstat(path, &st) != 0) return false;
    if (S_ISREG(st.st_mode)) {
        size_t n = strlen(path);
        if (n >= 5 && strcasecmp(path + n - 5, ".gguf") == 0) return string_copy(found, found_cap, path) == 0;
        return false;
    }
    if (!S_ISDIR(st.st_mode)) return false;
    DIR *dir = opendir(path);
    if (!dir) return false;
    bool ok = false;
    struct dirent *entry;
    while (!ok && (entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' && (entry->d_name[1] == '\0' ||
            (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) continue;
        char child[AO_MAX_PATH];
        int n = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (n > 0 && (size_t)n < sizeof(child)) ok = find_gguf_path(child, depth + 1, found, found_cap);
    }
    closedir(dir);
    return ok;
}

static uint64_t model_kv_bytes_per_token(LocalModel *model) {
    if (!model) return UINT64_C(16384);
    int64_t now = (int64_t)time(NULL);
    if (model->kv_metadata_checked_at && now - model->kv_metadata_checked_at < 300 &&
        model->kv_bytes_per_token_cached > 0) return model->kv_bytes_per_token_cached;
    uint64_t bytes = 0;
    char gguf_path[AO_MAX_PATH];
    if (find_gguf_path(model->model_path, 0, gguf_path, sizeof(gguf_path))) {
        FILE *f = fopen(gguf_path, "rb");
        if (f) {
            (void)gguf_read_kv_bytes_per_token(f, &bytes);
            fclose(f);
        }
    }
    if (bytes == 0) {
        long weights_mb = model_weight_size_mb(model);
        if (weights_mb <= 0) weights_mb = 1024;
        uint64_t fallback = (uint64_t)weights_mb * 16U;
        if (fallback < UINT64_C(16384)) fallback = UINT64_C(16384);
        if (fallback > UINT64_C(524288)) fallback = UINT64_C(524288);
        bytes = fallback;
    }
    model->kv_bytes_per_token_cached = bytes;
    model->kv_metadata_checked_at = now;
    return bytes;
}

static long memory_reserve_mb(long total_mb) {
    if (total_mb <= 0) return 768;
    long reserve = total_mb / 8; /* Keep approximately 12.5% for the OS and filesystem cache. */
    if (reserve < 768) reserve = 768;
    if (reserve > 4096) reserve = 4096;
    if (reserve > total_mb / 3) reserve = total_mb / 3;
    return reserve;
}

static int tier_parallel_cap(const LocalModel *m) {
    const char *tier = m ? m->tier : "medium";
    if (strcasecmp(tier, "high") == 0 || strcasecmp(tier, "large") == 0 ||
        strcasecmp(tier, "analysis") == 0 || strcasecmp(tier, "decision") == 0) return 1;
    if (strcasecmp(tier, "medium") == 0) return 2;
    if (strcasecmp(tier, "fast") == 0) return 3;
    if (strcasecmp(tier, "ultra-fast") == 0 || strcasecmp(tier, "ultrafast") == 0 ||
        strcasecmp(tier, "ultra_fast") == 0) {
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);
        if (cpus < 4) return 4;
        if (cpus > 8) cpus = 8;
        return (int)cpus;
    }
    return 2;
}

json_object *models_resource_snapshot_json(Runtime *rt) {
    json_object *out = json_object_new_object();
    if (!out) return NULL;
    HostMemoryStats memory = system_memory_stats();
    long ram = memory.available_mb;
    GPUStats gpu = system_gpu_stats();
    GPUProcessMemory gpu_processes[AO_MAX_MODELS * 2 + 128];
    bool gpu_process_snapshot_available = false;
    size_t gpu_process_count = nvidia_gpu_process_snapshot(gpu_processes,
        sizeof(gpu_processes) / sizeof(gpu_processes[0]), &gpu_process_snapshot_available);
    long vram = gpu.free_mb;
    long total_model_pss = 0;
    long gpu_process_memory_total = 0;
    pid_t tracked_pids[AO_MAX_MODELS * 2];
    size_t tracked_count = 0;
    json_object_object_add(out, "ram_total_mb", json_object_new_int64(memory.total_mb));
    json_object_object_add(out, "ram_available_mb", json_object_new_int64(ram));
    json_object_object_add(out, "ram_without_models_estimated_mb", json_object_new_int64(ram));
    json_object_object_add(out, "os_reserve_mb", json_object_new_int64(memory_reserve_mb(memory.total_mb)));
    json_object_object_add(out, "cgroup_limited", json_object_new_boolean(memory.cgroup_limited));
    json_object_object_add(out, "cgroup_limit_mb", json_object_new_int64(memory.cgroup_limit_mb));
    json_object_object_add(out, "vram_free_mb", json_object_new_int64(vram));
    json_object_object_add(out, "gpu_memory_mode", json_object_new_string(memory_gpu_mode(gpu)));
    json_object_object_add(out, "vram_total_mb", json_object_new_int64(gpu.total_mb));
    json_object_object_add(out, "vram_used_mb", json_object_new_int64(gpu.used_mb));
    json_object_object_add(out, "shared_gpu_memory_used_mb", json_object_new_int64(gpu.shared_system_ram ? gpu.used_mb : 0));
    json_object_object_add(out, "vram_monitor", json_object_new_string(gpu.source));
    json_object_object_add(out, "memory_planner", json_object_new_string("disk-weights + estimated KV cache + live PSS, cgroup-aware; shared GPUs use system RAM pool"));
    json_object *running = json_object_new_array();
    if (!running) { json_object_put(out); return NULL; }
    if (rt) {
        pthread_mutex_lock(&rt->models_mutex);
        for (int role = 0; role < 2; ++role) {
            LocalModel *models = role ? rt->decision_models : rt->models;
            size_t count = role ? rt->decision_model_count : rt->model_count;
            for (size_t i = 0; i < count; ++i) {
                LocalModel *m = &models[i];
                uint64_t disk_bytes = model_disk_size_bytes(m);
                bool is_running = m->running && m->pid > 0 && kill(m->pid, 0) == 0;
                long rss_mb = 0, pss_mb = 0;
                if (is_running) {
                    if (tracked_count < sizeof(tracked_pids) / sizeof(tracked_pids[0])) tracked_pids[tracked_count++] = m->pid;
                    process_memory_mb(m->pid, &rss_mb, &pss_mb);
                    m->actual_rss_mb = rss_mb;
                    m->actual_pss_mb = pss_mb;
                    m->actual_gpu_memory_mb = gpu_process_memory_lookup(gpu_processes, gpu_process_count,
                        gpu_process_snapshot_available, m->pid);
                    total_model_pss += pss_mb;
                } else {
                    m->actual_rss_mb = 0;
                    m->actual_pss_mb = 0;
                    m->actual_gpu_memory_mb = 0;
                }
                if (!is_running) continue;
                json_object *item = json_object_new_object();
                if (!item) continue;
                json_object_object_add(item, "id", json_object_new_string(m->id));
                json_object_object_add(item, "kind", json_object_new_string(role ? "decision" : "llm"));
                json_object_object_add(item, "tier", json_object_new_string(m->tier));
                json_object_object_add(item, "pid", json_object_new_int64(m->pid));
                json_object_object_add(item, "actual_rss_mb", json_object_new_int64(rss_mb));
                json_object_object_add(item, "actual_pss_mb", json_object_new_int64(pss_mb));
                json_object_object_add(item, "actual_gpu_memory_mb", json_object_new_int64(m->actual_gpu_memory_mb));
                json_object_object_add(item, "disk_size_bytes", json_object_new_int64(disk_bytes > INT64_MAX ? INT64_MAX : (int64_t)disk_bytes));
        json_object_object_add(item, "disk_size_mb", json_object_new_int64((int64_t)((disk_bytes + UINT64_C(1048575)) / UINT64_C(1048576))));
                json_object_object_add(item, "ram_estimate_mb", json_object_new_int(m->ram_estimate_mb));
                json_object_object_add(item, "vram_estimate_mb", json_object_new_int(m->vram_estimate_mb));
                json_object_object_add(item, "loaded_context_tokens", json_object_new_int(m->loaded_context_tokens));
                json_object_object_add(item, "loaded_parallel", json_object_new_int(m->loaded_parallel));
                json_object_object_add(item, "active_requests", json_object_new_int(m->active_requests));
                json_object_array_add(running, item);
            }
        }
        pthread_mutex_unlock(&rt->models_mutex);
    }
    scan_external_inference_processes(running, tracked_pids, tracked_count, &total_model_pss,
        gpu_processes, gpu_process_count, gpu_process_snapshot_available);
    if (gpu_process_snapshot_available) {
        for (size_t i = 0; i < gpu_process_count; ++i) {
            if (gpu_processes[i].memory_mb > 0 && LONG_MAX - gpu_process_memory_total >= gpu_processes[i].memory_mb)
                gpu_process_memory_total += gpu_processes[i].memory_mb;
        }
    }
    long baseline = ram < 0 ? ram : ram + total_model_pss;
    if (memory.total_mb > 0 && baseline > memory.total_mb) baseline = memory.total_mb;
    json_object_object_add(out, "ram_without_models_estimated_mb", json_object_new_int64(baseline));
    json_object_object_add(out, "running_model_pss_total_mb", json_object_new_int64(total_model_pss));
    long vram_baseline = -1;
    if (!gpu.shared_system_ram && gpu.total_mb > 0 && gpu.free_mb >= 0) {
        vram_baseline = gpu.free_mb + gpu_process_memory_total;
        if (vram_baseline > gpu.total_mb) vram_baseline = gpu.total_mb;
    }
    json_object_object_add(out, "vram_without_models_estimated_mb", json_object_new_int64(vram_baseline));
    json_object_object_add(out, "vram_without_models_estimated_method", json_object_new_string(
        gpu.shared_system_ram ? "not applicable: GPU shares system RAM" :
        (gpu_process_snapshot_available ? "free VRAM plus nvidia-smi compute process memory, capped at total VRAM" : "unavailable: per-process GPU memory monitor is unavailable")));
    json_object_object_add(out, "ram_without_models_estimated_method", json_object_new_string(
        "MemAvailable plus PSS for registered and recognized external inference processes; shared GPU use is separate to avoid double counting"));
    json_object_object_add(out, "running_models", running);
    return out;
}

const char *models_choose_analysis_model(Runtime *rt, char *out, size_t out_size) {
    if (!rt || !out || !out_size) return NULL;
    out[0] = '\0';
    if (rt->config.analysis_model_id[0]) {
        const LocalModel *explicit_model = model_find(rt, rt->config.analysis_model_id, false);
        if (explicit_model && explicit_model->enabled && strcmp(explicit_model->model_role, "voice_interpreter") != 0) {
            if (string_copy(out, out_size, explicit_model->id) == 0) return out;
        }
    }
    /* Analysis is intentionally assigned to large-tier models, not fast/medium agents. */
    const LocalModel *best = NULL;
    for (size_t i = 0; i < rt->model_count; ++i) {
        const LocalModel *m = &rt->models[i];
        if (!m->enabled || strcmp(m->model_role, "voice_interpreter") == 0 || strcmp(m->tier, "large") != 0) continue;
        if (!best || m->ram_estimate_mb > best->ram_estimate_mb) best = m;
    }
    if (best && string_copy(out, out_size, best->id) == 0) return out;
    /* Backward-compatibility only: legacy local_llm can handle analysis when no registry model exists. */
    if (rt->model_count == 0 && rt->config.llm_enabled) return "";
    return NULL;
}

const char *models_choose_task_model(Runtime *rt, const char *tier, char *out, size_t out_size) {
    if (!rt || !out || !out_size) return NULL;
    out[0] = '\0';
    const char *requested = tier && *tier ? tier : "fast";
    const char *fast_order[] = {"fast", "ultra-fast", "medium", "small", "large", "high"};
    const char *ultra_order[] = {"ultra-fast", "fast", "medium", "small", "large", "high"};
    const char *medium_order[] = {"medium", "fast", "ultra-fast", "small", "large", "high"};
    const char *high_order[] = {"large", "high", "medium", "fast", "ultra-fast", "small"};
    const char *small_order[] = {"small", "fast", "ultra-fast", "medium", "large", "high"};
    const char **order = fast_order;
    size_t order_count = sizeof(fast_order) / sizeof(fast_order[0]);
    if (strcmp(requested, "ultra-fast") == 0) { order = ultra_order; order_count = sizeof(ultra_order) / sizeof(ultra_order[0]); }
    else if (strcmp(requested, "medium") == 0) { order = medium_order; order_count = sizeof(medium_order) / sizeof(medium_order[0]); }
    else if (strcmp(requested, "high") == 0 || strcmp(requested, "large") == 0) { order = high_order; order_count = sizeof(high_order) / sizeof(high_order[0]); }
    else if (strcmp(requested, "small") == 0) { order = small_order; order_count = sizeof(small_order) / sizeof(small_order[0]); }

    for (size_t tier_index = 0; tier_index < order_count; ++tier_index) {
        const LocalModel *best = NULL;
        for (size_t i = 0; i < rt->model_count; ++i) {
            const LocalModel *model = &rt->models[i];
            if (!model->enabled || strcmp(model->model_role, "voice_interpreter") == 0 || strcmp(model->tier, order[tier_index]) != 0) continue;
            /* Within one tier prefer the smaller configured footprint for routine work. */
            if (!best || (model->ram_estimate_mb > 0 &&
                          (best->ram_estimate_mb <= 0 || model->ram_estimate_mb < best->ram_estimate_mb))) best = model;
        }
        if (best && string_copy(out, out_size, best->id) == 0) return out;
    }
    /* Older installs may use only the legacy endpoint instead of a model registry. */
    if (rt->model_count == 0 && rt->config.llm_enabled) return "";
    return NULL;
}

static LocalModel *model_find_mutable(Runtime *rt, const char *id, bool decision) {
    if (!rt || !id || !*id) return NULL;
    LocalModel *models = decision ? rt->decision_models : rt->models;
    size_t count = decision ? rt->decision_model_count : rt->model_count;
    for (size_t i = 0; i < count; ++i) if (strcmp(models[i].id, id) == 0) return &models[i];
    return NULL;
}

bool model_available(Runtime *rt, const char *id, bool decision) {
    if (!rt || !id || !*id) return rt ? rt->config.llm_enabled : false;
    bool available = false, should_start = false;
    pthread_mutex_lock(&rt->models_mutex);
    LocalModel *m = model_find_mutable(rt, id, decision);
    if (m && m->enabled) {
        if (!m->executable[0]) available = true; /* endpoint managed outside this process */
        else if (m->running && m->pid > 0) available = true;
        else should_start = m->auto_start;
    }
    pthread_mutex_unlock(&rt->models_mutex);
    if (!available && should_start) {
        char start_err[1024] = {0};
        available = model_start(rt, id, decision, start_err, sizeof(start_err)) == 0;
        if (!available) fprintf(stderr, "warning: on-demand model start '%s' failed: %s\n", id,
                                start_err[0] ? start_err : "unknown error");
    }
    return available;
}

static int replace_token(const char *src, const char *token, const char *value, char *dst, size_t cap) {
    if (!src || !token || !value || !dst || cap == 0) return -1;
    size_t used = 0, token_len = strlen(token), value_len = strlen(value);
    const char *p = src;
    while (*p) {
        if (strncmp(p, token, token_len) == 0) {
            if (value_len >= cap - used) return -1;
            memcpy(dst + used, value, value_len); used += value_len; p += token_len;
        } else {
            if (used + 1 >= cap) return -1;
            dst[used++] = *p++;
        }
    }
    dst[used] = '\0'; return 0;
}

static int expand_arg(const LocalModel *m, const char *arg, char *dst, size_t cap) {
    char a[MODEL_ARG_EXPANSION_MAX], b[MODEL_ARG_EXPANSION_MAX];
    char port[16]; (void)snprintf(port, sizeof(port), "%d", m->port);
    /* Expand sequentially; replacement helper copies an argument into a bounded buffer. */
    if (replace_token(arg, "{port}", port, a, sizeof(a)) != 0 ||
        replace_token(a, "{model_path}", m->model_path, b, sizeof(b)) != 0 ||
        replace_token(b, "{model_name}", m->model_name, a, sizeof(a)) != 0 ||
        replace_token(a, "{base_url}", m->base_url, b, sizeof(b)) != 0 ||
        replace_token(b, "{root_dir}", m->root_dir, a, sizeof(a)) != 0) return -1;
    return string_copy(dst, cap, a);
}

static size_t discard_body(char *ptr, size_t size, size_t nmemb, void *userdata) {
    (void)ptr; (void)userdata;
    return size && nmemb > SIZE_MAX / size ? 0 : size * nmemb;
}

typedef struct {
    int context_tokens;
    int parallel;
    long required_ram_mb;
    long required_vram_mb;
    long weights_mb;
    long kv_cache_mb;
} ModelResourcePlan;

static long max_long(long a, long b) { return a > b ? a : b; }

static long model_weight_size_mb(LocalModel *model) {
    uint64_t bytes = model_disk_size_bytes(model);
    if (bytes > 0) {
        uint64_t mb = (bytes + UINT64_C(1048575)) / UINT64_C(1048576);
        return mb > (uint64_t)LONG_MAX ? LONG_MAX : (long)mb;
    }
    if (model->ram_estimate_mb > 0) return model->ram_estimate_mb;
    return 1024; /* Fallback only when model files are inaccessible/unconfigured. */
}

static long model_kv_cache_mb(LocalModel *model, int context_tokens) {
    uint64_t bytes_per_token = model_kv_bytes_per_token(model);
    uint64_t tokens = (uint64_t)(context_tokens > 0 ? context_tokens : 2048);
    if (bytes_per_token && tokens > UINT64_MAX / bytes_per_token) return LONG_MAX;
    uint64_t bytes = tokens * bytes_per_token;
    uint64_t mb = (bytes + UINT64_C(1048575)) / UINT64_C(1048576);
    return mb > (uint64_t)LONG_MAX ? LONG_MAX : (long)mb;
}

static bool model_has_measurable_weights(LocalModel *model) {
    return model && model_disk_size_bytes(model) > 0;
}

static bool model_requests_gpu_offload(const LocalModel *model) {
    if (!model) return false;
    /* Preserve explicit legacy VRAM estimates, but don't require them for newly
       configured models if their launch arguments already enable GPU offload. */
    if (model->vram_estimate_mb > 0) return true;
    for (size_t i = 0; i < model->arg_count; ++i) {
        const char *arg = model->args[i];
        if (strcmp(arg, "-ngl") == 0 || strcmp(arg, "--n-gpu-layers") == 0 ||
            strcmp(arg, "--gpu-layers") == 0) {
            if (i + 1 < model->arg_count) {
                const char *value = model->args[i + 1];
                if (strcasecmp(value, "auto") == 0 || strcasecmp(value, "all") == 0) return true;
                char *end = NULL;
                errno = 0;
                long layers = strtol(value, &end, 10);
                if (errno == 0 && end != value && *end == '\0' && layers > 0) return true;
            }
        }
        if (strncmp(arg, "--n-gpu-layers=", 15) == 0 || strncmp(arg, "--gpu-layers=", 13) == 0) {
            const char *value = strchr(arg, '=') + 1;
            if (strcasecmp(value, "auto") == 0 || strcasecmp(value, "all") == 0) return true;
            char *end = NULL;
            errno = 0;
            long layers = strtol(value, &end, 10);
            if (errno == 0 && end != value && *end == '\0' && layers > 0) return true;
        }
    }
    return false;
}

static void model_requirement_values(LocalModel *model, int context_tokens,
                                     long vram_free_mb, bool shared_gpu, ModelResourcePlan *plan) {
    memset(plan, 0, sizeof(*plan));
    plan->context_tokens = context_tokens;
    bool disk_measured = model_has_measurable_weights(model);
    plan->weights_mb = model_weight_size_mb(model);
    plan->kv_cache_mb = model_kv_cache_mb(model, context_tokens);
    if (!shared_gpu && vram_free_mb >= 0 && model_requests_gpu_offload(model)) {
        /* A disk measurement is preferred to stale config guesses. Estimates remain
           a fallback for model registries that refer to an inaccessible/remote file. */
        long estimated_gpu = plan->weights_mb * 4 / 5 + plan->kv_cache_mb * 3 / 4;
        plan->required_vram_mb = disk_measured ? estimated_gpu :
            max_long(model->vram_estimate_mb, estimated_gpu);
        long host_floor = plan->weights_mb / 5 + plan->kv_cache_mb / 4 + 384;
        plan->required_ram_mb = disk_measured ? host_floor :
            max_long(model->ram_estimate_mb, host_floor);
    } else {
        /* UMA/shared memory is one pool: weights and KV cache both consume RAM.
           A small allowance covers runtime structures, allocator fragmentation and mmap tables. */
        long weights_floor = plan->weights_mb + max_long(384, plan->weights_mb / 20);
        if (!disk_measured) weights_floor = max_long(model->ram_estimate_mb, weights_floor);
        plan->required_ram_mb = weights_floor > LONG_MAX - plan->kv_cache_mb ? LONG_MAX :
                                weights_floor + plan->kv_cache_mb;
        plan->required_vram_mb = 0;
    }
}

static bool plan_fits(long ram_available_mb, long vram_available_mb,
                      const ModelResourcePlan *candidate) {
    bool ram_fits = ram_available_mb < 0 || candidate->required_ram_mb <= ram_available_mb;
    bool vram_fits = candidate->required_vram_mb <= 0 || vram_available_mb < 0 ||
                     candidate->required_vram_mb <= vram_available_mb;
    return ram_fits && vram_fits;
}

static bool model_plan_fits_total_capacity(LocalModel *model, int desired_context,
                                           int minimum_context) {
    HostMemoryStats memory = system_memory_stats();
    GPUStats gpu = system_gpu_stats();
    long reserve = memory_reserve_mb(memory.total_mb);
    long ram_capacity = memory.total_mb > 0 ?
        (memory.total_mb > reserve ? memory.total_mb - reserve : 0) : -1;
    long vram_capacity = gpu.total_mb > 0 ? gpu.total_mb : -1;
    int max_parallel = tier_parallel_cap(model);
    if (max_parallel < 1) max_parallel = 1;
    for (int n = max_parallel; n >= 1; --n) {
        for (int per_request = desired_context; per_request >= minimum_context; ) {
            if (per_request <= INT_MAX / n) {
                ModelResourcePlan candidate;
                model_requirement_values(model, per_request * n, gpu.free_mb,
                                         gpu.shared_system_ram, &candidate);
                if (plan_fits(ram_capacity, vram_capacity, &candidate)) return true;
            }
            if (per_request == minimum_context) break;
            per_request = per_request - minimum_context < 512 ? minimum_context : per_request - 512;
        }
    }
    return false;
}

static int select_model_plan(LocalModel *model, int desired_context, int minimum_context,
                             ModelResourcePlan *result, char *err, size_t err_size) {
    HostMemoryStats memory = system_memory_stats();
    GPUStats gpu = system_gpu_stats();
    long vram_free = gpu.free_mb;
    long reserve = memory_reserve_mb(memory.total_mb);
    long ram_budget = memory.available_mb >= 0 ? memory.available_mb - reserve : LONG_MAX / 4;
    if (ram_budget < 0) ram_budget = 0;
    int max_parallel = tier_parallel_cap(model);
    if (max_parallel < 1) max_parallel = 1;
    if (minimum_context < 2048) minimum_context = 2048;
    if (desired_context < minimum_context) desired_context = minimum_context;

    /* Parallelism is the primary optimization target. Keep the largest context
       that fits for each slot count, shrinking only as far as the prompt-safe
       minimum (estimated input + 512 output tokens). */
    for (int n = max_parallel; n >= 1; --n) {
        for (int per_request = desired_context; per_request >= minimum_context; ) {
            if (per_request <= INT_MAX / n) {
                ModelResourcePlan candidate;
                model_requirement_values(model, per_request * n, vram_free,
                                         gpu.shared_system_ram, &candidate);
                candidate.parallel = n;
                if (plan_fits(ram_budget, vram_free, &candidate)) {
                    *result = candidate;
                    return 0;
                }
            }
            if (per_request == minimum_context) break;
            per_request = per_request - minimum_context < 512 ? minimum_context : per_request - 512;
        }
    }
    ModelResourcePlan minimum;
    model_requirement_values(model, minimum_context, vram_free, gpu.shared_system_ram, &minimum);
    if (err && err_size) {
        (void)snprintf(err, err_size,
            "model '%s' cannot be admitted even at minimum context=%d tokens: RAM available=%ld MB, OS reserve=%ld MB, one-slot estimate=%ld MB; VRAM available=%ld MB, estimate=%ld MB",
            model->id, minimum_context, memory.available_mb, reserve,
            minimum.required_ram_mb, vram_free, minimum.required_vram_mb);
    }
    return -1;
}

static size_t estimate_json_tokens(const char *text) {
    if (!text) return 0;
    size_t ascii_bytes = 0, non_ascii_codepoints = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        if (*p < 0x80) { ++ascii_bytes; ++p; }
        else {
            /* Treat each UTF-8 code point as at least one token. This avoids
               undercounting Cyrillic/CJK prompts with the usual ASCII bytes/4 heuristic. */
            if ((*p & 0xC0U) != 0x80U) ++non_ascii_codepoints;
            ++p;
        }
    }
    size_t ascii_tokens = (ascii_bytes + 3U) / 4U;
    if (SIZE_MAX - ascii_tokens < non_ascii_codepoints) return SIZE_MAX;
    size_t estimate = ascii_tokens + non_ascii_codepoints;
    /* Reserve a small margin for tokenizer fragmentation and serialized tool schemas. */
    if (estimate <= (SIZE_MAX - 9U) / 10U) estimate = (estimate * 10U + 8U) / 9U;
    return estimate;
}

static int estimate_context_per_request(LocalModel *model, json_object *messages,
                                         json_object *tools, int *minimum_context_out) {
    const char *message_json = messages ? json_object_to_json_string_ext(messages, JSON_C_TO_STRING_PLAIN) : "";
    const char *tool_json = tools ? json_object_to_json_string_ext(tools, JSON_C_TO_STRING_PLAIN) : "";
    size_t message_tokens = estimate_json_tokens(message_json);
    size_t tool_tokens = estimate_json_tokens(tool_json);
    size_t input_tokens = SIZE_MAX - message_tokens < tool_tokens ? SIZE_MAX : message_tokens + tool_tokens;
    if (input_tokens < 512U) input_tokens = 512U;
    size_t bucket = input_tokens > SIZE_MAX - 511U ? SIZE_MAX : ((input_tokens + 511U) / 512U) * 512U;

    /* Minimum preserves the estimated input plus 512 output tokens. Desired adds
       extra answer/history headroom, but may be reduced by the memory planner. */
    size_t min_need = bucket > SIZE_MAX - 512U ? SIZE_MAX : bucket + 512U;
    min_need = ((min_need + 511U) / 512U) * 512U;
    if (min_need < 2048U) min_need = 2048U;
    if (min_need > 131072U) min_need = 131072U;
    if (minimum_context_out) *minimum_context_out = (int)min_need;

    if (model) {
        for (size_t i = 0; i < sizeof(model->cached_prompt_buckets) / sizeof(model->cached_prompt_buckets[0]); ++i) {
            if (model->cached_prompt_buckets[i] == bucket && model->cached_context_per_request[i] > 0)
                return model->cached_context_per_request[i];
        }
    }
    size_t extra = bucket / 4U;
    if (extra < 1024U) extra = 1024U;
    size_t need = bucket;
    if (SIZE_MAX - need < extra || SIZE_MAX - (need + extra) < 1024U) need = 131072U;
    else need += extra + 1024U;
    if (need > SIZE_MAX - 1023U) need = 131072U;
    else need = ((need + 1023U) / 1024U) * 1024U;
    if (need < min_need) need = min_need;
    if (need > 131072U) need = 131072U;
    if (model) {
        size_t cache_count = sizeof(model->cached_prompt_buckets) / sizeof(model->cached_prompt_buckets[0]);
        size_t slot = model->context_cache_next++ % cache_count;
        model->cached_prompt_buckets[slot] = bucket;
        model->cached_context_per_request[slot] = (int)need;
    }
    return (int)need;
}

static bool is_llama_server(const LocalModel *model) {
    if (!model || !model->executable[0]) return false;
    const char *base = strrchr(model->executable, '/');
    base = base ? base + 1 : model->executable;
    return strstr(base, "llama-server") != NULL;
}

static bool llama_context_option(const char *arg) {
    return strcmp(arg, "-c") == 0 || strcmp(arg, "--ctx-size") == 0 ||
           strcmp(arg, "--context-size") == 0 || strcmp(arg, "--parallel") == 0 ||
           strcmp(arg, "-np") == 0 || strncmp(arg, "--ctx-size=", 11) == 0 ||
           strncmp(arg, "--context-size=", 15) == 0 || strncmp(arg, "--parallel=", 11) == 0;
}

static int model_health_check(const char *url) {
    CURL *curl = curl_easy_init();
    if (!curl) return 0;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 700L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 300L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    CURLcode rc = curl_easy_perform(curl);
    long status = 0; (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    /* 401/403 still prove the listener is up when a readiness endpoint is protected;
       404 is not healthy and should not hide a misspelled health_url. */
    return rc == CURLE_OK && ((status >= 200 && status < 400) || status == 401 || status == 403);
}

static void stop_child(LocalModel *m) {
    if (!m || m->pid <= 0) { if (m) { m->running = false; m->pid = 0; m->loaded_context_tokens = 0; m->loaded_parallel = 0; } return; }
    pid_t pid = m->pid;
    (void)kill(-pid, SIGTERM);
    int status = 0;
    int attempts = m->shutdown_timeout_seconds * 10;
    for (int i = 0; i < attempts; ++i) {
        pid_t rc = waitpid(pid, &status, WNOHANG);
        if (rc == pid || (rc < 0 && errno == ECHILD)) { m->pid = 0; m->running = false; m->loaded_context_tokens = 0; m->loaded_parallel = 0; return; }
        (void)usleep(100000);
    }
    (void)kill(-pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    m->pid = 0; m->running = false; m->loaded_context_tokens = 0; m->loaded_parallel = 0;
}

static int start_child(LocalModel *m, char *err, size_t err_size) {
    if (!m->enabled) { set_error(err, err_size, "model is disabled in configuration"); return -1; }
    if (!m->executable[0]) { set_error(err, err_size, "model uses an external endpoint and has no managed executable"); return -1; }
    if (m->running && m->pid > 0 && kill(m->pid, 0) == 0) return 0;
    if (access(m->executable, X_OK) != 0) {
        if (err && err_size) snprintf(err, err_size, "model executable is not runnable: %s", m->executable);
        (void)snprintf(m->last_error, sizeof(m->last_error), "%s", err ? err : "executable unavailable"); return -1;
    }
    if (path_mkdirs(m->working_directory, 0700) != 0) { set_error(err, err_size, "cannot create/access model working_directory"); return -1; }
    char log_dir[AO_MAX_PATH];
    if (snprintf(log_dir, sizeof(log_dir), "%s/data/models", m->working_directory) >= (int)sizeof(log_dir) ||
        path_mkdirs(log_dir, 0700) != 0) { set_error(err, err_size, "cannot create model log directory"); return -1; }
    if (snprintf(m->log_path, sizeof(m->log_path), "%s/%s.log", log_dir, m->id) >= (int)sizeof(m->log_path)) {
        set_error(err, err_size, "model log path is too long"); return -1;
    }

    char expanded[AO_MAX_MODEL_ARGS][MODEL_ARG_EXPANSION_MAX];
    char context_value[32], parallel_value[16], port_value[16];
    char *argv[MODEL_MAX_ARGV + 16];
    size_t argc = 0;
    bool auto_context = is_llama_server(m);
    bool have_model = false, have_host = false, have_port = false;
    argv[argc++] = m->executable;
    for (size_t i = 0; i < m->arg_count; ++i) {
        if (auto_context && llama_context_option(m->args[i])) {
            /* Context size and slot count are runtime-planned, never per-model hand tuned. */
            if (strcmp(m->args[i], "-c") == 0 || strcmp(m->args[i], "--ctx-size") == 0 ||
                strcmp(m->args[i], "--context-size") == 0 || strcmp(m->args[i], "--parallel") == 0 ||
                strcmp(m->args[i], "-np") == 0) {
                if (i + 1 < m->arg_count) ++i;
            }
            continue;
        }
        if (auto_context) {
            const char *arg = m->args[i];
            bool model_separate = strcmp(arg, "-m") == 0 || strcmp(arg, "--model") == 0;
            bool model_inline = strncmp(arg, "--model=", 8) == 0 ||
                                (arg[0] == '-' && arg[1] == 'm' && arg[2] != '\0');
            bool host_separate = strcmp(arg, "--host") == 0;
            bool host_inline = strncmp(arg, "--host=", 7) == 0;
            bool port_separate = strcmp(arg, "--port") == 0;
            bool port_inline = strncmp(arg, "--port=", 7) == 0;
            if (model_separate || model_inline) {
                if (have_model) {
                    if (model_separate && i + 1 < m->arg_count) ++i;
                    continue;
                }
                have_model = true;
            }
            if (host_separate || host_inline) {
                if (have_host) {
                    if (host_separate && i + 1 < m->arg_count) ++i;
                    continue;
                }
                have_host = true;
            }
            if (port_separate || port_inline) {
                if (have_port) {
                    if (port_separate && i + 1 < m->arg_count) ++i;
                    continue;
                }
                have_port = true;
            }
        }
        if (expand_arg(m, m->args[i], expanded[i], sizeof(expanded[i])) != 0) {
            set_error(err, err_size, "model argument expansion exceeded size limits"); return -1;
        }
        argv[argc++] = expanded[i];
    }
    if (auto_context) {
        if (!have_model) {
            if (!m->model_path[0]) {
                set_error(err, err_size, "managed llama-server requires model_path or a configured -m/--model argument");
                return -1;
            }
            argv[argc++] = "-m";
            argv[argc++] = m->model_path;
        }
        if (m->port < 1 || m->port > 65535) {
            set_error(err, err_size, "managed llama-server requires a port in 1..65535");
            return -1;
        }
        (void)snprintf(port_value, sizeof(port_value), "%d", m->port);
        if (!have_host) {
            argv[argc++] = "--host";
            argv[argc++] = "127.0.0.1";
        }
        if (!have_port) {
            argv[argc++] = "--port";
            argv[argc++] = port_value;
        }
        (void)snprintf(context_value, sizeof(context_value), "%d", m->planned_context_tokens > 0 ? m->planned_context_tokens : 4096);
        (void)snprintf(parallel_value, sizeof(parallel_value), "%d", m->planned_parallel > 0 ? m->planned_parallel : tier_parallel_cap(m));
        argv[argc++] = "-c";
        argv[argc++] = context_value;
        argv[argc++] = "--parallel";
        argv[argc++] = parallel_value;
    }
    argv[argc] = NULL;
    pid_t pid = fork();
    if (pid < 0) {
        if (err && err_size) (void)snprintf(err, err_size, "fork for model failed: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        (void)setpgid(0, 0);
        (void)chdir(m->working_directory);
        (void)umask(0077);
        struct rlimit core = {0, 0}; (void)setrlimit(RLIMIT_CORE, &core);
        int logfd = open(m->log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (logfd >= 0) { (void)dup2(logfd, STDOUT_FILENO); (void)dup2(logfd, STDERR_FILENO); if (logfd > STDERR_FILENO) close(logfd); }
        else { int nullfd = open("/dev/null", O_WRONLY); if (nullfd >= 0) { (void)dup2(nullfd, STDOUT_FILENO); (void)dup2(nullfd, STDERR_FILENO); if (nullfd > STDERR_FILENO) close(nullfd); } }
        /* Do not leak server/client sockets or configuration file descriptors to the model. */
#ifdef SYS_close_range
        if (syscall(SYS_close_range, 3U, ~0U, 0U) != 0) {
            for (int fd = 3; fd < 65536; ++fd) (void)close(fd);
        }
#else
        for (int fd = 3; fd < 65536; ++fd) (void)close(fd);
#endif
        execv(m->executable, argv);
        (void)write(STDERR_FILENO, "model execv failed\n", sizeof("model execv failed\n") - 1);
        _exit(127);
    }
    (void)setpgid(pid, pid);
    m->pid = pid; m->running = true; m->last_error[0] = '\0';
    if (curl_runtime_init() != 0) { (void)snprintf(m->last_error, sizeof(m->last_error), "libcurl initialization failed"); stop_child(m); set_error(err, err_size, "libcurl initialization failed"); return -1; }
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 250000000L};
    int checks = m->startup_timeout_seconds * 4;
    for (int i = 0; i < checks; ++i) {
        if (model_health_check(m->health_url)) return 0;
        int child_status = 0; pid_t child = waitpid(pid, &child_status, WNOHANG);
        if (child == pid || (child < 0 && errno == ECHILD)) {
            m->pid = 0; m->running = false; m->loaded_context_tokens = 0; m->loaded_parallel = 0;
            if (err && err_size) snprintf(err, err_size, "model '%s' exited before readiness; inspect %s", m->id, m->log_path);
            (void)snprintf(m->last_error, sizeof(m->last_error), "process exited before readiness; see %.400s", m->log_path);
            return -1;
        }
        (void)nanosleep(&pause, NULL);
    }
    (void)snprintf(m->last_error, sizeof(m->last_error), "readiness timeout; see %.400s", m->log_path);
    if (err && err_size) snprintf(err, err_size, "model '%s' did not become ready within %d seconds; inspect %s", m->id, m->startup_timeout_seconds, m->log_path);
    stop_child(m);
    return -1;
}

static int wait_for_resource_admission(const LocalModel *model, char *err, size_t err_size) {
    if (!model || !model->executable[0]) return 0;
    ModelResourcePlan need;
    GPUStats gpu = system_gpu_stats();
    long vram_now = gpu.free_mb;
    int ctx = model->planned_context_tokens > 0 ? model->planned_context_tokens : 4096;
    model_requirement_values((LocalModel *)model, ctx, vram_now, gpu.shared_system_ram, &need);
    HostMemoryStats memory = system_memory_stats();
    long reserve = memory_reserve_mb(memory.total_mb);
    int max_wait = model->startup_timeout_seconds;
    if (max_wait < 1) max_wait = 1;
    if (max_wait > 120) max_wait = 120;
    int waited = 0;
    for (;;) {
        long available_ram = system_mem_available_mb();
        long available_vram = system_vram_free_mb();
        bool ram_ok = available_ram < 0 || available_ram >= need.required_ram_mb + reserve;
        bool vram_ok = need.required_vram_mb <= 0 || available_vram < 0 || available_vram >= need.required_vram_mb;
        if (ram_ok && vram_ok) return 0;
        if (waited >= max_wait) {
            if (err && err_size) (void)snprintf(err, err_size,
                "resource admission timeout for model '%s': RAM available=%ld MB, OS reserve=%ld MB, required=%ld MB; VRAM available=%ld MB, required=%ld MB",
                model->id, available_ram, reserve, need.required_ram_mb,
                available_vram, need.required_vram_mb);
            return -1;
        }
        fprintf(stderr, "model scheduler: waiting for resources for '%s' (RAM %ld/%ld MB + %ld MB reserve, VRAM %ld/%ld MB)\n",
                model->id, available_ram, need.required_ram_mb, reserve, available_vram, need.required_vram_mb);
        struct timespec pause = {.tv_sec = 1, .tv_nsec = 0};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
        ++waited;
    }
}

static int make_model_plan_wait(LocalModel *model, int desired_context, int minimum_context,
                                ModelResourcePlan *plan, char *err, size_t err_size) {
    if (!model_plan_fits_total_capacity(model, desired_context, minimum_context)) {
        HostMemoryStats memory = system_memory_stats();
        long reserve = memory_reserve_mb(memory.total_mb);
        if (err && err_size) (void)snprintf(err, err_size,
            "model '%s' weights plus prompt-safe context do not fit the RAM/VRAM capacity even when other models are stopped (total RAM/cgroup=%ld MB, OS reserve=%ld MB, minimum context=%d)",
            model->id, memory.total_mb, reserve, minimum_context);
        return -1;
    }
    int max_wait = model->startup_timeout_seconds;
    if (max_wait < 1) max_wait = 1;
    if (max_wait > 120) max_wait = 120;
    for (int waited = 0;; ++waited) {
        char last_error[768] = {0};
        if (select_model_plan(model, desired_context, minimum_context, plan,
                              last_error, sizeof(last_error)) == 0) return 0;
        if (waited >= max_wait) {
            set_error(err, err_size, last_error[0] ? last_error : "no resource plan fits the available system memory");
            return -1;
        }
        fprintf(stderr, "model scheduler: waiting for memory to free for '%s' (desired context=%d, minimum context=%d)\n",
                model->id, desired_context, minimum_context);
        struct timespec pause = {.tv_sec = 1, .tv_nsec = 0};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
    }
}

bool model_configured_enabled(Runtime *rt, const char *id, bool decision) {
    if (!rt) return false;
    if (!id || !*id) return rt->config.llm_enabled;
    bool enabled = false;
    pthread_mutex_lock(&rt->models_mutex);
    LocalModel *m = model_find_mutable(rt, id, decision);
    enabled = m && m->enabled;
    pthread_mutex_unlock(&rt->models_mutex);
    return enabled;
}

int model_request_acquire(Runtime *rt, const char *id, bool decision,
                          json_object *messages, json_object *tools,
                          char *err, size_t err_size) {
    if (!rt || !id || !*id) return 0; /* Legacy endpoint has no managed model registry entry. */
    if (rt->model_scheduler_mutex_initialized) pthread_mutex_lock(&rt->model_scheduler_mutex);
    pthread_mutex_lock(&rt->models_mutex);
    LocalModel *model = model_find_mutable(rt, id, decision);
    if (!model || !model->enabled) {
        bool known = model != NULL;
        pthread_mutex_unlock(&rt->models_mutex);
        if (rt->model_scheduler_mutex_initialized) pthread_mutex_unlock(&rt->model_scheduler_mutex);
        set_error(err, err_size, known ? "model is disabled" : "unknown model id");
        return -1;
    }
    if (!model->executable[0]) {
        ++model->active_requests;
        pthread_mutex_unlock(&rt->models_mutex);
        if (rt->model_scheduler_mutex_initialized) pthread_mutex_unlock(&rt->model_scheduler_mutex);
        return 0; /* External endpoints cannot be resized/unloaded and their process memory is not observable here. */
    }

    int minimum_context_per_request = 2048;
    int context_per_request = estimate_context_per_request(model, messages, tools, &minimum_context_per_request);
    bool running = model->running && model->pid > 0 && kill(model->pid, 0) == 0;
    bool runtime_resizable = is_llama_server(model);
    bool context_sufficient = running && (!runtime_resizable ||
        (model->loaded_context_tokens > 0 && model->loaded_parallel > 0 &&
         model->loaded_context_tokens / model->loaded_parallel >= context_per_request));
    if (context_sufficient) {
        ++model->active_requests;
        pthread_mutex_unlock(&rt->models_mutex);
        if (rt->model_scheduler_mutex_initialized) pthread_mutex_unlock(&rt->model_scheduler_mutex);
        return 0;
    }

    /* Only a llama-server can be resized. Wait for active work to finish before
       replacing its context so that no in-flight request is interrupted. */
    if (running) {
        int waited_ms = 0;
        int wait_limit_ms = model->startup_timeout_seconds * 1000;
        if (wait_limit_ms > 120000) wait_limit_ms = 120000;
        while (model->active_requests > 0 && waited_ms < wait_limit_ms) {
            pthread_mutex_unlock(&rt->models_mutex);
            struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000L};
            (void)nanosleep(&pause, NULL);
            waited_ms += 100;
            pthread_mutex_lock(&rt->models_mutex);
            model = model_find_mutable(rt, id, decision);
            if (!model) break;
        }
        if (!model || model->active_requests > 0) {
            pthread_mutex_unlock(&rt->models_mutex);
            if (rt->model_scheduler_mutex_initialized) pthread_mutex_unlock(&rt->model_scheduler_mutex);
            if (err && err_size) (void)snprintf(err, err_size,
                "timed out waiting for active requests before resizing model '%s' context", id);
            return -1;
        }
        stop_child(model);
    }

    /* Snapshot model configuration and release models_mutex before resource probing
       and wait loops. Health/status endpoints must stay responsive under memory pressure. */
    LocalModel plan_model = *model;
    pthread_mutex_unlock(&rt->models_mutex);

    ModelResourcePlan plan;
    if (make_model_plan_wait(&plan_model, context_per_request, minimum_context_per_request, &plan, err, err_size) != 0) {
        if (rt->model_scheduler_mutex_initialized) pthread_mutex_unlock(&rt->model_scheduler_mutex);
        return -1;
    }
    plan_model.planned_context_tokens = plan.context_tokens;
    plan_model.planned_parallel = plan.parallel;
    if (wait_for_resource_admission(&plan_model, err, err_size) != 0) {
        if (rt->model_scheduler_mutex_initialized) pthread_mutex_unlock(&rt->model_scheduler_mutex);
        return -1;
    }

    pthread_mutex_lock(&rt->models_mutex);
    model = model_find_mutable(rt, id, decision);
    if (!model || !model->enabled) {
        pthread_mutex_unlock(&rt->models_mutex);
        if (rt->model_scheduler_mutex_initialized) pthread_mutex_unlock(&rt->model_scheduler_mutex);
        set_error(err, err_size, model ? "model was disabled while waiting for resources" : "model disappeared from registry");
        return -1;
    }
    model->disk_size_bytes_cached = plan_model.disk_size_bytes_cached;
    model->disk_cache_mtime = plan_model.disk_cache_mtime;
    model->disk_cache_checked_at = plan_model.disk_cache_checked_at;
    model->kv_bytes_per_token_cached = plan_model.kv_bytes_per_token_cached;
    model->kv_metadata_checked_at = plan_model.kv_metadata_checked_at;
    model->planned_context_tokens = plan.context_tokens;
    model->planned_parallel = plan.parallel;
    int rc = start_child(model, err, err_size);
    if (rc == 0) {
        model->loaded_context_tokens = model->planned_context_tokens;
        model->loaded_parallel = model->planned_parallel;
        ++model->active_requests;
    }
    pthread_mutex_unlock(&rt->models_mutex);
    if (rt->model_scheduler_mutex_initialized) pthread_mutex_unlock(&rt->model_scheduler_mutex);
    return rc;
}

void model_request_release(Runtime *rt, const char *id, bool decision) {
    if (!rt || !id || !*id) return;
    pthread_mutex_lock(&rt->models_mutex);
    LocalModel *model = model_find_mutable(rt, id, decision);
    if (model && model->active_requests > 0) --model->active_requests;
    pthread_mutex_unlock(&rt->models_mutex);
}

int model_start(Runtime *rt, const char *id, bool decision, char *err, size_t err_size) {
    if (!rt || !id) { set_error(err, err_size, "runtime and model id are required"); return -1; }
    if (rt->model_scheduler_mutex_initialized) pthread_mutex_lock(&rt->model_scheduler_mutex);
    pthread_mutex_lock(&rt->models_mutex);
    LocalModel *model = model_find_mutable(rt, id, decision);
    if (!model) {
        pthread_mutex_unlock(&rt->models_mutex);
        if (rt->model_scheduler_mutex_initialized) pthread_mutex_unlock(&rt->model_scheduler_mutex);
        set_error(err, err_size, "unknown model id"); return -1;
    }
    if (model->running && model->pid > 0 && kill(model->pid, 0) == 0) {
        pthread_mutex_unlock(&rt->models_mutex);
        if (rt->model_scheduler_mutex_initialized) pthread_mutex_unlock(&rt->model_scheduler_mutex);
        return 0;
    }
    if (!model->enabled) {
        pthread_mutex_unlock(&rt->models_mutex);
        if (rt->model_scheduler_mutex_initialized) pthread_mutex_unlock(&rt->model_scheduler_mutex);
        set_error(err, err_size, "model is disabled in configuration"); return -1;
    }
    if (!model->executable[0]) {
        pthread_mutex_unlock(&rt->models_mutex);
        if (rt->model_scheduler_mutex_initialized) pthread_mutex_unlock(&rt->model_scheduler_mutex);
        set_error(err, err_size, "model uses an external endpoint and cannot be started by ASGEND"); return -1;
    }
    LocalModel plan_model = *model;
    pthread_mutex_unlock(&rt->models_mutex);

    ModelResourcePlan plan;
    int default_context = 4096;
    int minimum_context = 2048;
    if (make_model_plan_wait(&plan_model, default_context, minimum_context, &plan, err, err_size) != 0) {
        if (rt->model_scheduler_mutex_initialized) pthread_mutex_unlock(&rt->model_scheduler_mutex);
        return -1;
    }
    plan_model.planned_context_tokens = plan.context_tokens;
    plan_model.planned_parallel = plan.parallel;
    if (wait_for_resource_admission(&plan_model, err, err_size) != 0) {
        if (rt->model_scheduler_mutex_initialized) pthread_mutex_unlock(&rt->model_scheduler_mutex);
        return -1;
    }

    pthread_mutex_lock(&rt->models_mutex);
    model = model_find_mutable(rt, id, decision);
    if (!model || !model->enabled) {
        pthread_mutex_unlock(&rt->models_mutex);
        if (rt->model_scheduler_mutex_initialized) pthread_mutex_unlock(&rt->model_scheduler_mutex);
        set_error(err, err_size, model ? "model was disabled while waiting for resources" : "model disappeared from registry");
        return -1;
    }
    if (model->running && model->pid > 0 && kill(model->pid, 0) == 0) {
        pthread_mutex_unlock(&rt->models_mutex);
        if (rt->model_scheduler_mutex_initialized) pthread_mutex_unlock(&rt->model_scheduler_mutex);
        return 0;
    }
    model->disk_size_bytes_cached = plan_model.disk_size_bytes_cached;
    model->disk_cache_mtime = plan_model.disk_cache_mtime;
    model->disk_cache_checked_at = plan_model.disk_cache_checked_at;
    model->kv_bytes_per_token_cached = plan_model.kv_bytes_per_token_cached;
    model->kv_metadata_checked_at = plan_model.kv_metadata_checked_at;
    model->planned_context_tokens = plan.context_tokens;
    model->planned_parallel = plan.parallel;
    int rc = start_child(model, err, err_size);
    if (rc == 0) {
        model->loaded_context_tokens = model->planned_context_tokens;
        model->loaded_parallel = model->planned_parallel;
    }
    pthread_mutex_unlock(&rt->models_mutex);
    if (rt->model_scheduler_mutex_initialized) pthread_mutex_unlock(&rt->model_scheduler_mutex);
    return rc;
}

int model_stop(Runtime *rt, const char *id, bool decision, char *err, size_t err_size) {
    if (!rt || !id) { set_error(err, err_size, "runtime and model id are required"); return -1; }
    pthread_mutex_lock(&rt->models_mutex);
    LocalModel *m = model_find_mutable(rt, id, decision);
    if (!m) { pthread_mutex_unlock(&rt->models_mutex); set_error(err, err_size, "unknown model id"); return -1; }
    if (m->pid > 0) stop_child(m);
    m->last_error[0] = '\0';
    pthread_mutex_unlock(&rt->models_mutex);
    return 0;
}

int models_start_autostart(Runtime *rt, char *err, size_t err_size) {
    if (!rt) { set_error(err, err_size, "runtime is required"); return -1; }
    for (int role = 0; role < 2; ++role) {
        LocalModel *array = role ? rt->decision_models : rt->models;
        size_t count = role ? rt->decision_model_count : rt->model_count;
        for (size_t i = 0; i < count; ++i) {
            LocalModel *m = &array[i];
            if (!m->enabled || !m->start_at_boot) continue;
            char start_err[1024] = {0};
            if (model_start(rt, m->id, role != 0, start_err, sizeof(start_err)) != 0)
                fprintf(stderr, "warning: model autostart '%s' failed: %s\n", m->id, start_err);
            else fprintf(stderr, "model '%s' is ready at %s\n", m->id, m->base_url);
        }
    }
    (void)err; (void)err_size;
    return 0;
}

void models_stop_all(Runtime *rt) {
    if (!rt || !rt->models_mutex_initialized) return;
    pthread_mutex_lock(&rt->models_mutex);
    for (size_t i = 0; i < rt->model_count; ++i) stop_child(&rt->models[i]);
    for (size_t i = 0; i < rt->decision_model_count; ++i) stop_child(&rt->decision_models[i]);
    pthread_mutex_unlock(&rt->models_mutex);
}

static json_object *model_array_json(Runtime *rt, bool decision) {
    json_object *array_json = json_object_new_array();
    if (!array_json) return NULL;
    pthread_mutex_lock(&rt->models_mutex);
    LocalModel *models = decision ? rt->decision_models : rt->models;
    size_t count = decision ? rt->decision_model_count : rt->model_count;
    for (size_t i = 0; i < count; ++i) {
        LocalModel *m = &models[i];
        json_object *v = json_object_new_object();
        json_object_object_add(v, "id", json_object_new_string(m->id));
        json_object_object_add(v, "name", json_object_new_string(m->name));
        json_object_object_add(v, "kind", json_object_new_string(decision ? "decision" : "llm"));
        json_object_object_add(v, "enabled", json_object_new_boolean(m->enabled));
        json_object_object_add(v, "auto_start", json_object_new_boolean(m->auto_start));
        json_object_object_add(v, "start_at_boot", json_object_new_boolean(m->start_at_boot));
        json_object_object_add(v, "managed", json_object_new_boolean(m->executable[0] != '\0'));
        json_object_object_add(v, "running", json_object_new_boolean(m->running && m->pid > 0));
        bool available = m->enabled && (!m->executable[0] || (m->running && m->pid > 0));
        json_object_object_add(v, "available", json_object_new_boolean(available));
        json_object_object_add(v, "base_url", json_object_new_string(m->base_url));
        json_object_object_add(v, "api_type", json_object_new_string(m->api_type));
        json_object_object_add(v, "model_role", json_object_new_string(m->model_role));
        json_object_object_add(v, "runtime_adapter", json_object_new_string(m->runtime_adapter));
        json_object_object_add(v, "input_modalities", json_object_new_string(m->input_modalities));
        json_object_object_add(v, "output_modalities", json_object_new_string(m->output_modalities));
        json_object_object_add(v, "model_name", json_object_new_string(m->model_name));
        json_object_object_add(v, "node_id", json_object_new_string(m->node_id));
        json_object_object_add(v, "location_type", json_object_new_string(m->location_type));
        json_object_object_add(v, "tier", json_object_new_string(m->tier));
        json_object_object_add(v, "ram_estimate_mb", json_object_new_int(m->ram_estimate_mb));
        json_object_object_add(v, "vram_estimate_mb", json_object_new_int(m->vram_estimate_mb));
        uint64_t disk_bytes = model_disk_size_bytes(m);
        json_object_object_add(v, "disk_size_bytes", json_object_new_int64(disk_bytes > INT64_MAX ? INT64_MAX : (int64_t)disk_bytes));
        json_object_object_add(v, "disk_size_mb", json_object_new_int64((int64_t)((disk_bytes + UINT64_C(1048575)) / UINT64_C(1048576))));
        json_object_object_add(v, "kv_bytes_per_token_estimate", json_object_new_int64((int64_t)model_kv_bytes_per_token(m)));
        int estimated_context = m->planned_context_tokens > 0 ? m->planned_context_tokens : 2048;
        json_object_object_add(v, "estimated_context_tokens", json_object_new_int(estimated_context));
        json_object_object_add(v, "estimated_kv_cache_mb", json_object_new_int(model_kv_cache_mb(m, estimated_context)));
        json_object_object_add(v, "actual_rss_mb", json_object_new_int64(m->actual_rss_mb));
        json_object_object_add(v, "actual_pss_mb", json_object_new_int64(m->actual_pss_mb));
        json_object_object_add(v, "actual_gpu_memory_mb", json_object_new_int64(m->actual_gpu_memory_mb));
        json_object_object_add(v, "planned_context_tokens", json_object_new_int(m->planned_context_tokens));
        json_object_object_add(v, "loaded_context_tokens", json_object_new_int(m->loaded_context_tokens));
        json_object_object_add(v, "planned_parallel", json_object_new_int(m->planned_parallel));
        json_object_object_add(v, "loaded_parallel", json_object_new_int(m->loaded_parallel));
        json_object_object_add(v, "active_requests", json_object_new_int(m->active_requests));
        json_object_object_add(v, "parallel_cap_by_tier", json_object_new_int(tier_parallel_cap(m)));
        json_object_object_add(v, "port", json_object_new_int(m->port));
        json_object_object_add(v, "last_error", json_object_new_string(m->last_error));
        json_object_object_add(v, "log_path", json_object_new_string(m->log_path));
        json_object_array_add(array_json, v);
    }
    pthread_mutex_unlock(&rt->models_mutex);
    return array_json;
}

json_object *models_status_json(Runtime *rt) {
    if (!rt) return NULL;
    json_object *result = json_object_new_object();
    if (!result) return NULL;
    json_object *models = model_array_json(rt, false), *decisions = model_array_json(rt, true);
    json_object_object_add(result, "models", models ? models : json_object_new_array());
    json_object_object_add(result, "decision_models", decisions ? decisions : json_object_new_array());
    json_object_object_add(result, "default_model_id", json_object_new_string(rt->config.default_model_id));
    json_object_object_add(result, "decision_model_id", json_object_new_string(rt->config.decision_model_id));
    json_object *registry = rt->config.models_config_path[0] ? json_object_from_file(rt->config.models_config_path) : NULL;
    json_object *nodes = registry ? obj_get(registry, "nodes") : NULL;
    if (nodes && json_object_get_type(nodes) == json_type_array) json_object_object_add(result, "nodes", json_object_get(nodes));
    else json_object_object_add(result, "nodes", json_object_new_array());
    if (registry) json_object_put(registry);
    return result;
}


int model_set_enabled(Runtime *rt, const char *id, bool decision, bool enabled, char *err, size_t err_size) {
    if (!rt || !id || !*id || !rt->config.models_config_path[0]) { set_error(err, err_size, "model registry and id are required"); return -1; }
    uint64_t prior_fingerprint = config_editor_fingerprint(rt);
    bool disk_was_active = prior_fingerprint != 0 && prior_fingerprint == rt->active_config_fingerprint;
    pthread_mutex_lock(&rt->models_mutex);
    LocalModel *model = model_find_mutable(rt, id, decision);
    if (!model) { pthread_mutex_unlock(&rt->models_mutex); set_error(err, err_size, "model id was not found"); return -1; }

    json_object *root = json_object_from_file(rt->config.models_config_path);
    json_object *array = root ? obj_get(root, decision ? "decision_models" : "models") : NULL;
    json_object *entry = NULL;
    if (array && json_object_get_type(array) == json_type_array) {
        for (size_t i = 0; i < json_object_array_length(array); ++i) {
            json_object *candidate = json_object_array_get_idx(array, i);
            json_object *id_obj = obj_get(candidate, "id");
            if (id_obj && json_object_get_type(id_obj) == json_type_string && strcmp(json_object_get_string(id_obj), id) == 0) { entry = candidate; break; }
        }
    }
    if (!entry) {
        if (root) json_object_put(root);
        pthread_mutex_unlock(&rt->models_mutex);
        set_error(err, err_size, "model profile is missing from models.json"); return -1;
    }
    if (enabled) {
        json_object *node_id_obj = obj_get(entry, "node_id");
        const char *node_id_json = node_id_obj && json_object_get_type(node_id_obj) == json_type_string ? json_object_get_string(node_id_obj) : "";
        char node_id_copy[AO_MAX_ID] = "";
        if (string_copy(node_id_copy, sizeof(node_id_copy), node_id_json) != 0) {
            json_object_put(root); pthread_mutex_unlock(&rt->models_mutex);
            set_error(err, err_size, "model node_id is too long"); return -1;
        }
        const char *node_id = node_id_copy;
        if (*node_id) {
            json_object *nodes = obj_get(root, "nodes"), *node = NULL;
            for (size_t i = 0; nodes && json_object_get_type(nodes) == json_type_array && i < json_object_array_length(nodes); ++i) {
                json_object *candidate = json_object_array_get_idx(nodes, i), *candidate_id = obj_get(candidate, "id");
                if (candidate_id && json_object_get_type(candidate_id) == json_type_string && strcmp(json_object_get_string(candidate_id), node_id) == 0) { node = candidate; break; }
            }
            json_object *node_enabled = node ? obj_get(node, "enabled") : NULL;
            if (!node || !node_enabled || json_object_get_type(node_enabled) != json_type_boolean || !json_object_get_boolean(node_enabled)) {
                json_object_put(root); pthread_mutex_unlock(&rt->models_mutex);
                if (err && err_size) snprintf(err, err_size, "cannot enable model '%s': node '%s' is missing or disabled", id, node_id);
                return -1;
            }
        }
    }
    json_object_object_add(entry, "enabled", json_object_new_boolean(enabled));
    const char *serialized = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE);
    if (!serialized) {
        json_object_put(root); pthread_mutex_unlock(&rt->models_mutex); set_error(err, err_size, "cannot serialize model registry"); return -1;
    }
    char temp_path[AO_MAX_PATH];
    if (snprintf(temp_path, sizeof(temp_path), "%s.enableXXXXXX", rt->config.models_config_path) >= (int)sizeof(temp_path)) {
        json_object_put(root); pthread_mutex_unlock(&rt->models_mutex); set_error(err, err_size, "model registry path is too long"); return -1;
    }
    int fd = mkstemp(temp_path);
    if (fd < 0) {
        if (err && err_size) snprintf(err, err_size, "cannot stage model registry: %s", strerror(errno));
        json_object_put(root); pthread_mutex_unlock(&rt->models_mutex); return -1;
    }
    int rc = fchmod(fd, 0600);
    size_t len = strlen(serialized), offset = 0;
    while (rc == 0 && offset < len) {
        ssize_t n = write(fd, serialized + offset, len - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { rc = -1; break; }
        offset += (size_t)n;
    }
    if (rc == 0 && fsync(fd) != 0) rc = -1;
    int saved = errno;
    if (close(fd) != 0 && rc == 0) { rc = -1; saved = errno; }
    if (rc == 0 && rename(temp_path, rt->config.models_config_path) != 0) { rc = -1; saved = errno; }
    if (rc != 0) {
        (void)unlink(temp_path);
        if (err && err_size) snprintf(err, err_size, "cannot persist enabled state: %s", strerror(saved));
        json_object_put(root); pthread_mutex_unlock(&rt->models_mutex); return -1;
    }
    if (!enabled && model->running) stop_child(model);
    model->enabled = enabled;
    json_object_put(root);
    pthread_mutex_unlock(&rt->models_mutex);
    if (disk_was_active) {
        uint64_t fingerprint = config_editor_fingerprint(rt);
        if (fingerprint) rt->active_config_fingerprint = fingerprint;
    }
    return 0;
}

