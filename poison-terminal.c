/* © 2026 dancingmirrors */

#include "poison-terminal.h"

#define PROGRAM_NAME "poison-terminal"
#define FONT_FAMILY "Poison"
#define FONT_SIZE_PX 16
#define PADDING_PX 10
#define SCROLLBACK_LINES 10000
#define CHROMA 0.05

#define NOISE_MAX 0.25
#define NOISE_FPS_DEFAULT 30
#define NOISE_FPS_MAX 60
#define NOISE_TILE_PX 256
#define NOISE_TILES 8

static const char *const FONT_SEARCH_PATHS[] = {
    "poison.ttf",
    "fonts/poison.ttf",
    "/usr/local/share/fonts/poison.ttf",
    "/usr/share/fonts/poison.ttf",
    NULL};

#define CONFIG_GROUP "General"

static const GdkRGBA PALETTE[16] = {
    {0.000, 0.000, 0.000, 1.0},
    {0.620, 0.380, 0.380, 1.0},
    {0.453, 0.487, 0.453, 1.0},
    {0.621, 0.621, 0.588, 1.0},
    {0.242, 0.242, 0.416, 1.0},
    {0.316, 0.268, 0.316, 1.0},
    {0.499, 0.532, 0.532, 1.0},
    {0.667, 0.667, 0.667, 1.0},
    {0.333, 0.333, 0.333, 1.0},
    {0.501, 0.468, 0.468, 1.0},
    {0.786, 0.820, 0.786, 1.0},
    {0.954, 0.954, 0.921, 1.0},
    {0.379, 0.379, 0.412, 1.0},
    {0.547, 0.514, 0.547, 1.0},
    {0.832, 0.865, 0.865, 1.0},
    {1.000, 1.000, 1.000, 1.0},
};

static const GdkRGBA FG_COLOR = {1.0, 1.0, 1.0, 1.0};
static const GdkRGBA BG_OPAQUE = {0.0, 0.0, 0.0, 1.0};
static const GdkRGBA BG_TRANSP = {0.0, 0.0, 0.0, 0.0};

static PtConfig cfg;
static GtkWidget *main_window = NULL;
static GtkWidget *terminal_widget = NULL;
static GdkPixbuf *wallpaper_pixbuf = NULL;
static cairo_surface_t *wallpaper_surf = NULL;
static int cached_win_w = -1;
static int cached_win_h = -1;
static cairo_pattern_t *noise_tiles[NOISE_TILES];
static double noise_built_for = -1.0;
static guint noise_timer = 0;
static int noise_timer_fps = 0;
static int noise_frame = 0;
static double noise_ox = 0.0;
static double noise_oy = 0.0;

static gchar *config_path(void) {
    return g_build_filename(g_get_home_dir(), ".poison-terminal.rc", NULL);
}

void pt_config_load(PtConfig *c) {
    c->use_fake_transparency = FALSE;
    c->wallpaper_path = NULL;
    c->wallpaper_opacity = 0.50;
    c->wallpaper_scaling = PT_SCALING_FIT;
    c->wallpaper_scale_percent = 100.0;
    c->scale_from_top = FALSE;
    c->static_noise = 0.0;
    c->noise_fps = NOISE_FPS_DEFAULT;

    gchar *path = config_path();
    GKeyFile *kf = g_key_file_new();
    GError *err = NULL;

    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &err)) {
        g_clear_error(&err);
        g_key_file_free(kf);
        g_free(path);
        return;
    }

#define KF_BOOL(key, field) \
    do { \
        GError *e2 = NULL; \
        gboolean v = g_key_file_get_boolean(kf, CONFIG_GROUP, key, &e2); \
        if (!e2) \
            c->field = v; \
        g_clear_error(&e2); \
    } while (0)

#define KF_DOUBLE(key, field) \
    do { \
        GError *e2 = NULL; \
        gdouble v = g_key_file_get_double(kf, CONFIG_GROUP, key, &e2); \
        if (!e2) \
            c->field = v; \
        g_clear_error(&e2); \
    } while (0)

#define KF_INT(key, field) \
    do { \
        GError *e2 = NULL; \
        gint v = g_key_file_get_integer(kf, CONFIG_GROUP, key, &e2); \
        if (!e2) \
            c->field = v; \
        g_clear_error(&e2); \
    } while (0)

