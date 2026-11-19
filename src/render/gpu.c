// Copyright (c) lezginskiy <xanother-x12@proton.me>
// Copyright (c) fluent1s (akemi) <m1st1kad3ad1y@gmail.com>
// SPDX-License-Identifier: MIT

#include "gpu.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>

#include <gbm.h>

// 'ABGR', 'ARGB' and 'XRGB', the fourcc tags the bo and its gl import
// share; raw values keep the build free of libdrm include paths
#define X12_BO_ABGR8888 0x52474241u
#define X12_BO_ARGB8888 0x42475241u
#define X12_BO_XRGB8888 0x34325258u

// the gl canvas engine: the composite pass blends client buffers into a
// gpu canvas with the same integer premultiplied math the cpu path uses,
// so pixels stay bit exact across both paths. every entry point resolves
// through dlopen at init, the binary carries no hard gl dependency - a
// box without libegl runs the cpu blend path and nothing else changes

#define X12_EGL_SYMS(X)                                                        \
    X(p_eglGetProcAddress, "eglGetProcAddress",                                \
      __eglMustCastToProperFunctionPointerType, (const char *))                \
    X(p_eglGetDisplay, "eglGetDisplay", EGLDisplay,                            \
      (EGLNativeDisplayType))                                                  \
    X(p_eglInitialize, "eglInitialize", EGLBoolean,                            \
      (EGLDisplay, EGLint *, EGLint *))                                        \
    X(p_eglChooseConfig, "eglChooseConfig", EGLBoolean,                        \
      (EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *))             \
    X(p_eglCreateContext, "eglCreateContext", EGLContext,                      \
      (EGLDisplay, EGLConfig, EGLContext, const EGLint *))                     \
    X(p_eglCreatePbufferSurface, "eglCreatePbufferSurface", EGLSurface,        \
      (EGLDisplay, EGLConfig, const EGLint *))                                 \
    X(p_eglMakeCurrent, "eglMakeCurrent", EGLBoolean,                          \
      (EGLDisplay, EGLSurface, EGLSurface, EGLContext))                        \
    X(p_eglQueryString, "eglQueryString", const char *,                        \
      (EGLDisplay, EGLint))                                                    \
    X(p_eglTerminate, "eglTerminate", EGLBoolean, (EGLDisplay))

#define X12_GL_SYMS(X)                                                         \
    X(p_glCreateShader, "glCreateShader", GLuint, (GLenum))                    \
    X(p_glShaderSource, "glShaderSource", void,                                \
      (GLuint, GLsizei, const char *const *, const GLint *))                   \
    X(p_glCompileShader, "glCompileShader", void, (GLuint))                    \
    X(p_glGetShaderiv, "glGetShaderiv", void,                                  \
      (GLuint, GLenum, GLint *))                                               \
    X(p_glGetShaderInfoLog, "glGetShaderInfoLog", void,                        \
      (GLuint, GLsizei, GLsizei *, char *))                                    \
    X(p_glDeleteShader, "glDeleteShader", void, (GLuint))                      \
    X(p_glCreateProgram, "glCreateProgram", GLuint, (void))                    \
    X(p_glAttachShader, "glAttachShader", void, (GLuint, GLuint))              \
    X(p_glLinkProgram, "glLinkProgram", void, (GLuint))                        \
    X(p_glGetProgramiv, "glGetProgramiv", void,                                \
      (GLuint, GLenum, GLint *))                                               \
    X(p_glGetProgramInfoLog, "glGetProgramInfoLog", void,                      \
      (GLuint, GLsizei, GLsizei *, char *))                                    \
    X(p_glDeleteProgram, "glDeleteProgram", void, (GLuint))                    \
    X(p_glGetUniformLocation, "glGetUniformLocation", GLint,                    \
      (GLuint, const char *))                                                  \
    X(p_glUseProgram, "glUseProgram", void, (GLuint))                          \
    X(p_glUniform1i, "glUniform1i", void, (GLint, GLint))                      \
    X(p_glUniform1f, "glUniform1f", void, (GLint, GLfloat))                    \
    X(p_glUniform2i, "glUniform2i", void, (GLint, GLint, GLint))               \
    X(p_glUniform4i, "glUniform4i", void,                                      \
      (GLint, GLint, GLint, GLint, GLint))                                     \
    X(p_glUniform4f, "glUniform4f", void,                                      \
      (GLint, GLfloat, GLfloat, GLfloat, GLfloat))                             \
    X(p_glGenVertexArrays, "glGenVertexArrays", void, (GLsizei, GLuint *))     \
    X(p_glBindVertexArray, "glBindVertexArray", void, (GLuint))                \
    X(p_glDeleteVertexArrays, "glDeleteVertexArrays", void,                    \
      (GLsizei, const GLuint *))                                               \
    X(p_glGenFramebuffers, "glGenFramebuffers", void, (GLsizei, GLuint *))     \
    X(p_glBindFramebuffer, "glBindFramebuffer", void, (GLenum, GLuint))        \
    X(p_glDeleteFramebuffers, "glDeleteFramebuffers", void,                    \
      (GLsizei, const GLuint *))                                               \
    X(p_glFramebufferRenderbuffer, "glFramebufferRenderbuffer", void,          \
      (GLenum, GLenum, GLenum, GLuint))                                        \
    X(p_glFramebufferTexture2D, "glFramebufferTexture2D", void,                \
      (GLenum, GLenum, GLenum, GLuint, GLint))                                 \
    X(p_glGenRenderbuffers, "glGenRenderbuffers", void, (GLsizei, GLuint *))   \
    X(p_glBindRenderbuffer, "glBindRenderbuffer", void, (GLenum, GLuint))      \
    X(p_glDeleteRenderbuffers, "glDeleteRenderbuffers", void,                  \
      (GLsizei, const GLuint *))                                               \
    X(p_glRenderbufferStorage, "glRenderbufferStorage", void,                  \
      (GLenum, GLenum, GLsizei, GLsizei))                                      \
    X(p_glGenTextures, "glGenTextures", void, (GLsizei, GLuint *))             \
    X(p_glBindTexture, "glBindTexture", void, (GLenum, GLuint))                \
    X(p_glDeleteTextures, "glDeleteTextures", void, (GLsizei, const GLuint *)) \
    X(p_glTexImage2D, "glTexImage2D", void,                                    \
      (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,          \
       const void *))                                                          \
    X(p_glTexSubImage2D, "glTexSubImage2D", void,                              \
      (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,          \
       const void *))                                                          \
    X(p_glTexParameteri, "glTexParameteri", void, (GLenum, GLenum, GLint))     \
    X(p_glPixelStorei, "glPixelStorei", void, (GLenum, GLint))                 \
    X(p_glCopyTexSubImage2D, "glCopyTexSubImage2D", void,                      \
      (GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei))           \
    X(p_glViewport, "glViewport", void, (GLint, GLint, GLsizei, GLsizei))      \
    X(p_glReadPixels, "glReadPixels", void,                                    \
      (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))                \
    X(p_glActiveTexture, "glActiveTexture", void, (GLenum))                    \
    X(p_glDrawArrays, "glDrawArrays", void, (GLenum, GLint, GLsizei))          \
    X(p_glFinish, "glFinish", void, (void))

