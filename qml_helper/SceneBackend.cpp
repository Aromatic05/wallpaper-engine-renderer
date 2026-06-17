#include "SceneBackend.hpp"

#include <QtGlobal>
#include <QtCore/QObject>
#include <QtCore/QDir>
#include <QtCore/QThread>

#include <QtGui/QGuiApplication>
#include <QtGui/QOpenGLContext>
#include <QtQuick/QQuickWindow>

#include <QtGui/QOffscreenSurface>
#include <QtQuick/QSGSimpleTextureNode>
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
#include <QSGTexture>
#endif

#include <clocale>
#include <atomic>
#include <array>
#include <functional>

#include "glExtra.hpp"
#include "api/scene/WEScene.hpp"
#include "Type.hpp"
#include "Utils/Platform.hpp"
#include <cstdio>
#include <qobjectdefs.h>
#include <unistd.h>

using namespace scenebackend;

Q_LOGGING_CATEGORY(wekdeScene, "wekde.scene")

#define _Q_INFO(fmt, ...) qCInfo(wekdeScene, fmt, __VA_ARGS__)

namespace
{
void* get_proc_address(const char* name) {
    QOpenGLContext* glctx = QOpenGLContext::currentContext();
    if (! glctx) return nullptr;

    return reinterpret_cast<void*>(glctx->getProcAddress(QByteArray(name)));
}

QSGTexture* createTextureFromGl(uint32_t handle, QSize size, QQuickWindow* window) {
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
    return QNativeInterface::QSGOpenGLTexture::fromNative(handle, window, size);
#elif (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
    return window->createTextureFromNativeObject(
        QQuickWindow::NativeObjectTexture, &handle, 0, size);
#else
    return window->createTextureFromId(handle, size);
#endif
}

wallpaper::FillMode ToWPFillMode(int fillMode) {
    switch ((SceneObject::FillMode)fillMode) {
    case SceneObject::FillMode::STRETCH: return wallpaper::FillMode::STRETCH;
    case SceneObject::FillMode::ASPECTFIT: return wallpaper::FillMode::ASPECTFIT;
    case SceneObject::FillMode::ASPECTCROP:
    default: return wallpaper::FillMode::ASPECTCROP;
    }
}

} // namespace

namespace scenebackend
{

class TextureNode : public QObject, public QSGSimpleTextureNode {
    Q_OBJECT
public:
    typedef std::function<QSGTexture*(QQuickWindow*)> EatFrameOp;
    TextureNode(QQuickWindow*                                     window,
                SceneObject*                                      owner,
                std::shared_ptr<wallpaper::WESceneOutputBinding> outputBinding,
                bool                                              valid,
                EatFrameOp                                        eatFrameOp)
        : m_texture(nullptr),
          m_owner(owner),
          m_output_binding(std::move(outputBinding)),
          m_enable_valid(valid),
          m_eatFrameOp(eatFrameOp),
          m_window(window),
          m_first_frame(false) {
        // texture node must have a texture, so use the default 0 texture.
        m_texture      = createTextureFromGl(0, QSize(64, 64), window);
        m_init_texture = m_texture;
        setTexture(m_texture);
        setFiltering(QSGTexture::Linear);
        setOwnsTexture(false);
    }

    ~TextureNode() override {
        for (auto& item : texs_map) {
            auto& exh = item.second;
            // close(exh.fd);
            m_glex.deleteTexture(exh.gltex);
            delete exh.qsg;
        }
        delete m_init_texture;
        emit nodeDestroyed();
        _Q_INFO("Destroy texnode", "");
    }

    // only at qt render thread
    bool initGl() { return m_glex.init(get_proc_address); }

    // after gl, can run at any thread
    void initVulkan(uint16_t w, uint16_t h) {
        wallpaper::RenderInitInfo info;
        info.enable_valid_layer = m_enable_valid;
        info.offscreen          = true;
        info.offscreen_tiling   = m_glex.tiling();
        info.uuid               = m_glex.uuid();
        info.width              = w;
        info.height             = h;
        info.redraw_callback    = [this]() {
            Q_EMIT this->redraw();
        };

        auto cb = std::make_shared<wallpaper::FirstFrameCallback>([this]() {
            m_first_frame = true;
            Q_EMIT this->redraw();
        });
        m_owner->ensureSession();
        m_owner->ensureLoaded();
        wallpaper::SetWESceneFirstFrameCallback(*m_owner->session(), cb);
        m_output_binding = wallpaper::MakeWESceneOutputBinding(info);
        m_owner->setOutputBinding(m_output_binding);
        wallpaper::BindWESceneOutput(*m_owner->session(), m_output_binding);
    }

