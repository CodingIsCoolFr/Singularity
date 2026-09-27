#include "Theme.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QGuiApplication>
#include <QHash>
#include <QPalette>
#include <QRegularExpression>
#include <QStyleHints>
#include <QObject>
#include <QVector3D>
#include <QWidget>
#include <algorithm>
#include <cmath>

#ifdef Q_OS_WIN
#    include <dwmapi.h>
#    include <windows.h>
#endif

namespace Theme {

char Dark[8] = "#07090e";
char Light[8] = "#eef4fb";
char MidGray[8] = "#8b97ad";
char LightGray[8] = "#d5deea";
char Accent[8] = "#6ee7d8";
char AccentHover[8] = "#9af3e6";
char Highlight[8] = "#8aa4ff";
char Green[8] = "#3dd68c";
char Yellow[8] = "#e0b35c";
char Red[8] = "#ff6b7a";
char SurfaceRail[8] = "#0a0d14";
char SurfaceSidebar[8] = "#10151f";
char SurfaceChat[8] = "#151b28";
char SurfaceInput[8] = "#1c2433";
char SurfaceHover[8] = "#273044";
char Border[8] = "#2c3548";
char TextPrimary[8] = "#eef4fb";
char TextMuted[8] = "#9aa6bc";
char TextFaint[8] = "#6b768c";
char Deleted[8] = "#ff6b7a";

namespace {

QColor g_seed(QStringLiteral("#6ee7d8"));
QVector3D g_holeAccent(0.22f, 0.92f, 0.88f);
QVector3D g_holeDisk(0.18f, 0.72f, 0.70f);
QVector3D g_holeGrade(0.86f, 1.06f, 1.04f);

const Preset kPresets[] = {
    {"Monochrome", DefaultSeed},
    {"Singularity", "#6ee7d8"},
    {"Ember", "#eb2e4a"},
    {"Ocean", "#468cbe"},
    {"Violet", "#9b59b6"},
    {"Jade", "#27ae60"},
    {"Gold", "#f1c40f"},
    {"Rose", "#ff6b9d"},
    {"Ice", "#7dd3fc"},
    {"Sunset", "#fb923c"},
    {"Magenta", "#e879f9"},
    {"Void", "#000000"},
};

QString rgbaOf(const QColor &c, int alpha)
{
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(c.red())
        .arg(c.green())
        .arg(c.blue())
        .arg(alpha);
}

QColor mixRgb(const QColor &a, const QColor &b, qreal t)
{
    return QColor(qBound(0, qRound(a.red() + (b.red() - a.red()) * t), 255),
                  qBound(0, qRound(a.green() + (b.green() - a.green()) * t), 255),
                  qBound(0, qRound(a.blue() + (b.blue() - a.blue()) * t), 255));
}

void writeHex(char *buf, const QColor &c)
{
    const QColor ok = c.isValid() ? c : QColor(QStringLiteral("#6ee7d8"));
    const QByteArray hex = ok.name(QColor::HexRgb).toLatin1();
    qstrncpy(buf, hex.constData(), 8);
}

// Keep the accretion disk lit even when the seed is black / grey (hue = -1).
// Scaling a zero vector is a no-op, so that case falls back to Singularity teal.
void storeHole(const QColor &accentCol, bool silverDisk)
{
    if (silverDisk) {
        g_holeAccent = QVector3D(0.82f, 0.90f, 1.00f);
        g_holeDisk = QVector3D(0.62f, 0.78f, 1.00f);
        g_holeGrade = QVector3D(1.02f, 1.06f, 1.16f);
        return;
    }

    QVector3D a(float(accentCol.redF()), float(accentCol.greenF()), float(accentCol.blueF()));
    const float mx = qMax(a.x(), qMax(a.y(), a.z()));
    if (mx < 0.001f)
        a = QVector3D(0.22f, 0.92f, 0.88f);
    else if (mx < 0.42f)
        a *= (0.42f / mx);

    g_holeAccent = a;
    g_holeDisk = QVector3D(qBound(0.12f, a.x() * 0.78f, 1.f),
                           qBound(0.12f, a.y() * 0.78f, 1.f),
                           qBound(0.12f, a.z() * 0.78f, 1.f));
    g_holeGrade = QVector3D(0.78f + a.x() * 0.38f,
                            0.78f + a.y() * 0.38f,
                            0.78f + a.z() * 0.38f);
}

QString expand(const QString &sheet)
{
    const QColor accent{QLatin1String(Accent)};
    const QColor input{QLatin1String(SurfaceInput)};
    const QColor chat{QLatin1String(SurfaceChat)};
    const QColor dark{QLatin1String(Dark)};
    QColor voice = mixRgb(dark, accent, 0.22);

    const QHash<QString, QString> tokens = {
        {QStringLiteral("@dark"), QLatin1String(Dark)},
        {QStringLiteral("@light"), QLatin1String(Light)},
        {QStringLiteral("@midGray"), QLatin1String(MidGray)},
        {QStringLiteral("@lightGray"), QLatin1String(LightGray)},
        {QStringLiteral("@accentHover"), QLatin1String(AccentHover)},
        {QStringLiteral("@accent"), QLatin1String(Accent)},
        {QStringLiteral("@highlight"), QLatin1String(Highlight)},
        {QStringLiteral("@green"), QLatin1String(Green)},
        {QStringLiteral("@yellow"), QLatin1String(Yellow)},
        {QStringLiteral("@red"), QLatin1String(Red)},
        {QStringLiteral("@text"), QLatin1String(TextPrimary)},
        {QStringLiteral("@muted"), QLatin1String(TextMuted)},
        {QStringLiteral("@faint"), QLatin1String(TextFaint)},
        {QStringLiteral("@rail"), QLatin1String(SurfaceRail)},
        {QStringLiteral("@sidebar"), QLatin1String(SurfaceSidebar)},
        {QStringLiteral("@chat"), QLatin1String(SurfaceChat)},
        {QStringLiteral("@hover"), QLatin1String(SurfaceHover)},
        {QStringLiteral("@input"), QLatin1String(SurfaceInput)},
        {QStringLiteral("@border"), QLatin1String(Border)},
        {QStringLiteral("@deleted"), QLatin1String(Deleted)},
        {QStringLiteral("@glass"), rgbaOf(chat, 255)},
        // Enough to read a name over wherever the hole happens to be bright,
        // little enough that it still reads as glass rather than a panel.
        {QStringLiteral("@glass150"), rgbaOf(chat, 178)},
        {QStringLiteral("@accent22"), rgbaOf(accent, 22)},
        {QStringLiteral("@accent28"), rgbaOf(accent, 28)},
        {QStringLiteral("@accent38"), rgbaOf(accent, 38)},
        {QStringLiteral("@accent40"), rgbaOf(accent, 40)},
        {QStringLiteral("@accent55"), rgbaOf(accent, 55)},
        {QStringLiteral("@input200"), rgbaOf(input, 200)},
        {QStringLiteral("@input220"), rgbaOf(input, 220)},
        {QStringLiteral("@input230"), rgbaOf(input, 230)},
        {QStringLiteral("@input250"), rgbaOf(input, 250)},
        {QStringLiteral("@dark140"), rgbaOf(dark, 72)},
        {QStringLiteral("@voicePanel"), rgbaOf(voice, 160)},
    };

    QString out = sheet;
    QList<QString> keys = tokens.keys();
    std::sort(keys.begin(), keys.end(), [](const QString &a, const QString &b) {
        return a.size() > b.size();
    });
    for (const QString &key : keys)
        out.replace(key, tokens.value(key));
    return out;
}

class TitleBarPainter : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Show || event->type() == QEvent::WinIdChange) {
            if (auto *widget = qobject_cast<QWidget *>(watched)) {
                if (widget->isWindow())
                    applyDarkTitleBar(widget);
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

// Makes every drop-down list solid.
//
// The Windows 11 style gives a combo box's list a see-through window so it can
// round the corners, and then draws the background itself. With a style sheet
// on top, that drawing is skipped, so the list came out with no background at
// all: its items floated over whatever was underneath, and on the sign-in
// screen "Backup code" sat on top of the code field's placeholder text.
//
// Caught at Polish, which comes before the list's window exists, so the
// window is created solid rather than changed afterwards.
class PopupFix : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Polish || event->type() == QEvent::Show) {
            auto *widget = qobject_cast<QWidget *>(watched);
            if (widget && widget->inherits("QComboBoxPrivateContainer")
                && widget->styleSheet().isEmpty()) {
                // The Windows 11 style makes this window see-through so it can
                // round its own corners, then paints the background itself.
                // With a style sheet on the application that painting is
                // skipped, so nothing fills the window and the items float.
                // A palette colour will not show for the same reason - a
                // styled widget ignores it - so the background is set the one
                // way a style sheet honours, with a rule of its own.
                widget->setAttribute(Qt::WA_TranslucentBackground, false);
                widget->setStyleSheet(
                    QStringLiteral("QComboBoxPrivateContainer { background-color: %1; "
                                   "border: 1px solid %2; }")
                        .arg(QLatin1String(SurfaceSidebar), QLatin1String(Border)));
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

} // namespace

void installPopupFix()
{
    static bool installed = false;
    if (installed || !qApp)
        return;

    installed = true;
    qApp->installEventFilter(new PopupFix(qApp));
}

void applySeed(const QColor &seedIn)
{
    const QColor seed = seedIn.isValid() ? seedIn : QColor(QLatin1String(DefaultSeed));
    g_seed = seed;

    float h = 0, s = 0, l = 0, a = 1;
    seed.getHslF(&h, &s, &l, &a);

    // Black / grey / white report hue = -1. setHslF(-1, ...) makes an invalid
    // QColor, which writes empty tokens into the stylesheet and zeros the hole.
    const bool achromatic = (h < 0.f) || (s < 0.02f);
    const bool darkMode = l < 0.22f;
    if (h < 0.f)
        h = 0.55f;

    float accentS = s;
    float accentL = l;
    if (darkMode) {
        accentS = achromatic ? 0.10f : qMax(s, 0.35f);
        accentL = 0.60f;
    } else {
        if (accentS < 0.35f)
            accentS = 0.55f;
        if (accentL < 0.42f)
            accentL = 0.55f;
        else if (accentL > 0.72f)
            accentL = 0.65f;
    }

    QColor accent;
    accent.setHslF(h, qMin(accentS, 0.82f), accentL, 1.0f);
    if (!accent.isValid())
        accent = QColor(QStringLiteral("#6ee7d8"));

    QColor accentHover;
    accentHover.setHslF(h, qMin(accentS, 0.78f), qBound(0.50f, accentL + 0.10f, 0.82f), 1.0f);
    if (!accentHover.isValid())
        accentHover = mixRgb(accent, QColor(QStringLiteral("#ffffff")), 0.18);

    QColor highlight;
    highlight.setHslF(std::fmod(h + 0.08f, 1.0f), qMin(accentS * 0.7f, 0.65f),
                      qBound(0.58f, accentL + 0.12f, 0.82f), 1.0f);
    if (!highlight.isValid())
        highlight = mixRgb(accent, QColor(QStringLiteral("#8aa4ff")), 0.35);

    // Dark seeds keep night chrome and a silver disk. Colourful seeds tint
    // the hole the same way Singularity does: vivid accent, dimmer disk,
    // grade that can go a little over 1 so the photon ring stays visible.
    storeHole(accent, darkMode && achromatic);

    const qreal ink = darkMode ? 0.018 : 0.045;
    writeHex(Dark, mixRgb(QColor(QStringLiteral("#05060a")), accent, ink));
    writeHex(Light, QColor(QStringLiteral("#eef4fb")));
    writeHex(MidGray, mixRgb(QColor(QStringLiteral("#8b97ad")), accent, 0.08));
    writeHex(LightGray, mixRgb(QColor(QStringLiteral("#d5deea")), accent, 0.06));
    writeHex(Accent, accent);
    writeHex(Highlight, highlight);
    writeHex(AccentHover, accentHover);
    writeHex(TextPrimary, QColor(QStringLiteral("#eef4fb")));
    writeHex(TextMuted, mixRgb(QColor(QStringLiteral("#9aa6bc")), accent, 0.08));
    writeHex(TextFaint, mixRgb(QColor(QStringLiteral("#6b768c")), accent, 0.06));
    writeHex(SurfaceRail, mixRgb(QColor(QStringLiteral("#0a0d14")), accent, darkMode ? 0.02 : 0.05));
    writeHex(SurfaceSidebar, mixRgb(QColor(QStringLiteral("#10151f")), accent, darkMode ? 0.025 : 0.06));
    writeHex(SurfaceChat, mixRgb(QColor(QStringLiteral("#151b28")), accent, darkMode ? 0.02 : 0.05));
    writeHex(SurfaceHover, mixRgb(QColor(QStringLiteral("#273044")), accent, darkMode ? 0.06 : 0.10));
    writeHex(SurfaceInput, mixRgb(QColor(QStringLiteral("#1c2433")), accent, darkMode ? 0.04 : 0.08));
    writeHex(Border, mixRgb(QColor(QStringLiteral("#2c3548")), accent, darkMode ? 0.06 : 0.12));
}

QColor seedColor()
{
    return g_seed;
}

QVector3D holeAccent()
{
    return g_holeAccent;
}

QVector3D holeDisk()
{
    return g_holeDisk;
}

QVector3D holeGrade()
{
    return g_holeGrade;
}

const Preset *presets(int *count)
{
    if (count)
        *count = int(sizeof(kPresets) / sizeof(kPresets[0]));
    return kPresets;
}

// The program is dark, always, and says so to Qt.
//
// With Windows in light mode, Qt's Windows 11 style draws in its light colours
// wherever the style sheet does not name one - the fields and edges of the
// sign-in window came out white with pale text on them. Switching Windows back
// to dark did not repair a window already built. Telling Qt the scheme is dark,
// and giving it a palette with every colour set, takes the system setting out
// of the picture: a colour set here is not replaced when Windows changes its
// mind, because Qt only fills in the roles a palette leaves unset.
void applyPalette()
{
    if (!qApp)
        return;

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
#endif

    const QColor text{QLatin1String(TextPrimary)};
    const QColor faint{QLatin1String(TextFaint)};
    const QColor chat{QLatin1String(SurfaceChat)};
    const QColor input{QLatin1String(SurfaceInput)};
    const QColor hover{QLatin1String(SurfaceHover)};
    const QColor sidebar{QLatin1String(SurfaceSidebar)};
    const QColor rail{QLatin1String(SurfaceRail)};
    const QColor border{QLatin1String(Border)};
    const QColor accent{QLatin1String(Accent)};
    const QColor dark{QLatin1String(Dark)};

    QPalette palette;
    const auto both = [&palette](QPalette::ColorRole role, const QColor &colour) {
        palette.setColor(QPalette::Active, role, colour);
        palette.setColor(QPalette::Inactive, role, colour);
    };

    both(QPalette::Window, chat);
    both(QPalette::WindowText, text);
    both(QPalette::Base, input);
    both(QPalette::AlternateBase, sidebar);
    both(QPalette::ToolTipBase, rail);
    both(QPalette::ToolTipText, text);
    both(QPalette::PlaceholderText, faint);
    both(QPalette::Text, text);
    both(QPalette::Button, input);
    both(QPalette::ButtonText, text);
    both(QPalette::BrightText, QColor(Qt::white));
    both(QPalette::Light, hover);
    both(QPalette::Midlight, border);
    both(QPalette::Mid, border);
    both(QPalette::Dark, rail);
    both(QPalette::Shadow, dark);
    both(QPalette::Highlight, accent);
    both(QPalette::HighlightedText, dark);
    both(QPalette::Link, accent);
    both(QPalette::LinkVisited, accent);
    both(QPalette::Accent, accent);

    // Disabled is the same dark ground with the writing dimmed, never the
    // light greys Qt would otherwise fill in.
    const QPalette::ColorRole roles[] = {
        QPalette::Window, QPalette::Base, QPalette::AlternateBase, QPalette::ToolTipBase,
        QPalette::Button, QPalette::Light, QPalette::Midlight, QPalette::Mid,
        QPalette::Dark, QPalette::Shadow, QPalette::Highlight, QPalette::Accent,
        QPalette::ToolTipText, QPalette::BrightText, QPalette::Link, QPalette::LinkVisited,
        QPalette::HighlightedText};
    for (const QPalette::ColorRole role : roles)
        palette.setColor(QPalette::Disabled, role, palette.color(QPalette::Active, role));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, faint);
    palette.setColor(QPalette::Disabled, QPalette::Text, faint);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, faint);
    palette.setColor(QPalette::Disabled, QPalette::PlaceholderText, faint);

