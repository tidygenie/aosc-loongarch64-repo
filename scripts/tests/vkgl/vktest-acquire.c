/* Swapchain acquire exclusivity, acquire fence, and release regression test. */
#define VK_USE_PLATFORM_WAYLAND_KHR 1
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct wl_compositor *compositor;
static struct xdg_wm_base *wm_base;

static void reg_global(void *data, struct wl_registry *registry, uint32_t name,
                       const char *interface, uint32_t version) {
   (void)data; (void)version;
   if (!strcmp(interface, "wl_compositor"))
      compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
   else if (!strcmp(interface, "xdg_wm_base"))
      wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
}
static void reg_remove(void *data, struct wl_registry *registry, uint32_t name) {
   (void)data; (void)registry; (void)name;
}
static const struct wl_registry_listener reg_listener = {reg_global, reg_remove};
static void wm_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
   (void)data; xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener wm_listener = {wm_ping};
static void xdg_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
   (void)data; xdg_surface_ack_configure(surface, serial);
}
static const struct xdg_surface_listener xdg_listener = {xdg_configure};
static void top_configure(void *data, struct xdg_toplevel *top, int32_t width,
                          int32_t height, struct wl_array *states) {
   (void)data; (void)top; (void)width; (void)height; (void)states;
}
static void top_close(void *data, struct xdg_toplevel *top) { (void)data; (void)top; }
static const struct xdg_toplevel_listener top_listener = {
   .configure = top_configure,
   .close = top_close,
};

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
   fprintf(stderr, "FAIL line %d: %s = %d\n", __LINE__, #x, r_); exit(1); } } while (0)

int main(void) {
   struct wl_display *display = wl_display_connect(NULL);
   if (!display) { fprintf(stderr, "no wayland display\n"); return 1; }
   struct wl_registry *registry = wl_display_get_registry(display);
   wl_registry_add_listener(registry, &reg_listener, NULL);
   wl_display_roundtrip(display);
   wl_registry_destroy(registry);
   if (!compositor || !wm_base) { fprintf(stderr, "missing globals\n"); return 1; }
   xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
   struct wl_surface *wl_surface = wl_compositor_create_surface(compositor);
   struct xdg_surface *xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, wl_surface);
   xdg_surface_add_listener(xdg_surface, &xdg_listener, NULL);
   struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg_surface);
   xdg_toplevel_add_listener(toplevel, &top_listener, NULL);
   xdg_toplevel_set_title(toplevel, "vkgl acquire test");
   wl_surface_commit(wl_surface);
   wl_display_roundtrip(display);

   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "vktest-acquire", .apiVersion = VK_API_VERSION_1_0};
   const char *instance_extensions[] = {"VK_KHR_surface", "VK_KHR_wayland_surface"};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app, .enabledExtensionCount = 2,
      .ppEnabledExtensionNames = instance_extensions};
   VkInstance instance; CK(vkCreateInstance(&ici, NULL, &instance));
   uint32_t gpu_count = 0; CK(vkEnumeratePhysicalDevices(instance, &gpu_count, NULL));
   if (gpu_count != 1) { fprintf(stderr, "expected one physical device\n"); return 1; }
   VkPhysicalDevice gpu; CK(vkEnumeratePhysicalDevices(instance, &gpu_count, &gpu));
   VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(gpu, &properties);
   if (!strstr(properties.deviceName, "vkgl")) { fprintf(stderr, "wrong device: %s\n", properties.deviceName); return 1; }

   VkWaylandSurfaceCreateInfoKHR sci = {.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
      .display = display, .surface = wl_surface};
   VkSurfaceKHR surface; CK(vkCreateWaylandSurfaceKHR(instance, &sci, NULL, &surface));
   float priority = 1.0f;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueCount = 1, .pQueuePriorities = &priority};
   const char *device_extensions[] = {"VK_KHR_swapchain"};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
      .enabledExtensionCount = 1, .ppEnabledExtensionNames = device_extensions};
   VkDevice device; CK(vkCreateDevice(gpu, &dci, NULL, &device));
   VkQueue queue; vkGetDeviceQueue(device, 0, 0, &queue);

   VkSurfaceCapabilitiesKHR capabilities;
   CK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &capabilities));
   uint32_t format_count = 0;
   CK(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &format_count, NULL));
   VkSurfaceFormatKHR *formats = calloc(format_count, sizeof(*formats));
   CK(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &format_count, formats));
   VkExtent2D extent = {320, 240};
   VkSwapchainCreateInfoKHR swci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = surface, .minImageCount = capabilities.minImageCount,
      .imageFormat = formats[0].format, .imageColorSpace = formats[0].colorSpace,
      .imageExtent = extent, .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE};
   VkSwapchainKHR swapchain; CK(vkCreateSwapchainKHR(device, &swci, NULL, &swapchain));
   uint32_t image_count = 0; CK(vkGetSwapchainImagesKHR(device, swapchain, &image_count, NULL));
   if (image_count < 2) { fprintf(stderr, "not enough images\n"); return 1; }

   VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence; CK(vkCreateFence(device, &fci, NULL, &fence));
   uint32_t *indices = calloc(image_count, sizeof(*indices));
   for (uint32_t i = 0; i < image_count; i++) {
      CK(vkAcquireNextImageKHR(device, swapchain, 0, VK_NULL_HANDLE, fence, &indices[i]));
      CK(vkGetFenceStatus(device, fence));
      for (uint32_t j = 0; j < i; j++) {
         if (indices[j] == indices[i]) {
            fprintf(stderr, "image %u acquired twice before present\n", indices[i]);
            return 1;
         }
      }
      CK(vkResetFences(device, 1, &fence));
   }
   uint32_t extra = UINT32_MAX;
   VkResult result = vkAcquireNextImageKHR(device, swapchain, 0, VK_NULL_HANDLE,
                                           VK_NULL_HANDLE, &extra);
   if (result != VK_TIMEOUT && result != VK_NOT_READY) {
      fprintf(stderr, "exhausted swapchain acquire returned %d\n", result);
      return 1;
   }

   VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .swapchainCount = 1, .pSwapchains = &swapchain};
   for (uint32_t i = 0; i < image_count; i++) {
      present.pImageIndices = &indices[i];
      CK(vkQueuePresentKHR(queue, &present));
   }
   wl_display_roundtrip(display);
   uint32_t reacquired = UINT32_MAX;
   CK(vkAcquireNextImageKHR(device, swapchain, 1000000000, VK_NULL_HANDLE,
                            VK_NULL_HANDLE, &reacquired));
   present.pImageIndices = &reacquired;
   CK(vkQueuePresentKHR(queue, &present));
   wl_display_roundtrip(display);

   vkDestroyFence(device, fence, NULL);
   vkDestroySwapchainKHR(device, swapchain, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroySurfaceKHR(instance, surface, NULL);
   vkDestroyInstance(instance, NULL);
   free(indices); free(formats);
   xdg_toplevel_destroy(toplevel); xdg_surface_destroy(xdg_surface);
   wl_surface_destroy(wl_surface); xdg_wm_base_destroy(wm_base);
   wl_compositor_destroy(compositor); wl_display_disconnect(display);
   printf("PASS vktest-acquire (%u exclusive images, fence signaled, release reacquired)\n", image_count);
   return 0;
}
