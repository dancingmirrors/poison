/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <err.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#include "stb_image.h"

#include "wallpaper.h"
#include "poison.h"

void
wallpaper_init(struct wallpaper_state *state, Display *dpy, int screen)
{
	memset(state, 0, sizeof(*state));
	state->dpy = dpy;
	state->screen = screen;
	state->width = DisplayWidth(dpy, screen);
	state->height = DisplayHeight(dpy, screen);
	state->vertical_gradient = 1;  /* vertical is default */
	state->keep_aspect = 1;  /* always keep aspect ratio */
	state->tile_alpha = 255;
	state->emblem_alpha = 255;
	state->scale_width_percent = -1;
	state->scale_height_percent = -1;
}

void
wallpaper_free(struct wallpaper_state *state)
{
	free(state->color1);
	free(state->color2);
	free(state->image_file);
	free(state->geometry);
	free(state->avoid);
	free(state->tile_file);
	free(state->emblem_file);
}

/* Parse a color string (#RRGGBB or color name) to RGB values */
static int
parse_color(Display *dpy, int screen, const char *color_str,
    unsigned char *r, unsigned char *g, unsigned char *b)
{
	XColor xcolor, exact;
	Colormap cmap = DefaultColormap(dpy, screen);

	if (!XAllocNamedColor(dpy, cmap, color_str, &xcolor, &exact))
		return 0;

	*r = xcolor.red >> 8;
	*g = xcolor.green >> 8;
	*b = xcolor.blue >> 8;

	return 1;
}

/* Create RGBA buffer with gradient */
static unsigned char *
create_gradient(struct wallpaper_state *state)
{
	unsigned char *buffer;
	int x, y;
	unsigned char r1, g1, b1, r2, g2, b2;

	if (!state->color1)
		return NULL;

	if (!parse_color(state->dpy, state->screen, state->color1,
	    &r1, &g1, &b1)) {
		warnx("invalid color: %s", state->color1);
		return NULL;
	}

	if (state->color2) {
		if (!parse_color(state->dpy, state->screen, state->color2,
		    &r2, &g2, &b2)) {
			warnx("invalid color: %s", state->color2);
			return NULL;
		}
	} else {
		r2 = r1;
		g2 = g1;
		b2 = b1;
	}

	buffer = malloc(state->width * state->height * 4);
	if (!buffer)
		return NULL;

	if (state->vertical_gradient) {
		/* Vertical gradient */
		for (y = 0; y < state->height; y++) {
			double ratio = (state->height > 1) ?
			    (double)y / (state->height - 1) : 0;
			unsigned char r = r1 + (r2 - r1) * ratio;
			unsigned char g = g1 + (g2 - g1) * ratio;
			unsigned char b = b1 + (b2 - b1) * ratio;

			for (x = 0; x < state->width; x++) {
				int idx = (y * state->width + x) * 4;
				buffer[idx + 0] = r;
				buffer[idx + 1] = g;
				buffer[idx + 2] = b;
				buffer[idx + 3] = 255;
			}
		}
	} else {
		/* Horizontal gradient */
		for (y = 0; y < state->height; y++) {
			for (x = 0; x < state->width; x++) {
				double ratio = (state->width > 1) ?
				    (double)x / (state->width - 1) : 0;
				unsigned char r = r1 + (r2 - r1) * ratio;
				unsigned char g = g1 + (g2 - g1) * ratio;
				unsigned char b = b1 + (b2 - b1) * ratio;
				int idx = (y * state->width + x) * 4;
				buffer[idx + 0] = r;
				buffer[idx + 1] = g;
				buffer[idx + 2] = b;
				buffer[idx + 3] = 255;
			}
		}
	}

	return buffer;
}

/* Simple bilinear scaling */
static unsigned char *
scale_image(unsigned char *src, int src_w, int src_h,
    int dest_w, int dest_h)
{
	unsigned char *dest;
	int x, y, c;

	dest = malloc(dest_w * dest_h * 4);
	if (!dest)
		return NULL;

	for (y = 0; y < dest_h; y++) {
		for (x = 0; x < dest_w; x++) {
			double src_x = (double)x * src_w / dest_w;
			double src_y = (double)y * src_h / dest_h;
			int sx = (int)src_x;
			int sy = (int)src_y;

			/* Clamp to source bounds */
			if (sx >= src_w - 1) sx = src_w - 2;
			if (sy >= src_h - 1) sy = src_h - 2;
			if (sx < 0) sx = 0;
			if (sy < 0) sy = 0;

			/* Simple nearest neighbor for now */
			for (c = 0; c < 4; c++) {
				dest[(y * dest_w + x) * 4 + c] =
				    src[(sy * src_w + sx) * 4 + c];
			}
		}
	}

	return dest;
}

