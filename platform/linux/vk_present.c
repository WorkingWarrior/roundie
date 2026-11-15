#include "vk_present.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#define MAX_IMAGES 8U

struct vk_present {
    SDL_Window *window;
    uint32_t width;
    uint32_t height;
    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice physical;
    VkPhysicalDeviceProperties properties;
    uint32_t queue_family;
    VkDevice device;
    VkQueue queue;
    VkCommandPool pool;
    VkCommandBuffer commands;
    VkFence done;
    VkSemaphore acquired;
    VkSemaphore rendered[MAX_IMAGES];
    VkBuffer staging;
    VkDeviceMemory staging_memory;
    void *staging_map;
    VkImage frame;
    VkDeviceMemory frame_memory;
    bool frame_initialized;
    VkFormat format;
    VkSwapchainKHR swapchain;
    VkExtent2D extent;
    uint32_t image_count;
    VkImage images[MAX_IMAGES];
    bool swapchain_stale;
};

static bool check(VkResult result, const char *what)
{
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vulkan: %s failed (%d)\n", what, (int)result);
        return false;
    }
    return true;
}

static bool memory_type(const vk_present_t *present, uint32_t bits, VkMemoryPropertyFlags flags,
                        uint32_t *out)
{
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(present->physical, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; i++) {
        if ((bits & (1U << i)) != 0 && (memory.memoryTypes[i].propertyFlags & flags) == flags) {
            *out = i;
            return true;
        }
    }
    return false;
}

static bool create_instance(vk_present_t *present)
{
    Uint32 count = 0;
    const char *const *extensions = SDL_Vulkan_GetInstanceExtensions(&count);
    if (extensions == NULL) {
        fprintf(stderr, "vulkan: %s\n", SDL_GetError());
        return false;
    }
    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Roundie Desktop",
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
        .enabledExtensionCount = count,
        .ppEnabledExtensionNames = extensions,
    };
    if (!check(vkCreateInstance(&info, NULL, &present->instance), "vkCreateInstance")) {
        return false;
    }
    if (!SDL_Vulkan_CreateSurface(present->window, present->instance, NULL, &present->surface)) {
        fprintf(stderr, "vulkan: surface: %s\n", SDL_GetError());
        return false;
    }
    return true;
}

static bool choose_device(vk_present_t *present)
{
    uint32_t count = 0;
    if (!check(vkEnumeratePhysicalDevices(present->instance, &count, NULL), "enumerate devices") ||
        count == 0) {
        fprintf(stderr, "vulkan: no device\n");
        return false;
    }
    VkPhysicalDevice devices[16];
    if (count > 16) {
        count = 16;
    }
    vkEnumeratePhysicalDevices(present->instance, &count, devices);
    /* Prefer a discrete GPU; any device with a presenting graphics queue works. */
    int best_score = -1;
    for (uint32_t d = 0; d < count; d++) {
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[d], &families, NULL);
        VkQueueFamilyProperties properties[16];
        if (families > 16) {
            families = 16;
        }
        vkGetPhysicalDeviceQueueFamilyProperties(devices[d], &families, properties);
        for (uint32_t f = 0; f < families; f++) {
            VkBool32 supported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(devices[d], f, present->surface, &supported);
            if (supported != VK_TRUE || (properties[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
                continue;
            }
            VkPhysicalDeviceProperties device_properties;
            vkGetPhysicalDeviceProperties(devices[d], &device_properties);
            int score = device_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > best_score) {
                best_score = score;
                present->physical = devices[d];
                present->queue_family = f;
                present->properties = device_properties;
            }
            break;
        }
    }
    if (best_score < 0) {
        fprintf(stderr, "vulkan: no device presents to this window\n");
        return false;
    }
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = present->queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const char *extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = extensions,
    };
    if (!check(vkCreateDevice(present->physical, &info, NULL, &present->device), "vkCreateDevice")) {
        return false;
    }
    vkGetDeviceQueue(present->device, present->queue_family, 0, &present->queue);
    return true;
}

