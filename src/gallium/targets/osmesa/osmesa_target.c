/*
 * Copyright (c) 2013  Brian Paul   All Rights Reserved.
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


#include "target-helpers/inline_sw_helper.h"
#include "target-helpers/inline_debug_helper.h"

#include "sw/null/null_sw_winsys.h"

#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_PTHREAD
#include <pthread.h>
#endif

/* same gate as osmesa.c's
 * osmesa_dbg_thread() — pthread.h only under HAVE_PTHREAD, so this file
 * still builds where pthread headers do not exist. */
static const void *
osmesa_target_dbg_thread(void)
{
#ifdef HAVE_PTHREAD
   return (const void *)(size_t)pthread_self();
#else
   return NULL;
#endif
}

/* Diagnostic instrumentation gate (same pattern as virgl_iokit_winsys.c's
 * VIRGL_IOKIT_DEBUG and cgl_shim.mm's SHIM_TIMING). Set OSMESA_TARGET_DEBUG=1
 * in the guest environment to see osmesa_create_screen entry/exit. Off by
 * default. These probes are how the get_caps capset_id bug surfaced — the
 * entry/return pair showed sw_screen_create returning the screen while
 * glGetString was still returning plain "virgl", narrowing the fault to
 * caps parsing rather than screen creation. */
static int
osmesa_target_debug(void) {
   static int g_debug = -1;
   if (g_debug == -1)
      g_debug = getenv("OSMESA_TARGET_DEBUG") != NULL;
   return g_debug;
}
#define osmesa_dbg(...) do { if (osmesa_target_debug()) fprintf(stderr, __VA_ARGS__); } while (0)


struct pipe_screen *
osmesa_create_screen(void);


struct pipe_screen *
osmesa_create_screen(void)
{
   struct sw_winsys *winsys;
   struct pipe_screen *screen;

   osmesa_dbg("osmesa_target: osmesa_create_screen entry GALLIUM_DRIVER=\"%s\" thread=%p\n",
              getenv("GALLIUM_DRIVER") ?: "(unset)", osmesa_target_dbg_thread());

   /* We use a null software winsys since we always just render to ordinary
    * driver resources.
    */
   winsys = null_sw_create();
   if (!winsys) {
      /* iokit_dbg-style gate — this fired on every
       * OSMesaCreateContext of a misconfigured guest (GALLIUM_DRIVER unset
       * or wrong), with no way to silence it. OSMESA_TARGET_DEBUG gates the
       * other osmesa_target diagnostics already. */
      osmesa_dbg("osmesa_target: null_sw_create FAILED\n");
      return NULL;
   }

   /* Create llvmpipe or softpipe screen */
   screen = sw_screen_create(winsys);
   osmesa_dbg("osmesa_target: sw_screen_create returned %p\n", (void*)screen);
   if (!screen) {
      winsys->destroy(winsys);
      return NULL;
   }

   /* Inject optional trace, debug, etc. wrappers */
   return debug_screen_wrap(screen);
}
