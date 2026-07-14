#include "backend/video/internal/VideoBackend.hpp"

#include "backend/video/internal/VideoFrameSwapchain.hpp"
#include "backend/video/internal/VideoOutputBinding.hpp"
#include "backend/video/internal/VideoRenderPlan.hpp"
#include "wallpaper/scene/WESceneContract.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>
#include <drm/drm_fourcc.h>
#include <gst/app/gstappsink.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/gst.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/video-frame.h>
#include <gst/video/video-info-dma.h>
#include <gst/video/video.h>

namespace wallpaper
{
namespace
{
Result<void> unsupportedInput() {
    return Result<void>::failure(ResultCode::NotSupported,
                                 "video backend does not support input events");
}

Result<void> unsupportedDmabuf(std::string_view reason) {
    return Result<void>::failure(ResultCode::NotSupported,
                                 "DMA-BUF video output requires GStreamer VA post-processing with "
                                 "a compatible RGB format: " +
                                     std::string(reason));
}

std::string EscapeGstPropertyString(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char c : value) {
        if (c == '\\' || c == '"') escaped.push_back('\\');
        escaped.push_back(c);
    }
    return escaped;
}

std::vector<std::string> CollectDrmFormatStrings(const GValue* drm_value) {
    std::vector<std::string> drm_formats;
    if (drm_value == nullptr) return drm_formats;

    auto append_if_present = [&drm_formats](const GValue* value) {
        if (value == nullptr || ! G_VALUE_HOLDS_STRING(value)) return;
        const char* text = g_value_get_string(value);
        if (text == nullptr || *text == '\0') return;
        drm_formats.emplace_back(text);
    };

    if (G_VALUE_HOLDS_STRING(drm_value)) {
        append_if_present(drm_value);
        return drm_formats;
    }

    if (GST_VALUE_HOLDS_LIST(drm_value)) {
        const guint item_count = gst_value_list_get_size(drm_value);
        drm_formats.reserve(item_count);
        for (guint i = 0; i < item_count; ++i) {
            append_if_present(gst_value_list_get_value(drm_value, i));
        }
    }
    return drm_formats;
}

struct DmabufCapsInfo {
    std::vector<std::string> drm_formats;
};

bool IsTextureOutputCompatibleDrmFourcc(guint32 drm_fourcc) {
    return drm_fourcc == DRM_FORMAT_ABGR8888 || drm_fourcc == DRM_FORMAT_XBGR8888 ||
           drm_fourcc == DRM_FORMAT_ARGB8888 || drm_fourcc == DRM_FORMAT_XRGB8888;
}

std::optional<DmabufCapsInfo> QueryVaDmabufPostprocCaps() {
    GstElementFactory* factory = gst_element_factory_find("vapostproc");
    if (factory == nullptr) return std::nullopt;

    GstCaps* caps = nullptr;
    for (const GList* it = gst_element_factory_get_static_pad_templates(factory); it != nullptr;
         it              = it->next) {
        auto* templ = static_cast<GstStaticPadTemplate*>(it->data);
        if (templ == nullptr || templ->direction != GST_PAD_SRC) continue;
        caps = gst_static_pad_template_get_caps(templ);
        if (caps != nullptr) break;
    }
    gst_object_unref(factory);
    if (caps == nullptr) return std::nullopt;

    DmabufCapsInfo info;
    for (guint i = 0; i < gst_caps_get_size(caps); ++i) {
        const GstCapsFeatures* features = gst_caps_get_features(caps, i);
        if (features == nullptr ||
            ! gst_caps_features_contains(features, GST_CAPS_FEATURE_MEMORY_DMABUF)) {
            continue;
        }

        const GstStructure* structure = gst_caps_get_structure(caps, i);
        if (structure == nullptr) continue;
        auto drm_formats =
            CollectDrmFormatStrings(gst_structure_get_value(structure, "drm-format"));
        info.drm_formats.insert(info.drm_formats.end(), drm_formats.begin(), drm_formats.end());
    }

    gst_caps_unref(caps);
    return info;
}

std::optional<std::string>
SelectDmabufDrmFormat(const DmabufCapsInfo& caps, bool consumer_formats_known,
                      std::span<const DmabufFormatModifier> consumer_formats) {
    for (const auto& drm_format : caps.drm_formats) {
        guint64       modifier = DRM_FORMAT_MOD_INVALID;
        const guint32 fourcc = gst_video_dma_drm_fourcc_from_string(drm_format.c_str(), &modifier);
        if (! IsTextureOutputCompatibleDrmFourcc(fourcc)) continue;
        if (consumer_formats_known && ! SupportsDmabufFormat(consumer_formats, fourcc, modifier)) {
            continue;
        }
        return drm_format;
    }
    return std::nullopt;
}

std::optional<std::string> SelectedLinearDrmFormatFromCaps(GstCaps* caps, guint32 drm_fourcc) {
    if (caps == nullptr) return std::nullopt;

    gchar* linear_text = gst_video_dma_drm_fourcc_to_string(drm_fourcc, DRM_FORMAT_MOD_LINEAR);
    if (linear_text == nullptr) return std::nullopt;

    const std::string linear_format = linear_text;
    g_free(linear_text);
    const std::string base_linear_format = linear_format.substr(0, linear_format.find(':'));

    for (guint i = 0; i < gst_caps_get_size(caps); ++i) {
        const GstStructure* structure = gst_caps_get_structure(caps, i);
        if (structure == nullptr) continue;
        auto drm_formats =
            CollectDrmFormatStrings(gst_structure_get_value(structure, "drm-format"));
        for (const auto& drm_format : drm_formats) {
            if (drm_format == linear_format || drm_format == base_linear_format) {
                return drm_format;
            }
        }
    }

    return std::nullopt;
}

bool ResolveDmabufModifier(GstCaps* caps, guint32 drm_fourcc, guint64 drm_modifier,
                           std::uint64_t& resolved_modifier) {
    if (drm_modifier != DRM_FORMAT_MOD_INVALID) {
        resolved_modifier = drm_modifier;
        return true;
    }

    const auto linear_format = SelectedLinearDrmFormatFromCaps(caps, drm_fourcc);
    if (! linear_format.has_value()) return false;

    resolved_modifier = static_cast<std::uint64_t>(DRM_FORMAT_MOD_LINEAR);
    return true;
}

bool GetPlaneLayout(const GstVideoInfoDmaDrm& drm_info, GstVideoMeta* meta, guint plane_index,
                    gsize& plane_offset, gint& plane_stride) {
    if (meta != nullptr && plane_index < meta->n_planes) {
        plane_offset = static_cast<gsize>(meta->offset[plane_index]);
        plane_stride = meta->stride[plane_index];
        return plane_stride > 0;
    }

    plane_offset = static_cast<gsize>(GST_VIDEO_INFO_PLANE_OFFSET(&drm_info.vinfo, plane_index));
    plane_stride = GST_VIDEO_INFO_PLANE_STRIDE(&drm_info.vinfo, plane_index);
    return plane_stride > 0;
}

std::string BuildDmabufSampleLogSignature(GstCaps* caps, const GstVideoInfoDmaDrm& drm_info,
                                          guint plane_count, guint memory_count,
                                          const VideoDmabufFrame& frame) {
    gchar*      caps_text = caps != nullptr ? gst_caps_to_string(caps) : nullptr;
    std::string signature = caps_text != nullptr ? caps_text : "<null>";
    if (caps_text != nullptr) g_free(caps_text);

    signature += "|fourcc=" + std::to_string(drm_info.drm_fourcc);
    signature += "|modifier=" + std::to_string(frame.modifier);
    signature += "|planes=" + std::to_string(plane_count);
    signature += "|memories=" + std::to_string(memory_count);
    for (guint i = 0; i < plane_count && i < frame.plane_count; ++i) {
        signature += "|p" + std::to_string(i);
        signature += ":stride=" + std::to_string(frame.planes[i].stride);
        signature += ",offset=" + std::to_string(frame.planes[i].offset);
    }
    return signature;
}

void LogDmabufSampleDiagnostics(GstCaps* caps, const GstVideoInfoDmaDrm& drm_info,
                                guint plane_count, guint memory_count,
                                const VideoDmabufFrame* frame) {
    gchar* caps_text = caps != nullptr ? gst_caps_to_string(caps) : nullptr;
    std::fprintf(stderr,
                 "video-backend[debug]: pipeline-mode=dmabuf sample-caps=%s drm-fourcc=0x%08x "
                 "modifier=0x%016" PRIx64 " n-planes=%u memory-count=%u\n",
                 caps_text != nullptr ? caps_text : "<null>",
                 drm_info.drm_fourcc,
                 frame != nullptr ? frame->modifier
                                  : static_cast<std::uint64_t>(drm_info.drm_modifier),
                 plane_count,
                 memory_count);
    if (caps_text != nullptr) g_free(caps_text);

    if (frame == nullptr) return;
    for (guint i = 0; i < plane_count && i < frame->plane_count; ++i) {
        std::fprintf(stderr,
                     "video-backend[debug]: plane=%u fd=%d stride=%u offset=%u\n",
                     i,
                     frame->planes[i].fd,
                     frame->planes[i].stride,
                     frame->planes[i].offset);
    }
}

bool ParseDmabufSample(GstSample* sample, VideoDmabufFrame& frame,
                       std::optional<std::string>& last_logged_signature) {
    if (sample == nullptr) return false;

    GstCaps*   caps   = gst_sample_get_caps(sample);
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (caps == nullptr || buffer == nullptr || ! gst_video_is_dma_drm_caps(caps)) {
        std::fprintf(stderr, "video-backend: sample is not DMA_DRM caps\n");
        return false;
    }

    GstVideoInfoDmaDrm drm_info;
    gst_video_info_dma_drm_init(&drm_info);
    if (! gst_video_info_dma_drm_from_caps(&drm_info, caps)) {
        std::fprintf(stderr, "video-backend: gst_video_info_dma_drm_from_caps failed\n");
        return false;
    }

    const gint                width      = GST_VIDEO_INFO_WIDTH(&drm_info.vinfo);
    const gint                height     = GST_VIDEO_INFO_HEIGHT(&drm_info.vinfo);
    GstVideoMeta*             meta       = gst_buffer_get_video_meta(buffer);
    const GstVideoFormat      drm_format = gst_video_dma_drm_fourcc_to_format(drm_info.drm_fourcc);
    const GstVideoFormatInfo* drm_format_info =
        drm_format != GST_VIDEO_FORMAT_UNKNOWN ? gst_video_format_get_info(drm_format) : nullptr;
    const guint plane_count =
        meta != nullptr && meta->n_planes > 0
            ? meta->n_planes
            : (drm_format_info != nullptr ? GST_VIDEO_FORMAT_INFO_N_PLANES(drm_format_info)
                                          : GST_VIDEO_INFO_N_PLANES(&drm_info.vinfo));
    const guint memory_count = gst_buffer_n_memory(buffer);
    if (width <= 0 || height <= 0 || plane_count == 0 || plane_count > 4 || memory_count == 0) {
        std::fprintf(
            stderr,
            "video-backend: invalid dimensions/planes width=%d height=%d planes=%u memories=%u\n",
            width,
            height,
            plane_count,
            memory_count);
        return false;
    }

    frame             = {};
    frame.width       = static_cast<std::uint32_t>(width);
    frame.height      = static_cast<std::uint32_t>(height);
    frame.plane_count = plane_count;
    frame.drm_fourcc  = drm_info.drm_fourcc;
    if (! ResolveDmabufModifier(caps, drm_info.drm_fourcc, drm_info.drm_modifier, frame.modifier)) {
        std::fprintf(stderr,
                     "video-backend: DRM_FORMAT_MOD_INVALID without explicit linear caps for "
                     "fourcc=0x%08x\n",
                     drm_info.drm_fourcc);
        return false;
    }

    for (guint i = 0; i < plane_count; ++i) {
        gsize plane_offset = 0;
        gint  plane_stride = 0;
        if (! GetPlaneLayout(drm_info, meta, i, plane_offset, plane_stride)) {
            std::fprintf(stderr, "video-backend: plane %u missing valid layout metadata\n", i);
            return false;
        }

        guint memory_index = 0;
        guint memory_span  = 0;
        gsize memory_skip  = 0;
        if (! gst_buffer_find_memory(
                buffer, plane_offset, 1, &memory_index, &memory_span, &memory_skip) ||
            memory_span == 0 || memory_index >= memory_count) {
            std::fprintf(stderr,
                         "video-backend: plane %u offset=%zu could not map into buffer memory\n",
                         i,
                         plane_offset);
            return false;
        }

        GstMemory* memory = gst_buffer_peek_memory(buffer, memory_index);
        if (memory == nullptr || ! gst_is_dmabuf_memory(memory)) {
            std::fprintf(stderr,
                         "video-backend: plane %u memory_index=%u is not dmabuf "
                         "(memory_count=%u)\n",
                         i,
                         memory_index,
                         memory_count);
            return false;
        }

        gsize memory_offset = 0;
        gsize memory_size   = 0;
        (void)gst_memory_get_sizes(memory, &memory_offset, &memory_size);
        (void)memory_size;

        frame.planes[i].fd     = gst_dmabuf_memory_get_fd(memory);
        frame.planes[i].stride = static_cast<std::uint32_t>(plane_stride);
        frame.planes[i].offset = static_cast<std::uint32_t>(memory_offset + memory_skip);
        if (frame.planes[i].fd < 0 || frame.planes[i].stride == 0) {
            std::fprintf(stderr,
                         "video-backend: plane %u invalid fd=%d stride=%u offset=%u\n",
                         i,
                         frame.planes[i].fd,
                         frame.planes[i].stride,
                         frame.planes[i].offset);
            return false;
        }
    }

    const std::string signature =
        BuildDmabufSampleLogSignature(caps, drm_info, plane_count, memory_count, frame);
    if (! last_logged_signature.has_value() || last_logged_signature.value() != signature) {
        LogDmabufSampleDiagnostics(caps, drm_info, plane_count, memory_count, &frame);
        last_logged_signature = std::move(signature);
    }
    return true;
}

bool PublishShmSample(VideoFrameSwapchain& swapchain, GstSample* sample) {
    if (sample == nullptr) return false;

    GstCaps*     caps   = gst_sample_get_caps(sample);
    GstBuffer*   buffer = gst_sample_get_buffer(sample);
    GstVideoInfo info;
    if (caps == nullptr || buffer == nullptr || ! gst_video_info_from_caps(&info, caps)) {
        return false;
    }
    if (GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_BGRA) return false;

    GstVideoFrame frame;
    if (! gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) return false;

    const bool published =
        swapchain.publishFrame(frame.data[0],
                               static_cast<std::uint32_t>(GST_VIDEO_INFO_WIDTH(&info)),
                               static_cast<std::uint32_t>(GST_VIDEO_INFO_HEIGHT(&info)),
                               static_cast<std::uint32_t>(frame.info.stride[0]));
    gst_video_frame_unmap(&frame);
    return published;
}

gboolean OnAppSinkProposeAllocation(GstAppSink*, GstQuery* query, gpointer) {
    if (query == nullptr) return FALSE;
    gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, nullptr);
    return TRUE;
}

} // namespace

