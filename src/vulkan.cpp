#include "vulkan.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <ranges>
#include <source_location>
#include <stdexcept>
#include <utility>

#include <vulkan/vulkan.h>

// Vulkan device, swapchain, and compute passes. This is the private side of the pimpl.
//
// Owner is a move-only RAII handle. A concept requires the destroy function to be
// callable with the parent, the handle, and the allocator callbacks. Copy is deleted
// and move uses exchange so the source keeps an empty handle. The five special
// members agree on that single owner.
//
// source_location records the call site of a failed Vulkan result, and format builds
// the exception text. Atomics use relaxed ordering for a validation-error counter
// that does not publish other data. Designated initializers name the Vulkan fields
// each call sets. Ranges search extension and format lists in place. string_view
// compares extension names without allocating. enumerate pairs a frame slot with
// its index. countr_zero turns a composite-alpha bit mask into a bit index.
// filesystem::path finds the shader directory beside the executable.
//
// Returning a move-only Owner by value lets the compiler move it into the caller.
// std::move transfers a replacement swapchain into the one the frame loop keeps.
// memcpy copies trivially copyable cells and quads. The public destructor is
// defaulted in this file because unique_ptr needs Impl to be complete.

namespace sand {
namespace {

constexpr auto shaderAccess =
    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
constexpr auto computeStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr unsigned framesInFlight = 2;
constexpr unsigned pressureBatches = 12;
constexpr unsigned fluxLimitPasses = 5;
constexpr auto unlimited = std::numeric_limits<std::uint64_t>::max();
static_assert(pressureBatches % 2 == 0);
static_assert(fluxLimitPasses % 2 == 1);

void check(VkResult result, std::source_location loc = std::source_location::current())
{
    // The default argument captures the caller's line, so the message names that call.
    if (result != VK_SUCCESS)
    {
        throw std::runtime_error(std::format("Vulkan returned {} at {}:{}",
                                             static_cast<int>(result), loc.function_name(),
                                             loc.line()));
    }
}

// Parent is the Vulkan object the destroy function needs. Handle is the handle type.
// Destroy is a compile-time function pointer, so each handle kind gets its own Owner.
// The requires-clause drops the template unless Destroy can be called with those arguments.
template <class Parent, class Handle, auto Destroy>
    requires std::invocable<decltype(Destroy), Parent, Handle, const VkAllocationCallbacks*>
class Owner
{
  public:

    // A defaulted constructor owns nothing until put() acquires a handle.
    Owner() = default;

    ~Owner()
    {
        reset();
    }

    // Deleted copy. Two owners would pass the same handle to Destroy.
    Owner(const Owner&) = delete;
    Owner& operator=(const Owner&) = delete;

    // Move takes the handle and is noexcept because copying two handles cannot fail.
    // exchange leaves the source with an empty value.
    Owner(Owner&& other) noexcept : parent_(other.parent_), value_(std::exchange(other.value_, {}))
    {
    }

    Owner& operator=(Owner&& other) noexcept
    {
        // Skip self-move. Releasing first would destroy the handle this object still needs.
        if (this != &other)
        {
            reset();
            parent_ = other.parent_;

            // Install an empty handle in the source and keep the previous value here.
            value_ = std::exchange(other.value_, {});
        }
        return *this;
    }

    [[nodiscard]] Handle get() const
    {
        return value_;
    }

    Handle* put(Parent parent)
    {
        reset();
        parent_ = parent;
        return &value_;
    }

    void reset()
    {
        if (value_)
        {
            // exchange returns the live handle and stores an empty one before Destroy runs.
            Destroy(parent_, std::exchange(value_, {}), nullptr);
        }
    }

  private:
    Parent parent_{};
    Handle value_{};
};

template <class Handle, auto Destroy> using DeviceOwner = Owner<VkDevice, Handle, Destroy>;
using Semaphore = DeviceOwner<VkSemaphore, vkDestroySemaphore>;
using Pipeline = DeviceOwner<VkPipeline, vkDestroyPipeline>;

struct Context
{
    VkInstance instance{};
    VkDebugUtilsMessengerEXT messenger{};
    VkSurfaceKHR surface{};
    VkPhysicalDevice gpu{};
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkDevice device{};
    VkQueue queue{};
    std::uint32_t queueFamily{};
    std::uint32_t timestampBits{};

    // Independent counter. Relaxed ordering is enough because no other data is published with it.
    std::atomic_uint errors{};

    ~Context()
    {
        // Destroy in reverse order of acquisition: device, surface, messenger, instance.
        if (device)
        {
            vkDestroyDevice(device, nullptr);
        }
        if (surface)
        {
            vkDestroySurfaceKHR(instance, surface, nullptr);
        }
        if (messenger)
        {
            auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            destroy(instance, messenger, nullptr);
        }
        if (instance)
        {
            vkDestroyInstance(instance, nullptr);
        }
    }

    void create(HWND window, bool validation);
};

VKAPI_ATTR VkBool32 VKAPI_CALL debugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                            VkDebugUtilsMessageTypeFlagsEXT type,
                                            const VkDebugUtilsMessengerCallbackDataEXT* message,
                                            void* user)
{
    (void)type;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
    {
        // The validation callback can run on another thread. The counter is the only shared field.
        static_cast<Context*>(user)->errors.fetch_add(1, std::memory_order_relaxed);
    }
    std::fprintf(stderr, "Vulkan validation: %s\n", message->pMessage);
    return VK_FALSE;
}

void Context::create(HWND window, bool validation)
{
    // Designated initializers name the fields this call sets. The rest are value-initialized.
    const VkApplicationInfo app{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Falling Sand",
        .applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0),
        .pEngineName = "Falling Sand",
        .apiVersion = VK_API_VERSION_1_3,
    };

    std::vector<const char*> extensions;
    if (window)
    {
        extensions = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    }

