/* vkCmdUpdateBuffer must own its payload at record time and mutate the
 * destination only when the recorded command is submitted. */
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CK(call) do { VkResult result=(call); if (result!=VK_SUCCESS) { \
   fprintf(stderr,"Vulkan failure %d at line %d\n",result,__LINE__); return 1; \
} } while (0)

static uint32_t read_buffer(VkDevice dev, VkDeviceMemory mem) {
   void *ptr=NULL;
   if (vkMapMemory(dev,mem,0,4,0,&ptr)!=VK_SUCCESS) {
      fprintf(stderr,"vkMapMemory failed\n");
      return 0;
   }
   uint32_t value=0;
   memcpy(&value,ptr,4);
   vkUnmapMemory(dev,mem);
   return value;
}

static void write_buffer(VkDevice dev, VkDeviceMemory mem, uint32_t value) {
   void *ptr=NULL;
   if (vkMapMemory(dev,mem,0,4,0,&ptr)!=VK_SUCCESS) return;
   memcpy(ptr,&value,4);
   vkUnmapMemory(dev,mem);
}

int main(void) {
   VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName="vkgl-update-buffer",.apiVersion=VK_API_VERSION_1_0};
   VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo=&app};
   VkInstance instance=VK_NULL_HANDLE;
   CK(vkCreateInstance(&ici,NULL,&instance));
   uint32_t physical_count=1;
   VkPhysicalDevice physical=VK_NULL_HANDLE;
   CK(vkEnumeratePhysicalDevices(instance,&physical_count,&physical));
   if (physical_count!=1 || physical==VK_NULL_HANDLE) {
      fprintf(stderr,"expected one physical device\n"); return 1;
   }
   VkPhysicalDeviceProperties properties;
   vkGetPhysicalDeviceProperties(physical,&properties);
   printf("device=%s\n",properties.deviceName);
   if (strcmp(properties.deviceName,"LoongGPU LG110 (vkgl GLES2)")!=0) {
      fprintf(stderr,"wrong Vulkan device\n"); return 1;
   }
   float priority=1.0f;
   VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex=0,.queueCount=1,.pQueuePriorities=&priority};
   VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount=1,.pQueueCreateInfos=&qci};
   VkDevice dev=VK_NULL_HANDLE;
   CK(vkCreateDevice(physical,&dci,NULL,&dev));
   VkQueue queue=VK_NULL_HANDLE;
   vkGetDeviceQueue(dev,0,0,&queue);

   VkBufferCreateInfo bci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size=4,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
   VkBuffer buffer=VK_NULL_HANDLE;
   CK(vkCreateBuffer(dev,&bci,NULL,&buffer));
   VkMemoryRequirements requirements;
   vkGetBufferMemoryRequirements(dev,buffer,&requirements);
   VkMemoryAllocateInfo mai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize=requirements.size,.memoryTypeIndex=0};
   VkDeviceMemory memory=VK_NULL_HANDLE;
   CK(vkAllocateMemory(dev,&mai,NULL,&memory));
   CK(vkBindBufferMemory(dev,buffer,memory,0));

   const uint32_t initial=0x10203040U, payload=0x11223344U;
   const uint32_t changed_payload=0xaabbccddU, host_before_submit=0x5a5a5a5aU;
   write_buffer(dev,memory,initial);
   VkCommandPoolCreateInfo cpci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=0};
   VkCommandPool pool=VK_NULL_HANDLE;
   CK(vkCreateCommandPool(dev,&cpci,NULL,&pool));
   VkCommandBufferAllocateInfo cbai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
   VkCommandBuffer command=VK_NULL_HANDLE;
   CK(vkAllocateCommandBuffers(dev,&cbai,&command));
   VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   CK(vkBeginCommandBuffer(command,&begin));
   uint32_t source=payload;
   vkCmdUpdateBuffer(command,buffer,0,4,&source);
   CK(vkEndCommandBuffer(command));
   source=changed_payload;

   const uint32_t before_record_check=read_buffer(dev,memory);
   write_buffer(dev,memory,host_before_submit);
   VkFenceCreateInfo fci={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence=VK_NULL_HANDLE;
   CK(vkCreateFence(dev,&fci,NULL,&fence));
   VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount=1,.pCommandBuffers=&command};
   CK(vkQueueSubmit(queue,1,&submit,fence));
   CK(vkWaitForFences(dev,1,&fence,VK_TRUE,UINT64_MAX));
   const uint32_t after_submit=read_buffer(dev,memory);
   const uint32_t host_between_submits=0xdeadbeefU;
   write_buffer(dev,memory,host_between_submits);
   CK(vkResetFences(dev,1,&fence));
   CK(vkQueueSubmit(queue,1,&submit,fence));
   CK(vkWaitForFences(dev,1,&fence,VK_TRUE,UINT64_MAX));
   const uint32_t after_second_submit=read_buffer(dev,memory);
   printf("before_submit=%08x host_write=%08x after_submit=%08x host_between=%08x after_second=%08x expected_before=%08x expected_after=%08x\n",
      before_record_check,host_before_submit,after_submit,host_between_submits,
      after_second_submit,initial,payload);
   const int pass=before_record_check==initial && after_submit==payload &&
                  after_second_submit==payload;
   printf("%s vktest-update-buffer\n",pass?"PASS":"FAIL");
   return pass?0:1;
}
