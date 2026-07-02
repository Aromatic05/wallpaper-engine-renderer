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

Result<void> unsupportedVaH264Dmabuf(std::string_view reason) {
    return Result<void>::failure(ResultCode::NotSupported,
                                 "DMA-BUF mode currently requires MP4/H.264/vah264dec: "
                                     + std::string(reason));
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

struct DecoderDmabufCapsInfo {
    std::vector<std::string> drm_formats;
    std::string              caps_string;
};

std::optional<DecoderDmabufCapsInfo> QueryVaDmabufDecoderCaps() {
    GstElementFactory* factory = gst_element_factory_find("vah264dec");
    if (factory == nullptr) return std::nullopt;

    GstCaps* caps = nullptr;
    for (const GList* it = gst_element_factory_get_static_pad_templates(factory); it != nullptr;
         it = it->next) {
        auto* templ = static_cast<GstStaticPadTemplate*>(it->data);
        if (templ == nullptr || templ->direction != GST_PAD_SRC) continue;
        caps = gst_static_pad_template_get_caps(templ);
        if (caps != nullptr) break;
    }
    gst_object_unref(factory);
    if (caps == nullptr) return std::nullopt;

    DecoderDmabufCapsInfo info;
    gchar* caps_text = gst_caps_to_string(caps);
    if (caps_text != nullptr) {
        info.caps_string = caps_text;
        g_free(caps_text);
    }

    for (guint i = 0; i < gst_caps_get_size(caps); ++i) {
        const GstCapsFeatures* features = gst_caps_get_features(caps, i);
        if (features == nullptr ||
            ! gst_caps_features_contains(features, GST_CAPS_FEATURE_MEMORY_DMABUF)) {
            continue;
        }

        const GstStructure* structure = gst_caps_get_structure(caps, i);
        if (structure == nullptr) continue;
        const GValue* drm_value = gst_structure_get_value(structure, "drm-format");
        auto drm_formats = CollectDrmFormatStrings(drm_value);
        info.drm_formats.insert(info.drm_formats.end(), drm_formats.begin(), drm_formats.end());
    }

    gst_caps_unref(caps);
    return info;
}

std::optional<std::string> SelectedLinearDrmFormatFromCaps(GstCaps* caps, guint32 drm_fourcc) {
    if (caps == nullptr) return std::nullopt;

    gchar* linear_text = gst_video_dma_drm_fourcc_to_string(drm_fourcc, DRM_FORMAT_MOD_LINEAR);
    if (linear_text == nullptr) return std::nullopt;

    const std::string linear_format = linear_text;
    g_free(linear_text);
    const std::string base_linear_format =
        linear_format.substr(0, linear_format.find(':'));

    for (guint i = 0; i < gst_caps_get_size(caps); ++i) {
        const GstStructure* structure = gst_caps_get_structure(caps, i);
        if (structure == nullptr) continue;
        auto drm_formats = CollectDrmFormatStrings(gst_structure_get_value(structure, "drm-format"));
        for (const auto& drm_format : drm_formats) {
            if (drm_format == linear_format || drm_format == base_linear_format) {
                return drm_format;
            }
        }
    }

    return std::nullopt;
}

bool ResolveDmabufModifier(GstCaps* caps,
                           guint32 drm_fourcc,
                           guint64 drm_modifier,
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

bool GetPlaneLayout(const GstVideoInfoDmaDrm& drm_info,
                    GstVideoMeta* meta,
                    guint plane_index,
                    gsize& plane_offset,
                    gint& plane_stride) {
    if (meta != nullptr && plane_index < meta->n_planes) {
        plane_offset = static_cast<gsize>(meta->offset[plane_index]);
        plane_stride = meta->stride[plane_index];
        return plane_stride > 0;
    }

    plane_offset = static_cast<gsize>(GST_VIDEO_INFO_PLANE_OFFSET(&drm_info.vinfo, plane_index));
    plane_stride = GST_VIDEO_INFO_PLANE_STRIDE(&drm_info.vinfo, plane_index);
    return plane_stride > 0;
}

std::string BuildDmabufSampleLogSignature(GstCaps* caps,
                                          const GstVideoInfoDmaDrm& drm_info,
                                          guint plane_count,
                                          guint memory_count,
                                          const VideoDmabufFrame& frame) {
    gchar* caps_text = caps != nullptr ? gst_caps_to_string(caps) : nullptr;
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

void LogDmabufSampleDiagnostics(GstCaps* caps,
                                const GstVideoInfoDmaDrm& drm_info,
                                guint plane_count,
                                guint memory_count,
                                const VideoDmabufFrame* frame) {
    gchar* caps_text = caps != nullptr ? gst_caps_to_string(caps) : nullptr;
    std::fprintf(stderr,
                 "video-backend[debug]: pipeline-mode=dmabuf sample-caps=%s drm-fourcc=0x%08x "
                 "modifier=0x%016" PRIx64 " n-planes=%u memory-count=%u\n",
                 caps_text != nullptr ? caps_text : "<null>",
                 drm_info.drm_fourcc,
                 frame != nullptr ? frame->modifier : static_cast<std::uint64_t>(drm_info.drm_modifier),
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

bool ParseDmabufSample(GstSample* sample,
                       VideoDmabufFrame& frame,
                       std::optional<std::string>& last_logged_signature) {
    if (sample == nullptr) return false;

    GstCaps* caps = gst_sample_get_caps(sample);
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

    const gint width = GST_VIDEO_INFO_WIDTH(&drm_info.vinfo);
    const gint height = GST_VIDEO_INFO_HEIGHT(&drm_info.vinfo);
    GstVideoMeta* meta = gst_buffer_get_video_meta(buffer);
    const GstVideoFormat drm_format = gst_video_dma_drm_fourcc_to_format(drm_info.drm_fourcc);
    const GstVideoFormatInfo* drm_format_info =
        drm_format != GST_VIDEO_FORMAT_UNKNOWN ? gst_video_format_get_info(drm_format) : nullptr;
    const guint plane_count = meta != nullptr && meta->n_planes > 0
        ? meta->n_planes
        : (drm_format_info != nullptr ? GST_VIDEO_FORMAT_INFO_N_PLANES(drm_format_info)
                                      : GST_VIDEO_INFO_N_PLANES(&drm_info.vinfo));
    const guint memory_count = gst_buffer_n_memory(buffer);
    if (width <= 0 || height <= 0 || plane_count == 0 || plane_count > 4 || memory_count == 0) {
        std::fprintf(stderr,
                     "video-backend: invalid dimensions/planes width=%d height=%d planes=%u memories=%u\n",
                     width,
                     height,
                     plane_count,
                     memory_count);
        return false;
    }

    frame = {};
    frame.width = static_cast<std::uint32_t>(width);
    frame.height = static_cast<std::uint32_t>(height);
    frame.plane_count = plane_count;
    frame.drm_fourcc = drm_info.drm_fourcc;
    if (! ResolveDmabufModifier(caps, drm_info.drm_fourcc, drm_info.drm_modifier, frame.modifier)) {
        std::fprintf(stderr,
                     "video-backend: DRM_FORMAT_MOD_INVALID without explicit linear caps for "
                     "fourcc=0x%08x\n",
                     drm_info.drm_fourcc);
        return false;
    }

    for (guint i = 0; i < plane_count; ++i) {
        gsize plane_offset = 0;
        gint plane_stride = 0;
        if (! GetPlaneLayout(drm_info, meta, i, plane_offset, plane_stride)) {
            std::fprintf(stderr, "video-backend: plane %u missing valid layout metadata\n", i);
            return false;
        }

        guint memory_index = 0;
        guint memory_span = 0;
        gsize memory_skip = 0;
        if (! gst_buffer_find_memory(buffer, plane_offset, 1, &memory_index, &memory_span, &memory_skip) ||
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
        gsize memory_size = 0;
        (void)gst_memory_get_sizes(memory, &memory_offset, &memory_size);
        (void)memory_size;

        frame.planes[i].fd = gst_dmabuf_memory_get_fd(memory);
        frame.planes[i].stride = static_cast<std::uint32_t>(plane_stride);
        frame.planes[i].offset =
            static_cast<std::uint32_t>(memory_offset + memory_skip);
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

    GstCaps* caps = gst_sample_get_caps(sample);
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstVideoInfo info;
    if (caps == nullptr || buffer == nullptr || ! gst_video_info_from_caps(&info, caps)) {
        return false;
    }
    if (GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_BGRA) return false;

    GstVideoFrame frame;
    if (! gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) return false;

    const bool published = swapchain.publishFrame(frame.data[0],
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

gboolean OnAppSinkProposeAllocationSignal(GstElement*, GstQuery* query, gpointer user_data) {
    return OnAppSinkProposeAllocation(nullptr, query, user_data);
}
} // namespace

VideoBackend::VideoBackend(const BackendContext& context)
    : m_context(context)
    , m_sharedState(std::make_shared<SharedState>()) {
    if (! gst_is_initialized()) gst_init(nullptr, nullptr);

    auto weakSharedState = std::weak_ptr<SharedState>(m_sharedState);
    auto plan = std::make_shared<VideoRenderPlan>([this, weakSharedState](const OutputTarget& target) {
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
        m_renderBinding = std::move(binding);
        const auto& renderInfo = m_renderBinding->renderInitInfo();
        m_preferredPipelineMode = renderInfo.export_mode == ExternalFrameExportMode::SHM
            ? PipelineMode::Shm
            : PipelineMode::Dmabuf;
        m_frameSwapchain = std::make_unique<VideoFrameSwapchain>(renderInfo.width, renderInfo.height);
        m_renderBinding->attachSwapchain(m_frameSwapchain.get());
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
    capabilities.supportsRenderPlan = true;
    capabilities.supportsTextureOutput = false;
    capabilities.supportsSurfaceOutput = false;
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
    m_paused = false;
    m_started = false;
    m_eos = false;
    m_volume = 1.0f;
    m_muted = false;
    m_speed = 1.0f;

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

    m_paused = false;
    m_started = true;
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
    if (m_renderBinding) m_renderBinding->attachSwapchain(nullptr);
    m_frameSwapchain.reset();
    m_sharedState->readyState.store(BackendReadyState::Idle);
    m_sharedState->outputBound.store(false);
    m_sharedState->frameRequested.store(false);
    m_paused = false;
    m_started = false;
    m_eos = false;
    return Result<void>::success();
}

Result<void> VideoBackend::setProperty(std::string_view name, PropertyValue value) {
    if (name == WE_SCENE_PROPERTY_VOLUME) {
        if (const auto* volume = std::get_if<float>(&value)) {
            m_volume = *volume;
            return Result<void>::success();
        }
        if (const auto* volume = std::get_if<double>(&value)) {
            m_volume = static_cast<float>(*volume);
            return Result<void>::success();
        }
    } else if (name == WE_SCENE_PROPERTY_MUTED) {
        if (const auto* muted = std::get_if<bool>(&value)) {
            m_muted = *muted;
            return Result<void>::success();
        }
    } else if (name == WE_SCENE_PROPERTY_SPEED) {
        if (const auto* speed = std::get_if<float>(&value)) {
            m_speed = *speed;
            if (m_speed != 1.0f) {
                appendDiagnostic(DiagnosticSeverity::Warning,
                                 "video backend accepted speed property but currently plays at normal rate");
            }
            return Result<void>::success();
        }
    } else if (name == WE_SCENE_PROPERTY_FPS || name == WE_SCENE_PROPERTY_ASSETS
               || name == WE_SCENE_PROPERTY_SOURCE) {
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
    lifecycle.outputStateChanged = m_sharedState->outputStateChanged.exchange(false);
    lifecycle.frameRequested = m_sharedState->frameRequested.exchange(false);
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
    m_appsink = nullptr;
    m_bus = nullptr;
}

Result<void> VideoBackend::buildPipeline(PipelineMode mode) {
    destroyPipeline();

    m_pipelineMode = mode;
    m_selectedDmabufDrmFormat.reset();
    m_lastLoggedDmabufSampleSignature.reset();
    m_eos = false;
    if (mode == PipelineMode::Dmabuf) {
        GstElementFactory* factory = gst_element_factory_find("vah264dec");
        if (factory == nullptr) {
            std::fprintf(stderr, "video-backend: vah264dec not found\n");
            return unsupportedVaH264Dmabuf("missing vah264dec decoder");
        }
        gst_object_unref(factory);
    }

    const std::string escapedPath = EscapeGstPropertyString(m_sourcePath.string());
    std::string pipelineDescription;
    if (mode == PipelineMode::Dmabuf) {
        const auto decoder_caps = QueryVaDmabufDecoderCaps();
        if (! decoder_caps.has_value()) {
            std::fprintf(stderr, "video-backend: failed to query vah264dec caps\n");
            return unsupportedVaH264Dmabuf("failed to query vah264dec DMA-BUF caps");
        }
        if (decoder_caps->drm_formats.empty()) {
            std::fprintf(stderr, "video-backend: vah264dec exposes no DMA-BUF drm-format\n");
            return unsupportedVaH264Dmabuf("decoder exposed no DMA-BUF drm-format");
        }

        m_selectedDmabufDrmFormat = decoder_caps->drm_formats.front();
        appendDiagnostic(DiagnosticSeverity::Warning,
                         "video backend DMA-BUF path currently requires MP4/H.264/vah264dec; "
                         "importer drm-format intersection is not implemented yet (TODO), so "
                         "selection currently uses decoder-advertised drm-format only");
        std::fprintf(stderr,
                     "video-backend[debug]: pipeline-mode=dmabuf decoder-caps=%s selected-drm-format=%s\n",
                     decoder_caps->caps_string.empty() ? "<unavailable>"
                                                       : decoder_caps->caps_string.c_str(),
                     m_selectedDmabufDrmFormat->c_str());
        pipelineDescription =
            "filesrc location=\"" + escapedPath + "\" "
            "! qtdemux name=demux "
            "demux.video_0 ! queue "
            "! h264parse "
            "! vah264dec "
            "! capsfilter caps=\"video/x-raw(memory:DMABuf),format=(string)DMA_DRM,drm-format=(string)"
            + EscapeGstPropertyString(*m_selectedDmabufDrmFormat)
            + "\" "
            "! appsink name=sink sync=true max-buffers=1 drop=true wait-on-eos=false";
    } else {
        std::fprintf(stderr, "video-backend[debug]: pipeline-mode=shm\n");
        pipelineDescription =
            "filesrc location=\"" + escapedPath + "\" "
            "! qtdemux name=demux "
            "demux.video_0 ! queue "
            "! decodebin "
            "! videoconvert "
            "! video/x-raw,format=(string)BGRA "
            "! appsink name=sink sync=true max-buffers=1 drop=true wait-on-eos=false";
    }

    GError* error = nullptr;
    m_pipeline = gst_parse_launch(pipelineDescription.c_str(), &error);
    if (m_pipeline != nullptr) gst_object_ref_sink(m_pipeline);
    if (m_pipeline == nullptr || error != nullptr) {
        const std::string message = error != nullptr ? error->message : "unknown parse error";
        if (error != nullptr) g_error_free(error);
        destroyPipeline();
        if (mode == PipelineMode::Dmabuf) {
            return unsupportedVaH264Dmabuf("could not create pipeline: " + message);
        }
        return Result<void>::failure(ResultCode::InternalError,
                                     "video backend could not create pipeline: " + message);
    }

    m_appsink = gst_bin_get_by_name(GST_BIN(m_pipeline), "sink");
    if (m_appsink == nullptr) {
        destroyPipeline();
        return Result<void>::failure(ResultCode::InternalError,
                                     "video backend pipeline is missing appsink");
    }
    g_object_set(m_appsink, "emit-signals", TRUE, nullptr);
    GstAppSinkCallbacks callbacks {};
    callbacks.propose_allocation = OnAppSinkProposeAllocation;
    gst_app_sink_set_callbacks(GST_APP_SINK(m_appsink), &callbacks, nullptr, nullptr);
    g_signal_connect(m_appsink,
                     "propose-allocation",
                     G_CALLBACK(OnAppSinkProposeAllocationSignal),
                     nullptr);

    m_bus = gst_element_get_bus(m_pipeline);
    if (m_bus == nullptr) {
        appendDiagnostic(DiagnosticSeverity::Error,
                         "video backend could not create GStreamer bus");
        destroyPipeline();
        return Result<void>::failure(ResultCode::InternalError,
                                     "video backend could not create GStreamer bus");
    }

    if (gst_element_set_state(m_pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
        destroyPipeline();
        if (mode == PipelineMode::Dmabuf) {
            return unsupportedVaH264Dmabuf("failed to preroll MP4/H.264/vah264dec pipeline");
        }
        return Result<void>::failure(ResultCode::InternalError,
                                     std::string("video backend failed to preroll ")
                                         + (mode == PipelineMode::Dmabuf ? "DMA-BUF" : "SHM")
                                         + " pipeline");
    }

    GstStateChangeReturn stateResult =
        gst_element_get_state(m_pipeline, nullptr, nullptr, GST_SECOND);
    if (stateResult == GST_STATE_CHANGE_FAILURE) {
        destroyPipeline();
        if (mode == PipelineMode::Dmabuf) {
            return unsupportedVaH264Dmabuf("preroll did not complete for MP4/H.264/vah264dec pipeline");
        }
        return Result<void>::failure(ResultCode::InternalError,
                                     "video backend preroll did not complete");
    }

    GstSample* preroll = gst_app_sink_try_pull_preroll(GST_APP_SINK(m_appsink), GST_SECOND);
    if (preroll != nullptr) {
        auto publishResult = publishSample(preroll);
        gst_sample_unref(preroll);
        if (! publishResult) {
            destroyPipeline();
            return publishResult;
        }
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
            gchar* debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            const std::string messageText = error != nullptr ? error->message : "unknown GStreamer error";
            appendDiagnostic(DiagnosticSeverity::Error, "video backend pipeline error: " + messageText);
            if (error != nullptr) g_error_free(error);
            if (debug != nullptr) g_free(debug);
            m_sharedState->readyState.store(BackendReadyState::Error);
            break;
        }
        case GST_MESSAGE_EOS:
            m_eos = true;
            if (m_pipeline != nullptr) {
                (void)gst_element_seek_simple(m_pipeline,
                                              GST_FORMAT_TIME,
                                              static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
                                              0);
                if (! m_paused) (void)gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
            }
            break;
        default:
            break;
        }
        gst_message_unref(message);
    }
}
} // namespace wallpaper
