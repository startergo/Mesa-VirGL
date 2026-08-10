#ifndef _LIBSYNC_H_
#define _LIBSYNC_H_
/* Stub libsync header for macOS Mesa VirGL build.
 * Extracted from .github/workflows/macos.yml CI stubs. */

#include <stdint.h>

#define SYNC_IOC_WAIT       1
#define SYNC_IOC_MERGE      2
#define SYNC_IOC_INFO       3
#define SYNC_IOC_FILE_INFO  4

struct sync_fence_info {
    char name[32];
    int status;
    uint64_t timestamp;
};

struct sync_file_info {
    char name[32];
    int status;
    uint32_t num_fences;
    struct sync_fence_info *fences;
};

static inline int sync_wait(int fd, int timeout) { return 0; }
static inline int sync_merge(const char *name, int fd1, int fd2) { return -1; }
static inline int sync_fence_info(int fd, struct sync_file_info *info) { return -1; }
static inline void sync_accumulate(const char *name, int *fence_fd, int fd) {
}

#endif