    const char* validationLayer = "VK_LAYER_KHRONOS_validation";
    VkDebugUtilsMessengerCreateInfoEXT debug{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = debugMessage,
        .pUserData = this,
    };
    constexpr VkValidationFeatureEnableEXT syncValidation =
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    const VkValidationFeaturesEXT validationFeatures{
        .sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
        .pNext = &debug,
        .enabledValidationFeatureCount = 1,
        .pEnabledValidationFeatures = &syncValidation,
    };
    if (validation)
    {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
    }
    const VkInstanceCreateInfo init{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = validation ? &validationFeatures : nullptr,
        .pApplicationInfo = &app,
        .enabledLayerCount = validation ? 1u : 0u,
        .ppEnabledLayerNames = validation ? &validationLayer : nullptr,
        .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };
    check(vkCreateInstance(&init, nullptr, &instance));
    if (validation)
    {
        auto createDebug = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        check(createDebug(instance, &debug, nullptr, &messenger));
    }
    if (window)
    {
        const VkWin32SurfaceCreateInfoKHR native{
            .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
            .hinstance = GetModuleHandleW(nullptr),
            .hwnd = window,
        };
        check(vkCreateWin32SurfaceKHR(instance, &native, nullptr, &surface));
    }

    std::uint32_t count{};
    check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count);
    check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    int best = 0;
    for (VkPhysicalDevice candidate : devices)
    {
        VkPhysicalDeviceProperties details{};
        vkGetPhysicalDeviceProperties(candidate, &details);
        if (details.apiVersion < VK_API_VERSION_1_3 ||
            details.limits.maxComputeWorkGroupInvocations < 256 ||
            details.limits.maxPerStageDescriptorStorageBuffers < 10)
        {
            continue;
        }
        VkPhysicalDeviceVulkan13Features core13{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        };
        VkPhysicalDeviceFeatures2 features{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &core13,
        };
        vkGetPhysicalDeviceFeatures2(candidate, &features);
        if (!core13.synchronization2 || !core13.dynamicRendering || !core13.maintenance4)
        {
            continue;
        }
        if (surface)
        {
            check(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, nullptr));
            std::vector<VkExtensionProperties> available(count);
            check(
                vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, available.data()));

            // none_of is true when no listed extension has this name. string_view
            // borrows each fixed-size name for the comparison and does not allocate.
            if (std::ranges::none_of(available, [](const VkExtensionProperties& extension) {
                    return std::string_view(extension.extensionName) ==
                           VK_KHR_SWAPCHAIN_EXTENSION_NAME;
                }))
            {
                continue;
            }
        }

        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, queues.data());
        constexpr auto required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            VkBool32 present = VK_TRUE;
            if (surface)
            {
                check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &present));
            }
            const int rank = details.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if ((queues[i].queueFlags & required) == required && present && rank > best)
            {
                gpu = candidate;
                properties = details;
                queueFamily = i;
                timestampBits = queues[i].timestampValidBits;
                best = rank;
            }
        }
    }
    if (!gpu)
    {
        throw std::runtime_error(
            "No Vulkan 1.3 device with graphics, compute, and presentation support.");
    }
    vkGetPhysicalDeviceMemoryProperties(gpu, &memory);

    constexpr float priority = 1.0F;
    const VkDeviceQueueCreateInfo queueInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queueFamily,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const VkPhysicalDeviceVulkan13Features core{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE,
        .maintenance4 = VK_TRUE,
    };
    const char* swap = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    const VkDeviceCreateInfo deviceInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &core,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueInfo,
        .enabledExtensionCount = surface ? 1u : 0u,
        .ppEnabledExtensionNames = surface ? &swap : nullptr,
    };
    check(vkCreateDevice(gpu, &deviceInfo, nullptr, &device));
    vkGetDeviceQueue(device, queueFamily, 0, &queue);
}

struct Buffer
{
    DeviceOwner<VkDeviceMemory, vkFreeMemory> memory;
    DeviceOwner<VkBuffer, vkDestroyBuffer> buffer;
    VkDeviceSize size{};
    VkDevice device{};
    void* mapped{};

    ~Buffer()
    {
        // Unmap while the memory Owner is still alive. Members are destroyed after this body.
        if (mapped)
        {
            vkUnmapMemory(device, memory.get());
        }
    }

    void create(const Context& ctx, VkDeviceSize bytes, VkBufferUsageFlags usage, bool host = false)
    {
        // Record the device, then put() the new buffer and memory into their Owners.
        device = ctx.device;
        size = bytes;
        const VkBufferCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        check(vkCreateBuffer(device, &info, nullptr, buffer.put(device)));
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, buffer.get(), &requirements);
        const VkMemoryPropertyFlags flags =
            host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                 : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        std::uint32_t i = 0;
        for (; i < ctx.memory.memoryTypeCount; ++i)
        {
            if ((requirements.memoryTypeBits & (1u << i)) != 0 &&
                (ctx.memory.memoryTypes[i].propertyFlags & flags) == flags)
            {
                break;
            }
        }
        if (i == ctx.memory.memoryTypeCount)
        {
            throw std::runtime_error("Vulkan memory type unavailable.");
        }
        const VkMemoryAllocateInfo allocation{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex = i,
        };
        check(vkAllocateMemory(device, &allocation, nullptr, memory.put(device)));
        check(vkBindBufferMemory(device, buffer.get(), memory.get(), 0));
        if (host)
        {
            check(vkMapMemory(device, memory.get(), 0, size, 0, &mapped));
        }
    }

    [[nodiscard]] VkDescriptorBufferInfo descriptor() const
    {
        return {buffer.get(), 0, size};
    }
};

struct Swapchain
{
    DeviceOwner<VkSwapchainKHR, vkDestroySwapchainKHR> handle;
    VkExtent2D extent{};
    VkFormat format{};
    std::vector<VkImage> images;
    std::vector<DeviceOwner<VkImageView, vkDestroyImageView>> views;
    std::vector<Semaphore> finished;
    bool captureSupported{};
};

