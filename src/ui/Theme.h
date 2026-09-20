#pragma once

#include <QColor>
#include <QString>
#include <QVector3D>

class QWidget;

// Night ink with one seed colour as the decorative note.
//
// The animated black hole is the true ground. These values are the glass that
// sits on it: cool, slightly tinted toward the seed, never pure black or pure
// white. Status dots stay green / yellow / red because those still mean
// something.
//
// applySeed() rewrites the live #RRGGBB buffers so every QColor(Theme::Accent)
// and every stylesheet token picks up the new colour without a rebuild.
namespace Theme {

extern char Dark[8];
extern char Light[8];
extern char MidGray[8];
extern char LightGray[8];

// The singularity: selected pills, mentions, links, primary buttons.
extern char Accent[8];
extern char AccentHover[8];

// Secondary colour: connecting, badges, moonlight on idle chrome.
extern char Highlight[8];

extern char Green[8];
extern char Yellow[8];
extern char Red[8];

extern char SurfaceRail[8];
extern char SurfaceSidebar[8];
extern char SurfaceChat[8];
extern char SurfaceInput[8];
extern char SurfaceHover[8];
extern char Border[8];

extern char TextPrimary[8];
extern char TextMuted[8];
extern char TextFaint[8];
extern char Deleted[8];

struct Preset {
    const char *name;
    const char *hex;
};

void applySeed(const QColor &seed);
QColor seedColor();
QVector3D holeAccent();
QVector3D holeDisk();
QVector3D holeGrade();
const Preset *presets(int *count);

// Qt stylesheet for the whole application.
QString applicationStyleSheet();

// CSS used inside the message view, which is a rich text document.
QString messageViewCss(int bodyFontSize = 14);

// Paints the window's title bar to match the app, so the top edge is one
// surface instead of two. Windows 11 only; older versions are left alone.
void applyDarkTitleBar(QWidget *window);

// Does the above for every window this application opens, including ones added
// later, so nobody has to remember to call it.
void installDarkTitleBars();

} // namespace Theme