#define X12_SYM_PTR(name, raw, ret, params) static ret (*name) params;
X12_EGL_SYMS(X12_SYM_PTR)
X12_GL_SYMS(X12_SYM_PTR)
#undef X12_SYM_PTR

#define X12_GBM_SYMS(X)                                                        \
    X(p_gbm_create_device, "gbm_create_device", struct gbm_device *, (int))    \
    X(p_gbm_device_destroy, "gbm_device_destroy", void,                        \
      (struct gbm_device *))                                                   \
    X(p_gbm_bo_create, "gbm_bo_create", struct gbm_bo *,                       \
      (struct gbm_device *, uint32_t, uint32_t, uint32_t, uint32_t))           \
    X(p_gbm_bo_get_fd, "gbm_bo_get_fd", int, (struct gbm_bo *))                \
    X(p_gbm_bo_get_stride, "gbm_bo_get_stride", uint32_t, (struct gbm_bo *))   \
    X(p_gbm_bo_destroy, "gbm_bo_destroy", void, (struct gbm_bo *))

#define X12_SYM_PTR(name, raw, ret, params) static ret (*name) params;
X12_GBM_SYMS(X12_SYM_PTR)
#undef X12_SYM_PTR

// older libgbm carries no modifier allocation, the plain create keeps
// the present alive there
static struct gbm_bo *(*p_gbm_bo_create_with_modifiers2)(
    struct gbm_device *, uint32_t, uint32_t, uint32_t, const uint64_t *,
    const unsigned int, uint32_t);

// optional entry points: their absence degrades specific paths rather
// than the engine
static EGLImageKHR (*p_eglCreateImage)(EGLDisplay, EGLContext, EGLenum,
                                       EGLClientBuffer, const EGLint *);
static EGLImageKHR (*p_eglCreateImageKHR)(EGLDisplay, EGLContext, EGLenum,
                                          EGLClientBuffer, const EGLint *);
static EGLBoolean (*p_eglDestroyImage)(EGLDisplay, EGLImageKHR);
// the egl image texture hook lives in the es extension headers some
// installs trim, the plain signature carries it
typedef void (*x12_gl_image_tex2d)(GLenum, void *);
static x12_gl_image_tex2d p_glEGLImageTargetTexture2DOES;

static void *lib_egl, *lib_gl, *lib_gbm;
static struct gbm_device *gbm_dev;
static int gbm_fd = -1;
static EGLDisplay dpy;
static EGLContext ctx;
static EGLSurface surf;
static int ready;

static uint32_t canvas_w, canvas_h;
static GLuint prog, fill_prog, copy_prog, vao;
static GLuint out_rb, dst_tex, src_tex;
static GLint u_opaque, u_rect, u_origin, u_h;
static GLint u_src_size, u_scale_f, u_view;
static GLuint fbo, copy_fbo;
static GLint u_src, u_dst, u_rect_fill;
static GLint u_copy_swap, u_copy_src, u_copy_rect;
static uint32_t src_w_cur, src_h_cur;