struct FrameSlot
{
    Buffer ui;
    VkDescriptorSet uiSet{};
    VkCommandBuffer commands{};
    DeviceOwner<VkFence, vkDestroyFence> fence;
    Semaphore available;
    bool measured{};
};

struct Push
{
    std::array<std::int32_t, 2> size{worldWidth, worldHeight};
    std::uint32_t state{};
    std::uint32_t tick{};
    std::array<std::uint32_t, 4> operation{};
    std::array<float, 4> brush{};
    std::array<float, 4> canvas{};
    std::array<float, 4> cursor{};
    std::array<float, 4> screen{};
};

// The push-constant block is 96 bytes with screen at offset 80, matching the shaders.
static_assert(sizeof(Push) == 96 && offsetof(Push, screen) == 80);

enum class Kernel : std::size_t
{
    brush,
    reaction,
    grains,
    predict,
    equation,
    pressure,
    project,
    flux,
    limit,
    advect,
    separate
};
constexpr std::array kernelNames{"brush.comp",    "reaction.comp", "grains.comp",  "predict.comp",
                                 "equation.comp", "pressure.comp", "project.comp", "flux.comp",
                                 "limit.comp",    "advect.comp",   "separate.comp"};

constexpr std::size_t cellBytes = worldCells * sizeof(Cell);
constexpr std::size_t flowBytes = worldCells * sizeof(Point);
constexpr std::size_t pressureBytes = worldCells * sizeof(float);
constexpr std::array bufferSizes{cellBytes,          cellBytes,     flowBytes,     flowBytes,
                                 worldCells * 32ull, pressureBytes, pressureBytes, flowBytes,
                                 pressureBytes,      pressureBytes};
constexpr VkShaderStageFlags pushStages =
    VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT;

// Point is eight trivially copyable bytes, so a flow buffer can be memcpy'd as raw storage.
static_assert(sizeof(Point) == 8 && std::is_trivially_copyable_v<Point>);

std::filesystem::path shaderDirectory()
{
    std::wstring name(512, L'\0');
    for (;;)
    {
        DWORD length = GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
        if (!length)
        {
            throw std::runtime_error("Could not resolve the shader directory.");
        }
        if (length < name.size())
        {
            name.resize(length);
            return std::filesystem::path(name).parent_path() / "shaders";
        }
        name.resize(name.size() * 2);
    }
}

// Synchronization2 barrier. Designated fields name the stages and the access masks.
void dependency(VkCommandBuffer cmd, VkPipelineStageFlags2 source, VkAccessFlags2 sourceAccess,
                VkPipelineStageFlags2 target, VkAccessFlags2 targetAccess)
{
    const VkMemoryBarrier2 memory{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = source,
        .srcAccessMask = sourceAccess,
        .dstStageMask = target,
        .dstAccessMask = targetAccess,
    };
    const VkDependencyInfo info{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &memory,
    };
    vkCmdPipelineBarrier2(cmd, &info);
}

void toCompute(VkCommandBuffer cmd)
{
    dependency(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
               computeStage, shaderAccess);
}

void computePass(VkCommandBuffer cmd)
{
    dependency(cmd, computeStage, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, computeStage, shaderAccess);
}

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout,
                  VkImageLayout newLayout, VkPipelineStageFlags2 source,
                  VkAccessFlags2 sourceAccess, VkPipelineStageFlags2 target,
                  VkAccessFlags2 targetAccess)
{
    const VkImageMemoryBarrier2 barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = source,
        .srcAccessMask = sourceAccess,
        .dstStageMask = target,
        .dstAccessMask = targetAccess,
        .oldLayout = oldLayout,
        .newLayout = newLayout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    const VkDependencyInfo info{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &info);
}

constexpr unsigned groups(unsigned items, unsigned groupSize)
{
    return (items + groupSize - 1) / groupSize;
}

void startCommands(VkCommandBuffer cmd)
{
    check(vkResetCommandBuffer(cmd, 0));
    const VkCommandBufferBeginInfo begin{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    check(vkBeginCommandBuffer(cmd, &begin));
}

void dumpBitmap(const std::filesystem::path& path, const Buffer& source, VkExtent2D extent,
                VkFormat format)
{
    std::array<unsigned char, 54> header{};

    // The lambda captures the header by reference and writes one little-endian word.
    const auto set = [&header](std::size_t offset, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i)
        {
            header[offset + i] = static_cast<unsigned char>(value >> (8 * i));
        }
    };
    header[0] = 'B';
    header[1] = 'M';
    set(2, static_cast<std::uint32_t>(header.size() + source.size));
    set(10, static_cast<std::uint32_t>(header.size()));
    set(14, 40);
    set(18, extent.width);
    set(22, 0u - extent.height);
    header[26] = 1;
    header[28] = 32;
    set(34, static_cast<std::uint32_t>(source.size));

    const auto* data = static_cast<const char*>(source.mapped);
    std::vector<char> swizzled;
    if (format == VK_FORMAT_R8G8B8A8_SRGB)
    {
        swizzled.assign(data, data + source.size);
        for (std::size_t i = 0; i < swizzled.size(); i += 4)
        {
            std::swap(swizzled[i], swizzled[i + 2]);
        }
        data = swizzled.data();
    }

    // ofstream owns the screenshot file and closes it when this function returns.
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(header.data()), header.size());
    output.write(data, static_cast<std::streamsize>(source.size));
    output.close();
    if (!output)
    {
        throw std::runtime_error("Could not save screenshot.");
    }
}

} // namespace

struct VulkanGame::Impl
{
    Context ctx;
    std::array<Buffer, bufferSizes.size()> storage;
    Buffer staging;
    DeviceOwner<VkCommandPool, vkDestroyCommandPool> commands;
    DeviceOwner<VkDescriptorSetLayout, vkDestroyDescriptorSetLayout> worldLayout, uiLayout;
    DeviceOwner<VkDescriptorPool, vkDestroyDescriptorPool> descriptors;
    DeviceOwner<VkPipelineLayout, vkDestroyPipelineLayout> pipelineLayout;
    DeviceOwner<VkQueryPool, vkDestroyQueryPool> queries;
    std::array<Pipeline, kernelNames.size()> compute;
    Pipeline worldPipeline, uiPipeline;
    Swapchain swap;
    std::array<FrameSlot, framesInFlight> slots;
    VkDescriptorSet worldSet{};
    VkCommandBuffer immediate{};
    DeviceOwner<VkFence, vkDestroyFence> immediateFence;
    std::filesystem::path shaders{shaderDirectory()};
    Push push;
    Timing lastTiming{};
    unsigned frame{};
    bool outdated{};

