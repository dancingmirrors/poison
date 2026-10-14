#define _GNU_SOURCE

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "poison-test.h"

#define MAX_XWINDOWS 128
#define MAX_ATOMS 128

#define HINT_P_MIN_SIZE (1u << 4)
#define HINT_P_MAX_SIZE (1u << 5)
#define HINT_P_RESIZE_INC (1u << 6)
#define HINT_P_ASPECT (1u << 7)
#define HINT_P_BASE_SIZE (1u << 8)

#define MWM_HINTS_FUNCTIONS (1u << 0)
#define MWM_HINTS_DECORATIONS (1u << 1)
#define MWM_FUNC_MOVE (1u << 2)
#define MWM_FUNC_MINIMIZE (1u << 3)
#define MWM_FUNC_CLOSE (1u << 5)
#define MWM_DECOR_BORDER (1u << 1)
#define MWM_DECOR_TITLE (1u << 3)

static xcb_connection_t *conn;
static xcb_screen_t *screen;
static struct xwindow *windows[MAX_XWINDOWS];
static int window_count;

static struct {
    char *name;
    xcb_atom_t atom;
} atoms[MAX_ATOMS];
static int atom_count;

int x11_fd(void) {
    return conn ? xcb_get_file_descriptor(conn) : -1;
}

bool x11_connected(void) {
    return conn != NULL;
}

xcb_atom_t x11_atom(const char *name) {
    for (int i = 0; i < atom_count; i++) {
        if (strcmp(atoms[i].name, name) == 0) {
            return atoms[i].atom;
        }
    }
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(
        conn, xcb_intern_atom(conn, 0, strlen(name), name), NULL);
    REQUIRE(reply, "cannot intern the %s atom", name);
    xcb_atom_t atom = reply->atom;
    free(reply);
    if (atom_count < MAX_ATOMS) {
        atoms[atom_count].name = strdup(name);
        atoms[atom_count].atom = atom;
        atom_count++;
    }
    return atom;
}

static FILE *trace;
static int64_t trace_start;

static xcb_get_property_reply_t *get_property(xcb_window_t window,
                                              const char *name, xcb_atom_t type);
static char *atom_name(xcb_atom_t atom);
static void describe_atoms(FILE *out, xcb_window_t window, const char *name,
                           const char *strip);
void x11_roundtrip(void);

static void trace_select(xcb_window_t window) {
    uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_void_cookie_t cookie = xcb_change_window_attributes_checked(
        conn, window, XCB_CW_EVENT_MASK, &mask);
    xcb_discard_reply(conn, cookie.sequence);
}