// one pair of scanout bos per present setup, the composite folds into
// the back one and the backend flips it to the crtc
struct x12_pres_bo {
    struct gbm_bo *bo;
    EGLImageKHR img;
    GLuint tex;
    uint32_t stride;
    int swap;
};
static struct x12_pres_bo pres[2];
static uint32_t pres_w, pres_h;

static const char *vert_src =
    "#version 300 es\n"
    "uniform vec4 u_rect;\n"
    "void main() {\n"
    "    vec2 p = vec2(gl_VertexID == 1 || gl_VertexID == 2 ? 1.0 : 0.0,\n"
    "                  gl_VertexID == 2 || gl_VertexID == 3 ? 1.0 : 0.0);\n"
    "    gl_Position = vec4(u_rect.x + p.x * u_rect.z,\n"
    "                       u_rect.y + p.y * u_rect.w, 0.0, 1.0);\n"
    "}\n";

// the blend math is the cpu contract in uint form: premultiplied add
// with truncating division - float division would round differently
// and the contract test would catch it immediately
static const char *frag_src =
    "#version 300 es\n"
    "precision highp usampler2D;\n"
    "precision highp float;\n"
    "uniform usampler2D u_src;\n"
    "uniform usampler2D u_dst;\n"
    "uniform int u_opaque;\n"
    "uniform ivec2 u_origin;\n"
    "uniform ivec2 u_src_size;\n"
    "uniform ivec4 u_view;\n"
    "uniform float u_scale;\n"
    "uniform float u_h;\n"
    "out uvec4 color;\n"
    "void main() {\n"
    "    ivec2 c = ivec2(int(gl_FragCoord.x), int(gl_FragCoord.y));\n"
    "    uvec4 d = texelFetch(u_dst, c, 0);\n"
    "    // the canvas is top down, the frag coord bottom up\n"
    "    int yt = int(u_h) - 1 - c.y;\n"
    "    // the view rect bounds the blend like the cpu path clips it,\n"
    "    // everything outside keeps the old content\n"
    "    if (c.x < u_view.x || yt < u_view.y ||\n"
    "        c.x >= u_view.x + u_view.z || yt >= u_view.y + u_view.w) {\n"
    "        color = d;\n"
    "        return;\n"
    "    }\n"
    "    ivec2 t = ivec2(float(c.x - u_origin.x) * u_scale,\n"
    "                    float(yt - u_origin.y) * u_scale);\n"
    "    if (t.x < 0 || t.y < 0 || t.x >= u_src_size.x ||\n"
    "        t.y >= u_src_size.y) {\n"
    "        color = d;\n"
    "        return;\n"
    "    }\n"
    "    uvec4 s = texelFetch(u_src, t, 0);\n"
    "    uint a = u_opaque == 1 ? 255u : s.a;\n"
    "    uint inv = 255u - a;\n"
    "    uvec3 o = s.rgb + d.rgb * inv / 255u;\n"
    "    uint oa = a + d.a * inv / 255u;\n"
    "    color = uvec4(o, oa);\n"
    "}\n";

static const char *fill_src =
    "#version 300 es\n"
    "precision highp float;\n"
    "out uvec4 color;\n"
    "void main() { color = uvec4(0u, 0u, 0u, 255u); }\n";

// the canvas holds wire bytes in fixed point integer form, the scanout
// pass renders them unorm; v/255.0 normalized and rounded back is the
// same byte, so the flip shows the composite exactly. the swap picks
// the channel order the bo format implies for the gl import
static const char *copy_src =
    "#version 300 es\n"
    "precision highp usampler2D;\n"
    "precision highp float;\n"
    "uniform usampler2D u_canvas;\n"
    "uniform int u_swap;\n"
    "out vec4 color;\n"
    "void main() {\n"
    "    ivec2 c = ivec2(int(gl_FragCoord.x), int(gl_FragCoord.y));\n"
    "    uvec4 v = texelFetch(u_canvas, c, 0);\n"
    "    vec3 rgb = u_swap == 1 ? vec3(v.bgr) : vec3(v.rgb);\n"
    "    color = vec4(rgb, float(v.a)) / 255.0;\n"
    "}\n";

static GLuint make_shader(GLenum type, const char *src) {
    GLuint sh = p_glCreateShader(type);
    p_glShaderSource(sh, 1, &src, 0);
    p_glCompileShader(sh);
    GLint ok = 0;
    p_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        p_glGetShaderInfoLog(sh, sizeof log, 0, log);
        fprintf(stderr, "gpu engine: shader compile failed: %s\n", log);
        p_glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint make_program(const char *vs_src, const char *fs_src) {
    GLuint vs = make_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = make_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) {
        return 0;
    }
    GLuint p = p_glCreateProgram();
    p_glAttachShader(p, vs);
    p_glAttachShader(p, fs);
    p_glLinkProgram(p);
    p_glDeleteShader(vs);
    p_glDeleteShader(fs);
    GLint ok = 0;
    p_glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        p_glGetProgramInfoLog(p, sizeof log, 0, log);
        fprintf(stderr, "gpu engine: link failed: %s\n", log);
        return 0;
    }
    return p;
}

