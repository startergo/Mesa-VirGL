/*
 * Copyright 2026 startergo.  Derived from virgl_vtest_winsys.c (Red Hat, 2014).
 *
 * virgl_iokit_winsys — IOKit-backed virgl winsys for macOS 10.6 guest
 * under UTM.
 *
 * Replaces vtest's Unix-socket transport with IOConnectCallMethod against
 * the VMQemuVGA kext's VMVirtIOGPUUserClient.  The kext selectors are
 * 0x6000-0x6009 (see FB/VMVirtIOGPU.cpp externalMethod switch).
 *
 * Transport is proven end-to-end via:
 *   - probeTransport3D (kext-internal, LEDGER.md:202)
 *   - probeAttachBackingUser (selector 0x5000, LEDGER commit 0455ac9)
 *   - probe_winsys_selectors_test (all 0x6000-0x6009 selectors,
 *     LEDGER commit 6d9a278)
 *
 * The kext owns resource_id and context_id allocators (LEDGER.md: kext-
 * allocated IDs).  resource_create receives its ID from the kext's 0x6002
 * scalar output — the winsys never picks IDs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>

/* Diagnostic instrumentation gate. Set VIRGL_IOKIT_DEBUG=1 in the guest
 * environment to enable per-call fprintfs (submit_cmd cdw, transfer_put/get
 * box dims, winsys_wrap ENTRY). Off by default so steady-state logs stay
 * clean. Matches the SHIM_TIMING pattern in cgl_shim.mm. Keep the gating
 * rather than the unconditional prints — these call counts are how the
 * "5 submitCommand calls per frame" and "2 redundant transfer_get per
 * frame" findings were established, and they're needed again for the
 * IOSleep poll-loop investigation. */
static int
virgl_iokit_debug(void) {
   static int g_debug = -1;
   if (g_debug == -1)
      g_debug = getenv("VIRGL_IOKIT_DEBUG") != NULL;
   return g_debug;
}
#define iokit_dbg(...) do { if (virgl_iokit_debug()) fprintf(stderr, __VA_ARGS__); } while (0)

#include "util/macros.h"
#include "util/u_memory.h"
#include "util/u_inlines.h"
#include "util/format/u_format.h" /* util_format_get_stride */
#include "frontend/sw_winsys.h"

#include "virgl_iokit_winsys.h"
#include "virgl_iokit_public.h"
#include "virgl/virgl_encode.h"   /* pipe_to_virgl_format */

/* ===================================================================
 * IOKit selector numbers — must match FB/VMVirtIOGPU.cpp externalMethod
 * =================================================================== */
#define IOKIT_CREATE_CTX           0x6000
#define IOKIT_DESTROY_CTX          0x6001
#define IOKIT_CREATE_RES_3D        0x6002
#define IOKIT_ATTACH_BACKING       0x6003
#define IOKIT_DETACH_BACKING       0x6004
#define IOKIT_RESOURCE_UNREF       0x6005
#define IOKIT_GET_CAPSET_INFO      0x6006
#define IOKIT_GET_CAPSET           0x6007
#define IOKIT_SUBMIT_CMDS          0x6008
#define IOKIT_CTX_ATTACH_RESOURCE  0x6009
#define IOKIT_TRANSFER_TO_HOST_3D  0x3008
#define IOKIT_TRANSFER_FROM_3D     0x3009

/* ===================================================================
 * Resource lifecycle
 * =================================================================== */

static void
virgl_iokit_hw_res_destroy(struct virgl_iokit_winsys *iws,
                           struct virgl_hw_res *res)
{
   /* 0x6005 resourceUnref — kext sends RESOURCE_UNREF and also detaches
    * backing if still held (defensive cleanup). */
   uint64_t in[1] = { res->res_handle };
   IOConnectCallMethod(iws->connect, IOKIT_RESOURCE_UNREF,
                       in, 1, NULL, 0, NULL, NULL, NULL, NULL);

   if (res->ptr)
      align_free(res->ptr);
   FREE(res);
}

