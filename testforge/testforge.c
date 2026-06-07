#define _GNU_SOURCE

#include <ctype.h>
#include <curses.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_PARAMS 8
#define MAX_JOBS 64
#define MAX_LOGS 256
#define MAX_TEXT 256

enum {
    COLOR_LABEL = 1,
    COLOR_SELECT,
    COLOR_ACCENT,
    COLOR_WARN,
    COLOR_OK,
};

typedef enum {
    PARAM_INT,
    PARAM_BOOL,
    PARAM_TEXT,
    PARAM_CHOICE,
} ParamType;

typedef enum {
    DANGER_LOW,
    DANGER_MEDIUM,
    DANGER_HIGH,
    DANGER_CRITICAL,
} Danger;

typedef enum {
    VIEW_CATEGORIES,
    VIEW_ACTIONS,
    VIEW_CONFIG,
} ViewMode;

typedef enum {
    ACTION_MEMORY_PRESSURE,
    ACTION_MEMORY_LEAK,
    ACTION_MEMORY_FRAGMENTATION,
    ACTION_OOM_PRESSURE,
    ACTION_LARGE_FILE,
    ACTION_DISK_FILL,
    ACTION_INODE_FILL,
    ACTION_RANDOM_WRITE,
    ACTION_FSYNC_STORM,
    ACTION_CPU_BURN,
    ACTION_PROCESS_FLOOD,
    ACTION_ZOMBIE_PROCESS,
    ACTION_ORPHAN_PROCESS,
    ACTION_FD_EXHAUSTION,
    ACTION_REGULAR_FILE,
    ACTION_DIRECTORY,
    ACTION_DEEP_DIRECTORY,
    ACTION_HUGE_DIRECTORY,
    ACTION_PERMISSION_PATTERN,
    ACTION_SYMLINK,
    ACTION_BROKEN_SYMLINK,
    ACTION_SYMLINK_LOOP,
    ACTION_HARD_LINK,
    ACTION_FIFO,
    ACTION_UNIX_SOCKET,
    ACTION_CHAR_DEVICE,
    ACTION_BLOCK_DEVICE,
    ACTION_TCP_SERVER,
    ACTION_TCP_FLOOD,
    ACTION_SLOW_SERVER,
    ACTION_NO_RESPONSE_SERVER,
    ACTION_IMMEDIATE_DISCONNECT,
    ACTION_OPEN_FAILURE,
    ACTION_READ_FAILURE,
    ACTION_DEVICE_BUSY,
    ACTION_ENOSPC,
    ACTION_TIMEOUT,
    ACTION_SIGNAL_INJECTION,
    ACTION_FILE_TAMPER,
} ActionId;

typedef struct {
    const char *key;
    const char *label;
    ParamType type;
    const char *default_value;
    const char *choices;
} ParamDef;

typedef struct {
    ActionId id;
    const char *category;
    const char *name;
    const char *summary;
    Danger danger;
    bool root_required;
    ParamDef params[MAX_PARAMS];
    int param_count;
} ActionDef;

typedef struct {
    char value[MAX_TEXT];
} ParamValue;

typedef struct {
    const ActionDef *action;
    ParamValue params[MAX_PARAMS];
} ScenarioStep;

typedef enum {
    JOB_RUNNING,
    JOB_EXITED,
    JOB_FAILED,
    JOB_DRY_RUN,
} JobState;

typedef struct {
    pid_t pid;
    const ActionDef *action;
    time_t started;
    JobState state;
    int exit_code;
    char detail[MAX_TEXT];
} Job;

typedef struct {
    Job items[MAX_JOBS];
    int len;
} JobList;

typedef struct {
    char items[MAX_LOGS][MAX_TEXT];
    int len;
} EventLog;

typedef struct {
    char search[128];
    int focus;
    ViewMode view;
    bool category_open;
    int opened_category_index;
    int category_selected;
    int category_scroll;
    int action_selected;
    int action_scroll;
    int param_selected;
    bool dry_run;
    long mem_available_mb;
    ScenarioStep draft;
    bool draft_valid;
    JobList jobs;
    EventLog logs;
    char status[MAX_TEXT];
} App;

typedef struct {
    const char *name;
    const char *summary;
} CategoryDef;

static const CategoryDef categories[] = {
    {"Memory", "Memory pressure, leak, fragmentation, and OOM-oriented tests."},
    {"Disk", "Large files, disk fill, inode fill, random write, and fsync tests."},
    {"Process", "CPU burn, process flood, zombie/orphan process, and FD exhaustion tests."},
    {"File", "Filesystem object and path edge-case generation."},
    {"Network", "TCP servers, connection floods, slow/no response behavior."},
    {"Failure Injection", "Signals, timeout, ENOSPC, EBUSY, and tamper recipes."},
};

static const int category_count = (int)(sizeof(categories) / sizeof(categories[0]));

