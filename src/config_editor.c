#include "runtime.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define CONFIG_STAGE_MAX (AO_MAX_AGENTS + 2)

static pthread_mutex_t config_editor_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    char destination[AO_MAX_PATH];
    char temporary[AO_MAX_PATH];
} StagedFile;

static void set_error(char *dst, size_t cap, const char *message) {
    if (dst && cap) (void)snprintf(dst, cap, "%s", message ? message : "configuration operation failed");
}

static json_object *get_object(json_object *obj, const char *key) {
    json_object *value = NULL;
    return obj && json_object_object_get_ex(obj, key, &value) && value &&
           json_object_get_type(value) == json_type_object ? value : NULL;
}

static json_object *get_array(json_object *obj, const char *key) {
    json_object *value = NULL;
    return obj && json_object_object_get_ex(obj, key, &value) && value &&
           json_object_get_type(value) == json_type_array ? value : NULL;
}

static const char *get_string(json_object *obj, const char *key) {
    const char *value = NULL;
    return json_get_string(obj, key, &value) == 0 ? value : NULL;
}

static bool legacy_fixed_role_id(const char *id) {
    return id && (strcasecmp(id, "coder") == 0 || strcasecmp(id, "planner") == 0 ||
                  strcasecmp(id, "reviewer") == 0);
}

static bool valid_id(const char *id) {
    if (!id || !*id || strlen(id) >= AO_MAX_ID) return false;
    for (const unsigned char *p = (const unsigned char *)id; *p; ++p)
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return false;
    return true;
}

static int compare_paths(const void *a, const void *b) {
    const char *pa = (const char *)a;
    const char *pb = (const char *)b;
    return strcmp(pa, pb);
}

uint64_t config_editor_fingerprint(Runtime *rt) {
    if (!rt || !rt->config.main_config_path[0]) return 0;
    char paths[AO_MAX_AGENTS + 2][AO_MAX_PATH];
    size_t count = 0;
    (void)snprintf(paths[count++], AO_MAX_PATH, "%s", rt->config.main_config_path);
    if (rt->config.models_config_path[0]) (void)snprintf(paths[count++], AO_MAX_PATH, "%s", rt->config.models_config_path);
    DIR *dir = opendir(rt->config.agents_dir);
    if (!dir) return 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t n = strlen(entry->d_name);
        if (n < 6 || strcmp(entry->d_name + n - 5, ".json") != 0 || count >= AO_MAX_AGENTS + 2) continue;
        if (snprintf(paths[count], AO_MAX_PATH, "%s/%s", rt->config.agents_dir, entry->d_name) < (int)AO_MAX_PATH) count++;
    }
    closedir(dir);
    qsort(paths, count, sizeof(paths[0]), compare_paths);
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < count; ++i) {
        FILE *file = fopen(paths[i], "rb");
        if (!file) return 0;
        for (const unsigned char *p = (const unsigned char *)paths[i]; *p; ++p) { hash ^= *p; hash *= UINT64_C(1099511628211); }
        int c;
        while ((c = fgetc(file)) != EOF) { hash ^= (unsigned char)c; hash *= UINT64_C(1099511628211); }
        if (ferror(file)) { fclose(file); return 0; }
        fclose(file);
        hash ^= 0xffU; hash *= UINT64_C(1099511628211);
    }
    return hash ? hash : 1;
}

static int write_all(int fd, const char *data, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        ssize_t rc = write(fd, data + offset, length - offset);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) return -1;
        offset += (size_t)rc;
    }
    return 0;
}

