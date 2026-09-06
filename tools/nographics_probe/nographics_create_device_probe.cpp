#include <NoGraphicsAPI/NoGraphicsAPI.hpp>
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

static const char* error_name(gpu::Error error)
{
    switch (error)
    {
    case gpu::Error::none: return "none";
    case gpu::Error::unsupported: return "unsupported";
    case gpu::Error::device_lost: return "device_lost";
    case gpu::Error::driver_error: return "driver_error";
    }
    return "unknown";
}

static bool has_extension(const std::vector<VkExtensionProperties>& extensions, const char* name)
{
    for (const VkExtensionProperties& extension : extensions)
        if (std::strcmp(extension.extensionName, name) == 0)
            return true;
    return false;
}

static void print_extension_matrix()
{
    uint32_t loader_version = VK_API_VERSION_1_0;
    const VkResult loader_version_result = vkEnumerateInstanceVersion(&loader_version);
    if (loader_version_result != VK_SUCCESS)
        std::printf("loader_api_query=%d\n", loader_version_result);
    std::printf("loader_api=%u.%u.%u\n",
                VK_VERSION_MAJOR(loader_version),
                VK_VERSION_MINOR(loader_version),
                VK_VERSION_PATCH(loader_version));

    const VkApplicationInfo app = {
        VK_STRUCTURE_TYPE_APPLICATION_INFO,
        nullptr,
        "psxrecomp NoGraphicsAPI create-device probe",
        1,
        "psxrecomp",
        1,
        VK_API_VERSION_1_4,
    };
    const VkInstanceCreateInfo create_info = {
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        nullptr,
        0,
        &app,
        0,
        nullptr,
        0,
        nullptr,
    };

    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&create_info, nullptr, &instance);
    if (result != VK_SUCCESS)
    {
        std::printf("extension_probe=skipped vkCreateInstance=%d\n", result);
        return;
    }

    uint32_t device_count = 0;
    result = vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
    if (result != VK_SUCCESS)
    {
        std::printf("extension_probe=skipped vkEnumeratePhysicalDevices=%d\n", result);
        vkDestroyInstance(instance, nullptr);
        return;
    }

    std::vector<VkPhysicalDevice> devices(device_count);
    if (device_count)
    {
        result = vkEnumeratePhysicalDevices(instance, &device_count, devices.data());
        if (result != VK_SUCCESS)
        {
            std::printf("extension_probe=skipped vkEnumeratePhysicalDevices.list=%d\n", result);
            vkDestroyInstance(instance, nullptr);
            return;
        }
    }

    for (uint32_t device_index = 0; device_index < device_count; ++device_index)
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(devices[device_index], &props);
        std::printf("device[%u].name=%s\n", device_index, props.deviceName);
        std::printf("device[%u].api=%u.%u.%u\n", device_index,
                    VK_VERSION_MAJOR(props.apiVersion),
                    VK_VERSION_MINOR(props.apiVersion),
                    VK_VERSION_PATCH(props.apiVersion));
        std::printf("device[%u].driver_version_raw=%u\n", device_index, props.driverVersion);
        if (props.vendorID == 0x10de)
            std::printf("device[%u].nvidia_driver=%u.%u.%u.%u\n", device_index,
                        props.driverVersion >> 22, (props.driverVersion >> 14) & 0xff,
                        (props.driverVersion >> 6) & 0xff, props.driverVersion & 0x3f);

        /* Detect another prerequisite before proposing a driver change.
         * Existence alone does not prove buffer/image memory compatibility;
         * upstream create_device remains the authoritative eligibility test. */
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(devices[device_index], &memory);
        constexpr VkMemoryPropertyFlags required_memory = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        bool mapped_device_memory = false;
        for (uint32_t type = 0; type < memory.memoryTypeCount; ++type)
        {
            const uint32_t heap = memory.memoryTypes[type].heapIndex;
            if ((memory.memoryTypes[type].propertyFlags & required_memory) == required_memory &&
                heap < memory.memoryHeapCount && (memory.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT))
            {
                mapped_device_memory = true;
                std::printf("device[%u].coherent_device_local_type[%u].heap_bytes=%llu\n", device_index, type,
                            static_cast<unsigned long long>(memory.memoryHeaps[heap].size));
            }
        }
        std::printf("device[%u].coherent_device_local_memory=%s\n", device_index, mapped_device_memory ? "yes" : "no");

        uint32_t extension_count = 0;
        result = vkEnumerateDeviceExtensionProperties(devices[device_index], nullptr, &extension_count, nullptr);
        if (result != VK_SUCCESS)
        {
            std::printf("device[%u].extensions=skipped result=%d\n", device_index, result);
            continue;
        }
        std::vector<VkExtensionProperties> extensions(extension_count);
        if (extension_count)
        {
            result = vkEnumerateDeviceExtensionProperties(devices[device_index], nullptr, &extension_count, extensions.data());
            if (result != VK_SUCCESS)
            {
                std::printf("device[%u].extensions=skipped list_result=%d\n", device_index, result);
                continue;
            }
        }

        const bool descriptor_heap = has_extension(extensions, VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME);
        const bool address_commands = has_extension(extensions, VK_KHR_DEVICE_ADDRESS_COMMANDS_EXTENSION_NAME);
        const bool untyped_pointers = has_extension(extensions, VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME);
        const bool mesh_shader = has_extension(extensions, VK_EXT_MESH_SHADER_EXTENSION_NAME);
        const bool extension_only_pass = descriptor_heap && address_commands && untyped_pointers && mesh_shader;

        std::printf("device[%u].ext.%s=%s\n", device_index, VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME, descriptor_heap ? "yes" : "no");
        std::printf("device[%u].ext.%s=%s\n", device_index, VK_KHR_DEVICE_ADDRESS_COMMANDS_EXTENSION_NAME, address_commands ? "yes" : "no");
        std::printf("device[%u].ext.%s=%s\n", device_index, VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME, untyped_pointers ? "yes" : "no");
        std::printf("device[%u].ext.%s=%s\n", device_index, VK_EXT_MESH_SHADER_EXTENSION_NAME, mesh_shader ? "yes" : "no");
        std::printf("device[%u].extension_only_required=%s\n", device_index, extension_only_pass ? "pass" : "fail");
    }

    vkDestroyInstance(instance, nullptr);
}