static bool choose_format(vk_present_t *present)
{
    uint32_t count = 0;
    if (!check(vkGetPhysicalDeviceSurfaceFormatsKHR(present->physical, present->surface, &count, NULL),
               "surface format count") ||
        count == 0) {
        return false;
    }
    VkSurfaceFormatKHR *formats = calloc(count, sizeof(*formats));
    if (formats == NULL) {
        return false;
    }
    bool found = false;
    if (check(vkGetPhysicalDeviceSurfaceFormatsKHR(present->physical, present->surface, &count, formats),
              "surface formats")) {
        /* UNORM keeps LVGL's sRGB-encoded values unchanged through copy and
         * blit; the swapchain declares them sRGB non-linear. */
        for (uint32_t i = 0; i < count && !found; i++) {
            if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM &&
                formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                present->format = formats[i].format;
                found = true;
            }
        }
        if (!found) {
            fprintf(stderr, "vulkan: surface has no B8G8R8A8_UNORM sRGB format\n");
        }
    }
    free(formats);
    return found;
}

static bool create_frame(vk_present_t *present)
{
    VkDeviceSize bytes = (VkDeviceSize)present->width * present->height * 4U;
    VkBufferCreateInfo buffer = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (!check(vkCreateBuffer(present->device, &buffer, NULL, &present->staging), "staging buffer")) {
        return false;
    }
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(present->device, present->staging, &requirements);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
    };
    if (!memory_type(present, requirements.memoryTypeBits,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     &allocation.memoryTypeIndex) ||
        !check(vkAllocateMemory(present->device, &allocation, NULL, &present->staging_memory), "staging memory") ||
        !check(vkBindBufferMemory(present->device, present->staging, present->staging_memory, 0), "bind staging") ||
        !check(vkMapMemory(present->device, present->staging_memory, 0, bytes, 0, &present->staging_map),
               "map staging")) {
        return false;
    }
    VkImageCreateInfo image = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = present->format,
        .extent = {present->width, present->height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (!check(vkCreateImage(present->device, &image, NULL, &present->frame), "frame image")) {
        return false;
    }
    vkGetImageMemoryRequirements(present->device, present->frame, &requirements);
    allocation.allocationSize = requirements.size;
    if (!memory_type(present, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     &allocation.memoryTypeIndex) ||
        !check(vkAllocateMemory(present->device, &allocation, NULL, &present->frame_memory), "frame memory") ||
        !check(vkBindImageMemory(present->device, present->frame, present->frame_memory, 0), "bind frame")) {
        return false;
    }
    VkFormatProperties format;
    vkGetPhysicalDeviceFormatProperties(present->physical, present->format, &format);
    VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    if ((format.optimalTilingFeatures & needed) != needed) {
        fprintf(stderr, "vulkan: format cannot be blitted\n");
        return false;
    }
    return true;
}

static void destroy_swapchain(vk_present_t *present)
{
    if (present->swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(present->device, present->swapchain, NULL);
        present->swapchain = VK_NULL_HANDLE;
    }
    present->image_count = 0;
}

/* Returns false on error; a zero-sized (minimised) window leaves no swapchain. */
static bool create_swapchain(vk_present_t *present)
{
    VkSurfaceCapabilitiesKHR capabilities;
    if (!check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(present->physical, present->surface, &capabilities),
               "surface capabilities")) {
        return false;
    }
    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX) {
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(present->window, &width, &height);
        extent.width = (uint32_t)width;
        extent.height = (uint32_t)height;
    }
    if (extent.width == 0 || extent.height == 0) {
        present->swapchain_stale = true;
        return true;
    }
    if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0) {
        fprintf(stderr, "vulkan: swapchain images cannot be transfer destinations\n");
        return false;
    }
    uint32_t images = capabilities.minImageCount + 1U;
    if (capabilities.maxImageCount != 0 && images > capabilities.maxImageCount) {
        images = capabilities.maxImageCount;
    }
    if (images > MAX_IMAGES) {
        images = MAX_IMAGES;
    }
    VkSwapchainKHR previous = present->swapchain;
    VkSwapchainCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = present->surface,
        .minImageCount = images,
        .imageFormat = present->format,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
        .oldSwapchain = previous,
    };
    VkResult result = vkCreateSwapchainKHR(present->device, &info, NULL, &present->swapchain);
    if (previous != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(present->device, previous, NULL);
    }
    if (!check(result, "vkCreateSwapchainKHR")) {
        present->swapchain = VK_NULL_HANDLE;
        return false;
    }
    present->extent = extent;
    /* The driver may create more images than requested; acquired indices
     * must fit images[] and rendered[]. */
    if (!check(vkGetSwapchainImagesKHR(present->device, present->swapchain, &present->image_count, NULL),
               "swapchain image count")) {
        return false;
    }
    if (present->image_count > MAX_IMAGES) {
        fprintf(stderr, "vulkan: swapchain has %u images, more than %u\n", (unsigned)present->image_count,
                (unsigned)MAX_IMAGES);
        return false;
    }
    if (!check(vkGetSwapchainImagesKHR(present->device, present->swapchain, &present->image_count, present->images),
               "swapchain images")) {
        return false;
    }
    present->swapchain_stale = false;
    return true;
}

