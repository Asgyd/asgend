#define _POSIX_C_SOURCE 200809L
#include "asgend_tui.h"

#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define TUI_MIN_WIDTH 36
#define TUI_MIN_HEIGHT 8
#define TUI_DEFAULT_WIDTH 80
#define TUI_DEFAULT_HEIGHT 24
#define TUI_RENDER_ROWS 256
#define TUI_RENDER_BYTES 512
#define TUI_FRAME_BYTES 8192
#define TUI_BODY_MAX (ASGEND_TUI_TEXT_MAX * 8U)
#define TUI_MAX_TILES 512
#define TUI_FILES_VISIBLE 7
#define TUI_PREVIEW_PAGE_MARGIN 2

static volatile sig_atomic_t tui_resize_pending = 0;
static struct sigaction tui_previous_winch;
static bool tui_winch_installed = false;
static bool tui_has_pending_byte = false;
static unsigned char tui_pending_byte = 0;
static char tui_current_frame[TUI_RENDER_ROWS][TUI_FRAME_BYTES];
static char tui_previous_frame[TUI_RENDER_ROWS][TUI_FRAME_BYTES];
static int tui_previous_width = 0;
static int tui_previous_height = 0;
static bool tui_previous_valid = false;
static const AsgendTui *tui_previous_owner = NULL;
enum { TUI_ROW_DEFAULT = 0, TUI_ROW_USER = 1, TUI_ROW_ORCHESTRATOR = 2,
       TUI_ROW_TOOL = 3, TUI_ROW_QUESTION = 4, TUI_ROW_ADDED = 5,
       TUI_ROW_REMOVED = 6, TUI_ROW_SECTION = 7, TUI_ROW_MCP_ON = 8,
       TUI_ROW_MCP_OFF = 9 };
static unsigned char tui_center_row_palette[TUI_RENDER_ROWS];
static unsigned char tui_right_row_palette[TUI_RENDER_ROWS];

static void tui_handle_winch(int signal_number) {
    (void)signal_number;
    tui_resize_pending = 1;
}

static void tui_install_winch_handler(void) {
    if (tui_winch_installed) return;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = tui_handle_winch;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGWINCH, &action, &tui_previous_winch) == 0) tui_winch_installed = true;
}

static void tui_restore_winch_handler(void) {
    if (!tui_winch_installed) return;
    (void)sigaction(SIGWINCH, &tui_previous_winch, NULL);
    tui_winch_installed = false;
    tui_resize_pending = 0;
}

static void tui_mouse_enable(void) {
    fputs("\033[?1000h\033[?1002h\033[?1006h", stdout);
    fflush(stdout);
}

static void tui_mouse_disable(void) {
    fputs("\033[?1006l\033[?1002l\033[?1000l", stdout);
    fflush(stdout);
}

static void tui_dimensions(AsgendTui *tui) {
    if (!tui) return;
    /* Never enlarge the reported terminal size: doing so makes the renderer
     * write past the bottom/right edge on genuinely small terminals. */
    if (tui->width <= 0) tui->width = TUI_DEFAULT_WIDTH;
    if (tui->height <= 0) tui->height = TUI_DEFAULT_HEIGHT;

    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_col > 0) tui->width = (int)ws.ws_col;
        if (ws.ws_row > 0) tui->height = (int)ws.ws_row;
    }
}

static void text_copy(char *dst, size_t cap, const char *src) {
    if (cap == 0) return;
    (void)snprintf(dst, cap, "%s", src ? src : "");
}

static void tui_enable_raw(AsgendTui *tui) {
    if (!tui->terminal_saved || !tui->active || tui->suspended) return;
    tui->active_termios = tui->original_termios;
    tui->active_termios.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    tui->active_termios.c_iflag &= (tcflag_t)~(IXON | ICRNL);
    tui->active_termios.c_cc[VMIN] = 0;
    tui->active_termios.c_cc[VTIME] = 0;
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &tui->active_termios);
}

static int tile_find(const AsgendTui *tui, const char *key) {
    for (size_t i = 0; i < tui->tile_count; ++i) {
        if (strcmp(tui->tiles[i].key, key) == 0) return (int)i;
    }
    return -1;
}

static int utf8_sequence_length(const unsigned char *p) {
    if (*p >= 0xC2 && *p <= 0xDF) return 2;
    if (*p >= 0xE0 && *p <= 0xEF) return 3;
    if (*p >= 0xF0 && *p <= 0xF4) return 4;
    return 1;
}

static void line_set(char lines[TUI_RENDER_ROWS][TUI_RENDER_BYTES],
                     bool selected[TUI_RENDER_ROWS], int row,
                     const char *text, bool is_selected) {
    if (row < 0 || row >= TUI_RENDER_ROWS) return;
    const char *source = text ? text : "";
    size_t length = strlen(source);
    if (length >= TUI_RENDER_BYTES) length = TUI_RENDER_BYTES - 1;
    memcpy(lines[row], source, length);
    lines[row][length] = '\0';
    selected[row] = is_selected;
}

static int text_display_columns(const char *text) {
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    int columns = 0;
    while (*p) {
        int seq = utf8_sequence_length(p);
        bool valid = true;
        for (int i = 1; i < seq; ++i) {
            if (p[i] == 0 || (p[i] & 0xC0) != 0x80) { valid = false; break; }
        }
        if (!valid) seq = 1;
        if (seq == 1) {
            if (*p >= 0x20 && *p != 0x7F) ++columns;
        } else {
            columns += seq == 4 ? 2 : 1;
        }
        p += seq;
    }
    return columns;
}

static void frame_append(char *dst, size_t capacity, const char *text) {
    if (!dst || !text || capacity == 0) return;
    size_t used = strlen(dst), add = strlen(text);
    if (used >= capacity - 1) return;
    if (add > capacity - used - 1) add = capacity - used - 1;
    memcpy(dst + used, text, add);
    dst[used + add] = '\0';
}

static void frame_append_clipped(char *dst, size_t capacity, const char *text, int columns) {
    if (!dst || capacity == 0 || columns <= 0) return;
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    int used_columns = 0;
    while (*p && used_columns < columns) {
        int seq = utf8_sequence_length(p);
        bool valid = true;
        for (int i = 1; i < seq; ++i) {
            if (p[i] == 0 || (p[i] & 0xC0) != 0x80) { valid = false; break; }
        }
        if (!valid) seq = 1;
        if (seq == 1 && (*p < 0x20 || *p == 0x7F)) { ++p; continue; }
        int width = seq == 4 ? 2 : 1;
        if (used_columns + width > columns) break;
        size_t used = strlen(dst);
        if (used + (size_t)seq >= capacity) break;
        memcpy(dst + used, p, (size_t)seq);
        dst[used + (size_t)seq] = '\0';
        p += seq;
        used_columns += width;
    }
    while (used_columns++ < columns) frame_append(dst, capacity, " ");
}

/* Catppuccin Mocha semantic palette (official palette: https://catppuccin.com/palette/).
 * The terminal UI uses truecolor ANSI sequences; no terminal theme installation is required. */
typedef struct { int r, g, b; } TuiRgb;

static TuiRgb tui_palette_rgb(int role) {
    switch (role) {
        case 236: case 22: return (TuiRgb){49, 50, 68};       /* Surface 0 */
        case 235: return (TuiRgb){30, 30, 46};                /* Base */
        case 238: return (TuiRgb){69, 71, 90};                /* Surface 1 */
        case 239: return (TuiRgb){88, 91, 112};               /* Surface 2 */
        case 45:  return (TuiRgb){203, 166, 247};             /* Mauve */
        case 39:  return (TuiRgb){148, 226, 213};             /* Teal */
        case 81:  return (TuiRgb){137, 220, 235};             /* Sky */
        case 220: return (TuiRgb){249, 226, 175};             /* Yellow */
        case 157: return (TuiRgb){166, 227, 161};             /* Green */
        case 245: return (TuiRgb){166, 173, 200};             /* Subtext 0 */
        case 210: case 52: return (TuiRgb){243, 139, 168};    /* Red */
        case 255: return (TuiRgb){205, 214, 244};             /* Text */
        case 24:  return (TuiRgb){137, 180, 250};              /* Blue */
        case 159: return (TuiRgb){148, 226, 213};              /* Teal */
        case 242: return (TuiRgb){127, 132, 156};              /* Overlay 1 */
        case 246: return (TuiRgb){186, 194, 222};              /* Subtext 1 */
        default:  return (TuiRgb){205, 214, 244};
    }
}

static void frame_append_style(char *row, int background, int foreground) {
    TuiRgb bg = tui_palette_rgb(background);
    TuiRgb fg = tui_palette_rgb(foreground);
    char style[96];
    (void)snprintf(style, sizeof(style), "\033[48;2;%d;%d;%dm\033[38;2;%d;%d;%dm",
                   bg.r, bg.g, bg.b, fg.r, fg.g, fg.b);
    frame_append(row, TUI_FRAME_BYTES, style);
}

static void frame_append_fill(char *row, int width, int background, int foreground,
                              const char *text, int padding) {
    if (width <= 0) return;
    frame_append_style(row, background, foreground);
    int pad = padding < 0 ? 0 : padding;
    if (pad > width) pad = width;
    for (int i = 0; i < pad; ++i) frame_append(row, TUI_FRAME_BYTES, " ");
    int body_width = width - pad * 2;
    if (body_width < 0) body_width = 0;
    frame_append_clipped(row, TUI_FRAME_BYTES, text ? text : "", body_width);
    for (int i = 0; i < pad; ++i) frame_append(row, TUI_FRAME_BYTES, " ");
    frame_append(row, TUI_FRAME_BYTES, "\033[0m");
}

static void frame_append_top_border(char *row, int panel_width, const char *title,
                                    int background, int foreground) {
    /* Accent-filled heading, deliberately without box-drawing outlines. */
    frame_append_fill(row, panel_width, background, foreground, title, 1);
}

static void frame_append_bottom_border(char *row, int panel_width, const char *hint,
                                       int background, int foreground) {
    /* Footer hints use a muted surface rather than a visual border. */
    frame_append_fill(row, panel_width, background, foreground, hint, 1);
}

static void frame_append_status(char *row, int width, const char *text) {
    frame_append_fill(row, width, 236, 245, text, 1);
}

static void frame_append_panel_content(char *row, int panel_width, const char *text,
                                       bool selected, int background, int foreground) {
    if (panel_width <= 0) return;
    if (selected) background = 239;
    frame_append_fill(row, panel_width, background, selected ? 255 : foreground, text, 1);
}

static void frame_present(AsgendTui *tui, int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (width > TUI_FRAME_BYTES / 8) width = TUI_FRAME_BYTES / 8;
    if (height > TUI_RENDER_ROWS) height = TUI_RENDER_ROWS;
    bool full = !tui_previous_valid || tui_previous_owner != tui ||
                width != tui_previous_width || height != tui_previous_height;
    if (full) fputs("\033[2J", stdout);
    for (int row = 0; row < height; ++row) {
        if (full || strcmp(tui_current_frame[row], tui_previous_frame[row]) != 0) {
            printf("\033[%d;1H\033[2K", row + 1);
            fputs(tui_current_frame[row], stdout);
            /* Bound the cached row copy explicitly. GCC cannot infer the frame
             * builder's capacity through all helper calls and otherwise emits a
             * false-positive truncation warning for this fixed-size row. */
            size_t previous_capacity = sizeof(tui_previous_frame[row]);
            size_t row_length = strnlen(tui_current_frame[row], sizeof(tui_current_frame[row]));
            if (row_length >= previous_capacity) row_length = previous_capacity - 1;
            memcpy(tui_previous_frame[row], tui_current_frame[row], row_length);
            tui_previous_frame[row][row_length] = '\0';
        }
    }
    fflush(stdout);
    tui_previous_width = width;
    tui_previous_height = height;
    tui_previous_valid = true;
    tui_previous_owner = tui;
}

static const char *tui_file_icon(const char *path, bool directory) {
    if (directory) return "▸";
    const char *dot = path ? strrchr(path, '.') : NULL;
    if (!dot) return "◇";
    if (!strcasecmp(dot, ".c") || !strcasecmp(dot, ".h") || !strcasecmp(dot, ".cpp") ||
        !strcasecmp(dot, ".hpp") || !strcasecmp(dot, ".rs") || !strcasecmp(dot, ".go")) return "⌘";
    if (!strcasecmp(dot, ".py") || !strcasecmp(dot, ".js") || !strcasecmp(dot, ".ts") ||
        !strcasecmp(dot, ".jsx") || !strcasecmp(dot, ".tsx") || !strcasecmp(dot, ".java")) return "λ";
    if (!strcasecmp(dot, ".json") || !strcasecmp(dot, ".toml") || !strcasecmp(dot, ".yaml") ||
        !strcasecmp(dot, ".yml") || !strcasecmp(dot, ".ini") || !strcasecmp(dot, ".xml")) return "⚙";
    if (!strcasecmp(dot, ".md") || !strcasecmp(dot, ".txt") || !strcasecmp(dot, ".rst") ||
        !strcasecmp(dot, ".log")) return "≡";
    if (!strcasecmp(dot, ".sh") || !strcasecmp(dot, ".bash") || !strcasecmp(dot, ".zsh")) return "$_";
    if (!strcasecmp(dot, ".png") || !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") ||
        !strcasecmp(dot, ".gif") || !strcasecmp(dot, ".svg") || !strcasecmp(dot, ".webp")) return "▧";
    if (!strcasecmp(dot, ".pdf") || !strcasecmp(dot, ".docx") || !strcasecmp(dot, ".xlsx") ||
        !strcasecmp(dot, ".pptx")) return "▤";
    return "◇";
}

