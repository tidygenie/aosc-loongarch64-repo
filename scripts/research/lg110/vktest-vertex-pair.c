/* Same-process A/B/A vertex-state differential with per-frame readback.
 * LG110_PAIR_MODE=same is the noise control, offset alternates binding offset
 * 0/8/0, stride alternates pipeline UV stride 8/16/8, and pipeline alternates
 * two otherwise identical stride=16 pipelines. Use FRAME_LIMIT=3.
 * Cyclic positions and constant {0,0.5} UV pairs keep default center pixels
 * equal. LG110_ATTRIBUTE_SENSITIVE=1 instead uses distinct UV pairs and checks
 * A-B-A-distinct for stride or A-A-A for same/pipeline with a direct-color
 * fragment shader selected by LG110_FRAGMENT_SHADER.
 * The test writes Vulkan commands only; GPU BO inspection belongs to the
 * separately opt-in, read-only tracer. */
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

static int topology_clear_at_center(VkPrimitiveTopology topology) {
   return topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST ||
          topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST ||
          topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
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
   const char *first_index_text = getenv("LG110_FIRST_INDEX");
   const char *vertex_offset_text = getenv("LG110_VERTEX_OFFSET");
   const char *topology_text = getenv("LG110_DRAW_TOPOLOGY");
   const char *expected_topology_text = getenv("LG110_EXPECT_TOPOLOGY");
   const char *frame_text = getenv("LG110_FRAME_LIMIT");
   const char *position_offset_text = getenv("LG110_POSITION_OFFSET");
   const char *uv_stride_text = getenv("LG110_UV_STRIDE");
   const char *pair_text = getenv("LG110_PAIR_MODE");
   if (!draw_text) draw_text = "3";
   if (!expected_draw_text) expected_draw_text = draw_text;
   if (!first_text) first_text = "0";
   if (!instance_text) instance_text = "1";
   if (!first_instance_text) first_instance_text = "0";
   if (!indexed_text) indexed_text = "0";
   if (!indirect_text) indirect_text = "0";
   if (!index_type_text) index_type_text = "16";
   if (!first_index_text) first_index_text = "0";
   if (!vertex_offset_text) vertex_offset_text = "0";
   if (!topology_text) topology_text = "triangle";
   if (!expected_topology_text) expected_topology_text = topology_text;
   if (!frame_text) frame_text = "120";
   if (!position_offset_text) position_offset_text = "0";
   if (!uv_stride_text) uv_stride_text = "8";
   if (!pair_text) pair_text = "";
   int pair_offset = !strcmp(pair_text, "offset");
   int pair_stride = !strcmp(pair_text, "stride");
   int pair_pipeline = !strcmp(pair_text, "pipeline");
   int pair_same = !strcmp(pair_text, "same");
   int pair_mode = pair_offset || pair_stride || pair_pipeline || pair_same;
   int attribute_sensitive = getenv("LG110_ATTRIBUTE_SENSITIVE") &&
                             !strcmp(getenv("LG110_ATTRIBUTE_SENSITIVE"), "1");
   if (*pair_text && !pair_mode) {
      fprintf(stderr, "LG110_PAIR_MODE=offset|stride|pipeline|same\n"); return 1;
   }
   int expect_unsupported = getenv("LG110_EXPECT_UNSUPPORTED") &&
                            !strcmp(getenv("LG110_EXPECT_UNSUPPORTED"), "1");
   if ((strcmp(position_offset_text, "0") && strcmp(position_offset_text, "8")) ||
       (strcmp(uv_stride_text, "8") && strcmp(uv_stride_text, "16")) ||
       (strcmp(first_index_text, "0") && strcmp(first_index_text, "3")) ||
       (strcmp(vertex_offset_text, "-1") && strcmp(vertex_offset_text, "0") && strcmp(vertex_offset_text, "1")) ||
       (strcmp(index_type_text, "16") && strcmp(index_type_text, "32")) ||
       (strcmp(indexed_text, "0") && strcmp(indexed_text, "1")) ||
       (strcmp(indirect_text, "0") && strcmp(indirect_text, "1")) ||
       (strcmp(instance_text, "0") && strcmp(instance_text, "1") && strcmp(instance_text, "2")) ||
       (strcmp(first_instance_text, "0") && strcmp(first_instance_text, "1")) ||
       (strcmp(first_text, "0") && strcmp(first_text, "1")) ||
       (strcmp(draw_text, "3") && strcmp(draw_text, "2")) ||
       (strcmp(expected_draw_text, "3") && strcmp(expected_draw_text, "2")) ||
       topology_from_name(topology_text) == VK_PRIMITIVE_TOPOLOGY_MAX_ENUM ||
       topology_from_name(expected_topology_text) == VK_PRIMITIVE_TOPOLOGY_MAX_ENUM ||
       (strcmp(frame_text, "120") && strcmp(frame_text, "1") && strcmp(frame_text, "2") && strcmp(frame_text, "3")) ||
       (pair_mode && strcmp(frame_text, "2") && strcmp(frame_text, "3")) ||
       (!pair_mode && (!strcmp(frame_text, "2") || !strcmp(frame_text, "3"))) ||
       (pair_offset && strcmp(position_offset_text, "0")) ||
       (pair_stride && strcmp(uv_stride_text, "8")) ||
       (pair_pipeline && strcmp(uv_stride_text, "16")) ||
       (attribute_sensitive && (!pair_mode || pair_offset || strcmp(frame_text, "3")))) {
      fprintf(stderr, "draw=3|2 first=0|1 instances=0|1|2 first_instance=0|1 indexed=0|1 indirect=0|1 index_type=16|32 topology=... frames=120|1 pair=2|3\n"); return 1;
   }
   int position_offset = atoi(position_offset_text);
   int uv_stride = atoi(uv_stride_text);
   int draw_count = draw_text[0] - '0';
   int first_vertex = first_text[0] - '0';
   int instance_count = instance_text[0] - '0';
   int first_instance = first_instance_text[0] - '0';
   int indexed = indexed_text[0] - '0';
   int indirect = indirect_text[0] - '0';
   int index_type = atoi(index_type_text);
   int first_index = atoi(first_index_text);
   int vertex_offset = atoi(vertex_offset_text);
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
   int frame_limit = !strcmp(frame_text, "1") ? 1 : !strcmp(frame_text, "2") ? 2 : !strcmp(frame_text, "3") ? 3 : 120;
   printf("texture variant: %s red=%u other=0 draw=%d first=%d instances=%d first_instance=%d indexed=%d indirect=%d index_type=%d first_index=%d vertex_offset=%d expected_draw=%d topology=%s expected_topology=%s frames=%d\n",
          mode, nonzero ? 255u : 0u, draw_count, first_vertex, instance_count, first_instance, indexed, indirect, index_type, first_index, vertex_offset, expected_draw_count,
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
   const char *fragment_path = getenv("LG110_FRAGMENT_SHADER");
   if (!fragment_path) fragment_path = "vktest-gradient-sample-readback.frag.spv";
   unsigned char *fs = load_file(fragment_path, &fsz);
   VkShaderModuleCreateInfo smci={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
   VkShaderModule vsm, fsm;
   smci.codeSize=vsz; smci.pCode=(const uint32_t*)vs; CK(vkCreateShaderModule(dev,&smci,NULL,&vsm));
   smci.codeSize=fsz; smci.pCode=(const uint32_t*)fs; CK(vkCreateShaderModule(dev,&smci,NULL,&fsm));
   VkPipelineShaderStageCreateInfo stages[2]={
      {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vsm,.pName="main"},
      {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fsm,.pName="main"}};
   VkVertexInputBindingDescription vibindings[2]={
      {.binding=0,.stride=8,.inputRate=VK_VERTEX_INPUT_RATE_VERTEX},
      {.binding=1,.stride=(uint32_t)uv_stride,.inputRate=VK_VERTEX_INPUT_RATE_VERTEX}};
   VkVertexInputAttributeDescription viattributes[2]={
      {.location=0,.binding=0,.format=VK_FORMAT_R32G32_SFLOAT,.offset=0},
      {.location=1,.binding=1,.format=VK_FORMAT_R32G32_SFLOAT,.offset=0}};
   VkPipelineVertexInputStateCreateInfo vici={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount=2,.pVertexBindingDescriptions=vibindings,
      .vertexAttributeDescriptionCount=2,.pVertexAttributeDescriptions=viattributes};
   VkPipelineInputAssemblyStateCreateInfo iaci={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=topology};
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
   VkDynamicState dynamic_states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
   VkPipelineDynamicStateCreateInfo pdci={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount=2,.pDynamicStates=dynamic_states};
   VkGraphicsPipelineCreateInfo gpci={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=stages,
      .pVertexInputState=&vici,.pInputAssemblyState=&iaci,.pViewportState=&vsci,.pRasterizationState=&rsci,
      .pMultisampleState=&msci,.pColorBlendState=&cbci,.pDynamicState=&pdci,.layout=playout,.renderPass=rp};
   VkPipeline pipes[2];
   VkResult pipeline_result = vkCreateGraphicsPipelines(dev,VK_NULL_HANDLE,1,&gpci,NULL,&pipes[0]);
   if (expect_unsupported) {
      if (pipeline_result == VK_ERROR_FEATURE_NOT_PRESENT) {
         printf("PASS unsupported-topology topology=%s result=%d\n", topology_text, pipeline_result);
         return 0;
      }
      fprintf(stderr, "FAIL unsupported topology=%s result=%d\n", topology_text, pipeline_result);
      return 1;
   }
   CK(pipeline_result);
   pipes[1] = pipes[0];
   if (pair_stride || pair_pipeline) {
      if (pair_stride) vibindings[1].stride = 16;
      CK(vkCreateGraphicsPipelines(dev,VK_NULL_HANDLE,1,&gpci,NULL,&pipes[1]));
   }

   VkCommandPoolCreateInfo cpci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
   VkCommandPool cp; CK(vkCreateCommandPool(dev,&cpci,NULL,&cp));
   VkCommandBufferAllocateInfo cbai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cp,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
   VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev,&cbai,&cb));

   VkBufferCreateInfo rbci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size=4,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
   VkBuffer readback; CK(vkCreateBuffer(dev,&rbci,NULL,&readback));
   VkMemoryRequirements rmr; vkGetBufferMemoryRequirements(dev,readback,&rmr);
   VkMemoryAllocateInfo rma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=rmr.size,.memoryTypeIndex=0};
   VkDeviceMemory rbmem; CK(vkAllocateMemory(dev,&rma,NULL,&rbmem));
   CK(vkBindBufferMemory(dev,readback,rbmem,0));
   void *rinit; CK(vkMapMemory(dev,rbmem,0,4,0,&rinit));
   memset(rinit,0xa5,4); vkUnmapMemory(dev,rbmem);
    VkBuffer vertex_buffers[2];
    const VkDeviceSize vertex_sizes[2]={40,64};
    for (int binding=0;binding<2;binding++) {
       VkBufferCreateInfo bci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
          .size=vertex_sizes[binding],.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
       CK(vkCreateBuffer(dev,&bci,NULL,&vertex_buffers[binding]));
       VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev,vertex_buffers[binding],&mr);
       VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=0};
       VkDeviceMemory mem; CK(vkAllocateMemory(dev,&ma,NULL,&mem));
       CK(vkBindBufferMemory(dev,vertex_buffers[binding],mem,0));
       void *ptr; CK(vkMapMemory(dev,mem,0,vertex_sizes[binding],0,&ptr));
       if (binding==0) {
          const float positions[10]={-1,-1, 3,-1, -1,3, -1,-1, 3,-1};
          memcpy(ptr,positions,sizeof(positions));
       } else if (attribute_sensitive) {
          const float uv_stride[16]={0,0, 1,0, 0,1, 1,1,
                                     0.25f,0.75f, 0.8f,0.1f, 0.1f,0.9f, 0.6f,0.4f};
          memcpy(ptr,uv_stride,sizeof(uv_stride));
       } else {
          const float uv_stride[16]={0,0.5, 0,0.5, 0,0.5, 0,0.5, 0,0.5, 0,0.5, 0,0.5, 0,0.5};
          memcpy(ptr,uv_stride,sizeof(uv_stride));
       }
       vkUnmapMemory(dev,mem);
    }
    VkBuffer index_buffer=VK_NULL_HANDLE, indirect_buffer=VK_NULL_HANDLE;
    if (indexed) {
       VkBufferCreateInfo bci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
          .size=(VkDeviceSize)(index_type==32 ? 24 : 12),.usage=VK_BUFFER_USAGE_INDEX_BUFFER_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
       CK(vkCreateBuffer(dev,&bci,NULL,&index_buffer));
       VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev,index_buffer,&mr);
       VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=0};
       VkDeviceMemory mem; CK(vkAllocateMemory(dev,&ma,NULL,&mem));
       CK(vkBindBufferMemory(dev,index_buffer,mem,0));
       void *ptr; CK(vkMapMemory(dev,mem,0,index_type==32 ? 24 : 12,0,&ptr));
       if (index_type==32) { uint32_t indices[6]={0,1,2,1,2,3}; memcpy(ptr,indices,sizeof(indices)); }
       else { uint16_t indices[6]={0,1,2,1,2,3}; memcpy(ptr,indices,sizeof(indices)); }
       vkUnmapMemory(dev,mem);
    }
    if (indirect) {
       VkDeviceSize size=indexed ? sizeof(VkDrawIndexedIndirectCommand) : sizeof(VkDrawIndirectCommand);
       VkBufferCreateInfo bci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
          .size=size,.usage=VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
       CK(vkCreateBuffer(dev,&bci,NULL,&indirect_buffer));
       VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev,indirect_buffer,&mr);
       VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=0};
       VkDeviceMemory mem; CK(vkAllocateMemory(dev,&ma,NULL,&mem));
       CK(vkBindBufferMemory(dev,indirect_buffer,mem,0));
       void *ptr; CK(vkMapMemory(dev,mem,0,size,0,&ptr));
       if (indexed) {
          VkDrawIndexedIndirectCommand command={.indexCount=(uint32_t)draw_count,
             .instanceCount=(uint32_t)instance_count,.firstIndex=(uint32_t)first_index,.vertexOffset=(int32_t)vertex_offset,
             .firstInstance=(uint32_t)first_instance};
          memcpy(ptr,&command,sizeof(command));
       } else {
          VkDrawIndirectCommand command={.vertexCount=(uint32_t)draw_count,
             .instanceCount=(uint32_t)instance_count,.firstVertex=0,
             .firstInstance=(uint32_t)first_instance};
          memcpy(ptr,&command,sizeof(command));
       }
       vkUnmapMemory(dev,mem);
    }
   VkFenceCreateInfo fci={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence readback_fence; CK(vkCreateFence(dev,&fci,NULL,&readback_fence));

   const int expected_green=(int)(240.5/480.0*255.0+0.5);
   const int expected_red=nonzero ? 64 : 0;
   unsigned char expected[4];
   if (fmts[0].format==VK_FORMAT_B8G8R8A8_UNORM) {
      expected[0]=0; expected[1]=(unsigned char)expected_green;
      expected[2]=(unsigned char)expected_red; expected[3]=255;
   } else if (fmts[0].format==VK_FORMAT_R8G8B8A8_UNORM) {
      expected[0]=(unsigned char)expected_red; expected[1]=(unsigned char)expected_green;
      expected[2]=0; expected[3]=255;
   } else { fprintf(stderr,"unsupported readback format %d\n",fmts[0].format); return 1; }
   if (expected_draw_count == 2 || topology_clear_at_center(expected_topology)) {
      if (fmts[0].format==VK_FORMAT_B8G8R8A8_UNORM) {
         expected[0]=102; expected[1]=51; expected[2]=0; expected[3]=255;
      } else {
         expected[0]=0; expected[1]=51; expected[2]=102; expected[3]=255;
      }
   }
   unsigned char actual[4]={0xa5,0xa5,0xa5,0xa5};
   unsigned char pair_pixels[3][4]={{0}};
   int pixels_valid=0, all_pixels_valid=1, readback_attempted=0;
   if (attribute_sensitive)
      printf("attribute_sensitive=1 pair=%s expected_pattern=%s\n",
             pair_text, pair_stride ? "A-B-A-distinct" : "A-A-A");
   else
      printf("readback center=(320,240) format=%d tolerance=2 expected_bytes=%02x%02x%02x%02x\n",
         fmts[0].format,expected[0],expected[1],expected[2],expected[3]);

   VkSemaphoreCreateInfo semci={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   VkSemaphore acquire, done; CK(vkCreateSemaphore(dev,&semci,NULL,&acquire)); CK(vkCreateSemaphore(dev,&semci,NULL,&done));

   for (int frame=0; frame<frame_limit; frame++) {
      int readback_frame = frame == 0 || pair_mode;
      uint32_t idx=0;
      CK(vkAcquireNextImageKHR(dev,sc,~0ull,acquire,VK_NULL_HANDLE,&idx));
      VkCommandBufferBeginInfo bbi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      CK(vkBeginCommandBuffer(cb,&bbi));
      if (frame==0) {
         VkImageMemoryBarrier texture_barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask=0,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT,
            .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=teximg,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
         vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0,0,NULL,0,NULL,1,&texture_barrier);
      }
      VkClearValue clear={.color={{ (frame%60)/60.0f, 0.2f, 0.4f, 1.0f }}};
      VkRenderPassBeginInfo rbi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=rp,
         .framebuffer=fbs[idx],.renderArea={{0,0},ext},.clearValueCount=1,.pClearValues=&clear};
      vkCmdBeginRenderPass(cb,&rbi,VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipes[frame % 2]);
      vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,playout,0,1,&ds,0,NULL);
      vkCmdSetViewport(cb,0,1,&vp);
      vkCmdSetScissor(cb,0,1,&scissor);
      const VkDeviceSize vertex_offsets[2]={(VkDeviceSize)(pair_offset ? (frame % 2) * 8 : position_offset),0};
      vkCmdBindVertexBuffers(cb,0,2,vertex_buffers,vertex_offsets);
      if (indexed) vkCmdBindIndexBuffer(cb,index_buffer,0,index_type==32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
      if (indirect && indexed)
         vkCmdDrawIndexedIndirect(cb,indirect_buffer,0,1,sizeof(VkDrawIndexedIndirectCommand));
      else if (indirect)
         vkCmdDrawIndirect(cb,indirect_buffer,0,1,sizeof(VkDrawIndirectCommand));
      else if (indexed)
         vkCmdDrawIndexed(cb,(uint32_t)draw_count,(uint32_t)instance_count,(uint32_t)first_index,(int32_t)vertex_offset,(uint32_t)first_instance);
      else
         vkCmdDraw(cb,(uint32_t)draw_count,(uint32_t)instance_count,(uint32_t)first_vertex,(uint32_t)first_instance);
      vkCmdEndRenderPass(cb);
      if (readback_frame) {
         VkImageMemoryBarrier ib={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=imgs[idx],.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
         vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,0,NULL,0,NULL,1,&ib);
         VkBufferImageCopy copy={.bufferOffset=0,.bufferRowLength=0,.bufferImageHeight=0,
            .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageOffset={320,240,0},.imageExtent={1,1,1}};
         vkCmdCopyImageToBuffer(cb,imgs[idx],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback,1,&copy);
         ib.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT; ib.dstAccessMask=0;
         ib.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; ib.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
         vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0,0,NULL,0,NULL,1,&ib);
      }
      CK(vkEndCommandBuffer(cb));
      VkPipelineStageFlags wait=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.waitSemaphoreCount=1,.pWaitSemaphores=&acquire,
         .pWaitDstStageMask=&wait,.commandBufferCount=1,.pCommandBuffers=&cb,.signalSemaphoreCount=1,.pSignalSemaphores=&done};
      if (readback_frame && frame > 0) CK(vkResetFences(dev,1,&readback_fence));
      CK(vkQueueSubmit(q,1,&si,readback_frame ? readback_fence : VK_NULL_HANDLE));
      if (readback_frame) {
         CK(vkWaitForFences(dev,1,&readback_fence,VK_TRUE,~0ull));
         readback_attempted=1;
         void *rptr; CK(vkMapMemory(dev,rbmem,0,4,0,&rptr));
         memset(actual,0xa5,sizeof(actual));
         memcpy(actual,rptr,4); vkUnmapMemory(dev,rbmem);
         int matches=1, sentinel=1;
         if (pair_mode) memcpy(pair_pixels[frame],actual,4);
         for (int c=0;c<4;c++) {
            int delta=(int)actual[c]-(int)expected[c]; if (delta<0) delta=-delta;
            if (!attribute_sensitive && delta>2) matches=0;
            if (actual[c]!=0xa5) sentinel=0;
         }
         pixels_valid=attribute_sensitive ? !sentinel : matches;
         if (!pixels_valid) all_pixels_valid=0;
         printf("readback frame=%d actual_bytes=%02x%02x%02x%02x pixels_valid=%d sentinel_unchanged=%d\n",
            frame,actual[0],actual[1],actual[2],actual[3],pixels_valid,sentinel);
         char frame_pixel_path[4096];
         if (pair_mode && snprintf(frame_pixel_path,sizeof(frame_pixel_path),"%s.frame%d",pixel_path,frame) >= (int)sizeof(frame_pixel_path)) return 1;
         FILE *pixel_file=fopen(pair_mode ? frame_pixel_path : pixel_path,"wb");
         if (!pixel_file) { perror(pixel_path); return 1; }
         if (fwrite(actual,1,4,pixel_file)!=4) { perror("fwrite readback"); fclose(pixel_file); return 1; }
         fclose(pixel_file);
      }
      VkPresentInfoKHR pi={.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,.waitSemaphoreCount=1,.pWaitSemaphores=&done,
         .swapchainCount=1,.pSwapchains=&sc,.pImageIndices=&idx};
      CK(vkQueuePresentKHR(q,&pi));
      usleep(100000);
   }
   if (attribute_sensitive) {
      int pattern_valid;
      if (pair_stride)
         pattern_valid = !memcmp(pair_pixels[0],pair_pixels[2],4) &&
                         memcmp(pair_pixels[0],pair_pixels[1],4) != 0;
      else
         pattern_valid = !memcmp(pair_pixels[0],pair_pixels[1],4) &&
                         !memcmp(pair_pixels[0],pair_pixels[2],4);
      printf("attribute_sensitive pattern_valid=%d\n",pattern_valid);
      pixels_valid=pattern_valid;
      if (!pattern_valid) all_pixels_valid=0;
   }
   if (!readback_attempted || !pixels_valid || !all_pixels_valid) {
      printf("FAIL vktest-draw-count-readback variant=%s pixels_valid=0\n",mode);
      return 1;
   }
   printf("PASS vktest-vertex-state variant=%s pair=%s pixels_valid=1\n",mode,pair_text);
   return 0;
}