static void trace_event(xcb_generic_event_t *event) {
    bool synthetic = event->response_type & 0x80;
    uint8_t type = event->response_type & 0x7f;
    char head[32];
    snprintf(head, sizeof(head), "%7.3f%s", (now_ms() - trace_start) / 1000.0,
             synthetic ? " synthetic" : "");
    switch (type) {
    case XCB_CREATE_NOTIFY: {
        xcb_create_notify_event_t *ev = (xcb_create_notify_event_t *)event;
        if (ev->parent == screen->root) {
            fprintf(trace, "%s create 0x%x %dx%d+%d+%d%s\n", head, ev->window,
                    ev->width, ev->height, ev->x, ev->y,
                    ev->override_redirect ? " override-redirect" : "");
            trace_select(ev->window);
        }
        break;
    }
    case XCB_DESTROY_NOTIFY: {
        xcb_destroy_notify_event_t *ev = (xcb_destroy_notify_event_t *)event;
        if (ev->event == screen->root) {
            fprintf(trace, "%s destroy 0x%x\n", head, ev->window);
        }
        break;
    }
    case XCB_MAP_NOTIFY: {
        xcb_map_notify_event_t *ev = (xcb_map_notify_event_t *)event;
        if (ev->event == screen->root) {
            fprintf(trace, "%s map 0x%x%s\n", head, ev->window,
                    ev->override_redirect ? " override-redirect" : "");
        }
        break;
    }
    case XCB_UNMAP_NOTIFY: {
        xcb_unmap_notify_event_t *ev = (xcb_unmap_notify_event_t *)event;
        if (ev->event == screen->root) {
            fprintf(trace, "%s unmap 0x%x\n", head, ev->window);
        }
        break;
    }
    case XCB_CONFIGURE_NOTIFY: {
        xcb_configure_notify_event_t *ev = (xcb_configure_notify_event_t *)event;
        if (ev->event == screen->root) {
            fprintf(trace, "%s configure 0x%x %dx%d+%d+%d above 0x%x%s\n", head,
                    ev->window, ev->width, ev->height, ev->x, ev->y,
                    ev->above_sibling,
                    ev->override_redirect ? " override-redirect" : "");
        }
        break;
    }
    case XCB_REPARENT_NOTIFY: {
        xcb_reparent_notify_event_t *ev = (xcb_reparent_notify_event_t *)event;
        if (ev->event == screen->root) {
            fprintf(trace, "%s reparent 0x%x into 0x%x\n", head, ev->window,
                    ev->parent);
        }
        break;
    }
    case XCB_PROPERTY_NOTIFY: {
        xcb_property_notify_event_t *ev = (xcb_property_notify_event_t *)event;
        if (ev->atom == x11_atom("_POISON_TEST_TICK")) {
            break;
        }
        char *name = atom_name(ev->atom);
        fprintf(trace, "%s property 0x%x %s%s", head, ev->window, name,
                ev->state == XCB_PROPERTY_DELETE ? " deleted" : "");
        if (ev->state != XCB_PROPERTY_DELETE &&
            (strcmp(name, "_NET_WM_STATE") == 0 ||
             strcmp(name, "_NET_WM_WINDOW_TYPE") == 0)) {
            fprintf(trace, " [");
            describe_atoms(trace, ev->window, name, "_NET_WM_");
            fprintf(trace, "]");
        } else if (ev->state != XCB_PROPERTY_DELETE &&
                   (strcmp(name, "WM_STATE") == 0 ||
                    strcmp(name, "_NET_ACTIVE_WINDOW") == 0 ||
                    strcmp(name, "WM_TRANSIENT_FOR") == 0)) {
            xcb_get_property_reply_t *reply =
                get_property(ev->window, name, XCB_GET_PROPERTY_TYPE_ANY);
            if (reply && reply->format == 32 &&
                xcb_get_property_value_length(reply) >= 4) {
                fprintf(trace, " 0x%x", *(uint32_t *)xcb_get_property_value(reply));
            }
            free(reply);
        }
        fprintf(trace, "\n");
        free(name);
        break;
    }
    case XCB_CLIENT_MESSAGE: {
        xcb_client_message_event_t *ev = (xcb_client_message_event_t *)event;
        char *name = atom_name(ev->type);
        fprintf(trace, "%s message %s for 0x%x: %u %u %u %u %u\n", head, name,
                ev->window, ev->data.data32[0], ev->data.data32[1],
                ev->data.data32[2], ev->data.data32[3], ev->data.data32[4]);
        free(name);
        break;
    }
    default:
        break;
    }
}

