/* test_vulkan_astc.c — ASTC LDR textures through the Vulkan thunk, headless.
 *
 * A host GPU without ASTC (AMD and NVIDIA desktop drivers) is made to look like one with it: the thunk reports the
 * feature and the formats, keeps each ASTC image as a BC7 image and transcodes the guest's ASTC blocks when the upload
 * is submitted. This checks that from the guest's side: the feature and format queries (the plain and the "2" forms),
 * a device created with the feature enabled (through VkPhysicalDeviceFeatures2 and through pEnabledFeatures), and
 * images of three footprints and both colour spaces, with mip levels and partial edge blocks, uploaded with
 * vkCmdCopyBufferToImage and read back by a compute shader that fetches the texels. Each block is a void-extent block
 * (one colour), so the expected texels are known exactly; the colours lie on a line, which BC7 follows to a couple of
 * levels.
 *
 * Exit 0 = pass, 1 = fail, 77 = skip (no Vulkan driver, or a driver with neither ASTC nor BC7).
 *
 * Build for the emulator:
 *   make cross SRC=Tests/Apps/test_vulkan_astc.c OUT=Tests/Apps/test_vulkan_astc.elf \
 *     CROSS_EXTRA=-ITests/Apps/vulkan_headers/include
 * The helpers also build natively (-DVK_ASTC_HELPERS_ONLY gives a header-like unit for probes).
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan_core.h>
#ifdef __aarch64__
#include <sys/syscall.h>
#include <unistd.h>
#define __NR_bifrost_dlopen 0x1002
#define __NR_bifrost_dlsym 0x1003
static void* load_lib(const char* path) { return (void*)(long)syscall(__NR_bifrost_dlopen, path, 1L); }
static void* load_sym(void* h, const char* n) { return (void*)(long)syscall(__NR_bifrost_dlsym, h, n); }
#else
#include <dlfcn.h>
static void* load_lib(const char* path) { return dlopen(path, RTLD_NOW); }
static void* load_sym(void* h, const char* n) { return dlsym(h, n); }
#endif
#include "test_vulkan_astc_spv.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { if (cond) printf("  OK  %s\n", msg); else { printf(" FAIL  %s\n", msg); g_fail++; } } while (0)

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
static VkInstance instance;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static uint32_t family;
static VkPhysicalDeviceMemoryProperties memprops;

#define IFN(name) PFN_##name name = (PFN_##name)gipa(instance, #name)
#define DFN(name) PFN_##name name = (PFN_##name)gdpa(dev, #name)

static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

typedef struct { VkBuffer buf; VkDeviceMemory mem; void* ptr; } HostBuffer;

static int make_host_buffer(HostBuffer* b, VkDeviceSize size, VkBufferUsageFlags usage) {
    DFN(vkCreateBuffer); DFN(vkGetBufferMemoryRequirements); DFN(vkAllocateMemory); DFN(vkBindBufferMemory); DFN(vkMapMemory);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    if (vkCreateBuffer(dev, &bi, NULL, &b->buf) != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, b->buf, &req);
    uint32_t type = memtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
    if (type == UINT32_MAX || vkAllocateMemory(dev, &ai, NULL, &b->mem) != VK_SUCCESS) return 0;
    return vkBindBufferMemory(dev, b->buf, b->mem, 0) == VK_SUCCESS && vkMapMemory(dev, b->mem, 0, VK_WHOLE_SIZE, 0, &b->ptr) == VK_SUCCESS;
}

/* The compute pipeline that copies texelFetch(tex, p, mip) of every texel to a buffer. */
static VkDescriptorSetLayout set_layout;
static VkPipelineLayout pipe_layout;
static VkPipeline pipeline;
static VkSampler sampler;
static VkDescriptorPool dpool;
static VkCommandPool cpool;

