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

/* --- __DATA,__interpose section --- */
__attribute__((used))
static const struct { const void *replacement; const void *original; }
shim_interpose_entries[] __attribute__((section("__DATA,__interpose"))) = {
    { (const void *)shim_CGLEnable,       (const void *)CGLEnable },
    { (const void *)shim_CGLDisable,      (const void *)CGLDisable },
    { (const void *)shim_CGLFlushDrawable, (const void *)CGLFlushDrawable },
    { (const void *)shim_CGLDestroyContext, (const void *)CGLDestroyContext },
};
