#include "ui/Theme.h"

#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QHash>
#include <QWidget>

#ifdef Q_OS_WIN
#include <dwmapi.h>
#include <windows.h>
#endif

namespace Theme {

namespace {

// Colours are written as @name in the sheets below and swapped in here. That
// beats a long chain of numbered placeholders, which is easy to miscount.
QString expand(QString sheet)
{
    static const QHash<QString, QString> tokens{
        {QStringLiteral("@dark"), QLatin1String(Dark)},
        {QStringLiteral("@light"), QLatin1String(Light)},
        {QStringLiteral("@midGray"), QLatin1String(MidGray)},
        {QStringLiteral("@lightGray"), QLatin1String(LightGray)},
        {QStringLiteral("@accent"), QLatin1String(Accent)},
        {QStringLiteral("@highlight"), QLatin1String(Highlight)},
        {QStringLiteral("@green"), QLatin1String(Green)},
        {QStringLiteral("@yellow"), QLatin1String(Yellow)},
        {QStringLiteral("@red"), QLatin1String(Red)},
        {QStringLiteral("@rail"), QLatin1String(SurfaceRail)},
        {QStringLiteral("@sidebar"), QLatin1String(SurfaceSidebar)},
        {QStringLiteral("@chat"), QLatin1String(SurfaceChat)},
        {QStringLiteral("@input"), QLatin1String(SurfaceInput)},
        {QStringLiteral("@hover"), QLatin1String(SurfaceHover)},
        {QStringLiteral("@border"), QLatin1String(Border)},
        {QStringLiteral("@text"), QLatin1String(TextPrimary)},
        {QStringLiteral("@muted"), QLatin1String(TextMuted)},
        {QStringLiteral("@faint"), QLatin1String(TextFaint)},
        {QStringLiteral("@deleted"), QLatin1String(Deleted)},
    };

    // Longest names first, so @lightGray is not eaten by @light.
    QStringList names = tokens.keys();
    std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) {
        return a.size() > b.size();
    });
    for (const QString &name : names)
        sheet.replace(name, tokens.value(name));

    return sheet;
}

// Catches every window as it is first shown.
//
// Doing this centrally means a dialog added later is styled without anyone
// having to remember, which is the sort of thing that is always remembered for
// the main window and forgotten for the fifth dialog.
class TitleBarPainter : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Show) {
            auto *widget = qobject_cast<QWidget *>(watched);
            if (widget && widget->isWindow())
                applyDarkTitleBar(widget);
        }
        return QObject::eventFilter(watched, event);
    }
};

} // namespace

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
    background-color: @chat;
    color: @text;
    font-family: "Poppins", "Segoe UI", Arial, sans-serif, "Segoe UI Emoji";
    font-size: 13px;
}
QMainWindow, QDialog { background-color: @chat; }

/* ---- server rail ---------------------------------------------------- */
#GuildRail {
    background-color: @rail;
    border-right: 1px solid @border;
}
#GuildRail QListWidget {
    background-color: transparent;
    border: none;
    outline: none;
}
/* Rows here are drawn by GuildRailDelegate, so no item rules belong in this
   sheet. Anything set below would fight the painter. */

/* ---- channel sidebar ------------------------------------------------ */
#Sidebar {
    background-color: @sidebar;
    border-right: 1px solid @border;
}
#Sidebar QListWidget {
    background-color: transparent;
    border: none;
    outline: none;
}
/* Channel rows are drawn by ChannelDelegate, for the same reason. */

#SidebarHeader {
    color: @text;
    font-size: 15px;
    font-weight: 600;
    padding: 17px 14px 12px 14px;
    border-bottom: 1px solid @border;
}

/* ---- who you are ---------------------------------------------------- */
#UserPanel {
    background-color: @rail;
    border-top: 1px solid @border;
}
#SelfName   { color: @text; font-size: 13px; font-weight: 600; background: transparent; }
#SelfStatus { color: @faint; font-size: 11px; background: transparent; }

/* Shown only while you are sitting in a voice channel. */
#VoicePanel {
    background-color: @rail;
    border-top: 1px solid @border;
}
#VoiceHeading { color: @green; font-size: 12px; font-weight: 700; background: transparent; }
#VoiceChannel { color: @muted; font-size: 11px; background: transparent; }

/* These three share a 240 pixel sidebar, so they cannot carry the roomy
   padding the rest of the app uses or the words get cut in half. */
#VoicePanel QPushButton {
    padding: 3px 4px;
    font-size: 11px;
    border-radius: 6px;
}
#VoicePanel QPushButton:checked {
    background-color: @accent;
    color: @dark;
    border-color: @accent;
    font-weight: 600;
}