static void tui_format_file_line(char *line, size_t cap, const AsgendTuiFileItem *file) {
    bool directory = strcmp(file->kind, "directory") == 0;
    const char *icon = tui_file_icon(file->path, directory);
    if (directory) {
        (void)snprintf(line, cap, "%s %s/", icon, file->path);
    } else if (file->size >= 1024 * 1024) {
        (void)snprintf(line, cap, "%s %s (%lld MiB)", icon, file->path,
                       (long long)(file->size / (1024 * 1024)));
    } else if (file->size >= 1024) {
        (void)snprintf(line, cap, "%s %s (%lld KiB)", icon, file->path,
                       (long long)(file->size / 1024));
    } else {
        (void)snprintf(line, cap, "%s %s (%lld B)", icon, file->path, (long long)file->size);
    }
}

static int make_left_panel(AsgendTui *tui,
                           char lines[TUI_RENDER_ROWS][TUI_RENDER_BYTES],
                           bool selected[TUI_RENDER_ROWS], int rows) {
    size_t visible = rows > 0 ? (size_t)rows : 0;
    size_t max_offset = tui->file_count > visible ? tui->file_count - visible : 0;
    if (tui->selected_file_index < tui->file_scroll_offset)
        tui->file_scroll_offset = tui->selected_file_index;
    else if (visible > 0 && tui->selected_file_index >= tui->file_scroll_offset + visible)
        tui->file_scroll_offset = tui->selected_file_index - visible + 1;
    if (tui->file_scroll_offset > max_offset) tui->file_scroll_offset = max_offset;

    int row = 0;
    if (tui->file_count == 0) {
        if (tui->welcome_screen) {
            line_set(lines, selected, row++, "Выберите задачу", false);
            if (row < rows) line_set(lines, selected, row++, "Файлы выбранной задачи появятся здесь", false);
        } else {
            line_set(lines, selected, row++, "Каталог пуст", false);
            if (row < rows) line_set(lines, selected, row++, "Файлы проекта появятся здесь", false);
        }
        return row;
    }
    size_t end = tui->file_scroll_offset + visible;
    if (end > tui->file_count) end = tui->file_count;
    for (size_t i = tui->file_scroll_offset; i < end && row < rows; ++i, ++row) {
        const AsgendTuiFileItem *file = &tui->files[i];
        char line[320];
        tui_format_file_line(line, sizeof(line), file);
        line_set(lines, selected, row, line, tui->focused_pane == 0 && i == tui->selected_file_index);
    }
    return row;
}

/* Render a scrollable window from the beginning of a UTF-8 text buffer. */
static int wrap_body_window(const char *body, int columns, size_t first_line,
                            char (*lines)[TUI_RENDER_BYTES], int max_lines) {
    if (!body || max_lines <= 0) return 0;
    if (columns < 8) columns = 8;
    char line[TUI_RENDER_BYTES];
    size_t line_bytes = 0;
    int line_columns = 0;
    size_t visual_line = 0;
    int written = 0;
    const unsigned char *p = (const unsigned char *)body;

    while (*p && written < max_lines) {
        if (*p == '\n') {
            line[line_bytes] = '\0';
            if (visual_line >= first_line)
                (void)snprintf(lines[written++], TUI_RENDER_BYTES, "%s", line);
            ++visual_line;
            line_bytes = 0;
            line_columns = 0;
            ++p;
            continue;
        }

        int seq = utf8_sequence_length(p);
        bool valid = true;
        for (int i = 1; i < seq; ++i) {
            if (p[i] == 0 || (p[i] & 0xC0) != 0x80) { valid = false; break; }
        }
        if (!valid) seq = 1;
        int char_columns = seq == 4 ? 2 : 1;
        if (line_columns + char_columns > columns || line_bytes + (size_t)seq >= sizeof(line)) {
            line[line_bytes] = '\0';
            if (visual_line >= first_line)
                (void)snprintf(lines[written++], TUI_RENDER_BYTES, "%s", line);
            ++visual_line;
            line_bytes = 0;
            line_columns = 0;
            if (written >= max_lines) break;
            continue;
        }
        memcpy(line + line_bytes, p, (size_t)seq);
        line_bytes += (size_t)seq;
        line_columns += char_columns;
        p += seq;
    }
    if (written < max_lines && line_bytes > 0) {
        line[line_bytes] = '\0';
        if (visual_line >= first_line)
            (void)snprintf(lines[written++], TUI_RENDER_BYTES, "%s", line);
    }
    return written;
}

static size_t count_preview_lines(const char *body, int columns, bool no_wrap) {
    if (!body || !*body) return 0;
    if (columns < 8) columns = 8;
    if (no_wrap) {
        size_t lines = 0;
        for (const char *p = body; *p; ++p) if (*p == '\n') ++lines;
        size_t length = strlen(body);
        if (length > 0 && body[length - 1] != '\n') ++lines;
        return lines;
    }
    size_t lines = 0;
    int line_columns = 0;
    const unsigned char *p = (const unsigned char *)body;
    while (*p) {
        if (*p == '\n') { ++lines; line_columns = 0; ++p; continue; }
        int seq = utf8_sequence_length(p);
        bool valid = true;
        for (int i = 1; i < seq; ++i) {
            if (!p[i] || (p[i] & 0xc0U) != 0x80U) { valid = false; break; }
        }
        if (!valid) seq = 1;
        int width = seq == 4 ? 2 : 1;
        if (line_columns + width > columns) { ++lines; line_columns = 0; continue; }
        line_columns += width;
        p += seq;
    }
    if (line_columns > 0) ++lines;
    return lines;
}

static int nowrap_body_window(const char *body, int columns, size_t first_line,
                              size_t horizontal_offset,
                              char (*lines)[TUI_RENDER_BYTES], int max_lines) {
    if (!body || max_lines <= 0) return 0;
    if (columns < 8) columns = 8;
    size_t physical_line = 0;
    int written = 0;
    const unsigned char *p = (const unsigned char *)body;
    const unsigned char *line_start = p;
    for (;;) {
        if (*p == '\n' || *p == '\0') {
            if (physical_line >= first_line && written < max_lines) {
                const unsigned char *q = line_start;
                size_t columns_used = 0;
                size_t bytes_used = 0;
                while (q < p) {
                    int seq = utf8_sequence_length(q);
                    bool valid = true;
                    for (int i = 1; i < seq && q + i < p; ++i)
                        if ((q[i] & 0xc0U) != 0x80U) { valid = false; break; }
                    if (!valid || q + seq > p) seq = 1;
                    size_t char_width = seq == 4 ? 2U : 1U;
                    if (columns_used + char_width <= horizontal_offset) {
                        columns_used += char_width; q += seq; continue;
                    }
                    if (columns_used < horizontal_offset) {
                        columns_used += char_width; q += seq; continue;
                    }
                    if (columns_used + char_width > horizontal_offset + (size_t)columns ||
                        bytes_used + (size_t)seq >= TUI_RENDER_BYTES) break;
                    memcpy(lines[written] + bytes_used, q, (size_t)seq);
                    bytes_used += (size_t)seq;
                    columns_used += char_width;
                    q += seq;
                }
                lines[written][bytes_used] = '\0';
                ++written;
            }
            if (*p == '\0' || written >= max_lines) break;
            ++physical_line;
            ++p;
            line_start = p;
            continue;
        }
        ++p;
    }
    return written;
}

static size_t tile_visual_lines(const AsgendTuiTile *tile, int columns) {
    if (!tile || !tile->expanded || !tile->body || !tile->body[0]) return 1;
    int body_columns = columns > 6 ? columns - 4 : 4;
    return count_preview_lines(tile->body, body_columns, false) + 2;
}

static size_t center_tile_visual_start(const AsgendTui *tui, size_t tile_index, int columns) {
    size_t offset = 0;
    if (!tui) return 0;
    if (tile_index > tui->tile_count) tile_index = tui->tile_count;
    for (size_t i = 0; i < tile_index; ++i)
        offset += tile_visual_lines(&tui->tiles[i], columns);
    return offset;
}

static size_t center_total_visual_lines(const AsgendTui *tui, int columns) {
    if (!tui || tui->tile_count == 0) return 1;
    return center_tile_visual_start(tui, tui->tile_count, columns);
}

static void center_follow_stream_tail(AsgendTui *tui) {
    if (!tui) return;
    int columns = tui->center_width > 2 ? tui->center_width - 2 : 8;
    size_t total_lines = center_total_visual_lines(tui, columns);
    size_t visible = tui->content_rows > 2 ? (size_t)(tui->content_rows - 2) : 1;
    tui->scroll_offset = total_lines > visible ? total_lines - visible : 0;
    tui->follow_stream_tail_pending = false;
}

static void center_keep_selection_visible(AsgendTui *tui) {
    if (!tui) return;
    int columns = tui->center_width > 2 ? tui->center_width - 2 : 8;
    size_t selected_start = center_tile_visual_start(tui, tui->selected_tile, columns);
    size_t visible = tui->content_rows > 0 ? (size_t)tui->content_rows : 1;
    if (selected_start < tui->scroll_offset) tui->scroll_offset = selected_start;
    else if (selected_start >= tui->scroll_offset + visible)
        tui->scroll_offset = selected_start >= visible ? selected_start - visible + 1 : 0;
    size_t total = center_total_visual_lines(tui, columns);
    size_t max_offset = total > visible ? total - visible : 0;
    if (tui->scroll_offset > max_offset) tui->scroll_offset = max_offset;
}

static size_t tile_for_visual_line(const AsgendTui *tui, size_t visual_line, int columns) {
    size_t cursor = 0;
    if (!tui || tui->tile_count == 0) return 0;
    for (size_t i = 0; i < tui->tile_count; ++i) {
        size_t rows = tile_visual_lines(&tui->tiles[i], columns);
        if (visual_line < cursor + rows) return i;
        cursor += rows;
    }
    return tui->tile_count - 1;
}

static unsigned char tile_row_palette(const AsgendTuiTile *tile) {
    if (!tile) return TUI_ROW_DEFAULT;
    if (strncmp(tile->key, "user:", 5) == 0 || strcmp(tile->title, "Вы") == 0)
        return TUI_ROW_USER;
    if (strncmp(tile->key, "orchestrator:", 13) == 0 || strcmp(tile->title, "Оркестратор") == 0)
        return TUI_ROW_ORCHESTRATOR;
    if (tile->kind == ASGEND_TUI_TILE_TOOL)
        return TUI_ROW_TOOL;
    return TUI_ROW_DEFAULT;
}

static bool tui_task_is_active(const AsgendTuiTaskItem *task) {
    if (!task) return false;
    return strcmp(task->status, "в очереди") == 0 || strcmp(task->status, "выполняется") == 0 ||
           strcmp(task->status, "ожидает ответа") == 0 || strcmp(task->status, "queued") == 0 ||
           strcmp(task->status, "running") == 0 || strcmp(task->status, "waiting_input") == 0;
}

static bool tui_text_contains(const char *text, const char *query) {
    if (!query || !*query) return true;
    if (!text) return false;
    size_t qlen = strlen(query);
    for (const char *p = text; *p; ++p) {
        size_t i = 0;
        while (i < qlen && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)query[i])) ++i;
        if (i == qlen) return true;
    }
    return false;
}

static void tui_rebuild_task_filters(AsgendTui *tui) {
    if (!tui) return;
    tui->welcome_filtered_count = 0;
    tui->session_filtered_count = 0;
    for (size_t i = 0; i < tui->task_count && i < ASGEND_TUI_ITEMS_MAX; ++i) {
        const AsgendTuiTaskItem *task = &tui->tasks[i];
        if (tui_task_is_active(task))
            tui->welcome_filtered_indices[tui->welcome_filtered_count++] = i;
        if (tui_text_contains(task->title, tui->session_query) ||
            tui_text_contains(task->id, tui->session_query) ||
            tui_text_contains(task->status, tui->session_query) ||
            tui_text_contains(task->created_at, tui->session_query))
            tui->session_filtered_indices[tui->session_filtered_count++] = i;
    }
    if (tui->selected_task >= tui->welcome_filtered_count) tui->selected_task = tui->welcome_filtered_count ? tui->welcome_filtered_count - 1 : 0;
    if (tui->session_selected >= tui->session_filtered_count) tui->session_selected = tui->session_filtered_count ? tui->session_filtered_count - 1 : 0;
    if (tui->session_scroll_offset > tui->session_selected) tui->session_scroll_offset = tui->session_selected;
}

