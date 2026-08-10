/*
 * OS compat shims for macOS 10.6.
 * Compiled as a separate translation unit, linked via the cross file.
 */
#include <sys/time.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>

/* clock_gettime — macOS 10.12+ */
int clock_gettime(int clk_id, struct timespec *ts)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ts->tv_sec = tv.tv_sec;
    ts->tv_nsec = (long)tv.tv_usec * 1000;
    return 0;
}

/* strndup — macOS 10.7+ */
char *strndup(const char *s, size_t n)
{
    size_t len = strlen(s);
    if (len > n) len = n;
    char *p = (char *)malloc(len + 1);
    if (p) { memcpy(p, s, len); p[len] = '\0'; }
    return p;
}

/* strnlen — macOS 10.7+ */
size_t strnlen(const char *s, size_t maxlen)
{
    size_t len = 0;
    while (len < maxlen && s[len]) len++;
    return len;
}

/* open_memstream — macOS 10.13+. Minimal implementation using tmpfile. */
#include <stdio.h>
FILE *open_memstream(char **bufp, size_t *sizep)
{
    FILE *f = tmpfile();
    if (!f) return NULL;
    *bufp = NULL;
    *sizep = 0;
    return f;
}