static void
virgl_iokit_resource_reference(struct virgl_winsys *vws,
                               struct virgl_hw_res **dres,
                               struct virgl_hw_res *sres)
{
   struct virgl_iokit_winsys *iws = virgl_iokit_winsys(vws);
   struct virgl_hw_res *old = *dres;

   if (pipe_reference(&(*dres)->reference, &sres->reference))
      virgl_iokit_hw_res_destroy(iws, old);

   *dres = sres;
}

/* ===================================================================
 * Resource create — malloc backing + 0x6002 + 0x6003 + 0x6009
 * =================================================================== */

static struct virgl_hw_res *
virgl_iokit_resource_create(struct virgl_winsys *vws,
                            enum pipe_texture_target target,
                            const void *map_front_private,
                            uint32_t format,
                            uint32_t bind,
                            uint32_t width,
                            uint32_t height,
                            uint32_t depth,
                            uint32_t array_size,
                            uint32_t last_level,
                            uint32_t nr_samples,
                            uint32_t flags,
                            uint32_t size)
{
   struct virgl_iokit_winsys *iws = virgl_iokit_winsys(vws);
   struct virgl_hw_res *res;
   kern_return_t kr;

   res = CALLOC_STRUCT(virgl_hw_res);
   if (!res)
      return NULL;

   /* malloc backing — vtest model, proven by ATTACH_BACKING probe.
    * align_malloc(size, 64) produces unaligned, multi-segment scatter
    * lists (the real-world case for Mesa's resource allocation). */
   if (size > 0) {
      res->ptr = align_malloc(size, 64);
      if (!res->ptr) {
         FREE(res);
         return NULL;
      }
   }
   res->size = size;
   res->bind = bind;
   res->format = format;
   res->width = width;
   res->height = height;

   /* 0x6002 createResource3DEx — kext allocates resource_id.
    * Format is converted from pipe_format to virgl_formats here. */
   {
      uint32_t virgl_fmt = pipe_to_virgl_format(format);
      uint64_t in[11] = {
         iws->ctx_id,             /* ctx_id (probeTransport3D sets hdr.ctx_id explicitly) */
         (uint64_t)target,        /* target */
         (uint64_t)virgl_fmt,     /* format */
         (uint64_t)bind,          /* bind */
         (uint64_t)width,
         (uint64_t)height,
         (uint64_t)depth,
         (uint64_t)array_size,
         (uint64_t)last_level,
         (uint64_t)nr_samples,
         (uint64_t)flags,
      };
      uint64_t out[1] = { 0 };
      uint32_t out_cnt = 1;
      kr = IOConnectCallMethod(iws->connect, IOKIT_CREATE_RES_3D,
                               in, 11, NULL, 0,
                               out, &out_cnt, NULL, NULL);
      if (kr != KERN_SUCCESS) {
         fprintf(stderr, "virgl_iokit: 0x6002 createResource3DEx FAIL 0x%x\n", kr);
         align_free(res->ptr);
         FREE(res);
         return NULL;
      }
      res->res_handle = (uint32_t)out[0];
   }

   /* 0x6003 attachBackingUser — wires the malloc'd pages into the device.
    * Kext holds the descriptor prepared for the resource's lifetime
    * (constraint 3, LEDGER.md:799). */
   if (size > 0) {
      uint64_t addr = (uint64_t)(uintptr_t)res->ptr;
      uint64_t len  = size;
      uint64_t ab_in[5] = {
         res->res_handle,
         (uint32_t)(addr & 0xFFFFFFFFull),
         (uint32_t)(addr >> 32),
         (uint32_t)(len  & 0xFFFFFFFFull),
         (uint32_t)(len  >> 32),
      };
      kr = IOConnectCallMethod(iws->connect, IOKIT_ATTACH_BACKING,
                               ab_in, 5, NULL, 0,
                               NULL, NULL, NULL, NULL);
      if (kr != KERN_SUCCESS) {
         fprintf(stderr, "virgl_iokit: 0x6003 attachBackingUser FAIL 0x%x\n", kr);
         /* Best-effort cleanup: unref the resource we just created */
         uint64_t unref_in[1] = { res->res_handle };
         IOConnectCallMethod(iws->connect, IOKIT_RESOURCE_UNREF,
                             unref_in, 1, NULL, 0, NULL, NULL, NULL, NULL);
         align_free(res->ptr);
         FREE(res);
         return NULL;
      }
   }

   /* 0x6009 ctxAttachResource — REQUIRED before SET_FRAMEBUFFER_STATE can
    * reference a surface built on this resource (LEDGER commit 6d9a278).
    * Unconditional for the first slice; may need gating on bind flags
    * for real Mesa workloads (LEDGER: CTX_ATTACH_RESOURCE caveat). */
   {
      uint64_t ca_in[2] = { iws->ctx_id, res->res_handle };
      kr = IOConnectCallMethod(iws->connect, IOKIT_CTX_ATTACH_RESOURCE,
                               ca_in, 2, NULL, 0,
                               NULL, NULL, NULL, NULL);
      if (kr != KERN_SUCCESS) {
         fprintf(stderr, "virgl_iokit: WARN 0x6009 ctxAttachResource FAIL 0x%x\n", kr);
         /* Non-fatal for now — matches probeTransport3D's behavior. */
      }
   }

   pipe_reference_init(&res->reference, 1);
   p_atomic_set(&res->num_cs_references, 0);
   return res;
}

