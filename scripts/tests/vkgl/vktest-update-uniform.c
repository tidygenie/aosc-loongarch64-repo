/* Record a red vec4 uniform update, mutate the source, then restore blue
 * through the persistent host mapping before submit. The fragment shader must
 * consume red after submit-time UpdateBuffer; the record-time-only path is blue. */
#define _DEFAULT_SOURCE 1
#define VK_USE_PLATFORM_WAYLAND_KHR 1
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include <stdio.h>
#include <stdint.h>
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

static VkPrimitiveTopology topology_from_name(const char *name) {
   if (!strcmp(name, "point")) return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
   if (!strcmp(name, "line")) return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
   if (!strcmp(name, "line-strip")) return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
   if (!strcmp(name, "triangle")) return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
   if (!strcmp(name, "triangle-strip")) return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
   if (!strcmp(name, "triangle-fan")) return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
   if (!strcmp(name, "adjacency")) return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
   if (!strcmp(name, "patch")) return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
   return VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
}

int main(void) {
   const char *mode = getenv("LG110_TEXTURE_VARIANT");
   if (!mode || (strcmp(mode, "A") && strcmp(mode, "B"))) {
      fprintf(stderr, "set LG110_TEXTURE_VARIANT=A or B\n"); return 1;
   }
   int nonzero = !strcmp(mode, "B");
   const char *pixel_path = getenv("LG110_READBACK_PIXEL");
   if (!pixel_path) { fprintf(stderr, "set LG110_READBACK_PIXEL\n"); return 1; }
   const char *draw_text = getenv("LG110_DRAW_VERTEX_COUNT");
   const char *expected_draw_text = getenv("LG110_EXPECT_DRAW_COUNT");
   const char *first_text = getenv("LG110_DRAW_FIRST_VERTEX");
   const char *instance_text = getenv("LG110_DRAW_INSTANCE_COUNT");
   const char *first_instance_text = getenv("LG110_DRAW_FIRST_INSTANCE");
   const char *indexed_text = getenv("LG110_DRAW_INDEXED");
   const char *indirect_text = getenv("LG110_DRAW_INDIRECT");
   const char *index_type_text = getenv("LG110_INDEX_TYPE");
   const char *topology_text = getenv("LG110_DRAW_TOPOLOGY");
   const char *restart_text = getenv("LG110_PRIMITIVE_RESTART");
   const char *sentinel_text = getenv("LG110_RESTART_SENTINEL");
   const char *expected_clear_text = getenv("LG110_EXPECT_CLEAR");
   const char *index_mapped_text = getenv("LG110_INDEX_MAPPED");
   const char *update_restart_text = getenv("LG110_UPDATE_RESTART_SECOND_FRAME");
   const char *expected_topology_text = getenv("LG110_EXPECT_TOPOLOGY");
   const char *frame_text = getenv("LG110_FRAME_LIMIT");
   if (!draw_text) draw_text = "3";
   if (!expected_draw_text) expected_draw_text = draw_text;
   if (!first_text) first_text = "0";
   if (!instance_text) instance_text = "1";
   if (!first_instance_text) first_instance_text = "0";
   if (!indexed_text) indexed_text = "0";
   if (!indirect_text) indirect_text = "0";
   if (!index_type_text) index_type_text = "16";
   if (!topology_text) topology_text = "triangle";
   if (!restart_text) restart_text = "0";
   if (!sentinel_text) sentinel_text = "0";
   if (!index_mapped_text) index_mapped_text = "0";
   if (!update_restart_text) update_restart_text = "0";
   if (!expected_topology_text) expected_topology_text = topology_text;
   if (!frame_text) frame_text = "120";
   int expect_unsupported = getenv("LG110_EXPECT_UNSUPPORTED") &&
                            !strcmp(getenv("LG110_EXPECT_UNSUPPORTED"), "1");
   if ((strcmp(index_type_text, "16") && strcmp(index_type_text, "32")) ||
       (strcmp(restart_text, "0") && strcmp(restart_text, "1")) ||
       (strcmp(sentinel_text, "0") && strcmp(sentinel_text, "1")) ||
       (strcmp(index_mapped_text, "0") && strcmp(index_mapped_text, "1")) ||
       (strcmp(update_restart_text, "0") && strcmp(update_restart_text, "1")) ||
       (!strcmp(index_mapped_text, "1") && strcmp(indexed_text, "1")) ||
       (!strcmp(update_restart_text, "1") &&
        (strcmp(index_mapped_text, "1") || strcmp(sentinel_text, "1") ||
         strcmp(restart_text, "1") || strcmp(frame_text, "120"))) ||
       (expected_clear_text && strcmp(expected_clear_text, "0") && strcmp(expected_clear_text, "1")) ||
       (strcmp(indexed_text, "0") && strcmp(indexed_text, "1")) ||
       (strcmp(indirect_text, "0") && strcmp(indirect_text, "1")) ||
       (!strcmp(sentinel_text, "1") &&
        (!strcmp(indexed_text, "0") ||
         (strcmp(topology_text, "triangle-strip") && strcmp(topology_text, "triangle-fan")))) ||
       (strcmp(instance_text, "0") && strcmp(instance_text, "1") && strcmp(instance_text, "2")) ||
       (strcmp(first_instance_text, "0") && strcmp(first_instance_text, "1")) ||
       (strcmp(first_text, "0") && strcmp(first_text, "1")) ||
       (strcmp(draw_text, "3") && strcmp(draw_text, "2") &&
        (strcmp(draw_text, "7") || strcmp(sentinel_text, "1"))) ||
       (strcmp(expected_draw_text, "3") && strcmp(expected_draw_text, "2")) ||
       topology_from_name(topology_text) == VK_PRIMITIVE_TOPOLOGY_MAX_ENUM ||
       topology_from_name(expected_topology_text) == VK_PRIMITIVE_TOPOLOGY_MAX_ENUM ||
       (strcmp(frame_text, "120") && strcmp(frame_text, "1"))) {
      fprintf(stderr, "draw=3|2|7(restart) first=0|1 instances=0|1|2 first_instance=0|1 indexed=0|1 indirect=0|1 index_type=16|32 primitive_restart=0|1 sentinel=0|1 expect_clear=0|1 mapped=0|1 update_second=0|1 topology=... frames=120|1\n"); return 1;
   }
   int draw_count = draw_text[0] - '0';
   int first_vertex = first_text[0] - '0';
   int instance_count = instance_text[0] - '0';
   int first_instance = first_instance_text[0] - '0';
   int indexed = indexed_text[0] - '0';
   int indirect = indirect_text[0] - '0';
   int index_type = atoi(index_type_text);
   int primitive_restart = restart_text[0] == '1';
   int restart_sentinel = sentinel_text[0] == '1';
   int keep_index_mapped = index_mapped_text[0] == '1';
   int update_restart_second = update_restart_text[0] == '1';
   int expected_draw_count = expected_draw_text[0] - '0';
   VkPrimitiveTopology topology = topology_from_name(topology_text);
   VkPrimitiveTopology expected_topology = topology_from_name(expected_topology_text);
   int unsupported_topology = topology == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST ||
                              topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
   if (unsupported_topology != expect_unsupported ||
       (expect_unsupported && expected_topology != topology)) {
      fprintf(stderr, "adjacency/patch require LG110_EXPECT_UNSUPPORTED=1 and matching expectation\n");
      return 1;
   }
   int frame_limit = frame_text[0] == '1' && !frame_text[1] ? 1 : 120;
   if (indirect || indexed || draw_count != 3 || expected_draw_count != 3 ||
       instance_count != 1 || first_vertex != 0 || first_instance != 0 ||
       primitive_restart || restart_sentinel || keep_index_mapped ||
       update_restart_second || frame_limit != 1 ||
       topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) {
      fprintf(stderr,"update-uniform test requires direct nonindexed draw=3 instances=1 topology=triangle frames=1\n");
      return 1;
   }
   printf("texture variant: %s red=%u other=0 draw=%d first=%d instances=%d first_instance=%d indexed=%d indirect=%d index_type=%d primitive_restart=%d sentinel=%d mapped=%d update_second=%d expect_clear=%s expected_draw=%d topology=%s expected_topology=%s frames=%d\n",
          mode, nonzero ? 255u : 0u, draw_count, first_vertex, instance_count, first_instance, indexed, indirect, index_type, primitive_restart, restart_sentinel, keep_index_mapped, update_restart_second, expected_clear_text ? expected_clear_text : "auto", expected_draw_count,
          topology_text, expected_topology_text, frame_limit);
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
   xdg_toplevel_set_title(toplevel, "vkgl gradient sampling readback A/B");
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
   unsigned char *fs = load_file("update-uniform.frag.spv", &fsz);
   VkShaderModuleCreateInfo smci={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
   VkShaderModule vsm, fsm;
   smci.codeSize=vsz; smci.pCode=(const uint32_t*)vs; CK(vkCreateShaderModule(dev,&smci,NULL,&vsm));
   smci.codeSize=fsz; smci.pCode=(const uint32_t*)fs; CK(vkCreateShaderModule(dev,&smci,NULL,&fsm));
   VkPipelineShaderStageCreateInfo stages[2]={
      {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vsm,.pName="main"},
      {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fsm,.pName="main"}};
   VkPipelineVertexInputStateCreateInfo vici={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   VkPipelineInputAssemblyStateCreateInfo iaci={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=topology,.primitiveRestartEnable=(VkBool32)primitive_restart};
   VkViewport vp={0,0,640,480,0,1};
   VkRect2D scissor={.offset={0,0},.extent={640,240}};
   VkPipelineViewportStateCreateInfo vsci={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.pViewports=&vp,.scissorCount=1,.pScissors=&scissor};
   VkPipelineRasterizationStateCreateInfo rsci={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1.0f};
   VkPipelineMultisampleStateCreateInfo msci={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
   VkPipelineColorBlendAttachmentState cba={.colorWriteMask=0xf};
   VkPipelineColorBlendStateCreateInfo cbci={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&cba};
   /* Match vktest-texture's binding 0, but initialize the layout before use. */
   VkDescriptorSetLayoutBinding dslb={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT};
   VkDescriptorSetLayoutCreateInfo dslci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&dslb};
   VkDescriptorSetLayout dsl;
   CK(vkCreateDescriptorSetLayout(dev,&dslci,NULL,&dsl));
   VkPipelineLayoutCreateInfo plci={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&dsl};
   VkPipelineLayout playout; CK(vkCreatePipelineLayout(dev,&plci,NULL,&playout));

   VkBuffer uniform_buffer;
   VkBufferCreateInfo ubci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size=16,.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
   CK(vkCreateBuffer(dev,&ubci,NULL,&uniform_buffer));
   VkMemoryRequirements umr; vkGetBufferMemoryRequirements(dev,uniform_buffer,&umr);
   VkMemoryAllocateInfo uma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=umr.size,.memoryTypeIndex=0};
   VkDeviceMemory umem; CK(vkAllocateMemory(dev,&uma,NULL,&umem));
   CK(vkBindBufferMemory(dev,uniform_buffer,umem,0));
   void *uniform_mapped; CK(vkMapMemory(dev,umem,0,16,0,&uniform_mapped));
   const float initial_blue[4]={0,0,1,1};
   memcpy(uniform_mapped,initial_blue,sizeof(initial_blue));
   VkDescriptorPoolSize dps={.type=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.descriptorCount=1};
   VkDescriptorPoolCreateInfo dpci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&dps};
   VkDescriptorPool dp; CK(vkCreateDescriptorPool(dev,&dpci,NULL,&dp));
   VkDescriptorSetAllocateInfo dsai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=dp,.descriptorSetCount=1,.pSetLayouts=&dsl};
   VkDescriptorSet ds; CK(vkAllocateDescriptorSets(dev,&dsai,&ds));
   VkDescriptorBufferInfo dbi={.buffer=uniform_buffer,.offset=0,.range=16};
   VkWriteDescriptorSet wds={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=ds,.dstBinding=0,.descriptorCount=1,
      .descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.pBufferInfo=&dbi};
   vkUpdateDescriptorSets(dev,1,&wds,0,NULL);
   VkDynamicState dynamic_states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
   VkPipelineDynamicStateCreateInfo pdci={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount=2,.pDynamicStates=dynamic_states};
   VkGraphicsPipelineCreateInfo gpci={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=stages,
      .pVertexInputState=&vici,.pInputAssemblyState=&iaci,.pViewportState=&vsci,.pRasterizationState=&rsci,
      .pMultisampleState=&msci,.pColorBlendState=&cbci,.pDynamicState=&pdci,.layout=playout,.renderPass=rp};
   VkPipeline pipe;
   VkResult pipeline_result = vkCreateGraphicsPipelines(dev,VK_NULL_HANDLE,1,&gpci,NULL,&pipe);
   if (expect_unsupported) {
      if (pipeline_result == VK_ERROR_FEATURE_NOT_PRESENT) {
         printf("PASS unsupported-topology topology=%s result=%d\n", topology_text, pipeline_result);
         return 0;
      }
      fprintf(stderr, "FAIL unsupported topology=%s result=%d\n", topology_text, pipeline_result);
      return 1;
   }
   CK(pipeline_result);

   VkCommandPoolCreateInfo cpci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
   VkCommandPool cp; CK(vkCreateCommandPool(dev,&cpci,NULL,&cp));
   VkCommandBufferAllocateInfo cbai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cp,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
   VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev,&cbai,&cb));

   VkBufferCreateInfo rbci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size=8,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
   VkBuffer readback; CK(vkCreateBuffer(dev,&rbci,NULL,&readback));
   VkMemoryRequirements rmr; vkGetBufferMemoryRequirements(dev,readback,&rmr);
   VkMemoryAllocateInfo rma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=rmr.size,.memoryTypeIndex=0};
   VkDeviceMemory rbmem; CK(vkAllocateMemory(dev,&rma,NULL,&rbmem));
   CK(vkBindBufferMemory(dev,readback,rbmem,0));
   void *rinit; CK(vkMapMemory(dev,rbmem,0,8,0,&rinit));
   memset(rinit,0xa5,8); vkUnmapMemory(dev,rbmem);
    VkBuffer index_buffer=VK_NULL_HANDLE, indirect_buffer=VK_NULL_HANDLE;
    void *index_mapped = NULL;
    if (indexed) {
       size_t index_count = restart_sentinel ? 7U : 3U;
       VkDeviceSize index_bytes = (VkDeviceSize)index_count * (index_type == 32 ? 4U : 2U);
       VkBufferCreateInfo bci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
          .size=index_bytes,.usage=VK_BUFFER_USAGE_INDEX_BUFFER_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
       CK(vkCreateBuffer(dev,&bci,NULL,&index_buffer));
       VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev,index_buffer,&mr);
       VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=0};
       VkDeviceMemory mem; CK(vkAllocateMemory(dev,&ma,NULL,&mem));
       CK(vkBindBufferMemory(dev,index_buffer,mem,0));
       void *ptr; CK(vkMapMemory(dev,mem,0,index_bytes,0,&ptr));
       if (index_type==32) {
          uint32_t normal[3]={0,1,2};
          uint32_t with_restart[7]={0,1,2,UINT32_MAX,3,4,5};
          memcpy(ptr,restart_sentinel ? (const void *)with_restart : (const void *)normal,index_bytes);
       } else {
          uint16_t normal[3]={0,1,2};
          uint16_t with_restart[7]={0,1,2,UINT16_MAX,3,4,5};
          memcpy(ptr,restart_sentinel ? (const void *)with_restart : (const void *)normal,index_bytes);
       }
       if (keep_index_mapped) index_mapped = ptr;
       else vkUnmapMemory(dev,mem);
    }
    if (indirect) {
       VkDeviceSize size=indexed ? sizeof(VkDrawIndexedIndirectCommand) : sizeof(VkDrawIndirectCommand);
       VkBufferCreateInfo bci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
          .size=size,.usage=VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
       CK(vkCreateBuffer(dev,&bci,NULL,&indirect_buffer));
       VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev,indirect_buffer,&mr);
       VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=0};
       VkDeviceMemory mem; CK(vkAllocateMemory(dev,&ma,NULL,&mem));
       CK(vkBindBufferMemory(dev,indirect_buffer,mem,0));
       void *ptr; CK(vkMapMemory(dev,mem,0,size,0,&ptr));
       if (indexed) {
          VkDrawIndexedIndirectCommand command={.indexCount=(uint32_t)draw_count,
             .instanceCount=(uint32_t)instance_count,.firstIndex=0,.vertexOffset=0,
             .firstInstance=(uint32_t)first_instance};
          memcpy(ptr,&command,sizeof(command));
       } else {
          VkDrawIndirectCommand command={.vertexCount=2,
             .instanceCount=(uint32_t)instance_count,.firstVertex=0,
             .firstInstance=(uint32_t)first_instance};
          memcpy(ptr,&command,sizeof(command));
       }
       vkUnmapMemory(dev,mem);
    }
   VkFenceCreateInfo fci={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence readback_fence; CK(vkCreateFence(dev,&fci,NULL,&readback_fence));

   unsigned char expected[8]={0,0,0,255,0,0,0,255};
   if (fmts[0].format==VK_FORMAT_B8G8R8A8_UNORM) {
      expected[2]=255;
   } else if (fmts[0].format==VK_FORMAT_R8G8B8A8_UNORM) {
      expected[0]=255;
   } else { fprintf(stderr,"unsupported readback format %d\n",fmts[0].format); return 1; }
   unsigned char actual[8]={0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5};
   int pixels_valid=0, readback_attempted=0;
   printf("scissor top_half=(0,0,640,240) readback top=(320,120) bottom=(320,360) format=%d expected_top=%02x%02x%02x%02x expected_bottom=%02x%02x%02x%02x\n",
      fmts[0].format,expected[0],expected[1],expected[2],expected[3],
      expected[4],expected[5],expected[6],expected[7]);

   VkSemaphoreCreateInfo semci={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   VkSemaphore acquire, done; CK(vkCreateSemaphore(dev,&semci,NULL,&acquire)); CK(vkCreateSemaphore(dev,&semci,NULL,&done));

   int readback_frame = update_restart_second ? 1 : 0;
   for (int frame=0; frame<frame_limit; frame++) {
      uint32_t idx=0;
      CK(vkAcquireNextImageKHR(dev,sc,~0ull,acquire,VK_NULL_HANDLE,&idx));
      if (update_restart_second && frame == 1) {
         if (index_type == 32) ((uint32_t *)index_mapped)[3] = 3;
         else ((uint16_t *)index_mapped)[3] = 3;
      }
      VkCommandBufferBeginInfo bbi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      CK(vkBeginCommandBuffer(cb,&bbi));
      float update_color[4]={1,0,0,1};
      vkCmdUpdateBuffer(cb,uniform_buffer,0,sizeof(update_color),update_color);
      update_color[0]=0;
      update_color[2]=1;
      VkClearValue clear={.color={{0.0f, 0.0f, 0.0f, 1.0f}}};
      VkRenderPassBeginInfo rbi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=rp,
         .framebuffer=fbs[idx],.renderArea={{0,0},ext},.clearValueCount=1,.pClearValues=&clear};
      vkCmdBeginRenderPass(cb,&rbi,VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe);
      vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,playout,0,1,&ds,0,NULL);
      vkCmdSetViewport(cb,0,1,&vp);
      vkCmdSetScissor(cb,0,1,&scissor);
      if (indexed) vkCmdBindIndexBuffer(cb,index_buffer,0,index_type==32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
      if (indirect && indexed)
         vkCmdDrawIndexedIndirect(cb,indirect_buffer,0,1,sizeof(VkDrawIndexedIndirectCommand));
      else if (indirect)
         vkCmdDrawIndirect(cb,indirect_buffer,0,1,sizeof(VkDrawIndirectCommand));
      else if (indexed)
         vkCmdDrawIndexed(cb,(uint32_t)draw_count,(uint32_t)instance_count,0,0,(uint32_t)first_instance);
      else
         vkCmdDraw(cb,(uint32_t)draw_count,(uint32_t)instance_count,(uint32_t)first_vertex,(uint32_t)first_instance);
      vkCmdEndRenderPass(cb);
      if (frame==readback_frame) {
         VkImageMemoryBarrier ib={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=imgs[idx],.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
         vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,0,NULL,0,NULL,1,&ib);
         VkBufferImageCopy copies[2]={
            {.bufferOffset=0,.bufferRowLength=0,.bufferImageHeight=0,
             .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageOffset={320,120,0},.imageExtent={1,1,1}},
            {.bufferOffset=4,.bufferRowLength=0,.bufferImageHeight=0,
             .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageOffset={320,360,0},.imageExtent={1,1,1}}};
         vkCmdCopyImageToBuffer(cb,imgs[idx],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback,2,copies);
         ib.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT; ib.dstAccessMask=0;
         ib.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; ib.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
         vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0,0,NULL,0,NULL,1,&ib);
      }
      CK(vkEndCommandBuffer(cb));
      const float host_blue[4]={0,0,1,1};
      memcpy(uniform_mapped,&host_blue,sizeof(host_blue));
      VkPipelineStageFlags wait=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.waitSemaphoreCount=1,.pWaitSemaphores=&acquire,
         .pWaitDstStageMask=&wait,.commandBufferCount=1,.pCommandBuffers=&cb,.signalSemaphoreCount=1,.pSignalSemaphores=&done};
      CK(vkQueueSubmit(q,1,&si,frame==readback_frame ? readback_fence : VK_NULL_HANDLE));
      if (frame==readback_frame) {
         CK(vkWaitForFences(dev,1,&readback_fence,VK_TRUE,~0ull));
         readback_attempted=1;
         void *rptr; CK(vkMapMemory(dev,rbmem,0,8,0,&rptr));
         memcpy(actual,rptr,8); vkUnmapMemory(dev,rbmem);
         pixels_valid=memcmp(actual,expected,8)==0;
         printf("readback top=%02x%02x%02x%02x bottom=%02x%02x%02x%02x pixels_valid=%d exact=1\n",
            actual[0],actual[1],actual[2],actual[3],
            actual[4],actual[5],actual[6],actual[7],pixels_valid);
         FILE *pixel_file=fopen(pixel_path,"wb");
         if (!pixel_file) { perror(pixel_path); return 1; }
         if (fwrite(actual,1,8,pixel_file)!=8) { perror("fwrite readback"); fclose(pixel_file); return 1; }
         fclose(pixel_file);
      }
      VkPresentInfoKHR pi={.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,.waitSemaphoreCount=1,.pWaitSemaphores=&done,
         .swapchainCount=1,.pSwapchains=&sc,.pImageIndices=&idx};
      CK(vkQueuePresentKHR(q,&pi));
      usleep(100000);
   }
   if (!readback_attempted || !pixels_valid) {
      printf("FAIL vktest-update-uniform variant=%s pixels_valid=0\n",mode);
      return 1;
   }
   printf("PASS vktest-update-uniform variant=%s pixels_valid=1\n",mode);
   return 0;
}
