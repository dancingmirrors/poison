/* © 2026 dancingmirrors */

#include <cairo/cairo.h>
#include <drm_fourcc.h>
#include <pango/pangocairo.h>

#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>

#include "poison.h"

enum {
    FRAME_BORDER_TOP,
    FRAME_BORDER_BOTTOM,
    FRAME_BORDER_LEFT,
    FRAME_BORDER_RIGHT,
    FRAME_BORDER_COUNT
};

#define TITLE_TEXT_PADDING 8

static const char *decoration_font(struct poison_server *server) {
    return server->config.decoration_font ? server->config.decoration_font
                                          : POISON_DECORATION_FONT;
}

static int measure_line_height(struct poison_server *server) {
    cairo_surface_t *surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return -1;
    }

    cairo_t *cr = cairo_create(surface);
    int height = -1;
    PangoLayout *layout = pango_cairo_create_layout(cr);
    if (layout) {
        PangoFontDescription *desc =
            pango_font_description_from_string(decoration_font(server));
        if (desc) {
            pango_layout_set_font_description(layout, desc);
            pango_font_description_free(desc);
        }
        pango_layout_set_text(layout, "Agjy", -1);
        int width;
        pango_layout_get_pixel_size(layout, &width, &height);
        g_object_unref(layout);
    }
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    return height;
}

static int title_bar_height(struct poison_server *server) {
    if (server->config.decoration_title_height >= 0) {
        return server->config.decoration_title_height;
    }
    if (server->frame_title_height_resolved) {
        return server->frame_title_height;
    }

    int line = measure_line_height(server);
    server->frame_title_height = line > 0
        ? line + 2 * POISON_DECORATION_TITLE_PADDING
        : POISON_DECORATION_TITLE_HEIGHT;
    server->frame_title_height_resolved = true;
    return server->frame_title_height;
}

struct poison_cairo_buffer {
    struct wlr_buffer base;
    cairo_surface_t *surface;
};

static void cairo_buffer_destroy(struct wlr_buffer *wlr_buffer) {
    struct poison_cairo_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
    cairo_surface_destroy(buffer->surface);
    free(buffer);
}

static bool cairo_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
                                               uint32_t flags, void **data,
                                               uint32_t *format,
                                               size_t *stride) {
    struct poison_cairo_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);

    if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) {
        return false;
    }

    *data = cairo_image_surface_get_data(buffer->surface);
    *format = DRM_FORMAT_ARGB8888;
    *stride = (size_t)cairo_image_surface_get_stride(buffer->surface);
    return *data != NULL;
}

static void cairo_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
    /* Nothing to release because the mapping is the Cairo surface itself. */
}

static const struct wlr_buffer_impl cairo_buffer_impl = {
    .destroy = cairo_buffer_destroy,
    .begin_data_ptr_access = cairo_buffer_begin_data_ptr_access,
    .end_data_ptr_access = cairo_buffer_end_data_ptr_access,
};

/* Takes ownership of the surface on success. */
static struct wlr_buffer *cairo_buffer_create(cairo_surface_t *surface) {
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        return NULL;
    }

    struct poison_cairo_buffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer) {
        return NULL;
    }

    cairo_surface_flush(surface);
    buffer->surface = surface;
    wlr_buffer_init(&buffer->base, &cairo_buffer_impl,
                    cairo_image_surface_get_width(surface),
                    cairo_image_surface_get_height(surface));
    return &buffer->base;
}

static void shade_color(const float in[static 4], float factor,
                        float out[static 4]) {
    for (int i = 0; i < 3; i++) {
        float v = in[i] * factor;
        out[i] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }
    out[3] = in[3];
}

static const float *frame_border_color(struct poison_server *server,
                                       bool active) {
    return active ? server->config.decoration_active_border_color
                  : server->config.decoration_inactive_border_color;
}

static void premultiply_color(const float in[static 4], float out[static 4]) {
    for (int i = 0; i < 3; i++) {
        out[i] = in[i] * in[3];
    }
    out[3] = in[3];
}

