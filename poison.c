/* © 2026 dancingmirrors */

#include "poison.h"

#include <wayland-version.h>

static struct poison_cursor_image poison_cursor_image_xcursor(const char *name);
static struct poison_cursor_image poison_cursor_image_none(void);
static void set_cursor_image(struct poison_server *server,
                             struct poison_cursor_image image);
static void update_pointer_constraint(struct poison_server *server,
                                      struct wlr_surface *surface);
static void deactivate_constraint_for_surface(struct poison_server *server,
                                              struct wlr_surface *surface);
static void apply_xwayland_clip(struct poison_xwayland_view *view);
static void queue_pointer_motion(struct poison_server *server,
                                 uint32_t time_msec, double sx, double sy);
static void schedule_arrange(struct poison_server *server);

static struct poison_server *sigchld_server;

static void handle_sigchld(int sig) {
    (void)sig;
    int saved_errno = errno;
    pid_t pid;
    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
        struct poison_server *s = sigchld_server;
        if (s) {
            if (pid == s->indicator_pid) {
                s->indicator_pid = 0;
            }
            if (pid == s->notify_pid) {
                s->notify_pid = 0;
            }
            if (pid == s->launcher_pid) {
                s->launcher_pid = 0;
            }
            if (pid == s->window_selector_pid) {
                s->window_selector_pid = 0;
            }
            if (pid == s->console_pid) {
                s->console_pid = 0;
            }
        }
    }
    errno = saved_errno;
}

static void kill_tracked_child(pid_t *pid_field) {
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGCHLD);
    sigprocmask(SIG_BLOCK, &block, &old);
    pid_t pid = *pid_field;
    *pid_field = 0;
    if (pid > 0) {
        kill(pid, SIGTERM);
    }
    sigprocmask(SIG_SETMASK, &old, NULL);
}

static struct poison_output *poison_output_from_wlr(
    struct poison_server *server, struct wlr_output *wlr_output) {
    struct poison_output *output;
    wl_list_for_each(output, &server->outputs, link) {
        if (output->wlr_output == wlr_output) {
            return output;
        }
    }
    return NULL;
}

static void poison_output_sync_layout(struct poison_output *output,
                                      bool manual, int lx, int ly) {
    struct poison_server *server = output->server;
    struct wlr_output *wlr_output = output->wlr_output;

    if (!wlr_output->enabled) {
        wlr_output_layout_remove(server->output_layout, wlr_output);
        return;
    }

    struct wlr_scene_output *scene_output =
        wlr_scene_get_scene_output(server->scene, wlr_output);
    bool have_layout =
        wlr_output_layout_get(server->output_layout, wlr_output) != NULL;

    if (have_layout && scene_output) {
        if (manual) {
            wlr_output_layout_add(server->output_layout, wlr_output, lx, ly);
        }
        return;
    }

    if (scene_output) {
        wlr_scene_output_destroy(scene_output);
    }
    if (have_layout) {
        wlr_output_layout_remove(server->output_layout, wlr_output);
    }

    struct wlr_output_layout_output *l_output = manual
        ? wlr_output_layout_add(server->output_layout, wlr_output, lx, ly)
        : wlr_output_layout_add_auto(server->output_layout, wlr_output);
    if (!l_output) {
        wlr_log(WLR_ERROR, "Failed to add output %s to the layout!",
                wlr_output->name);
        return;
    }

    scene_output = wlr_scene_output_create(server->scene, wlr_output);
    if (!scene_output) {
        wlr_log(WLR_ERROR, "Failed to create a scene output for %s!",
                wlr_output->name);
        wlr_output_layout_remove(server->output_layout, wlr_output);
        return;
    }

    wlr_scene_output_layout_add_output(server->scene_layout, l_output,
                                       scene_output);
}

static bool output_try_mode(struct wlr_output *wlr_output,
                            struct wlr_output_mode *mode) {
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    if (mode != NULL) {
        wlr_output_state_set_mode(&state, mode);
    }

    bool ok = wlr_output_test_state(wlr_output, &state) &&
        wlr_output_commit_state(wlr_output, &state);

    wlr_output_state_finish(&state);
    return ok;
}

static bool output_enable_with_fallback(struct wlr_output *wlr_output) {
    struct wlr_output_mode *preferred = wlr_output_preferred_mode(wlr_output);

    if (preferred != NULL) {
        if (output_try_mode(wlr_output, preferred)) {
            return true;
        }
        wlr_log(WLR_ERROR, "Output %s rejected its preferred mode "
                           "%dx%d@%.3f Hz.",
                wlr_output->name, preferred->width, preferred->height,
                preferred->refresh / 1000.0);
    }

    struct wlr_output_mode *mode;
    wl_list_for_each(mode, &wlr_output->modes, link) {
        if (preferred != NULL && mode == preferred) {
            continue;
        }
        if (output_try_mode(wlr_output, mode)) {
            wlr_log(WLR_INFO, "Output %s fell back to %dx%d@%.3f Hz.",
                    wlr_output->name, mode->width, mode->height,
                    mode->refresh / 1000.0);
            return true;
        }
    }

    return output_try_mode(wlr_output, NULL);
}

static void publish_output_configuration(struct poison_server *server) {
    if (!server->output_manager) {
        return;
    }

    struct wlr_output_configuration_v1 *config =
        wlr_output_configuration_v1_create();
    if (!config) {
        wlr_log(WLR_ERROR, "Failed to allocate an output configuration!");
        return;
    }

    struct poison_output *output;
    wl_list_for_each(output, &server->outputs, link) {
        struct wlr_output_configuration_head_v1 *head =
            wlr_output_configuration_head_v1_create(config,
                                                    output->wlr_output);
        if (!head) {
            wlr_log(WLR_ERROR, "Failed to allocate an output configuration "
                               "head for %s!",
                    output->wlr_output->name);
            continue;
        }

        struct wlr_output_layout_output *l_output =
            wlr_output_layout_get(server->output_layout, output->wlr_output);
        if (l_output) {
            head->state.x = l_output->x;
            head->state.y = l_output->y;
        }
    }

    wlr_output_manager_v1_set_configuration(server->output_manager, config);
}

void output_destroy(struct wl_listener *listener, void *data) {
    struct poison_output *output =
        wl_container_of(listener, output, destroy);

    poison_render_output_finish(output);

    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->request_state.link);
    wl_list_remove(&output->link);

    free(output);
}

void output_request_state(struct wl_listener *listener, void *data) {
    struct poison_output *output =
        wl_container_of(listener, output, request_state);
    const struct wlr_output_event_request_state *event = data;

    if (!wlr_output_commit_state(output->wlr_output, event->state)) {
        wlr_log(WLR_ERROR, "Failed to commit the state requested for output "
                           "%s.",
                output->wlr_output->name);
        return;
    }

    poison_output_sync_layout(output, false, 0, 0);
    poison_render_request_repaint(output);
}

static void handle_renderer_lost(struct wl_listener *listener, void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, renderer_lost);

    poison_device_apply_env(&server->config);

    struct wlr_renderer *renderer = wlr_renderer_autocreate(server->backend);
    struct wlr_allocator *allocator =
        renderer ? wlr_allocator_autocreate(server->backend, renderer) : NULL;

    poison_device_clear_env();

    if (renderer == NULL) {
        wlr_log(WLR_ERROR, "Failed to re-create the renderer after GPU loss!");
        return;
    }
    if (allocator == NULL) {
        wlr_log(WLR_ERROR, "Failed to re-create the allocator after GPU loss!");
        wlr_renderer_destroy(renderer);
        return;
    }

    struct wlr_renderer *old_renderer = server->renderer;
    struct wlr_allocator *old_allocator = server->allocator;

    server->renderer = renderer;
    server->allocator = allocator;

    wl_list_remove(&server->renderer_lost.link);
    wl_signal_add(&server->renderer->events.lost, &server->renderer_lost);

    if (server->compositor) {
        wlr_compositor_set_renderer(server->compositor, renderer);
    }

    if (server->gamma_control_manager) {
        server->gamma_control_manager->fallback_gamma_size =
            renderer->features.output_color_transform ? GAMMA_FALLBACK_RAMP_SIZE : 0;
    }

    struct poison_output *output;
    wl_list_for_each(output, &server->outputs, link) {
        if (!wlr_output_init_render(output->wlr_output, server->allocator,
                                    server->renderer)) {
            wlr_log(WLR_ERROR, "Failed to re-initialize rendering for output "
                               "%s!",
                    output->wlr_output->name);
            continue;
        }
        poison_render_request_repaint(output);
    }

    wlr_allocator_destroy(old_allocator);
    wlr_renderer_destroy(old_renderer);

    /* The title bar textures went with the old renderer. */
    poison_decoration_invalidate_all(server);

    poison_device_log_topology(server);
}

void server_new_output(struct wl_listener *listener, void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, new_output);
    struct wlr_output *wlr_output = data;

    if (wlr_output->non_desktop) {
        return;
    }

    if (!wlr_output_init_render(wlr_output, server->allocator,
                                server->renderer)) {
        wlr_log(WLR_ERROR, "Failed to initialize rendering for output %s: "
                           "leaving it disabled.",
                wlr_output->name);
        return;
    }

    poison_device_log_output(server, wlr_output);

    struct poison_output *output = calloc(1, sizeof(*output));
    if (!output) {
        wlr_log(WLR_ERROR, "Failed to allocate output!");
        return;
    }
    output->wlr_output = wlr_output;
    output->server = server;

    output->frame.notify = poison_render_output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);
    output->destroy.notify = output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);
    output->request_state.notify = output_request_state;
    wl_signal_add(&wlr_output->events.request_state, &output->request_state);

    wl_list_insert(&server->outputs, &output->link);

    if (!output_enable_with_fallback(wlr_output)) {
        wlr_log(WLR_ERROR, "Failed to enable output %s with any of its modes.",
                wlr_output->name);
        return;
    }

    poison_output_sync_layout(output, false, 0, 0);

    wlr_xcursor_manager_load(server->cursor_mgr, wlr_output->scale);
}

void server_session_active(struct wl_listener *listener, void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, session_active);

    if (!server->session->active) {
        wlr_log(WLR_INFO, "Session inactive. Suspending output commits.");
        return;
    }

    wlr_log(WLR_INFO, "Session active. Repainting outputs.");

    struct poison_output *output;
    wl_list_for_each(output, &server->outputs, link) {
        poison_render_request_repaint(output);
    }
}

static bool apply_output_configuration(struct poison_server *server,
                                       struct wlr_output_configuration_v1 *config,
                                       bool test_only) {
    if (wl_list_empty(&config->heads)) {
        return true;
    }

    bool ok = true;
    struct wlr_output_configuration_head_v1 *head;

    wl_list_for_each(head, &config->heads, link) {
        struct wlr_output_state state;
        wlr_output_state_init(&state);
        wlr_output_head_v1_state_apply(&head->state, &state);
        ok = wlr_output_test_state(head->state.output, &state);
        wlr_output_state_finish(&state);
        if (!ok) {
            wlr_log(WLR_INFO, "Output %s cannot take the requested state.",
                    head->state.output->name);
            break;
        }
    }

    if (test_only || !ok) {
        return ok;
    }

    wl_list_for_each(head, &config->heads, link) {
        struct wlr_output_state state;
        wlr_output_state_init(&state);
        wlr_output_head_v1_state_apply(&head->state, &state);
        bool applied = wlr_output_commit_state(head->state.output, &state);
        wlr_output_state_finish(&state);

        if (!applied) {
            wlr_log(WLR_ERROR, "Failed to commit state for output %s.",
                    head->state.output->name);
            ok = false;
            continue;
        }

        /* XXX */
        struct poison_output *output =
            poison_output_from_wlr(server, head->state.output);
        if (output) {
            poison_output_sync_layout(output, true, head->state.x,
                                      head->state.y);
        }
    }

    return ok;
}

void output_manager_apply(struct wl_listener *listener, void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, output_manager_apply);
    struct wlr_output_configuration_v1 *config = data;

    if (apply_output_configuration(server, config, false)) {
        wlr_output_configuration_v1_send_succeeded(config);
    } else {
        wlr_output_configuration_v1_send_failed(config);
    }
    wlr_output_configuration_v1_destroy(config);

    schedule_arrange(server);
}

void output_manager_test(struct wl_listener *listener, void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, output_manager_test);
    struct wlr_output_configuration_v1 *config = data;

    if (apply_output_configuration(server, config, true)) {
        wlr_output_configuration_v1_send_succeeded(config);
    } else {
        wlr_output_configuration_v1_send_failed(config);
    }
    wlr_output_configuration_v1_destroy(config);
}

static void constrain_position_to_output(int *x, int *y, int width, int height,
                                         struct wlr_box *output_box, int padding) {
    int min_x = output_box->x + padding;
    int min_y = output_box->y + padding;
    int max_x = output_box->x + output_box->width - padding;
    int max_y = output_box->y + output_box->height - padding;

    if (*x < min_x) {
        *x = min_x;
    }
    if (*y < min_y) {
        *y = min_y;
    }

    if (*x + width > max_x) {
        *x = max_x - width;
        if (*x < min_x) {
            *x = min_x;
        }
    }
    if (*y + height > max_y) {
        *y = max_y - height;
        if (*y < min_y) {
            *y = min_y;
        }
    }
}

void poison_toplevel_set_tiling(struct poison_toplevel *toplevel, bool tiled,
                                bool fills_output) {
    if (!toplevel || !toplevel->xdg_toplevel) {
        return;
    }

    wlr_xdg_toplevel_set_tiled(toplevel->xdg_toplevel,
                               tiled ? (WLR_EDGE_TOP | WLR_EDGE_BOTTOM |
                                        WLR_EDGE_LEFT | WLR_EDGE_RIGHT)
                                     : WLR_EDGE_NONE);
    wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, fills_output);
}

static void toplevel_visible_size(struct poison_toplevel *toplevel, int *width,
                                  int *height) {
    *width = 0;
    *height = 0;

    if (!toplevel->xdg_toplevel || !toplevel->xdg_toplevel->base) {
        return;
    }

    struct wlr_box geo = toplevel->xdg_toplevel->base->geometry;
    if (geo.width > 0 && geo.height > 0) {
        *width = geo.width;
        *height = geo.height;
        return;
    }

    struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;
    if (surface) {
        *width = surface->current.width;
        *height = surface->current.height;
    }
}

static void toplevel_place_visible(struct poison_toplevel *toplevel, int x,
                                   int y) {
    struct wlr_box geo = {0};
    if (toplevel->xdg_toplevel && toplevel->xdg_toplevel->base) {
        geo = toplevel->xdg_toplevel->base->geometry;
    }

    wlr_scene_node_set_position(&toplevel->scene_tree->node, x - geo.x,
                                y - geo.y);
}

void center_toplevel(struct poison_toplevel *toplevel) {
    if (!toplevel || !toplevel->scene_tree) {
        return;
    }

    struct wlr_box output_box;
    wlr_output_layout_get_box(toplevel->server->output_layout, NULL,
                              &output_box);

    struct poison_frame_insets insets = poison_toplevel_insets(toplevel);
    int padding = toplevel->server->config.padding;
    int x = output_box.x + padding + insets.left;
    int y = output_box.y + padding + insets.top;

    int width, height;
    toplevel_visible_size(toplevel, &width, &height);

    if (width > 0 && height > 0 && output_box.width > 0 &&
        output_box.height > 0) {
        int frame_width = width + insets.left + insets.right;
        int frame_height = height + insets.top + insets.bottom;
        x = output_box.x + (output_box.width - frame_width) / 2;
        y = output_box.y + (output_box.height - frame_height) / 2;

        constrain_position_to_output(&x, &y, frame_width, frame_height,
                                     &output_box, padding);
        x += insets.left;
        y += insets.top;
    }

    toplevel_place_visible(toplevel, x, y);
    poison_decoration_update_toplevel(toplevel);
}

#define TILE_SHORTFALL_SLACK 8

static void place_tiled_toplevel(struct poison_toplevel *toplevel) {
    if (!toplevel || !toplevel->scene_tree) {
        return;
    }

    struct wlr_box output_box;
    wlr_output_layout_get_box(toplevel->server->output_layout, NULL,
                              &output_box);

    int padding = toplevel->server->config.padding;
    if (padding == 0) {
        padding = 1;
    }

    struct poison_frame_insets insets = poison_toplevel_insets(toplevel);
    int x = output_box.x + padding + insets.left;
    int y = output_box.y + padding + insets.top;

    int width, height;
    toplevel_visible_size(toplevel, &width, &height);

    int acked_width = 0;
    int acked_height = 0;
    if (toplevel->xdg_toplevel) {
        acked_width = toplevel->xdg_toplevel->current.width;
        acked_height = toplevel->xdg_toplevel->current.height;
    }

    bool too_narrow = acked_width > 0 && width > 0 &&
        width + TILE_SHORTFALL_SLACK < acked_width;
    bool too_short = acked_height > 0 && height > 0 &&
        height + TILE_SHORTFALL_SLACK < acked_height;

    if (!too_narrow && !too_short) {
        poison_toplevel_apply_position(toplevel, output_box.x + padding,
                                       output_box.y + padding);
        return;
    }

    if (too_narrow) {
        x = output_box.x +
            (output_box.width - (width + insets.left + insets.right)) / 2 +
            insets.left;
    }
    if (too_short) {
        y = output_box.y +
            (output_box.height - (height + insets.top + insets.bottom)) / 2 +
            insets.top;
    }

    toplevel_place_visible(toplevel, x, y);
    poison_decoration_update_toplevel(toplevel);
}

#define FLOAT_FALLBACK_WIDTH(output) (((output).width * 2) / 5)
#define FLOAT_FALLBACK_HEIGHT(output) (((output).height * 2) / 3)

static void clamp_float_size(struct poison_server *server,
                             struct poison_frame_insets insets, int *width,
                             int *height) {
    struct wlr_box output_box;
    wlr_output_layout_get_box(server->output_layout, NULL, &output_box);

    int padding = server->config.padding == 0 ? 1 : server->config.padding;
    int max_width = output_box.width - 2 * padding - insets.left - insets.right;
    int max_height =
        output_box.height - 2 * padding - insets.top - insets.bottom;

    if (max_width > 0 && *width > max_width) {
        *width = max_width;
    }
    if (max_height > 0 && *height > max_height) {
        *height = max_height;
    }
    if (*width < 1) {
        *width = 1;
    }
    if (*height < 1) {
        *height = 1;
    }
}

static void clamp_floating_toplevel(struct poison_toplevel *toplevel) {
    if (!toplevel->xdg_toplevel || toplevel->maximized) {
        return;
    }

    int width, height;
    toplevel_visible_size(toplevel, &width, &height);
    if (width <= 0 || height <= 0) {
        return;
    }

    int fit_width = width;
    int fit_height = height;
    clamp_float_size(toplevel->server, poison_toplevel_insets(toplevel),
                     &fit_width, &fit_height);
    if (fit_width == width && fit_height == height) {
        toplevel->clamp_seen_width = 0;
        toplevel->clamp_seen_height = 0;
        return;
    }

    struct wlr_xdg_toplevel_configure *scheduled =
        &toplevel->xdg_toplevel->scheduled;
    if (width == toplevel->clamp_seen_width &&
        height == toplevel->clamp_seen_height &&
        fit_width == scheduled->width && fit_height == scheduled->height) {
        return;
    }
    toplevel->clamp_seen_width = width;
    toplevel->clamp_seen_height = height;

    wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, fit_width, fit_height);
}

static void toplevel_float_size(struct poison_toplevel *toplevel, int *width,
                                int *height) {
    struct wlr_box output_box;
    wlr_output_layout_get_box(toplevel->server->output_layout, NULL,
                              &output_box);

    int w = toplevel->saved_float_width;
    int h = toplevel->saved_float_height;
    if (w <= 0 || h <= 0) {
        w = FLOAT_FALLBACK_WIDTH(output_box);
        h = FLOAT_FALLBACK_HEIGHT(output_box);
    }

    struct wlr_xdg_toplevel_state *state = &toplevel->xdg_toplevel->current;
    if (state->max_width > 0 && w > state->max_width) {
        w = state->max_width;
    }
    if (state->max_height > 0 && h > state->max_height) {
        h = state->max_height;
    }
    if (state->min_width > 0 && w < state->min_width) {
        w = state->min_width;
    }
    if (state->min_height > 0 && h < state->min_height) {
        h = state->min_height;
    }

    clamp_float_size(toplevel->server, poison_toplevel_insets(toplevel), &w, &h);

    *width = w;
    *height = h;
}

static bool maximize_toplevel(struct poison_toplevel *toplevel) {
    struct wlr_box output_box;
    wlr_output_layout_get_box(toplevel->server->output_layout, NULL,
                              &output_box);

    int ep = toplevel->server->config.padding == 0
        ? 1
        : toplevel->server->config.padding;
    if (output_box.width <= 2 * ep || output_box.height <= 2 * ep) {
        wlr_log(WLR_ERROR, "Output dimensions too small!");
        return false;
    }

    toplevel->maximized = true;
    poison_toplevel_set_tiling(toplevel, false, true);
    poison_toplevel_apply_box(toplevel, output_box.x + ep, output_box.y + ep,
                              output_box.width - 2 * ep,
                              output_box.height - 2 * ep);
    return true;
}

static void float_toplevel(struct poison_toplevel *toplevel) {
    if (toplevel->maximized && maximize_toplevel(toplevel)) {
        return;
    }
    toplevel->maximized = false;

    int width, height;
    toplevel_float_size(toplevel, &width, &height);

    poison_toplevel_set_tiling(toplevel, false, false);
    wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, width, height);
    center_toplevel(toplevel);
}

bool toplevel_fullscreen_now(struct poison_toplevel *toplevel) {
    if (!toplevel || !toplevel->xdg_toplevel) {
        return false;
    }
    return toplevel->xdg_toplevel->current.fullscreen ||
        toplevel->xdg_toplevel->scheduled.fullscreen;
}

bool restore_toplevel_fullscreen_from_hsplit(struct poison_toplevel *toplevel) {
    if (!toplevel || !toplevel->pre_hsplit_fullscreen) {
        return false;
    }
    toplevel->pre_hsplit_fullscreen = false;

    if (!toplevel->xdg_toplevel || !toplevel->xdg_toplevel->base->initialized) {
        return false;
    }

    struct wlr_box output_box;
    wlr_output_layout_get_box(toplevel->server->output_layout, NULL,
                              &output_box);
    if (output_box.width <= 0 || output_box.height <= 0) {
        return false;
    }

    toplevel->floating = toplevel->pre_hsplit_floating;
    toplevel->pre_hsplit_floating = false;
    toplevel->needs_retile = false;

    poison_toplevel_set_tiling(toplevel, !toplevel->floating, false);
    wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, output_box.width,
                              output_box.height);
    wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, true);
    if (toplevel->scene_tree) {
        wlr_scene_node_set_position(&toplevel->scene_tree->node, 0, 0);
    }

    return true;
}

void restore_toplevel_from_hsplit(struct poison_toplevel *toplevel) {
    if (!toplevel || !toplevel->xdg_toplevel) {
        return;
    }
    if (restore_toplevel_fullscreen_from_hsplit(toplevel)) {
        return;
    }
    toplevel->pre_hsplit_floating = false;
    toplevel->floating = true;
    float_toplevel(toplevel);
}

bool restore_xwayland_fullscreen_from_hsplit(
    struct poison_xwayland_view *xwayland_view) {
    if (!xwayland_view || !xwayland_view->pre_hsplit_fullscreen) {
        return false;
    }
    xwayland_view->pre_hsplit_fullscreen = false;

    if (!xwayland_view->xwayland_surface) {
        return false;
    }

    struct wlr_box output_box;
    wlr_output_layout_get_box(xwayland_view->server->output_layout, NULL,
                              &output_box);
    if (output_box.width <= 0 || output_box.height <= 0) {
        return false;
    }

    xwayland_view->pre_hsplit_floating = false;
    xwayland_view->floating = false;

    wlr_xwayland_surface_configure(xwayland_view->xwayland_surface,
                                   output_box.x, output_box.y,
                                   output_box.width, output_box.height);
    wlr_xwayland_surface_set_fullscreen(xwayland_view->xwayland_surface, true);
    if (xwayland_view->scene_tree) {
        wlr_scene_node_set_position(&xwayland_view->scene_tree->node,
                                    output_box.x, output_box.y);
    }
    set_xwayland_view_clip(xwayland_view, NULL);

    return true;
}

static void xwayland_view_float_size(struct poison_xwayland_view *view,
                                     int *width, int *height) {
    struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
    struct wlr_box output_box;
    wlr_output_layout_get_box(view->server->output_layout, NULL, &output_box);

    int w = view->saved_float_width;
    int h = view->saved_float_height;

    if ((w <= 0 || h <= 0) && xsurface->size_hints &&
        xsurface->size_hints->width > 0 && xsurface->size_hints->height > 0) {
        w = xsurface->size_hints->width;
        h = xsurface->size_hints->height;
    }
    if (w <= 0 || h <= 0) {
        w = FLOAT_FALLBACK_WIDTH(output_box);
        h = FLOAT_FALLBACK_HEIGHT(output_box);
    }

    clamp_float_size(view->server, poison_xwayland_view_insets(view), &w, &h);

    *width = w;
    *height = h;
}

static bool maximize_xwayland_view(struct poison_xwayland_view *view) {
    struct wlr_xwayland_surface *xsurface = view->xwayland_surface;

    struct wlr_box output_box;
    wlr_output_layout_get_box(view->server->output_layout, NULL, &output_box);

    int ep = view->server->config.padding == 0
        ? 1
        : view->server->config.padding;

    struct poison_frame_insets insets = poison_xwayland_view_insets(view);
    int x = output_box.x + ep + insets.left;
    int y = output_box.y + ep + insets.top;
    int width = output_box.width - 2 * ep - insets.left - insets.right;
    int height = output_box.height - 2 * ep - insets.top - insets.bottom;

    if (width < 1 || height < 1) {
        wlr_log(WLR_ERROR, "Output dimensions too small!");
        return false;
    }

    view->maximized = true;
    wlr_xwayland_surface_set_fullscreen(xsurface, false);
    wlr_xwayland_surface_set_maximized(xsurface, true, true);
    wlr_xwayland_surface_configure(xsurface, x, y, width, height);
    if (view->scene_tree) {
        wlr_scene_node_set_position(&view->scene_tree->node, x, y);
    }
    set_xwayland_view_clip(view, NULL);
    poison_decoration_update_xwayland(view);
    return true;
}

static void float_xwayland_view(struct poison_xwayland_view *view) {
    if (view->maximized && maximize_xwayland_view(view)) {
        return;
    }
    view->maximized = false;

    struct wlr_xwayland_surface *xsurface = view->xwayland_surface;

    wlr_xwayland_surface_set_fullscreen(xsurface, false);
    wlr_xwayland_surface_set_maximized(xsurface, false, false);

    struct wlr_box output_box;
    wlr_output_layout_get_box(view->server->output_layout, NULL, &output_box);

    int width, height;
    xwayland_view_float_size(view, &width, &height);

    struct poison_frame_insets insets = poison_xwayland_view_insets(view);
    int frame_width = width + insets.left + insets.right;
    int frame_height = height + insets.top + insets.bottom;
    int x = output_box.x + (output_box.width - frame_width) / 2;
    int y = output_box.y + (output_box.height - frame_height) / 2;
    constrain_position_to_output(&x, &y, frame_width, frame_height, &output_box,
                                 view->server->config.padding);
    x += insets.left;
    y += insets.top;

    wlr_xwayland_surface_configure(xsurface, x, y, width, height);
    if (view->scene_tree) {
        wlr_scene_node_set_position(&view->scene_tree->node, x, y);
    }
    set_xwayland_view_clip(view, NULL);
    poison_decoration_update_xwayland(view);
}

void restore_xwayland_from_hsplit(struct poison_xwayland_view *xwayland_view) {
    if (!xwayland_view || !xwayland_view->xwayland_surface) {
        return;
    }
    if (restore_xwayland_fullscreen_from_hsplit(xwayland_view)) {
        return;
    }
    xwayland_view->pre_hsplit_floating = false;
    xwayland_view->floating = true;
    float_xwayland_view(xwayland_view);
}

static void apply_needs_retile(struct poison_toplevel *toplevel) {
    if (!toplevel->needs_retile) {
        return;
    }
    toplevel->needs_retile = false;
    if (!toplevel->xdg_toplevel || toplevel->in_hsplit) {
        return;
    }
    if (restore_toplevel_fullscreen_from_hsplit(toplevel)) {
        return;
    }
    if (toplevel_fullscreen_now(toplevel)) {
        return;
    }
    if (toplevel->pre_hsplit_floating) {
        restore_toplevel_from_hsplit(toplevel);
    } else if (!toplevel->floating) {
        struct wlr_box output_box;
        wlr_output_layout_get_box(toplevel->server->output_layout, NULL,
                                  &output_box);
        int ep = toplevel->server->config.padding == 0 ? 1
                                                       : toplevel->server->config.padding;
        poison_toplevel_set_tiling(toplevel, true, true);
        if (output_box.width > 2 * ep && output_box.height > 2 * ep) {
            poison_toplevel_apply_size(toplevel,
                                       output_box.width - 2 * ep, output_box.height - 2 * ep);
        }
        poison_toplevel_apply_position(toplevel,
                                       output_box.x + ep, output_box.y + ep);
    }
}

