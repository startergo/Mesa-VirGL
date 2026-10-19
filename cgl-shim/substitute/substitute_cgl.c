/*
 * substitute_cgl.c — CGL implementation for the substitute OpenGL.framework.
 *
 * In the substitute framework, these symbols are DEFINED (not interposed).
 * Consumers (XUL, Gecko, QuartzCore) resolve them via dyld binding or dlsym
 * — both find our definitions because we ARE the OpenGL framework.
 *
 * ~10 symbols that XUL actually uses have real implementations wrapping
 * OSMesa. The remaining ~52 CGL symbols are forwarded to the real OpenGL
 * via dlopen of a renamed copy (libOpenGL_real.dylib).
 *
 * NSOpenGLContext swizzling is in cgl_shim.mm (loaded via +load at
 * framework init). CGL context management here mirrors the shim_ctx
 * infrastructure that cgl_shim.mm's swizzle path uses.
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <unistd.h>

/* ObjC GC support marker. System Preferences (and other GC-required apps
 * on 10.6) abort if any loaded image lacks __objc_image_info with the
 * OBJC_IMAGE_SUPPORTS_GC flag. The cross-compiler on Apple Silicon can't
 * emit -fobjc-gc (deprecated post-10.8), so we inject the section
 * directly. The ObjC runtime finds it during image scanning.
 *
 * Format: version (uint32) + flags (uint32).
 * Bit 1 (0x2) = OBJC_IMAGE_SUPPORTS_GC. */
/* ObjC GC support marker. */
__attribute__((used, section("__DATA,__objc_imageinfo")))
const uint32_t _objc_image_info[2] = { 0, 2 };

/* Mesa headers — paired with Mesa's library, not Apple's GL */
#include <GL/gl.h>
#include <GL/osmesa.h>

/* CGL types from the SDK */
#include <OpenGL/CGLTypes.h>
#include <OpenGL/CGLCurrent.h>
#include <OpenGL/OpenGL.h>
#include <IOSurface/IOSurface.h>

/* Mesa function pointers (defined in cgl_interpose.c, populated by its
 * constructor via dlsym on libOSMesa). */
extern void   (*p_glPixelStorei)(GLenum, GLint);
extern void   (*p_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid *);
extern void   (*p_glGetFloatv)(GLenum, GLfloat *);
extern GLenum (*p_glGetError)(void);
extern void   (*p_glGetIntegerv)(GLenum, GLint *);

/* Private types not in public headers */
typedef void *CGLShareGroupObj;
typedef void (*CGLDispatchFunc)(void);

#include "cgl_shim.h"

/* -----------------------------------------------------------------------
 * Real OpenGL forwarding (dlopen of renamed real framework binary)
 *
 * The substitute framework deploys alongside a renamed copy of the real
 * OpenGL binary (libOpenGL_real.dylib). Symbols we don't implement are
 * forwarded via dlsym so QuartzCore/CoreVideo get real CGL behavior.
 * ----------------------------------------------------------------------- */

static void *g_real_gl = NULL;

/* -----------------------------------------------------------------------
 * TLS current-context, last-set-wins across BOTH occupant kinds
 * (2026-08-16, design pre-registered in LEDGER entry 5 before code):
 * token (shim) or real pointer (forwarded foreign contexts). Set by
 * CGLSetCurrentContext AND by every OSMesaMakeCurrent site via
 * substitute_cgl_tls_set(), so the swizzle path cannot leave a stale
 * real occupant. CGLGetCurrentContext trusts a REAL occupant from the
 * TSD directly; shim occupants and unset fall through to the existing
 * OSMesa-derivation.
 * ----------------------------------------------------------------------- */
static pthread_key_t g_tls_cgl;
/* CGL-emulation handoff serialization (2026-08-19): defined in
 * cgl_shim.mm — one global mutex making context switches atomic, with
 * owner tracking for HANDOFF-VIOLATION detection. */
extern void shim_osmesa_make_current(struct shim_ctx *sc);

static int g_tls_cgl_ok = 0;

void substitute_cgl_tls_set(CGLContextObj ctx)
{
    if (g_tls_cgl_ok) pthread_setspecific(g_tls_cgl, ctx);
}

__attribute__((constructor))
static void substitute_init(void)
{
    g_tls_cgl_ok = (pthread_key_create(&g_tls_cgl, NULL) == 0);

    /* Must run before any OSMesaCreateContextExt: Mesa's driver choice
     * is a process-global call_once keyed off GALLIUM_DRIVER. If the
     * first context is created first, the whole process silently lands
     * on softpipe (observed in PowerFox: GL_RENDERER "softpipe", GL 3.3,
     * shaders compile but compositing never reaches first paint under
     * TCG). The setenv previously sat in CGLCreateContext AFTER the
     * OSMesaCreateContextExt call — too late for the first context. */
    if (!getenv("GALLIUM_DRIVER"))
        setenv("GALLIUM_DRIVER", "virgl", 0);

    /* Try several rpath-relative names for the real GL copy */
    const char *paths[] = {
        "@rpath/libOpenGL_real.dylib",
        "@loader_path/libOpenGL_real.dylib",
        "@loader_path/../libOpenGL_real.dylib",
        "/tmp/libOpenGL_real.dylib",
        NULL
    };

    /* dlopen with @rpath/@loader_path doesn't work for plain strings;
     * we need to use the actual path. Try the most common locations. */
    g_real_gl = dlopen("@rpath/libOpenGL_real.dylib", RTLD_LAZY | RTLD_GLOBAL);
    if (!g_real_gl) g_real_gl = dlopen("@loader_path/libOpenGL_real.dylib", RTLD_LAZY | RTLD_GLOBAL);
    if (!g_real_gl) g_real_gl = dlopen("@loader_path/../libOpenGL_real.dylib", RTLD_LAZY | RTLD_GLOBAL);
    if (!g_real_gl) g_real_gl = dlopen("/tmp/libOpenGL_real.dylib", RTLD_LAZY | RTLD_GLOBAL);

    if (!g_real_gl) {
        fprintf(stderr, "substitute_cgl: WARNING — could not dlopen libOpenGL_real.dylib"
                " (CGL forwards will return errors). dlopen error: %s\n", dlerror());
    } else {
        fprintf(stderr, "substitute_cgl: libOpenGL_real loaded at %p\n", g_real_gl);
    }
}

/* Forward a CGL call to the real OpenGL. Lazy dlsym on first call.
 * Logs every call so we can see which forwards Gecko actually exercises —
 * observed rather than predicted. Same technique as the XUL symbol census. */
