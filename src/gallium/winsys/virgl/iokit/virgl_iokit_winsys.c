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
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_time.h>   /* rung276: monotonic clock for the quarantine */
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
#define IOKIT_DRAIN_3D             0x600A   /* ordering barrier for the async submit queue */
#define IOKIT_TRANSFER_TO_HOST_3D  0x3008
#define IOKIT_TRANSFER_FROM_3D     0x3009

/* ===================================================================
 * Census registry (2026-08-20) — SHIM_CONTENT_CENSUS instrumentation.
 * Tracks every alive hw_res so the flush-time census can read back ALL
 * color/depth resources, not just the presented front — answers "which
 * resource holds the compositor's draws" when the front reads zero.
 * Registry mutex is held across census transfers so a concurrent
 * destroy cannot free a backing mid-readback (destroys block for the
 * census duration; debug-run serialization, accepted).
 * =================================================================== */
#define CENSUS_REG_MAX 512
struct census_entry {
   struct virgl_hw_res *res;   /* raw ptr; validity under the mutex */
   uint32_t handle, fmt, w, h, size;
   int alive;
};
static struct census_entry s_creg[CENSUS_REG_MAX];
static pthread_mutex_t s_creg_mtx = PTHREAD_MUTEX_INITIALIZER;
static int s_creg_overflow = 0;

static void census_register(struct virgl_hw_res *res)
{
   pthread_mutex_lock(&s_creg_mtx);
   for (int i = 0; i < CENSUS_REG_MAX; i++) {
      if (!s_creg[i].alive) {
         s_creg[i].res = res;
         s_creg[i].handle = res->res_handle;
         s_creg[i].fmt = res->format;
         s_creg[i].w = res->width;
         s_creg[i].h = res->height;
         s_creg[i].size = res->size;
         s_creg[i].alive = 1;
         pthread_mutex_unlock(&s_creg_mtx);
         return;
      }
   }
   if (!s_creg_overflow) {
      s_creg_overflow = 1;
      fprintf(stderr, "iokit: CENSUS registry OVERFLOW (>512 alive)\n");
   }
   pthread_mutex_unlock(&s_creg_mtx);
}

static void census_unregister(struct virgl_hw_res *res)
{
   pthread_mutex_lock(&s_creg_mtx);
   for (int i = 0; i < CENSUS_REG_MAX; i++) {
      if (s_creg[i].alive && s_creg[i].res == res) {
         s_creg[i].alive = 0;
         break;
      }
   }
   pthread_mutex_unlock(&s_creg_mtx);
}

/* ===================================================================
 * RUNG 236a — resource backings OUTSIDE the app's malloc heap.
 * Measured (VMQemuVGA LEDGER, 2026-10-11): PowerFox crashed three times in
 * SpiderMonkey string-buffer memcpy (nursery sweep / promotion / rope
 * flatten) with the kext stack loaded, never without it (user). In
 * Firefox, mozjemalloc is the default malloc zone, so align_malloc'd
 * backings shared one heap with JS string buffers: any late or stray
 * write into a backing (host DMA after free, a CPU write-back past its
 * end) lands in JS data. Backings now come from mmap (page-granular,
 * their own VM regions) and, on release, are made PROT_NONE and kept in
 * a quarantine ring before munmap:
 *   - a CPU write into a released backing faults AT THE WRITER;
 *   - a host write cannot reach the app's heap any more.
 * Env: VIRGL_IOKIT_HEAPBACK=1 restores align_malloc;
 *      VIRGL_IOKIT_QUARANTINE=<n> ring size (default 64, 0 = munmap now);
 *      quarantined bytes capped at 64 MB (oldest unmapped first).
 * =================================================================== */
#define BK_QMAX 256
static struct { void *p; size_t len; uint64_t ms; } s_bk_q[BK_QMAX];
static unsigned s_bk_qhead, s_bk_qn;
static pthread_mutex_t s_bk_mtx = PTHREAD_MUTEX_INITIALIZER;
static int s_bk_heap = -1, s_bk_qcap = -1, s_bk_hold = -1;
static unsigned long s_bk_alloc_n, s_bk_free_n, s_bk_young_holds, s_bk_young_forced;
static size_t s_bk_qbytes;
#define BK_QBYTES_MAX (64u << 20)   /* quarantined pages stay resident: cap at 64 MB */

/* F2 (2026-10-17 winsys review): how long a quarantined backing must age
 * before it may be munmapped. The kext defers the release of a user
 * backing vmq_r270 ms (default 2000) AFTER the synchronous UNREF — the
 * pages stay wired and canary-filled (r271) for that long. The measured
 * free rate is ~59/s (6,656 UNREFs in 112 s, LEDGER rung 254), so the old
 * 64-entry ring turned over in ~1.08 s and munmapped every backing while
 * the kext still held it. Not a corruption path (the wired pages cannot
 * be reused until the kext completes them; r271 canary: 0 hits), but it
 * halves the PROT_NONE window that makes a stray CPU write fault at the
 * writer and decouples the winsys quarantine from the kext contract.
 * Entries younger than the hold are kept even past the soft cap; the
 * hard cap BK_QMAX still forces eviction (logged). */
static uint64_t bk_now_ms(void)
{
   /* rung276 (PR review t3cau): MONOTONIC. gettimeofday() follows the
    * wall clock, and in this VM the wall clock steps backward (NTP at
    * boot, host suspend/resume). A backward step makes
    * (now - s_bk_q[k].ms) wrap to ~2^64 ms and every entry read as
    * older-than-hold — the eviction loop would munmap backings the kext
    * still holds inside its r270 deferred release, the exact hazard the
    * hold exists to prevent. mach_absolute_time never moves backward
    * (Apple: "monotonic ... unaffected by wall clock adjustments"), so
    * only the elapsed time is compared; the epoch is irrelevant. */
   static mach_timebase_info_data_t s_tbi;
   static int s_have_tbi = 0;
   if (!s_have_tbi) {
      if (mach_timebase_info(&s_tbi) != KERN_SUCCESS) {
         /* x86_64 macOS timebase is 1/1; a failure leaves abs == ns. */
         s_tbi.numer = 1;
         s_tbi.denom = 1;
      }
      s_have_tbi = 1;
   }
   uint64_t abs = mach_absolute_time();
   return (uint64_t)((double)abs * (double)s_tbi.numer /
                     (double)s_tbi.denom / 1000000.0);
}

