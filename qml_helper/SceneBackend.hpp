#pragma once

#include <QtQuick/QQuickFramebufferObject>
#include <QtCore/QLoggingCategory>
#include <QtCore/QTimer>
#include <QtGui/QMouseEvent>
#include <QtGui/QHoverEvent>

#include "api/WallpaperRuntime.hpp"
#include "backend/scene/compatibility/WESceneOutputTarget.hpp"

Q_DECLARE_LOGGING_CATEGORY(wekdeScene)

namespace scenebackend
{

class SceneObject : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(QUrl assets READ assets WRITE setAssets)
    Q_PROPERTY(int fps READ fps WRITE setFps NOTIFY fpsChanged)
    Q_PROPERTY(int fillMode READ fillMode WRITE setFillMode NOTIFY fillModeChanged)
    Q_PROPERTY(float speed READ speed WRITE setSpeed NOTIFY speedChanged)
    Q_PROPERTY(float volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMuted)
public:
    constexpr static std::string_view CACHE_DIR { "wescene-renderer" };
    static std::string                GetDefaultCachePath();

    enum FillMode
    {
        STRETCH,
        ASPECTFIT,
        ASPECTCROP
    };
    Q_ENUM(FillMode)

    QUrl source() const;
    QUrl assets() const;
    void setSource(const QUrl& source);
    void setAssets(const QUrl& assets);

    int   fps() const;
    int   fillMode() const;
    float speed() const;
    float volume() const;
    bool  muted() const;

    void setFps(int);
    void setFillMode(int);
    void setSpeed(float);
    void setVolume(float);
    void setMuted(bool);

    // debug
    bool vulkanValid() const;
    void enableVulkanValid();
    void enableGenGraphviz();

    Q_INVOKABLE void setAcceptMouse(bool);
    Q_INVOKABLE void setAcceptHover(bool);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;

public slots:
    void resizeFb();
    void play();
    void pause();

signals:
    void sourceChanged();
    void fpsChanged();
    void fillModeChanged();
    void speedChanged();
    void volumeChanged();
    void firstFrame();

private:
    QUrl m_source;
    QUrl m_assets;

    int   m_fps { 15 };
    int   m_fillMode { FillMode::ASPECTCROP };
    float m_speed { 1.0f };
    float m_volume { 1.0f };
    bool  m_muted { false };
    bool  m_genGraphviz { false };

public:
    static void on_update(void* ctx);

    explicit SceneObject(QQuickItem* parent = nullptr);
    virtual ~SceneObject();

    wallpaper::WallpaperSession*                            session() const;
    std::shared_ptr<wallpaper::WESceneOutputBinding>        outputBinding() const;
    void                                                   setOutputBinding(std::shared_ptr<wallpaper::WESceneOutputBinding> binding);
    void                                                   ensureSession();
    void                                                   ensureLoaded();

private:
    void setScenePropertyQurl(std::string_view, QUrl);
    bool m_enable_valid { false };
    bool m_loaded { false };

    wallpaper::WallpaperRuntime                   m_runtime;
    std::shared_ptr<wallpaper::BackendFactory>   m_backendFactory;
    std::unique_ptr<wallpaper::WallpaperSession> m_session;
    std::shared_ptr<wallpaper::WESceneOutputBinding> m_outputBinding;

protected:
    QSGNode* updatePaintNode(QSGNode*, UpdatePaintNodeData*);
};

} // namespace scenebackend
