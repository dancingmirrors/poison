/* © 2026 dancingmirrors */

#ifndef POISON_TERMINAL_H
#define POISON_TERMINAL_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fontconfig/fontconfig.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <vte/vte.h>

typedef enum {
    PT_SCALING_FIT = 0,
    PT_SCALING_FILL = 1,
    PT_SCALING_CENTER = 2,
    PT_SCALING_STRETCH = 3
} PtScaling;

typedef struct {
    gboolean use_fake_transparency;
    gchar *wallpaper_path;
    gdouble wallpaper_opacity;
    PtScaling wallpaper_scaling;
    gdouble wallpaper_scale_percent;
    gboolean scale_from_top;
    gdouble static_noise;
    gint noise_fps;
} PtConfig;

void pt_config_load(PtConfig *cfg);

void pt_config_save(const PtConfig *cfg);

#endif /* POISON_TERMINAL_H */
