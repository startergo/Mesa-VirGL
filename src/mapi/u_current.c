/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 1999-2008  Brian Paul   All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */


/*
 * This file manages the OpenGL API dispatch layer.
 * The dispatch table (struct _glapi_table) is basically just a list
 * of function pointers.
 * There are functions to set/get the current dispatch table for the
 * current thread and to manage registration/dispatch of dynamically
 * added extension functions.
 *
 * It's intended that this file and the other glapi*.[ch] files are
 * flexible enough to be reused in several places:  XFree86, DRI-
 * based libGL.so, and perhaps the SGI SI.
 *
 * NOTE: There are no dependencies on Mesa in this code.
 *
 * Versions (API changes):
 *   2000/02/23  - original version for Mesa 3.3 and XFree86 4.0
 *   2001/01/16  - added dispatch override feature for Mesa 3.5
 *   2002/06/28  - added _glapi_set_warning_func(), Mesa 4.1.
 *   2002/10/01  - _glapi_get_proc_address() will now generate new entrypoints
 *                 itself (using offset ~0).  _glapi_add_entrypoint() can be
 *                 called afterward and it'll fill in the correct dispatch
 *                 offset.  This allows DRI libGL to avoid probing for DRI
 *                 drivers!  No changes to the public glapi interface.
 */

#include "c11/threads.h"
#include "util/u_thread.h"
#include "u_current.h"

#ifdef MAPI_PTHREAD_TSD
#include <pthread.h>
#include "util/simple_mtx.h"
#endif

#ifndef MAPI_MODE_UTIL

#include "table.h"
#include "stub.h"

#else

extern void init_glapi_relocs_once(void);
extern void (*__glapi_noop_table[])(void);

#define table_noop_array __glapi_noop_table
#define stub_init_once() init_glapi_relocs_once()

#endif

#ifdef MAPI_PTHREAD_TSD
/* ===================================================================
 * PTHREAD-TSD RESTORATION (2026-08-19) — the pre-2015 upstream design
 * (u_current.c before "THREADS was always defined, remove it"), ported
 * to the modern symbol names. Apple clang REFUSES TLS for
 * x86_64-apple-macos10.6 even with -femulated-tls, so the gate above
 * makes _glapi_tls_* plain globals — and Gecko runs GL on two threads
 * (main chrome context + compositor thread), stomping each other's
 * current-context/dispatch on every MakeCurrent. The 2026-08-19 fence-
 * era error census is that stomp's object-table signature
 * (uLayerRects/uLayerTransform uniform mismatches, non-existent
 * texture 2, target mismatches).
 *
 * Historical shape, preserved deliberately:
 *  - single-threaded fast path = PLAIN globals (_glapi_Dispatch /
 *    _glapi_Context) — no pthread_getspecific on the hot path while
 *    one thread holds GL;
 *  - ThreadSafe flips 0→1 forever the moment a MakeCurrent arrives
 *    from a DIFFERENT thread; from then on storage is per-thread TSD;
 *  - the thread check runs from the set entry points (the "call this
 *    periodically from MakeCurrent" contract), under one mutex;
 *  - keys are created once (pthread_once) with a destructor slot.
 * The dead _glapi_tls_* globals stay defined for ABI compatibility
 * (glapi-symbols.txt lists them optional; nothing reads them here).
 * =================================================================== */

/* dead ABI symbols — see block comment */
struct _glapi_table *_glapi_tls_Dispatch;
void *_glapi_tls_Context;

/* single-threaded fast-path storage (the same names the non-TSD build
 * keeps as dead compat symbols — here they are the real globals) */
const struct _glapi_table *_glapi_Dispatch;
const void *_glapi_Context;

static pthread_key_t tsd_dispatch_key, tsd_context_key;
static int tsd_keys_ready;
static pthread_once_t tsd_once = PTHREAD_ONCE_INIT;

static void
tsd_keys_create(void)
{
   pthread_key_create(&tsd_dispatch_key, NULL);
   pthread_key_create(&tsd_context_key, NULL);
   tsd_keys_ready = 1;
}

static simple_mtx_t ThreadCheckMutex = SIMPLE_MTX_INITIALIZER;
static int ThreadSafe;            /* 0→1, never back */
static pthread_t KnownThread;
static int FirstThreadCheck = 1;

/* The historical u_current_init(): flip to per-thread storage when a
 * second thread touches GL. Called from both set entry points. */
