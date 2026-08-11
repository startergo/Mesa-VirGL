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
#include <objc/runtime.h>
#include <GL/gl.h>
#include <GL/osmesa.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "cgl_shim.h"

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

/* --- Presentation (runs on main thread via dispatch_async) --- */

static void shim_present(NSOpenGLContext *nsc)
{
    struct shim_ctx *sc = get_shim_ctx(nsc);
    if (!sc || !sc->present_buf || !sc->view) return;

    NSView *view = (NSView *)sc->view;
    NSRect bounds = [view bounds];
    if (bounds.size.width < 1 || bounds.size.height < 1) return;

    /* Clear the coalescing flag under the lock — next -flushBuffer
     * will enqueue a new present. */
    pthread_mutex_lock(&sc->lock);
    sc->present_pending = 0;
    unsigned char *planes[1] = { (unsigned char *)sc->present_buf };
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:planes
                      pixelsWide:sc->width
                      pixelsHigh:sc->height
                   bitsPerSample:8
                 samplesPerPixel:4
                        hasAlpha:YES
                        isPlanar:NO
                  colorSpaceName:NSDeviceRGBColorSpace
                     bytesPerRow:sc->width * 4
                    bitsPerPixel:32];
    pthread_mutex_unlock(&sc->lock);

    if (!rep) return;

    [NSGraphicsContext saveGraphicsState];
    [rep drawInRect:bounds
           fromRect:NSMakeRect(0, 0, sc->width, sc->height)
          operation:NSCompositeCopy
           fraction:1.0f];
    [NSGraphicsContext restoreGraphicsState];
    [rep release];
    [view setNeedsDisplay:YES];
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
    /* Call original to get a real NSOpenGLContext object. We need the
     * object for AppKit state tracking, but we'll route GL through OSMesa. */
    self = [self shim_initWithFormat:fmt shareContext:share];
    if (!self) return nil;

    /* Set GALLIUM_DRIVER if not already set. softpipe is the default
     * fallback; virgl is the target. */
    if (!getenv("GALLIUM_DRIVER"))
        setenv("GALLIUM_DRIVER", "virgl", 0);

    /* Create the shim context state. */
    struct shim_ctx *sc = (struct shim_ctx *)calloc(1, sizeof(struct shim_ctx));
    if (!sc) { return self; }

    sc->magic = SHIM_MAGIC;
    sc->osmesa_ctx = OSMesaCreateContextExt(OSMESA_BGRA, 16, 0, 0, NULL);
    if (!sc->osmesa_ctx) {
        free(sc);
        return self;
    }

    /* Start with a 1x1 buffer; reallocated on -setView: or -flushBuffer. */
    alloc_buffers(sc, 1, 1);
    OSMesaMakeCurrent(sc->osmesa_ctx, sc->render_buf, GL_UNSIGNED_BYTE, 1, 1);
    pthread_mutex_init(&sc->lock, NULL);
    sc->last_view_w = -1;
    sc->last_view_h = -1;

    /* Register for CGL token lookup and store on the context. */
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
    glFinish();  /* simple-correct; fence would be the optimisation */

    pthread_mutex_lock(&sc->lock);
    /* Swap render and present buffers */
    void *tmp = sc->render_buf;
    sc->render_buf = sc->present_buf;
    sc->present_buf = tmp;
    /* Re-bind context to new render buffer.
     * THE correctness-critical step: without this, subsequent rendering
     * writes into the buffer the main thread is reading. */
    OSMesaMakeCurrent(sc->osmesa_ctx, sc->render_buf, GL_UNSIGNED_BYTE,
                      sc->width, sc->height);

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
    /* Most parameters are no-ops for OSMesa. Pass through to original
     * for AppKit bookkeeping. */
    [self shim_setValues:vals forParameter:param];
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

    fprintf(stderr, "cgl_shim: swizzles installed (GALLIUM_DRIVER=%s)\n",
            getenv("GALLIUM_DRIVER") ?: "(unset)");
}
@end
