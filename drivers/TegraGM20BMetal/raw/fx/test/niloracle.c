// SPDX-License-Identifier: MIT
// raw/fx host test (r12-cafx): Mesa NIL's own answer for a mipmapped colour image's layout, asked through the NVK host twin
// (libvulkan_nouveau.dylib, macOS build of this build's NVK) over the mock GM20B (raw/pack/mock_iokit.c). NVK computes an
// optimal-tiled image's levels with NIL (nvk_image_init -> nil_image_init) and reports them in vkGetImageSubresourceLayout
// (nvk_get_image_subresource_layout: nil_image_level_layer_offset_B, levels[l].row_stride_B), and the image's size and alignment
// in vkGetImageMemoryRequirements. Nothing executes on a GPU.
// usage: niloracle <host libvulkan_nouveau.dylib> <format: 44 BGRA8 | 37 RGBA8 | 97 RGBA16F> WxHxL...
// prints one line per image: "W H L size align | off,rowPitch,size ..." (mipcheck compares fx's layout with it)
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define GET(inst, name) ((PFN_##name)gipa(inst, #name))

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <libvulkan_nouveau.dylib> <vk format> WxHxL...\n", argv[0]);
        return 2;
    }
    setenv("NVK_DARWIN_SERVICE", "AGXAccelerator", 0);
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
    if (!gipa) return 1;
    const VkApplicationInfo app = {
        VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "niloracle", 1, "fx", 1, VK_API_VERSION_1_3};
    const VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    VkInstance inst;
    VkResult r = GET(NULL, vkCreateInstance)(&ici, NULL, &inst);
    if (r) {
        fprintf(stderr, "vkCreateInstance %d\n", r);
        return 1;
    }
    uint32_t n = 1;
    VkPhysicalDevice pd;
    r = GET(inst, vkEnumeratePhysicalDevices)(inst, &n, &pd);
    if (r < 0 || !n) {
        fprintf(stderr, "no physical device (%d)\n", r);
        return 1;
    }
    VkPhysicalDeviceProperties props;
    GET(inst, vkGetPhysicalDeviceProperties)(pd, &props);
    fprintf(stderr, "device: %s\n", props.deviceName);
    const float prio = 1;
    const VkDeviceQueueCreateInfo q = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    const VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &q, 0, NULL, 0, NULL, NULL};
    VkDevice dev;
    r = GET(inst, vkCreateDevice)(pd, &dci, NULL, &dev);
    if (r) {
        fprintf(stderr, "vkCreateDevice %d\n", r);
        return 1;
    }
    PFN_vkGetDeviceProcAddr gdpa = GET(inst, vkGetDeviceProcAddr);
    PFN_vkCreateImage createImage = (PFN_vkCreateImage)gdpa(dev, "vkCreateImage");
    PFN_vkDestroyImage destroyImage = (PFN_vkDestroyImage)gdpa(dev, "vkDestroyImage");
    PFN_vkGetImageMemoryRequirements req = (PFN_vkGetImageMemoryRequirements)gdpa(dev, "vkGetImageMemoryRequirements");
    PFN_vkGetImageSubresourceLayout sub = (PFN_vkGetImageSubresourceLayout)gdpa(dev, "vkGetImageSubresourceLayout");
    const VkFormat format = (VkFormat)atoi(argv[2]);
    for (int a = 3; a < argc; a++) {
        unsigned w, h, l;
        if (sscanf(argv[a], "%ux%ux%u", &w, &h, &l) != 3) continue;
        const VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                      NULL,
                                      0,
                                      VK_IMAGE_TYPE_2D,
                                      format,
                                      {w, h, 1},
                                      l,
                                      1,
                                      VK_SAMPLE_COUNT_1_BIT,
                                      VK_IMAGE_TILING_OPTIMAL,
                                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                      VK_SHARING_MODE_EXCLUSIVE,
                                      0,
                                      NULL,
                                      VK_IMAGE_LAYOUT_UNDEFINED};
        VkImage img;
        r = createImage(dev, &ci, NULL, &img);
        if (r) {
            printf("%u %u %u error %d\n", w, h, l, r);
            continue;
        }
        VkMemoryRequirements mr;
        req(dev, img, &mr);
        printf("%u %u %u %llu %llu |", w, h, l, (unsigned long long)mr.size, (unsigned long long)mr.alignment);
        for (unsigned i = 0; i < l; i++) {
            const VkImageSubresource s = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0};
            VkSubresourceLayout lay;
            sub(dev, img, &s, &lay);
            printf(" %llu,%llu,%llu", (unsigned long long)lay.offset, (unsigned long long)lay.rowPitch,
                   (unsigned long long)lay.size);
        }
        printf("\n");
        destroyImage(dev, img, NULL);
    }
    return 0;
}
