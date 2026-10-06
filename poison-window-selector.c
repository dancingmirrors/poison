/* © 2026 dancingmirrors */

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include <cairo/cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <pango/pangocairo.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#ifndef BTN_LEFT
#define BTN_LEFT 0x110
#endif
#ifndef BTN_RIGHT
#define BTN_RIGHT 0x111
#endif

#if defined(__linux__)
#include <sys/syscall.h>
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#endif

#if defined(__FreeBSD__) || defined(__NetBSD__)
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#endif

#define MAX_WINDOWS 128
#define MAX_TITLE_LEN 256
#define FONT_NAME "Poison 12"
#define SELECTOR_MARGIN_TOP 20
#define SELECTOR_MARGIN_LEFT 20
#define SELECTOR_PADDING_X 10
#define LINE_HEIGHT 30

struct window_entry {
    char title[MAX_TITLE_LEN];
    int window_id;
};

struct row_box {
    int y;
    int height;
};

struct selector_state {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_buffer *buffer;
    struct xkb_context *xkb_context;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
    struct window_entry windows[MAX_WINDOWS];
    struct row_box rows[MAX_WINDOWS];

    int window_count;
    int rows_drawn;
    int selected_index;
    int width;
    int height;
    bool running;
    bool configured;
    bool cancelled;
    bool order_changed;
    double pointer_x;
    double pointer_y;
    bool pointer_known;
    void *shm_data;
    size_t shm_size;
};

static void render(struct selector_state *state);

