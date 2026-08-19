// offscreen_min.mm — minimal FBO clear + readback through the substitute.
// No shaders, no quad, no uploads: the flushed 0x6008 stream should be
// small enough to appear IN FULL in the kext's hex dump, letting us see
// whether SET_FRAMEBUFFER_STATE + CLEAR(magenta) are actually encoded.
#define _SECURITY_SECKEYCHAIN_H_ 1
#include <stdio.h>
#import <Cocoa/Cocoa.h>
#import <OpenGL/gl.h>
#import <OpenGL/OpenGL.h>

int main(void) {
    fprintf(stderr, "== offscreen_min\n");
    const NSOpenGLPixelFormatAttribute attrs[] = { (NSOpenGLPixelFormatAttribute)0 };
    NSOpenGLPixelFormat *fmt = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
    NSOpenGLContext *ctx = [[NSOpenGLContext alloc] initWithFormat:fmt shareContext:nil];
    if (!ctx) { fprintf(stderr, "FAIL ctx\n"); return 1; }
    [ctx makeCurrentContext];
    fprintf(stderr, "RENDERER: %s\n", glGetString(GL_RENDERER));

    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 400, 300, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    fprintf(stderr, "FBO status: %s err 0x%04x\n",
            glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE
            ? "COMPLETE" : "INCOMPLETE", glGetError());

    glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
    glDisable(GL_SCISSOR_TEST);          /* viewport/scissor hypothesis: */
    glViewport(0, 0, 400, 300);          /* offscreen ctx inherits 1x1 ->  */
    glClear(GL_COLOR_BUFFER_BIT);        /* clear clipped to nothing      */
    glFinish();

    GLubyte p[4] = {170, 170, 170, 170};
    glReadPixels(200, 150, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    fprintf(stderr, "read(200,150): %u,%u,%u,%u\n", p[0], p[1], p[2], p[3]);
    /* Geometry map: offset-dependence vs size-dependence of the failure. */
    {
        struct { int x, y, w, h; } probes[] = {
            {0,0,1,1}, {1,0,1,1}, {0,1,1,1}, {10,0,1,1}, {0,10,1,1},
            {399,299,1,1}, {0,0,2,2}, {0,0,400,1}, {0,149,400,1},
            {200,150,2,2}, {0,0,64,64},
        };
        for (unsigned i = 0; i < sizeof(probes)/sizeof(probes[0]); i++) {
            GLubyte q[4] = {170,170,170,170};
            glReadPixels(probes[i].x, probes[i].y, probes[i].w, probes[i].h,
                         GL_RGBA, GL_UNSIGNED_BYTE, q);
            /* For multi-pixel boxes sample the requested pixel. */
            fprintf(stderr, "probe(%d,%d %dx%d): %u,%u,%u,%u\n",
                    probes[i].x, probes[i].y, probes[i].w, probes[i].h,
                    q[0], q[1], q[2], q[3]);
        }
    }

    /* FULL-TEXTURE readback: where, if anywhere, did the magenta land?
     * all-zero = nothing landed; all-magenta = probe coordinate issue;
     * partial = offset/flip/scissor geometry. */
    {
        static GLubyte full[400 * 300 * 4];
        glReadPixels(0, 0, 400, 300, GL_RGBA, GL_UNSIGNED_BYTE, full);
        unsigned mag = 0, zero = 0, other = 0, i;
        for (i = 0; i < 400 * 300; i++) {
            GLubyte *q = full + i * 4;
            if (q[0] == 255 && q[1] == 0 && q[2] == 255 && q[3] == 255) mag++;
            else if (!q[0] && !q[1] && !q[2] && !q[3]) zero++;
            else other++;
        }
        fprintf(stderr, "full: %u magenta, %u zero, %u other / 120000\n",
                mag, zero, other);
        if (other) {
            fprintf(stderr, "other sample [0]: %u,%u,%u,%u  [60000]: %u,%u,%u,%u\n",
                    full[0], full[1], full[2], full[3],
                    full[60000 * 4], full[60000 * 4 + 1],
                    full[60000 * 4 + 2], full[60000 * 4 + 3]);
        }
        if (mag) {
            unsigned first = 0;
            for (i = 0; i < 400 * 300; i++)
                if (full[i * 4] == 255 && full[i * 4 + 1] == 0) { first = i; break; }
            fprintf(stderr, "first magenta at linear %u = (x=%u y=%u)\n",
                    first, first % 400, first / 400);
        }
    }
    fprintf(stderr, "err 0x%04x\n== done\n", glGetError());
    return 0;
}
