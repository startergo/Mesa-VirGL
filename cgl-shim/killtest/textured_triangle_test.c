// textured_triangle_test.c — first test of textures + samplers + multi-resource
//
// Exercises everything the plain triangle does, PLUS:
//   - Texture creation (RESOURCE_CREATE_3D with VIRGL_BIND_SAMPLER_VIEW)
//   - Texture data upload (transfer_put with pixel data — guest→host)
//   - Sampler state (CREATE_OBJECT for sampler view + sampler state)
//   - Texture sampling in fragment shader (GLSL texture2D())
//   - Multiple live resources (color RT + VBO + texture = 3 simultaneous)
//
// Same bisect pattern: GALLIUM_DRIVER=softpipe (reference) then virgl.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/osmesa.h>

#define W 256
#define H 256
#define TEX_W 4
#define TEX_H 4

/* Vertex data: interleaved position (xy) + UV (xy), 4 floats per vertex */
static const GLfloat tri_data[] = {
    /* pos.x   pos.y   uv.u   uv.v */
    -0.8f,   -0.8f,  0.0f, 0.0f,   /* bottom-left  → texel (0,0) */
     0.8f,   -0.8f,  1.0f, 0.0f,   /* bottom-right → texel (3,0) */
     0.0f,    0.8f,  0.5f, 1.0f,   /* top-center   → texel (2,3) */
};

/* 4×4 RGBA texture: each texel has a distinct color so UV interpolation
 * actually produces different colors at different screen positions.
 * texel[x][y] = (x*64, y*64, 200, 255) → a simple gradient with a blue
 * offset. Blue is NOT 128 anywhere: with the old constant B=128 a red/blue
 * channel swap was invisible at every sampled texel (R=128 at x=2). B=200
 * makes the swap fail the check. Center pixel (128,128) maps to UV
 * (0.5, 0.5) → texel (2,2) = (128, 128, 200, 255) (barycentric — NOT the
 * UV centroid (that is (0.5, 1/3)); the screen center NDC (0,0) carries
 * weights (0.25, 0.25, 0.5) w.r.t. the NDC corners (-1,-1),(1,-1),(0,1)
 * (x: a=b; y: c=2a; sum: 4a=1), and 0.25*(0,0)+0.25*(1,0)+0.5*(0.5,1)
 * = (0.5, 0.5)). */
static const char *vs_src =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "  v_uv = a_uv;\n"
    "}\n";

static const char *fs_src =
    "uniform sampler2D u_tex;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "  gl_FragColor = texture2D(u_tex, v_uv);\n"
    "}\n";

static int check_pixel(unsigned char *buf, int x, int y,
                       int exp_r, int exp_g, int exp_b, const char *label)
{
    int idx = (y * W + x) * 4;
    int got_b = buf[idx + 0];
    int got_g = buf[idx + 1];
    int got_r = buf[idx + 2];
    int tol = 2;  /* ±2 for texture filtering tolerance */

    if (abs(got_r - exp_r) <= tol && abs(got_g - exp_g) <= tol && abs(got_b - exp_b) <= tol) {
        printf("PASS [%s]: pixel(%d,%d) = RGB(%d,%d,%d) expected RGB(%d,%d,%d)\n",
               label, x, y, got_r, got_g, got_b, exp_r, exp_g, exp_b);
        return 0;
    } else {
        printf("FAIL [%s]: pixel(%d,%d) = RGB(%d,%d,%d) expected RGB(%d,%d,%d)\n",
               label, x, y, got_r, got_g, got_b, exp_r, exp_g, exp_b);
        return 1;
    }
}