static void move_selected_entry(struct selector_state *state, int delta) {
    int from = state->selected_index;
    int to = from + delta;

    if (from < 0 || from >= state->window_count ||
        to < 0 || to >= state->window_count) {
        return;
    }

    struct window_entry tmp = state->windows[from];
    state->windows[from] = state->windows[to];
    state->windows[to] = tmp;
    state->selected_index = to;
    state->order_changed = true;
    render(state);
}

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                            uint32_t format, int fd, uint32_t size) {
    struct selector_state *state = data;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }

    char *map_shm = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map_shm == MAP_FAILED) {
        close(fd);
        return;
    }

    struct xkb_keymap *keymap = xkb_keymap_new_from_string(
        state->xkb_context, map_shm, XKB_KEYMAP_FORMAT_TEXT_V1, 0);
    munmap(map_shm, size);
    close(fd);

    if (!keymap) {
        return;
    }

    struct xkb_state *xkb_state = xkb_state_new(keymap);
    if (!xkb_state) {
        xkb_keymap_unref(keymap);
        return;
    }

    if (state->xkb_keymap) {
        xkb_keymap_unref(state->xkb_keymap);
    }
    if (state->xkb_state) {
        xkb_state_unref(state->xkb_state);
    }

    state->xkb_keymap = keymap;
    state->xkb_state = xkb_state;
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface,
                           struct wl_array *keys) {
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
    (void)keys;
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface) {
    struct selector_state *state = data;
    (void)keyboard;
    (void)serial;
    (void)surface;
    state->cancelled = true;
    state->running = false;
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard,
                         uint32_t serial, uint32_t time, uint32_t key,
                         uint32_t state_w) {
    (void)keyboard;
    (void)serial;
    (void)time;

    struct selector_state *state = data;

    if (state_w != WL_KEYBOARD_KEY_STATE_PRESSED) {
        return;
    }

    if (!state->xkb_state) {
        return;
    }

    xkb_keysym_t sym = xkb_state_key_get_one_sym(state->xkb_state, key + 8);
    bool shift = xkb_state_mod_name_is_active(state->xkb_state,
                                              XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0;

    if (sym == XKB_KEY_Escape) {
        state->cancelled = true;
        state->running = false;
    } else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        if (state->selected_index >= 0 && state->selected_index < state->window_count) {
            state->running = false;
        }
    } else if (sym == XKB_KEY_Up) {
        if (shift) {
            move_selected_entry(state, -1);
        } else if (state->selected_index > 0) {
            state->selected_index--;
            render(state);
        }
    } else if (sym == XKB_KEY_Down) {
        if (shift) {
            move_selected_entry(state, 1);
        } else if (state->selected_index < state->window_count - 1) {
            state->selected_index++;
            render(state);
        }
    }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
                               uint32_t serial, uint32_t mods_depressed,
                               uint32_t mods_latched, uint32_t mods_locked,
                               uint32_t group) {
    (void)keyboard;
    (void)serial;

    struct selector_state *state = data;
    if (state->xkb_state) {
        xkb_state_update_mask(state->xkb_state, mods_depressed, mods_latched,
                              mods_locked, 0, 0, group);
    }
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
                                 int32_t rate, int32_t delay) {
    (void)data;
    (void)keyboard;
    (void)rate;
    (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

static int row_at(struct selector_state *state, double x, double y) {
    if (state->rows_drawn <= 0) {
        return -1;
    }

    if (x < SELECTOR_PADDING_X || x >= state->width - SELECTOR_PADDING_X) {
        return -1;
    }

    for (int i = 0; i < state->rows_drawn; i++) {
        int top = state->rows[i].y;
        int bottom = (i + 1 < state->rows_drawn)
            ? state->rows[i + 1].y
            : state->rows[i].y + state->rows[i].height;
        if (y >= top && y < bottom) {
            return i;
        }
    }

    return -1;
}

static void pointer_enter(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t sx, wl_fixed_t sy) {
    (void)pointer;
    (void)serial;

    struct selector_state *state = data;

    if (surface != state->surface) {
        return;
    }

    state->pointer_x = wl_fixed_to_double(sx);
    state->pointer_y = wl_fixed_to_double(sy);
    state->pointer_known = true;
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface) {
    (void)pointer;
    (void)serial;

    struct selector_state *state = data;

    if (surface == state->surface) {
        state->pointer_known = false;
    }
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
                           uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {
    (void)pointer;
    (void)time;

    struct selector_state *state = data;

    state->pointer_x = wl_fixed_to_double(sx);
    state->pointer_y = wl_fixed_to_double(sy);
    state->pointer_known = true;

    int row = row_at(state, state->pointer_x, state->pointer_y);
    if (row >= 0 && row != state->selected_index) {
        state->selected_index = row;
        render(state);
    }
}

static void pointer_button(void *data, struct wl_pointer *pointer,
                           uint32_t serial, uint32_t time, uint32_t button,
                           uint32_t button_state) {
    (void)pointer;
    (void)serial;
    (void)time;

    struct selector_state *state = data;

    if (button_state != WL_POINTER_BUTTON_STATE_PRESSED) {
        return;
    }

    if (button == BTN_RIGHT) {
        state->cancelled = true;
        state->running = false;
        return;
    }

    if (button != BTN_LEFT || !state->pointer_known) {
        return;
    }

    int row = row_at(state, state->pointer_x, state->pointer_y);
    if (row >= 0) {
        state->selected_index = row;
        state->running = false;
    }
}

static void pointer_axis(void *data, struct wl_pointer *pointer,
                         uint32_t time, uint32_t axis, wl_fixed_t value) {
    (void)data;
    (void)pointer;
    (void)time;
    (void)axis;
    (void)value;
}

static void pointer_frame(void *data, struct wl_pointer *pointer) {
    (void)data;
    (void)pointer;
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer,
                                uint32_t axis_source) {
    (void)data;
    (void)pointer;
    (void)axis_source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer,
                              uint32_t time, uint32_t axis) {
    (void)data;
    (void)pointer;
    (void)time;
    (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer,
                                  uint32_t axis, int32_t discrete) {
    (void)data;
    (void)pointer;
    (void)axis;
    (void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    struct selector_state *state = data;

    if (caps & WL_SEAT_CAPABILITY_KEYBOARD) {
        if (!state->keyboard) {
            state->keyboard = wl_seat_get_keyboard(seat);
            wl_keyboard_add_listener(state->keyboard, &keyboard_listener, state);
        }
    }

    if (caps & WL_SEAT_CAPABILITY_POINTER) {
        if (!state->pointer) {
            state->pointer = wl_seat_get_pointer(seat);
            wl_pointer_add_listener(state->pointer, &pointer_listener, state);
        }
    } else if (state->pointer) {
        wl_pointer_release(state->pointer);
        state->pointer = NULL;
        state->pointer_known = false;
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
    (void)data;
    (void)seat;
    (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void layer_surface_configure(void *data,
                                    struct zwlr_layer_surface_v1 *surface,
                                    uint32_t serial, uint32_t width, uint32_t height) {
    struct selector_state *state = data;
    state->width = width;
    state->height = height;
    state->configured = true;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface) {
    (void)surface;
    struct selector_state *state = data;
    state->running = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface, uint32_t version) {
    struct selector_state *state = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        state->layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        state->seat = wl_registry_bind(registry, name, &wl_seat_interface, 5);
        wl_seat_add_listener(state->seat, &seat_listener, state);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static int create_shm_file(size_t size) {
    int fd = -1;

#if defined(__linux__) && defined(SYS_memfd_create)
    fd = syscall(SYS_memfd_create, "poison-window-selector", MFD_CLOEXEC);
    if (fd >= 0) {
        if (ftruncate(fd, size) < 0) {
            close(fd);
            return -1;
        }
        return fd;
    }
#elif defined(__FreeBSD__) || defined(__NetBSD__)
    fd = memfd_create("poison-window-selector", MFD_CLOEXEC);
    if (fd >= 0) {
        if (ftruncate(fd, size) < 0) {
            close(fd);
            return -1;
        }
        return fd;
    }
#endif

    char name[64];
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    snprintf(name, sizeof(name), "/poison-window-selector-%d-%lld-%lld",
             getpid(), (long long)ts.tv_sec, (long long)ts.tv_nsec);

    fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        return -1;
    }

    shm_unlink(name);

    if (ftruncate(fd, size) < 0) {
        close(fd);
        return -1;
    }

    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static void render(struct selector_state *state) {
    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, state->width);
    size_t size = stride * state->height;

    if (state->shm_size < size) {
        if (state->shm_data) {
            munmap(state->shm_data, state->shm_size);
            state->shm_data = NULL;
        }

        int fd = create_shm_file(size);
        if (fd < 0) {
            fprintf(stderr, "Failed to create shm file!\n");
            return;
        }

        state->shm_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (state->shm_data == MAP_FAILED) {
            close(fd);
            fprintf(stderr, "Failed to mmap!\n");
            return;
        }
        state->shm_size = size;

        struct wl_shm_pool *pool = wl_shm_create_pool(state->shm, fd, size);
        state->buffer = wl_shm_pool_create_buffer(pool, 0, state->width, state->height,
                                                  stride, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);
    }

    cairo_surface_t *cairo_surface = cairo_image_surface_create_for_data(
        state->shm_data, CAIRO_FORMAT_ARGB32, state->width, state->height, stride);
    cairo_t *cr = cairo_create(cairo_surface);

    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string(FONT_NAME);
    pango_font_description_set_weight(desc, PANGO_WEIGHT_NORMAL);
    pango_layout_set_font_description(layout, desc);

    int y_offset = SELECTOR_MARGIN_TOP;
    int line_height = LINE_HEIGHT;

    state->rows_drawn = 0;

    for (int i = 0; i < state->window_count; i++) {
        pango_layout_set_text(layout, state->windows[i].title, -1);

        PangoRectangle logical_rect;
        pango_layout_get_extents(layout, NULL, &logical_rect);
        int text_height = logical_rect.height / PANGO_SCALE;
        int text_y_offset = logical_rect.y / PANGO_SCALE;
        int box_height = line_height - 2;
        int rect_y = y_offset + text_y_offset - (box_height - text_height) / 2;

        state->rows[i].y = rect_y;
        state->rows[i].height = box_height;
        state->rows_drawn = i + 1;

        if (i == state->selected_index) {
            cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
            cairo_rectangle(cr, SELECTOR_PADDING_X, rect_y, state->width - 2 * SELECTOR_PADDING_X, box_height);
            cairo_fill(cr);
            cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
        }

        cairo_move_to(cr, SELECTOR_MARGIN_LEFT, y_offset);
        pango_cairo_show_layout(cr, layout);

        y_offset += line_height;

        if (y_offset > state->height) {
            break;
        }
    }

    pango_font_description_free(desc);
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(cairo_surface);

    wl_surface_attach(state->surface, state->buffer, 0, 0);
    wl_surface_damage_buffer(state->surface, 0, 0, state->width, state->height);
    wl_surface_commit(state->surface);
}

static int load_windows_from_stdin(struct selector_state *state) {
    char line[MAX_TITLE_LEN + 16];
    state->window_count = 0;

    while (fgets(line, sizeof(line), stdin) && state->window_count < MAX_WINDOWS) {
        int id;
        char *endptr;
        char *lineptr = line;

        while (*lineptr == ' ' || *lineptr == '\t') {
            lineptr++;
        }

        id = strtol(lineptr, &endptr, 10);
        if (endptr == lineptr || (*endptr != ' ' && *endptr != '\t')) {
            continue;
        }

        lineptr = endptr;
        while (*lineptr == ' ' || *lineptr == '\t') {
            lineptr++;
        }

        char *title_start = lineptr;
        char *newline = strchr(title_start, '\n');
        if (newline) {
            *newline = '\0';
        }

        state->windows[state->window_count].window_id = id;
        strncpy(state->windows[state->window_count].title, title_start, MAX_TITLE_LEN - 1);
        state->windows[state->window_count].title[MAX_TITLE_LEN - 1] = '\0';
        state->window_count++;
    }

    return state->window_count;
}

#define COMPOSITOR_TIMEOUT_MS 5000
#define SELECTOR_POLL_TIMEOUT_MS 60000

static void sync_done(void *data, struct wl_callback *cb, uint32_t serial) {
    (void)cb;
    (void)serial;
    *(bool *)data = true;
}

static const struct wl_callback_listener sync_listener = {
    .done = sync_done,
};

static int roundtrip_timeout(struct wl_display *display) {
    bool done = false;
    int fd = wl_display_get_fd(display);
    struct wl_callback *cb = wl_display_sync(display);
    if (!cb) {
        return -1;
    }
    wl_callback_add_listener(cb, &sync_listener, &done);

    while (!done) {
        if (wl_display_prepare_read(display) == -1) {
            if (wl_display_dispatch_pending(display) < 0) {
                goto fail;
            }
            continue;
        }
        if (wl_display_flush(display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(display);
            goto fail;
        }
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int ret = poll(&pfd, 1, COMPOSITOR_TIMEOUT_MS);
        if (ret <= 0) {
            wl_display_cancel_read(display);
            if (ret == 0) {
                fprintf(stderr, "Timed out waiting for compositor!\n");
            }
            goto fail;
        }
        if (wl_display_read_events(display) < 0) {
            goto fail;
        }
        if (wl_display_dispatch_pending(display) < 0) {
            goto fail;
        }
    }
    wl_callback_destroy(cb);
    return 0;
fail:
    wl_callback_destroy(cb);
    return -1;
}

int main(void) {
    struct selector_state state = {0};
    state.selected_index = 0;
    state.running = true;
    state.cancelled = false;

    if (load_windows_from_stdin(&state) == 0) {
        fprintf(stderr, "No windows to select from.\n");
        return 1;
    }

    state.display = wl_display_connect(NULL);
    if (!state.display) {
        fprintf(stderr, "Failed to connect to Wayland display!\n");
        return 1;
    }

    state.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!state.xkb_context) {
        fprintf(stderr, "Failed to create XKB context!\n");
        wl_display_disconnect(state.display);
        return 1;
    }

    state.registry = wl_display_get_registry(state.display);
    wl_registry_add_listener(state.registry, &registry_listener, &state);
    if (roundtrip_timeout(state.display) < 0) {
        fprintf(stderr, "Timed out or error during registry roundtrip!\n");
        wl_display_disconnect(state.display);
        xkb_context_unref(state.xkb_context);
        return 1;
    }

    if (!state.compositor || !state.layer_shell || !state.shm || !state.seat) {
        fprintf(stderr, "Missing required Wayland interfaces!\n");
        wl_display_disconnect(state.display);
        xkb_context_unref(state.xkb_context);
        return 1;
    }

    state.surface = wl_compositor_create_surface(state.compositor);
    state.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        state.layer_shell, state.surface, NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "window-selector");

    zwlr_layer_surface_v1_set_size(state.layer_surface, 0, 0);
    zwlr_layer_surface_v1_set_anchor(state.layer_surface,
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(state.layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(state.layer_surface,
                                                     ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    zwlr_layer_surface_v1_add_listener(state.layer_surface, &layer_surface_listener, &state);

    wl_surface_commit(state.surface);
    if (roundtrip_timeout(state.display) < 0) {
        fprintf(stderr, "Timed out or error waiting for layer surface configure!\n");
        wl_display_disconnect(state.display);
        xkb_context_unref(state.xkb_context);
        return 1;
    }

    if (state.configured) {
        render(&state);
    }

    int display_fd = wl_display_get_fd(state.display);
    while (state.running) {
        if (wl_display_prepare_read(state.display) == -1) {
            if (wl_display_dispatch_pending(state.display) < 0) {
                break;
            }
            continue;
        }
        int flush_ret = wl_display_flush(state.display);
        bool need_flush = (flush_ret < 0 && errno == EAGAIN);
        if (flush_ret < 0 && !need_flush) {
            wl_display_cancel_read(state.display);
            break;
        }
        struct pollfd pfd = {
            .fd = display_fd,
            .events = (short)(POLLIN | (need_flush ? POLLOUT : 0)),
        };
        int ret = poll(&pfd, 1, SELECTOR_POLL_TIMEOUT_MS);
        if (ret < 0) {
            wl_display_cancel_read(state.display);
            break;
        }
        if (ret == 0) {
            wl_display_cancel_read(state.display);
            state.cancelled = true;
            break;
        }
        if (pfd.revents & POLLIN) {
            if (wl_display_read_events(state.display) < 0) {
                break;
            }
        } else {
            wl_display_cancel_read(state.display);
        }
        if (wl_display_dispatch_pending(state.display) < 0) {
            break;
        }
    }

    if (!state.cancelled && state.selected_index >= 0 && state.selected_index < state.window_count) {
        if (state.order_changed) {
            printf("order");
            for (int i = 0; i < state.window_count; i++) {
                printf(" %d", state.windows[i].window_id);
            }
            printf("\n");
        }
        printf("%d\n", state.windows[state.selected_index].window_id);
    } else {
        printf("-1\n");
    }

    if (state.pointer) {
        wl_pointer_release(state.pointer);
    }
    if (state.shm_data) {
        munmap(state.shm_data, state.shm_size);
    }
    if (state.xkb_state) {
        xkb_state_unref(state.xkb_state);
    }
    if (state.xkb_keymap) {
        xkb_keymap_unref(state.xkb_keymap);
    }
    xkb_context_unref(state.xkb_context);
    wl_display_disconnect(state.display);

    return 0;
}
