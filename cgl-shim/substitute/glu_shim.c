/* glu_shim.c — GLU subset implemented over Mesa, defined INSIDE the
 * substitute OpenGL.framework.
 *
 * THE GLU-ROUTING FIX (Mesa-VirGL LEDGER entry 13, 2026-08-17):
 * GLU is a CLIENT of GL, not a peer. Flurry (and any 10.6-era GL app
 * using GLU — near-universal in that era) resolves _gluBuild2DMipmaps
 * against the OpenGL umbrella; Apple's umbrella re-exports libGLU.dylib,
 * and our substitute re-exports the real framework — so Apple's GLU ran
 * with a shim token where it dereferences _CGLContextObject internals,
 * and walked off memory in halveImage_ubyte (crash report
 * ScreenSaverEngine_2026-08-17). Defining glu* HERE shadows the
 * re-export chain: an image's own definitions win its export table.
 * Apple's GLU never executes.
 *
 * Scope: the entry points era apps actually use — gluBuild2DMipmaps,
 * gluScaleImage, gluErrorString, and the matrix helpers. Implemented
 * against Mesa's functions (same p_* pointers as the rest of the
 * substitute; the few extras dlsym'd lazily from the already-open
 * libOSMesa). Pixel-store state is saved/restored around every upload
 * — the context-global lesson (GL_UNPACK_ROW_LENGTH bug, 2026-08-13).
 * Simplified box filter, not pixel-identical to Apple's GLU — visual
 * equivalence, not bit-equivalence, is the bar.
 *
 * Deliberately ABSENT: quadrics, NURBS, tessellation, project/unproject.
 * A missing symbol fails the client's bind loudly — the honest failure —
 * rather than routing to Apple's implementation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>

/* Mesa's gl.h is force-included by the build (-include). p_* pointers
 * and g_mesa live in cgl_interpose.c. */
extern void *g_mesa;
extern void   (*p_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid *);
extern void   (*p_glPixelStorei)(GLenum, GLint);
extern GLint  (*p_glGetIntegerv)(GLenum, GLint *);
extern void   (*p_glMatrixMode)(GLenum);
extern void   (*p_glLoadIdentity)(void);

/* Lazily-dlsym'd extras for the matrix helpers. */
static void (*s_glMultMatrixf)(const GLfloat *);
static void (*s_glRotatef)(GLfloat, GLfloat, GLfloat, GLfloat);
static void (*s_glTranslatef)(GLfloat, GLfloat, GLfloat);

static void glu_lazy_init(void)
{
    static int s_done = 0;
    if (!s_done) {
        s_done = 1;
        if (g_mesa) {
            s_glMultMatrixf = (void (*)(const GLfloat *))dlsym(g_mesa, "glMultMatrixf");
            s_glRotatef     = (void (*)(GLfloat, GLfloat, GLfloat, GLfloat))dlsym(g_mesa, "glRotatef");
            s_glTranslatef  = (void (*)(GLfloat, GLfloat, GLfloat))dlsym(g_mesa, "glTranslatef");
        }
        if (!s_glMultMatrixf || !s_glRotatef || !s_glTranslatef)
            fprintf(stderr, "glu_shim: WARNING — matrix-helper dlsym "
                    "incomplete (mult=%p rot=%p xlt=%p)\n",
                    (void *)s_glMultMatrixf, (void *)s_glRotatef,
                    (void *)s_glTranslatef);
    }
}

#ifndef GLU_INVALID_ENUM
#define GLU_INVALID_ENUM 100900
#define GLU_INVALID_VALUE 100901
#define GLU_OUT_OF_MEMORY 100902
#define GLU_INVALID_OPERATION 100903
#endif

/* ---------------------------------------------------------------- bytes */
static int bpp_of(GLenum format)
{
    switch (format) {
    case GL_LUMINANCE:          return 1;
    case GL_LUMINANCE_ALPHA:    return 2;
    case GL_RGB:  case GL_BGR:  return 3;
    case GL_RGBA: case GL_BGRA: return 4;
    default:                    return 0;
    }
}

/* Box-filter halve: dst is (w+1)/2 x (h+1)/2. */
static int halve_image(const unsigned char *src, int w, int h, int bpp,
                       unsigned char *dst)
{
    int dw = (w + 1) / 2, dh = (h + 1) / 2;
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            for (int c = 0; c < bpp; c++) {
                unsigned s0 = src[(2 * y) * w * bpp + (2 * x) * bpp + c];
                unsigned s1 = (2 * x + 1 < w) ? src[(2 * y) * w * bpp + (2 * x + 1) * bpp + c] : s0;
                unsigned s2 = (2 * y + 1 < h) ? src[(2 * y + 1) * w * bpp + (2 * x) * bpp + c] : s0;
                unsigned s3 = (2 * x + 1 < w && 2 * y + 1 < h)
                    ? src[(2 * y + 1) * w * bpp + (2 * x + 1) * bpp + c] : s0;
                dst[y * dw * bpp + x * bpp + c] =
                    (unsigned char)((s0 + s1 + s2 + s3 + 2) >> 2);
            }
        }
    }
    return dw * dh * bpp;
}

