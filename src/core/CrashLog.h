#pragma once

// The last words of a program that is about to stop.
//
// Without this a crash left nothing in the log: the next start truncated it,
// and a C++ exception nobody catches ends in abort() with no line saying
// which one. install() hooks both ways out - terminate() and Windows' last
// chance fault filter - so the reason is written and flushed first. The
// previous run's log is kept as singularity.prev.log (see Logger).
//
// Windows' own handling still follows: the crash dump and the report.
namespace CrashLog {
void install();
}