VideoBackend::VideoBackend(const BackendContext& context)
    : m_context(context), m_sharedState(std::make_shared<SharedState>()) {
    if (! gst_is_initialized()) gst_init(nullptr, nullptr);

    auto weakSharedState = std::weak_ptr<SharedState>(m_sharedState);
    auto plan =
        std::make_shared<VideoRenderPlan>([this, weakSharedState](const OutputTarget& target) {
            auto shared = weakSharedState.lock();
            if (! shared) {
                return Result<void>::failure(ResultCode::InvalidState,
                                             "video backend was destroyed before bind");
            }
            auto binding = std::dynamic_pointer_cast<VideoOutputBinding>(target.binding);
            if (! binding) {
                return Result<void>::failure(ResultCode::InvalidArgument,
                                             "video render plan requires a VideoOutputBinding");
            }
            const auto&        renderInfo = binding->renderInitInfo();
            const PipelineMode requestedMode =
                renderInfo.export_mode == ExternalFrameExportMode::SHM ? PipelineMode::Shm
                                                                       : PipelineMode::Dmabuf;
            const bool pipelineConfigChanged =
                requestedMode != m_preferredPipelineMode ||
                renderInfo.allow_shm_fallback != m_allowShmFallback ||
                renderInfo.consumer_dmabuf_formats_known != m_consumerDmabufFormatsKnown ||
                renderInfo.consumer_dmabuf_formats != m_consumerDmabufFormats;

            if (m_renderBinding) m_renderBinding->attachSwapchain(nullptr);
            m_frameSwapchain.reset();
            m_renderBinding              = std::move(binding);
            m_preferredPipelineMode      = requestedMode;
            m_allowShmFallback           = renderInfo.allow_shm_fallback;
            m_consumerDmabufFormatsKnown = renderInfo.consumer_dmabuf_formats_known;
            m_consumerDmabufFormats      = renderInfo.consumer_dmabuf_formats;
            m_frameSwapchain =
                std::make_unique<VideoFrameSwapchain>(renderInfo.width, renderInfo.height);
            m_renderBinding->attachSwapchain(m_frameSwapchain.get());

            if (m_pipeline != nullptr && pipelineConfigChanged) destroyPipeline();
            if (m_started && m_pipeline == nullptr) {
                auto rebuildResult = ensurePipeline();
                if (! rebuildResult) return rebuildResult;
                auto stateResult = applyPlaybackState();
                if (! stateResult) return stateResult;
            }
            shared->outputBound.store(true);
            shared->outputStateChanged.store(true);
            if (shared->readyState.load() == BackendReadyState::Loaded) {
                shared->readyState.store(BackendReadyState::OutputReady);
                shared->contentStateChanged.store(true);
            }
            return Result<void>::success();
        });
    m_outputSource = std::make_unique<VideoOutputSource>(plan);
}

