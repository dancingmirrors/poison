#include <stdlib.h>
#include <string.h>

#include "poison-test.h"

#define TILE_W (OUTPUT_WIDTH - 10)
#define TILE_H (OUTPUT_HEIGHT - 10)

#define LOST_INITIAL_STATE \
    "wlroots rewrites _NET_WM_STATE from what it knows when the window maps, " \
    "before it has read the property, so a fullscreen or modal state set " \
    "before mapping is lost"

#define WITHDRAWN_STATE 0
#define NORMAL_STATE 1
#define ICONIC_STATE 3

static struct xwindow *map_xwindow(struct xwindow_opts opts) {
    struct xwindow *window = xwindow_create(&opts);
    xwindow_map(window);
    if (!opts.override_redirect) {
        WAIT_UNTIL(x11_client_list_contains(window), 3000);
    }
    return window;
}

static bool xwindow_at(struct xwindow *window, int x, int y, int width,
                       int height) {
    return window->x == x && window->y == y && window->width == width &&
        window->height == height;
}

#define CHECK_X11_GEOMETRY(window, x_, y_, w_, h_) \
    CHECK(WAIT_UNTIL(xwindow_at((window), (x_), (y_), (w_), (h_)), 2000), \
          "X11 sees the window at %d,%d %dx%d, want %d,%d %dx%d", \
          (window)->x, (window)->y, (window)->width, (window)->height, \
          (x_), (y_), (w_), (h_))

TEST(x11_normal_window_tiled, .x11 = true) {
    x11_connect();
    struct xwindow *window = map_xwindow((struct xwindow_opts){
        .color = C_RED,
        .x = 30,
        .y = 40,
        .width = 400,
        .height = 300,
    });
    struct shot *shot = expect_window_at(C_RED, 5, 5, TILE_W, TILE_H);
    expect_no_frame(shot, C_RED);
    shot_free(shot);

    CHECK_X11_GEOMETRY(window, 5, 5, TILE_W, TILE_H);
    CHECK(xwindow_has_net_state(window, "MAXIMIZED_VERT") &&
              xwindow_has_net_state(window, "MAXIMIZED_HORZ"),
          "a tiled window should be reported as maximized");
    CHECK(xwindow_wm_state(window) == NORMAL_STATE,
          "WM_STATE should be NormalState, got %d", xwindow_wm_state(window));
    CHECK(x11_active_window() == window->id, "the window should be active");
    CHECK(x11_client_list_contains(window),
          "_NET_CLIENT_LIST should list the window");
}

TEST(x11_tiled_padding_20, .x11 = true, .rc = "padding \"20\"") {
    x11_connect();
    struct xwindow *window = map_xwindow((struct xwindow_opts){.color = C_GREEN});
    shot_free(expect_window_at(C_GREEN, 20, 20, OUTPUT_WIDTH - 40,
                               OUTPUT_HEIGHT - 40));
    CHECK_X11_GEOMETRY(window, 20, 20, OUTPUT_WIDTH - 40, OUTPUT_HEIGHT - 40);
}

TEST(x11_tiled_padding_0_keeps_one_pixel, .x11 = true, .rc = "padding \"0\"") {
    x11_connect();
    map_xwindow((struct xwindow_opts){.color = C_BLUE});
    shot_free(expect_window_at(C_BLUE, 1, 1, OUTPUT_WIDTH - 2, OUTPUT_HEIGHT - 2));
}

TEST(x11_normal_type_tiled, .x11 = true) {
    x11_connect();
    map_xwindow((struct xwindow_opts){.color = C_RED, .type = "NORMAL"});
    shot_free(expect_window_at(C_RED, 5, 5, TILE_W, TILE_H));
}

TEST(x11_min_size_only_tiled, .x11 = true) {
    x11_connect();
    map_xwindow((struct xwindow_opts){
        .color = C_RED,
        .min_width = 300,
        .min_height = 200,
        .inc_width = 7,
        .inc_height = 15,
    });
    shot_free(expect_window_at(C_RED, 5, 5, TILE_W, TILE_H));
}

TEST(x11_fixed_size_floats_with_frame, .x11 = true) {
    x11_connect();
    struct xwindow *window = map_xwindow((struct xwindow_opts){
        .color = C_PURPLE,
        .width = 400,
        .height = 300,
        .min_width = 400,
        .min_height = 300,
        .max_width = 400,
        .max_height = 300,
    });
    struct box want = centered(400, 300, true);
    struct shot *shot = expect_window_at(C_PURPLE, want.x, want.y, 400, 300);
    expect_frame(shot, C_PURPLE, true);
    shot_free(shot);
    CHECK_X11_GEOMETRY(window, want.x, want.y, 400, 300);
    CHECK(!xwindow_has_net_state(window, "MAXIMIZED_VERT"),
          "a floating window should not be reported as maximized");
    CHECK(x11_active_window() == window->id, "the window should be active");
}

TEST(x11_floating_ignores_requested_position, .x11 = true) {
    x11_connect();
    map_xwindow((struct xwindow_opts){
        .color = C_PURPLE,
        .x = 10,
        .y = 10,
        .width = 400,
        .height = 300,
        .min_width = 400,
        .min_height = 300,
        .max_width = 400,
        .max_height = 300,
    });
    struct box want = centered(400, 300, true);
    shot_free(expect_window_at(C_PURPLE, want.x, want.y, 400, 300));
}

TEST(x11_fixed_aspect_floats_with_frame, .x11 = true) {
    x11_connect();
    map_xwindow((struct xwindow_opts){
        .color = C_TEAL,
        .width = 640,
        .height = 360,
        .min_aspect_x = 16,
        .min_aspect_y = 9,
        .max_aspect_x = 16,
        .max_aspect_y = 9,
    });
    struct box want = centered(640, 360, true);
    struct shot *shot = expect_window_at(C_TEAL, want.x, want.y, 640, 360);
    expect_frame(shot, C_TEAL, true);
    shot_free(shot);
}

