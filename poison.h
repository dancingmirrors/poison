/* © 2026 dancingmirrors */

#ifndef POISON_H
#define POISON_H

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_alpha_modifier_v1.h>
#include <wlr/types/wlr_color_management_v1.h>
#include <wlr/types/wlr_color_representation_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_content_type_v1.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_fixes.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_dialog_v1.h>
#include <wlr/types/wlr_xdg_foreign_registry.h>
#include <wlr/types/wlr_xdg_foreign_v1.h>
#include <wlr/types/wlr_xdg_foreign_v2.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_system_bell_v1.h>
#include <wlr/types/wlr_xdg_toplevel_tag_v1.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>
#include <wlr/xwayland.h>
#include <xkbcommon/xkbcommon.h>

#define MAX_EXEC_COMMANDS 32
#define POISON_PADDING 5
#define HSPLIT_GAP 5
#define HOT_CORNER_THRESHOLD 600
#define MAX_COMMIT_RETRIES 10
#define COMMIT_RETRY_BASE_MS 50
#define COMMIT_RETRY_MAX_MS 1000
#define GAMMA_FALLBACK_RAMP_SIZE 1024

struct poison_config {
    int padding;
    uint32_t idle_timeout_ms;
    char *terminal_command;
    char *volume_up_command;
    char *volume_down_command;
    char *mute_command;
    int hacks_enabled;
    int notifications_enabled;
    int hot_corner_enabled;
    int hot_corner_threshold;
    char *exec_commands[MAX_EXEC_COMMANDS];
    int exec_command_count;
    char *drm_devices;
    char *render_device;
    char *renderer_name;
};

void poison_config_init(struct poison_config *config);
void poison_config_load(struct poison_config *config);
void poison_config_execute_commands(struct poison_config *config);
void poison_config_cleanup(struct poison_config *config);
void poison_config_apply_filter(struct poison_config *config);

enum poison_view_type {
    POISON_VIEW_XDG,
    POISON_VIEW_XWAYLAND
};

struct poison_view_order {
    struct wl_list link; /* poison_server.view_order */
    enum poison_view_type type;
    uint32_t serial;
};

enum poison_cursor_image_type {
    POISON_CURSOR_IMAGE_NONE,
    POISON_CURSOR_IMAGE_XCURSOR,
    POISON_CURSOR_IMAGE_CLIENT
};

struct poison_cursor_image {
    enum poison_cursor_image_type type;
    union {
        const char *xcursor_name;
        struct {
            struct wlr_surface *surface;
            int32_t hotspot_x;
            int32_t hotspot_y;
        } client;
    };
};

struct poison_server {
    struct poison_config config;
    struct wl_display *wl_display;
    struct wlr_backend *backend;
    struct wlr_session *session;
    struct wl_listener session_active;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wl_listener renderer_lost;
    struct wlr_compositor *compositor;
    struct wlr_scene *scene;
    struct wlr_scene_output_layout *scene_layout;

    struct wlr_scene_tree *layer_tree_background;
    struct wlr_scene_tree *layer_tree_bottom;
    struct wlr_scene_tree *layer_tree_top;
    struct wlr_scene_tree *layer_tree_overlay;
    struct wlr_scene_tree *tree_toplevels;

    struct wlr_xdg_shell *xdg_shell;
    struct wl_listener new_xdg_toplevel;
    struct wl_list toplevels;
    struct wl_list view_order;
    uint32_t next_view_serial;

    struct wlr_xwayland *xwayland;
    struct wl_listener new_xwayland_surface;
    struct wl_list xwayland_views;
    struct wl_list xwayland_unmanaged;
    struct wl_listener xwayland_ready;

    struct wlr_xdg_decoration_manager_v1 *xdg_decoration_manager;
    struct wl_listener new_decoration;

    struct wlr_server_decoration_manager *server_decoration_manager;

    struct wlr_xdg_activation_v1 *xdg_activation_v1;
    struct wl_listener request_activate;

    struct wlr_xdg_foreign_registry *foreign_registry;
    struct wlr_xdg_foreign_v1 *foreign_v1;
    struct wlr_xdg_foreign_v2 *foreign_v2;

    struct wlr_layer_shell_v1 *layer_shell;
    struct wl_listener new_layer_surface;
    struct wl_list layer_surfaces;