#define KF_STR(key, field) \
    do { \
        GError *e2 = NULL; \
        gchar *v = g_key_file_get_string(kf, CONFIG_GROUP, key, &e2); \
        if (!e2 && v && *v) { \
            g_free(c->field); \
            c->field = v; \
        } else \
            g_free(v); \
        g_clear_error(&e2); \
    } while (0)

    KF_BOOL("use_fake_transparency", use_fake_transparency);
    KF_STR("wallpaper_path", wallpaper_path);
    KF_DOUBLE("wallpaper_opacity", wallpaper_opacity);
    KF_INT("wallpaper_scaling", wallpaper_scaling);
    KF_DOUBLE("wallpaper_scale_percent", wallpaper_scale_percent);
    KF_BOOL("scale_from_top", scale_from_top);
    KF_DOUBLE("static_noise", static_noise);
    KF_INT("noise_fps", noise_fps);

    c->static_noise = CLAMP(c->static_noise, 0.0, NOISE_MAX);
    c->noise_fps = CLAMP(c->noise_fps, 1, NOISE_FPS_MAX);

#undef KF_BOOL
#undef KF_DOUBLE
#undef KF_INT
#undef KF_STR

    g_key_file_free(kf);
    g_free(path);
}

static void kf_set_double(GKeyFile *kf, const gchar *key, gdouble value) {
    gchar buf[G_ASCII_DTOSTR_BUF_SIZE];

    for (int decimals = 0; decimals <= 17; decimals++) {
        gchar fmt[8];
        g_snprintf(fmt, sizeof(fmt), "%%.%df", decimals);
        g_ascii_formatd(buf, sizeof(buf), fmt, value);
        if (g_ascii_strtod(buf, NULL) == value) {
            g_key_file_set_value(kf, CONFIG_GROUP, key, buf);
            return;
        }
    }

    g_ascii_dtostr(buf, sizeof(buf), value);
    g_key_file_set_value(kf, CONFIG_GROUP, key, buf);
}

void pt_config_save(const PtConfig *c) {
    gchar *path = config_path();

    GKeyFile *kf = g_key_file_new();
    g_key_file_set_boolean(kf, CONFIG_GROUP, "use_fake_transparency",
                           c->use_fake_transparency);
    if (c->wallpaper_path) {
        g_key_file_set_string(kf, CONFIG_GROUP, "wallpaper_path",
                              c->wallpaper_path);
    }
    kf_set_double(kf, "wallpaper_opacity", c->wallpaper_opacity);
    g_key_file_set_integer(kf, CONFIG_GROUP, "wallpaper_scaling",
                           (int)c->wallpaper_scaling);
    kf_set_double(kf, "wallpaper_scale_percent", c->wallpaper_scale_percent);
    g_key_file_set_boolean(kf, CONFIG_GROUP, "scale_from_top",
                           c->scale_from_top);
    kf_set_double(kf, "static_noise", c->static_noise);
    g_key_file_set_integer(kf, CONFIG_GROUP, "noise_fps", c->noise_fps);

    GError *err = NULL;
    g_key_file_save_to_file(kf, path, &err);
    if (err) {
        g_warning("Failed to save config: %s!", err->message);
        g_clear_error(&err);
    }
    g_key_file_free(kf);
    g_free(path);
}

static void load_font(void) {
    gchar *home_local = g_build_filename(g_get_home_dir(),
                                         ".local", "share",
                                         PROGRAM_NAME,
                                         "poison.ttf",
                                         NULL);

    const char *paths[G_N_ELEMENTS(FONT_SEARCH_PATHS) + 1];
    gsize n = 0;
    for (gsize i = 0; i < G_N_ELEMENTS(FONT_SEARCH_PATHS) - 1; i++) {
        paths[n++] = FONT_SEARCH_PATHS[i];
    }
    paths[n++] = home_local;
    paths[n] = NULL;

    for (gsize i = 0; i < n; i++) {
        if (!paths[i] || !g_file_test(paths[i], G_FILE_TEST_IS_REGULAR)) {
            continue;
        }
        if (FcConfigAppFontAddFile(NULL, (const FcChar8 *)paths[i])) {
            break;
        }
    }

    g_free(home_local);
}

