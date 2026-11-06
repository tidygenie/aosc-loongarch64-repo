#define _POSIX_C_SOURCE 200809L
#include <vulkan/vulkan.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK_W 40u
#define CHECK_H 32u
#define COPY_W 17u
#define COPY_H 11u
#define COPY_X 5u
#define COPY_Y 7u
#define SOURCE_OFFSET 16u
#define INIT_SALT 23u
#define INIT_ROW (CHECK_W + 3u)
#define DEFAULT_BENCH_W 1024u
#define DEFAULT_BENCH_H 1024u
#define DEFAULT_BENCH_UPLOADS 64u
#define DEFAULT_BENCH_ROUNDS 7u

#define CK(call) do { \
   VkResult ck_result_ = (call); \
   if (ck_result_ != VK_SUCCESS) { \
      fprintf(stderr, "FAIL %s:%d: %s = %d\n", __FILE__, __LINE__, #call, ck_result_); \
      exit(EXIT_FAILURE); \
   } \
} while (0)

typedef struct Context {
   VkInstance instance;
   VkPhysicalDevice gpu;
   VkDevice device;
   VkQueue queue;
   VkCommandPool command_pool;
   VkCommandBuffer cb;
} Context;

typedef struct Buffer {
   VkBuffer handle;
   VkDeviceMemory memory;
   VkDeviceSize size;
} Buffer;

typedef struct Image {
   VkImage handle;
   VkDeviceMemory memory;
   uint32_t width;
   uint32_t height;
} Image;

typedef struct UploadCase {
   const char *name;
   uint32_t row_length;
   uint32_t buffer_row_length;
   uint32_t salt;
   VkImageTiling tiling;
} UploadCase;

static void die(const char *message) {
   fprintf(stderr, "FAIL: %s\n", message);
   exit(EXIT_FAILURE);
}

static void *xmalloc(size_t size) {
   void *p = malloc(size);
   if (!p) die("host allocation failed");
   return p;
}

static size_t image_bytes(uint32_t width, uint32_t height) {
   return (size_t)width * (size_t)height * 4u;
}

static size_t upload_bytes(uint32_t width, uint32_t height, uint32_t row_length,
                           size_t offset) {
   return offset + ((size_t)(height - 1u) * row_length + width) * 4u;
}

static void pattern_pixel(uint32_t x, uint32_t y, uint32_t salt, uint8_t out[4]) {
   out[0] = (uint8_t)(x * 7u + y * 13u + salt * 3u + 1u);
   out[1] = (uint8_t)(x * 11u + y * 5u + salt * 17u + 2u);
   out[2] = (uint8_t)((x ^ (y * 3u) ^ salt) + 29u);
   out[3] = (uint8_t)(255u - ((x + y + salt) & 31u));
}

static void fill_staging(uint8_t *data, size_t size, size_t offset, uint32_t row_length,
                         uint32_t width, uint32_t height, uint32_t salt) {
   memset(data, 0xcd, size);
   for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
         uint8_t pixel[4];
         pattern_pixel(x, y, salt, pixel);
         memcpy(data + offset + ((size_t)y * row_length + x) * 4u, pixel, 4u);
      }
   }
}

// The published CopyImageToBuffer path reverses rows after glReadPixels.
// Normalize that existing behavior so this test isolates upload packing rather
// than asserting a different readback orientation.
static void normalize_readback_rows(uint8_t *data, uint32_t width, uint32_t height) {
   const size_t pitch = (size_t)width * 4u;
   for (uint32_t y = 0; y < height / 2u; ++y) {
      uint8_t *top = data + (size_t)y * pitch;
      uint8_t *bottom = data + (size_t)(height - 1u - y) * pitch;
      for (size_t i = 0; i < pitch; ++i) {
         uint8_t tmp = top[i];
         top[i] = bottom[i];
         bottom[i] = tmp;
      }
   }
}

static Buffer create_buffer(Context *ctx, VkDeviceSize size, VkBufferUsageFlags usage) {
   Buffer b = {0};
   b.size = size;
   VkBufferCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   CK(vkCreateBuffer(ctx->device, &ci, NULL, &b.handle));
   VkMemoryRequirements req = {0};
   vkGetBufferMemoryRequirements(ctx->device, b.handle, &req);
   VkMemoryAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = 0,
   };
   CK(vkAllocateMemory(ctx->device, &ai, NULL, &b.memory));
   CK(vkBindBufferMemory(ctx->device, b.handle, b.memory, 0));
   return b;
}