vk_present_t *vk_present_create(SDL_Window *window, uint32_t width, uint32_t height)
{
    vk_present_t *present = calloc(1, sizeof(*present));
    if (present == NULL) {
        return NULL;
    }
    present->window = window;
    present->width = width;
    present->height = height;
    bool ok = create_instance(present) && choose_device(present) && choose_format(present) &&
              create_frame(present);
    if (ok) {
        VkCommandPoolCreateInfo pool = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = present->queue_family,
        };
        ok = check(vkCreateCommandPool(present->device, &pool, NULL, &present->pool), "command pool");
    }
    if (ok) {
        VkCommandBufferAllocateInfo allocate = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = present->pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        ok = check(vkAllocateCommandBuffers(present->device, &allocate, &present->commands), "command buffer");
    }
    VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fence = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    if (ok) {
        ok = check(vkCreateFence(present->device, &fence, NULL, &present->done), "fence") &&
             check(vkCreateSemaphore(present->device, &semaphore, NULL, &present->acquired), "semaphore");
    }
    for (uint32_t i = 0; ok && i < MAX_IMAGES; i++) {
        ok = check(vkCreateSemaphore(present->device, &semaphore, NULL, &present->rendered[i]), "semaphore");
    }
    if (ok) {
        ok = create_swapchain(present);
    }
    if (!ok) {
        vk_present_destroy(present);
        return NULL;
    }
    return present;
}

static void barrier(VkCommandBuffer commands, VkImage image, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags source_access, VkAccessFlags target_access,
                    VkPipelineStageFlags source_stage, VkPipelineStageFlags target_stage)
{
    VkImageMemoryBarrier info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = source_access,
        .dstAccessMask = target_access,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(commands, source_stage, target_stage, 0, 0, NULL, 0, NULL, 1, &info);
}

/* Largest integer zoom that fits, else fit by aspect: crisp pixels. */
static void layout(const vk_present_t *present, int32_t *left, int32_t *top, uint32_t *out_w, uint32_t *out_h)
{
    uint32_t zoom_x = present->extent.width / present->width;
    uint32_t zoom_y = present->extent.height / present->height;
    uint32_t zoom = zoom_x < zoom_y ? zoom_x : zoom_y;
    *out_w = present->width * zoom;
    *out_h = present->height * zoom;
    if (zoom == 0) {
        uint32_t side = present->extent.width < present->extent.height ? present->extent.width : present->extent.height;
        *out_w = side;
        *out_h = side;
    }
    *left = (int32_t)(present->extent.width - *out_w) / 2;
    *top = (int32_t)(present->extent.height - *out_h) / 2;
}

bool vk_present_map(const vk_present_t *present, float x, float y, int32_t *panel_x, int32_t *panel_y)
{
    int32_t left = 0;
    int32_t top = 0;
    uint32_t out_w = 0;
    uint32_t out_h = 0;
    layout(present, &left, &top, &out_w, &out_h);
    if (out_w == 0 || out_h == 0) {
        return false;
    }
    float px = (x - (float)left) * (float)present->width / (float)out_w;
    float py = (y - (float)top) * (float)present->height / (float)out_h;
    bool inside = px >= 0.0f && py >= 0.0f && px < (float)present->width && py < (float)present->height;
    if (px < 0.0f) {
        px = 0.0f;
    }
    if (py < 0.0f) {
        py = 0.0f;
    }
    *panel_x = (int32_t)px;
    *panel_y = (int32_t)py;
    if (*panel_x >= (int32_t)present->width) {
        *panel_x = (int32_t)present->width - 1;
    }
    if (*panel_y >= (int32_t)present->height) {
        *panel_y = (int32_t)present->height - 1;
    }
    return inside;
}

