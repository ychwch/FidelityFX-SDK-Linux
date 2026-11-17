#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <limits>
#include <vector>

#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>
#include <vulkan/vulkan.h>

template <typename T>
T sym(void* lib, const char* name) {
    return reinterpret_cast<T>(dlsym(lib, name));
}
struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
};

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
};

uint32_t memory_type(VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags required) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & required) == required)
            return i;
    return UINT32_MAX;
}

bool make_image(VkPhysicalDevice physical, VkDevice device, uint32_t width, uint32_t height, VkFormat format, Image& out) {
    out.info.imageType = VK_IMAGE_TYPE_2D;
    out.info.format = format;
    out.info.extent = {width, height, 1};
    out.info.mipLevels = 1;
    out.info.arrayLayers = 1;
    out.info.samples = VK_SAMPLE_COUNT_1_BIT;
    out.info.tiling = VK_IMAGE_TILING_OPTIMAL;
    out.info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    out.info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    out.info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &out.info, nullptr, &out.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, out.image, &req);
    const uint32_t type = memory_type(physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) return false;
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    return vkAllocateMemory(device, &alloc, nullptr, &out.memory) == VK_SUCCESS &&
           vkBindImageMemory(device, out.image, out.memory, 0) == VK_SUCCESS;
}

bool make_readback(VkPhysicalDevice physical, VkDevice device, VkDeviceSize size, Buffer& out) {
    out.size = size;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &info, nullptr, &out.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, out.buffer, &req);
    const uint32_t type = memory_type(physical, req.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) return false;
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    return vkAllocateMemory(device, &alloc, nullptr, &out.memory) == VK_SUCCESS &&
           vkBindBufferMemory(device, out.buffer, out.memory, 0) == VK_SUCCESS;
}

FfxApiResource resource(const Image& image, uint32_t state) {
    return ffxApiGetResourceVK((void*)image.image, ffxApiGetImageResourceDescriptionVK(image.image, image.info, 0), state);
}

void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
                VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 image_barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    image_barrier.srcStageMask = src_stage;
    image_barrier.srcAccessMask = src_access;
    image_barrier.dstStageMask = dst_stage;
    image_barrier.dstAccessMask = dst_access;
    image_barrier.oldLayout = old_layout;
    image_barrier.newLayout = new_layout;
    image_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.image = image;
    image_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &image_barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

float half_to_float(uint16_t bits) {
    const unsigned exponent = (bits >> 10) & 0x1fu;
    const unsigned mantissa = bits & 0x3ffu;
    float value;
    if (exponent == 0)
        value = std::ldexp((float)mantissa, -24);
    else if (exponent == 0x1f)
        value = mantissa ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
    else
        value = std::ldexp((float)(0x400u + mantissa), (int)exponent - 25);
    return bits & 0x8000u ? -value : value;
}

bool native_color_ok(const void* mapped, size_t pixels) {
    const auto* half = static_cast<const uint16_t*>(mapped);
    const std::array<double, 4> expected{0.25, 0.5, 0.75, 1.0};
    std::array<double, 4> sum{};
    std::array<float, 4> low{INFINITY, INFINITY, INFINITY, INFINITY};
    std::array<float, 4> high{-INFINITY, -INFINITY, -INFINITY, -INFINITY};
    for (size_t pixel = 0; pixel < pixels; ++pixel) {
        for (size_t channel = 0; channel < 4; ++channel) {
            const float value = half_to_float(half[pixel * 4 + channel]);
            if (!std::isfinite(value)) return false;
            sum[channel] += value;
            low[channel] = std::min(low[channel], value);
            high[channel] = std::max(high[channel], value);
        }
    }
    bool ok = true;
    for (size_t channel = 0; channel < 4; ++channel) {
        const double mean = sum[channel] / pixels;
        std::printf("output channel %zu: mean %.6f, range %.6f..%.6f, expected %.2f\n", channel, mean,
                    low[channel], high[channel], expected[channel]);
        // A constant native-resolution input must survive the temporal pass.
        // This rejects zero/white output and accidental perceptual/linear
        // conversion while allowing small implementation-specific FP16 noise.
        if (std::fabs(mean - expected[channel]) > 0.02) ok = false;
    }
    return ok;
}

