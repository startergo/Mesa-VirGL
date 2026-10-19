/*
 * Wrapper for <string.h>: adds strndup/strnlen declarations missing from
 * the 10.6 SDK. Found via -I before the SDK in the include search path;
 * #include_next reaches the SDK's real <string.h>.
 */
#ifndef COMPAT_STRING_H
#define COMPAT_STRING_H

#include_next <string.h>

/* strndup — macOS 10.7+ */
extern char *strndup(const char *s, size_t n);
/* strnlen — macOS 10.7+ */
extern size_t strnlen(const char *s, size_t maxlen);

#endif
