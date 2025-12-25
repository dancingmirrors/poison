/*
 * Copyright © 2000, 2001, 2002, 2003, 2004 Shawn Betts <sabetts@vcn.bc.ca>
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

#ifndef _POISON_H
#define _POISON_H 1

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xlocale.h>
#include <X11/Xmd.h>
#include <X11/extensions/XRes.h>
#include <X11/X.h>
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>
#include <fcntl.h>

#if defined(__BASE_FILE__)
#define RP_FILE_NAME __BASE_FILE__
#else
#define RP_FILE_NAME __FILE__
#endif

/* Helper macro for error and debug reporting. */
#define PRINT_LINE(type) printf (PROGNAME ":%s:%d: %s: ",RP_FILE_NAME,  __LINE__, #type)

/* Debug reporting macros. */
#ifdef DEBUG
#define PRINT_DEBUG(fmt)                        \
do {                                            \
  PRINT_LINE (debug);                           \
  printf fmt;                                   \
  fflush (stdout);                              \
} while (0)
#else
#define PRINT_DEBUG(fmt) do {} while (0)
#endif                          /* DEBUG */

#ifdef SENDCMD_DEBUG
#define WARNX_DEBUG(fmt, ...)                   \
do {                                            \
  fprintf (stderr, fmt, __VA_ARGS__);           \
  fflush (stderr);                              \
} while (0)
#else
#define WARNX_DEBUG(fmt, ...) do {} while (0)
#endif                          /* SENDCMD_DEBUG */

#ifdef INPUT_DEBUG
#define PRINT_INPUT_DEBUG(fmt)                  \
do {                                            \
  PRINT_LINE (debug);                           \
  printf fmt;                                   \
  fflush (stdout);                              \
} while (0)
#else
#define PRINT_INPUT_DEBUG(fmt) do {} while (0)
#endif                          /* INPUT_DEBUG */

#define PROGNAME	"poison"

/* Use Super_L as escape key by default. */
#define KEY_PREFIX      XK_Super_L
#define MODIFIER_PREFIX 0

/* Terminal executed by default. */
#define TERM_PROG	"xterm"

/* This is the abort key when typing input. */
#define INPUT_ABORT_KEY      XK_g
#define INPUT_ABORT_MODIFIER RP_CONTROL_MASK

/* Key used to enlarge frame vertically when in resize mode. */
#define RESIZE_VGROW_KEY      XK_n
#define RESIZE_VGROW_MODIFIER RP_CONTROL_MASK

/* Key used to shrink frame vertically when in resize mode. */
#define RESIZE_VSHRINK_KEY      XK_p
#define RESIZE_VSHRINK_MODIFIER RP_CONTROL_MASK

/* Key used to enlarge frame horizontally when in resize mode. */
#define RESIZE_HGROW_KEY      XK_f
#define RESIZE_HGROW_MODIFIER RP_CONTROL_MASK

/* Key used to shrink frame horizontally when in resize mode. */
#define RESIZE_HSHRINK_KEY      XK_b
#define RESIZE_HSHRINK_MODIFIER RP_CONTROL_MASK

/* Key used to shrink frame to fit it's current window. */
#define RESIZE_SHRINK_TO_WINDOW_KEY             XK_s
#define RESIZE_SHRINK_TO_WINDOW_MODIFIER        0

/* Key used to exit resize mode. */
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
#define DEFAULT_XFT_FONT "PxPlus IBM VGA8:antialias=true:hintstyle=hintfull:size=12"

#include <string.h>

/*
 * Simple doubly linked list implementation.
 *
 * Some of the internal functions ("__xxx") are useful when
 * manipulating whole lists rather than single entries, as
 * sometimes we already know the next/prev entries and we can
 * generate better code by using them directly rather than
 * using the generic single-entry routines.
 */

struct list_head {
    struct list_head *next, *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }

#define LIST_HEAD(name) \
        struct list_head name = LIST_HEAD_INIT(name)

#define INIT_LIST_HEAD(ptr) do { \
        (ptr)->next = (ptr); (ptr)->prev = (ptr); \
} while (0)

/* Prototypes of C functions. */
int list_size(struct list_head *list);
void list_splice_init(struct list_head *list, struct list_head *head);

void list_splice(struct list_head *list, struct list_head *head);

void __list_splice(struct list_head *list, struct list_head *head);

int list_empty(struct list_head *head);

void list_move_tail(struct list_head *list, struct list_head *head);

void list_move(struct list_head *list, struct list_head *head);

void list_del_init(struct list_head *entry);
void list_del(struct list_head *entry);
void __list_del(struct list_head *prev, struct list_head *next);
void list_add_tail(struct list_head *new, struct list_head *head);
void list_add(struct list_head *new, struct list_head *head);
void __list_add(struct list_head *new, struct list_head *prev,
                struct list_head *next);

#define prefetch(x) __builtin_prefetch(x)

/* Return the last element in the list. */
#define list_last(last, head, member)                           \
{                                                               \
  last = list_entry((head)->prev, typeof(*last), member);       \
  if (&last->member == (head))                                  \
    last = NULL;                                                \
}

/**
 * container_of - cast a member of a structure out to the containing structure
 *
 * @ptr:        the pointer to the member.
 * @type:       the type of the container struct this is embedded in.
 * @member:     the name of the member within the struct.
 *
 */
#define container_of(ptr, type, member) ({                      \
        const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
        (type *)( (char *)__mptr - offsetof(type,member) );})