static void suppress_bold_font(void) {
    const FcChar8 *rule = (const FcChar8 *)("<fontconfig>"
                                            "  <match target=\"pattern\">"
                                            "    <test name=\"family\" compare=\"eq\">"
                                            "      <string>Poison</string>"
                                            "    </test>"
                                            "    <test name=\"weight\" compare=\"more_eq\">"
                                            "      <const>bold</const>"
                                            "    </test>"
                                            "    <edit name=\"weight\" mode=\"assign\">"
                                            "      <const>regular</const>"
                                            "    </edit>"
                                            "  </match>"
                                            "  <match target=\"font\">"
                                            "    <test name=\"family\" compare=\"eq\">"
                                            "      <string>Poison</string>"
                                            "    </test>"
                                            "    <edit name=\"embolden\" mode=\"assign\">"
                                            "      <bool>false</bool>"
                                            "    </edit>"
                                            "  </match>"
                                            "</fontconfig>");
    FcConfigParseAndLoadFromMemory(FcConfigGetCurrent(), rule, FcFalse);
}

static void rebuild_wallpaper_surface(int win_w, int win_h) {
    if (wallpaper_surf) {
        cairo_surface_destroy(wallpaper_surf);
        wallpaper_surf = NULL;
    }
    if (!wallpaper_pixbuf) {
        return;
    }

    int pb_w = gdk_pixbuf_get_width(wallpaper_pixbuf);
    int pb_h = gdk_pixbuf_get_height(wallpaper_pixbuf);
    double percent = cfg.wallpaper_scale_percent / 100.0;

    double dst_w, dst_h, ox, oy;

    switch (cfg.wallpaper_scaling) {
    case PT_SCALING_FIT: {
        double r = fmin((double)win_w / pb_w,
                        (double)win_h / pb_h) *
            percent;
        dst_w = pb_w * r;
        dst_h = pb_h * r;
        break;
    }
    case PT_SCALING_FILL: {
        double r = fmax((double)win_w / pb_w,
                        (double)win_h / pb_h) *
            percent;
        dst_w = pb_w * r;
        dst_h = pb_h * r;
        break;
    }
    case PT_SCALING_CENTER:
        dst_w = pb_w * percent;
        dst_h = pb_h * percent;
        break;
    case PT_SCALING_STRETCH:
    default:
        dst_w = win_w * percent;
        dst_h = win_h * percent;
        break;
    }

    ox = (win_w - dst_w) / 2.0;
    oy = cfg.scale_from_top ? 0.0 : (win_h - dst_h) / 2.0;

    wallpaper_surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, win_w, win_h);
    cairo_t *cr = cairo_create(wallpaper_surf);

    cairo_translate(cr, ox, oy);
    cairo_scale(cr, dst_w / pb_w, dst_h / pb_h);
    gdk_cairo_set_source_pixbuf(cr, wallpaper_pixbuf, 0.0, 0.0);
    cairo_paint(cr);

    cairo_destroy(cr);
    cached_win_w = win_w;
    cached_win_h = win_h;
}

static void build_noise_tiles(void) {
    if (noise_built_for == cfg.static_noise) {
        return;
    }
    noise_built_for = cfg.static_noise;

    guint32 a = (guint32)(255.0 * cfg.static_noise);

    for (int i = 0; i < NOISE_TILES; i++) {
        if (noise_tiles[i]) {
            cairo_pattern_destroy(noise_tiles[i]);
        }

        cairo_surface_t *tile =
            cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                       NOISE_TILE_PX, NOISE_TILE_PX);
        guchar *data = cairo_image_surface_get_data(tile);
        int stride = cairo_image_surface_get_stride(tile);

        for (int y = 0; y < NOISE_TILE_PX; y++) {
            guint32 *row = (guint32 *)(data + y * stride);
            for (int x = 0; x < NOISE_TILE_PX; x++) {
                double t = g_random_double();
                guint32 v = (guint32)(t * t * 255.0 * cfg.static_noise);
                row[x] = (a << 24) | (v << 16) | (v << 8) | v;
            }
        }

        cairo_surface_mark_dirty(tile);
        noise_tiles[i] = cairo_pattern_create_for_surface(tile);
        cairo_pattern_set_extend(noise_tiles[i], CAIRO_EXTEND_REPEAT);
        cairo_pattern_set_filter(noise_tiles[i], CAIRO_FILTER_NEAREST);
        cairo_surface_destroy(tile);
    }
}