static int stage_json(const char *destination, json_object *obj, StagedFile *stage,
                      char *err, size_t err_size) {
    const char *serialized = json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE);
    if (!serialized || strlen(destination) + 16 >= sizeof(stage->temporary)) {
        set_error(err, err_size, "configuration path or JSON serialization is too large"); return -1;
    }
    (void)snprintf(stage->destination, sizeof(stage->destination), "%s", destination);
    (void)snprintf(stage->temporary, sizeof(stage->temporary), "%s.stageXXXXXX", destination);
    int fd = mkstemp(stage->temporary);
    if (fd < 0) {
        if (err && err_size) snprintf(err, err_size, "cannot stage config file %s: %s", destination, strerror(errno));
        stage->temporary[0] = '\0'; return -1;
    }
    int rc = fchmod(fd, 0600);
    if (rc == 0) rc = write_all(fd, serialized, strlen(serialized));
    if (rc == 0) rc = fsync(fd);
    int saved = errno;
    if (close(fd) != 0 && rc == 0) { rc = -1; saved = errno; }
    if (rc != 0) {
        (void)unlink(stage->temporary); stage->temporary[0] = '\0'; errno = saved;
        if (err && err_size) snprintf(err, err_size, "cannot write staged config %s: %s", destination, strerror(errno));
        return -1;
    }
    return 0;
}

static void cleanup_stages(StagedFile *stages, size_t count) {
    for (size_t i = 0; i < count; ++i) if (stages[i].temporary[0]) (void)unlink(stages[i].temporary);
}

static int commit_stages(StagedFile *stages, size_t count, char *err, size_t err_size) {
    for (size_t i = 0; i < count; ++i) {
        if (rename(stages[i].temporary, stages[i].destination) != 0) {
            if (err && err_size) snprintf(err, err_size, "cannot commit config file %s: %s", stages[i].destination, strerror(errno));
            return -1;
        }
        stages[i].temporary[0] = '\0';
    }
    return 0;
}

static json_object *agents_read_json(Runtime *rt, char *err, size_t err_size) {
    json_object *array = json_object_new_array();
    if (!array) { set_error(err, err_size, "out of memory reading agents"); return NULL; }
    DIR *dir = opendir(rt->config.agents_dir);
    if (!dir) {
        if (err && err_size) snprintf(err, err_size, "cannot open agents directory: %s", strerror(errno));
        json_object_put(array); return NULL;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t n = strlen(entry->d_name);
        if (n < 6 || strcmp(entry->d_name + n - 5, ".json") != 0) continue;
        char path[AO_MAX_PATH];
        if (snprintf(path, sizeof(path), "%s/%s", rt->config.agents_dir, entry->d_name) >= (int)sizeof(path)) continue;
        struct stat st;
        if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > 128 * 1024) continue;
        json_object *agent = json_object_from_file(path);
        if (!agent || json_object_get_type(agent) != json_type_object) {
            if (agent) json_object_put(agent);
            closedir(dir); json_object_put(array);
            if (err && err_size) snprintf(err, err_size, "invalid agent JSON on disk: %s", entry->d_name);
            return NULL;
        }
        const char *agent_id = get_string(agent, "id");
        if (legacy_fixed_role_id(agent_id)) {
            /* Hide legacy fixed roles from clients; a subsequent save removes these files. */
            json_object_put(agent);
            continue;
        }
        json_object_array_add(array, agent);
    }
    closedir(dir);
    return array;
}

static json_object *config_editor_get_unlocked(Runtime *rt, char *err, size_t err_size) {
    if (!rt) { set_error(err, err_size, "runtime is required"); return NULL; }
    json_object *main = json_object_from_file(rt->config.main_config_path);
    if (!main || json_object_get_type(main) != json_type_object) {
        if (main) json_object_put(main);
        if (err && err_size) snprintf(err, err_size, "cannot read main config at %s", rt->config.main_config_path);
        return NULL;
    }
    json_object *models = NULL;
    if (rt->config.models_config_path[0]) {
        models = json_object_from_file(rt->config.models_config_path);
        if (!models || json_object_get_type(models) != json_type_object) {
            if (models) json_object_put(models);
            json_object_put(main);
            if (err && err_size) snprintf(err, err_size, "cannot read model registry at %s", rt->config.models_config_path);
            return NULL;
        }
    } else {
        models = json_object_new_object();
        json_object_object_add(models, "defaults", json_object_new_object());
        json_object_object_add(models, "models", json_object_new_array());
        json_object_object_add(models, "decision_models", json_object_new_array());
    }
    json_object *agents = agents_read_json(rt, err, err_size);
    if (!agents) { json_object_put(main); json_object_put(models); return NULL; }
    json_object *result = json_object_new_object();
    if (!result) { json_object_put(main); json_object_put(models); json_object_put(agents); set_error(err, err_size, "out of memory creating config response"); return NULL; }
    json_object_object_add(result, "ok", json_object_new_boolean(true));
    json_object_object_add(result, "main", main);
    json_object_object_add(result, "models", models);
    json_object_object_add(result, "agents", agents);
    uint64_t current_fingerprint = config_editor_fingerprint(rt);
    bool restart_required = rt->active_config_fingerprint == 0 || current_fingerprint == 0 || current_fingerprint != rt->active_config_fingerprint;
    json_object_object_add(result, "restart_required", json_object_new_boolean(restart_required));
    return result;
}

