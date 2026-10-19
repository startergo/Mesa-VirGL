/*
 * Copyright 2014, 2015 Red Hat.
 * Copyright 2026 startergo.
 *
 * Derived from virgl_vtest_public.h: IOKit-backed winsys for the macOS
 * 10.6 guest under UTM (IOServiceOpen of VMQemuVGAAccelerator instead
 * of a vtest Unix socket).
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
#ifndef VIRGL_IOKIT_PUBLIC_H
#define VIRGL_IOKIT_PUBLIC_H

struct virgl_winsys;
struct sw_winsys;

/*
 * Factory: open VMQemuVGAAccelerator (IOServiceOpen type=4 →
 * VMVirtIOGPUUserClient), create one virgl context, and return a
 * virgl_winsys vtable. One io_connect_t per pipe_screen; all contexts
 * created from the screen share it (matches vtest's single-socket model).
 *
 * The kext owns the resource_id and context_id allocators (LEDGER.md:
 * kext-allocated IDs). resource_create receives its ID from the kext's
 * 0x6002 scalar output — the winsys never picks IDs.
 */
struct virgl_winsys *virgl_iokit_winsys_wrap(struct sw_winsys *sws);

#endif
