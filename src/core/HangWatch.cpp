#include "core/HangWatch.h"

#include "core/Logger.h"

#include <QCoreApplication>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// These need windows.h first.
#include <dbghelp.h>
#include <psapi.h>
#endif

namespace HangWatch {

#ifdef Q_OS_WIN
namespace {

constexpr qint64 StallMs = 400;      // a freeze worth looking at
constexpr qint64 SampleEveryMs = 250;
constexpr int MaxSamples = 40;
constexpr int FramesPerSample = 24;
constexpr size_t StackCopyBytes = 256 * 1024;

qint64 nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::atomic<qint64> g_lastBeat{0};
std::atomic<bool> g_running{false};
std::thread g_watcher;
HANDLE g_gui = nullptr;
uintptr_t g_stackBase = 0;
uintptr_t g_stackLimit = 0;
QTimer *g_beat = nullptr;

// The copy of the paused thread's stack. Reserved up front so nothing has to
// be allocated while that thread is stopped.
alignas(16) unsigned char g_stackCopy[StackCopyBytes];

struct Range
{
    uintptr_t start = 0;
    uintptr_t end = 0;
    uintptr_t moduleBase = 0;
    QString module;
};

// Executable sections of every loaded module. Built and refreshed only while
// the window's thread is running.
std::vector<Range> g_code;

void refreshModules()
{
    g_code.clear();
    HMODULE modules[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
        return;
    const DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), 1024);
    for (DWORD i = 0; i < count; ++i) {
        const auto base = reinterpret_cast<uintptr_t>(modules[i]);
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(modules[i], path, MAX_PATH);
        QString name = QString::fromWCharArray(path);
        name = name.mid(name.lastIndexOf(QLatin1Char('\\')) + 1);

        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            continue;
        const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
        const IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
        for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++section) {
            if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE))
                continue;
            Range range;
            range.start = base + section->VirtualAddress;
            range.end = range.start + std::max<DWORD>(section->Misc.VirtualSize, section->SizeOfRawData);
            range.moduleBase = base;
            range.module = name;
            g_code.push_back(range);
        }
    }
}

const Range *codeRangeFor(uintptr_t address)
{
    for (const Range &range : g_code) {
        if (address >= range.start && address < range.end)
            return &range;
    }
    return nullptr;
}

// A value on the stack is taken for a return address only if it points into
// code just after a CALL instruction. That filters out the pointers to code
// that are merely sitting in local variables.
bool followsCall(uintptr_t address, const Range &range)
{
    if (address < range.start + 7)
        return false;
    const auto *p = reinterpret_cast<const unsigned char *>(address);
    if (p[-5] == 0xE8)                                           // call rel32
        return true;
    if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10)                 // call [rip+disp32] and friends
        return true;
    if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10)                 // call reg / call [reg]
        return true;
    if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10)                 // call [reg+disp8]
        return true;
    if (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10)                 // call [reg+disp32] with SIB
        return true;
    if (p[-3] == 0x41 && p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) // call r8-r15
        return true;
    return false;
}

bool g_symbolsReady = false;

QString describe(uintptr_t address)
{
    const Range *range = codeRangeFor(address);
    if (!range)
        return QStringLiteral("0x%1").arg(quint64(address), 0, 16);

    if (g_symbolsReady) {
        alignas(SYMBOL_INFOW) char buffer[sizeof(SYMBOL_INFOW) + 256 * sizeof(wchar_t)] = {};
        auto *symbol = reinterpret_cast<SYMBOL_INFOW *>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        if (SymFromAddrW(GetCurrentProcess(), address, &displacement, symbol)) {
            return QStringLiteral("%1!%2+0x%3")
                .arg(range->module, QString::fromWCharArray(symbol->Name))
                .arg(quint64(displacement), 0, 16);
        }
    }
    return QStringLiteral("%1+0x%2").arg(range->module).arg(quint64(address - range->moduleBase), 0, 16);
}

struct Sample
{
    uintptr_t frames[FramesPerSample] = {};
    int count = 0;
};