static float decoration_scale(struct poison_server *server) {
    float scale = 1.0f;
    struct poison_output *output;
    wl_list_for_each(output, &server->outputs, link) {
        if (output->wlr_output && output->wlr_output->scale > scale) {
            scale = output->wlr_output->scale;
        }
    }
    return scale;
}

static struct wlr_buffer *render_title_bar(struct poison_server *server,
                                           const char *title, int width,
                                           int height, float scale,
                                           bool active) {
    int pixel_width = (int)((float)width * scale + 0.5f);
    int pixel_height = (int)((float)height * scale + 0.5f);
    if (pixel_width <= 0 || pixel_height <= 0) {
        return NULL;
    }

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                          pixel_width,
                                                          pixel_height);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return NULL;
    }

    cairo_t *cr = cairo_create(surface);
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return NULL;
    }

    cairo_scale(cr, scale, scale);

    const float *base = active ? server->config.decoration_active_color
                               : server->config.decoration_inactive_color;
    float top[4], bottom[4];
    shade_color(base, POISON_DECORATION_GRADIENT_TOP, top);
    shade_color(base, POISON_DECORATION_GRADIENT_BOTTOM, bottom);

    cairo_pattern_t *gradient = cairo_pattern_create_linear(0, 0, 0, height);
    cairo_pattern_add_color_stop_rgba(gradient, 0.0, top[0], top[1], top[2],
                                      top[3]);
    cairo_pattern_add_color_stop_rgba(gradient, 1.0, bottom[0], bottom[1],
                                      bottom[2], bottom[3]);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source(cr, gradient);
    cairo_paint(cr);
    cairo_pattern_destroy(gradient);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    int avail = width - 2 * TITLE_TEXT_PADDING;
    if (title && *title && avail > 0) {
        PangoLayout *layout = pango_cairo_create_layout(cr);
        if (layout) {
            PangoFontDescription *desc =
                pango_font_description_from_string(decoration_font(server));
            if (desc) {
                pango_layout_set_font_description(layout, desc);
                pango_font_description_free(desc);
            }
            pango_layout_set_text(layout, title, -1);
            pango_layout_set_single_paragraph_mode(layout, TRUE);
            pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
            pango_layout_set_width(layout, avail * PANGO_SCALE);

            int text_width, text_height;
            pango_layout_get_pixel_size(layout, &text_width, &text_height);

            const float *color = active
                ? server->config.decoration_active_text_color
                : server->config.decoration_inactive_text_color;
            cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);
            cairo_move_to(cr, TITLE_TEXT_PADDING,
                          (double)(height - text_height) / 2.0);
            pango_cairo_show_layout(cr, layout);
            g_object_unref(layout);
        }
    }

    cairo_destroy(cr);

    struct wlr_buffer *buffer = cairo_buffer_create(surface);
    if (!buffer) {
        cairo_surface_destroy(surface);
    }
    return buffer;
}

static bool decorations_on(struct poison_server *server) {
    return server && server->config.decorations_enabled;
}

static bool state_wants_frame(bool floating, bool in_hsplit, bool fullscreen) {
    return floating && !in_hsplit && !fullscreen;
}

bool poison_toplevel_decorated(struct poison_toplevel *toplevel) {
    if (!toplevel || !toplevel->server || !decorations_on(toplevel->server)) {
        return false;
    }
    if (!toplevel->xdg_toplevel || !toplevel->xdg_toplevel->base ||
        !toplevel->scene_tree) {
        return false;
    }
    if (toplevel->client_side_decorations) {
        return false;
    }
    if (!state_wants_frame(toplevel->floating, toplevel->in_hsplit,
                           toplevel->xdg_toplevel->scheduled.fullscreen)) {
        return false;
    }
    if (toplevel->xdg_toplevel->parent) {
        return false;
    }
    return true;
}

static bool xwayland_surface_is_transient(
    struct wlr_xwayland_surface *xsurface) {
    if (xsurface->parent || xsurface->modal) {
        return true;
    }
    return xsurface->window_type_len > 0 &&
        !wlr_xwayland_surface_has_window_type(
               xsurface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_NORMAL);
}

