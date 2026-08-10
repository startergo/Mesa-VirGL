/*
 * OS compat shims for macOS 10.6 SDK.
 *
 * Provides the following POSIX/BSD functions that are missing or incomplete
 * in the 10.6 SDK:
 *
 *   - clock_gettime:   Uses mach_absolute_time() for CLOCK_MONOTONIC and
 *                      CLOCK_MONOTONIC_RAW (monotonic, never goes backward),
 *                      and gettimeofday() for CLOCK_REALTIME. Unsupported
 *                      clock IDs return -1 with errno=EINVAL.
 *   - strndup:         POSIX.1-2008 strndup; examines at most n bytes via
 *                      memchr() so it never reads past the bound.
 *   - strnlen:         POSIX.1-2008 strnlen.
 *   - open_memstream:  GNU/BSD open_memstream built on funopen() (BSD,
 *                      available since 10.4). Maintains a growing malloc'd
 *                      buffer; *bufp and *sizep are kept valid from open
 *                      through close and are updated on each write and on
 *                      close. Caller must free(*bufp) after fclose. The
 *                      buffer is NUL-terminated for convenience, but *sizep
 *                      does not count the NUL.
 *
 * Compiled as a separate translation unit, linked via the cross file.
 * _DARWIN_C_SOURCE is defined before any include so <stdio.h> exposes
 * funopen() (it is hidden under strict POSIX source modes).
 */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif

#include <sys/types.h>
#include <sys/time.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <mach/mach_time.h>
#include <stdio.h>

/* The 10.6 SDK has no clockid_t / CLOCK_* macros. The cross file's c_args
 * defines CLOCK_REALTIME=0 and CLOCK_MONOTONIC=1, but step 1 of build_10.6.md
 * compiles this file standalone (no -D flags), so define them here if the
 * SDK / compile flags haven't already. Values match macOS. */
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW 2
#endif

/* ------------------------------------------------------------------ */
/* clock_gettime — macOS 10.12+                                       */
/* ------------------------------------------------------------------ */

int clock_gettime(int clk_id, struct timespec *ts)
{
    switch (clk_id) {
    case CLOCK_REALTIME: {
        struct timeval tv;
        if (gettimeofday(&tv, NULL) != 0)
            return -1;
        ts->tv_sec  = tv.tv_sec;
        ts->tv_nsec = (long)tv.tv_usec * 1000;
        return 0;
    }
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW: {
        /*
         * mach_absolute_time() is monotonic and unaffected by wall-clock
         * adjustments (NTP, manual changes, DST). Convert Mach absolute
         * time (machine ticks) to nanoseconds via mach_timebase_info.
         */
        static mach_timebase_info_data_t s_tbi;
        static int s_have_tbi = 0;
        if (!s_have_tbi) {
            if (mach_timebase_info(&s_tbi) != KERN_SUCCESS) {
                errno = EINVAL;
                return -1;
            }
            s_have_tbi = 1;
        }
        uint64_t abs  = mach_absolute_time();
        uint64_t nano = (uint64_t)((double)abs *
                                   (double)s_tbi.numer /
                                   (double)s_tbi.denom);
        ts->tv_sec  = (time_t)(nano / 1000000000ULL);
        ts->tv_nsec = (long)(nano % 1000000000ULL);
        return 0;
    }
    default:
        errno = EINVAL;
        return -1;
    }
}

/* ------------------------------------------------------------------ */
/* strndup — macOS 10.7+                                              */
/* ------------------------------------------------------------------ */

char *strndup(const char *s, size_t n)
{
    /*
     * Use memchr() so we never read past the bound n. If a NUL appears
     * within the first n bytes, len is the offset of that NUL; otherwise
     * len is exactly n. This matches POSIX.1-2008 strndup.
     */
    const char *endp = (const char *)memchr(s, '\0', n);
    size_t len = endp ? (size_t)(endp - s) : n;
    char *p = (char *)malloc(len + 1);
    if (p) {
        memcpy(p, s, len);
        p[len] = '\0';
    }
    return p;
}

/* ------------------------------------------------------------------ */
/* strnlen — macOS 10.7+                                              */
/* ------------------------------------------------------------------ */

size_t strnlen(const char *s, size_t maxlen)
{
    const char *endp = (const char *)memchr(s, '\0', maxlen);
    return endp ? (size_t)(endp - s) : maxlen;
}

/* ------------------------------------------------------------------ */
/* open_memstream — macOS 10.13+                                      */
/* ------------------------------------------------------------------ */

/*
 * State carried in the funopen() cookie. Owned by the FILE *; freed on
 * close. The buffer is grown as writes arrive and is exposed to the caller
 * via *bufp / *sizep so that the caller can read the captured bytes after
 * fclose (and must free() them).
 */
struct memstream {
    char  **bufp;    /* caller's out-pointer                      */
    size_t *sizep;   /* caller's out-size                         */
    char   *buf;     /* current buffer (may be NULL if unused)    */
    size_t  len;     /* bytes written so far                      */
    size_t  cap;     /* allocated capacity (>= len + 1)           */
};

