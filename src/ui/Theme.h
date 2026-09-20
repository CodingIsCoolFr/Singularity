#pragma once

#include <QString>

class QWidget;

// Tokyo Night: deep blue-black surfaces and desaturated accents.
//
// Three rules shape everything here, and they are the reason the numbers look
// the way they do rather than being rounder:
//
//   - Nothing is pure black. White text on #000 bleeds at its edges, an effect
//     called halation, and it is tiring to read. The darkest surface below is
//     still a colour.
//   - Depth is built from layers, not from shadows. Each surface is a real
//     step lighter than the one behind it, so the rail, the sidebar and the
//     conversation read as separate planes without a single border.
//   - Accents are muted. A fully saturated colour on a dark ground appears to
//     vibrate, so every accent here is pulled back.
//
// Every colour in the app comes from this file, so a future light theme only
// has to change one place.
namespace Theme {

// Palette.
constexpr auto Dark = "#13141c";       // the ground everything sits on
constexpr auto Light = "#c0caf5";      // text, not white
constexpr auto MidGray = "#a9b1d6";    // secondary text
constexpr auto LightGray = "#d9def0";  // text that sits on an accent
constexpr auto Accent = "#7aa2f7";     // selection, links, anything active
constexpr auto Blue = "#7dcfff";       // a cooler accent, for links in prose
constexpr auto Green = "#9ece6a";      // online, and the speaking ring
constexpr auto Yellow = "#e0af68";     // idle, and warnings
constexpr auto Red = "#f7768e";        // busy, errors, destructive buttons

// Surfaces, darkest first. Furthest back to closest, the way Discord stacks
// them: the rail sits deepest, the conversation nearest.
constexpr auto SurfaceRail = "#13141c";
constexpr auto SurfaceSidebar = "#16171f";
constexpr auto SurfaceChat = "#1a1b26";
constexpr auto SurfaceInput = "#20222f";   // the composer, lifted off the chat
constexpr auto SurfaceHover = "#292e42";   // the only surface that moves
constexpr auto Border = "#262838";

constexpr auto TextPrimary = "#c0caf5";
constexpr auto TextMuted = "#a9b1d6";
constexpr auto TextFaint = "#6e769e";
constexpr auto Deleted = "#f7768e";

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
