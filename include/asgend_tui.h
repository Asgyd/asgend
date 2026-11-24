#ifndef ASGEND_TUI_H
#define ASGEND_TUI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <termios.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASGEND_TUI_TEXT_MAX 16384
#define ASGEND_TUI_TITLE_MAX 160
#define ASGEND_TUI_KEY_MAX 96
#define ASGEND_TUI_PATH_MAX 256
#define ASGEND_TUI_ITEMS_MAX 64
#define ASGEND_TUI_MCP_MAX 32
#define ASGEND_TUI_QUESTION_OPTIONS_MAX 8
#define ASGEND_TUI_INPUT_MAX 1024
#define ASGEND_TUI_KEY_NAVIGATION 0x10000
#define ASGEND_TUI_KEY_ESCAPE 0x10001
#define ASGEND_TUI_KEY_MOUSE 0x10002
#define ASGEND_TUI_KEY_RESIZE 0x10003
#define ASGEND_TUI_KEY_PAGE_UP 0x10004
#define ASGEND_TUI_KEY_PAGE_DOWN 0x10005

typedef enum {
    ASGEND_TUI_DISPLAY_DYNAMIC = 0,
    ASGEND_TUI_DISPLAY_ALL_EXPANDED = 1,
    ASGEND_TUI_DISPLAY_ALL_COLLAPSED = 2
} AsgendTuiDisplayMode;

typedef enum {
    ASGEND_TUI_TILE_CHAT = 0,
    ASGEND_TUI_TILE_ACTION,
    ASGEND_TUI_TILE_TOOL,
    ASGEND_TUI_TILE_THINKING,
    ASGEND_TUI_TILE_FILE,
    ASGEND_TUI_TILE_PROGRESS,
    ASGEND_TUI_TILE_PLAN
} AsgendTuiTileKind;

typedef struct {
    char key[ASGEND_TUI_KEY_MAX];
    char title[ASGEND_TUI_TITLE_MAX];
    char *body;
    AsgendTuiTileKind kind;
    bool expanded;
    bool streaming;
    bool body_truncated;
    size_t bytes;
    uint64_t sequence;
} AsgendTuiTile;

typedef struct {
    int64_t id;
    char title[ASGEND_TUI_TITLE_MAX];
    char status[24];
    char kind[8];
} AsgendTuiPlanItem;

typedef struct {
    char id[ASGEND_TUI_KEY_MAX];
    char title[ASGEND_TUI_TITLE_MAX];
    char status[24];
    char created_at[40];
} AsgendTuiTaskItem;

typedef struct {
    char path[ASGEND_TUI_PATH_MAX];
    char kind[16];
    int64_t size;
} AsgendTuiFileItem;

typedef struct {
    char name[ASGEND_TUI_TITLE_MAX];
    char status[24];
    char transport[24];
    bool connected;
} AsgendTuiMcpItem;

typedef struct AsgendTui {
    bool active;
    bool suspended;
    unsigned render_defer_depth;
    bool render_pending;
    bool follow_stream_tail;
    bool follow_stream_tail_pending;
    bool terminal_saved;
    struct termios original_termios;
    struct termios active_termios;
    AsgendTuiDisplayMode display_mode;
    AsgendTuiTile *tiles;
    size_t tile_count;
    size_t tile_capacity;
    size_t selected_tile;
    size_t selected_task;
    bool welcome_screen;
    bool session_picker_active;
    char session_query[128];
    size_t session_selected;
    size_t session_scroll_offset;
    size_t session_filtered_indices[ASGEND_TUI_ITEMS_MAX];
    size_t session_filtered_count;
    size_t welcome_filtered_indices[ASGEND_TUI_ITEMS_MAX];
    size_t welcome_filtered_count;
    size_t welcome_scroll_offset;
    size_t welcome_visible_count;
    int welcome_start_row;
    int session_modal_x;
    int session_modal_width;
    int session_modal_top;
    int session_modal_list_top;
    size_t session_modal_visible_count;
    int session_cursor_row;
    int session_cursor_col;
    size_t selected_left_index;
    size_t left_scroll_offset;
    size_t scroll_offset;
    int focused_pane;
    uint64_t next_sequence;
    uint64_t dropped_tile_count;
    bool history_truncated;
    unsigned spinner_frame;
    char header[160];
    char task_id[ASGEND_TUI_KEY_MAX];
    char status[128];
    AsgendTuiTaskItem tasks[ASGEND_TUI_ITEMS_MAX];
    size_t task_count;
    AsgendTuiPlanItem todos[ASGEND_TUI_ITEMS_MAX];
    size_t todo_count;
    AsgendTuiPlanItem goals[ASGEND_TUI_ITEMS_MAX];
    size_t goal_count;
    AsgendTuiFileItem files[ASGEND_TUI_ITEMS_MAX];
    size_t file_count;
    size_t selected_file_index;
    size_t file_scroll_offset;
    bool right_pane_goals;
    char workspace_path[ASGEND_TUI_PATH_MAX];
    char project_path[ASGEND_TUI_PATH_MAX];
    AsgendTuiMcpItem mcp_servers[ASGEND_TUI_MCP_MAX];
    size_t mcp_server_count;
    char input_line[ASGEND_TUI_INPUT_MAX];
    char input_prompt[160];
    size_t input_length;
    bool input_mode;
    char question_text[512];
    char question_title[160];
    char question_options[ASGEND_TUI_QUESTION_OPTIONS_MAX][ASGEND_TUI_TITLE_MAX];
    size_t question_option_count;
    size_t question_selected;
    bool question_active;
    char preview_path[ASGEND_TUI_PATH_MAX];
    char *preview_content;
    size_t preview_length;
    size_t preview_line_offset;
    size_t preview_horizontal_offset;
    int64_t preview_file_size;
    int64_t preview_chunk_offset;
    int64_t preview_next_offset;
    bool preview_eof;
    bool preview_no_wrap;
    int preview_scroll_direction;
    size_t selected_plan;
    bool selected_plan_is_goal;
    int width;
    int height;
    int left_width;
    int center_width;
    int right_width;
    int content_rows;
} AsgendTui;

