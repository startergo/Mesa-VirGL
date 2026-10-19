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

#include <time.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
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
        /* Review PRRT_kwDOPZhcm86YKBwS: this was a bare glFinish() — the
         * frame was never swapped or presented, so a caller using raw CGL
         * against a shim context got a frozen window while the
         * -flushBuffer path worked. Present through the same entry the
         * substitute framework uses (cgl_shim_present_from_cgl in
         * cgl_shim.mm: readback into render_buf, swap, rebind, store on
         * the view, trigger drawRect). It takes sc->lock with trylock and
         * drops the frame rather than blocking this thread. */
        extern void cgl_shim_present_from_cgl(void *scv);
        cgl_shim_present_from_cgl(s);
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
/* GL 1.5 prototype — the 10.6 SDK's gl.h stops at 1.2; the
 * symbol exists in OpenGL.framework for interposition. */
extern void glUniform4fv(GLint location, GLsizei count,
                         const GLfloat* value);
extern void glUniform1f(GLint, GLfloat);
extern void glUniform2f(GLint, GLfloat, GLfloat);
extern void glUniform3f(GLint, GLfloat, GLfloat, GLfloat);
extern void glUniform4f(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
/* RUNG 128e (the uniform-leg boundary capture): the app's
 * glUniform* calls pass through here BEFORE any engine
 * transformation — log every 4f upload's first vec4. */
void   (*p_glUniform4fv)(GLint, GLsizei, const GLfloat*);
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
    GL_DLSYM(glUniform4fv);
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

/* RUNG 128e: the uniform-boundary capture — every glUniform4fv
 * logged (first vec4) to the stub's log via the same file the
 * stub reads; env-gated VMGLD_R128E. */
static void ip_glUniform4fv(GLint loc, GLsizei cnt,
                            const GLfloat* v)
{
    static int s_128e = -1;
    if (s_128e < 0)
        s_128e = getenv("VMGLD_R128E") ? 1 : 0;
    if (s_128e && v) {
        FILE* f = fopen("/var/log/vm_gld_stub.log", "a");
        if (f) {
            struct timeval tv;
            gettimeofday(&tv, 0);
            char ts[32];
            strftime(ts, sizeof(ts), "%H:%M:%S",
                     localtime((const time_t*)&tv.tv_sec));
            for (GLsizei i = 0; i < cnt && i < 4; i++)
                fprintf(f,
                    "[%s pid=%d] rung128e: glUniform4fv loc=%d "
                    "[%d]=(%.4g,%.4g,%.4g,%.4g)\n",
                    ts, (int)getpid(), loc, (int)i,
                    v[i*4], v[i*4+1], v[i*4+2], v[i*4+3]);
            fclose(f);
        }
        s_128e = 0;   /* one upload per process reported */
    }
    if (OSMesaGetCurrentContext() && p_glUniform4fv) {
        p_glUniform4fv(loc, cnt, v); return;
    }
    static void (*o)(GLint, GLsizei, const GLfloat*);
    if (!o) o = dlsym(RTLD_NEXT, "glUniform4fv");
    if (o) o(loc, cnt, v);
}
/* RUNG 128e2: the SCALAR uniform family — glmark2's wrapper
 * uses glUniformNf, not 4fv. Same one-per-process log shape. */
#define ULOG128(ar, n) do { \
    if (s_128e2 < 24 && v_ok) { s_128e2++; \
        char b128[160]; int o128 = snprintf(b128, sizeof(b128), \
            "rung128e: glUniform%s loc=%d:", n, loc); \
        for (int q = 0; q < ar && o128 < 140; q++) \
            o128 += snprintf(b128 + o128, sizeof(b128) - o128, \
                " %.4g", (double)v[q]); \
        ep_log128(b128); } \
} while (0)
static int s_128e2 = 0;
static void ep_log128(const char* m)
{
    FILE* f = fopen("/var/log/vm_gld_stub.log", "a");
    if (!f) return;
    struct timeval tv; gettimeofday(&tv, 0);
    char ts[32];
    time_t t = (time_t)tv.tv_sec;
    strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&t));
    fprintf(f, "[%s pid=%d] %s\n", ts, (int)getpid(), m);
    fclose(f);
}
static void ip_glUniform1f(GLint loc, GLfloat x)
{ int v_ok=1; GLfloat v[1]={x}; ULOG128(1,"1f");
  if (OSMesaGetCurrentContext() && p_glUniform4fv) {}
  static void (*o)(GLint, GLfloat);
  if (!o) o = dlsym(RTLD_NEXT, "glUniform1f");
  if (o) o(loc, x); }
