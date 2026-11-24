#include "runtime.h"
#include <dirent.h>
#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static void set_error(char *dst, size_t n, const char *msg) {
    if (dst && n) (void)snprintf(dst, n, "%s", msg ? msg : "agent configuration error");
}

static void copy_json_string(json_object *obj, const char *key, char *dst, size_t cap, const char *fallback) {
    const char *s = NULL;
    if (json_get_string(obj, key, &s) == 0 && strlen(s) < cap) (void)snprintf(dst, cap, "%s", s);
    else (void)snprintf(dst, cap, "%s", fallback ? fallback : "");
}

static void load_string_array(json_object *obj, const char *key, char values[][AO_MAX_NAME], size_t *count, size_t max) {
    json_object *arr = NULL;
    *count = 0;
    if (!json_object_object_get_ex(obj, key, &arr) || !arr || json_object_get_type(arr) != json_type_array) return;
    size_t n = json_object_array_length(arr);
    if (n > max) n = max;
    for (size_t i = 0; i < n; ++i) {
        json_object *v = json_object_array_get_idx(arr, i);
        if (!v || json_object_get_type(v) != json_type_string) continue;
        const char *s = json_object_get_string(v);
        if (!s || strlen(s) >= AO_MAX_NAME) continue;
        (void)snprintf(values[*count], AO_MAX_NAME, "%s", s);
        ++*count;
    }
}

static int parse_agent(Runtime *rt, const char *file, char *err, size_t err_size) {
    if (rt->agent_count >= AO_MAX_AGENTS) return 0;
    json_object *obj = json_object_from_file(file);
    if (!obj || json_object_get_type(obj) != json_type_object) {
        if (obj) json_object_put(obj);
        if (err && err_size) (void)snprintf(err, err_size, "invalid agent JSON: %s", file);
        return -1;
    }
    Agent a;
    memset(&a, 0, sizeof(a));
    copy_json_string(obj, "id", a.id, sizeof(a.id), "");
    /* Ignore the old shipped role aliases when upgrading an existing home.
       They are not execution profiles; all steps use the one task-level route. */
    if (strcasecmp(a.id, "coder") == 0 || strcasecmp(a.id, "planner") == 0 ||
        strcasecmp(a.id, "reviewer") == 0) {
        json_object_put(obj);
        return 0;
    }
    copy_json_string(obj, "name", a.name, sizeof(a.name), a.id);
    copy_json_string(obj, "description", a.description, sizeof(a.description), "");
    copy_json_string(obj, "model_id", a.model_id, sizeof(a.model_id), "");
    copy_json_string(obj, "system_prompt", a.system_prompt, sizeof(a.system_prompt), "You are a helpful task execution agent. Follow the task instruction and return a concise, verifiable result.");
    load_string_array(obj, "tools", a.tools, &a.tool_count, AO_MAX_TOOLS);
    load_string_array(obj, "skills", a.skills, &a.skill_count, AO_MAX_SKILLS);
    json_object_put(obj);
    if (!a.id[0] || a.id[0] == '.' || strchr(a.id, '/') || strchr(a.id, '\\')) {
        if (err && err_size) (void)snprintf(err, err_size, "agent id is empty or invalid in %s", file);
        return -1;
    }
    for (size_t i = 0; i < rt->agent_count; ++i) {
        if (strcmp(rt->agents[i].id, a.id) == 0) {
            if (err && err_size) (void)snprintf(err, err_size, "duplicate agent id '%s'", a.id);
            return -1;
        }
    }
    rt->agents[rt->agent_count++] = a;
    return 0;
}

int agents_load(Runtime *rt, char *err, size_t err_size) {
    DIR *d = opendir(rt->config.agents_dir);
    if (!d) {
        if (err && err_size) (void)snprintf(err, err_size, "cannot open agents directory %s: %s", rt->config.agents_dir, strerror(errno));
        return -1;
    }
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len < 6 || strcmp(entry->d_name + len - 5, ".json") != 0) continue;
        char full[AO_MAX_PATH];
        int n = snprintf(full, sizeof(full), "%s/%s", rt->config.agents_dir, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(full)) { closedir(d); set_error(err, err_size, "agent config path too long"); return -1; }
        if (parse_agent(rt, full, err, err_size) != 0) { closedir(d); return -1; }
    }
    closedir(d);
    if (rt->agent_count == 0) { set_error(err, err_size, "no agent JSON files found"); return -1; }
    return 0;
}

const Agent *agent_find(const Runtime *rt, const char *id) {
    if (!rt || !id || !*id) return NULL;
    for (size_t i = 0; i < rt->agent_count; ++i)
        if (strcmp(rt->agents[i].id, id) == 0) return &rt->agents[i];
    return NULL;
}

const Agent *agent_choose(const Runtime *rt, const char *task, const char *requested_id) {
    const Agent *requested = agent_find(rt, requested_id);
    if (requested) return requested;
    /* The fallback deliberately has no built-in coder/planner/reviewer roles. Agents
       are data-driven profiles; the decision model performs semantic routing when on. */
    if (!rt || rt->agent_count == 0) return NULL;
    const Agent *best = &rt->agents[0];
    unsigned best_score = 0;
    if (task) {
        for (size_t i = 0; i < rt->agent_count; ++i) {
            const Agent *candidate = &rt->agents[i];
            unsigned score = 0;
            const char *fields[] = {candidate->name, candidate->description, candidate->id};
            for (size_t f = 0; f < sizeof(fields) / sizeof(fields[0]); ++f) {
                if (!fields[f] || !*fields[f]) continue;
                /* Reward meaningful profile phrases appearing in the task, not fixed roles. */
                char phrase[128]; size_t used = 0;
                const char *p = fields[f];
                while (*p) {
                    while (*p && !(isalnum((unsigned char)*p) || (unsigned char)*p >= 0x80)) ++p;
                    size_t n = 0;
                    while (p[n] && (isalnum((unsigned char)p[n]) || (unsigned char)p[n] >= 0x80) && n < sizeof(phrase) - 1) ++n;
                    if (n >= 4 && n < sizeof(phrase)) {
                        memcpy(phrase, p, n); phrase[n] = '\0';
                        if (strcasestr(task, phrase)) score += (unsigned)(n > 12 ? 12 : n);
                    }
                    p += n ? n : (*p ? 1 : 0);
                    if (++used > 64) break;
                }
            }
            if (score > best_score) { best = candidate; best_score = score; }
        }
    }
    return best;
}