    QApplication::setPalette(palette);
}

void showThrough(QAbstractScrollArea *area)
{
    if (!area)
        return;
    area->setAutoFillBackground(false);
    QPalette clear = area->palette();
    clear.setColor(QPalette::Base, Qt::transparent);
    clear.setColor(QPalette::Window, Qt::transparent);
    area->setPalette(clear);
    if (QWidget *view = area->viewport()) {
        view->setAutoFillBackground(false);
        view->setPalette(clear);
    }
}

void installDarkTitleBars()
{
    static bool installed = false;
    if (installed || !qApp)
        return;

    installed = true;
    qApp->installEventFilter(new TitleBarPainter(qApp));
}

// Paints the real Windows title bar rather than drawing a replacement.
//
// A frameless window with a hand made title bar is the usual way to do this,
// and it costs you snap layouts, the system menu, the double click to
// maximise, and correct behaviour on a second monitor. Windows 11 will simply
// colour its own caption if asked, which keeps all of that and still leaves no
// seam across the top.
//
// Silently does nothing on Windows 10 and older, where the attributes are not
// recognised. A pale title bar is a blemish, not a fault.
void applyDarkTitleBar(QWidget *window)
{
#ifdef Q_OS_WIN
    if (!window)
        return;
    if (window->windowFlags() & Qt::FramelessWindowHint)
        return;

    // Forces the frame to exist now. Before this the handle can be null, and
    // the call would quietly do nothing.
    window->winId();

    auto handle = reinterpret_cast<HWND>(window->window()->winId());
    if (!handle)
        return;

    // Windows wants 0x00BBGGRR, which is the reverse of the usual order.
    const auto toWindows = [](const char *hex) -> COLORREF {
        // Braces, not brackets. With brackets this is read as a function
        // declaration rather than a variable, which is the most vexing parse.
        const QColor colour{QLatin1String(hex)};
        return RGB(colour.red(), colour.green(), colour.blue());
    };

    // 20, 35, 36 and 34 are DWMWA_USE_IMMERSIVE_DARK_MODE, _CAPTION_COLOR,
    // _TEXT_COLOR and _BORDER_COLOR. Written as numbers because older Windows
    // SDK headers do not name them all.
    const BOOL useDarkMode = TRUE;
    DwmSetWindowAttribute(handle, 20, &useDarkMode, sizeof(useDarkMode));

    const COLORREF caption = toWindows(SurfaceRail);
    DwmSetWindowAttribute(handle, 35, &caption, sizeof(caption));

    const COLORREF text = toWindows(TextMuted);
    DwmSetWindowAttribute(handle, 36, &text, sizeof(text));

    // The same colour as the caption, so the window has no outline of its own
    // and reads as one solid shape.
    const COLORREF border = toWindows(Border);
    DwmSetWindowAttribute(handle, 34, &border, sizeof(border));
#else
    Q_UNUSED(window)
#endif
}

QString applicationStyleSheet()
{
    return expand(QStringLiteral(R"(
QWidget {
    background-color: transparent;
    color: @text;
    font-family: "Poppins", "Segoe UI", Arial, sans-serif, "Segoe UI Emoji";
    font-size: 13px;
}
QMainWindow { background-color: #02040a; }
QOpenGLWidget, AuroraWidget {
    background: none;
    border: none;
}
QDialog { background-color: @chat; }

#StatusMessage { color: @faint; background: transparent; }
#StatusDot { background: transparent; padding-right: 4px; }

#StatusChip {
    background-color: @chat;
    border: 1px solid @accent22;
    border-radius: 8px;
}

#CaptionMin, #CaptionMax, #CaptionClose {
    background-color: @chat;
    color: @muted;
    border: none;
    border-radius: 8px;
    padding: 0;
    min-width: 46px;
    max-width: 46px;
    min-height: 28px;
    font-size: 11px;
    margin: 0 2px;
}
#CaptionMin:hover, #CaptionMax:hover {
    background: @hover;
    color: @text;
}
#CaptionClose:hover {
    background-color: #e81123;
    color: #ffffff;
}

#AppMenu {
    background-color: @chat;
    color: @muted;
    padding: 0 6px;
    border: 1px solid @accent22;
    border-radius: 8px;
}
#AppMenu::item {
    background: transparent;
    padding: 4px 12px;
    border-radius: 8px;
    color: @muted;
}
#AppMenu::item:selected { background-color: @hover; color: @text; }