static const ActionDef actions[] = {
    {ACTION_MEMORY_PRESSURE, "Memory", "Memory Pressure", "Allocate memory and optionally touch pages.", DANGER_MEDIUM, false,
     {{"size_mb", "Size MB", PARAM_INT, "256", NULL},
      {"touch_pages", "Touch Pages", PARAM_BOOL, "true", NULL},
      {"duration_sec", "Duration Sec", PARAM_INT, "60", NULL}},
     3},
    {ACTION_MEMORY_LEAK, "Memory", "Memory Leak", "Allocate memory at a fixed growth rate.", DANGER_HIGH, false,
     {{"mb_per_sec", "MB / Sec", PARAM_INT, "16", NULL},
      {"max_mb", "Max MB", PARAM_INT, "512", NULL},
      {"duration_sec", "Duration Sec", PARAM_INT, "60", NULL}},
     3},
    {ACTION_MEMORY_FRAGMENTATION, "Memory", "Memory Fragmentation", "Repeated malloc/free fragmentation pattern.", DANGER_MEDIUM, false,
     {{"blocks", "Blocks", PARAM_INT, "4096", NULL}, {"iterations", "Iterations", PARAM_INT, "10000", NULL}}, 2},
    {ACTION_OOM_PRESSURE, "Memory", "OOM Pressure", "Attempt to trigger OOM killer.", DANGER_CRITICAL, false,
     {{"target_mb", "Target MB", PARAM_INT, "4096", NULL},
      {"touch_pages", "Touch Pages", PARAM_BOOL, "true", NULL},
      {"duration_sec", "Duration Sec", PARAM_INT, "60", NULL}},
     3},

    {ACTION_LARGE_FILE, "Disk", "Large File Creation", "Create sparse, zero, random, or pattern-filled file.", DANGER_MEDIUM, false,
     {{"size_mb", "Size MB", PARAM_INT, "1024", NULL},
      {"path", "Output Path", PARAM_TEXT, "/tmp/testforge-large-file.bin", NULL},
      {"mode", "Mode", PARAM_CHOICE, "sparse", "sparse|zero|random|pattern"},
      {"pattern", "Pattern", PARAM_TEXT, "TF", NULL}},
     4},
    {ACTION_DISK_FILL, "Disk", "Disk Fill", "Fill target directory until requested usage.", DANGER_HIGH, false,
     {{"target_dir", "Target Dir", PARAM_TEXT, "/tmp", NULL}, {"target_percent", "Target %", PARAM_INT, "90", NULL}}, 2},
    {ACTION_INODE_FILL, "Disk", "Inode Fill", "Create many small files.", DANGER_HIGH, false,
     {{"target_dir", "Target Dir", PARAM_TEXT, "/tmp/testforge-inodes", NULL},
      {"count", "Count", PARAM_INT, "10000", NULL},
      {"file_size", "File Size", PARAM_INT, "0", NULL}},
     3},
    {ACTION_RANDOM_WRITE, "Disk", "Random Write", "Random write workload.", DANGER_MEDIUM, false,
     {{"path", "Path", PARAM_TEXT, "/tmp/testforge-random.bin", NULL}, {"size_mb", "Size MB", PARAM_INT, "512", NULL}}, 2},
    {ACTION_FSYNC_STORM, "Disk", "fsync Storm", "Issue many fsync calls.", DANGER_MEDIUM, false,
     {{"base", "Base", PARAM_TEXT, "/tmp/testforge-fsync", NULL},
      {"files", "Files", PARAM_INT, "128", NULL},
      {"iterations", "Iterations", PARAM_INT, "1000", NULL}},
     3},

    {ACTION_CPU_BURN, "Process", "CPU Burn", "Run CPU busy loops for the selected duration.", DANGER_MEDIUM, false,
     {{"workers", "Workers", PARAM_INT, "1", NULL}, {"duration_sec", "Duration Sec", PARAM_INT, "60", NULL}}, 2},
    {ACTION_PROCESS_FLOOD, "Process", "Process Flood", "Spawn many processes.", DANGER_HIGH, false,
     {{"processes", "Processes", PARAM_INT, "100", NULL},
      {"rate_per_sec", "Rate / Sec", PARAM_INT, "20", NULL},
      {"duration_sec", "Duration Sec", PARAM_INT, "60", NULL}},
     3},
    {ACTION_ZOMBIE_PROCESS, "Process", "Zombie Process", "Create zombie children.", DANGER_MEDIUM, false,
     {{"count", "Count", PARAM_INT, "10", NULL}, {"duration_sec", "Duration Sec", PARAM_INT, "60", NULL}}, 2},
    {ACTION_ORPHAN_PROCESS, "Process", "Orphan Process", "Create orphan process pattern.", DANGER_MEDIUM, false,
     {{"count", "Count", PARAM_INT, "10", NULL}, {"duration_sec", "Duration Sec", PARAM_INT, "60", NULL}}, 2},
    {ACTION_FD_EXHAUSTION, "Process", "FD Exhaustion", "Open many file descriptors.", DANGER_HIGH, false,
     {{"fds", "FDs", PARAM_INT, "1024", NULL}, {"duration_sec", "Duration Sec", PARAM_INT, "60", NULL}}, 2},

    {ACTION_REGULAR_FILE, "File", "Regular File", "Create a regular file.", DANGER_LOW, false,
     {{"path", "Path", PARAM_TEXT, "/tmp/testforge-file", NULL}}, 1},
    {ACTION_DIRECTORY, "File", "Directory", "Create a directory.", DANGER_LOW, false,
     {{"path", "Path", PARAM_TEXT, "/tmp/testforge-dir", NULL}}, 1},
    {ACTION_DEEP_DIRECTORY, "File", "Deep Directory", "Create deep directory tree for PATH_MAX testing.", DANGER_MEDIUM, false,
     {{"base", "Base", PARAM_TEXT, "/tmp/testforge-deep", NULL}, {"depth", "Depth", PARAM_INT, "64", NULL}}, 2},
    {ACTION_HUGE_DIRECTORY, "File", "Huge Directory", "Create many files in a directory.", DANGER_MEDIUM, false,
     {{"base", "Base", PARAM_TEXT, "/tmp/testforge-huge", NULL}, {"count", "Count", PARAM_INT, "10000", NULL}}, 2},
    {ACTION_PERMISSION_PATTERN, "File", "Permission Pattern", "Create chmod 000 through 777 samples.", DANGER_LOW, false,
     {{"base", "Base", PARAM_TEXT, "/tmp/testforge-perms", NULL}}, 1},
    {ACTION_SYMLINK, "File", "Symlink", "Create symbolic link.", DANGER_LOW, false,
     {{"target", "Target", PARAM_TEXT, "/tmp/target", NULL}, {"link", "Link", PARAM_TEXT, "/tmp/link", NULL}}, 2},
    {ACTION_BROKEN_SYMLINK, "File", "Broken Symlink", "Create dangling symbolic link.", DANGER_LOW, false,
     {{"link", "Link", PARAM_TEXT, "/tmp/broken-link", NULL}}, 1},
    {ACTION_SYMLINK_LOOP, "File", "Symlink Loop", "Create A -> B -> C -> A loop.", DANGER_MEDIUM, false,
     {{"base", "Base", PARAM_TEXT, "/tmp/testforge-loop", NULL}}, 1},
    {ACTION_HARD_LINK, "File", "Hard Link", "Create hard link.", DANGER_LOW, false,
     {{"source", "Source", PARAM_TEXT, "/tmp/source", NULL}, {"link", "Link", PARAM_TEXT, "/tmp/hardlink", NULL}}, 2},
    {ACTION_FIFO, "File", "FIFO", "Create named pipe.", DANGER_LOW, false,
     {{"path", "Path", PARAM_TEXT, "/tmp/testforge.fifo", NULL}}, 1},
    {ACTION_UNIX_SOCKET, "File", "Unix Domain Socket", "Create UNIX domain socket path.", DANGER_LOW, false,
     {{"path", "Path", PARAM_TEXT, "/tmp/testforge.sock", NULL}}, 1},
    {ACTION_CHAR_DEVICE, "File", "Character Device", "Create character device with mknod.", DANGER_HIGH, true,
     {{"path", "Path", PARAM_TEXT, "/tmp/testforge-char", NULL}, {"major", "Major", PARAM_INT, "1", NULL}, {"minor", "Minor", PARAM_INT, "7", NULL}}, 3},
    {ACTION_BLOCK_DEVICE, "File", "Block Device", "Create block device with mknod.", DANGER_HIGH, true,
     {{"path", "Path", PARAM_TEXT, "/tmp/testforge-block", NULL}, {"major", "Major", PARAM_INT, "7", NULL}, {"minor", "Minor", PARAM_INT, "0", NULL}}, 3},

    {ACTION_TCP_SERVER, "Network", "TCP Server", "Listen on a TCP port.", DANGER_LOW, false,
     {{"port", "Port", PARAM_INT, "8080", NULL}, {"duration_sec", "Duration Sec", PARAM_INT, "60", NULL}}, 2},
    {ACTION_TCP_FLOOD, "Network", "TCP Connection Flood", "Open many TCP connections.", DANGER_HIGH, false,
     {{"host", "Host", PARAM_TEXT, "127.0.0.1", NULL},
      {"port", "Port", PARAM_INT, "8080", NULL},
      {"connections", "Connections", PARAM_INT, "1000", NULL},
      {"duration_sec", "Duration Sec", PARAM_INT, "60", NULL}},
     4},
    {ACTION_SLOW_SERVER, "Network", "Slow Response Server", "Delay response after receive.", DANGER_LOW, false,
     {{"port", "Port", PARAM_INT, "8080", NULL},
      {"delay_ms", "Delay ms", PARAM_INT, "5000", NULL},
      {"duration_sec", "Duration Sec", PARAM_INT, "60", NULL}},
     3},
    {ACTION_NO_RESPONSE_SERVER, "Network", "No Response Server", "Accept connections and never respond.", DANGER_LOW, false,
     {{"port", "Port", PARAM_INT, "8080", NULL}, {"duration_sec", "Duration Sec", PARAM_INT, "60", NULL}}, 2},
    {ACTION_IMMEDIATE_DISCONNECT, "Network", "Immediate Disconnect", "Accept and immediately close.", DANGER_LOW, false,
     {{"port", "Port", PARAM_INT, "8080", NULL}, {"duration_sec", "Duration Sec", PARAM_INT, "60", NULL}}, 2},

    {ACTION_OPEN_FAILURE, "Failure Injection", "Open Failure", "Generate open failure recipe.", DANGER_MEDIUM, false,
     {{"path", "Path", PARAM_TEXT, "/tmp/no-such-file", NULL}, {"errno", "Errno", PARAM_TEXT, "ENOENT", NULL}}, 2},
    {ACTION_READ_FAILURE, "Failure Injection", "Read Failure", "Generate read failure recipe.", DANGER_MEDIUM, false,
     {{"path", "Path", PARAM_TEXT, "/", NULL}, {"errno", "Errno", PARAM_TEXT, "EISDIR", NULL}}, 2},
    {ACTION_DEVICE_BUSY, "Failure Injection", "Device Busy", "Generate EBUSY reproduction recipe.", DANGER_MEDIUM, false,
     {{"path", "Path", PARAM_TEXT, "/tmp/testforge-busy", NULL}, {"duration_sec", "Duration Sec", PARAM_INT, "60", NULL}}, 2},
    {ACTION_ENOSPC, "Failure Injection", "ENOSPC", "Generate capacity exhaustion recipe.", DANGER_HIGH, false,
     {{"target_dir", "Target Dir", PARAM_TEXT, "/tmp", NULL}}, 1},
    {ACTION_TIMEOUT, "Failure Injection", "Timeout", "Delay response or operation.", DANGER_LOW, false,
     {{"duration_sec", "Duration Sec", PARAM_INT, "30", NULL}}, 1},
    {ACTION_SIGNAL_INJECTION, "Failure Injection", "Signal Injection", "Send a signal to target process.", DANGER_HIGH, false,
     {{"pid", "PID", PARAM_INT, "1", NULL}, {"signal", "Signal", PARAM_CHOICE, "SIGTERM", "SIGTERM|SIGKILL|SIGSEGV"}}, 2},
    {ACTION_FILE_TAMPER, "Failure Injection", "File Tamper", "Modify a file after delay.", DANGER_MEDIUM, false,
     {{"path", "Path", PARAM_TEXT, "/tmp/target", NULL}, {"delay_sec", "Delay Sec", PARAM_INT, "10", NULL}, {"text", "Text", PARAM_TEXT, "tampered", NULL}}, 3},
};

static const int action_count = (int)(sizeof(actions) / sizeof(actions[0]));

static void set_status(App *app, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(app->status, sizeof(app->status), fmt, args);
    va_end(args);
}

static void log_event(App *app, const char *fmt, ...) {
    if (app->logs.len == MAX_LOGS) {
        memmove(app->logs.items, app->logs.items + 1, sizeof(app->logs.items[0]) * (MAX_LOGS - 1));
        app->logs.len--;
    }
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char prefix[32];
    strftime(prefix, sizeof(prefix), "[%H:%M:%S] ", &tmv);
    va_list args;
    va_start(args, fmt);
    char msg[MAX_TEXT];
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    char *target = app->logs.items[app->logs.len++];
    snprintf(target, MAX_TEXT, "%s", prefix);
    size_t used = strlen(target);
    size_t available = MAX_TEXT - used - 1;
    size_t copy_len = strlen(msg);
    if (copy_len > available) {
        copy_len = available;
    }
    memcpy(target + used, msg, copy_len);
    target[used + copy_len] = '\0';
}

static int param_int(const ScenarioStep *step, const char *key) {
    for (int i = 0; i < step->action->param_count; i++) {
        if (strcmp(step->action->params[i].key, key) == 0) {
            return atoi(step->params[i].value);
        }
    }
    return 0;
}

