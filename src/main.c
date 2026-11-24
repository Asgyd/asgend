#include "runtime.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>

static Runtime *signal_runtime = NULL;

static void handle_signal(int signo) {
    (void)signo;
    if (signal_runtime) signal_runtime->stopping = 1;
}

static void usage(const char *program) {
    fprintf(stderr, "Usage: %s [-c config.json] [--check-config] [--version]\n", program);
}

int main(int argc, char **argv) {
    /* MCP sandbox/environment setup runs in a fresh single-threaded image,
       not in the post-fork child of the multithreaded service. */
    if (argc == 2 && strcmp(argv[1], "--mcp-launcher") == 0)
        return mcp_launcher_main(3);
    (void)setlocale(LC_CTYPE, "");
    const char *config_path = "config/config.json";
    bool check_only = false;
    static const struct option long_options[] = {
        {"config", required_argument, NULL, 'c'},
        {"check-config", no_argument, NULL, 'k'},
        {"version", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "c:kvh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'c': config_path = optarg; break;
            case 'k': check_only = true; break;
            case 'v': puts("agent-orchestrator-c " AGEND_VERSION); return 0;
            case 'h': usage(argv[0]); return 0;
            default: usage(argv[0]); return 2;
        }
    }
    /* Runtime contains fixed-size registries and is too large for the default
       8 MiB process stack on some configurations. Keep it on the heap. */
    Runtime *rt = calloc(1, sizeof(*rt));
    if (!rt) { fprintf(stderr, "fatal: cannot allocate runtime\n"); return 1; }
    rt->listen_fd = -1;
    rt->unix_listen_fd = -1;
    rt->ui_listen_fd = -1;
    if (pthread_mutex_init(&rt->db_mutex, NULL) != 0 ||
        pthread_mutex_init(&rt->queue_mutex, NULL) != 0 ||
        pthread_cond_init(&rt->queue_cond, NULL) != 0 ||
        pthread_mutex_init(&rt->plan_mutex, NULL) != 0 ||
        pthread_cond_init(&rt->plan_cond, NULL) != 0 ||
        pthread_mutex_init(&rt->connections_mutex, NULL) != 0 ||
        pthread_mutex_init(&rt->models_mutex, NULL) != 0 ||
        pthread_mutex_init(&rt->model_scheduler_mutex, NULL) != 0) {
        fprintf(stderr, "fatal: cannot initialize synchronization primitives\n");
        free(rt); return 1;
    }
    rt->plan_sync_initialized = true;
    rt->models_mutex_initialized = true;
    rt->model_scheduler_mutex_initialized = true;
    char err[2048] = {0};
    if (config_load(&rt->config, config_path, err, sizeof(err)) != 0) {
        fprintf(stderr, "configuration error: %s\n", err);
        goto fail;
    }
    /* Daemons must not keep a deleted interactive-shell CWD. Shell-based probes
       (for example the optional nvidia-smi check) otherwise print getcwd errors.
       Configured runtime paths are already resolved, so this fallback is safe. */
    char working_directory[AO_MAX_PATH];
    if (!getcwd(working_directory, sizeof(working_directory))) {
        if (chdir(rt->config.root_dir) != 0 && chdir("/") != 0) {
            fprintf(stderr, "warning: cannot recover from unavailable working directory: %s\n", strerror(errno));
        }
    }
    if (agents_load(rt, err, sizeof(err)) != 0) {
        fprintf(stderr, "agent configuration error: %s\n", err);
        goto fail;
    }
    if (models_load(rt, err, sizeof(err)) != 0) {
        fprintf(stderr, "model configuration error: %s\n", err);
        goto fail;
    }
    rt->active_config_fingerprint = config_editor_fingerprint(rt);
    if (check_only) {
        printf("Configuration OK. Agents loaded: %zu\n", rt->agent_count);
        for (size_t i = 0; i < rt->agent_count; ++i) printf("- %s (%s)\n", rt->agents[i].id, rt->agents[i].name);
        goto success;
    }
    /* Initialize libcurl before creating worker/listener threads. Managed model
       processes may be started lazily later from worker threads. */
    if (resources_init_instructions(rt, err, sizeof(err)) != 0) {
        fprintf(stderr, "warning: could not initialize user instructions directory: %s\n", err);
    }
    if (curl_runtime_init() != 0) {
        fprintf(stderr, "fatal: cannot initialize HTTP client runtime\n");
        goto fail;
    }
    if (db_init(rt, err, sizeof(err)) != 0) {
        fprintf(stderr, "database initialization error: %s\n", err);
        goto fail;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGTERM, &sa, NULL);
    (void)signal(SIGPIPE, SIG_IGN);
    signal_runtime = rt;
    if (models_start_autostart(rt, err, sizeof(err)) != 0) {
        fprintf(stderr, "model supervisor initialization error: %s\n", err);
        goto fail;
    }
    if (mcp_start_all(rt, err, sizeof(err)) != 0) {
        fprintf(stderr, "MCP initialization error: %s\n", err[0] ? err : "unknown error");
        goto fail;
    }
    if (lsp_init_all(rt, err, sizeof(err)) != 0) {
        fprintf(stderr, "LSP initialization error: %s\n", err[0] ? err : "unknown error");
        goto fail;
    }
    int rc = server_run(rt, err, sizeof(err));
    lsp_stop_all(rt);
    mcp_stop_all(rt);
    models_stop_all(rt);
    signal_runtime = NULL;
    if (rc != 0) {
        fprintf(stderr, "server error: %s\n", err[0] ? err : "unknown error");
        goto fail;
    }
    db_close(rt);
    goto success;

fail:
    signal_runtime = NULL;
    lsp_stop_all(rt);
    mcp_stop_all(rt);
    models_stop_all(rt);
    db_close(rt);
    free(rt->workers);
    pthread_mutex_destroy(&rt->db_mutex);
    pthread_mutex_destroy(&rt->queue_mutex);
    pthread_cond_destroy(&rt->queue_cond);
    if (rt->plan_sync_initialized) {
        pthread_mutex_destroy(&rt->plan_mutex);
        pthread_cond_destroy(&rt->plan_cond);
    }
    pthread_mutex_destroy(&rt->connections_mutex);
    if (rt->models_mutex_initialized) pthread_mutex_destroy(&rt->models_mutex);
    if (rt->model_scheduler_mutex_initialized) pthread_mutex_destroy(&rt->model_scheduler_mutex);
    free(rt);
    return 1;

success:
    pthread_mutex_destroy(&rt->db_mutex);
    pthread_mutex_destroy(&rt->queue_mutex);
    pthread_cond_destroy(&rt->queue_cond);
    if (rt->plan_sync_initialized) {
        pthread_mutex_destroy(&rt->plan_mutex);
        pthread_cond_destroy(&rt->plan_cond);
    }
    pthread_mutex_destroy(&rt->connections_mutex);
    if (rt->models_mutex_initialized) pthread_mutex_destroy(&rt->models_mutex);
    if (rt->model_scheduler_mutex_initialized) pthread_mutex_destroy(&rt->model_scheduler_mutex);
    free(rt);
    return 0;
}