static void bk_config(void)
{
   if (s_bk_heap >= 0) return;
   const char *e = getenv("VIRGL_IOKIT_HEAPBACK");
   s_bk_heap = (e && *e == '1') ? 1 : 0;
   e = getenv("VIRGL_IOKIT_QUARANTINE");
   s_bk_qcap = e ? atoi(e) : 64;
   if (s_bk_qcap < 0) s_bk_qcap = 0;
   if (s_bk_qcap > BK_QMAX) s_bk_qcap = BK_QMAX;
   e = getenv("VIRGL_IOKIT_QHOLD");
   s_bk_hold = e ? atoi(e) : 2500;   /* r270 default 2000 ms + slack */
   if (s_bk_hold < 0) s_bk_hold = 0;
   fprintf(stderr, "virgl_iokit: rung236a backings %s, quarantine %d, hold %d ms\n",
           s_bk_heap ? "align_malloc (heap)" : "mmap (off-heap)", s_bk_qcap, s_bk_hold);
}

static size_t bk_len(size_t size) { return (size + 4095) & ~(size_t)4095; }

/* RUNG 259 instrument (VMQemuVGA LEDGER 2026-10-13): PowerFox pid 1318
 * crashed reading the first byte of an unreadable page (cr2 0x12a8dd000)
 * inside a JS module's source text. The only code of ours that makes pages
 * unreadable is this file's backing quarantine (PROT_NONE, then munmap).
 * Armed by /var/db/vmgld_r259_on (checked once): every backing map,
 * protect and unmap is appended UNBUFFERED (one write(2) per line) to
 * /tmp/virgl_iokit_bk.<pid>.log as "<seq> <sec.usec> <op> <start> <end>",
 * op = MAP / PROT / UNMAP. After a crash: is cr2 inside a range we touched,
 * and when? */
static int s_r259 = -1, s_r259_fd = -1;
static unsigned long s_r259_seq;
static void r259_log(const char *op, void *p, size_t len)
{
   if (s_r259 < 0) {
      s_r259 = access("/var/db/vmgld_r259_on", F_OK) == 0;
      if (s_r259) {
         char path[64]; snprintf(path, sizeof path, "/tmp/virgl_iokit_bk.%d.log", (int)getpid());
         s_r259_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
         if (s_r259_fd < 0) s_r259 = 0;
         else fprintf(stderr, "virgl_iokit: rung259 backing map log -> %s\n", path);
      }
   }
   if (!s_r259) return;
   struct timeval tv; gettimeofday(&tv, NULL);
   char b[128];
   int n = snprintf(b, sizeof b, "%lu %ld.%06ld %s %p %p\n", __sync_add_and_fetch(&s_r259_seq, 1),
                    (long)tv.tv_sec, (long)tv.tv_usec, op, p, (void *)((char *)p + len));
   if (n > 0) (void)!write(s_r259_fd, b, (size_t)n);
}

