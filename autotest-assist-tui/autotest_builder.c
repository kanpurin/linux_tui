#define _XOPEN_SOURCE_EXTENDED 1

#include <ctype.h>
#include <limits.h>
#include <locale.h>
#include <ncurses.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wchar.h>

#define MAX_CASES 64
#define MAX_CLEANUPS 32
#define TEXT_LEN 256
#define LONG_LEN 4096
#define PREVIEW_LINES 512
#define EDITOR_LINES 160
#define EDITOR_LINE_INITIAL_CAP 128
#define EDITOR_TAB_WIDTH 4
#define EDITOR_UNDO_DEPTH 32
#define COMPLETION_MAX 16
#define LEGACY_REGISTRY_CHECKS 4

typedef enum {
    SCREEN_DASHBOARD,
    SCREEN_EDITOR,
    SCREEN_FORM,
    SCREEN_MATCH,
    SCREEN_CLEANUP,
    SCREEN_REBOOT,
    SCREEN_SCRIPT_EDITOR,
    SCREEN_PREVIEW,
    SCREEN_RESULT,
    SCREEN_CONFIRM
} Screen;

typedef enum {
    MATCH_NONE,
    MATCH_EXACT,
    MATCH_NOT_EXACT,
    MATCH_CONTAINS,
    MATCH_NOT_CONTAINS,
    MATCH_EMPTY,
    MATCH_NOT_EMPTY
} MatchType;

typedef enum {
    CMD_SHELL,
    CMD_REBOOT,
    CMD_VIM
} CommandKind;

typedef enum {
    EDIT_TARGET_COMMAND,
    EDIT_TARGET_CHECK_EXPECTED,
    EDIT_TARGET_DESCRIPTION
} EditorTarget;

typedef enum {
    CONFIRM_GENERATE,
    CONFIRM_DELETE
} ConfirmAction;

typedef struct {
    char var[TEXT_LEN];
    MatchType match;
    char expected[LONG_LEN];
} CheckRule;

typedef struct {
    int line_no;
    char message[TEXT_LEN];
} SyntaxError;

typedef struct {
    char id[32];
    char title[TEXT_LEN];
    char description[LONG_LEN];
    char *command;
    char script_path[LONG_LEN];
    CommandKind kind;
    bool selected;
    int expected_exit;
    CheckRule checks[LEGACY_REGISTRY_CHECKS];
    int check_count;
    char cleanup[LONG_LEN];
    int timeout_sec;
    bool requires_root;
} TestCase;

typedef struct {
    char name[TEXT_LEN];
    TestCase cases[MAX_CASES];
    int case_count;
    char cleanups[MAX_CLEANUPS][LONG_LEN];
    int cleanup_count;
} Project;

typedef struct {
    Project project;
    Screen screen;
    Screen previous_screen;
    int selected_case;
    int selected_menu;
    int selected_field;
    int selected_cleanup;
    int selected_preview_file;
    char case_filter[TEXT_LEN];
    bool selected_only;
    bool run_after_generate;
    bool stop_on_failure;
    ConfirmAction confirm_action;
    char *editor_lines[EDITOR_LINES];
    size_t editor_line_caps[EDITOR_LINES];
    int editor_line_count;
    int editor_row;
    int editor_col;
    int editor_desired_col;
    int editor_first_row;
    int editor_first_wrap_row;
    bool editor_insert;
    bool editor_command_mode;
    bool editor_search_mode;
    bool editor_dirty;
    bool editor_new_case;
    bool editor_range_pending;
    char editor_range_op;
    int editor_range_anchor;
    bool completion_open;
    int completion_count;
    int completion_selected;
    int completion_start_col;
    int completion_end_col;
    char completion_items[COMPLETION_MAX][TEXT_LEN];
    bool regex_template_list_open;
    bool editor_help_open;
    int editor_help_scroll;
    EditorTarget editor_target;
    char editor_normal_command[16];
    int editor_normal_command_len;
    char editor_command[64];
    int editor_command_len;
    char editor_search[TEXT_LEN];
    int editor_search_len;
    char *editor_clipboard[EDITOR_LINES];
    int editor_clipboard_count;
    char *editor_undo_lines[EDITOR_UNDO_DEPTH][EDITOR_LINES];
    int editor_undo_line_count[EDITOR_UNDO_DEPTH];
    int editor_undo_row[EDITOR_UNDO_DEPTH];
    int editor_undo_col[EDITOR_UNDO_DEPTH];
    int editor_undo_count;
    char status[TEXT_LEN];
    char preview[PREVIEW_LINES][LONG_LEN];
    int preview_count;
    int preview_scroll;
} App;

typedef struct {
    const char *label;
    const char *text;
} RegexTemplate;

static const RegexTemplate REGEX_TEMPLATES[] = {
    {"digits", "[[:digit:]]+"},
    {"alpha", "[[:alpha:]]+"},
    {"alnum", "[[:alnum:]]+"},
    {"space", "[[:space:]]+"},
    {"word", "[[:alnum:]_]+"},
    {"any", ".*"},
    {"optional", "?"},
    {"repeat", "*"},
    {"one_plus", "+"},
    {"count", "{1,3}"},
    {"group", "()"},
    {"line_start", "^"},
    {"line_end", "$"}
};

#define REGEX_TEMPLATE_COUNT ((int)(sizeof(REGEX_TEMPLATES) / sizeof(REGEX_TEMPLATES[0])))

static int read_tui_input(void) {
    wint_t ch = 0;
    int rc = get_wch(&ch);
    if (rc == ERR) return ERR;
    return (int)ch;
}

static const char *EDITOR_HELP_LINES[] = {
    "Close: Esc / Enter / q    Scroll: Up/Down PgUp/PgDn",
    ":print                         Print current editor text to stdout.",
    "",
    "Script directives",
    "@check <var> <match> <expected>  Add a variable assertion.",
    "@check <var> <match> <<'EOF'    Use heredoc expected text.",
    "    exact      Regex must match the whole variable value.",
    "    not_exact  Regex must not match the whole variable value.",
    "    empty      Variable value must be empty.",
    "    contains   Regex must match somewhere in the value.",
    "    not_contains Regex must not match anywhere in the value.",
    "    not_empty  Variable value must not be empty.",
    "    none       No variable check.",
    "@assert <condition>             Fail immediately when condition is false.",
    "@backup <path>                  Save file or directory before mutation.",
    "@restore <path>                 Restore a previously backed-up path.",
    "@reboot-if <condition>          Reboot only when condition is true.",
    "",
    "TUI automation",
    "@tui <command>                  Start pseudo-terminal automation.",
    "@end                            End the @tui block.",
    "send <keys>                     Send literal keys without newline.",
    "    Quotes group text and are not sent; escape them as \\\" or \\'.",
    "send-shell <expr>               Send shell-expanded text.",
    "text <text>                     Send text plus newline.",
    "text-shell <expr>               Send shell-expanded text plus newline.",
    "enter / esc / tab / space       Send common keys.",
    "sleep <seconds>                 Wait.",
    "ctrl <key>                      Send Ctrl key, example: ctrl c.",
    "vim-clear                       Vim normal-mode clear buffer.",
    "vim-write-quit                  Vim :wq.",
    "",
    "TUI output variables",
    "AUTOTEST_TUI_STDOUT             Cleaned TUI output for assertions.",
    "AUTOTEST_TUI_TEXT               Same cleaned TUI output.",
    "AUTOTEST_TUI_TRANSCRIPT         Raw pseudo-terminal transcript.",
    "AUTOTEST_TUI_STDERR             Last script-wrapper stderr.",
    "AUTOTEST_TUI_STATUS             Last script-wrapper exit status.",
    "AUTOTEST_TUI_STDOUT_FILE        Clean output backing file.",
    "AUTOTEST_TUI_TEXT_FILE          Clean text backing file.",
    "AUTOTEST_TUI_TRANSCRIPT_FILE    Raw transcript backing file.",
    "AUTOTEST_TUI_INPUT_FILE         Sent printable input backing file.",
    "AUTOTEST_TUI_STDERR_FILE        Stderr backing file.",
    "",
    "Generated script options",
    "--detail [path]                Detail to stdout, or file when path given.",
    "--help                          Show generated script usage.",
    "unknown option                  Show usage and exit with error.",
    "",
    "Editor normal mode",
    "i insert, :w save, :wq save quit, :q quit, :q! quit without save.",
    "dd / dNd delete lines, yy / yNy copy lines, p / P paste.",
    "u undo, gg / G / nG jump, /word search, n / N next/previous.",
    "",
    "Editor insert mode",
    "Tab completes custom syntax when available; otherwise inserts a tab."
};

#define EDITOR_HELP_LINE_COUNT ((int)(sizeof(EDITOR_HELP_LINES) / sizeof(EDITOR_HELP_LINES[0])))

static void write_match_function(FILE *f);
static void write_case(FILE *f, const TestCase *tc, int index);
static void write_summary(FILE *f);
static void shell_quote(FILE *f, const char *s);
static void shell_quote_buf(char *dst, size_t dst_sz, const char *src);
static void write_comment_block(FILE *f, const char *label, const char *text);
static void auto_configure_test(TestCase *tc);
static void save_test_registry(const Project *p);
static const char *test_command(const TestCase *tc);
static bool set_test_command(TestCase *tc, const char *command);
static bool command_looks_like_reboot(const char *cmd);
static void trim_line_copy(char *dst, size_t dst_sz, const char *line, size_t len);
static void copy_text(char *dst, size_t dst_sz, const char *src);
static bool parse_check_arg(const char *arg, CheckRule *out, char *heredoc_delim, size_t heredoc_delim_sz);
static void collect_check_heredoc(const char *body_start, const char *delim, char *out, size_t out_sz, const char **after);

static const char *match_name(MatchType m) {
    switch (m) {
    case MATCH_NONE: return "none";
    case MATCH_EXACT: return "exact";
    case MATCH_NOT_EXACT: return "not_exact";
    case MATCH_CONTAINS: return "contains";
    case MATCH_NOT_CONTAINS: return "not_contains";
    case MATCH_EMPTY: return "empty";
    case MATCH_NOT_EMPTY: return "not_empty";
    }
    return "none";
}

static const char *command_kind_name(CommandKind k) {
    switch (k) {
    case CMD_SHELL: return "shell";
    case CMD_REBOOT: return "reboot";
    case CMD_VIM: return "vim";
    }
    return "shell";
}

static CheckRule *primary_check(TestCase *tc) {
    if (tc->check_count < 1) tc->check_count = 1;
    if (!tc->checks[0].var[0]) copy_text(tc->checks[0].var, sizeof(tc->checks[0].var), "AUTOTEST_ACTUAL");
    return &tc->checks[0];
}

static const CheckRule *primary_check_const(const TestCase *tc) {
    if (tc->check_count > 0 && tc->checks[0].var[0]) return &tc->checks[0];
    return NULL;
}

static bool include_case(const Project *p, int index, bool selected_only) {
    if (index < 0 || index >= p->case_count) return false;
    return !selected_only || p->cases[index].selected;
}

static int selected_count(const Project *p) {
    int count = 0;
    for (int i = 0; i < p->case_count; i++) {
        if (p->cases[i].selected) count++;
    }
    return count;
}

static bool has_reboot_for(const Project *p, bool selected_only) {
    for (int i = 0; i < p->case_count; i++) {
        if (!include_case(p, i, selected_only)) continue;
        if (p->cases[i].kind == CMD_REBOOT) {
            return true;
        }
    }
    return false;
}

static bool has_reboot(const Project *p) {
    return has_reboot_for(p, false);
}

static int ascii_lower_char(int ch) {
    return (ch >= 'A' && ch <= 'Z') ? ch + ('a' - 'A') : ch;
}

static bool text_contains_ascii_ci(const char *haystack, const char *needle) {
    if (!needle || !needle[0]) return true;
    if (!haystack) return false;
    size_t needle_len = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < needle_len && p[i] &&
               ascii_lower_char((unsigned char)p[i]) == ascii_lower_char((unsigned char)needle[i])) {
            i++;
        }
        if (i == needle_len) return true;
    }
    return false;
}

static bool case_matches_filter(const TestCase *tc, const char *filter) {
    if (!filter || !filter[0]) return true;
    return text_contains_ascii_ci(tc->id, filter) ||
           text_contains_ascii_ci(tc->title, filter) ||
           text_contains_ascii_ci(tc->description, filter) ||
           text_contains_ascii_ci(tc->script_path, filter) ||
           text_contains_ascii_ci(test_command(tc), filter);
}

static int filtered_case_count(const App *app) {
    int count = 0;
    for (int i = 0; i < app->project.case_count; i++) {
        if (case_matches_filter(&app->project.cases[i], app->case_filter)) count++;
    }
    return count;
}

static int first_filtered_case(const App *app) {
    for (int i = 0; i < app->project.case_count; i++) {
        if (case_matches_filter(&app->project.cases[i], app->case_filter)) return i;
    }
    return -1;
}

static void select_first_filtered_case(App *app) {
    int first = first_filtered_case(app);
    if (first >= 0) app->selected_case = first;
}

static void move_filtered_case(App *app, int direction) {
    int count = filtered_case_count(app);
    if (count <= 0) return;
    int current = app->selected_case;
    if (current < 0 || current >= app->project.case_count ||
        !case_matches_filter(&app->project.cases[current], app->case_filter)) {
        select_first_filtered_case(app);
        return;
    }
    for (int step = 1; step <= app->project.case_count; step++) {
        int next = (current + direction * step) % app->project.case_count;
        if (next < 0) next += app->project.case_count;
        if (case_matches_filter(&app->project.cases[next], app->case_filter)) {
            app->selected_case = next;
            return;
        }
    }
}

static void clamp_selected(App *app) {
    if (app->project.case_count <= 0) {
        app->selected_case = 0;
    } else if (app->selected_case >= app->project.case_count) {
        app->selected_case = app->project.case_count - 1;
    } else if (app->selected_case < 0) {
        app->selected_case = 0;
    }
    if (app->project.case_count > 0 && app->case_filter[0] &&
        !case_matches_filter(&app->project.cases[app->selected_case], app->case_filter)) {
        select_first_filtered_case(app);
    }
    if (app->selected_cleanup >= app->project.cleanup_count) {
        app->selected_cleanup = app->project.cleanup_count - 1;
    }
    if (app->selected_cleanup < 0) {
        app->selected_cleanup = 0;
    }
}

static void set_status(App *app, const char *msg) {
    snprintf(app->status, sizeof(app->status), "%s", msg);
}

static void copy_text(char *dst, size_t dst_sz, const char *src) {
    if (dst_sz == 0) return;
    const char *safe_src = src ? src : "";
    size_t len = strlen(safe_src);
    if (len >= dst_sz) len = dst_sz - 1;
    memcpy(dst, safe_src, len);
    dst[len] = '\0';
}