static int validate_agents(json_object *agents, char *err, size_t err_size) {
    if (!agents || json_object_get_type(agents) != json_type_array || json_object_array_length(agents) == 0 ||
        json_object_array_length(agents) > AO_MAX_AGENTS) {
        set_error(err, err_size, "agents must be an array with 1..64 entries"); return -1;
    }
    for (size_t i = 0; i < json_object_array_length(agents); ++i) {
        json_object *a = json_object_array_get_idx(agents, i);
        const char *id = get_string(a, "id");
        const char *name = get_string(a, "name");
        if (!a || json_object_get_type(a) != json_type_object || !valid_id(id) || !name || !*name) {
            set_error(err, err_size, "each execution profile needs a valid id and a non-empty name"); return -1;
        }
        if (legacy_fixed_role_id(id)) {
            set_error(err, err_size, "fixed-role profiles coder/planner/reviewer are obsolete; use a flexible execution profile"); return -1;
        }
        for (size_t j = 0; j < i; ++j) {
            json_object *prev = json_object_array_get_idx(agents, j);
            const char *prev_id = get_string(prev, "id");
            if (prev_id && strcmp(id, prev_id) == 0) { if (err && err_size) snprintf(err, err_size, "duplicate agent id '%s'", id); return -1; }
        }
        const char *prompt = get_string(a, "system_prompt");
        if (prompt && strlen(prompt) >= AO_MAX_TEXT) { if (err && err_size) snprintf(err, err_size, "system_prompt for agent '%s' exceeds %d bytes", id, AO_MAX_TEXT - 1); return -1; }
        const char *array_keys[] = {"tools", "skills"};
        const size_t limits[] = {AO_MAX_TOOLS, AO_MAX_SKILLS};
        for (size_t k = 0; k < 2; ++k) {
            json_object *array = NULL;
            if (!json_object_object_get_ex(a, array_keys[k], &array)) continue;
            if (!array || json_object_get_type(array) != json_type_array || json_object_array_length(array) > limits[k]) {
                if (err && err_size) (void)snprintf(err, err_size, "%s for agent '%s' must be an array within size limits", array_keys[k], id);
                return -1;
            }
            for (size_t j = 0; j < json_object_array_length(array); ++j) {
                json_object *v = json_object_array_get_idx(array, j);
                if (!v || json_object_get_type(v) != json_type_string || strlen(json_object_get_string(v)) >= AO_MAX_NAME) {
                    if (err && err_size) (void)snprintf(err, err_size, "%s values for agent '%s' must be strings shorter than %d bytes", array_keys[k], id, AO_MAX_NAME);
                    return -1;
                }
            }
        }
    }
    return 0;
}

static void build_preview_agents(Runtime *preview, json_object *agents) {
    preview->agent_count = 0;
    for (size_t i = 0; i < json_object_array_length(agents) && i < AO_MAX_AGENTS; ++i) {
        json_object *item = json_object_array_get_idx(agents, i);
        Agent *a = &preview->agents[preview->agent_count++];
        const char *id = get_string(item, "id"), *model_id = get_string(item, "model_id");
        if (id) (void)snprintf(a->id, sizeof(a->id), "%s", id);
        if (model_id && strlen(model_id) < sizeof(a->model_id)) (void)snprintf(a->model_id, sizeof(a->model_id), "%s", model_id);
    }
}