int main(int argc, char** argv) {
    if (argc != 2) return std::fprintf(stderr, "usage: %s libamd_fidelityfx_vk.so\n", argv[0]), 2;
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) return std::fprintf(stderr, "dlopen: %s\n", dlerror()), 3;
    const auto create = sym<PfnFfxCreateContext>(lib, "ffxCreateContext");
    const auto destroy = sym<PfnFfxDestroyContext>(lib, "ffxDestroyContext");
    const auto dispatch = sym<PfnFfxDispatch>(lib, "ffxDispatch");
    if (!create || !destroy || !dispatch) return std::fprintf(stderr, "missing FFX API exports\n"), 4;

    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "ffx context smoke";
    ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &ai;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) return std::fprintf(stderr, "vkCreateInstance failed\n"), 5;

    uint32_t physical_count = 0;
    vkEnumeratePhysicalDevices(instance, &physical_count, nullptr);
    std::vector<VkPhysicalDevice> physicals(physical_count);
    vkEnumeratePhysicalDevices(instance, &physical_count, physicals.data());
    if (physicals.empty()) return std::fprintf(stderr, "no Vulkan device\n"), 6;
    const VkPhysicalDevice physical = physicals[0];

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < family_count; i++)
        if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = i; break; }
    if (family == UINT32_MAX) return std::fprintf(stderr, "no compute queue\n"), 7;

    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f12.pNext = &f13;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(physical, &features);
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f12;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &features.features;
    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(physical, &dci, nullptr, &device) != VK_SUCCESS) return std::fprintf(stderr, "vkCreateDevice failed\n"), 8;

    ffxCreateBackendVKDesc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.vkDevice = device;
    backend.vkPhysicalDevice = physical;
    backend.vkDeviceProcAddr = vkGetDeviceProcAddr;
    ffxCreateContextDescUpscale desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.header.pNext = &backend.header;
    desc.maxRenderSize = {64, 64};
    desc.maxUpscaleSize = {64, 64};
    desc.fpMessage = [](uint32_t type, const wchar_t* message) {
        std::fwprintf(stderr, L"FFX %u: %ls\n", type, message ? message : L"(null)");
    };
    ffxContext context = nullptr;
    const ffxReturnCode_t made = create(&context, &desc.header, nullptr);
    std::printf("ffxCreateContext: %u, context %p\n", made, context);

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, family, 0, &queue);
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_info.commandBufferCount = 1;
    Image color, depth, motion, output;
    Buffer readback;
    ffxReturnCode_t dispatched = FFX_API_RETURN_ERROR;
    VkResult submitted = VK_ERROR_INITIALIZATION_FAILED;
    if (made == FFX_API_RETURN_OK &&
        vkCreateCommandPool(device, &pool_info, nullptr, &pool) == VK_SUCCESS) {
        cmd_info.commandPool = pool;
        if (vkAllocateCommandBuffers(device, &cmd_info, &cmd) == VK_SUCCESS &&
            make_image(physical, device, 64, 64, VK_FORMAT_R16G16B16A16_SFLOAT, color) &&
            make_image(physical, device, 64, 64, VK_FORMAT_R32_SFLOAT, depth) &&
            make_image(physical, device, 64, 64, VK_FORMAT_R16G16_SFLOAT, motion) &&
            make_image(physical, device, 64, 64, VK_FORMAT_R16G16B16A16_SFLOAT, output) &&
            make_readback(physical, device, 64u * 64u * 8u, readback)) {
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            vkBeginCommandBuffer(cmd, &begin);
            const std::array<Image*, 3> inputs{&color, &depth, &motion};
            const std::array<VkClearColorValue, 3> clears{{{{0.25f, 0.5f, 0.75f, 1.0f}},
                                                          {{1.0f, 0.0f, 0.0f, 0.0f}},
                                                          {{0.0f, 0.0f, 0.0f, 0.0f}}}};
            for (size_t i = 0; i < inputs.size(); ++i) {
                Image* input = inputs[i];
                transition(cmd, input->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                           VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdClearColorImage(cmd, input->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clears[i], 1, &range);
                transition(cmd, input->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
            }
            transition(cmd, output.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                       VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            const VkClearColorValue zero{};
            const VkImageSubresourceRange output_range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cmd, output.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &output_range);
            transition(cmd, output.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                       VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);

            ffxDispatchDescUpscale frame{};
            frame.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
            frame.commandList = (void*)cmd;
            frame.color = resource(color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
            frame.depth = resource(depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
            frame.motionVectors = resource(motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
            frame.output = resource(output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
            frame.motionVectorScale = {64.0f, 64.0f};
            frame.renderSize = {64, 64};
            frame.upscaleSize = {64, 64};
            frame.frameTimeDelta = 16.667f;
            frame.preExposure = 1.0f;
            frame.reset = true;
            frame.cameraNear = 0.1f;
            frame.cameraFar = 1000.0f;
            frame.cameraFovAngleVertical = 1.0f;
            frame.viewSpaceToMetersFactor = 1.0f;
            dispatched = dispatch(&context, &frame.header);
            std::printf("ffxDispatch frame 0 (reset): %u\n", dispatched);
            if (dispatched == FFX_API_RETURN_OK) {
                // Exercise retained temporal history too. The input is
                // constant, so changing to the next Halton jitter must not
                // change its native-resolution colour.
                frame.reset = false;
                frame.jitterOffset = {-0.25f, 1.0f / 6.0f};
                dispatched = dispatch(&context, &frame.header);
                std::printf("ffxDispatch frame 1 (history): %u\n", dispatched);
            }
            if (dispatched == FFX_API_RETURN_OK) {
                // Exercise the optional RCAS pass on retained history. A
                // spatially constant image has no edge to sharpen, so RCAS
                // must preserve the same linear colour checked below.
                frame.jitterOffset = {0.25f, -5.0f / 18.0f};
                frame.enableSharpening = true;
                frame.sharpness = 0.3f;
                dispatched = dispatch(&context, &frame.header);
                std::printf("ffxDispatch frame 2 (history + RCAS 0.3): %u\n", dispatched);
            }
            if (dispatched == FFX_API_RETURN_OK) {
                transition(cmd, output.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                VkBufferImageCopy copy{};
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.imageExtent = {64, 64, 1};
                vkCmdCopyImageToBuffer(cmd, output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
            }
            const VkResult ended = vkEndCommandBuffer(cmd);
            if (ended == VK_SUCCESS && dispatched == FFX_API_RETURN_OK) {
                VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                submit.commandBufferCount = 1;
                submit.pCommandBuffers = &cmd;
                submitted = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
                if (submitted == VK_SUCCESS) submitted = vkQueueWaitIdle(queue);
            } else {
                submitted = ended;
            }
            std::printf("Vulkan submit/wait: %d\n", submitted);
            if (submitted == VK_SUCCESS) {
                void* mapped = nullptr;
                if (vkMapMemory(device, readback.memory, 0, readback.size, 0, &mapped) == VK_SUCCESS) {
                    const auto* bytes = static_cast<const unsigned char*>(mapped);
                    const size_t nonzero = std::count_if(bytes, bytes + readback.size, [](unsigned char value) { return value != 0; });
                    std::printf("output non-zero bytes: %zu/%zu\n", nonzero, (size_t)readback.size);
                    if (!nonzero || !native_color_ok(mapped, 64u * 64u)) submitted = VK_ERROR_UNKNOWN;
                    vkUnmapMemory(device, readback.memory);
                } else {
                    submitted = VK_ERROR_MEMORY_MAP_FAILED;
                }
            }
        } else {
            std::fprintf(stderr, "Vulkan image/command setup failed\n");
        }
    }

    vkDeviceWaitIdle(device);
    const ffxReturnCode_t dropped = context ? destroy(&context, nullptr) : made;
    std::printf("ffxDestroyContext: %u\n", dropped);

    for (Image* image : {&output, &motion, &depth, &color}) {
        if (image->image) vkDestroyImage(device, image->image, nullptr);
        if (image->memory) vkFreeMemory(device, image->memory, nullptr);
    }
    if (readback.buffer) vkDestroyBuffer(device, readback.buffer, nullptr);
    if (readback.memory) vkFreeMemory(device, readback.memory, nullptr);
    if (pool) vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    dlclose(lib);
    return made || dispatched || submitted || dropped ? 9 : 0;
}
