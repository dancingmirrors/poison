#include <stdlib.h>
#include <string.h>

#include "poison-test.h"

#define TILE_W (OUTPUT_WIDTH - 10)
#define TILE_H (OUTPUT_HEIGHT - 10)

static struct window *map_window(struct client *client,
                                 struct window_opts opts) {
    struct window *window = window_create(client, &opts);
    window_map(window);
    return window;
}

TEST(xdg_tiled_fills_output) {
    struct client *client = client_connect();
    struct window *window = map_window(client, (struct window_opts){
                                                   .color = C_RED,
                                               });

    CHECK(window->configured_width == TILE_W &&
              window->configured_height == TILE_H,
          "configured %dx%d, want %dx%d", window->configured_width,
          window->configured_height, TILE_W, TILE_H);
    CHECK((window->states & STATE_TILED) == STATE_TILED,
          "a tiled window should be told it is tiled on every edge");
    CHECK(window->states & STATE_MAXIMIZED,
          "a tiled window should be told it is maximized");
    CHECK(window->states & STATE_ACTIVATED, "the new window should be active");

    struct shot *shot = expect_window_at(C_RED, 5, 5, TILE_W, TILE_H);
    expect_no_frame(shot, C_RED);
    shot_free(shot);
}

TEST(xdg_tiled_padding_20, .rc = "padding \"20\"") {
    struct client *client = client_connect();
    map_window(client, (struct window_opts){.color = C_GREEN});
    shot_free(expect_window_at(C_GREEN, 20, 20, OUTPUT_WIDTH - 40,
                               OUTPUT_HEIGHT - 40));
}

TEST(xdg_tiled_padding_0_keeps_one_pixel, .rc = "padding \"0\"") {
    struct client *client = client_connect();
    map_window(client, (struct window_opts){.color = C_BLUE});
    shot_free(expect_window_at(C_BLUE, 1, 1, OUTPUT_WIDTH - 2,
                               OUTPUT_HEIGHT - 2));
}

TEST(xdg_bounds_and_capabilities) {
    struct client *client = client_connect();
    struct window *window = map_window(client, (struct window_opts){
                                                   .color = C_TEAL,
                                               });
    CHECK(window->bounds_width == TILE_W && window->bounds_height == TILE_H,
          "bounds %dx%d, want %dx%d", window->bounds_width,
          window->bounds_height, TILE_W, TILE_H);
    uint32_t maximize = 1u << 2, fullscreen = 1u << 3, minimize = 1u << 4;
    CHECK(window->wm_capabilities & maximize, "maximize should be supported");
    CHECK(window->wm_capabilities & fullscreen, "fullscreen should be supported");
    CHECK(!(window->wm_capabilities & minimize),
          "minimize is not supported, so it should not be advertised");
}

TEST(xdg_fullscreen_at_map) {
    struct client *client = client_connect();
    struct window *window = map_window(client, (struct window_opts){
                                                   .color = C_ORANGE,
                                                   .fullscreen = true,
                                               });
    CHECK(window->states & STATE_FULLSCREEN, "the window should be fullscreen");
    CHECK(!(window->states & STATE_TILED), "a fullscreen window is not tiled");
    CHECK(window->configured_width == OUTPUT_WIDTH &&
              window->configured_height == OUTPUT_HEIGHT,
          "configured %dx%d, want the whole output", window->configured_width,
          window->configured_height);
    struct shot *shot = expect_window_at(C_ORANGE, 0, 0, OUTPUT_WIDTH,
                                         OUTPUT_HEIGHT);
    expect_no_frame(shot, C_ORANGE);
    shot_free(shot);
}