static int tui_render_welcome_panel(AsgendTui *tui,
                                    char lines[TUI_RENDER_ROWS][TUI_RENDER_BYTES],
                                    bool selected[TUI_RENDER_ROWS], int rows, int columns) {
    memset(tui_center_row_palette, TUI_ROW_DEFAULT, sizeof(tui_center_row_palette));
    if (rows <= 0) return 0;
    const int visible = rows >= 5 ? (rows - 3) / 2 : 1;
    size_t max_visible = visible > 0 ? (size_t)visible : 1;
    size_t count = tui->welcome_filtered_count;
    if (count == 0) {
        int start = rows > 4 ? rows / 2 - 1 : 0;
        if (start < 0) start = 0;
        if (start < rows) {
            line_set(lines, selected, start, "Продолжить работу", false);
            tui_center_row_palette[start] = TUI_ROW_ORCHESTRATOR;
        }
        if (start + 1 < rows) line_set(lines, selected, start + 1, "Нет активных задач · введите запрос ниже", false);
        if (start + 2 < rows) line_set(lines, selected, start + 2, "/session — история всех задач", false);
        tui->welcome_start_row = start;
        tui->welcome_scroll_offset = 0;
        tui->welcome_visible_count = 0;
        return rows;
    }
    if (max_visible > count) max_visible = count;
    size_t max_offset = count > max_visible ? count - max_visible : 0;
    size_t offset = tui->selected_task >= max_visible ? tui->selected_task - max_visible + 1 : 0;
    if (offset > max_offset) offset = max_offset;
    tui->welcome_scroll_offset = offset;
    tui->welcome_visible_count = max_visible;
    int block_rows = 1 + (int)max_visible * 2;
    int start = (rows - block_rows) / 2;
    if (start < 0) start = 0;
    tui->welcome_start_row = start;
    line_set(lines, selected, start, "Активные задачи · ↑/↓ выбор · Enter открыть", false);
    tui_center_row_palette[start] = TUI_ROW_ORCHESTRATOR;
    int row = start + 1;
    for (size_t visible_index = 0; visible_index < max_visible && row < rows; ++visible_index, row += 2) {
        size_t filter_index = offset + visible_index;
        if (filter_index >= count) break;
        size_t task_index = tui->welcome_filtered_indices[filter_index];
        const AsgendTuiTaskItem *task = &tui->tasks[task_index];
        bool is_selected = filter_index == tui->selected_task;
        char title[TUI_RENDER_BYTES], meta[TUI_RENDER_BYTES];
        (void)snprintf(title, sizeof(title), "  %.400s", task->title[0] ? task->title : task->id);
        (void)snprintf(meta, sizeof(meta), "    %s", task->status[0] ? task->status : "статус неизвестен");
        line_set(lines, selected, row, title, is_selected);
        tui_center_row_palette[row] = is_selected ? TUI_ROW_USER : TUI_ROW_DEFAULT;
        if (row + 1 < rows) {
            line_set(lines, selected, row + 1, meta, false);
            tui_center_row_palette[row + 1] = TUI_ROW_SECTION;
        }
    }
    (void)columns;
    return rows;
}

static void tui_overlay_session_row(AsgendTui *tui, int row, int usable_width,
                                    int x, int modal_width, const char *text,
                                    bool selected) {
    if (!tui || row < 0 || row >= tui->height || usable_width <= 0) return;
    tui_current_frame[row][0] = '\0';
    if (x > 0) frame_append_fill(tui_current_frame[row], x, 235, 242, "", 0);
    frame_append_fill(tui_current_frame[row], modal_width, selected ? 239 : 236,
                      selected ? 255 : 246, text, 1);
    int suffix = usable_width - x - modal_width;
    if (suffix > 0) frame_append_fill(tui_current_frame[row], suffix, 235, 242, "", 0);
}

static void tui_render_session_picker(AsgendTui *tui, int usable_width, int height, int prompt_top) {
    if (!tui || !tui->session_picker_active) return;
    int content_top = 3;
    int content_rows = prompt_top - content_top;
    if (content_rows < 4) return;
    int modal_width = usable_width - 4;
    if (modal_width > 78) modal_width = 78;
    if (modal_width < 20) modal_width = usable_width > 0 ? usable_width : 1;
    if (modal_width > usable_width) modal_width = usable_width;
    int x = (usable_width - modal_width) / 2;
    size_t max_visible = content_rows > 4 ? (size_t)((content_rows - 3) / 2) : 1;
    if (max_visible > tui->session_filtered_count) max_visible = tui->session_filtered_count;
    if (max_visible == 0 && tui->session_filtered_count > 0) max_visible = 1;
    size_t max_offset = tui->session_filtered_count > max_visible ? tui->session_filtered_count - max_visible : 0;
    if (tui->session_selected < tui->session_scroll_offset) tui->session_scroll_offset = tui->session_selected;
    if (max_visible && tui->session_selected >= tui->session_scroll_offset + max_visible)
        tui->session_scroll_offset = tui->session_selected - max_visible + 1;
    if (tui->session_scroll_offset > max_offset) tui->session_scroll_offset = max_offset;
    size_t visible = tui->session_filtered_count - tui->session_scroll_offset;
    if (visible > max_visible) visible = max_visible;
    int modal_height = 3 + (int)visible * 2;
    if (tui->session_filtered_count == 0) modal_height = 4;
    if (modal_height > content_rows) modal_height = content_rows;
    int top = content_top + (content_rows - modal_height) / 2;
    if (top < content_top) top = content_top;
    tui->session_modal_x = x;
    tui->session_modal_width = modal_width;
    tui->session_modal_top = top;
    tui->session_modal_list_top = top + 3;
    tui->session_modal_visible_count = visible;

    tui_overlay_session_row(tui, top, usable_width, x, modal_width, "Выбор задачи · /session", false);
    char search[TUI_RENDER_BYTES];
    (void)snprintf(search, sizeof(search), "Поиск  %s", tui->session_query);
    tui_overlay_session_row(tui, top + 1, usable_width, x, modal_width, search, false);
    tui_overlay_session_row(tui, top + 2, usable_width, x, modal_width,
                            "↑/↓ выбрать · Enter открыть · Esc закрыть", false);
    if (tui->session_filtered_count == 0) {
        if (top + 3 < prompt_top)
            tui_overlay_session_row(tui, top + 3, usable_width, x, modal_width,
                                    "Совпадений не найдено", false);
    } else {
        for (size_t i = 0; i < visible; ++i) {
            size_t filter_index = tui->session_scroll_offset + i;
            size_t task_index = tui->session_filtered_indices[filter_index];
            const AsgendTuiTaskItem *task = &tui->tasks[task_index];
            char title[TUI_RENDER_BYTES], meta[TUI_RENDER_BYTES];
            (void)snprintf(title, sizeof(title), "  %.420s", task->title[0] ? task->title : task->id);
            (void)snprintf(meta, sizeof(meta), "    %s · %.16s", task->status[0] ? task->status : "—",
                           task->created_at[0] ? task->created_at : "дата неизвестна");
            int y = top + 3 + (int)i * 2;
            if (y < prompt_top) tui_overlay_session_row(tui, y, usable_width, x, modal_width,
                                                        title, filter_index == tui->session_selected);
            if (y + 1 < prompt_top) tui_overlay_session_row(tui, y + 1, usable_width, x, modal_width, meta, false);
        }
    }
    tui->session_cursor_row = top + 1;
    int query_cols = text_display_columns(tui->session_query);
    int cursor = x + 8 + query_cols;
    if (cursor >= x + modal_width - 1) cursor = x + modal_width - 2;
    if (cursor < x + 7) cursor = x + 7;
    tui->session_cursor_col = cursor;
    (void)height;
}

static int make_center_panel(AsgendTui *tui,
                             char lines[TUI_RENDER_ROWS][TUI_RENDER_BYTES],
                             bool selected[TUI_RENDER_ROWS], int rows, int columns) {
    int available = rows;
    if (available <= 0) return 0;
    memset(tui_center_row_palette, TUI_ROW_DEFAULT, sizeof(tui_center_row_palette));
    if (tui->welcome_screen && !tui->session_picker_active)
        return tui_render_welcome_panel(tui, lines, selected, rows, columns);
    size_t total = center_total_visual_lines(tui, columns);
    size_t max_offset = total > (size_t)available ? total - (size_t)available : 0;
    if (tui->scroll_offset > max_offset) tui->scroll_offset = max_offset;
    size_t line_start = tui->scroll_offset;
    size_t line_end = line_start + (size_t)available;
    size_t line_cursor = 0;
    int row = 0;

    if (tui->tile_count == 0) {
        line_set(lines, selected, 0, "Диалог готов · / — новый запрос · o — открыть задачу по ID", false);
        if (rows > 1) line_set(lines, selected, 1, "Модель, инструменты и события появятся здесь", false);
        row = rows < 2 ? rows : 2;
    } else {
        int body_columns = columns > 6 ? columns - 4 : 4;
        for (size_t i = 0; i < tui->tile_count && row < rows; ++i) {
            const AsgendTuiTile *tile = &tui->tiles[i];
            size_t visual_rows = tile_visual_lines(tile, columns);
            if (line_cursor >= line_end || line_cursor + visual_rows <= line_start) {
                line_cursor += visual_rows;
                continue;
            }
            size_t local_cursor = 0;
            char heading[TUI_RENDER_BYTES];
            char state[16] = "";
            char activity[8] = "";
            const char *speaker = tile->title[0] ? tile->title : "Событие";
            static const char *const spinner[] = {"◐", "◓", "◑", "◒"};
            if (tile->streaming)
                (void)snprintf(activity, sizeof(activity), "%s ", spinner[tui->spinner_frame % 4U]);
            /* Event titles can carry explicit ↓/→ state. Don't append a second
             * arrow; generic progress cards still receive the default marker. */
            if (!strstr(speaker, "↓") && !strstr(speaker, "→") &&
                !tile->streaming && !tile->expanded)
                (void)snprintf(state, sizeof(state), " →");
            (void)snprintf(heading, sizeof(heading), "%s%s%s", activity, speaker, state);
            if (line_cursor >= line_start && line_cursor < line_end) {
                row = (int)(line_cursor - line_start);
                if (row < rows) {
                    line_set(lines, selected, row, heading,
                             i == tui->selected_tile && tui->focused_pane == 1);
                    tui_center_row_palette[row] = tile_row_palette(tile);
                }
            }
            ++local_cursor;
            if (tile->expanded && tile->body && tile->body[0]) {
                size_t body_lines = count_preview_lines(tile->body, body_columns, false);
                size_t body_visible_start = line_cursor + local_cursor;
                size_t body_visible_end = body_visible_start + body_lines;
                if (body_visible_start < line_end && body_visible_end > line_start) {
                    size_t first = line_start > body_visible_start ? line_start - body_visible_start : 0;
                    size_t last = body_lines;
                    size_t max_last = line_end > body_visible_start ? line_end - body_visible_start : 0;
                    if (last > max_last) last = max_last;
                    if (last > first) {
                        int want = (int)(last - first);
                        char wrapped[TUI_RENDER_ROWS][TUI_RENDER_BYTES] = {{0}};
                        int got = wrap_body_window(tile->body, body_columns, first, wrapped, want);
                        for (int j = 0; j < got; ++j) {
                            size_t abs_line = body_visible_start + first + (size_t)j;
                            if (abs_line >= line_start && abs_line < line_end) {
                                row = (int)(abs_line - line_start);
                                char body_line[TUI_RENDER_BYTES];
                                (void)snprintf(body_line, sizeof(body_line), "  %.500s", wrapped[j]);
                                line_set(lines, selected, row, body_line, false);
                                tui_center_row_palette[row] = tile_row_palette(tile);
                            }
                        }
                    }
                }
                local_cursor += body_lines;
                size_t bottom_line = line_cursor + local_cursor;
                if (bottom_line >= line_start && bottom_line < line_end) {
                    row = (int)(bottom_line - line_start);
                    line_set(lines, selected, row, "", false);
                    tui_center_row_palette[row] = tile_row_palette(tile);
                }
                ++local_cursor;
            }
            line_cursor += local_cursor;
        }
    }

    if (tui->question_active && rows >= 4) {
        int box_rows = (int)tui->question_option_count + 3;
        if (box_rows < 4) box_rows = 4;
        if (box_rows > rows) box_rows = rows;
        int start = rows - box_rows;
        char text[TUI_RENDER_BYTES];
        (void)snprintf(text, sizeof(text), "Вопрос · %.150s", tui->question_title[0] ? tui->question_title : "Уточнение от модели");
        line_set(lines, selected, start, text, false);
        if (start >= 0 && start < TUI_RENDER_ROWS) tui_center_row_palette[start] = TUI_ROW_QUESTION;
        (void)snprintf(text, sizeof(text), "  %.500s", tui->question_text);
        if (start + 1 < rows) {
            line_set(lines, selected, start + 1, text, false);
            if (start + 1 >= 0 && start + 1 < TUI_RENDER_ROWS) tui_center_row_palette[start + 1] = TUI_ROW_QUESTION;
        }
        for (size_t i = 0; i < tui->question_option_count && start + 2 + (int)i < rows; ++i) {
            (void)snprintf(text, sizeof(text), "  %c %zu. %s", i == tui->question_selected ? '>' : ' ', i + 1,
                           tui->question_options[i]);
            line_set(lines, selected, start + 2 + (int)i, text, i == tui->question_selected);
            if (start + 2 + (int)i >= 0 && start + 2 + (int)i < TUI_RENDER_ROWS)
                tui_center_row_palette[start + 2 + (int)i] = TUI_ROW_QUESTION;
        }
        int bottom = start + box_rows - 1;
        if (bottom >= 0 && bottom < rows) {
            line_set(lines, selected, bottom, "↑/↓ выбрать · Enter подтвердить · Esc отложить", false);
            if (bottom >= 0 && bottom < TUI_RENDER_ROWS) tui_center_row_palette[bottom] = TUI_ROW_QUESTION;
        }
    }
    return rows;
}

static size_t tui_file_visible_items(const AsgendTui *tui) {
    if (!tui || tui->content_rows <= 0) return 1;
    return (size_t)tui->content_rows;
}

static size_t tui_plan_window_offset(size_t selected, size_t total, size_t visible) {
    if (visible == 0) return 0;
    size_t offset = selected >= visible ? selected - visible + 1 : 0;
    size_t max_offset = total > visible ? total - visible : 0;
    return offset > max_offset ? max_offset : offset;
}

