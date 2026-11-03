#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <link.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <xf86drm.h>
#include "wayland-server.h"

#ifndef EGL_EXTENSIONS
#define EGL_EXTENSIONS 0x3055
#endif

typedef const char *(*egl_query_string_fn)(EGLDisplay, EGLint);
static int is_kwin_process(void);

static EGLBoolean shim_query_dmabuf_formats(EGLDisplay display, EGLint max, EGLint *formats, EGLint *count)
{
    (void)display;
    if (!count)
        return EGL_FALSE;
    const EGLint supported[] = {
        (EGLint)0x34325241, /* DRM_FORMAT_ARGB8888 / AR24 */
        (EGLint)0x34325258, /* DRM_FORMAT_XRGB8888 / XR24 */
    };
    *count = 2;
    if (max < 2)
        return max == 0 ? EGL_TRUE : EGL_FALSE;
    if (formats) {
        formats[0] = supported[0];
        formats[1] = supported[1];
    }
    return EGL_TRUE;
}

static EGLBoolean shim_query_dmabuf_modifiers(EGLDisplay display, EGLint format, EGLint max, uint64_t *modifiers, EGLBoolean *external_only, EGLint *count)
{
    (void)display;
    (void)format;
    (void)max;
    (void)modifiers;
    (void)external_only;
    if (!count)
        return EGL_FALSE;
    /* The vendor driver uses private/implicit tiling and does not expose
     * explicit modifiers. Returning zero makes KWin fall back to
     * DRM_FORMAT_MOD_INVALID + DRM_FORMAT_MOD_LINEAR. */
    *count = 0;
    return EGL_TRUE;
}

static void *shim_epoxy_eglGetProcAddress(const char *name)
{
    static void *(*real_func)(const char *);
    if (!real_func) {
        real_func = (void *(*)(const char *))dlsym(RTLD_NEXT, "eglGetProcAddress");
    }
    if (is_kwin_process() && name) {
        if (strcmp(name, "eglQueryDmaBufFormatsEXT") == 0)
            return (void *)shim_query_dmabuf_formats;
        if (strcmp(name, "eglQueryDmaBufModifiersEXT") == 0)
            return (void *)shim_query_dmabuf_modifiers;
    }
    return real_func ? real_func(name) : NULL;
}

/* libepoxy exports this as an 8-byte function pointer object. */
void *(*epoxy_eglGetProcAddress)(const char *) = shim_epoxy_eglGetProcAddress;

#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif

static int is_kwin_process(void)
{
    char comm[16] = {0};
    if (prctl(PR_GET_NAME, comm, 0, 0, 0) < 0)
        return 0;
    return strncmp(comm, "kwin_wayland", sizeof("kwin_wayland") - 1) == 0;
}

static int is_card0_path(const char *path)
{
    if (!path)
        return 0;
    size_t n = strlen(path);
    if (n >= sizeof("/dev/dri/card0") &&
        strcmp(path + n - (sizeof("/dev/dri/card0") - 1), "/dev/dri/card0") == 0)
        return 1;
    return strcmp(path, "/dev/dri/card0") == 0;
}

static int existing_card0_fd(int cloexec)
{
    int max = 1024;
    long limit = sysconf(_SC_OPEN_MAX);
    if (limit > 0 && limit < max)
        max = (int)limit;
    for (int fd = 0; fd < max; fd++) {
        char buf[PATH_MAX];
        ssize_t n = readlinkat(AT_FDCWD, "/proc/self/fd", buf, 0); /* anchor procfs use */
        (void)n;
        char path[64];
        snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
        n = readlink(path, buf, sizeof(buf) - 1);
        if (n <= 0)
            continue;
        buf[n] = '\0';
        if (strcmp(buf, "/dev/dri/card0") != 0)
            continue;
        return cloexec ? fcntl(fd, F_DUPFD_CLOEXEC, 0) : dup(fd);
    }
    return -1;
}