int gluScaleImage(GLenum format,
                  GLsizei wIn, GLsizei hIn, GLenum typeIn, const void *dataIn,
                  GLsizei wOut, GLsizei hOut, GLenum typeOut, void *dataOut)
{
    if (typeIn != GL_UNSIGNED_BYTE || typeOut != GL_UNSIGNED_BYTE) {
        fprintf(stderr, "glu_shim: gluScaleImage: only GL_UNSIGNED_BYTE "
                "supported (in=0x%x out=0x%x)\n", typeIn, typeOut);
        return GLU_INVALID_ENUM;
    }
    int bpp = bpp_of(format);
    if (!bpp || wIn <= 0 || hIn <= 0 || wOut <= 0 || hOut <= 0)
        return GLU_INVALID_VALUE;

    /* General box filter: each output pixel averages the source block it
     * covers. (Halving falls out exactly.) */
    unsigned long long *acc = calloc((size_t)wOut * hOut * bpp, sizeof(unsigned long long));
    if (!acc) return GLU_OUT_OF_MEMORY;
    for (int sy = 0; sy < hIn; sy++) {
        int dy0 = (int)((long long)sy * hOut / hIn);
        int dy1 = (int)(((long long)sy + 1) * hOut / hIn);
        if (dy1 <= dy0) dy1 = dy0 + 1;
        if (dy1 > hOut) dy1 = hOut;
        for (int sx = 0; sx < wIn; sx++) {
            int dx0 = (int)((long long)sx * wOut / wIn);
            int dx1 = (int)(((long long)sx + 1) * wOut / wIn);
            if (dx1 <= dx0) dx1 = dx0 + 1;
            if (dx1 > wOut) dx1 = wOut;
            const unsigned char *sp = (const unsigned char *)dataIn
                + ((size_t)sy * wIn + sx) * bpp;
            for (int dy = dy0; dy < dy1; dy++)
                for (int dx = dx0; dx < dx1; dx++)
                    for (int c = 0; c < bpp; c++)
                        acc[((size_t)dy * wOut + dx) * bpp + c] += sp[c];
        }
    }
    /* Divide by block size. Exact block areas vary at edges; use the
     * floor-based block bounds again for the count. */
    for (int dy = 0; dy < hOut; dy++) {
        for (int dx = 0; dx < wOut; dx++) {
            int sy0 = (int)((long long)dy * hIn / hOut);
            int sy1 = (int)(((long long)dy + 1) * hIn / hOut); if (sy1 <= sy0) sy1 = sy0 + 1;
            int sx0 = (int)((long long)dx * wIn / wOut);
            int sx1 = (int)(((long long)dx + 1) * wIn / wOut); if (sx1 <= sx0) sx1 = sx0 + 1;
            /* Clamp to source bounds (edge rounding). */
            if (sy1 > hIn) sy1 = hIn;
            if (sx1 > wIn) sx1 = wIn;
            unsigned long long n = (unsigned long long)(sy1 - sy0) * (sx1 - sx0);
            if (!n) n = 1;
            unsigned char *dp = (unsigned char *)dataOut + ((size_t)dy * wOut + dx) * bpp;
            for (int c = 0; c < bpp; c++)
                dp[c] = (unsigned char)(acc[((size_t)dy * wOut + dx) * bpp + c] / n);
        }
    }
    free(acc);
    return 0;
}

int gluBuild2DMipmaps(GLenum target, GLint internalFormat,
                      GLsizei width, GLsizei height, GLenum format, GLenum type,
                      const void *data)
{
    if (type != GL_UNSIGNED_BYTE) {
        fprintf(stderr, "glu_shim: gluBuild2DMipmaps: only GL_UNSIGNED_BYTE "
                "supported (type=0x%x)\n", type);
        return GLU_INVALID_ENUM;
    }
    int bpp = bpp_of(format);
    if (!bpp || width <= 0 || height <= 0)
        return GLU_INVALID_VALUE;

    GLint save_align = 4;
    if (p_glGetIntegerv) p_glGetIntegerv(GL_UNPACK_ALIGNMENT, &save_align);
    if (p_glPixelStorei) p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    /* Level 0: the caller's data (internalFormat honoured as given). */
    if (p_glTexImage2D)
        p_glTexImage2D(target, 0, internalFormat, width, height, 0,
                       format, type, data);

    int levels = 0;
    unsigned char *cur = malloc((size_t)width * height * bpp);
    if (!cur) { if (p_glPixelStorei) p_glPixelStorei(GL_UNPACK_ALIGNMENT, save_align); return GLU_OUT_OF_MEMORY; }
    memcpy(cur, data, (size_t)width * height * bpp);
    int w = width, h = height;
    while (w > 1 || h > 1) {
        int dw = (w + 1) / 2, dh = (h + 1) / 2;
        unsigned char *next = malloc((size_t)dw * dh * bpp);
        if (!next) { free(cur); if (p_glPixelStorei) p_glPixelStorei(GL_UNPACK_ALIGNMENT, save_align); return GLU_OUT_OF_MEMORY; }
        halve_image(cur, w, h, bpp, next);
        free(cur);
        cur = next;
        w = dw; h = dh;
        if (p_glTexImage2D)
            p_glTexImage2D(target, levels + 1, internalFormat, w, h, 0,
                           format, type, cur);
        levels++;
        if (levels > 32) break;   /* safety */
    }
    free(cur);
    if (p_glPixelStorei) p_glPixelStorei(GL_UNPACK_ALIGNMENT, save_align);
    return 0;
}

