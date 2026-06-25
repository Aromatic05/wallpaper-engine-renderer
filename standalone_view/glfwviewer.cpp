// glfwviewer: minimal C ABI consumer that opens a GLFW window, drives
// wallpaper-engine-renderer through its public C ABI surface, and
// presents each produced frame (a dmabuf) to the window by importing
// the dmabuf into a Vulkan image and blitting to the swapchain.
//
// This file is intentionally the only piece of consumer code that
// touches Vulkan directly. The lib itself does scene parsing,
// scripting, audio, and rendering; the consumer only:
//   - drives session lifecycle (create / set_source / set_render_config
//     / play / tick / stop / destroy)
//   - presents frames (acquire_frame / frame_release)
//   - forwards input (send_pointer_event)

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include <vulkan/vulkan.h>

#ifndef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_FEATURES
// Fallback: system's vulkan_core.h only ships the guard for this 1.1+ type.
typedef struct VkPhysicalDeviceExternalMemoryFeatures {
    VkStructureType              sType;
    void*                        pNext;
    VkExternalMemoryFeatureFlags externalMemoryFD;
    VkExternalMemoryFeatureFlags externalMemoryFC;
    VkExternalMemoryFeatureFlags externalMemoryFDCombine;
} VkPhysicalDeviceExternalMemoryFeatures;
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_FEATURES \
    ((VkStructureType)1000071000)
#endif

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "arg.hpp"
#include "wallpaper/abi/WeRenderer.h"

namespace {

// ---- Vulkan state -----------------------------------------------------

struct VkState {
    VkInstance               instance   { VK_NULL_HANDLE };
    VkPhysicalDevice         phys       { VK_NULL_HANDLE };
    VkDevice                 device     { VK_NULL_HANDLE };
    VkQueue                  queue      { VK_NULL_HANDLE };
    VkSurfaceKHR             surface    { VK_NULL_HANDLE };
    VkSwapchainKHR           swapchain  { VK_NULL_HANDLE };
    VkCommandPool            cmd_pool   { VK_NULL_HANDLE };
    VkRenderPass             render_pass{ VK_NULL_HANDLE };

    uint32_t                 queue_family { 0 };
    VkFormat                 format     { VK_FORMAT_B8G8R8A8_UNORM };
    VkExtent2D               extent     { 0, 0 };
    std::vector<VkImage>     swap_images;
    std::vector<VkImageView> swap_views;
    std::vector<VkFramebuffer> framebuffers;

    // For dmabuf import
    bool has_external_memory_fd { false };
};

// ---- Vulkan helpers ---------------------------------------------------

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                              VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT*,
                                              void*) {
    return VK_FALSE; // suppress; we don't print noise
}

bool createInstance(VkState& vk, const std::vector<const char*>& enabled) {
    VkApplicationInfo app {};
    app.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "sceneviewer";
    app.apiVersion       = VK_API_VERSION_1_1;

    VkInstanceCreateInfo info {};
    info.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo        = &app;
    info.enabledLayerCount       = 0;
    info.enabledExtensionCount   = static_cast<uint32_t>(enabled.size());
    info.ppEnabledExtensionNames = enabled.data();

    return vkCreateInstance(&info, nullptr, &vk.instance) == VK_SUCCESS;
}

bool pickPhysicalDevice(VkState& vk) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(vk.instance, &count, nullptr);
    if (count == 0) return false;
    std::vector<VkPhysicalDevice> devs(count);
    vkEnumeratePhysicalDevices(vk.instance, &count, devs.data());

    // Pick first device that has graphics + present queue and supports
    // importing external memory fds (the renderer outputs dmabuf).
    for (auto d : devs) {
        uint32_t qfc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qfc, nullptr);
        std::vector<VkQueueFamilyProperties> qfp(qfc);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qfc, qfp.data());
        int32_t gq = -1, pq = -1;
        for (uint32_t i = 0; i < qfc; ++i) {
            if (qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) gq = (int32_t)i;
            VkBool32 can_present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, vk.surface, &can_present);
            if (can_present) pq = (int32_t)i;
            if (gq >= 0 && pq >= 0) break;
        }
        if (gq < 0 || pq < 0 || gq != pq) continue;

        VkPhysicalDeviceExternalMemoryFeatures ext {};
        ext.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_FEATURES;
        VkPhysicalDeviceFeatures2 feat2 {};
        feat2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        feat2.pNext = &ext;
        vkGetPhysicalDeviceFeatures2(d, &feat2);
        if (! ext.externalMemoryFD) continue;

        vk.phys        = d;
        vk.queue_family = static_cast<uint32_t>(gq);
        return true;
    }
    return false;
}