static const char *param_text(const ScenarioStep *step, const char *key) {
    for (int i = 0; i < step->action->param_count; i++) {
        if (strcmp(step->action->params[i].key, key) == 0) {
            return step->params[i].value;
        }
    }
    return "";
}

static bool param_bool(const ScenarioStep *step, const char *key) {
    const char *value = param_text(step, key);
    return strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "yes") == 0;
}

static long read_mem_available_mb(void) {
    FILE *file = fopen("/proc/meminfo", "r");
    if (!file) {
        return 0;
    }
    char line[256];
    long kb = 0;
    while (fgets(line, sizeof(line), file)) {
        if (sscanf(line, "MemAvailable: %ld kB", &kb) == 1) {
            break;
        }
    }
    fclose(file);
    return kb > 0 ? kb / 1024 : 0;
}

static int param_limit_mb(const App *app, const ScenarioStep *step, int param_index) {
    if (!step || param_index < 0 || param_index >= step->action->param_count) {
        return 0;
    }
    const ParamDef *param = &step->action->params[param_index];
    if ((step->action->id == ACTION_MEMORY_PRESSURE || step->action->id == ACTION_MEMORY_LEAK ||
         step->action->id == ACTION_OOM_PRESSURE) &&
        (strcmp(param->key, "size_mb") == 0 || strcmp(param->key, "max_mb") == 0 ||
         strcmp(param->key, "target_mb") == 0)) {
        return app->mem_available_mb > 0 ? (int)app->mem_available_mb : 0;
    }
    return 0;
}

static void clamp_step_params(App *app, ScenarioStep *step) {
    if (!step) {
        return;
    }
    for (int i = 0; i < step->action->param_count; i++) {
        int limit = param_limit_mb(app, step, i);
        if (limit > 0 && atoi(step->params[i].value) > limit) {
            snprintf(step->params[i].value, sizeof(step->params[i].value), "%d", limit);
        }
    }
}

static const char *param_help(const ScenarioStep *step, int param_index) {
    if (!step || param_index < 0 || param_index >= step->action->param_count) {
        return "";
    }
    const ParamDef *param = &step->action->params[param_index];
    switch (step->action->id) {
    case ACTION_MEMORY_PRESSURE:
        if (strcmp(param->key, "size_mb") == 0) {
            return "Allocate this many MB. The value is capped by current MemAvailable.";
        }
        if (strcmp(param->key, "touch_pages") == 0) {
            return "When true, write to each page so Linux really backs the allocation with memory.";
        }
        if (strcmp(param->key, "duration_sec") == 0) {
            return "Keep the allocated memory for this many seconds, then release it.";
        }
        break;
    case ACTION_CPU_BURN:
        if (strcmp(param->key, "workers") == 0) {
            return "Number of child processes that run CPU busy loops at the same time.";
        }
        if (strcmp(param->key, "duration_sec") == 0) {
            return "Run the CPU workers for this many seconds, then stop them.";
        }
        break;
    case ACTION_LARGE_FILE:
        if (strcmp(param->key, "size_mb") == 0) {
            return "Create a file with this logical size in MB.";
        }
        if (strcmp(param->key, "path") == 0) {
            return "Write the generated file to this path. Existing files are truncated except sparse mode opens without truncation first.";
        }
        if (strcmp(param->key, "mode") == 0) {
            return "sparse uses ftruncate, zero writes zero bytes, random writes /dev/urandom data, pattern repeats the Pattern text.";
        }
        if (strcmp(param->key, "pattern") == 0) {
            return "Text repeated into the file when Mode is pattern. Ignored by sparse, zero, and random.";
        }
        break;
    default:
        break;
    }
    if (strcmp(param->key, "duration_sec") == 0) {
        return "Keep this action active for this many seconds. Zero is treated as one second.";
    }
    if (strcmp(param->key, "delay_sec") == 0) {
        return "Wait this many seconds before performing the action.";
    }
    if (strcmp(param->key, "delay_ms") == 0) {
        return "Wait this many milliseconds before sending a response.";
    }
    if (strcmp(param->key, "target_dir") == 0 || strcmp(param->key, "base") == 0) {
        return "Directory used as the root for generated files or directories.";
    }
    if (strcmp(param->key, "size_mb") == 0 || strcmp(param->key, "max_mb") == 0 ||
        strcmp(param->key, "target_mb") == 0) {
        return "Memory or file size in MB. Memory values are capped by current MemAvailable when applicable.";
    }
    if (strcmp(param->key, "path") == 0 || strcmp(param->key, "source") == 0 || strcmp(param->key, "target") == 0 ||
        strcmp(param->key, "link") == 0) {
        return "Filesystem path used by this action.";
    }
    if (strcmp(param->key, "count") == 0 || strcmp(param->key, "blocks") == 0 || strcmp(param->key, "files") == 0 ||
        strcmp(param->key, "iterations") == 0 || strcmp(param->key, "processes") == 0 ||
        strcmp(param->key, "connections") == 0 || strcmp(param->key, "fds") == 0) {
        return "Amount of objects or loop iterations this action creates or performs.";
    }
    if (strcmp(param->key, "rate_per_sec") == 0 || strcmp(param->key, "mb_per_sec") == 0) {
        return "Maximum creation or allocation rate per second.";
    }
    if (strcmp(param->key, "file_size") == 0) {
        return "Bytes written to each generated file. Zero creates empty files.";
    }
    if (strcmp(param->key, "target_percent") == 0) {
        return "Stop writing when filesystem usage reaches this percentage.";
    }
    if (strcmp(param->key, "host") == 0) {
        return "IPv4 address to connect to.";
    }
    if (strcmp(param->key, "port") == 0) {
        return "TCP port used for listening or connecting.";
    }
    if (strcmp(param->key, "major") == 0 || strcmp(param->key, "minor") == 0) {
        return "Device number passed to mknod. Creating device nodes usually requires root.";
    }
    if (strcmp(param->key, "errno") == 0) {
        return "Expected errno name or number. The action succeeds when that error is observed.";
    }
    if (strcmp(param->key, "pid") == 0) {
        return "Target process ID for signal delivery.";
    }
    if (strcmp(param->key, "signal") == 0) {
        return "Signal sent to the target PID.";
    }
    if (strcmp(param->key, "text") == 0) {
        return "Text appended to the target file.";
    }
    if (param->type == PARAM_INT) {
        return "Numeric value. Only digits can be entered.";
    }
    if (param->type == PARAM_BOOL) {
        return "Boolean value. Use Left/Right to toggle.";
    }
    if (param->type == PARAM_CHOICE) {
        return "Choice value. Use Left/Right to cycle options.";
    }
    return "Text value.";
}

static bool contains_ci(const char *haystack, const char *needle) {
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

static bool action_visible(const ActionDef *action, const char *search) {
    return contains_ci(action->category, search) || contains_ci(action->name, search) || contains_ci(action->summary, search);
}

static bool category_visible(const CategoryDef *category, const char *search) {
    if (!search[0]) {
        return true;
    }
    return contains_ci(category->name, search) || contains_ci(category->summary, search);
}

static int visible_category_index(int visible_pos, const char *search) {
    int pos = 0;
    for (int i = 0; i < category_count; i++) {
        if (!category_visible(&categories[i], search)) {
            continue;
        }
        if (pos == visible_pos) {
            return i;
        }
        pos++;
    }
    return -1;
}

static int visible_category_count(const char *search) {
    int count = 0;
    for (int i = 0; i < category_count; i++) {
        if (category_visible(&categories[i], search)) {
            count++;
        }
    }
    return count;
}

static const CategoryDef *selected_category(App *app) {
    if (app->category_open) {
        if (app->opened_category_index < 0 || app->opened_category_index >= category_count) {
            return NULL;
        }
        return &categories[app->opened_category_index];
    }
    int index = visible_category_index(app->category_selected, app->search);
    if (index < 0) {
        return NULL;
    }
    return &categories[index];
}

static bool action_in_current_category(App *app, const ActionDef *action) {
    const CategoryDef *category = selected_category(app);
    return category && strcmp(action->category, category->name) == 0;
}

static int visible_action_index(App *app, int visible_pos) {
    int pos = 0;
    for (int i = 0; i < action_count; i++) {
        if (!action_in_current_category(app, &actions[i]) || !action_visible(&actions[i], app->search)) {
            continue;
        }
        if (pos == visible_pos) {
            return i;
        }
        pos++;
    }
    return -1;
}

static int visible_action_count(App *app) {
    int count = 0;
    for (int i = 0; i < action_count; i++) {
        if (action_in_current_category(app, &actions[i]) && action_visible(&actions[i], app->search)) {
            count++;
        }
    }
    return count;
}

static void make_step(const ActionDef *action, ScenarioStep *step) {
    memset(step, 0, sizeof(*step));
    step->action = action;
    for (int i = 0; i < action->param_count; i++) {
        snprintf(step->params[i].value, sizeof(step->params[i].value), "%s", action->params[i].default_value);
    }
}

static const ActionDef *selected_action(App *app) {
    if (!app->category_open) {
        return NULL;
    }
    int index = visible_action_index(app, app->action_selected);
    if (index < 0) {
        return NULL;
    }
    return &actions[index];
}

static void open_config(App *app) {
    const ActionDef *action = selected_action(app);
    if (!action) {
        set_status(app, "no action selected");
        return;
    }
    make_step(action, &app->draft);
    clamp_step_params(app, &app->draft);
    app->draft_valid = true;
    app->param_selected = 0;
    app->view = VIEW_CONFIG;
    app->focus = 1;
    set_status(app, "configure %s", action->name);
}

static void memory_pressure_child(const ScenarioStep *step) {
    size_t size = (size_t)param_int(step, "size_mb") * 1024UL * 1024UL;
    int duration = param_int(step, "duration_sec");
    char *memory = malloc(size);
    if (!memory) {
        _exit(2);
    }
    if (param_bool(step, "touch_pages")) {
        for (size_t i = 0; i < size; i += 4096) {
            memory[i] = 1;
        }
    }
    sleep(duration > 0 ? duration : 1);
    free(memory);
    _exit(0);
}

static bool fill_random_block(char *block, size_t size) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return false;
    }
    size_t offset = 0;
    while (offset < size) {
        ssize_t n = read(fd, block + offset, size - offset);
        if (n <= 0) {
            close(fd);
            return false;
        }
        offset += (size_t)n;
    }
    close(fd);
    return true;
}

