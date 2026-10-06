/* © 2026 dancingmirrors */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

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

#define FONT_NAME "Poison 12"

#define MAX_LINES 1024
#define LINE_CAP 512

#define PAD 24
#define LINE_GAP 3

struct log_line {
    char cls;
    char text[LINE_CAP];
};

struct console_state {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_buffer *buffer;
    struct xkb_context *xkb_context;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;

    struct log_line lines[MAX_LINES];
    int line_count;
    int line_head;

    char inbuf[8192];
    size_t inlen;

    int width;
    int height;
    bool running;
    bool configured;
    bool dirty;
    void *shm_data;
    size_t shm_size;
    int buf_w, buf_h, buf_stride;
};

static void render(struct console_state *state);

static void push_line(struct console_state *state, char cls, const char *text) {
    struct log_line *slot = &state->lines[state->line_head];
    slot->cls = cls;
    size_t len = strlen(text);
    if (len > LINE_CAP - 1) {
        len = LINE_CAP - 1;
    }
    memcpy(slot->text, text, len);
    slot->text[len] = '\0';

    state->line_head = (state->line_head + 1) % MAX_LINES;
    if (state->line_count < MAX_LINES) {
        state->line_count++;
    }
    state->dirty = true;
}

static void ingest_record(struct console_state *state, char *record) {
    if (record[0] == '\0') {
        return;
    }
    char cls = 'I';
    char *text = record;
    if ((record[0] == 'E' || record[0] == 'W' || record[0] == 'I') &&
        record[1] == ' ') {
        cls = record[0];
        text = record + 2;
    }
    push_line(state, cls, text);
}

