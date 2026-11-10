/* Three exact offscreen uniform-array updates/readbacks; no WSI or swapchain. */
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

static unsigned char *load_file(const char *name,size_t *size) {
   FILE *f=fopen(name,"rb"); if(!f){perror(name);exit(2);}
   fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
   if(n<=0||n%4){fclose(f);exit(2);} unsigned char *p=malloc((size_t)n);
   if(!p||fread(p,1,(size_t)n,f)!=(size_t)n){fclose(f);exit(2);}
   fclose(f);*size=(size_t)n;return p;
}

int main(void) {
   const char *pixel_path=getenv("LG110_OFFSCREEN_PIXEL");
   if (!pixel_path) { fprintf(stderr,"set LG110_OFFSCREEN_PIXEL\n"); return 1; }
   const char *large_text=getenv("LG110_OFFSCREEN_LARGE");
   int large=large_text && !strcmp(large_text,"1");
   const char *dual_text=getenv("LG110_OFFSCREEN_DUAL");
   int dual=dual_text && !strcmp(dual_text,"1");
   if(!large||!dual){fprintf(stderr,"uniform test requires LG110_OFFSCREEN_LARGE=1 LG110_OFFSCREEN_DUAL=1\n");return 1;}
   uint32_t width=large?640:64, height=large?480:64;
   printf("offscreen_uniform_extent=%ux%u\n",width,height);
   VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="offscreen-uniform",.apiVersion=VK_API_VERSION_1_0};
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

   VkBufferCreateInfo uniform_ci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=144,
      .usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
   VkBuffer uniform_buffer; CK(vkCreateBuffer(device,&uniform_ci,NULL,&uniform_buffer));
   VkMemoryRequirements uniform_req; vkGetBufferMemoryRequirements(device,uniform_buffer,&uniform_req);
   uint32_t uniform_type=memory_type(memory_properties,uniform_req.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   if(uniform_type==UINT32_MAX)return 2;
   VkMemoryAllocateInfo uniform_ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize=uniform_req.size,.memoryTypeIndex=uniform_type};
   VkDeviceMemory uniform_memory; CK(vkAllocateMemory(device,&uniform_ai,NULL,&uniform_memory));
   CK(vkBindBufferMemory(device,uniform_buffer,uniform_memory,0));
   float *uniform_mapped=NULL; CK(vkMapMemory(device,uniform_memory,0,144,0,(void **)&uniform_mapped));

   VkDescriptorSetLayoutBinding binding={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT};
   VkDescriptorSetLayoutCreateInfo layout_ci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount=1,.pBindings=&binding};
   VkDescriptorSetLayout descriptor_layout; CK(vkCreateDescriptorSetLayout(device,&layout_ci,NULL,&descriptor_layout));
   VkPipelineLayoutCreateInfo pipeline_layout_ci={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount=1,.pSetLayouts=&descriptor_layout};
   VkPipelineLayout pipeline_layout; CK(vkCreatePipelineLayout(device,&pipeline_layout_ci,NULL,&pipeline_layout));
   VkDescriptorPoolSize pool_size={.type=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.descriptorCount=1};
   VkDescriptorPoolCreateInfo descriptor_pool_ci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets=1,.poolSizeCount=1,.pPoolSizes=&pool_size};
   VkDescriptorPool descriptor_pool; CK(vkCreateDescriptorPool(device,&descriptor_pool_ci,NULL,&descriptor_pool));
   VkDescriptorSetAllocateInfo descriptor_ai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool=descriptor_pool,.descriptorSetCount=1,.pSetLayouts=&descriptor_layout};
   VkDescriptorSet descriptor_set; CK(vkAllocateDescriptorSets(device,&descriptor_ai,&descriptor_set));
   VkDescriptorBufferInfo descriptor_buffer={.buffer=uniform_buffer,.offset=0,.range=144};
   VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=descriptor_set,
      .dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.pBufferInfo=&descriptor_buffer};
   vkUpdateDescriptorSets(device,1,&write,0,NULL);

   size_t vs_size=0,fs_size=0;
   unsigned char *vs=load_file("grad.vert.spv",&vs_size);
   unsigned char *fs=load_file("uniform-array.frag.spv",&fs_size);
   VkShaderModuleCreateInfo shader_ci={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
   VkShaderModule vertex_shader,fragment_shader;
   shader_ci.codeSize=vs_size;shader_ci.pCode=(const uint32_t *)vs;CK(vkCreateShaderModule(device,&shader_ci,NULL,&vertex_shader));
   shader_ci.codeSize=fs_size;shader_ci.pCode=(const uint32_t *)fs;CK(vkCreateShaderModule(device,&shader_ci,NULL,&fragment_shader));
   VkPipelineShaderStageCreateInfo stages[2]={
      {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vertex_shader,.pName="main"},
      {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fragment_shader,.pName="main"}};
   VkPipelineVertexInputStateCreateInfo vertex_input={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   VkPipelineInputAssemblyStateCreateInfo input_assembly={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkViewport viewport={0,0,(float)width,(float)height,0,1};
   VkRect2D scissor={{0,0},{width,height/2}};
   VkPipelineViewportStateCreateInfo viewport_state={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount=1,.scissorCount=1};
   VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1};
   VkPipelineMultisampleStateCreateInfo multisample={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
   VkPipelineColorBlendAttachmentState blend_attachment={.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT};
   VkPipelineColorBlendStateCreateInfo blend={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount=1,.pAttachments=&blend_attachment};
   VkDynamicState dynamic_states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
   VkPipelineDynamicStateCreateInfo dynamic={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount=2,.pDynamicStates=dynamic_states};
   VkGraphicsPipelineCreateInfo pipeline_ci={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount=2,.pStages=stages,.pVertexInputState=&vertex_input,.pInputAssemblyState=&input_assembly,
      .pViewportState=&viewport_state,.pRasterizationState=&raster,.pMultisampleState=&multisample,
      .pColorBlendState=&blend,.pDynamicState=&dynamic,.layout=pipeline_layout,.renderPass=render_pass};
   VkPipeline pipeline; CK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pipeline_ci,NULL,&pipeline));

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

   size_t frame_bytes=8;
   const unsigned char expected[24]={
      0,0,255,255, 0,0,0,255,
      16,16,255,255, 0,0,0,255,
      255,16,16,255, 0,0,0,255};
   unsigned char actual[24]; memset(actual,0xa5,sizeof(actual));
   uint32_t valid_mask=0;
   for (uint32_t frame=0;frame<3;frame++) {
      CK(vkResetFences(device,1,&fence)); CK(vkResetCommandBuffer(command_buffer,0));
      VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
      CK(vkBeginCommandBuffer(command_buffer,&begin));
      float update_color[36]={0};
      if(frame==0){update_color[32]=1;update_color[35]=1;}
      else if(frame==1){
         update_color[0]=.25f;update_color[9]=.25f;update_color[18]=.25f;update_color[23]=.25f;
         update_color[32]=1;update_color[35]=1;
      }else{
         update_color[9]=.25f;update_color[18]=.25f;update_color[23]=.25f;update_color[24]=.25f;
         update_color[34]=1;update_color[35]=1;
      }
      vkCmdUpdateBuffer(command_buffer,uniform_buffer,0,sizeof(update_color),update_color);
      update_color[0]=1;update_color[32]=0;update_color[34]=0;
      VkClearValue clear={.color={{0,0,0,1}}};
      VkRenderPassBeginInfo rp={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
         .renderPass=render_pass,.framebuffer=framebuffer,.renderArea={{0,0},{width,height}},
         .clearValueCount=1,.pClearValues=&clear};
      vkCmdBeginRenderPass(command_buffer,&rp,VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(command_buffer,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
      vkCmdBindDescriptorSets(command_buffer,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_layout,0,1,&descriptor_set,0,NULL);
      vkCmdSetViewport(command_buffer,0,1,&viewport);
      vkCmdSetScissor(command_buffer,0,1,&scissor);
      vkCmdDraw(command_buffer,3,1,0,0);
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
      memset(uniform_mapped,0,144);
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
   if (valid_mask!=7) { printf("FAIL vktest-offscreen-uniform\n"); return 1; }
   printf("PASS vktest-offscreen-uniform exact=red,red_plus_16,blue_plus_16\n"); return 0;
}
