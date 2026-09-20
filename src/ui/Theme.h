#pragma once

#include <QString>

// The Claude look: warm near-black surfaces, a soft off-white for text, and
// the clay orange accent. Every colour in the app comes from here so a future
// light theme only has to change one file.
namespace Theme {

// Brand palette.
constexpr auto Dark = "#141413";
constexpr auto Light = "#faf9f5";
constexpr auto MidGray = "#b0aea5";
constexpr auto LightGray = "#e8e6dc";
constexpr auto Orange = "#d97757";
constexpr auto Blue = "#6a9bcc";
constexpr auto Green = "#788c5d";

// Surfaces derived from the palette, darkest first.
constexpr auto SurfaceRail = "#0f0f0e";
constexpr auto SurfaceSidebar = "#1a1a18";
constexpr auto SurfaceChat = "#141413";
constexpr auto SurfaceInput = "#232320";
constexpr auto SurfaceHover = "#2a2a26";
constexpr auto Border = "#2f2f2b";

constexpr auto TextPrimary = "#faf9f5";
constexpr auto TextMuted = "#b0aea5";
constexpr auto TextFaint = "#75736b";
constexpr auto Deleted = "#c76f57";

// Qt stylesheet for the whole application.
QString applicationStyleSheet();

// CSS used inside the message view, which is a rich text document.
QString messageViewCss(int bodyFontSize = 14);

} // namespace Theme