TEST(x11_aspect_range_tiles, .x11 = true) {
    x11_connect();
    map_xwindow((struct xwindow_opts){
        .color = C_TEAL,
        .width = 640,
        .height = 360,
        .min_aspect_x = 4,
        .min_aspect_y = 3,
        .max_aspect_x = 2,
        .max_aspect_y = 1,
    });
    struct shot *shot = expect_window_at(C_TEAL, 5, 5, TILE_W, TILE_H);
    expect_no_frame(shot, C_TEAL);
    shot_free(shot);
}

TEST(x11_motif_not_resizable_floats_with_frame, .x11 = true) {
    x11_connect();
    map_xwindow((struct xwindow_opts){
        .color = C_PURPLE,
        .width = 640,
        .height = 400,
        .motif_no_resize = true,
    });
    struct box want = centered(640, 400, true);
    struct shot *shot = expect_window_at(C_PURPLE, want.x, want.y, 640, 400);
    expect_frame(shot, C_PURPLE, true);
    shot_free(shot);
}

TEST(x11_transient_floats_without_frame, .x11 = true) {
    x11_connect();
    struct xwindow *parent = map_xwindow((struct xwindow_opts){.color = C_RED});
    shot_free(expect_window_at(C_RED, 5, 5, TILE_W, TILE_H));
    struct xwindow *child = map_xwindow((struct xwindow_opts){
        .color = C_BLUE,
        .width = 400,
        .height = 300,
        .transient_for = parent,
    });
    struct box want = centered(400, 300, false);
    struct shot *shot = expect_window_at(C_BLUE, want.x, want.y, 400, 300);
    expect_no_frame(shot, C_BLUE);
    CHECK(shot_count(shot, C_RED) == TILE_W * TILE_H - 400 * 300,
          "the parent should stay tiled behind its transient");
    shot_free(shot);
    CHECK(x11_active_window() == child->id, "the transient should be active");

    xwindow_destroy(child);
    shot_free(expect_window_at(C_RED, 5, 5, TILE_W, TILE_H));
    CHECK(WAIT_UNTIL(x11_active_window() == parent->id, 2000),
          "the parent should be active again");
}

TEST(x11_transient_for_unmapped_window_tiles, .x11 = true) {
    x11_connect();
    struct xwindow *owner = xwindow_create(&(struct xwindow_opts){.color = C_GRAY});
    struct xwindow *window = map_xwindow((struct xwindow_opts){
        .color = C_RED,
        .width = 400,
        .height = 300,
        .transient_for = owner,
    });
    struct shot *shot = expect_window_at(C_RED, 5, 5, TILE_W, TILE_H);
    expect_no_frame(shot, C_RED);
    shot_free(shot);
    CHECK(x11_active_window() == window->id, "the window should be active");
}

TEST(x11_dialog_type_floats_without_frame, .x11 = true) {
    x11_connect();
    map_xwindow((struct xwindow_opts){
        .color = C_BLUE,
        .width = 420,
        .height = 260,
        .type = "DIALOG",
    });
    struct box want = centered(420, 260, false);
    struct shot *shot = expect_window_at(C_BLUE, want.x, want.y, 420, 260);
    expect_no_frame(shot, C_BLUE);
    shot_free(shot);
}

TEST(x11_splash_floats_without_frame, .x11 = true) {
    x11_connect();
    map_xwindow((struct xwindow_opts){
        .color = C_ORANGE,
        .width = 500,
        .height = 250,
        .type = "SPLASH",
    });
    struct box want = centered(500, 250, false);
    struct shot *shot = expect_window_at(C_ORANGE, want.x, want.y, 500, 250);
    expect_no_frame(shot, C_ORANGE);
    shot_free(shot);
}

TEST(x11_transient_stays_floating_when_parent_hides, .x11 = true) {
    x11_connect();
    struct xwindow *parent = map_xwindow((struct xwindow_opts){.color = C_RED});
    shot_free(expect_window_at(C_RED, 5, 5, TILE_W, TILE_H));
    struct xwindow *child = map_xwindow((struct xwindow_opts){
        .color = C_BLUE,
        .width = 400,
        .height = 300,
        .transient_for = parent,
    });
    struct box want = centered(400, 300, false);
    shot_free(expect_window_at(C_BLUE, want.x, want.y, 400, 300));

    xwindow_unmap(parent);
    xwindow_set_size_hints(child, 100, 100, 0, 0);
    x11_sync();
    settle();
    shot_free(expect_window_at(C_BLUE, want.x, want.y, 400, 300));
}

TEST(x11_modal_floats_without_frame, .x11 = true, .xfail = LOST_INITIAL_STATE) {
    x11_connect();
    for (int i = 0; i < 4; i++) {
        struct xwindow *window = map_xwindow((struct xwindow_opts){
            .color = C_TEAL,
            .width = 360,
            .height = 240,
            .modal = true,
        });
        struct box want = centered(360, 240, false);
        struct shot *shot = expect_window_at(C_TEAL, want.x, want.y, 360, 240);
        expect_no_frame(shot, C_TEAL);
        shot_free(shot);
        CHECK(xwindow_has_net_state(window, "MODAL"),
              "window %d lost its modal state", i);
        xwindow_destroy(window);
        expect_gone(C_TEAL);
        if (test_failed()) {
            break;
        }
    }
}

