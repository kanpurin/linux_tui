#define _GNU_SOURCE

#include <ctype.h>
#include <curses.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#define REFRESH_MS 1000
#define INITIAL_CAP 256
#define MAX_NAME 128
#define MAX_USER 64
#define MAX_CMD 4096
#define STATUS_LEN 256

enum {
    COLOR_LABEL = 1,
    COLOR_SELECT,
    COLOR_ACCENT,
    COLOR_WARN,
    COLOR_TREE_PATH,
};

typedef struct {
    int pid;
    int ppid;
    char state;
    char name[MAX_NAME];
    char user[MAX_USER];
    long rss_kb;
    unsigned long long cpu_ticks;
    unsigned long long cpu_delta;
    unsigned long long start_ticks;
    double cpu_percent;
    double mem_percent;
    char *cmdline;
    int *children;
    size_t child_count;
    size_t child_cap;
} Process;

typedef struct {
    Process *items;
    size_t len;
    size_t cap;
} ProcessList;

typedef struct {
    int index;
    int depth;
    bool *more;
    size_t more_len;
} Row;

typedef struct {
    Row *items;
    size_t len;
    size_t cap;
} RowList;

typedef struct {
    int index;
    int depth;
    bool is_last;
    bool is_selected;
} TreeRow;

typedef struct {
    TreeRow *items;
    size_t len;
    size_t cap;
} TreeRowList;

typedef struct {
    int pid;
    unsigned long long ticks;
} TickEntry;

typedef struct {
    TickEntry *items;
    size_t len;
    size_t cap;
} TickList;

typedef struct {
    ProcessList processes;
    RowList rows;
    TreeRowList tree_rows;
    TickList previous;
    char filter[256];
    char status[STATUS_LEN];
    int selected;
    int scroll;
    int tree_selected;
    int tree_scroll;
    int focus_panel;
    bool tree_mode;
    long clock_ticks;
    long total_mem_kb;
    time_t boot_time;
    long last_refresh_ms;
} App;

static void build_tree_rows(App *app);

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void set_status(App *app, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(app->status, sizeof(app->status), fmt, args);
    va_end(args);
}

