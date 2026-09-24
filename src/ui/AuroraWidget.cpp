#include "AuroraWidget.h"

#include "core/Logger.h"

#include <QDebug>
#include <QFileInfo>
#include <QMovie>
#include <QOpenGLShaderProgram>
#include <QPainter>
#include <QResizeEvent>
#include <QScreen>
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
            float tw = 0.78 + 0.22 * sin(uTime * (0.22 + h * 1.0) + h * 40.0);
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

    float t = uTime * 0.02;
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
                float spir = 0.5 + 0.5 * sin(2.0 * ang - log(rho + 0.04) * 8.5 - uTime * 0.38);
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

    // A flat scrim, not a multiply. Multiplying crushed a bright GIF into a
    // spotlight in the middle of the window, which is why an uploaded picture
    // looked pasted on rather than like a wallpaper.
    float dim = clamp(uDim, 0.0, 0.85);
    col = mix(col, vec3(0.027, 0.035, 0.051), dim);

    // A slight darkening at the far edges, so the picture meets the frame
    // instead of ending in a hard cut. Not enough to hide the picture.
    vec2 q = gl_FragCoord.xy / uResolution - 0.5;
    float edge = smoothstep(0.78, 0.35, length(q * vec2(1.1, 1.0)));
    col *= mix(0.90, 1.0, edge);

    fragColor = vec4(col, 1.0);
}
)";

// The rays are fixed. They are marched once, when the window changes size,
// and every frame after that only turns the disk and twinkles the stars.
// Marching them again for every pixel is what made the background hitch, and
// drawing that result small is what made it look nothing like itself.
constexpr const char *kBake = R"(#version 330 core
uniform vec2 uResolution;
uniform float uZoom;
uniform float uOrbit;
layout(location = 0) out vec4 oHitA;
layout(location = 1) out vec4 oHitB;
layout(location = 2) out vec4 oSky;

const float RS = 0.50;
const float INNER = RS * 2.45;
const float OUTER = RS * 11.2;

void main()
{
    vec2 uv = (gl_FragCoord.xy - 0.5 * uResolution) / uResolution.y;
    float cs = cos(uOrbit);
    float sn = sin(uOrbit);
    vec3 ro = vec3(0.0, 0.62, 7.15);
    ro.xz = mat2(cs, -sn, sn, cs) * ro.xz;
    vec3 ta = vec3(0.0, 0.02, 0.0);
    vec3 ww = normalize(ta - ro);
    vec3 uu = normalize(cross(ww, vec3(0.0, 1.0, 0.0)));
    vec3 vv = cross(uu, ww);
    vec3 rd = normalize(uv.x * uu + uv.y * vv + uZoom * ww);

    vec3 pos = ro;
    vec3 vel = rd;
    vec4 hitA = vec4(-1.0, 1.0, 0.0, 0.0);
    vec4 hitB = vec4(-1.0, 1.0, 0.0, 0.0);
    int found = 0;
    float photon = 0.0;
    bool captured = false;

    for (int i = 0; i < 100; ++i) {
        float r = length(pos);
        if (r < RS) { captured = true; break; }
        float dt = 0.036 * max(r, 0.22);
        vel += -1.50 * RS * pos / (r * r * r * r) * dt;
        vec3 nxt = pos + vel * dt;
        photon += smoothstep(0.09, 0.0, abs(r - 1.52 * RS)) * 0.075;
        if (pos.y * nxt.y < 0.0) {
            float f = pos.y / (pos.y - nxt.y + 1e-6);
            vec3 hit = mix(pos, nxt, f);
            float rho = length(hit.xz);
            if (rho > INNER && rho < OUTER) {
                float x = (rho - INNER) / (OUTER - INNER);
                float ang = atan(hit.z, hit.x);
                float phase = 2.0 * ang - log(rho + 0.04) * 8.5;
                vec3 tang = normalize(vec3(-hit.z, 0.0, hit.x));
                float vkep = 0.50 / sqrt(max(rho, 0.2));
                float dop = 1.0 + dot(normalize(vel), tang) * vkep * 1.35;
                vec4 rec = vec4(x, dop, cos(phase), sin(phase));
                if (found == 0) { hitA = rec; found = 1; }
                else if (found == 1) { hitB = rec; found = 2; }
            }
        }
        pos = nxt;
        if (r > 22.0) break;
    }

    oHitA = hitA;
    oHitB = hitB;
    oSky = vec4(captured ? vec3(0.0) : normalize(vel), photon);
}
)";

