/* Three exact offscreen clears/readbacks; no surface, swapchain, or shader. */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CK(x) do { VkResult _r=(x); if (_r!=VK_SUCCESS) { \
   fprintf(stderr,"VKFAIL %s:%d %s=%d\n",__FILE__,__LINE__,#x,_r); exit(2); } } while (0)

static uint32_t memory_type(VkPhysicalDeviceMemoryProperties p, uint32_t bits,
                            VkMemoryPropertyFlags required) {
   for (uint32_t i=0;i<p.memoryTypeCount;i++)
      if ((bits&(1u<<i)) && (p.memoryTypes[i].propertyFlags&required)==required) return i;
   return UINT32_MAX;
}

int main(void) {
   const char *pixel_path=getenv("LG110_OFFSCREEN_PIXEL");
   if (!pixel_path) { fprintf(stderr,"set LG110_OFFSCREEN_PIXEL\n"); return 1; }
   const char *large_text=getenv("LG110_OFFSCREEN_LARGE");
   int large=large_text && !strcmp(large_text,"1");
   const char *dual_text=getenv("LG110_OFFSCREEN_DUAL");
   int dual=dual_text && !strcmp(dual_text,"1");
   uint32_t width=large?640:64, height=large?480:64;
   printf("offscreen_clear_extent=%ux%u\n",width,height);
   VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="offscreen-clear",.apiVersion=VK_API_VERSION_1_0};
   VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
   VkInstance instance; CK(vkCreateInstance(&ici,NULL,&instance));
   uint32_t physical_count=0; CK(vkEnumeratePhysicalDevices(instance,&physical_count,NULL));
   if (!physical_count) return 2;
   VkPhysicalDevice *physicals=calloc(physical_count,sizeof(*physicals));
   CK(vkEnumeratePhysicalDevices(instance,&physical_count,physicals));
   VkPhysicalDevice physical=physicals[0];
   for (uint32_t i=0;i<physical_count;i++) {
      VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(physicals[i],&properties);
      printf("device[%u]=%s\n",i,properties.deviceName);
      if (strstr(properties.deviceName,"LoongGPU")) physical=physicals[i];
   }
   VkPhysicalDeviceProperties selected; vkGetPhysicalDeviceProperties(physical,&selected);
   printf("selected=%s api=0x%x\n",selected.deviceName,selected.apiVersion);
   VkPhysicalDeviceMemoryProperties memory_properties; vkGetPhysicalDeviceMemoryProperties(physical,&memory_properties);
   uint32_t queue_count=0; vkGetPhysicalDeviceQueueFamilyProperties(physical,&queue_count,NULL);
   VkQueueFamilyProperties *queues=calloc(queue_count,sizeof(*queues));
   vkGetPhysicalDeviceQueueFamilyProperties(physical,&queue_count,queues);
   uint32_t queue_family=UINT32_MAX;
   for (uint32_t i=0;i<queue_count;i++) if (queues[i].queueFlags&VK_QUEUE_GRAPHICS_BIT) { queue_family=i; break; }
   if (queue_family==UINT32_MAX) return 2;
   float priority=1.0f;
   VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=queue_family,.queueCount=1,.pQueuePriorities=&priority};
   VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci};
   VkDevice device; CK(vkCreateDevice(physical,&dci,NULL,&device));
   PFN_vkGetDeviceQueue get_queue=(PFN_vkGetDeviceQueue)vkGetDeviceProcAddr(device,"vkGetDeviceQueue");
#define GETD(name) PFN_##name name=(PFN_##name)vkGetDeviceProcAddr(device,#name)
   GETD(vkCreateImage); GETD(vkGetImageMemoryRequirements); GETD(vkAllocateMemory);
   GETD(vkBindImageMemory); GETD(vkCreateImageView); GETD(vkCreateRenderPass);
   GETD(vkCreateFramebuffer); GETD(vkCreateBuffer); GETD(vkGetBufferMemoryRequirements);
   GETD(vkBindBufferMemory); GETD(vkMapMemory); GETD(vkCreateCommandPool);
   GETD(vkAllocateCommandBuffers); GETD(vkBeginCommandBuffer); GETD(vkEndCommandBuffer);
   GETD(vkResetCommandBuffer); GETD(vkCmdBeginRenderPass); GETD(vkCmdEndRenderPass);
   GETD(vkCmdPipelineBarrier); GETD(vkCmdCopyImageToBuffer); GETD(vkCreateFence);
   GETD(vkResetFences); GETD(vkQueueSubmit); GETD(vkWaitForFences);
   VkQueue queue; get_queue(device,queue_family,0,&queue);

   VkImage image;
   VkImageCreateInfo image_ci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
      .format=VK_FORMAT_B8G8R8A8_UNORM,.extent={width,height,1},.mipLevels=1,.arrayLayers=1,
      .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
      .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
   CK(vkCreateImage(device,&image_ci,NULL,&image));
   VkMemoryRequirements image_req; vkGetImageMemoryRequirements(device,image,&image_req);
   uint32_t image_type=memory_type(memory_properties,image_req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (image_type==UINT32_MAX) image_type=memory_type(memory_properties,image_req.memoryTypeBits,0);
   VkMemoryAllocateInfo image_ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=image_req.size,.memoryTypeIndex=image_type};
   VkDeviceMemory image_memory; CK(vkAllocateMemory(device,&image_ai,NULL,&image_memory));
   CK(vkBindImageMemory(device,image,image_memory,0));
   VkImageViewCreateInfo view_ci={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image,
      .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_B8G8R8A8_UNORM,
      .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
   VkImageView view; CK(vkCreateImageView(device,&view_ci,NULL,&view));

   VkAttachmentDescription attachment={.format=VK_FORMAT_B8G8R8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,
      .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
   VkAttachmentReference color_ref={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&color_ref};
   VkRenderPassCreateInfo rp_ci={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount=1,.pAttachments=&attachment,.subpassCount=1,.pSubpasses=&subpass};
   VkRenderPass render_pass; CK(vkCreateRenderPass(device,&rp_ci,NULL,&render_pass));
   VkFramebufferCreateInfo fb_ci={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass=render_pass,.attachmentCount=1,.pAttachments=&view,.width=width,.height=height,.layers=1};
   VkFramebuffer framebuffer; CK(vkCreateFramebuffer(device,&fb_ci,NULL,&framebuffer));

   VkBufferCreateInfo buffer_ci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=dual?24:4,
      .usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
   VkBuffer buffer; CK(vkCreateBuffer(device,&buffer_ci,NULL,&buffer));
   VkMemoryRequirements buffer_req; vkGetBufferMemoryRequirements(device,buffer,&buffer_req);
   uint32_t buffer_type=memory_type(memory_properties,buffer_req.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   if (buffer_type==UINT32_MAX) return 2;
   printf("memory_types image=%u buffer=%u\n",image_type,buffer_type);
   VkMemoryAllocateInfo buffer_ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize=buffer_req.size,.memoryTypeIndex=buffer_type};
   VkDeviceMemory buffer_memory; CK(vkAllocateMemory(device,&buffer_ai,NULL,&buffer_memory));
   CK(vkBindBufferMemory(device,buffer,buffer_memory,0));
   unsigned char *mapped=NULL; CK(vkMapMemory(device,buffer_memory,0,dual?24:4,0,(void **)&mapped));

   VkCommandPoolCreateInfo pool_ci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=queue_family};
   VkCommandPool pool; CK(vkCreateCommandPool(device,&pool_ci,NULL,&pool));
   VkCommandBufferAllocateInfo cb_ai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
   VkCommandBuffer command_buffer; CK(vkAllocateCommandBuffers(device,&cb_ai,&command_buffer));
   VkFenceCreateInfo fence_ci={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence; CK(vkCreateFence(device,&fence_ci,NULL,&fence));

   const float colors[3][4]={{1,0,0,1},{0,1,0,1},{0,0,1,1}};
   const unsigned char bgra[3][4]={{0,0,255,255},{0,255,0,255},{255,0,0,255}};
   size_t frame_bytes=dual?8:4;
   unsigned char expected[24]={0}, actual[24];
   memset(actual,0xa5,sizeof(actual));
   for (uint32_t frame=0;frame<3;frame++) {
      memcpy(expected+frame*frame_bytes,bgra[frame],4);
      if (dual) memcpy(expected+frame*frame_bytes+4,bgra[frame],4);
   }
   uint32_t valid_mask=0;
   for (uint32_t frame=0;frame<3;frame++) {
      CK(vkResetFences(device,1,&fence)); CK(vkResetCommandBuffer(command_buffer,0));
      VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
      CK(vkBeginCommandBuffer(command_buffer,&begin));
      VkClearValue clear={.color={{colors[frame][0],colors[frame][1],colors[frame][2],1}}};
      VkRenderPassBeginInfo rp={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
         .renderPass=render_pass,.framebuffer=framebuffer,.renderArea={{0,0},{width,height}},
         .clearValueCount=1,.pClearValues=&clear};
      vkCmdBeginRenderPass(command_buffer,&rp,VK_SUBPASS_CONTENTS_INLINE);
      vkCmdEndRenderPass(command_buffer);
      VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
         .oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
         .image=image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
      vkCmdPipelineBarrier(command_buffer,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
      VkBufferImageCopy copies[2]={
         {.bufferOffset=dual?frame*8:0,.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
          .imageOffset={(int32_t)(width/2),large?120:32,0},.imageExtent={1,1,1}},
         {.bufferOffset=frame*8+4,.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
          .imageOffset={(int32_t)(width/2),large?360:48,0},.imageExtent={1,1,1}}};
      vkCmdCopyImageToBuffer(command_buffer,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,dual?2:1,copies);
      CK(vkEndCommandBuffer(command_buffer));
      VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command_buffer};
      CK(vkQueueSubmit(queue,1,&submit,fence)); CK(vkWaitForFences(device,1,&fence,VK_TRUE,~0ull));
      size_t base=frame*frame_bytes;
      memcpy(actual+base,mapped+(dual?frame*8:0),frame_bytes);
      int valid=!memcmp(actual+base,expected+base,frame_bytes); if (valid) valid_mask|=1u<<frame;
      printf("frame=%u actual=%02x%02x%02x%02x expected=%02x%02x%02x%02x valid=%d\n",frame,
         actual[base],actual[base+1],actual[base+2],actual[base+3],
         expected[base],expected[base+1],expected[base+2],expected[base+3],valid);
   }
   FILE *output=fopen(pixel_path,"wb"); if (!output) return 2;
   if (fwrite(actual,1,dual?24:12,output)!=(size_t)(dual?24:12)) return 2;
   fclose(output);
   printf("OFFSCREEN_PIXEL=%s valid_mask=%u\n",pixel_path,valid_mask);
   if (valid_mask!=7) { printf("FAIL vktest-offscreen-clear\n"); return 1; }
   printf("PASS vktest-offscreen-clear exact=red,green,blue\n"); return 0;
}
