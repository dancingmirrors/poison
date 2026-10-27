#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "poison-test.h"

#define MAX_TESTS 512
#define EXIT_SKIP 77
#define DEFAULT_TIMEOUT 30
#define EXIT_RESULT 100

static const char base_rc[] =
    "idle_timeout \"0\"\n"
    "hot_corner \"0\"\n"
    "notifications \"0\"\n"
    "tray \"0\"\n"
    "decorations \"1\"\n"
    "decoration_border_width \"2\"\n"
    "decoration_title_height \"20\"\n"
    "decoration_active_color \"#00ff00\"\n"
    "decoration_active_text_color \"#00ff00\"\n"
    "decoration_inactive_color \"#0000ff\"\n"
    "decoration_inactive_text_color \"#0000ff\"\n"
    "decoration_active_border_color \"#ffff00\"\n"
    "decoration_inactive_border_color \"#ff00ff\"\n"
    "exec printf '%s\\n%s\\n' \"$WAYLAND_DISPLAY\" \"$DISPLAY\" > "
    "\"$HOME/.displays.tmp\" && mv \"$HOME/.displays.tmp\" \"$HOME/displays\"\n";

static const struct test_case *tests[MAX_TESTS];
static int test_count;

void test_register(const struct test_case *test) {
    if (test_count < MAX_TESTS) {
        tests[test_count++] = test;
    }
}

static const char *current_dir;
static char current_home[PATH_MAX];
static pid_t current_compositor;
static bool current_failed;

const char *test_dir(void) {
    return current_dir;
}

const char *test_home(void) {
    return current_home;
}

pid_t test_compositor_pid(void) {
    return current_compositor;
}

bool test_compositor_alive(void) {
    if (current_compositor <= 0 || kill(current_compositor, 0) != 0) {
        return false;
    }
    char path[64], stat[512];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)current_compositor);
    FILE *file = fopen(path, "r");
    if (!file) {
        return true;
    }
    size_t len = fread(stat, 1, sizeof(stat) - 1, file);
    fclose(file);
    stat[len] = '\0';
    const char *paren = strrchr(stat, ')');
    return !paren || paren[1] != ' ' || (paren[2] != 'Z' && paren[2] != 'X');
}

char *test_compositor_log(void) {
    char path[PATH_MAX];
    if ((size_t)snprintf(path, sizeof(path), "%s/poison.log", current_dir) >=
        sizeof(path)) {
        return strdup("");
    }
    char *text = read_file(path);
    return text ? text : strdup("");
}

int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void sleep_ms(int ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

void test_fail_at(const char *file, int line, const char *fmt, ...) {
    current_failed = true;
    const char *base = strrchr(file, '/');
    fprintf(stderr, "%s:%d: ", base ? base + 1 : file, line);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    fflush(stderr);
}

void test_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    fflush(stderr);
}

void test_skip(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    fflush(stderr);
    exit(EXIT_SKIP);
}

void test_abort(void) {
    exit(1);
}

bool test_failed(void) {
    return current_failed;
}

struct options {
    const char *compositor;
    const char *workdir;
    int jobs;
    bool keep;
    bool verbose;
    bool list;
    bool stress;
    char **patterns;
    int pattern_count;
};

static struct options opts;

enum result {
    RESULT_PASS,
    RESULT_FAIL,
    RESULT_SKIP,
    RESULT_XFAIL,
    RESULT_XPASS,
    RESULT_FLAKY,
    RESULT_COUNT,
};

static const char *result_names[] = {
    [RESULT_PASS] = "PASS",
    [RESULT_FAIL] = "FAIL",
    [RESULT_SKIP] = "SKIP",
    [RESULT_XFAIL] = "XFAIL",
    [RESULT_XPASS] = "XPASS",
    [RESULT_FLAKY] = "FLAKY",
};

static volatile sig_atomic_t interrupted;

static void on_interrupt(int sig) {
    interrupted = sig;
}

static void catch_interrupts(void) {
    struct sigaction action = {0};
    action.sa_handler = on_interrupt;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
}

static void default_signals(void) {
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
}

static void die(const char *fmt, ...)
    __attribute__((format(printf, 1, 2), noreturn));

static void die(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "poison-test: ");
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    exit(2);
}