constexpr const char *kDraw = R"(#version 330 core
uniform sampler2D tHitA;
uniform sampler2D tHitB;
uniform sampler2D tSky;
uniform vec2 uResolution;
uniform float uTime;
uniform vec2 uDrift;
uniform vec3 uAccent;
uniform vec3 uDisk;
uniform vec3 uGrade;
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
        vec2 f = fract(uv) - 0.5;
        float h = hash12(id + float(k) * 17.13);
        if (h > 0.905) {
            float d = length(f);
            float br = smoothstep(0.080 + h * 0.035, 0.0, d);
            float tw = 0.78 + 0.22 * sin(uTime * (0.22 + h * 1.0) + h * 40.0);
            vec3 tc = mix(mix(vec3(0.75, 0.82, 0.95), uAccent, 0.35), vec3(0.95, 0.97, 1.0), fract(h * 9.1));
            if (fract(h * 13.7) > 0.82)
                tc = mix(vec3(0.55, 0.70, 1.0), uAccent, 0.45);
            col += br * tc * tw * (0.28 + 2.4 * pow(h, 9.0));
            if (h > 0.988) {
                vec3 sp = mix(vec3(0.75, 0.88, 1.0), uAccent, 0.4);
                col += sp * 0.35 * exp(-abs(f.x) * 55.0) * exp(-abs(f.y) * 10.0);
                col += sp * 0.18 * exp(-abs(f.y) * 55.0) * exp(-abs(f.x) * 10.0);
            }
        }
    }
    return col;
}

void disk(vec4 rec, float wt, inout vec3 col, inout float trans)
{
    float x = rec.x;
    if (x < -0.5) return;
    float s = rec.w * cos(wt) - rec.z * sin(wt);
    float spir = 0.5 + 0.5 * s;
    float dens = pow(max(1.0 - x, 0.0), 1.05) * (0.62 + 0.38 * spir);
    dens *= smoothstep(0.0, 0.08, x) * smoothstep(1.0, 0.72, x);
    if (dens <= 0.0) return;
    float dop = rec.y;
    vec3 outerC = uDisk * 0.42;
    vec3 midC = uDisk;
    vec3 hotC = mix(vec3(1.15, 1.25, 1.40), uAccent, 0.55) * 1.25;
    vec3 dcol = mix(hotC, mix(midC, outerC, smoothstep(0.18, 1.0, x)), smoothstep(0.0, 0.48, x));
    dcol *= pow(clamp(dop, 0.30, 2.1), 2.4);
    dcol += uAccent * smoothstep(1.12, 1.55, dop) * 0.40;
    dcol *= mix(vec3(1.0), vec3(0.55, 0.40, 0.48), smoothstep(1.0, 0.55, dop));
    col += trans * dcol * dens * 0.72;
    trans *= 1.0 - clamp(dens * 0.55, 0.0, 0.78);
}