TEST(x11_motif_no_border_floating_no_frame, .x11 = true) {
    x11_connect();
    map_xwindow((struct xwindow_opts){
        .color = C_PURPLE,
        .width = 400,
        .height = 300,
        .min_width = 400,
        .min_height = 300,
        .max_width = 400,
        .max_height = 300,
        .motif_no_border = true,
    });
    struct box want = centered(400, 300, false);
    struct shot *shot = expect_window_at(C_PURPLE, want.x, want.y, 400, 300);
    expect_no_frame(shot, C_PURPLE);
    shot_free(shot);
}

TEST(x11_motif_no_border_overridden, .x11 = true,
     .rc = "decoration_respect_client \"0\"") {
    x11_connect();
    map_xwindow((struct xwindow_opts){
        .color = C_PURPLE,
        .width = 400,
        .height = 300,
        .min_width = 400,
        .min_height = 300,
        .max_width = 400,
        .max_height = 300,
        .motif_no_border = true,
    });
    struct box want = centered(400, 300, true);
    struct shot *shot = expect_window_at(C_PURPLE, want.x, want.y, 400, 300);
    expect_frame(shot, C_PURPLE, true);
    shot_free(shot);
}

TEST(x11_motif_no_title_keeps_frame, .x11 = true) {
    x11_connect();
    map_xwindow((struct xwindow_opts){
        .color = C_PURPLE,
        .width = 400,
        .height = 300,
        .min_width = 400,
        .min_height = 300,
        .max_width = 400,
        .max_height = 300,
        .motif_no_title = true,
    });
    struct box want = centered(400, 300, true);
    struct shot *shot = expect_window_at(C_PURPLE, want.x, want.y, 400, 300);
    expect_frame(shot, C_PURPLE, true);
    shot_free(shot);
}

TEST(x11_client_decorated_window_tiled, .x11 = true) {
    x11_connect();
    map_xwindow((struct xwindow_opts){
        .color = C_ORANGE,
        .width = 900,
        .height = 600,
        .motif_no_border = true,
    });
    struct shot *shot = expect_window_at(C_ORANGE, 5, 5, TILE_W, TILE_H);
    expect_no_frame(shot, C_ORANGE);
    shot_free(shot);
}

TEST(x11_decorations_disabled, .x11 = true, .rc = "decorations \"0\"") {
    x11_connect();
    map_xwindow((struct xwindow_opts){
        .color = C_PURPLE,
        .width = 400,
        .height = 300,
        .min_width = 400,
        .min_height = 300,
        .max_width = 400,
        .max_height = 300,
    });
    struct box want = centered(400, 300, false);
    struct shot *shot = expect_window_at(C_PURPLE, want.x, want.y, 400, 300);
    expect_no_frame(shot, C_PURPLE);
    shot_free(shot);
}

TEST(x11_fullscreen_at_map, .x11 = true, .xfail = LOST_INITIAL_STATE) {
    x11_connect();
    for (int i = 0; i < 4; i++) {
        struct xwindow *window = map_xwindow((struct xwindow_opts){
            .color = C_ORANGE,
            .width = 640,
            .height = 480,
            .fullscreen = true,
        });
        struct shot *shot = expect_window_at(C_ORANGE, 0, 0, OUTPUT_WIDTH,
                                             OUTPUT_HEIGHT);
        CHECK(shot_count_frame_pixels(shot) == 0,
              "a fullscreen window should not have a frame");
        shot_free(shot);
        CHECK_X11_GEOMETRY(window, 0, 0, OUTPUT_WIDTH, OUTPUT_HEIGHT);
        CHECK(xwindow_has_net_state(window, "FULLSCREEN"),
              "window %d should be reported as fullscreen", i);
        xwindow_destroy(window);
        expect_gone(C_ORANGE);
        if (test_failed()) {
            break;
        }
    }
}

TEST(x11_fullscreen_and_back_while_tiled, .x11 = true) {
    x11_connect();
    struct xwindow *window = map_xwindow((struct xwindow_opts){.color = C_RED});
    shot_free(expect_window_at(C_RED, 5, 5, TILE_W, TILE_H));

    xwindow_request_state(window, 1, "FULLSCREEN", NULL);
    shot_free(expect_window_at(C_RED, 0, 0, OUTPUT_WIDTH, OUTPUT_HEIGHT));
    CHECK(xwindow_has_net_state(window, "FULLSCREEN"),
          "the window should be reported as fullscreen");

    xwindow_request_state(window, 0, "FULLSCREEN", NULL);
    shot_free(expect_window_at(C_RED, 5, 5, TILE_W, TILE_H));
    CHECK(!xwindow_has_net_state(window, "FULLSCREEN"),
          "the window should not be reported as fullscreen any more");
    CHECK_X11_GEOMETRY(window, 5, 5, TILE_W, TILE_H);
}

TEST(x11_fullscreen_and_back_while_floating, .x11 = true) {
    x11_connect();
    struct xwindow *window = map_xwindow((struct xwindow_opts){
        .color = C_PURPLE,
        .width = 400,
        .height = 300,
        .min_width = 400,
        .min_height = 300,
        .max_width = 400,
        .max_height = 300,
    });
    struct box floating = centered(400, 300, true);
    shot_free(expect_window_at(C_PURPLE, floating.x, floating.y, 400, 300));

    xwindow_request_state(window, 1, "FULLSCREEN", NULL);
    struct shot *shot = expect_window_at(C_PURPLE, 0, 0, OUTPUT_WIDTH,
                                         OUTPUT_HEIGHT);
    CHECK(shot_count_frame_pixels(shot) == 0,
          "a fullscreen window should not have a frame");
    shot_free(shot);

    xwindow_request_state(window, 0, "FULLSCREEN", NULL);
    shot = expect_window_at(C_PURPLE, floating.x, floating.y, 400, 300);
    expect_frame(shot, C_PURPLE, true);
    shot_free(shot);
}