static int g_cgl_fwd_log = 1; /* set to 0 to suppress after the survey */
#define CGL_FORWARD(name, rettype, params, args) \
rettype name params { \
    static rettype (*real_fn) params; \
    if (g_cgl_fwd_log) fprintf(stderr, "CGL_FWD: %s\n", #name); \
    if (!real_fn && g_real_gl) real_fn = (rettype (*)params)dlsym(g_real_gl, #name); \
    if (real_fn) return real_fn args; \
    return (rettype)0; \
}

/* -----------------------------------------------------------------------
 * CGL symbols we IMPLEMENT (wrapping OSMesa for XUL/Gecko)
 *
 * These override the re-exported real CGL symbols because our own
 * definitions take precedence in the substitute's symbol table.
 * ----------------------------------------------------------------------- */

/* -----------------------------------------------------------------------
 * gl* no-context probe: log when gl* arrives with no current OSMesa
 * context. Answers the blast-radius question empirically — does
 * QuartzCore (or any other system framework) call gl* during PowerFox's
 * lifecycle? If nothing shows up, the blast radius is theoretical and
 * per-caller routing is unnecessary. Same technique that corrected the
 * CGLDescribePixelFormat hypothesis: measure which functions actually
 * fire, rather than reasoning about which might.
 *
 * Overrides the re-exported glClear for this one function only.
 * Forwards to Mesa regardless (Mesa's no-op dispatch handles no-context
 * calls gracefully). Gated to first 20 hits to avoid flooding. */
static int g_no_ctx_count = 0;
void glClear(GLbitfield mask) {
    if (!OSMesaGetCurrentContext() && g_no_ctx_count < 20) {
        g_no_ctx_count++;
        fprintf(stderr, "GL_NO_CTX: glClear mask=0x%x thread=%p (call %d)\n",
                mask, (void *)pthread_self(), g_no_ctx_count);
    }
    /* Forward to Mesa regardless — Mesa's no-op dispatch handles
     * no-context calls without crashing. */
    extern void (*p_glClear)(GLbitfield);
    if (p_glClear) p_glClear(mask);
}

/* -----------------------------------------------------------------------
 * THE WALK INSTRUMENT (Mesa-VirGL LEDGER entry 18; scissor falsified
 * — Gecko makes zero glScissor calls; FMTX falsified — transforms are
 * shader uniforms. Source read of Gecko 52's CompositorOGL/OGLShaderProgram
 * established the exact carriers of the destination data:
 *   SetLayerRects   → glUniform4fv(loc, 4, 16 floats)  DESTINATION rects
 *   SetTextureRects → glUniform4fv(loc, 4, 16 floats)  SOURCE rects
 *   uMatrixProj     → glUniformMatrix4fv               transform
 *   RenderTargetOffset → glUniform2f                   render offset
 *   draws           → glDrawArrays (TRIANGLES, 6*quads)
 *   textures        → glBindTexture
 * Each wrapper forwards to Mesa (lazy dlsym — the glScissor wrapper
 * pattern, proven live 2026-08-17) and logs under SHIM_WALK_LOG=1.
 *
 * NO value dedup by design: repetition is the thing being measured.
 * A total-line cap (WALK_CAP) bounds the file; the cap logs itself.
 *
 * PRE-REGISTERED DECODE (double-tab + duplicated grow-box, both via
 * RectTextureImage::Draw — MaybeDrawTitlebar / mResizerImage):
 *   1. same texture/source-rects, DIFFERENT LayerRects across two
 *      draws → destination offset measured in Gecko's quad data.
 *   2. identical LayerRects, different MatrixProj/RenderTargetOffset
 *      → transform-side offset.
 *   3. two draws with IDENTICAL texture+rects+matrices → Gecko issued
 *      the same draw twice (duplicate layer); shim-side EXHAUSTED.
 *   4. duplicate's LayerRects == the widget's PREVIOUS rect (visible
 *      as history: rect N-1 vs N) → STALE-DESTINATION redraw — ties
 *      to the entry-9 CGL contract (DoubleBuffer without BackingStore;
 *      undefined-after-swap on real 10.6, ever-persistent here) and
 *      the bounded ten-click verdict (only the last position survives).
 *      Direction check: strip duplicate was DOWN by its height;
 *      grow-box duplicate observed UP-AND-LEFT — consistent with
 *      "each duplicate at that widget's own old rect", NOT one shared
 *      translation.
 *
 * Export requirement identical to the GLU shim: without
 * -Wl,-exported_symbol these never fire (installation ≠ firing class).
 * ----------------------------------------------------------------------- */
static int walk_log_on(void)
{
    /* Master arm (env) AND a runtime window gate (marker file): the
     * first build logged unconditionally under the env and burned the
     * 100k cap during STARTUP (~hundreds of lines per composite) —
     * long before the artifact window. Now: SHIM_WALK_LOG=1 arms the
     * instrument, and logging runs only while /tmp/walk_on exists, so
     * the capture is windowed to exactly the trigger (touch/rm over
     * ssh; no relaunch needed). access() per call is cheap next to a
     * composite. */
    static int s_armed = -1;
    static int s_banner = 0;
    if (s_armed < 0)
        s_armed = (getenv("SHIM_WALK_LOG") != NULL);
    if (!s_armed) return 0;
    int window = (access("/tmp/walk_on", F_OK) == 0);
    if (window && !s_banner) {
        s_banner = 1;
        fprintf(stderr, "substitute_cgl: WALK INSTRUMENT ACTIVE "
                "(SHIM_WALK_LOG=1; window opened — /tmp/walk_on seen)\n");
    }
    return window;
}

#define WALK_CAP 100000
static unsigned long g_walk_lines = 0;
static void walk_line(const char *fmt, ...)
{
    if (g_walk_lines >= WALK_CAP) return;
    if (g_walk_lines == WALK_CAP - 1) {
        g_walk_lines++;
        fprintf(stderr, "WALK[%lu] CAP REACHED — logging stops\n",
                g_walk_lines);
        return;
    }
    g_walk_lines++;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void *walk_mesa_fn(const char *name)
{
    extern void *g_mesa;
    return g_mesa ? dlsym(g_mesa, name) : NULL;
}

/* diff instrument engine + vacate fix (defined after the walk
 * wrappers below) */
static int diff_armed(void);
static int vacate_on(void);
static int diff_active(void);
static void diff_record_u4fv(const GLfloat *v);
static void diff_commit_draw(void);

void glUniform4fv(GLint location, GLsizei count, const GLfloat *value)
{
    if (diff_active() && count == 4)
        diff_record_u4fv(value);
    if (walk_log_on()) {
        if (count == 4) {
            /* LayerRects / TextureRects: 4 rects, each x,y,w,h */
            walk_line("WALK[%lu] U4fv loc=%d rects: "
                      "(%.1f,%.1f %.1fx%.1f) (%.1f,%.1f %.1fx%.1f) "
                      "(%.1f,%.1f %.1fx%.1f) (%.1f,%.1f %.1fx%.1f)\n",
                      g_walk_lines, (int)location,
                      value[0], value[1], value[2], value[3],
                      value[4], value[5], value[6], value[7],
                      value[8], value[9], value[10], value[11],
                      value[12], value[13], value[14], value[15]);
        } else {
            walk_line("WALK[%lu] U4fv loc=%d count=%d v0..3=%.1f,%.1f,%.1f,%.1f\n",
                      g_walk_lines, (int)location, (int)count,
                      value[0], value[1], value[2], value[3]);
        }
    }
    static void (*fn)(GLint, GLsizei, const GLfloat *);
    if (!fn) fn = (void (*)(GLint, GLsizei, const GLfloat *))
        walk_mesa_fn("glUniform4fv");
    if (fn) fn(location, count, value);
}

void glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose,
                        const GLfloat *value)
{
    if (walk_log_on()) {
        walk_line("WALK[%lu] UMat loc=%d t=%d "
                  "row0=[%.2f %.2f %.2f %.2f] T=[%.2f %.2f %.2f %.2f]\n",
                  g_walk_lines, (int)location, transpose ? 1 : 0,
                  value[0], value[1], value[2], value[3],
                  value[12], value[13], value[14], value[15]);
    }
    static void (*fn)(GLint, GLsizei, GLboolean, const GLfloat *);
    if (!fn) fn = (void (*)(GLint, GLsizei, GLboolean, const GLfloat *))
        walk_mesa_fn("glUniformMatrix4fv");
    if (fn) fn(location, count, transpose, value);
}

/* RUNG 128e3: the scalar family the app actually calls — same
 * shape as the existing exports, forwarding via walk_mesa_fn,
 * first-24-calls logged to the stub log (the (B) uniform table). */
static int s_128e3 = 0;
static void u3log(const char* fn, GLint loc, int n,
                  const float* v)
{
    if (s_128e3 >= 24) return;
    s_128e3++;
    FILE* f = fopen("/var/log/vm_gld_stub.log", "a");
    if (!f) return;
    struct timeval tv; gettimeofday(&tv, 0);
    time_t t = (time_t)tv.tv_sec;
    char ts[32];
    strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&t));
    fprintf(f, "[%s pid=%d] rung128e: glUniform%s loc=%d:",
            ts, (int)getpid(), fn, (int)loc);
    for (int i = 0; i < n; i++)
        fprintf(f, " %.4g", (double)v[i]);
    fprintf(f, "\n");
    fclose(f);
}
void glUniform1f(GLint location, GLfloat x)
{
    float v[1] = { x }; u3log("1f", location, 1, v);
    static void (*fn)(GLint, GLfloat);
    if (!fn) fn = (void (*)(GLint, GLfloat))
        walk_mesa_fn("glUniform1f");
    if (fn) fn(location, x);
}
void glUniform3f(GLint location, GLfloat x, GLfloat y, GLfloat z)
{
    float v[3] = { x, y, z }; u3log("3f", location, 3, v);
    static void (*fn)(GLint, GLfloat, GLfloat, GLfloat);
    if (!fn) fn = (void (*)(GLint, GLfloat, GLfloat, GLfloat))
        walk_mesa_fn("glUniform3f");
    if (fn) fn(location, x, y, z);
}
void glUniform4f(GLint location, GLfloat x, GLfloat y,
                 GLfloat z, GLfloat w)
{
    float v[4] = { x, y, z, w }; u3log("4f", location, 4, v);
    static void (*fn)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
    if (!fn) fn = (void (*)(GLint, GLfloat, GLfloat,
                           GLfloat, GLfloat))
        walk_mesa_fn("glUniform4f");
    if (fn) fn(location, x, y, z, w);
}
void glUniform2f(GLint location, GLfloat x, GLfloat y)
{
    if (walk_log_on())
        walk_line("WALK[%lu] U2f loc=%d (%.1f,%.1f)\n",
                  g_walk_lines, (int)location, x, y);
    static void (*fn)(GLint, GLfloat, GLfloat);
    if (!fn) fn = (void (*)(GLint, GLfloat, GLfloat))
        walk_mesa_fn("glUniform2f");
    if (fn) fn(location, x, y);
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    if (diff_active())
        diff_commit_draw();
    if (walk_log_on())
        walk_line("WALK[%lu] DRAW mode=0x%x first=%d count=%d\n",
                  g_walk_lines, (unsigned)mode, (int)first, (int)count);
    static void (*fn)(GLenum, GLint, GLsizei);
    if (!fn) fn = (void (*)(GLenum, GLint, GLsizei))
        walk_mesa_fn("glDrawArrays");
    if (fn) fn(mode, first, count);
}