    Impl(HWND window, bool validation);
    ~Impl();
    void setupDescriptors(bool windowed);
    void resize(int width, int height);
    void makeGraphics();
    [[nodiscard]] DeviceOwner<VkShaderModule, vkDestroyShaderModule> shader(
        std::string_view name) const;
    [[nodiscard]] Pipeline graphics(std::string_view vertex, std::string_view frag) const;
    void dispatch(VkCommandBuffer cmd, Kernel kernel, unsigned x, unsigned y);
    void beginCompute(VkCommandBuffer cmd);
    void recordBrush(VkCommandBuffer cmd, const Stroke& stroke);
    void recordSim(VkCommandBuffer cmd, unsigned ticks);
    bool draw(const DrawFrame& params);
    void recordDraw(VkCommandBuffer cmd, unsigned imageIndex, const DrawFrame& params,
                    const FrameSlot& slot, const Buffer* capture);
    void readTiming(const FrameSlot& slot, std::uint32_t first);
    void finishImmediate();
    std::vector<Cell> snapshot();
    FlowSnapshot flowSnapshot();
};

VulkanGame::Impl::Impl(HWND window, bool validation)
{
    ctx.create(window, validation);
    const VkCommandPoolCreateInfo commandInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = ctx.queueFamily,
    };
    check(vkCreateCommandPool(ctx.device, &commandInfo, nullptr, commands.put(ctx.device)));
    std::array<VkCommandBuffer, framesInFlight + 1> buffers{};
    const VkCommandBufferAllocateInfo allocate{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = commands.get(),
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = window ? static_cast<std::uint32_t>(buffers.size()) : 1u,
    };
    check(vkAllocateCommandBuffers(ctx.device, &allocate, buffers.data()));
    immediate = buffers.front();
    const VkFenceCreateInfo immediateInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(vkCreateFence(ctx.device, &immediateInfo, nullptr, immediateFence.put(ctx.device)));

    constexpr auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    for (std::size_t i = 0; i < storage.size(); ++i)
    {
        storage[i].create(ctx, bufferSizes[i], usage);
    }
    staging.create(ctx, std::max(cellBytes, flowBytes * 2),
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);

    if (window)
    {
        constexpr VkFenceCreateInfo fenceInfo{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        constexpr VkSemaphoreCreateInfo semInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

        // enumerate names the slot index. The command buffer for that slot sits one past immediate.
        for (auto [i, slot] : std::views::enumerate(slots))
        {
            slot.commands = buffers[static_cast<std::size_t>(i) + 1];
            check(vkCreateFence(ctx.device, &fenceInfo, nullptr, slot.fence.put(ctx.device)));
            check(vkCreateSemaphore(ctx.device, &semInfo, nullptr, slot.available.put(ctx.device)));
            slot.ui.create(ctx, sizeof(Quad) * maxQuads, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        }
        if (ctx.timestampBits)
        {
            const VkQueryPoolCreateInfo query{
                .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .queryType = VK_QUERY_TYPE_TIMESTAMP,
                .queryCount = 3 * framesInFlight,
            };
            check(vkCreateQueryPool(ctx.device, &query, nullptr, queries.put(ctx.device)));
        }
    }

    setupDescriptors(window != nullptr);
    for (std::size_t i = 0; i < compute.size(); ++i)
    {
        auto module = shader(kernelNames[i]);
        const VkComputePipelineCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage =
                {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                    .module = module.get(),
                    .pName = "main",
                },
            .layout = pipelineLayout.get(),
        };
        check(vkCreateComputePipelines(ctx.device, {}, 1, &info, nullptr,
                                       compute[i].put(ctx.device)));
    }
}

VulkanGame::Impl::~Impl()
{
    vkDeviceWaitIdle(ctx.device);
}

void VulkanGame::Impl::setupDescriptors(bool windowed)
{
    // The binding array matches the storage buffers. Its extent is the buffer count.
    std::array<VkDescriptorSetLayoutBinding, bufferSizes.size()> bindings{};
    for (std::uint32_t i = 0; i < bindings.size(); ++i)
    {
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                       VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    }
    VkDescriptorSetLayoutCreateInfo setInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<std::uint32_t>(bindings.size()),
        .pBindings = bindings.data(),
    };
    check(vkCreateDescriptorSetLayout(ctx.device, &setInfo, nullptr, worldLayout.put(ctx.device)));
    const VkDescriptorSetLayoutBinding uiBinding{
        0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr,
    };
    setInfo.bindingCount = 1;
    setInfo.pBindings = &uiBinding;
    check(vkCreateDescriptorSetLayout(ctx.device, &setInfo, nullptr, uiLayout.put(ctx.device)));

    const std::array layouts{worldLayout.get(), uiLayout.get()};
    const VkPushConstantRange range{pushStages, 0, sizeof(Push)};
    const VkPipelineLayoutCreateInfo layout{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = static_cast<std::uint32_t>(layouts.size()),
        .pSetLayouts = layouts.data(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    };
    check(vkCreatePipelineLayout(ctx.device, &layout, nullptr, pipelineLayout.put(ctx.device)));

    const VkDescriptorPoolSize poolSize{
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        static_cast<std::uint32_t>(bindings.size()) + (windowed ? framesInFlight : 0u),
    };
    const VkDescriptorPoolCreateInfo pool{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1 + (windowed ? framesInFlight : 0u),
        .poolSizeCount = 1,
        .pPoolSizes = &poolSize,
    };
    check(vkCreateDescriptorPool(ctx.device, &pool, nullptr, descriptors.put(ctx.device)));
    VkDescriptorSetAllocateInfo allocate{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptors.get(),
        .descriptorSetCount = 1,
        .pSetLayouts = layouts.data(),
    };
    check(vkAllocateDescriptorSets(ctx.device, &allocate, &worldSet));

    // Captures this so the write can use the device. The lambda lives for this function.
    const auto update = [this](VkDescriptorSet set, std::uint32_t binding, const Buffer& buffer) {
        const auto info = buffer.descriptor();
        const VkWriteDescriptorSet write{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = binding,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &info,
        };
        vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);
    };
    for (std::uint32_t i = 0; i < storage.size(); ++i)
    {
        update(worldSet, i, storage[i]);
    }
    if (windowed)
    {
        allocate.pSetLayouts = layouts.data() + 1;
        for (auto& slot : slots)
        {
            check(vkAllocateDescriptorSets(ctx.device, &allocate, &slot.uiSet));
            update(slot.uiSet, 0, slot.ui);
        }
    }
}