    void emitSceneFirstFrame() { Q_EMIT sceneFirstFrame(); }
signals:
    void textureInUse();
    void nodeDestroyed();
    void redraw();
    void sceneFirstFrame();

public slots:
    void newTexture() {
        if (! m_output_binding || m_output_binding->swapchain() == nullptr) return;

        wallpaper::ExHandle* exh = m_output_binding->swapchain()->eatFrame();
        if (exh != nullptr) {
            int id = exh->id();
            if (texs_map.count(id) == 0) {
                _Q_INFO("receive external texture(%dx%d) from fd: %d",
                        exh->width,
                        exh->height,
                        exh->fd);
                ExTex ex_tex;
                int   fd    = exh->fd;
                uint  gltex = m_glex.genExTexture(*exh);

                ex_tex.gltex = gltex;
                ex_tex.qsg   = createTextureFromGl(gltex, QSize(exh->width, exh->height), m_window);
                texs_map[id] = ex_tex;
                close(fd);
            }
            auto& newtex = texs_map.at(id);
            if (newtex.qsg != nullptr)
                m_texture = newtex.qsg;
            else
                m_texture = m_init_texture;

            setTexture(m_texture);
            markDirty(DirtyMaterial);
            Q_EMIT textureInUse();

            bool expected = true;
            if (m_first_frame.compare_exchange_strong(expected, false)) {
                Q_EMIT sceneFirstFrame();
            }
        }
    }

private:
    SceneObject*                                      m_owner;
    std::shared_ptr<wallpaper::WESceneOutputBinding> m_output_binding;
    bool                                              m_enable_valid;

    QSGTexture*       m_init_texture;
    QSGTexture*       m_texture;
    EatFrameOp        m_eatFrameOp;
    QQuickWindow*     m_window;
    std::atomic<bool> m_first_frame;

    GlExtra m_glex;

    struct ExTex {
        // int fd;
        uint        gltex;
        QSGTexture* qsg;
    };
    std::unordered_map<int, ExTex> texs_map;
};

} // namespace scenebackend

SceneObject::SceneObject(QQuickItem* parent)
    : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    ensureSession();
}

SceneObject::~SceneObject() { _Q_INFO("Destroy sceneobject", ""); }

void SceneObject::resizeFb() {
    QSize size;
    size.setWidth(this->width());
    size.setHeight(this->height());
}

QSGNode* SceneObject::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    TextureNode* node = static_cast<TextureNode*>(oldNode);
    if (! node) {
        ensureSession();
        node = new TextureNode(window(), this, m_outputBinding, m_enable_valid, [this](QQuickWindow* window) {
            return (QSGTexture*)nullptr;
        });
        if (node->initGl()) {
            node->initVulkan(width()*window()->devicePixelRatio(), height()*window()->devicePixelRatio());

            connect(
                node, &TextureNode::redraw, window(), &QQuickWindow::update, Qt::QueuedConnection);
            connect(window(),
                    &QQuickWindow::beforeRendering,
                    node,
                    &TextureNode::newTexture,
                    Qt::DirectConnection);
            connect(node, &TextureNode::sceneFirstFrame, this, &SceneObject::firstFrame);
        }
    }

    node->setRect(boundingRect());
    return node;
}

void SceneObject::setSceneSource(QUrl source) {
    ensureSession();
    m_source = std::move(source);
    m_loaded = false;
    ensureLoaded();
}

void SceneObject::setSceneAssets(QUrl assets) {
    auto assetsPath = QDir::toNativeSeparators(assets.toLocalFile()).toStdString();
    ensureSession();
    m_assets = std::move(assets);
    wallpaper::SetWESceneAssets(*m_session, std::move(assetsPath));
}
// qobject

QUrl SceneObject::source() const { return m_source; }
QUrl SceneObject::assets() const { return m_assets; }

int   SceneObject::fps() const { return m_fps; }
int   SceneObject::fillMode() const { return m_fillMode; }
float SceneObject::speed() const { return m_speed; }
float SceneObject::volume() const { return m_volume; }
bool  SceneObject::muted() const { return m_muted; }

void SceneObject::setSource(const QUrl& source) {
    if (source == m_source) return;
    m_source = source;
    setSceneSource(m_source);
    Q_EMIT sourceChanged();
}

void SceneObject::setAssets(const QUrl& assets) {
    if (m_assets == assets) return;
    m_assets = assets;
    setSceneAssets(m_assets);
}

