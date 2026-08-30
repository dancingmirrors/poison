/* © 2026 dancingmirrors */

#include "poison.h"

struct hsplit_entry {
    struct poison_toplevel *xdg;
    struct poison_xwayland_view *xw;
    int order;
};

static int hsplit_focused_index(struct poison_server *server);
static void hsplit_focus_at(struct poison_server *server, int idx);

static void hsplit_forget_displaced(struct poison_toplevel *t) {
    t->hsplit_displaced_toplevel = NULL;
    t->hsplit_displaced_xwayland = NULL;
}
static void hsplit_forget_displaced_xw(struct poison_xwayland_view *xw) {
    xw->hsplit_displaced_toplevel = NULL;
    xw->hsplit_displaced_xwayland = NULL;
}

static bool hsplit_candidate(struct poison_view_order *node,
                             struct poison_toplevel **out_xdg,
                             struct poison_xwayland_view **out_xw) {
    *out_xdg = NULL;
    *out_xw = NULL;
    if (node->type == POISON_VIEW_XDG) {
        struct poison_toplevel *t = wl_container_of(node, t, order);
        if (t->in_hsplit || !t->xdg_toplevel || t->xdg_toplevel->parent) {
            return false;
        }
        *out_xdg = t;
        return true;
    }
    struct poison_xwayland_view *xw = wl_container_of(node, xw, order);
    if (xw->in_hsplit || !xw->xwayland_surface ||
        xw->xwayland_surface->parent || xw->xwayland_surface->modal) {
        return false;
    }
    *out_xw = xw;
    return true;
}

static void hsplit_sync_view_order(struct poison_server *server,
                                   struct hsplit_entry *wins, int n) {
    if (n < 2) {
        return;
    }
    struct poison_view_order **panes = calloc(n, sizeof(*panes));
    if (!panes) {
        return;
    }

    for (int i = 0; i < n; i++) {
        panes[i] = wins[i].xdg ? &wins[i].xdg->order : &wins[i].xw->order;
    }

    view_order_arrange(server, panes, n);

    free(panes);
}

struct wl_list *hsplit_evict(struct poison_toplevel *old) {
    struct poison_server *server = old->server;
    struct wl_list *pos = old->hsplit_link.prev;

    old->in_hsplit = false;
    wl_list_remove(&old->hsplit_link);
    wl_list_init(&old->hsplit_link);

    if (restore_toplevel_fullscreen_from_hsplit(old)) {
        return pos;
    }

    struct wlr_box output_box;
    wlr_output_layout_get_box(server->output_layout, NULL, &output_box);
    int effective_padding = server->config.padding == 0 ? 1 : server->config.padding;
    if (output_box.width > 2 * effective_padding &&
        output_box.height > 2 * effective_padding) {
        wlr_xdg_toplevel_set_size(old->xdg_toplevel,
                                  output_box.width - 2 * effective_padding,
                                  output_box.height - 2 * effective_padding);
    }
    wlr_xdg_toplevel_set_tiled(old->xdg_toplevel,
                               WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
    return pos;
}

struct wl_list *hsplit_evict_xwayland(struct poison_xwayland_view *old) {
    struct poison_server *server = old->server;
    struct wl_list *pos = old->hsplit_link.prev;

    old->in_hsplit = false;
    wl_list_remove(&old->hsplit_link);
    wl_list_init(&old->hsplit_link);

    if (restore_xwayland_fullscreen_from_hsplit(old)) {
        return pos;
    }