void x11_trace_windows(void) {
    if (trace) {
        return;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/x11-events.log", test_dir());
    trace = fopen(path, "w");
    REQUIRE(trace, "cannot write %s", path);
    setvbuf(trace, NULL, _IOLBF, 0);
    trace_start = now_ms();
    uint32_t mask =
        XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
    xcb_change_window_attributes(conn, screen->root, XCB_CW_EVENT_MASK, &mask);
    xcb_query_tree_reply_t *tree =
        xcb_query_tree_reply(conn, xcb_query_tree(conn, screen->root), NULL);
    if (tree) {
        int n = xcb_query_tree_children_length(tree);
        xcb_window_t *children = xcb_query_tree_children(tree);
        for (int i = 0; i < n; i++) {
            trace_select(children[i]);
        }
        free(tree);
    }
    x11_roundtrip();
}

static struct xwindow *lookup(xcb_window_t id) {
    for (int i = 0; i < window_count; i++) {
        if (windows[i] && windows[i]->id == id) {
            return windows[i];
        }
    }
    return NULL;
}

static void handle_event(xcb_generic_event_t *event) {
    uint8_t type = event->response_type & 0x7f;
    if (trace) {
        trace_event(event);
    }
    switch (type) {
    case 0: {
        xcb_generic_error_t *error = (xcb_generic_error_t *)event;
        test_log("X11 error %u for resource 0x%x (request %u.%u)",
                 error->error_code, error->resource_id, error->major_code,
                 error->minor_code);
        break;
    }
    case XCB_CONFIGURE_NOTIFY: {
        xcb_configure_notify_event_t *ev = (xcb_configure_notify_event_t *)event;
        struct xwindow *window = lookup(ev->window);
        if (window && ev->event == ev->window) {
            window->x = ev->x;
            window->y = ev->y;
            window->width = ev->width;
            window->height = ev->height;
            window->configure_notifies++;
            test_bump_activity();
        }
        break;
    }
    case XCB_MAP_NOTIFY: {
        xcb_map_notify_event_t *ev = (xcb_map_notify_event_t *)event;
        struct xwindow *window = lookup(ev->window);
        if (window && ev->event == ev->window) {
            window->mapped = true;
            test_bump_activity();
        }
        break;
    }
    case XCB_UNMAP_NOTIFY: {
        xcb_unmap_notify_event_t *ev = (xcb_unmap_notify_event_t *)event;
        struct xwindow *window = lookup(ev->window);
        if (window && ev->event == ev->window) {
            window->mapped = false;
            test_bump_activity();
        }
        break;
    }
    case XCB_CLIENT_MESSAGE: {
        xcb_client_message_event_t *ev = (xcb_client_message_event_t *)event;
        struct xwindow *window = lookup(ev->window);
        if (window && ev->type == x11_atom("WM_PROTOCOLS") &&
            ev->data.data32[0] == x11_atom("WM_DELETE_WINDOW")) {
            window->delete_requested = true;
            test_bump_activity();
        }
        break;
    }
    case XCB_PROPERTY_NOTIFY: {
        xcb_property_notify_event_t *ev = (xcb_property_notify_event_t *)event;
        struct xwindow *window = lookup(ev->window);
        if (window && ev->atom == x11_atom("WM_STATE")) {
            window->wm_state_writes++;
        }
        if (ev->atom != x11_atom("_POISON_TEST_TICK")) {
            test_bump_activity();
        }
        break;
    }
    default:
        break;
    }
}

static bool background_traffic = true;
static int64_t last_tick;
static uint32_t tick_count;

void x11_set_background_traffic(bool enabled) {
    background_traffic = enabled;
}

static void make_traffic(void) {
    int64_t now = now_ms();
    bool flush = false;
    if (background_traffic && now - last_tick >= 50) {
        last_tick = now;
        tick_count++;
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, screen->root,
                            x11_atom("_POISON_TEST_TICK"), XCB_ATOM_CARDINAL,
                            32, 1, &tick_count);
        flush = true;
    }
    for (int i = 0; i < window_count; i++) {
        struct xwindow *window = windows[i];
        if (!window || !window->mapped || window->opts.static_content ||
            now - window->last_repaint < 200) {
            continue;
        }
        window->last_repaint = now;
        xcb_clear_area(conn, 0, window->id, 0, 0, 0, 0);
        flush = true;
    }
    if (flush) {
        xcb_flush(conn);
    }
}

void x11_dispatch(void) {
    if (!conn) {
        return;
    }
    make_traffic();
    xcb_generic_event_t *event;
    while ((event = xcb_poll_for_event(conn)) != NULL) {
        handle_event(event);
        free(event);
    }
    REQUIRE(!xcb_connection_has_error(conn), "lost the X11 connection%s",
            test_compositor_alive() ? "" : " (the compositor is gone)");
}

void x11_flush(void) {
    xcb_flush(conn);
}

void x11_roundtrip(void) {
    free(xcb_get_input_focus_reply(conn, xcb_get_input_focus(conn), NULL));
    x11_dispatch();
}

void x11_sync(void) {
    x11_roundtrip();
    settle();
}

