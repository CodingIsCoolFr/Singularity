#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLWidget>
#include <QPointer>
#include <QResizeEvent>
#include <QTimer>
#include <QVector3D>

class QMovie;
class QOpenGLShaderProgram;
class QOpenGLTexture;

// Whatever is behind the whole window. The rest of the UI is painted on top of
// this as translucent glass, so the background shows through the gaps.
//
// Two kinds. The drawn black hole, which is a shader, and a picture of your
// own, which may be animated. They share this one widget rather than being two
// widgets that take turns, because everything else in the window is composited
// onto whatever this draws - the overlay machinery below only knows how to do
// that once.
class AuroraWidget : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core
{
    Q_OBJECT

public:
    enum class Background {
        Hole,      // the drawn one
        Picture,   // one of yours
    };

    explicit AuroraWidget(QWidget *parent = nullptr);
    ~AuroraWidget() override;

    void setOverlay(QWidget *overlay);
    void setRunning(bool on);
    void setHoleColors(const QVector3D &accent, const QVector3D &disk, const QVector3D &grade);

    // A picture instead of the hole.
    //
    // `path` may be an animated GIF or WebP, or a still image. `dimPercent`
    // darkens it, which is not decoration: a bright picture behind the
    // conversation makes the text unreadable, and somebody who has just chosen
    // a photograph they like will blame the client rather than the photograph.
    void setBackgroundPicture(const QString &path, int dimPercent);
    void setBackgroundMode(Background mode);
    Background backgroundMode() const { return m_background; }

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void resizeEvent(QResizeEvent *event) override;
    bool event(QEvent *event) override;

private:
    void refreshOverlay();
    void paintOverlay();
    void updateOverlayFbo();
    bool compileProgram();
    bool compilePictureProgram();
    void destroyGl();
    QWidget *pickOverlay(const QPoint &pos) const;

    void loadPicture();
    void uploadFrame(const QImage &frame);

    QOpenGLShaderProgram *m_program = nullptr;
    QOpenGLShaderProgram *m_pictureProgram = nullptr;

    Background m_background = Background::Hole;
    QString m_picturePath;
    int m_pictureDim = 45;

    // The animation, when there is one. A still picture leaves this null and
    // the texture simply never changes.
    QMovie *m_movie = nullptr;
    GLuint m_pictureTex = 0;
    QSize m_pictureSize;
    bool m_pictureDirty = false;
    QImage m_pendingFrame;

    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    int m_uResolution = -1;
    int m_uTime = -1;
    int m_uAccent = -1;
    int m_uDisk = -1;
    int m_uGrade = -1;

    QWidget *m_overlay = nullptr;
    QPointer<QWidget> m_pressTarget;
    QTimer m_overlayTimer;
    bool m_inOverlayPaint = false;
    GLuint m_overlayTex = 0;
    GLuint m_overlayFbo = 0;
    QSize m_overlaySize;
    QImage m_overlayImage;
    bool m_running = true;
    QElapsedTimer m_clock;
    float m_time = 0.f;
    qint64 m_lastMs = 0;

    QVector3D m_accent{0.22f, 0.92f, 0.88f};
    QVector3D m_disk{0.18f, 0.72f, 0.70f};
    QVector3D m_grade{0.86f, 1.06f, 1.04f};
    QVector3D m_accentTarget{0.22f, 0.92f, 0.88f};
    QVector3D m_diskTarget{0.18f, 0.72f, 0.70f};
    QVector3D m_gradeTarget{0.86f, 1.06f, 1.04f};
    bool m_paletteInited = false;
};