/* ===================================================================
 * resource_map — return the malloc'd pointer (vtest model)
 * =================================================================== */
static void *
virgl_iokit_resource_map(struct virgl_winsys *vws, struct virgl_hw_res *res)
{
   return res->ptr;
}

/* ===================================================================
 * Transfer operations — 0x3008 / 0x3009
 * =================================================================== */

static int
virgl_iokit_transfer_put(struct virgl_winsys *vws,
                         struct virgl_hw_res *res,
                         const struct pipe_box *box,
                         uint32_t stride, uint32_t layer_stride,
                         uint32_t buf_offset, uint32_t level)
{
   struct virgl_iokit_winsys *iws = virgl_iokit_winsys(vws);
   iokit_dbg("iokit: transfer_put res=%u box=%ux%ux%u\n",
             res->res_handle, box->width, box->height, box->depth);
   uint64_t in[9] = {
      res->res_handle, level,
      box->x, box->y, box->z,
      box->width, box->height, box->depth,
      iws->ctx_id,
   };
   kern_return_t kr = IOConnectCallMethod(iws->connect, IOKIT_TRANSFER_TO_HOST_3D,
                                           in, 9, NULL, 0,
                                           NULL, NULL, NULL, NULL);
   return (kr == KERN_SUCCESS) ? 0 : -1;
}

static int
virgl_iokit_transfer_get(struct virgl_winsys *vws,
                         struct virgl_hw_res *res,
                         const struct pipe_box *box,
                         uint32_t stride, uint32_t layer_stride,
                         uint32_t buf_offset, uint32_t level)
{
   struct virgl_iokit_winsys *iws = virgl_iokit_winsys(vws);
   iokit_dbg("iokit: transfer_get res=%u box=%ux%ux%u\n",
             res->res_handle, box->width, box->height, box->depth);
   uint64_t in[9] = {
      res->res_handle, level,
      box->x, box->y, box->z,
      box->width, box->height, box->depth,
      iws->ctx_id,
   };
   kern_return_t kr = IOConnectCallMethod(iws->connect, IOKIT_TRANSFER_FROM_3D,
                                           in, 9, NULL, 0,
                                           NULL, NULL, NULL, NULL);
   return (kr == KERN_SUCCESS) ? 0 : -1;
}

/* ===================================================================
 * Command buffer management — malloc/reloc tracking
 * =================================================================== */

static bool
virgl_iokit_lookup_res(struct virgl_iokit_cmd_buf *cbuf, struct virgl_hw_res *res)
{
   unsigned hash = res->res_handle % (ARRAY_SIZE(cbuf->is_handle_added));
   return cbuf->is_handle_added[hash];
}