static bool drain_stdin(struct console_state *state) {
    for (;;) {
        if (state->inlen >= sizeof(state->inbuf) - 1) {
            state->inbuf[state->inlen] = '\0';
            ingest_record(state, state->inbuf);
            state->inlen = 0;
        }

        ssize_t n = read(STDIN_FILENO, state->inbuf + state->inlen,
                         sizeof(state->inbuf) - 1 - state->inlen);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }

        state->inlen += (size_t)n;
        state->inbuf[state->inlen] = '\0';

        char *start = state->inbuf;
        char *nl;
        while ((nl = memchr(start, '\n', state->inbuf + state->inlen - start))) {
            *nl = '\0';
            ingest_record(state, start);
            start = nl + 1;
        }

        size_t remaining = state->inbuf + state->inlen - start;
        if (remaining > 0 && start != state->inbuf) {
            memmove(state->inbuf, start, remaining);
        }
        state->inlen = remaining;

        if ((size_t)n < sizeof(state->inbuf) - 1) {
            return true;
        }
    }
}

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                            uint32_t format, int fd, uint32_t size) {
    (void)keyboard;
    struct console_state *state = data;

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
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard,
                         uint32_t serial, uint32_t time, uint32_t key,
                         uint32_t state_w) {
    (void)keyboard;
    (void)serial;
    (void)time;

    struct console_state *state = data;

    if (state_w != WL_KEYBOARD_KEY_STATE_PRESSED) {
        return;
    }
    if (!state->xkb_state) {
        return;
    }

    xkb_keysym_t sym = xkb_state_key_get_one_sym(state->xkb_state, key + 8);

    if (sym == XKB_KEY_Escape || sym == XKB_KEY_q || sym == XKB_KEY_Q) {
        state->running = false;
    }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
                               uint32_t serial, uint32_t mods_depressed,
                               uint32_t mods_latched, uint32_t mods_locked,
                               uint32_t group) {
    (void)keyboard;
    (void)serial;

    struct console_state *state = data;
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

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    struct console_state *state = data;

    if (caps & WL_SEAT_CAPABILITY_KEYBOARD) {
        if (!state->keyboard) {
            state->keyboard = wl_seat_get_keyboard(seat);
            wl_keyboard_add_listener(state->keyboard, &keyboard_listener, state);
        }
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
                                    uint32_t serial, uint32_t width,
                                    uint32_t height) {
    struct console_state *state = data;
    state->width = width;
    state->height = height;
    state->configured = true;
    state->dirty = true;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
}

static void layer_surface_closed(void *data,
                                 struct zwlr_layer_surface_v1 *surface) {
    (void)surface;
    struct console_state *state = data;
    state->running = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version) {
    (void)version;
    struct console_state *state = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state->compositor = wl_registry_bind(registry, name,
                                             &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        state->layer_shell = wl_registry_bind(registry, name,
                                              &zwlr_layer_shell_v1_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        state->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        wl_seat_add_listener(state->seat, &seat_listener, state);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name) {
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
    fd = syscall(SYS_memfd_create, "poison-console", MFD_CLOEXEC);
    if (fd >= 0) {
        if (ftruncate(fd, size) < 0) {
            close(fd);
            return -1;
        }
        return fd;
    }
#elif defined(__FreeBSD__) || defined(__NetBSD__)
    fd = memfd_create("poison-console", MFD_CLOEXEC);
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
    snprintf(name, sizeof(name), "/poison-console-%d-%lld-%lld",
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

static void render(struct console_state *state) {
    if (state->width <= 0 || state->height <= 0) {
        return;
    }

    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32,
                                               state->width);
    size_t size = (size_t)stride * state->height;

    if (!state->buffer || state->shm_size < size ||
        state->buf_w != state->width || state->buf_h != state->height ||
        state->buf_stride != stride) {
        if (state->shm_data) {
            munmap(state->shm_data, state->shm_size);
            state->shm_data = NULL;
        }
        if (state->buffer) {
            wl_buffer_destroy(state->buffer);
            state->buffer = NULL;
        }

        int fd = create_shm_file(size);
        if (fd < 0) {
            fprintf(stderr, "poison-console: failed to create shm file!\n");
            return;
        }

        state->shm_data = mmap(NULL, size, PROT_READ | PROT_WRITE,
                               MAP_SHARED, fd, 0);
        if (state->shm_data == MAP_FAILED) {
            close(fd);
            state->shm_data = NULL;
            fprintf(stderr, "poison-console: mmap failed!\n");
            return;
        }
        state->shm_size = size;

        struct wl_shm_pool *pool = wl_shm_create_pool(state->shm, fd, size);
        state->buffer = wl_shm_pool_create_buffer(pool, 0, state->width,
                                                  state->height, stride,
                                                  WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);

        state->buf_w = state->width;
        state->buf_h = state->height;
        state->buf_stride = stride;
    }

    cairo_surface_t *cairo_surface = cairo_image_surface_create_for_data(
        state->shm_data, CAIRO_FORMAT_ARGB32, state->width, state->height,
        stride);
    cairo_t *cr = cairo_create(cairo_surface);

    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    PangoFontDescription *desc = pango_font_description_from_string(FONT_NAME);

    int body_x = PAD;
    int body_top = PAD;
    int body_bottom = state->height - PAD;
    int body_w = state->width - 2 * PAD;

    if (state->line_count == 0) {
        PangoLayout *empty = pango_cairo_create_layout(cr);
        pango_layout_set_font_description(empty, desc);
        pango_layout_set_text(empty, "(no messages yet)", -1);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0);
        cairo_move_to(cr, body_x, body_top);
        pango_cairo_show_layout(cr, empty);
        g_object_unref(empty);
    }

    double y = body_bottom;
    for (int i = 0; i < state->line_count && y > body_top; i++) {
        int idx = (state->line_head - 1 - i + MAX_LINES * 2) % MAX_LINES;
        struct log_line *line = &state->lines[idx];

        PangoLayout *layout = pango_cairo_create_layout(cr);
        pango_layout_set_font_description(layout, desc);
        pango_layout_set_width(layout, body_w * PANGO_SCALE);
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_text(layout, line->text, -1);

        PangoRectangle rect;
        pango_layout_get_extents(layout, NULL, &rect);
        int h = rect.height / PANGO_SCALE;

        y -= h;
        if (y < body_top) {
            g_object_unref(layout);
            break;
        }

        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0);
        cairo_move_to(cr, body_x, y);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);

        y -= LINE_GAP;
    }

    pango_font_description_free(desc);
    cairo_destroy(cr);
    cairo_surface_destroy(cairo_surface);

    wl_surface_attach(state->surface, state->buffer, 0, 0);
    wl_surface_damage_buffer(state->surface, 0, 0, state->width, state->height);
    wl_surface_commit(state->surface);
    state->dirty = false;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    struct console_state state = {0};
    state.running = true;

    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }

    state.display = wl_display_connect(NULL);
    if (!state.display) {
        fprintf(stderr, "poison-console: failed to connect to display!\n");
        return 1;
    }

    state.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!state.xkb_context) {
        fprintf(stderr, "poison-console: failed to create XKB context!\n");
        wl_display_disconnect(state.display);
        return 1;
    }

    state.registry = wl_display_get_registry(state.display);
    wl_registry_add_listener(state.registry, &registry_listener, &state);
    wl_display_roundtrip(state.display);

    if (!state.compositor || !state.layer_shell || !state.shm) {
        fprintf(stderr, "poison-console: missing Wayland interfaces!\n");
        wl_display_disconnect(state.display);
        return 1;
    }

    state.surface = wl_compositor_create_surface(state.compositor);
    state.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        state.layer_shell, state.surface, NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "poison-console");

    zwlr_layer_surface_v1_set_size(state.layer_surface, 0, 0);
    zwlr_layer_surface_v1_set_anchor(state.layer_surface,
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(state.layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(state.layer_surface,
                                                     ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    zwlr_layer_surface_v1_add_listener(state.layer_surface,
                                       &layer_surface_listener, &state);

    wl_surface_commit(state.surface);
    wl_display_roundtrip(state.display);

    if (!drain_stdin(&state)) {
        state.running = false;
    }

    if (state.configured) {
        render(&state);
        wl_display_flush(state.display);
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

        struct pollfd pfds[2] = {
            {.fd = display_fd,
             .events = (short)(POLLIN | (need_flush ? POLLOUT : 0))},
            {.fd = STDIN_FILENO, .events = POLLIN},
        };

        int ret = poll(pfds, 2, -1);
        if (ret < 0) {
            wl_display_cancel_read(state.display);
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (pfds[0].revents & POLLIN) {
            if (wl_display_read_events(state.display) < 0) {
                break;
            }
        } else {
            wl_display_cancel_read(state.display);
        }

        if (wl_display_dispatch_pending(state.display) < 0) {
            break;
        }

        if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (!drain_stdin(&state)) {
                state.running = false;
            }
        }

        if (state.dirty && state.configured) {
            render(&state);
        }

        if (wl_display_flush(state.display) < 0 && errno != EAGAIN) {
            break;
        }
    }

    if (state.shm_data) {
        munmap(state.shm_data, state.shm_size);
    }
    if (state.buffer) {
        wl_buffer_destroy(state.buffer);
    }
    if (state.keyboard) {
        wl_keyboard_destroy(state.keyboard);
    }
    if (state.layer_surface) {
        zwlr_layer_surface_v1_destroy(state.layer_surface);
    }
    if (state.surface) {
        wl_surface_destroy(state.surface);
    }
    if (state.xkb_state) {
        xkb_state_unref(state.xkb_state);
    }
    if (state.xkb_keymap) {
        xkb_keymap_unref(state.xkb_keymap);
    }
    if (state.xkb_context) {
        xkb_context_unref(state.xkb_context);
    }
    wl_display_disconnect(state.display);
    return 0;
}
