/* © 2026 dancingmirrors */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <cairo/cairo.h>
#include <dbus/dbus.h>
#include <pango/pangocairo.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include <wayland-client.h>

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

#define NOTIFY_PATH "/org/freedesktop/Notifications"
#define NOTIFY_IFACE "org.freedesktop.Notifications"

#define FONT_NAME "Poison 12"

#define BOX_WIDTH 340
#define BOX_HEIGHT 92
#define BOX_PADDING 12
#define EDGE_GAP 10
#define STACK_GAP 8
#define BORDER_WIDTH 2

#define STORE_MAX 64
#define VISIBLE_HARD_MAX 12

#define REASON_EXPIRED 1
#define REASON_DISMISSED 2
#define REASON_CLOSED 3
#define REASON_UNDEFINED 4

struct notify_state;

struct notif {
    struct wl_list link;
    struct notify_state *state;

    uint32_t id;
    char *summary;
    char *body;
    bool has_default_action;

    bool is_overflow;
    int more_count;

    bool visible;
    int slot;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_buffer *buffer;
    void *shm_data;
    size_t shm_size;
    int buf_w, buf_h;
    int buf_scale;
    bool configured;
};

struct notify_state {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_output *output;

    DBusConnection *dbus;

    struct wl_list notifs;
    int notif_count;
    struct notif *overflow;

    uint32_t next_id;
    int max_slots;
    int output_mode_h;
    int output_height;
    int output_scale;

    struct notif *pointer_focus;

    bool running;
};

static void relayout(struct notify_state *state);
static void render_notif(struct notif *n);