xcb_connection_t *x11_connect(void) {
    if (conn) {
        return conn;
    }
    const char *display = getenv("DISPLAY");
    REQUIRE(display && *display, "no DISPLAY: XWayland did not start");

    int64_t deadline = now_ms() + 10000;
    int screen_number = 0;
    for (;;) {
        conn = xcb_connect(display, &screen_number);
        if (!xcb_connection_has_error(conn)) {
            break;
        }
        xcb_disconnect(conn);
        conn = NULL;
        REQUIRE(now_ms() < deadline, "cannot connect to X display %s", display);
        sleep_ms(50);
    }

    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(xcb_get_setup(conn));
    for (int i = 0; i < screen_number; i++) {
        xcb_screen_next(&iter);
    }
    screen = iter.data;

    xcb_atom_t wm_s0 = x11_atom("WM_S0");
    deadline = now_ms() + 10000;
    for (;;) {
        xcb_get_selection_owner_reply_t *reply = xcb_get_selection_owner_reply(
            conn, xcb_get_selection_owner(conn, wm_s0), NULL);
        bool owned = reply && reply->owner != XCB_NONE;
        free(reply);
        if (owned) {
            break;
        }
        REQUIRE(now_ms() < deadline, "no window manager on %s", display);
        sleep_ms(20);
    }

    uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(conn, screen->root, XCB_CW_EVENT_MASK, &mask);
    x11_roundtrip();
    return conn;
}

static void set_property(xcb_window_t window, const char *name, xcb_atom_t type,
                         uint8_t format, uint32_t count, const void *data) {
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, window, x11_atom(name),
                        type, format, count, data);
}

static void set_string(xcb_window_t window, const char *name, xcb_atom_t type,
                       const char *value) {
    set_property(window, name, type, 8, strlen(value), value);
}

static void write_size_hints(struct xwindow *window) {
    const struct xwindow_opts *o = &window->opts;
    uint32_t hints[18] = {0};
    if (o->min_width > 0 || o->min_height > 0) {
        hints[0] |= HINT_P_MIN_SIZE;
        hints[5] = o->min_width;
        hints[6] = o->min_height;
    }
    if (o->max_width > 0 || o->max_height > 0) {
        hints[0] |= HINT_P_MAX_SIZE;
        hints[7] = o->max_width;
        hints[8] = o->max_height;
    }
    if (o->inc_width > 0 || o->inc_height > 0) {
        hints[0] |= HINT_P_RESIZE_INC;
        hints[9] = o->inc_width;
        hints[10] = o->inc_height;
    }
    if (o->min_aspect_x > 0 || o->max_aspect_x > 0) {
        hints[0] |= HINT_P_ASPECT;
        hints[11] = o->min_aspect_x;
        hints[12] = o->min_aspect_y;
        hints[13] = o->max_aspect_x;
        hints[14] = o->max_aspect_y;
    }
    if (o->base_width > 0 || o->base_height > 0) {
        hints[0] |= HINT_P_BASE_SIZE;
        hints[15] = o->base_width;
        hints[16] = o->base_height;
    }
    set_property(window->id, "WM_NORMAL_HINTS", XCB_ATOM_WM_SIZE_HINTS, 32, 18,
                 hints);
}

static void write_motif_hints(struct xwindow *window) {
    const struct xwindow_opts *o = &window->opts;
    uint32_t motif[5] = {MWM_HINTS_DECORATIONS, 0, 0, 0, 0};
    if (!o->motif_no_border) {
        motif[2] |= MWM_DECOR_BORDER;
        if (!o->motif_no_title) {
            motif[2] |= MWM_DECOR_TITLE;
        }
    }
    if (o->motif_no_resize) {
        motif[0] |= MWM_HINTS_FUNCTIONS;
        motif[1] = MWM_FUNC_MOVE | MWM_FUNC_MINIMIZE | MWM_FUNC_CLOSE;
    }
    set_property(window->id, "_MOTIF_WM_HINTS", x11_atom("_MOTIF_WM_HINTS"), 32,
                 5, motif);
}

static void write_net_wm_state(struct xwindow *window) {
    const struct xwindow_opts *o = &window->opts;
    xcb_atom_t states[4];
    int n = 0;
    if (o->fullscreen) {
        states[n++] = x11_atom("_NET_WM_STATE_FULLSCREEN");
    }
    if (o->modal) {
        states[n++] = x11_atom("_NET_WM_STATE_MODAL");
    }
    if (o->maximized) {
        states[n++] = x11_atom("_NET_WM_STATE_MAXIMIZED_VERT");
        states[n++] = x11_atom("_NET_WM_STATE_MAXIMIZED_HORZ");
    }
    set_property(window->id, "_NET_WM_STATE", XCB_ATOM_ATOM, 32, n, states);
}

