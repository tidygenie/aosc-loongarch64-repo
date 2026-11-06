#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
    fprintf(stderr, "FAIL line %d: %s = %d\n", __LINE__, #x, r_); return 1; \
} } while (0)

static void expect_rejected(VkDevice device, const char *name,
                            const VkSamplerCreateInfo *info, int *failures) {
    VkSampler sampler = VK_NULL_HANDLE;
    VkResult result = vkCreateSampler(device, info, NULL, &sampler);
    printf("REJECT name=%s result=%d\n", name, result);
    if (result != VK_ERROR_FEATURE_NOT_PRESENT) {
        fprintf(stderr, "FAIL unsupported sampler %s returned %d\n", name, result);
        ++*failures;
    }
    if (result == VK_SUCCESS) vkDestroySampler(device, sampler, NULL);
}

int main(void) {
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "vktest-sampler-create",
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
    if (count != 1) return 1;
    VkPhysicalDevice gpu;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, &gpu));
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceProperties(gpu, &properties);
    vkGetPhysicalDeviceFeatures(gpu, &features);
    printf("SAMPLER device=%s feature=%u maxAnisotropy=%.1f\n",
           properties.deviceName, features.samplerAnisotropy,
           properties.limits.maxSamplerAnisotropy);
    int failures = 0;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1, .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
    };
    VkDevice device;
    CHECK(vkCreateDevice(gpu, &device_info, NULL, &device));

    VkSamplerCreateInfo valid = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .anisotropyEnable = VK_FALSE, .maxAnisotropy = 1.0f,
    };
    VkSampler sampler;
    CHECK(vkCreateSampler(device, &valid, NULL, &sampler));
    vkDestroySampler(device, sampler, NULL);

    VkSamplerCreateInfo invalid = valid;
    invalid.compareEnable = VK_TRUE;
    expect_rejected(device, "compare", &invalid, &failures);
    invalid = valid; invalid.unnormalizedCoordinates = VK_TRUE;
    expect_rejected(device, "unnormalized", &invalid, &failures);
    invalid = valid; invalid.mipLodBias = 0.5f;
    expect_rejected(device, "lod-bias", &invalid, &failures);
    invalid = valid; invalid.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    expect_rejected(device, "clamp-border", &invalid, &failures);
    invalid = valid; invalid.addressModeU = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    expect_rejected(device, "mirror-clamp", &invalid, &failures);
    invalid = valid; invalid.magFilter = VK_FILTER_CUBIC_IMG;
    expect_rejected(device, "cubic-filter", &invalid, &failures);

    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    if (failures) { fprintf(stderr, "FAIL sampler-create failures=%d\n", failures); return 1; }
    printf("PASS vktest-sampler-create (filter/wrap + 6 explicit rejections)\n");
    return 0;
}