VideoBackend::~VideoBackend() { destroyPipeline(); }

BackendType VideoBackend::type() const { return BackendType::Video; }

BackendCapabilities VideoBackend::capabilities() const {
    BackendCapabilities capabilities;
    capabilities.supportsProperties = true;
    return capabilities;
}

Result<void> VideoBackend::load(const WallpaperSource& source) {
    m_sharedState->readyState.store(BackendReadyState::Loading);
    m_sharedState->outputBound.store(false);
    m_sharedState->contentStateChanged.store(true);
    m_sharedState->outputStateChanged.store(false);
    m_sharedState->frameRequested.store(false);

    destroyPipeline();
    m_diagnostics.entries.clear();
    m_sourcePath = source.uri;
    m_selectedDmabufDrmFormat.reset();
    m_lastLoggedDmabufSampleSignature.reset();
    m_preferredPipelineMode = PipelineMode::Dmabuf;
    m_paused                = false;
    m_started               = false;
    m_eos                   = false;
    m_volume                = 1.0f;
    m_muted                 = false;
    m_speed                 = 1.0f;

    std::error_code ec;
    if (m_sourcePath.empty() || ! std::filesystem::exists(m_sourcePath, ec) || ec) {
        m_sharedState->readyState.store(BackendReadyState::Error);
        return Result<void>::failure(ResultCode::NotFound,
                                     "video backend source file does not exist: " + source.uri);
    }

    for (const auto& [name, value] : source.initialProperties) {
        auto propertyResult = setProperty(name, value);
        if (! propertyResult) {
            m_sharedState->readyState.store(BackendReadyState::Error);
            return propertyResult;
        }
    }

    m_sharedState->readyState.store(BackendReadyState::Loaded);
    return Result<void>::success();
}