DeviceOwner<VkShaderModule, vkDestroyShaderModule> VulkanGame::Impl::shader(
    std::string_view name) const
{
    // string_view borrows the shader name. format and path build the .spv location.
    // ifstream owns the file, opened at the end so the size is one tellg.
    const auto file = shaders / std::format("{}.spv", name);
    std::ifstream input(file, std::ios::binary | std::ios::ate);
    const auto end = input.tellg();
    if (end <= 0 || end % 4 != 0)
    {
        throw std::runtime_error(std::format("Could not load shader {}.", file.string()));
    }
    std::vector<std::uint32_t> words(static_cast<std::size_t>(end) / sizeof(std::uint32_t));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(words.data()), end))
    {
        throw std::runtime_error(std::format("Could not read shader {}.", file.string()));
    }
    const VkShaderModuleCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = words.size() * sizeof(std::uint32_t),
        .pCode = words.data(),
    };
    DeviceOwner<VkShaderModule, vkDestroyShaderModule> module;
    check(vkCreateShaderModule(ctx.device, &info, nullptr, module.put(ctx.device)));

    // Return the named owner directly. It is move-only, so the caller receives the handle.
    return module;
}

void VulkanGame::Impl::resize(int width, int height)
{
    // Rebuild the swapchain after the device is idle. The old one is passed in as oldSwapchain.
    check(vkDeviceWaitIdle(ctx.device));
    VkSurfaceCapabilitiesKHR capabilities{};
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.gpu, ctx.surface, &capabilities));
    if (capabilities.currentExtent.width == 0 || capabilities.currentExtent.height == 0)
    {
        return;
    }
    std::uint32_t count{};
    check(vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.gpu, ctx.surface, &count, nullptr));
    std::vector<VkSurfaceFormatKHR> formats(count);
    check(vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.gpu, ctx.surface, &count, formats.data()));

    // find_if returns an iterator. The lambda tests one format and captures nothing.
    auto srgb = std::ranges::find_if(formats, [](VkSurfaceFormatKHR f) {
        return (f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB) &&
               f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    if (srgb == formats.end() &&
        !(formats.size() == 1 && formats.front().format == VK_FORMAT_UNDEFINED))
    {
        throw std::runtime_error("The surface does not provide an sRGB swapchain format.");
    }

    Swapchain replacement;
    replacement.format = srgb == formats.end() ? VK_FORMAT_B8G8R8A8_SRGB : srgb->format;
    replacement.extent = capabilities.currentExtent;
    if (replacement.extent.width == std::numeric_limits<std::uint32_t>::max())
    {
        replacement.extent = {
            std::clamp(static_cast<std::uint32_t>(width), capabilities.minImageExtent.width,
                       capabilities.maxImageExtent.width),
            std::clamp(static_cast<std::uint32_t>(height), capabilities.minImageExtent.height,
                       capabilities.maxImageExtent.height),
        };
    }
    auto presentMode = VK_PRESENT_MODE_FIFO_KHR;
    check(vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.gpu, ctx.surface, &count, nullptr));
    std::vector<VkPresentModeKHR> modes(count);
    check(vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.gpu, ctx.surface, &count, modes.data()));
    for (auto mode : {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR})
    {
        // contains reports whether this present mode is in the list the surface advertised.
        if (std::ranges::contains(modes, mode))
        {
            presentMode = mode;
            break;
        }
    }
    replacement.captureSupported =
        (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    const auto minImages =
        capabilities.maxImageCount == 0
            ? capabilities.minImageCount + 1
            : std::min(capabilities.minImageCount + 1, capabilities.maxImageCount);

    // countr_zero finds the lowest set bit in the mask, which is the alpha mode to request.
    const auto alpha = static_cast<VkCompositeAlphaFlagBitsKHR>(
        1u << std::countr_zero(capabilities.supportedCompositeAlpha));
    const VkSwapchainCreateInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = ctx.surface,
        .minImageCount = minImages,
        .imageFormat = replacement.format,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = replacement.extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      (replacement.captureSupported
                           ? static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
                           : 0u),
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = alpha,
        .presentMode = presentMode,
        .clipped = VK_TRUE,
        .oldSwapchain = swap.handle.get(),
    };
    check(vkCreateSwapchainKHR(ctx.device, &info, nullptr, replacement.handle.put(ctx.device)));
    check(vkGetSwapchainImagesKHR(ctx.device, replacement.handle.get(), &count, nullptr));
    replacement.images.resize(count);
    replacement.views.resize(count);
    replacement.finished.resize(count);
    check(vkGetSwapchainImagesKHR(ctx.device, replacement.handle.get(), &count,
                                  replacement.images.data()));
    const VkSemaphoreCreateInfo semaphore{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const VkImageViewCreateInfo view{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = replacement.images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = replacement.format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        check(vkCreateImageView(ctx.device, &view, nullptr, replacement.views[i].put(ctx.device)));
        check(vkCreateSemaphore(ctx.device, &semaphore, nullptr,
                                replacement.finished[i].put(ctx.device)));
    }
    const bool changed = swap.format != replacement.format;

    // Drop the old views and semaphores before the move, then transfer the new swapchain.
    swap.views.clear();
    swap.finished.clear();
    swap = std::move(replacement);
    if (changed)
    {
        makeGraphics();
    }
    outdated = false;
}

