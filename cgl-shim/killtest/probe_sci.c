/* probe_sci.c — is _glScissor exported by the substitute and bound to
 * the wrapper?
 *
 * Run on the guest:
 *   SHIM_SCISSOR_LOG=1 /tmp/probe_sci
 *
 * Discriminates the zero-scissor-lines ambiguity (2026-08-17):
 *   prints "ptr=..." and calls glScissor(0,0,64,64) twice
 *   wrapper bound  -> banner + one SCISSOR[0] line (second call deduped)
 *   ptr resolves to Mesa's re-export -> ptr non-NULL, NO banner
 *   ptr NULL                        -> not exported at all
 */
#include <stdio.h>
#include <dlfcn.h>

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

    f(0, 0, 64, 64);
    f(0, 0, 64, 64);   /* same rect: exercises the dedup path */
    f(8, 16, 32, 32);  /* changed rect: second line if wrapper bound */
    printf("RESULT: called glScissor x3 — banner/lines above ⇒ wrapper "
           "bound; silence ⇒ dlsym gave Mesa's re-export\n");
    return 0;
}
