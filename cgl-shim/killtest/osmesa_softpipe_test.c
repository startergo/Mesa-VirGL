/*
 * osmesa_softpipe_test.c — minimal validation that libOSMesa.8.dylib
 * loads in 10.6's dyld, the TLS gate survives, and softpipe produces
 * correct pixels.
 *
 * Usage: DYLD_LIBRARY_PATH=/tmp GALLIUM_DRIVER=softpipe ./osmesa_softpipe_test
 *
 * Renders two known-color clears, reads back the buffer, verifies bytes.
 * No window, no AppKit, no socket — pure offscreen OSMesa.
 *
 * Expected values use the LEDGER's non-boundary convention:
 *   Color A: (0.20, 0.40, 0.60, 1.00) → RGB(51, 102, 153) = BGRA(153, 102, 51, 255)
 *   Color B: (0.80, 0.20, 0.40, 1.00) → RGB(204, 51, 102) = BGRA(102, 51, 204, 255)
 */
#include <GL/gl.h>
#include <GL/osmesa.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 256
#define H 256

static int check_clear(OSMesaContext ctx, const char *name,
                        float r, float g, float b, float a,
                        int exp_R, int exp_G, int exp_B) {
    unsigned char buf[W * H * 4];

    if (!OSMesaMakeCurrent(ctx, buf, GL_UNSIGNED_BYTE, W, H)) {
        fprintf(stderr, "FAIL [%s]: OSMesaMakeCurrent failed\n", name);
        return 1;
    }

    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();

    /* Check first and last pixel — full-buffer clear should be uniform */
    unsigned char *p0 = buf;
    unsigned char *pLast = buf + (W * H - 1) * 4;

    /* OSMESA_BGRA byte order: [B, G, R, A] */
    int tol = 1; /* ±1 ULP per GL spec on float-to-unorm */
    int ok = 1;

    #define CHECK(px, label) do { \
        int dB = abs((int)px[0] - exp_B); \
        int dG = abs((int)px[1] - exp_G); \
        int dR = abs((int)px[2] - exp_R); \
        int dA = abs((int)px[3] - 255); \
        if (dB > tol || dG > tol || dR > tol || dA > tol) { \
            fprintf(stderr, "FAIL [%s/%s]: B=%d G=%d R=%d A=%d  " \
                    "expected B=%d G=%d R=%d A=255\n", \
                    name, label, px[0], px[1], px[2], px[3], \
                    exp_B, exp_G, exp_R); \
            ok = 0; \
        } \
    } while (0)

    CHECK(p0, "first");
    CHECK(pLast, "last");

    if (ok) {
        /* PRRT_kwDOPZhcm86uEfdE: buffer is OSMESA_BGRA ([B,G,R,A]), so
         * (p[2], p[1], p[0]) is R,G,B — print it under the right
         * label; expected stays RGB(r,g,b). */
        fprintf(stdout, "PASS [%s]: first=RGB(%d,%d,%d) last=RGB(%d,%d,%d) "
                        "expected RGB(%d,%d,%d)\n",
                name, p0[2], p0[1], p0[0], pLast[2], pLast[1], pLast[0],
                exp_R, exp_G, exp_B);
    }
    return ok ? 0 : 1;
}

int main(void) {
    fprintf(stdout, "osmesa_softpipe_test starting\n");
    fflush(stdout);

    OSMesaContext ctx = OSMesaCreateContextExt(OSMESA_BGRA, 16, 0, 0, NULL);
    if (!ctx) {
        fprintf(stderr, "FAIL: OSMesaCreateContextExt returned NULL — "
                        "library loaded but context creation failed\n");
        return 2;
    }
    fprintf(stdout, "OK: OSMesaCreateContextExt succeeded (ctx=%p)\n", ctx);
    fflush(stdout);

    int fail = 0;

    /* Clear A: green-dominant — non-boundary values per LEDGER convention */
    fail |= check_clear(ctx, "clearA", 0.20f, 0.40f, 0.60f, 1.0f,
                         51, 102, 153);

    /* Clear B: red-dominant */
    fail |= check_clear(ctx, "clearB", 0.80f, 0.20f, 0.40f, 1.0f,
                         204, 51, 102);

    OSMesaDestroyContext(ctx);

    if (fail) {
        fprintf(stderr, "RESULT: FAIL (%d clear(s) wrong)\n", fail);
    } else {
        fprintf(stdout, "RESULT: PASS — both clears byte-exact (±1 ULP)\n");
    }
    return fail ? 1 : 0;
}
