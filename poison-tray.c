
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <cairo/cairo-xcb.h>
#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <xcb/composite.h>
#include <xcb/damage.h>
#include <xcb/randr.h>
#include <xcb/shape.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>

#define FONT_NAME "Poison 12"
#define EMPTY_TEXT "(no tray icons)"
#define DEFAULT_FLASH_MS 2000
#define MAX_FLASH_MS 600000
#define HOVER_GRACE_MS 1000
#define ICON_SIZE 24
#define ICON_SPACING 2
#define TRAY_BORDER 1
#define TRAY_PADDING 3
#define TRAY_MARGIN 5
#define HIDDEN_X 30000
#define TEXT_PADDING 10

#define SYSTEM_TRAY_REQUEST_DOCK 0
#define SYSTEM_TRAY_ORIENTATION_HORZ 0
#define XEMBED_EMBEDDED_NOTIFY 0
#define XEMBED_VERSION 0
#define XEMBED_MAPPED (1u << 0)

enum tray_atom {
    ATOM_SYSTEM_TRAY_OPCODE,
    ATOM_SYSTEM_TRAY_ORIENTATION,
    ATOM_SYSTEM_TRAY_VISUAL,
    ATOM_SYSTEM_TRAY_COLORS,
    ATOM_MANAGER,
    ATOM_XEMBED,
    ATOM_XEMBED_INFO,
    ATOM_NET_WM_NAME,
    ATOM_NET_WM_PID,
    ATOM_NET_WM_WINDOW_TYPE,
    ATOM_NET_WM_WINDOW_TYPE_UTILITY,
    ATOM_UTF8_STRING,
    ATOM_COUNT
};

static const char *const atom_names[ATOM_COUNT] = {
    [ATOM_SYSTEM_TRAY_OPCODE] = "_NET_SYSTEM_TRAY_OPCODE",
    [ATOM_SYSTEM_TRAY_ORIENTATION] = "_NET_SYSTEM_TRAY_ORIENTATION",
    [ATOM_SYSTEM_TRAY_VISUAL] = "_NET_SYSTEM_TRAY_VISUAL",
    [ATOM_SYSTEM_TRAY_COLORS] = "_NET_SYSTEM_TRAY_COLORS",
    [ATOM_MANAGER] = "MANAGER",
    [ATOM_XEMBED] = "_XEMBED",
    [ATOM_XEMBED_INFO] = "_XEMBED_INFO",
    [ATOM_NET_WM_NAME] = "_NET_WM_NAME",
    [ATOM_NET_WM_PID] = "_NET_WM_PID",
    [ATOM_NET_WM_WINDOW_TYPE] = "_NET_WM_WINDOW_TYPE",
    [ATOM_NET_WM_WINDOW_TYPE_UTILITY] = "_NET_WM_WINDOW_TYPE_UTILITY",
    [ATOM_UTF8_STRING] = "UTF8_STRING",
};

struct tray_icon {
    xcb_window_t window;
    xcb_damage_damage_t damage;
    cairo_surface_t *surface;
    xcb_rectangle_t *shape;
    int shape_count;
    bool mapped;
    bool placed;
    int x, y;
    bool unmapping;
    uint16_t unmap_sequence;
};

struct tray {
    xcb_connection_t *conn;
    xcb_screen_t *screen;
    int screen_number;
    xcb_atom_t atoms[ATOM_COUNT];
    xcb_atom_t selection;
    xcb_window_t window;
    xcb_visualtype_t *visual;
    xcb_visualtype_t *argb_visual;
    uint8_t damage_event;
    uint8_t shape_event;
    uint8_t randr_event;
    bool have_shape;
    bool have_xfixes;
    bool have_randr;
    bool have_randr_monitors;
    bool outputs_changed;
    cairo_surface_t *surface;

    struct tray_icon *icons;
    int icon_count;
    int icon_capacity;

    int x, y, width, height;
    int empty_width;
    bool had_icons;
    bool shown;
    bool pinned;
    bool hovered;
    int flash_ms;
    int64_t flash_until;
    bool dirty;
    bool running;
};

static int signal_pipe[2] = {-1, -1};