/* Solid while the black hole is up. A see-through widget above that OpenGL
   surface paints black on Windows. Active Now cards stay solid either way:
   they are short blocks of text and a wash behind them turns to mud.

   Over a picture the same panels are clear. The dim slider is the only
   darkening, so a second veil would put the boxes back. */
#GuildRail, #Sidebar, #ChatColumn, #ActivityCard {
    background-color: @chat;
    border: 1px solid @accent38;
    border-radius: 18px;
}
QMainWindow[glass="true"] #GuildRail,
QMainWindow[glass="true"] #Sidebar,
QMainWindow[glass="true"] #ChatColumn,
QMainWindow[glass="true"] #MemberPanel {
    background-color: transparent;
    border: none;
}

/* The member list is glass rather than a panel.

   Filled solid it read as a grey slab bolted to the side - the one part of
   the window that looked like somebody else's client. It carries short lines
   with a lot of space between them, so unlike the conversation it can afford
   to let the hole through and still be easy to read. */
#MemberPanel {
    background-color: @glass150;
    border: 1px solid @accent22;
    border-radius: 18px;
}
#GuildRail QListWidget,
#Sidebar QListWidget,
#MemberPanel QListWidget {
    background-color: transparent;
    border: none;
    outline: none;
}

/* The people down the right. */
#MemberHeader, #MemberPanel QWidget {
    background: transparent;
}
#MemberToggle {
    background: transparent;
    border: none;
    color: @muted;
    font-size: 12px;
    font-weight: 700;
    letter-spacing: 1px;
    text-align: left;
    padding: 14px 14px 8px 14px;
}
#MemberToggle:hover { color: @text; }