static void path_join(char *out, size_t size, const char *dir,
                      const char *name) {
    if ((size_t)snprintf(out, size, "%s/%s", dir, name) >= size) {
        die("path too long: %s/%s", dir, name);
    }
}

void test_known_issue(const char *fmt, ...) {
    char path[PATH_MAX];
    path_join(path, sizeof(path), test_dir(), "known-issue.txt");
    FILE *file = fopen(path, "w");
    if (!file) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(file, fmt, args);
    va_end(args);
    fclose(file);
}

static void write_file(const char *path, const char *text) {
    FILE *file = fopen(path, "w");
    if (!file) {
        die("cannot write %s: %s", path, strerror(errno));
    }
    fputs(text, file);
    fclose(file);
}

char *read_file(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) {
        return NULL;
    }
    char *text = NULL;
    size_t size = 0;
    FILE *out = open_memstream(&text, &size);
    if (!out) {
        fclose(file);
        return NULL;
    }
    char buf[4096];
    size_t n;
    do {
        n = fread(buf, 1, sizeof(buf), file);
        fwrite(buf, 1, n, out);
    } while (n == sizeof(buf));
    fclose(out);
    fclose(file);
    return text;
}

static void remove_tree(const char *path) {
    DIR *dir = opendir(path);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            char child[PATH_MAX];
            snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
            struct stat st;
            if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
                remove_tree(child);
            } else {
                unlink(child);
            }
        }
        closedir(dir);
    }
    rmdir(path);
}

static bool find_in_path(const char *name) {
    if (strchr(name, '/')) {
        return access(name, X_OK) == 0;
    }
    const char *path = getenv("PATH");
    if (!path) {
        path = "/usr/local/bin:/usr/bin:/bin";
    }
    char *copy = strdup(path);
    bool found = false;
    for (char *save = NULL, *dir = strtok_r(copy, ":", &save); dir;
         dir = strtok_r(NULL, ":", &save)) {
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", dir, name);
        if (access(full, X_OK) == 0) {
            found = true;
            break;
        }
    }
    free(copy);
    return found;
}

bool have_program(const char *name) {
    return find_in_path(name);
}

static bool xwayland_available(void) {
    const char *override = getenv("WLR_XWAYLAND");
    if (override && *override) {
        return access(override, X_OK) == 0;
    }
    return find_in_path("Xwayland");
}

static int stop_compositor(pid_t pid, bool *crashed, int *signal_out) {
    int status = 0;
    pid_t done = waitpid(pid, &status, WNOHANG);
    if (done == pid) {
        *crashed = true;
        *signal_out = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
        kill(-pid, SIGKILL);
        return status;
    }

    *crashed = false;
    kill(-pid, SIGTERM);
    int64_t deadline = now_ms() + 3000;
    while (now_ms() < deadline) {
        done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            break;
        }
        sleep_ms(10);
    }
    if (done != pid) {
        kill(-pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
    kill(-pid, SIGKILL);
    return status;
}

static void set_env_pairs(const char *pairs) {
    if (!pairs) {
        return;
    }
    char *copy = strdup(pairs);
    for (char *save = NULL, *pair = strtok_r(copy, " ", &save); pair;
         pair = strtok_r(NULL, " ", &save)) {
        char *eq = strchr(pair, '=');
        if (!eq) {
            unsetenv(pair);
            continue;
        }
        *eq = '\0';
        setenv(pair, eq + 1, 1);
    }
    free(copy);
}

static pid_t start_compositor(const struct test_case *test, const char *dir,
                              const char *home, const char *runtime) {
    char log_path[PATH_MAX];
    path_join(log_path, sizeof(log_path), dir, "poison.log");

    pid_t pid = fork();
    if (pid < 0) {
        die("fork failed: %s", strerror(errno));
    }
    if (pid > 0) {
        setpgid(pid, pid);
        return pid;
    }

    setpgid(0, 0);
    default_signals();
    prctl(PR_SET_PDEATHSIG, SIGKILL);

    int fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
    }
    int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
    }

    unsetenv("WAYLAND_DISPLAY");
    unsetenv("WAYLAND_SOCKET");
    unsetenv("DISPLAY");
    unsetenv("LD_PRELOAD");
    setenv("HOME", home, 1);
    setenv("XDG_RUNTIME_DIR", runtime, 1);
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "pixman", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    setenv("WLR_LIBINPUT_NO_DEVICES", "1", 1);
    setenv("ASAN_OPTIONS", "detect_leaks=0:abort_on_error=1:symbolize=1", 0);
    setenv("UBSAN_OPTIONS", "print_stacktrace=1", 0);

    char built[PATH_MAX];
    if (realpath(opts.compositor, built)) {
        char *slash = strrchr(built, '/');
        if (slash) {
            *slash = '\0';
            const char *path = getenv("PATH");
            char *joined = NULL;
            if (asprintf(&joined, "%s:%s", built,
                         path ? path : "/usr/local/bin:/usr/bin:/bin") >= 0) {
                setenv("PATH", joined, 1);
                free(joined);
            }
        }
    }
    set_env_pairs(test->opts.env);

    execl(opts.compositor, opts.compositor, (char *)NULL);
    fprintf(stderr, "poison-test: cannot run %s: %s\n", opts.compositor,
            strerror(errno));
    _exit(127);
}

