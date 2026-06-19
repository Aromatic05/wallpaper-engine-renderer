#pragma once
#include "core/NoCopyMove.hpp"
#include "vulkan/GraphicsPipeline.hpp"
#include "vulkan/StagingBuffer.hpp"
#include "vvk/vma_wrapper.hpp"
#include <memory>
#include <string>
#include <unordered_map>

namespace wallpaper
{
namespace vulkan
{

struct RenderingResources {
    vvk::CommandBuffer command;

    vvk::Semaphore sem_swap_wait_image;
    vvk::Semaphore sem_swap_finish;
    vvk::Fence     fence_frame;

    StagingBuffer* vertex_buf;
    StagingBuffer* dyn_buf;

    std::shared_ptr<GraphicsPipelineStateCache> pipeline_cache;

    // 3D model chunks are emitted as separate CustomShaderPass instances, but authored WE models
    // rely on them sharing one depth buffer per output target. Keeping that depth storage here makes
    // the behavior opt-in for model passes and leaves all legacy 2D render targets color-only.
    std::unordered_map<std::string, VmaImageParameters> model_depth_images;
};
} // namespace vulkan
} // namespace wallpaper
