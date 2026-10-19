#ifndef _XF86DRM_H_
#define _XF86DRM_H_
/* Comprehensive DRM header for macOS Mesa VirGL build.
 * Extracted from .github/workflows/macos.yml CI stubs.
 * All functions are no-op stubs — the DRM winsys will never be called
 * at runtime on macOS (vtest winsys is used instead). */

#include <stdint.h>
#include <stdlib.h>   /* abort() */
#include <sys/types.h>

/* Only define if not already defined by Mesa's drm-uapi headers */
#ifndef DRM_NODE_PRIMARY
#define DRM_NODE_PRIMARY 0
#define DRM_NODE_RENDER 2
#endif

#ifndef DRM_CLOEXEC
#define DRM_CLOEXEC 0x80000000
#endif

/* Basic DRM types - only if not defined */
#ifndef _DRM_H_
typedef int drmDevice;
typedef int drmDevicePtr;
typedef struct drm_version *drmVersionPtr;
#endif

/* Additional DRM function stubs needed by VirGL — all abort.
 * These are unreachable on macOS (vtest winsys is the active path).
 * See drmIoctl comment below for the fail-loud rationale.
 *
 * __builtin_unreachable() after abort() suppresses "control reaches end
 * of non-void function" under -Wall: abort() is noreturn by the standard,
 * but older toolchains don't always treat the libc abort() prototype as
 * _Noreturn, so the compiler conservatively assumes it might return. */
static inline int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle) {
    abort();
    __builtin_unreachable();
}

static inline int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd) {
    abort();
    __builtin_unreachable();
}

static inline drmVersionPtr drmGetVersion(int fd) {
    abort();
    __builtin_unreachable();
}

static inline void drmFreeVersion(drmVersionPtr version) {
    abort();
}

/* Main DRM ioctl stub — must abort, not return success.
 * If the DRM winsys is ever selected at runtime on macOS, something is
 * fundamentally wrong. Returning 0 would produce the exact silent-failure
 * pattern that cost this project weeks on submitCommand (reported success,
 * did nothing). An abort turns the mystery into a stack trace. */
static inline int drmIoctl(int fd, unsigned long request, void *arg) {
    abort();
    __builtin_unreachable();
}

#endif