static void fill_pattern_block(char *block, size_t size, const char *pattern) {
    size_t pattern_len = strlen(pattern);
    if (pattern_len == 0) {
        memset(block, 0, size);
        return;
    }
    for (size_t i = 0; i < size; i++) {
        block[i] = pattern[i % pattern_len];
    }
}

static void large_file_child(const ScenarioStep *step) {
    const char *path = param_text(step, "path");
    const char *mode = param_text(step, "mode");
    int size_mb = param_int(step, "size_mb");
    if (strcmp(mode, "sparse") == 0) {
        int fd = open(path, O_CREAT | O_WRONLY, 0644);
        if (fd < 0) {
            _exit(2);
        }
        int rc = ftruncate(fd, (off_t)size_mb * 1024 * 1024);
        close(fd);
        _exit(rc == 0 ? 0 : 3);
    }
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        _exit(2);
    }
    char block[1024 * 1024];
    if (strcmp(mode, "pattern") == 0) {
        fill_pattern_block(block, sizeof(block), param_text(step, "pattern"));
    } else if (strcmp(mode, "random") != 0) {
        memset(block, 0, sizeof(block));
    }
    for (int i = 0; i < size_mb; i++) {
        if (strcmp(mode, "random") == 0 && !fill_random_block(block, sizeof(block))) {
            close(fd);
            _exit(4);
        }
        if (write(fd, block, sizeof(block)) != (ssize_t)sizeof(block)) {
            close(fd);
            _exit(3);
        }
    }
    close(fd);
    _exit(0);
}

static int bounded_positive(int value, int fallback, int max_value) {
    if (value <= 0) {
        value = fallback;
    }
    if (max_value > 0 && value > max_value) {
        value = max_value;
    }
    return value;
}

static int duration_seconds(const ScenarioStep *step, const char *key) {
    return bounded_positive(param_int(step, key), 1, 86400);
}

static bool ensure_dir(const char *path) {
    if (mkdir(path, 0755) == 0 || errno == EEXIST) {
        return true;
    }
    return false;
}

static bool write_repeated_byte(int fd, size_t bytes, char value) {
    char block[4096];
    memset(block, value, sizeof(block));
    while (bytes > 0) {
        size_t chunk = bytes < sizeof(block) ? bytes : sizeof(block);
        if (write(fd, block, chunk) != (ssize_t)chunk) {
            return false;
        }
        bytes -= chunk;
    }
    return true;
}

static int parse_errno_name(const char *name) {
    if (strcmp(name, "ENOENT") == 0) {
        return ENOENT;
    }
    if (strcmp(name, "EACCES") == 0) {
        return EACCES;
    }
    if (strcmp(name, "EISDIR") == 0) {
        return EISDIR;
    }
    if (strcmp(name, "EBUSY") == 0) {
        return EBUSY;
    }
    if (strcmp(name, "ENOSPC") == 0) {
        return ENOSPC;
    }
    if (strcmp(name, "EIO") == 0) {
        return EIO;
    }
    return atoi(name);
}

static int parse_signal_name(const char *name) {
    if (strcmp(name, "SIGTERM") == 0) {
        return SIGTERM;
    }
    if (strcmp(name, "SIGKILL") == 0) {
        return SIGKILL;
    }
    if (strcmp(name, "SIGSEGV") == 0) {
        return SIGSEGV;
    }
    return atoi(name);
}

static void memory_leak_child(const ScenarioStep *step) {
    int rate_mb = bounded_positive(param_int(step, "mb_per_sec"), 1, 4096);
    int max_mb = bounded_positive(param_int(step, "max_mb"), rate_mb, 1024 * 1024);
    int duration = duration_seconds(step, "duration_sec");
    int chunks = (max_mb + rate_mb - 1) / rate_mb;
    char **allocated = calloc((size_t)chunks, sizeof(char *));
    if (!allocated) {
        _exit(2);
    }
    int used = 0;
    for (int i = 0; i < chunks && used < max_mb && i < duration; i++) {
        int mb = rate_mb;
        if (used + mb > max_mb) {
            mb = max_mb - used;
        }
        size_t bytes = (size_t)mb * 1024UL * 1024UL;
        allocated[i] = malloc(bytes);
        if (!allocated[i]) {
            break;
        }
        for (size_t p = 0; p < bytes; p += 4096) {
            allocated[i][p] = 1;
        }
        used += mb;
        sleep(1);
    }
    for (int i = 0; i < chunks; i++) {
        free(allocated[i]);
    }
    free(allocated);
    _exit(0);
}

static void memory_fragmentation_child(const ScenarioStep *step) {
    int blocks = bounded_positive(param_int(step, "blocks"), 1, 200000);
    int iterations = bounded_positive(param_int(step, "iterations"), 1, 10000000);
    void **slots = calloc((size_t)blocks, sizeof(void *));
    if (!slots) {
        _exit(2);
    }
    for (int i = 0; i < iterations; i++) {
        int index = i % blocks;
        free(slots[index]);
        size_t size = (size_t)(((unsigned)i * 1103515245u + 12345u) % 8192u) + 16u;
        slots[index] = malloc(size);
        if (slots[index]) {
            memset(slots[index], i & 0xff, size);
        }
        if ((i % 3) == 0) {
            int other = (index * 17 + 7) % blocks;
            free(slots[other]);
            slots[other] = NULL;
        }
    }
    for (int i = 0; i < blocks; i++) {
        free(slots[i]);
    }
    free(slots);
    _exit(0);
}

static void oom_pressure_child(const ScenarioStep *step) {
    int target_mb = bounded_positive(param_int(step, "target_mb"), 1, 1024 * 1024);
    int duration = duration_seconds(step, "duration_sec");
    size_t bytes = (size_t)target_mb * 1024UL * 1024UL;
    char *memory = malloc(bytes);
    if (!memory) {
        _exit(2);
    }
    if (param_bool(step, "touch_pages")) {
        for (size_t i = 0; i < bytes; i += 4096) {
            memory[i] = 1;
        }
    }
    sleep(duration);
    free(memory);
    _exit(0);
}

static int current_usage_percent(const char *path) {
    struct statvfs fs;
    if (statvfs(path, &fs) != 0 || fs.f_blocks == 0) {
        return -1;
    }
    unsigned long used = fs.f_blocks - fs.f_bavail;
    return (int)((used * 100UL) / fs.f_blocks);
}

static void disk_fill_child(const ScenarioStep *step) {
    const char *dir = param_text(step, "target_dir");
    int target = bounded_positive(param_int(step, "target_percent"), 1, 100);
    if (!ensure_dir(dir)) {
        _exit(2);
    }
    char path[MAX_TEXT];
    char block[1024 * 1024];
    memset(block, 0, sizeof(block));
    for (int i = 0; i < 100000; i++) {
        int usage = current_usage_percent(dir);
        if (usage >= target && usage >= 0) {
            _exit(0);
        }
        snprintf(path, sizeof(path), "%s/testforge-fill-%05d.bin", dir, i);
        int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) {
            _exit(errno == ENOSPC ? 0 : 3);
        }
        if (write(fd, block, sizeof(block)) != (ssize_t)sizeof(block)) {
            int saved = errno;
            close(fd);
            _exit(saved == ENOSPC ? 0 : 4);
        }
        close(fd);
    }
    _exit(0);
}

static void inode_fill_child(const ScenarioStep *step) {
    const char *dir = param_text(step, "target_dir");
    int count = bounded_positive(param_int(step, "count"), 1, 1000000);
    int file_size = bounded_positive(param_int(step, "file_size"), 0, 1024 * 1024);
    if (!ensure_dir(dir)) {
        _exit(2);
    }
    char path[MAX_TEXT];
    for (int i = 0; i < count; i++) {
        snprintf(path, sizeof(path), "%s/inode-%06d", dir, i);
        int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) {
            _exit(errno == ENOSPC ? 0 : 3);
        }
        if (file_size > 0 && !write_repeated_byte(fd, (size_t)file_size, 'x')) {
            close(fd);
            _exit(errno == ENOSPC ? 0 : 4);
        }
        close(fd);
    }
    _exit(0);
}