static int reuse_card0_open(const char *path, int flags)
{
    if (!is_kwin_process() || !is_card0_path(path))
        return -1;
    if (!(flags & O_WRONLY) && !(flags & O_RDWR))
        return -1;

    int fd = existing_card0_fd(!!(flags & O_CLOEXEC));
    if (fd >= 0) {
        /* A dup of the session master fd cannot serve where KWin needs a
         * *non-master* fd (DrmGpu::createNonMasterFd for DRM leasing): the dup
         * still reports master and drmDropMaster() returns EPERM.  A fresh open
         * on the same device is not master, which is exactly what the lease
         * path wants; keep the dup as a fallback. */
        if (drmIsMaster(fd)) {
            int fresh = (int)syscall(SYS_openat, AT_FDCWD, path, flags, 0);
            if (fresh >= 0 && !drmIsMaster(fresh)) {
                static const char msg[] = "kwin-drm-fd-reuse: fresh non-master card0 fd\n";
                (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
                close(fd);
                return fresh;
            }
            if (fresh >= 0)
                close(fresh);
        }
        static const char msg[] = "kwin-drm-fd-reuse: reused authorized card0 fd\n";
        (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
        return fd;
    }

    /* KWin's vendor EGL initialization opens card0 before logind has granted
     * the KMS fd. That open accidentally becomes DRM master and makes the
     * subsequent logind TakeDevice() fail with EBUSY. Rendering does not need
     * KMS, so redirect this first accidental open to the render node. */
    fd = (int)syscall(SYS_openat, AT_FDCWD, "/dev/dri/renderD128", flags, 0);
    if (fd >= 0) {
        static const char msg[] = "kwin-drm-fd-reuse: redirected early card0 open to renderD128\n";
        (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    }
    return fd;
}

extern const struct wl_interface wl_buffer_interface;

static const struct wl_interface *drm_types[] = {
    NULL,
    &wl_buffer_interface,
    NULL, NULL, NULL, NULL, NULL,
    &wl_buffer_interface,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    &wl_buffer_interface,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
};

static const struct wl_message wl_drm_requests[] = {
    {"authenticate", "u", drm_types + 0},
    {"create_buffer", "nuiiuu", drm_types + 1},
    {"create_planar_buffer", "nuiiiiiiii", drm_types + 7},
    {"create_prime_buffer", "nhiiiiiiii", drm_types + 18},
};

static const struct wl_message wl_drm_events[] = {
    {"device", "s", NULL},
    {"format", "u", NULL},
    {"authenticated", "", NULL},
    {"capabilities", "u", NULL},
};

const struct wl_interface wl_drm_interface = {
    "wl_drm", 2,
    4, wl_drm_requests,
    4, wl_drm_events,
};

struct wl_drm_interface {
    void (*authenticate)(struct wl_client *, struct wl_resource *, uint32_t);
    void (*create_buffer)(struct wl_client *, struct wl_resource *, uint32_t, uint32_t,
                          int32_t, int32_t, uint32_t, uint32_t);
    void (*create_planar_buffer)(struct wl_client *, struct wl_resource *, uint32_t, uint32_t,
                                 int32_t, int32_t, uint32_t, int32_t, int32_t,
                                 int32_t, int32_t, int32_t, int32_t);
    void (*create_prime_buffer)(struct wl_client *, struct wl_resource *, uint32_t, int32_t,
                                int32_t, int32_t, uint32_t, int32_t, int32_t,
                                int32_t, int32_t, int32_t, int32_t);
};

enum {
    WL_DRM_EVENT_DEVICE = 0,
    WL_DRM_EVENT_FORMAT = 1,
    WL_DRM_EVENT_AUTHENTICATED = 2,
    WL_DRM_EVENT_CAPABILITIES = 3,
    WL_DRM_CAPABILITY_PRIME = 1,
    WL_DRM_ERROR_AUTHENTICATE_FAIL = 0,
    WL_DRM_ERROR_INVALID_FORMAT = 1,
};

static void wl_drm_send_device(struct wl_resource *resource, const char *name)
{
    wl_resource_post_event(resource, WL_DRM_EVENT_DEVICE, name);
}

static void wl_drm_send_format(struct wl_resource *resource, uint32_t format)
{
    wl_resource_post_event(resource, WL_DRM_EVENT_FORMAT, format);
}

static void wl_drm_send_authenticated(struct wl_resource *resource)
{
    wl_resource_post_event(resource, WL_DRM_EVENT_AUTHENTICATED);
}

static void wl_drm_send_capabilities(struct wl_resource *resource, uint32_t capabilities)
{
    wl_resource_post_event(resource, WL_DRM_EVENT_CAPABILITIES, capabilities);
}

static int authorized_card0_fd(void)
{
    int max = 1024;
    long limit = sysconf(_SC_OPEN_MAX);
    if (limit > 0 && limit < max)
        max = (int)limit;
    for (int fd = 0; fd < max; fd++) {
        char path[64], target[PATH_MAX];
        snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
        ssize_t n = readlink(path, target, sizeof(target) - 1);
        if (n <= 0)
            continue;
        target[n] = '\0';
        if (strcmp(target, "/dev/dri/card0") == 0)
            return fd;
    }
    return -1;
}

static void noop_buffer_destroy(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    (void)resource;
}

static const struct wl_buffer_interface noop_buffer_interface = {
    noop_buffer_destroy,
};

static void noop_buffer_resource_destroy(struct wl_resource *resource)
{
    (void)resource;
}

static struct wl_resource *make_noop_buffer(struct wl_client *client, uint32_t id)
{
    struct wl_resource *buffer = wl_resource_create(client, &wl_buffer_interface, 1, id);
    if (buffer)
        wl_resource_set_implementation(buffer, &noop_buffer_interface, NULL, noop_buffer_resource_destroy);
    return buffer;
}

static void wl_drm_authenticate(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
    (void)client;
    int fd = authorized_card0_fd();
    if (fd >= 0 && drmAuthMagic(fd, id) == 0) {
        wl_drm_send_authenticated(resource);
        return;
    }
    static const char msg[] = "kwin-wl-drm: drmAuthMagic failed\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    wl_resource_post_error(resource, WL_DRM_ERROR_AUTHENTICATE_FAIL, "drmAuthMagic failed");
}

static void wl_drm_create_buffer(struct wl_client *client, struct wl_resource *resource, uint32_t id,
                                 uint32_t name, int32_t width, int32_t height, uint32_t stride, uint32_t format)
{
    (void)resource; (void)name; (void)stride;
    dprintf(STDERR_FILENO, "kwin-wl-drm: create_buffer %dx%d fmt=%08x id=%u\n", width, height, format, id);
    make_noop_buffer(client, id);
}

static void wl_drm_create_planar_buffer(struct wl_client *client, struct wl_resource *resource, uint32_t id,
                                        uint32_t name, int32_t width, int32_t height, uint32_t format,
                                        int32_t offset0, int32_t stride0, int32_t offset1, int32_t stride1,
                                        int32_t offset2, int32_t stride2)
{
    (void)resource; (void)name; (void)offset0; (void)stride0; (void)offset1; (void)stride1; (void)offset2; (void)stride2;
    dprintf(STDERR_FILENO, "kwin-wl-drm: create_planar_buffer %dx%d fmt=%08x id=%u\n", width, height, format, id);
    make_noop_buffer(client, id);
}

static void wl_drm_create_prime_buffer(struct wl_client *client, struct wl_resource *resource, uint32_t id,
                                       int32_t name, int32_t width, int32_t height, uint32_t format,
                                       int32_t offset0, int32_t stride0, int32_t offset1, int32_t stride1,
                                       int32_t offset2, int32_t stride2)
{
    (void)resource; (void)offset1; (void)stride1; (void)offset2; (void)stride2;
    typedef struct wl_resource *(*create_dmabuf_fn)(struct wl_client *, uint32_t, int, int32_t, int32_t,
                                                          uint32_t, int32_t, int32_t);
    static create_dmabuf_fn create_dmabuf;
    if (!create_dmabuf)
        create_dmabuf = (create_dmabuf_fn)dlsym(RTLD_DEFAULT, "loonggpu_create_wl_drm_buffer");
    if (!create_dmabuf)
        create_dmabuf = (create_dmabuf_fn)dlsym(RTLD_DEFAULT, "kwin_create_wl_drm_buffer");
    static int created = 0;
    if (created++ < 3) {
        dprintf(STDERR_FILENO, "kwin-wl-drm: create_prime_buffer %dx%d fmt=%08x stride=%d factory=%p\n",
                width, height, format, stride0, (void *)create_dmabuf);
    }
    if (create_dmabuf) {
        if (create_dmabuf(client, id, name, width, height, format, offset0, stride0))
            return;
    } else {
        close(name);
    }
    wl_resource_post_no_memory(resource);
}

static const struct wl_drm_interface wl_drm_implementation = {
    wl_drm_authenticate,
    wl_drm_create_buffer,
    wl_drm_create_planar_buffer,
    wl_drm_create_prime_buffer,
};

static void bind_wl_drm(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *resource = wl_resource_create(client, &wl_drm_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &wl_drm_implementation, NULL, NULL);
    wl_drm_send_device(resource, "/dev/dri/card0");
    wl_drm_send_format(resource, 0x34325241); /* AR24 */
    wl_drm_send_format(resource, 0x34325258); /* XR24 */
    wl_drm_send_format(resource, 0x36314752); /* RG16 */
    wl_drm_send_capabilities(resource, WL_DRM_CAPABILITY_PRIME);
    static const char msg[] = "kwin-wl-drm: bound legacy wl_drm global\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
}

static struct wl_global *legacy_wl_drm_global;
static int creating_legacy_wl_drm;
static struct wl_display *captured_wl_display;
struct wl_global *(*real_wl_global_create)(struct wl_display *, const struct wl_interface *,
                                           int, void *, wl_global_bind_func_t);

static void ensure_legacy_wl_drm_global(struct wl_display *display)
{
    if (!is_kwin_process() || !display || legacy_wl_drm_global || creating_legacy_wl_drm)
        return;
    if (!real_wl_global_create)
        real_wl_global_create = dlsym(RTLD_NEXT, "wl_global_create");
    if (!real_wl_global_create)
        return;
    captured_wl_display = display;
    creating_legacy_wl_drm = 1;
    legacy_wl_drm_global = real_wl_global_create(display, &wl_drm_interface, 2, NULL, bind_wl_drm);
    creating_legacy_wl_drm = 0;
    if (!legacy_wl_drm_global) {
        static const char msg[] = "kwin-wl-drm: failed to create legacy global\n";
        (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    } else {
        static const char msg[] = "kwin-wl-drm: created legacy global at display creation\n";
        (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    }
}

struct wl_display *(*real_wl_display_create)(void);

struct wl_display *wl_display_create(void)
{
    if (!real_wl_display_create)
        real_wl_display_create = dlsym(RTLD_NEXT, "wl_display_create");
    struct wl_display *display = real_wl_display_create ? real_wl_display_create() : NULL;
    ensure_legacy_wl_drm_global(display);
    return display;
}

struct wl_global *wl_global_create(struct wl_display *display, const struct wl_interface *interface,
                                   int version, void *data, wl_global_bind_func_t bind)
{
    if (!real_wl_global_create)
        real_wl_global_create = dlsym(RTLD_NEXT, "wl_global_create");
    struct wl_global *global = real_wl_global_create(display, interface, version, data, bind);
    if (is_kwin_process() && !legacy_wl_drm_global && !creating_legacy_wl_drm && global) {
        captured_wl_display = display;
        creating_legacy_wl_drm = 1;
        legacy_wl_drm_global = real_wl_global_create(display, &wl_drm_interface, 2, NULL, bind_wl_drm);
        creating_legacy_wl_drm = 0;
        if (!legacy_wl_drm_global) {
            static const char msg[] = "kwin-wl-drm: failed to create legacy global\n";
            (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
        }
    }
    return global;
}

int open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, mode_t);
    va_end(ap);

    int reused = reuse_card0_open(path, flags);
    if (reused >= 0)
        return reused;
    return (int)syscall(SYS_openat, AT_FDCWD, path, flags, mode);
}

int open64(const char *path, int flags, ...)
{
    mode_t mode = 0;
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, mode_t);
    va_end(ap);

    int reused = reuse_card0_open(path, flags);
    if (reused >= 0)
        return reused;
    return (int)syscall(SYS_openat, AT_FDCWD, path, flags | O_LARGEFILE, mode);
}

int openat(int dirfd, const char *path, int flags, ...)
{
    mode_t mode = 0;
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, mode_t);
    va_end(ap);

    if (dirfd == AT_FDCWD) {
        int reused = reuse_card0_open(path, flags);
        if (reused >= 0)
            return reused;
    }
    return (int)syscall(SYS_openat, dirfd, path, flags, mode);
}

static const char *extend_kwin_egl_extensions(EGLDisplay display, const char *value)
{
    if (!is_kwin_process() || !value)
        return value;

    const char *add = NULL;
    if (display == EGL_NO_DISPLAY) {
        if (strstr(value, "EGL_MESA_platform_gbm") && !strstr(value, "EGL_KHR_platform_gbm"))
            add = "EGL_KHR_platform_gbm";
    } else if (strstr(value, "EGL_EXT_image_dma_buf_import") &&
               !strstr(value, "EGL_EXT_image_dma_buf_import_modifiers")) {
        add = "EGL_EXT_image_dma_buf_import_modifiers";
    }
    if (!add)
        return value;

    static char buffers[2][8192];
    static unsigned index;
    char *extended = buffers[index];
    index = (index + 1) % 2;
    int n = snprintf(extended, sizeof(buffers[0]), "%s %s", value, add);
    if (n < 0 || (size_t)n >= sizeof(buffers[0]))
        return value;
    return extended;
}

static const char *shim_epoxy_eglQueryString(EGLDisplay display, EGLint name)
{
    static egl_query_string_fn real_func;
    if (!real_func) {
        real_func = (egl_query_string_fn)dlsym(RTLD_NEXT, "eglQueryString");
        if (!real_func)
            return NULL;
    }

    const char *value = real_func(display, name);
    if (name == EGL_EXTENSIONS)
        value = extend_kwin_egl_extensions(display, value);
    return value;
}

/* libepoxy exports epoxy_eglQueryString as an 8-byte function pointer,
 * not a callable function. Supply a compatible pointer object. */
const char *(*epoxy_eglQueryString)(EGLDisplay, EGLint) = shim_epoxy_eglQueryString;

const char *eglQueryString(EGLDisplay display, EGLint name)
{
    static egl_query_string_fn real_func;
    if (!real_func) {
        real_func = (egl_query_string_fn)dlsym(RTLD_NEXT, "eglQueryString");
        if (!real_func)
            return NULL;
    }

    const char *value = real_func(display, name);
    if (name == EGL_EXTENSIONS)
        value = extend_kwin_egl_extensions(display, value);
    return value;
}