/*
 * Grow the buffer so it can hold at least `need` bytes plus a trailing NUL.
 * On success, updates st->cap, st->buf, *st->bufp and *st->sizep. Returns 0
 * on success, -1 on allocation failure (errno left as ENOMEM).
 */
static int memstream_reserve(struct memstream *st, size_t need)
{
    if (need == (size_t)-1) return -1; /* overflow guard */
    if (need + 1 <= st->cap) return 0;

    size_t newcap = st->cap ? st->cap : 64;
    while (newcap < need + 1) {
        if (newcap > (size_t)-1 / 2) { newcap = need + 1; break; }
        newcap *= 2;
    }
    char *p = (char *)realloc(st->buf, newcap);
    if (!p) {
        errno = ENOMEM;
        return -1;
    }
    st->buf = p;
    st->cap = newcap;
    st->buf[st->len] = '\0'; /* keep the invariant: buf[len] == '\0' */
    *st->bufp  = st->buf;
    *st->sizep = st->len;
    return 0;
}

/* funopen write callback: append `len` bytes from `data`. Return 0 on
 * success, -1 on error (funopen treats nonzero write return as failure). */
static int memstream_write(void *cookie, const char *data, int len)
{
    if (len < 0) { errno = EINVAL; return -1; }
    struct memstream *st = (struct memstream *)cookie;
    size_t ulen = (size_t)len;
    /* Guard against size_t overflow on st->len + ulen. */
    if (ulen > (size_t)-1 - st->len) { errno = ENOMEM; return -1; }
    if (memstream_reserve(st, st->len + ulen) != 0)
        return -1;
    memcpy(st->buf + st->len, data, ulen);
    st->len += ulen;
    st->buf[st->len] = '\0';
    *st->bufp  = st->buf;
    *st->sizep = st->len;
    return 0;
}

/* funopen seek callback. open_memstream is seekable; seeking updates the
 * logical size and zero-fills any gap. The returned cursor is the new write
 * offset. */
static fpos_t memstream_seek(void *cookie, fpos_t offset, int whence)
{
    struct memstream *st = (struct memstream *)cookie;
    fpos_t base;
    switch (whence) {
        case SEEK_SET: base = 0;                break;
        case SEEK_CUR: base = (fpos_t)st->len;  break;
        case SEEK_END: base = (fpos_t)st->len;  break;
        default: errno = EINVAL; return -1;
    }
    if (offset < 0 && (fpos_t)(-offset) > base) { errno = EINVAL; return -1; }
    fpos_t newpos = base + offset;
    if (newpos < 0) { errno = EINVAL; return -1; }
    if ((size_t)newpos > (size_t)-1 - 1) { errno = ENOMEM; return -1; }
    if (memstream_reserve(st, (size_t)newpos) != 0)
        return -1;
    if ((size_t)newpos > st->len) {
        /* Zero-fill the gap so the buffer is well-defined. */
        memset(st->buf + st->len, 0, (size_t)newpos - st->len);
        st->len = (size_t)newpos;
    }
    st->buf[st->len] = '\0';
    *st->bufp  = st->buf;
    *st->sizep = st->len;
    return newpos;
}

/* funopen close callback: finalize *bufp/*sizep, free cookie. Returns 0 on
 * success. The buffer is left for the caller to free(). */
static int memstream_close(void *cookie)
{
    struct memstream *st = (struct memstream *)cookie;
    if (!st) return 0;
    /*
     * Ensure the caller always sees at least an empty NUL-terminated buffer
     * (matching glibc open_memstream semantics: caller can always free
     * *bufp after fclose, even if no writes occurred).
     */
    if (!st->buf) {
        st->buf = (char *)malloc(1);
        if (st->buf) { st->buf[0] = '\0'; st->cap = 1; }
    } else {
        st->buf[st->len] = '\0';
    }
    *st->bufp  = st->buf;
    *st->sizep = st->len;
    free(st);
    return 0;
}

FILE *open_memstream(char **bufp, size_t *sizep)
{
    if (!bufp || !sizep) {
        errno = EINVAL;
        return NULL;
    }
    /*
     * Initialize the caller's out-parameters up front so they remain valid
     * from open through close even if no writes happen (glibc guarantee).
     */
    *bufp  = NULL;
    *sizep = 0;

    struct memstream *st = (struct memstream *)calloc(1, sizeof(*st));
    if (!st) { errno = ENOMEM; return NULL; }
    st->bufp  = bufp;
    st->sizep = sizep;

    FILE *f = funopen((const void *)st,
                      /*read*/  NULL,
                      /*write*/ memstream_write,
                      /*seek*/  memstream_seek,
                      /*close*/ memstream_close);
    if (!f) {
        int saved = errno;
        free(st);
        errno = saved;
        return NULL;
    }
    return f;
}