static void *xrealloc(void *ptr, size_t size) {
    void *next = realloc(ptr, size);
    if (!next) {
        endwin();
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    return next;
}

static char *xstrdup(const char *value) {
    char *copy = strdup(value ? value : "");
    if (!copy) {
        endwin();
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    return copy;
}

static void process_list_init(ProcessList *list) {
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

static void process_free(Process *proc) {
    free(proc->cmdline);
    free(proc->children);
}

static void process_list_clear(ProcessList *list) {
    for (size_t i = 0; i < list->len; i++) {
        process_free(&list->items[i]);
    }
    list->len = 0;
}

static void process_list_free(ProcessList *list) {
    process_list_clear(list);
    free(list->items);
}

static int process_list_push(ProcessList *list, Process proc) {
    if (list->len == list->cap) {
        list->cap = list->cap ? list->cap * 2 : INITIAL_CAP;
        list->items = xrealloc(list->items, list->cap * sizeof(list->items[0]));
    }
    list->items[list->len] = proc;
    return (int)list->len++;
}

static void row_list_init(RowList *list) {
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

static void row_list_clear(RowList *list) {
    for (size_t i = 0; i < list->len; i++) {
        free(list->items[i].more);
    }
    list->len = 0;
}

static void row_list_free(RowList *list) {
    row_list_clear(list);
    free(list->items);
}

static void row_list_push(RowList *list, int index, int depth, const bool *more, size_t more_len) {
    if (list->len == list->cap) {
        list->cap = list->cap ? list->cap * 2 : INITIAL_CAP;
        list->items = xrealloc(list->items, list->cap * sizeof(list->items[0]));
    }
    Row *row = &list->items[list->len++];
    row->index = index;
    row->depth = depth;
    row->more_len = more_len;
    row->more = NULL;
    if (more_len > 0) {
        row->more = xrealloc(NULL, more_len * sizeof(row->more[0]));
        memcpy(row->more, more, more_len * sizeof(row->more[0]));
    }
}

static void tree_row_list_init(TreeRowList *list) {
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

static void tree_row_list_clear(TreeRowList *list) {
    list->len = 0;
}

static void tree_row_list_free(TreeRowList *list) {
    free(list->items);
}

static void tree_row_list_push(TreeRowList *list, int index, int depth, bool is_last, bool is_selected) {
    if (list->len == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 64;
        list->items = xrealloc(list->items, list->cap * sizeof(list->items[0]));
    }
    list->items[list->len++] = (TreeRow){
        .index = index,
        .depth = depth,
        .is_last = is_last,
        .is_selected = is_selected,
    };
}

static void tick_list_init(TickList *list) {
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

static void tick_list_free(TickList *list) {
    free(list->items);
}

static unsigned long long tick_list_get(const TickList *list, int pid, unsigned long long fallback) {
    for (size_t i = 0; i < list->len; i++) {
        if (list->items[i].pid == pid) {
            return list->items[i].ticks;
        }
    }
    return fallback;
}

static void tick_list_set(TickList *list, int pid, unsigned long long ticks) {
    if (list->len == list->cap) {
        list->cap = list->cap ? list->cap * 2 : INITIAL_CAP;
        list->items = xrealloc(list->items, list->cap * sizeof(list->items[0]));
    }
    list->items[list->len++] = (TickEntry){.pid = pid, .ticks = ticks};
}

static void tick_list_replace_from_processes(TickList *ticks, const ProcessList *processes) {
    ticks->len = 0;
    for (size_t i = 0; i < processes->len; i++) {
        tick_list_set(ticks, processes->items[i].pid, processes->items[i].cpu_ticks);
    }
}

static int find_process_index(const ProcessList *list, int pid) {
    for (size_t i = 0; i < list->len; i++) {
        if (list->items[i].pid == pid) {
            return (int)i;
        }
    }
    return -1;
}

static void process_add_child(Process *proc, int child_index) {
    if (proc->child_count == proc->child_cap) {
        proc->child_cap = proc->child_cap ? proc->child_cap * 2 : 8;
        proc->children = xrealloc(proc->children, proc->child_cap * sizeof(proc->children[0]));
    }
    proc->children[proc->child_count++] = child_index;
}

static bool read_file_text(const char *path, char *buffer, size_t size) {
    FILE *file = fopen(path, "r");
    if (!file) {
        return false;
    }
    size_t read = fread(buffer, 1, size - 1, file);
    buffer[read] = '\0';
    fclose(file);
    return true;
}

static void read_cmdline(int pid, char *buffer, size_t size) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    FILE *file = fopen(path, "rb");
    if (!file) {
        buffer[0] = '\0';
        return;
    }
    size_t read = fread(buffer, 1, size - 1, file);
    fclose(file);
    buffer[read] = '\0';
    for (size_t i = 0; i < read; i++) {
        if (buffer[i] == '\0') {
            buffer[i] = ' ';
        }
    }
    while (read > 0 && isspace((unsigned char)buffer[read - 1])) {
        buffer[--read] = '\0';
    }
}

static void uid_to_name(uid_t uid, char *buffer, size_t size) {
    struct passwd *pw = getpwuid(uid);
    if (pw) {
        snprintf(buffer, size, "%s", pw->pw_name);
    } else {
        snprintf(buffer, size, "%u", (unsigned int)uid);
    }
}

static long read_total_mem_kb(void) {
    FILE *file = fopen("/proc/meminfo", "r");
    if (!file) {
        return 0;
    }
    char line[256];
    long total = 0;
    while (fgets(line, sizeof(line), file)) {
        if (sscanf(line, "MemTotal: %ld kB", &total) == 1) {
            break;
        }
    }
    fclose(file);
    return total;
}

static time_t read_boot_time(void) {
    FILE *file = fopen("/proc/stat", "r");
    if (!file) {
        return 0;
    }
    char line[256];
    long long boot = 0;
    while (fgets(line, sizeof(line), file)) {
        if (sscanf(line, "btime %lld", &boot) == 1) {
            break;
        }
    }
    fclose(file);
    return (time_t)boot;
}

static bool parse_stat(int pid, int *ppid, char *state, unsigned long long *ticks, unsigned long long *start_ticks) {
    char path[64];
    char buffer[8192];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    if (!read_file_text(path, buffer, sizeof(buffer))) {
        return false;
    }

    char *right = strrchr(buffer, ')');
    if (!right || right[1] != ' ') {
        return false;
    }

    char *cursor = right + 2;
    char parsed_state = '\0';
    int parsed_ppid = 0;
    unsigned long long utime = 0;
    unsigned long long stime = 0;
    unsigned long long parsed_start = 0;
    int field = 3;
    char *save = NULL;
    for (char *token = strtok_r(cursor, " ", &save); token; token = strtok_r(NULL, " ", &save), field++) {
        if (field == 3) {
            parsed_state = token[0];
        } else if (field == 4) {
            parsed_ppid = atoi(token);
        } else if (field == 14) {
            utime = strtoull(token, NULL, 10);
        } else if (field == 15) {
            stime = strtoull(token, NULL, 10);
        } else if (field == 22) {
            parsed_start = strtoull(token, NULL, 10);
            break;
        }
    }

    if (!parsed_state) {
        return false;
    }
    *ppid = parsed_ppid;
    *state = parsed_state;
    *ticks = utime + stime;
    *start_ticks = parsed_start;
    return true;
}

static bool parse_status(int pid, char *name, size_t name_size, uid_t *uid, long *rss_kb) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *file = fopen(path, "r");
    if (!file) {
        return false;
    }

    char line[512];
    bool have_name = false;
    *uid = 0;
    *rss_kb = 0;
    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, "Name:", 5) == 0) {
            char *value = line + 5;
            while (isspace((unsigned char)*value)) {
                value++;
            }
            value[strcspn(value, "\n")] = '\0';
            size_t len = strcspn(value, "\n");
            if (len >= name_size) {
                len = name_size - 1;
            }
            memcpy(name, value, len);
            name[len] = '\0';
            have_name = true;
        } else if (strncmp(line, "Uid:", 4) == 0) {
            unsigned int parsed_uid = 0;
            if (sscanf(line + 4, "%u", &parsed_uid) == 1) {
                *uid = (uid_t)parsed_uid;
            }
        } else if (strncmp(line, "VmRSS:", 6) == 0) {
            long parsed_rss = 0;
            if (sscanf(line + 6, "%ld", &parsed_rss) == 1) {
                *rss_kb = parsed_rss;
            }
        }
    }
    fclose(file);
    return have_name;
}

static bool read_process(int pid, Process *proc) {
    int ppid = 0;
    char state = '?';
    unsigned long long ticks = 0;
    unsigned long long start_ticks = 0;
    char name[MAX_NAME] = "?";
    uid_t uid = 0;
    long rss_kb = 0;
    char cmdline[MAX_CMD];

    if (!parse_stat(pid, &ppid, &state, &ticks, &start_ticks)) {
        return false;
    }
    if (!parse_status(pid, name, sizeof(name), &uid, &rss_kb)) {
        return false;
    }

    read_cmdline(pid, cmdline, sizeof(cmdline));
    if (cmdline[0] == '\0') {
        snprintf(cmdline, sizeof(cmdline), "[%s]", name);
    }

    memset(proc, 0, sizeof(*proc));
    proc->pid = pid;
    proc->ppid = ppid;
    proc->state = state;
    snprintf(proc->name, sizeof(proc->name), "%s", name);
    uid_to_name(uid, proc->user, sizeof(proc->user));
    proc->rss_kb = rss_kb;
    proc->cpu_ticks = ticks;
    proc->start_ticks = start_ticks;
    proc->cmdline = xstrdup(cmdline);
    return true;
}

static int compare_child_name(const void *left, const void *right, void *arg) {
    const ProcessList *list = arg;
    int a = *(const int *)left;
    int b = *(const int *)right;
    int name_cmp = strcasecmp(list->items[a].name, list->items[b].name);
    if (name_cmp != 0) {
        return name_cmp;
    }
    return list->items[a].pid - list->items[b].pid;
}

static int compare_list_rows(const void *left, const void *right, void *arg) {
    const ProcessList *list = arg;
    const Row *a = left;
    const Row *b = right;
    const Process *pa = &list->items[a->index];
    const Process *pb = &list->items[b->index];
    if (pa->cpu_delta < pb->cpu_delta) {
        return 1;
    }
    if (pa->cpu_delta > pb->cpu_delta) {
        return -1;
    }
    if (pa->rss_kb < pb->rss_kb) {
        return 1;
    }
    if (pa->rss_kb > pb->rss_kb) {
        return -1;
    }
    return pa->pid - pb->pid;
}

static int compare_pid_rows(const void *left, const void *right, void *arg) {
    const ProcessList *list = arg;
    const Row *a = left;
    const Row *b = right;
    return list->items[a->index].pid - list->items[b->index].pid;
}

static void sort_children(ProcessList *list) {
    for (size_t i = 0; i < list->len; i++) {
        if (list->items[i].child_count > 1) {
            qsort_r(list->items[i].children, list->items[i].child_count, sizeof(int), compare_child_name, list);
        }
    }
}

static bool contains_case_insensitive(const char *haystack, const char *needle) {
    if (!needle[0]) {
        return true;
    }
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) {
            return true;
        }
    }
    return false;
}