struct xwindow *xwindow_create(const struct xwindow_opts *opts) {
    REQUIRE(conn, "x11_connect() first");
    REQUIRE(window_count < MAX_XWINDOWS, "too many X11 windows");

    struct xwindow *window = calloc(1, sizeof(*window));
    window->opts = *opts;
    struct xwindow_opts *o = &window->opts;
    if (o->color == 0) {
        o->color = C_GRAY;
    }
    if (o->width <= 0) {
        o->width = 400;
    }
    if (o->height <= 0) {
        o->height = 300;
    }

    window->id = xcb_generate_id(conn);
    uint32_t values[] = {
        o->color,
        o->override_redirect ? 1 : 0,
        XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE |
            XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_EXPOSURE,
    };
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, window->id, screen->root,
                      o->x, o->y, o->width, o->height, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT |
                          XCB_CW_EVENT_MASK,
                      values);
    window->x = o->x;
    window->y = o->y;
    window->width = o->width;
    window->height = o->height;

    const char *title = o->title ? o->title : "poison-test";
    set_string(window->id, "WM_NAME", XCB_ATOM_STRING, title);
    set_string(window->id, "_NET_WM_NAME", x11_atom("UTF8_STRING"), title);
    const char *class = o->class ? o->class : "poison-test";
    char class_value[256];
    int len = snprintf(class_value, sizeof(class_value), "%s%c%s", class, '\0',
                       class);
    set_property(window->id, "WM_CLASS", XCB_ATOM_STRING, 8, len + 1, class_value);

    xcb_atom_t protocols[] = {x11_atom("WM_DELETE_WINDOW")};
    set_property(window->id, "WM_PROTOCOLS", XCB_ATOM_ATOM, 32, 1, protocols);

    uint32_t wm_hints[9] = {1, o->no_input ? 0 : 1, 1};
    set_property(window->id, "WM_HINTS", XCB_ATOM_WM_HINTS, 32, 9, wm_hints);

    write_size_hints(window);

    if (o->type) {
        xwindow_set_type(window, o->type);
    }
    if (o->transient_for) {
        xwindow_set_transient_for(window, o->transient_for);
    }
    if (o->motif_no_border || o->motif_no_title || o->motif_no_resize) {
        write_motif_hints(window);
    }
    if (o->fullscreen || o->modal || o->maximized) {
        write_net_wm_state(window);
    }

    windows[window_count++] = window;
    x11_flush();
    return window;
}

void xwindow_map(struct xwindow *window) {
    xcb_map_window(conn, window->id);
    x11_sync();
}

void xwindow_unmap(struct xwindow *window) {
    xcb_unmap_window(conn, window->id);
    x11_sync();
}

void xwindow_withdraw(struct xwindow *window) {
    xcb_unmap_window(conn, window->id);
    char buf[32] = {0};
    xcb_unmap_notify_event_t *ev = (xcb_unmap_notify_event_t *)buf;
    ev->response_type = XCB_UNMAP_NOTIFY;
    ev->event = screen->root;
    ev->window = window->id;
    ev->from_configure = 0;
    xcb_send_event(conn, 0, screen->root,
                   XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                       XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                   buf);
    x11_sync();
}

void xwindow_destroy(struct xwindow *window) {
    xcb_destroy_window(conn, window->id);
    for (int i = 0; i < window_count; i++) {
        if (windows[i] == window) {
            windows[i] = NULL;
        }
    }
    free(window);
    x11_sync();
}

void xwindow_configure(struct xwindow *window, int x, int y, int width,
                       int height) {
    uint32_t values[] = {(uint32_t)x, (uint32_t)y, (uint32_t)width,
                         (uint32_t)height};
    xcb_configure_window(conn, window->id,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                             XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                         values);
    x11_sync();
}

void xwindow_set_override_redirect(struct xwindow *window, bool enabled) {
    uint32_t value = enabled ? 1 : 0;
    window->opts.override_redirect = enabled;
    xcb_change_window_attributes(conn, window->id, XCB_CW_OVERRIDE_REDIRECT,
                                 &value);
    x11_flush();
}

void xwindow_set_title(struct xwindow *window, const char *title) {
    set_string(window->id, "WM_NAME", XCB_ATOM_STRING, title);
    set_string(window->id, "_NET_WM_NAME", x11_atom("UTF8_STRING"), title);
    x11_sync();
}