int main(void)
{
    OSMesaContext ctx;
    unsigned char *buf;
    int failures = 0;

    printf("textured_triangle_test starting\n");

    ctx = OSMesaCreateContextExt(OSMESA_BGRA, 16, 0, 0, NULL);
    if (!ctx) { printf("FAIL: OSMesaCreateContextExt\n"); return 1; }
    printf("OK: context created\n");

    buf = (unsigned char *)malloc(W * H * 4);
    if (!buf) { OSMesaDestroyContext(ctx); return 1; }
    if (!OSMesaMakeCurrent(ctx, buf, GL_UNSIGNED_BYTE, W, H)) {
        printf("FAIL: OSMesaMakeCurrent\n"); free(buf); OSMesaDestroyContext(ctx); return 1;
    }

    /* --- Create texture with distinct texel colors --- */
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    unsigned char tex_data[TEX_W * TEX_H * 4];
    for (int y = 0; y < TEX_H; y++) {
        for (int x = 0; x < TEX_W; x++) {
            int i = (y * TEX_W + x) * 4;
            tex_data[i + 0] = (unsigned char)(x * 64);   /* R: 0, 64, 128, 192 */
            tex_data[i + 1] = (unsigned char)(y * 64);   /* G: 0, 64, 128, 192 */
            tex_data[i + 2] = 200;                        /* B: constant 200, ≠ R everywhere (YKBwm) */
            tex_data[i + 3] = 255;                        /* A: opaque */
        }
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TEX_W, TEX_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex_data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    printf("OK: texture created (%dx%d, gradient)\n", TEX_W, TEX_H);

    /* --- Shaders --- */
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);
    GLint vs_ok = GL_FALSE;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &vs_ok);
    if (!vs_ok) {
        char log[512]; glGetShaderInfoLog(vs, sizeof(log), NULL, log);
        printf("FAIL: VS compile: %s\n", log); free(buf); OSMesaDestroyContext(ctx); return 1;
    }
    printf("OK: vertex shader compiled\n");

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);
    GLint fs_ok = GL_FALSE;
    glGetShaderiv(fs, GL_COMPILE_STATUS, &fs_ok);
    if (!fs_ok) {
        char log[512]; glGetShaderInfoLog(fs, sizeof(log), NULL, log);
        printf("FAIL: FS compile: %s\n", log); free(buf); OSMesaDestroyContext(ctx); return 1;
    }
    printf("OK: fragment shader compiled\n");

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint link_ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &link_ok);
    if (!link_ok) {
        char log[512]; glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        printf("FAIL: link: %s\n", log); free(buf); OSMesaDestroyContext(ctx); return 1;
    }
    printf("OK: program linked\n");
    glUseProgram(prog);

    /* Bind sampler uniform to texture unit 0 */
    GLint tex_loc = glGetUniformLocation(prog, "u_tex");
    if (tex_loc < 0) {
        printf("FAIL: glGetUniformLocation(u_tex) = %d\n", tex_loc);
        free(buf); OSMesaDestroyContext(ctx); return 1;
    }
    glUniform1i(tex_loc, 0);
    printf("OK: sampler bound to unit 0\n");

    /* --- VBO with interleaved position + UV --- */
    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(tri_data), tri_data, GL_STATIC_DRAW);

    GLint pos_loc = glGetAttribLocation(prog, "a_pos");
    GLint uv_loc = glGetAttribLocation(prog, "a_uv");
    if (pos_loc < 0 || uv_loc < 0) {
        printf("FAIL: attrib locations pos=%d uv=%d\n", pos_loc, uv_loc);
        free(buf); OSMesaDestroyContext(ctx); return 1;
    }
    printf("OK: attrib locations: pos=%d uv=%d\n", pos_loc, uv_loc);

    glEnableVertexAttribArray(pos_loc);
    glVertexAttribPointer(pos_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), 0);
    glEnableVertexAttribArray(uv_loc);
    glVertexAttribPointer(uv_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));

    /* --- Clear + draw --- */
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);  /* dark grey background */
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    printf("OK: draw complete\n");

    /* --- Verify --- */
    /* Center pixel (128, 128): inside the triangle.
     * UV at screen center via barycentric: (0.5, 0.5) → texel(2,2).
     * texel(2,2) = R=128, G=128, B=200 → RGB(128, 128, 200) */
    failures += check_pixel(buf, W / 2, H / 2, 128, 128, 200, "center-texel(2,2)");

    /* Corner pixel (5, 5): outside the triangle → background */
    failures += check_pixel(buf, 5, 5, 26, 26, 26, "corner-background");

    /* Upper-center (128, 40): inside the triangle near the base.
     * UV ≈ (0.5, 0.0) → texel(2, 0) = RGB(128, 0, 200) */
    failures += check_pixel(buf, W / 2, 40, 128, 0, 200, "bottom-texel(2,0)");

    if (failures == 0)
        printf("RESULT: PASS — textured triangle rendered correctly (3/3 pixels match)\n");
    else
        printf("RESULT: FAIL — %d pixel check(s) failed\n", failures);

    /* Cleanup */
    glDeleteBuffers(1, &vbo);
    glDeleteTextures(1, &tex);
    glDeleteProgram(prog);
    glDeleteShader(fs);
    glDeleteShader(vs);
    free(buf);
    OSMesaDestroyContext(ctx);
    return failures ? 1 : 0;
}
