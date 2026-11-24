#ifndef AO_RUNTIME_H
#define AO_RUNTIME_H

#include <json-c/json.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/types.h>

#define AO_MAX_AGENTS 64
#define AO_MAX_TOOLS 32
#define AO_MAX_SKILLS 32
#define AO_MAX_MCP_SERVERS 16
#define AO_MAX_MODELS 32
#define AO_MAX_MODEL_ARGS 32
#define AO_MAX_MCP_ARGS 32
#define AO_MAX_MCP_ENV_VARS 16
#define AO_MAX_MCP_ENV_NAME 128
#define AO_MAX_MCP_READ_PATHS 16
#define AO_MAX_MCP_WRITE_PATHS 8
#define AO_MAX_MCP_TOOLS 128
#define AO_MAX_LSP_SERVERS 16
#define AO_MAX_LSP_ARGS 16
#define AO_MAX_LSP_EXTENSIONS 16
#define AO_MCP_LINE_MAX (1024U * 1024U)
#define AO_MAX_NAME 96
#define AO_MAX_ID 128
#define AO_MAX_PATH 1024
#define AO_MAX_TEXT 16384
#define AO_MAX_SUBTASKS 5
#define AO_DEFAULT_REQ_MAX (1024U * 1024U)
#define AO_MAX_REQ_MAX (16U * 1024U * 1024U)

struct Runtime;
typedef struct Runtime Runtime;

typedef struct {
    char id[AO_MAX_ID];
    char name[192];
    char description[512];
    char system_prompt[AO_MAX_TEXT];
    char model_id[AO_MAX_ID];
    char tools[AO_MAX_TOOLS][AO_MAX_NAME];
    size_t tool_count;
    char skills[AO_MAX_SKILLS][AO_MAX_NAME];
    size_t skill_count;
} Agent;

typedef struct {
    char id[AO_MAX_ID];
    char name[192];
    bool enabled;
    bool auto_start;
    bool start_at_boot;
    bool is_decision;
    char executable[AO_MAX_PATH];
    char model_path[AO_MAX_PATH];
    char working_directory[AO_MAX_PATH];
    char root_dir[AO_MAX_PATH];
    char base_url[512];
    /* api_type selects the core text protocol; runtime_adapter is for companion multimodal services. */
    char api_type[32];
    char model_role[32]; /* general, multimodal, voice_interpreter */
    char runtime_adapter[48]; /* openai_compatible, llama_server_openai_compatible */
    char input_modalities[128]; /* comma-separated: text,audio,image,video */
    char output_modalities[64]; /* comma-separated: text,audio */
    char health_url[512];
    char model_name[192];
    char api_key[512];
    char node_id[AO_MAX_ID];
    char location_type[32];
    char tier[24];
    int ram_estimate_mb;
    int vram_estimate_mb;
    /* Runtime-only measurements/caches: never written back into model configuration. */
    uint64_t disk_size_bytes_cached;
    int64_t disk_cache_mtime;
    int64_t disk_cache_checked_at;
    uint64_t kv_bytes_per_token_cached; /* GGUF architecture estimate, or conservative fallback. */
    int64_t kv_metadata_checked_at;
    int planned_context_tokens;       /* Total context across parallel slots. */
    int loaded_context_tokens;
    int planned_parallel;
    int loaded_parallel;
    int active_requests;
    size_t cached_prompt_buckets[8];
    int cached_context_per_request[8];
    unsigned int context_cache_next;
    long actual_rss_mb;
    long actual_pss_mb;
    long actual_gpu_memory_mb; /* -1 when driver cannot report per-process device memory. */
    char args[AO_MAX_MODEL_ARGS][AO_MAX_PATH];
    size_t arg_count;
    int port;
    int startup_timeout_seconds;
    int shutdown_timeout_seconds;
    int request_timeout_seconds;
    int first_token_timeout_seconds;
    int stream_idle_timeout_seconds;
    int total_timeout_seconds; /* Optional hard cap; zero means no total cap. */
    double temperature;
    pid_t pid;
    bool running;
    char last_error[512];
    char log_path[AO_MAX_PATH];
} LocalModel;

typedef struct {
    char name[AO_MAX_MCP_ENV_NAME];
    char value[AO_MAX_PATH];
} MCPEnvironmentVariable;