/* Raw-byte round trip for the actual renderer's two resource roles. This
 * validates transport, not PSX channel interpretation or rendering. */
static bool run_texture_roundtrip(gpu::Device* device, const char* name,
                                 gpu::Format format, uint32_t bytes_per_pixel,
                                 gpu::TextureUsage usage)
{
    constexpr uint32_t vram_w = 1024;
    constexpr uint32_t vram_h = 512;
    const uint64_t vram_bytes = uint64_t{vram_w} * vram_h * bytes_per_pixel;
    std::printf("texture_smoke.format=%s\n", name);
    const gpu::TextureDesc vram_desc{
        .type = gpu::TextureType::two_d,
        .extent = {.x = vram_w, .y = vram_h, .z = 1},
        .mip_levels = 1,
        .layer_count = 1,
        .format = format,
        .mutable_format = false,
        .usage = usage,
    };

    if (!gpu::supports_texture_format(device, vram_desc.format, vram_desc.usage))
    {
        std::printf("texture_smoke=fail unsupported_format_usage\n");
        return false;
    }

    const gpu::SizeAlign vram_size_align = gpu::get_texture_size_align(device, vram_desc);
    if (vram_size_align.size == 0 || vram_size_align.align == 0)
    {
        std::printf("psx_vram_smoke=fail invalid_texture_size_align\n");
        return false;
    }

    gpu::GpuHeap upload = gpu::create_gpu_heap(device, vram_bytes, gpu::MemoryType::cpu_visible);
    if (!upload.range.cpu || !upload.range.gpu || upload.range.size < vram_bytes)
    {
        std::printf("psx_vram_smoke=fail upload_heap\n");
        if (upload.owner) gpu::destroy_gpu_heap(upload);
        return false;
    }

    gpu::GpuHeap readback = gpu::create_gpu_heap(device, vram_bytes, gpu::MemoryType::readback);
    if (!readback.range.cpu || !readback.range.gpu || readback.range.size < vram_bytes)
    {
        std::printf("psx_vram_smoke=fail readback_heap\n");
        if (readback.owner) gpu::destroy_gpu_heap(readback);
        gpu::destroy_gpu_heap(upload);
        return false;
    }

    gpu::TextureHeap texture_heap = gpu::create_texture_heap(device, vram_size_align.size);
    if (!texture_heap.owner || texture_heap.size < vram_size_align.size)
    {
        std::printf("psx_vram_smoke=fail texture_heap\n");
        if (texture_heap.owner) gpu::destroy_texture_heap(texture_heap);
        gpu::destroy_gpu_heap(readback);
        gpu::destroy_gpu_heap(upload);
        return false;
    }

    gpu::Texture* vram = gpu::create_texture(device, vram_desc, texture_heap, 0);
    if (!vram)
    {
        std::printf("psx_vram_smoke=fail create_texture\n");
        gpu::destroy_texture_heap(texture_heap);
        gpu::destroy_gpu_heap(readback);
        gpu::destroy_gpu_heap(upload);
        return false;
    }

    uint8_t* upload_bytes = reinterpret_cast<uint8_t*>(upload.range.cpu);
    for (uint64_t i = 0; i < vram_bytes; ++i)
        upload_bytes[i] = static_cast<uint8_t>(i ^ (i >> 8u) ^ (i >> 16u));
    std::memset(readback.range.cpu, 0, static_cast<size_t>(vram_bytes));

    gpu::TimelineSemaphore* timeline = gpu::create_timeline_semaphore(device);
    if (!timeline)
    {
        std::printf("psx_vram_smoke=fail timeline\n");
        gpu::destroy_texture(vram);
        gpu::destroy_texture_heap(texture_heap);
        gpu::destroy_gpu_heap(readback);
        gpu::destroy_gpu_heap(upload);
        return false;
    }

    gpu::CommandBuffer* commands = gpu::begin_commands(device);
    const gpu::TextureCopyDesc full_vram_copy{
        .mip_level = 0,
        .base_slice = 0,
        .slice_count = 1,
        .offset = {},
        .extent = {.x = vram_w, .y = vram_h, .z = 1},
        .row_pitch_bytes = vram_w * bytes_per_pixel,
        .slice_pitch_bytes = vram_bytes,
    };
    gpu::copy_memory_to_texture(commands, gpu::gpu_range(upload), vram, full_vram_copy);
    gpu::barrier(commands,
                 gpu::Stage::transfer, gpu::Access::transfer_write,
                 gpu::Stage::transfer, gpu::Access::transfer_read);
    gpu::copy_texture_to_memory(commands, vram, gpu::gpu_range(readback), full_vram_copy);
    gpu::barrier(commands,
                 gpu::Stage::transfer, gpu::Access::transfer_write,
                 gpu::Stage::host, gpu::Access::host_read);
    const gpu::TimelinePoint completion{.semaphore = timeline, .value = 1};
    gpu::CommandBuffer* submit_commands[] = {commands};
    gpu::submit(submit_commands, completion);
    gpu::wait_timeline(completion);

    const bool matches = std::memcmp(upload.range.cpu, readback.range.cpu,
                                     static_cast<size_t>(vram_bytes)) == 0;
    std::printf("texture_smoke=%s\n", matches ? "pass" : "fail readback_mismatch");

    gpu::destroy_timeline_semaphore(timeline);
    gpu::destroy_texture(vram);
    gpu::destroy_texture_heap(texture_heap);
    gpu::destroy_gpu_heap(readback);
    gpu::destroy_gpu_heap(upload);
    return matches;
}

