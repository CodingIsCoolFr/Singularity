#include "AuroraWidget.h"

#include "core/Logger.h"

#include <QDebug>
#include <QFileInfo>
#include <QMovie>
#include <QOpenGLShaderProgram>
#include <QPainter>
#include <QResizeEvent>
#include <QVector2D>

namespace {

constexpr const char *kVertex = R"(#version 330 core
void main()
{
    const vec2 verts[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(verts[gl_VertexID], 0.0, 1.0);
}
)";

// Copied from Singularity's BlackHoleWidget: geodesic disk, photon ring,
// Doppler boosting, starfield. Theme seed retints uAccent/uDisk/uGrade.
constexpr const char *kFragment = R"(#version 330 core
uniform vec2  uResolution;
uniform float uTime;
uniform vec2  uPointer;
uniform vec3  uAccent;
uniform vec3  uDisk;
uniform vec3  uGrade;
out vec4 fragColor;

float hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec3 starfield(vec3 rd)
{
    vec3 col = vec3(0.0015, 0.0022, 0.0060);

    float band = exp(-11.0 * rd.y * rd.y);
    col += mix(vec3(0.028, 0.040, 0.070), uAccent * 0.08, 0.55) * band;
    col += uAccent * 0.04 * band * hash12(rd.xz * 8.0);

    float az = atan(rd.x, rd.z);
    float el = asin(clamp(rd.y, -1.0, 1.0));

    for (int k = 0; k < 5; ++k) {
        float sc = exp2(float(k) * 1.18 + 4.05);
        vec2 uv = vec2(az, el) * sc;
        vec2 id = floor(uv);
        vec2 f  = fract(uv) - 0.5;
        float h = hash12(id + float(k) * 17.13);
        if (h > 0.905) {
            float d = length(f);
            float br = smoothstep(0.080 + h * 0.035, 0.0, d);
            float tw = 0.78 + 0.22 * sin(uTime * (0.7 + h * 3.2) + h * 40.0);
            vec3 tc = mix(mix(vec3(0.75, 0.82, 0.95), uAccent, 0.35), vec3(0.95, 0.97, 1.0), fract(h * 9.1));
            if (fract(h * 13.7) > 0.82)
                tc = mix(vec3(0.55, 0.70, 1.0), uAccent, 0.45);
            col += br * tc * tw * (0.28 + 2.4 * pow(h, 9.0));

            if (h > 0.988) {
                col += mix(vec3(0.75, 0.88, 1.0), uAccent, 0.4) * 0.35 * exp(-abs(f.x) * 55.0) * exp(-abs(f.y) * 10.0);
                col += mix(vec3(0.75, 0.88, 1.0), uAccent, 0.4) * 0.18 * exp(-abs(f.y) * 55.0) * exp(-abs(f.x) * 10.0);
            }
        }
    }
    return col;
}

void main()
{
    vec2 uv = (gl_FragCoord.xy - 0.5 * uResolution) / uResolution.y;

    float t = uTime * 0.055;
    float cs = cos(t);
    float sn = sin(t);

    vec3 ro = vec3(0.0, 0.62 + uPointer.y * 0.08, 7.15);
    ro.xz = mat2(cs, -sn, sn, cs) * ro.xz;

    vec3 ta = vec3(uPointer.x * 0.08, 0.02, 0.0);
    vec3 ww = normalize(ta - ro);
    vec3 uu = normalize(cross(ww, vec3(0.0, 1.0, 0.0)));
    vec3 vv = cross(uu, ww);
    vec3 rd = normalize(uv.x * uu + uv.y * vv + 1.22 * ww);

    const float RS = 0.50;
    const int STEPS = 100;

    vec3 pos = ro;
    vec3 vel = rd;
    vec3 col = vec3(0.0);
    float trans = 1.0;
    float closest = 1e5;

    for (int i = 0; i < STEPS; ++i) {
        float r = length(pos);
        closest = min(closest, r);

        if (r < RS) {
            trans = 0.0;
            break;
        }

        float dt = 0.036 * max(r, 0.22);
        vel += -1.50 * RS * pos / (r * r * r * r) * dt;
        vec3 nxt = pos + vel * dt;

        float photon = smoothstep(0.09, 0.0, abs(r - 1.52 * RS));
        col += trans * photon * uAccent * 0.075;

        if (pos.y * nxt.y < 0.0) {
            float f = pos.y / (pos.y - nxt.y + 1e-6);
            vec3 hit = mix(pos, nxt, f);
            float rho = length(hit.xz);
            float inner = RS * 2.45;
            float outer = RS * 11.2;

            if (rho > inner && rho < outer) {
                float x = (rho - inner) / (outer - inner);
                float ang = atan(hit.z, hit.x);
                float spir = 0.5 + 0.5 * sin(2.0 * ang - log(rho + 0.04) * 8.5 - uTime * 1.15);
                float dens = pow(1.0 - x, 1.05) * (0.62 + 0.38 * spir);
                dens *= smoothstep(0.0, 0.08, x) * smoothstep(1.0, 0.72, x);

                vec3 outerC = uDisk * 0.42;
                vec3 midC   = uDisk;
                vec3 hotC   = mix(vec3(1.15, 1.25, 1.40), uAccent, 0.55) * 1.25;
                vec3 dcol = mix(hotC, mix(midC, outerC, smoothstep(0.18, 1.0, x)), smoothstep(0.0, 0.48, x));

                vec3 tang = normalize(vec3(-hit.z, 0.0, hit.x));
                float vkep = 0.50 / sqrt(max(rho, 0.2));
                float dop  = 1.0 + dot(normalize(vel), tang) * vkep * 1.35;
                dcol *= pow(clamp(dop, 0.30, 2.1), 2.4);
                dcol += uAccent * smoothstep(1.12, 1.55, dop) * 0.40;
                dcol *= mix(vec3(1.0), vec3(0.55, 0.40, 0.48), smoothstep(1.0, 0.55, dop));

                col += trans * dcol * dens * 0.72;
                trans *= 1.0 - clamp(dens * 0.55, 0.0, 0.78);
                if (trans < 0.03)
                    break;
            }
        }

        pos = nxt;
        if (r > 22.0)
            break;
    }

    col += trans * starfield(normalize(vel));

    float ring = smoothstep(0.16, 0.0, abs(closest - 1.5 * RS));
    col += ring * uAccent * 0.28;

    col *= uGrade;
    col = col / (1.0 + col * 0.70);
    col = pow(clamp(col, 0.0, 1.0), vec3(0.92));

    vec2 q = gl_FragCoord.xy / uResolution;
    float vig = pow(16.0 * q.x * q.y * (1.0 - q.x) * (1.0 - q.y), 0.38);
    col *= mix(0.22, 1.0, vig);

    float g = hash12(gl_FragCoord.xy + vec2(fract(uTime) * 73.1, fract(uTime * 0.37) * 19.0));
    col += (g - 0.5) * 0.012;

    fragColor = vec4(col, 1.0);
}
)";