static bool wait_for_displays(pid_t compositor, const char *home,
                              char *wayland, size_t wayland_size, char *x11,
                              size_t x11_size) {
    char path[PATH_MAX];
    path_join(path, sizeof(path), home, "displays");

    int64_t deadline = now_ms() + 15000;
    while (now_ms() < deadline) {
        char *text = read_file(path);
        if (text) {
            char *newline = strchr(text, '\n');
            if (newline) {
                *newline = '\0';
                snprintf(wayland, wayland_size, "%s", text);
                char *second = newline + 1;
                second[strcspn(second, "\n")] = '\0';
                snprintf(x11, x11_size, "%s", second);
                free(text);
                return wayland[0] != '\0';
            }
            free(text);
        }
        if (waitpid(compositor, NULL, WNOHANG) == compositor || interrupted) {
            return false;
        }
        sleep_ms(10);
    }
    return false;
}

static const char *log_problem(const char *log) {
    static const char *needles[] = {
        "ERROR: AddressSanitizer",
        "ERROR: LeakSanitizer",
        "runtime error:",
        NULL,
    };
    for (int i = 0; needles[i]; i++) {
        const char *hit = strstr(log, needles[i]);
        if (hit) {
            return hit;
        }
    }
    return NULL;
}

static void append_note(const char *dir, const char *fmt, ...) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/result.txt", dir);
    FILE *file = fopen(path, "a");
    if (!file) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(file, fmt, args);
    va_end(args);
    fputc('\n', file);
    fclose(file);
}