static void draw_noise(cairo_t *cr) {
    cairo_pattern_t *tile = noise_tiles[noise_frame];
    cairo_matrix_t m;

    cairo_matrix_init_translate(&m, noise_ox, noise_oy);
    cairo_pattern_set_matrix(tile, &m);

    cairo_set_source(cr, tile);
    cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
    cairo_paint(cr);
}

static gboolean noise_advance(gpointer data) {
    (void)data;

    noise_frame = (noise_frame + 1) % NOISE_TILES;
    noise_ox = g_random_int_range(0, NOISE_TILE_PX);
    noise_oy = g_random_int_range(0, NOISE_TILE_PX);

    gtk_widget_queue_draw(main_window);
    return G_SOURCE_CONTINUE;
}

static void noise_sync(void) {
    if (cfg.static_noise > 0.0) {
        build_noise_tiles();
    }

    gboolean want = cfg.static_noise > 0.0;

    if (noise_timer && (!want || noise_timer_fps != cfg.noise_fps)) {
        g_source_remove(noise_timer);
        noise_timer = 0;
    }

    if (want && !noise_timer) {
        noise_timer_fps = cfg.noise_fps;
        noise_timer = g_timeout_add(1000 / noise_timer_fps,
                                    noise_advance, NULL);
    }
}

static gboolean on_window_draw(GtkWidget *widget, cairo_t *cr,
                               gpointer user_data) {
    (void)user_data;
    int w = gtk_widget_get_allocated_width(widget);
    int h = gtk_widget_get_allocated_height(widget);

    if (cfg.static_noise > 0.0 && noise_tiles[0]) {
        draw_noise(cr);
    }

    cairo_set_operator(cr, CAIRO_OPERATOR_DEST_OVER);

    if (cfg.use_fake_transparency) {
        if (w != cached_win_w || h != cached_win_h) {
            rebuild_wallpaper_surface(w, h);
        }

        if (wallpaper_surf) {
            cairo_set_source_surface(cr, wallpaper_surf, 0.0, 0.0);
            cairo_paint_with_alpha(cr, cfg.wallpaper_opacity);
        }
    }

    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0);
    cairo_paint(cr);

    return FALSE;
}

static void on_child_exited(VteTerminal *vte, int status, gpointer data) {
    (void)vte;
    (void)status;
    (void)data;
    gtk_widget_destroy(main_window);
}

static void on_copy(GtkMenuItem *item, gpointer data) {
    (void)item;
    (void)data;
    vte_terminal_copy_clipboard_format(VTE_TERMINAL(terminal_widget),
                                       VTE_FORMAT_TEXT);
}

static void on_paste(GtkMenuItem *item, gpointer data) {
    (void)item;
    (void)data;
    vte_terminal_paste_clipboard(VTE_TERMINAL(terminal_widget));
}

static void on_quit(GtkMenuItem *item, gpointer data) {
    (void)item;
    (void)data;
    gtk_widget_destroy(main_window);
}

static void apply_settings(const PtConfig *new_cfg) {
    if (wallpaper_surf) {
        cairo_surface_destroy(wallpaper_surf);
        wallpaper_surf = NULL;
    }
    if (wallpaper_pixbuf) {
        g_object_unref(wallpaper_pixbuf);
        wallpaper_pixbuf = NULL;
    }
    cached_win_w = cached_win_h = -1;

    g_free(cfg.wallpaper_path);
    cfg = *new_cfg;
    cfg.wallpaper_path = g_strdup(new_cfg->wallpaper_path);

    pt_config_save(&cfg);

    if (cfg.use_fake_transparency && cfg.wallpaper_path) {
        GError *err = NULL;
        wallpaper_pixbuf = gdk_pixbuf_new_from_file(cfg.wallpaper_path, &err);
        if (err) {
            g_warning("Could not load wallpaper '%s': %s!",
                      cfg.wallpaper_path, err->message);
            g_clear_error(&err);
        }
    }

    vte_terminal_set_colors(VTE_TERMINAL(terminal_widget),
                            &FG_COLOR,
                            cfg.use_fake_transparency ? &BG_TRANSP : &BG_OPAQUE,
                            PALETTE, G_N_ELEMENTS(PALETTE));

    noise_sync();
    gtk_widget_queue_draw(main_window);
}

