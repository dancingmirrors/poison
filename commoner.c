/*
 * commoner - yet another X11 compositor
 *
 * Based on xcompmgr - copyright © 2003, Keith Packard
 *                                 2008, Dana Jansens
 *          compton                2011, Christopher Jeffrey
 *          fastcompmgr            2023, Tycho Kirchner
 *          commoner               2025, dancingmirrors
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/Xmd.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xpresent.h>
#include <X11/extensions/sync.h>

#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include "commoner.h"

Atom atom_opacity;
Atom atom_win_type;
Atom atom_pixmap;
Atom atom_wm_state;
Atom atom_net_frame_extents;
Atom atom_gtk_frame_extents;
Atom atom_root_pmap;

Display *g_dpy = NULL;
int g_screen = 0;

time_t _program_start_secs = 0;

Window root;
Window overlay_window;
int root_width;
int root_height;

EGLDisplay egl_display;
EGLContext egl_context;
EGLSurface egl_surface;
EGLConfig egl_config;

static PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR_func = NULL;
static PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR_func = NULL;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC
    glEGLImageTargetTexture2DOES_func = NULL;
static Bool eglimage_supported = False;

GLuint root_fbo;
GLuint root_texture;
GLuint root_bg_texture = 0;
Pixmap root_bg_pixmap = None;
GLuint shader_program;
GLuint vao, vbo;

int present_opcode;
int present_event_base;
int present_error_base;
uint64_t present_serial;

const char *root_background_props[] = {
    "_XROOTPMAP_ID",
    "_XSETROOT_ID",
    0
};

static bool rect_contains(CompRect *r1, CompRect *r2)
{
    return r1->x1 <= r2->x1 && r1->y1 <= r2->y1 &&
        r1->x2 >= r2->x2 && r1->y2 >= r2->y2;
}

static bool rects_are_intersecting(CompRect *r1, CompRect *r2)
{
    if (r1->x1 > r2->x2 || r2->x1 > r1->x2) {
        return false;
    }
    if (r1->y1 > r2->y2 || r2->y1 > r1->y2) {
        return false;
    }
    return true;
}

bool rect_paint_needed(CompRect *ignore_reg, CompRect *reg)
{
    if (rect_contains(ignore_reg, reg)) {
        return false;
    }
    if (!rects_are_intersecting(ignore_reg, reg)) {
        if (reg->w * reg->h > ignore_reg->w * ignore_reg->h) {
            *ignore_reg = *reg;
        }
        return true;
    }

    short x1 = (ignore_reg->x1 > reg->x1) ? ignore_reg->x1 : reg->x1;
    short x2 = (ignore_reg->x2 < reg->x2) ? ignore_reg->x1 : reg->x1;
    short y1 = (ignore_reg->y1 > reg->y1) ? ignore_reg->y1 : reg->y1;
    short y2 = (ignore_reg->y2 < reg->y2) ? ignore_reg->y1 : reg->y1;
    short w = x2 - x1;
    short h = y2 - y1;

    if (reg->w * reg->h > ignore_reg->w * ignore_reg->h) {
        *ignore_reg = *reg;
    }
    if (w * h > ignore_reg->w * ignore_reg->h) {
        CompRect r_intersect = {.x1 = x1,.y1 = y1,
            .x2 = x2,.y2 = y2,
            .w = w,.h = h
        };
        *ignore_reg = r_intersect;
    }
    return true;
}

static inline int _get_valid_pixmap_depth(Pixmap pxmap)
{
    if (!pxmap)
        return 0;

    Window rroot = None;
    int rx = 0, ry = 0;
    unsigned rwid = 0, rhei = 0, rborder = 0, rdepth = 0;

    bool is_valid = XGetGeometry(g_dpy, pxmap, &rroot, &rx, &ry,
                                 &rwid, &rhei, &rborder, &rdepth) && rwid
        && rhei;
    if (is_valid) {
        return rdepth;
    }
    return 0;
}

static const char *vertex_shader_source =
    "#version 130\n"
    "in vec2 position;\n"
    "in vec2 texcoord;\n"
    "out vec2 v_texcoord;\n"
    "uniform mat4 projection;\n"
    "void main() {\n"
    "    gl_Position = projection * vec4(position, 0.0, 1.0);\n"
    "    v_texcoord = texcoord;\n" "}\n";

static const char *fragment_shader_source =
    "#version 130\n"
    "in vec2 v_texcoord;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D tex;\n"
    "uniform float alpha;\n"
    "void main() {\n"
    "    vec4 color = texture(tex, v_texcoord);\n"
    "    \n" "    \n" "    fragColor = color * alpha;\n" "}\n";

static GLuint compile_shader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint status;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "Shader compilation error: %s\n", log);
        return 0;
    }
    return shader;
}

static bool init_gl_shaders()
{
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_shader_source);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_source);

    if (!vs || !fs) {
        return false;
    }

    shader_program = glCreateProgram();
    glAttachShader(shader_program, vs);
    glAttachShader(shader_program, fs);
    glLinkProgram(shader_program);

    GLint status;
    glGetProgramiv(shader_program, GL_LINK_STATUS, &status);
    if (!status) {
        char log[512];
        glGetProgramInfoLog(shader_program, sizeof(log), NULL, log);
        fprintf(stderr, "Program linking error: %s\n", log);
        return false;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    float vertices[] = {
        0.0f, 0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 1.0f, 0.0f,
        1.0f, 1.0f, 1.0f, 1.0f,
        0.0f, 0.0f, 0.0f, 0.0f,
        1.0f, 1.0f, 1.0f, 1.0f,
        0.0f, 1.0f, 0.0f, 1.0f,
    };

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices,
                 GL_STATIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void *) 0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void *) (2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);

    return true;
}

static bool init_egl()
{
    egl_display = eglGetDisplay((EGLNativeDisplayType) g_dpy);
    if (egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "Failed to get EGL display\n");
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize(egl_display, &major, &minor)) {
        fprintf(stderr, "Failed to initialize EGL\n");
        return false;
    }

    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0,
        EGL_STENCIL_SIZE, 0,
        EGL_NONE
    };

    EGLint num_configs;
    if (!eglChooseConfig
        (egl_display, config_attribs, &egl_config, 1, &num_configs)) {
        fprintf(stderr, "Failed to choose EGL config\n");
        return false;
    }

    eglBindAPI(EGL_OPENGL_API);

    const EGLint context_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 0,
        EGL_NONE
    };

    egl_context =
        eglCreateContext(egl_display, egl_config, EGL_NO_CONTEXT,
                         context_attribs);
    if (egl_context == EGL_NO_CONTEXT) {
        fprintf(stderr, "Failed to create EGL context\n");
        return false;
    }

    overlay_window = XCompositeGetOverlayWindow(g_dpy, root);
    XSelectInput(g_dpy, overlay_window, ExposureMask);

    XserverRegion region = XFixesCreateRegion(g_dpy, NULL, 0);
    XFixesSetWindowShapeRegion(g_dpy, overlay_window, ShapeInput, 0, 0,
                               region);
    XFixesDestroyRegion(g_dpy, region);

    egl_surface =
        eglCreateWindowSurface(egl_display, egl_config, overlay_window,
                               NULL);
    if (egl_surface == EGL_NO_SURFACE) {
        fprintf(stderr, "Failed to create EGL surface\n");
        return false;
    }

    if (!eglMakeCurrent
        (egl_display, egl_surface, egl_surface, egl_context)) {
        fprintf(stderr, "Failed to make EGL context current\n");
        return false;
    }

    const char *egl_extensions =
        eglQueryString(egl_display, EGL_EXTENSIONS);
    if (egl_extensions && strstr(egl_extensions, "EGL_KHR_image_pixmap")) {
        eglCreateImageKHR_func = (PFNEGLCREATEIMAGEKHRPROC)
            eglGetProcAddress("eglCreateImageKHR");
        eglDestroyImageKHR_func = (PFNEGLDESTROYIMAGEKHRPROC)
            eglGetProcAddress("eglDestroyImageKHR");
        glEGLImageTargetTexture2DOES_func =
            (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
            eglGetProcAddress("glEGLImageTargetTexture2DOES");

        if (eglCreateImageKHR_func && eglDestroyImageKHR_func &&
            glEGLImageTargetTexture2DOES_func) {
            eglimage_supported = True;
            fprintf(stderr,
                    "EGLImage support enabled for zero-copy textures\n");
        }
    }

    if (!eglimage_supported) {
        fprintf(stderr,
                "Warning: EGLImage not supported, falling back to XGetImage (slower)\n");
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);

    if (!init_gl_shaders()) {
        return false;
    }

    glGenFramebuffers(1, &root_fbo);
    glGenTextures(1, &root_texture);

    glBindTexture(GL_TEXTURE_2D, root_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, root_width, root_height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glBindFramebuffer(GL_FRAMEBUFFER, root_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, root_texture, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
        GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "Framebuffer incomplete\n");
        return false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    return true;
}

bool root_init()
{
    root_width = DisplayWidth(g_dpy, g_screen);
    root_height = DisplayHeight(g_dpy, g_screen);

    if (!init_egl()) {
        return false;
    }

    return true;
}

#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)

#if COMPOSITE_MAJOR > 0 || COMPOSITE_MINOR >= 2
#define HAS_NAME_WINDOW_PIXMAP 1
#endif

#define CAN_DO_USABLE 0

typedef enum {
    WINTYPE_UNKNOWN,
    WINTYPE_DESKTOP,
    WINTYPE_DOCK,
    WINTYPE_TOOLBAR,
    WINTYPE_MENU,
    WINTYPE_UTILITY,
    WINTYPE_SPLASH,
    WINTYPE_DIALOG,
    WINTYPE_NORMAL,
    WINTYPE_DROPDOWN_MENU,
    WINTYPE_POPUP_MENU,
    WINTYPE_TOOLTIP,
    WINTYPE_NOTIFY,
    WINTYPE_COMBO,
    WINTYPE_DND,
    NUM_WINTYPES
} wintype;

typedef enum {
    SHADOW_UNKNOWN,
    SHADOW_YES,
    SHADOW_NO
} shadowtype;

typedef struct _ignore {
    struct _ignore *next;
    unsigned long sequence;
} ignore;

typedef struct _win {
    struct _win *next;
    Window id;
#if HAS_NAME_WINDOW_PIXMAP
    Pixmap pixmap;
#endif
    XWindowAttributes a;
#if CAN_DO_USABLE
    Bool usable;
    XRectangle damage_bounds;
#endif
    int mode;
    int damaged;
    Damage damage;
    GLuint texture;
    EGLImageKHR egl_image;
    GLuint shadow_texture;
    XserverRegion border_size;
    XserverRegion extents;
    int shadow_dx;
    int shadow_dy;
    int shadow_width;
    int shadow_height;
    unsigned int opacity;
    unsigned int target_opacity;
    Bool fade_finished;
    wintype window_type;
    shadowtype shadow_type;
    unsigned long damage_sequence;
    Bool destroyed;
    Bool paint_needed;
    unsigned int left_width;
    unsigned int right_width;
    unsigned int top_width;
    unsigned int bottom_width;

    Bool need_configure;
    bool configure_size_changed;
    XConfigureEvent queue_configure;

    XserverRegion border_clip;
    struct _win *prev_trans;
} win;

typedef struct _conv {
    int size;
    double *data;
} conv;

win *list;
Display *dpy;
GLuint shadow_texture_cache;
XserverRegion all_damage;
XserverRegion g_xregion_tmp;
Bool all_damage_is_dirty;
Bool clip_changed;
#if HAS_NAME_WINDOW_PIXMAP
Bool has_name_pixmap;
#endif
ringBuffer_typedef(ulong, IgnoreErrRingbuf);
IgnoreErrRingbuf ignore_ringbuf;
IgnoreErrRingbuf *p_ignore_ringbuf = &ignore_ringbuf;
int xfixes_event, xfixes_error;
int damage_event, damage_error;
int composite_event, composite_error;
int shape_event, shape_error;
int composite_opcode;
static Bool g_paint_ignore_region_is_dirty = True;

Atom win_type[NUM_WINTYPES];
double win_type_opacity[NUM_WINTYPES];
Bool win_type_shadow[NUM_WINTYPES];

#define REGISTER_PROP "_NET_WM_CM_S"

#define OPAQUE 0xffffffff

conv *gaussian_map;

#define WINDOW_SOLID 0
#define WINDOW_TRANS 1
#define WINDOW_ARGB 2

#ifndef MONITOR_REPAINT
#define MONITOR_REPAINT 0
#endif

#define CONFIGURE_TIMEOUT_MS 2

static void determine_mode(Display * dpy, win * w);
static bool is_gtk_frame_extent(Display * dpy, Window w);

static void do_configure_win(Display * dpy, win * w);
static void set_paint_ignore_region_dirty(void);

static void add_damage(Display * dpy, XserverRegion damage);

static XserverRegion win_extents(Display * dpy, win * w);

static void finish_unmap_win(Display * dpy, win * w);
static void apply_opacity_change(Display * dpy, win * w);
static void
set_target_opacity(Display * dpy, win * w, unsigned long target);
static void fade_step(Display * dpy, win * w);
static void gl_draw_texture(GLuint texture, int x, int y, int width,
                            int height, float alpha);

int shadow_radius = 12;
int shadow_offset_x = -15;
int shadow_offset_y = -15;
double shadow_opacity = .75;
double inactive_opacity = 0;

Bool fade_enabled = True;
double fade_in_step = 0.06;
double fade_out_step = 0.07;
int fade_delta = 8;
Bool fade_debug = False;

Bool redirected = True;
Bool unredir_fullscreen = False;

static Bool should_unredir = False;
static Bool should_redir = False;

static void redir_start(Display *dpy)
{
    if (!redirected) {
        XMapWindow(dpy, overlay_window);

        XCompositeRedirectSubwindows(dpy, root, CompositeRedirectManual);
        XSync(dpy, False);
        redirected = True;

        for (win * w = list; w; w = w->next) {
            if (w->damage != None) {
                set_ignore(dpy, NextRequest(dpy));
                XDamageSubtract(dpy, w->damage, None, None);
            }
        }

        XRectangle root_rect = {.x = 0,.y = 0,
            .width = root_width,.height = root_height
        };
        XFixesSetRegion(dpy, g_xregion_tmp, &root_rect, 1);
        add_damage(dpy, g_xregion_tmp);

        if (fade_debug) {
            fprintf(stderr,
                    "[unredir] Re-enabled compositor (redirected all windows, mapped overlay)\n");
        }
    }
}

static void redir_stop(Display *dpy)
{
    if (redirected) {
        for (win * w = list; w; w = w->next) {
#if HAS_NAME_WINDOW_PIXMAP
            if (w->pixmap) {
                XFreePixmap(dpy, w->pixmap);
                w->pixmap = None;
            }
#endif
            if (w->texture) {
                glDeleteTextures(1, &w->texture);
                w->texture = 0;
            }
            if (w->egl_image && eglimage_supported) {
                eglDestroyImageKHR_func(egl_display, w->egl_image);
                w->egl_image = NULL;
            }
        }

        XCompositeUnredirectSubwindows(dpy, root, CompositeRedirectManual);

        XUnmapWindow(dpy, overlay_window);
        XSync(dpy, False);
        redirected = False;

        if (fade_debug) {
            fprintf(stderr,
                    "[unredir] Disabled compositor (unredirected all windows, unmapped overlay)\n");
        }
    }
}

#define INACTIVE_OPACITY \
(unsigned long)((double)inactive_opacity * OPAQUE)

#define IS_NORMAL_WIN(w) \
((w) && ((w)->window_type == WINTYPE_NORMAL \
         || (w)->window_type == WINTYPE_UTILITY))

int Gsize = -1;
unsigned char *shadow_corner = NULL;
unsigned char *shadow_top = NULL;

static double gaussian(double r, double x, double y)
{
    return ((1 / (sqrt(2 * M_PI * r))) *
            exp((-(x * x + y * y)) / (2 * r * r)));
}

static conv *make_gaussian_map(Display *dpy, double r)
{
    conv *c;
    int size = ((int) ceil((r * 3)) + 1) & ~1;
    int center = size / 2;
    int x, y;
    double t;
    double g;
    (void) dpy;

    c = malloc(sizeof(conv) + size * size * sizeof(double));
    c->size = size;
    c->data = (double *) (c + 1);
    t = 0.0;

    for (y = 0; y < size; y++) {
        for (x = 0; x < size; x++) {
            g = gaussian(r, (double) (x - center), (double) (y - center));
            t += g;
            c->data[y * size + x] = g;
        }
    }

    for (y = 0; y < size; y++) {
        for (x = 0; x < size; x++) {
            c->data[y * size + x] /= t;
        }
    }

    return c;
}

static unsigned char
sum_gaussian(conv *map, double opacity,
             int x, int y, int width, int height)
{
    int fx, fy;
    double *g_data;
    double *g_line = map->data;
    int g_size = map->size;
    int center = g_size / 2;
    int fx_start, fx_end;
    int fy_start, fy_end;
    double v;

    fx_start = center - x;
    if (fx_start < 0)
        fx_start = 0;
    fx_end = width + center - x;
    if (fx_end > g_size)
        fx_end = g_size;

    fy_start = center - y;
    if (fy_start < 0)
        fy_start = 0;
    fy_end = height + center - y;
    if (fy_end > g_size)
        fy_end = g_size;

    g_line = g_line + fy_start * g_size + fx_start;

    v = 0;

    for (fy = fy_start; fy < fy_end; fy++) {
        g_data = g_line;
        g_line += g_size;

        for (fx = fx_start; fx < fx_end; fx++) {
            v += *g_data++;
        }
    }

    if (v > 1)
        v = 1;

    return ((unsigned char) (v * opacity * 255.0));
}

static void presum_gaussian(conv *map)
{
    int center = map->size / 2;
    int opacity, x, y;

    Gsize = map->size;

    if (shadow_corner)
        free((void *) shadow_corner);
    if (shadow_top)
        free((void *) shadow_top);

    shadow_corner =
        (unsigned char *) (malloc((Gsize + 1) * (Gsize + 1) * 26));
    shadow_top = (unsigned char *) (malloc((Gsize + 1) * 26));

    for (x = 0; x <= Gsize; x++) {
        shadow_top[25 * (Gsize + 1) + x] =
            sum_gaussian(map, 1, x - center, center, Gsize * 2, Gsize * 2);

        for (opacity = 0; opacity < 25; opacity++) {
            shadow_top[opacity * (Gsize + 1) + x] =
                shadow_top[25 * (Gsize + 1) + x] * opacity / 25;
        }

        for (y = 0; y <= x; y++) {
            shadow_corner[25 * (Gsize + 1) * (Gsize + 1) +
                          y * (Gsize + 1) + x]
                = sum_gaussian(map, 1, x - center, y - center, Gsize * 2,
                               Gsize * 2);
            shadow_corner[25 * (Gsize + 1) * (Gsize + 1) +
                          x * (Gsize + 1) + y]
                = shadow_corner[25 * (Gsize + 1) * (Gsize + 1) +
                                y * (Gsize + 1) + x];

            for (opacity = 0; opacity < 25; opacity++) {
                shadow_corner[opacity * (Gsize + 1) * (Gsize + 1)
                              + y * (Gsize + 1) + x]
                    = shadow_corner[opacity * (Gsize + 1) * (Gsize + 1)
                                    + x * (Gsize + 1) + y]
                    = shadow_corner[25 * (Gsize + 1) * (Gsize + 1)
                                    + y * (Gsize + 1) + x] * opacity / 25;
            }
        }
    }
}

static XImage *make_shadow(Display *dpy, double opacity,
                           int width, int height)
{
    XImage *ximage;
    unsigned char *data;
    int gsize = gaussian_map->size;
    int ylimit, xlimit;
    int swidth = width + gsize;
    int sheight = height + gsize;
    int center = gsize / 2;
    int x, y;
    unsigned char d;
    int x_diff;
    int opacity_int = (int) (opacity * 25);

    data = malloc(swidth * sheight * sizeof(unsigned char));
    if (!data)
        return 0;

    ximage = XCreateImage(dpy, DefaultVisual(dpy, DefaultScreen(dpy)), 8,
                          ZPixmap, 0, (char *) data, swidth, sheight, 8,
                          swidth * sizeof(unsigned char));

    if (!ximage) {
        free(data);
        return 0;
    }

    if (Gsize > 0) {
        d = shadow_top[opacity_int * (Gsize + 1) + Gsize];
    } else {
        d = sum_gaussian(gaussian_map,
                         opacity, center, center, width, height);
    }

    memset(data, d, sheight * swidth);

    ylimit = gsize;
    if (ylimit > sheight / 2)
        ylimit = (sheight + 1) / 2;

    xlimit = gsize;
    if (xlimit > swidth / 2)
        xlimit = (swidth + 1) / 2;

    for (y = 0; y < ylimit; y++)
        for (x = 0; x < xlimit; x++) {
            if (xlimit == Gsize && ylimit == Gsize) {
                d = shadow_corner[opacity_int * (Gsize + 1) * (Gsize + 1)
                                  + y * (Gsize + 1) + x];
            } else {
                d = sum_gaussian(gaussian_map,
                                 opacity, x - center, y - center, width,
                                 height);
            }
            data[y * swidth + x] = d;
            data[(sheight - y - 1) * swidth + x] = d;
            data[(sheight - y - 1) * swidth + (swidth - x - 1)] = d;
            data[y * swidth + (swidth - x - 1)] = d;
        }

    x_diff = swidth - (gsize * 2);
    if (x_diff > 0 && ylimit > 0) {
        for (y = 0; y < ylimit; y++) {
            if (ylimit == Gsize) {
                d = shadow_top[opacity_int * (Gsize + 1) + y];
            } else {
                d = sum_gaussian(gaussian_map,
                                 opacity, center, y - center, width,
                                 height);
            }
            memset(&data[y * swidth + gsize], d, x_diff);
            memset(&data[(sheight - y - 1) * swidth + gsize], d, x_diff);
        }
    }

    for (x = 0; x < xlimit; x++) {
        if (xlimit == Gsize) {
            d = shadow_top[opacity_int * (Gsize + 1) + x];
        } else {
            d = sum_gaussian(gaussian_map,
                             opacity, x - center, center, width, height);
        }
        for (y = gsize; y < sheight - gsize; y++) {
            data[y * swidth + x] = d;
            data[y * swidth + (swidth - x - 1)] = d;
        }
    }

    return ximage;
}

static GLuint
create_shadow_texture(Display *dpy, double opacity, int width, int height,
                      int *wp, int *hp)
{
    XImage *shadowImage;
    GLuint texture;
    (void) dpy;

    shadowImage = make_shadow(dpy, opacity, width, height);
    if (!shadowImage)
        return 0;

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, shadowImage->width,
                 shadowImage->height, 0, GL_RED, GL_UNSIGNED_BYTE,
                 shadowImage->data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    *wp = shadowImage->width;
    *hp = shadowImage->height;
    XDestroyImage(shadowImage);

    return texture;
}

void discard_ignore(unsigned long sequence)
{
    while (!isBufferEmpty(p_ignore_ringbuf)) {
        ulong buf_seq;
        buf_seq = bufferReadPeek(p_ignore_ringbuf);
        if ((long) (sequence - buf_seq) > 0) {
            bufferReadSkip(p_ignore_ringbuf);
        } else {
            break;
        }
    }
}

void set_ignore(Display *dpy, unsigned long sequence)
{
    (void) dpy;
    if (unlikely(isBufferFull(p_ignore_ringbuf))) {
        bufferIncrease(p_ignore_ringbuf, p_ignore_ringbuf->size * 2);
    }
    bufferWrite(p_ignore_ringbuf, sequence);
}

int should_ignore(unsigned long sequence)
{
    ulong buf_seq;
    discard_ignore(sequence);
    if (isBufferEmpty(p_ignore_ringbuf))
        return False;
    buf_seq = bufferReadPeek(p_ignore_ringbuf);
    return buf_seq == sequence;
}

static win *find_win(Window id)
{
    win *w;

    for (w = list; w; w = w->next) {
        if (w->id == id && !w->destroyed)
            return w;
    }

    return 0;
}

static void update_root_background(Display *dpy)
{
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;
    Pixmap new_pixmap = None;

    if (XGetWindowProperty(dpy, root, atom_root_pmap, 0, 1, False,
                           XA_PIXMAP, &actual_type, &actual_format,
                           &nitems, &bytes_after, &prop) == Success) {
        if (prop) {
            if (actual_type == XA_PIXMAP && nitems == 1) {
                memcpy(&new_pixmap, prop, sizeof(Pixmap));
            }
            XFree(prop);
        }
    }
    if (new_pixmap == root_bg_pixmap && root_bg_texture != 0) {
        return;
    }
    if (root_bg_texture != 0) {
        glDeleteTextures(1, &root_bg_texture);
        root_bg_texture = 0;
    }

    root_bg_pixmap = new_pixmap;

    if (new_pixmap != None) {
        Window root_ret;
        int x, y;
        unsigned int width, height, border, depth;

        if (XGetGeometry(dpy, new_pixmap, &root_ret, &x, &y,
                         &width, &height, &border, &depth)) {

            if (fade_debug) {
                fprintf(stderr,
                        "[update_root_background] Pixmap 0x%lx geometry: %ux%u depth=%u (screen: %dx%d)\n",
                        new_pixmap, width, height, depth, root_width,
                        root_height);
            }
            if (width == (unsigned int) root_width
                && height == (unsigned int) root_height) {
                XImage *image =
                    XGetImage(dpy, new_pixmap, 0, 0, width, height,
                              AllPlanes, ZPixmap);
                if (image) {
                    glGenTextures(1, &root_bg_texture);
                    glBindTexture(GL_TEXTURE_2D, root_bg_texture);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                                    GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                                    GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                                    GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                                    GL_CLAMP_TO_EDGE);

                    GLenum format = GL_RGBA;
                    GLenum internal_format = GL_RGBA8;

                    if (image->bits_per_pixel == 32) {
                        format =
                            (image->byte_order ==
                             LSBFirst) ? GL_BGRA : GL_RGBA;
                        internal_format = GL_RGBA8;
                        if (depth == 24) {
                            unsigned char *data =
                                (unsigned char *) image->data;
                            int alpha_offset =
                                (image->byte_order == LSBFirst) ? 3 : 0;
                            for (unsigned int i = 0; i < width * height;
                                 i++) {
                                data[i * 4 + alpha_offset] = 0xFF;
                            }
                        }
                    } else if (image->bits_per_pixel == 24) {
                        format =
                            (image->byte_order ==
                             LSBFirst) ? GL_BGR : GL_RGB;
                        internal_format = GL_RGB8;
                    }

                    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width,
                                 height, 0, format, GL_UNSIGNED_BYTE,
                                 image->data);

                    XDestroyImage(image);

                    if (fade_debug) {
                        const char *format_str =
                            format == GL_BGRA ? "BGRA" :
                            format == GL_RGBA ? "RGBA" :
                            format == GL_BGR ? "BGR" : "RGB";
                        const char *internal_str =
                            internal_format == GL_RGBA8 ? "RGBA8" : "RGB8";
                        fprintf(stderr,
                                "[update_root_background] Created texture from pixmap 0x%lx (%ux%u depth=%u bpp=%d format=%s internal=%s)\n",
                                new_pixmap, width, height, depth,
                                image->bits_per_pixel, format_str,
                                internal_str);
                    }
                } else {
                    if (fade_debug) {
                        fprintf(stderr,
                                "[update_root_background] Failed to get XImage from pixmap 0x%lx\n",
                                new_pixmap);
                    }
                }
            } else {
                if (fade_debug) {
                    fprintf(stderr,
                            "[update_root_background] Pixmap size mismatch, waiting for properly sized pixmap\n");
                }
            }
        }
    }
}

static void paint_root(Display *dpy)
{
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (root_bg_texture != 0) {
        gl_draw_texture(root_bg_texture, 0, 0, root_width, root_height,
                        1.0f);
    } else {
        update_root_background(dpy);
    }
}

static void gl_draw_texture(GLuint texture, int x, int y, int width,
                            int height, float alpha)
{
    if (!texture)
        return;

    if (fade_debug && alpha < 1.0f) {
        GLboolean blend_enabled;
        GLint blend_src_rgb, blend_dst_rgb, blend_src_alpha,
            blend_dst_alpha;
        glGetBooleanv(GL_BLEND, &blend_enabled);
        glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_alpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_alpha);
        fprintf(stderr,
                "### GL_DRAW ### texture=%u pos=(%d,%d) size=%dx%d alpha=%.3f blend=%d src_rgb=%d dst_rgb=%d src_a=%d dst_a=%d\n",
                texture, x, y, width, height, alpha, blend_enabled,
                blend_src_rgb, blend_dst_rgb, blend_src_alpha,
                blend_dst_alpha);
    }

    glUseProgram(shader_program);
    glBindTexture(GL_TEXTURE_2D, texture);

    float projection[16] = {
        2.0f / root_width, 0.0f, 0.0f, 0.0f,
        0.0f, -2.0f / root_height, 0.0f, 0.0f,
        0.0f, 0.0f, -1.0f, 0.0f,
        -1.0f, 1.0f, 0.0f, 1.0f
    };

    GLint proj_loc = glGetUniformLocation(shader_program, "projection");
    glUniformMatrix4fv(proj_loc, 1, GL_FALSE, projection);

    GLint alpha_loc = glGetUniformLocation(shader_program, "alpha");
    glUniform1f(alpha_loc, alpha);

    float vertices[] = {
        (float) x, (float) y, 0.0f, 0.0f,
        (float) (x + width), (float) y, 1.0f, 0.0f,
        (float) (x + width), (float) (y + height), 1.0f, 1.0f,
        (float) x, (float) y, 0.0f, 0.0f,
        (float) (x + width), (float) (y + height), 1.0f, 1.0f,
        (float) x, (float) (y + height), 0.0f, 1.0f,
    };

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);

    glDrawArrays(GL_TRIANGLES, 0, 6);

    glBindVertexArray(0);
}

static void update_window_texture(Display *dpy, win *w)
{
    if (!w->pixmap)
        return;

    Window root_ret;
    int x, y;
    unsigned int width, height, border, depth;
    if (!XGetGeometry
        (dpy, w->pixmap, &root_ret, &x, &y, &width, &height, &border,
         &depth)) {
        return;
    }

    if (!w->texture) {
        glGenTextures(1, &w->texture);
        glBindTexture(GL_TEXTURE_2D, w->texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                        GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                        GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, w->texture);
    }

    if (eglimage_supported) {
        if (w->egl_image) {
            eglDestroyImageKHR_func(egl_display, w->egl_image);
            w->egl_image = NULL;
        }

        const EGLint pixmap_attribs[] = {
            EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
            EGL_NONE
        };

        w->egl_image = eglCreateImageKHR_func(egl_display,
                                              EGL_NO_CONTEXT,
                                              EGL_NATIVE_PIXMAP_KHR,
                                              (EGLClientBuffer) (uintptr_t)
                                              w->pixmap, pixmap_attribs);

        if (w->egl_image != EGL_NO_IMAGE_KHR) {
            glEGLImageTargetTexture2DOES_func(GL_TEXTURE_2D, w->egl_image);
            return;
        } else {
            if (fade_debug) {
                fprintf(stderr,
                        "[update_texture] Failed to create EGLImage for window 0x%lx, falling back to XGetImage\n",
                        w->id);
            }
        }
    }

    XImage *image =
        XGetImage(dpy, w->pixmap, 0, 0, width, height, AllPlanes, ZPixmap);
    if (!image) {
        if (fade_debug) {
            fprintf(stderr,
                    "[update_texture] Failed to get image for window 0x%lx\n",
                    w->id);
        }
        return;
    }

    GLenum format = GL_RGBA;
    GLenum internal_format = GL_RGBA8;

    if (depth == 32) {
        internal_format = GL_RGBA8;
        if (image->byte_order == LSBFirst) {
            format = GL_BGRA;
        } else {
            format = GL_RGBA;
        }

        if (w->mode != WINDOW_ARGB && image->bits_per_pixel == 32) {
            unsigned char *data = (unsigned char *) image->data;
            int alpha_offset = (image->byte_order == LSBFirst) ? 3 : 0;
            for (unsigned int i = 0; i < width * height; i++) {
                data[i * 4 + alpha_offset] = 0xFF;
            }
        }
    } else if (depth == 24) {
        internal_format = GL_RGBA8;
        if (image->byte_order == LSBFirst) {
            format = GL_BGRA;
        } else {
            format = GL_RGBA;
        }

        if (image->bits_per_pixel == 32) {
            unsigned char *data = (unsigned char *) image->data;
            int alpha_offset = (image->byte_order == LSBFirst) ? 3 : 0;
            for (unsigned int i = 0; i < width * height; i++) {
                data[i * 4 + alpha_offset] = 0xFF;
            }
        }
    } else {
        if (fade_debug) {
            fprintf(stderr,
                    "[update_texture] Unsupported depth %u for window 0x%lx\n",
                    depth, w->id);
        }
        XDestroyImage(image);
        return;
    }

    if (fade_debug) {
        unsigned int alpha_sample = 0;
        if (image->bits_per_pixel == 32 && width > 0 && height > 0) {
            int alpha_offset = (image->byte_order == LSBFirst) ? 3 : 0;
            alpha_sample = ((unsigned char *) image->data)[alpha_offset];
        }
        fprintf(stderr,
                "### TEXTURE_DEBUG ### window=0x%lx size=%ux%u depth=%u bpp=%d format=%s alpha_sample=%u mode=%d\n",
                w->id, width, height, depth, image->bits_per_pixel,
                format == GL_BGRA ? "BGRA" : format ==
                GL_BGR ? "BGR" : format == GL_RGBA ? "RGBA" : "RGB",
                alpha_sample, w->mode);
    }

    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0,
                 format, GL_UNSIGNED_BYTE, image->data);

    XDestroyImage(image);
}

static XserverRegion win_extents(Display *dpy, win *w)
{
    XRectangle r;

    r.x = w->a.x;
    r.y = w->a.y;
    r.width = w->a.width + w->a.border_width * 2;
    r.height = w->a.height + w->a.border_width * 2;

    if (unlikely(w->shadow_type == SHADOW_UNKNOWN)) {
        bool shadow_yes = (likely(w->window_type)
                           && win_type_shadow[w->window_type] &&
                           (!w->a.override_redirect
                            || w->window_type != WINTYPE_NORMAL)
                           && !is_gtk_frame_extent(dpy, w->id));

        if (w->mode != WINDOW_SOLID) {
            switch (w->window_type) {
            case WINTYPE_NORMAL:
            case WINTYPE_DIALOG:
            case WINTYPE_DOCK:
                shadow_yes = shadow_yes && true;
                break;
            default:
                shadow_yes = false;
            }
        }
        w->shadow_type = (shadow_yes) ? SHADOW_YES : SHADOW_NO;
    }

    if (w->shadow_type == SHADOW_YES) {
        XRectangle sr;

        w->shadow_dx = shadow_offset_x;
        w->shadow_dy = shadow_offset_y;

        if (!w->shadow_texture) {
            double opacity = shadow_opacity;

            if (w->mode != WINDOW_SOLID) {
                opacity =
                    opacity * ((double) w->opacity) / ((double) OPAQUE);
            }

            w->shadow_texture = create_shadow_texture(dpy, opacity,
                                                      w->a.width +
                                                      w->a.border_width *
                                                      2,
                                                      w->a.height +
                                                      w->a.border_width *
                                                      2, &w->shadow_width,
                                                      &w->shadow_height);
        }

        sr.x = w->a.x + w->shadow_dx;
        sr.y = w->a.y + w->shadow_dy;
        sr.width = w->shadow_width;
        sr.height = w->shadow_height;

        if (sr.x < r.x) {
            r.width = (r.x + r.width) - sr.x;
            r.x = sr.x;
        }

        if (sr.y < r.y) {
            r.height = (r.y + r.height) - sr.y;
            r.y = sr.y;
        }

        if (sr.x + sr.width > r.x + r.width) {
            r.width = sr.x + sr.width - r.x;
        }

        if (sr.y + sr.height > r.y + r.height) {
            r.height = sr.y + sr.height - r.y;
        }
    }
    if (!w->extents) {
        w->extents = XFixesCreateRegion(dpy, &r, 1);
    } else {
        XFixesSetRegion(dpy, w->extents, &r, 1);
    }

    return w->extents;
}

static XserverRegion border_size(Display *dpy, win *w)
{
    XserverRegion border;

    set_ignore(dpy, NextRequest(dpy));
    border =
        XFixesCreateRegionFromWindow(dpy, w->id, WindowRegionBounding);

    set_ignore(dpy, NextRequest(dpy));
    XFixesTranslateRegion(dpy, border,
                          w->a.x + w->a.border_width,
                          w->a.y + w->a.border_width);

    return border;
}

static Window find_client_win(Display *dpy, Window win)
{
    Window root, parent;
    Window *children;
    unsigned int nchildren;
    unsigned int i;
    Atom type = None;
    int format;
    unsigned long nitems, after;
    unsigned char *data = NULL;
    Window client = 0;
    int res;

    res = XGetWindowProperty(dpy, win, atom_wm_state, 0, 0, False,
                             AnyPropertyType, &type, &format, &nitems,
                             &after, &data);
    if (likely(res == Success && data != NULL)) {
        XFree(data);
        if (likely(type))
            return win;
    }

    if (!XQueryTree(dpy, win, &root, &parent, &children, &nchildren)) {
        return 0;
    }

    for (i = 0; i < nchildren; i++) {
        client = find_client_win(dpy, children[i]);
        if (client)
            break;
    }

    if (children)
        XFree((char *) children);

    return client;
}

static void
get_frame_extents(Display *dpy, Window w,
                  unsigned int *left,
                  unsigned int *right,
                  unsigned int *top, unsigned int *bottom)
{
    long *extents;
    Atom type;
    int format;
    unsigned long nitems, after;
    unsigned char *data = NULL;
    int result;

    *left = 0;
    *right = 0;
    *top = 0;
    *bottom = 0;

    w = find_client_win(dpy, w);
    if (!w)
        return;

    result = XGetWindowProperty(dpy, w, atom_net_frame_extents,
                                0L, 4L, False, AnyPropertyType,
                                &type, &format, &nitems, &after,
                                (unsigned char **) &data);

    if (result == Success) {
        if (nitems == 4 && after == 0) {
            extents = (long *) data;
            *left = (unsigned int) extents[0];
            *right = (unsigned int) extents[1];
            *top = (unsigned int) extents[2];
            *bottom = (unsigned int) extents[3];
        }
        XFree(data);
    }
}

static Bool win_paint_needed(win *w, CompRect *ignore_reg)
{
    if (unlikely(w->a.x + w->a.width < 1 || w->a.y + w->a.height < 1
                 || w->a.x >= root_width || w->a.y >= root_height)) {
        return False;
    }

    if (w->a.map_state != IsViewable || w->destroyed
        || w->opacity != OPAQUE || w->a.override_redirect) {
        return True;
    }
    CompRect w_rect = {.x1 = w->a.x,.y1 = w->a.y,
        .x2 = w->a.x + w->a.width,.y2 = w->a.y + w->a.height,
        .w = w->a.width,.h = w->a.height
    };
    return rect_paint_needed(ignore_reg, &w_rect);
}

static inline Bool is_fullscreen(win *w)
{
    return (w->a.x <= 0 && w->a.y <= 0
            && (w->a.x + w->a.width + w->a.border_width * 2) >= root_width
            && (w->a.y + w->a.height + w->a.border_width * 2) >=
            root_height);
}

static void check_unredirect(Display *dpy)
{
    (void) dpy;
    if (!unredir_fullscreen)
        return;

    Bool unredir_possible = False;
    win *w;

    for (w = list; w; w = w->next) {
        if (w->a.map_state == IsViewable && !w->destroyed &&
            w->opacity == OPAQUE && is_fullscreen(w)) {
            if (w->window_type == WINTYPE_SPLASH ||
                w->window_type == WINTYPE_TOOLTIP ||
                w->window_type == WINTYPE_NOTIFY ||
                w->window_type == WINTYPE_MENU ||
                w->window_type == WINTYPE_DROPDOWN_MENU ||
                w->window_type == WINTYPE_POPUP_MENU ||
                w->window_type == WINTYPE_COMBO ||
                w->window_type == WINTYPE_DND) {
                continue;
            }
            unredir_possible = True;
            break;
        }
    }

    if (unredir_possible) {
        if (redirected) {
            should_unredir = True;
        }
    } else {
        if (!redirected) {
            should_redir = True;
        }
    }
}

static void paint_all(Display *dpy, XserverRegion region)
{
    win *w;
    win *t = 0;
    Bool ignore_region_is_dirty = g_paint_ignore_region_is_dirty;
    g_paint_ignore_region_is_dirty = False;
    (void) region;

    if (!redirected) {
        return;
    }

    if (fade_enabled) {
        Bool has_fading = False;
        int fading_count = 0;
        for (w = list; w; w = w->next) {
            if (w->opacity != w->target_opacity) {
                fade_step(dpy, w);
                has_fading = True;
                fading_count++;
            }
        }
        if (has_fading) {
            clip_changed = True;
            if (fade_debug) {
                fprintf(stderr, "[fade] %d windows fading\n",
                        fading_count);
            }
        }

        int cleanup_count = 0;
        for (w = list; w; w = w->next) {
            if (w->fade_finished) {
                if (fade_debug) {
                    fprintf(stderr,
                            "[fade] cleanup window id=0x%lx opacity=%u target=%u map_state=%d\n",
                            w->id, w->opacity, w->target_opacity,
                            w->a.map_state);
                }
                finish_unmap_win(dpy, w);
                w->fade_finished = False;
                cleanup_count++;
            }
        }
        if (cleanup_count > 0 && fade_debug) {
            fprintf(stderr, "[fade] cleaned up %d windows\n",
                    cleanup_count);
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, root_width, root_height);

    paint_root(dpy);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    CompRect ignore_reg = { 0 };

    for (w = list; w; w = w->next) {
#if CAN_DO_USABLE
        if (!w->usable)
            continue;
#endif

        if (w->a.map_state == IsUnmapped && !w->texture)
            continue;

        if (!w->texture) {
#if HAS_NAME_WINDOW_PIXMAP
            if (has_name_pixmap && !w->pixmap) {
                set_ignore(dpy, NextRequest(dpy));
                w->pixmap = XCompositeNameWindowPixmap(dpy, w->id);
            }
            if (w->pixmap) {
                update_window_texture(dpy, w);
                w->damaged = 0;
            }
#endif
        } else if (w->damaged) {
            update_window_texture(dpy, w);
            w->damaged = 0;
        }

        if (unlikely(ignore_region_is_dirty)) {
            w->paint_needed = win_paint_needed(w, &ignore_reg);
        }
        if (!w->paint_needed)
            continue;

        if (clip_changed) {
            if (w->border_size) {
                set_ignore(dpy, NextRequest(dpy));
                XFixesDestroyRegion(dpy, w->border_size);
                w->border_size = None;
            }
            win_extents(dpy, w);
        }

        if (!w->border_size) {
            w->border_size = border_size(dpy, w);
        }

        if (unlikely(!w->extents)) {
            win_extents(dpy, w);
        }

        XFixesCopyRegion(dpy, w->border_clip, region);
        w->prev_trans = t;
        t = w;
    }

    for (w = t; w; w = w->prev_trans) {
        int x, y, wid, hei;

#if HAS_NAME_WINDOW_PIXMAP
        x = w->a.x;
        y = w->a.y;
        wid = w->a.width + w->a.border_width * 2;
        hei = w->a.height + w->a.border_width * 2;
#else
        x = w->a.x + w->a.border_width;
        y = w->a.y + w->a.border_width;
        wid = w->a.width;
        hei = w->a.height;
#endif

        if (w->shadow_type == SHADOW_YES && w->shadow_texture) {
            float shadow_alpha = (float) w->opacity / (float) OPAQUE;
            gl_draw_texture(w->shadow_texture,
                            x + w->shadow_dx, y + w->shadow_dy,
                            w->shadow_width, w->shadow_height,
                            shadow_alpha);
        }

        if (w->texture) {
            float alpha =
                (w->opacity ==
                 OPAQUE) ? 1.0f : (float) w->opacity / (float) OPAQUE;

            if (fade_debug) {
                fprintf(stderr,
                        "### RENDER_DEBUG ### window=0x%lx opacity=%u alpha=%.3f texture=%u\n",
                        w->id, w->opacity, alpha, w->texture);
            }

            gl_draw_texture(w->texture, x, y, wid, hei, alpha);
        }
    }

    glDisable(GL_BLEND);
}

static void add_damage(Display *dpy, XserverRegion damage)
{
    if (all_damage_is_dirty) {
        XFixesUnionRegion(dpy, all_damage, all_damage, damage);
    } else {
        XFixesCopyRegion(dpy, all_damage, damage);
        all_damage_is_dirty = True;
    }
}

static void repair_win(Display *dpy, win *w)
{
    XserverRegion parts;

    if (!w->damaged) {
        parts = win_extents(dpy, w);
        set_ignore(dpy, NextRequest(dpy));
        XDamageSubtract(dpy, w->damage, None, None);
    } else {
        parts = g_xregion_tmp;
        set_ignore(dpy, NextRequest(dpy));
        XDamageSubtract(dpy, w->damage, None, parts);
        XFixesTranslateRegion(dpy, parts,
                              w->a.x + w->a.border_width,
                              w->a.y + w->a.border_width);
    }

    add_damage(dpy, parts);
    w->damaged = 1;
}

static wintype get_wintype_prop(Display *dpy, Window w)
{
    Atom actual;
    int format;
    unsigned long n, left, off;
    unsigned char *data;

    off = 0;

    do {
        set_ignore(dpy, NextRequest(dpy));

        int result =
            XGetWindowProperty(dpy, w, atom_win_type, off, 1L, False,
                               XA_ATOM,
                               &actual, &format, &n, &left, &data);

        if (unlikely(result != Success))
            break;

        if (likely(data != None)) {
            unsigned int i;
            for (i = 1; i < NUM_WINTYPES; ++i) {
                Atom a;
                memcpy(&a, data, sizeof(Atom));
                if (a == win_type[i]) {
                    XFree((void *) data);
                    return i;
                }
            }
            XFree((void *) data);
        }
        ++off;
    } while (left >= 4);

    return WINTYPE_UNKNOWN;
}

static wintype determine_wintype(Display *dpy, Window w, Window top)
{
    Window root_return, parent_return;
    Window *children = NULL;
    unsigned int nchildren, i;
    wintype type;

    type = get_wintype_prop(dpy, w);
    if (type != WINTYPE_UNKNOWN)
        return type;

    set_ignore(dpy, NextRequest(dpy));
    if (unlikely(!XQueryTree(dpy, w, &root_return, &parent_return,
                             &children, &nchildren))) {
        goto free_out;
    }

    for (i = 0; i < nchildren; i++) {
        type = determine_wintype(dpy, children[i], top);
        if (type != WINTYPE_UNKNOWN)
            goto free_out;
    }

    if (w != top) {
        type = WINTYPE_UNKNOWN;
    } else {
        type = WINTYPE_NORMAL;
    }

  free_out:
    if (children)
        XFree((void *) children);
    return type;
}

static unsigned int
get_opacity_prop(Display * dpy, win * w, unsigned int def);

static void handle_ConfigureNotify(Display * dpy, XConfigureEvent * ce);

static void map_win(Display *dpy, Window id)
{
    win *w = find_win(id);

    if (unlikely(!w))
        return;

    if (fade_debug) {
        fprintf(stderr, "[map_win] window id=0x%lx opacity=%u target=%u\n",
                id, w->opacity, w->target_opacity);
    }

    w->a.map_state = IsViewable;
    w->window_type = determine_wintype(dpy, w->id, w->id);

    if (!w->border_clip) {
        w->border_clip = XFixesCreateRegion(dpy, 0, 0);
    }

    XSelectInput(dpy, id, PropertyChangeMask | FocusChangeMask);
    XShapeSelectInput(dpy, id, ShapeNotifyMask);

    determine_mode(dpy, w);

    if (fade_enabled) {
        if (inactive_opacity && IS_NORMAL_WIN(w)) {
            w->target_opacity = INACTIVE_OPACITY;
        } else {
            w->target_opacity = OPAQUE;
        }
        w->opacity = 0;
        if (fade_debug) {
            fprintf(stderr,
                    "[map_win] set target=%u, starting fade-in from 0\n",
                    w->target_opacity);
        }
    } else {
        w->target_opacity = w->opacity;
    }

#if CAN_DO_USABLE
    w->damage_bounds.x = w->damage_bounds.y = 0;
    w->damage_bounds.width = w->damage_bounds.height = 0;
#endif
    w->damaged = 0;
    w->paint_needed = True;

    set_paint_ignore_region_dirty();
}

static void finish_unmap_win(Display *dpy, win *w)
{
    w->damaged = 0;
#if CAN_DO_USABLE
    w->usable = False;
#endif

    if (w->extents != None) {
        add_damage(dpy, w->extents);
    }
#if HAS_NAME_WINDOW_PIXMAP
    if (w->pixmap) {
        XFreePixmap(dpy, w->pixmap);
        w->pixmap = None;
    }
#endif

    if (w->texture) {
        glDeleteTextures(1, &w->texture);
        w->texture = 0;
    }

    if (w->border_size) {
        set_ignore(dpy, NextRequest(dpy));
        XFixesDestroyRegion(dpy, w->border_size);
        w->border_size = None;
    }

    if (w->shadow_texture) {
        glDeleteTextures(1, &w->shadow_texture);
        w->shadow_texture = 0;
    }

    clip_changed = True;
}

static void unmap_win(Display *dpy, Window id)
{
    win *w = find_win(id);

    if (!w)
        return;

    if (fade_debug) {
        fprintf(stderr,
                "[unmap_win] window id=0x%lx opacity=%u target=%u\n", id,
                w->opacity, w->target_opacity);
    }

    w->a.map_state = IsUnmapped;
    set_paint_ignore_region_dirty();

    if (fade_enabled) {
        set_target_opacity(dpy, w, 0);
    } else {
        finish_unmap_win(dpy, w);
    }
}

static bool is_gtk_frame_extent(Display *dpy, Window w)
{
    Atom type;
    int format;
    unsigned long nitems, after;
    unsigned char *data = NULL;
    int result;

    result =
        XGetWindowProperty(dpy, w, atom_gtk_frame_extents, 0, LONG_MAX,
                           false, XA_CARDINAL, &type, &format, &nitems,
                           &after, (unsigned char **) &data);
    if (result == Success && data != NULL) {
        XFree((void *) data);
        return nitems == 4;
    }
    return false;
}

static unsigned int
get_opacity_prop(Display *dpy, win *w, unsigned int def)
{
    Atom actual;
    int format;
    unsigned long n, left;

    unsigned char *data;
    int result =
        XGetWindowProperty(dpy, w->id, atom_opacity, 0L, 1L, False,
                           XA_CARDINAL, &actual, &format, &n, &left,
                           &data);

    if (result == Success && data != NULL) {
        unsigned int i;
        memcpy(&i, data, sizeof(unsigned int));
        XFree((void *) data);
        return i;
    }

    return def;
}

static void determine_mode(Display *dpy, win *w)
{
    int mode;
    (void) dpy;

    if (w->a.class == InputOnly) {
        mode = WINDOW_SOLID;
    } else {
        XVisualInfo vinfo_template;
        XVisualInfo *vinfo;
        int nvisuals;
        Bool has_alpha = False;

        vinfo_template.visualid = XVisualIDFromVisual(w->a.visual);
        vinfo =
            XGetVisualInfo(dpy, VisualIDMask, &vinfo_template, &nvisuals);

        if (vinfo) {
            if (vinfo->depth == 32 && vinfo->class == TrueColor) {
                has_alpha = True;
            }
            XFree(vinfo);
        }

        if (has_alpha) {
            mode = WINDOW_ARGB;
        } else if (w->opacity != OPAQUE) {
            mode = WINDOW_TRANS;
        } else {
            mode = WINDOW_SOLID;
        }
    }

    w->mode = mode;

    if (w->extents) {
        add_damage(dpy, w->extents);
    }
}

static void apply_opacity_change(Display *dpy, win *w)
{
    determine_mode(dpy, w);
    if (w->shadow_texture) {
        glDeleteTextures(1, &w->shadow_texture);
        w->shadow_texture = 0;
        win_extents(dpy, w);
    }
    if (w->extents) {
        add_damage(dpy, w->extents);
    }
    set_paint_ignore_region_dirty();
}

static void set_target_opacity(Display *dpy, win *w, unsigned long target)
{
    if (!fade_enabled) {
        w->opacity = target;
        w->target_opacity = target;
        apply_opacity_change(dpy, w);
    } else {
        w->target_opacity = target;
    }
}

static void fade_step(Display *dpy, win *w)
{
    Bool was_complete = (w->opacity == w->target_opacity);

    if (was_complete) {
        if (w->a.map_state == IsUnmapped && w->opacity == 0) {
            if (fade_debug) {
                fprintf(stderr,
                        "[fade_step] window id=0x%lx complete and unmapped, marking for cleanup\n",
                        w->id);
            }
            w->fade_finished = True;
        }
        return;
    }

    if (fade_debug) {
        fprintf(stderr,
                "[fade_step] window id=0x%lx opacity=%u->%u map_state=%d\n",
                w->id, w->opacity, w->target_opacity, w->a.map_state);
    }

    if (w->opacity < w->target_opacity) {
        unsigned long step = (unsigned long) (fade_in_step * OPAQUE);
        if (w->opacity + step > w->target_opacity) {
            w->opacity = w->target_opacity;
        } else {
            w->opacity += step;
        }
    } else {
        unsigned long step = (unsigned long) (fade_out_step * OPAQUE);
        if (w->opacity < step || w->opacity - step < w->target_opacity) {
            w->opacity = w->target_opacity;
        } else {
            w->opacity -= step;
        }
    }

    apply_opacity_change(dpy, w);

    if (w->a.map_state == IsUnmapped && w->opacity == 0) {
        if (fade_debug) {
            fprintf(stderr,
                    "[fade_step] window id=0x%lx just completed fade to 0, marking for cleanup\n",
                    w->id);
        }
        w->fade_finished = True;
    }
}

static void set_opacity(Display *dpy, win *w, unsigned long opacity)
{
    set_target_opacity(dpy, w, opacity);
}

static void add_win(Display *dpy, Window id, Window prev)
{
    win *new = calloc(1, sizeof(win));
    win **p;

    if (unlikely(!new))
        return;

    if (prev) {
        for (p = &list; *p; p = &(*p)->next) {
            if ((*p)->id == prev && !(*p)->destroyed)
                break;
        }
    } else {
        p = &list;
    }

    new->id = id;
    set_ignore(dpy, NextRequest(dpy));

    if (unlikely(!XGetWindowAttributes(dpy, id, &new->a))) {
        free(new);
        return;
    }
#if HAS_NAME_WINDOW_PIXMAP
    new->pixmap = None;
#endif
    new->texture = 0;
    new->egl_image = NULL;

    if (new->a.class == InputOnly) {

        new->damage = None;
    } else {
        new->damage_sequence = NextRequest(dpy);
        set_ignore(dpy, NextRequest(dpy));
        new->damage = XDamageCreate(dpy, id, XDamageReportNonEmpty);
    }

    new->border_size = None;
    new->extents = None;
    new->shadow_texture = 0;
    new->opacity = OPAQUE;
    new->target_opacity = OPAQUE;
    new->fade_finished = False;
    new->border_clip = None;

    get_frame_extents(dpy, id,
                      &new->left_width, &new->right_width,
                      &new->top_width, &new->bottom_width);

    new->next = *p;
    *p = new;

    if (new->a.map_state == IsViewable) {
        new->window_type = determine_wintype(dpy, id, id);
        if (inactive_opacity && IS_NORMAL_WIN(new)) {
            new->opacity = INACTIVE_OPACITY;
            new->target_opacity = INACTIVE_OPACITY;
        }
        map_win(dpy, id);
    }
}

static void set_paint_ignore_region_dirty(void)
{
    g_paint_ignore_region_is_dirty = True;
}

void restack_win(Display *dpy, win *w, Window new_above)
{
    Window old_above;
    (void) dpy;

    if (w->next) {
        old_above = w->next->id;
    } else {
        old_above = None;
    }

    if (old_above != new_above) {
        win **prev;

        for (prev = &list; *prev; prev = &(*prev)->next) {
            if ((*prev) == w)
                break;
        }

        *prev = w->next;

        for (prev = &list; *prev; prev = &(*prev)->next) {
            if ((*prev)->id == new_above && !(*prev)->destroyed)
                break;
        }

        w->next = *prev;
        *prev = w;
    }
}

static void do_configure_win(Display *dpy, win *w)
{
    XConfigureEvent *ce = &w->queue_configure;

    w->need_configure = False;
    w->a.x = ce->x;
    w->a.y = ce->y;
    if (w->configure_size_changed) {

#if HAS_NAME_WINDOW_PIXMAP
        if (w->pixmap) {
            XFreePixmap(dpy, w->pixmap);
            w->pixmap = None;
            if (w->texture) {
                glDeleteTextures(1, &w->texture);
                w->texture = 0;
            }
        }
#endif

        if (w->shadow_texture) {
            glDeleteTextures(1, &w->shadow_texture);
            w->shadow_texture = 0;
        }
    }

    w->a.width = ce->width;
    w->a.height = ce->height;
    w->a.border_width = ce->border_width;

    if (w->a.map_state != IsUnmapped
#if CAN_DO_USABLE
        && w->usable
#endif
        ) {
        if (likely(w->extents != None)) {
            add_damage(dpy, w->extents);
        }
        add_damage(dpy, win_extents(dpy, w));
    }

    clip_changed = True;
    w->a.override_redirect = ce->override_redirect;
    w->configure_size_changed = false;
    set_paint_ignore_region_dirty();
}

Bool g_configure_needed = False;

static void handle_ConfigureNotify(Display *dpy, XConfigureEvent *ce)
{
    win *w = find_win(ce->window);

    if (unlikely(!w)) {
        if (ce->window == root) {
            if (root_texture) {
                glDeleteTextures(1, &root_texture);
                glDeleteFramebuffers(1, &root_fbo);

                glGenTextures(1, &root_texture);
                glBindTexture(GL_TEXTURE_2D, root_texture);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ce->width,
                             ce->height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                             NULL);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                                GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                                GL_LINEAR);

                glGenFramebuffers(1, &root_fbo);
                glBindFramebuffer(GL_FRAMEBUFFER, root_fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER,
                                       GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                       root_texture, 0);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
            }
            root_width = ce->width;
            root_height = ce->height;
        }
        return;
    }

    g_configure_needed = True;
    w->need_configure = True;
    if (w->a.width != ce->width || w->a.height != ce->height) {
        w->configure_size_changed = true;
    }

    w->queue_configure = *ce;

    restack_win(dpy, w, ce->above);
}

static void circulate_win(Display *dpy, XCirculateEvent *ce)
{
    win *w = find_win(ce->window);
    Window new_above;

    if (!w)
        return;

    if (ce->place == PlaceOnTop) {
        new_above = list->id;
    } else {
        new_above = None;
    }

    restack_win(dpy, w, new_above);
    clip_changed = True;
}

static void finish_destroy_win(Display *dpy, Window id)
{
    win **prev, *w;

    for (prev = &list; (w = *prev); prev = &w->next) {
        if (w->id == id && w->destroyed) {
            finish_unmap_win(dpy, w);
            *prev = w->next;

            if (w->shadow_texture) {
                glDeleteTextures(1, &w->shadow_texture);
                w->shadow_texture = 0;
            }

            if (w->egl_image && eglimage_supported) {
                eglDestroyImageKHR_func(egl_display, w->egl_image);
                w->egl_image = NULL;
            }

            if (w->damage != None) {
                set_ignore(dpy, NextRequest(dpy));
                XDamageDestroy(dpy, w->damage);
                w->damage = None;
            }

            if (w->border_clip) {
                XFixesDestroyRegion(dpy, w->border_clip);
                w->border_clip = None;
            }
            if (w->extents) {
                XFixesDestroyRegion(dpy, w->extents);
                w->extents = None;
            }

            set_ignore(dpy, NextRequest(dpy));
            XSelectInput(dpy, id, 0);

            free(w);
            break;
        }
    }
}

static void destroy_win(Display *dpy, Window id)
{
    win *w = find_win(id);

    if (w)
        w->destroyed = True;

    set_paint_ignore_region_dirty();

    finish_destroy_win(dpy, id);
}

static void damage_win(Display *dpy, XDamageNotifyEvent *de)
{
    win *w = find_win(de->drawable);

    if (unlikely(!w))
        return;

    if (!redirected) {
        all_damage_is_dirty = True;
        return;
    }
#if CAN_DO_USABLE
    if (!w->usable) {
        if (w->damage_bounds.width == 0 || w->damage_bounds.height == 0) {
            w->damage_bounds = de->area;
        } else {
            if (de->area.x < w->damage_bounds.x) {
                w->damage_bounds.width +=
                    (w->damage_bounds.x - de->area.x);
                w->damage_bounds.x = de->area.x;
            }
            if (de->area.y < w->damage_bounds.y) {
                w->damage_bounds.height +=
                    (w->damage_bounds.y - de->area.y);
                w->damage_bounds.y = de->area.y;
            }
            if (de->area.x + de->area.width
                > w->damage_bounds.x + w->damage_bounds.width) {
                w->damage_bounds.width =
                    de->area.x + de->area.width - w->damage_bounds.x;
            }
            if (de->area.y + de->area.height
                > w->damage_bounds.y + w->damage_bounds.height) {
                w->damage_bounds.height =
                    de->area.y + de->area.height - w->damage_bounds.y;
            }
        }

        if (w->damage_bounds.x <= 0
            && w->damage_bounds.y <= 0
            && w->a.width <= w->damage_bounds.x + w->damage_bounds.width
            && w->a.height <=
            w->damage_bounds.y + w->damage_bounds.height) {
            clip_changed = True;
            w->usable = True;
        }
    }

    if (w->usable)
#endif
        repair_win(dpy, w);
}

static int error(Display *dpy, XErrorEvent *ev)
{
    int o;
    (void) dpy;

    if (should_ignore(ev->serial)) {
        return 0;
    }

    if (ev->request_code == composite_opcode
        && ev->minor_code == X_CompositeRedirectSubwindows) {
        exit(1);
    }

    o = ev->error_code - xfixes_error;
    switch (o) {
    case BadRegion:
        break;
    default:
        break;
    }

    o = ev->error_code - damage_error;
    switch (o) {
    case BadDamage:
        break;
    default:
        break;
    }

    o = ev->error_code - damage_error;
    switch (o) {
    case BadDamage:
        break;
    default:
        break;
    }

    return 0;
}

static void expose_root(Display *dpy, XRectangle *rects, int nrects)
{
    XFixesSetRegion(dpy, g_xregion_tmp, rects, nrects);
    add_damage(dpy, g_xregion_tmp);
}

static void daemonize(void)
{
    pid_t pid;
    int fd;

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "Failed to fork: %s\n", strerror(errno));
        exit(1);
    }

    if (pid > 0) {
        exit(0);
    }

    if (setsid() < 0) {
        fprintf(stderr, "Failed to create new session: %s\n",
                strerror(errno));
        exit(1);
    }

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "Failed to fork second time: %s\n",
                strerror(errno));
        exit(1);
    }

    if (pid > 0) {
        exit(0);
    }

    if (chdir("/") < 0) {
        fprintf(stderr, "Failed to change directory to /: %s\n",
                strerror(errno));
        exit(1);
    }

    fd = open("/dev/null", O_RDWR);
    if (fd != -1) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > 2) {
            close(fd);
        }
    }
}

void usage(int exitcode)
{
    fprintf(stderr, "usage: commoner [options]\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr,
            "  -b, --daemonize                Run as a daemon in the background\n");
    fprintf(stderr, "  -d display                     X display to use\n");
    fprintf(stderr,
            "  -r radius                      Shadow radius (default: 12)\n");
    fprintf(stderr,
            "  -o opacity                     Shadow opacity (default: 0.75)\n");
    fprintf(stderr,
            "  -l left-offset                 Shadow left offset (default: -15)\n");
    fprintf(stderr,
            "  -t top-offset                  Shadow top offset (default: -15)\n");
    fprintf(stderr,
            "  -i opacity                     Inactive window opacity\n");
    fprintf(stderr,
            "  -C                             Disable shadows on dock windows\n");
    fprintf(stderr,
            "  --no-fading                    Disable fading (enabled by default)\n");
    fprintf(stderr,
            "  --fade-in-step value           Fade in step (default: 0.06)\n");
    fprintf(stderr,
            "  --fade-out-step value          Fade out step (default: 0.07)\n");
    fprintf(stderr,
            "  --fade-delta ms                Time between fade steps in ms (default: 8)\n");
    fprintf(stderr,
            "  --unredir-if-possible          Unredirect fullscreen windows for better performance\n");
    fprintf(stderr,
            "  --debug                        Enable debug logging to stderr\n");
    fprintf(stderr,
            "  --version                      Show version information\n");
    fprintf(stderr, "  -h, --help                     Show this help\n");
    exit(exitcode);
}

static Bool register_cm(Display *dpy)
{
    Window w;
    Atom a;
    static char net_wm_cm[] = "_NET_WM_CM_Sxx";

    snprintf(net_wm_cm, sizeof(net_wm_cm), "_NET_WM_CM_S%d", g_screen);
    a = XInternAtom(dpy, net_wm_cm, False);
    w = XGetSelectionOwner(dpy, a);
    if (w != None) {
        XTextProperty tp;
        char **strs;
        int count;
        Atom winNameAtom = XInternAtom(dpy, "_NET_WM_NAME", False);

        if (!XGetTextProperty(dpy, w, &tp, winNameAtom) &&
            !XGetTextProperty(dpy, w, &tp, XA_WM_NAME)) {
            return False;
        }
        if (XmbTextPropertyToTextList(dpy, &tp, &strs, &count) == Success) {
            XFreeStringList(strs);
        }
        XFree(tp.value);
        return False;
    }

    w = XCreateSimpleWindow(dpy, RootWindow(dpy, g_screen), 0, 0, 1, 1, 0,
                            None, None);

    Xutf8SetWMProperties(dpy, w, "commoner", "commoner", NULL, 0, NULL,
                         NULL, NULL);

    XSetSelectionOwner(dpy, a, w, 0);
    return True;
}

static void run_configures(Display *dpy)
{
    win *w;
    for (w = list; w; w = w->next) {
        if (w->need_configure && !w->destroyed) {
            do_configure_win(dpy, w);
        }
    }
}

static Bool has_fading_windows(void)
{
    if (!fade_enabled)
        return False;
    win *w;
    for (w = list; w; w = w->next) {
        if (w->opacity != w->target_opacity) {
            return True;
        }
    }
    return False;
}

static void do_paint(Display *dpy)
{
    if (should_redir) {
        redir_start(dpy);
        should_redir = False;
    }

    if (redirected) {
        paint_all(dpy, all_damage);

        eglSwapBuffers(egl_display, egl_surface);
        XFlush(dpy);
    }

    all_damage_is_dirty = False;
    clip_changed = False;

    check_unredirect(dpy);

    if (should_unredir) {
        redir_stop(dpy);
        should_unredir = False;
    }
}

static Bool configure_timer_started = False;
static int configure_time = 0;

static void check_paint(Display *dpy)
{
    if (unlikely(g_configure_needed)) {
        if (!configure_timer_started) {

            run_configures(dpy);
            do_paint(dpy);
            configure_timer_started = True;
            configure_time =
                get_time_in_milliseconds() + CONFIGURE_TIMEOUT_MS;
        } else {
            int delta;
            delta = get_time_in_milliseconds() - configure_time;
            if (delta < CONFIGURE_TIMEOUT_MS) {
                return;
            }
            g_configure_needed = False;
            configure_timer_started = False;
            run_configures(dpy);
            do_paint(dpy);
        }
    } else {
        if (likely(all_damage_is_dirty) || has_fading_windows()) {
            do_paint(dpy);
        }
    }
}

int main(int argc, char **argv)
{
    static const struct option longopt[] = {
        { "daemonize", no_argument, NULL, 0 },
        { "help", no_argument, NULL, 0 },
        { "no-fading", no_argument, NULL, 0 },
        { "fade-in-step", required_argument, NULL, 0 },
        { "fade-out-step", required_argument, NULL, 0 },
        { "fade-delta", required_argument, NULL, 0 },
        { "debug", no_argument, NULL, 0 },
        { "version", no_argument, NULL, 0 },
        { "unredir-if-possible", no_argument, NULL, 0 },
        { 0, 0, 0, 0 },
    };

    XEvent ev;
    Window root_return, parent_return;
    Window *children;
    unsigned int nchildren;
    unsigned int i;
    XRectangle *expose_rects = 0;
    int size_expose = 0;
    int n_expose = 0;
    struct pollfd ufd;
    int p;
    int composite_major, composite_minor;
    char *display = 0;
    int o;
    int longopt_idx;
    Bool no_dock_shadow = False;
    Bool should_daemonize = False;
    bufferInit(ignore_ringbuf, 2048, ulong);

    for (i = 0; i < NUM_WINTYPES; ++i) {
        win_type_shadow[i] = False;
        win_type_opacity[i] = 1.0;
    }

    while ((o = getopt_long(argc, argv, "d:r:o:l:t:i:hCb",
                            longopt, &longopt_idx)) != -1) {
        switch (o) {

        case 0:
            switch (longopt_idx) {
            case 0:
                should_daemonize = True;
                break;
            case 1:
                usage(0);
                break;
            case 2:
                fade_enabled = False;
                break;
            case 3:
                fade_in_step = atof(optarg);
                break;
            case 4:
                fade_out_step = atof(optarg);
                break;
            case 5:
                fade_delta = atoi(optarg);
                break;
            case 6:
                fade_debug = True;
                break;
            case 7:
#ifdef VERSION
                printf("commoner version %s\n", VERSION);
#else
                printf("commoner version unknown\n");
#endif
                exit(0);
                break;
            case 8:
                unredir_fullscreen = True;
                break;
            default:
                exit(2);
            }
            break;

        case 'd':
            display = optarg;
            break;
        case 'h':
            usage(0);
            break;
        case 'C':
            no_dock_shadow = True;
            break;
        case 'r':
            shadow_radius = atoi(optarg);
            break;
        case 'o':
            shadow_opacity = atof(optarg);
            break;
        case 'l':
            shadow_offset_x = atoi(optarg);
            break;
        case 't':
            shadow_offset_y = atoi(optarg);
            break;
        case 'i':
            inactive_opacity = (double) atof(optarg);
            break;
        case 'b':
            should_daemonize = True;
            break;
        default:
            usage(1);
            break;
        }
    }

    if (no_dock_shadow) {
        win_type_shadow[WINTYPE_DOCK] = False;
    }

    if (should_daemonize) {
        daemonize();
    }

    dpy = XOpenDisplay(display);
    if (!dpy) {
        exit(1);
    }
    g_dpy = dpy;

    XSetErrorHandler(error);

    g_screen = DefaultScreen(dpy);
    root = RootWindow(dpy, g_screen);

    int present_major, present_minor;
    if (!XPresentQueryExtension
        (dpy, &present_opcode, &present_event_base, &present_error_base)
        || !XPresentQueryVersion(dpy, &present_major, &present_minor)) {
        fprintf(stderr,
                "XPresent extension is required but not available\n");
        exit(1);
    }

    if (!XQueryExtension(dpy, COMPOSITE_NAME, &composite_opcode,
                         &composite_event, &composite_error)) {
        exit(1);
    }

    XCompositeQueryVersion(dpy, &composite_major, &composite_minor);

#if HAS_NAME_WINDOW_PIXMAP
    if (composite_major > 0 || composite_minor >= 2) {
        has_name_pixmap = True;
    }
#endif

    if (!XDamageQueryExtension(dpy, &damage_event, &damage_error)) {
        exit(1);
    }

    if (!XFixesQueryExtension(dpy, &xfixes_event, &xfixes_error)) {
        exit(1);
    }

    if (!XShapeQueryExtension(dpy, &shape_event, &shape_error)) {
        fprintf(stderr,
                "XShape extension is required but not available\n");
        exit(1);
    }

    if (!register_cm(dpy))
        exit(1);

    atom_opacity = XInternAtom(dpy, "_NET_WM_WINDOW_OPACITY", False);
    atom_win_type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    atom_pixmap = XInternAtom(dpy, "PIXMAP", False);
    atom_wm_state = XInternAtom(dpy, "WM_STATE", False);
    atom_net_frame_extents = XInternAtom(dpy, "_NET_FRAME_EXTENTS", False);
    atom_gtk_frame_extents = XInternAtom(dpy, "_GTK_FRAME_EXTENTS", False);
    atom_root_pmap = XInternAtom(dpy, "_XROOTPMAP_ID", False);
    win_type[WINTYPE_DESKTOP] = XInternAtom(dpy,
                                            "_NET_WM_WINDOW_TYPE_DESKTOP",
                                            False);
    win_type[WINTYPE_DOCK] =
        XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    win_type[WINTYPE_TOOLBAR] =
        XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_TOOLBAR", False);
    win_type[WINTYPE_MENU] =
        XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_MENU", False);
    win_type[WINTYPE_UTILITY] =
        XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_UTILITY", False);
    win_type[WINTYPE_SPLASH] =
        XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_SPLASH", False);
    win_type[WINTYPE_DIALOG] =
        XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    win_type[WINTYPE_NORMAL] =
        XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    win_type[WINTYPE_DROPDOWN_MENU] =
        XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU", False);
    win_type[WINTYPE_POPUP_MENU] =
        XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_POPUP_MENU", False);
    win_type[WINTYPE_TOOLTIP] =
        XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_TOOLTIP", False);
    win_type[WINTYPE_NOTIFY] =
        XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);
    win_type[WINTYPE_COMBO] =
        XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_COMBO", False);
    win_type[WINTYPE_DND] =
        XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DND", False);

    gaussian_map = make_gaussian_map(dpy, shadow_radius);
    presum_gaussian(gaussian_map);

    if (!root_init()) {
        exit(1);
    }

    all_damage = XFixesCreateRegion(dpy, 0, 0);
    all_damage_is_dirty = False;
    g_xregion_tmp = XFixesCreateRegion(dpy, 0, 0);

    update_root_background(dpy);

    clip_changed = True;
    XGrabServer(dpy);

    XCompositeRedirectSubwindows(dpy, root, CompositeRedirectManual);

    XSelectInput(dpy, root,
                 SubstructureNotifyMask
                 | ExposureMask
                 | StructureNotifyMask | PropertyChangeMask);

    XQueryTree(dpy, root, &root_return,
               &parent_return, &children, &nchildren);

    for (i = 0; i < nchildren; i++) {
        add_win(dpy, children[i], i ? children[i - 1] : None);
    }

    XFree(children);

    XUngrabServer(dpy);

    ufd.fd = ConnectionNumber(dpy);
    ufd.events = POLLIN;

    {
        XRectangle root_rect = {.x = 0,.y = 0,
            .width = root_width,.height = root_height
        };
        XFixesSetRegion(dpy, g_xregion_tmp, &root_rect, 1);
        paint_all(dpy, g_xregion_tmp);
    }

    for (;;) {
        do {
            if (!QLength(dpy)) {
                int timeout = -1;
                if (configure_timer_started) {
                    timeout = CONFIGURE_TIMEOUT_MS;
                } else if (has_fading_windows()) {
                    timeout = fade_delta;
                }
                if (unlikely(poll(&ufd, 1, timeout) == 0)) {
                    check_paint(dpy);
                    break;
                }
            }

            XNextEvent(dpy, &ev);

            if (likely((ev.type & 0x7f) != KeymapNotify)) {
                discard_ignore(ev.xany.serial);
            }

            switch (ev.type) {
            case FocusIn:{
                    if (!inactive_opacity)
                        break;

                    if (ev.xfocus.detail == NotifyPointer)
                        break;

                    win *fw = find_win(ev.xfocus.window);
                    if (IS_NORMAL_WIN(fw)) {
                        set_opacity(dpy, fw, OPAQUE);
                    }
                    break;
                }
            case FocusOut:{
                    if (!inactive_opacity)
                        break;

                    if (ev.xfocus.mode != NotifyGrab
                        && ev.xfocus.detail == NotifyVirtual)
                        break;

                    win *fw = find_win(ev.xfocus.window);
                    if (IS_NORMAL_WIN(fw)) {
                        set_opacity(dpy, fw, INACTIVE_OPACITY);
                    }
                    break;
                }
            case CreateNotify:
                add_win(dpy, ev.xcreatewindow.window, 0);
                break;
            case ConfigureNotify:
                handle_ConfigureNotify(dpy, &ev.xconfigure);
                break;
            case DestroyNotify:
                destroy_win(dpy, ev.xdestroywindow.window);
                break;
            case MapNotify:
                map_win(dpy, ev.xmap.window);
                break;
            case UnmapNotify:
                unmap_win(dpy, ev.xunmap.window);
                break;
            case ReparentNotify:
                if (ev.xreparent.parent == root) {
                    add_win(dpy, ev.xreparent.window, 0);
                } else {
                    destroy_win(dpy, ev.xreparent.window);
                }
                break;
            case CirculateNotify:
                circulate_win(dpy, &ev.xcirculate);
                break;
            case Expose:
                if (ev.xexpose.window == root) {
                    int more = ev.xexpose.count + 1;
                    if (n_expose == size_expose) {
                        if (expose_rects) {
                            expose_rects = realloc(expose_rects,
                                                   (size_expose +
                                                    more) *
                                                   sizeof(XRectangle));
                            size_expose += more;
                        } else {
                            expose_rects =
                                malloc(more * sizeof(XRectangle));
                            size_expose = more;
                        }
                    }
                    expose_rects[n_expose].x = ev.xexpose.x;
                    expose_rects[n_expose].y = ev.xexpose.y;
                    expose_rects[n_expose].width = ev.xexpose.width;
                    expose_rects[n_expose].height = ev.xexpose.height;
                    n_expose++;
                    if (ev.xexpose.count == 0) {
                        expose_root(dpy, expose_rects, n_expose);
                        n_expose = 0;
                    }
                }
                break;
            case PropertyNotify:
                for (p = 0; root_background_props[p]; p++) {
                    if (ev.xproperty.atom ==
                        XInternAtom(dpy, root_background_props[p],
                                    False)) {
                        update_root_background(dpy);
                        add_damage(dpy, None);
                        break;
                    }
                }
                if (ev.xproperty.atom == atom_opacity) {
                    win *w = find_win(ev.xproperty.window);
                    if (w) {
                        double def = win_type_opacity[w->window_type];
                        set_opacity(dpy, w,
                                    get_opacity_prop(dpy, w,
                                                     (unsigned
                                                      long) (OPAQUE *
                                                             def)));
                    }
                }
                break;
            case SelectionClear:
                exit(0);
                break;
            default:
                if (likely(ev.type == damage_event + XDamageNotify)) {
                    damage_win(dpy, (XDamageNotifyEvent *) & ev);
                } else if (ev.type == shape_event + ShapeNotify) {
                    win *w = find_win(((XShapeEvent *) & ev)->window);
                    if (w) {
                        if (w->border_size) {
                            set_ignore(dpy, NextRequest(dpy));
                            XFixesDestroyRegion(dpy, w->border_size);
                            w->border_size = None;
                        }
                        if (w->extents) {
                            add_damage(dpy, w->extents);
                        }
                        w->border_size = border_size(dpy, w);
                        if (w->extents) {
                            add_damage(dpy, w->extents);
                        }
                        clip_changed = True;
                    }
                }
                break;
            }
        } while (QLength(dpy));

        check_paint(dpy);
    }
}
