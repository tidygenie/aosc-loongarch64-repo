#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <gbm.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef GL_PRIMITIVE_RESTART_NV
#define GL_PRIMITIVE_RESTART_NV 0x8558
#endif
#ifndef GL_PRIMITIVE_RESTART_INDEX_NV
#define GL_PRIMITIVE_RESTART_INDEX_NV 0x8559
#endif
typedef void (*PrimitiveRestartIndexNVFn)(GLuint);

static void die(const char *message) {
    fprintf(stderr, "PROBE_ERROR %s egl_error=0x%x\n", message, eglGetError());
    exit(1);
}

static int has_extension(const char *extensions, const char *wanted) {
    size_t length = strlen(wanted);
    const char *p = extensions;
    while (p && *p) {
        while (*p == ' ') ++p;
        const char *end = strchr(p, ' ');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n == length && !memcmp(p, wanted, length)) return 1;
        if (!end) break;
        p = end + 1;
    }
    return 0;
}

int main(void) {
    static const char *vendors[] = {
        "/usr/share/glvnd/egl_vendor.d/40_gsgpu.json",
        "/usr/share/glvnd/egl_vendor.d/60_gsgpu.json",
        "/usr/share/glvnd/egl_vendor.d/60_loonggpu.json",
    };
    for (size_t i = 0; i < sizeof(vendors) / sizeof(vendors[0]); ++i) {
        if (access(vendors[i], R_OK) == 0) {
            setenv("__EGL_VENDOR_LIBRARY_FILENAMES", vendors[i], 1);
            printf("EGL_VENDOR %s\n", vendors[i]);
            break;
        }
    }
    setenv("GBM_BACKEND", "gsgpu", 1);
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open renderD128"); return 1; }
    struct gbm_device *gbm = gbm_create_device(fd);
    if (!gbm) die("gbm_create_device");
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!get_display) die("no eglGetPlatformDisplayEXT");
    EGLDisplay display = get_display(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    if (display == EGL_NO_DISPLAY) die("no EGL display");
    EGLint major = 0, minor = 0;
    if (!eglInitialize(display, &major, &minor)) die("eglInitialize");
    printf("EGL_VERSION %d.%d\n", major, minor);
    if (!eglBindAPI(EGL_OPENGL_API)) die("eglBindAPI OpenGL");
    EGLConfig config;
    EGLint config_count = 0;
    if (!eglGetConfigs(display, &config, 1, &config_count) || config_count < 1)
        die("eglGetConfigs");
    struct gbm_surface *gbm_surface = gbm_surface_create(
        gbm, 16, 16, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING);
    if (!gbm_surface) die("gbm_surface_create");
    PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC create_surface =
        (PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC)
        eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT");
    if (!create_surface) die("no eglCreatePlatformWindowSurfaceEXT");
    EGLSurface surface = create_surface(display, config, gbm_surface, NULL);
    if (surface == EGL_NO_SURFACE) die("eglCreatePlatformWindowSurfaceEXT");
    EGLint context_attributes[] = {
        EGL_CONTEXT_MAJOR_VERSION, 2, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE
    };
    EGLContext context = eglCreateContext(
        display, config, EGL_NO_CONTEXT, context_attributes);
    if (context == EGL_NO_CONTEXT) die("eglCreateContext 2.1");
    if (!eglMakeCurrent(display, surface, surface, context)) die("eglMakeCurrent");

    const char *vendor = (const char *)glGetString(GL_VENDOR);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version = (const char *)glGetString(GL_VERSION);
    const char *glsl = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    if (!vendor || !renderer || !version || !glsl) die("glGetString capability");
    printf("GL_VENDOR %s\n", vendor);
    printf("GL_RENDERER %s\n", renderer);
    printf("GL_VERSION %s\n", version);
    printf("GL_SHADING_LANGUAGE_VERSION %s\n", glsl);
    printf("GL_EXTENSIONS %s\n", extensions ? extensions : "(null)");
    if (!extensions) die("GL_EXTENSIONS unavailable on requested compatibility context");

    char *renderer_lower = strdup(renderer);
    char *version_lower = strdup(version);
    if (!renderer_lower || !version_lower) die("strdup");
    for (char *p = renderer_lower; *p; ++p) *p |= 0x20;
    for (char *p = version_lower; *p; ++p) *p |= 0x20;
    if (strstr(renderer_lower, "llvmpipe") || strstr(renderer_lower, "swrast") ||
        strstr(renderer_lower, "softpipe") || strstr(renderer_lower, "software") ||
        strstr(version_lower, "llvmpipe") || strstr(version_lower, "swrast")) {
        fprintf(stderr, "PROBE_ERROR software renderer forbidden\n");
        return 2;
    }
    int ext_gpu_shader4 = has_extension(extensions, "GL_EXT_gpu_shader4");
    unsigned glsl_major = 0, glsl_minor = 0;
    int parsed = sscanf(glsl, "%u.%u", &glsl_major, &glsl_minor);
    int glsl_130 = parsed == 2 && (glsl_major > 1 || (glsl_major == 1 && glsl_minor >= 30));
    printf("CAP_GL_EXT_gpu_shader4 %s\n", ext_gpu_shader4 ? "yes" : "no");
    printf("CAP_GLSL_130 %s\n", glsl_130 ? "yes" : "no");
    int primitive_restart = has_extension(extensions, "GL_NV_primitive_restart");
    printf("CAP_GL_NV_primitive_restart %s\n", primitive_restart ? "yes" : "no");
    if (primitive_restart) {
        PrimitiveRestartIndexNVFn restart_index =
            (PrimitiveRestartIndexNVFn)eglGetProcAddress("glPrimitiveRestartIndexNV");
        printf("PROC_glPrimitiveRestartIndexNV %s\n", restart_index ? "yes" : "no");
        if (!restart_index) die("glPrimitiveRestartIndexNV unavailable");
        glEnable(GL_PRIMITIVE_RESTART_NV);
        GLenum enable_error = glGetError();
        restart_index(0xffffU);
        GLenum set_error = glGetError();
        GLint enabled = glIsEnabled(GL_PRIMITIVE_RESTART_NV);
        GLenum enabled_error = glGetError();
        GLint index = 0;
        glGetIntegerv(GL_PRIMITIVE_RESTART_INDEX_NV, &index);
        GLenum get_error = glGetError();
        printf("PRIMITIVE_RESTART_STATE enabled=%d index=0x%x enable_error=0x%x "
               "set_error=0x%x enabled_error=0x%x get_error=0x%x\n",
               enabled, (unsigned)index, enable_error, set_error, enabled_error, get_error);
        int functional = enabled == 1 && index == 0xffff && enable_error == GL_NO_ERROR &&
                         set_error == GL_NO_ERROR && enabled_error == GL_NO_ERROR &&
                         get_error == GL_NO_ERROR;
        printf("CAP_GL_NV_primitive_restart_functional %s\n", functional ? "yes" : "no");
        if (enabled == 1) glDisable(GL_PRIMITIVE_RESTART_NV);
        if (!functional && getenv("LG110_REQUIRE_PRIMITIVE_RESTART") &&
            !strcmp(getenv("LG110_REQUIRE_PRIMITIVE_RESTART"), "1"))
            die("GL_NV_primitive_restart state verification");
    }
    printf("PROBE_HARDWARE accepted (software names absent)\n");

    free(renderer_lower);
    free(version_lower);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    eglTerminate(display);
    gbm_surface_destroy(gbm_surface);
    gbm_device_destroy(gbm);
    close(fd);
    return 0;
}