static void random_write_child(const ScenarioStep *step) {
    const char *path = param_text(step, "path");
    int size_mb = bounded_positive(param_int(step, "size_mb"), 1, 1024 * 1024);
    off_t size = (off_t)size_mb * 1024 * 1024;
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0 || ftruncate(fd, size) != 0) {
        if (fd >= 0) {
            close(fd);
        }
        _exit(2);
    }
    char block[4096];
    if (!fill_random_block(block, sizeof(block))) {
        close(fd);
        _exit(3);
    }
    int writes = size_mb * 16;
    for (int i = 0; i < writes; i++) {
        off_t offset = (off_t)(((unsigned)i * 2654435761u) % (unsigned)(size / 4096)) * 4096;
        if (pwrite(fd, block, sizeof(block), offset) != (ssize_t)sizeof(block)) {
            close(fd);
            _exit(4);
        }
    }
    fsync(fd);
    close(fd);
    _exit(0);
}

static void fsync_storm_child(const ScenarioStep *step) {
    const char *base = param_text(step, "base");
    int files = bounded_positive(param_int(step, "files"), 1, 4096);
    int iterations = bounded_positive(param_int(step, "iterations"), 1, 10000000);
    if (!ensure_dir(base)) {
        _exit(2);
    }
    int *fds = calloc((size_t)files, sizeof(int));
    if (!fds) {
        _exit(2);
    }
    char path[MAX_TEXT];
    for (int i = 0; i < files; i++) {
        snprintf(path, sizeof(path), "%s/fsync-%04d", base, i);
        fds[i] = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fds[i] < 0) {
            _exit(3);
        }
    }
    char byte = 'f';
    for (int i = 0; i < iterations; i++) {
        int fd = fds[i % files];
        if (write(fd, &byte, 1) != 1 || fsync(fd) != 0) {
            _exit(4);
        }
    }
    for (int i = 0; i < files; i++) {
        close(fds[i]);
    }
    free(fds);
    _exit(0);
}

static void cpu_burn_child(const ScenarioStep *step) {
    int workers = bounded_positive(param_int(step, "workers"), 1, 1024);
    int duration = duration_seconds(step, "duration_sec");
    pid_t *pids = calloc((size_t)workers, sizeof(pid_t));
    if (!pids) {
        _exit(2);
    }
    for (int i = 0; i < workers; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            time_t end = time(NULL) + duration;
            volatile unsigned long long x = 0;
            while (time(NULL) < end) {
                for (int n = 0; n < 100000; n++) {
                    x += (unsigned long long)n;
                }
            }
            _exit(0);
        }
        if (pid > 0) {
            pids[i] = pid;
        }
    }
    for (int i = 0; i < workers; i++) {
        if (pids[i] > 0) {
            waitpid(pids[i], NULL, 0);
        }
    }
    free(pids);
    _exit(0);
}

static void process_flood_child(const ScenarioStep *step) {
    int processes = bounded_positive(param_int(step, "processes"), 1, 10000);
    int rate = bounded_positive(param_int(step, "rate_per_sec"), 1, 10000);
    int duration = duration_seconds(step, "duration_sec");
    pid_t *pids = calloc((size_t)processes, sizeof(pid_t));
    if (!pids) {
        _exit(2);
    }
    for (int i = 0; i < processes; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            sleep(duration);
            _exit(0);
        }
        if (pid > 0) {
            pids[i] = pid;
        }
        if ((i + 1) % rate == 0) {
            sleep(1);
        }
    }
    for (int i = 0; i < processes; i++) {
        if (pids[i] > 0) {
            waitpid(pids[i], NULL, 0);
        }
    }
    free(pids);
    _exit(0);
}

static void zombie_process_child(const ScenarioStep *step) {
    int count = bounded_positive(param_int(step, "count"), 1, 10000);
    int duration = duration_seconds(step, "duration_sec");
    for (int i = 0; i < count; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            _exit(0);
        }
        if (pid < 0) {
            break;
        }
    }
    sleep(duration);
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
    _exit(0);
}

static void orphan_process_child(const ScenarioStep *step) {
    int count = bounded_positive(param_int(step, "count"), 1, 10000);
    int duration = duration_seconds(step, "duration_sec");
    for (int i = 0; i < count; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            sleep(duration);
            _exit(0);
        }
    }
    _exit(0);
}

static void fd_exhaustion_child(const ScenarioStep *step) {
    int fds = bounded_positive(param_int(step, "fds"), 1, 1000000);
    int duration = duration_seconds(step, "duration_sec");
    int *opened = calloc((size_t)fds, sizeof(int));
    if (!opened) {
        _exit(2);
    }
    int count = 0;
    for (; count < fds; count++) {
        opened[count] = open("/dev/null", O_RDONLY);
        if (opened[count] < 0) {
            break;
        }
    }
    sleep(duration);
    for (int i = 0; i < count; i++) {
        close(opened[i]);
    }
    free(opened);
    _exit(count > 0 ? 0 : 3);
}

static void regular_file_child(const ScenarioStep *step) {
    int fd = open(param_text(step, "path"), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        _exit(2);
    }
    close(fd);
    _exit(0);
}

static void directory_child(const ScenarioStep *step) {
    _exit(ensure_dir(param_text(step, "path")) ? 0 : 2);
}

static void deep_directory_child(const ScenarioStep *step) {
    const char *base = param_text(step, "base");
    int depth = bounded_positive(param_int(step, "depth"), 1, 512);
    char path[MAX_TEXT];
    snprintf(path, sizeof(path), "%s", base);
    if (!ensure_dir(path)) {
        _exit(2);
    }
    for (int i = 0; i < depth; i++) {
        size_t used = strlen(path);
        if (used + 8 >= sizeof(path)) {
            _exit(0);
        }
        snprintf(path + used, sizeof(path) - used, "/d%04d", i);
        if (!ensure_dir(path)) {
            _exit(3);
        }
    }
    _exit(0);
}

static void huge_directory_child(const ScenarioStep *step) {
    const char *base = param_text(step, "base");
    int count = bounded_positive(param_int(step, "count"), 1, 1000000);
    if (!ensure_dir(base)) {
        _exit(2);
    }
    char path[MAX_TEXT];
    for (int i = 0; i < count; i++) {
        snprintf(path, sizeof(path), "%s/file-%06d", base, i);
        int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) {
            _exit(errno == ENOSPC ? 0 : 3);
        }
        close(fd);
    }
    _exit(0);
}

static void permission_pattern_child(const ScenarioStep *step) {
    const char *base = param_text(step, "base");
    if (!ensure_dir(base)) {
        _exit(2);
    }
    char path[MAX_TEXT];
    for (int mode = 0; mode <= 0777; mode++) {
        snprintf(path, sizeof(path), "%s/mode-%03o", base, mode);
        int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, mode);
        if (fd < 0) {
            _exit(3);
        }
        close(fd);
        chmod(path, (mode_t)mode);
    }
    _exit(0);
}

static void symlink_child(const ScenarioStep *step) {
    unlink(param_text(step, "link"));
    _exit(symlink(param_text(step, "target"), param_text(step, "link")) == 0 ? 0 : 2);
}

static void broken_symlink_child(const ScenarioStep *step) {
    const char *link_path = param_text(step, "link");
    unlink(link_path);
    _exit(symlink("/tmp/testforge-missing-target", link_path) == 0 ? 0 : 2);
}

static void symlink_loop_child(const ScenarioStep *step) {
    const char *base = param_text(step, "base");
    if (!ensure_dir(base)) {
        _exit(2);
    }
    char a[MAX_TEXT], b[MAX_TEXT], c[MAX_TEXT];
    snprintf(a, sizeof(a), "%s/a", base);
    snprintf(b, sizeof(b), "%s/b", base);
    snprintf(c, sizeof(c), "%s/c", base);
    unlink(a);
    unlink(b);
    unlink(c);
    if (symlink(b, a) != 0 || symlink(c, b) != 0 || symlink(a, c) != 0) {
        _exit(3);
    }
    _exit(0);
}

static void hard_link_child(const ScenarioStep *step) {
    const char *source = param_text(step, "source");
    int fd = open(source, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) {
        close(fd);
    }
    unlink(param_text(step, "link"));
    _exit(link(source, param_text(step, "link")) == 0 ? 0 : 2);
}

static void fifo_child(const ScenarioStep *step) {
    unlink(param_text(step, "path"));
    _exit(mkfifo(param_text(step, "path"), 0644) == 0 ? 0 : 2);
}

static void unix_socket_child(const ScenarioStep *step) {
    const char *path = param_text(step, "path");
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        _exit(2);
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    int rc = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    close(fd);
    _exit(rc == 0 ? 0 : 3);
}

static void device_node_child(const ScenarioStep *step, mode_t type) {
    const char *path = param_text(step, "path");
    int major_no = bounded_positive(param_int(step, "major"), 0, 4095);
    int minor_no = bounded_positive(param_int(step, "minor"), 0, 1048575);
    unlink(path);
    _exit(mknod(path, type | 0600, makedev((unsigned)major_no, (unsigned)minor_no)) == 0 ? 0 : 2);
}