static uint32_t poison_now_msec(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

static void update_cursor_focus(struct poison_server *server) {
    double sx, sy;
    struct wlr_surface *surface = NULL;
    struct wlr_scene_node *node =
        wlr_scene_node_at(&server->scene_layers->node, server->cursor->x,
                          server->cursor->y, &sx, &sy);

    if (node && node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_buffer *scene_buffer =
            wlr_scene_buffer_from_node(node);
        struct wlr_scene_surface *scene_surface =
            wlr_scene_surface_try_from_buffer(scene_buffer);
        if (scene_surface) {
            surface = scene_surface->surface;
        }
    }

    if (server->focused_toplevel && server->focused_toplevel->xdg_toplevel) {
        bool found_focused_surface = false;

        if (node) {
            struct wlr_scene_node *parent = node;
            while (parent) {
                if (parent->data == server->focused_toplevel) {
                    found_focused_surface = true;
                    break;
                }
                parent = parent->parent ? &parent->parent->node : NULL;
            }
        }

        if (!found_focused_surface) {
            struct wlr_box box;
            if (wlr_scene_node_coords(&server->focused_toplevel->scene_tree->node, &box.x, &box.y)) {
                box.width = server->focused_toplevel->xdg_toplevel->current.width;
                box.height = server->focused_toplevel->xdg_toplevel->current.height;

                if (wlr_box_contains_point(&box, server->cursor->x, server->cursor->y)) {
                    struct wlr_box geo =
                        server->focused_toplevel->xdg_toplevel->base->geometry;
                    surface = server->focused_toplevel->xdg_toplevel->base->surface;
                    sx = server->cursor->x - box.x + geo.x;
                    sy = server->cursor->y - box.y + geo.y;
                }
            }
        }
    }

    if (surface) {
        if (server->seat->pointer_state.focused_surface == surface) {
            if (sx != server->seat->pointer_state.sx ||
                sy != server->seat->pointer_state.sy) {
                queue_pointer_motion(server, poison_now_msec(), sx, sy);
            }
        } else {
            if (server->cursor_image.type == POISON_CURSOR_IMAGE_CLIENT) {
                set_cursor_image(server, poison_cursor_image_xcursor("default"));
            }
            wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
        }
    } else {
        set_cursor_image(server, poison_cursor_image_xcursor("default"));
        wlr_seat_pointer_notify_clear_focus(server->seat);
    }
    update_pointer_constraint(server, surface);
}

void focus_xwayland_view(struct poison_server *server,
                         struct poison_xwayland_view *xwayland_view) {
    if (!xwayland_view || !xwayland_view->xwayland_surface) {
        return;
    }

    bool already_focused =
        server->focused_xwayland_view == xwayland_view &&
        xwayland_view->xwayland_surface->surface != NULL &&
        server->seat->keyboard_state.focused_surface ==
            xwayland_view->xwayland_surface->surface;

    if (!already_focused) {
        if (server->focused_toplevel && server->focused_toplevel->xdg_toplevel && server->focused_toplevel->xdg_toplevel->base->initialized) {
            wlr_xdg_toplevel_set_activated(server->focused_toplevel->xdg_toplevel, false);
        } else if (server->focused_xwayland_view && server->focused_xwayland_view->xwayland_surface) {
            wlr_xwayland_surface_activate(server->focused_xwayland_view->xwayland_surface, false);
        }
    }

    wlr_xwayland_surface_activate(xwayland_view->xwayland_surface, true);
    if (xwayland_view->scene_tree) {
        wlr_scene_node_raise_to_top(&xwayland_view->scene_tree->node);

        if (xwayland_view->xwayland_surface->fullscreen &&
            !xwayland_view->in_hsplit) {
            struct wlr_box output_box;
            wlr_output_layout_get_box(server->output_layout, NULL, &output_box);
            wlr_scene_node_set_position(&xwayland_view->scene_tree->node,
                                        output_box.x, output_box.y);
        }
    }

    if (!already_focused) {
        struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
        if (keyboard != NULL) {
            wlr_seat_keyboard_notify_enter(server->seat,
                                           xwayland_view->xwayland_surface->surface,
                                           keyboard->keycodes, keyboard->num_keycodes,
                                           &keyboard->modifiers);
        }

        server->prev_focused_toplevel = server->focused_toplevel;
        server->prev_focused_xwayland_view = server->focused_xwayland_view;
    }
    server->focused_xwayland_view = xwayland_view;
    server->focused_toplevel = NULL;
    server->hsplit_focused_slot = -1;

    if (server->prev_focused_xwayland_view &&
        server->prev_focused_xwayland_view != xwayland_view) {
        apply_xwayland_clip(server->prev_focused_xwayland_view);
    }
    apply_xwayland_clip(xwayland_view);

    poison_decoration_update_all(server);

    update_cursor_focus(server);
}

static void reset_cursor_to_default(struct poison_server *server) {
    if (!server) {
        return;
    }
    wlr_seat_pointer_notify_clear_focus(server->seat);
    set_cursor_image(server, poison_cursor_image_xcursor("default"));
}

static struct poison_toplevel *find_topmost_toplevel(struct poison_server *server) {
    if (wl_list_empty(&server->toplevels)) {
        return NULL;
    }

    struct poison_toplevel *topmost = NULL;
    struct wl_list *children = &server->tree_toplevels->children;
    struct wl_list *last_child = children->prev;

    while (last_child != children) {
        struct wlr_scene_node *node = wl_container_of(last_child, node, link);

        if (node->data && node->type == WLR_SCENE_NODE_TREE) {
            struct poison_toplevel *toplevel;
            wl_list_for_each(toplevel, &server->toplevels, link) {
                if (toplevel->scene_tree && &toplevel->scene_tree->node == node) {
                    topmost = toplevel;
                    break;
                }
            }
            if (topmost) {
                break;
            }
        }

        last_child = last_child->prev;
    }

    return topmost ? topmost : wl_container_of(server->toplevels.next, topmost, link);
}

static struct poison_xwayland_view *find_topmost_xwayland_view(struct poison_server *server) {
    if (wl_list_empty(&server->xwayland_views)) {
        return NULL;
    }

    struct poison_xwayland_view *topmost = NULL;
    struct wl_list *children = &server->tree_toplevels->children;
    struct wl_list *last_child = children->prev;

    while (last_child != children) {
        struct wlr_scene_node *node = wl_container_of(last_child, node, link);

        if (node->data && node->type == WLR_SCENE_NODE_TREE) {
            struct poison_xwayland_view *xwayland_view;
            wl_list_for_each(xwayland_view, &server->xwayland_views, link) {
                if (xwayland_view->scene_tree && &xwayland_view->scene_tree->node == node) {
                    topmost = xwayland_view;
                    break;
                }
            }
            if (topmost) {
                break;
            }
        }

        last_child = last_child->prev;
    }

    return topmost ? topmost : wl_container_of(server->xwayland_views.next, topmost, link);
}

static void restore_toplevel_focus(struct poison_server *server) {
    struct poison_toplevel *next_toplevel = NULL;
    struct poison_xwayland_view *next_xwayland = NULL;

    if (server->prev_focused_toplevel != NULL) {
        struct poison_toplevel *t;
        wl_list_for_each(t, &server->toplevels, link) {
            if (t == server->prev_focused_toplevel) {
                next_toplevel = server->prev_focused_toplevel;
                break;
            }
        }
    }

    if (next_toplevel == NULL && server->prev_focused_xwayland_view != NULL) {
        struct poison_xwayland_view *xw;
        wl_list_for_each(xw, &server->xwayland_views, link) {
            if (xw == server->prev_focused_xwayland_view) {
                next_xwayland = server->prev_focused_xwayland_view;
                break;
            }
        }
    }

    if (next_toplevel != NULL) {
        focus_toplevel(next_toplevel);
    } else if (next_xwayland != NULL) {
        focus_xwayland_view(server, next_xwayland);
    } else {
        next_toplevel = find_topmost_toplevel(server);
        if (next_toplevel) {
            focus_toplevel(next_toplevel);
        } else {
            next_xwayland = find_topmost_xwayland_view(server);
            if (next_xwayland) {
                focus_xwayland_view(server, next_xwayland);
            }
        }
    }
}

void focus_toplevel(struct poison_toplevel *toplevel) {
    if (toplevel == NULL || toplevel->xdg_toplevel == NULL) {
        return;
    }

    struct poison_server *server = toplevel->server;
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *prev_surface =
        seat->keyboard_state.focused_surface;

    bool already_focused =
        prev_surface == toplevel->xdg_toplevel->base->surface &&
        server->focused_toplevel == toplevel;

    if (!already_focused) {
        if (server->focused_toplevel && server->focused_toplevel->xdg_toplevel && server->focused_toplevel->xdg_toplevel->base->initialized) {
            wlr_xdg_toplevel_set_activated(server->focused_toplevel->xdg_toplevel, false);
        } else if (server->focused_xwayland_view && server->focused_xwayland_view->xwayland_surface) {
            wlr_xwayland_surface_activate(server->focused_xwayland_view->xwayland_surface, false);
        }
    }

    wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);

    apply_needs_retile(toplevel);

    if (toplevel->xdg_toplevel && toplevel->scene_tree &&
        toplevel->xdg_toplevel->current.fullscreen &&
        !toplevel->in_hsplit) {
        wlr_scene_node_set_position(&toplevel->scene_tree->node, 0, 0);
    }

    if (!already_focused && toplevel->xdg_toplevel) {
        wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);

        struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
        if (keyboard != NULL) {
            wlr_seat_keyboard_notify_enter(seat,
                                           toplevel->xdg_toplevel->base->surface, keyboard->keycodes,
                                           keyboard->num_keycodes,
                                           &keyboard->modifiers);
        }
    }

    if (server->focused_toplevel != toplevel) {
        server->prev_focused_toplevel = server->focused_toplevel;
        server->prev_focused_xwayland_view = server->focused_xwayland_view;
    }
    server->focused_toplevel = toplevel;
    server->focused_xwayland_view = NULL;
    server->hsplit_focused_slot = -1;

    if (server->prev_focused_xwayland_view) {
        apply_xwayland_clip(server->prev_focused_xwayland_view);
    }

    poison_decoration_update_all(server);

    update_cursor_focus(server);
}

void cycle_toplevel(struct poison_server *server, bool reverse) {
    if (wl_list_empty(&server->toplevels) && wl_list_empty(&server->xwayland_views)) {
        return;
    }

    bool current_is_xdg = false;
    bool current_is_xwayland = false;
    struct poison_toplevel *current_toplevel = server->focused_toplevel;
    struct poison_xwayland_view *current_xwayland = server->focused_xwayland_view;

    if (current_toplevel) {
        current_is_xdg = true;
    } else if (current_xwayland) {
        current_is_xwayland = true;
    }

    if (!current_is_xdg && !current_is_xwayland) {
        if (!wl_list_empty(&server->toplevels)) {
            struct poison_toplevel *next = wl_container_of(server->toplevels.next, next, link);
            focus_toplevel(next);
        } else if (!wl_list_empty(&server->xwayland_views)) {
            struct poison_xwayland_view *next =
                wl_container_of(server->xwayland_views.next, next, link);
            focus_xwayland_view(server, next);
        }
        return;
    }

    if (current_is_xdg) {
        if (!reverse) {
            if (current_toplevel->link.next != &server->toplevels) {
                struct poison_toplevel *next =
                    wl_container_of(current_toplevel->link.next, next, link);
                focus_toplevel(next);
            } else if (!wl_list_empty(&server->xwayland_views)) {
                struct poison_xwayland_view *next =
                    wl_container_of(server->xwayland_views.next, next, link);
                focus_xwayland_view(server, next);
            } else {
                struct poison_toplevel *next =
                    wl_container_of(server->toplevels.next, next, link);
                focus_toplevel(next);
            }
        } else {
            if (current_toplevel->link.prev != &server->toplevels) {
                struct poison_toplevel *next =
                    wl_container_of(current_toplevel->link.prev, next, link);
                focus_toplevel(next);
            } else if (!wl_list_empty(&server->xwayland_views)) {
                struct poison_xwayland_view *next =
                    wl_container_of(server->xwayland_views.prev, next, link);
                focus_xwayland_view(server, next);
            } else {
                struct poison_toplevel *next =
                    wl_container_of(server->toplevels.prev, next, link);
                focus_toplevel(next);
            }
        }
    } else if (current_is_xwayland) {
        if (!reverse) {
            if (current_xwayland->link.next != &server->xwayland_views) {
                struct poison_xwayland_view *next =
                    wl_container_of(current_xwayland->link.next, next, link);
                focus_xwayland_view(server, next);
            } else if (!wl_list_empty(&server->toplevels)) {
                struct poison_toplevel *next =
                    wl_container_of(server->toplevels.next, next, link);
                focus_toplevel(next);
            } else {
                struct poison_xwayland_view *next =
                    wl_container_of(server->xwayland_views.next, next, link);
                focus_xwayland_view(server, next);
            }
        } else {
            if (current_xwayland->link.prev != &server->xwayland_views) {
                struct poison_xwayland_view *next =
                    wl_container_of(current_xwayland->link.prev, next, link);
                focus_xwayland_view(server, next);
            } else if (!wl_list_empty(&server->toplevels)) {
                struct poison_toplevel *next =
                    wl_container_of(server->toplevels.prev, next, link);
                focus_toplevel(next);
            } else {
                struct poison_xwayland_view *next =
                    wl_container_of(server->xwayland_views.prev, next, link);
                focus_xwayland_view(server, next);
            }
        }
    }
}

void close_toplevel(struct poison_toplevel *toplevel) {
    if (toplevel && toplevel->xdg_toplevel) {
        wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
    }
}

/* Put a window back where it was before it went fullscreen. */
static void leave_toplevel_fullscreen(struct poison_toplevel *toplevel) {
    struct wlr_xdg_toplevel *xdg_toplevel = toplevel->xdg_toplevel;

    /* Drop the fullscreen state up front: the window is decorated again from
     * here on, and the size below has to account for its frame. */
    wlr_xdg_toplevel_set_fullscreen(xdg_toplevel, false);

    if (toplevel->pre_fullscreen_floating) {
        toplevel->floating = true;
        float_toplevel(toplevel);
        return;
    }

    struct wlr_box output_box;
    wlr_output_layout_get_box(toplevel->server->output_layout, NULL,
                              &output_box);
    int ep = toplevel->server->config.padding == 0
        ? 1
        : toplevel->server->config.padding;
    toplevel->floating = false;
    toplevel->maximized = false;
    poison_toplevel_set_tiling(toplevel, true, true);
    if (output_box.width > 2 * ep && output_box.height > 2 * ep) {
        poison_toplevel_apply_size(toplevel,
                                   output_box.width - 2 * ep,
                                   output_box.height - 2 * ep);
    } else {
        wlr_xdg_toplevel_set_size(xdg_toplevel,
                                  output_box.width, output_box.height);
    }
    poison_toplevel_apply_position(toplevel,
                                   output_box.x + ep, output_box.y + ep);
}

void toggle_float_toplevel(struct poison_toplevel *toplevel) {
    if (!toplevel || !toplevel->xdg_toplevel) {
        return;
    }

    if (toplevel->in_hsplit) {
        return;
    }

    if (toplevel_fullscreen_now(toplevel)) {
        leave_toplevel_fullscreen(toplevel);
        return;
    }

    toplevel->floating = !toplevel->floating;

    struct wlr_box output_box;
    wlr_output_layout_get_box(toplevel->server->output_layout, NULL,
                              &output_box);

    int effective_padding = toplevel->server->config.padding == 0 ? 1 : toplevel->server->config.padding;

    if (toplevel->floating) {
        float_toplevel(toplevel);
        return;
    }

    if (!toplevel->maximized) {
        struct wlr_box geo = toplevel->xdg_toplevel->base->geometry;
        if (geo.width > 0 && geo.height > 0) {
            toplevel->saved_float_width = geo.width;
            toplevel->saved_float_height = geo.height;
        }
    }

    toplevel->maximized = false;

    if (output_box.width > 2 * effective_padding &&
        output_box.height > 2 * effective_padding) {
        poison_toplevel_set_tiling(toplevel, true, true);
        poison_toplevel_apply_size(toplevel,
                                   output_box.width - 2 * effective_padding,
                                   output_box.height - 2 * effective_padding);
    } else {
        wlr_log(WLR_ERROR, "Output dimensions too small!");
    }

    poison_toplevel_apply_position(toplevel, output_box.x + effective_padding,
                                   output_box.y + effective_padding);
}

void toggle_maximize_toplevel(struct poison_toplevel *toplevel) {
    if (!toplevel || !toplevel->xdg_toplevel || toplevel->in_hsplit) {
        return;
    }

    if (toplevel_fullscreen_now(toplevel)) {
        leave_toplevel_fullscreen(toplevel);
        return;
    }

    if (!toplevel->floating) {
        toggle_float_toplevel(toplevel);
        return;
    }

    if (toplevel->maximized) {
        toplevel->maximized = false;
        float_toplevel(toplevel);
        return;
    }

    struct wlr_box geo = toplevel->xdg_toplevel->base->geometry;
    if (geo.width > 0 && geo.height > 0) {
        toplevel->saved_float_width = geo.width;
        toplevel->saved_float_height = geo.height;
    }
    maximize_toplevel(toplevel);
}

void toggle_float_xwayland_view(struct poison_xwayland_view *xwayland_view) {
    if (!xwayland_view || !xwayland_view->xwayland_surface) {
        return;
    }

    struct wlr_xwayland_surface *xsurface = xwayland_view->xwayland_surface;

    if (xsurface->surface == NULL || !xsurface->surface->mapped) {
        return;
    }

    if (xwayland_view->in_hsplit) {
        return;
    }

    xwayland_view->floating = !xwayland_view->floating;

    struct wlr_box output_box;
    wlr_output_layout_get_box(xwayland_view->server->output_layout, NULL,
                              &output_box);

    if (xwayland_view->floating) {
        float_xwayland_view(xwayland_view);
    } else {
        int effective_padding = xwayland_view->server->config.padding == 0 ? 1 : xwayland_view->server->config.padding;

        /* Remember the floating size before we tile so we can restore it
         * later. At this point the surface still reports its floating size,
         * unless it's maximized, in which case saved_float_* already holds
         * the size from before the maximize, which is the one worth keeping. */
        if (!xwayland_view->maximized && xsurface->width > 0 &&
            xsurface->height > 0) {
            xwayland_view->saved_float_width = xsurface->width;
            xwayland_view->saved_float_height = xsurface->height;
        }

        xwayland_view->maximized = false;
        wlr_xwayland_surface_set_maximized(xsurface, false, false);

        int x = output_box.x + effective_padding;
        int y = output_box.y + effective_padding;
        int width = output_box.width - 2 * effective_padding;
        int height = output_box.height - 2 * effective_padding;

        if (width > 0 && height > 0) {
            wlr_xwayland_surface_configure(xsurface, x, y, width, height);
            wlr_xwayland_surface_set_fullscreen(xsurface, true);

            if (xwayland_view->scene_tree) {
                wlr_scene_node_set_position(&xwayland_view->scene_tree->node, x, y);
            }
            struct wlr_box tile = {.x = x, .y = y, .width = width, .height = height};
            set_xwayland_view_clip(xwayland_view, &tile);
            poison_decoration_update_xwayland(xwayland_view);
        } else {
            wlr_log(WLR_ERROR, "Output dimensions too small!");
        }
    }
}

void toggle_maximize_xwayland_view(struct poison_xwayland_view *view) {
    if (!view || !view->xwayland_surface || view->in_hsplit) {
        return;
    }

    struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
    if (xsurface->surface == NULL || !xsurface->surface->mapped) {
        return;
    }

    if (xsurface->fullscreen) {
        return;
    }

    if (!view->floating) {
        toggle_float_xwayland_view(view);
        return;
    }

    if (view->maximized) {
        view->maximized = false;
        float_xwayland_view(view);
        return;
    }

    if (xsurface->width > 0 && xsurface->height > 0) {
        view->saved_float_width = xsurface->width;
        view->saved_float_height = xsurface->height;
    }
    maximize_xwayland_view(view);
}

static const char *get_toplevel_title(struct poison_toplevel *toplevel) {
    if (!toplevel || !toplevel->xdg_toplevel) {
        return NULL;
    }

    const char *title = toplevel->xdg_toplevel->title;
    if (title && title[0] != '\0') {
        return title;
    }

    const char *app_id = toplevel->xdg_toplevel->app_id;
    if (app_id && app_id[0] != '\0') {
        return app_id;
    }

    return "Untitled";
}

static const char *get_xwayland_title(struct poison_xwayland_view *view) {
    if (!view || !view->xwayland_surface) {
        return NULL;
    }

    const char *title = view->xwayland_surface->title;
    if (title && title[0] != '\0') {
        return title;
    }

    const char *class = view->xwayland_surface->class;
    if (class && class[0] != '\0') {
        return class;
    }

    return "Untitled";
}

void view_order_arrange(struct poison_server *server,
                        struct poison_view_order **wanted, int n) {
    if (n < 2) {
        return;
    }
    int count = wl_list_length(&server->view_order);
    if (count < 2) {
        return;
    }

    struct poison_view_order **nodes = calloc(count, sizeof(*nodes));
    struct poison_view_order **present = calloc(n, sizeof(*present));
    if (!nodes || !present) {
        free(nodes);
        free(present);
        return;
    }

    int seen = 0;
    struct poison_view_order *node;
    wl_list_for_each(node, &server->view_order, link) {
        nodes[seen++] = node;
    }

    int np = 0;
    for (int j = 0; j < n; j++) {
        for (int i = 0; i < seen; i++) {
            if (nodes[i] == wanted[j]) {
                present[np++] = wanted[j];
                break;
            }
        }
    }

    int next = 0;
    for (int i = 0; i < seen && next < np; i++) {
        for (int j = 0; j < np; j++) {
            if (nodes[i] == present[j]) {
                nodes[i] = present[next++];
                break;
            }
        }
    }

    wl_list_init(&server->view_order);
    for (int i = 0; i < seen; i++) {
        wl_list_insert(server->view_order.prev, &nodes[i]->link);
    }

    free(nodes);
    free(present);
}

static struct poison_view_order *view_by_serial(struct poison_server *server,
                                                uint32_t serial) {
    if (serial == 0) {
        return NULL;
    }
    struct poison_view_order *node;
    wl_list_for_each(node, &server->view_order, link) {
        if (node->serial == serial) {
            return node;
        }
    }
    return NULL;
}

static void apply_selector_order(struct poison_server *server,
                                 const char *spec) {
    int count = wl_list_length(&server->view_order);
    if (count <= 0) {
        return;
    }

    struct poison_view_order **wanted = calloc(count, sizeof(*wanted));
    if (!wanted) {
        return;
    }

    int n = 0;
    const char *p = spec;
    while (*p != '\0' && *p != '\n' && n < count) {
        char *end;
        unsigned long serial = strtoul(p, &end, 10);
        if (end == p) {
            break;
        }
        p = end;
        while (*p == ' ' || *p == '\t') {
            p++;
        }

        struct poison_view_order *found =
            view_by_serial(server, (uint32_t)serial);
        if (!found) {
            continue;
        }
        bool duplicate = false;
        for (int i = 0; i < n; i++) {
            if (wanted[i] == found) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            wanted[n++] = found;
        }
    }

    view_order_arrange(server, wanted, n);

    free(wanted);
}

static int handle_window_selector_result(int fd, uint32_t mask, void *data) {
    struct poison_server *server = data;
    (void)mask;

    char chunk[512];
    ssize_t n = read(fd, chunk, sizeof(chunk));
    if (n > 0) {
        size_t room = sizeof(server->window_selector_reply) - 1 -
            server->window_selector_reply_len;
        size_t copy = (size_t)n < room ? (size_t)n : room;
        memcpy(server->window_selector_reply +
                   server->window_selector_reply_len,
               chunk, copy);
        server->window_selector_reply_len += copy;
        return 0;
    }

    char *buffer = server->window_selector_reply;
    buffer[server->window_selector_reply_len] = '\0';

    struct poison_view_order *selected = NULL;

    if (server->window_selector_reply_len > 0) {
        char *selection = buffer;
        char *order_spec = NULL;
        if (strncmp(buffer, "order ", 6) == 0) {
            order_spec = buffer + 6;
            char *newline = strchr(order_spec, '\n');
            selection = newline ? newline + 1 : order_spec + strlen(order_spec);
        }

        long selected_serial = strtol(selection, NULL, 10);
        if (selected_serial > 0) {
            selected = view_by_serial(server, (uint32_t)selected_serial);
        }

        if (order_spec) {
            apply_selector_order(server, order_spec);
        }
    }

    if (selected != NULL && selected->type == POISON_VIEW_XDG) {
        struct poison_toplevel *toplevel =
            wl_container_of(selected, toplevel, order);
        if (toplevel->scene_tree) {
            wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
        }
        focus_toplevel(toplevel);

        if (server->hsplit_active &&
            !toplevel->xdg_toplevel->parent) {
            struct poison_toplevel *prev_t =
                server->window_selector_prev_toplevel;
            struct poison_xwayland_view *prev_xw =
                server->window_selector_prev_xwayland_view;

            if (toplevel->in_hsplit) {
                int target_slot = -1;
                if (server->hsplit_focused_slot >= 0) {
                    target_slot = server->hsplit_focused_slot;
                } else if (prev_t && prev_t->in_hsplit) {
                    target_slot = prev_t->hsplit_order;
                } else if (prev_xw && prev_xw->in_hsplit) {
                    target_slot = prev_xw->hsplit_order;
                }
                if (target_slot >= 0 &&
                    target_slot != toplevel->hsplit_order) {
                    int old_order = toplevel->hsplit_order;
                    if (toplevel->hsplit_displaced_toplevel ||
                        toplevel->hsplit_displaced_xwayland) {
                        struct poison_toplevel *old_dt =
                            toplevel->hsplit_displaced_toplevel;
                        struct poison_xwayland_view *old_dxw =
                            toplevel->hsplit_displaced_xwayland;
                        if (prev_t && prev_t->in_hsplit &&
                            prev_t->hsplit_order == target_slot) {
                            hsplit_evict(prev_t);
                            toplevel->hsplit_displaced_toplevel = prev_t;
                            toplevel->hsplit_displaced_xwayland = NULL;
                        } else if (prev_xw && prev_xw->in_hsplit &&
                                   prev_xw->hsplit_order == target_slot) {
                            hsplit_evict_xwayland(prev_xw);
                            toplevel->hsplit_displaced_toplevel = NULL;
                            toplevel->hsplit_displaced_xwayland = prev_xw;
                        } else {
                            server->hsplit_focused_slot = -1;
                            toplevel->hsplit_displaced_toplevel = NULL;
                            toplevel->hsplit_displaced_xwayland = NULL;
                        }
                        toplevel->hsplit_order = target_slot;
                        if (old_dt && !old_dt->in_hsplit) {
                            old_dt->hsplit_order = old_order;
                            hsplit_place(old_dt,
                                         server->hsplit_toplevels.prev);
                        } else if (old_dxw && !old_dxw->in_hsplit) {
                            old_dxw->hsplit_order = old_order;
                            hsplit_place_xwayland(old_dxw,
                                                  server->hsplit_xwayland_views.prev);
                        }
                    } else {
                        if (prev_t && prev_t->in_hsplit &&
                            prev_t->hsplit_order == target_slot) {
                            prev_t->hsplit_order = old_order;
                        } else if (prev_xw && prev_xw->in_hsplit &&
                                   prev_xw->hsplit_order == target_slot) {
                            prev_xw->hsplit_order = old_order;
                        } else if (server->hsplit_focused_slot >= 0) {
                            server->hsplit_focused_slot = old_order;
                        }
                        toplevel->hsplit_order = target_slot;
                    }
                    apply_hsplit_layout(server);
                }
            } else {
                struct wl_list *pos;
                if (server->hsplit_focused_slot >= 0) {
                    toplevel->hsplit_order = server->hsplit_focused_slot;
                    server->hsplit_focused_slot = -1;
                    pos = server->hsplit_toplevels.prev;
                    toplevel->hsplit_displaced_toplevel = NULL;
                    toplevel->hsplit_displaced_xwayland = NULL;
                } else if (prev_t && prev_t->in_hsplit) {
                    toplevel->hsplit_order = prev_t->hsplit_order;
                    pos = hsplit_evict(prev_t);
                    toplevel->hsplit_displaced_toplevel = prev_t;
                    toplevel->hsplit_displaced_xwayland = NULL;
                } else if (prev_xw && prev_xw->in_hsplit) {
                    toplevel->hsplit_order = prev_xw->hsplit_order;
                    hsplit_evict_xwayland(prev_xw);
                    pos = server->hsplit_toplevels.prev;
                    toplevel->hsplit_displaced_toplevel = NULL;
                    toplevel->hsplit_displaced_xwayland = prev_xw;
                } else {
                    int total_now =
                        wl_list_length(&server->hsplit_toplevels) +
                        wl_list_length(&server->hsplit_xwayland_views);
                    if (total_now < 2) {
                        toplevel->hsplit_order = total_now > 0 ? 1 : 0;
                        pos = server->hsplit_toplevels.prev;
                        toplevel->hsplit_displaced_toplevel = NULL;
                        toplevel->hsplit_displaced_xwayland = NULL;
                    } else if (!wl_list_empty(&server->hsplit_toplevels)) {
                        struct poison_toplevel *last =
                            wl_container_of(server->hsplit_toplevels.prev,
                                            last, hsplit_link);
                        toplevel->hsplit_order = last->hsplit_order;
                        pos = hsplit_evict(last);
                        toplevel->hsplit_displaced_toplevel = last;
                        toplevel->hsplit_displaced_xwayland = NULL;
                    } else {
                        struct poison_xwayland_view *last_xw =
                            wl_container_of(server->hsplit_xwayland_views.prev,
                                            last_xw, hsplit_link);
                        toplevel->hsplit_order = last_xw->hsplit_order;
                        hsplit_evict_xwayland(last_xw);
                        pos = server->hsplit_toplevels.prev;
                        toplevel->hsplit_displaced_toplevel = NULL;
                        toplevel->hsplit_displaced_xwayland = last_xw;
                    }
                }
                hsplit_place(toplevel, pos);
                apply_hsplit_layout(server);
            }
        }
        update_cursor_focus(server);
    } else if (selected != NULL) {
        struct poison_xwayland_view *xwayland_view =
            wl_container_of(selected, xwayland_view, order);
        if (xwayland_view->scene_tree) {
            wlr_scene_node_raise_to_top(&xwayland_view->scene_tree->node);
        }
        focus_xwayland_view(server, xwayland_view);

        if (server->hsplit_active) {
            struct poison_toplevel *prev_t =
                server->window_selector_prev_toplevel;
            struct poison_xwayland_view *prev_xw =
                server->window_selector_prev_xwayland_view;

            if (xwayland_view->in_hsplit) {
                int target_slot = -1;
                if (server->hsplit_focused_slot >= 0) {
                    target_slot = server->hsplit_focused_slot;
                } else if (prev_t && prev_t->in_hsplit) {
                    target_slot = prev_t->hsplit_order;
                } else if (prev_xw && prev_xw->in_hsplit) {
                    target_slot = prev_xw->hsplit_order;
                }
                if (target_slot >= 0 &&
                    target_slot != xwayland_view->hsplit_order) {
                    int old_order = xwayland_view->hsplit_order;
                    if (xwayland_view->hsplit_displaced_toplevel ||
                        xwayland_view->hsplit_displaced_xwayland) {
                        struct poison_toplevel *old_dt =
                            xwayland_view->hsplit_displaced_toplevel;
                        struct poison_xwayland_view *old_dxw =
                            xwayland_view->hsplit_displaced_xwayland;
                        if (prev_t && prev_t->in_hsplit &&
                            prev_t->hsplit_order == target_slot) {
                            hsplit_evict(prev_t);
                            xwayland_view->hsplit_displaced_toplevel = prev_t;
                            xwayland_view->hsplit_displaced_xwayland = NULL;
                        } else if (prev_xw && prev_xw->in_hsplit &&
                                   prev_xw->hsplit_order == target_slot) {
                            hsplit_evict_xwayland(prev_xw);
                            xwayland_view->hsplit_displaced_toplevel = NULL;
                            xwayland_view->hsplit_displaced_xwayland = prev_xw;
                        } else {
                            server->hsplit_focused_slot = -1;
                            xwayland_view->hsplit_displaced_toplevel = NULL;
                            xwayland_view->hsplit_displaced_xwayland = NULL;
                        }
                        xwayland_view->hsplit_order = target_slot;
                        if (old_dt && !old_dt->in_hsplit) {
                            old_dt->hsplit_order = old_order;
                            hsplit_place(old_dt,
                                         server->hsplit_toplevels.prev);
                        } else if (old_dxw && !old_dxw->in_hsplit) {
                            old_dxw->hsplit_order = old_order;
                            hsplit_place_xwayland(old_dxw,
                                                  server->hsplit_xwayland_views.prev);
                        }
                    } else {
                        if (prev_t && prev_t->in_hsplit &&
                            prev_t->hsplit_order == target_slot) {
                            prev_t->hsplit_order = old_order;
                        } else if (prev_xw && prev_xw->in_hsplit &&
                                   prev_xw->hsplit_order == target_slot) {
                            prev_xw->hsplit_order = old_order;
                        } else if (server->hsplit_focused_slot >= 0) {
                            server->hsplit_focused_slot = old_order;
                        }
                        xwayland_view->hsplit_order = target_slot;
                    }
                    apply_hsplit_layout(server);
                }
            } else {
                if (server->hsplit_focused_slot >= 0) {
                    xwayland_view->hsplit_order = server->hsplit_focused_slot;
                    server->hsplit_focused_slot = -1;
                    xwayland_view->hsplit_displaced_toplevel = NULL;
                    xwayland_view->hsplit_displaced_xwayland = NULL;
                } else if (prev_t && prev_t->in_hsplit) {
                    xwayland_view->hsplit_order = prev_t->hsplit_order;
                    hsplit_evict(prev_t);
                    xwayland_view->hsplit_displaced_toplevel = prev_t;
                    xwayland_view->hsplit_displaced_xwayland = NULL;
                } else if (prev_xw && prev_xw->in_hsplit) {
                    xwayland_view->hsplit_order = prev_xw->hsplit_order;
                    hsplit_evict_xwayland(prev_xw);
                    xwayland_view->hsplit_displaced_toplevel = NULL;
                    xwayland_view->hsplit_displaced_xwayland = prev_xw;
                } else {
                    int total_now =
                        wl_list_length(&server->hsplit_toplevels) +
                        wl_list_length(&server->hsplit_xwayland_views);
                    if (total_now < 2) {
                        xwayland_view->hsplit_order = total_now > 0 ? 1 : 0;
                        xwayland_view->hsplit_displaced_toplevel = NULL;
                        xwayland_view->hsplit_displaced_xwayland = NULL;
                    } else if (!wl_list_empty(&server->hsplit_xwayland_views)) {
                        struct poison_xwayland_view *last_xw =
                            wl_container_of(server->hsplit_xwayland_views.prev,
                                            last_xw, hsplit_link);
                        xwayland_view->hsplit_order = last_xw->hsplit_order;
                        hsplit_evict_xwayland(last_xw);
                        xwayland_view->hsplit_displaced_toplevel = NULL;
                        xwayland_view->hsplit_displaced_xwayland = last_xw;
                    } else {
                        struct poison_toplevel *last_t =
                            wl_container_of(server->hsplit_toplevels.prev,
                                            last_t, hsplit_link);
                        xwayland_view->hsplit_order = last_t->hsplit_order;
                        hsplit_evict(last_t);
                        xwayland_view->hsplit_displaced_toplevel = last_t;
                        xwayland_view->hsplit_displaced_xwayland = NULL;
                    }
                }
                hsplit_place_xwayland(xwayland_view,
                                      server->hsplit_xwayland_views.prev);
                apply_hsplit_layout(server);
            }
        }
        update_cursor_focus(server);
    } else {
        if (server->window_selector_prev_xwayland_view) {
            focus_xwayland_view(server, server->window_selector_prev_xwayland_view);
        } else if (server->window_selector_prev_toplevel) {
            focus_toplevel(server->window_selector_prev_toplevel);
        }
        update_cursor_focus(server);
        server->prev_focused_toplevel = server->window_selector_saved_prev_toplevel;
        server->prev_focused_xwayland_view = server->window_selector_saved_prev_xwayland_view;
    }

    if (server->window_selector_event_source) {
        wl_event_source_remove(server->window_selector_event_source);
        server->window_selector_event_source = NULL;
    }
    close(fd);
    server->window_selector_fd = -1;
    server->window_selector_reply_len = 0;

    server->window_selector_prev_toplevel = NULL;
    server->window_selector_prev_xwayland_view = NULL;
    server->window_selector_saved_prev_toplevel = NULL;
    server->window_selector_saved_prev_xwayland_view = NULL;

    return 0;
}