static int validate_model_registry(Runtime *rt, Config *candidate_config, json_object *models,
                                   json_object *agents, const char *models_stage,
                                   char *err, size_t err_size) {
    if (!models || json_object_get_type(models) != json_type_object ||
        (get_array(models, "models") == NULL && get_array(models, "decision_models") == NULL)) {
        set_error(err, err_size, "models config must be an object with models and/or decision_models arrays"); return -1;
    }
    /* Runtime is intentionally large (agent prompts, model registry and MCP state).
       Keep this temporary validation instance off the request worker's stack. */
    Runtime *preview = calloc(1, sizeof(*preview));
    if (!preview) {
        set_error(err, err_size, "out of memory allocating configuration preview");
        return -1;
    }
    preview->config = *candidate_config;
    if (strlen(models_stage) >= sizeof(preview->config.models_config_path)) {
        set_error(err, err_size, "staged model registry path is too long");
        free(preview);
        return -1;
    }
    memcpy(preview->config.models_config_path, models_stage, strlen(models_stage) + 1);
    build_preview_agents(preview, agents);
    int rc = models_load(preview, err, err_size);
    free(preview);
    (void)rt;
    return rc;
}

static bool id_in_agent_array(json_object *agents, const char *id) {
    for (size_t i = 0; i < json_object_array_length(agents); ++i) {
        const char *value = get_string(json_object_array_get_idx(agents, i), "id");
        if (value && strcmp(value, id) == 0) return true;
    }
    return false;
}

static int config_editor_save_unlocked(Runtime *rt, json_object *body, char *err, size_t err_size) {
    if (!rt || !body || json_object_get_type(body) != json_type_object) { set_error(err, err_size, "request body must be a JSON object"); return -1; }
    if (!rt->config.models_config_path[0]) { set_error(err, err_size, "configuration editor requires paths.models_config to be configured"); return -1; }
    json_object *main = get_object(body, "main"), *models = get_object(body, "models");
    json_object *agents = get_array(body, "agents");
    if (!main || !models || !agents) { set_error(err, err_size, "request must contain main object, models object and agents array"); return -1; }
    if (validate_agents(agents, err, err_size) != 0) return -1;

    StagedFile stages[CONFIG_STAGE_MAX]; memset(stages, 0, sizeof(stages));
    size_t staged = 0;
    if (stage_json(rt->config.main_config_path, main, &stages[staged], err, err_size) != 0) goto fail;
    size_t main_stage_i = staged++;
    Config candidate_config;
    if (config_load(&candidate_config, stages[main_stage_i].temporary, err, err_size) != 0) goto fail;
    /* The web editor may change service settings, but configuration locations are pinned to the active config tree. */
    if (strcmp(candidate_config.agents_dir, rt->config.agents_dir) != 0 ||
        strcmp(candidate_config.models_config_path, rt->config.models_config_path) != 0) {
        set_error(err, err_size, "paths.agents_dir and paths.models_config are fixed to keep the UI and on-disk configuration in sync"); goto fail;
    }
    if (stage_json(rt->config.models_config_path, models, &stages[staged], err, err_size) != 0) goto fail;
    size_t models_stage_i = staged++;
    if (validate_model_registry(rt, &candidate_config, models, agents, stages[models_stage_i].temporary, err, err_size) != 0) goto fail;

    for (size_t i = 0; i < json_object_array_length(agents); ++i) {
        json_object *agent = json_object_array_get_idx(agents, i);
        const char *id = get_string(agent, "id");
        char destination[AO_MAX_PATH];
        if (snprintf(destination, sizeof(destination), "%s/%s.json", rt->config.agents_dir, id) >= (int)sizeof(destination)) {
            set_error(err, err_size, "agent config path too long"); goto fail;
        }
        if (stage_json(destination, agent, &stages[staged], err, err_size) != 0) goto fail;
        staged++;
    }
    if (commit_stages(stages, staged, err, err_size) != 0) goto fail;

    DIR *dir = opendir(rt->config.agents_dir);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            size_t n = strlen(entry->d_name);
            if (n < 6 || strcmp(entry->d_name + n - 5, ".json") != 0) continue;
            char id[AO_MAX_ID]; size_t id_len = n - 5;
            if (id_len == 0 || id_len >= sizeof(id)) continue;
            memcpy(id, entry->d_name, id_len); id[id_len] = '\0';
            /* Legacy fixed-role profiles are intentionally hidden from config_get.
               Never delete a file merely because it is omitted from that response. */
            if (valid_id(id) && !legacy_fixed_role_id(id) && !id_in_agent_array(agents, id)) {
                char remove_path[AO_MAX_PATH];
                if (snprintf(remove_path, sizeof(remove_path), "%s/%s", rt->config.agents_dir, entry->d_name) < (int)sizeof(remove_path)) (void)unlink(remove_path);
            }
        }
        closedir(dir);
    }
    return 0;
