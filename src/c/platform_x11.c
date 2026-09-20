/* platform_x11.c — window, input, timing and pixel presentation.
 *
 * Only X11 is required (libX11 + libXext).  XShm is used when the server
 * offers it so presenting a frame costs one memcpy into shared memory and
 * nothing else; otherwise we fall back to XPutImage over the socket.
 *
 * Window is a plain non-decorated-by-us, fixed-aspect client area sized to
 * internal_resolution * integer_scale, so pixel art stays perfectly square.
 */
#define _POSIX_C_SOURCE 200809L

#include "platform.h"
#include "platform_sdl.h"

#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/XShm.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ state */
#define SHM_SLOTS 2

typedef struct {
    XImage *img;
    uint32_t *data;
    XShmSegmentInfo shm;
    int in_flight;
    int used;
} ShmSlot;

static struct {
    Display *dpy;
    Window win;
    GC gc;
    Visual *vis;
    int screen;
    int depth;
    int have_shm;
    int shm_event;
    int shm_pending;
    ShmSlot slot[SHM_SLOTS];
    int slot_next;

    int headless;
    int native_wayland;
    int internal_w, internal_h;
    int scale;

    uint32_t *scaled;   /* internal -> window, nearest neighbour */
    int scaled_w, scaled_h;
    char mode[48];

    /* Fallback presentation image, created once and reused.  Only used when the
     * X server has no MIT-SHM; recreating it every frame would be wasteful. */
    XImage *fallback;

    /* input: keycode -> action table rebuilt on MappingNotify */
    signed char keymap[256];
} P;

/* ------------------------------------------------------------- key mapping */
typedef struct {
    KeySym sym;
    int action;
} KeyBind;

static const KeyBind BINDS[] = {
    {XK_Left, PK_LEFT},   {XK_a, PK_LEFT},
    {XK_Right, PK_RIGHT}, {XK_d, PK_RIGHT},
    {XK_Up, PK_UP},       {XK_w, PK_UP},
    {XK_Down, PK_DOWN},   {XK_s, PK_DOWN},
    {XK_space, PK_FIRE},  {XK_z, PK_FIRE},      {XK_j, PK_FIRE},
    {XK_Return, PK_ENTER},{XK_KP_Enter, PK_ENTER},{XK_space, PK_ENTER},
    {XK_Escape, PK_BACK},
    {XK_p, PK_PAUSE},     {XK_P, PK_PAUSE},
    {XK_m, PK_MUTE},      {XK_M, PK_MUTE},
    {XK_bracketleft, PK_VOL_DOWN},  {XK_bracketright, PK_VOL_UP},
    {XK_minus, PK_SCALE_DOWN},      {XK_equal, PK_SCALE_UP}, {XK_plus, PK_SCALE_UP},
    {XK_1, PK_ALLY_1}, {XK_2, PK_ALLY_2}, {XK_3, PK_ALLY_3}, {XK_4, PK_ALLY_4},
    {XK_KP_End, PK_ALLY_1}, {XK_KP_Down, PK_ALLY_2}, {XK_KP_Next, PK_ALLY_3}, {XK_KP_Left, PK_ALLY_4},
    {XK_KP_1, PK_ALLY_1}, {XK_KP_2, PK_ALLY_2}, {XK_KP_3, PK_ALLY_3}, {XK_KP_4, PK_ALLY_4},
    {XK_n, PK_MUSIC}, {XK_N, PK_MUSIC},
    {XK_F1, PK_DEBUG}, {XK_F2, PK_SCALE_DOWN}, {XK_F3, PK_SCALE_UP},
};

static void rebuild_keymap(void)
{
    memset(P.keymap, -1, sizeof(P.keymap));
    int min_kc = 0, max_kc = 0;
    XDisplayKeycodes(P.dpy, &min_kc, &max_kc);
    for (int kc = min_kc; kc <= max_kc && kc < 256; ++kc) {
        for (int lvl = 0; lvl < 4; ++lvl) {
            KeySym sym = XkbKeycodeToKeysym(P.dpy, (KeyCode)kc, 0, lvl);
            if (sym == NoSymbol) continue;
            for (size_t i = 0; i < sizeof(BINDS) / sizeof(BINDS[0]); ++i) {
                if (BINDS[i].sym == sym && P.keymap[kc] < 0) {
                    P.keymap[kc] = (signed char)BINDS[i].action;
                }
            }
        }
    }
}