typedef struct {
    char name[AO_MAX_NAME];
    char transport[24]; /* stdio or streamable_http */
    char command[AO_MAX_PATH];
    char url[AO_MAX_PATH];
    char bearer_token_env[128];
    bool allow_insecure_http;
    char args[AO_MAX_MCP_ARGS][AO_MAX_PATH];
    size_t arg_count;
    MCPEnvironmentVariable env[AO_MAX_MCP_ENV_VARS];
    size_t env_count;
    char read_only_paths[AO_MAX_MCP_READ_PATHS][AO_MAX_PATH];
    size_t read_only_path_count;
    char write_paths[AO_MAX_MCP_WRITE_PATHS][AO_MAX_PATH];
    size_t write_path_count;
    int timeout_seconds;
} MCPServerConfig;

typedef struct {
    MCPServerConfig config;
    pid_t pid;
    int to_child;
    int from_child;
    pthread_mutex_t mutex;
    bool mutex_initialized;
    atomic_bool started;
    pthread_t supervisor_thread;
    bool supervisor_thread_started;
    atomic_bool supervisor_stop;
    atomic_bool restart_requested;
    bool supports_tools;
    bool supports_resources;
    bool supports_resource_templates;
    bool supports_prompts;
    bool supports_completions;
    bool stateless_protocol; /* MCP protocol 2026-07-28+ */
    char protocol_version[32];
    char workspace_root[AO_MAX_PATH];
    char sandbox_dir[AO_MAX_PATH];
    int next_request_id;
    char *read_buffer;
    size_t read_length;
    size_t read_capacity;
} MCPServer;

enum {
    AO_MCP_TOOL_CALL = 0,
    AO_MCP_RESOURCES_LIST,
    AO_MCP_RESOURCE_READ,
    AO_MCP_RESOURCE_TEMPLATES_LIST,
    AO_MCP_PROMPTS_LIST,
    AO_MCP_PROMPT_GET,
    AO_MCP_COMPLETION_COMPLETE
};

typedef struct {
    char alias[AO_MAX_ID];
    char tool_name[AO_MAX_ID];
    char method[64];
    int kind;
    size_t server_index;
    char description[512];
    json_object *input_schema;
} MCPTool;

typedef struct {
    bool enabled;
    char language[32];
    char command[AO_MAX_PATH];
    char args[AO_MAX_LSP_ARGS][AO_MAX_PATH];
    size_t arg_count;
    char extensions[AO_MAX_LSP_EXTENSIONS][24];
    size_t extension_count;
    char root_dir[AO_MAX_PATH];
    int timeout_seconds;
} LSPServerConfig;

typedef struct {
    LSPServerConfig config;
    pid_t pid;
    int to_child;
    int from_child;
    pthread_mutex_t mutex;
    bool mutex_initialized;
    bool started;
    int next_request_id;
    char *read_buffer;
    size_t read_length;
    size_t read_capacity;
    void *documents;
} LSPServer;

typedef struct {
    char host[128];
    int port;
    char auth_token[256];
    size_t max_request_bytes;
    int max_connections;
    int request_timeout_seconds;
    int workers;
    int queue_max_pending;
    int max_retries;
    int priority_aging_seconds;
    bool unix_socket_enabled;
    char unix_socket_path[AO_MAX_PATH];
    bool ui_enabled;
    int ui_port;
    char ui_auth_token[256];
    char ui_dir[AO_MAX_PATH];
    bool sandbox_enabled;
    bool sandbox_require_landlock;
    bool sandbox_allow_network;
    int sandbox_cpu_seconds;
    int sandbox_memory_mb;
    int sandbox_file_size_mb;
    int sandbox_open_files;
    char database_path[AO_MAX_PATH];
    char asgend_home[AO_MAX_PATH];
    char main_config_path[AO_MAX_PATH];
    char models_config_path[AO_MAX_PATH];
    char default_model_id[AO_MAX_ID];
    char decision_model_id[AO_MAX_ID];
    char analysis_model_id[AO_MAX_ID];
    char agents_dir[AO_MAX_PATH];
    char skills_dir[AO_MAX_PATH];
    char knowledge_dir[AO_MAX_PATH];
    char system_prompts_dir[AO_MAX_PATH];
    char tools_dir[AO_MAX_PATH];
    char instructions_dir[AO_MAX_PATH];
    char root_dir[AO_MAX_PATH]; /* configuration/state root */
    char project_root[AO_MAX_PATH]; /* code/document corpus used by project memory */
    bool llm_enabled;
    char llm_base_url[512];
    char llm_model[192];
    char llm_api_key[512];
    int llm_timeout_seconds;
    double llm_temperature;
    MCPServerConfig mcp_servers[AO_MAX_MCP_SERVERS];
    size_t mcp_server_count;
    LSPServerConfig lsp_servers[AO_MAX_LSP_SERVERS];
    size_t lsp_server_count;
} Config;