/**
 * list_entry - get the struct for this entry
 * @ptr:        the &struct list_head pointer.
 * @type:       the type of the struct this is embedded in.
 * @member:     the name of the list_struct within the struct.
 */
#define list_entry(ptr, type, member) \
        container_of(ptr, type, member)

/**
 * __list_for_each      -       iterate over a list
 * @pos:        the &struct list_head to use as a loop counter.
 * @head:       the head for your list.
 *
 * This variant differs from list_for_each() in that it's the
 * simplest possible list iteration code, no prefetching is done.
 * Use this for code that knows the list to be very short (empty
 * or 1 entry) most of the time.
 */
#define list_for_each(pos, head) \
        for (pos = (head)->next; pos != (head); pos = pos->next)

/**
 * list_for_each_prev   -       iterate over a list backwards
 * @pos:        the &struct list_head to use as a loop counter.
 * @head:       the head for your list.
 */
#define list_for_each_prev(pos, head) \
        for (pos = (head)->prev, prefetch(pos->prev); pos != (head); \
        pos = pos->prev, prefetch(pos->prev))

/**
 * list_for_each_safe   -       iterate over a list safe against removal of list entry
 * @pos:        the &struct list_head to use as a loop counter.
 * @n:          another &struct list_head to use as temporary storage
 * @head:       the head for your list.
 */
#define list_for_each_safe(pos, n, head) \
        for (pos = (head)->next, n = pos->next; pos != (head); \
                pos = n, n = pos->next)

#define list_for_each_safe_entry(item, pos, n, head, member) \
        for (pos = (head)->next,  \
             item = list_entry(pos, typeof(*item), member), \
             n = pos->next  \
                     ; \
             pos != (head) \
                     ; \
             pos = n,  \
             item = list_entry(pos, typeof(*item), member), \
             n = pos->next) \

/**
 * list_for_each_entry  -       iterate over list of given type
 * @pos:        the type * to use as a loop counter.
 * @head:       the head for your list.
 * @member:     the name of the list_struct within the struct.
 */
#define list_for_each_entry(pos, head, member)                          \
        for (pos = list_entry((head)->next, typeof(*pos), member),      \
                     prefetch(pos->member.next);                        \
             &pos->member != (head);                                    \
             pos = list_entry(pos->member.next, typeof(*pos), member),  \
                     prefetch(pos->member.next))

#define list_for_each_entry_safe(pos, n, head, member)                  \
        for (pos = list_entry((head)->next, typeof(*pos), member),      \
                n = list_entry(pos->member.next, typeof(*pos), member); \
             &pos->member != (head);                                    \
             pos = n,                                                   \
                n = list_entry(pos->member.next, typeof(*pos), member))

#define list_direction_entry(pos, head, member, direction) \
({ \
        typeof(pos) ret = NULL;  \
        struct list_head *a_head = head;  \
        if (pos->member.direction == a_head) { \
                        ret = list_entry(a_head->direction,  \
                                         typeof(*pos), member); \
        } else { \
                ret = list_entry(pos->member.direction,  \
                                 typeof(*pos), member); \
        } \
        ret; \
})

#define list_next_entry(pos, head, member) \
        list_direction_entry(pos, head, member, next)

#define list_prev_entry(pos, head, member) \
        list_direction_entry(pos, head, member, prev)

#define list_for_each_entry_prev(pos, head, member)                     \
        for (pos = list_entry((head)->prev, typeof(*pos), member),      \
                     prefetch(pos->member.prev);                        \
             &pos->member != (head);                                    \
             pos = list_entry(pos->member.prev, typeof(*pos), member),  \
                     prefetch(pos->member.prev))

/* Return the first element in the list. */
#define list_first(first, head, member)                         \
{                                                               \
  first = list_entry((head)->next, typeof(*first), member);     \
  if (&first->member == (head))                                 \
    first = NULL;                                               \
}

void
list_sort(void *priv, struct list_head *head,
          int (*cmp)(void *priv, struct list_head * a,
                     struct list_head * b));

struct numset;

struct numset *numset_new(void);
void numset_free(struct numset *ns);
void numset_release(struct numset *ns, int n);
int numset_request(struct numset *ns);
int numset_add_num(struct numset *ns, int n);

#define MESSAGE_NO_OTHER_WINDOW		"No other window"
#define MESSAGE_NO_OTHER_FRAME		"No other frame"
#define MESSAGE_NO_MANAGED_WINDOWS	"No managed windows"
#define MESSAGE_UNKNOWN_COMMAND		"Unknown command '%s'"
#define MESSAGE_WINDOW_INFORMATION	"This is window %d (%s)"

#define MESSAGE_RAISE_TRANSIENT		"Raise request from transient window %d (%s)"
#define MESSAGE_RAISE_WINDOW		"Raise request from window %d (%s)"
#define MESSAGE_RAISE_TRANSIENT_VSCREEN	"Raise request from transient window %d (%s) on vscreen %d"
#define MESSAGE_RAISE_WINDOW_VSCREEN	"Raise request from window %d (%s) on vscreen %d"
#define MESSAGE_MAP_TRANSIENT		"New transient window %d (%s)"
#define MESSAGE_MAP_WINDOW		"New window %d (%s)"
#define MESSAGE_MAP_TRANSIENT_VSCREEN	"New transient window %d (%s) on vscreen %d"
#define MESSAGE_MAP_WINDOW_VSCREEN	"New window %d (%s) on vscreen %d"