void VulkanGame::Impl::makeGraphics()
{
    worldPipeline = graphics("fullscreen.vert", "world.frag");
    uiPipeline = graphics("ui.vert", "ui.frag");
}

Pipeline VulkanGame::Impl::graphics(std::string_view vertex, std::string_view frag) const
{
    // string_view borrows the shader names. The modules are released at the end of this function.
    auto vs = shader(vertex);
    auto fs = shader(frag);
    const std::array stages{
        VkPipelineShaderStageCreateInfo{.sType =
                                            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                        .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                        .module = vs.get(),
                                        .pName = "main"},
        VkPipelineShaderStageCreateInfo{.sType =
                                            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                        .module = fs.get(),
                                        .pName = "main"},
    };
    const VkPipelineVertexInputStateCreateInfo input{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    const VkPipelineInputAssemblyStateCreateInfo assembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    const VkPipelineViewportStateCreateInfo viewport{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    const VkPipelineRasterizationStateCreateInfo raster{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0F,
    };
    const VkPipelineMultisampleStateCreateInfo msaa{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    const VkPipelineColorBlendAttachmentState channels{
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo color{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &channels,
    };
    const std::array dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size()),
        .pDynamicStates = dynamicStates.data(),
    };
    const VkPipelineRenderingCreateInfo rendering{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &swap.format,
    };
    const VkGraphicsPipelineCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .stageCount = static_cast<std::uint32_t>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &input,
        .pInputAssemblyState = &assembly,
        .pViewportState = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState = &msaa,
        .pColorBlendState = &color,
        .pDynamicState = &dynamic,
        .layout = pipelineLayout.get(),
    };
    Pipeline result;
    check(vkCreateGraphicsPipelines(ctx.device, {}, 1, &info, nullptr, result.put(ctx.device)));

    // Named return of a move-only pipeline. The caller owns the Vulkan pipeline after this.
    return result;
}

void VulkanGame::Impl::beginCompute(VkCommandBuffer cmd)
{
    toCompute(cmd);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout.get(), 0, 1,
                            &worldSet, 0, nullptr);
}

void VulkanGame::Impl::dispatch(VkCommandBuffer cmd, Kernel kernel, unsigned x, unsigned y)
{
    // to_underlying supplies the kernel's integer index into the pipeline array.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      compute[std::to_underlying(kernel)].get());
    vkCmdPushConstants(cmd, pipelineLayout.get(), pushStages, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, x, y, 1);
    computePass(cmd);
}

void VulkanGame::Impl::recordBrush(VkCommandBuffer cmd, const Stroke& stroke)
{
    push.operation = {stroke.stir ? 1u : 0u, 0, std::to_underlying(stroke.material), 0};
    push.brush = {stroke.from.x, stroke.from.y, stroke.to.x, stroke.to.y};
    push.cursor[2] = stroke.radius;

    // Init-capture copies the radius into the lambda. The stroke need not stay alive later.
    const auto covering = [radius = stroke.radius](float a, float b) {
        return static_cast<unsigned>(std::ceil(std::abs(a - b) + 2 * radius)) + 2;
    };
    dispatch(cmd, Kernel::brush, groups(covering(stroke.from.x, stroke.to.x), 16),
             groups(covering(stroke.from.y, stroke.to.y), 8));
}

void VulkanGame::Impl::recordSim(VkCommandBuffer cmd, unsigned ticks)
{
    // One ordinary loop per tick. Each dispatch finishes recording before the next is written.
    constexpr unsigned fullX = groups(worldWidth, 16), fullY = groups(worldHeight, 8);
    for (unsigned step = 0; step < ticks; ++step)
    {
        ++push.tick;
        dispatch(cmd, Kernel::reaction, fullX, fullY);
        push.state ^= 1u;
        dispatch(cmd, Kernel::grains, groups((worldWidth + 2) / 2, 16),
                 groups((worldHeight + 2) / 2, 8));
        dispatch(cmd, Kernel::predict, fullX, fullY);
        dispatch(cmd, Kernel::equation, fullX, fullY);
        for (unsigned pass = 0; pass < pressureBatches; ++pass)
        {
            push.operation[1] = pass & 1u;
            dispatch(cmd, Kernel::pressure, groups(worldWidth, 12), groups(worldHeight, 12));
        }
        dispatch(cmd, Kernel::project, fullX, fullY);
        dispatch(cmd, Kernel::flux, fullX, fullY);
        for (unsigned pass = 0; pass < fluxLimitPasses; ++pass)
        {
            push.operation = {pass, pass & 1u, 0, 0};
            dispatch(cmd, Kernel::limit, fullX, fullY);
        }
        dispatch(cmd, Kernel::advect, fullX, fullY);
        push.state ^= 1u;
        for (unsigned phase = 0; phase < 2; ++phase)
        {
            push.operation[1] = (phase + push.tick) & 1u;
            dispatch(cmd, Kernel::separate, fullX, groups(worldHeight / 2, 8));
        }
    }
}

void VulkanGame::Impl::readTiming(const FrameSlot& slot, std::uint32_t first)
{
    if (!queries.get() || !slot.measured)
    {
        return;
    }
    std::array<std::uint64_t, 3> stamps{};
    check(vkGetQueryPoolResults(ctx.device, queries.get(), first,
                                static_cast<std::uint32_t>(stamps.size()), sizeof(stamps),
                                stamps.data(), sizeof(stamps[0]), VK_QUERY_RESULT_64_BIT));
    const std::uint64_t mask = unlimited >> (64 - ctx.timestampBits);
    const auto milliseconds = [this, mask](std::uint64_t before, std::uint64_t after) {
        return static_cast<double>((after - before) & mask) *
               ctx.properties.limits.timestampPeriod / 1'000'000.0;
    };
    lastTiming = {milliseconds(stamps[0], stamps[1]), milliseconds(stamps[0], stamps[2])};
}