TEST(x11_fixed_size_after_fullscreen_at_map_floats, .x11 = true) {
    x11_connect();
    struct xwindow *window = xwindow_create(&(struct xwindow_opts){
        .color = C_PURPLE,
        .width = 800,
        .height = 600,
    });
    xwindow_request_state(window, 1, "FULLSCREEN", NULL);
    xwindow_map(window);
    shot_free(expect_window_at(C_PURPLE, 0, 0, OUTPUT_WIDTH, OUTPUT_HEIGHT));

    xwindow_set_size_hints(window, 800, 600, 800, 600);
    xwindow_request_state(window, 0, "FULLSCREEN", NULL);
    struct box want = centered(800, 600, true);
    struct shot *shot = expect_window_at(C_PURPLE, want.x, want.y, 800, 600);
    expect_frame(shot, C_PURPLE, true);
    shot_free(shot);
}

TEST(x11_not_resizable_after_fullscreen_at_map_floats, .x11 = true) {
    x11_connect();
    struct xwindow *window = xwindow_create(&(struct xwindow_opts){
        .color = C_PURPLE,
        .width = 800,
        .height = 600,
    });
    xwindow_request_state(window, 1, "FULLSCREEN", NULL);
    xwindow_map(window);
    shot_free(expect_window_at(C_PURPLE, 0, 0, OUTPUT_WIDTH, OUTPUT_HEIGHT));

    xwindow_set_motif_no_resize(window, true);
    xwindow_request_state(window, 0, "FULLSCREEN", NULL);
    struct box want = centered(800, 600, true);
    struct shot *shot = expect_window_at(C_PURPLE, want.x, want.y, 800, 600);
    expect_frame(shot, C_PURPLE, true);
    shot_free(shot);
}

/* mpv --fs, then leaving fullscreen. */
TEST(x11_fixed_aspect_after_fullscreen_at_map_floats, .x11 = true) {
    x11_connect();
    struct xwindow *window = xwindow_create(&(struct xwindow_opts){
        .color = C_TEAL,
        .width = 640,
        .height = 360,
        .min_aspect_x = 16,
        .min_aspect_y = 9,
        .max_aspect_x = 16,
        .max_aspect_y = 9,
    });
    xwindow_request_state(window, 1, "FULLSCREEN", NULL);
    xwindow_map(window);
    shot_free(expect_window_at(C_TEAL, 0, 0, OUTPUT_WIDTH, OUTPUT_HEIGHT));

    xwindow_request_state(window, 0, "FULLSCREEN", NULL);
    struct box want = centered(640, 360, true);
    struct shot *shot = expect_window_at(C_TEAL, want.x, want.y, 640, 360);
    expect_frame(shot, C_TEAL, true);
    shot_free(shot);
}

TEST(x11_maximize_and_restore_while_floating, .x11 = true) {
    x11_connect();
    struct xwindow *window = map_xwindow((struct xwindow_opts){
        .color = C_PURPLE,
        .width = 400,
        .height = 300,
        .min_width = 400,
        .min_height = 300,
        .max_width = 400,
        .max_height = 300,
    });
    struct box floating = centered(400, 300, true);
    shot_free(expect_window_at(C_PURPLE, floating.x, floating.y, 400, 300));

    xwindow_request_state(window, 1, "MAXIMIZED_VERT", "MAXIMIZED_HORZ");
    int width = TILE_W - 2 * FRAME_BORDER;
    int height = TILE_H - FRAME_TOP - FRAME_BORDER;
    struct shot *shot = expect_window_at(C_PURPLE, 5 + FRAME_BORDER,
                                         5 + FRAME_TOP, width, height);
    expect_frame(shot, C_PURPLE, true);
    shot_free(shot);
    CHECK(xwindow_has_net_state(window, "MAXIMIZED_VERT"),
          "the window should be reported as maximized");

    xwindow_request_state(window, 0, "MAXIMIZED_VERT", "MAXIMIZED_HORZ");
    shot = expect_window_at(C_PURPLE, floating.x, floating.y, 400, 300);
    expect_frame(shot, C_PURPLE, true);
    shot_free(shot);
    CHECK(!xwindow_has_net_state(window, "MAXIMIZED_VERT"),
          "the window should not be reported as maximized any more");
}

TEST(x11_unmaximize_while_tiled_is_refused, .x11 = true) {
    x11_connect();
    struct xwindow *window = map_xwindow((struct xwindow_opts){.color = C_RED});
    shot_free(expect_window_at(C_RED, 5, 5, TILE_W, TILE_H));
    xwindow_request_state(window, 0, "MAXIMIZED_VERT", "MAXIMIZED_HORZ");
    shot_free(expect_window_at(C_RED, 5, 5, TILE_W, TILE_H));
    CHECK(WAIT_UNTIL(xwindow_has_net_state(window, "MAXIMIZED_VERT"), 1000),
          "a tiled window should still be reported as maximized");
}

TEST(x11_configure_request_while_tiled_is_refused, .x11 = true) {
    x11_connect();
    struct xwindow *window = map_xwindow((struct xwindow_opts){.color = C_RED});
    shot_free(expect_window_at(C_RED, 5, 5, TILE_W, TILE_H));
    xwindow_configure(window, 100, 100, 300, 200);
    shot_free(expect_window_at(C_RED, 5, 5, TILE_W, TILE_H));
    CHECK_X11_GEOMETRY(window, 5, 5, TILE_W, TILE_H);
}

