/*
 * vkgl — a minimal Vulkan ICD for the Loongson LG110 GPU.
 *
 * The vendor stack (gsgpu Mesa 18.3.6) only exposes OpenGL ES 2.0 through
 * EGL/GBM, and the hardware command processor speaks Loongson GSPKT rather
 * than AMD PM4, so RADV cannot work.  This ICD implements the Vulkan 1.0
 * subset needed by simple applications on top of GLES2.
 *
 * Stage 1 (this file): instance/device/queue, memory, buffers, images,
 * render passes, command buffers, swapchain (Wayland wl_shm) and a
 * clear-only submit path.  Pipelines / draws are added incrementally.
 */
#define VK_USE_PLATFORM_WAYLAND_KHR 1
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include "linux-dmabuf-client-protocol.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gbm.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#ifndef GL_DEPTH_STENCIL
#define GL_DEPTH_STENCIL 0x84F9
#endif
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#endif
#ifndef GL_UNSIGNED_INT_24_8
#define GL_UNSIGNED_INT_24_8 0x84FA
#endif
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <poll.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <string>
#include <algorithm>
#include <array>
#include <utility>

#define VKGL_LOG(...) do { if (getenv("VKGL_DEBUG")) fprintf(stderr, "vkgl: " __VA_ARGS__); } while (0)

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
template <typename T> static T *from_handle(uint64_t h) { return reinterpret_cast<T *>(h); }
template <typename T> static uint64_t to_handle(T *p) { return reinterpret_cast<uint64_t>(p); }

struct Obj {
   VkObjectType type = VK_OBJECT_TYPE_UNKNOWN;
   virtual ~Obj() = default;
};

// ---------------------------------------------------------------------------
// GL context (single global, created lazily on first device use)
// ---------------------------------------------------------------------------
struct GlCtx {
   int drm_fd = -1;
   gbm_device *gbm = nullptr;
   gbm_surface *gbm_surf = nullptr;
   EGLDisplay dpy = EGL_NO_DISPLAY;
   EGLContext ctx = EGL_NO_CONTEXT;
   EGLSurface surf = EGL_NO_SURFACE;
   bool ok = false;

   static GlCtx &get() {
      static GlCtx g;
      static bool init_done = false;
      if (init_done) return g;
      init_done = true;
      g.init();
      return g;
   }

   void init() {
      /* Pick the first vendor JSON that actually exists: different vendor
       * tarballs install it under different names (and some none at all, in
       * which case glvnd's default enumeration is the best we can do). */
      {
         static const char *cands[] = {
            "/usr/share/glvnd/egl_vendor.d/40_gsgpu.json",
            "/usr/share/glvnd/egl_vendor.d/60_gsgpu.json",
            "/usr/share/glvnd/egl_vendor.d/60_loonggpu.json",
         };
         for (const char *c : cands) {
            if (access(c, R_OK) == 0) { setenv("__EGL_VENDOR_LIBRARY_FILENAMES", c, 1); break; }
         }
      }
      setenv("GBM_BACKEND", "gsgpu", 1);
      drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
      if (drm_fd < 0) { perror("vkgl: open renderD128"); return; }
      gbm = gbm_create_device(drm_fd);
      if (!gbm) { fprintf(stderr, "vkgl: gbm_create_device failed\n"); return; }
      auto getpd = (EGLDisplay (*)(EGLenum, void *, const EGLint *))
         eglGetProcAddress("eglGetPlatformDisplayEXT");
      if (!getpd) { fprintf(stderr, "vkgl: no eglGetPlatformDisplayEXT\n"); return; }
      dpy = getpd(EGL_PLATFORM_GBM_KHR, gbm, nullptr);
      if (dpy == EGL_NO_DISPLAY) { fprintf(stderr, "vkgl: no EGL display\n"); return; }
      EGLint maj, min;
      if (!eglInitialize(dpy, &maj, &min)) { fprintf(stderr, "vkgl: eglInitialize failed 0x%x\n", eglGetError()); return; }
      eglBindAPI(EGL_OPENGL_API);
      EGLint n = 0;
      eglGetConfigs(dpy, nullptr, 0, &n);
      std::vector<EGLConfig> cfgs(n);
      eglGetConfigs(dpy, cfgs.data(), n, &n);
      EGLConfig cfg = cfgs.empty() ? nullptr : cfgs[0];
      gbm_surf = gbm_surface_create(gbm, 16, 16, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING);
      auto cw = (EGLSurface (*)(EGLDisplay, EGLConfig, void *, const EGLint *))
         eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT");
      if (gbm_surf && cw) surf = cw(dpy, cfg, gbm_surf, nullptr);
      if (surf == EGL_NO_SURFACE) { fprintf(stderr, "vkgl: egl window surface failed 0x%x\n", eglGetError()); return; }
      EGLint ca[] = {EGL_CONTEXT_MAJOR_VERSION, 2, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
      ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ca);
      if (ctx == EGL_NO_CONTEXT) { fprintf(stderr, "vkgl: eglCreateContext failed 0x%x\n", eglGetError()); return; }
      if (!eglMakeCurrent(dpy, surf, surf, ctx)) { fprintf(stderr, "vkgl: eglMakeCurrent failed 0x%x\n", eglGetError()); return; }
      ok = true;
      VKGL_LOG("GL %s / %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
   }

   void make_current() {
      if (!ok) return;
      if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
         fprintf(stderr, "vkgl: eglMakeCurrent lost: 0x%x\n", eglGetError());
         // try to recreate the surface/context once
         if (surf != EGL_NO_SURFACE) { eglDestroySurface(dpy, surf); surf = EGL_NO_SURFACE; }
         if (gbm_surf) {
            auto cw = (EGLSurface (*)(EGLDisplay, EGLConfig, void *, const EGLint *))
               eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT");
            EGLint n = 0; eglGetConfigs(dpy, nullptr, 0, &n);
            std::vector<EGLConfig> cfgs(n); eglGetConfigs(dpy, cfgs.data(), n, &n);
            if (cw && !cfgs.empty()) surf = cw(dpy, cfgs[0], gbm_surf, nullptr);
         }
         if (surf != EGL_NO_SURFACE) eglMakeCurrent(dpy, surf, surf, ctx);
      }
   }
};

// ---------------------------------------------------------------------------
// objects
// ---------------------------------------------------------------------------
struct Instance : Obj {
   std::vector<const char *> exts;
};

struct PhysicalDevice : Obj {
   Instance *inst = nullptr;
};

struct Device : Obj {
   PhysicalDevice *phys = nullptr;
};

struct Queue : Obj {
   Device *dev = nullptr;
   uint32_t family = 0;
};

struct Memory : Obj {
   VkDeviceSize size = 0;
   uint32_t type_index = 0;
   VkMemoryPropertyFlags props = 0;
   std::vector<uint8_t> shadow;   // host-visible data (also used for uploads)
   bool host_visible = false;
   // GL object when bound
   GLuint gl_buffer = 0;
   GLuint gl_texture = 0;
   bool bound_buffer = false;
   bool bound_image = false;
   bool mapped = false;
   uint64_t revision = 1;
};

struct Buffer : Obj {
   VkDeviceSize size = 0;
   VkBufferUsageFlags usage = 0;
   Memory *mem = nullptr;
   VkDeviceSize mem_offset = 0;
   GLuint gl = 0;                 // GL buffer (created on bind)
   uint64_t uploaded_revision = 0;
};

struct Image : Obj {
   VkFormat format = VK_FORMAT_UNDEFINED;
   VkExtent3D extent = {0, 0, 1};
   VkImageUsageFlags usage = 0;
   VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
   Memory *mem = nullptr;
   VkDeviceSize mem_offset = 0;
   GLuint tex = 0;
   GLuint fbo = 0;
   GLenum gl_format = GL_RGBA;
   GLenum gl_type = GL_UNSIGNED_BYTE;
   uint64_t uploaded_revision = 0;
};

struct ImageView : Obj {
   Image *image = nullptr;
   VkFormat format = VK_FORMAT_UNDEFINED;
};

struct RenderPass : Obj {
   VkFormat color_format = VK_FORMAT_UNDEFINED;
   VkFormat depth_format = VK_FORMAT_UNDEFINED;
   VkAttachmentLoadOp color_load = VK_ATTACHMENT_LOAD_OP_LOAD;
   VkAttachmentStoreOp color_store = VK_ATTACHMENT_STORE_OP_STORE;
   VkAttachmentLoadOp depth_load = VK_ATTACHMENT_LOAD_OP_LOAD;
};

struct Framebuffer : Obj {
   RenderPass *rp = nullptr;
   ImageView *color = nullptr;
   ImageView *depth = nullptr;
   uint32_t width = 0, height = 0;
};

struct ShaderModule : Obj {
   std::vector<uint32_t> code;
};

struct ShaderStage {
   VkShaderStageFlagBits stage;
   ShaderModule *module;
   std::string entry;
   VkSpecializationInfo spec = {};
};

struct PipelineLayout : Obj {
   VkPipelineLayoutCreateInfo info = {};
   std::vector<VkDescriptorSetLayout> set_layouts;
   std::vector<VkPushConstantRange> push_ranges;
};

struct Pipeline : Obj {
   PipelineLayout *layout = nullptr;
   VkPipelineBindPoint bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
   VkGraphicsPipelineCreateInfo info = {};
   std::vector<ShaderStage> stages;
   VkVertexInputBindingDescription vib = {};
   std::vector<VkVertexInputAttributeDescription> via;
   GLuint program = 0;
   bool built = false;
   VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
   VkCullModeFlags cull = VK_CULL_MODE_NONE;
   VkFrontFace front = VK_FRONT_FACE_COUNTER_CLOCKWISE;
   VkBool32 depth_test = VK_FALSE, depth_write = VK_FALSE;
   VkCompareOp depth_op = VK_COMPARE_OP_ALWAYS;
   VkBool32 blend = VK_FALSE;
   RenderPass *rp = nullptr;
   uint32_t subpass = 0;

   // reflection for the GLES2 translation
   struct PipeUbo {
      std::string name;
      GLint loc = -1;
      uint32_t set = 0, binding = 0;
      uint32_t vec4_count = 0;
      bool push_constant = false;
   };
   std::vector<PipeUbo> ubos;
   struct PipeTex { std::string name; GLint loc = -1; uint32_t set = 0, binding = 0; };
   std::vector<PipeTex> textures;
   std::vector<std::pair<std::string, uint32_t>> attribs;  // name -> location
   VkExtent2D fb_extent = {0, 0};
   bool has_depth = false;
   bool uses_vertex_id = false;
};

struct DescriptorSetLayout : Obj {
   std::vector<VkDescriptorSetLayoutBinding> bindings;
};

struct DescriptorPool : Obj {
   std::vector<VkDescriptorPoolSize> sizes;
};

struct DescriptorSet : Obj {
   DescriptorPool *pool = nullptr;
   DescriptorSetLayout *layout = nullptr;
   std::vector<VkDescriptorBufferInfo> buffers;
   std::vector<VkDescriptorImageInfo> images;
   std::vector<uint8_t> written;  // per binding
   std::vector<std::pair<uint32_t, VkDeviceSize>> dynamic_offsets;  // binding -> offset
};

struct CommandPool : Obj {
   Device *dev = nullptr;
};

struct CommandBuffer;

struct Cmd {
   enum Kind {
      BeginRenderPass, EndRenderPass, BindPipeline, BindVertexBuffer, BindIndexBuffer,
      BindDescriptorSet, SetViewport, SetScissor, Draw, DrawIndexed, ClearColorImage,
      CopyBuffer, CopyBufferToImage, CopyImageToBuffer, UpdateBuffer, Barrier, PipelineBarrier,
      DrawIndirect, DrawIndexedIndirect, FillBuffer, ExecuteCommands,
   } kind;
   Framebuffer *fb = nullptr;
   RenderPass *rp = nullptr;
   std::vector<VkClearValue> clears;
   Pipeline *pipe = nullptr;
   Buffer *buf = nullptr;
   VkDeviceSize offset = 0;
   DescriptorSet *set = nullptr;
   VkViewport vp = {};
   VkRect2D scissor = {};
   uint32_t first = 0, count = 0, instances = 1;
   int32_t vertex_offset = 0;
   Image *img = nullptr;
   VkClearColorValue cc = {};
   VkImageSubresourceRange range = {};
   VkBufferImageCopy region = {};
   Buffer *src = nullptr;
   std::vector<VkBufferCopy> copies;
   uint32_t fill_value = 0;
   VkDeviceSize size = 0;
   std::vector<CommandBuffer *> secondary;
   uint32_t indirect_stride = 0;
   const void *update_data = nullptr;
   uint32_t update_size = 0;
   VkDeviceSize update_offset = 0;
};

struct CommandBuffer : Obj {
   CommandPool *pool = nullptr;
   bool recording = false;
   bool recorded = false;
   std::vector<Cmd> cmds;
   Framebuffer *active_fb = nullptr;
   RenderPass *active_rp = nullptr;
   VkViewport cur_vp = {};
   VkRect2D cur_scissor = {};
   Pipeline *cur_pipe = nullptr;
};

struct Semaphore : Obj {};
struct Fence : Obj { bool signaled = true; };

struct Surface : Obj {
   wl_display *display = nullptr;
   wl_surface *surface = nullptr;
};

static const bool dmabuf_disabled = getenv("VKGL_NO_DMABUF") != nullptr;

typedef void (*PFNEGLIMAGETARGETTEXTURE2DOESPROC)(unsigned int, void *);
struct ShmRelease;
struct Swapchain : Obj {
   Device *dev = nullptr;
   Surface *surface = nullptr;
   VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
   VkColorSpaceKHR cs = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
   VkExtent2D extent = {0, 0};
   uint32_t count = 0;
   std::vector<Image *> images;
   std::vector<VkImage> image_handles;
   std::vector<wl_buffer *> wl_buffers;
   std::vector<void *> shm_data;
   std::vector<size_t> shm_size;
   std::vector<wl_shm_pool *> pools;
   zwp_linux_dmabuf_v1 *dmabuf = nullptr;
   wl_event_queue *queue = nullptr;
   bool dmabuf_ok = false;
   std::vector<gbm_bo *> bos;
   std::vector<wl_buffer *> dbufs;
   std::vector<EGLImage> eglimgs;
   std::vector<ShmRelease *> releases;
   wl_shm *shm = nullptr;
   uint32_t current = 0;
   ~Swapchain();
};

// ---------------------------------------------------------------------------
// global registries (single device world)
// ---------------------------------------------------------------------------
static std::vector<Instance *> g_instances;
static Instance *g_instance = nullptr;
static PhysicalDevice *g_phys = nullptr;
static Device *g_device = nullptr;
static Queue *g_queue = nullptr;
static std::unordered_map<uint64_t, Obj *> g_objects;

template <typename T> static void reg(Obj *o, T *h) { g_objects[(uint64_t)h] = o; }
static void reg(Obj *o, uint64_t h) { g_objects[h] = o; }
static void unreg(uint64_t h) { g_objects.erase(h); }

// ---------------------------------------------------------------------------
// memory helpers
// ---------------------------------------------------------------------------
static void flip_rows(uint8_t *data, size_t width, size_t height, size_t bpp) {
   const size_t row = width * bpp;
   std::vector<uint8_t> tmp(row);
   for (size_t y = 0; y < height / 2; y++) {
      uint8_t *a = data + y * row;
      uint8_t *b = data + (height - 1 - y) * row;
      memcpy(tmp.data(), a, row);
      memcpy(a, b, row);
      memcpy(b, tmp.data(), row);
   }
}
static uint32_t find_memory_type(const VkPhysicalDeviceMemoryProperties &mp, uint32_t bits,
                                 VkMemoryPropertyFlags want) {
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
      if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
   }
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if (bits & (1u << i)) return i;
   return 0;
}

static void ensure_gl_buffer(Buffer *b) {
   if (b->gl) return;
   GlCtx::get().make_current();
   glGenBuffers(1, &b->gl);
   glBindBuffer(GL_ARRAY_BUFFER, b->gl);
   glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)b->size, nullptr, GL_DYNAMIC_DRAW);
   glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static void ensure_gl_texture(Image *im) {
   if (im->tex) return;
   GlCtx::get().make_current();
   glGenTextures(1, &im->tex);
   glBindTexture(GL_TEXTURE_2D, im->tex);
   GLenum internal = GL_RGBA;
   im->gl_format = GL_RGBA;
   im->gl_type = GL_UNSIGNED_BYTE;
   switch (im->format) {
   case VK_FORMAT_B8G8R8A8_UNORM:
   case VK_FORMAT_B8G8R8A8_SRGB: im->gl_format = GL_BGRA_EXT; break;
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_R8G8B8A8_SRGB: im->gl_format = GL_RGBA; break;
   case VK_FORMAT_R16G16B16A16_SFLOAT: internal = GL_RGBA; im->gl_format = GL_RGBA; im->gl_type = GL_HALF_FLOAT_OES; break;
   case VK_FORMAT_D16_UNORM: internal = GL_DEPTH_COMPONENT16; im->gl_format = GL_DEPTH_COMPONENT; im->gl_type = GL_UNSIGNED_SHORT; break;
   case VK_FORMAT_D24_UNORM_S8_UINT: internal = GL_DEPTH_STENCIL; im->gl_format = GL_DEPTH_STENCIL; im->gl_type = GL_UNSIGNED_INT_24_8; break;
   case VK_FORMAT_D32_SFLOAT:
   case VK_FORMAT_D32_SFLOAT_S8_UINT: internal = GL_DEPTH_COMPONENT; im->gl_format = GL_DEPTH_COMPONENT; im->gl_type = GL_FLOAT; break;
   default: break;
   }
   glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal, (GLsizei)im->extent.width,
                (GLsizei)im->extent.height, 0, im->gl_format, im->gl_type, nullptr);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   glBindTexture(GL_TEXTURE_2D, 0);
}

static void ensure_fbo(Image *im) {
   if (im->fbo) return;
   GlCtx::get().make_current();
   ensure_gl_texture(im);
   glGenFramebuffers(1, &im->fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, im->fbo);
   if (im->format == VK_FORMAT_D16_UNORM || im->format == VK_FORMAT_D32_SFLOAT ||
       im->format == VK_FORMAT_D24_UNORM_S8_UINT || im->format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
      if (im->format == VK_FORMAT_D24_UNORM_S8_UINT)
         glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, im->tex, 0);
      else
         glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, im->tex, 0);
   }
   else
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, im->tex, 0);
   glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// ---------------------------------------------------------------------------
