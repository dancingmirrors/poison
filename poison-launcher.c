/* © 2026 dancingmirrors */

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include <cairo/cairo.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pango/pangocairo.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#if defined(__linux__)
#include <sys/syscall.h>
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#endif

#if defined(__FreeBSD__) || defined(__NetBSD__)
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#endif

#define MAX_ENTRIES 512
#define MAX_NAME_LEN 256
#define MAX_EXEC_LEN 512
#define MAX_DESKTOP_IDS 1024
#define MAX_DESKTOP_ID_LEN 256
#define DESKTOP_EXT ".desktop"
#define FONT_NAME "Poison 12"
#define DEFAULT_DATA_DIRS "/usr/local/share:/usr/share"

struct desktop_entry {
    char name[MAX_NAME_LEN];
    char exec[MAX_EXEC_LEN];
};

struct app_state {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_buffer *buffer;
    struct xkb_context *xkb_context;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
    struct desktop_entry entries[MAX_ENTRIES];
    char seen_ids[MAX_DESKTOP_IDS][MAX_DESKTOP_ID_LEN];

    int seen_count;
    int entry_count;
    int selected_index;
    int first_visible;
    int width;
    int height;
    bool running;
    bool configured;
    void *shm_data;
    size_t shm_size;
    int buf_w, buf_h, buf_stride;
};

static void render(struct app_state *state);

#define LAUNCHER_LINE_HEIGHT 30
#define LAUNCHER_TOP_MARGIN 20