static int make_right_panel(AsgendTui *tui,
                            char lines[TUI_RENDER_ROWS][TUI_RENDER_BYTES],
                            bool selected[TUI_RENDER_ROWS], int rows, int columns) {
    int row = 0;
    if (tui->preview_content) {
        char head[320];
        if (tui->preview_file_size >= 0 && tui->preview_file_size > (int64_t)tui->preview_length) {
            (void)snprintf(head, sizeof(head), "ПРОСМОТР · %.140s · %lld/%lld B%s",
                           tui->preview_path, (long long)tui->preview_chunk_offset,
                           (long long)tui->preview_file_size, tui->preview_no_wrap ? " · без переноса" : "");
        } else {
            (void)snprintf(head, sizeof(head), "ФАЙЛ · %.240s%s", tui->preview_path,
                           tui->preview_no_wrap ? " · без переноса" : "");
        }
        line_set(lines, selected, row++, head, false);
        if (row < rows) line_set(lines, selected, row++, tui->preview_no_wrap
                 ? "↑/↓ · без переноса · Esc закрыть"
                 : "↑/↓ прокрутка · PgUp/PgDn · Esc закрыть", false);
        int available = rows - row;
        if (available < 0) available = 0;
        int body_columns = columns > 2 ? columns : 8;
        int body_start = row;
        int count = tui->preview_no_wrap
            ? nowrap_body_window(tui->preview_content, body_columns, tui->preview_line_offset,
                                 tui->preview_horizontal_offset, lines + row, available)
            : wrap_body_window(tui->preview_content, body_columns, tui->preview_line_offset,
                               lines + row, available);
        for (int i = 0; i < count && body_start + i < TUI_RENDER_ROWS; ++i) {
            const char *line = lines[body_start + i];
            if (line[0] == '+' && line[1] == ' ') tui_right_row_palette[body_start + i] = TUI_ROW_ADDED;
            else if (line[0] == '-' && line[1] == ' ') tui_right_row_palette[body_start + i] = TUI_ROW_REMOVED;
        }
        row += count;
        if (count == 0 && row < rows) line_set(lines, selected, row++, "(пустой файл или конец файла)", false);
        return row;
    }

    const size_t goal_limit = 3;
    const size_t todo_limit = 3;
    line_set(lines, selected, row, "GOALS", false);
    if (row >= 0 && row < TUI_RENDER_ROWS) tui_right_row_palette[row] = TUI_ROW_SECTION;
    ++row;
    if (row < rows) {
        if (tui->goal_count == 0) line_set(lines, selected, row++, "  (целей пока нет)", false);
        else {
            size_t shown = tui->goal_count < goal_limit ? tui->goal_count : goal_limit;
            size_t offset = tui->selected_plan_is_goal
                ? tui_plan_window_offset(tui->selected_plan, tui->goal_count, shown) : 0;
            for (size_t i = offset; i < tui->goal_count && i < offset + shown && row < rows; ++i, ++row) {
                char line[320];
                const char *mark = strcmp(tui->goals[i].status, "done") == 0 ? "✓" : "○";
                (void)snprintf(line, sizeof(line), " %s %.280s", mark, tui->goals[i].title);
                line_set(lines, selected, row, line,
                         tui->focused_pane == 2 && tui->selected_plan_is_goal && tui->selected_plan == i);
            }
        }
    }
    if (row < rows) {
        line_set(lines, selected, row, "MCP SERVERS", false);
        if (row >= 0 && row < TUI_RENDER_ROWS) tui_right_row_palette[row] = TUI_ROW_SECTION;
        ++row;
    }
    if (tui->mcp_server_count == 0 && row < rows) {
        line_set(lines, selected, row, "  ○ MCP не настроены", false);
        if (row >= 0 && row < TUI_RENDER_ROWS) tui_right_row_palette[row] = TUI_ROW_MCP_OFF;
        ++row;
    }
    for (size_t i = 0; i < tui->mcp_server_count && row < rows; ++i, ++row) {
        char line[320];
        const AsgendTuiMcpItem *mcp = &tui->mcp_servers[i];
        (void)snprintf(line, sizeof(line), " %s %.140s · %.100s",
                       mcp->connected ? "●" : "○", mcp->name,
                       mcp->status[0] ? mcp->status : (mcp->connected ? "подключён" : "не подключён"));
        line_set(lines, selected, row, line, false);
        if (row >= 0 && row < TUI_RENDER_ROWS)
            tui_right_row_palette[row] = mcp->connected ? TUI_ROW_MCP_ON : TUI_ROW_MCP_OFF;
    }
    if (row < rows) {
        line_set(lines, selected, row, "TO-DO", false);
        if (row >= 0 && row < TUI_RENDER_ROWS) tui_right_row_palette[row] = TUI_ROW_SECTION;
        ++row;
    }
    if (row < rows) {
        if (tui->todo_count == 0) line_set(lines, selected, row++, "  (список пуст)", false);
        else {
            size_t shown = tui->todo_count < todo_limit ? tui->todo_count : todo_limit;
            size_t offset = tui->selected_plan_is_goal ? 0
                : tui_plan_window_offset(tui->selected_plan, tui->todo_count, shown);
            for (size_t i = offset; i < tui->todo_count && i < offset + shown && row < rows; ++i, ++row) {
                char line[320];
                const char *mark = strcmp(tui->todos[i].status, "done") == 0 ? "✓" : "○";
                (void)snprintf(line, sizeof(line), " %s %.280s", mark, tui->todos[i].title);
                line_set(lines, selected, row, line,
                         tui->focused_pane == 2 && !tui->selected_plan_is_goal && tui->selected_plan == i);
            }
        }
    }
    if (tui->goal_count > goal_limit && row < rows) {
        char line[96]; (void)snprintf(line, sizeof(line), "  … ещё %zu целей", tui->goal_count - goal_limit);
        line_set(lines, selected, row++, line, false);
    }
    if (tui->todo_count > todo_limit && row < rows) {
        char line[96]; (void)snprintf(line, sizeof(line), "  … ещё %zu пунктов", tui->todo_count - todo_limit);
        line_set(lines, selected, row++, line, false);
    }
    return row;
}

void asgend_tui_render(AsgendTui *tui) {
    if (!tui || !tui->active || tui->suspended) return;
    if (tui->render_defer_depth > 0) {
        tui->render_pending = true;
        return;
    }
    if (tui->follow_stream_tail_pending && tui->follow_stream_tail)
        center_follow_stream_tail(tui);
    else
        tui->follow_stream_tail_pending = false;
    tui_dimensions(tui);
    int width = tui->width;
    int height = tui->height;
    if (width > TUI_FRAME_BYTES / 16) width = TUI_FRAME_BYTES / 16;
    if (height > TUI_RENDER_ROWS) height = TUI_RENDER_ROWS;
    if (width <= 0 || height <= 0) return;

    memset(tui_current_frame, 0, sizeof(tui_current_frame));
    memset(tui_right_row_palette, TUI_ROW_DEFAULT, sizeof(tui_right_row_palette));
    if (width < TUI_MIN_WIDTH || height < TUI_MIN_HEIGHT) {
        char warning[TUI_FRAME_BYTES];
        (void)snprintf(warning, sizeof(warning),
                       "ASGEND TUI: окно %dx%d; минимум %dx%d. Увеличьте терминал.",
                       width, height, TUI_MIN_WIDTH, TUI_MIN_HEIGHT);
        frame_append(tui_current_frame[0], TUI_FRAME_BYTES, "\033[48;5;235m\033[38;5;203m");
        frame_append_clipped(tui_current_frame[0], TUI_FRAME_BYTES, warning, width > 1 ? width - 1 : 1);
        frame_append(tui_current_frame[0], TUI_FRAME_BYTES, "\033[0m");
        frame_present(tui, width, height);
        tui->content_rows = 0;
        return;
    }

    /* Reserve the last terminal column to avoid delayed-wrap artifacts. */
    int usable_width = width - 1;
    /* Progressive responsive layout: hide right column first, then left. */
    bool show_left = usable_width >= 68;
    bool show_right = usable_width >= 100;
    int left = show_left ? usable_width * 26 / 100 : 0;
    int right = show_right ? usable_width * 24 / 100 : 0;
    int center = usable_width - left - right;
    if (show_left && left < 16) left = 16;
    /* Reserve enough interior columns for a status glyph plus long plan titles.
     * At 120 columns the old 18-column floor clipped the last characters from
     * synchronized To-do/Goal labels, making a real update appear missing. */
    if (show_right && right < 29) right = 29;
    center = usable_width - left - right;
    if (center < 16 && show_right) {
        show_right = false;
        right = 0;
        center = usable_width - left;
    }
    if (center < 16 && show_left) {
        show_left = false;
        left = 0;
        center = usable_width - right;
    }
    if (!show_left) left = 0;
    if (!show_right) right = 0;
    center = usable_width - left - right;
    int rows = height - 6; /* 3 header rows, body to composer, 3-row composer */
    if (rows > TUI_RENDER_ROWS) rows = TUI_RENDER_ROWS;
    if (rows < 1) rows = 1;
    tui->left_width = left;
    tui->center_width = center;
    tui->right_width = right;
    tui->content_rows = rows;

    char (*panels)[TUI_RENDER_ROWS][TUI_RENDER_BYTES] = calloc(3, sizeof(*panels));
    bool (*marks)[TUI_RENDER_ROWS] = calloc(3, sizeof(*marks));
    if (!panels || !marks) {
        free(panels);
        free(marks);
        return;
    }
    (void)make_left_panel(tui, panels[0], marks[0], rows);
    (void)make_center_panel(tui, panels[1], marks[1], rows, center);
    (void)make_right_panel(tui, panels[2], marks[2], rows, right > 0 ? right : center);

    char title[512];
    (void)snprintf(title, sizeof(title), "ASGEND · %s", tui->header[0] ? tui->header : "Рабочее пространство");
    frame_append_top_border(tui_current_frame[0], usable_width, title, 236, 45);
    const char *mode = tui->display_mode == ASGEND_TUI_DISPLAY_ALL_EXPANDED ? "все раскрыты" :
                       tui->display_mode == ASGEND_TUI_DISPLAY_ALL_COLLAPSED ? "все скрыты" : "динамика";
    char status[1024];
    (void)snprintf(status, sizeof(status), "Задача: %s · %s · %s · ↑/↓ навигация · Tab панели",
                   tui->task_id[0] ? tui->task_id : "обзор", tui->status, mode);
    if (tui->history_truncated) {
        size_t used = strlen(status);
        if (used < sizeof(status) - 1)
            (void)snprintf(status + used, sizeof(status) - used,
                           " · история ограничена, пропущено плиток: %llu",
                           (unsigned long long)tui->dropped_tile_count);
    }
    frame_append_status(tui_current_frame[1], usable_width, status);

    char left_title[512], center_title[256], right_title[512];
    const char *project = tui->project_path[0] ? tui->project_path :
                          (tui->workspace_path[0] ? tui->workspace_path : ".");
    const char *leaf = strrchr(project, '/');
    leaf = leaf ? leaf + 1 : project;
    if (!*leaf) leaf = "/";
    /* Use the current directory's basename so a long absolute path doesn't
     * consume the whole tile header or hide the part that changes on navigation. */
    (void)snprintf(left_title, sizeof(left_title), "Проект · %.220s", leaf);
    (void)snprintf(center_title, sizeof(center_title), "%s", tui->welcome_screen ? "Активные задачи" :
                   (tui->task_id[0] ? "Чат задачи" : "Чат · обзор"));
    if (tui->preview_content) {
        const char *preview_leaf = strrchr(tui->preview_path, '/');
        preview_leaf = preview_leaf ? preview_leaf + 1 : tui->preview_path;
        char with_prefix[512];
        (void)snprintf(with_prefix, sizeof(with_prefix), "Preview · %.220s", preview_leaf);
        /* On a narrow right panel, preserve the filename instead of clipping
         * the end of a long project-relative path. The body also shows context. */
        int title_capacity = right - 5;
        if (text_display_columns(with_prefix) <= title_capacity)
            (void)snprintf(right_title, sizeof(right_title), "%s", with_prefix);
        else
            (void)snprintf(right_title, sizeof(right_title), "%.220s", preview_leaf);
    } else {
        (void)snprintf(right_title, sizeof(right_title), "Goals · MCP");
    }
    if (show_left) frame_append_top_border(tui_current_frame[2], left, left_title, 235, 45);
    frame_append_top_border(tui_current_frame[2], center, center_title, 235, 81);
    if (show_right) frame_append_top_border(tui_current_frame[2], right, right_title, 235, 39);

    for (int r = 0; r < rows; ++r) {
        int screen_row = 3 + r;
        if (show_left) frame_append_panel_content(tui_current_frame[screen_row], left, panels[0][r], marks[0][r], 235, 255);
        int chat_bg = 235, chat_fg = 81;
        if (tui_center_row_palette[r] == TUI_ROW_USER) { chat_bg = 22; chat_fg = 157; }
        else if (tui_center_row_palette[r] == TUI_ROW_ORCHESTRATOR) { chat_bg = 236; chat_fg = 245; }
        else if (tui_center_row_palette[r] == TUI_ROW_TOOL) { chat_bg = 238; chat_fg = 220; }
        else if (tui_center_row_palette[r] == TUI_ROW_QUESTION) { chat_bg = 24; chat_fg = 159; }
        frame_append_panel_content(tui_current_frame[screen_row], center, panels[1][r], marks[1][r], chat_bg, chat_fg);
        int right_bg = 235, right_fg = 220;
        if (tui_right_row_palette[r] == TUI_ROW_ADDED) { right_bg = 22; right_fg = 157; }
        else if (tui_right_row_palette[r] == TUI_ROW_REMOVED) { right_bg = 235; right_fg = 210; }
        else if (tui_right_row_palette[r] == TUI_ROW_SECTION) { right_bg = 236; right_fg = 45; }
        else if (tui_right_row_palette[r] == TUI_ROW_MCP_ON) { right_bg = 235; right_fg = 157; }
        else if (tui_right_row_palette[r] == TUI_ROW_MCP_OFF) { right_bg = 235; right_fg = 210; }
        if (show_right) frame_append_panel_content(tui_current_frame[screen_row], right, panels[2][r], marks[2][r], right_bg, right_fg);
    }

    /* The prompt is a real rectangular input component centered under the chat.
     * The side columns remain quiet so only the active component attracts attention. */
    int prompt_width = center;
    int prompt_start = show_left ? left : 0;
    if (prompt_width < 12) { prompt_width = usable_width; prompt_start = 0; }
    if (prompt_start + prompt_width > usable_width) prompt_start = usable_width - prompt_width;
    int prompt_top = height - 3;
    int prompt_body = height - 2;
    int prompt_bottom = height - 1;
    if (prompt_top >= 0 && prompt_top < height) {
        for (int i = 0; i < prompt_start; ++i) frame_append(tui_current_frame[prompt_top], TUI_FRAME_BYTES, " ");
        frame_append_top_border(tui_current_frame[prompt_top], prompt_width,
                                 tui->question_active ? "Ответ на вопрос" :
                                 (tui->input_mode ? (tui->input_prompt[0] ? tui->input_prompt : "Новый запрос · Enter отправить · Esc отменить") : "Запрос · нажмите / для ввода"),
                                 238, 45);
        for (int i = prompt_start + prompt_width; i < usable_width; ++i) frame_append(tui_current_frame[prompt_top], TUI_FRAME_BYTES, " ");
        frame_append(tui_current_frame[prompt_top], TUI_FRAME_BYTES, "\033[0m");
    }
    if (prompt_body >= 0 && prompt_body < height) {
        for (int i = 0; i < prompt_start; ++i) frame_append(tui_current_frame[prompt_body], TUI_FRAME_BYTES, " ");
        frame_append_style(tui_current_frame[prompt_body], 238, 255);
        frame_append(tui_current_frame[prompt_body], TUI_FRAME_BYTES, "  ");
        char prompt[ASGEND_TUI_INPUT_MAX + 80];
        const char *placeholder = tui->question_active ? "↑/↓ выбрать · Enter подтвердить · можно ввести ответ" :
                                  tui->input_mode ? (tui->input_prompt[0] ? tui->input_prompt : "Введите запрос...") : "Напишите / чтобы начать задачу";
        if (tui->input_length > 0)
            (void)snprintf(prompt, sizeof(prompt), "> %s", tui->input_line);
        else
            (void)snprintf(prompt, sizeof(prompt), "> %s", placeholder);
        frame_append_clipped(tui_current_frame[prompt_body], TUI_FRAME_BYTES, prompt, prompt_width - 4);
        frame_append(tui_current_frame[prompt_body], TUI_FRAME_BYTES, "  \033[0m");
        for (int i = prompt_start + prompt_width; i < usable_width; ++i) frame_append(tui_current_frame[prompt_body], TUI_FRAME_BYTES, " ");
    }
    if (prompt_bottom >= 0 && prompt_bottom < height) {
        for (int i = 0; i < prompt_start; ++i) frame_append(tui_current_frame[prompt_bottom], TUI_FRAME_BYTES, " ");
        frame_append_bottom_border(tui_current_frame[prompt_bottom], prompt_width,
                                   tui->question_active ? "Enter выбрать · Esc отложить" :
                                   "Enter отправить · Tab панель · ? помощь · q выход",
                                   238, 45);
        for (int i = prompt_start + prompt_width; i < usable_width; ++i) frame_append(tui_current_frame[prompt_bottom], TUI_FRAME_BYTES, " ");
    }

    tui_render_session_picker(tui, usable_width, height, prompt_top);
    frame_present(tui, usable_width, height);
    if (tui->session_picker_active) {
        printf("\033[%d;%dH\033[2 q\033[?25h", tui->session_cursor_row + 1, tui->session_cursor_col + 1);
        fflush(stdout);
    } else if (tui->input_mode) {
        size_t cursor_columns = tui->input_length > 0 ? (size_t)text_display_columns(tui->input_line) : 0;
        size_t max_cursor = prompt_width > 6 ? (size_t)(prompt_width - 6) : 0;
        if (cursor_columns > max_cursor) cursor_columns = max_cursor;
        int cursor_x = prompt_start + 4 + (int)cursor_columns;
        if (cursor_x > usable_width) cursor_x = usable_width;
        printf("\033[%d;%dH\033[2 q\033[?25h", prompt_body + 1, cursor_x + 1);
        fflush(stdout);
    } else {
        fputs("\033[?25l", stdout);
        fflush(stdout);
    }
    free(panels);
    free(marks);
}