void xwindow_set_size_hints(struct xwindow *window, int min_width,
                            int min_height, int max_width, int max_height) {
    window->opts.min_width = min_width;
    window->opts.min_height = min_height;
    window->opts.max_width = max_width;
    window->opts.max_height = max_height;
    write_size_hints(window);
    x11_flush();
}

void xwindow_set_motif_no_resize(struct xwindow *window, bool no_resize) {
    window->opts.motif_no_resize = no_resize;
    write_motif_hints(window);
    x11_flush();
}

void xwindow_set_transient_for(struct xwindow *window, struct xwindow *parent) {
    xcb_window_t id = parent ? parent->id : XCB_NONE;
    set_property(window->id, "WM_TRANSIENT_FOR", XCB_ATOM_WINDOW, 32, 1, &id);
    x11_flush();
}

void xwindow_set_type(struct xwindow *window, const char *type) {
    char name[128];
    snprintf(name, sizeof(name), "_NET_WM_WINDOW_TYPE_%s", type);
    xcb_atom_t atom = x11_atom(name);
    set_property(window->id, "_NET_WM_WINDOW_TYPE", XCB_ATOM_ATOM, 32, 1, &atom);
    x11_flush();
}

static void send_to_root(xcb_window_t window, const char *type, uint32_t d0,
                         uint32_t d1, uint32_t d2, uint32_t d3, uint32_t d4) {
    xcb_client_message_event_t ev = {0};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = window;
    ev.type = x11_atom(type);
    ev.data.data32[0] = d0;
    ev.data.data32[1] = d1;
    ev.data.data32[2] = d2;
    ev.data.data32[3] = d3;
    ev.data.data32[4] = d4;
    xcb_send_event(conn, 0, screen->root,
                   XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                       XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                   (const char *)&ev);
}

void xwindow_request_state(struct xwindow *window, int action,
                           const char *state, const char *state2) {
    char name[128], name2[128];
    snprintf(name, sizeof(name), "_NET_WM_STATE_%s", state);
    xcb_atom_t second = XCB_NONE;
    if (state2) {
        snprintf(name2, sizeof(name2), "_NET_WM_STATE_%s", state2);
        second = x11_atom(name2);
    }
    send_to_root(window->id, "_NET_WM_STATE", (uint32_t)action, x11_atom(name),
                 second, 1, 0);
    x11_sync();
}

void xwindow_reparent(struct xwindow *window, struct xwindow *parent, int x,
                      int y) {
    xcb_reparent_window(conn, window->id, parent ? parent->id : screen->root,
                        (int16_t)x, (int16_t)y);
    x11_sync();
}

void xwindow_iconify(struct xwindow *window) {
    send_to_root(window->id, "WM_CHANGE_STATE", 3, 0, 0, 0, 0);
    x11_sync();
}

void xwindow_request_activate(struct xwindow *window) {
    send_to_root(window->id, "_NET_ACTIVE_WINDOW", 1, XCB_CURRENT_TIME,
                 XCB_NONE, 0, 0);
    x11_sync();
}

static xcb_get_property_reply_t *get_property(xcb_window_t window,
                                              const char *name, xcb_atom_t type) {
    return xcb_get_property_reply(
        conn, xcb_get_property(conn, 0, window, x11_atom(name), type, 0, 1024),
        NULL);
}

int xwindow_wm_state(struct xwindow *window) {
    xcb_get_property_reply_t *reply =
        get_property(window->id, "WM_STATE", x11_atom("WM_STATE"));
    int state = -1;
    if (reply && reply->format == 32 && xcb_get_property_value_length(reply) >= 4) {
        state = (int)((uint32_t *)xcb_get_property_value(reply))[0];
    }
    free(reply);
    return state;
}

bool xwindow_viewable(struct xwindow *window) {
    xcb_get_window_attributes_reply_t *reply = xcb_get_window_attributes_reply(
        conn, xcb_get_window_attributes(conn, window->id), NULL);
    bool viewable = reply && reply->map_state == XCB_MAP_STATE_VIEWABLE;
    free(reply);
    return viewable;
}