static int make_pipeline(void) {
    DFN(vkCreateDescriptorSetLayout); DFN(vkCreatePipelineLayout); DFN(vkCreateShaderModule); DFN(vkCreateComputePipelines);
    DFN(vkCreateSampler); DFN(vkCreateDescriptorPool); DFN(vkCreateCommandPool);
    VkDescriptorSetLayoutBinding b[2] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b};
    VkPushConstantRange pc = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 16};
    if (vkCreateDescriptorSetLayout(dev, &li, NULL, &set_layout) != VK_SUCCESS) return 0;
    VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &set_layout,
                                      .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
    if (vkCreatePipelineLayout(dev, &pli, NULL, &pipe_layout) != VK_SUCCESS) return 0;
    VkShaderModuleCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(k_fetch_spv), .pCode = k_fetch_spv};
    VkShaderModule sm;
    if (vkCreateShaderModule(dev, &si, NULL, &sm) != VK_SUCCESS) return 0;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pipe_layout,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main"}};
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &pipeline) != VK_SUCCESS) return 0;
    VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 16.f};
    if (vkCreateSampler(dev, &sci, NULL, &sampler) != VK_SUCCESS) return 0;
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16}};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 16, .poolSizeCount = 2, .pPoolSizes = ps};
    if (vkCreateDescriptorPool(dev, &dpi, NULL, &dpool) != VK_SUCCESS) return 0;
    VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family};
    return vkCreateCommandPool(dev, &cpi, NULL, &cpool) == VK_SUCCESS;
}

static VkCommandBuffer begin_cmd(void) {
    DFN(vkAllocateCommandBuffers); DFN(vkBeginCommandBuffer);
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(dev, &ai, &cb);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

static int submit_wait(VkCommandBuffer cb) {
    DFN(vkEndCommandBuffer); DFN(vkQueueSubmit); DFN(vkQueueWaitIdle); DFN(vkFreeCommandBuffers);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    int ok = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS && vkQueueWaitIdle(queue) == VK_SUCCESS;
    vkFreeCommandBuffers(dev, cpool, 1, &cb);
    return ok;
}

/* Creates a 2D image of `format`, uploads `data` (the mips at mip_offsets, rows packed) with one
 * vkCmdCopyBufferToImage, and returns the texels of each mip as float RGBA in out[mip] (caller frees). */
static int upload_and_fetch(VkFormat format, uint32_t w, uint32_t h, uint32_t mips, const uint8_t* data, size_t size,
                            const size_t* mip_offsets, float** out) {
    DFN(vkCreateImage); DFN(vkGetImageMemoryRequirements); DFN(vkAllocateMemory); DFN(vkBindImageMemory); DFN(vkCreateImageView);
    DFN(vkCmdPipelineBarrier); DFN(vkCmdCopyBufferToImage); DFN(vkAllocateDescriptorSets); DFN(vkUpdateDescriptorSets);
    DFN(vkCmdBindPipeline); DFN(vkCmdBindDescriptorSets); DFN(vkCmdPushConstants); DFN(vkCmdDispatch);
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = {w, h, 1},
        .mipLevels = mips, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage image;
    if (vkCreateImage(dev, &ii, NULL, &image) != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, image, &req);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                               .memoryTypeIndex = memtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory imem;
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &ai, NULL, &imem) != VK_SUCCESS || vkBindImageMemory(dev, image, imem, 0) != VK_SUCCESS) return 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1}};
    VkImageView view;
    if (vkCreateImageView(dev, &vi, NULL, &view) != VK_SUCCESS) return 0;

    HostBuffer staging;
    if (!make_host_buffer(&staging, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return 0;
    memcpy(staging.ptr, data, size);
    /* Engines differ: some keep staging memory mapped, some unmap it before submitting. Do both. */
    static int uploads;
    if (uploads++ & 1) { PFN_vkUnmapMemory vkUnmapMemory = (PFN_vkUnmapMemory)gdpa(dev, "vkUnmapMemory"); vkUnmapMemory(dev, staging.mem); }
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
    VkCommandBuffer cb = begin_cmd();
    VkImageMemoryBarrier to_dst = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image, .subresourceRange = all};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_dst);
    VkBufferImageCopy regions[16];
    for (uint32_t m = 0; m < mips; m++)
        regions[m] = (VkBufferImageCopy){.bufferOffset = mip_offsets[m], .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1},
            .imageExtent = {w >> m ? w >> m : 1, h >> m ? h >> m : 1, 1}};
    vkCmdCopyBufferToImage(cb, staging.buf, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mips, regions);
    VkImageMemoryBarrier to_read = to_dst;
    to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &to_read);
    if (!submit_wait(cb)) return 0;

    for (uint32_t m = 0; m < mips; m++) {
        const uint32_t mw = w >> m ? w >> m : 1, mh = h >> m ? h >> m : 1;
        HostBuffer res;
        if (!make_host_buffer(&res, (VkDeviceSize)mw * mh * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return 0;
        VkDescriptorSet set;
        VkDescriptorSetAllocateInfo dsi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &set_layout};
        if (vkAllocateDescriptorSets(dev, &dsi, &set) != VK_SUCCESS) return 0;
        VkDescriptorImageInfo di = {sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo bi = {res.buf, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet wr[2] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &di},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi}};
        vkUpdateDescriptorSets(dev, 2, wr, 0, NULL);
        cb = begin_cmd();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_layout, 0, 1, &set, 0, NULL);
        int32_t pc[4] = {(int32_t)mw, (int32_t)mh, (int32_t)m, 0};
        vkCmdPushConstants(cb, pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, pc);
        vkCmdDispatch(cb, (mw + 7) / 8, (mh + 7) / 8, 1);
        if (!submit_wait(cb)) return 0;
        out[m] = malloc((size_t)mw * mh * 16);
        memcpy(out[m], res.ptr, (size_t)mw * mh * 16);
    }
    return 1;
}