void glBindTexture(GLenum target, GLuint texture)
{
    if (walk_log_on())
        walk_line("WALK[%lu] BIND tex=%u (target 0x%x)\n",
                  g_walk_lines, (unsigned)texture, (unsigned)target);
    static void (*fn)(GLenum, GLuint);
    if (!fn) fn = (void (*)(GLenum, GLuint))
        walk_mesa_fn("glBindTexture");
    if (fn) fn(target, texture);
}

/* -----------------------------------------------------------------------
 * THE DIFF INSTRUMENT (pre-registered 2026-08-17, entry 18 follow-up;
 * duplication localized to virgl/host render-target persistence by the
 * softpipe bisect — this measures it mechanically, no visual reading).
 *
 * At present time, the presented buffer is compared against the UNION
 * of the current frame's draw rects (recorded by the walk wrappers:
 * the second-to-last 4×vec4 glUniform4fv before each glDrawArrays is
 * the LayerRects set — SetLayerRects precedes SetTextureRects):
 *   outside coverage + NON-BLACK  → stale content persisted from
 *                                   earlier frames = the ghost regions
 *   outside coverage + BLACK      → never drawn in current layout
 *                                   = the band regions
 * Reporting: 32-px tile aggregates merged into horizontal runs, with
 * per-frame covered/stale/blank pixel counts. Windowed like the walk
 * capture: SHIM_DIFF_LOG=1 arms; /tmp/walk_on opens the capture.
 *
 * Coordinate note: LayerRects are y-down (Gecko layer space; the grow
 * box logs at y≈895 in a 910-tall window); the readback buffer is
 * y-up (glReadPixels rows from the bottom) — rects are flipped when
 * marked. First run calibrates against the known titlebar rect
 * (0,0 1491x79 → buffer rows 831..909).
 * ----------------------------------------------------------------------- */
static int diff_armed(void)
{
    static int s = -1;
    if (s < 0) s = (getenv("SHIM_DIFF_LOG") != NULL);
    return s;
}

/* THE FIX (2026-08-17, pre-registered from the diff instrument's
 * measurement): vacated-rect clearing. Keep the previous frame's
 * coverage; pixels COVERED in frame N-1 but UNCOVERED in frame N are
 * vacated (the widget moved away) — zero them in the presented
 * buffer. Never-covered regions (the undrawn band Gecko depends on
 * for persistence — entry 9) are NEVER touched: only actually-
 * vacated pixels are discarded, which is what avoids the entry-9
 * full-clear breakage by construction. Env-gated:
 * SHIM_VACATE_CLEAR=1. Implies coverage recording (the hooks arm on
 * diff_active()). */
static int vacate_on(void)
{
    static int s = -1;
    if (s < 0) s = (getenv("SHIM_VACATE_CLEAR") != NULL);
    return s;
}

static int diff_active(void)
{
    return diff_armed() || vacate_on();
}

#define DIFF_MAX_LINES 5000
static unsigned long g_diff_lines = 0;

static unsigned char *g_cov = NULL;   /* w*h coverage, 1 = drawn this frame */
static unsigned char *g_cov_prev = NULL;  /* previous frame's coverage */
static int g_cov_w = 0, g_cov_h = 0;
static unsigned long g_cov_marked = 0;
static unsigned long g_draws_in_frame = 0;
static unsigned long g_vacated_px = 0;

/* ring of the last two count==4 U4fv rect sets */
static float g_rect_ring[2][16];
static int g_rect_ring_n = 0;
static pthread_mutex_t g_diff_mu = PTHREAD_MUTEX_INITIALIZER;

static void diff_mark_rect(float fx, float fy, float fw, float fh)
{
    if (!g_cov) return;
    int x = (int)fx, y = (int)fy, w = (int)fw, h = (int)fh;
    if (w <= 0 || h <= 0) return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    /* y-down layer space → y-up buffer rows */
    int row0 = g_cov_h - (y + h);      /* bottom row of the rect */
    if (row0 < 0) row0 = 0;
    int row1 = g_cov_h - y - 1;        /* top row of the rect */
    if (row1 > g_cov_h - 1) row1 = g_cov_h - 1;
    int col0 = x, col1 = x + w - 1;
    if (col1 > g_cov_w - 1) col1 = g_cov_w - 1;
    for (int r = row0; r <= row1; r++) {
        unsigned char *line = g_cov + (size_t)r * g_cov_w;
        for (int c = col0; c <= col1; c++)
            if (!line[c]) { line[c] = 1; g_cov_marked++; }
    }
}

/* Called from glUniform4fv (count==4) when armed. */
static void diff_record_u4fv(const GLfloat *v)
{
    pthread_mutex_lock(&g_diff_mu);
    memcpy(g_rect_ring[0], g_rect_ring[1], sizeof(g_rect_ring[0]));
    memcpy(g_rect_ring[1], v, sizeof(g_rect_ring[1]));
    g_rect_ring_n++;
    pthread_mutex_unlock(&g_diff_mu);
}

/* Called from glDrawArrays when armed: second-to-last set = LayerRects. */
static void diff_commit_draw(void)
{
    pthread_mutex_lock(&g_diff_mu);
    g_draws_in_frame++;
    if (g_rect_ring_n >= 2) {
        const float *lr = g_rect_ring[0];
        for (int i = 0; i < 4; i++)
            diff_mark_rect(lr[4*i], lr[4*i+1], lr[4*i+2], lr[4*i+3]);
    }
    pthread_mutex_unlock(&g_diff_mu);
}

