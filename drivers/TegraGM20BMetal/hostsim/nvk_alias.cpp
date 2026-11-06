// SPDX-License-Identifier: MIT
// The raw GM20B path's memory as NVK sees it in place: a chunk the raw path bound on NVK's connection (raw_kernel.h), handed
// to NVK as foreign memory (switch_nvk_import_foreign) and viewed by a Vulkan image of the raw texture's shape:
//  - pitch: a pool chunk (PTE kind 0) and a VK_IMAGE_TILING_LINEAR image at a span inside it, as wide as the raw texture's
//    rows (m2v's texture in place takes the row pitch itself);
//  - block-linear: a mip chain's chunk (fx::kBlockLinearKind) and a VK_IMAGE_TILING_OPTIMAL image of the chain's format, size
//    and levels, whose texels raw/fx placed (and NVK writes back where raw/fx reads them).
// For each: NVK's GPU address of the memory is the raw path's VA, NIL's layout is the raw path's (VK_EXT_host_image_copy reads
// and writes through it on the CPU), the view costs no BoImport, BoAlloc or VmBind, and freeing it drops the raw path's
// lease with no BoFree or VmBind.
// usage: nvk_alias <host libvulkan_nouveau.dylib>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../raw/fx/fx_mip.h"
#include "../raw/runtime/raw_kernel.h"
#include "TegraGM20BABI.h"

extern "C" unsigned long mock_iokit_calls(unsigned method);

namespace {
int failures, holds, drops;

void expect(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "OK  " : "FAIL", what);
    failures += !ok;
}

void hold(void *)
{
    holds++;
}

void drop(void *)
{
    drops++;
}

uint8_t texel(uint32_t level, uint32_t x, uint32_t y, uint32_t c, uint32_t seed)
{
    return uint8_t(level * 61u + x * 7u + y * 13u + c * 29u + seed);
}

std::vector<uint8_t> image(uint32_t w, uint32_t h, uint32_t level, uint32_t seed)
{
    std::vector<uint8_t> v(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            for (uint32_t c = 0; c < 4; c++)
                v[(size_t(y) * w + x) * 4 + c] = texel(level, x, y, c, seed);
    return v;
}

struct Calls {
    unsigned long imports, allocs, binds, frees;

    static Calls now()
    {
        return {mock_iokit_calls(TegraGM20BMethodBoImport), mock_iokit_calls(TegraGM20BMethodBoAlloc),
                mock_iokit_calls(TegraGM20BMethodVmBind), mock_iokit_calls(TegraGM20BMethodBoFree)};
    }

    bool operator==(const Calls &o) const
    {
        return imports == o.imports && allocs == o.allocs && binds == o.binds && frees == o.frees;
    }
};

#define VK_FN(f) PFN_##f f = nullptr

struct Vk {
    using Foreign = void (*)(const void *, uint64_t, uint64_t, uint64_t, uint32_t, void (*)(void *), void (*)(void *),
                             void *);
    Foreign foreign = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    VK_FN(vkGetMemoryHostPointerPropertiesEXT);
    VK_FN(vkAllocateMemory);
    VK_FN(vkFreeMemory);
    VK_FN(vkCreateImage);
    VK_FN(vkDestroyImage);
    VK_FN(vkGetImageMemoryRequirements);
    VK_FN(vkGetImageSubresourceLayout);
    VK_FN(vkBindImageMemory);
    VK_FN(vkTransitionImageLayoutEXT);
    VK_FN(vkCopyImageToMemoryEXT);
    VK_FN(vkCopyMemoryToImageEXT);
    VK_FN(vkCreateBuffer);
    VK_FN(vkDestroyBuffer);
    VK_FN(vkBindBufferMemory);
    VK_FN(vkGetBufferDeviceAddress);

    bool open(const char *path);
    // NVK's memory over [cpu, cpu + bytes) of `bo` (whose first byte is `base`), as the frontend makes it
    VkDeviceMemory foreignMemory(const raw::Bo &bo, const uint8_t *base, uint8_t *cpu, uint64_t bytes, uint32_t kind);
    uint64_t address(VkDeviceMemory memory, uint64_t bytes);
    bool read(VkImage image, uint32_t level, uint32_t w, uint32_t h, std::vector<uint8_t> &out);
    bool write(VkImage image, uint32_t level, uint32_t w, uint32_t h, const std::vector<uint8_t> &in);
};

bool Vk::open(const char *path)
{
    void *nvk = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    auto gipa = nvk ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(nvk, "vkGetInstanceProcAddr")) : nullptr;
    foreign = nvk ? reinterpret_cast<Foreign>(dlsym(nvk, "switch_nvk_import_foreign")) : nullptr;
    if (!gipa || !foreign)
        return false;
    const auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (createInstance(&ici, nullptr, &instance) != VK_SUCCESS)
        return false;
    const auto enumerate =
        reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
    const auto createDevice = reinterpret_cast<PFN_vkCreateDevice>(gipa(instance, "vkCreateDevice"));
    const auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance, "vkGetDeviceProcAddr"));
    uint32_t count = 1;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    enumerate(instance, &count, &pd);
    VkPhysicalDeviceBufferDeviceAddressFeatures bda = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    bda.bufferDeviceAddress = VK_TRUE;
    VkPhysicalDeviceHostImageCopyFeaturesEXT hic = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES_EXT,
                                                    &bda};
    hic.hostImageCopy = VK_TRUE;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo q = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueCount = 1;
    q.pQueuePriorities = &priority;
    const char *extensions[] = {VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME, VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &hic};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &q;
    dci.enabledExtensionCount = 2;
    dci.ppEnabledExtensionNames = extensions;
    if (!pd || createDevice(pd, &dci, nullptr, &device) != VK_SUCCESS)
        return false;