static int make_device(int use_features2, int want_astc) {
    IFN(vkCreateDevice);
    IFN(vkGetDeviceProcAddr);
    gdpa = (PFN_vkGetDeviceProcAddr)vkGetDeviceProcAddr;
    float prio = 1.f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &prio};
    VkPhysicalDeviceFeatures wanted = {.textureCompressionASTC_LDR = want_astc ? VK_TRUE : VK_FALSE};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .features = wanted};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
    if (use_features2 == 1) di.pNext = &f2; else if (use_features2 == 0) di.pEnabledFeatures = &wanted;  /* 2: no features */
    return vkCreateDevice(phys, &di, NULL, &dev) == VK_SUCCESS;
}

#ifndef VK_ASTC_HELPERS_ONLY

/* One void-extent ASTC block of colour (r, g, b, a): each component is the 16-bit value v * 257. */
static void void_block(uint8_t* out, int r, int g, int b, int a) {
    /* Bits 8..0 are 111111100, bit 9 (HDR) is 0, bits 10 and 11 are reserved ones, the 52 coordinate bits are all
     * ones (no extent), then R, G, B, A as UNORM16. */
    out[0] = 0xFC; out[1] = 0xFD;
    memset(out + 2, 0xFF, 6);
    const int comp[4] = {r, g, b, a};
    for (int c = 0; c < 4; c++) { uint16_t v = (uint16_t)(comp[c] * 257); out[8 + 2 * c] = (uint8_t)v; out[9 + 2 * c] = (uint8_t)(v >> 8); }
}

/* Colours on one line in RGB, varying with the block and the mip. */
static void block_color(int bx, int by, int mip, int* rgb) {
    const int a = (bx * 7 + by * 13 + mip * 29) % 40;
    rgb[0] = a + 20; rgb[1] = a / 2 + 60; rgb[2] = 200 - a;
}

/* An sRGB image samples as linear light; the error is measured back in the encoded values the blocks hold. */
static float linear_to_srgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.f / 2.4f) - 0.055f; }