bool poison_xwayland_view_decorated(struct poison_xwayland_view *view) {
    if (!view || !view->server || !decorations_on(view->server)) {
        return false;
    }
    if (!view->xwayland_surface || !view->scene_tree) {
        return false;
    }
    if (view->server->config.decoration_respect_client &&
        (view->xwayland_surface->decorations &
         WLR_XWAYLAND_SURFACE_DECORATIONS_NO_BORDER)) {
        return false;
    }
    if (!state_wants_frame(view->floating, view->in_hsplit,
                           view->xwayland_surface->fullscreen)) {
        return false;
    }
    if (xwayland_surface_is_transient(view->xwayland_surface)) {
        return false;
    }
    return true;
}

static struct poison_frame_insets frame_insets(struct poison_server *server,
                                               bool decorated) {
    struct poison_frame_insets insets = {0};
    if (!decorated) {
        return insets;
    }

    int border = server->config.decoration_border_width;
    int title = title_bar_height(server);
    if (border < 0) {
        border = 0;
    }
    if (title < 0) {
        title = 0;
    }

    insets.left = border;
    insets.right = border;
    insets.bottom = border;
    insets.top = title > 0 ? border + title + border : border;
    return insets;
}

struct poison_frame_insets poison_toplevel_insets(
    struct poison_toplevel *toplevel) {
    if (!toplevel || !toplevel->server) {
        return (struct poison_frame_insets){0};
    }
    return frame_insets(toplevel->server, poison_toplevel_decorated(toplevel));
}

struct poison_frame_insets poison_xwayland_view_insets(
    struct poison_xwayland_view *view) {
    if (!view || !view->server) {
        return (struct poison_frame_insets){0};
    }
    return frame_insets(view->server, poison_xwayland_view_decorated(view));
}

static struct wlr_buffer *render_button(struct poison_server *server,
                                        enum poison_frame_button button,
                                        int size, float scale, bool active) {
    int pixels = (int)((float)size * scale + 0.5f);
    if (pixels <= 0) {
        return NULL;
    }

    cairo_surface_t *surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pixels, pixels);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return NULL;
    }

    cairo_t *cr = cairo_create(surface);
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return NULL;
    }

    cairo_scale(cr, scale, scale);

    const float *base = active ? server->config.decoration_active_color
                               : server->config.decoration_inactive_color;
    float top[4], bottom[4];
    shade_color(base, POISON_DECORATION_GRADIENT_TOP, top);
    shade_color(base, POISON_DECORATION_GRADIENT_BOTTOM, bottom);

    cairo_pattern_t *gradient = cairo_pattern_create_linear(0, 0, 0, size);
    cairo_pattern_add_color_stop_rgba(gradient, 0.0, top[0], top[1], top[2],
                                      top[3]);
    cairo_pattern_add_color_stop_rgba(gradient, 1.0, bottom[0], bottom[1],
                                      bottom[2], bottom[3]);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source(cr, gradient);
    cairo_paint(cr);
    cairo_pattern_destroy(gradient);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    const float *color = active ? server->config.decoration_active_text_color
                                : server->config.decoration_inactive_text_color;
    cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);

    double center = (double)size / 2.0;
    double arm = (double)size * 0.22;
    double line = (double)size / 12.0;
    if (line < 1.0) {
        line = 1.0;
    }
    cairo_set_line_width(cr, line);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);

    if (button == POISON_FRAME_BUTTON_CLOSE) {
        cairo_move_to(cr, center - arm, center - arm);
        cairo_line_to(cr, center + arm, center + arm);
        cairo_move_to(cr, center + arm, center - arm);
        cairo_line_to(cr, center - arm, center + arm);
    } else {
        cairo_move_to(cr, center - arm, center);
        cairo_line_to(cr, center + arm, center);
        cairo_move_to(cr, center, center - arm);
        cairo_line_to(cr, center, center + arm);
    }
    cairo_stroke(cr);

    cairo_destroy(cr);

    struct wlr_buffer *buffer = cairo_buffer_create(surface);
    if (!buffer) {
        cairo_surface_destroy(surface);
    }
    return buffer;
}

