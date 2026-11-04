#include <vulkan/vulkan.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define CHECK(x) do { VkResult r = (x); if (r != VK_SUCCESS) { \
   fprintf(stderr, "%s failed: %d\n", #x, r); return 1; } } while (0)

int main(void) {
   VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
   app.apiVersion = VK_API_VERSION_1_0;
   VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
   ici.pApplicationInfo = &app;
   VkInstance inst;
   CHECK(vkCreateInstance(&ici, NULL, &inst));

   uint32_t n = 0;
   vkEnumeratePhysicalDevices(inst, &n, NULL);
   if (!n) { fprintf(stderr, "no devices\n"); return 1; }
   VkPhysicalDevice *pds = calloc(n, sizeof(*pds));
   vkEnumeratePhysicalDevices(inst, &n, pds);
   VkPhysicalDevice pd = pds[0];
   VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
   printf("device: %s\n", props.deviceName);

   uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, NULL);
   VkQueueFamilyProperties *qf = calloc(qn, sizeof(*qf));
   vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf);
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
   qci.queueFamilyIndex = 0; qci.queueCount = 1; qci.pQueuePriorities = &prio;
   VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
   dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
   VkDevice dev;
   CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
   VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);

   VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
   bci.size = 256; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
   VkBuffer buf; CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
   VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, buf, &mr);
   VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   uint32_t type = 0;
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((mr.memoryTypeBits & (1u << i)) &&
          (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
          (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { type = i; break; }
   VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
   mai.allocationSize = mr.size; mai.memoryTypeIndex = type;
   VkDeviceMemory mem; CHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
   CHECK(vkBindBufferMemory(dev, buf, mem, 0));

   VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
   pci.queueFamilyIndex = 0;
   VkCommandPool pool; CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));
   VkCommandBufferAllocateInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
   cbi.commandPool = pool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
   VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers(dev, &cbi, &cb));
   VkCommandBufferAllocateInfo cbi2 = cbi;
   cbi2.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
   cbi2.commandBufferCount = 1;
   VkCommandBuffer sec; CHECK(vkAllocateCommandBuffers(dev, &cbi2, &sec));
   VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   VkCommandBufferInheritanceInfo inherit = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO };
   VkCommandBufferBeginInfo bi2 = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   bi2.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
   bi2.pInheritanceInfo = &inherit;
   CHECK(vkBeginCommandBuffer(sec, &bi2));
   vkCmdFillBuffer(sec, buf, 0, VK_WHOLE_SIZE, 0xDEADBEEFu);
   CHECK(vkEndCommandBuffer(sec));

   CHECK(vkBeginCommandBuffer(cb, &bi));
   vkCmdExecuteCommands(cb, 1, &sec);
   CHECK(vkEndCommandBuffer(cb));
   VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
   si.commandBufferCount = 1; si.pCommandBuffers = &cb;
   CHECK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(q));

   void *map = NULL;
   CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &map));
   uint32_t *w = (uint32_t *)map;
   int bad = 0;
   for (int i = 0; i < 64; i++) if (w[i] != 0xDEADBEEFu) { bad = i + 1; break; }
   printf("fill verification: %s (first mismatch word %d)\n", bad ? "FAIL" : "OK", bad);
   vkUnmapMemory(dev, mem);
   vkDestroyCommandPool(dev, pool, NULL);
   vkDestroyBuffer(dev, buf, NULL);
   vkFreeMemory(dev, mem, NULL);
   vkDestroyDevice(dev, NULL);
   vkDestroyInstance(inst, NULL);
   return bad ? 1 : 0;
}