static bool process_matches(const Process *proc, const char *filter) {
    return contains_case_insensitive(proc->name, filter);
}

static void build_rows(App *app) {
    row_list_clear(&app->rows);
    if (app->processes.len == 0) {
        return;
    }

    for (size_t i = 0; i < app->processes.len; i++) {
        if (process_matches(&app->processes.items[i], app->filter)) {
            row_list_push(&app->rows, (int)i, 0, NULL, 0);
        }
    }
    qsort_r(app->rows.items, app->rows.len, sizeof(app->rows.items[0]),
            app->tree_mode ? compare_pid_rows : compare_list_rows, &app->processes);
}

static void snapshot_processes(App *app, bool reset_delta) {
    ProcessList next;
    process_list_init(&next);

    DIR *dir = opendir("/proc");
    if (!dir) {
        set_status(app, "failed to open /proc: %s", strerror(errno));
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!isdigit((unsigned char)entry->d_name[0])) {
            continue;
        }
        char *end = NULL;
        long pid = strtol(entry->d_name, &end, 10);
        if (*end != '\0' || pid <= 0 || pid > 99999999) {
            continue;
        }
        Process proc;
        if (read_process((int)pid, &proc)) {
            unsigned long long previous = tick_list_get(&app->previous, proc.pid, proc.cpu_ticks);
            proc.cpu_delta = reset_delta || proc.cpu_ticks < previous ? 0 : proc.cpu_ticks - previous;
            proc.cpu_percent = app->clock_ticks > 0 ? (proc.cpu_delta * 100.0) / (double)app->clock_ticks : 0.0;
            proc.mem_percent = app->total_mem_kb > 0 ? (proc.rss_kb * 100.0) / (double)app->total_mem_kb : 0.0;
            process_list_push(&next, proc);
        }
    }
    closedir(dir);

    for (size_t i = 0; i < next.len; i++) {
        int parent = find_process_index(&next, next.items[i].ppid);
        if (parent >= 0) {
            process_add_child(&next.items[parent], (int)i);
        }
    }
    sort_children(&next);

    process_list_clear(&app->processes);
    free(app->processes.items);
    app->processes = next;
    tick_list_replace_from_processes(&app->previous, &app->processes);
    build_rows(app);

    int max_selected = app->rows.len == 0 ? 0 : (int)app->rows.len - 1;
    if (app->selected > max_selected) {
        app->selected = max_selected;
    }
    if (app->selected < 0) {
        app->selected = 0;
    }
    build_tree_rows(app);
    app->last_refresh_ms = now_ms();
}