// Pauses the window's thread, copies its registers and stack, resumes it.
// Between SuspendThread and ResumeThread: no allocation, no locks, no logging.
bool capture(Sample &sample, uintptr_t &rspOut, size_t &copiedOut)
{
    if (SuspendThread(g_gui) == static_cast<DWORD>(-1))
        return false;

    CONTEXT context;
    ZeroMemory(&context, sizeof(context));
    context.ContextFlags = CONTEXT_CONTROL;
    const bool got = GetThreadContext(g_gui, &context) != 0;

    size_t copied = 0;
    uintptr_t rsp = 0;
    if (got) {
        rsp = static_cast<uintptr_t>(context.Rsp);
        if (rsp >= g_stackLimit && rsp < g_stackBase) {
            copied = std::min<size_t>(g_stackBase - rsp, StackCopyBytes);
            memcpy(g_stackCopy, reinterpret_cast<const void *>(rsp), copied);
        }
    }

    ResumeThread(g_gui);

    if (!got)
        return false;
    sample.frames[0] = static_cast<uintptr_t>(context.Rip);
    sample.count = 1;
    rspOut = rsp;
    copiedOut = copied;
    return true;
}

void collectReturnAddresses(Sample &sample, size_t copied)
{
    for (size_t offset = 0; offset + sizeof(uintptr_t) <= copied && sample.count < FramesPerSample;
         offset += sizeof(uintptr_t)) {
        uintptr_t value = 0;
        memcpy(&value, g_stackCopy + offset, sizeof(value));
        const Range *range = codeRangeFor(value);
        if (range && followsCall(value, *range))
            sample.frames[sample.count++] = value;
    }
}

DWORD pageFaults()
{
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return 0;
    return counters.PageFaultCount;
}

DWORD g_faultsAtStart = 0;

// `faults` is counted when the freeze ends, before the symbol loading below,
// which faults in pages of its own.
void report(qint64 durationMs, const std::vector<Sample> &samples, DWORD faults)
{
    if (!g_symbolsReady) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS
                      | SYMOPT_NO_PROMPTS);
        // Only the program's own folder: never a symbol server, never the
        // network.
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir(exe);
        dir = dir.substr(0, dir.find_last_of(L'\\'));
        g_symbolsReady = SymInitializeW(GetCurrentProcess(), dir.c_str(), TRUE) != 0;
    }

    // Where the thread was each time, and which frames were on the stack in
    // most samples: the frames present throughout are the call that did not
    // return.
    QHash<QString, int> topCounts;
    QHash<QString, int> frameCounts;
    QStringList firstStack;
    for (size_t i = 0; i < samples.size(); ++i) {
        const Sample &sample = samples[i];
        topCounts[describe(sample.frames[0])] += 1;
        QSet<QString> seen;
        for (int f = 0; f < sample.count; ++f) {
            const QString name = describe(sample.frames[f]);
            if (i == 0)
                firstStack << name;
            if (!seen.contains(name)) {
                seen.insert(name);
                frameCounts[name] += 1;
            }
        }
    }

    QString top;
    int topHits = 0;
    for (auto it = topCounts.constBegin(); it != topCounts.constEnd(); ++it) {
        if (it.value() > topHits) {
            topHits = it.value();
            top = it.key();
        }
    }

    // Frames on the stack in at least half the samples, in stack order as they
    // appeared in the first sample.
    QStringList steady;
    const int half = std::max<int>(1, int(samples.size()) / 2);
    for (const QString &name : firstStack) {
        if (frameCounts.value(name) >= half && !steady.contains(name))
            steady << name;
    }

    wlog(QStringLiteral("hang"),
         QStringLiteral("window thread was stuck for %1 ms; %2 look(s) at it")
             .arg(durationMs)
             .arg(samples.size()));
    wlog(QStringLiteral("hang"), QStringLiteral("  most often at: %1 (%2 of %3)")
                                     .arg(top)
                                     .arg(topHits)
                                     .arg(samples.size()));
    wlog(QStringLiteral("hang"),
         QStringLiteral("  held throughout: %1").arg(steady.mid(0, 16).join(QStringLiteral("  <-  "))));
    wlog(QStringLiteral("hang"),
         QStringLiteral("  first look: %1").arg(firstStack.mid(0, 16).join(QStringLiteral("  <-  "))));

    // The machine as a whole. A freeze that is the program's own fault looks
    // the same whatever else is running; one caused by the computer running
    // out of memory shows up as the memory being nearly full and this program
    // having to fetch its own pages back from disk, which is slow enough to
    // stop anything.
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    GlobalMemoryStatusEx(&memory);
    wlog(QStringLiteral("hang"),
         QStringLiteral("  the computer then: memory %1% used, %2 MB free of %3 MB; this program took %4 page faults "
                        "during the freeze")
             .arg(memory.dwMemoryLoad)
             .arg(memory.ullAvailPhys / (1024 * 1024))
             .arg(memory.ullTotalPhys / (1024 * 1024))
             .arg(faults));
}

