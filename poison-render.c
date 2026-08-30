/* © 2026 dancingmirrors */

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
}

void poison_render_finish(struct poison_server *server) {
    (void)server;
}

void poison_render_request_repaint(struct poison_output *output) {
    output->force_repaint = true;
    output->commit_failures = 0;
    wlr_output_schedule_frame(output->wlr_output);
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
        /* Whatever we failed to present is still owed a full repaint. */
        wlr_damage_ring_add_whole(&scene_output->damage_ring);

        if (++output->commit_failures > MAX_COMMIT_RETRIES) {
            wlr_log(WLR_ERROR, "Scene output commit failed %u times. Leaving "
                               "the retries to the frame clock.",
                    output->commit_failures);
            output->commit_failures = 0;
        } else {
            wlr_log(WLR_DEBUG, "Scene output commit failed. Scheduling a retry.");
            wlr_output_schedule_frame(output->wlr_output);
        }
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);
}