#define DIFF_TILE 32
void cgl_shim_diff_at_present(const void *buf, int w, int h)
{
    /* Vacate-clearing runs EVERY frame when SHIM_VACATE_CLEAR is set
     * (it is the fix, not an instrument — no capture window). The
     * stale/blank REPORTING stays windowed (diff reporting needs
     * deliberate captures). */
    const int do_vacate = vacate_on();
    const int do_report = diff_armed() &&
                          (access("/tmp/walk_on", F_OK) == 0) &&
                          (g_diff_lines < DIFF_MAX_LINES);
    if (!do_vacate && !do_report) return;
    if (!buf || w <= 0 || h <= 0) return;

    pthread_mutex_lock(&g_diff_mu);
    if (!g_cov || g_cov_w != w || g_cov_h != h) {
        free(g_cov); free(g_cov_prev);
        g_cov = (unsigned char *)malloc((size_t)w * h);
        g_cov_prev = (unsigned char *)calloc((size_t)w * h, 1);
        if (!g_cov || !g_cov_prev) {
            free(g_cov); free(g_cov_prev);
            g_cov = NULL; g_cov_prev = NULL;
            pthread_mutex_unlock(&g_diff_mu); return;
        }
        memset(g_cov, 0, (size_t)w * h);
        g_cov_w = w; g_cov_h = h; g_cov_marked = 0;
    }

    const unsigned char *px = (const unsigned char *)buf;
    const size_t total = (size_t)w * h;

    if (do_vacate) {
        /* vacated = covered last frame, uncovered now → zero the
         * pixels in the presented buffer (transparent → composites
         * black; a redraw overwrites). Never-covered pixels are
         * untouched — the entry-9 persistence Gecko depends on. */
        unsigned char *pbuf = (unsigned char *)buf;
        g_vacated_px = 0;
        for (size_t i = 0; i < total; i++) {
            if (g_cov_prev[i] && !g_cov[i]) {
                unsigned char *p = pbuf + i * 4;
                p[0] = p[1] = p[2] = p[3] = 0;
                g_vacated_px++;
            }
        }
    }

    unsigned long stale = 0, blank = 0;
    /* tile grid of stale counts */
    int tw = (w + DIFF_TILE - 1) / DIFF_TILE;
    int th = (h + DIFF_TILE - 1) / DIFF_TILE;
    static unsigned int *tiles = NULL;
    static int tiles_n = 0;
    if (tiles_n < tw * th) {
        free(tiles);
        tiles = (unsigned int *)calloc((size_t)tw * th, sizeof(unsigned int));
        tiles_n = tw * th;
    } else {
        memset(tiles, 0, (size_t)tw * th * sizeof(unsigned int));
    }

    for (size_t i = 0; i < total; i++) {
        if (g_cov[i]) continue;
        const unsigned char *p = px + i * 4;
        if (p[0] > 8 || p[1] > 8 || p[2] > 8) {
            stale++;
            tiles[(i / w) / DIFF_TILE * tw + (i % w) / DIFF_TILE]++;
        } else {
            blank++;
        }
    }

    fprintf(stderr, "DIFF[%lu] %dx%d draws=%lu covered=%lu (%.1f%%) "
            "STALE=%lu BLANK=%lu VACATED=%lu\n",
            g_diff_lines, w, h, g_draws_in_frame,
            g_cov_marked, total ? 100.0 * g_cov_marked / total : 0.0,
            stale, blank, g_vacated_px);

    /* merge horizontally-adjacent stale tiles into runs */
    int reported = 0;
    for (int tr = 0; tr < th && reported < 24; tr++) {
        int tc = 0;
        while (tc < tw && reported < 24) {
            if (tiles[tr * tw + tc] >= DIFF_TILE) {
                int tc0 = tc;
                int best = tiles[tr * tw + tc];
                while (tc < tw && tiles[tr * tw + tc] >= DIFF_TILE) {
                    if (tiles[tr * tw + tc] > best)
                        best = tiles[tr * tw + tc];
                    tc++;
                }
                /* convert tile row to y-up screen coords for readability */
                int y0 = h - (tr + 1) * DIFF_TILE;
                int y1 = h - tr * DIFF_TILE - 1;
                if (y0 < 0) y0 = 0;
                fprintf(stderr, "  STALE-RUN y=%d..%d x=%d..%d peak=%u\n",
                        y0, y1, tc0 * DIFF_TILE, tc * DIFF_TILE - 1, best);
                reported++;
            } else {
                tc++;
            }
        }
    }

    if (do_report)
        g_diff_lines++;
    /* reset per-frame state for the next frame: prev <- cov, cov <- 0 */
    memcpy(g_cov_prev, g_cov, total);
    memset(g_cov, 0, total);
    g_cov_marked = 0;
    g_draws_in_frame = 0;
    g_rect_ring_n = 0;
    pthread_mutex_unlock(&g_diff_mu);
}

/* Pixel format: use a static token. XUL calls CGLChoosePixelFormat then
 * passes the result to CGLCreateContext. We just need a non-NULL token
 * that CGLDestroyPixelFormat / CGLReleasePixelFormat accept. */
static int g_pixel_format_token = 1;
#define PIXEL_FORMAT_TOKEN ((CGLPixelFormatObj)&g_pixel_format_token)

CGLError
CGLChoosePixelFormat(const CGLPixelFormatAttribute *attribs,
                     CGLPixelFormatObj *pix, GLint *npix)
{
    if (!pix) return kCGLBadAddress;
    *pix = PIXEL_FORMAT_TOKEN;
    if (npix) *npix = 1;
    return kCGLNoError;
}

CGLError
CGLDestroyPixelFormat(CGLPixelFormatObj pix)
{
    /* Static token — nothing to free */
    return kCGLNoError;
}

void
CGLReleasePixelFormat(CGLPixelFormatObj pix)
{
    /* Static token — nothing to free */
}

CGLError
CGLCreateContext(CGLPixelFormatObj pix, CGLContextObj share, CGLContextObj *ctx)
{
    if (!ctx) return kCGLBadAddress;

    /* SHARELIST FIX (2026-08-19 — see cgl_shim.mm's shim_initWithFormat
     * for the full chain): the `share` argument was received and dropped
     * (NULL below), putting every OSMesa context in a private share
     * group. Gecko's chrome compositing shares objects across contexts;
     * a texture created in one was non-existent in the other →
     * incomplete framebuffer → empty readback → transparent frames →
     * blit-skip → white window. In the substitute world a CGLContextObj
     * IS a shim_ctx pointer, so the share's OSMesa context is directly
     * reachable. */
    struct shim_ctx *share_sc = share ? shim_lookup(share) : NULL;
    OSMesaContext share_osmesa = share_sc ? share_sc->osmesa_ctx : NULL;

    /* Create OSMesa context */
    OSMesaContext osmesa = OSMesaCreateContextExt(OSMESA_RGBA, 16, 0, 0,
                                                  share_osmesa);
    if (!osmesa) {
        fprintf(stderr, "substitute_cgl: CGLCreateContext OSMesaCreateContextExt FAILED\n");
        return kCGLBadContext;
    }

    /* Wrap in shim_ctx (reuses Phase 1 infrastructure) */
    struct shim_ctx *sc = (struct shim_ctx *)calloc(1, sizeof(struct shim_ctx));
    if (!sc) {
        OSMesaDestroyContext(osmesa);
        return kCGLBadAlloc;
    }

    sc->magic = SHIM_MAGIC;
    sc->osmesa_ctx = osmesa;

    /* Allocate initial 1x1 buffer (will be resized on setView/makeCurrent) */
    sc->width = 1;
    sc->height = 1;
    substitute_cgl_tls_set((CGLContextObj)sc);   /* initial bind at creation */
    sc->render_buf = calloc(1, 4);  /* 1x1 RGBA */
    sc->present_buf = calloc(1, 4);
    pthread_mutex_init(&sc->lock, NULL);
    pthread_mutex_init(&sc->cgl_lock, NULL);
    sc->last_view_w = -1;
    sc->last_view_h = -1;

    shim_osmesa_make_current(sc);   /* creation bind — uniform path */

    shim_register(sc);

    /* Ensure GALLIUM_DRIVER is set */
    if (!getenv("GALLIUM_DRIVER"))
        setenv("GALLIUM_DRIVER", "virgl", 0);

    *ctx = (CGLContextObj)sc;
    fprintf(stderr, "substitute_cgl: CGLCreateContext -> ctx=%p osmesa=%p\n",
            (void *)*ctx, (void *)osmesa);
    return kCGLNoError;
}