TEST(x11_configure_request_while_floating_recenters, .x11 = true) {
    x11_connect();
    struct xwindow *window = map_xwindow((struct xwindow_opts){
        .color = C_PURPLE,
        .width = 400,
        .height = 300,
        .min_width = 400,
        .min_height = 300,
        .max_width = 400,
        .max_height = 300,
    });
    struct box floating = centered(400, 300, true);
    shot_free(expect_window_at(C_PURPLE, floating.x, floating.y, 400, 300));
    xwindow_configure(window, 50, 60, 500, 350);
    struct box want = centered(500, 350, true);
    struct shot *shot = expect_window_at(C_PURPLE, want.x, want.y, 500, 350);
    expect_frame(shot, C_PURPLE, true);
    shot_free(shot);
    CHECK_X11_GEOMETRY(window, want.x, want.y, 500, 350);
}

TEST(x11_override_redirect_goes_where_it_asks, .x11 = true) {
    x11_connect();
    struct xwindow *main_window = map_xwindow((struct xwindow_opts){.color = C_RED});
    shot_free(expect_window_at(C_RED, 5, 5, TILE_W, TILE_H));
    map_xwindow((struct xwindow_opts){
        .color = C_TEAL,
        .x = 100,
        .y = 200,
        .width = 300,
        .height = 150,
        .override_redirect = true,
    });
    struct shot *shot = expect_window_at(C_TEAL, 100, 200, 300, 150);
    expect_no_frame(shot, C_TEAL);
    shot_free(shot);
    CHECK(x11_active_window() == main_window->id,
          "a popup should not take the focus");
}

TEST(x11_override_redirect_above_fullscreen, .x11 = true) {
    x11_connect();
    struct xwindow *window = map_xwindow((struct xwindow_opts){.color = C_RED});
    shot_free(expect_window_at(C_RED, 5, 5, TILE_W, TILE_H));
    xwindow_request_state(window, 1, "FULLSCREEN", NULL);
    shot_free(expect_window_at(C_RED, 0, 0, OUTPUT_WIDTH, OUTPUT_HEIGHT));
    map_xwindow((struct xwindow_opts){
        .color = C_TEAL,
        .x = 600,
        .y = 400,
        .width = 200,
        .height = 100,
        .override_redirect = true,
    });
    shot_free(expect_window_at(C_TEAL, 600, 400, 200, 100));
}

TEST(x11_override_redirect_follows_moves, .x11 = true) {
    x11_connect();
    struct xwindow *popup = map_xwindow((struct xwindow_opts){
        .color = C_TEAL,
        .x = 100,
        .y = 100,
        .width = 200,
        .height = 100,
        .override_redirect = true,
    });
    shot_free(expect_window_at(C_TEAL, 100, 100, 200, 100));
    xwindow_configure(popup, 700, 450, 250, 120);
    shot_free(expect_window_at(C_TEAL, 700, 450, 250, 120));
}

TEST(x11_unmap_leaves_nothing, .x11 = true) {
    x11_connect();
    struct xwindow *window = map_xwindow((struct xwindow_opts){.color = C_RED});
    shot_free(expect_window_at(C_RED, 5, 5, TILE_W, TILE_H));
    xwindow_unmap(window);
    expect_gone(C_RED);
    struct shot *shot = screen_settle();
    CHECK(shot_is_black(shot), "nothing should be left on screen (%s)",
          shot_dump(shot, "leftovers"));
    shot_free(shot);
    CHECK(WAIT_UNTIL(xwindow_wm_state(window) == WITHDRAWN_STATE, 2000),
          "WM_STATE should become WithdrawnState, got %d",
          xwindow_wm_state(window));
    CHECK(WAIT_UNTIL(!x11_client_list_contains(window), 2000),
          "_NET_CLIENT_LIST should not list an unmapped window");
}

TEST(x11_withdraw_leaves_nothing, .x11 = true) {
    x11_connect();
    struct xwindow *window = map_xwindow((struct xwindow_opts){
        .color = C_PURPLE,
        .width = 400,
        .height = 300,
        .min_width = 400,
        .min_height = 300,
        .max_width = 400,
        .max_height = 300,
    });
    struct box floating = centered(400, 300, true);
    shot_free(expect_window_at(C_PURPLE, floating.x, floating.y, 400, 300));
    xwindow_withdraw(window);
    expect_gone(C_PURPLE);
    struct shot *shot = screen_settle();
    CHECK(shot_is_black(shot), "nothing should be left on screen (%s)",
          shot_dump(shot, "leftovers"));
    shot_free(shot);
    CHECK(WAIT_UNTIL(xwindow_wm_state(window) == WITHDRAWN_STATE, 2000),
          "WM_STATE should become WithdrawnState, got %d",
          xwindow_wm_state(window));
}

TEST(x11_destroy_leaves_nothing, .x11 = true) {
    x11_connect();
    struct xwindow *window = map_xwindow((struct xwindow_opts){
        .color = C_PURPLE,
        .width = 400,
        .height = 300,
        .min_width = 400,
        .min_height = 300,
        .max_width = 400,
        .max_height = 300,
    });
    struct box floating = centered(400, 300, true);
    shot_free(expect_window_at(C_PURPLE, floating.x, floating.y, 400, 300));
    xwindow_destroy(window);
    expect_gone(C_PURPLE);
    struct shot *shot = screen_settle();
    CHECK(shot_is_black(shot), "nothing should be left on screen (%s)",
          shot_dump(shot, "leftovers"));
    shot_free(shot);
}

TEST(x11_remap_tiles_again, .x11 = true) {
    x11_connect();
    struct xwindow *window = map_xwindow((struct xwindow_opts){.color = C_RED});
    shot_free(expect_window_at(C_RED, 5, 5, TILE_W, TILE_H));
    xwindow_unmap(window);
    expect_gone(C_RED);
    xwindow_map(window);
    shot_free(expect_window_at(C_RED, 5, 5, TILE_W, TILE_H));
    CHECK(WAIT_UNTIL(x11_active_window() == window->id, 2000),
          "the window should be active again");
}