// instance / physical device
// ---------------------------------------------------------------------------
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_CreateInstance(const VkInstanceCreateInfo *ci,
                                                          const VkAllocationCallbacks *,
                                                          VkInstance *out) {
   auto *i = new Instance();
   i->type = VK_OBJECT_TYPE_INSTANCE;
   if (ci && ci->ppEnabledExtensionNames)
      i->exts.assign(ci->ppEnabledExtensionNames, ci->ppEnabledExtensionNames + ci->enabledExtensionCount);
   g_instances.push_back(i);
   g_instance = i;
   *out = (VkInstance)to_handle(i);
   reg(i, *out);
   VKGL_LOG("instance created\n");
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL vkgl_DestroyInstance(VkInstance inst, const VkAllocationCallbacks *) {
   auto *i = from_handle<Instance>((uint64_t)inst);
   unreg((uint64_t)inst);
   g_instances.erase(std::remove(g_instances.begin(), g_instances.end(), i), g_instances.end());
   if (g_instance == i) g_instance = nullptr;
   delete i;
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_EnumerateInstanceVersion(uint32_t *v) {
   *v = VK_API_VERSION_1_0;
   return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_EnumerateInstanceExtensionProperties(
   const char *layer, uint32_t *count, VkExtensionProperties *props) {
   (void)layer;
   static const VkExtensionProperties exts[] = {
      {"VK_KHR_surface", 25},
      {"VK_KHR_wayland_surface", 6},
      {"VK_KHR_xcb_surface", 6},
      {"VK_KHR_xlib_surface", 6},
      {"VK_EXT_debug_report", 10},
      {"VK_KHR_get_surface_capabilities2", 1},
   };
   uint32_t n = sizeof(exts) / sizeof(exts[0]);
   if (!props) { *count = n; return VK_SUCCESS; }
   n = std::min(n, *count);
   memcpy(props, exts, n * sizeof(VkExtensionProperties));
   *count = n;
   return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_EnumerateInstanceLayerProperties(uint32_t *count, VkLayerProperties *) {
   *count = 0;
   return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_EnumerateDeviceExtensionProperties(
   VkPhysicalDevice, const char *layer, uint32_t *count, VkExtensionProperties *props) {
   (void)layer;
   static const VkExtensionProperties exts[] = {
      {"VK_KHR_swapchain", 70},
      {"VK_KHR_maintenance1", 2},
      {"VK_KHR_image_format_list", 1},
   };
   uint32_t n = sizeof(exts) / sizeof(exts[0]);
   if (!props) { *count = n; return VK_SUCCESS; }
   n = std::min(n, *count);
   memcpy(props, exts, n * sizeof(VkExtensionProperties));
   *count = n;
   return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_EnumerateDeviceLayerProperties(VkPhysicalDevice, uint32_t *count, VkLayerProperties *) {
   *count = 0;
   return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_EnumeratePhysicalDevices(VkInstance inst, uint32_t *count, VkPhysicalDevice *devs) {
   (void)inst;
   static PhysicalDevice phys;
   if (!g_phys) {
      phys.type = VK_OBJECT_TYPE_PHYSICAL_DEVICE;
      phys.inst = from_handle<Instance>((uint64_t)inst);
      g_phys = &phys;
   }
   if (!devs) { *count = 1; return VK_SUCCESS; }
   if (*count < 1) return VK_INCOMPLETE;
   devs[0] = (VkPhysicalDevice)to_handle(g_phys);
   *count = 1;
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL vkgl_GetPhysicalDeviceFeatures(VkPhysicalDevice, VkPhysicalDeviceFeatures *f) {
   memset(f, 0, sizeof(*f));
   f->depthClamp = VK_TRUE;
   f->fillModeNonSolid = VK_TRUE;
   f->wideLines = VK_TRUE;
   f->largePoints = VK_TRUE;
   f->textureCompressionETC2 = VK_TRUE;
}

static VKAPI_ATTR void VKAPI_CALL vkgl_GetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties *p) {
   memset(p, 0, sizeof(*p));
   p->apiVersion = VK_API_VERSION_1_0;
   p->driverVersion = VK_MAKE_VERSION(1, 0, 0);
   p->vendorID = 0x0014;
   p->deviceID = 0x7a25;
   p->deviceType = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
   strncpy(p->deviceName, "LoongGPU LG110 (vkgl GLES2)", VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1);
   p->pipelineCacheUUID[0] = 0x4c; p->pipelineCacheUUID[1] = 0x47; p->pipelineCacheUUID[2] = 0x31; p->pipelineCacheUUID[3] = 0x31;
   VkPhysicalDeviceLimits &L = p->limits;
   L.maxImageDimension1D = 4096;
   L.maxImageDimension2D = 4096;
   L.maxImageDimension3D = 2048;
   L.maxImageDimensionCube = 4096;
   L.maxImageArrayLayers = 256;
   L.maxTexelBufferElements = 65536;
   L.maxUniformBufferRange = 65536;
   L.maxStorageBufferRange = 65536;
   L.maxPushConstantsSize = 128;
   L.maxMemoryAllocationCount = 4096;
   L.maxSamplerAllocationCount = 1024;
   L.bufferImageGranularity = 1;
   L.sparseAddressSpaceSize = 0;
   L.maxBoundDescriptorSets = 4;
   L.maxPerStageDescriptorSamplers = 16;
   L.maxPerStageDescriptorUniformBuffers = 12;
   L.maxPerStageDescriptorStorageBuffers = 4;
   L.maxPerStageDescriptorSampledImages = 16;
   L.maxPerStageDescriptorStorageImages = 4;
   L.maxPerStageDescriptorInputAttachments = 1;
   L.maxPerStageResources = 64;
   L.maxDescriptorSetSamplers = 16;
   L.maxDescriptorSetUniformBuffers = 12;
   L.maxDescriptorSetUniformBuffersDynamic = 8;
   L.maxDescriptorSetStorageBuffers = 4;
   L.maxDescriptorSetStorageBuffersDynamic = 4;
   L.maxDescriptorSetSampledImages = 16;
   L.maxDescriptorSetStorageImages = 4;
   L.maxDescriptorSetInputAttachments = 1;
   L.maxVertexInputAttributes = 16;
   L.maxVertexInputBindings = 16;
   L.maxVertexInputAttributeOffset = 2047;
   L.maxVertexInputBindingStride = 2048;
   L.maxVertexOutputComponents = 64;
   L.maxTessellationGenerationLevel = 0;
   L.maxTessellationPatchSize = 0;
   L.maxTessellationControlPerVertexInputComponents = 0;
   L.maxTessellationControlPerVertexOutputComponents = 0;
   L.maxTessellationControlPerPatchOutputComponents = 0;
   L.maxTessellationControlTotalOutputComponents = 0;
   L.maxTessellationEvaluationInputComponents = 0;
   L.maxTessellationEvaluationOutputComponents = 0;
   L.maxGeometryShaderInvocations = 0;
   L.maxGeometryInputComponents = 0;
   L.maxGeometryOutputComponents = 0;
   L.maxGeometryOutputVertices = 0;
   L.maxGeometryTotalOutputComponents = 0;
   L.maxFragmentInputComponents = 64;
   L.maxFragmentOutputAttachments = 1;
   L.maxFragmentDualSrcAttachments = 0;
   L.maxFragmentCombinedOutputResources = 8;
   L.maxComputeSharedMemorySize = 16384;
   L.maxComputeWorkGroupCount[0] = 65535;
   L.maxComputeWorkGroupCount[1] = 65535;
   L.maxComputeWorkGroupCount[2] = 65535;
   L.maxComputeWorkGroupInvocations = 128;
   L.maxComputeWorkGroupSize[0] = 128;
   L.maxComputeWorkGroupSize[1] = 128;
   L.maxComputeWorkGroupSize[2] = 64;
   L.subPixelPrecisionBits = 4;
   L.subTexelPrecisionBits = 4;
   L.mipmapPrecisionBits = 4;
   L.maxDrawIndexedIndexValue = 0xFFFFFFFFu;
   L.maxDrawIndirectCount = 1;
   L.maxSamplerLodBias = 2.0f;
   L.maxSamplerAnisotropy = 1.0f;
   L.maxViewports = 1;
   L.maxViewportDimensions[0] = 4096;
   L.maxViewportDimensions[1] = 4096;
   L.viewportBoundsRange[0] = -8192.0f;
   L.viewportBoundsRange[1] = 8191.0f;
   L.viewportSubPixelBits = 0;
   L.minMemoryMapAlignment = 64;
   L.minTexelBufferOffsetAlignment = 1;
   L.minUniformBufferOffsetAlignment = 1;
   L.minStorageBufferOffsetAlignment = 1;
   L.minTexelOffset = -8;
   L.maxTexelOffset = 7;
   L.minTexelGatherOffset = -8;
   L.maxTexelGatherOffset = 7;
   L.minInterpolationOffset = -0.5f;
   L.maxInterpolationOffset = 0.5f;
   L.subPixelInterpolationOffsetBits = 4;
   L.maxFramebufferWidth = 4096;
   L.maxFramebufferHeight = 4096;
   L.maxFramebufferLayers = 256;
   L.framebufferColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
   L.framebufferDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT;
   L.framebufferStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT;
   L.framebufferNoAttachmentsSampleCounts = VK_SAMPLE_COUNT_1_BIT;
   L.maxColorAttachments = 1;
   L.sampledImageColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
   L.sampledImageIntegerSampleCounts = VK_SAMPLE_COUNT_1_BIT;
   L.sampledImageDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT;
   L.sampledImageStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT;
   L.storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT;
   L.maxSampleMaskWords = 1;
   L.timestampComputeAndGraphics = VK_FALSE;
   L.timestampPeriod = 0.0f;
   L.maxClipDistances = 0;
   L.maxCullDistances = 0;
   L.maxCombinedClipAndCullDistances = 0;
   L.discreteQueuePriorities = 1;
   L.pointSizeRange[0] = 1.0f;
   L.pointSizeRange[1] = 64.0f;
   L.lineWidthRange[0] = 1.0f;
   L.lineWidthRange[1] = 1.0f;
   L.pointSizeGranularity = 1.0f;
   L.lineWidthGranularity = 0.0f;
   L.strictLines = VK_FALSE;
   L.standardSampleLocations = VK_TRUE;
   L.optimalBufferCopyOffsetAlignment = 1;
   L.optimalBufferCopyRowPitchAlignment = 1;
   L.nonCoherentAtomSize = 1;
   p->sparseProperties = {};
}

static VKAPI_ATTR void VKAPI_CALL vkgl_GetPhysicalDeviceQueueFamilyProperties(
   VkPhysicalDevice, uint32_t *count, VkQueueFamilyProperties *props) {
   if (!props) { *count = 1; return; }
   if (*count < 1) { *count = 0; return; }
   memset(&props[0], 0, sizeof(props[0]));
   props[0].queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
   props[0].queueCount = 1;
   props[0].timestampValidBits = 0;
   props[0].minImageTransferGranularity = {1, 1, 1};
   *count = 1;
}

static VKAPI_ATTR void VKAPI_CALL vkgl_GetPhysicalDeviceMemoryProperties(
   VkPhysicalDevice, VkPhysicalDeviceMemoryProperties *mp) {
   memset(mp, 0, sizeof(*mp));
   mp->memoryTypeCount = 2;
   mp->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   mp->memoryTypes[0].heapIndex = 0;
   mp->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   mp->memoryTypes[1].heapIndex = 0;
   mp->memoryHeapCount = 1;
   mp->memoryHeaps[0].size = 256ull * 1024 * 1024;
   mp->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
}

static VKAPI_ATTR void VKAPI_CALL vkgl_GetPhysicalDeviceFormatProperties(
   VkPhysicalDevice, VkFormat fmt, VkFormatProperties *p) {
   memset(p, 0, sizeof(*p));
   switch (fmt) {
   case VK_FORMAT_B8G8R8A8_UNORM:
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_B8G8R8A8_SRGB:
   case VK_FORMAT_R8G8B8A8_SRGB:
      p->optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
         VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
         VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
      p->linearTilingFeatures = p->optimalTilingFeatures;
      break;
   case VK_FORMAT_D16_UNORM:
   case VK_FORMAT_D32_SFLOAT:
   case VK_FORMAT_D24_UNORM_S8_UINT:
   case VK_FORMAT_D32_SFLOAT_S8_UINT:
      p->optimalTilingFeatures = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
      p->linearTilingFeatures = p->optimalTilingFeatures;
      break;
   default:
      p->optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
      p->linearTilingFeatures = p->optimalTilingFeatures;
      break;
   }
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_GetPhysicalDeviceImageFormatProperties(
   VkPhysicalDevice, VkFormat, VkImageType, VkImageTiling, VkImageUsageFlags,
   VkImageCreateFlags, VkImageFormatProperties *p) {
   memset(p, 0, sizeof(*p));
   p->maxExtent = {4096, 4096, 1};
   p->maxMipLevels = 1;
   p->maxArrayLayers = 1;
   p->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
   p->maxResourceSize = 256ull * 1024 * 1024;
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL vkgl_GetPhysicalDeviceSparseImageFormatProperties(
   VkPhysicalDevice, VkFormat, VkImageType, VkSampleCountFlagBits, VkImageUsageFlags,
   VkImageTiling, uint32_t *count, VkSparseImageFormatProperties *) {
   *count = 0;
}

static VKAPI_ATTR void VKAPI_CALL vkgl_GetPhysicalDeviceProperties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p) {
   (void)pd;
   vkgl_GetPhysicalDeviceProperties(pd, &p->properties);
}

// ---------------------------------------------------------------------------
// device / queue
// ---------------------------------------------------------------------------
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci,
                                                        const VkAllocationCallbacks *, VkDevice *out) {
   auto *d = new Device();
   d->type = VK_OBJECT_TYPE_DEVICE;
   d->phys = from_handle<PhysicalDevice>((uint64_t)pd);
   (void)ci;
   GlCtx::get();
   auto *q = new Queue();
   q->type = VK_OBJECT_TYPE_QUEUE;
   q->dev = d;
   g_device = d;
   g_queue = q;
   *out = (VkDevice)to_handle(d);
   reg(d, *out);
   VKGL_LOG("device created\n");
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL vkgl_DestroyDevice(VkDevice dev, const VkAllocationCallbacks *) {
   VKGL_LOG("DestroyDevice\n");
   auto *d = from_handle<Device>((uint64_t)dev);
   delete g_queue; g_queue = nullptr;
   if (g_device == d) g_device = nullptr;
   unreg((uint64_t)dev);
   delete d;
}

static VKAPI_ATTR void VKAPI_CALL vkgl_GetDeviceQueue(VkDevice, uint32_t, uint32_t, VkQueue *q) {
   VKGL_LOG("get device queue\n");
   *q = (VkQueue)to_handle(g_queue);
   reg(g_queue, *q);
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_DeviceWaitIdle(VkDevice) {
   GlCtx::get().make_current();
   glFinish();
   return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_QueueWaitIdle(VkQueue) {
   GlCtx::get().make_current();
   glFinish();
   return VK_SUCCESS;
}

// ---------------------------------------------------------------------------
// memory
// ---------------------------------------------------------------------------
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_AllocateMemory(VkDevice, const VkMemoryAllocateInfo *ai,
                                                          const VkAllocationCallbacks *, VkDeviceMemory *out) {
   auto *m = new Memory();
   m->type = VK_OBJECT_TYPE_DEVICE_MEMORY;
   m->size = ai->allocationSize;
   m->type_index = ai->memoryTypeIndex;
   m->props = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   m->host_visible = true;
   m->shadow.resize((size_t)m->size);
   *out = (VkDeviceMemory)to_handle(m);
   reg(m, *out);
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL vkgl_FreeMemory(VkDevice, VkDeviceMemory mem, const VkAllocationCallbacks *) {
   auto *m = from_handle<Memory>((uint64_t)mem);
   GlCtx::get().make_current();
   if (m->gl_buffer) glDeleteBuffers(1, &m->gl_buffer);
   if (m->gl_texture) glDeleteTextures(1, &m->gl_texture);
   unreg((uint64_t)mem);
   delete m;
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_MapMemory(VkDevice, VkDeviceMemory mem, VkDeviceSize off,
                                                     VkDeviceSize size, VkMemoryMapFlags, void **ptr) {
   auto *m = from_handle<Memory>((uint64_t)mem);
   if (off >= m->size) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   if (size == VK_WHOLE_SIZE || off + size > m->size) size = m->size - off;
   m->mapped = true;
   *ptr = m->shadow.data() + off;
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL vkgl_UnmapMemory(VkDevice, VkDeviceMemory mem) {
   auto *m = from_handle<Memory>((uint64_t)mem);
   m->mapped = false;
   m->revision++;
}

static VKAPI_ATTR void VKAPI_CALL vkgl_FlushMappedMemoryRanges(VkDevice, uint32_t, const VkMappedMemoryRange *) {}
static VKAPI_ATTR void VKAPI_CALL vkgl_InvalidateMappedMemoryRanges(VkDevice, uint32_t, const VkMappedMemoryRange *) {}

// ---------------------------------------------------------------------------
// buffers / images
// ---------------------------------------------------------------------------
static VKAPI_ATTR void VKAPI_CALL vkgl_GetBufferMemoryRequirements(VkDevice, VkBuffer buf, VkMemoryRequirements *r) {
   auto *b = from_handle<Buffer>((uint64_t)buf);
   r->size = b->size;
   r->alignment = 64;
   r->memoryTypeBits = 0x3;
}

static VKAPI_ATTR void VKAPI_CALL vkgl_GetImageMemoryRequirements(VkDevice, VkImage img, VkMemoryRequirements *r) {
   auto *im = from_handle<Image>((uint64_t)img);
   r->size = (VkDeviceSize)im->extent.width * im->extent.height * 4;
   r->alignment = 64;
   r->memoryTypeBits = 0x3;
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_CreateBuffer(VkDevice, const VkBufferCreateInfo *ci,
                                                        const VkAllocationCallbacks *, VkBuffer *out) {
   auto *b = new Buffer();
   b->type = VK_OBJECT_TYPE_BUFFER;
   b->size = ci->size;
   b->usage = ci->usage;
   *out = (VkBuffer)to_handle(b);
   reg(b, *out);
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL vkgl_DestroyBuffer(VkDevice, VkBuffer buf, const VkAllocationCallbacks *) {
   auto *b = from_handle<Buffer>((uint64_t)buf);
   if (b->gl) { GlCtx::get().make_current(); glDeleteBuffers(1, &b->gl); }
   unreg((uint64_t)buf);
   delete b;
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_BindBufferMemory(VkDevice, VkBuffer buf, VkDeviceMemory mem, VkDeviceSize off) {
   auto *b = from_handle<Buffer>((uint64_t)buf);
   b->mem = from_handle<Memory>((uint64_t)mem);
   b->mem_offset = off;
   b->uploaded_revision = 0;
   ensure_gl_buffer(b);
   return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_CreateImage(VkDevice, const VkImageCreateInfo *ci,
                                                       const VkAllocationCallbacks *, VkImage *out) {
   VKGL_LOG("CreateImage fmt=%d %ux%u tiling=%d usage=0x%x\n", ci->format, ci->extent.width, ci->extent.height, ci->tiling, ci->usage);
   auto *im = new Image();
   im->type = VK_OBJECT_TYPE_IMAGE;
   im->format = ci->format;
   im->extent = ci->extent;
   im->usage = ci->usage;
   im->tiling = ci->tiling;
   *out = (VkImage)to_handle(im);
   reg(im, *out);
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL vkgl_DestroyImage(VkDevice, VkImage img, const VkAllocationCallbacks *) {
   auto *im = from_handle<Image>((uint64_t)img);
   GlCtx::get().make_current();
   if (im->fbo) glDeleteFramebuffers(1, &im->fbo);
   if (im->tex) glDeleteTextures(1, &im->tex);
   unreg((uint64_t)img);
   delete im;
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_BindImageMemory(VkDevice, VkImage img, VkDeviceMemory mem, VkDeviceSize off) {
   auto *im = from_handle<Image>((uint64_t)img);
   im->mem = from_handle<Memory>((uint64_t)mem);
   im->mem_offset = off;
   im->uploaded_revision = 0;
   ensure_gl_texture(im);
   return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_CreateImageView(VkDevice, const VkImageViewCreateInfo *ci,
                                                           const VkAllocationCallbacks *, VkImageView *out) {
   VKGL_LOG("CreateImageView img=%p fmt=%d\n", (void*)ci->image, ci->format);
   auto *v = new ImageView();
   v->type = VK_OBJECT_TYPE_IMAGE_VIEW;
   v->image = from_handle<Image>((uint64_t)ci->image);
   v->format = ci->format;
   *out = (VkImageView)to_handle(v);
   reg(v, *out);
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL vkgl_DestroyImageView(VkDevice, VkImageView v, const VkAllocationCallbacks *) {
   unreg((uint64_t)v);
   delete from_handle<ImageView>((uint64_t)v);
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_CreateSampler(VkDevice, const VkSamplerCreateInfo *ci,
                                                         const VkAllocationCallbacks *, VkSampler *out) {
   auto *s = new Obj();
   s->type = VK_OBJECT_TYPE_SAMPLER;
   *out = (VkSampler)to_handle(s);
   reg(s, *out);
   (void)ci;
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL vkgl_DestroySampler(VkDevice, VkSampler s, const VkAllocationCallbacks *) {
   unreg((uint64_t)s);
   delete from_handle<Obj>((uint64_t)s);
}

// ---------------------------------------------------------------------------
// render pass / framebuffer
// ---------------------------------------------------------------------------
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_CreateRenderPass(VkDevice, const VkRenderPassCreateInfo *ci,
                                                            const VkAllocationCallbacks *, VkRenderPass *out) {
   VKGL_LOG("CreateRenderPass attachments=%u subpasses=%u\n", ci->attachmentCount, ci->subpassCount);
   auto *rp = new RenderPass();
   rp->type = VK_OBJECT_TYPE_RENDER_PASS;
   for (uint32_t i = 0; i < ci->attachmentCount; i++) {
      const VkAttachmentDescription &a = ci->pAttachments[i];
      bool depth = (a.format == VK_FORMAT_D16_UNORM || a.format == VK_FORMAT_D32_SFLOAT ||
                    a.format == VK_FORMAT_D24_UNORM_S8_UINT || a.format == VK_FORMAT_D32_SFLOAT_S8_UINT);
      if (depth) {
         rp->depth_format = a.format;
         rp->depth_load = a.loadOp;
      } else {
         rp->color_format = a.format;
         rp->color_load = a.loadOp;
         rp->color_store = a.storeOp;
      }
   }
   *out = (VkRenderPass)to_handle(rp);
   reg(rp, *out);
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL vkgl_DestroyRenderPass(VkDevice, VkRenderPass rp, const VkAllocationCallbacks *) {
   unreg((uint64_t)rp);
   delete from_handle<RenderPass>((uint64_t)rp);
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_CreateFramebuffer(VkDevice, const VkFramebufferCreateInfo *ci,
                                                             const VkAllocationCallbacks *, VkFramebuffer *out) {
   VKGL_LOG("CreateFramebuffer %ux%u attachments=%u\n", ci->width, ci->height, ci->attachmentCount);
   auto *fb = new Framebuffer();
   fb->type = VK_OBJECT_TYPE_FRAMEBUFFER;
   fb->rp = from_handle<RenderPass>((uint64_t)ci->renderPass);
   fb->width = ci->width;
   fb->height = ci->height;
   for (uint32_t i = 0; i < ci->attachmentCount; i++) {
      auto *v = from_handle<ImageView>((uint64_t)ci->pAttachments[i]);
      if (!v) continue;
      bool depth = v->image && (v->image->format == VK_FORMAT_D16_UNORM ||
                                v->image->format == VK_FORMAT_D32_SFLOAT ||
                                v->image->format == VK_FORMAT_D24_UNORM_S8_UINT ||
                                v->image->format == VK_FORMAT_D32_SFLOAT_S8_UINT);
      if (depth) fb->depth = v; else fb->color = v;
   }
   *out = (VkFramebuffer)to_handle(fb);
   reg(fb, *out);
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL vkgl_DestroyFramebuffer(VkDevice, VkFramebuffer fb, const VkAllocationCallbacks *) {
   unreg((uint64_t)fb);
   delete from_handle<Framebuffer>((uint64_t)fb);
}

// ---------------------------------------------------------------------------
// shader modules / pipeline layout / descriptors
// ---------------------------------------------------------------------------
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_CreateShaderModule(VkDevice, const VkShaderModuleCreateInfo *ci,
                                                              const VkAllocationCallbacks *, VkShaderModule *out) {
   VKGL_LOG("CreateShaderModule bytes=%zu\n", ci->codeSize);
   auto *m = new ShaderModule();
   m->type = VK_OBJECT_TYPE_SHADER_MODULE;
   m->code.assign(ci->pCode, ci->pCode + ci->codeSize / 4);
   *out = (VkShaderModule)to_handle(m);
   reg(m, *out);
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL vkgl_DestroyShaderModule(VkDevice, VkShaderModule m, const VkAllocationCallbacks *) {
   unreg((uint64_t)m);
   delete from_handle<ShaderModule>((uint64_t)m);
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_CreateDescriptorSetLayout(VkDevice, const VkDescriptorSetLayoutCreateInfo *ci,
                                                                     const VkAllocationCallbacks *, VkDescriptorSetLayout *out) {
   VKGL_LOG("CreateDescriptorSetLayout bindings=%u\n", ci->bindingCount);
   auto *l = new DescriptorSetLayout();
   l->type = VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT;
   l->bindings.assign(ci->pBindings, ci->pBindings + ci->bindingCount);
   *out = (VkDescriptorSetLayout)to_handle(l);
   reg(l, *out);
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL vkgl_DestroyDescriptorSetLayout(VkDevice, VkDescriptorSetLayout l, const VkAllocationCallbacks *) {
   unreg((uint64_t)l);
   delete from_handle<DescriptorSetLayout>((uint64_t)l);
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_CreatePipelineLayout(VkDevice, const VkPipelineLayoutCreateInfo *ci,
                                                                const VkAllocationCallbacks *, VkPipelineLayout *out) {
   VKGL_LOG("CreatePipelineLayout sets=%u push=%u\n", ci->setLayoutCount, ci->pushConstantRangeCount);
   auto *l = new PipelineLayout();
   l->type = VK_OBJECT_TYPE_PIPELINE_LAYOUT;
   l->info = *ci;
   for (uint32_t i = 0; i < ci->setLayoutCount; i++)
      l->set_layouts.push_back((VkDescriptorSetLayout)ci->pSetLayouts[i]);
   if (ci->pPushConstantRanges)
      l->push_ranges.assign(ci->pPushConstantRanges, ci->pPushConstantRanges + ci->pushConstantRangeCount);
   *out = (VkPipelineLayout)to_handle(l);
   reg(l, *out);
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL vkgl_DestroyPipelineLayout(VkDevice, VkPipelineLayout l, const VkAllocationCallbacks *) {
   unreg((uint64_t)l);
   delete from_handle<PipelineLayout>((uint64_t)l);
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_CreateDescriptorPool(VkDevice, const VkDescriptorPoolCreateInfo *ci,
                                                                const VkAllocationCallbacks *, VkDescriptorPool *out) {
   VKGL_LOG("CreateDescriptorPool sizes=%u maxSets=%u\n", ci->poolSizeCount, ci->maxSets);
   auto *p = new DescriptorPool();
   p->type = VK_OBJECT_TYPE_DESCRIPTOR_POOL;
   p->sizes.assign(ci->pPoolSizes, ci->pPoolSizes + ci->poolSizeCount);
   *out = (VkDescriptorPool)to_handle(p);
   reg(p, *out);
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL vkgl_DestroyDescriptorPool(VkDevice, VkDescriptorPool p, const VkAllocationCallbacks *) {
   unreg((uint64_t)p);
   delete from_handle<DescriptorPool>((uint64_t)p);
}

static VKAPI_ATTR void VKAPI_CALL vkgl_ResetDescriptorPool(VkDevice, VkDescriptorPool, VkDescriptorPoolResetFlags) {}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_AllocateDescriptorSets(VkDevice, const VkDescriptorSetAllocateInfo *ai,
                                                                  VkDescriptorSet *sets) {
   auto *pool = from_handle<DescriptorPool>((uint64_t)ai->descriptorPool);
   for (uint32_t i = 0; i < ai->descriptorSetCount; i++) {
      auto *s = new DescriptorSet();
      s->type = VK_OBJECT_TYPE_DESCRIPTOR_SET;
      s->pool = pool;
      s->layout = from_handle<DescriptorSetLayout>((uint64_t)ai->pSetLayouts[i]);
      s->buffers.resize(s->layout->bindings.size());
      s->images.resize(s->layout->bindings.size());
      s->written.resize(s->layout->bindings.size(), 0);
      sets[i] = (VkDescriptorSet)to_handle(s);
      reg(s, (uint64_t)sets[i]);
   }
   return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_FreeDescriptorSets(VkDevice, VkDescriptorPool, uint32_t n, const VkDescriptorSet *sets) {
   for (uint32_t i = 0; i < n; i++) {
      unreg((uint64_t)sets[i]);
      delete from_handle<DescriptorSet>((uint64_t)sets[i]);
   }
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL vkgl_UpdateDescriptorSets(VkDevice, uint32_t writes_n, const VkWriteDescriptorSet *writes,
                                                            uint32_t, const VkCopyDescriptorSet *) {
   for (uint32_t i = 0; i < writes_n; i++) {
      const auto &w = writes[i];
      auto *s = from_handle<DescriptorSet>((uint64_t)w.dstSet);
      if (!s) continue;
      uint32_t idx = 0;
      for (; idx < s->layout->bindings.size(); idx++)
         if (s->layout->bindings[idx].binding == w.dstBinding) break;
      if (idx >= s->layout->bindings.size()) continue;
      if (w.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && w.pBufferInfo)
         s->buffers[idx] = w.pBufferInfo[0];
      if ((w.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
           w.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) && w.pImageInfo)
         s->images[idx] = w.pImageInfo[0];
      s->written[idx] = 1;
   }
}

// ---------------------------------------------------------------------------
// command pools / buffers (recording only; execution in submit)
// ---------------------------------------------------------------------------
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_CreateCommandPool(VkDevice dev, const VkCommandPoolCreateInfo *,
                                                            const VkAllocationCallbacks *, VkCommandPool *out) {
   VKGL_LOG("CreateCommandPool\n");
   auto *p = new CommandPool();
   p->type = VK_OBJECT_TYPE_COMMAND_POOL;
   p->dev = from_handle<Device>((uint64_t)dev);
   *out = (VkCommandPool)to_handle(p);
   reg(p, *out);
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL vkgl_DestroyCommandPool(VkDevice, VkCommandPool p, const VkAllocationCallbacks *) {
   unreg((uint64_t)p);
   delete from_handle<CommandPool>((uint64_t)p);
}

static VKAPI_ATTR void VKAPI_CALL vkgl_ResetCommandPool(VkDevice, VkCommandPool, VkCommandPoolResetFlags) {}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_AllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo *ai,
                                                                 VkCommandBuffer *out) {
   auto *pool = from_handle<CommandPool>((uint64_t)ai->commandPool);
   for (uint32_t i = 0; i < ai->commandBufferCount; i++) {
      auto *cb = new CommandBuffer();
      cb->type = VK_OBJECT_TYPE_COMMAND_BUFFER;
      cb->pool = pool;
      out[i] = (VkCommandBuffer)to_handle(cb);
      reg(cb, (uint64_t)out[i]);
   }
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL vkgl_FreeCommandBuffers(VkDevice, VkCommandPool, uint32_t n, const VkCommandBuffer *cbs) {
   for (uint32_t i = 0; i < n; i++) {
      unreg((uint64_t)cbs[i]);
      delete from_handle<CommandBuffer>((uint64_t)cbs[i]);
   }
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *) {
   auto *c = from_handle<CommandBuffer>((uint64_t)cb);
   c->cmds.clear();
   c->recording = true;
   c->recorded = false;
   return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_EndCommandBuffer(VkCommandBuffer cb) {
   auto *c = from_handle<CommandBuffer>((uint64_t)cb);
   c->recording = false;
   c->recorded = true;
   return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_ResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags) {
   auto *c = from_handle<CommandBuffer>((uint64_t)cb);
   c->cmds.clear();
   c->recorded = false;
   return VK_SUCCESS;
}

// ---------------------------------------------------------------------------
// command recording
// ---------------------------------------------------------------------------
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi,
                                                          VkSubpassContents) {
   auto *c = from_handle<CommandBuffer>((uint64_t)cb);
   Cmd cmd{};
   cmd.kind = Cmd::BeginRenderPass;
   cmd.fb = from_handle<Framebuffer>((uint64_t)bi->framebuffer);
   cmd.rp = from_handle<RenderPass>((uint64_t)bi->renderPass);
   for (uint32_t i = 0; i < bi->clearValueCount; i++) cmd.clears.push_back(bi->pClearValues[i]);
   c->cmds.push_back(std::move(cmd));
   c->active_fb = cmd.fb;
   c->active_rp = cmd.rp;
}

static VKAPI_ATTR void VKAPI_CALL vkgl_CmdEndRenderPass(VkCommandBuffer cb) {
   auto *c = from_handle<CommandBuffer>((uint64_t)cb);
   Cmd cmd{}; cmd.kind = Cmd::EndRenderPass;
   c->cmds.push_back(cmd);
   c->active_fb = nullptr;
   c->active_rp = nullptr;
}

static VKAPI_ATTR void VKAPI_CALL vkgl_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline pipe) {
   auto *c = from_handle<CommandBuffer>((uint64_t)cb);
   Cmd cmd{}; cmd.kind = Cmd::BindPipeline; cmd.pipe = from_handle<Pipeline>((uint64_t)pipe);
   c->cmds.push_back(cmd);
   c->cur_pipe = cmd.pipe;
   (void)bp;
}

static VKAPI_ATTR void VKAPI_CALL vkgl_CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint, VkPipelineLayout,
                                                             uint32_t first, uint32_t n, const VkDescriptorSet *sets,
                                                             uint32_t dyn_count, const uint32_t *dyn_offsets) {
   auto *c = from_handle<CommandBuffer>((uint64_t)cb);
   for (uint32_t i = 0; i < n; i++) {
      Cmd cmd{}; cmd.kind = Cmd::BindDescriptorSet;
      cmd.set = from_handle<DescriptorSet>((uint64_t)sets[i]);
      cmd.first = first + i;
      if (cmd.set && dyn_count && dyn_offsets) {
         cmd.set->dynamic_offsets.clear();
         uint32_t di = 0;
         for (auto &b : cmd.set->layout->bindings) {
            if ((b.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
                 b.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) && di < dyn_count)
               cmd.set->dynamic_offsets.push_back({b.binding, dyn_offsets[di++]});
         }
      }
      c->cmds.push_back(cmd);
   }
}

static VKAPI_ATTR void VKAPI_CALL vkgl_CmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t n,
                                                            const VkBuffer *bufs, const VkDeviceSize *offsets) {
   auto *c = from_handle<CommandBuffer>((uint64_t)cb);
   for (uint32_t i = 0; i < n; i++) {
      Cmd cmd{}; cmd.kind = Cmd::BindVertexBuffer;
      cmd.buf = from_handle<Buffer>((uint64_t)bufs[i]);
      cmd.offset = offsets ? offsets[i] : 0;
      cmd.first = first + i;
      c->cmds.push_back(cmd);
   }
}

static VKAPI_ATTR void VKAPI_CALL vkgl_CmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkIndexType) {
   auto *c = from_handle<CommandBuffer>((uint64_t)cb);
   Cmd cmd{}; cmd.kind = Cmd::BindIndexBuffer; cmd.buf = from_handle<Buffer>((uint64_t)buf); cmd.offset = off;
   c->cmds.push_back(cmd);
}

static VKAPI_ATTR void VKAPI_CALL vkgl_CmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport *vps) {
   auto *c = from_handle<CommandBuffer>((uint64_t)cb);
   for (uint32_t i = 0; i < n; i++) {
      Cmd cmd{}; cmd.kind = Cmd::SetViewport; cmd.vp = vps[i]; cmd.first = first + i;
      c->cmds.push_back(cmd);
      c->cur_vp = vps[i];
   }
}

static VKAPI_ATTR void VKAPI_CALL vkgl_CmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D *rs) {
   auto *c = from_handle<CommandBuffer>((uint64_t)cb);
   for (uint32_t i = 0; i < n; i++) {
      Cmd cmd{}; cmd.kind = Cmd::SetScissor; cmd.scissor = rs[i]; cmd.first = first + i;
      c->cmds.push_back(cmd);
      c->cur_scissor = rs[i];
   }
}

static VKAPI_ATTR void VKAPI_CALL vkgl_CmdDraw(VkCommandBuffer cb, uint32_t vertex_count, uint32_t instance_count,
                                               uint32_t first_vertex, uint32_t) {
   auto *c = from_handle<CommandBuffer>((uint64_t)cb);
   Cmd cmd{}; cmd.kind = Cmd::Draw; cmd.first = first_vertex; cmd.count = vertex_count;
   cmd.instances = instance_count;
   c->cmds.push_back(cmd);
}

static VKAPI_ATTR void VKAPI_CALL vkgl_CmdDrawIndexed(VkCommandBuffer cb, uint32_t count, uint32_t instances,
                                                      uint32_t first, int32_t voff, uint32_t) {
   auto *c = from_handle<CommandBuffer>((uint64_t)cb);
   Cmd cmd{}; cmd.kind = Cmd::DrawIndexed; cmd.count = count; cmd.instances = instances;
   cmd.first = first; cmd.vertex_offset = voff;
   c->cmds.push_back(cmd);
}

static VKAPI_ATTR void VKAPI_CALL vkgl_CmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags,
                                                          VkDependencyFlags, uint32_t, const VkMemoryBarrier *,
                                                          uint32_t, const VkBufferMemoryBarrier *,
                                                          uint32_t, const VkImageMemoryBarrier *) {}

static VKAPI_ATTR void VKAPI_CALL vkgl_CmdClearColorImage(VkCommandBuffer cb, VkImage img, VkImageLayout,
                                                          const VkClearColorValue *color, uint32_t n,
                                                          const VkImageSubresourceRange *ranges) {
   auto *c = from_handle<CommandBuffer>((uint64_t)cb);
   Cmd cmd{}; cmd.kind = Cmd::ClearColorImage; cmd.img = from_handle<Image>((uint64_t)img);
   cmd.cc = *color;
   if (n) cmd.range = ranges[0];
   c->cmds.push_back(cmd);
}

static VKAPI_ATTR void VKAPI_CALL vkgl_CmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst,
                                                     uint32_t n, const VkBufferCopy *regions) {
   auto *c = from_handle<CommandBuffer>((uint64_t)cb);
   Cmd cmd{}; cmd.kind = Cmd::CopyBuffer;
   cmd.src = from_handle<Buffer>((uint64_t)src);
   cmd.buf = from_handle<Buffer>((uint64_t)dst);
   for (uint32_t i = 0; i < n; i++) cmd.copies.push_back(regions[i]);
   c->cmds.push_back(cmd);
}

static VKAPI_ATTR void VKAPI_CALL vkgl_CmdDrawIndirect(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off,
                                                       uint32_t count, uint32_t stride) {
   auto *c = from_handle<CommandBuffer>((uint64_t)cb);
   Cmd cmd{}; cmd.kind = Cmd::DrawIndirect; cmd.buf = from_handle<Buffer>((uint64_t)buf);
   cmd.offset = off; cmd.count = count; cmd.indirect_stride = stride ? stride : sizeof(VkDrawIndirectCommand);
   c->cmds.push_back(cmd);
}

static VKAPI_ATTR void VKAPI_CALL vkgl_CmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off,
                                                              uint32_t count, uint32_t stride) {
   auto *c = from_handle<CommandBuffer>((uint64_t)cb);
   Cmd cmd{}; cmd.kind = Cmd::DrawIndexedIndirect; cmd.buf = from_handle<Buffer>((uint64_t)buf);
   cmd.offset = off; cmd.count = count; cmd.indirect_stride = stride ? stride : sizeof(VkDrawIndexedIndirectCommand);
   c->cmds.push_back(cmd);
}

static VKAPI_ATTR void VKAPI_CALL vkgl_CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer buf, VkImage img,
                                                            VkImageLayout, uint32_t n,
                                                            const VkBufferImageCopy *regions) {
   auto *c = from_handle<CommandBuffer>((uint64_t)cb);
   for (uint32_t i = 0; i < n; i++) {
      Cmd cmd{}; cmd.kind = Cmd::CopyBufferToImage;
      cmd.buf = from_handle<Buffer>((uint64_t)buf);
      cmd.img = from_handle<Image>((uint64_t)img);
      cmd.region = regions[i];
      c->cmds.push_back(cmd);
   }
}
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdCopyImageToBuffer(VkCommandBuffer cb, VkImage img, VkImageLayout,
                                                            VkBuffer buf, uint32_t n,
                                                            const VkBufferImageCopy *regions) {
   auto *c = from_handle<CommandBuffer>((uint64_t)cb);
   for (uint32_t i = 0; i < n; i++) {
      Cmd cmd{}; cmd.kind = Cmd::CopyImageToBuffer;
      cmd.buf = from_handle<Buffer>((uint64_t)buf);
      cmd.img = from_handle<Image>((uint64_t)img);
      cmd.region = regions[i];
      c->cmds.push_back(cmd);
   }
}
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdUpdateBuffer(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off,
                                                       VkDeviceSize size, const void *data) {
   auto *b = from_handle<Buffer>((uint64_t)buf);
   if (b && b->mem && off + size <= b->size &&
       b->mem_offset + off + size <= b->mem->shadow.size()) {
      memcpy(b->mem->shadow.data() + b->mem_offset + off, data, (size_t)size);
      b->mem->revision++;
   }
}

// ---------------------------------------------------------------------------
// fences / semaphores / events / queries
// ---------------------------------------------------------------------------
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_CreateFence(VkDevice, const VkFenceCreateInfo *ci,
                                                       const VkAllocationCallbacks *, VkFence *out) {
   auto *f = new Fence();
   f->type = VK_OBJECT_TYPE_FENCE;
   f->signaled = ci && (ci->flags & VK_FENCE_CREATE_SIGNALED_BIT);
   *out = (VkFence)to_handle(f);
   reg(f, *out);
   return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL vkgl_DestroyFence(VkDevice, VkFence f, const VkAllocationCallbacks *) {
   unreg((uint64_t)f);
   delete from_handle<Fence>((uint64_t)f);
}
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_GetFenceStatus(VkDevice, VkFence f) {
   return from_handle<Fence>((uint64_t)f)->signaled ? VK_SUCCESS : VK_NOT_READY;
}
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_ResetFences(VkDevice, uint32_t n, const VkFence *fs) {
   for (uint32_t i = 0; i < n; i++) from_handle<Fence>((uint64_t)fs[i])->signaled = false;
   return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_WaitForFences(VkDevice, uint32_t n, const VkFence *fs,
                                                         VkBool32, uint64_t) {
   for (uint32_t i = 0; i < n; i++) from_handle<Fence>((uint64_t)fs[i])->signaled = true;
   return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_CreateSemaphore(VkDevice, const VkSemaphoreCreateInfo *,
                                                           const VkAllocationCallbacks *, VkSemaphore *out) {
   auto *s = new Semaphore();
   s->type = VK_OBJECT_TYPE_SEMAPHORE;
   *out = (VkSemaphore)to_handle(s);
   reg(s, *out);
   return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL vkgl_DestroySemaphore(VkDevice, VkSemaphore s, const VkAllocationCallbacks *) {
   unreg((uint64_t)s);
   delete from_handle<Semaphore>((uint64_t)s);
}
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_CreateEvent(VkDevice, const VkEventCreateInfo *, const VkAllocationCallbacks *, VkEvent *out) {
   auto *e = new Obj(); e->type = VK_OBJECT_TYPE_EVENT;
   *out = (VkEvent)to_handle(e); reg(e, (uint64_t)*out); return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL vkgl_DestroyEvent(VkDevice, VkEvent e, const VkAllocationCallbacks *) {
   unreg((uint64_t)e); delete from_handle<Obj>((uint64_t)e);
}
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_GetEventStatus(VkDevice, VkEvent) { return VK_EVENT_SET; }
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_SetEvent(VkDevice, VkEvent) { return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_ResetEvent(VkDevice, VkEvent) { return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_CreateQueryPool(VkDevice, const VkQueryPoolCreateInfo *, const VkAllocationCallbacks *, VkQueryPool *out) {
   auto *q = new Obj(); q->type = VK_OBJECT_TYPE_QUERY_POOL;
   *out = (VkQueryPool)to_handle(q); reg(q, (uint64_t)*out); return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL vkgl_DestroyQueryPool(VkDevice, VkQueryPool q, const VkAllocationCallbacks *) {
   unreg((uint64_t)q); delete from_handle<Obj>((uint64_t)q);
}
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_GetQueryPoolResults(VkDevice, VkQueryPool, uint32_t, uint32_t,
                                                               size_t, void *data, VkDeviceSize, VkQueryResultFlags) {
   memset(data, 0, 0);
   return VK_SUCCESS;
}

// ---------------------------------------------------------------------------
// execution
// ---------------------------------------------------------------------------
static void execute_cmds(CommandBuffer *c) {
   GlCtx::get().make_current();
   Pipeline *cur_pipe = nullptr;
   std::vector<DescriptorSet *> sets;
   Buffer *vbuf = nullptr; VkDeviceSize voff = 0;
   Buffer *ibuf = nullptr; VkDeviceSize ioff = 0;
   VkViewport vp = {0, 0, 0, 0, 0, 1};
   VkRect2D sc = {{0, 0}, {0, 0}};
   int fb_height = 0;

   for (auto &cmd : c->cmds) {
      switch (cmd.kind) {
      case Cmd::BeginRenderPass: {
         Framebuffer *fb = cmd.fb;
         if (!fb) break;
         uint32_t w = fb->width, h = fb->height;
         GLuint fbo = 0;
         if (fb->color && fb->color->image) { ensure_fbo(fb->color->image); fbo = fb->color->image->fbo; }
         glBindFramebuffer(GL_FRAMEBUFFER, fbo);
         glViewport(0, 0, (GLsizei)w, (GLsizei)h);
         vp = {0, 0, (float)w, (float)h, 0, 1};
         sc = {{0, 0}, {w, h}};
         fb_height = (int)h;
         if (fb->depth && fb->depth->image) {
            ensure_fbo(fb->depth->image);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, fb->depth->image->tex, 0);
            glEnable(GL_DEPTH_TEST);
         } else {
            glDisable(GL_DEPTH_TEST);
         }
         glDisable(GL_SCISSOR_TEST);
         /* The common single-color + depth case needs one combined clear.
          * Set both values first; keep the original fallback for every other
          * attachment/clear arrangement. */
         if (fb->color && fb->depth && cmd.clears.size() == 2) {
            const VkClearValue &color = cmd.clears[0];
            const VkClearValue &depth = cmd.clears[1];
            glClearColor(color.color.float32[0], color.color.float32[1],
                         color.color.float32[2], color.color.float32[3]);
            glClearDepthf(depth.depthStencil.depth);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
         } else {
            for (size_t i = 0; i < cmd.clears.size(); i++) {
               const VkClearValue &cv = cmd.clears[i];
               bool is_depth = fb->depth && i >= (cmd.clears.size() - (fb->depth ? 1 : 0));
               if (is_depth) {
                  glClearDepthf(cv.depthStencil.depth);
                  glClear(GL_DEPTH_BUFFER_BIT);
               } else {
                  glClearColor(cv.color.float32[0], cv.color.float32[1], cv.color.float32[2], cv.color.float32[3]);
                  GLbitfield bits = GL_COLOR_BUFFER_BIT;
                  if (fb->depth) bits |= GL_DEPTH_BUFFER_BIT;
                  glClear(bits);
               }
            }
            if (!cmd.clears.empty() && fb->depth) {
               glClearDepthf(cmd.clears.back().depthStencil.depth);
               glClear(GL_DEPTH_BUFFER_BIT);
            }
         }
         glBindFramebuffer(GL_FRAMEBUFFER, fbo);
         break;
      }
      case Cmd::EndRenderPass:
         glBindFramebuffer(GL_FRAMEBUFFER, 0);
         break;
      case Cmd::BindPipeline:
         cur_pipe = cmd.pipe;
         break;
      case Cmd::BindDescriptorSet:
         if (sets.size() <= cmd.first) sets.resize(cmd.first + 1, nullptr);
         sets[cmd.first] = cmd.set;
         break;
      case Cmd::BindVertexBuffer:
         vbuf = cmd.buf; voff = cmd.offset;
         break;
      case Cmd::BindIndexBuffer:
         ibuf = cmd.buf; ioff = cmd.offset;
         break;
      case Cmd::SetViewport:
         vp = cmd.vp;
         /* Vulkan viewport origin is top-left, GL is bottom-left */
         glViewport((GLint)vp.x, (GLint)(fb_height - vp.y - vp.height),
                    (GLsizei)vp.width, (GLsizei)vp.height);
         break;
      case Cmd::SetScissor:
         sc = cmd.scissor;
         glEnable(GL_SCISSOR_TEST);
         glScissor(sc.offset.x, sc.offset.y, (GLsizei)sc.extent.width, (GLsizei)sc.extent.height);
         break;
      case Cmd::DrawIndirect:
         if (cur_pipe && cmd.buf && cmd.buf->mem) {
            const uint8_t *base = cmd.buf->mem->shadow.data() + cmd.buf->mem_offset + cmd.offset;
            for (uint32_t i = 0; i < cmd.count; i++) {
               const VkDrawIndirectCommand *d = (const VkDrawIndirectCommand *)(base + i * cmd.indirect_stride);
               Cmd sub = cmd; sub.kind = Cmd::Draw;
               sub.first = d->firstVertex; sub.count = d->vertexCount; sub.instances = d->instanceCount;
               extern void vkgl_execute_draw(Pipeline *, std::vector<DescriptorSet *> &, Buffer *, VkDeviceSize,
                                             Buffer *, VkDeviceSize, const Cmd &);
               vkgl_execute_draw(cur_pipe, sets, vbuf, voff, ibuf, ioff, sub);
            }
         }
         break;
      case Cmd::DrawIndexedIndirect:
         if (cur_pipe && cmd.buf && cmd.buf->mem) {
            const uint8_t *base = cmd.buf->mem->shadow.data() + cmd.buf->mem_offset + cmd.offset;
            for (uint32_t i = 0; i < cmd.count; i++) {
               const VkDrawIndexedIndirectCommand *d = (const VkDrawIndexedIndirectCommand *)(base + i * cmd.indirect_stride);
               Cmd sub = cmd; sub.kind = Cmd::DrawIndexed;
               sub.first = d->firstIndex; sub.count = d->indexCount;
               sub.vertex_offset = d->vertexOffset; sub.instances = d->instanceCount;
               extern void vkgl_execute_draw(Pipeline *, std::vector<DescriptorSet *> &, Buffer *, VkDeviceSize,
                                             Buffer *, VkDeviceSize, const Cmd &);
               vkgl_execute_draw(cur_pipe, sets, vbuf, voff, ibuf, ioff, sub);
            }
         }
         break;
      case Cmd::Draw:
      case Cmd::DrawIndexed:
         if (cur_pipe) {
            // draw support is provided by the pipeline module (stage 2)
            extern void vkgl_execute_draw(Pipeline *, std::vector<DescriptorSet *> &, Buffer *, VkDeviceSize,
                                          Buffer *, VkDeviceSize, const Cmd &);
            vkgl_execute_draw(cur_pipe, sets, vbuf, voff, ibuf, ioff, cmd);
         }
         break;
      case Cmd::FillBuffer:
         if (cmd.buf && cmd.buf->mem) {
            uint8_t *p = cmd.buf->mem->shadow.data() + cmd.buf->mem_offset + cmd.offset;
            VkDeviceSize n = (cmd.size == VK_WHOLE_SIZE) ? (cmd.buf->size - cmd.offset) : cmd.size;
            for (VkDeviceSize i = 0; i < n; i += 4) {
               uint32_t v = cmd.fill_value;
               size_t chunk = (size_t)std::min<VkDeviceSize>(4, n - i);
               memcpy(p + i, &v, chunk);
               if (i + chunk == n) cmd.buf->mem->revision++;
            }
         }
         break;
      case Cmd::ExecuteCommands:
         for (auto *sec : cmd.secondary) if (sec) execute_cmds(sec);
         break;
      case Cmd::CopyBuffer:
         if (cmd.src && cmd.buf && cmd.src->mem && cmd.buf->mem) {
            for (auto &r : cmd.copies) {
               if (r.srcOffset + r.size > cmd.src->size || r.dstOffset + r.size > cmd.buf->size) continue;
               memcpy(cmd.buf->mem->shadow.data() + cmd.buf->mem_offset + r.dstOffset,
                      cmd.src->mem->shadow.data() + cmd.src->mem_offset + r.srcOffset, (size_t)r.size);
               cmd.buf->mem->revision++;
            }
         }
         break;
      case Cmd::CopyBufferToImage:
         VKGL_LOG("CopyBufferToImage buf=%p img=%p extent=%ux%u\n", (void*)cmd.buf, (void*)cmd.img, cmd.region.imageExtent.width, cmd.region.imageExtent.height);
         if (cmd.buf && cmd.buf->mem && cmd.img) {
            const VkBufferImageCopy &r = cmd.region;
            size_t bpp = 4;
            switch (cmd.img->format) {
            case VK_FORMAT_R16G16B16A16_SFLOAT: bpp = 8; break;
            case VK_FORMAT_D16_UNORM: bpp = 2; break;
            case VK_FORMAT_D32_SFLOAT_S8_UINT: bpp = 8; break;
            default: break;
            }
            uint64_t width = r.imageExtent.width, height = r.imageExtent.height;
            uint64_t row_texels = r.bufferRowLength ? r.bufferRowLength : width;
            uint64_t row_bytes = width * bpp;
            bool linear = cmd.img->tiling == VK_IMAGE_TILING_LINEAR && cmd.img->mem;
            bool valid = width && height && r.imageExtent.depth == 1 && r.imageOffset.z == 0 &&
               row_texels >= width &&
               (uint64_t)r.imageOffset.x + width <= cmd.img->extent.width &&
               (uint64_t)r.imageOffset.y + height <= cmd.img->extent.height;
            std::vector<uint8_t> packed;
            if (valid) packed.resize((size_t)(row_bytes * height));
            for (uint64_t y = 0; valid && y < height; y++) {
               uint64_t src_rel = r.bufferOffset + y * row_texels * bpp;
               uint64_t dst_rel = ((uint64_t)r.imageOffset.y + y) * cmd.img->extent.width * bpp +
                  (uint64_t)r.imageOffset.x * bpp;
               const auto &src_shadow = cmd.buf->mem->shadow;
               if (src_rel > cmd.buf->size || row_bytes > cmd.buf->size - src_rel ||
                   cmd.buf->mem_offset > src_shadow.size() || src_rel > src_shadow.size() - cmd.buf->mem_offset ||
                   row_bytes > src_shadow.size() - cmd.buf->mem_offset - src_rel) {
                  valid = false;
                  break;
               }
               memcpy(packed.data() + y * row_bytes, src_shadow.data() + cmd.buf->mem_offset + src_rel, (size_t)row_bytes);
               if (linear) {
                  const auto &dst_shadow = cmd.img->mem->shadow;
                  if (cmd.img->mem_offset > dst_shadow.size() || dst_rel > dst_shadow.size() - cmd.img->mem_offset ||
                      row_bytes > dst_shadow.size() - cmd.img->mem_offset - dst_rel) valid = false;
               }
            }
            if (!valid) {
               VKGL_LOG("CopyBufferToImage rejected unsupported or out-of-bounds region\n");
               break;
            }
            if (linear) {
               uint8_t *dst = cmd.img->mem->shadow.data() + cmd.img->mem_offset;
               uint64_t image_pitch = (uint64_t)cmd.img->extent.width * bpp;
               for (uint64_t y = 0; y < height; y++)
                  memcpy(dst + ((uint64_t)r.imageOffset.y + y) * image_pitch + (uint64_t)r.imageOffset.x * bpp,
                         packed.data() + y * row_bytes, (size_t)row_bytes);
            }
            ensure_gl_texture(cmd.img);
            glBindTexture(GL_TEXTURE_2D, cmd.img->tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexSubImage2D(GL_TEXTURE_2D, 0, (GLint)r.imageOffset.x, (GLint)r.imageOffset.y,
                            (GLsizei)width, (GLsizei)height,
                            cmd.img->gl_format, cmd.img->gl_type, packed.data());
            if (linear) {
               cmd.img->mem->revision++;
               cmd.img->uploaded_revision = cmd.img->mem->revision;
            }
            glBindTexture(GL_TEXTURE_2D, 0);
         }
         break;
      case Cmd::CopyImageToBuffer:
         break;
      case Cmd::UpdateBuffer:
         break;
      case Cmd::ClearColorImage:
         if (cmd.img) {
            ensure_fbo(cmd.img);
            glBindFramebuffer(GL_FRAMEBUFFER, cmd.img->fbo);
            glClearColor(cmd.cc.float32[0], cmd.cc.float32[1], cmd.cc.float32[2], cmd.cc.float32[3]);
            glClear(GL_COLOR_BUFFER_BIT);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
         }
         break;
      default:
         break;
      }
   }
   glFlush();
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_QueueSubmit(VkQueue, uint32_t n, const VkSubmitInfo *submits, VkFence fence) {
   for (uint32_t i = 0; i < n; i++) {
      const auto &s = submits[i];
      for (uint32_t k = 0; k < s.commandBufferCount; k++) {
         auto *c = from_handle<CommandBuffer>((uint64_t)s.pCommandBuffers[k]);
         if (c) execute_cmds(c);
      }
   }
   if (fence) from_handle<Fence>((uint64_t)fence)->signaled = true;
   return VK_SUCCESS;
}

// ---------------------------------------------------------------------------
// Wayland WSI
// ---------------------------------------------------------------------------
struct ShmRelease {
   wl_buffer *buf = nullptr;
   bool released = true;
   bool acquired = false;
};

static void shm_release(void *data, wl_buffer *) {
   auto *r = (ShmRelease *)data;
   r->released = true;
}

static const wl_buffer_listener shm_listener = {shm_release};

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_CreateWaylandSurfaceKHR(VkInstance,
                                                                   const VkWaylandSurfaceCreateInfoKHR *ci,
                                                                   const VkAllocationCallbacks *, VkSurfaceKHR *out) {
   auto *s = new Surface();
   s->type = VK_OBJECT_TYPE_SURFACE_KHR;
   s->display = ci->display;
   s->surface = ci->surface;
   *out = (VkSurfaceKHR)to_handle(s);
   reg(s, *out);
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL vkgl_DestroySurfaceKHR(VkInstance, VkSurfaceKHR s, const VkAllocationCallbacks *) {
   unreg((uint64_t)s);
   delete from_handle<Surface>((uint64_t)s);
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice, uint32_t,
                                                                             VkSurfaceKHR, VkBool32 *supported) {
   *supported = VK_TRUE;
   return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_GetPhysicalDeviceSurfaceCapabilitiesKHR(
   VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR *caps) {
   VKGL_LOG("surface capabilities\n");
   memset(caps, 0, sizeof(*caps));
   caps->minImageCount = 2;
   caps->maxImageCount = 3;
   caps->currentExtent = {0xFFFFFFFFu, 0xFFFFFFFFu};
   caps->minImageExtent = {1, 1};
   caps->maxImageExtent = {4096, 4096};
   caps->maxImageArrayLayers = 1;
   caps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
   caps->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
   return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice, VkSurfaceKHR,
                                                                              uint32_t *n, VkSurfaceFormatKHR *f) {
   VKGL_LOG("surface formats n=%u\n", *n);
   static const VkSurfaceFormatKHR fmts[] = {
      {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
      {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
   };
   if (!f) { *n = 2; return VK_SUCCESS; }
   uint32_t c = std::min<uint32_t>(2, *n);
   memcpy(f, fmts, c * sizeof(VkSurfaceFormatKHR));
   *n = c;
   return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_GetPhysicalDeviceSurfacePresentModesKHR(
   VkPhysicalDevice, VkSurfaceKHR, uint32_t *n, VkPresentModeKHR *m) {
   VKGL_LOG("present modes n=%u\n", *n);
   static const VkPresentModeKHR modes[] = {VK_PRESENT_MODE_FIFO_KHR};
   if (!m) { *n = 1; return VK_SUCCESS; }
   uint32_t c = std::min<uint32_t>(1, *n);
   memcpy(m, modes, c * sizeof(VkPresentModeKHR));
   *n = c;
   return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_CreateSwapchainKHR(VkDevice, const VkSwapchainCreateInfoKHR *ci,
                                                              const VkAllocationCallbacks *, VkSwapchainKHR *out) {
   VKGL_LOG("CreateSwapchainKHR enter extent=%ux%u fmt=%d surface=%p\n", ci->imageExtent.width, ci->imageExtent.height, ci->imageFormat, (void*)ci->surface);
   auto *sc = new Swapchain();
   sc->type = VK_OBJECT_TYPE_SWAPCHAIN_KHR;
   sc->surface = from_handle<Surface>((uint64_t)ci->surface);
   sc->dev = g_device;
   sc->format = ci->imageFormat;
   sc->cs = ci->imageColorSpace;
   sc->extent = ci->imageExtent;
   sc->count = ci->minImageCount;
   if (sc->count < 2) sc->count = 2;
   if (sc->count > 3) sc->count = 3;

   GlCtx::get().make_current();
   VKGL_LOG("swapchain: GL ready\n");
   wl_shm *shm = nullptr;
   // find wl_shm in the registry via the surface's display
   struct Finder {
      wl_shm *shm = nullptr;
      zwp_linux_dmabuf_v1 *dmabuf = nullptr;
   } finder;
   auto global_cb = [](void *data, wl_registry *reg, uint32_t name, const char *iface, uint32_t) {
      auto *f = (Finder *)data;
      if (strcmp(iface, "wl_shm") == 0) f->shm = (wl_shm *)wl_registry_bind(reg, name, &wl_shm_interface, 1);
      else if (strcmp(iface, "zwp_linux_dmabuf_v1") == 0)
         f->dmabuf = (zwp_linux_dmabuf_v1 *)wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface, 3);
   };
   auto global_remove = [](void *, wl_registry *, uint32_t) {};
   const wl_registry_listener reg_listener = {global_cb, global_remove};
   wl_registry *wlreg = wl_display_get_registry(sc->surface->display);
   wl_registry_add_listener(wlreg, &reg_listener, &finder);
   wl_display_roundtrip(sc->surface->display);
   wl_registry_destroy(wlreg);
   shm = finder.shm;
   sc->shm = shm;
   sc->dmabuf = finder.dmabuf;
   sc->queue = wl_display_create_queue(sc->surface->display);
   if (!sc->queue) {
      fprintf(stderr, "vkgl: cannot create presentation event queue\n");
      delete sc;
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   if (!shm) { fprintf(stderr, "vkgl: no wl_shm\n"); delete sc; return VK_ERROR_INITIALIZATION_FAILED; }
   VKGL_LOG("swapchain: wl_shm ok, creating %u images\n", sc->count);

   size_t stride = (size_t)sc->extent.width * 4;
   size_t bytes = stride * sc->extent.height;
   sc->images.resize(sc->count);
   sc->image_handles.resize(sc->count);
   sc->wl_buffers.resize(sc->count, nullptr);
   sc->shm_data.resize(sc->count, nullptr);
   sc->shm_size.resize(sc->count, bytes);
   sc->pools.resize(sc->count, nullptr);
   sc->bos.resize(sc->count, nullptr);
   sc->dbufs.resize(sc->count, nullptr);
   sc->eglimgs.resize(sc->count, EGL_NO_IMAGE_KHR);
   sc->releases.resize(sc->count, nullptr);
   uint32_t dmabuf_images = 0;
   for (uint32_t i = 0; i < sc->count; i++) {
      int fd = memfd_create("vkgl-swapchain", MFD_CLOEXEC);
      if (fd < 0) { perror("vkgl: memfd_create"); delete sc; return VK_ERROR_OUT_OF_HOST_MEMORY; }
      if (ftruncate(fd, (off_t)bytes) != 0) { perror("vkgl: ftruncate"); close(fd); delete sc; return VK_ERROR_OUT_OF_HOST_MEMORY; }
      void *data = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      if (data == MAP_FAILED) { perror("vkgl: mmap"); close(fd); delete sc; return VK_ERROR_OUT_OF_HOST_MEMORY; }
      wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)bytes);
      wl_buffer *wbuf = wl_shm_pool_create_buffer(pool, 0, (int32_t)sc->extent.width,
                                                  (int32_t)sc->extent.height, (int32_t)stride,
                                                  WL_SHM_FORMAT_XRGB8888);
      auto *rel = new ShmRelease(); rel->buf = wbuf; rel->released = true;
      wl_proxy_set_queue((wl_proxy *)wbuf, sc->queue);
      wl_buffer_add_listener(wbuf, &shm_listener, rel);
      close(fd);
      sc->pools[i] = pool;
      sc->wl_buffers[i] = wbuf;
      sc->shm_data[i] = data;
      sc->releases[i] = rel;
      auto *img = new Image();
      img->type = VK_OBJECT_TYPE_IMAGE;
      img->format = sc->format;
      img->extent = {sc->extent.width, sc->extent.height, 1};
      img->usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      /* Render directly into a linear GBM BO and give that same allocation to
       * the compositor.  The wl_shm image above remains a per-image fallback. */
      bool image_dmabuf = false;
      if (!dmabuf_disabled && sc->dmabuf && GlCtx::get().ok) {
         GlCtx &g = GlCtx::get();
         gbm_bo *bo = gbm_bo_create(g.gbm, sc->extent.width, sc->extent.height,
                                    GBM_FORMAT_XRGB8888, GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
         if (bo) {
            int bfd = gbm_bo_get_fd(bo);
            uint32_t bstride = gbm_bo_get_stride(bo);
            EGLAttrib iattr[] = {
               EGL_WIDTH, (EGLAttrib)sc->extent.width,
               EGL_HEIGHT, (EGLAttrib)sc->extent.height,
               EGL_LINUX_DRM_FOURCC_EXT, 0x34325258 /* DRM_FORMAT_XRGB8888 */,
               EGL_DMA_BUF_PLANE0_FD_EXT, (EGLAttrib)bfd,
               EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
               EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLAttrib)bstride,
               EGL_NONE };
            EGLImage eimg = bfd >= 0 ? eglCreateImage(g.dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, iattr)
                                     : EGL_NO_IMAGE_KHR;
            GLuint tex = 0;
            wl_buffer *wb = nullptr;
            if (eimg != EGL_NO_IMAGE_KHR) {
               auto fn = (PFNEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
               if (fn) {
                  glGenTextures(1, &tex);
                  glBindTexture(GL_TEXTURE_2D, tex);
                  fn(GL_TEXTURE_2D, (GLeglImageOES)eimg);
                  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
               }
               if (tex && glGetError() == GL_NO_ERROR) {
                  zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(sc->dmabuf);
                  constexpr uint64_t mod_invalid = 0x00ffffffffffffffULL;
                  zwp_linux_buffer_params_v1_add(params, bfd, 0, 0, bstride,
                                                 (uint32_t)(mod_invalid >> 32), (uint32_t)mod_invalid);
                  wb = zwp_linux_buffer_params_v1_create_immed(params, sc->extent.width, sc->extent.height,
                                                              0x34325258, 0);
                  zwp_linux_buffer_params_v1_destroy(params);
               }
            }
            if (bfd >= 0) close(bfd);
            if (tex && wb) {
               img->tex = tex;
               sc->bos[i] = bo;
               sc->dbufs[i] = wb;
               sc->eglimgs[i] = eimg;
               if (sc->queue) wl_proxy_set_queue((wl_proxy *)wb, sc->queue);
               wl_buffer_add_listener(wb, &shm_listener, rel);
               image_dmabuf = true;
               VKGL_LOG("swapchain[%u]: dmabuf zero-copy bo=%p stride=%u\n", i, (void*)bo, bstride);
            } else {
               if (wb) wl_buffer_destroy(wb);
               if (eimg != EGL_NO_IMAGE_KHR) eglDestroyImage(g.dpy, eimg);
               if (tex) glDeleteTextures(1, &tex);
               gbm_bo_destroy(bo);
            }
         }
      }
      if (!image_dmabuf) ensure_gl_texture(img);
      ensure_fbo(img);
      if (image_dmabuf) {
         GlCtx::get().make_current();
         glBindFramebuffer(GL_FRAMEBUFFER, img->fbo);
         GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
         glBindFramebuffer(GL_FRAMEBUFFER, 0);
         if (status == GL_FRAMEBUFFER_COMPLETE) {
            dmabuf_images++;
         } else {
            fprintf(stderr, "vkgl: dmabuf FBO incomplete (0x%x), using wl_shm for image %u\n", status, i);
            glDeleteFramebuffers(1, &img->fbo);
            glDeleteTextures(1, &img->tex);
            img->fbo = img->tex = 0;
            wl_buffer_destroy(sc->dbufs[i]);
            eglDestroyImage(GlCtx::get().dpy, sc->eglimgs[i]);
            gbm_bo_destroy(sc->bos[i]);
            sc->dbufs[i] = nullptr;
            sc->eglimgs[i] = EGL_NO_IMAGE_KHR;
            sc->bos[i] = nullptr;
            ensure_gl_texture(img);
            ensure_fbo(img);
         }
      }
      sc->images[i] = img;
      sc->image_handles[i] = (VkImage)to_handle(img);
   }
   sc->dmabuf_ok = dmabuf_images == sc->count;
   *out = (VkSwapchainKHR)to_handle(sc);
   reg(sc, *out);
   VKGL_LOG("swapchain %ux%u x%u\n", sc->extent.width, sc->extent.height, sc->count);
   return VK_SUCCESS;
}

Swapchain::~Swapchain() {
   size_t n = images.size();
   if (n) {
      GlCtx &g = GlCtx::get();
      if (g.ok) g.make_current();
      for (size_t i = 0; i < n; i++) {
         if (dbufs[i]) wl_buffer_destroy(dbufs[i]);
         if (wl_buffers[i]) wl_buffer_destroy(wl_buffers[i]);
         if (images[i] && g.ok) {
            if (images[i]->fbo) glDeleteFramebuffers(1, &images[i]->fbo);
            if (images[i]->tex) glDeleteTextures(1, &images[i]->tex);
         }
         if (eglimgs[i] != EGL_NO_IMAGE_KHR && g.ok) eglDestroyImage(g.dpy, eglimgs[i]);
         if (bos[i]) gbm_bo_destroy(bos[i]);
         if (pools[i]) wl_shm_pool_destroy(pools[i]);
         if (shm_data[i]) munmap(shm_data[i], shm_size[i]);
         delete releases[i];
         delete images[i];
      }
   }
   if (shm) wl_shm_destroy(shm);
   if (dmabuf) zwp_linux_dmabuf_v1_destroy(dmabuf);
   if (queue) wl_event_queue_destroy(queue);
}

static VKAPI_ATTR void VKAPI_CALL vkgl_DestroySwapchainKHR(VkDevice, VkSwapchainKHR h, const VkAllocationCallbacks *) {
   unreg((uint64_t)h);
   delete from_handle<Swapchain>((uint64_t)h);
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_GetSwapchainImagesKHR(VkDevice, VkSwapchainKHR h,
                                                                 uint32_t *n, VkImage *imgs) {
   VKGL_LOG("GetSwapchainImages n=%p\n", (void*)n);
   auto *sc = from_handle<Swapchain>((uint64_t)h);
   if (!imgs) { *n = sc->count; return VK_SUCCESS; }
   uint32_t c = std::min(sc->count, *n);
   for (uint32_t i = 0; i < c; i++) imgs[i] = sc->image_handles[i];
   *n = c;
   return c == sc->count ? VK_SUCCESS : VK_INCOMPLETE;
}

static int find_released_image(Swapchain *sc) {
   for (uint32_t n = 0; n < sc->count; n++) {
      uint32_t idx = (sc->current + n) % sc->count;
      ShmRelease *rel = sc->releases[idx];
      if (rel && rel->released && !rel->acquired) return (int)idx;
   }
   return -1;
}

static void dispatch_release_events(Swapchain *sc) {
   if (sc->queue) wl_display_dispatch_queue_pending(sc->surface->display, sc->queue);
}

static bool wait_buffer_release(Swapchain *sc, uint64_t timeout_ns) {
   wl_display *display = sc->surface->display;
   wl_event_queue *queue = sc->queue;
   if (!queue) return find_released_image(sc) >= 0;
   /* Even a nonblocking acquire must consume releases which already arrived. */
   dispatch_release_events(sc);
   if (find_released_image(sc) >= 0) return true;
   if (timeout_ns == 0) return false;
   const bool infinite = timeout_ns == UINT64_MAX;
   int64_t remaining = infinite ? -1 : (int64_t)(timeout_ns / 1000000);
   if (!infinite && remaining < 1) remaining = 1;
   while (infinite || remaining > 0) {
      /* Never dispatch the application's default queue from inside a Vulkan
       * call: its configure callback may legally recreate the swapchain. */
      dispatch_release_events(sc);
      if (find_released_image(sc) >= 0) return true;
      while (wl_display_prepare_read_queue(display, queue) != 0)
         dispatch_release_events(sc);
      if (find_released_image(sc) >= 0) {
         wl_display_cancel_read(display);
         return true;
      }
      wl_display_flush(display);
      pollfd pfd = {wl_display_get_fd(display), POLLIN, 0};
      int slice = infinite ? 4 : (int)std::min<int64_t>(4, remaining);
      int pr = poll(&pfd, 1, slice);
      if (pr > 0 && (pfd.revents & POLLIN)) wl_display_read_events(display);
      else wl_display_cancel_read(display);
      dispatch_release_events(sc);
      if (find_released_image(sc) >= 0) return true;
      if (!infinite) remaining -= slice;
   }
   VKGL_LOG("acquire timed out waiting for compositor release\n");
   return find_released_image(sc) >= 0;
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_AcquireNextImageKHR(VkDevice, VkSwapchainKHR h, uint64_t timeout,
                                                               VkSemaphore, VkFence fence, uint32_t *index) {
   auto *sc = from_handle<Swapchain>((uint64_t)h);
   int idx = find_released_image(sc);
   bool waited = idx < 0;
   if (waited) {
      if (!wait_buffer_release(sc, timeout)) return VK_TIMEOUT;
      idx = find_released_image(sc);
   }
   if (idx < 0) return VK_TIMEOUT;
   sc->releases[idx]->acquired = true;
   if (fence) from_handle<Fence>((uint64_t)fence)->signaled = true;
   *index = (uint32_t)idx;
   sc->current = ((uint32_t)idx + 1) % sc->count;
   VKGL_LOG("acquire sc=%p idx=%d dmabuf=%d after_wait=%d\n", (void*)sc, idx,
            sc->dbufs[idx] != nullptr, waited);
   return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_QueuePresentKHR(VkQueue, const VkPresentInfoKHR *pi) {
   static int present_count = 0;
   if ((++present_count % 30) == 1) VKGL_LOG("present #%d\n", present_count);
   for (uint32_t i = 0; i < pi->swapchainCount; i++) {
      auto *sc = from_handle<Swapchain>((uint64_t)pi->pSwapchains[i]);
      uint32_t idx = pi->pImageIndices[i];
      if (idx >= sc->count) continue;
      VKGL_LOG("present sc=%p idx=%u dmabuf=%d\n", (void*)sc, idx, sc->dbufs[idx] != nullptr);
      Image *img = sc->images[idx];
      GlCtx::get().make_current();
      wl_buffer *present_buf = sc->dbufs[idx] ? sc->dbufs[idx] : sc->wl_buffers[idx];
      if (sc->dbufs[idx]) {
         /* glReadPixels used to provide an implicit full GPU sync.  Keep that
          * guarantee without copying pixels through the CPU. */
         glBindFramebuffer(GL_FRAMEBUFFER, img->fbo);
         glFinish();
         glBindFramebuffer(GL_FRAMEBUFFER, 0);
      } else {
         glBindFramebuffer(GL_FRAMEBUFFER, img->fbo);
         glPixelStorei(GL_PACK_ALIGNMENT, 4);
         /* wl_shm XRGB8888 is little-endian BGRA; read BGRA when the swapchain
          * is B8G8R8A8 so colours are not swapped on screen. */
         if (sc->format == VK_FORMAT_B8G8R8A8_UNORM)
            glReadPixels(0, 0, (GLsizei)sc->extent.width, (GLsizei)sc->extent.height, GL_BGRA_EXT, GL_UNSIGNED_BYTE, sc->shm_data[idx]);
         else
            glReadPixels(0, 0, (GLsizei)sc->extent.width, (GLsizei)sc->extent.height, GL_RGBA, GL_UNSIGNED_BYTE, sc->shm_data[idx]);
         glBindFramebuffer(GL_FRAMEBUFFER, 0);
         static int plog = 0;
         if (plog++ < 6) {
            unsigned char *p8 = (unsigned char *)sc->shm_data[idx];
            size_t off = ((size_t)sc->extent.height / 2) * (size_t)sc->extent.width * 4 + (size_t)sc->extent.width * 2;
            VKGL_LOG("present idx=%u px(center)=%02x %02x %02x %02x fbo=%u tex=%u\n", idx, p8[off], p8[off+1], p8[off+2], p8[off+3], img->fbo, img->tex);
         }
      }
      if (sc->releases[idx]) {
         sc->releases[idx]->released = false;
         sc->releases[idx]->acquired = false;
      }
      wl_surface_attach(sc->surface->surface, present_buf, 0, 0);
      wl_surface_damage(sc->surface->surface, 0, 0, (int32_t)sc->extent.width, (int32_t)sc->extent.height);
      wl_surface_commit(sc->surface->surface);
      wl_display_flush(sc->surface->display);
      if (pi->pResults) pi->pResults[i] = VK_SUCCESS;
   }
   return VK_SUCCESS;
}

// pipeline / draw (stage 2) is implemented below the dispatch table

// ---------------------------------------------------------------------------
// dispatch table
// ---------------------------------------------------------------------------
// forward declarations for optional entry points appended below
static VKAPI_ATTR VkBool32 VKAPI_CALL vkgl_GetPhysicalDeviceWaylandPresentationSupportKHR(VkPhysicalDevice, uint32_t, struct wl_display *);
static VKAPI_ATTR VkBool32 VKAPI_CALL vkgl_GetPhysicalDeviceXlibPresentationSupportKHR(VkPhysicalDevice, uint32_t, void *, unsigned long);
static VKAPI_ATTR VkBool32 VKAPI_CALL vkgl_GetPhysicalDeviceXcbPresentationSupportKHR(VkPhysicalDevice, uint32_t, void *, uint32_t);
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_GetPhysicalDevicePresentRectanglesKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t *, VkRect2D *);
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_EnumeratePhysicalDeviceGroups(VkInstance, uint32_t *, void *);
static VKAPI_ATTR void VKAPI_CALL vkgl_GetPhysicalDeviceFeatures2(VkPhysicalDevice, VkPhysicalDeviceFeatures2 *);
static VKAPI_ATTR void VKAPI_CALL vkgl_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice, VkFormat, VkFormatProperties2 *);
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice, const VkPhysicalDeviceImageFormatInfo2 *, VkImageFormatProperties2 *);
static VKAPI_ATTR void VKAPI_CALL vkgl_GetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice, uint32_t *, VkQueueFamilyProperties2 *);
static VKAPI_ATTR void VKAPI_CALL vkgl_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties2 *);
static VKAPI_ATTR void VKAPI_CALL vkgl_GetPhysicalDeviceSparseImageFormatProperties2(VkPhysicalDevice, const VkPhysicalDeviceSparseImageFormatInfo2 *, uint32_t *, VkSparseImageFormatProperties2 *);
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_GetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR *, VkSurfaceCapabilities2KHR *);
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_GetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR *, uint32_t *, VkSurfaceFormat2KHR *);
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_GetDeviceGroupSurfacePresentModesKHR(VkDevice, VkSurfaceKHR, VkDeviceGroupPresentModeFlagsKHR *);
static VKAPI_ATTR void VKAPI_CALL vkgl_GetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice, const VkPhysicalDeviceExternalBufferInfo *, VkExternalBufferProperties *);
static VKAPI_ATTR void VKAPI_CALL vkgl_GetPhysicalDeviceExternalFenceProperties(VkPhysicalDevice, const VkPhysicalDeviceExternalFenceInfo *, VkExternalFenceProperties *);
static VKAPI_ATTR void VKAPI_CALL vkgl_GetPhysicalDeviceExternalSemaphoreProperties(VkPhysicalDevice, const VkPhysicalDeviceExternalSemaphoreInfo *, VkExternalSemaphoreProperties *);

static VKAPI_ATTR void VKAPI_CALL vkgl_GetImageSubresourceLayout(VkDevice, VkImage, const VkImageSubresource *, VkSubresourceLayout *);

struct Entry { const char *name; void *fn; };

/* ---- stubs for not-yet-implemented core entry points (keep the loader
 * from handing out NULL pointers; they are no-ops returning success) ---- */
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_BindBufferMemory2(void *a1, ...) { (void)a1; { return VK_SUCCESS; } }
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_BindImageMemory2(void *a1, ...) { (void)a1; { return VK_SUCCESS; } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdBeginQuery(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdBeginRenderPass2(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdBeginRendering(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdBindDescriptorSets2(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdBindIndexBuffer2(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdBindVertexBuffers2(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdBlitImage(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdBlitImage2(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdClearAttachments(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdClearDepthStencilImage(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdCopyBuffer2(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdCopyBufferToImage2(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdCopyImage(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdCopyImage2(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdCopyImageToBuffer2(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdCopyQueryPoolResults(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdDispatch(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdDispatchBase(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdDispatchIndirect(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdDrawIndexedIndirectCount(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdDrawIndirectCount(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdEndQuery(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdEndRenderPass2(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdEndRendering(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdNextSubpass(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdNextSubpass2(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdPipelineBarrier2(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdPushConstants(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdPushConstants2(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdPushDescriptorSet(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdPushDescriptorSet2(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdPushDescriptorSetWithTemplate(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdPushDescriptorSetWithTemplate2(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdResetEvent(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdResetEvent2(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdResetQueryPool(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdResolveImage(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdResolveImage2(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdSetBlendConstants(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdSetCullMode(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdSetDepthBias(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdSetDepthBiasEnable(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdSetDepthBounds(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdSetDepthBoundsTestEnable(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdSetDepthCompareOp(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdSetDepthTestEnable(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdSetDepthWriteEnable(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdSetDeviceMask(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdSetEvent(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdSetEvent2(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdSetFrontFace(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdSetLineStipple(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdSetLineWidth(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdSetPrimitiveRestartEnable(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdSetPrimitiveTopology(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdSetRasterizerDiscardEnable(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdSetRenderingAttachmentLocations(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdSetRenderingInputAttachmentIndices(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdSetScissorWithCount(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdSetStencilCompareMask(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdSetStencilOp(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdSetStencilReference(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdSetStencilTestEnable(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdSetStencilWriteMask(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdSetViewportWithCount(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdWaitEvents(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdWaitEvents2(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdWriteTimestamp(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_CmdWriteTimestamp2(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_CopyImageToImage(void *a1, ...) { (void)a1; { return VK_SUCCESS; } }
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_CopyImageToMemory(void *a1, ...) { (void)a1; { return VK_SUCCESS; } }
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_CopyMemoryToImage(void *a1, ...) { (void)a1; { return VK_SUCCESS; } }
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_CreateBufferView(void *a1, ...) { (void)a1; { return VK_SUCCESS; } }
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_CreateComputePipelines(void *a1, ...) { (void)a1; { return VK_SUCCESS; } }
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_CreateDescriptorUpdateTemplate(void *a1, ...) { (void)a1; { return VK_SUCCESS; } }
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_CreatePrivateDataSlot(void *a1, ...) { (void)a1; { return VK_SUCCESS; } }
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_CreateRenderPass2(void *a1, ...) { (void)a1; { return VK_SUCCESS; } }
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_CreateSamplerYcbcrConversion(void *a1, ...) { (void)a1; { return VK_SUCCESS; } }
static VKAPI_ATTR void VKAPI_CALL vkgl_DestroyBufferView(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_DestroyDescriptorUpdateTemplate(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_DestroyPrivateDataSlot(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_DestroySamplerYcbcrConversion(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR VkDeviceAddress VKAPI_CALL vkgl_GetBufferDeviceAddress(void *a1, ...) { (void)a1; { return {}; } }
static VKAPI_ATTR void VKAPI_CALL vkgl_GetBufferMemoryRequirements2(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR uint64_t VKAPI_CALL vkgl_GetBufferOpaqueCaptureAddress(void *a1, ...) { (void)a1; { return 0; } }
static VKAPI_ATTR void VKAPI_CALL vkgl_GetDescriptorSetLayoutSupport(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_GetDeviceBufferMemoryRequirements(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_GetDeviceGroupPeerMemoryFeatures(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_GetDeviceImageMemoryRequirements(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_GetDeviceImageSparseMemoryRequirements(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_GetDeviceImageSubresourceLayout(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_GetDeviceMemoryCommitment(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR uint64_t VKAPI_CALL vkgl_GetDeviceMemoryOpaqueCaptureAddress(void *a1, ...) { (void)a1; { return 0; } }
static VKAPI_ATTR void VKAPI_CALL vkgl_GetDeviceQueue2(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_GetImageMemoryRequirements2(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_GetImageSparseMemoryRequirements(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_GetImageSparseMemoryRequirements2(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_GetImageSubresourceLayout2(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_GetPhysicalDeviceToolProperties(void *a1, ...) { (void)a1; { return VK_SUCCESS; } }
static VKAPI_ATTR void VKAPI_CALL vkgl_GetPrivateData(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_GetRenderAreaGranularity(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR void VKAPI_CALL vkgl_GetRenderingAreaGranularity(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_GetSemaphoreCounterValue(void *a1, ...) { (void)a1; { return VK_SUCCESS; } }
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_MapMemory2(void *a1, ...) { (void)a1; { return VK_SUCCESS; } }
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_QueueBindSparse(void *a1, ...) { (void)a1; { return VK_SUCCESS; } }
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_QueueSubmit2(void *a1, ...) { (void)a1; { return VK_SUCCESS; } }
static VKAPI_ATTR void VKAPI_CALL vkgl_ResetQueryPool(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_SetPrivateData(void *a1, ...) { (void)a1; { return VK_SUCCESS; } }
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_SignalSemaphore(void *a1, ...) { (void)a1; { return VK_SUCCESS; } }
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_TransitionImageLayout(void *a1, ...) { (void)a1; { return VK_SUCCESS; } }
static VKAPI_ATTR void VKAPI_CALL vkgl_TrimCommandPool(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_UnmapMemory2(void *a1, ...) { (void)a1; { return VK_SUCCESS; } }
static VKAPI_ATTR void VKAPI_CALL vkgl_UpdateDescriptorSetWithTemplate(void *a1, ...) { (void)a1; { } }
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_WaitSemaphores(void *a1, ...) { (void)a1; { return VK_SUCCESS; } }


static VKAPI_ATTR void VKAPI_CALL vkgl_CmdFillBuffer(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off,
                                                     VkDeviceSize size, uint32_t data) {
   auto *c = from_handle<CommandBuffer>((uint64_t)cb);
   Cmd cmd{}; cmd.kind = Cmd::FillBuffer; cmd.buf = from_handle<Buffer>((uint64_t)buf);
   cmd.offset = off; cmd.size = size; cmd.fill_value = data;
   c->cmds.push_back(cmd);
}

static VKAPI_ATTR void VKAPI_CALL vkgl_CmdExecuteCommands(VkCommandBuffer cb, uint32_t count,
                                                           const VkCommandBuffer *bufs) {
   auto *c = from_handle<CommandBuffer>((uint64_t)cb);
   Cmd cmd{}; cmd.kind = Cmd::ExecuteCommands;
   for (uint32_t i = 0; i < count; i++) cmd.secondary.push_back(from_handle<CommandBuffer>((uint64_t)bufs[i]));
   c->cmds.push_back(cmd);
}

#define FN(name) {"vk" #name, (void *)vkgl_##name}

static const Entry g_entry_table[] = {
   FN(CreateInstance), FN(DestroyInstance), FN(EnumerateInstanceVersion),
   FN(BindBufferMemory2), FN(BindImageMemory2), FN(CmdBeginQuery), FN(CmdBeginRenderPass2), FN(CmdBeginRendering), FN(CmdBindDescriptorSets2), FN(CmdBindIndexBuffer2), FN(CmdBindVertexBuffers2), FN(CmdBlitImage), FN(CmdBlitImage2), FN(CmdClearAttachments), FN(CmdClearDepthStencilImage), FN(CmdCopyBuffer2), FN(CmdCopyBufferToImage2), FN(CmdCopyImage), FN(CmdCopyImage2), FN(CmdCopyImageToBuffer2), FN(CmdCopyQueryPoolResults), FN(CmdDispatch), FN(CmdDispatchBase), FN(CmdDispatchIndirect), FN(CmdDrawIndexedIndirectCount), FN(CmdDrawIndirectCount), FN(CmdEndQuery), FN(CmdEndRenderPass2), FN(CmdEndRendering), FN(CmdExecuteCommands), FN(CmdFillBuffer), FN(CmdNextSubpass), FN(CmdNextSubpass2), FN(CmdPipelineBarrier2), FN(CmdPushConstants), FN(CmdPushConstants2), FN(CmdPushDescriptorSet), FN(CmdPushDescriptorSet2), FN(CmdPushDescriptorSetWithTemplate), FN(CmdPushDescriptorSetWithTemplate2), FN(CmdResetEvent), FN(CmdResetEvent2), FN(CmdResetQueryPool), FN(CmdResolveImage), FN(CmdResolveImage2), FN(CmdSetBlendConstants), FN(CmdSetCullMode), FN(CmdSetDepthBias), FN(CmdSetDepthBiasEnable), FN(CmdSetDepthBounds), FN(CmdSetDepthBoundsTestEnable), FN(CmdSetDepthCompareOp), FN(CmdSetDepthTestEnable), FN(CmdSetDepthWriteEnable), FN(CmdSetDeviceMask), FN(CmdSetEvent), FN(CmdSetEvent2), FN(CmdSetFrontFace), FN(CmdSetLineStipple), FN(CmdSetLineWidth), FN(CmdSetPrimitiveRestartEnable), FN(CmdSetPrimitiveTopology), FN(CmdSetRasterizerDiscardEnable), FN(CmdSetRenderingAttachmentLocations), FN(CmdSetRenderingInputAttachmentIndices), FN(CmdSetScissorWithCount), FN(CmdSetStencilCompareMask), FN(CmdSetStencilOp), FN(CmdSetStencilReference), FN(CmdSetStencilTestEnable), FN(CmdSetStencilWriteMask), FN(CmdSetViewportWithCount), FN(CmdWaitEvents), FN(CmdWaitEvents2), FN(CmdWriteTimestamp), FN(CmdWriteTimestamp2), FN(CopyImageToImage), FN(CopyImageToMemory), FN(CopyMemoryToImage), FN(CreateBufferView), FN(CreateComputePipelines), FN(CreateDescriptorUpdateTemplate), FN(CreatePrivateDataSlot), FN(CreateRenderPass2), FN(CreateSamplerYcbcrConversion), FN(DestroyBufferView), FN(DestroyDescriptorUpdateTemplate), FN(DestroyPrivateDataSlot), FN(DestroySamplerYcbcrConversion), FN(GetBufferDeviceAddress), FN(GetBufferMemoryRequirements2), FN(GetBufferOpaqueCaptureAddress), FN(GetDescriptorSetLayoutSupport), FN(GetDeviceBufferMemoryRequirements), FN(GetDeviceGroupPeerMemoryFeatures), FN(GetDeviceImageMemoryRequirements), FN(GetDeviceImageSparseMemoryRequirements), FN(GetDeviceImageSubresourceLayout), FN(GetDeviceMemoryCommitment), FN(GetDeviceMemoryOpaqueCaptureAddress), FN(GetDeviceQueue2), FN(GetImageMemoryRequirements2), FN(GetImageSparseMemoryRequirements), FN(GetImageSparseMemoryRequirements2), FN(GetImageSubresourceLayout2), FN(GetPhysicalDeviceToolProperties), FN(GetPrivateData), FN(GetRenderAreaGranularity), FN(GetRenderingAreaGranularity), FN(GetSemaphoreCounterValue), FN(MapMemory2), FN(QueueBindSparse), FN(QueueSubmit2), FN(ResetQueryPool), FN(SetPrivateData), FN(SignalSemaphore), FN(TransitionImageLayout), FN(TrimCommandPool), FN(UnmapMemory2), FN(UpdateDescriptorSetWithTemplate), FN(WaitSemaphores),
   FN(EnumerateInstanceExtensionProperties), FN(EnumerateInstanceLayerProperties),
   FN(EnumeratePhysicalDevices), FN(GetPhysicalDeviceFeatures), FN(GetPhysicalDeviceProperties),
   FN(GetPhysicalDeviceProperties2), FN(GetPhysicalDeviceQueueFamilyProperties),
   FN(GetPhysicalDeviceMemoryProperties), FN(GetPhysicalDeviceFormatProperties),
   FN(GetPhysicalDeviceImageFormatProperties), FN(GetPhysicalDeviceSparseImageFormatProperties),
   FN(EnumerateDeviceExtensionProperties), FN(EnumerateDeviceLayerProperties),
   FN(CreateDevice), FN(DestroyDevice), FN(GetDeviceQueue), FN(DeviceWaitIdle), FN(QueueWaitIdle),
   FN(AllocateMemory), FN(FreeMemory), FN(MapMemory), FN(UnmapMemory),
   FN(FlushMappedMemoryRanges), FN(InvalidateMappedMemoryRanges),
   FN(GetBufferMemoryRequirements), FN(GetImageMemoryRequirements),
   FN(CreateBuffer), FN(DestroyBuffer), FN(BindBufferMemory),
   FN(CreateImage), FN(DestroyImage), FN(BindImageMemory),
   FN(CreateImageView), FN(DestroyImageView), FN(CreateSampler), FN(DestroySampler),
   FN(GetImageSubresourceLayout),
   FN(CreateRenderPass), FN(DestroyRenderPass), FN(CreateFramebuffer), FN(DestroyFramebuffer),
   FN(CreateShaderModule), FN(DestroyShaderModule),
   FN(CreateDescriptorSetLayout), FN(DestroyDescriptorSetLayout),
   FN(CreatePipelineLayout), FN(DestroyPipelineLayout),
   FN(CreateDescriptorPool), FN(DestroyDescriptorPool), FN(ResetDescriptorPool),
   FN(AllocateDescriptorSets), FN(FreeDescriptorSets), FN(UpdateDescriptorSets),
   FN(CreateCommandPool), FN(DestroyCommandPool), FN(ResetCommandPool),
   FN(AllocateCommandBuffers), FN(FreeCommandBuffers),
   FN(BeginCommandBuffer), FN(EndCommandBuffer), FN(ResetCommandBuffer),
   FN(CmdBeginRenderPass), FN(CmdEndRenderPass), FN(CmdBindPipeline), FN(CmdBindDescriptorSets),
   FN(CmdBindVertexBuffers), FN(CmdBindIndexBuffer), FN(CmdSetViewport), FN(CmdSetScissor),
   FN(CmdDraw), FN(CmdDrawIndexed), FN(CmdPipelineBarrier), FN(CmdClearColorImage), FN(CmdCopyBuffer),
   FN(CmdCopyBufferToImage), FN(CmdCopyImageToBuffer), FN(CmdUpdateBuffer),
   FN(CmdDrawIndirect), FN(CmdDrawIndexedIndirect),
   FN(CreateFence), FN(DestroyFence), FN(GetFenceStatus), FN(ResetFences), FN(WaitForFences),
   FN(CreateSemaphore), FN(DestroySemaphore),
   FN(CreateEvent), FN(DestroyEvent), FN(GetEventStatus), FN(SetEvent), FN(ResetEvent),
   FN(CreateQueryPool), FN(DestroyQueryPool), FN(GetQueryPoolResults),
   FN(QueueSubmit),
   FN(CreateWaylandSurfaceKHR), FN(DestroySurfaceKHR),
   FN(GetPhysicalDeviceWaylandPresentationSupportKHR), FN(GetPhysicalDeviceXlibPresentationSupportKHR),
   FN(GetPhysicalDeviceXcbPresentationSupportKHR),    FN(GetPhysicalDevicePresentRectanglesKHR), FN(EnumeratePhysicalDeviceGroups),
   FN(GetPhysicalDeviceSurfaceCapabilities2KHR), FN(GetPhysicalDeviceSurfaceFormats2KHR),
   FN(GetDeviceGroupSurfacePresentModesKHR),
   FN(GetPhysicalDeviceFeatures2), FN(GetPhysicalDeviceFormatProperties2),
   FN(GetPhysicalDeviceImageFormatProperties2), FN(GetPhysicalDeviceQueueFamilyProperties2),
   FN(GetPhysicalDeviceMemoryProperties2), FN(GetPhysicalDeviceSparseImageFormatProperties2),
   FN(GetPhysicalDeviceExternalBufferProperties), FN(GetPhysicalDeviceExternalFenceProperties),
   FN(GetPhysicalDeviceExternalSemaphoreProperties),
   {"vkGetPhysicalDeviceFeatures2KHR", (void *)vkgl_GetPhysicalDeviceFeatures2},
   {"vkGetPhysicalDeviceProperties2KHR", (void *)vkgl_GetPhysicalDeviceProperties2},
   {"vkGetPhysicalDeviceFormatProperties2KHR", (void *)vkgl_GetPhysicalDeviceFormatProperties2},
   {"vkGetPhysicalDeviceImageFormatProperties2KHR", (void *)vkgl_GetPhysicalDeviceImageFormatProperties2},
   {"vkGetPhysicalDeviceQueueFamilyProperties2KHR", (void *)vkgl_GetPhysicalDeviceQueueFamilyProperties2},
   {"vkGetPhysicalDeviceMemoryProperties2KHR", (void *)vkgl_GetPhysicalDeviceMemoryProperties2},
   {"vkGetPhysicalDeviceSparseImageFormatProperties2KHR", (void *)vkgl_GetPhysicalDeviceSparseImageFormatProperties2},
   {"vkCreateWaylandSurfaceKHR", (void *)vkgl_CreateWaylandSurfaceKHR},
   {"vkCreateXlibSurfaceKHR", (void *)vkgl_CreateWaylandSurfaceKHR},
   {"vkCreateXcbSurfaceKHR", (void *)vkgl_CreateWaylandSurfaceKHR},
   FN(GetPhysicalDeviceSurfaceSupportKHR), FN(GetPhysicalDeviceSurfaceCapabilitiesKHR),
   FN(GetPhysicalDeviceSurfaceFormatsKHR), FN(GetPhysicalDeviceSurfacePresentModesKHR),
   FN(CreateSwapchainKHR), FN(DestroySwapchainKHR), FN(GetSwapchainImagesKHR),
   FN(AcquireNextImageKHR), FN(QueuePresentKHR),
};

// forward declarations for stage 2 entry points referenced by the dispatch code
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_CreateGraphicsPipelines(
   VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo *,
   const VkAllocationCallbacks *, VkPipeline *);
static VKAPI_ATTR void VKAPI_CALL vkgl_DestroyPipeline(VkDevice, VkPipeline, const VkAllocationCallbacks *);
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_CreatePipelineCache(VkDevice, const VkPipelineCacheCreateInfo *,
                                                               const VkAllocationCallbacks *, VkPipelineCache *);
static VKAPI_ATTR void VKAPI_CALL vkgl_DestroyPipelineCache(VkDevice, VkPipelineCache, const VkAllocationCallbacks *);
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_GetPipelineCacheData(VkDevice, VkPipelineCache, size_t *, void *);
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_MergePipelineCaches(VkDevice, VkPipelineCache, uint32_t, const VkPipelineCache *);


// forward declarations for optional entry points appended below
static VKAPI_ATTR VkBool32 VKAPI_CALL vkgl_GetPhysicalDeviceWaylandPresentationSupportKHR(VkPhysicalDevice, uint32_t, struct wl_display *);
static VKAPI_ATTR VkBool32 VKAPI_CALL vkgl_GetPhysicalDeviceXlibPresentationSupportKHR(VkPhysicalDevice, uint32_t, void *, unsigned long);
static VKAPI_ATTR VkBool32 VKAPI_CALL vkgl_GetPhysicalDeviceXcbPresentationSupportKHR(VkPhysicalDevice, uint32_t, void *, uint32_t);
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_GetPhysicalDevicePresentRectanglesKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t *, VkRect2D *);
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_EnumeratePhysicalDeviceGroups(VkInstance, uint32_t *, void *);
static VKAPI_ATTR void VKAPI_CALL vkgl_GetPhysicalDeviceFeatures2(VkPhysicalDevice, VkPhysicalDeviceFeatures2 *);
static VKAPI_ATTR void VKAPI_CALL vkgl_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice, VkFormat, VkFormatProperties2 *);
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice, const VkPhysicalDeviceImageFormatInfo2 *, VkImageFormatProperties2 *);
static VKAPI_ATTR void VKAPI_CALL vkgl_GetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice, uint32_t *, VkQueueFamilyProperties2 *);
static VKAPI_ATTR void VKAPI_CALL vkgl_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties2 *);
static VKAPI_ATTR void VKAPI_CALL vkgl_GetPhysicalDeviceSparseImageFormatProperties2(VkPhysicalDevice, const VkPhysicalDeviceSparseImageFormatInfo2 *, uint32_t *, VkSparseImageFormatProperties2 *);
static VKAPI_ATTR void VKAPI_CALL vkgl_GetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice, const VkPhysicalDeviceExternalBufferInfo *, VkExternalBufferProperties *);
static VKAPI_ATTR void VKAPI_CALL vkgl_GetPhysicalDeviceExternalFenceProperties(VkPhysicalDevice, const VkPhysicalDeviceExternalFenceInfo *, VkExternalFenceProperties *);
static VKAPI_ATTR void VKAPI_CALL vkgl_GetPhysicalDeviceExternalSemaphoreProperties(VkPhysicalDevice, const VkPhysicalDeviceExternalSemaphoreInfo *, VkExternalSemaphoreProperties *);

static const char *stub_names[1024] = {nullptr};

template <int N> static VkResult stubN() {
   if (getenv("VKGL_TRACE")) fprintf(stderr, "vkgl: STUB#%d called for %s\n", N, stub_names[N] ? stub_names[N] : "?");
   return VK_SUCCESS;
}
template <size_t... I>
static std::array<PFN_vkVoidFunction, sizeof...(I)> make_stubs(std::index_sequence<I...>) {
   return { (PFN_vkVoidFunction)stubN<(int)I>... };
}
static std::array<PFN_vkVoidFunction, 1024> &stub_table() {
   static auto t = make_stubs(std::make_index_sequence<1024>{});
   return t;
}

extern "C" {

VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *version) {
   if (*version > 5) *version = 5;
   return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance, const char *name) {
   if (!name) return nullptr;
   if (getenv("VKGL_TRACE")) fprintf(stderr, "vkgl gpdpa: %s\n", name);
   static const char *const pd_names[] = {
      "vkGetPhysicalDeviceFeatures", "vkGetPhysicalDeviceProperties", "vkGetPhysicalDeviceQueueFamilyProperties",
      "vkGetPhysicalDeviceMemoryProperties", "vkGetPhysicalDeviceFormatProperties",
      "vkGetPhysicalDeviceImageFormatProperties", "vkGetPhysicalDeviceSparseImageFormatProperties",
      "vkEnumerateDeviceExtensionProperties", "vkEnumerateDeviceLayerProperties",
      "vkGetPhysicalDeviceSurfaceSupportKHR", "vkGetPhysicalDeviceSurfaceCapabilitiesKHR",
      "vkGetPhysicalDeviceSurfaceFormatsKHR", "vkGetPhysicalDeviceSurfacePresentModesKHR",
      "vkGetPhysicalDeviceWaylandPresentationSupportKHR", "vkGetPhysicalDeviceXlibPresentationSupportKHR",
      "vkGetPhysicalDeviceXcbPresentationSupportKHR", "vkGetPhysicalDevicePresentRectanglesKHR",
      "vkGetPhysicalDeviceFeatures2", "vkGetPhysicalDeviceFormatProperties2",
      "vkGetPhysicalDeviceImageFormatProperties2", "vkGetPhysicalDeviceQueueFamilyProperties2",
      "vkGetPhysicalDeviceMemoryProperties2", "vkGetPhysicalDeviceSparseImageFormatProperties2",
      "vkGetPhysicalDeviceExternalBufferProperties", "vkGetPhysicalDeviceExternalFenceProperties",
      "vkGetPhysicalDeviceExternalSemaphoreProperties", "vkGetPhysicalDeviceProperties2",
   };
   for (const char *n : pd_names)
      if (strcmp(n, name) == 0)
         for (const auto &e : g_entry_table)
            if (strcmp(e.name, name) == 0) return (PFN_vkVoidFunction)e.fn;
   return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance, const char *name) {
   if (!name) return nullptr;
   const bool trace_all = getenv("VKGL_TRACE") != nullptr;
   if (trace_all) fprintf(stderr, "vkgl gipa: %-55s -> ", name);
   for (const auto &e : g_entry_table)
      if (strcmp(e.name, name) == 0) {
         if (trace_all) fprintf(stderr, "%p\n", (void *)e.fn);
         return (PFN_vkVoidFunction)e.fn;
      }
   if (strcmp(name, "vkGetInstanceProcAddr") == 0) return (PFN_vkVoidFunction)vk_icdGetInstanceProcAddr;
   if (strcmp(name, "vkGetDeviceProcAddr") == 0) return (PFN_vkVoidFunction)vk_icdGetInstanceProcAddr;
   if (strcmp(name, "vkCreateGraphicsPipelines") == 0) return (PFN_vkVoidFunction)vkgl_CreateGraphicsPipelines;
   if (strcmp(name, "vkDestroyPipeline") == 0) return (PFN_vkVoidFunction)vkgl_DestroyPipeline;
   if (strcmp(name, "vkCreatePipelineCache") == 0) return (PFN_vkVoidFunction)vkgl_CreatePipelineCache;
   if (strcmp(name, "vkDestroyPipelineCache") == 0) return (PFN_vkVoidFunction)vkgl_DestroyPipelineCache;
   if (strcmp(name, "vkGetPipelineCacheData") == 0) return (PFN_vkVoidFunction)vkgl_GetPipelineCacheData;
   if (strcmp(name, "vkMergePipelineCaches") == 0) return (PFN_vkVoidFunction)vkgl_MergePipelineCaches;
   if (trace_all) fprintf(stderr, "NULL\n");
   if (getenv("VKGL_DEBUG")) fprintf(stderr, "vkgl: missing entry point %s\n", name);
   if (getenv("VKGL_STUB_ALL")) {
      static int idx = 0;
      int slot = idx++ % 1024;
      stub_names[slot] = name;
      return (PFN_vkVoidFunction)stub_table()[slot];
   }
   return nullptr;
}

} // extern "C"

// ---------------------------------------------------------------------------
// pipeline cache (stubs)
// ---------------------------------------------------------------------------
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_CreatePipelineCache(VkDevice, const VkPipelineCacheCreateInfo *,
                                                               const VkAllocationCallbacks *, VkPipelineCache *out) {
   VKGL_LOG("CreatePipelineCache\n");
   auto *o = new Obj(); o->type = VK_OBJECT_TYPE_PIPELINE_CACHE;
   *out = (VkPipelineCache)to_handle(o); reg(o, (uint64_t)*out);
   return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL vkgl_DestroyPipelineCache(VkDevice, VkPipelineCache c, const VkAllocationCallbacks *) {
   if (!c) return;
   unreg((uint64_t)c); delete from_handle<Obj>((uint64_t)c);
}
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_GetPipelineCacheData(VkDevice, VkPipelineCache, size_t *n, void *) {
   *n = 0; return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_MergePipelineCaches(VkDevice, VkPipelineCache, uint32_t, const VkPipelineCache *) {
   return VK_SUCCESS;
}

// ===========================================================================
// stage 2: SPIR-V -> GLSL ES 1.00 translation and draw execution
// ===========================================================================
#include "spirv_glsl.hpp"
#include "spirv_cross_util.hpp"

static GLenum gl_type_for_format(VkFormat f, GLint *components, GLboolean *normalized) {
   *normalized = GL_FALSE;
   switch (f) {
   case VK_FORMAT_R32G32B32A32_SFLOAT: *components = 4; return GL_FLOAT;
   case VK_FORMAT_R32G32B32_SFLOAT:    *components = 3; return GL_FLOAT;
   case VK_FORMAT_R32G32_SFLOAT:       *components = 2; return GL_FLOAT;
   case VK_FORMAT_R32_SFLOAT:          *components = 1; return GL_FLOAT;
   case VK_FORMAT_R16G16B16A16_SFLOAT: *components = 4; return GL_HALF_FLOAT_OES;
   case VK_FORMAT_R16G16_SFLOAT:       *components = 2; return GL_HALF_FLOAT_OES;
   case VK_FORMAT_R8G8B8A8_UNORM:      *components = 4; *normalized = GL_TRUE; return GL_UNSIGNED_BYTE;
   case VK_FORMAT_R8G8_UNORM:          *components = 2; *normalized = GL_TRUE; return GL_UNSIGNED_BYTE;
   case VK_FORMAT_R8G8B8A8_SNORM:      *components = 4; *normalized = GL_TRUE; return GL_BYTE;
   case VK_FORMAT_R16G16_UNORM:        *components = 2; *normalized = GL_TRUE; return GL_UNSIGNED_SHORT;
   default:                            *components = 4; return GL_FLOAT;
   }
}

// translate one stage; fills reflection arrays
static bool build_stage(GLuint *out_shader, GLenum gl_stage, const std::vector<uint32_t> &spirv,
                        const char *entry, Pipeline *p, bool is_vertex,
                        std::vector<std::pair<std::string, uint32_t>> *attrib_names) {
   try {
      spirv_cross::CompilerGLSL c(spirv);
      spirv_cross::CompilerGLSL::Options opts;
      opts.version = 120;
      opts.es = false;
      opts.enable_420pack_extension = false;
      opts.emit_push_constant_as_uniform_buffer = false;
      c.set_common_options(opts);
      auto res = c.get_shader_resources();
      // Give varyings location-based names so the vertex output and fragment
      // input interfaces always match, regardless of SPIR-V debug names.
      if (is_vertex) {
         for (auto &o : res.stage_outputs) {
            uint32_t loc = c.get_decoration(o.id, spv::DecorationLocation);
            spirv_cross_util::rename_interface_variable(c, res.stage_outputs, loc,
                                                        "vglv_" + std::to_string(loc));
         }
      } else {
         for (auto &i : res.stage_inputs) {
            uint32_t loc = c.get_decoration(i.id, spv::DecorationLocation);
            spirv_cross_util::rename_interface_variable(c, res.stage_inputs, loc,
                                                        "vglv_" + std::to_string(loc));
         }
      }
      for (auto &ubo : res.uniform_buffers) c.flatten_buffer_block(ubo.id);
      for (auto &pc : res.push_constant_buffers) c.flatten_buffer_block(pc.id);

      // reflection: vertex inputs
      if (is_vertex) {
         for (auto &in : res.stage_inputs) {
            uint32_t loc = c.get_decoration(in.id, spv::DecorationLocation);
            attrib_names->push_back({in.name, loc});
         }
      }
      // reflection: flattened UBOs / push constants
      for (auto &ubo : res.uniform_buffers) {
         uint32_t set = c.get_decoration(ubo.id, spv::DecorationDescriptorSet);
         uint32_t binding = c.get_decoration(ubo.id, spv::DecorationBinding);
         uint32_t size = (uint32_t)c.get_declared_struct_size(c.get_type(ubo.base_type_id));
         Pipeline::PipeUbo u;
         u.name = ubo.name;
         u.set = set; u.binding = binding;
         u.vec4_count = (size + 15) / 16;
         u.push_constant = false;
         bool dup = false;
         for (auto &e : p->ubos) if (e.name == u.name) dup = true;
         if (!dup) p->ubos.push_back(u);
      }
      for (auto &pc : res.push_constant_buffers) {
         uint32_t size = (uint32_t)c.get_declared_struct_size(c.get_type(pc.base_type_id));
         Pipeline::PipeUbo u;
         u.name = pc.name;
         u.vec4_count = (size + 15) / 16;
         u.push_constant = true;
         bool dup = false;
         for (auto &e : p->ubos) if (e.name == u.name) dup = true;
         if (!dup) p->ubos.push_back(u);
      }
      // reflection: sampled images
      for (auto &img : res.sampled_images) {
         uint32_t set = c.get_decoration(img.id, spv::DecorationDescriptorSet);
         uint32_t binding = c.get_decoration(img.id, spv::DecorationBinding);
         Pipeline::PipeTex t;
         t.name = img.name; t.set = set; t.binding = binding;
         bool dup = false;
         for (auto &e : p->textures) if (e.name == t.name) dup = true;
         if (!dup) p->textures.push_back(t);
      }

      std::string glsl = c.compile();
      // GLSL ES 1.00 has no gl_VertexID; emulate it with an attribute that the
      // backend fills with 0..N-1 (indexed fetch then yields the index value).
      if (is_vertex && glsl.find("gl_VertexID") != std::string::npos) {
         size_t pos = 0;
         while ((pos = glsl.find("gl_VertexID", pos)) != std::string::npos) {
            glsl.replace(pos, 11, "int(vkgl_VertexID)");
            pos += 18;
         }
         size_t nl = glsl.find('\n');
         glsl.insert(nl + 1, "attribute float vkgl_VertexID;\n");
         p->uses_vertex_id = true;
      }
      if (getenv("VKGL_DUMP_GLSL")) fprintf(stderr, "---- %s ----\n%s\n", entry, glsl.c_str());
      GLuint sh = glCreateShader(gl_stage);
      const char *src = glsl.c_str();
      glShaderSource(sh, 1, &src, nullptr);
      glCompileShader(sh);
      GLint ok = 0;
      glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
      if (!ok) {
         char log[4096] = {0};
         glGetShaderInfoLog(sh, sizeof(log) - 1, nullptr, log);
         fprintf(stderr, "vkgl: shader compile failed (%s):\n%s\n%s\n", entry, log, glsl.c_str());
         glDeleteShader(sh);
         return false;
      }
      *out_shader = sh;
      return true;
   } catch (const std::exception &e) {
      fprintf(stderr, "vkgl: SPIRV-Cross failed for %s: %s\n", entry, e.what());
      return false;
   }
}

static VKAPI_ATTR VkResult VKAPI_CALL vkgl_CreateGraphicsPipelines(
   VkDevice, VkPipelineCache, uint32_t count, const VkGraphicsPipelineCreateInfo *ci,
   const VkAllocationCallbacks *, VkPipeline *out) {
   VKGL_LOG("CreateGraphicsPipelines count=%u\n", count);
   for (uint32_t n = 0; n < count; n++) {
      auto *p = new Pipeline();
      p->type = VK_OBJECT_TYPE_PIPELINE;
      p->info = ci[n];
      p->layout = from_handle<PipelineLayout>((uint64_t)ci[n].layout);
      p->rp = from_handle<RenderPass>((uint64_t)ci[n].renderPass);
      p->topology = ci[n].pInputAssemblyState ? ci[n].pInputAssemblyState->topology : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      GlCtx::get().make_current();

      std::vector<std::pair<std::string, uint32_t>> attribs;
      std::vector<GLuint> shaders;
      for (uint32_t i = 0; i < ci[n].stageCount; i++) {
         const auto &st = ci[n].pStages[i];
         auto *mod = from_handle<ShaderModule>((uint64_t)st.module);
         GLenum gs = st.stage == VK_SHADER_STAGE_VERTEX_BIT ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;
         GLuint sh = 0;
         if (!build_stage(&sh, gs, mod->code, st.pName, p, st.stage == VK_SHADER_STAGE_VERTEX_BIT, &attribs)) {
            delete p;
            return VK_ERROR_INITIALIZATION_FAILED;
         }
         shaders.push_back(sh);
      }
      GLuint prog = glCreateProgram();
      for (GLuint sh : shaders) glAttachShader(prog, sh);
      for (auto &a : attribs) glBindAttribLocation(prog, a.second, a.first.c_str());
      if (p->uses_vertex_id) glBindAttribLocation(prog, 15, "vkgl_VertexID");
      glLinkProgram(prog);
      GLint ok = 0;
      glGetProgramiv(prog, GL_LINK_STATUS, &ok);
      if (!ok) {
         char log[4096] = {0};
         glGetProgramInfoLog(prog, sizeof(log) - 1, nullptr, log);
         fprintf(stderr, "vkgl: link failed: %s\n", log);
         delete p;
         return VK_ERROR_INITIALIZATION_FAILED;
      }
      for (GLuint sh : shaders) { glDetachShader(prog, sh); glDeleteShader(sh); }
      p->program = prog;
      p->attribs = attribs;

      for (auto &u : p->ubos) u.loc = glGetUniformLocation(prog, u.name.c_str());
      for (auto &t : p->textures) t.loc = glGetUniformLocation(prog, t.name.c_str());

      // static pipeline state
      if (ci[n].pRasterizationState) {
         p->cull = ci[n].pRasterizationState->cullMode;
         p->front = ci[n].pRasterizationState->frontFace;
      }
      if (ci[n].pDepthStencilState && ci[n].pDepthStencilState->depthTestEnable) {
         p->depth_test = ci[n].pDepthStencilState->depthTestEnable;
         p->depth_write = ci[n].pDepthStencilState->depthWriteEnable;
         p->depth_op = ci[n].pDepthStencilState->depthCompareOp;
      }
      if (ci[n].pMultisampleState) {}
      if (ci[n].pColorBlendState && ci[n].pColorBlendState->attachmentCount) {
         p->blend = ci[n].pColorBlendState->pAttachments[0].blendEnable;
      }
      if (ci[n].pVertexInputState && ci[n].pVertexInputState->vertexBindingDescriptionCount)
         p->vib = ci[n].pVertexInputState->pVertexBindingDescriptions[0];
      if (ci[n].pVertexInputState) {
         auto *via = ci[n].pVertexInputState->pVertexAttributeDescriptions;
         for (uint32_t i = 0; i < ci[n].pVertexInputState->vertexAttributeDescriptionCount; i++)
            p->via.push_back(via[i]);
      }
      p->built = true;
      VKGL_LOG("pipeline built program=%u ubos=%zu textures=%zu attribs=%zu\n", p->program, p->ubos.size(), p->textures.size(), p->attribs.size());
      out[n] = (VkPipeline)to_handle(p);
      reg(p, (uint64_t)out[n]);
   }
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL vkgl_DestroyPipeline(VkDevice, VkPipeline h, const VkAllocationCallbacks *) {
   auto *p = from_handle<Pipeline>((uint64_t)h);
   if (p->program) { GlCtx::get().make_current(); glDeleteProgram(p->program); }
   unreg((uint64_t)h);
   delete p;
}

static GLenum compare_op_to_gl(VkCompareOp op) {
   switch (op) {
   case VK_COMPARE_OP_NEVER: return GL_NEVER;
   case VK_COMPARE_OP_LESS: return GL_LESS;
   case VK_COMPARE_OP_EQUAL: return GL_EQUAL;
   case VK_COMPARE_OP_LESS_OR_EQUAL: return GL_LEQUAL;
   case VK_COMPARE_OP_GREATER: return GL_GREATER;
   case VK_COMPARE_OP_NOT_EQUAL: return GL_NOTEQUAL;
   case VK_COMPARE_OP_GREATER_OR_EQUAL: return GL_GEQUAL;
   case VK_COMPARE_OP_ALWAYS: default: return GL_ALWAYS;
   }
}

void vkgl_execute_draw(Pipeline *p, std::vector<DescriptorSet *> &sets, Buffer *vbuf, VkDeviceSize voff,
                       Buffer *ibuf, VkDeviceSize ioff, const Cmd &cmd) {
   if (!p || !p->program) return;
   GlCtx::get().make_current();
   glUseProgram(p->program);

   // fixed state
   if (p->depth_test) {
      glEnable(GL_DEPTH_TEST);
      glDepthFunc(compare_op_to_gl(p->depth_op));
      glDepthMask(p->depth_write ? GL_TRUE : GL_FALSE);
   } else {
      glDisable(GL_DEPTH_TEST);
   }
   if (p->cull != VK_CULL_MODE_NONE) {
      glEnable(GL_CULL_FACE);
      glCullFace(p->cull == VK_CULL_MODE_FRONT_BIT ? GL_FRONT : GL_BACK);
      /* Vulkan NDC is Y-down while GL is Y-up, so the winding flips */
      glFrontFace(p->front == VK_FRONT_FACE_CLOCKWISE ? GL_CCW : GL_CW);
   } else {
      glDisable(GL_CULL_FACE);
   }
   if (p->blend) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); }
   else glDisable(GL_BLEND);

   // vertex buffer content: upload shadow memory
   if (vbuf && vbuf->mem && vbuf->gl) {
      glBindBuffer(GL_ARRAY_BUFFER, vbuf->gl);
      if (vbuf->mem->mapped || vbuf->uploaded_revision != vbuf->mem->revision) {
         glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)vbuf->size, vbuf->mem->shadow.data() + vbuf->mem_offset);
         vbuf->uploaded_revision = vbuf->mem->revision;
      }
   }
   for (auto &a : p->via) {
      GLint comps; GLboolean norm;
      GLenum type = gl_type_for_format(a.format, &comps, &norm);
      glEnableVertexAttribArray(a.location);
      glBindBuffer(GL_ARRAY_BUFFER, vbuf ? vbuf->gl : 0);
      glVertexAttribPointer(a.location, comps, type, norm,
                            (GLsizei)p->vib.stride,
                            (const void *)(uintptr_t)(voff + a.offset));
   }

   VKGL_LOG("draw: prog=%u ubos=%zu tex=%zu draws=%u idx=%d\n", p->program, p->ubos.size(), p->textures.size(), cmd.count, cmd.kind == Cmd::DrawIndexed);

   // uniform buffers (flattened to vec4 arrays)
   for (auto &u : p->ubos) {
      if (u.loc < 0) { VKGL_LOG("ubo %s location missing\n", u.name.c_str()); continue; }
      if (u.push_constant) continue;
      for (auto *s : sets) {
         if (!s) continue;
         for (size_t i = 0; i < s->layout->bindings.size(); i++) {
            const auto &b = s->layout->bindings[i];
            if (b.binding != u.binding) continue;
            if (b.descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
                b.descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) continue;
            if (!s->written[i]) continue;
            const auto &bi = s->buffers[i];
            auto *buf = from_handle<Buffer>((uint64_t)bi.buffer);
            if (!buf || !buf->mem) continue;
            VkDeviceSize dyn = 0;
            for (auto &d : s->dynamic_offsets) if (d.first == u.binding) dyn = d.second;
            const uint8_t *data = buf->mem->shadow.data() + buf->mem_offset + bi.offset + dyn;
            static int ubo_log = 0;
            if (ubo_log++ < 8) {
               const float *f = (const float *)data;
               VKGL_LOG("ubo %s loc=%d n=%u data0=(%.3f %.3f %.3f %.3f) data1=(%.3f %.3f %.3f %.3f)\n",
                        u.name.c_str(), u.loc, u.vec4_count, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
            }
            glUniform4fv(u.loc, (GLsizei)u.vec4_count, (const GLfloat *)data);
         }
      }
   }

   // textures
   int unit = 0;
   for (auto &t : p->textures) {
      if (t.loc < 0) continue;
      for (auto *s : sets) {
         if (!s) continue;
         for (size_t i = 0; i < s->layout->bindings.size(); i++) {
            const auto &b = s->layout->bindings[i];
            if (b.binding != t.binding) continue;
            if (!s->written[i]) continue;
            auto *view = from_handle<ImageView>((uint64_t)s->images[i].imageView);
            if (!view || !view->image) { VKGL_LOG("texture binding %u has no view\n", t.binding); continue; }
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_2D, view->image->tex);
            // linear-tiled images are filled by mapping their memory directly;
            // push that shadow data into the GL texture before sampling
            if (view->image->tiling == VK_IMAGE_TILING_LINEAR && view->image->mem) {
               ensure_gl_texture(view->image);
               glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
               if (view->image->mem->mapped || view->image->uploaded_revision != view->image->mem->revision)
                  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                               (GLsizei)view->image->extent.width, (GLsizei)view->image->extent.height,
                               view->image->gl_format, view->image->gl_type,
                               view->image->mem->shadow.data() + view->image->mem_offset);
               view->image->uploaded_revision = view->image->mem->revision;
            }
            glUniform1i(t.loc, unit);
            VKGL_LOG("texture bound binding=%u tex=%u unit=%d loc=%d img=%p\n", t.binding, view->image->tex, unit, t.loc, (void*)view->image);
            unit++;
         }
      }
   }

   if (p->uses_vertex_id) {
      static GLuint vid_buf = 0;
      static const int vid_count = 65536;
      if (!vid_buf) {
         glGenBuffers(1, &vid_buf);
         glBindBuffer(GL_ARRAY_BUFFER, vid_buf);
         std::vector<float> ids(vid_count);
         for (int i = 0; i < vid_count; i++) ids[i] = (float)i;
         glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vid_count * sizeof(float)), ids.data(), GL_STATIC_DRAW);
      }
      glBindBuffer(GL_ARRAY_BUFFER, vid_buf);
      glEnableVertexAttribArray(15);
      glVertexAttribPointer(15, 1, GL_FLOAT, GL_FALSE, 0, (const void *)0);
   }

   if (cmd.kind == Cmd::DrawIndexed) {
      if (ibuf && ibuf->gl) {
         glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibuf->gl);
         if (ibuf->mem->mapped || ibuf->uploaded_revision != ibuf->mem->revision) {
            glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, (GLsizeiptr)ibuf->size, ibuf->mem->shadow.data() + ibuf->mem_offset);
            ibuf->uploaded_revision = ibuf->mem->revision;
         }
         GLenum it = GL_UNSIGNED_SHORT;
         glDrawElements(GL_TRIANGLES, (GLsizei)cmd.count, it,
                        (const void *)(uintptr_t)(ioff + (VkDeviceSize)cmd.first * (it == GL_UNSIGNED_INT ? 4 : 2)));
      }
   } else {
      glDrawArrays(GL_TRIANGLES, (GLint)cmd.first, (GLsizei)cmd.count);
   }
   static int glerr_log = 0;
   GLenum err = glGetError();
   if (err != GL_NO_ERROR && glerr_log++ < 10) VKGL_LOG("GL error 0x%x after draw\n", err);
}

// ---------------------------------------------------------------------------
// presentation support / optional entry points the loader probes
// ---------------------------------------------------------------------------
static VKAPI_ATTR VkBool32 VKAPI_CALL vkgl_GetPhysicalDeviceWaylandPresentationSupportKHR(
   VkPhysicalDevice, uint32_t, struct wl_display *) { return VK_TRUE; }
static VKAPI_ATTR VkBool32 VKAPI_CALL vkgl_GetPhysicalDeviceXlibPresentationSupportKHR(
   VkPhysicalDevice, uint32_t, void *, unsigned long) { return VK_FALSE; }
static VKAPI_ATTR VkBool32 VKAPI_CALL vkgl_GetPhysicalDeviceXcbPresentationSupportKHR(
   VkPhysicalDevice, uint32_t, void *, uint32_t) { return VK_FALSE; }
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_GetPhysicalDevicePresentRectanglesKHR(VkPhysicalDevice, VkSurfaceKHR,
                                                                                 uint32_t *n, VkRect2D *) {
   *n = 0; return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_EnumeratePhysicalDeviceGroups(VkInstance, uint32_t *n, void *) {
   *n = 0; return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL vkgl_GetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f) {
   vkgl_GetPhysicalDeviceFeatures(pd, &f->features);
}
static VKAPI_ATTR void VKAPI_CALL vkgl_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice pd, VkFormat fmt,
                                                                           VkFormatProperties2 *p) {
   vkgl_GetPhysicalDeviceFormatProperties(pd, fmt, &p->formatProperties);
}
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_GetPhysicalDeviceImageFormatProperties2(
   VkPhysicalDevice pd, const VkPhysicalDeviceImageFormatInfo2 *info, VkImageFormatProperties2 *p) {
   return vkgl_GetPhysicalDeviceImageFormatProperties(pd, info->format, info->type, info->tiling,
                                                      info->usage, info->flags, &p->imageFormatProperties);
}
static VKAPI_ATTR void VKAPI_CALL vkgl_GetPhysicalDeviceQueueFamilyProperties2(
   VkPhysicalDevice pd, uint32_t *n, VkQueueFamilyProperties2 *p) {
   if (!p) { vkgl_GetPhysicalDeviceQueueFamilyProperties(pd, n, nullptr); return; }
   std::vector<VkQueueFamilyProperties> props(*n);
   uint32_t cnt = *n;
   vkgl_GetPhysicalDeviceQueueFamilyProperties(pd, &cnt, props.data());
   for (uint32_t i = 0; i < cnt; i++) p[i].queueFamilyProperties = props[i];
   *n = cnt;
}
static VKAPI_ATTR void VKAPI_CALL vkgl_GetPhysicalDeviceMemoryProperties2(
   VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties2 *p) {
   vkgl_GetPhysicalDeviceMemoryProperties(pd, &p->memoryProperties);
}
static VKAPI_ATTR void VKAPI_CALL vkgl_GetPhysicalDeviceSparseImageFormatProperties2(
   VkPhysicalDevice, const VkPhysicalDeviceSparseImageFormatInfo2 *, uint32_t *n, VkSparseImageFormatProperties2 *) {
   *n = 0;
}
static VKAPI_ATTR void VKAPI_CALL vkgl_GetPhysicalDeviceExternalBufferProperties(
   VkPhysicalDevice, const VkPhysicalDeviceExternalBufferInfo *, VkExternalBufferProperties *p) {
   memset(p, 0, sizeof(*p));
}
static VKAPI_ATTR void VKAPI_CALL vkgl_GetPhysicalDeviceExternalFenceProperties(
   VkPhysicalDevice, const VkPhysicalDeviceExternalFenceInfo *, VkExternalFenceProperties *p) {
   memset(p, 0, sizeof(*p));
}
static VKAPI_ATTR void VKAPI_CALL vkgl_GetPhysicalDeviceExternalSemaphoreProperties(
   VkPhysicalDevice, const VkPhysicalDeviceExternalSemaphoreInfo *, VkExternalSemaphoreProperties *p) {
   memset(p, 0, sizeof(*p));
}

// surface capabilities/formats 2 (VK_KHR_get_surface_capabilities2)
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_GetPhysicalDeviceSurfaceCapabilities2KHR(
   VkPhysicalDevice pd, const VkPhysicalDeviceSurfaceInfo2KHR *info, VkSurfaceCapabilities2KHR *caps) {
   return vkgl_GetPhysicalDeviceSurfaceCapabilitiesKHR(pd, info->surface, &caps->surfaceCapabilities);
}
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_GetPhysicalDeviceSurfaceFormats2KHR(
   VkPhysicalDevice pd, const VkPhysicalDeviceSurfaceInfo2KHR *info, uint32_t *n,
   VkSurfaceFormat2KHR *formats) {
   VkSurfaceFormatKHR f[8];
   uint32_t cnt = 8;
   VkResult r = vkgl_GetPhysicalDeviceSurfaceFormatsKHR(pd, info->surface, &cnt, f);
   if (r != VK_SUCCESS) return r;
   if (!formats) { *n = cnt; return VK_SUCCESS; }
   uint32_t c = std::min(cnt, *n);
   for (uint32_t i = 0; i < c; i++) formats[i].surfaceFormat = f[i];
   *n = c;
   return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL vkgl_GetDeviceGroupSurfacePresentModesKHR(
   VkDevice, VkSurfaceKHR, VkDeviceGroupPresentModeFlagsKHR *modes) {
   *modes = 0;
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL vkgl_GetImageSubresourceLayout(VkDevice, VkImage img,
                                                                 const VkImageSubresource *sub,
                                                                 VkSubresourceLayout *layout) {
   auto *im = from_handle<Image>((uint64_t)img);
   (void)sub;
   memset(layout, 0, sizeof(*layout));
   if (!im) return;
   layout->rowPitch = (VkDeviceSize)im->extent.width * 4;
   layout->size = layout->rowPitch * im->extent.height;
}
