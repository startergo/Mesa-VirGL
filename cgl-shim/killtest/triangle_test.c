// triangle_test.c — first test of shader compilation + vertex buffers + draw
//
// Same OSMesa + GALLIUM_DRIVER env-var pattern as osmesa_softpipe_test.
// Run with GALLIUM_DRIVER=softpipe (reference) then GALLIUM_DRIVER=virgl.
//
// Exercises:
//   - GLSL shader compilation (GLSL → TGSI in Mesa's state tracker)
//   - Shader object creation (virgl CREATE_OBJECT(VIRGL_OBJECT_SHADER))
//   - Vertex buffer (pipe_resource + transfer_put with vertex data)
//   - Vertex element state (CREATE_OBJECT(VIRGL_OBJECT_VERTEX_ELEMENTS))
//   - Draw call (DRAW_VBO in a command buffer via SUBMIT_3D)
//   - Shader translation (TGSI → GLSL in virglrenderer → Metal in ANGLE)
//
// A clear proves context+resource+backing+submit+transfer. A triangle
// additionally proves the shader compilation chain and the draw pipeline.
// Softpipe stays the reference — a wrong triangle bisects the same way
// a wrong clear would.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/osmesa.h>

#define W 256
#define H 256

/* A full-viewport triangle covering roughly 2/3 of the framebuffer.
 * Vertices in clip space (-1..1). Center pixel (W/2, H/2) should be
 * inside; corner (0,0) should be outside. */
static const GLfloat tri_vertices[] = {
    -0.8f, -0.8f,  /* bottom-left */
     0.8f, -0.8f,  /* bottom-right */
     0.0f,  0.8f,  /* top-center  */
};

static const char *vs_src =
    "void main() {\n"
    "  gl_Position = vec4(gl_Vertex.xy, 0.0, 1.0);\n"
    "}\n";

static const char *fs_src =
    "void main() {\n"
    "  gl_FragColor = vec4(0.8, 0.2, 0.4, 1.0);\n"
    "}\n";

/* Expected: triangle color = (0.8, 0.2, 0.4, 1.0) → RGB(204, 51, 102) = BGR(102, 51, 204)
 *           clear color    = (0.2, 0.6, 0.8, 1.0) → RGB(51, 153, 204) = BGR(204, 153, 51) */
#define TRI_R 204
#define TRI_G 51
#define TRI_B 102
#define BG_R  51
#define BG_G  153
#define BG_B  204

static int check_pixel(unsigned char *buf, int x, int y,
                       int exp_r, int exp_g, int exp_b, const char *label)
{
    /* OSMesa BGRA: buf[(y*W + x) * 4 + 0] = B, +1 = G, +2 = R, +3 = A */
    int idx = (y * W + x) * 4;
    int got_b = buf[idx + 0];
    int got_g = buf[idx + 1];
    int got_r = buf[idx + 2];
    int tol = 1;  /* ±1 ULP for unorm */

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

    printf("triangle_test starting\n");

    ctx = OSMesaCreateContextExt(OSMESA_BGRA, 16, 0, 0, NULL);
    if (!ctx) {
        printf("FAIL: OSMesaCreateContextExt returned NULL\n");
        return 1;
    }
    printf("OK: OSMesaCreateContextExt succeeded (ctx=%p)\n", (void *)ctx);

    buf = (unsigned char *)malloc(W * H * 4);
    if (!buf) {
        printf("FAIL: malloc returned NULL\n");
        OSMesaDestroyContext(ctx);
        return 1;
    }

    if (!OSMesaMakeCurrent(ctx, buf, GL_UNSIGNED_BYTE, W, H)) {
        printf("FAIL: OSMesaMakeCurrent failed\n");
        free(buf);
        OSMesaDestroyContext(ctx);
        return 1;
    }

    /* Clear to background colour — outside the triangle */
    glClearColor(0.20f, 0.60f, 0.80f, 1.00f);
    glClear(GL_COLOR_BUFFER_BIT);

    /* Compile vertex shader */
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);
    GLint vs_ok = GL_FALSE;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &vs_ok);
    if (!vs_ok) {
        char log[512];
        glGetShaderInfoLog(vs, sizeof(log), NULL, log);
        printf("FAIL: vertex shader compile: %s\n", log);
        free(buf);
        OSMesaDestroyContext(ctx);
        return 1;
    }
    printf("OK: vertex shader compiled\n");

    /* Compile fragment shader */
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);
    GLint fs_ok = GL_FALSE;
    glGetShaderiv(fs, GL_COMPILE_STATUS, &fs_ok);
    if (!fs_ok) {
        char log[512];
        glGetShaderInfoLog(fs, sizeof(log), NULL, log);
        printf("FAIL: fragment shader compile: %s\n", log);
        free(buf);
        OSMesaDestroyContext(ctx);
        return 1;
    }
    printf("OK: fragment shader compiled\n");

    /* Link program */
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint link_ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &link_ok);
    if (!link_ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        printf("FAIL: program link: %s\n", log);
        free(buf);
        OSMesaDestroyContext(ctx);
        return 1;
    }
    printf("OK: program linked\n");

    glUseProgram(prog);

    /* Set up vertex buffer */
    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(tri_vertices), tri_vertices, GL_STATIC_DRAW);

    /* Enable vertex attrib array — attribute 0 for vec2 positions */
    GLint pos_loc = glGetAttribLocation(prog, "gl_Vertex");
    /* gl_Vertex is a built-in; on Mesa it's typically at location 0.
     * If glGetAttribLocation returns -1, try 0 directly. */
    if (pos_loc < 0) {
        printf("NOTE: glGetAttribLocation(gl_Vertex) = %d, using 0\n", pos_loc);
        pos_loc = 0;
    }
    glEnableVertexAttribArray(pos_loc);
    glVertexAttribPointer(pos_loc, 2, GL_FLOAT, GL_FALSE, 0, 0);

    /* Draw the triangle */
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    printf("OK: glDrawArrays(GL_TRIANGLES, 0, 3) + glFinish\n");

    /* Verify pixels:
     *   Center (W/2, H/2) should be inside the triangle → TRI color
     *   Corner (5, 5) should be outside → BG color
     *   Upper-center (W/2, 40): NDC y ≈ +0.69, between the apex (0, 0.8)
     *   and the base (-0.8) — inside the triangle → TRI color. (Pixel
     *   (W/2, H-5) would be NDC y ≈ -0.969, BELOW the base edge at -0.8:
     *   outside. It is not sampled. Label corrected — review YKBwf.)
     */
    failures += check_pixel(buf, W / 2, H / 2,         TRI_R, TRI_G, TRI_B, "center-inside");
    failures += check_pixel(buf, 5, 5,                  BG_R,  BG_G,  BG_B,  "corner-outside");
    failures += check_pixel(buf, W / 2, 40,             TRI_R, TRI_G, TRI_B, "upper-inside");

    if (failures == 0) {
        printf("RESULT: PASS — triangle rendered correctly (3/3 pixels match)\n");
    } else {
        printf("RESULT: FAIL — %d pixel check(s) failed\n", failures);
    }

    /* Cleanup */
    glDeleteBuffers(1, &vbo);
    glDeleteProgram(prog);
    glDeleteShader(fs);
    glDeleteShader(vs);
    free(buf);
    OSMesaDestroyContext(ctx);
    return failures ? 1 : 0;
}
