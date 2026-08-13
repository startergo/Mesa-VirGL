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
#include <GL/gl.h>
#include <GL/osmesa.h>
#include <pthread.h>
#include <stdlib.h>
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
extern void   (*p_glGetIntegerv)(GLenum, GLint *);
extern const GLubyte *(*p_glGetString)(GLenum);
extern void   (*p_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid *);
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

static void alloc_buffers(struct shim_ctx *sc, int w, int h)
{
    size_t sz = (size_t)w * (size_t)h * 4;
    sc->render_buf  = calloc(1, sz);
    sc->present_buf = calloc(1, sz);
    sc->width = w;
    sc->height = h;
}

static void free_buffers(struct shim_ctx *sc)
{
    free(sc->render_buf);   sc->render_buf = NULL;
    free(sc->present_buf);  sc->present_buf = NULL;
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
    }
}

/* Called from shim_drawRect — logs blit cost (if verbose) and folds into
 * rolling stats. Single main thread, so no lock needed. */
static void shim_timing_record_draw(uint64_t blit_ticks)
{
    if (g_timing_verbose) {
        fprintf(stderr, "cgl_shim: drawRect blit=%.2f ms\n",
                shim_ticks_to_ms(blit_ticks));
    }
    if (g_timing.warmup_settled) {
        shim_stats_update(&g_timing.blit, blit_ticks);
    }
}

/* --- Presentation (runs on main thread via dispatch_async) --- */

/* Associated object key for present_buf on the view (for drawRect swizzle) */
static char kPresentBufKey;
static char kPresentSizeKey;
#define PRESENT_BUF_KEY (&kPresentBufKey)
#define PRESENT_SIZE_KEY (&kPresentSizeKey)

static void shim_present(NSOpenGLContext *nsc)
{
    struct shim_ctx *sc = get_shim_ctx(nsc);
    if (!sc || !sc->present_buf || !sc->view) return;

    NSView *view = (NSView *)sc->view;

    /* Clear the coalescing flag, store present_buf on the view for
     * drawRect to pick up. */
    pthread_mutex_lock(&sc->lock);
    sc->present_pending = 0;

    NSData *bufData = [NSData dataWithBytesNoCopy:sc->present_buf
                                           length:(NSUInteger)(sc->width * sc->height * 4)
                                     freeWhenDone:NO];
    NSDictionary *sizeDict = [NSDictionary dictionaryWithObjectsAndKeys:
                              [NSNumber numberWithInt:sc->width], @"w",
                              [NSNumber numberWithInt:sc->height], @"h", nil];
    objc_setAssociatedObject(view, PRESENT_BUF_KEY, bufData, OBJC_ASSOCIATION_RETAIN);
    objc_setAssociatedObject(view, PRESENT_SIZE_KEY, sizeDict, OBJC_ASSOCIATION_RETAIN);

    pthread_mutex_unlock(&sc->lock);

    /* Trigger drawRect on the main thread */
    [view performSelectorOnMainThread:@selector(setNeedsDisplay:)
                           withObject:[NSNumber numberWithBool:YES]
                        waitUntilDone:NO];
}

/* ===================================================================
 * Swizzled NSOpenGLContext methods.
 *
 * Each shim_ method calls the original via [self shim_xxx] (because
 * method_exchangeImplementations swaps the IMPs — calling shim_xxx
 * now invokes the original).
 * =================================================================== */

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
    sc->osmesa_ctx = OSMesaCreateContextExt(OSMESA_RGBA, 16, 0, 0, NULL);
    fprintf(stderr, "cgl_shim: post-OSMesaCreateContextExt osmesa_ctx=%p\n",
            (void*)sc->osmesa_ctx);
    if (!sc->osmesa_ctx) {
        free(sc);
        return self;
    }

    alloc_buffers(sc, 1, 1);
    OSMesaMakeCurrent(sc->osmesa_ctx, sc->render_buf, GL_UNSIGNED_BYTE, 1, 1);
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
    pthread_mutex_init(&sc->lock, NULL);
    sc->last_view_w = -1;
    sc->last_view_h = -1;

    shim_register(sc);
    set_shim_ctx(self, sc);

    return self;
}

+ (NSOpenGLContext *)shim_currentContext
{
    /* Gecko's IsCurrent() compares this against its own mContext pointer.
     * Must return the actual NSOpenGLContext*, not a token. */
    return get_current_ctx();
}

