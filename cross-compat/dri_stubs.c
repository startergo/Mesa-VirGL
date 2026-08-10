/*
 * Stub implementations for DRI config (drirc) functions.
 * xmlconfig.c is not compiled in the cross-10.6 build despite
 * -Dxmlconfig=enabled — meson's feature resolution appears to fail
 * for expat in cross-compilation mode. These functions read driver
 * options from drirc.d XML files; there is no drirc.d on a stock
 * Snow Leopard guest, and the driver's default options are sensible.
 *
 * Stubs return safe defaults rather than abort() — unlike the video
 * stubs, these ARE on the driver init path (virgl_create_screen calls
 * driParseConfigFiles at startup). Aborting would prevent the driver
 * from initializing at all. Returning "no config found" is the correct
 * behavior when there's no drirc to parse.
 *
 * Signatures match src/util/xmlconfig.h exactly (types AND parameter
 * count) so the linker is not relying on ABI luck. The real types live
 * in xmlconfig.h; here we forward-declare the one struct the caller
 * passes by pointer so the prototypes are self-contained.
 */

#include <stdint.h>

/* Forward-declared to match the real API in xmlconfig.h without pulling
 * in Mesa's full header chain (mesa-sha1.h, ralloc.h, etc.). The real
 * typedef is `typedef struct driOptionCache { ... } driOptionCache;`
 * (a tagged struct), so we declare it under the same tag name. */
struct driOptionCache;

/* driParseConfigFiles: parse drirc XML files into option cache.
 * Real signature (xmlconfig.h): 10 parameters.
 * Stub: no-op, leaves cache at defaults. */
void driParseConfigFiles(struct driOptionCache *cache,
                         const struct driOptionCache *info,
                         int screenNum, const char *driverName,
                         const char *kernelDriverName, const char *deviceName,
                         const char *applicationName, uint32_t applicationVersion,
                         const char *engineName, uint32_t engineVersion) {
    /* No-op: no drirc.d on target, defaults are already set */
}

/* driQueryOptionb: query boolean option value.
 * Real signature (xmlconfig.h): returns unsigned char, 2nd param is const char *.
 * Stub: return false (safe default for all virgl options). */
unsigned char driQueryOptionb(const struct driOptionCache *cache,
                              const char *name) {
    return 0;
}

/* driQueryOptioni: query integer option value.
 * Real signature (xmlconfig.h): returns int, 2nd param is const char *.
 * Stub: return 0 (safe default for all virgl options). */
int driQueryOptioni(const struct driOptionCache *cache,
                    const char *name) {
    return 0;
}
