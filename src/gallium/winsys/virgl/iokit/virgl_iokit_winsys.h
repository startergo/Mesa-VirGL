/*
 * Copyright 2026 startergo.  Derived from virgl_vtest_winsys.h (Red Hat, 2014).
 *
 * virgl_iokit_winsys — IOKit-backed virgl winsys for macOS 10.6 guest
 * under UTM.  Replaces vtest's Unix-socket transport with IOConnectCallMethod
 * against the VMQemuVGA kext's VMVirtIOGPUUserClient (selectors 0x6000-0x6009).
 */
#ifndef VIRGL_IOKIT_WINSYS_H
#define VIRGL_IOKIT_WINSYS_H

#include <stdint.h>
#include "util/compiler.h"
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "util/u_thread.h"

#include "virgl/virgl_winsys.h"
#include <IOKit/IOKitLib.h>

struct pipe_fence_handle;
struct sw_winsys;

struct virgl_iokit_winsys {
   struct virgl_winsys base;
   struct sw_winsys *sws;

   /* ONE io_connect_t per pipe_screen — shared by all contexts.
    * Matches vtest's single-socket model.  Opened in the factory
    * (virgl_iokit_winsys_wrap), closed in destroy(). */
   io_connect_t connect;

   /* ONE virgl context per winsys (sufficient for first slice).
    * Allocated by selector 0x6000 createVirglContextEx, used by
    * all subsequent submit_cmd (0x6008) and transfers (0x3008/0x3009). */
   uint32_t ctx_id;

   mtx_t mutex;
};

struct virgl_hw_res {
   struct pipe_reference reference;
   uint32_t res_handle;           /* from kext createResource3DEx scalar output */
   int num_cs_references;

   void *ptr;                     /* align_malloc'd backing (vtest model) */
   uint32_t size;

   uint32_t format;
   uint32_t stride;
   uint32_t width;
   uint32_t height;
   uint32_t bind;
};

struct virgl_iokit_cmd_buf {
   struct virgl_cmd_buf base;
   uint32_t *buf;
   unsigned nres;
   unsigned cres;
   struct virgl_winsys *ws;
   struct virgl_hw_res **res_bo;

   /* 2026-10-17 (winsys review, F1): set when add_res could not record a
    * resource whose handle emit_res had already written into the blob
    * (res_bo REALLOC failed). Such a batch must never be submitted: the
    * handle would not be in the FCE1 list (no kext fence for it) and no
    * cbuf reference would hold it alive — the r250 "Illegal resource"
    * class. submit_cmd drops the batch instead. */
   int poisoned;

   char     is_handle_added[512];
   unsigned reloc_indices_hashlist[512];
};

static inline struct virgl_hw_res *
virgl_hw_res(struct pipe_fence_handle *f)
{
   return (struct virgl_hw_res *)f;
}

static inline struct virgl_iokit_winsys *
virgl_iokit_winsys(struct virgl_winsys *iws)
{
   return (struct virgl_iokit_winsys *)iws;
}

static inline struct virgl_iokit_cmd_buf *
virgl_iokit_cmd_buf(struct virgl_cmd_buf *cbuf)
{
   return (struct virgl_iokit_cmd_buf *)cbuf;
}

#endif
