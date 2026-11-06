/* GPU CopyImage A/B: draw/src sample -> offscreen dst at a different offset,
 * then padded CopyImageToBuffer from dst; guards/padding must stay a5. */
#define _DEFAULT_SOURCE 1
#define VK_USE_PLATFORM_WAYLAND_KHR 1
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned char *load_file(const char *path, size_t *len) {
   FILE *f = fopen(path, "rb");
   if (!f) { perror(path); exit(1); }
   fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
   unsigned char *b = malloc(n); fread(b, 1, n, f); fclose(f); *len = n; return b;
}

static struct wl_display *dpy;
static struct wl_compositor *compositor;
static struct xdg_wm_base *wm_base;
static struct wl_surface *surface;
static struct xdg_surface *xdg_surf;
static struct xdg_toplevel *toplevel;

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v) {
   (void)d; (void)v;
   if (!strcmp(iface, "wl_compositor")) compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
   else if (!strcmp(iface, "xdg_wm_base")) wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_listener = {reg_global, reg_remove};
static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial) { (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_listener = {wm_ping};
static void xdg_configure(void *d, struct xdg_surface *s, uint32_t serial) { (void)d; xdg_surface_ack_configure(s, serial); }
static const struct xdg_surface_listener xdg_listener = {xdg_configure};
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) {
   (void)d; (void)t; (void)w; (void)h; (void)st;
}
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; }
static const struct xdg_toplevel_listener top_listener = {.configure=top_configure, .close=top_close};

#define CK(x) do { VkResult r_=(x); if(r_!=VK_SUCCESS){fprintf(stderr,"FAIL %s:%d %s = %d\n",__FILE__,__LINE__,#x,r_);exit(1);} } while(0)