// Your own picture, fitted and dimmed.
//
// Fitted by covering rather than stretching. A wallpaper squashed to the
// window's shape looks wrong in a way people notice immediately even when they
// cannot say why, so the picture keeps its proportions and the overflow is
// cropped - which is what every operating system does with a wallpaper.
//
// The dimming and the vignette are not decoration. Every surface above this is
// translucent glass, and a bright or busy picture behind the conversation
// makes the text unreadable. Somebody who has just chosen a photograph they
// like will blame the client rather than the photograph, so the default is
// dark enough to read over and the slider lets them go brighter deliberately.
constexpr const char *kPictureFragment = R"(#version 330 core
uniform sampler2D uTex;
uniform vec2  uResolution;
uniform vec2  uTexSize;
uniform float uDim;
out vec4 fragColor;

void main()
{
    vec2 uv = gl_FragCoord.xy / uResolution;

    // Images arrive with their first row at the top; OpenGL counts from the
    // bottom. Without this the wallpaper is upside down.
    uv.y = 1.0 - uv.y;

    float screenAspect = uResolution.x / uResolution.y;
    float imageAspect = uTexSize.x / max(uTexSize.y, 1.0);

    if (imageAspect > screenAspect) {
        // Wider than the window, so crop the sides.
        float s = screenAspect / imageAspect;
        uv.x = (uv.x - 0.5) * s + 0.5;
    } else {
        // Taller, so crop top and bottom.
        float s = imageAspect / screenAspect;
        uv.y = (uv.y - 0.5) * s + 0.5;
    }

    vec3 col = texture(uTex, uv).rgb;
    col *= (1.0 - uDim);

    vec2 q = gl_FragCoord.xy / uResolution;
    float vig = pow(16.0 * q.x * q.y * (1.0 - q.x) * (1.0 - q.y), 0.30);
    col *= mix(0.45, 1.0, vig);

    fragColor = vec4(col, 1.0);
}
)";

} // namespace

