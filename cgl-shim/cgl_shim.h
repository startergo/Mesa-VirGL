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

    /* Generation guard (2026-10-06, the substitute-route abort fix):
     * clearDrawable bumps gen under the lock; the async present
     * captures gen at ENQUEUE and returns if it changed at run —
     * a freed/reused sc handing present a garbage view was the
     * ___forwarding___ abort (shim_present+478, three reports). */
    uint32_t gen;

    /* Serialises flush vs present vs resize for this context.
     * Assumption: one context per view. Internal-only: NEVER returned
     * to callers as CGLLockContext's backing mutex. */
    pthread_mutex_t lock;

    /* CGL-emulation handoff ownership (2026-08-19): real CGL contexts
     * live server-side and context switches are atomic w.r.t. other
     * threads; Gecko relies on that. The substitute's in-process Mesa
     * must provide the same: shim_osmesa_make_current() takes the
     * global handoff mutex around the WHOLE switch (outgoing context's
     * implicit flush included — it lives inside _mesa_make_current).
     * These fields track the owner for violation detection: a
     * MakeCurrent arriving while another thread still owns the context
     * means concurrent same-context use (which real CGL tolerated and
     * we do not) — logged, never fatal. */
    pthread_t owner_th;
    int owned;

    /* Public CGL lock (CGLLockContext/CGLUnlockContext). MUST be a
     * separate mutex from `lock`: Gecko calls CGLLockContext around
     * -flushBuffer on the SAME thread (nsChildView::DoRemoteComposition,
     * nsChildView.mm CGLLockContext call sites) — one shared non-
     * recursive mutex self-deadlocks the first flush. Real CGL's lock
     * serializes caller-side GL usage only; internal shim paths never
     * take it. */
    pthread_mutex_t cgl_lock;

    /* For bounds-check resize detection in -flushBuffer. */
    int last_view_w;
    int last_view_h;

    /* Coalescing flag: prevents unbounded dispatch_async accumulation
     * when the main thread falls behind under TCG. Set under lock
     * before enqueueing; cleared by the presentation block under lock.
     * If already set when -flushBuffer fires, skip the enqueue — the
     * pending present will pick up the latest buffer. */
    int present_pending;
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