fail:
    cleanup_stages(stages, staged + 1 <= CONFIG_STAGE_MAX ? staged + 1 : staged);
    return -1;
}



static bool setting_key_allowed(const char *section, const char *key) {
    if (!section || !key) return false;
    if (strcmp(section, "server") == 0)
        return strcmp(key, "max_connections") == 0 || strcmp(key, "request_timeout_seconds") == 0;
    if (strcmp(section, "runtime") == 0)
        return strcmp(key, "workers") == 0 || strcmp(key, "queue_max_pending") == 0 ||
               strcmp(key, "max_retries") == 0 || strcmp(key, "priority_aging_seconds") == 0;
    if (strcmp(section, "sandbox") == 0)
        return strcmp(key, "enabled") == 0 || strcmp(key, "require_landlock") == 0 ||
               strcmp(key, "allow_network") == 0 || strcmp(key, "cpu_seconds") == 0 ||
               strcmp(key, "memory_mb") == 0 || strcmp(key, "file_size_mb") == 0 ||
               strcmp(key, "open_files") == 0;
    return false;
}

static int validate_setting_value(const char *section, const char *key, json_object *value,
                                  char *err, size_t err_size) {
    if (!setting_key_allowed(section, key)) {
        if (err && err_size) (void)snprintf(err, err_size, "setting %s.%s is not editable through this endpoint", section, key);
        return -1;
    }
    if (strcmp(section, "sandbox") == 0 &&
        (strcmp(key, "enabled") == 0 || strcmp(key, "require_landlock") == 0 || strcmp(key, "allow_network") == 0)) {
        if (!value || json_object_get_type(value) != json_type_boolean) {
            if (err && err_size) (void)snprintf(err, err_size, "setting %s.%s must be a boolean", section, key);
            return -1;
        }
        return 0;
    }
    if (!value || json_object_get_type(value) != json_type_int) {
        if (err && err_size) (void)snprintf(err, err_size, "setting %s.%s must be an integer", section, key);
        return -1;
    }
    int64_t n = json_object_get_int64(value);
    int64_t low = 0, high = 0;
    if (strcmp(section, "server") == 0 && strcmp(key, "max_connections") == 0) { low = 1; high = 4096; }
    else if (strcmp(section, "server") == 0 && strcmp(key, "request_timeout_seconds") == 0) { low = 1; high = 600; }
    else if (strcmp(section, "runtime") == 0 && strcmp(key, "workers") == 0) { low = 1; high = 64; }
    else if (strcmp(section, "runtime") == 0 && strcmp(key, "queue_max_pending") == 0) { low = 1; high = 1000000; }
    else if (strcmp(section, "runtime") == 0 && strcmp(key, "max_retries") == 0) { low = 0; high = 10; }
    else if (strcmp(section, "runtime") == 0 && strcmp(key, "priority_aging_seconds") == 0) { low = 1; high = 86400; }
    else if (strcmp(section, "sandbox") == 0 && strcmp(key, "cpu_seconds") == 0) { low = 1; high = 86400; }
    else if (strcmp(section, "sandbox") == 0 && strcmp(key, "memory_mb") == 0) { low = 64; high = 65536; }
    else if (strcmp(section, "sandbox") == 0 && strcmp(key, "file_size_mb") == 0) { low = 1; high = 4096; }
    else if (strcmp(section, "sandbox") == 0 && strcmp(key, "open_files") == 0) { low = 16; high = 4096; }
    else { if (err && err_size) (void)snprintf(err, err_size, "unsupported setting %s.%s", section, key); return -1; }
    if (n < low || n > high) {
        if (err && err_size) (void)snprintf(err, err_size, "setting %s.%s must be in %lld..%lld", section, key,
                                             (long long)low, (long long)high);
        return -1;
    }
    return 0;
}