AuroraWidget::AuroraWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_StyledBackground, false);
    setAutoFillBackground(false);
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
    setStyleSheet(QStringLiteral("background:none;border:none;"));

    m_clock.start();
    m_overlayTimer.setInterval(16);
    connect(&m_overlayTimer, &QTimer::timeout, this, QOverload<>::of(&QWidget::update));
    m_overlayTimer.start();
}

AuroraWidget::~AuroraWidget()
{
    makeCurrent();
    destroyGl();
    doneCurrent();
}

void AuroraWidget::setOverlay(QWidget *overlay)
{
    m_overlay = overlay;
    if (!overlay)
        return;
    if (overlay->parentWidget() != this)
        overlay->setParent(this);
    // A child of QOpenGLWidget composites over the FBO. WA_TranslucentBackground
    // is what used to spawn a native layered HWND and paint black over the disk.
    overlay->setAttribute(Qt::WA_TranslucentBackground, false);
    overlay->setAutoFillBackground(false);
    overlay->setGeometry(rect());
    overlay->raise();
    overlay->show();
}

QWidget *AuroraWidget::pickOverlay(const QPoint &pos) const
{
    if (!m_overlay)
        return nullptr;
    QWidget *hit = m_overlay->childAt(pos);
    return hit ? hit : m_overlay;
}

bool AuroraWidget::event(QEvent *event)
{
    return QOpenGLWidget::event(event);
}

void AuroraWidget::setRunning(bool on)
{
    if (on == m_running)
        return;
    if (on) {
        m_lastMs = m_clock.elapsed();
        m_overlayTimer.start();
    } else {
        m_overlayTimer.stop();
    }
    m_running = on;
    update();
}

void AuroraWidget::setBackgroundMode(Background mode)
{
    if (m_background == mode)
        return;
    m_background = mode;

    // The animation only runs while it is the thing being shown. A GIF
    // decoding in the background of a black hole is work nobody asked for.
    if (m_movie) {
        if (mode == Background::Picture)
            m_movie->start();
        else
            m_movie->stop();
    }

    update();
}

void AuroraWidget::setBackgroundPicture(const QString &path, int dimPercent)
{
    m_pictureDim = qBound(0, dimPercent, 95);

    if (path == m_picturePath) {
        update();
        return;
    }

    m_picturePath = path;

    delete m_movie;
    m_movie = nullptr;
    m_pictureSize = QSize();

    if (!m_picturePath.isEmpty())
        loadPicture();

    update();
}

void AuroraWidget::loadPicture()
{
    // QMovie reads animated formats one frame at a time rather than holding
    // every frame in memory at once, which matters: a long GIF decoded whole
    // is hundreds of megabytes. A still image is simply a movie with one
    // frame, so there is only one path here rather than two.
    m_movie = new QMovie(m_picturePath, QByteArray(), this);
    if (!m_movie->isValid()) {
        wlog(QStringLiteral("theme"),
             QStringLiteral("could not read the background picture: %1").arg(m_picturePath));
        delete m_movie;
        m_movie = nullptr;
        return;
    }

    // Decoded frames are thrown away after being handed over, so a long
    // animation costs the same as a short one.
    m_movie->setCacheMode(QMovie::CacheNone);

    connect(m_movie, &QMovie::frameChanged, this, [this](int) {
        if (!m_movie || m_background != Background::Picture)
            return;
        m_pendingFrame = m_movie->currentImage();
        m_pictureDirty = true;
        update();
    });

    m_movie->jumpToFrame(0);
    m_pendingFrame = m_movie->currentImage();
    m_pictureDirty = !m_pendingFrame.isNull();

    if (m_background == Background::Picture)
        m_movie->start();

    wlog(QStringLiteral("theme"),
         QStringLiteral("background picture %1 (%2 frames, %3x%4)")
             .arg(QFileInfo(m_picturePath).fileName())
             .arg(m_movie->frameCount())
             .arg(m_pendingFrame.width())
             .arg(m_pendingFrame.height()));
}