static void
virgl_iokit_add_res(struct virgl_iokit_winsys *iws,
                    struct virgl_iokit_cmd_buf *cbuf,
                    struct virgl_hw_res *res)
{
   unsigned hash = res->res_handle % ARRAY_SIZE(cbuf->is_handle_added);
   cbuf->res_bo[cbuf->cres] = NULL;
   virgl_iokit_resource_reference(&iws->base, &cbuf->res_bo[cbuf->cres], res);
   cbuf->is_handle_added[hash] = true;
   cbuf->reloc_indices_hashlist[hash] = cbuf->cres;
   p_atomic_inc(&res->num_cs_references);
   cbuf->cres++;
}

static void
virgl_iokit_release_all_res(struct virgl_iokit_winsys *iws,
                            struct virgl_iokit_cmd_buf *cbuf)
{
   for (unsigned i = 0; i < cbuf->cres; i++) {
      if (cbuf->res_bo[i])
         virgl_iokit_resource_reference(&iws->base, &cbuf->res_bo[i], NULL);
   }
   cbuf->cres = 0;
}

static struct virgl_cmd_buf *
virgl_iokit_cmd_buf_create(struct virgl_winsys *vws, uint32_t size)
{
   struct virgl_iokit_cmd_buf *cbuf = CALLOC_STRUCT(virgl_iokit_cmd_buf);
   if (!cbuf)
      return NULL;

   cbuf->nres = 512;
   cbuf->res_bo = CALLOC(cbuf->nres, sizeof(struct virgl_hw_res *));
   if (!cbuf->res_bo) { FREE(cbuf); return NULL; }

   cbuf->buf = CALLOC(size, sizeof(uint32_t));
   if (!cbuf->buf) { FREE(cbuf->res_bo); FREE(cbuf); return NULL; }

   cbuf->ws = vws;
   cbuf->base.buf = cbuf->buf;
   return &cbuf->base;
}

static void
virgl_iokit_cmd_buf_destroy(struct virgl_cmd_buf *_cbuf)
{
   struct virgl_iokit_cmd_buf *cbuf = virgl_iokit_cmd_buf(_cbuf);
   virgl_iokit_release_all_res(virgl_iokit_winsys(cbuf->ws), cbuf);
   FREE(cbuf->res_bo);
   FREE(cbuf->buf);
   FREE(cbuf);
}

static void
virgl_iokit_emit_res(struct virgl_winsys *vws,
                     struct virgl_cmd_buf *_cbuf,
                     struct virgl_hw_res *res, bool write_buf)
{
   struct virgl_iokit_winsys *iws = virgl_iokit_winsys(vws);
   struct virgl_iokit_cmd_buf *cbuf = virgl_iokit_cmd_buf(_cbuf);
   bool already = virgl_iokit_lookup_res(cbuf, res);
   if (write_buf)
      cbuf->base.buf[cbuf->base.cdw++] = res->res_handle;
   if (!already)
      virgl_iokit_add_res(iws, cbuf, res);
}

static bool
virgl_iokit_res_is_ref(struct virgl_winsys *vws,
                       struct virgl_cmd_buf *_cbuf,
                       struct virgl_hw_res *res)
{
   return p_atomic_read(&res->num_cs_references) != 0;
}

/* ===================================================================
 * submit_cmd — 0x6008 submitVirglCommandsEx
 *
 * *** 0x1100 means "QEMU parsed it", never "host accepted it" ***
 * (LEDGER.md: 0x1100 rule).  Only readback proves acceptance.
 * =================================================================== */
static int
virgl_iokit_submit_cmd(struct virgl_winsys *vws,
                       struct virgl_cmd_buf *_cbuf,
                       struct pipe_fence_handle **fence)
{
   struct virgl_iokit_winsys *iws = virgl_iokit_winsys(vws);
   struct virgl_iokit_cmd_buf *cbuf = virgl_iokit_cmd_buf(_cbuf);
   kern_return_t kr;

   if (cbuf->base.cdw == 0)
      return 0;