static int create_shm_file(size_t size) {
    int fd = -1;

#if defined(__linux__) && defined(SYS_memfd_create)
    fd = syscall(SYS_memfd_create, "poison-notify", MFD_CLOEXEC);
    if (fd >= 0) {
        if (ftruncate(fd, size) < 0) {
            close(fd);
            return -1;
        }
        return fd;
    }
#elif defined(__FreeBSD__) || defined(__NetBSD__)
    fd = memfd_create("poison-notify", MFD_CLOEXEC);
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
    snprintf(name, sizeof(name), "/poison-notify-%d-%lld-%lld",
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

static void render_notif(struct notif *n) {
    struct notify_state *state = n->state;
    int w = n->buf_w > 0 ? n->buf_w : BOX_WIDTH;
    int h = n->buf_h > 0 ? n->buf_h : BOX_HEIGHT;
    int scale = state->output_scale > 0 ? state->output_scale : 1;
    int pw = w * scale;
    int ph = h * scale;

    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, pw);
    size_t size = (size_t)stride * ph;

    if (n->shm_size < size || !n->buffer || n->buf_scale != scale) {
        if (n->shm_data) {
            munmap(n->shm_data, n->shm_size);
            n->shm_data = NULL;
        }
        if (n->buffer) {
            wl_buffer_destroy(n->buffer);
            n->buffer = NULL;
        }

        int fd = create_shm_file(size);
        if (fd < 0) {
            fprintf(stderr, "poison-notify: failed to create shm file!\n");
            return;
        }
        n->shm_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                           fd, 0);
        if (n->shm_data == MAP_FAILED) {
            close(fd);
            n->shm_data = NULL;
            fprintf(stderr, "poison-notify: mmap failed!\n");
            return;
        }
        n->shm_size = size;

        struct wl_shm_pool *pool = wl_shm_create_pool(state->shm, fd, size);
        n->buffer = wl_shm_pool_create_buffer(pool, 0, pw, ph, stride,
                                              WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);
    }
    n->buf_scale = scale;

    cairo_surface_t *cs = cairo_image_surface_create_for_data(
        n->shm_data, CAIRO_FORMAT_ARGB32, pw, ph, stride);
    cairo_t *cr = cairo_create(cs);
    cairo_scale(cr, scale, scale);

    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
    cairo_rectangle(cr, BORDER_WIDTH, BORDER_WIDTH,
                    w - 2 * BORDER_WIDTH, h - 2 * BORDER_WIDTH);
    cairo_fill(cr);

    int text_w = w - 2 * BOX_PADDING;

    if (n->is_overflow) {
        char buf[64];
        snprintf(buf, sizeof(buf), "+%d more", n->more_count);

        PangoLayout *layout = pango_cairo_create_layout(cr);
        PangoFontDescription *desc =
            pango_font_description_from_string(FONT_NAME);
        pango_layout_set_font_description(layout, desc);
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        pango_layout_set_width(layout, text_w * PANGO_SCALE);
        pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
        pango_layout_set_text(layout, buf, -1);

        PangoRectangle logical;
        pango_layout_get_extents(layout, NULL, &logical);
        int th = logical.height / PANGO_SCALE;

        cairo_set_source_rgba(cr, 0.35, 0.35, 0.35, 1.0);
        cairo_move_to(cr, BOX_PADDING, (h - th) / 2);
        pango_cairo_show_layout(cr, layout);

        pango_font_description_free(desc);
        g_object_unref(layout);
    } else {
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0);

        PangoLayout *sl = pango_cairo_create_layout(cr);
        PangoFontDescription *sd =
            pango_font_description_from_string(FONT_NAME);
        pango_layout_set_font_description(sl, sd);
        pango_layout_set_ellipsize(sl, PANGO_ELLIPSIZE_END);
        pango_layout_set_width(sl, text_w * PANGO_SCALE);
        pango_layout_set_text(sl,
                              (n->summary && n->summary[0]) ? n->summary : "(no summary)", -1);

        PangoRectangle s_logical;
        pango_layout_get_extents(sl, NULL, &s_logical);
        int sh = s_logical.height / PANGO_SCALE;

        cairo_move_to(cr, BOX_PADDING, BOX_PADDING);
        pango_cairo_show_layout(cr, sl);

        pango_font_description_free(sd);
        g_object_unref(sl);

        if (n->body && n->body[0]) {
            int body_top = BOX_PADDING + sh + 4;
            int body_h = h - body_top - BOX_PADDING;
            if (body_h > 4) {
                PangoLayout *bl = pango_cairo_create_layout(cr);
                PangoFontDescription *bd =
                    pango_font_description_from_string(FONT_NAME);
                pango_layout_set_font_description(bl, bd);
                pango_layout_set_width(bl, text_w * PANGO_SCALE);
                pango_layout_set_height(bl, body_h * PANGO_SCALE);
                pango_layout_set_wrap(bl, PANGO_WRAP_WORD_CHAR);
                pango_layout_set_ellipsize(bl, PANGO_ELLIPSIZE_END);
                pango_layout_set_text(bl, n->body, -1);

                cairo_set_source_rgba(cr, 0.15, 0.15, 0.15, 1.0);
                cairo_move_to(cr, BOX_PADDING, body_top);
                pango_cairo_show_layout(cr, bl);

                pango_font_description_free(bd);
                g_object_unref(bl);
            }
        }
    }

    cairo_destroy(cr);
    cairo_surface_destroy(cs);

    wl_surface_set_buffer_scale(n->surface, scale);
    wl_surface_attach(n->surface, n->buffer, 0, 0);
    wl_surface_damage_buffer(n->surface, 0, 0, pw, ph);
    wl_surface_commit(n->surface);
}

static void layer_surface_configure(void *data,
                                    struct zwlr_layer_surface_v1 *surface,
                                    uint32_t serial, uint32_t width,
                                    uint32_t height) {
    struct notif *n = data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);

    int w = width > 0 ? (int)width : BOX_WIDTH;
    int h = height > 0 ? (int)height : BOX_HEIGHT;
    bool changed = (!n->configured || w != n->buf_w || h != n->buf_h);
    n->buf_w = w;
    n->buf_h = h;
    n->configured = true;
    if (changed) {
        render_notif(n);
    }
}

static void notif_hide(struct notif *n);