Result<void> VideoBackend::start() {
    auto pipelineResult = ensurePipeline();
    if (! pipelineResult) return pipelineResult;

    m_paused         = false;
    m_started        = true;
    auto stateResult = applyPlaybackState();
    if (! stateResult) return stateResult;

    m_sharedState->readyState.store(BackendReadyState::OutputReady);
    m_sharedState->contentStateChanged.store(true);
    return Result<void>::success();
}

Result<void> VideoBackend::pause() {
    m_paused = true;
    return applyPlaybackState();
}

Result<void> VideoBackend::resume() {
    if (! m_started) return start();
    m_paused = false;
    return applyPlaybackState();
}

Result<void> VideoBackend::stop() {
    destroyPipeline();
    const bool outputBound = m_frameSwapchain != nullptr;
    m_sharedState->readyState.store(outputBound ? BackendReadyState::OutputReady
                                                : BackendReadyState::Loaded);
    m_sharedState->outputBound.store(outputBound);
    m_sharedState->frameRequested.store(false);
    m_paused  = false;
    m_started = false;
    m_eos     = false;
    return Result<void>::success();
}

Result<void> VideoBackend::setProperty(std::string_view name, PropertyValue value) {
    if (name == WE_SCENE_PROPERTY_VOLUME) {
        if (const auto* volume = std::get_if<float>(&value)) {
            m_volume = std::clamp(*volume, 0.0f, 10.0f);
            applyPlaybackProperties();
            return Result<void>::success();
        }
        if (const auto* volume = std::get_if<double>(&value)) {
            m_volume = std::clamp(static_cast<float>(*volume), 0.0f, 10.0f);
            applyPlaybackProperties();
            return Result<void>::success();
        }
    } else if (name == WE_SCENE_PROPERTY_MUTED) {
        if (const auto* muted = std::get_if<bool>(&value)) {
            m_muted = *muted;
            applyPlaybackProperties();
            return Result<void>::success();
        }
    } else if (name == WE_SCENE_PROPERTY_SPEED) {
        if (const auto* speed = std::get_if<float>(&value)) {
            if (*speed <= 0.0f) {
                return Result<void>::failure(ResultCode::InvalidArgument,
                                             "video playback speed must be positive");
            }
            m_speed = *speed;
            if (m_speed != 1.0f) {
                appendDiagnostic(
                    DiagnosticSeverity::Warning,
                    "video backend accepted speed property but currently plays at normal rate");
            }
            return Result<void>::success();
        }
    } else if (name == WE_SCENE_PROPERTY_FPS || name == WE_SCENE_PROPERTY_ASSETS ||
               name == WE_SCENE_PROPERTY_SOURCE) {
        return Result<void>::success();
    }

    appendDiagnostic(DiagnosticSeverity::Warning,
                     "video backend ignored unsupported property: " + std::string(name));
    return Result<void>::success();
}