static void summarize_multiline(char *dst, size_t dst_sz, const char *src) {
    size_t j = 0;
    if (dst_sz == 0) return;
    for (size_t i = 0; src && src[i] && j + 2 < dst_sz; i++) {
        if (src[i] == '\n') {
            dst[j++] = '\\';
            dst[j++] = 'n';
        } else if (src[i] == '\t') {
            dst[j++] = '\\';
            dst[j++] = 't';
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

static bool editor_is_text_char(wint_t ch) {
    if (ch >= KEY_MIN && ch <= KEY_MAX) return false;
    return ch >= 0x80 || (ch < 0x80 && isprint((unsigned char)ch));
}

static int utf8_char_len_at(const char *s, int index) {
    unsigned char c = (unsigned char)s[index];
    int len = (int)strlen(s);
    if (c < 0x80) return 1;
    if ((c & 0xe0) == 0xc0 && index + 1 < len) return 2;
    if ((c & 0xf0) == 0xe0 && index + 2 < len) return 3;
    if ((c & 0xf8) == 0xf0 && index + 3 < len) return 4;
    return 1;
}

static int utf8_char_width_at(const char *s, int index) {
    unsigned char c = (unsigned char)s[index];
    if (c == '\t') return EDITOR_TAB_WIDTH;
    return c < 0x80 ? 1 : 2;
}

static int utf8_prev_index(const char *line, int col) {
    if (col <= 0) return 0;
    col--;
    while (col > 0 && (((unsigned char)line[col] & 0xc0) == 0x80)) col--;
    return col;
}

static int utf8_next_index(const char *line, int col) {
    int len = (int)strlen(line);
    if (col >= len) return len;
    col += utf8_char_len_at(line, col);
    return col > len ? len : col;
}

static int editor_visual_col(const char *line, int col) {
    int visual = 0;
    for (int i = 0; line && line[i] && i < col;) {
        visual += utf8_char_width_at(line, i);
        i += utf8_char_len_at(line, i);
    }
    return visual;
}

static int editor_index_for_visual_col(const char *line, int target_col) {
    int visual = 0;
    int i = 0;
    if (target_col <= 0) return 0;
    while (line && line[i]) {
        int char_w = utf8_char_width_at(line, i);
        if (visual + char_w > target_col) break;
        visual += char_w;
        i += utf8_char_len_at(line, i);
    }
    return i;
}

static void editor_update_desired_col(App *app) {
    app->editor_desired_col = editor_visual_col(app->editor_lines[app->editor_row], app->editor_col);
}

static void editor_move_vertical(App *app, int delta) {
    int row = app->editor_row + delta;
    if (row < 0 || row >= app->editor_line_count) return;
    app->editor_row = row;
    app->editor_col = editor_index_for_visual_col(app->editor_lines[app->editor_row], app->editor_desired_col);
}

static void print_editor_line(int y, int x, int w, const char *line) {
    int cx = x;
    for (int i = 0; line && line[i] && cx < x + w;) {
        if (line[i] == '\t') {
            for (int n = 0; n < EDITOR_TAB_WIDTH && cx < x + w; n++) {
                mvaddch(y, cx++, ' ');
            }
            i++;
        } else {
            int len = utf8_char_len_at(line, i);
            int char_w = utf8_char_width_at(line, i);
            if (cx + char_w > x + w) break;
            mvaddnstr(y, cx, line + i, len);
            cx += char_w;
            i += len;
        }
    }
    while (cx < x + w) {
        mvaddch(y, cx++, ' ');
    }
}

static void print_editor_line_segment(int y, int x, int w, const char *line, int start_visual) {
    int cx = x;
    int visual = 0;
    if (start_visual < 0) start_visual = 0;
    for (int i = 0; line && line[i] && cx < x + w;) {
        int len = utf8_char_len_at(line, i);
        int char_w = utf8_char_width_at(line, i);
        int next_visual = visual + char_w;
        if (next_visual <= start_visual) {
            visual = next_visual;
            i += len;
            continue;
        }
        if (line[i] == '\t') {
            int first_space = start_visual > visual ? start_visual - visual : 0;
            for (int n = first_space; n < EDITOR_TAB_WIDTH && cx < x + w; n++) {
                mvaddch(y, cx++, ' ');
            }
        } else {
            if (cx + char_w > x + w) break;
            mvaddnstr(y, cx, line + i, len);
            cx += char_w;
        }
        visual = next_visual;
        i += len;
    }
    while (cx < x + w) {
        mvaddch(y, cx++, ' ');
    }
}

static int editor_wrapped_rows_for_line(const App *app, int line_index, int wrap_w) {
    if (wrap_w < 1) wrap_w = 1;
    const char *line = app->editor_lines[line_index];
    int visual = editor_visual_col(line, (int)strlen(line));
    int rows = visual > 0 ? (visual + wrap_w - 1) / wrap_w : 1;
    if (line_index == app->editor_row) {
        int cursor_visual = editor_visual_col(line, app->editor_col);
        int cursor_row = cursor_visual / wrap_w;
        if (rows <= cursor_row) rows = cursor_row + 1;
    }
    return rows < 1 ? 1 : rows;
}

static void draw_editor_cursor(const App *app, int y, int x, int w, const char *line, int start_visual) {
    int len = (int)strlen(line);
    int rel = editor_visual_col(line, app->editor_col) - start_visual;
    if (rel < 0 || rel >= w) return;
    int cx = x + rel;
    if (cx >= x + w) return;
    attron(A_UNDERLINE);
    if (app->editor_col < len) {
        if (line[app->editor_col] == '\t') {
            for (int n = 0; n < EDITOR_TAB_WIDTH && cx + n < x + w; n++) {
                mvaddch(y, cx + n, ' ');
            }
        } else {
            int char_len = utf8_char_len_at(line, app->editor_col);
            mvaddnstr(y, cx, line + app->editor_col, char_len);
        }
    } else {
        mvaddch(y, cx, '_');
    }
    attroff(A_UNDERLINE);
}

static void unescape_field(char *dst, size_t dst_sz, const char *src) {
    size_t j = 0;
    if (dst_sz == 0) return;
    for (size_t i = 0; src && src[i] && j + 1 < dst_sz; i++) {
        if (src[i] == '\\' && src[i + 1]) {
            i++;
            if (src[i] == 'n') dst[j++] = '\n';
            else if (src[i] == 't') dst[j++] = '\t';
            else dst[j++] = src[i];
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

static char *unescape_field_alloc(const char *src) {
    size_t cap = 128;
    size_t j = 0;
    char *dst = malloc(cap);
    if (!dst) return NULL;
    for (size_t i = 0; src && src[i]; i++) {
        char ch = src[i];
        if (ch == '\\' && src[i + 1]) {
            i++;
            if (src[i] == 'n') ch = '\n';
            else if (src[i] == 't') ch = '\t';
            else ch = src[i];
        }
        if (j + 1 >= cap) {
            size_t next_cap = cap * 2;
            char *grown = realloc(dst, next_cap);
            if (!grown) {
                free(dst);
                return NULL;
            }
            dst = grown;
            cap = next_cap;
        }
        dst[j++] = ch;
    }
    dst[j] = '\0';
    return dst;
}

static void write_escaped_field(FILE *f, const char *src) {
    for (size_t i = 0; src && src[i]; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\n') fputs("\\n", f);
        else if (c == '\t') fputs("\\t", f);
        else if (c == '\\') fputs("\\\\", f);
        else fputc((char)c, f);
    }
}

static char *read_line_alloc(FILE *f) {
    size_t cap = 1024;
    size_t len = 0;
    char *line = malloc(cap);
    if (!line) return NULL;
    int ch;
    while ((ch = fgetc(f)) != EOF) {
        if (len + 1 >= cap) {
            size_t next_cap = cap * 2;
            char *grown = realloc(line, next_cap);
            if (!grown) {
                free(line);
                return NULL;
            }
            line = grown;
            cap = next_cap;
        }
        line[len++] = (char)ch;
        if (ch == '\n') break;
    }
    if (len == 0 && ch == EOF) {
        free(line);
        return NULL;
    }
    line[len] = '\0';
    return line;
}

static MatchType parse_match(const char *s) {
    if (strcmp(s, "regex") == 0) return MATCH_EXACT;
    for (int i = MATCH_NONE; i <= MATCH_NOT_EMPTY; i++) {
        if (strcmp(s, match_name((MatchType)i)) == 0) return (MatchType)i;
    }
    return MATCH_NONE;
}

static bool is_match_name(const char *s) {
    if (strcmp(s, "regex") == 0) return true;
    for (int i = MATCH_NONE; i <= MATCH_NOT_EMPTY; i++) {
        if (strcmp(s, match_name((MatchType)i)) == 0) return true;
    }
    return false;
}

static CommandKind parse_kind(const char *s) {
    if (strcmp(s, "reboot") == 0) return CMD_REBOOT;
    if (strcmp(s, "vim") == 0) return CMD_VIM;
    return CMD_SHELL;
}

static int registry_path(char *out, size_t out_sz) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) return -1;
    if (snprintf(out, out_sz, "%s/.config/autotest-assist/tests.tsv", home) >= (int)out_sz) return -1;
    return 0;
}

static int ensure_registry_dir(void) {
    const char *home = getenv("HOME");
    char path[LONG_LEN];
    if (!home || !home[0]) return -1;
    if (snprintf(path, sizeof(path), "%s/.config", home) >= (int)sizeof(path)) return -1;
    mkdir(path, 0755);
    if (snprintf(path, sizeof(path), "%s/.config/autotest-assist", home) >= (int)sizeof(path)) return -1;
    mkdir(path, 0755);
    return 0;
}

static bool file_contains_reboot(const char *path) {
    FILE *f = fopen(path, "r");
    char line[LONG_LEN];
    bool found = false;
    if (!f) return false;
    while (fgets(line, sizeof(line), f)) {
        if (command_looks_like_reboot(line)) {
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

static const char *editor_target_name(EditorTarget target) {
    switch (target) {
    case EDIT_TARGET_COMMAND: return "script body";
    case EDIT_TARGET_CHECK_EXPECTED: return "variable expected";
    case EDIT_TARGET_DESCRIPTION: return "description";
    }
    return "text";
}

static char *dup_text_heap(const char *src) {
    if (!src) src = "";
    size_t len = strlen(src);
    char *out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, src, len + 1);
    return out;
}

static const char *test_command(const TestCase *tc) {
    return (tc && tc->command) ? tc->command : "";
}

static bool set_test_command(TestCase *tc, const char *command) {
    char *copy = dup_text_heap(command);
    if (!copy) return false;
    free(tc->command);
    tc->command = copy;
    return true;
}

static bool take_test_command(TestCase *tc, char *command) {
    if (!command) return set_test_command(tc, "");
    free(tc->command);
    tc->command = command;
    return true;
}

static void free_test_case(TestCase *tc) {
    if (!tc) return;
    free(tc->command);
    tc->command = NULL;
}

static bool editor_ensure_line_capacity(App *app, int row, size_t content_len) {
    if (row < 0 || row >= EDITOR_LINES) return false;
    size_t needed = content_len + 1;
    if (needed < EDITOR_LINE_INITIAL_CAP) needed = EDITOR_LINE_INITIAL_CAP;
    if (app->editor_lines[row] && app->editor_line_caps[row] >= needed) return true;

    size_t cap = app->editor_line_caps[row] ? app->editor_line_caps[row] : EDITOR_LINE_INITIAL_CAP;
    while (cap < needed) {
        if (cap > ((size_t)-1) / 2) {
            cap = needed;
            break;
        }
        cap *= 2;
    }
    char *grown = realloc(app->editor_lines[row], cap);
    if (!grown) {
        set_status(app, "Out of memory.");
        return false;
    }
    if (!app->editor_lines[row]) grown[0] = '\0';
    app->editor_lines[row] = grown;
    app->editor_line_caps[row] = cap;
    return true;
}

static bool editor_set_line(App *app, int row, const char *text) {
    if (!text) text = "";
    size_t len = strlen(text);
    if (!editor_ensure_line_capacity(app, row, len)) return false;
    memcpy(app->editor_lines[row], text, len + 1);
    return true;
}

static void editor_clear_line(App *app, int row) {
    if (editor_ensure_line_capacity(app, row, 0)) app->editor_lines[row][0] = '\0';
}

static void editor_free_line(App *app, int row) {
    if (row < 0 || row >= EDITOR_LINES) return;
    free(app->editor_lines[row]);
    app->editor_lines[row] = NULL;
    app->editor_line_caps[row] = 0;
}

static void editor_free_clipboard(App *app) {
    for (int i = 0; i < EDITOR_LINES; i++) {
        free(app->editor_clipboard[i]);
        app->editor_clipboard[i] = NULL;
    }
    app->editor_clipboard_count = 0;
}

static bool editor_set_clipboard_line(App *app, int index, const char *text) {
    if (index < 0 || index >= EDITOR_LINES) return false;
    char *copy = dup_text_heap(text);
    if (!copy) {
        set_status(app, "Out of memory.");
        return false;
    }
    free(app->editor_clipboard[index]);
    app->editor_clipboard[index] = copy;
    return true;
}

static void editor_free_undo_slot(App *app, int slot) {
    if (slot < 0 || slot >= EDITOR_UNDO_DEPTH) return;
    for (int r = 0; r < EDITOR_LINES; r++) {
        free(app->editor_undo_lines[slot][r]);
        app->editor_undo_lines[slot][r] = NULL;
    }
}

static void editor_clear_undo(App *app) {
    for (int i = 0; i < EDITOR_UNDO_DEPTH; i++) editor_free_undo_slot(app, i);
    app->editor_undo_count = 0;
}

static void editor_free_all_lines(App *app) {
    for (int r = 0; r < EDITOR_LINES; r++) editor_free_line(app, r);
}

static bool editor_insert_empty_line(App *app, int at) {
    if (app->editor_line_count >= EDITOR_LINES) {
        set_status(app, "Editor line limit reached.");
        return false;
    }
    if (at < 0) at = 0;
    if (at > app->editor_line_count) at = app->editor_line_count;
    for (int r = app->editor_line_count; r > at; r--) {
        app->editor_lines[r] = app->editor_lines[r - 1];
        app->editor_line_caps[r] = app->editor_line_caps[r - 1];
    }
    app->editor_lines[at] = NULL;
    app->editor_line_caps[at] = 0;
    app->editor_line_count++;
    return editor_set_line(app, at, "");
}

static void load_editor_text(App *app, const char *src) {
    editor_clear_undo(app);
    editor_free_clipboard(app);
    editor_free_all_lines(app);

    app->editor_line_count = 1;
    app->editor_row = 0;
    app->editor_col = 0;
    app->editor_desired_col = 0;
    app->editor_first_row = 0;
    app->editor_first_wrap_row = 0;
    app->editor_insert = false;
    app->editor_command_mode = false;
    app->editor_search_mode = false;
    app->editor_dirty = false;
    app->editor_range_pending = false;
    app->editor_range_op = '\0';
    app->editor_range_anchor = 0;
    app->completion_open = false;
    app->completion_count = 0;
    app->completion_selected = 0;
    app->completion_start_col = 0;
    app->completion_end_col = 0;
    app->editor_command_len = 0;
    app->editor_command[0] = '\0';
    app->editor_normal_command_len = 0;
    app->editor_normal_command[0] = '\0';
    app->editor_search_len = 0;
    app->editor_search[0] = '\0';

    int row = 0;
    const char *start = src ? src : "";
    for (const char *p = start; row < EDITOR_LINES; p++) {
        if (*p == '\n' || *p == '\0') {
            size_t len = (size_t)(p - start);
            if (!editor_ensure_line_capacity(app, row, len)) {
                app->editor_line_count = row + 1;
                break;
            }
            memcpy(app->editor_lines[row], start, len);
            app->editor_lines[row][len] = '\0';
            app->editor_line_count = row + 1;
            if (*p == '\0') break;
            row++;
            start = p + 1;
        }
    }
    if (app->editor_line_count < 1) {
        app->editor_line_count = 1;
        editor_clear_line(app, 0);
    }
}

static void save_editor_text(App *app, char *dst, size_t dst_sz) {
    size_t pos = 0;
    if (dst_sz == 0) return;
    dst[0] = '\0';
    for (int r = 0; r < app->editor_line_count; r++) {
        const char *line = app->editor_lines[r] ? app->editor_lines[r] : "";
        size_t len = strlen(line);
        if (pos + len + 2 >= dst_sz) break;
        memcpy(dst + pos, line, len);
        pos += len;
        if (r + 1 < app->editor_line_count) dst[pos++] = '\n';
    }
    dst[pos] = '\0';
}

static char *editor_text_alloc(const App *app) {
    size_t total = 1;
    for (int r = 0; r < app->editor_line_count; r++) {
        total += strlen(app->editor_lines[r] ? app->editor_lines[r] : "");
        if (r + 1 < app->editor_line_count) total++;
    }
    char *out = malloc(total);
    if (!out) return NULL;
    size_t pos = 0;
    for (int r = 0; r < app->editor_line_count; r++) {
        const char *line = app->editor_lines[r] ? app->editor_lines[r] : "";
        size_t len = strlen(line);
        memcpy(out + pos, line, len);
        pos += len;
        if (r + 1 < app->editor_line_count) out[pos++] = '\n';
    }
    out[pos] = '\0';
    return out;
}

static int editor_visible_rows(const App *app, int height) {
    int rows = height - 9;
    if (app->editor_command_mode || app->editor_search_mode) rows = height - 10;
    if (rows < 1) rows = 1;
    return rows;
}

static int editor_total_screen_rows(const App *app, int wrap_w) {
    int total = 0;
    if (wrap_w < 1) wrap_w = 1;
    for (int r = 0; r < app->editor_line_count; r++) {
        total += editor_wrapped_rows_for_line(app, r, wrap_w);
    }
    return total < 1 ? 1 : total;
}

static int editor_line_screen_start(const App *app, int line_index, int wrap_w) {
    int row = 0;
    if (wrap_w < 1) wrap_w = 1;
    if (line_index < 0) return 0;
    if (line_index > app->editor_line_count) line_index = app->editor_line_count;
    for (int r = 0; r < line_index; r++) {
        row += editor_wrapped_rows_for_line(app, r, wrap_w);
    }
    return row;
}

static int editor_scroll_screen_row(const App *app, int wrap_w) {
    int row = editor_line_screen_start(app, app->editor_first_row, wrap_w);
    int wrapped = editor_wrapped_rows_for_line(app, app->editor_first_row, wrap_w);
    int first_wrap = app->editor_first_wrap_row;
    if (first_wrap < 0) first_wrap = 0;
    if (first_wrap >= wrapped) first_wrap = wrapped - 1;
    return row + first_wrap;
}

static int editor_cursor_screen_row_abs(const App *app, int wrap_w) {
    if (wrap_w < 1) wrap_w = 1;
    return editor_line_screen_start(app, app->editor_row, wrap_w) +
           editor_visual_col(app->editor_lines[app->editor_row], app->editor_col) / wrap_w;
}

static void editor_set_scroll_screen_row(App *app, int scroll, int rows, int wrap_w) {
    if (wrap_w < 1) wrap_w = 1;
    int total = editor_total_screen_rows(app, wrap_w);
    int max_scroll = total > rows ? total - rows : 0;
    if (scroll < 0) scroll = 0;
    if (scroll > max_scroll) scroll = max_scroll;

    int acc = 0;
    for (int r = 0; r < app->editor_line_count; r++) {
        int wrapped = editor_wrapped_rows_for_line(app, r, wrap_w);
        if (scroll < acc + wrapped) {
            app->editor_first_row = r;
            app->editor_first_wrap_row = scroll - acc;
            return;
        }
        acc += wrapped;
    }
    app->editor_first_row = app->editor_line_count > 0 ? app->editor_line_count - 1 : 0;
    app->editor_first_wrap_row = 0;
}

static void editor_clamp_scroll(App *app, int rows) {
    int wrap_w = COLS - 9;
    if (wrap_w < 1) wrap_w = 1;
    editor_set_scroll_screen_row(app, editor_scroll_screen_row(app, wrap_w), rows, wrap_w);
}

static int editor_cursor_screen_row_from_first(const App *app, int first_row, int wrap_w) {
    int scroll = editor_line_screen_start(app, first_row, wrap_w);
    if (first_row == app->editor_first_row) {
        int wrapped = editor_wrapped_rows_for_line(app, first_row, wrap_w);
        int first_wrap = app->editor_first_wrap_row;
        if (first_wrap < 0) first_wrap = 0;
        if (first_wrap >= wrapped) first_wrap = wrapped - 1;
        scroll += first_wrap;
    }
    return editor_cursor_screen_row_abs(app, wrap_w) - scroll;
}

static void editor_ensure_cursor_visible(App *app, int rows) {
    int wrap_w = COLS - 9;
    if (wrap_w < 1) wrap_w = 1;
    editor_clamp_scroll(app, rows);
    int scroll = editor_scroll_screen_row(app, wrap_w);
    int cursor = editor_cursor_screen_row_abs(app, wrap_w);
    if (cursor < scroll) {
        scroll = cursor;
    } else if (cursor >= scroll + rows) {
        scroll = cursor - rows + 1;
    }
    editor_set_scroll_screen_row(app, scroll, rows, wrap_w);
}

static void print_raw_script_text(const char *text) {
    def_prog_mode();
    endwin();
    fputs(text ? text : "", stdout);
    if (!text || text[0] == '\0' || text[strlen(text) - 1] != '\n') fputc('\n', stdout);
    fflush(stdout);
    fprintf(stderr, "\nPress Enter to return to TUI.");
    fflush(stderr);
    (void)getchar();
    reset_prog_mode();
    refresh();
}

static void print_current_test_script(App *app) {
    if (app->project.case_count <= 0) {
        set_status(app, "No test script to print.");
        return;
    }
    print_raw_script_text(test_command(&app->project.cases[app->selected_case]));
    set_status(app, "Printed script to stdout.");
}

static void print_editor_script(App *app) {
    char *buf = editor_text_alloc(app);
    if (!buf) {
        set_status(app, "Out of memory.");
        return;
    }
    print_raw_script_text(buf);
    free(buf);
    set_status(app, "Printed editor script to stdout.");
}

static void load_editor_from_case(App *app) {
    if (app->project.case_count == 0) return;
    const TestCase *tc = &app->project.cases[app->selected_case];
    const CheckRule *check = primary_check_const(tc);
    const char *src = test_command(tc);
    if (app->editor_target == EDIT_TARGET_CHECK_EXPECTED && check) src = check->expected;
    if (app->editor_target == EDIT_TARGET_DESCRIPTION) src = tc->description;
    load_editor_text(app, src);
}

static void save_editor_to_case(App *app) {
    if (app->project.case_count == 0) return;
    TestCase *tc = &app->project.cases[app->selected_case];
    if (app->editor_target == EDIT_TARGET_CHECK_EXPECTED) {
        CheckRule *check = primary_check(tc);
        save_editor_text(app, check->expected, sizeof(check->expected));
    } else if (app->editor_target == EDIT_TARGET_DESCRIPTION) {
        save_editor_text(app, tc->description, sizeof(tc->description));
    } else {
        char *command = editor_text_alloc(app);
        if (!command) {
            set_status(app, "Out of memory.");
            return;
        }
        take_test_command(tc, command);
        tc->kind = CMD_SHELL;
        auto_configure_test(tc);
    }
}

static bool editor_uses_regex_templates(const App *app) {
    if (app->project.case_count == 0) return false;
    if (app->editor_target == EDIT_TARGET_COMMAND) return true;
    if (app->editor_target == EDIT_TARGET_DESCRIPTION) return false;
    const TestCase *tc = &app->project.cases[app->selected_case];
    const CheckRule *check = primary_check_const(tc);
    return app->editor_target == EDIT_TARGET_CHECK_EXPECTED && check &&
           (check->match == MATCH_EXACT || check->match == MATCH_NOT_EXACT ||
            check->match == MATCH_CONTAINS || check->match == MATCH_NOT_CONTAINS);
}

static bool text_contains(const char *haystack, const char *needle) {
    return haystack && needle && strstr(haystack, needle) != NULL;
}

static bool command_looks_like_reboot(const char *command) {
    while (command && isspace((unsigned char)*command)) command++;
    if (strncmp(command, "@reboot-if", 10) == 0 &&
        (command[10] == '\0' || isspace((unsigned char)command[10]))) {
        return true;
    }
    return text_contains(command, "@reboot-if") ||
           text_contains(command, "\nreboot") ||
           text_contains(command, ";reboot") ||
           text_contains(command, " reboot") ||
           strncmp(command, "reboot ", 7) == 0 ||
           text_contains(command, "reboot\n") ||
           text_contains(command, "systemctl reboot") ||
           text_contains(command, "shutdown -r") ||
           text_contains(command, "init 6") ||
           strcmp(command, "reboot") == 0;
}

static bool line_is_reboot_command(const char *line, size_t len) {
    char buf[TEXT_LEN];
    size_t start = 0;
    while (start < len && isspace((unsigned char)line[start])) start++;
    while (len > start && isspace((unsigned char)line[len - 1])) len--;
    if (len - start >= sizeof(buf)) return false;
    memcpy(buf, line + start, len - start);
    buf[len - start] = '\0';
    return strcmp(buf, "reboot") == 0 ||
           strncmp(buf, "reboot ", 7) == 0 ||
           strcmp(buf, "systemctl reboot") == 0 ||
           strncmp(buf, "systemctl reboot ", 17) == 0 ||
           strncmp(buf, "shutdown -r", 11) == 0 ||
           strcmp(buf, "init 6") == 0;
}

static bool line_starts_reboot_if(const char *line, size_t len, char *condition, size_t condition_sz) {
    char trimmed[LONG_LEN];
    trim_line_copy(trimmed, sizeof(trimmed), line, len);
    if (strncmp(trimmed, "@reboot-if", 10) != 0 ||
        (trimmed[10] && !isspace((unsigned char)trimmed[10]))) {
        return false;
    }
    const char *p = trimmed + 10;
    while (isspace((unsigned char)*p)) p++;
    copy_text(condition, condition_sz, p);
    return condition[0] != '\0';
}

static bool line_starts_directive(const char *line, size_t len, const char *name, char *arg, size_t arg_sz) {
    char trimmed[LONG_LEN];
    size_t name_len = strlen(name);
    trim_line_copy(trimmed, sizeof(trimmed), line, len);
    if (strncmp(trimmed, name, name_len) != 0 ||
        (trimmed[name_len] && !isspace((unsigned char)trimmed[name_len]))) {
        return false;
    }
    const char *p = trimmed + name_len;
    while (isspace((unsigned char)*p)) p++;
    copy_text(arg, arg_sz, p);
    return arg[0] != '\0';
}

static bool line_is_reboot_boundary(const char *line, size_t len) {
    char condition[LONG_LEN];
    return line_is_reboot_command(line, len) ||
           line_starts_reboot_if(line, len, condition, sizeof(condition));
}

static void trim_line_copy(char *dst, size_t dst_sz, const char *line, size_t len) {
    size_t start = 0;
    if (dst_sz == 0) return;
    while (start < len && isspace((unsigned char)line[start])) start++;
    while (len > start && isspace((unsigned char)line[len - 1])) len--;
    if (len - start >= dst_sz) len = start + dst_sz - 1;
    memcpy(dst, line + start, len - start);
    dst[len - start] = '\0';
}

static void shell_quote_len(FILE *f, const char *s, size_t len) {
    fputc('\'', f);
    for (size_t i = 0; s && i < len; i++) {
        if (s[i] == '\'') {
            fputs("'\\''", f);
        } else {
            fputc(s[i], f);
        }
    }
    fputc('\'', f);
}

static bool decode_tui_argument_checked(const char *src, char *dst, size_t dst_sz) {
    size_t j = 0;
    char quote = '\0';
    if (dst_sz == 0) return false;
    for (size_t i = 0; src && src[i] && j + 1 < dst_sz; i++) {
        char c = src[i];
        if (c == '\\' && src[i + 1]) {
            dst[j++] = src[++i];
        } else if ((c == '"' || c == '\'') && (quote == '\0' || quote == c)) {
            quote = quote == c ? '\0' : c;
        } else {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
    return quote == '\0';
}

static void decode_tui_argument(const char *src, char *dst, size_t dst_sz) {
    (void)decode_tui_argument_checked(src, dst, dst_sz);
}

static bool line_starts_tui(const char *line, size_t len, char *cmd, size_t cmd_sz) {
    char trimmed[LONG_LEN];
    trim_line_copy(trimmed, sizeof(trimmed), line, len);
    if (strncmp(trimmed, "@tui", 4) != 0 || (trimmed[4] && !isspace((unsigned char)trimmed[4]))) return false;
    const char *p = trimmed + 4;
    while (isspace((unsigned char)*p)) p++;
    copy_text(cmd, cmd_sz, p);
    return cmd[0] != '\0';
}

static bool line_is_tui_end(const char *line, size_t len) {
    char trimmed[TEXT_LEN];
    trim_line_copy(trimmed, sizeof(trimmed), line, len);
    return strcmp(trimmed, "@end") == 0;
}

static void write_tui_instruction(FILE *f, const char *line, size_t len) {
    char trimmed[LONG_LEN];
    char decoded[LONG_LEN];
    trim_line_copy(trimmed, sizeof(trimmed), line, len);
    if (strncmp(trimmed, "send ", 5) == 0) {
        decode_tui_argument(trimmed + 5, decoded, sizeof(decoded));
        fputs("  printf ", f);
        shell_quote_len(f, decoded, strlen(decoded));
        fputs(" >>\"$AUTOTEST_TUI_INPUT_FILE\"\n", f);
        fputs("  printf ", f);
        shell_quote_len(f, decoded, strlen(decoded));
        fputc('\n', f);
    } else if (strncmp(trimmed, "send-shell ", 11) == 0) {
        fputs("  printf '%s' ", f);
        fputs(trimmed + 11, f);
        fputs(" >>\"$AUTOTEST_TUI_INPUT_FILE\"\n", f);
        fputs("  printf '%s' ", f);
        fputs(trimmed + 11, f);
        fputc('\n', f);
    } else if (strncmp(trimmed, "text ", 5) == 0) {
        decode_tui_argument(trimmed + 5, decoded, sizeof(decoded));
        fputs("  printf ", f);
        shell_quote_len(f, decoded, strlen(decoded));
        fputs(" >>\"$AUTOTEST_TUI_INPUT_FILE\"\n  printf '\\n' >>\"$AUTOTEST_TUI_INPUT_FILE\"\n", f);
        fputs("  printf ", f);
        shell_quote_len(f, decoded, strlen(decoded));
        fputs("\n  printf '\\n'\n", f);
    } else if (strncmp(trimmed, "text-shell ", 11) == 0) {
        fputs("  printf '%s\\n' ", f);
        fputs(trimmed + 11, f);
        fputs(" >>\"$AUTOTEST_TUI_INPUT_FILE\"\n", f);
        fputs("  printf '%s\\n' ", f);
        fputs(trimmed + 11, f);
        fputc('\n', f);
    } else if (strcmp(trimmed, "enter") == 0) {
        fputs("  printf '\\n' >>\"$AUTOTEST_TUI_INPUT_FILE\"\n", f);
        fputs("  printf '\\r'\n", f);
    } else if (strcmp(trimmed, "esc") == 0) {
        fputs("  printf '\\033' >>\"$AUTOTEST_TUI_INPUT_FILE\"\n", f);
        fputs("  printf '\\033'\n", f);
    } else if (strcmp(trimmed, "tab") == 0) {
        fputs("  printf '\\t' >>\"$AUTOTEST_TUI_INPUT_FILE\"\n", f);
        fputs("  printf '\\t'\n", f);
    } else if (strcmp(trimmed, "space") == 0) {
        fputs("  printf ' ' >>\"$AUTOTEST_TUI_INPUT_FILE\"\n", f);
        fputs("  printf ' '\n", f);
    } else if (strcmp(trimmed, "vim-clear") == 0) {
        fputs("  printf 'ggdG' >>\"$AUTOTEST_TUI_INPUT_FILE\"\n", f);
        fputs("  printf 'ggdG'\n", f);
    } else if (strcmp(trimmed, "vim-write-quit") == 0) {
        fputs("  printf '\\033:wq\\n' >>\"$AUTOTEST_TUI_INPUT_FILE\"\n", f);
        fputs("  printf '\\033:wq\\r'\n", f);
    } else if (strncmp(trimmed, "sleep ", 6) == 0) {
        fputs("  sleep ", f);
        for (const char *p = trimmed + 6; *p; p++) {
            if (isdigit((unsigned char)*p) || *p == '.') fputc(*p, f);
        }
        fputc('\n', f);
    } else if (strncmp(trimmed, "ctrl ", 5) == 0 && trimmed[5]) {
        unsigned char c = (unsigned char)tolower((unsigned char)trimmed[5]);
        if (c >= 'a' && c <= 'z') {
            fprintf(f, "  printf '\\%03o' >>\"$AUTOTEST_TUI_INPUT_FILE\"\n", c - 'a' + 1);
            fprintf(f, "  printf '\\%03o'\n", c - 'a' + 1);
        } else {
            fputs("  echo '[NG] invalid ctrl key in @tui block' >&2\n  exit 1\n", f);
        }
    } else if (trimmed[0]) {
        fputs("  echo ", f);
        shell_quote_len(f, "[NG] unknown command in @tui block: ", 36);
        fputs(" >&2\n", f);
        fputs("  echo ", f);
        shell_quote_len(f, trimmed, strlen(trimmed));
        fputs(" >&2\n  exit 1\n", f);
    }
}

static void write_shell_line(FILE *f, const char *line, size_t len) {
    fwrite(line, 1, len, f);
    fputc('\n', f);
}

static void write_inline_check_capture(FILE *f, const CheckRule *check) {
    const char *var = check->var[0] ? check->var : "AUTOTEST_ACTUAL";
    fputs("AUTOTEST_CHECK_INDEX=$(( ${AUTOTEST_CHECK_INDEX:-0} + 1 ))\n", f);
    fputs("printf '%s' \"$AUTOTEST_CHECK_INDEX\" >\"$actual_dir/check_count\"\n", f);
    fputs("printf '%s' ", f);
    shell_quote(f, var);
    fputs(" >\"$actual_dir/check_var_${AUTOTEST_CHECK_INDEX}\"\n", f);
    fputs("printf '%s' ", f);
    shell_quote(f, match_name(check->match));
    fputs(" >\"$actual_dir/check_match_${AUTOTEST_CHECK_INDEX}\"\n", f);
    fputs("printf '%s' ", f);
    shell_quote(f, check->expected);
    fputs(" >\"$actual_dir/expected_${AUTOTEST_CHECK_INDEX}\"\n", f);
    fprintf(f, "printf '%%s' \"${%s-}\" >\"$actual_dir/actual_${AUTOTEST_CHECK_INDEX}\"\n", var);
}

static void write_command_expanded(FILE *f, const char *command) {
    bool in_tui = false;
    char tui_cmd[LONG_LEN];
    const char *p = command;
    while (p && *p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (!in_tui) {
            char directive_arg[LONG_LEN];
            if (line_starts_tui(p, len, tui_cmd, sizeof(tui_cmd))) {
                fputs("if ! command -v script >/dev/null 2>&1; then echo '[NG] missing script command' >&2; exit 1; fi\n", f);
                fputs("AUTOTEST_TUI_INDEX=$(( ${AUTOTEST_TUI_INDEX:-0} + 1 ))\n", f);
                fputs("AUTOTEST_TUI_DIR=\"${AUTOTEST_TUI_DIR:-${WORK_DIR:-/tmp}/autotest-tui}\"\n", f);
                fputs("AUTOTEST_TUI_TERM=\"${TERM:-xterm}\"\n", f);
                fputs("[ \"$AUTOTEST_TUI_TERM\" = dumb ] && AUTOTEST_TUI_TERM=xterm\n", f);
                fputs("mkdir -p \"$AUTOTEST_TUI_DIR\"\n", f);
                fputs("AUTOTEST_TUI_TRANSCRIPT_FILE=\"$AUTOTEST_TUI_DIR/tui_${AUTOTEST_TUI_INDEX}.transcript\"\n", f);
                fputs("AUTOTEST_TUI_STDOUT_FILE=\"$AUTOTEST_TUI_DIR/tui_${AUTOTEST_TUI_INDEX}.stdout\"\n", f);
                fputs("AUTOTEST_TUI_STDERR_FILE=\"$AUTOTEST_TUI_DIR/tui_${AUTOTEST_TUI_INDEX}.stderr\"\n", f);
                fputs("AUTOTEST_TUI_INPUT_FILE=\"$AUTOTEST_TUI_DIR/tui_${AUTOTEST_TUI_INDEX}.input\"\n", f);
                fputs("AUTOTEST_TUI_TEXT_FILE=\"$AUTOTEST_TUI_STDOUT_FILE\"\n", f);
                fputs(": >\"$AUTOTEST_TUI_INPUT_FILE\"\n", f);
                fputs("{\n", f);
                in_tui = true;
            } else if (line_starts_directive(p, len, "@assert", directive_arg, sizeof(directive_arg))) {
                fputs("if ! ", f);
                fputs(directive_arg, f);
                fputs("; then echo ", f);
                shell_quote_len(f, "[NG] assert failed: ", 20);
                fputs(" >&2; echo ", f);
                shell_quote_len(f, directive_arg, strlen(directive_arg));
                fputs(" >&2; exit 1; fi\n", f);
            } else if (line_starts_directive(p, len, "@backup", directive_arg, sizeof(directive_arg))) {
                fputs("autotest_backup ", f);
                fputs(directive_arg, f);
                fputc('\n', f);
            } else if (line_starts_directive(p, len, "@restore", directive_arg, sizeof(directive_arg))) {
                fputs("autotest_restore ", f);
                fputs(directive_arg, f);
                fputc('\n', f);
            } else if (line_starts_directive(p, len, "@check", directive_arg, sizeof(directive_arg))) {
                CheckRule parsed;
                char heredoc_delim[TEXT_LEN];
                if (parse_check_arg(directive_arg, &parsed, heredoc_delim, sizeof(heredoc_delim))) {
                    if (heredoc_delim[0]) {
                        const char *after = NULL;
                        collect_check_heredoc(end ? end + 1 : NULL, heredoc_delim,
                                              parsed.expected, sizeof(parsed.expected), &after);
                        write_inline_check_capture(f, &parsed);
                        fputs(": # @check handled by autotest builder\n", f);
                        p = after;
                        continue;
                    }
                    write_inline_check_capture(f, &parsed);
                }
                fputs(": # @check handled by autotest builder\n", f);
            } else if (line_is_tui_end(p, len)) {
                fputs("echo '[NG] @end without @tui' >&2\nexit 1\n", f);
            } else {
                write_shell_line(f, p, len);
            }
        } else {
            if (line_is_tui_end(p, len)) {
                fputs("} | TERM=\"$AUTOTEST_TUI_TERM\" script -q -c ", f);
                shell_quote_len(f, tui_cmd, strlen(tui_cmd));
                fputs(" \"$AUTOTEST_TUI_TRANSCRIPT_FILE\" >/dev/null 2>\"$AUTOTEST_TUI_STDERR_FILE\"\n", f);
                fputs("AUTOTEST_TUI_STATUS=\"$?\"\n", f);
                fputs("autotest_clean_tui_output \"$AUTOTEST_TUI_TRANSCRIPT_FILE\" \"$AUTOTEST_TUI_STDOUT_FILE\"\n", f);
                fputs("autotest_strip_tui_echo \"$AUTOTEST_TUI_STDOUT_FILE\" \"$AUTOTEST_TUI_INPUT_FILE\"\n", f);
                fputs("AUTOTEST_TUI_TRANSCRIPT=\"$(cat \"$AUTOTEST_TUI_TRANSCRIPT_FILE\" 2>/dev/null || true)\"\n", f);
                fputs("AUTOTEST_TUI_STDOUT=\"$(cat \"$AUTOTEST_TUI_STDOUT_FILE\" 2>/dev/null || true)\"\n", f);
                fputs("AUTOTEST_TUI_STDERR=\"$(cat \"$AUTOTEST_TUI_STDERR_FILE\" 2>/dev/null || true)\"\n", f);
                fputs("AUTOTEST_TUI_TEXT=\"$AUTOTEST_TUI_STDOUT\"\n", f);
                fputs("export AUTOTEST_TUI_STATUS AUTOTEST_TUI_STDOUT AUTOTEST_TUI_STDERR AUTOTEST_TUI_TEXT AUTOTEST_TUI_TRANSCRIPT AUTOTEST_TUI_STDOUT_FILE AUTOTEST_TUI_STDERR_FILE AUTOTEST_TUI_TEXT_FILE AUTOTEST_TUI_TRANSCRIPT_FILE AUTOTEST_TUI_INPUT_FILE\n", f);
                in_tui = false;
            } else if (line_starts_tui(p, len, tui_cmd, sizeof(tui_cmd))) {
                fputs("  echo '[NG] nested @tui block' >&2\n  exit 1\n", f);
            } else {
                write_tui_instruction(f, p, len);
            }
        }
        p = end ? end + 1 : NULL;
    }
    if (in_tui) {
        fputs("  echo '[NG] missing @end for @tui block' >&2\n  exit 1\n", f);
        fputs("} | TERM=\"$AUTOTEST_TUI_TERM\" script -q -c ", f);
        shell_quote_len(f, tui_cmd, strlen(tui_cmd));
        fputs(" \"$AUTOTEST_TUI_TRANSCRIPT_FILE\" >/dev/null 2>\"$AUTOTEST_TUI_STDERR_FILE\"\n", f);
        fputs("AUTOTEST_TUI_STATUS=\"$?\"\n", f);
        fputs("autotest_clean_tui_output \"$AUTOTEST_TUI_TRANSCRIPT_FILE\" \"$AUTOTEST_TUI_STDOUT_FILE\"\n", f);
        fputs("autotest_strip_tui_echo \"$AUTOTEST_TUI_STDOUT_FILE\" \"$AUTOTEST_TUI_INPUT_FILE\"\n", f);
        fputs("AUTOTEST_TUI_TRANSCRIPT=\"$(cat \"$AUTOTEST_TUI_TRANSCRIPT_FILE\" 2>/dev/null || true)\"\n", f);
        fputs("AUTOTEST_TUI_STDOUT=\"$(cat \"$AUTOTEST_TUI_STDOUT_FILE\" 2>/dev/null || true)\"\n", f);
        fputs("AUTOTEST_TUI_STDERR=\"$(cat \"$AUTOTEST_TUI_STDERR_FILE\" 2>/dev/null || true)\"\n", f);
        fputs("AUTOTEST_TUI_TEXT=\"$AUTOTEST_TUI_STDOUT\"\n", f);
        fputs("export AUTOTEST_TUI_STATUS AUTOTEST_TUI_STDOUT AUTOTEST_TUI_STDERR AUTOTEST_TUI_TEXT AUTOTEST_TUI_TRANSCRIPT AUTOTEST_TUI_STDOUT_FILE AUTOTEST_TUI_STDERR_FILE AUTOTEST_TUI_TEXT_FILE AUTOTEST_TUI_TRANSCRIPT_FILE AUTOTEST_TUI_INPUT_FILE\n", f);
    }
}

static void set_syntax_error(SyntaxError *err, int line_no, const char *message) {
    if (!err) return;
    err->line_no = line_no;
    copy_text(err->message, sizeof(err->message), message);
}

static bool is_valid_var_name(const char *s) {
    if (!s || !s[0]) return false;
    if (!(isalpha((unsigned char)s[0]) || s[0] == '_')) return false;
    for (const char *p = s + 1; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '_')) return false;
    }
    return true;
}

static bool trimmed_has_directive(const char *trimmed, const char *name) {
    size_t len = strlen(name);
    return strncmp(trimmed, name, len) == 0 &&
           (trimmed[len] == '\0' || isspace((unsigned char)trimmed[len]));
}

static bool validate_check_directive(const char *arg, int line_no, SyntaxError *err,
                                     char *heredoc_delim, size_t heredoc_delim_sz) {
    CheckRule parsed;
    char buf[LONG_LEN];
    char *var;
    char *match;
    char *expected;
    if (heredoc_delim && heredoc_delim_sz > 0) heredoc_delim[0] = '\0';
    copy_text(buf, sizeof(buf), arg);
    var = buf;
    while (isspace((unsigned char)*var)) var++;
    match = var;
    while (*match && !isspace((unsigned char)*match)) match++;
    if (*match) *match++ = '\0';
    while (isspace((unsigned char)*match)) match++;
    expected = match;
    while (*expected && !isspace((unsigned char)*expected)) expected++;
    if (*expected) *expected++ = '\0';
    while (isspace((unsigned char)*expected)) expected++;
    if (!var[0] || !match[0]) {
        set_syntax_error(err, line_no, "@check requires: @check <var> <match> <expected>");
        return false;
    }
    if (!is_valid_var_name(var)) {
        set_syntax_error(err, line_no, "@check variable name is invalid");
        return false;
    }
    if (!is_match_name(match)) {
        set_syntax_error(err, line_no, "@check match must be exact, not_exact, contains, not_contains, empty, not_empty, or none");
        return false;
    }
    if (strncmp(expected, "<<", 2) == 0) {
        char heredoc[TEXT_LEN];
        if (!parse_check_arg(arg, &parsed, heredoc, sizeof(heredoc)) || !heredoc[0]) {
            set_syntax_error(err, line_no, "@check heredoc delimiter is missing");
            return false;
        }
        copy_text(heredoc_delim, heredoc_delim_sz, heredoc);
        return true;
    }
    if (!parse_check_arg(arg, &parsed, heredoc_delim, heredoc_delim_sz)) {
        set_syntax_error(err, line_no, "@check requires: @check <var> <match> <expected>");
        return false;
    }
    return true;
}

static bool validate_tui_instruction(const char *line, size_t len, int line_no, SyntaxError *err) {
    char trimmed[LONG_LEN];
    char decoded[LONG_LEN];
    trim_line_copy(trimmed, sizeof(trimmed), line, len);
    if (!trimmed[0]) return true;
    if (strncmp(trimmed, "send ", 5) == 0) {
        if (!decode_tui_argument_checked(trimmed + 5, decoded, sizeof(decoded))) {
            set_syntax_error(err, line_no, "unterminated quote in send");
            return false;
        }
        return true;
    }
    if (strncmp(trimmed, "text ", 5) == 0) {
        if (!decode_tui_argument_checked(trimmed + 5, decoded, sizeof(decoded))) {
            set_syntax_error(err, line_no, "unterminated quote in text");
            return false;
        }
        return true;
    }
    if (strncmp(trimmed, "send-shell ", 11) == 0) {
        if (trimmed[11]) return true;
        set_syntax_error(err, line_no, "send-shell requires an expression");
        return false;
    }
    if (strncmp(trimmed, "text-shell ", 11) == 0) {
        if (trimmed[11]) return true;
        set_syntax_error(err, line_no, "text-shell requires an expression");
        return false;
    }
    if (strcmp(trimmed, "enter") == 0 || strcmp(trimmed, "esc") == 0 ||
        strcmp(trimmed, "tab") == 0 || strcmp(trimmed, "space") == 0 ||
        strcmp(trimmed, "vim-clear") == 0 || strcmp(trimmed, "vim-write-quit") == 0) {
        return true;
    }
    if (strncmp(trimmed, "sleep ", 6) == 0) {
        bool digit = false;
        int dots = 0;
        for (const char *p = trimmed + 6; *p; p++) {
            if (isdigit((unsigned char)*p)) {
                digit = true;
            } else if (*p == '.') {
                dots++;
            } else {
                set_syntax_error(err, line_no, "sleep expects a numeric value");
                return false;
            }
        }
        if (!digit || dots > 1) {
            set_syntax_error(err, line_no, "sleep expects a numeric value");
            return false;
        }
        return true;
    }
    if (strncmp(trimmed, "ctrl ", 5) == 0) {
        const char *p = trimmed + 5;
        if (isalpha((unsigned char)p[0]) && p[1] == '\0') return true;
        set_syntax_error(err, line_no, "ctrl expects one alphabet key, example: ctrl c");
        return false;
    }
    set_syntax_error(err, line_no, "unknown command in @tui block");
    return false;
}

static bool validate_custom_syntax(const char *command, SyntaxError *err) {
    bool in_tui = false;
    char tui_cmd[LONG_LEN];
    char heredoc_delim[TEXT_LEN] = "";
    int heredoc_line = 0;
    int line_no = 1;
    const char *p = command;
    if (err) {
        err->line_no = 0;
        err->message[0] = '\0';
    }
    while (p && *p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char trimmed[LONG_LEN];
        trim_line_copy(trimmed, sizeof(trimmed), p, len);
        if (heredoc_delim[0]) {
            if (strcmp(trimmed, heredoc_delim) == 0) {
                heredoc_delim[0] = '\0';
                heredoc_line = 0;
            }
            p = end ? end + 1 : NULL;
            line_no++;
            continue;
        }
        if (!in_tui) {
            char directive_arg[LONG_LEN];
            if (line_starts_tui(p, len, tui_cmd, sizeof(tui_cmd))) {
                in_tui = true;
            } else if (trimmed_has_directive(trimmed, "@tui")) {
                set_syntax_error(err, line_no, "@tui requires a command");
                return false;
            } else if (line_is_tui_end(p, len)) {
                set_syntax_error(err, line_no, "@end without @tui");
                return false;
            } else if (line_starts_directive(p, len, "@check", directive_arg, sizeof(directive_arg))) {
                if (!validate_check_directive(directive_arg, line_no, err, heredoc_delim, sizeof(heredoc_delim))) {
                    return false;
                }
                if (heredoc_delim[0]) heredoc_line = line_no;
            } else if (trimmed_has_directive(trimmed, "@check")) {
                set_syntax_error(err, line_no, "@check requires: @check <var> <match> <expected>");
                return false;
            } else if (line_starts_directive(p, len, "@assert", directive_arg, sizeof(directive_arg))) {
                /* bash -n validates the condition after expansion. */
            } else if (trimmed_has_directive(trimmed, "@assert")) {
                set_syntax_error(err, line_no, "@assert requires a condition");
                return false;
            } else if (line_starts_directive(p, len, "@backup", directive_arg, sizeof(directive_arg))) {
                /* Generated shell validates quoting and syntax. */
            } else if (trimmed_has_directive(trimmed, "@backup")) {
                set_syntax_error(err, line_no, "@backup requires a path");
                return false;
            } else if (line_starts_directive(p, len, "@restore", directive_arg, sizeof(directive_arg))) {
                /* Generated shell validates quoting and syntax. */
            } else if (trimmed_has_directive(trimmed, "@restore")) {
                set_syntax_error(err, line_no, "@restore requires a path");
                return false;
            } else if (line_starts_reboot_if(p, len, directive_arg, sizeof(directive_arg))) {
                /* write_reboot_command expands this later. */
            } else if (trimmed_has_directive(trimmed, "@reboot-if")) {
                set_syntax_error(err, line_no, "@reboot-if requires a condition");
                return false;
            } else if (trimmed[0] == '@') {
                set_syntax_error(err, line_no, "unknown directive");
                return false;
            }
        } else {
            if (line_is_tui_end(p, len)) {
                in_tui = false;
            } else if (line_starts_tui(p, len, tui_cmd, sizeof(tui_cmd)) ||
                       trimmed_has_directive(trimmed, "@tui")) {
                set_syntax_error(err, line_no, "nested @tui block is not allowed");
                return false;
            } else if (!validate_tui_instruction(p, len, line_no, err)) {
                return false;
            }
        }
        p = end ? end + 1 : NULL;
        line_no++;
    }
    if (heredoc_delim[0]) {
        set_syntax_error(err, heredoc_line, "@check heredoc delimiter is not closed");
        return false;
    }
    if (in_tui) {
        set_syntax_error(err, line_no > 1 ? line_no - 1 : 1, "missing @end for @tui block");
        return false;
    }
    return true;
}

static bool validate_generated_bash(const char *command, SyntaxError *err) {
    char script_path[TEXT_LEN];
    char err_path[LONG_LEN];
    char cmd[LONG_LEN * 2];
    FILE *f;
    int rc;
    snprintf(script_path, sizeof(script_path), "/tmp/autotest-syntax-%ld.sh", (long)getpid());
    f = fopen(script_path, "w");
    if (!f) {
        set_syntax_error(err, 0, "failed to write syntax check temp file");
        return false;
    }
    fputs("#!/usr/bin/env bash\n", f);
    write_command_expanded(f, command);
    fclose(f);
    snprintf(err_path, sizeof(err_path), "%s.err", script_path);
    snprintf(cmd, sizeof(cmd), "bash -n %s 2>%s", script_path, err_path);
    rc = system(cmd);
    unlink(script_path);
    if (rc != 0) {
        FILE *ef = fopen(err_path, "r");
        char line[TEXT_LEN] = "generated shell syntax error";
        if (ef) {
            if (fgets(line, sizeof(line), ef)) {
                line[strcspn(line, "\r\n")] = '\0';
            }
            fclose(ef);
        }
        unlink(err_path);
        set_syntax_error(err, 0, line);
        return false;
    }
    unlink(err_path);
    return true;
}

static bool validate_script_syntax(const char *command, SyntaxError *err) {
    if (!validate_custom_syntax(command, err)) return false;
    return validate_generated_bash(command, err);
}

static void write_command_phase(FILE *f, const char *command, bool want_post) {
    bool after_reboot = false;
    char phase[LONG_LEN];
    size_t phase_pos = 0;
    const char *p = command;
    phase[0] = '\0';
    while (p && *p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (line_is_reboot_boundary(p, len)) {
            after_reboot = true;
        } else if (after_reboot == want_post) {
            if (phase_pos + len + 2 < sizeof(phase)) {
                memcpy(phase + phase_pos, p, len);
                phase_pos += len;
                phase[phase_pos++] = '\n';
                phase[phase_pos] = '\0';
            }
        }
        p = end ? end + 1 : NULL;
    }
    write_command_expanded(f, phase);
}

static void write_reboot_command(FILE *f, const char *command) {
    const char *p = command;
    while (p && *p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char condition[LONG_LEN];
        if (line_starts_reboot_if(p, len, condition, sizeof(condition))) {
            fprintf(f, "if %s; then\n", condition);
            fputs("  reboot\n", f);
            fputs("else\n", f);
            fputs("  exit 125\n", f);
            fputs("fi\n", f);
            return;
        } else if (line_is_reboot_command(p, len)) {
            fwrite(p, 1, len, f);
            fputc('\n', f);
            return;
        }
        p = end ? end + 1 : NULL;
    }
    fputs("reboot\n", f);
}

static void make_safe_id(const char *id, char *out, size_t out_sz, char replacement) {
    size_t j = 0;
    if (out_sz == 0) return;
    for (size_t i = 0; id && id[i] && j + 1 < out_sz; i++) {
        unsigned char c = (unsigned char)id[i];
        out[j++] = (char)(isalnum(c) ? tolower(c) : replacement);
    }
    if (j == 0) {
        copy_text(out, out_sz, "test");
    } else {
        out[j] = '\0';
    }
}

static bool cleanup_has_path(const char *cleanup, const char *path) {
    return cleanup && path && strstr(cleanup, path) != NULL;
}

static void append_cleanup_path(TestCase *tc, const char *path) {
    if (!path[0] || cleanup_has_path(tc->cleanup, path)) return;
    char quoted[LONG_LEN];
    char line[LONG_LEN];
    shell_quote_buf(quoted, sizeof(quoted), path);
    const char *prefix = "rm -rf ";
    size_t prefix_len = strlen(prefix);
    size_t quoted_len = strlen(quoted);
    if (prefix_len + quoted_len + 1 > sizeof(line)) return;
    memcpy(line, prefix, prefix_len);
    memcpy(line + prefix_len, quoted, quoted_len + 1);
    size_t used = strlen(tc->cleanup);
    size_t add = strlen(line);
    if (used + add + 4 >= sizeof(tc->cleanup)) return;
    if (used > 0) {
        strcat(tc->cleanup, " ; ");
    }
    strcat(tc->cleanup, line);
}

static void collect_temp_paths(TestCase *tc, const char *prefix) {
    const char *p = test_command(tc);
    size_t prefix_len = strlen(prefix);
    while ((p = strstr(p, prefix)) != NULL) {
        char path[TEXT_LEN];
        size_t len = 0;
        while (p[len] &&
               !isspace((unsigned char)p[len]) &&
               p[len] != '\'' &&
               p[len] != '"' &&
               p[len] != ';' &&
               p[len] != '|' &&
               p[len] != '&' &&
               p[len] != ')' &&
               p[len] != '(' &&
               p[len] != '<' &&
               p[len] != '>' &&
               len + 1 < sizeof(path)) {
            path[len] = p[len];
            len++;
        }
        while (len > prefix_len && (path[len - 1] == ',' || path[len - 1] == ']' || path[len - 1] == '}')) {
            len--;
        }
        path[len] = '\0';
        if (len > prefix_len) append_cleanup_path(tc, path);
        p += prefix_len;
    }
}

static bool parse_check_arg(const char *arg, CheckRule *out, char *heredoc_delim, size_t heredoc_delim_sz) {
    char buf[LONG_LEN];
    copy_text(buf, sizeof(buf), arg);
    if (heredoc_delim && heredoc_delim_sz > 0) heredoc_delim[0] = '\0';
    char *var = buf;
    while (isspace((unsigned char)*var)) var++;
    char *match = var;
    while (*match && !isspace((unsigned char)*match)) match++;
    if (*match) *match++ = '\0';
    while (isspace((unsigned char)*match)) match++;
    char *expected = match;
    while (*expected && !isspace((unsigned char)*expected)) expected++;
    if (*expected) *expected++ = '\0';
    while (isspace((unsigned char)*expected)) expected++;
    if (!var[0] || !match[0]) return false;
    memset(out, 0, sizeof(*out));
    copy_text(out->var, sizeof(out->var), var);
    out->match = parse_match(match);
    if (strncmp(expected, "<<", 2) == 0) {
        char *delim = expected + 2;
        if (*delim == '-') delim++;
        while (isspace((unsigned char)*delim)) delim++;
        if (*delim == '\'' || *delim == '"') {
            char quote = *delim++;
            char *end = strchr(delim, quote);
            if (end) *end = '\0';
        } else {
            char *end = delim;
            while (*end && !isspace((unsigned char)*end)) end++;
            *end = '\0';
        }
        if (delim[0] && heredoc_delim && heredoc_delim_sz > 0) {
            copy_text(heredoc_delim, heredoc_delim_sz, delim);
            out->expected[0] = '\0';
            return true;
        }
    }
    unescape_field(out->expected, sizeof(out->expected), expected);
    return true;
}

static void collect_check_heredoc(const char *body_start, const char *delim, char *out, size_t out_sz, const char **after) {
    const char *p = body_start;
    size_t pos = 0;
    bool first = true;
    if (out_sz > 0) out[0] = '\0';
    while (p && *p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char trimmed[TEXT_LEN];
        trim_line_copy(trimmed, sizeof(trimmed), p, len);
        if (strcmp(trimmed, delim) == 0) {
            if (after) *after = end ? end + 1 : NULL;
            return;
        }
        if (!first && pos + 1 < out_sz) out[pos++] = '\n';
        first = false;
        if (pos + len >= out_sz) len = out_sz > pos ? out_sz - pos - 1 : 0;
        if (len > 0) {
            memcpy(out + pos, p, len);
            pos += len;
        }
        if (out_sz > 0) out[pos] = '\0';
        p = end ? end + 1 : NULL;
    }
    if (after) *after = NULL;
}

static void collect_check_directives(TestCase *tc) {
    int count = 0;
    bool stored_first = false;
    const char *p = test_command(tc);
    memset(tc->checks, 0, sizeof(tc->checks));
    while (p && *p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char arg[LONG_LEN];
        if (line_starts_directive(p, len, "@check", arg, sizeof(arg))) {
            CheckRule parsed;
            char heredoc_delim[TEXT_LEN];
            if (parse_check_arg(arg, &parsed, heredoc_delim, sizeof(heredoc_delim))) {
                const char *next = end ? end + 1 : NULL;
                if (heredoc_delim[0]) {
                    collect_check_heredoc(next, heredoc_delim, parsed.expected, sizeof(parsed.expected), &next);
                    p = next;
                } else {
                    p = next;
                }
                if (!stored_first) {
                    CheckRule *check = &tc->checks[0];
                    *check = parsed;
                    stored_first = true;
                }
                count++;
                continue;
            }
        }
        p = end ? end + 1 : NULL;
    }
    tc->check_count = count;
}

static bool next_check_directive(const char **cursor, CheckRule *out) {
    const char *p = cursor ? *cursor : NULL;
    while (p && *p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char arg[LONG_LEN];
        if (line_starts_directive(p, len, "@check", arg, sizeof(arg))) {
            char heredoc_delim[TEXT_LEN];
            if (parse_check_arg(arg, out, heredoc_delim, sizeof(heredoc_delim))) {
                const char *next = end ? end + 1 : NULL;
                if (heredoc_delim[0]) {
                    collect_check_heredoc(next, heredoc_delim, out->expected, sizeof(out->expected), &next);
                }
                *cursor = next;
                return true;
            }
        }
        p = end ? end + 1 : NULL;
    }
    if (cursor) *cursor = NULL;
    return false;
}

static int command_check_count(const char *command) {
    int count = 0;
    const char *cursor = command;
    CheckRule check;
    while (next_check_directive(&cursor, &check)) count++;
    return count;
}

static bool legacy_primary_check_enabled(const TestCase *tc) {
    return tc &&
           command_check_count(test_command(tc)) == 0 &&
           tc->check_count > 0 &&
           tc->checks[0].var[0] &&
           tc->checks[0].match != MATCH_NONE;
}

static bool next_effective_check(const TestCase *tc, const char **cursor, CheckRule *out, bool *used_legacy) {
    if (next_check_directive(cursor, out)) return true;
    if (used_legacy && !*used_legacy && legacy_primary_check_enabled(tc)) {
        *out = tc->checks[0];
        *used_legacy = true;
        return true;
    }
    return false;
}

static void auto_configure_test(TestCase *tc) {
    collect_check_directives(tc);
    tc->cleanup[0] = '\0';
    collect_temp_paths(tc, "/tmp/");
    collect_temp_paths(tc, "/var/tmp/");
    if (!test_command(tc)[0]) return;
    if (command_looks_like_reboot(test_command(tc))) {
        tc->kind = CMD_REBOOT;
    } else if (tc->kind == CMD_REBOOT) {
        tc->kind = CMD_SHELL;
    }
}

static void make_test_filename(const TestCase *tc, char *out, size_t out_sz) {
    char safe_name[TEXT_LEN];
    const char *src = tc->title[0] ? tc->title : tc->id;
    size_t j = 0;
    bool has_name = false;
    bool last_underscore = false;
    for (size_t i = 0; src[i] && j + 1 < sizeof(safe_name); i++) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c)) {
            safe_name[j++] = (char)tolower(c);
            has_name = true;
            last_underscore = false;
        } else if (c >= 0x80 || c == '.' || c == '-') {
            safe_name[j++] = (char)c;
            has_name = true;
            last_underscore = false;
        } else if (c == '_' || isspace(c)) {
            if (has_name && !last_underscore) {
                safe_name[j++] = '_';
                last_underscore = true;
            }
        } else if (has_name && !last_underscore) {
            safe_name[j++] = '_';
            last_underscore = true;
        }
    }
    while (j > 0 && safe_name[j - 1] == '_') j--;
    if (!has_name || j == 0) {
        copy_text(safe_name, sizeof(safe_name), "test");
    } else {
        safe_name[j] = '\0';
    }
    snprintf(out, out_sz, "%s.sh", safe_name);
}

static void make_result_path(const char *script_path, char *out, size_t out_sz) {
    copy_text(out, out_sz, script_path);
    size_t len = strlen(out);
    if (len > 3 && strcmp(out + len - 3, ".sh") == 0) {
        snprintf(out + len - 3, out_sz - len + 3, ".result");
    } else if (len + 8 < out_sz) {
        strcat(out, ".result");
    }
}

static int make_test_path(const TestCase *tc, char *out, size_t out_sz) {
    char dir[LONG_LEN];
    char filename[TEXT_LEN];
    make_test_filename(tc, filename, sizeof(filename));
    if (tc->script_path[0]) {
        copy_text(dir, sizeof(dir), tc->script_path);
        char *slash = strrchr(dir, '/');
        if (slash) {
            *slash = '\0';
        } else {
            copy_text(dir, sizeof(dir), ".");
        }
    } else if (!getcwd(dir, sizeof(dir))) {
        return -1;
    }
    size_t dir_len = strlen(dir);
    size_t file_len = strlen(filename);
    if (dir_len + 1 + file_len + 1 > out_sz) return -1;
    memcpy(out, dir, dir_len);
    out[dir_len] = '/';
    memcpy(out + dir_len + 1, filename, file_len + 1);
    return 0;
}

static bool title_path_conflicts(const TestCase *tc, const char *title) {
    TestCase tmp = *tc;
    char path[LONG_LEN];
    copy_text(tmp.title, sizeof(tmp.title), title);
    if (make_test_path(&tmp, path, sizeof(path)) != 0) return false;
    if (tmp.script_path[0] && strcmp(tmp.script_path, path) == 0) return false;
    return access(path, F_OK) == 0;
}

static int write_single_test_script(TestCase *tc) {
    auto_configure_test(tc);
    char path[LONG_LEN];
    char old_path[LONG_LEN];
    copy_text(old_path, sizeof(old_path), tc->script_path);
    if (make_test_path(tc, path, sizeof(path)) != 0) return -1;
    if ((!old_path[0] || strcmp(old_path, path) != 0) && access(path, F_OK) == 0) {
        return -2;
    }

    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "#!/usr/bin/env bash\n");
    write_comment_block(f, "Title", tc->title);
    write_comment_block(f, "Description", tc->description);
    fputc('\n', f);
    fprintf(f, "set -u\n\n");
    fprintf(f, "TOTAL_COUNT=0\n");
    fprintf(f, "OK_COUNT=0\n");
    fprintf(f, "NG_COUNT=0\n\n");
    fprintf(f, "SELF_PATH=\"$(readlink -f \"$0\" 2>/dev/null || printf '%%s\\n' \"$0\")\"\n");
    fprintf(f, "RESULT_FILE=\"${SELF_PATH%%.sh}.result\"\n\n");
    fprintf(f, "MODE=run\n");
    fprintf(f, "DETAIL_RESULT_FILE=\"\"\n");
    fprintf(f, "DETAIL_STDOUT=0\n");
    fprintf(f, "usage() { echo \"Usage: $0 [--detail [PATH]]\"; echo \"       $0 --resume\"; }\n");
    fprintf(f, "while [ \"$#\" -gt 0 ]; do\n");
    fprintf(f, "  case \"$1\" in\n");
    fprintf(f, "    --resume) MODE=--resume; shift ;;\n");
    fprintf(f, "    --detail) shift; if [ \"$#\" -gt 0 ] && [ \"${1#-}\" = \"$1\" ]; then DETAIL_RESULT_FILE=\"$1\"; shift; else DETAIL_STDOUT=1; fi ;;\n");
    fprintf(f, "    --detail=*) DETAIL_RESULT_FILE=\"${1#--detail=}\"; shift ;;\n");
    fprintf(f, "    --help) usage; exit 0 ;;\n");
    fprintf(f, "    --*) usage >&2; echo \"[NG] unknown option: $1\" >&2; exit 2 ;;\n");
    fprintf(f, "    *) usage >&2; echo \"[NG] unexpected argument: $1\" >&2; exit 2 ;;\n");
    fprintf(f, "  esac\n");
    fprintf(f, "done\n\n");
    fprintf(f, "WORK_DIR=\"${TMPDIR:-/tmp}/autotest-single.$$\"\n");
    fprintf(f, "mkdir -p \"$WORK_DIR\"\n");
    fprintf(f, "AUTOTEST_BACKUP_DIR=\"$WORK_DIR/backups\"\n");
    fprintf(f, "mkdir -p \"$AUTOTEST_BACKUP_DIR\"\n");
    fprintf(f, "cleanup() {\n");
    fprintf(f, "  rm -rf \"$WORK_DIR\"\n");
    if (tc->cleanup[0]) fprintf(f, "  %s || true\n", tc->cleanup);
    fprintf(f, "}\ntrap cleanup EXIT\n\n");
    write_match_function(f);
    write_case(f, tc, 0);
    fprintf(f, "run_case_001 \"$MODE\"\n");
    write_summary(f);
    fclose(f);
    chmod(path, 0755);
    if (old_path[0] && strcmp(old_path, path) != 0) {
        char old_result[LONG_LEN];
        unlink(old_path);
        make_result_path(old_path, old_result, sizeof(old_result));
        unlink(old_result);
    }
    copy_text(tc->script_path, sizeof(tc->script_path), path);
    return 0;
}

static void open_text_editor(App *app, EditorTarget target, Screen return_screen) {
    app->editor_target = target;
    app->previous_screen = return_screen;
    app->regex_template_list_open = false;
    app->editor_help_open = false;
    app->editor_help_scroll = 0;
    load_editor_from_case(app);
    app->screen = SCREEN_SCRIPT_EDITOR;
    app->selected_menu = 0;
    set_status(app, "Text editor: i insert, Esc normal, :w save, :wq save quit, :q! discard.");
}

static void open_script_editor(App *app) {
    open_text_editor(app, EDIT_TARGET_COMMAND, SCREEN_EDITOR);
}

static void discard_current_case_if_new(App *app) {
    if (!app->editor_new_case || app->project.case_count == 0) return;
    int idx = app->selected_case;
    if (idx < 0 || idx >= app->project.case_count) return;
    free_test_case(&app->project.cases[idx]);
    for (int i = idx; i < app->project.case_count - 1; i++) {
        app->project.cases[i] = app->project.cases[i + 1];
    }
    app->project.case_count--;
    if (app->project.case_count >= 0) memset(&app->project.cases[app->project.case_count], 0, sizeof(app->project.cases[app->project.case_count]));
    app->editor_new_case = false;
    clamp_selected(app);
}

static bool editor_has_unsaved_changes(const App *app) {
    return app->editor_dirty ||
           (app->editor_new_case && app->editor_target == EDIT_TARGET_COMMAND);
}

static int editor_buffer_bytes(const App *app) {
    int bytes = 0;
    for (int i = 0; i < app->editor_line_count; i++) {
        bytes += (int)strlen(app->editor_lines[i]);
        if (i + 1 < app->editor_line_count) bytes++;
    }
    return bytes;
}

static void set_editor_written_status(App *app, const char *name) {
    snprintf(app->status, sizeof(app->status), "\"%s\" %dL, %dB written",
             name, app->editor_line_count, editor_buffer_bytes(app));
}

static bool save_current_editor(App *app, bool exit_after_save) {
    if (app->editor_target == EDIT_TARGET_COMMAND) {
        char *candidate = editor_text_alloc(app);
        SyntaxError err;
        if (!candidate) {
            set_status(app, "Out of memory.");
            return false;
        }
        if (!validate_script_syntax(candidate, &err)) {
            free(candidate);
            if (err.line_no > 0) {
                snprintf(app->status, sizeof(app->status),
                         "Syntax error line %d: %s", err.line_no, err.message);
            } else {
                snprintf(app->status, sizeof(app->status),
                         "Syntax error: %s", err.message);
            }
            return false;
        }
        free(candidate);
    }

    save_editor_to_case(app);
    if (app->editor_target == EDIT_TARGET_COMMAND) {
        int rc = write_single_test_script(&app->project.cases[app->selected_case]);
        if (rc == 0) {
            TestCase *tc = &app->project.cases[app->selected_case];
            save_test_registry(&app->project);
            app->editor_new_case = false;
            app->editor_dirty = false;
            set_editor_written_status(app, tc->script_path[0] ? tc->script_path : tc->title);
            if (exit_after_save) app->screen = app->previous_screen;
            return true;
        }
        if (rc == -2) {
            set_status(app, "Script name already exists in this directory.");
        } else {
            set_status(app, "Failed to save test script.");
        }
        return false;
    }

    if (app->project.cases[app->selected_case].script_path[0]) {
        write_single_test_script(&app->project.cases[app->selected_case]);
    }
    save_test_registry(&app->project);
    app->editor_dirty = false;
    set_editor_written_status(app, editor_target_name(app->editor_target));
    if (exit_after_save) app->screen = app->previous_screen;
    return true;
}

static void shell_quote(FILE *f, const char *s) {
    fputc('\'', f);
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '\'') fputs("'\\''", f);
        else fputc(s[i], f);
    }
    fputc('\'', f);
}

