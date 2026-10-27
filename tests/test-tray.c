#include <signal.h>
#include <stdlib.h>

#include "poison-test.h"

#define ICON_SIZE 24
#define TRAY_SIZE (ICON_SIZE + 8)
#define TRAY_MARGIN 5
#define ICON_X (OUTPUT_WIDTH - TRAY_MARGIN - TRAY_SIZE + 4)
#define ICON_Y (OUTPUT_HEIGHT - TRAY_MARGIN - TRAY_SIZE + 4)

#define SYSTEM_TRAY_REQUEST_DOCK 0
#define XEMBED_MAPPED 1

static xcb_connection_t *conn;

static xcb_window_t tray_owner(void) {
    xcb_get_selection_owner_reply_t *reply = xcb_get_selection_owner_reply(
        conn, xcb_get_selection_owner(conn, x11_atom("_NET_SYSTEM_TRAY_S0")),
        NULL);
    xcb_window_t owner = reply ? reply->owner : XCB_NONE;
    free(reply);
    return owner;
}

static xcb_window_t wait_for_tray(void) {
    REQUIRE(WAIT_UNTIL(tray_owner() != XCB_NONE, 5000),
            "nobody became the system tray; is poison-tray built?");
    return tray_owner();
}

static pid_t tray_pid(xcb_window_t tray) {
    xcb_get_property_reply_t *reply = xcb_get_property_reply(
        conn,
        xcb_get_property(conn, 0, tray, x11_atom("_NET_WM_PID"),
                         XCB_ATOM_CARDINAL, 0, 1),
        NULL);
    pid_t pid = 0;
    if (reply && xcb_get_property_value_length(reply) == 4) {
        pid = (pid_t) * (uint32_t *)xcb_get_property_value(reply);
    }
    free(reply);
    return pid;
}

static int window_x(xcb_window_t window) {
    xcb_get_geometry_reply_t *geometry =
        xcb_get_geometry_reply(conn, xcb_get_geometry(conn, window), NULL);
    int x = geometry ? geometry->x : 0;
    free(geometry);
    return x;
}

static xcb_window_t parent_of(xcb_window_t window) {
    xcb_query_tree_reply_t *tree =
        xcb_query_tree_reply(conn, xcb_query_tree(conn, window), NULL);
    xcb_window_t parent = tree ? tree->parent : XCB_NONE;
    free(tree);
    return parent;
}

static bool icon_mapped(struct xwindow *icon) {
    xcb_get_window_attributes_reply_t *attributes =
        xcb_get_window_attributes_reply(
            conn, xcb_get_window_attributes(conn, icon->id), NULL);
    bool mapped =
        attributes && attributes->map_state != XCB_MAP_STATE_UNMAPPED;
    free(attributes);
    return mapped;
}

static void set_xembed_mapped(struct xwindow *icon, bool mapped) {
    uint32_t info[2] = {0, mapped ? XEMBED_MAPPED : 0};
    xcb_atom_t atom = x11_atom("_XEMBED_INFO");
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, icon->id, atom, atom, 32,
                        2, info);
    x11_sync();
}

static struct xwindow *dock_icon(xcb_window_t tray, uint32_t color) {
    struct xwindow *icon = xwindow_create(&(struct xwindow_opts){
        .color = color,
        .width = 20,
        .height = 20,
    });
    set_xembed_mapped(icon, true);

    xcb_client_message_event_t event = {0};
    event.response_type = XCB_CLIENT_MESSAGE;
    event.format = 32;
    event.window = tray;
    event.type = x11_atom("_NET_SYSTEM_TRAY_OPCODE");
    event.data.data32[0] = XCB_CURRENT_TIME;
    event.data.data32[1] = SYSTEM_TRAY_REQUEST_DOCK;
    event.data.data32[2] = icon->id;
    xcb_send_event(conn, 0, tray, XCB_EVENT_MASK_NO_EVENT, (const char *)&event);
    x11_sync();

    REQUIRE(WAIT_UNTIL(parent_of(icon->id) == tray, 3000),
            "the tray did not take the icon");
    return icon;
}

