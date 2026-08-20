/*
 * cgl_interpose.c — __DATA,__interpose layer for _CGL* functions.
 *
 * Provides __DATA,__interpose entries that dyld applies at load time
 * when this dylib is loaded via DYLD_INSERT_LIBRARIES. Each interposed
 * function checks shim_lookup() to see if the CGLContextObj belongs
 * to the shim. If yes: handle internally (mostly no-ops). If no:
 * forward to the real CGL function.
 *
 * Identity test is by POINTER VALUE — shim_lookup never dereferences
 * the incoming pointer. A wild/stale/NULL pointer simply isn't in the
 * registry and gets forwarded to the real function (which will reject
 * it normally).
 */

#include "cgl_shim.h"
#include <stdio.h>
#include <dlfcn.h>
#include <OpenGL/CGLTypes.h>
#include <OpenGL/CGLCurrent.h>
#include <OpenGL/OpenGL.h>

/* Registry (defined here, declared in cgl_shim.h) */
struct shim_ctx *shim_registry[MAX_SHIM_CONTEXTS];
pthread_mutex_t shim_registry_lock = PTHREAD_MUTEX_INITIALIZER;

int shim_register(struct shim_ctx *ctx)
{
    int ret = -1;
    pthread_mutex_lock(&shim_registry_lock);
    for (int i = 0; i < MAX_SHIM_CONTEXTS; i++) {
        if (shim_registry[i] == NULL) {
            shim_registry[i] = ctx;
            ret = 0;
            break;
        }
    }
    pthread_mutex_unlock(&shim_registry_lock);
    return ret;
}

