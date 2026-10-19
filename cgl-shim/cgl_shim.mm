/*
 * cgl_shim.mm — NSOpenGLContext swizzle layer for OSMesa+virgl.
 *
 * Intercepts 9 NSOpenGLContext methods to route GL rendering through
 * OSMesa. Loaded via DYLD_INSERT_LIBRARIES. The +load method installs
 * the swizzles at class-load time.
 *
 * Three design problems (see plan):
 *   1. Compositor-thread marshalling: double-buffer + lock + re-bind
 *   2. Buffer lifetime across resize: lock + realloc + bounds check
 *   3. CGLContextObj identity: registry (in cgl_interpose.c)
 */

// Guard out the SecKeychain macro that modern clang rejects on 10.6 SDK.
#define _SECURITY_SECKEYCHAIN_H_ 1

#import <Cocoa/Cocoa.h>
#import <OpenGL/OpenGL.h>
#import <QuartzCore/QuartzCore.h>
#include <objc/runtime.h>
#include <objc/objc-auto.h>
#include <GL/gl.h>
#include <GL/osmesa.h>
#include <pthread.h>
#include <sys/time.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>
#include <mach/mach_time.h>

#include "cgl_shim.h"

/* Probe target: libglapi export that returns the current thread's
 * _glapi_tls_Dispatch value. Used by dispcheck[shim] in flushBuffer to
 * compare against the interpose path's pointer (see diag_glViewport).
 * extern "C" guarded so the same source compiles as both .mm (interpose
 * build) and .m (substitute framework build). */
#ifdef __cplusplus
extern "C" {
#endif
const void *_glapi_get_dispatch(void);
const void *_glapi_get_context(void);
int _glapi_get_dispatch_table_size(void);

/* Mesa function pointers (defined in cgl_interpose.c, set by its
 * __attribute__((constructor)) gl_interpose_init). cgl_shim's own gl*
 * calls bind to OpenGL.framework (from nm -m), and DYLD_INTERPOSE does
 * NOT apply within the interposing image. So any gl* call from inside
 * cgl_shim.mm goes to Apple's GL, which has no current context. To
 * reach Mesa, we call through these p_* pointers, which were resolved
 * via dlsym(libOSMesa.8.dylib). */
extern void   (*p_glFinish)(void);
extern GLenum (*p_glGetError)(void);
extern void   (*p_glGetFloatv)(GLenum, GLfloat *);
extern void   (*p_glGetIntegerv)(GLenum, GLint *);
extern const GLubyte *(*p_glGetString)(GLenum);
extern void   (*p_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid *);
extern void   (*p_glPixelStorei)(GLenum, GLint);   /* set in cgl_interpose.c */

/* Internal readback with a fixed layout (review PRRT_kwDOPZhcm86YQzAU).
 * glReadPixels honours the context's GL_PACK_* state, which is the
 * APPLICATION's: a non-default PACK_ROW_LENGTH or PACK_SKIP_* both skews
 * the rows we read into render_buf and, with a row length larger than the
 * buffer width, writes past its end. Save the four pack parameters, set
 * the tightly-packed layout every readback site assumes, restore. Also
 * the single guard for the p_* pointers (review PRRT_kwDOPZhcm86Yova0):
 * gl_interpose_init can leave them NULL (dlopen/dlsym failure) and every
 * other call site in this file checks before calling. */
static void shim_readback(GLsizei w, GLsizei h, void *dst)
{
    if (!p_glReadPixels || !dst || w <= 0 || h <= 0) return;
    GLint pa = 4, prl = 0, psp = 0, psr = 0;
    int saved = (p_glGetIntegerv && p_glPixelStorei) ? 1 : 0;
    if (saved) {
        p_glGetIntegerv(GL_PACK_ALIGNMENT, &pa);
        p_glGetIntegerv(GL_PACK_ROW_LENGTH, &prl);
        p_glGetIntegerv(GL_PACK_SKIP_PIXELS, &psp);
        p_glGetIntegerv(GL_PACK_SKIP_ROWS, &psr);
        p_glPixelStorei(GL_PACK_ALIGNMENT, 1);
        p_glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        p_glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        p_glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    }
    p_glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, dst);
    if (saved) {
        p_glPixelStorei(GL_PACK_ALIGNMENT, pa);
        p_glPixelStorei(GL_PACK_ROW_LENGTH, prl);
        p_glPixelStorei(GL_PACK_SKIP_PIXELS, psp);
        p_glPixelStorei(GL_PACK_SKIP_ROWS, psr);
    }
}
/* TLS current-context mirror (LEDGER entry 5): every OSMesaMakeCurrent
 * site in THIS file must update the substitute's last-set-wins TSD so a
 * real (foreign) context cannot linger as a stale occupant. */
extern void substitute_cgl_tls_set(CGLContextObj ctx);
#ifdef __cplusplus
}
#endif

/* --- TLS for +currentContext via pthread_key (not __thread — ObjC
 * pointers don't work with -femulated-tls on this target) --- */
static pthread_key_t tls_key_current;
static pthread_once_t tls_key_once = PTHREAD_ONCE_INIT;
static void tls_key_init(void) { pthread_key_create(&tls_key_current, NULL); }

static inline NSOpenGLContext *get_current_ctx(void) {
    pthread_once(&tls_key_once, tls_key_init);
    return (NSOpenGLContext *)pthread_getspecific(tls_key_current);
}
static inline void set_current_ctx(NSOpenGLContext *ctx) {
    pthread_once(&tls_key_once, tls_key_init);
    pthread_setspecific(tls_key_current, ctx);
}

/* ===================================================================
 * CGL-EMULATION HANDOFF SERIALIZATION (2026-08-19) — the WindowServer
 * atomicity Gecko is entitled to. Real CGL contexts are server-side;
 * switching contexts is atomic w.r.t. every other thread. Our
 * in-process Mesa has _mesa_make_current performing the OUTGOING
 * context's implicit flush (st_glFlush → virgl_flush_eq → transfer
 * queue) before installing the new one — a window in which another
 * thread's MakeCurrent/draws on the same context race the flush. That
 * window is the startup program-not-linked family and the 19:16
 * transfer-queue crash (crash powerfox_2026-08-19-191644).
 *
 * LOCK ORDERING INVARIANT: flushBuffer's sc->lock → handoff →
 * (Mesa-internal queue lock). flushBuffer legitimately re-binds via
 * this helper while holding sc->lock; the handoff critical section
 * calls ONLY OSMesaMakeCurrent (pure Mesa — it cannot reach back into
 * any shim_ctx lock), so the order is never inverted.
 *
 * OWNING-THREAD TRACKING: per-thread current shim_ctx via pthread key
 * (10.6 has no usable __thread for this target). A MakeCurrent on a
 * context still owned by a DIFFERENT thread logs HANDOFF-VIOLATION —
 * concurrent same-context use, which real CGL tolerated server-side
 * and we do not. Zero violations across validation ⇒ switch-only
 * serialization is sufficient; violations ⇒ escalate to skip-flush.
 * =================================================================== */
static pthread_mutex_t g_gl_handoff = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t g_cur_sc_key;
static pthread_once_t g_cur_sc_once = PTHREAD_ONCE_INIT;
static void g_cur_sc_init(void) { pthread_key_create(&g_cur_sc_key, NULL); }

extern "C" void
shim_osmesa_make_current(struct shim_ctx *sc)
{
    pthread_once(&g_cur_sc_once, g_cur_sc_init);
    pthread_mutex_lock(&g_gl_handoff);
    struct shim_ctx *prev = (struct shim_ctx *)pthread_getspecific(g_cur_sc_key);
    if (prev && prev != sc) {
        prev->owned = 0;               /* this thread releases its old context */
    }
    if (sc) {
        if (sc->owned && !pthread_equal(sc->owner_th, pthread_self())) {
            static int s_viol = 0;
            if (s_viol < 10) {
                s_viol++;
                fprintf(stderr, "cgl_shim: HANDOFF-VIOLATION[%d]: context %p "
                        "still owned by another thread at MakeCurrent — "
                        "concurrent same-context use\n", s_viol, (void*)sc);
            }
        }
        sc->owned = 1;
        sc->owner_th = pthread_self();
        pthread_setspecific(g_cur_sc_key, sc);
        /* The ONLY raw OSMesaMakeCurrent — a bulk-regex once routed this
         * through the helper itself: self-recursion, self-deadlock on the
         * non-recursive mutex (the dock-stuck run, two stacked helper
         * frames in the sample). */
        OSMesaMakeCurrent(sc->osmesa_ctx, sc->render_buf, GL_UNSIGNED_BYTE,
                          sc->width, sc->height);
    } else {
        pthread_setspecific(g_cur_sc_key, NULL);
        OSMesaMakeCurrent(NULL, NULL, 0, 0, 0);
    }
    pthread_mutex_unlock(&g_gl_handoff);
}

/* --- HOST-SIDE BLIT support (2026-08-19): the window's desktop rect
 * and mode live in libOSMesa's winsys (one-way dependency — this
 * framework re-exports that library). The drawRect path (main thread,
 * AppKit-safe) calls the setter; flushBuffer gates readback on the
 * mode. --- */
/* Defined in libOSMesa's winsys; resolved via the same dlopen/dlsym
 * path as every other Mesa symbol (the substitute does not link
 * libOSMesa at build time). */
extern "C" void *g_mesa;
extern int  (*p_cgl_shim_host_present_enabled)(void);
extern void (*p_cgl_shim_set_host_present_rect)(int, int, int, int);
static inline int host_present_mode(void) {
    /* The mode is env-only — read locally, no cross-library call. The
     * dlsym'd winsys pointer failed to resolve in the first host-blit
     * run (0 HOST-BLIT rect lines with drawRect firing — silent NULL);
     * the mode gate must not depend on it. */
    static int m = -1;
    if (m < 0) m = (getenv("SHIM_HOST_PRESENT") != NULL);
    return m;
}
static inline void set_host_rect(int x, int y, int w, int h) {
    if (p_cgl_shim_set_host_present_rect)
        p_cgl_shim_set_host_present_rect(x, y, w, h);
    else {
        static int s_warn = 0;
        if (s_warn < 3) {
            s_warn++;
            fprintf(stderr, "cgl_shim: HOST-PRESENT rect setter UNRESOLVED "
                    "(dlsym failed) — blit cannot fire\n");
        }
    }
}

/* --- Associated-object key for per-NSOpenGLContext shim state --- */
static char kShimCtxKey;  /* address used as unique key, not the value */
#define SHIM_CTX_ASSOC_KEY (&kShimCtxKey)

static inline struct shim_ctx *get_shim_ctx(NSOpenGLContext *ctx)
{
    return (struct shim_ctx *)objc_getAssociatedObject(ctx, SHIM_CTX_ASSOC_KEY);
}

static inline void set_shim_ctx(NSOpenGLContext *ctx, struct shim_ctx *sc)
{
    objc_setAssociatedObject(ctx, SHIM_CTX_ASSOC_KEY, (id)sc, OBJC_ASSOCIATION_ASSIGN);
}

/* --- Buffer helpers --- */

/* SHIM_SINGLE_BUF=1 — diagnostic single-buffer mode (2026-08-13).
 * Gecko's compositor assumes the framebuffer RETAINS the previous
 * frame: on partial damage it redraws only the damaged quads and
 * leaves undamaged regions carrying frame N−1. The double-buffer
 * swap hands it the buffer holding frame N−2 instead, so partial
 * damage composites two frames' worth of content in one presented
 * frame (observed: the same uBlock description line rendered twice
 * at different horizontal offsets, superimposed). Single-buffer
 * mode points render_buf and present_buf at ONE allocation so
 * partial damage lands on the last presented frame by construction.
 * Expected cost: tearing (drawRect may read while the compositor
 * writes) — that is itself informative, not a defect of the test.
 * The swap in flushBuffer becomes a no-op when the pointers are
 * equal, so no other site needs guarding. */
static void alloc_buffers(struct shim_ctx *sc, int w, int h)
{
    size_t sz = (size_t)w * (size_t)h * 4;
    static int s_single = -1;
    if (s_single < 0)
        s_single = getenv("SHIM_SINGLE_BUF") ? 1 : 0;
    if (s_single) {
        sc->render_buf  = calloc(1, sz);
        sc->present_buf = sc->render_buf;
        fprintf(stderr, "cgl_shim: SINGLE-BUF mode (SHIM_SINGLE_BUF=1) "
                "buf=%p %dx%d — expect tearing, not doubling\n",
                (void *)sc->render_buf, w, h);
    } else {
        sc->render_buf  = calloc(1, sz);
        sc->present_buf = calloc(1, sz);
    }
    sc->width = w;
    sc->height = h;
}

/* associated-object keys (defined below, near shim_present) */
static char kPresentBufKey;
static char kPresentSizeKey;

static void free_buffers(struct shim_ctx *sc)
{
    /* Single-buffer mode: both pointers alias one allocation — free
     * once or we'd double-free. */
    if (sc->render_buf == sc->present_buf) {
        free(sc->render_buf);
    } else {
        free(sc->render_buf);
        free(sc->present_buf);
    }
    sc->render_buf = NULL;
    sc->present_buf = NULL;

    /* UAF guard (2026-08-18, crash in shim_blit_present_buf +217):
     * the main-thread drawRect reads the view's associated
     * PRESENT_BUF_KEY NSData WITHOUT the GL lock; after a resize-free
     * here, that object wraps freed memory until the next present
     * replaces it — the webgl page's resize churn crashed exactly in
     * that window. Nil the associated objects now so the blit's
     * no-buffer early-return covers the race at EVERY free site.
     * objc_setAssociatedObject is safe off-main (associated-object
     * table is lock-protected per object). */
    if (sc->view) {
        objc_setAssociatedObject((id)sc->view, &kPresentBufKey, nil,
                                 OBJC_ASSOCIATION_RETAIN);
        objc_setAssociatedObject((id)sc->view, &kPresentSizeKey, nil,
                                 OBJC_ASSOCIATION_RETAIN);
    }
}

