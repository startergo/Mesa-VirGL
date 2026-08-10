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
 *
 * Signatures match src/gallium/auxiliary/vl/vl_video_buffer.h exactly
 * (return types, parameter types, and parameter order). The real types
 * come from Gallium's pipe/p_*.h headers; we forward-declare the tagged
 * structs/enums we need so the prototypes are self-contained without
 * dragging in the full Gallium auxiliary header chain.
 */

#include <stdbool.h>   /* bool */
#include <stddef.h>    /* NULL */

/* Forward-declared tagged structs matching Gallium's real types. The
 * typedefs in pipe/p_defines.h, pipe/p_video_codec.h, etc. are
 * `typedef struct pipe_screen { ... } pipe_screen;` and so on, so
 * referring to them as `struct pipe_screen` here matches the tag the
 * real API uses. */
struct pipe_screen;
struct pipe_context;
struct pipe_video_buffer;
struct pipe_video_codec;

/* is_format_supported takes three Gallium enums (pipe_format,
 * pipe_video_profile, pipe_video_entrypoint). C allows forward declarations
 * of enum *tags* but a forward-declared enum is an incomplete type, so the
 * 10.6 toolchain rejects it in a parameter list. Use `int` instead: under
 * the C calling convention Gallium enums are int-sized, and the stub never
 * inspects the value (it aborts unconditionally), so the ABI is preserved. */

void
vl_video_buffer_destroy(struct pipe_video_buffer *buf) { __builtin_abort(); }

struct pipe_video_buffer *
vl_video_buffer_create(struct pipe_context *ctx,
                       const struct pipe_video_buffer *templat) {
    __builtin_abort();
}

void *
vl_video_buffer_get_associated_data(struct pipe_video_buffer *vbuf,
                                    struct pipe_video_codec *vcodec) {
    __builtin_abort();
}

void
vl_video_buffer_set_associated_data(struct pipe_video_buffer *vbuf,
                                    struct pipe_video_codec *vcodec,
                                    void *associated_data,
                                    void (*destroy_associated_data)(void *)) {
    __builtin_abort();
}

bool
vl_video_buffer_is_format_supported(struct pipe_screen *screen,
                                    int format,
                                    int profile,
                                    int entrypoint) {
    __builtin_abort();
}