#define VK_LOAD(f) f = reinterpret_cast<PFN_##f>(gdpa(device, #f))
    VK_LOAD(vkGetMemoryHostPointerPropertiesEXT);
    VK_LOAD(vkAllocateMemory);
    VK_LOAD(vkFreeMemory);
    VK_LOAD(vkCreateImage);
    VK_LOAD(vkDestroyImage);
    VK_LOAD(vkGetImageMemoryRequirements);
    VK_LOAD(vkGetImageSubresourceLayout);
    VK_LOAD(vkBindImageMemory);
    VK_LOAD(vkTransitionImageLayoutEXT);
    VK_LOAD(vkCopyImageToMemoryEXT);
    VK_LOAD(vkCopyMemoryToImageEXT);
    VK_LOAD(vkCreateBuffer);
    VK_LOAD(vkDestroyBuffer);
    VK_LOAD(vkBindBufferMemory);
    VK_LOAD(vkGetBufferDeviceAddress);
    return true;
}

VkDeviceMemory Vk::foreignMemory(const raw::Bo &bo, const uint8_t *base, uint8_t *cpu, uint64_t bytes, uint32_t kind)
{
    VkMemoryHostPointerPropertiesEXT props = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    vkGetMemoryHostPointerPropertiesEXT(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, cpu, &props);
    VkImportMemoryHostPointerInfoEXT import = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    import.pHostPointer = cpu;
    VkMemoryAllocateFlagsInfo addressed = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, &import};
    addressed.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &addressed};
    mai.allocationSize = bytes;
    mai.memoryTypeIndex = uint32_t(__builtin_ctz(props.memoryTypeBits ? props.memoryTypeBits : 1));
    foreign(base, bo.bytes, bo.handle, bo.vaBase, kind, hold, drop, nullptr);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(device, &mai, nullptr, &memory) != VK_SUCCESS)
        memory = VK_NULL_HANDLE;
    foreign(nullptr, 0, 0, 0, 0, nullptr, nullptr, nullptr);
    return memory;
}

uint64_t Vk::address(VkDeviceMemory memory, uint64_t bytes)
{
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer buffer = VK_NULL_HANDLE;
    uint64_t va = 0;
    if (vkCreateBuffer(device, &bi, nullptr, &buffer) == VK_SUCCESS &&
        vkBindBufferMemory(device, buffer, memory, 0) == VK_SUCCESS) {
        VkBufferDeviceAddressInfo ai = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        ai.buffer = buffer;
        va = vkGetBufferDeviceAddress(device, &ai);
    }
    vkDestroyBuffer(device, buffer, nullptr);
    return va;
}

bool Vk::read(VkImage image, uint32_t level, uint32_t w, uint32_t h, std::vector<uint8_t> &out)
{
    out.assign(size_t(w) * h * 4, 0);
    VkImageToMemoryCopyEXT region = {VK_STRUCTURE_TYPE_IMAGE_TO_MEMORY_COPY_EXT};
    region.pHostPointer = out.data();
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
    region.imageExtent = {w, h, 1};
    VkCopyImageToMemoryInfoEXT copy = {VK_STRUCTURE_TYPE_COPY_IMAGE_TO_MEMORY_INFO_EXT};
    copy.srcImage = image;
    copy.srcImageLayout = VK_IMAGE_LAYOUT_GENERAL;
    copy.regionCount = 1;
    copy.pRegions = &region;
    return vkCopyImageToMemoryEXT(device, &copy) == VK_SUCCESS;
}

