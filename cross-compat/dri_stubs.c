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
 */

/* Opaque types matching the real API */
struct driOptionCache;
struct driInfoCache;

/* driParseConfigFiles: parse drirc XML files into option cache.
 * Stub: leave cache empty (all options at defaults). */
void driParseConfigFiles(struct driOptionCache *cache,
    struct driInfoCache *info, unsigned int screen_num,
    const char *driver_name, const char *kernel_driver_name,
    const char *device_name, int api_type) {
    /* No-op: no drirc.d on target, defaults are already set */
}

/* driQueryOptionb: query boolean option from cache.
 * Stub: return false (safe default for all virgl options). */
int driQueryOptionb(struct driOptionCache *cache, unsigned int opt) {
    return 0;
}

/* driQueryOptioni: query integer option from cache.
 * Stub: return 0 (safe default for all virgl options). */
int driQueryOptioni(struct driOptionCache *cache, unsigned int opt) {
    return 0;
}