typedef struct {
    GtkWidget *file_btn;
    GtkWidget *sld_opacity;
    GtkWidget *cmb_scaling;
    GtkWidget *spn_pct;
    GtkWidget *chk_top;
} SettingsWidgets;

static void on_transp_toggled(GtkToggleButton *btn, gpointer data) {
    SettingsWidgets *sw = data;
    gboolean active = gtk_toggle_button_get_active(btn);
    gtk_widget_set_sensitive(sw->file_btn, active);
    gtk_widget_set_sensitive(sw->sld_opacity, active);
    gtk_widget_set_sensitive(sw->cmb_scaling, active);
    gtk_widget_set_sensitive(sw->spn_pct, active);
    gtk_widget_set_sensitive(sw->chk_top, active);
}

static void on_settings(GtkMenuItem *item, gpointer data) {
    (void)item;
    (void)data;

    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Settings",
        GTK_WINDOW(main_window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_OK", GTK_RESPONSE_OK,
        "_Cancel", GTK_RESPONSE_CANCEL,
        NULL);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    gtk_box_pack_start(GTK_BOX(content), grid, TRUE, TRUE, 0);

    int row = 0;

    GtkWidget *chk_transp = gtk_check_button_new_with_label(
        "Enable fake transparency");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk_transp),
                                 cfg.use_fake_transparency);
    gtk_grid_attach(GTK_GRID(grid), chk_transp, 0, row++, 2, 1);

    GtkWidget *lbl_wp = gtk_label_new("Wallpaper:");
    gtk_widget_set_halign(lbl_wp, GTK_ALIGN_END);
    GtkWidget *file_btn = gtk_file_chooser_button_new(
        "Choose Wallpaper", GTK_FILE_CHOOSER_ACTION_OPEN);
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Images");
    gtk_file_filter_add_pixbuf_formats(filter);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(file_btn), filter);
    if (cfg.wallpaper_path) {
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(file_btn),
                                      cfg.wallpaper_path);
    }
    gtk_widget_set_hexpand(file_btn, TRUE);
    gtk_grid_attach(GTK_GRID(grid), lbl_wp, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), file_btn, 1, row++, 1, 1);

    GtkWidget *lbl_op = gtk_label_new("Opacity:");
    gtk_widget_set_halign(lbl_op, GTK_ALIGN_END);
    GtkWidget *sld_opacity = gtk_scale_new_with_range(
        GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.05);
    gtk_range_set_value(GTK_RANGE(sld_opacity), cfg.wallpaper_opacity);
    gtk_scale_set_value_pos(GTK_SCALE(sld_opacity), GTK_POS_RIGHT);
    gtk_widget_set_hexpand(sld_opacity, TRUE);
    gtk_grid_attach(GTK_GRID(grid), lbl_op, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), sld_opacity, 1, row++, 1, 1);

    GtkWidget *lbl_sc = gtk_label_new("Scaling:");
    gtk_widget_set_halign(lbl_sc, GTK_ALIGN_END);
    GtkWidget *cmb_scaling = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cmb_scaling), "Fit");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cmb_scaling), "Fill");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cmb_scaling), "Center");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cmb_scaling), "Stretch");
    gtk_combo_box_set_active(GTK_COMBO_BOX(cmb_scaling),
                             (int)cfg.wallpaper_scaling);
    gtk_grid_attach(GTK_GRID(grid), lbl_sc, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), cmb_scaling, 1, row++, 1, 1);

    GtkWidget *lbl_pct = gtk_label_new("Scale %:");
    gtk_widget_set_halign(lbl_pct, GTK_ALIGN_END);
    GtkWidget *spn_pct = gtk_spin_button_new_with_range(1.0, 400.0, 1.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spn_pct),
                              cfg.wallpaper_scale_percent);
    gtk_grid_attach(GTK_GRID(grid), lbl_pct, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), spn_pct, 1, row++, 1, 1);

    GtkWidget *chk_top = gtk_check_button_new_with_label(
        "Anchor wallpaper to top edge");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk_top),
                                 cfg.scale_from_top);
    gtk_grid_attach(GTK_GRID(grid), chk_top, 0, row++, 2, 1);

    GtkWidget *sep_ns = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_grid_attach(GTK_GRID(grid), sep_ns, 0, row++, 2, 1);

    GtkWidget *lbl_ns = gtk_label_new("Static noise:");
    gtk_widget_set_halign(lbl_ns, GTK_ALIGN_END);
    GtkWidget *sld_noise = gtk_scale_new_with_range(
        GTK_ORIENTATION_HORIZONTAL, 0.0, NOISE_MAX, 0.01);
    gtk_range_set_value(GTK_RANGE(sld_noise), cfg.static_noise);
    gtk_scale_set_value_pos(GTK_SCALE(sld_noise), GTK_POS_RIGHT);
    gtk_widget_set_hexpand(sld_noise, TRUE);
    gtk_grid_attach(GTK_GRID(grid), lbl_ns, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), sld_noise, 1, row++, 1, 1);

    GtkWidget *lbl_fps = gtk_label_new("Noise FPS:");
    gtk_widget_set_halign(lbl_fps, GTK_ALIGN_END);
    GtkWidget *spn_fps = gtk_spin_button_new_with_range(1.0, NOISE_FPS_MAX,
                                                        1.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spn_fps), cfg.noise_fps);
    gtk_grid_attach(GTK_GRID(grid), lbl_fps, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), spn_fps, 1, row++, 1, 1);

    SettingsWidgets sw = {file_btn, sld_opacity, cmb_scaling, spn_pct, chk_top};
    gboolean transp = cfg.use_fake_transparency;
    gtk_widget_set_sensitive(file_btn, transp);
    gtk_widget_set_sensitive(sld_opacity, transp);
    gtk_widget_set_sensitive(cmb_scaling, transp);
    gtk_widget_set_sensitive(spn_pct, transp);
    gtk_widget_set_sensitive(chk_top, transp);

    g_signal_connect(chk_transp, "toggled",
                     G_CALLBACK(on_transp_toggled), &sw);

    gtk_widget_show_all(dialog);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        PtConfig new_cfg;
        new_cfg.use_fake_transparency =
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(chk_transp));
        new_cfg.wallpaper_path =
            gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(file_btn));
        new_cfg.wallpaper_opacity =
            gtk_range_get_value(GTK_RANGE(sld_opacity));
        new_cfg.wallpaper_scaling =
            (PtScaling)gtk_combo_box_get_active(GTK_COMBO_BOX(cmb_scaling));
        new_cfg.wallpaper_scale_percent =
            gtk_spin_button_get_value(GTK_SPIN_BUTTON(spn_pct));
        new_cfg.scale_from_top =
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(chk_top));
        new_cfg.static_noise = gtk_range_get_value(GTK_RANGE(sld_noise));
        new_cfg.noise_fps =
            gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spn_fps));
        apply_settings(&new_cfg);
        g_free(new_cfg.wallpaper_path);
    }

    gtk_widget_destroy(dialog);
}