static void shell_quote_str(FILE *f, const char *s) {
    shell_quote(f, s);
}

static void shell_quote_buf(char *dst, size_t dst_sz, const char *src) {
    size_t j = 0;
    if (dst_sz == 0) return;
    dst[j++] = '\'';
    for (size_t i = 0; src && src[i] && j + 5 < dst_sz; i++) {
        if (src[i] == '\'') {
            dst[j++] = '\'';
            dst[j++] = '\\';
            dst[j++] = '\'';
            dst[j++] = '\'';
        } else {
            dst[j++] = src[i];
        }
    }
    if (j + 1 < dst_sz) dst[j++] = '\'';
    dst[j] = '\0';
}

static void write_comment_block(FILE *f, const char *label, const char *text) {
    fprintf(f, "# %s:\n", label);
    if (!text || !text[0]) {
        fputs("#   <none>\n", f);
        return;
    }
    const char *p = text;
    while (p && *p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        fputs("#   ", f);
        fwrite(p, 1, len, f);
        fputc('\n', f);
        p = end ? end + 1 : NULL;
    }
}

static void save_test_registry(const Project *p) {
    char path[LONG_LEN];
    if (ensure_registry_dir() != 0) return;
    if (registry_path(path, sizeof(path)) != 0) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# autotest-assist tests v3\n");
    fprintf(f, "# path\ttitle\tselected\tkind\texpected_exit\tcheck_count\tcheck1_var\tcheck1_match\tcheck1_expected\tcheck2_var\tcheck2_match\tcheck2_expected\tcheck3_var\tcheck3_match\tcheck3_expected\tcheck4_var\tcheck4_match\tcheck4_expected\tcommand\tdescription\n");
    for (int i = 0; i < p->case_count; i++) {
        const TestCase *tc = &p->cases[i];
        if (!tc->script_path[0]) continue;
        write_escaped_field(f, tc->script_path);
        fputc('\t', f);
        write_escaped_field(f, tc->title);
        fprintf(f, "\t%d\t", tc->selected ? 1 : 0);
        write_escaped_field(f, command_kind_name(tc->kind));
        fprintf(f, "\t%d\t%d", tc->expected_exit, tc->check_count);
        for (int c = 0; c < LEGACY_REGISTRY_CHECKS; c++) {
            fputc('\t', f);
            write_escaped_field(f, tc->checks[c].var);
            fputc('\t', f);
            write_escaped_field(f, match_name(tc->checks[c].match));
            fputc('\t', f);
            write_escaped_field(f, tc->checks[c].expected);
        }
        fputc('\t', f);
        write_escaped_field(f, test_command(tc));
        fputc('\t', f);
        write_escaped_field(f, tc->description);
        fputc('\n', f);
    }
    fclose(f);
}

static bool load_test_registry(Project *p) {
    char path[LONG_LEN];
    if (registry_path(path, sizeof(path)) != 0) return false;
    FILE *f = fopen(path, "r");
    if (!f) return false;

    p->case_count = 0;
    char *line = NULL;
    while ((line = read_line_alloc(f)) && p->case_count < MAX_CASES) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0') {
            free(line);
            line = NULL;
            continue;
        }
        line[strcspn(line, "\r\n")] = '\0';
        char *fields[25] = {0};
        int field_count = 0;
        char *cursor = line;
        while (field_count < 25) {
            fields[field_count++] = cursor;
            char *tab = strchr(cursor, '\t');
            if (!tab) break;
            *tab = '\0';
            cursor = tab + 1;
        }
        if (field_count < 2 || !fields[0][0]) {
            free(line);
            line = NULL;
            continue;
        }

        TestCase *tc = &p->cases[p->case_count];
        memset(tc, 0, sizeof(*tc));
        snprintf(tc->id, sizeof(tc->id), "TC%03d", p->case_count + 1);
        unescape_field(tc->script_path, sizeof(tc->script_path), fields[0]);
        unescape_field(tc->title, sizeof(tc->title), fields[1]);
        tc->selected = field_count > 2 ? atoi(fields[2]) != 0 : false;
        tc->kind = field_count > 3 ? parse_kind(fields[3]) : CMD_SHELL;
        tc->expected_exit = field_count > 4 ? atoi(fields[4]) : 0;
        tc->check_count = 1;
        copy_text(tc->checks[0].var, sizeof(tc->checks[0].var), "AUTOTEST_ACTUAL");
        tc->checks[0].match = MATCH_NONE;
        if (field_count >= 19) {
            tc->check_count = atoi(fields[5]);
            if (tc->check_count < 1) tc->check_count = 1;
            if (tc->check_count > LEGACY_REGISTRY_CHECKS) tc->check_count = LEGACY_REGISTRY_CHECKS;
            for (int c = 0; c < LEGACY_REGISTRY_CHECKS; c++) {
                int base = 6 + c * 3;
                unescape_field(tc->checks[c].var, sizeof(tc->checks[c].var), fields[base]);
                tc->checks[c].match = parse_match(fields[base + 1]);
                unescape_field(tc->checks[c].expected, sizeof(tc->checks[c].expected), fields[base + 2]);
            }
            if (!tc->checks[0].var[0]) copy_text(tc->checks[0].var, sizeof(tc->checks[0].var), "AUTOTEST_ACTUAL");
            take_test_command(tc, unescape_field_alloc(fields[18]));
            if (field_count > 19) unescape_field(tc->description, sizeof(tc->description), fields[19]);
        } else if (field_count > 7 && is_match_name(fields[5]) && is_match_name(fields[7])) {
            tc->checks[0].match = parse_match(fields[5]);
            if (field_count > 6) unescape_field(tc->checks[0].expected, sizeof(tc->checks[0].expected), fields[6]);
            if (field_count > 9) take_test_command(tc, unescape_field_alloc(fields[9]));
        } else {
            if (field_count > 5) unescape_field(tc->checks[0].var, sizeof(tc->checks[0].var), fields[5]);
            if (!tc->checks[0].var[0]) copy_text(tc->checks[0].var, sizeof(tc->checks[0].var), "AUTOTEST_ACTUAL");
            tc->checks[0].match = field_count > 6 ? parse_match(fields[6]) : MATCH_NONE;
            if (field_count > 7) unescape_field(tc->checks[0].expected, sizeof(tc->checks[0].expected), fields[7]);
            if (field_count > 8) take_test_command(tc, unescape_field_alloc(fields[8]));
        }
        if (!tc->command) set_test_command(tc, "");
        tc->timeout_sec = 30;
        if (!tc->title[0]) copy_text(tc->title, sizeof(tc->title), tc->script_path);
        if (command_check_count(test_command(tc)) > 0) {
            collect_check_directives(tc);
        } else if (tc->checks[0].match == MATCH_NONE) {
            tc->check_count = 0;
            memset(tc->checks, 0, sizeof(tc->checks));
        }
        if (tc->kind != CMD_REBOOT && file_contains_reboot(tc->script_path)) tc->kind = CMD_REBOOT;
        p->case_count++;
        free(line);
        line = NULL;
    }
    free(line);
    fclose(f);
    return p->case_count > 0;
}

static void init_project(Project *p) {
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "autotest-assist");
    p->cleanup_count = 0;
    load_test_registry(p);
}

static void init_app(App *app) {
    memset(app, 0, sizeof(*app));
    init_project(&app->project);
    app->screen = SCREEN_DASHBOARD;
    set_status(app, "Ready.");
}

static void draw_box(int y, int x, int h, int w, const char *title) {
    mvhline(y, x + 1, ACS_HLINE, w - 2);
    mvhline(y + h - 1, x + 1, ACS_HLINE, w - 2);
    mvvline(y + 1, x, ACS_VLINE, h - 2);
    mvvline(y + 1, x + w - 1, ACS_VLINE, h - 2);
    mvaddch(y, x, ACS_ULCORNER);
    mvaddch(y, x + w - 1, ACS_URCORNER);
    mvaddch(y + h - 1, x, ACS_LLCORNER);
    mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);
    if (title && *title) mvprintw(y, x + 2, " %.*s ", w - 6, title);
}