#define MESSAGE_PROMPT_SWITCH_TO_WINDOW	"Switch to window: "
#define MESSAGE_PROMPT_NEW_WINDOW_NAME	"Set window's title to: "
#define MESSAGE_PROMPT_SHELL_COMMAND	"/bin/sh -c "
#define MESSAGE_PROMPT_COMMAND		":"
#define MESSAGE_PROMPT_XTERM_COMMAND	MESSAGE_PROMPT_SHELL_COMMAND TERM_PROG " -e "
#define MESSAGE_PROMPT_SWITCH_TO_VSCREEN "Switch to vscreen: "
#define MESSAGE_PROMPT_SELECT_VAR	"Variable: "
#define MESSAGE_PROMPT_VAR_VALUE	"Value: "

#define MESSAGE_WELCOME			"Welcome to " PROGNAME " - press \"%s %s\" for help."

#define EMPTY_FRAME_MESSAGE		"Current Frame"

#include <stdlib.h>

struct sbuf {
    char *data;
    size_t len;
    size_t maxsz;

    /* sbuf can exist in a list. */
    struct list_head node;
};

struct sbuf *sbuf_new(size_t initsz);
void sbuf_free(struct sbuf *b);
char *sbuf_free_struct(struct sbuf *b);
char *sbuf_concat(struct sbuf *b, const char *str);
char *sbuf_nconcat(struct sbuf *b, const char *str, int len);
char *sbuf_utf8_nconcat(struct sbuf *b, const char *s, int width);
char *sbuf_copy(struct sbuf *b, const char *str);
char *sbuf_clear(struct sbuf *b);
char *sbuf_get(struct sbuf *b);
char *sbuf_printf(struct sbuf *b, char *fmt, ...);
char *sbuf_printf_concat(struct sbuf *b, char *fmt, ...);
void sbuf_chop(struct sbuf *b);

#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>

typedef struct rp_window rp_window;
typedef struct rp_screen rp_screen;
typedef struct rp_global_screen rp_global_screen;
typedef struct rp_vscreen rp_vscreen;
typedef struct rp_action rp_action;
typedef struct rp_keymap rp_keymap;
typedef struct rp_frame rp_frame;
typedef struct rp_child_info rp_child_info;
typedef struct rp_window_elem rp_window_elem;
typedef struct rp_completions rp_completions;
typedef struct rp_input_line rp_input_line;

enum rp_edge {
    EDGE_TOP = (1 << 1),
    EDGE_LEFT = (1 << 2),
    EDGE_RIGHT = (1 << 3),
    EDGE_BOTTOM = (1 << 4),
};

struct rp_frame {
    rp_vscreen *vscreen;

    int number;
    int x, y, width, height;

    /* The number of the window that is focused in this frame. */
    int win_number;

    /* The number of the window to focus when restoring this frame. */
    int restore_win_number;

    /* For determining the last frame. */
    int last_access;

    /*
     * Boolean that is set when a frame is `dedicated' (a.k.a. glued) to
     * one window.
     */
    unsigned int dedicated;

    /* Whether this frame is touching an edge before a screen update */
    enum rp_edge edges;

    struct list_head node;
};

struct rp_window {
    rp_vscreen *vscreen;
    Window w;
    int state;
    int last_access;
    int named;

    /*
     * A number uniquely identifying this window. This is a different
     * number than the one given to it by the vscreen it is in. This number
     * is used for internal purposes, whereas the vscreen number is what
     * the user sees.
     */
    int number;

    /* Window name hints */
    char *user_name;
    char *wm_name;
    char *res_name;
    char *res_class;

    /* Dimensions */
    int x, y, width, height, border, full_screen;

    /* WM Hints */
    XSizeHints *hints;

    /* Colormap */
    Colormap colormap;

    /* Is this a transient window? */
    int transient;
    Window transient_for;

    /* Does this window accept input focus (from WM_HINTS)? */
    int accepts_input;

    /* Does this window support WM_TAKE_FOCUS protocol? */
    int supports_wm_take_focus;

    /* Timestamp when window was withdrawn (for cleanup of lingering windows) */
    time_t withdrawn_at;

    /* Is this a floated window? */
    int floated;

    /* Saved mouse position */
    int mouse_x, mouse_y;

    /*
     * The alignment of the window. Decides to what side or corner the
     * window sticks to.
     */
    int gravity;

    /*
     * A window can be visible inside a frame but not the frame's current
     * window. This keeps track of what frame the window was mapped into.
     */
    int frame_number;

    /* The frame number we want to remain in */
    int sticky_frame;

    /*
     * Sometimes a window is intended for a certain frame. When a window is
     * mapped and this is >0 then use the frame (if it exists).
     */
    int intended_frame_number;

    struct list_head node;
};

struct rp_window_elem {
    rp_window *win;
    int number;
    struct list_head node;
};

struct rp_global_screen {
    Window root, wm_check;
    unsigned long fgcolor, bgcolor, fwcolor, bwcolor, bar_bordercolor;

    /* This numset is responsible for giving out numbers for each screen */
    struct numset *numset;
};

struct xrandr_info {
    int output;
    int crtc;
    int primary;
    char *name;
};

struct rp_vscreen {
    rp_screen *screen;

    /* Virtual screen number, handled by rp_screen's vscreens_numset */
    int number;

    /* Name */
    char *name;

    /* For determining the last vscreen. */
    int last_access;

    /*
     * A list of frames that may or may not contain windows. There should
     * always be one in the list.
     */
    struct list_head frames;

    /* Keep track of which numbers have been given to frames. */
    struct numset *frames_numset;

