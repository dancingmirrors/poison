#ifndef POISON_TEST_H
#define POISON_TEST_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include <wayland-client.h>
#include <xcb/xcb.h>

#define OUTPUT_WIDTH 1280
#define OUTPUT_HEIGHT 720

#define FRAME_BORDER 2
#define FRAME_TITLE 20
#define FRAME_TOP (FRAME_BORDER + FRAME_TITLE + FRAME_BORDER)

#define C_RED 0xc03030
#define C_GREEN 0x30b040
#define C_BLUE 0x3040c0
#define C_ORANGE 0xd08020
#define C_PURPLE 0x8040b0
#define C_TEAL 0x20a0a0
#define C_GRAY 0x808080
#define C_BROWN 0x806040
#define C_PINK 0xe080a0
#define C_SHADOW 0x404040

#define FRAME_ACTIVE_BORDER 0xffff00
#define FRAME_INACTIVE_BORDER 0xff00ff

struct box {
    int x, y, width, height;
};

#define BOX(x_, y_, w_, h_) ((struct box){(x_), (y_), (w_), (h_)})

struct test_opts {
    const char *rc;
    const char *env;
    const char *xfail;
    const char *flaky;
    bool stress;
    bool x11;
    int timeout;
    const char *serial;
};

struct test_case {
    const char *name;
    const char *file;
    void (*run)(void);
    struct test_opts opts;
};

void test_register(const struct test_case *test);

#define TEST(fn, ...) \
    static void fn(void); \
    static const struct test_case test_case_##fn = { \
        .name = #fn, .file = __FILE__, .run = fn, .opts = {__VA_ARGS__}}; \
    __attribute__((constructor)) static void test_register_##fn(void) { \
        test_register(&test_case_##fn); \
    } \
    static void fn(void)

void test_fail_at(const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void test_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void test_skip(const char *fmt, ...)
    __attribute__((format(printf, 1, 2), noreturn));
void test_abort(void) __attribute__((noreturn));
void test_known_issue(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
bool test_failed(void);

#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            test_fail_at(__FILE__, __LINE__, __VA_ARGS__); \
        } \
    } while (0)

#define REQUIRE(cond, ...) \
    do { \
        if (!(cond)) { \
            test_fail_at(__FILE__, __LINE__, __VA_ARGS__); \
            test_abort(); \
        } \
    } while (0)

const char *test_dir(void);
const char *test_home(void);
pid_t test_compositor_pid(void);
bool test_compositor_alive(void);
char *test_compositor_log(void);
char *read_file(const char *path);
int64_t now_ms(void);
void sleep_ms(int ms);

enum size_policy {
    SIZE_OBEY,
    SIZE_FIXED,
    SIZE_ASPECT,
    SIZE_SNAP,
};

enum decoration_request {
    DECORATION_NONE,
    DECORATION_CLIENT,
    DECORATION_SERVER,
    DECORATION_UNSET,
};

struct window;

struct window_opts {
    uint32_t color;
    const char *title;
    const char *app_id;
    int width, height;
    enum size_policy policy;
    int snap_width, snap_height;
    int min_width, min_height, max_width, max_height;
    struct window *parent;
    bool fullscreen;
    bool maximized;
    enum decoration_request decoration;
    int shadow;
};

enum {
    STATE_MAXIMIZED = 1 << 0,
    STATE_FULLSCREEN = 1 << 1,
    STATE_RESIZING = 1 << 2,
    STATE_ACTIVATED = 1 << 3,
    STATE_TILED_LEFT = 1 << 4,
    STATE_TILED_RIGHT = 1 << 5,
    STATE_TILED_TOP = 1 << 6,
    STATE_TILED_BOTTOM = 1 << 7,
    STATE_SUSPENDED = 1 << 8,
};
#define STATE_TILED \
    (STATE_TILED_LEFT | STATE_TILED_RIGHT | STATE_TILED_TOP | STATE_TILED_BOTTOM)

struct client;

struct window {
    struct client *client;
    struct window_opts opts;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct zxdg_toplevel_decoration_v1 *decoration;

    int configured_width, configured_height;
    uint32_t states;
    int bounds_width, bounds_height;
    uint32_t wm_capabilities;
    int decoration_mode;
    int configures;
    bool close_requested;

    int width, height;
    bool committed;
    bool hidden;

    int pending_width, pending_height;
    uint32_t pending_states;
};

struct client *client_connect(void);
void client_destroy(struct client *client);
struct wl_display *client_display(struct client *client);
void client_roundtrip(struct client *client);

struct window *window_create(struct client *client,
                             const struct window_opts *opts);
void window_map(struct window *window);
void window_destroy(struct window *window);
void window_unmap(struct window *window);
void window_remap(struct window *window);
void window_redraw(struct window *window);
void window_commit_size(struct window *window, int width, int height);
void window_set_fullscreen(struct window *window, bool fullscreen);
void window_set_maximized(struct window *window, bool maximized);
void window_set_title(struct window *window, const char *title);
void window_set_min_size(struct window *window, int width, int height);
void window_set_max_size(struct window *window, int width, int height);

struct xwindow;

