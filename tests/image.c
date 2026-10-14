#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "poison-test.h"

#define OUTSIDE 0xffffffffu

void shot_free(struct shot *shot) {
    if (shot) {
        free(shot->pixels);
        free(shot);
    }
}

uint32_t shot_pixel(struct shot *shot, int x, int y) {
    if (x < 0 || y < 0 || x >= shot->width || y >= shot->height) {
        return OUTSIDE;
    }
    return shot->pixels[y * shot->width + x];
}

struct box shot_find(struct shot *shot, uint32_t color, int *count_out) {
    int min_x = INT_MAX, min_y = INT_MAX, max_x = -1, max_y = -1, count = 0;
    for (int y = 0; y < shot->height; y++) {
        const uint32_t *row = &shot->pixels[y * shot->width];
        for (int x = 0; x < shot->width; x++) {
            if (row[x] == color) {
                count++;
                if (x < min_x) {
                    min_x = x;
                }
                if (x > max_x) {
                    max_x = x;
                }
                if (y < min_y) {
                    min_y = y;
                }
                if (y > max_y) {
                    max_y = y;
                }
            }
        }
    }
    if (count_out) {
        *count_out = count;
    }
    if (count == 0) {
        return BOX(0, 0, 0, 0);
    }
    return BOX(min_x, min_y, max_x - min_x + 1, max_y - min_y + 1);
}

int shot_count(struct shot *shot, uint32_t color) {
    int count;
    shot_find(shot, color, &count);
    return count;
}

int shot_count_other(struct shot *shot, const uint32_t *colors, int n) {
    int count = 0;
    size_t total = (size_t)shot->width * shot->height;
    for (size_t i = 0; i < total; i++) {
        uint32_t p = shot->pixels[i];
        if (p == 0) {
            continue;
        }
        bool known = false;
        for (int j = 0; j < n; j++) {
            if (p == colors[j]) {
                known = true;
                break;
            }
        }
        if (!known) {
            count++;
        }
    }
    return count;
}

bool shot_box_filled(struct shot *shot, struct box box, uint32_t color) {
    for (int y = box.y; y < box.y + box.height; y++) {
        for (int x = box.x; x < box.x + box.width; x++) {
            if (shot_pixel(shot, x, y) != color) {
                return false;
            }
        }
    }
    return true;
}

bool shot_is_black(struct shot *shot) {
    size_t total = (size_t)shot->width * shot->height;
    for (size_t i = 0; i < total; i++) {
        if (shot->pixels[i] != 0) {
            return false;
        }
    }
    return true;
}

static bool is_border(uint32_t p) {
    return p == FRAME_ACTIVE_BORDER || p == FRAME_INACTIVE_BORDER;
}

static int title_kind(uint32_t p) {
    if (p == OUTSIDE) {
        return 0;
    }
    uint32_t r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
    if (r == 0 && b == 0 && g >= 0xa0) {
        return 1;
    }
    if (r == 0 && g == 0 && b >= 0xa0) {
        return 2;
    }
    return 0;
}

static bool is_frame_pixel(uint32_t p) {
    return is_border(p) || title_kind(p) != 0;
}

struct frame_info shot_frame(struct shot *shot, struct box c) {
    struct frame_info f = {0};
    int mid_x = c.x + c.width / 2;
    int mid_y = c.y + c.height / 2;
    uint32_t border_seen = 0;

    for (int x = c.x - 1; is_border(shot_pixel(shot, x, mid_y)); x--) {
        border_seen = shot_pixel(shot, x, mid_y);
        f.left++;
    }
    for (int x = c.x + c.width; is_border(shot_pixel(shot, x, mid_y)); x++) {
        border_seen = shot_pixel(shot, x, mid_y);
        f.right++;
    }
    for (int y = c.y + c.height; is_border(shot_pixel(shot, mid_x, y)); y++) {
        border_seen = shot_pixel(shot, mid_x, y);
        f.bottom++;
    }

    int y = c.y - 1;
    int separator = 0;
    while (is_border(shot_pixel(shot, mid_x, y))) {
        border_seen = shot_pixel(shot, mid_x, y);
        separator++;
        y--;
    }
    int kind = 0;
    while (title_kind(shot_pixel(shot, mid_x, y))) {
        kind = title_kind(shot_pixel(shot, mid_x, y));
        f.title++;
        y--;
    }
    int top_border = 0;
    if (f.title > 0) {
        while (is_border(shot_pixel(shot, mid_x, y))) {
            top_border++;
            y--;
        }
    }
    f.top = separator + f.title + top_border;