CGLError
CGLDestroyContext(CGLContextObj ctx)
{
    struct shim_ctx *sc = shim_lookup(ctx);
    if (sc) {
        /* PRRT_kwDOPZhcm86uEfZS (PR review): destroying the CURRENT
         * context is legal — real CGL then has none current. Clear BOTH
         * bindings this file maintains BEFORE unregistering and freeing:
         * the OSMesa/GLAPI current (else every later gl* call dispatches
         * through the destroyed Mesa context) and the substitute TSD
         * (else CGLGetCurrentContext's TSD path answers the freed
         * pointer as a "foreign" context). */
        if (sc->osmesa_ctx && OSMesaGetCurrentContext() == sc->osmesa_ctx)
            shim_osmesa_make_current(NULL);
        if (g_tls_cgl_ok &&
            pthread_getspecific(g_tls_cgl) == (void *)sc)
            substitute_cgl_tls_set(NULL);
        shim_unregister(sc);
        if (sc->osmesa_ctx) OSMesaDestroyContext(sc->osmesa_ctx);
        free(sc->render_buf);
        free(sc->present_buf);
        pthread_mutex_destroy(&sc->lock);
        free(sc);
    }
    return kCGLNoError;
}

CGLContextObj
CGLGetCurrentContext(void)
{
    /* REAL occupant from the TSD wins — a foreign context made current
     * via the forwarded CGLSetCurrentContext must be answered with
     * itself, not the stale Mesa-TLS derivation (LEDGER entry 5). */
    if (g_tls_cgl_ok) {
        CGLContextObj t = (CGLContextObj)pthread_getspecific(g_tls_cgl);
        if (t && !shim_lookup(t)) {
            static int s_gcc_log = 0;
            if (s_gcc_log < 12) {
                s_gcc_log++;
                fprintf(stderr, "substitute_cgl: GetCurrentContext -> "
                        "%p (FOREIGN)\n", (void *)t);
            }
            return t;
        }
    }
    CGLContextObj r = NULL;

    /* OSMesaGetCurrentContext returns the OSMesaContext; we need to
     * return the shim_ctx that wraps it. The shim stores the current
     * context via the same mechanism as NSOpenGLContext's makeCurrent. */
    OSMesaContext osmc = OSMesaGetCurrentContext();
    if (!osmc) return NULL;

    /* Search registry for matching OSMesa context */
    pthread_mutex_lock(&shim_registry_lock);
    struct shim_ctx *found = NULL;
    for (int i = 0; i < MAX_SHIM_CONTEXTS; i++) {
        if (shim_registry[i] && shim_registry[i]->osmesa_ctx == osmc) {
            found = shim_registry[i];
            break;
        }
    }
    pthread_mutex_unlock(&shim_registry_lock);
    r = (CGLContextObj)found;
    {
        static int s_gcc_shim_log = 0;
        if (r && s_gcc_shim_log < 12) {
            s_gcc_shim_log++;
            fprintf(stderr, "substitute_cgl: GetCurrentContext -> %p "
                    "(shim)\n", (void *)r);
        }
    }
    return r;
}

CGLError
CGLSetCurrentContext(CGLContextObj ctx)
{
    if (!ctx) {
        shim_osmesa_make_current(NULL);
        substitute_cgl_tls_set(NULL);
        /* Clear the real side too, so neither kind lingers. */
        static CGLError (*real)(CGLContextObj);
        if (!real && g_real_gl) real = dlsym(g_real_gl, "CGLSetCurrentContext");
        if (real) real(NULL);
        return kCGLNoError;
    }

    struct shim_ctx *sc = shim_lookup(ctx);
    if (sc) {
        shim_osmesa_make_current(sc);
        substitute_cgl_tls_set(ctx);
        return kCGLNoError;
    }

    /* Foreign/real context (QuartzCore CARenderer etc.): FORWARD,
     * don't reject (policy fix 2026-08-16, LEDGER entry 5 — the old
     * kCGLBadContext here could permanently bar a real context from
     * becoming current; the IOSurface-present failure was its visible
     * symptom). Record in the TSD so GetCurrentContext answers with
     * whichever was set last. */
    static CGLError (*real)(CGLContextObj);
    if (!real && g_real_gl) real = dlsym(g_real_gl, "CGLSetCurrentContext");
    if (real) {
        CGLError r = real(ctx);
        static int s_fwd_log = 0;
        static long s_fwd_count = 0;
        s_fwd_count++;
        if (s_fwd_log < 10) {
            s_fwd_log++;
            fprintf(stderr, "substitute_cgl: CGLSetCurrentContext FOREIGN "
                    "ctx=%p forwarded -> %d (#%ld)\n", (void *)ctx, (int)r,
                    s_fwd_count);
        }
        if (r == kCGLNoError)
            substitute_cgl_tls_set(ctx);
        return r;
    }
    return kCGLBadContext;   /* no real GL loaded — unchanged */
}

CGLError
CGLEnable(CGLContextObj ctx, CGLContextEnable pname)
{
    /* OSMesa doesn't need CGLEnable — no-op for shim contexts */
    struct shim_ctx *sc = shim_lookup(ctx);
    if (sc) return kCGLNoError;
    /* Forward for non-shim contexts */
    static CGLError (*real)(CGLContextObj, CGLContextEnable);
    if (!real && g_real_gl) real = dlsym(g_real_gl, "CGLEnable");
    if (real) return real(ctx, pname);
    return kCGLNoError;
}

CGLError
CGLDisable(CGLContextObj ctx, CGLContextEnable pname)
{
    struct shim_ctx *sc = shim_lookup(ctx);
    if (sc) return kCGLNoError;
    static CGLError (*real)(CGLContextObj, CGLContextEnable);
    if (!real && g_real_gl) real = dlsym(g_real_gl, "CGLDisable");
    if (real) return real(ctx, pname);
    return kCGLNoError;
}

CGLError
CGLLockContext(CGLContextObj ctx)
{
    struct shim_ctx *sc = shim_lookup(ctx);
    if (sc) {
        /* Uses cgl_lock, NOT sc->lock: Gecko locks the context around
         * -flushBuffer on the same thread (nsChildView::
         * DoRemoteComposition), so a shared mutex self-deadlocks the
         * first flush (observed 2026-08-13: compositor thread parked in
         * shim_flushBuffer's pthread_mutex_lock while itself holding
         * the lock via this function). */
        {
            static int s_lock_log = 0;
            if (s_lock_log < 10) {
                s_lock_log++;
                fprintf(stderr, "substitute_cgl: CGLLockContext ctx=%p thread=%p\n",
                        (void *)ctx, (void *)pthread_self());
            }
        }
        pthread_mutex_lock(&sc->cgl_lock);
        return kCGLNoError;
    }
    /* Forward for non-shim contexts */
    static CGLError (*real)(CGLContextObj);
    if (!real && g_real_gl) real = dlsym(g_real_gl, "CGLLockContext");
    if (real) return real(ctx);
    return kCGLNoError;
}

CGLError
CGLUnlockContext(CGLContextObj ctx)
{
    struct shim_ctx *sc = shim_lookup(ctx);
    if (sc) {
        pthread_mutex_unlock(&sc->cgl_lock);
        return kCGLNoError;
    }
    static CGLError (*real)(CGLContextObj);
    if (!real && g_real_gl) real = dlsym(g_real_gl, "CGLUnlockContext");
    if (real) return real(ctx);
    return kCGLNoError;
}

CGLError
CGLFlushDrawable(CGLContextObj ctx)
{
    /* CGLFlushDrawable = the DIRECT-CGL present (LEDGER entry 14):
     * apps that skip -flushBuffer on NSOpenGLContext call
     * CGLFlushDrawable(CGLGetCurrentContext()) — Flurry's shape
     * (swizzle installed, shim_flushBuffer never ran). For a shim
     * context, run THE SHIM'S OWN PRESENTATION (readback + swap +
     * drawRect trigger — implemented in cgl_shim.mm against this
     * shim_ctx), NOT a forward: third instance of the token rule
     * (CGL contexts, foreign CGLSetCurrentContext, now GLU/flush). */
    struct shim_ctx *sc = shim_lookup(ctx);
    if (sc) {
        extern void cgl_shim_present_from_cgl(void *sc);
        static int s_fd_log = 0;
        static long s_fd_count = 0;
        s_fd_count++;
        if (s_fd_log < 8) {
            s_fd_log++;
            fprintf(stderr, "substitute_cgl: CGLFlushDrawable SHIM ctx "
                    "→ shim present (#%ld)\n", s_fd_count);
        }
        cgl_shim_present_from_cgl(sc);
        return kCGLNoError;
    }
    static CGLError (*real)(CGLContextObj);
    if (!real && g_real_gl) real = dlsym(g_real_gl, "CGLFlushDrawable");
    if (real) return real(ctx);
    return kCGLNoError;
}