struct xwindow_opts {
    uint32_t color;
    int x, y, width, height;
    bool override_redirect;
    const char *title;
    const char *class;
    const char *type;
    struct xwindow *transient_for;
    int min_width, min_height, max_width, max_height;
    int base_width, base_height, inc_width, inc_height;
    int min_aspect_x, min_aspect_y, max_aspect_x, max_aspect_y;
    bool motif_no_border;
    bool motif_no_title;
    bool motif_no_resize;
    bool fullscreen;
    bool modal;
    bool maximized;
    bool no_input;
    bool static_content;
};

struct xwindow {
    xcb_window_t id;
    struct xwindow_opts opts;
    int x, y, width, height;
    bool mapped;
    bool delete_requested;
    int configure_notifies;
    int wm_state_writes;
    int64_t last_repaint;
};

xcb_connection_t *x11_connect(void);
void x11_flush(void);
void x11_sync(void);
xcb_atom_t x11_atom(const char *name);
void x11_set_background_traffic(bool enabled);

struct xwindow *xwindow_create(const struct xwindow_opts *opts);
void xwindow_map(struct xwindow *window);
void xwindow_unmap(struct xwindow *window);
void xwindow_withdraw(struct xwindow *window);
void xwindow_destroy(struct xwindow *window);
void xwindow_configure(struct xwindow *window, int x, int y, int width,
                       int height);
void xwindow_set_override_redirect(struct xwindow *window, bool enabled);
void xwindow_set_title(struct xwindow *window, const char *title);
void xwindow_set_size_hints(struct xwindow *window, int min_width,
                            int min_height, int max_width, int max_height);
void xwindow_set_transient_for(struct xwindow *window, struct xwindow *parent);
void xwindow_set_motif_no_resize(struct xwindow *window, bool no_resize);
void xwindow_set_type(struct xwindow *window, const char *type);
void xwindow_request_state(struct xwindow *window, int action,
                           const char *state, const char *state2);
void xwindow_reparent(struct xwindow *window, struct xwindow *parent, int x,
                      int y);
void xwindow_iconify(struct xwindow *window);
void xwindow_request_activate(struct xwindow *window);
int xwindow_wm_state(struct xwindow *window);
bool xwindow_viewable(struct xwindow *window);
bool xwindow_has_net_state(struct xwindow *window, const char *state);
xcb_window_t x11_active_window(void);
xcb_window_t x11_input_focus(void);
bool x11_client_list_contains(struct xwindow *window);
char *x11_describe_windows(void);
void x11_trace_windows(void);
xcb_window_t x11_find_toplevel(const char *name, struct box *box);

struct shot {
    int width, height;
    uint32_t *pixels;
    int index;
};

struct shot *screen_capture(void);
void shot_free(struct shot *shot);
struct shot *screen_settle(void);
struct shot *screen_wait(bool (*pred)(struct shot *shot, void *data),
                         void *data, int timeout_ms, bool *matched);
const char *shot_dump(struct shot *shot, const char *tag);

uint32_t shot_pixel(struct shot *shot, int x, int y);
struct box shot_find(struct shot *shot, uint32_t color, int *count);
int shot_count(struct shot *shot, uint32_t color);
int shot_count_other(struct shot *shot, const uint32_t *colors, int n);
bool shot_box_filled(struct shot *shot, struct box box, uint32_t color);
bool shot_is_black(struct shot *shot);

struct frame_info {
    bool found;
    bool active;
    int left, right, top, bottom, title;
};

struct frame_info shot_frame(struct shot *shot, struct box content);
int shot_count_frame_pixels(struct shot *shot);

void pump(int timeout_ms);

#define WAIT_UNTIL(cond, ms) \
    ({ \
        int64_t wait_deadline_ = now_ms() + (ms); \
        while (!(cond) && now_ms() < wait_deadline_) { \
            pump(20); \
        } \
        (bool)(cond); \
    })
void settle(void);
unsigned long test_activity(void);
void test_bump_activity(void);

struct shot *expect_window_at_impl(const char *file, int line, uint32_t color,
                                   struct box want);
#define expect_window_at(color, ...) \
    expect_window_at_impl(__FILE__, __LINE__, (color), BOX(__VA_ARGS__))
void expect_frame_impl(const char *file, int line, struct shot *shot,
                       uint32_t color, bool want_frame, int want_active);
#define expect_frame(shot, color, active) \
    expect_frame_impl(__FILE__, __LINE__, (shot), (color), true, (active))
#define expect_no_frame(shot, color) \
    expect_frame_impl(__FILE__, __LINE__, (shot), (color), false, -1)
void expect_gone_impl(const char *file, int line, uint32_t color);
#define expect_gone(color) expect_gone_impl(__FILE__, __LINE__, (color))

struct box centered(int width, int height, bool framed);
struct box centered_padded(int width, int height, bool framed, int padding);

struct app {
    pid_t pid;
    bool exited;
    int status;
    char log[4096];
};

bool have_program(const char *name);
struct app *app_spawn(const char *name, char *const argv[],
                      const char *const env[]);
bool app_running(struct app *app);
void app_kill(struct app *app);
char *app_log(struct app *app);
const char *app_describe_exit(struct app *app);

#endif
