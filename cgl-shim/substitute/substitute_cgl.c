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
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>

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

__attribute__((constructor))
static void substitute_init(void)
{
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

    /* Create OSMesa context */
    OSMesaContext osmesa = OSMesaCreateContextExt(OSMESA_RGBA, 16, 0, 0, NULL);
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
    sc->render_buf = calloc(1, 4);  /* 1x1 RGBA */
    sc->present_buf = calloc(1, 4);
    pthread_mutex_init(&sc->lock, NULL);
    pthread_mutex_init(&sc->cgl_lock, NULL);
    sc->last_view_w = -1;
    sc->last_view_h = -1;

    OSMesaMakeCurrent(osmesa, sc->render_buf, GL_UNSIGNED_BYTE, 1, 1);

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
    return (CGLContextObj)found;
}

CGLError
CGLSetCurrentContext(CGLContextObj ctx)
{
    if (!ctx) {
        OSMesaMakeCurrent(NULL, NULL, 0, 0, 0);
        return kCGLNoError;
    }

    struct shim_ctx *sc = shim_lookup(ctx);
    if (!sc) return kCGLBadContext;

    OSMesaMakeCurrent(sc->osmesa_ctx, sc->render_buf, GL_UNSIGNED_BYTE,
                      sc->width, sc->height);
    return kCGLNoError;
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
    /* Forward to glFinish if shim context; real if not */
    struct shim_ctx *sc = shim_lookup(ctx);
    if (sc) {
        /* The actual presentation is handled by cgl_shim.mm's
         * NSOpenGLContext -flushBuffer swizzle. Direct CGL flush
         * is a no-op for shim contexts (or glFinish via p_glFinish). */
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
CGL_FORWARD(CGLReleaseContext, void, (CGLContextObj ctx), (ctx))
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
CGL_FORWARD(CGLSetSurface, CGLError, (CGLContextObj ctx, GLuint read_write, GLuint surface_id, GLenum surface_type, GLint width, GLint height), (ctx, read_write, surface_id, surface_type, width, height))
CGL_FORWARD(CGLSetVirtualScreen, CGLError, (CGLContextObj ctx, GLint screen), (ctx, screen))
CGL_FORWARD(CGLTexImageIOSurface2D, CGLError, (CGLContextObj ctx, GLenum target, GLenum internal_format, GLsizei width, GLsizei height, GLenum format, GLenum type, void *iosurface, GLuint plane), (ctx, target, internal_format, width, height, format, type, iosurface, plane))
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