/* --- Performance instrumentation (readback thread) ----------------------------
 * Six timestamps per frame isolate the synchronous readback cost:
 *   T1 (flushBuffer entry, post-resize)
 *   T2 (after glFinish)         submit_cmd cost = T2-T1
 *   T3 (after glReadPixels)     transfer cost   = T3-T2
 *   T4 (after dispatch_async)   lock/swap cost  = T4-T3
 *   T5 (shim_drawRect entry)
 *   T6 (after [rep drawInRect]) blit cost       = T6-T5
 *
 * Wall-clock frame-to-frame (T1[N] - T1[N-1]) captures everything the four
 * flushBuffer splits miss — the killtest render loop, CPU raster for softpipe,
 * inter-frame scheduling. softpipe's four splits sum small; the wall delta is
 * the denominator that makes the frame reconstructable.
 *
 * Time-based warmup discard: TCG translation dominates the first 10-15s and
 * would skew min/mean/max. Default 12s elapsed since first frame; override
 * with SHIM_TIMING_WARMUP_SECS. Frame-count gating was wrong because 15
 * frames at 8 fps (Flurry) barely covers 2s, while at 120 fps (killtest)
 * it covers 0.12s — neither matches the ramp.
 *
 * Transfer size logged per frame to confirm full-surface-always and size
 * lever 2 (dirty-region readback) in the same run. Per-frame log gated by
 * SHIM_TIMING env var; rolling stats every 30 frames always logged.
 */

#define SHIM_TIMING_WARMUP_DEFAULT_SECS 12.0
#define SHIM_TIMING_WINDOW 30

struct shim_timing_stats {
    uint64_t min_ticks, max_ticks, sum_ticks;
    uint32_t count;
};

struct shim_timing_state {
    uint64_t t1, t2, t3, t4;
    uint64_t last_t1;
    uint64_t first_frame_t1;
    int      has_last_t1;
    int      warmup_settled;
    uint32_t flush_count;
    uint32_t window_count;
    struct shim_timing_stats wall, submit, transfer, lock, blit;
};

static struct shim_timing_state g_timing;
static mach_timebase_info_data_t g_timebase;
static int g_timing_verbose = 0;
/* Yovbk (PR review): guards every g_timing field (flush path and drawRect). */
static pthread_mutex_t g_timing_mtx = PTHREAD_MUTEX_INITIALIZER;
static double g_warmup_secs = SHIM_TIMING_WARMUP_DEFAULT_SECS;

static double shim_ticks_to_ms(uint64_t ticks)
{
    if (g_timebase.denom == 0) return 0.0;
    uint64_t ns = (ticks * (uint64_t)g_timebase.numer) / (uint64_t)g_timebase.denom;
    return (double)ns / 1000000.0;
}

static void shim_timing_init(void)
{
    mach_timebase_info(&g_timebase);
    memset(&g_timing, 0, sizeof(g_timing));
    g_timing_verbose = (getenv("SHIM_TIMING") != NULL);
    const char *ws = getenv("SHIM_TIMING_WARMUP_SECS");
    if (ws) g_warmup_secs = atof(ws);
}

static void shim_stats_update(struct shim_timing_stats *s, uint64_t delta)
{
    if (s->count == 0 || delta < s->min_ticks) s->min_ticks = delta;
    if (delta > s->max_ticks) s->max_ticks = delta;
    s->sum_ticks += delta;
    s->count++;
}

static void shim_stats_reset(struct shim_timing_stats *s)
{
    s->min_ticks = s->max_ticks = s->sum_ticks = 0;
    s->count = 0;
}

static void shim_stats_log(const char *label, const struct shim_timing_stats *s)
{
    if (s->count == 0) {
        fprintf(stderr, "  %-10s: (no samples)\n", label);
        return;
    }
    double mean = shim_ticks_to_ms(s->sum_ticks) / (double)s->count;
    fprintf(stderr, "  %-10s: min=%7.2fms  mean=%7.2fms  max=%7.2fms  (n=%u)\n",
            label, shim_ticks_to_ms(s->min_ticks), mean,
            shim_ticks_to_ms(s->max_ticks), s->count);
}

/* Called at end of shim_flushBuffer — computes splits, logs per-frame (if
 * verbose), folds into rolling stats (always). sc supplies width/height for
 * the transfer-size readout. */
static void shim_timing_record_flush(struct shim_ctx *sc)
{
    g_timing.flush_count++;

    /* Time-based warmup gate — settled once, stays settled. */
    if (g_timing.flush_count == 1)
        g_timing.first_frame_t1 = g_timing.t1;
    if (!g_timing.warmup_settled) {
        double elapsed_s = shim_ticks_to_ms(g_timing.t1 - g_timing.first_frame_t1) / 1000.0;
        if (elapsed_s >= g_warmup_secs) {
            g_timing.warmup_settled = 1;
            fprintf(stderr, "cgl_shim: warmup settled at frame %u (%.1fs elapsed)\n",
                    g_timing.flush_count, elapsed_s);
        }
    }

    uint64_t submit_d   = g_timing.t2 - g_timing.t1;
    uint64_t transfer_d = g_timing.t3 - g_timing.t2;
    uint64_t lock_d     = g_timing.t4 - g_timing.t3;
    uint64_t size_bytes = (uint64_t)sc->width * (uint64_t)sc->height * 4ULL;

    uint64_t wall_d = 0;
    int have_wall = g_timing.has_last_t1;
    if (have_wall) wall_d = g_timing.t1 - g_timing.last_t1;
    g_timing.last_t1 = g_timing.t1;
    g_timing.has_last_t1 = 1;

    if (g_timing_verbose) {
        /* Include t1_abs for first 10 frames so it pairs with KT_T0 from
         * killtest (top of renderFrame). Diff = unmeasured span. */
        static int s_t1_abs_log = 0;
        if (s_t1_abs_log < 10) {
            fprintf(stderr,
                "cgl_shim: frame[%u] wall=%.2f submit=%.2f transfer=%.2f lock=%.2f ms  size=%lluB t1_abs=%llu\n",
                g_timing.flush_count,
                have_wall ? shim_ticks_to_ms(wall_d) : 0.0,
                shim_ticks_to_ms(submit_d),
                shim_ticks_to_ms(transfer_d),
                shim_ticks_to_ms(lock_d),
                (unsigned long long)size_bytes,
                (unsigned long long)g_timing.t1);
            s_t1_abs_log++;
        } else {
            fprintf(stderr,
                "cgl_shim: frame[%u] wall=%.2f submit=%.2f transfer=%.2f lock=%.2f ms  size=%lluB\n",
                g_timing.flush_count,
                have_wall ? shim_ticks_to_ms(wall_d) : 0.0,
                shim_ticks_to_ms(submit_d),
                shim_ticks_to_ms(transfer_d),
                shim_ticks_to_ms(lock_d),
                (unsigned long long)size_bytes);
        }
    }

    if (g_timing.warmup_settled) {
        /* Yovbk (PR review): flushBuffer runs on Gecko's compositor thread
         * AND the GL thread, and shim_timing_record_draw updates
         * g_timing.blit from the main thread, so these updates race: torn
         * min/max, a mean that drifts, and a window_count that can be seen
         * crossing the threshold twice. One mutex for the whole timing
         * struct (g_timing_mtx, also taken in shim_timing_record_draw) —
         * held for a few hundred ns per frame, nothing compared to the
         * readback it is measuring. */
        pthread_mutex_lock(&g_timing_mtx);
        if (have_wall) shim_stats_update(&g_timing.wall,     wall_d);
        shim_stats_update(&g_timing.submit,   submit_d);
        shim_stats_update(&g_timing.transfer, transfer_d);
        shim_stats_update(&g_timing.lock,     lock_d);
        g_timing.window_count++;

        if (g_timing.window_count >= SHIM_TIMING_WINDOW) {
            fprintf(stderr, "cgl_shim: === stats frames %u..%u ===\n",
                    g_timing.flush_count - SHIM_TIMING_WINDOW + 1,
                    g_timing.flush_count);
            shim_stats_log("wall",     &g_timing.wall);
            shim_stats_log("submit",   &g_timing.submit);
            shim_stats_log("transfer", &g_timing.transfer);
            shim_stats_log("lock",     &g_timing.lock);
            shim_stats_log("blit",     &g_timing.blit);
            shim_stats_reset(&g_timing.wall);
            shim_stats_reset(&g_timing.submit);
            shim_stats_reset(&g_timing.transfer);
            shim_stats_reset(&g_timing.lock);
            shim_stats_reset(&g_timing.blit);
            g_timing.window_count = 0;
        }
        pthread_mutex_unlock(&g_timing_mtx);
    }
}

/* Called from shim_drawRect — logs blit cost (if verbose) and folds into
 * rolling stats. drawRect is main-thread only, but the flush threads read and
 * RESET g_timing.blit under g_timing_mtx, so this update takes it too. */
static void shim_timing_record_draw(uint64_t blit_ticks)
{
    if (g_timing_verbose) {
        fprintf(stderr, "cgl_shim: drawRect blit=%.2f ms\n",
                shim_ticks_to_ms(blit_ticks));
    }
    if (g_timing.warmup_settled) {
        pthread_mutex_lock(&g_timing_mtx);
        shim_stats_update(&g_timing.blit, blit_ticks);
        pthread_mutex_unlock(&g_timing_mtx);
    }
}

/* --- Presentation (runs on main thread via dispatch_async) --- */

/* Associated object key for present_buf on the view (for drawRect swizzle) */
static char kScKey;
#define PRESENT_BUF_KEY (&kPresentBufKey)
#define PRESENT_SIZE_KEY (&kPresentSizeKey)
#define SC_KEY (&kScKey)

/* DIFF INSTRUMENT engine (substitute_cgl.c): compares the presented
 * buffer against the union of this frame's draw rects. */
extern "C" void cgl_shim_diff_at_present(const void *buf, int w, int h);

/* t8dmQ (PR review): per-Method originals for the drawRect: swizzle
 * installed in shim_setView. A shared category IMP cannot remember two
 * different originals, so the original of each patched Method is kept
 * here and called back by its own trampoline.
 * t80b1 (PR review): the original must follow the INVOKED Method, not
 * [self class]. The patched Method belongs to the DECLARING class and is
 * shared by every subclass that inherits it, so (a) a receiver whose
 * class differs from the class recorded at install time found no entry
 * and fell through to NSView's drawRect:, skipping the custom
 * implementation, and (b) with two patched overrides in one chain,
 * [super drawRect:] re-entered the shim with the SAME [self class]
 * (objc_msgSendSuper keeps the receiver) and got the same original back
 * — infinite recursion. A plain IMP cannot know which Method it was
 * reached through, so each patched Method gets its own trampoline IMP
 * bound to its table slot. */
static struct { Method m; Class cls; IMP orig; } g_cv_swz[16];
static unsigned g_cv_swz_n = 0;
static pthread_mutex_t g_cv_swz_mtx = PTHREAD_MUTEX_INITIALIZER;

static void shim_cv_draw_body(id self, SEL _cmd, NSRect dirtyRect,
                              unsigned idx);

#define SHIM_CV_TRAMPOLINE(n)                                          \
    static void shim_cv_trampoline_##n(id self, SEL _cmd,              \
                                       NSRect dirtyRect)               \
    {                                                                  \
        shim_cv_draw_body(self, _cmd, dirtyRect, n);                   \
    }
SHIM_CV_TRAMPOLINE(0)
SHIM_CV_TRAMPOLINE(1)
SHIM_CV_TRAMPOLINE(2)
SHIM_CV_TRAMPOLINE(3)
SHIM_CV_TRAMPOLINE(4)
SHIM_CV_TRAMPOLINE(5)
SHIM_CV_TRAMPOLINE(6)
SHIM_CV_TRAMPOLINE(7)
SHIM_CV_TRAMPOLINE(8)
SHIM_CV_TRAMPOLINE(9)
SHIM_CV_TRAMPOLINE(10)
SHIM_CV_TRAMPOLINE(11)
SHIM_CV_TRAMPOLINE(12)
SHIM_CV_TRAMPOLINE(13)
SHIM_CV_TRAMPOLINE(14)
SHIM_CV_TRAMPOLINE(15)
#undef SHIM_CV_TRAMPOLINE

/* shim_setView installs g_cv_trampoline[i] on the Method recorded at
 * table slot i; the trampoline calls that slot's orig. */
static void (*const g_cv_trampoline[16])(id, SEL, NSRect) = {
    shim_cv_trampoline_0, shim_cv_trampoline_1, shim_cv_trampoline_2,
    shim_cv_trampoline_3, shim_cv_trampoline_4, shim_cv_trampoline_5,
    shim_cv_trampoline_6, shim_cv_trampoline_7, shim_cv_trampoline_8,
    shim_cv_trampoline_9, shim_cv_trampoline_10, shim_cv_trampoline_11,
    shim_cv_trampoline_12, shim_cv_trampoline_13, shim_cv_trampoline_14,
    shim_cv_trampoline_15,
};