static int supervise_once(const struct test_case *test, const char *dir,
                          bool *plain_failure) {
    *plain_failure = false;
    char home[PATH_MAX];
    path_join(home, sizeof(home), dir, "home");
    mkdir(dir, 0755);
    mkdir(home, 0755);

    char runtime[] = "/tmp/poison-test-XXXXXX";
    if (!mkdtemp(runtime)) {
        die("cannot create a runtime directory: %s", strerror(errno));
    }

    char rc_path[PATH_MAX];
    path_join(rc_path, sizeof(rc_path), home, ".poison.rc");
    size_t rc_size = sizeof(base_rc) + (test->opts.rc ? strlen(test->opts.rc) : 0) + 2;
    char *rc = calloc(1, rc_size);
    snprintf(rc, rc_size, "%s%s%s", base_rc, test->opts.rc ? test->opts.rc : "",
             test->opts.rc ? "\n" : "");
    write_file(rc_path, rc);
    free(rc);

    if (test->opts.x11 && !xwayland_available()) {
        append_note(dir, "Xwayland is not installed");
        remove_tree(runtime);
        return RESULT_SKIP;
    }

    pid_t compositor = start_compositor(test, dir, home, runtime);

    char wayland[256] = "", x11[256] = "";
    if (!wait_for_displays(compositor, home, wayland, sizeof(wayland), x11,
                           sizeof(x11))) {
        bool crashed;
        int sig;
        stop_compositor(compositor, &crashed, &sig);
        append_note(dir, "the compositor did not come up (see poison.log)");
        remove_tree(runtime);
        return RESULT_FAIL;
    }

    char test_log_path[PATH_MAX];
    path_join(test_log_path, sizeof(test_log_path), dir, "test.log");

    int64_t started = now_ms();
    pid_t child = fork();
    if (child < 0) {
        append_note(dir, "cannot start the test: %s", strerror(errno));
        bool crashed;
        int sig;
        stop_compositor(compositor, &crashed, &sig);
        remove_tree(runtime);
        return RESULT_FAIL;
    }
    if (child == 0) {
        setpgid(0, 0);
        default_signals();
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        int fd = open(test_log_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                      0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
        }
        setenv("WAYLAND_DISPLAY", wayland, 1);
        if (x11[0]) {
            setenv("DISPLAY", x11, 1);
        } else {
            unsetenv("DISPLAY");
        }
        setenv("XDG_RUNTIME_DIR", runtime, 1);
        setenv("HOME", home, 1);
        current_dir = dir;
        snprintf(current_home, sizeof(current_home), "%s", home);
        current_compositor = compositor;
        test->run();
        if (getenv("POISON_TEST_PEEK")) {
            struct shot *shot = screen_settle();
            test_log("final screen: %s", shot_dump(shot, "final"));
            shot_free(shot);
        }
        exit(current_failed ? 1 : 0);
    }
    setpgid(child, child);

    int timeout = test->opts.timeout > 0 ? test->opts.timeout : DEFAULT_TIMEOUT;
    int status = 0;
    bool timed_out = false;
    for (;;) {
        pid_t done = waitpid(child, &status, WNOHANG);
        if (done == child) {
            break;
        }
        if (now_ms() - started > timeout * 1000 || interrupted) {
            timed_out = !interrupted;
            kill(-child, SIGKILL);
            waitpid(child, &status, 0);
            break;
        }
        sleep_ms(20);
    }
    kill(-child, SIGKILL);

    bool crashed = false;
    int crash_signal = 0;
    int compositor_status = stop_compositor(compositor, &crashed, &crash_signal);
    (void)compositor_status;

    int result;
    bool plain = false;
    if (interrupted) {
        append_note(dir, "interrupted");
        result = RESULT_FAIL;
    } else if (timed_out) {
        append_note(dir, "timed out after %d seconds", timeout);
        result = RESULT_FAIL;
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SKIP) {
        result = RESULT_SKIP;
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        result = RESULT_PASS;
    } else {
        if (WIFSIGNALED(status)) {
            append_note(dir, "test process died from signal %d (%s)",
                        WTERMSIG(status), strsignal(WTERMSIG(status)));
        }
        plain = WIFEXITED(status) && WEXITSTATUS(status) == 1;
        result = RESULT_FAIL;
    }

    if (crashed) {
        if (crash_signal) {
            append_note(dir, "the compositor died from signal %d (%s)",
                        crash_signal, strsignal(crash_signal));
        } else {
            append_note(dir, "the compositor exited during the test");
        }
        if (result != RESULT_SKIP) {
            result = RESULT_FAIL;
            plain = false;
        }
    }

    char log_path[PATH_MAX];
    path_join(log_path, sizeof(log_path), dir, "poison.log");
    char *log = read_file(log_path);
    if (log) {
        const char *problem = log_problem(log);
        if (problem) {
            int len = (int)strcspn(problem, "\n");
            append_note(dir, "compositor log: %.*s", len, problem);
            if (result != RESULT_SKIP) {
                result = RESULT_FAIL;
                plain = false;
            }
        }
        free(log);
    }

    remove_tree(runtime);

    char issue[PATH_MAX];
    path_join(issue, sizeof(issue), dir, "known-issue.txt");
    if (test->opts.xfail || access(issue, F_OK) == 0) {
        if (result == RESULT_FAIL && plain) {
            result = RESULT_XFAIL;
        } else if (result == RESULT_PASS) {
            result = RESULT_XPASS;
        }
    }
    *plain_failure = result == RESULT_FAIL && plain;
    return result;
}