static void adjust_scroll(App *app, int body_height) {
    if (app->selected < app->scroll) {
        app->scroll = app->selected;
    } else if (app->selected >= app->scroll + body_height) {
        app->scroll = app->selected - body_height + 1;
    }

    int max_scroll = (int)app->rows.len - body_height;
    if (max_scroll < 0) {
        max_scroll = 0;
    }
    if (app->scroll > max_scroll) {
        app->scroll = max_scroll;
    }
    if (app->scroll < 0) {
        app->scroll = 0;
    }
}

static void draw_title_box(int y, int x, int h, int w, const char *title, bool focused) {
    if (h < 2 || w < 2) {
        return;
    }
    mvhline(y, x + 1, ACS_HLINE, w - 2);
    mvhline(y + h - 1, x + 1, ACS_HLINE, w - 2);
    mvvline(y + 1, x, ACS_VLINE, h - 2);
    mvvline(y + 1, x + w - 1, ACS_VLINE, h - 2);
    mvaddch(y, x, ACS_ULCORNER);
    mvaddch(y, x + w - 1, ACS_URCORNER);
    mvaddch(y + h - 1, x, ACS_LLCORNER);
    mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);
    if (title && title[0]) {
        if (focused) {
            attron(COLOR_PAIR(COLOR_LABEL) | A_BOLD);
        }
        mvprintw(y, x + 1, "%s", title);
        if (focused) {
            attroff(COLOR_PAIR(COLOR_LABEL) | A_BOLD);
        }
    }
}

static int selected_process_index(App *app) {
    if (app->rows.len == 0) {
        return -1;
    }
    return app->rows.items[app->selected].index;
}

static int active_process_index(App *app) {
    if (app->focus_panel == 4 && app->tree_rows.len > 0) {
        return app->tree_rows.items[app->tree_selected].index;
    }
    return selected_process_index(app);
}

static Process *active_process(App *app) {
    int index = active_process_index(app);
    if (index < 0) {
        return NULL;
    }
    return &app->processes.items[index];
}

static void append_tree_descendants(App *app, int index, int depth) {
    Process *proc = &app->processes.items[index];
    for (size_t i = 0; i < proc->child_count; i++) {
        int child = proc->children[i];
        tree_row_list_push(&app->tree_rows, child, depth, i + 1 == proc->child_count, false);
        append_tree_descendants(app, child, depth + 1);
    }
}

static void build_tree_rows(App *app) {
    int base = selected_process_index(app);
    int desired_pid = base >= 0 ? app->processes.items[base].pid : -1;
    if (app->focus_panel == 4 && app->tree_rows.len > 0 && app->tree_selected < (int)app->tree_rows.len) {
        desired_pid = app->processes.items[app->tree_rows.items[app->tree_selected].index].pid;
    }

    tree_row_list_clear(&app->tree_rows);
    app->tree_selected = 0;
    app->tree_scroll = 0;
    if (base < 0) {
        return;
    }

    int chain[256];
    size_t chain_len = 0;
    int current = base;
    while (current >= 0 && chain_len < sizeof(chain) / sizeof(chain[0])) {
        chain[chain_len++] = current;
        current = find_process_index(&app->processes, app->processes.items[current].ppid);
    }

    for (size_t i = 0; i < chain_len; i++) {
        int index = chain[chain_len - 1 - i];
        tree_row_list_push(&app->tree_rows, index, (int)i, true, index == base);
    }
    append_tree_descendants(app, base, (int)chain_len);

    for (size_t i = 0; i < app->tree_rows.len; i++) {
        if (app->processes.items[app->tree_rows.items[i].index].pid == desired_pid) {
            app->tree_selected = (int)i;
            return;
        }
    }
    for (size_t i = 0; i < app->tree_rows.len; i++) {
        if (app->tree_rows.items[i].is_selected) {
            app->tree_selected = (int)i;
            return;
        }
    }
}

static void draw_filter_bar(App *app, int width) {
    attron(COLOR_PAIR(COLOR_LABEL) | A_BOLD);
    mvaddstr(0, 0, "cmd name:");
    attroff(COLOR_PAIR(COLOR_LABEL) | A_BOLD);
    attron(COLOR_PAIR(COLOR_ACCENT));
    int input_x = 9;
    int input_w = width - input_x;
    if (input_w > 36) {
        input_w = 36;
    }
    mvhline(0, input_x, ' ', input_w);
    mvaddnstr(0, input_x, app->filter, input_w);
    attroff(COLOR_PAIR(COLOR_ACCENT));
}

static void draw_processes_panel(App *app, int y, int x, int h, int w) {
    draw_title_box(y, x, h, w, "processes", app->focus_panel == 0);
    if (h < 4 || w < 20) {
        return;
    }
    attron(COLOR_PAIR(COLOR_LABEL) | A_BOLD);
    mvwprintw(stdscr, y + 1, x + 1, "%-7s %-7s %s", "Pid", "PPid", "Cmd");
    attroff(COLOR_PAIR(COLOR_LABEL) | A_BOLD);

    int body_h = h - 3;
    adjust_scroll(app, body_h);
    for (int i = 0; i < body_h; i++) {
        int row_index = app->scroll + i;
        if (row_index >= (int)app->rows.len) {
            break;
        }
        Process *proc = &app->processes.items[app->rows.items[row_index].index];
        int attr = row_index == app->selected ? COLOR_PAIR(COLOR_SELECT) : A_NORMAL;
        attron(attr);
        int cmd_w = w - 18;
        if (cmd_w < 1) {
            cmd_w = 1;
        }
        mvprintw(y + 2 + i, x + 1, "%-7d %-7d %-*.*s", proc->pid, proc->ppid, cmd_w, cmd_w, proc->name);
        attroff(attr);
    }
}