    struct wlr_box output_box;
    wlr_output_layout_get_box(server->output_layout, NULL, &output_box);
    int effective_padding = server->config.padding == 0 ? 1 : server->config.padding;
    if (output_box.width > 2 * effective_padding &&
        output_box.height > 2 * effective_padding) {
        wlr_xwayland_surface_configure(old->xwayland_surface,
                                       output_box.x + effective_padding, output_box.y + effective_padding,
                                       output_box.width - 2 * effective_padding,
                                       output_box.height - 2 * effective_padding);
        struct wlr_box tile = {
            .x = output_box.x + effective_padding,
            .y = output_box.y + effective_padding,
            .width = output_box.width - 2 * effective_padding,
            .height = output_box.height - 2 * effective_padding};
        set_xwayland_view_clip(old, &tile);
    }
    return pos;
}

void hsplit_place(struct poison_toplevel *toplevel, struct wl_list *pos) {
    bool arrived_fullscreen = toplevel->xdg_toplevel->base->surface->mapped
        ? toplevel_fullscreen_now(toplevel)
        : toplevel->xdg_toplevel->requested.fullscreen;

    if (arrived_fullscreen) {
        toplevel->pre_hsplit_fullscreen = true;
        wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, false);
    }
    /* So fullscreen windows don't escape the split. */
    wlr_xdg_toplevel_set_tiled(toplevel->xdg_toplevel,
                               WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
    if (toplevel->floating) {
        toplevel->pre_hsplit_floating = true;
    }
    if (toplevel->floating && toplevel->xdg_toplevel->base &&
        (toplevel->saved_float_width <= 0 || toplevel->saved_float_height <= 0)) {
        struct wlr_box geo = toplevel->xdg_toplevel->base->geometry;
        if (geo.width > 0 && geo.height > 0) {
            toplevel->saved_float_width = geo.width;
            toplevel->saved_float_height = geo.height;
        }
    }
    toplevel->floating = false;
    toplevel->in_hsplit = true;
    wl_list_insert(pos, &toplevel->hsplit_link);
}

void hsplit_place_xwayland(struct poison_xwayland_view *xw,
                           struct wl_list *pos) {
    if (xw->xwayland_surface && xw->xwayland_surface->fullscreen) {
        xw->pre_hsplit_fullscreen = true;
        wlr_xwayland_surface_set_fullscreen(xw->xwayland_surface, false);
        xw->floating = xw->pre_fullscreen_floating;
    }
    if (xw->floating) {
        xw->pre_hsplit_floating = true;
    }
    if (xw->floating && xw->xwayland_surface &&
        (xw->saved_float_width <= 0 || xw->saved_float_height <= 0) &&
        xw->xwayland_surface->width > 0 && xw->xwayland_surface->height > 0) {
        xw->saved_float_width = xw->xwayland_surface->width;
        xw->saved_float_height = xw->xwayland_surface->height;
    }
    xw->floating = false;
    xw->in_hsplit = true;
    wl_list_insert(pos, &xw->hsplit_link);
}

void apply_hsplit_layout(struct poison_server *server) {
    int count_xdg = wl_list_length(&server->hsplit_toplevels);
    int count_xw = wl_list_length(&server->hsplit_xwayland_views);
    int count = count_xdg + count_xw;
    if (count == 0) {
        return;
    }

    struct wlr_box output_box;
    wlr_output_layout_get_box(server->output_layout, NULL, &output_box);
    if (output_box.width <= 0 || output_box.height <= 0) {
        return;
    }

    int effective_padding = server->config.padding == 0 ? 1 : server->config.padding;
    int total_width = output_box.width - 2 * effective_padding;
    int height = output_box.height - 2 * effective_padding;
    if (total_width <= 0 || height <= 0) {
        return;
    }

    int n_slots = count < 2 ? 2 : count;
    int total_gap = (n_slots - 1) * HSPLIT_GAP;
    int slot_width = (total_width - total_gap) / n_slots;
    if (slot_width <= 0) {
        return;
    }

    struct hsplit_entry *wins = calloc(count, sizeof(*wins));
    if (!wins) {
        return;
    }
    int n = 0;

    struct poison_toplevel *t;
    wl_list_for_each(t, &server->hsplit_toplevels, hsplit_link) {
        wins[n].xdg = t;
        wins[n].xw = NULL;
        wins[n].order = t->hsplit_order;
        n++;
    }
    struct poison_xwayland_view *xw;
    wl_list_for_each(xw, &server->hsplit_xwayland_views, hsplit_link) {
        wins[n].xdg = NULL;
        wins[n].xw = xw;
        wins[n].order = xw->hsplit_order;
        n++;
    }

    for (int i = 1; i < n; i++) {
        struct hsplit_entry key = wins[i];
        int j = i - 1;
        while (j >= 0 && wins[j].order > key.order) {
            wins[j + 1] = wins[j];
            j--;
        }
        wins[j + 1] = key;
    }

    for (int i = 0; i < n; i++) {
        int x = output_box.x + effective_padding + i * (slot_width + HSPLIT_GAP);
        int y = output_box.y + effective_padding;
        if (wins[i].xdg) {
            struct poison_toplevel *tl = wins[i].xdg;
            wlr_xdg_toplevel_set_size(tl->xdg_toplevel, slot_width, height);
            wlr_scene_node_set_position(&tl->scene_tree->node, x, y);
            wlr_scene_node_raise_to_top(&tl->scene_tree->node);
        } else {
            struct poison_xwayland_view *xwv = wins[i].xw;
            wlr_xwayland_surface_configure(xwv->xwayland_surface, x, y,
                                           slot_width, height);
            if (xwv->scene_tree) {
                wlr_scene_node_set_position(&xwv->scene_tree->node, x, y);
                wlr_scene_node_raise_to_top(&xwv->scene_tree->node);
            }
            struct wlr_box slot = {.x = x, .y = y, .width = slot_width, .height = height};
            set_xwayland_view_clip(xwv, &slot);
        }
    }

    wl_list_init(&server->hsplit_toplevels);
    wl_list_init(&server->hsplit_xwayland_views);
    for (int i = 0; i < n; i++) {
        if (wins[i].xdg) {
            wl_list_insert(server->hsplit_toplevels.prev,
                           &wins[i].xdg->hsplit_link);
        } else {
            wl_list_insert(server->hsplit_xwayland_views.prev,
                           &wins[i].xw->hsplit_link);
        }
    }

    hsplit_sync_view_order(server, wins, n);

    free(wins);
}

void hsplit_restore_focus(struct poison_server *server) {
    if (server->prev_focused_toplevel &&
        server->prev_focused_toplevel->in_hsplit) {
        struct poison_toplevel *t;
        wl_list_for_each(t, &server->hsplit_toplevels, hsplit_link) {
            if (t == server->prev_focused_toplevel) {
                focus_toplevel(t);
                return;
            }
        }
    }

    if (server->prev_focused_xwayland_view &&
        server->prev_focused_xwayland_view->in_hsplit) {
        struct poison_xwayland_view *xw;
        wl_list_for_each(xw, &server->hsplit_xwayland_views, hsplit_link) {
            if (xw == server->prev_focused_xwayland_view) {
                focus_xwayland_view(server, xw);
                return;
            }
        }
    }

    hsplit_focus_at(server, 0);
}

void hsplit_update_after_removal(struct poison_server *server) {
    int xdg_count = wl_list_length(&server->hsplit_toplevels);
    int xw_count = wl_list_length(&server->hsplit_xwayland_views);
    int count = xdg_count + xw_count;
    if (count == 0) {
        server->hsplit_active = false;
        server->hsplit_focused_slot = -1;
        return;
    }

    if (count == 1) {
        struct wlr_box output_box;
        wlr_output_layout_get_box(server->output_layout, NULL, &output_box);
        int ep = server->config.padding == 0 ? 1 : server->config.padding;

        if (xdg_count == 1) {
            struct poison_toplevel *t =
                wl_container_of(server->hsplit_toplevels.next, t, hsplit_link);
            t->in_hsplit = false;
            hsplit_forget_displaced(t);
            wl_list_remove(&t->hsplit_link);
            wl_list_init(&t->hsplit_link);
            server->prev_focused_toplevel = t;
            server->prev_focused_xwayland_view = NULL;
            if (t->scene_tree) {
                wlr_scene_node_raise_to_top(&t->scene_tree->node);
            }
            if (restore_toplevel_fullscreen_from_hsplit(t)) {
            } else if (t->pre_hsplit_floating) {
                restore_toplevel_from_hsplit(t);
            } else {
                t->floating = false;
                wlr_xdg_toplevel_set_tiled(t->xdg_toplevel,
                                           WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
                if (output_box.width > 2 * ep && output_box.height > 2 * ep) {
                    wlr_xdg_toplevel_set_size(t->xdg_toplevel,
                                              output_box.width - 2 * ep, output_box.height - 2 * ep);
                }
                wlr_scene_node_set_position(&t->scene_tree->node,
                                            output_box.x + ep, output_box.y + ep);
            }
        } else {
            struct poison_xwayland_view *xw = wl_container_of(
                server->hsplit_xwayland_views.next, xw, hsplit_link);
            xw->in_hsplit = false;
            hsplit_forget_displaced_xw(xw);
            wl_list_remove(&xw->hsplit_link);
            wl_list_init(&xw->hsplit_link);
            server->prev_focused_xwayland_view = xw;
            server->prev_focused_toplevel = NULL;
            if (xw->scene_tree) {
                wlr_scene_node_raise_to_top(&xw->scene_tree->node);
            }
            if (restore_xwayland_fullscreen_from_hsplit(xw)) {
            } else if (xw->pre_hsplit_floating) {
                restore_xwayland_from_hsplit(xw);
            } else {
                xw->floating = false;
                if (output_box.width > 2 * ep && output_box.height > 2 * ep) {
                    int w = output_box.width - 2 * ep;
                    int h = output_box.height - 2 * ep;
                    int x = output_box.x + ep;
                    int y = output_box.y + ep;
                    wlr_xwayland_surface_configure(xw->xwayland_surface,
                                                   x, y, w, h);
                    if (xw->scene_tree) {
                        wlr_scene_node_set_position(&xw->scene_tree->node, x, y);
                    }
                    struct wlr_box tile = {.x = x, .y = y, .width = w, .height = h};
                    set_xwayland_view_clip(xw, &tile);
                }
            }
        }
        server->hsplit_active = false;
        server->hsplit_focused_slot = -1;
        return;
    }

    struct hsplit_entry *wins = calloc(count, sizeof(*wins));
    if (!wins) {
        return;
    }
    int n = 0;
    struct poison_toplevel *t;
    wl_list_for_each(t, &server->hsplit_toplevels, hsplit_link) {
        wins[n].xdg = t;
        wins[n].xw = NULL;
        wins[n].order = t->hsplit_order;
        n++;
    }
    struct poison_xwayland_view *xw;
    wl_list_for_each(xw, &server->hsplit_xwayland_views, hsplit_link) {
        wins[n].xdg = NULL;
        wins[n].xw = xw;
        wins[n].order = xw->hsplit_order;
        n++;
    }

    for (int i = 1; i < n; i++) {
        struct hsplit_entry key = wins[i];
        int j = i - 1;
        while (j >= 0 && wins[j].order > key.order) {
            wins[j + 1] = wins[j];
            j--;
        }
        wins[j + 1] = key;
    }
    for (int i = 0; i < n; i++) {
        if (wins[i].xdg) {
            wins[i].xdg->hsplit_order = i;
        } else {
            wins[i].xw->hsplit_order = i;
        }
    }

    free(wins);

    /* Allow a split even with one window a la Ratpoison. */
    apply_hsplit_layout(server);
}

void toggle_hsplit(struct poison_server *server) {
    if (server->hsplit_active) {
        struct wlr_box output_box;
        wlr_output_layout_get_box(server->output_layout, NULL, &output_box);
        int effective_padding = server->config.padding == 0 ? 1 : server->config.padding;

        struct poison_toplevel *t, *tmp;
        wl_list_for_each_safe(t, tmp, &server->hsplit_toplevels, hsplit_link) {
            t->in_hsplit = false;
            hsplit_forget_displaced(t);
            wl_list_remove(&t->hsplit_link);
            wl_list_init(&t->hsplit_link);
            if (restore_toplevel_fullscreen_from_hsplit(t)) {
                continue;
            }
            if (t->pre_hsplit_floating) {
                restore_toplevel_from_hsplit(t);
                continue;
            }
            wlr_xdg_toplevel_set_tiled(t->xdg_toplevel,
                                       WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
            if (output_box.width > 2 * effective_padding &&
                output_box.height > 2 * effective_padding) {
                wlr_xdg_toplevel_set_size(t->xdg_toplevel,
                                          output_box.width - 2 * effective_padding,
                                          output_box.height - 2 * effective_padding);
            }
        }
        struct poison_xwayland_view *xw, *xwtmp;
        wl_list_for_each_safe(xw, xwtmp, &server->hsplit_xwayland_views, hsplit_link) {
            xw->in_hsplit = false;
            hsplit_forget_displaced_xw(xw);
            wl_list_remove(&xw->hsplit_link);
            wl_list_init(&xw->hsplit_link);
            if (restore_xwayland_fullscreen_from_hsplit(xw)) {
                continue;
            }
            if (xw->pre_hsplit_floating) {
                restore_xwayland_from_hsplit(xw);
                continue;
            }
            if (output_box.width > 2 * effective_padding &&
                output_box.height > 2 * effective_padding) {
                int w = output_box.width - 2 * effective_padding;
                int h = output_box.height - 2 * effective_padding;
                wlr_xwayland_surface_configure(xw->xwayland_surface,
                                               output_box.x + effective_padding, output_box.y + effective_padding,
                                               w, h);
                if (xw->scene_tree) {
                    wlr_scene_node_set_position(&xw->scene_tree->node,
                                                output_box.x + effective_padding,
                                                output_box.y + effective_padding);
                }
                struct wlr_box tile = {
                    .x = output_box.x + effective_padding,
                    .y = output_box.y + effective_padding,
                    .width = w,
                    .height = h};
                set_xwayland_view_clip(xw, &tile);
            }
        }
        server->hsplit_active = false;
        server->hsplit_focused_slot = -1;
        if (server->focused_toplevel) {
            focus_toplevel(server->focused_toplevel);
        } else if (server->focused_xwayland_view) {
            focus_xwayland_view(server, server->focused_xwayland_view);
        }
    } else {
        struct poison_toplevel *first = NULL, *second = NULL;
        struct poison_xwayland_view *first_xw = NULL, *second_xw = NULL;

        struct wl_list *start = NULL;
        struct poison_view_order *node;

        struct poison_toplevel *cur = server->focused_toplevel;
        if (cur && cur->xdg_toplevel && !cur->xdg_toplevel->parent &&
            !cur->in_hsplit) {
            first = cur;
            start = &cur->order.link;
        } else if (server->focused_xwayland_view &&
                   !server->focused_xwayland_view->in_hsplit) {
            first_xw = server->focused_xwayland_view;
            start = &server->focused_xwayland_view->order.link;
        } else {
            wl_list_for_each(node, &server->view_order, link) {
                if (hsplit_candidate(node, &first, &first_xw)) {
                    start = &node->link;
                    break;
                }
            }
        }

        if (start) {
            for (struct wl_list *l = start->next; l != start; l = l->next) {
                if (l == &server->view_order) {
                    continue;
                }
                node = wl_container_of(l, node, link);
                if (hsplit_candidate(node, &second, &second_xw)) {
                    break;
                }
            }
        }

        if (first) {
            first->hsplit_order = 0;
            hsplit_place(first, server->hsplit_toplevels.prev);
        } else if (first_xw) {
            first_xw->hsplit_order = 0;
            hsplit_place_xwayland(first_xw, server->hsplit_xwayland_views.prev);
        }
        if (second) {
            second->hsplit_order = 1;
            hsplit_place(second, server->hsplit_toplevels.prev);
        } else if (second_xw) {
            second_xw->hsplit_order = 1;
            hsplit_place_xwayland(second_xw, server->hsplit_xwayland_views.prev);
        }

        int total = wl_list_length(&server->hsplit_toplevels) +
            wl_list_length(&server->hsplit_xwayland_views);
        if (total > 0) {
            server->hsplit_active = true;
            apply_hsplit_layout(server);
            if (!(server->focused_toplevel &&
                  server->focused_toplevel->in_hsplit) &&
                !(server->focused_xwayland_view &&
                  server->focused_xwayland_view->in_hsplit)) {
                hsplit_focus_at(server, 0);
            }
        }
    }
}

static int hsplit_focused_index(struct poison_server *server) {
    if (server->focused_toplevel && server->focused_toplevel->in_hsplit) {
        return server->focused_toplevel->hsplit_order;
    }
    if (server->focused_xwayland_view &&
        server->focused_xwayland_view->in_hsplit) {
        return server->focused_xwayland_view->hsplit_order;
    }
    return -1;
}

static void hsplit_focus_at(struct poison_server *server, int idx) {
    struct poison_toplevel *t;
    wl_list_for_each(t, &server->hsplit_toplevels, hsplit_link) {
        if (t->hsplit_order == idx) {
            focus_toplevel(t);
            return;
        }
    }
    struct poison_xwayland_view *xw;
    wl_list_for_each(xw, &server->hsplit_xwayland_views, hsplit_link) {
        if (xw->hsplit_order == idx) {
            focus_xwayland_view(server, xw);
            return;
        }
    }
}

static void hsplit_deactivate_focused(struct poison_server *server) {
    if (server->focused_toplevel && server->focused_toplevel->xdg_toplevel &&
        server->focused_toplevel->xdg_toplevel->base->initialized) {
        wlr_xdg_toplevel_set_activated(server->focused_toplevel->xdg_toplevel,
                                       false);
    } else if (server->focused_xwayland_view &&
               server->focused_xwayland_view->xwayland_surface) {
        wlr_xwayland_surface_activate(
            server->focused_xwayland_view->xwayland_surface, false);
    }
}

void hsplit_navigate(struct poison_server *server, bool next) {
    if (!server->hsplit_active) {
        return;
    }

    int xdg_count = wl_list_length(&server->hsplit_toplevels);
    int xw_count = wl_list_length(&server->hsplit_xwayland_views);
    int total = xdg_count + xw_count;

    int n_slots = total < 2 ? 2 : total;

    int current = hsplit_focused_index(server);

    if (current >= 0) {
        if (!next && current == 0) {
            return;
        }
        if (next && current >= n_slots - 1) {
            return;
        }
        int new_idx = next ? current + 1 : current - 1;
        if (new_idx < total) {
            hsplit_focus_at(server, new_idx);
        } else {
            server->hsplit_focused_slot = new_idx;
            server->focused_toplevel = NULL;
            server->focused_xwayland_view = NULL;
            wlr_seat_keyboard_clear_focus(server->seat);
        }
    } else if (server->hsplit_focused_slot >= 0) {
        int cur_empty = server->hsplit_focused_slot;
        if (!next && cur_empty == 0) {
            return;
        }
        if (next && cur_empty >= n_slots - 1) {
            return;
        }
        int new_idx = next ? cur_empty + 1 : cur_empty - 1;
        if (new_idx < total) {
            hsplit_focus_at(server, new_idx);
        } else {
            hsplit_deactivate_focused(server);
            server->hsplit_focused_slot = new_idx;
            server->focused_toplevel = NULL;
            server->focused_xwayland_view = NULL;
            wlr_seat_keyboard_clear_focus(server->seat);
        }
    } else {
        /* XXX */
        if (total == 0) {
            return;
        }
        hsplit_focus_at(server, next ? 0 : total - 1);
    }
}

void hsplit_only(struct poison_server *server) {
    bool focused_xdg = (server->focused_toplevel &&
                        server->focused_toplevel->in_hsplit);
    bool focused_xw = (!focused_xdg &&
                       server->focused_xwayland_view &&
                       server->focused_xwayland_view->in_hsplit);
    if (!focused_xdg && !focused_xw) {
        return;
    }

    struct wlr_box output_box;
    wlr_output_layout_get_box(server->output_layout, NULL, &output_box);
    int effective_padding = server->config.padding == 0 ? 1 : server->config.padding;

    struct poison_toplevel *t, *tmp;
    wl_list_for_each_safe(t, tmp, &server->hsplit_toplevels, hsplit_link) {
        t->in_hsplit = false;
        hsplit_forget_displaced(t);
        wl_list_remove(&t->hsplit_link);
        wl_list_init(&t->hsplit_link);
        t->floating = false;
        wlr_xdg_toplevel_set_tiled(t->xdg_toplevel,
                                   WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
        if (t == server->focused_toplevel) {
            if (output_box.width > 2 * effective_padding &&
                output_box.height > 2 * effective_padding) {
                wlr_xdg_toplevel_set_size(t->xdg_toplevel,
                                          output_box.width - 2 * effective_padding,
                                          output_box.height - 2 * effective_padding);
            }
            wlr_scene_node_set_position(&t->scene_tree->node,
                                        output_box.x + effective_padding,
                                        output_box.y + effective_padding);
        } else {
            t->needs_retile = true;
        }
    }
    struct poison_xwayland_view *xw, *xwtmp;
    wl_list_for_each_safe(xw, xwtmp, &server->hsplit_xwayland_views, hsplit_link) {
        xw->in_hsplit = false;
        hsplit_forget_displaced_xw(xw);
        wl_list_remove(&xw->hsplit_link);
        wl_list_init(&xw->hsplit_link);
        if (focused_xw && xw == server->focused_xwayland_view) {
            continue; /* Handled by the focused block below. */
        }
        if (restore_xwayland_fullscreen_from_hsplit(xw)) {
            continue;
        }
        if (xw->pre_hsplit_floating) {
            restore_xwayland_from_hsplit(xw);
            continue;
        }
        xw->floating = false;
        if (output_box.width > 2 * effective_padding &&
            output_box.height > 2 * effective_padding) {
            int w = output_box.width - 2 * effective_padding;
            int h = output_box.height - 2 * effective_padding;
            int x = output_box.x + effective_padding;
            int y = output_box.y + effective_padding;
            wlr_xwayland_surface_configure(xw->xwayland_surface, x, y, w, h);
            if (xw->scene_tree) {
                wlr_scene_node_set_position(&xw->scene_tree->node, x, y);
            }
            struct wlr_box tile = {.x = x, .y = y, .width = w, .height = h};
            set_xwayland_view_clip(xw, &tile);
        }
    }
    server->hsplit_active = false;
    server->hsplit_focused_slot = -1;

    if (focused_xdg) {
        struct poison_toplevel *focused = server->focused_toplevel;
        if (restore_toplevel_fullscreen_from_hsplit(focused)) {
        } else if (focused->pre_hsplit_floating) {
            restore_toplevel_from_hsplit(focused);
        } else {
            focused->floating = false;
            wlr_xdg_toplevel_set_tiled(focused->xdg_toplevel,
                                       WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
            if (output_box.width > 2 * effective_padding &&
                output_box.height > 2 * effective_padding) {
                wlr_xdg_toplevel_set_size(focused->xdg_toplevel,
                                          output_box.width - 2 * effective_padding,
                                          output_box.height - 2 * effective_padding);
            }
            wlr_scene_node_set_position(&focused->scene_tree->node,
                                        output_box.x + effective_padding,
                                        output_box.y + effective_padding);
        }
        wlr_scene_node_raise_to_top(&focused->scene_tree->node);
        focus_toplevel(focused);
    } else {
        struct poison_xwayland_view *focused_xwayland =
            server->focused_xwayland_view;
        if (restore_xwayland_fullscreen_from_hsplit(focused_xwayland)) {
        } else if (focused_xwayland->pre_hsplit_floating) {
            restore_xwayland_from_hsplit(focused_xwayland);
        } else {
            focused_xwayland->floating = false;
            if (output_box.width > 2 * effective_padding &&
                output_box.height > 2 * effective_padding) {
                int w = output_box.width - 2 * effective_padding;
                int h = output_box.height - 2 * effective_padding;
                int x = output_box.x + effective_padding;
                int y = output_box.y + effective_padding;
                wlr_xwayland_surface_configure(focused_xwayland->xwayland_surface,
                                               x, y, w, h);
                if (focused_xwayland->scene_tree) {
                    wlr_scene_node_set_position(
                        &focused_xwayland->scene_tree->node, x, y);
                }
                struct wlr_box tile = {.x = x, .y = y, .width = w, .height = h};
                set_xwayland_view_clip(focused_xwayland, &tile);
            }
        }
        if (focused_xwayland->scene_tree) {
            wlr_scene_node_raise_to_top(&focused_xwayland->scene_tree->node);
        }
        focus_xwayland_view(server, focused_xwayland);
    }
}

void hsplit_exchange(struct poison_server *server, bool right) {
    if (!server->hsplit_active) {
        return;
    }

    int total = wl_list_length(&server->hsplit_toplevels) +
        wl_list_length(&server->hsplit_xwayland_views);
    int cur = hsplit_focused_index(server);
    if (cur < 0) {
        return;
    }
    int neighbor = right ? cur + 1 : cur - 1;
    if (neighbor < 0 || neighbor >= total) {
        return;
    }

    int *a = NULL, *b = NULL;
    struct poison_toplevel *t;
    wl_list_for_each(t, &server->hsplit_toplevels, hsplit_link) {
        if (t->hsplit_order == cur) {
            a = &t->hsplit_order;
        } else if (t->hsplit_order == neighbor) {
            b = &t->hsplit_order;
        }
    }
    struct poison_xwayland_view *xw;
    wl_list_for_each(xw, &server->hsplit_xwayland_views, hsplit_link) {
        if (xw->hsplit_order == cur) {
            a = &xw->hsplit_order;
        } else if (xw->hsplit_order == neighbor) {
            b = &xw->hsplit_order;
        }
    }

    if (a && b) {
        int tmp = *a;
        *a = *b;
        *b = tmp;
        apply_hsplit_layout(server);
    }
}