static void shim_present(NSOpenGLContext *nsc, uint32_t genAtEnqueue)
{
    struct shim_ctx *sc = get_shim_ctx(nsc);
    if (!sc) return;

    /* Clear the coalescing flag FIRST, on every path (review
     * PRRT_kwDOPZhcm86YKBu-): shim_flushBuffer only enqueues a present
     * while !present_pending, so a present that returns before the old
     * "present_pending = 0" line (line ~490, after all the guards below)
     * left the flag stuck at 1 and presentation for this context froze
     * forever — no further present was ever enqueued. Every early return
     * below is a deliberate frame drop; the next -flushBuffer must still
     * be able to enqueue. */
    pthread_mutex_lock(&sc->lock);
    sc->present_pending = 0;
    pthread_mutex_unlock(&sc->lock);

    if (!sc->present_buf || !sc->view) return;

    /* THE ABORT FIX (2026-10-06): if clearDrawable (or teardown)
     * bumped the generation between enqueue and run, this sc's
     * state is stale — the view may be gone (the ___forwarding___
     * abort at the first objc_setAssociatedObject, three identical
     * reports). Drop the frame; the next flush re-presents on the
     * new generation. */
    pthread_mutex_lock(&sc->lock);
    uint32_t genNow = sc->gen;
    pthread_mutex_unlock(&sc->lock);
    if (genNow != genAtEnqueue) return;

    /* VIEW-LIVENESS ROUND-TRIP (2026-10-07, the abort fix): the
     * view's SC_KEY association dies WITH the view — if the view
     * is dead, the read returns nil and present drops the frame.
     * objc_getAssociatedObject does not message the receiver. */
    {
        NSValue *scv = (NSValue *)
            objc_getAssociatedObject((id)sc->view, SC_KEY);
        if (!scv || [scv pointerValue] != (void *)sc) return;
    }

    NSView *view = (NSView *)sc->view;

    /* present_pending is already cleared above (review PRRT_kwDOPZhcm86YKBu-). */
    pthread_mutex_lock(&sc->lock);

    /* DIFF INSTRUMENT (2026-08-17, pre-registered entry-18 follow-up):
     * compare the presented buffer against the union of THIS frame's
     * draw rects (accumulated by the substitute's walk wrappers).
     * Outside-coverage non-black = stale ghost content; outside-
     * coverage black = the never-drawn band. Engine in
     * substitute_cgl.c; windowed by SHIM_DIFF_LOG + /tmp/walk_on. */
    cgl_shim_diff_at_present(sc->present_buf, sc->width, sc->height);

    NSData *bufData = [NSData dataWithBytesNoCopy:sc->present_buf
                                           length:(NSUInteger)(sc->width * sc->height * 4)
                                     freeWhenDone:NO];
    NSDictionary *sizeDict = [NSDictionary dictionaryWithObjectsAndKeys:
                              [NSNumber numberWithInt:sc->width], @"w",
                              [NSNumber numberWithInt:sc->height], @"h", nil];
    objc_setAssociatedObject(view, PRESENT_BUF_KEY, bufData, OBJC_ASSOCIATION_RETAIN);
    objc_setAssociatedObject(view, PRESENT_SIZE_KEY, sizeDict, OBJC_ASSOCIATION_RETAIN);

    pthread_mutex_unlock(&sc->lock);

    /* RUNG 70d — THE TIMER FIX: a repeating 30 Hz timer that forces
     * display on the view, independent of the flush path and the
     * AppKit display cycle. The cycle stops re-firing on 10.6 for
     * SSH-launched windows; a timer on the main runloop keeps
     * drawing alive. The timer's block captures the view weakly
     * (no retain cycle). */
    static NSTimer *s_timer = nil;
    static NSView *s_timer_view = nil;   /* RETAINED (2026-10-06):
                                          * the unsafe_unretained era
                                          * ended when the crash logs
                                          * showed the 30 Hz timer
                                          * messaging a dead view —
                                          * ___forwarding___ → abort,
                                          * three identical reports.
                                          * 10.6 has no zeroing weak;
                                          * retain/release is the
                                          * correct lifetime. */
    if (s_timer_view != view) {
        [view retain];
        [s_timer_view release];
        s_timer_view = view;
    }
    if (!s_timer) {
        /* 2026-10-07: the BLOCK timer API is 10.7+ — on 10.6 it
         * forwarded to the abort (the console crash log). The
         * selector form with the retained view as target. */
        s_timer = [NSTimer timerWithTimeInterval:1.0/30.0
                                         target:(id)s_timer_view
                                       selector:@selector(shim_tick:)
                                       userInfo:nil
                                        repeats:YES];
        [[NSRunLoop mainRunLoop] addTimer:s_timer
                             forMode:NSDefaultRunLoopMode];
    }
    /* also do the direct call for the current frame */
    if ([NSThread isMainThread]) {
        [view setNeedsDisplay:YES];
        [view display];
    } else {
        [view performSelectorOnMainThread:@selector(shim_setNeedsDisplay)
                               withObject:nil
                            waitUntilDone:NO];
        [view performSelectorOnMainThread:@selector(display)
                               withObject:nil
                            waitUntilDone:NO];
    }
}

/* Direct-CGL present entry (LEDGER entry 14): CGLFlushDrawable on a
 * shim context, from substitute_cgl.c. Mirrors shim_flushBuffer's tail
 * — readback into render_buf (virgl path), swap, rebind, store on the
 * view, trigger drawRect — so the existing present/blit/dump machinery
 * all lights up. Runs on whatever thread the app flushed from. */
extern "C" void cgl_shim_present_from_cgl(void *scv)
{
    struct shim_ctx *sc = (struct shim_ctx *)scv;
    if (!sc || !sc->view) return;

    if (pthread_mutex_trylock(&sc->lock) != 0)
        return;   /* never block a foreign flush on our lock */

    if (sc->osmesa_ctx && sc->render_buf) {
        /* Render with the (pre-swap) render buffer current, so the
         * readback captures this frame's output. */
        shim_osmesa_make_current(sc);
        substitute_cgl_tls_set((CGLContextObj)sc);
    }
    if (p_glFinish) p_glFinish();
    if (sc->render_buf)
        shim_readback(sc->width, sc->height, sc->render_buf);

    void *tmp = sc->render_buf;
    sc->render_buf = sc->present_buf;
    sc->present_buf = tmp;
    if (sc->osmesa_ctx && sc->render_buf)
        shim_osmesa_make_current(sc);

    NSView *view = (NSView *)sc->view;
    NSData *bufData = [NSData dataWithBytesNoCopy:sc->present_buf
                                           length:(NSUInteger)(sc->width * sc->height * 4)
                                     freeWhenDone:NO];
    NSDictionary *sizeDict = [NSDictionary dictionaryWithObjectsAndKeys:
                              [NSNumber numberWithInt:sc->width], @"w",
                              [NSNumber numberWithInt:sc->height], @"h", nil];
    objc_setAssociatedObject(view, PRESENT_BUF_KEY, bufData, OBJC_ASSOCIATION_RETAIN);
    objc_setAssociatedObject(view, PRESENT_SIZE_KEY, sizeDict, OBJC_ASSOCIATION_RETAIN);
    pthread_mutex_unlock(&sc->lock);

    /* RUNG 70b — same re-fire fix (direct call when on main thread) */
    if ([NSThread isMainThread]) {
        [view setNeedsDisplay:YES];
        [view display];
    } else {
        [view performSelectorOnMainThread:@selector(shim_setNeedsDisplay)
                               withObject:nil
                            waitUntilDone:NO];
        [view performSelectorOnMainThread:@selector(display)
                               withObject:nil
                            waitUntilDone:NO];
    }
}

/* ===================================================================
 * Swizzled NSOpenGLContext methods.
 *
 * Each shim_ method calls the original via [self shim_xxx] (because
 * method_exchangeImplementations swaps the IMPs — calling shim_xxx
 * now invokes the original).
 * =================================================================== */

/* Forward declaration: defined in the +load section below; the lazy
 * ChildView swizzle in shim_setView: runs before that point in the
 * file. */
static void swizzle_method(Class cls, SEL originalSel, SEL shimSel);

@interface NSView (CGLShimTick)
- (void)shim_tick:(NSTimer *)t;
- (void)shim_setNeedsDisplay;
@end
@implementation NSView (CGLShimTick)
- (void)shim_tick:(NSTimer *)t { (void)t; [self setNeedsDisplay:YES]; [self display]; }
/* setNeedsDisplay: takes a BOOL; performSelectorOnMainThread passes its
 * object argument in the register a BOOL parameter reads (review
 * PRRT_kwDOPZhcm86YRSF-). On 10.6 BOOL is signed char and NSNumber has no
 * tagged pointers, so the object is a 16-byte-aligned heap pointer whose
 * low byte is 0x00 one allocation in sixteen — that decodes as NO and the
 * view is never invalidated. Route the YES through a no-argument method
 * instead (same trick as shim_tick:). */
- (void)shim_setNeedsDisplay { [self setNeedsDisplay:YES]; }
@end

@interface NSOpenGLContext (CGLShim)
- (id)shim_initWithFormat:(NSOpenGLPixelFormat *)fmt shareContext:(NSOpenGLContext *)share;
+ (NSOpenGLContext *)shim_currentContext;
+ (void)shim_clearCurrentContext;
- (void)shim_makeCurrentContext;
- (void)shim_clearDrawable;
- (void)shim_flushBuffer;
- (void)shim_setView:(NSView *)view;
- (void)shim_update;
- (void)shim_setValues:(const GLint *)vals forParameter:(NSOpenGLContextParameter)param;
- (CGLContextObj)shim_CGLContextObj;
@end

/* ===================================================================
 * NSOpenGLPixelFormat strip swizzle — THE COMPOSITOR GATE
 * (2026-08-17, mechanism measured by probe/probe_pix.c):
 *
 * gfxPlatformMac::RequiresAcceleratedGLContextForCompositorOGL() is
 * unconditionally true, so CompositorOGL demands
 * {NSOpenGLPFAAccelerated, AllowOfflineRenderers, DoubleBuffer} via
 * NSOpenGLPixelFormat initWithAttributes. That matching consults the
 * REAL renderer list, which in this VM contains only the Apple
 * software renderer (CGLQueryRendererInfo: nrend=1 id=0x01020400
 * accelerated=0 — boot-constant, clean or not). The format therefore
 * returns nil, Gecko reports FEATURE_FAILURE_OPENGL_CREATE_CONTEXT
 * with zero CGL traffic, falls to BasicCompositor, and the view-path
 * then presents its empty opaque-black buffer over Gecko's software
 * paint — the white-then-black symptom.
 *
 * The substitute's GL is virgl-on-host-GPU; "acceleration" is not a
 * property AppKit can measure here. Stripping the Accelerated
 * attribute lets the format match (software renderer satisfies the
 * remaining attributes), after which the ALREADY-SWIZZLED
 * NSOpenGLContext initWithFormat takes over and routes everything
 * through OSMesa. Same fix class as drawRect-as-flush: satisfy the
 * consumer's contract in emulated semantics.
 *
 * Negative control built in: SHIM_KEEP_ACCEL_ATTR=1 disables the
 * strip — the launch must then fail as before. Log first strips so
 * firing is observable (installation ≠ firing).
 * =================================================================== */
@interface NSOpenGLPixelFormat (CGLShim)
- (id)shim_initWithAttributes:(const NSOpenGLPixelFormatAttribute *)attribs;
@end

@implementation NSOpenGLPixelFormat (CGLShim)
- (id)shim_initWithAttributes:(const NSOpenGLPixelFormatAttribute *)attribs
{
    static int s_keep = -1;
    static int s_strip_log = 0;
    if (s_keep < 0)
        s_keep = (getenv("SHIM_KEEP_ACCEL_ATTR") != NULL);

    if (attribs && !s_keep) {
        /* Boolean attributes (no value follows): Accelerated=73,
         * AllowOfflineRenderers=96, DoubleBuffer=5 is also boolean.
         * Count, then rebuild without 73 if present. */
        NSUInteger n = 0, has_accel = 0;
        while (attribs[n] != 0) {
            if (attribs[n] == NSOpenGLPFAAccelerated) has_accel = 1;
            n++;
        }
        if (has_accel) {
            NSOpenGLPixelFormatAttribute stripped[32];
            NSUInteger i, j = 0;
            if (n < 32) {
                for (i = 0; i < n; i++)
                    if (attribs[i] != NSOpenGLPFAAccelerated)
                        stripped[j++] = attribs[i];
                stripped[j] = 0;
                if (s_strip_log < 10) {
                    s_strip_log++;
                    fprintf(stderr, "cgl_shim: PIXFMT_STRIP[%d] dropped "
                            "NSOpenGLPFAAccelerated from %lu-attr list — "
                            "real renderer list is software-only "
                            "(SHIM_KEEP_ACCEL_ATTR=1 undoes)\n",
                            s_strip_log, (unsigned long)n);
                }
                return [self shim_initWithAttributes:stripped];
            } else if (s_strip_log < 10) {
                s_strip_log++;
                fprintf(stderr, "cgl_shim: PIXFMT_STRIP attr list %lu > 32 "
                        "— passing through unstripped\n", (unsigned long)n);
            }
        }
    }
    return [self shim_initWithAttributes:attribs];
}
@end

@implementation NSOpenGLContext (CGLShim)