/* ------------------------------------------------------------- shm helpers */
static void shm_slot_destroy(ShmSlot *s)
{
    if (s->img) {
        if (s->used) {
            XShmDetach(P.dpy, &s->shm);
            s->img->data = NULL;
        }
        XDestroyImage(s->img);
        s->img = NULL;
    }
    if (s->used) {
        if (s->shm.shmaddr) shmdt(s->shm.shmaddr);
        if (s->shm.shmid >= 0) shmctl(s->shm.shmid, IPC_RMID, NULL);
        s->used = 0;
    }
    s->data = NULL;
    s->in_flight = 0;
}

/* Drain ShmCompletion events so we know which shared buffer the server is
 * done reading.  Only matching events are consumed; keyboard events stay
 * queued for plat_poll(). */
static void shm_drain(void)
{
    if (P.shm_pending <= 0) return;
    XEvent ev;
    while (P.shm_pending > 0 && XCheckTypedEvent(P.dpy, P.shm_event, &ev)) {
        P.shm_pending--;
        for (int i = 0; i < SHM_SLOTS; ++i) {
            if (P.slot[i].used && P.slot[i].in_flight) { P.slot[i].in_flight = 0; break; }
        }
    }
}

static void shm_destroy_all(void)
{
    if (!P.dpy) return;
    for (int i = 0; i < SHM_SLOTS; ++i) shm_slot_destroy(&P.slot[i]);
    P.shm_pending = 0;
}

static int shm_create_all(int w, int h)
{
    for (int i = 0; i < SHM_SLOTS; ++i) {
        ShmSlot *s = &P.slot[i];
        s->img = XShmCreateImage(P.dpy, P.vis, (unsigned)P.depth, ZPixmap, NULL, &s->shm, (unsigned)w, (unsigned)h);
        if (!s->img) return -1;
        /* owner-only shared segment; removed again in shm_slot_destroy() */
        s->shm.shmid = shmget(IPC_PRIVATE, (size_t)s->img->bytes_per_line * (size_t)h, IPC_CREAT | 0x180);
        if (s->shm.shmid < 0) { XDestroyImage(s->img); s->img = NULL; return -1; }
        s->shm.shmaddr = (char *)shmat(s->shm.shmid, NULL, 0);
        s->shm.readOnly = False;
        if (s->shm.shmaddr == (char *)-1) {
            shmctl(s->shm.shmid, IPC_RMID, NULL);
            XDestroyImage(s->img);
            s->img = NULL;
            return -1;
        }
        s->img->data = s->shm.shmaddr;
        if (!XShmAttach(P.dpy, &s->shm)) {
            shmdt(s->shm.shmaddr);
            shmctl(s->shm.shmid, IPC_RMID, NULL);
            s->img->data = NULL;
            XDestroyImage(s->img);
            s->img = NULL;
            return -1;
        }
        s->used = 1;
        s->data = (uint32_t *)s->shm.shmaddr;
        s->in_flight = 0;
    }
    return 0;
}

/* ------------------------------------------------------------ scaled buffer */
static int scaled_ensure(int w, int h)
{
    if (P.scaled && P.scaled_w == w && P.scaled_h == h) return 0;
    free(P.scaled);
    P.scaled = NULL;
    P.scaled_w = P.scaled_h = 0;
    size_t n = (size_t)w * (size_t)h;
    uint32_t *buf = (uint32_t *)malloc(n * sizeof(uint32_t));
    if (!buf) return -1;
    P.scaled = buf;
    P.scaled_w = w;
    P.scaled_h = h;
    return 0;
}

