#include "runtime.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/stat.h>

static bool agent_allows(const Agent *agent, const char *name) {
    if (!agent || !name) return false;
    for (size_t i = 0; i < agent->tool_count; ++i)
        if (strcmp(agent->tools[i], name) == 0) return true;
    return false;
}

static void add_tool_schema(json_object *arr, const char *name, const char *description,
                            const char *parameter, const char *parameter_description,
                            bool required) {
    json_object *tool = json_object_new_object();
    json_object *fn = json_object_new_object();
    json_object *params = json_object_new_object();
    json_object *props = json_object_new_object();
    json_object *prop = json_object_new_object();
    json_object *required_arr = json_object_new_array();
    json_object_object_add(tool, "type", json_object_new_string("function"));
    json_object_object_add(fn, "name", json_object_new_string(name));
    json_object_object_add(fn, "description", json_object_new_string(description));
    json_object_object_add(prop, "type", json_object_new_string("string"));
    json_object_object_add(prop, "description", json_object_new_string(parameter_description));
    json_object_object_add(props, parameter, prop);
    json_object_object_add(params, "type", json_object_new_string("object"));
    json_object_object_add(params, "properties", props);
    if (required) { json_object_array_add(required_arr, json_object_new_string(parameter)); json_object_object_add(params, "required", required_arr); }
    else json_object_put(required_arr);
    json_object_object_add(params, "additionalProperties", json_object_new_boolean(false));
    json_object_object_add(fn, "parameters", params);
    json_object_object_add(tool, "function", fn);
    json_object_array_add(arr, tool);
}

json_object *tools_schemas_for_agent(Runtime *rt, const Agent *agent) {
    json_object *arr = json_object_new_array();
    if (!arr) return NULL;
    for (size_t i = 0; i < agent->tool_count; ++i) {
        const char *name = agent->tools[i];
        if (strcmp(name, "echo") == 0)
            add_tool_schema(arr, "echo", "Return the supplied text. Use only for testing or simple text transformations.", "text", "Text to return", true);
        else if (strcmp(name, "current_time") == 0) {
            json_object *tool = json_object_new_object();
            json_object *fn = json_object_new_object();
            json_object *params = json_object_new_object();
            json_object_object_add(tool, "type", json_object_new_string("function"));
            json_object_object_add(fn, "name", json_object_new_string("current_time"));
            json_object_object_add(fn, "description", json_object_new_string("Return current UTC time as ISO 8601."));
            json_object_object_add(params, "type", json_object_new_string("object"));
            json_object_object_add(params, "properties", json_object_new_object());
            json_object_object_add(params, "additionalProperties", json_object_new_boolean(false));
            json_object_object_add(fn, "parameters", params);
            json_object_object_add(tool, "function", fn);
            json_object_array_add(arr, tool);
        } else if (strcmp(name, "knowledge_search") == 0)
            add_tool_schema(arr, "knowledge_search", "Search local trusted .md/.txt files in the knowledge directory.", "query", "Keywords to search for", true);
        else if (strcmp(name, "lsp_diagnostics") == 0)
            add_tool_schema(arr, "lsp_diagnostics", "Run the configured language server against a file inside the workspace and return real compiler/linter diagnostics. An empty diagnostics list means no issues were reported by that server.", "file_path", "Path to an existing workspace file, relative to the workspace root or absolute inside that root", true);
    }
    for (size_t i = 0; i < rt->mcp_tool_count; ++i) {
        MCPTool *mcp_tool = &rt->mcp_tools[i];
        if (!agent_allows(agent, mcp_tool->alias)) continue;
        json_object *tool = json_object_new_object();
        json_object *fn = json_object_new_object();
        json_object_object_add(tool, "type", json_object_new_string("function"));
        json_object_object_add(fn, "name", json_object_new_string(mcp_tool->alias));
        json_object_object_add(fn, "description", json_object_new_string(mcp_tool->description));
        json_object_object_add(fn, "parameters", mcp_tool->input_schema ? json_object_get(mcp_tool->input_schema) : json_object_new_object());
        json_object_object_add(tool, "function", fn);
        json_object_array_add(arr, tool);
    }
    return arr;
}

static char *tool_error(const char *message) {
    json_object *obj = json_object_new_object();
    json_object_object_add(obj, "ok", json_object_new_boolean(false));
    json_object_object_add(obj, "error", json_object_new_string(message));
    const char *s = json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN);
    char *copy = string_duplicate(s ? s : "{\"ok\":false}");
    json_object_put(obj);
    return copy;
}