TEST(xdg_fixed_size_floats_centered_with_frame) {
    struct client *client = client_connect();
    struct window *window = map_window(client, (struct window_opts){
                                                   .color = C_PURPLE,
                                                   .policy = SIZE_FIXED,
                                                   .width = 640,
                                                   .height = 360,
                                               });
    struct box want = centered(640, 360, true);
    struct shot *shot = expect_window_at(C_PURPLE, want.x, want.y, 640, 360);
    expect_frame(shot, C_PURPLE, true);
    shot_free(shot);

    CHECK(!(window->states & (STATE_TILED | STATE_MAXIMIZED)),
          "a floating window should not be told it is tiled or maximized");
    CHECK(window->configured_width == 0 && window->configured_height == 0,
          "a floating window should get to pick its size, got %dx%d",
          window->configured_width, window->configured_height);
    CHECK(window->states & STATE_ACTIVATED, "the window should be active");
}

TEST(xdg_fixed_size_client_side_decorations_no_frame) {
    struct client *client = client_connect();
    struct window *window = map_window(client, (struct window_opts){
                                                   .color = C_PURPLE,
                                                   .policy = SIZE_FIXED,
                                                   .width = 640,
                                                   .height = 360,
                                                   .decoration = DECORATION_CLIENT,
                                               });
    CHECK(window->decoration_mode == 1,
          "client-side decorations should be granted, got mode %d",
          window->decoration_mode);
    struct box want = centered(640, 360, false);
    struct shot *shot = expect_window_at(C_PURPLE, want.x, want.y, 640, 360);
    expect_no_frame(shot, C_PURPLE);
    shot_free(shot);
}

TEST(xdg_fixed_size_server_side_decorations_frame) {
    struct client *client = client_connect();
    struct window *window = map_window(client, (struct window_opts){
                                                   .color = C_PURPLE,
                                                   .policy = SIZE_FIXED,
                                                   .width = 640,
                                                   .height = 360,
                                                   .decoration = DECORATION_SERVER,
                                               });
    CHECK(window->decoration_mode == 2,
          "server-side decorations should be granted, got mode %d",
          window->decoration_mode);
    struct box want = centered(640, 360, true);
    struct shot *shot = expect_window_at(C_PURPLE, want.x, want.y, 640, 360);
    expect_frame(shot, C_PURPLE, true);
    shot_free(shot);
}

TEST(xdg_decoration_without_mode_gets_frame) {
    struct client *client = client_connect();
    struct window *window = map_window(client, (struct window_opts){
                                                   .color = C_BROWN,
                                                   .policy = SIZE_FIXED,
                                                   .width = 500,
                                                   .height = 400,
                                                   .decoration = DECORATION_UNSET,
                                               });
    CHECK(window->decoration_mode == 2, "want server-side, got mode %d",
          window->decoration_mode);
    struct box want = centered(500, 400, true);
    struct shot *shot = expect_window_at(C_BROWN, want.x, want.y, 500, 400);
    expect_frame(shot, C_BROWN, true);
    shot_free(shot);
}

TEST(xdg_client_side_request_overridden, .rc = "decoration_respect_client \"0\"") {
    struct client *client = client_connect();
    struct window *window = map_window(client, (struct window_opts){
                                                   .color = C_PURPLE,
                                                   .policy = SIZE_FIXED,
                                                   .width = 640,
                                                   .height = 360,
                                                   .decoration = DECORATION_CLIENT,
                                               });
    CHECK(window->decoration_mode == 2,
          "with decoration_respect_client 0 the answer should be server-side, "
          "got mode %d",
          window->decoration_mode);
    struct box want = centered(640, 360, true);
    struct shot *shot = expect_window_at(C_PURPLE, want.x, want.y, 640, 360);
    expect_frame(shot, C_PURPLE, true);
    shot_free(shot);
}

TEST(xdg_decorations_disabled, .rc = "decorations \"0\"") {
    struct client *client = client_connect();
    map_window(client, (struct window_opts){
                           .color = C_PURPLE,
                           .policy = SIZE_FIXED,
                           .width = 640,
                           .height = 360,
                       });
    struct box want = centered(640, 360, false);
    struct shot *shot = expect_window_at(C_PURPLE, want.x, want.y, 640, 360);
    expect_no_frame(shot, C_PURPLE);
    shot_free(shot);
}