static void check_image(const char* name, VkFormat format, int srgb, int bw, int bh, uint32_t w, uint32_t h, uint32_t mips) {
    size_t offsets[16], total = 0;
    for (uint32_t m = 0; m < mips; m++) {
        offsets[m] = total;
        const uint32_t mw = w >> m ? w >> m : 1, mh = h >> m ? h >> m : 1;
        total += (size_t)((mw + bw - 1) / bw) * ((mh + bh - 1) / bh) * 16;
    }
    uint8_t* data = calloc(1, total);
    for (uint32_t m = 0; m < mips; m++) {
        const uint32_t mw = w >> m ? w >> m : 1, mh = h >> m ? h >> m : 1;
        const int nx = (mw + bw - 1) / bw, ny = (mh + bh - 1) / bh;
        for (int by = 0; by < ny; by++)
            for (int bx = 0; bx < nx; bx++) {
                int c[3];
                block_color(bx, by, m, c);
                void_block(data + offsets[m] + ((size_t)by * nx + bx) * 16, c[0], c[1], c[2], 255);
            }
    }
    float* out[16] = {0};
    char label[160];
    snprintf(label, sizeof label, "%s uploads and samples", name);
    const int ok = upload_and_fetch(format, w, h, mips, data, total, offsets, out);
    CHECK(ok, label);
    if (ok) {
        double worst = 0;
        for (uint32_t m = 0; m < mips; m++) {
            const uint32_t mw = w >> m ? w >> m : 1, mh = h >> m ? h >> m : 1;
            for (uint32_t y = 0; y < mh; y++)
                for (uint32_t x = 0; x < mw; x++) {
                    int c[3];
                    block_color((int)(x / bw), (int)(y / bh), (int)m, c);
                    for (int k = 0; k < 3; k++) {
                        float got = out[m][(y * mw + x) * 4 + k];
                        if (srgb) got = linear_to_srgb(got);
                        const double err = fabs(got - c[k] / 255.f) * 255.0;
                        if (err > worst) worst = err;
                    }
                    if (fabs(out[m][(y * mw + x) * 4 + 3] - 1.f) > 0.01f) worst = 1000;
                }
        }
        snprintf(label, sizeof label, "%s texels match (worst error %.2f levels)", name, worst);
        CHECK(worst <= 3.5, label);
    }
    for (int m = 0; m < 16; m++) free(out[m]);
    free(data);
}