- (id)shim_initWithFormat:(NSOpenGLPixelFormat *)fmt shareContext:(NSOpenGLContext *)share
{
    /* Call original to get a valid NSOpenGLContext object with AppKit
     * ivars initialized. We need this for AppKit state tracking; we'll
     * route GL through OSMesa. The CGL context is created but we never
     * call through to original -setView:, so no CGS GL surface should
     * be bound. */
    self = [self shim_initWithFormat:fmt shareContext:share];
    if (!self) return nil;

    if (!getenv("GALLIUM_DRIVER"))
        setenv("GALLIUM_DRIVER", "virgl", 0);

    struct shim_ctx *sc = (struct shim_ctx *)calloc(1, sizeof(struct shim_ctx));
    if (!sc) return self;

    sc->magic = SHIM_MAGIC;
    /* Probe: confirm GALLIUM_DRIVER at the moment OSMesaCreateContextExt
     * runs. Mesa's call_once fires here, reading GALLIUM_DRIVER to pick
     * the driver. If this prints "(unset)", the call_once fired earlier. */
    fprintf(stderr, "cgl_shim: pre-OSMesaCreateContextExt GALLIUM_DRIVER=\"%s\" thread=%p\n",
            getenv("GALLIUM_DRIVER") ?: "(unset)", (void*)pthread_self());
    /* SHARELIST FIX (2026-08-19, operator-named cause): Gecko's chrome
     * compositing passes a real shareContext and expects object sharing
     * — textures/FBOs created in one context must exist in the other.
     * We received the argument and DROPPED it (NULL here and in
     * substitute_cgl.c's CGLCreateContext): every OSMesa context lived
     * in its own share group, so a texture created in the content
     * context was non-existent in the compositor's — the exact
     * glFramebufferTexture2D(non-existent texture 2) → incomplete
     * framebuffer → empty readback → transparent frame → blit-skip →
     * WHITE chain. The aquarium worked because a WebGL canvas is one
     * self-contained context; chrome compositing is where Gecko
     * shares. Mesa plumbing verified (osmesa.c:575-681 passes share →
     * st_shared; virgl handles are global). */
    struct shim_ctx *share_sc = share ? get_shim_ctx(share) : NULL;
    /* RUNG 70c — THE PROFILE FIX: explicitly request COMPAT profile
     * via OSMesaCreateContextAttribs. The legacy Ext call may land in
     * core profile on some Mesa builds (virgl prefers core); core
     * rejects client-side vertex arrays — the named GL_INVALID_
     * OPERATION from the suite ("glVertexAttribPointer(non-VBO
     * array)"). Compat matches what 10.6's GL 2.1 apps expect. */
    {
        int attribs[] = {
            OSMESA_FORMAT, OSMESA_RGBA,
            OSMESA_DEPTH_BITS, 16,
            OSMESA_PROFILE, OSMESA_COMPAT_PROFILE,
            0
        };
        sc->osmesa_ctx = OSMesaCreateContextAttribs(
            attribs, share_sc ? share_sc->osmesa_ctx : NULL);
        if (!sc->osmesa_ctx)
            sc->osmesa_ctx = OSMesaCreateContextExt(
                OSMESA_RGBA, 16, 0, 0,
                share_sc ? share_sc->osmesa_ctx : NULL);
    }
    fprintf(stderr, "cgl_shim: post-OSMesaCreateContextExt osmesa_ctx=%p "
            "share=%p share_osmesa=%p\n",
            (void*)sc->osmesa_ctx, (void*)share,
            share_sc ? (void*)share_sc->osmesa_ctx : NULL);
    if (!sc->osmesa_ctx) {
        free(sc);
        return self;
    }

    alloc_buffers(sc, 1, 1);
    shim_osmesa_make_current(sc);
    substitute_cgl_tls_set((CGLContextObj)sc);
    /* Dispatch probe right after OSMesaMakeCurrent: did it install a real
     * dispatch table, or is _glapi_tls_Dispatch still the no-op table? */
    {
        const void *disp = _glapi_get_dispatch();
        const void **entries = (const void **)disp;
        fprintf(stderr, "cgl_shim: dispcheck[init] post-MakeCurrent dispatch=%p thread=%p "
                "entries[0..3]=%p,%p,%p,%p\n",
                (void*)disp, (void*)pthread_self(),
                entries ? (void*)entries[0] : (void*)0,
                entries ? (void*)entries[1] : (void*)0,
                entries ? (void*)entries[2] : (void*)0,
                entries ? (void*)entries[3] : (void*)0);
    }

    /* RUNG 70 — THE GL_INVALID_OPERATION HUNT: check the error state
     * right after MakeCurrent (before any app draw). If 0x502 is
     * already set, it's from setup (Mesa init or virgl ctx creation).
     * Then create+bind a VAO — on a core profile, no-VAO draws are
     * the classic GL_INVALID_OPERATION source (clears unaffected). */
    {
        typedef GLenum (*fn_geterr_t)(void);
        typedef void (*fn_gen_t)(int, unsigned int*);
        typedef void (*fn_bind_t)(unsigned int);
        fn_geterr_t pGetErr = (fn_geterr_t)dlsym(g_mesa, "glGetError");
        fn_gen_t pGen = (fn_gen_t)dlsym(g_mesa, "glGenVertexArrays");
        fn_bind_t pBind = (fn_bind_t)dlsym(g_mesa, "glBindVertexArray");
        if (pGetErr && pGen && pBind) {
            GLenum setup_err = pGetErr();
            GLuint vao = 0;
            pGen(1, &vao);
            if (vao) {
                pBind(vao);
                GLenum post_vao_err = pGetErr();
                fprintf(stderr, "cgl_shim: rung70 SETUP err=0x%x "
                        "vao=%u post_vao_err=0x%x\n",
                        setup_err, vao, post_vao_err);
            } else {
                fprintf(stderr, "cgl_shim: rung70 SETUP err=0x%x "
                        "GenVertexArrays FAILED\n", setup_err);
            }
            while (pGetErr() != GL_NO_ERROR) ;
        }
    }
    pthread_mutex_init(&sc->lock, NULL);
    pthread_mutex_init(&sc->cgl_lock, NULL);
    sc->last_view_w = -1;
    sc->last_view_h = -1;

    /* Review PRRT_kwDOPZhcm86YKBvN: a full registry must not leave a
     * half-shimmed context live. shim_register() fails once
     * MAX_SHIM_CONTEXTS contexts are registered (shim_lookup then cannot
     * resolve this sc), and shim_CGLContextObj hands the raw sc pointer
     * out as a CGLContextObj token — every _CGL* interpose would forward
     * that token to real CGL and crash inside Apple's GL. Tear this sc
     * down and leave the context on Apple's renderer instead: -flushBuffer
     * and friends fall back to the original IMPs when get_shim_ctx(self)
     * is NULL (see shim_flushBuffer's `if (!sc || !sc->osmesa_ctx)`). */
    if (shim_register(sc) != 0) {
        fprintf(stderr, "cgl_shim: shim registry full (%d) — context %p left "
                "on Apple GL (not shimmed)\n", MAX_SHIM_CONTEXTS, (void *)self);
        /* t7gy_ (PR review): initWithFormat bound sc into OSMesa and the
         * substitute TLS above (shim_osmesa_make_current(sc) +
         * substitute_cgl_tls_set(sc)) WITHOUT going through
         * -makeCurrentContext, so get_current_ctx() is NOT a witness for the
         * bound state — a TLS check here would skip both unbinds and free
         * sc while OSMesa still has it current on this thread. Clear both
         * unconditionally, then the TLS slot only if it points at self. */
        if (get_current_ctx() == self) {
            set_current_ctx(nil);
        }
        shim_osmesa_make_current(NULL);
        substitute_cgl_tls_set(NULL);
        free_buffers(sc);
        OSMesaDestroyContext(sc->osmesa_ctx);
        pthread_mutex_destroy(&sc->cgl_lock);
        pthread_mutex_destroy(&sc->lock);
        free(sc);
        return self;
    }
    set_shim_ctx(self, sc);

    return self;
}

+ (NSOpenGLContext *)shim_currentContext
{
    NSOpenGLContext *ctx = get_current_ctx();
    if (ctx) {
        /* Gecko's IsCurrent() compares this against its own mContext pointer.
         * Must return the actual NSOpenGLContext*, not a token. */
        return ctx;
    }
    /* YKBwO (PR review): a native (non-shim) context can be current —
     * -makeCurrentContext's fallback path calls the ORIGINAL IMP for
     * contexts the shim did not take over (registry full, no OSMesa
     * context), and that path never touches our TLS. With the TLS empty,
     * returning nil here hid a genuinely current native context, which is
     * exactly what the "fall back to the original IMP" design promised
     * would keep working. swizzle_class_method used
     * method_exchangeImplementations, so the original IMP lives under the
     * shim selector — calling it through the class re-enters Apple's
     * +currentContext, not this method. */
    return [NSOpenGLContext shim_currentContext];
}

+ (void)shim_clearCurrentContext
{
    set_current_ctx(nil);
    shim_osmesa_make_current(NULL);
    substitute_cgl_tls_set(NULL);
}

- (void)shim_makeCurrentContext
{
    struct shim_ctx *sc = get_shim_ctx(self);
    if (sc && sc->osmesa_ctx && sc->render_buf) {
        set_current_ctx(self);
        shim_osmesa_make_current(sc);
        substitute_cgl_tls_set((CGLContextObj)sc);
    } else {
        /* Fall through to original for non-shim contexts. */
        [self shim_makeCurrentContext];
    }
}

- (void)shim_clearDrawable
{
    struct shim_ctx *sc = get_shim_ctx(self);
    if (sc) {
        pthread_mutex_lock(&sc->lock);
        sc->gen++;               /* 2026-10-06: invalidate any
                                  * enqueued present against this sc */
        pthread_mutex_unlock(&sc->lock);
        set_current_ctx(nil);
        shim_osmesa_make_current(NULL);
        substitute_cgl_tls_set(NULL);
    } else {
        [self shim_clearDrawable];
    }
}

