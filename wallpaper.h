/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef WALLPAPER_H
#define WALLPAPER_H

#include <X11/Xlib.h>

/* Wallpaper state structure */
struct wallpaper_state {
	/* Display and screen info */
	Display *dpy;
	int screen;
	int width;
	int height;

	/* Background colors */
	char *color1;
	char *color2;
	int vertical_gradient;

	/* Main image/emblem */
	char *image_file;
	int center_x;
	int center_y;
	double scale_width_percent;
	double scale_height_percent;
	int keep_aspect;
	char *geometry;
	char *avoid;

	/* Tile image */
	char *tile_file;
	int tile_alpha;

	/* Emblem overlay */
	char *emblem_file;
	int emblem_alpha;
	int emboss;
};

/* Initialize wallpaper state with defaults */
void wallpaper_init(struct wallpaper_state *state, Display *dpy, int screen);

/* Free wallpaper state resources */
void wallpaper_free(struct wallpaper_state *state);

/* Set the root window background pixmap */
int wallpaper_apply(struct wallpaper_state *state);

#endif /* WALLPAPER_H */
