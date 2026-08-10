/*
 * clock_gettime compat for macOS < 10.12.
 * Compiled as a separate translation unit to avoid -include on assembly files.
 * CLOCK_REALTIME / CLOCK_MONOTONIC are defined via -D in the cross file.
 */
#include <sys/time.h>
#include <time.h>

int clock_gettime(int clk_id, struct timespec *ts)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ts->tv_sec = tv.tv_sec;
    ts->tv_nsec = (long)tv.tv_usec * 1000;
    return 0;
}