static void record(vk_present_t *present, VkImage target)
{
    VkCommandBuffer commands = present->commands;
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(commands, &begin);
    barrier(commands, present->frame,
            present->frame_initialized ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy = {
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {present->width, present->height, 1},
    };
    vkCmdCopyBufferToImage(commands, present->staging, present->frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &copy);
    barrier(commands, present->frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    present->frame_initialized = true;

    barrier(commands, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue black = {.float32 = {0.0f, 0.0f, 0.0f, 1.0f}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(commands, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    barrier(commands, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);

    int32_t left = 0;
    int32_t top = 0;
    uint32_t out_w = 0;
    uint32_t out_h = 0;
    layout(present, &left, &top, &out_w, &out_h);
    VkImageBlit blit = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffsets = {{0, 0, 0}, {(int32_t)present->width, (int32_t)present->height, 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffsets = {{left, top, 0}, {left + (int32_t)out_w, top + (int32_t)out_h, 1}},
    };
    vkCmdBlitImage(commands, present->frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    barrier(commands, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vkEndCommandBuffer(commands);
}

bool vk_present_frame(vk_present_t *present, const uint32_t *pixels)
{
    if (!check(vkWaitForFences(present->device, 1, &present->done, VK_TRUE, UINT64_MAX), "wait fence")) {
        return false;
    }
    if (present->swapchain_stale || present->swapchain == VK_NULL_HANDLE) {
        vkDeviceWaitIdle(present->device);
        if (!create_swapchain(present)) {
            return false;
        }
        if (present->swapchain == VK_NULL_HANDLE) {
            return true; /* minimised */
        }
    }
    uint32_t index = 0;
    VkResult result = vkAcquireNextImageKHR(present->device, present->swapchain, UINT64_MAX, present->acquired,
                                            VK_NULL_HANDLE, &index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        present->swapchain_stale = true;
        return true;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        return check(result, "acquire image");
    }
    memcpy(present->staging_map, pixels, (size_t)present->width * present->height * 4U);
    vkResetFences(present->device, 1, &present->done);
    vkResetCommandBuffer(present->commands, 0);
    record(present, present->images[index]);
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &present->acquired,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &present->commands,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &present->rendered[index],
    };
    if (!check(vkQueueSubmit(present->queue, 1, &submit, present->done), "submit")) {
        return false;
    }
    VkPresentInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &present->rendered[index],
        .swapchainCount = 1,
        .pSwapchains = &present->swapchain,
        .pImageIndices = &index,
    };
    result = vkQueuePresentKHR(present->queue, &info);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        present->swapchain_stale = true;
        return true;
    }
    return check(result, "present");
}

const char *vk_present_device(const vk_present_t *present)
{
    return present->properties.deviceName;
}

void vk_present_destroy(vk_present_t *present)
{
    if (present == NULL) {
        return;
    }
    if (present->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(present->device);
        destroy_swapchain(present);
        for (uint32_t i = 0; i < MAX_IMAGES; i++) {
            if (present->rendered[i] != VK_NULL_HANDLE) {
                vkDestroySemaphore(present->device, present->rendered[i], NULL);
            }
        }
        if (present->acquired != VK_NULL_HANDLE) {
            vkDestroySemaphore(present->device, present->acquired, NULL);
        }
        if (present->done != VK_NULL_HANDLE) {
            vkDestroyFence(present->device, present->done, NULL);
        }
        if (present->pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(present->device, present->pool, NULL);
        }
        if (present->frame != VK_NULL_HANDLE) {
            vkDestroyImage(present->device, present->frame, NULL);
        }
        if (present->frame_memory != VK_NULL_HANDLE) {
            vkFreeMemory(present->device, present->frame_memory, NULL);
        }
        if (present->staging != VK_NULL_HANDLE) {
            vkDestroyBuffer(present->device, present->staging, NULL);
        }
        if (present->staging_memory != VK_NULL_HANDLE) {
            vkFreeMemory(present->device, present->staging_memory, NULL);
        }
        vkDestroyDevice(present->device, NULL);
    }
    if (present->surface != VK_NULL_HANDLE) {
        SDL_Vulkan_DestroySurface(present->instance, present->surface, NULL);
    }
    if (present->instance != VK_NULL_HANDLE) {
        vkDestroyInstance(present->instance, NULL);
    }
    free(present);
}