static void restore_launcher_focus(struct poison_server *server, bool restore_prev_chain) {
    if (server->launcher_prev_xwayland_view) {
        focus_xwayland_view(server, server->launcher_prev_xwayland_view);
    } else if (server->launcher_prev_toplevel) {
        focus_toplevel(server->launcher_prev_toplevel);
    }
    update_cursor_focus(server);
    if (restore_prev_chain) {
        server->prev_focused_toplevel = server->launcher_saved_prev_toplevel;
        server->prev_focused_xwayland_view = server->launcher_saved_prev_xwayland_view;
    }
}

static int handle_launcher_result(int fd, uint32_t mask, void *data) {
    struct poison_server *server = data;
    (void)mask;

    char buffer[2048];
    ssize_t n = read(fd, buffer, sizeof(buffer) - 1);

    if (n > 0) {
        buffer[n] = '\0';

        if (buffer[n - 1] == '\n') {
            buffer[n - 1] = '\0';
        }

        if (buffer[0] == '\0') {
            restore_launcher_focus(server, true);
        } else {
            pid_t pid = fork();
            if (pid == 0) {
                setsid();

                int devnull = open("/dev/null", O_RDWR);
                if (devnull >= 0) {
                    dup2(devnull, STDIN_FILENO);
                    dup2(devnull, STDOUT_FILENO);
                    dup2(devnull, STDERR_FILENO);
                    if (devnull > 2) {
                        close(devnull);
                    }
                }

                int max_fd = sysconf(_SC_OPEN_MAX);
                if (max_fd < 0) {
                    max_fd = 1024;
                }
                for (int fd_iter = 3; fd_iter < max_fd; fd_iter++) {
                    close(fd_iter);
                }

                execl("/bin/sh", "/bin/sh", "-c", buffer, (void *)NULL);
                _exit(1);
            } else if (pid < 0) {
                wlr_log(WLR_ERROR, "Failed to fork to launch application!");
            }
            restore_launcher_focus(server, false);
        }
    } else {
        restore_launcher_focus(server, true);
    }

    if (server->launcher_event_source) {
        wl_event_source_remove(server->launcher_event_source);
        server->launcher_event_source = NULL;
    }
    close(fd);
    server->launcher_fd = -1;

    server->launcher_prev_toplevel = NULL;
    server->launcher_prev_xwayland_view = NULL;
    server->launcher_saved_prev_toplevel = NULL;
    server->launcher_saved_prev_xwayland_view = NULL;

    return 0;
}

/* Resolve any grab conflicts. */
static void dismiss_popups(struct poison_server *server) {
    if (!server->focused_toplevel || !server->focused_toplevel->xdg_toplevel) {
        return;
    }

    struct wlr_xdg_surface *base = server->focused_toplevel->xdg_toplevel->base;
    if (!base) {
        return;
    }

    struct wlr_xdg_popup *popup, *tmp;
    wl_list_for_each_safe(popup, tmp, &base->popups, link) {
        wlr_xdg_popup_destroy(popup);
    }
}

void start_window_selector(struct poison_server *server) {
    if (wl_list_empty(&server->view_order)) {
        return;
    }

    if (server->window_selector_fd != -1 || server->launcher_fd != -1 ||
        server->console_pid > 0) {
        return;
    }

    dismiss_popups(server);

    server->window_selector_prev_toplevel = server->focused_toplevel;
    server->window_selector_prev_xwayland_view = server->focused_xwayland_view;
    server->window_selector_saved_prev_toplevel = server->prev_focused_toplevel;
    server->window_selector_saved_prev_xwayland_view = server->prev_focused_xwayland_view;

    wlr_seat_keyboard_notify_clear_focus(server->seat);

    server->focused_toplevel = NULL;
    server->focused_xwayland_view = NULL;

    int input_pipe[2];
    int output_pipe[2];

    if (pipe(input_pipe) == -1) {
        wlr_log(WLR_ERROR, "Failed to create pipes for window selector!");
        return;
    }
    if (pipe(output_pipe) == -1) {
        wlr_log(WLR_ERROR, "Failed to create pipes for window selector!");
        close(input_pipe[0]);
        close(input_pipe[1]);
        return;
    }

    pid_t pid = fork();
    if (pid == -1) {
        wlr_log(WLR_ERROR, "Failed to fork window selector!");
        close(input_pipe[0]);
        close(input_pipe[1]);
        close(output_pipe[0]);
        close(output_pipe[1]);
        return;
    }

    if (pid == 0) {
        close(input_pipe[1]);
        close(output_pipe[0]);

        dup2(input_pipe[0], STDIN_FILENO);
        close(input_pipe[0]);

        dup2(output_pipe[1], STDOUT_FILENO);
        close(output_pipe[1]);

        execlp("poison-window-selector", "poison-window-selector", (char *)NULL);
        execl("/usr/local/bin/poison-window-selector", "poison-window-selector", (char *)NULL);
        execl("./poison-window-selector", "poison-window-selector", (char *)NULL);

        _exit(1);
    }

    close(input_pipe[0]);
    close(output_pipe[1]);

    FILE *input_file = fdopen(input_pipe[1], "w");
    if (!input_file) {
        close(input_pipe[1]);
        close(output_pipe[0]);
        wlr_log(WLR_ERROR, "Failed to create FILE* for input pipe!");
        return;
    }

    struct poison_view_order *node;
    wl_list_for_each(node, &server->view_order, link) {
        const char *title;
        if (node->type == POISON_VIEW_XDG) {
            struct poison_toplevel *toplevel =
                wl_container_of(node, toplevel, order);
            title = get_toplevel_title(toplevel);
        } else {
            struct poison_xwayland_view *xwayland_view =
                wl_container_of(node, xwayland_view, order);
            title = get_xwayland_title(xwayland_view);
        }
        fprintf(input_file, "%u %s\n", node->serial, title);
    }

    fclose(input_file);

    server->window_selector_fd = output_pipe[0];
    server->window_selector_pid = pid;
    server->window_selector_reply_len = 0;

    struct wl_event_loop *event_loop = wl_display_get_event_loop(server->wl_display);
    server->window_selector_event_source = wl_event_loop_add_fd(
        event_loop, output_pipe[0], WL_EVENT_READABLE,
        handle_window_selector_result, server);

    if (!server->window_selector_event_source) {
        wlr_log(WLR_ERROR, "Failed to add window selector fd to event loop!");
        close(output_pipe[0]);
        server->window_selector_fd = -1;
    }
}

void stop_window_selector(struct poison_server *server, bool select) {
    (void)server;
    (void)select;
}

void window_selector_move_selection(struct poison_server *server, int direction) {
    (void)server;
    (void)direction;
}

void start_application_launcher(struct poison_server *server) {
    if (server->launcher_fd != -1 || server->window_selector_fd != -1 ||
        server->console_pid > 0) {
        return;
    }

    dismiss_popups(server);

    server->launcher_prev_toplevel = server->focused_toplevel;
    server->launcher_prev_xwayland_view = server->focused_xwayland_view;
    server->launcher_saved_prev_toplevel = server->prev_focused_toplevel;
    server->launcher_saved_prev_xwayland_view = server->prev_focused_xwayland_view;

    wlr_seat_keyboard_notify_clear_focus(server->seat);

    server->focused_toplevel = NULL;
    server->focused_xwayland_view = NULL;

    int output_pipe[2];

    if (pipe(output_pipe) == -1) {
        wlr_log(WLR_ERROR, "Failed to create pipes for launcher!");
        return;
    }

    pid_t pid = fork();
    if (pid == -1) {
        wlr_log(WLR_ERROR, "Failed to fork launcher!");
        close(output_pipe[0]);
        close(output_pipe[1]);
        return;
    }

    if (pid == 0) {
        close(output_pipe[0]);

        dup2(output_pipe[1], STDOUT_FILENO);
        close(output_pipe[1]);

        execlp("poison-launcher", "poison-launcher", (char *)NULL);
        execl("/usr/local/bin/poison-launcher", "poison-launcher", (char *)NULL);
        execl("./poison-launcher", "poison-launcher", (char *)NULL);

        _exit(1);
    }

    close(output_pipe[1]);

    server->launcher_fd = output_pipe[0];
    server->launcher_pid = pid;

    struct wl_event_loop *event_loop = wl_display_get_event_loop(server->wl_display);
    server->launcher_event_source = wl_event_loop_add_fd(
        event_loop, output_pipe[0], WL_EVENT_READABLE,
        handle_launcher_result, server);

    if (!server->launcher_event_source) {
        wlr_log(WLR_ERROR, "Failed to add launcher fd to event loop!");
        close(output_pipe[0]);
        server->launcher_fd = -1;
    }
}

static void show_split_indicator_impl(struct poison_server *server,
                                      const char *override_msg) {
    kill_tracked_child(&server->indicator_pid);

    const char *title = override_msg;
    int focused_idx = -1;

    if (!override_msg) {
        if (server->focused_toplevel && server->focused_toplevel->in_hsplit) {
            title = get_toplevel_title(server->focused_toplevel);
            focused_idx = server->focused_toplevel->hsplit_order;
        } else if (server->focused_xwayland_view &&
                   server->focused_xwayland_view->in_hsplit) {
            title = get_xwayland_title(server->focused_xwayland_view);
            focused_idx = server->focused_xwayland_view->hsplit_order;
        } else if (server->hsplit_focused_slot >= 0) {
            title = "";
            focused_idx = server->hsplit_focused_slot;
        }
    } else {
        if (server->focused_toplevel && server->focused_toplevel->in_hsplit) {
            focused_idx = server->focused_toplevel->hsplit_order;
        } else if (server->focused_xwayland_view &&
                   server->focused_xwayland_view->in_hsplit) {
            focused_idx = server->focused_xwayland_view->hsplit_order;
        } else if (server->hsplit_focused_slot >= 0) {
            focused_idx = server->hsplit_focused_slot;
        }
    }

    if (focused_idx < 0) {
        return;
    }

    struct wlr_box output_box;
    wlr_output_layout_get_box(server->output_layout, NULL, &output_box);
    if (output_box.width <= 0 || output_box.height <= 0) {
        return;
    }

    int n = wl_list_length(&server->hsplit_toplevels) +
        wl_list_length(&server->hsplit_xwayland_views);
    int n_slots = n < 2 ? 2 : n;
    int effective_padding = server->config.padding == 0 ? 1 : server->config.padding;
    int total_width = output_box.width - 2 * effective_padding;
    int height = output_box.height - 2 * effective_padding;
    int total_gap = (n_slots - 1) * HSPLIT_GAP;
    int slot_width = (total_width - total_gap) / n_slots;

    int frame_x = output_box.x + effective_padding +
        focused_idx * (slot_width + HSPLIT_GAP);
    int frame_y = output_box.y + effective_padding;

    char title_buf[1024];
    strncpy(title_buf, title ? title : "(empty)", sizeof(title_buf) - 1);
    title_buf[sizeof(title_buf) - 1] = '\0';

    char sx[16], sy[16], sw[16], sh[16];
    snprintf(sx, sizeof(sx), "%d", frame_x);
    snprintf(sy, sizeof(sy), "%d", frame_y);
    snprintf(sw, sizeof(sw), "%d", slot_width);
    snprintf(sh, sizeof(sh), "%d", height);

    pid_t pid = fork();
    if (pid == 0) {
        setsid();

        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) {
                close(devnull);
            }
        }

        execlp("poison-indicator", "poison-indicator",
               title_buf, sx, sy, sw, sh, (char *)NULL);
        execl("/usr/local/bin/poison-indicator", "poison-indicator",
              title_buf, sx, sy, sw, sh, (char *)NULL);
        execl("./poison-indicator", "poison-indicator",
              title_buf, sx, sy, sw, sh, (char *)NULL);
        _exit(1);
    } else if (pid > 0) {
        server->indicator_pid = pid;
    }
}

void show_split_indicator(struct poison_server *server) {
    if (!server->hsplit_active) {
        return;
    }
    show_split_indicator_impl(server, NULL);
}

static void start_notification_daemon(struct poison_server *server) {
    if (!server->config.notifications_enabled) {
        return;
    }
    if (server->notify_pid > 0) {
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        setsid();

        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) {
                close(devnull);
            }
        }

        execlp("poison-notify", "poison-notify", (char *)NULL);
        execl("/usr/local/bin/poison-notify", "poison-notify", (char *)NULL);
        execl("./poison-notify", "poison-notify", (char *)NULL);
        _exit(1);
    } else if (pid > 0) {
        server->notify_pid = pid;
        wlr_log(WLR_INFO, "Started notification daemon (pid %d).", pid);
    } else {
        wlr_log(WLR_ERROR, "Failed to fork notification daemon!");
    }
}

static bool is_layer_exclusive_focused(struct poison_server *server) {
    struct wlr_surface *focused =
        server->seat->keyboard_state.focused_surface;
    if (!focused) {
        return false;
    }
    struct wlr_layer_surface_v1 *layer_surface =
        wlr_layer_surface_v1_try_from_wlr_surface(focused);
    return layer_surface != NULL &&
        layer_surface->current.keyboard_interactive ==
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
}

static bool toplevel_is_transient_dialog(struct poison_toplevel *toplevel) {
    struct wlr_xdg_toplevel *xdg = toplevel->xdg_toplevel;
    if (!xdg || !xdg->parent) {
        return false;
    }

    struct wlr_xdg_toplevel_state *state = &xdg->current;
    return state->min_width > 0 && state->min_height > 0 &&
        state->min_width == state->max_width &&
        state->min_height == state->max_height;
}

static struct wl_client *toplevel_client(struct poison_toplevel *toplevel) {
    if (!toplevel || !toplevel->xdg_toplevel ||
        !toplevel->xdg_toplevel->base ||
        !toplevel->xdg_toplevel->base->surface) {
        return NULL;
    }
    return wl_resource_get_client(
        toplevel->xdg_toplevel->base->surface->resource);
}

/* A hack for races between an unparented dialog and its main window. */
static bool focus_held_by_recent_sibling(struct poison_server *server,
                                         struct poison_toplevel *mapped) {
    struct poison_toplevel *focused = server->focused_toplevel;
    if (!focused || focused == mapped) {
        return false;
    }

    if (mapped->xdg_toplevel && mapped->xdg_toplevel->parent) {
        return false;
    }

    struct wl_client *client = toplevel_client(mapped);
    if (client == NULL || toplevel_client(focused) != client) {
        return false;
    }

    int mapped_width, mapped_height, focused_width, focused_height;
    toplevel_visible_size(mapped, &mapped_width, &mapped_height);
    toplevel_visible_size(focused, &focused_width, &focused_height);
    if (mapped_width > 0 && mapped_height > 0 && focused_width > 0 &&
        focused_height > 0 &&
        (int64_t)mapped_width * mapped_height <
            (int64_t)focused_width * focused_height) {
        return false;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t dt_ms = (int64_t)(now.tv_sec - focused->map_time.tv_sec) * 1000 +
        (now.tv_nsec - focused->map_time.tv_nsec) / 1000000;
    return dt_ms >= 0 && dt_ms <= 1000;
}

void toplevel_map(struct wl_listener *listener, void *data) {
    struct poison_toplevel *toplevel =
        wl_container_of(listener, toplevel, map);
    struct poison_server *server = toplevel->server;
    clock_gettime(CLOCK_MONOTONIC, &toplevel->map_time);
    wl_list_insert(&server->toplevels, &toplevel->link);
    toplevel->order.serial = ++server->next_view_serial;
    wl_list_insert(server->view_order.prev, &toplevel->order.link);

    bool inhibit_focus = is_layer_exclusive_focused(server);

    if (toplevel->xdg_toplevel &&
        toplevel->xdg_toplevel->requested.fullscreen &&
        !server->hsplit_active) {
        wlr_scene_node_set_position(&toplevel->scene_tree->node, 0, 0);
        if (!inhibit_focus) {
            focus_toplevel(toplevel);
        }
        return;
    }

    if (server->hsplit_active && toplevel->xdg_toplevel &&
        !toplevel->xdg_toplevel->parent) {
        if (server->hsplit_focused_slot >= 0) {
            toplevel->hsplit_order = server->hsplit_focused_slot;
            hsplit_place(toplevel, server->hsplit_toplevels.prev);
            server->hsplit_focused_slot = -1;
            apply_hsplit_layout(server);
            if (!inhibit_focus) {
                focus_toplevel(toplevel);
            }
            return;
        }
        struct poison_toplevel *displaced = NULL;
        struct poison_xwayland_view *displaced_xw = NULL;
        if (server->focused_toplevel && server->focused_toplevel->in_hsplit) {
            displaced = server->focused_toplevel;
        } else if (server->focused_xwayland_view &&
                   server->focused_xwayland_view->in_hsplit) {
            displaced_xw = server->focused_xwayland_view;
        } else if (!wl_list_empty(&server->hsplit_toplevels)) {
            displaced = wl_container_of(server->hsplit_toplevels.prev,
                                        displaced, hsplit_link);
        } else if (!wl_list_empty(&server->hsplit_xwayland_views)) {
            displaced_xw = wl_container_of(server->hsplit_xwayland_views.prev,
                                           displaced_xw, hsplit_link);
        }
        if (displaced) {
            toplevel->hsplit_order = displaced->hsplit_order;
            struct wl_list *pos = hsplit_evict(displaced);
            hsplit_place(toplevel, pos);
            toplevel->hsplit_displaced_toplevel = displaced;
            toplevel->hsplit_displaced_xwayland = NULL;
        } else if (displaced_xw) {
            toplevel->hsplit_order = displaced_xw->hsplit_order;
            hsplit_evict_xwayland(displaced_xw);
            hsplit_place(toplevel, server->hsplit_toplevels.prev);
            toplevel->hsplit_displaced_toplevel = NULL;
            toplevel->hsplit_displaced_xwayland = displaced_xw;
        } else {
            center_toplevel(toplevel);
            if (!inhibit_focus) {
                focus_toplevel(toplevel);
            }
            return;
        }
        apply_hsplit_layout(server);
        if (!inhibit_focus) {
            focus_toplevel(toplevel);
        }
        return;
    }

    if (toplevel_is_transient_dialog(toplevel) && !toplevel->floating) {
        toplevel->floating = true;
        poison_toplevel_set_tiling(toplevel, false, false);
        wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
    }

    if (toplevel->floating) {
        center_toplevel(toplevel);
    } else {
        place_tiled_toplevel(toplevel);
    }
    poison_decoration_update_toplevel(toplevel);
    if (!inhibit_focus) {
        if (focus_held_by_recent_sibling(server, toplevel)) {
            struct poison_toplevel *held = server->focused_toplevel;
            if (held->scene_tree) {
                wlr_scene_node_raise_to_top(&held->scene_tree->node);
            }
            server->prev_focused_toplevel = toplevel;
        } else {
            focus_toplevel(toplevel);
        }
    }
}

static struct poison_toplevel *hsplit_live_toplevel(
    struct poison_server *server, struct poison_toplevel *toplevel) {
    struct poison_toplevel *it;
    wl_list_for_each(it, &server->toplevels, link) {
        if (it == toplevel) {
            return toplevel;
        }
    }

    return NULL;
}

static struct poison_xwayland_view *hsplit_live_xwayland(
    struct poison_server *server, struct poison_xwayland_view *view) {
    struct poison_xwayland_view *it;
    wl_list_for_each(it, &server->xwayland_views, link) {
        if (it == view) {
            return view;
        }
    }

    return NULL;
}

void toplevel_unmap(struct wl_listener *listener, void *data) {
    struct poison_toplevel *toplevel =
        wl_container_of(listener, toplevel, unmap);
    struct poison_server *server = toplevel->server;

    if (toplevel->xdg_toplevel && toplevel->xdg_toplevel->base) {
        deactivate_constraint_for_surface(server,
                                          toplevel->xdg_toplevel->base->surface);
    }

    poison_decoration_destroy_toplevel(toplevel);

    bool was_focused = (server->focused_toplevel == toplevel);
    bool was_in_hsplit = toplevel->in_hsplit;
    struct poison_toplevel *restore_t = NULL;
    struct poison_xwayland_view *restore_xw = NULL;

    if (server->prev_focused_toplevel == toplevel) {
        server->prev_focused_toplevel = NULL;
    }

    if (server->window_selector_prev_toplevel == toplevel) {
        server->window_selector_prev_toplevel = NULL;
    }

    if (server->window_selector_saved_prev_toplevel == toplevel) {
        server->window_selector_saved_prev_toplevel = NULL;
    }

    if (server->launcher_prev_toplevel == toplevel) {
        server->launcher_prev_toplevel = NULL;
    }

    if (server->launcher_saved_prev_toplevel == toplevel) {
        server->launcher_saved_prev_toplevel = NULL;
    }

    if (was_focused) {
        server->focused_toplevel = NULL;
    }

    if (toplevel->in_hsplit) {
        restore_t = hsplit_live_toplevel(server,
                                         toplevel->hsplit_displaced_toplevel);
        restore_xw = hsplit_live_xwayland(server,
                                          toplevel->hsplit_displaced_xwayland);
        struct wl_list *pos = toplevel->hsplit_link.prev;
        toplevel->in_hsplit = false;
        wl_list_remove(&toplevel->hsplit_link);
        wl_list_init(&toplevel->hsplit_link);
        if (restore_t && !restore_t->in_hsplit) {
            restore_t->hsplit_order = toplevel->hsplit_order;
            hsplit_place(restore_t, pos);
            apply_hsplit_layout(server);
        } else if (restore_xw && !restore_xw->in_hsplit) {
            restore_xw->hsplit_order = toplevel->hsplit_order;
            hsplit_place_xwayland(restore_xw, &server->hsplit_xwayland_views);
            apply_hsplit_layout(server);
        } else {
            hsplit_update_after_removal(server);
        }
    }

    wl_list_remove(&toplevel->link);
    wl_list_init(&toplevel->link);
    wl_list_remove(&toplevel->order.link);
    wl_list_init(&toplevel->order.link);

    {
        struct poison_toplevel *t;
        wl_list_for_each(t, &server->toplevels, link) {
            if (t->hsplit_displaced_toplevel == toplevel) {
                t->hsplit_displaced_toplevel = NULL;
            }
        }
        struct poison_xwayland_view *xw;
        wl_list_for_each(xw, &server->xwayland_views, link) {
            if (xw->hsplit_displaced_toplevel == toplevel) {
                xw->hsplit_displaced_toplevel = NULL;
            }
        }
    }

    if (was_focused) {
        bool split_windows_remain = server->hsplit_active &&
            (!wl_list_empty(&server->hsplit_toplevels) ||
             !wl_list_empty(&server->hsplit_xwayland_views));
        if (was_in_hsplit && split_windows_remain) {
            if (restore_t && restore_t->in_hsplit) {
                focus_toplevel(restore_t);
            } else if (restore_xw && restore_xw->in_hsplit) {
                focus_xwayland_view(server, restore_xw);
            } else {
                hsplit_restore_focus(server);
            }
        } else {
            restore_toplevel_focus(server);
        }
    }

    if (wl_list_empty(&server->toplevels) && wl_list_empty(&server->xwayland_views)) {
        reset_cursor_to_default(server);
    }
}

/* Intended to prevent click stealing at edges. */
static void update_toplevel_clip(struct poison_toplevel *toplevel) {
    if (!toplevel || !toplevel->surface_tree || !toplevel->xdg_toplevel ||
        !toplevel->xdg_toplevel->base) {
        return;
    }

    struct wlr_box clip = {0};
    if (!toplevel->floating && !toplevel_fullscreen_now(toplevel)) {
        clip = toplevel->xdg_toplevel->base->geometry;
    }

    /* Don't clip sibling popups. */
    wlr_scene_subsurface_tree_set_clip(&toplevel->surface_tree->node, &clip);
}

static void announce_toplevel_capabilities(struct poison_toplevel *toplevel) {
    wlr_xdg_toplevel_set_wm_capabilities(
        toplevel->xdg_toplevel, WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE | WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN);

    struct wlr_box output_box;
    wlr_output_layout_get_box(toplevel->server->output_layout, NULL,
                              &output_box);
    int ep = toplevel->server->config.padding == 0
        ? 1
        : toplevel->server->config.padding;
    struct poison_frame_insets insets = poison_toplevel_insets(toplevel);
    int bounds_width = output_box.width - 2 * ep - insets.left - insets.right;
    int bounds_height = output_box.height - 2 * ep - insets.top - insets.bottom;
    if (bounds_width > 0 && bounds_height > 0) {
        wlr_xdg_toplevel_set_bounds(toplevel->xdg_toplevel, bounds_width,
                                    bounds_height);
    }
}

void toplevel_commit(struct wl_listener *listener, void *data) {
    struct poison_toplevel *toplevel =
        wl_container_of(listener, toplevel, commit);

    if (toplevel->xdg_toplevel->base->initial_commit) {
        announce_toplevel_capabilities(toplevel);

        if (toplevel->xdg_toplevel->requested.fullscreen) {
            struct wlr_box output_box;
            wlr_output_layout_get_box(toplevel->server->output_layout, NULL,
                                      &output_box);
            if (output_box.width > 0 && output_box.height > 0) {
                poison_toplevel_set_tiling(toplevel, false, false);
                wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
                                          output_box.width, output_box.height);
                wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, true);
            } else {
                wlr_log(WLR_ERROR, "Invalid output dimensions for fullscreen: %dx%d!",
                        output_box.width, output_box.height);
            }
        } else if (toplevel->floating) {
            poison_toplevel_set_tiling(toplevel, false, toplevel->maximized);
            wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
        } else {
            struct wlr_box output_box;
            wlr_output_layout_get_box(toplevel->server->output_layout, NULL,
                                      &output_box);

            int effective_padding = toplevel->server->config.padding == 0 ? 1 : toplevel->server->config.padding;

            poison_toplevel_set_tiling(toplevel, true, true);
            if (output_box.width > 2 * effective_padding &&
                output_box.height > 2 * effective_padding) {
                poison_toplevel_apply_size(toplevel,
                                           output_box.width - 2 * effective_padding,
                                           output_box.height - 2 * effective_padding);
            } else {
                wlr_log(WLR_ERROR, "Output dimensions too small!");
                wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
                                          output_box.width, output_box.height);
            }
        }
        return;
    }

    apply_needs_retile(toplevel);

    if (!toplevel_fullscreen_now(toplevel) && !toplevel->in_hsplit) {
        if (toplevel->floating) {
            clamp_floating_toplevel(toplevel);
            center_toplevel(toplevel);
        } else {
            place_tiled_toplevel(toplevel);
        }
    }
    update_toplevel_clip(toplevel);
    poison_decoration_update_toplevel(toplevel);
}