void plat_scale_blit(uint32_t *dst, int dw, int dh, const uint32_t *src, int sw, int sh, int scale)
{
    (void)dh; /* dst is always sized internal*scale, so the height is implied */
    if (scale <= 0) return;
    for (int y = 0; y < sh; ++y) {
        const uint32_t *row = src + (size_t)y * (size_t)sw;
        uint32_t *out = dst + (size_t)y * (size_t)scale * (size_t)dw;
        /* horizontal expansion of one source row */
        for (int x = 0; x < sw; ++x) {
            uint32_t c = row[x];
            uint32_t *o = out + (size_t)x * (size_t)scale;
            for (int k = 0; k < scale; ++k) o[k] = c;
        }
        /* vertical duplication of that row */
        for (int r = 1; r < scale; ++r) {
            memcpy(out + (size_t)r * (size_t)dw, out, (size_t)sw * (size_t)scale * sizeof(uint32_t));
        }
    }
}

/* X protocol errors are reported and swallowed: a window manager can produce
 * benign BadMatch/BadWindow during map/resize races, and dying because of one
 * would be a poor way for a game to end. */
static int P_last_x_error = 0;

static int x_error_handler(Display *dpy, XErrorEvent *ev)
{
    char text[128];
    text[0] = '\0';
    XGetErrorText(dpy, ev->error_code, text, (int)sizeof(text));
    P_last_x_error = (int)ev->error_code;
    (void)text;
    return 0; /* ignore and keep running */
}

/* ---------------------------------------------------------------- open/close */
static int pick_scale(int iw, int ih, int want)
{
    int sw = 0, sh = 0;
    plat_screen_size(&sw, &sh);
    if (want > 0) {
        if (want > 6) want = 6;
        /* still refuse to exceed the screen */
        while (want > 1 && (iw * want > sw * 95 / 100 || ih * want > sh * 88 / 100)) want--;
        return want < 1 ? 1 : want;
    }
    /* Automatic scale is capped at 4x: beyond that the presentation buffers
     * (and therefore RSS) grow faster than the picture improves.  The player
     * can still go higher deliberately with F3 / '='. */
    int best = 1;
    for (int s = 2; s <= 4; ++s) {
        if (iw * s <= sw * 95 / 100 && ih * s <= sh * 88 / 100) best = s;
    }
    return best;
}

