/* © 2026 dancingmirrors */

#include <errno.h>
#include <sys/socket.h>

#include "poison.h"

#define DIAG_RING_LINES 256
#define DIAG_LINE_CAP 512

static char diag_ring[DIAG_RING_LINES][DIAG_LINE_CAP];
static int diag_ring_head;
static int diag_ring_count;

static struct poison_server *g_diag_server = NULL;

static const char *diag_curated[] = {
    "too big for buffer",
    "protocol error",
    "killing client",
    "out of memory",
    "failed to allocate",
    "no space left",
    "disconnect",
    NULL,
};

static char ascii_lower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

static bool str_contains_ci(const char *haystack, const char *needle) {
    if (!needle[0]) {
        return true;
    }
    for (const char *h = haystack; *h; h++) {
        const char *a = h;
        const char *b = needle;
        while (*a && *b && ascii_lower(*a) == ascii_lower(*b)) {
            a++;
            b++;
        }
        if (!*b) {
            return true;
        }
    }
    return false;
}

static bool diag_is_curated(const char *msg) {
    for (int i = 0; diag_curated[i] != NULL; i++) {
        if (str_contains_ci(msg, diag_curated[i])) {
            return true;
        }
    }
    return false;
}

static void diag_sanitize(const char *in, char *out, size_t outsz) {
    size_t j = 0;
    for (size_t i = 0; in[i] != '\0' && j + 1 < outsz; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x20 || c == 0x7f) {
            out[j++] = ' ';
        } else {
            out[j++] = (char)c;
        }
    }
    while (j > 0 && out[j - 1] == ' ') {
        j--;
    }
    out[j] = '\0';

    size_t lead = 0;
    while (out[lead] == ' ') {
        lead++;
    }
    if (lead > 0) {
        memmove(out, out + lead, j - lead + 1);
    }
}

static void diag_write_record(int fd, const char *record) {
    char out[DIAG_LINE_CAP + 2];
    int n = snprintf(out, sizeof(out), "%s\n", record);
    if (n <= 0) {
        return;
    }
    if (n > (int)sizeof(out)) {
        n = (int)sizeof(out);
    }
    ssize_t w = send(fd, out, (size_t)n, MSG_NOSIGNAL);
    (void)w;
}

static void diag_emit(struct poison_server *server, char cls, const char *msg) {
    char clean[DIAG_LINE_CAP - 2];
    diag_sanitize(msg, clean, sizeof(clean));
    if (clean[0] == '\0') {
        return;
    }

    char *slot = diag_ring[diag_ring_head];
    snprintf(slot, DIAG_LINE_CAP, "%c %s", cls, clean);
    diag_ring_head = (diag_ring_head + 1) % DIAG_RING_LINES;
    if (diag_ring_count < DIAG_RING_LINES) {
        diag_ring_count++;
    }

    if (server->console_fd >= 0) {
        diag_write_record(server->console_fd, slot);
    }
}

static const char *diag_wlr_level(enum wlr_log_importance importance) {
    switch (importance) {
    case WLR_ERROR:
        return "ERROR";
    case WLR_INFO:
        return "INFO";
    case WLR_DEBUG:
        return "DEBUG";
    default:
        return "?";
    }
}

static void poison_diag_wlr_log(enum wlr_log_importance importance,
                                const char *fmt, va_list args) {
    static bool in_handler = false;

    char buf[DIAG_LINE_CAP];
    vsnprintf(buf, sizeof(buf), fmt, args);

    fprintf(stderr, "[%s] %s\n", diag_wlr_level(importance), buf);

    if (in_handler || g_diag_server == NULL) {
        return;
    }

    char cls = '\0';
    if (importance <= WLR_ERROR) {
        cls = 'E';
    } else if (diag_is_curated(buf)) {
        cls = 'W';
    }
    if (cls == '\0') {
        return;
    }

    in_handler = true;
    diag_emit(g_diag_server, cls, buf);
    in_handler = false;
}

static void poison_diag_wl_log(const char *fmt, va_list args) {
    static bool in_handler = false;

    char buf[DIAG_LINE_CAP];
    vsnprintf(buf, sizeof(buf), fmt, args);

    fprintf(stderr, "poison[wl-server] %s", buf);

    if (in_handler || g_diag_server == NULL) {
        return;
    }

    in_handler = true;
    diag_emit(g_diag_server, 'E', buf);
    in_handler = false;
}

static void stop_console(struct poison_server *server) {
    if (server->console_event_source) {
        wl_event_source_remove(server->console_event_source);
        server->console_event_source = NULL;
    }
    if (server->console_fd >= 0) {
        close(server->console_fd);
        server->console_fd = -1;
    }
    if (server->console_pid > 0) {
        kill(server->console_pid, SIGTERM);
        server->console_pid = 0;
    }
}

static int console_fd_event(int fd, uint32_t mask, void *data) {
    struct poison_server *server = data;

    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        stop_console(server);
        return 0;
    }

    if (mask & WL_EVENT_READABLE) {
        char scratch[256];
        ssize_t n = read(fd, scratch, sizeof(scratch));
        if (n == 0) {
            stop_console(server);
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                   errno != EINTR) {
            stop_console(server);
        }
    }

    return 0;
}

static void start_console(struct poison_server *server) {
    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == -1) {
        wlr_log(WLR_ERROR, "Failed to create socketpair for console!");
        return;
    }

    pid_t pid = fork();
    if (pid == -1) {
        wlr_log(WLR_ERROR, "Failed to fork console!");
        close(sp[0]);
        close(sp[1]);
        return;
    }

    if (pid == 0) {
        close(sp[0]);
        if (sp[1] != STDIN_FILENO) {
            dup2(sp[1], STDIN_FILENO);
            close(sp[1]);
        }

        execlp("poison-console", "poison-console", (char *)NULL);
        execl("/usr/local/bin/poison-console", "poison-console", (char *)NULL);
        execl("./poison-console", "poison-console", (char *)NULL);
        _exit(1);
    }

    close(sp[1]);
    fcntl(sp[0], F_SETFD, FD_CLOEXEC);
    int flags = fcntl(sp[0], F_GETFL, 0);
    if (flags >= 0) {
        fcntl(sp[0], F_SETFL, flags | O_NONBLOCK);
    }

    server->console_fd = sp[0];
    server->console_pid = pid;

    struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);
    server->console_event_source = wl_event_loop_add_fd(
        loop, sp[0], WL_EVENT_READABLE, console_fd_event, server);
    if (!server->console_event_source) {
        wlr_log(WLR_ERROR, "Failed to add console fd to event loop!");
        stop_console(server);
        return;
    }

    for (int i = 0; i < diag_ring_count; i++) {
        int idx = (diag_ring_head - diag_ring_count + i +
                   DIAG_RING_LINES * 2) %
            DIAG_RING_LINES;
        diag_write_record(server->console_fd, diag_ring[idx]);
    }
}

void poison_console_toggle(struct poison_server *server) {
    if (server->console_pid > 0) {
        stop_console(server);
    } else if (server->window_selector_fd == -1 &&
               server->launcher_fd == -1) {
        start_console(server);
    }
}

void poison_diag_init(struct poison_server *server) {
    g_diag_server = server;
    server->console_pid = 0;
    server->console_fd = -1;
    server->console_event_source = NULL;
    diag_ring_head = 0;
    diag_ring_count = 0;

    wl_log_set_handler_server(poison_diag_wl_log);
    wlr_log_init(WLR_INFO, poison_diag_wlr_log);
}

void poison_diag_finish(struct poison_server *server) {
    stop_console(server);
    g_diag_server = NULL;
}