void SceneObject::setFps(int value) {
    if (m_fps == value) return;
    m_fps = value;
    ensureSession();
    wallpaper::SetWESceneFps(*m_session, static_cast<std::int32_t>(value));
    Q_EMIT fpsChanged();
}
void SceneObject::setFillMode(int value) {
    if (m_fillMode == value) return;
    m_fillMode = value;
    ensureSession();
    wallpaper::SetWESceneFillMode(*m_session, (int32_t)ToWPFillMode(value));
    Q_EMIT fillModeChanged();
}
void SceneObject::setSpeed(float value) {
    if (m_speed == value) return;
    m_speed = value;
    ensureSession();
    wallpaper::SetWESceneSpeed(*m_session, value);
    Q_EMIT speedChanged();
}
void SceneObject::setVolume(float value) {
    if (m_volume == value) return;
    m_volume = value;
    ensureSession();
    wallpaper::SetWESceneVolume(*m_session, value);
    Q_EMIT volumeChanged();
}
void SceneObject::setMuted(bool value) {
    if (m_muted == value) return;
    m_muted = value;
    ensureSession();
    wallpaper::SetWESceneMuted(*m_session, value);
}

void SceneObject::play() {
    ensureLoaded();
    m_session->play();
}
void SceneObject::pause() {
    ensureLoaded();
    m_session->pause();
}

bool SceneObject::vulkanValid() const { return m_enable_valid; }
void SceneObject::enableVulkanValid() { m_enable_valid = true; }
void SceneObject::enableGenGraphviz() {
    m_genGraphviz = true;
    ensureSession();
    wallpaper::SetWESceneGraphviz(*m_session, true);
}

void SceneObject::setAcceptMouse(bool value) {
    if (value)
        setAcceptedMouseButtons(Qt::LeftButton);
    else
        setAcceptedMouseButtons(Qt::NoButton);
}

void SceneObject::setAcceptHover(bool value) { setAcceptHoverEvents(value); }

void SceneObject::mousePressEvent(QMouseEvent* event) {}
void SceneObject::mouseMoveEvent(QMouseEvent* event) {
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
    auto pos = event->position();
#else
    auto pos = event->localPos();
#endif
    ensureLoaded();
    wallpaper::InputEvent input;
    input.type     = wallpaper::InputEventType::PointerMove;
    input.pointerX = pos.x() / width();
    input.pointerY = pos.y() / height();
    m_session->sendInput(input);
}

void SceneObject::hoverMoveEvent(QHoverEvent* event) {
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
    auto pos = event->position();
#else
    auto pos = event->posF();
#endif
    ensureLoaded();
    wallpaper::InputEvent input;
    input.type     = wallpaper::InputEventType::PointerMove;
    input.pointerX = pos.x() / width();
    input.pointerY = pos.y() / height();
    m_session->sendInput(input);
}

std::string SceneObject::GetDefaultCachePath() {
    return wallpaper::platform::GetCachePath(CACHE_DIR);
}

wallpaper::WallpaperSession* SceneObject::session() const { return m_session.get(); }

std::shared_ptr<wallpaper::WESceneOutputBinding> SceneObject::outputBinding() const {
    return m_outputBinding;
}

void SceneObject::setOutputBinding(std::shared_ptr<wallpaper::WESceneOutputBinding> binding) {
    m_outputBinding = std::move(binding);
}

void SceneObject::ensureSession() {
    if (m_session) return;
    m_session = wallpaper::CreateWESceneSession(m_runtime, GetDefaultCachePath());
}

void SceneObject::ensureLoaded() {
    ensureSession();
    if (m_loaded || ! m_source.isValid() || m_source.isEmpty()) return;

    wallpaper::WESceneSourceConfig sourceConfig;
    sourceConfig.uri      = QDir::toNativeSeparators(m_source.toLocalFile()).toStdString();
    sourceConfig.assets   = m_assets.isValid() && ! m_assets.isEmpty()
                                ? QDir::toNativeSeparators(m_assets.toLocalFile()).toStdString()
                                : std::string {};
    sourceConfig.fps      = static_cast<std::int32_t>(m_fps);
    sourceConfig.fillMode = static_cast<std::int32_t>(ToWPFillMode(m_fillMode));
    sourceConfig.speed    = m_speed;
    sourceConfig.volume   = m_volume;
    sourceConfig.muted    = m_muted;
    sourceConfig.graphviz = m_genGraphviz;
    auto result = wallpaper::LoadWEScene(*m_session, sourceConfig);
    if (! result) return;

    m_loaded = true;
    if (m_outputBinding) {
        wallpaper::BindWESceneOutput(*m_session, m_outputBinding);
    }
}

#include "SceneBackend.moc"
