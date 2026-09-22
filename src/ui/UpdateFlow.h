#pragma once

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

} // namespace UpdateFlow