static void layer_surface_closed(void *data,
                                 struct zwlr_layer_surface_v1 *surface) {
    (void)surface;
    struct notif *n = data;
    notif_hide(n);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

static void notif_hide(struct notif *n) {
    if (n->state->pointer_focus == n) {
        n->state->pointer_focus = NULL;
    }
    if (n->layer_surface) {
        zwlr_layer_surface_v1_destroy(n->layer_surface);
        n->layer_surface = NULL;
    }
    if (n->surface) {
        wl_surface_destroy(n->surface);
        n->surface = NULL;
    }
    if (n->buffer) {
        wl_buffer_destroy(n->buffer);
        n->buffer = NULL;
    }
    if (n->shm_data) {
        munmap(n->shm_data, n->shm_size);
        n->shm_data = NULL;
        n->shm_size = 0;
    }
    n->configured = false;
    n->buf_w = 0;
    n->buf_h = 0;
    n->visible = false;
}

static int slot_top_margin(int slot) {
    return EDGE_GAP + slot * (BOX_HEIGHT + STACK_GAP);
}

static void notif_show_at(struct notif *n, int slot) {
    struct notify_state *state = n->state;
    n->slot = slot;
    n->visible = true;

    if (!n->surface) {
        n->surface = wl_compositor_create_surface(state->compositor);
        n->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
            state->layer_shell, n->surface, NULL,
            ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "poison-notify");
        zwlr_layer_surface_v1_set_size(n->layer_surface, BOX_WIDTH, BOX_HEIGHT);
        zwlr_layer_surface_v1_set_anchor(n->layer_surface,
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                             ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
        zwlr_layer_surface_v1_set_exclusive_zone(n->layer_surface, 0);
        zwlr_layer_surface_v1_set_margin(n->layer_surface,
                                         slot_top_margin(slot), EDGE_GAP, 0, 0);
        zwlr_layer_surface_v1_set_keyboard_interactivity(n->layer_surface,
                                                         ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
        zwlr_layer_surface_v1_add_listener(n->layer_surface,
                                           &layer_surface_listener, n);
        wl_surface_commit(n->surface);
    } else {
        zwlr_layer_surface_v1_set_margin(n->layer_surface,
                                         slot_top_margin(slot), EDGE_GAP, 0, 0);
        if (n->configured) {
            wl_surface_commit(n->surface);
        }
    }
}

static void relayout(struct notify_state *state) {
    int fit = VISIBLE_HARD_MAX;
    if (state->output_height > 0) {
        int usable = state->output_height - 2 * EDGE_GAP + STACK_GAP;
        fit = usable / (BOX_HEIGHT + STACK_GAP);
        if (fit < 1) {
            fit = 1;
        }
        if (fit > VISIBLE_HARD_MAX) {
            fit = VISIBLE_HARD_MAX;
        }
    }
    state->max_slots = fit;

    int total = state->notif_count;
    bool use_counter = (total > fit) && (fit >= 2);
    int visible_notifs = use_counter ? (fit - 1)
                                     : (total < fit ? total : fit);

    int hide_before = total - visible_notifs;
    if (hide_before < 0) {
        hide_before = 0;
    }

    int idx = 0;
    int slot_base = use_counter ? 1 : 0;
    int shown = 0;
    struct notif *n;
    wl_list_for_each(n, &state->notifs, link) {
        if (idx < hide_before) {
            if (n->surface) {
                notif_hide(n);
            } else {
                n->visible = false;
            }
        } else {
            notif_show_at(n, slot_base + shown);
            shown++;
        }
        idx++;
    }

    if (use_counter) {
        int more = total - visible_notifs;
        if (!state->overflow) {
            state->overflow = calloc(1, sizeof(*state->overflow));
            if (state->overflow) {
                state->overflow->state = state;
                state->overflow->is_overflow = true;
            }
        }
        if (state->overflow) {
            struct notif *ov = state->overflow;
            bool changed = (ov->more_count != more) || !ov->surface;
            ov->more_count = more;
            notif_show_at(ov, 0);
            if (changed && ov->configured) {
                render_notif(ov);
            }
        }
    } else if (state->overflow && state->overflow->surface) {
        notif_hide(state->overflow);
    }
}

static void emit_closed(struct notify_state *state, uint32_t id,
                        uint32_t reason) {
    DBusMessage *sig = dbus_message_new_signal(NOTIFY_PATH, NOTIFY_IFACE,
                                               "NotificationClosed");
    if (!sig) {
        return;
    }
    dbus_message_append_args(sig, DBUS_TYPE_UINT32, &id,
                             DBUS_TYPE_UINT32, &reason, DBUS_TYPE_INVALID);
    dbus_connection_send(state->dbus, sig, NULL);
    dbus_message_unref(sig);
    dbus_connection_flush(state->dbus);
}

static void emit_action_invoked(struct notify_state *state, uint32_t id,
                                const char *key) {
    DBusMessage *sig = dbus_message_new_signal(NOTIFY_PATH, NOTIFY_IFACE,
                                               "ActionInvoked");
    if (!sig) {
        return;
    }
    dbus_message_append_args(sig, DBUS_TYPE_UINT32, &id,
                             DBUS_TYPE_STRING, &key, DBUS_TYPE_INVALID);
    dbus_connection_send(state->dbus, sig, NULL);
    dbus_message_unref(sig);
    dbus_connection_flush(state->dbus);
}

static struct notif *find_notif(struct notify_state *state, uint32_t id) {
    struct notif *n;
    wl_list_for_each(n, &state->notifs, link) {
        if (n->id == id) {
            return n;
        }
    }
    return NULL;
}

static void notif_free(struct notif *n) {
    notif_hide(n);
    free(n->summary);
    free(n->body);
#ifdef __clang_analyzer__
    (void)n;
#else
    free(n);
#endif
}

static void close_notif(struct notify_state *state, struct notif *n,
                        uint32_t reason) {
    uint32_t id = n->id;
    wl_list_remove(&n->link);
    state->notif_count--;
    notif_free(n);
    emit_closed(state, id, reason);
    relayout(state);
}

static void close_notif_by_id(struct notify_state *state, uint32_t id,
                              uint32_t reason) {
    struct notif *n = find_notif(state, id);
    if (n) {
        close_notif(state, n, reason);
    }
}

static struct notif *notif_for_surface(struct notify_state *state,
                                       struct wl_surface *surface) {
    struct notif *n;
    wl_list_for_each(n, &state->notifs, link) {
        if (n->surface == surface) {
            return n;
        }
    }
    if (state->overflow && state->overflow->surface == surface) {
        return state->overflow;
    }
    return NULL;
}

static void pointer_enter(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t sx, wl_fixed_t sy) {
    (void)pointer;
    (void)serial;
    (void)sx;
    (void)sy;
    struct notify_state *state = data;
    state->pointer_focus = notif_for_surface(state, surface);
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface) {
    (void)pointer;
    (void)serial;
    struct notify_state *state = data;
    if (state->pointer_focus &&
        state->pointer_focus->surface == surface) {
        state->pointer_focus = NULL;
    }
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
                           uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {
    (void)data;
    (void)pointer;
    (void)time;
    (void)sx;
    (void)sy;
}

static void pointer_button(void *data, struct wl_pointer *pointer,
                           uint32_t serial, uint32_t time, uint32_t button,
                           uint32_t button_state) {
    (void)pointer;
    (void)serial;
    (void)time;
    struct notify_state *state = data;

    if (button_state != WL_POINTER_BUTTON_STATE_PRESSED) {
        return;
    }
    if (button != BTN_LEFT && button != BTN_RIGHT) {
        return;
    }
    struct notif *n = state->pointer_focus;
    if (!n) {
        return;
    }
    if (n->is_overflow) {
        return;
    }
    if (button == BTN_LEFT && n->has_default_action) {
        emit_action_invoked(state, n->id, "default");
    }
    close_notif(state, n, REASON_DISMISSED);
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
    struct notify_state *state = data;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !state->pointer) {
        state->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(state->pointer, &pointer_listener, state);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && state->pointer) {
        wl_pointer_release(state->pointer);
        state->pointer = NULL;
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

static void output_geometry(void *data, struct wl_output *output, int32_t x,
                            int32_t y, int32_t pw, int32_t ph, int32_t sub,
                            const char *make, const char *model,
                            int32_t transform) {
    (void)data;
    (void)output;
    (void)x;
    (void)y;
    (void)pw;
    (void)ph;
    (void)sub;
    (void)make;
    (void)model;
    (void)transform;
}

static void output_mode(void *data, struct wl_output *output, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh) {
    (void)output;
    (void)refresh;
    struct notify_state *state = data;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        state->output_mode_h = height;
    }
}

static void output_done(void *data, struct wl_output *output) {
    (void)output;
    struct notify_state *state = data;
    int scale = state->output_scale > 0 ? state->output_scale : 1;
    if (state->output_mode_h > 0) {
        state->output_height = state->output_mode_h / scale;
    }
    relayout(state);
    struct notif *n;
    wl_list_for_each(n, &state->notifs, link) {
        if (n->configured) {
            render_notif(n);
        }
    }
    if (state->overflow && state->overflow->configured) {
        render_notif(state->overflow);
    }
}

static void output_scale(void *data, struct wl_output *output, int32_t factor) {
    (void)output;
    struct notify_state *state = data;
    if (factor > 0) {
        state->output_scale = factor;
    }
}

static void output_name(void *data, struct wl_output *output, const char *name) {
    (void)data;
    (void)output;
    (void)name;
}

static void output_description(void *data, struct wl_output *output,
                               const char *desc) {
    (void)data;
    (void)output;
    (void)desc;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version) {
    (void)version;
    struct notify_state *state = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state->compositor = wl_registry_bind(registry, name,
                                             &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        state->layer_shell = wl_registry_bind(registry, name,
                                              &zwlr_layer_shell_v1_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        if (!state->seat) {
            state->seat = wl_registry_bind(registry, name,
                                           &wl_seat_interface, 5);
            wl_seat_add_listener(state->seat, &seat_listener, state);
        }
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        if (!state->output) {
            state->output = wl_registry_bind(registry, name,
                                             &wl_output_interface, 3);
            wl_output_add_listener(state->output, &output_listener, state);
        }
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

static const char *introspection_xml =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
    " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node>\n"
    "  <interface name=\"org.freedesktop.Notifications\">\n"
    "    <method name=\"Notify\">\n"
    "      <arg type=\"s\" name=\"app_name\" direction=\"in\"/>\n"
    "      <arg type=\"u\" name=\"replaces_id\" direction=\"in\"/>\n"
    "      <arg type=\"s\" name=\"app_icon\" direction=\"in\"/>\n"
    "      <arg type=\"s\" name=\"summary\" direction=\"in\"/>\n"
    "      <arg type=\"s\" name=\"body\" direction=\"in\"/>\n"
    "      <arg type=\"as\" name=\"actions\" direction=\"in\"/>\n"
    "      <arg type=\"a{sv}\" name=\"hints\" direction=\"in\"/>\n"
    "      <arg type=\"i\" name=\"expire_timeout\" direction=\"in\"/>\n"
    "      <arg type=\"u\" name=\"id\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"CloseNotification\">\n"
    "      <arg type=\"u\" name=\"id\" direction=\"in\"/>\n"
    "    </method>\n"
    "    <method name=\"GetCapabilities\">\n"
    "      <arg type=\"as\" name=\"capabilities\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"GetServerInformation\">\n"
    "      <arg type=\"s\" name=\"name\" direction=\"out\"/>\n"
    "      <arg type=\"s\" name=\"vendor\" direction=\"out\"/>\n"
    "      <arg type=\"s\" name=\"version\" direction=\"out\"/>\n"
    "      <arg type=\"s\" name=\"spec_version\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <signal name=\"NotificationClosed\">\n"
    "      <arg type=\"u\" name=\"id\"/>\n"
    "      <arg type=\"u\" name=\"reason\"/>\n"
    "    </signal>\n"
    "    <signal name=\"ActionInvoked\">\n"
    "      <arg type=\"u\" name=\"id\"/>\n"
    "      <arg type=\"s\" name=\"action_key\"/>\n"
    "    </signal>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
    "    <method name=\"Introspect\">\n"
    "      <arg type=\"s\" name=\"data\" direction=\"out\"/>\n"
    "    </method>\n"
    "  </interface>\n"
    "</node>\n";

static void send_reply(struct notify_state *state, DBusMessage *reply) {
    if (!reply) {
        return;
    }
    dbus_connection_send(state->dbus, reply, NULL);
    dbus_message_unref(reply);
    dbus_connection_flush(state->dbus);
}

static void handle_introspect(struct notify_state *state, DBusMessage *msg) {
    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (!reply) {
        return;
    }
    dbus_message_append_args(reply, DBUS_TYPE_STRING, &introspection_xml,
                             DBUS_TYPE_INVALID);
    send_reply(state, reply);
}

static void handle_get_server_information(struct notify_state *state,
                                          DBusMessage *msg) {
    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (!reply) {
        return;
    }
    const char *name = "poison";
    const char *vendor = "poison";
    const char *version = "1.0";
    const char *spec = "1.2";
    dbus_message_append_args(reply,
                             DBUS_TYPE_STRING, &name,
                             DBUS_TYPE_STRING, &vendor,
                             DBUS_TYPE_STRING, &version,
                             DBUS_TYPE_STRING, &spec,
                             DBUS_TYPE_INVALID);
    send_reply(state, reply);
}

static void handle_get_capabilities(struct notify_state *state,
                                    DBusMessage *msg) {
    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (!reply) {
        return;
    }
    DBusMessageIter iter, arr;
    dbus_message_iter_init_append(reply, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "s", &arr);
    /* Chromium demands "actions" even though we don't draw buttons. */
    const char *caps[] = {"actions", "body", "persistence"};
    for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
        const char *c = caps[i];
        dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &c);
    }
    dbus_message_iter_close_container(&iter, &arr);
    send_reply(state, reply);
}

static void handle_close_notification(struct notify_state *state,
                                      DBusMessage *msg) {
    DBusError err;
    dbus_error_init(&err);
    dbus_uint32_t id = 0;
    if (!dbus_message_get_args(msg, &err, DBUS_TYPE_UINT32, &id,
                               DBUS_TYPE_INVALID)) {
        dbus_error_free(&err);
        DBusMessage *e = dbus_message_new_error(msg,
                                                DBUS_ERROR_INVALID_ARGS, "CloseNotification expects a UINT32 id");
        send_reply(state, e);
        return;
    }

    close_notif_by_id(state, id, REASON_CLOSED);

    DBusMessage *reply = dbus_message_new_method_return(msg);
    send_reply(state, reply);
}

static void handle_notify(struct notify_state *state, DBusMessage *msg) {
    DBusMessageIter iter;
    if (!dbus_message_iter_init(msg, &iter)) {
        goto bad_args;
    }

    const char *app_name = NULL, *app_icon = NULL, *summary = NULL,
               *body = NULL;
    dbus_uint32_t replaces_id = 0;

    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING) {
        goto bad_args;
    }
    dbus_message_iter_get_basic(&iter, &app_name);
    dbus_message_iter_next(&iter);

    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_UINT32) {
        goto bad_args;
    }
    dbus_message_iter_get_basic(&iter, &replaces_id);
    dbus_message_iter_next(&iter);

    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING) {
        goto bad_args;
    }
    dbus_message_iter_get_basic(&iter, &app_icon);
    dbus_message_iter_next(&iter);

    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING) {
        goto bad_args;
    }
    dbus_message_iter_get_basic(&iter, &summary);
    dbus_message_iter_next(&iter);

    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING) {
        goto bad_args;
    }
    dbus_message_iter_get_basic(&iter, &body);

    bool has_default = false;
    dbus_message_iter_next(&iter);
    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
        DBusMessageIter actions;
        dbus_message_iter_recurse(&iter, &actions);
        while (dbus_message_iter_get_arg_type(&actions) == DBUS_TYPE_STRING) {
            const char *a = NULL;
            dbus_message_iter_get_basic(&actions, &a);
            if (a && strcmp(a, "default") == 0) {
                has_default = true;
            }
            dbus_message_iter_next(&actions);
        }
    }

    (void)app_name;
    (void)app_icon;

    char *new_summary = strdup(summary ? summary : "");
    char *new_body = strdup(body ? body : "");
    if (!new_summary || !new_body) {
        free(new_summary);
        free(new_body);
        DBusMessage *e = dbus_message_new_error(msg, DBUS_ERROR_NO_MEMORY,
                                                "out of memory");
        send_reply(state, e);
        return;
    }

    struct notif *n = NULL;
    if (replaces_id != 0) {
        n = find_notif(state, replaces_id);
    }

    dbus_uint32_t out_id;
    if (n) {
        free(n->summary);
        free(n->body);
        n->summary = new_summary;
        n->body = new_body;
        n->has_default_action = has_default;
        out_id = n->id;
        if (n->configured) {
            render_notif(n);
        }
    } else {
        n = calloc(1, sizeof(*n));
        if (!n) {
            free(new_summary);
            free(new_body);
            DBusMessage *e = dbus_message_new_error(msg, DBUS_ERROR_NO_MEMORY,
                                                    "out of memory");
            send_reply(state, e);
            return;
        }
        n->state = state;
        n->summary = new_summary;
        n->body = new_body;
        n->has_default_action = has_default;

        if (replaces_id != 0) {
            n->id = replaces_id;
            if (replaces_id >= state->next_id) {
                state->next_id = replaces_id + 1;
            }
        } else {
            n->id = state->next_id++;
            if (state->next_id == 0) {
                state->next_id = 1;
            }
        }
        out_id = n->id;

        while (state->notif_count >= STORE_MAX &&
               !wl_list_empty(&state->notifs)) {
            struct notif *oldest =
                wl_container_of(state->notifs.next, oldest, link);
            close_notif(state, oldest, REASON_EXPIRED);
        }

        wl_list_insert(state->notifs.prev, &n->link);
        state->notif_count++;
        relayout(state);
    }

    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (reply) {
        dbus_message_append_args(reply, DBUS_TYPE_UINT32, &out_id,
                                 DBUS_TYPE_INVALID);
        send_reply(state, reply);
    }
    return;