static void destroy_buffer(Context *ctx, Buffer *b) {
   vkDestroyBuffer(ctx->device, b->handle, NULL);
   vkFreeMemory(ctx->device, b->memory, NULL);
   memset(b, 0, sizeof(*b));
}

static void upload_host_data(Context *ctx, Buffer *b, const void *data, size_t size) {
   if ((uint64_t)size > b->size) die("host upload exceeds buffer size");
   void *mapped = NULL;
   CK(vkMapMemory(ctx->device, b->memory, 0, VK_WHOLE_SIZE, 0, &mapped));
   memcpy(mapped, data, size);
   vkUnmapMemory(ctx->device, b->memory);
}

static Buffer create_staging_buffer(Context *ctx, const uint8_t *data, size_t size) {
   Buffer b = create_buffer(ctx, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
   upload_host_data(ctx, &b, data, size);
   return b;
}

static Image create_image(Context *ctx, uint32_t width, uint32_t height, VkImageTiling tiling) {
   Image image = {0};
   image.width = width;
   image.height = height;
   VkImageCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = {width, height, 1u},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = tiling,
      .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   CK(vkCreateImage(ctx->device, &ci, NULL, &image.handle));
   VkMemoryRequirements req = {0};
   vkGetImageMemoryRequirements(ctx->device, image.handle, &req);
   VkMemoryAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = 0,
   };
   CK(vkAllocateMemory(ctx->device, &ai, NULL, &image.memory));
   CK(vkBindImageMemory(ctx->device, image.handle, image.memory, 0));
   return image;
}

static void destroy_image(Context *ctx, Image *image) {
   vkDestroyImage(ctx->device, image->handle, NULL);
   vkFreeMemory(ctx->device, image->memory, NULL);
   memset(image, 0, sizeof(*image));
}

static void begin_commands(Context *ctx) {
   VkCommandBufferBeginInfo bi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   CK(vkBeginCommandBuffer(ctx->cb, &bi));
}

static void submit_and_wait(Context *ctx) {
   CK(vkEndCommandBuffer(ctx->cb));
   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &ctx->cb,
   };
   CK(vkQueueSubmit(ctx->queue, 1, &si, VK_NULL_HANDLE));
   CK(vkQueueWaitIdle(ctx->queue));
}

static void image_barrier(Context *ctx, Image *image, VkImageLayout old_layout,
                          VkImageLayout new_layout, VkAccessFlags src_access,
                          VkAccessFlags dst_access) {
   VkImageMemoryBarrier barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = src_access,
      .dstAccessMask = dst_access,
      .oldLayout = old_layout,
      .newLayout = new_layout,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image->handle,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   vkCmdPipelineBarrier(ctx->cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static void record_copy_to_image(Context *ctx, Buffer *src, Image *dst,
                                 uint32_t buffer_row_length, size_t buffer_offset,
                                 uint32_t image_x, uint32_t image_y,
                                 uint32_t width, uint32_t height) {
   VkBufferImageCopy region = {
      .bufferOffset = buffer_offset,
      .bufferRowLength = buffer_row_length,
      .bufferImageHeight = 0,
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .imageOffset = {(int32_t)image_x, (int32_t)image_y, 0},
      .imageExtent = {width, height, 1u},
   };
   vkCmdCopyBufferToImage(ctx->cb, src->handle, dst->handle,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

static Context create_context(void) {
   Context ctx = {0};
   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "vktest-tight-upload",
      .apiVersion = VK_API_VERSION_1_0,
   };
   VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   CK(vkCreateInstance(&ici, NULL, &ctx.instance));
   uint32_t gpu_count = 0;
   CK(vkEnumeratePhysicalDevices(ctx.instance, &gpu_count, NULL));
   if (!gpu_count) die("no Vulkan physical device");
   VkPhysicalDevice *gpus = xmalloc((size_t)gpu_count * sizeof(*gpus));
   CK(vkEnumeratePhysicalDevices(ctx.instance, &gpu_count, gpus));
   for (uint32_t i = 0; i < gpu_count; ++i) {
      VkPhysicalDeviceProperties properties = {0};
      vkGetPhysicalDeviceProperties(gpus[i], &properties);
      if (strstr(properties.deviceName, "vkgl")) {
         ctx.gpu = gpus[i];
         printf("TEST_DEVICE %s\n", properties.deviceName);
         break;
      }
   }
   free(gpus);
   if (!ctx.gpu) die("selected ICD is not vkgl; set VK_ICD_FILENAMES explicitly");

   float priority = 1.0f;
   VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };
   VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qci,
   };
   CK(vkCreateDevice(ctx.gpu, &dci, NULL, &ctx.device));
   vkGetDeviceQueue(ctx.device, 0, 0, &ctx.queue);
   VkCommandPoolCreateInfo pci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = 0,
   };
   CK(vkCreateCommandPool(ctx.device, &pci, NULL, &ctx.command_pool));
   VkCommandBufferAllocateInfo cbai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = ctx.command_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   CK(vkAllocateCommandBuffers(ctx.device, &cbai, &ctx.cb));
   return ctx;
}