Result<void> VideoBackend::sendInput(const InputEvent&) { return unsupportedInput(); }

Result<void> VideoBackend::update() { return Result<void>::success(); }

Result<OutputSource*> VideoBackend::acquireOutput() {
    return Result<OutputSource*>::success(m_outputSource.get());
}

Result<FrameLifecycle> VideoBackend::tick() {
    drainBus();

    if (m_appsink != nullptr) {
        GstSample* latestSample = nullptr;
        while (GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(m_appsink), 0)) {
            if (latestSample != nullptr) gst_sample_unref(latestSample);
            latestSample = sample;
        }
        if (latestSample != nullptr) {
            auto publishResult = publishSample(latestSample);
            gst_sample_unref(latestSample);
            if (! publishResult) return Result<FrameLifecycle>(publishResult.error());
        }
    }

    FrameLifecycle lifecycle;
    lifecycle.contentStateChanged = m_sharedState->contentStateChanged.exchange(false);
    lifecycle.outputStateChanged  = m_sharedState->outputStateChanged.exchange(false);
    lifecycle.frameRequested      = m_sharedState->frameRequested.exchange(false);
    return Result<FrameLifecycle>::success(lifecycle);
}

BackendReadyState VideoBackend::readyState() const { return m_sharedState->readyState.load(); }