static int validate_settings_request(json_object *request, json_object *settings,
                                     char *err, size_t err_size) {
    if (!request || json_object_get_type(request) != json_type_object || !settings ||
        json_object_get_type(settings) != json_type_object) {
        set_error(err, err_size, "request must contain a settings object");
        return -1;
    }
    json_object_object_foreach(request, root_key, root_value) {
        (void)root_value;
        if (strcmp(root_key, "op") != 0 && strcmp(root_key, "auth_token") != 0 && strcmp(root_key, "settings") != 0) {
            if (err && err_size) (void)snprintf(err, err_size, "unexpected request field: %s", root_key);
            return -1;
        }
    }
    if (json_object_object_length(settings) == 0) {
        set_error(err, err_size, "settings object must contain at least one editable field");
        return -1;
    }
    json_object_object_foreach(settings, section_key, section_value) {
        if (strcmp(section_key, "server") != 0 && strcmp(section_key, "runtime") != 0 && strcmp(section_key, "sandbox") != 0) {
            if (err && err_size) (void)snprintf(err, err_size, "settings section %s is not editable", section_key);
            return -1;
        }
        if (!section_value || json_object_get_type(section_value) != json_type_object ||
            json_object_object_length(section_value) == 0) {
            if (err && err_size) (void)snprintf(err, err_size, "settings section %s must be a non-empty object", section_key);
            return -1;
        }
        json_object_object_foreach(section_value, setting_key, setting_value) {
            if (validate_setting_value(section_key, setting_key, setting_value, err, err_size) != 0) return -1;
        }
    }
    return 0;
}

static int fsync_parent_directory(const char *path) {
    char parent[AO_MAX_PATH];
    if (!path || strlen(path) >= sizeof(parent)) { errno = ENAMETOOLONG; return -1; }
    (void)snprintf(parent, sizeof(parent), "%s", path);
    char *slash = strrchr(parent, '/');
    if (!slash) (void)snprintf(parent, sizeof(parent), ".");
    else if (slash == parent) slash[1] = '\0';
    else *slash = '\0';
    int fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    int rc = fsync(fd);
    int saved = errno;
    (void)close(fd);
    errno = saved;
    return rc;
}