TEST(x11_wm_state_follows_quickly, .x11 = true) {
    x11_connect();
    struct xwindow *window = xwindow_create(&(struct xwindow_opts){.color = C_RED});
    int64_t start = now_ms();
    xwindow_map(window);
    CHECK(WAIT_UNTIL(xwindow_wm_state(window) == NORMAL_STATE, 2000),
          "WM_STATE did not become NormalState within the two seconds Wine "
          "waits");
    test_log("NormalState after %lld ms", (long long)(now_ms() - start));

    start = now_ms();
    xwindow_withdraw(window);
    CHECK(WAIT_UNTIL(xwindow_wm_state(window) == WITHDRAWN_STATE, 2000),
          "WM_STATE did not become WithdrawnState within the two seconds Wine "
          "waits");
    test_log("WithdrawnState after %lld ms", (long long)(now_ms() - start));
}

TEST(x11_iconify_while_unfocused, .x11 = true) {
    x11_connect();
    struct xwindow *big = map_xwindow((struct xwindow_opts){
        .color = C_BROWN,
        .width = 800,
        .height = 500,
        .min_width = 800,
        .min_height = 500,
        .max_width = 800,
        .max_height = 500,
    });
    struct xwindow *small = map_xwindow((struct xwindow_opts){
        .color = C_TEAL,
        .width = 300,
        .height = 200,
        .min_width = 300,
        .min_height = 200,
        .max_width = 300,
        .max_height = 200,
    });
    struct box small_box = centered(300, 200, true);
    shot_free(expect_window_at(C_TEAL, small_box.x, small_box.y, 300, 200));
    CHECK(x11_active_window() == small->id, "the small window should be active");

    xwindow_iconify(big);
    CHECK(WAIT_UNTIL(xwindow_wm_state(big) == ICONIC_STATE, 2000),
          "an unfocused window should be allowed to iconify, WM_STATE is %d",
          xwindow_wm_state(big));
    struct shot *shot = screen_settle();
    CHECK(shot_count(shot, C_BROWN) > 0, "the iconified window stays on screen");
    shot_free(shot);

    xwindow_request_activate(big);
    CHECK(WAIT_UNTIL(xwindow_wm_state(big) == NORMAL_STATE, 2000),
          "focusing the window should make it normal again, WM_STATE is %d",
          xwindow_wm_state(big));
    CHECK(WAIT_UNTIL(x11_active_window() == big->id, 2000),
          "the window should be active");
}

TEST(x11_iconify_while_focused_is_refused, .x11 = true) {
    x11_connect();
    struct xwindow *window = map_xwindow((struct xwindow_opts){.color = C_RED});
    shot_free(expect_window_at(C_RED, 5, 5, TILE_W, TILE_H));
    int writes = window->wm_state_writes;
    xwindow_iconify(window);
    CHECK(WAIT_UNTIL(window->wm_state_writes > writes, 2000),
          "the window manager never answered the iconify request");
    CHECK(xwindow_wm_state(window) == NORMAL_STATE,
          "the focused window should stay normal, WM_STATE is %d",
          xwindow_wm_state(window));
    shot_free(expect_window_at(C_RED, 5, 5, TILE_W, TILE_H));
}

TEST(x11_focus_moves_between_x11_and_wayland, .x11 = true) {
    x11_connect();
    struct xwindow *xwindow = map_xwindow((struct xwindow_opts){.color = C_RED});
    shot_free(expect_window_at(C_RED, 5, 5, TILE_W, TILE_H));
    CHECK(x11_active_window() == xwindow->id, "the X11 window should be active");

    struct client *client = client_connect();
    struct window *window = window_create(client, &(struct window_opts){
                                                      .color = C_BLUE,
                                                  });
    window_map(window);
    shot_free(expect_window_at(C_BLUE, 5, 5, TILE_W, TILE_H));
    CHECK(window->states & STATE_ACTIVATED, "the Wayland window should be active");
    CHECK(WAIT_UNTIL(x11_active_window() != xwindow->id, 2000),
          "the X11 window should lose the focus");

    window_destroy(window);
    shot_free(expect_window_at(C_RED, 5, 5, TILE_W, TILE_H));
    CHECK(WAIT_UNTIL(x11_active_window() == xwindow->id, 2000),
          "the X11 window should get the focus back");
}

TEST(x11_unmanaged_window_becoming_managed, .x11 = true) {
    x11_connect();
    struct xwindow *window = xwindow_create(&(struct xwindow_opts){
        .color = C_RED,
        .x = 60,
        .y = 80,
        .width = 400,
        .height = 300,
        .override_redirect = true,
    });
    xwindow_set_override_redirect(window, false);
    xwindow_map(window);
    shot_free(expect_window_at(C_RED, 5, 5, TILE_W, TILE_H));
    CHECK(WAIT_UNTIL(x11_active_window() == window->id, 2000),
          "the window should be active");
}

TEST(x11_popup_shown_then_made_managed, .x11 = true) {
    x11_connect();
    struct xwindow *window = map_xwindow((struct xwindow_opts){
        .color = C_RED,
        .x = 60,
        .y = 80,
        .width = 400,
        .height = 300,
        .override_redirect = true,
    });
    shot_free(expect_window_at(C_RED, 60, 80, 400, 300));
    xwindow_unmap(window);
    expect_gone(C_RED);
    xwindow_set_override_redirect(window, false);
    xwindow_map(window);
    shot_free(expect_window_at(C_RED, 5, 5, TILE_W, TILE_H));
    CHECK(WAIT_UNTIL(x11_active_window() == window->id, 2000),
          "the window should be active");
}