/* -----------------------------------------------------------------------
 * CGL symbols we FORWARD to the real OpenGL (QuartzCore, CoreVideo, etc.)
 *
 * These are generated from the full 62-symbol CGL export list, minus
 * the ~12 we implement above. Each is a lazy dlsym forward.
 * ----------------------------------------------------------------------- */

CGL_FORWARD(CGLAreContextsShared, CGLError, (CGLContextObj ctx1, CGLContextObj ctx2, GLint *result), (ctx1, ctx2, result))
CGL_FORWARD(CGLClearDrawable, CGLError, (CGLContextObj ctx), (ctx))
CGL_FORWARD(CGLCopyContext, CGLError, (CGLContextObj src, CGLContextObj dst, GLuint mask), (src, dst, mask))
CGL_FORWARD(CGLCreatePBuffer, CGLError, (GLint width, GLint height, GLenum target, GLenum internalFormat, GLint max, CGLPBufferObj *pbuffer), (width, height, target, internalFormat, max, pbuffer))
CGL_FORWARD(CGLDescribePBuffer, CGLError, (CGLPBufferObj pbuffer, GLint *width, GLint *height, GLenum *target, GLenum *internalFormat, GLint *mipmap), (pbuffer, width, height, target, internalFormat, mipmap))
CGL_FORWARD(CGLDescribePixelFormat, CGLError, (CGLPixelFormatObj pix, GLint pix_num, CGLPixelFormatAttribute attrib, GLint *value), (pix, pix_num, attrib, value))
CGL_FORWARD(CGLDescribeRenderer, CGLError, (CGLRendererInfoObj rend, GLint rend_num, CGLRendererProperty prop, GLint *val), (rend, rend_num, prop, val))
CGL_FORWARD(CGLDestroyPBuffer, CGLError, (CGLPBufferObj pbuffer), (pbuffer))
CGL_FORWARD(CGLDestroyRendererInfo, CGLError, (CGLRendererInfoObj rend), (rend))
CGL_FORWARD(CGLErrorString, const char *, (CGLError error), (error))
CGL_FORWARD(CGLGetContextRetainCount, GLuint, (CGLContextObj ctx), (ctx))
CGL_FORWARD(CGLGetGlobalOption, CGLError, (CGLGlobalOption pname, GLint *params), (pname, params))
CGL_FORWARD(CGLGetNextContext, CGLError, (CGLContextObj *ctx), (ctx))
CGL_FORWARD(CGLGetOffScreen, CGLError, (CGLContextObj ctx, GLsizei *width, GLsizei *height, GLint *rowbytes, void **baseaddr), (ctx, width, height, rowbytes, baseaddr))
CGL_FORWARD(CGLGetOption, CGLError, (CGLGlobalOption pname, GLint *params), (pname, params))
CGL_FORWARD(CGLGetPBuffer, CGLError, (CGLContextObj ctx, CGLPBufferObj *pbuffer, GLenum *face, GLint *level, GLint *screen), (ctx, pbuffer, face, level, screen))
CGL_FORWARD(CGLGetPBufferRetainCount, GLuint, (CGLPBufferObj pbuffer), (pbuffer))
CGL_FORWARD(CGLGetParameter, CGLError, (CGLContextObj ctx, CGLContextParameter pname, GLint *params), (ctx, pname, params))
CGL_FORWARD(CGLGetPixelFormat, CGLPixelFormatObj, (CGLContextObj ctx), (ctx))
CGL_FORWARD(CGLGetPixelFormatRetainCount, GLuint, (CGLPixelFormatObj pix), (pix))
CGL_FORWARD(CGLGetShareGroup, CGLError, (CGLContextObj ctx, CGLShareGroupObj *group), (ctx, group))
CGL_FORWARD(CGLGetSurface, CGLError, (CGLContextObj ctx, GLuint *name, GLenum *type, GLint *width, GLint *height), (ctx, name, type, width, height))
CGL_FORWARD(CGLGetVersion, void, (GLint *major, GLint *minor), (major, minor))
CGL_FORWARD(CGLGetVirtualScreen, CGLError, (CGLContextObj ctx, GLint *screen), (ctx, screen))
CGL_FORWARD(CGLIsEnabled, CGLError, (CGLContextObj ctx, CGLContextEnable pname, GLint *enable), (ctx, pname, enable))
CGL_FORWARD(CGLQueryRendererInfo, CGLError, (GLuint display_mask, CGLRendererInfoObj *rend, GLint *nrend), (display_mask, rend, nrend))
/* 2026-10-07: NOT forwarded — the app-side NSOpenGLContext dealloc
 * releases libOpenGL_real's context into the documented GLEngine
 * destroy-path bug (gleFreeTextureState abort). Swallow it; the shim's
 * own teardown rides CGLDestroyContext. */
void CGLReleaseContext(CGLContextObj ctx)
{
    struct shim_ctx *sc = shim_lookup(ctx);
    if (sc) shim_unregister(sc);
    (void)ctx;
}
CGL_FORWARD(CGLReleasePBuffer, void, (CGLPBufferObj pbuffer), (pbuffer))
CGL_FORWARD(CGLRetainContext, CGLContextObj, (CGLContextObj ctx), (ctx))
CGL_FORWARD(CGLRetainPBuffer, CGLPBufferObj, (CGLPBufferObj pbuffer), (pbuffer))
CGL_FORWARD(CGLRetainPixelFormat, CGLPixelFormatObj, (CGLPixelFormatObj pix), (pix))
CGL_FORWARD(CGLSetFullScreen, CGLError, (CGLContextObj ctx), (ctx))
CGL_FORWARD(CGLSetFullScreenOnDisplay, CGLError, (CGLContextObj ctx, GLuint display_mask), (ctx, display_mask))
CGL_FORWARD(CGLSetGlobalOption, CGLError, (CGLGlobalOption pname, const GLint *params), (pname, params))
CGL_FORWARD(CGLSetOffScreen, CGLError, (CGLContextObj ctx, GLsizei width, GLsizei height, GLint rowbytes, void *baseaddr), (ctx, width, height, rowbytes, baseaddr))
CGL_FORWARD(CGLSetOption, CGLError, (CGLGlobalOption pname, GLint param), (pname, param))
CGL_FORWARD(CGLSetPBuffer, CGLError, (CGLContextObj ctx, CGLPBufferObj pbuffer, GLenum face, GLint level, GLint screen), (ctx, pbuffer, face, level, screen))
CGL_FORWARD(CGLSetPBufferVolatileState, CGLError, (CGLContextObj ctx, GLenum state), (ctx, state))
/* CGLSetParameter: observed firing during Gecko GL init with a shim token.
 * Must not forward the token to real OpenGL — Apple dereferences it.
 * Registry lookup: no-op for shim contexts, forward for real ones. */
CGLError
CGLSetParameter(CGLContextObj ctx, CGLContextParameter pname, const GLint *params)
{
    struct shim_ctx *sc = shim_lookup(ctx);
    if (sc) return kCGLNoError;
    static CGLError (*real)(CGLContextObj, CGLContextParameter, const GLint *);
    if (!real && g_real_gl) real = dlsym(g_real_gl, "CGLSetParameter");
    if (real) return real(ctx, pname, params);
    return kCGLNoError;
}
/* CGLSetSurface — MILESTONE 3 (2026-08-21): the system hands us the
 * WINDOW'S CGS SURFACE ID here (docs/ga-cfplugin.md). Intercepted, not
 * forwarded (the real Apple GL must not attach to our OSMesa context's
 * token): record the id, lazily start the GA session, and bind the
 * surface via AllocateSurface — the kernel relay then presents into
 * the WINDOW'S surface (single source of truth; WindowServer
 * composites; the flicker-era desktop-scanout writes retire). */
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/graphics/IOGraphicsInterface.h>