static int config_editor_update_settings_unlocked(Runtime *rt, json_object *request,
                                                  char *err, size_t err_size) {
    if (!rt || !request || json_object_get_type(request) != json_type_object) {
        set_error(err, err_size, "request body must be a JSON object");
        return -1;
    }
    json_object *settings = get_object(request, "settings");
    if (validate_settings_request(request, settings, err, err_size) != 0) return -1;

    struct stat st;
    if (lstat(rt->config.main_config_path, &st) != 0 || !S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
        set_error(err, err_size, "main config must be a regular non-symlink file");
        return -1;
    }
    json_object *main = json_object_from_file(rt->config.main_config_path);
    if (!main || json_object_get_type(main) != json_type_object) {
        if (main) json_object_put(main);
        set_error(err, err_size, "cannot read existing main config object");
        return -1;
    }

    StagedFile candidate_stage; memset(&candidate_stage, 0, sizeof(candidate_stage));
    StagedFile backup_stage; memset(&backup_stage, 0, sizeof(backup_stage));
    char backup_path[AO_MAX_PATH];
    if (snprintf(backup_path, sizeof(backup_path), "%s.android-rollback", rt->config.main_config_path) >= (int)sizeof(backup_path)) {
        json_object_put(main); set_error(err, err_size, "config rollback path is too long"); return -1;
    }
    /* Never replace an earlier recovery copy left by an uncertain commit. */
    struct stat rollback_st;
    if (lstat(backup_path, &rollback_st) == 0) {
        json_object_put(main);
        set_error(err, err_size, "unresolved settings rollback file exists; reconcile it before another update");
        return -1;
    }
    if (errno != ENOENT) {
        int saved = errno;
        json_object_put(main);
        if (err && err_size) (void)snprintf(err, err_size, "cannot inspect config rollback path: %s", strerror(saved));
        errno = saved;
        return -1;
    }
    /* Stage the original before mutating the in-memory document. */
    if (stage_json(backup_path, main, &backup_stage, err, err_size) != 0) {
        json_object_put(main); return -1;
    }

    json_object_object_foreach(settings, section_key, section_value) {
        json_object *current_section = get_object(main, section_key);
        if (!current_section) {
            json_object_put(main); cleanup_stages(&backup_stage, 1);
            if (err && err_size) (void)snprintf(err, err_size, "existing config section %s is missing or invalid", section_key);
            return -1;
        }
        json_object_object_foreach(section_value, setting_key, setting_value) {
            (void)json_object_object_add(current_section, setting_key, json_object_get(setting_value));
        }
    }

    if (stage_json(rt->config.main_config_path, main, &candidate_stage, err, err_size) != 0) {
        json_object_put(main); cleanup_stages(&backup_stage, 1); return -1;
    }
    json_object_put(main);

    Config candidate_config;
    if (config_load(&candidate_config, candidate_stage.temporary, err, err_size) != 0) goto fail_stages;
    if (strcmp(candidate_config.agents_dir, rt->config.agents_dir) != 0 ||
        strcmp(candidate_config.models_config_path, rt->config.models_config_path) != 0 ||
        strcmp(candidate_config.database_path, rt->config.database_path) != 0) {
        set_error(err, err_size, "paths and database location are fixed through the settings update endpoint");
        goto fail_stages;
    }

    /* Save a private rollback copy before replacing the single config file. */
    if (rename(backup_stage.temporary, backup_path) != 0) {
        if (err && err_size) (void)snprintf(err, err_size, "cannot stage rollback copy: %s", strerror(errno));
        goto fail_stages;
    }
    backup_stage.temporary[0] = '\0';
    if (rename(candidate_stage.temporary, rt->config.main_config_path) != 0) {
        int saved = errno;
        (void)unlink(backup_path);
        errno = saved;
        if (err && err_size) (void)snprintf(err, err_size, "cannot commit settings update: %s", strerror(errno));
        goto fail_stages;
    }
    candidate_stage.temporary[0] = '\0';
    if (fsync_parent_directory(rt->config.main_config_path) != 0) {
        int saved = errno;
        if (rename(backup_path, rt->config.main_config_path) != 0) {
            if (err && err_size) (void)snprintf(err, err_size, "settings commit uncertain; rollback copy retained");
        } else {
            (void)fsync_parent_directory(rt->config.main_config_path);
            if (err && err_size) (void)snprintf(err, err_size, "settings commit failed durability check and was rolled back");
        }
        errno = saved;
        goto fail_stages;
    }
    if (unlink(backup_path) == 0) (void)fsync_parent_directory(rt->config.main_config_path);
    cleanup_stages(&candidate_stage, 1);
    cleanup_stages(&backup_stage, 1);
    return 0;

fail_stages:
    cleanup_stages(&candidate_stage, 1);
    cleanup_stages(&backup_stage, 1);
    return -1;
}

/* Serializes config reads against multi-file saves and prevents concurrent UI
   sessions from interleaving their independently staged updates. */
json_object *config_editor_get(Runtime *rt, char *err, size_t err_size) {
    pthread_mutex_lock(&config_editor_mutex);
    json_object *result = config_editor_get_unlocked(rt, err, err_size);
    pthread_mutex_unlock(&config_editor_mutex);
    return result;
}

int config_editor_save(Runtime *rt, json_object *body, char *err, size_t err_size) {
    pthread_mutex_lock(&config_editor_mutex);
    int rc = config_editor_save_unlocked(rt, body, err, err_size);
    pthread_mutex_unlock(&config_editor_mutex);
    return rc;
}


int config_editor_update_settings(Runtime *rt, json_object *request, char *err, size_t err_size) {
    pthread_mutex_lock(&config_editor_mutex);
    int rc = config_editor_update_settings_unlocked(rt, request, err, err_size);
    pthread_mutex_unlock(&config_editor_mutex);
    return rc;
}