bool createDevice(VkState& vk) {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci {};
    qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = vk.queue_family;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &prio;

    VkPhysicalDeviceExternalMemoryFeatures ext_enable {};
    ext_enable.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_FEATURES;
    ext_enable.externalMemoryFD = VK_TRUE;

    VkPhysicalDeviceFeatures features {};
    // We need a blit-capable queue; samplerable images for the dmabuf.
    features.samplerAnisotropy = VK_FALSE;

    const char* exts[] = { VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME };

    VkDeviceCreateInfo info {};
    info.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.pNext                   = &ext_enable;
    info.queueCreateInfoCount    = 1;
    info.pQueueCreateInfos       = &qci;
    info.pEnabledFeatures        = &features;
    info.enabledExtensionCount   = 1;
    info.ppEnabledExtensionNames = exts;

    if (vkCreateDevice(vk.phys, &info, nullptr, &vk.device) != VK_SUCCESS) return false;
    vkGetDeviceQueue(vk.device, vk.queue_family, 0, &vk.queue);
    vk.has_external_memory_fd = true;
    return true;
}

bool createSwapchain(VkState& vk, int width, int height) {
    VkSurfaceCapabilitiesKHR caps {};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.phys, vk.surface, &caps);

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        extent.width  = std::clamp(static_cast<uint32_t>(width),  caps.minImageExtent.width,  caps.maxImageExtent.width);
        extent.height = std::clamp(static_cast<uint32_t>(height), caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    vk.extent = extent;

    uint32_t fmt_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk.phys, vk.surface, &fmt_count, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fmt_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk.phys, vk.surface, &fmt_count, fmts.data());
    VkFormat fmt = VK_FORMAT_B8G8R8A8_UNORM;
    VkColorSpaceKHR cs = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    for (auto& f : fmts) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM) { fmt = f.format; cs = f.colorSpace; break; }
    }
    vk.format = fmt;

    uint32_t img_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && img_count > caps.maxImageCount) img_count = caps.maxImageCount;

    VkSwapchainCreateInfoKHR info {};
    info.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface          = vk.surface;
    info.minImageCount    = img_count;
    info.imageFormat      = fmt;
    info.imageColorSpace  = cs;
    info.imageExtent      = extent;
    info.imageArrayLayers = 1;
    info.imageUsage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.preTransform     = caps.currentTransform;
    info.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode      = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped          = VK_TRUE;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateSwapchainKHR(vk.device, &info, nullptr, &vk.swapchain) != VK_SUCCESS) return false;

    vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &img_count, nullptr);
    vk.swap_images.resize(img_count);
    vk.swap_views .resize(img_count);
    vk.framebuffers.resize(img_count);
    vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &img_count, vk.swap_images.data());

    // Render pass: load=clear, store=store, single color attachment
    VkAttachmentDescription ad {};
    ad.format         = fmt;
    ad.samples        = VK_SAMPLE_COUNT_1_BIT;
    ad.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ad.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    ad.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    ad.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    ad.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ar { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp {};
    sp.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments    = &ar;
    VkSubpassDependency dep {};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rp {};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1;
    rp.pAttachments    = &ad;
    rp.subpassCount    = 1;
    rp.pSubpasses     = &sp;
    rp.dependencyCount = 1;
    rp.pDependencies   = &dep;
    if (vkCreateRenderPass(vk.device, &rp, nullptr, &vk.render_pass) != VK_SUCCESS) return false;

    for (uint32_t i = 0; i < img_count; ++i) {
        VkImageViewCreateInfo vi {};
        vi.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image    = vk.swap_images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format   = fmt;
        vi.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount     = 1;
        vi.subresourceRange.layerCount     = 1;
        if (vkCreateImageView(vk.device, &vi, nullptr, &vk.swap_views[i]) != VK_SUCCESS) return false;
        VkFramebufferCreateInfo fi {};
        fi.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass      = vk.render_pass;
        fi.attachmentCount = 1;
        fi.pAttachments    = &vk.swap_views[i];
        fi.width           = extent.width;
        fi.height          = extent.height;
        fi.layers          = 1;
        if (vkCreateFramebuffer(vk.device, &fi, nullptr, &vk.framebuffers[i]) != VK_SUCCESS) return false;
    }
    return true;
}