/* A fully standalone ANSI/termios TUI library: no JSON, networking or core dependency. */
int asgend_tui_init(AsgendTui *tui, const char *header);
void asgend_tui_shutdown(AsgendTui *tui);
void asgend_tui_suspend(AsgendTui *tui);
void asgend_tui_resume(AsgendTui *tui);
void asgend_tui_render(AsgendTui *tui);
/* Batch model/event mutations into one incremental terminal frame. */
void asgend_tui_batch_begin(AsgendTui *tui);
void asgend_tui_batch_end(AsgendTui *tui);
void asgend_tui_set_header(AsgendTui *tui, const char *header);
void asgend_tui_clear_tiles(AsgendTui *tui);
void asgend_tui_set_task(AsgendTui *tui, const char *task_id, const char *status);
void asgend_tui_set_tasks(AsgendTui *tui, const AsgendTuiTaskItem *tasks, size_t count);
void asgend_tui_set_welcome(AsgendTui *tui, bool enabled);
void asgend_tui_open_session_picker(AsgendTui *tui, const char *initial_query);
void asgend_tui_close_session_picker(AsgendTui *tui);
bool asgend_tui_session_picker_active(const AsgendTui *tui);
bool asgend_tui_welcome_active(const AsgendTui *tui);
bool asgend_tui_selected_session(AsgendTui *tui, AsgendTuiTaskItem *out);
bool asgend_tui_session_query_append(AsgendTui *tui, const char *text);
void asgend_tui_session_query_backspace(AsgendTui *tui);
void asgend_tui_set_plan(AsgendTui *tui, const AsgendTuiPlanItem *items, size_t count);
void asgend_tui_set_files(AsgendTui *tui, const AsgendTuiFileItem *files, size_t count);
void asgend_tui_set_workspace_path(AsgendTui *tui, const char *path);
void asgend_tui_set_project_path(AsgendTui *tui, const char *path);
void asgend_tui_set_mcp_servers(AsgendTui *tui, const AsgendTuiMcpItem *items, size_t count);
void asgend_tui_input_begin(AsgendTui *tui);
void asgend_tui_input_cancel(AsgendTui *tui);
bool asgend_tui_input_append(AsgendTui *tui, const char *text);
void asgend_tui_input_backspace(AsgendTui *tui);
char *asgend_tui_input_take(AsgendTui *tui);
void asgend_tui_set_question(AsgendTui *tui, const char *question,
                             const char *const *options, size_t option_count);
void asgend_tui_set_question_title(AsgendTui *tui, const char *title);
void asgend_tui_clear_question(AsgendTui *tui);
bool asgend_tui_question_active(const AsgendTui *tui);
const char *asgend_tui_question_answer(const AsgendTui *tui);
const char *asgend_tui_workspace_path(const AsgendTui *tui);
bool asgend_tui_selected_file(AsgendTui *tui, AsgendTuiFileItem *out);
bool asgend_tui_enter_selected_directory(AsgendTui *tui);
bool asgend_tui_parent_directory(AsgendTui *tui);
void asgend_tui_show_file_preview(AsgendTui *tui, const char *path, const char *content);
void asgend_tui_show_file_preview_chunk(AsgendTui *tui, const char *path, const char *content,
                                        int64_t file_size, int64_t chunk_offset,
                                        int64_t next_offset, bool eof, bool start_at_end);
void asgend_tui_close_file_preview(AsgendTui *tui);
/* Returns -1/+1 when the current preview should stream the previous/next chunk. */
int asgend_tui_preview_stream_action(AsgendTui *tui);
/* If append is true, body is appended to the existing tile with the same key. */
int asgend_tui_upsert_tile(AsgendTui *tui, const char *key, AsgendTuiTileKind kind,
                           const char *title, const char *body, bool append,
                           bool streaming, bool expanded);
void asgend_tui_set_display_mode(AsgendTui *tui, AsgendTuiDisplayMode mode);
void asgend_tui_toggle_selected(AsgendTui *tui);
void asgend_tui_move_selection(AsgendTui *tui, int direction);
/* Returns a key code/ASCII byte if input is ready, zero if not. Call during event polling. */
int asgend_tui_poll_key(AsgendTui *tui);
/* Advance visible activity indicators; only dirty rows are emitted. */
void asgend_tui_tick(AsgendTui *tui);
/* Selected plan item helper; returns false when no item is selected. */
bool asgend_tui_selected_plan(AsgendTui *tui, AsgendTuiPlanItem *out);
bool asgend_tui_selected_task(AsgendTui *tui, AsgendTuiTaskItem *out);

#ifdef __cplusplus
}
#endif
#endif