void toplevel_destroy(struct wl_listener *listener, void *data) {
    struct poison_toplevel *toplevel =
        wl_container_of(listener, toplevel, destroy);
    struct poison_server *server = toplevel->server;

    bool was_focused = (server->focused_toplevel == toplevel);
    bool was_in_list = !wl_list_empty(&toplevel->link);
    bool was_in_hsplit = toplevel->in_hsplit;
    struct poison_toplevel *restore_t = NULL;
    struct poison_xwayland_view *restore_xw = NULL;

    if (was_focused) {
        server->focused_toplevel = NULL;
    }
    if (server->prev_focused_toplevel == toplevel) {
        server->prev_focused_toplevel = NULL;
    }
    if (server->window_selector_prev_toplevel == toplevel) {
        server->window_selector_prev_toplevel = NULL;
    }
    if (server->window_selector_saved_prev_toplevel == toplevel) {
        server->window_selector_saved_prev_toplevel = NULL;
    }
    if (server->launcher_prev_toplevel == toplevel) {
        server->launcher_prev_toplevel = NULL;
    }
    if (server->launcher_saved_prev_toplevel == toplevel) {
        server->launcher_saved_prev_toplevel = NULL;
    }

    wl_list_remove(&toplevel->map.link);
    wl_list_remove(&toplevel->unmap.link);
    wl_list_remove(&toplevel->commit.link);
    wl_list_remove(&toplevel->destroy.link);
    wl_list_remove(&toplevel->request_fullscreen.link);
    wl_list_remove(&toplevel->request_maximize.link);
    wl_list_remove(&toplevel->new_popup.link);
    wl_list_remove(&toplevel->set_title.link);

    poison_decoration_destroy_toplevel(toplevel);
    if (toplevel->scene_tree) {
        toplevel->scene_tree->node.data = NULL;
        wlr_scene_node_destroy(&toplevel->scene_tree->node);
        toplevel->scene_tree = NULL;
        toplevel->surface_tree = NULL;
    }

    if (toplevel->in_hsplit) {
        restore_t = hsplit_live_toplevel(server,
                                         toplevel->hsplit_displaced_toplevel);
        restore_xw = hsplit_live_xwayland(server,
                                          toplevel->hsplit_displaced_xwayland);
        struct wl_list *pos = toplevel->hsplit_link.prev;
        toplevel->in_hsplit = false;
        wl_list_remove(&toplevel->hsplit_link);
        if (restore_t && !restore_t->in_hsplit) {
            restore_t->hsplit_order = toplevel->hsplit_order;
            hsplit_place(restore_t, pos);
            apply_hsplit_layout(server);
        } else if (restore_xw && !restore_xw->in_hsplit) {
            restore_xw->hsplit_order = toplevel->hsplit_order;
            hsplit_place_xwayland(restore_xw, &server->hsplit_xwayland_views);
            apply_hsplit_layout(server);
        } else {
            hsplit_update_after_removal(server);
        }
    }

    if (!wl_list_empty(&toplevel->link)) {
        wl_list_remove(&toplevel->link);
    }
    if (!wl_list_empty(&toplevel->order.link)) {
        wl_list_remove(&toplevel->order.link);
    }

    {
        struct poison_toplevel *t;
        wl_list_for_each(t, &server->toplevels, link) {
            if (t->hsplit_displaced_toplevel == toplevel) {
                t->hsplit_displaced_toplevel = NULL;
            }
        }
        struct poison_xwayland_view *xw;
        wl_list_for_each(xw, &server->xwayland_views, link) {
            if (xw->hsplit_displaced_toplevel == toplevel) {
                xw->hsplit_displaced_toplevel = NULL;
            }
        }
    }

    if (was_focused && was_in_list) {
        bool split_windows_remain = server->hsplit_active &&
            (!wl_list_empty(&server->hsplit_toplevels) ||
             !wl_list_empty(&server->hsplit_xwayland_views));
        if (was_in_hsplit && split_windows_remain) {
            if (restore_t && restore_t->in_hsplit) {
                focus_toplevel(restore_t);
            } else if (restore_xw && restore_xw->in_hsplit) {
                focus_xwayland_view(server, restore_xw);
            } else {
                hsplit_restore_focus(server);
            }
        } else {
            restore_toplevel_focus(server);
        }
    }

    if (was_in_list &&
        wl_list_empty(&server->toplevels) &&
        wl_list_empty(&server->xwayland_views)) {
        reset_cursor_to_default(server);
    }

    free(toplevel);
}

void toplevel_request_fullscreen(struct wl_listener *listener, void *data) {
    struct poison_toplevel *toplevel =
        wl_container_of(listener, toplevel, request_fullscreen);
    struct wlr_xdg_toplevel *xdg_toplevel = toplevel->xdg_toplevel;

    if (!xdg_toplevel) {
        return;
    }

    if (!xdg_toplevel->base->initialized) {
        return;
    }

    if (xdg_toplevel->requested.fullscreen) {
        if (toplevel->in_hsplit) {
            apply_hsplit_layout(toplevel->server);
            wlr_xdg_toplevel_set_fullscreen(xdg_toplevel, false);
            return;
        }
        if (xdg_toplevel->base->surface->mapped &&
            !xdg_toplevel->current.fullscreen) {
            toplevel->pre_fullscreen_floating = toplevel->floating;
        }
        struct wlr_box output_box;
        wlr_output_layout_get_box(toplevel->server->output_layout, NULL,
                                  &output_box);

        if (output_box.width > 0 && output_box.height > 0) {
            poison_toplevel_set_tiling(toplevel, false, false);
            wlr_scene_node_set_position(&toplevel->scene_tree->node, 0, 0);
            wlr_xdg_toplevel_set_size(xdg_toplevel,
                                      output_box.width, output_box.height);
        } else {
            wlr_log(WLR_ERROR, "Invalid output dimensions for fullscreen!");
            return;
        }
    } else {
        if (toplevel->in_hsplit) {
            apply_hsplit_layout(toplevel->server);
            wlr_xdg_toplevel_set_fullscreen(xdg_toplevel, false);
            return;
        }
        wlr_xdg_toplevel_set_fullscreen(xdg_toplevel, false);

        leave_toplevel_fullscreen(toplevel);
    }

    wlr_xdg_toplevel_set_fullscreen(xdg_toplevel,
                                    xdg_toplevel->requested.fullscreen);
}

void toplevel_request_maximize(struct wl_listener *listener, void *data) {
    (void)data;
    struct poison_toplevel *toplevel =
        wl_container_of(listener, toplevel, request_maximize);
    struct wlr_xdg_toplevel *xdg_toplevel = toplevel->xdg_toplevel;

    if (!xdg_toplevel || !xdg_toplevel->base->initialized) {
        return;
    }

    if (toplevel->in_hsplit || toplevel_fullscreen_now(toplevel)) {
        wlr_xdg_toplevel_set_maximized(xdg_toplevel, false);
        return;
    }

    if (toplevel->floating) {
        if (xdg_toplevel->requested.maximized) {
            if (!toplevel->maximized) {
                struct wlr_box geo = xdg_toplevel->base->geometry;
                if (geo.width > 0 && geo.height > 0) {
                    toplevel->saved_float_width = geo.width;
                    toplevel->saved_float_height = geo.height;
                }
                if (!maximize_toplevel(toplevel)) {
                    wlr_xdg_toplevel_set_maximized(xdg_toplevel, false);
                }
            } else {
                wlr_xdg_toplevel_set_maximized(xdg_toplevel, true);
            }
        } else if (toplevel->maximized) {
            toplevel->maximized = false;
            float_toplevel(toplevel);
        } else {
            wlr_xdg_toplevel_set_maximized(xdg_toplevel, false);
        }
        return;
    }

    struct wlr_box output_box;
    wlr_output_layout_get_box(toplevel->server->output_layout, NULL,
                              &output_box);
    int effective_padding = toplevel->server->config.padding == 0
        ? 1
        : toplevel->server->config.padding;

    poison_toplevel_set_tiling(toplevel, true, true);
    if (output_box.width > 2 * effective_padding &&
        output_box.height > 2 * effective_padding) {
        poison_toplevel_apply_box(toplevel,
                                  output_box.x + effective_padding,
                                  output_box.y + effective_padding,
                                  output_box.width - 2 * effective_padding,
                                  output_box.height - 2 * effective_padding);
    }
}

void popup_reposition(struct wl_listener *listener, void *data) {
    struct poison_popup *popup =
        wl_container_of(listener, popup, reposition);

    struct wlr_output *output =
        wlr_output_layout_get_center_output(popup->server->output_layout);

    if (!output) {
        return;
    }

    struct wlr_box output_box;
    wlr_output_layout_get_box(popup->server->output_layout,
                              output, &output_box);

    int root_lx, root_ly;
    wlr_scene_node_coords(&popup->root_tree->node, &root_lx, &root_ly);

    output_box.x -= root_lx;
    output_box.y -= root_ly;

    wlr_xdg_popup_unconstrain_from_box(popup->xdg_popup, &output_box);
}

void popup_commit(struct wl_listener *listener, void *data) {
    struct poison_popup *popup = wl_container_of(listener, popup, commit);

    if (popup->xdg_popup->base->initial_commit) {
        popup_reposition(&popup->reposition, NULL);
    }
}

void popup_map(struct wl_listener *listener, void *data) {
    struct poison_popup *popup = wl_container_of(listener, popup, map);

    wlr_scene_node_raise_to_top(&popup->scene_tree->node);
}

void popup_unmap(struct wl_listener *listener, void *data) {
    struct poison_popup *popup = wl_container_of(listener, popup, unmap);
    struct poison_server *server = popup->server;
    struct wlr_seat *seat = server->seat;

    if (seat->keyboard_state.focused_surface == popup->xdg_popup->base->surface) {
        if (popup->root_tree && popup->root_tree->node.data) {
            struct poison_toplevel *parent_toplevel = popup->root_tree->node.data;
            struct poison_toplevel *t;
            bool found = false;
            wl_list_for_each(t, &server->toplevels, link) {
                if (t == parent_toplevel) {
                    found = true;
                    break;
                }
            }
            if (found) {
                focus_toplevel(parent_toplevel);
                return;
            }

            struct poison_xwayland_view *parent_xwayland = popup->root_tree->node.data;
            struct poison_xwayland_view *xw;
            wl_list_for_each(xw, &server->xwayland_views, link) {
                if (xw == parent_xwayland) {
                    focus_xwayland_view(server, parent_xwayland);
                    return;
                }
            }
        }

        wlr_seat_keyboard_notify_clear_focus(seat);
    }
}

void popup_destroy(struct wl_listener *listener, void *data) {
    struct poison_popup *popup = wl_container_of(listener, popup, destroy);

    wl_list_remove(&popup->map.link);
    wl_list_remove(&popup->unmap.link);
    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->destroy.link);
    wl_list_remove(&popup->new_popup.link);
    wl_list_remove(&popup->reposition.link);

    free(popup);
}

void popup_new_popup(struct wl_listener *listener, void *data) {
    struct poison_popup *popup =
        wl_container_of(listener, popup, new_popup);
    struct wlr_xdg_popup *wlr_popup = data;

    struct poison_popup *new_popup = calloc(1, sizeof(*new_popup));
    if (!new_popup) {
        wlr_log(WLR_ERROR, "Failed to allocate popup!");
        return;
    }
    new_popup->server = popup->server;
    new_popup->xdg_popup = wlr_popup;
    new_popup->root_tree = popup->root_tree;
    new_popup->scene_tree =
        wlr_scene_xdg_surface_create(popup->scene_tree, wlr_popup->base);
    if (!new_popup->scene_tree) {
        wlr_log(WLR_ERROR, "Failed to create scene tree for popup!");
        free(new_popup);
        return;
    }

    new_popup->map.notify = popup_map;
    wl_signal_add(&wlr_popup->base->surface->events.map, &new_popup->map);
    new_popup->unmap.notify = popup_unmap;
    wl_signal_add(&wlr_popup->base->surface->events.unmap,
                  &new_popup->unmap);
    new_popup->commit.notify = popup_commit;
    wl_signal_add(&wlr_popup->base->surface->events.commit,
                  &new_popup->commit);
    new_popup->destroy.notify = popup_destroy;
    wl_signal_add(&wlr_popup->events.destroy, &new_popup->destroy);
    new_popup->new_popup.notify = popup_new_popup;
    wl_signal_add(&wlr_popup->base->events.new_popup,
                  &new_popup->new_popup);
    new_popup->reposition.notify = popup_reposition;
    wl_signal_add(&wlr_popup->events.reposition, &new_popup->reposition);
}

void toplevel_set_title(struct wl_listener *listener, void *data) {
    struct poison_toplevel *toplevel =
        wl_container_of(listener, toplevel, set_title);
    poison_decoration_update_toplevel(toplevel);
}

void toplevel_new_popup(struct wl_listener *listener, void *data) {
    struct poison_toplevel *toplevel =
        wl_container_of(listener, toplevel, new_popup);
    struct wlr_xdg_popup *wlr_popup = data;

    struct poison_popup *popup = calloc(1, sizeof(*popup));
    if (!popup) {
        wlr_log(WLR_ERROR, "Failed to allocate popup!");
        return;
    }
    popup->server = toplevel->server;
    popup->xdg_popup = wlr_popup;
    popup->root_tree = toplevel->scene_tree;
    popup->scene_tree =
        wlr_scene_xdg_surface_create(toplevel->scene_tree,
                                     wlr_popup->base);
    if (!popup->scene_tree) {
        wlr_log(WLR_ERROR, "Failed to create scene tree for popup!");
        free(popup);
        return;
    }

    popup->map.notify = popup_map;
    wl_signal_add(&wlr_popup->base->surface->events.map, &popup->map);
    popup->unmap.notify = popup_unmap;
    wl_signal_add(&wlr_popup->base->surface->events.unmap, &popup->unmap);
    popup->commit.notify = popup_commit;
    wl_signal_add(&wlr_popup->base->surface->events.commit,
                  &popup->commit);
    popup->destroy.notify = popup_destroy;
    wl_signal_add(&wlr_popup->events.destroy, &popup->destroy);
    popup->new_popup.notify = popup_new_popup;
    wl_signal_add(&wlr_popup->base->events.new_popup, &popup->new_popup);
    popup->reposition.notify = popup_reposition;
    wl_signal_add(&wlr_popup->events.reposition, &popup->reposition);
}

void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, new_xdg_toplevel);
    struct wlr_xdg_toplevel *xdg_toplevel = data;

    struct poison_toplevel *toplevel = calloc(1, sizeof(*toplevel));
    if (!toplevel) {
        wlr_log(WLR_ERROR, "Failed to allocate toplevel!");
        return;
    }
    toplevel->server = server;
    toplevel->xdg_toplevel = xdg_toplevel;
    toplevel->floating = false;
    toplevel->pre_fullscreen_floating = true;
    toplevel->scene_tree = wlr_scene_tree_create(server->tree_toplevels);
    toplevel->surface_tree =
        wlr_scene_xdg_surface_create(toplevel->scene_tree,
                                     xdg_toplevel->base);
    toplevel->scene_tree->node.data = toplevel;
    xdg_toplevel->base->data = toplevel->scene_tree;

    toplevel->map.notify = toplevel_map;
    wl_signal_add(&xdg_toplevel->base->surface->events.map,
                  &toplevel->map);
    toplevel->unmap.notify = toplevel_unmap;
    wl_signal_add(&xdg_toplevel->base->surface->events.unmap,
                  &toplevel->unmap);
    toplevel->commit.notify = toplevel_commit;
    wl_signal_add(&xdg_toplevel->base->surface->events.commit,
                  &toplevel->commit);
    toplevel->destroy.notify = toplevel_destroy;
    wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);
    toplevel->request_fullscreen.notify = toplevel_request_fullscreen;
    wl_signal_add(&xdg_toplevel->events.request_fullscreen,
                  &toplevel->request_fullscreen);
    toplevel->request_maximize.notify = toplevel_request_maximize;
    wl_signal_add(&xdg_toplevel->events.request_maximize,
                  &toplevel->request_maximize);
    toplevel->new_popup.notify = toplevel_new_popup;
    wl_signal_add(&xdg_toplevel->base->events.new_popup,
                  &toplevel->new_popup);
    toplevel->set_title.notify = toplevel_set_title;
    wl_signal_add(&xdg_toplevel->events.set_title, &toplevel->set_title);

    wl_list_init(&toplevel->link);
    toplevel->order.type = POISON_VIEW_XDG;
    wl_list_init(&toplevel->order.link);
    wl_list_init(&toplevel->hsplit_link);
}

void decoration_destroy(struct wl_listener *listener, void *data) {
    struct poison_decoration *decoration =
        wl_container_of(listener, decoration, destroy);
    wl_list_remove(&decoration->destroy.link);
    wl_list_remove(&decoration->request_mode.link);
    free(decoration);
}

void decoration_request_mode(struct wl_listener *listener, void *data) {
    struct poison_decoration *decoration =
        wl_container_of(listener, decoration, request_mode);
    struct wlr_xdg_toplevel_decoration_v1 *wlr_decoration =
        decoration->wlr_decoration;
    struct poison_server *server = decoration->server;

    if (!wlr_decoration->toplevel || !wlr_decoration->toplevel->base) {
        return;
    }

    enum wlr_xdg_toplevel_decoration_v1_mode mode =
        WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
    if (server->config.decorations_enabled &&
        server->config.decoration_respect_client &&
        wlr_decoration->requested_mode ==
            WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE) {
        mode = WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
    }

    struct wlr_scene_tree *tree = wlr_decoration->toplevel->base->data;
    struct poison_toplevel *toplevel = tree ? tree->node.data : NULL;
    bool client_side = mode == WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
    bool changed = toplevel && toplevel->client_side_decorations != client_side;
    if (changed) {
        toplevel->client_side_decorations = client_side;
    }

    if (wlr_decoration->toplevel->base->initialized) {
        wlr_xdg_toplevel_decoration_v1_set_mode(wlr_decoration, mode);
    }

    if (changed) {
        poison_decoration_update_toplevel(toplevel);
        schedule_arrange(server);
    }
}

void server_new_decoration(struct wl_listener *listener, void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, new_decoration);
    struct wlr_xdg_toplevel_decoration_v1 *wlr_decoration = data;

    struct poison_decoration *decoration = calloc(1, sizeof(*decoration));
    if (!decoration) {
        wlr_log(WLR_ERROR, "Failed to allocate decoration!");
        return;
    }
    decoration->server = server;
    decoration->wlr_decoration = wlr_decoration;

    decoration->destroy.notify = decoration_destroy;
    wl_signal_add(&wlr_decoration->events.destroy, &decoration->destroy);
    decoration->request_mode.notify = decoration_request_mode;
    wl_signal_add(&wlr_decoration->events.request_mode,
                  &decoration->request_mode);

    decoration_request_mode(&decoration->request_mode, NULL);
}

void server_request_activate(struct wl_listener *listener, void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, request_activate);
    struct wlr_xdg_activation_v1_request_activate_event *event = data;

    if (!event || !event->surface) {
        return;
    }

    if (!event->token || !event->token->seat) {
        return;
    }

    if (is_layer_exclusive_focused(server)) {
        return;
    }

    struct wlr_surface *surface = event->surface;

    struct poison_toplevel *toplevel;
    wl_list_for_each(toplevel, &server->toplevels, link) {
        if (toplevel->xdg_toplevel &&
            toplevel->xdg_toplevel->base &&
            toplevel->xdg_toplevel->base->surface == surface) {
            if (server->hsplit_active && !toplevel->in_hsplit) {
                return;
            }
            focus_toplevel(toplevel);
            return;
        }
    }

    struct poison_xwayland_view *xwayland_view;
    wl_list_for_each(xwayland_view, &server->xwayland_views, link) {
        if (xwayland_view->xwayland_surface &&
            xwayland_view->xwayland_surface->surface == surface) {
            if (server->hsplit_active && !xwayland_view->in_hsplit) {
                return;
            }
            focus_xwayland_view(server, xwayland_view);
            return;
        }
    }
}

void server_xwayland_ready(struct wl_listener *listener, void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, xwayland_ready);
    wlr_xwayland_set_seat(server->xwayland, server->seat);

    struct wlr_xcursor *xcursor =
        wlr_xcursor_manager_get_xcursor(server->cursor_mgr, "default", 1);
    if (xcursor) {
        struct wlr_xcursor_image *image = xcursor->images[0];
        struct wlr_buffer *buffer = wlr_xcursor_image_get_buffer(image);
        wlr_xwayland_set_cursor(server->xwayland, buffer, xcursor->images[0]->hotspot_x,
                                xcursor->images[0]->hotspot_y);
    }
}

void xwayland_view_associate(struct wl_listener *listener, void *data) {
    struct poison_xwayland_view *xwayland_view =
        wl_container_of(listener, xwayland_view, associate);
    struct wlr_xwayland_surface *xsurface = xwayland_view->xwayland_surface;

    xwayland_view->map.notify = xwayland_view_map;
    wl_signal_add(&xsurface->surface->events.map, &xwayland_view->map);
    xwayland_view->unmap.notify = xwayland_view_unmap;
    wl_signal_add(&xsurface->surface->events.unmap, &xwayland_view->unmap);
}

void xwayland_view_dissociate(struct wl_listener *listener, void *data) {
    struct poison_xwayland_view *xwayland_view =
        wl_container_of(listener, xwayland_view, dissociate);
    wl_list_remove(&xwayland_view->map.link);
    wl_list_remove(&xwayland_view->unmap.link);
    if (xwayland_view->commit_listener_active) {
        wl_list_remove(&xwayland_view->commit.link);
        xwayland_view->commit_listener_active = false;
    }
    poison_decoration_destroy_xwayland(xwayland_view);
    if (xwayland_view->scene_tree) {
        wlr_scene_node_destroy(&xwayland_view->scene_tree->node);
        xwayland_view->scene_tree = NULL;
    }
}

static void apply_xwayland_clip(struct poison_xwayland_view *view) {
    if (!view || !view->scene_tree) {
        return;
    }

    struct wlr_box clip = {0};
    bool focused = (view == view->server->focused_xwayland_view);
    bool clip_focused = view->in_hsplit;
    if (!view->floating && (!focused || clip_focused) &&
        view->clip_box.width > 0 && view->clip_box.height > 0) {
        int lx, ly;
        if (wlr_scene_node_coords(&view->scene_tree->node, &lx, &ly)) {
            clip.x = view->clip_box.x - lx;
            clip.y = view->clip_box.y - ly;
            clip.width = view->clip_box.width;
            clip.height = view->clip_box.height;
        }
    }

    /* An empty clip disables clipping. */
    wlr_scene_subsurface_tree_set_clip(&view->scene_tree->node, &clip);
}

void set_xwayland_view_clip(struct poison_xwayland_view *view,
                            struct wlr_box *layout_box) {
    if (!view) {
        return;
    }
    if (layout_box) {
        view->clip_box = *layout_box;
    } else {
        view->clip_box = (struct wlr_box){0};
    }
    apply_xwayland_clip(view);
}

static struct wlr_box xwayland_view_tile_box(struct poison_xwayland_view *view) {
    struct wlr_box output_box;
    wlr_output_layout_get_box(view->server->output_layout, NULL, &output_box);
    int pad = view->server->config.padding;
    struct poison_frame_insets insets = poison_xwayland_view_insets(view);
    struct wlr_box tile = {
        .x = output_box.x + pad + insets.left,
        .y = output_box.y + pad + insets.top,
        .width = output_box.width - 2 * pad - insets.left - insets.right,
        .height = output_box.height - 2 * pad - insets.top - insets.bottom,
    };
    if (tile.width < 0) {
        tile.width = 0;
    }
    if (tile.height < 0) {
        tile.height = 0;
    }
    return tile;
}

static void arrange_layer_surface(struct poison_layer_surface *layer_surface) {
    struct wlr_layer_surface_v1 *wlr_layer_surface =
        layer_surface->layer_surface;

    if (!wlr_layer_surface->output || !layer_surface->scene_layer_surface) {
        return;
    }

    struct wlr_box full_area = {
        .x = 0,
        .y = 0,
        .width = wlr_layer_surface->output->width,
        .height = wlr_layer_surface->output->height,
    };
    struct wlr_box usable_area = full_area;

    wlr_scene_layer_surface_v1_configure(layer_surface->scene_layer_surface,
                                         &full_area, &usable_area);

    int lx, ly;
    if (layer_surface->popup_tree &&
        wlr_scene_node_coords(
            &layer_surface->scene_layer_surface->tree->node, &lx, &ly)) {
        wlr_scene_node_set_position(&layer_surface->popup_tree->node, lx, ly);
    }
}

void poison_arrange_all(struct poison_server *server) {
    struct wlr_box output_box;
    wlr_output_layout_get_box(server->output_layout, NULL, &output_box);
    if (output_box.width <= 0 || output_box.height <= 0) {
        return;
    }

    struct poison_layer_surface *layer_surface;
    wl_list_for_each(layer_surface, &server->layer_surfaces, link) {
        arrange_layer_surface(layer_surface);
    }

    if (server->hsplit_active) {
        apply_hsplit_layout(server);
    }

    int padding = server->config.padding;
    int effective_padding = padding == 0 ? 1 : padding;

    struct poison_toplevel *toplevel;
    wl_list_for_each(toplevel, &server->toplevels, link) {
        if (!toplevel->xdg_toplevel || !toplevel->scene_tree ||
            !toplevel->xdg_toplevel->base->initialized ||
            toplevel->in_hsplit) {
            continue;
        }

        if (toplevel_fullscreen_now(toplevel)) {
            wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
                                      output_box.width, output_box.height);
            wlr_scene_node_set_position(&toplevel->scene_tree->node,
                                        output_box.x, output_box.y);
        } else if (toplevel->maximized) {
            if (!maximize_toplevel(toplevel)) {
                center_toplevel(toplevel);
            }
        } else if (toplevel->floating) {
            center_toplevel(toplevel);
        } else {
            poison_toplevel_set_tiling(toplevel, true, true);
            if (output_box.width > 2 * effective_padding &&
                output_box.height > 2 * effective_padding) {
                poison_toplevel_apply_size(
                    toplevel,
                    output_box.width - 2 * effective_padding,
                    output_box.height - 2 * effective_padding);
            }
            poison_toplevel_apply_position(toplevel,
                                           output_box.x + effective_padding,
                                           output_box.y + effective_padding);
        }
    }

    struct poison_xwayland_view *view;
    wl_list_for_each(view, &server->xwayland_views, link) {
        struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
        if (!xsurface || !xsurface->surface || !xsurface->surface->mapped ||
            !view->scene_tree || view->in_hsplit) {
            continue;
        }

        if (xsurface->fullscreen) {
            wlr_scene_node_set_position(&view->scene_tree->node,
                                        output_box.x, output_box.y);
            wlr_xwayland_surface_configure(xsurface, output_box.x, output_box.y,
                                           output_box.width, output_box.height);
            set_xwayland_view_clip(view, NULL);
            continue;
        }

        if (view->maximized && maximize_xwayland_view(view)) {
            continue;
        }

        struct poison_frame_insets insets = poison_xwayland_view_insets(view);
        int width = xsurface->width;
        int height = xsurface->height;
        int x = output_box.x + padding + insets.left;
        int y = output_box.y + padding + insets.top;

        if (width > 0 && height > 0) {
            int max_width =
                output_box.width - 2 * padding - insets.left - insets.right;
            int max_height =
                output_box.height - 2 * padding - insets.top - insets.bottom;
            if (max_width > 0 && width > max_width) {
                width = max_width;
            }
            if (max_height > 0 && height > max_height) {
                height = max_height;
            }
            int frame_width = width + insets.left + insets.right;
            int frame_height = height + insets.top + insets.bottom;
            x = output_box.x + (output_box.width - frame_width) / 2;
            y = output_box.y + (output_box.height - frame_height) / 2;
            constrain_position_to_output(&x, &y, frame_width, frame_height,
                                         &output_box, padding);
            x += insets.left;
            y += insets.top;
        }

        wlr_scene_node_set_position(&view->scene_tree->node, x, y);
        wlr_xwayland_surface_configure(xsurface, x, y, width, height);

        if (view->floating) {
            set_xwayland_view_clip(view, NULL);
        } else {
            struct wlr_box tile = xwayland_view_tile_box(view);
            set_xwayland_view_clip(view, &tile);
        }
    }

    poison_decoration_update_all(server);
}