void shim_unregister(struct shim_ctx *ctx)
{
    pthread_mutex_lock(&shim_registry_lock);
    for (int i = 0; i < MAX_SHIM_CONTEXTS; i++) {
        if (shim_registry[i] == ctx) {
            shim_registry[i] = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&shim_registry_lock);
}

struct shim_ctx *shim_lookup(void *ptr)
{
    struct shim_ctx *found = NULL;
    pthread_mutex_lock(&shim_registry_lock);
    for (int i = 0; i < MAX_SHIM_CONTEXTS; i++) {
        if (shim_registry[i] == ptr) {
            found = shim_registry[i];
            break;
        }
    }
    pthread_mutex_unlock(&shim_registry_lock);
    return found;
}

/* --- Interposed CGL functions --- */

static CGLError
shim_CGLEnable(CGLContextObj ctx, CGLContextEnable pname)
{
    struct shim_ctx *s = shim_lookup(ctx);
    if (s) {
        /* OSMesa doesn't need CGLEnable — no-op for shim contexts. */
        return kCGLNoError;
    }
    /* Forward to real CGLEnable for non-shim contexts. */
    CGLError (*real)(CGLContextObj, CGLContextEnable) =
        dlsym(RTLD_NEXT, "CGLEnable");
    return real ? real(ctx, pname) : kCGLBadContext;
}

static CGLError
shim_CGLDisable(CGLContextObj ctx, CGLContextEnable pname)
{
    struct shim_ctx *s = shim_lookup(ctx);
    if (s) {
        return kCGLNoError;
    }
    CGLError (*real)(CGLContextObj, CGLContextEnable) =
        dlsym(RTLD_NEXT, "CGLDisable");
    return real ? real(ctx, pname) : kCGLBadContext;
}

static CGLError
shim_CGLFlushDrawable(CGLContextObj ctx)
{
    struct shim_ctx *s = shim_lookup(ctx);
    if (s) {
        /* The actual presentation is done in NSOpenGLContext -flushBuffer's
         * swizzle (cgl_shim.mm). CGLFlushDrawable arriving here is a
         * direct CGL call — trigger the same swap+present path. */
        glFinish();
        /* Swap + present handled by the ObjC side via a function pointer
         * set during shim init. For now, no-op — the -flushBuffer swizzle
         * is the primary presentation path. */
        return kCGLNoError;
    }
    CGLError (*real)(CGLContextObj) =
        dlsym(RTLD_NEXT, "CGLFlushDrawable");
    return real ? real(ctx) : kCGLBadContext;
}

static CGLError
shim_CGLDestroyContext(CGLContextObj ctx)
{
    struct shim_ctx *s = shim_lookup(ctx);
    if (s) {
        /* Cleanup is handled by the ObjC dealloc swizzle. Just unregister. */
        shim_unregister(s);
        return kCGLNoError;
    }
    CGLError (*real)(CGLContextObj) =
        dlsym(RTLD_NEXT, "CGLDestroyContext");
    return real ? real(ctx) : kCGLBadContext;
}

/* ===================================================================
 * GL function interpose — route gl* calls to Mesa via dlsym
 *
 * DYLD_INTERPOSE replaces all bindings to the target symbol across the
 * process. The target (e.g. glClear) must bind to OpenGL.framework at
 * link time — NOT to libOSMesa — so that the interpose targets Apple's
 * glClear, which is what apps call. If it bound to Mesa's glClear, the
 * interpose would replace Mesa's implementation and break the shim's
 * own calls.
 *
 * Link order: -framework OpenGL BEFORE -lOSMesa. Verify post-build:
 * nm -m cgl_shim.dylib | grep '_glClear' → (from OpenGL)
 *
 * Forward path: dlopen("libOSMesa.8.dylib") + dlsym. The dlsym'd
 * pointer bypasses the dyld stub, so no recursion.
 * =================================================================== */

void *g_mesa = NULL;
/* Host-side blit entry points in libOSMesa's winsys (2026-08-19). */
int  (*p_cgl_shim_host_present_enabled)(void) = NULL;
void (*p_cgl_shim_set_host_present_rect)(int, int, int, int) = NULL;

/* Cached Mesa function pointers — set once by gl_interpose_init().
 * Non-static: cgl_shim.mm needs to call Mesa directly via these pointers
 * because its own gl* symbol bindings go to OpenGL.framework (the
 * DYLD_INTERPOSE does not apply within the interposing image). */
void   (*p_glBegin)(GLenum);
void   (*p_glClear)(GLbitfield);
void   (*p_glClearColor)(GLclampf, GLclampf, GLclampf, GLclampf);
void   (*p_glColor3f)(GLfloat, GLfloat, GLfloat);
void   (*p_glDisable)(GLenum);
void   (*p_glEnd)(void);
void   (*p_glFinish)(void);
GLenum (*p_glGetError)(void);
void   (*p_glGetFloatv)(GLenum, GLfloat *);
void   (*p_glGetIntegerv)(GLenum, GLint *);
const GLubyte *(*p_glGetString)(GLenum);
void   (*p_glLoadIdentity)(void);
void   (*p_glMatrixMode)(GLenum);
void   (*p_glOrtho)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
void   (*p_glPixelStorei)(GLenum, GLint);
void   (*p_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid *);
void   (*p_glRotatef)(GLfloat, GLfloat, GLfloat, GLfloat);
void   (*p_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid *);
void   (*p_glVertex2f)(GLfloat, GLfloat);
void   (*p_glViewport)(GLint, GLint, GLsizei, GLsizei);

__attribute__((constructor))
static void gl_interpose_init(void)
{
    g_mesa = dlopen("libOSMesa.8.dylib", RTLD_LAZY | RTLD_GLOBAL);
    if (!g_mesa) {
        fprintf(stderr, "cgl_shim: gl_interpose: dlopen libOSMesa FAILED: %s\n",
                dlerror());
        return;
    }
#define GL_DLSYM(name) p_##name = (void *)dlsym(g_mesa, #name)
    p_cgl_shim_host_present_enabled =
        (int (*)(void))dlsym(g_mesa, "cgl_shim_host_present_enabled");
    p_cgl_shim_set_host_present_rect =
        (void (*)(int, int, int, int))dlsym(g_mesa, "cgl_shim_set_host_present_rect");
    GL_DLSYM(glBegin);
    GL_DLSYM(glClear);
    GL_DLSYM(glClearColor);
    GL_DLSYM(glColor3f);
    GL_DLSYM(glDisable);
    GL_DLSYM(glEnd);
    GL_DLSYM(glFinish);
    GL_DLSYM(glGetError);
    GL_DLSYM(glGetFloatv);
    GL_DLSYM(glGetIntegerv);
    GL_DLSYM(glGetString);
    GL_DLSYM(glLoadIdentity);
    GL_DLSYM(glMatrixMode);
    GL_DLSYM(glOrtho);
    GL_DLSYM(glPixelStorei);
    GL_DLSYM(glReadPixels);
    GL_DLSYM(glRotatef);
    GL_DLSYM(glTexImage2D);
    GL_DLSYM(glVertex2f);
    GL_DLSYM(glViewport);
#undef GL_DLSYM
    fprintf(stderr, "cgl_shim: gl_interpose_init: g_mesa=%p p_glClear=%p p_glGetString=%p p_glFinish=%p\n",
            g_mesa, (void*)p_glClear, (void*)p_glGetString, (void*)p_glFinish);
}

/* Thin wrappers — if Mesa context is current, forward to Mesa via dlsym.
 * If not (Apple's CGL internal calls during context creation), fall through
 * to Apple's original via RTLD_NEXT. This prevents the interpose blast
 * radius from crashing Apple's CGL when Mesa has no current context. */
#define GL_IP_VOID(name, params, args) \
    static void ip_##name params { \
        if (OSMesaGetCurrentContext() && p_##name) { p_##name args; return; } \
        static void (*o) params; if (!o) o = dlsym(RTLD_NEXT, #name); \
        if (o) o args; \
    }
#define GL_IP_RET(name, rettype, params, args) \
    static rettype ip_##name params { \
        if (OSMesaGetCurrentContext() && p_##name) return p_##name args; \
        static rettype (*o) params; if (!o) o = dlsym(RTLD_NEXT, #name); \
        return o ? o args : (rettype)0; \
    }

GL_IP_VOID(glBegin,       (GLenum a), (a))
GL_IP_VOID(glClear,       (GLbitfield a), (a))
GL_IP_VOID(glClearColor,  (GLclampf a, GLclampf b, GLclampf c, GLclampf d), (a,b,c,d))
GL_IP_VOID(glColor3f,     (GLfloat a, GLfloat b, GLfloat c), (a,b,c))
GL_IP_VOID(glDisable,     (GLenum a), (a))
GL_IP_VOID(glEnd,         (void), ())
GL_IP_VOID(glFinish,      (void), ())
GL_IP_RET (glGetError,    GLenum, (void), ())
GL_IP_VOID(glGetIntegerv, (GLenum a, GLint *b), (a,b))
GL_IP_RET (glGetString,   const GLubyte *, (GLenum a), (a))
GL_IP_VOID(glLoadIdentity,(void), ())
GL_IP_VOID(glMatrixMode,  (GLenum a), (a))
GL_IP_VOID(glOrtho,       (GLdouble a, GLdouble b, GLdouble c, GLdouble d, GLdouble e, GLdouble f), (a,b,c,d,e,f))
GL_IP_VOID(glReadPixels,  (GLint a, GLint b, GLsizei c, GLsizei d, GLenum e, GLenum f, GLvoid *g), (a,b,c,d,e,f,g))
GL_IP_VOID(glRotatef,     (GLfloat a, GLfloat b, GLfloat c, GLfloat d), (a,b,c,d))
GL_IP_VOID(glVertex2f,    (GLfloat a, GLfloat b), (a,b))
GL_IP_VOID(glViewport,    (GLint a, GLint b, GLsizei c, GLsizei d), (a,b,c,d))

/* Diagnostic: confirm the interpose fires and check what it sees.
 * Also probes _glapi_get_dispatch() so we can compare the dispatch
 * pointer against the one logged from the shim's glFinish path. Same
 * non-NULL value = TLS dispatch fine; NULL here & non-NULL there =
 * emulated-TLS confirmed (the interpose path reads a different
 * _glapi_tls_Dispatch slot than OSMesaMakeCurrent wrote). */
static void diag_glViewport(GLint a, GLint b, GLsizei c, GLsizei d) {
    static int logged = 0;
    OSMesaContext osmc = OSMesaGetCurrentContext();
    if (!logged) { logged = 1;
        extern const void *_glapi_get_dispatch(void);
        extern const void *_glapi_get_context(void);
        const void *disp = _glapi_get_dispatch();
        const void *gctx = _glapi_get_context();
        fprintf(stderr, "cgl_shim: ip_glViewport FIRED osmc=%p p=%p args=(%d,%d,%d,%d) "
                "dispatch=%p context=%p thread=%p\n",
                (void*)osmc, (void*)p_glViewport, a, b, c, d,
                (void*)disp, (void*)gctx, (void*)pthread_self());

        /* Probe: call p_glViewport (Mesa's entry stub) and check state before
         * and after via p_glGetIntegerv. If state updates, the path works
         * for this call. If not, _mesa_Viewport isn't taking effect. */
        {
            GLint vp_before[4] = {-1, -1, -1, -1};
            p_glGetIntegerv(GL_VIEWPORT, vp_before);
            p_glViewport(a, b, c, d);
            GLint vp_after[4] = {-1, -1, -1, -1};
            p_glGetIntegerv(GL_VIEWPORT, vp_after);
            GLenum err_after = p_glGetError();
            fprintf(stderr, "cgl_shim:               p_glViewport: "
                    "before=(%d,%d,%d,%d) after=(%d,%d,%d,%d) glError=0x%x\n",
                    vp_before[0], vp_before[1], vp_before[2], vp_before[3],
                    vp_after[0], vp_after[1], vp_after[2], vp_after[3], err_after);
        }

        /* Check MaxViewport dims correctly — returns 2 values */
        GLint dims[2] = {0, 0};
        p_glGetIntegerv(GL_MAX_VIEWPORT_DIMS, dims);
        fprintf(stderr, "cgl_shim:               MaxViewportDims=(%d,%d)\n",
                dims[0], dims[1]);
    }
    if (osmc && p_glViewport) { p_glViewport(a,b,c,d); return; }
    static void (*o)(GLint, GLint, GLsizei, GLsizei); if (!o) o = dlsym(RTLD_NEXT, "glViewport");
    if (o) o(a,b,c,d);
}

/* --- __DATA,__interpose section ---
 *
 * EXCLUDED from the substitute framework build. In the substitute model,
 * we ARE OpenGL.framework — interpose entries would replace gl* bindings
 * in ALL images (AppKit, QuartzCore, etc.) with ip_gl* stubs that check
 * OSMesaGetCurrentContext(). For AppKit's own GL (window compositing),
 * there's no OSMesa context current → RTLD_NEXT → NULL → no-op.
 * Windows stop rendering. The substitute doesn't need interposition:
 * gl* resolves to Mesa via -reexport_library. */
#ifndef SUBSTITUTE_FRAMEWORK
__attribute__((used))
static const struct { const void *replacement; const void *original; }
shim_interpose_entries[] __attribute__((section("__DATA,__interpose"))) = {
    /* CGL interposes */
    { (const void *)shim_CGLEnable,       (const void *)CGLEnable },
    { (const void *)shim_CGLDisable,      (const void *)CGLDisable },
    { (const void *)shim_CGLFlushDrawable, (const void *)CGLFlushDrawable },
    { (const void *)shim_CGLDestroyContext, (const void *)CGLDestroyContext },
    /* GL interposes — target binds to OpenGL via link order.
     * Forward path goes to Mesa via dlsym. */
    { (const void *)ip_glBegin,           (const void *)glBegin },
    { (const void *)ip_glClear,           (const void *)glClear },
    { (const void *)ip_glClearColor,      (const void *)glClearColor },
    { (const void *)ip_glColor3f,         (const void *)glColor3f },
    { (const void *)ip_glDisable,         (const void *)glDisable },
    { (const void *)ip_glEnd,             (const void *)glEnd },
    { (const void *)ip_glFinish,          (const void *)glFinish },
    { (const void *)ip_glGetError,        (const void *)glGetError },
    { (const void *)ip_glGetIntegerv,     (const void *)glGetIntegerv },
    { (const void *)ip_glGetString,       (const void *)glGetString },
    { (const void *)ip_glLoadIdentity,    (const void *)glLoadIdentity },
    { (const void *)ip_glMatrixMode,      (const void *)glMatrixMode },
    { (const void *)ip_glOrtho,           (const void *)glOrtho },
    { (const void *)ip_glReadPixels,      (const void *)glReadPixels },
    { (const void *)ip_glRotatef,         (const void *)glRotatef },
    { (const void *)ip_glVertex2f,        (const void *)glVertex2f },
    { (const void *)diag_glViewport,      (const void *)glViewport },
};
#endif /* !SUBSTITUTE_FRAMEWORK */