static void handle_signal(int sig) {
    int saved_errno = errno;
    unsigned char c = (unsigned char)sig;
    ssize_t n = write(signal_pipe[1], &c, 1);
    (void)n;
    errno = saved_errno;
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static xcb_visualtype_t *find_visualtype(xcb_screen_t *screen,
                                         xcb_visualid_t id) {
    xcb_depth_iterator_t depth = xcb_screen_allowed_depths_iterator(screen);
    for (; depth.rem; xcb_depth_next(&depth)) {
        xcb_visualtype_iterator_t visual = xcb_depth_visuals_iterator(depth.data);
        for (; visual.rem; xcb_visualtype_next(&visual)) {
            if (visual.data->visual_id == id) {
                return visual.data;
            }
        }
    }
    return NULL;
}

static xcb_visualtype_t *find_argb_visualtype(xcb_screen_t *screen) {
    xcb_depth_iterator_t depth = xcb_screen_allowed_depths_iterator(screen);
    for (; depth.rem; xcb_depth_next(&depth)) {
        if (depth.data->depth != 32) {
            continue;
        }
        xcb_visualtype_iterator_t visual = xcb_depth_visuals_iterator(depth.data);
        for (; visual.rem; xcb_visualtype_next(&visual)) {
            if (visual.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR) {
                return visual.data;
            }
        }
    }
    return NULL;
}

static xcb_atom_t intern_atom(xcb_connection_t *conn, const char *name) {
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(
        conn, xcb_intern_atom(conn, 0, strlen(name), name), NULL);
    xcb_atom_t atom = reply ? reply->atom : XCB_ATOM_NONE;
    free(reply);
    return atom;
}

static void intern_atoms(struct tray *tray) {
    xcb_intern_atom_cookie_t cookies[ATOM_COUNT];
    for (int i = 0; i < ATOM_COUNT; i++) {
        cookies[i] = xcb_intern_atom(tray->conn, 0, strlen(atom_names[i]),
                                     atom_names[i]);
    }
    for (int i = 0; i < ATOM_COUNT; i++) {
        xcb_intern_atom_reply_t *reply =
            xcb_intern_atom_reply(tray->conn, cookies[i], NULL);
        tray->atoms[i] = reply ? reply->atom : XCB_ATOM_NONE;
        free(reply);
    }

    char name[32];
    snprintf(name, sizeof(name), "_NET_SYSTEM_TRAY_S%d", tray->screen_number);
    tray->selection = intern_atom(tray->conn, name);
}

static bool extension_present(xcb_connection_t *conn, xcb_extension_t *ext,
                              uint8_t *first_event) {
    const xcb_query_extension_reply_t *reply = xcb_get_extension_data(conn, ext);
    if (!reply || !reply->present) {
        return false;
    }
    if (first_event) {
        *first_event = reply->first_event;
    }
    return true;
}

static void init_extensions(struct tray *tray) {
    xcb_connection_t *conn = tray->conn;

    xcb_prefetch_extension_data(conn, &xcb_composite_id);
    xcb_prefetch_extension_data(conn, &xcb_damage_id);
    xcb_prefetch_extension_data(conn, &xcb_shape_id);
    xcb_prefetch_extension_data(conn, &xcb_xfixes_id);
    xcb_prefetch_extension_data(conn, &xcb_randr_id);

    tray->have_shape =
        extension_present(conn, &xcb_shape_id, &tray->shape_event);

    if (extension_present(conn, &xcb_xfixes_id, NULL)) {
        xcb_xfixes_query_version_reply_t *reply = xcb_xfixes_query_version_reply(
            conn, xcb_xfixes_query_version(conn, 5, 0), NULL);
        tray->have_xfixes = reply && reply->major_version >= 2;
        free(reply);
    }

    if (extension_present(conn, &xcb_randr_id, &tray->randr_event)) {
        xcb_randr_query_version_reply_t *reply = xcb_randr_query_version_reply(
            conn, xcb_randr_query_version(conn, 1, 5), NULL);
        if (reply) {
            tray->have_randr =
                reply->major_version > 1 || reply->minor_version >= 2;
            tray->have_randr_monitors =
                reply->major_version > 1 || reply->minor_version >= 5;
        }
        free(reply);
    }

    if (!extension_present(conn, &xcb_composite_id, NULL) ||
        !extension_present(conn, &xcb_damage_id, &tray->damage_event)) {
        return;
    }
    xcb_composite_query_version_reply_t *composite =
        xcb_composite_query_version_reply(
            conn, xcb_composite_query_version(conn, 0, 4), NULL);
    xcb_damage_query_version_reply_t *damage = xcb_damage_query_version_reply(
        conn, xcb_damage_query_version(conn, 1, 1), NULL);
    if (composite && damage &&
        (composite->major_version > 0 || composite->minor_version >= 2)) {
        tray->argb_visual = find_argb_visualtype(tray->screen);
    }
    free(composite);
    free(damage);
}

static int measure_empty_text(void) {
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(surface);
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string(FONT_NAME);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_text(layout, EMPTY_TEXT, -1);

    int width, height;
    pango_layout_get_pixel_size(layout, &width, &height);

    pango_font_description_free(desc);
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    return width + 2 * TEXT_PADDING;
}

static int visible_icon_count(struct tray *tray) {
    int count = 0;
    for (int i = 0; i < tray->icon_count; i++) {
        if (tray->icons[i].mapped) {
            count++;
        }
    }
    return count;
}

static void tray_size(struct tray *tray, int *width, int *height) {
    int inset = TRAY_BORDER + TRAY_PADDING;
    int count = visible_icon_count(tray);
    *height = 2 * inset + ICON_SIZE;
    if (count > 0) {
        *width = 2 * inset + count * ICON_SIZE + (count - 1) * ICON_SPACING;
    } else {
        *width = tray->empty_width;
    }
}

static void tray_corner(struct tray *tray, int *right, int *bottom) {
    xcb_connection_t *conn = tray->conn;
    xcb_window_t root = tray->screen->root;

    if (tray->have_randr_monitors) {
        xcb_query_pointer_cookie_t pointer_cookie = xcb_query_pointer(conn, root);
        xcb_randr_get_monitors_cookie_t monitors_cookie =
            xcb_randr_get_monitors(conn, root, 1);
        xcb_query_pointer_reply_t *pointer =
            xcb_query_pointer_reply(conn, pointer_cookie, NULL);
        xcb_randr_get_monitors_reply_t *reply =
            xcb_randr_get_monitors_reply(conn, monitors_cookie, NULL);
        xcb_randr_monitor_info_t *chosen = NULL;
        if (reply) {
            xcb_randr_monitor_info_iterator_t it =
                xcb_randr_get_monitors_monitors_iterator(reply);
            xcb_randr_monitor_info_t *first = it.rem ? it.data : NULL;
            xcb_randr_monitor_info_t *under_pointer = NULL;
            for (; it.rem; xcb_randr_monitor_info_next(&it)) {
                xcb_randr_monitor_info_t *monitor = it.data;
                if (monitor->primary && !chosen) {
                    chosen = monitor;
                }
                if (pointer && pointer->same_screen && !under_pointer &&
                    pointer->root_x >= monitor->x &&
                    pointer->root_y >= monitor->y &&
                    pointer->root_x < monitor->x + monitor->width &&
                    pointer->root_y < monitor->y + monitor->height) {
                    under_pointer = monitor;
                }
            }
            if (!chosen) {
                chosen = under_pointer ? under_pointer : first;
            }
        }
        if (chosen) {
            *right = chosen->x + chosen->width;
            *bottom = chosen->y + chosen->height;
        }
        free(pointer);
        free(reply);
        if (chosen) {
            return;
        }
    }

    xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(
        conn, xcb_get_geometry(conn, root), NULL);
    if (geometry) {
        *right = geometry->width;
        *bottom = geometry->height;
    } else {
        *right = tray->screen->width_in_pixels;
        *bottom = tray->screen->height_in_pixels;
    }
    free(geometry);
}

static void move_tray(struct tray *tray, int x, int y, int width, int height) {
    tray->x = x;
    tray->y = y;
    tray->width = width;
    tray->height = height;
    uint32_t values[] = {(uint32_t)x, (uint32_t)y, (uint32_t)width,
                         (uint32_t)height};
    xcb_configure_window(tray->conn, tray->window,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                             XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                         values);
    if (tray->surface) {
        cairo_xcb_surface_set_size(tray->surface, width, height);
    }
    tray->dirty = true;
}

static void place_tray(struct tray *tray) {
    int right, bottom;
    tray_corner(tray, &right, &bottom);
    int x = tray->shown ? right - TRAY_MARGIN - tray->width : HIDDEN_X;
    move_tray(tray, x, bottom - TRAY_MARGIN - tray->height, tray->width,
              tray->height);
}

static void update_visibility(struct tray *tray);

static void layout(struct tray *tray) {
    int width, height;
    tray_size(tray, &width, &height);

    int inset = TRAY_BORDER + TRAY_PADDING;
    int x = width - inset - ICON_SIZE;
    for (int i = 0; i < tray->icon_count; i++) {
        struct tray_icon *icon = &tray->icons[i];
        if (!icon->mapped) {
            continue;
        }
        if (!icon->placed || icon->x != x || icon->y != inset) {
            icon->x = x;
            icon->y = inset;
            icon->placed = true;
            uint32_t values[] = {(uint32_t)x, (uint32_t)inset, ICON_SIZE,
                                 ICON_SIZE};
            xcb_configure_window(tray->conn, icon->window,
                                 XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                                     XCB_CONFIG_WINDOW_WIDTH |
                                     XCB_CONFIG_WINDOW_HEIGHT,
                                 values);
        }
        x -= ICON_SIZE + ICON_SPACING;
    }

    if (width != tray->width || height != tray->height) {
        move_tray(tray, tray->x + tray->width - width,
                  tray->y + tray->height - height, width, height);
    }
    tray->dirty = true;

    bool has_icons = visible_icon_count(tray) > 0;
    if (tray->had_icons && !has_icons) {
        tray->pinned = false;
        tray->flash_until = 0;
        update_visibility(tray);
    }
    tray->had_icons = has_icons;
}

static void update_visibility(struct tray *tray) {
    bool show = tray->pinned || tray->flash_until != 0;
    if (show == tray->shown) {
        return;
    }
    tray->shown = show;
    if (show) {
        place_tray(tray);
        uint32_t stack = XCB_STACK_MODE_ABOVE;
        xcb_configure_window(tray->conn, tray->window,
                             XCB_CONFIG_WINDOW_STACK_MODE, &stack);
        xcb_map_window(tray->conn, tray->window);
    } else {
        xcb_unmap_window(tray->conn, tray->window);
        tray->hovered = false;
        place_tray(tray);
    }
}

static void flash(struct tray *tray) {
    if (tray->pinned || tray->flash_ms <= 0) {
        return;
    }
    tray->flash_until = now_ms() + tray->flash_ms;
    update_visibility(tray);
}

static void toggle(struct tray *tray) {
    tray->pinned = !tray->pinned;
    tray->flash_until = 0;
    update_visibility(tray);
}

static void expire_flash(struct tray *tray) {
    if (tray->flash_until == 0) {
        return;
    }
    int64_t now = now_ms();
    if (now < tray->flash_until) {
        return;
    }
    if (tray->hovered) {
        tray->flash_until = now + HOVER_GRACE_MS;
        return;
    }
    tray->flash_until = 0;
    update_visibility(tray);
}

static void update_shape(struct tray *tray, struct tray_icon *icon) {
    free(icon->shape);
    icon->shape = NULL;
    icon->shape_count = -1;
    if (!tray->have_shape) {
        return;
    }
    xcb_shape_get_rectangles_reply_t *reply = xcb_shape_get_rectangles_reply(
        tray->conn,
        xcb_shape_get_rectangles(tray->conn, icon->window, XCB_SHAPE_SK_BOUNDING),
        NULL);
    if (!reply) {
        return;
    }
    int count = xcb_shape_get_rectangles_rectangles_length(reply);
    icon->shape = malloc((size_t)(count > 0 ? count : 1) * sizeof(*icon->shape));
    if (icon->shape) {
        memcpy(icon->shape, xcb_shape_get_rectangles_rectangles(reply),
               (size_t)count * sizeof(*icon->shape));
        icon->shape_count = count;
    }
    free(reply);
}

static void draw(struct tray *tray) {
    tray->dirty = false;
    if (!tray->shown) {
        return;
    }

    cairo_t *cr = cairo_create(tray->surface);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    int visible = 0;
    for (int i = 0; i < tray->icon_count; i++) {
        struct tray_icon *icon = &tray->icons[i];
        if (!icon->mapped) {
            continue;
        }
        visible++;
        if (!icon->surface) {
            continue;
        }
        cairo_save(cr);
        if (icon->shape_count >= 0) {
            for (int j = 0; j < icon->shape_count; j++) {
                xcb_rectangle_t *rect = &icon->shape[j];
                cairo_rectangle(cr, icon->x + rect->x, icon->y + rect->y,
                                rect->width, rect->height);
            }
            cairo_clip(cr);
        }
        cairo_set_source_surface(cr, icon->surface, icon->x, icon->y);
        cairo_paint(cr);
        cairo_restore(cr);
    }

    if (visible == 0) {
        PangoLayout *layout = pango_cairo_create_layout(cr);
        PangoFontDescription *desc =
            pango_font_description_from_string(FONT_NAME);
        pango_layout_set_font_description(layout, desc);
        pango_layout_set_text(layout, EMPTY_TEXT, -1);
        int width, height;
        pango_layout_get_pixel_size(layout, &width, &height);
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_move_to(cr, TEXT_PADDING, (tray->height - height) / 2);
        pango_cairo_show_layout(cr, layout);
        pango_font_description_free(desc);
        g_object_unref(layout);
    }

    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_set_line_width(cr, TRAY_BORDER);
    cairo_rectangle(cr, TRAY_BORDER / 2.0, TRAY_BORDER / 2.0,
                    tray->width - TRAY_BORDER, tray->height - TRAY_BORDER);
    cairo_stroke(cr);

    cairo_destroy(cr);
    cairo_surface_flush(tray->surface);
}

static int find_icon(struct tray *tray, xcb_window_t window) {
    for (int i = 0; i < tray->icon_count; i++) {
        if (tray->icons[i].window == window) {
            return i;
        }
    }
    return -1;
}

static bool xembed_wants_mapped(struct tray *tray, xcb_window_t window) {
    xcb_atom_t info = tray->atoms[ATOM_XEMBED_INFO];
    xcb_get_property_reply_t *reply = xcb_get_property_reply(
        tray->conn, xcb_get_property(tray->conn, 0, window, info, info, 0, 2),
        NULL);
    bool mapped = true;
    if (reply && reply->format == 32 &&
        xcb_get_property_value_length(reply) >= 8) {
        const uint32_t *values = xcb_get_property_value(reply);
        mapped = values[1] & XEMBED_MAPPED;
    }
    free(reply);
    return mapped;
}

static void send_embedded_notify(struct tray *tray, xcb_window_t window) {
    xcb_client_message_event_t event = {0};
    event.response_type = XCB_CLIENT_MESSAGE;
    event.format = 32;
    event.window = window;
    event.type = tray->atoms[ATOM_XEMBED];
    event.data.data32[0] = XCB_CURRENT_TIME;
    event.data.data32[1] = XEMBED_EMBEDDED_NOTIFY;
    event.data.data32[3] = tray->window;
    event.data.data32[4] = XEMBED_VERSION;
    xcb_send_event(tray->conn, 0, window, XCB_EVENT_MASK_NO_EVENT,
                   (const char *)&event);
}

static void send_configure_notify(struct tray *tray, struct tray_icon *icon) {
    xcb_configure_notify_event_t event = {0};
    event.response_type = XCB_CONFIGURE_NOTIFY;
    event.event = icon->window;
    event.window = icon->window;
    event.above_sibling = XCB_NONE;
    event.x = (int16_t)(tray->x + icon->x);
    event.y = (int16_t)(tray->y + icon->y);
    event.width = ICON_SIZE;
    event.height = ICON_SIZE;
    event.border_width = 0;
    event.override_redirect = 0;
    xcb_send_event(tray->conn, 0, icon->window,
                   XCB_EVENT_MASK_STRUCTURE_NOTIFY, (const char *)&event);
}

static void unmap_icon(struct tray *tray, struct tray_icon *icon) {
    xcb_void_cookie_t cookie = xcb_unmap_window(tray->conn, icon->window);
    icon->unmapping = true;
    icon->unmap_sequence = (uint16_t)cookie.sequence;
}

static void set_icon_mapped(struct tray *tray, int index, bool mapped) {
    struct tray_icon *icon = &tray->icons[index];
    if (icon->mapped == mapped) {
        return;
    }
    icon->mapped = mapped;
    layout(tray);
    if (mapped) {
        xcb_map_window(tray->conn, icon->window);
        flash(tray);
        send_configure_notify(tray, icon);
    } else {
        unmap_icon(tray, icon);
    }
}

static void dock(struct tray *tray, xcb_window_t window) {
    xcb_connection_t *conn = tray->conn;
    if (window == XCB_NONE || window == tray->window ||
        window == tray->screen->root || find_icon(tray, window) >= 0) {
        return;
    }

    uint32_t event_mask =
        XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(conn, window, XCB_CW_EVENT_MASK, &event_mask);
    xcb_get_window_attributes_cookie_t attributes_cookie =
        xcb_get_window_attributes(conn, window);
    xcb_get_geometry_cookie_t geometry_cookie = xcb_get_geometry(conn, window);
    xcb_query_tree_cookie_t tree_cookie = xcb_query_tree(conn, window);
    xcb_get_window_attributes_reply_t *attributes =
        xcb_get_window_attributes_reply(conn, attributes_cookie, NULL);
    xcb_get_geometry_reply_t *geometry =
        xcb_get_geometry_reply(conn, geometry_cookie, NULL);
    xcb_query_tree_reply_t *tree = xcb_query_tree_reply(conn, tree_cookie, NULL);
    bool usable = attributes && geometry && tree &&
        tree->parent == tray->screen->root &&
        attributes->_class == XCB_WINDOW_CLASS_INPUT_OUTPUT;
    free(tree);
    if (!usable) {
        if (attributes) {
            event_mask = XCB_EVENT_MASK_NO_EVENT;
            xcb_change_window_attributes(conn, window, XCB_CW_EVENT_MASK,
                                         &event_mask);
        }
        free(attributes);
        free(geometry);
        return;
    }

    if (tray->icon_count == tray->icon_capacity) {
        int capacity = tray->icon_capacity ? 2 * tray->icon_capacity : 8;
        struct tray_icon *icons =
            realloc(tray->icons, (size_t)capacity * sizeof(*icons));
        if (!icons) {
            fprintf(stderr, "poison-tray: out of memory!\n");
            free(attributes);
            free(geometry);
            return;
        }
        tray->icons = icons;
        tray->icon_capacity = capacity;
    }
    struct tray_icon *icon = &tray->icons[tray->icon_count++];
    memset(icon, 0, sizeof(*icon));
    icon->window = window;
    icon->shape_count = -1;

    if (tray->have_xfixes) {
        xcb_xfixes_change_save_set(conn, XCB_SET_MODE_INSERT,
                                   XCB_XFIXES_SAVE_SET_TARGET_ROOT,
                                   XCB_XFIXES_SAVE_SET_MAPPING_UNMAP, window);
    } else {
        xcb_change_save_set(conn, XCB_SET_MODE_INSERT, window);
    }
    if (attributes->map_state != XCB_MAP_STATE_UNMAPPED) {
        unmap_icon(tray, icon);
    }
    xcb_reparent_window(conn, window, tray->window, 0, 0);
    uint32_t values[] = {ICON_SIZE, ICON_SIZE, 0};
    xcb_configure_window(conn, window,
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
                             XCB_CONFIG_WINDOW_BORDER_WIDTH,
                         values);

    xcb_visualtype_t *visual = find_visualtype(tray->screen, attributes->visual);
    if (tray->argb_visual && geometry->depth == 32 && visual) {
        xcb_composite_redirect_window(conn, window,
                                      XCB_COMPOSITE_REDIRECT_MANUAL);
        icon->damage = xcb_generate_id(conn);
        xcb_damage_create(conn, icon->damage, window,
                          XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY);
        icon->surface = cairo_xcb_surface_create(conn, window, visual,
                                                 ICON_SIZE, ICON_SIZE);
        if (tray->have_shape) {
            xcb_shape_select_input(conn, window, 1);
            update_shape(tray, icon);
        }
    }
    free(attributes);
    free(geometry);

    send_embedded_notify(tray, window);
    if (xembed_wants_mapped(tray, window)) {
        set_icon_mapped(tray, tray->icon_count - 1, true);
    }
}

static void undock(struct tray *tray, int index, bool destroyed) {
    struct tray_icon *icon = &tray->icons[index];
    xcb_connection_t *conn = tray->conn;

    if (!destroyed) {
        if (icon->damage) {
            xcb_damage_destroy(conn, icon->damage);
        }
        if (icon->surface) {
            xcb_composite_unredirect_window(conn, icon->window,
                                            XCB_COMPOSITE_REDIRECT_MANUAL);
            if (tray->have_shape) {
                xcb_shape_select_input(conn, icon->window, 0);
            }
        }
        uint32_t event_mask = XCB_EVENT_MASK_NO_EVENT;
        xcb_change_window_attributes(conn, icon->window, XCB_CW_EVENT_MASK,
                                     &event_mask);
        xcb_change_save_set(conn, XCB_SET_MODE_DELETE, icon->window);
    }
    if (icon->surface) {
        cairo_surface_destroy(icon->surface);
    }
    free(icon->shape);

    memmove(icon, icon + 1,
            (size_t)(tray->icon_count - index - 1) * sizeof(*icon));
    tray->icon_count--;
    layout(tray);
}

static void handle_configure_request(struct tray *tray,
                                     xcb_configure_request_event_t *event) {
    int index = find_icon(tray, event->window);
    if (index < 0) {
        uint32_t values[7];
        int n = 0;
        if (event->value_mask & XCB_CONFIG_WINDOW_X) {
            values[n++] = (uint32_t)event->x;
        }
        if (event->value_mask & XCB_CONFIG_WINDOW_Y) {
            values[n++] = (uint32_t)event->y;
        }
        if (event->value_mask & XCB_CONFIG_WINDOW_WIDTH) {
            values[n++] = event->width;
        }
        if (event->value_mask & XCB_CONFIG_WINDOW_HEIGHT) {
            values[n++] = event->height;
        }
        if (event->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) {
            values[n++] = event->border_width;
        }
        if (event->value_mask & XCB_CONFIG_WINDOW_SIBLING) {
            values[n++] = event->sibling;
        }
        if (event->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) {
            values[n++] = event->stack_mode;
        }
        xcb_configure_window(tray->conn, event->window, event->value_mask,
                             values);
        return;
    }

    struct tray_icon *icon = &tray->icons[index];
    uint32_t values[] = {(uint32_t)icon->x, (uint32_t)icon->y, ICON_SIZE,
                         ICON_SIZE, 0};
    xcb_configure_window(tray->conn, icon->window,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                             XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
                             XCB_CONFIG_WINDOW_BORDER_WIDTH,
                         values);
    send_configure_notify(tray, icon);
}

static void handle_event(struct tray *tray, xcb_generic_event_t *event) {
    uint8_t type = event->response_type & ~0x80;

    if (tray->argb_visual && type == tray->damage_event + XCB_DAMAGE_NOTIFY) {
        xcb_damage_notify_event_t *damage = (xcb_damage_notify_event_t *)event;
        xcb_damage_subtract(tray->conn, damage->damage, XCB_NONE, XCB_NONE);
        int index = find_icon(tray, damage->drawable);
        if (index >= 0 && tray->icons[index].surface) {
            cairo_surface_mark_dirty(tray->icons[index].surface);
        }
        tray->dirty = true;
        return;
    }

    if (tray->have_randr &&
        (type == tray->randr_event + XCB_RANDR_SCREEN_CHANGE_NOTIFY ||
         type == tray->randr_event + XCB_RANDR_NOTIFY)) {
        tray->outputs_changed = true;
        return;
    }

    if (tray->have_shape && type == tray->shape_event + XCB_SHAPE_NOTIFY) {
        xcb_shape_notify_event_t *shape = (xcb_shape_notify_event_t *)event;
        int index = find_icon(tray, shape->affected_window);
        if (index >= 0 && shape->shape_kind == XCB_SHAPE_SK_BOUNDING &&
            tray->icons[index].surface) {
            update_shape(tray, &tray->icons[index]);
            tray->dirty = true;
        }
        return;
    }

    switch (type) {
    case 0:
        break;
    case XCB_CLIENT_MESSAGE: {
        xcb_client_message_event_t *message =
            (xcb_client_message_event_t *)event;
        if (message->window == tray->window && message->format == 32 &&
            message->type == tray->atoms[ATOM_SYSTEM_TRAY_OPCODE] &&
            message->data.data32[1] == SYSTEM_TRAY_REQUEST_DOCK) {
            dock(tray, message->data.data32[2]);
        }
        break;
    }
    case XCB_SELECTION_CLEAR: {
        xcb_selection_clear_event_t *clear = (xcb_selection_clear_event_t *)event;
        if (clear->selection == tray->selection) {
            fprintf(stderr, "poison-tray: another system tray took over!\n");
            tray->running = false;
        }
        break;
    }
    case XCB_EXPOSE: {
        xcb_expose_event_t *expose = (xcb_expose_event_t *)event;
        if (expose->window == tray->window) {
            tray->dirty = true;
        }
        break;
    }
    case XCB_ENTER_NOTIFY: {
        xcb_enter_notify_event_t *enter = (xcb_enter_notify_event_t *)event;
        if (enter->event == tray->window &&
            enter->detail != XCB_NOTIFY_DETAIL_INFERIOR) {
            tray->hovered = true;
        }
        break;
    }
    case XCB_LEAVE_NOTIFY: {
        xcb_leave_notify_event_t *leave = (xcb_leave_notify_event_t *)event;
        if (leave->event == tray->window &&
            leave->detail != XCB_NOTIFY_DETAIL_INFERIOR) {
            tray->hovered = false;
        }
        break;
    }
    case XCB_DESTROY_NOTIFY: {
        xcb_destroy_notify_event_t *destroy = (xcb_destroy_notify_event_t *)event;
        int index = find_icon(tray, destroy->window);
        if (index >= 0) {
            undock(tray, index, true);
        }
        break;
    }
    case XCB_REPARENT_NOTIFY: {
        xcb_reparent_notify_event_t *reparent =
            (xcb_reparent_notify_event_t *)event;
        int index = find_icon(tray, reparent->window);
        if (index >= 0 && reparent->parent != tray->window) {
            undock(tray, index, false);
        }
        break;
    }
    case XCB_UNMAP_NOTIFY: {
        xcb_unmap_notify_event_t *unmap = (xcb_unmap_notify_event_t *)event;
        int index = find_icon(tray, unmap->window);
        if (index < 0) {
            break;
        }
        struct tray_icon *icon = &tray->icons[index];
        if (icon->unmapping && unmap->sequence == icon->unmap_sequence) {
            icon->unmapping = false;
        } else if (icon->mapped) {
            icon->mapped = false;
            layout(tray);
        }
        break;
    }
    case XCB_MAP_REQUEST: {
        xcb_map_request_event_t *request = (xcb_map_request_event_t *)event;
        int index = find_icon(tray, request->window);
        if (index >= 0) {
            set_icon_mapped(tray, index, true);
        }
        xcb_map_window(tray->conn, request->window);
        break;
    }
    case XCB_CONFIGURE_REQUEST:
        handle_configure_request(tray, (xcb_configure_request_event_t *)event);
        break;
    case XCB_PROPERTY_NOTIFY: {
        xcb_property_notify_event_t *property =
            (xcb_property_notify_event_t *)event;
        if (property->atom != tray->atoms[ATOM_XEMBED_INFO]) {
            break;
        }
        int index = find_icon(tray, property->window);
        if (index >= 0) {
            set_icon_mapped(tray, index,
                            xembed_wants_mapped(tray, property->window));
        }
        break;
    }
    default:
        break;
    }
}

static void set_string_property(struct tray *tray, xcb_atom_t property,
                                xcb_atom_t type, const char *value,
                                size_t length) {
    xcb_change_property(tray->conn, XCB_PROP_MODE_REPLACE, tray->window,
                        property, type, 8, (uint32_t)length, value);
}

static bool create_window(struct tray *tray) {
    xcb_connection_t *conn = tray->conn;
    xcb_screen_t *screen = tray->screen;

    tray->visual = find_visualtype(screen, screen->root_visual);
    if (!tray->visual) {
        fprintf(stderr, "poison-tray: no visual for the root window!\n");
        return false;
    }

    int width, height;
    tray_size(tray, &width, &height);
    tray->width = width;
    tray->height = height;

    tray->window = xcb_generate_id(conn);
    uint32_t values[] = {
        screen->black_pixel,
        1,
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_ENTER_WINDOW |
            XCB_EVENT_MASK_LEAVE_WINDOW |
            XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
    };
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, tray->window, screen->root,
                      0, 0, (uint16_t)width, (uint16_t)height, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT |
                          XCB_CW_EVENT_MASK,
                      values);

    static const char name[] = "poison-tray";
    static const char class[] = "poison-tray\0poison-tray";
    set_string_property(tray, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, name,
                        sizeof(name) - 1);
    set_string_property(tray, tray->atoms[ATOM_NET_WM_NAME],
                        tray->atoms[ATOM_UTF8_STRING], name, sizeof(name) - 1);
    set_string_property(tray, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, class,
                        sizeof(class));

    uint32_t pid = (uint32_t)getpid();
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, tray->window,
                        tray->atoms[ATOM_NET_WM_PID], XCB_ATOM_CARDINAL, 32, 1,
                        &pid);
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, tray->window,
                        tray->atoms[ATOM_NET_WM_WINDOW_TYPE], XCB_ATOM_ATOM,
                        32, 1, &tray->atoms[ATOM_NET_WM_WINDOW_TYPE_UTILITY]);

    uint32_t orientation = SYSTEM_TRAY_ORIENTATION_HORZ;
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, tray->window,
                        tray->atoms[ATOM_SYSTEM_TRAY_ORIENTATION],
                        XCB_ATOM_CARDINAL, 32, 1, &orientation);
    static const uint32_t colors[4][3] = {
        {0xffff, 0xffff, 0xffff},
        {0xcccc, 0x0000, 0x0000},
        {0xf5f5, 0x7979, 0x3e3e},
        {0x4e4e, 0x9a9a, 0x0606},
    };
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, tray->window,
                        tray->atoms[ATOM_SYSTEM_TRAY_COLORS], XCB_ATOM_CARDINAL,
                        32, 4 * 3, colors);
    if (tray->argb_visual) {
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, tray->window,
                            tray->atoms[ATOM_SYSTEM_TRAY_VISUAL],
                            XCB_ATOM_VISUALID, 32, 1,
                            &tray->argb_visual->visual_id);
    }

    tray->surface = cairo_xcb_surface_create(conn, tray->window, tray->visual,
                                             width, height);

    place_tray(tray);
    if (tray->have_randr) {
        xcb_randr_select_input(conn, screen->root,
                               XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE |
                                   XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE);
    }
    return true;
}