void main()
{
    vec2 q = gl_FragCoord.xy / uResolution;
    vec2 suv = clamp((q - 0.5) * 0.90 + 0.5 + uDrift, vec2(0.001), vec2(0.999));
    vec4 a = texture(tHitA, suv);
    vec4 b = texture(tHitB, suv);
    vec4 sky = texture(tSky, suv);
    float wt = uTime * 0.38;
    vec3 col = sky.w * uAccent;
    float trans = 1.0;
    disk(a, wt, col, trans);
    disk(b, wt, col, trans);
    if (dot(sky.xyz, sky.xyz) > 0.25)
        col += trans * starfield(normalize(sky.xyz));
    col *= uGrade;
    col = col / (1.0 + col * 0.70);
    col = pow(clamp(col, 0.0, 1.0), vec3(0.92));
    float vig = pow(16.0 * q.x * q.y * (1.0 - q.x) * (1.0 - q.y), 0.38);
    col *= mix(0.22, 1.0, vig);
    float g = hash12(gl_FragCoord.xy + vec2(fract(uTime) * 73.1, fract(uTime * 0.37) * 19.0));
    col += (g - 0.5) * 0.012;
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

    // Each frame is timed from the moment the previous one reached the screen.
    //
    // This was a repeating 16 ms timer, and on a 240 Hz screen that is the
    // bug. A frame there lasts 4.17 ms, so sixty frames a second is exactly
    // four refreshes - 16.67 ms. A 16 ms timer runs 0.67 ms ahead of that
    // every frame, and after twenty five frames it has gained a whole refresh:
    // one frame is shown twice or skipped. That is the small hitch roughly
    // every half second that looked like the background lagging, with nothing
    // else going on at all.
    //
    // frameSwapped fires just after a frame is handed to the screen, which is
    // on a refresh. Measuring from there, and asking for the next frame a
    // little before the target refresh, lands every frame on the same beat
    // and cannot drift, because each one is re-anchored to the last.
    m_overlayTimer.setTimerType(Qt::PreciseTimer);
    m_overlayTimer.setSingleShot(true);
    connect(&m_overlayTimer, &QTimer::timeout, this, QOverload<>::of(&QWidget::update));
    connect(this, &QOpenGLWidget::frameSwapped, this, &AuroraWidget::armNextFrame);

    // If a frame is ever missed - hidden, minimised, a lost context - nothing
    // would swap and nothing would ask again. This restarts the chain.
    m_frameWatchdog.setInterval(250);
    connect(&m_frameWatchdog, &QTimer::timeout, this, [this]() {
        if (m_running && isVisible() && m_clock.elapsed() - m_lastFrameMs > 200)
            update();
    });
    m_frameWatchdog.start();
}

double AuroraWidget::framePeriodMs() const
{
    // About sixty a second, rounded to a whole number of the screen's own
    // refreshes: four on 240 Hz, two on 120 Hz, one on 60 Hz. A period that
    // is not a whole number of refreshes is uneven by construction.
    double refresh = 60.0;
    if (QScreen *s = screen())
        refresh = s->refreshRate() > 1.0 ? s->refreshRate() : 60.0;
    const int refreshesPerFrame = qMax(1, qRound(refresh / 60.0));
    return 1000.0 * refreshesPerFrame / refresh;
}