enum poison_frame_button poison_frame_button_at(struct poison_frame *frame,
                                                struct wlr_scene_node *node) {
    if (!frame || !node) {
        return POISON_FRAME_BUTTON_NONE;
    }
    for (int i = 0; i < POISON_FRAME_BUTTON_COUNT; i++) {
        if (frame->button[i] && &frame->button[i]->node == node) {
            return (enum poison_frame_button)i;
        }
    }
    return POISON_FRAME_BUTTON_NONE;
}

static void frame_destroy(struct poison_frame *frame) {
    if (!frame) {
        return;
    }
    if (frame->tree) {
        wlr_scene_node_destroy(&frame->tree->node);
    }
    free(frame->title_text);
    free(frame);
}

static struct poison_frame *frame_create(struct poison_server *server,
                                         struct wlr_scene_tree *parent) {
    struct poison_frame *frame = calloc(1, sizeof(*frame));
    if (!frame) {
        wlr_log(WLR_ERROR, "Failed to allocate a window frame!");
        return NULL;
    }
    frame->server = server;
    frame->title_scale = 1.0f;
    frame->button_scale = 1.0f;

    frame->tree = wlr_scene_tree_create(parent);
    if (!frame->tree) {
        wlr_log(WLR_ERROR, "Failed to create a scene tree for a window frame!");
        free(frame);
        return NULL;
    }

    wlr_scene_node_lower_to_bottom(&frame->tree->node);

    static const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (int i = 0; i < FRAME_BORDER_COUNT; i++) {
        frame->border[i] = wlr_scene_rect_create(frame->tree, 0, 0, transparent);
        if (!frame->border[i]) {
            wlr_log(WLR_ERROR, "Failed to create a window frame border!");
            frame_destroy(frame);
            return NULL;
        }
    }
    frame->separator = wlr_scene_rect_create(frame->tree, 0, 0, transparent);
    if (!frame->separator) {
        wlr_log(WLR_ERROR, "Failed to create a window frame separator!");
        frame_destroy(frame);
        return NULL;
    }

    frame->title = wlr_scene_buffer_create(frame->tree, NULL);
    if (!frame->title) {
        wlr_log(WLR_ERROR, "Failed to create a window title bar!");
        frame_destroy(frame);
        return NULL;
    }
    for (int i = 0; i < POISON_FRAME_BUTTON_COUNT; i++) {
        frame->button[i] = wlr_scene_buffer_create(frame->tree, NULL);
        if (!frame->button[i]) {
            wlr_log(WLR_ERROR, "Failed to create a title bar button!");
            frame_destroy(frame);
            return NULL;
        }
    }

    return frame;
}

static void place_rect(struct wlr_scene_rect *rect, int x, int y, int width,
                       int height, const float color[static 4]) {
    if (width < 0) {
        width = 0;
    }
    if (height < 0) {
        height = 0;
    }
    bool visible = width > 0 && height > 0;
    wlr_scene_node_set_enabled(&rect->node, visible);
    if (!visible) {
        return;
    }
    float premultiplied[4];
    premultiply_color(color, premultiplied);
    wlr_scene_rect_set_size(rect, width, height);
    wlr_scene_rect_set_color(rect, premultiplied);
    wlr_scene_node_set_position(&rect->node, x, y);
}

static void hide_node(struct wlr_scene_node *node) {
    wlr_scene_node_set_enabled(node, false);
}

static void frame_hide(struct poison_frame *frame) {
    if (frame && frame->tree) {
        hide_node(&frame->tree->node);
    }
}