    struct wlr_viewporter *viewporter;
    struct wlr_fractional_scale_manager_v1 *fractional_scale_manager;
    struct wlr_presentation *presentation;
    struct wlr_linux_drm_syncobj_manager_v1 *linux_drm_syncobj_manager;
    int syncobj_drm_fd;
    struct wlr_linux_dmabuf_v1 *linux_dmabuf;

    struct wlr_idle_inhibit_manager_v1 *idle_inhibit_manager;
    struct wl_listener new_idle_inhibitor;
    struct wl_list idle_inhibitors;

    struct wlr_keyboard_shortcuts_inhibit_manager_v1
        *keyboard_shortcuts_inhibit_manager;
    struct wl_listener new_keyboard_shortcuts_inhibitor;

    struct wlr_color_manager_v1 *color_manager;
    struct wlr_color_representation_manager_v1 *color_representation_manager;
    struct wlr_gamma_control_manager_v1 *gamma_control_manager;

    struct wlr_tearing_control_manager_v1 *tearing_control_manager;

    struct wlr_screencopy_manager_v1 *screencopy_manager;
    struct wlr_export_dmabuf_manager_v1 *export_dmabuf_manager;
    struct wlr_ext_image_copy_capture_manager_v1 *image_copy_capture_manager;
    struct wlr_ext_output_image_capture_source_manager_v1 *output_image_capture_source_manager;

    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_mgr;
    char *cursor_theme_name;
    struct poison_cursor_image cursor_image;
    struct wl_listener cursor_image_surface_destroy;
    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;
    struct wl_listener cursor_touch_down;
    struct wl_listener cursor_touch_up;
    struct wl_listener cursor_touch_motion;
    struct wl_listener cursor_touch_frame;

    struct wlr_pointer_constraints_v1 *pointer_constraints;
    struct wlr_relative_pointer_manager_v1 *relative_pointer_manager;
    struct wl_listener new_pointer_constraint;
    struct wl_list pointer_constraints_list;
    struct wlr_pointer_constraint_v1 *active_constraint;
    /* Avoid spurious edge warping, basically. */
    bool active_confine_requires_warp;

    double grab_sx, grab_sy;
    double grab_lx, grab_ly;

    bool pointer_motion_pending;
    uint32_t pointer_motion_time;
    double pointer_motion_sx;
    double pointer_motion_sy;

    bool pointer_rel_pending;
    uint32_t pointer_rel_time;
    double pointer_rel_dx, pointer_rel_dy;
    double pointer_rel_unaccel_dx, pointer_rel_unaccel_dy;

    double hot_corner_pressure;
    uint32_t hot_corner_time;
    uint32_t hot_corner_start;
    bool hot_corner_armed;

    struct wlr_seat *seat;
    struct wl_listener new_input;
    struct wl_listener request_cursor;
    struct wl_listener request_set_cursor_shape;
    struct wl_listener request_set_selection;
    struct wl_listener request_set_primary_selection;
    struct wl_listener request_start_drag;
    struct wlr_scene_tree *drag_icon;
    struct wl_listener drag_icon_destroy;
    struct wl_list keyboards;

    struct wlr_output_layout *output_layout;
    struct wlr_xdg_output_manager_v1 *xdg_output_manager;
    struct wlr_output_manager_v1 *output_manager;
    struct wl_listener output_manager_apply;
    struct wl_listener output_manager_test;
    struct wl_list outputs;
    struct wl_listener new_output;
    struct wl_listener output_layout_change;
    struct wl_event_source *arrange_idle;

    struct poison_toplevel *focused_toplevel;
    struct poison_toplevel *prev_focused_toplevel;
    struct poison_xwayland_view *focused_xwayland_view;
    struct poison_xwayland_view *prev_focused_xwayland_view;

    bool hsplit_active;
    struct wl_list hsplit_toplevels;
    struct wl_list hsplit_xwayland_views;
    int hsplit_focused_slot;
    pid_t indicator_pid;
    pid_t notify_pid;

    pid_t console_pid;
    int console_fd;
    struct wl_event_source *console_event_source;