bool createCommandPool(VkState& vk) {
    VkCommandPoolCreateInfo ci {};
    ci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.queueFamilyIndex = vk.queue_family;
    ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    return vkCreateCommandPool(vk.device, &ci, nullptr, &vk.cmd_pool) == VK_SUCCESS;
}

bool initVulkan(GLFWwindow* window, VkState& vk) {
    uint32_t glfw_ext_count = 0;
    auto exts = glfwGetRequiredInstanceExtensions(&glfw_ext_count);
    std::vector<const char*> instance_exts(exts, exts + glfw_ext_count);
    if (! createInstance(vk, instance_exts)) return false;

    if (glfwCreateWindowSurface(vk.instance, window, nullptr, &vk.surface) != VK_SUCCESS) return false;

    if (! pickPhysicalDevice(vk))   return false;
    if (! createDevice(vk))         return false;
    int w, h;
    glfwGetFramebufferSize(window, &w, &h);
    if (! createSwapchain(vk, w, h)) return false;
    if (! createCommandPool(vk))    return false;
    return true;
}

void cleanupVulkan(VkState& vk) {
    if (vk.device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(vk.device);
    for (auto fb : vk.framebuffers) vkDestroyFramebuffer(vk.device, fb, nullptr);
    for (auto v  : vk.swap_views)   vkDestroyImageView(vk.device, v, nullptr);
    if (vk.render_pass) vkDestroyRenderPass(vk.device, vk.render_pass, nullptr);
    if (vk.swapchain)   vkDestroySwapchainKHR(vk.device, vk.swapchain, nullptr);
    if (vk.cmd_pool)    vkDestroyCommandPool(vk.device, vk.cmd_pool, nullptr);
    if (vk.surface)     vkDestroySurfaceKHR(vk.instance, vk.surface, nullptr);
    if (vk.device)      vkDestroyDevice(vk.device, nullptr);
    if (vk.instance)    vkDestroyInstance(vk.instance, nullptr);
}

// ---- dmabuf import + present -----------------------------------------

struct DmaImage {
    VkImage     image  { VK_NULL_HANDLE };
    VkDeviceMemory memory{ VK_NULL_HANDLE };
};

bool importDmabuf(VkState& vk, const we_frame_v1& frame, DmaImage& out) {
    // Pick primary plane's fd
    if (frame.n_planes == 0 || frame.planes[0].fd < 0) return false;

    VkImageCreateInfo ic {};
    ic.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ic.imageType     = VK_IMAGE_TYPE_2D;
    ic.format        = static_cast<VkFormat>(frame.drm_fourcc);
    ic.extent        = { frame.width, frame.height, 1 };
    ic.mipLevels     = 1;
    ic.arrayLayers   = 1;
    ic.samples       = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling        = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    ic.usage         = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ic.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkExternalMemoryImageCreateInfo ext_ic {};
    ext_ic.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext_ic.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    ic.pNext = &ext_ic;

    VkImageDrmFormatModifierListCreateInfoEXT list {};
    list.sType            = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
    list.drmFormatModifierCount = 1;
    std::array<uint64_t, 1> mods { frame.drm_modifier };
    list.pDrmFormatModifiers = mods.data();
    ext_ic.pNext = &list;

    if (vkCreateImage(vk.device, &ic, nullptr, &out.image) != VK_SUCCESS) {
        // Fall back: try without explicit modifier list
        ext_ic.pNext = nullptr;
        if (vkCreateImage(vk.device, &ic, nullptr, &out.image) != VK_SUCCESS) return false;
    }

    VkMemoryRequirements req {};
    vkGetImageMemoryRequirements(vk.device, out.image, &req);

    VkPhysicalDeviceMemoryProperties mp {};
    vkGetPhysicalDeviceMemoryProperties(vk.phys, &mp);
    int32_t type_idx = -1;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((req.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            type_idx = (int32_t)i; break;
        }
    }
    if (type_idx < 0) {
        vkDestroyImage(vk.device, out.image, nullptr);
        return false;
    }

    VkMemoryAllocateInfo mai {};
    mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = static_cast<uint32_t>(type_idx);

    VkImportMemoryFdInfoKHR import_fd {};
    import_fd.sType      = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    import_fd.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import_fd.fd         = ::dup(frame.planes[0].fd);
    mai.pNext = &import_fd;

    if (vkAllocateMemory(vk.device, &mai, nullptr, &out.memory) != VK_SUCCESS) {
        vkDestroyImage(vk.device, out.image, nullptr);
        return false;
    }
    if (vkBindImageMemory(vk.device, out.image, out.memory, 0) != VK_SUCCESS) {
        vkFreeMemory(vk.device, out.memory, nullptr);
        vkDestroyImage(vk.device, out.image, nullptr);
        return false;
    }
    return true;
}

void destroyDmaImage(VkState& vk, DmaImage& img) {
    if (img.memory) vkFreeMemory(vk.device, img.memory, nullptr);
    if (img.image)  vkDestroyImage(vk.device, img.image, nullptr);
    img = {};
}

bool presentFrame(VkState& vk, const we_frame_v1& frame) {
    DmaImage src {};
    if (! importDmabuf(vk, frame, src)) return false;

    // Acquire swapchain image
    uint32_t img_idx = 0;
    VkResult r = vkAcquireNextImageKHR(vk.device, vk.swapchain, UINT64_MAX,
                                       VK_NULL_HANDLE, VK_NULL_HANDLE, &img_idx);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        destroyDmaImage(vk, src);
        return false;
    }

    // Allocate + record command buffer: blit src -> swap, present
    VkCommandBufferAllocateInfo cbai {};
    cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool        = vk.cmd_pool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(vk.device, &cbai, &cb);

    VkCommandBufferBeginInfo cbbi {};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &cbbi);

    VkImageMemoryBarrier src_barrier {};
    src_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    src_barrier.srcAccessMask = 0;
    src_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    src_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    src_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    src_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    src_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    src_barrier.image = src.image;
    src_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    src_barrier.subresourceRange.levelCount = 1;
    src_barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &src_barrier);

    VkImageMemoryBarrier dst_barrier {};
    dst_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    dst_barrier.srcAccessMask = 0;
    dst_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dst_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    dst_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dst_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dst_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dst_barrier.image = vk.swap_images[img_idx];
    dst_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    dst_barrier.subresourceRange.levelCount = 1;
    dst_barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &dst_barrier);

    VkImageBlit blit {};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[1] = { (int32_t)frame.width, (int32_t)frame.height, 1 };
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[1] = { (int32_t)vk.extent.width, (int32_t)vk.extent.height, 1 };
    vkCmdBlitImage(cb,
        src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        vk.swap_images[img_idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &blit, VK_FILTER_LINEAR);

    VkImageMemoryBarrier to_present {};
    to_present.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_present.dstAccessMask = 0;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.image = vk.swap_images[img_idx];
    to_present.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_present.subresourceRange.levelCount = 1;
    to_present.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 1, &to_present);

    vkEndCommandBuffer(cb);

    VkSubmitInfo si {};
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cb;
    vkQueueSubmit(vk.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(vk.queue);
    vkFreeCommandBuffers(vk.device, vk.cmd_pool, 1, &cb);

    VkPresentInfoKHR pi {};
    pi.sType          = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.swapchainCount = 1;
    pi.pSwapchains    = &vk.swapchain;
    pi.pImageIndices  = &img_idx;
    vkQueuePresentKHR(vk.queue, &pi);

    destroyDmaImage(vk, src);
    return true;
}