// a missing entry point means the installed gl stack does not match the
// headers this file was built against, the engine refuses to run rather
// than calling a null pointer mid frame
static int resolve_syms(void) {
    lib_egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    lib_gl = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!lib_egl || !lib_gl) {
        return 0;
    }
#define X12_SYM_LOAD_EGL(name, raw, ret, params)                               \
    name = (ret(*)params)dlsym(lib_egl, raw);                                  \
    if (!name) {                                                               \
        return 0;                                                              \
    }
#define X12_SYM_LOAD_GL(name, raw, ret, params)                                \
    name = (ret(*)params)dlsym(lib_gl, raw);                                   \
    if (!name) {                                                               \
        return 0;                                                              \
    }
    X12_EGL_SYMS(X12_SYM_LOAD_EGL)
    X12_GL_SYMS(X12_SYM_LOAD_GL)
#undef X12_SYM_LOAD_EGL
#undef X12_SYM_LOAD_GL
    p_eglCreateImage = (EGLImageKHR(*)(EGLDisplay, EGLContext, EGLenum,
                                       EGLClientBuffer, const EGLint *))
                           dlsym(lib_egl, "eglCreateImage");
    // glvnd does not export the khr entry point to dlsym, the proc
    // address hook hands it through with the display bound
    p_eglCreateImageKHR = (EGLImageKHR(*)(EGLDisplay, EGLContext, EGLenum,
                                          EGLClientBuffer, const EGLint *))
                              p_eglGetProcAddress("eglCreateImageKHR");
    p_eglDestroyImage = (EGLBoolean(*)(EGLDisplay, EGLImageKHR)) dlsym(
        lib_egl, "eglDestroyImage");
    p_glEGLImageTargetTexture2DOES =
        (x12_gl_image_tex2d)p_eglGetProcAddress("glEGLImageTargetTexture2DOES");
    return 1;
}

// the dma import hooks of the gbm present ride here, their absence
// degrades the present to the cpu path only
static int resolve_gbm(void) {
    lib_gbm = dlopen("libgbm.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib_gbm) {
        return 0;
    }
#define X12_SYM_LOAD_GBM(name, raw, ret, params)                               \
    name = (ret(*)params)dlsym(lib_gbm, raw);                                  \
    if (!name) {                                                               \
        return 0;                                                              \
    }
    X12_GBM_SYMS(X12_SYM_LOAD_GBM)
#undef X12_SYM_LOAD_GBM
    p_gbm_bo_create_with_modifiers2 = (struct gbm_bo *(*)(
        struct gbm_device *, uint32_t, uint32_t, uint32_t, const uint64_t *,
        const unsigned int, uint32_t))dlsym(lib_gbm,
                                            "gbm_bo_create_with_modifiers2");
    p_eglCreateImage = (EGLImageKHR(*)(EGLDisplay, EGLContext, EGLenum,
                                       EGLClientBuffer, const EGLint *))
                           dlsym(lib_egl, "eglCreateImage");
    p_eglCreateImageKHR = (EGLImageKHR(*)(EGLDisplay, EGLContext, EGLenum,
                                          EGLClientBuffer, const EGLint *))
                              p_eglGetProcAddress("eglCreateImageKHR");
    p_eglDestroyImage = (EGLBoolean(*)(EGLDisplay, EGLImageKHR)) dlsym(
        lib_egl, "eglDestroyImage");
    p_glEGLImageTargetTexture2DOES =
        (x12_gl_image_tex2d)p_eglGetProcAddress("glEGLImageTargetTexture2DOES");
    return p_eglCreateImage && p_eglDestroyImage &&
           p_glEGLImageTargetTexture2DOES;
}