TEST(xdg_tiled_server_side_no_frame) {
    struct client *client = client_connect();
    struct window *window = map_window(client, (struct window_opts){
                                                   .color = C_RED,
                                                   .decoration = DECORATION_SERVER,
                                               });
    CHECK(window->decoration_mode == 2, "want server-side, got mode %d",
          window->decoration_mode);
    struct shot *shot = expect_window_at(C_RED, 5, 5, TILE_W, TILE_H);
    expect_no_frame(shot, C_RED);
    shot_free(shot);
}

TEST(xdg_cell_grid_within_slack_stays_tiled) {
    struct client *client = client_connect();
    struct window *window = map_window(client, (struct window_opts){
                                                   .color = C_GRAY,
                                                   .policy = SIZE_SNAP,
                                                   .snap_width = 7,
                                                   .snap_height = 15,
                                               });
    int width = TILE_W / 7 * 7, height = TILE_H / 15 * 15;
    CHECK(window->width == width && window->height == height,
          "client drew %dx%d", window->width, window->height);
    struct shot *shot = expect_window_at(C_GRAY, 5, 5, width, height);
    expect_no_frame(shot, C_GRAY);
    shot_free(shot);
}

TEST(xdg_coarse_cell_grid_floats) {
    struct client *client = client_connect();
    struct window *window = map_window(client, (struct window_opts){
                                                   .color = C_GRAY,
                                                   .policy = SIZE_SNAP,
                                                   .snap_width = 20,
                                                   .snap_height = 20,
                                               });
    CHECK(!(window->states & STATE_TILED), "the window should float");
    int width = TILE_W / 20 * 20;
    int height = (TILE_H - FRAME_TOP - FRAME_BORDER) / 20 * 20;
    struct box want = centered(width, height, true);
    struct shot *shot = expect_window_at(C_GRAY, want.x, want.y, width, height);
    expect_frame(shot, C_GRAY, true);
    shot_free(shot);
}

TEST(xdg_aspect_narrow_centered_horizontally) {
    struct client *client = client_connect();
    struct window *window = map_window(client, (struct window_opts){
                                                   .color = C_PINK,
                                                   .policy = SIZE_ASPECT,
                                                   .width = 800,
                                                   .height = 600,
                                               });
    int width = TILE_H * 800 / 600;
    CHECK(window->width == width && window->height == TILE_H,
          "client drew %dx%d", window->width, window->height);
    struct shot *shot =
        expect_window_at(C_PINK, (OUTPUT_WIDTH - width) / 2, 5, width, TILE_H);
    expect_no_frame(shot, C_PINK);
    shot_free(shot);
}

TEST(xdg_aspect_wide_centered_vertically) {
    struct client *client = client_connect();
    struct window *window = map_window(client, (struct window_opts){
                                                   .color = C_PINK,
                                                   .policy = SIZE_ASPECT,
                                                   .width = 2400,
                                                   .height = 900,
                                               });
    int height = TILE_W * 900 / 2400;
    CHECK(window->width == TILE_W && window->height == height,
          "client drew %dx%d", window->width, window->height);
    struct shot *shot =
        expect_window_at(C_PINK, 5, (OUTPUT_HEIGHT - height) / 2, TILE_W, height);
    expect_no_frame(shot, C_PINK);
    shot_free(shot);
}

TEST(xdg_fixed_dialog_with_parent_floats_without_frame) {
    struct client *client = client_connect();
    struct window *parent = map_window(client, (struct window_opts){
                                                   .color = C_RED,
                                               });
    struct window *dialog = map_window(client, (struct window_opts){
                                                   .color = C_BLUE,
                                                   .parent = parent,
                                                   .min_width = 400,
                                                   .min_height = 300,
                                                   .max_width = 400,
                                                   .max_height = 300,
                                               });
    struct box want = centered(400, 300, false);
    struct shot *shot = expect_window_at(C_BLUE, want.x, want.y, 400, 300);
    expect_no_frame(shot, C_BLUE);
    int parent_pixels = shot_count(shot, C_RED);
    CHECK(parent_pixels == TILE_W * TILE_H - 400 * 300,
          "the parent should stay tiled behind the dialog");
    shot_free(shot);

    CHECK(dialog->states & STATE_ACTIVATED, "the dialog should be active");
    CHECK(!(parent->states & STATE_ACTIVATED), "the parent should not be active");

    window_destroy(dialog);
    shot_free(expect_window_at(C_RED, 5, 5, TILE_W, TILE_H));
    CHECK(parent->states & STATE_ACTIVATED,
          "the parent should be active again once the dialog closes");
}

