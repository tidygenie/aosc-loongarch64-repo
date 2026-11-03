#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

#ifndef EGL_WINDOW_BIT
#define EGL_WINDOW_BIT 0x0001
#endif
#ifndef EGL_PIXMAP_BIT
#define EGL_PIXMAP_BIT 0x0002
#endif
#ifndef EGL_PBUFFER_BIT
#define EGL_PBUFFER_BIT 0x0004
#endif

typedef EGLBoolean (*get_config_attrib_fn)(EGLDisplay, EGLConfig, EGLint, EGLint *);
typedef EGLBoolean (*choose_config_fn)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
typedef EGLSurface (*create_window_fn)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);

static int is_plasma_process(void)
{
    char comm[16] = {0};
    if (prctl(PR_GET_NAME, comm, 0, 0, 0) < 0)
        return 0;
    return strncmp(comm, "plasmashell", sizeof("plasmashell") - 1) == 0;
}

EGLBoolean eglGetConfigAttrib(EGLDisplay display, EGLConfig config,
                              EGLint attribute, EGLint *value)
{
    static get_config_attrib_fn real_func;
    if (!real_func)
        real_func = (get_config_attrib_fn)dlsym(RTLD_NEXT, "eglGetConfigAttrib");
    EGLBoolean ok = real_func(display, config, attribute, value);
    if (ok && is_plasma_process() && attribute == EGL_SURFACE_TYPE &&
        value && (*value & EGL_PBUFFER_BIT)) {
        *value |= EGL_WINDOW_BIT;
    }
    return ok;
}

EGLBoolean eglChooseConfig(EGLDisplay display, const EGLint *attrib_list,
                           EGLConfig *configs, EGLint config_size, EGLint *num_config)
{
    static choose_config_fn real_func;
    if (!real_func)
        real_func = (choose_config_fn)dlsym(RTLD_NEXT, "eglChooseConfig");

    EGLint modified[128];
    int count = 0;
    if (is_plasma_process() && attrib_list) {
        for (count = 0; attrib_list[count] != EGL_NONE && count < 126; count += 2) {
            modified[count] = attrib_list[count];
            modified[count + 1] = attrib_list[count + 1];
            if (attrib_list[count] == EGL_SURFACE_TYPE &&
                attrib_list[count + 1] == EGL_WINDOW_BIT)
                modified[count + 1] = EGL_PBUFFER_BIT;
        }
        if (attrib_list[count] == EGL_NONE && count < 127)
            modified[count] = EGL_NONE;
        else
            return real_func(display, attrib_list, configs, config_size, num_config);
    }

    const EGLint *effective = is_plasma_process() && count ? modified : attrib_list;
    return real_func(display, effective, configs, config_size, num_config);
}

EGLSurface eglCreatePlatformWindowSurfaceEXT(EGLDisplay display, EGLConfig config,
                                             void *native_window, const EGLint *attrib_list)
{
    static create_window_fn real_func;
    if (!real_func)
        real_func = (create_window_fn)dlsym(RTLD_NEXT, "eglCreatePlatformWindowSurfaceEXT");
    if (is_plasma_process()) {
        static const char msg[] = "gsgpu-egl-window: creating native EGL window surface\n";
        (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    }
    return real_func(display, config, (EGLNativeWindowType)native_window, attrib_list);
}

EGLSurface eglCreateWindowSurface(EGLDisplay display, EGLConfig config,
                                  EGLNativeWindowType native_window, const EGLint *attrib_list)
{
    static create_window_fn real_func;
    if (!real_func)
        real_func = (create_window_fn)dlsym(RTLD_NEXT, "eglCreateWindowSurface");
    if (is_plasma_process()) {
        static const char msg[] = "gsgpu-egl-window: creating EGL window surface\n";
        (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    }
    return real_func(display, config, native_window, attrib_list);
}