static struct {
    IOCFPlugInInterface** plug;
    void* obj;                          /* the COM object */
    IOGraphicsAcceleratorInterface* vt; /* vtable (deref once) */
    int state;                          /* 0=untried 1=ok 2=failed */
    IOBlitSurface surf;
    GLuint bound_id;
} g_ga;

static void ga_session_init(void)
{
    io_service_t fb;
    CFMutableDictionaryRef m;
    SInt32 score = 0;
    kern_return_t kr;

    if (g_ga.state != 0) return;
    g_ga.state = 2;

    m = IOServiceMatching("VMVirtIOFramebuffer");
    if (!m) return;
    fb = IOServiceGetMatchingService(kIOMasterPortDefault, m);
    if (fb == IO_OBJECT_NULL) return;

    kr = IOCreatePlugInInterfaceForService(
        fb, kIOGraphicsAcceleratorTypeID, kIOGraphicsAcceleratorInterfaceID,
        &g_ga.plug, &score);
    if (kr != KERN_SUCCESS || !g_ga.plug) return;

    HRESULT qrc = (*g_ga.plug)->QueryInterface(
        g_ga.plug, CFUUIDGetUUIDBytes(kIOGraphicsAcceleratorInterfaceID),
        (LPVOID*)&g_ga.obj);
    if (qrc != S_OK || !g_ga.obj) return;

    /* COM shape: the object's first field is the vtable pointer. */
    g_ga.vt = *(IOGraphicsAcceleratorInterface***)g_ga.obj;

    if (g_ga.vt->Probe(g_ga.obj, NULL, fb, &score) != kIOReturnSuccess)
        return;
    if (g_ga.vt->Start(g_ga.obj, NULL, fb) != kIOReturnSuccess)
        return;

    memset(&g_ga.surf, 0, sizeof(g_ga.surf));
    g_ga.surf.pixelFormat = kIO32BGRAPixelFormat;
    g_ga.state = 1;
    IOObjectRelease(fb);
}

CGLError CGLSetSurface(CGLContextObj ctx, GLuint read_write,
                       GLuint surface_id, GLenum surface_type,
                       GLint width, GLint height)
{
    (void)ctx; (void)read_write; (void)surface_type; (void)width; (void)height;
    ga_session_init();
    if (g_ga.state == 1 && surface_id && surface_id != g_ga.bound_id) {
        IOReturn rc = g_ga.vt->AllocateSurface(
            g_ga.obj, kIOBlitHasCGSSurface, &g_ga.surf, (void*)(uintptr_t)surface_id);
        if (rc == kIOReturnSuccess) {
            g_ga.bound_id = surface_id;
            fprintf(stderr, "substitute_cgl: GA surface %u BOUND — relay "
                            "presents into the window surface\n", surface_id);
        } else {
            fprintf(stderr, "substitute_cgl: GA AllocateSurface(%u) -> 0x%x\n",
                    surface_id, rc);
        }
    }
    return kCGLNoError;
}
CGL_FORWARD(CGLSetVirtualScreen, CGLError, (CGLContextObj ctx, GLint screen), (ctx, screen))
/* CGLTexImageIOSurface2D: implemented as a CPU upload into the current
 * OSMesa context. Never forwarded — forwarding passes our shim_ctx
 * token where real CGL dereferences a CGLContextObj.
 *
 * Gecko's mac pixel input path: it renders into an IOSurface and binds
 * it here. Without this, the compositor's textures stay empty and
 * everything composites black (observed 2026-08-13: blit visible,
 * content black). Details that matter:
 *  - HONOUR the caller's target: Gecko binds rectangle textures
 *    (GL_TEXTURE_RECTANGLE_ARB) and its shaders use sampler2DRect;
 *    assuming GL_TEXTURE_2D draws nothing with no GL error.
 *  - HONOUR format/type: Gecko's surfaces are BGRA with
 *    GL_UNSIGNED_INT_8_8_8_8_REV.
 *  - IOSurfaceLock with kIOSurfaceLockReadOnly.
 * Cost note: this converts Gecko's zero-copy path into a full CPU copy
 * per texture per frame, on top of the flushBuffer readback — correct
 * first, optimize later. */