bool VulkanGame::Impl::draw(const DrawFrame& params)
{
    // Two frames in flight. The slot's fence waits until the GPU is done with that slot's buffers.
    if (params.ui.size() > maxQuads)
    {
        throw std::runtime_error("The pixel UI exceeded its quad budget.");
    }
    auto& slot = slots[frame % framesInFlight];
    const auto fence = slot.fence.get();
    check(vkWaitForFences(ctx.device, 1, &fence, VK_TRUE, unlimited));
    readTiming(slot, (frame % framesInFlight) * 3);

    std::uint32_t index{};
    const auto acquired = vkAcquireNextImageKHR(ctx.device, swap.handle.get(), unlimited,
                                                slot.available.get(), {}, &index);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR)
    {
        outdated = true;
        return false;
    }
    if (acquired != VK_SUBOPTIMAL_KHR)
    {
        check(acquired);
    }
    Buffer capture;
    if (!params.capture.empty())
    {
        if (!swap.captureSupported)
        {
            throw std::runtime_error("This swapchain cannot be captured.");
        }
        capture.create(ctx, static_cast<VkDeviceSize>(swap.extent.width) * swap.extent.height * 4,
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    }

    // Quad is trivially copyable. size_bytes() is the span's extent times the element size.
    std::memcpy(slot.ui.mapped, params.ui.data(), params.ui.size_bytes());
    auto cmd = slot.commands;
    startCommands(cmd);
    const unsigned first = (frame % framesInFlight) * 3;
    if (queries.get())
    {
        vkCmdResetQueryPool(cmd, queries.get(), first, 3);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, queries.get(), first);
    }
    if (params.stroke || params.ticks)
    {
        beginCompute(cmd);
        if (params.stroke)
        {
            recordBrush(cmd, *params.stroke);
        }
        recordSim(cmd, params.ticks);
    }
    if (queries.get())
    {
        vkCmdWriteTimestamp2(cmd, computeStage, queries.get(), first + 1);
    }
    recordDraw(cmd, index, params, slot, capture.mapped ? &capture : nullptr);
    if (queries.get())
    {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, queries.get(), first + 2);
        slot.measured = true;
    }
    check(vkEndCommandBuffer(cmd));

    const VkSemaphoreSubmitInfo available{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = slot.available.get(),
        .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    };
    const VkSemaphoreSubmitInfo ready{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = swap.finished[index].get(),
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    const VkCommandBufferSubmitInfo commandsInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = cmd,
    };
    const VkSubmitInfo2 submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = 1,
        .pWaitSemaphoreInfos = &available,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &commandsInfo,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &ready,
    };
    check(vkResetFences(ctx.device, 1, &fence));
    check(vkQueueSubmit2(ctx.queue, 1, &submit, fence));
    const auto swapchain = swap.handle.get();
    const VkPresentInfoKHR present{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &ready.semaphore,
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &index,
    };
    const auto displayed = vkQueuePresentKHR(ctx.queue, &present);
    if (displayed != VK_SUBOPTIMAL_KHR && displayed != VK_ERROR_OUT_OF_DATE_KHR)
    {
        check(displayed);
    }
    outdated = displayed != VK_SUCCESS || acquired == VK_SUBOPTIMAL_KHR;
    if (capture.mapped)
    {
        check(vkWaitForFences(ctx.device, 1, &fence, VK_TRUE, unlimited));
        dumpBitmap(params.capture, capture, swap.extent, swap.format);
    }
    ++frame;
    return true;
}

// Dynamic rendering. The image moves from undefined, to color attachment, then to present.
void VulkanGame::Impl::recordDraw(VkCommandBuffer cmd, unsigned imageIndex, const DrawFrame& p,
                                  const FrameSlot& slot, const Buffer* capture)
{
    dependency(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    const auto image = swap.images[imageIndex];
    constexpr auto colorOutput = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    constexpr auto colors = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    imageBarrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 colorOutput, 0, colorOutput, colors);

    const VkRenderingAttachmentInfo attachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = swap.views[imageIndex].get(),
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.color = {{0, 0, 0, 1}}},
    };
    const VkRect2D bounds{{0, 0}, swap.extent};
    const VkRenderingInfo rendering{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = bounds,
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    };
    vkCmdBeginRendering(cmd, &rendering);
    const auto width = static_cast<float>(swap.extent.width);
    const auto height = static_cast<float>(swap.extent.height);
    const VkViewport vp{0, 0, width, height, 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    const VkRect2D canvas{
        {static_cast<int>(p.canvas.x), static_cast<int>(p.canvas.y)},
        {static_cast<unsigned>(p.canvas.w), static_cast<unsigned>(p.canvas.h)},
    };
    vkCmdSetScissor(cmd, 0, 1, &canvas);
    push.canvas = {p.canvas.x, p.canvas.y, p.canvas.w, p.canvas.h};
    push.cursor = {p.cursor.x, p.cursor.y, p.radius, p.cursorVisible ? 1.0F : 0.0F};
    push.screen = {width, height, p.visualTime, 0};
    vkCmdPushConstants(cmd, pipelineLayout.get(), pushStages, 0, sizeof(push), &push);
    const std::array sets{worldSet, slot.uiSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout.get(), 0,
                            static_cast<std::uint32_t>(sets.size()), sets.data(), 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, worldPipeline.get());
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdSetScissor(cmd, 0, 1, &bounds);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, uiPipeline.get());
    vkCmdDraw(cmd, 6, static_cast<std::uint32_t>(p.ui.size()), 0, 0);
    vkCmdEndRendering(cmd);

    if (capture)
    {
        constexpr auto copy = VK_PIPELINE_STAGE_2_COPY_BIT;
        imageBarrier(cmd, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, colorOutput, colors, copy,
                     VK_ACCESS_2_TRANSFER_READ_BIT);
        const VkBufferImageCopy rectangle{
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {swap.extent.width, swap.extent.height, 1},
        };
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               capture->buffer.get(), 1, &rectangle);
        dependency(cmd, copy, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                   VK_ACCESS_2_HOST_READ_BIT);
        imageBarrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, copy, VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_2_NONE, 0);
    } else
    {
        imageBarrier(cmd, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, colorOutput, colors, VK_PIPELINE_STAGE_2_NONE,
                     0);
    }
}

