/*
 * commoner - yet another X11 compositor
 *
 * Based on xcompmgr - copyright © 2003, Keith Packard
 *                                 2008, Dana Jansens
 *          compton                2011, Christopher Jeffrey
 *          fastcompmgr            2023, Tycho Kirchner
 *          commoner               2025, dancingmirrors
 */
#pragma once

#include <X11/Xlib.h>
#include <EGL/egl.h>
#include <GL/gl.h>
#include <stdbool.h>
#include <sys/time.h>
#include <string.h>
#include <stdlib.h>

/*
 * Ring Buffer Macros
 * Copyright © 2013 Philip Thrasher
 *
 * The MIT License (MIT)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef RINGBUFFER_USE_STATIC_MEMORY
#define RINGBUFFER_USE_STATIC_MEMORY 0
#endif

#ifndef RINGBUFFER_AVOID_MODULO
#define RINGBUFFER_AVOID_MODULO 0
#endif

#define ringBuffer_typedef(T, NAME) \
  typedef struct { \
    int size; \
    int start; \
    int end; \
    T* elems; \
  } NAME

#if RINGBUFFER_USE_STATIC_MEMORY == 1
#define bufferInit(BUF, S, T) \
  { \
    static T StaticBufMemory[S + 1];\
    BUF.elems = StaticBufMemory; \
  } \
    BUF.size = S; \
    BUF.start = 0; \
    BUF.end = 0;
#else

#define bufferInit(BUF, S, T) \
  do {          \
  (BUF).size = (S); \
  (BUF).start = 0; \
  (BUF).end = 0; \
  (BUF).elems = (T*)calloc((BUF).size + 1, sizeof(T)); \
  } while(0)

#define bufferIncrease(BUF, BUFSIZE)                                             \
  do {                                                                           \
  const size_t sizeof_t = sizeof( typeof(*(BUF)->elems));                        \
  typeof((BUF)->elems) ELEMS = (typeof((BUF)->elems))calloc(BUFSIZE+1,sizeof_t); \
  int NEW_END;                                                                   \
  if(isBufferEmpty(BUF)){                                                        \
    NEW_END = 0;                                                                 \
  } else {                                                                       \
    if((BUF)->start < (BUF)->end ){                                              \
      NEW_END = (BUF)->end - (BUF)->start;                                       \
      memcpy(ELEMS, &(BUF)->elems[(BUF)->start], NEW_END*sizeof_t);              \
    } else {                                                                     \
      int COUNT1 = ((BUF)->size + 1 - (BUF)->start);                             \
      int COUNT2 = (BUF)->end;                                                   \
      NEW_END = COUNT1 + COUNT2;                                                 \
      memcpy(ELEMS, &(BUF)->elems[(BUF)->start], COUNT1*sizeof_t);               \
      memcpy(&ELEMS[COUNT1], (BUF)->elems, COUNT2*sizeof_t);                     \
    }                                                                            \
  }                                                                              \
  free((BUF)->elems);                                                            \
  (BUF)->start = 0;                                                              \
  (BUF)->end = NEW_END;                                                          \
  (BUF)->elems = (ELEMS);                                                        \
  (BUF)->size = (BUFSIZE);                                                       \
} while(0)

#define bufferDestroy(BUF) \
  do {                     \
  free((BUF)->elems);      \
  } while(0)

#endif

#if RINGBUFFER_AVOID_MODULO == 1

#define nextStartIndex(BUF) (((BUF)->start != (BUF)->size) ? ((BUF)->start + 1) : 0)
#define nextEndIndex(BUF) (((BUF)->end != (BUF)->size) ? ((BUF)->end + 1) : 0)

#else

#define nextStartIndex(BUF) (((BUF)->start + 1) % ((BUF)->size + 1))
#define nextEndIndex(BUF) (((BUF)->end + 1) % ((BUF)->size + 1))

#endif

#define isBufferEmpty(BUF) ((BUF)->end == (BUF)->start)
#define isBufferFull(BUF) (nextEndIndex(BUF) == (BUF)->start)

#define bufferWritePeek(BUF) (BUF)->elems[(BUF)->end]
#define bufferWriteSkip(BUF) \
  do { \
  (BUF)->end = nextEndIndex(BUF); \
  if (isBufferEmpty(BUF)) { \
    (BUF)->start = nextStartIndex(BUF); \
  } \
  } while(0)

#define bufferReadPeek(BUF) (BUF)->elems[(BUF)->start]
#define bufferReadSkip(BUF) \
  do { \
  (BUF)->start = nextStartIndex(BUF); \
  } while(0)

#define bufferWrite(BUF, ELEM) \
  do { \
  bufferWritePeek(BUF) = ELEM; \
  bufferWriteSkip(BUF); \
  } while(0)

#define bufferRead(BUF, ELEM) \
  do { \
  ELEM = bufferReadPeek(BUF); \
  bufferReadSkip(BUF); \
  } while(0)

extern Atom atom_opacity;
extern Atom atom_win_type;
extern Atom atom_pixmap;
extern Atom atom_wm_state;
extern Atom atom_net_frame_extents;
extern Atom atom_gtk_frame_extents;
extern Display *g_dpy;
extern int g_screen;
extern Window root;
extern Window overlay_window;
extern int root_width;
extern int root_height;
extern const char *root_background_props[];
extern time_t _program_start_secs;

extern EGLDisplay egl_display;
extern EGLContext egl_context;
extern EGLSurface egl_surface;
extern GLuint root_fbo;
extern GLuint root_texture;

#define ACCESS_ONCE(x) (*(volatile typeof(x) *)&(x))

#define READ_ONCE(x) \
({ typeof(x) ___x = ACCESS_ONCE(x); ___x; })

#define WRITE_ONCE(x, val) \
do { ACCESS_ONCE(x) = (val); } while (0)

static inline int get_time_in_milliseconds()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec - _program_start_secs) * 1000 + tv.tv_usec / 1000;
}

static inline double normalize_d(double d)
{
    if (d > 1.0)
        return 1.0;
    if (d < 0.0)
        return 0.0;

    return d;
}

bool root_init();

typedef struct {
    short x1;
    short y1;
    short x2;
    short y2;
    short w;
    short h;
} CompRect;

bool rect_paint_needed(CompRect * ignore_reg, CompRect * reg);

void discard_ignore(unsigned long sequence);
void set_ignore(Display * dpy, unsigned long sequence);
int should_ignore(unsigned long sequence);
void usage(int exitcode);