static void format_started(const App *app, const Process *proc, char *buffer, size_t size) {
    if (app->boot_time == 0 || app->clock_ticks <= 0) {
        snprintf(buffer, size, "-");
        return;
    }
    time_t started = app->boot_time + (time_t)(proc->start_ticks / (unsigned long long)app->clock_ticks);
    struct tm tm_value;
    localtime_r(&started, &tm_value);
    strftime(buffer, size, "%m/%d %H:%M:%S %Y", &tm_value);
}

static void draw_info_panel(App *app, int y, int x, int h, int w, Process *proc) {
    draw_title_box(y, x, h, w, "process info", app->focus_panel == 1);
    if (!proc || h < 4) {
        return;
    }
    int inner_w = w - 5;
    if (inner_w < 1) {
        inner_w = 1;
    }
    attron(COLOR_PAIR(COLOR_LABEL) | A_BOLD);
    char header[160];
    snprintf(header, sizeof(header), "%-7s %-7s %6s %6s  %-20s  %s", "PID", "PPID", "%CPU", "%MEM", "STARTED", "USER");
    mvaddnstr(y + 1, x + 3, header, inner_w);
    attroff(COLOR_PAIR(COLOR_LABEL) | A_BOLD);
    char started[64];
    format_started(app, proc, started, sizeof(started));
    int user_w = w - 61;
    if (user_w < 1) {
        user_w = 1;
    }
    char line[256];
    snprintf(line, sizeof(line), "%-7d %-7d %6.1f %6.1f  %-20s  %-*.*s", proc->pid, proc->ppid, proc->cpu_percent,
             proc->mem_percent, started, user_w, user_w, proc->user);
    mvaddnstr(y + 2, x + 3, line, inner_w);
}

static const char *file_type_name(mode_t mode) {
    if (S_ISDIR(mode)) {
        return "DIR";
    }
    if (S_ISREG(mode)) {
        return "REG";
    }
    if (S_ISCHR(mode)) {
        return "CHR";
    }
    if (S_ISBLK(mode)) {
        return "BLK";
    }
    if (S_ISFIFO(mode)) {
        return "FIFO";
    }
    if (S_ISSOCK(mode)) {
        return "SOCK";
    }
    if (S_ISLNK(mode)) {
        return "LNK";
    }
    return "UNK";
}

static void draw_open_file_row(int y, int x, int w, Process *proc, const char *fd, const char *target) {
    struct stat st;
    char type[16] = "UNK";
    char device[32] = "-";
    if (stat(target, &st) == 0) {
        snprintf(type, sizeof(type), "%s", file_type_name(st.st_mode));
        snprintf(device, sizeof(device), "%u,%u", major(st.st_dev), minor(st.st_dev));
    }
    int device_w = w - 43;
    if (device_w < 1) {
        device_w = 1;
    }
    mvprintw(y, x + 1, "%-9.9s %-6d %-10.10s %-5.5s %-6.6s %*.*s", proc->name, proc->pid, proc->user, fd, type,
             device_w, device_w, device);
}

static void draw_open_files_panel(App *app, int y, int x, int h, int w, Process *proc) {
    draw_title_box(y, x, h, w, "process open files", app->focus_panel == 2);
    if (!proc || h < 4) {
        return;
    }
    attron(COLOR_PAIR(COLOR_LABEL) | A_BOLD);
    int device_w = w - 43;
    if (device_w < 1) {
        device_w = 1;
    }
    mvprintw(y + 1, x + 1, "%-9s %-6s %-10s %-5s %-6s %*s", "COMMAND", "PID", "USER", "FD", "TYPE", device_w, "DEVICE");
    attroff(COLOR_PAIR(COLOR_LABEL) | A_BOLD);

    int line = y + 2;
    int max_line = y + h - 1;
    char path[128];
    char target[4096];
    ssize_t len;

    snprintf(path, sizeof(path), "/proc/%d/cwd", proc->pid);
    len = readlink(path, target, sizeof(target) - 1);
    if (len >= 0 && line < max_line) {
        target[len] = '\0';
        draw_open_file_row(line++, x, w, proc, "cwd", target);
    }

    snprintf(path, sizeof(path), "/proc/%d/exe", proc->pid);
    len = readlink(path, target, sizeof(target) - 1);
    if (len >= 0 && line < max_line) {
        target[len] = '\0';
        draw_open_file_row(line++, x, w, proc, "txt", target);
    }

    snprintf(path, sizeof(path), "/proc/%d/fd", proc->pid);
    DIR *dir = opendir(path);
    if (!dir) {
        if (line < max_line) {
            mvprintw(line, x + 1, "permission denied or process exited");
        }
        return;
    }
    struct dirent *entry;
    while (line < max_line && (entry = readdir(dir))) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char fd_path[PATH_MAX];
        snprintf(fd_path, sizeof(fd_path), "%s/%s", path, entry->d_name);
        len = readlink(fd_path, target, sizeof(target) - 1);
        if (len < 0) {
            continue;
        }
        target[len] = '\0';
        draw_open_file_row(line++, x, w, proc, entry->d_name, target);
    }
    closedir(dir);
}

