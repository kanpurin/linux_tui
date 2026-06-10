#include <ctype.h>
#include <ncurses.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_CASES 64
#define MAX_CLEANUPS 32
#define TEXT_LEN 256
#define LONG_LEN 4096
#define PREVIEW_LINES 512
#define EDITOR_LINES 160
#define EDITOR_COLS 256
#define REGISTRY_LINE (LONG_LEN * 4)
#define EDITOR_TAB_WIDTH 4
#define MAX_CHECKS 4

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
    MATCH_CONTAINS,
    MATCH_REGEX,
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
    EDIT_TARGET_CHECK_EXPECTED
} EditorTarget;

typedef struct {
    char var[TEXT_LEN];
    MatchType match;
    char expected[LONG_LEN];
} CheckRule;

typedef struct {
    char id[32];
    char title[TEXT_LEN];
    char command[LONG_LEN];
    char script_path[LONG_LEN];
    CommandKind kind;
    bool selected;
    int expected_exit;
    CheckRule checks[MAX_CHECKS];
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
    bool selected_only;
    bool run_after_generate;
    char editor_lines[EDITOR_LINES][EDITOR_COLS];
    int editor_line_count;
    int editor_row;
    int editor_col;
    bool editor_insert;
    bool editor_command_mode;
    bool editor_search_mode;
    bool editor_new_case;
    bool regex_template_list_open;
    EditorTarget editor_target;
    char editor_normal_command[16];
    int editor_normal_command_len;
    char editor_command[64];
    int editor_command_len;
    char editor_search[TEXT_LEN];
    int editor_search_len;
    char editor_clipboard[EDITOR_LINES][EDITOR_COLS];
    int editor_clipboard_count;
    char editor_undo_lines[EDITOR_LINES][EDITOR_COLS];
    int editor_undo_line_count;
    int editor_undo_row;
    int editor_undo_col;
    bool editor_has_undo;
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

static void write_match_function(FILE *f);
static void write_case(FILE *f, const TestCase *tc, int index);
static void write_summary(FILE *f);
static void shell_quote_buf(char *dst, size_t dst_sz, const char *src);
static void auto_configure_test(TestCase *tc);
static void save_test_registry(const Project *p);
static bool command_looks_like_reboot(const char *cmd);
static void trim_line_copy(char *dst, size_t dst_sz, const char *line, size_t len);
static void copy_text(char *dst, size_t dst_sz, const char *src);

static const char *match_name(MatchType m) {
    switch (m) {
    case MATCH_NONE: return "none";
    case MATCH_EXACT: return "exact";
    case MATCH_CONTAINS: return "contains";
    case MATCH_REGEX: return "regex";
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

static void clamp_selected(App *app) {
    if (app->project.case_count <= 0) {
        app->selected_case = 0;
    } else if (app->selected_case >= app->project.case_count) {
        app->selected_case = app->project.case_count - 1;
    } else if (app->selected_case < 0) {
        app->selected_case = 0;
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

static int editor_visual_col(const char *line, int col) {
    int visual = 0;
    for (int i = 0; line && line[i] && i < col; i++) {
        visual += line[i] == '\t' ? EDITOR_TAB_WIDTH : 1;
    }
    return visual;
}

static void print_editor_line(int y, int x, int w, const char *line) {
    int cx = x;
    for (int i = 0; line && line[i] && cx < x + w; i++) {
        if (line[i] == '\t') {
            for (int n = 0; n < EDITOR_TAB_WIDTH && cx < x + w; n++) {
                mvaddch(y, cx++, ' ');
            }
        } else {
            mvaddch(y, cx++, line[i]);
        }
    }
    while (cx < x + w) {
        mvaddch(y, cx++, ' ');
    }
}

static void draw_editor_cursor(const App *app, int y, int x, int w, const char *line) {
    int len = (int)strlen(line);
    int cx = x + editor_visual_col(line, app->editor_col);
    if (cx >= x + w) return;
    attron(A_UNDERLINE);
    if (app->editor_col < len) {
        if (line[app->editor_col] == '\t') {
            for (int n = 0; n < EDITOR_TAB_WIDTH && cx + n < x + w; n++) {
                mvaddch(y, cx + n, ' ');
            }
        } else {
            mvaddch(y, cx, line[app->editor_col]);
        }
    } else {
        mvaddch(y, cx, '_');
    }
    attroff(A_UNDERLINE);
}

static void escape_field(char *dst, size_t dst_sz, const char *src) {
    size_t j = 0;
    if (dst_sz == 0) return;
    for (size_t i = 0; src && src[i] && j + 2 < dst_sz; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\n') {
            dst[j++] = '\\';
            dst[j++] = 'n';
        } else if (c == '\t') {
            dst[j++] = '\\';
            dst[j++] = 't';
        } else if (c == '\\') {
            dst[j++] = '\\';
            dst[j++] = '\\';
        } else {
            dst[j++] = (char)c;
        }
    }
    dst[j] = '\0';
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

static MatchType parse_match(const char *s) {
    for (int i = MATCH_NONE; i <= MATCH_NOT_EMPTY; i++) {
        if (strcmp(s, match_name((MatchType)i)) == 0) return (MatchType)i;
    }
    return MATCH_NONE;
}

static bool is_match_name(const char *s) {
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
    }
    return "text";
}

static void load_editor_text(App *app, const char *src) {
    app->editor_line_count = 1;
    app->editor_row = 0;
    app->editor_col = 0;
    app->editor_insert = false;
    app->editor_command_mode = false;
    app->editor_search_mode = false;
    app->editor_command_len = 0;
    app->editor_command[0] = '\0';
    app->editor_normal_command_len = 0;
    app->editor_normal_command[0] = '\0';
    app->editor_search_len = 0;
    app->editor_search[0] = '\0';
    app->editor_clipboard_count = 0;
    app->editor_has_undo = false;
    memset(app->editor_lines, 0, sizeof(app->editor_lines));

    int row = 0;
    int col = 0;
    for (size_t i = 0; src[i] && row < EDITOR_LINES; i++) {
        if (src[i] == '\n') {
            app->editor_lines[row][col] = '\0';
            row++;
            col = 0;
            if (row >= EDITOR_LINES) break;
            continue;
        }
        if (col + 1 < EDITOR_COLS) {
            app->editor_lines[row][col++] = src[i];
        }
    }
    if (row < EDITOR_LINES) {
        app->editor_lines[row][col] = '\0';
        app->editor_line_count = row + 1;
    } else {
        app->editor_line_count = EDITOR_LINES;
    }
}

static void save_editor_text(App *app, char *dst, size_t dst_sz) {
    size_t pos = 0;
    if (dst_sz == 0) return;
    dst[0] = '\0';
    for (int r = 0; r < app->editor_line_count; r++) {
        const char *line = app->editor_lines[r];
        size_t len = strlen(line);
        if (pos + len + 2 >= dst_sz) break;
        memcpy(dst + pos, line, len);
        pos += len;
        if (r + 1 < app->editor_line_count) dst[pos++] = '\n';
    }
    dst[pos] = '\0';
}

static void load_editor_from_case(App *app) {
    if (app->project.case_count == 0) return;
    const TestCase *tc = &app->project.cases[app->selected_case];
    const CheckRule *check = primary_check_const(tc);
    const char *src = tc->command;
    if (app->editor_target == EDIT_TARGET_CHECK_EXPECTED && check) src = check->expected;
    load_editor_text(app, src);
}

static void save_editor_to_case(App *app) {
    if (app->project.case_count == 0) return;
    TestCase *tc = &app->project.cases[app->selected_case];
    if (app->editor_target == EDIT_TARGET_CHECK_EXPECTED) {
        CheckRule *check = primary_check(tc);
        save_editor_text(app, check->expected, sizeof(check->expected));
    } else {
        save_editor_text(app, tc->command, sizeof(tc->command));
        tc->kind = CMD_SHELL;
        auto_configure_test(tc);
    }
}

static bool editor_uses_regex_templates(const App *app) {
    if (app->project.case_count == 0) return false;
    const TestCase *tc = &app->project.cases[app->selected_case];
    const CheckRule *check = primary_check_const(tc);
    return app->editor_target == EDIT_TARGET_CHECK_EXPECTED && check && check->match == MATCH_REGEX;
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
    trim_line_copy(trimmed, sizeof(trimmed), line, len);
    if (strncmp(trimmed, "send ", 5) == 0) {
        fputs("  printf ", f);
        shell_quote_len(f, trimmed + 5, strlen(trimmed + 5));
        fputc('\n', f);
    } else if (strncmp(trimmed, "send-shell ", 11) == 0) {
        fputs("  printf '%s' ", f);
        fputs(trimmed + 11, f);
        fputc('\n', f);
    } else if (strncmp(trimmed, "text ", 5) == 0) {
        fputs("  printf ", f);
        shell_quote_len(f, trimmed + 5, strlen(trimmed + 5));
        fputs("\n  printf '\\n'\n", f);
    } else if (strncmp(trimmed, "text-shell ", 11) == 0) {
        fputs("  printf '%s\\n' ", f);
        fputs(trimmed + 11, f);
        fputc('\n', f);
    } else if (strcmp(trimmed, "enter") == 0) {
        fputs("  printf '\\r'\n", f);
    } else if (strcmp(trimmed, "esc") == 0) {
        fputs("  printf '\\033'\n", f);
    } else if (strcmp(trimmed, "tab") == 0) {
        fputs("  printf '\\t'\n", f);
    } else if (strcmp(trimmed, "space") == 0) {
        fputs("  printf ' '\n", f);
    } else if (strncmp(trimmed, "sleep ", 6) == 0) {
        fputs("  sleep ", f);
        for (const char *p = trimmed + 6; *p; p++) {
            if (isdigit((unsigned char)*p) || *p == '.') fputc(*p, f);
        }
        fputc('\n', f);
    } else if (strncmp(trimmed, "ctrl ", 5) == 0 && trimmed[5]) {
        unsigned char c = (unsigned char)tolower((unsigned char)trimmed[5]);
        if (c >= 'a' && c <= 'z') {
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
                fputs(": # @check handled by autotest builder\n", f);
            } else if (line_is_tui_end(p, len)) {
                fputs("echo '[NG] @end without @tui' >&2\nexit 1\n", f);
            } else {
                write_shell_line(f, p, len);
            }
        } else {
            if (line_is_tui_end(p, len)) {
                fputs("} | script -q -c ", f);
                shell_quote_len(f, tui_cmd, strlen(tui_cmd));
                fputs(" /dev/null >/dev/null 2>&1\n", f);
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
        fputs("} | script -q -c ", f);
        shell_quote_len(f, tui_cmd, strlen(tui_cmd));
        fputs(" /dev/null >/dev/null 2>&1\n", f);
    }
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
    const char *p = tc->command;
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

static void collect_check_directives(TestCase *tc) {
    bool found = false;
    int count = 0;
    const char *p = tc->command;
    while (p && *p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char arg[LONG_LEN];
        if (line_starts_directive(p, len, "@check", arg, sizeof(arg))) {
            char *var = arg;
            while (isspace((unsigned char)*var)) var++;
            char *match = var;
            while (*match && !isspace((unsigned char)*match)) match++;
            if (*match) *match++ = '\0';
            while (isspace((unsigned char)*match)) match++;
            char *expected = match;
            while (*expected && !isspace((unsigned char)*expected)) expected++;
            if (*expected) *expected++ = '\0';
            while (isspace((unsigned char)*expected)) expected++;
            if (var[0] && match[0] && count < MAX_CHECKS) {
                CheckRule *check = &tc->checks[count++];
                copy_text(check->var, sizeof(check->var), var);
                check->match = parse_match(match);
                copy_text(check->expected, sizeof(check->expected), expected);
                found = true;
            }
        }
        p = end ? end + 1 : NULL;
    }
    if (found) {
        tc->check_count = count;
    }
}

static void auto_configure_test(TestCase *tc) {
    (void)primary_check(tc);
    collect_check_directives(tc);
    tc->cleanup[0] = '\0';
    collect_temp_paths(tc, "/tmp/");
    collect_temp_paths(tc, "/var/tmp/");
    if (!tc->command[0]) return;
    if (command_looks_like_reboot(tc->command)) {
        tc->kind = CMD_REBOOT;
    } else if (tc->kind == CMD_REBOOT) {
        tc->kind = CMD_SHELL;
    }
}

static void make_test_filename(const TestCase *tc, char *out, size_t out_sz) {
    char safe_id[64];
    size_t j = 0;
    for (size_t i = 0; tc->id[i] && j + 1 < sizeof(safe_id); i++) {
        unsigned char c = (unsigned char)tc->id[i];
        safe_id[j++] = (char)(isalnum(c) ? tolower(c) : '_');
    }
    if (j == 0) {
        copy_text(safe_id, sizeof(safe_id), "test");
    } else {
        safe_id[j] = '\0';
    }
    snprintf(out, out_sz, "autotest_%s.sh", safe_id);
}

static int write_single_test_script(TestCase *tc) {
    auto_configure_test(tc);
    char cwd[LONG_LEN];
    char filename[TEXT_LEN];
    char path[LONG_LEN];
    if (tc->script_path[0]) {
        copy_text(path, sizeof(path), tc->script_path);
    } else {
        if (!getcwd(cwd, sizeof(cwd))) return -1;
        make_test_filename(tc, filename, sizeof(filename));
        size_t cwd_len = strlen(cwd);
        size_t file_len = strlen(filename);
        if (cwd_len + 1 + file_len + 1 > sizeof(path)) return -1;
        memcpy(path, cwd, cwd_len);
        path[cwd_len] = '/';
        memcpy(path + cwd_len + 1, filename, file_len + 1);
    }

    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "#!/usr/bin/env bash\n");
    fprintf(f, "set -u\n\n");
    fprintf(f, "TOTAL_COUNT=0\n");
    fprintf(f, "OK_COUNT=0\n");
    fprintf(f, "NG_COUNT=0\n\n");
    fprintf(f, "SELF_PATH=\"$(readlink -f \"$0\" 2>/dev/null || printf '%%s\\n' \"$0\")\"\n");
    fprintf(f, "RESULT_FILE=\"${SELF_PATH%%.sh}.result\"\n\n");
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
    fprintf(f, "run_case_001 \"${1:-run}\"\n");
    write_summary(f);
    fclose(f);
    chmod(path, 0755);
    copy_text(tc->script_path, sizeof(tc->script_path), path);
    return 0;
}

static void open_text_editor(App *app, EditorTarget target, Screen return_screen) {
    app->editor_target = target;
    app->previous_screen = return_screen;
    app->regex_template_list_open = false;
    load_editor_from_case(app);
    app->screen = SCREEN_SCRIPT_EDITOR;
    app->selected_menu = 0;
    set_status(app, "Text editor: i insert, Esc normal, :wq save, :q cancel.");
}

static void open_script_editor(App *app) {
    open_text_editor(app, EDIT_TARGET_COMMAND, SCREEN_EDITOR);
}

static void discard_current_case_if_new(App *app) {
    if (!app->editor_new_case || app->project.case_count == 0) return;
    int idx = app->selected_case;
    if (idx < 0 || idx >= app->project.case_count) return;
    for (int i = idx; i < app->project.case_count - 1; i++) {
        app->project.cases[i] = app->project.cases[i + 1];
    }
    app->project.case_count--;
    app->editor_new_case = false;
    clamp_selected(app);
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

static void save_test_registry(const Project *p) {
    char path[LONG_LEN];
    if (ensure_registry_dir() != 0) return;
    if (registry_path(path, sizeof(path)) != 0) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# autotest-assist tests v2\n");
    fprintf(f, "# path\ttitle\tselected\tkind\texpected_exit\tcheck_count\tcheck1_var\tcheck1_match\tcheck1_expected\tcheck2_var\tcheck2_match\tcheck2_expected\tcheck3_var\tcheck3_match\tcheck3_expected\tcheck4_var\tcheck4_match\tcheck4_expected\tcommand\n");
    for (int i = 0; i < p->case_count; i++) {
        const TestCase *tc = &p->cases[i];
        if (!tc->script_path[0]) continue;
        char fields[6 + MAX_CHECKS * 3 + 1][LONG_LEN];
        escape_field(fields[0], sizeof(fields[0]), tc->script_path);
        escape_field(fields[1], sizeof(fields[1]), tc->title);
        snprintf(fields[2], sizeof(fields[2]), "%d", tc->selected ? 1 : 0);
        escape_field(fields[3], sizeof(fields[3]), command_kind_name(tc->kind));
        snprintf(fields[4], sizeof(fields[4]), "%d", tc->expected_exit);
        snprintf(fields[5], sizeof(fields[5]), "%d", tc->check_count > 0 ? tc->check_count : 1);
        for (int c = 0; c < MAX_CHECKS; c++) {
            int base = 6 + c * 3;
            escape_field(fields[base], sizeof(fields[base]), tc->checks[c].var);
            escape_field(fields[base + 1], sizeof(fields[base + 1]), match_name(tc->checks[c].match));
            escape_field(fields[base + 2], sizeof(fields[base + 2]), tc->checks[c].expected);
        }
        escape_field(fields[6 + MAX_CHECKS * 3], sizeof(fields[6 + MAX_CHECKS * 3]), tc->command);
        for (int c = 0; c < 6 + MAX_CHECKS * 3 + 1; c++) {
            if (c) fputc('\t', f);
            fputs(fields[c], f);
        }
        fputc('\n', f);
    }
    fclose(f);
}

static bool load_test_registry(Project *p) {
    char path[LONG_LEN];
    char line[REGISTRY_LINE];
    if (registry_path(path, sizeof(path)) != 0) return false;
    FILE *f = fopen(path, "r");
    if (!f) return false;

    p->case_count = 0;
    while (fgets(line, sizeof(line), f) && p->case_count < MAX_CASES) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0') continue;
        line[strcspn(line, "\r\n")] = '\0';
        char *fields[24] = {0};
        int field_count = 0;
        char *cursor = line;
        while (field_count < 24) {
            fields[field_count++] = cursor;
            char *tab = strchr(cursor, '\t');
            if (!tab) break;
            *tab = '\0';
            cursor = tab + 1;
        }
        if (field_count < 2 || !fields[0][0]) continue;

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
            if (tc->check_count > MAX_CHECKS) tc->check_count = MAX_CHECKS;
            for (int c = 0; c < MAX_CHECKS; c++) {
                int base = 6 + c * 3;
                unescape_field(tc->checks[c].var, sizeof(tc->checks[c].var), fields[base]);
                tc->checks[c].match = parse_match(fields[base + 1]);
                unescape_field(tc->checks[c].expected, sizeof(tc->checks[c].expected), fields[base + 2]);
            }
            if (!tc->checks[0].var[0]) copy_text(tc->checks[0].var, sizeof(tc->checks[0].var), "AUTOTEST_ACTUAL");
            unescape_field(tc->command, sizeof(tc->command), fields[18]);
        } else if (field_count > 7 && is_match_name(fields[5]) && is_match_name(fields[7])) {
            tc->checks[0].match = parse_match(fields[5]);
            if (field_count > 6) unescape_field(tc->checks[0].expected, sizeof(tc->checks[0].expected), fields[6]);
            if (field_count > 9) unescape_field(tc->command, sizeof(tc->command), fields[9]);
        } else {
            if (field_count > 5) unescape_field(tc->checks[0].var, sizeof(tc->checks[0].var), fields[5]);
            if (!tc->checks[0].var[0]) copy_text(tc->checks[0].var, sizeof(tc->checks[0].var), "AUTOTEST_ACTUAL");
            tc->checks[0].match = field_count > 6 ? parse_match(fields[6]) : MATCH_NONE;
            if (field_count > 7) unescape_field(tc->checks[0].expected, sizeof(tc->checks[0].expected), fields[7]);
            if (field_count > 8) unescape_field(tc->command, sizeof(tc->command), fields[8]);
        }
        tc->timeout_sec = 30;
        if (!tc->title[0]) copy_text(tc->title, sizeof(tc->title), tc->script_path);
        if (tc->kind != CMD_REBOOT && file_contains_reboot(tc->script_path)) tc->kind = CMD_REBOOT;
        p->case_count++;
    }
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
    draw_box(y, x, h, w, "Test Cases");
    int visible = h - 2;
    for (int i = 0; i < app->project.case_count && i < visible; i++) {
        const TestCase *tc = &app->project.cases[i];
        if (i == app->selected_case) attron(A_REVERSE);
        mvprintw(y + 1 + i, x + 1, "%c %-5s %-22.22s %-7s %s",
                 tc->selected ? '*' : ' ',
                 tc->id,
                 tc->title,
                 command_kind_name(tc->kind),
                 tc->script_path[0] ? "saved" : "not_saved");
        if (i == app->selected_case) attroff(A_REVERSE);
    }
    if (app->project.case_count == 0) {
        mvprintw(y + 1, x + 1, "No test cases. Select Add test case.");
    }
}

static void draw_case_details(const App *app, int y, int x, int h, int w) {
    draw_box(y, x, h, w, "Details");
    if (app->project.case_count == 0) return;
    const TestCase *tc = &app->project.cases[app->selected_case];
    const CheckRule *check = primary_check_const(tc);
    char check_summary[LONG_LEN];
    summarize_multiline(check_summary, sizeof(check_summary), check ? check->expected : "");
    mvprintw(y + 1, x + 1, "id: %s", tc->id);
    mvprintw(y + 2, x + 1, "title: %.*s", w - 9, tc->title);
    mvprintw(y + 3, x + 1, "kind: %s  selected: %s", command_kind_name(tc->kind), tc->selected ? "yes" : "no");
    mvprintw(y + 4, x + 1, "expected_exit: %d", tc->expected_exit);
    mvprintw(y + 5, x + 1, "checks: %d  primary: %.*s", tc->check_count, w - 25, check ? check->var : "AUTOTEST_ACTUAL");
    mvprintw(y + 6, x + 1, "check: %s %.*s", match_name(check ? check->match : MATCH_NONE), w - 16, check_summary);
    mvprintw(y + 7, x + 1, "cleanup: auto %s", tc->cleanup[0] ? "generated" : "none");
    mvprintw(y + 8, x + 1, "reboot: auto %s", tc->kind == CMD_REBOOT ? "detected" : "none");
    mvprintw(y + 9, x + 1, "path: %.*s", w - 8, tc->script_path[0] ? tc->script_path : "<not saved>");
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
    static const char *menu[] = {"Open test list", "Create test", "Preview selected", "Start selected tests", "Help"};
    draw_menu(15, 1, width - 2, menu, 5, app->selected_menu);
}

static void draw_editor(const App *app, int height, int width) {
    draw_header(app, width);
    int top = 3;
    int bottom = height - 5;
    int left_w = width / 2;
    draw_cases(app, top, 0, bottom - top, left_w);
    draw_case_details(app, top, left_w + 1, bottom - top, width - left_w - 1);
    draw_box(bottom, 0, 4, width, "Selection / Warnings");
    mvprintw(bottom + 1, 1, "shell:bash  final NG exit:1  selected:%d  cleanup/reboot:auto",
             selected_count(&app->project));
    if (has_reboot(&app->project)) {
        mvprintw(bottom + 2, 1, "selected reboot tests may interrupt sequential execution");
    }
    static const char *menu[] = {"Edit test", "Select test", "Delete test", "Variable match", "Preview selected", "Start selected tests", "Back"};
    draw_menu(height - 4, 1, width - 2, menu, 7, app->selected_menu);
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
    snprintf(values[4], sizeof(values[4]), "%s", tc->command);
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
    draw_box(3, 0, height - 7, width, "Variable Match Builder");
    if (app->project.case_count == 0) return;
    const TestCase *tc = &app->project.cases[app->selected_case];
    const CheckRule *check = primary_check_const(tc);
    char check_summary[LONG_LEN];
    summarize_multiline(check_summary, sizeof(check_summary), check ? check->expected : "");
    mvprintw(5, 2, "primary variable: %.*s", width - 20, check ? check->var : "AUTOTEST_ACTUAL");
    mvprintw(6, 2, "checks: %d/%d  match: %-10s expected: %.*s", tc->check_count, MAX_CHECKS, match_name(check ? check->match : MATCH_NONE), width - 50, check_summary);
    mvprintw(8, 2, "Match types:");
    mvprintw(9, 4, "none       no variable check");
    mvprintw(10, 4, "exact      output must be identical");
    mvprintw(11, 4, "contains   output must contain expected text");
    mvprintw(12, 4, "regex      output must match expected regular expression");
    mvprintw(13, 4, "empty      output must be empty");
    mvprintw(14, 4, "not_empty  output must not be empty");
    static const char *menu[] = {"Set variable", "Set match", "Set expected", "Back"};
    draw_menu(height - 4, 1, width - 2, menu, 4, app->selected_menu);
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
    static const char *menu[] = {"Preview selected", "Start selected tests", "Back"};
    draw_menu(height - 4, 1, width - 2, menu, 3, app->selected_menu);
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
    int rows = height - 10;
    for (int i = 0; i < rows && i < app->editor_line_count; i++) {
        int y = 6 + i;
        mvprintw(y, 2, "%3d ", i + 1);
        print_editor_line(y, 7, width - 9, app->editor_lines[i]);
        if (i == app->editor_row) {
            draw_editor_cursor(app, y, 7, width - 9, app->editor_lines[i]);
        }
    }
    if (regex_templates) {
        mvhline(height - 5, 1, ' ', width - 2);
        mvprintw(height - 5, 2, "Regex templates: :template");
    }
    mvprintw(height - 4, 2, "-- %s --  Arrow move  i insert  Esc normal  :wq save  :q cancel",
             app->editor_insert ? "INSERT" : "NORMAL");
    if (!app->editor_insert && app->editor_command_mode) {
        mvprintw(height - 3, 2, ":%s", app->editor_command);
    } else if (!app->editor_insert && app->editor_search_mode) {
        mvprintw(height - 3, 2, "/%s", app->editor_search);
    } else {
        mvprintw(height - 3, 2, "Editing %s.", editor_target_name(app->editor_target));
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
    if (app->editor_insert) {
        int cy = 6 + app->editor_row;
        int cx = 7 + editor_visual_col(app->editor_lines[app->editor_row], app->editor_col);
        if (cy < height - 4 && cx < width - 1) move(cy, cx);
    }
}

static void preview_add(App *app, const char *line) {
    if (app->preview_count < PREVIEW_LINES) {
        snprintf(app->preview[app->preview_count++], LONG_LEN, "%s", line);
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
        if (tc->kind == CMD_REBOOT) {
            preview_add(app, "  # split script body into pre-reboot and post-reboot phases");
            preview_add(app, "  # install a one-shot systemd resume unit, then run reboot command");
            preview_add(app, "  # after boot: run post phase, verify result, cleanup");
        } else {
            snprintf(line, sizeof(line), "  bash -c '%s' >\"$WORK_DIR/case_%03d.stdout\" 2>\"$WORK_DIR/case_%03d.stderr\"", tc->command, i + 1, i + 1);
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

static void draw_result(const App *app, int height, int width) {
    draw_header(app, width);
    draw_box(3, 0, height - 7, width, "Saved Test Scripts");
    int y = 5;
    for (int i = 0; i < app->project.case_count && y < height - 8; i++) {
        const TestCase *tc = &app->project.cases[i];
        if (!tc->selected) continue;
        mvprintw(y++, 2, "[%s] %s %s", tc->script_path[0] ? "OK" : "NG", tc->id, tc->title);
        if (tc->script_path[0] && y < height - 8) {
            print_clip(y++, 6, width - 8, tc->script_path);
        }
    }
    mvprintw(height - 8, 2, "Selected tests are executed sequentially by these saved paths.");
    static const char *menu[] = {"Back to tests", "Preview selected", "Dashboard"};
    draw_menu(height - 4, 1, width - 2, menu, 3, app->selected_menu);
}

static void draw_confirm(const App *app, int height, int width) {
    draw_header(app, width);
    draw_box(4, 4, height - 9, width - 8, "Confirm Script Generation");
    mvprintw(6, 6, "Selected test scripts may contain risky commands.");
    if (has_reboot(&app->project)) mvprintw(8, 8, "- reboot");
    for (int i = 0; i < app->project.cleanup_count && i < 4; i++) {
        mvprintw(9 + i, 8, "- cleanup: %.*s", width - 22, app->project.cleanups[i]);
    }
    mvprintw(height - 9, 6, app->run_after_generate ? "Start selected tests?" : "Save selected test scripts?");
    static const char *menu[] = {"Continue", "Back"};
    draw_menu(height - 7, 6, width - 12, menu, 2, app->selected_menu);
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
    refresh();
}

static bool prompt_text(const char *label, char *buf, size_t buf_sz) {
    echo();
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
    mvhline(h / 2, 5, ' ', w - 10);
    mvprintw(h / 2, 6, "> ");
    clrtoeol();
    char tmp[LONG_LEN];
    snprintf(tmp, sizeof(tmp), "%s", buf);
    move(h / 2, 8);
    int rc = getnstr(tmp, (int)sizeof(tmp) - 1);
    noecho();
    curs_set(0);
    if (rc == ERR) return false;
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
    *m = (MatchType)((*m + 1) % 6);
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
    tc->check_count = 1;
    copy_text(tc->checks[0].var, sizeof(tc->checks[0].var), "AUTOTEST_ACTUAL");
    tc->checks[0].match = MATCH_NONE;
    copy_text(tc->command, sizeof(tc->command), "__AUTOTEST_REBOOT__");
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
    snprintf(tc->command, sizeof(tc->command),
             "{ sleep 0.3; printf 'gg'; printf 'dG'; printf 'i'; printf '%s\\n'; printf '\\033'; printf ':wq\\r'; } | script -q -c \"vim -Nu NONE -n '%s'\" /dev/null >/dev/null 2>&1",
             qtext, qpath);
    tc->kind = CMD_VIM;
    tc->expected_exit = 0;
    tc->check_count = 1;
    copy_text(tc->checks[0].var, sizeof(tc->checks[0].var), "AUTOTEST_ACTUAL");
    tc->checks[0].match = MATCH_NONE;
}

static void editor_save_undo(App *app) {
    memcpy(app->editor_undo_lines, app->editor_lines, sizeof(app->editor_lines));
    app->editor_undo_line_count = app->editor_line_count;
    app->editor_undo_row = app->editor_row;
    app->editor_undo_col = app->editor_col;
    app->editor_has_undo = true;
}

static void editor_undo(App *app) {
    if (!app->editor_has_undo) {
        set_status(app, "No undo available.");
        return;
    }
    memcpy(app->editor_lines, app->editor_undo_lines, sizeof(app->editor_lines));
    app->editor_line_count = app->editor_undo_line_count;
    app->editor_row = app->editor_undo_row;
    app->editor_col = app->editor_undo_col;
    app->editor_has_undo = false;
    set_status(app, "Undo.");
}

static void editor_insert_char(App *app, int ch) {
    editor_save_undo(app);
    char *line = app->editor_lines[app->editor_row];
    int len = (int)strlen(line);
    if (len + 1 >= EDITOR_COLS) return;
    if (app->editor_col < 0) app->editor_col = 0;
    if (app->editor_col > len) app->editor_col = len;
    memmove(line + app->editor_col + 1, line + app->editor_col, (size_t)(len - app->editor_col + 1));
    line[app->editor_col] = (char)ch;
    app->editor_col++;
}

static void editor_insert_text(App *app, const char *text) {
    for (size_t i = 0; text && text[i]; i++) {
        editor_insert_char(app, (unsigned char)text[i]);
    }
}

static void editor_newline(App *app) {
    if (app->editor_line_count >= EDITOR_LINES) return;
    editor_save_undo(app);
    char *line = app->editor_lines[app->editor_row];
    int len = (int)strlen(line);
    if (app->editor_col > len) app->editor_col = len;
    for (int r = app->editor_line_count; r > app->editor_row + 1; r--) {
        copy_text(app->editor_lines[r], EDITOR_COLS, app->editor_lines[r - 1]);
    }
    copy_text(app->editor_lines[app->editor_row + 1], EDITOR_COLS, line + app->editor_col);
    line[app->editor_col] = '\0';
    app->editor_line_count++;
    app->editor_row++;
    app->editor_col = 0;
}

static void editor_backspace(App *app) {
    char *line = app->editor_lines[app->editor_row];
    int len = (int)strlen(line);
    if (app->editor_col > 0 && app->editor_col <= len) {
        editor_save_undo(app);
        memmove(line + app->editor_col - 1, line + app->editor_col, (size_t)(len - app->editor_col + 1));
        app->editor_col--;
    } else if (app->editor_row > 0) {
        int prev_len = (int)strlen(app->editor_lines[app->editor_row - 1]);
        if (prev_len + len + 1 < EDITOR_COLS) {
            editor_save_undo(app);
            memcpy(app->editor_lines[app->editor_row - 1] + prev_len, line, (size_t)len + 1);
            for (int r = app->editor_row; r < app->editor_line_count - 1; r++) {
                copy_text(app->editor_lines[r], EDITOR_COLS, app->editor_lines[r + 1]);
            }
            app->editor_line_count--;
            app->editor_row--;
            app->editor_col = prev_len;
        }
    }
}

static void editor_delete_lines(App *app, int count) {
    if (count < 1) count = 1;
    editor_save_undo(app);
    if (app->editor_line_count <= 1) {
        app->editor_lines[0][0] = '\0';
        app->editor_row = 0;
        app->editor_col = 0;
        return;
    }
    if (count > app->editor_line_count - app->editor_row) {
        count = app->editor_line_count - app->editor_row;
    }
    for (int r = app->editor_row; r + count < app->editor_line_count; r++) {
        copy_text(app->editor_lines[r], EDITOR_COLS, app->editor_lines[r + count]);
    }
    app->editor_line_count -= count;
    if (app->editor_line_count < 1) {
        app->editor_line_count = 1;
        app->editor_lines[0][0] = '\0';
    }
    if (app->editor_row >= app->editor_line_count) app->editor_row = app->editor_line_count - 1;
    int len = (int)strlen(app->editor_lines[app->editor_row]);
    if (app->editor_col > len) app->editor_col = len;
}

static void editor_copy_lines(App *app, int count) {
    if (count < 1) count = 1;
    if (count > app->editor_line_count - app->editor_row) {
        count = app->editor_line_count - app->editor_row;
    }
    app->editor_clipboard_count = count;
    for (int i = 0; i < count; i++) {
        copy_text(app->editor_clipboard[i], EDITOR_COLS, app->editor_lines[app->editor_row + i]);
    }
    set_status(app, "Copied line(s).");
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
        copy_text(app->editor_lines[r + count], EDITOR_COLS, app->editor_lines[r]);
    }
    for (int i = 0; i < count; i++) {
        copy_text(app->editor_lines[insert_at + i], EDITOR_COLS, app->editor_clipboard[i]);
    }
    app->editor_line_count += count;
    app->editor_row = insert_at;
    app->editor_col = 0;
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
}

static bool editor_handle_normal_sequence(App *app, int ch) {
    if (app->editor_normal_command_len == 0 &&
        ch != 'd' && ch != 'y' && ch != 'g' && !isdigit((unsigned char)ch)) {
        return false;
    }
    if (ch == 27) {
        app->editor_normal_command_len = 0;
        app->editor_normal_command[0] = '\0';
        return true;
    }
    if (!isprint(ch)) {
        app->editor_normal_command_len = 0;
        app->editor_normal_command[0] = '\0';
        return false;
    }
    int len = app->editor_normal_command_len;
    if (len + 1 >= (int)sizeof(app->editor_normal_command)) {
        app->editor_normal_command_len = 0;
        app->editor_normal_command[0] = '\0';
        return true;
    }
    if (len == 0) {
        app->editor_normal_command[len++] = (char)ch;
        app->editor_normal_command[len] = '\0';
        app->editor_normal_command_len = len;
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
        app->editor_normal_command_len = 0;
        app->editor_normal_command[0] = '\0';
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
        app->editor_normal_command_len = 0;
        app->editor_normal_command[0] = '\0';
    } else if (first == 'g' && ch == 'g') {
        editor_goto_line(app, 0);
        app->editor_normal_command_len = 0;
        app->editor_normal_command[0] = '\0';
    } else if (isdigit((unsigned char)first) && ch == 'G') {
        int line_no = atoi(app->editor_normal_command);
        editor_goto_line(app, line_no - 1);
        app->editor_normal_command_len = 0;
        app->editor_normal_command[0] = '\0';
    }
    return true;
}

static void handle_script_editor_key(App *app, int ch) {
    if (app->editor_insert) {
        if (ch == 27) {
            app->editor_insert = false;
            curs_set(0);
        } else if (ch == '\n' || ch == KEY_ENTER) {
            editor_newline(app);
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            editor_backspace(app);
        } else if (ch == KEY_LEFT && app->editor_col > 0) {
            app->editor_col--;
        } else if (ch == KEY_RIGHT) {
            int len = (int)strlen(app->editor_lines[app->editor_row]);
            if (app->editor_col < len) app->editor_col++;
        } else if (ch == KEY_HOME) {
            app->editor_col = 0;
        } else if (ch == KEY_END) {
            app->editor_col = (int)strlen(app->editor_lines[app->editor_row]);
        } else if (ch == KEY_UP && app->editor_row > 0) {
            app->editor_row--;
        } else if (ch == KEY_DOWN && app->editor_row + 1 < app->editor_line_count) {
            app->editor_row++;
        } else if (ch == '\t') {
            editor_insert_char(app, '\t');
        } else if (isprint(ch)) {
            editor_insert_char(app, ch);
        }
        int len = (int)strlen(app->editor_lines[app->editor_row]);
        if (app->editor_col > len) app->editor_col = len;
        return;
    }

    if (app->editor_command_mode) {
        if (ch == '\n' || ch == KEY_ENTER) {
            app->editor_command[app->editor_command_len] = '\0';
            if (strcmp(app->editor_command, "wq") == 0) {
                save_editor_to_case(app);
                if (app->editor_target == EDIT_TARGET_COMMAND &&
                    write_single_test_script(&app->project.cases[app->selected_case]) == 0) {
                    save_test_registry(&app->project);
                    set_status(app, "Saved test script in current directory.");
                } else if (app->editor_target != EDIT_TARGET_COMMAND) {
                    if (app->project.cases[app->selected_case].script_path[0]) {
                        write_single_test_script(&app->project.cases[app->selected_case]);
                    }
                    save_test_registry(&app->project);
                    set_status(app, "Saved variable expectation.");
                } else {
                    set_status(app, "Failed to save test script.");
                }
                if (app->editor_target == EDIT_TARGET_COMMAND) app->editor_new_case = false;
                app->screen = app->previous_screen;
            } else if (strcmp(app->editor_command, "q") == 0) {
                if (app->editor_target == EDIT_TARGET_COMMAND) discard_current_case_if_new(app);
                app->screen = app->previous_screen;
                set_status(app, "Canceled script editor.");
            } else if (strcmp(app->editor_command, "template") == 0 && editor_uses_regex_templates(app)) {
                app->regex_template_list_open = true;
                app->selected_menu = 0;
                set_status(app, "Opened regex templates.");
            } else {
                set_status(app, "Unknown editor command.");
            }
            app->editor_command_len = 0;
            app->editor_command[0] = '\0';
            app->editor_command_mode = false;
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (app->editor_command_len > 0) app->editor_command[--app->editor_command_len] = '\0';
        } else if (isprint(ch) && app->editor_command_len + 1 < (int)sizeof(app->editor_command)) {
            app->editor_command[app->editor_command_len++] = (char)ch;
            app->editor_command[app->editor_command_len] = '\0';
        } else if (ch == 27) {
            app->editor_command_len = 0;
            app->editor_command[0] = '\0';
            app->editor_command_mode = false;
        }
        return;
    }

    if (app->editor_search_mode) {
        if (ch == '\n' || ch == KEY_ENTER) {
            app->editor_search[app->editor_search_len] = '\0';
            app->editor_search_mode = false;
            editor_search_next(app, 1);
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (app->editor_search_len > 0) app->editor_search[--app->editor_search_len] = '\0';
        } else if (ch == 27) {
            app->editor_search_len = 0;
            app->editor_search[0] = '\0';
            app->editor_search_mode = false;
        } else if (isprint(ch) && app->editor_search_len + 1 < (int)sizeof(app->editor_search)) {
            app->editor_search[app->editor_search_len++] = (char)ch;
            app->editor_search[app->editor_search_len] = '\0';
        }
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

    if (editor_handle_normal_sequence(app, ch)) {
        int len = (int)strlen(app->editor_lines[app->editor_row]);
        if (app->editor_col > len) app->editor_col = len;
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
        if (app->editor_row > 0) app->editor_row--;
        break;
    case KEY_DOWN:
    case 'j':
        if (app->editor_row + 1 < app->editor_line_count) app->editor_row++;
        break;
    case KEY_LEFT:
    case 'h':
        if (app->editor_col > 0) app->editor_col--;
        break;
    case KEY_RIGHT:
    case 'l': {
        int len = (int)strlen(app->editor_lines[app->editor_row]);
        if (app->editor_col < len) app->editor_col++;
        break;
    }
    case KEY_HOME:
    case '0':
        app->editor_col = 0;
        break;
    case KEY_END:
    case '$':
        app->editor_col = (int)strlen(app->editor_lines[app->editor_row]);
        break;
    case 'x': {
        char *line = app->editor_lines[app->editor_row];
        int len = (int)strlen(line);
        if (app->editor_col < len) {
            editor_save_undo(app);
            memmove(line + app->editor_col, line + app->editor_col + 1, (size_t)(len - app->editor_col));
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
        set_status(app, "Use :wq to save or :q to cancel.");
        break;
    default:
        break;
    }
    int len = (int)strlen(app->editor_lines[app->editor_row]);
    if (app->editor_col > len) app->editor_col = len;
}

static void add_case(App *app) {
    if (app->project.case_count >= MAX_CASES) {
        set_status(app, "Maximum test cases reached.");
        return;
    }
    TestCase *tc = &app->project.cases[app->project.case_count];
    memset(tc, 0, sizeof(*tc));
    snprintf(tc->id, sizeof(tc->id), "TC%03d", app->project.case_count + 1);
    snprintf(tc->title, sizeof(tc->title), "new test case");
    snprintf(tc->command, sizeof(tc->command), "true");
    tc->kind = CMD_SHELL;
    tc->selected = true;
    tc->expected_exit = 0;
    tc->check_count = 1;
    copy_text(tc->checks[0].var, sizeof(tc->checks[0].var), "AUTOTEST_ACTUAL");
    tc->checks[0].match = MATCH_NONE;
    tc->timeout_sec = 30;
    app->selected_case = app->project.case_count;
    app->project.case_count++;
    app->editor_new_case = true;
    prompt_text("test title", tc->title, sizeof(tc->title));
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
        copy_text(result_path, sizeof(result_path), deleted_path);
        size_t len = strlen(result_path);
        if (len > 3 && strcmp(result_path + len - 3, ".sh") == 0) {
            snprintf(result_path + len - 3, sizeof(result_path) - len + 3, ".result");
        } else if (len + 8 < sizeof(result_path)) {
            strcat(result_path, ".result");
        }
        result_unlink_rc = unlink(result_path);
    }
    for (int i = app->selected_case; i < app->project.case_count - 1; i++) {
        app->project.cases[i] = app->project.cases[i + 1];
    }
    app->project.case_count--;
    clamp_selected(app);
    save_test_registry(&app->project);
    if (deleted_path[0] && (unlink_rc != 0 || (result_unlink_rc != 0 && access(result_path, F_OK) == 0))) {
        set_status(app, "Deleted test case, but failed to delete script file.");
    } else {
        set_status(app, "Deleted test case, script file, and result file.");
    }
}

static void edit_selected_field(App *app) {
    if (app->project.case_count == 0) return;
    TestCase *tc = &app->project.cases[app->selected_case];
    switch (app->selected_field) {
    case 0: prompt_text("id", tc->id, sizeof(tc->id)); break;
    case 1: prompt_text("title", tc->title, sizeof(tc->title)); break;
    case 2: tc->selected = !tc->selected; break;
    case 3: cycle_kind(tc); break;
    case 4: prompt_text("command", tc->command, sizeof(tc->command)); tc->kind = CMD_SHELL; break;
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
    fputs("    exact) [ \"$actual\" = \"$expected\" ] ;;\n", f);
    fputs("    contains) case \"$actual\" in *\"$expected\"*) return 0 ;; *) return 1 ;; esac ;;\n", f);
    fputs("    regex) printf '%s\\n' \"$actual\" | grep -Eq -- \"$expected\" ;;\n", f);
    fputs("    empty) [ -z \"$actual\" ] ;;\n", f);
    fputs("    not_empty) [ -n \"$actual\" ] ;;\n", f);
    fputs("    *) return 1 ;;\n", f);
    fputs("  esac\n", f);
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
    int count = tc->check_count > 0 ? tc->check_count : 1;
    for (int i = 0; i < count && i < MAX_CHECKS; i++) {
        const CheckRule *check = &tc->checks[i];
        const char *var = check->var[0] ? check->var : "AUTOTEST_ACTUAL";
        fprintf(f, "%sprintf '%%s' \"${%s-}\" >\"%s/actual_%d\"\n", indent, var, dir_expr, i + 1);
    }
}

static void write_validate_checks(FILE *f, const TestCase *tc, const char *indent, const char *dir_expr) {
    int count = tc->check_count > 0 ? tc->check_count : 1;
    fprintf(f, "%scheck_count=%d\n", indent, count);
    for (int i = 0; i < count && i < MAX_CHECKS; i++) {
        const CheckRule *check = &tc->checks[i];
        const char *var = check->var[0] ? check->var : "AUTOTEST_ACTUAL";
        fprintf(f, "%scheck_var_%d=", indent, i + 1);
        shell_quote(f, var);
        fprintf(f, "\n");
        fprintf(f, "%sactual_value_%d=$(cat \"%s/actual_%d\" 2>/dev/null || true)\n", indent, i + 1, dir_expr, i + 1);
        fprintf(f, "%smatch_value %s \"$actual_value_%d\" ", indent, match_name(check->match), i + 1);
        shell_quote(f, check->expected);
        fprintf(f, " || ok=0\n");
    }
}

static void write_validate_check_names(FILE *f, const TestCase *tc, const char *indent) {
    int count = tc->check_count > 0 ? tc->check_count : 1;
    for (int i = 0; i < count && i < MAX_CHECKS; i++) {
        const CheckRule *check = &tc->checks[i];
        const char *var = check->var[0] ? check->var : "AUTOTEST_ACTUAL";
        fprintf(f, "%sif ! is_valid_var_name ", indent);
        shell_quote(f, var);
        fprintf(f, "; then status_msg='[NG] invalid check variable name'; echo \"$status_msg\"; echo \"$status_msg\" >\"$RESULT_FILE\"; NG_COUNT=$((NG_COUNT + 1)); return; fi\n");
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
        fprintf(f, "    systemctl disable \"$unit_name\" >/dev/null 2>&1 || true\n");
        fprintf(f, "    rm -f \"/etc/systemd/system/$unit_name\"\n");
        fprintf(f, "    systemctl daemon-reload >/dev/null 2>&1 || true\n");
        if (tc->cleanup[0]) fprintf(f, "    %s || true\n", tc->cleanup);
        fprintf(f, "    if [ \"$ok\" -eq 1 ]; then\n");
        fprintf(f, "      status_msg=");
        shell_quote(f, ok_msg);
        fprintf(f, "\n");
        fprintf(f, "      echo \"$status_msg\"\n");
        fprintf(f, "      OK_COUNT=$((OK_COUNT + 1))\n");
        fprintf(f, "    else\n");
        fprintf(f, "      status_msg=");
        shell_quote(f, ng_prefix);
        fprintf(f, "\"$actual_exit\"\n");
        fprintf(f, "      echo \"$status_msg\"\n");
        fprintf(f, "      echo \"     stdout: $stdout_file\"\n");
        fprintf(f, "      echo \"     stderr: $stderr_file\"\n");
        fprintf(f, "      NG_COUNT=$((NG_COUNT + 1))\n");
        fprintf(f, "    fi\n");
        fprintf(f, "    {\n");
        fprintf(f, "      echo \"$status_msg\"\n");
        fprintf(f, "      echo \"Summary: total=$TOTAL_COUNT OK=$OK_COUNT NG=$NG_COUNT\"\n");
        fprintf(f, "      echo \"check_count=$check_count\"\n");
        fprintf(f, "      for i in $(seq 1 \"$check_count\"); do eval \"name=\\${check_var_$i}\"; eval \"value=\\${actual_value_$i}\"; echo \"check_$i=$name\"; echo \"actual_$i=$value\"; done\n");
        fprintf(f, "      echo \"actual_dir=$actual_dir\"\n");
        fprintf(f, "      echo \"stdout_file=$stdout_file\"\n");
        fprintf(f, "      echo \"stderr_file=$stderr_file\"\n");
        fprintf(f, "      echo '--- stdout ---'\n");
        fprintf(f, "      [ -f \"$stdout_file\" ] && cat \"$stdout_file\"\n");
        fprintf(f, "      echo '--- stderr ---'\n");
        fprintf(f, "      [ -f \"$stderr_file\" ] && cat \"$stderr_file\"\n");
        fprintf(f, "    } >\"$RESULT_FILE\"\n");
        fprintf(f, "    rm -rf \"$state_dir\"\n");
        write_rescue_continue(f, "    ");
        fprintf(f, "  }\n");
        fprintf(f, "  mkdir -p \"$state_dir\"\n");
        fprintf(f, "  mkdir -p \"$actual_dir\"\n");
        fprintf(f, "  mkdir -p \"$AUTOTEST_BACKUP_DIR\"\n");
        fprintf(f, "  cat >\"$state_dir/pre.sh\" <<'AUTOTEST_PRE'\n");
        write_command_phase(f, tc->command, false);
        fprintf(f, "AUTOTEST_PRE\n");
        fprintf(f, "  cat >\"$state_dir/reboot.sh\" <<'AUTOTEST_REBOOT'\n");
        write_reboot_command(f, tc->command);
        fprintf(f, "AUTOTEST_REBOOT\n");
        fprintf(f, "  cat >\"$state_dir/post.sh\" <<'AUTOTEST_POST'\n");
        write_command_phase(f, tc->command, true);
        fprintf(f, "AUTOTEST_POST\n");
        fprintf(f, "  chmod 700 \"$state_dir/pre.sh\" \"$state_dir/reboot.sh\" \"$state_dir/post.sh\"\n");
        write_validate_check_names(f, tc, "  ");
        fprintf(f, "  if [ \"$mode\" = \"--resume\" ]; then\n");
        fprintf(f, "    ( set -a; [ -f \"$state_dir/env.sh\" ] && . \"$state_dir/env.sh\"; . \"$state_dir/post.sh\"; post_rc=\"$?\"; ");
        write_capture_checks(f, tc, "", "$actual_dir");
        fprintf(f, "exit \"$post_rc\" ) >>\"$stdout_file\" 2>>\"$stderr_file\"\n");
        fprintf(f, "    finish_reboot_case \"$?\"\n");
        fprintf(f, "    return\n");
        fprintf(f, "  fi\n");
        fprintf(f, "  : >\"$stdout_file\"\n");
        fprintf(f, "  : >\"$stderr_file\"\n");
        fprintf(f, "  ( set -a; . \"$state_dir/pre.sh\"; pre_rc=\"$?\"; ");
        write_capture_checks(f, tc, "", "$actual_dir");
        fprintf(f, "export -p >\"$state_dir/env.sh\"; exit \"$pre_rc\" ) >>\"$stdout_file\" 2>>\"$stderr_file\"\n");
        fprintf(f, "  pre_exit=\"$?\"\n");
        fprintf(f, "  if [ \"$pre_exit\" -ne 0 ]; then finish_reboot_case \"$pre_exit\"; return; fi\n");
        fprintf(f, "  if [ \"$(id -u)\" -ne 0 ]; then status_msg='[NG] root required for reboot test resume'; echo \"$status_msg\"; echo \"$status_msg\" >\"$RESULT_FILE\"; NG_COUNT=$((NG_COUNT + 1)); return; fi\n");
        fprintf(f, "  self_path=\"$(readlink -f \"$0\")\"\n");
        fprintf(f, "  cat >\"/etc/systemd/system/$unit_name\" <<AUTOTEST_UNIT\n");
        fprintf(f, "[Unit]\nDescription=AutoTest %s resume\nDefaultDependencies=no\nAfter=sysinit.target\nBefore=rescue.service\nBefore=shutdown.target\nConflicts=shutdown.target\n\n", safe_id);
        fprintf(f, "[Service]\nType=oneshot\nExecStart=/bin/bash \"$self_path\" --resume\n\n[Install]\nWantedBy=multi-user.target rescue.target\n");
        fprintf(f, "AUTOTEST_UNIT\n");
        fprintf(f, "  systemctl daemon-reload\n");
        fprintf(f, "  systemctl enable \"$unit_name\"\n");
        fprintf(f, "  bash \"$state_dir/reboot.sh\" >>\"$stdout_file\" 2>>\"$stderr_file\"\n");
        fprintf(f, "  reboot_exit=\"$?\"\n");
        fprintf(f, "  if [ \"$reboot_exit\" -eq 0 ]; then echo '[PENDING] Reboot command issued. Result check will resume after boot.' >\"$RESULT_FILE\"; echo 'Reboot command issued. Result check will resume after boot.'; exit 0; fi\n");
        fprintf(f, "  if [ \"$reboot_exit\" -eq 125 ]; then\n");
        fprintf(f, "    ( set -a; [ -f \"$state_dir/env.sh\" ] && . \"$state_dir/env.sh\"; . \"$state_dir/post.sh\"; post_rc=\"$?\"; ");
        write_capture_checks(f, tc, "", "$actual_dir");
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
    write_command_expanded(f, tc->command);
    fprintf(f, "AUTOTEST_BODY_%03d\n", index + 1);
    write_validate_check_names(f, tc, "  ");
    fprintf(f, "  ( set -a; . \"$body_file\"; body_rc=\"$?\"; ");
    write_capture_checks(f, tc, "", "$actual_dir");
    fprintf(f, "exit \"$body_rc\" ) >\"$stdout_file\" 2>\"$stderr_file\"\n");
    fprintf(f, "  actual_exit=$?\n");
    fprintf(f, "  ok=1\n");
    fprintf(f, "  [ \"$actual_exit\" -eq %d ] || ok=0\n", tc->expected_exit);
    write_validate_checks(f, tc, "  ", "$actual_dir");
    fprintf(f, "  if [ \"$ok\" -eq 1 ]; then\n");
    fprintf(f, "    echo ");
    shell_quote(f, ok_msg);
    fprintf(f, "\n");
    fprintf(f, "    OK_COUNT=$((OK_COUNT + 1))\n");
    fprintf(f, "  else\n");
    fprintf(f, "    echo ");
    shell_quote(f, ng_prefix);
    fprintf(f, "\"$actual_exit\"\n");
    fprintf(f, "    echo \"     actual: $actual_dir\"\n");
    fprintf(f, "    echo \"     stdout: $stdout_file\"\n");
    fprintf(f, "    echo \"     stderr: $stderr_file\"\n");
    fprintf(f, "    NG_COUNT=$((NG_COUNT + 1))\n");
    fprintf(f, "  fi\n");
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
    fprintf(f, "  else\n");
    fprintf(f, "    log \"[NG] reboot test $safe_id result file was not found\"\n");
    fprintf(f, "    failures=$((failures + 1))\n");
    fprintf(f, "  fi\n");
    fprintf(f, "  write_failures \"$failures\"\n");
    fprintf(f, "  rm -f \"$WAIT_SAFE_FILE\" \"$WAIT_RESULT_FILE\"\n");
    fprintf(f, "}\n");
    fprintf(f, "mkdir -p \"$STATE_DIR\"\n");
    fprintf(f, "touch \"$SUMMARY_PATH\"\n");
    fprintf(f, "check_waiting_reboot\n");
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
    fprintf(f, "      failures=$((failures + 1))\n");
    fprintf(f, "      write_failures \"$failures\"\n");
    fprintf(f, "      rm -f \"$WAIT_SAFE_FILE\" \"$WAIT_RESULT_FILE\"\n");
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
    fprintf(f, "    write_failures \"$failures\"\n");
    fprintf(f, "  else\n");
    fprintf(f, "    bash \"$path\"\n");
    fprintf(f, "    rc=$?\n");
    fprintf(f, "    if [ \"$rc\" -ne 0 ]; then failures=$((failures + 1)); write_failures \"$failures\"; fi\n");
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
    int index = 1;
    for (int i = 0; i < app->project.case_count; i++) {
        TestCase *tc = &app->project.cases[i];
        if (!tc->selected) continue;
        if (!tc->script_path[0]) {
            if (write_single_test_script(tc) != 0) {
                printf("[NG] %s %s failed to create test script\n", tc->id, tc->title);
                continue;
            }
        }
        char quoted[LONG_LEN + 16];
        char cmd[LONG_LEN + 64];
        shell_quote_buf(quoted, sizeof(quoted), tc->script_path);
        snprintf(cmd, sizeof(cmd), "%s", quoted);
        printf("[%02d] %s %s\n%s\n", index++, tc->id, tc->title, tc->script_path);
        int rc = system(cmd);
        printf("exit=%d\n\n", rc);
        if (tc->kind == CMD_REBOOT) {
            printf("Reboot test was executed. If the machine rebooted, run remaining tests after boot.\n");
            break;
        }
    }
    printf("Automated test sequence finished. Press Enter to return to TUI.");
    fflush(stdout);
    (void)getchar();
    reset_prog_mode();
    refresh();
}

static void run_selected_menu(App *app) {
    switch (app->screen) {
    case SCREEN_DASHBOARD:
        if (app->selected_menu == 0) { app->screen = SCREEN_EDITOR; app->selected_menu = 0; }
        else if (app->selected_menu == 1) add_case(app);
        else if (app->selected_menu == 2) { app->selected_only = true; preview_script(app); app->screen = SCREEN_PREVIEW; app->selected_menu = 0; }
        else if (app->selected_menu == 3) {
            if (selected_count(&app->project) == 0) set_status(app, "Select at least one test before starting.");
            else { app->selected_only = true; app->run_after_generate = true; app->previous_screen = app->screen; app->screen = SCREEN_CONFIRM; app->selected_menu = 0; }
        }
        else set_status(app, "Use menu items with Enter. Esc exits.");
        break;
    case SCREEN_EDITOR:
        if (app->selected_menu == 0 && app->project.case_count) { app->editor_new_case = false; open_script_editor(app); }
        else if (app->selected_menu == 1 && app->project.case_count) { app->project.cases[app->selected_case].selected = !app->project.cases[app->selected_case].selected; save_test_registry(&app->project); }
        else if (app->selected_menu == 2) delete_case(app);
        else if (app->selected_menu == 3) { app->screen = SCREEN_MATCH; app->selected_menu = 0; }
        else if (app->selected_menu == 4) { app->selected_only = true; preview_script(app); app->screen = SCREEN_PREVIEW; app->selected_menu = 0; }
        else if (app->selected_menu == 5) {
            if (selected_count(&app->project) == 0) set_status(app, "Select at least one test before starting.");
            else { app->selected_only = true; app->run_after_generate = true; app->previous_screen = app->screen; app->screen = SCREEN_CONFIRM; app->selected_menu = 0; }
        }
        else if (app->selected_menu == 6) { app->screen = SCREEN_DASHBOARD; app->selected_menu = 0; }
        break;
    case SCREEN_FORM:
        if (app->selected_menu == 0 || app->selected_menu == 1) edit_selected_field(app);
        else if (app->selected_menu == 2 && app->project.case_count) { app->project.cases[app->selected_case].kind = CMD_SHELL; prompt_text("shell command", app->project.cases[app->selected_case].command, sizeof(app->project.cases[app->selected_case].command)); }
        else if (app->selected_menu == 3 && app->project.case_count) set_reboot_command(&app->project.cases[app->selected_case]);
        else if (app->selected_menu == 4 && app->project.case_count) set_vim_command(&app->project.cases[app->selected_case]);
        else if (app->selected_menu == 5) { app->screen = SCREEN_EDITOR; app->selected_menu = 0; set_status(app, "Saved test."); }
        else if (app->selected_menu == 6) { app->screen = SCREEN_EDITOR; app->selected_menu = 0; }
        break;
    case SCREEN_MATCH:
        if (app->project.case_count == 0) { app->screen = SCREEN_EDITOR; break; }
        if (app->selected_menu == 0) { CheckRule *check = primary_check(&app->project.cases[app->selected_case]); prompt_text("check_var", check->var, sizeof(check->var)); write_single_test_script(&app->project.cases[app->selected_case]); save_test_registry(&app->project); }
        else if (app->selected_menu == 1) { cycle_match(&primary_check(&app->project.cases[app->selected_case])->match); write_single_test_script(&app->project.cases[app->selected_case]); save_test_registry(&app->project); }
        else if (app->selected_menu == 2) { open_text_editor(app, EDIT_TARGET_CHECK_EXPECTED, SCREEN_MATCH); }
        else if (app->selected_menu == 3) { app->screen = SCREEN_EDITOR; app->selected_menu = 0; }
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
        if (app->selected_menu == 0) { app->selected_only = true; preview_script(app); app->screen = SCREEN_PREVIEW; app->selected_menu = 0; }
        else if (app->selected_menu == 1) {
            if (selected_count(&app->project) == 0) set_status(app, "Select at least one test before starting.");
            else { app->run_after_generate = true; app->previous_screen = app->screen; app->screen = SCREEN_CONFIRM; app->selected_menu = 0; }
        }
        else if (app->selected_menu == 2) { app->screen = SCREEN_EDITOR; app->selected_menu = 0; }
        break;
    case SCREEN_SCRIPT_EDITOR:
        break;
    case SCREEN_PREVIEW:
        if (app->selected_menu <= 3) {
            app->selected_preview_file = app->selected_menu;
            set_status(app, "Preview tabs are placeholders in MVP; main script preview is shown.");
        } else if (app->selected_menu == 4) { app->run_after_generate = false; app->previous_screen = app->screen; app->screen = SCREEN_CONFIRM; app->selected_menu = 0; }
        else if (app->selected_menu == 5) { app->screen = SCREEN_EDITOR; app->selected_menu = 0; }
        break;
    case SCREEN_RESULT:
        if (app->selected_menu == 0) { app->screen = SCREEN_EDITOR; app->selected_menu = 0; }
        else if (app->selected_menu == 1) { preview_script(app); app->screen = SCREEN_PREVIEW; app->selected_menu = 0; }
        else if (app->selected_menu == 2) { app->screen = SCREEN_DASHBOARD; app->selected_menu = 0; }
        break;
    case SCREEN_CONFIRM:
        if (app->selected_menu == 0) {
            if (app->run_after_generate) {
                start_selected_test_scripts(app);
                app->run_after_generate = false;
                app->screen = SCREEN_RESULT;
                app->selected_menu = 0;
                set_status(app, "Finished selected test sequence.");
            } else if (save_selected_test_scripts(app) == 0) {
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
        break;
    }
}

static int menu_count_for(Screen s) {
    switch (s) {
    case SCREEN_DASHBOARD: return 5;
    case SCREEN_EDITOR: return 7;
    case SCREEN_FORM: return 7;
    case SCREEN_MATCH: return 4;
    case SCREEN_CLEANUP: return 6;
    case SCREEN_REBOOT: return 3;
    case SCREEN_SCRIPT_EDITOR: return 1;
    case SCREEN_PREVIEW: return 6;
    case SCREEN_RESULT: return 3;
    case SCREEN_CONFIRM: return 2;
    }
    return 1;
}

static void handle_key(App *app, int ch) {
    if (app->screen == SCREEN_SCRIPT_EDITOR) {
        handle_script_editor_key(app, ch);
        return;
    }
    int mc = menu_count_for(app->screen);
    switch (ch) {
    case KEY_UP:
        if (app->screen == SCREEN_EDITOR) app->selected_case--;
        else if (app->screen == SCREEN_FORM) app->selected_field--;
        else if (app->screen == SCREEN_CLEANUP) app->selected_cleanup--;
        else if (app->screen == SCREEN_PREVIEW && app->preview_scroll > 0) app->preview_scroll--;
        break;
    case KEY_DOWN:
        if (app->screen == SCREEN_EDITOR) app->selected_case++;
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
    init_app(&app);

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    set_escdelay(25);
    curs_set(0);
    if (has_colors()) {
        start_color();
        use_default_colors();
    }

    bool running = true;
    while (running) {
        draw_app(&app);
        int ch = getch();
        if (app.screen != SCREEN_SCRIPT_EDITOR && ch == 27) {
            running = false;
            continue;
        }
        handle_key(&app, ch);
    }

    endwin();
    return 0;
}