int main(void) {
    void* vk = load_lib("libvulkan.so.1");
    if (!vk) { printf("SKIP: no host libvulkan\n"); return 77; }
    gipa = (PFN_vkGetInstanceProcAddr)load_sym(vk, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)load_sym(vk, "vkCreateInstance");
    if (!gipa || !vkCreateInstance) { printf("SKIP: no vkCreateInstance\n"); return 77; }
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "bifrost astc", .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkResult r = vkCreateInstance(&ici, NULL, &instance);
    if (r == VK_ERROR_INCOMPATIBLE_DRIVER) { printf("SKIP: host has no Vulkan driver\n"); return 77; }
    CHECK(r == VK_SUCCESS, "vkCreateInstance");
    if (r != VK_SUCCESS) return 1;

    IFN(vkEnumeratePhysicalDevices); IFN(vkGetPhysicalDeviceFeatures); IFN(vkGetPhysicalDeviceFeatures2);
    IFN(vkGetPhysicalDeviceFormatProperties); IFN(vkGetPhysicalDeviceFormatProperties2); IFN(vkGetPhysicalDeviceImageFormatProperties);
    IFN(vkGetPhysicalDeviceImageFormatProperties2); IFN(vkGetPhysicalDeviceQueueFamilyProperties); IFN(vkGetPhysicalDeviceMemoryProperties);
    uint32_t n = 1;
    r = vkEnumeratePhysicalDevices(instance, &n, &phys);
    if (n == 0 || (r != VK_SUCCESS && r != VK_INCOMPLETE)) { printf("SKIP: no physical device\n"); return 77; }
    vkGetPhysicalDeviceMemoryProperties(phys, &memprops);

    /* The feature, in both query forms. A host with neither ASTC nor BC7 cannot offer it: skip. */
    VkPhysicalDeviceFeatures f = {0};
    vkGetPhysicalDeviceFeatures(phys, &f);
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    vkGetPhysicalDeviceFeatures2(phys, &f2);
    if (!f.textureCompressionASTC_LDR && !f2.features.textureCompressionASTC_LDR) { printf("SKIP: neither ASTC nor BC7 on this host\n"); return 77; }
    CHECK(f.textureCompressionASTC_LDR, "vkGetPhysicalDeviceFeatures reports textureCompressionASTC_LDR");
    CHECK(f2.features.textureCompressionASTC_LDR, "vkGetPhysicalDeviceFeatures2 reports textureCompressionASTC_LDR");

    /* The formats the engines ask about, in the plain and the "2" forms, and as image formats. */
    const VkFormat formats[] = {VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_ASTC_6x6_UNORM_BLOCK, VK_FORMAT_ASTC_8x8_SRGB_BLOCK, VK_FORMAT_ASTC_12x12_SRGB_BLOCK};
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    int props_ok = 1, props2_ok = 1, image_ok = 1, image2_ok = 1;
    for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        VkFormatProperties p = {0};
        vkGetPhysicalDeviceFormatProperties(phys, formats[i], &p);
        props_ok &= (p.optimalTilingFeatures & need) == need;
        VkFormatProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
        vkGetPhysicalDeviceFormatProperties2(phys, formats[i], &p2);
        props2_ok &= (p2.formatProperties.optimalTilingFeatures & need) == need;
        VkImageFormatProperties ip;
        image_ok &= vkGetPhysicalDeviceImageFormatProperties(phys, formats[i], VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, &ip) == VK_SUCCESS;
        VkPhysicalDeviceImageFormatInfo2 info = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, .format = formats[i],
            .type = VK_IMAGE_TYPE_2D, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VkImageFormatProperties2 ip2 = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        image2_ok &= vkGetPhysicalDeviceImageFormatProperties2(phys, &info, &ip2) == VK_SUCCESS;
    }
    CHECK(props_ok, "vkGetPhysicalDeviceFormatProperties: ASTC formats can be sampled and copied to");
    CHECK(props2_ok, "vkGetPhysicalDeviceFormatProperties2: the same");
    CHECK(image_ok, "vkGetPhysicalDeviceImageFormatProperties accepts the ASTC formats");
    CHECK(image2_ok, "vkGetPhysicalDeviceImageFormatProperties2 accepts the ASTC formats");

    /* A device created with the feature enabled, both ways an engine does it. */
    uint32_t nfam = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nfam, NULL);
    VkQueueFamilyProperties qf[16];
    if (nfam > 16) nfam = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nfam, qf);
    family = UINT32_MAX;
    for (uint32_t i = 0; i < nfam; i++) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = i; break; }
    if (family == UINT32_MAX) { printf("SKIP: no compute queue\n"); return 77; }
    PFN_vkDestroyDevice destroy_device = NULL;
    static const int kForms[3] = {1, 2, 0};
    static const char* const kFormNames[3] = {"vkCreateDevice with the feature in VkPhysicalDeviceFeatures2",
                                              "vkCreateDevice naming no features",
                                              "vkCreateDevice with the feature in pEnabledFeatures"};
    for (int form = 0; form < 3; form++) {
        int ok = make_device(kForms[form], 1);
        CHECK(ok, kFormNames[form]);
        if (!ok) return 1;
        if (form < 2) {  /* keep the last device for the image tests, destroy the others */
            destroy_device = (PFN_vkDestroyDevice)gdpa(dev, "vkDestroyDevice");
            destroy_device(dev, NULL);
        }
    }
    PFN_vkGetDeviceQueue vkGetDeviceQueue = (PFN_vkGetDeviceQueue)gdpa(dev, "vkGetDeviceQueue");
    vkGetDeviceQueue(dev, family, 0, &queue);
    CHECK(make_pipeline(), "compute pipeline for fetching texels");

    check_image("ASTC 4x4 UNORM 16x16, 2 mips", VK_FORMAT_ASTC_4x4_UNORM_BLOCK, 0, 4, 4, 16, 16, 2);
    check_image("ASTC 6x6 UNORM 30x18, 2 mips (edge blocks)", VK_FORMAT_ASTC_6x6_UNORM_BLOCK, 0, 6, 6, 30, 18, 2);
    check_image("ASTC 8x8 sRGB 16x16", VK_FORMAT_ASTC_8x8_SRGB_BLOCK, 1, 8, 8, 16, 16, 1);
    check_image("ASTC 10x5 UNORM 40x20", VK_FORMAT_ASTC_10x5_UNORM_BLOCK, 0, 10, 5, 40, 20, 1);

    printf(g_fail ? "VULKAN ASTC TEST FAILED\n" : "VULKAN ASTC TEST PASSED\n");
    return g_fail != 0;
}
#endif