    f.found = f.left || f.right || f.top || f.bottom;
    if (kind) {
        f.active = kind == 1;
    } else {
        f.active = border_seen == FRAME_ACTIVE_BORDER;
    }
    return f;
}

int shot_count_frame_pixels(struct shot *shot) {
    int count = 0;
    size_t total = (size_t)shot->width * shot->height;
    for (size_t i = 0; i < total; i++) {
        if (is_frame_pixel(shot->pixels[i])) {
            count++;
        }
    }
    return count;
}

const char *shot_dump(struct shot *shot, const char *tag) {
    static char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/shot-%02d-%s.ppm", test_dir(), shot->index,
             tag ? tag : "screen");
    FILE *file = fopen(path, "wb");
    if (!file) {
        return "(could not write screenshot)";
    }
    fprintf(file, "P6\n%d %d\n255\n", shot->width, shot->height);
    size_t total = (size_t)shot->width * shot->height;
    for (size_t i = 0; i < total; i++) {
        uint32_t p = shot->pixels[i];
        unsigned char rgb[3] = {(p >> 16) & 0xff, (p >> 8) & 0xff, p & 0xff};
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
    return path;
}

static bool shots_equal(struct shot *a, struct shot *b) {
    return a->width == b->width && a->height == b->height &&
        memcmp(a->pixels, b->pixels, (size_t)a->width * a->height * 4) == 0;
}

struct shot *screen_settle(void) {
    settle();
    struct shot *prev = screen_capture();
    int same = 0;
    for (int i = 0; i < 100 && same < 2; i++) {
        pump(30);
        settle();
        struct shot *cur = screen_capture();
        same = shots_equal(prev, cur) ? same + 1 : 0;
        shot_free(prev);
        prev = cur;
    }
    return prev;
}

struct shot *screen_wait(bool (*pred)(struct shot *shot, void *data),
                         void *data, int timeout_ms, bool *matched) {
    int64_t deadline = now_ms() + timeout_ms;
    for (;;) {
        settle();
        struct shot *shot = screen_capture();
        if (pred(shot, data)) {
            *matched = true;
            return shot;
        }
        if (now_ms() >= deadline) {
            *matched = false;
            return shot;
        }
        shot_free(shot);
        pump(30);
    }
}