static bool acquire_selection(struct tray *tray) {
    xcb_connection_t *conn = tray->conn;

    xcb_get_selection_owner_reply_t *owner = xcb_get_selection_owner_reply(
        conn, xcb_get_selection_owner(conn, tray->selection), NULL);
    bool taken = owner && owner->owner != XCB_NONE;
    free(owner);
    if (taken) {
        fprintf(stderr, "poison-tray: another system tray is running!\n");
        return false;
    }

    xcb_set_selection_owner(conn, tray->window, tray->selection,
                            XCB_CURRENT_TIME);
    owner = xcb_get_selection_owner_reply(
        conn, xcb_get_selection_owner(conn, tray->selection), NULL);
    bool ours = owner && owner->owner == tray->window;
    free(owner);
    if (!ours) {
        fprintf(stderr, "poison-tray: failed to become the system tray!\n");
        return false;
    }

    xcb_client_message_event_t event = {0};
    event.response_type = XCB_CLIENT_MESSAGE;
    event.format = 32;
    event.window = tray->screen->root;
    event.type = tray->atoms[ATOM_MANAGER];
    event.data.data32[0] = XCB_CURRENT_TIME;
    event.data.data32[1] = tray->selection;
    event.data.data32[2] = tray->window;
    xcb_send_event(conn, 0, tray->screen->root,
                   XCB_EVENT_MASK_STRUCTURE_NOTIFY, (const char *)&event);
    xcb_flush(conn);
    return true;
}

