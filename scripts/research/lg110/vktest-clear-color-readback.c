/* One-frame clear-only readback for independent LG110 color-field mutations.
 * There is no shader, pipeline, texture, or draw. The rendered operation is a
 * 640x480 clear to red=0, green=0.2, blue=0.4, alpha=1, followed by a
 * one-pixel CopyImageToBuffer and bounded fence wait.
 *
 * Required environment:
 *   LG110_EXPECT_CLEAR_FIELD=base|red|green|blue|alpha
 *   LG110_READBACK_PIXEL=path   raw four-byte result, saved before validation
 *   LG110_PASSIVE_CLEAR_ALPHA_05=1  use alpha=0.5 without an IB mutation
 * Red/green/blue expect 1.0; alpha accepts either UNORM encoding of 0.5.
 */
#define _DEFAULT_SOURCE 1
#define VK_USE_PLATFORM_WAYLAND_KHR 1
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct wl_display *dpy;
static struct wl_compositor *compositor;
static struct xdg_wm_base *wm_base;
static struct wl_surface *surface;
static struct xdg_surface *xdg_surf;
static struct xdg_toplevel *toplevel;

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t v)
{
   (void)d;
   (void)v;
   if (!strcmp(iface, "wl_compositor"))
      compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
   else if (!strcmp(iface, "xdg_wm_base"))
      wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
}

static void reg_remove(void *d, struct wl_registry *r, uint32_t n)
{
   (void)d;
   (void)r;
   (void)n;
}

static const struct wl_registry_listener reg_listener = {reg_global, reg_remove};

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial)
{
   (void)d;
   xdg_wm_base_pong(b, serial);
}

static const struct xdg_wm_base_listener wm_listener = {wm_ping};

static void xdg_configure(void *d, struct xdg_surface *s, uint32_t serial)
{
   (void)d;
   xdg_surface_ack_configure(s, serial);
}

static const struct xdg_surface_listener xdg_listener = {xdg_configure};

static void top_configure(void *d, struct xdg_toplevel *t, int32_t w,
                          int32_t h, struct wl_array *st)
{
   (void)d;
   (void)t;
   (void)w;
   (void)h;
   (void)st;
}

static void top_close(void *d, struct xdg_toplevel *t)
{
   (void)d;
   (void)t;
}

static const struct xdg_toplevel_listener top_listener = {
   .configure = top_configure,
   .close = top_close,
};

#define CK(x) do { \
   VkResult r_ = (x); \
   if (r_ != VK_SUCCESS) { \
      fprintf(stderr, "FAIL %s:%d %s = %d\n", \
              __FILE__, __LINE__, #x, r_); \
      exit(1); \
   } \
} while (0)

static int save_pixel(const char *path, const unsigned char pixel[4])
{
   FILE *file = fopen(path, "wb");

   if (!file) {
      perror(path);
      return 1;
   }
   if (fwrite(pixel, 1, 4, file) != 4) {
      perror("fwrite readback");
      fclose(file);
      return 1;
   }
   if (fclose(file)) {
      perror("fclose readback");
      return 1;
   }
   return 0;
}