static void frame_apply(struct poison_frame *frame,
                        const struct wlr_box *client, bool active,
                        const char *title, bool show_maximize_button) {
    struct poison_server *server = frame->server;

    if (client->width <= 0 || client->height <= 0) {
        frame_hide(frame);
        return;
    }

    struct poison_frame_insets insets = frame_insets(server, true);
    int border = insets.left;

    struct wlr_box outer = {
        .x = client->x - insets.left,
        .y = client->y - insets.top,
        .width = client->width + insets.left + insets.right,
        .height = client->height + insets.top + insets.bottom,
    };

    wlr_scene_node_set_enabled(&frame->tree->node, true);

    const float *border_color = frame_border_color(server, active);
    place_rect(frame->border[FRAME_BORDER_TOP], outer.x, outer.y, outer.width,
               border, border_color);
    place_rect(frame->border[FRAME_BORDER_BOTTOM], outer.x,
               outer.y + outer.height - border, outer.width, border,
               border_color);
    place_rect(frame->border[FRAME_BORDER_LEFT], outer.x, outer.y + border,
               border, outer.height - 2 * border, border_color);
    place_rect(frame->border[FRAME_BORDER_RIGHT],
               outer.x + outer.width - border, outer.y + border, border,
               outer.height - 2 * border, border_color);

    int title_height = title_bar_height(server);
    int title_width = outer.width - 2 * border;
    if (title_height <= 0 || title_width <= 0) {
        hide_node(&frame->title->node);
        hide_node(&frame->separator->node);
        for (int i = 0; i < POISON_FRAME_BUTTON_COUNT; i++) {
            hide_node(&frame->button[i]->node);
        }
        return;
    }

    place_rect(frame->separator, outer.x + border,
               outer.y + border + title_height, title_width, border,
               border_color);

    int button_size = title_height;
    int button_count = show_maximize_button ? 2 : 1;
    bool show_buttons = title_width >= button_size * button_count + button_size;
    int buttons_width = show_buttons ? button_size * button_count : 0;

    float scale = decoration_scale(server);
    int text_width = title_width - buttons_width;

    if (text_width > 0 &&
        (!frame->title_text || strcmp(frame->title_text, title ? title : "") != 0 ||
         frame->title_width != text_width || frame->title_height != title_height ||
         frame->title_scale != scale || frame->title_active != active)) {
        struct wlr_buffer *buffer = render_title_bar(server, title, text_width,
                                                     title_height, scale,
                                                     active);
        /* A failed render leaves the previous buffer in place, which looks
         * better than a hole in the frame. */
        if (buffer) {
            wlr_scene_buffer_set_buffer(frame->title, buffer);
            wlr_buffer_drop(buffer);

            free(frame->title_text);
            frame->title_text = strdup(title ? title : "");
            frame->title_width = text_width;
            frame->title_height = title_height;
            frame->title_scale = scale;
            frame->title_active = active;
            frame->title_ready = true;
        }
    }

    bool have_title = text_width > 0 && frame->title_ready;
    wlr_scene_node_set_enabled(&frame->title->node, have_title);
    if (have_title) {
        wlr_scene_buffer_set_dest_size(frame->title, text_width, title_height);
        wlr_scene_node_set_position(&frame->title->node, outer.x + border,
                                    outer.y + border);
    }

    if (frame->button_size != button_size || frame->button_active != active ||
        frame->button_scale != scale) {
        bool rendered = true;
        for (int i = 0; i < POISON_FRAME_BUTTON_COUNT; i++) {
            struct wlr_buffer *buffer =
                render_button(server, (enum poison_frame_button)i, button_size,
                              scale, active);
            if (!buffer) {
                rendered = false;
                continue;
            }
            wlr_scene_buffer_set_buffer(frame->button[i], buffer);
            wlr_buffer_drop(buffer);
        }
        if (rendered) {
            frame->button_size = button_size;
            frame->button_scale = scale;
            frame->button_active = active;
            frame->buttons_ready = true;
        }
    }

    int button_x = outer.x + outer.width - border - button_size;
    for (int i = POISON_FRAME_BUTTON_COUNT - 1; i >= 0; i--) {
        bool visible = show_buttons && frame->buttons_ready &&
            (i != POISON_FRAME_BUTTON_MAXIMIZE || show_maximize_button);
        wlr_scene_node_set_enabled(&frame->button[i]->node, visible);
        if (!visible) {
            continue;
        }
        wlr_scene_buffer_set_dest_size(frame->button[i], button_size,
                                       button_size);
        wlr_scene_node_set_position(&frame->button[i]->node, button_x,
                                    outer.y + border);
        button_x -= button_size;
    }
}