static void ip_glUniform2f(GLint loc, GLfloat x, GLfloat y)
{ int v_ok=1; GLfloat v[2]={x,y}; ULOG128(2,"2f");
  static void (*o)(GLint, GLfloat, GLfloat);
  if (!o) o = dlsym(RTLD_NEXT, "glUniform2f");
  if (o) o(loc, x, y); }
static void ip_glUniform3f(GLint loc, GLfloat x, GLfloat y, GLfloat z)
{ int v_ok=1; GLfloat v[3]={x,y,z}; ULOG128(3,"3f");
  static void (*o)(GLint, GLfloat, GLfloat, GLfloat);
  if (!o) o = dlsym(RTLD_NEXT, "glUniform3f");
  if (o) o(loc, x, y, z); }
static void ip_glUniform4f(GLint loc, GLfloat x, GLfloat y,
                           GLfloat z, GLfloat w)
{ int v_ok=1; GLfloat v[4]={x,y,z,w}; ULOG128(4,"4f");
  static void (*o)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
  if (!o) o = dlsym(RTLD_NEXT, "glUniform4f");
  if (o) o(loc, x, y, z, w); }
/* RUNG 128e4: glad (glmark2's loader) resolves GL entries via
 * dlsym at startup — log every uniform-class query and its answer. */
static int s_128e4 = 0;
static void* ip_dlsym(void* handle, const char* name)
{
    static void* (*o)(void*, const char*);
    if (!o) o = (void* (*)(void*, const char*))
        dlsym(RTLD_NEXT, "dlsym");
    void* r = o ? o(handle, name) : 0;
    if (name && r && s_128e4 < 24 && strstr(name, "Uniform")) {
        s_128e4++;
        char b128[160];
        snprintf(b128, sizeof(b128),
                 "rung128e: dlsym(%s) -> %p", name, r);
        FILE* f = fopen("/var/log/vm_gld_stub.log", "a");
        if (f) {
            struct timeval tv; gettimeofday(&tv, 0);
            time_t t = (time_t)tv.tv_sec;
            char ts[32];
            strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&t));
            fprintf(f, "[%s pid=%d] %s\n", ts, (int)getpid(), b128);
            fclose(f);
        }
    }
    return r;
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
    /* Review PRRT_kwDOPZhcm86Yova8: the probe must not dereference p_*
     * when libOSMesa failed to load (gl_interpose_init leaves them NULL).
     * Gate the whole one-shot block on what it calls, so the function
     * still reaches the Apple fallback below. */
    if (!logged) { logged = 1;
        if (osmc && p_glViewport && p_glGetIntegerv && p_glGetError) {
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
         * for this call. If not, _mesa_Viewport isn't taking effect.
         * No p_glGetError() here: glGetError CLEARS the pending error, so a
         * probe would eat the application's error state. */
        {
            GLint vp_before[4] = {-1, -1, -1, -1};
            p_glGetIntegerv(GL_VIEWPORT, vp_before);
            p_glViewport(a, b, c, d);
            GLint vp_after[4] = {-1, -1, -1, -1};
            p_glGetIntegerv(GL_VIEWPORT, vp_after);
            fprintf(stderr, "cgl_shim:               p_glViewport: "
                    "before=(%d,%d,%d,%d) after=(%d,%d,%d,%d)\n",
                    vp_before[0], vp_before[1], vp_before[2], vp_before[3],
                    vp_after[0], vp_after[1], vp_after[2], vp_after[3]);
        }

        /* Check MaxViewport dims correctly — returns 2 values */
        GLint dims[2] = {0, 0};
        p_glGetIntegerv(GL_MAX_VIEWPORT_DIMS, dims);
        fprintf(stderr, "cgl_shim:               MaxViewportDims=(%d,%d)\n",
                dims[0], dims[1]);
        } else {
            fprintf(stderr, "cgl_shim: ip_glViewport FIRED but no OSMesa ctx / "
                    "p_* unresolved (osmc=%p) — using Apple fallback\n",
                    (void*)osmc);
        }
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
    { (const void *)ip_glUniform4fv,     (const void *)glUniform4fv },
    { (const void *)ip_dlsym,             (const void *)dlsym },
    { (const void *)ip_glUniform1f,      (const void *)glUniform1f },
    { (const void *)ip_glUniform2f,      (const void *)glUniform2f },
    { (const void *)ip_glUniform3f,      (const void *)glUniform3f },
    { (const void *)ip_glUniform4f,      (const void *)glUniform4f },
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