static gboolean on_button_press(GtkWidget *widget, GdkEventButton *event,
                                gpointer data) {
    (void)widget;
    (void)data;
    if (event->button != 3) {
        return FALSE;
    }

    GtkWidget *menu = gtk_menu_new();
    GtkWidget *item_cp = gtk_menu_item_new_with_label("Copy");
    GtkWidget *item_ps = gtk_menu_item_new_with_label("Paste");
    GtkWidget *sep = gtk_separator_menu_item_new();
    GtkWidget *item_st = gtk_menu_item_new_with_label("Settings");
    GtkWidget *sep2 = gtk_separator_menu_item_new();
    GtkWidget *item_qt = gtk_menu_item_new_with_label("Quit");

    g_signal_connect(item_cp, "activate", G_CALLBACK(on_copy), NULL);
    g_signal_connect(item_ps, "activate", G_CALLBACK(on_paste), NULL);
    g_signal_connect(item_st, "activate", G_CALLBACK(on_settings), NULL);
    g_signal_connect(item_qt, "activate", G_CALLBACK(on_quit), NULL);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_cp);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_ps);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_st);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep2);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_qt);
    gtk_widget_show_all(menu);

    g_signal_connect(menu, "selection-done",
                     G_CALLBACK(gtk_widget_destroy), NULL);

    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
    return TRUE;
}