static bool toplevel_client_box(struct poison_toplevel *toplevel,
                                struct wlr_box *box) {
    struct wlr_xdg_surface *base = toplevel->xdg_toplevel->base;
    struct wlr_box geo = base->geometry;
    if (geo.width > 0 && geo.height > 0) {
        *box = (struct wlr_box){
            .x = 0,
            .y = 0,
            .width = geo.width,
            .height = geo.height,
        };
        return true;
    }

    struct wlr_surface *surface = base->surface;
    if (!surface || surface->current.width <= 0 || surface->current.height <= 0) {
        return false;
    }
    *box = (struct wlr_box){
        .x = 0,
        .y = 0,
        .width = surface->current.width,
        .height = surface->current.height,
    };
    return true;
}

void poison_decoration_update_toplevel(struct poison_toplevel *toplevel) {
    if (!toplevel) {
        return;
    }

    if (!poison_toplevel_decorated(toplevel)) {
        frame_hide(toplevel->frame);
        return;
    }

    struct wlr_box box;
    if (!toplevel_client_box(toplevel, &box)) {
        frame_hide(toplevel->frame);
        return;
    }

    if (!toplevel->frame) {
        toplevel->frame = frame_create(toplevel->server, toplevel->scene_tree);
        if (!toplevel->frame) {
            return;
        }
    }

    frame_apply(toplevel->frame, &box,
                toplevel->server->focused_toplevel == toplevel,
                toplevel->xdg_toplevel->title, !toplevel->in_hsplit);
}

static bool xwayland_client_box(struct poison_xwayland_view *view,
                                struct wlr_box *box) {
    struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
    if (xsurface->width <= 0 || xsurface->height <= 0) {
        return false;
    }

    struct wlr_box client = {
        .x = 0,
        .y = 0,
        .width = xsurface->width,
        .height = xsurface->height,
    };

    bool focused = (view == view->server->focused_xwayland_view);
    bool clipped = !view->floating && (!focused || view->in_hsplit) &&
        view->clip_box.width > 0 && view->clip_box.height > 0;
    if (clipped) {
        int lx, ly;
        if (wlr_scene_node_coords(&view->scene_tree->node, &lx, &ly)) {
            struct wlr_box clip = {
                .x = view->clip_box.x - lx,
                .y = view->clip_box.y - ly,
                .width = view->clip_box.width,
                .height = view->clip_box.height,
            };
            if (!wlr_box_intersection(&client, &client, &clip)) {
                return false;
            }
        }
    }

    *box = client;
    return true;
}

void poison_decoration_update_xwayland(struct poison_xwayland_view *view) {
    if (!view) {
        return;
    }

    if (!poison_xwayland_view_decorated(view)) {
        frame_hide(view->frame);
        return;
    }

    struct wlr_box box;
    if (!xwayland_client_box(view, &box)) {
        frame_hide(view->frame);
        return;
    }

    if (!view->frame) {
        view->frame = frame_create(view->server, view->scene_tree);
        if (!view->frame) {
            return;
        }
    }

    bool no_title = view->server->config.decoration_respect_client &&
        (view->xwayland_surface->decorations &
         WLR_XWAYLAND_SURFACE_DECORATIONS_NO_TITLE);

    frame_apply(view->frame, &box,
                view->server->focused_xwayland_view == view,
                no_title ? NULL : view->xwayland_surface->title,
                !view->in_hsplit);
}

void poison_decoration_update_all(struct poison_server *server) {
    if (!server) {
        return;
    }

    struct poison_toplevel *toplevel;
    wl_list_for_each(toplevel, &server->toplevels, link) {
        poison_decoration_update_toplevel(toplevel);
    }

    struct poison_xwayland_view *view;
    wl_list_for_each(view, &server->xwayland_views, link) {
        poison_decoration_update_xwayland(view);
    }
}

static void frame_invalidate(struct poison_frame *frame) {
    if (!frame) {
        return;
    }
    free(frame->title_text);
    frame->title_text = NULL;
    frame->title_width = 0;
    frame->title_height = 0;
    frame->title_ready = false;
    frame->button_size = 0;
    frame->buttons_ready = false;
}