static void
u_current_thread_check(void)
{
   if (ThreadSafe)
      return;

   simple_mtx_lock(&ThreadCheckMutex);
   if (!tsd_keys_ready)
      pthread_once(&tsd_once, tsd_keys_create);
   if (FirstThreadCheck) {
      KnownThread = pthread_self();
      FirstThreadCheck = 0;
   } else if (!pthread_equal(KnownThread, pthread_self())) {
      ThreadSafe = 1;
      /* Historical hygiene: clear the single-threaded globals so no
       * stale table can be read after the flip. */
      _glapi_Dispatch = NULL;
      _glapi_Context = NULL;
      pthread_setspecific(tsd_dispatch_key, (void *) table_noop_array);
      pthread_setspecific(tsd_context_key, NULL);
   }
   simple_mtx_unlock(&ThreadCheckMutex);
}

void
_glapi_destroy_multithread(void)
{
   if (tsd_keys_ready) {
      pthread_key_delete(tsd_dispatch_key);
      pthread_key_delete(tsd_context_key);
      tsd_keys_ready = 0;
   }
}

void
_glapi_check_multithread(void)
{
   u_current_thread_check();
}

void
_glapi_set_context(void *ptr)
{
   u_current_thread_check();
   if (tsd_keys_ready)
      pthread_setspecific(tsd_context_key, ptr);
   if (!ThreadSafe)
      _glapi_Context = ptr;
}

void *
_glapi_get_context(void)
{
   if (ThreadSafe)
      return tsd_keys_ready ? pthread_getspecific(tsd_context_key) : NULL;
   return (void *) _glapi_Context;
}

void
_glapi_set_dispatch(struct _glapi_table *tbl)
{
   stub_init_once();
   u_current_thread_check();

   if (!tbl)
      tbl = (struct _glapi_table *) table_noop_array;

   if (tsd_keys_ready)
      pthread_setspecific(tsd_dispatch_key, tbl);
   if (!ThreadSafe)
      _glapi_Dispatch = tbl;
}

struct _glapi_table *
_glapi_get_dispatch(void)
{
   if (ThreadSafe)
      return tsd_keys_ready
         ? (struct _glapi_table *) pthread_getspecific(tsd_dispatch_key)
         : NULL;
   return (struct _glapi_table *) _glapi_Dispatch;
}

#else /* !MAPI_PTHREAD_TSD — upstream storage, untouched */

/**
 * \name Current dispatch and current context control variables
 *
 * Depending on whether or not multithreading is support, and the type of
 * support available, several variables are used to store the current context
 * pointer and the current dispatch table pointer. In the non-threaded case,
 * the variables \c _glapi_Dispatch and \c _glapi_Context are used for this
 * purpose.
 *
 * In multi threaded case, The TLS variables \c _glapi_tls_Dispatch and
 * \c _glapi_tls_Context are used. Having \c _glapi_Dispatch and \c _glapi_Context
 * be hardcoded to \c NULL maintains binary compatability between TLS enabled
 * loaders and non-TLS DRI drivers. When \c _glapi_Dispatch and \c _glapi_Context
 * are \c NULL, the thread state data \c ContextTSD are used. Drivers and the
 * static dispatch functions access these variables via \c _glapi_get_dispatch
 * and \c _glapi_get_context.
 */
/*@{*/

__THREAD_INITIAL_EXEC struct _glapi_table *_glapi_tls_Dispatch
   = (struct _glapi_table *) table_noop_array;

__THREAD_INITIAL_EXEC void *_glapi_tls_Context;

/* not used, but defined for compatibility */
const struct _glapi_table *_glapi_Dispatch;
const void *_glapi_Context;

/*@}*/

/* not used, but defined for compatibility */
void
_glapi_destroy_multithread(void)
{
}

/* not used, but defined for compatibility */
void
_glapi_check_multithread(void)
{
}

/**
 * Set the current context pointer for this thread.
 * The context pointer is an opaque type which should be cast to
 * void from the real context pointer type.
 */
void
_glapi_set_context(void *ptr)
{
   _glapi_tls_Context = ptr;
}

/**
 * Get the current context pointer for this thread.
 * The context pointer is an opaque type which should be cast from
 * void to the real context pointer type.
 */
void *
_glapi_get_context(void)
{
   return _glapi_tls_Context;
}

/**
 * Set the global or per-thread dispatch table pointer.
 * If the dispatch parameter is NULL we'll plug in the no-op dispatch
 * table (__glapi_noop_table).
 */
void
_glapi_set_dispatch(struct _glapi_table *tbl)
{
   stub_init_once();

   if (!tbl)
      tbl = (struct _glapi_table *) table_noop_array;

   _glapi_tls_Dispatch = tbl;
}

/**
 * Return pointer to current dispatch table for calling thread.
 */
struct _glapi_table *
_glapi_get_dispatch(void)
{
   return _glapi_tls_Dispatch;
}

#endif /* MAPI_PTHREAD_TSD */