static void arrange_idle_handler(void *data) {
    struct poison_server *server = data;

    server->arrange_idle = NULL;
    poison_arrange_all(server);
    publish_output_configuration(server);
}

static void schedule_arrange(struct poison_server *server) {
    if (server->arrange_idle) {
        return;
    }

    server->arrange_idle =
        wl_event_loop_add_idle(wl_display_get_event_loop(server->wl_display),
                               arrange_idle_handler, server);
    if (!server->arrange_idle) {
        wlr_log(WLR_ERROR, "Failed to defer the arrange: doing it now.");
        poison_arrange_all(server);
        publish_output_configuration(server);
    }
}

static void handle_output_layout_change(struct wl_listener *listener,
                                        void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, output_layout_change);

    schedule_arrange(server);
}

void xwayland_view_commit(struct wl_listener *listener, void *data) {
    struct poison_xwayland_view *xwayland_view =
        wl_container_of(listener, xwayland_view, commit);
    apply_xwayland_clip(xwayland_view);
    poison_decoration_update_xwayland(xwayland_view);
}

void xwayland_view_map(struct wl_listener *listener, void *data) {
    struct poison_xwayland_view *xwayland_view =
        wl_container_of(listener, xwayland_view, map);
    struct poison_server *server = xwayland_view->server;
    struct wlr_xwayland_surface *xsurface = xwayland_view->xwayland_surface;

    poison_decoration_destroy_xwayland(xwayland_view);
    if (xwayland_view->scene_tree) {
        wlr_scene_node_destroy(&xwayland_view->scene_tree->node);
        xwayland_view->scene_tree = NULL;
    }
    xwayland_view->scene_tree =
        wlr_scene_subsurface_tree_create(server->tree_toplevels,
                                         xsurface->surface);
    if (xwayland_view->scene_tree) {
        xwayland_view->scene_tree->node.data = xwayland_view;
    }

    if (!xwayland_view->commit_listener_active) {
        xwayland_view->commit.notify = xwayland_view_commit;
        wl_signal_add(&xsurface->surface->events.commit,
                      &xwayland_view->commit);
        xwayland_view->commit_listener_active = true;
    }
    xwayland_view->clip_box = (struct wlr_box){0};

    wl_list_insert(&server->xwayland_views,
                   &xwayland_view->link);
    xwayland_view->order.serial = ++server->next_view_serial;
    wl_list_insert(server->view_order.prev, &xwayland_view->order.link);

    struct wlr_box output_box;
    wlr_output_layout_get_box(server->output_layout, NULL,
                              &output_box);

    bool inhibit_focus = is_layer_exclusive_focused(server);

    if (server->hsplit_active && xsurface->parent == NULL && !xsurface->modal) {
        if (xsurface->fullscreen) {
            wlr_xwayland_surface_set_fullscreen(xsurface, false);
        }
        if (server->hsplit_focused_slot >= 0) {
            xwayland_view->hsplit_order = server->hsplit_focused_slot;
            hsplit_place_xwayland(xwayland_view,
                                  server->hsplit_xwayland_views.prev);
            server->hsplit_focused_slot = -1;
            apply_hsplit_layout(server);
            if (!inhibit_focus) {
                focus_xwayland_view(server, xwayland_view);
            }
            return;
        }
        struct poison_toplevel *displaced_t = NULL;
        struct poison_xwayland_view *displaced_xw = NULL;
        if (server->focused_toplevel && server->focused_toplevel->in_hsplit) {
            displaced_t = server->focused_toplevel;
        } else if (server->focused_xwayland_view &&
                   server->focused_xwayland_view->in_hsplit) {
            displaced_xw = server->focused_xwayland_view;
        } else if (!wl_list_empty(&server->hsplit_toplevels)) {
            displaced_t = wl_container_of(server->hsplit_toplevels.prev,
                                          displaced_t, hsplit_link);
        } else if (!wl_list_empty(&server->hsplit_xwayland_views)) {
            displaced_xw = wl_container_of(server->hsplit_xwayland_views.prev,
                                           displaced_xw, hsplit_link);
        }
        if (displaced_t) {
            xwayland_view->hsplit_order = displaced_t->hsplit_order;
            hsplit_evict(displaced_t);
            xwayland_view->hsplit_displaced_toplevel = displaced_t;
        } else if (displaced_xw) {
            xwayland_view->hsplit_order = displaced_xw->hsplit_order;
            hsplit_evict_xwayland(displaced_xw);
            xwayland_view->hsplit_displaced_xwayland = displaced_xw;
        }
        hsplit_place_xwayland(xwayland_view, server->hsplit_xwayland_views.prev);
        apply_hsplit_layout(server);
    } else if (xsurface->fullscreen) {
        xwayland_view->floating = false;
        wlr_scene_node_set_position(&xwayland_view->scene_tree->node,
                                    output_box.x, output_box.y);
        if (output_box.width > 0 && output_box.height > 0) {
            wlr_xwayland_surface_configure(xsurface, output_box.x, output_box.y,
                                           output_box.width, output_box.height);
        }
        set_xwayland_view_clip(xwayland_view, NULL);
    } else {
        struct poison_frame_insets insets =
            poison_xwayland_view_insets(xwayland_view);
        int x = output_box.x + server->config.padding + insets.left;
        int y = output_box.y + server->config.padding + insets.top;
        int width = xsurface->width;
        int height = xsurface->height;

        if (width > 0 && height > 0 &&
            output_box.width > 0 && output_box.height > 0) {
            int max_width = output_box.width - 2 * server->config.padding -
                insets.left - insets.right;
            int max_height = output_box.height - 2 * server->config.padding -
                insets.top - insets.bottom;
            if (max_width > 0 && width > max_width) {
                width = max_width;
            }
            if (max_height > 0 && height > max_height) {
                height = max_height;
            }

            int frame_width = width + insets.left + insets.right;
            int frame_height = height + insets.top + insets.bottom;
            x = output_box.x + (output_box.width - frame_width) / 2;
            y = output_box.y + (output_box.height - frame_height) / 2;
            constrain_position_to_output(&x, &y, frame_width, frame_height,
                                         &output_box, server->config.padding);
            x += insets.left;
            y += insets.top;
        }

        wlr_scene_node_set_position(&xwayland_view->scene_tree->node, x, y);
        wlr_xwayland_surface_configure(xsurface, x, y, width, height);

        struct wlr_box tile = xwayland_view_tile_box(xwayland_view);
        set_xwayland_view_clip(xwayland_view, &tile);
        poison_decoration_update_xwayland(xwayland_view);
    }

    if (!inhibit_focus) {
        focus_xwayland_view(server, xwayland_view);
    }
}

void xwayland_view_unmap(struct wl_listener *listener, void *data) {
    struct poison_xwayland_view *xwayland_view =
        wl_container_of(listener, xwayland_view, unmap);
    struct poison_server *server = xwayland_view->server;

    if (xwayland_view->xwayland_surface) {
        deactivate_constraint_for_surface(server,
                                          xwayland_view->xwayland_surface->surface);
    }

    bool was_focused = (server->focused_xwayland_view == xwayland_view);
    bool was_in_hsplit = xwayland_view->in_hsplit;
    struct poison_toplevel *restore_t = NULL;
    struct poison_xwayland_view *restore_xw = NULL;

    if (server->prev_focused_xwayland_view == xwayland_view) {
        server->prev_focused_xwayland_view = NULL;
    }

    if (server->window_selector_prev_xwayland_view == xwayland_view) {
        server->window_selector_prev_xwayland_view = NULL;
    }

    if (server->window_selector_saved_prev_xwayland_view == xwayland_view) {
        server->window_selector_saved_prev_xwayland_view = NULL;
    }

    if (server->launcher_prev_xwayland_view == xwayland_view) {
        server->launcher_prev_xwayland_view = NULL;
    }

    if (server->launcher_saved_prev_xwayland_view == xwayland_view) {
        server->launcher_saved_prev_xwayland_view = NULL;
    }

    if (was_focused) {
        server->focused_xwayland_view = NULL;
    }

    if (xwayland_view->commit_listener_active) {
        wl_list_remove(&xwayland_view->commit.link);
        xwayland_view->commit_listener_active = false;
    }

    poison_decoration_destroy_xwayland(xwayland_view);
    if (xwayland_view->scene_tree) {
        wlr_scene_node_destroy(&xwayland_view->scene_tree->node);
        xwayland_view->scene_tree = NULL;
    }

    if (xwayland_view->in_hsplit) {
        restore_t = hsplit_live_toplevel(
            server, xwayland_view->hsplit_displaced_toplevel);
        restore_xw = hsplit_live_xwayland(
            server, xwayland_view->hsplit_displaced_xwayland);
        struct wl_list *pos = xwayland_view->hsplit_link.prev;
        xwayland_view->in_hsplit = false;
        wl_list_remove(&xwayland_view->hsplit_link);
        wl_list_init(&xwayland_view->hsplit_link);
        if (restore_t && !restore_t->in_hsplit) {
            restore_t->hsplit_order = xwayland_view->hsplit_order;
            hsplit_place(restore_t, &server->hsplit_toplevels);
            apply_hsplit_layout(server);
        } else if (restore_xw && !restore_xw->in_hsplit) {
            restore_xw->hsplit_order = xwayland_view->hsplit_order;
            hsplit_place_xwayland(restore_xw, pos);
            apply_hsplit_layout(server);
        } else {
            hsplit_update_after_removal(server);
        }
    }

    wl_list_remove(&xwayland_view->link);
    wl_list_init(&xwayland_view->link);
    wl_list_remove(&xwayland_view->order.link);
    wl_list_init(&xwayland_view->order.link);

    {
        struct poison_toplevel *t;
        wl_list_for_each(t, &server->toplevels, link) {
            if (t->hsplit_displaced_xwayland == xwayland_view) {
                t->hsplit_displaced_xwayland = NULL;
            }
        }
        struct poison_xwayland_view *xw;
        wl_list_for_each(xw, &server->xwayland_views, link) {
            if (xw->hsplit_displaced_xwayland == xwayland_view) {
                xw->hsplit_displaced_xwayland = NULL;
            }
        }
    }

    if (was_focused) {
        bool split_windows_remain = server->hsplit_active &&
            (!wl_list_empty(&server->hsplit_toplevels) ||
             !wl_list_empty(&server->hsplit_xwayland_views));
        if (was_in_hsplit && split_windows_remain) {
            if (restore_t && restore_t->in_hsplit) {
                focus_toplevel(restore_t);
            } else if (restore_xw && restore_xw->in_hsplit) {
                focus_xwayland_view(server, restore_xw);
            } else {
                hsplit_restore_focus(server);
            }
        } else {
            restore_toplevel_focus(server);
        }
    }

    if (wl_list_empty(&server->toplevels) && wl_list_empty(&server->xwayland_views)) {
        reset_cursor_to_default(server);
    }
}

void xwayland_view_destroy(struct wl_listener *listener, void *data) {
    struct poison_xwayland_view *xwayland_view =
        wl_container_of(listener, xwayland_view, destroy);
    struct poison_server *server = xwayland_view->server;

    bool was_focused = (server->focused_xwayland_view == xwayland_view);
    bool was_in_list = !wl_list_empty(&xwayland_view->link);
    bool was_in_hsplit = xwayland_view->in_hsplit;
    struct poison_toplevel *restore_t = NULL;
    struct poison_xwayland_view *restore_xw = NULL;

    if (was_focused) {
        wlr_seat_keyboard_notify_clear_focus(server->seat);
        server->focused_xwayland_view = NULL;
    }
    if (server->prev_focused_xwayland_view == xwayland_view) {
        server->prev_focused_xwayland_view = NULL;
    }
    if (server->window_selector_prev_xwayland_view == xwayland_view) {
        server->window_selector_prev_xwayland_view = NULL;
    }
    if (server->window_selector_saved_prev_xwayland_view == xwayland_view) {
        server->window_selector_saved_prev_xwayland_view = NULL;
    }
    if (server->launcher_prev_xwayland_view == xwayland_view) {
        server->launcher_prev_xwayland_view = NULL;
    }
    if (server->launcher_saved_prev_xwayland_view == xwayland_view) {
        server->launcher_saved_prev_xwayland_view = NULL;
    }

    wl_list_remove(&xwayland_view->destroy.link);
    wl_list_remove(&xwayland_view->request_configure.link);
    wl_list_remove(&xwayland_view->request_fullscreen.link);
    wl_list_remove(&xwayland_view->request_maximize.link);
    wl_list_remove(&xwayland_view->request_activate.link);
    wl_list_remove(&xwayland_view->set_title.link);
    wl_list_remove(&xwayland_view->set_decorations.link);
    wl_list_remove(&xwayland_view->associate.link);
    wl_list_remove(&xwayland_view->dissociate.link);

    poison_decoration_destroy_xwayland(xwayland_view);
    if (xwayland_view->scene_tree) {
        xwayland_view->scene_tree->node.data = NULL;
        wlr_scene_node_destroy(&xwayland_view->scene_tree->node);
        xwayland_view->scene_tree = NULL;
    }

    if (xwayland_view->in_hsplit) {
        restore_t = hsplit_live_toplevel(
            server, xwayland_view->hsplit_displaced_toplevel);
        restore_xw = hsplit_live_xwayland(
            server, xwayland_view->hsplit_displaced_xwayland);
        struct wl_list *pos = xwayland_view->hsplit_link.prev;
        xwayland_view->in_hsplit = false;
        wl_list_remove(&xwayland_view->hsplit_link);
        wl_list_init(&xwayland_view->hsplit_link);
        if (restore_t && !restore_t->in_hsplit) {
            restore_t->hsplit_order = xwayland_view->hsplit_order;
            hsplit_place(restore_t, &server->hsplit_toplevels);
            apply_hsplit_layout(server);
        } else if (restore_xw && !restore_xw->in_hsplit) {
            restore_xw->hsplit_order = xwayland_view->hsplit_order;
            hsplit_place_xwayland(restore_xw, pos);
            apply_hsplit_layout(server);
        } else {
            hsplit_update_after_removal(server);
        }
    }

    if (!wl_list_empty(&xwayland_view->link)) {
        wl_list_remove(&xwayland_view->link);
    }
    if (!wl_list_empty(&xwayland_view->order.link)) {
        wl_list_remove(&xwayland_view->order.link);
    }

    {
        struct poison_toplevel *t;
        wl_list_for_each(t, &server->toplevels, link) {
            if (t->hsplit_displaced_xwayland == xwayland_view) {
                t->hsplit_displaced_xwayland = NULL;
            }
        }
        struct poison_xwayland_view *xw;
        wl_list_for_each(xw, &server->xwayland_views, link) {
            if (xw->hsplit_displaced_xwayland == xwayland_view) {
                xw->hsplit_displaced_xwayland = NULL;
            }
        }
    }

    if (was_focused && was_in_list) {
        bool split_windows_remain = server->hsplit_active &&
            (!wl_list_empty(&server->hsplit_toplevels) ||
             !wl_list_empty(&server->hsplit_xwayland_views));
        if (was_in_hsplit && split_windows_remain) {
            if (restore_t && restore_t->in_hsplit) {
                focus_toplevel(restore_t);
            } else if (restore_xw && restore_xw->in_hsplit) {
                focus_xwayland_view(server, restore_xw);
            } else {
                hsplit_restore_focus(server);
            }
        } else {
            restore_toplevel_focus(server);
        }
    }

    if (was_in_list &&
        wl_list_empty(&server->toplevels) &&
        wl_list_empty(&server->xwayland_views)) {
        reset_cursor_to_default(server);
    }

    free(xwayland_view);
}

void xwayland_view_request_configure(struct wl_listener *listener, void *data) {
    struct poison_xwayland_view *xwayland_view =
        wl_container_of(listener, xwayland_view, request_configure);
    struct wlr_xwayland_surface_configure_event *event = data;
    struct wlr_xwayland_surface *xsurface = xwayland_view->xwayland_surface;

    if (xwayland_view->in_hsplit) {
        apply_hsplit_layout(xwayland_view->server);
        if (xsurface->fullscreen) {
            wlr_xwayland_surface_set_fullscreen(xsurface, false);
        }
    } else if (xsurface->fullscreen) {
        struct wlr_box output_box;
        wlr_output_layout_get_box(xwayland_view->server->output_layout, NULL,
                                  &output_box);
        if (output_box.width > 0 && output_box.height > 0) {
            wlr_xwayland_surface_configure(xsurface, output_box.x, output_box.y,
                                           output_box.width, output_box.height);
            if (xwayland_view->scene_tree) {
                wlr_scene_node_set_position(&xwayland_view->scene_tree->node,
                                            output_box.x, output_box.y);
            }
        }
        set_xwayland_view_clip(xwayland_view, NULL);
    } else {
        struct wlr_box output_box;
        wlr_output_layout_get_box(xwayland_view->server->output_layout, NULL,
                                  &output_box);

        struct poison_frame_insets insets =
            poison_xwayland_view_insets(xwayland_view);
        int width = event->width;
        int height = event->height;
        int padding = xwayland_view->server->config.padding;

        int x, y;
        if (width > 0 && height > 0 && output_box.width > 0 && output_box.height > 0) {
            int max_width =
                output_box.width - 2 * padding - insets.left - insets.right;
            int max_height =
                output_box.height - 2 * padding - insets.top - insets.bottom;
            if (max_width > 0 && width > max_width) {
                width = max_width;
            }
            if (max_height > 0 && height > max_height) {
                height = max_height;
            }
            int frame_width = width + insets.left + insets.right;
            int frame_height = height + insets.top + insets.bottom;
            x = output_box.x + (output_box.width - frame_width) / 2;
            y = output_box.y + (output_box.height - frame_height) / 2;
            constrain_position_to_output(&x, &y, frame_width, frame_height,
                                         &output_box, padding);
            x += insets.left;
            y += insets.top;

            if (xwayland_view->scene_tree) {
                wlr_scene_node_set_position(&xwayland_view->scene_tree->node, x, y);
            }
        } else {
            x = output_box.x + padding + insets.left;
            y = output_box.y + padding + insets.top;
        }

        wlr_xwayland_surface_configure(xsurface, x, y, width, height);
        struct wlr_box tile = xwayland_view_tile_box(xwayland_view);
        set_xwayland_view_clip(xwayland_view, &tile);
        poison_decoration_update_xwayland(xwayland_view);
    }
}

void xwayland_view_request_maximize(struct wl_listener *listener,
                                    void *data) {
    (void)data;
    struct poison_xwayland_view *view =
        wl_container_of(listener, view, request_maximize);
    struct wlr_xwayland_surface *xsurface = view->xwayland_surface;

    if (!xsurface || !view->floating || view->in_hsplit) {
        return;
    }

    bool wants = xsurface->maximized_horz || xsurface->maximized_vert;
    if (wants != view->maximized) {
        toggle_maximize_xwayland_view(view);
    }
}

void xwayland_view_request_fullscreen(struct wl_listener *listener,
                                      void *data) {
    struct poison_xwayland_view *xwayland_view =
        wl_container_of(listener, xwayland_view, request_fullscreen);
    struct wlr_xwayland_surface *xsurface = xwayland_view->xwayland_surface;

    if (xsurface->surface == NULL || !xsurface->surface->mapped) {
        return;
    }

    if (xwayland_view->in_hsplit) {
        apply_hsplit_layout(xwayland_view->server);
        wlr_xwayland_surface_set_fullscreen(xsurface, false);
        return;
    }

    wlr_xwayland_surface_set_fullscreen(xsurface, xsurface->fullscreen);

    if (xsurface->fullscreen) {
        xwayland_view->pre_fullscreen_floating = xwayland_view->floating;
        xwayland_view->floating = false;
    } else {
        xwayland_view->floating = xwayland_view->pre_fullscreen_floating;
    }

    struct wlr_box output_box;
    wlr_output_layout_get_box(xwayland_view->server->output_layout, NULL,
                              &output_box);

    if (xsurface->fullscreen) {
        if (output_box.width > 0 && output_box.height > 0) {
            wlr_scene_node_set_position(&xwayland_view->scene_tree->node,
                                        output_box.x, output_box.y);
            wlr_xwayland_surface_configure(xsurface, output_box.x, output_box.y,
                                           output_box.width, output_box.height);
        }
        set_xwayland_view_clip(xwayland_view, NULL);
    } else if (xwayland_view->floating) {
        float_xwayland_view(xwayland_view);
    } else {
        struct poison_frame_insets insets =
            poison_xwayland_view_insets(xwayland_view);
        int padding = xwayland_view->server->config.padding;
        int x = output_box.x + padding + insets.left;
        int y = output_box.y + padding + insets.top;

        if (xsurface->width > 0 && xsurface->height > 0 &&
            output_box.width > 0 && output_box.height > 0) {
            int frame_width = xsurface->width + insets.left + insets.right;
            int frame_height = xsurface->height + insets.top + insets.bottom;
            x = output_box.x + (output_box.width - frame_width) / 2;
            y = output_box.y + (output_box.height - frame_height) / 2;
            constrain_position_to_output(&x, &y, frame_width, frame_height,
                                         &output_box, padding);
            x += insets.left;
            y += insets.top;
        }

        wlr_scene_node_set_position(&xwayland_view->scene_tree->node, x, y);
        wlr_xwayland_surface_configure(xsurface, x, y,
                                       xsurface->width, xsurface->height);
        struct wlr_box tile = xwayland_view_tile_box(xwayland_view);
        set_xwayland_view_clip(xwayland_view, &tile);
        poison_decoration_update_xwayland(xwayland_view);
    }
}

void xwayland_view_request_activate(struct wl_listener *listener, void *data) {
    struct poison_xwayland_view *xwayland_view =
        wl_container_of(listener, xwayland_view, request_activate);
    struct wlr_xwayland_surface *xsurface = xwayland_view->xwayland_surface;

    if (xsurface->surface == NULL || !xsurface->surface->mapped) {
        return;
    }

    struct poison_server *server = xwayland_view->server;
    if (server->hsplit_active && !xwayland_view->in_hsplit) {
        return;
    }

    focus_xwayland_view(server, xwayland_view);
}

void xwayland_view_set_title(struct wl_listener *listener, void *data) {
    struct poison_xwayland_view *xwayland_view =
        wl_container_of(listener, xwayland_view, set_title);
    poison_decoration_update_xwayland(xwayland_view);
}

void xwayland_view_set_decorations(struct wl_listener *listener, void *data) {
    struct poison_xwayland_view *xwayland_view =
        wl_container_of(listener, xwayland_view, set_decorations);
    poison_decoration_update_xwayland(xwayland_view);
    schedule_arrange(xwayland_view->server);
}

void xwayland_unmanaged_associate(struct wl_listener *listener, void *data) {
    struct poison_xwayland_unmanaged *unmanaged =
        wl_container_of(listener, unmanaged, associate);
    struct wlr_xwayland_surface *xsurface = unmanaged->xwayland_surface;

    unmanaged->map.notify = xwayland_unmanaged_map;
    wl_signal_add(&xsurface->surface->events.map, &unmanaged->map);
    unmanaged->unmap.notify = xwayland_unmanaged_unmap;
    wl_signal_add(&xsurface->surface->events.unmap, &unmanaged->unmap);
}

void xwayland_unmanaged_dissociate(struct wl_listener *listener, void *data) {
    struct poison_xwayland_unmanaged *unmanaged =
        wl_container_of(listener, unmanaged, dissociate);
    wl_list_remove(&unmanaged->map.link);
    wl_list_remove(&unmanaged->unmap.link);
}

void xwayland_unmanaged_map(struct wl_listener *listener, void *data) {
    struct poison_xwayland_unmanaged *unmanaged =
        wl_container_of(listener, unmanaged, map);
    struct wlr_xwayland_surface *xsurface = unmanaged->xwayland_surface;

    unmanaged->scene_tree =
        wlr_scene_subsurface_tree_create(unmanaged->server->layer_tree_overlay,
                                         xsurface->surface);
    if (unmanaged->scene_tree) {
        unmanaged->scene_tree->node.data = unmanaged;
        wlr_scene_node_set_position(&unmanaged->scene_tree->node,
                                    xsurface->x, xsurface->y);
    }

    wl_list_insert(&unmanaged->server->xwayland_unmanaged, &unmanaged->link);

    if (wlr_xwayland_surface_override_redirect_wants_focus(xsurface)) {
        struct wlr_seat *seat = unmanaged->server->seat;
        struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
        if (kb) {
            wlr_seat_keyboard_notify_enter(seat, xsurface->surface,
                                           kb->keycodes, kb->num_keycodes, &kb->modifiers);
        }
    }
}

void xwayland_unmanaged_unmap(struct wl_listener *listener, void *data) {
    struct poison_xwayland_unmanaged *unmanaged =
        wl_container_of(listener, unmanaged, unmap);
    struct poison_server *server = unmanaged->server;

    if (unmanaged->xwayland_surface && unmanaged->xwayland_surface->surface &&
        server->seat->keyboard_state.focused_surface ==
            unmanaged->xwayland_surface->surface) {
        struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
        struct wlr_surface *restore = NULL;
        if (server->focused_toplevel &&
            server->focused_toplevel->xdg_toplevel) {
            restore = server->focused_toplevel->xdg_toplevel->base->surface;
        } else if (server->focused_xwayland_view &&
                   server->focused_xwayland_view->xwayland_surface) {
            restore =
                server->focused_xwayland_view->xwayland_surface->surface;
        }
        if (restore && kb) {
            wlr_seat_keyboard_notify_enter(server->seat, restore,
                                           kb->keycodes, kb->num_keycodes, &kb->modifiers);
        } else {
            wlr_seat_keyboard_notify_clear_focus(server->seat);
        }
    }

    if (unmanaged->scene_tree) {
        wlr_scene_node_destroy(&unmanaged->scene_tree->node);
        unmanaged->scene_tree = NULL;
    }
    wl_list_remove(&unmanaged->link);
    wl_list_init(&unmanaged->link);
}

void xwayland_unmanaged_destroy(struct wl_listener *listener, void *data) {
    struct poison_xwayland_unmanaged *unmanaged =
        wl_container_of(listener, unmanaged, destroy);

    wl_list_remove(&unmanaged->destroy.link);
    wl_list_remove(&unmanaged->request_configure.link);
    wl_list_remove(&unmanaged->associate.link);
    wl_list_remove(&unmanaged->dissociate.link);

    if (!wl_list_empty(&unmanaged->link)) {
        wl_list_remove(&unmanaged->link);
    }

    free(unmanaged);
}

void xwayland_unmanaged_request_configure(struct wl_listener *listener,
                                          void *data) {
    struct poison_xwayland_unmanaged *unmanaged =
        wl_container_of(listener, unmanaged, request_configure);
    struct wlr_xwayland_surface_configure_event *event = data;
    struct wlr_xwayland_surface *xsurface = unmanaged->xwayland_surface;

    wlr_xwayland_surface_configure(xsurface, event->x, event->y,
                                   event->width, event->height);

    if (unmanaged->scene_tree) {
        wlr_scene_node_set_position(&unmanaged->scene_tree->node,
                                    event->x, event->y);
    }
}