   iokit_dbg("iokit: submit_cmd cdw=%u\n", cbuf->base.cdw);

   uint64_t ctx_scalar = iws->ctx_id;
   kr = IOConnectCallMethod(iws->connect, IOKIT_SUBMIT_CMDS,
                            &ctx_scalar, 1,
                            cbuf->base.buf, cbuf->base.cdw * sizeof(uint32_t),
                            NULL, NULL, NULL, NULL);
   if (kr != KERN_SUCCESS) {
      fprintf(stderr, "virgl_iokit: 0x6008 submitVirglCommandsEx FAIL 0x%x\n", kr);
   }

   virgl_iokit_release_all_res(iws, cbuf);
   memset(cbuf->is_handle_added, 0, sizeof(cbuf->is_handle_added));
   cbuf->base.cdw = 0;
   return (kr == KERN_SUCCESS) ? 0 : -1;
}

/* ===================================================================
 * get_caps — 0x6006 + 0x6007 (real GET_CAPSET, not hardcoded sizes)
 * Mesa parses the blob's CONTENTS to decide GL version/features.
 * =================================================================== */
static int
virgl_iokit_get_caps(struct virgl_winsys *vws, struct virgl_drm_caps *caps)
{
   struct virgl_iokit_winsys *iws = virgl_iokit_winsys(vws);
   virgl_ws_fill_new_caps_defaults(caps);

   /* Match the drm winsys pattern (virgl_drm_winsys.c:1011): try capset_id=2
    * (VIRGL2, v2 layout, 1408 B — has host_feature_check_version, renderer,
    * capability_bits_v2) first, fall back to capset_id=1 (VIRGL, v1, 308 B).
    *
    * The v2 capset is what Mesa actually expects — virgl_get_name reads
    * caps.v2.renderer when host_feature_check_version >= 5, virgl_can_use_staging
    * reads caps.v2.capability_bits_v2, etc. Querying by capset_id (not by
    * capset_index) avoids depending on the host's index ordering.
    *
    * Prior version iterated indices 0..1 and used whatever idx=0 returned,
    * which on this device is VIRGL (v1). That left every v2 field at the
    * fill_new_caps_defaults value (host_feature_check_version=0, renderer=""),
    * so glGetString(GL_RENDERER) returned plain "virgl" and the driver
    * believed the host supports less than it does. */
   for (uint32_t want_id = 2; want_id >= 1; want_id--) {
      for (uint32_t idx = 0; idx < 2; idx++) {
         uint64_t in[1] = { idx };
         uint64_t out[3] = { 0, 0, 0 };
         uint32_t out_cnt = 3;
         kern_return_t kr = IOConnectCallMethod(iws->connect, IOKIT_GET_CAPSET_INFO,
                                                 in, 1, NULL, 0,
                                                 out, &out_cnt, NULL, NULL);
         if (kr != KERN_SUCCESS)
            continue;

         uint32_t capset_id = out[0], capset_ver = out[1], capset_size = out[2];
         if (capset_size == 0 || capset_size > 2048)
            continue;
         if (capset_id != want_id)
            continue;

         uint8_t blob[2048];
         size_t blob_sz = capset_size;
         uint64_t cap_in[2] = { capset_id, capset_ver };
         kr = IOConnectCallMethod(iws->connect, IOKIT_GET_CAPSET,
                                  cap_in, 2, NULL, 0,
                                  NULL, NULL, blob, &blob_sz);
         if (kr != KERN_SUCCESS)
            continue;

         uint32_t copy = MIN2((uint32_t)blob_sz, (uint32_t)sizeof(union virgl_caps));
         memcpy(&caps->caps, blob, copy);
         fprintf(stderr, "virgl_iokit: get_caps capset_id=%u ver=%u size=%u copy=%u\n",
                 capset_id, capset_ver, (uint32_t)blob_sz, copy);
         return 0;
      }
   }

   fprintf(stderr, "virgl_iokit: get_caps — no capset returned by host\n");
   return -1;
}