TEST(xdg_resizable_child_is_tiled) {
    struct client *client = client_connect();
    struct window *parent = map_window(client, (struct window_opts){
                                                   .color = C_RED,
                                               });
    struct window *child = map_window(client, (struct window_opts){
                                                  .color = C_GREEN,
                                                  .parent = parent,
                                              });
    struct shot *shot = expect_window_at(C_GREEN, 5, 5, TILE_W, TILE_H);
    expect_no_frame(shot, C_GREEN);
    shot_free(shot);
    CHECK(child->states & STATE_ACTIVATED, "the child should be active");
}

TEST(xdg_dconf_editor_nag_stays_on_top) {
    struct client *client = client_connect();
    struct window *nag = map_window(client, (struct window_opts){
                                                .color = C_ORANGE,
                                                .policy = SIZE_FIXED,
                                                .width = 400,
                                                .height = 200,
                                            });
    struct window *main_window = map_window(client, (struct window_opts){
                                                        .color = C_TEAL,
                                                    });
    struct box want = centered(400, 200, true);
    struct shot *shot = expect_window_at(C_ORANGE, want.x, want.y, 400, 200);
    expect_frame(shot, C_ORANGE, true);
    CHECK(shot_count(shot, C_TEAL) > 0, "the main window should be mapped");
    shot_free(shot);
    CHECK(nag->states & STATE_ACTIVATED, "the nag should keep the focus");
    CHECK(!(main_window->states & STATE_ACTIVATED),
          "the main window should not take the focus from the nag");

    window_destroy(nag);
    shot_free(expect_window_at(C_TEAL, 5, 5, TILE_W, TILE_H));
    CHECK(main_window->states & STATE_ACTIVATED,
          "the main window should be active after the nag closes");
}

TEST(xdg_late_sibling_takes_focus) {
    struct client *client = client_connect();
    struct window *first = map_window(client, (struct window_opts){
                                                  .color = C_ORANGE,
                                                  .policy = SIZE_FIXED,
                                                  .width = 400,
                                                  .height = 200,
                                              });
    sleep_ms(1200);
    struct window *second = map_window(client, (struct window_opts){
                                                   .color = C_TEAL,
                                               });
    shot_free(expect_window_at(C_TEAL, 5, 5, TILE_W, TILE_H));
    CHECK(second->states & STATE_ACTIVATED, "the new window should be active");
    CHECK(!(first->states & STATE_ACTIVATED), "the old window should not be");
}

TEST(xdg_other_client_takes_focus) {
    struct client *a = client_connect();
    struct client *b = client_connect();
    struct window *first = map_window(a, (struct window_opts){
                                             .color = C_ORANGE,
                                             .policy = SIZE_FIXED,
                                             .width = 400,
                                             .height = 200,
                                         });
    struct window *second = map_window(b, (struct window_opts){
                                              .color = C_TEAL,
                                          });
    shot_free(expect_window_at(C_TEAL, 5, 5, TILE_W, TILE_H));
    CHECK(second->states & STATE_ACTIVATED, "the new window should be active");
    CHECK(!(first->states & STATE_ACTIVATED), "the old window should not be");
}

TEST(xdg_smaller_sibling_takes_focus) {
    struct client *client = client_connect();
    struct window *big = map_window(client, (struct window_opts){
                                                .color = C_TEAL,
                                            });
    struct window *small = map_window(client, (struct window_opts){
                                                  .color = C_ORANGE,
                                                  .policy = SIZE_FIXED,
                                                  .width = 400,
                                                  .height = 200,
                                              });
    struct box want = centered(400, 200, true);
    struct shot *shot = expect_window_at(C_ORANGE, want.x, want.y, 400, 200);
    expect_frame(shot, C_ORANGE, true);
    shot_free(shot);
    CHECK(small->states & STATE_ACTIVATED, "the small window should be active");
    CHECK(!(big->states & STATE_ACTIVATED), "the big one should not be");
}

