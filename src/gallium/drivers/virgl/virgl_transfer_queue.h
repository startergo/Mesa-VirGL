/*
 * Copyright 2018 Chromium.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef VIRGL_TRANSFER_QUEUE_H
#define VIRGL_TRANSFER_QUEUE_H

#include "pipe/p_defines.h"
#include "util/list.h"
#include "util/simple_mtx.h"

struct virgl_cmd_buf;
struct virgl_screen;
struct virgl_context;
struct virgl_transfer;

struct virgl_transfer_queue {
   struct list_head transfer_list;
   struct virgl_screen *vs;
   struct virgl_context *vctx;
   struct virgl_cmd_buf *tbuf;
   uint32_t num_dwords;
   /* CROSS-THREAD LOCK (2026-08-19): the queue is per-context and its
    * API was bare — legal under the GL one-thread-per-context contract
    * EXCEPT the MakeCurrent handoff window: the implicit st_glFlush of
    * the OLD context (make_current → virgl_flush_eq → clear) can race
    * another thread beginning work on the same context (unmap /
    * extend_buffer mutating the same list/hash). Under the pre-TSD
    * globals-stomp the race was accidentally serialized; TSD exposed
    * it — crash powerfox_2026-08-19-191644 (SIGSEGV in clear +242).
    * Every PUBLIC entry takes this lock; internal helpers (add_internal,
    * perform_action, compare_and_perform_action, find_overlap when
    * called internally) run under the caller's hold — simple_mtx is
    * NOT recursive, so only public entries lock.
    *
    * Lock BOUNDARY: this lock protects the transfer LIST (and the
    * num_dwords bookkeeping that the list operations perform) against
    * the MakeCurrent handoff window above. It does NOT cover the
    * command buffer: cbuf->cdw is written by virgl_transfer_queue_clear
    * and concurrently by every encoder call on the same context.
    * Concurrent use of one context from two threads at once is
    * UNSUPPORTED — the same contract as upstream virgl (which locks
    * nothing here at all): a context is owned by one thread at a time;
    * handing it over requires a MakeCurrent boundary, and that boundary
    * is what this lock makes safe. */
   simple_mtx_t lock;
};

void virgl_transfer_queue_init(struct virgl_transfer_queue *queue,
                               struct virgl_context *vctx);

void virgl_transfer_queue_fini(struct virgl_transfer_queue *queue);

int virgl_transfer_queue_unmap(struct virgl_transfer_queue *queue,
                               struct virgl_transfer *transfer);

int virgl_transfer_queue_clear(struct virgl_transfer_queue *queue,
                               struct virgl_cmd_buf *buf);

bool virgl_transfer_queue_is_queued(struct virgl_transfer_queue *queue,
                                    struct virgl_transfer *transfer);

/*
 * Search the transfer queue for a transfer suitable for extension and
 * extend it to include the specified data.
 */
bool virgl_transfer_queue_extend_buffer(struct virgl_transfer_queue *queue,
                                        const struct virgl_hw_res *hw_res,
                                        unsigned offset, unsigned size,
                                        const void *data);

#endif /* VIRGL_TRANSFER_QUEUE_H */