static int supervise(const struct test_case *test, const char *dir) {
    int attempts = test->opts.flaky ? 3 : 1;
    int result = RESULT_FAIL;
    for (int attempt = 1; attempt <= attempts; attempt++) {
        char previous[PATH_MAX];
        if (attempt > 1) {
            if ((size_t)snprintf(previous, sizeof(previous), "%s.attempt%d", dir,
                                 attempt - 1) >= sizeof(previous)) {
                break;
            }
            rename(dir, previous);
        }
        bool plain_failure;
        result = supervise_once(test, dir, &plain_failure);
        if (result == RESULT_FAIL && (!plain_failure || interrupted)) {
            break;
        }
        if (result != RESULT_FAIL) {
            if (attempt > 1) {
                append_note(dir, "failed %d time%s before passing", attempt - 1,
                            attempt > 2 ? "s" : "");
                for (int i = 1; i < attempt && !opts.keep; i++) {
                    if ((size_t)snprintf(previous, sizeof(previous), "%s.attempt%d",
                                         dir, i) < sizeof(previous)) {
                        remove_tree(previous);
                    }
                }
                if (result == RESULT_PASS) {
                    result = RESULT_FLAKY;
                }
            }
            break;
        }
    }
    return result;
}

static bool selected(const struct test_case *test) {
    if (test->opts.stress && !opts.stress) {
        return false;
    }
    if (opts.pattern_count == 0) {
        return true;
    }
    for (int i = 0; i < opts.pattern_count; i++) {
        const char *pattern = opts.patterns[i];
        if (fnmatch(pattern, test->name, 0) == 0 || strstr(test->name, pattern)) {
            return true;
        }
    }
    return false;
}

struct job {
    const struct test_case *test;
    char dir[PATH_MAX];
    pid_t pid;
    int64_t started;
    int64_t elapsed;
    int result;
    bool running;
    bool done;
};

static struct job *next_job(struct job *jobs, int count) {
    for (int i = 0; i < count; i++) {
        struct job *job = &jobs[i];
        if (job->running || job->done) {
            continue;
        }
        bool blocked = false;
        for (int j = 0; job->test->opts.serial && j < count && !blocked; j++) {
            blocked = jobs[j].running && jobs[j].test->opts.serial &&
                strcmp(jobs[j].test->opts.serial, job->test->opts.serial) == 0;
        }
        if (!blocked) {
            return job;
        }
    }
    return NULL;
}

/* Debug chatter from wlroots and Xwayland's keymap warnings drown out
 * anything useful at the end of a compositor log. */
static bool noise(const char *line) {
    return strstr(line, "[DEBUG]") || strstr(line, "Could not resolve keysym") ||
        strncmp(line, "> ", 2) == 0 || strstr(line, "Errors from xkbcomp") ||
        strstr(line, "The XKEYBOARD keymap compiler");
}

static char *strip_noise(char *text) {
    char *out = text;
    for (char *line = text; *line;) {
        char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line + 1) : strlen(line);
        char saved = line[len - (end ? 1 : 0)];
        line[len - (end ? 1 : 0)] = '\0';
        bool skip = noise(line);
        line[len - (end ? 1 : 0)] = saved;
        if (!skip) {
            memmove(out, line, len);
            out += len;
        }
        line += len;
    }
    *out = '\0';
    return text;
}