static gboolean on_key_press(GtkWidget *widget, GdkEventKey *event,
                             gpointer data) {
    (void)widget;
    (void)data;
    if ((event->state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) == (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) {
        switch (event->keyval) {
        case GDK_KEY_C:
        case GDK_KEY_c:
            vte_terminal_copy_clipboard_format(VTE_TERMINAL(terminal_widget),
                                               VTE_FORMAT_TEXT);
            return TRUE;
        case GDK_KEY_V:
        case GDK_KEY_v:
            vte_terminal_paste_clipboard(VTE_TERMINAL(terminal_widget));
            return TRUE;
        default:
            break;
        }
    }
    return FALSE;
}

static void spawn_cb(VteTerminal *vte, GPid pid, GError *err, gpointer data) {
    (void)vte;
    (void)data;
    if (err) {
        g_warning("Failed to spawn shell: %s!", err->message);
        gtk_widget_destroy(main_window);
    } else {
        (void)pid;
    }
}

static void spawn_shell(void) {
    if (!terminal_widget) {
        return;
    }

    const gchar *shell = g_getenv("SHELL");
    if (!shell || !*shell) {
        shell = "/bin/sh";
    }

    gchar *argv[] = {(gchar *)shell, NULL};

    gchar **envp = g_get_environ();
    envp = g_environ_unsetenv(envp, "TERMCAP");
    envp = g_environ_setenv(envp, "TERMINAL", PROGRAM_NAME, TRUE);
    envp = g_environ_setenv(envp, "TERM", "screen-256color", TRUE);

    vte_terminal_spawn_async(
        VTE_TERMINAL(terminal_widget),
        VTE_PTY_DEFAULT,
        NULL,
        argv,
        envp,
        G_SPAWN_SEARCH_PATH,
        NULL, NULL,
        NULL,
        -1,
        NULL,
        spawn_cb,
        NULL);

    g_strfreev(envp);
}

static void setup_terminal(void) {
    terminal_widget = vte_terminal_new();
    VteTerminal *vte = VTE_TERMINAL(terminal_widget);

    PangoFontDescription *fd =
        pango_font_description_new();
    pango_font_description_set_family(fd, FONT_FAMILY);
    pango_font_description_set_absolute_size(fd,
                                             FONT_SIZE_PX * PANGO_SCALE);
    vte_terminal_set_font(vte, fd);
    pango_font_description_free(fd);

    vte_terminal_set_colors(vte,
                            &FG_COLOR,
                            cfg.use_fake_transparency ? &BG_TRANSP : &BG_OPAQUE,
                            PALETTE, G_N_ELEMENTS(PALETTE));

    vte_terminal_set_cursor_blink_mode(vte, VTE_CURSOR_BLINK_SYSTEM);
    vte_terminal_set_cursor_shape(vte, VTE_CURSOR_SHAPE_BLOCK);
    vte_terminal_set_scrollback_lines(vte, SCROLLBACK_LINES);
    vte_terminal_set_scroll_on_keystroke(vte, TRUE);
    vte_terminal_set_bold_is_bright(vte, TRUE);

    gtk_widget_set_margin_start(terminal_widget, PADDING_PX);
    gtk_widget_set_margin_end(terminal_widget, PADDING_PX);
    gtk_widget_set_margin_top(terminal_widget, PADDING_PX);
    gtk_widget_set_margin_bottom(terminal_widget, PADDING_PX);

    g_signal_connect(vte, "child-exited",
                     G_CALLBACK(on_child_exited), NULL);
    g_signal_connect(terminal_widget, "button-press-event",
                     G_CALLBACK(on_button_press), NULL);
    g_signal_connect(terminal_widget, "key-press-event",
                     G_CALLBACK(on_key_press), NULL);
}

static void setup_window(void) {
    main_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(main_window), PROGRAM_NAME);
    gtk_window_set_default_size(GTK_WINDOW(main_window), 1024, 768);

    g_signal_connect(main_window, "destroy",
                     G_CALLBACK(gtk_main_quit), NULL);

    GdkScreen *screen = gtk_widget_get_screen(main_window);
    GdkVisual *visual = gdk_screen_get_rgba_visual(screen);
    if (visual) {
        gtk_widget_set_visual(main_window, visual);
    }

    gtk_widget_set_app_paintable(main_window, TRUE);

    g_signal_connect_after(main_window, "draw",
                           G_CALLBACK(on_window_draw), NULL);

    gtk_container_add(GTK_CONTAINER(main_window), terminal_widget);
    gtk_widget_show_all(main_window);

    noise_sync();
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "poison-terminal 0.50\n"
            "Usage: %s [OPTIONS]\n"
            "\n"
            "Options:\n"
            "  --wallpaper PATH          Enable fake transparency\n"
            "  --opacity FLOAT           Wallpaper opacity 0.0–1.0 (default 0.50)\n"
            "  --scaling MODE            0=fit 1=fill 2=center 3=stretch (default 0)\n"
            "  --scale-percent PERCENT   Extra scale %% (default 100)\n"
            "  --scale-from-top          Anchor wallpaper to top edge\n"
            "  --static-noise FLOAT      CRT snow 0.0–0.25 (default 0.0, off)\n"
            "  --noise-fps N             Snow refresh rate 1–60 (default 20)\n"
            "  --fullscreen              Start in fullscreen\n"
            "  --maximize                Start maximized\n"
            "  --help                    Show this help message\n",
            prog);
}