static void print_clip(int y, int x, int w, const char *text) {
    char buf[LONG_LEN];
    snprintf(buf, sizeof(buf), "%s", text ? text : "");
    if ((int)strlen(buf) > w) {
        if (w > 3) {
            buf[w - 3] = '.';
            buf[w - 2] = '.';
            buf[w - 1] = '.';
            buf[w] = '\0';
        } else if (w >= 0) {
            buf[w] = '\0';
        }
    }
    mvprintw(y, x, "%-*s", w, buf);
}

static int draw_wrapped_text(int y, int x, int w, int max_rows, const char *text) {
    int row = 0;
    const char *p = text && text[0] ? text : "<none>";
    while (row < max_rows && p && *p) {
        int used = 0;
        int bytes = 0;
        const char *line = p;
        while (line[bytes] && line[bytes] != '\n' && used < w) {
            int char_len = utf8_char_len_at(line, bytes);
            int char_w = utf8_char_width_at(line, bytes);
            if (used + char_w > w) break;
            used += char_w;
            bytes += char_len;
        }
        char buf[LONG_LEN];
        if (bytes >= (int)sizeof(buf)) bytes = (int)sizeof(buf) - 1;
        memcpy(buf, line, (size_t)bytes);
        buf[bytes] = '\0';
        print_editor_line(y + row, x, w, buf);
        row++;
        p = line + bytes;
        if (*p == '\n') p++;
    }
    while (row < max_rows) {
        print_editor_line(y + row, x, w, "");
        row++;
    }
    return row;
}

static void draw_header(const App *app, int width) {
    attron(A_REVERSE);
    mvprintw(0, 0, "%-*s", width, " AutoTest Builder - create tests, select tests, start automated test");
    attroff(A_REVERSE);
    mvprintw(1, 1, "Tests:%d  selected:%d  reboot:%s  cleanup:auto  saved-paths:%d",
             app->project.case_count,
             selected_count(&app->project),
             has_reboot(&app->project) ? "yes" : "no",
             selected_count(&app->project));
}

static void draw_status(const App *app, int y, int width) {
    attron(A_REVERSE);
    mvprintw(y, 0, "%-*s", width, app->status);
    attroff(A_REVERSE);
}

static void draw_menu(int y, int x, int w, const char **items, int count, int selected) {
    for (int row = 0; row < 4; row++) {
        mvhline(y + row, x, ' ', w);
    }
    mvprintw(y, x, "Action Menu");
    int cx = x;
    for (int i = 0; i < count; i++) {
        char item[TEXT_LEN];
        snprintf(item, sizeof(item), " %s ", items[i]);
        if (cx + (int)strlen(item) >= x + w) {
            y++;
            cx = x;
        }
        if (i == selected) attron(A_REVERSE);
        mvprintw(y + 1, cx, "%s", item);
        if (i == selected) attroff(A_REVERSE);
        cx += (int)strlen(item) + 1;
    }
}

static void draw_cases(const App *app, int y, int x, int h, int w) {
    char title[TEXT_LEN];
    int filtered = filtered_case_count(app);
    if (app->case_filter[0]) {
        snprintf(title, sizeof(title), "Test Cases filtered:%d/%d", filtered, app->project.case_count);
    } else {
        snprintf(title, sizeof(title), "Test Cases");
    }
    draw_box(y, x, h, w, title);
    int visible = h - 2;
    int row = 0;
    for (int i = 0; i < app->project.case_count && row < visible; i++) {
        const TestCase *tc = &app->project.cases[i];
        if (!case_matches_filter(tc, app->case_filter)) continue;
        if (i == app->selected_case) attron(A_REVERSE);
        mvprintw(y + 1 + row, x + 1, "%c %-5s %-22.22s %-7s %s",
                 tc->selected ? '*' : ' ',
                 tc->id,
                 tc->title,
                 command_kind_name(tc->kind),
                 tc->script_path[0] ? "saved" : "not_saved");
        if (i == app->selected_case) attroff(A_REVERSE);
        row++;
    }
    if (app->project.case_count == 0) {
        mvprintw(y + 1, x + 1, "No test cases. Select Add test case.");
    } else if (filtered == 0) {
        mvprintw(y + 1, x + 1, "No matching tests. Clear or change filter.");
    }
}

static void draw_case_details(const App *app, int y, int x, int h, int w) {
    draw_box(y, x, h, w, "Details");
    if (app->project.case_count == 0) return;
    if (app->case_filter[0] && filtered_case_count(app) == 0) {
        int filter_w = w - 31;
        if (filter_w < 1) filter_w = 1;
        mvprintw(y + 1, x + 1, "No matching test for filter: %.*s", filter_w, app->case_filter);
        return;
    }
    const TestCase *tc = &app->project.cases[app->selected_case];
    int check_count = command_check_count(test_command(tc));
    int line = y + 1;
    mvprintw(line++, x + 1, "id: %s", tc->id);
    mvprintw(line++, x + 1, "title: %.*s", w - 9, tc->title);
    mvprintw(line++, x + 1, "kind: %s  selected: %s", command_kind_name(tc->kind), tc->selected ? "yes" : "no");
    mvprintw(line++, x + 1, "expected_exit: %d", tc->expected_exit);
    mvprintw(line++, x + 1, "checks: %d from @check lines", check_count);
    mvprintw(line++, x + 1, "@check <var> <match> <expected>");
    mvprintw(line++, x + 3, "exact: regex full match");
    mvprintw(line++, x + 3, "not_exact: regex must not full match");
    mvprintw(line++, x + 3, "contains: regex search");
    mvprintw(line++, x + 3, "not_contains: regex must not match");
    mvprintw(line++, x + 3, "empty: no text  not_empty: any text");
    mvprintw(line++, x + 1, "cleanup: auto %s", tc->cleanup[0] ? "generated" : "none");
    mvprintw(line++, x + 1, "reboot: auto %s", tc->kind == CMD_REBOOT ? "detected" : "none");
    if (line < y + h - 1) mvprintw(line++, x + 1, "path: %.*s", w - 8, tc->script_path[0] ? tc->script_path : "<not saved>");
    if (line < y + h - 1) mvprintw(line++, x + 1, "description:");
    int desc_rows = y + h - 1 - line;
    if (desc_rows > 0) draw_wrapped_text(line, x + 1, w - 3, desc_rows, tc->description);
}

static void draw_dashboard(const App *app, int height, int width) {
    (void)height;
    draw_header(app, width);
    int top = 3;
    int left_w = width / 2 - 1;
    draw_box(top, 0, 11, left_w, "Tests");
    attron(A_REVERSE);
    mvprintw(top + 1, 1, "%-*s", left_w - 2, "Create, select, and start tests");
    attroff(A_REVERSE);
    mvprintw(top + 2, 1, "Create test opens the built-in editor.");
    mvprintw(top + 3, 1, "Selected tests are used for Start.");
    draw_box(top, left_w + 1, 11, width - left_w - 1, "Summary");
    mvprintw(top + 1, left_w + 2, "generated: current directory");
    mvprintw(top + 2, left_w + 2, "reboot tests: %s", has_reboot(&app->project) ? "yes" : "no");
    mvprintw(top + 3, left_w + 2, "cleanup script: %s", app->project.cleanup_count ? "yes" : "no");
    mvprintw(top + 4, left_w + 2, "result: OK / NG, final exit 0 / 1");
    static const char *menu[] = {"Open test list", "Create test", "Start selected tests", "Help"};
    draw_menu(15, 1, width - 2, menu, 4, app->selected_menu);
}

static void draw_editor(const App *app, int height, int width) {
    draw_header(app, width);
    int top = 3;
    int bottom = height - 5;
    int left_w = width / 2;
    draw_cases(app, top, 0, bottom - top, left_w);
    draw_case_details(app, top, left_w + 1, bottom - top, width - left_w - 1);
    draw_box(bottom, 0, 4, width, "Selection / Warnings");
    if (app->case_filter[0]) {
        int filter_w = width - 42;
        if (filter_w < 1) filter_w = 1;
        mvprintw(bottom + 1, 1, "filter:%.*s  matches:%d/%d  selected:%d",
                 filter_w, app->case_filter,
                 filtered_case_count(app), app->project.case_count,
                 selected_count(&app->project));
    } else {
        mvprintw(bottom + 1, 1, "shell:bash  final NG exit:1  selected:%d  cleanup/reboot:auto",
                 selected_count(&app->project));
    }
    if (has_reboot(&app->project)) {
        mvprintw(bottom + 2, 1, "selected reboot tests may interrupt sequential execution");
    }
    static const char *menu[] = {"Edit test", "Edit description", "Rename test", "Copy test", "Select test", "Delete test", "Print script", "Filter", "Start selected tests", "Back"};
    draw_menu(height - 4, 1, width - 2, menu, 10, app->selected_menu);
}

static void draw_form(const App *app, int height, int width) {
    draw_header(app, width);
    draw_box(3, 0, height - 7, width, "Edit Test Case");
    if (app->project.case_count == 0) return;
    const TestCase *tc = &app->project.cases[app->selected_case];
    const CheckRule *check = primary_check_const(tc);
    const char *labels[] = {"id", "title", "selected", "kind", "command", "expected_exit", "check_var", "check_match", "check_expected", "cleanup", "timeout_sec", "requires_root", "script_path"};
    char values[13][LONG_LEN];
    snprintf(values[0], sizeof(values[0]), "%s", tc->id);
    snprintf(values[1], sizeof(values[1]), "%s", tc->title);
    snprintf(values[2], sizeof(values[2]), "%s", tc->selected ? "yes" : "no");
    snprintf(values[3], sizeof(values[3]), "%s", command_kind_name(tc->kind));
    snprintf(values[4], sizeof(values[4]), "%s", test_command(tc));
    snprintf(values[5], sizeof(values[5]), "%d", tc->expected_exit);
    snprintf(values[6], sizeof(values[6]), "%s", check ? check->var : "AUTOTEST_ACTUAL");
    snprintf(values[7], sizeof(values[7]), "%s", match_name(check ? check->match : MATCH_NONE));
    summarize_multiline(values[8], sizeof(values[8]), check ? check->expected : "");
    snprintf(values[9], sizeof(values[9]), "%s", tc->cleanup);
    snprintf(values[10], sizeof(values[10]), "%d", tc->timeout_sec);
    snprintf(values[11], sizeof(values[11]), "%s", tc->requires_root ? "yes" : "no");
    snprintf(values[12], sizeof(values[12]), "%s", tc->script_path[0] ? tc->script_path : "<not saved>");
    for (int i = 0; i < 13; i++) {
        if (i == app->selected_field) attron(A_REVERSE);
        mvprintw(4 + i, 2, "%-16s", labels[i]);
        print_clip(4 + i, 20, width - 23, values[i]);
        if (i == app->selected_field) attroff(A_REVERSE);
    }
    mvprintw(height - 9, 2, "Result rule: OK when expected_exit and configured variable check pass.");
    static const char *menu[] = {"Edit value", "Change option", "Insert shell", "Insert reboot", "Insert vim", "Save test", "Cancel"};
    draw_menu(height - 4, 1, width - 2, menu, 7, app->selected_menu);
}

static void draw_match(const App *app, int height, int width) {
    draw_header(app, width);
    draw_box(3, 0, height - 7, width, "@check Reference");
    if (app->project.case_count == 0) return;
    const TestCase *tc = &app->project.cases[app->selected_case];
    int check_count = command_check_count(test_command(tc));
    mvprintw(5, 2, "checks: %d from script @check lines", check_count);
    mvprintw(6, 2, "@check <var> <match> <expected>");
    mvprintw(7, 4, "exact      expected regex must match the whole value");
    mvprintw(8, 4, "not_exact  expected regex must not match the whole value");
    mvprintw(9, 4, "contains   expected regex must match somewhere");
    mvprintw(10, 4, "not_contains expected regex must not match anywhere");
    mvprintw(11, 4, "empty      variable value must be empty");
    mvprintw(12, 4, "not_empty  variable value must not be empty");
    mvprintw(14, 4, "none       no variable check");
    static const char *menu[] = {"Back"};
    draw_menu(height - 4, 1, width - 2, menu, 1, app->selected_menu);
}

static void draw_cleanup(const App *app, int height, int width) {
    draw_header(app, width);
    draw_box(3, 0, height - 7, width, "Cleanup Builder");
    for (int i = 0; i < app->project.cleanup_count && i < height - 11; i++) {
        if (i == app->selected_cleanup) attron(A_REVERSE);
        mvprintw(4 + i, 2, "%02d  %.*s", i + 1, width - 8, app->project.cleanups[i]);
        if (i == app->selected_cleanup) attroff(A_REVERSE);
    }
    if (app->project.cleanup_count == 0) mvprintw(4, 2, "No cleanup commands.");
    mvprintw(height - 8, 2, "Generated scripts use trap cleanup EXIT and a cleanup-only script.");
    static const char *menu[] = {"Add command", "Edit command", "Delete command", "Preview", "Save", "Back"};
    draw_menu(height - 4, 1, width - 2, menu, 6, app->selected_menu);
}

static void draw_reboot(const App *app, int height, int width) {
    draw_header(app, width);
    draw_box(3, 0, height - 7, width, "Reboot Handling");
    mvprintw(5, 2, "There is no after-reboot category.");
    mvprintw(6, 2, "A reboot test is just a saved test script that may reboot the machine.");
    mvprintw(8, 2, "Selected tests are executed sequentially by their saved paths.");
    mvprintw(9, 2, "If a selected test reboots the machine, the sequence is naturally interrupted.");
    mvprintw(11, 2, "Create a follow-up test and run it after reboot when needed.");
    static const char *menu[] = {"Start selected tests", "Back"};
    draw_menu(height - 4, 1, width - 2, menu, 2, app->selected_menu);
}

static void draw_editor_help(const App *app, int height, int width) {
    int box_w = width - 10;
    if (box_w > 92) box_w = 92;
    if (box_w < 40) box_w = width - 2;
    int box_h = EDITOR_HELP_LINE_COUNT + 4;
    if (box_h > height - 4) box_h = height - 4;
    if (box_h < 8) box_h = 8;
    int y = (height - box_h) / 2;
    int x = (width - box_w) / 2;
    if (x < 0) x = 0;
    for (int row = 0; row < box_h; row++) {
        mvhline(y + row, x, ' ', box_w);
    }
    draw_box(y, x, box_h, box_w, "Editor Help");
    int rows = box_h - 3;
    int max_scroll = EDITOR_HELP_LINE_COUNT > rows ? EDITOR_HELP_LINE_COUNT - rows : 0;
    int scroll = app->editor_help_scroll;
    if (scroll < 0) scroll = 0;
    if (scroll > max_scroll) scroll = max_scroll;
    for (int i = 0; i < rows; i++) {
        int line_index = scroll + i;
        if (line_index >= EDITOR_HELP_LINE_COUNT) {
            print_clip(y + 1 + i, x + 2, box_w - 4, "");
            continue;
        }
        const char *line = EDITOR_HELP_LINES[line_index];
        if (line[0] && line[0] != ' ' &&
            strcmp(line, EDITOR_HELP_LINES[0]) != 0 &&
            strchr(line, '<') == NULL &&
            strchr(line, '/') == NULL &&
            strchr(line, ':') == NULL) {
            attron(A_BOLD);
            print_clip(y + 1 + i, x + 2, box_w - 4, line);
            attroff(A_BOLD);
        } else {
            print_clip(y + 1 + i, x + 2, box_w - 4, line);
        }
    }
    if (max_scroll > 0) {
        mvprintw(y + box_h - 2, x + box_w - 18, " %2d/%2d ", scroll + 1, max_scroll + 1);
    }
}

static void draw_completion_window(const App *app, int height, int width, int first_row) {
    if (!app->completion_open || app->completion_count <= 0) return;
    int rows = app->completion_count;
    if (rows > 8) rows = 8;
    int box_h = rows + 2;
    int box_w = 24;
    for (int i = 0; i < app->completion_count && i < rows; i++) {
        int item_w = editor_visual_col(app->completion_items[i], (int)strlen(app->completion_items[i])) + 4;
        if (item_w > box_w) box_w = item_w;
    }
    if (box_w > 48) box_w = 48;
    if (box_w > width - 2) box_w = width - 2;
    if (box_h > height - 6) box_h = height - 6;
    if (box_w < 12 || box_h < 3) return;

    int wrap_w = width - 9;
    if (wrap_w < 1) wrap_w = 1;
    int cursor_visual = editor_visual_col(app->editor_lines[app->editor_row], app->editor_col);
    int cursor_y = 6 + editor_cursor_screen_row_from_first(app, first_row, wrap_w);
    int cursor_x = 7 + (cursor_visual % wrap_w);
    int y = cursor_y + 1;
    int x = cursor_x;
    if (y + box_h > height - 4) y = cursor_y - box_h;
    if (y < 4) y = 4;
    if (x + box_w > width - 1) x = width - box_w - 1;
    if (x < 1) x = 1;

    for (int row = 0; row < box_h; row++) {
        mvhline(y + row, x, ' ', box_w);
    }
    for (int i = 0; i < app->completion_count && i < rows; i++) {
        if (i == app->completion_selected) attron(A_REVERSE);
        print_clip(y + 1 + i, x + 1, box_w - 2, app->completion_items[i]);
        if (i == app->completion_selected) attroff(A_REVERSE);
    }
    mvaddch(y, x, ACS_ULCORNER);
    mvhline(y, x + 1, ACS_HLINE, box_w - 2);
    mvaddch(y, x + box_w - 1, ACS_URCORNER);
    for (int row = 1; row < box_h - 1; row++) {
        mvaddch(y + row, x, ACS_VLINE);
        mvaddch(y + row, x + box_w - 1, ACS_VLINE);
    }
    mvaddch(y + box_h - 1, x, ACS_LLCORNER);
    mvhline(y + box_h - 1, x + 1, ACS_HLINE, box_w - 2);
    mvaddch(y + box_h - 1, x + box_w - 1, ACS_LRCORNER);
}

static bool editor_cursor_position(const App *app, int height, int width, int *out_y, int *out_x) {
    if (!app || app->screen != SCREEN_SCRIPT_EDITOR || !app->editor_insert) return false;
    int rows = editor_visible_rows(app, height);
    int wrap_w = width - 9;
    if (wrap_w < 1) wrap_w = 1;
    int first_row = app->editor_first_row;
    if (first_row < 0) first_row = 0;
    if (first_row >= app->editor_line_count) first_row = app->editor_line_count - 1;
    int cy = 6 + editor_cursor_screen_row_from_first(app, first_row, wrap_w);
    int cursor_visual = editor_visual_col(app->editor_lines[app->editor_row], app->editor_col);
    int cx = 7 + (cursor_visual % wrap_w);
    if (cy < 6 || cy >= 6 + rows || cy > height - 4 || cx < 7 || cx >= width - 1) return false;
    if (out_y) *out_y = cy;
    if (out_x) *out_x = cx;
    return true;
}

static void draw_script_editor(const App *app, int height, int width) {
    draw_header(app, width);
    char title[TEXT_LEN];
    snprintf(title, sizeof(title), "Text Editor: %s", editor_target_name(app->editor_target));
    draw_box(3, 0, height - 5, width, title);
    if (app->project.case_count > 0) {
        const TestCase *tc = &app->project.cases[app->selected_case];
        mvprintw(4, 2, "%s %s", tc->id, tc->title);
    }
    bool regex_templates = editor_uses_regex_templates(app);
    int rows = editor_visible_rows(app, height);
    int wrap_w = width - 9;
    if (wrap_w < 1) wrap_w = 1;
    int first_row = app->editor_first_row;
    if (first_row < 0) first_row = 0;
    if (first_row >= app->editor_line_count) first_row = app->editor_line_count - 1;
    int first_wrap = app->editor_first_wrap_row;
    int screen_row = 0;
    for (int line_index = first_row; line_index < app->editor_line_count && screen_row < rows; line_index++) {
        int wrapped_rows = editor_wrapped_rows_for_line(app, line_index, wrap_w);
        int start_wrap = line_index == first_row ? first_wrap : 0;
        if (start_wrap < 0) start_wrap = 0;
        if (start_wrap >= wrapped_rows) start_wrap = wrapped_rows - 1;
        for (int wrap_row = start_wrap; wrap_row < wrapped_rows && screen_row < rows; wrap_row++) {
            int y = 6 + screen_row;
            if (wrap_row == 0) {
                mvprintw(y, 2, "%3d ", line_index + 1);
            } else {
                mvprintw(y, 2, "    ");
            }
            int start_visual = wrap_row * wrap_w;
            print_editor_line_segment(y, 7, wrap_w, app->editor_lines[line_index], start_visual);
            if (line_index == app->editor_row) {
                draw_editor_cursor(app, y, 7, wrap_w, app->editor_lines[line_index], start_visual);
            }
            screen_row++;
        }
    }
    while (screen_row < rows) {
        int y = 6 + screen_row;
        mvhline(y, 1, ' ', width - 2);
        screen_row++;
    }
    if (app->editor_insert) {
        mvhline(height - 4, 1, ' ', width - 2);
        mvprintw(height - 4, 2, "-- INSERT --");
    } else if (app->editor_command_mode) {
        mvhline(height - 4, 1, ' ', width - 2);
        mvprintw(height - 4, 2, ":%s", app->editor_command);
    } else if (app->editor_search_mode) {
        mvhline(height - 4, 1, ' ', width - 2);
        mvprintw(height - 4, 2, "/%s", app->editor_search);
    }
    if (regex_templates && app->regex_template_list_open) {
        int box_h = REGEX_TEMPLATE_COUNT + 4;
        int box_w = 58;
        if (box_w > width - 8) box_w = width - 8;
        if (box_h > height - 6) box_h = height - 6;
        int y = (height - box_h) / 2;
        int x = (width - box_w) / 2;
        for (int row = 0; row < box_h; row++) {
            mvhline(y + row, x, ' ', box_w);
        }
        draw_box(y, x, box_h, box_w, "Regex Templates");
        mvprintw(y + 1, x + 2, "Tab/Up/Down select  Enter insert  Esc close");
        int list_rows = box_h - 3;
        for (int i = 0; i < REGEX_TEMPLATE_COUNT && i < list_rows; i++) {
            if (i == app->selected_menu) attron(A_REVERSE);
            mvprintw(y + 2 + i, x + 2, "%-12s %.*s",
                     REGEX_TEMPLATES[i].label,
                     box_w - 18,
                     REGEX_TEMPLATES[i].text);
            if (i == app->selected_menu) attroff(A_REVERSE);
        }
    }
    if (app->editor_help_open) {
        draw_editor_help(app, height, width);
    }
    if (app->completion_open) {
        draw_completion_window(app, height, width, first_row);
    }
    if (app->editor_insert) {
        int cy = 0;
        int cx = 0;
        if (editor_cursor_position(app, height, width, &cy, &cx)) move(cy, cx);
    }
}

static void preview_add(App *app, const char *line) {
    if (app->preview_count < PREVIEW_LINES) {
        snprintf(app->preview[app->preview_count++], LONG_LEN, "%s", line);
    }
}

static void preview_add_comment_block(App *app, const char *indent, const char *label, const char *text) {
    char line[LONG_LEN];
    snprintf(line, sizeof(line), "%s# %s:", indent, label);
    preview_add(app, line);
    if (!text || !text[0]) {
        snprintf(line, sizeof(line), "%s#   <none>", indent);
        preview_add(app, line);
        return;
    }
    const char *p = text;
    while (p && *p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len > sizeof(line) - strlen(indent) - 6) len = sizeof(line) - strlen(indent) - 6;
        snprintf(line, sizeof(line), "%s#   %.*s", indent, (int)len, p);
        preview_add(app, line);
        p = end ? end + 1 : NULL;
    }
}

static void preview_script(App *app) {
    app->preview_count = 0;
    app->preview_scroll = 0;
    preview_add(app, "#!/usr/bin/env bash");
    preview_add(app, "set -u");
    preview_add(app, "OK_COUNT=0");
    preview_add(app, "NG_COUNT=0");
    preview_add(app, "WORK_DIR=\"${TMPDIR:-/tmp}/autotest-script-builder.$$\"");
    preview_add(app, "mkdir -p \"$WORK_DIR\"");
    preview_add(app, "cleanup() {");
    for (int i = 0; i < app->project.cleanup_count; i++) {
        char line[LONG_LEN];
        snprintf(line, sizeof(line), "  %s", app->project.cleanups[i]);
        preview_add(app, line);
    }
    preview_add(app, "}");
    preview_add(app, "trap cleanup EXIT");
    preview_add(app, "");
    for (int i = 0; i < app->project.case_count; i++) {
        if (!include_case(&app->project, i, app->selected_only)) continue;
        const TestCase *tc = &app->project.cases[i];
        char line[LONG_LEN];
        snprintf(line, sizeof(line), "run_case_%03d() {", i + 1);
        preview_add(app, line);
        preview_add_comment_block(app, "  ", "Title", tc->title);
        preview_add_comment_block(app, "  ", "Description", tc->description);
        if (tc->kind == CMD_REBOOT) {
            preview_add(app, "  # split script body into pre-reboot and post-reboot phases");
            preview_add(app, "  # install a one-shot systemd resume unit, then run reboot command");
            preview_add(app, "  # after boot: run post phase, verify result, cleanup");
        } else {
            snprintf(line, sizeof(line), "  bash -c '%s' >\"$WORK_DIR/case_%03d.stdout\" 2>\"$WORK_DIR/case_%03d.stderr\"", test_command(tc), i + 1, i + 1);
            preview_add(app, line);
            preview_add(app, "  actual_exit=$?");
            snprintf(line, sizeof(line), "  # OK when actual_exit == %d and variable check passes", tc->expected_exit);
            preview_add(app, line);
        }
        snprintf(line, sizeof(line), "  echo \"[OK/NG] %s %s\"", tc->id, tc->title);
        preview_add(app, line);
        preview_add(app, "}");
        preview_add(app, "");
    }
}

static void draw_preview(App *app, int height, int width) {
    if (app->preview_count == 0) preview_script(app);
    draw_header(app, width);
    draw_box(3, 0, height - 7, width, "Script Preview");
    int rows = height - 10;
    for (int i = 0; i < rows && app->preview_scroll + i < app->preview_count; i++) {
        mvprintw(4 + i, 2, "%03d ", app->preview_scroll + i + 1);
        print_clip(4 + i, 7, width - 9, app->preview[app->preview_scroll + i]);
    }
    static const char *menu[] = {"Preview body", "Saved test scripts", "Cleanup script", "README", "Save selected scripts", "Back"};
    draw_menu(height - 4, 1, width - 2, menu, 6, app->selected_menu);
}

static bool selected_result_path(char *out, size_t out_sz) {
    char cwd[LONG_LEN];
    if (!getcwd(cwd, sizeof(cwd))) return false;
    if (strlen(cwd) + strlen("/autotest_selected.result") + 1 > out_sz) return false;
    snprintf(out, out_sz, "%s/autotest_selected.result", cwd);
    return true;
}

static void draw_result(const App *app, int height, int width) {
    draw_header(app, width);
    draw_box(3, 0, height - 7, width, "Selected Test Results");
    int y = 5;
    char result_path[LONG_LEN];
    if (selected_result_path(result_path, sizeof(result_path))) {
        mvprintw(y++, 2, "result: %.*s", width - 12, result_path);
        FILE *f = fopen(result_path, "r");
        if (f) {
            char line[LONG_LEN];
            while (y < height - 8 && fgets(line, sizeof(line), f)) {
                size_t len = strlen(line);
                while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
                print_clip(y++, 2, width - 4, line);
            }
            fclose(f);
        } else {
            mvprintw(y++, 2, "No selected test result has been written yet.");
            for (int i = 0; i < app->project.case_count && y < height - 8; i++) {
                const TestCase *tc = &app->project.cases[i];
                if (!tc->selected) continue;
                mvprintw(y++, 2, "[SAVED] %s %s", tc->id, tc->title);
                if (tc->script_path[0] && y < height - 8) print_clip(y++, 6, width - 8, tc->script_path);
            }
        }
    } else {
        mvprintw(y++, 2, "Failed to resolve selected result path.");
    }
    mvprintw(height - 8, 2, "Start selected tests writes one aggregate result file in the current directory.");
    static const char *menu[] = {"Back to tests", "Dashboard"};
    draw_menu(height - 4, 1, width - 2, menu, 2, app->selected_menu);
}

static void draw_confirm(const App *app, int height, int width) {
    draw_header(app, width);
    draw_box(4, 4, height - 9, width - 8,
             app->confirm_action == CONFIRM_DELETE ? "Confirm Delete Test" : "Confirm Script Generation");
    if (app->confirm_action == CONFIRM_DELETE) {
        if (app->project.case_count == 0) {
            mvprintw(6, 6, "No test case to delete.");
        } else {
            const TestCase *tc = &app->project.cases[app->selected_case];
            mvprintw(6, 6, "Delete this test case?");
            mvprintw(8, 8, "title: %.*s", width - 18, tc->title);
            mvprintw(9, 8, "script: %.*s", width - 19, tc->script_path[0] ? tc->script_path : "<not saved>");
            mvprintw(11, 6, "The registry entry, script file, and result file will be removed.");
        }
        mvprintw(height - 9, 6, "Delete selected test?");
    } else {
        mvprintw(6, 6, "Selected test scripts may contain risky commands.");
        if (app->run_after_generate) mvprintw(7, 8, "- choose Stop on NG before starting");
        if (has_reboot(&app->project)) mvprintw(8, 8, "- reboot");
        for (int i = 0; i < app->project.cleanup_count && i < 4; i++) {
            mvprintw(9 + i, 8, "- cleanup: %.*s", width - 22, app->project.cleanups[i]);
        }
        mvprintw(height - 9, 6, app->run_after_generate ? "Start selected tests?" : "Save selected test scripts?");
    }
    if (app->confirm_action == CONFIRM_GENERATE && app->run_after_generate) {
        char stop_label[32];
        snprintf(stop_label, sizeof(stop_label), "Stop on NG: %s", app->stop_on_failure ? "yes" : "no");
        const char *menu[] = {"Start", stop_label, "Back"};
        draw_menu(height - 7, 6, width - 12, menu, 3, app->selected_menu);
    } else {
        static const char *menu[] = {"Continue", "Back"};
        draw_menu(height - 7, 6, width - 12, menu, 2, app->selected_menu);
    }
}

static void draw_app(App *app) {
    int height, width;
    getmaxyx(stdscr, height, width);
    erase();
    if (height < 20 || width < 70) {
        mvprintw(0, 0, "Terminal too small. Need at least 70x20.");
        refresh();
        return;
    }
    if (app->screen != SCREEN_SCRIPT_EDITOR || !app->editor_insert) curs_set(0);
    switch (app->screen) {
    case SCREEN_DASHBOARD: draw_dashboard(app, height, width); break;
    case SCREEN_EDITOR: draw_editor(app, height, width); break;
    case SCREEN_FORM: draw_form(app, height, width); break;
    case SCREEN_MATCH: draw_match(app, height, width); break;
    case SCREEN_CLEANUP: draw_cleanup(app, height, width); break;
    case SCREEN_REBOOT: draw_reboot(app, height, width); break;
    case SCREEN_SCRIPT_EDITOR: draw_script_editor(app, height, width); break;
    case SCREEN_PREVIEW: draw_preview(app, height, width); break;
    case SCREEN_RESULT: draw_result(app, height, width); break;
    case SCREEN_CONFIRM: draw_confirm(app, height, width); break;
    }
    draw_status(app, height - 1, width);
    if (app->screen == SCREEN_SCRIPT_EDITOR && app->editor_insert) {
        int cy = 0;
        int cx = 0;
        if (editor_cursor_position(app, height, width, &cy, &cx)) {
            curs_set(1);
            move(cy, cx);
        }
    }
    refresh();
}