void server_new_xwayland_surface(struct wl_listener *listener, void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, new_xwayland_surface);
    struct wlr_xwayland_surface *xsurface = data;

    if (xsurface->override_redirect) {
        struct poison_xwayland_unmanaged *unmanaged =
            calloc(1, sizeof(*unmanaged));
        if (!unmanaged) {
            wlr_log(WLR_ERROR, "Failed to allocate XWayland unmanaged!");
            return;
        }
        unmanaged->server = server;
        unmanaged->xwayland_surface = xsurface;

        unmanaged->destroy.notify = xwayland_unmanaged_destroy;
        wl_signal_add(&xsurface->events.destroy, &unmanaged->destroy);
        unmanaged->request_configure.notify =
            xwayland_unmanaged_request_configure;
        wl_signal_add(&xsurface->events.request_configure,
                      &unmanaged->request_configure);
        unmanaged->associate.notify = xwayland_unmanaged_associate;
        wl_signal_add(&xsurface->events.associate, &unmanaged->associate);
        unmanaged->dissociate.notify = xwayland_unmanaged_dissociate;
        wl_signal_add(&xsurface->events.dissociate, &unmanaged->dissociate);

        wl_list_init(&unmanaged->link);

        if (xsurface->surface != NULL) {
            xwayland_unmanaged_associate(&unmanaged->associate, NULL);
            if (xsurface->surface->mapped) {
                xwayland_unmanaged_map(&unmanaged->map, NULL);
            }
        }
    } else {
        struct poison_xwayland_view *xwayland_view =
            calloc(1, sizeof(*xwayland_view));
        if (!xwayland_view) {
            wlr_log(WLR_ERROR, "Failed to allocate XWayland view!");
            return;
        }
        xwayland_view->server = server;
        xwayland_view->xwayland_surface = xsurface;
        xwayland_view->floating = true;

        xwayland_view->destroy.notify = xwayland_view_destroy;
        wl_signal_add(&xsurface->events.destroy, &xwayland_view->destroy);
        xwayland_view->request_configure.notify =
            xwayland_view_request_configure;
        wl_signal_add(&xsurface->events.request_configure,
                      &xwayland_view->request_configure);
        xwayland_view->request_fullscreen.notify =
            xwayland_view_request_fullscreen;
        wl_signal_add(&xsurface->events.request_fullscreen,
                      &xwayland_view->request_fullscreen);
        xwayland_view->request_maximize.notify =
            xwayland_view_request_maximize;
        wl_signal_add(&xsurface->events.request_maximize,
                      &xwayland_view->request_maximize);
        xwayland_view->request_activate.notify =
            xwayland_view_request_activate;
        wl_signal_add(&xsurface->events.request_activate,
                      &xwayland_view->request_activate);
        xwayland_view->set_title.notify = xwayland_view_set_title;
        wl_signal_add(&xsurface->events.set_title,
                      &xwayland_view->set_title);
        xwayland_view->set_decorations.notify = xwayland_view_set_decorations;
        wl_signal_add(&xsurface->events.set_decorations,
                      &xwayland_view->set_decorations);
        xwayland_view->associate.notify = xwayland_view_associate;
        wl_signal_add(&xsurface->events.associate,
                      &xwayland_view->associate);
        xwayland_view->dissociate.notify = xwayland_view_dissociate;
        wl_signal_add(&xsurface->events.dissociate,
                      &xwayland_view->dissociate);

        wl_list_init(&xwayland_view->link);
        xwayland_view->order.type = POISON_VIEW_XWAYLAND;
        wl_list_init(&xwayland_view->order.link);
        wl_list_init(&xwayland_view->hsplit_link);

        if (xsurface->surface != NULL) {
            xwayland_view_associate(&xwayland_view->associate, NULL);
            if (xsurface->surface->mapped) {
                xwayland_view_map(&xwayland_view->map, NULL);
            }
        }
    }
}

void layer_surface_destroy(struct wl_listener *listener, void *data) {
    struct poison_layer_surface *layer_surface =
        wl_container_of(listener, layer_surface, destroy);
    wl_list_remove(&layer_surface->link);
    wl_list_remove(&layer_surface->destroy.link);
    wl_list_remove(&layer_surface->map.link);
    wl_list_remove(&layer_surface->unmap.link);
    wl_list_remove(&layer_surface->commit.link);
    wl_list_remove(&layer_surface->new_popup.link);
    if (layer_surface->popup_tree) {
        wlr_scene_node_destroy(&layer_surface->popup_tree->node);
    }
    free(layer_surface);
}

void layer_surface_map(struct wl_listener *listener, void *data) {
    struct poison_layer_surface *layer_surface =
        wl_container_of(listener, layer_surface, map);
    struct wlr_layer_surface_v1 *wlr_layer_surface =
        layer_surface->layer_surface;

    if (wlr_layer_surface->current.keyboard_interactive) {
        struct poison_server *server = layer_surface->server;
        struct wlr_seat *seat = server->seat;

        server->prev_focused_toplevel = server->focused_toplevel;
        server->prev_focused_xwayland_view = server->focused_xwayland_view;

        struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
        if (keyboard != NULL) {
            wlr_seat_keyboard_notify_enter(seat,
                                           wlr_layer_surface->surface,
                                           keyboard->keycodes,
                                           keyboard->num_keycodes,
                                           &keyboard->modifiers);
        }

        update_cursor_focus(server);
    }
}

void layer_surface_unmap(struct wl_listener *listener, void *data) {
    struct poison_layer_surface *layer_surface =
        wl_container_of(listener, layer_surface, unmap);
    struct wlr_layer_surface_v1 *wlr_layer_surface =
        layer_surface->layer_surface;
    struct poison_server *server = layer_surface->server;

    struct wlr_seat *seat = server->seat;
    if (seat->keyboard_state.focused_surface == wlr_layer_surface->surface) {
        struct poison_xwayland_view *next_xwayland = NULL;
        struct poison_toplevel *next_toplevel = NULL;

        if (server->prev_focused_xwayland_view != NULL) {
            struct poison_xwayland_view *xw;
            wl_list_for_each(xw, &server->xwayland_views, link) {
                if (xw == server->prev_focused_xwayland_view) {
                    next_xwayland = server->prev_focused_xwayland_view;
                    break;
                }
            }
        }

        if (next_xwayland == NULL && server->prev_focused_toplevel != NULL) {
            struct poison_toplevel *t;
            wl_list_for_each(t, &server->toplevels, link) {
                if (t == server->prev_focused_toplevel) {
                    next_toplevel = server->prev_focused_toplevel;
                    break;
                }
            }
        }

        if (next_xwayland != NULL) {
            focus_xwayland_view(server, next_xwayland);
        } else if (next_toplevel != NULL) {
            focus_toplevel(next_toplevel);
        } else {
            wlr_seat_keyboard_notify_clear_focus(seat);
        }
    }

    if (seat->pointer_state.focused_surface == wlr_layer_surface->surface) {
        update_cursor_focus(server);
    }
}

void layer_surface_commit(struct wl_listener *listener, void *data) {
    struct poison_layer_surface *layer_surface =
        wl_container_of(listener, layer_surface, commit);
    struct wlr_layer_surface_v1 *wlr_layer_surface =
        layer_surface->layer_surface;

    if (wlr_layer_surface->current.committed != 0) {
        arrange_layer_surface(layer_surface);
    }
}

void layer_surface_new_popup(struct wl_listener *listener, void *data) {
    struct poison_layer_surface *layer_surface =
        wl_container_of(listener, layer_surface, new_popup);
    struct wlr_xdg_popup *wlr_popup = data;

    if (!layer_surface->popup_tree) {
        wlr_log(WLR_ERROR, "Layer surface has no popup tree!");
        return;
    }

    struct poison_popup *popup = calloc(1, sizeof(*popup));
    if (!popup) {
        wlr_log(WLR_ERROR, "Failed to allocate popup!");
        return;
    }

    popup->server = layer_surface->server;
    popup->xdg_popup = wlr_popup;
    popup->root_tree = layer_surface->popup_tree;
    popup->scene_tree =
        wlr_scene_xdg_surface_create(layer_surface->popup_tree,
                                     wlr_popup->base);
    if (!popup->scene_tree) {
        wlr_log(WLR_ERROR, "Failed to create scene tree for popup!");
        free(popup);
        return;
    }

    popup->map.notify = popup_map;
    wl_signal_add(&wlr_popup->base->surface->events.map, &popup->map);
    popup->unmap.notify = popup_unmap;
    wl_signal_add(&wlr_popup->base->surface->events.unmap, &popup->unmap);
    popup->commit.notify = popup_commit;
    wl_signal_add(&wlr_popup->base->surface->events.commit,
                  &popup->commit);
    popup->destroy.notify = popup_destroy;
    wl_signal_add(&wlr_popup->events.destroy, &popup->destroy);
    popup->new_popup.notify = popup_new_popup;
    wl_signal_add(&wlr_popup->base->events.new_popup, &popup->new_popup);
    popup->reposition.notify = popup_reposition;
    wl_signal_add(&wlr_popup->events.reposition, &popup->reposition);
}

void server_new_layer_surface(struct wl_listener *listener, void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, new_layer_surface);
    struct wlr_layer_surface_v1 *wlr_layer_surface = data;

    if (!wlr_layer_surface->output) {
        struct wlr_output *output =
            wlr_output_layout_output_at(server->output_layout,
                                        server->cursor->x,
                                        server->cursor->y);
        if (!output) {
            output =
                wlr_output_layout_get_center_output(server->output_layout);
        }
        if (output) {
            wlr_layer_surface->output = output;
        }
    }

    struct poison_layer_surface *layer_surface =
        calloc(1, sizeof(*layer_surface));
    if (!layer_surface) {
        wlr_log(WLR_ERROR, "Failed to allocate layer surface!");
        return;
    }
    layer_surface->server = server;
    layer_surface->layer_surface = wlr_layer_surface;
    wl_list_insert(&server->layer_surfaces, &layer_surface->link);

    layer_surface->destroy.notify = layer_surface_destroy;
    wl_signal_add(&wlr_layer_surface->events.destroy,
                  &layer_surface->destroy);
    layer_surface->map.notify = layer_surface_map;
    wl_signal_add(&wlr_layer_surface->surface->events.map,
                  &layer_surface->map);
    layer_surface->unmap.notify = layer_surface_unmap;
    wl_signal_add(&wlr_layer_surface->surface->events.unmap,
                  &layer_surface->unmap);
    layer_surface->commit.notify = layer_surface_commit;
    wl_signal_add(&wlr_layer_surface->surface->events.commit,
                  &layer_surface->commit);
    layer_surface->new_popup.notify = layer_surface_new_popup;
    wl_signal_add(&wlr_layer_surface->events.new_popup,
                  &layer_surface->new_popup);

    struct wlr_scene_tree *layer_tree;
    switch (wlr_layer_surface->pending.layer) {
    case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
        layer_tree = server->layer_tree_background;
        break;
    case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
        layer_tree = server->layer_tree_bottom;
        break;
    case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
        layer_tree = server->layer_tree_top;
        break;
    case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
        layer_tree = server->layer_tree_overlay;
        break;
    default:
        layer_tree = server->layer_tree_bottom;
        break;
    }

    layer_surface->scene_layer_surface =
        wlr_scene_layer_surface_v1_create(layer_tree, wlr_layer_surface);
    layer_surface->popup_tree =
        wlr_scene_tree_create(server->scene_layers);
    if (layer_surface->popup_tree) {
        wlr_scene_node_place_below(&layer_surface->popup_tree->node,
                                   &server->layer_tree_overlay->node);
    } else {
        wlr_log(WLR_ERROR, "Failed to create popup tree for layer surface!");
    }
}

static void set_scene_nodes_enabled(struct poison_server *server, bool enabled) {
    wlr_scene_node_set_enabled(&server->layer_tree_background->node, enabled);
    wlr_scene_node_set_enabled(&server->layer_tree_bottom->node, enabled);
    wlr_scene_node_set_enabled(&server->tree_toplevels->node, enabled);
    wlr_scene_node_set_enabled(&server->layer_tree_top->node, enabled);
    wlr_scene_node_set_enabled(&server->layer_tree_overlay->node, enabled);
    if (server->drag_icon_tree) {
        wlr_scene_node_set_enabled(&server->drag_icon_tree->node, enabled);
    }
}

static void schedule_output_frames(struct poison_server *server) {
    struct poison_output *output;
    wl_list_for_each(output, &server->outputs, link) {
        wlr_output_schedule_frame(output->wlr_output);
    }
}

static void schedule_pointer_flush_frame(struct poison_server *server) {
    struct wlr_output *o = wlr_output_layout_output_at(server->output_layout,
                                                       server->cursor->x, server->cursor->y);
    if (o) {
        wlr_output_schedule_frame(o);
    } else if (!wl_list_empty(&server->outputs)) {
        struct poison_output *po =
            wl_container_of(server->outputs.next, po, link);
        wlr_output_schedule_frame(po->wlr_output);
    }
}

static void blank_screens(struct poison_server *server) {
    if (server->screens_blanked) {
        return;
    }

    wlr_log(WLR_INFO, "Idle timeout reached. Blanking screens.");

    set_scene_nodes_enabled(server, false);
    wlr_seat_pointer_notify_clear_focus(server->seat);
    set_cursor_image(server, poison_cursor_image_none());
    server->screens_blanked = true;
    schedule_output_frames(server);
}

static void unblank_screens(struct poison_server *server) {
    if (!server->screens_blanked) {
        return;
    }

    wlr_log(WLR_INFO, "Unblanking screens.");

    server->screens_blanked = false;
    set_scene_nodes_enabled(server, true);
    set_cursor_image(server, poison_cursor_image_xcursor("default"));
    update_cursor_focus(server);
    schedule_output_frames(server);
}

static int idle_timer_handler(void *data) {
    struct poison_server *server = data;

    if (!wl_list_empty(&server->idle_inhibitors)) {
        wl_event_source_timer_update(server->idle_timer, server->idle_timeout_ms);
        return 0;
    }

    if (server->window_selector_fd != -1) {
        wlr_log(WLR_INFO, "Closing window selector due to idle timeout.");
        if (server->window_selector_event_source) {
            wl_event_source_remove(server->window_selector_event_source);
            server->window_selector_event_source = NULL;
        }
        close(server->window_selector_fd);
        server->window_selector_fd = -1;
        server->window_selector_reply_len = 0;
        kill_tracked_child(&server->window_selector_pid);
        if (server->window_selector_prev_xwayland_view) {
            focus_xwayland_view(server, server->window_selector_prev_xwayland_view);
        } else if (server->window_selector_prev_toplevel) {
            focus_toplevel(server->window_selector_prev_toplevel);
        }
        server->prev_focused_toplevel = server->window_selector_saved_prev_toplevel;
        server->prev_focused_xwayland_view = server->window_selector_saved_prev_xwayland_view;
        server->window_selector_prev_toplevel = NULL;
        server->window_selector_prev_xwayland_view = NULL;
        server->window_selector_saved_prev_toplevel = NULL;
        server->window_selector_saved_prev_xwayland_view = NULL;
    }

    if (server->launcher_fd != -1) {
        wlr_log(WLR_INFO, "Closing launcher due to idle timeout.");
        if (server->launcher_event_source) {
            wl_event_source_remove(server->launcher_event_source);
            server->launcher_event_source = NULL;
        }
        close(server->launcher_fd);
        server->launcher_fd = -1;
        kill_tracked_child(&server->launcher_pid);
        if (server->launcher_prev_xwayland_view) {
            focus_xwayland_view(server, server->launcher_prev_xwayland_view);
        } else if (server->launcher_prev_toplevel) {
            focus_toplevel(server->launcher_prev_toplevel);
        }
        server->prev_focused_toplevel = server->launcher_saved_prev_toplevel;
        server->prev_focused_xwayland_view = server->launcher_saved_prev_xwayland_view;
        server->launcher_prev_toplevel = NULL;
        server->launcher_prev_xwayland_view = NULL;
        server->launcher_saved_prev_toplevel = NULL;
        server->launcher_saved_prev_xwayland_view = NULL;
    }

    blank_screens(server);
    return 0;
}

void reset_idle_timer(struct poison_server *server) {
    if (server->screens_blanked) {
        unblank_screens(server);
    }

    if (server->idle_timer && server->idle_timeout_ms > 0) {
        wl_event_source_timer_update(server->idle_timer, server->idle_timeout_ms);
    }
}

void idle_inhibitor_destroy(struct wl_listener *listener, void *data) {
    struct poison_idle_inhibitor *inhibitor =
        wl_container_of(listener, inhibitor, destroy);

    wl_list_remove(&inhibitor->link);
    wl_list_remove(&inhibitor->destroy.link);
    free(inhibitor);
}

void server_new_idle_inhibitor(struct wl_listener *listener, void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, new_idle_inhibitor);
    struct wlr_idle_inhibitor_v1 *wlr_inhibitor = data;

    struct poison_idle_inhibitor *inhibitor = calloc(1, sizeof(*inhibitor));
    if (!inhibitor) {
        wlr_log(WLR_ERROR, "Failed to allocate memory for idle inhibitor!");
        return;
    }

    inhibitor->server = server;
    inhibitor->wlr_inhibitor = wlr_inhibitor;
    inhibitor->destroy.notify = idle_inhibitor_destroy;
    wl_signal_add(&wlr_inhibitor->events.destroy, &inhibitor->destroy);
    wl_list_insert(&server->idle_inhibitors, &inhibitor->link);
}

static bool constraint_surface_origin(struct poison_server *server,
                                      struct wlr_pointer_constraint_v1 *constraint,
                                      double *lx, double *ly) {
    struct wlr_surface *surface = constraint->surface;
    struct wlr_scene_tree *tree = NULL;
    struct wlr_xdg_surface *xdg_surface = NULL;

    struct poison_xwayland_view *xwayland_view;
    wl_list_for_each(xwayland_view, &server->xwayland_views, link) {
        if (xwayland_view->xwayland_surface &&
            xwayland_view->xwayland_surface->surface == surface) {
            tree = xwayland_view->scene_tree;
            break;
        }
    }
    if (!tree) {
        struct poison_xwayland_unmanaged *unmanaged;
        wl_list_for_each(unmanaged, &server->xwayland_unmanaged, link) {
            if (unmanaged->xwayland_surface &&
                unmanaged->xwayland_surface->surface == surface) {
                tree = unmanaged->scene_tree;
                break;
            }
        }
    }
    if (!tree) {
        struct poison_toplevel *toplevel;
        wl_list_for_each(toplevel, &server->toplevels, link) {
            if (toplevel->xdg_toplevel &&
                toplevel->xdg_toplevel->base->surface == surface) {
                tree = toplevel->scene_tree;
                xdg_surface = toplevel->xdg_toplevel->base;
                break;
            }
        }
    }
    if (!tree) {
        struct poison_layer_surface *layer_surface;
        wl_list_for_each(layer_surface, &server->layer_surfaces, link) {
            if (layer_surface->layer_surface &&
                layer_surface->layer_surface->surface == surface &&
                layer_surface->scene_layer_surface) {
                tree = layer_surface->scene_layer_surface->tree;
                break;
            }
        }
    }
    if (!tree) {
        return false;
    }

    int ix, iy;
    if (!wlr_scene_node_coords(&tree->node, &ix, &iy)) {
        return false;
    }
    *lx = ix;
    *ly = iy;

    if (xdg_surface) {
        *lx -= xdg_surface->geometry.x;
        *ly -= xdg_surface->geometry.y;
    }
    return true;
}

static void constraint_apply_cursor_hint(struct poison_server *server,
                                         struct wlr_pointer_constraint_v1 *constraint) {
    if (!constraint ||
        constraint->type != WLR_POINTER_CONSTRAINT_V1_LOCKED ||
        !constraint->current.cursor_hint.enabled) {
        return;
    }

    double lx, ly;
    if (!constraint_surface_origin(server, constraint, &lx, &ly)) {
        return;
    }

    double sx = constraint->current.cursor_hint.x;
    double sy = constraint->current.cursor_hint.y;
    wlr_cursor_warp(server->cursor, NULL, lx + sx, ly + sy);

    if (server->seat->pointer_state.focused_surface == constraint->surface) {
        queue_pointer_motion(server, poison_now_msec(), sx, sy);
    }

    wlr_log(WLR_DEBUG, "Pointer constraint: warped to cursor hint %.1f,%.1f "
                       "(layout %.1f,%.1f).",
            sx, sy, lx + sx, ly + sy);
}

static bool constraint_region_warp_target(struct wlr_pointer_constraint_v1 *constraint,
                                          double *sx, double *sy) {
    const pixman_region32_t *region = &constraint->region;
    if (!pixman_region32_not_empty(region)) {
        return false;
    }

    const pixman_box32_t *extents = pixman_region32_extents(region);
    double cx = *sx;
    double cy = *sy;
    if (cx < extents->x1) {
        cx = extents->x1;
    } else if (cx > extents->x2 - 1) {
        cx = extents->x2 - 1;
    }
    if (cy < extents->y1) {
        cy = extents->y1;
    } else if (cy > extents->y2 - 1) {
        cy = extents->y2 - 1;
    }

    if (pixman_region32_contains_point(region, (int)floor(cx), (int)floor(cy),
                                       NULL)) {
        *sx = cx;
        *sy = cy;
        return true;
    }

    int n;
    const pixman_box32_t *boxes = pixman_region32_rectangles(region, &n);
    if (n <= 0) {
        return false;
    }
    *sx = (boxes[0].x1 + boxes[0].x2) / 2.0;
    *sy = (boxes[0].y1 + boxes[0].y2) / 2.0;
    return true;
}

static bool constraint_confine(struct poison_server *server,
                               struct wlr_pointer_constraint_v1 *constraint,
                               double old_lx, double old_ly,
                               double new_lx, double new_ly,
                               double *out_lx, double *out_ly) {
    double lx, ly;
    if (!constraint_surface_origin(server, constraint, &lx, &ly)) {
        return false;
    }

    double old_sx = old_lx - lx;
    double old_sy = old_ly - ly;

    if (server->active_confine_requires_warp) {
        server->active_confine_requires_warp = false;
        if (!pixman_region32_contains_point(&constraint->region,
                                            (int)floor(old_sx),
                                            (int)floor(old_sy), NULL)) {
            double wx = old_sx, wy = old_sy;
            if (constraint_region_warp_target(constraint, &wx, &wy)) {
                old_sx = wx;
                old_sy = wy;
            }
        }
    }

    double new_sx = new_lx - lx;
    double new_sy = new_ly - ly;

    double cx, cy;
    if (pixman_region32_not_empty(&constraint->region)) {
        if (!wlr_region_confine(&constraint->region, old_sx, old_sy,
                                new_sx, new_sy, &cx, &cy)) {
            cx = old_sx;
            cy = old_sy;
        }
    } else {
        cx = new_sx;
        cy = new_sy;
    }

    *out_lx = cx + lx;
    *out_ly = cy + ly;
    return true;
}

static void update_pointer_constraint(struct poison_server *server,
                                      struct wlr_surface *surface) {
    if (server->active_constraint &&
        server->active_constraint->surface ==
            server->seat->keyboard_state.focused_surface) {
        surface = server->active_constraint->surface;
    }

    struct wlr_pointer_constraint_v1 *constraint = NULL;
    if (surface) {
        constraint = wlr_pointer_constraints_v1_constraint_for_surface(
            server->pointer_constraints, surface, server->seat);
    }
    if (constraint == server->active_constraint) {
        return;
    }
    struct wlr_pointer_constraint_v1 *old = server->active_constraint;
    if (old) {
        constraint_apply_cursor_hint(server, old);
        server->active_constraint = NULL;
        server->active_confine_requires_warp = false;
        wlr_pointer_constraint_v1_send_deactivated(old);
    }
    server->active_constraint = constraint;
    if (constraint) {
        server->active_confine_requires_warp =
            constraint->type == WLR_POINTER_CONSTRAINT_V1_CONFINED;
        if (constraint->type == WLR_POINTER_CONSTRAINT_V1_CONFINED &&
            pixman_region32_not_empty(&constraint->region)) {
            double lx, ly;
            if (constraint_surface_origin(server, constraint, &lx, &ly)) {
                double sx = server->cursor->x - lx;
                double sy = server->cursor->y - ly;
                if (!pixman_region32_contains_point(&constraint->region,
                                                    (int)floor(sx), (int)floor(sy), NULL) &&
                    constraint_region_warp_target(constraint, &sx, &sy)) {
                    wlr_cursor_warp(server->cursor, NULL, lx + sx, ly + sy);
                    server->active_confine_requires_warp = false;
                }
            }
        }
        wlr_pointer_constraint_v1_send_activated(constraint);
        wlr_log(WLR_DEBUG, "Pointer constraint activated (%s, region %s).",
                constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED
                    ? "locked"
                    : "confined",
                pixman_region32_not_empty(&constraint->region) ? "set" : "whole surface");
    }
}

static void deactivate_constraint_for_surface(struct poison_server *server,
                                              struct wlr_surface *surface) {
    if (!surface || !server->active_constraint ||
        server->active_constraint->surface != surface) {
        return;
    }
    struct wlr_pointer_constraint_v1 *old = server->active_constraint;
    server->active_constraint = NULL;
    server->active_confine_requires_warp = false;
    wlr_pointer_constraint_v1_send_deactivated(old);
}

static void pointer_constraint_set_region(struct wl_listener *listener,
                                          void *data) {
    struct poison_pointer_constraint *pc =
        wl_container_of(listener, pc, set_region);
    if (pc->server->active_constraint != pc->constraint) {
        return;
    }
    pc->server->active_confine_requires_warp = true;
}

void server_new_pointer_constraint(struct wl_listener *listener, void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, new_pointer_constraint);
    struct wlr_pointer_constraint_v1 *constraint = data;

    struct poison_pointer_constraint *pc = calloc(1, sizeof(*pc));
    if (!pc) {
        wlr_log(WLR_ERROR, "Failed to allocate memory for pointer constraint!");
        return;
    }
    pc->server = server;
    pc->constraint = constraint;
    pc->set_region.notify = pointer_constraint_set_region;
    wl_signal_add(&constraint->events.set_region, &pc->set_region);
    pc->destroy.notify = pointer_constraint_destroy;
    wl_signal_add(&constraint->events.destroy, &pc->destroy);
    wl_list_insert(&server->pointer_constraints_list, &pc->link);

    if (server->seat->pointer_state.focused_surface == constraint->surface ||
        server->seat->keyboard_state.focused_surface == constraint->surface) {
        update_pointer_constraint(server, constraint->surface);
    }
}

void pointer_constraint_destroy(struct wl_listener *listener, void *data) {
    struct poison_pointer_constraint *pc =
        wl_container_of(listener, pc, destroy);
    struct poison_server *server = pc->server;

    if (server->active_constraint == pc->constraint) {
        constraint_apply_cursor_hint(server, pc->constraint);
        server->active_constraint = NULL;
        server->active_confine_requires_warp = false;
    }
    wl_list_remove(&pc->set_region.link);
    wl_list_remove(&pc->destroy.link);
    wl_list_remove(&pc->link);
    free(pc);
}

static void spawn_detached_command(const char *cmd) {
    if (!cmd || *cmd == '\0') {
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        setsid();

        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) {
                close(devnull);
            }
        }

        int max_fd = sysconf(_SC_OPEN_MAX);
        if (max_fd < 0) {
            max_fd = 1024;
        }
        for (int fd = 3; fd < max_fd; fd++) {
            close(fd);
        }

        execl("/bin/sh", "/bin/sh", "-c", cmd, (void *)NULL);
        _exit(1);
    } else if (pid < 0) {
        wlr_log(WLR_ERROR, "Failed to fork to run command '%s'!", cmd);
    }
}

static void update_keyboard_shortcuts_inhibitors(struct poison_server *server) {
    if (!server->keyboard_shortcuts_inhibit_manager) {
        return;
    }
    struct wlr_surface *focused = server->seat->keyboard_state.focused_surface;
    struct wlr_keyboard_shortcuts_inhibitor_v1 *inhibitor;
    wl_list_for_each(inhibitor,
                     &server->keyboard_shortcuts_inhibit_manager->inhibitors,
                     link) {
        bool should_inhibit = inhibitor->seat == server->seat &&
            inhibitor->surface == focused;
        if (should_inhibit && !inhibitor->active) {
            wlr_keyboard_shortcuts_inhibitor_v1_activate(inhibitor);
        } else if (!should_inhibit && inhibitor->active) {
            wlr_keyboard_shortcuts_inhibitor_v1_deactivate(inhibitor);
        }
    }
}

static bool keyboard_shortcuts_inhibited(struct poison_server *server) {
    struct wlr_surface *focused = server->seat->keyboard_state.focused_surface;
    if (!focused || !server->keyboard_shortcuts_inhibit_manager) {
        return false;
    }
    struct wlr_keyboard_shortcuts_inhibitor_v1 *inhibitor;
    wl_list_for_each(inhibitor,
                     &server->keyboard_shortcuts_inhibit_manager->inhibitors,
                     link) {
        if (inhibitor->seat == server->seat &&
            inhibitor->surface == focused && inhibitor->active) {
            return true;
        }
    }
    return false;
}

void server_new_keyboard_shortcuts_inhibitor(struct wl_listener *listener,
                                             void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, new_keyboard_shortcuts_inhibitor);
    (void)data;
    update_keyboard_shortcuts_inhibitors(server);
}