// ---- mouse forwarding ------------------------------------------------

void onMouseButton(GLFWwindow* w, int button, int action, int /*mods*/) {
    if (button != GLFW_MOUSE_BUTTON_LEFT) return;
    auto* session = reinterpret_cast<we_session_t*>(glfwGetWindowUserPointer(w));
    if (! session) return;
    double x = 0, y = 0;
    glfwGetCursorPos(w, &x, &y);
    int ww = 0, hh = 0;
    glfwGetFramebufferSize(w, &ww, &hh);
    if (ww == 0 || hh == 0) return;
    uint32_t type = (action == GLFW_PRESS) ? WE_POINTER_DOWN : WE_POINTER_UP;
    we_session_send_pointer_event(session, type,
                                 static_cast<float>(x / ww),
                                 static_cast<float>(y / hh));
}

void onCursorPos(GLFWwindow* w, double x, double y) {
    auto* session = reinterpret_cast<we_session_t*>(glfwGetWindowUserPointer(w));
    if (! session) return;
    int ww = 0, hh = 0;
    glfwGetFramebufferSize(w, &ww, &hh);
    if (ww == 0 || hh == 0) return;
    we_session_send_pointer_event(session, WE_POINTER_MOVE,
                                 static_cast<float>(x / ww),
                                 static_cast<float>(y / hh));
}

} // namespace