static void destroy_context(Context *ctx) {
   CK(vkDeviceWaitIdle(ctx->device));
   vkDestroyCommandPool(ctx->device, ctx->command_pool, NULL);
   vkDestroyDevice(ctx->device, NULL);
   vkDestroyInstance(ctx->instance, NULL);
   memset(ctx, 0, sizeof(*ctx));
}

static uint8_t *run_check_case(Context *ctx, Buffer *init, const UploadCase *test_case) {
   const size_t src_size = upload_bytes(COPY_W, COPY_H, test_case->row_length, SOURCE_OFFSET);
   uint8_t *src_data = xmalloc(src_size);
   fill_staging(src_data, src_size, SOURCE_OFFSET, test_case->row_length,
                COPY_W, COPY_H, test_case->salt);
   Buffer src = create_staging_buffer(ctx, src_data, src_size);
   free(src_data);

   Image dst = create_image(ctx, CHECK_W, CHECK_H, test_case->tiling);
   Buffer readback = create_buffer(ctx, image_bytes(CHECK_W, CHECK_H),
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT);
   begin_commands(ctx);
   image_barrier(ctx, &dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 0, VK_ACCESS_TRANSFER_WRITE_BIT);
   record_copy_to_image(ctx, init, &dst, INIT_ROW, 0, 0, 0, CHECK_W, CHECK_H);
   image_barrier(ctx, &dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT);
   record_copy_to_image(ctx, &src, &dst, test_case->buffer_row_length, SOURCE_OFFSET,
                        COPY_X, COPY_Y, COPY_W, COPY_H);
   image_barrier(ctx, &dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_ACCESS_TRANSFER_READ_BIT);
   VkBufferImageCopy read_region = {
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .imageExtent = {CHECK_W, CHECK_H, 1u},
   };
   vkCmdCopyImageToBuffer(ctx->cb, dst.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          readback.handle, 1, &read_region);
   submit_and_wait(ctx);

   const size_t expected_size = image_bytes(CHECK_W, CHECK_H);
   uint8_t *expected = xmalloc(expected_size);
   for (uint32_t y = 0; y < CHECK_H; ++y) {
      for (uint32_t x = 0; x < CHECK_W; ++x) {
         uint8_t pixel[4];
         pattern_pixel(x, y, INIT_SALT, pixel);
         memcpy(expected + ((size_t)y * CHECK_W + x) * 4u, pixel, 4u);
      }
   }
   for (uint32_t y = 0; y < COPY_H; ++y) {
      for (uint32_t x = 0; x < COPY_W; ++x) {
         uint8_t pixel[4];
         pattern_pixel(x, y, test_case->salt, pixel);
         memcpy(expected + ((size_t)(COPY_Y + y) * CHECK_W + COPY_X + x) * 4u, pixel, 4u);
      }
   }

   void *mapped = NULL;
   CK(vkMapMemory(ctx->device, readback.memory, 0, VK_WHOLE_SIZE, 0, &mapped));
   uint8_t *actual = xmalloc(expected_size);
   memcpy(actual, mapped, expected_size);
   vkUnmapMemory(ctx->device, readback.memory);
   normalize_readback_rows(actual, CHECK_W, CHECK_H);
   if (memcmp(actual, expected, expected_size) != 0) {
      size_t bad = 0;
      while (bad < expected_size && actual[bad] == expected[bad]) ++bad;
      size_t pixel_index = bad / 4u;
      fprintf(stderr,
              "FAIL case=%s x=%zu y=%zu channel=%zu expected=%02x actual=%02x\n",
              test_case->name, pixel_index % CHECK_W, pixel_index / CHECK_W, bad % 4u,
              bad < expected_size ? expected[bad] : 0u,
              bad < expected_size ? actual[bad] : 0u);
      exit(EXIT_FAILURE);
   }
   printf("PASS case=%s bufferRowLength=%u bufferOffset=%u imageOffset=%u,%u\n",
          test_case->name, test_case->buffer_row_length, SOURCE_OFFSET, COPY_X, COPY_Y);
   free(expected);
   destroy_buffer(ctx, &readback);
   destroy_image(ctx, &dst);
   destroy_buffer(ctx, &src);
   return actual;
}