TEST(x11_managed_window_becoming_popup, .x11 = true) {
    x11_connect();
    struct xwindow *main_window = map_xwindow((struct xwindow_opts){.color = C_RED});
    shot_free(expect_window_at(C_RED, 5, 5, TILE_W, TILE_H));
    struct xwindow *popup = xwindow_create(&(struct xwindow_opts){
        .color = C_TEAL,
        .x = 300,
        .y = 200,
        .width = 250,
        .height = 120,
    });
    xwindow_set_override_redirect(popup, true);
    xwindow_map(popup);
    shot_free(expect_window_at(C_TEAL, 300, 200, 250, 120));
    CHECK(x11_active_window() == main_window->id,
          "a popup should not take the focus");
}

TEST(x11_popup_churn_leaves_nothing, .x11 = true) {
    x11_connect();
    map_xwindow((struct xwindow_opts){.color = C_RED});
    shot_free(expect_window_at(C_RED, 5, 5, TILE_W, TILE_H));

    static const uint32_t colors[] = {C_GREEN, C_BLUE, C_ORANGE, C_PURPLE,
                                      C_TEAL, C_GRAY, C_BROWN, C_PINK};
    struct xwindow *reused = xwindow_create(&(struct xwindow_opts){
        .color = C_PINK,
        .x = 900,
        .y = 100,
        .width = 200,
        .height = 300,
        .override_redirect = true,
    });
    for (int i = 0; i < 24; i++) {
        struct xwindow *popup = xwindow_create(&(struct xwindow_opts){
            .color = colors[i % 7],
            .x = 50 + (i * 37) % 700,
            .y = 50 + (i * 53) % 500,
            .width = 150 + (i * 13) % 100,
            .height = 40 + (i * 17) % 200,
            .override_redirect = true,
            .type = i % 2 ? "POPUP_MENU" : "TOOLTIP",
        });
        xwindow_map(popup);
        xwindow_map(reused);
        if (i % 3 == 0) {
            xwindow_configure(popup, 60 + i * 5, 70 + i * 3, 180, 90);
        }
        switch (i % 3) {
        case 0:
            xwindow_unmap(popup);
            xwindow_destroy(popup);
            break;
        case 1:
            xwindow_destroy(popup);
            break;
        default:
            xwindow_withdraw(popup);
            xwindow_destroy(popup);
            break;
        }
        xwindow_unmap(reused);
    }

    struct shot *shot = screen_settle();
    CHECK(shot_box_filled(shot, BOX(5, 5, TILE_W, TILE_H), C_RED),
          "only the main window should be left (%s)",
          shot_dump(shot, "leftovers"));
    shot_free(shot);
}

static bool window_on_screen(struct shot *shot, void *data) {
    struct xwindow *window = data;
    return shot_count(shot, window->opts.color) > 0;
}

static bool window_matches_map_state(struct shot *shot, void *data) {
    return window_on_screen(shot, data) == xwindow_viewable(data);
}

static void expect_screen_matches_map_state(struct xwindow **windows, int count) {
    for (int i = 0; i < count; i++) {
        struct xwindow *window = windows[i];
        bool matched;
        struct shot *shot =
            screen_wait(window_matches_map_state, window, 3000, &matched);
        if (!matched && xwindow_viewable(window) &&
            !window->opts.override_redirect) {
            shot_free(shot);
            xwindow_request_activate(window);
            shot = screen_wait(window_matches_map_state, window, 3000, &matched);
        }
        bool viewable = xwindow_viewable(window);
        CHECK(matched, "window %06x is %s in X11 but has %d pixels on screen (%s)",
              window->opts.color, viewable ? "viewable" : "unmapped",
              shot_count(shot, window->opts.color), shot_dump(shot, "map-state"));
        shot_free(shot);
    }
}

TEST(x11_map_unmap_churn_matches_x11, .x11 = true) {
    xcb_connection_t *conn = x11_connect();
    struct xwindow *windows[] = {
        xwindow_create(&(struct xwindow_opts){.color = C_BLUE}),
        xwindow_create(&(struct xwindow_opts){
            .color = C_TEAL,
            .x = 200,
            .y = 200,
            .width = 300,
            .height = 200,
            .override_redirect = true,
        }),
        xwindow_create(&(struct xwindow_opts){
            .color = C_ORANGE,
            .width = 300,
            .height = 200,
            .type = "DIALOG",
        }),
    };
    for (int i = 0; i < 40; i++) {
        for (int j = 0; j < 3; j++) {
            xcb_map_window(conn, windows[j]->id);
        }
        if (i % 4 == 0) {
            x11_flush();
            sleep_ms(1);
        }
        for (int j = 0; j < 3; j++) {
            xcb_unmap_window(conn, windows[j]->id);
        }
        x11_flush();
        if (i % 5 == 0) {
            pump(5);
        }
    }
    x11_sync();
    expect_screen_matches_map_state(windows, 3);

    for (int j = 0; j < 3; j++) {
        WAIT_UNTIL(windows[j]->mapped == xwindow_viewable(windows[j]), 2000);
        xwindow_unmap(windows[j]);
    }
    expect_gone(C_BLUE);
    expect_gone(C_TEAL);
    expect_gone(C_ORANGE);
}

TEST(x11_withdraw_right_after_map, .x11 = true,
     .xfail = "wlroots ignores a synthetic UnmapNotify: a window withdrawn "
              "before the window manager handled its MapRequest gets mapped "
              "anyway and lingers, hidden as far as the client knows") {
    xcb_connection_t *conn = x11_connect();
    struct xwindow *window = xwindow_create(&(struct xwindow_opts){.color = C_BLUE});
    xcb_map_window(conn, window->id);
    xwindow_withdraw(window);
    sleep_ms(300);
    x11_sync();
    struct shot *shot = screen_settle();
    CHECK(!xwindow_viewable(window), "the withdrawn window got mapped");
    CHECK(shot_count(shot, C_BLUE) == 0,
          "the withdrawn window is on screen (%s)", shot_dump(shot, "withdrawn"));
    CHECK(xwindow_wm_state(window) != NORMAL_STATE,
          "WM_STATE should not be NormalState");
    shot_free(shot);
}