- (void)shim_flushBuffer
{
    struct shim_ctx *sc = get_shim_ctx(self);
    if (!sc || !sc->osmesa_ctx) {
        [self shim_flushBuffer];  /* original for non-shim */
        return;
    }

    /* Per-context flush accounting (2026-08-20): the empty-front census
     * shows ONE context's drawable resized to the window while the other
     * stayed 1x1 — the composite renders into one pixel. This answers,
     * per shim_ctx: does it ever enter flushBuffer, with what viewport,
     * at what buffer dims, from which thread. Separates "compositor
     * never flushes" from "flushes with vp=0/1x1" from "lookup fails". */
    {
        static struct { struct shim_ctx *sc; int n; } s_acc[8];
        int slot = -1;
        for (int i = 0; i < 8; i++) {
            if (s_acc[i].sc == sc) { slot = i; break; }
            if (s_acc[i].sc == NULL) {
                slot = i; s_acc[i].sc = sc; s_acc[i].n = 0;
                break;
            }
        }
        if (slot >= 0 && s_acc[slot].n < 12) {
            s_acc[slot].n++;
            GLint vpq[4] = {0, 0, 0, 0};
            if (sc->osmesa_ctx && p_glGetIntegerv) p_glGetIntegerv(GL_VIEWPORT, vpq);
            fprintf(stderr,
                    "cgl_shim: FLUSHACC sc=%p osmesa=%p th=%p #%d dims=%dx%d vp=(%d,%d,%d,%d)\n",
                    (void *)sc, (void *)sc->osmesa_ctx,
                    (void *)pthread_self(), s_acc[slot].n,
                    sc->width, sc->height,
                    vpq[0], vpq[1], vpq[2], vpq[3]);
        }
    }

    /* Probe: viewport BEFORE resize check. If viewport=(0,0,800,600) here,
     * the glViewport from ip_glViewport persisted. If (0,0,0,0), something
     * between ip_glViewport and flushBuffer reset it. */
    {
        static int s_pre_resize_log = 0;
        if (s_pre_resize_log < 3) {
            s_pre_resize_log++;
            GLint vp[4] = {-1, -1, -1, -1};
            if (p_glGetIntegerv) p_glGetIntegerv(GL_VIEWPORT, vp);
            const void *ctx = _glapi_get_context();
            fprintf(stderr, "cgl_shim: pre-resize[%d] viewport=(%d,%d,%d,%d) context=%p\n",
                    s_pre_resize_log, vp[0], vp[1], vp[2], vp[3], (void*)ctx);
        }
    }

    /* --- Resize detection (design problem #2, revised 2026-08-13) ---
     * Gecko doesn't use NSOpenGLView, so -update may never fire. The
     * previous approach queried [view bounds] here — but flushBuffer
     * (and setView:) run on Gecko's COMPOSITOR thread, and AppKit
     * geometry calls off-main deadlock against the main thread's
     * display cycle: main holds the AppKit display path and waits in
     * PCompositorBridgeChild::SendFlushRendering for the compositor,
     * while the compositor blocks in [view bounds] inside this shim.
     * Observed 2026-08-13: both threads parked 30+ minutes (sample of
     * pid 5326 — Compositor thread inside shim_setView:+221, main
     * inside _recursiveDisplayAllDirtyWithLockFocus → SendFlushRendering).
     * Fix: take the drawable size from the renderer's own viewport —
     * a thread-local GL query with no AppKit involvement, correct by
     * construction because glViewport is how the app tells GL the
     * drawable size. If no context is current, vp stays 0 and the
     * check safely skips. */
    GLint vp_rs[4] = {0, 0, 0, 0};
    if (p_glGetIntegerv) p_glGetIntegerv(GL_VIEWPORT, vp_rs);   /* NULL -> vp stays 0: resize check skips, by design */
    if (vp_rs[2] > 0 && vp_rs[3] > 0 &&
        (vp_rs[2] != sc->width || vp_rs[3] != sc->height)) {
        /* trylock: a full lock here once deadlocked the compositor on a
         * mutex no live thread held (2026-08-13, gen-3 sample: first
         * acquisition ever on this sc blocked forever — sc suspected
         * stale/corrupted). Skipping the resize for one frame is always
         * recoverable; blocking the compositor is not. */
        if (pthread_mutex_trylock(&sc->lock) == 0) {
            free_buffers(sc);
            alloc_buffers(sc, vp_rs[2], vp_rs[3]);
            shim_osmesa_make_current(sc);
            substitute_cgl_tls_set((CGLContextObj)sc);
            pthread_mutex_unlock(&sc->lock);
        } else {
            static int s_rs_skip_log = 0;
            if (s_rs_skip_log < 5) {
                s_rs_skip_log++;
                fprintf(stderr, "cgl_shim: resize-skip[%d] lock busy/odd "
                        "sc=%p magic=0x%08x thread=%p\n", s_rs_skip_log,
                        (void *)sc, sc ? sc->magic : 0, (void *)pthread_self());
            }
        }
    }

    /* --- Compositor-thread marshalling (design problem #1) --- */

    /* Diagnostic: is the OSMesa context current? Which renderer? */
    {
        static int s_ctx_log = 0;
        if (s_ctx_log < 5) {
            s_ctx_log++;
            OSMesaContext osmc = OSMesaGetCurrentContext();
            const GLubyte *renderer = p_glGetString ? p_glGetString(GL_RENDERER) : NULL;
            fprintf(stderr, "cgl_shim: ctxcheck[%d] OSMesaCurrent=%p renderer=\"%s\"\n",
                    s_ctx_log, (void*)osmc,
                    renderer ? (const char *)renderer : "(null)");
        }
    }

    /* TLS-dispatch probe (shim path). _glapi_get_dispatch() returns the
     * current thread's _glapi_tls_Dispatch value. Compare against the
     * pointer logged from diag_glViewport in cgl_interpose.c — same non-NULL
     * value means dispatch is fine; mismatched/NULL in interpose confirms
     * the emulated-TLS hypothesis. Also dumps first 4 dispatch entries:
     * all identical → no-op dispatch table; all different → real dispatch. */
    {
        static int s_disp_log = 0;
        if (s_disp_log < 5) {
            s_disp_log++;
            const void *disp = _glapi_get_dispatch();
            const void *ctx = _glapi_get_context();
            int tbl_size = _glapi_get_dispatch_table_size();
            const void **entries = (const void **)disp;
            /* CAP_Viewport is slot 326 in the glapitemp.h enum (varies by Mesa
             * version — dump a range to find it). */
            fprintf(stderr, "cgl_shim: dispcheck[shim,%d] dispatch=%p context=%p thread=%p size=%d\n",
                    s_disp_log, (void*)disp, (void*)ctx, (void*)pthread_self(), tbl_size);
            if (entries) {
                /* Dump entries 0, 1, 2, 3 and a mid-range sample + last */
                fprintf(stderr, "cgl_shim:             entries[0..3]=%p,%p,%p,%p\n",
                        entries[0] ? (void*)entries[0] : (void*)0,
                        entries[1] ? (void*)entries[1] : (void*)0,
                        entries[2] ? (void*)entries[2] : (void*)0,
                        entries[3] ? (void*)entries[3] : (void*)0);
                fprintf(stderr, "cgl_shim:             entries[100..103]=%p,%p,%p,%p\n",
                        entries[100] ? (void*)entries[100] : (void*)0,
                        entries[101] ? (void*)entries[101] : (void*)0,
                        entries[102] ? (void*)entries[102] : (void*)0,
                        entries[103] ? (void*)entries[103] : (void*)0);
                fprintf(stderr, "cgl_shim:             entries[326..329]=%p,%p,%p,%p\n",
                        entries[326] ? (void*)entries[326] : (void*)0,
                        entries[327] ? (void*)entries[327] : (void*)0,
                        entries[328] ? (void*)entries[328] : (void*)0,
                        entries[329] ? (void*)entries[329] : (void*)0);
                /* entries[199] = glClear slot per glClear disassembly (jmpq *0x658(%rax) = 1624/8 = 203)
                 * But that's the offset, not index. 1624/8 = 203. So entries[203] = glClear impl. */
                fprintf(stderr, "cgl_shim:             entries[199..205]=%p,%p,%p,%p,%p,%p,%p\n",
                        entries[199] ? (void*)entries[199] : (void*)0,
                        entries[200] ? (void*)entries[200] : (void*)0,
                        entries[201] ? (void*)entries[201] : (void*)0,
                        entries[202] ? (void*)entries[202] : (void*)0,
                        entries[203] ? (void*)entries[203] : (void*)0,
                        entries[204] ? (void*)entries[204] : (void*)0,
                        entries[205] ? (void*)entries[205] : (void*)0);
                if (tbl_size > 4) {
                    fprintf(stderr, "cgl_shim:             entries[size-4..size-1]=%p,%p,%p,%p\n",
                            entries[tbl_size-4] ? (void*)entries[tbl_size-4] : (void*)0,
                            entries[tbl_size-3] ? (void*)entries[tbl_size-3] : (void*)0,
                            entries[tbl_size-2] ? (void*)entries[tbl_size-2] : (void*)0,
                            entries[tbl_size-1] ? (void*)entries[tbl_size-1] : (void*)0);
                }
            }
        }
    }

    g_timing.t1 = mach_absolute_time();
    if (p_glFinish) p_glFinish();  /* flush Mesa's command queue to the host */
    g_timing.t2 = mach_absolute_time();

    /* CORNER-STAMP instrument (LEDGER entry 10, pre-registered):
     * stamp varying magic values into the 4 corner blocks of the GUEST
     * render_buf immediately before glReadPixels, read them back
     * immediately after. SURVIVES ⇒ the readback is dirty-region /
     * guest-passthrough (undamaged regions are NOT overwritten with
     * host content — and the SHIM_CLEAR_FRAME magenta experiment
     * measured the readback, not Gecko; persistence conclusion needs
     * redo). OVERWRITTEN ⇒ full-frame host transfer per readback
     * (and the magenta WAS Gecko's behavior). */
    static long s_stamp_frame = 0;
    static int s_stamp_log = 0;
    uint32_t stamp_val = 0;
    uint32_t post[4] = {0, 0, 0, 0};
    int stamp_ok = (!host_present_mode() && sc->render_buf &&
                    sc->width >= 16 && sc->height >= 16);
    if (stamp_ok) {
        s_stamp_frame++;
        stamp_val = 0xCAFE0000u | ((uint32_t)s_stamp_frame & 0xFFFFu);
        uint32_t *b = (uint32_t *)sc->render_buf;
        int W = sc->width, H = sc->height;
        for (int dy = 0; dy < 8; dy++) {
            for (int dx = 0; dx < 8; dx++) {
                b[(size_t)dy * W + dx] = stamp_val;                 /* TL */
                b[(size_t)dy * W + (W - 8 + dx)] = stamp_val;       /* TR */
                b[(size_t)(H - 8 + dy) * W + dx] = stamp_val;       /* BL */
                b[(size_t)(H - 8 + dy) * W + (W - 8 + dx)] = stamp_val; /* BR */
            }
        }
    }

    /* Force readback from host GPU to guest buffer. virgl renders on the
     * host; the guest-side OSMesa buffer only gets the pixels when a
     * TRANSFER_FROM_HOST_3D fires. softpipe renders directly into the
     * guest buffer and doesn't need this. Without it, present_buf stays
     * zero-filled and the window shows white.
     * HOST-PRESENT MODE: the front buffer goes to the display via the
     * GPU-side blit (winsys flush_frontbuffer → kext 0x600C) — skip the
     * readback entirely (the measured 61-76 ms per-flush transfer half
     * IS this copy). The STAMP probe measures readback survival and is
     * meaningless without it; skipped with it. */
    if (!host_present_mode())
    shim_readback(sc->width, sc->height, sc->render_buf);

    if (stamp_ok) {
        uint32_t *b = (uint32_t *)sc->render_buf;
        int W = sc->width, H = sc->height;
        post[0] = b[0];
        post[1] = b[W - 1];
        post[2] = b[(size_t)(H - 1) * W];
        post[3] = b[(size_t)(H - 1) * W + (W - 1)];
        int surv = 0;
        for (int i = 0; i < 4; i++) if (post[i] == stamp_val) surv++;
        if (s_stamp_log < 30) {
            s_stamp_log++;
            fprintf(stderr, "cgl_shim: STAMP[%ld] val=0x%08x post="
                    "[0x%08x 0x%08x 0x%08x 0x%08x] survived=%d/4\n",
                    s_stamp_frame, stamp_val,
                    post[0], post[1], post[2], post[3], surv);
        }
    }
    g_timing.t3 = mach_absolute_time();

    /* Diagnostic: scratch readback with sentinels + error check.
     * Sentinels distinguish "wrote zeros" from "didn't write". */
    {
        static int s_diag_log = 0;
        /* Yovbg (PR review): gated on g_timing_verbose (the same env as the
         * timing prints) AND no p_glGetError() any more — glGetError CLEARS
         * the pending error, so a diagnostic probe on the application's
         * context was eating its error state. */
        if (g_timing_verbose && s_diag_log < 5) {
            s_diag_log++;
            uint32_t scratch[4] = {0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF};
            shim_readback(2, 2, scratch);
            GLint vp[4] = {0};
            if (p_glGetIntegerv) p_glGetIntegerv(GL_VIEWPORT, vp);
            fprintf(stderr, "cgl_shim: diag[%d] scratch=(0x%08x,0x%08x,0x%08x,0x%08x) "
                    "viewport=(%d,%d,%d,%d)\n",
                    s_diag_log, scratch[0], scratch[1], scratch[2], scratch[3],
                    vp[0], vp[1], vp[2], vp[3]);
        }
    }

    /* Diagnostic: buffer pointer + first pixel before swap */
    {
        static int s_pre_swap_log = 0;
        if (s_pre_swap_log < 5) {
            s_pre_swap_log++;
            uint32_t *rb = (uint32_t *)sc->render_buf;
            uint32_t *pb = (uint32_t *)sc->present_buf;
            fprintf(stderr, "cgl_shim: preswap[%d] render=%p[0]=0x%08x present=%p[0]=0x%08x\n",
                    s_pre_swap_log, (void*)sc->render_buf, rb ? rb[0] : 0xDEAD,
                    (void*)sc->present_buf, pb ? pb[0] : 0xDEAD);
        }
    }

    /* trylock for the same reason as the resize section: never block the
     * compositor on this mutex. Busy → drop this frame; the coalescing
     * flag below ensures a present is still pending or will re-fire on
     * the next flush. */
    if (pthread_mutex_trylock(&sc->lock) != 0) {
        static int s_swap_skip_log = 0;
        if (s_swap_skip_log < 5) {
            s_swap_skip_log++;
            fprintf(stderr, "cgl_shim: swap-skip[%d] lock busy/odd sc=%p "
                    "magic=0x%08x thread=%p\n", s_swap_skip_log,
                    (void *)sc, sc ? sc->magic : 0, (void *)pthread_self());
        }
        return;
    }
    /* Swap render and present buffers */
    void *tmp = sc->render_buf;
    sc->render_buf = sc->present_buf;
    sc->present_buf = tmp;
    /* SHIM_CLEAR_FRAME — the persistence-vs-two-draws discriminator
     * (LEDGER entry 8, pre-registered): nothing has established that
     * render_buf is ever cleared between frames. If it isn't, "two
     * bands in one dumped buffer" is equally consistent with ONE draw
     * per click plus pixel persistence (band N left from frame N−1)
     * as with two draws in one frame. Real double-buffered GL
     * DISCARDS the back buffer on swap — Gecko may lawfully rely on
     * swap-to-clear; our persistent render_buf never clears. Clear
     * the fresh render_buf (old present) to magenta AFTER the swap —
     * present_buf still holds frame N for drawRect.
     *   bands VANISH → persistence + walking destination; the fix is
     *     swap-to-clear semantics in the shim (maybe THE artifact).
     *   bands REMAIN → both draws genuinely occur per frame. */
    {
        static int s_clear_frame = -1;
        if (s_clear_frame < 0) {
            s_clear_frame = (getenv("SHIM_CLEAR_FRAME") != NULL);
            if (s_clear_frame)
                fprintf(stderr, "cgl_shim: CLEAR_FRAME — render_buf "
                        "magenta-cleared after each swap\n");
        }
        if (s_clear_frame && sc->render_buf && sc->width > 0 &&
            sc->height > 0) {
            uint32_t *p = (uint32_t *)sc->render_buf;
            size_t n = (size_t)sc->width * sc->height;
            for (size_t i = 0; i < n; i++)
                p[i] = 0xFFFF00FFu;   /* RGBA bytes: magenta, opaque */
        }
    }
    /* FMTX probe at COMPOSITE time (LEDGER entry 7): URL-bar clicks
     * fire NO new uploads, so the upload-site MTX probe never samples
     * artifact frames. Sample HERE — every flushBuffer, the state Gecko
     * leaves at frame end. Discriminator (pre-registered): a translation
     * component growing by ~h per click = accumulating GL state (shim's
     * to save/restore); identity every frame = accumulation is inside
     * Gecko's own draw loop, shim exonerated. */
    if (p_glGetFloatv && p_glGetIntegerv) {
        /* Log on CHANGE only (cap redesign after 80 identity startup
         * frames exhausted the first cap): identity-forever prints one
         * line; a walking translation prints each step. */
        static GLfloat s_last[8] = {-1,-1,-1,-1,-1,-1,-1,-1};
        static long s_fmtx_count = 0, s_fmtx_changes = 0;
        s_fmtx_count++;
        GLfloat mv[16], pr[16];
        GLint fmode = 0;
        p_glGetFloatv(GL_MODELVIEW_MATRIX, mv);
        p_glGetFloatv(GL_PROJECTION_MATRIX, pr);
        p_glGetIntegerv(GL_MATRIX_MODE, &fmode);
        GLfloat cur[8] = {mv[12], mv[13], mv[14], mv[15],
                          pr[12], pr[13], pr[14], pr[15]};
        int changed = (s_last[0] < 0);
        for (int i = 0; i < 8; i++)
            if (cur[i] != s_last[i]) changed = 1;
        if (changed) {
            s_fmtx_changes++;
            for (int i = 0; i < 8; i++) s_last[i] = cur[i];
            fprintf(stderr, "cgl_shim: FMTX[change#%ld frame#%ld] "
                    "mode=%u mv_t=[%.1f %.1f %.1f %.1f] "
                    "pr_t=[%.1f %.1f %.1f %.1f]\n",
                    s_fmtx_changes, s_fmtx_count, (unsigned)fmode,
                    cur[0], cur[1], cur[2], cur[3],
                    cur[4], cur[5], cur[6], cur[7]);
        }
    }

    /* Re-bind context to new render buffer.
     * THE correctness-critical step: without this, subsequent rendering
     * writes into the buffer the main thread is reading. */
    {
        shim_osmesa_make_current(sc);
        substitute_cgl_tls_set((CGLContextObj)sc);
        static int s_swap_mc_log = 0;
        if (s_swap_mc_log < 5) {
            s_swap_mc_log++;
            fprintf(stderr, "cgl_shim: swap_mc[%d] buf=%p %dx%d\n",
                    s_swap_mc_log, (void*)sc->render_buf,
                    sc->width, sc->height);
        }
    }

    /* Coalesce: if a present is already queued, don't enqueue another.
     * The pending present will pick up the latest buffer. Without this,
     * dispatch_async blocks accumulate without bound under TCG. */
    int should_enqueue = !sc->present_pending;
    sc->present_pending = 1;
    pthread_mutex_unlock(&sc->lock);

    /* Trigger presentation on main thread */
    if (should_enqueue) {
        NSOpenGLContext *ctxForPresent = self;
        uint32_t genAtEnqueue = sc->gen;   /* abort fix: present
                                            * returns if teardown
                                            * bumped it meanwhile */
        dispatch_async(dispatch_get_main_queue(), ^{
            shim_present(ctxForPresent, genAtEnqueue);
        });
    }

    g_timing.t4 = mach_absolute_time();
    shim_timing_record_flush(sc);
}