    /*
     * The number of the currently focused frame. One for each vscreen so
     * when you switch vscreens the focus doesn't get frobbed.
     */
    int current_frame;

    /* The list of windows participating in this vscreen. */
    struct list_head mapped_windows, unmapped_windows;

    /*
     * This numset is responsible for giving out numbers for each window in
     * the vscreen.
     */
    struct numset *numset;

    struct list_head node;
};

struct rp_font {
    char *name;
    XftFont *font;
};

struct rp_screen {
    GC normal_gc, inverse_gc;
    Window root, bar_window, key_window, input_window, frame_window,
        help_window;
    int bar_is_raised;
    int screen_num;             /* Our screen number as dictated by X */
    Colormap def_cmap;
    Cursor rat;

    /* Screen number, handled by rp_global_screen numset */
    int number;

    struct xrandr_info xrandr;

    /* Here to abstract over the Xrandr versus X screens difference */
    int left, top, width, height;

    char *display_string;

    /* Used by sfrestore */
    struct sbuf *scratch_buffer;

    XftFont *xft_font;
    struct rp_font xft_font_cache[5];
    XftColor xft_fgcolor, xft_bgcolor;

    struct list_head vscreens;
    struct numset *vscreens_numset;
    rp_vscreen *current_vscreen;

    rp_window *full_screen_win;

    struct sbuf *bar_text;

    /* This structure can exist in a list. */
    struct list_head node;
};

struct rp_action {
    KeySym key;
    unsigned int state;
    char *data;                 /* misc data to be passed to the function */
    /* void (*func)(void *); */
};

struct rp_keymap {
    char *name;
    rp_action *actions;
    int actions_last;
    int actions_size;

    /* This structure can be part of a list. */
    struct list_head node;
};

struct rp_key {
    KeySym sym;
    unsigned int state;
};

struct rp_defaults {
    /*
     * Default positions for new normal windows, transient windows, and
     * normal windows with maxsize hints.
     */
    int win_gravity;
    int trans_gravity;
    int maxsize_gravity;

    int input_window_size;
    int window_border_width;
    int only_border;

    int bar_x_padding;
    int bar_y_padding;
    int bar_location;
    int bar_timeout;
    int bar_border_width;
    int bar_in_padding;

    int frame_indicator_timeout;
    int frame_resize_unit;

    int padding_left;
    int padding_right;
    int padding_top;
    int padding_bottom;

    char *font_string;

    char *fgcolor_string;
    char *bgcolor_string;
    char *fwcolor_string;
    char *bwcolor_string;
    char *barbordercolor_string;

    int wait_for_key_cursor;

    char *window_fmt;
    char *info_fmt;
    char *resize_fmt;

    /* Which name to use: wm_name, res_name, res_class. */
    int win_name;

    /*
     * Decides whether the window list is displayed in a row or a column.
     */
    int window_list_style;

    /* Pointer warping toggle. */
    int warp;

    char *frame_selectors;

    /* How many frame sets to remember when undoing. */
    int maxundos;

    /* The name of the top level keymap */
    char *top_kmap;

    /* Frame indicator format */
    char *frame_fmt;

    /* Number of virtual screens */
    int vscreens;

    /* Window gap */
    int gap;

    /* Whether to ignore window size hints */
    int ignore_resize_hints;

    /* New mapped window always uses current vscreen */
    int win_add_cur_vscreen;
};

/* Information about a child process. */
struct rp_child_info {
    /* The command that was executed. */
    char *cmd;

    /* PID of the process. */
    int pid;

    /* Return status when the child process finished. */
    int status;

    /* When this is != 0 then the process finished. */
    int terminated;

    /* what was current when it was launched? */
    rp_frame *frame;
    rp_screen *screen;
    rp_vscreen *vscreen;

    /*
     * Non-zero when the pid has mapped a window. This is to prevent every
     * window the program opens from getting mapped in the frame it was
     * launched from. Only the first window should do this.
     */
    int window_mapped;

    /* This structure can exist in a list. */
    struct list_head node;
};

/*
 * These defines should be used to specify the modifier mask for keys and they
 * are translated into the X11 modifier mask when the time comes to compare
 * modifier masks.
 */
#define RP_SHIFT_MASK   1
#define RP_CONTROL_MASK 2
#define RP_META_MASK    4
#define RP_ALT_MASK     8
#define RP_SUPER_MASK   16
#define RP_HYPER_MASK   32

struct modifier_info {
    /* unsigned int mode_switch_mask; */
    unsigned int meta_mod_mask;
    unsigned int alt_mod_mask;
    unsigned int super_mod_mask;
    unsigned int hyper_mod_mask;

    /*
     * Keep track of these because they mess up the grab and should be
     * ignored.
     */
    unsigned int num_lock_mask;
    unsigned int scroll_lock_mask;
};

typedef struct list_head *(*completion_fn) (char *string);

/*
  BASIC: The completion shall begin with the same characters as the partial
  string. Case is ignored.

  SUBSTRING: The partial string shall be a subpart of the completion. Case
  is ignored.
*/
enum completion_styles {
    BASIC,
    SUBSTRING
};

struct rp_completions {
    /*
     * A pointer to the partial string that is being completed. We need to
     * store this so that the user can cycle through all possible
     * completions.
     */
    char *partial;

    /*
     * A pointer to the string that was last matched string. Used to keep
     * track of where we are in the completion list.
     */
    struct sbuf *last_match;

    /* A list of sbuf's which are possible completions. */
    struct list_head completion_list;