// drives one display through config, context and make current, programs
// included. any failure terminates the display, the caller tries its
// next candidate
static int build_context(EGLDisplay d) {
    if (!p_eglInitialize(d, 0, 0)) {
        fprintf(stderr, "gpu engine: egl initialize failed\n");
        return 0;
    }
    static const EGLint ctx_attr[] = {
        EGL_CONTEXT_MAJOR_VERSION,
        3,
        EGL_CONTEXT_MINOR_VERSION,
        0,
        EGL_NONE,
    };
    static const EGLint cfg_attr[] = {
        // surface type zero matches every config, the gbm platform has
        // no pbuffer configs and the context goes surfaceless there
        EGL_SURFACE_TYPE,
        0,
        EGL_RENDERABLE_TYPE,
        EGL_OPENGL_ES3_BIT,
        EGL_NONE,
    };
    EGLConfig cfg;
    EGLint n = 0;
    if (!p_eglChooseConfig(d, cfg_attr, &cfg, 1, &n) || n < 1) {
        fprintf(stderr, "gpu engine: no config\n");
        goto fail;
    }
    ctx = p_eglCreateContext(d, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "gpu engine: no context\n");
        goto fail;
    }
    static const EGLint pb_attr[] = {
        EGL_WIDTH,
        16,
        EGL_HEIGHT,
        16,
        EGL_NONE,
    };
    surf = p_eglCreatePbufferSurface(d, cfg, pb_attr);
    if (surf != EGL_NO_SURFACE &&
        !p_eglMakeCurrent(d, surf, surf, ctx) &&
        strstr(p_eglQueryString(d, EGL_EXTENSIONS),
               "EGL_KHR_surfaceless_context")) {
        surf = EGL_NO_SURFACE;
    }
    if (!p_eglMakeCurrent(d, surf, surf, ctx)) {
        fprintf(stderr, "gpu engine: no current\n");
        goto fail;
    }
    prog = make_program(vert_src, frag_src);
    fill_prog = make_program(vert_src, fill_src);
    copy_prog = make_program(vert_src, copy_src);
    if (!prog || !fill_prog || !copy_prog) {
        goto fail;
    }
    u_src = p_glGetUniformLocation(prog, "u_src");
    u_dst = p_glGetUniformLocation(prog, "u_dst");
    u_opaque = p_glGetUniformLocation(prog, "u_opaque");
    u_rect = p_glGetUniformLocation(prog, "u_rect");
    u_origin = p_glGetUniformLocation(prog, "u_origin");
    u_h = p_glGetUniformLocation(prog, "u_h");
    u_src_size = p_glGetUniformLocation(prog, "u_src_size");
    u_scale_f = p_glGetUniformLocation(prog, "u_scale");
    u_view = p_glGetUniformLocation(prog, "u_view");
    u_rect_fill = p_glGetUniformLocation(fill_prog, "u_rect");
    u_copy_swap = p_glGetUniformLocation(copy_prog, "u_swap");
    u_copy_src = p_glGetUniformLocation(copy_prog, "u_canvas");
    u_copy_rect = p_glGetUniformLocation(copy_prog, "u_rect");
    p_glGenVertexArrays(1, &vao);
    p_glBindVertexArray(vao);
    p_glGenFramebuffers(1, &copy_fbo);
    ready = 1;
    return 1;
fail:
    p_eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    p_eglTerminate(d);
    ctx = EGL_NO_CONTEXT;
    surf = EGL_NO_SURFACE;
    prog = fill_prog = copy_prog = 0;
    return 0;
}

int x12_gpu_init(const char *node) {
    if (!resolve_syms()) {
        fprintf(stderr, "gpu engine: no usable gl stack, cpu blend path\n");
        return 0;
    }
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC) p_eglGetProcAddress(
            "eglGetPlatformDisplayEXT");
    // the gbm display of the render node puts the context on the same
    // device the scanout bos allocate on, a foreign context would
    // refuse their import; the surfaceless mesa display is the headless
    // path and the fallback
    if (node && resolve_gbm()) {
        gbm_fd = open(node, O_RDWR | O_CLOEXEC);
        if (gbm_fd >= 0) {
            gbm_dev = p_gbm_create_device(gbm_fd);
        }
        if (gbm_dev && get_platform) {
            dpy = get_platform(EGL_PLATFORM_GBM_MESA, gbm_dev, NULL);
            if (dpy != EGL_NO_DISPLAY && build_context(dpy)) {
                fprintf(stderr, "gpu engine: context on %s\n", node);
                return 1;
            }
            dpy = EGL_NO_DISPLAY;
        }
        if (gbm_dev) {
            p_gbm_device_destroy(gbm_dev);
            gbm_dev = 0;
        }
        if (gbm_fd >= 0) {
            close(gbm_fd);
            gbm_fd = -1;
        }
    }
    if (get_platform) {
        dpy = get_platform(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY,
                           NULL);
    }
    if (dpy == EGL_NO_DISPLAY) {
        dpy = p_eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }
    if (dpy == EGL_NO_DISPLAY) {
        return 0;
    }
    return build_context(dpy);
}

void x12_gpu_shutdown(void) {
    if (!ready) {
        return;
    }
    x12_gpu_present_shutdown();
    p_glDeleteRenderbuffers(1, &out_rb);
    p_glDeleteFramebuffers(1, &fbo);
    p_glDeleteFramebuffers(1, &copy_fbo);
    p_glDeleteTextures(1, &dst_tex);
    p_glDeleteTextures(1, &src_tex);
    p_glDeleteProgram(prog);
    p_glDeleteProgram(fill_prog);
    p_glDeleteProgram(copy_prog);
    p_glDeleteVertexArrays(1, &vao);
    p_eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    p_eglTerminate(dpy);
    if (gbm_dev) {
        p_gbm_device_destroy(gbm_dev);
        gbm_dev = 0;
    }
    if (gbm_fd >= 0) {
        close(gbm_fd);
        gbm_fd = -1;
    }
    if (lib_gbm) {
        dlclose(lib_gbm);
        lib_gbm = 0;
    }
    dlclose(lib_egl);
    dlclose(lib_gl);
    lib_egl = lib_gl = 0;
    canvas_w = canvas_h = 0;
    fbo = 0;
    ready = 0;
}

