#ifndef _XF86DRM_H_
#define _XF86DRM_H_
/* Comprehensive DRM header for macOS Mesa VirGL build.
 * Extracted from .github/workflows/macos.yml CI stubs.
 * All functions are no-op stubs — the DRM winsys will never be called
 * at runtime on macOS (vtest winsys is used instead). */

#include <stdint.h>
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

/* Additional DRM function stubs needed by VirGL */
static inline int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle) {
    return -1;
}

static inline int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd) {
    return -1;
}

static inline drmVersionPtr drmGetVersion(int fd) {
    return NULL;
}

static inline void drmFreeVersion(drmVersionPtr version) {
}

/* Main DRM ioctl stub */
static inline int drmIoctl(int fd, unsigned long request, void *arg) {
    return 0;
}

#endif