/* ---- chat ----------------------------------------------------------- */
#ChatHeader {
    background-color: @chat;
    border-bottom: 1px solid @border;
}
#ChannelTitle { color: @text; font-size: 15px; font-weight: 600; }
#ChannelTopic { color: @faint; font-size: 12px; }

QTextBrowser {
    background-color: @chat;
    border: none;
    padding: 14px 20px;
}

#Composer { background-color: @chat; }
#ComposerBox {
    background-color: @input;
    border: 1px solid @border;
    border-radius: 10px;
}
#ComposerBox:focus-within { border-color: @accent; }
QTextEdit#MessageInput {
    background-color: transparent;
    border: none;
    padding: 11px 14px;
    color: @text;
    font-family: "Lora", Georgia, serif;
    font-size: 14px;
}

#TypingLabel {
    color: @faint;
    font-size: 11px;
    font-style: italic;
    padding: 1px 22px;
    background-color: @chat;
}

/* ---- controls ------------------------------------------------------- */
QPushButton {
    background-color: @input;
    color: @text;
    border: 1px solid @border;
    border-radius: 8px;
    padding: 8px 16px;
}
QPushButton:hover   { background-color: @hover; border-color: @accent; }
QPushButton:pressed { background-color: @rail; }
QPushButton#PrimaryButton {
    background-color: @accent;
    color: @dark;
    border: none;
    font-weight: 600;
}
QPushButton#PrimaryButton:hover { background-color: #e08b6d; }

QLineEdit {
    background-color: @input;
    border: 1px solid @border;
    border-radius: 8px;
    padding: 9px 12px;
    color: @text;
    selection-background-color: @accent;
}
QLineEdit:focus { border-color: @accent; }

QComboBox {
    background-color: @input;
    border: 1px solid @border;
    border-radius: 8px;
    padding: 6px 10px;
}

/* Sliders. Without these Windows paints its own, in the system accent, which
   is how a blue bar ended up in the middle of a grey window. */
QSlider::groove:horizontal {
    height: 4px;
    background: @input;
    border-radius: 2px;
}
QSlider::sub-page:horizontal {
    background: @muted;
    border-radius: 2px;
}
QSlider::handle:horizontal {
    width: 14px;
    height: 14px;
    margin: -5px 0;
    border-radius: 7px;
    background: @accent;
}
QSlider::handle:horizontal:hover { background: @text; }
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

/* Check boxes, for the same reason. */
QCheckBox { spacing: 8px; }
QCheckBox::indicator {
    width: 16px;
    height: 16px;
    border: 1px solid @border;
    border-radius: 5px;
    background: @input;
}
QCheckBox::indicator:hover { border-color: @muted; }
QCheckBox::indicator:checked {
    background: @accent;
    border-color: @accent;
}
QComboBox QAbstractItemView {
    background-color: @sidebar;
    border: 1px solid @border;
    selection-background-color: @hover;
}

QListWidget, QTableWidget {
    background-color: @sidebar;
    border: 1px solid @border;
    border-radius: 10px;
    outline: none;
}
QHeaderView::section {
    background-color: @input;
    color: @muted;
    border: none;
    padding: 6px;
}

QCheckBox { spacing: 8px; }
QCheckBox::indicator {
    width: 16px; height: 16px;
    border-radius: 4px;
    border: 1px solid @midGray;
    background-color: @input;
}
QCheckBox::indicator:checked { background-color: @accent; border-color: @accent; }

QScrollBar:vertical {
    background: transparent;
    width: 10px;
    margin: 4px 2px;
}
QScrollBar::handle:vertical {
    background: @border;
    border-radius: 5px;
    min-height: 30px;
}
QScrollBar::handle:vertical:hover { background: @midGray; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }

QStatusBar {
    background-color: @rail;
    color: @faint;
    border-top: 1px solid @border;
}
QMenuBar { background-color: @rail; color: @muted; }
QMenuBar::item:selected { background-color: @hover; color: @text; }
QMenu { background-color: @sidebar; border: 1px solid @border; padding: 4px; }
QMenu::item { padding: 6px 22px; border-radius: 6px; }
QMenu::item:selected { background-color: @hover; }

QTabWidget::pane { border: 1px solid @border; border-radius: 10px; }
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
    return expand(QStringLiteral(R"(
body {
    background-color: @chat;
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
td.gut { padding-right: 6px; }
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
    background-color: @input;
    padding: 8px 12px;
}
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

.mention {
    color: @accent;
    font-family: "Poppins", "Segoe UI", sans-serif;
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
)")
                      .replace(QStringLiteral("@bodySize"), QString::number(qBound(11, bodyFontSize, 20))));
}

} // namespace Theme