bool Vk::write(VkImage image, uint32_t level, uint32_t w, uint32_t h, const std::vector<uint8_t> &in)
{
    VkMemoryToImageCopyEXT region = {VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY_EXT};
    region.pHostPointer = in.data();
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
    region.imageExtent = {w, h, 1};
    VkCopyMemoryToImageInfoEXT copy = {VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO_EXT};
    copy.dstImage = image;
    copy.dstImageLayout = VK_IMAGE_LAYOUT_GENERAL;
    copy.regionCount = 1;
    copy.pRegions = &region;
    return vkCopyMemoryToImageEXT(device, &copy) == VK_SUCCESS;
}

// A raw chunk of `bytes`, bound with PTE kind `kind` on the raw path's connection
uint8_t *chunk(raw::Kernel &k, uint64_t bytes, uint32_t kind, raw::Bo &bo)
{
    vm_address_t at = 0;
    vm_allocate(mach_task_self(), &at, bytes, VM_FLAGS_ANYWHERE);
    uint8_t *cpu = reinterpret_cast<uint8_t *>(at);
    expect(k.importPages(cpu, bytes, bo) && k.bindImport(cpu, bo, kind), "the raw path's chunk is bound");
    return cpu;
}

VkImage makeImage(Vk &vk, VkDeviceMemory memory, VkImageTiling tiling, uint32_t w, uint32_t h, uint32_t levels,
                  VkMemoryRequirements &req)
{
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_B8G8R8A8_UNORM;
    ii.extent = {w, h, 1};
    ii.mipLevels = levels;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = tiling;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT;
    VkImage image = VK_NULL_HANDLE;
    if (vk.vkCreateImage(vk.device, &ii, nullptr, &image) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    vk.vkGetImageMemoryRequirements(vk.device, image, &req);
    if (vk.vkBindImageMemory(vk.device, image, memory, 0) != VK_SUCCESS) {
        vk.vkDestroyImage(vk.device, image, nullptr);
        return VK_NULL_HANDLE;
    }
    VkHostImageLayoutTransitionInfoEXT transition = {VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO_EXT};
    transition.image = image;
    transition.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    transition.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    transition.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
    vk.vkTransitionImageLayoutEXT(vk.device, 1, &transition);
    return image;
}

// Frees the view; the lease is dropped once, with no kernel call
void release(Vk &vk, VkImage image, VkDeviceMemory memory, const char *what)
{
    const int dropped = drops;
    const Calls before = Calls::now();
    vk.vkDestroyImage(vk.device, image, nullptr);
    vk.vkFreeMemory(vk.device, memory, nullptr);
    const Calls after = Calls::now();
    char line[200];
    snprintf(line, sizeof line, "%s: freeing the view dropped the lease (%d) with no BoFree (%lu) or VmBind (%lu)",
             what, drops - dropped, after.frees - before.frees, after.binds - before.binds);
    expect(drops - dropped == 1 && after == before, line);
}

// A pool span of a raw pitch texture (rows 256-byte aligned, as raw::Device::createTexture lays them) in a kind-0 chunk
void pitch(Vk &vk, raw::Kernel &k)
{
    const uint32_t rowPitch = 512, w = rowPitch / 4, h = 60;
    const uint64_t chunkBytes = 1 << 20, spanOffset = 0x40000, spanBytes = uint64_t(rowPitch) * h;
    raw::Bo bo;
    uint8_t *base = chunk(k, chunkBytes, 0, bo), *span = base + spanOffset;
    const std::vector<uint8_t> texels = image(w, h, 0, 3);
    memcpy(span, texels.data(), texels.size());
    const Calls before = Calls::now();
    const int held = holds;
    VkDeviceMemory memory = vk.foreignMemory(bo, base, span, (spanBytes + 0x3fff) & ~uint64_t(0x3fff), 0);
    VkMemoryRequirements req = {};
    VkImage image = memory ? makeImage(vk, memory, VK_IMAGE_TILING_LINEAR, w, h, 1, req) : VK_NULL_HANDLE;
    expect(image && holds - held == 1, "pitch: NVK's memory over the span holds the raw path's lease");
    char line[200];
    const uint64_t va = memory ? vk.address(memory, spanBytes) : 0;
    snprintf(line, sizeof line, "pitch: NVK's GPU address of the span is the raw path's VA (%llx, %llx)",
             (unsigned long long)va, (unsigned long long)(bo.vaBase + spanOffset));
    expect(va == bo.vaBase + spanOffset, line);
    VkSubresourceLayout layout = {};
    const VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    if (image)
        vk.vkGetImageSubresourceLayout(vk.device, image, &sub, &layout);
    snprintf(line, sizeof line,
             "pitch: NIL's linear image has the raw texture's row pitch (%llu, %u) at offset 0 (%llu)",
             (unsigned long long)layout.rowPitch, rowPitch, (unsigned long long)layout.offset);
    expect(layout.rowPitch == rowPitch && !layout.offset, line);
    const Calls viewed = Calls::now();
    snprintf(line, sizeof line, "pitch: the view cost no BoImport (%lu), BoAlloc (%lu) or VmBind (%lu)",
             viewed.imports - before.imports, viewed.allocs - before.allocs, viewed.binds - before.binds);
    expect(viewed == before, line);
    std::vector<uint8_t> got;
    expect(image && vk.read(image, 0, w, h, got) && got == texels, "pitch: NVK reads the texels the raw path wrote");
    if (image)
        release(vk, image, memory, "pitch");
    k.free(bo);
}

// A raw mip chain (raw/fx's layout) in a kind-0xFE chunk
void blockLinear(Vk &vk, raw::Kernel &k)
{
    const uint32_t w = 100, h = 60, levels = 7;
    raw::fx::MipChain c;
    expect(raw::fx::mipChainLayout(w, h, levels, 4, c), "block-linear: raw/fx lays out the chain");
    raw::Bo bo;
    uint8_t *base = chunk(k, 1 << 20, raw::fx::kBlockLinearKind, bo);
    for (uint32_t l = 0; l < levels; l++) {
        const std::vector<uint8_t> v = image(c.level[l].width, c.level[l].height, l, 1);
        raw::fx::writeLevel(c, base, l, 0, 0, c.level[l].width, c.level[l].height, v.data(), c.level[l].width * 4);
    }
    const Calls before = Calls::now();
    const int held = holds;
    VkDeviceMemory memory = vk.foreignMemory(bo, base, base, 1 << 20, raw::fx::kBlockLinearKind);
    VkMemoryRequirements req = {};
    VkImage image = memory ? makeImage(vk, memory, VK_IMAGE_TILING_OPTIMAL, w, h, levels, req) : VK_NULL_HANDLE;
    expect(image && holds - held == 1, "block-linear: NVK's memory over the chain holds the raw path's lease");
    char line[200];
    const uint64_t va = memory ? vk.address(memory, c.bytes) : 0;
    snprintf(line, sizeof line, "block-linear: NVK's GPU address of the chain is the raw path's VA (%llx, %llx)",
             (unsigned long long)va, (unsigned long long)bo.vaBase);
    expect(va == bo.vaBase, line);
    snprintf(line, sizeof line, "block-linear: NIL's image is raw/fx's chain size (%llu, %llu)",
             (unsigned long long)req.size, (unsigned long long)c.bytes);
    expect(req.size == c.bytes, line);
    const Calls viewed = Calls::now();
    snprintf(line, sizeof line, "block-linear: the view cost no BoImport (%lu), BoAlloc (%lu) or VmBind (%lu)",
             viewed.imports - before.imports, viewed.allocs - before.allocs, viewed.binds - before.binds);
    expect(viewed == before, line);
    for (uint32_t l = 0; image && l < levels; l++) {
        std::vector<uint8_t> got;
        snprintf(line, sizeof line, "block-linear: level %u (%ux%u): NVK reads the texels raw/fx wrote", l,
                 c.level[l].width, c.level[l].height);
        expect(vk.read(image, l, c.level[l].width, c.level[l].height, got) &&
                   got == ::image(c.level[l].width, c.level[l].height, l, 1),
               line);
    }
    if (image) {
        const uint32_t l = 2;
        const std::vector<uint8_t> v = ::image(c.level[l].width, c.level[l].height, l, 9);
        std::vector<uint8_t> got(v.size(), 0);
        const bool written = vk.write(image, l, c.level[l].width, c.level[l].height, v);
        raw::fx::readLevel(c, base, l, 0, 0, c.level[l].width, c.level[l].height, got.data(), c.level[l].width * 4);
        expect(written && got == v, "block-linear: level 2: raw/fx reads the texels NVK wrote");
        release(vk, image, memory, "block-linear");
    }
    k.free(bo);
}
}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: nvk_alias <host libvulkan_nouveau.dylib>\n");
        return 2;
    }
    Vk vk;
    if (!vk.open(argv[1])) {
        fprintf(stderr, "no Vulkan device over NVK at %s\n", argv[1]);
        return 1;
    }
    raw::Kernel k;
    std::string err;
    expect(k.open(err) && k.shared(), "the raw path's kernel connection is NVK's");
    pitch(vk, k);
    blockLinear(vk, k);
    printf("%s\n", failures ? "FAILED" : "ALL OK");
    return failures ? 1 : 0;
}
