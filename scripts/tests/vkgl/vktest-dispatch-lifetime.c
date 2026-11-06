#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { \
  fprintf(stderr, "FAIL line=%d result=%d\n", __LINE__, r_); return 1; } } while (0)

int main(void) {
  VkApplicationInfo app = {0};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "vkgl-dispatch-lifetime";
  app.apiVersion = VK_API_VERSION_1_0;
  VkInstanceCreateInfo ici = {0};
  ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ici.pApplicationInfo = &app;
  VkInstance inst = VK_NULL_HANDLE;
  CK(vkCreateInstance(&ici, NULL, &inst));
  uint32_t count = 1;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  CK(vkEnumeratePhysicalDevices(inst, &count, &phys));
  if (count != 1 || phys == VK_NULL_HANDLE) return 2;
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(phys, &props);
  printf("TEST_DEVICE %s\n", props.deviceName);
  uint32_t family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(phys, &family_count, NULL);
  if (!family_count || family_count > 64) return 3;
  VkQueueFamilyProperties families[64];
  vkGetPhysicalDeviceQueueFamilyProperties(phys, &family_count, families);
  uint32_t family = family_count;
  for (uint32_t i = 0; i < family_count; i++) {
    if (families[i].queueCount && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
      family = i;
      break;
    }
  }
  if (family == family_count) return 4;
  float priority = 1.0f;
  VkDeviceQueueCreateInfo qci = {0};
  qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qci.queueFamilyIndex = family;
  qci.queueCount = 1;
  qci.pQueuePriorities = &priority;
  VkDeviceCreateInfo dci = {0};
  dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  VkDevice dev = VK_NULL_HANDLE;
  CK(vkCreateDevice(phys, &dci, NULL, &dev));
  VkQueue queue = VK_NULL_HANDLE;
  vkGetDeviceQueue(dev, family, 0, &queue);
  if (!queue) return 5;
  for (uint32_t cycle = 0; cycle < 64; cycle++) {
    VkCommandPoolCreateInfo pci = {0};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    CK(vkCreateCommandPool(dev, &pci, NULL, &pool));
    VkCommandBufferAllocateInfo cai = {0};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    CK(vkAllocateCommandBuffers(dev, &cai, &cb));
    if (!cb) return 6;
    vkFreeCommandBuffers(dev, pool, 1, &cb);
    vkDestroyCommandPool(dev, pool, NULL);
  }
  vkDestroyDevice(dev, NULL);
  vkDestroyInstance(inst, NULL);
  puts("PASS vktest-dispatch-lifetime cycles=64");
  return 0;
}