static int tcp_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 128) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void tcp_server_loop(int fd, int duration, int mode, int delay_ms) {
    time_t end = time(NULL) + duration;
    int held[256];
    int held_count = 0;
    while (time(NULL) < end) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        struct timeval tv = {0, 200000};
        int rc = select(fd + 1, &set, NULL, NULL, &tv);
        if (rc <= 0) {
            continue;
        }
        int client = accept(fd, NULL, NULL);
        if (client < 0) {
            continue;
        }
        if (mode == 1) {
            usleep((useconds_t)delay_ms * 1000U);
            write(client, "OK\n", 3);
            close(client);
        } else if (mode == 2) {
            if (held_count < (int)(sizeof(held) / sizeof(held[0]))) {
                held[held_count++] = client;
            } else {
                close(client);
            }
        } else {
            close(client);
        }
    }
    for (int i = 0; i < held_count; i++) {
        close(held[i]);
    }
}

static void tcp_server_child(const ScenarioStep *step, int mode) {
    int port = bounded_positive(param_int(step, "port"), 1, 65535);
    int duration = duration_seconds(step, "duration_sec");
    int delay_ms = bounded_positive(param_int(step, "delay_ms"), 0, 600000);
    int fd = tcp_listen_socket(port);
    if (fd < 0) {
        _exit(2);
    }
    tcp_server_loop(fd, duration, mode, delay_ms);
    close(fd);
    _exit(0);
}

static void tcp_flood_child(const ScenarioStep *step) {
    const char *host = param_text(step, "host");
    int port = bounded_positive(param_int(step, "port"), 1, 65535);
    int connections = bounded_positive(param_int(step, "connections"), 1, 100000);
    int duration = duration_seconds(step, "duration_sec");
    int *fds = calloc((size_t)connections, sizeof(int));
    if (!fds) {
        _exit(2);
    }
    int opened = 0;
    for (int i = 0; i < connections; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            continue;
        }
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, host, &addr.sin_addr) != 1 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            close(fd);
            continue;
        }
        fds[opened++] = fd;
    }
    sleep(duration);
    for (int i = 0; i < opened; i++) {
        close(fds[i]);
    }
    free(fds);
    _exit(opened > 0 ? 0 : 3);
}

static void open_failure_child(const ScenarioStep *step) {
    int expected = parse_errno_name(param_text(step, "errno"));
    errno = 0;
    int fd = open(param_text(step, "path"), O_RDONLY);
    if (fd >= 0) {
        close(fd);
        _exit(3);
    }
    _exit(errno == expected ? 0 : 4);
}

static void read_failure_child(const ScenarioStep *step) {
    int expected = parse_errno_name(param_text(step, "errno"));
    errno = 0;
    int fd = open(param_text(step, "path"), O_RDONLY);
    if (fd < 0) {
        _exit(errno == expected ? 0 : 3);
    }
    char byte;
    ssize_t n = read(fd, &byte, 1);
    int saved = errno;
    close(fd);
    _exit(n < 0 && saved == expected ? 0 : 4);
}

static void device_busy_child(const ScenarioStep *step) {
    int fd = open(param_text(step, "path"), O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        _exit(2);
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        _exit(3);
    }
    sleep(duration_seconds(step, "duration_sec"));
    flock(fd, LOCK_UN);
    close(fd);
    _exit(0);
}

static void enospc_child(const ScenarioStep *step) {
    ScenarioStep copy = *step;
    static const ActionDef fill_action = {ACTION_DISK_FILL, "Disk", "Disk Fill", "", DANGER_HIGH, false,
                                          {{"target_dir", "Target Dir", PARAM_TEXT, "/tmp", NULL},
                                           {"target_percent", "Target %", PARAM_INT, "100", NULL}},
                                          2};
    copy.action = &fill_action;
    snprintf(copy.params[1].value, sizeof(copy.params[1].value), "100");
    disk_fill_child(&copy);
}

static void timeout_child(const ScenarioStep *step) {
    sleep(duration_seconds(step, "duration_sec"));
    _exit(0);
}

static void signal_injection_child(const ScenarioStep *step) {
    int pid = param_int(step, "pid");
    int sig = parse_signal_name(param_text(step, "signal"));
    _exit(kill((pid_t)pid, sig) == 0 ? 0 : 2);
}

static void file_tamper_child(const ScenarioStep *step) {
    sleep(duration_seconds(step, "delay_sec"));
    int fd = open(param_text(step, "path"), O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd < 0) {
        _exit(2);
    }
    const char *text = param_text(step, "text");
    bool ok = write(fd, text, strlen(text)) == (ssize_t)strlen(text) && write(fd, "\n", 1) == 1;
    close(fd);
    _exit(ok ? 0 : 3);
}

static void run_child_action(const ScenarioStep *step) {
    switch (step->action->id) {
    case ACTION_MEMORY_PRESSURE:
        memory_pressure_child(step);
        break;
    case ACTION_MEMORY_LEAK:
        memory_leak_child(step);
        break;
    case ACTION_MEMORY_FRAGMENTATION:
        memory_fragmentation_child(step);
        break;
    case ACTION_OOM_PRESSURE:
        oom_pressure_child(step);
        break;
    case ACTION_LARGE_FILE:
        large_file_child(step);
        break;
    case ACTION_DISK_FILL:
        disk_fill_child(step);
        break;
    case ACTION_INODE_FILL:
        inode_fill_child(step);
        break;
    case ACTION_RANDOM_WRITE:
        random_write_child(step);
        break;
    case ACTION_FSYNC_STORM:
        fsync_storm_child(step);
        break;
    case ACTION_CPU_BURN:
        cpu_burn_child(step);
        break;
    case ACTION_PROCESS_FLOOD:
        process_flood_child(step);
        break;
    case ACTION_ZOMBIE_PROCESS:
        zombie_process_child(step);
        break;
    case ACTION_ORPHAN_PROCESS:
        orphan_process_child(step);
        break;
    case ACTION_FD_EXHAUSTION:
        fd_exhaustion_child(step);
        break;
    case ACTION_REGULAR_FILE:
        regular_file_child(step);
        break;
    case ACTION_DIRECTORY:
        directory_child(step);
        break;
    case ACTION_DEEP_DIRECTORY:
        deep_directory_child(step);
        break;
    case ACTION_HUGE_DIRECTORY:
        huge_directory_child(step);
        break;
    case ACTION_PERMISSION_PATTERN:
        permission_pattern_child(step);
        break;
    case ACTION_SYMLINK:
        symlink_child(step);
        break;
    case ACTION_BROKEN_SYMLINK:
        broken_symlink_child(step);
        break;
    case ACTION_SYMLINK_LOOP:
        symlink_loop_child(step);
        break;
    case ACTION_HARD_LINK:
        hard_link_child(step);
        break;
    case ACTION_FIFO:
        fifo_child(step);
        break;
    case ACTION_UNIX_SOCKET:
        unix_socket_child(step);
        break;
    case ACTION_CHAR_DEVICE:
        device_node_child(step, S_IFCHR);
        break;
    case ACTION_BLOCK_DEVICE:
        device_node_child(step, S_IFBLK);
        break;
    case ACTION_TCP_SERVER:
        tcp_server_child(step, 0);
        break;
    case ACTION_TCP_FLOOD:
        tcp_flood_child(step);
        break;
    case ACTION_SLOW_SERVER:
        tcp_server_child(step, 1);
        break;
    case ACTION_NO_RESPONSE_SERVER:
        tcp_server_child(step, 2);
        break;
    case ACTION_IMMEDIATE_DISCONNECT:
        tcp_server_child(step, 0);
        break;
    case ACTION_OPEN_FAILURE:
        open_failure_child(step);
        break;
    case ACTION_READ_FAILURE:
        read_failure_child(step);
        break;
    case ACTION_DEVICE_BUSY:
        device_busy_child(step);
        break;
    case ACTION_ENOSPC:
        enospc_child(step);
        break;
    case ACTION_TIMEOUT:
        timeout_child(step);
        break;
    case ACTION_SIGNAL_INJECTION:
        signal_injection_child(step);
        break;
    case ACTION_FILE_TAMPER:
        file_tamper_child(step);
        break;
    default:
        _exit(10);
    }
}

static void add_job(App *app, pid_t pid, const ActionDef *action, JobState state, const char *detail) {
    if (app->jobs.len == MAX_JOBS) {
        return;
    }
    Job *job = &app->jobs.items[app->jobs.len++];
    job->pid = pid;
    job->action = action;
    job->started = time(NULL);
    job->state = state;
    job->exit_code = 0;
    snprintf(job->detail, sizeof(job->detail), "%s", detail);
}

static void run_step(App *app, ScenarioStep *step) {
    if (step->action->danger >= DANGER_HIGH && !app->dry_run) {
        log_event(app, "running high-danger action: %s", step->action->name);
    }
    if (app->dry_run) {
        add_job(app, 0, step->action, JOB_DRY_RUN, "dry run only");
        log_event(app, "dry run: %s", step->action->name);
        set_status(app, "dry run: %s", step->action->name);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        set_status(app, "fork failed: %s", strerror(errno));
        return;
    }
    if (pid == 0) {
        run_child_action(step);
    }
    add_job(app, pid, step->action, JOB_RUNNING, "running");
    log_event(app, "started %s pid=%d", step->action->name, pid);
    set_status(app, "started %s pid=%d", step->action->name, pid);
}