static void clamp_scroll(struct app_state *state) {
    int rows = (state->height - LAUNCHER_TOP_MARGIN) / LAUNCHER_LINE_HEIGHT;
    if (rows < 1) {
        rows = 1;
    }
    if (state->selected_index < state->first_visible) {
        state->first_visible = state->selected_index;
    } else if (state->selected_index >= state->first_visible + rows) {
        state->first_visible = state->selected_index - rows + 1;
    }
    if (state->first_visible < 0) {
        state->first_visible = 0;
    }
}

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                            uint32_t format, int fd, uint32_t size) {
    struct app_state *state = data;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }

    char *map_shm = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map_shm == MAP_FAILED) {
        close(fd);
        return;
    }

    struct xkb_keymap *keymap = xkb_keymap_new_from_string(
        state->xkb_context, map_shm, XKB_KEYMAP_FORMAT_TEXT_V1, 0);
    munmap(map_shm, size);
    close(fd);

    if (!keymap) {
        return;
    }

    struct xkb_state *xkb_state = xkb_state_new(keymap);
    if (!xkb_state) {
        xkb_keymap_unref(keymap);
        return;
    }

    if (state->xkb_keymap) {
        xkb_keymap_unref(state->xkb_keymap);
    }
    if (state->xkb_state) {
        xkb_state_unref(state->xkb_state);
    }

    state->xkb_keymap = keymap;
    state->xkb_state = xkb_state;
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface,
                           struct wl_array *keys) {
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
    (void)keys;
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface) {
    (void)keyboard;
    (void)serial;
    (void)surface;

    struct app_state *state = data;
    state->running = false;
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard,
                         uint32_t serial, uint32_t time, uint32_t key,
                         uint32_t state_w) {
    (void)keyboard;
    (void)serial;
    (void)time;

    struct app_state *state = data;

    if (state_w != WL_KEYBOARD_KEY_STATE_PRESSED) {
        return;
    }

    if (!state->xkb_state) {
        return;
    }

    xkb_keysym_t sym = xkb_state_key_get_one_sym(state->xkb_state, key + 8);

    if (sym == XKB_KEY_Escape) {
        state->running = false;
    } else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        if (state->selected_index >= 0 && state->selected_index < state->entry_count) {
            struct desktop_entry *entry = &state->entries[state->selected_index];

            printf("%s\n", entry->exec);
            fflush(stdout);

            state->running = false;
        }
    } else if (sym == XKB_KEY_Up) {
        if (state->selected_index > 0) {
            state->selected_index--;
            clamp_scroll(state);
            render(state);
        }
    } else if (sym == XKB_KEY_Down) {
        if (state->selected_index < state->entry_count - 1) {
            state->selected_index++;
            clamp_scroll(state);
            render(state);
        }
    }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
                               uint32_t serial, uint32_t mods_depressed,
                               uint32_t mods_latched, uint32_t mods_locked,
                               uint32_t group) {
    (void)keyboard;
    (void)serial;

    struct app_state *state = data;
    if (state->xkb_state) {
        xkb_state_update_mask(state->xkb_state, mods_depressed, mods_latched,
                              mods_locked, 0, 0, group);
    }
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
                                 int32_t rate, int32_t delay) {
    (void)data;
    (void)keyboard;
    (void)rate;
    (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    struct app_state *state = data;

    if (caps & WL_SEAT_CAPABILITY_KEYBOARD) {
        if (!state->keyboard) {
            state->keyboard = wl_seat_get_keyboard(seat);
            wl_keyboard_add_listener(state->keyboard, &keyboard_listener, state);
        }
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
    (void)data;
    (void)seat;
    (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void layer_surface_configure(void *data,
                                    struct zwlr_layer_surface_v1 *surface,
                                    uint32_t serial, uint32_t width, uint32_t height) {
    struct app_state *state = data;
    state->width = width;
    state->height = height;
    state->configured = true;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface) {
    (void)surface;
    struct app_state *state = data;
    state->running = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface, uint32_t version) {
    struct app_state *state = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        state->layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        state->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        wl_seat_add_listener(state->seat, &seat_listener, state);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static int create_shm_file(size_t size) {
    int fd = -1;

#if defined(__linux__) && defined(SYS_memfd_create)
    fd = syscall(SYS_memfd_create, "poison-launcher", MFD_CLOEXEC);
    if (fd >= 0) {
        if (ftruncate(fd, size) < 0) {
            close(fd);
            return -1;
        }
        return fd;
    }
#elif defined(__FreeBSD__) || defined(__NetBSD__)
    fd = memfd_create("poison-launcher", MFD_CLOEXEC);
    if (fd >= 0) {
        if (ftruncate(fd, size) < 0) {
            close(fd);
            return -1;
        }
        return fd;
    }
#endif

    char name[64];
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    snprintf(name, sizeof(name), "/poison-launcher-%d-%lld-%lld",
             getpid(), (long long)ts.tv_sec, (long long)ts.tv_nsec);

    fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        return -1;
    }

    shm_unlink(name);

    if (ftruncate(fd, size) < 0) {
        close(fd);
        return -1;
    }

    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static void render(struct app_state *state) {
    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, state->width);
    size_t size = stride * state->height;

    if (!state->buffer || state->shm_size < size ||
        state->buf_w != state->width || state->buf_h != state->height ||
        state->buf_stride != stride) {
        if (state->shm_data) {
            munmap(state->shm_data, state->shm_size);
            state->shm_data = NULL;
        }
        if (state->buffer) {
            wl_buffer_destroy(state->buffer);
            state->buffer = NULL;
        }

        int fd = create_shm_file(size);
        if (fd < 0) {
            fprintf(stderr, "Failed to create shm file!\n");
            return;
        }

        state->shm_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (state->shm_data == MAP_FAILED) {
            close(fd);
            state->shm_data = NULL;
            fprintf(stderr, "Failed to mmap!\n");
            return;
        }
        state->shm_size = size;

        struct wl_shm_pool *pool = wl_shm_create_pool(state->shm, fd, size);
        state->buffer = wl_shm_pool_create_buffer(pool, 0, state->width, state->height,
                                                  stride, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);

        state->buf_w = state->width;
        state->buf_h = state->height;
        state->buf_stride = stride;
    }

    cairo_surface_t *cairo_surface = cairo_image_surface_create_for_data(
        state->shm_data, CAIRO_FORMAT_ARGB32, state->width, state->height, stride);
    cairo_t *cr = cairo_create(cairo_surface);

    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string(FONT_NAME);
    pango_font_description_set_weight(desc, PANGO_WEIGHT_NORMAL);
    pango_layout_set_font_description(layout, desc);

    int y_offset = LAUNCHER_TOP_MARGIN;
    int line_height = LAUNCHER_LINE_HEIGHT;

    for (int i = state->first_visible; i < state->entry_count; i++) {
        pango_layout_set_text(layout, state->entries[i].name, -1);

        if (i == state->selected_index) {
            PangoRectangle logical_rect;
            pango_layout_get_extents(layout, NULL, &logical_rect);
            int text_height = logical_rect.height / PANGO_SCALE;
            int text_y_offset = logical_rect.y / PANGO_SCALE;

            cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
            int box_height = line_height - 2;
            int rect_y = y_offset + text_y_offset - (box_height - text_height) / 2;
            cairo_rectangle(cr, 10, rect_y, state->width - 20, box_height);
            cairo_fill(cr);
            cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
        }

        cairo_move_to(cr, 20, y_offset);
        pango_cairo_show_layout(cr, layout);

        y_offset += line_height;

        if (y_offset > state->height) {
            break;
        }
    }

    pango_font_description_free(desc);
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(cairo_surface);

    wl_surface_attach(state->surface, state->buffer, 0, 0);
    wl_surface_damage_buffer(state->surface, 0, 0, state->width, state->height);
    wl_surface_commit(state->surface);
}

static char *trim_whitespace(char *str) {
    char *end;

    while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r') {
        str++;
    }

    if (*str == 0) {
        return str;
    }

    end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        end--;
    }

    end[1] = '\0';
    return str;
}

static void strip_exec_field_codes(char *exec) {
    char *src = exec;
    char *dst = exec;

    while (*src) {
        if (*src == '%' && *(src + 1)) {
            char code = *(src + 1);
            if (code == 'f' || code == 'F' || code == 'u' || code == 'U' ||
                code == 'd' || code == 'D' || code == 'n' || code == 'N' ||
                code == 'i' || code == 'c' || code == 'k' || code == 'v' ||
                code == 'm') {
                src += 2;
                while (*src == ' ') {
                    src++;
                }
                continue;
            } else if (code == '%') {
                *dst++ = '%';
                src += 2;
                continue;
            }
        }
        *dst++ = *src++;
    }
    *dst = '\0';

    while (dst > exec && (*(dst - 1) == ' ' || *(dst - 1) == '\t')) {
        dst--;
        *dst = '\0';
    }
}

static bool desktop_id_claimed(struct app_state *state, const char *id) {
    for (int i = 0; i < state->seen_count; i++) {
        if (strcmp(state->seen_ids[i], id) == 0) {
            return true;
        }
    }
    return false;
}

static void claim_desktop_id(struct app_state *state, const char *id) {
    if (state->seen_count >= MAX_DESKTOP_IDS) {
        return;
    }
    snprintf(state->seen_ids[state->seen_count], MAX_DESKTOP_ID_LEN, "%s", id);
    state->seen_count++;
}

static void parse_desktop_file(struct app_state *state, const char *path,
                               const char *id) {
    if (desktop_id_claimed(state, id)) {
        return;
    }

    FILE *file = fopen(path, "r");
    if (!file) {
        return;
    }

    char line[1024];
    bool in_desktop_entry = false;
    bool no_display = false;
    bool hidden = false;
    char name[MAX_NAME_LEN] = {0};
    char exec[MAX_EXEC_LEN] = {0};

    while (fgets(line, sizeof(line), file)) {
        char *trimmed = trim_whitespace(line);

        if (trimmed[0] == '[') {
            if (strcmp(trimmed, "[Desktop Entry]") == 0) {
                in_desktop_entry = true;
            } else {
                in_desktop_entry = false;
            }
            continue;
        }

        if (!in_desktop_entry) {
            continue;
        }

        if (strncmp(trimmed, "Name=", 5) == 0) {
            snprintf(name, MAX_NAME_LEN, "%s", trimmed + 5);
        } else if (strncmp(trimmed, "Exec=", 5) == 0) {
            snprintf(exec, MAX_EXEC_LEN, "%s", trimmed + 5);
        } else if (strncmp(trimmed, "NoDisplay=", 10) == 0) {
            char *value = trim_whitespace(trimmed + 10);
            if (strcmp(value, "true") == 0) {
                no_display = true;
            }
        } else if (strncmp(trimmed, "Hidden=", 7) == 0) {
            char *value = trim_whitespace(trimmed + 7);
            if (strcmp(value, "true") == 0) {
                hidden = true;
            }
        }
    }

    fclose(file);

    claim_desktop_id(state, id);

    if (!no_display && !hidden && name[0] != '\0' && exec[0] != '\0' &&
        state->entry_count < MAX_ENTRIES) {
        strip_exec_field_codes(exec);
        snprintf(state->entries[state->entry_count].name, MAX_NAME_LEN, "%s", name);
        snprintf(state->entries[state->entry_count].exec, MAX_EXEC_LEN, "%s", exec);
        state->entry_count++;
    }
}

static void scan_directory(struct app_state *state, const char *dir_path) {
    DIR *dir = opendir(dir_path);
    if (!dir) {
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG && entry->d_type != DT_LNK &&
            entry->d_type != DT_UNKNOWN) {
            continue;
        }

        size_t len = strlen(entry->d_name);
        size_t ext_len = strlen(DESKTOP_EXT);
        if (len < ext_len || strcmp(entry->d_name + len - ext_len, DESKTOP_EXT) != 0) {
            continue;
        }

        char path[2048];
        snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);

        if (entry->d_type != DT_REG) {
            struct stat st;
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
                continue;
            }
        }

        parse_desktop_file(state, path, entry->d_name);
    }

    closedir(dir);
}