int x12_gpu_ready(void) {
    return ready;
}

int x12_gpu_canvas(uint32_t w, uint32_t h) {
    if (!ready || !w || !h || w > 8192 || h > 8192) {
        return 0;
    }
    if (canvas_w == w && canvas_h == h) {
        return 1;
    }
    canvas_w = w;
    canvas_h = h;
    src_w_cur = src_h_cur = 0;
    if (!fbo) {
        p_glGenFramebuffers(1, &fbo);
        p_glGenRenderbuffers(1, &out_rb);
        p_glGenTextures(1, &dst_tex);
        p_glGenTextures(1, &src_tex);
        p_glUseProgram(prog);
        p_glUniform1i(u_dst, 1);
        p_glUniform1i(u_src, 0);
    }
    p_glBindRenderbuffer(GL_RENDERBUFFER, out_rb);
    p_glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8UI, w, h);
    p_glBindTexture(GL_TEXTURE_2D, dst_tex);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, w, h, 0, GL_RGBA_INTEGER,
                   GL_UNSIGNED_BYTE, 0);
    p_glBindTexture(GL_TEXTURE_2D, src_tex);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return 1;
}

// folds the rect the pass just painted back into the history texture,
// so the next blend of the same frame stacks over it the way the cpu
// path accumulates fill and views; the sampler lets go during the copy,
// a texture sampled and written in one stream is undefined
static void commit_rect(const struct x12_rect *r) {
    int32_t x0 = r->x < 0 ? 0 : r->x;
    int32_t y0 = r->y < 0 ? 0 : r->y;
    int32_t x1 = r->x + (int32_t)r->w > (int32_t)canvas_w
                     ? (int32_t)canvas_w
                     : r->x + (int32_t)r->w;
    int32_t y1 = r->y + (int32_t)r->h > (int32_t)canvas_h
                     ? (int32_t)canvas_h
                     : r->y + (int32_t)r->h;
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    p_glActiveTexture(GL_TEXTURE1);
    p_glBindTexture(GL_TEXTURE_2D, 0);
    p_glBindTexture(GL_TEXTURE_2D, dst_tex);
    p_glCopyTexSubImage2D(GL_TEXTURE_2D, 0, x0, (GLint)(canvas_h - y1), x0,
                          (GLint)(canvas_h - y1), x1 - x0, y1 - y0);
}

int x12_gpu_blend(const struct x12_view *view, const struct x12_bufref *ref,
                  uint32_t scale, const struct x12_rect *rects, int n_rects) {
    if (!ready || !canvas_w || !view || !ref->pixels) {
        return 0;
    }
    if (scale == 0) {
        scale = 256;
    }
    uint32_t src_w = ref->width, src_h = ref->height;
    p_glBindTexture(GL_TEXTURE_2D, src_tex);
    p_glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(ref->stride / 4));
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    // same size uploads ride tex sub image, a fresh allocation only
    // happens when the view changed its buffer dimensions
    if (src_w != src_w_cur || src_h != src_h_cur) {
        p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, (GLsizei)src_w,
                       (GLsizei)src_h, 0, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE,
                       ref->pixels);
        src_w_cur = src_w;
        src_h_cur = src_h;
    } else {
        p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)src_w,
                          (GLsizei)src_h, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE,
                          ref->pixels);
    }
    p_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_RENDERBUFFER, out_rb);
    p_glViewport(0, 0, (GLsizei)canvas_w, (GLsizei)canvas_h);
    p_glUseProgram(prog);
    p_glUniform1f(u_h, (float)canvas_h);
    p_glActiveTexture(GL_TEXTURE0);
    p_glBindTexture(GL_TEXTURE_2D, src_tex);
    p_glActiveTexture(GL_TEXTURE1);
    p_glBindTexture(GL_TEXTURE_2D, dst_tex);
    p_glUniform1i(u_opaque, ref->format == X12_FORMAT_ARGB8888 ? 0 : 1);
    p_glUniform2i(u_origin, view->x, view->y);
    p_glUniform2i(u_src_size, (int)src_w, (int)src_h);
    p_glUniform4i(u_view, view->x, view->y, (int)view->w, (int)view->h);
    p_glUniform1f(u_scale_f, (float)scale / 256.0f);
    for (int i = 0; i < n_rects; i++) {
        const struct x12_rect *r = &rects[i];
        p_glUniform4f(u_rect, (float)r->x * 2.0f / canvas_w - 1.0f,
                      1.0f - (float)(r->y + r->h) * 2.0f / canvas_h,
                      (float)r->w * 2.0f / canvas_w,
                      (float)r->h * 2.0f / canvas_h);
        p_glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        commit_rect(r);
    }
    return 1;
}