static void print_file_tail(const char *path, const char *label, int lines) {
    char *text = read_file(path);
    if (!text || !*text) {
        free(text);
        return;
    }
    strip_noise(text);
    int count = 0;
    char *start = text + strlen(text);
    while (start > text) {
        start--;
        if (*start == '\n' && start[1] != '\0' && ++count > lines) {
            start++;
            break;
        }
    }
    fprintf(stdout, "    --- %s ---\n", label);
    for (char *save = NULL, *line = strtok_r(start, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        fprintf(stdout, "    %s\n", line);
    }
    free(text);
}

static void report(struct job *job) {
    const struct test_case *test = job->test;
    printf("%-6s %-58s %5.1fs\n", result_names[job->result], test->name,
           job->elapsed / 1000.0);

    char path[PATH_MAX];
    if (job->result == RESULT_XFAIL || job->result == RESULT_XPASS) {
        snprintf(path, sizeof(path), "%s/known-issue.txt", job->dir);
        char *issue = read_file(path);
        printf("    known issue: %s\n",
               issue && *issue ? issue : test->opts.xfail);
        free(issue);
    }
    if (job->result == RESULT_FLAKY) {
        printf("    known race: %s\n", test->opts.flaky);
    }

    snprintf(path, sizeof(path), "%s/result.txt", job->dir);
    char *notes = read_file(path);

    bool loud = job->result == RESULT_FAIL || job->result == RESULT_XPASS ||
        (opts.verbose && job->result != RESULT_PASS);
    if (notes && *notes &&
        (loud || job->result == RESULT_SKIP || job->result == RESULT_FLAKY)) {
        for (char *save = NULL, *line = strtok_r(notes, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            printf("    %s\n", line);
        }
    }
    free(notes);

    if (loud || (opts.verbose && job->result == RESULT_SKIP)) {
        snprintf(path, sizeof(path), "%s/test.log", job->dir);
        print_file_tail(path, "test output", 40);
    }
    if (job->result == RESULT_FAIL || job->result == RESULT_XPASS) {
        snprintf(path, sizeof(path), "%s/poison.log", job->dir);
        print_file_tail(path, "compositor log (tail)", 15);
        printf("    logs and screenshots: %s\n", job->dir);
    }
    if (job->result == RESULT_SKIP && !opts.verbose) {
        snprintf(path, sizeof(path), "%s/test.log", job->dir);
        char *why = read_file(path);
        if (why && *why) {
            char *last = why + strlen(why);
            while (last > why && (last[-1] == '\n')) {
                *--last = '\0';
            }
            char *line = strrchr(why, '\n');
            printf("    %s\n", line ? line + 1 : why);
        }
        free(why);
    }
    fflush(stdout);

    bool keep = opts.keep || job->result == RESULT_FAIL ||
        job->result == RESULT_XPASS;
    if (!keep) {
        remove_tree(job->dir);
    }
}

static void usage(const char *argv0) {
    printf("Usage: %s [OPTIONS] [PATTERN...]\n"
           "Runs poison's window placement tests against a headless compositor.\n"
           "\n"
           "  -c, --compositor PATH  compositor binary (default: ./poison)\n"
           "  -j, --jobs N           tests to run at once (default: 4)\n"
           "  -k, --keep             keep every test's logs and screenshots\n"
           "  -l, --list             list the tests and exit\n"
           "  -s, --stress           also run the slow race hunts\n"
           "  -v, --verbose          print output of skipped and known-failing tests\n"
           "  -w, --workdir DIR      where to put per-test directories\n"
           "\n"
           "PATTERN is a glob or substring matched against test names.\n",
           argv0);
}

int main(int argc, char *argv[]) {
    opts.compositor = "./poison";
    opts.jobs = 4;

    static const struct option long_options[] = {
        {"compositor", required_argument, NULL, 'c'},
        {"jobs", required_argument, NULL, 'j'},
        {"keep", no_argument, NULL, 'k'},
        {"list", no_argument, NULL, 'l'},
        {"stress", no_argument, NULL, 's'},
        {"verbose", no_argument, NULL, 'v'},
        {"workdir", required_argument, NULL, 'w'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0},
    };

    int c;
    while ((c = getopt_long(argc, argv, "c:j:klsvw:h", long_options, NULL)) != -1) {
        switch (c) {
        case 'c':
            opts.compositor = optarg;
            break;
        case 'j':
            opts.jobs = atoi(optarg);
            if (opts.jobs < 1) {
                opts.jobs = 1;
            }
            break;
        case 'k':
            opts.keep = true;
            break;
        case 'l':
            opts.list = true;
            break;
        case 's':
            opts.stress = true;
            break;
        case 'v':
            opts.verbose = true;
            break;
        case 'w':
            opts.workdir = optarg;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 2;
        }
    }
    opts.patterns = argv + optind;
    opts.pattern_count = argc - optind;

    struct job *jobs = calloc(test_count, sizeof(*jobs));
    if (!jobs) {
        die("out of memory");
    }
    int job_count = 0;
    for (int i = 0; i < test_count; i++) {
        if (selected(tests[i])) {
            jobs[job_count++].test = tests[i];
        }
    }

    if (opts.list) {
        for (int i = 0; i < job_count; i++) {
            printf("%s%s%s\n", jobs[i].test->name,
                   jobs[i].test->opts.xfail ? " (known issue)" : "",
                   jobs[i].test->opts.stress ? " (stress)" : "");
        }
        free(jobs);
        return 0;
    }
    if (job_count == 0) {
        fprintf(stderr, "poison-test: no tests match\n");
        free(jobs);
        return 2;
    }

    char compositor[PATH_MAX];
    if (!realpath(opts.compositor, compositor)) {
        die("cannot find the compositor at %s", opts.compositor);
    }
    opts.compositor = compositor;

    char source_dir[PATH_MAX];
    if (!getenv("POISON_TEST_SOURCE_DIR") && realpath(argv[0], source_dir)) {
        for (int i = 0; i < 2; i++) {
            char *slash = strrchr(source_dir, '/');
            if (slash) {
                *slash = '\0';
            }
        }
        setenv("POISON_TEST_SOURCE_DIR", source_dir, 1);
    }

    char workdir[PATH_MAX];
    if (opts.workdir) {
        mkdir(opts.workdir, 0755);
        if (!realpath(opts.workdir, workdir)) {
            die("cannot use %s", opts.workdir);
        }
        size_t used = strlen(workdir);
        if ((size_t)snprintf(workdir + used, sizeof(workdir) - used,
                             "/run-XXXXXX") >= sizeof(workdir) - used) {
            die("work directory path too long");
        }
    } else {
        const char *tmp = getenv("TMPDIR");
        snprintf(workdir, sizeof(workdir), "%s/poison-check-XXXXXX",
                 tmp && *tmp ? tmp : "/tmp");
    }
    if (!mkdtemp(workdir)) {
        die("cannot create %s: %s", workdir, strerror(errno));
    }

    signal(SIGPIPE, SIG_IGN);
    catch_interrupts();

    printf("Testing %s with %d test%s, %d at a time.\n", compositor, job_count,
           job_count == 1 ? "" : "s", opts.jobs);
    fflush(stdout);

    int running = 0, reported = 0;
    int counts[RESULT_COUNT] = {0};
    int64_t suite_start = now_ms();

    while (reported < job_count) {
        struct job *job;
        while (!interrupted && running < opts.jobs &&
               (job = next_job(jobs, job_count)) != NULL) {
            path_join(job->dir, sizeof(job->dir), workdir, job->test->name);
            job->started = now_ms();
            fflush(stdout);
            pid_t pid = fork();
            if (pid < 0) {
                die("fork failed: %s", strerror(errno));
            }
            if (pid == 0) {
                prctl(PR_SET_PDEATHSIG, SIGTERM);
                _exit(EXIT_RESULT + supervise(job->test, job->dir));
            }
            job->pid = pid;
            job->running = true;
            running++;
        }
        if (running == 0) {
            break;
        }

        int status;
        pid_t pid = wait(&status);
        if (pid < 0) {
            if (errno == EINTR) {
                for (int i = 0; interrupted && i < job_count; i++) {
                    if (jobs[i].running) {
                        kill(jobs[i].pid, SIGTERM);
                    }
                }
                continue;
            }
            die("wait failed: %s", strerror(errno));
        }
        for (int i = 0; i < job_count; i++) {
            if (jobs[i].pid == pid && jobs[i].running) {
                int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                jobs[i].running = false;
                jobs[i].done = true;
                jobs[i].elapsed = now_ms() - jobs[i].started;
                jobs[i].result = code >= EXIT_RESULT && code < EXIT_RESULT + RESULT_COUNT
                    ? code - EXIT_RESULT
                    : RESULT_FAIL;
                running--;
                break;
            }
        }

        while (reported < job_count && jobs[reported].done) {
            report(&jobs[reported]);
            counts[jobs[reported].result]++;
            reported++;
        }
    }

    if (interrupted) {
        printf("\nInterrupted, logs are in %s\n", workdir);
        free(jobs);
        return 128 + interrupted;
    }

    printf("\n%d passed, %d failed, %d skipped, %d known issues, %d unexpected "
           "passes, %d passed after retrying in %.1fs\n",
           counts[RESULT_PASS], counts[RESULT_FAIL], counts[RESULT_SKIP],
           counts[RESULT_XFAIL], counts[RESULT_XPASS], counts[RESULT_FLAKY],
           (now_ms() - suite_start) / 1000.0);

    bool ok = counts[RESULT_FAIL] == 0 && counts[RESULT_XPASS] == 0;
    if (!opts.keep && ok) {
        remove_tree(workdir);
    } else {
        rmdir(workdir);
    }
    free(jobs);
    return ok ? 0 : 1;
}