void VideoBackend::notifyOutputBound() {
    m_sharedState->outputBound.store(true);
    m_sharedState->outputStateChanged.store(true);
    if (m_sharedState->readyState.load() == BackendReadyState::Loaded) {
        m_sharedState->readyState.store(BackendReadyState::OutputReady);
        m_sharedState->contentStateChanged.store(true);
    }
}

OutputSource& VideoBackend::outputSource() { return *m_outputSource; }

DiagnosticsSnapshot VideoBackend::diagnostics() const { return m_diagnostics; }

void VideoBackend::appendDiagnostic(DiagnosticSeverity severity, std::string message) {
    m_diagnostics.append(severity, "backend.video", std::move(message));
}

Result<void> VideoBackend::ensurePipeline() {
    if (m_pipeline != nullptr) return Result<void>::success();
    return buildPipeline(m_preferredPipelineMode);
}

void VideoBackend::destroyPipeline() {
    if (m_pipeline != nullptr) (void)gst_element_set_state(m_pipeline, GST_STATE_NULL);
    if (m_bus != nullptr) gst_object_unref(m_bus);
    if (m_appsink != nullptr) gst_object_unref(m_appsink);
    if (m_pipeline != nullptr) gst_object_unref(m_pipeline);
    m_pipeline = nullptr;
    m_appsink  = nullptr;
    m_bus      = nullptr;
}

Result<void> VideoBackend::buildPipeline(PipelineMode requestedMode) {
    destroyPipeline();
    m_selectedDmabufDrmFormat.reset();
    m_lastLoggedDmabufSampleSignature.reset();
    m_eos = false;

    PipelineMode mode = requestedMode;
    if (mode == PipelineMode::Dmabuf) {
        const auto postprocCaps = QueryVaDmabufPostprocCaps();
        if (postprocCaps.has_value()) {
            m_selectedDmabufDrmFormat = SelectDmabufDrmFormat(
                *postprocCaps, m_consumerDmabufFormatsKnown, m_consumerDmabufFormats);
        }

        if (! m_selectedDmabufDrmFormat.has_value()) {
            const std::string reason =
                postprocCaps.has_value()
                    ? "vapostproc exposes no RGB DMA-BUF format accepted by the output"
                    : "vapostproc is unavailable";
            if (! m_allowShmFallback) return unsupportedDmabuf(reason);
            appendDiagnostic(DiagnosticSeverity::Warning,
                             "video backend DMA-BUF output is unavailable (" + reason +
                                 "); using SHM");
            mode = PipelineMode::Shm;
        }
    }

    auto result = createPipeline(mode);
    if (result || mode != PipelineMode::Dmabuf || ! m_allowShmFallback) return result;

    appendDiagnostic(DiagnosticSeverity::Warning,
                     "video backend DMA-BUF pipeline failed (" + result.error().message +
                         "); using SHM");
    destroyPipeline();
    m_selectedDmabufDrmFormat.reset();
    m_lastLoggedDmabufSampleSignature.reset();
    return createPipeline(PipelineMode::Shm);
}

