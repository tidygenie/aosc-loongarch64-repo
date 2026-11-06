#define _POSIX_C_SOURCE 200809L
/* Headless optimal-tiling RGBA clear/readback control for LG110 alpha research.
 * LG110_OFFSCREEN_ALPHA=1.0|0.5 selects a passive ClearColorImage value.
 * LG110_READBACK_PIXEL is a mandatory four-byte raw output path.
 * LG110_EXPECT_OFFSCREEN_ALPHA can differ from the input only for an explicitly
 * paired active IB mutation. No mutation is performed by this test itself.
 */
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CK(call) do { \
    VkResult r_ = (call); \
    if (r_ != VK_SUCCESS) { \
        fprintf(stderr, "FAIL %s:%d %s=%d\n", __FILE__, __LINE__, #call, r_); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

typedef struct { VkInstance instance; VkPhysicalDevice gpu; VkDevice device; VkQueue queue; VkCommandPool pool; VkCommandBuffer cb; } Context;
typedef struct { VkBuffer handle; VkDeviceMemory memory; } Buffer;
typedef struct { VkImage handle; VkDeviceMemory memory; } Image;

static Context create_context(void) {
    Context c = {0};
    VkApplicationInfo app = {.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="lg110-offscreen-alpha",.apiVersion=VK_API_VERSION_1_0};
    VkInstanceCreateInfo ici = {.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    CK(vkCreateInstance(&ici,NULL,&c.instance));
    uint32_t count=0; CK(vkEnumeratePhysicalDevices(c.instance,&count,NULL));
    if (!count) { fprintf(stderr,"no Vulkan devices\n"); exit(EXIT_FAILURE); }
    VkPhysicalDevice *gpus=calloc(count,sizeof(*gpus)); if (!gpus) exit(EXIT_FAILURE);
    CK(vkEnumeratePhysicalDevices(c.instance,&count,gpus));
    for (uint32_t i=0;i<count;i++) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(gpus[i],&p);
        printf("GPU%u type=%d name=%s\n",i,p.deviceType,p.deviceName);
        if (strstr(p.deviceName,"vkgl")) c.gpu=gpus[i];
    }
    free(gpus);
    if (!c.gpu) { fprintf(stderr,"vkgl device not found\n"); exit(EXIT_FAILURE); }
    float priority=1.0f;
    VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci};
    CK(vkCreateDevice(c.gpu,&dci,NULL,&c.device)); vkGetDeviceQueue(c.device,0,0,&c.queue);
    VkCommandPoolCreateInfo pci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    CK(vkCreateCommandPool(c.device,&pci,NULL,&c.pool));
    VkCommandBufferAllocateInfo cbai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=c.pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    CK(vkAllocateCommandBuffers(c.device,&cbai,&c.cb));
    return c;
}

static Buffer create_buffer(VkDevice dev, VkDeviceSize size) {
    Buffer b={0};
    VkBufferCreateInfo ci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=size,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    CK(vkCreateBuffer(dev,&ci,NULL,&b.handle));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(dev,b.handle,&req);
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=0};
    CK(vkAllocateMemory(dev,&ai,NULL,&b.memory)); CK(vkBindBufferMemory(dev,b.handle,b.memory,0));
    return b;
}

static Image create_image(VkDevice dev) {
    Image image={0};
    VkImageCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,.extent={64,64,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    CK(vkCreateImage(dev,&ci,NULL,&image.handle));
    VkMemoryRequirements req; vkGetImageMemoryRequirements(dev,image.handle,&req);
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=0};
    CK(vkAllocateMemory(dev,&ai,NULL,&image.memory)); CK(vkBindImageMemory(dev,image.handle,image.memory,0));
    return image;
}

static void barrier(VkCommandBuffer cb, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout, VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=src,.dstAccessMask=dst,.oldLayout=old_layout,.newLayout=new_layout,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,NULL,0,NULL,1,&b);
}

int main(void) {
    const char *alpha_text=getenv("LG110_OFFSCREEN_ALPHA");
    const char *path=getenv("LG110_READBACK_PIXEL");
    const char *expected_text=getenv("LG110_EXPECT_OFFSCREEN_ALPHA");
    float alpha;
    float expected_alpha;
    if (!alpha_text || (strcmp(alpha_text,"1.0") && strcmp(alpha_text,"0.5")) || !path || !*path) {
        fprintf(stderr,"set LG110_OFFSCREEN_ALPHA=1.0|0.5 and LG110_READBACK_PIXEL=path\n"); return EXIT_FAILURE;
    }
    alpha=strcmp(alpha_text,"0.5") ? 1.0f : 0.5f;
    if (!expected_text) expected_text=alpha_text;
    if (strcmp(expected_text,"1.0") && strcmp(expected_text,"0.5")) {
        fprintf(stderr,"LG110_EXPECT_OFFSCREEN_ALPHA must be 1.0 or 0.5\n"); return EXIT_FAILURE;
    }
    expected_alpha=strcmp(expected_text,"0.5") ? 1.0f : 0.5f;
    Context c=create_context(); Image image=create_image(c.device); Buffer readback=create_buffer(c.device,4);
    void *mapped=NULL; CK(vkMapMemory(c.device,readback.memory,0,4,0,&mapped)); memset(mapped,0xa5,4); vkUnmapMemory(c.device,readback.memory);
    VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CK(vkBeginCommandBuffer(c.cb,&bi));
    barrier(c.cb,image.handle,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT);
    VkClearColorValue clear={.float32={0.0f,0.2f,0.4f,alpha}};
    VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdClearColorImage(c.cb,image.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&range);
    barrier(c.cb,image.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={1,1,1}};
    vkCmdCopyImageToBuffer(c.cb,image.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.handle,1,&copy);
    CK(vkEndCommandBuffer(c.cb));
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&c.cb};
    CK(vkQueueSubmit(c.queue,1,&si,VK_NULL_HANDLE)); CK(vkQueueWaitIdle(c.queue));
    uint8_t actual[4]; CK(vkMapMemory(c.device,readback.memory,0,4,0,&mapped)); memcpy(actual,mapped,4); vkUnmapMemory(c.device,readback.memory);
    FILE *file=fopen(path,"wb"); if (!file || fwrite(actual,1,4,file)!=4 || fclose(file)!=0) { perror("raw output"); return EXIT_FAILURE; }
    uint8_t expected[4]={0,51,102,expected_alpha==1.0f ? 255u : 128u};
    int valid=actual[0]==expected[0] && actual[1]==expected[1] && actual[2]==expected[2];
    if (expected_alpha==1.0f) valid=valid && actual[3]==255; else valid=valid && (actual[3]==127 || actual[3]==128);
    printf("offscreen clear alpha=%s expected_alpha=%s expected=%02x%02x%02x%02x actual=%02x%02x%02x%02x pixels_valid=%d sentinel_unchanged=%d\n",alpha_text,expected_text,expected[0],expected[1],expected[2],expected[3],actual[0],actual[1],actual[2],actual[3],valid,actual[0]==0xa5 && actual[1]==0xa5 && actual[2]==0xa5 && actual[3]==0xa5);
    vkDestroyBuffer(c.device,readback.handle,NULL); vkFreeMemory(c.device,readback.memory,NULL);
    vkDestroyImage(c.device,image.handle,NULL); vkFreeMemory(c.device,image.memory,NULL);
    CK(vkDeviceWaitIdle(c.device)); vkDestroyCommandPool(c.device,c.pool,NULL); vkDestroyDevice(c.device,NULL); vkDestroyInstance(c.instance,NULL);
    return valid ? EXIT_SUCCESS : EXIT_FAILURE;
}
