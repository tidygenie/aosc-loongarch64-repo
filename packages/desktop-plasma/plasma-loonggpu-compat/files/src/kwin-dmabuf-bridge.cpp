// Bridge legacy wl_drm prime buffers into KWin's GraphicsBuffer path.
#include <unistd.h>
#include <utility>
#include <drm/drm_fourcc.h>
// This avoids rebuilding libkwin while keeping the actual GPU dmabuf.

#include <QSize>
#include <QtGlobal>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <unordered_map>
#include <wayland-server.h>

#include "core/graphicsbuffer.h"

namespace
{

class WlDrmBuffer : public KWin::GraphicsBuffer
{
public:
    explicit WlDrmBuffer(KWin::DmaBufAttributes &&attrs)
        : m_attrs(std::move(attrs))
        , m_hasAlpha(KWin::GraphicsBuffer::alphaChannelFromDrmFormat(m_attrs.format))
    {
    }

    QSize size() const override
    {
        return QSize(m_attrs.width, m_attrs.height);
    }

    bool hasAlphaChannel() const override
    {
        return m_hasAlpha;
    }

    const KWin::DmaBufAttributes *dmabufAttributes() const override
    {
        return &m_attrs;
    }

    void setResource(wl_resource *resource)
    {
        m_resource = resource;
        connect(this, &KWin::GraphicsBuffer::released, this, [this]() {
            if (m_resource) {
                wl_buffer_send_release(m_resource);
            }
        });
    }

    void clearResource()
    {
        m_resource = nullptr;
    }

private:
    KWin::DmaBufAttributes m_attrs;
    bool m_hasAlpha;
    wl_resource *m_resource = nullptr;
};

std::unordered_map<wl_resource *, WlDrmBuffer *> s_buffers;

void buffer_destroy(wl_client *, wl_resource *resource)
{
    wl_resource_destroy(resource);
}

void buffer_destroy_resource(wl_resource *resource)
{
    auto it = s_buffers.find(resource);
    if (it == s_buffers.end()) {
        return;
    }
    WlDrmBuffer *buffer = it->second;
    s_buffers.erase(it);
    buffer->clearResource();
    buffer->drop();
}

const struct wl_buffer_interface s_buffer_implementation = {
    .destroy = buffer_destroy,
};

} // namespace

extern "C" wl_resource *loonggpu_create_wl_drm_buffer(wl_client *client, uint32_t id, int fd,
                                                       int32_t width, int32_t height, uint32_t format,
                                                       int32_t offset, int32_t stride)
{
    if (fd < 0 || width <= 0 || height <= 0 || offset < 0 || stride <= 0) {
        if (fd >= 0) {
            close(fd);
        }
        return nullptr;
    }

    KWin::DmaBufAttributes attrs;
    attrs.planeCount = 1;
    attrs.width = width;
    attrs.height = height;
    attrs.format = format;
    attrs.modifier = DRM_FORMAT_MOD_INVALID;
    attrs.fd[0] = KWin::FileDescriptor(fd);
    attrs.offset[0] = offset;
    attrs.pitch[0] = stride;

    wl_resource *resource = wl_resource_create(client, &wl_buffer_interface, 1, id);
    if (!resource) {
        close(fd);
        return nullptr;
    }

    auto *buffer = new WlDrmBuffer(std::move(attrs));
    s_buffers.emplace(resource, buffer);
    buffer->setResource(resource);
    wl_resource_set_implementation(resource, &s_buffer_implementation, buffer, buffer_destroy_resource);

    static int created = 0;
    if (created++ < 3) {
        std::fprintf(stderr, "kwin-dmabuf-bridge: wrapped wl_drm buffer %dx%d fmt=%08x stride=%d\n",
                     width, height, format, stride);
    }
    return resource;
}

extern "C" KWin::GraphicsBuffer *_ZN4KWin7Display17bufferForResourceEP11wl_resource(wl_resource *resource)
{
    using BufferForResource = KWin::GraphicsBuffer *(*)(wl_resource *);
    static BufferForResource real_func;
    if (!real_func) {
        real_func = reinterpret_cast<BufferForResource>(
            dlsym(RTLD_NEXT, "_ZN4KWin7Display17bufferForResourceEP11wl_resource"));
    }

    auto it = s_buffers.find(resource);
    if (it != s_buffers.end()) {
        static int hits = 0;
        if (hits++ < 3) {
            static const char msg[] = "kwin-dmabuf-bridge: Display::bufferForResource hit interposer\n";
            (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
        }
        return it->second;
    }
    return real_func ? real_func(resource) : nullptr;
}