#MemberCounts {
    font-size: 11.5px;
    color: @muted;
}

#SidebarHeader {
    color: @text;
    font-size: 15px;
    font-weight: 600;
    letter-spacing: 0.2px;
    padding: 18px 16px 14px 16px;
    background: transparent;
    border-bottom: 1px solid @accent22;
}

#UserPanel {
    background-color: @dark140;
    border-top: 1px solid @accent22;
    border-bottom-left-radius: 18px;
    border-bottom-right-radius: 18px;
}
QMainWindow[glass="true"] #UserPanel {
    background-color: transparent;
    border: none;
}
#SelfName   { color: @text; font-size: 13px; font-weight: 600; background: transparent; }
#SelfStatus { color: @faint; font-size: 11px; background: transparent; }
#UserPanel QPushButton {
    background: transparent;
    border: none;
    color: @muted;
    border-radius: 6px;
    padding: 0;
    min-width: 26px;
    max-width: 26px;
    min-height: 26px;
    max-height: 26px;
}
#UserPanel QPushButton:hover { background-color: @hover; color: @text; }
#UserPanel QPushButton:checked { color: @red; background: transparent; }

#VoicePanel {
    background-color: @voicePanel;
    border-top: 1px solid @accent40;
}
QMainWindow[glass="true"] #VoicePanel {
    background-color: transparent;
    border: none;
}
#VoiceHeading { color: @text; font-size: 12px; font-weight: 700; background: transparent; }
#VoiceChannel { color: @muted; font-size: 11px; background: transparent; }