Result<void> VideoBackend::createPipeline(PipelineMode mode) {
    destroyPipeline();
    m_pipelineMode = mode;

    const char* playbinFactory = "playbin3";
    m_pipeline                 = gst_element_factory_make(playbinFactory, "video-player");
    if (m_pipeline == nullptr) {
        playbinFactory = "playbin";
        m_pipeline     = gst_element_factory_make(playbinFactory, "video-player");
    }
    if (m_pipeline == nullptr) {
        return Result<void>::failure(ResultCode::NotSupported,
                                     "video backend requires GStreamer playbin3 or playbin");
    }
    gst_object_ref_sink(m_pipeline);

    GError* uriError = nullptr;
    gchar*  uri      = gst_filename_to_uri(m_sourcePath.string().c_str(), &uriError);
    if (uri == nullptr) {
        const std::string message =
            uriError != nullptr ? uriError->message : "could not convert source path to URI";
        if (uriError != nullptr) g_error_free(uriError);
        destroyPipeline();
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "video backend source URI creation failed: " + message);
    }

    std::string sinkDescription;
    if (mode == PipelineMode::Dmabuf) {
        if (! m_selectedDmabufDrmFormat.has_value()) {
            g_free(uri);
            destroyPipeline();
            return Result<void>::failure(
                ResultCode::InternalError,
                "DMA-BUF pipeline selected without a negotiated DRM format");
        }
        sinkDescription =
            "vapostproc "
            "! capsfilter "
            "caps=\"video/x-raw(memory:DMABuf),format=(string)DMA_DRM,drm-format=(string)" +
            EscapeGstPropertyString(*m_selectedDmabufDrmFormat) +
            "\" "
            "! appsink name=sink sync=true max-buffers=1 drop=true wait-on-eos=false "
            "enable-last-sample=false";
        std::fprintf(stderr,
                     "video-backend[debug]: source=playbin pipeline-mode=dmabuf "
                     "selected-drm-format=%s\n",
                     m_selectedDmabufDrmFormat->c_str());
    } else {
        std::uint32_t targetWidth  = 0;
        std::uint32_t targetHeight = 0;
        if (m_renderBinding) {
            const auto& renderInfo = m_renderBinding->renderInitInfo();
            targetWidth            = renderInfo.width;
            targetHeight           = renderInfo.height;
        }

        sinkDescription = "videoconvert ! videoscale ";
        if (targetWidth > 0 && targetHeight > 0) {
            sinkDescription += "! video/x-raw,format=(string)BGRA,width=(int)" +
                               std::to_string(targetWidth) + ",height=(int)" +
                               std::to_string(targetHeight) + " ";
        } else {
            sinkDescription += "! video/x-raw,format=(string)BGRA ";
        }
        sinkDescription +=
            "! appsink name=sink sync=true max-buffers=1 drop=true wait-on-eos=false "
            "enable-last-sample=false";
        std::fprintf(stderr,
                     "video-backend[debug]: source=playbin pipeline-mode=shm target=%ux%u\n",
                     static_cast<unsigned>(targetWidth),
                     static_cast<unsigned>(targetHeight));
    }

    GError*     sinkError = nullptr;
    GstElement* videoSink =
        gst_parse_bin_from_description(sinkDescription.c_str(), TRUE, &sinkError);
    if (videoSink != nullptr) gst_object_ref_sink(videoSink);
    if (videoSink == nullptr || sinkError != nullptr) {
        const std::string message =
            sinkError != nullptr ? sinkError->message : "unknown sink parse error";
        if (sinkError != nullptr) g_error_free(sinkError);
        if (videoSink != nullptr) gst_object_unref(videoSink);
        g_free(uri);
        destroyPipeline();
        return Result<void>::failure(ResultCode::InternalError,
                                     "video backend could not create video sink: " + message);
    }

    m_appsink = gst_bin_get_by_name(GST_BIN(videoSink), "sink");
    if (m_appsink == nullptr) {
        gst_object_unref(videoSink);
        g_free(uri);
        destroyPipeline();
        return Result<void>::failure(ResultCode::InternalError,
                                     "video backend video sink is missing appsink");
    }

    GstAppSinkCallbacks callbacks {};
    callbacks.propose_allocation = OnAppSinkProposeAllocation;
    gst_app_sink_set_callbacks(GST_APP_SINK(m_appsink), &callbacks, nullptr, nullptr);
    g_object_set(m_appsink, "emit-signals", FALSE, nullptr);

    g_object_set(m_pipeline,
                 "uri",
                 uri,
                 "video-sink",
                 videoSink,
                 "volume",
                 static_cast<gdouble>(m_volume),
                 "mute",
                 static_cast<gboolean>(m_muted),
                 nullptr);
    gst_object_unref(videoSink);
    g_free(uri);

    m_bus = gst_element_get_bus(m_pipeline);
    if (m_bus == nullptr) {
        destroyPipeline();
        return Result<void>::failure(ResultCode::InternalError,
                                     "video backend could not create GStreamer bus");
    }

    const auto failureMessage = [this](std::string_view fallback) {
        GstMessage* message = gst_bus_pop_filtered(
            m_bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        if (message == nullptr) return std::string(fallback);

        std::string result(fallback);
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            GError* error = nullptr;
            gchar*  debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            if (error != nullptr) result = error->message;
            if (debug != nullptr && *debug != '\0') result += " [" + std::string(debug) + "]";
            if (error != nullptr) g_error_free(error);
            if (debug != nullptr) g_free(debug);
        } else {
            result = "video stream reached EOS before producing a frame";
        }
        gst_message_unref(message);
        return result;
    };

    if (gst_element_set_state(m_pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
        const auto message = failureMessage("failed to enter PAUSED state");
        destroyPipeline();
        return Result<void>::failure(ResultCode::InternalError,
                                     "video backend preroll failed: " + message);
    }

    const GstStateChangeReturn stateResult =
        gst_element_get_state(m_pipeline, nullptr, nullptr, 5 * GST_SECOND);
    if (stateResult == GST_STATE_CHANGE_FAILURE) {
        const auto message = failureMessage("GStreamer state change failed during preroll");
        destroyPipeline();
        return Result<void>::failure(ResultCode::InternalError,
                                     "video backend preroll failed: " + message);
    }

    GstSample* preroll = gst_app_sink_try_pull_preroll(GST_APP_SINK(m_appsink), 5 * GST_SECOND);
    if (preroll == nullptr) {
        const auto message = failureMessage("timed out waiting for the first decoded video frame");
        destroyPipeline();
        return Result<void>::failure(ResultCode::InternalError,
                                     "video backend preroll failed: " + message);
    }

    auto publishResult = publishSample(preroll);
    gst_sample_unref(preroll);
    if (! publishResult) {
        destroyPipeline();
        return publishResult;
    }

    return Result<void>::success();
}

