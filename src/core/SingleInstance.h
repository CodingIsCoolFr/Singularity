#pragma once

#include <QString>

#include <functional>

// One Singularity at a time, the way Discord behaves.
//
// Opening the program again used to start a second full copy: two sign-ins to
// Discord, two voice engines, and - once one of them was an old version left
// hidden by an interrupted update - an "Update available" box from a copy the
// person could not even see, while the copy they were using said it was up to
// date.
//
// Now a starting copy looks for a running one over a local socket:
//  - an ordinary second launch asks the running copy to come to the front,
//    then exits;
//  - a newer version (what the updater starts), or a launch with --replace
//    (an account switch restarting), asks the running copy to quit and waits
//    for it to go before carrying on.
namespace SingleInstance {

// True if this copy should carry on. False if it handed over to the copy
// already running and should exit now.
bool claim(const QString &version, bool replaceRunning);

// What to do when a later launch asks this copy to show itself.
void setShowHandler(std::function<void()> handler);

// True once another copy has asked this one to quit, so startup code that is
// still running (the sign-in window's wait) knows not to carry on.
bool replaced();

} // namespace SingleInstance