#VoicePanel QPushButton {
    padding: 4px 12px;
    min-height: 32px;
    font-size: 11px;
    font-weight: 600;
    border-radius: 8px;
    background-color: transparent;
    color: @light;
    border: 1px solid @accent38;
}
#VoicePanel QPushButton:hover {
    background-color: @accent28;
    color: @light;
}
#VoicePanel QPushButton:checked {
    background-color: @red;
    color: @light;
    border: 1px solid @red;
}
#VoicePanel QPushButton:checked:hover {
    background-color: @red;
    color: @light;
}

#ChatHeader {
    background-color: transparent;
    border-bottom: 1px solid @accent22;
}
#ChannelTitle { color: @text; font-size: 16px; font-weight: 600; letter-spacing: 0.2px; }
#ChannelTopic { color: @faint; font-size: 12px; }

#CallView { background: transparent; border: none; }

QTextBrowser {
    background-color: transparent;
    border: none;
    padding: 16px 22px;
}

#Composer { background: transparent; }
#ComposerBox {
    background-color: @input230;
    border: 1px solid @accent40;
    border-radius: 14px;
}
#ComposerBox:focus-within {
    border-color: @accent;
    background-color: @input250;
}
QMainWindow[glass="true"] #ComposerBox,
QMainWindow[glass="true"] #ComposerBox:focus-within {
    background-color: transparent;
    border-color: transparent;
}
QTextEdit#MessageInput {
    background-color: transparent;
    border: none;
    padding: 4px 2px;
    color: @text;
    font-family: "Lora", Georgia, serif;
    font-size: 14px;
}
QPushButton#ComposerTool {
    background: transparent;
    border: none;
    color: @muted;
    padding: 0 8px;
    min-width: 64px;
    min-height: 28px;
    max-height: 28px;
    font-family: "Segoe UI", Arial, sans-serif;
    font-size: 12px;
    font-weight: 600;
    border-radius: 6px;
}
QPushButton#ComposerTool:hover {
    background-color: @hover;
    color: @text;
    border: none;
}