int main(int argc, char** argv) {
    Args args;
    std::string err;
    if (! parseArgs(argc, argv, args, err)) {
        printHelp(argv[0]);
        if (! err.empty()) std::cerr << "error: " << err << "\n";
        return err.empty() ? 0 : 1;
    }

    if (! glfwInit()) {
        std::cerr << "glfwInit failed\n"; return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    GLFWwindow* window = glfwCreateWindow(args.width, args.height, "sceneviewer", nullptr, nullptr);
    if (! window) {
        std::cerr << "glfwCreateWindow failed\n";
        glfwTerminate();
        return 1;
    }

    VkState vk;
    if (! initVulkan(window, vk)) {
        std::cerr << "initVulkan failed\n";
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    we_session_t* session = we_session_create();
    if (! session) {
        std::cerr << "we_session_create failed\n";
        cleanupVulkan(vk);
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    we_source_v1 source {};
    source.size    = sizeof(source);
    source.version = 1;
    source.kind    = WE_SOURCE_KIND_SCENE;
    source.uri     = args.scene.c_str();
    source.assets_uri = args.assets.c_str();
    source.fps     = args.fps;
    if (int32_t r = we_session_set_source(session, &source); r != 0) {
        std::cerr << "we_session_set_source failed: " << r << "\n";
        we_session_destroy(session);
        cleanupVulkan(vk);
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    we_render_config_v1 cfg {};
    cfg.size    = sizeof(cfg);
    cfg.version = 1;
    cfg.width   = static_cast<uint32_t>(args.width);
    cfg.height  = static_cast<uint32_t>(args.height);
    cfg.prefer_dmabuf = true;
    if (int32_t r = we_session_set_render_config(session, &cfg); r != 0) {
        std::cerr << "we_session_set_render_config failed: " << r << "\n";
        we_session_destroy(session);
        cleanupVulkan(vk);
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    if (int32_t r = we_session_play(session); r != 0) {
        std::cerr << "we_session_play failed: " << r << "\n";
        we_session_destroy(session);
        cleanupVulkan(vk);
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    glfwSetWindowUserPointer(window, session);
    glfwSetMouseButtonCallback(window, onMouseButton);
    glfwSetCursorPosCallback(window, onCursorPos);

    bool presented_any = false;
    while (! glfwWindowShouldClose(window)) {
        glfwPollEvents();
        we_session_tick(session);

        we_frame_v1 frame {};
        frame.size    = sizeof(frame);
        frame.version = 1;
        if (we_session_acquire_frame(session, &frame) == 0) {
            if (frame.kind == WE_FRAME_KIND_DMABUF && frame.n_planes > 0) {
                if (presentFrame(vk, frame)) presented_any = true;
            }
            we_frame_release(&frame);
        }

        if (! presented_any) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }

    we_session_stop(session);
    we_session_destroy(session);
    cleanupVulkan(vk);
    glfwDestroyWindow(window);
    glfwTerminate();
    std::cout << "sceneviewer: presented " << (presented_any ? "frames" : "no frames") << "\n";
    return 0;
}