void asgend_tui_batch_begin(AsgendTui *tui) {
    if (tui && tui->render_defer_depth < UINT_MAX) ++tui->render_defer_depth;
}

void asgend_tui_batch_end(AsgendTui *tui) {
    if (!tui || tui->render_defer_depth == 0) return;
    --tui->render_defer_depth;
    if (tui->render_defer_depth == 0) {
        if (tui->follow_stream_tail_pending && tui->follow_stream_tail)
            center_follow_stream_tail(tui);
        else
            tui->follow_stream_tail_pending = false;
        if (tui->render_pending) {
            tui->render_pending = false;
            asgend_tui_render(tui);
        }
    }
}

int asgend_tui_init(AsgendTui *tui, const char *header) {
    if (!tui) return -1;
    memset(tui, 0, sizeof(*tui));
    tui_has_pending_byte = false;
    tui_pending_byte = 0;
    tui_previous_valid = false;
    tui_previous_owner = NULL;
    text_copy(tui->header, sizeof(tui->header), header ? header : "ASGEND");
    text_copy(tui->status, sizeof(tui->status), "Ожидание команды");
    tui->display_mode = ASGEND_TUI_DISPLAY_DYNAMIC;
    tui->focused_pane = 1; /* Chat is the default keyboard focus. */
    tui->follow_stream_tail = true;
    tui->tile_capacity = 16;
    tui->tiles = calloc(tui->tile_capacity, sizeof(*tui->tiles));
    if (!tui->tiles) return -1;
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return 0;
    tui->active = true;
    tui->terminal_saved = tcgetattr(STDIN_FILENO, &tui->original_termios) == 0;
    tui_install_winch_handler();
    tui_enable_raw(tui);
    fputs("\033[?1049h\033[?25l\033[2J\033[H", stdout);
    tui_mouse_enable();
    fflush(stdout);
    tui_dimensions(tui);
    asgend_tui_render(tui);
    return 1;
}

void asgend_tui_shutdown(AsgendTui *tui) {
    if (!tui) return;
    if (tui->active) {
        if (tui->terminal_saved) (void)tcsetattr(STDIN_FILENO, TCSANOW, &tui->original_termios);
        tui_mouse_disable();
        fputs("\033[0 q\033[?25h\033[?1049l", stdout);
        fflush(stdout);
    }
    tui_restore_winch_handler();
    tui_has_pending_byte = false;
    if (tui_previous_owner == tui) { tui_previous_valid = false; tui_previous_owner = NULL; }
    for (size_t i = 0; i < tui->tile_count; ++i) free(tui->tiles[i].body);
    free(tui->preview_content);
    free(tui->tiles);
    memset(tui, 0, sizeof(*tui));
}

void asgend_tui_suspend(AsgendTui *tui) {
    if (!tui || !tui->active || tui->suspended) return;
    if (tui->terminal_saved) (void)tcsetattr(STDIN_FILENO, TCSANOW, &tui->original_termios);
    tui_mouse_disable();
    fputs("\033[0 q\033[?25h\033[?1049l", stdout);
    fflush(stdout);
    tui->suspended = true;
}

void asgend_tui_resume(AsgendTui *tui) {
    if (!tui || !tui->active || !tui->suspended) return;
    fputs("\033[?1049h\033[?25l\033[2J\033[H", stdout);
    tui_mouse_enable();
    fflush(stdout);
    tui->suspended = false;
    tui_previous_valid = false;
    tui_previous_owner = NULL;
    tui_enable_raw(tui);
    tui_dimensions(tui);
    asgend_tui_render(tui);
}

void asgend_tui_set_header(AsgendTui *tui, const char *header) {
    if (!tui) return;
    text_copy(tui->header, sizeof(tui->header), header);
    asgend_tui_render(tui);
}

void asgend_tui_clear_tiles(AsgendTui *tui) {
    if (!tui) return;
    for (size_t i = 0; i < tui->tile_count; ++i) free(tui->tiles[i].body);
    tui->tile_count = 0;
    tui->selected_tile = 0;
    tui->scroll_offset = 0;
    tui->follow_stream_tail = true;
    tui->follow_stream_tail_pending = false;
    tui->scroll_offset = 0;
    asgend_tui_render(tui);
}

void asgend_tui_set_task(AsgendTui *tui, const char *task_id, const char *status) {
    if (!tui) return;
    text_copy(tui->task_id, sizeof(tui->task_id), task_id);
    text_copy(tui->status, sizeof(tui->status), status);
    asgend_tui_render(tui);
}

void asgend_tui_set_tasks(AsgendTui *tui, const AsgendTuiTaskItem *tasks, size_t count) {
    if (!tui) return;
    if (count > ASGEND_TUI_ITEMS_MAX) count = ASGEND_TUI_ITEMS_MAX;
    if (tasks && count) memcpy(tui->tasks, tasks, count * sizeof(*tasks));
    tui->task_count = tasks ? count : 0;
    if (tui->selected_left_index >= tui->task_count + tui->todo_count) tui->selected_left_index = 0;
    tui_rebuild_task_filters(tui);
    asgend_tui_render(tui);
}

void asgend_tui_set_welcome(AsgendTui *tui, bool enabled) {
    if (!tui) return;
    tui->welcome_screen = enabled;
    if (enabled) {
        tui->session_picker_active = false;
        tui->input_mode = false;
        tui->input_prompt[0] = '\0';
    }
    asgend_tui_render(tui);
}

void asgend_tui_open_session_picker(AsgendTui *tui, const char *initial_query) {
    if (!tui) return;
    tui->session_picker_active = true;
    tui->session_query[0] = '\0';
    if (initial_query) text_copy(tui->session_query, sizeof(tui->session_query), initial_query);
    tui->session_selected = 0;
    tui->session_scroll_offset = 0;
    tui_rebuild_task_filters(tui);
    asgend_tui_render(tui);
}

void asgend_tui_close_session_picker(AsgendTui *tui) {
    if (!tui) return;
    tui->session_picker_active = false;
    tui->focused_pane = 1;
    tui->session_query[0] = '\0';
    tui->session_selected = 0;
    tui->session_scroll_offset = 0;
    tui->session_cursor_row = 0;
    tui->session_cursor_col = 0;
    asgend_tui_render(tui);
}

bool asgend_tui_session_picker_active(const AsgendTui *tui) { return tui && tui->session_picker_active; }
bool asgend_tui_welcome_active(const AsgendTui *tui) { return tui && tui->welcome_screen; }

bool asgend_tui_selected_session(AsgendTui *tui, AsgendTuiTaskItem *out) {
    if (!tui || !out) return false;
    size_t source_index = 0;
    if (tui->session_picker_active) {
        if (tui->session_selected >= tui->session_filtered_count) return false;
        source_index = tui->session_filtered_indices[tui->session_selected];
    } else if (tui->welcome_screen) {
        if (tui->selected_task >= tui->welcome_filtered_count) return false;
        source_index = tui->welcome_filtered_indices[tui->selected_task];
    } else return false;
    if (source_index >= tui->task_count) return false;
    *out = tui->tasks[source_index];
    return true;
}

bool asgend_tui_session_query_append(AsgendTui *tui, const char *text) {
    if (!tui || !tui->session_picker_active || !text || !*text) return false;
    size_t len = strlen(text), used = strlen(tui->session_query);
    if (len + used >= sizeof(tui->session_query)) return false;
    memcpy(tui->session_query + used, text, len + 1);
    tui_rebuild_task_filters(tui);
    tui->session_selected = 0;
    tui->session_scroll_offset = 0;
    asgend_tui_render(tui);
    return true;
}

void asgend_tui_session_query_backspace(AsgendTui *tui) {
    if (!tui || !tui->session_picker_active) return;
    size_t len = strlen(tui->session_query);
    if (len == 0) return;
    do { --len; } while (len > 0 && (((unsigned char)tui->session_query[len] & 0xc0U) == 0x80U));
    tui->session_query[len] = '\0';
    tui_rebuild_task_filters(tui);
    tui->session_selected = 0;
    tui->session_scroll_offset = 0;
    asgend_tui_render(tui);
}

static int tui_file_item_compare(const void *left, const void *right) {
    const AsgendTuiFileItem *a = left;
    const AsgendTuiFileItem *b = right;
    /* Directory-first ordering makes navigation predictable regardless of
       the underlying filesystem's readdir() order. Keep names case-insensitive
       for display, then use byte order as a deterministic tie-breaker. */
    int a_rank = strcmp(a->kind, "directory") == 0 ? 0 :
                 strcmp(a->kind, "file") == 0 ? 1 : 2;
    int b_rank = strcmp(b->kind, "directory") == 0 ? 0 :
                 strcmp(b->kind, "file") == 0 ? 1 : 2;
    if (a_rank != b_rank) return a_rank - b_rank;
    int name_order = strcasecmp(a->path, b->path);
    if (name_order != 0) return name_order;
    return strcmp(a->path, b->path);
}