#TypingLabel {
    color: @faint;
    font-size: 11px;
    font-style: italic;
    padding: 2px 24px;
    background: transparent;
}

QPushButton {
    background-color: @input;
    color: @text;
    border: 1px solid @border;
    border-radius: 10px;
    padding: 8px 16px;
}
QPushButton:hover   { background-color: @hover; border-color: @accent; }
QPushButton:pressed { background-color: @rail; }
QPushButton#PrimaryButton {
    background-color: @accent;
    color: @dark;
    border: none;
    font-weight: 600;
    padding: 9px 18px;
}
QPushButton#PrimaryButton:hover { background-color: @accentHover; }
QPushButton#TabButton {
    background: transparent;
    border: none;
    color: @muted;
    padding: 6px 10px;
    border-radius: 8px;
}
QPushButton#TabButton:hover { color: @text; background-color: @hover; }
QPushButton#TabButton[active="true"] { color: @text; background-color: @hover; font-weight: 600; }

QLineEdit {
    background-color: @input;
    border: 1px solid @border;
    border-radius: 10px;
    padding: 9px 12px;
    color: @text;
    selection-background-color: @accent;
    selection-color: @dark;
}
QLineEdit:focus { border-color: @accent; }

QComboBox {
    background-color: @input;
    border: 1px solid @border;
    border-radius: 10px;
    padding: 6px 10px;
    color: @text;
}
QComboBox QAbstractItemView {
    background-color: @sidebar;
    border: 1px solid @border;
    selection-background-color: @hover;
    color: @text;
}