static void refresh_jobs(App *app) {
    for (int i = 0; i < app->jobs.len; i++) {
        Job *job = &app->jobs.items[i];
        if (job->state != JOB_RUNNING || job->pid <= 0) {
            continue;
        }
        int status = 0;
        pid_t rc = waitpid(job->pid, &status, WNOHANG);
        if (rc == 0) {
            continue;
        }
        if (rc < 0) {
            job->state = JOB_FAILED;
            snprintf(job->detail, sizeof(job->detail), "wait failed: %s", strerror(errno));
            continue;
        }
        job->state = WIFEXITED(status) && WEXITSTATUS(status) == 0 ? JOB_EXITED : JOB_FAILED;
        job->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        snprintf(job->detail, sizeof(job->detail), "exit=%d", job->exit_code);
        log_event(app, "finished %s pid=%d %s", job->action->name, job->pid, job->detail);
    }
}

static void draw_box(int y, int x, int h, int w, const char *title, bool focus) {
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
    if (focus) {
        attron(COLOR_PAIR(COLOR_LABEL) | A_BOLD);
    }
    mvaddnstr(y, x + 1, title, w - 2);
    if (focus) {
        attroff(COLOR_PAIR(COLOR_LABEL) | A_BOLD);
    }
}

static void draw_header(App *app, int width) {
    attron(COLOR_PAIR(COLOR_LABEL) | A_BOLD);
    mvaddstr(0, 0, "TestForge");
    attroff(COLOR_PAIR(COLOR_LABEL) | A_BOLD);
    const char *crumb = "Categories";
    if (app->view == VIEW_ACTIONS) {
        const CategoryDef *category = selected_category(app);
        crumb = category ? category->name : "Actions";
    } else if (app->view == VIEW_CONFIG && app->draft_valid) {
        crumb = app->draft.action->name;
    }
    mvprintw(0, 11, "> %s", crumb);
    mvaddstr(0, width / 2, "Search:");
    attron(COLOR_PAIR(COLOR_ACCENT));
    int input_x = width / 2 + 8;
    int sw = width - input_x - 20;
    if (sw > 32) {
        sw = 32;
    }
    if (sw > 0) {
        mvhline(0, input_x, ' ', sw);
        mvaddnstr(0, input_x, app->search, sw);
    }
    attroff(COLOR_PAIR(COLOR_ACCENT));
    attron(app->dry_run ? COLOR_PAIR(COLOR_OK) : COLOR_PAIR(COLOR_WARN));
    mvprintw(0, width - 18, "DryRun:%s", app->dry_run ? "ON " : "OFF");
    attroff(app->dry_run ? COLOR_PAIR(COLOR_OK) : COLOR_PAIR(COLOR_WARN));
}

static void draw_actions(App *app, int y, int x, int h, int w) {
    const CategoryDef *category = selected_category(app);
    char title[96];
    snprintf(title, sizeof(title), "%s", app->category_open && category ? category->name : "categories");
    draw_box(y, x, h, w, title, app->focus == 0);
    int body = h - 2;
    if (!app->category_open) {
        int visible = visible_category_count(app->search);
        if (app->category_selected >= visible) {
            app->category_selected = visible > 0 ? visible - 1 : 0;
        }
        if (app->category_selected < app->category_scroll) {
            app->category_scroll = app->category_selected;
        }
        if (app->category_selected >= app->category_scroll + body) {
            app->category_scroll = app->category_selected - body + 1;
        }
        for (int row = 0; row < body; row++) {
            int visible_pos = app->category_scroll + row;
            int index = visible_category_index(visible_pos, app->search);
            if (index < 0) {
                break;
            }
            int attr = visible_pos == app->category_selected ? COLOR_PAIR(COLOR_SELECT) : A_NORMAL;
            attron(attr);
            mvprintw(y + 1 + row, x + 1, "%-*.*s", w - 2, w - 2, categories[index].name);
            attroff(attr);
        }
    } else {
        int visible = visible_action_count(app);
        if (app->action_selected >= visible) {
            app->action_selected = visible > 0 ? visible - 1 : 0;
        }
        if (app->action_selected < app->action_scroll) {
            app->action_scroll = app->action_selected;
        }
        if (app->action_selected >= app->action_scroll + body) {
            app->action_scroll = app->action_selected - body + 1;
        }
        for (int row = 0; row < body; row++) {
            int visible_pos = app->action_scroll + row;
            int index = visible_action_index(app, visible_pos);
            if (index < 0) {
                break;
            }
            const ActionDef *action = &actions[index];
            int attr = visible_pos == app->action_selected ? COLOR_PAIR(COLOR_SELECT) : A_NORMAL;
            attron(attr);
            mvprintw(y + 1 + row, x + 1, "%-*.*s", w - 2, w - 2, action->name);
            attroff(attr);
        }
    }
}

static void draw_details(App *app, int y, int x, int h, int w) {
    draw_box(y, x, h, w, "details", app->focus == 1);
    if (!app->category_open) {
        const CategoryDef *category = selected_category(app);
        if (!category) {
            mvaddstr(y + 1, x + 1, "No category");
            return;
        }
        attron(COLOR_PAIR(COLOR_LABEL) | A_BOLD);
        mvprintw(y + 1, x + 1, "%s", category->name);
        attroff(COLOR_PAIR(COLOR_LABEL) | A_BOLD);
        mvaddnstr(y + 2, x + 1, category->summary, w - 2);
        mvprintw(y + 4, x + 1, "Press Enter to open %s actions.", category->name);
        return;
    }
    ScenarioStep preview;
    ScenarioStep *step = NULL;
    const ActionDef *action = NULL;
    if (app->view == VIEW_CONFIG && app->draft_valid) {
        step = &app->draft;
        action = step->action;
    } else {
        action = selected_action(app);
        if (action) {
            make_step(action, &preview);
            step = &preview;
        }
    }
    if (!action) {
        mvaddstr(y + 1, x + 1, "No action");
        return;
    }
    attron(COLOR_PAIR(COLOR_LABEL) | A_BOLD);
    mvprintw(y + 1, x + 1, "%s", action->name);
    attroff(COLOR_PAIR(COLOR_LABEL) | A_BOLD);
    mvaddnstr(y + 2, x + 1, action->summary, w - 2);
    if (action->id == ACTION_MEMORY_PRESSURE || action->id == ACTION_MEMORY_LEAK || action->id == ACTION_OOM_PRESSURE) {
        mvprintw(y + 3, x + 1, "Available memory: %ld MB", app->mem_available_mb);
    } else if (action->root_required) {
        mvaddstr(y + 3, x + 1, "This action may require root privileges.");
    } else {
        mvaddstr(y + 3, x + 1, "Configure values, then run.");
    }

    for (int i = 0; i < action->param_count && y + 5 + i < y + h - 1; i++) {
        int attr = app->focus == 1 && i == app->param_selected ? COLOR_PAIR(COLOR_SELECT) : A_NORMAL;
        attron(attr);
        int limit = param_limit_mb(app, step, i);
        if (limit > 0) {
            mvprintw(y + 5 + i, x + 1, "%-16.16s %s  (max %d MB)", action->params[i].label, step->params[i].value,
                     limit);
        } else if (action->params[i].type == PARAM_BOOL || action->params[i].type == PARAM_CHOICE) {
            mvprintw(y + 5 + i, x + 1, "%-16.16s %s  (Left/Right)", action->params[i].label, step->params[i].value);
        } else {
            mvprintw(y + 5 + i, x + 1, "%-16.16s %s", action->params[i].label, step->params[i].value);
        }
        attroff(attr);
    }
    if (app->view == VIEW_CONFIG && step && app->param_selected < action->param_count && h > 10) {
        int help_y = y + h - 4;
        mvhline(help_y - 1, x + 1, ACS_HLINE, w - 2);
        attron(COLOR_PAIR(COLOR_LABEL) | A_BOLD);
        mvaddstr(help_y, x + 1, "Selected value");
        attroff(COLOR_PAIR(COLOR_LABEL) | A_BOLD);
        mvaddnstr(help_y + 1, x + 1, param_help(step, app->param_selected), w - 2);
    }
}

static void draw_footer(App *app, int y, int width) {
    if (app->view == VIEW_CATEGORIES) {
        attron(A_BOLD);
        mvaddstr(y, 0, "Enter:");
        attroff(A_BOLD);
        addstr(" open category  /:search  q:quit  ");
    } else if (app->view == VIEW_ACTIONS) {
        attron(A_BOLD);
        mvaddstr(y, 0, "Enter:");
        attroff(A_BOLD);
        addstr(" configure action  Esc:categories  /:search  q:quit  ");
    } else {
        attron(A_BOLD);
        mvaddstr(y, 0, "Enter:");
        attroff(A_BOLD);
        addstr(" edit value  r:run  d:dry-run  Esc:actions  q:quit  ");
    }
    addnstr(app->status, width - getcurx(stdscr) - 1);
}

static void draw_app(App *app) {
    int height, width;
    getmaxyx(stdscr, height, width);
    erase();
    if (height < 24 || width < 100) {
        mvaddstr(0, 0, "Terminal too small for TestForge");
        refresh();
        return;
    }
    draw_header(app, width);
    int top = 2;
    int footer = height - 1;
    int left_w = width / 3;
    if (left_w < 28) {
        left_w = 28;
    }
    int right_w = width - left_w - 1;
    draw_actions(app, top, 0, footer - top, left_w);
    draw_details(app, top, left_w + 1, footer - top, right_w);
    draw_footer(app, footer, width);
    refresh();
}

