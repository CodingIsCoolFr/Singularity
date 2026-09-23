#pragma once

// Finds out what the window's thread is doing when it freezes.
//
// The stall gauge could say how long the window was blocked, but not by what,
// and a log with a five-second gap in it and nothing to explain the gap is
// where guessing starts. This watches the window's thread from a thread of its
// own. When the window stops answering for more than 0.4 s, it pauses that
// thread for a moment, copies where it is - the instruction it is on and the
// return addresses on its stack - lets it go, and does that again every
// quarter second until the freeze ends. Then it writes the call path that
// kept turning up to the log, as named functions where the names are known and
// as module+offset where they are not (offsets in Singularity.exe resolve
// against the PDB kept for that version).
//
// It is the same idea as the hang reporter Discord's client runs. While the
// window's thread is paused nothing here allocates memory or takes a lock:
// that thread may be holding the heap's lock, and waiting on it would turn a
// freeze into a deadlock.
namespace HangWatch {

// Call once, on the window's thread, after the event loop objects exist.
void start();

// Stops the watcher thread. Safe to call if it never started.
void stop();

} // namespace HangWatch
