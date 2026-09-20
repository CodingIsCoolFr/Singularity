#pragma once

#include <QString>

class QWidget;

// Black and grey, with colour kept for the few things that mean something.
//
// Every grey below is neutral: equal red, green and blue, so no surface leans
// warm or cool. Three rules shape the numbers, and they are the reason none of
// them is round:
//
//   - Nothing is pure black, and nothing is pure white. White text on #000
//     bleeds at its edges, an effect called halation, and it is tiring to
//     read. The darkest surface here is #0a0a0a and the brightest text is
//     #ededed.
//   - Depth is built from layers, not from shadows. Each surface is a real
//     step lighter than the one behind it, so the rail, the sidebar and the
//     conversation read as separate planes without needing a single border.
//   - Colour is information, never decoration. The only coloured things left
//     are the status dots and errors, because green, yellow and red are what
//     tell you somebody is online, away or busy. Making those grey would look
//     tidier and say less.
//
// Every colour in the app comes from this file, so a future light theme only
// has to change one place.
namespace Theme {

// Greys.
constexpr auto Dark = "#0a0a0a";       // the ground everything sits on
constexpr auto Light = "#ededed";      // text, not white
constexpr auto MidGray = "#a3a3a3";    // secondary text
constexpr auto LightGray = "#d4d4d4";  // text that sits on a bright fill

// The bright one: the selected pill, mentions, links, anything active.
constexpr auto Accent = "#ededed";

// The quiet one: embed edges, badges, and states that are passing through,
// like "connecting". Dim on purpose, so it never competes with Accent.
constexpr auto Highlight = "#8f8f8f";

// The only colour left, and only where it carries meaning.
constexpr auto Green = "#3ba55c";      // online, and the speaking ring
constexpr auto Yellow = "#d9a441";     // idle
constexpr auto Red = "#e05561";        // busy, errors, deletions

// Surfaces, darkest first. Furthest back to closest, the way Discord stacks
// them: the rail sits deepest, the conversation nearest.
constexpr auto SurfaceRail = "#0a0a0a";
constexpr auto SurfaceSidebar = "#101010";
constexpr auto SurfaceChat = "#161616";
constexpr auto SurfaceInput = "#1e1e1e";   // the composer, lifted off the chat
constexpr auto SurfaceHover = "#2a2a2a";   // the only surface that moves
constexpr auto Border = "#242424";

constexpr auto TextPrimary = "#ededed";
constexpr auto TextMuted = "#a3a3a3";
constexpr auto TextFaint = "#6b6b6b";
constexpr auto Deleted = "#e05561";

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