static void draw_environment_panel(App *app, int y, int x, int h, int w, Process *proc) {
    draw_title_box(y, x, h, w, "process environments", app->focus_panel == 3);
    if (!proc || h < 3) {
        return;
    }
    char path[128];
    snprintf(path, sizeof(path), "/proc/%d/environ", proc->pid);
    FILE *file = fopen(path, "rb");
    if (!file) {
        mvprintw(y + 1, x + 1, "permission denied or process exited");
        return;
    }

    char buffer[8192];
    size_t read = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[read] = '\0';

    int line = y + 1;
    int max_line = y + h - 1;
    size_t pos = 0;
    while (pos < read && line < max_line) {
        char *item = &buffer[pos];
        size_t len = strlen(item);
        if (len == 0) {
            pos++;
            continue;
        }
        char *eq = strchr(item, '=');
        if (eq) {
            *eq = '\0';
            attron(COLOR_PAIR(COLOR_LABEL) | A_BOLD);
            mvprintw(line, x + 1, "%-16.16s", item);
            attroff(COLOR_PAIR(COLOR_LABEL) | A_BOLD);
            mvaddnstr(line, x + 18, eq + 1, w - 20);
        } else {
            mvaddnstr(line, x + 1, item, w - 2);
        }
        line++;
        pos += len + 1;
    }
}

static void adjust_tree_scroll(App *app, int body_height) {
    if (app->tree_selected < app->tree_scroll) {
        app->tree_scroll = app->tree_selected;
    } else if (app->tree_selected >= app->tree_scroll + body_height) {
        app->tree_scroll = app->tree_selected - body_height + 1;
    }
    int max_scroll = (int)app->tree_rows.len - body_height;
    if (max_scroll < 0) {
        max_scroll = 0;
    }
    if (app->tree_scroll > max_scroll) {
        app->tree_scroll = max_scroll;
    }
    if (app->tree_scroll < 0) {
        app->tree_scroll = 0;
    }
}

static void draw_tree_panel(App *app, int y, int x, int h, int w, Process *proc) {
    draw_title_box(y, x, h, w, "process tree", app->focus_panel == 4);
    if (!proc || h < 3) {
        return;
    }
    int body_h = h - 2;
    adjust_tree_scroll(app, body_h);
    for (int i = 0; i < body_h; i++) {
        int row_index = app->tree_scroll + i;
        if (row_index >= (int)app->tree_rows.len) {
            break;
        }
        TreeRow *row = &app->tree_rows.items[row_index];
        Process *row_proc = &app->processes.items[row->index];
        int line_y = y + 1 + i;
        int indent = row->depth * 3;
        if (indent > w - 24) {
            indent = w - 24;
        }
        if (indent < 0) {
            indent = 0;
        }
        if (row->depth > 0) {
            mvprintw(line_y, x + 1, "%*s%s", indent, "", row->is_last ? "`-" : "|-");
        }
        int text_x = x + 1 + indent + (row->depth > 0 ? 2 : 0);
        int text_w = x + w - 1 - text_x;
        if (text_w < 1) {
            text_w = 1;
        }
        bool cursor = app->focus_panel == 4 && row_index == app->tree_selected;
        if (cursor) {
            attron(COLOR_PAIR(COLOR_SELECT));
        } else if (row->is_selected) {
            attron(COLOR_PAIR(COLOR_TREE_PATH));
        }
        int name_w = text_w - 17;
        if (name_w < 1) {
            name_w = 1;
        }
        mvprintw(line_y, text_x, "PID: %-6d CMD: %-*.*s", row_proc->pid, name_w, name_w, row_proc->name);
        if (cursor) {
            attroff(COLOR_PAIR(COLOR_SELECT));
        } else if (row->is_selected) {
            attroff(COLOR_PAIR(COLOR_TREE_PATH));
        }
    }
}

static void draw_app(App *app) {
    int height, width;
    getmaxyx(stdscr, height, width);
    erase();

    if (height < 20 || width < 80) {
        mvaddnstr(0, 0, "Terminal too small", width - 1);
        refresh();
        return;
    }

    draw_filter_bar(app, width);

    int top = 1;
    int footer_y = height - 1;
    int left_w = width / 3;
    if (left_w < 28) {
        left_w = 28;
    }
    if (left_w > 48) {
        left_w = 48;
    }
    int right_x = left_w + 1;
    int right_w = width - right_x - 1;
    int pane_h = footer_y - top;
    Process *proc = active_process(app);

    draw_processes_panel(app, top, 0, pane_h, left_w);

    int info_h = 5;
    int files_h = pane_h / 4 + 1;
    int env_h = pane_h / 4 + 1;
    int tree_h = pane_h - info_h - files_h - env_h;
    if (tree_h < 5) {
        tree_h = 5;
        env_h = pane_h - info_h - files_h - tree_h;
    }

    int y = top;
    draw_info_panel(app, y, right_x, info_h, right_w, proc);
    y += info_h + 1;
    draw_open_files_panel(app, y, right_x, files_h, right_w, proc);
    y += files_h + 1;
    draw_environment_panel(app, y, right_x, env_h, right_w, proc);
    y += env_h + 1;
    draw_tree_panel(app, y, right_x, footer_y - y, right_w, proc);

    attron(A_BOLD);
    mvaddstr(footer_y, 0, "Tab:");
    attroff(A_BOLD);
    mvaddstr(footer_y, 5, " next panel, ");
    attron(COLOR_PAIR(COLOR_WARN) | A_BOLD);
    addstr("Shift-Tab:");
    attroff(COLOR_PAIR(COLOR_WARN) | A_BOLD);
    addstr(" previous panel, ");
    attron(COLOR_PAIR(COLOR_WARN) | A_BOLD);
    addstr("K:");
    attroff(COLOR_PAIR(COLOR_WARN) | A_BOLD);
    addstr(" kill process, /: filter   ");
    addnstr(app->status, width - getcurx(stdscr) - 1);
    refresh();
}

