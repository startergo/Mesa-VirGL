/* substitute_stub.c — the two post-Aug-24 symbols cgl_shim.mm
 * references, stubbed: the shim never needed the substitute
 * framework's machinery, and linking its object pulled a second
 * C++ runtime into the dylib (static-init segfault under
 * DYLD_INSERT, 2026-10-06; my 128e edit exonerated by the
 * gated discriminator build). Empty TLS-set, no-op diff. */
#include <stdint.h>
void substitute_cgl_tls_set(void* ctx)
{
    (void)ctx;
}
void cgl_shim_diff_at_present(const void* buf, int w, int h)
{
    (void)buf; (void)w; (void)h;
}
