/*
 * Copyright 2022 Yonggang Luo
 * SPDX-License-Identifier: MIT
 */

#include "u_call_once.h"

#include <stdio.h>
#include <stdlib.h>

struct util_call_once_context_t
{
   const void *data;
   util_call_once_data_func func;
};

#if defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__) && __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ < 1070
/* macOS 10.6 lacks TLS runtime (TLV arrived in 10.7), so thread_local is
 * not an option — but POSIX TSD is (pthread is always available on
 * macOS; the include is inside this macOS-only branch on purpose).
 *
 * The context must be per THREAD, never one global: C11 call_once takes
 * no argument, so util_call_once_data_slow parks data/func in the
 * context and the callback reads it back. With a single global, two
 * threads racing on DIFFERENT once_flags overwrite each other's
 * data/func before the callback runs — and every simple_mtx first-lock
 * does exactly that (simple_mtx.h passes its own mutex as data with its
 * own once_flag), so the callback would initialize the wrong mutex and
 * leave the other one uninitialized while it is about to be used.
 *
 * Per-thread TSD restores the thread_local semantics: a thread only ever
 * reads the context it wrote itself, and a thread blocked in call_once
 * on another thread's flag runs no callback at all. */
#include <pthread.h>

static pthread_key_t  call_once_context_key;
static pthread_once_t call_once_context_key_once = PTHREAD_ONCE_INIT;

/* Last-resort context when the thread's allocation fails: racy like the
 * old single global, but only under OOM — still better than dropping
 * the one-time initialization the caller needs. Static, so it needs no
 * key-destructor handling. */
static struct util_call_once_context_t call_once_context_shared;

static void
call_once_context_free(void *p)
{
   free(p);
}

static void
call_once_context_key_init(void)
{
   /* Review thread PRRT_kwDOPZhcm86uAnOQ: pthread_once marks this
    * routine done whether or not it succeeded, so a failed key creation
    * is permanent — and the key variable keeps its zero initialization,
    * after which every pthread_getspecific/pthread_setspecific would use
    * a key id this file never created. On Darwin key ids are
    * process-global small-integer TSD slots, so id 0 can be another
    * subsystem's live key: its per-thread value would be returned as
    * our callback context (arbitrary func/data called through it), and
    * our writes would corrupt that subsystem's state. Creation fails
    * only with the process out of TSD slots (EAGAIN, PTHREAD_KEYS_MAX)
    * or out of memory (ENOMEM) — die at the cause, before any get/set,
    * instead of corrupting unrelated per-thread data. */
   if (pthread_key_create(&call_once_context_key, call_once_context_free) != 0) {
      fprintf(stderr, "u_call_once: pthread_key_create failed - no "
              "per-thread call_once context, aborting\n");
      abort();
   }
}

static struct util_call_once_context_t *
call_once_context_get(void)
{
   struct util_call_once_context_t *ctx;

   pthread_once(&call_once_context_key_once, call_once_context_key_init);
   ctx = (struct util_call_once_context_t *)
      pthread_getspecific(call_once_context_key);
   if (!ctx) {
      ctx = (struct util_call_once_context_t *)
         malloc(sizeof(struct util_call_once_context_t));
      if (ctx) {
         if (pthread_setspecific(call_once_context_key, ctx) != 0) {
            free(ctx);
            ctx = NULL;
         }
      }
      if (!ctx)
         ctx = &call_once_context_shared;
   }
   return ctx;
}
#else
static thread_local struct util_call_once_context_t call_once_context;

static struct util_call_once_context_t *
call_once_context_get(void)
{
   return &call_once_context;
}
#endif

static void
util_call_once_data_slow_once(void)
{
   struct util_call_once_context_t *once_context = call_once_context_get();
   once_context->func(once_context->data);
}

void
util_call_once_data_slow(once_flag *once, util_call_once_data_func func, const void *data)
{
   struct util_call_once_context_t *once_context = call_once_context_get();
   once_context->data = data;
   once_context->func = func;
   call_once(once, util_call_once_data_slow_once);
}