static bool prompt_text(const char *label, char *buf, size_t buf_sz) {
    if (buf_sz == 0) return false;
    noecho();
    curs_set(1);
    int h, w;
    getmaxyx(stdscr, h, w);
    int y = h / 2 - 2;
    int x = 4;
    int box_h = 5;
    int box_w = w - 8;
    for (int row = 0; row < box_h; row++) {
        mvhline(y + row, x, ' ', box_w);
    }
    draw_box(y, x, box_h, box_w, label);
    char tmp[LONG_LEN];
    snprintf(tmp, sizeof(tmp), "%s", buf);
    size_t max_len = buf_sz - 1;
    if (max_len >= sizeof(tmp)) max_len = sizeof(tmp) - 1;
    tmp[max_len] = '\0';
    int len = (int)strlen(tmp);
    int col = len;
    bool accepted = false;
    for (;;) {
        int input_w = w - 12;
        if (input_w < 1) input_w = 1;
        int start = 0;
        if (col >= input_w) start = col - input_w + 1;
        mvhline(h / 2, 5, ' ', w - 10);
        mvprintw(h / 2, 6, "> ");
        mvprintw(h / 2, 8, "%.*s", input_w, tmp + start);
        move(h / 2, 8 + col - start);
        int ch = read_tui_input();
        if (ch == 27) {
            accepted = false;
            break;
        }
        if (ch == '\n' || ch == KEY_ENTER) {
            accepted = true;
            break;
        }
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (col > 0) {
                int prev = utf8_prev_index(tmp, col);
                memmove(tmp + prev, tmp + col, (size_t)(len - col + 1));
                len -= col - prev;
                col = prev;
            }
        } else if (ch == KEY_LEFT) {
            if (col > 0) col = utf8_prev_index(tmp, col);
        } else if (ch == KEY_RIGHT) {
            if (col < len) col = utf8_next_index(tmp, col);
        } else if (ch == KEY_HOME) {
            col = 0;
        } else if (ch == KEY_END) {
            col = len;
        } else if (editor_is_text_char(ch)) {
            char bytes[MB_LEN_MAX];
            int byte_count = 0;
            if (ch >= 0 && ch < 0x80) {
                bytes[0] = (char)ch;
                byte_count = 1;
            } else {
                mbstate_t state;
                memset(&state, 0, sizeof(state));
                size_t converted = wcrtomb(bytes, (wchar_t)ch, &state);
                if (converted != (size_t)-1 && converted > 0) byte_count = (int)converted;
            }
            if (byte_count > 0 && len + byte_count <= (int)max_len) {
                memmove(tmp + col + byte_count, tmp + col, (size_t)(len - col + 1));
                memcpy(tmp + col, bytes, (size_t)byte_count);
                col += byte_count;
                len += byte_count;
            }
        }
    }
    curs_set(0);
    if (!accepted) return false;
    snprintf(buf, buf_sz, "%s", tmp);
    return true;
}

static int prompt_int(const char *label, int current) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%d", current);
    if (!prompt_text(label, buf, sizeof(buf))) return current;
    return atoi(buf);
}

static void cycle_match(MatchType *m) {
    *m = (MatchType)((*m + 1) % (MATCH_NOT_EMPTY + 1));
}

static void cycle_kind(TestCase *tc) {
    tc->kind = (CommandKind)((tc->kind + 1) % 3);
}

