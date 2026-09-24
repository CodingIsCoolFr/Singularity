#pragma once

// Housekeeping for an updated copy, run once a little after start.
//
// Updates install beside each other, one folder per version under versions\.
// Two things went wrong with that:
//
//   - The installer rewrites the Start menu and desktop shortcuts, but not a
//     shortcut pinned to the taskbar. A pin made weeks ago still opened that
//     week's version, so quitting and clicking the taskbar icon went back in
//     time.
//   - Every version was kept forever. Thirty-seven of them, by 0.6.94.
//
// So this points every Singularity shortcut - Start menu, desktop, taskbar
// and Start pins - at the running copy, and then removes old copies, keeping
// this one and the two before it. A copy that is still open, or that any
// shortcut still points at, is left alone.
//
// Does nothing when not running from a versions\<n> folder, and nothing off
// Windows.
#include <QString>
#include <QStringList>

namespace InstallTidy {
void run();

// The same work for a given copy and given shortcut folders, so it can be
// tested on a throwaway install without touching the real one. Waits for the
// removal to finish.
void runWith(const QString &exePath, const QStringList &shortcutFolders);
}