void x12_gpu_fill(const struct x12_rect *rects, int n_rects) {
    if (!ready || !canvas_w) {
        return;
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_RENDERBUFFER, out_rb);
    p_glViewport(0, 0, (GLsizei)canvas_w, (GLsizei)canvas_h);
    p_glUseProgram(fill_prog);
    for (int i = 0; i < n_rects; i++) {
        const struct x12_rect *r = &rects[i];
        p_glUniform4f(u_rect_fill, (float)r->x * 2.0f / canvas_w - 1.0f,
                      1.0f - (float)(r->y + r->h) * 2.0f / canvas_h,
                      (float)r->w * 2.0f / canvas_w,
                      (float)r->h * 2.0f / canvas_h);
        p_glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        commit_rect(r);
    }
}

void x12_gpu_dst_commit(void) {
    // the same format both sides, the copy rides the gpu; the sampler
    // of the destination lets go during the copy, a texture sampled and
    // written in one stream is undefined
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_RENDERBUFFER, out_rb);
    p_glActiveTexture(GL_TEXTURE1);
    p_glBindTexture(GL_TEXTURE_2D, 0);
    p_glBindTexture(GL_TEXTURE_2D, dst_tex);
    p_glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, (GLsizei)canvas_w,
                          (GLsizei)canvas_h);
}

void x12_gpu_readback(uint8_t *out, uint32_t stride_bytes) {
    if (!ready || !canvas_w) {
        return;
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_RENDERBUFFER, out_rb);
    for (uint32_t y = 0; y < canvas_h; y++) {
        // the framebuffer reads bottom up, the canvas is top down
        p_glReadPixels(0, (GLint)(canvas_h - 1 - y), (GLsizei)canvas_w, 1,
                       GL_RGBA_INTEGER, GL_UNSIGNED_BYTE,
                       out + (size_t)y * stride_bytes);
    }
}

void x12_gpu_readback_band(uint8_t *out, uint32_t stride_bytes, uint32_t y0,
                           uint32_t y1) {
    if (!ready || !canvas_w || y1 > canvas_h || y1 <= y0) {
        return;
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_RENDERBUFFER, out_rb);
    uint32_t bh = y1 - y0;
    uint8_t *tmp = malloc((size_t)canvas_w * bh * 4);
    if (!tmp) {
        return;
    }
    // one transfer per band, the rows come back bottom up and land
    // flipped so the caller keeps its top down framebuffer layout
    p_glReadPixels(0, (GLint)(canvas_h - y1), (GLsizei)canvas_w, (GLsizei)bh,
                   GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, tmp);
    for (uint32_t i = 0; i < bh; i++) {
        memcpy(out + (size_t)(y1 - 1 - i) * stride_bytes,
               tmp + (size_t)i * canvas_w * 4, (size_t)canvas_w * 4);
    }
    free(tmp);
}

// the scanout pass target and state, the attachment rides with the
// caller, the draw only carries the program and its uniforms
static void unorm_draw(int swap) {
    p_glViewport(0, 0, (GLsizei)canvas_w, (GLsizei)canvas_h);
    p_glUseProgram(copy_prog);
    // the shared vertex shader needs the full canvas quad
    p_glUniform4f(u_copy_rect, -1.0f, -1.0f, 2.0f, 2.0f);
    p_glUniform1i(u_copy_swap, swap);
    p_glUniform1i(u_copy_src, 0);
    p_glActiveTexture(GL_TEXTURE0);
    p_glBindTexture(GL_TEXTURE_2D, dst_tex);
    p_glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

int x12_gpu_unorm_probe(uint8_t *out, uint32_t stride_bytes) {
    if (!ready || !canvas_w) {
        return 0;
    }
    GLuint rb, fbo_probe;
    p_glGenRenderbuffers(1, &rb);
    p_glBindRenderbuffer(GL_RENDERBUFFER, rb);
    p_glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, (GLsizei)canvas_w,
                            (GLsizei)canvas_h);
    p_glGenFramebuffers(1, &fbo_probe);
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_probe);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_RENDERBUFFER, rb);
    unorm_draw(0);
    for (uint32_t y = 0; y < canvas_h; y++) {
        p_glReadPixels(0, (GLint)(canvas_h - 1 - y), (GLsizei)canvas_w, 1,
                       GL_RGBA, GL_UNSIGNED_BYTE,
                       out + (size_t)y * stride_bytes);
    }
    p_glDeleteFramebuffers(1, &fbo_probe);
    p_glDeleteRenderbuffers(1, &rb);
    return 1;
}