QSlider::groove:horizontal {
    height: 4px;
    background: @input;
    border-radius: 2px;
}
QSlider::sub-page:horizontal {
    background: @accent;
    border-radius: 2px;
}
QSlider::handle:horizontal {
    width: 14px;
    height: 14px;
    margin: -5px 0;
    border-radius: 7px;
    background: @accent;
}
QSlider::handle:horizontal:hover { background: @accentHover; }
QSlider::groove:vertical {
    width: 4px;
    background: @input;
    border-radius: 2px;
}
QSlider::handle:vertical {
    width: 14px;
    height: 14px;
    margin: 0 -5px;
    border-radius: 7px;
    background: @accent;
}

QCheckBox { spacing: 8px; }
QCheckBox::indicator {
    width: 16px;
    height: 16px;
    border: 1px solid @border;
    border-radius: 5px;
    background: @input;
}
QCheckBox::indicator:hover { border-color: @accent; }
QCheckBox::indicator:checked {
    background: @accent;
    border-color: @accent;
}

QListWidget, QTableWidget {
    background-color: transparent;
    border: none;
    border-radius: 12px;
    outline: none;
}
QStackedWidget, QSplitter {
    background: transparent;
}

QHeaderView::section {
    background-color: @input;
    color: @muted;
    border: none;
    padding: 6px;
}

QScrollBar:vertical {
    background: transparent;
    width: 8px;
    margin: 8px 2px;
}
QScrollBar::handle:vertical {
    background: @accent55;
    border-radius: 4px;
    min-height: 32px;
}
QScrollBar::handle:vertical:hover { background: @accent; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }
QScrollBar:horizontal {
    background: transparent;
    height: 8px;
    margin: 2px 8px;
}
QScrollBar::handle:horizontal {
    background: @accent55;
    border-radius: 4px;
    min-width: 32px;
}
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: none; }

QSplitter::handle:vertical {
    background: @accent28;
    height: 6px;
    margin: 0 18px;
    border-radius: 3px;
}
QSplitter::handle:vertical:hover { background: @accent; }

QStatusBar {
    background-color: transparent;
    color: @faint;
    border: none;
}
QMenuBar {
    background-color: transparent;
    color: @muted;
    padding: 2px 8px;
}
QMenuBar::item { padding: 6px 12px; border-radius: 8px; background: transparent; }
QMenuBar::item:selected { background-color: @hover; color: @text; }
QMenu {
    background-color: @sidebar;
    border: 1px solid @accent40;
    border-radius: 12px;
    padding: 6px;
}
QMenu::item { padding: 7px 22px; border-radius: 8px; }
QMenu::item:selected { background-color: @hover; color: @text; }

QTabWidget::pane { border: 1px solid @border; border-radius: 12px; }
QTabBar::tab {
    background: transparent;
    color: @muted;
    padding: 8px 14px;
    margin-right: 4px;
    border-radius: 8px;
}
QTabBar::tab:selected { background: @hover; color: @text; }
)"));
}