void asgend_tui_set_files(AsgendTui *tui, const AsgendTuiFileItem *files, size_t count) {
    if (!tui) return;
    if (count > ASGEND_TUI_ITEMS_MAX) count = ASGEND_TUI_ITEMS_MAX;
    if (files && count) memcpy(tui->files, files, count * sizeof(*files));
    if (files && count > 1) qsort(tui->files, count, sizeof(tui->files[0]), tui_file_item_compare);
    tui->file_count = files ? count : 0;
    if (tui->file_count == 0) {
        tui->selected_file_index = 0;
        tui->file_scroll_offset = 0;
    } else {
        if (tui->selected_file_index >= tui->file_count) tui->selected_file_index = tui->file_count - 1;
        if (tui->selected_file_index < tui->file_scroll_offset)
            tui->file_scroll_offset = tui->selected_file_index;
        size_t visible = tui_file_visible_items(tui);
        if (tui->selected_file_index >= tui->file_scroll_offset + visible)
            tui->file_scroll_offset = tui->selected_file_index - visible + 1;
    }
    asgend_tui_render(tui);
}

void asgend_tui_set_workspace_path(AsgendTui *tui, const char *path) {
    if (!tui) return;
    char normalized[ASGEND_TUI_PATH_MAX];
    text_copy(normalized, sizeof(normalized), path);
    while (normalized[0] == '.' && normalized[1] == '/')
        memmove(normalized, normalized + 2, strlen(normalized + 2) + 1);
    while (normalized[0] == '/')
        memmove(normalized, normalized + 1, strlen(normalized + 1) + 1);
    if (strcmp(tui->workspace_path, normalized) == 0) return;
    text_copy(tui->workspace_path, sizeof(tui->workspace_path), normalized);
    tui->selected_file_index = 0;
    tui->file_scroll_offset = 0;
    asgend_tui_close_file_preview(tui);
    asgend_tui_render(tui);
}

const char *asgend_tui_workspace_path(const AsgendTui *tui) {
    return tui ? tui->workspace_path : "";
}

void asgend_tui_set_project_path(AsgendTui *tui, const char *path) {
    if (!tui) return;
    text_copy(tui->project_path, sizeof(tui->project_path), path);
    asgend_tui_render(tui);
}

void asgend_tui_set_mcp_servers(AsgendTui *tui, const AsgendTuiMcpItem *items, size_t count) {
    if (!tui) return;
    if (count > ASGEND_TUI_MCP_MAX) count = ASGEND_TUI_MCP_MAX;
    if (items && count) memcpy(tui->mcp_servers, items, count * sizeof(*items));
    tui->mcp_server_count = items ? count : 0;
    asgend_tui_render(tui);
}

void asgend_tui_input_begin(AsgendTui *tui) {
    if (!tui) return;
    tui->input_mode = true;
    tui->input_length = strlen(tui->input_line);
    tui->focused_pane = 1;
    asgend_tui_render(tui);
}

void asgend_tui_input_cancel(AsgendTui *tui) {
    if (!tui) return;
    tui->input_mode = false;
    tui->input_length = 0;
    tui->input_line[0] = '\0';
    asgend_tui_render(tui);
}

bool asgend_tui_input_append(AsgendTui *tui, const char *text) {
    if (!tui || !text || !tui->input_mode) return false;
    size_t length = strlen(text);
    if (length == 0 || tui->input_length + length >= sizeof(tui->input_line)) return false;
    memcpy(tui->input_line + tui->input_length, text, length);
    tui->input_length += length;
    tui->input_line[tui->input_length] = '\0';
    asgend_tui_render(tui);
    return true;
}

void asgend_tui_input_backspace(AsgendTui *tui) {
    if (!tui || !tui->input_mode || tui->input_length == 0) return;
    do {
        --tui->input_length;
    } while (tui->input_length > 0 &&
             (((unsigned char)tui->input_line[tui->input_length] & 0xC0U) == 0x80U));
    tui->input_line[tui->input_length] = '\0';
    asgend_tui_render(tui);
}

char *asgend_tui_input_take(AsgendTui *tui) {
    if (!tui) return NULL;
    char *copy = strdup(tui->input_line);
    tui->input_mode = false;
    tui->input_length = 0;
    tui->input_line[0] = '\0';
    tui->input_prompt[0] = '\0';
    asgend_tui_render(tui);
    return copy;
}

void asgend_tui_set_question(AsgendTui *tui, const char *question,
                             const char *const *options, size_t option_count) {
    if (!tui) return;
    text_copy(tui->question_text, sizeof(tui->question_text), question ? question : "Агенту нужна дополнительная информация.");
    if (option_count > ASGEND_TUI_QUESTION_OPTIONS_MAX) option_count = ASGEND_TUI_QUESTION_OPTIONS_MAX;
    tui->question_option_count = option_count;
    tui->question_selected = 0;
    for (size_t i = 0; i < option_count; ++i)
        text_copy(tui->question_options[i], sizeof(tui->question_options[i]), options ? options[i] : "");
    tui->question_active = true;
    tui->focused_pane = 1;
    tui->input_mode = option_count == 0;
    tui->input_prompt[0] = '\0';
    tui->input_length = 0;
    tui->input_line[0] = '\0';
    asgend_tui_render(tui);
}

void asgend_tui_set_question_title(AsgendTui *tui, const char *title) {
    if (!tui) return;
    text_copy(tui->question_title, sizeof(tui->question_title), title ? title : "Уточнение от модели");
    asgend_tui_render(tui);
}

void asgend_tui_clear_question(AsgendTui *tui) {
    if (!tui) return;
    tui->question_active = false;
    tui->question_text[0] = '\0';
    tui->question_title[0] = '\0';
    tui->question_option_count = 0;
    tui->question_selected = 0;
    asgend_tui_render(tui);
}

bool asgend_tui_question_active(const AsgendTui *tui) {
    return tui && tui->question_active;
}

const char *asgend_tui_question_answer(const AsgendTui *tui) {
    if (!tui || !tui->question_active) return "";
    if (tui->question_option_count > 0 && tui->question_selected < tui->question_option_count)
        return tui->question_options[tui->question_selected];
    return tui->input_line;
}

bool asgend_tui_selected_file(AsgendTui *tui, AsgendTuiFileItem *out) {
    if (!tui || !out || tui->selected_file_index >= tui->file_count) return false;
    const AsgendTuiFileItem *file = &tui->files[tui->selected_file_index];
    memset(out, 0, sizeof(*out));
    if (tui->workspace_path[0]) {
        int n = snprintf(out->path, sizeof(out->path), "%s/%s", tui->workspace_path, file->path);
        if (n < 0 || (size_t)n >= sizeof(out->path)) return false;
    } else {
        text_copy(out->path, sizeof(out->path), file->path);
    }
    text_copy(out->kind, sizeof(out->kind), file->kind);
    out->size = file->size;
    return out->path[0] != '\0';
}

bool asgend_tui_enter_selected_directory(AsgendTui *tui) {
    if (!tui || tui->selected_file_index >= tui->file_count) return false;
    const AsgendTuiFileItem *file = &tui->files[tui->selected_file_index];
    if (strcmp(file->kind, "directory") != 0 || !file->path[0] ||
        strcmp(file->path, ".") == 0 || strcmp(file->path, "..") == 0 || strchr(file->path, '/'))
        return false;
    char next_path[ASGEND_TUI_PATH_MAX];
    int n = tui->workspace_path[0]
        ? snprintf(next_path, sizeof(next_path), "%s/%s", tui->workspace_path, file->path)
        : snprintf(next_path, sizeof(next_path), "%s", file->path);
    if (n < 0 || (size_t)n >= sizeof(next_path)) return false;
    asgend_tui_set_workspace_path(tui, next_path);
    return true;
}

bool asgend_tui_parent_directory(AsgendTui *tui) {
    if (!tui || !tui->workspace_path[0]) return false;
    char parent[ASGEND_TUI_PATH_MAX];
    text_copy(parent, sizeof(parent), tui->workspace_path);
    char *slash = strrchr(parent, '/');
    if (slash) *slash = '\0';
    else parent[0] = '\0';
    asgend_tui_set_workspace_path(tui, parent);
    return true;
}

void asgend_tui_show_file_preview(AsgendTui *tui, const char *path, const char *content) {
    if (!tui) return;
    const char *text = content ? content : "";
    size_t length = strlen(text);
    bool truncated = length > TUI_BODY_MAX - 1;
    if (truncated) length = TUI_BODY_MAX - 1;
    char *copy = malloc(length + (truncated ? 48U : 1U));
    if (!copy) return;
    memcpy(copy, text, length);
    copy[length] = '\0';
    if (truncated) (void)snprintf(copy + length, 48U, "\n\n[предпросмотр урезан]");
    free(tui->preview_content);
    tui->preview_content = copy;
    tui->preview_length = strlen(copy);
    text_copy(tui->preview_path, sizeof(tui->preview_path), path ? path : "(файл)");
    tui->preview_file_size = (int64_t)tui->preview_length;
    tui->preview_chunk_offset = 0;
    tui->preview_next_offset = tui->preview_file_size;
    tui->preview_eof = true;
    tui->preview_no_wrap = false;
    tui->preview_horizontal_offset = 0;
    tui->preview_line_offset = 0;
    tui->preview_scroll_direction = 0;
    tui->focused_pane = 2;
    asgend_tui_render(tui);
}

void asgend_tui_show_file_preview_chunk(AsgendTui *tui, const char *path, const char *content,
                                        int64_t file_size, int64_t chunk_offset,
                                        int64_t next_offset, bool eof, bool start_at_end) {
    if (!tui) return;
    const char *text = content ? content : "";
    size_t length = strlen(text);
    char *copy = malloc(length + 1);
    if (!copy) return;
    memcpy(copy, text, length + 1);
    free(tui->preview_content);
    tui->preview_content = copy;
    tui->preview_length = length;
    text_copy(tui->preview_path, sizeof(tui->preview_path), path ? path : "(файл)");
    tui->preview_file_size = file_size;
    tui->preview_chunk_offset = chunk_offset;
    tui->preview_next_offset = next_offset;
    tui->preview_eof = eof;
    tui->preview_no_wrap = false;
    tui->preview_horizontal_offset = 0;
    tui->preview_line_offset = 0;
    tui->preview_scroll_direction = 0;
    tui->focused_pane = 2;
    if (start_at_end) {
        tui_dimensions(tui);
        int columns = tui->right_width > 2 ? tui->right_width - 2 : 8;
        size_t lines = count_preview_lines(tui->preview_content, columns, tui->preview_no_wrap);
        size_t visible = tui->content_rows > 2 ? (size_t)(tui->content_rows - 2) : 1;
        tui->preview_line_offset = lines > visible ? lines - visible : 0;
    }
    asgend_tui_render(tui);
}

void asgend_tui_close_file_preview(AsgendTui *tui) {
    if (!tui || !tui->preview_content) return;
    free(tui->preview_content);
    tui->preview_content = NULL;
    tui->preview_length = 0;
    tui->preview_path[0] = '\0';
    tui->preview_line_offset = 0;
    tui->preview_horizontal_offset = 0;
    tui->preview_file_size = 0;
    tui->preview_chunk_offset = 0;
    tui->preview_next_offset = 0;
    tui->preview_eof = true;
    tui->preview_no_wrap = false;
    tui->preview_scroll_direction = 0;
    asgend_tui_render(tui);
}

int asgend_tui_preview_stream_action(AsgendTui *tui) {
    if (!tui || !tui->preview_content) return 0;
    int direction = tui->preview_scroll_direction;
    tui->preview_scroll_direction = 0;
    if (direction < 0 && tui->preview_chunk_offset > 0 && tui->preview_line_offset == 0)
        return -1;
    if (direction > 0 && !tui->preview_eof) {
        int columns = tui->right_width > 2 ? tui->right_width - 2 : 8;
        size_t lines = count_preview_lines(tui->preview_content, columns, tui->preview_no_wrap);
        size_t visible = tui->content_rows > 2 ? (size_t)(tui->content_rows - 2) : 1;
        if (tui->preview_line_offset + visible >= lines) return 1;
    }
    return 0;
}

void asgend_tui_set_plan(AsgendTui *tui, const AsgendTuiPlanItem *items, size_t count) {
    if (!tui) return;
    tui->todo_count = 0;
    tui->goal_count = 0;
    for (size_t i = 0; items && i < count; ++i) {
        if (strcmp(items[i].kind, "goal") == 0) {
            if (tui->goal_count < ASGEND_TUI_ITEMS_MAX) tui->goals[tui->goal_count++] = items[i];
        } else if (tui->todo_count < ASGEND_TUI_ITEMS_MAX) {
            tui->todos[tui->todo_count++] = items[i];
        }
    }
    size_t focused_count = tui->selected_plan_is_goal ? tui->goal_count : tui->todo_count;
    if (focused_count == 0) {
        tui->selected_plan = 0;
        tui->selected_plan_is_goal = false;
    } else if (tui->selected_plan >= focused_count) {
        tui->selected_plan = 0;
    }
    if (tui->selected_left_index >= tui->task_count + tui->todo_count)
        tui->selected_left_index = tui->task_count;
    asgend_tui_render(tui);
}

static const char TUI_BODY_TRUNCATION_PREFIX[] = "[… начало вывода скрыто из-за лимита …]\n";

static bool tui_utf8_continuation(unsigned char value) {
    return (value & 0xC0U) == 0x80U;
}