typedef struct {
    Runtime *runtime;
    int fd;
    bool http_mode;
} ConnectionArgs;

typedef struct {
    char *id;
    char *task_json;
    int attempt;
} ClaimedTask;

struct Runtime {
    Config config;
    Agent agents[AO_MAX_AGENTS];
    size_t agent_count;
    LocalModel models[AO_MAX_MODELS];
    size_t model_count;
    LocalModel decision_models[AO_MAX_MODELS];
    size_t decision_model_count;
    uint64_t active_config_fingerprint;
    pthread_mutex_t models_mutex;
    pthread_mutex_t model_scheduler_mutex;
    bool models_mutex_initialized;
    bool model_scheduler_mutex_initialized;
    sqlite3 *db;
    pthread_mutex_t db_mutex;
    pthread_mutex_t queue_mutex;
    pthread_cond_t queue_cond;
    /* Monotonic in-process revision + condition variable for cross-client plan updates. */
    pthread_mutex_t plan_mutex;
    pthread_cond_t plan_cond;
    uint64_t plan_revision;
    bool plan_sync_initialized;
    pthread_mutex_t connections_mutex;
    int active_connections;
    int connection_fds[4096];
    MCPServer mcp_servers[AO_MAX_MCP_SERVERS];
    size_t mcp_server_count;
    MCPTool mcp_tools[AO_MAX_MCP_TOOLS];
    size_t mcp_tool_count;
    LSPServer lsp_servers[AO_MAX_LSP_SERVERS];
    size_t lsp_server_count;
    volatile sig_atomic_t stopping;
    pthread_t *workers;
    int worker_count;
    int listen_fd;
    int unix_listen_fd;
    int ui_listen_fd;
};

int config_load(Config *cfg, const char *path, char *err, size_t err_size);
int agents_load(Runtime *rt, char *err, size_t err_size);
int models_load(Runtime *rt, char *err, size_t err_size);
int model_set_enabled(Runtime *rt, const char *id, bool decision, bool enabled, char *err, size_t err_size);
int resources_init_instructions(Runtime *rt, char *err, size_t err_size);
int resources_prepare_agent(Runtime *rt, const char *task, const char *task_id,
                            const Agent *base, Agent *prepared, json_object **selection,
                            char *err, size_t err_size);
json_object *resources_status_json(Runtime *rt);
uint64_t config_editor_fingerprint(Runtime *rt);
json_object *config_editor_get(Runtime *rt, char *err, size_t err_size);
int config_editor_save(Runtime *rt, json_object *body, char *err, size_t err_size);
int config_editor_update_settings(Runtime *rt, json_object *request, char *err, size_t err_size);
int models_start_autostart(Runtime *rt, char *err, size_t err_size);
void models_stop_all(Runtime *rt);
int curl_runtime_init(void);
int model_start(Runtime *rt, const char *id, bool decision, char *err, size_t err_size);
int model_stop(Runtime *rt, const char *id, bool decision, char *err, size_t err_size);
const LocalModel *model_find(const Runtime *rt, const char *id, bool decision);
bool model_available(Runtime *rt, const char *id, bool decision);
bool model_configured_enabled(Runtime *rt, const char *id, bool decision);
int model_request_acquire(Runtime *rt, const char *id, bool decision,
                          json_object *messages, json_object *tools,
                          char *err, size_t err_size);
void model_request_release(Runtime *rt, const char *id, bool decision);
const char *models_choose_analysis_model(Runtime *rt, char *out, size_t out_size);
const char *models_choose_task_model(Runtime *rt, const char *tier, char *out, size_t out_size);
json_object *models_resource_snapshot_json(Runtime *rt);
json_object *models_status_json(Runtime *rt);
const Agent *agent_find(const Runtime *rt, const char *id);
const Agent *agent_choose(const Runtime *rt, const char *task, const char *requested_id);

int db_init(Runtime *rt, char *err, size_t err_size);
void db_close(Runtime *rt);
int db_recover_running(Runtime *rt, char *err, size_t err_size);
int db_enqueue(Runtime *rt, const char *task_json, const char *session_id,
               const char *idempotency_key, int priority, char *task_id, size_t task_id_size,
               bool *duplicate, char *err, size_t err_size);
int db_claim_next(Runtime *rt, ClaimedTask *claim, char *err, size_t err_size);
void db_claim_free(ClaimedTask *claim);
int db_task_cancelled(Runtime *rt, const char *task_id);
int db_finish(Runtime *rt, const char *task_id, int attempt, bool success,
              const char *result_json, const char *error, bool *retry,
              char *err, size_t err_size);
