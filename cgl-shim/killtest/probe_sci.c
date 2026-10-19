/* probe_sci.c — is glScissor exported by the substitute, and what does
 * dlsym hand out for it?
 *
 * Run on the guest:
 *   /tmp/probe_sci
 *
 * History (2026-08-17): this probe was written to discriminate
 * wrapper-vs-re-export via a SHIM_SCISSOR_LOG banner, but no glScissor
 * wrapper ever existed in the substitute (no wrapper function, no
 * _glScissor export, SHIM_SCISSOR_LOG read by nothing) — that outcome
 * was unreachable, so the banner logic was removed
 * The zero-scissor-lines ambiguity was
 * resolved the other way: Gecko makes zero glScissor calls
 * (substitute_cgl.c).
 *
 * Outcomes it CAN show:
 *   dlsym address + three calls through without crashing
 *     -> the framework re-exports Mesa's glScissor
 *   dlsym NULL -> glScissor not exported at all
 */
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <GL/gl.h>
#include <GL/osmesa.h>

typedef void (*scissor_fn)(int, int, int, int);

int main(void)
{
    const char *fw = "/tmp/subst/OpenGL.framework/Versions/A/OpenGL";
    void *h = dlopen(fw, RTLD_LAZY | RTLD_GLOBAL);
    if (!h) {
        printf("dlopen FAILED: %s\n", dlerror());
        return 1;
    }
    printf("dlopen ok %s -> %p\n", fw, h);

    scissor_fn f = (scissor_fn)dlsym(h, "glScissor");
    printf("dlsym glScissor -> %p\n", (void *)f);
    if (!f) {
        printf("RESULT: glScissor NOT EXPORTED by the substitute\n");
        return 2;
    }

    /* glScissor needs a CURRENT GL context - without one the call goes
     * through the no-context dispatch path and proves nothing about the
     * symbol the framework binds. Make a small OSMesa context current
     * (the framework re-exports the OSMesa entry points) and use that. */
    OSMesaContext (*mkctx)(GLenum, GLint, GLint, GLint, OSMesaContext) =
        (OSMesaContext (*)(GLenum, GLint, GLint, GLint, OSMesaContext))
            dlsym(h, "OSMesaCreateContextExt");
    GLboolean (*mkcur)(OSMesaContext, GLenum, GLint, GLint, void *) =
        (GLboolean (*)(OSMesaContext, GLenum, GLint, GLint, void *))
            dlsym(h, "OSMesaMakeCurrent");
    void (*delctx)(OSMesaContext) =
        (void (*)(OSMesaContext))dlsym(h, "OSMesaDestroyContext");
    if (!mkctx || !mkcur || !delctx) {
        printf("RESULT: OSMesa entry points not exported by the "
               "framework - cannot establish a context\n");
        return 3;
    }
    OSMesaContext ctx = mkctx(OSMESA_BGRA, 16, 0, 0, NULL);
    static unsigned char sbuf[64 * 64 * 4];
    if (!ctx || !mkcur(ctx, GL_UNSIGNED_BYTE, 64, 64, sbuf)) {
        printf("RESULT: OSMesa context/make-current failed - the "
               "glScissor calls below would go through no context\n");
        if (ctx) delctx(ctx);
        return 3;
    }

    f(0, 0, 64, 64);
    f(0, 0, 64, 64);   /* same rect: exercises the dedup path */
    f(8, 16, 32, 32);  /* changed rect */
    /* there IS no wrapper to be bound — the
     * substitute never defined a glScissor wrapper, never exported
     * _glScissor and never read SHIM_SCISSOR_LOG (the 2026-08-17
     * ambiguity was resolved the other way: "Gecko makes zero
     * glScissor calls", substitute_cgl.c:192). This probe now reports
     * the plain dlsym outcome: whatever address dlsym returned is
     * Mesa's re-export from the framework, and the calls either
     * reach Mesa's glScissor or crash — that is all it can show. */
    printf("RESULT: dlsym address above is the framework's re-export "
           "(no wrapper exists in the substitute); calls went through "
           "without an observable banner by design\n");
    delctx(ctx);
    return 0;
}