/* Return a safe suffix start: never begin on a UTF-8 continuation byte. */
static size_t tui_utf8_suffix_start(const char *text, size_t length, size_t start) {
    if (!text) return length;
    if (start > length) start = length;
    while (start < length && tui_utf8_continuation((unsigned char)text[start])) ++start;
    return start;
}

static char *tui_body_copy_limited(const char *body, bool *truncated) {
    const char *source = body ? body : "";
    size_t length = strlen(source);
    if (length <= TUI_BODY_MAX) {
        if (truncated) *truncated = false;
        return strdup(source);
    }
    size_t marker_size = sizeof(TUI_BODY_TRUNCATION_PREFIX) - 1;
    size_t budget = TUI_BODY_MAX > marker_size ? TUI_BODY_MAX - marker_size : 0;
    size_t start = tui_utf8_suffix_start(source, length, length - budget);
    size_t tail_size = length - start;
    char *copy = malloc(marker_size + tail_size + 1);
    if (!copy) return NULL;
    memcpy(copy, TUI_BODY_TRUNCATION_PREFIX, marker_size);
    memcpy(copy + marker_size, source + start, tail_size);
    copy[marker_size + tail_size] = '\0';
    if (truncated) *truncated = true;
    return copy;
}

static char *tui_body_append_limited(const char *old_body, const char *fragment,
                                    bool *truncated, bool *did_truncate) {
    const char *old_text = old_body ? old_body : "";
    const char *new_text = fragment ? fragment : "";
    size_t old_size = strlen(old_text), new_size = strlen(new_text);
    if (did_truncate) *did_truncate = false;
    if (old_size <= TUI_BODY_MAX && new_size <= TUI_BODY_MAX - old_size) {
        char *copy = realloc((void *)old_body, old_size + new_size + 1);
        if (!copy) return NULL;
        memcpy(copy + old_size, new_text, new_size + 1);
        if (truncated) *truncated = false;
        return copy;
    }

    size_t marker_size = sizeof(TUI_BODY_TRUNCATION_PREFIX) - 1;
    size_t budget = TUI_BODY_MAX > marker_size ? TUI_BODY_MAX - marker_size : 0;
    /* Keep the most useful part of the stream: recent old context plus the
     * latest fragment. This is explicitly marked, never silently truncated. */
    size_t new_keep = new_size < (budget * 2U) / 3U ? new_size : (budget * 2U) / 3U;
    size_t new_start = tui_utf8_suffix_start(new_text, new_size, new_size - new_keep);
    new_keep = new_size - new_start;
    size_t old_keep_budget = budget > new_keep ? budget - new_keep : 0;
    size_t old_start = old_size > old_keep_budget ? old_size - old_keep_budget : 0;
    old_start = tui_utf8_suffix_start(old_text, old_size, old_start);
    size_t old_keep = old_size - old_start;
    if (old_keep + new_keep > budget) {
        size_t excess = old_keep + new_keep - budget;
        old_start = tui_utf8_suffix_start(old_text, old_size, old_start + excess);
        old_keep = old_size - old_start;
    }
    char *copy = malloc(marker_size + old_keep + new_keep + 1);
    if (!copy) return NULL;
    memcpy(copy, TUI_BODY_TRUNCATION_PREFIX, marker_size);
    memcpy(copy + marker_size, old_text + old_start, old_keep);
    memcpy(copy + marker_size + old_keep, new_text + new_start, new_keep);
    copy[marker_size + old_keep + new_keep] = '\0';
    if (truncated) *truncated = true;
    if (did_truncate) *did_truncate = true;
    return copy;
}

int asgend_tui_upsert_tile(AsgendTui *tui, const char *key, AsgendTuiTileKind kind,
                           const char *title, const char *body, bool append,
                           bool streaming, bool expanded) {
    if (!tui || !key || !*key) return -1;
    int found = tile_find(tui, key);
    AsgendTuiTile *tile = NULL;
    if (found < 0 && streaming && tui->display_mode == ASGEND_TUI_DISPLAY_DYNAMIC) {
        for (size_t i = 0; i < tui->tile_count; ++i) {
            if (!tui->tiles[i].streaming && tui->tiles[i].kind != ASGEND_TUI_TILE_CHAT)
                tui->tiles[i].expanded = false;
            if (kind == ASGEND_TUI_TILE_CHAT && tui->tiles[i].kind == ASGEND_TUI_TILE_CHAT)
                tui->tiles[i].expanded = false;
        }
    }
    if (found >= 0) {
        tile = &tui->tiles[found];
    } else {
        if (tui->tile_count >= TUI_MAX_TILES) {
            size_t victim = SIZE_MAX;
            /* Prefer removing completed transient/non-chat tiles. Never evict
             * an active stream merely because a new event arrived. */
            for (size_t i = 0; i < tui->tile_count; ++i) {
                if (!tui->tiles[i].streaming && tui->tiles[i].kind != ASGEND_TUI_TILE_CHAT) {
                    victim = i;
                    break;
                }
            }
            /* If all transient cards are gone, an old completed chat tile is
             * the next least harmful candidate. */
            if (victim == SIZE_MAX) {
                for (size_t i = 0; i < tui->tile_count; ++i) {
                    if (!tui->tiles[i].streaming) { victim = i; break; }
                }
            }
            if (victim == SIZE_MAX) {
                tui->history_truncated = true;
                ++tui->dropped_tile_count;
                if (tui->active && !tui->suspended) asgend_tui_render(tui);
                return -1;
            }
            free(tui->tiles[victim].body);
            memmove(tui->tiles + victim, tui->tiles + victim + 1,
                    (tui->tile_count - victim - 1) * sizeof(*tui->tiles));
            --tui->tile_count;
            memset(&tui->tiles[tui->tile_count], 0, sizeof(*tui->tiles));
            if (tui->selected_tile > victim) --tui->selected_tile;
            else if (tui->selected_tile == victim && tui->selected_tile >= tui->tile_count)
                tui->selected_tile = tui->tile_count ? tui->tile_count - 1 : 0;
            tui->history_truncated = true;
            ++tui->dropped_tile_count;
        }
        if (tui->tile_count == tui->tile_capacity) {
            size_t next_capacity = tui->tile_capacity * 2;
            AsgendTuiTile *next = realloc(tui->tiles, next_capacity * sizeof(*next));
            if (!next) return -1;
            memset(next + tui->tile_capacity, 0, (next_capacity - tui->tile_capacity) * sizeof(*next));
            tui->tiles = next;
            tui->tile_capacity = next_capacity;
        }
        tile = &tui->tiles[tui->tile_count++];
        memset(tile, 0, sizeof(*tile));
        text_copy(tile->key, sizeof(tile->key), key);
        tile->sequence = ++tui->next_sequence;
        tile->expanded = expanded;
    }
    tile->kind = kind;
    text_copy(tile->title, sizeof(tile->title), title);
    if (!append || !tile->body) {
        bool truncated = false;
        char *copy = tui_body_copy_limited(body, &truncated);
        if (!copy) return -1;
        free(tile->body);
        tile->body = copy;
        tile->body_truncated = truncated;
    } else if (body && *body && !tile->body_truncated) {
        bool truncated = false;
        char *next = tui_body_append_limited(tile->body, body, &truncated, &truncated);
        if (!next) return -1;
        tile->body = next;
        tile->body_truncated = truncated;
    }
    tile->bytes = tile->body ? strlen(tile->body) : 0;
    tile->streaming = streaming;
    if (tui->display_mode == ASGEND_TUI_DISPLAY_ALL_EXPANDED) tile->expanded = true;
    else if (tui->display_mode == ASGEND_TUI_DISPLAY_ALL_COLLAPSED) tile->expanded = false;
    else if (tile->kind != ASGEND_TUI_TILE_CHAT && !streaming) tile->expanded = false;
    if (streaming && tui->display_mode == ASGEND_TUI_DISPLAY_DYNAMIC && tui->follow_stream_tail) {
        tui->selected_tile = (size_t)(tile - tui->tiles);
        tui->follow_stream_tail_pending = true;
        if (tui->render_defer_depth == 0) center_follow_stream_tail(tui);
    }
    if (tui->active && !tui->suspended) asgend_tui_render(tui);
    return 0;
}

void asgend_tui_set_display_mode(AsgendTui *tui, AsgendTuiDisplayMode mode) {
    if (!tui) return;
    tui->display_mode = mode;
    for (size_t i = 0; i < tui->tile_count; ++i) {
        if (mode == ASGEND_TUI_DISPLAY_ALL_EXPANDED) tui->tiles[i].expanded = true;
        else if (mode == ASGEND_TUI_DISPLAY_ALL_COLLAPSED) tui->tiles[i].expanded = false;
        else if (!tui->tiles[i].streaming && tui->tiles[i].kind != ASGEND_TUI_TILE_CHAT)
            tui->tiles[i].expanded = false;
    }
    asgend_tui_render(tui);
}

void asgend_tui_toggle_selected(AsgendTui *tui) {
    if (!tui || tui->tile_count == 0) return;
    if (tui->selected_tile >= tui->tile_count) tui->selected_tile = tui->tile_count - 1;
    tui->tiles[tui->selected_tile].expanded = !tui->tiles[tui->selected_tile].expanded;
    asgend_tui_render(tui);
}

void asgend_tui_move_selection(AsgendTui *tui, int direction) {
    if (!tui) return;
    if (tui->session_picker_active) {
        if (direction < 0 && tui->session_selected > 0) --tui->session_selected;
        else if (direction > 0 && tui->session_selected + 1 < tui->session_filtered_count) ++tui->session_selected;
        asgend_tui_render(tui);
        return;
    }
    if (tui->welcome_screen) {
        if (direction < 0 && tui->selected_task > 0) --tui->selected_task;
        else if (direction > 0 && tui->selected_task + 1 < tui->welcome_filtered_count) ++tui->selected_task;
        asgend_tui_render(tui);
        return;
    }
    if (tui->question_active) {
        if (tui->question_option_count > 0) {
            if (direction < 0 && tui->question_selected > 0) --tui->question_selected;
            else if (direction > 0 && tui->question_selected + 1 < tui->question_option_count)
                ++tui->question_selected;
        } else if (tui->input_mode) {
            if (direction < 0) tui->scroll_offset = tui->scroll_offset > 0 ? tui->scroll_offset - 1 : 0;
            else if (tui->scroll_offset < SIZE_MAX) ++tui->scroll_offset;
        }
        asgend_tui_render(tui);
        return;
    }
    if (tui->focused_pane == 0 && tui->file_count > 0) {
        if (direction < 0 && tui->selected_file_index > 0) --tui->selected_file_index;
        else if (direction > 0 && tui->selected_file_index + 1 < tui->file_count) ++tui->selected_file_index;
        size_t visible = tui_file_visible_items(tui);
        if (tui->selected_file_index < tui->file_scroll_offset)
            tui->file_scroll_offset = tui->selected_file_index;
        else if (tui->selected_file_index >= tui->file_scroll_offset + visible)
            tui->file_scroll_offset = tui->selected_file_index - visible + 1;
    } else if (tui->focused_pane == 2 && tui->preview_content) {
        tui->preview_scroll_direction = direction;
        if (direction < 0) tui->preview_line_offset = tui->preview_line_offset > 3 ? tui->preview_line_offset - 3 : 0;
        else if (tui->preview_line_offset <= SIZE_MAX - 3) tui->preview_line_offset += 3;
    } else if (tui->focused_pane == 2) {
        size_t count = tui->selected_plan_is_goal ? tui->goal_count : tui->todo_count;
        if (count > 0) {
            if (direction < 0 && tui->selected_plan > 0) --tui->selected_plan;
            else if (direction > 0 && tui->selected_plan + 1 < count) ++tui->selected_plan;
        } else if (tui->selected_plan_is_goal && tui->todo_count > 0) {
            tui->selected_plan_is_goal = false; tui->selected_plan = 0;
        } else if (!tui->selected_plan_is_goal && tui->goal_count > 0) {
            tui->selected_plan_is_goal = true; tui->selected_plan = 0;
        }
    } else if (tui->focused_pane == 1 && tui->tile_count > 0) {
        if (direction < 0 && tui->selected_tile > 0) --tui->selected_tile;
        else if (direction > 0 && tui->selected_tile + 1 < tui->tile_count) ++tui->selected_tile;
        tui->follow_stream_tail = tui->selected_tile + 1 >= tui->tile_count;
        center_keep_selection_visible(tui);
    }
    asgend_tui_render(tui);
}

static int tui_poll_byte(int timeout_ms, unsigned char *out) {
    struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN};
    int rc = poll(&pfd, 1, timeout_ms);
    if (rc < 0 && errno == EINTR) return 0;
    if (rc <= 0 || !(pfd.revents & POLLIN)) return 0;
    return read(STDIN_FILENO, out, 1) == 1;
}

static void tui_mouse_hscroll(AsgendTui *tui, int x, int y, int direction) {
    (void)y;
    if (!tui || tui->focused_pane != 2 || !tui->preview_content) return;
    /* Horizontal panning switches the preview to no-wrap mode so moving the
     * viewport reveals the remainder of long source/code lines. */
    tui->preview_no_wrap = true;
    if (direction < 0) tui->preview_horizontal_offset = tui->preview_horizontal_offset > 4 ? tui->preview_horizontal_offset - 4 : 0;
    else if (tui->preview_horizontal_offset <= SIZE_MAX - 4) tui->preview_horizontal_offset += 4;
    (void)x;
    asgend_tui_render(tui);
}

