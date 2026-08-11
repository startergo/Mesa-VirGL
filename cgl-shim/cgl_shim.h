/*
 * cgl_shim.h — shared definitions for the CGL shim.
 *
 * The shim intercepts NSOpenGLContext (via ObjC swizzle) and _CGL*
 * functions (via __DATA,__interpose) to route GL rendering through
 * OSMesa → virgl → host GPU instead of Apple's software renderer.
 *
 * Loaded via DYLD_INSERT_LIBRARIES. Context lifecycle is intercepted;
 * GL entry points resolve via DYLD_LIBRARY_PATH (no interposition).
 */
#ifndef CGL_SHIM_H
#define CGL_SHIM_H

#include <stdint.h>
#include <pthread.h>
#include <GL/gl.h>
#include <GL/osmesa.h>

/* "SHIM" in ASCII — belt-and-braces inside a struct whose identity is
 * established by registry membership, not by dereferencing this field. */
#define SHIM_MAGIC 0x5348494D

#define MAX_SHIM_CONTEXTS 16

/*
 * Per-context shim state. One instance per NSOpenGLContext created
 * through the swizzled -initWithFormat:shareContext:.
 *
 * Lifetime: created in -initWithFormat, destroyed in -dealloc or
 * CGLDestroyContext. Registered in shim_registry[] for CGL token
 * lookup.
 */
struct shim_ctx {
    uint32_t magic;             /* SHIM_MAGIC */

    OSMesaContext osmesa_ctx;   /* the real Mesa context */

    /* Double-buffered rendering: compositor renders into render_buf,
     * main thread reads from present_buf. Swapped under lock in
     * -flushBuffer, with OSMesaMakeCurrent re-bind to the new
     * render_buf (the correctness-critical step). */
    void *render_buf;
    void *present_buf;
    int width;
    int height;

    /* NSView retained on -setView:, released on dealloc. */
    void *view;                 /* NSView* (void* to avoid ObjC in C header) */

    /* Serialises flush vs present vs resize for this context.
     * Assumption: one context per view. */
    pthread_mutex_t lock;

    /* For bounds-check resize detection in -flushBuffer. */
    int last_view_w;
    int last_view_h;
};

/* Registry of shim contexts. Membership is tested by POINTER VALUE —
 * never dereference an untrusted pointer to check identity.
 * Same shape as the kext's m_user_backings[] backing table. */
extern struct shim_ctx *shim_registry[MAX_SHIM_CONTEXTS];
extern pthread_mutex_t shim_registry_lock;

/* Register/unregister a shim context. Returns 0 on success. */
#ifdef __cplusplus
extern "C" {
#endif
int shim_register(struct shim_ctx *ctx);
void shim_unregister(struct shim_ctx *ctx);

/* Look up a pointer in the registry. Returns the ctx if found,
 * NULL if not. NEVER dereferences the incoming pointer. */
struct shim_ctx *shim_lookup(void *ptr);
#ifdef __cplusplus
}
#endif

#endif /* CGL_SHIM_H */