TEST(tray_keeps_icons_out_of_sight, .x11 = true,
     .rc = "tray \"1\"\ntray_flash \"0\"") {
    conn = x11_connect();
    xcb_window_t tray = wait_for_tray();
    dock_icon(tray, C_GREEN);
    struct shot *shot = screen_settle();
    CHECK(shot_is_black(shot), "neither the tray nor the icon should show (%s)",
          shot_dump(shot, "tray"));
    shot_free(shot);
    CHECK(window_x(tray) >= OUTPUT_WIDTH,
          "a hidden tray should wait off screen, not at %d", window_x(tray));
}

TEST(tray_flashes_for_a_new_icon, .x11 = true,
     .rc = "tray \"1\"\ntray_flash \"2000\"") {
    conn = x11_connect();
    dock_icon(wait_for_tray(), C_GREEN);
    shot_free(expect_window_at(C_GREEN, ICON_X, ICON_Y, ICON_SIZE, ICON_SIZE));
    expect_gone(C_GREEN);
}

TEST(tray_flashes_for_an_icon_shown_again, .x11 = true,
     .rc = "tray \"1\"\ntray_flash \"2000\"") {
    conn = x11_connect();
    struct xwindow *icon = dock_icon(wait_for_tray(), C_GREEN);
    shot_free(expect_window_at(C_GREEN, ICON_X, ICON_Y, ICON_SIZE, ICON_SIZE));
    expect_gone(C_GREEN);
    set_xembed_mapped(icon, false);
    REQUIRE(WAIT_UNTIL(!icon_mapped(icon), 3000), "the tray did not hide the icon");
    set_xembed_mapped(icon, true);
    shot_free(expect_window_at(C_GREEN, ICON_X, ICON_Y, ICON_SIZE, ICON_SIZE));
}

TEST(tray_toggles_on_sigusr1, .x11 = true,
     .rc = "tray \"1\"\ntray_flash \"0\"") {
    conn = x11_connect();
    xcb_window_t tray = wait_for_tray();
    pid_t pid = tray_pid(tray);
    REQUIRE(pid > 0, "the tray does not say who it is");
    dock_icon(tray, C_GREEN);
    struct xwindow *other = dock_icon(tray, C_BLUE);

    kill(pid, SIGUSR1);
    shot_free(expect_window_at(C_GREEN, ICON_X, ICON_Y, ICON_SIZE, ICON_SIZE));
    shot_free(expect_window_at(C_BLUE, ICON_X - ICON_SIZE - 2, ICON_Y,
                               ICON_SIZE, ICON_SIZE));

    xwindow_destroy(other);
    expect_gone(C_BLUE);
    shot_free(expect_window_at(C_GREEN, ICON_X, ICON_Y, ICON_SIZE, ICON_SIZE));

    kill(pid, SIGUSR1);
    expect_gone(C_GREEN);
    struct shot *shot = screen_settle();
    CHECK(shot_is_black(shot), "the tray should be gone again (%s)",
          shot_dump(shot, "tray"));
    shot_free(shot);
}

TEST(tray_goes_with_its_last_icon, .x11 = true,
     .rc = "tray \"1\"\ntray_flash \"0\"") {
    conn = x11_connect();
    xcb_window_t tray = wait_for_tray();
    pid_t pid = tray_pid(tray);
    REQUIRE(pid > 0, "the tray does not say who it is");
    struct xwindow *icon = dock_icon(tray, C_GREEN);

    kill(pid, SIGUSR1);
    shot_free(expect_window_at(C_GREEN, ICON_X, ICON_Y, ICON_SIZE, ICON_SIZE));

    xwindow_destroy(icon);
    expect_gone(C_GREEN);
    struct shot *shot = screen_settle();
    CHECK(shot_is_black(shot), "the tray should go with its last icon (%s)",
          shot_dump(shot, "tray"));
    shot_free(shot);
}

TEST(tray_disabled, .x11 = true, .rc = "tray \"0\"") {
    conn = x11_connect();
    CHECK(!WAIT_UNTIL(tray_owner() != XCB_NONE, 1500),
          "the tray should not run when it is disabled");
}