/* ===================================================================
 * Stubs / trivial — see LEDGER vtable scope classification
 * =================================================================== */
static int  virgl_iokit_get_fd(struct virgl_winsys *vws) { return -1; }
static void virgl_iokit_resource_wait(struct virgl_winsys *vws, struct virgl_hw_res *res) { /* sync submit */ }
static bool virgl_iokit_resource_is_busy(struct virgl_winsys *vws, struct virgl_hw_res *res) { return false; }
static uint32_t virgl_iokit_resource_get_storage_size(struct virgl_winsys *vws, struct virgl_hw_res *res) { return res->size; }

/* Fence stubs — valid only while submit_cmd is synchronous (LEDGER.md) */
static struct pipe_fence_handle *virgl_iokit_cs_create_fence(struct virgl_winsys *vws, int fd) { return NULL; }
static bool virgl_iokit_fence_wait(struct virgl_winsys *vws, struct pipe_fence_handle *f, uint64_t timeout) { return true; }
static void virgl_iokit_fence_reference(struct virgl_winsys *vws, struct pipe_fence_handle **dst, struct pipe_fence_handle *src) { *dst = src; }
static int  virgl_iokit_fence_get_fd(struct virgl_winsys *vws, struct pipe_fence_handle *f) { return -1; }
static void virgl_iokit_flush_frontbuffer(struct virgl_winsys *vws, struct virgl_cmd_buf *cbuf,
                                          struct virgl_hw_res *res, unsigned level, unsigned layer,
                                          void *winsys_drawable_handle, struct pipe_box *sub_box) { }

/* ===================================================================
 * destroy
 * =================================================================== */
static void
virgl_iokit_winsys_destroy(struct virgl_winsys *vws)
{
   struct virgl_iokit_winsys *iws = virgl_iokit_winsys(vws);

   /* 0x6001 destroyVirglContextEx */
   if (iws->ctx_id) {
      uint64_t in[1] = { iws->ctx_id };
      IOConnectCallMethod(iws->connect, IOKIT_DESTROY_CTX,
                          in, 1, NULL, 0, NULL, NULL, NULL, NULL);
   }
   if (iws->connect)
      IOServiceClose(iws->connect);
   mtx_destroy(&iws->mutex);
   FREE(iws);
}

/* ===================================================================
 * Factory — virgl_iokit_winsys_wrap()
 *
 * Opens VMQemuVGAAccelerator with IOServiceOpen type=4 →
 * VMVirtIOGPUUserClient.  Creates one virgl context (0x6000).
 * Fills the 25-entry virgl_winsys vtable.
 * =================================================================== */