static char *knowledge_search(Runtime *rt, const char *query) {
    if (!query || !*query) return tool_error("query is required");
    DIR *d = opendir(rt->config.knowledge_dir);
    if (!d) return tool_error("knowledge directory is not available");
    json_object *matches = json_object_new_array();
    struct dirent *entry;
    size_t total = 0;
    while ((entry = readdir(d)) != NULL && total < 6) {
        size_t n = strlen(entry->d_name);
        if (n < 4 || (strcmp(entry->d_name + n - 3, ".md") != 0 && strcmp(entry->d_name + n - 4, ".txt") != 0)) continue;
        char path[AO_MAX_PATH];
        int written = snprintf(path, sizeof(path), "%s/%s", rt->config.knowledge_dir, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(path)) continue;
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > 1024 * 1024) continue;
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        size_t cap = (size_t)st.st_size + 1;
        char *content = malloc(cap);
        if (!content) { fclose(f); break; }
        size_t got = fread(content, 1, cap - 1, f);
        fclose(f); content[got] = '\0';
        if (strcasestr(content, query)) {
            json_object *match = json_object_new_object();
            size_t extract = got > 2500 ? 2500 : got;
            content[extract] = '\0';
            json_object_object_add(match, "file", json_object_new_string(entry->d_name));
            json_object_object_add(match, "excerpt", json_object_new_string(content));
            json_object_array_add(matches, match);
            ++total;
        }
        free(content);
    }
    closedir(d);
    json_object *out = json_object_new_object();
    json_object_object_add(out, "ok", json_object_new_boolean(true));
    json_object_object_add(out, "query", json_object_new_string(query));
    json_object_object_add(out, "matches", matches);
    const char *s = json_object_to_json_string_ext(out, JSON_C_TO_STRING_PLAIN);
    char *copy = string_duplicate(s ? s : "{}");
    json_object_put(out);
    return copy;
}

char *tool_execute(Runtime *rt, const Agent *agent, const char *name,
                   json_object *arguments, char *err, size_t err_size) {
    if (!rt || !name || !*name) {
        if (err && err_size) (void)snprintf(err, err_size, "runtime and non-empty tool name are required");
        return tool_error("runtime and tool name are required");
    }
    if (!agent_allows(agent, name)) {
        if (err && err_size) (void)snprintf(err, err_size, "agent '%s' is not allowed to call tool '%s'", agent ? agent->id : "?", name ? name : "?");
        return tool_error("tool call denied by agent allow-list");
    }
    if (!arguments || json_object_get_type(arguments) != json_type_object) return tool_error("tool arguments must be a JSON object");
    if (strcmp(name, "echo") == 0) {
        const char *text = NULL;
        if (json_get_string(arguments, "text", &text) != 0) return tool_error("text is required");
        json_object *out = json_object_new_object();
        json_object_object_add(out, "ok", json_object_new_boolean(true));
        json_object_object_add(out, "text", json_object_new_string_len(text, (int)(strlen(text) > 8192 ? 8192 : strlen(text))));
        const char *s = json_object_to_json_string_ext(out, JSON_C_TO_STRING_PLAIN);
        char *copy = string_duplicate(s ? s : "{}"); json_object_put(out); return copy;
    }
    if (strcmp(name, "current_time") == 0) {
        time_t now = time(NULL); struct tm tm_utc;
        if (!gmtime_r(&now, &tm_utc)) return tool_error("failed to read current time");
        char stamp[64];
        if (strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &tm_utc) == 0) return tool_error("failed to format current time");
        json_object *out = json_object_new_object();
        json_object_object_add(out, "ok", json_object_new_boolean(true));
        json_object_object_add(out, "utc", json_object_new_string(stamp));
        const char *s = json_object_to_json_string_ext(out, JSON_C_TO_STRING_PLAIN);
        char *copy = string_duplicate(s ? s : "{}"); json_object_put(out); return copy;
    }
    if (strcmp(name, "knowledge_search") == 0) {
        const char *query = NULL;
        if (json_get_string(arguments, "query", &query) != 0) return tool_error("query is required");
        return knowledge_search(rt, query);
    }
    if (strcmp(name, "lsp_diagnostics") == 0) {
        const char *file_path = NULL;
        if (json_get_string(arguments, "file_path", &file_path) != 0 || !*file_path) return tool_error("file_path is required");
        char lsp_err[1024] = {0};
        char *diagnostics = lsp_run_diagnostics(rt, file_path, lsp_err, sizeof(lsp_err));
        if (!diagnostics) return tool_error(lsp_err[0] ? lsp_err : "language server diagnostics failed");
        json_object *out = json_object_new_object();
        json_object_object_add(out, "ok", json_object_new_boolean(true));
        json_object_object_add(out, "diagnostics", json_object_new_string(diagnostics));
        free(diagnostics);
        const char *s = json_object_to_json_string_ext(out, JSON_C_TO_STRING_PLAIN);
        char *copy = string_duplicate(s ? s : "{}");
        json_object_put(out);
        return copy;
    }
    for (size_t i = 0; i < rt->mcp_tool_count; ++i) {
        if (strcmp(rt->mcp_tools[i].alias, name) == 0)
            return mcp_call_tool(rt, name, arguments, err, err_size);
    }
    if (err && err_size) (void)snprintf(err, err_size, "unknown tool '%s'", name ? name : "?");
    return tool_error("unknown tool");
}