int plat_open(int internal_w, int internal_h, int requested_scale, int headless)
{
    memset(&P, 0, sizeof(P));
    P.headless = headless;
    P.internal_w = internal_w;
    P.internal_h = internal_h;
    P.scale = requested_scale > 0 ? requested_scale : 3;
    P.shm_event = -1;
    strcpy(P.mode, headless ? "headless" : "x11");

    if (headless) {
        P.scale = 1;
        snprintf(P.mode, sizeof(P.mode), "headless %dx%d", internal_w, internal_h);
        return 0;
    }

    const char *backend = getenv("MSS_VIDEO_BACKEND");
    if (backend && strcmp(backend, "x11") != 0 && strcmp(backend, "wayland") != 0) {
        fprintf(stderr, "mini-space-shooter: MSS_VIDEO_BACKEND must be x11 or wayland.\n");
        return -1;
    }
    if ((backend && strcmp(backend, "wayland") == 0) ||
        (!backend && getenv("WAYLAND_DISPLAY") && getenv("WAYLAND_DISPLAY")[0])) {
        if (sdlplat_open(internal_w, internal_h, requested_scale) == 0) {
            P.native_wayland = 1;
            return 0;
        }
        if (backend) return -1;
        fprintf(stderr, "mini-space-shooter: trying the X11 fallback.\n");
    }

    P.dpy = XOpenDisplay(NULL);
    if (!P.dpy) return -1;

    P.screen = DefaultScreen(P.dpy);
    P.vis = DefaultVisual(P.dpy, P.screen);
    P.depth = DefaultDepth(P.dpy, P.screen);
    P.scale = pick_scale(internal_w, internal_h, requested_scale);

    int w = internal_w * P.scale, h = internal_h * P.scale;
    Window root = RootWindow(P.dpy, P.screen);
    XSetWindowAttributes attrs;
    memset(&attrs, 0, sizeof(attrs));
    attrs.background_pixel = BlackPixel(P.dpy, P.screen);
    attrs.border_pixel = BlackPixel(P.dpy, P.screen);
    attrs.event_mask = KeyPressMask | KeyReleaseMask | StructureNotifyMask | ExposureMask | FocusChangeMask;

    P.win = XCreateWindow(P.dpy, root, 0, 0, (unsigned)w, (unsigned)h, 0, P.depth, InputOutput, P.vis,
                          CWBackPixel | CWBorderPixel | CWEventMask, &attrs);
    if (!P.win) { XCloseDisplay(P.dpy); P.dpy = NULL; return -1; }

    /* fixed aspect: the user may scale the window only by whole numbers */
    XSizeHints hints;
    memset(&hints, 0, sizeof(hints));
    hints.flags = PAspect | PMinSize | PMaxSize;
    hints.min_aspect.x = internal_w;
    hints.min_aspect.y = internal_h;
    hints.max_aspect.x = internal_w;
    hints.max_aspect.y = internal_h;
    hints.min_width = internal_w;
    hints.min_height = internal_h;
    hints.max_width = internal_w * 6;
    hints.max_height = internal_h * 6;
    XSetWMNormalHints(P.dpy, P.win, &hints);

    XWMHints wm_hints;
    memset(&wm_hints, 0, sizeof(wm_hints));
    wm_hints.flags = InputHint | StateHint;
    wm_hints.input = True;
    wm_hints.initial_state = NormalState;
    XSetWMHints(P.dpy, P.win, &wm_hints);

    XStoreName(P.dpy, P.win, "Mini Space Shooter");
    char res_name[] = "mini-space-shooter";
    char res_class[] = "MiniSpaceShooter";
    XClassHint ch;
    ch.res_name = res_name;
    ch.res_class = res_class;
    XSetClassHint(P.dpy, P.win, &ch);

    Atom wm_delete = XInternAtom(P.dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(P.dpy, P.win, &wm_delete, 1);
    Atom wm_pid = XInternAtom(P.dpy, "_NET_WM_PID", False);
    unsigned long pid = (unsigned long)getpid(); /* Xlib format=32 consumes longs */
    XChangeProperty(P.dpy, P.win, wm_pid, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&pid, 1);

    P.gc = XCreateGC(P.dpy, P.win, 0, NULL);
    XSetForeground(P.dpy, P.gc, BlackPixel(P.dpy, P.screen));
    XSetErrorHandler(x_error_handler);

    /* suppress auto-repeat release/press pairs so edge detection is clean */
    Bool supported = False;
    XkbSetDetectableAutoRepeat(P.dpy, True, &supported);

    rebuild_keymap();

    int shm_ok = 0;
    /* MSS_NO_SHM=1 forces the plain XPutImage path; it is the only way to
     * exercise the fallback on a machine that has MIT-SHM (and useful for
     * debugging remote displays). */
    if (!getenv("MSS_NO_SHM") && XShmQueryExtension(P.dpy)) {
        int major = 0, minor = 0;
        Bool shared = False;
        if (XShmQueryVersion(P.dpy, &major, &minor, &shared)) {
            P.shm_event = XShmGetEventBase(P.dpy) + ShmCompletion;
            if (shm_create_all(w, h) == 0) shm_ok = 1;
        }
    }
    P.have_shm = shm_ok;
    if (!shm_ok) shm_destroy_all();

    if (scaled_ensure(w, h) != 0) { plat_close(); return -1; }

    XMapWindow(P.dpy, P.win);
    XFlush(P.dpy);
    snprintf(P.mode, sizeof(P.mode), "%s %dx%d (x%d)", shm_ok ? "x11+shm" : "x11", w, h, P.scale);
    return 0;
}

/* Rebuilds the presentation buffers after a scale change. */
static void resize_for_scale(int new_scale)
{
    if (new_scale < 1) new_scale = 1;
    if (new_scale > 6) new_scale = 6;
    if (new_scale == P.scale) return;
    shm_drain();
    shm_destroy_all();
    if (P.fallback) {
        P.fallback->data = NULL; /* points into the buffer about to be reallocated */
        XDestroyImage(P.fallback);
        P.fallback = NULL;
    }
    P.scale = new_scale;
    int w = P.internal_w * P.scale, h = P.internal_h * P.scale;
    if (scaled_ensure(w, h) != 0) return;
    if (P.have_shm) {
        if (shm_create_all(w, h) != 0) { shm_destroy_all(); P.have_shm = 0; }
    }
    XResizeWindow(P.dpy, P.win, (unsigned)w, (unsigned)h);
    XFlush(P.dpy);
    snprintf(P.mode, sizeof(P.mode), "%s %dx%d (x%d)", P.have_shm ? "x11+shm" : "x11", w, h, P.scale);
}

void plat_close(void)
{
    if (P.native_wayland) { sdlplat_close(); P.native_wayland = 0; return; }
    free(P.scaled);
    P.scaled = NULL;
    if (P.fallback) {
        P.fallback->data = NULL; /* owned by P.scaled, already released above */
        XDestroyImage(P.fallback);
        P.fallback = NULL;
    }
    if (!P.dpy) return;
    if (!P.dpy) { /* nothing opened */ }
    else {
        shm_drain();
        shm_destroy_all();
        if (P.gc) XFreeGC(P.dpy, P.gc);
        if (P.win) XDestroyWindow(P.dpy, P.win);
        XCloseDisplay(P.dpy);
        P.dpy = NULL;
    }
    P.gc = NULL;
    P.win = 0;
}

/* ------------------------------------------------------------------- input */
void plat_poll(PlatInput *out)
{
    if (P.native_wayland) { sdlplat_poll(out); return; }
    static uint8_t prev[PK_COUNT];
    memset(out, 0, sizeof(*out));
    out->scale = P.scale;
    memcpy(out->down, prev, sizeof(prev));

    if (!P.dpy || !P.win) {
        memset(prev, 0, sizeof(prev)); /* headless: nothing to read */
        return;
    }

    XEvent ev;
    /* MSS_DEBUG_EVENTS=1 traces every X event; kept because this is the only
     * sensible way to debug input problems on someone else's machine. */
    static int trace_events = -1;
    if (trace_events < 0) trace_events = getenv("MSS_DEBUG_EVENTS") ? 1 : 0;
    while (XPending(P.dpy)) {
        XNextEvent(P.dpy, &ev);
        if (trace_events) fprintf(stderr, "[ev] type=%d\n", ev.type);
        if (trace_events && ev.type == ClientMessage)
            fprintf(stderr, "[ev] client message_type=0x%lx data0=0x%lx data1=0x%lx wm_delete=0x%lx\n",
                    (unsigned long)ev.xclient.message_type, (unsigned long)ev.xclient.data.l[0],
                    (unsigned long)ev.xclient.data.l[1], (unsigned long)XInternAtom(P.dpy, "WM_DELETE_WINDOW", False));
        switch (ev.type) {
        case KeyPress:
        case KeyRelease: {
            unsigned int kc = ev.xkey.keycode;
            if (kc < 256) {
                int act = P.keymap[kc];
                if (act >= 0 && act < PK_COUNT) out->down[act] = (ev.type == KeyPress);
            }
            break;
        }
        case FocusOut:
            memset(out->down, 0, sizeof(out->down));
            break;
        case MappingNotify:
            XRefreshKeyboardMapping(&ev.xmapping);
            rebuild_keymap();
            break;
        case ConfigureNotify: {
            /* external resize: snap to the nearest whole pixel scale */
            int s = ev.xconfigure.width / (P.internal_w > 0 ? P.internal_w : 1);
            if (s < 1) s = 1;
            if (s > 6) s = 6;
            if (s != P.scale) resize_for_scale(s);
            break;
        }
        case ClientMessage: {
            Atom wm_delete = XInternAtom(P.dpy, "WM_DELETE_WINDOW", False);
            if ((Atom)ev.xclient.data.l[0] == wm_delete) out->quit = 1;
            break;
        }
        default:
            if (P.shm_event >= 0 && ev.type == P.shm_event) {
                if (P.shm_pending > 0) P.shm_pending--;
                for (int i = 0; i < SHM_SLOTS; ++i) {
                    if (P.slot[i].used && P.slot[i].in_flight) { P.slot[i].in_flight = 0; break; }
                }
            }
            break;
        }
    }

    for (int i = 0; i < PK_COUNT; ++i) {
        out->pressed[i] = (uint8_t)(out->down[i] && !prev[i]);
        out->released[i] = (uint8_t)(!out->down[i] && prev[i]);
    }
    /* scale keys are handled here so every screen gets them for free */
    if (out->pressed[PK_SCALE_UP]) resize_for_scale(P.scale + 1);
    if (out->pressed[PK_SCALE_DOWN]) resize_for_scale(P.scale - 1);
    out->scale = P.scale;
    memcpy(prev, out->down, sizeof(prev));
}

/* ----------------------------------------------------------------- present */
void plat_present(const uint32_t *pixels, int w, int h)
{
    if (P.native_wayland) { sdlplat_present(pixels, w, h); return; }
    if (P.headless || !P.dpy || !P.win || !pixels) return;
    if (scaled_ensure(P.scaled_w, P.scaled_h) != 0) return;

    int dw = P.scaled_w, dh = P.scaled_h;
    plat_scale_blit(P.scaled, dw, dh, pixels, w, h, P.scale);

    if (P.have_shm) {
        /* pick a slot the server is not reading from */
        ShmSlot *s = NULL;
        for (int i = 0; i < SHM_SLOTS; ++i) {
            ShmSlot *cand = &P.slot[(P.slot_next + i) % SHM_SLOTS];
            if (!cand->in_flight) { s = cand; P.slot_next = (P.slot_next + i + 1) % SHM_SLOTS; break; }
        }
        if (!s) {
            shm_drain();
            for (int i = 0; i < SHM_SLOTS; ++i) {
                if (!P.slot[i].in_flight) { s = &P.slot[i]; break; }
            }
        }
        if (!s) { XSync(P.dpy, False); P.shm_pending = 0; s = &P.slot[0]; s->in_flight = 0; }
        memcpy(s->data, P.scaled, (size_t)dw * (size_t)dh * sizeof(uint32_t));
        XShmPutImage(P.dpy, P.win, P.gc, s->img, 0, 0, 0, 0, (unsigned)dw, (unsigned)dh, True);
        s->in_flight = 1;
        P.shm_pending++;
        XFlush(P.dpy);
        /* If the compositor is slow, keep the queue bounded. */
        if (P.shm_pending > SHM_SLOTS * 3) {
            XSync(P.dpy, False);
            shm_drain();
        }
        return;
    }

    /* Fallback: reuse one XImage that points at the scaled buffer. */
    if (!P.fallback || P.fallback->width != dw || P.fallback->height != dh) {
        if (P.fallback) {
            P.fallback->data = NULL; /* the data belongs to P.scaled */
            XDestroyImage(P.fallback);
            P.fallback = NULL;
        }
        P.fallback = XCreateImage(P.dpy, P.vis, (unsigned)P.depth, ZPixmap, 0, (char *)P.scaled, (unsigned)dw,
                                  (unsigned)dh, 32, 0);
        if (!P.fallback) return;
        P.fallback->byte_order = LSBFirst;
        P.fallback->bitmap_bit_order = LSBFirst;
    }
    XPutImage(P.dpy, P.win, P.gc, P.fallback, 0, 0, 0, 0, (unsigned)dw, (unsigned)dh);
    XFlush(P.dpy);
}

int plat_verify_present(void)
{
    if (!P.dpy || !P.win || !P.scaled || P.scaled_w <= 0 || P.scaled_h <= 0) return -1;
    XWindowAttributes attrs;
    if (!XGetWindowAttributes(P.dpy, P.win, &attrs) || attrs.map_state != IsViewable) return -2;
    int w = P.scaled_w, h = P.scaled_h;
    P_last_x_error = 0;
    XImage *img = XGetImage(P.dpy, P.win, 0, 0, (unsigned)w, (unsigned)h, AllPlanes, ZPixmap);
    if (!img) {
        /* XWayland and some compositors answer BadMatch for a client window
         * (error 8).  Fall back to reading the root drawable at the window's
         * position, which is what the screen actually shows. */
        Window child = None;
        int rx = 0, ry = 0;
        if (XTranslateCoordinates(P.dpy, P.win, DefaultRootWindow(P.dpy), 0, 0, &rx, &ry, &child)) {
            P_last_x_error = 0;
            img = XGetImage(P.dpy, DefaultRootWindow(P.dpy), rx, ry, (unsigned)w, (unsigned)h, AllPlanes, ZPixmap);
        }
    }
    if (!img) {
        if (getenv("MSS_DEBUG_EVENTS"))
            fprintf(stderr, "[verify] read-back failed, last X error %d\n", P_last_x_error);
        return -1;
    }
    int match = 0;
    for (int y = 0; y < h; ++y) {
        const uint32_t *row = P.scaled + (size_t)y * (size_t)w;
        for (int x = 0; x < w; ++x) {
            if ((uint32_t)XGetPixel(img, x, y) == row[x]) match++;
        }
    }
    XDestroyImage(img);
    return match;
}

/* ------------------------------------------------------------------ timing */
double plat_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void plat_sleep(double seconds)
{
    if (seconds <= 0.0) return;
    struct timespec ts;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    if (ts.tv_nsec < 0) ts.tv_nsec = 0;
    if (ts.tv_nsec > 999999999L) ts.tv_nsec = 999999999L;
    nanosleep(&ts, NULL);
}

/* ---------------------------------------------------------------- helpers */
int plat_scale(void) { return P.native_wayland ? sdlplat_scale() : P.scale; }

void plat_screen_size(int *w, int *h)
{
    *w = 1280;
    *h = 800;
    Display *d = P.dpy ? P.dpy : XOpenDisplay(NULL);
    if (!d) return;
    int s = DefaultScreen(d);
    *w = DisplayWidth(d, s);
    *h = DisplayHeight(d, s);
    if (!P.dpy) XCloseDisplay(d);
}

int plat_headless(void) { return P.headless; }

int plat_focus_self(void)
{
    if (P.native_wayland) return sdlplat_focus();
    if (!P.dpy || !P.win) return -1;
    /* Only a viewable window can take the input focus (otherwise the server
     * raises BadMatch, which we would rather avoid than provoke). */
    XWindowAttributes attrs;
    if (!XGetWindowAttributes(P.dpy, P.win, &attrs)) return -1;
    if (attrs.map_state != IsViewable) return -1;
    XSetInputFocus(P.dpy, P.win, RevertToParent, CurrentTime);
    XSync(P.dpy, False);
    Window focused = None;
    int revert = 0;
    XGetInputFocus(P.dpy, &focused, &revert);
    return focused == P.win ? 0 : -1;
}

void plat_mode_label(char *out, int cap)
{
    if (P.native_wayland) { sdlplat_mode(out, cap); return; }
    if (cap <= 0) return;
    snprintf(out, (size_t)cap, "%s", P.mode);
}

int plat_write_ppm(const char *path, const uint32_t *px, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    /* Write in row chunks to keep the stack tiny. */
    uint8_t row[384 * 3];
    for (int y = 0; y < h; ++y) {
        const uint32_t *src = px + (size_t)y * (size_t)w;
        int x = 0;
        while (x < w) {
            int n = w - x;
            if (n > 384) n = 384;
            for (int i = 0; i < n; ++i) {
                uint32_t c = src[x + i];
                row[i * 3 + 0] = (uint8_t)((c >> 16) & 0xFF);
                row[i * 3 + 1] = (uint8_t)((c >> 8) & 0xFF);
                row[i * 3 + 2] = (uint8_t)(c & 0xFF);
            }
            if (fwrite(row, 3u, (size_t)n, f) != (size_t)n) { fclose(f); return -1; }
            x += n;
        }
    }
    if (fclose(f) != 0) return -1;
    return 0;
}

int plat_snprintf(char *dst, int cap, const char *fmt, ...)
{
    if (cap <= 0) return 0;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst, (size_t)cap, fmt, ap);
    va_end(ap);
    return n < 0 ? 0 : n;
}