static void tui_mouse_scroll(AsgendTui *tui, int x, int y, int direction) {
    if (!tui) return;
    if (x <= tui->left_width) tui->focused_pane = 0;
    else if (x <= tui->left_width + tui->center_width) tui->focused_pane = 1;
    else tui->focused_pane = 2;
    if (tui->question_active) {
        asgend_tui_move_selection(tui, direction);
        return;
    }
    if (tui->focused_pane == 0) {
        if (direction < 0 && tui->selected_file_index > 0) --tui->selected_file_index;
        else if (direction > 0 && tui->selected_file_index + 1 < tui->file_count) ++tui->selected_file_index;
        size_t visible = tui_file_visible_items(tui);
        if (tui->selected_file_index < tui->file_scroll_offset) tui->file_scroll_offset = tui->selected_file_index;
        else if (tui->selected_file_index >= tui->file_scroll_offset + visible) tui->file_scroll_offset = tui->selected_file_index - visible + 1;
    } else if (tui->focused_pane == 2 && tui->preview_content) {
        tui->preview_scroll_direction = direction;
        if (direction < 0) tui->preview_line_offset = tui->preview_line_offset > 3 ? tui->preview_line_offset - 3 : 0;
        else tui->preview_line_offset += 3;
    } else if (tui->focused_pane == 1) {
        size_t step = 3;
        if (direction < 0) tui->scroll_offset = tui->scroll_offset > step ? tui->scroll_offset - step : 0;
        else tui->scroll_offset += step;
        int columns = tui->center_width > 2 ? tui->center_width - 2 : 8;
        size_t total = center_total_visual_lines(tui, columns);
        size_t visible = tui->content_rows > 2 ? (size_t)(tui->content_rows - 2) : 1;
        size_t max_offset = total > visible ? total - visible : 0;
        if (tui->scroll_offset > max_offset) tui->scroll_offset = max_offset;
        tui->follow_stream_tail = tui->scroll_offset >= max_offset;
        tui->selected_tile = tile_for_visual_line(tui, tui->scroll_offset, columns);
    } else {
        size_t count = tui->selected_plan_is_goal ? tui->goal_count : tui->todo_count;
        if (count) {
            if (direction < 0 && tui->selected_plan > 0) --tui->selected_plan;
            else if (direction > 0 && tui->selected_plan + 1 < count) ++tui->selected_plan;
        }
    }
    (void)y;
    asgend_tui_render(tui);
}

static int tui_mouse_select(AsgendTui *tui, int x, int y) {
    if (!tui || x < 1 || y < 1) return 0;
    if (y >= tui->height - 2) {
        tui->input_mode = true;
        tui->focused_pane = 1;
        asgend_tui_render(tui);
        return 1;
    }
    if (tui->session_picker_active) {
        int body_row = y - 4;
        if (x >= tui->session_modal_x && x < tui->session_modal_x + tui->session_modal_width &&
            body_row >= tui->session_modal_list_top && tui->session_modal_visible_count > 0) {
            int relative = body_row - tui->session_modal_list_top;
            size_t visible_index = tui->session_scroll_offset + (size_t)(relative / 2);
            if ((relative % 2) == 0 && visible_index < tui->session_filtered_count) {
                tui->session_selected = visible_index;
                asgend_tui_render(tui);
                return 1;
            }
        }
        return 0;
    }
    if (y < 3) return 0;
    /* Panel content begins on terminal row 4 (rows 1-3 are title, status,
     * and panel headers). Convert the 1-based SGR coordinate into a zero-based
     * index within the panel body. */
    int row = y - 4;
    if (row < 0 || row >= tui->content_rows) return 0;
    if (x <= tui->left_width) {
        tui->focused_pane = 0;
        size_t index = tui->file_scroll_offset + (size_t)row;
        if (index < tui->file_count) tui->selected_file_index = index;
    } else if (x <= tui->left_width + tui->center_width) {
        tui->focused_pane = 1;
        if (tui->welcome_screen && !tui->session_picker_active && tui->welcome_visible_count > 0) {
            int index_row = row - tui->welcome_start_row - 1;
            if (index_row >= 0) {
                size_t visible_index = tui->welcome_scroll_offset + (size_t)(index_row / 2);
                if ((index_row % 2) == 0 && visible_index < tui->welcome_filtered_count)
                    tui->selected_task = visible_index;
            }
        } else if (!tui->question_active && tui->tile_count > 0) {
            int columns = tui->center_width > 2 ? tui->center_width - 2 : 8;
            tui->selected_tile = tile_for_visual_line(tui, tui->scroll_offset + (size_t)row, columns);
            tui->follow_stream_tail = tui->selected_tile + 1 >= tui->tile_count;
        }
        /* A click in the conversation hands focus to the rectangular composer. */
        if (!tui->question_active && !tui->welcome_screen) tui->input_mode = true;
    } else {
        tui->focused_pane = 2;
        if (tui->preview_content) {
            tui->preview_line_offset += (size_t)(row > 0 ? row : 0);
        } else {
            /* Map rows to the visible Goals / To-do sections. MCP rows are status-only. */
            int goal_header = 0;
            int goal_first = goal_header + 1;
            int goal_rows = tui->goal_count < 3 ? (int)tui->goal_count : 3;
            int todo_header = goal_first + (goal_rows > 0 ? goal_rows : 1);
            int todo_first = todo_header + 1;
            int todo_rows = tui->todo_count < 3 ? (int)tui->todo_count : 3;
            if (row >= goal_first && row < goal_first + goal_rows) {
                tui->selected_plan_is_goal = true;
                tui->selected_plan = (size_t)(row - goal_first);
            } else if (row >= todo_first && row < todo_first + todo_rows) {
                tui->selected_plan_is_goal = false;
                tui->selected_plan = (size_t)(row - todo_first);
            }
        }
    }
    asgend_tui_render(tui);
    return 1;
}
void asgend_tui_tick(AsgendTui *tui) {
    if (!tui || !tui->active || tui->suspended || tui->render_defer_depth > 0) return;
    for (size_t i = 0; i < tui->tile_count; ++i) {
        if (tui->tiles[i].streaming) {
            tui->spinner_frame = (tui->spinner_frame + 1U) % 4U;
            asgend_tui_render(tui);
            return;
        }
    }
}

int asgend_tui_poll_key(AsgendTui *tui) {
    if (!tui || !tui->active || tui->suspended) return 0;
    if (tui_resize_pending) {
        tui_resize_pending = 0;
        tui_dimensions(tui);
        asgend_tui_render(tui);
        return ASGEND_TUI_KEY_RESIZE;
    }
    unsigned char c = 0;
    if (tui_has_pending_byte) {
        c = tui_pending_byte;
        tui_has_pending_byte = false;
    } else if (!tui_poll_byte(0, &c)) return 0;
    if (c == 27) {
        unsigned char first = 0, second = 0;
        if (!tui_poll_byte(20, &first)) return ASGEND_TUI_KEY_ESCAPE;
        if (first != '[') {
            tui_pending_byte = first;
            tui_has_pending_byte = true;
            return ASGEND_TUI_KEY_ESCAPE;
        }
        if (tui_poll_byte(20, &second)) {
            if (second == 'A') { asgend_tui_move_selection(tui, -1); return ASGEND_TUI_KEY_NAVIGATION; }
            if (second == 'B') { asgend_tui_move_selection(tui, 1); return ASGEND_TUI_KEY_NAVIGATION; }
            if (second == '5' || second == '6') {
                unsigned char ending = 0;
                if (tui_poll_byte(20, &ending) && ending == '~') {
                    if (tui->focused_pane == 2 && tui->preview_content) {
                        tui->preview_scroll_direction = second == '5' ? -1 : 1;
                        size_t page = tui->content_rows > 4 ? (size_t)(tui->content_rows - 4) : 1;
                        if (second == '5') tui->preview_line_offset = tui->preview_line_offset > page
                            ? tui->preview_line_offset - page : 0;
                        else tui->preview_line_offset += page;
                        asgend_tui_render(tui);
                    } else if (tui->focused_pane == 1 && tui->tile_count > 0) {
                        size_t page = tui->content_rows > 4 ? (size_t)(tui->content_rows - 4) : 1;
                        if (second == '5') tui->scroll_offset = tui->scroll_offset > page ? tui->scroll_offset - page : 0;
                        else tui->scroll_offset += page;
                        int columns = tui->center_width > 2 ? tui->center_width - 2 : 8;
                        size_t total = center_total_visual_lines(tui, columns);
                        size_t visible = tui->content_rows > 2 ? (size_t)(tui->content_rows - 2) : 1;
                        size_t max_offset = total > visible ? total - visible : 0;
                        if (tui->scroll_offset > max_offset) tui->scroll_offset = max_offset;
                        tui->follow_stream_tail = tui->scroll_offset >= max_offset;
                        tui->selected_tile = tile_for_visual_line(tui, tui->scroll_offset, columns);
                        asgend_tui_render(tui);
                    }
                    return second == '5' ? ASGEND_TUI_KEY_PAGE_UP : ASGEND_TUI_KEY_PAGE_DOWN;
                }
            }
            if (second == '<') {
                char sequence[48];
                size_t used = 0;
                unsigned char ch = 0;
                while (used + 1 < sizeof(sequence) && tui_poll_byte(20, &ch)) {
                    if (ch == 'M' || ch == 'm') break;
                    sequence[used++] = (char)ch;
                }
                sequence[used] = '\0';
                int button = -1, x = 0, y = 0;
                if ((ch == 'M' || ch == 'm') &&
                    sscanf(sequence, "%d;%d;%d", &button, &x, &y) == 3 && button >= 0) {
                    /* SGR lowercase 'm' is a button release, not keyboard Escape.
                     * It completes a drag without re-selecting or closing previews. */
                    if (ch == 'm') return ASGEND_TUI_KEY_MOUSE;
                    if ((button & 64) != 0) {
                        int wheel_button = button & 3;
                        if (wheel_button <= 1) {
                            tui_mouse_scroll(tui, x, y, wheel_button == 0 ? -1 : 1);
                            return ASGEND_TUI_KEY_MOUSE;
                        }
                        tui_mouse_hscroll(tui, x, y, wheel_button == 2 ? -1 : 1);
                        return ASGEND_TUI_KEY_MOUSE;
                    } else if ((button & 32) != 0 && (button & 3) == 0) {
                        /* Button-motion reporting turns a held left click into
                         * drag-selection of visible task/tile/file rows. */
                        if (tui_mouse_select(tui, x, y)) return ASGEND_TUI_KEY_MOUSE;
                    } else if ((button & 3) == 0 && tui_mouse_select(tui, x, y)) {
                        return ASGEND_TUI_KEY_MOUSE;
                    }
                }
            }
        }
        return ASGEND_TUI_KEY_ESCAPE;
    }
    if (tui->session_picker_active && c != '\t' && c != '\r' && c != '\n' && c >= 0x20) {
        return (int)c;
    }
    if (tui->input_mode && c != '\t' && c != 127 && c != 8 && c != '\r' && c != '\n' && c >= 0x20) {
        return (int)c;
    }
    if (c == '\t') {
        int next = tui->focused_pane;
        for (int attempts = 0; attempts < 3; ++attempts) {
            next = (next + 1) % 3;
            if ((next == 0 && tui->left_width > 0) ||
                (next == 1 && tui->center_width > 0) ||
                (next == 2 && tui->right_width > 0)) break;
        }
        tui->focused_pane = next;
        if (tui->focused_pane == 2) tui->right_pane_goals = false;
        asgend_tui_render(tui);
        return '\t';
    }
    if (c == '\r' || c == '\n') {
        /* Enter action is dispatched by the CLI so file tiles can open previews. */
        return '\n';
    }
    if (c == 'p' || c == 'P') {
        if (tui->focused_pane == 2) {
            tui->right_pane_goals = !tui->right_pane_goals;
            if (tui->right_pane_goals) {
                tui->selected_plan_is_goal = true;
                if (tui->selected_plan >= tui->goal_count) tui->selected_plan = 0;
            }
            asgend_tui_render(tui);
        }
        return c;
    }
    if (c == 'b' || c == 127 || c == 8) return c;
    if (c == '1') {
        asgend_tui_set_display_mode(tui, ASGEND_TUI_DISPLAY_ALL_EXPANDED);
        return c;
    }
    if (c == '2') {
        asgend_tui_set_display_mode(tui, ASGEND_TUI_DISPLAY_ALL_COLLAPSED);
        return c;
    }
    if (c == '3') {
        asgend_tui_set_display_mode(tui, ASGEND_TUI_DISPLAY_DYNAMIC);
        return c;
    }
    if (c == 'j') {
        asgend_tui_move_selection(tui, 1);
        return c;
    }
    if (c == 'k') {
        asgend_tui_move_selection(tui, -1);
        return c;
    }
    return (int)c;
}

bool asgend_tui_selected_plan(AsgendTui *tui, AsgendTuiPlanItem *out) {
    if (!tui || !out || tui->focused_pane != 2 || tui->preview_content) return false;
    if (tui->selected_plan_is_goal) {
        if (tui->goal_count == 0 || tui->selected_plan >= tui->goal_count) return false;
        *out = tui->goals[tui->selected_plan];
    } else {
        if (tui->todo_count == 0 || tui->selected_plan >= tui->todo_count) return false;
        *out = tui->todos[tui->selected_plan];
    }
    return true;
}

bool asgend_tui_selected_task(AsgendTui *tui, AsgendTuiTaskItem *out) {
    if (!tui || !out || tui->focused_pane != 0 || tui->selected_left_index >= tui->task_count) return false;
    *out = tui->tasks[tui->selected_left_index];
    return true;
}