    /* The function that generates the completions. */
    completion_fn complete_fn;

    /*
     * virgin = 1 means no completions have been attempted on the input
     * string.
     */
    unsigned short int virgin;

    /* The completion style used to perform string comparisons */
    enum completion_styles style;
};

struct rp_input_line {
    char *buffer;
    char *prompt;
    char *saved;
    size_t position;
    size_t length;
    size_t size;
    rp_completions *compl;
    Atom selection;
};

/* The hook dictionary. */
struct rp_hook_db_entry {
    char *name;
    struct list_head *hook;
};

typedef struct rp_xselection rp_xselection;
struct rp_xselection {
    char *text;
    int len;
};

/* codes used in the cmdret code in actions.c */
#define RET_SUCCESS 1
#define RET_FAILURE 0

#define FONT_HEIGHT(s) ((s)->xft_font->ascent + (s)->xft_font->descent)
#define FONT_ASCENT(s) ((s)->xft_font->ascent)
#define FONT_DESCENT(s) ((s)->xft_font->descent)

#define MAX_FONT_WIDTH(f) (rp_font_width)

#define WIN_EVENTS (StructureNotifyMask | PropertyChangeMask | \
    ColormapChangeMask | FocusChangeMask)

/*
 * EMPTY is used when a frame doesn't contain a window, or a window doesn't
 * have a frame. Any time a field refers to the number of a
 * window/frame/screen/etc, Use EMPTY to denote a lack there of.
 */
#define EMPTY -1

/* Possible values for defaults.window_list_style */
#define STYLE_ROW    0
#define STYLE_COLUMN 1

/* Possible values for defaults.win_name */
#define WIN_NAME_TITLE          0
#define WIN_NAME_RES_CLASS      1
#define WIN_NAME_RES_NAME       2

/* Possible directions to traverse the completions list. */
#define COMPLETION_NEXT         0
#define COMPLETION_PREVIOUS     1

/* Font styles */
#define STYLE_NORMAL  0
#define STYLE_INVERSE 1

/* Whether or not we support xrandr */
extern int rp_have_xrandr;

/* Whether or not we support shape extension */
extern int rp_have_shape;
extern int rp_shape_event_base;

/*
 * Each child process is stored in this list. spawn, creates a new entry in
 * this list, the SIGCHLD handler sets child.terminated to be true and
 * handle_signals in events.c processes each terminated process by printing a
 * message saying the process ended and displaying it's exit code.
 */
extern struct list_head rp_children;

extern struct rp_defaults defaults;

/* Cached font info. */
extern int rp_font_ascent, rp_font_descent, rp_font_width;

/* The prefix key also known as the command character under screen. */
extern struct rp_key prefix_key;

/*
 * A list of mapped windows. These windows show up in the window list and have
 * a number assigned to them.
 */
extern struct list_head rp_mapped_window;

/*
 * A list of unmapped windows. These windows do not have a number assigned to
 * them and are not visible/active.
 */
extern struct list_head rp_unmapped_window;

/* The list of screens. */
extern struct list_head rp_screens;
extern rp_screen *rp_current_screen;
extern rp_global_screen rp_glob_screen;

extern Display *dpy;

extern XEvent rp_current_event;

extern Atom rp_selection;

extern Atom wm_name;
extern Atom wm_state;
extern Atom wm_change_state;
extern Atom wm_protocols;
extern Atom wm_delete;
extern Atom wm_take_focus;
extern Atom wm_colormaps;

/* TEXT atoms */
extern Atom xa_string;
extern Atom xa_compound_text;
extern Atom xa_utf8_string;

/* netwm atoms. */
extern Atom _net_active_window;
extern Atom _net_client_list;
extern Atom _net_client_list_stacking;
extern Atom _net_current_desktop;
extern Atom _net_number_of_desktops;
extern Atom _net_supported;
extern Atom _net_workarea;
extern Atom _net_wm_name;
extern Atom _net_wm_pid;
extern Atom _net_wm_state;
#define _NET_WM_STATE_REMOVE	0       /* remove/unset property */
#define _NET_WM_STATE_ADD	1       /* add/set property */
#define _NET_WM_STATE_TOGGLE	2       /* toggle property  */
extern Atom _net_wm_state_fullscreen;
extern Atom _net_wm_window_type;
extern Atom _net_wm_window_type_dialog;
extern Atom _net_wm_window_type_dock;
extern Atom _net_wm_window_type_splash;
extern Atom _net_wm_window_type_tooltip;
extern Atom _net_wm_window_type_utility;
extern Atom _net_supporting_wm_check;

/*
 * When unmapping or deleting windows, it is sometimes helpful to ignore a bad
 * window when attempting to clean the window up. This does just that when set
 * to 1.
 */
extern int ignore_badwindow;

/* Arguments passed at startup. */
extern char **myargv;

/* Keeps track of which mod mask each modifier is under. */
extern struct modifier_info rp_modifier_info;

/*
 * nonzero if an alarm signal was raised. This means we should hide our
 * popup windows.
 */
extern int alarm_signalled;
extern int kill_signalled;
extern int hup_signalled;
extern int chld_signalled;

/* rudeness levels */
extern int rp_honour_transient_raise;
extern int rp_honour_normal_raise;
extern int rp_honour_transient_map;
extern int rp_honour_normal_map;
extern int rp_honour_vscreen_switch;

/* Keep track of X11 error messages. */
extern char *rp_error_msg;

/* Number sets for windows. */
extern struct numset *rp_window_numset;

