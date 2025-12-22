/*
 * Config file. Edit these values and recompile.
 * Copyright (C) 2000, 2001, 2002, 2003, 2004 Shawn Betts <sabetts@vcn.bc.ca>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place, Suite 330, Boston, MA 02111-1307 USA.
 */

#ifndef _POISON_CONF_H
#define _POISON_CONF_H 1

#include "data.h"
#include "actions.h"

#define PROGNAME	"poison"

/* Use Super_L as escape key by default. */
#define KEY_PREFIX      XK_Super_L
#define MODIFIER_PREFIX 0

/* Terminal executed by default. */
#define TERM_PROG	"xterm"

/* This is the abort key when typing input. */
#define INPUT_ABORT_KEY      XK_g
#define INPUT_ABORT_MODIFIER RP_CONTROL_MASK

/* Key used to enlarge frame vertically when in resize mode.  */
#define RESIZE_VGROW_KEY      XK_n
#define RESIZE_VGROW_MODIFIER RP_CONTROL_MASK

/* Key used to shrink frame vertically when in resize mode.  */
#define RESIZE_VSHRINK_KEY      XK_p
#define RESIZE_VSHRINK_MODIFIER RP_CONTROL_MASK

/* Key used to enlarge frame horizontally when in resize mode.  */
#define RESIZE_HGROW_KEY      XK_f
#define RESIZE_HGROW_MODIFIER RP_CONTROL_MASK

/* Key used to shrink frame horizontally when in resize mode.  */
#define RESIZE_HSHRINK_KEY      XK_b
#define RESIZE_HSHRINK_MODIFIER RP_CONTROL_MASK

/* Key used to shrink frame to fit it's current window.  */
#define RESIZE_SHRINK_TO_WINDOW_KEY             XK_s
#define RESIZE_SHRINK_TO_WINDOW_MODIFIER        0

/* Key used to exit resize mode.  */
#define RESIZE_END_KEY      XK_Return
#define RESIZE_END_MODIFIER 0

/*
 * Treat windows with maxsize hints as if they were a transient window (don't
 * hide the windows underneath, and center them)
 */
#define MAXSIZE_WINDOWS_ARE_TRANSIENTS

/*
 * Treat windows with aspect hints as if they were a transient window (don't
 * hide the windows underneath, and center them)
 */
#define ASPECT_WINDOWS_ARE_TRANSIENTS

/*
 * An alias command could recursively call inself infinitely. This stops that
 * behavior.
 */
#define MAX_ALIAS_RECURSIVE_DEPTH 16

/* Maximum depth of a link. Used in the 'link' command. */
#define MAX_LINK_DEPTH 16

/* This is the name of the first vscreen that is created. */
#define DEFAULT_VSCREEN_NAME "default"

/* The name of the root keymap */
#define ROOT_KEYMAP "root"

/* The name of the top level keymap */
#define TOP_KEYMAP "top"

/* The default font */
#define DEFAULT_XFT_FONT "monospace:size=12"

#endif                          /* !_ _POISON_CONF_H */