void poison_decoration_invalidate_all(struct poison_server *server) {
    if (!server) {
        return;
    }

    struct poison_toplevel *toplevel;
    wl_list_for_each(toplevel, &server->toplevels, link) {
        frame_invalidate(toplevel->frame);
    }

    struct poison_xwayland_view *view;
    wl_list_for_each(view, &server->xwayland_views, link) {
        frame_invalidate(view->frame);
    }

    poison_decoration_update_all(server);
}

void poison_decoration_destroy_toplevel(struct poison_toplevel *toplevel) {
    if (!toplevel || !toplevel->frame) {
        return;
    }
    frame_destroy(toplevel->frame);
    toplevel->frame = NULL;
}

void poison_decoration_destroy_xwayland(struct poison_xwayland_view *view) {
    if (!view || !view->frame) {
        return;
    }
    frame_destroy(view->frame);
    view->frame = NULL;
}

void poison_toplevel_apply_size(struct poison_toplevel *toplevel, int width,
                                int height) {
    if (!toplevel || !toplevel->xdg_toplevel) {
        return;
    }

    struct poison_frame_insets insets = poison_toplevel_insets(toplevel);
    if (width > 0) {
        width -= insets.left + insets.right;
        if (width < 1) {
            width = 1;
        }
    }
    if (height > 0) {
        height -= insets.top + insets.bottom;
        if (height < 1) {
            height = 1;
        }
    }

    wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, width, height);
}

void poison_toplevel_apply_position(struct poison_toplevel *toplevel, int x,
                                    int y) {
    if (!toplevel || !toplevel->xdg_toplevel || !toplevel->scene_tree) {
        return;
    }

    struct poison_frame_insets insets = poison_toplevel_insets(toplevel);
    wlr_scene_node_set_position(&toplevel->scene_tree->node, x + insets.left,
                                y + insets.top);

    poison_decoration_update_toplevel(toplevel);
}

void poison_toplevel_apply_box(struct poison_toplevel *toplevel, int x, int y,
                               int width, int height) {
    if (!toplevel || !toplevel->xdg_toplevel || !toplevel->scene_tree) {
        return;
    }

    poison_toplevel_apply_size(toplevel, width, height);
    poison_toplevel_apply_position(toplevel, x, y);
}

static bool node_is_descendant(struct wlr_scene_node *node,
                               struct wlr_scene_tree *tree) {
    if (!node || !tree) {
        return false;
    }
    for (struct wlr_scene_node *walk = node; walk != NULL;
         walk = walk->parent ? &walk->parent->node : NULL) {
        if (walk == &tree->node) {
            return true;
        }
    }
    return false;
}

struct poison_toplevel *poison_decoration_toplevel_at(
    struct poison_server *server, struct wlr_scene_node *node) {
    if (!server || !node || !decorations_on(server)) {
        return NULL;
    }

    struct poison_toplevel *toplevel;
    wl_list_for_each(toplevel, &server->toplevels, link) {
        if (toplevel->frame &&
            node_is_descendant(node, toplevel->frame->tree)) {
            return toplevel;
        }
    }
    return NULL;
}

struct poison_xwayland_view *poison_decoration_xwayland_at(
    struct poison_server *server, struct wlr_scene_node *node) {
    if (!server || !node || !decorations_on(server)) {
        return NULL;
    }

    struct poison_xwayland_view *view;
    wl_list_for_each(view, &server->xwayland_views, link) {
        if (view->frame && node_is_descendant(node, view->frame->tree)) {
            return view;
        }
    }
    return NULL;
}

void poison_decoration_init(struct poison_server *server) {
    if (!server || !decorations_on(server)) {
        return;
    }
    wlr_log(WLR_INFO,
            "Decorations enabled for floating windows: %dpx border, %dpx title "
            "bar, font '%s'.",
            server->config.decoration_border_width, title_bar_height(server),
            decoration_font(server));
}

void poison_decoration_finish(struct poison_server *server) {
    if (!server) {
        return;
    }

    struct poison_toplevel *toplevel;
    wl_list_for_each(toplevel, &server->toplevels, link) {
        poison_decoration_destroy_toplevel(toplevel);
    }

    struct poison_xwayland_view *view;
    wl_list_for_each(view, &server->xwayland_views, link) {
        poison_decoration_destroy_xwayland(view);
    }
}