extern struct list_head rp_key_hook;
extern struct list_head rp_switch_win_hook;
extern struct list_head rp_switch_frame_hook;
extern struct list_head rp_switch_screen_hook;
extern struct list_head rp_switch_vscreen_hook;
extern struct list_head rp_delete_window_hook;
extern struct list_head rp_quit_hook;
extern struct list_head rp_new_window_hook;
extern struct list_head rp_title_changed_hook;

extern struct rp_hook_db_entry rp_hook_db[];

void set_rp_window_focus(rp_window * win);
void set_window_focus(Window window);

extern struct numset *rp_frame_numset;

/* Selection handling globals */
extern rp_xselection selection;
void set_selection(char *txt);
void set_nselection(char *txt, int len);
char *get_selection(void);

/* Wrapper font functions to support Xft */

XftFont *rp_get_font(rp_screen * s, char *font);
void rp_clear_cached_fonts(rp_screen * s);
void rp_draw_string(rp_screen * s, Drawable d, int style, int x, int y,
                    char *string, int length, char *font, char *color);
int rp_text_width(rp_screen * s, char *string, int count, char *font);

void check_child_procs(void);
void chld_handler(int signum);
void set_sig_handler(int sig, void (*action)(int));
void set_close_on_exec(int fd);
void read_rc_file(FILE * file);
const char *get_homedir(void);
char *get_config_dir(void);
void clean_up(void);

void register_atom(Atom * a, char *name);
int set_atom(Window w, Atom a, Atom type, unsigned long *val,
             unsigned long nitems);
int append_atom(Window w, Atom a, Atom type, unsigned long *val,
                unsigned long nitems);
unsigned long get_atom(Window w, Atom a, Atom type, unsigned long off,
                       unsigned long *ret, unsigned long nitems,
                       unsigned long *left);
void remove_atom(Window w, Atom a, Atom type, unsigned long remove);

void clear_unmanaged_list(void);
char *list_unmanaged_windows(void);
void add_unmanaged_window(char *name);
int unmanaged_window(Window w);
void clear_floated_list(void);
char *list_floated_windows(void);
void add_floated_window(char *name);
int floated_window(Window w);
void scanwins(void);
void unmanage(rp_window * w);
int update_window_name(rp_window * win);
void update_normal_hints(rp_window * win);
void rename_current_window(void);
void set_state(rp_window * win, int state);
long get_state(rp_window * win);
void check_state(rp_window * win);

int window_is_transient(rp_window * win);
Atom get_net_wm_window_type(rp_window * win);
int is_unmanaged_window_type(Window win);
void update_window_information(rp_window * win);
void update_window_input_hint(rp_window * win);
void update_window_protocols(rp_window * win);
void cleanup_withdrawn_windows(void);
void map_window(rp_window * win);

void maximize(rp_window * win);
void force_maximize(rp_window * win);

void grab_top_level_keys(Window w);
void ungrab_top_level_keys(Window w);
void ungrab_keys_all_wins(void);
void grab_keys_all_wins(void);

void hide_window(rp_window * win);
void unhide_window(rp_window * win);
void unhide_all_windows(void);
void withdraw_window(rp_window * win);
void hide_others(rp_window * win);
void hide_vscreen_windows(rp_vscreen * v);
void raise_utility_windows(void);

void free_window(rp_window * w);
rp_window *add_to_window_list(rp_screen * s, Window w);
void last_window(void);
rp_window *find_window_in_list(Window w, struct list_head *list);
rp_window *find_window(Window w);
void maximize_current_window(void);
void give_window_focus(rp_window * win, rp_window * last_win);
void set_active_window(rp_window * win);
void set_active_window_force(rp_window * win);
void goto_window(rp_window * win);
void update_window_gravity(rp_window * win);
char *window_name(rp_window * win);

/* int goto_window_name (char *name); */
rp_window *find_window_other(rp_vscreen * vscreen);
rp_window *find_window_by_number(int n);
rp_window *find_window_name(char *name, int exact_match);
rp_window *find_window_number(int n);

void insert_into_list(rp_window * win, struct list_head *list);

void get_current_window_in_fmt(char *fmt, struct sbuf *buffer);
void get_window_list(char *fmt, char *delim, struct sbuf *buffer,
                     int *mark_start, int *mark_end);
void init_window_stuff(void);
void free_window_stuff(void);

rp_frame *win_get_frame(rp_window * win);

struct rp_child_info *get_child_info(Window w, int add);
void change_windows_vscreen(rp_vscreen * v, rp_vscreen * new_vscreen);

void window_full_screen(rp_window * win);

/* Possible values for bar_is_raised status. */
#define BAR_IS_HIDDEN		0
#define BAR_IS_WINDOW_LIST	1
#define BAR_IS_VSCREEN_LIST	2
#define BAR_IS_MESSAGE		3

#define BAR_IS_RAISED(s)	(s->bar_is_raised != BAR_IS_HIDDEN)

void init_bar(void);
void bar_reset_alarm(void);
void update_window_names(rp_screen * s, char *fmt);
void update_vscreen_names(rp_screen * s);
void update_bar(rp_screen * s);
void show_bar(rp_screen * s, char *fmt);
void show_vscreen_bar(rp_screen * s);
void hide_bar(rp_screen * s, int force);
int bar_y(rp_screen * s, int height);
int bar_x(rp_screen * s, int width);

void message(char *s);
void marked_message(char *s, int mark_start, int mark_end, int bar_type);
void marked_message_printf(int mark_start, int mark_end, char *fmt, ...);
void redraw_last_message(void);
void show_last_message(void);
void free_bar(void);