- (void)shim_setView:(NSView *)view
{
    struct shim_ctx *sc = get_shim_ctx(self);
    if (sc) {
        /* Retain new view, release old. 10.6 has no zeroing weak refs.
         * Store through the GC write barrier: this image declares ObjC
         * GC support (__objc_imageinfo), so in a GC process (e.g. System
         * Preferences) retain/release are no-ops and a bare store into
         * this calloc'd (non-collectable) struct is invisible to the
         * collector — the view could be collected while we still hold
         * the pointer. objc_assign_strongCast makes the location a
         * scanned reference; with GC off it is a plain store
         * (objc-auto.h). */
        if (sc->view) [(NSView *)sc->view release];
        if (view) [view retain];
        objc_assign_strongCast((id)view, (id *)&sc->view);

        /* NSOpenGLView-idiom support (LEDGER entry 16): associate the
         * shim_ctx on the view so the drawRect swizzle can present on
         * return from the app's draw — the idiom where AppKit flushes
         * through private __NS_CGL* wrappers our swizzles cannot see.
         * 2026-10-07: set UNCONDITIONALLY — the view-liveness round-
         * trip in shim_present reads this association as its liveness
         * anchor; under the old env gate every present dropped (the
         * white-compositor report — app rendered offscreen at full
         * FPS while the screen never updated). */
        {
            objc_setAssociatedObject(view, SC_KEY,
                [NSValue valueWithPointer:(void *)sc],
                OBJC_ASSOCIATION_RETAIN);
            fprintf(stderr, "cgl_shim: DRAWRECT_PRESENT armed for view "
                    "class %s sc=%p\n", class_getName([view class]),
                    (void *)sc);
        }

        /* Lazily swizzle the ACTUAL Gecko view class's drawRect:.
         * The NSView-level swizzle never runs under Gecko: ChildView
         * overrides drawRect: and per-class dispatch lets the override
         * win (observed: drawptr 0, blit no samples, white content
         * area). The view arriving here IS the Gecko view instance,
         * so [view class] is the exact class to patch. Only classes
         * that genuinely override drawRect: (IMP differs from
         * NSView's) are patched, so a plain NSView from another app
         * doesn't consume the slot. One-time PER METHOD; the runtime
         * locks method exchange internally.
         *
         * ZZBKD (PR review): this used to be a process-wide one-shot
         * (s_childview_done). The first custom view a shim context saw
         * consumed it, and a second custom view class — e.g. a splash
         * view set before Gecko's ChildView — then never received the
         * presentation wrapper, while the first (wrong) class kept it.
         *
         * t8Nom (PR review): the table tracks the METHOD that was
         * exchanged, not the class. class_getInstanceMethod() returns a
         * SUPERCLASS Method when the subclass does not override
         * drawRect: itself, so for such a subclass the "already done"
         * check must not key on the class — exchanging the inherited
         * Method a second time would swap the IMPs back and silently
         * unswizzle the parent. Membership by Method pointer catches
         * both cases (same class seen again, or a subclass that
         * inherits the same Method). */
        /* t8dmQ (PR review): the original drawRect: IMP is per METHOD.
         * method_exchangeImplementations against the shared category
         * Method cannot hold two originals: a second exchange would
         * leave the first class's "original" pointing at the second
         * class's drawRect:. Install a slot-bound trampoline IMP with
         * method_setImplementation and keep each Method's original in
         * g_cv_swz (file scope, shared with the trampolines above).
         * t80b1 (PR review): the trampoline is chosen TOGETHER with its
         * table entry, under the lock — a full table must not install
         * anything, or the patched Method would have no original to
         * call. */
        if (view) {
            Class cv = [view class];
            Method m = class_getInstanceMethod(cv, @selector(drawRect:));
            pthread_mutex_lock(&g_cv_swz_mtx);
            int already = 0;
            for (unsigned i = 0; i < g_cv_swz_n; i++)
                if (g_cv_swz[i].m == m) { already = 1; break; }
            if (!already) {
                Method base = class_getInstanceMethod([NSView class],
                                                      @selector(drawRect:));
                if (base && g_cv_swz_n < 16 &&
                    method_getImplementation(m) !=
                        method_getImplementation(base)) {
                    IMP origIMP = method_getImplementation(m);
                    method_setImplementation(
                        m, (IMP)g_cv_trampoline[g_cv_swz_n]);
                    g_cv_swz[g_cv_swz_n].m = m;
                    g_cv_swz[g_cv_swz_n].cls = cv;
                    g_cv_swz[g_cv_swz_n].orig = origIMP;
                    g_cv_swz_n++;
                    fprintf(stderr, "cgl_shim: drawRect swizzled on %s "
                            "(custom view class, via setView; %u method(s) "
                            "patched)\n", class_getName(cv), g_cv_swz_n);
                }
            }
            pthread_mutex_unlock(&g_cv_swz_mtx);
        }

        /* No sizing here. Gecko calls setView: from the COMPOSITOR
         * thread (BasicCompositor::TryToEndRemoteDrawing), so any
         * AppKit call — [view bounds] — can deadlock against the main
         * thread's display cycle (see flushBuffer's resize note for
         * the observed ABBA). Buffer sizing is handled entirely by
         * the viewport check at the top of flushBuffer, which runs on
         * the rendering thread with the context current. retain/
         * release are refcount-atomic and safe off-main. */
    } else {
        [self shim_setView:view];  /* original */
    }
}

- (void)shim_update
{
    struct shim_ctx *sc = get_shim_ctx(self);
    if (sc) {
        /* No-op for shim contexts. -update fires on the main thread;
         * calling OSMesaMakeCurrent here would bind the OSMesa context
         * to the MAIN thread's TLS dispatch, breaking compositor-thread
         * rendering. Resize is handled entirely by the viewport check
         * in -flushBuffer, which runs on the rendering thread. */
    } else {
        [self shim_update];  /* original */
    }
}

- (void)shim_setValues:(const GLint *)vals forParameter:(NSOpenGLContextParameter)param
{
    struct shim_ctx *sc = get_shim_ctx(self);
    if (sc) {
        /* No-op for shim contexts — no CGL context to configure. */
    } else {
        [self shim_setValues:vals forParameter:param];  /* original */
    }
}

- (CGLContextObj)shim_CGLContextObj
{
    struct shim_ctx *sc = get_shim_ctx(self);
    if (sc) {
        /* Return the shim token as the CGLContextObj. The _CGL* interpose
         * layer in cgl_interpose.c recognises it via registry lookup. */
        return (CGLContextObj)sc;
    }
    return [self shim_CGLContextObj];  /* original for non-shim */
}

@end

/* ===================================================================
 * drawRect blit — draws present_buf into a view during the display
 * cycle. THE ONLY presentation route that works on 10.6: lockFocus
 * re-enters the run loop (deadlock — architecture-3d.md #2),
 * dispatch_async+drawInRect, CGContextDrawImage via graphicsPort and
 * CALayer contents are the other recorded failures. Shared body for
 * both swizzle targets below.
 * =================================================================== */

