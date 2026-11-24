#include "asgend_tui.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc == 2 && (strcmp(argv[1], "--render") == 0 ||
                      strcmp(argv[1], "--render-welcome") == 0 ||
                      strcmp(argv[1], "--render-session") == 0)) {
        bool render_welcome = strcmp(argv[1], "--render-welcome") == 0;
        bool render_session = strcmp(argv[1], "--render-session") == 0;
        AsgendTui render_tui;
        int render_status = asgend_tui_init(&render_tui, "terminal layout test");
        if (render_status < 0) return 1;
        if (render_status == 1) {
            asgend_tui_set_header(&render_tui,
                "Long workspace header for terminal clipping and geometry checks");
            asgend_tui_set_task(&render_tui, "task-12345678901234567890",
                "A deliberately long status string to verify UTF-8-safe clipping in the TUI");

            AsgendTuiTaskItem tasks[ASGEND_TUI_ITEMS_MAX] = {0};
            AsgendTuiPlanItem plan[ASGEND_TUI_ITEMS_MAX] = {0};
            AsgendTuiFileItem files[ASGEND_TUI_ITEMS_MAX] = {0};
            for (size_t i = 0; i < ASGEND_TUI_ITEMS_MAX; ++i) {
                (void)snprintf(tasks[i].id, sizeof(tasks[i].id), "task-%02zu", i);
                (void)snprintf(tasks[i].title, sizeof(tasks[i].title),
                               "Task %02zu with a deliberately long title for narrow terminal panes", i);
                (void)snprintf(tasks[i].status, sizeof(tasks[i].status), "%s",
                               i % 2 == 0 ? "running" : "queued");
                (void)snprintf(tasks[i].created_at, sizeof(tasks[i].created_at), "%s", "2026-10-21T10:15:00Z");
                plan[i].id = (int64_t)i + 1;
                (void)snprintf(plan[i].kind, sizeof(plan[i].kind), "%s",
                               i % 2 == 0 ? "todo" : "goal");
                if (i == 0) {
                    (void)snprintf(plan[i].title, sizeof(plan[i].title), "%s", "TUI_TWO_CLIENT_SYNC_TODO");
                } else if (i == 1) {
                    (void)snprintf(plan[i].title, sizeof(plan[i].title), "%s", "TUI_TWO_CLIENT_SYNC_GOAL");
                } else {
                    (void)snprintf(plan[i].title, sizeof(plan[i].title),
                                   "Plan item %02zu with a long title for responsive layout", i);
                }
                (void)snprintf(plan[i].status, sizeof(plan[i].status), "%s",
                               i % 3 == 0 ? "done" : "open");
                (void)snprintf(files[i].path, sizeof(files[i].path),
                               "long_filename_%02zu_for_terminal_layout_checks.txt", i);
                (void)snprintf(files[i].kind, sizeof(files[i].kind), "file");
                files[i].size = (int64_t)i * 4096;
            }
            asgend_tui_set_tasks(&render_tui, tasks, ASGEND_TUI_ITEMS_MAX);
            asgend_tui_set_plan(&render_tui, plan, ASGEND_TUI_ITEMS_MAX);
            asgend_tui_set_files(&render_tui, files, ASGEND_TUI_ITEMS_MAX);
            render_tui.focused_pane = 0;
            render_tui.selected_left_index = ASGEND_TUI_ITEMS_MAX - 1;
            if (render_welcome) {
                asgend_tui_set_welcome(&render_tui, true);
                render_tui.selected_task = 0;
                asgend_tui_render(&render_tui);
            } else if (render_session) {
                asgend_tui_open_session_picker(&render_tui, "Task 02");
            } else {
                asgend_tui_render(&render_tui);
            }
            render_tui.focused_pane = 2;
            render_tui.right_pane_goals = false;
            asgend_tui_render(&render_tui);
            render_tui.right_pane_goals = true;
            render_tui.selected_plan_is_goal = false;
            render_tui.selected_plan = 0;
            asgend_tui_render(&render_tui);
        }
        asgend_tui_shutdown(&render_tui);
        return render_status == 1 ? 0 : 2;
    }

    AsgendTui tui;
    int initialized = asgend_tui_init(&tui, "test workspace");
    assert(initialized >= 0);
    assert(tui.focused_pane == 1); /* Chat is focused when the TUI opens. */

    AsgendTuiTaskItem tasks[1] = {0};
    (void)snprintf(tasks[0].id, sizeof(tasks[0].id), "%s", "task123abc");
    (void)snprintf(tasks[0].title, sizeof(tasks[0].title), "%s", "Task fixture");
    (void)snprintf(tasks[0].status, sizeof(tasks[0].status), "%s", "running");
    (void)snprintf(tasks[0].created_at, sizeof(tasks[0].created_at), "%s", "2026-10-21T10:15:00Z");
    asgend_tui_set_tasks(&tui, tasks, 1);
    asgend_tui_set_welcome(&tui, true);
    assert(asgend_tui_welcome_active(&tui));
    AsgendTuiTaskItem selected_session;
    assert(asgend_tui_selected_session(&tui, &selected_session));
    assert(strcmp(selected_session.id, "task123abc") == 0);
    asgend_tui_open_session_picker(&tui, "fixture");
    assert(asgend_tui_session_picker_active(&tui));
    assert(asgend_tui_selected_session(&tui, &selected_session));
    assert(strcmp(selected_session.title, "Task fixture") == 0);
    (void)asgend_tui_session_query_append(&tui, "missing");
    assert(!asgend_tui_selected_session(&tui, &selected_session));
    asgend_tui_close_session_picker(&tui);
    assert(tui.focused_pane == 1);
    asgend_tui_set_welcome(&tui, false);
    asgend_tui_input_begin(&tui);
    assert(tui.input_mode && tui.focused_pane == 1);
    asgend_tui_input_cancel(&tui);

    AsgendTuiPlanItem plan[2] = {0};
    plan[0].id = 1;
    (void)snprintf(plan[0].kind, sizeof(plan[0].kind), "%s", "todo");
    (void)snprintf(plan[0].title, sizeof(plan[0].title), "%s", "write tests");
    (void)snprintf(plan[0].status, sizeof(plan[0].status), "%s", "open");
    plan[1].id = 2;
    (void)snprintf(plan[1].kind, sizeof(plan[1].kind), "%s", "goal");
    (void)snprintf(plan[1].title, sizeof(plan[1].title), "%s", "ship release");
    (void)snprintf(plan[1].status, sizeof(plan[1].status), "%s", "open");
    asgend_tui_set_plan(&tui, plan, 2);

    AsgendTuiFileItem files[3] = {0};
    /* Supply filesystem order deliberately unsorted: file first, then z/src. */
    (void)snprintf(files[0].path, sizeof(files[0].path), "%s", "README.md");
    (void)snprintf(files[0].kind, sizeof(files[0].kind), "%s", "file");
    files[0].size = 2048;
    (void)snprintf(files[1].path, sizeof(files[1].path), "%s", "zeta");
    (void)snprintf(files[1].kind, sizeof(files[1].kind), "%s", "directory");
    (void)snprintf(files[2].path, sizeof(files[2].path), "%s", "src");
    (void)snprintf(files[2].kind, sizeof(files[2].kind), "%s", "directory");
    asgend_tui_set_files(&tui, files, 3);
    assert(tui.file_count == 3);
    assert(strcmp(tui.files[0].path, "src") == 0 && strcmp(tui.files[0].kind, "directory") == 0);
    assert(strcmp(tui.files[1].path, "zeta") == 0 && strcmp(tui.files[1].kind, "directory") == 0);
    assert(strcmp(tui.files[2].path, "README.md") == 0 && strcmp(tui.files[2].kind, "file") == 0);
    AsgendTuiFileItem selected_file;
    tui.selected_file_index = 2;
    assert(asgend_tui_selected_file(&tui, &selected_file));
    assert(strcmp(selected_file.path, "README.md") == 0);
    tui.selected_file_index = 0;
    assert(asgend_tui_enter_selected_directory(&tui));
    assert(strcmp(asgend_tui_workspace_path(&tui), "src") == 0);
    assert(asgend_tui_parent_directory(&tui));
    assert(strcmp(asgend_tui_workspace_path(&tui), "") == 0);
    assert(!asgend_tui_parent_directory(&tui));
    asgend_tui_set_workspace_path(&tui, "src/lib");
    tui.selected_file_index = 2;
    assert(asgend_tui_selected_file(&tui, &selected_file));
    assert(strcmp(selected_file.path, "src/lib/README.md") == 0);
    asgend_tui_show_file_preview(&tui, selected_file.path, "line one\nline two\n");
    assert(tui.preview_content && strcmp(tui.preview_path, "src/lib/README.md") == 0);
    assert(tui.preview_length == strlen("line one\nline two\n"));
    asgend_tui_show_file_preview_chunk(&tui, "large.log", "row-0\nrow-1\nrow-2\n",
                                       5000000, 32768, 32786, false, false);
    assert(tui.preview_file_size == 5000000 && tui.preview_chunk_offset == 32768);
    assert(tui.preview_next_offset == 32786 && !tui.preview_eof);
    tui.center_width = 60;
    tui.content_rows = 10;
    tui.preview_scroll_direction = 1;
    tui.preview_line_offset = 3;
    assert(asgend_tui_preview_stream_action(&tui) == 1);
    tui.preview_scroll_direction = -1;
    tui.preview_line_offset = 0;
    assert(asgend_tui_preview_stream_action(&tui) == -1);
    asgend_tui_close_file_preview(&tui);
    assert(tui.preview_content == NULL);
    asgend_tui_set_workspace_path(&tui, "");
    tui.focused_pane = 0;

    assert(asgend_tui_upsert_tile(&tui, "assistant:1", ASGEND_TUI_TILE_CHAT,
                                  "Assistant", "Hello", false, true, true) == 0);
    assert(asgend_tui_upsert_tile(&tui, "assistant:1", ASGEND_TUI_TILE_CHAT,
                                  "Assistant", " world", true, false, true) == 0);
    assert(tui.tile_count == 1);
    assert(strcmp(tui.tiles[0].body, "Hello world") == 0);

    asgend_tui_set_display_mode(&tui, ASGEND_TUI_DISPLAY_ALL_COLLAPSED);
    assert(!tui.tiles[0].expanded);
    asgend_tui_set_display_mode(&tui, ASGEND_TUI_DISPLAY_ALL_EXPANDED);
    assert(tui.tiles[0].expanded);
    asgend_tui_set_display_mode(&tui, ASGEND_TUI_DISPLAY_DYNAMIC);
    assert(asgend_tui_upsert_tile(&tui, "tool:1", ASGEND_TUI_TILE_TOOL,
                                  "Tool call", "running", true, false, true) == 0);
    assert(!tui.tiles[1].expanded);

    AsgendTuiTaskItem selected_task;
    assert(asgend_tui_selected_task(&tui, &selected_task));
    assert(strcmp(selected_task.id, "task123abc") == 0);
    /* The redesigned workspace keeps project files on the left and plan on the right. */
    tui.focused_pane = 2;
    tui.selected_plan_is_goal = false;
    tui.selected_plan = 0;
    AsgendTuiPlanItem selected_plan;
    assert(asgend_tui_selected_plan(&tui, &selected_plan));
    assert(selected_plan.id == 1 && strcmp(selected_plan.kind, "todo") == 0);
    tui.focused_pane = 2;
    tui.right_pane_goals = true;
    tui.selected_plan_is_goal = true;
    tui.selected_plan = 0;
    assert(asgend_tui_selected_plan(&tui, &selected_plan));
    assert(selected_plan.id == 2 && strcmp(selected_plan.kind, "goal") == 0);

    asgend_tui_clear_tiles(&tui);
    assert(tui.tile_count == 0);

    /* Long body content must be visibly marked as truncated, and retain its tail. */
    size_t large_size = (size_t)ASGEND_TUI_TEXT_MAX * 8U + 8192U;
    char *large_body = malloc(large_size + 1);
    assert(large_body != NULL);
    memset(large_body, 'x', large_size);
    memcpy(large_body + large_size - 12, "RECENT-TAIL", 11);
    large_body[large_size] = '\0';
    assert(asgend_tui_upsert_tile(&tui, "large-body", ASGEND_TUI_TILE_CHAT,
                                  "Long answer", large_body, false, true, true) == 0);
    free(large_body);
    assert(tui.tile_count == 1 && tui.tiles[0].body_truncated);
    assert(strstr(tui.tiles[0].body, "начало вывода скрыто") != NULL);
    assert(strstr(tui.tiles[0].body, "RECENT-TAIL") != NULL);
    assert(tui.tiles[0].bytes <= (size_t)ASGEND_TUI_TEXT_MAX * 8U);

    /* Eviction prefers completed non-chat tiles and never evicts active streams. */
    asgend_tui_clear_tiles(&tui);
    assert(asgend_tui_upsert_tile(&tui, "active-stream", ASGEND_TUI_TILE_CHAT,
                                  "Model stream", "live", false, true, true) == 0);
    char tile_key[ASGEND_TUI_KEY_MAX];
    for (size_t i = 1; i < 512; ++i) {
        (void)snprintf(tile_key, sizeof(tile_key), "completed-%03zu", i);
        assert(asgend_tui_upsert_tile(&tui, tile_key, ASGEND_TUI_TILE_ACTION,
                                      "Completed", "done", false, false, false) == 0);
    }
    tui.history_truncated = false;
    tui.dropped_tile_count = 0;
    assert(asgend_tui_upsert_tile(&tui, "new-chat", ASGEND_TUI_TILE_CHAT,
                                  "New message", "hello", false, false, true) == 0);
    assert(tui.tile_count == 512 && tui.history_truncated && tui.dropped_tile_count == 1);
    bool active_preserved = false;
    for (size_t i = 0; i < tui.tile_count; ++i)
        if (strcmp(tui.tiles[i].key, "active-stream") == 0 && tui.tiles[i].streaming)
            active_preserved = true;
    assert(active_preserved);

    for (size_t i = 0; i < tui.tile_count; ++i) tui.tiles[i].streaming = true;
    tui.history_truncated = false;
    tui.dropped_tile_count = 0;
    assert(asgend_tui_upsert_tile(&tui, "blocked-new-stream", ASGEND_TUI_TILE_TOOL,
                                  "Another stream", "live", false, true, true) == -1);
    assert(tui.tile_count == 512 && tui.history_truncated && tui.dropped_tile_count == 1);
    asgend_tui_clear_tiles(&tui);
    assert(tui.tile_count == 0);
    asgend_tui_shutdown(&tui);
    return 0;
}