TEST(xdg_maximize_request_while_tiled) {
    struct client *client = client_connect();
    struct window *window = map_window(client, (struct window_opts){
                                                   .color = C_RED,
                                               });
    window_set_maximized(window, true);
    shot_free(expect_window_at(C_RED, 5, 5, TILE_W, TILE_H));
    window_set_maximized(window, false);
    shot_free(expect_window_at(C_RED, 5, 5, TILE_W, TILE_H));
    CHECK((window->states & STATE_TILED) == STATE_TILED,
          "the window should still be tiled");
}

static struct window *map_floating_obeying(struct client *client,
                                           uint32_t color, int width,
                                           int height) {
    struct window *window = map_window(client, (struct window_opts){
                                                   .color = color,
                                                   .policy = SIZE_FIXED,
                                                   .width = width,
                                                   .height = height,
                                               });
    window->opts.policy = SIZE_OBEY;
    return window;
}

TEST(xdg_floating_maximize_and_restore) {
    struct client *client = client_connect();
    struct window *window = map_floating_obeying(client, C_PURPLE, 640, 360);
    struct box floating = centered(640, 360, true);
    shot_free(expect_window_at(C_PURPLE, floating.x, floating.y, 640, 360));

    window_set_maximized(window, true);
    CHECK(window->states & STATE_MAXIMIZED, "the window should be maximized");
    struct box maximized = BOX(5 + FRAME_BORDER, 5 + FRAME_TOP,
                               TILE_W - 2 * FRAME_BORDER,
                               TILE_H - FRAME_TOP - FRAME_BORDER);
    struct shot *shot = expect_window_at(C_PURPLE, maximized.x, maximized.y,
                                         maximized.width, maximized.height);
    expect_frame(shot, C_PURPLE, true);
    shot_free(shot);

    window_set_maximized(window, false);
    CHECK(!(window->states & STATE_MAXIMIZED), "the window should be restored");
    shot = expect_window_at(C_PURPLE, floating.x, floating.y, 640, 360);
    expect_frame(shot, C_PURPLE, true);
    shot_free(shot);
}

TEST(xdg_fullscreen_and_back_while_tiled) {
    struct client *client = client_connect();
    struct window *window = map_window(client, (struct window_opts){
                                                   .color = C_RED,
                                               });
    window_set_fullscreen(window, true);
    CHECK(window->states & STATE_FULLSCREEN, "the window should be fullscreen");
    shot_free(expect_window_at(C_RED, 0, 0, OUTPUT_WIDTH, OUTPUT_HEIGHT));

    window_set_fullscreen(window, false);
    CHECK(!(window->states & STATE_FULLSCREEN), "the window should be windowed");
    CHECK((window->states & STATE_TILED) == STATE_TILED,
          "the window should be tiled again");
    shot_free(expect_window_at(C_RED, 5, 5, TILE_W, TILE_H));
}

TEST(xdg_fullscreen_and_back_while_floating) {
    struct client *client = client_connect();
    struct window *window = map_floating_obeying(client, C_PURPLE, 640, 360);
    struct box floating = centered(640, 360, true);
    shot_free(expect_window_at(C_PURPLE, floating.x, floating.y, 640, 360));

    window_set_fullscreen(window, true);
    struct shot *shot = expect_window_at(C_PURPLE, 0, 0, OUTPUT_WIDTH,
                                         OUTPUT_HEIGHT);
    CHECK(shot_count_frame_pixels(shot) == 0,
          "a fullscreen window should not have a frame");
    shot_free(shot);

    window_set_fullscreen(window, false);
    shot = expect_window_at(C_PURPLE, floating.x, floating.y, 640, 360);
    expect_frame(shot, C_PURPLE, true);
    shot_free(shot);
}