QString messageViewCss(int bodyFontSize)
{
    QString css = expand(QStringLiteral(R"(
body {
    background-color: transparent;
    color: @text;
    /* Segoe UI Emoji is last so ordinary letters keep the serif face while
       emoji still come out in colour instead of empty boxes. */
    font-family: "Lora", Georgia, serif, "Segoe UI Emoji";
    font-size: @bodySizepx;
}

/* One message block. The left cell holds the avatar. */
table.row { margin: 0 0 2px 0; }
td.ava { padding-top: 2px; }

.hdr { margin-bottom: 1px; }
.author {
    color: @lightGray;
    font-family: "Poppins", "Segoe UI", sans-serif;
    font-weight: 600;
    font-size: 13px;
}
.author-self { color: @accent; }

/* The name is a link so it can open a profile, but it must not look like a
   web link. */
a.namelink { color: @lightGray; text-decoration: none; }

.stamp, .meta {
    color: @faint;
    font-family: "Poppins", "Segoe UI", sans-serif;
    font-size: 11px;
}

/* The strip left of a grouped message. */
/* The small time sits level with the first line of text beside it. */
td.gut { padding-right: 6px; padding-top: 4px; }
.gutter {
    color: @faint;
    font-family: "Poppins", "Segoe UI", sans-serif;
    font-size: 10px;
}

.body { color: @text; }
.deleted { color: @deleted; }

.tag-deleted {
    color: @deleted;
    font-family: "Poppins", "Segoe UI", sans-serif;
    font-size: 10px;
}
.tag-edited {
    color: @faint;
    font-family: "Poppins", "Segoe UI", sans-serif;
    font-size: 10px;
}

.attach { margin: 5px 0 2px 0; }

/* Link previews. The coloured strip is a table cell, because Qt's rich text
   has no left border on a div. */
table.embed { margin: 6px 0 4px 0; }
.embed-inner {
    background-color: @input220;
    padding: 8px 12px;
}
/* A forwarded message: a thin bar on the left and the original inside. */
table.fwd { margin: 4px 0 4px 0; }
.fwd-inner { padding: 2px 0 2px 12px; }
.fwd-tag {
    color: @faint;
    font-family: "Poppins", "Segoe UI", sans-serif;
    font-size: 12px;
    margin-bottom: 2px;
}
.fwd-src {
    color: @faint;
    font-family: "Poppins", "Segoe UI", sans-serif;
    font-size: 11px;
    margin-top: 4px;
}
a.fwd-link { color: @faint; text-decoration: none; }

.embed-provider {
    color: @faint;
    font-family: "Poppins", "Segoe UI", sans-serif;
    font-size: 10px;
}
.embed-author {
    color: @lightGray;
    font-family: "Poppins", "Segoe UI", sans-serif;
    font-size: 11px;
}
.embed-title {
    font-family: "Poppins", "Segoe UI", sans-serif;
    font-size: 13px;
    font-weight: 600;
}
.embed-body {
    color: @muted;
    font-size: 12px;
}
.file {
    margin: 4px 0 2px 0;
    font-family: "Poppins", "Segoe UI", sans-serif;
    font-size: 12px;
}

.mention, a.mention {
    color: @accent;
    background-color: @accent40;
    font-family: "Poppins", "Segoe UI", sans-serif;
    font-weight: 600;
    text-decoration: none;
}
a.cmpbtn {
    font-family: "Poppins", "Segoe UI", sans-serif;
    font-size: 13px;
    font-weight: 600;
    text-decoration: none;
    padding: 4px 12px;
}

/* Underlined, not just tinted. In a grey scheme a colour alone is not enough
   to mark a link apart from the words around it. */
a { color: @accent; text-decoration: underline; }
code {
    background-color: @input;
    font-family: Consolas, monospace;
    font-size: 13px;
}
.system {
    color: @faint;
    font-style: italic;
}
.reactions {
    margin-top: 4px;
    font-size: 11px;
}
)"));

    // Names, times, embeds and the rest grow with the message text, keeping
    // the proportions they have at the usual 14 px, so a larger text size
    // does not leave small grey labels next to big words.
    const double scale = qBound(12, bodyFontSize, 24) / 14.0;
    static const QRegularExpression fixedSize(QStringLiteral("font-size: (\\d+)px"));
    QString scaled;
    int last = 0;
    for (auto it = fixedSize.globalMatch(css); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        scaled += css.mid(last, match.capturedStart() - last);
        scaled += QStringLiteral("font-size: %1px").arg(qRound(match.captured(1).toInt() * scale));
        last = int(match.capturedEnd());
    }
    scaled += css.mid(last);
    css = scaled;

    css.replace(QStringLiteral("@bodySize"), QString::number(bodyFontSize));
    return css;
}

} // namespace Theme
