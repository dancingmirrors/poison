/* © 2026 dancingmirrors */

#include <limits.h>

#include <wlr/types/wlr_damage_ring.h>
#include <wlr/types/wlr_scene.h>

#include "poison.h"

void poison_render_init(struct poison_server *server) {
    if (!server || !server->scene) {
        wlr_log(WLR_ERROR, "Render init failed!");
        return;
    }

    if (server->linux_dmabuf) {
        wlr_scene_set_linux_dmabuf_v1(server->scene, server->linux_dmabuf);
    }

    if (server->color_manager) {
        wlr_scene_set_color_manager_v1(server->scene, server->color_manager);
    }

    if (server->gamma_control_manager) {
        wlr_scene_set_gamma_control_manager_v1(server->scene,
                                               server->gamma_control_manager);
    }
}

void poison_render_finish(struct poison_server *server) {
    (void)server;
}

void poison_render_output_finish(struct poison_output *output) {
    if (output->retry_timer) {
        wl_event_source_remove(output->retry_timer);
        output->retry_timer = NULL;
    }
}

static int output_retry_handler(void *data) {
    struct poison_output *output = data;
    output->force_repaint = true;
    wlr_output_schedule_frame(output->wlr_output);
    return 0;
}

void poison_render_request_repaint(struct poison_output *output) {
    output->force_repaint = true;
    output->commit_failures = 0;
    if (output->retry_timer) {
        wl_event_source_timer_update(output->retry_timer, 0);
    }
    wlr_output_schedule_frame(output->wlr_output);
}

static void schedule_commit_retry(struct poison_output *output) {
    if (!output->retry_timer) {
        struct wl_event_loop *loop =
            wl_display_get_event_loop(output->server->wl_display);
        output->retry_timer =
            wl_event_loop_add_timer(loop, output_retry_handler, output);
        if (!output->retry_timer) {
            wlr_log(WLR_ERROR, "Failed to create a commit retry timer for "
                               "output %s!",
                    output->wlr_output->name);
            /* Better a busy loop than a dead screen. */
            wlr_output_schedule_frame(output->wlr_output);
            return;
        }
    }

    unsigned int delay = COMMIT_RETRY_BASE_MS;
    for (unsigned int i = MAX_COMMIT_RETRIES + 1;
         i < output->commit_failures && delay < COMMIT_RETRY_MAX_MS; i++) {
        delay *= 2;
    }
    if (delay > COMMIT_RETRY_MAX_MS) {
        delay = COMMIT_RETRY_MAX_MS;
    }

    if (output->commit_failures == MAX_COMMIT_RETRIES + 1) {
        wlr_log(WLR_ERROR, "Output %s has failed %u commits in a row. Backing "
                           "off and retrying.",
                output->wlr_output->name, output->commit_failures);
    } else {
        wlr_log(WLR_DEBUG, "Output %s commit failure %u: retrying in %u ms.",
                output->wlr_output->name, output->commit_failures, delay);
    }

    wl_event_source_timer_update(output->retry_timer, (int)delay);
}

void poison_render_output_frame(struct wl_listener *listener, void *data) {
    struct poison_output *output =
        wl_container_of(listener, output, frame);
    struct poison_server *server = output->server;
    struct wlr_scene *scene = server->scene;
    struct wlr_scene_output *scene_output =
        wlr_scene_get_scene_output(scene, output->wlr_output);

    if (!scene_output) {
        wlr_log(WLR_ERROR, "No scene output for wlr_output!");
        return;
    }

    if (server->session && !server->session->active) {
        return;
    }

    /* Deliver any coalesced pointer motion on the frame clock, even when the
     * scene itself needs no repaint. */
    poison_flush_pointer_motion(server);

    if (!output->force_repaint && !wlr_scene_output_needs_frame(scene_output)) {
        return;
    }

    if (output->force_repaint) {
        wlr_damage_ring_add_whole(&scene_output->damage_ring);
    }

    if (wlr_scene_output_commit(scene_output, NULL)) {
        output->commit_failures = 0;
        output->force_repaint = false;
    } else {
        /* Whatever we failed to present is still owed a full repaint, so
         * keep asking for one until it lands. */
        wlr_damage_ring_add_whole(&scene_output->damage_ring);
        output->force_repaint = true;

        if (output->commit_failures < UINT_MAX) {
            output->commit_failures++;
        }

        if (output->commit_failures > MAX_COMMIT_RETRIES) {
            schedule_commit_retry(output);
        } else {
            wlr_log(WLR_DEBUG, "Scene output commit failed. Scheduling a retry.");
            wlr_output_schedule_frame(output->wlr_output);
        }
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);
}