static Buffer create_check_init(Context *ctx) {
   size_t size = upload_bytes(CHECK_W, CHECK_H, INIT_ROW, 0);
   uint8_t *data = xmalloc(size);
   fill_staging(data, size, 0, INIT_ROW, CHECK_W, CHECK_H, INIT_SALT);
   Buffer init = create_staging_buffer(ctx, data, size);
   free(data);
   return init;
}

static void run_checks(Context *ctx) {
   static const UploadCase cases[] = {
      {"optimal-padded-reference", COPY_W + 7u, COPY_W + 7u, 71u, VK_IMAGE_TILING_OPTIMAL},
      {"optimal-tight-row-zero", COPY_W, 0u, 71u, VK_IMAGE_TILING_OPTIMAL},
      {"optimal-tight-row-width", COPY_W, COPY_W, 71u, VK_IMAGE_TILING_OPTIMAL},
      {"linear-padded-reference", COPY_W + 7u, COPY_W + 7u, 71u, VK_IMAGE_TILING_LINEAR},
      {"linear-tight-row-zero", COPY_W, 0u, 71u, VK_IMAGE_TILING_LINEAR},
      {"linear-tight-row-width", COPY_W, COPY_W, 71u, VK_IMAGE_TILING_LINEAR},
   };
   Buffer init = create_check_init(ctx);
   uint8_t *outputs[sizeof(cases) / sizeof(cases[0])] = {0};
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
      outputs[i] = run_check_case(ctx, &init, &cases[i]);
   for (size_t i = 1; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      if (memcmp(outputs[0], outputs[i], image_bytes(CHECK_W, CHECK_H)) != 0)
         die("tight output differs from padded reference");
   }
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) free(outputs[i]);
   destroy_buffer(ctx, &init);
   printf("PASS tight-vs-padded pixel comparison\n");
}

static double monotonic_seconds(void) {
   struct timespec ts = {0};
   if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
      perror("clock_gettime");
      exit(EXIT_FAILURE);
   }
   return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static double time_upload_batch(Context *ctx, Buffer *src, Image *dst,
                                uint32_t buffer_row_length, uint32_t width,
                                uint32_t height, uint32_t uploads, VkImageLayout old_layout) {
   begin_commands(ctx);
   image_barrier(ctx, dst, old_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 old_layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0u : VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT);
   for (uint32_t i = 0; i < uploads; ++i) {
      record_copy_to_image(ctx, src, dst, buffer_row_length, 0, 0, 0, width, height);
      image_barrier(ctx, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
   }
   CK(vkEndCommandBuffer(ctx->cb));
   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &ctx->cb,
   };
   double start = monotonic_seconds();
   CK(vkQueueSubmit(ctx->queue, 1, &si, VK_NULL_HANDLE));
   CK(vkQueueWaitIdle(ctx->queue));
   double end = monotonic_seconds();
   return end - start;
}

static int compare_doubles(const void *a, const void *b) {
   double da = *(const double *)a;
   double db = *(const double *)b;
   return (da > db) - (da < db);
}