Result<void> VideoBackend::applyPlaybackState() {
    if (m_pipeline == nullptr) return Result<void>::success();

    const GstState targetState = m_paused ? GST_STATE_PAUSED : GST_STATE_PLAYING;
    if (gst_element_set_state(m_pipeline, targetState) == GST_STATE_CHANGE_FAILURE) {
        return Result<void>::failure(ResultCode::InternalError,
                                     "video backend failed to change playback state");
    }
    return Result<void>::success();
}

void VideoBackend::applyPlaybackProperties() {
    if (m_pipeline == nullptr) return;
    g_object_set(m_pipeline,
                 "volume",
                 static_cast<gdouble>(m_volume),
                 "mute",
                 static_cast<gboolean>(m_muted),
                 nullptr);
}

Result<void> VideoBackend::publishSample(GstSample* sample) {
    if (m_frameSwapchain == nullptr) return Result<void>::success();

    if (m_pipelineMode == PipelineMode::Dmabuf) {
        VideoDmabufFrame frame;
        if (! ParseDmabufSample(sample, frame, m_lastLoggedDmabufSampleSignature)) {
            return Result<void>::failure(ResultCode::NotSupported,
                                         "video backend could not translate DMA-BUF sample");
        }
        if (! m_frameSwapchain->publishFrame(frame)) {
            return Result<void>::failure(ResultCode::InternalError,
                                         "video backend failed to publish DMA-BUF frame");
        }
        m_sharedState->frameRequested.store(true);
        return Result<void>::success();
    }

    if (! PublishShmSample(*m_frameSwapchain, sample)) {
        return Result<void>::failure(ResultCode::InternalError,
                                     "video backend failed to publish SHM frame");
    }
    m_sharedState->frameRequested.store(true);
    return Result<void>::success();
}

void VideoBackend::drainBus() {
    if (m_bus == nullptr) return;

    while (GstMessage* message = gst_bus_pop(m_bus)) {
        switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: {
            GError* error = nullptr;
            gchar*  debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            const std::string messageText =
                error != nullptr ? error->message : "unknown GStreamer error";
            appendDiagnostic(DiagnosticSeverity::Error,
                             "video backend pipeline error: " + messageText);
            if (error != nullptr) g_error_free(error);
            if (debug != nullptr) g_free(debug);
            m_sharedState->readyState.store(BackendReadyState::Error);
            break;
        }
        case GST_MESSAGE_EOS:
            m_eos = true;
            if (m_pipeline != nullptr) {
                (void)gst_element_seek_simple(
                    m_pipeline,
                    GST_FORMAT_TIME,
                    static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
                    0);
                if (! m_paused) (void)gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
            }
            break;
        default: break;
        }
        gst_message_unref(message);
    }
}
} // namespace wallpaper