static bool handle_keybinding(struct poison_server *server,
                              xkb_keysym_t sym, uint32_t modifiers) {
    bool super = modifiers & WLR_MODIFIER_LOGO;
    bool ctrl = modifiers & WLR_MODIFIER_CTRL;
    bool alt = modifiers & WLR_MODIFIER_ALT;
    bool shift = modifiers & WLR_MODIFIER_SHIFT;

    if (ctrl && alt && sym == XKB_KEY_BackSpace) {
        wl_display_terminate(server->wl_display);
        return true;
    }

    if (super && sym == XKB_KEY_Escape) {
        if (server->focused_toplevel) {
            close_toplevel(server->focused_toplevel);
        } else if (server->focused_xwayland_view && server->focused_xwayland_view->xwayland_surface) {
            wlr_xwayland_surface_close(server->focused_xwayland_view->xwayland_surface);
        }
        return true;
    }

    if (super && sym == XKB_KEY_Tab) {
        start_window_selector(server);
        return true;
    }

    if (super && sym == XKB_KEY_p) {
        start_application_launcher(server);
        return true;
    }

    if (super && shift && (sym == XKB_KEY_i || sym == XKB_KEY_I)) {
        poison_console_toggle(server);
        return true;
    }

    if (super && sym == XKB_KEY_f) {
        if (server->focused_toplevel) {
            struct poison_toplevel *toplevel = server->focused_toplevel;
            focus_toplevel(toplevel);
            toggle_float_toplevel(toplevel);
        } else if (server->focused_xwayland_view) {
            struct poison_xwayland_view *view = server->focused_xwayland_view;
            focus_xwayland_view(server, view);
            toggle_float_xwayland_view(view);
        }
        return true;
    }

    if (super && sym == XKB_KEY_Return) {
        wlr_seat_keyboard_notify_clear_focus(server->seat);

        server->focused_toplevel = NULL;
        server->focused_xwayland_view = NULL;

        pid_t pid = fork();
        if (pid == 0) {
            setsid();

            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                if (devnull > 2) {
                    close(devnull);
                }
            }

            int max_fd = sysconf(_SC_OPEN_MAX);
            if (max_fd < 0) {
                max_fd = 1024;
            }
            for (int fd = 3; fd < max_fd; fd++) {
                close(fd);
            }

            const char *cmd_to_exec = NULL;
            if (server->config.terminal_command && *server->config.terminal_command != '\0') {
                cmd_to_exec = server->config.terminal_command;
            } else {
                cmd_to_exec = "poison-terminal --maximize";
            }

            execl("/bin/sh", "/bin/sh", "-c", cmd_to_exec, (void *)NULL);
            _exit(1);
        } else if (pid < 0) {
            wlr_log(WLR_ERROR, "Failed to fork for terminal!");
        }
        return true;
    }

    if (super && sym == XKB_KEY_h) {
        toggle_hsplit(server);
        show_split_indicator(server);
        return true;
    }

    if (super && sym == XKB_KEY_o) {
        hsplit_only(server);
        return true;
    }

    if (super && shift && (sym == XKB_KEY_Left || sym == XKB_KEY_Right)) {
        hsplit_exchange(server, sym == XKB_KEY_Right);
        return true;
    }

    if (super && sym == XKB_KEY_Left) {
        hsplit_navigate(server, false);
        show_split_indicator(server);
        return true;
    }

    if (super && sym == XKB_KEY_Right) {
        hsplit_navigate(server, true);
        show_split_indicator(server);
        return true;
    }

    if (sym == XKB_KEY_XF86AudioRaiseVolume) {
        spawn_detached_command(server->config.volume_up_command);
        return true;
    }

    if (sym == XKB_KEY_XF86AudioLowerVolume) {
        spawn_detached_command(server->config.volume_down_command);
        return true;
    }

    if (sym == XKB_KEY_XF86AudioMute) {
        spawn_detached_command(server->config.mute_command);
        return true;
    }

    if (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
        if (server->session) {
            unsigned int vt = sym - XKB_KEY_XF86Switch_VT_1 + 1;
            if (!wlr_session_change_vt(server->session, vt)) {
                wlr_log(WLR_ERROR, "Failed to switch to VT %u!", vt);
            }
        }
        return true;
    }

    return false;
}

void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
    struct poison_keyboard *keyboard =
        wl_container_of(listener, keyboard, modifiers);
    wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
    wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
                                       &keyboard->wlr_keyboard->modifiers);
}

void keyboard_handle_key(struct wl_listener *listener, void *data) {
    struct poison_keyboard *keyboard =
        wl_container_of(listener, keyboard, key);
    struct poison_server *server = keyboard->server;
    struct wlr_keyboard_key_event *event = data;
    struct wlr_seat *seat = server->seat;

    reset_idle_timer(server);

    uint32_t keycode = event->keycode + 8;
    const xkb_keysym_t *syms;
    int nsyms =
        xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state, keycode,
                               &syms);

    bool handled = false;
    uint32_t modifiers =
        wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
    update_keyboard_shortcuts_inhibitors(server);
    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED &&
        !keyboard_shortcuts_inhibited(server)) {
        for (int i = 0; i < nsyms; i++) {
            handled = handle_keybinding(server, syms[i], modifiers);
            if (handled) {
                break;
            }
        }
    }

    if (!handled) {
        wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
        wlr_seat_keyboard_notify_key(seat, event->time_msec,
                                     event->keycode, event->state);
    }
}

void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
    struct poison_keyboard *keyboard =
        wl_container_of(listener, keyboard, destroy);
    wl_list_remove(&keyboard->modifiers.link);
    wl_list_remove(&keyboard->key.link);
    wl_list_remove(&keyboard->destroy.link);
    wl_list_remove(&keyboard->link);
    free(keyboard);
}

static void server_new_keyboard(struct poison_server *server,
                                struct wlr_input_device *device) {
    struct wlr_keyboard *wlr_keyboard =
        wlr_keyboard_from_input_device(device);

    struct poison_keyboard *keyboard = calloc(1, sizeof(*keyboard));
    if (!keyboard) {
        wlr_log(WLR_ERROR, "Failed to allocate keyboard!");
        return;
    }
    keyboard->server = server;
    keyboard->wlr_keyboard = wlr_keyboard;

    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!context) {
        wlr_log(WLR_ERROR, "Failed to create XKB context!");
        free(keyboard);
        return;
    }

    struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
                                                          XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap) {
        wlr_log(WLR_ERROR, "Failed to create XKB keymap!");
        xkb_context_unref(context);
        free(keyboard);
        return;
    }

    wlr_keyboard_set_keymap(wlr_keyboard, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

    keyboard->modifiers.notify = keyboard_handle_modifiers;
    wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
    keyboard->key.notify = keyboard_handle_key;
    wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
    keyboard->destroy.notify = keyboard_handle_destroy;
    wl_signal_add(&device->events.destroy, &keyboard->destroy);

    wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);

    wl_list_insert(&server->keyboards, &keyboard->link);
}

static void server_new_pointer(struct poison_server *server,
                               struct wlr_input_device *device) {
    wlr_cursor_attach_input_device(server->cursor, device);
}

void server_new_input(struct wl_listener *listener, void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, new_input);
    struct wlr_input_device *device = data;

    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        server_new_keyboard(server, device);
        break;
    case WLR_INPUT_DEVICE_POINTER:
        server_new_pointer(server, device);
        break;
    case WLR_INPUT_DEVICE_TOUCH:
        wlr_cursor_attach_input_device(server->cursor, device);
        break;
    default:
        break;
    }

    uint32_t caps = WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_TOUCH;
    if (!wl_list_empty(&server->keyboards)) {
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    wlr_seat_set_capabilities(server->seat, caps);
}

static void update_drag_icons(struct poison_server *server) {
    if (server->drag_icon && server->seat->drag &&
        server->seat->drag->icon &&
        server->seat->drag->icon->surface->mapped) {
        wlr_scene_node_set_position(&server->drag_icon->node,
                                    (int)server->cursor->x,
                                    (int)server->cursor->y);
    }
}

static void drag_icon_destroy_notify(struct wl_listener *listener, void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, drag_icon_destroy);
    wl_list_remove(&server->drag_icon_destroy.link);
    wl_list_init(&server->drag_icon_destroy.link);
    server->drag_icon = NULL;
}

static void drag_destroy_notify(struct wl_listener *listener, void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, drag_destroy);
    (void)data;
    wl_list_remove(&server->drag_destroy.link);
    wl_list_init(&server->drag_destroy.link);

    update_cursor_focus(server);
}

static void track_drag_icon(struct poison_server *server,
                            struct wlr_drag_icon *icon) {
    if (!icon || !server->drag_icon_tree) {
        return;
    }
    server->drag_icon =
        wlr_scene_drag_icon_create(server->drag_icon_tree, icon);
    if (server->drag_icon) {
        server->drag_icon_destroy.notify = drag_icon_destroy_notify;
        wl_signal_add(&server->drag_icon->node.events.destroy,
                      &server->drag_icon_destroy);
        /* Otherwise it sits at 0,0 until the next motion event. */
        wlr_scene_node_set_position(&server->drag_icon->node,
                                    (int)server->cursor->x,
                                    (int)server->cursor->y);
    }
}

static void track_drag(struct poison_server *server, struct wlr_drag *drag) {
    server->drag_destroy.notify = drag_destroy_notify;
    wl_signal_add(&drag->events.destroy, &server->drag_destroy);
    track_drag_icon(server, drag->icon);
}

#define IMPLICIT_GRAB_EDGE_DRIFT 32.0

static double clamp_edge_drift(double v) {
    if (v < -IMPLICIT_GRAB_EDGE_DRIFT) {
        return -IMPLICIT_GRAB_EDGE_DRIFT;
    }
    if (v > IMPLICIT_GRAB_EDGE_DRIFT) {
        return IMPLICIT_GRAB_EDGE_DRIFT;
    }
    return v;
}

void poison_flush_pointer_motion(struct poison_server *server) {
    bool sent = false;

    if (server->pointer_rel_pending) {
        server->pointer_rel_pending = false;
        if (server->pointer_rel_dx != 0.0 || server->pointer_rel_dy != 0.0 ||
            server->pointer_rel_unaccel_dx != 0.0 ||
            server->pointer_rel_unaccel_dy != 0.0) {
            wlr_relative_pointer_manager_v1_send_relative_motion(
                server->relative_pointer_manager, server->seat,
                (uint64_t)server->pointer_rel_time * 1000,
                server->pointer_rel_dx, server->pointer_rel_dy,
                server->pointer_rel_unaccel_dx, server->pointer_rel_unaccel_dy);
            sent = true;
        }
        server->pointer_rel_dx = 0;
        server->pointer_rel_dy = 0;
        server->pointer_rel_unaccel_dx = 0;
        server->pointer_rel_unaccel_dy = 0;
    }

    if (server->pointer_motion_pending) {
        server->pointer_motion_pending = false;
        wlr_seat_pointer_notify_motion(server->seat, server->pointer_motion_time,
                                       server->pointer_motion_sx,
                                       server->pointer_motion_sy);
        sent = true;
    }

    if (sent) {
        wlr_seat_pointer_notify_frame(server->seat);
    }
}

static void queue_pointer_motion(struct poison_server *server,
                                 uint32_t time_msec, double sx, double sy) {
    server->pointer_motion_pending = true;
    server->pointer_motion_time = time_msec;
    server->pointer_motion_sx = sx;
    server->pointer_motion_sy = sy;
    /* Ensure a frame is scheduled so the pending motion is actually flushed,
     * even when the scene has no other damage. */
    schedule_pointer_flush_frame(server);
}

static void forget_pointer_abs_device(struct poison_server *server) {
    if (!wl_list_empty(&server->pointer_abs_device_destroy.link)) {
        wl_list_remove(&server->pointer_abs_device_destroy.link);
        wl_list_init(&server->pointer_abs_device_destroy.link);
    }
    server->pointer_abs_device = NULL;
    server->pointer_abs_valid = false;
}

static void pointer_abs_device_destroy(struct wl_listener *listener,
                                       void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, pointer_abs_device_destroy);
    forget_pointer_abs_device(server);
}

static void track_pointer_abs_device(struct poison_server *server,
                                     struct wlr_input_device *device) {
    if (server->pointer_abs_device == device) {
        return;
    }
    forget_pointer_abs_device(server);
    server->pointer_abs_device = device;
    wl_signal_add(&device->events.destroy,
                  &server->pointer_abs_device_destroy);
}

static void queue_relative_motion(struct poison_server *server,
                                  uint32_t time_msec, double dx, double dy,
                                  double unaccel_dx, double unaccel_dy) {
    if (server->seat->pointer_state.focused_surface == NULL) {
        return;
    }
    if (dx == 0.0 && dy == 0.0 && unaccel_dx == 0.0 && unaccel_dy == 0.0) {
        return;
    }

    server->pointer_rel_pending = true;
    server->pointer_rel_time = time_msec;
    server->pointer_rel_dx += dx;
    server->pointer_rel_dy += dy;
    server->pointer_rel_unaccel_dx += unaccel_dx;
    server->pointer_rel_unaccel_dy += unaccel_dy;
    schedule_pointer_flush_frame(server);
}

static void drop_pending_relative_motion(struct poison_server *server) {
    server->pointer_rel_pending = false;
    server->pointer_rel_dx = 0;
    server->pointer_rel_dy = 0;
    server->pointer_rel_unaccel_dx = 0;
    server->pointer_rel_unaccel_dy = 0;
}

#define HOT_CORNER_RADIUS 24.0
#define HOT_CORNER_WINDOW_MS 300
#define HOT_CORNER_MIN_MS 100

static void handle_hot_corner(struct poison_server *server, uint32_t time_msec,
                              double edge_dx, double edge_dy) {
    if (!server->config.hot_corner_enabled ||
        server->seat->pointer_state.button_count != 0 ||
        server->seat->drag != NULL) {
        return;
    }

    struct wlr_box layout_box;
    wlr_output_layout_get_box(server->output_layout, NULL, &layout_box);
    if (layout_box.width <= 0 || layout_box.height <= 0) {
        return;
    }

    if (server->cursor->x > layout_box.x + HOT_CORNER_RADIUS ||
        server->cursor->y > layout_box.y + HOT_CORNER_RADIUS) {
        server->hot_corner_pressure = 0.0;
        server->hot_corner_start = time_msec;
        server->hot_corner_armed = true;
        return;
    }

    if (!server->hot_corner_armed) {
        return;
    }

    double into_x = edge_dx < 0.0 ? -edge_dx : 0.0;
    double into_y = edge_dy < 0.0 ? -edge_dy : 0.0;
    if (into_x == 0.0 && into_y == 0.0) {
        return;
    }

    if (time_msec - server->hot_corner_time > HOT_CORNER_WINDOW_MS) {
        server->hot_corner_pressure = 0.0;
        server->hot_corner_start = time_msec;
    }

    server->hot_corner_time = time_msec;
    server->hot_corner_pressure += hypot(into_x, into_y);

    if (server->hot_corner_pressure < server->config.hot_corner_threshold ||
        time_msec - server->hot_corner_start < HOT_CORNER_MIN_MS) {
        return;
    }

    server->hot_corner_pressure = 0.0;
    server->hot_corner_armed = false;
    start_window_selector(server);
}

static bool handle_implicit_pointer_grab(struct poison_server *server,
                                         uint32_t time_msec,
                                         double edge_dx, double edge_dy) {
    if (server->seat->pointer_state.button_count == 0 ||
        server->seat->drag != NULL ||
        server->seat->pointer_state.focused_surface == NULL) {
        return false;
    }

    double sx = server->grab_sx + (server->cursor->x - server->grab_lx) + clamp_edge_drift(edge_dx);
    double sy = server->grab_sy + (server->cursor->y - server->grab_ly) + clamp_edge_drift(edge_dy);
    queue_pointer_motion(server, time_msec, sx, sy);
    return true;
}

void server_cursor_motion(struct wl_listener *listener, void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *event = data;

    bool was_blanked = server->screens_blanked;
    reset_idle_timer(server);

    queue_relative_motion(server, event->time_msec, event->delta_x,
                          event->delta_y, event->unaccel_dx, event->unaccel_dy);

    double edge_dx = 0.0, edge_dy = 0.0;
    if (server->active_constraint) {
        if (server->active_constraint->type ==
            WLR_POINTER_CONSTRAINT_V1_LOCKED) {
            /* This is "mouselook". */
            return;
        }
        /* XXX: This needs more testing. */
        double lx, ly;
        if (constraint_confine(server, server->active_constraint,
                               server->cursor->x, server->cursor->y,
                               server->cursor->x + event->delta_x,
                               server->cursor->y + event->delta_y,
                               &lx, &ly)) {
            wlr_cursor_warp_closest(server->cursor, &event->pointer->base, lx,
                                    ly);
        } else {
            wlr_cursor_move(server->cursor, &event->pointer->base,
                            event->delta_x, event->delta_y);
        }
    } else {
        double before_x = server->cursor->x;
        double before_y = server->cursor->y;
        wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x,
                        event->delta_y);
        edge_dx = event->delta_x - (server->cursor->x - before_x);
        edge_dy = event->delta_y - (server->cursor->y - before_y);
    }

    update_drag_icons(server);

    if (handle_implicit_pointer_grab(server, event->time_msec, edge_dx,
                                     edge_dy)) {
        return;
    }

    double sx, sy;
    struct wlr_surface *surface = NULL;
    struct wlr_scene_node *node =
        wlr_scene_node_at(&server->scene_layers->node, server->cursor->x,
                          server->cursor->y, &sx, &sy);

    if (node && node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_buffer *scene_buffer =
            wlr_scene_buffer_from_node(node);
        struct wlr_scene_surface *scene_surface =
            wlr_scene_surface_try_from_buffer(scene_buffer);
        if (scene_surface) {
            surface = scene_surface->surface;
        }
    }

    if (surface) {
        wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
        queue_pointer_motion(server, event->time_msec, sx, sy);
    } else {
        set_cursor_image(server, poison_cursor_image_xcursor("default"));
        drop_pending_relative_motion(server);
        poison_flush_pointer_motion(server);
        wlr_seat_pointer_notify_clear_focus(server->seat);
    }
    update_pointer_constraint(server, surface);

    if (!was_blanked) {
        handle_hot_corner(server, event->time_msec, edge_dx, edge_dy);
    }
}

void server_cursor_motion_absolute(struct wl_listener *listener,
                                   void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;

    reset_idle_timer(server);

    double abs_lx, abs_ly;
    wlr_cursor_absolute_to_layout_coords(server->cursor, &event->pointer->base,
                                         event->x, event->y, &abs_lx, &abs_ly);
    bool same_device = server->pointer_abs_valid &&
        server->pointer_abs_device == &event->pointer->base;
    double abs_dx = same_device ? abs_lx - server->pointer_abs_lx : 0.0;
    double abs_dy = same_device ? abs_ly - server->pointer_abs_ly : 0.0;
    track_pointer_abs_device(server, &event->pointer->base);
    server->pointer_abs_lx = abs_lx;
    server->pointer_abs_ly = abs_ly;
    server->pointer_abs_valid = true;
    queue_relative_motion(server, event->time_msec, abs_dx, abs_dy, abs_dx,
                          abs_dy);

    if (server->active_constraint) {
        if (server->active_constraint->type ==
            WLR_POINTER_CONSTRAINT_V1_LOCKED) {
            return;
        }
        double old_lx = server->cursor->x;
        double old_ly = server->cursor->y;
        wlr_cursor_warp_absolute(server->cursor, &event->pointer->base,
                                 event->x, event->y);
        double new_lx = server->cursor->x;
        double new_ly = server->cursor->y;
        double lx, ly;
        if (constraint_confine(server, server->active_constraint, old_lx,
                               old_ly, new_lx, new_ly, &lx, &ly)) {
            wlr_cursor_warp_closest(server->cursor, &event->pointer->base, lx,
                                    ly);
        }
    } else {
        wlr_cursor_warp_absolute(server->cursor, &event->pointer->base,
                                 event->x, event->y);
    }

    update_drag_icons(server);

    if (handle_implicit_pointer_grab(server, event->time_msec, 0.0, 0.0)) {
        return;
    }

    double sx, sy;
    struct wlr_surface *surface = NULL;
    struct wlr_scene_node *node =
        wlr_scene_node_at(&server->scene_layers->node, server->cursor->x,
                          server->cursor->y, &sx, &sy);

    if (node && node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_buffer *scene_buffer =
            wlr_scene_buffer_from_node(node);
        struct wlr_scene_surface *scene_surface =
            wlr_scene_surface_try_from_buffer(scene_buffer);
        if (scene_surface) {
            surface = scene_surface->surface;
        }
    }

    if (surface) {
        wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
        queue_pointer_motion(server, event->time_msec, sx, sy);
    } else {
        set_cursor_image(server, poison_cursor_image_xcursor("default"));
        drop_pending_relative_motion(server);
        poison_flush_pointer_motion(server);
        wlr_seat_pointer_notify_clear_focus(server->seat);
    }
    update_pointer_constraint(server, surface);
}

#ifndef BTN_LEFT
#define BTN_LEFT 0x110
#endif

static void handle_frame_click(struct poison_server *server,
                               struct wlr_scene_node *node) {
    struct poison_toplevel *toplevel =
        poison_decoration_toplevel_at(server, node);
    struct poison_xwayland_view *view = NULL;
    if (!toplevel) {
        view = poison_decoration_xwayland_at(server, node);
    }
    if (!toplevel && !view) {
        return;
    }

    struct poison_frame *frame = toplevel ? toplevel->frame : view->frame;
    enum poison_frame_button button = poison_frame_button_at(frame, node);

    if (toplevel) {
        focus_toplevel(toplevel);
    } else {
        focus_xwayland_view(server, view);
    }

    switch (button) {
    case POISON_FRAME_BUTTON_CLOSE:
        if (toplevel) {
            close_toplevel(toplevel);
        } else if (view->xwayland_surface) {
            wlr_xwayland_surface_close(view->xwayland_surface);
        }
        break;
    case POISON_FRAME_BUTTON_MAXIMIZE:
        if (toplevel) {
            toggle_maximize_toplevel(toplevel);
        } else {
            toggle_maximize_xwayland_view(view);
        }
        break;
    default:
        break;
    }
}

void server_cursor_button(struct wl_listener *listener, void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *event = data;

    reset_idle_timer(server);

    double sx, sy;
    struct wlr_surface *surface = NULL;
    struct wlr_scene_node *node =
        wlr_scene_node_at(&server->scene_layers->node, server->cursor->x,
                          server->cursor->y, &sx, &sy);

    if (node && node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_buffer *scene_buffer =
            wlr_scene_buffer_from_node(node);
        struct wlr_scene_surface *scene_surface =
            wlr_scene_surface_try_from_buffer(scene_buffer);
        if (scene_surface) {
            surface = scene_surface->surface;
        }
    }

    if (!surface && event->state == WL_POINTER_BUTTON_STATE_PRESSED &&
        event->button == BTN_LEFT) {
        handle_frame_click(server, node);
    }

    if (surface && event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
        struct wlr_xdg_surface *xdg_surface =
            wlr_xdg_surface_try_from_wlr_surface(surface);
        if (xdg_surface && xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
            struct wlr_scene_tree *tree = xdg_surface->data;
            if (tree && tree->node.data) {
                struct poison_toplevel *toplevel = tree->node.data;
                focus_toplevel(toplevel);
            }
        } else {
            struct wlr_scene_node *check_node = node;
            bool found_xwayland = false;
            while (check_node && check_node->parent &&
                   check_node != &server->scene->tree.node && !found_xwayland) {
                if (check_node->type == WLR_SCENE_NODE_TREE) {
                    struct wlr_scene_tree *tree =
                        wlr_scene_tree_from_node(check_node);
                    if (tree->node.data) {
                        struct poison_xwayland_view *xwayland_view;
                        wl_list_for_each(xwayland_view, &server->xwayland_views, link) {
                            if (tree->node.data == xwayland_view) {
                                focus_xwayland_view(server, xwayland_view);
                                found_xwayland = true;
                                break;
                            }
                        }
                    }
                }
                check_node = &check_node->parent->node;
            }
        }

        server->grab_sx = sx;
        server->grab_sy = sy;
        server->grab_lx = server->cursor->x;
        server->grab_ly = server->cursor->y;
    }

    /* Deliver the up-to-date cursor position before the button, since motion is
     * otherwise only flushed once per frame. */
    poison_flush_pointer_motion(server);
    wlr_seat_pointer_notify_button(server->seat, event->time_msec,
                                   event->button, event->state);
    wlr_seat_pointer_notify_frame(server->seat);
}

void server_cursor_axis(struct wl_listener *listener, void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, cursor_axis);
    struct wlr_pointer_axis_event *event = data;
    reset_idle_timer(server);
    poison_flush_pointer_motion(server);
    wlr_seat_pointer_notify_axis(server->seat, event->time_msec,
                                 event->orientation, event->delta,
                                 event->delta_discrete, event->source,
                                 event->relative_direction);
    wlr_seat_pointer_notify_frame(server->seat);
}

void server_cursor_frame(struct wl_listener *listener, void *data) {
    (void)listener;
    (void)data;
}

void server_cursor_touch_down(struct wl_listener *listener, void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, cursor_touch_down);
    struct wlr_touch_down_event *event = data;

    reset_idle_timer(server);

    double lx, ly;
    wlr_cursor_absolute_to_layout_coords(server->cursor, &event->touch->base,
                                         event->x, event->y, &lx, &ly);

    double sx, sy;
    struct wlr_surface *surface = NULL;
    struct wlr_scene_node *node =
        wlr_scene_node_at(&server->scene_layers->node, lx, ly, &sx, &sy);

    if (node && node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_buffer *scene_buffer =
            wlr_scene_buffer_from_node(node);
        struct wlr_scene_surface *scene_surface =
            wlr_scene_surface_try_from_buffer(scene_buffer);
        if (scene_surface) {
            surface = scene_surface->surface;
        }
    }

    if (surface) {
        wlr_seat_touch_notify_down(server->seat, surface, event->time_msec,
                                   event->touch_id, sx, sy);
        wlr_cursor_warp_closest(server->cursor, NULL, lx, ly);
    } else {
        wlr_cursor_warp_closest(server->cursor, NULL, lx, ly);
        wlr_seat_pointer_notify_button(server->seat, event->time_msec,
                                       0x110,
                                       WL_POINTER_BUTTON_STATE_PRESSED);
        wlr_seat_pointer_notify_frame(server->seat);
    }
}

void server_cursor_touch_up(struct wl_listener *listener, void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, cursor_touch_up);
    struct wlr_touch_up_event *event = data;

    reset_idle_timer(server);

    struct wlr_touch_point *point =
        wlr_seat_touch_get_point(server->seat, event->touch_id);
    if (point) {
        wlr_seat_touch_notify_up(server->seat, event->time_msec, event->touch_id);
    } else {
        wlr_seat_pointer_notify_button(server->seat, event->time_msec,
                                       0x110,
                                       WL_POINTER_BUTTON_STATE_RELEASED);
        wlr_seat_pointer_notify_frame(server->seat);
    }
}

void server_cursor_touch_motion(struct wl_listener *listener, void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, cursor_touch_motion);
    struct wlr_touch_motion_event *event = data;

    reset_idle_timer(server);

    double lx, ly;
    wlr_cursor_absolute_to_layout_coords(server->cursor, &event->touch->base,
                                         event->x, event->y, &lx, &ly);

    double sx, sy;
    struct wlr_surface *surface = NULL;
    struct wlr_scene_node *node =
        wlr_scene_node_at(&server->scene_layers->node, lx, ly, &sx, &sy);

    if (node && node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_buffer *scene_buffer =
            wlr_scene_buffer_from_node(node);
        struct wlr_scene_surface *scene_surface =
            wlr_scene_surface_try_from_buffer(scene_buffer);
        if (scene_surface) {
            surface = scene_surface->surface;
        }
    }

    struct wlr_touch_point *point =
        wlr_seat_touch_get_point(server->seat, event->touch_id);

    if (surface && point) {
        wlr_seat_touch_notify_motion(server->seat, event->time_msec,
                                     event->touch_id, sx, sy);
        wlr_cursor_warp_closest(server->cursor, NULL, lx, ly);
    } else if (!point) {
        wlr_cursor_warp_closest(server->cursor, NULL, lx, ly);
        wlr_seat_pointer_notify_motion(server->seat, event->time_msec, sx, sy);
        wlr_seat_pointer_notify_frame(server->seat);
    }

    if (server->seat->drag) {
        wlr_cursor_warp_closest(server->cursor, NULL, lx, ly);
        update_drag_icons(server);
    }
}

void server_cursor_touch_frame(struct wl_listener *listener, void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, cursor_touch_frame);
    wlr_seat_touch_notify_frame(server->seat);
}

static void cursor_image_surface_destroy(struct wl_listener *listener,
                                         void *data_unused) {
    struct poison_server *server =
        wl_container_of(listener, server, cursor_image_surface_destroy);
    (void)data_unused;

    server->cursor_image.type = POISON_CURSOR_IMAGE_NONE;
    wl_list_remove(&server->cursor_image_surface_destroy.link);
    wl_list_init(&server->cursor_image_surface_destroy.link);
}

static struct poison_cursor_image poison_cursor_image_xcursor(const char *name) {
    struct poison_cursor_image image = {
        .type = POISON_CURSOR_IMAGE_XCURSOR,
        .xcursor_name = name};
    return image;
}

static struct poison_cursor_image poison_cursor_image_none(void) {
    struct poison_cursor_image image = {
        .type = POISON_CURSOR_IMAGE_NONE};
    return image;
}

static void set_cursor_image(struct poison_server *server,
                             struct poison_cursor_image image) {
    if (server->cursor_image.type == POISON_CURSOR_IMAGE_CLIENT &&
        !wl_list_empty(&server->cursor_image_surface_destroy.link)) {
        wl_list_remove(&server->cursor_image_surface_destroy.link);
        wl_list_init(&server->cursor_image_surface_destroy.link);
    }

    server->cursor_image = image;

    switch (image.type) {
    case POISON_CURSOR_IMAGE_NONE:
        wlr_cursor_unset_image(server->cursor);
        break;
    case POISON_CURSOR_IMAGE_XCURSOR:
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
                               image.xcursor_name);
        break;
    case POISON_CURSOR_IMAGE_CLIENT:
        wlr_cursor_set_surface(server->cursor, image.client.surface,
                               image.client.hotspot_x, image.client.hotspot_y);
        wl_list_init(&server->cursor_image_surface_destroy.link);
        wl_signal_add(&image.client.surface->events.destroy,
                      &server->cursor_image_surface_destroy);
        break;
    }
}

void seat_request_cursor(struct wl_listener *listener, void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, request_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *event = data;
    struct wlr_seat_client *focused_client =
        server->seat->pointer_state.focused_client;

    if (focused_client == event->seat_client) {
        struct poison_cursor_image image;
        if (event->surface) {
            image.type = POISON_CURSOR_IMAGE_CLIENT;
            image.client.surface = event->surface;
            image.client.hotspot_x = event->hotspot_x;
            image.client.hotspot_y = event->hotspot_y;
        } else {
            image = poison_cursor_image_none();
        }
        set_cursor_image(server, image);
    }
}

void seat_request_set_cursor_shape(struct wl_listener *listener, void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, request_set_cursor_shape);
    const struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
    struct wlr_seat_client *focused_client =
        server->seat->pointer_state.focused_client;

    if (focused_client == NULL ||
        event->seat_client->client != focused_client->client) {
        return;
    }

    const char *cursor_name = wlr_cursor_shape_v1_name(event->shape);
    set_cursor_image(server, poison_cursor_image_xcursor(cursor_name));
}