TEST(x11_destroy_parent_before_transient, .x11 = true) {
    x11_connect();
    struct xwindow *parent = map_xwindow((struct xwindow_opts){.color = C_RED});
    struct xwindow *child = map_xwindow((struct xwindow_opts){
        .color = C_BLUE,
        .width = 400,
        .height = 300,
        .transient_for = parent,
    });
    struct box want = centered(400, 300, false);
    shot_free(expect_window_at(C_BLUE, want.x, want.y, 400, 300));
    xwindow_destroy(parent);
    expect_gone(C_RED);
    shot_free(expect_window_at(C_BLUE, want.x, want.y, 400, 300));
    xwindow_destroy(child);
    expect_gone(C_BLUE);
    struct shot *shot = screen_settle();
    CHECK(shot_is_black(shot), "nothing should be left on screen (%s)",
          shot_dump(shot, "leftovers"));
    shot_free(shot);
}

TEST(x11_reparent_away_and_back, .x11 = true) {
    x11_connect();
    struct xwindow *host = map_xwindow((struct xwindow_opts){.color = C_RED});
    shot_free(expect_window_at(C_RED, 5, 5, TILE_W, TILE_H));
    struct xwindow *guest = map_xwindow((struct xwindow_opts){
        .color = C_TEAL,
        .width = 300,
        .height = 200,
        .type = "DIALOG",
    });
    struct box want = centered(300, 200, false);
    shot_free(expect_window_at(C_TEAL, want.x, want.y, 300, 200));

    xwindow_reparent(guest, host, 10, 10);
    shot_free(expect_window_at(C_TEAL, 15, 15, 300, 200));

    xwindow_reparent(guest, NULL, 0, 0);
    xwindow_map(guest);
    shot_free(expect_window_at(C_TEAL, want.x, want.y, 300, 200));

    xwindow_destroy(guest);
    xwindow_destroy(host);
    struct shot *shot = screen_settle();
    CHECK(shot_is_black(shot), "nothing should be left on screen (%s)",
          shot_dump(shot, "leftovers"));
    shot_free(shot);
}

TEST(x11_embedded_window_destroyed_with_host, .x11 = true) {
    x11_connect();
    struct xwindow *host = map_xwindow((struct xwindow_opts){.color = C_RED});
    struct xwindow *guest = map_xwindow((struct xwindow_opts){
        .color = C_TEAL,
        .width = 300,
        .height = 200,
        .type = "DIALOG",
    });
    struct box want = centered(300, 200, false);
    shot_free(expect_window_at(C_TEAL, want.x, want.y, 300, 200));
    xwindow_reparent(guest, host, 10, 10);
    shot_free(expect_window_at(C_TEAL, 15, 15, 300, 200));
    xwindow_destroy(host);
    expect_gone(C_RED);
    expect_gone(C_TEAL);

    map_xwindow((struct xwindow_opts){.color = C_BLUE});
    shot_free(expect_window_at(C_BLUE, 5, 5, TILE_W, TILE_H));
}

TEST(stress_x11_window_drawn_once_appears, .x11 = true, .stress = true,
     .timeout = 300) {
    x11_connect();
    int lost = 0, tries = 60;
    for (int i = 0; i < tries; i++) {
        struct xwindow *window = xwindow_create(&(struct xwindow_opts){
            .color = C_PURPLE,
            .width = 300,
            .height = 200,
            .type = "DIALOG",
            .static_content = true,
        });
        xwindow_map(window);
        struct box want = centered(300, 200, false);
        bool seen = false;
        int64_t deadline = now_ms() + 2000;
        while (!seen && now_ms() < deadline) {
            struct shot *shot = screen_capture();
            seen = shot_box_filled(shot, BOX(want.x, want.y, 300, 200), C_PURPLE);
            shot_free(shot);
            pump(20);
        }
        if (!seen && xwindow_viewable(window)) {
            lost++;
        }
        xwindow_destroy(window);
        expect_gone(C_PURPLE);
    }
    CHECK(lost == 0,
          "%d of %d viewable windows that drew only once never showed up: "
          "wlroots does not map a surface whose buffer was committed before "
          "Xwayland's WL_SURFACE_SERIAL message arrived",
          lost, tries);
}

TEST(stress_x11_initial_fullscreen_kept, .x11 = true, .stress = true,
     .timeout = 300) {
    x11_connect();
    int lost = 0, tries = 40;
    for (int i = 0; i < tries; i++) {
        struct xwindow *window = xwindow_create(&(struct xwindow_opts){
            .color = C_ORANGE,
            .width = 640,
            .height = 480,
            .fullscreen = true,
        });
        xwindow_map(window);
        WAIT_UNTIL(window->width == OUTPUT_WIDTH || window->width == OUTPUT_WIDTH - 10,
                   2000);
        if (!xwindow_has_net_state(window, "FULLSCREEN")) {
            lost++;
            test_log("attempt %d: %d,%d %dx%d without FULLSCREEN", i, window->x,
                     window->y, window->width, window->height);
        }
        xwindow_destroy(window);
        expect_gone(C_ORANGE);
    }
    CHECK(lost == 0, "%d of %d windows lost the fullscreen state they asked for "
                     "before mapping: " LOST_INITIAL_STATE,
          lost, tries);
}