int main(void) {
   const char *mode = getenv("LG110_TEXTURE_VARIANT");
   if (!mode || (strcmp(mode, "A") && strcmp(mode, "B"))) {
      fprintf(stderr, "set LG110_TEXTURE_VARIANT=A or B\n"); return 1;
   }
   int nonzero = !strcmp(mode, "B");
   const char *pixel_path = getenv("LG110_READBACK_PIXEL");
   if (!pixel_path) { fprintf(stderr, "set LG110_READBACK_PIXEL\n"); return 1; }
   printf("texture variant: %s red=%u other=0\n", mode, nonzero ? 255u : 0u);
   dpy = wl_display_connect(NULL);
   if (!dpy) { fprintf(stderr, "no wayland display\n"); return 1; }
   struct wl_registry *reg = wl_display_get_registry(dpy);
   wl_registry_add_listener(reg, &reg_listener, NULL);
   wl_display_roundtrip(dpy);
   if (!compositor || !wm_base) { fprintf(stderr, "missing globals\n"); return 1; }
   xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
   surface = wl_compositor_create_surface(compositor);
   xdg_surf = xdg_wm_base_get_xdg_surface(wm_base, surface);
   xdg_surface_add_listener(xdg_surf, &xdg_listener, NULL);
   toplevel = xdg_surface_get_toplevel(xdg_surf);
   xdg_toplevel_add_listener(toplevel, &top_listener, NULL);
   xdg_toplevel_set_title(toplevel, "vkgl gradient GPU CopyImage A/B");
   xdg_toplevel_set_fullscreen(toplevel, NULL);
   wl_surface_commit(surface);
   wl_display_roundtrip(dpy);

   VkApplicationInfo app = {.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName="vktest-clear", .apiVersion=VK_API_VERSION_1_0};
   const char *exts[] = {"VK_KHR_surface", "VK_KHR_wayland_surface"};
   VkInstanceCreateInfo ici = {.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo=&app,
                               .enabledExtensionCount=2, .ppEnabledExtensionNames=exts};
   VkInstance inst; CK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t n=0; CK(vkEnumeratePhysicalDevices(inst,&n,NULL));
   printf("physical devices: %u\n", n);
   if (!n) return 1;
   VkPhysicalDevice *gpus = calloc(n, sizeof(*gpus));
   CK(vkEnumeratePhysicalDevices(inst,&n,gpus));
   VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(gpus[0],&props);
   printf("device: %s\n", props.deviceName);

   VkWaylandSurfaceCreateInfoKHR sci = {.sType=VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR, .display=dpy, .surface=surface};
   VkSurfaceKHR surf; CK(vkCreateWaylandSurfaceKHR(inst,&sci,NULL,&surf));

   float prio=1.0f;
   VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueCount=1,.pQueuePriorities=&prio};
   const char *dexts[] = {"VK_KHR_swapchain"};
   VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci,
                           .enabledExtensionCount=1,.ppEnabledExtensionNames=dexts};
   VkDevice dev; CK(vkCreateDevice(gpus[0],&dci,NULL,&dev));
   VkQueue q; vkGetDeviceQueue(dev,0,0,&q);

   VkSurfaceCapabilitiesKHR caps; CK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpus[0],surf,&caps));
   uint32_t nf=0; CK(vkGetPhysicalDeviceSurfaceFormatsKHR(gpus[0],surf,&nf,NULL));
   VkSurfaceFormatKHR *fmts=calloc(nf,sizeof(*fmts)); CK(vkGetPhysicalDeviceSurfaceFormatsKHR(gpus[0],surf,&nf,fmts));
   printf("formats: %u first=%d\n", nf, fmts[0].format);
   VkExtent2D ext = {640, 480};
   VkSwapchainCreateInfoKHR swci={.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,.surface=surf,
      .minImageCount=caps.minImageCount,.imageFormat=fmts[0].format,.imageColorSpace=fmts[0].colorSpace,
      .imageExtent=ext,.imageArrayLayers=1,.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .imageSharingMode=VK_SHARING_MODE_EXCLUSIVE,.preTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,.presentMode=VK_PRESENT_MODE_FIFO_KHR,.clipped=VK_TRUE};
   VkSwapchainKHR sc; CK(vkCreateSwapchainKHR(dev,&swci,NULL,&sc));
   uint32_t ni=0; CK(vkGetSwapchainImagesKHR(dev,sc,&ni,NULL));
   printf("swapchain images: %u\n", ni);
   VkImage *imgs=calloc(ni,sizeof(*imgs)); CK(vkGetSwapchainImagesKHR(dev,sc,&ni,imgs));

   VkAttachmentDescription att={.format=fmts[0].format,.samples=VK_SAMPLE_COUNT_1_BIT,
      .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
   VkAttachmentReference ref={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sp={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&ref};
   VkRenderPassCreateInfo rpci={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=1,.pAttachments=&att,
      .subpassCount=1,.pSubpasses=&sp};
   VkRenderPass rp; CK(vkCreateRenderPass(dev,&rpci,NULL,&rp));

   VkFramebuffer *fbs=calloc(ni,sizeof(*fbs));
   for (uint32_t i=0;i<ni;i++){
      VkImageViewCreateInfo vci={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=imgs[i],
         .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=fmts[0].format,
         .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
      VkImageView v; CK(vkCreateImageView(dev,&vci,NULL,&v));
      VkFramebufferCreateInfo fci={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=rp,
         .attachmentCount=1,.pAttachments=&v,.width=ext.width,.height=ext.height,.layers=1};
      CK(vkCreateFramebuffer(dev,&fci,NULL,&fbs[i]));
   }

   size_t vsz=0, fsz=0;
   unsigned char *vs = load_file("grad.vert.spv", &vsz);
   unsigned char *fs = load_file("vktest-gradient-copy-image.frag.spv", &fsz);
   VkShaderModuleCreateInfo smci={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
   VkShaderModule vsm, fsm;
   smci.codeSize=vsz; smci.pCode=(const uint32_t*)vs; CK(vkCreateShaderModule(dev,&smci,NULL,&vsm));
   smci.codeSize=fsz; smci.pCode=(const uint32_t*)fs; CK(vkCreateShaderModule(dev,&smci,NULL,&fsm));
   VkPipelineShaderStageCreateInfo stages[2]={
      {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vsm,.pName="main"},
      {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fsm,.pName="main"}};
   VkPipelineVertexInputStateCreateInfo vici={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   VkPipelineInputAssemblyStateCreateInfo iaci={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkViewport vp={0,0,640,480,0,1};
   VkRect2D scissor={.offset={0,0},.extent={640,480}};
   VkPipelineViewportStateCreateInfo vsci={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.pViewports=&vp,.scissorCount=1,.pScissors=&scissor};
   VkPipelineRasterizationStateCreateInfo rsci={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1.0f};
   VkPipelineMultisampleStateCreateInfo msci={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
   VkPipelineColorBlendAttachmentState cba={.colorWriteMask=0xf};
   VkPipelineColorBlendStateCreateInfo cbci={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&cba};
   /* Match vktest-texture's binding 0, but initialize the layout before use. */
   VkDescriptorSetLayoutBinding dslb={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT};
   VkDescriptorSetLayoutCreateInfo dslci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&dslb};
   VkDescriptorSetLayout dsl;
   CK(vkCreateDescriptorSetLayout(dev,&dslci,NULL,&dsl));
   VkPipelineLayoutCreateInfo plci={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&dsl};
   VkPipelineLayout playout; CK(vkCreatePipelineLayout(dev,&plci,NULL,&playout));

   /* A is all zero; B changes only the red byte. Alpha is intentionally zero
    * in the source texture because the shader always outputs alpha 1. */
   VkImageCreateInfo tici={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
      .format=VK_FORMAT_R8G8B8A8_UNORM,.extent={64,64,1},.mipLevels=1,.arrayLayers=1,
      .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_LINEAR,.usage=VK_IMAGE_USAGE_SAMPLED_BIT,
      .sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
   VkImage teximg; CK(vkCreateImage(dev,&tici,NULL,&teximg));
   VkMemoryRequirements tmr; vkGetImageMemoryRequirements(dev,teximg,&tmr);
   VkMemoryAllocateInfo tma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=tmr.size,.memoryTypeIndex=0};
   VkDeviceMemory texmem; CK(vkAllocateMemory(dev,&tma,NULL,&texmem));
   CK(vkBindImageMemory(dev,teximg,texmem,0));
   void *tptr; CK(vkMapMemory(dev,texmem,0,tmr.size,0,&tptr));
   for (int y=0;y<64;y++) for (int x=0;x<64;x++) {
      unsigned char *p=(unsigned char*)tptr+(y*64+x)*4;
      p[0]=nonzero ? 255 : 0; p[1]=0; p[2]=0; p[3]=0;
   }
   VkImageViewCreateInfo tvci={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=teximg,
      .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
   VkImageView texview; CK(vkCreateImageView(dev,&tvci,NULL,&texview));
   VkSamplerCreateInfo tsci={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_NEAREST,.minFilter=VK_FILTER_NEAREST};
   VkSampler texsam; CK(vkCreateSampler(dev,&tsci,NULL,&texsam));
   VkDescriptorPoolSize dps={.type=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=1};
   VkDescriptorPoolCreateInfo dpci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&dps};
   VkDescriptorPool dp; CK(vkCreateDescriptorPool(dev,&dpci,NULL,&dp));
   VkDescriptorSetAllocateInfo dsai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=dp,.descriptorSetCount=1,.pSetLayouts=&dsl};
   VkDescriptorSet ds; CK(vkAllocateDescriptorSets(dev,&dsai,&ds));
   VkDescriptorImageInfo dii={.sampler=texsam,.imageView=texview,.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
   VkWriteDescriptorSet wds={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=ds,.dstBinding=0,.descriptorCount=1,
      .descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&dii};
   vkUpdateDescriptorSets(dev,1,&wds,0,NULL);
   VkGraphicsPipelineCreateInfo gpci={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=stages,
      .pVertexInputState=&vici,.pInputAssemblyState=&iaci,.pViewportState=&vsci,.pRasterizationState=&rsci,
      .pMultisampleState=&msci,.pColorBlendState=&cbci,.layout=playout,.renderPass=rp};
   VkPipeline pipe; CK(vkCreateGraphicsPipelines(dev,VK_NULL_HANDLE,1,&gpci,NULL,&pipe));

   VkImageCreateInfo dici={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
      .format=fmts[0].format,.extent={64,64,1},.mipLevels=1,.arrayLayers=1,
      .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
      .usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
   VkImage dstimg; CK(vkCreateImage(dev,&dici,NULL,&dstimg));
   VkMemoryRequirements dmr; vkGetImageMemoryRequirements(dev,dstimg,&dmr);
   VkMemoryAllocateInfo dma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=dmr.size,.memoryTypeIndex=0};
   VkDeviceMemory dstmem; CK(vkAllocateMemory(dev,&dma,NULL,&dstmem));
   CK(vkBindImageMemory(dev,dstimg,dstmem,0));
   printf("copy-image src=(320,239) dst=(7,9) extent=2x2 offscreen=64x64 format=%d\n",fmts[0].format);

   VkCommandPoolCreateInfo cpci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
   VkCommandPool cp; CK(vkCreateCommandPool(dev,&cpci,NULL,&cp));
   VkCommandBufferAllocateInfo cbai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cp,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
   VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev,&cbai,&cb));

   VkBufferCreateInfo rbci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size=32,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
   VkBuffer readback; CK(vkCreateBuffer(dev,&rbci,NULL,&readback));
   VkMemoryRequirements rmr; vkGetBufferMemoryRequirements(dev,readback,&rmr);
   VkMemoryAllocateInfo rma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=rmr.size,.memoryTypeIndex=0};
   VkDeviceMemory rbmem; CK(vkAllocateMemory(dev,&rma,NULL,&rbmem));
   CK(vkBindBufferMemory(dev,readback,rbmem,0));
   void *rinit; CK(vkMapMemory(dev,rbmem,0,32,0,&rinit));
   memset(rinit,0xa5,32); vkUnmapMemory(dev,rbmem);
   VkFenceCreateInfo fci={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence readback_fence; CK(vkCreateFence(dev,&fci,NULL,&readback_fence));

   const int expected_red=nonzero ? 64 : 0;
   unsigned char expected[32]; memset(expected,0xa5,sizeof(expected));
   for (int row=0;row<2;row++) for (int x=0;x<2;x++) {
      size_t pos=4+(size_t)row*16+(size_t)x*4;
      int green=(int)((240.5-row)/480.0*255.0+0.5);
      if (fmts[0].format==VK_FORMAT_B8G8R8A8_UNORM) {
         expected[pos]=0; expected[pos+1]=(unsigned char)green;
         expected[pos+2]=(unsigned char)expected_red; expected[pos+3]=255;
      } else if (fmts[0].format==VK_FORMAT_R8G8B8A8_UNORM) {
         expected[pos]=(unsigned char)expected_red; expected[pos+1]=(unsigned char)green;
         expected[pos+2]=0; expected[pos+3]=255;
      } else { fprintf(stderr,"unsupported readback format %d\n",fmts[0].format); return 1; }
   }
   unsigned char actual[32]; memset(actual,0xa5,sizeof(actual));
   int pixels_valid=0, readback_attempted=0;
   printf("readback dst_origin=(7,9) extent=2x2 row_length=4 buffer_offset=4 size=32 format=%d tolerance=2\n",
      fmts[0].format);
   printf("readback expected32="); for (int i=0;i<32;i++) printf("%02x",expected[i]); printf("\n");

   VkSemaphoreCreateInfo semci={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   VkSemaphore acquire, done; CK(vkCreateSemaphore(dev,&semci,NULL,&acquire)); CK(vkCreateSemaphore(dev,&semci,NULL,&done));

   for (int frame=0; frame<120; frame++) {
      uint32_t idx=0;
      CK(vkAcquireNextImageKHR(dev,sc,~0ull,acquire,VK_NULL_HANDLE,&idx));
      VkCommandBufferBeginInfo bbi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      CK(vkBeginCommandBuffer(cb,&bbi));
      VkClearValue clear={.color={{ (frame%60)/60.0f, 0.2f, 0.4f, 1.0f }}};
      VkRenderPassBeginInfo rbi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=rp,
         .framebuffer=fbs[idx],.renderArea={{0,0},ext},.clearValueCount=1,.pClearValues=&clear};
      vkCmdBeginRenderPass(cb,&rbi,VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe);
      vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,playout,0,1,&ds,0,NULL);
      vkCmdSetViewport(cb,0,1,&vp);
      vkCmdSetScissor(cb,0,1,&scissor);
      vkCmdDraw(cb,3,1,0,0);
      vkCmdEndRenderPass(cb);
      if (frame==0) {
         VkImageMemoryBarrier ib={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=imgs[idx],.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
         vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,0,NULL,0,NULL,1,&ib);
         VkImageMemoryBarrier db={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask=0,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=dstimg,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
         vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,0,NULL,0,NULL,1,&db);
         VkClearColorValue a5={.float32={165.0f/255.0f,165.0f/255.0f,165.0f/255.0f,165.0f/255.0f}};
         VkImageSubresourceRange dr={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
         vkCmdClearColorImage(cb,dstimg,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&a5,1,&dr);
         VkImageCopy image_copy={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
            .srcOffset={320,239,0},.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
            .dstOffset={7,9,0},.extent={2,2,1}};
         vkCmdCopyImage(cb,imgs[idx],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            dstimg,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&image_copy);
         db.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; db.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
         db.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; db.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
         vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,0,NULL,0,NULL,1,&db);
         VkBufferImageCopy copy={.bufferOffset=4,.bufferRowLength=4,.bufferImageHeight=0,
            .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageOffset={7,9,0},.imageExtent={2,2,1}};
         vkCmdCopyImageToBuffer(cb,dstimg,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback,1,&copy);
         ib.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT; ib.dstAccessMask=0;
         ib.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; ib.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
         vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0,0,NULL,0,NULL,1,&ib);
      }
      CK(vkEndCommandBuffer(cb));
      VkPipelineStageFlags wait=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.waitSemaphoreCount=1,.pWaitSemaphores=&acquire,
         .pWaitDstStageMask=&wait,.commandBufferCount=1,.pCommandBuffers=&cb,.signalSemaphoreCount=1,.pSignalSemaphores=&done};
      CK(vkQueueSubmit(q,1,&si,frame==0 ? readback_fence : VK_NULL_HANDLE));
      if (frame==0) {
         CK(vkWaitForFences(dev,1,&readback_fence,VK_TRUE,~0ull));
         readback_attempted=1;
         void *rptr; CK(vkMapMemory(dev,rbmem,0,32,0,&rptr));
         memcpy(actual,rptr,32); vkUnmapMemory(dev,rbmem);
         int matches=1, sentinel=1, guards_padding_ok=1;
         for (int i=0;i<32;i++) {
            int data=(i>=4 && i<12)||(i>=20 && i<28);
            int delta=(int)actual[i]-(int)expected[i]; if (delta<0) delta=-delta;
            if (data) { if (delta>2) matches=0; }
            else if (actual[i]!=0xa5) { matches=0; guards_padding_ok=0; }
            if (actual[i]!=0xa5) sentinel=0;
         }
         pixels_valid=matches ? 4 : 0;
         printf("readback actual32="); for (int i=0;i<32;i++) printf("%02x",actual[i]); printf("\n");
         printf("readback pixels_valid=%d guards_padding_ok=%d sentinel_unchanged=%d\n",
            pixels_valid,guards_padding_ok,sentinel);
         FILE *pixel_file=fopen(pixel_path,"wb");
         if (!pixel_file) { perror(pixel_path); return 1; }
         if (fwrite(actual,1,32,pixel_file)!=32) { perror("fwrite readback"); fclose(pixel_file); return 1; }
         fclose(pixel_file);
      }
      VkPresentInfoKHR pi={.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,.waitSemaphoreCount=1,.pWaitSemaphores=&done,
         .swapchainCount=1,.pSwapchains=&sc,.pImageIndices=&idx};
      CK(vkQueuePresentKHR(q,&pi));
      usleep(100000);
   }
   if (!readback_attempted || pixels_valid!=4) {
      printf("FAIL vktest-gradient-copy-image variant=%s pixels_valid=%d\n",mode,pixels_valid);
      return 1;
   }
   printf("PASS vktest-gradient-copy-image variant=%s pixels_valid=4\n",mode);
   return 0;
}