bool xwindow_has_net_state(struct xwindow *window, const char *state) {
    char name[128];
    snprintf(name, sizeof(name), "_NET_WM_STATE_%s", state);
    xcb_atom_t want = x11_atom(name);
    xcb_get_property_reply_t *reply =
        get_property(window->id, "_NET_WM_STATE", XCB_ATOM_ATOM);
    bool found = false;
    if (reply && reply->format == 32) {
        int n = xcb_get_property_value_length(reply) / 4;
        xcb_atom_t *list = xcb_get_property_value(reply);
        for (int i = 0; i < n; i++) {
            if (list[i] == want) {
                found = true;
            }
        }
    }
    free(reply);
    return found;
}

xcb_window_t x11_active_window(void) {
    xcb_get_property_reply_t *reply =
        get_property(screen->root, "_NET_ACTIVE_WINDOW", XCB_ATOM_WINDOW);
    xcb_window_t window = XCB_NONE;
    if (reply && reply->format == 32 && xcb_get_property_value_length(reply) >= 4) {
        window = ((xcb_window_t *)xcb_get_property_value(reply))[0];
    }
    free(reply);
    return window;
}

xcb_window_t x11_input_focus(void) {
    xcb_get_input_focus_reply_t *reply =
        xcb_get_input_focus_reply(conn, xcb_get_input_focus(conn), NULL);
    xcb_window_t focus = reply ? reply->focus : XCB_NONE;
    free(reply);
    return focus;
}

bool x11_client_list_contains(struct xwindow *window) {
    xcb_get_property_reply_t *reply =
        get_property(screen->root, "_NET_CLIENT_LIST", XCB_ATOM_WINDOW);
    bool found = false;
    if (reply && reply->format == 32) {
        int n = xcb_get_property_value_length(reply) / 4;
        xcb_window_t *list = xcb_get_property_value(reply);
        for (int i = 0; i < n; i++) {
            if (list[i] == window->id) {
                found = true;
            }
        }
    }
    free(reply);
    return found;
}

static char *atom_name(xcb_atom_t atom) {
    xcb_get_atom_name_reply_t *reply =
        xcb_get_atom_name_reply(conn, xcb_get_atom_name(conn, atom), NULL);
    char *name = reply ? strndup(xcb_get_atom_name_name(reply),
                                 xcb_get_atom_name_name_length(reply))
                       : strdup("?");
    free(reply);
    return name;
}

static void describe_atoms(FILE *out, xcb_window_t window, const char *name,
                           const char *strip) {
    xcb_get_property_reply_t *reply = get_property(window, name, XCB_ATOM_ATOM);
    if (reply && reply->format == 32) {
        int n = xcb_get_property_value_length(reply) / 4;
        xcb_atom_t *list = xcb_get_property_value(reply);
        for (int i = 0; i < n; i++) {
            char *text = atom_name(list[i]);
            size_t len = strlen(strip);
            fprintf(out, "%s%s", i ? "," : "",
                    strncmp(text, strip, len) == 0 ? text + len : text);
            free(text);
        }
    }
    free(reply);
}

static void describe_text(FILE *out, xcb_window_t window, const char *name) {
    xcb_get_property_reply_t *reply =
        get_property(window, name, XCB_GET_PROPERTY_TYPE_ANY);
    if (reply && reply->format == 8) {
        const char *text = xcb_get_property_value(reply);
        int len = xcb_get_property_value_length(reply);
        for (int i = 0; i < len; i++) {
            fputc(text[i] ? text[i] : '/', out);
        }
    }
    free(reply);
}

