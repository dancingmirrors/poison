#define _GNU_SOURCE

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "poison-test.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

struct client {
    struct wl_list link;
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct zxdg_decoration_manager_v1 *decoration_manager;
    struct wl_output *output;
    struct ext_output_image_capture_source_manager_v1 *source_manager;
    struct ext_image_copy_capture_manager_v1 *capture_manager;
    bool reported_error;
};

static struct wl_list clients = {&clients, &clients};

static unsigned long activity;

int x11_fd(void);
void x11_dispatch(void);

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base,
                         uint32_t serial) {
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static uint32_t min_u32(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version) {
    struct client *client = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        client->compositor = wl_registry_bind(registry, name,
                                              &wl_compositor_interface,
                                              min_u32(version, 4));
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        client->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        client->wm_base = wl_registry_bind(registry, name,
                                           &xdg_wm_base_interface,
                                           min_u32(version, 6));
        xdg_wm_base_add_listener(client->wm_base, &wm_base_listener, client);
    } else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
        client->decoration_manager =
            wl_registry_bind(registry, name,
                             &zxdg_decoration_manager_v1_interface, 1);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        if (!client->output) {
            client->output = wl_registry_bind(registry, name,
                                              &wl_output_interface, 1);
        }
    } else if (strcmp(interface,
                      ext_output_image_capture_source_manager_v1_interface.name) == 0) {
        client->source_manager = wl_registry_bind(
            registry, name, &ext_output_image_capture_source_manager_v1_interface, 1);
    } else if (strcmp(interface,
                      ext_image_copy_capture_manager_v1_interface.name) == 0) {
        client->capture_manager = wl_registry_bind(
            registry, name, &ext_image_copy_capture_manager_v1_interface, 1);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void check_display_error(struct client *client) {
    if (client->reported_error) {
        return;
    }
    int err = wl_display_get_error(client->display);
    if (err == 0) {
        return;
    }
    client->reported_error = true;
    const struct wl_interface *interface = NULL;
    uint32_t id = 0;
    uint32_t code = wl_display_get_protocol_error(client->display, &interface, &id);
    if (err == EPROTO) {
        test_fail_at(__FILE__, __LINE__,
                     "the compositor sent a protocol error: %s@%u code %u",
                     interface ? interface->name : "?", id, code);
    } else {
        test_fail_at(__FILE__, __LINE__,
                     "lost the Wayland connection: %s%s", strerror(err),
                     test_compositor_alive() ? "" : " (the compositor is gone)");
    }
    test_abort();
}

struct client *client_connect(void) {
    struct client *client = calloc(1, sizeof(*client));
    client->display = wl_display_connect(NULL);
    REQUIRE(client->display, "cannot connect to the compositor");
    client->registry = wl_display_get_registry(client->display);
    wl_registry_add_listener(client->registry, &registry_listener, client);
    wl_display_roundtrip(client->display);
    wl_display_roundtrip(client->display);
    REQUIRE(client->compositor && client->shm && client->wm_base,
            "the compositor lacks wl_compositor, wl_shm or xdg_wm_base");
    wl_list_insert(clients.prev, &client->link);
    return client;
}

void client_destroy(struct client *client) {
    wl_list_remove(&client->link);
    wl_display_disconnect(client->display);
    free(client);
}

struct wl_display *client_display(struct client *client) {
    return client->display;
}

void client_roundtrip(struct client *client) {
    if (wl_display_roundtrip(client->display) < 0) {
        check_display_error(client);
    }
}

struct buffer {
    struct wl_buffer *wl_buffer;
    void *data;
    size_t size;
};

static void buffer_release(void *data, struct wl_buffer *wl_buffer) {
    struct buffer *buffer = data;
    wl_buffer_destroy(buffer->wl_buffer);
    munmap(buffer->data, buffer->size);
    free(buffer);
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static struct wl_buffer *create_shm_buffer(struct wl_shm *shm, int width,
                                           int height, uint32_t format,
                                           void **data_out) {
    int stride = width * 4;
    size_t size = (size_t)stride * height;
    int fd = memfd_create("poison-test", MFD_CLOEXEC);
    REQUIRE(fd >= 0, "memfd_create failed: %s", strerror(errno));
    REQUIRE(ftruncate(fd, (off_t)size) == 0, "ftruncate failed");
    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    REQUIRE(data != MAP_FAILED, "mmap failed");
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
    struct wl_buffer *wl_buffer =
        wl_shm_pool_create_buffer(pool, 0, width, height, stride, format);
    wl_shm_pool_destroy(pool);
    close(fd);
    *data_out = data;
    return wl_buffer;
}

static struct wl_buffer *window_buffer(struct client *client, int width,
                                       int height, int shadow, uint32_t color) {
    int full_width = width + 2 * shadow;
    int full_height = height + 2 * shadow;
    struct buffer *buffer = calloc(1, sizeof(*buffer));
    buffer->size = (size_t)full_width * full_height * 4;
    buffer->wl_buffer = create_shm_buffer(client->shm, full_width, full_height,
                                          WL_SHM_FORMAT_ARGB8888, &buffer->data);
    uint32_t *pixels = buffer->data;
    for (int y = 0; y < full_height; y++) {
        for (int x = 0; x < full_width; x++) {
            bool inside = x >= shadow && x < shadow + width && y >= shadow &&
                y < shadow + height;
            pixels[y * full_width + x] =
                0xff000000u | (inside ? color : C_SHADOW);
        }
    }
    wl_buffer_add_listener(buffer->wl_buffer, &buffer_listener, buffer);
    return buffer->wl_buffer;
}

static void window_pick_size(struct window *window, int *width_out,
                             int *height_out) {
    const struct window_opts *o = &window->opts;
    int preferred_width = o->width > 0 ? o->width : 400;
    int preferred_height = o->height > 0 ? o->height : 300;
    int box_width = window->configured_width > 0 ? window->configured_width
                                                 : preferred_width;
    int box_height = window->configured_height > 0 ? window->configured_height
                                                   : preferred_height;
    int width = preferred_width, height = preferred_height;

    switch (o->policy) {
    case SIZE_OBEY:
        width = box_width;
        height = box_height;
        break;
    case SIZE_FIXED:
        break;
    case SIZE_ASPECT:
        if ((int64_t)box_width * preferred_height <=
            (int64_t)box_height * preferred_width) {
            width = box_width;
            height = (int)((int64_t)box_width * preferred_height / preferred_width);
        } else {
            height = box_height;
            width = (int)((int64_t)box_height * preferred_width / preferred_height);
        }
        break;
    case SIZE_SNAP:
        width = box_width / o->snap_width * o->snap_width;
        height = box_height / o->snap_height * o->snap_height;
        break;
    }

    if (o->policy != SIZE_FIXED) {
        if (o->max_width > 0 && width > o->max_width) {
            width = o->max_width;
        }
        if (o->max_height > 0 && height > o->max_height) {
            height = o->max_height;
        }
        if (o->min_width > 0 && width < o->min_width) {
            width = o->min_width;
        }
        if (o->min_height > 0 && height < o->min_height) {
            height = o->min_height;
        }
    }
    if (width < 1) {
        width = 1;
    }
    if (height < 1) {
        height = 1;
    }
    *width_out = width;
    *height_out = height;
}

static void window_draw(struct window *window) {
    int width, height;
    window_pick_size(window, &width, &height);
    int shadow = window->opts.shadow;
    struct wl_buffer *buffer = window_buffer(window->client, width, height,
                                             shadow, window->opts.color);
    wl_surface_attach(window->surface, buffer, 0, 0);
    wl_surface_damage_buffer(window->surface, 0, 0, INT32_MAX, INT32_MAX);
    if (shadow > 0) {
        xdg_surface_set_window_geometry(window->xdg_surface, shadow, shadow,
                                        width, height);
    }
    wl_surface_commit(window->surface);
    window->width = width;
    window->height = height;
    window->committed = true;
}

void window_redraw(struct window *window) {
    window_draw(window);
    settle();
}

void window_commit_size(struct window *window, int width, int height) {
    struct window_opts saved = window->opts;
    window->opts.policy = SIZE_FIXED;
    window->opts.width = width;
    window->opts.height = height;
    window_draw(window);
    window->opts = saved;
    settle();
}

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
                                  uint32_t serial) {
    struct window *window = data;
    window->configured_width = window->pending_width;
    window->configured_height = window->pending_height;
    window->states = window->pending_states;
    window->configures++;
    activity++;
    xdg_surface_ack_configure(xdg_surface, serial);
    if (window->hidden) {
        return;
    }
    window_draw(window);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                               int32_t width, int32_t height,
                               struct wl_array *states) {
    struct window *window = data;
    window->pending_width = width;
    window->pending_height = height;
    window->pending_states = 0;
    uint32_t *state;
    wl_array_for_each(state, states) {
        switch (*state) {
        case XDG_TOPLEVEL_STATE_MAXIMIZED:
            window->pending_states |= STATE_MAXIMIZED;
            break;
        case XDG_TOPLEVEL_STATE_FULLSCREEN:
            window->pending_states |= STATE_FULLSCREEN;
            break;
        case XDG_TOPLEVEL_STATE_RESIZING:
            window->pending_states |= STATE_RESIZING;
            break;
        case XDG_TOPLEVEL_STATE_ACTIVATED:
            window->pending_states |= STATE_ACTIVATED;
            break;
        case XDG_TOPLEVEL_STATE_TILED_LEFT:
            window->pending_states |= STATE_TILED_LEFT;
            break;
        case XDG_TOPLEVEL_STATE_TILED_RIGHT:
            window->pending_states |= STATE_TILED_RIGHT;
            break;
        case XDG_TOPLEVEL_STATE_TILED_TOP:
            window->pending_states |= STATE_TILED_TOP;
            break;
        case XDG_TOPLEVEL_STATE_TILED_BOTTOM:
            window->pending_states |= STATE_TILED_BOTTOM;
            break;
        case XDG_TOPLEVEL_STATE_SUSPENDED:
            window->pending_states |= STATE_SUSPENDED;
            break;
        }
    }
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    struct window *window = data;
    window->close_requested = true;
    activity++;
}

static void toplevel_configure_bounds(void *data, struct xdg_toplevel *toplevel,
                                      int32_t width, int32_t height) {
    struct window *window = data;
    window->bounds_width = width;
    window->bounds_height = height;
}

static void toplevel_wm_capabilities(void *data, struct xdg_toplevel *toplevel,
                                     struct wl_array *capabilities) {
    struct window *window = data;
    window->wm_capabilities = 0;
    uint32_t *cap;
    wl_array_for_each(cap, capabilities) {
        window->wm_capabilities |= 1u << *cap;
    }
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_configure_bounds,
    .wm_capabilities = toplevel_wm_capabilities,
};

static void decoration_configure(void *data,
                                 struct zxdg_toplevel_decoration_v1 *decoration,
                                 uint32_t mode) {
    struct window *window = data;
    window->decoration_mode = (int)mode;
    activity++;
}

static const struct zxdg_toplevel_decoration_v1_listener decoration_listener = {
    .configure = decoration_configure,
};

static void window_initial_commit(struct window *window) {
    window->committed = false;
    window->hidden = false;
    wl_surface_commit(window->surface);
}

struct window *window_create(struct client *client,
                             const struct window_opts *opts) {
    struct window *window = calloc(1, sizeof(*window));
    window->client = client;
    window->opts = *opts;
    if (window->opts.color == 0) {
        window->opts.color = C_GRAY;
    }

    window->surface = wl_compositor_create_surface(client->compositor);
    window->xdg_surface = xdg_wm_base_get_xdg_surface(client->wm_base,
                                                      window->surface);
    xdg_surface_add_listener(window->xdg_surface, &xdg_surface_listener, window);
    window->toplevel = xdg_surface_get_toplevel(window->xdg_surface);
    xdg_toplevel_add_listener(window->toplevel, &toplevel_listener, window);

    if (opts->title) {
        xdg_toplevel_set_title(window->toplevel, opts->title);
    }
    if (opts->app_id) {
        xdg_toplevel_set_app_id(window->toplevel, opts->app_id);
    }
    if (opts->parent) {
        xdg_toplevel_set_parent(window->toplevel, opts->parent->toplevel);
    }
    if (opts->min_width > 0 || opts->min_height > 0) {
        xdg_toplevel_set_min_size(window->toplevel, opts->min_width,
                                  opts->min_height);
    }
    if (opts->max_width > 0 || opts->max_height > 0) {
        xdg_toplevel_set_max_size(window->toplevel, opts->max_width,
                                  opts->max_height);
    }
    if (opts->fullscreen) {
        xdg_toplevel_set_fullscreen(window->toplevel, NULL);
    }
    if (opts->maximized) {
        xdg_toplevel_set_maximized(window->toplevel);
    }

    if (opts->decoration != DECORATION_NONE) {
        REQUIRE(client->decoration_manager,
                "the compositor does not offer xdg-decoration");
        window->decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
            client->decoration_manager, window->toplevel);
        zxdg_toplevel_decoration_v1_add_listener(window->decoration,
                                                 &decoration_listener, window);
        if (opts->decoration == DECORATION_CLIENT) {
            zxdg_toplevel_decoration_v1_set_mode(
                window->decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
        } else if (opts->decoration == DECORATION_SERVER) {
            zxdg_toplevel_decoration_v1_set_mode(
                window->decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        }
    }

    window_initial_commit(window);
    return window;
}

void window_map(struct window *window) {
    int64_t deadline = now_ms() + 5000;
    while (!window->committed && now_ms() < deadline) {
        pump(50);
    }
    REQUIRE(window->committed, "the window was never configured");
    settle();
}

void window_destroy(struct window *window) {
    if (window->decoration) {
        zxdg_toplevel_decoration_v1_destroy(window->decoration);
    }
    xdg_toplevel_destroy(window->toplevel);
    xdg_surface_destroy(window->xdg_surface);
    wl_surface_destroy(window->surface);
    struct client *client = window->client;
    free(window);
    client_roundtrip(client);
    settle();
}

void window_unmap(struct window *window) {
    window->hidden = true;
    window->committed = false;
    wl_surface_attach(window->surface, NULL, 0, 0);
    wl_surface_commit(window->surface);
    settle();
}

void window_remap(struct window *window) {
    window_initial_commit(window);
    window_map(window);
}

void window_set_fullscreen(struct window *window, bool fullscreen) {
    if (fullscreen) {
        xdg_toplevel_set_fullscreen(window->toplevel, NULL);
    } else {
        xdg_toplevel_unset_fullscreen(window->toplevel);
    }
    wl_surface_commit(window->surface);
    settle();
}

void window_set_maximized(struct window *window, bool maximized) {
    if (maximized) {
        xdg_toplevel_set_maximized(window->toplevel);
    } else {
        xdg_toplevel_unset_maximized(window->toplevel);
    }
    wl_surface_commit(window->surface);
    settle();
}

void window_set_title(struct window *window, const char *title) {
    xdg_toplevel_set_title(window->toplevel, title);
    wl_surface_commit(window->surface);
    settle();
}

void window_set_min_size(struct window *window, int width, int height) {
    window->opts.min_width = width;
    window->opts.min_height = height;
    xdg_toplevel_set_min_size(window->toplevel, width, height);
}

void window_set_max_size(struct window *window, int width, int height) {
    window->opts.max_width = width;
    window->opts.max_height = height;
    xdg_toplevel_set_max_size(window->toplevel, width, height);
}

struct capture {
    struct client *client;
    struct ext_image_capture_source_v1 *source;
    struct ext_image_copy_capture_session_v1 *session;
    int width, height;
    uint32_t format;
    bool format_ok;
    bool session_done;
    bool stopped;
    struct wl_buffer *buffer;
    int buffer_width, buffer_height;
    uint32_t buffer_format;
    void *data;
    int shots;
};

static struct capture capture;

static void session_buffer_size(void *data,
                                struct ext_image_copy_capture_session_v1 *session,
                                uint32_t width, uint32_t height) {
    capture.width = (int)width;
    capture.height = (int)height;
}

static void session_shm_format(void *data,
                               struct ext_image_copy_capture_session_v1 *session,
                               uint32_t format) {
    if (format == WL_SHM_FORMAT_XRGB8888 || format == WL_SHM_FORMAT_ARGB8888) {
        if (!capture.format_ok || format == WL_SHM_FORMAT_XRGB8888) {
            capture.format = format;
        }
        capture.format_ok = true;
    }
}

static void session_dmabuf_device(void *data,
                                  struct ext_image_copy_capture_session_v1 *session,
                                  struct wl_array *device) {
}

static void session_dmabuf_format(void *data,
                                  struct ext_image_copy_capture_session_v1 *session,
                                  uint32_t format, struct wl_array *modifiers) {
}

static void session_done(void *data,
                         struct ext_image_copy_capture_session_v1 *session) {
    capture.session_done = true;
}

static void session_stopped(void *data,
                            struct ext_image_copy_capture_session_v1 *session) {
    capture.stopped = true;
}

static const struct ext_image_copy_capture_session_v1_listener session_listener = {
    .buffer_size = session_buffer_size,
    .shm_format = session_shm_format,
    .dmabuf_device = session_dmabuf_device,
    .dmabuf_format = session_dmabuf_format,
    .done = session_done,
    .stopped = session_stopped,
};

struct frame_wait {
    bool ready;
    bool failed;
    uint32_t reason;
};

static void frame_transform(void *data, struct ext_image_copy_capture_frame_v1 *frame,
                            uint32_t transform) {
}

static void frame_damage(void *data, struct ext_image_copy_capture_frame_v1 *frame,
                         int32_t x, int32_t y, int32_t width, int32_t height) {
}

static void frame_presentation_time(void *data,
                                    struct ext_image_copy_capture_frame_v1 *frame,
                                    uint32_t sec_hi, uint32_t sec_lo,
                                    uint32_t nsec) {
}

static void frame_ready(void *data, struct ext_image_copy_capture_frame_v1 *frame) {
    struct frame_wait *wait = data;
    wait->ready = true;
}

static void frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *frame,
                         uint32_t reason) {
    struct frame_wait *wait = data;
    wait->failed = true;
    wait->reason = reason;
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
    .transform = frame_transform,
    .damage = frame_damage,
    .presentation_time = frame_presentation_time,
    .ready = frame_ready,
    .failed = frame_failed,
};

static void capture_init(void) {
    if (capture.client) {
        return;
    }
    capture.client = client_connect();
    struct client *client = capture.client;
    REQUIRE(client->output, "the compositor has no output");
    REQUIRE(client->source_manager && client->capture_manager,
            "the compositor does not offer ext-image-copy-capture");
    capture.source = ext_output_image_capture_source_manager_v1_create_source(
        client->source_manager, client->output);
}

static void capture_session_start(void) {
    capture.session_done = false;
    capture.stopped = false;
    capture.format_ok = false;
    capture.session = ext_image_copy_capture_manager_v1_create_session(
        capture.client->capture_manager, capture.source, 0);
    ext_image_copy_capture_session_v1_add_listener(capture.session,
                                                   &session_listener, NULL);

    int64_t deadline = now_ms() + 5000;
    while (!capture.session_done && !capture.stopped && now_ms() < deadline) {
        pump(20);
    }
    REQUIRE(capture.session_done, "the capture session never got ready");
    REQUIRE(capture.format_ok, "no usable shm format for screen capture");
    REQUIRE(capture.width > 0 && capture.height > 0, "bad capture size %dx%d",
            capture.width, capture.height);

    if (capture.buffer && (capture.buffer_width != capture.width || capture.buffer_height != capture.height || capture.buffer_format != capture.format)) {
        wl_buffer_destroy(capture.buffer);
        munmap(capture.data, (size_t)capture.buffer_width * capture.buffer_height * 4);
        capture.buffer = NULL;
    }
    if (!capture.buffer) {
        capture.buffer = create_shm_buffer(capture.client->shm, capture.width,
                                           capture.height, capture.format,
                                           &capture.data);
        capture.buffer_width = capture.width;
        capture.buffer_height = capture.height;
        capture.buffer_format = capture.format;
    }
}

struct shot *screen_capture(void) {
    capture_init();
    capture_session_start();

    struct frame_wait wait = {0};
    struct ext_image_copy_capture_frame_v1 *frame =
        ext_image_copy_capture_session_v1_create_frame(capture.session);
    ext_image_copy_capture_frame_v1_add_listener(frame, &frame_listener, &wait);
    ext_image_copy_capture_frame_v1_attach_buffer(frame, capture.buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0, capture.width,
                                                  capture.height);
    ext_image_copy_capture_frame_v1_capture(frame);

    int64_t deadline = now_ms() + 5000;
    while (!wait.ready && !wait.failed && now_ms() < deadline) {
        pump(20);
    }
    ext_image_copy_capture_frame_v1_destroy(frame);
    ext_image_copy_capture_session_v1_destroy(capture.session);
    capture.session = NULL;
    REQUIRE(wait.ready, "screen capture %s",
            wait.failed ? "failed" : "timed out");

    struct shot *shot = calloc(1, sizeof(*shot));
    shot->width = capture.width;
    shot->height = capture.height;
    shot->index = ++capture.shots;
    size_t count = (size_t)shot->width * shot->height;
    shot->pixels = malloc(count * 4);
    const uint32_t *src = capture.data;
    for (size_t i = 0; i < count; i++) {
        shot->pixels[i] = src[i] & 0xffffff;
    }
    return shot;
}

void pump(int timeout_ms) {
    struct pollfd fds[64];
    struct client *list[64];
    int n = 0;

    struct client *client;
    wl_list_for_each(client, &clients, link) {
        if (n >= 63) {
            break;
        }
        while (wl_display_prepare_read(client->display) != 0) {
            if (wl_display_dispatch_pending(client->display) < 0) {
                check_display_error(client);
            }
        }
        if (wl_display_flush(client->display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(client->display);
            check_display_error(client);
            continue;
        }
        list[n] = client;
        fds[n].fd = wl_display_get_fd(client->display);
        fds[n].events = POLLIN;
        fds[n].revents = 0;
        n++;
    }

    int xfd = x11_fd();
    int nfds = n;
    if (xfd >= 0) {
        fds[nfds].fd = xfd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;
    }

    int ret = poll(fds, nfds, timeout_ms);

    for (int i = 0; i < n; i++) {
        if (ret > 0 && (fds[i].revents & (POLLIN | POLLERR | POLLHUP))) {
            if (wl_display_read_events(list[i]->display) < 0) {
                check_display_error(list[i]);
            }
        } else {
            wl_display_cancel_read(list[i]->display);
        }
        if (wl_display_dispatch_pending(list[i]->display) < 0) {
            check_display_error(list[i]);
        }
    }

    if (xfd >= 0) {
        x11_dispatch();
    }
}

unsigned long test_activity(void) {
    return activity;
}

void test_bump_activity(void) {
    activity++;
}

bool x11_connected(void);
void x11_roundtrip(void);

void settle(void) {
    for (int i = 0; i < 50; i++) {
        unsigned long before = activity;
        struct client *client;
        wl_list_for_each(client, &clients, link) {
            if (wl_display_roundtrip(client->display) < 0) {
                check_display_error(client);
            }
        }
        if (x11_connected()) {
            x11_roundtrip();
        }
        if (activity == before) {
            break;
        }
    }
}