int main()
{
    print_extension_matrix();

    const gpu::DeviceInit init = gpu::create_device({});
    std::printf("create_device.error=%s\n", error_name(init.error));

    if (init.error != gpu::Error::none)
    {
        gpu::destroy_device(init.device);
        return init.error == gpu::Error::unsupported ? 77 : 1;
    }

    const gpu::TextureUsage common_usage = gpu::TextureUsage::transfer_source |
        gpu::TextureUsage::transfer_destination | gpu::TextureUsage::sampled;
    /* gpu_vk_renderer.c uses an RGBA8 color target and an R16_UINT sampled /
     * storage raw mirror. A packed 5551 color target is not its contract. */
    const bool color_ok = run_texture_roundtrip(init.device, "rgba8_color", gpu::Format::rgba8_unorm, 4,
                                               common_usage | gpu::TextureUsage::color_attachment);
    const bool raw_ok = run_texture_roundtrip(init.device, "r16_raw", gpu::Format::r16_uint, 2,
                                             common_usage | gpu::TextureUsage::storage);
    const bool smoke_ok = color_ok && raw_ok;
    std::printf("psx_vram_smoke=%s\n", smoke_ok ? "pass" : "fail");
    const gpu::DeviceCaps& caps = gpu::get_device_caps(init.device);
    std::printf("device=%s\n", caps.device_name ? caps.device_name : "(unnamed)");
    std::printf("max_push_data_size=%llu\n", static_cast<unsigned long long>(caps.max_push_data_size));
    std::printf("texture_heap_alignment=%llu\n", static_cast<unsigned long long>(caps.texture_heap_alignment));
    std::printf("texture_descriptor_size=%llu\n", static_cast<unsigned long long>(caps.texture_descriptor_size));
    std::printf("sampler_descriptor_size=%llu\n", static_cast<unsigned long long>(caps.sampler_descriptor_size));
    std::printf("texture_compression_bc=%d\n", caps.texture_compression_bc ? 1 : 0);
    std::printf("texture_compression_astc=%d\n", caps.texture_compression_astc ? 1 : 0);
    std::printf("storage_input_output16=%d\n", caps.storage_input_output16 ? 1 : 0);

    gpu::destroy_device(init.device);
    return smoke_ok ? 0 : 2;
}