void AuroraWidget::uploadFrame(const QImage &frame)
{
    if (frame.isNull())
        return;

    const QImage rgba = frame.convertToFormat(QImage::Format_RGBA8888);

    if (m_pictureTex == 0) {
        glGenTextures(1, &m_pictureTex);
        glBindTexture(GL_TEXTURE_2D, m_pictureTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, m_pictureTex);
    }

    // Rows of a QImage are padded to four bytes. RGBA8888 is already a
    // multiple of four per pixel so this is always one, but saying so costs
    // nothing and stops a future format change tearing the picture diagonally.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, rgba.bytesPerLine() / 4);

    // Reallocating only when the size changes. An animation's frames are all
    // the same size, so the usual case is a straight overwrite.
    if (m_pictureSize != rgba.size()) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rgba.width(), rgba.height(), 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, rgba.constBits());
        m_pictureSize = rgba.size();
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, rgba.width(), rgba.height(), GL_RGBA,
                        GL_UNSIGNED_BYTE, rgba.constBits());
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

void AuroraWidget::setHoleColors(const QVector3D &accent, const QVector3D &disk, const QVector3D &grade)
{
    m_accentTarget = accent;
    m_diskTarget = disk;
    m_gradeTarget = grade;
    if (!m_running || !m_paletteInited) {
        m_accent = accent;
        m_disk = disk;
        m_grade = grade;
        m_paletteInited = true;
    }
    update();
}

void AuroraWidget::initializeGL()
{
    initializeOpenGLFunctions();
    if (!m_clock.isValid())
        m_clock.start();
    m_lastMs = m_clock.elapsed();

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glClearColor(0.01f, 0.01f, 0.02f, 1.0f);

    compileProgram();
    compilePictureProgram();

    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);
    glBindVertexArray(0);
}

void AuroraWidget::resizeGL(int w, int h)
{
    Q_UNUSED(w)
    Q_UNUSED(h)
    if (m_overlay)
        m_overlay->setGeometry(rect());
}

void AuroraWidget::resizeEvent(QResizeEvent *event)
{
    QOpenGLWidget::resizeEvent(event);
    if (m_overlay)
        m_overlay->setGeometry(rect());
}