char *x11_describe_windows(void) {
    char *text = NULL;
    size_t size = 0;
    FILE *out = open_memstream(&text, &size);
    xcb_query_tree_reply_t *tree =
        xcb_query_tree_reply(conn, xcb_query_tree(conn, screen->root), NULL);
    int n = tree ? xcb_query_tree_children_length(tree) : 0;
    xcb_window_t *children = tree ? xcb_query_tree_children(tree) : NULL;
    fprintf(out, "top-level X windows, bottom to top:");
    int shown = 0;
    for (int i = 0; i < n; i++) {
        xcb_window_t id = children[i];
        xcb_get_window_attributes_reply_t *attrs = xcb_get_window_attributes_reply(
            conn, xcb_get_window_attributes(conn, id), NULL);
        xcb_get_geometry_reply_t *geo =
            xcb_get_geometry_reply(conn, xcb_get_geometry(conn, id), NULL);
        struct xwindow fake = {.id = id};
        int wm_state = xwindow_wm_state(&fake);
        if (attrs && geo &&
            (attrs->map_state != XCB_MAP_STATE_UNMAPPED || wm_state >= 0)) {
            static const char *map_states[] = {"unmapped", "unviewable",
                                               "viewable"};
            fprintf(out, "\n  0x%x %s%s %dx%d+%d+%d", id,
                    map_states[attrs->map_state % 3],
                    attrs->override_redirect ? " override-redirect" : "",
                    geo->width, geo->height, geo->x, geo->y);
            if (wm_state >= 0) {
                fprintf(out, " WM_STATE=%s",
                        wm_state == 0       ? "Withdrawn"
                            : wm_state == 1 ? "Normal"
                            : wm_state == 3 ? "Iconic"
                                            : "?");
            }
            fprintf(out, " state=[");
            describe_atoms(out, id, "_NET_WM_STATE", "_NET_WM_STATE_");
            fprintf(out, "] type=[");
            describe_atoms(out, id, "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_");
            xcb_get_property_reply_t *transient =
                get_property(id, "WM_TRANSIENT_FOR", XCB_ATOM_WINDOW);
            if (transient && transient->format == 32 &&
                xcb_get_property_value_length(transient) >= 4) {
                fprintf(out, "] transient-for=0x%x",
                        *(xcb_window_t *)xcb_get_property_value(transient));
            } else {
                fprintf(out, "]");
            }
            free(transient);
            xcb_get_property_reply_t *hints = get_property(
                id, "WM_NORMAL_HINTS", XCB_GET_PROPERTY_TYPE_ANY);
            if (hints && hints->format == 32 &&
                xcb_get_property_value_length(hints) >= 9 * 4) {
                uint32_t *v = xcb_get_property_value(hints);
                if (v[0] & HINT_P_MIN_SIZE) {
                    fprintf(out, " min=%ux%u", v[5], v[6]);
                }
                if (v[0] & HINT_P_MAX_SIZE) {
                    fprintf(out, " max=%ux%u", v[7], v[8]);
                }
                if ((v[0] & HINT_P_ASPECT) &&
                    xcb_get_property_value_length(hints) >= 15 * 4) {
                    fprintf(out, " aspect=%u:%u-%u:%u", v[11], v[12], v[13],
                            v[14]);
                }
            }
            free(hints);
            fprintf(out, " class=");
            describe_text(out, id, "WM_CLASS");
            fprintf(out, " name=");
            describe_text(out, id, "WM_NAME");
            shown++;
        }
        free(attrs);
        free(geo);
    }
    if (!shown) {
        fprintf(out, " none");
    }
    free(tree);
    fclose(out);
    return text;
}

xcb_window_t x11_find_toplevel(const char *name, struct box *box) {
    xcb_query_tree_reply_t *tree =
        xcb_query_tree_reply(conn, xcb_query_tree(conn, screen->root), NULL);
    int n = tree ? xcb_query_tree_children_length(tree) : 0;
    xcb_window_t *children = tree ? xcb_query_tree_children(tree) : NULL;
    xcb_window_t found = XCB_NONE;
    for (int i = n - 1; i >= 0 && !found; i--) {
        xcb_get_window_attributes_reply_t *attrs = xcb_get_window_attributes_reply(
            conn, xcb_get_window_attributes(conn, children[i]), NULL);
        bool viewable = attrs && attrs->map_state == XCB_MAP_STATE_VIEWABLE;
        free(attrs);
        if (!viewable) {
            continue;
        }
        xcb_get_property_reply_t *reply =
            get_property(children[i], "WM_NAME", XCB_GET_PROPERTY_TYPE_ANY);
        if (reply && reply->format == 8) {
            char *text = strndup(xcb_get_property_value(reply),
                                 xcb_get_property_value_length(reply));
            if (strcasestr(text, name)) {
                xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(
                    conn, xcb_get_geometry(conn, children[i]), NULL);
                if (geo) {
                    *box = BOX(geo->x, geo->y, geo->width, geo->height);
                    found = children[i];
                }
                free(geo);
            }
            free(text);
        }
        free(reply);
    }
    free(tree);
    return found;
}