void VulkanGame::Impl::finishImmediate()
{
    check(vkEndCommandBuffer(immediate));
    const VkCommandBufferSubmitInfo buffer{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = immediate,
    };
    const VkSubmitInfo2 submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &buffer,
    };
    const auto fence = immediateFence.get();
    check(vkResetFences(ctx.device, 1, &fence));
    check(vkQueueSubmit2(ctx.queue, 1, &submit, fence));
    check(vkWaitForFences(ctx.device, 1, &fence, VK_TRUE, unlimited));
}

std::vector<Cell> VulkanGame::Impl::snapshot()
{
    // Copy the device buffer to host staging, then memcpy into an owned vector.
    startCommands(immediate);
    dependency(immediate, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    const VkBufferCopy region{0, 0, cellBytes};
    vkCmdCopyBuffer(immediate, storage[push.state].buffer.get(), staging.buffer.get(), 1, &region);
    dependency(immediate, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    finishImmediate();

    std::vector<Cell> result(worldCells);
    std::memcpy(result.data(), staging.mapped, cellBytes);

    // Return the named vector directly so it can be constructed in the caller.
    return result;
}

FlowSnapshot VulkanGame::Impl::flowSnapshot()
{
    startCommands(immediate);
    dependency(immediate, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    const VkBufferCopy projected{0, 0, flowBytes}, predicted{0, flowBytes, flowBytes};
    vkCmdCopyBuffer(immediate, storage[2].buffer.get(), staging.buffer.get(), 1, &projected);
    vkCmdCopyBuffer(immediate, storage[3].buffer.get(), staging.buffer.get(), 1, &predicted);
    dependency(immediate, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    finishImmediate();

    FlowSnapshot result{std::vector<Point>(worldCells), std::vector<Point>(worldCells)};

    // std::byte is the pointer type for raw storage. The Point arrays are filled by memcpy.
    const auto* bytes = static_cast<const std::byte*>(staging.mapped);
    std::memcpy(result.projected.data(), bytes, flowBytes);
    std::memcpy(result.predictor.data(), bytes + flowBytes, flowBytes);
    return result;
}

VulkanGame::VulkanGame(HWND window, bool /*benchmark*/, bool validate)
    : impl_(std::make_unique<Impl>(window, validate))
{
}

// Defined here, where Impl is complete. unique_ptr destroys Impl from this destructor.
VulkanGame::~VulkanGame() = default;

std::array<int, 2> VulkanGame::prepare(int width, int height)
{
    auto& self = *impl_;
    if (!self.ctx.surface || width <= 0 || height <= 0)
    {
        return {};
    }
    if (self.outdated || static_cast<int>(self.swap.extent.width) != width ||
        static_cast<int>(self.swap.extent.height) != height)
    {
        self.resize(width, height);
    }
    return {static_cast<int>(self.swap.extent.width), static_cast<int>(self.swap.extent.height)};
}

bool VulkanGame::draw(const DrawFrame& frame)
{
    return impl_->draw(frame);
}

void VulkanGame::replaceWorld(std::span<const Cell> cells)
{
    // The span borrows the caller's cells. Cell is trivially copyable, so memcpy is defined.
    auto& self = *impl_;
    if (cells.size() != worldCells)
    {
        throw std::runtime_error("World dimensions do not match.");
    }
    std::memcpy(self.staging.mapped, cells.data(), cellBytes);
    auto cmd = self.immediate;
    startCommands(cmd);
    dependency(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
               VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkBufferCopy copy{0, 0, cellBytes};
    vkCmdCopyBuffer(cmd, self.staging.buffer.get(), self.storage[0].buffer.get(), 1, &copy);
    for (std::size_t index : {2u, 3u, 5u, 6u})
    {
        vkCmdFillBuffer(cmd, self.storage[index].buffer.get(), 0, VK_WHOLE_SIZE, 0);
    }
    self.finishImmediate();
    self.push.state = 0;
    self.push.tick = 0;
}

std::vector<Cell> VulkanGame::snapshot()
{
    return impl_->snapshot();
}

FlowSnapshot VulkanGame::flowSnapshot()
{
    return impl_->flowSnapshot();
}

void VulkanGame::simulate(unsigned ticks)
{
    auto& self = *impl_;
    while (ticks != 0)
    {
        auto cmd = self.immediate;
        const unsigned batch = std::min(ticks, 24u);
        startCommands(cmd);
        self.beginCompute(cmd);
        self.recordSim(cmd, batch);
        self.finishImmediate();
        ticks -= batch;
    }
}

void VulkanGame::paint(const Stroke& stroke)
{
    auto& self = *impl_;
    auto cmd = self.immediate;
    startCommands(cmd);
    self.beginCompute(cmd);
    self.recordBrush(cmd, stroke);
    self.finishImmediate();
}

void VulkanGame::wait()
{
    check(vkDeviceWaitIdle(impl_->ctx.device));
}

Timing VulkanGame::timing() const
{
    return impl_->lastTiming;
}

std::string_view VulkanGame::deviceName() const
{
    // The view borrows the device-name array inside Impl. It stays valid while the game does.
    return impl_->ctx.properties.deviceName;
}

unsigned VulkanGame::validationErrors() const
{
    // Same relaxed counter the validation callback increments. No other payload is attached.
    return impl_->ctx.errors.load(std::memory_order_relaxed);
}

} // namespace sand