void AuroraWidget::paintGL()
{
    const qreal dpr = devicePixelRatioF();
    const int w = int(width() * dpr);
    const int h = int(height() * dpr);
    glViewport(0, 0, w, h);
    glDisable(GL_BLEND);
    glClearColor(0.01f, 0.01f, 0.02f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    m_accent += (m_accentTarget - m_accent) * 0.12f;
    m_disk += (m_diskTarget - m_disk) * 0.12f;
    m_grade += (m_gradeTarget - m_grade) * 0.12f;

    if (m_running) {
        const qint64 now = m_clock.elapsed();
        m_time += float(now - m_lastMs) * 0.001f;
        m_lastMs = now;
    }

    // A picture of your own, when there is one to show.
    //
    // Falling back to the hole rather than to black: a background that failed
    // to load should look like the program's own design, not like a fault.
    if (m_background == Background::Picture && m_pictureProgram
        && m_pictureProgram->isLinked()) {

        if (m_pictureDirty) {
            uploadFrame(m_pendingFrame);
            m_pendingFrame = QImage();
            m_pictureDirty = false;
        }

        if (m_pictureTex != 0 && m_pictureSize.isValid()) {
            m_pictureProgram->bind();
            m_pictureProgram->setUniformValue("uResolution", QVector2D(float(w), float(h)));
            m_pictureProgram->setUniformValue(
                "uTexSize", QVector2D(float(m_pictureSize.width()), float(m_pictureSize.height())));
            m_pictureProgram->setUniformValue("uDim", float(m_pictureDim) / 100.0f);
            m_pictureProgram->setUniformValue("uTex", 0);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, m_pictureTex);

            glBindVertexArray(m_vao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindVertexArray(0);
            m_pictureProgram->release();
            return;
        }
    }

    if (m_program && m_program->isLinked()) {
        m_program->bind();
        m_program->setUniformValue(m_uResolution, QVector2D(float(w), float(h)));
        m_program->setUniformValue(m_uTime, m_time);
        m_program->setUniformValue("uPointer", QVector2D(0.f, 0.f));
        m_program->setUniformValue(m_uAccent, m_accent);
        m_program->setUniformValue(m_uDisk, m_disk);
        m_program->setUniformValue(m_uGrade, m_grade);

        glBindVertexArray(m_vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
        m_program->release();
    }
}

void AuroraWidget::refreshOverlay()
{
    if (!m_overlay || m_inOverlayPaint)
        return;
    const qreal dpr = devicePixelRatioF();
    const QSize px(int(width() * dpr), int(height() * dpr));
    if (px.isEmpty())
        return;

    m_inOverlayPaint = true;
    if (m_overlayImage.size() != px) {
        m_overlayImage = QImage(px, QImage::Format_ARGB32_Premultiplied);
        m_overlayImage.setDevicePixelRatio(dpr);
    }
    m_overlayImage.fill(Qt::transparent);
    {
        QPainter p(&m_overlayImage);
        p.setRenderHint(QPainter::Antialiasing, true);
        m_overlay->render(&p, QPoint(), QRegion(), QWidget::DrawChildren);
    }
    m_inOverlayPaint = false;
}

void AuroraWidget::paintOverlay()
{
    if (m_inOverlayPaint || m_overlayImage.isNull())
        return;

    QPainter painter(this);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.drawImage(QRect(0, 0, width(), height()), m_overlayImage);
}

void AuroraWidget::updateOverlayFbo()
{
    const qreal dpr = devicePixelRatioF();
    const QSize px(int(width() * dpr), int(height() * dpr));
    if (px.isEmpty())
        return;

    if (m_overlaySize != px) {
        if (m_overlayFbo) {
            glDeleteFramebuffers(1, &m_overlayFbo);
            m_overlayFbo = 0;
        }
        if (m_overlayTex) {
            glDeleteTextures(1, &m_overlayTex);
            m_overlayTex = 0;
        }
        m_overlaySize = px;
        m_overlayImage = QImage(px, QImage::Format_ARGB32_Premultiplied);
        m_overlayImage.setDevicePixelRatio(dpr);
    }

    m_overlayImage.fill(Qt::transparent);
    QPainter p(&m_overlayImage);
    p.setRenderHint(QPainter::Antialiasing, true);
    m_overlay->render(&p, QPoint(), QRegion(), QWidget::DrawChildren);
    p.end();
}

bool AuroraWidget::compileProgram()
{
    delete m_program;
    m_program = new QOpenGLShaderProgram(this);
    if (!m_program->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertex)) {
        qWarning() << "aurora vertex" << m_program->log();
        return false;
    }
    if (!m_program->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragment)) {
        qWarning() << "aurora fragment" << m_program->log();
        return false;
    }
    if (!m_program->link()) {
        qWarning() << "aurora link" << m_program->log();
        return false;
    }

    m_uResolution = m_program->uniformLocation("uResolution");
    m_uTime = m_program->uniformLocation("uTime");
    m_uAccent = m_program->uniformLocation("uAccent");
    m_uDisk = m_program->uniformLocation("uDisk");
    m_uGrade = m_program->uniformLocation("uGrade");
    return true;
}

bool AuroraWidget::compilePictureProgram()
{
    delete m_pictureProgram;
    m_pictureProgram = new QOpenGLShaderProgram(this);

    if (!m_pictureProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertex)
        || !m_pictureProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, kPictureFragment)
        || !m_pictureProgram->link()) {
        qWarning() << "aurora picture" << m_pictureProgram->log();
        delete m_pictureProgram;
        m_pictureProgram = nullptr;
        return false;
    }
    return true;
}

void AuroraWidget::destroyGl()
{
    delete m_pictureProgram;
    m_pictureProgram = nullptr;
    if (m_pictureTex) {
        glDeleteTextures(1, &m_pictureTex);
        m_pictureTex = 0;
    }
    m_pictureSize = QSize();

    delete m_program;
    m_program = nullptr;
    if (m_vbo)
        glDeleteBuffers(1, &m_vbo);
    if (m_vao)
        glDeleteVertexArrays(1, &m_vao);
    if (m_overlayFbo)
        glDeleteFramebuffers(1, &m_overlayFbo);
    if (m_overlayTex)
        glDeleteTextures(1, &m_overlayTex);
    m_vbo = 0;
    m_vao = 0;
    m_overlayFbo = 0;
    m_overlayTex = 0;
}