+ (void)shim_clearCurrentContext
{
    set_current_ctx(nil);
    OSMesaMakeCurrent(NULL, NULL, 0, 0, 0);
}

- (void)shim_makeCurrentContext
{
    struct shim_ctx *sc = get_shim_ctx(self);
    if (sc && sc->osmesa_ctx && sc->render_buf) {
        set_current_ctx(self);
        OSMesaMakeCurrent(sc->osmesa_ctx, sc->render_buf, GL_UNSIGNED_BYTE,
                          sc->width, sc->height);
    } else {
        /* Fall through to original for non-shim contexts. */
        [self shim_makeCurrentContext];
    }
}

- (void)shim_clearDrawable
{
    struct shim_ctx *sc = get_shim_ctx(self);
    if (sc) {
        set_current_ctx(nil);
        OSMesaMakeCurrent(NULL, NULL, 0, 0, 0);
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

    /* Probe: viewport BEFORE resize check. If viewport=(0,0,800,600) here,
     * the glViewport from ip_glViewport persisted. If (0,0,0,0), something
     * between ip_glViewport and flushBuffer reset it. */
    {
        static int s_pre_resize_log = 0;
        if (s_pre_resize_log < 3) {
            s_pre_resize_log++;
            GLint vp[4] = {-1, -1, -1, -1};
            p_glGetIntegerv(GL_VIEWPORT, vp);
            const void *ctx = _glapi_get_context();
            fprintf(stderr, "cgl_shim: pre-resize[%d] viewport=(%d,%d,%d,%d) context=%p\n",
                    s_pre_resize_log, vp[0], vp[1], vp[2], vp[3], (void*)ctx);
        }
    }

    /* --- Resize detection (design problem #2) ---
     * Gecko doesn't use NSOpenGLView, so -update may never fire.
     * Check view bounds on every flushBuffer and reallocate if changed. */
    NSView *view = (NSView *)sc->view;
    if (view) {
        NSRect bounds = [view bounds];
        int vw = (int)bounds.size.width;
        int vh = (int)bounds.size.height;
        if (vw > 0 && vh > 0 && (vw != sc->last_view_w || vh != sc->last_view_h)) {
            pthread_mutex_lock(&sc->lock);
            free_buffers(sc);
            alloc_buffers(sc, vw, vh);
            OSMesaMakeCurrent(sc->osmesa_ctx, sc->render_buf, GL_UNSIGNED_BYTE, vw, vh);
            sc->last_view_w = vw;
            sc->last_view_h = vh;
            pthread_mutex_unlock(&sc->lock);
        }
    }

    /* --- Compositor-thread marshalling (design problem #1) --- */

    /* Diagnostic: is the OSMesa context current? Which renderer? */
    {
        static int s_ctx_log = 0;
        if (s_ctx_log < 5) {
            s_ctx_log++;
            OSMesaContext osmc = OSMesaGetCurrentContext();
            const GLubyte *renderer = p_glGetString(GL_RENDERER);
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
    p_glFinish();  /* flush Mesa's command queue to the host */
    g_timing.t2 = mach_absolute_time();

    /* Force readback from host GPU to guest buffer. virgl renders on the
     * host; the guest-side OSMesa buffer only gets the pixels when a
     * TRANSFER_FROM_HOST_3D fires. softpipe renders directly into the
     * guest buffer and doesn't need this. Without it, present_buf stays
     * zero-filled and the window shows white. */
    p_glReadPixels(0, 0, sc->width, sc->height, GL_RGBA, GL_UNSIGNED_BYTE,
                   sc->render_buf);
    g_timing.t3 = mach_absolute_time();

    /* Diagnostic: scratch readback with sentinels + error check.
     * Sentinels distinguish "wrote zeros" from "didn't write". */
    {
        static int s_diag_log = 0;
        if (s_diag_log < 5) {
            s_diag_log++;
            uint32_t scratch[4] = {0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF};
            p_glReadPixels(0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, scratch);
            GLenum err1 = p_glGetError();
            GLint vp[4] = {0};
            p_glGetIntegerv(GL_VIEWPORT, vp);
            fprintf(stderr, "cgl_shim: diag[%d] scratch=(0x%08x,0x%08x,0x%08x,0x%08x) "
                    "glError=0x%x viewport=(%d,%d,%d,%d)\n",
                    s_diag_log, scratch[0], scratch[1], scratch[2], scratch[3],
                    err1, vp[0], vp[1], vp[2], vp[3]);
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

    pthread_mutex_lock(&sc->lock);
    /* Swap render and present buffers */
    void *tmp = sc->render_buf;
    sc->render_buf = sc->present_buf;
    sc->present_buf = tmp;
    /* Re-bind context to new render buffer.
     * THE correctness-critical step: without this, subsequent rendering
     * writes into the buffer the main thread is reading. */
    {
        GLboolean mc_ret = OSMesaMakeCurrent(sc->osmesa_ctx, sc->render_buf,
                                              GL_UNSIGNED_BYTE, sc->width, sc->height);
        static int s_swap_mc_log = 0;
        if (s_swap_mc_log < 5) {
            s_swap_mc_log++;
            fprintf(stderr, "cgl_shim: swap_mc[%d] ret=%d buf=%p %dx%d\n",
                    s_swap_mc_log, (int)mc_ret, (void*)sc->render_buf,
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
        dispatch_async(dispatch_get_main_queue(), ^{
            shim_present(ctxForPresent);
        });
    }

    g_timing.t4 = mach_absolute_time();
    shim_timing_record_flush(sc);
}

- (void)shim_setView:(NSView *)view
{
    struct shim_ctx *sc = get_shim_ctx(self);
    if (sc) {
        /* Retain new view, release old. 10.6 has no zeroing weak refs. */
        if (sc->view) [(NSView *)sc->view release];
        if (view) [view retain];
        sc->view = view;

        /* Allocate buffer at view size immediately if valid. */
        if (view) {
            NSRect bounds = [view bounds];
            int vw = (int)bounds.size.width;
            int vh = (int)bounds.size.height;
            if (vw > 0 && vh > 0) {
                pthread_mutex_lock(&sc->lock);
                free_buffers(sc);
                alloc_buffers(sc, vw, vh);
                OSMesaMakeCurrent(sc->osmesa_ctx, sc->render_buf,
                                  GL_UNSIGNED_BYTE, vw, vh);
                sc->last_view_w = vw;
                sc->last_view_h = vh;
                pthread_mutex_unlock(&sc->lock);
            }
        }
    } else {
        [self shim_setView:view];  /* original */
    }
}

- (void)shim_update
{
    struct shim_ctx *sc = get_shim_ctx(self);
    if (sc) {
        /* -update is called by NSOpenGLView on resize. For shim contexts,
         * realloc at current view size. Gecko may never call this — the
         * bounds-check in -flushBuffer is the primary resize trigger. */
        NSView *view = (NSView *)sc->view;
        if (view) {
            NSRect bounds = [view bounds];
            int vw = (int)bounds.size.width;
            int vh = (int)bounds.size.height;
            if (vw > 0 && vh > 0) {
                pthread_mutex_lock(&sc->lock);
                free_buffers(sc);
                alloc_buffers(sc, vw, vh);
                OSMesaMakeCurrent(sc->osmesa_ctx, sc->render_buf,
                                  GL_UNSIGNED_BYTE, vw, vh);
                sc->last_view_w = vw;
                sc->last_view_h = vh;
                pthread_mutex_unlock(&sc->lock);
            }
        }
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
 * NSView drawRect swizzle — draws present_buf into the view when
 * the system calls drawRect during the display cycle.
 * =================================================================== */

@interface NSView (CGLShimDraw)
- (void)shim_drawRect:(NSRect)dirtyRect;
@end

@implementation NSView (CGLShimDraw)

- (void)shim_drawRect:(NSRect)dirtyRect
{
    /* Check if this view has a present buffer (only shim-attached views do). */
    NSData *bufData = objc_getAssociatedObject(self, PRESENT_BUF_KEY);
    NSDictionary *sizeDict = objc_getAssociatedObject(self, PRESENT_SIZE_KEY);
    if (bufData && sizeDict) {
        uint64_t t5 = mach_absolute_time();
        int w = [[sizeDict objectForKey:@"w"] intValue];
        int h = [[sizeDict objectForKey:@"h"] intValue];
        const void *buf = [bufData bytes];

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
            [rep drawInRect:[self bounds]
                   fromRect:NSMakeRect(0, 0, w, h)
                  operation:NSCompositeCopy
                   fraction:1.0f
             respectFlipped:YES
                      hints:nil];
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

    /* Call original drawRect for non-shim views */
    [self shim_drawRect:dirtyRect];
}

@end

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

    shim_timing_init();

    fprintf(stderr, "cgl_shim: swizzles installed (GALLIUM_DRIVER=%s)\n",
            getenv("GALLIUM_DRIVER") ?: "(unset)");
}
@end