static void *bk_alloc(size_t size)
{
   bk_config();
   if (s_bk_heap) return align_malloc(size, 64);
   void *p = mmap(NULL, bk_len(size), PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
   if (p == MAP_FAILED) return NULL;
   r259_log("MAP", p, bk_len(size));   /* RUNG 259 */
   s_bk_alloc_n++;
   return p;
}

static void bk_free(void *p, size_t size)
{
   if (!p) return;
   bk_config();
   if (s_bk_heap) { align_free(p); return; }
   size_t len = bk_len(size);
   if (s_bk_qcap == 0) { r259_log("UNMAP", p, len); munmap(p, len); return; }
   r259_log("PROT", p, len);   /* RUNG 259 */
   mprotect(p, len, PROT_NONE);
   pthread_mutex_lock(&s_bk_mtx);
   {
      uint64_t now = bk_now_ms();
      while (s_bk_qn > 0 && (s_bk_qn >= (unsigned)s_bk_qcap || s_bk_qbytes + len > BK_QBYTES_MAX)) {   /* evict oldest */
         unsigned k = (s_bk_qhead + BK_QMAX - s_bk_qn) % BK_QMAX;
         if ((uint64_t)(now - s_bk_q[k].ms) < (uint64_t)s_bk_hold) {
            if (s_bk_qn < BK_QMAX) {   /* F2: keep young entries past the soft cap */
               s_bk_young_holds++;
               if (s_bk_young_holds <= 4 || (s_bk_young_holds & 1023) == 0)
                  fprintf(stderr, "virgl_iokit: rung236a quarantine holds young entries "
                          "(ring %u/%d soft, %u hard, %.1f MB; hold #%lu)\n",
                          s_bk_qn, s_bk_qcap, BK_QMAX, (double)(s_bk_qbytes + len) / 1048576.0,
                          s_bk_young_holds);
               break;
            }
            s_bk_young_forced++;   /* hard cap: munmap anyway, say so */
            if (s_bk_young_forced <= 4 || (s_bk_young_forced & 1023) == 0)
               fprintf(stderr, "virgl_iokit: rung236a quarantine HARD CAP — munmapping a "
                       "%llu ms-old entry before the kext's hold (#%lu)\n",
                       (unsigned long long)(now - s_bk_q[k].ms), s_bk_young_forced);
         }
         r259_log("UNMAP", s_bk_q[k].p, s_bk_q[k].len);   /* RUNG 259 */
         munmap(s_bk_q[k].p, s_bk_q[k].len);
         s_bk_qbytes -= s_bk_q[k].len;
         s_bk_qn--;
      }
   }
   s_bk_q[s_bk_qhead].p = p; s_bk_q[s_bk_qhead].len = len;
   s_bk_q[s_bk_qhead].ms = bk_now_ms();
   s_bk_qhead = (s_bk_qhead + 1) % BK_QMAX; s_bk_qn++;
   s_bk_qbytes += len;
   s_bk_free_n++;
   if (s_bk_free_n <= 4 || (s_bk_free_n & 1023) == 0)
      fprintf(stderr, "virgl_iokit: rung236a released backing %p len %zu -> PROT_NONE quarantine (%lu allocs, %lu frees)\n",
              p, len, s_bk_alloc_n, s_bk_free_n);
   pthread_mutex_unlock(&s_bk_mtx);
}

/* ===================================================================
 * Resource lifecycle
 * =================================================================== */

/* RUNG 254 (VMQemuVGA LEDGER 2026-10-13): the per-buffer lifecycle lines
 * (CREATE / 0x6009 attach / UNREF, rung 68) went to stderr, and a GUI app's
 * stderr travels launchd -> syslogd -> system.log. Measured: ~178 lines/s
 * from PowerFox (19,958 lines in 112 s, all three kinds ~6,650 each);
 * guest top: syslogd ~11% + launchd x2 ~13% CPU. Here they go to a private,
 * fully-buffered file instead, /tmp/virgl_iokit.<pid>.log (64 KB buffer,
 * flushed when full, so the newest lines may lag in a live read).
 * VIRGL_IOKIT_LIFELOG: 0 = off, 1 = file (default), 2 = stderr (old path). */
static pthread_mutex_t s_ll_mtx = PTHREAD_MUTEX_INITIALIZER;
static FILE *s_ll_f;
static int s_ll_mode = -1;
static void
lifelog(const char *fmt, ...)
{
   if (s_ll_mode == 0) return;
   pthread_mutex_lock(&s_ll_mtx);
   if (s_ll_mode < 0) {
      const char *e = getenv("VIRGL_IOKIT_LIFELOG");
      s_ll_mode = e ? atoi(e) : 1;
      if (s_ll_mode == 1) {
         char path[64];
         snprintf(path, sizeof path, "/tmp/virgl_iokit.%d.log", (int)getpid());
         s_ll_f = fopen(path, "a");
         if (s_ll_f) setvbuf(s_ll_f, NULL, _IOFBF, 65536);
         else s_ll_mode = 2;
      }
      fprintf(stderr, "virgl_iokit: rung254 lifecycle log mode %d (%s)\n", s_ll_mode,
              s_ll_mode == 0 ? "off" : s_ll_mode == 1 ? "/tmp/virgl_iokit.<pid>.log" : "stderr");
   }
   if (s_ll_mode != 0) {
      va_list ap; va_start(ap, fmt);
      vfprintf(s_ll_mode == 1 && s_ll_f ? s_ll_f : stderr, fmt, ap);
      va_end(ap);
   }
   pthread_mutex_unlock(&s_ll_mtx);
}

static void
virgl_iokit_hw_res_destroy(struct virgl_iokit_winsys *iws,
                           struct virgl_hw_res *res)
{
   census_unregister(res);
   /* RUNG 68: log the unref for lifecycle correlation */
   lifelog("virgl_iokit: 0x6005 UNREF res=%u\n",
           (unsigned)res->res_handle);
   /* 0x6005 resourceUnref — kext sends RESOURCE_UNREF and also detaches
    * backing if still held (defensive cleanup). */
   uint64_t in[1] = { res->res_handle };
   IOConnectCallMethod(iws->connect, IOKIT_RESOURCE_UNREF,
                       in, 1, NULL, 0, NULL, NULL, NULL, NULL);

   if (res->ptr)
      bk_free(res->ptr, res->size);   /* 236a */
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
      res->ptr = bk_alloc(size);   /* 236a: off-heap (see bk_alloc) */
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
         bk_free(res->ptr, size);
         FREE(res);
         return NULL;
      }
      res->res_handle = (uint32_t)out[0];
      /* RUNG 68: log every create for lifecycle correlation */
      lifelog("virgl_iokit: 0x6002 CREATE res=%u ctx=%u fmt=%u %ux%u bind=0x%x\n",
              (unsigned)res->res_handle, (unsigned)iws->ctx_id,
              (unsigned)format, width, height, (unsigned)bind);
      /* Resource identity log (2026-08-20): pairs kext handle with
       * fmt/dims/bind so a flushed handle (e.g. res=287) is identifiable
       * without decoding the command stream. VIRGL_IOKIT_DEBUG-gated. */
      iokit_dbg("iokit: res create handle=%u fmt=%u %ux%u bind=0x%x size=%u\n",
                res->res_handle, res->format, res->width, res->height,
                res->bind, res->size);
      census_register(res);
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
         /*
          * the 3D path already census_register()ed this res — take it
          * out of the registry BEFORE freeing it, or the entry stays
          * alive with a dangling pointer for the next census flush
          * (use-after-free) and the slot is never reclaimed. */
         census_unregister(res);
         /* Best-effort cleanup: unref the resource we just created */
         uint64_t unref_in[1] = { res->res_handle };
         IOConnectCallMethod(iws->connect, IOKIT_RESOURCE_UNREF,
                             unref_in, 1, NULL, 0, NULL, NULL, NULL, NULL);
         bk_free(res->ptr, size);
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
      /* RUNG 68: log every 0x6009 with ctx+res — correlate against
       * the UTM debug log's "Illegal resource N" (the cascade seed) */
      lifelog("virgl_iokit: 0x6009 ctx=%u res=%u -> 0x%x\n",
              (unsigned)iws->ctx_id, (unsigned)res->res_handle, kr);
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

/* PER-RESOURCE WAIT (2026-08-19 fence era, the DRM VIRTGPU_WAIT
 * contract — Linux virtgpu_ioctl.c): replaces the global drain. The
 * kext records last_seq per resource at dispatch time (the FCE1
 * handle list each submit now carries); 0x600B waits until that
 * resource's fences complete (15 s bound) — or tests without blocking
 * under NOWAIT (the real resource_is_busy; the return-false interim
 * of a88c72eb4b4 is retired). The global drain could only say
 * "everything done" or "something isn't" — the approximation behind
 * the is_busy famine and the pipeline serialization, both 2026-08-19. */
#define IOKIT_FENCE_WAIT  0x600B
#define FENCE_FLAG_NOWAIT 1u

static kern_return_t
virgl_iokit_fence_call(struct virgl_iokit_winsys *iws, uint32_t res,
                       uint32_t flags)
{
   uint64_t in[2] = { res, flags };
   return IOConnectCallMethod(iws->connect, IOKIT_FENCE_WAIT,
                              in, 2, NULL, 0,
                              NULL, NULL, NULL, NULL);
}

/* resource_wait: block (bounded 15 s) until the resource is ready. */
static void
virgl_iokit_res_wait(struct virgl_iokit_winsys *iws, uint32_t res)
{
   virgl_iokit_fence_call(iws, res, 0);
}

/* Retired global drain (kept only as a fallback if a pre-fence kext
 * is ever booted against this winsys — it logs on the kext side). */
static void
virgl_iokit_drain(struct virgl_iokit_winsys *iws)
{
   uint64_t in[1] = { 120000 };   /* timeout ms */
   IOConnectCallMethod(iws->connect, IOKIT_DRAIN_3D,
                       in, 1, NULL, 0,
                       NULL, NULL, NULL, NULL);
}

static int
virgl_iokit_transfer_put(struct virgl_winsys *vws,
                         struct virgl_hw_res *res,
                         const struct pipe_box *box,
                         uint32_t stride, uint32_t layer_stride,
                         uint32_t buf_offset, uint32_t level)
{
   struct virgl_iokit_winsys *iws = virgl_iokit_winsys(vws);
   iokit_dbg("iokit: transfer_put res=%u box=%ux%ux%u stride=%u lstride=%u off=%u\n",
             res->res_handle, box->width, box->height, box->depth,
             stride, layer_stride, buf_offset);
   virgl_iokit_res_wait(iws, res->res_handle);   /* fence era: wait THIS resource, not the world */
   /* Scalars [9..11] carry the iov layout (stride/layer_stride/offset).
    * THE 2026-08-18 ROOT-CAUSE FIX: these were silently discarded and the
    * kext wired stride=0/layer_stride=0/offset=0, so vrend placed every
    * sub-box row at y*0 → offset-collapsed transfers. Full-surface-at-
    * origin boxes happened to work (sequential host path), masking the
    * bug everywhere the 2D desktop relied on it. Indicies 0-8 unchanged
    * for ABI compatibility with the pre-fix kext (which ignores [9..11]). */
   uint64_t in[12] = {
      res->res_handle, level,
      box->x, box->y, box->z,
      box->width, box->height, box->depth,
      iws->ctx_id,
      stride, layer_stride, buf_offset,
   };
   kern_return_t kr = IOConnectCallMethod(iws->connect, IOKIT_TRANSFER_TO_HOST_3D,
                                           in, 12, NULL, 0,
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
   iokit_dbg("iokit: transfer_get res=%u box=%ux%ux%u stride=%u lstride=%u off=%u\n",
             res->res_handle, box->width, box->height, box->depth,
             stride, layer_stride, buf_offset);
   virgl_iokit_res_wait(iws, res->res_handle);   /* fence era: readback waits THIS resource */
   /* See transfer_put — same fix, readback direction. */
   uint64_t in[12] = {
      res->res_handle, level,
      box->x, box->y, box->z,
      box->width, box->height, box->depth,
      iws->ctx_id,
      stride, layer_stride, buf_offset,
   };
   kern_return_t kr = IOConnectCallMethod(iws->connect, IOKIT_TRANSFER_FROM_3D,
                                           in, 12, NULL, 0,
                                           NULL, NULL, NULL, NULL);
   return (kr == KERN_SUCCESS) ? 0 : -1;
}

/* ===================================================================
 * Command buffer management — malloc/reloc tracking
 * =================================================================== */

/* 2026-10-12 (VMQemuVGA LEDGER, ringtest): the hash slot alone was taken as
 * "already added" — any resource colliding (handle % 512) with one already in
 * the cbuf was never added: no FCE1 fence, no cbuf reference, no
 * num_cs_references, so Mesa neither flushed nor waited before mapping it
 * (stale readbacks; SubData over buffers queued draws still needed). Upstream
 * virgl_drm_lookup_res semantics: verify the slot's resource, else scan. */
static bool
virgl_iokit_lookup_res(struct virgl_iokit_cmd_buf *cbuf, struct virgl_hw_res *res)
{
   unsigned hash = res->res_handle % (ARRAY_SIZE(cbuf->is_handle_added));
   if (!cbuf->is_handle_added[hash])
      return false;
   unsigned i = cbuf->reloc_indices_hashlist[hash];
   if (i < cbuf->cres && cbuf->res_bo[i] == res)
      return true;
   for (i = 0; i < cbuf->cres; i++) {
      if (cbuf->res_bo[i] == res) {
         cbuf->reloc_indices_hashlist[hash] = i;
         return true;
      }
   }
   return false;
}

static void
virgl_iokit_add_res(struct virgl_iokit_winsys *iws,
                    struct virgl_iokit_cmd_buf *cbuf,
                    struct virgl_hw_res *res)
{
   unsigned hash = res->res_handle % ARRAY_SIZE(cbuf->is_handle_added);
   if (cbuf->cres >= cbuf->nres) {   /* 2026-10-12: grow (upstream virgl_drm_add_res) */
      unsigned new_nres = cbuf->nres + 256;
      struct virgl_hw_res **nb = REALLOC(cbuf->res_bo,
                                         cbuf->nres * sizeof(struct virgl_hw_res *),
                                         new_nres * sizeof(struct virgl_hw_res *));
      if (!nb) {
         fprintf(stderr, "virgl_iokit: failure to add resource %u (%u)\n", cbuf->cres, new_nres);
         /* F1 (2026-10-17 winsys review): emit_res has already written
          * res->res_handle into the command stream (or is about to), so
          * returning here would leave a handle in the blob that is NOT in
          * res_bo: absent from the FCE1 frame (the kext records no fence
          * for it, so a later unref's fence-wait at 0x6005 does not cover
          * the batch) and unreferenced (the resource can be destroyed,
          * UNREF'd, and its backing freed while the batch is still
          * queued). That is rung 250's "SET_VERTEX_BUFFERS names a handle
          * not in FCE1" — the host freeze trigger 1. Poison the cbuf;
          * submit_cmd refuses to send it. */
         cbuf->poisoned = 1;
         return;
      }
      cbuf->res_bo = nb;
      cbuf->nres = new_nres;
   }
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
      if (cbuf->res_bo[i]) {
         /* 2026-10-12: the cbuf's use ends here (upstream virgl_drm_release_all_res);
          * never decremented before, so res_is_ref stayed true forever */
         p_atomic_dec(&cbuf->res_bo[i]->num_cs_references);
         virgl_iokit_resource_reference(&iws->base, &cbuf->res_bo[i], NULL);
      }
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

   cbuf->poisoned = 0;   /* F1 */
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

/* RUNG 251 instrument (VMQemuVGA LEDGER 2026-10-13). Kext rung 250 matched
 * the host's freeze trigger "vrend_set_single_vbo: Illegal resource 779" to a
 * SET_VERTEX_BUFFERS naming handle 779 that was ALREADY unref'd and was NOT
 * in that batch's FCE1 list (= this cbuf's res_bo at submit). By the code,
 * emit_res always adds a handle it writes, so the blob and the list should
 * never disagree. This checks that at submit, on the guest side, and keeps
 * a ring of emit_res events so a miss can say whether emit_res ever saw the
 * handle in this cbuf generation, on which thread, and whether two threads
 * were inside the winsys at once (emit_res / submit overlap). */
#define R251_RING 8192
static struct { uint32_t handle, cdw, cres, gen; void *cbuf; void *tid; unsigned char already; } s_r251[R251_RING];
static volatile unsigned s_r251_n, s_r251_gen, s_r251_inflight, s_r251_overlap;
static void *s_r251_last_tid;
/* VIRGL_IOKIT_R251=0 turns the whole instrument off (ring, overlap, submit
 * walk) for the on/off A/B: does 251's own cost hide the race? Read once. */
static int r251_on(void)
{
   static int v = -1;
   if (v < 0) {
      const char *e = getenv("VIRGL_IOKIT_R251");
      v = !(e && *e == '0');
      fprintf(stderr, "rung251: instrument %s (VIRGL_IOKIT_R251=%s)\n", v ? "ON" : "OFF", e ? e : "unset");
   }
   return v;
}
static inline void r251_enter(const char *who)
{
   if (!r251_on()) return;
   unsigned v = p_atomic_inc_return(&s_r251_inflight);
   if (v > 1) {
      unsigned o = p_atomic_inc_return(&s_r251_overlap);
      if (o <= 16 || (o & 1023) == 0)
         fprintf(stderr, "rung251: OVERLAP #%u in %s: %u threads inside the winsys (this %p, last %p)\n",
                 o, who, v, (void *)pthread_self(), s_r251_last_tid);
   }
   s_r251_last_tid = (void *)pthread_self();
}
static inline void r251_leave(void) { if (r251_on()) p_atomic_dec(&s_r251_inflight); }

static void
virgl_iokit_emit_res(struct virgl_winsys *vws,
                     struct virgl_cmd_buf *_cbuf,
                     struct virgl_hw_res *res, bool write_buf)
{
   struct virgl_iokit_winsys *iws = virgl_iokit_winsys(vws);
   struct virgl_iokit_cmd_buf *cbuf = virgl_iokit_cmd_buf(_cbuf);
   r251_enter("emit_res");
   bool already = virgl_iokit_lookup_res(cbuf, res);
   unsigned at = cbuf->base.cdw;
   if (write_buf)
      cbuf->base.buf[cbuf->base.cdw++] = res->res_handle;
   if (!already)
      virgl_iokit_add_res(iws, cbuf, res);
   if (r251_on()) {
      unsigned k = p_atomic_inc_return(&s_r251_n) % R251_RING;
      s_r251[k].handle = res->res_handle; s_r251[k].cdw = write_buf ? at : 0xffffffffu;
      s_r251[k].cres = cbuf->cres; s_r251[k].gen = s_r251_gen; s_r251[k].cbuf = cbuf;
      s_r251[k].tid = (void *)pthread_self(); s_r251[k].already = already;
   }
   r251_leave();
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

   /* rung277 (PR review t3cbN): a resource-only emit_res(..., write_buf =
    * false) can poison a cbuf whose cdw is still 0 — add_res's REALLOC
    * failed with no handle written into the blob. The empty-buffer return
    * below would then keep poisoned set AND keep the res_bo references, so
    * the NEXT submit, a valid batch, is the one that gets dropped (and the
    * failed batch's references leak until then). Clear it here with the
    * same cleanup the drop path does; there is nothing to submit. */
   if (cbuf->poisoned && cbuf->base.cdw == 0) {
      virgl_iokit_release_all_res(iws, cbuf);
      memset(cbuf->is_handle_added, 0, sizeof(cbuf->is_handle_added));
      cbuf->poisoned = 0;
   }

   if (cbuf->base.cdw == 0)
      return 0;

   iokit_dbg("iokit: submit_cmd cdw=%u\n", cbuf->base.cdw);

   /* RUNG 68b — SETUP-BATCH OPCODE DECODE: for the first N submits,
    * decode every command header's opcode byte into its name. The
    * first GL_INVALID_OPERATION after MakeCurrent traces to a specific
    * opcode; this names it. VIRGL_IOKIT_OPS=N caps at N submits. */
   {
      static int s_ops_left = -1;
      if (s_ops_left < 0) {
         const char *e = getenv("VIRGL_IOKIT_OPS");
         s_ops_left = e ? atoi(e) : 0;
      }
      if (s_ops_left > 0) {
         s_ops_left--;
         static const char *names[] = {
            "NOP", "CREATE_OBJECT", "BIND_OBJECT", "DESTROY_OBJECT",
            "SET_VIEWPORT", "SET_FRAMEBUFFER", "SET_VERTEX_BUFFERS",
            "CLEAR", "DRAW_VBO", "RESOURCE_INLINE_WRITE",
            "SET_SAMPLER_VIEWS", "SET_INDEX_BUFFER", "SET_CONSTANT_BUF",
            "SET_STENCIL_REF", "SET_BLEND_COLOR", "SET_SCISSOR",
            "BLIT", "RESOURCE_COPY_REGION", "BIND_SAMPLER_STATES",
            "BEGIN_QUERY", "END_QUERY", "GET_QUERY_RESULT",
            "SET_POLYGON_STIPPLE", "SET_CLIP_STATE", "SET_SAMPLE_MASK",
            "SET_STREAMOUT_TARGETS", "SET_RENDER_CONDITION",
            "SET_UNIFORM_BUFFER", "SET_SUB_CTX", "CREATE_SUB_CTX",
            "DESTROY_SUB_CTX", "BIND_SHADER", "SET_TESS_STATE",
            "SET_MIN_SAMPLES", "SET_SHADER_BUFFERS", "SET_SHADER_IMAGES",
            "MEMORY_BARRIER", "LAUNCH_GRID", "SET_FB_NO_ATTACH",
            "TEXTURE_BARRIER",
         };
         fprintf(stderr, "rung68b OPS(cdw=%u):", cbuf->base.cdw);
         unsigned i = 0;
         while (i < cbuf->base.cdw && i < 4096) {
            uint32_t hdr = cbuf->base.buf[i];
            unsigned op = hdr & 0xff;
            unsigned len = (hdr >> 16) & 0x3fff;
            fprintf(stderr, " %s(%u,len=%u)",
                    op < 40 ? names[op] : "???", op, len);
            i += len + 1;
            if (len == 0) i++;  /* at least header */
         }
         fprintf(stderr, "\n");
      }
   }

   /* Full command-stream dump (FBO-clear hunt, 2026-08-18): the kext's
    * hex dump caps at 20 dwords and misparses the header layout, so the
    * guest-side stream was never fully visible. VIRGL_IOKIT_DUMP=1
    * prints every dword of every submit (capped at 4096 dwords), one
    * line per 8 dwords, prefixed with the submit size. Header layout
    * per virgl_protocol.h:152 — cmd | (obj << 8) | (len << 16). */
   {
      static bool s_dump = false;
      static bool s_dump_init = false;
      if (!s_dump_init) {
         s_dump_init = true;
         s_dump = getenv("VIRGL_IOKIT_DUMP") != NULL;
      }
      if (s_dump && cbuf->base.cdw > 0) {
         unsigned n = cbuf->base.cdw;
         if (n > 4096) n = 4096;
         fprintf(stderr, "iokit DUMP submit cdw=%u:\n", cbuf->base.cdw);
         for (unsigned i = 0; i < n; i += 8) {
            fprintf(stderr, "  [%04u]", i);
            for (unsigned j = i; j < i + 8 && j < n; j++)
               fprintf(stderr, " %08x", cbuf->base.buf[j]);
            fprintf(stderr, "\n");
         }
      }
   }

   /* FENCE FRAME (2026-08-19, DRM execbuffer model): prefix the blob
    * with the command buffer's resource-handle list so the kext can
    * track last_seq per resource and 0x600B WAIT can express
    * per-resource readiness. Frame: ['FCE1'][cres][handles][blob] in
    * ONE contiguous buffer (IOConnectCallMethod takes a single
    * structureInput). The kext discriminates on the magic; a pre-fence
    * kext would treat the whole frame as the blob, so kext+winsys
    * deploy as a pair. */
   /* 2026-10-12: the full list, no 96 cap — the kext now fences every
    * handle (it no longer stores them per batch; the frame is bounded only
    * by its 1 MB size, i.e. far beyond any cbuf). Requires the matching kext. */
   r251_enter("submit");
   if (cbuf->poisoned) {   /* F1: a handle in the blob has no FCE1 entry / no ref */
      static unsigned s_poisoned;
      s_poisoned++;
      /* rung279 (PR review t3cbQ): rate-limit like the counters above —
       * under a persistent add_res OOM every submit wrote a stderr line. */
      if (s_poisoned <= 4 || (s_poisoned & 1023) == 0)
         fprintf(stderr, "virgl_iokit: cbuf POISONED (add_res OOM) — batch DROPPED "
                 "(cdw=%u cres=%u, drop #%u)\n", cbuf->base.cdw, cbuf->cres, s_poisoned);
      virgl_iokit_release_all_res(iws, cbuf);
      memset(cbuf->is_handle_added, 0, sizeof(cbuf->is_handle_added));
      cbuf->base.cdw = 0;
      cbuf->poisoned = 0;
      r251_leave();
      return -1;
   }
   if (r251_on()) {   /* RUNG 251: every SET_VERTEX_BUFFERS handle must be in res_bo */
      static unsigned s_vb, s_miss;
      unsigned n = cbuf->base.cdw, i = 0;
      while (i < n) {
         uint32_t hdr = cbuf->base.buf[i], len = hdr >> 16;
         if (i + 1 + len > n) {
            static unsigned s_bad; if (++s_bad <= 8) fprintf(stderr, "rung251: walk overran at dword %u of %u (hdr %08x)\n", i, n, hdr);
            break;
         }
         if ((hdr & 0xff) == 6 && len && (len % 3) == 0) {
            for (uint32_t v = 0; v < len / 3; v++) {
               uint32_t h = cbuf->base.buf[i + 1 + v * 3 + 2];
               if (!h) continue;
               s_vb++;
               bool in = false;
               for (unsigned q = 0; q < cbuf->cres; q++) if (cbuf->res_bo[q] && cbuf->res_bo[q]->res_handle == h) { in = true; break; }
               if (in) continue;
               s_miss++;
               if (s_miss <= 64 || (s_miss & 1023) == 0) {
                  fprintf(stderr, "rung251: MISS #%u handle %u at dword %u (cmd at %u, slot %u) not in res_bo (cres %u, cdw %u, gen %u, cbuf %p, tid %p, vb %u)\n",
                          s_miss, h, i + 1 + v * 3 + 2, i, v, cbuf->cres, n, s_r251_gen, (void *)cbuf, (void *)pthread_self(), s_vb);
                  unsigned shown = 0, top = s_r251_n;
                  for (unsigned b = 0; b < R251_RING && shown < 6; b++) {
                     unsigned k = (top - b) % R251_RING;
                     if (s_r251[k].handle != h) continue;
                     shown++;
                     fprintf(stderr, "rung251:   emit_res %u back: handle %u cdw %d already %u cres-after %u gen %u cbuf %p tid %p\n",
                             b, s_r251[k].handle, (int)s_r251[k].cdw, s_r251[k].already, s_r251[k].cres, s_r251[k].gen, s_r251[k].cbuf, s_r251[k].tid);
                  }
                  if (!shown) fprintf(stderr, "rung251:   handle %u not in the last %u emit_res events\n", h, R251_RING);
               }
            }
         }
         i += 1 + len;
      }
   }
   p_atomic_inc(&s_r251_gen);
   uint32_t cres = cbuf->cres;
   uint32_t blob_bytes = cbuf->base.cdw * sizeof(uint32_t);
   uint32_t frame_bytes = 8 + cres * 4 + blob_bytes;
   uint32_t *frame = (uint32_t *)malloc(frame_bytes);
   if (!frame) {
      virgl_iokit_release_all_res(iws, cbuf);
      memset(cbuf->is_handle_added, 0, sizeof(cbuf->is_handle_added));
      cbuf->base.cdw = 0;
      r251_leave();
      return -1;
   }
   frame[0] = 0x31454346u;   /* 'FCE1' */
   frame[1] = cres;
   for (uint32_t i = 0; i < cres; i++)
      frame[2 + i] = cbuf->res_bo[i]->res_handle;
   memcpy(&frame[2 + cres], cbuf->base.buf, blob_bytes);

   uint64_t ctx_scalar = iws->ctx_id;
   kr = IOConnectCallMethod(iws->connect, IOKIT_SUBMIT_CMDS,
                            &ctx_scalar, 1,
                            frame, frame_bytes,
                            NULL, NULL, NULL, NULL);
   free(frame);
   if (kr != KERN_SUCCESS) {
      fprintf(stderr, "virgl_iokit: 0x6008 submitVirglCommandsEx FAIL 0x%x\n", kr);
   }

   virgl_iokit_release_all_res(iws, cbuf);
   memset(cbuf->is_handle_added, 0, sizeof(cbuf->is_handle_added));
   cbuf->base.cdw = 0;
   cbuf->poisoned = 0;   /* F1: a fresh generation starts clean (the poisoned blob was dropped or sent) */
   r251_leave();
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

   /* RUNG 174a — THE NULL GUARD (the fftest3 SIGSEGV class):
    * when the host stops answering virtio, the winsys create
    * can complete with a dead/absent connection; the screen
    * then called through it unguarded. Defaults stand, the
    * caller sees a caps-less screen instead of a crash. */
   if (!iws || !iws->connect) {
      fprintf(stderr, "virgl_iokit: get_caps REFUSED — no "
              "connection (host virtio timeout class); "
              "caps stay at defaults\n");
      return 0;   /* r174a's intent: defaults stand, screen continues.
                     Was a bare `return;` (UB in a non-void function) since
                     r174a; only surfaces as an error with current clang. */
   }

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
         /* rung278 (PR review YovcR): iokit_dbg, not fprintf — these two
          * lines fired on every screen init with VIRGL_IOKIT_DEBUG unset,
          * so every application paid for them. */
         iokit_dbg("iokit: get_caps capset_id=%u ver=%u size=%u copy=%u\n",
                   capset_id, capset_ver, (uint32_t)blob_sz, copy);
         return 0;
      }
   }

   iokit_dbg("iokit: get_caps — no capset returned by host\n");
   return -1;
}

/* ===================================================================
 * Stubs / trivial — see LEDGER vtable scope classification
 * =================================================================== */
static int  virgl_iokit_get_fd(struct virgl_winsys *vws) { return -1; }
/* resource_wait (fence era, 2026-08-19): the DRM VIRTGPU_WAIT contract
 * — block until THIS resource's fences complete (15 s bound). The
 * global-drain version waited out every client's work; this waits the
 * resource the caller actually needs. */
static void virgl_iokit_resource_wait(struct virgl_winsys *vws, struct virgl_hw_res *res)
{
   struct virgl_iokit_winsys *iws = virgl_iokit_winsys(vws);
   virgl_iokit_res_wait(iws, res->res_handle);
}
static bool virgl_iokit_resource_is_busy(struct virgl_winsys *vws, struct virgl_hw_res *res)
{
   /* REAL busy check (fence era): the 0x600B NOWAIT probe tests THIS
    * resource's fences only — the DRM dma_resv_test_signaled
    * equivalent. Retires the return-false interim of a88c72eb4b4:
    * that fix ended the DONTBLOCK famine (the 1 ms global-drain probe
    * reported busy under any in-flight batch, erroring every
    * PIPE_MAP_DONTBLOCK map); this replaces the approximation with
    * the reference semantics. */
   struct virgl_iokit_winsys *iws = virgl_iokit_winsys(vws);
   return virgl_iokit_fence_call(iws, res->res_handle,
                                  FENCE_FLAG_NOWAIT) != KERN_SUCCESS;
}
static uint32_t virgl_iokit_resource_get_storage_size(struct virgl_winsys *vws, struct virgl_hw_res *res) { return res->size; }

/* Fence stubs — valid only while submit_cmd is synchronous (LEDGER.md) */
static struct pipe_fence_handle *virgl_iokit_cs_create_fence(struct virgl_winsys *vws, int fd) { return NULL; }
static bool virgl_iokit_fence_wait(struct virgl_winsys *vws, struct pipe_fence_handle *f, uint64_t timeout) { return true; }
static void virgl_iokit_fence_reference(struct virgl_winsys *vws, struct pipe_fence_handle **dst, struct pipe_fence_handle *src) { *dst = src; }
static int  virgl_iokit_fence_get_fd(struct virgl_winsys *vws, struct pipe_fence_handle *f) { return -1; }
/* HOST-SIDE BLIT (2026-08-19, the recorded design): the front buffer
 * is presented by a GPU-side VIRGL_CCMD_BLIT into the desktop scanout
 * resource at the window's rect (kext 0x600C) — no 5.5 MB guest
 * readback (measured per-flush wall 130 ms mean; the transfer half IS
 * the readback). The shim stashes the window's desktop rect from the
 * AppKit-safe main thread (drawRect) and exposes it via
 * cgl_shim_host_present_rect(); without a valid rect (host-present
 * mode off, or before the first drawRect) this is a no-op and the
 * legacy readback path stands. CONTRACT: fence-wait the resource
 * first — prior 3D work must be complete host-side before the blit
 * reads it. */
/* Rect state lives HERE (libOSMesa) — the substitute re-exports this
 * library, so the shim calls the setter; dependency direction stays
 * one-way. Written from the AppKit-safe main thread, read here. */
__attribute__((visibility("default")))
int cgl_shim_host_present_enabled(void);
/* Forward declarations only — no code change vs r259. These two are
 * dlsym'd by cgl_interpose.c (not header-linked), so their prototypes
 * live here to satisfy -Werror=missing-prototypes under CI's clang. */
__attribute__((visibility("default")))
int cgl_shim_host_present_rect(int *x, int *y, int *w, int *h);
__attribute__((visibility("default")))
void cgl_shim_set_host_present_rect(int x, int y, int w, int h);
static int g_hp_x, g_hp_y, g_hp_w, g_hp_h;
static volatile int g_hp_valid;
static int g_hp_mode = -1;

__attribute__((visibility("default")))
int cgl_shim_host_present_enabled(void)
{
   if (g_hp_mode < 0)
      g_hp_mode = (getenv("SHIM_HOST_PRESENT") != NULL);
   return g_hp_mode;
}

__attribute__((visibility("default")))
int cgl_shim_host_present_rect(int *x, int *y, int *w, int *h)
{
   if (!cgl_shim_host_present_enabled() || !g_hp_valid)
      return 0;
   *x = g_hp_x; *y = g_hp_y; *w = g_hp_w; *h = g_hp_h;
   return 1;
}

__attribute__((visibility("default")))
void cgl_shim_set_host_present_rect(int x, int y, int w, int h)
{
   g_hp_x = x; g_hp_y = y; g_hp_w = w; g_hp_h = h;
   g_hp_valid = 1;
}

static void virgl_iokit_flush_frontbuffer(struct virgl_winsys *vws, struct virgl_cmd_buf *cbuf,
                                          struct virgl_hw_res *res, unsigned level, unsigned layer,
                                          void *winsys_drawable_handle, struct pipe_box *sub_box)
{
   struct virgl_iokit_winsys *iws = virgl_iokit_winsys(vws);
   static int s_hook_log = 0;
   int x, y, w, h;
   if (s_hook_log < 5) {
      s_hook_log++;
      fprintf(stderr, "iokit: flush_frontbuffer HOOK ctx=%u res=%u lvl=%u fmt=%u %ux%u\n",
              iws->ctx_id, res ? res->res_handle : 0, level,
              res ? res->format : 0, res ? res->width : 0, res ? res->height : 0);
   }
   if (!res || level != 0 || layer != 0)
      return;

   /* CONTENT CENSUS (2026-08-20): under SHIM_CONTENT_CENSUS, read back
    * EVERY alive color/depth resource (registry) through the device
    * (transfer_get, full box) and count nonzero 32-bit words. The
    * presented front reading zero while some OTHER resource holds the
    * compositor's draws is the identity-mismatch signature. Registry
    * mutex held across transfers so backings cannot be freed mid-read
    * (destores serialize behind censuses; debug-run behavior). The
    * backings are align_malloc'd — NOT zeroed — so uniform zeros after
    * a get=0 transfer are genuine device content, not a skipped read. */
   {
      static int s_census = -1, s_census_log = 0;
      if (s_census < 0)
         s_census = (getenv("SHIM_CONTENT_CENSUS") != NULL);
      if (s_census && s_census_log < 6) {
         int fired = 0;
         pthread_mutex_lock(&s_creg_mtx);
         for (int i = 0; i < CENSUS_REG_MAX && fired < 26; i++) {
            struct census_entry *e = &s_creg[i];
            if (!e->alive || !e->res->ptr)
               continue;
            /* color fmts 53/54 (B8G8R8A8/R8G8B8A8) + depth 142. No
             * size floor: a 1x1 drawable front is a live candidate —
             * the composite may be executing into a stale 1x1 target
             * (the constant-size batch stream + zero window fb + a
             * content-bearing 1x1 in an earlier run point there). */
            if (e->fmt != 53 && e->fmt != 54 && e->fmt != 142)
               continue;
            unsigned bpp = util_format_get_blocksize(e->fmt);
            uint64_t stride = (uint64_t)e->w * bpp;
            uint64_t need = stride * e->h;
            if (bpp < 1 || bpp > 8 || need < 4 || need > e->res->size)
               continue;
            fired++;
            struct pipe_box box;
            memset(&box, 0, sizeof(box));
            box.width = e->w; box.height = e->h; box.depth = 1;
            int cr = virgl_iokit_transfer_get(vws, e->res, &box,
                                              (uint32_t)stride,
                                              (uint32_t)need, 0, 0);
            uint32_t total = (uint32_t)(need / 4);
            const uint32_t *p = (const uint32_t *)e->res->ptr;
            uint32_t nz = 0, first = 0; int found = 0;
            for (uint32_t k = 0; k < total; k++) {
               if (p[k]) { nz++; if (!found) { found = 1; first = k; } }
            }
            fprintf(stderr, "iokit: CENSUS res=%u fmt=%u %ux%u bpp=%u get=%d nonzero=%u/%u first_off=%u\n",
                    e->handle, e->fmt, e->w, e->h,
                    bpp, cr, nz, total, first * 4u);
         }
         pthread_mutex_unlock(&s_creg_mtx);
         if (fired)
            s_census_log++;   /* one fire = one flush's batch of reads */
      }
   }

   /* RUNG 67 — the zero-copy present, FIRST (before the host-present
    * rect gate — it needs no rect; geometry from the resource): bind
    * the 3D resource to the scanout + flush per frame; the host
    * composites directly, NO readback. Presentation is FULLSCREEN
    * (SET_SCANOUT maps the resource rect to the whole display).
    * SHIM_ZERO_COPY gates it; the default stays the proven relay. */
   {
      static int s_zcp = -1;
      if (s_zcp < 0)
         s_zcp = (getenv("SHIM_ZERO_COPY") != NULL);
      if (s_zcp) {
         virgl_iokit_res_wait(iws, res->res_handle);
         uint64_t zi[3] = { res->res_handle,
                            (uint64_t)res->width, (uint64_t)res->height };
         IOConnectCallMethod(iws->connect, 0x600F /* scanoutPresent3D */,
                             zi, 3, NULL, 0,
                             NULL, NULL, NULL, NULL);
         return;
      }
   }
   if (!cgl_shim_host_present_rect(&x, &y, &w, &h))
      return;
   virgl_iokit_res_wait(iws, res->res_handle);   /* prior 3D complete host-side */
   uint64_t in[5] = { res->res_handle, (uint64_t)x, (uint64_t)y,
                      (uint64_t)w, (uint64_t)h };
   IOConnectCallMethod(iws->connect, 0x600C /* hostBlit3D */,
                       in, 5, NULL, 0,
                       NULL, NULL, NULL, NULL);
}

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

   /* RUNG 261 (2026-10-19): build-identity line — one per process, grep
    * 'rung261'. This build = .r260 + F1 + F2:
    *   F1  add_res-oom-poison: a res_bo REALLOC failure poisons the cmd
    *       buffer and submit_cmd DROPS the batch (an FCE1-incomplete frame
    *       is never sent; r250 'Illegal resource' class closed).
    *   F2  quarantine min-hold: released backings age VIRGL_IOKIT_QHOLD ms
    *       (default 2500 = kext r270 deferral 2000 + slack) before munmap;
    *       hard cap BK_QMAX=256 still forces eviction (logged).
    * qhold echoes the effective value (same parse/clamp as bk_config). */
   {
      const char *qh = getenv("VIRGL_IOKIT_QHOLD");
      int hold = qh ? atoi(qh) : 2500;
      if (hold < 0) hold = 0;
      fprintf(stderr, "virgl_iokit: rung261: qhold=%d ms hardcap=256 "
              "add_res-oom-poison=on\n", hold);
   }

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
