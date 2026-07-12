#include "vulkanrender/FinalOutputMsaa.hpp"

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace
{
[[noreturn]] void Fail(std::string_view message) {
    std::fprintf(stderr,
                 "final-output-msaa-test: %.*s\n",
                 static_cast<int>(message.size()),
                 message.data());
    std::abort();
}

void Require(bool condition, std::string_view message) {
    if (! condition) Fail(message);
}
} // namespace

int main() {
    using wallpaper::vulkan::BuildFinalOutputAttachmentPlan;
    using wallpaper::vulkan::ResolveFinalOutputSampleCount;
    using wallpaper::vulkan::BuildFinalOutputMultisampleState;

    constexpr VkSampleCountFlags samples_1_2_4 =
        VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT;

    Require(ResolveFinalOutputSampleCount(0, samples_1_2_4) == VK_SAMPLE_COUNT_1_BIT,
            "zero request must preserve legacy single-sample output");
    Require(ResolveFinalOutputSampleCount(1, samples_1_2_4) == VK_SAMPLE_COUNT_1_BIT,
            "explicit one-sample request changed");
    Require(ResolveFinalOutputSampleCount(4, samples_1_2_4) == VK_SAMPLE_COUNT_4_BIT,
            "supported four-sample request was not selected");
    Require(ResolveFinalOutputSampleCount(8, samples_1_2_4) == VK_SAMPLE_COUNT_4_BIT,
            "unsupported request must clamp to the highest supported lower count");
    Require(ResolveFinalOutputSampleCount(3, samples_1_2_4) == VK_SAMPLE_COUNT_2_BIT,
            "non-power-of-two request must clamp deterministically");
    Require(ResolveFinalOutputSampleCount(64, VK_SAMPLE_COUNT_1_BIT) == VK_SAMPLE_COUNT_1_BIT,
            "single-sample-only device must remain usable");
    Require(ResolveFinalOutputSampleCount(4, samples_1_2_4, false) ==
                VK_SAMPLE_COUNT_1_BIT,
            "device without sample-rate shading must not expose ineffective MSAA");

    const auto single = BuildFinalOutputAttachmentPlan(VK_SAMPLE_COUNT_1_BIT);
    Require(! single.uses_resolve, "single-sample output must not allocate a resolve attachment");
    Require(single.attachment_count == 1,
            "single-sample output must expose exactly one attachment");
    Require(single.color_attachment == 0,
            "single-sample output must render directly into the present attachment");
    const auto single_state = BuildFinalOutputMultisampleState(VK_SAMPLE_COUNT_1_BIT);
    Require(single_state.rasterizationSamples == VK_SAMPLE_COUNT_1_BIT &&
                single_state.sampleShadingEnable == VK_FALSE,
            "single-sample pipeline state changed");

    const auto multisampled = BuildFinalOutputAttachmentPlan(VK_SAMPLE_COUNT_4_BIT);
    Require(multisampled.uses_resolve, "multisampled output must resolve before export");
    Require(multisampled.attachment_count == 2,
            "multisampled output must expose color plus resolve attachments");
    Require(multisampled.color_attachment == 0 && multisampled.resolve_attachment == 1,
            "multisampled output attachment ordering changed");
    Require(multisampled.sample_count == VK_SAMPLE_COUNT_4_BIT,
            "attachment plan lost the selected sample count");
    const auto multisample_state = BuildFinalOutputMultisampleState(VK_SAMPLE_COUNT_4_BIT);
    Require(multisample_state.rasterizationSamples == VK_SAMPLE_COUNT_4_BIT,
            "multisample pipeline lost its rasterization sample count");
    Require(multisample_state.sampleShadingEnable == VK_TRUE &&
                multisample_state.minSampleShading == 1.0f,
            "final-output MSAA must shade each sample position before resolve");

    return 0;
}