static bool setup_signals(void) {
    if (pipe(signal_pipe) == -1) {
        return false;
    }
    for (int i = 0; i < 2; i++) {
        fcntl(signal_pipe[i], F_SETFD, FD_CLOEXEC);
        int flags = fcntl(signal_pipe[i], F_GETFL, 0);
        if (flags >= 0) {
            fcntl(signal_pipe[i], F_SETFL, flags | O_NONBLOCK);
        }
    }

    struct sigaction sa = {0};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    return true;
}

static void handle_signals(struct tray *tray) {
    unsigned char sig;
    while (read(signal_pipe[0], &sig, 1) == 1) {
        if (sig == SIGUSR1) {
            toggle(tray);
        } else {
            tray->running = false;
        }
    }
}

static void run(struct tray *tray) {
    int xcb_fd = xcb_get_file_descriptor(tray->conn);

    while (tray->running) {
        xcb_generic_event_t *event;
        while ((event = xcb_poll_for_event(tray->conn)) != NULL) {
            handle_event(tray, event);
            free(event);
        }
        if (xcb_connection_has_error(tray->conn)) {
            break;
        }

        handle_signals(tray);
        expire_flash(tray);
        if (tray->outputs_changed) {
            tray->outputs_changed = false;
            place_tray(tray);
        }
        if (tray->dirty) {
            draw(tray);
        }
        if (xcb_flush(tray->conn) <= 0) {
            break;
        }

        if ((event = xcb_poll_for_queued_event(tray->conn)) != NULL) {
            handle_event(tray, event);
            free(event);
            continue;
        }
        if (!tray->running) {
            break;
        }

        int timeout = -1;
        if (tray->flash_until != 0) {
            int64_t left = tray->flash_until - now_ms();
            timeout = left > 0 ? (int)left : 0;
        }
        struct pollfd fds[2] = {
            {.fd = xcb_fd, .events = POLLIN},
            {.fd = signal_pipe[0], .events = POLLIN},
        };
        if (poll(fds, 2, timeout) == -1 && errno != EINTR) {
            break;
        }
    }
}