    int window_selector_fd;
    struct wl_event_source *window_selector_event_source;
    pid_t window_selector_pid;
    char window_selector_reply[2048];
    size_t window_selector_reply_len;
    struct poison_toplevel *window_selector_prev_toplevel;
    struct poison_xwayland_view *window_selector_prev_xwayland_view;
    struct poison_toplevel *window_selector_saved_prev_toplevel;
    struct poison_xwayland_view *window_selector_saved_prev_xwayland_view;

    int launcher_fd;
    struct wl_event_source *launcher_event_source;
    pid_t launcher_pid;
    struct poison_toplevel *launcher_prev_toplevel;
    struct poison_xwayland_view *launcher_prev_xwayland_view;
    struct poison_toplevel *launcher_saved_prev_toplevel;
    struct poison_xwayland_view *launcher_saved_prev_xwayland_view;

    uint32_t idle_timeout_ms;
    struct wl_event_source *idle_timer;
    bool screens_blanked;
};

struct poison_toplevel {
    struct wl_list link;
    struct poison_view_order order;
    struct poison_server *server;
    struct wlr_xdg_toplevel *xdg_toplevel;
    struct wlr_scene_tree *scene_tree;
    struct wlr_scene_tree *surface_tree;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_listener request_fullscreen;
    struct wl_listener request_maximize;
    struct wl_listener new_popup;

    bool floating;
    bool pre_fullscreen_floating;
    bool pre_hsplit_floating;
    bool pre_hsplit_fullscreen;
    bool in_hsplit;
    bool needs_retile;
    int hsplit_order;
    struct wl_list hsplit_link;
    struct poison_toplevel *hsplit_displaced_toplevel;
    struct poison_xwayland_view *hsplit_displaced_xwayland;

    int saved_float_width;
    int saved_float_height;
    bool awaiting_float_size;
    int pre_float_width;
    int pre_float_height;

    struct timespec map_time;
};

struct poison_xwayland_view {
    struct wl_list link;
    struct poison_view_order order;
    struct poison_server *server;
    struct wlr_xwayland_surface *xwayland_surface;
    struct wlr_scene_tree *scene_tree;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener destroy;
    struct wl_listener request_configure;
    struct wl_listener request_fullscreen;
    struct wl_listener request_activate;
    struct wl_listener set_title;
    struct wl_listener associate;
    struct wl_listener dissociate;
    struct wl_listener commit;

    bool floating;
    bool pre_fullscreen_floating;
    bool pre_hsplit_floating;
    bool pre_hsplit_fullscreen;
    bool in_hsplit;
    int hsplit_order;
    struct wl_list hsplit_link;
    struct poison_toplevel *hsplit_displaced_toplevel;
    struct poison_xwayland_view *hsplit_displaced_xwayland;

    struct wlr_box clip_box;
    bool commit_listener_active;

    int saved_float_width;
    int saved_float_height;
};

struct poison_xwayland_unmanaged {
    struct wl_list link;
    struct poison_server *server;
    struct wlr_xwayland_surface *xwayland_surface;
    struct wlr_scene_tree *scene_tree;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener destroy;
    struct wl_listener request_configure;
    struct wl_listener associate;
    struct wl_listener dissociate;
};

struct poison_popup {
    struct poison_server *server;
    struct wlr_xdg_popup *xdg_popup;
    struct wlr_scene_tree *scene_tree;
    struct wlr_scene_tree *root_tree;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_listener new_popup;
    struct wl_listener reposition;
};

struct poison_decoration {
    struct wlr_xdg_toplevel_decoration_v1 *wlr_decoration;
    struct poison_server *server;
    struct wl_listener destroy;
    struct wl_listener request_mode;
};

struct poison_keyboard {
    struct wl_list link;
    struct poison_server *server;
    struct wlr_keyboard *wlr_keyboard;
    struct wl_listener modifiers;
    struct wl_listener key;
    struct wl_listener destroy;
};

struct poison_output {
    struct wl_list link;
    struct poison_server *server;
    struct wlr_output *wlr_output;
    struct wl_listener frame;
    struct wl_listener destroy;
    struct wl_listener request_state;
    unsigned int commit_failures;
    struct wl_event_source *retry_timer;
    /* Repaint on the next frame even if the scene thinks it is up to date. */
    bool force_repaint;
};

struct poison_layer_surface {
    struct wl_list link;
    struct poison_server *server;
    struct wlr_layer_surface_v1 *layer_surface;
    struct wl_listener destroy;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener new_popup;
    struct wlr_scene_layer_surface_v1 *scene_layer_surface;
    struct wlr_scene_tree *popup_tree;
};