TEST(xdg_unset_fullscreen_when_not_fullscreen) {
    struct client *client = client_connect();
    struct window *window = map_floating_obeying(client, C_PURPLE, 640, 360);
    struct box floating = centered(640, 360, true);
    shot_free(expect_window_at(C_PURPLE, floating.x, floating.y, 640, 360));
    window_set_fullscreen(window, false);
    struct shot *shot = expect_window_at(C_PURPLE, floating.x, floating.y, 640, 360);
    expect_frame(shot, C_PURPLE, true);
    shot_free(shot);
}

static bool tile_corners_red(struct shot *shot, void *data) {
    return shot_pixel(shot, 5, 5) == C_RED &&
        shot_pixel(shot, 5 + TILE_W - 1, 5) == C_RED &&
        shot_pixel(shot, 5, 5 + TILE_H - 1) == C_RED &&
        shot_pixel(shot, 5 + TILE_W - 1, 5 + TILE_H - 1) == C_RED;
}

TEST(xdg_fullscreen_smaller_surface_centered) {
    struct client *client = client_connect();
    map_window(client, (struct window_opts){.color = C_RED});
    struct window *game = map_window(client, (struct window_opts){
                                                 .color = C_ORANGE,
                                                 .policy = SIZE_FIXED,
                                                 .width = 640,
                                                 .height = 480,
                                                 .fullscreen = true,
                                             });
    struct shot *shot = expect_window_at(C_ORANGE, (OUTPUT_WIDTH - 640) / 2,
                                         (OUTPUT_HEIGHT - 480) / 2, 640, 480);
    CHECK(shot_count(shot, C_RED) == 0,
          "the tiled window should not show around the fullscreen one (%s)",
          shot_dump(shot, "fullscreen"));
    shot_free(shot);

    window_set_fullscreen(game, false);
    CHECK(WAIT_UNTIL(!(game->states & STATE_FULLSCREEN), 2000),
          "the window should leave fullscreen");
    bool matched;
    shot = screen_wait(tile_corners_red, NULL, 3000, &matched);
    CHECK(matched, "the tiled window should show again around the other one (%s)",
          shot_dump(shot, "windowed"));
    shot_free(shot);
}

TEST(xdg_shadow_tiled_geometry_fills_tile) {
    struct client *client = client_connect();
    map_window(client, (struct window_opts){
                           .color = C_GREEN,
                           .shadow = 20,
                           .decoration = DECORATION_CLIENT,
                       });
    struct shot *shot = expect_window_at(C_GREEN, 5, 5, TILE_W, TILE_H);
    CHECK(shot_count(shot, C_SHADOW) == 0,
          "the shadow of a tiled window should be clipped (%s)",
          shot_dump(shot, "shadow"));
    shot_free(shot);
}

TEST(xdg_shadow_floating_geometry_centered) {
    struct client *client = client_connect();
    map_window(client, (struct window_opts){
                           .color = C_GREEN,
                           .policy = SIZE_FIXED,
                           .width = 640,
                           .height = 360,
                           .shadow = 20,
                           .decoration = DECORATION_CLIENT,
                       });
    struct box want = centered(640, 360, false);
    struct shot *shot = expect_window_at(C_GREEN, want.x, want.y, 640, 360);
    struct box shadow = shot_find(shot, C_SHADOW, NULL);
    CHECK(shadow.x == want.x - 20 && shadow.y == want.y - 20 &&
              shadow.width == 680 && shadow.height == 400,
          "the shadow should surround the window, got %d,%d %dx%d", shadow.x,
          shadow.y, shadow.width, shadow.height);
    shot_free(shot);
}

TEST(xdg_shadow_narrow_tiled_centered) {
    struct client *client = client_connect();
    map_window(client, (struct window_opts){
                           .color = C_PINK,
                           .policy = SIZE_ASPECT,
                           .width = 800,
                           .height = 600,
                           .shadow = 20,
                           .decoration = DECORATION_CLIENT,
                       });
    int width = TILE_H * 800 / 600;
    shot_free(expect_window_at(C_PINK, (OUTPUT_WIDTH - width) / 2, 5, width,
                               TILE_H));
}

