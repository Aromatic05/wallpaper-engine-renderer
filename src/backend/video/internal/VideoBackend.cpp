#include "backend/video/internal/VideoBackend.hpp"

#include "backend/video/internal/VideoFrameSwapchain.hpp"
#include "backend/video/internal/VideoOutputBinding.hpp"
#include "backend/video/internal/VideoRenderPlan.hpp"
#include "wallpaper/scene/WESceneContract.hpp"

#include <algorithm>
#include <cstring>
#include <optional>
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
constexpr const char* kFallbackVaDmabufDrmFormat = "NV12:0x0200000000401b03";

Result<void> unsupportedInput() {
    return Result<void>::failure(ResultCode::NotSupported,
                                 "video backend does not support input events");
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

std::optional<std::string> QueryVaDmabufDrmFormatString() {
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

    std::optional<std::string> drm_format;
    for (guint i = 0; i < gst_caps_get_size(caps) && !drm_format.has_value(); ++i) {
        const GstCapsFeatures* features = gst_caps_get_features(caps, i);
        if (features == nullptr || !gst_caps_features_contains(features, GST_CAPS_FEATURE_MEMORY_DMABUF)) {
            continue;
        }

        const GstStructure* structure = gst_caps_get_structure(caps, i);
        if (structure == nullptr) continue;
        const GValue* drm_value = gst_structure_get_value(structure, "drm-format");
        if (drm_value == nullptr) continue;

        if (G_VALUE_HOLDS_STRING(drm_value)) {
            const char* text = g_value_get_string(drm_value);
            if (text != nullptr && *text != '\0') drm_format = text;
        } else if (GST_VALUE_HOLDS_LIST(drm_value)) {
            for (guint j = 0; j < gst_value_list_get_size(drm_value); ++j) {
                const GValue* item = gst_value_list_get_value(drm_value, j);
                if (item != nullptr && G_VALUE_HOLDS_STRING(item)) {
                    const char* text = g_value_get_string(item);
                    if (text != nullptr && *text != '\0') {
                        drm_format = text;
                        break;
                    }
                }
            }
        }
    }

    gst_caps_unref(caps);
    if (drm_format.has_value()) return drm_format;
    return std::string(kFallbackVaDmabufDrmFormat);
}

bool ParseDmabufSample(GstSample* sample, VideoDmabufFrame& frame) {
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
    frame.modifier = drm_info.drm_modifier == DRM_FORMAT_MOD_INVALID
        ? static_cast<std::uint64_t>(DRM_FORMAT_MOD_LINEAR)
        : drm_info.drm_modifier;

    for (guint i = 0; i < plane_count; ++i) {
        const guint memory_index = memory_count == 1 ? 0u : i;
        if (memory_index >= memory_count) {
            std::fprintf(stderr,
                         "video-backend: plane %u maps outside memory_count=%u\n",
                         i,
                         memory_count);
            return false;
        }
        GstMemory* memory = gst_buffer_peek_memory(buffer, memory_index);
        if (memory == nullptr || ! gst_is_dmabuf_memory(memory)) {
            std::fprintf(stderr,
                         "video-backend: plane %u memory is not dmabuf (memory_count=%u)\n",
                         i,
                         memory_count);
            return false;
        }

        gsize memory_offset = 0;
        gsize memory_size = 0;
        (void)gst_memory_get_sizes(memory, &memory_offset, &memory_size);
        (void)memory_size;

        frame.planes[i].fd = gst_dmabuf_memory_get_fd(memory);
        frame.planes[i].stride = static_cast<std::uint32_t>(
            meta != nullptr ? meta->stride[i] : GST_VIDEO_INFO_PLANE_STRIDE(&drm_info.vinfo, i));
        const std::uint32_t plane_offset = static_cast<std::uint32_t>(
            meta != nullptr
                ? (memory_count == 1 ? meta->offset[i] : 0)
                : (memory_count == 1 ? GST_VIDEO_INFO_PLANE_OFFSET(&drm_info.vinfo, i) : 0));
        frame.planes[i].offset = static_cast<std::uint32_t>(memory_offset) + plane_offset;
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
    m_sourceUri.clear();
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

    gchar* gst_uri = gst_filename_to_uri(m_sourcePath.c_str(), nullptr);
    if (gst_uri == nullptr) {
        m_sharedState->readyState.store(BackendReadyState::Error);
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "video backend could not convert source path to URI: " + source.uri);
    }
    m_sourceUri = gst_uri;
    g_free(gst_uri);

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
            if (m_pipeline != nullptr) g_object_set(m_pipeline, "volume", static_cast<gdouble>(m_volume), nullptr);
            return Result<void>::success();
        }
        if (const auto* volume = std::get_if<double>(&value)) {
            m_volume = static_cast<float>(*volume);
            if (m_pipeline != nullptr) g_object_set(m_pipeline, "volume", static_cast<gdouble>(m_volume), nullptr);
            return Result<void>::success();
        }
    } else if (name == WE_SCENE_PROPERTY_MUTED) {
        if (const auto* muted = std::get_if<bool>(&value)) {
            m_muted = *muted;
            if (m_pipeline != nullptr) g_object_set(m_pipeline, "mute", static_cast<gboolean>(m_muted), nullptr);
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
    m_eos = false;
    if (mode == PipelineMode::Dmabuf && gst_element_factory_find("vah264dec") == nullptr) {
        std::fprintf(stderr, "video-backend: vah264dec not found\n");
        return Result<void>::failure(ResultCode::NotSupported,
                                     "video backend DMA-BUF mode requires vah264dec");
    }

    const std::string escapedPath = EscapeGstPropertyString(m_sourcePath.string());
    std::string pipelineDescription;
    if (mode == PipelineMode::Dmabuf) {
        const auto drm_format = QueryVaDmabufDrmFormatString();
        if (!drm_format.has_value()) {
            std::fprintf(stderr, "video-backend: failed to query drm-format\n");
            return Result<void>::failure(ResultCode::NotSupported,
                                         "video backend DMA-BUF mode could not query decoder drm-format");
        }
        std::fprintf(stderr, "video-backend: selected drm-format=%s\n", drm_format->c_str());
        pipelineDescription =
            "filesrc location=\"" + escapedPath + "\" "
            "! qtdemux name=demux "
            "demux.video_0 ! queue "
            "! h264parse "
            "! vah264dec "
            "! capsfilter caps=\"video/x-raw(memory:DMABuf),format=(string)DMA_DRM,drm-format=(string)"
            + EscapeGstPropertyString(*drm_format)
            + "\" "
            "! appsink name=sink sync=true max-buffers=1 drop=true wait-on-eos=false";
    } else {
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
        return Result<void>::failure(ResultCode::InternalError,
                                     std::string("video backend failed to preroll ")
                                         + (mode == PipelineMode::Dmabuf ? "DMA-BUF" : "SHM")
                                         + " pipeline");
    }

    GstStateChangeReturn stateResult =
        gst_element_get_state(m_pipeline, nullptr, nullptr, GST_SECOND);
    if (stateResult == GST_STATE_CHANGE_FAILURE) {
        destroyPipeline();
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
        if (! ParseDmabufSample(sample, frame)) {
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