void listen_for_events(void);
void show_rudeness_msg(rp_window * win, int raised);

char *keysym_to_string(KeySym keysym, unsigned int modifier);
int cook_keycode(XKeyEvent * ev, KeySym * keysym, unsigned int *mod,
                 char *keysym_name, int len, int ignore_bad_mods);
char *get_input(char *prompt, completion_fn fn);
char *get_more_input(char *prompt, char *preinput,
                     enum completion_styles style, completion_fn fn);
void read_any_key(void);
int read_single_key(KeySym * keysym, unsigned int *modifiers,
                    char *keysym_name, int len);
int read_key(KeySym * keysym, unsigned int *modifiers, char *keysym_name,
             int len);
unsigned int x11_mask_to_rp_mask(unsigned int mask);
unsigned int rp_mask_to_x11_mask(unsigned int mask);
void update_modifier_map(void);
void grab_key(KeySym keysym, unsigned int modifiers, Window grab_window);

void init_xkb(void);

rp_window *set_frames_window(rp_frame * frame, rp_window * win);
void cleanup_frame(rp_frame * frame);
void maximize_all_windows_in_frame(rp_frame * frame);
void maximize_frame(rp_frame * frame);
void h_split_frame(rp_frame * frame, int pixels);
void v_split_frame(rp_frame * frame, int pixels);
void remove_all_splits(void);
void resize_shrink_to_window(rp_frame * frame);
void resize_frame_horizontally(rp_frame * frame, int diff);
void resize_frame_vertically(rp_frame * frame, int diff);
void remove_frame(rp_frame * frame);
rp_window *find_window_for_frame(rp_frame * frame);
rp_frame *find_windows_frame(rp_window * win);
int num_frames(rp_vscreen * v);
rp_frame *find_frame_next(rp_frame * frame);
rp_frame *find_frame_prev(rp_frame * frame);
rp_window *current_window(void);
void init_frame_list(rp_vscreen * vscreen);
void set_active_frame(rp_frame * frame, int force_indicator);
void exchange_with_frame(rp_frame * cur, rp_frame * frame);
void blank_frame(rp_frame * frame);
void show_frame_indicator(int force);
void hide_frame_indicator(void);

void show_frame_message(char *msg);

rp_frame *find_frame_right(rp_frame * frame);
rp_frame *find_frame_left(rp_frame * frame);
rp_frame *find_frame_down(rp_frame * frame);
rp_frame *find_frame_up(rp_frame * frame);
rp_frame *find_last_frame(rp_vscreen * v);
rp_frame *find_frame_number(rp_vscreen * v, int num);

rp_frame *current_frame(rp_vscreen * v);

void frame_resize_down(rp_frame * frame, int amount);
void frame_resize_up(rp_frame * frame, int amount);
void frame_resize_right(rp_frame * frame, int amount);
void frame_resize_left(rp_frame * frame, int amount);
void mark_edge_frames(void);
int frame_height(rp_frame * frame);
int frame_width(rp_frame * frame);
int frame_bottom(rp_frame * frame);
int frame_bottom_screen_edge(rp_frame * frame);
int frame_right(rp_frame * frame);
int frame_right_screen_edge(rp_frame * frame);
int frame_top(rp_frame * frame);
int frame_top_screen_edge(rp_frame * frame);
int frame_left(rp_frame * frame);
int frame_left_screen_edge(rp_frame * frame);
int frame_bottom_abs(rp_frame * frame);
int frame_right_abs(rp_frame * frame);
int frame_top_abs(rp_frame * frame);
int frame_left_abs(rp_frame * frame);

rp_frame *frame_new(rp_vscreen * v);
void frame_free(rp_vscreen * v, rp_frame * f);
rp_frame *frame_copy(rp_frame * frame);
char *frame_dump(rp_frame * frame, rp_vscreen * vscreen);
rp_frame *frame_read(char *str, rp_vscreen * vscreen);

rp_vscreen *frames_vscreen(rp_frame *);

int screen_bottom(rp_screen * s);
int screen_top(rp_screen * s);
int screen_right(rp_screen * s);
int screen_left(rp_screen * s);
int screen_height(rp_screen * s);
int screen_width(rp_screen * s);

rp_screen *find_screen(Window w);
rp_screen *find_screen_by_attr(XWindowAttributes w);

void init_screens(void);
void activate_screen(rp_screen * s);
void deactivate_screen(rp_screen * s);

int is_rp_window(Window w);
int is_a_root_window(unsigned int w);

char *screen_dump(rp_screen * screen);

void screen_update(rp_screen * s, int left, int top, int width,
                   int height);
void screen_update_frames(rp_screen * s);
void screen_update_workarea(rp_screen * s);

int screen_count(void);
rp_screen *screen_primary(void);
rp_screen *screen_next(void);
rp_screen *screen_prev(void);

rp_screen *screen_number(int number);

void screen_sort(void);

rp_screen *screen_add(int rr_output);
void screen_del(rp_screen * s);
void screen_free(rp_screen * s);
void screen_free_final(void);

void init_vscreen(rp_vscreen * v, rp_screen * s);
void vscreen_del(rp_vscreen * v);
void vscreen_free(rp_vscreen * v);
int vscreens_resize(int n);

rp_vscreen *screen_find_vscreen_by_number(rp_screen * s, int n);
rp_vscreen *screen_find_vscreen_by_name(rp_screen * s, char *name,
                                        int exact_match);