CGLError
CGLTexImageIOSurface2D(CGLContextObj ctx, GLenum target, GLenum internal_format,
                       GLsizei width, GLsizei height, GLenum format, GLenum type,
                       void *iosurface, GLuint plane)
{
    /* SHIM_DROP_UPLOAD (Mesa-VirGL LEDGER, drop test): early-return
     * BEFORE any upload — nothing is bound, nothing can be composited
     * from this path. Pre-registered outcomes:
     *   desktop unchanged except the duplicate strip VANISHES →
     *     the upload is redundant; suppression is the fix.
     *   chrome disappears / goes black → the path is load-bearing;
     *     the destination must be corrected properly. */
    {
        static int s_drop_upload = -1;
        if (s_drop_upload < 0)
            s_drop_upload = (getenv("SHIM_DROP_UPLOAD") != NULL);
        if (s_drop_upload) {
            static int s_du_log = 0;
            if (s_du_log < 5) {
                s_du_log++;
                fprintf(stderr, "substitute_cgl: DROP_UPLOAD: declining "
                        "CGLTexImageIOSurface2D %ldx%ld\n",
                        (long)width, (long)height);
            }
            return kCGLNoError;
        }
    }

    struct shim_ctx *sc = shim_lookup(ctx);
    if (sc) {
        IOSurfaceRef surf = (IOSurfaceRef)iosurface;
        if (!surf || plane != 0)
            return kCGLBadContext;

        /* MATRIX-ACCUMULATION PROBE (LEDGER entry 7, pre-registered):
         * N clicks → N+1 copies at 0,h,2h = a repeated translation
         * composing across frames — GL state persisting that Gecko
         * believes it resets. Leading suspect: matrix-management calls
         * (push/pop/loadidentity) no-op'ing outside the wrapper set
         * while transforms reach Mesa. Log the matrix stacks at each
         * upload; a translation component growing by ~h per call is
         * the whole answer. */
        if (p_glGetFloatv && p_glGetIntegerv) {
            static int s_mtx_log = 0;
            if (s_mtx_log < 24) {
                s_mtx_log++;
                GLfloat mv[16], pr[16];
                GLint mode = 0, vp[4] = {0, 0, 0, 0};
                p_glGetFloatv(GL_MODELVIEW_MATRIX, mv);
                p_glGetFloatv(GL_PROJECTION_MATRIX, pr);
                p_glGetIntegerv(GL_MATRIX_MODE, &mode);
                p_glGetIntegerv(GL_VIEWPORT, vp);
                fprintf(stderr, "substitute_cgl: MTX[%d] mode=%u "
                        "mv=[%.1f %.1f %.1f %.1f] pr=[%.1f %.1f %.1f %.1f] "
                        "vp=%d,%d %dx%d\n",
                        s_mtx_log, (unsigned)mode,
                        mv[12], mv[13], mv[14], mv[15],
                        pr[12], pr[13], pr[14], pr[15],
                        vp[0], vp[1], vp[2], vp[3]);
            }
        }

        if (!OSMesaGetCurrentContext() || !p_glTexImage2D || !p_glPixelStorei) {
            static int s_noc_log = 0;
            if (s_noc_log++ < 5)
                fprintf(stderr, "substitute_cgl: CGLTexImageIOSurface2D: no "
                        "current OSMesa ctx / fn ptrs — declining\n");
            return kCGLBadContext;
        }

        IOReturn kr = IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
        void *base = IOSurfaceGetBaseAddress(surf);
        size_t bpr = IOSurfaceGetBytesPerRow(surf);
        GLenum glerr = GL_NO_ERROR;
        if (kr == kIOReturnSuccess && base && width > 0 && height > 0) {
            size_t bpp = 4;
            if (format == GL_RGB) bpp = 3;
            /* else RGBA/BGRA 8-bit → 4 */

            /* SHIM_PATTERN_FILL — stride/duplication discriminator for the
             * PowerFox chrome-artifact class (Mesa-VirGL LEDGER entry 1;
             * pre-registered before first run). Overwrites the upload
             * source IN PLACE, using the SURFACE'S OWN stride and bpp, so
             * the pattern tests every hop DOWNSTREAM of this write:
             * teximage unpack → virgl encode → winsys transfer → kext →
             * host → display. Two modes:
             *   SHIM_PATTERN_FILL=1|checker — 8-colour 8-px checkerboard,
             *     palette[((x>>3)+(y>>3))&7]; at-a-glance triage. NOTE the
             *     periodicity blind spot: repeats every 16 px, so a
             *     translation by a multiple of 16 is invisible — and the
             *     duplication artifact IS a translation. Use =ramp to
             *     measure.
             *   SHIM_PATTERN_FILL=ramp — coordinate ramp; every pixel
             *     encodes its own SOURCE coordinates, so a screenshot
             *     gives measured offsets, not just a verdict:
             *       R = x & 0xFF,  G = y & 0xFF,
             *       B = ((x>>8) & 0x0F) | (((y>>8) & 0x0F) << 4)
             *     (the B tier exists because R alone repeats every 256 px
             *     — the same alias one level up as the checkerboard's 16;
             *     with B, coordinates decode uniquely to 4096²).
             *     Decoding: x = R + 256*(B & 0xF), y = G + 256*(B >> 4).
             * READ THE RESULT AS (pre-registered):
             *   staircase/shear (R drifting relative to y) → stride fault
             *     downstream (virgl unpack / transfer box / scatter pitch)
             *   duplicated regions whose encoded coordinates disagree with
             *     their screen position → composite destinations, and the
             *     disagreement IS the measured offset (read R,G,B, decode,
             *     subtract screen position)
             *   clean and singular → path exonerated; fault is in the
             *     content — redo with pattern as overlay, not replacement.
             * Channel note: written in the surface's byte order (BGRA
             * here); if the display chain swaps channel order the constant
             * tier moves with it — identify channels by structure (one
             * ramp-along-x, one ramp-along-y, one coarse), not by name. */
            static int s_pattern_mode = -1;   /* 0=off 1=checker 2=ramp */
            if (s_pattern_mode < 0) {
                const char *v = getenv("SHIM_PATTERN_FILL");
                if (v && (!strcmp(v, "ramp")))
                    s_pattern_mode = 2;
                else if (v && *v && strcmp(v, "0"))
                    s_pattern_mode = 1;
                else
                    s_pattern_mode = 0;
                if (s_pattern_mode)
                    fprintf(stderr, "substitute_cgl: PATTERN FILL ACTIVE "
                            "(SHIM_PATTERN_FILL=%s) — uploads replaced by "
                            "%s\n", v,
                            s_pattern_mode == 2 ? "coordinate RAMP"
                                                : "CHECKERBOARD");
            }
            if (s_pattern_mode) {
                static const unsigned char pal[8][3] = {
                    {0xff, 0x00, 0x00}, {0x00, 0xff, 0x00},
                    {0x00, 0x00, 0xff}, {0xff, 0xff, 0x00},
                    {0xff, 0x00, 0xff}, {0x00, 0xff, 0xff},
                    {0xff, 0xff, 0xff}, {0x80, 0x80, 0x80},
                };
                for (GLsizei y = 0; y < height; y++) {
                    unsigned char *row = (unsigned char *)base + (size_t)y * bpr;
                    for (GLsizei x = 0; x < width; x++) {
                        unsigned char *px = row + (size_t)x * bpp;
                        if (s_pattern_mode == 2) {
                            /* BGRA byte order: B G R */
                            px[0] = (unsigned char)
                                ((((x >> 8) & 0x0F)) | ((((y >> 8) & 0x0F)) << 4));
                            px[1] = (unsigned char)(y & 0xFF);
                            px[2] = (unsigned char)(x & 0xFF);
                        } else {
                            const unsigned char *c =
                                pal[((x >> 3) + (y >> 3)) & 7];
                            px[0] = c[0]; px[1] = c[1]; px[2] = c[2];
                        }
                        if (bpp == 4) px[3] = 0xff;
                    }
                }
            }

            /* GL_UNPACK_* is CONTEXT-GLOBAL state: values set here
             * leak into the app's subsequent texture uploads unless
             * restored (2026-08-13: ALIGNMENT=1 was left installed
             * after every IOSurface upload, and ROW_LENGTH was reset
             * to a hardcoded 0 rather than the app's value. Harmless
             * for 4-byte/texel formats — rows are inherently
             * 4-aligned — but for sub-4-byte formats (glyph/alpha
             * atlases) with width%4!=0 the app's alignment-4 row
             * padding is misread, shifting each successive atlas row
             * horizontally. Signature: text at progressive horizontal
             * offsets clipped at consistent x edges; survives
             * single-buffering because it is texture content, not
             * buffer content.) */
            GLint save_align = 4, save_rowlen = 0;
            p_glGetIntegerv(GL_UNPACK_ALIGNMENT, &save_align);
            p_glGetIntegerv(GL_UNPACK_ROW_LENGTH, &save_rowlen);
            p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            p_glPixelStorei(GL_UNPACK_ROW_LENGTH,
                            (GLint)(bpp ? bpr / bpp : bpr));
            p_glTexImage2D(target, 0, (GLint)internal_format,
                           width, height, 0, format, type, base);
            p_glPixelStorei(GL_UNPACK_ROW_LENGTH, save_rowlen);
            p_glPixelStorei(GL_UNPACK_ALIGNMENT, save_align);
            glerr = p_glGetError();
            IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
        }
        static int s_iosf_log = 0;
        static long s_iosf_calls = 0;
        s_iosf_calls++;
        /* Log first 5 small calls unconditionally; every call for large
         * surfaces (w>=1000) — the main-window content surface must not
         * hide behind the small-call cap (instrument error 2026-08-13:
         * "no window-sized upload" was concluded from a capped log). */
        if (s_iosf_log < 5 || width >= 1000) {
            if (s_iosf_log < 5) s_iosf_log++;
            fprintf(stderr, "substitute_cgl: IOSurface2D #%ld ctx=%p "
                    "target=0x%x fmt=0x%x type=0x%x %ldx%ld bpr=%lu "
                    "glErr=0x%x lock=0x%x\n",
                    s_iosf_calls, (void *)ctx, target, format, type,
                    (long)width, (long)height, (unsigned long)bpr,
                    glerr, kr);
        }
        return (glerr == GL_NO_ERROR) ? kCGLNoError : kCGLBadContext;
    }
    static CGLError (*real)(CGLContextObj, GLenum, GLenum, GLsizei, GLsizei,
                            GLenum, GLenum, void *, GLuint);
    if (!real && g_real_gl) real = dlsym(g_real_gl, "CGLTexImageIOSurface2D");
    if (real) return real(ctx, target, internal_format, width, height,
                          format, type, iosurface, plane);
    return kCGLBadContext;
}
CGL_FORWARD(CGLTexImagePBuffer, CGLError, (CGLContextObj ctx, CGLPBufferObj pbuffer, GLenum source), (ctx, pbuffer, source))
CGL_FORWARD(CGLUpdateContext, CGLError, (CGLContextObj ctx), (ctx))

/* Private/dispatch CGL symbols — forward all */
CGL_FORWARD(CGLBackDispatch, CGLError, (CGLContextObj ctx, GLuint dtype, const void *data, GLsizei size), (ctx, dtype, data, size))
CGL_FORWARD(CGLFrontDispatch, CGLError, (CGLContextObj ctx, GLuint dtype, const void *data, GLsizei size), (ctx, dtype, data, size))
CGL_FORWARD(CGLOpenCLMuxLockDown, CGLError, (void), ())
CGL_FORWARD(CGLRestoreDispatch, CGLError, (CGLContextObj ctx), (ctx))
CGL_FORWARD(CGLRestoreDispatchFunction, CGLError, (CGLContextObj ctx), (ctx))
CGL_FORWARD(CGLSelectDispatch, CGLError, (CGLContextObj ctx, GLuint dtype), (ctx, dtype))
CGL_FORWARD(CGLSelectDispatchBounded, CGLError, (CGLContextObj ctx, GLuint dtype, GLsizei size), (ctx, dtype, size))
CGL_FORWARD(CGLSelectDispatchFunction, CGLError, (CGLContextObj ctx, GLuint dtype, CGLDispatchFunc func), (ctx, dtype, func))