struct virgl_winsys *
virgl_iokit_winsys_wrap(struct sw_winsys *sws)
{
   struct virgl_iokit_winsys *iws;
   kern_return_t kr;

   iokit_dbg("virgl_iokit: winsys_wrap ENTRY sws=%p thread=%p\n",
             (void*)sws, (void*)pthread_self());

   iws = CALLOC_STRUCT(virgl_iokit_winsys);
   if (!iws)
      return NULL;

   iws->sws = sws;
   iws->connect = IO_OBJECT_NULL;
   iws->ctx_id = 0;

   /* Find VMQemuVGAAccelerator (registered, id 0x1000002b8 per ioreg).
    * NOT VMVirtIOGPUAccelerator (subclass, 0 instances). */
   CFMutableDictionaryRef matching = IOServiceMatching("VMQemuVGAAccelerator");
   if (!matching) {
      fprintf(stderr, "virgl_iokit: IOServiceMatching failed\n");
      FREE(iws);
      return NULL;
   }
   io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault, matching);
   if (service == IO_OBJECT_NULL) {
      fprintf(stderr, "virgl_iokit: VMQemuVGAAccelerator not found (kext loaded?)\n");
      FREE(iws);
      return NULL;
   }

   /* type=4 → VMVirtIOGPUUserClient (VMQemuVGAAccelerator.cpp:425) */
   kr = IOServiceOpen(service, mach_task_self(), 4, &iws->connect);
   IOObjectRelease(service);
   if (kr != KERN_SUCCESS) {
      fprintf(stderr, "virgl_iokit: IOServiceOpen type=4 fail 0x%x\n", kr);
      FREE(iws);
      return NULL;
   }

   /* 0x6000 createVirglContextEx — kext allocates ctx_id */
   {
      uint64_t out[1] = { 0 };
      uint32_t out_cnt = 1;
      kr = IOConnectCallMethod(iws->connect, IOKIT_CREATE_CTX,
                               NULL, 0, NULL, 0,
                               out, &out_cnt, NULL, NULL);
      if (kr != KERN_SUCCESS) {
         fprintf(stderr, "virgl_iokit: 0x6000 createVirglContextEx fail 0x%x\n", kr);
         IOServiceClose(iws->connect);
         FREE(iws);
         return NULL;
      }
      iws->ctx_id = (uint32_t)out[0];
   }

   (void)mtx_init(&iws->mutex, mtx_plain);

   /* Fill vtable — mirrors vtest at virgl_vtest_winsys.c:717-744 */
   iws->base.destroy                  = virgl_iokit_winsys_destroy;
   iws->base.transfer_put             = virgl_iokit_transfer_put;
   iws->base.transfer_get             = virgl_iokit_transfer_get;
   iws->base.resource_create          = virgl_iokit_resource_create;
   iws->base.resource_reference       = virgl_iokit_resource_reference;
   iws->base.resource_map             = virgl_iokit_resource_map;
   iws->base.resource_wait            = virgl_iokit_resource_wait;
   iws->base.resource_is_busy         = virgl_iokit_resource_is_busy;
   iws->base.cmd_buf_create           = virgl_iokit_cmd_buf_create;
   iws->base.cmd_buf_destroy          = virgl_iokit_cmd_buf_destroy;
   iws->base.submit_cmd               = virgl_iokit_submit_cmd;
   iws->base.emit_res                 = virgl_iokit_emit_res;
   iws->base.res_is_referenced        = virgl_iokit_res_is_ref;
   iws->base.get_caps                 = virgl_iokit_get_caps;

   /* Fence stubs — NULL where callers NULL-check, value where they don't */
   iws->base.cs_create_fence          = virgl_iokit_cs_create_fence; /* returns NULL */
   iws->base.fence_wait               = virgl_iokit_fence_wait;
   iws->base.fence_reference          = virgl_iokit_fence_reference;

   /* Data fields */
   iws->base.pci_id                   = 0;
   iws->base.supports_fences          = 0;
   /* drm and vtest both set this to 1 (upstream convention) but our iokit
    * transport diverges: with =1, virgl takes the staging transfer path
    * (encode COPY_TRANSFER into submit_cmd, flush, wait) and pixels come
    * back RGBA(0,0,0,0) — the host isn't executing the encoded transfers
    * the way virgl expects on this transport. With =0, the direct
    * transfer_get path is taken, pixels are byte-exact, and wall time
    * drops from 683 ms to 347 ms (n=13 vs n=32, consistent v2 caps both
    * sides via the get_caps capset_id=2 fix).
    *
    * The divergence is justified by correctness first (staging produces
    * no rendering here), perf second. The driver-robustness gates in
    * virgl_resource.c (virgl_can_use_staging + transfer_prepare) make
    * either value safe against the assertion; they stay regardless. */
   iws->base.supports_encoded_transfers = 0;
   iws->base.supports_coherent        = 0;

   /* Remaining stubs */
   iws->base.get_fd                   = virgl_iokit_get_fd;
   iws->base.resource_get_storage_size = virgl_iokit_resource_get_storage_size;
   iws->base.flush_frontbuffer        = virgl_iokit_flush_frontbuffer;
   iws->base.fence_get_fd             = virgl_iokit_fence_get_fd;

   /* fence_server_sync and resource_create_from_handle / resource_set_type /
    * resource_get_handle are left NULL — upstream virgl NULL-checks callers
    * (LEDGER.md:122 fence safety verification). */

   return &iws->base;
}