static void edit_search(App *app) {
    int width = getmaxx(stdscr);
    nodelay(stdscr, FALSE);
    echo();
    curs_set(1);
    draw_header(app, width);
    move(0, width / 2 + 8);
    getnstr(app->search, (int)sizeof(app->search) - 1);
    noecho();
    curs_set(0);
    nodelay(stdscr, TRUE);
    if (app->category_open) {
        app->action_selected = 0;
        app->action_scroll = 0;
    } else {
        app->category_selected = 0;
        app->category_scroll = 0;
    }
    set_status(app, "search: %s", app->search[0] ? app->search : "-");
}

static void get_digits_input(int y, int x, char *buffer, size_t size) {
    size_t len = strlen(buffer);
    int ch;
    while (true) {
        move(y, x);
        clrtoeol();
        attron(COLOR_PAIR(COLOR_ACCENT));
        mvaddnstr(y, x, buffer, (int)size - 1);
        attroff(COLOR_PAIR(COLOR_ACCENT));
        move(y, x + (int)len);
        refresh();
        ch = getch();
        if (ch == '\n' || ch == '\r') {
            return;
        }
        if (ch == 27) {
            buffer[0] = '\0';
            return;
        }
        if ((ch == KEY_BACKSPACE || ch == 127 || ch == '\b') && len > 0) {
            buffer[--len] = '\0';
            continue;
        }
        if (ch >= '0' && ch <= '9' && len + 1 < size) {
            buffer[len++] = (char)ch;
            buffer[len] = '\0';
        }
    }
}

static void edit_selected_param(App *app) {
    if (!app->draft_valid) {
        set_status(app, "open an action configuration first");
        return;
    }
    ScenarioStep *step = &app->draft;
    if (app->param_selected >= step->action->param_count) {
        app->param_selected = 0;
    }
    ParamDef def = step->action->params[app->param_selected];
    int height, width;
    getmaxyx(stdscr, height, width);
    nodelay(stdscr, FALSE);
    echo();
    curs_set(1);
    attron(COLOR_PAIR(COLOR_ACCENT));
    mvhline(height - 1, 0, ' ', width);
    mvprintw(height - 1, 0, "%s: ", def.label);
    attroff(COLOR_PAIR(COLOR_ACCENT));
    char input[MAX_TEXT];
    snprintf(input, sizeof(input), "%s", step->params[app->param_selected].value);
    move(height - 1, (int)strlen(def.label) + 2);
    if (def.type == PARAM_INT) {
        noecho();
        get_digits_input(height - 1, (int)strlen(def.label) + 2, input, sizeof(input));
        echo();
        if (input[0] == '\0') {
            noecho();
            curs_set(0);
            nodelay(stdscr, TRUE);
            set_status(app, "cancelled");
            return;
        }
    } else {
        getnstr(input, sizeof(input) - 1);
    }
    snprintf(step->params[app->param_selected].value, sizeof(step->params[app->param_selected].value), "%s", input);
    clamp_step_params(app, step);
    noecho();
    curs_set(0);
    nodelay(stdscr, TRUE);
    set_status(app, "updated %s", def.key);
}

static void cycle_selected_param(App *app, int direction) {
    if (app->view != VIEW_CONFIG || !app->draft_valid) {
        return;
    }
    ScenarioStep *step = &app->draft;
    if (app->param_selected >= step->action->param_count) {
        return;
    }
    ParamDef def = step->action->params[app->param_selected];
    char *value = step->params[app->param_selected].value;
    if (def.type == PARAM_BOOL) {
        snprintf(value, MAX_TEXT, "%s", param_bool(step, def.key) ? "false" : "true");
        set_status(app, "updated %s", def.key);
        return;
    }
    if (def.type == PARAM_CHOICE && def.choices) {
        char choices[MAX_TEXT];
        snprintf(choices, sizeof(choices), "%s", def.choices);
        const char *items[32];
        int count = 0;
        char *save = NULL;
        for (char *token = strtok_r(choices, "|", &save); token && count < 32; token = strtok_r(NULL, "|", &save)) {
            items[count++] = token;
        }
        if (count == 0) {
            return;
        }
        int current = 0;
        for (int i = 0; i < count; i++) {
            if (strcmp(items[i], value) == 0) {
                current = i;
                break;
            }
        }
        current = (current + direction + count) % count;
        snprintf(value, MAX_TEXT, "%s", items[current]);
        set_status(app, "updated %s", def.key);
    }
}

static void open_selected_category(App *app) {
    int index = visible_category_index(app->category_selected, app->search);
    if (index < 0) {
        set_status(app, "no category selected");
        return;
    }
    const CategoryDef *category = &categories[index];
    app->category_open = true;
    app->view = VIEW_ACTIONS;
    app->opened_category_index = index;
    app->action_selected = 0;
    app->action_scroll = 0;
    app->param_selected = 0;
    set_status(app, "opened %s", category->name);
}

static void close_category(App *app) {
    if (!app->category_open) {
        return;
    }
    app->category_open = false;
    app->view = VIEW_CATEGORIES;
    app->opened_category_index = -1;
    app->action_selected = 0;
    app->action_scroll = 0;
    app->param_selected = 0;
    set_status(app, "back to categories");
}

static void close_config(App *app) {
    if (app->view != VIEW_CONFIG) {
        return;
    }
    app->view = VIEW_ACTIONS;
    app->focus = 0;
    app->draft_valid = false;
    app->param_selected = 0;
    set_status(app, "back to actions");
}

static void handle_key(App *app, int ch) {
    if (ch == ERR) {
        return;
    }
    int visible_categories = visible_category_count(app->search);
    int visible_actions = app->category_open ? visible_action_count(app) : 0;
    switch (ch) {
    case 'q':
    case 'Q':
        endwin();
        exit(0);
    case '\t':
        set_status(app, "only the details panel is editable");
        break;
    case KEY_BTAB:
        set_status(app, "only the details panel is editable");
        break;
    case '/':
        edit_search(app);
        break;
    case 27:
    case KEY_BACKSPACE:
    case 127:
        if (app->view == VIEW_CONFIG) {
            close_config(app);
        } else if (app->category_open) {
            close_category(app);
        } else if (app->search[0]) {
            app->search[0] = '\0';
            app->category_selected = 0;
            app->category_scroll = 0;
            set_status(app, "search cleared");
        }
        break;
    case KEY_DOWN:
    case 'j':
        if (app->view == VIEW_CATEGORIES && app->category_selected + 1 < visible_categories) {
            app->category_selected++;
        } else if (app->view == VIEW_ACTIONS && app->action_selected + 1 < visible_actions) {
            app->action_selected++;
        } else if (app->view == VIEW_CONFIG && app->focus == 1 && app->draft_valid &&
                   app->param_selected + 1 < app->draft.action->param_count) {
            app->param_selected++;
        }
        break;
    case KEY_UP:
    case 'k':
        if (app->view == VIEW_CATEGORIES && app->category_selected > 0) {
            app->category_selected--;
        } else if (app->view == VIEW_ACTIONS && app->action_selected > 0) {
            app->action_selected--;
        } else if (app->view == VIEW_CONFIG && app->focus == 1 && app->param_selected > 0) {
            app->param_selected--;
        }
        break;
    case KEY_LEFT:
    case 'h':
    case 'H':
        cycle_selected_param(app, -1);
        break;
    case KEY_RIGHT:
    case 'l':
    case 'L':
        cycle_selected_param(app, 1);
        break;
    case 'a':
    case 'A':
        if (app->view == VIEW_CATEGORIES) {
            open_selected_category(app);
        } else {
            open_config(app);
        }
        break;
    case '\n':
    case '\r':
        if (app->view == VIEW_CATEGORIES) {
            open_selected_category(app);
        } else if (app->view == VIEW_ACTIONS) {
            open_config(app);
        } else if (app->view == VIEW_CONFIG && app->focus == 1) {
            edit_selected_param(app);
        }
        break;
    case 'd':
    case 'D':
        app->dry_run = !app->dry_run;
        set_status(app, "dry run %s", app->dry_run ? "enabled" : "disabled");
        break;
    case 'r':
    case 'R':
        if (app->view == VIEW_CONFIG && app->draft_valid) {
            clamp_step_params(app, &app->draft);
            run_step(app, &app->draft);
        } else {
            set_status(app, "configure an action before running");
        }
        break;
    default:
        break;
    }
}

static void app_init(App *app) {
    memset(app, 0, sizeof(*app));
    app->dry_run = true;
    app->view = VIEW_CATEGORIES;
    app->opened_category_index = -1;
    app->mem_available_mb = read_mem_available_mb();
    snprintf(app->status, sizeof(app->status), "ready");
    log_event(app, "TestForge started");
}

int main(void) {
    App app;
    app_init(&app);
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(COLOR_LABEL, COLOR_YELLOW, -1);
        init_pair(COLOR_SELECT, COLOR_BLACK, COLOR_WHITE);
        init_pair(COLOR_ACCENT, COLOR_WHITE, COLOR_BLUE);
        init_pair(COLOR_WARN, COLOR_RED, -1);
        init_pair(COLOR_OK, COLOR_GREEN, -1);
    }
    while (true) {
        app.mem_available_mb = read_mem_available_mb();
        refresh_jobs(&app);
        draw_app(&app);
        handle_key(&app, getch());
        napms(50);
    }
}