struct poison_idle_inhibitor {
    struct wl_list link;
    struct poison_server *server;
    struct wlr_idle_inhibitor_v1 *wlr_inhibitor;
    struct wl_listener destroy;
};

struct poison_pointer_constraint {
    struct wl_list link;
    struct poison_server *server;
    struct wlr_pointer_constraint_v1 *constraint;
    struct wl_listener destroy;
};

void server_new_output(struct wl_listener *listener, void *data);
void server_session_active(struct wl_listener *listener, void *data);
void output_manager_apply(struct wl_listener *listener, void *data);
void output_manager_test(struct wl_listener *listener, void *data);
void server_new_xdg_toplevel(struct wl_listener *listener, void *data);
void server_new_decoration(struct wl_listener *listener, void *data);
void server_request_activate(struct wl_listener *listener, void *data);
void server_new_layer_surface(struct wl_listener *listener, void *data);
void server_new_idle_inhibitor(struct wl_listener *listener, void *data);
void server_new_keyboard_shortcuts_inhibitor(struct wl_listener *listener,
                                             void *data);
void server_new_input(struct wl_listener *listener, void *data);
void server_cursor_motion(struct wl_listener *listener, void *data);
void server_cursor_motion_absolute(struct wl_listener *listener,
                                   void *data);
void server_cursor_button(struct wl_listener *listener, void *data);
void server_cursor_axis(struct wl_listener *listener, void *data);
void server_cursor_frame(struct wl_listener *listener, void *data);
void poison_flush_pointer_motion(struct poison_server *server);
void server_cursor_touch_down(struct wl_listener *listener, void *data);
void server_cursor_touch_up(struct wl_listener *listener, void *data);
void server_cursor_touch_motion(struct wl_listener *listener, void *data);
void server_cursor_touch_frame(struct wl_listener *listener, void *data);
void seat_request_cursor(struct wl_listener *listener, void *data);
void seat_request_set_cursor_shape(struct wl_listener *listener, void *data);
void seat_request_set_selection(struct wl_listener *listener, void *data);
void seat_request_set_primary_selection(struct wl_listener *listener,
                                        void *data);
void seat_request_start_drag(struct wl_listener *listener, void *data);

void toplevel_map(struct wl_listener *listener, void *data);
void toplevel_unmap(struct wl_listener *listener, void *data);
void toplevel_commit(struct wl_listener *listener, void *data);
void toplevel_destroy(struct wl_listener *listener, void *data);
void toplevel_request_fullscreen(struct wl_listener *listener, void *data);
void toplevel_request_maximize(struct wl_listener *listener, void *data);
void toplevel_new_popup(struct wl_listener *listener, void *data);

void popup_commit(struct wl_listener *listener, void *data);
void popup_destroy(struct wl_listener *listener, void *data);
void popup_map(struct wl_listener *listener, void *data);
void popup_unmap(struct wl_listener *listener, void *data);
void popup_new_popup(struct wl_listener *listener, void *data);
void popup_reposition(struct wl_listener *listener, void *data);

void decoration_destroy(struct wl_listener *listener, void *data);
void decoration_request_mode(struct wl_listener *listener, void *data);

void layer_surface_destroy(struct wl_listener *listener, void *data);
void layer_surface_map(struct wl_listener *listener, void *data);
void layer_surface_unmap(struct wl_listener *listener, void *data);
void layer_surface_commit(struct wl_listener *listener, void *data);
void layer_surface_new_popup(struct wl_listener *listener, void *data);

void keyboard_handle_modifiers(struct wl_listener *listener, void *data);
void keyboard_handle_key(struct wl_listener *listener, void *data);
void keyboard_handle_destroy(struct wl_listener *listener, void *data);

void output_frame(struct wl_listener *listener, void *data);
void output_destroy(struct wl_listener *listener, void *data);
void output_request_state(struct wl_listener *listener, void *data);

void poison_arrange_all(struct poison_server *server);

void poison_device_apply_env(const struct poison_config *config);
void poison_device_clear_env(void);
void poison_device_log_topology(struct poison_server *server);
void poison_device_log_output(struct poison_server *server,
                              struct wlr_output *wlr_output);