static int prompt_char(const char *message) {
    int height, width;
    getmaxyx(stdscr, height, width);
    nodelay(stdscr, FALSE);
    curs_set(0);
    int h = 9;
    int w = 46;
    int y = (height - h) / 2;
    int x = (width - w) / 2;
    if (y < 1) {
        y = 1;
    }
    if (x < 1) {
        x = 1;
    }

    bool kill_selected_button = true;
    while (true) {
        attron(COLOR_PAIR(COLOR_ACCENT));
        for (int row = 0; row < h; row++) {
            mvhline(y + row, x, ' ', w);
        }
        mvhline(y, x + 1, ACS_HLINE, w - 2);
        mvhline(y + h - 1, x + 1, ACS_HLINE, w - 2);
        mvvline(y + 1, x, ACS_VLINE, h - 2);
        mvvline(y + 1, x + w - 1, ACS_VLINE, h - 2);
        mvaddch(y, x, ACS_ULCORNER);
        mvaddch(y, x + w - 1, ACS_URCORNER);
        mvaddch(y + h - 1, x, ACS_LLCORNER);
        mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);
        mvaddnstr(y + 2, x + 5, message, w - 10);
        attroff(COLOR_PAIR(COLOR_ACCENT));

        attron(kill_selected_button ? COLOR_PAIR(COLOR_SELECT) : COLOR_PAIR(COLOR_ACCENT));
        mvaddstr(y + 5, x + 10, "  kill  ");
        attroff(kill_selected_button ? COLOR_PAIR(COLOR_SELECT) : COLOR_PAIR(COLOR_ACCENT));

        attron(!kill_selected_button ? COLOR_PAIR(COLOR_SELECT) : COLOR_PAIR(COLOR_ACCENT));
        mvaddstr(y + 5, x + 23, "  Cancel  ");
        attroff(!kill_selected_button ? COLOR_PAIR(COLOR_SELECT) : COLOR_PAIR(COLOR_ACCENT));
        refresh();

        int ch = getch();
        switch (ch) {
        case KEY_LEFT:
        case KEY_RIGHT:
        case '\t':
        case 'h':
        case 'H':
        case 'l':
        case 'L':
            kill_selected_button = !kill_selected_button;
            break;
        case '\n':
        case '\r':
            nodelay(stdscr, TRUE);
            return kill_selected_button ? 'y' : 'n';
        case 'y':
        case 'Y':
        case 'k':
        case 'K':
            nodelay(stdscr, TRUE);
            return 'y';
        case 27:
        case 'n':
        case 'N':
        case 'c':
        case 'C':
            nodelay(stdscr, TRUE);
            return 'n';
        default:
            break;
        }
    }
    nodelay(stdscr, TRUE);
    return 'n';
}

static void edit_filter(App *app) {
    int width = getmaxx(stdscr);
    char input[sizeof(app->filter)] = "";
    nodelay(stdscr, FALSE);
    echo();
    curs_set(1);
    draw_filter_bar(app, width);
    int input_x = 9;
    int input_w = width - input_x;
    if (input_w > 36) {
        input_w = 36;
    }
    attron(COLOR_PAIR(COLOR_ACCENT));
    mvhline(0, input_x, ' ', input_w);
    attroff(COLOR_PAIR(COLOR_ACCENT));
    move(0, input_x);
    getnstr(input, input_w < (int)sizeof(input) ? input_w : (int)sizeof(input) - 1);
    noecho();
    curs_set(0);
    nodelay(stdscr, TRUE);

    snprintf(app->filter, sizeof(app->filter), "%s", input);
    app->selected = 0;
    app->scroll = 0;
    build_rows(app);
    build_tree_rows(app);
    set_status(app, "filter set: %s", app->filter[0] ? app->filter : "-");
}

static void kill_selected(App *app, int sig) {
    int index = active_process_index(app);
    if (index < 0) {
        return;
    }

    Process *proc = &app->processes.items[index];
    if (proc->pid == getpid()) {
        set_status(app, "refusing to kill procview itself");
        return;
    }

    char message[256];
    snprintf(message, sizeof(message), "Do you want to kill this process?");
    int answer = prompt_char(message);
    if (answer != 'y' && answer != 'Y') {
        set_status(app, "kill cancelled");
        return;
    }

    if (kill(proc->pid, sig) == 0) {
        set_status(app, "sent %s to PID %d", sig == SIGKILL ? "SIGKILL" : "SIGTERM", proc->pid);
        snapshot_processes(app, true);
    } else {
        set_status(app, "kill failed for PID %d: %s", proc->pid, strerror(errno));
    }
}

static void move_process_selection(App *app, int delta) {
    if (app->rows.len == 0) {
        app->selected = 0;
        return;
    }
    app->selected += delta;
    if (app->selected < 0) {
        app->selected = 0;
    }
    if (app->selected >= (int)app->rows.len) {
        app->selected = (int)app->rows.len - 1;
    }
    build_tree_rows(app);
}