struct list_head *vscreen_copy_frameset(rp_vscreen * v);
void vscreen_restore_frameset(rp_vscreen * v, struct list_head *head);
void vscreen_free_nums(rp_vscreen * v);
void frameset_free(struct list_head *head);
rp_frame *vscreen_get_frame(rp_vscreen * v, int frame_num);
rp_frame *vscreen_find_frame_by_frame(rp_vscreen * v, rp_frame * f);

void set_current_vscreen(rp_vscreen * v);
void vscreen_move_window(rp_vscreen * v, rp_window * w);

void vscreen_add_window(rp_vscreen * v, rp_window * w);
void vscreen_resort_window(rp_vscreen * v, rp_window_elem * w);
void vscreen_insert_window(struct list_head *h, rp_window_elem * w);

void vscreen_del_window(rp_vscreen * v, rp_window * win);

void vscreen_map_window(rp_vscreen * v, rp_window * win);

void vscreen_unmap_window(rp_vscreen * v, rp_window * win);

struct numset *vscreen_get_numset(rp_vscreen * v);
void get_vscreen_list(rp_screen * s, char *delim, struct sbuf *buffer,
                      int *mark_start, int *mark_end);

rp_window *vscreen_prev_window(rp_vscreen * v, rp_window * win);
rp_window *vscreen_next_window(rp_vscreen * v, rp_window * win);

rp_window *vscreen_last_window(rp_vscreen * v);

rp_vscreen *vscreen_prev_vscreen(rp_vscreen * v);
rp_vscreen *vscreen_next_vscreen(rp_vscreen * v);
rp_vscreen *screen_last_vscreen(rp_screen * screen);

void vscreen_rename(rp_vscreen * v, char *name);

rp_window_elem *vscreen_find_window(struct list_head *list,
                                    rp_window * win);

void vscreen_move_window(rp_vscreen * to, rp_window * win);
void vscreens_merge(rp_vscreen * from, rp_vscreen * to);

void set_current_vscreen(rp_vscreen * v);

rp_window *vscreen_last_window_by_class(rp_vscreen * v, char *class);
rp_window *vscreen_last_window_by_class_complement(rp_vscreen * v,
                                                   char *class);

void vscreen_announce_current(rp_vscreen * v);

#define rp_current_vscreen	(rp_current_screen->current_vscreen)

extern int isu8char(char c);
extern int isu8start(char c);
extern int isu8cont(char c);

extern int utf8_locale;

int utf8_check_locale(void);

typedef enum edit_status {
    EDIT_INSERT,
    EDIT_DELETE,
    EDIT_MOVE,
    EDIT_COMPLETE,
    EDIT_ABORT,
    EDIT_DONE,
    EDIT_NO_OP
} edit_status;

/* Input line functions */
rp_input_line *input_line_new(char *prompt, char *preinput,
                              enum completion_styles style,
                              completion_fn fn);
void input_line_free(rp_input_line * line);

edit_status execute_edit_action(rp_input_line * line, KeySym ch,
                                unsigned int modifier, char *keysym_buf);

char *completions_complete(rp_completions * c, char *partial,
                           int direction);
rp_completions *completions_new(completion_fn list_fn,
                                enum completion_styles style);
void completions_free(rp_completions * c);

void hook_run(struct list_head *hook);
void hook_remove(struct list_head *hook, struct sbuf *s);
void hook_add(struct list_head *hook, struct sbuf *s);
struct list_head *hook_lookup(char *s);

void format_string(char *fmt, rp_window_elem * win_elem,
                   struct sbuf *buffer);

#define __dead	__attribute__((__noreturn__))

__dead void fatal(const char *msg);
void *xmalloc(size_t size);
void *xrealloc(void *ptr, size_t size);
char *xstrdup(const char *s);
char *xvsprintf(char *fmt, va_list ap);
char *xsprintf(char *fmt, ...);
char *strtok_ws(char *s);
int str_comp(char *s1, char *s2, size_t len);
void start_compositor(void);
char *expand_env_vars(const char *str);

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
void wallpaper_init(struct wallpaper_state *state, Display * dpy,
                    int screen);

/* Free wallpaper state resources */
void wallpaper_free(struct wallpaper_state *state);

/* Set the root window background pixmap */
int wallpaper_apply(struct wallpaper_state *state);

/* The structure returned by a command. */
typedef struct cmdret {
    char *output;
    int success;
} cmdret;

void clear_frame_undos(void);
cmdret *frestore(char *data, rp_vscreen * v);
char *fdump(rp_vscreen * vscreen);
rp_keymap *find_keymap(char *name);
void init_user_commands(void);
void initialize_default_keybindings(void);
cmdret *command(int interactive, char *data);
cmdret *cmdret_new(int success, char *fmt, ...);
void cmdret_free(cmdret * ret);
void free_user_commands(void);
void free_aliases(void);
void free_keymaps(void);
char *wingravity_to_string(int g);
rp_action *find_keybinding(KeySym keysym, unsigned int state,
                           rp_keymap * map);
rp_action *find_keybinding_by_action(char *action, rp_keymap * map);
int spawn(char *cmd, rp_frame * frame);
int vspawn(char *cmd, rp_frame * frame, rp_vscreen * vscreen);

void init_xrandr(void);
int xrandr_query_screen(int **outputs);
int xrandr_is_primary(rp_screen * screen);
void xrandr_fill_screen(int rr_output, rp_screen * screen);
void xrandr_notify(XEvent * ev);

#endif                          /* ! _POISON_H */