/* Alpha blend src onto dest */
static void
alpha_blend(unsigned char *dest, unsigned char *src, int w, int h,
    int dest_x, int dest_y, int dest_w, int dest_h, int alpha)
{
	int x, y, dx, dy;

	for (y = 0; y < h; y++) {
		dy = dest_y + y;
		if (dy < 0 || dy >= dest_h)
			continue;

		for (x = 0; x < w; x++) {
			dx = dest_x + x;
			if (dx < 0 || dx >= dest_w)
				continue;

			int src_idx = (y * w + x) * 4;
			int dest_idx = (dy * dest_w + dx) * 4;
			int src_alpha = (src[src_idx + 3] * alpha) / 255;
			int inv_alpha = 255 - src_alpha;

			dest[dest_idx + 0] = (src[src_idx + 0] * src_alpha +
			    dest[dest_idx + 0] * inv_alpha) / 255;
			dest[dest_idx + 1] = (src[src_idx + 1] * src_alpha +
			    dest[dest_idx + 1] * inv_alpha) / 255;
			dest[dest_idx + 2] = (src[src_idx + 2] * src_alpha +
			    dest[dest_idx + 2] * inv_alpha) / 255;
		}
	}
}

/* Parse geometry string (WIDTHxHEIGHT+X+Y) */
static int
parse_geometry(const char *geom, int *x, int *y, int *w, int *h, int *flags)
{
	unsigned int uw, uh;

	*flags = XParseGeometry(geom, x, y, &uw, &uh);
	*w = uw;
	*h = uh;

	return *flags != 0;
}

/* Apply image to background buffer */
static void
apply_image(struct wallpaper_state *state, unsigned char *bg,
    const char *image_file, int is_emblem)
{
	unsigned char *img_data, *scaled_data = NULL;
	int img_w, img_h, img_channels;
	int dest_x, dest_y, dest_w, dest_h;
	int geom_flags = 0;
	int geom_x = 0, geom_y = 0, geom_w = 0, geom_h = 0;
	int alpha = is_emblem ? state->emblem_alpha : 255;

	if (!image_file)
		return;

	img_data = stbi_load(image_file, &img_w, &img_h, &img_channels, 4);
	if (!img_data) {
		warnx("cannot load image: %s", image_file);
		return;
	}

	/* Parse geometry if provided */
	if (state->geometry)
		parse_geometry(state->geometry, &geom_x, &geom_y,
		    &geom_w, &geom_h, &geom_flags);

	/* Determine target dimensions */
	if (state->scale_width_percent > 0)
		dest_w = (int)(state->width * state->scale_width_percent / 100.0);
	else if (geom_flags & WidthValue)
		dest_w = geom_w;
	else
		dest_w = img_w;

	if (state->scale_height_percent > 0)
		dest_h = (int)(state->height * state->scale_height_percent / 100.0);
	else if (geom_flags & HeightValue)
		dest_h = geom_h;
	else
		dest_h = img_h;

	/* Apply aspect ratio correction if needed */
	if (state->keep_aspect && img_h > 0) {
		double img_aspect = (double)img_w / img_h;
		double dest_aspect = (dest_h > 0) ? (double)dest_w / dest_h : 1.0;

		if (dest_aspect > img_aspect) {
			/* Too wide, reduce width */
			dest_w = (int)(dest_h * img_aspect);
		} else if (dest_aspect < img_aspect) {
			/* Too tall, reduce height */
			dest_h = (int)(dest_w / img_aspect);
		}
	}

	/* Determine position */
	if (state->center_x)
		dest_x = (state->width - dest_w) / 2;
	else if (geom_flags & XValue) {
		if (geom_flags & XNegative)
			dest_x = state->width - dest_w + geom_x;
		else
			dest_x = geom_x;
	} else
		dest_x = 0;

	if (state->center_y)
		dest_y = (state->height - dest_h) / 2;
	else if (geom_flags & YValue) {
		if (geom_flags & YNegative)
			dest_y = state->height - dest_h + geom_y;
		else
			dest_y = geom_y;
	} else
		dest_y = 0;

	/* Scale if needed */
	if (dest_w != img_w || dest_h != img_h) {
		scaled_data = scale_image(img_data, img_w, img_h, dest_w, dest_h);
		if (scaled_data) {
			stbi_image_free(img_data);
			img_data = scaled_data;
			img_w = dest_w;
			img_h = dest_h;
		}
	}

	/* Blend onto background */
	alpha_blend(bg, img_data, img_w, img_h, dest_x, dest_y,
	    state->width, state->height, alpha);

	stbi_image_free(img_data);
}

