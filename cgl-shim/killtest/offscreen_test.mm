// offscreen_test.mm — Gecko's offscreen WebGL context path, without Gecko.
//
// Mirrors GLContextProviderCGL::CreateOffscreenFBOContext (compat path):
//   NSOpenGLPixelFormat initWithAttributes:{0}   (kAttribs_offscreen = empty)
//   NSOpenGLContext initWithFormat:shareContext:nil
//   makeCurrentContext — NO view, NO pbuffer (Gecko renders via FBOs only)
// then the webgltest5/6 question at C level:
//   texture FBO -> clear magenta -> glReadPixels  (expect 255,0,255,255)
//   default FB (0,0) read                        (the 1x1 OSMesa buffer)
//
// Run on the guest with the substitute:
//   DYLD_FRAMEWORK_PATH=/tmp/subst DYLD_LIBRARY_PATH=/tmp/subst \
//   GALLIUM_DRIVER=virgl /Users/sl/offscreen_test
// A/B: same binary with GALLIUM_DRIVER=softpipe.

#define _SECURITY_SECKEYCHAIN_H_ 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   /* sleep() — PRRT_kwDOPZhcm86uEfcN (PR review) */

#import <Cocoa/Cocoa.h>
#import <OpenGL/gl.h>
#import <OpenGL/OpenGL.h>

static void perr(const char *step) {
    GLenum e = glGetError();
    fprintf(stderr, "  [after %s] glGetError=0x%04x\n", step, e);
}