void watch()
{
    std::vector<Sample> samples;
    samples.reserve(MaxSamples);

    bool inStall = false;
    qint64 stallStart = 0;
    qint64 lastSample = 0;
    refreshModules();

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const qint64 now = nowMs();
        const qint64 beat = g_lastBeat.load();
        if (beat == 0)
            continue;
        const qint64 lag = now - beat;

        if (!inStall && lag > StallMs) {
            inStall = true;
            stallStart = beat;
            samples.clear();
            lastSample = 0;
            g_faultsAtStart = pageFaults();
        }

        if (inStall && lag > StallMs && now - lastSample >= SampleEveryMs
            && int(samples.size()) < MaxSamples) {
            Sample sample;
            uintptr_t rsp = 0;
            size_t copied = 0;
            if (capture(sample, rsp, copied)) {
                // The window's thread is running again from here on.
                if (!codeRangeFor(sample.frames[0]))
                    refreshModules();   // something new was loaded
                collectReturnAddresses(sample, copied);
                samples.push_back(sample);
            }
            lastSample = now;
        }

        if (inStall && lag < StallMs / 2) {
            inStall = false;
            if (!samples.empty())
                report(beat - stallStart, samples, pageFaults() - g_faultsAtStart);
        }
    }
}

} // namespace
#endif

void start()
{
#ifdef Q_OS_WIN
    if (g_running.load())
        return;

    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_gui,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0)) {
        wlog(QStringLiteral("hang"), QStringLiteral("could not watch the window thread"));
        return;
    }

    // This thread's own stack bounds, read from its thread information block.
    const auto *tib = reinterpret_cast<const NT_TIB *>(NtCurrentTeb());
    g_stackBase = reinterpret_cast<uintptr_t>(tib->StackBase);
    g_stackLimit = reinterpret_cast<uintptr_t>(tib->StackLimit);
    // The committed limit moves as the stack grows; reserve bound is safer.
    ULONG_PTR low = 0;
    ULONG_PTR high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    if (low)
        g_stackLimit = low;

    g_beat = new QTimer(QCoreApplication::instance());
    g_beat->setTimerType(Qt::PreciseTimer);
    g_beat->setInterval(100);
    QObject::connect(g_beat, &QTimer::timeout, []() { g_lastBeat.store(nowMs()); });
    g_beat->start();
    g_lastBeat.store(nowMs());

    g_running.store(true);
    g_watcher = std::thread(watch);
    // Whichever way the program ends - including an early return from main,
    // such as cancelling the sign-in window - the watcher is joined before
    // its std::thread is destroyed, which would otherwise end the process.
    qAddPostRoutine(&HangWatch::stop);
    wlog(QStringLiteral("hang"), QStringLiteral("watching the window thread for freezes"));
#endif
}

void stop()
{
#ifdef Q_OS_WIN
    if (!g_running.exchange(false))
        return;
    if (g_watcher.joinable())
        g_watcher.join();
    if (g_gui) {
        CloseHandle(g_gui);
        g_gui = nullptr;
    }
#endif
}

} // namespace HangWatch