int main(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            print_usage(argv[0]);
            return 0;
        }
    }

    gtk_init(&argc, &argv);

    pt_config_load(&cfg);

    gboolean do_fullscreen = FALSE;
    gboolean do_maximize = FALSE;
    gboolean config_overridden = FALSE;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
        } else if (!strcmp(argv[i], "--wallpaper") && i + 1 < argc) {
            g_free(cfg.wallpaper_path);
            cfg.wallpaper_path = g_strdup(argv[++i]);
            cfg.use_fake_transparency = TRUE;
            config_overridden = TRUE;

        } else if (!strcmp(argv[i], "--opacity") && i + 1 < argc) {
            cfg.wallpaper_opacity = g_ascii_strtod(argv[++i], NULL);
            config_overridden = TRUE;

        } else if (!strcmp(argv[i], "--scaling") && i + 1 < argc) {
            cfg.wallpaper_scaling = (PtScaling)atoi(argv[++i]);
            config_overridden = TRUE;

        } else if (!strcmp(argv[i], "--scale-percent") && i + 1 < argc) {
            cfg.wallpaper_scale_percent = g_ascii_strtod(argv[++i], NULL);
            config_overridden = TRUE;

        } else if (!strcmp(argv[i], "--scale-from-top")) {
            cfg.scale_from_top = TRUE;
            config_overridden = TRUE;

        } else if (!strcmp(argv[i], "--static-noise") && i + 1 < argc) {
            gdouble noise = g_ascii_strtod(argv[++i], NULL);
            cfg.static_noise = CLAMP(noise, 0.0, NOISE_MAX);
            config_overridden = TRUE;

        } else if (!strcmp(argv[i], "--noise-fps") && i + 1 < argc) {
            int fps = atoi(argv[++i]);
            cfg.noise_fps = CLAMP(fps, 1, NOISE_FPS_MAX);
            config_overridden = TRUE;

        } else if (!strcmp(argv[i], "--fullscreen")) {
            do_fullscreen = TRUE;
        } else if (!strcmp(argv[i], "--maximize")) {
            do_maximize = TRUE;
        }
    }

    if (cfg.use_fake_transparency && cfg.wallpaper_path) {
        GError *err = NULL;
        wallpaper_pixbuf = gdk_pixbuf_new_from_file(cfg.wallpaper_path, &err);
        if (err) {
            g_warning("Could not load wallpaper '%s': %s!",
                      cfg.wallpaper_path, err->message);
            g_clear_error(&err);
            cfg.use_fake_transparency = FALSE;
        }
    }

    if (config_overridden) {
        pt_config_save(&cfg);
    }

    load_font();
    suppress_bold_font();

    setup_terminal();
    setup_window();

    if (do_fullscreen) {
        gtk_window_fullscreen(GTK_WINDOW(main_window));
    } else if (do_maximize) {
        gtk_window_maximize(GTK_WINDOW(main_window));
    }

    spawn_shell();

    gtk_main();

    if (wallpaper_surf) {
        cairo_surface_destroy(wallpaper_surf);
    }
    if (wallpaper_pixbuf) {
        g_object_unref(wallpaper_pixbuf);
    }
    for (int i = 0; i < NOISE_TILES && noise_tiles[i]; i++) {
        cairo_pattern_destroy(noise_tiles[i]);
    }
    g_free(cfg.wallpaper_path);

    return 0;
}