static bool box_eq(struct box a, struct box b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

struct window_at {
    uint32_t color;
    struct box want;
};

static bool window_at_pred(struct shot *shot, void *data) {
    struct window_at *at = data;
    int count;
    struct box got = shot_find(shot, at->color, &count);
    return box_eq(got, at->want) && count == at->want.width * at->want.height;
}

struct shot *expect_window_at_impl(const char *file, int line, uint32_t color,
                                   struct box want) {
    struct window_at at = {color, want};
    bool matched;
    struct shot *shot = screen_wait(window_at_pred, &at, 3000, &matched);
    if (matched) {
        shot_free(shot);
        shot = screen_settle();
        matched = window_at_pred(shot, &at);
    }
    if (!matched) {
        int count;
        struct box got = shot_find(shot, color, &count);
        test_fail_at(file, line,
                     "window %06x: want %d,%d %dx%d, got %d,%d %dx%d with %d of "
                     "its %d pixels visible (%s)",
                     color, want.x, want.y, want.width, want.height, got.x,
                     got.y, got.width, got.height, count,
                     got.width * got.height, shot_dump(shot, "window-at"));
    }
    return shot;
}

void expect_frame_impl(const char *file, int line, struct shot *shot,
                       uint32_t color, bool want_frame, int want_active) {
    int count;
    struct box content = shot_find(shot, color, &count);
    if (count == 0) {
        test_fail_at(file, line, "window %06x is not on screen (%s)", color,
                     shot_dump(shot, "frame"));
        return;
    }
    struct frame_info f = shot_frame(shot, content);
    if (!want_frame) {
        if (f.found) {
            test_fail_at(file, line,
                         "window %06x should have no frame, found one with "
                         "left %d, top %d, right %d, bottom %d (%s)",
                         color, f.left, f.top, f.right, f.bottom,
                         shot_dump(shot, "frame"));
        }
        return;
    }
    if (!f.found) {
        test_fail_at(file, line, "window %06x should have a frame (%s)", color,
                     shot_dump(shot, "frame"));
        return;
    }
    if (f.left != FRAME_BORDER || f.right != FRAME_BORDER ||
        f.bottom != FRAME_BORDER || f.top != FRAME_TOP ||
        f.title != FRAME_TITLE) {
        test_fail_at(file, line,
                     "window %06x frame: want left/right/bottom %d, top %d with a "
                     "%d title; got left %d, right %d, bottom %d, top %d with a "
                     "%d title (%s)",
                     color, FRAME_BORDER, FRAME_TOP, FRAME_TITLE, f.left,
                     f.right, f.bottom, f.top, f.title,
                     shot_dump(shot, "frame"));
    }
    if (want_active >= 0 && f.active != (want_active != 0)) {
        test_fail_at(file, line, "window %06x frame should look %s (%s)", color,
                     want_active ? "active" : "inactive",
                     shot_dump(shot, "frame"));
    }
}

static bool gone_pred(struct shot *shot, void *data) {
    return shot_count(shot, *(uint32_t *)data) == 0;
}

void expect_gone_impl(const char *file, int line, uint32_t color) {
    bool matched;
    struct shot *shot = screen_wait(gone_pred, &color, 3000, &matched);
    if (matched) {
        shot_free(shot);
        shot = screen_settle();
        matched = gone_pred(shot, &color);
    }
    if (!matched) {
        int count;
        struct box got = shot_find(shot, color, &count);
        test_fail_at(file, line,
                     "window %06x should be gone, but %d pixels are still at "
                     "%d,%d %dx%d (%s)",
                     color, count, got.x, got.y, got.width, got.height,
                     shot_dump(shot, "gone"));
    }
    shot_free(shot);
}

struct box centered_padded(int width, int height, bool framed, int padding) {
    int side = framed ? FRAME_BORDER : 0;
    int top = framed ? FRAME_TOP : 0;
    int frame_width = width + 2 * side;
    int frame_height = height + top + side;
    int x = (OUTPUT_WIDTH - frame_width) / 2;
    int y = (OUTPUT_HEIGHT - frame_height) / 2;
    if (x < padding) {
        x = padding;
    }
    if (y < padding) {
        y = padding;
    }
    if (x + frame_width > OUTPUT_WIDTH - padding) {
        x = OUTPUT_WIDTH - padding - frame_width;
        if (x < padding) {
            x = padding;
        }
    }
    if (y + frame_height > OUTPUT_HEIGHT - padding) {
        y = OUTPUT_HEIGHT - padding - frame_height;
        if (y < padding) {
            y = padding;
        }
    }
    return BOX(x + side, y + top, width, height);
}

struct box centered(int width, int height, bool framed) {
    return centered_padded(width, height, framed, 5);
}

struct app *app_spawn(const char *name, char *const argv[],
                      const char *const env[]) {
    struct app *app = calloc(1, sizeof(*app));
    snprintf(app->log, sizeof(app->log), "%s/%s.log", test_dir(), name);

    pid_t pid = fork();
    REQUIRE(pid >= 0, "fork failed: %s", strerror(errno));
    if (pid == 0) {
        int fd = open(app->log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        for (int i = 0; env && env[i]; i++) {
            putenv(strdup(env[i]));
        }
        execvp(argv[0], argv);
        fprintf(stderr, "cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    app->pid = pid;
    return app;
}

bool app_running(struct app *app) {
    if (app->exited) {
        return false;
    }
    int status;
    pid_t done = waitpid(app->pid, &status, WNOHANG);
    if (done == app->pid) {
        app->exited = true;
        app->status = status;
        return false;
    }
    return true;
}

void app_kill(struct app *app) {
    if (!app_running(app)) {
        return;
    }
    kill(app->pid, SIGTERM);
    int64_t deadline = now_ms() + 3000;
    while (now_ms() < deadline && app_running(app)) {
        pump(20);
    }
    if (app_running(app)) {
        kill(app->pid, SIGKILL);
        waitpid(app->pid, &app->status, 0);
        app->exited = true;
    }
}

char *app_log(struct app *app) {
    char *text = read_file(app->log);
    return text ? text : strdup("");
}

const char *app_describe_exit(struct app *app) {
    static char text[128];
    if (!app->exited) {
        return "still running";
    }
    if (WIFSIGNALED(app->status)) {
        snprintf(text, sizeof(text), "killed by signal %d (%s)",
                 WTERMSIG(app->status), strsignal(WTERMSIG(app->status)));
    } else {
        snprintf(text, sizeof(text), "exited with status %d",
                 WEXITSTATUS(app->status));
    }
    return text;
}