static void move_tree_selection(App *app, int delta) {
    if (app->tree_rows.len == 0) {
        app->tree_selected = 0;
        return;
    }
    app->tree_selected += delta;
    if (app->tree_selected < 0) {
        app->tree_selected = 0;
    }
    if (app->tree_selected >= (int)app->tree_rows.len) {
        app->tree_selected = (int)app->tree_rows.len - 1;
    }
}

static bool handle_key(App *app, int ch) {
    if (ch == ERR) {
        return true;
    }

    switch (ch) {
    case 'q':
    case 'Q':
        return false;
    case KEY_DOWN:
    case 'j':
        if (app->focus_panel == 4) {
            move_tree_selection(app, 1);
        } else if (app->focus_panel == 0) {
            move_process_selection(app, 1);
        }
        break;
    case KEY_UP:
        if (app->focus_panel == 4) {
            move_tree_selection(app, -1);
        } else if (app->focus_panel == 0) {
            move_process_selection(app, -1);
        }
        break;
    case 'g':
    case KEY_HOME:
        if (app->focus_panel == 4) {
            app->tree_selected = 0;
        } else if (app->focus_panel == 0) {
            app->selected = 0;
            build_tree_rows(app);
        }
        break;
    case 'G':
    case KEY_END:
        if (app->focus_panel == 4) {
            app->tree_selected = app->tree_rows.len == 0 ? 0 : (int)app->tree_rows.len - 1;
        } else if (app->focus_panel == 0) {
            app->selected = app->rows.len == 0 ? 0 : (int)app->rows.len - 1;
            build_tree_rows(app);
        }
        break;
    case KEY_NPAGE:
        if (app->focus_panel == 4) {
            move_tree_selection(app, 10);
        } else if (app->focus_panel == 0) {
            move_process_selection(app, 10);
        }
        break;
    case KEY_PPAGE:
        if (app->focus_panel == 4) {
            move_tree_selection(app, -10);
        } else if (app->focus_panel == 0) {
            move_process_selection(app, -10);
        }
        break;
    case '/':
        edit_filter(app);
        break;
    case '\t':
        app->focus_panel = (app->focus_panel + 1) % 5;
        set_status(app, "panel: %s",
                   (const char *[]){"processes", "process info", "process open files", "process environments",
                                    "process tree"}[app->focus_panel]);
        break;
    case KEY_BTAB:
        app->focus_panel = (app->focus_panel + 4) % 5;
        set_status(app, "panel: %s",
                   (const char *[]){"processes", "process info", "process open files", "process environments",
                                    "process tree"}[app->focus_panel]);
        break;
    case 27:
        app->filter[0] = '\0';
        app->selected = 0;
        app->scroll = 0;
        build_rows(app);
        build_tree_rows(app);
        set_status(app, "filter cleared");
        break;
    case 't':
    case 'T':
        app->tree_mode = !app->tree_mode;
        app->selected = 0;
        app->scroll = 0;
        build_rows(app);
        build_tree_rows(app);
        set_status(app, "mode: %s", app->tree_mode ? "tree" : "list");
        break;
    case 'r':
    case 'R':
        snapshot_processes(app, true);
        set_status(app, "refreshed");
        break;
    case 'k':
    case KEY_DC:
    case '\n':
    case '\r':
        if (app->focus_panel == 0 || app->focus_panel == 4) {
            kill_selected(app, SIGTERM);
        } else {
            set_status(app, "move to processes or process tree to kill");
        }
        break;
    case 'K':
        if (app->focus_panel == 0 || app->focus_panel == 4) {
            kill_selected(app, SIGKILL);
        } else {
            set_status(app, "move to processes or process tree to kill");
        }
        break;
    default:
        break;
    }
    return true;
}

static void app_init(App *app) {
    memset(app, 0, sizeof(*app));
    process_list_init(&app->processes);
    row_list_init(&app->rows);
    tree_row_list_init(&app->tree_rows);
    tick_list_init(&app->previous);
    app->tree_mode = true;
    app->clock_ticks = sysconf(_SC_CLK_TCK);
    app->total_mem_kb = read_total_mem_kb();
    app->boot_time = read_boot_time();
    snprintf(app->status, sizeof(app->status), "ready");
}

static void app_free(App *app) {
    process_list_free(&app->processes);
    row_list_free(&app->rows);
    tree_row_list_free(&app->tree_rows);
    tick_list_free(&app->previous);
}

int main(void) {
    if (access("/proc", R_OK) != 0) {
        fprintf(stderr, "procview requires Linux /proc\n");
        return 1;
    }

    App app;
    app_init(&app);

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);
    use_default_colors();
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(COLOR_LABEL, COLOR_YELLOW, -1);
        init_pair(COLOR_SELECT, COLOR_BLACK, COLOR_WHITE);
        init_pair(COLOR_ACCENT, COLOR_WHITE, COLOR_BLUE);
        init_pair(COLOR_WARN, COLOR_RED, -1);
        init_pair(COLOR_TREE_PATH, COLOR_GREEN, -1);
    }

    snapshot_processes(&app, true);
    bool running = true;
    while (running) {
        if (now_ms() - app.last_refresh_ms >= REFRESH_MS) {
            snapshot_processes(&app, false);
        }
        draw_app(&app);
        running = handle_key(&app, getch());
        napms(30);
    }

    endwin();
    app_free(&app);
    return 0;
}