int main(void) {
    fprintf(stderr, "== offscreen_test: Gecko-style offscreen ctx, FBO clear+read\n");

    const NSOpenGLPixelFormatAttribute attrs[] = {
        (NSOpenGLPixelFormatAttribute)0
    };
    NSOpenGLPixelFormat *fmt = [[NSOpenGLPixelFormat alloc]
                                initWithAttributes:attrs];
    if (!fmt) { fprintf(stderr, "FAIL: pixel format (empty attrs) returned nil\n"); return 1; }

    NSOpenGLContext *ctx = [[NSOpenGLContext alloc] initWithFormat:fmt
                                                       shareContext:nil];
    if (!ctx) { fprintf(stderr, "FAIL: NSOpenGLContext\n"); return 1; }
    fprintf(stderr, "ctx OK: %p\n", (void *)ctx);

    [ctx makeCurrentContext];
    perr("makeCurrentContext");

    fprintf(stderr, "RENDERER: %s\n", glGetString(GL_RENDERER));
    fprintf(stderr, "VERSION:  %s\n", glGetString(GL_VERSION));

    /* --- texture FBO, clear magenta, read --- */
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 400, 300, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);

    /* ORDERING PROBE: a 1x1 dummy upload BEFORE any render. If the first
     * clear now lands (read = magenta), the resource's host-side lifecycle
     * was the blocker — renders issued before the first transfer were
     * dropped. Env-gated so the old behavior stays reproducible. */
    if (getenv("DUMMY_UPLOAD_FIRST")) {
        GLubyte one[4] = {9, 9, 9, 9};
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA,
                        GL_UNSIGNED_BYTE, one);
        glFinish();
        fprintf(stderr, "[dummy 1x1 upload done before first render]\n");
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    fprintf(stderr, "FBO status: %s\n",
            st == GL_FRAMEBUFFER_COMPLETE ? "COMPLETE" : "INCOMPLETE");
    perr("FBO setup");

    glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    perr("clear+finish");

    /* ASYNC PROBE: sleep before the FIRST read, with no getTexImage in
     * the picture. If the read turns magenta, the submit->execute path
     * is merely ASYNC (host catches up in time); if still zeros, the
     * clear command is LOST, not delayed. */
    if (getenv("SLEEP_BEFORE_READ")) {
        sleep(3);
        fprintf(stderr, "[slept 3s before first read]\n");
    }

    GLubyte p[4] = {170, 170, 170, 170};
    glReadPixels(200, 150, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    fprintf(stderr, "FBO read(200,150): %u,%u,%u,%u  %s\n",
            p[0], p[1], p[2], p[3],
            (p[0]==170 && p[1]==170) ? "(UNWRITTEN)" :
            (p[0]==255 && p[1]==0 && p[2]==255) ? "(MAGENTA — correct)" :
            (p[0]==0 && p[1]==0 && p[2]==0) ? "(ZEROS — clear did not land)" : "(other)");
    perr("FBO read");

    /* --- discriminator 1: UPLOAD known pixels into the same texture ---
     * If the uploaded red block reads back but the cleared region reads
     * zero: reads work, renders-into-FBO are what's dropped. */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    {
        GLubyte blk[2 * 2 * 4];
        int i;
        for (i = 0; i < 4; i++) {
            blk[i * 4 + 0] = 255; blk[i * 4 + 1] = 0;
            blk[i * 4 + 2] = 0;   blk[i * 4 + 3] = 255;
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, 200, 150, 2, 2, GL_RGBA,
                        GL_UNSIGNED_BYTE, blk);
        glFinish();
        perr("upload");
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        GLubyte u[4] = {170, 170, 170, 170};
        glReadPixels(200, 150, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, u);
        fprintf(stderr, "FBO read after UPLOAD(200,150): %u,%u,%u,%u  %s\n",
                u[0], u[1], u[2], u[3],
                (u[0] == 255 && u[1] == 0) ? "(UPLOAD LANDS — reads OK, renders dropped)" :
                (u[0] == 170 && u[1] == 170) ? "(UNWRITTEN)" : "(other)");
        GLubyte v[4] = {170, 170, 170, 170};
        glReadPixels(10, 10, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, v);
        fprintf(stderr, "FBO read clear-region(10,10): %u,%u,%u,%u\n",
                v[0], v[1], v[2], v[3]);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    /* --- discriminator 2: DRAW a quad into the FBO (fixed-function) --- */
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, 400, 300);
    glDisable(GL_DEPTH_TEST);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glBegin(GL_QUADS);
    glColor4f(0.0f, 1.0f, 0.0f, 1.0f);
    glVertex2f(-1.0f, -1.0f);
    glVertex2f(1.0f, -1.0f);
    glVertex2f(1.0f, 1.0f);
    glVertex2f(-1.0f, 1.0f);
    glEnd();
    glFinish();
    perr("quad draw");
    {
        GLubyte d[4] = {170, 170, 170, 170};
        glReadPixels(200, 150, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, d);
        fprintf(stderr, "FBO read after QUAD(200,150): %u,%u,%u,%u  %s\n",
                d[0], d[1], d[2], d[3],
                (d[1] == 255 && d[0] == 0) ? "(DRAW LANDS)" : "(draw dropped too)");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    /* --- discriminator 3: glGetTexImage read path --- */
    {
        static GLubyte full[400 * 300 * 4];
        memset(full, 0xAB, sizeof(full));
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, full);
        perr("getTexImage");
        int nonab = 0, nz = 0, i;
        for (i = 0; i < 400 * 300; i++) {
            if (full[i * 4] != 0xAB || full[i * 4 + 1] != 0xAB) nonab++;
            if (full[i * 4] || full[i * 4 + 1] || full[i * 4 + 2] || full[i * 4 + 3]) nz++;
        }
        fprintf(stderr, "getTexImage: %d/%d pixels differ from 0xAB prefill, "
                "%d/%d nonzero\n", nonab, 400 * 300, nz, 400 * 300);
        fprintf(stderr, "getTexImage samples: (200,150)=%u,%u,%u,%u "
                "(10,10)=%u,%u,%u,%u (5,5)=%u,%u,%u,%u\n",
                full[(150 * 400 + 200) * 4 + 0], full[(150 * 400 + 200) * 4 + 1],
                full[(150 * 400 + 200) * 4 + 2], full[(150 * 400 + 200) * 4 + 3],
                full[(10 * 400 + 10) * 4 + 0], full[(10 * 400 + 10) * 4 + 1],
                full[(10 * 400 + 10) * 4 + 2], full[(10 * 400 + 10) * 4 + 3],
                full[(5 * 400 + 5) * 4 + 0], full[(5 * 400 + 5) * 4 + 1],
                full[(5 * 400 + 5) * 4 + 2], full[(5 * 400 + 5) * 4 + 3]);
    }

    /* --- discriminator 4: readPixels AFTER getTexImage + a timed flush --- */
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    {
        GLubyte g1[4] = {170, 170, 170, 170};
        glReadPixels(200, 150, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, g1);
        fprintf(stderr, "FBO read after getTexImage(200,150): %u,%u,%u,%u  %s\n",
                g1[0], g1[1], g1[2], g1[3],
                (g1[1] == 255) ? "(CURRENT — earlier read predated flush)" :
                (g1[0] == 255 && g1[1] == 0) ? "(STILL STALE after getTexImage)" : "(other)");
    }
    glFlush();
    sleep(2);
    {
        GLubyte g2[4] = {170, 170, 170, 170};
        glReadPixels(200, 150, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, g2);
        fprintf(stderr, "FBO read after flush+2s(200,150): %u,%u,%u,%u  %s\n",
                g2[0], g2[1], g2[2], g2[3],
                (g2[1] == 255) ? "(CURRENT — time/flush heals it)" :
                (g2[0] == 255 && g2[1] == 0) ? "(STALE even after flush+2s)" : "(other)");
        GLubyte g3[4] = {170, 170, 170, 170};
        glReadPixels(10, 10, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, g3);
        fprintf(stderr, "FBO read after flush+2s(10,10): %u,%u,%u,%u\n",
                g3[0], g3[1], g3[2], g3[3]);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    /* --- default framebuffer read at a safe coordinate --- */
    glClearColor(0.2f, 0.6f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    GLubyte q[4] = {170, 170, 170, 170};
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, q);
    fprintf(stderr, "DFB read(0,0): %u,%u,%u,%u\n", q[0], q[1], q[2], q[3]);
    perr("DFB read");

    fprintf(stderr, "== done\n");
    return 0;
}