static void quote_for_printf(char *dst, size_t dst_sz, const char *src) {
    size_t j = 0;
    if (dst_sz == 0) return;
    for (size_t i = 0; src && src[i] && j + 2 < dst_sz; i++) {
        if (src[i] == '\'') {
            if (j + 4 >= dst_sz) break;
            dst[j++] = '\'';
            dst[j++] = '\\';
            dst[j++] = '\'';
            dst[j++] = '\'';
        } else if (src[i] == '\\') {
            if (j + 2 >= dst_sz) break;
            dst[j++] = '\\';
            dst[j++] = '\\';
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

static void set_reboot_command(TestCase *tc) {
    tc->kind = CMD_REBOOT;
    tc->expected_exit = 0;
    tc->check_count = 0;
    memset(tc->checks, 0, sizeof(tc->checks));
    set_test_command(tc, "__AUTOTEST_REBOOT__");
}

static void set_vim_command(TestCase *tc) {
    char path[TEXT_LEN] = "/tmp/testfile";
    char text[TEXT_LEN] = "new text";
    prompt_text("vim target file", path, sizeof(path));
    prompt_text("text inserted by vim", text, sizeof(text));
    char qpath[TEXT_LEN];
    char qtext[TEXT_LEN * 2];
    quote_for_printf(qpath, sizeof(qpath), path);
    quote_for_printf(qtext, sizeof(qtext), text);
    char command[LONG_LEN];
    snprintf(command, sizeof(command),
             "{ sleep 0.3; printf 'gg'; printf 'dG'; printf 'i'; printf '%s\\n'; printf '\\033'; printf ':wq\\r'; } | script -q -c \"vim -Nu NONE -n '%s'\" /dev/null >/dev/null 2>&1",
             qtext, qpath);
    set_test_command(tc, command);
    tc->kind = CMD_VIM;
    tc->expected_exit = 0;
    tc->check_count = 0;
    memset(tc->checks, 0, sizeof(tc->checks));
}

static void editor_save_undo(App *app) {
    app->editor_dirty = true;
    if (app->editor_undo_count >= EDITOR_UNDO_DEPTH) {
        editor_free_undo_slot(app, 0);
        memmove(app->editor_undo_lines, app->editor_undo_lines + 1, sizeof(app->editor_undo_lines[0]) * (EDITOR_UNDO_DEPTH - 1));
        memmove(app->editor_undo_line_count, app->editor_undo_line_count + 1, sizeof(app->editor_undo_line_count[0]) * (EDITOR_UNDO_DEPTH - 1));
        memmove(app->editor_undo_row, app->editor_undo_row + 1, sizeof(app->editor_undo_row[0]) * (EDITOR_UNDO_DEPTH - 1));
        memmove(app->editor_undo_col, app->editor_undo_col + 1, sizeof(app->editor_undo_col[0]) * (EDITOR_UNDO_DEPTH - 1));
        memset(app->editor_undo_lines[EDITOR_UNDO_DEPTH - 1], 0, sizeof(app->editor_undo_lines[EDITOR_UNDO_DEPTH - 1]));
        app->editor_undo_count = EDITOR_UNDO_DEPTH - 1;
    }
    int slot = app->editor_undo_count++;
    editor_free_undo_slot(app, slot);
    for (int r = 0; r < app->editor_line_count; r++) {
        app->editor_undo_lines[slot][r] = dup_text_heap(app->editor_lines[r]);
        if (!app->editor_undo_lines[slot][r]) {
            editor_free_undo_slot(app, slot);
            app->editor_undo_count--;
            set_status(app, "Out of memory. Undo not saved.");
            return;
        }
    }
    app->editor_undo_line_count[slot] = app->editor_line_count;
    app->editor_undo_row[slot] = app->editor_row;
    app->editor_undo_col[slot] = app->editor_col;
}

static void editor_undo(App *app) {
    if (app->editor_undo_count <= 0) {
        set_status(app, "No undo available.");
        return;
    }
    int slot = --app->editor_undo_count;
    editor_free_all_lines(app);
    app->editor_line_count = app->editor_undo_line_count[slot];
    if (app->editor_line_count < 1) app->editor_line_count = 1;
    if (app->editor_line_count > EDITOR_LINES) app->editor_line_count = EDITOR_LINES;
    for (int r = 0; r < app->editor_line_count; r++) {
        app->editor_lines[r] = app->editor_undo_lines[slot][r];
        app->editor_undo_lines[slot][r] = NULL;
        app->editor_line_caps[r] = app->editor_lines[r] ? strlen(app->editor_lines[r]) + 1 : 0;
        if (!app->editor_lines[r]) editor_clear_line(app, r);
    }
    editor_free_undo_slot(app, slot);
    app->editor_row = app->editor_undo_row[slot];
    app->editor_col = app->editor_undo_col[slot];
    if (app->editor_row < 0) app->editor_row = 0;
    if (app->editor_row >= app->editor_line_count) app->editor_row = app->editor_line_count - 1;
    int len = (int)strlen(app->editor_lines[app->editor_row]);
    if (app->editor_col > len) app->editor_col = len;
    editor_update_desired_col(app);
    set_status(app, "Undo.");
}

static void editor_insert_char(App *app, int ch) {
    if (!editor_ensure_line_capacity(app, app->editor_row, 0)) return;
    char *line = app->editor_lines[app->editor_row];
    int len = (int)strlen(line);
    if (!editor_ensure_line_capacity(app, app->editor_row, (size_t)len + 1)) return;
    line = app->editor_lines[app->editor_row];
    editor_save_undo(app);
    if (app->editor_col < 0) app->editor_col = 0;
    if (app->editor_col > len) app->editor_col = len;
    memmove(line + app->editor_col + 1, line + app->editor_col, (size_t)(len - app->editor_col + 1));
    line[app->editor_col] = (char)ch;
    app->editor_col++;
    editor_update_desired_col(app);
}

static void editor_insert_bytes(App *app, const char *bytes, int byte_count) {
    if (!bytes || byte_count <= 0) return;
    if (!editor_ensure_line_capacity(app, app->editor_row, 0)) return;
    char *line = app->editor_lines[app->editor_row];
    int len = (int)strlen(line);
    if (!editor_ensure_line_capacity(app, app->editor_row, (size_t)len + (size_t)byte_count)) return;
    line = app->editor_lines[app->editor_row];
    editor_save_undo(app);
    if (app->editor_col < 0) app->editor_col = 0;
    if (app->editor_col > len) app->editor_col = len;
    memmove(line + app->editor_col + byte_count, line + app->editor_col,
            (size_t)(len - app->editor_col + 1));
    memcpy(line + app->editor_col, bytes, (size_t)byte_count);
    app->editor_col += byte_count;
    editor_update_desired_col(app);
}

static void editor_insert_wchar(App *app, wint_t ch) {
    if (ch < 0x80) {
        editor_insert_char(app, (int)ch);
        return;
    }
    char bytes[MB_LEN_MAX];
    mbstate_t state;
    memset(&state, 0, sizeof(state));
    size_t len = wcrtomb(bytes, (wchar_t)ch, &state);
    if (len == (size_t)-1 || len == 0 || len >= sizeof(bytes)) return;
    editor_insert_bytes(app, bytes, (int)len);
}

static bool append_wchar_utf8(char *dst, int *len, size_t dst_sz, wint_t ch) {
    if (!dst || !len || *len < 0) return false;
    if (ch < 0x80) {
        if ((size_t)*len + 1 >= dst_sz) return false;
        dst[(*len)++] = (char)ch;
        dst[*len] = '\0';
        return true;
    }
    char bytes[MB_LEN_MAX];
    mbstate_t state;
    memset(&state, 0, sizeof(state));
    size_t byte_count = wcrtomb(bytes, (wchar_t)ch, &state);
    if (byte_count == (size_t)-1 || byte_count == 0) return false;
    if ((size_t)*len + byte_count >= dst_sz) return false;
    memcpy(dst + *len, bytes, byte_count);
    *len += (int)byte_count;
    dst[*len] = '\0';
    return true;
}

static void editor_insert_text(App *app, const char *text) {
    for (size_t i = 0; text && text[i]; i++) {
        editor_insert_char(app, (unsigned char)text[i]);
    }
}

static const char *skip_spaces_const(const char *s) {
    while (s && (*s == ' ' || *s == '\t')) s++;
    return s ? s : "";
}

static bool token_equals(const char *s, const char *token) {
    s = skip_spaces_const(s);
    size_t len = strlen(token);
    return strncmp(s, token, len) == 0 &&
           (s[len] == '\0' || isspace((unsigned char)s[len]));
}

static int editor_current_token_start(const App *app) {
    const char *line = app->editor_lines[app->editor_row];
    int col = app->editor_col;
    int len = (int)strlen(line);
    if (col > len) col = len;
    while (col > 0 && !isspace((unsigned char)line[col - 1])) col--;
    return col;
}

static int editor_token_index_before_col(const char *line, int col) {
    int index = 0;
    bool in_token = false;
    for (int i = 0; line && line[i] && i < col; i++) {
        if (isspace((unsigned char)line[i])) {
            in_token = false;
        } else if (!in_token) {
            index++;
            in_token = true;
        }
    }
    return index;
}

static void editor_current_prefix(const App *app, int start_col, char *dst, size_t dst_sz) {
    const char *line = app->editor_lines[app->editor_row];
    int col = app->editor_col;
    int len = (int)strlen(line);
    if (col > len) col = len;
    if (start_col < 0) start_col = 0;
    if (start_col > col) start_col = col;
    size_t n = (size_t)(col - start_col);
    if (n >= dst_sz) n = dst_sz - 1;
    memcpy(dst, line + start_col, n);
    dst[n] = '\0';
}

static bool editor_inside_tui_block(const App *app) {
    bool inside = false;
    for (int r = 0; r < app->editor_row; r++) {
        const char *line = skip_spaces_const(app->editor_lines[r]);
        if (token_equals(line, "@tui")) inside = true;
        else if (token_equals(line, "@end")) inside = false;
    }
    if (inside && token_equals(app->editor_lines[app->editor_row], "@end")) return false;
    return inside;
}

static bool editor_seen_tui_before_current(const App *app) {
    for (int r = 0; r < app->editor_row; r++) {
        if (token_equals(app->editor_lines[r], "@tui")) return true;
    }
    return false;
}

static void completion_reset(App *app) {
    app->completion_open = false;
    app->completion_count = 0;
    app->completion_selected = 0;
    app->completion_start_col = 0;
    app->completion_end_col = 0;
}

static void completion_add(App *app, const char *item, const char *prefix) {
    if (app->completion_count >= COMPLETION_MAX) return;
    if (prefix && prefix[0] && strncmp(item, prefix, strlen(prefix)) != 0) return;
    copy_text(app->completion_items[app->completion_count], TEXT_LEN, item);
    app->completion_count++;
}

static bool editor_build_completion(App *app) {
    static const char *directives[] = {
        "@check ", "@assert ", "@backup ", "@restore ", "@tui ", "@reboot-if "
    };
    static const char *tui_commands[] = {
        "send ", "text ", "enter", "esc", "tab", "space", "sleep ", "ctrl ",
        "vim-clear", "vim-write-quit"
    };
    static const char *check_matches[] = {
        "exact ", "not_exact ", "contains ", "not_contains ", "empty", "not_empty"
    };
    static const char *shell_blocks[] = {
        "for ", "if "
    };
    static const char *tui_vars[] = {
        "AUTOTEST_TUI_STDOUT", "AUTOTEST_TUI_TEXT", "AUTOTEST_TUI_TRANSCRIPT",
        "AUTOTEST_TUI_STDERR", "AUTOTEST_TUI_STATUS", "AUTOTEST_TUI_STDOUT_FILE",
        "AUTOTEST_TUI_TEXT_FILE", "AUTOTEST_TUI_TRANSCRIPT_FILE",
        "AUTOTEST_TUI_INPUT_FILE", "AUTOTEST_TUI_STDERR_FILE"
    };

    completion_reset(app);
    if (app->editor_target != EDIT_TARGET_COMMAND) return false;

    const char *line = app->editor_lines[app->editor_row];
    int start_col = editor_current_token_start(app);
    int token_index = editor_token_index_before_col(line, start_col);
    char prefix[TEXT_LEN];
    editor_current_prefix(app, start_col, prefix, sizeof(prefix));
    if (prefix[0] == '\0') return false;

    app->completion_start_col = start_col;
    app->completion_end_col = app->editor_col;

    if (token_equals(line, "@check") && token_index == 2) {
        for (size_t i = 0; i < sizeof(check_matches) / sizeof(check_matches[0]); i++) {
            completion_add(app, check_matches[i], prefix);
        }
    } else if (editor_inside_tui_block(app)) {
        for (size_t i = 0; i < sizeof(tui_commands) / sizeof(tui_commands[0]); i++) {
            completion_add(app, tui_commands[i], prefix);
        }
    } else if (editor_seen_tui_before_current(app) && prefix[0] &&
               (strncmp(prefix, "AUTOTEST_TUI_", strlen("AUTOTEST_TUI_")) == 0 ||
                strncmp("AUTOTEST_TUI_", prefix, strlen(prefix)) == 0)) {
        for (size_t i = 0; i < sizeof(tui_vars) / sizeof(tui_vars[0]); i++) {
            completion_add(app, tui_vars[i], prefix);
        }
    } else {
        bool only_indent_before_token = true;
        for (int i = 0; line[i] && i < start_col; i++) {
            if (line[i] != ' ' && line[i] != '\t') {
                only_indent_before_token = false;
                break;
            }
        }
        if (!only_indent_before_token && prefix[0] != '@') return false;
        if (only_indent_before_token && prefix[0] != '@') {
            for (size_t i = 0; i < sizeof(shell_blocks) / sizeof(shell_blocks[0]); i++) {
                completion_add(app, shell_blocks[i], prefix);
            }
            return app->completion_count > 0;
        }
        for (size_t i = 0; i < sizeof(directives) / sizeof(directives[0]); i++) {
            completion_add(app, directives[i], prefix);
        }
    }

    return app->completion_count > 0;
}

static void editor_replace_range(App *app, int start_col, int end_col, const char *text) {
    char *line = app->editor_lines[app->editor_row];
    int len = (int)strlen(line);
    if (start_col < 0) start_col = 0;
    if (end_col < start_col) end_col = start_col;
    if (end_col > len) end_col = len;
    int text_len = (int)strlen(text);
    size_t new_len = (size_t)(len - (end_col - start_col) + text_len);
    if (!editor_ensure_line_capacity(app, app->editor_row, new_len)) return;
    line = app->editor_lines[app->editor_row];
    editor_save_undo(app);
    memmove(line + start_col + text_len, line + end_col, (size_t)(len - end_col + 1));
    memcpy(line + start_col, text, (size_t)text_len);
    app->editor_col = start_col + text_len;
    editor_update_desired_col(app);
}

static void editor_complete_tui_block(App *app, int start_col, int end_col) {
    if (app->editor_line_count >= EDITOR_LINES) {
        editor_replace_range(app, start_col, end_col, "@tui ");
        return;
    }

    char *line = app->editor_lines[app->editor_row];
    int len = (int)strlen(line);
    if (start_col < 0) start_col = 0;
    if (end_col < start_col) end_col = start_col;
    if (end_col > len) end_col = len;

    const char *text = "@tui ";
    int text_len = (int)strlen(text);
    size_t new_len = (size_t)(len - (end_col - start_col) + text_len);
    if (!editor_ensure_line_capacity(app, app->editor_row, new_len)) return;
    line = app->editor_lines[app->editor_row];

    int indent_len = 0;
    while (line[indent_len] == ' ' || line[indent_len] == '\t') indent_len++;
    char *end_line = malloc((size_t)indent_len + 5);
    if (!end_line) {
        set_status(app, "Out of memory.");
        return;
    }
    memcpy(end_line, line, (size_t)indent_len);
    memcpy(end_line + indent_len, "@end", 5);

    editor_save_undo(app);
    memmove(line + start_col + text_len, line + end_col, (size_t)(len - end_col + 1));
    memcpy(line + start_col, text, (size_t)text_len);

    if (!editor_insert_empty_line(app, app->editor_row + 1)) {
        free(end_line);
        return;
    }
    editor_set_line(app, app->editor_row + 1, end_line);
    free(end_line);
    app->editor_col = start_col + text_len;
    editor_update_desired_col(app);
    set_status(app, "Completed @tui block.");
}

static void editor_complete_shell_block(App *app, int start_col, int end_col, const char *kind) {
    if (app->editor_line_count + 2 > EDITOR_LINES) {
        editor_replace_range(app, start_col, end_col, kind);
        return;
    }

    char *line = app->editor_lines[app->editor_row];
    int len = (int)strlen(line);
    if (start_col < 0) start_col = 0;
    if (end_col < start_col) end_col = start_col;
    if (end_col > len) end_col = len;

    int indent_len = 0;
    while (line[indent_len] == ' ' || line[indent_len] == '\t') indent_len++;

    const char *header = strcmp(kind, "for ") == 0 ? "for item in ; do" : "if [  ]; then";
    const char *footer = strcmp(kind, "for ") == 0 ? "done" : "fi";
    int cursor_offset = strcmp(kind, "for ") == 0 ?
        (int)strlen("for item in ") :
        (int)strlen("if [ ");
    int header_len = (int)strlen(header);
    size_t new_len = (size_t)(len - (end_col - start_col) + header_len);
    if (!editor_ensure_line_capacity(app, app->editor_row, new_len)) return;
    line = app->editor_lines[app->editor_row];

    char *body_line = malloc((size_t)indent_len + 3);
    char *end_line = malloc((size_t)indent_len + strlen(footer) + 1);
    if (!body_line || !end_line) {
        free(body_line);
        free(end_line);
        set_status(app, "Out of memory.");
        return;
    }
    memcpy(body_line, line, (size_t)indent_len);
    memcpy(body_line + indent_len, "  ", 3);
    memcpy(end_line, line, (size_t)indent_len);
    memcpy(end_line + indent_len, footer, strlen(footer) + 1);

    editor_save_undo(app);
    memmove(line + start_col + header_len, line + end_col, (size_t)(len - end_col + 1));
    memcpy(line + start_col, header, (size_t)header_len);

    if (!editor_insert_empty_line(app, app->editor_row + 1)) {
        free(body_line);
        free(end_line);
        return;
    }
    editor_set_line(app, app->editor_row + 1, body_line);
    if (!editor_insert_empty_line(app, app->editor_row + 2)) {
        free(body_line);
        free(end_line);
        return;
    }
    editor_set_line(app, app->editor_row + 2, end_line);
    free(body_line);
    free(end_line);
    app->editor_col = start_col + cursor_offset;
    editor_update_desired_col(app);
    set_status(app, "Completed shell block.");
}

static void editor_accept_completion(App *app) {
    if (app->completion_count <= 0) return;
    if (app->completion_selected < 0) app->completion_selected = 0;
    if (app->completion_selected >= app->completion_count) app->completion_selected = app->completion_count - 1;
    char item[TEXT_LEN];
    copy_text(item, sizeof(item), app->completion_items[app->completion_selected]);
    int start_col = app->completion_start_col;
    int end_col = app->completion_end_col;
    completion_reset(app);
    if (strcmp(item, "@tui ") == 0) {
        editor_complete_tui_block(app, start_col, end_col);
    } else if (strcmp(item, "for ") == 0 || strcmp(item, "if ") == 0) {
        editor_complete_shell_block(app, start_col, end_col, item);
    } else {
        editor_replace_range(app, start_col, end_col, item);
        set_status(app, "Completed.");
    }
}

static bool completion_common_prefix(const App *app, char *out, size_t out_sz) {
    if (out_sz == 0 || app->completion_count <= 1) return false;
    copy_text(out, out_sz, app->completion_items[0]);
    for (int i = 1; i < app->completion_count; i++) {
        size_t j = 0;
        while (out[j] && app->completion_items[i][j] &&
               out[j] == app->completion_items[i][j] &&
               j + 1 < out_sz) {
            j++;
        }
        out[j] = '\0';
    }
    return out[0] != '\0';
}

static bool editor_try_tab_completion(App *app) {
    if (!editor_build_completion(app)) return false;
    if (app->completion_count == 1) {
        app->completion_selected = 0;
        editor_accept_completion(app);
    } else {
        char common[TEXT_LEN];
        if (completion_common_prefix(app, common, sizeof(common))) {
            int typed_len = app->completion_end_col - app->completion_start_col;
            if ((int)strlen(common) > typed_len) {
                int start_col = app->completion_start_col;
                int end_col = app->completion_end_col;
                completion_reset(app);
                editor_replace_range(app, start_col, end_col, common);
                set_status(app, "Completed common prefix.");
                return true;
            }
        }
        app->completion_open = true;
        app->completion_selected = 0;
        set_status(app, "Completion opened.");
    }
    return true;
}

static void editor_newline(App *app) {
    if (app->editor_line_count >= EDITOR_LINES) return;
    char *line = app->editor_lines[app->editor_row];
    int len = (int)strlen(line);
    if (app->editor_col > len) app->editor_col = len;
    int indent_len = 0;
    while (line[indent_len] == ' ' || line[indent_len] == '\t') indent_len++;
    size_t suffix_len = strlen(line + app->editor_col);
    size_t next_len = (size_t)indent_len + suffix_len;
    char *next_line = malloc(next_len + 1);
    if (!next_line) {
        set_status(app, "Out of memory.");
        return;
    }
    if (indent_len > 0) memcpy(next_line, line, (size_t)indent_len);
    if (suffix_len > 0) memcpy(next_line + indent_len, line + app->editor_col, suffix_len);
    next_line[next_len] = '\0';

    editor_save_undo(app);
    if (!editor_insert_empty_line(app, app->editor_row + 1)) {
        free(next_line);
        return;
    }
    line = app->editor_lines[app->editor_row];
    line[app->editor_col] = '\0';
    editor_set_line(app, app->editor_row + 1, next_line);
    free(next_line);
    app->editor_row++;
    app->editor_col = indent_len;
    editor_update_desired_col(app);
}

static void editor_backspace(App *app) {
    char *line = app->editor_lines[app->editor_row];
    int len = (int)strlen(line);
    if (app->editor_col > 0 && app->editor_col <= len) {
        int prev = utf8_prev_index(line, app->editor_col);
        editor_save_undo(app);
        memmove(line + prev, line + app->editor_col, (size_t)(len - app->editor_col + 1));
        app->editor_col = prev;
        editor_update_desired_col(app);
    } else if (app->editor_row > 0) {
        int prev_len = (int)strlen(app->editor_lines[app->editor_row - 1]);
        if (editor_ensure_line_capacity(app, app->editor_row - 1, (size_t)prev_len + (size_t)len)) {
            editor_save_undo(app);
            memcpy(app->editor_lines[app->editor_row - 1] + prev_len, line, (size_t)len + 1);
            free(app->editor_lines[app->editor_row]);
            for (int r = app->editor_row; r < app->editor_line_count - 1; r++) {
                app->editor_lines[r] = app->editor_lines[r + 1];
                app->editor_line_caps[r] = app->editor_line_caps[r + 1];
            }
            app->editor_lines[app->editor_line_count - 1] = NULL;
            app->editor_line_caps[app->editor_line_count - 1] = 0;
            app->editor_line_count--;
            app->editor_row--;
            app->editor_col = prev_len;
            editor_update_desired_col(app);
        }
    }
}

static void editor_delete_lines(App *app, int count) {
    if (count < 1) count = 1;
    editor_save_undo(app);
    if (app->editor_line_count <= 1) {
        editor_clear_line(app, 0);
        app->editor_row = 0;
        app->editor_col = 0;
        return;
    }
    if (count > app->editor_line_count - app->editor_row) {
        count = app->editor_line_count - app->editor_row;
    }
    for (int r = app->editor_row; r < app->editor_row + count; r++) free(app->editor_lines[r]);
    for (int r = app->editor_row; r + count < app->editor_line_count; r++) {
        app->editor_lines[r] = app->editor_lines[r + count];
        app->editor_line_caps[r] = app->editor_line_caps[r + count];
    }
    for (int r = app->editor_line_count - count; r < app->editor_line_count; r++) {
        app->editor_lines[r] = NULL;
        app->editor_line_caps[r] = 0;
    }
    app->editor_line_count -= count;
    if (app->editor_line_count < 1) {
        app->editor_line_count = 1;
        editor_clear_line(app, 0);
    }
    if (app->editor_row >= app->editor_line_count) app->editor_row = app->editor_line_count - 1;
    int len = (int)strlen(app->editor_lines[app->editor_row]);
    if (app->editor_col > len) app->editor_col = len;
    editor_update_desired_col(app);
}

static void editor_copy_lines(App *app, int count) {
    if (count < 1) count = 1;
    if (count > app->editor_line_count - app->editor_row) {
        count = app->editor_line_count - app->editor_row;
    }
    app->editor_clipboard_count = count;
    for (int i = 0; i < count; i++) {
        editor_set_clipboard_line(app, i, app->editor_lines[app->editor_row + i]);
    }
    set_status(app, "Copied line(s).");
}

static void editor_copy_line_range(App *app, int start, int end) {
    if (start > end) {
        int tmp = start;
        start = end;
        end = tmp;
    }
    if (start < 0) start = 0;
    if (end >= app->editor_line_count) end = app->editor_line_count - 1;
    int count = end - start + 1;
    if (count < 1) return;
    app->editor_clipboard_count = count;
    for (int i = 0; i < count; i++) {
        editor_set_clipboard_line(app, i, app->editor_lines[start + i]);
    }
    snprintf(app->status, sizeof(app->status), "Copied %d line(s).", count);
}

static void editor_set_range_status(App *app) {
    int start = app->editor_range_anchor;
    int end = app->editor_row;
    if (start > end) {
        int tmp = start;
        start = end;
        end = tmp;
    }
    snprintf(app->status, sizeof(app->status), "%c range: line %d to %d",
             app->editor_range_op, start + 1, end + 1);
}

static void editor_cancel_line_range(App *app) {
    app->editor_range_pending = false;
    app->editor_range_op = '\0';
    app->editor_range_anchor = 0;
    app->editor_normal_command_len = 0;
    app->editor_normal_command[0] = '\0';
}

static void editor_start_line_range(App *app, char op) {
    app->editor_range_pending = true;
    app->editor_range_op = op;
    app->editor_range_anchor = app->editor_row;
    editor_set_range_status(app);
}

static bool editor_apply_line_range(App *app, char op) {
    if (!app->editor_range_pending || app->editor_range_op != op) return false;
    int start = app->editor_range_anchor;
    int end = app->editor_row;
    if (start > end) {
        int tmp = start;
        start = end;
        end = tmp;
    }
    int count = end - start + 1;
    if (op == 'y') {
        editor_copy_line_range(app, start, end);
    } else if (op == 'd') {
        app->editor_row = start;
        editor_delete_lines(app, count);
        snprintf(app->status, sizeof(app->status), "Deleted %d line(s).", count);
    } else {
        editor_cancel_line_range(app);
        return false;
    }
    editor_cancel_line_range(app);
    return true;
}

static bool editor_is_range_motion_key(int ch) {
    return ch == KEY_UP || ch == KEY_DOWN || ch == 'j' || ch == 'k' ||
           ch == 'G' || ch == 'n' || ch == 'N';
}

static void editor_paste_lines(App *app, bool above) {
    if (app->editor_clipboard_count <= 0) {
        set_status(app, "Clipboard is empty.");
        return;
    }
    int insert_at = above ? app->editor_row : app->editor_row + 1;
    int count = app->editor_clipboard_count;
    if (count > EDITOR_LINES - app->editor_line_count) {
        count = EDITOR_LINES - app->editor_line_count;
    }
    if (count <= 0) {
        set_status(app, "Editor line limit reached.");
        return;
    }
    editor_save_undo(app);
    for (int r = app->editor_line_count - 1; r >= insert_at; r--) {
        app->editor_lines[r + count] = app->editor_lines[r];
        app->editor_line_caps[r + count] = app->editor_line_caps[r];
    }
    for (int i = 0; i < count; i++) {
        app->editor_lines[insert_at + i] = dup_text_heap(app->editor_clipboard[i]);
        app->editor_line_caps[insert_at + i] = app->editor_lines[insert_at + i] ?
            strlen(app->editor_lines[insert_at + i]) + 1 : 0;
        if (!app->editor_lines[insert_at + i]) editor_clear_line(app, insert_at + i);
    }
    app->editor_line_count += count;
    app->editor_row = insert_at;
    app->editor_col = 0;
    editor_update_desired_col(app);
    set_status(app, "Pasted line(s).");
}

static bool editor_search_next(App *app, int direction) {
    if (!app->editor_search[0]) {
        set_status(app, "No search pattern.");
        return false;
    }
    int start = app->editor_row;
    for (int step = 1; step <= app->editor_line_count; step++) {
        int row = (start + direction * step) % app->editor_line_count;
        if (row < 0) row += app->editor_line_count;
        char *pos = strstr(app->editor_lines[row], app->editor_search);
        if (pos) {
            app->editor_row = row;
            app->editor_col = (int)(pos - app->editor_lines[row]);
            editor_update_desired_col(app);
            set_status(app, "Found.");
            return true;
        }
    }
    set_status(app, "Pattern not found.");
    return false;
}

static void editor_goto_line(App *app, int row) {
    if (row < 0) row = 0;
    if (row >= app->editor_line_count) row = app->editor_line_count - 1;
    app->editor_row = row;
    int len = (int)strlen(app->editor_lines[app->editor_row]);
    if (app->editor_col > len) app->editor_col = len;
    editor_update_desired_col(app);
}

static bool editor_handle_normal_sequence(App *app, int ch) {
    if (app->editor_normal_command_len == 0 &&
        ch != 'd' && ch != 'y' && ch != 'g' && !isdigit((unsigned char)ch)) {
        return false;
    }
    if (ch == 27) {
        editor_cancel_line_range(app);
        return true;
    }
    if (ch < 0 || ch > 127 || !isprint((unsigned char)ch)) {
        editor_cancel_line_range(app);
        return false;
    }
    int len = app->editor_normal_command_len;
    if (len + 1 >= (int)sizeof(app->editor_normal_command)) {
        editor_cancel_line_range(app);
        return true;
    }
    if (len == 0) {
        app->editor_normal_command[len++] = (char)ch;
        app->editor_normal_command[len] = '\0';
        app->editor_normal_command_len = len;
        if (ch == 'd' || ch == 'y') {
            editor_start_line_range(app, (char)ch);
        }
        return true;
    }
    char first = app->editor_normal_command[0];
    bool valid = false;
    if ((first == 'd' || first == 'y') && len == 1 && (isdigit((unsigned char)ch) || ch == first)) valid = true;
    else if ((first == 'd' || first == 'y') && len > 1 &&
             isdigit((unsigned char)app->editor_normal_command[len - 1]) &&
             (isdigit((unsigned char)ch) || ch == first)) valid = true;
    else if (first == 'g' && len == 1 && ch == 'g') valid = true;
    else if (isdigit((unsigned char)first) && (isdigit((unsigned char)ch) || ch == 'G')) valid = true;
    if (!valid) {
        editor_cancel_line_range(app);
        return false;
    }
    app->editor_normal_command[len++] = (char)ch;
    app->editor_normal_command[len] = '\0';
    app->editor_normal_command_len = len;
    first = app->editor_normal_command[0];
    if ((first == 'd' || first == 'y') && ch == first) {
        int count = 1;
        if (len > 2) {
            count = atoi(app->editor_normal_command + 1);
            if (count < 1) count = 1;
        }
        if (first == 'd') editor_delete_lines(app, count);
        else editor_copy_lines(app, count);
        editor_cancel_line_range(app);
    } else if (first == 'g' && ch == 'g') {
        editor_goto_line(app, 0);
        editor_cancel_line_range(app);
    } else if (isdigit((unsigned char)first) && ch == 'G') {
        int line_no = atoi(app->editor_normal_command);
        editor_goto_line(app, line_no - 1);
        editor_cancel_line_range(app);
    }
    return true;
}

static bool editor_handle_escape_sequence(App *app) {
    int seq[16];
    int n = 0;
    nodelay(stdscr, TRUE);
    for (int i = 0; i < (int)(sizeof(seq) / sizeof(seq[0])); i++) {
        int c = read_tui_input();
        if (c == ERR) break;
        seq[n++] = c;
        if (seq[0] == 'O' && n >= 2) break;
        if (seq[0] == '[' && n >= 2 && c >= '@' && c <= '~') break;
    }
    nodelay(stdscr, FALSE);
    if (n == 0) return false;

    bool handled = false;
    if (n >= 2 && seq[0] == '[' && seq[1] == 'H') {
        app->editor_col = 0;
        editor_update_desired_col(app);
        handled = true;
    } else if (n >= 2 && seq[0] == '[' && seq[1] == 'F') {
        app->editor_col = (int)strlen(app->editor_lines[app->editor_row]);
        editor_update_desired_col(app);
        handled = true;
    } else if (n >= 2 && seq[0] == 'O' && seq[1] == 'H') {
        app->editor_col = 0;
        editor_update_desired_col(app);
        handled = true;
    } else if (n >= 2 && seq[0] == 'O' && seq[1] == 'F') {
        app->editor_col = (int)strlen(app->editor_lines[app->editor_row]);
        editor_update_desired_col(app);
        handled = true;
    } else if (n >= 3 && seq[0] == '[') {
        int first_param = -1;
        if (isdigit((unsigned char)seq[1])) first_param = seq[1] - '0';
        if (seq[n - 1] == 'H' || (seq[n - 1] == '~' && (first_param == 1 || first_param == 7))) {
            app->editor_col = 0;
            editor_update_desired_col(app);
            handled = true;
        } else if (seq[n - 1] == 'F' || (seq[n - 1] == '~' && (first_param == 4 || first_param == 8))) {
            app->editor_col = (int)strlen(app->editor_lines[app->editor_row]);
            editor_update_desired_col(app);
            handled = true;
        }
    }
    if (!handled) {
        for (int i = n - 1; i >= 0; i--) ungetch(seq[i]);
    }
    return handled;
}

static void handle_script_editor_key(App *app, int ch) {
    if (app->editor_insert) {
        bool completion_handled = false;
        if (app->completion_open) {
            if (ch == '\t' || ch == KEY_DOWN) {
                app->completion_selected = (app->completion_selected + 1) % app->completion_count;
                completion_handled = true;
            } else if (ch == KEY_UP || ch == KEY_BTAB) {
                app->completion_selected--;
                if (app->completion_selected < 0) app->completion_selected = app->completion_count - 1;
                completion_handled = true;
            } else if (ch == '\n' || ch == KEY_ENTER) {
                editor_accept_completion(app);
                completion_handled = true;
            } else if (ch == 27) {
                completion_reset(app);
                set_status(app, "Closed completion.");
                completion_handled = true;
            } else {
                completion_reset(app);
            }
        }
        if (completion_handled) {
            editor_ensure_cursor_visible(app, editor_visible_rows(app, LINES));
            return;
        }

        if (ch == 27) {
            if (!editor_handle_escape_sequence(app)) {
                app->editor_insert = false;
                curs_set(0);
            }
        } else if (ch == '\n' || ch == KEY_ENTER) {
            editor_newline(app);
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            editor_backspace(app);
        } else if (ch == KEY_LEFT && app->editor_col > 0) {
            app->editor_col = utf8_prev_index(app->editor_lines[app->editor_row], app->editor_col);
            editor_update_desired_col(app);
        } else if (ch == KEY_RIGHT) {
            int len = (int)strlen(app->editor_lines[app->editor_row]);
            if (app->editor_col < len) {
                app->editor_col = utf8_next_index(app->editor_lines[app->editor_row], app->editor_col);
                editor_update_desired_col(app);
            }
        } else if (ch == KEY_HOME) {
            app->editor_col = 0;
            editor_update_desired_col(app);
        } else if (ch == KEY_END) {
            app->editor_col = (int)strlen(app->editor_lines[app->editor_row]);
            editor_update_desired_col(app);
        } else if (ch == KEY_UP && app->editor_row > 0) {
            editor_move_vertical(app, -1);
        } else if (ch == KEY_DOWN && app->editor_row + 1 < app->editor_line_count) {
            editor_move_vertical(app, 1);
        } else if (ch == '\t') {
            if (!editor_try_tab_completion(app)) {
                editor_insert_char(app, '\t');
            }
        } else if (editor_is_text_char(ch)) {
            editor_insert_wchar(app, (wint_t)ch);
        }
        int len = (int)strlen(app->editor_lines[app->editor_row]);
        if (app->editor_col > len) app->editor_col = len;
        editor_ensure_cursor_visible(app, editor_visible_rows(app, LINES));
        return;
    }

    if (app->editor_help_open) {
        if (ch == 27 || ch == '\n' || ch == KEY_ENTER || ch == 'q') {
            app->editor_help_open = false;
            set_status(app, "Closed editor help.");
        } else {
            int box_h = EDITOR_HELP_LINE_COUNT + 4;
            if (box_h > LINES - 4) box_h = LINES - 4;
            if (box_h < 8) box_h = 8;
            int rows = box_h - 3;
            int max_scroll = EDITOR_HELP_LINE_COUNT > rows ? EDITOR_HELP_LINE_COUNT - rows : 0;
            if (ch == KEY_DOWN || ch == 'j') {
                if (app->editor_help_scroll < max_scroll) app->editor_help_scroll++;
            } else if (ch == KEY_UP || ch == 'k') {
                if (app->editor_help_scroll > 0) app->editor_help_scroll--;
            } else if (ch == KEY_NPAGE) {
                app->editor_help_scroll += rows;
                if (app->editor_help_scroll > max_scroll) app->editor_help_scroll = max_scroll;
            } else if (ch == KEY_PPAGE) {
                app->editor_help_scroll -= rows;
                if (app->editor_help_scroll < 0) app->editor_help_scroll = 0;
            } else if (ch == KEY_HOME || ch == 'g') {
                app->editor_help_scroll = 0;
            } else if (ch == KEY_END || ch == 'G') {
                app->editor_help_scroll = max_scroll;
            }
        }
        return;
    }

    if (app->editor_command_mode) {
        if (ch == '\n' || ch == KEY_ENTER) {
            app->editor_command[app->editor_command_len] = '\0';
            if (strcmp(app->editor_command, "w") == 0) {
                save_current_editor(app, false);
            } else if (strcmp(app->editor_command, "wq") == 0) {
                save_current_editor(app, true);
            } else if (strcmp(app->editor_command, "q") == 0) {
                if (editor_has_unsaved_changes(app)) {
                    set_status(app, "E37: No write since last change (add ! to override)");
                } else {
                    app->screen = app->previous_screen;
                    set_status(app, "Canceled script editor.");
                }
            } else if (strcmp(app->editor_command, "q!") == 0) {
                if (app->editor_target == EDIT_TARGET_COMMAND) discard_current_case_if_new(app);
                app->editor_dirty = false;
                app->screen = app->previous_screen;
                set_status(app, "Canceled script editor.");
            } else if (strcmp(app->editor_command, "print") == 0) {
                print_editor_script(app);
            } else if (strcmp(app->editor_command, "template") == 0 && editor_uses_regex_templates(app)) {
                app->regex_template_list_open = true;
                app->selected_menu = 0;
                set_status(app, "Opened regex templates.");
            } else if (strcmp(app->editor_command, "help") == 0) {
                app->editor_help_open = true;
                app->editor_help_scroll = 0;
                app->regex_template_list_open = false;
                set_status(app, "Opened editor help.");
            } else {
                set_status(app, "Unknown editor command.");
            }
            app->editor_command_len = 0;
            app->editor_command[0] = '\0';
            app->editor_command_mode = false;
        } else if (ch == KEY_BACKSPACE || ch == KEY_DC || ch == 127 || ch == 8) {
            if (app->editor_command_len > 0) {
                app->editor_command[--app->editor_command_len] = '\0';
            } else {
                app->editor_command_mode = false;
            }
        } else if (ch >= 0 && ch < 128 && isprint((unsigned char)ch) &&
                   app->editor_command_len + 1 < (int)sizeof(app->editor_command)) {
            app->editor_command[app->editor_command_len++] = (char)ch;
            app->editor_command[app->editor_command_len] = '\0';
        } else if (ch == 27) {
            app->editor_command_len = 0;
            app->editor_command[0] = '\0';
            app->editor_command_mode = false;
        }
        if (app->screen == SCREEN_SCRIPT_EDITOR) {
            editor_ensure_cursor_visible(app, editor_visible_rows(app, LINES));
        }
        return;
    }

    if (app->editor_search_mode) {
        if (ch == '\n' || ch == KEY_ENTER) {
            app->editor_search[app->editor_search_len] = '\0';
            app->editor_search_mode = false;
            editor_search_next(app, 1);
        } else if (ch == KEY_BACKSPACE || ch == KEY_DC || ch == 127 || ch == 8) {
            if (app->editor_search_len > 0) {
                int prev = utf8_prev_index(app->editor_search, app->editor_search_len);
                app->editor_search_len = prev;
                app->editor_search[app->editor_search_len] = '\0';
            } else {
                app->editor_search_mode = false;
            }
        } else if (ch == 27) {
            app->editor_search_len = 0;
            app->editor_search[0] = '\0';
            app->editor_search_mode = false;
        } else if (editor_is_text_char(ch)) {
            append_wchar_utf8(app->editor_search, &app->editor_search_len,
                              sizeof(app->editor_search), (wint_t)ch);
        }
        editor_ensure_cursor_visible(app, editor_visible_rows(app, LINES));
        return;
    }

    if (editor_uses_regex_templates(app)) {
        if (app->regex_template_list_open && ch == 27) {
            app->regex_template_list_open = false;
            set_status(app, "Closed regex templates.");
            return;
        }
        if (app->regex_template_list_open && (ch == '\t' || ch == KEY_DOWN)) {
            app->selected_menu = (app->selected_menu + 1) % REGEX_TEMPLATE_COUNT;
            return;
        }
        if (app->regex_template_list_open && (ch == KEY_BTAB || ch == KEY_UP)) {
            app->selected_menu--;
            if (app->selected_menu < 0) app->selected_menu = REGEX_TEMPLATE_COUNT - 1;
            return;
        }
        if (app->regex_template_list_open && (ch == '\n' || ch == KEY_ENTER)) {
            if (app->selected_menu < 0 || app->selected_menu >= REGEX_TEMPLATE_COUNT) app->selected_menu = 0;
            editor_insert_text(app, REGEX_TEMPLATES[app->selected_menu].text);
            set_status(app, "Inserted regex template.");
            return;
        }
    }

    bool allow_normal_sequence = true;
    if (app->editor_range_pending) {
        if (ch == 27) {
            editor_cancel_line_range(app);
            set_status(app, "Canceled line range.");
            editor_ensure_cursor_visible(app, editor_visible_rows(app, LINES));
            return;
        }
        if (ch == app->editor_range_op && app->editor_normal_command_len == 1) {
            editor_apply_line_range(app, (char)ch);
            editor_ensure_cursor_visible(app, editor_visible_rows(app, LINES));
            return;
        }
        if (editor_is_range_motion_key(ch)) {
            allow_normal_sequence = false;
        } else if (!(isdigit((unsigned char)ch) ||
                     (ch == app->editor_range_op && app->editor_normal_command_len > 1))) {
            editor_cancel_line_range(app);
        }
    }

    if (allow_normal_sequence && editor_handle_normal_sequence(app, ch)) {
        int len = (int)strlen(app->editor_lines[app->editor_row]);
        if (app->editor_col > len) app->editor_col = len;
        editor_ensure_cursor_visible(app, editor_visible_rows(app, LINES));
        return;
    }

    switch (ch) {
    case 'i':
        app->editor_normal_command_len = 0;
        app->editor_normal_command[0] = '\0';
        app->editor_insert = true;
        curs_set(1);
        break;
    case ':':
        app->editor_normal_command_len = 0;
        app->editor_normal_command[0] = '\0';
        app->editor_command_mode = true;
        app->editor_command_len = 0;
        app->editor_command[0] = '\0';
        break;
    case '/':
        app->editor_normal_command_len = 0;
        app->editor_normal_command[0] = '\0';
        app->editor_search_mode = true;
        app->editor_search_len = 0;
        app->editor_search[0] = '\0';
        break;
    case 'u':
        editor_undo(app);
        break;
    case 'p':
        editor_paste_lines(app, false);
        break;
    case 'P':
        editor_paste_lines(app, true);
        break;
    case 'G':
        editor_goto_line(app, app->editor_line_count - 1);
        break;
    case 'n':
        editor_search_next(app, 1);
        break;
    case 'N':
        editor_search_next(app, -1);
        break;
    case KEY_UP:
    case 'k':
        if (app->editor_row > 0) editor_move_vertical(app, -1);
        break;
    case KEY_DOWN:
    case 'j':
        if (app->editor_row + 1 < app->editor_line_count) editor_move_vertical(app, 1);
        break;
    case KEY_LEFT:
    case 'h':
        if (app->editor_col > 0) {
            app->editor_col = utf8_prev_index(app->editor_lines[app->editor_row], app->editor_col);
            editor_update_desired_col(app);
        }
        break;
    case KEY_RIGHT:
    case 'l': {
        int len = (int)strlen(app->editor_lines[app->editor_row]);
        if (app->editor_col < len) {
            app->editor_col = utf8_next_index(app->editor_lines[app->editor_row], app->editor_col);
            editor_update_desired_col(app);
        }
        break;
    }
    case KEY_HOME:
    case '0':
        app->editor_col = 0;
        editor_update_desired_col(app);
        break;
    case KEY_END:
    case '$':
        app->editor_col = (int)strlen(app->editor_lines[app->editor_row]);
        editor_update_desired_col(app);
        break;
    case 'x': {
        char *line = app->editor_lines[app->editor_row];
        int len = (int)strlen(line);
        if (app->editor_col < len) {
            int next = utf8_next_index(line, app->editor_col);
            editor_save_undo(app);
            memmove(line + app->editor_col, line + next, (size_t)(len - next + 1));
            editor_update_desired_col(app);
        }
        break;
    }
    case 'o':
        app->editor_col = (int)strlen(app->editor_lines[app->editor_row]);
        editor_newline(app);
        app->editor_insert = true;
        curs_set(1);
        break;
    case 27:
        if (!editor_handle_escape_sequence(app)) {
            set_status(app, "Type  :w  to write,  :q!  to quit without saving.");
        }
        break;
    default:
        break;
    }
    int len = (int)strlen(app->editor_lines[app->editor_row]);
    if (app->editor_col > len) app->editor_col = len;
    if (app->editor_range_pending && editor_is_range_motion_key(ch)) {
        editor_set_range_status(app);
    }
    editor_ensure_cursor_visible(app, editor_visible_rows(app, LINES));
}

static void add_case(App *app) {
    if (app->project.case_count >= MAX_CASES) {
        set_status(app, "Maximum test cases reached.");
        return;
    }
    TestCase new_case;
    memset(&new_case, 0, sizeof(new_case));
    snprintf(new_case.id, sizeof(new_case.id), "TC%03d", app->project.case_count + 1);
    snprintf(new_case.title, sizeof(new_case.title), "new test case");
    set_test_command(&new_case, "true");
    new_case.kind = CMD_SHELL;
    new_case.selected = true;
    new_case.expected_exit = 0;
    new_case.check_count = 0;
    new_case.timeout_sec = 30;
    if (!prompt_text("test title", new_case.title, sizeof(new_case.title))) {
        free_test_case(&new_case);
        set_status(app, "Canceled test title.");
        return;
    }
    if (title_path_conflicts(&new_case, new_case.title)) {
        free_test_case(&new_case);
        set_status(app, "Script name already exists in this directory.");
        return;
    }
    app->selected_case = app->project.case_count;
    app->project.cases[app->project.case_count] = new_case;
    app->project.case_count++;
    app->editor_new_case = true;
    open_script_editor(app);
    app->selected_field = 1;
    set_status(app, "Added a test case.");
}

static void delete_case(App *app) {
    if (app->project.case_count == 0) return;
    char deleted_path[LONG_LEN];
    char result_path[LONG_LEN];
    int unlink_rc = 0;
    int result_unlink_rc = 0;
    copy_text(deleted_path, sizeof(deleted_path), app->project.cases[app->selected_case].script_path);
    result_path[0] = '\0';
    if (deleted_path[0]) {
        unlink_rc = unlink(deleted_path);
        make_result_path(deleted_path, result_path, sizeof(result_path));
        result_unlink_rc = unlink(result_path);
    }
    free_test_case(&app->project.cases[app->selected_case]);
    for (int i = app->selected_case; i < app->project.case_count - 1; i++) {
        app->project.cases[i] = app->project.cases[i + 1];
    }
    app->project.case_count--;
    if (app->project.case_count >= 0) memset(&app->project.cases[app->project.case_count], 0, sizeof(app->project.cases[app->project.case_count]));
    clamp_selected(app);
    save_test_registry(&app->project);
    if (deleted_path[0] && (unlink_rc != 0 || (result_unlink_rc != 0 && access(result_path, F_OK) == 0))) {
        set_status(app, "Deleted test case, but failed to delete script file.");
    } else {
        set_status(app, "Deleted test case, script file, and result file.");
    }
}

static void rename_case(App *app) {
    if (app->project.case_count == 0) return;
    TestCase *tc = &app->project.cases[app->selected_case];
    char old_title[TEXT_LEN];
    copy_text(old_title, sizeof(old_title), tc->title);
    if (!prompt_text("test title", tc->title, sizeof(tc->title))) {
        set_status(app, "Canceled test rename.");
        return;
    }
    if (title_path_conflicts(tc, tc->title)) {
        copy_text(tc->title, sizeof(tc->title), old_title);
        set_status(app, "Script name already exists in this directory.");
        return;
    }
    int rc = write_single_test_script(tc);
    if (rc == 0) {
        save_test_registry(&app->project);
        set_status(app, "Renamed test.");
    } else if (rc == -2) {
        copy_text(tc->title, sizeof(tc->title), old_title);
        set_status(app, "Script name already exists in this directory.");
    } else {
        copy_text(tc->title, sizeof(tc->title), old_title);
        set_status(app, "Failed to rename test.");
    }
}

static void copy_case(App *app) {
    if (app->project.case_count == 0) return;
    if (app->project.case_count >= MAX_CASES) {
        set_status(app, "Maximum test cases reached.");
        return;
    }
    TestCase new_case = app->project.cases[app->selected_case];
    new_case.command = dup_text_heap(test_command(&app->project.cases[app->selected_case]));
    if (!new_case.command) {
        set_status(app, "Out of memory.");
        return;
    }
    snprintf(new_case.id, sizeof(new_case.id), "TC%03d", app->project.case_count + 1);
    char default_title[TEXT_LEN];
    copy_text(default_title, sizeof(default_title), new_case.title[0] ? new_case.title : new_case.id);
    if (strlen(default_title) + 5 < sizeof(default_title)) strcat(default_title, " copy");
    copy_text(new_case.title, sizeof(new_case.title), default_title);
    new_case.script_path[0] = '\0';
    new_case.selected = true;
    if (!prompt_text("copy test title", new_case.title, sizeof(new_case.title))) {
        free_test_case(&new_case);
        set_status(app, "Canceled test copy.");
        return;
    }
    if (title_path_conflicts(&new_case, new_case.title)) {
        free_test_case(&new_case);
        set_status(app, "Script name already exists in this directory.");
        return;
    }
    int rc = write_single_test_script(&new_case);
    if (rc == 0) {
        app->selected_case = app->project.case_count;
        app->project.cases[app->project.case_count++] = new_case;
        save_test_registry(&app->project);
        set_status(app, "Copied test.");
    } else if (rc == -2) {
        free_test_case(&new_case);
        set_status(app, "Script name already exists in this directory.");
    } else {
        free_test_case(&new_case);
        set_status(app, "Failed to copy test.");
    }
}

static void edit_selected_field(App *app) {
    if (app->project.case_count == 0) return;
    TestCase *tc = &app->project.cases[app->selected_case];
    switch (app->selected_field) {
    case 0: prompt_text("id", tc->id, sizeof(tc->id)); break;
    case 1: {
        char old_title[TEXT_LEN];
        copy_text(old_title, sizeof(old_title), tc->title);
        if (prompt_text("title", tc->title, sizeof(tc->title))) {
            if (title_path_conflicts(tc, tc->title)) {
                copy_text(tc->title, sizeof(tc->title), old_title);
                set_status(app, "Script name already exists in this directory.");
                return;
            }
        }
        break;
    }
    case 2: tc->selected = !tc->selected; break;
    case 3: cycle_kind(tc); break;
    case 4: {
        char command[LONG_LEN];
        copy_text(command, sizeof(command), test_command(tc));
        if (prompt_text("command", command, sizeof(command))) {
            set_test_command(tc, command);
            tc->kind = CMD_SHELL;
        }
        break;
    }
    case 5: tc->expected_exit = prompt_int("expected_exit", tc->expected_exit); break;
    case 6: prompt_text("check_var", primary_check(tc)->var, sizeof(primary_check(tc)->var)); break;
    case 7: cycle_match(&primary_check(tc)->match); break;
    case 8: open_text_editor(app, EDIT_TARGET_CHECK_EXPECTED, SCREEN_FORM); break;
    case 9: prompt_text("cleanup", tc->cleanup, sizeof(tc->cleanup)); break;
    case 10: tc->timeout_sec = prompt_int("timeout_sec", tc->timeout_sec); break;
    case 11: tc->requires_root = !tc->requires_root; break;
    case 12: set_status(app, "script_path is set when the editor is saved."); break;
    default: break;
    }
    set_status(app, "Updated field.");
}

static void write_match_function(FILE *f) {
    fputs("is_valid_var_name() {\n", f);
    fputs("  case \"$1\" in ''|[0-9]*|*[!A-Za-z0-9_]*) return 1 ;; *) return 0 ;; esac\n", f);
    fputs("}\n\n", f);
    fputs("match_value() {\n", f);
    fputs("  type=\"$1\"\n", f);
    fputs("  actual=\"$2\"\n", f);
    fputs("  expected=\"$3\"\n", f);
    fputs("  case \"$type\" in\n", f);
    fputs("    none) return 0 ;;\n", f);
    fputs("    exact) AUTOTEST_MATCH_ACTUAL=\"$actual\" AUTOTEST_MATCH_EXPECTED=\"$expected\" awk 'BEGIN { pattern = \"^(\" ENVIRON[\"AUTOTEST_MATCH_EXPECTED\"] \")$\"; exit(ENVIRON[\"AUTOTEST_MATCH_ACTUAL\"] ~ pattern ? 0 : 1) }' ;;\n", f);
    fputs("    not_exact) AUTOTEST_MATCH_ACTUAL=\"$actual\" AUTOTEST_MATCH_EXPECTED=\"$expected\" awk 'BEGIN { pattern = \"^(\" ENVIRON[\"AUTOTEST_MATCH_EXPECTED\"] \")$\"; exit(ENVIRON[\"AUTOTEST_MATCH_ACTUAL\"] ~ pattern ? 1 : 0) }' ;;\n", f);
    fputs("    contains) AUTOTEST_MATCH_ACTUAL=\"$actual\" AUTOTEST_MATCH_EXPECTED=\"$expected\" awk 'BEGIN { exit(ENVIRON[\"AUTOTEST_MATCH_ACTUAL\"] ~ ENVIRON[\"AUTOTEST_MATCH_EXPECTED\"] ? 0 : 1) }' ;;\n", f);
    fputs("    not_contains) AUTOTEST_MATCH_ACTUAL=\"$actual\" AUTOTEST_MATCH_EXPECTED=\"$expected\" awk 'BEGIN { exit(ENVIRON[\"AUTOTEST_MATCH_ACTUAL\"] ~ ENVIRON[\"AUTOTEST_MATCH_EXPECTED\"] ? 1 : 0) }' ;;\n", f);
    fputs("    empty) [ -z \"$actual\" ] ;;\n", f);
    fputs("    not_empty) [ -n \"$actual\" ] ;;\n", f);
    fputs("    *) return 1 ;;\n", f);
    fputs("  esac\n", f);
    fputs("}\n\n", f);
    fputs("autotest_print_status() {\n", f);
    fputs("  local msg=\"$1\"\n", f);
    fputs("  local tag=\"\"\n", f);
    fputs("  local color=\"\"\n", f);
    fputs("  local rest=\"\"\n", f);
    fputs("  case \"$msg\" in\n", f);
    fputs("    '[OK]'*) tag='[OK]'; color=32; rest=\"${msg:4}\" ;;\n", f);
    fputs("    '[NG]'*) tag='[NG]'; color=31; rest=\"${msg:4}\" ;;\n", f);
    fputs("  esac\n", f);
    fputs("  if [ -n \"$tag\" ] && [ -t 1 ]; then\n", f);
    fputs("    printf '\\033[%sm%s\\033[0m%s\\n' \"$color\" \"$tag\" \"$rest\"\n", f);
    fputs("  else\n", f);
    fputs("    printf '%s\\n' \"$msg\"\n", f);
    fputs("  fi\n", f);
    fputs("}\n\n", f);
    fputs("autotest_backup_key() { printf '%s' \"$1\" | cksum | awk '{print $1}'; }\n\n", f);
    fputs("autotest_backup() {\n", f);
    fputs("  path=\"$1\"\n", f);
    fputs("  [ -n \"${AUTOTEST_BACKUP_DIR:-}\" ] || return 0\n", f);
    fputs("  key=$(autotest_backup_key \"$path\")\n", f);
    fputs("  dest=\"$AUTOTEST_BACKUP_DIR/$key\"\n", f);
    fputs("  rm -rf \"$dest\"\n", f);
    fputs("  mkdir -p \"$dest\"\n", f);
    fputs("  printf '%s\\n' \"$path\" >\"$dest/path\"\n", f);
    fputs("  if [ -e \"$path\" ]; then echo 1 >\"$dest/exists\"; cp -a \"$path\" \"$dest/item\"; else echo 0 >\"$dest/exists\"; fi\n", f);
    fputs("}\n\n", f);
    fputs("autotest_restore() {\n", f);
    fputs("  path=\"$1\"\n", f);
    fputs("  [ -n \"${AUTOTEST_BACKUP_DIR:-}\" ] || return 0\n", f);
    fputs("  key=$(autotest_backup_key \"$path\")\n", f);
    fputs("  dest=\"$AUTOTEST_BACKUP_DIR/$key\"\n", f);
    fputs("  [ -d \"$dest\" ] || return 0\n", f);
    fputs("  if [ \"$(cat \"$dest/exists\" 2>/dev/null || echo 0)\" = 1 ]; then rm -rf \"$path\"; cp -a \"$dest/item\" \"$path\"; else rm -rf \"$path\"; fi\n", f);
    fputs("}\n\n", f);
    fputs("autotest_clean_tui_output() {\n", f);
    fputs("  input=\"$1\"\n", f);
    fputs("  output=\"$2\"\n", f);
    fputs("  if command -v perl >/dev/null 2>&1; then\n", f);
    fputs("    perl -0pe 's/^Script started on .*?\\n//s; s/\\n?Script done on .*?\\n?\\z//s; s/\\e\\[\\?(?:1049|1047|47)h.*?\\e\\[\\?(?:1049|1047|47)l//gs; s/\\e\\[[0-?]*[ -\\/]*[@-~]//g; s/\\e\\][^\\a]*(?:\\a|\\e\\\\)//g; s/\\r+\\n/\\n/g; s/\\r/\\n/g; s/[\\x00-\\x08\\x0B\\x0C\\x0E-\\x1F\\x7F]//g' \"$input\" >\"$output\"\n", f);
    fputs("  else\n", f);
    fputs("    sed -r '/^Script started on /d; /^Script done on /d; s/\\x1B\\[[0-?]*[ -\\/]*[@-~]//g; s/\\r$//g' \"$input\" | tr '\\r' '\\n' | tr -d '\\000-\\010\\013\\014\\016-\\037\\177' >\"$output\"\n", f);
    fputs("  fi\n", f);
    fputs("}\n\n", f);
    fputs("autotest_strip_tui_echo() {\n", f);
    fputs("  output=\"$1\"\n", f);
    fputs("  input=\"$2\"\n", f);
    fputs("  [ -f \"$output\" ] || return 0\n", f);
    fputs("  [ -f \"$input\" ] || return 0\n", f);
    fputs("  output_text=$(cat \"$output\" 2>/dev/null || true)\n", f);
    fputs("  input_text=$(cat \"$input\" 2>/dev/null || true)\n", f);
    fputs("  if command -v perl >/dev/null 2>&1; then\n", f);
    fputs("    perl -0777 -e 'my ($out_path,$in_path)=@ARGV; open my $ofh,\"<\",$out_path or exit 0; local $/; my $out=readline($ofh); open my $ifh,\"<\",$in_path or exit 0; my $in=readline($ifh); my $target=$in; $target =~ s/[\\x00-\\x1F\\x7F]//g; exit 0 unless length $target; my $seen=\"\"; my $pos=-1; for (my $i=0; $i<length($out); $i++) { my $ch=substr($out,$i,1); if ($ch =~ /[\\x00-\\x1F\\x7F]/) { next; } if ($ch eq \"^\" && $i + 1 < length($out) && substr($out,$i+1,1) =~ /[\\@-_?]/) { $i++; next; } $seen .= $ch; if (index($target,$seen) != 0) { last; } if ($seen eq $target) { $pos=$i+1; last; } } if ($pos >= 0) { $out=substr($out,$pos); $out =~ s/^\\s+//; open my $wfh,\">\",$out_path or exit 0; print {$wfh} $out; }' \"$output\" \"$input\"\n", f);
    fputs("  elif [ -n \"$input_text\" ] && [ \"$output_text\" = \"$input_text\" ]; then\n", f);
    fputs("    : >\"$output\"\n", f);
    fputs("  fi\n", f);
    fputs("}\n\n", f);
    fputs("autotest_detail_sanitize() {\n", f);
    fputs("  if command -v perl >/dev/null 2>&1; then\n", f);
    fputs("    perl -0pe 's/\\e\\[[0-?]*[ -\\/]*[@-~]//g; s/\\e\\][^\\a]*(?:\\a|\\e\\\\)//g; s/\\r+\\n/\\n/g; s/\\r/\\n/g; s/[\\x00-\\x08\\x0B\\x0C\\x0E-\\x1F\\x7F]//g'\n", f);
    fputs("  else\n", f);
    fputs("    tr '\\r' '\\n' | tr -d '\\000-\\010\\013\\014\\016-\\037\\177'\n", f);
    fputs("  fi\n", f);
    fputs("}\n\n", f);
    fputs("autotest_detail_cat() {\n", f);
    fputs("  local path=\"$1\"\n", f);
    fputs("  [ -n \"$path\" ] && [ -f \"$path\" ] || return 0\n", f);
    fputs("  if [ \"${AUTOTEST_DETAIL_SAFE:-0}\" = 1 ]; then\n", f);
    fputs("    autotest_detail_sanitize <\"$path\" 2>/dev/null || true\n", f);
    fputs("  else\n", f);
    fputs("    cat \"$path\"\n", f);
    fputs("  fi\n", f);
    fputs("}\n\n", f);
    fputs("autotest_detail_kv() {\n", f);
    fputs("  local key=\"$1\"\n", f);
    fputs("  local value=\"$2\"\n", f);
    fputs("  if [ \"${AUTOTEST_DETAIL_COLOR:-0}\" = 1 ]; then\n", f);
    fputs("    printf '\\033[36m%s\\033[0m=%s\\n' \"$key\" \"$value\"\n", f);
    fputs("  else\n", f);
    fputs("    printf '%s=%s\\n' \"$key\" \"$value\"\n", f);
    fputs("  fi\n", f);
    fputs("}\n\n", f);
    fputs("autotest_detail_value() {\n", f);
    fputs("  local key=\"$1\"\n", f);
    fputs("  local value=\"$2\"\n", f);
    fputs("  if [ \"${AUTOTEST_DETAIL_COLOR:-0}\" = 1 ]; then\n", f);
    fputs("    printf '\\033[36m%s\\033[0m:\\n' \"$key\"\n", f);
    fputs("    printf '%s\\n' \"$value\" | autotest_detail_sanitize\n", f);
    fputs("  else\n", f);
    fputs("    printf '%s=%s\\n' \"$key\" \"$value\"\n", f);
    fputs("  fi\n", f);
    fputs("}\n\n", f);
    fputs("autotest_detail_check_kv() {\n", f);
    fputs("  local failed=\"$1\"\n", f);
    fputs("  local key=\"$2\"\n", f);
    fputs("  local value=\"$3\"\n", f);
    fputs("  if [ \"${AUTOTEST_DETAIL_COLOR:-0}\" = 1 ] && [ \"$failed\" = 1 ]; then\n", f);
    fputs("    printf '\\033[31m%s\\033[0m=%s\\n' \"$key\" \"$value\"\n", f);
    fputs("  else\n", f);
    fputs("    autotest_detail_kv \"$key\" \"$value\"\n", f);
    fputs("  fi\n", f);
    fputs("}\n\n", f);
    fputs("autotest_detail_check_value() {\n", f);
    fputs("  local failed=\"$1\"\n", f);
    fputs("  local key=\"$2\"\n", f);
    fputs("  local value=\"$3\"\n", f);
    fputs("  if [ \"${AUTOTEST_DETAIL_COLOR:-0}\" = 1 ] && [ \"$failed\" = 1 ]; then\n", f);
    fputs("    printf '\\033[31m%s\\033[0m:\\n' \"$key\"\n", f);
    fputs("    printf '%s\\n' \"$value\" | autotest_detail_sanitize\n", f);
    fputs("  else\n", f);
    fputs("    autotest_detail_value \"$key\" \"$value\"\n", f);
    fputs("  fi\n", f);
    fputs("}\n\n", f);
    fputs("autotest_detail_section() {\n", f);
    fputs("  if [ \"${AUTOTEST_DETAIL_COLOR:-0}\" = 1 ]; then\n", f);
    fputs("    printf '\\033[33m%s\\033[0m\\n' \"$1\"\n", f);
    fputs("  else\n", f);
    fputs("    echo \"$1\"\n", f);
    fputs("  fi\n", f);
    fputs("}\n\n", f);
    fputs("autotest_emit_detail_result() {\n", f);
    fputs("  {\n", f);
    fputs("    if [ \"${AUTOTEST_DETAIL_COLOR:-0}\" = 1 ]; then autotest_print_status \"${status_msg-}\"; else echo \"${status_msg-}\"; fi\n", f);
    fputs("    echo \"Summary: total=$TOTAL_COUNT OK=$OK_COUNT NG=$NG_COUNT\"\n", f);
    fputs("    autotest_detail_kv expected_exit \"${expected_exit-}\"\n", f);
    fputs("    autotest_detail_kv actual_exit \"${actual_exit-}\"\n", f);
    fputs("    autotest_detail_kv ok \"${ok-}\"\n", f);
    fputs("    autotest_detail_kv check_count \"${check_count:-0}\"\n", f);
    fputs("    i=1\n", f);
    fputs("    while [ \"$i\" -le \"${check_count:-0}\" ]; do\n", f);
    fputs("      name_ref=\"check_var_$i\"\n", f);
    fputs("      match_ref=\"check_match_$i\"\n", f);
    fputs("      expected_ref=\"expected_value_$i\"\n", f);
    fputs("      value_ref=\"actual_value_$i\"\n", f);
    fputs("      name=\"${!name_ref}\"\n", f);
    fputs("      match=\"${!match_ref}\"\n", f);
    fputs("      expected=\"${!expected_ref}\"\n", f);
    fputs("      value=\"${!value_ref}\"\n", f);
    fputs("      check_failed=0\n", f);
    fputs("      match_value \"$match\" \"$value\" \"$expected\" || check_failed=1\n", f);
    fputs("      autotest_detail_check_kv \"$check_failed\" \"check_$i\" \"$name\"\n", f);
    fputs("      autotest_detail_check_kv \"$check_failed\" \"match_$i\" \"$match\"\n", f);
    fputs("      if [ \"${AUTOTEST_DETAIL_SAFE:-0}\" = 1 ]; then\n", f);
    fputs("        autotest_detail_check_value \"$check_failed\" \"expected $name\" \"$expected\"\n", f);
    fputs("        autotest_detail_check_value \"$check_failed\" \"actual $name\" \"$value\"\n", f);
    fputs("      else\n", f);
    fputs("        autotest_detail_check_kv \"$check_failed\" \"expected_$i\" \"$expected\"\n", f);
    fputs("        autotest_detail_check_kv \"$check_failed\" \"actual_$i\" \"$value\"\n", f);
    fputs("      fi\n", f);
    fputs("      i=$((i + 1))\n", f);
    fputs("    done\n", f);
    fputs("    if [ \"${AUTOTEST_DETAIL_SAFE:-0}\" != 1 ]; then\n", f);
    fputs("      autotest_detail_kv actual_dir \"${actual_dir-}\"\n", f);
    fputs("      autotest_detail_kv tui_text_file \"${AUTOTEST_TUI_TEXT_FILE-}\"\n", f);
    fputs("      autotest_detail_kv tui_transcript_file \"${AUTOTEST_TUI_TRANSCRIPT_FILE-}\"\n", f);
    fputs("      autotest_detail_kv tui_input_file \"${AUTOTEST_TUI_INPUT_FILE-}\"\n", f);
    fputs("      autotest_detail_kv stdout_file \"${stdout_file-}\"\n", f);
    fputs("      autotest_detail_kv stderr_file \"${stderr_file-}\"\n", f);
    fputs("      autotest_detail_section '--- tui text ---'\n", f);
    fputs("      autotest_detail_cat \"${AUTOTEST_TUI_TEXT_FILE-}\"\n", f);
    fputs("      autotest_detail_section '--- tui transcript ---'\n", f);
    fputs("      autotest_detail_cat \"${AUTOTEST_TUI_TRANSCRIPT_FILE-}\"\n", f);
    fputs("      autotest_detail_section '--- stdout ---'\n", f);
    fputs("      autotest_detail_cat \"${stdout_file-}\"\n", f);
    fputs("      autotest_detail_section '--- stderr ---'\n", f);
    fputs("      autotest_detail_cat \"${stderr_file-}\"\n", f);
    fputs("    else\n", f);
    fputs("      if [ -n \"${stdout_file-}\" ] && [ -s \"$stdout_file\" ]; then autotest_detail_section '--- stdout ---'; autotest_detail_cat \"$stdout_file\"; fi\n", f);
    fputs("      if [ -n \"${stderr_file-}\" ] && [ -s \"$stderr_file\" ]; then autotest_detail_section '--- stderr ---'; autotest_detail_cat \"$stderr_file\"; fi\n", f);
    fputs("    fi\n", f);
    fputs("  }\n", f);
    fputs("}\n\n", f);
    fputs("autotest_write_detail_result() {\n", f);
    fputs("  detail_file=\"$1\"\n", f);
    fputs("  [ -n \"$detail_file\" ] || return 0\n", f);
    fputs("  if [ \"$detail_file\" = - ]; then AUTOTEST_DETAIL_SAFE=1 AUTOTEST_DETAIL_COLOR=1 autotest_emit_detail_result; return 0; fi\n", f);
    fputs("  detail_dir=$(dirname \"$detail_file\" 2>/dev/null || echo .)\n", f);
    fputs("  mkdir -p \"$detail_dir\" 2>/dev/null || true\n", f);
    fputs("  autotest_emit_detail_result >\"$detail_file\"\n", f);
    fputs("}\n\n", f);
    fputs("autotest_write_detail_outputs() {\n", f);
    fputs("  autotest_write_detail_result \"$DETAIL_RESULT_FILE\"\n", f);
    fputs("  [ \"${DETAIL_STDOUT:-0}\" = 1 ] && autotest_write_detail_result -\n", f);
    fputs("}\n\n", f);
}

static void write_rescue_continue(FILE *f, const char *indent) {
    fprintf(f, "%sdefault_target=$(systemctl get-default 2>/dev/null || true)\n", indent);
    fprintf(f, "%sif [ \"$default_target\" = \"multi-user.target\" ] || [ \"$default_target\" = \"runlevel2.target\" ] || [ \"$default_target\" = \"runlevel3.target\" ] || [ \"$default_target\" = \"runlevel4.target\" ]; then\n", indent);
    fprintf(f, "%s  systemctl default --no-block >/dev/null 2>&1 || true\n", indent);
    fprintf(f, "%s  systemd-run --on-active=5s /bin/sh -c 'systemctl restart systemd-update-utmp-runlevel.service >/dev/null 2>&1 || /lib/systemd/systemd-update-utmp runlevel >/dev/null 2>&1 || true' >/dev/null 2>&1 || true\n", indent);
    fprintf(f, "%selif systemctl -q is-active rescue.target 2>/dev/null; then\n", indent);
    fprintf(f, "%s  systemctl default --no-block >/dev/null 2>&1 || true\n", indent);
    fprintf(f, "%s  systemd-run --on-active=5s /bin/sh -c 'systemctl restart systemd-update-utmp-runlevel.service >/dev/null 2>&1 || /lib/systemd/systemd-update-utmp runlevel >/dev/null 2>&1 || true' >/dev/null 2>&1 || true\n", indent);
    fprintf(f, "%sfi\n", indent);
}

static void write_capture_checks(FILE *f, const TestCase *tc, const char *indent, const char *dir_expr) {
    if (command_check_count(test_command(tc)) > 0) return;
    int count = 0;
    const char *cursor = test_command(tc);
    bool used_legacy = false;
    CheckRule check;
    while (next_effective_check(tc, &cursor, &check, &used_legacy)) {
        const char *var = check.var[0] ? check.var : "AUTOTEST_ACTUAL";
        count++;
        fprintf(f, "%sprintf '%%s' \"${%s-}\" >\"%s/actual_%d\"\n", indent, var, dir_expr, count);
    }
}

static void write_validate_checks(FILE *f, const TestCase *tc, const char *indent, const char *dir_expr) {
    if (command_check_count(test_command(tc)) > 0) {
        fprintf(f, "%scheck_count=$(cat \"%s/check_count\" 2>/dev/null || echo 0)\n", indent, dir_expr);
        fprintf(f, "%si=1\n", indent);
        fprintf(f, "%swhile [ \"$i\" -le \"$check_count\" ]; do\n", indent);
        fprintf(f, "%s  check_var_i=$(cat \"%s/check_var_$i\" 2>/dev/null || true)\n", indent, dir_expr);
        fprintf(f, "%s  check_match_i=$(cat \"%s/check_match_$i\" 2>/dev/null || true)\n", indent, dir_expr);
        fprintf(f, "%s  expected_value_i=$(cat \"%s/expected_$i\" 2>/dev/null || true)\n", indent, dir_expr);
        fprintf(f, "%s  actual_value_i=$(cat \"%s/actual_$i\" 2>/dev/null || true)\n", indent, dir_expr);
        fprintf(f, "%s  printf -v \"check_var_$i\" '%%s' \"$check_var_i\"\n", indent);
        fprintf(f, "%s  printf -v \"check_match_$i\" '%%s' \"$check_match_i\"\n", indent);
        fprintf(f, "%s  printf -v \"expected_value_$i\" '%%s' \"$expected_value_i\"\n", indent);
        fprintf(f, "%s  printf -v \"actual_value_$i\" '%%s' \"$actual_value_i\"\n", indent);
        fprintf(f, "%s  if ! is_valid_var_name \"$check_var_i\"; then ok=0; fi\n", indent);
        fprintf(f, "%s  match_value \"$check_match_i\" \"$actual_value_i\" \"$expected_value_i\" || ok=0\n", indent);
        fprintf(f, "%s  i=$((i + 1))\n", indent);
        fprintf(f, "%sdone\n", indent);
        return;
    }
    int count = 0;
    const char *cursor = test_command(tc);
    bool used_legacy = false;
    CheckRule check;
    while (next_effective_check(tc, &cursor, &check, &used_legacy)) {
        const char *var = check.var[0] ? check.var : "AUTOTEST_ACTUAL";
        count++;
        fprintf(f, "%scheck_var_%d=", indent, count);
        shell_quote(f, var);
        fprintf(f, "\n");
        fprintf(f, "%scheck_match_%d=", indent, count);
        shell_quote(f, match_name(check.match));
        fprintf(f, "\n");
        fprintf(f, "%sexpected_value_%d=", indent, count);
        shell_quote(f, check.expected);
        fprintf(f, "\n");
        fprintf(f, "%sactual_value_%d=$(cat \"%s/actual_%d\" 2>/dev/null || true)\n", indent, count, dir_expr, count);
        fprintf(f, "%smatch_value %s \"$actual_value_%d\" ", indent, match_name(check.match), count);
        shell_quote(f, check.expected);
        fprintf(f, " || ok=0\n");
    }
    fprintf(f, "%scheck_count=%d\n", indent, count);
}

static void write_capture_tui_metadata(FILE *f, const char *indent, const char *dir_expr) {
    fprintf(f, "%sprintf '%%s' \"${AUTOTEST_TUI_TEXT_FILE-}\" >\"%s/tui_text_file\"\n", indent, dir_expr);
    fprintf(f, "%sprintf '%%s' \"${AUTOTEST_TUI_TRANSCRIPT_FILE-}\" >\"%s/tui_transcript_file\"\n", indent, dir_expr);
    fprintf(f, "%sprintf '%%s' \"${AUTOTEST_TUI_INPUT_FILE-}\" >\"%s/tui_input_file\"\n", indent, dir_expr);
}

static void write_load_tui_metadata(FILE *f, const char *indent, const char *dir_expr) {
    fprintf(f, "%sAUTOTEST_TUI_TEXT_FILE=$(cat \"%s/tui_text_file\" 2>/dev/null || true)\n", indent, dir_expr);
    fprintf(f, "%sAUTOTEST_TUI_TRANSCRIPT_FILE=$(cat \"%s/tui_transcript_file\" 2>/dev/null || true)\n", indent, dir_expr);
    fprintf(f, "%sAUTOTEST_TUI_INPUT_FILE=$(cat \"%s/tui_input_file\" 2>/dev/null || true)\n", indent, dir_expr);
}

static void write_validate_check_names(FILE *f, const TestCase *tc, const char *indent) {
    const char *cursor = test_command(tc);
    bool used_legacy = false;
    CheckRule check;
    while (next_effective_check(tc, &cursor, &check, &used_legacy)) {
        const char *var = check.var[0] ? check.var : "AUTOTEST_ACTUAL";
        fprintf(f, "%sif ! is_valid_var_name ", indent);
        shell_quote(f, var);
        fprintf(f, "; then status_msg='[NG] invalid check variable name'; autotest_print_status \"$status_msg\"; echo \"$status_msg\" >\"$RESULT_FILE\"; NG_COUNT=$((NG_COUNT + 1)); return; fi\n");
    }
}

static void write_case(FILE *f, const TestCase *tc, int index) {
    char ok_msg[LONG_LEN];
    char ng_prefix[LONG_LEN];
    char safe_id[64];
    snprintf(ok_msg, sizeof(ok_msg), "[OK] %s %s", tc->id, tc->title);
    snprintf(ng_prefix, sizeof(ng_prefix), "[NG] %s %s expected_exit=%d actual_exit=", tc->id, tc->title, tc->expected_exit);
    make_safe_id(tc->id, safe_id, sizeof(safe_id), '-');

    fprintf(f, "run_case_%03d() {\n", index + 1);
    fprintf(f, "  TOTAL_COUNT=$((TOTAL_COUNT + 1))\n");
    fprintf(f, "  expected_exit=%d\n", tc->expected_exit);
    if (tc->kind == CMD_REBOOT) {
        fprintf(f, "  mode=\"${1:-run}\"\n");
        fprintf(f, "  state_dir=\"/var/tmp/autotest-%s-state\"\n", safe_id);
        fprintf(f, "  unit_name=\"autotest-%s-resume.service\"\n", safe_id);
        fprintf(f, "  stdout_file=\"$state_dir/stdout\"\n");
        fprintf(f, "  stderr_file=\"$state_dir/stderr\"\n");
        fprintf(f, "  actual_dir=\"$state_dir/actual\"\n");
        fprintf(f, "  AUTOTEST_BACKUP_DIR=\"$state_dir/backups\"\n");
        fprintf(f, "  finish_reboot_case() {\n");
        fprintf(f, "    actual_exit=\"$1\"\n");
        fprintf(f, "    ok=1\n");
        fprintf(f, "    [ \"$actual_exit\" -eq %d ] || ok=0\n", tc->expected_exit);
        write_validate_checks(f, tc, "    ", "$actual_dir");
        write_load_tui_metadata(f, "    ", "$actual_dir");
        fprintf(f, "    systemctl disable \"$unit_name\" >/dev/null 2>&1 || true\n");
        fprintf(f, "    rm -f \"/etc/systemd/system/$unit_name\"\n");
        fprintf(f, "    systemctl daemon-reload >/dev/null 2>&1 || true\n");
        if (tc->cleanup[0]) fprintf(f, "    %s || true\n", tc->cleanup);
        fprintf(f, "    if [ \"$ok\" -eq 1 ]; then\n");
        fprintf(f, "      status_msg=");
        shell_quote(f, ok_msg);
        fprintf(f, "\n");
        fprintf(f, "      autotest_print_status \"$status_msg\"\n");
        fprintf(f, "      OK_COUNT=$((OK_COUNT + 1))\n");
        fprintf(f, "    else\n");
        fprintf(f, "      status_msg=");
        shell_quote(f, ng_prefix);
        fprintf(f, "\"$actual_exit\"\n");
        fprintf(f, "      autotest_print_status \"$status_msg\"\n");
        fprintf(f, "      NG_COUNT=$((NG_COUNT + 1))\n");
        fprintf(f, "    fi\n");
        fprintf(f, "    {\n");
        fprintf(f, "      echo \"$status_msg\"\n");
        fprintf(f, "      echo \"Summary: total=$TOTAL_COUNT OK=$OK_COUNT NG=$NG_COUNT\"\n");
        fprintf(f, "      echo \"check_count=$check_count\"\n");
        fprintf(f, "      for i in $(seq 1 \"$check_count\"); do name_ref=\"check_var_$i\"; value_ref=\"actual_value_$i\"; name=\"${!name_ref}\"; value=\"${!value_ref}\"; echo \"check_$i=$name\"; echo \"actual_$i=$value\"; done\n");
        fprintf(f, "      echo \"actual_dir=$actual_dir\"\n");
        fprintf(f, "      echo \"stdout_file=$stdout_file\"\n");
        fprintf(f, "      echo \"stderr_file=$stderr_file\"\n");
        fprintf(f, "      echo '--- stdout ---'\n");
        fprintf(f, "      [ -f \"$stdout_file\" ] && cat \"$stdout_file\"\n");
        fprintf(f, "      echo '--- stderr ---'\n");
        fprintf(f, "      [ -f \"$stderr_file\" ] && cat \"$stderr_file\"\n");
        fprintf(f, "    } >\"$RESULT_FILE\"\n");
        fprintf(f, "    autotest_write_detail_outputs\n");
        fprintf(f, "    rm -rf \"$state_dir\"\n");
        write_rescue_continue(f, "    ");
        fprintf(f, "  }\n");
        fprintf(f, "  mkdir -p \"$state_dir\"\n");
        fprintf(f, "  mkdir -p \"$actual_dir\"\n");
        fprintf(f, "  mkdir -p \"$AUTOTEST_BACKUP_DIR\"\n");
        fprintf(f, "  if [ \"$mode\" = \"--resume\" ]; then\n");
        fprintf(f, "    [ -f \"$state_dir/detail_result_path\" ] && DETAIL_RESULT_FILE=$(cat \"$state_dir/detail_result_path\" 2>/dev/null || true)\n");
        fprintf(f, "    [ -f \"$state_dir/detail_stdout\" ] && DETAIL_STDOUT=$(cat \"$state_dir/detail_stdout\" 2>/dev/null || echo 0)\n");
        fprintf(f, "  elif [ -n \"$DETAIL_RESULT_FILE\" ]; then\n");
        fprintf(f, "    printf '%%s\\n' \"$DETAIL_RESULT_FILE\" >\"$state_dir/detail_result_path\"\n");
        fprintf(f, "  fi\n");
        fprintf(f, "  if [ \"$mode\" != \"--resume\" ] && [ \"${DETAIL_STDOUT:-0}\" = 1 ]; then printf '1\\n' >\"$state_dir/detail_stdout\"; fi\n");
        fprintf(f, "  cat >\"$state_dir/pre.sh\" <<'AUTOTEST_PRE'\n");
        write_command_phase(f, test_command(tc), false);
        fprintf(f, "AUTOTEST_PRE\n");
        fprintf(f, "  cat >\"$state_dir/reboot.sh\" <<'AUTOTEST_REBOOT'\n");
        write_reboot_command(f, test_command(tc));
        fprintf(f, "AUTOTEST_REBOOT\n");
        fprintf(f, "  cat >\"$state_dir/post.sh\" <<'AUTOTEST_POST'\n");
        write_command_phase(f, test_command(tc), true);
        fprintf(f, "AUTOTEST_POST\n");
        fprintf(f, "  chmod 700 \"$state_dir/pre.sh\" \"$state_dir/reboot.sh\" \"$state_dir/post.sh\"\n");
        write_validate_check_names(f, tc, "  ");
        fprintf(f, "  if [ \"$mode\" = \"--resume\" ]; then\n");
        fprintf(f, "    ( set -a; AUTOTEST_CHECK_INDEX=$(cat \"$actual_dir/check_count\" 2>/dev/null || echo 0); [ -f \"$state_dir/env.sh\" ] && . \"$state_dir/env.sh\"; . \"$state_dir/post.sh\"; post_rc=\"$?\"; ");
        write_capture_checks(f, tc, "", "$actual_dir");
        write_capture_tui_metadata(f, "", "$actual_dir");
        fprintf(f, "exit \"$post_rc\" ) >>\"$stdout_file\" 2>>\"$stderr_file\"\n");
        fprintf(f, "    finish_reboot_case \"$?\"\n");
        fprintf(f, "    return\n");
        fprintf(f, "  fi\n");
        fprintf(f, "  : >\"$stdout_file\"\n");
        fprintf(f, "  : >\"$stderr_file\"\n");
        fprintf(f, "  ( set -a; AUTOTEST_CHECK_INDEX=$(cat \"$actual_dir/check_count\" 2>/dev/null || echo 0); . \"$state_dir/pre.sh\"; pre_rc=\"$?\"; ");
        write_capture_checks(f, tc, "", "$actual_dir");
        write_capture_tui_metadata(f, "", "$actual_dir");
        fprintf(f, "export -p >\"$state_dir/env.sh\"; exit \"$pre_rc\" ) >>\"$stdout_file\" 2>>\"$stderr_file\"\n");
        fprintf(f, "  pre_exit=\"$?\"\n");
        fprintf(f, "  if [ \"$pre_exit\" -ne 0 ]; then finish_reboot_case \"$pre_exit\"; return; fi\n");
        fprintf(f, "  if [ \"$(id -u)\" -ne 0 ]; then status_msg='[NG] root required for reboot test resume'; autotest_print_status \"$status_msg\"; echo \"$status_msg\" >\"$RESULT_FILE\"; NG_COUNT=$((NG_COUNT + 1)); return; fi\n");
        fprintf(f, "  self_path=\"$(readlink -f \"$0\")\"\n");
        fprintf(f, "  cat >\"/etc/systemd/system/$unit_name\" <<AUTOTEST_UNIT\n");
        fprintf(f, "[Unit]\nDescription=AutoTest %s resume\nDefaultDependencies=no\nAfter=sysinit.target\nBefore=rescue.service\nBefore=shutdown.target\nConflicts=shutdown.target\n\n", safe_id);
        fprintf(f, "[Service]\nType=oneshot\nExecStart=/bin/bash \"$self_path\" --resume\n\n[Install]\nWantedBy=multi-user.target rescue.target\n");
        fprintf(f, "AUTOTEST_UNIT\n");
        fprintf(f, "  systemctl daemon-reload\n");
        fprintf(f, "  systemctl enable \"$unit_name\"\n");
        fprintf(f, "  bash \"$state_dir/reboot.sh\" >>\"$stdout_file\" 2>>\"$stderr_file\"\n");
        fprintf(f, "  reboot_exit=\"$?\"\n");
        fprintf(f, "  if [ \"$reboot_exit\" -eq 0 ]; then status_msg='[PENDING] Reboot command issued. Result check will resume after boot.'; actual_exit=0; ok=0; check_count=0; ");
        write_load_tui_metadata(f, "", "$actual_dir");
        fprintf(f, "echo \"$status_msg\" >\"$RESULT_FILE\"; autotest_write_detail_outputs; echo 'Reboot command issued. Result check will resume after boot.'; exit 0; fi\n");
        fprintf(f, "  if [ \"$reboot_exit\" -eq 125 ]; then\n");
        fprintf(f, "    ( set -a; AUTOTEST_CHECK_INDEX=$(cat \"$actual_dir/check_count\" 2>/dev/null || echo 0); [ -f \"$state_dir/env.sh\" ] && . \"$state_dir/env.sh\"; . \"$state_dir/post.sh\"; post_rc=\"$?\"; ");
        write_capture_checks(f, tc, "", "$actual_dir");
        write_capture_tui_metadata(f, "", "$actual_dir");
        fprintf(f, "exit \"$post_rc\" ) >>\"$stdout_file\" 2>>\"$stderr_file\"\n");
        fprintf(f, "    finish_reboot_case \"$?\"\n");
        fprintf(f, "    return\n");
        fprintf(f, "  fi\n");
        fprintf(f, "  finish_reboot_case \"$reboot_exit\"\n");
        fprintf(f, "}\n\n");
        return;
    }
    fprintf(f, "  stdout_file=\"$WORK_DIR/case_%03d.stdout\"\n", index + 1);
    fprintf(f, "  stderr_file=\"$WORK_DIR/case_%03d.stderr\"\n", index + 1);
    fprintf(f, "  actual_dir=\"$WORK_DIR/case_%03d.actual\"\n", index + 1);
    fprintf(f, "  mkdir -p \"$actual_dir\"\n");
    fprintf(f, "  body_file=\"$WORK_DIR/case_%03d.body.sh\"\n", index + 1);
    fprintf(f, "  cat >\"$body_file\" <<'AUTOTEST_BODY_%03d'\n", index + 1);
    write_command_expanded(f, test_command(tc));
    fprintf(f, "AUTOTEST_BODY_%03d\n", index + 1);
    write_validate_check_names(f, tc, "  ");
    fprintf(f, "  ( set -a; AUTOTEST_CHECK_INDEX=$(cat \"$actual_dir/check_count\" 2>/dev/null || echo 0); . \"$body_file\"; body_rc=\"$?\"; ");
    write_capture_checks(f, tc, "", "$actual_dir");
    write_capture_tui_metadata(f, "", "$actual_dir");
    fprintf(f, "exit \"$body_rc\" ) >\"$stdout_file\" 2>\"$stderr_file\"\n");
    fprintf(f, "  actual_exit=$?\n");
    fprintf(f, "  ok=1\n");
    fprintf(f, "  [ \"$actual_exit\" -eq %d ] || ok=0\n", tc->expected_exit);
    write_validate_checks(f, tc, "  ", "$actual_dir");
    write_load_tui_metadata(f, "  ", "$actual_dir");
    fprintf(f, "  if [ \"$ok\" -eq 1 ]; then\n");
    fprintf(f, "    status_msg=");
    shell_quote(f, ok_msg);
    fprintf(f, "\n");
    fprintf(f, "    autotest_print_status \"$status_msg\"\n");
    fprintf(f, "    OK_COUNT=$((OK_COUNT + 1))\n");
    fprintf(f, "  else\n");
    fprintf(f, "    status_msg=");
    shell_quote(f, ng_prefix);
    fprintf(f, "\"$actual_exit\"\n");
    fprintf(f, "    autotest_print_status \"$status_msg\"\n");
    fprintf(f, "    NG_COUNT=$((NG_COUNT + 1))\n");
    fprintf(f, "  fi\n");
    fprintf(f, "  autotest_write_detail_outputs\n");
    if (tc->cleanup[0]) fprintf(f, "  %s || true\n", tc->cleanup);
    fprintf(f, "}\n\n");
}

static void write_summary(FILE *f) {
    fprintf(f, "echo \"Summary: total=$TOTAL_COUNT OK=$OK_COUNT NG=$NG_COUNT\"\n");
    fprintf(f, "if [ \"$NG_COUNT\" -eq 0 ]; then exit 0; fi\n");
    fprintf(f, "exit 1\n");
}

static int save_selected_test_scripts(App *app) {
    int failures = 0;
    for (int i = 0; i < app->project.case_count; i++) {
        TestCase *tc = &app->project.cases[i];
        if (!tc->selected) continue;
        if (write_single_test_script(tc) != 0) failures++;
    }
    if (failures == 0) save_test_registry(&app->project);
    return failures == 0 ? 0 : -1;
}

static int first_selected_reboot_index(App *app) {
    for (int i = 0; i < app->project.case_count; i++) {
        TestCase *tc = &app->project.cases[i];
        if (!tc->selected) continue;
        auto_configure_test(tc);
        if (tc->kind == CMD_REBOOT) return i;
    }
    return -1;
}

static int ensure_selected_test_scripts(App *app) {
    for (int i = 0; i < app->project.case_count; i++) {
        TestCase *tc = &app->project.cases[i];
        if (!tc->selected) continue;
        if (!tc->script_path[0] && write_single_test_script(tc) != 0) return -1;
    }
    save_test_registry(&app->project);
    return 0;
}

static int write_resume_automation(App *app, char *runner_path, size_t runner_sz) {
    char cwd[LONG_LEN];
    char resume_path[LONG_LEN];
    char unit_path[LONG_LEN];
    char result_path[LONG_LEN];
    int reboot_idx = first_selected_reboot_index(app);
    if (reboot_idx < 0) return -1;
    if (ensure_selected_test_scripts(app) != 0) return -1;
    if (!getcwd(cwd, sizeof(cwd))) return -1;
    if (strlen(cwd) + 64 >= LONG_LEN) return -1;
    if (strchr(cwd, ' ') != NULL) return -1;
    snprintf(runner_path, runner_sz, "%s/autotest_selected_runner.sh", cwd);
    snprintf(resume_path, sizeof(resume_path), "%s/autotest_selected_resume.sh", cwd);
    snprintf(unit_path, sizeof(unit_path), "%s/autotest-selected-resume.service", cwd);
    snprintf(result_path, sizeof(result_path), "%s/autotest_selected.result", cwd);

    FILE *f = fopen(resume_path, "w");
    if (!f) return -1;
    fprintf(f, "#!/usr/bin/env bash\nset -u\n");
    fprintf(f, "STATE_DIR=/var/tmp/autotest-selected-state\n");
    fprintf(f, "UNIT_NAME=autotest-selected-resume.service\n");
    fprintf(f, "INDEX_FILE=\"$STATE_DIR/index\"\n");
    fprintf(f, "FAIL_FILE=\"$STATE_DIR/failures\"\n");
    fprintf(f, "WAIT_SAFE_FILE=\"$STATE_DIR/waiting_safe_id\"\n");
    fprintf(f, "WAIT_RESULT_FILE=\"$STATE_DIR/waiting_result\"\n");
    fprintf(f, "SUMMARY_PATH=");
    shell_quote_str(f, result_path);
    fprintf(f, "\n");
    fprintf(f, "STOP_ON_FAILURE=%d\n", app->stop_on_failure ? 1 : 0);
    fprintf(f, "ids=(");
    for (int i = 0; i < app->project.case_count; i++) {
        TestCase *tc = &app->project.cases[i];
        if (!tc->selected) continue;
        shell_quote_str(f, tc->id);
        fprintf(f, " ");
    }
    fprintf(f, ")\n");
    fprintf(f, "paths=(");
    for (int i = 0; i < app->project.case_count; i++) {
        TestCase *tc = &app->project.cases[i];
        if (!tc->selected) continue;
        shell_quote_str(f, tc->script_path);
        fprintf(f, " ");
    }
    fprintf(f, ")\n");
    fprintf(f, "kinds=(");
    for (int i = 0; i < app->project.case_count; i++) {
        TestCase *tc = &app->project.cases[i];
        if (!tc->selected) continue;
        auto_configure_test(tc);
        shell_quote_str(f, tc->kind == CMD_REBOOT ? "reboot" : "shell");
        fprintf(f, " ");
    }
    fprintf(f, ")\n");
    fprintf(f, "safe_ids=(");
    for (int i = 0; i < app->project.case_count; i++) {
        TestCase *tc = &app->project.cases[i];
        char safe_id[64];
        if (!tc->selected) continue;
        make_safe_id(tc->id, safe_id, sizeof(safe_id), '-');
        shell_quote_str(f, safe_id);
        fprintf(f, " ");
    }
    fprintf(f, ")\n");
    fprintf(f, "count=${#paths[@]}\n");
    fprintf(f, "log() { echo \"$*\" | tee -a \"$SUMMARY_PATH\"; }\n");
    fprintf(f, "read_num() { if [ -f \"$1\" ]; then cat \"$1\"; else echo 0; fi; }\n");
    fprintf(f, "write_failures() { echo \"$1\" >\"$FAIL_FILE\"; }\n");
    fprintf(f, "cleanup_selected_runner() {\n");
    fprintf(f, "  systemctl disable \"$UNIT_NAME\" >/dev/null 2>&1 || true\n");
    fprintf(f, "  rm -f \"/etc/systemd/system/$UNIT_NAME\"\n");
    fprintf(f, "  systemctl daemon-reload >/dev/null 2>&1 || true\n");
    fprintf(f, "  rm -rf \"$STATE_DIR\"\n");
    write_rescue_continue(f, "  ");
    fprintf(f, "}\n");
    fprintf(f, "stop_after_failure_if_needed() {\n");
    fprintf(f, "  failures=$(read_num \"$FAIL_FILE\")\n");
    fprintf(f, "  if [ \"$STOP_ON_FAILURE\" = 1 ] && [ \"$failures\" -gt 0 ]; then\n");
    fprintf(f, "    log \"[NG] selected test sequence stopped after failure failures=$failures\"\n");
    fprintf(f, "    cleanup_selected_runner\n");
    fprintf(f, "    exit 1\n");
    fprintf(f, "  fi\n");
    fprintf(f, "}\n");
    fprintf(f, "check_waiting_reboot() {\n");
    fprintf(f, "  [ -f \"$WAIT_SAFE_FILE\" ] || return 0\n");
    fprintf(f, "  safe_id=$(cat \"$WAIT_SAFE_FILE\")\n");
    fprintf(f, "  result_file=$(cat \"$WAIT_RESULT_FILE\" 2>/dev/null || true)\n");
    fprintf(f, "  log \"Waiting for reboot test $safe_id to finish.\"\n");
    fprintf(f, "  for n in $(seq 1 600); do [ ! -d \"/var/tmp/autotest-${safe_id}-state\" ] && break; sleep 1; done\n");
    fprintf(f, "  failures=$(read_num \"$FAIL_FILE\")\n");
    fprintf(f, "  if [ -d \"/var/tmp/autotest-${safe_id}-state\" ]; then\n");
    fprintf(f, "    log \"[NG] reboot test $safe_id did not finish after boot\"\n");
    fprintf(f, "    failures=$((failures + 1))\n");
    fprintf(f, "  elif [ -n \"$result_file\" ] && [ -f \"$result_file\" ]; then\n");
    fprintf(f, "    if grep -q '^\\[NG\\]' \"$result_file\"; then failures=$((failures + 1)); fi\n");
    fprintf(f, "    log \"result: $result_file\"\n");
    fprintf(f, "    cat \"$result_file\" >>\"$SUMMARY_PATH\"\n");
    fprintf(f, "    echo >>\"$SUMMARY_PATH\"\n");
    fprintf(f, "    rm -f \"$result_file\"\n");
    fprintf(f, "  else\n");
    fprintf(f, "    log \"[NG] reboot test $safe_id result file was not found\"\n");
    fprintf(f, "    failures=$((failures + 1))\n");
    fprintf(f, "  fi\n");
    fprintf(f, "  write_failures \"$failures\"\n");
    fprintf(f, "  rm -f \"$WAIT_SAFE_FILE\" \"$WAIT_RESULT_FILE\"\n");
    fprintf(f, "}\n");
    fprintf(f, "mkdir -p \"$STATE_DIR\"\n");
    fprintf(f, "touch \"$SUMMARY_PATH\"\n");
    fprintf(f, "log \"stop_on_ng=$([ \"$STOP_ON_FAILURE\" = 1 ] && echo yes || echo no)\"\n");
    fprintf(f, "check_waiting_reboot\n");
    fprintf(f, "stop_after_failure_if_needed\n");
    fprintf(f, "idx=$(read_num \"$INDEX_FILE\")\n");
    fprintf(f, "failures=$(read_num \"$FAIL_FILE\")\n");
    fprintf(f, "while [ \"$idx\" -lt \"$count\" ]; do\n");
    fprintf(f, "  id=${ids[$idx]}\n");
    fprintf(f, "  path=${paths[$idx]}\n");
    fprintf(f, "  kind=${kinds[$idx]}\n");
    fprintf(f, "  safe_id=${safe_ids[$idx]}\n");
    fprintf(f, "  result_file=\"${path%%.sh}.result\"\n");
    fprintf(f, "  log \"[$((idx + 1))/$count] $id $path\"\n");
    fprintf(f, "  if [ \"$kind\" = \"reboot\" ]; then\n");
    fprintf(f, "    echo $((idx + 1)) >\"$INDEX_FILE\"\n");
    fprintf(f, "    echo \"$safe_id\" >\"$WAIT_SAFE_FILE\"\n");
    fprintf(f, "    echo \"$result_file\" >\"$WAIT_RESULT_FILE\"\n");
    fprintf(f, "    bash \"$path\"\n");
    fprintf(f, "    rc=$?\n");
    fprintf(f, "    if [ \"$rc\" -ne 0 ]; then\n");
    fprintf(f, "      log \"[NG] $id failed before reboot rc=$rc\"\n");
    fprintf(f, "      if [ -f \"$result_file\" ]; then cat \"$result_file\" >>\"$SUMMARY_PATH\"; echo >>\"$SUMMARY_PATH\"; rm -f \"$result_file\"; fi\n");
    fprintf(f, "      failures=$((failures + 1))\n");
    fprintf(f, "      write_failures \"$failures\"\n");
    fprintf(f, "      rm -f \"$WAIT_SAFE_FILE\" \"$WAIT_RESULT_FILE\"\n");
    fprintf(f, "      stop_after_failure_if_needed\n");
    fprintf(f, "      idx=$((idx + 1))\n");
    fprintf(f, "      echo \"$idx\" >\"$INDEX_FILE\"\n");
    fprintf(f, "      continue\n");
    fprintf(f, "    fi\n");
    fprintf(f, "    if grep -q '^\\[PENDING\\]' \"$result_file\" 2>/dev/null; then\n");
    fprintf(f, "      log \"[PENDING] $id reboot issued; selected tests continue after boot.\"\n");
    fprintf(f, "      write_failures \"$failures\"\n");
    fprintf(f, "      exit 0\n");
    fprintf(f, "    fi\n");
    fprintf(f, "    rm -f \"$WAIT_SAFE_FILE\" \"$WAIT_RESULT_FILE\"\n");
    fprintf(f, "    if grep -q '^\\[NG\\]' \"$result_file\" 2>/dev/null; then failures=$((failures + 1)); fi\n");
    fprintf(f, "    if [ -f \"$result_file\" ]; then cat \"$result_file\" >>\"$SUMMARY_PATH\"; echo >>\"$SUMMARY_PATH\"; rm -f \"$result_file\"; fi\n");
    fprintf(f, "    write_failures \"$failures\"\n");
    fprintf(f, "    stop_after_failure_if_needed\n");
    fprintf(f, "  else\n");
    fprintf(f, "    bash \"$path\"\n");
    fprintf(f, "    rc=$?\n");
    fprintf(f, "    if [ \"$rc\" -ne 0 ]; then failures=$((failures + 1)); write_failures \"$failures\"; fi\n");
    fprintf(f, "    if [ -f \"$result_file\" ]; then cat \"$result_file\" >>\"$SUMMARY_PATH\"; echo >>\"$SUMMARY_PATH\"; rm -f \"$result_file\"; fi\n");
    fprintf(f, "    stop_after_failure_if_needed\n");
    fprintf(f, "  fi\n");
    fprintf(f, "  idx=$((idx + 1))\n");
    fprintf(f, "  echo \"$idx\" >\"$INDEX_FILE\"\n");
    fprintf(f, "done\n");
    fprintf(f, "if [ \"$failures\" -eq 0 ]; then log '[OK] selected test sequence'; else log \"[NG] selected test sequence failures=$failures\"; fi\n");
    fprintf(f, "cleanup_selected_runner\n");
    fprintf(f, "if [ \"$failures\" -eq 0 ]; then exit 0; fi\n");
    fprintf(f, "exit 1\n");
    fclose(f);
    chmod(resume_path, 0755);

    f = fopen(unit_path, "w");
    if (!f) return -1;
    fprintf(f, "[Unit]\nDescription=AutoTest selected resume\nDefaultDependencies=no\nAfter=sysinit.target\nBefore=rescue.service\nBefore=shutdown.target\nConflicts=shutdown.target\n\n");
    fprintf(f, "[Service]\nType=oneshot\nExecStart=%s\n\n[Install]\nWantedBy=multi-user.target rescue.target\n", resume_path);
    fclose(f);

    f = fopen(runner_path, "w");
    if (!f) return -1;
    fprintf(f, "#!/usr/bin/env bash\nset -u\n");
    fprintf(f, "echo 'Preparing reboot resume automation.'\n");
    fprintf(f, "if [ \"$(id -u)\" -ne 0 ]; then echo 'NG root required for reboot resume automation'; exit 1; fi\n");
    fprintf(f, "STATE_DIR=/var/tmp/autotest-selected-state\n");
    fprintf(f, "rm -rf \"$STATE_DIR\"\n");
    fprintf(f, "mkdir -p \"$STATE_DIR\"\n");
    fprintf(f, "echo 0 >\"$STATE_DIR/index\"\n");
    fprintf(f, "echo 0 >\"$STATE_DIR/failures\"\n");
    fprintf(f, ": >");
    shell_quote_str(f, result_path);
    fprintf(f, "\n");
    fprintf(f, "cp ");
    shell_quote_str(f, unit_path);
    fprintf(f, " /etc/systemd/system/autotest-selected-resume.service\n");
    fprintf(f, "systemctl daemon-reload\n");
    fprintf(f, "systemctl enable autotest-selected-resume.service\n");
    fprintf(f, "exec ");
    shell_quote_str(f, resume_path);
    fprintf(f, "\n");
    fclose(f);
    chmod(runner_path, 0755);
    return 0;
}

static int system_exit_code(int rc) {
    if (rc == -1) return 127;
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    if (WIFSIGNALED(rc)) return 128 + WTERMSIG(rc);
    return 127;
}

static void append_file_to_stream(FILE *out, const char *path) {
    FILE *in = fopen(path, "r");
    if (!in) return;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        fwrite(buf, 1, n, out);
    }
    fclose(in);
}

static void start_selected_test_scripts(App *app) {
    def_prog_mode();
    endwin();
    printf("\nStarting selected automated tests sequentially.\n\n");
    int reboot_idx = first_selected_reboot_index(app);
    if (reboot_idx >= 0) {
        char runner_path[LONG_LEN];
        if (write_resume_automation(app, runner_path, sizeof(runner_path)) == 0) {
            char quoted[LONG_LEN + 16];
            shell_quote_buf(quoted, sizeof(quoted), runner_path);
            printf("Reboot-capable runner generated:\n%s\n\n", runner_path);
            int rc = system(quoted);
            printf("\nRunner finished with status %d. If reboot happened, remaining tests will continue after boot.\n", rc);
        } else {
            printf("[NG] failed to create reboot resume automation\n");
        }
        printf("Press Enter to return to TUI.");
        fflush(stdout);
        (void)getchar();
        reset_prog_mode();
        refresh();
        return;
    }
    char selected_result[LONG_LEN];
    char output_path[LONG_LEN];
    FILE *result = NULL;
    int total = 0;
    int ok_count = 0;
    int ng_count = 0;
    if (selected_result_path(selected_result, sizeof(selected_result))) {
        result = fopen(selected_result, "w");
    }
    if (result) {
        fprintf(result, "AutoTest selected result\n");
        fprintf(result, "cwd=%s\n\n", getcwd(output_path, sizeof(output_path)) ? output_path : ".");
        fprintf(result, "stop_on_ng=%s\n\n", app->stop_on_failure ? "yes" : "no");
    }
    int index = 1;
    for (int i = 0; i < app->project.case_count; i++) {
        TestCase *tc = &app->project.cases[i];
        if (!tc->selected) continue;
        total++;
        if (!tc->script_path[0]) {
            if (write_single_test_script(tc) != 0) {
                printf("[NG] %s %s failed to create test script\n", tc->id, tc->title);
                if (result) fprintf(result, "[NG] %s %s failed to create test script\n\n", tc->id, tc->title);
                ng_count++;
                if (app->stop_on_failure) {
                    printf("Stopping selected tests after failure.\n");
                    if (result) fprintf(result, "Stopped after failure.\n\n");
                    break;
                }
                continue;
            }
        }
        char quoted[LONG_LEN + 16];
        char output_quoted[LONG_LEN + 16];
        char cmd[LONG_LEN * 2 + 64];
        char result_path[LONG_LEN];
        shell_quote_buf(quoted, sizeof(quoted), tc->script_path);
        make_result_path(tc->script_path, result_path, sizeof(result_path));
        unlink(result_path);
        const char *tmpdir = getenv("TMPDIR");
        if (!tmpdir || !tmpdir[0]) tmpdir = "/tmp";
        snprintf(output_path, sizeof(output_path), "%s/autotest-selected-output.%d.%d", tmpdir, (int)getpid(), index);
        shell_quote_buf(output_quoted, sizeof(output_quoted), output_path);
        snprintf(cmd, sizeof(cmd), "%s >%s 2>&1", quoted, output_quoted);
        printf("[%02d] %s %s\n%s\n", index++, tc->id, tc->title, tc->script_path);
        int rc = system(cmd);
        int exit_code = system_exit_code(rc);
        append_file_to_stream(stdout, output_path);
        printf("exit=%d\n\n", exit_code);
        if (result) {
            fprintf(result, "[%02d] %s %s\n", index - 1, tc->id, tc->title);
            fprintf(result, "script=%s\n", tc->script_path);
            fprintf(result, "exit=%d\n", exit_code);
            append_file_to_stream(result, output_path);
            fprintf(result, "\n");
        }
        if (exit_code == 0) ok_count++; else ng_count++;
        unlink(output_path);
        if (result_path[0] && access(result_path, F_OK) == 0) {
            if (result) {
                fprintf(result, "individual_result=%s\n", result_path);
                append_file_to_stream(result, result_path);
                fprintf(result, "\n");
            }
            unlink(result_path);
        }
        if (exit_code != 0 && app->stop_on_failure) {
            printf("Stopping selected tests after failure.\n");
            if (result) fprintf(result, "Stopped after failure.\n\n");
            break;
        }
        if (tc->kind == CMD_REBOOT) {
            printf("Reboot test was executed. If the machine rebooted, run remaining tests after boot.\n");
            break;
        }
    }
    if (result) {
        fprintf(result, "Summary: total=%d OK=%d NG=%d\n", total, ok_count, ng_count);
        fclose(result);
        printf("Selected result: %s\n", selected_result);
    } else {
        printf("[NG] failed to write selected result file\n");
    }
    printf("Automated test sequence finished. Press Enter to return to TUI.");
    fflush(stdout);
    (void)getchar();
    reset_prog_mode();
    refresh();
}

static void edit_case_filter(App *app) {
    char buf[TEXT_LEN];
    copy_text(buf, sizeof(buf), app->case_filter);
    if (!prompt_text("filter tests", buf, sizeof(buf))) {
        set_status(app, "Filter unchanged.");
        return;
    }
    copy_text(app->case_filter, sizeof(app->case_filter), buf);
    clamp_selected(app);
    if (!app->case_filter[0]) {
        set_status(app, "Filter cleared.");
    } else if (filtered_case_count(app) == 0) {
        set_status(app, "Filter applied. No matching tests.");
    } else {
        select_first_filtered_case(app);
        set_status(app, "Filter applied.");
    }
}

static void run_selected_menu(App *app) {
    switch (app->screen) {
    case SCREEN_DASHBOARD:
        if (app->selected_menu == 0) { app->screen = SCREEN_EDITOR; app->selected_menu = 0; }
        else if (app->selected_menu == 1) add_case(app);
        else if (app->selected_menu == 2) {
            if (selected_count(&app->project) == 0) set_status(app, "Select at least one test before starting.");
            else { app->selected_only = true; app->run_after_generate = true; app->confirm_action = CONFIRM_GENERATE; app->previous_screen = app->screen; app->screen = SCREEN_CONFIRM; app->selected_menu = 0; }
        }
        else set_status(app, "Use menu items with Enter. Esc exits.");
        break;
    case SCREEN_EDITOR: {
        bool has_visible_case = app->project.case_count > 0 && filtered_case_count(app) > 0;
        if (app->selected_menu == 0 && has_visible_case) { app->editor_new_case = false; open_script_editor(app); }
        else if (app->selected_menu == 1 && has_visible_case) open_text_editor(app, EDIT_TARGET_DESCRIPTION, SCREEN_EDITOR);
        else if (app->selected_menu == 2 && has_visible_case) rename_case(app);
        else if (app->selected_menu == 3 && has_visible_case) copy_case(app);
        else if (app->selected_menu == 4 && has_visible_case) { app->project.cases[app->selected_case].selected = !app->project.cases[app->selected_case].selected; save_test_registry(&app->project); }
        else if (app->selected_menu == 5 && has_visible_case) { app->confirm_action = CONFIRM_DELETE; app->previous_screen = app->screen; app->screen = SCREEN_CONFIRM; app->selected_menu = 0; }
        else if (app->selected_menu == 6 && has_visible_case) print_current_test_script(app);
        else if (app->selected_menu == 7) edit_case_filter(app);
        else if (app->selected_menu == 8) {
            if (selected_count(&app->project) == 0) set_status(app, "Select at least one test before starting.");
            else { app->selected_only = true; app->run_after_generate = true; app->confirm_action = CONFIRM_GENERATE; app->previous_screen = app->screen; app->screen = SCREEN_CONFIRM; app->selected_menu = 0; }
        }
        else if (app->selected_menu == 9) { app->screen = SCREEN_DASHBOARD; app->selected_menu = 0; }
        else if (!has_visible_case && app->selected_menu <= 6) set_status(app, "No visible test for current filter.");
        break;
    }
    case SCREEN_FORM:
        if (app->selected_menu == 0 || app->selected_menu == 1) edit_selected_field(app);
        else if (app->selected_menu == 2 && app->project.case_count) {
            TestCase *tc = &app->project.cases[app->selected_case];
            char command[LONG_LEN];
            copy_text(command, sizeof(command), test_command(tc));
            if (prompt_text("shell command", command, sizeof(command))) {
                set_test_command(tc, command);
                tc->kind = CMD_SHELL;
            }
        }
        else if (app->selected_menu == 3 && app->project.case_count) set_reboot_command(&app->project.cases[app->selected_case]);
        else if (app->selected_menu == 4 && app->project.case_count) set_vim_command(&app->project.cases[app->selected_case]);
        else if (app->selected_menu == 5) {
            int rc = write_single_test_script(&app->project.cases[app->selected_case]);
            if (rc == 0) {
                save_test_registry(&app->project);
                app->screen = SCREEN_EDITOR;
                app->selected_menu = 0;
                set_status(app, "Saved test.");
            } else if (rc == -2) {
                set_status(app, "Script name already exists in this directory.");
            } else {
                set_status(app, "Failed to save test.");
            }
        }
        else if (app->selected_menu == 6) { app->screen = SCREEN_EDITOR; app->selected_menu = 0; }
        break;
    case SCREEN_MATCH:
        app->screen = SCREEN_EDITOR;
        app->selected_menu = 0;
        break;
    case SCREEN_CLEANUP:
        if (app->selected_menu == 0) {
            if (app->project.cleanup_count < MAX_CLEANUPS) {
                char buf[LONG_LEN] = "true";
                if (prompt_text("cleanup command", buf, sizeof(buf))) {
                    snprintf(app->project.cleanups[app->project.cleanup_count++], LONG_LEN, "%s", buf);
                    app->selected_cleanup = app->project.cleanup_count - 1;
                }
            }
        } else if (app->selected_menu == 1 && app->project.cleanup_count) {
            prompt_text("cleanup command", app->project.cleanups[app->selected_cleanup], LONG_LEN);
        } else if (app->selected_menu == 2 && app->project.cleanup_count) {
            for (int i = app->selected_cleanup; i < app->project.cleanup_count - 1; i++) {
                snprintf(app->project.cleanups[i], LONG_LEN, "%s", app->project.cleanups[i + 1]);
            }
            app->project.cleanup_count--;
            clamp_selected(app);
        } else if (app->selected_menu == 3) { preview_script(app); app->screen = SCREEN_PREVIEW; app->selected_menu = 0; }
        else if (app->selected_menu == 4) set_status(app, "Cleanup settings saved in memory.");
        else if (app->selected_menu == 5) { app->screen = SCREEN_EDITOR; app->selected_menu = 0; }
        break;
    case SCREEN_REBOOT:
        if (app->selected_menu == 0) {
            if (selected_count(&app->project) == 0) set_status(app, "Select at least one test before starting.");
            else { app->run_after_generate = true; app->confirm_action = CONFIRM_GENERATE; app->previous_screen = app->screen; app->screen = SCREEN_CONFIRM; app->selected_menu = 0; }
        }
        else if (app->selected_menu == 1) { app->screen = SCREEN_EDITOR; app->selected_menu = 0; }
        break;
    case SCREEN_SCRIPT_EDITOR:
        break;
    case SCREEN_PREVIEW:
        if (app->selected_menu <= 3) {
            app->selected_preview_file = app->selected_menu;
            set_status(app, "Preview tabs are placeholders in MVP; main script preview is shown.");
        } else if (app->selected_menu == 4) { app->run_after_generate = false; app->confirm_action = CONFIRM_GENERATE; app->previous_screen = app->screen; app->screen = SCREEN_CONFIRM; app->selected_menu = 0; }
        else if (app->selected_menu == 5) { app->screen = SCREEN_EDITOR; app->selected_menu = 0; }
        break;
    case SCREEN_RESULT:
        if (app->selected_menu == 0) { app->screen = SCREEN_EDITOR; app->selected_menu = 0; }
        else if (app->selected_menu == 1) { app->screen = SCREEN_DASHBOARD; app->selected_menu = 0; }
        break;
    case SCREEN_CONFIRM:
        if (app->confirm_action == CONFIRM_DELETE) {
            if (app->selected_menu == 0) {
                delete_case(app);
                app->screen = app->previous_screen;
                app->selected_menu = 0;
            } else {
                app->screen = app->previous_screen;
                app->selected_menu = 0;
            }
        } else if (app->run_after_generate) {
            if (app->selected_menu == 0) {
                start_selected_test_scripts(app);
                app->run_after_generate = false;
                app->screen = SCREEN_RESULT;
                app->selected_menu = 0;
                set_status(app, "Finished selected test sequence.");
            } else if (app->selected_menu == 1) {
                app->stop_on_failure = !app->stop_on_failure;
                set_status(app, app->stop_on_failure ? "Stop on NG enabled." : "Stop on NG disabled.");
            } else {
                app->screen = app->previous_screen;
                app->selected_menu = 0;
            }
        } else {
            if (app->selected_menu == 0) {
                if (save_selected_test_scripts(app) == 0) {
                    app->screen = SCREEN_RESULT;
                    app->selected_menu = 0;
                    set_status(app, "Saved selected test scripts in current directory.");
                } else {
                    set_status(app, "Failed to generate scripts.");
                    app->screen = app->previous_screen;
                }
            } else {
                app->screen = app->previous_screen;
                app->selected_menu = 0;
            }
        }
        break;
    }
}

static int menu_count_for(Screen s) {
    switch (s) {
    case SCREEN_DASHBOARD: return 4;
    case SCREEN_EDITOR: return 10;
    case SCREEN_FORM: return 7;
    case SCREEN_MATCH: return 1;
    case SCREEN_CLEANUP: return 6;
    case SCREEN_REBOOT: return 2;
    case SCREEN_SCRIPT_EDITOR: return 1;
    case SCREEN_PREVIEW: return 6;
    case SCREEN_RESULT: return 2;
    case SCREEN_CONFIRM: return 3;
    }
    return 1;
}

static void handle_key(App *app, int ch) {
    if (app->screen == SCREEN_SCRIPT_EDITOR) {
        handle_script_editor_key(app, ch);
        return;
    }
    int mc = menu_count_for(app->screen);
    if (app->screen == SCREEN_CONFIRM &&
        !(app->confirm_action == CONFIRM_GENERATE && app->run_after_generate)) {
        mc = 2;
    }
    switch (ch) {
    case KEY_UP:
        if (app->screen == SCREEN_EDITOR && app->project.case_count > 0) {
            move_filtered_case(app, -1);
        }
        else if (app->screen == SCREEN_FORM) app->selected_field--;
        else if (app->screen == SCREEN_CLEANUP) app->selected_cleanup--;
        else if (app->screen == SCREEN_PREVIEW && app->preview_scroll > 0) app->preview_scroll--;
        break;
    case KEY_DOWN:
        if (app->screen == SCREEN_EDITOR && app->project.case_count > 0) {
            move_filtered_case(app, 1);
        }
        else if (app->screen == SCREEN_FORM) app->selected_field++;
        else if (app->screen == SCREEN_CLEANUP) app->selected_cleanup++;
        else if (app->screen == SCREEN_PREVIEW && app->preview_scroll + 1 < app->preview_count) app->preview_scroll++;
        break;
    case KEY_LEFT:
        app->selected_menu--;
        break;
    case KEY_RIGHT:
    case '\t':
        app->selected_menu++;
        break;
    case '\n':
    case KEY_ENTER:
        run_selected_menu(app);
        break;
    case '/':
        if (app->screen == SCREEN_EDITOR) edit_case_filter(app);
        break;
    case 27:
        set_status(app, "Esc exits the TUI from any non-editor screen.");
        break;
    case KEY_F(1):
        set_status(app, "Use Arrow/Tab to select Action Menu, Enter to run, Esc to go back.");
        break;
    default:
        break;
    }
    if (app->selected_menu < 0) app->selected_menu = mc - 1;
    if (app->selected_menu >= mc) app->selected_menu = 0;
    if (app->selected_field < 0) app->selected_field = 13;
    if (app->selected_field > 13) app->selected_field = 0;
    clamp_selected(app);
}

int main(void) {
    App app;
    setlocale(LC_ALL, "");
    init_app(&app);

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    meta(stdscr, TRUE);
    set_escdelay(150);
    curs_set(0);
    if (has_colors()) {
        start_color();
        use_default_colors();
    }

    bool running = true;
    while (running) {
        draw_app(&app);
        int ch = read_tui_input();
        if (app.screen != SCREEN_SCRIPT_EDITOR && ch == 27) {
            running = false;
            continue;
        }
        handle_key(&app, ch);
    }

    endwin();
    return 0;
}