int main(void)
{
   const char *expect = getenv("LG110_EXPECT_CLEAR_FIELD");
   const char *pixel_path = getenv("LG110_READBACK_PIXEL");
   unsigned int red_value = 0, green_value = 51, blue_value = 102;
   unsigned int alpha_value = 255;
   int alpha_midpoint = 0;
   VkInstance inst;
   VkPhysicalDevice gpu = VK_NULL_HANDLE;
   VkDevice dev;
   VkQueue q;
   VkSurfaceKHR surf;
   VkSwapchainKHR sc;
   VkFormat fmt;
   VkRenderPass rp;
   VkCommandPool pool;
   VkCommandBuffer cb;
   VkBuffer buf;
   VkDeviceMemory bufmem;
   VkFence fence;
   VkSemaphore acquire, done;
   uint32_t n = 0, idx, i;
   VkPhysicalDevice *gpus;
   VkSurfaceFormatKHR *formats;
   VkImage *images;
   VkImageView *views;
   VkFramebuffer *fbs;
   unsigned char actual[4], expected[4];
   unsigned int red_index, green_index, blue_index;
   int sentinel = 1, pixels_valid = 1;
   void *ptr;

   setbuf(stdout, NULL);
   setbuf(stderr, NULL);
   if (!expect || (strcmp(expect, "base") && strcmp(expect, "red") &&
                   strcmp(expect, "green") && strcmp(expect, "blue") &&
                   strcmp(expect, "alpha"))) {
      fprintf(stderr, "set LG110_EXPECT_CLEAR_FIELD=base|red|green|blue|alpha\n");
      return 1;
   }
   if (!pixel_path || !*pixel_path) {
      fprintf(stderr, "set LG110_READBACK_PIXEL to a raw output path\n");
      return 1;
   }
   if (!strcmp(expect, "red"))
      red_value = 255;
   else if (!strcmp(expect, "green"))
      green_value = 255;
   else if (!strcmp(expect, "blue"))
      blue_value = 255;
   else if (!strcmp(expect, "alpha")) {
      alpha_value = 128;
      alpha_midpoint = 1;
   }
   printf("clear-only readback: field=%s red=%u green=%u blue=%u alpha=%u%s\n",
          expect, red_value, green_value, blue_value, alpha_value,
          alpha_midpoint ? " (127 or 128 accepted)" : "");

   dpy = wl_display_connect(NULL);
   if (!dpy) {
      fprintf(stderr, "no wayland display\n");
      return 1;
   }
   {
      struct wl_registry *reg = wl_display_get_registry(dpy);
      wl_registry_add_listener(reg, &reg_listener, NULL);
      wl_display_roundtrip(dpy);
   }
   if (!compositor || !wm_base) {
      fprintf(stderr, "missing compositor or xdg_wm_base\n");
      return 1;
   }
   xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
   surface = wl_compositor_create_surface(compositor);
   xdg_surf = xdg_wm_base_get_xdg_surface(wm_base, surface);
   xdg_surface_add_listener(xdg_surf, &xdg_listener, NULL);
   toplevel = xdg_surface_get_toplevel(xdg_surf);
   xdg_toplevel_add_listener(toplevel, &top_listener, NULL);
   xdg_toplevel_set_title(toplevel, "LG110 clear-only readback");
   wl_surface_commit(surface);
   wl_display_roundtrip(dpy);

   {
      VkApplicationInfo app = {
         .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
         .pApplicationName = "vktest-clear-color-readback",
         .apiVersion = VK_API_VERSION_1_0,
      };
      const char *exts[] = {"VK_KHR_surface", "VK_KHR_wayland_surface"};
      VkInstanceCreateInfo ici = {
         .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
         .pApplicationInfo = &app,
         .enabledExtensionCount = 2,
         .ppEnabledExtensionNames = exts,
      };
      CK(vkCreateInstance(&ici, NULL, &inst));
   }
   CK(vkEnumeratePhysicalDevices(inst, &n, NULL));
   if (!n) {
      fprintf(stderr, "no physical devices\n");
      return 1;
   }
   gpus = calloc(n, sizeof(*gpus));
   if (!gpus)
      return 1;
   CK(vkEnumeratePhysicalDevices(inst, &n, gpus));
   for (i = 0; i < n; ++i) {
      VkPhysicalDeviceProperties props;
      vkGetPhysicalDeviceProperties(gpus[i], &props);
      printf("GPU%u type=%d name=%s\n", i, props.deviceType, props.deviceName);
      if (gpu == VK_NULL_HANDLE && props.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU)
         gpu = gpus[i];
   }
   if (gpu == VK_NULL_HANDLE) {
      fprintf(stderr, "no non-CPU physical device\n");
      return 1;
   }
   {
      VkWaylandSurfaceCreateInfoKHR sci = {
         .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
         .display = dpy,
         .surface = surface,
      };
      CK(vkCreateWaylandSurfaceKHR(inst, &sci, NULL, &surf));
   }
   {
      float prio = 1.0f;
      VkDeviceQueueCreateInfo qci = {
         .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
         .queueFamilyIndex = 0,
         .queueCount = 1,
         .pQueuePriorities = &prio,
      };
      VkPhysicalDeviceFeatures feats = {0};
      const char *device_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
      VkDeviceCreateInfo dci = {
         .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
         .queueCreateInfoCount = 1,
         .pQueueCreateInfos = &qci,
         .enabledExtensionCount = 1,
         .ppEnabledExtensionNames = device_exts,
         .pEnabledFeatures = &feats,
      };
      CK(vkCreateDevice(gpu, &dci, NULL, &dev));
   }
   vkGetDeviceQueue(dev, 0, 0, &q);

   CK(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surf, &n, NULL));
   formats = calloc(n, sizeof(*formats));
   if (!n || !formats)
      return 1;
   CK(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surf, &n, formats));
   fmt = formats[0].format;
   if (fmt != VK_FORMAT_B8G8R8A8_UNORM && fmt != VK_FORMAT_R8G8B8A8_UNORM) {
      fprintf(stderr, "unsupported readback format %d\n", fmt);
      return 1;
   }
   printf("surface format=%d\n", fmt);
   {
      VkSwapchainCreateInfoKHR scci = {
         .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
         .surface = surf,
         .minImageCount = 2,
         .imageFormat = fmt,
         .imageColorSpace = formats[0].colorSpace,
         .imageExtent = {640, 480},
         .imageArrayLayers = 1,
         .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
         .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
         .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
         .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
         .presentMode = VK_PRESENT_MODE_FIFO_KHR,
         .clipped = VK_TRUE,
      };
      CK(vkCreateSwapchainKHR(dev, &scci, NULL, &sc));
   }
   CK(vkGetSwapchainImagesKHR(dev, sc, &n, NULL));
   images = calloc(n, sizeof(*images));
   views = calloc(n, sizeof(*views));
   fbs = calloc(n, sizeof(*fbs));
   if (!images || !views || !fbs)
      return 1;
   CK(vkGetSwapchainImagesKHR(dev, sc, &n, images));
   printf("swapchain images=%u\n", n);

   {
      VkAttachmentDescription att = {
         .format = fmt,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
      };
      VkAttachmentReference ref = {
         .attachment = 0,
         .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      };
      VkSubpassDescription sub = {
         .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
         .colorAttachmentCount = 1,
         .pColorAttachments = &ref,
      };
      VkRenderPassCreateInfo rpci = {
         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
         .attachmentCount = 1,
         .pAttachments = &att,
         .subpassCount = 1,
         .pSubpasses = &sub,
      };
      CK(vkCreateRenderPass(dev, &rpci, NULL, &rp));
   }
   for (i = 0; i < n; ++i) {
      VkImageViewCreateInfo vci = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = images[i],
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = fmt,
         .components = {
            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
         },
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      };
      VkFramebufferCreateInfo fci;
      CK(vkCreateImageView(dev, &vci, NULL, &views[i]));
      fci = (VkFramebufferCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
         .renderPass = rp,
         .attachmentCount = 1,
         .pAttachments = &views[i],
         .width = 640,
         .height = 480,
         .layers = 1,
      };
      CK(vkCreateFramebuffer(dev, &fci, NULL, &fbs[i]));
   }
   {
      VkCommandPoolCreateInfo pci = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .queueFamilyIndex = 0,
      };
      VkCommandBufferAllocateInfo cai;
      CK(vkCreateCommandPool(dev, &pci, NULL, &pool));
      cai = (VkCommandBufferAllocateInfo) {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      };
      CK(vkAllocateCommandBuffers(dev, &cai, &cb));
   }
   {
      VkBufferCreateInfo bci = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .size = 4,
         .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      };
      VkMemoryRequirements req;
      VkMemoryAllocateInfo mai;
      CK(vkCreateBuffer(dev, &bci, NULL, &buf));
      vkGetBufferMemoryRequirements(dev, buf, &req);
      mai = (VkMemoryAllocateInfo) {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = req.size,
         .memoryTypeIndex = 0,
      };
      CK(vkAllocateMemory(dev, &mai, NULL, &bufmem));
      CK(vkBindBufferMemory(dev, buf, bufmem, 0));
      CK(vkMapMemory(dev, bufmem, 0, 4, 0, &ptr));
      memset(ptr, 0xa5, 4);
      vkUnmapMemory(dev, bufmem);
   }
   {
      VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      CK(vkCreateFence(dev, &fci, NULL, &fence));
      CK(vkCreateSemaphore(dev, &sci, NULL, &acquire));
      CK(vkCreateSemaphore(dev, &sci, NULL, &done));
   }

   CK(vkAcquireNextImageKHR(dev, sc, 2000000000ULL, acquire, VK_NULL_HANDLE, &idx));
   {
      VkCommandBufferBeginInfo cbi = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
      };
      VkClearValue clear = {
         .color = {.float32 = {0.0f, 0.2f, 0.4f,
                                getenv("LG110_PASSIVE_CLEAR_ALPHA_05") ? 0.5f : 1.0f}},
      };
      VkRenderPassBeginInfo rbi = {
         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
         .renderPass = rp,
         .framebuffer = fbs[idx],
         .renderArea = {{0, 0}, {640, 480}},
         .clearValueCount = 1,
         .pClearValues = &clear,
      };
      VkImageMemoryBarrier to_copy = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = 0,
         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = images[idx],
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      };
      VkBufferImageCopy copy = {
         .bufferOffset = 0,
         .bufferRowLength = 0,
         .bufferImageHeight = 0,
         .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .imageOffset = {320, 240, 0},
         .imageExtent = {1, 1, 1},
      };
      VkImageMemoryBarrier to_present = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
         .dstAccessMask = 0,
         .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = images[idx],
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      };
      CK(vkBeginCommandBuffer(cb, &cbi));
      vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdEndRenderPass(cb);
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
                           0, NULL, 1, &to_copy);
      vkCmdCopyImageToBuffer(cb, images[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             buf, 1, &copy);
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL,
                           0, NULL, 1, &to_present);
      CK(vkEndCommandBuffer(cb));
   }
   {
      VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      VkSubmitInfo si = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .waitSemaphoreCount = 1,
         .pWaitSemaphores = &acquire,
         .pWaitDstStageMask = &wait,
         .commandBufferCount = 1,
         .pCommandBuffers = &cb,
         .signalSemaphoreCount = 1,
         .pSignalSemaphores = &done,
      };
      CK(vkQueueSubmit(q, 1, &si, fence));
   }
   CK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 2000000000ULL));
   CK(vkMapMemory(dev, bufmem, 0, 4, 0, &ptr));
   memcpy(actual, ptr, 4);
   vkUnmapMemory(dev, bufmem);
   if (save_pixel(pixel_path, actual))
      return 1;

   if (fmt == VK_FORMAT_B8G8R8A8_UNORM) {
      blue_index = 0;
      green_index = 1;
      red_index = 2;
   } else {
      red_index = 0;
      green_index = 1;
      blue_index = 2;
   }
   memset(expected, 0, sizeof(expected));
   expected[red_index] = (unsigned char)red_value;
   expected[green_index] = (unsigned char)green_value;
   expected[blue_index] = (unsigned char)blue_value;
   expected[3] = (unsigned char)alpha_value;
   for (i = 0; i < 4; ++i) {
      if (i == 3 && alpha_midpoint) {
         if (actual[i] != 127 && actual[i] != 128)
            pixels_valid = 0;
      } else if (actual[i] != expected[i]) {
         pixels_valid = 0;
      }
   }
   for (i = 0; i < 4; ++i)
      if (actual[i] != 0xa5)
         sentinel = 0;
   if (sentinel)
      pixels_valid = 0;

   printf("readback center=(320,240) expected=%02x%02x%02x%02x "
          "actual=%02x%02x%02x%02x pixels_valid=%d sentinel_unchanged=%d\n",
          expected[0], expected[1], expected[2], expected[3],
          actual[0], actual[1], actual[2], actual[3], pixels_valid, sentinel);
   {
      VkPresentInfoKHR pi = {
         .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
         .waitSemaphoreCount = 1,
         .pWaitSemaphores = &done,
         .swapchainCount = 1,
         .pSwapchains = &sc,
         .pImageIndices = &idx,
      };
      CK(vkQueuePresentKHR(q, &pi));
   }
   if (!pixels_valid) {
      printf("FAIL vktest-clear-color-readback\n");
      return 1;
   }
   printf("PASS vktest-clear-color-readback\n");
   return 0;
}
