/* © 2026 dancingmirrors */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <cairo/cairo.h>
#include <pango/pangocairo.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include <wayland-client.h>

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
#define DISPLAY_DURATION_MS 500
#define BOX_PADDING_X 20
#define BOX_MAX_WIDTH 150
#define BOX_HEIGHT 30

struct indicator_state {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_buffer *buffer;

    const char *title;
    int frame_x, frame_y, frame_w, frame_h;

    int width, height;
    bool running;
    bool configured;
    void *shm_data;
    size_t shm_size;

    int duration_ms;
    int max_box_width;
};

static void layer_surface_configure(void *data,
                                    struct zwlr_layer_surface_v1 *surface,
                                    uint32_t serial, uint32_t width,
                                    uint32_t height) {
    struct indicator_state *state = data;
    state->width = width;
    state->height = height;
    state->configured = true;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
}

static void layer_surface_closed(void *data,
                                 struct zwlr_layer_surface_v1 *surface) {
    (void)surface;
    struct indicator_state *state = data;
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
    struct indicator_state *state = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state->compositor = wl_registry_bind(registry, name,
                                             &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        state->layer_shell = wl_registry_bind(registry, name,
                                              &zwlr_layer_shell_v1_interface,
                                              1);
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
    fd = syscall(SYS_memfd_create, "poison-indicator", MFD_CLOEXEC);
    if (fd >= 0) {
        if (ftruncate(fd, size) < 0) {
            close(fd);
            return -1;
        }
        return fd;
    }
#elif defined(__FreeBSD__) || defined(__NetBSD__)
    fd = memfd_create("poison-indicator", MFD_CLOEXEC);
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
    snprintf(name, sizeof(name), "/poison-indicator-%d-%lld-%lld",
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

static void render(struct indicator_state *state) {
    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32,
                                               state->width);
    size_t size = (size_t)stride * state->height;

    if (state->shm_size < size) {
        if (state->shm_data) {
            munmap(state->shm_data, state->shm_size);
            state->shm_data = NULL;
        }

        int fd = create_shm_file(size);
        if (fd < 0) {
            fprintf(stderr, "poison-indicator: failed to create shm file!\n");
            return;
        }

        state->shm_data = mmap(NULL, size, PROT_READ | PROT_WRITE,
                               MAP_SHARED, fd, 0);
        if (state->shm_data == MAP_FAILED) {
            close(fd);
            fprintf(stderr, "poison-indicator: mmap failed!\n");
            return;
        }
        state->shm_size = size;

        struct wl_shm_pool *pool = wl_shm_create_pool(state->shm, fd, size);
        state->buffer = wl_shm_pool_create_buffer(pool, 0, state->width,
                                                  state->height, stride,
                                                  WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);
    }

    cairo_surface_t *cairo_surface = cairo_image_surface_create_for_data(
        state->shm_data, CAIRO_FORMAT_ARGB32, state->width, state->height,
        stride);
    cairo_t *cr = cairo_create(cairo_surface);

    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
    cairo_paint(cr);

    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string(FONT_NAME);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_width(layout,
                           (state->max_box_width - 2 * BOX_PADDING_X) * PANGO_SCALE);

    const char *display_title = (state->title && state->title[0] != '\0')
        ? state->title
        : "(empty)";
    pango_layout_set_text(layout, display_title, -1);

    PangoRectangle logical_rect;
    pango_layout_get_extents(layout, NULL, &logical_rect);
    int text_h = logical_rect.height / PANGO_SCALE;

    int box_w = state->max_box_width;
    int box_h = BOX_HEIGHT;

    int cx = state->frame_x + state->frame_w / 2;
    int cy = state->frame_y + state->frame_h / 2;
    int box_x = cx - box_w / 2;
    int box_y = cy - box_h / 2;

    if (box_x < 0) {
        box_x = 0;
    }
    if (box_y < 0) {
        box_y = 0;
    }
    if (box_x + box_w > state->width) {
        box_x = state->width - box_w;
    }
    if (box_y + box_h > state->height) {
        box_y = state->height - box_h;
    }
    if (box_x < 0) {
        box_x = 0;
    }
    if (box_y < 0) {
        box_y = 0;
    }

    double r = 5.0;
    double bx = box_x, by = box_y, bw = box_w, bh = box_h;
    cairo_new_path(cr);
    cairo_move_to(cr, bx + r, by);
    cairo_line_to(cr, bx + bw - r, by);
    cairo_arc(cr, bx + bw - r, by + r, r, -M_PI / 2.0, 0);
    cairo_line_to(cr, bx + bw, by + bh - r);
    cairo_arc(cr, bx + bw - r, by + bh - r, r, 0, M_PI / 2.0);
    cairo_line_to(cr, bx + r, by + bh);
    cairo_arc(cr, bx + r, by + bh - r, r, M_PI / 2.0, M_PI);
    cairo_line_to(cr, bx, by + r);
    cairo_arc(cr, bx + r, by + r, r, M_PI, 3.0 * M_PI / 2.0);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0);
    int text_x = box_x + BOX_PADDING_X;
    int text_y = box_y + (box_h - text_h) / 2;
    cairo_move_to(cr, text_x, text_y);
    pango_cairo_show_layout(cr, layout);

    pango_font_description_free(desc);
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(cairo_surface);

    wl_surface_attach(state->surface, state->buffer, 0, 0);
    wl_surface_damage_buffer(state->surface, 0, 0, state->width, state->height);
    wl_surface_commit(state->surface);
}

int main(int argc, char *argv[]) {
    if (argc < 6) {
        fprintf(stderr,
                "Usage: poison-indicator <title> <x> <y> <w> <h> "
                "[duration_ms] [max_width]\n");
        return 1;
    }

    struct indicator_state state = {0};
    state.title = argv[1];

    char *end;
    errno = 0;
    state.frame_x = (int)strtol(argv[2], &end, 10);
    if (errno || end == argv[2]) {
        fprintf(stderr, "poison-indicator: invalid x!\n");
        return 1;
    }
    errno = 0;
    state.frame_y = (int)strtol(argv[3], &end, 10);
    if (errno || end == argv[3]) {
        fprintf(stderr, "poison-indicator: invalid y!\n");
        return 1;
    }
    errno = 0;
    state.frame_w = (int)strtol(argv[4], &end, 10);
    if (errno || end == argv[4]) {
        fprintf(stderr, "poison-indicator: invalid w!\n");
        return 1;
    }
    errno = 0;
    state.frame_h = (int)strtol(argv[5], &end, 10);
    if (errno || end == argv[5]) {
        fprintf(stderr, "poison-indicator: invalid h!\n");
        return 1;
    }

    state.duration_ms = DISPLAY_DURATION_MS;
    state.max_box_width = BOX_MAX_WIDTH;
    if (argc >= 7) {
        errno = 0;
        int d = (int)strtol(argv[6], &end, 10);
        if (!errno && end != argv[6] && d > 0) {
            state.duration_ms = d;
        }
    }
    if (argc >= 8) {
        errno = 0;
        int w = (int)strtol(argv[7], &end, 10);
        if (!errno && end != argv[7] && w > 2 * BOX_PADDING_X) {
            state.max_box_width = w;
        }
    }

    state.running = true;

    state.display = wl_display_connect(NULL);
    if (!state.display) {
        fprintf(stderr, "poison-indicator: failed to connect to display!\n");
        return 1;
    }

    state.registry = wl_display_get_registry(state.display);
    wl_registry_add_listener(state.registry, &registry_listener, &state);
    wl_display_roundtrip(state.display);

    if (!state.compositor || !state.layer_shell || !state.shm) {
        fprintf(stderr, "poison-indicator: missing Wayland interfaces!\n");
        wl_display_disconnect(state.display);
        return 1;
    }

    state.surface = wl_compositor_create_surface(state.compositor);
    state.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        state.layer_shell, state.surface, NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "poison-indicator");

    zwlr_layer_surface_v1_set_size(state.layer_surface, 0, 0);
    zwlr_layer_surface_v1_set_anchor(state.layer_surface,
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(state.layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(state.layer_surface,
                                                     ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    zwlr_layer_surface_v1_add_listener(state.layer_surface,
                                       &layer_surface_listener, &state);

    wl_surface_commit(state.surface);
    wl_display_roundtrip(state.display);

    if (state.configured) {
        render(&state);
        wl_display_flush(state.display);
    }

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (state.running) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000L +
            (now.tv_nsec - start.tv_nsec) / 1000000L;
        if (elapsed >= state.duration_ms) {
            break;
        }

        if (wl_display_flush(state.display) < 0 && errno != EAGAIN) {
            break;
        }

        long remaining = state.duration_ms - elapsed;
        struct pollfd pfd = {
            .fd = wl_display_get_fd(state.display),
            .events = POLLIN,
        };
        int ret = poll(&pfd, 1, (int)remaining);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (ret > 0 && (pfd.revents & POLLIN)) {
            if (wl_display_dispatch(state.display) < 0) {
                break;
            }
        }
    }

    if (state.shm_data) {
        munmap(state.shm_data, state.shm_size);
    }
    wl_display_disconnect(state.display);
    return 0;
}