static int compare_entries(const void *a, const void *b) {
    const struct desktop_entry *ea = a;
    const struct desktop_entry *eb = b;
    return strcasecmp(ea->name, eb->name);
}

#define COMPOSITOR_TIMEOUT_MS 5000

static void sync_done(void *data, struct wl_callback *cb, uint32_t serial) {
    (void)cb;
    (void)serial;
    *(bool *)data = true;
}

static const struct wl_callback_listener sync_listener = {
    .done = sync_done,
};

static int roundtrip_timeout(struct wl_display *display) {
    bool done = false;
    int fd = wl_display_get_fd(display);
    struct wl_callback *cb = wl_display_sync(display);
    if (!cb) {
        return -1;
    }
    wl_callback_add_listener(cb, &sync_listener, &done);

    while (!done) {
        if (wl_display_prepare_read(display) == -1) {
            if (wl_display_dispatch_pending(display) < 0) {
                goto fail;
            }
            continue;
        }
        if (wl_display_flush(display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(display);
            goto fail;
        }
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int ret = poll(&pfd, 1, COMPOSITOR_TIMEOUT_MS);
        if (ret <= 0) {
            wl_display_cancel_read(display);
            if (ret == 0) {
                fprintf(stderr, "Timed out waiting for compositor!\n");
            }
            goto fail;
        }
        if (wl_display_read_events(display) < 0) {
            goto fail;
        }
        if (wl_display_dispatch_pending(display) < 0) {
            goto fail;
        }
    }
    wl_callback_destroy(cb);
    return 0;
fail:
    wl_callback_destroy(cb);
    return -1;
}

static void scan_data_dir(struct app_state *state, const char *data_dir) {
    if (data_dir[0] != '/') {
        return;
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/applications", data_dir);
    scan_directory(state, path);
}

static void load_desktop_entries(struct app_state *state) {
    char data_home[512];
    const char *xdg_data_home = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");

    if (xdg_data_home && xdg_data_home[0] != '\0') {
        snprintf(data_home, sizeof(data_home), "%s", xdg_data_home);
        scan_data_dir(state, data_home);
    } else if (home) {
        snprintf(data_home, sizeof(data_home), "%s/.local/share", home);
        scan_data_dir(state, data_home);
    }

    const char *xdg_data_dirs = getenv("XDG_DATA_DIRS");
    if (!xdg_data_dirs || xdg_data_dirs[0] == '\0') {
        xdg_data_dirs = DEFAULT_DATA_DIRS;
    }

    char data_dirs[2048];
    snprintf(data_dirs, sizeof(data_dirs), "%s", xdg_data_dirs);

    char *saveptr = NULL;
    for (char *dir = strtok_r(data_dirs, ":", &saveptr); dir != NULL;
         dir = strtok_r(NULL, ":", &saveptr)) {
        scan_data_dir(state, dir);
    }

    qsort(state->entries, state->entry_count, sizeof(struct desktop_entry), compare_entries);
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    struct app_state state = {0};
    state.running = true;
    state.selected_index = 0;

    load_desktop_entries(&state);

    if (state.entry_count == 0) {
        fprintf(stderr, "No desktop entries found!\n");
        return 1;
    }

    state.display = wl_display_connect(NULL);
    if (!state.display) {
        fprintf(stderr, "Failed to connect to Wayland display!\n");
        return 1;
    }

    state.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!state.xkb_context) {
        fprintf(stderr, "Failed to create XKB context!\n");
        wl_display_disconnect(state.display);
        return 1;
    }

    state.registry = wl_display_get_registry(state.display);
    wl_registry_add_listener(state.registry, &registry_listener, &state);
    if (roundtrip_timeout(state.display) < 0) {
        fprintf(stderr, "Timed out or error during registry roundtrip!\n");
        if (state.xkb_context) {
            xkb_context_unref(state.xkb_context);
        }
        wl_display_disconnect(state.display);
        return 1;
    }

    if (!state.compositor || !state.shm || !state.layer_shell) {
        fprintf(stderr, "Missing required Wayland interfaces!\n");
        if (state.xkb_context) {
            xkb_context_unref(state.xkb_context);
        }
        wl_display_disconnect(state.display);
        return 1;
    }

    state.surface = wl_compositor_create_surface(state.compositor);
    state.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        state.layer_shell, state.surface, NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "poison-launcher");

    zwlr_layer_surface_v1_set_size(state.layer_surface, 0, 0);
    zwlr_layer_surface_v1_set_anchor(state.layer_surface,
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(state.layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(state.layer_surface,
                                                     ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    zwlr_layer_surface_v1_add_listener(state.layer_surface, &layer_surface_listener, &state);

    wl_surface_commit(state.surface);
    if (roundtrip_timeout(state.display) < 0) {
        fprintf(stderr, "Timed out or error waiting for layer surface configure!\n");
        if (state.xkb_state) {
            xkb_state_unref(state.xkb_state);
        }
        if (state.xkb_keymap) {
            xkb_keymap_unref(state.xkb_keymap);
        }
        if (state.xkb_context) {
            xkb_context_unref(state.xkb_context);
        }
        wl_display_disconnect(state.display);
        return 1;
    }

    if (!state.configured) {
        fprintf(stderr, "Layer surface not configured!\n");
        if (state.xkb_state) {
            xkb_state_unref(state.xkb_state);
        }
        if (state.xkb_keymap) {
            xkb_keymap_unref(state.xkb_keymap);
        }
        if (state.xkb_context) {
            xkb_context_unref(state.xkb_context);
        }
        wl_display_disconnect(state.display);
        return 1;
    }

    render(&state);

    int display_fd = wl_display_get_fd(state.display);
    while (state.running) {
        if (wl_display_prepare_read(state.display) == -1) {
            if (wl_display_dispatch_pending(state.display) < 0) {
                break;
            }
            continue;
        }
        int flush_ret = wl_display_flush(state.display);
        bool need_flush = (flush_ret < 0 && errno == EAGAIN);
        if (flush_ret < 0 && !need_flush) {
            wl_display_cancel_read(state.display);
            break;
        }
        struct pollfd pfd = {
            .fd = display_fd,
            .events = (short)(POLLIN | (need_flush ? POLLOUT : 0)),
        };
        int ret = poll(&pfd, 1, -1);
        if (ret < 0) {
            wl_display_cancel_read(state.display);
            break;
        }
        if (pfd.revents & POLLIN) {
            if (wl_display_read_events(state.display) < 0) {
                break;
            }
        } else {
            wl_display_cancel_read(state.display);
        }
        if (wl_display_dispatch_pending(state.display) < 0) {
            break;
        }
    }

    if (state.shm_data) {
        munmap(state.shm_data, state.shm_size);
    }
    if (state.buffer) {
        wl_buffer_destroy(state.buffer);
    }
    if (state.keyboard) {
        wl_keyboard_destroy(state.keyboard);
    }
    if (state.layer_surface) {
        zwlr_layer_surface_v1_destroy(state.layer_surface);
    }
    if (state.surface) {
        wl_surface_destroy(state.surface);
    }
    if (state.xkb_state) {
        xkb_state_unref(state.xkb_state);
    }
    if (state.xkb_keymap) {
        xkb_keymap_unref(state.xkb_keymap);
    }
    if (state.xkb_context) {
        xkb_context_unref(state.xkb_context);
    }
    wl_display_disconnect(state.display);

    return 0;
}
