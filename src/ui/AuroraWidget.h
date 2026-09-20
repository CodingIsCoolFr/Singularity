#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLWidget>
#include <QPointer>
#include <QResizeEvent>
#include <QTimer>
#include <QVector3D>

class QOpenGLShaderProgram;

// Full-window black hole. The rest of the UI is painted on top of this as
// translucent glass, so the accretion disk shows in the gaps.
class AuroraWidget : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core
{
    Q_OBJECT

public:
    explicit AuroraWidget(QWidget *parent = nullptr);
    ~AuroraWidget() override;

    void setOverlay(QWidget *overlay);
    void setRunning(bool on);
    void setHoleColors(const QVector3D &accent, const QVector3D &disk, const QVector3D &grade);

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
    void destroyGl();
    QWidget *pickOverlay(const QPoint &pos) const;

    QOpenGLShaderProgram *m_program = nullptr;
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
