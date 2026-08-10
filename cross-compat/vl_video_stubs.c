/*
 * Stub implementations for vl_video_buffer_* functions.
 * The vl subsystem (src/gallium/auxiliary/vl/) is not compiled when
 * video-codecs=[] — video decode is irrelevant for a Snow Leopard 3D
 * rendering use case. virgl_video.c references these five functions;
 * they are unreachable at runtime (no app will call the video codec
 * path through virgl on this target) but must resolve at link time.
 *
 * All abort() — per the project's fail-loud pattern. If video support
 * is ever needed, compile src/gallium/auxiliary/vl/ properly and
 * delete this file.
 */
#include <stddef.h>

/* Forward-declare the types we need to match the vl API signatures.
 * The real types come from vl/vl_video_buffer.h, but we can't include
 * that here without pulling in the full Gallium auxiliary header chain. */
struct pipe_screen;
struct pipe_context;
struct pipe_video_buffer;

void vl_video_buffer_destroy(struct pipe_video_buffer *buf) { __builtin_abort(); }
struct pipe_video_buffer *vl_video_buffer_create(struct pipe_context *ctx,
    const void *tmpl) { __builtin_abort(); }
void *vl_video_buffer_get_associated_data(struct pipe_video_buffer *buf,
    void *destroy_cb) { __builtin_abort(); }
void vl_video_buffer_set_associated_data(struct pipe_video_buffer *buf,
    struct pipe_context *ctx, void *data, void (*destroy)(void *)) { __builtin_abort(); }
int vl_video_buffer_is_format_supported(struct pipe_screen *screen,
    unsigned int format, unsigned int profile, unsigned int entrypoint) { __builtin_abort(); }