int main(int argc, char *argv[]) {
    struct tray tray = {0};
    tray.flash_ms = DEFAULT_FLASH_MS;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--show") == 0) {
            tray.pinned = true;
            continue;
        }
        char *end;
        errno = 0;
        long flash_ms = strtol(argv[i], &end, 10);
        if (end == argv[i] || *end != '\0' || flash_ms < 0) {
            fprintf(stderr, "Usage: poison-tray [--show] [flash_ms]\n");
            return 1;
        }
        tray.flash_ms =
            errno || flash_ms > MAX_FLASH_MS ? MAX_FLASH_MS : (int)flash_ms;
    }

    if (!setup_signals()) {
        fprintf(stderr, "poison-tray: failed to set up signals!\n");
        return 1;
    }

    tray.conn = xcb_connect(NULL, &tray.screen_number);
    if (xcb_connection_has_error(tray.conn)) {
        fprintf(stderr, "poison-tray: failed to connect to X!\n");
        xcb_disconnect(tray.conn);
        return 1;
    }
    xcb_screen_iterator_t screens =
        xcb_setup_roots_iterator(xcb_get_setup(tray.conn));
    for (int i = 0; i < tray.screen_number && screens.rem; i++) {
        xcb_screen_next(&screens);
    }
    tray.screen = screens.data;
    if (!tray.screen) {
        fprintf(stderr, "poison-tray: no X screen!\n");
        xcb_disconnect(tray.conn);
        return 1;
    }

    intern_atoms(&tray);
    init_extensions(&tray);
    tray.empty_width = measure_empty_text();

    int exit_code = 1;
    if (create_window(&tray) && acquire_selection(&tray)) {
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGUSR1);
        sigaddset(&mask, SIGTERM);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGHUP);
        sigprocmask(SIG_UNBLOCK, &mask, NULL);

        update_visibility(&tray);
        tray.running = true;
        run(&tray);
        exit_code = 0;
    }

    for (int i = 0; i < tray.icon_count; i++) {
        if (tray.icons[i].surface) {
            cairo_surface_destroy(tray.icons[i].surface);
        }
        free(tray.icons[i].shape);
    }
    free(tray.icons);
    if (tray.surface) {
        cairo_device_t *device = cairo_surface_get_device(tray.surface);
        if (device) {
            cairo_device_finish(device);
        }
        cairo_surface_destroy(tray.surface);
    }
    xcb_disconnect(tray.conn);
    return exit_code;
}