bad_args:;
    DBusMessage *e = dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS,
                                            "Notify: bad arguments");
    send_reply(state, e);
}

static DBusHandlerResult dbus_message_handler(DBusConnection *conn,
                                              DBusMessage *msg, void *data) {
    (void)conn;
    struct notify_state *state = data;

    if (dbus_message_is_method_call(msg,
                                    "org.freedesktop.DBus.Introspectable", "Introspect")) {
        handle_introspect(state, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    const char *iface = dbus_message_get_interface(msg);
    if (iface && strcmp(iface, NOTIFY_IFACE) != 0) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    const char *member = dbus_message_get_member(msg);
    if (!member) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    if (strcmp(member, "Notify") == 0) {
        handle_notify(state, msg);
    } else if (strcmp(member, "CloseNotification") == 0) {
        handle_close_notification(state, msg);
    } else if (strcmp(member, "GetCapabilities") == 0) {
        handle_get_capabilities(state, msg);
    } else if (strcmp(member, "GetServerInformation") == 0) {
        handle_get_server_information(state, msg);
    } else {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    return DBUS_HANDLER_RESULT_HANDLED;
}

static const DBusObjectPathVTable notify_vtable = {
    .unregister_function = NULL,
    .message_function = dbus_message_handler,
};

static void dbus_dispatch_all(struct notify_state *state) {
    while (dbus_connection_get_dispatch_status(state->dbus) ==
           DBUS_DISPATCH_DATA_REMAINS) {
        dbus_connection_dispatch(state->dbus);
    }
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    struct notify_state state = {0};
    wl_list_init(&state.notifs);
    state.next_id = 1;
    state.output_scale = 1;
    state.max_slots = VISIBLE_HARD_MAX;
    state.running = true;

    state.display = wl_display_connect(NULL);
    if (!state.display) {
        fprintf(stderr, "poison-notify: failed to connect to display!\n");
        return 1;
    }

    state.registry = wl_display_get_registry(state.display);
    wl_registry_add_listener(state.registry, &registry_listener, &state);

    DBusError err;
    dbus_error_init(&err);
    state.dbus = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!state.dbus || dbus_error_is_set(&err)) {
        fprintf(stderr, "poison-notify: no session bus: %s\n",
                dbus_error_is_set(&err) ? err.message : "unknown");
        dbus_error_free(&err);
        wl_display_disconnect(state.display);
        return 1;
    }
    dbus_connection_set_exit_on_disconnect(state.dbus, FALSE);

    int req = dbus_bus_request_name(state.dbus, NOTIFY_IFACE,
                                    DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "poison-notify: request name failed: %s\n",
                err.message);
        dbus_error_free(&err);
        dbus_connection_unref(state.dbus);
        wl_display_disconnect(state.display);
        return 1;
    }
    if (req != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        fprintf(stderr, "poison-notify: %s already owned by another daemon; "
                        "exiting.\n",
                NOTIFY_IFACE);
        dbus_connection_unref(state.dbus);
        wl_display_disconnect(state.display);
        return 0;
    }

    if (!dbus_connection_register_object_path(state.dbus, NOTIFY_PATH,
                                              &notify_vtable, &state)) {
        fprintf(stderr, "poison-notify: failed to register object path!\n");
        dbus_connection_unref(state.dbus);
        wl_display_disconnect(state.display);
        return 1;
    }

    wl_display_roundtrip(state.display);
    wl_display_roundtrip(state.display);

    if (!state.compositor || !state.layer_shell || !state.shm) {
        fprintf(stderr, "poison-notify: missing Wayland interfaces!\n");
        dbus_connection_unref(state.dbus);
        wl_display_disconnect(state.display);
        return 1;
    }

    int wl_fd = wl_display_get_fd(state.display);
    int dbus_fd = -1;
    if (!dbus_connection_get_unix_fd(state.dbus, &dbus_fd) || dbus_fd < 0) {
        fprintf(stderr, "poison-notify: could not get D-Bus socket fd!\n");
        wl_display_disconnect(state.display);
        return 1;
    }

    while (state.running) {
        dbus_dispatch_all(&state);
        if (!state.running) {
            break;
        }
        dbus_connection_flush(state.dbus);
        if (dbus_connection_get_dispatch_status(state.dbus) ==
            DBUS_DISPATCH_DATA_REMAINS) {
            continue;
        }

        while (wl_display_prepare_read(state.display) != 0) {
            if (wl_display_dispatch_pending(state.display) < 0) {
                state.running = false;
                break;
            }
        }
        if (!state.running) {
            break;
        }
        int flush_ret = wl_display_flush(state.display);
        bool need_flush = (flush_ret < 0 && errno == EAGAIN);
        if (flush_ret < 0 && !need_flush) {
            wl_display_cancel_read(state.display);
            break;
        }

        struct pollfd pfds[2];
        pfds[0].fd = wl_fd;
        pfds[0].events = (short)(POLLIN | (need_flush ? POLLOUT : 0));
        pfds[0].revents = 0;
        pfds[1].fd = dbus_fd;
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;

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

        if (pfds[1].revents & (POLLIN | POLLERR | POLLHUP)) {
            if (!dbus_connection_read_write(state.dbus, 0)) {
                state.running = false;
            }
        }
    }

    struct notif *n, *tmp;
    wl_list_for_each_safe(n, tmp, &state.notifs, link) {
        wl_list_remove(&n->link);
        notif_free(n);
    }
    if (state.overflow) {
        notif_free(state.overflow);
    }
    if (state.dbus) {
        dbus_connection_unref(state.dbus);
    }
    if (state.pointer) {
        wl_pointer_release(state.pointer);
    }
    if (state.output) {
        wl_output_release(state.output);
    }
    wl_display_disconnect(state.display);
    return 0;
}