const GLubyte *gluErrorString(GLenum error)
{
    switch (error) {
    case 0:                      return (const GLubyte *)"no error";
    case GL_INVALID_ENUM:        return (const GLubyte *)"invalid enumerant";
    case GL_INVALID_VALUE:       return (const GLubyte *)"invalid value";
    case GL_INVALID_OPERATION:   return (const GLubyte *)"invalid operation";
    case GL_STACK_OVERFLOW:      return (const GLubyte *)"stack overflow";
    case GL_STACK_UNDERFLOW:     return (const GLubyte *)"stack underflow";
    case GL_OUT_OF_MEMORY:       return (const GLubyte *)"out of memory";
    case GLU_INVALID_ENUM:       return (const GLubyte *)"invalid enumerant";
    case GLU_INVALID_VALUE:      return (const GLubyte *)"invalid value";
    case GLU_OUT_OF_MEMORY:      return (const GLubyte *)"out of memory";
    case GLU_INVALID_OPERATION:  return (const GLubyte *)"invalid operation";
    default:                     return (const GLubyte *)"unknown error";
    }
}

/* ------------------------------------------------------------ matrices */

void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble zNear, GLdouble zFar)
{
    glu_lazy_init();
    if (!s_glMultMatrixf) return;
    double f = 1.0 / tan(fovy * 3.14159265358979323846 / 360.0);
    float m[16] = {0};
    m[0] = (float)(f / aspect);
    m[5] = (float)f;
    m[10] = (float)((zFar + zNear) / (zNear - zFar));
    m[11] = -1.0f;
    m[14] = (float)(2.0 * zFar * zNear / (zNear - zFar));
    s_glMultMatrixf(m);
}

void gluOrtho2D(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top)
{
    /* glOrtho(l,r,b,t,-1,1). */
    glu_lazy_init();
    if (!s_glMultMatrixf) return;
    float m[16];
    m[0]  = (float)(2.0 / (right - left));
    m[1] = m[2] = m[3] = 0;
    m[5]  = (float)(2.0 / (top - bottom));
    m[4] = m[6] = m[7] = 0;
    m[10] = (float)(-2.0 / (1.0 - (-1.0)));
    m[11] = 0;
    m[14] = (float)(-(1.0 + (-1.0)) / (1.0 - (-1.0)));
    m[8] = m[9] = 0;
    m[12] = (float)(-(right + left) / (right - left));
    m[13] = (float)(-(top + bottom) / (top - bottom));
    m[15] = 1;
    s_glMultMatrixf(m);
}

void gluLookAt(GLdouble eyex, GLdouble eyey, GLdouble eyez,
               GLdouble centerx, GLdouble centery, GLdouble centerz,
               GLdouble upx, GLdouble upy, GLdouble upz)
{
    glu_lazy_init();
    if (!s_glMultMatrixf || !s_glRotatef || !s_glTranslatef) return;
    double fx = centerx - eyex, fy = centery - eyey, fz = centerz - eyez;
    double fl = sqrt(fx * fx + fy * fy + fz * fz);
    if (fl == 0) return;
    fx /= fl; fy /= fl; fz /= fl;
    double sx = fy * upz - fz * upy, sy = fz * upx - fx * upz, sz = fx * upy - fy * upx;
    double sl = sqrt(sx * sx + sy * sy + sz * sz);
    if (sl == 0) return;
    sx /= sl; sy /= sl; sz /= sl;
    double ux = sy * fz - sz * fy, uy = sz * fx - sx * fz, uz = sx * fy - sy * fx;

    float m[16] = {
        (float)sx, (float)ux, (float)-fx, 0,
        (float)sy, (float)uy, (float)-fy, 0,
        (float)sz, (float)uz, (float)-fz, 0,
        0, 0, 0, 1
    };
    s_glMultMatrixf(m);
    s_glRotatef(180.0f, 0.0f, 1.0f, 0.0f);
    s_glTranslatef((float)-eyex, (float)-eyey, (float)-eyez);
}

void gluPickMatrix(GLdouble x, GLdouble y, GLdouble width, GLdouble height,
                   GLint viewport[4])
{
    glu_lazy_init();
    if (!s_glMultMatrixf) return;
    if (width <= 0 || height <= 0) return;
    float m[16] = {0};
    m[0]  = (float)(viewport[2] / width);
    m[5]  = (float)(viewport[3] / height);
    m[10] = 1.0f;
    m[12] = (float)(viewport[2] + 2.0 * (viewport[0] - x) / width);
    m[13] = (float)(viewport[3] + 2.0 * (viewport[1] - y) / height);
    m[15] = 1.0f;
    s_glMultMatrixf(m);
}