void AuroraWidget::armNextFrame()
{
    // The self-driving frame loop is only for the drawn hole, which changes
    // every frame. A picture does not need it: a still one never changes, and
    // an animated one is driven by its own QMovie, which asks for a repaint
    // only when it has a new frame. Spinning at sixty a second behind a picture
    // is the "black hole still running" that nobody wanted, so in picture mode
    // we let the loop stop. Real changes - the movie, a resize, a new dim -
    // still ask for their own repaint.
    if (!m_running || m_background != Background::Hole || m_overlayTimer.isActive())
        return;

    // Something else in the window can cause a swap too. Timing from our own
    // last frame rather than from this swap keeps those from pushing ours
    // back, and ignoring swaps while a frame is already booked keeps them from
    // restarting it.
    const double period = framePeriodMs();
    const qint64 sinceOurs = m_lastFrameMs < 0 ? 0 : m_clock.elapsed() - m_lastFrameMs;

    // Ask a little early. The request is served, drawn and handed over before
    // the refresh it is aimed at, and then waits for that refresh.
    constexpr double Margin = 3.0;
    const int delay = qBound(0, int(period - double(sinceOurs) - Margin), int(period));
    m_overlayTimer.start(delay);
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
        m_runOffset = m_clock.elapsed() - qint64(m_time * 1000.f);
        // One frame starts the chain; every swap after it books the next.
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

    // Transparent pixels in a GIF would upload as black and read as holes in
    // the wallpaper. They are painted onto the same night colour as the rest
    // of the window first, so a frame with see-through parts stays one picture.
    QImage flat(frame.size(), QImage::Format_RGBA8888);
    flat.fill(QColor(7, 9, 13, 255));
    {
        QPainter painter(&flat);
        painter.drawImage(0, 0, frame);
    }
    const QImage rgba = flat;

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

    // Wall clock, not a capped step. A late frame used to slow the disk, which
    // is the hitch that reads as the background stuttering.
    m_lastFrameMs = m_clock.elapsed();
    if (m_running)
        m_time = float(m_lastFrameMs - m_runOffset) * 0.001f;

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

void AuroraWidget::ensureBakeTargets(int width, int height)
{
    if (m_bakeFbo) {
        glDeleteFramebuffers(1, &m_bakeFbo);
        m_bakeFbo = 0;
    }
    const GLuint olds[3] = {m_hitA, m_hitB, m_sky};
    glDeleteTextures(3, olds);
    m_hitA = m_hitB = m_sky = 0;

    auto make = [this](GLuint &name, int w, int h) {
        glGenTextures(1, &name);
        glBindTexture(GL_TEXTURE_2D, name);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    make(m_hitA, width, height);
    make(m_hitB, width, height);
    make(m_sky, width, height);

    glGenFramebuffers(1, &m_bakeFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_bakeFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_hitA, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, m_hitB, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, m_sky, 0);
    const GLenum bufs[3] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
    glDrawBuffers(3, bufs);
    const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
    if (!ok) {
        const GLuint made[3] = {m_hitA, m_hitB, m_sky};
        glDeleteFramebuffers(1, &m_bakeFbo);
        glDeleteTextures(3, made);
        m_bakeFbo = 0;
        m_hitA = m_hitB = m_sky = 0;
        m_bakeSize = QSize();
    }
}

void AuroraWidget::bakeHole(int width, int height)
{
    if (!m_bakeProgram || !m_bakeProgram->isLinked() || width <= 0 || height <= 0)
        return;
    ensureBakeTargets(width, height);
    if (m_bakeFbo == 0)
        return;

    const GLuint windowFbo = defaultFramebufferObject();
    glBindFramebuffer(GL_FRAMEBUFFER, m_bakeFbo);
    glViewport(0, 0, width, height);
    glDisable(GL_BLEND);
    m_bakeProgram->bind();
    m_bakeProgram->setUniformValue(m_bakeRes, QVector2D(float(width), float(height)));
    m_bakeProgram->setUniformValue(m_bakeZoom, 1.22f);
    // About a third of the original orbit, so the ring moves and a frame of
    // ray marching is not asked for sixty times a second.
    m_bakeProgram->setUniformValue(m_bakeOrbit, m_time * 0.02f);
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    m_bakeProgram->release();
    glBindFramebuffer(GL_FRAMEBUFFER, windowFbo);
    glViewport(0, 0, width, height);
    m_bakeSize = QSize(width, height);
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
    if (!m_program->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertex)
        || !m_program->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragment)
        || !m_program->link()) {
        qWarning() << "aurora draw" << m_program->log();
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

    delete m_bakeProgram;
    m_bakeProgram = nullptr;
    delete m_program;
    m_program = nullptr;
    if (m_bakeFbo)
        glDeleteFramebuffers(1, &m_bakeFbo);
    const GLuint baked[3] = {m_hitA, m_hitB, m_sky};
    glDeleteTextures(3, baked);
    m_bakeFbo = 0;
    m_hitA = m_hitB = m_sky = 0;
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