static void run_bench_mode(Context *ctx, const char *name, int padded,
                           uint32_t width, uint32_t height, uint32_t uploads,
                           uint32_t rounds) {
   uint32_t row_length = padded ? width + 13u : width;
   uint32_t buffer_row_length = padded ? row_length : 0u;
   size_t src_size = upload_bytes(width, height, row_length, 0);
   uint8_t *src_data = xmalloc(src_size);
   fill_staging(src_data, src_size, 0, row_length, width, height, 89u);
   Buffer src = create_staging_buffer(ctx, src_data, src_size);
   free(src_data);
   Image dst = create_image(ctx, width, height, VK_IMAGE_TILING_OPTIMAL);

   (void)time_upload_batch(ctx, &src, &dst, buffer_row_length, width, height, 1u,
                           VK_IMAGE_LAYOUT_UNDEFINED);
   double *samples = xmalloc((size_t)rounds * sizeof(*samples));
   uint64_t total_bytes = (uint64_t)uploads * width * height * 4u;
   printf("BENCH_BEGIN mode=%s width=%u height=%u uploads=%u rounds=%u bytes=%" PRIu64 "\n",
          name, width, height, uploads, rounds, total_bytes);
   for (uint32_t round = 0; round < rounds; ++round) {
      samples[round] = time_upload_batch(ctx, &src, &dst, buffer_row_length, width, height,
                                         uploads, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      double mib = (double)total_bytes / (1024.0 * 1024.0);
      printf("BENCH_ROUND mode=%s round=%u seconds=%.9f MiB_per_s=%.3f\n",
             name, round, samples[round], mib / samples[round]);
   }
   qsort(samples, rounds, sizeof(*samples), compare_doubles);
   double median = samples[rounds / 2u];
   if ((rounds & 1u) == 0u) median = (median + samples[rounds / 2u - 1u]) * 0.5;
   printf("BENCH_MEDIAN mode=%s seconds=%.9f MiB_per_s=%.3f\n",
          name, median, (double)total_bytes / (1024.0 * 1024.0) / median);

   free(samples);
   destroy_image(ctx, &dst);
   destroy_buffer(ctx, &src);
}

static uint32_t parse_u32(const char *text, const char *name, uint32_t maximum) {
   if (!text[0] || text[0] == '-') die("numeric arguments must be positive integers");
   errno = 0;
   char *end = NULL;
   unsigned long value = strtoul(text, &end, 10);
   if (errno || !end || *end || !value || value > maximum) {
      fprintf(stderr, "FAIL: invalid %s: %s (maximum %u)\n", name, text, maximum);
      exit(EXIT_FAILURE);
   }
   return (uint32_t)value;
}

static void usage(const char *program) {
   fprintf(stderr,
           "Usage: %s [--check]\n"
           "       %s --bench UPLOADS [WIDTH HEIGHT [ROUNDS]]\n"
           "Defaults: WIDTH=%u HEIGHT=%u ROUNDS=%u\n",
           program, program, DEFAULT_BENCH_W, DEFAULT_BENCH_H, DEFAULT_BENCH_ROUNDS);
   exit(EXIT_FAILURE);
}

int main(int argc, char **argv) {
   int bench = 0;
   uint32_t uploads = DEFAULT_BENCH_UPLOADS;
   uint32_t width = DEFAULT_BENCH_W;
   uint32_t height = DEFAULT_BENCH_H;
   uint32_t rounds = DEFAULT_BENCH_ROUNDS;
   if (argc == 2 && strcmp(argv[1], "--check") == 0) {
      bench = 0;
   } else if (argc >= 3 && strcmp(argv[1], "--bench") == 0) {
      if (argc != 3 && argc != 5 && argc != 6) usage(argv[0]);
      bench = 1;
      uploads = parse_u32(argv[2], "upload count", 100000u);
      if (argc >= 5) {
         width = parse_u32(argv[3], "width", 4096u);
         height = parse_u32(argv[4], "height", 4096u);
      }
      if (argc == 6) rounds = parse_u32(argv[5], "round count", 101u);
   } else if (argc != 1) {
      usage(argv[0]);
   }

   Context ctx = create_context();
   run_checks(&ctx);
   if (bench) {
      run_bench_mode(&ctx, "tight-row-zero", 0, width, height, uploads, rounds);
      run_bench_mode(&ctx, "padded-reference", 1, width, height, uploads, rounds);
   }
   destroy_context(&ctx);
   printf("PASS vktest-tight-upload%s\n", bench ? " (correctness and benchmark)" : "");
   return EXIT_SUCCESS;
}