void focus_toplevel(struct poison_toplevel *toplevel);
void focus_xwayland_view(struct poison_server *server,
                         struct poison_xwayland_view *xwayland_view);
void center_toplevel(struct poison_toplevel *toplevel);
void view_order_arrange(struct poison_server *server,
                        struct poison_view_order **wanted, int n);
void restore_toplevel_from_hsplit(struct poison_toplevel *toplevel);
bool toplevel_fullscreen_now(struct poison_toplevel *toplevel);
bool restore_toplevel_fullscreen_from_hsplit(struct poison_toplevel *toplevel);
void cycle_toplevel(struct poison_server *server, bool reverse);
void close_toplevel(struct poison_toplevel *toplevel);
void toggle_float_toplevel(struct poison_toplevel *toplevel);

struct wl_list *hsplit_evict(struct poison_toplevel *old);
struct wl_list *hsplit_evict_xwayland(struct poison_xwayland_view *old);
void hsplit_place(struct poison_toplevel *toplevel, struct wl_list *pos);
void hsplit_place_xwayland(struct poison_xwayland_view *xw, struct wl_list *pos);
void hsplit_update_after_removal(struct poison_server *server);
void hsplit_restore_focus(struct poison_server *server);
void apply_hsplit_layout(struct poison_server *server);
void toggle_hsplit(struct poison_server *server);
void hsplit_navigate(struct poison_server *server, bool next);
void hsplit_only(struct poison_server *server);
void hsplit_exchange(struct poison_server *server, bool right);

void show_split_indicator(struct poison_server *server);

void poison_diag_init(struct poison_server *server);
void poison_diag_finish(struct poison_server *server);
void poison_console_toggle(struct poison_server *server);

void server_new_xwayland_surface(struct wl_listener *listener, void *data);
void server_xwayland_ready(struct wl_listener *listener, void *data);

void xwayland_view_map(struct wl_listener *listener, void *data);
void xwayland_view_unmap(struct wl_listener *listener, void *data);
void xwayland_view_destroy(struct wl_listener *listener, void *data);
void xwayland_view_request_configure(struct wl_listener *listener,
                                     void *data);
void xwayland_view_request_fullscreen(struct wl_listener *listener,
                                      void *data);
void xwayland_view_request_activate(struct wl_listener *listener, void *data);
void xwayland_view_set_title(struct wl_listener *listener, void *data);
void xwayland_view_associate(struct wl_listener *listener, void *data);
void xwayland_view_dissociate(struct wl_listener *listener, void *data);
void xwayland_view_commit(struct wl_listener *listener, void *data);
void toggle_float_xwayland_view(struct poison_xwayland_view *xwayland_view);
void restore_xwayland_from_hsplit(struct poison_xwayland_view *xwayland_view);
bool restore_xwayland_fullscreen_from_hsplit(
    struct poison_xwayland_view *xwayland_view);

/* Clipping to handle padding gap. */
void set_xwayland_view_clip(struct poison_xwayland_view *view,
                            struct wlr_box *layout_box);

void xwayland_unmanaged_map(struct wl_listener *listener, void *data);
void xwayland_unmanaged_unmap(struct wl_listener *listener, void *data);
void xwayland_unmanaged_destroy(struct wl_listener *listener, void *data);
void xwayland_unmanaged_request_configure(struct wl_listener *listener,
                                          void *data);
void xwayland_unmanaged_associate(struct wl_listener *listener, void *data);
void xwayland_unmanaged_dissociate(struct wl_listener *listener, void *data);

void start_window_selector(struct poison_server *server);
void stop_window_selector(struct poison_server *server, bool select);
void window_selector_move_selection(struct poison_server *server, int direction);

void start_application_launcher(struct poison_server *server);

void idle_inhibitor_destroy(struct wl_listener *listener, void *data);
void reset_idle_timer(struct poison_server *server);

void server_new_pointer_constraint(struct wl_listener *listener, void *data);
void pointer_constraint_destroy(struct wl_listener *listener, void *data);

void poison_render_init(struct poison_server *server);
void poison_render_finish(struct poison_server *server);
void poison_render_output_frame(struct wl_listener *listener, void *data);
void poison_render_output_finish(struct poison_output *output);
void poison_render_request_repaint(struct poison_output *output);

#endif /* POISON_H */
