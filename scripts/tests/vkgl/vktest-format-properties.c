#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
    fprintf(stderr, "FAIL line %d: %s = %d\n", __LINE__, #x, r_); return 1; \
} } while (0)

static int require_bits(const char *name, VkFormat format,
                        const VkFormatProperties *properties, int *failures) {
    const VkFormatFeatureFlags color =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    printf("FORMAT name=%s format=%d linear=0x%08x optimal=0x%08x buffer=0x%08x\n",
           name, format, properties->linearTilingFeatures,
           properties->optimalTilingFeatures, properties->bufferFeatures);
    if ((properties->linearTilingFeatures & color) != color ||
        (properties->optimalTilingFeatures & color) != color) {
        fprintf(stderr, "MISSING color/linear-filter/transfer bits: %s\n", name);
        ++*failures;
    }
    return 0;
}

int main(void) {
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "vktest-format-properties",
        .apiVersion = VK_API_VERSION_1_0,
    };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    VkInstance instance;
    CHECK(vkCreateInstance(&instance_info, NULL, &instance));
    uint32_t count = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, NULL));
    if (count != 1) { fprintf(stderr, "expected one GPU, got %u\n", count); return 1; }
    VkPhysicalDevice gpu;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, &gpu));
    VkPhysicalDeviceProperties device;
    vkGetPhysicalDeviceProperties(gpu, &device);
    if (!strstr(device.deviceName, "vkgl")) {
        fprintf(stderr, "wrong device: %s\n", device.deviceName); return 1;
    }

    const VkFormat colors[] = {
        VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB,
    };
    const char *color_names[] = { "B8A8_UNORM", "R8A8_UNORM", "B8A8_SRGB", "R8A8_SRGB" };
    int failures = 0;
    for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); ++i) {
        VkFormatProperties properties;
        vkGetPhysicalDeviceFormatProperties(gpu, colors[i], &properties);
        require_bits(color_names[i], colors[i], &properties, &failures);
    }

    const VkFormat vertices[] = {
        VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT,
        VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT,
    };
    const char *vertex_names[] = { "R32F", "R32G32F", "R32G32B32F", "R32G32B32A32F" };
    for (size_t i = 0; i < sizeof(vertices) / sizeof(vertices[0]); ++i) {
        VkFormatProperties properties;
        vkGetPhysicalDeviceFormatProperties(gpu, vertices[i], &properties);
        printf("FORMAT name=%s format=%d linear=0x%08x optimal=0x%08x buffer=0x%08x\n",
               vertex_names[i], vertices[i], properties.linearTilingFeatures,
               properties.optimalTilingFeatures, properties.bufferFeatures);
        if (!(properties.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) {
            fprintf(stderr, "MISSING vertex-buffer bit: %s\n", vertex_names[i]);
            ++failures;
        }
    }

    VkFormatProperties2 properties2 = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2 };
    PFN_vkGetPhysicalDeviceFormatProperties2 get2 =
        (PFN_vkGetPhysicalDeviceFormatProperties2)vkGetInstanceProcAddr(
            instance, "vkGetPhysicalDeviceFormatProperties2");
    if (!get2) {
        fprintf(stderr, "MISSING GetPhysicalDeviceFormatProperties2\n");
        ++failures;
    } else {
        get2(gpu, VK_FORMAT_R32G32B32_SFLOAT, &properties2);
        printf("FORMAT2 buffer=0x%08x\n", properties2.formatProperties.bufferFeatures);
        if (!(properties2.formatProperties.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT))
            ++failures;
    }

    vkDestroyInstance(instance, NULL);
    if (failures) { fprintf(stderr, "FAIL format-properties failures=%d\n", failures); return 1; }
    printf("PASS vktest-format-properties (4 color + 4 vertex + format2)\n");
    return 0;
}