static void shim_blit_present_buf(id view)
{
    /* HOST-SIDE BLIT rect stash: this runs on the MAIN thread inside
     * the AppKit display cycle — the only AppKit-geometry-safe point.
     * Convert the view's frame to desktop coordinates (origin
     * top-left, matching the scanout resource) and publish for the
     * winsys flush_frontbuffer. In host-present mode the CG blit is
     * skipped entirely — the GPU-side blit delivers the content. */
    if (host_present_mode()) {
        NSView *v = (NSView *)view;
        NSWindow *win = [v window];
        if (win) {
            NSRect vr = [v frame];
            NSRect wr = [win frame];
            /* view origin (bottom-left, window coords) → screen:
             * window frame origin + view offset. */
            CGFloat sx = wr.origin.x + vr.origin.x;
            CGFloat syBl = wr.origin.y + vr.origin.y;
            CGFloat screenH = [[NSScreen mainScreen] frame].size.height;
            int hx = (int)sx;
            /* top-left desktop Y: screen height − bottom − view height */
            int hy = (int)(screenH - syBl - vr.size.height);
            set_host_rect(hx, hy, (int)vr.size.width, (int)vr.size.height);
            static int s_rect_log = 0;
            if (s_rect_log < 5) {
                s_rect_log++;
                fprintf(stderr, "cgl_shim: HOST-BLIT rect %.0fx%.0f at (%d,%d)\n",
                        vr.size.width, vr.size.height, hx, hy);
            }
        }
        return;   /* no CG blit — content arrives via the GPU blit */
    }

    /* SHIM_DROP_BLIT (Mesa-VirGL LEDGER, dump-verdict follow-up):
     * the 2026-08-16 dump showed Gecko's composite CLEAN while the
     * screen duplicates — the blit is the suspect. Early-return
     * leaves Gecko's own software paint standing (the same outcome
     * as the empty-frame guard). Pre-registered: duplicate vanishes
     * and chrome still renders → blit misplacement confirmed as the
     * artifact origin; chrome missing → the blit is load-bearing. */
    {
        static int s_drop_blit = -1;
        if (s_drop_blit < 0) {
            s_drop_blit = (getenv("SHIM_DROP_BLIT") != NULL);
            if (s_drop_blit)
                fprintf(stderr, "cgl_shim: DROP_BLIT: skipping all "
                        "present_buf blits — Gecko's own paint stands\n");
        }
        if (s_drop_blit)
            return;
    }

    NSData *bufData = objc_getAssociatedObject(view, PRESENT_BUF_KEY);
    NSDictionary *sizeDict = objc_getAssociatedObject(view, PRESENT_SIZE_KEY);
    if (!bufData || !sizeDict) return;

    /* Empty-frame guard: when the compositor has no content it clears
     * to fully transparent (observed RGBA(0,0,0,0) across thousands
     * of main-window frames). NSCompositeCopy REPLACES, so blitting a
     * transparent frame paints black over whatever Gecko itself
     * rendered through its software CGContext path (observed 2026-08-13
     * as white-then-black: Gecko's white software paint, erased by our
     * empty blit). Skip the blit and let Gecko's own painting stand.
     *
     * ZZBKH (PR review): the verdict used to come from pixel[0] alone,
     * so a frame with a transparent top-left pixel and real content
     * everywhere else was dropped. The comment says "fully
     * transparent", so test that: sample a fixed grid (every
     * alpha in a stride of at most len/256) and skip ONLY when all
     * sampled alphas are zero. Fewer skips is the safe direction —
     * the skip path replaces our frame with Gecko's software paint. */
    {
        const uint32_t *px = (const uint32_t *)[bufData bytes];
        NSUInteger npx = [bufData length] / 4;
        uint32_t p0 = npx ? px[0] : 0;
        NSUInteger stride = npx / 256;
        if (stride == 0) stride = 1;
        int all_transparent = (npx > 0);
        for (NSUInteger i = 0; i < npx && all_transparent; i += stride)
            if (px[i] & 0xff000000u) all_transparent = 0;
        if (all_transparent) {
            static int s_empty_skip_log = 0;
            if (s_empty_skip_log < 5) {
                s_empty_skip_log++;
                fprintf(stderr, "cgl_shim: blit-skip[%d] empty frame "
                        "p0=0x%08x — Gecko's own paint stands\n",
                        s_empty_skip_log, p0);
            }
            /* DUMP-ON-SKIP (2026-08-19, the queued instrument): the
             * ledger's sparse-frame mechanism left empty frames
             * undumpable — the guard returns before SHIM_DUMP_BUFFER.
             * An empty-buffer dump is itself a datum: uniformly zero =
             * consistent with upload famine (pre-registered reading);
             * content beyond pixel[0] = the single-pixel guard is a
             * false-positive filter eating real frames. Same 24-bit
             * BMP writer as the blit dump, own counter/env. */
            static int s_skip_dump_left = -1, s_skip_dump_idx = 0;
            if (s_skip_dump_left < 0) {
                const char *dv = getenv("SHIM_DUMP_SKIP");
                s_skip_dump_left = dv ? atoi(dv) : 0;
                if (s_skip_dump_left > 0)
                    fprintf(stderr, "cgl_shim: SKIP DUMP ACTIVE "
                            "(SHIM_DUMP_SKIP=%d frames)\n", s_skip_dump_left);
            }
            if (s_skip_dump_left > 0) {
                int w = [[sizeDict objectForKey:@"w"] intValue];
                int h = [[sizeDict objectForKey:@"h"] intValue];
                const unsigned char *buf =
                    (const unsigned char *)[bufData bytes];
                if (w > 0 && h > 0 && w < 8192 && h < 8192) {
                    char dpath[64];
                    snprintf(dpath, sizeof dpath,
                             "/tmp/shim_skip_%02d.bmp", s_skip_dump_idx);
                    FILE *df = fopen(dpath, "wb");
                    if (df) {
                        uint32_t rowsz =
                            ((uint32_t)w * 3 + 3u) & ~3u;
                        uint32_t datasz = rowsz * (uint32_t)h;
                        uint32_t filesz = 54u + datasz;
                        unsigned char hdr[54] = {0};
                        hdr[0] = 'B'; hdr[1] = 'M';
                        *(uint32_t *)&hdr[2] = filesz;
                        *(uint32_t *)&hdr[10] = 54u;
                        *(uint32_t *)&hdr[14] = 40u;
                        *(int32_t *)&hdr[18] = w;
                        *(int32_t *)&hdr[22] = h;
                        *(uint16_t *)&hdr[26] = 1u;
                        *(uint16_t *)&hdr[28] = 24u;
                        *(uint32_t *)&hdr[34] = datasz;
                        fwrite(hdr, 1, 54, df);
                        unsigned char *rowbuf =
                            (unsigned char *)calloc(1, rowsz);
                        if (rowbuf) {
                            for (int y = 0; y < h; y++) {
                                const unsigned char *srow =
                                    buf + (size_t)y * w * 4;
                                for (int x = 0; x < w; x++) {
                                    rowbuf[x*3+0] = srow[x*4+2];
                                    rowbuf[x*3+1] = srow[x*4+1];
                                    rowbuf[x*3+2] = srow[x*4+0];
                                }
                                fwrite(rowbuf, 1, rowsz, df);
                            }
                            free(rowbuf);
                        }
                        fclose(df);
                        s_skip_dump_idx++;
                        s_skip_dump_left--;
                        fprintf(stderr, "cgl_shim: skip-dump[%d] %dx%d "
                                "-> %s\n", s_skip_dump_idx, w, h, dpath);
                    }
                }
            }
            return;
        }
    }

    uint64_t t5 = mach_absolute_time();
    int w = [[sizeDict objectForKey:@"w"] intValue];
    int h = [[sizeDict objectForKey:@"h"] intValue];
    const void *buf = [bufData bytes];

    /* SHIM_DUMP_BUFFER=<n> — the artifact-B discriminator
     * (Mesa-VirGL LEDGER entry 2, pre-registered before build):
     * dump the OSMesa buffer immediately before the drawRect blit.
     *   duplication ALREADY in the buffer → Gecko's compositing
     *     (wrong-source-rect copy upstream of everything of ours)
     *   buffer clean, duplication on screen → the blit introduces it
     * 24-bit BMP, rows written in buffer order — the buffer is
     * bottom-up (GL origin) and BMP stores rows bottom-up by spec,
     * so the dump reads screen-upright with no flip. Dumps the
     * FIRST n non-empty frames (startup IS the artifact page). */
    {
        static int s_dump_left = -1, s_dump_idx = 0;
        if (s_dump_left < 0) {
            const char *dv = getenv("SHIM_DUMP_BUFFER");
            s_dump_left = dv ? atoi(dv) : 0;
            if (s_dump_left > 0)
                fprintf(stderr, "cgl_shim: BUFFER DUMP ACTIVE "
                        "(SHIM_DUMP_BUFFER=%d frames)\n", s_dump_left);
        }
        /* Trigger-correlated dumping (2026-08-16, after the dump-07
         * overreach was caught): the artifact is TRANSIENT — it appears
         * on a URL-bar click — so a startup dump arbitrates nothing.
         * `touch /tmp/shim_dump_now` arms the next 6 blit dumps; the
         * flag is consumed on arming. Dump the moment the artifact is
         * ON SCREEN, then read whether the buffer contains it. */
        if (s_dump_left == 0 && access("/tmp/shim_dump_now", F_OK) == 0) {
            unlink("/tmp/shim_dump_now");
            s_dump_left = 6;
            fprintf(stderr, "cgl_shim: DUMP ARMED — next 6 blits\n");
        }
        if (s_dump_left > 0 && w > 0 && h > 0 && w < 8192 && h < 8192 &&
            (w >= 1000 || s_dump_idx < 2)) {   /* main-window surfaces
                                    * must not hide behind small-frame
                                    * capture — instrument error recorded
                                    * 2026-08-13 for the upload log; first
                                    * 2 any-size frames prove the writer */
            char dpath[64];
            snprintf(dpath, sizeof dpath, "/tmp/shim_dump_%02d.bmp",
                     s_dump_idx);
            FILE *df = fopen(dpath, "wb");
            if (df) {
                uint32_t rowsz = ((uint32_t)w * 3 + 3u) & ~3u;
                uint32_t datasz = rowsz * (uint32_t)h;
                uint32_t filesz = 54u + datasz;
                unsigned char hdr[54] = {0};
                hdr[0] = 'B'; hdr[1] = 'M';
                *(uint32_t *)&hdr[2] = filesz;
                *(uint32_t *)&hdr[10] = 54u;
                *(uint32_t *)&hdr[14] = 40u;      /* BITMAPINFOHEADER */
                *(int32_t *)&hdr[18] = w;
                *(int32_t *)&hdr[22] = h;         /* + → bottom-up rows */
                *(uint16_t *)&hdr[26] = 1u;
                *(uint16_t *)&hdr[28] = 24u;      /* bpp */
                *(uint32_t *)&hdr[34] = datasz;
                fwrite(hdr, 1, 54, df);
                unsigned char *rowbuf =
                    (unsigned char *)calloc(1, rowsz);
                if (rowbuf) {
                    const unsigned char *src = (const unsigned char *)buf;
                    for (int y = 0; y < h; y++) {
                        const unsigned char *srow = src + (size_t)y * w * 4;
                        for (int x = 0; x < w; x++) {
                            rowbuf[x * 3 + 0] = srow[x * 4 + 2]; /* B */
                            rowbuf[x * 3 + 1] = srow[x * 4 + 1]; /* G */
                            rowbuf[x * 3 + 2] = srow[x * 4 + 0]; /* R */
                        }
                        fwrite(rowbuf, 1, rowsz, df);
                    }
                    free(rowbuf);
                }
                fclose(df);
                fprintf(stderr, "cgl_shim: buffer dump[%d] %s %dx%d\n",
                        s_dump_idx, dpath, w, h);
                s_dump_idx++;
                s_dump_left--;
            }
        }
    }

    /* Diagnostic: buffer pointer seen by drawRect vs what flushBuffer swapped */
    {
        static int s_draw_ptr_log = 0;
        if (s_draw_ptr_log < 5) {
            s_draw_ptr_log++;
            uint32_t *bp = (uint32_t *)buf;
            fprintf(stderr, "cgl_shim: drawptr[%d] buf=%p[0]=0x%08x\n",
                    s_draw_ptr_log, (void*)buf, bp ? bp[0] : 0xDEAD);
        }
    }

    /* SHIM_LOG_BLITS — double-blit detector (dump verdict: buffer clean,
     * blit sole content source, screen duplicated → the blit path draws
     * twice at different destinations). Two swizzles can both reach this
     * function: shim_cv_draw_body via its trampolines (Gecko's class) AND
     * the NSView-level
     * shim_drawRect (whose "never fires for ChildView" claim predates the
     * ChildView swizzle). Log view class, bounds, and the CTM AT ENTRY —
     * two consecutive blits with different CTMs is the duplication
     * mechanism, and the CTM delta IS the measured offset. */
    {
        static int s_log_blits = -1;
        static int s_blit_seq = 0;
        if (s_log_blits < 0)
            s_log_blits = (getenv("SHIM_LOG_BLITS") != NULL);
        if (s_log_blits) {
            NSGraphicsContext *gctx = [NSGraphicsContext currentContext];
            double a = 0, b = 0, c = 0, d = 0, tx = 0, ty = 0;
            if (gctx) {
                CGAffineTransform t =
                    CGContextGetCTM((CGContextRef)[gctx graphicsPort]);
                a = t.a; b = t.b; c = t.c; d = t.d; tx = t.tx; ty = t.ty;
            }
            NSRect vb = [view bounds];
            fprintf(stderr, "cgl_shim: BLIT[%d] view=%s bounds=(%.1f,%.1f "
                    "%.1fx%.1f) ctm=[%.2f %.2f %.2f %.2f %.1f %.1f]%s\n",
                    s_blit_seq, class_getName([view class]),
                    vb.origin.x, vb.origin.y, vb.size.width, vb.size.height,
                    a, b, c, d, tx, ty,
                    gctx ? "" : " NO-CTX");
            s_blit_seq++;
        }
    }

    /* Draw present_buf via NSBitmapImageRep — same pattern as killtest */
    unsigned char *planes[1] = { (unsigned char *)buf };
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:planes
                      pixelsWide:w
                      pixelsHigh:h
                   bitsPerSample:8
                 samplesPerPixel:4
                        hasAlpha:YES
                        isPlanar:NO
                  colorSpaceName:NSDeviceRGBColorSpace
                     bytesPerRow:w * 4
                    bitsPerPixel:32];
    if (rep) {
        /* Row-order flip at draw time: content reaches present_buf
         * bottom-up (GL origin). OSMesaPixelStore(OSMESA_Y_UP, 0) is
         * INERT in this Mesa's gallium OSMesa frontend — hash-verified
         * no-op, image byte-identical with/without (2026-08-13) — so
         * the flip is absorbed here via CTM. Zero pixel copies. Note
         * the flip only became visible with real content: solid red
         * and symmetric triangles are flip-invariant. */
        [NSGraphicsContext saveGraphicsState];
        NSAffineTransform *flip = [NSAffineTransform transform];
        [flip translateXBy:0.0 yBy:NSMaxY([view bounds])];
        [flip scaleXBy:1.0 yBy:-1.0];
        [flip concat];
        [rep drawInRect:[view bounds]
               fromRect:NSMakeRect(0, 0, w, h)
              operation:NSCompositeCopy
               fraction:1.0f
         respectFlipped:YES
                  hints:nil];
        [NSGraphicsContext restoreGraphicsState];
        [rep release];
    }
    uint64_t t6 = mach_absolute_time();
    shim_timing_record_draw(t6 - t5);
    /* Diagnostic */
    static int draw_count = 0;
    if (draw_count < 3) {
        fprintf(stderr, "cgl_shim: drawRect[%d] %dx%d pixel RGBA(%d,%d,%d,%d)\n",
                draw_count, w, h,
                ((unsigned char *)buf)[0], ((unsigned char *)buf)[1],
                ((unsigned char *)buf)[2], ((unsigned char *)buf)[3]);
        draw_count++;
    }
}

/* --- Target 1: NSView-level swizzle (covers non-Gecko apps whose
 * views don't override drawRect:). Never fires for Gecko's ChildView
 * because per-class dispatch lets the override win. --- */

@interface NSView (CGLShimDraw)
- (void)shim_drawRect:(NSRect)dirtyRect;
@end

@implementation NSView (CGLShimDraw)

/* CHANGE-TRIGGERED drawRect logging (2026-08-18): the first-40 +
 * every-200th sampling cannot resolve a typing-burst transition (a
 * false negative reads as a result). Log each (class, bounds, CTM)
 * tuple ONCE on first appearance — the transitions and nothing else —
 * with wall-clock time to correlate with user events. Bounded. */
#define DR_SEEN_MAX 2048
static struct { char cls[64]; float bx, by, bw, bh, tx, ty; }
    g_dr_seen[DR_SEEN_MAX];
static int g_dr_seen_n = 0;
static unsigned long g_dr_logged = 0;