/* Create X11 pixmap from RGBA buffer */
static Pixmap
create_pixmap_from_buffer(Display *dpy, int screen, unsigned char *buffer,
    int width, int height)
{
	Pixmap pixmap;
	GC gc;
	XImage *ximage;
	Visual *visual;
	int depth;
	unsigned char *xdata;
	int x, y;

	visual = DefaultVisual(dpy, screen);
	depth = DefaultDepth(dpy, screen);

	pixmap = XCreatePixmap(dpy, RootWindow(dpy, screen), width, height, depth);
	gc = XCreateGC(dpy, pixmap, 0, NULL);

	/* Convert RGBA to X11 image format */
	xdata = malloc(width * height * 4);
	if (!xdata) {
		XFreeGC(dpy, gc);
		XFreePixmap(dpy, pixmap);
		return 0;
	}

	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			int idx = (y * width + x) * 4;
			unsigned char r = buffer[idx + 0];
			unsigned char g = buffer[idx + 1];
			unsigned char b = buffer[idx + 2];

			/* Store in X11 format (BGRA on little-endian) */
			xdata[idx + 0] = b;
			xdata[idx + 1] = g;
			xdata[idx + 2] = r;
			xdata[idx + 3] = 0;
		}
	}

	ximage = XCreateImage(dpy, visual, depth, ZPixmap, 0,
	    (char *)xdata, width, height, 32, 0);
	if (!ximage) {
		free(xdata);
		XFreeGC(dpy, gc);
		XFreePixmap(dpy, pixmap);
		return 0;
	}

	XPutImage(dpy, pixmap, gc, ximage, 0, 0, 0, 0, width, height);

	ximage->data = NULL;  /* Don't free xdata twice */
	XDestroyImage(ximage);
	free(xdata);
	XFreeGC(dpy, gc);

	return pixmap;
}

/* Set root window pixmap following ESETROOT spec */
static void
set_root_pixmap(Display *dpy, int screen, Pixmap pixmap)
{
	Window root = RootWindow(dpy, screen);
	Atom esetroot_pmap_id, xrootpmap_id;
	Atom type;
	int format;
	unsigned long nitems, bytes_after;
	unsigned char *data_esetroot = NULL;
	Pixmap old_pixmap;

	esetroot_pmap_id = XInternAtom(dpy, "ESETROOT_PMAP_ID", False);
	xrootpmap_id = XInternAtom(dpy, "_XROOTPMAP_ID", False);

	XGrabServer(dpy);

	/* Check for existing pixmap and free it */
	if (XGetWindowProperty(dpy, root, esetroot_pmap_id,
	    0L, 1L, False, XA_PIXMAP, &type, &format, &nitems,
	    &bytes_after, &data_esetroot) == Success) {
		if (type == XA_PIXMAP && format == 32 && nitems == 1) {
			old_pixmap = *((Pixmap *)data_esetroot);
			XKillClient(dpy, old_pixmap);
		}
		if (data_esetroot)
			XFree(data_esetroot);
	}

	/* Set new pixmap properties */
	XChangeProperty(dpy, root, esetroot_pmap_id, XA_PIXMAP, 32,
	    PropModeReplace, (unsigned char *)&pixmap, 1);
	XChangeProperty(dpy, root, xrootpmap_id, XA_PIXMAP, 32,
	    PropModeReplace, (unsigned char *)&pixmap, 1);

	/* Set as background */
	XSetWindowBackgroundPixmap(dpy, root, pixmap);
	XClearWindow(dpy, root);

	XUngrabServer(dpy);
	XFlush(dpy);
}

int
wallpaper_apply(struct wallpaper_state *state)
{
	unsigned char *buffer;
	Pixmap pixmap;

	/* Create background gradient or solid color */
	buffer = create_gradient(state);
	if (!buffer) {
		warnx("failed to create background");
		return -1;
	}

	/* Apply tile if specified (not fully implemented yet) */
	if (state->tile_file) {
		apply_image(state, buffer, state->tile_file, 0);
	}

	/* Apply main image */
	if (state->image_file)
		apply_image(state, buffer, state->image_file, 0);

	/* Apply emblem overlay if different from main image */
	if (state->emblem_file && (!state->image_file ||
	    strcmp(state->emblem_file, state->image_file) != 0))
		apply_image(state, buffer, state->emblem_file, 1);

	/* Create pixmap from buffer */
	pixmap = create_pixmap_from_buffer(state->dpy, state->screen, buffer,
	    state->width, state->height);
	free(buffer);

	if (!pixmap) {
		warnx("failed to create pixmap");
		return -1;
	}

	/* Set as root pixmap */
	set_root_pixmap(state->dpy, state->screen, pixmap);

	return 0;
}