int x12_gpu_present_setup(uint32_t w, uint32_t h, int fds[2],
                          uint32_t strides[2]) {
    if (!ready || !gbm_dev || !w || !h || w > 8192 || h > 8192 ||
        w != canvas_w || h != canvas_h) {
        return 0;
    }
    // a fresh pair every call, the dma fds belong to the caller the
    // moment the call succeeds and the engine never closes them
    x12_gpu_present_shutdown();
    for (int b = 0; b < 2; b++) {
        struct x12_pres_bo *pb = &pres[b];
        pb->swap = 0;
        fds[b] = -1;
        if (p_gbm_bo_create_with_modifiers2) {
            pb->bo = p_gbm_bo_create_with_modifiers2(
                gbm_dev, w, h, X12_BO_ABGR8888, NULL, 0,
                GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
        }
        if (!pb->bo) {
            pb->bo = p_gbm_bo_create(gbm_dev, w, h, GBM_BO_FORMAT_ARGB8888,
                                     GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
            pb->swap = 1;
        }
        if (!pb->bo) {
            goto fail;
        }
        pb->stride = p_gbm_bo_get_stride(pb->bo);
        int fd = p_gbm_bo_get_fd(pb->bo);
        if (fd < 0) {
            goto fail;
        }
        EGLint attrs[13] = {
            EGL_WIDTH,
            (EGLint)w,
            EGL_HEIGHT,
            (EGLint)h,
            EGL_LINUX_DRM_FOURCC_EXT,
            (EGLint)(pb->swap ? X12_BO_ARGB8888 : X12_BO_ABGR8888),
            EGL_DMA_BUF_PLANE0_FD_EXT,
            fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT,
            0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT,
            (EGLint)pb->stride,
            EGL_NONE,
        };
        // the khr hook goes first when present, some stacks return
        // bad parameter through the 1.5 core path for attributes the
        // same driver accepts through khr; a rejected alpha fourcc
        // falls back to its xrgb twin, the memory layout is identical
        // and the scanout fb carries no alpha anyway
        EGLint fourcc_a[2] = {
            (EGLint)(pb->swap ? X12_BO_ARGB8888 : X12_BO_ABGR8888),
            (EGLint)X12_BO_XRGB8888,
        };
        for (int t = 0; t < 2 && pb->img == EGL_NO_IMAGE_KHR; t++) {
            attrs[5] = fourcc_a[t];
            if (p_eglCreateImageKHR) {
                pb->img = p_eglCreateImageKHR(dpy, EGL_NO_CONTEXT,
                                              EGL_LINUX_DMA_BUF_EXT, NULL,
                                              attrs);
            } else {
                pb->img = p_eglCreateImage(dpy, EGL_NO_CONTEXT,
                                           EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
            }
        }
        if (pb->img != EGL_NO_IMAGE_KHR) {
            // egl takes a reference of its own and never closes the
            // descriptor it was handed, the caller gets a fresh one
            fds[b] = dup(fd);
        }
        close(fd);
        if (pb->img == EGL_NO_IMAGE_KHR) {
            goto fail;
        }
        p_glGenTextures(1, &pb->tex);
        p_glBindTexture(GL_TEXTURE_2D, pb->tex);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, pb->img);
        strides[b] = pb->stride;
    }
    pres_w = w;
    pres_h = h;
    return 1;
fail:
    for (int b = 0; b < 2; b++) {
        if (fds[b] >= 0) {
            close(fds[b]);
            fds[b] = -1;
        }
        if (pres[b].tex) {
            p_glDeleteTextures(1, &pres[b].tex);
            pres[b].tex = 0;
        }
        if (pres[b].img) {
            p_eglDestroyImage(dpy, pres[b].img);
            pres[b].img = 0;
        }
        if (pres[b].bo) {
            p_gbm_bo_destroy(pres[b].bo);
            pres[b].bo = 0;
        }
    }
    pres_w = pres_h = 0;
    return 0;
}

int x12_gpu_present_copy(int idx) {
    if (!ready || idx < 0 || idx > 1 || !pres[idx].bo || !canvas_w ||
        pres_w != canvas_w || pres_h != canvas_h) {
        return 0;
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER, copy_fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, pres[idx].tex, 0);
    unorm_draw(pres[idx].swap);
    // the page flip the backend arms reads the bo straight away, the
    // pass must be on the hardware before the ioctl
    p_glFinish();
    return 1;
}

int x12_gpu_present_readback(int idx, uint8_t *out, uint32_t stride_bytes) {
    if (!ready || idx < 0 || idx > 1 || !pres[idx].bo || !canvas_w) {
        return 0;
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER, copy_fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, pres[idx].tex, 0);
    for (uint32_t y = 0; y < canvas_h; y++) {
        p_glReadPixels(0, (GLint)(canvas_h - 1 - y), (GLsizei)canvas_w, 1,
                       GL_RGBA, GL_UNSIGNED_BYTE,
                       out + (size_t)y * stride_bytes);
    }
    return 1;
}

void x12_gpu_present_shutdown(void) {
    for (int b = 0; b < 2; b++) {
        if (pres[b].tex) {
            p_glDeleteTextures(1, &pres[b].tex);
            pres[b].tex = 0;
        }
        if (pres[b].img) {
            p_eglDestroyImage(dpy, pres[b].img);
            pres[b].img = 0;
        }
        if (pres[b].bo) {
            p_gbm_bo_destroy(pres[b].bo);
            pres[b].bo = 0;
        }
    }
    pres_w = pres_h = 0;
}