static int dr_log_if_new(const char *cls, NSRect bb, CGFloat tx,
                         CGFloat ty)
{
    for (int i = 0; i < g_dr_seen_n; i++) {
        if (strncmp(g_dr_seen[i].cls, cls, sizeof(g_dr_seen[0].cls)) == 0 &&
            g_dr_seen[i].bx == bb.origin.x && g_dr_seen[i].by == bb.origin.y &&
            g_dr_seen[i].bw == bb.size.width && g_dr_seen[i].bh == bb.size.height &&
            g_dr_seen[i].tx == (float)tx && g_dr_seen[i].ty == (float)ty)
            return 0;   /* seen — no log */
    }
    if (g_dr_seen_n < DR_SEEN_MAX) {
        strlcpy(g_dr_seen[g_dr_seen_n].cls, cls,
                sizeof(g_dr_seen[0].cls));
        g_dr_seen[g_dr_seen_n].bx = bb.origin.x;
        g_dr_seen[g_dr_seen_n].by = bb.origin.y;
        g_dr_seen[g_dr_seen_n].bw = bb.size.width;
        g_dr_seen[g_dr_seen_n].bh = bb.size.height;
        g_dr_seen[g_dr_seen_n].tx = (float)tx;
        g_dr_seen[g_dr_seen_n].ty = (float)ty;
        g_dr_seen_n++;
    }
    return 1;
}

- (void)shim_drawRect:(NSRect)dirtyRect
{
    /* DRAWRECT-ENTRY INSTRUMENT (2026-08-18; the drop-blit run proved
     * the duplication lives in Gecko's own paint — the queued question
     * is whether OUR swizzle chain causes Gecko's drawRect to run
     * twice per cycle at a shifted CTM, or Gecko's single paint
     * contains the duplication. Log entry count, class, bounds, dirty
     * rect, CTM. SHIM_DRAWRECT_LOG=1; first 40 + every 200th. */
    {
        static unsigned long s_dr_n = 0;
        static int s_on = -1;
        if (s_on < 0) s_on = (getenv("SHIM_DRAWRECT_LOG") != NULL);
        s_dr_n++;
        if (s_on && g_dr_logged < 2000) {
            CGContextRef cg = nullptr;
            NSGraphicsContext *nsctx = [NSGraphicsContext currentContext];
            if (nsctx) cg = (CGContextRef)[nsctx graphicsPort];
            CGFloat a=0,b=0,c=0,d=0,tx=0,ty=0;
            if (cg) { const CGAffineTransform m = CGContextGetCTM(cg);
                      a=m.a; b=m.b; c=m.c; d=m.d; tx=m.tx; ty=m.ty; }
            NSRect bb = [self bounds];
            const char *cls1 = class_getName([self class]);
            if (dr_log_if_new(cls1, bb, tx, ty)) {
                g_dr_logged++;
                struct timeval tv; gettimeofday(&tv, NULL);
                fprintf(stderr, "DR[T1][%lu] %ld.%03ld %s "
                        "bounds=(%.0f,%.0f %.0fx%.0f) "
                        "dirty=(%.0f,%.0f %.0fx%.0f) ctm=[%.1f %.1f %.1f "
                        "%.1f tx=%.0f ty=%.0f]\n",
                        s_dr_n, (long)tv.tv_sec, (long)tv.tv_usec/1000,
                        cls1,
                        bb.origin.x, bb.origin.y, bb.size.width, bb.size.height,
                        dirtyRect.origin.x, dirtyRect.origin.y,
                        dirtyRect.size.width, dirtyRect.size.height,
                        a, b, c, d, tx, ty);
            }
        }
    }
    shim_blit_present_buf(self);

    /* Call original drawRect for non-shim views */
    [self shim_drawRect:dirtyRect];
}

@end

/* --- Target 2: Gecko's ChildView class, resolved lazily.
 * Method swizzling is per-class: patching NSView does nothing when
 * ChildView overrides drawRect: — Gecko's implementation runs and
 * ours never fires (observed: drawptr 0, blit no samples, window
 * content area white). The view passed to -setView: IS the Gecko
 * view instance, so its class is the exact class to patch. Not done
 * at +load: XUL isn't mapped when this framework initializes. --- */

/* t80b1 (PR review): the drawRect: wrapper is a plain C function
 * reached through the per-slot trampolines installed in shim_setView —
 * no longer a category method, because a category method cannot know
 * WHICH patched Method invoked it (only a trampoline bound to the slot
 * can). idx is that slot; the original IMP lives in g_cv_swz[idx]. */
static void shim_cv_draw_body(id self, SEL _cmd, NSRect dirtyRect,
                              unsigned idx)
{
    /* FIRING probe (LEDGER entry 16): installation was proven
     * ("drawRect swizzled on <class>"), firing never was — the blit
     * below early-returns silently when no present_buf exists, so a
     * firing swizzle is invisible. If this logs for
     * AppleFlurryOpenGLView, the app-drawRect-as-flush-point hook is
     * already in place. */
    {
        static int s_cvd_log = 0;
        if (s_cvd_log < 10) {
            s_cvd_log++;
            fprintf(stderr, "cgl_shim: drawRect FIRED on %s\n",
                    class_getName([self class]));
        }
    }
    /* DRAWRECT-ENTRY INSTRUMENT, Target 2 (see Target 1 note). */
    {
        static unsigned long s_dr2_n = 0;
        static int s2_on = -1;
        if (s2_on < 0) s2_on = (getenv("SHIM_DRAWRECT_LOG") != NULL);
        s_dr2_n++;
        if (s2_on && g_dr_logged < 2000) {
            CGContextRef cg = nullptr;
            NSGraphicsContext *nsctx = [NSGraphicsContext currentContext];
            if (nsctx) cg = (CGContextRef)[nsctx graphicsPort];
            CGFloat a=0,b=0,c=0,d=0,tx=0,ty=0;
            if (cg) { const CGAffineTransform m = CGContextGetCTM(cg);
                      a=m.a; b=m.b; c=m.c; d=m.d; tx=m.tx; ty=m.ty; }
            NSView *selfView2 = (NSView *)self;
            NSRect bb = [selfView2 bounds];
            const char *cls2 = class_getName([self class]);
            if (dr_log_if_new(cls2, bb, tx, ty)) {
                g_dr_logged++;
                struct timeval tv; gettimeofday(&tv, NULL);
                fprintf(stderr, "DR[T2][%lu] %ld.%03ld %s "
                        "bounds=(%.0f,%.0f %.0fx%.0f) ctm=[%.1f %.1f %.1f "
                        "%.1f tx=%.0f ty=%.0f]\n",
                        s_dr2_n, (long)tv.tv_sec, (long)tv.tv_usec/1000,
                        cls2,
                        bb.origin.x, bb.origin.y, bb.size.width, bb.size.height,
                        a, b, c, d, tx, ty);
            }
        }
    }

    /* NSOpenGLView-idiom flush point (entry 16): the app drew its GL
     * IN drawRect and expects AppKit's private flush — which never
     * reaches us. Do the flushBuffer-equivalent here: readback + swap
     * (cgl_shim_present_from_cgl, minus its setNeedsDisplay — we are
     * already in drawRect), then the blit below presents. Gated by
     * SHIM_DRAWRECT_PRESENT (setView arms the association only then). */
    {
        NSValue *scv = objc_getAssociatedObject(self, SC_KEY);
        if (scv) {
            struct shim_ctx *sc0 =
                (struct shim_ctx *)[scv pointerValue];
            /* RESIZE BEFORE THE APP DRAWS (entry 16, the 1x1 lesson:
             * flushBuffer owns the viewport-driven resize; apps that
             * never flushBuffer stay at the initial 1x1 allocation and
             * everything clips to one pixel — observed: drawRect[0]
             * 1x1, presented pixel real but frame one pixel). We are
             * on the main thread inside drawRect — AppKit is safe. */
            NSView *selfView = (NSView *)self;
            NSRect b = [selfView bounds];
            if (sc0 && (sc0->width != (int)b.size.width ||
                        sc0->height != (int)b.size.height) &&
                pthread_mutex_trylock(&sc0->lock) == 0) {
                free_buffers(sc0);
                alloc_buffers(sc0, (int)b.size.width, (int)b.size.height);
                if (sc0->osmesa_ctx && sc0->render_buf)
                    shim_osmesa_make_current(sc0);
                pthread_mutex_unlock(&sc0->lock);
                fprintf(stderr, "cgl_shim: DRAWRECT_PRESENT resized to "
                        "%dx%d\n", sc0->width, sc0->height);
            }
        }
    }

    /* t8dmQ/t80b1: call the ORIGINAL drawRect: of the Method that was
     * invoked — the trampoline bound its table slot at install time, so
     * the original follows the Method, not the receiver's class.
     * [self class] is wrong as a key: the Method is shared with every
     * inheriting subclass (an entry recorded under one class must serve
     * them all), and a nested [super drawRect:] must get the
     * superclass's original, not the subclass's again — same [self
     * class] under objc_msgSendSuper, which used to recurse forever.
     * No fallback branch: install records the entry in the same locked
     * section that plants the trampoline, so a reached trampoline
     * always has a slot. */
    {
        IMP origIMP = NULL;
        pthread_mutex_lock(&g_cv_swz_mtx);
        if (idx < g_cv_swz_n) origIMP = g_cv_swz[idx].orig;
        pthread_mutex_unlock(&g_cv_swz_mtx);
        if (origIMP)
            ((void (*)(id, SEL, NSRect))origIMP)(self, _cmd, dirtyRect);
    }

    {
        NSValue *scv = objc_getAssociatedObject(self, SC_KEY);
        if (scv) {
            struct shim_ctx *sc =
                (struct shim_ctx *)[scv pointerValue];
            if (sc && sc->present_buf) {
                if (sc->osmesa_ctx && sc->render_buf) {
                    shim_osmesa_make_current(sc);
                    substitute_cgl_tls_set((CGLContextObj)sc);
                }
                if (p_glFinish) p_glFinish();
                if (sc->render_buf)
                    shim_readback(sc->width, sc->height, sc->render_buf);
                void *tmp = sc->render_buf;
                sc->render_buf = sc->present_buf;
                sc->present_buf = tmp;
                if (sc->osmesa_ctx && sc->render_buf)
                    shim_osmesa_make_current(sc);
                /* store for the blit below (same as shim_present) */
                NSData *bufData =
                    [NSData dataWithBytesNoCopy:sc->present_buf
                        length:(NSUInteger)(sc->width * sc->height * 4)
                        freeWhenDone:NO];
                NSDictionary *sizeDict =
                    [NSDictionary dictionaryWithObjectsAndKeys:
                        [NSNumber numberWithInt:sc->width], @"w",
                        [NSNumber numberWithInt:sc->height], @"h", nil];
                objc_setAssociatedObject(self, PRESENT_BUF_KEY,
                                         bufData, OBJC_ASSOCIATION_RETAIN);
                objc_setAssociatedObject(self, PRESENT_SIZE_KEY,
                                         sizeDict, OBJC_ASSOCIATION_RETAIN);
            }
        }
    }

    shim_blit_present_buf(self);
}

/* ===================================================================
 * +load: install swizzles at class-load time.
 * =================================================================== */

static void swizzle_method(Class cls, SEL originalSel, SEL shimSel)
{
    Method orig = class_getInstanceMethod(cls, originalSel);
    Method shim = class_getInstanceMethod(cls, shimSel);
    if (orig && shim) {
        method_exchangeImplementations(orig, shim);
    }
}

static void swizzle_class_method(Class cls, SEL originalSel, SEL shimSel)
{
    Method orig = class_getClassMethod(cls, originalSel);
    Method shim = class_getClassMethod(cls, shimSel);
    if (orig && shim) {
        method_exchangeImplementations(orig, shim);
    }
}

@interface CGLShimLoader : NSObject
@end

@implementation CGLShimLoader
+ (void)load
{
    /* Before anything can create an OSMesa context: Mesa's driver
     * selection is a process-global call_once reading GALLIUM_DRIVER,
     * so this must precede the first OSMesaCreateContextExt in the
     * process. The +load probe below doubles as the canary: it must
     * print "virgl", never "(unset)". */
    if (!getenv("GALLIUM_DRIVER"))
        setenv("GALLIUM_DRIVER", "virgl", 0);

    Class cls = [NSOpenGLContext class];

    /* Instance methods */
    swizzle_method(cls, @selector(initWithFormat:shareContext:),
                        @selector(shim_initWithFormat:shareContext:));
    swizzle_method(cls, @selector(makeCurrentContext),
                        @selector(shim_makeCurrentContext));
    swizzle_method(cls, @selector(clearDrawable),
                        @selector(shim_clearDrawable));
    swizzle_method(cls, @selector(flushBuffer),
                        @selector(shim_flushBuffer));
    swizzle_method(cls, @selector(setView:),
                        @selector(shim_setView:));
    swizzle_method(cls, @selector(update),
                        @selector(shim_update));
    swizzle_method(cls, @selector(setValues:forParameter:),
                        @selector(shim_setValues:forParameter:));
    swizzle_method(cls, @selector(CGLContextObj),
                        @selector(shim_CGLContextObj));

    /* Class methods */
    swizzle_class_method(cls, @selector(currentContext),
                              @selector(shim_currentContext));
    swizzle_class_method(cls, @selector(clearCurrentContext),
                              @selector(shim_clearCurrentContext));

    /* Swizzle NSView drawRect for presentation path */
    Class viewCls = [NSView class];
    swizzle_method(viewCls, @selector(drawRect:),
                        @selector(shim_drawRect:));

    /* Pixel-format gate: strip NSOpenGLPFAAccelerated before real
     * AppKit matching (software-only renderer list — see the category
     * comment above). Without this, CompositorOGL cannot get a format. */
    Class pixCls = [NSOpenGLPixelFormat class];
    swizzle_method(pixCls, @selector(initWithAttributes:),
                        @selector(shim_initWithAttributes:));

    shim_timing_init();

    fprintf(stderr, "cgl_shim: swizzles installed (GALLIUM_DRIVER=%s)\n",
            getenv("GALLIUM_DRIVER") ?: "(unset)");
}
@end