TEST(xdg_two_floating_frames_show_focus) {
    struct client *a = client_connect();
    struct client *b = client_connect();
    map_window(a, (struct window_opts){
                      .color = C_BROWN,
                      .policy = SIZE_FIXED,
                      .width = 800,
                      .height = 500,
                  });
    map_window(b, (struct window_opts){
                      .color = C_TEAL,
                      .policy = SIZE_FIXED,
                      .width = 300,
                      .height = 200,
                  });
    struct box small = centered(300, 200, true);
    struct shot *shot = expect_window_at(C_TEAL, small.x, small.y, 300, 200);
    expect_frame(shot, C_TEAL, true);
    expect_frame(shot, C_BROWN, false);
    shot_free(shot);
}

TEST(xdg_floating_clamped_to_output) {
    struct client *client = client_connect();
    struct window *window = map_window(client, (struct window_opts){
                                                   .color = C_PURPLE,
                                                   .width = 640,
                                                   .height = 360,
                                                   .max_width = 640,
                                                   .max_height = 360,
                                               });
    struct box floating = centered(640, 360, true);
    shot_free(expect_window_at(C_PURPLE, floating.x, floating.y, 640, 360));

    window_set_max_size(window, 0, 0);
    window_commit_size(window, 2000, 1000);

    int width = OUTPUT_WIDTH - 10 - 2 * FRAME_BORDER;
    int height = OUTPUT_HEIGHT - 10 - FRAME_TOP - FRAME_BORDER;
    CHECK(window->configured_width == width && window->configured_height == height,
          "the window should be asked to fit the output, got %dx%d",
          window->configured_width, window->configured_height);
    struct shot *shot = expect_window_at(C_PURPLE, 5 + FRAME_BORDER, 5 + FRAME_TOP,
                                         width, height);
    expect_frame(shot, C_PURPLE, true);
    shot_free(shot);
}

TEST(xdg_unmap_and_remap) {
    struct client *client = client_connect();
    struct window *window = map_window(client, (struct window_opts){
                                                   .color = C_RED,
                                               });
    shot_free(expect_window_at(C_RED, 5, 5, TILE_W, TILE_H));
    window_unmap(window);
    expect_gone(C_RED);
    window_remap(window);
    shot_free(expect_window_at(C_RED, 5, 5, TILE_W, TILE_H));
}

TEST(xdg_closing_leaves_nothing_behind) {
    struct client *client = client_connect();
    struct window *window = map_window(client, (struct window_opts){
                                                   .color = C_PURPLE,
                                                   .policy = SIZE_FIXED,
                                                   .width = 640,
                                                   .height = 360,
                                               });
    struct box floating = centered(640, 360, true);
    shot_free(expect_window_at(C_PURPLE, floating.x, floating.y, 640, 360));
    window_destroy(window);
    expect_gone(C_PURPLE);
    struct shot *shot = screen_settle();
    CHECK(shot_is_black(shot), "nothing should be left on screen (%s)",
          shot_dump(shot, "leftovers"));
    shot_free(shot);
}

TEST(xdg_client_crash_leaves_nothing_behind) {
    struct client *client = client_connect();
    map_window(client, (struct window_opts){.color = C_RED});
    map_window(client, (struct window_opts){
                           .color = C_PURPLE,
                           .policy = SIZE_FIXED,
                           .width = 640,
                           .height = 360,
                       });
    struct box floating = centered(640, 360, true);
    shot_free(expect_window_at(C_PURPLE, floating.x, floating.y, 640, 360));
    client_destroy(client);
    expect_gone(C_RED);
    expect_gone(C_PURPLE);
    struct shot *shot = screen_settle();
    CHECK(shot_is_black(shot), "nothing should be left on screen (%s)",
          shot_dump(shot, "leftovers"));
    shot_free(shot);
}
