#pragma once

#include <QString>

#include <functional>

class QWidget;

// The question-and-answer part of updating, kept away from any one window.
//
// It used to live in MainWindow, which meant the check only ever ran after a
// successful sign-in. A log line made the hole obvious:
//
//     [app] no saved token, showing sign-in
//
// The main window is not built on that path, so nothing checked. Somebody
// sitting at the sign-in screen was never offered an update, including an
// update that fixed signing in.
//
// So it lives here instead, and startup asks for it directly.
namespace UpdateFlow {

// `quiet` says nothing when already up to date, which is what a check on
// startup should do. A check somebody asked for reports either answer.
//
// `parent` may be null, because at startup there may be no window yet.
void run(bool quiet, QWidget *parent);

// Looks once the window is up, then again every couple of minutes. An update
// published while the program is open still offers itself. Already being
// current stays silent.
void watch();

// True from the moment somebody presses Update until the update either fails
// or finishes. The sign-in window uses this: the updater hides every window to
// show its install screen, and hiding the sign-in window ends it exactly as
// if the person had closed it - so the program took that for "cancelled" and
// exited halfway through the download.
bool updating();

// Waits, with the event loop running, until the update fails (the windows
// come back) or finishes (the program quits so the new copy can start).
void waitForUpdate();

// Updates the way Discord does. The quiet check installs a new version in
// the background, beside the running one, without starting it, and leaves
// this file in its folder once it is complete. The next launch - of any older
// copy, from any shortcut or pin - finds it and opens that instead.
inline constexpr const char *ReadyMarker = "ready-to-open";

// Told when a background update has finished installing, with its version.
void setStagedHandler(std::function<void(const QString &version)> handler);

// At startup: a newer complete version beside this one, if there is one. The
// caller starts it and exits. Empty when this copy is the newest.
QString newerInstalledCopy(const QString &myVersion);

} // namespace UpdateFlow