#define POINTER_FOCUS_CHURN_PER_SEC 100

static void seat_pointer_focus_change(struct wl_listener *listener,
                                      void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, pointer_focus_change);
    uint32_t now = poison_now_msec();

    if (now - server->pointer_focus_window_start >= 1000) {
        server->pointer_focus_window_start = now;
        server->pointer_focus_changes = 0;
        server->pointer_focus_warned = false;
    }

    server->pointer_focus_changes++;
    if (server->pointer_focus_changes > POINTER_FOCUS_CHURN_PER_SEC &&
        !server->pointer_focus_warned) {
        server->pointer_focus_warned = true;
        wlr_log(WLR_ERROR,
                "Pointer focus changed %u times in a second: something is "
                "thrashing enter/leave. This overruns client connection "
                "buffers and gets them disconnected.",
                server->pointer_focus_changes);
    }
}

void seat_request_set_selection(struct wl_listener *listener, void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, request_set_selection);
    struct wlr_seat_request_set_selection_event *event = data;
    wlr_seat_set_selection(server->seat, event->source, event->serial);
}

void seat_request_set_primary_selection(struct wl_listener *listener,
                                        void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, request_set_primary_selection);
    struct wlr_seat_request_set_primary_selection_event *event = data;
    wlr_seat_set_primary_selection(server->seat, event->source,
                                   event->serial);
}

void seat_request_start_drag(struct wl_listener *listener, void *data) {
    struct poison_server *server =
        wl_container_of(listener, server, request_start_drag);
    struct wlr_seat_request_start_drag_event *event = data;

    struct wlr_touch_point *point;
    if (wlr_seat_validate_pointer_grab_serial(server->seat, event->origin,
                                              event->serial)) {
        wlr_seat_start_pointer_drag(server->seat, event->drag, event->serial);
        track_drag(server, event->drag);
    } else if (wlr_seat_validate_touch_grab_serial(server->seat, event->origin,
                                                   event->serial, &point)) {
        wlr_seat_start_touch_drag(server->seat, event->drag, event->serial,
                                  point);
        track_drag(server, event->drag);
    } else {
        wlr_data_source_destroy(event->drag->source);
    }
}

int main(int argc, char *argv[]) {
    static struct option long_options[] = {
        {"version", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0}};

    int opt;
    while ((opt = getopt_long(argc, argv, "vh", long_options, NULL)) != -1) {
        switch (opt) {
        case 'v':
            printf("%s %s\n", VERSION, GIT_SHA);
            return 0;
        case 'h':
            printf("Usage: %s [OPTIONS]\n", argv[0]);
            printf("Options:\n");
            printf("  -v, --version    Show version information\n");
            printf("  -h, --help       Show this help message\n");
            return 0;
        default:
            fprintf(stderr, "Usage: %s [OPTIONS]\n", argv[0]);
            fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
            return 1;
        }
    }

    if (optind < argc) {
        fprintf(stderr, "Error: unknown argument '%s'\n", argv[optind]);
        fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
        return 1;
    }

    if (optind > 1 && optind == argc && optind - 1 < argc && strcmp(argv[optind - 1], "--") == 0) {
        fprintf(stderr, "Error: invalid option -- '--'\n");
        fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
        return 1;
    }

    wlr_log_init(WLR_INFO, NULL);

    struct sigaction sa;
    sa.sa_handler = handle_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        wlr_log(WLR_ERROR, "Failed to set up SIGCHLD handler!");
    }

    int exit_code = 1;
    struct poison_server server = {0};
    sigchld_server = &server;

    poison_config_init(&server.config);
    poison_config_load(&server.config);

    poison_config_apply_filter(&server.config);

    wl_list_init(&server.renderer_lost.link);
    wl_list_init(&server.output_layout_change.link);

    poison_device_apply_env(&server.config);

    server.window_selector_fd = -1;
    server.window_selector_event_source = NULL;
    server.window_selector_pid = 0;
    server.window_selector_reply_len = 0;
    server.window_selector_prev_toplevel = NULL;
    server.window_selector_prev_xwayland_view = NULL;
    server.window_selector_saved_prev_toplevel = NULL;
    server.window_selector_saved_prev_xwayland_view = NULL;

    server.console_fd = -1;
    server.console_event_source = NULL;
    server.console_pid = 0;

    server.launcher_fd = -1;
    server.launcher_event_source = NULL;
    server.launcher_pid = 0;
    server.launcher_prev_toplevel = NULL;
    server.launcher_prev_xwayland_view = NULL;
    server.launcher_saved_prev_toplevel = NULL;
    server.launcher_saved_prev_xwayland_view = NULL;

    server.indicator_pid = 0;
    server.notify_pid = 0;

    server.wl_display = wl_display_create();
    if (server.wl_display == NULL) {
        wlr_log(WLR_ERROR, "Failed to create wl_display!");
        return 1;
    }

#if WAYLAND_VERSION_MAJOR > 1 || \
    (WAYLAND_VERSION_MAJOR == 1 && \
     (WAYLAND_VERSION_MINOR > 22 || \
      (WAYLAND_VERSION_MINOR == 22 && WAYLAND_VERSION_MICRO >= 90)))
     /* Data too big for buffer (4080 + 20 > 4096) */
    wl_display_set_default_max_buffer_size(server.wl_display, 1 << 17);
#endif

    server.backend =
        wlr_backend_autocreate(wl_display_get_event_loop(server.wl_display), &server.session);
    if (server.backend == NULL) {
        wlr_log(WLR_ERROR, "Failed to create wlr_backend!");
        wl_display_destroy(server.wl_display);
        return 1;
    }

    server.renderer = wlr_renderer_autocreate(server.backend);
    if (server.renderer == NULL) {
        wlr_log(WLR_ERROR, "Failed to create wlr_renderer!");
        wlr_backend_destroy(server.backend);
        wl_display_destroy(server.wl_display);
        return 1;
    }

    wlr_renderer_init_wl_shm(server.renderer, server.wl_display);

    server.allocator =
        wlr_allocator_autocreate(server.backend, server.renderer);
    if (server.allocator == NULL) {
        wlr_log(WLR_ERROR, "Failed to create wlr_allocator!");
        wlr_renderer_destroy(server.renderer);
        wlr_backend_destroy(server.backend);
        wl_display_destroy(server.wl_display);
        return 1;
    }

    server.renderer_lost.notify = handle_renderer_lost;
    wl_signal_add(&server.renderer->events.lost, &server.renderer_lost);

    poison_device_clear_env();
    poison_device_log_topology(&server);

    int render_drm_fd = wlr_renderer_get_drm_fd(server.renderer);
    bool have_dmabuf =
        wlr_renderer_get_texture_formats(server.renderer,
                                         WLR_BUFFER_CAP_DMABUF) != NULL;
    bool renderer_timeline = server.renderer->features.timeline;
    bool backend_timeline = server.backend->features.timeline;
    bool have_timeline = renderer_timeline && backend_timeline;

    if (render_drm_fd < 0 || !have_dmabuf) {
        wlr_log(WLR_INFO, "Running without GPU acceleration (render fd=%d, "
                          "dmabuf=%s): GPU-dependent protocols will be disabled.",
                render_drm_fd, have_dmabuf ? "yes" : "no");
    }

    struct wlr_compositor *compositor =
        wlr_compositor_create(server.wl_display, 5, server.renderer);
    server.compositor = compositor;
    wlr_subcompositor_create(server.wl_display);
    wlr_data_device_manager_create(server.wl_display);
    wlr_primary_selection_v1_device_manager_create(server.wl_display);
    wlr_data_control_manager_v1_create(server.wl_display);
    wlr_ext_data_control_manager_v1_create(server.wl_display, 1);
    wlr_single_pixel_buffer_manager_v1_create(server.wl_display);
    wlr_alpha_modifier_v1_create(server.wl_display);
    wlr_content_type_manager_v1_create(server.wl_display, 1);
    wlr_fixes_create(server.wl_display, 1);

    server.linux_dmabuf = NULL;
    if (have_dmabuf) {
        server.linux_dmabuf =
            wlr_linux_dmabuf_v1_create_with_renderer(server.wl_display, 5,
                                                     server.renderer);
    } else {
        wlr_log(WLR_INFO, "Renderer does not support DMA-BUF: "
                          "skipping linux-dmabuf-v1.");
    }

    server.viewporter = wlr_viewporter_create(server.wl_display);
    server.fractional_scale_manager =
        wlr_fractional_scale_manager_v1_create(server.wl_display, 1);
    server.presentation = wlr_presentation_create(server.wl_display, server.backend, 1);

    server.linux_drm_syncobj_manager = NULL;
    server.syncobj_drm_fd = -1;
    if (render_drm_fd >= 0 && have_timeline) {
        server.syncobj_drm_fd = fcntl(render_drm_fd, F_DUPFD_CLOEXEC, 0);
        if (server.syncobj_drm_fd < 0) {
            wlr_log(WLR_ERROR, "Failed to duplicate the render device fd: "
                               "skipping linux-drm-syncobj-v1.");
        } else {
            server.linux_drm_syncobj_manager =
                wlr_linux_drm_syncobj_manager_v1_create(server.wl_display, 1,
                                                        server.syncobj_drm_fd);
        }
    } else {
        wlr_log(WLR_INFO, "Explicit synchronization unavailable "
                          "(render fd=%d, renderer timeline=%s, backend "
                          "timeline=%s): skipping linux-drm-syncobj-v1.",
                render_drm_fd, renderer_timeline ? "yes" : "no",
                backend_timeline ? "yes" : "no");
        if (!backend_timeline) {
            poison_device_log_timeline_blockers(&server);
        }
    }

    size_t tf_len = 0, prim_len = 0;
    enum wp_color_manager_v1_transfer_function *transfer_functions =
        wlr_color_manager_v1_transfer_function_list_from_renderer(server.renderer, &tf_len);
    enum wp_color_manager_v1_primaries *primaries =
        wlr_color_manager_v1_primaries_list_from_renderer(server.renderer, &prim_len);

    static const enum wp_color_manager_v1_render_intent render_intents[] = {
        WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL,
        WP_COLOR_MANAGER_V1_RENDER_INTENT_RELATIVE,
        WP_COLOR_MANAGER_V1_RENDER_INTENT_SATURATION,
        WP_COLOR_MANAGER_V1_RENDER_INTENT_ABSOLUTE,
    };

    struct wlr_color_manager_v1_options color_mgr_options = {
        .features = {
            .icc_v2_v4 = false,
            .parametric = true,
            .set_primaries = false,
            .set_tf_power = false,
            .set_luminances = false,
            .set_mastering_display_primaries = true,
            .extended_target_volume = false,
            .windows_scrgb = false,
        },
        .render_intents = render_intents,
        .render_intents_len = sizeof(render_intents) / sizeof(render_intents[0]),
        .transfer_functions = transfer_functions,
        .transfer_functions_len = tf_len,
        .primaries = primaries,
        .primaries_len = prim_len,
    };

    /* A software renderer reports no transfer functions or primaries. */
    server.color_manager = NULL;
    if (tf_len > 0 && prim_len > 0) {
        server.color_manager = wlr_color_manager_v1_create(server.wl_display, 1,
                                                           &color_mgr_options);
    } else {
        wlr_log(WLR_INFO, "Renderer reports no color-management capabilities: "
                          "skipping color-management-v1.");
    }

    if (transfer_functions) {
        free(transfer_functions);
    }
    if (primaries) {
        free(primaries);
    }

    server.color_representation_manager =
        wlr_color_representation_manager_v1_create_with_renderer(server.wl_display, 1,
                                                                 server.renderer);

    server.gamma_control_manager =
        wlr_gamma_control_manager_v1_create(server.wl_display);
    if (server.gamma_control_manager) {
        if (server.renderer->features.output_color_transform) {
            server.gamma_control_manager->fallback_gamma_size =
                GAMMA_FALLBACK_RAMP_SIZE;
        } else {
            wlr_log(WLR_INFO, "Renderer cannot transform output colors: "
                              "gamma control is limited to outputs with a "
                              "hardware LUT.");
        }
    } else {
        wlr_log(WLR_ERROR, "Failed to create gamma control manager.");
    }

    server.tearing_control_manager = wlr_tearing_control_manager_v1_create(server.wl_display, 1);

    server.screencopy_manager = wlr_screencopy_manager_v1_create(server.wl_display);
    server.export_dmabuf_manager = wlr_export_dmabuf_manager_v1_create(server.wl_display);
    server.image_copy_capture_manager = wlr_ext_image_copy_capture_manager_v1_create(server.wl_display, 1);
    server.output_image_capture_source_manager =
        wlr_ext_output_image_capture_source_manager_v1_create(server.wl_display, 1);

    server.output_layout = wlr_output_layout_create(server.wl_display);
    server.output_layout_change.notify = handle_output_layout_change;
    wl_signal_add(&server.output_layout->events.change,
                  &server.output_layout_change);

    server.xdg_output_manager =
        wlr_xdg_output_manager_v1_create(server.wl_display, server.output_layout);

    server.output_manager = wlr_output_manager_v1_create(server.wl_display);
    if (server.output_manager) {
        server.output_manager_apply.notify = output_manager_apply;
        wl_signal_add(&server.output_manager->events.apply,
                      &server.output_manager_apply);
        server.output_manager_test.notify = output_manager_test;
        wl_signal_add(&server.output_manager->events.test,
                      &server.output_manager_test);
    } else {
        wlr_log(WLR_ERROR, "Failed to create output manager.");
    }

    wl_list_init(&server.outputs);
    server.new_output.notify = server_new_output;
    wl_signal_add(&server.backend->events.new_output, &server.new_output);

    wl_list_init(&server.session_active.link);
    if (server.session) {
        server.session_active.notify = server_session_active;
        wl_signal_add(&server.session->events.active, &server.session_active);
    }

    server.scene = wlr_scene_create();
    server.scene_layout =
        wlr_scene_attach_output_layout(server.scene, server.output_layout);

    server.scene_layers = wlr_scene_tree_create(&server.scene->tree);
    server.layer_tree_background =
        wlr_scene_tree_create(server.scene_layers);
    server.layer_tree_bottom = wlr_scene_tree_create(server.scene_layers);
    server.tree_toplevels = wlr_scene_tree_create(server.scene_layers);
    server.layer_tree_top = wlr_scene_tree_create(server.scene_layers);
    server.layer_tree_overlay = wlr_scene_tree_create(server.scene_layers);
    /* Not picked. */
    server.drag_icon_tree = wlr_scene_tree_create(&server.scene->tree);

    poison_render_init(&server);
    poison_decoration_init(&server);

    wl_list_init(&server.toplevels);
    wl_list_init(&server.view_order);
    server.next_view_serial = 0;
    wl_list_init(&server.hsplit_toplevels);
    wl_list_init(&server.hsplit_xwayland_views);
    server.hsplit_active = false;
    server.hsplit_focused_slot = -1;
    server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 6);
    if (server.xdg_shell) {
        server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
        wl_signal_add(&server.xdg_shell->events.new_toplevel,
                      &server.new_xdg_toplevel);
    } else {
        wlr_log(WLR_ERROR, "Failed to create xdg-shell: "
                           "applications using it will not work.");
    }

    server.xdg_decoration_manager =
        wlr_xdg_decoration_manager_v1_create(server.wl_display, 1);
    if (server.xdg_decoration_manager) {
        server.new_decoration.notify = server_new_decoration;
        wl_signal_add(&server.xdg_decoration_manager->events.new_toplevel_decoration, &server.new_decoration);
    } else {
        wlr_log(WLR_ERROR, "Failed to create xdg-decoration manager: "
                           "server-side decoration negotiation will be unavailable.");
    }

    server.server_decoration_manager =
        wlr_server_decoration_manager_create(server.wl_display);
    wlr_server_decoration_manager_set_default_mode(
        server.server_decoration_manager,
        WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);

    server.xdg_activation_v1 =
        wlr_xdg_activation_v1_create(server.wl_display);
    if (server.xdg_activation_v1) {
        server.xdg_activation_v1->token_timeout_msec = 10000;
        server.request_activate.notify = server_request_activate;
        wl_signal_add(&server.xdg_activation_v1->events.request_activate,
                      &server.request_activate);
    } else {
        wlr_log(WLR_ERROR, "Failed to create xdg-activation manager.");
    }

    server.foreign_registry =
        wlr_xdg_foreign_registry_create(server.wl_display);
    server.foreign_v1 =
        wlr_xdg_foreign_v1_create(server.wl_display, server.foreign_registry);
    server.foreign_v2 =
        wlr_xdg_foreign_v2_create(server.wl_display, server.foreign_registry);

    wlr_xdg_wm_dialog_v1_create(server.wl_display, 1);
    wlr_xdg_system_bell_v1_create(server.wl_display, 1);
    wlr_xdg_toplevel_tag_manager_v1_create(server.wl_display, 1);

    wl_list_init(&server.layer_surfaces);
    server.layer_shell = wlr_layer_shell_v1_create(server.wl_display, 4);
    if (server.layer_shell) {
        server.new_layer_surface.notify = server_new_layer_surface;
        wl_signal_add(&server.layer_shell->events.new_surface,
                      &server.new_layer_surface);
    } else {
        wlr_log(WLR_ERROR, "Failed to create layer-shell.");
    }

    server.idle_inhibit_manager =
        wlr_idle_inhibit_v1_create(server.wl_display);
    if (server.idle_inhibit_manager) {
        server.new_idle_inhibitor.notify = server_new_idle_inhibitor;
        wl_signal_add(&server.idle_inhibit_manager->events.new_inhibitor,
                      &server.new_idle_inhibitor);
    } else {
        wlr_log(WLR_ERROR, "Failed to create idle-inhibit manager.");
    }
    wl_list_init(&server.idle_inhibitors);

    server.keyboard_shortcuts_inhibit_manager =
        wlr_keyboard_shortcuts_inhibit_v1_create(server.wl_display);
    if (server.keyboard_shortcuts_inhibit_manager) {
        server.new_keyboard_shortcuts_inhibitor.notify =
            server_new_keyboard_shortcuts_inhibitor;
        wl_signal_add(
            &server.keyboard_shortcuts_inhibit_manager->events.new_inhibitor,
            &server.new_keyboard_shortcuts_inhibitor);
    } else {
        wlr_log(WLR_ERROR, "Failed to create keyboard-shortcuts-inhibit "
                           "manager.");
    }

    server.idle_timeout_ms = server.config.idle_timeout_ms;
    server.screens_blanked = false;
    server.idle_timer = NULL;
    if (server.idle_timeout_ms > 0) {
        struct wl_event_loop *event_loop = wl_display_get_event_loop(server.wl_display);
        server.idle_timer = wl_event_loop_add_timer(event_loop, idle_timer_handler, &server);
        if (server.idle_timer) {
            wl_event_source_timer_update(server.idle_timer, server.idle_timeout_ms);
            wlr_log(WLR_INFO, "Idle blanking enabled with timeout of %u ms.", server.idle_timeout_ms);
        } else {
            wlr_log(WLR_ERROR, "Failed to create idle timer!");
        }
    }

    wl_list_init(&server.xwayland_views);
    wl_list_init(&server.xwayland_unmanaged);
    server.xwayland = wlr_xwayland_create(server.wl_display, compositor, false);
    if (server.xwayland) {
        server.new_xwayland_surface.notify = server_new_xwayland_surface;
        wl_signal_add(&server.xwayland->events.new_surface,
                      &server.new_xwayland_surface);
        server.xwayland_ready.notify = server_xwayland_ready;
        wl_signal_add(&server.xwayland->events.ready,
                      &server.xwayland_ready);
        setenv("DISPLAY", server.xwayland->display_name, true);
        wlr_log(WLR_INFO, "XWayland running on display %s.",
                server.xwayland->display_name);
    } else {
        wlr_log(WLR_INFO, "XWayland support not available.");
    }

    server.cursor = wlr_cursor_create();
    if (!server.cursor) {
        wlr_log(WLR_ERROR, "Failed to create cursor!");
        goto cleanup;
    }
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

    const char *cursor_theme = getenv("XCURSOR_THEME");
    if (!cursor_theme) {
        cursor_theme = "default";
    }
    server.cursor_theme_name = strdup(cursor_theme);
    if (!server.cursor_theme_name) {
        wlr_log(WLR_ERROR, "Failed to duplicate cursor theme name!");
        goto cleanup;
    }
    setenv("XCURSOR_THEME", cursor_theme, true);

    const char *cursor_size_str = getenv("XCURSOR_SIZE");
    int cursor_size = 24;
    if (cursor_size_str) {
        char *end;
        long size = strtol(cursor_size_str, &end, 10);
        if (*end == '\0' && size >= 1 && size <= 128) {
            cursor_size = (int)size;
        }
    }

    char cursor_size_buf[16];
    snprintf(cursor_size_buf, sizeof(cursor_size_buf), "%d", cursor_size);
    setenv("XCURSOR_SIZE", cursor_size_buf, true);

    server.cursor_mgr =
        wlr_xcursor_manager_create(server.cursor_theme_name, cursor_size);
    if (!server.cursor_mgr) {
        wlr_log(WLR_ERROR, "Failed to create cursor manager!");
        goto cleanup;
    }
    wlr_xcursor_manager_load(server.cursor_mgr, 1);

    server.cursor_image.type = POISON_CURSOR_IMAGE_XCURSOR;
    server.cursor_image.xcursor_name = "default";
    server.cursor_image_surface_destroy.notify = cursor_image_surface_destroy;
    wl_list_init(&server.cursor_image_surface_destroy.link);
    server.pointer_abs_device_destroy.notify = pointer_abs_device_destroy;
    wl_list_init(&server.pointer_abs_device_destroy.link);

    server.hot_corner_armed = true;
    if (server.config.hot_corner_enabled) {
        wlr_log(WLR_INFO, "Top left hot corner enabled at %d pixels of shove.",
                server.config.hot_corner_threshold);
    }

    server.cursor_motion.notify = server_cursor_motion;
    wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
    server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
    wl_signal_add(&server.cursor->events.motion_absolute,
                  &server.cursor_motion_absolute);
    server.cursor_button.notify = server_cursor_button;
    wl_signal_add(&server.cursor->events.button, &server.cursor_button);
    server.cursor_axis.notify = server_cursor_axis;
    wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
    server.cursor_frame.notify = server_cursor_frame;
    wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);
    server.cursor_touch_down.notify = server_cursor_touch_down;
    wl_signal_add(&server.cursor->events.touch_down,
                  &server.cursor_touch_down);
    server.cursor_touch_up.notify = server_cursor_touch_up;
    wl_signal_add(&server.cursor->events.touch_up, &server.cursor_touch_up);
    server.cursor_touch_motion.notify = server_cursor_touch_motion;
    wl_signal_add(&server.cursor->events.touch_motion,
                  &server.cursor_touch_motion);
    server.cursor_touch_frame.notify = server_cursor_touch_frame;
    wl_signal_add(&server.cursor->events.touch_frame,
                  &server.cursor_touch_frame);

    server.pointer_constraints = wlr_pointer_constraints_v1_create(server.wl_display);
    if (server.pointer_constraints) {
        server.new_pointer_constraint.notify = server_new_pointer_constraint;
        wl_signal_add(&server.pointer_constraints->events.new_constraint,
                      &server.new_pointer_constraint);
    } else {
        wlr_log(WLR_ERROR, "Failed to create pointer-constraints manager.");
    }
    wl_list_init(&server.pointer_constraints_list);

    server.relative_pointer_manager = wlr_relative_pointer_manager_v1_create(server.wl_display);

    wl_list_init(&server.keyboards);
    server.new_input.notify = server_new_input;
    wl_signal_add(&server.backend->events.new_input, &server.new_input);

    server.seat = wlr_seat_create(server.wl_display, "seat0");
    server.request_cursor.notify = seat_request_cursor;
    wl_signal_add(&server.seat->events.request_set_cursor,
                  &server.request_cursor);
    server.request_set_selection.notify = seat_request_set_selection;
    wl_signal_add(&server.seat->events.request_set_selection,
                  &server.request_set_selection);
    server.request_set_primary_selection.notify =
        seat_request_set_primary_selection;
    wl_signal_add(&server.seat->events.request_set_primary_selection,
                  &server.request_set_primary_selection);
    server.request_start_drag.notify = seat_request_start_drag;
    wl_signal_add(&server.seat->events.request_start_drag,
                  &server.request_start_drag);
    server.pointer_focus_change.notify = seat_pointer_focus_change;
    wl_signal_add(&server.seat->pointer_state.events.focus_change,
                  &server.pointer_focus_change);

    struct wlr_cursor_shape_manager_v1 *cursor_shape_manager =
        wlr_cursor_shape_manager_v1_create(server.wl_display, 1);
    if (cursor_shape_manager) {
        server.request_set_cursor_shape.notify = seat_request_set_cursor_shape;
        wl_signal_add(&cursor_shape_manager->events.request_set_shape,
                      &server.request_set_cursor_shape);
    } else {
        wl_list_init(&server.request_set_cursor_shape.link);
        wlr_log(WLR_ERROR, "Failed to create cursor-shape manager.");
    }

    const char *socket = wl_display_add_socket_auto(server.wl_display);
    if (!socket) {
        wlr_log(WLR_ERROR, "Failed to create Wayland socket!");
        goto cleanup;
    }

    if (!wlr_backend_start(server.backend)) {
        wlr_log(WLR_ERROR, "Failed to start backend!");
        goto cleanup;
    }

    set_cursor_image(&server, poison_cursor_image_xcursor("default"));

    setenv("WAYLAND_DISPLAY", socket, true);

    start_notification_daemon(&server);

    poison_config_execute_commands(&server.config);

    poison_diag_init(&server);

    wl_display_run(server.wl_display);

    exit_code = 0;

cleanup:
    wl_list_remove(&server.output_layout_change.link);
    if (server.arrange_idle) {
        wl_event_source_remove(server.arrange_idle);
        server.arrange_idle = NULL;
    }

    poison_decoration_finish(&server);
    poison_diag_finish(&server);
    kill_tracked_child(&server.notify_pid);

    if (server.xwayland) {
        wl_list_remove(&server.new_xwayland_surface.link);
        wl_list_remove(&server.xwayland_ready.link);
        wlr_xwayland_destroy(server.xwayland);
        server.xwayland = NULL;
    }

    if (server.wl_display) {
        wl_display_destroy_clients(server.wl_display);
    }
    if (server.scene) {
        wlr_scene_node_destroy(&server.scene->tree.node);
    }

    if (server.cursor_mgr) {
        wlr_xcursor_manager_destroy(server.cursor_mgr);
    }
    if (server.cursor_theme_name) {
        free(server.cursor_theme_name);
    }

    if (server.cursor) {
        wl_list_remove(&server.cursor_motion.link);
        wl_list_remove(&server.cursor_motion_absolute.link);
        wl_list_remove(&server.cursor_button.link);
        wl_list_remove(&server.cursor_axis.link);
        wl_list_remove(&server.cursor_frame.link);
        wl_list_remove(&server.cursor_touch_down.link);
        wl_list_remove(&server.cursor_touch_up.link);
        wl_list_remove(&server.cursor_touch_motion.link);
        wl_list_remove(&server.cursor_touch_frame.link);
        if (server.cursor_image.type == POISON_CURSOR_IMAGE_CLIENT &&
            !wl_list_empty(&server.cursor_image_surface_destroy.link)) {
            wl_list_remove(&server.cursor_image_surface_destroy.link);
        }
        if (!wl_list_empty(&server.pointer_abs_device_destroy.link)) {
            wl_list_remove(&server.pointer_abs_device_destroy.link);
        }
    }

    if (server.backend) {
        wl_list_remove(&server.new_input.link);
        wl_list_remove(&server.new_output.link);
    }

    if (server.seat) {
        wl_list_remove(&server.request_cursor.link);
        wl_list_remove(&server.request_set_cursor_shape.link);
        wl_list_remove(&server.request_set_selection.link);
        wl_list_remove(&server.request_set_primary_selection.link);
        wl_list_remove(&server.request_start_drag.link);
        wl_list_remove(&server.pointer_focus_change.link);
    }

    if (server.xdg_shell) {
        wl_list_remove(&server.new_xdg_toplevel.link);
    }

    if (server.xdg_decoration_manager) {
        wl_list_remove(&server.new_decoration.link);
    }

    if (server.xdg_activation_v1) {
        wl_list_remove(&server.request_activate.link);
    }

    wl_list_remove(&server.session_active.link);

    if (server.layer_shell) {
        wl_list_remove(&server.new_layer_surface.link);
    }

    if (server.idle_inhibit_manager) {
        wl_list_remove(&server.new_idle_inhibitor.link);
    }

    if (server.keyboard_shortcuts_inhibit_manager) {
        wl_list_remove(&server.new_keyboard_shortcuts_inhibitor.link);
    }

    if (server.pointer_constraints) {
        wl_list_remove(&server.new_pointer_constraint.link);
    }

    if (server.output_manager) {
        wl_list_remove(&server.output_manager_apply.link);
        wl_list_remove(&server.output_manager_test.link);
    }

    wl_list_remove(&server.renderer_lost.link);

    if (server.idle_timer) {
        wl_event_source_remove(server.idle_timer);
    }

    if (server.cursor) {
        wlr_cursor_destroy(server.cursor);
    }
    if (server.allocator) {
        wlr_allocator_destroy(server.allocator);
    }
    if (server.renderer) {
        wlr_renderer_destroy(server.renderer);
    }
    if (server.backend) {
        wlr_backend_destroy(server.backend);
    }
    if (server.wl_display) {
        wl_display_destroy(server.wl_display);
    }

    if (server.syncobj_drm_fd >= 0) {
        close(server.syncobj_drm_fd);
        server.syncobj_drm_fd = -1;
    }

    poison_config_cleanup(&server.config);

    return exit_code;
}