int db_release_claim(Runtime *rt, const char *task_id, int attempt, char *err, size_t err_size);
int db_retry_failed_task(Runtime *rt, const char *task_id, char *err, size_t err_size);
int db_cancel_task(Runtime *rt, const char *task_id, char *err, size_t err_size);
json_object *db_task_status(Runtime *rt, const char *task_id, char *err, size_t err_size);
int db_resolve_task_id(Runtime *rt, const char *prefix, char *resolved, size_t resolved_size, char *err, size_t err_size);
int db_task_workspace_path(Runtime *rt, const char *task_id, char *path, size_t path_size);
int db_task_set_title(Runtime *rt, const char *task_id, const char *title, char *err, size_t err_size);
json_object *db_queue_health(Runtime *rt, char *err, size_t err_size);
json_object *db_list_tasks(Runtime *rt, const char *status_filter, int limit, char *err, size_t err_size);
int db_append_session_message(Runtime *rt, const char *session_id, const char *task_id,
                              const char *role, const char *content);
json_object *db_session_context(Runtime *rt, const char *session_id, const char *exclude_task_id);
int db_append_task_event(Runtime *rt, const char *task_id, const char *event_type,
                         const char *actor, const char *message, json_object *payload);
json_object *db_task_events(Runtime *rt, const char *task_id, int after_id, int limit,
                            char *err, size_t err_size);
json_object *db_plan_list(Runtime *rt, const char *scope_id, const char *kind, char *err, size_t err_size);
int db_plan_add(Runtime *rt, const char *scope_id, const char *kind, const char *title,
                const char *description, int64_t *item_id, char *err, size_t err_size);
int db_plan_update(Runtime *rt, const char *scope_id, int64_t item_id, const char *title,
                   const char *description, const char *status, char *err, size_t err_size);
int db_plan_delete(Runtime *rt, const char *scope_id, int64_t item_id, char *err, size_t err_size);
int db_answer_task(Runtime *rt, const char *task_id, const char *answer, const char *option_id,
                   char *err, size_t err_size);

int server_run(Runtime *rt, char *err, size_t err_size);
json_object *server_handle_request(Runtime *rt, json_object *request);
void server_request_stop(Runtime *rt);
void *worker_main(void *arg);

int orchestrator_execute(Runtime *rt, const char *task_id, const char *request_json,
                         char **result_json, bool *success, char *status,
                         size_t status_size, char *err, size_t err_size);
void orchestrator_diagnose_failure(Runtime *rt, const char *task_id, const char *request_json, const char *error);
int agent_execute(Runtime *rt, const Agent *agent, const char *task_id,
                  const char *session_id, const char *original_task, const char *instruction,
                  char **output, bool *model_used, char *err, size_t err_size);
json_object *llm_chat(Runtime *rt, json_object *messages, json_object *tools,
                      char *err, size_t err_size);
json_object *llm_chat_model(Runtime *rt, const char *model_id, bool decision,
                            json_object *messages, json_object *tools, char *err, size_t err_size);
void llm_set_active_task(const char *task_id);
const char *llm_get_active_task(void);
void llm_set_stream_context(const char *kind);
json_object *tools_schemas_for_agent(Runtime *rt, const Agent *agent);
int mcp_start_all(Runtime *rt, char *err, size_t err_size);
void mcp_stop_all(Runtime *rt);
/* Internal single-threaded exec mode used to prepare an MCP stdio child. */
int mcp_launcher_main(int control_fd);
char *mcp_call_tool(Runtime *rt, const char *alias, json_object *arguments, char *err, size_t err_size);
json_object *mcp_list_resources(Runtime *rt, const char *server_name, char *err, size_t err_size);
json_object *mcp_read_resource(Runtime *rt, const char *server_name, const char *uri, char *err, size_t err_size);
json_object *mcp_list_prompts(Runtime *rt, const char *server_name, char *err, size_t err_size);
json_object *mcp_get_prompt(Runtime *rt, const char *server_name, const char *name, json_object *arguments, char *err, size_t err_size);
char *tool_execute(Runtime *rt, const Agent *agent, const char *name,
                   json_object *arguments, char *err, size_t err_size);
char *lsp_run_diagnostics(Runtime *rt, const char *file_path, char *err, size_t err_size);
int lsp_init_all(Runtime *rt, char *err, size_t err_size);
void lsp_stop_all(Runtime *rt);

int path_mkdirs(const char *path, unsigned mode);
char *string_duplicate(const char *s);
int string_copy(char *dst, size_t dst_size, const char *src);
int json_get_string(json_object *obj, const char *key, const char **out);
int json_to_string(json_object *obj, char **out);
void runtime_signal_stop(Runtime *rt);

#endif
