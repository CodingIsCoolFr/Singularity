#include "core/CrashLog.h"

#include "core/Logger.h"

#include <QFileInfo>
#include <QString>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <typeinfo>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

void logUncaughtAndAbort()
{
    // Reached from a catch block's rethrow, current_exception() names it.
    // Reached from the fault filter below, it is empty: the filter has
    // already written what the exception was.
    QString what = QStringLiteral("(named above)");
    if (const std::exception_ptr current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        } catch (const std::exception &error) {
            what = QStringLiteral("%1: %2").arg(QString::fromLatin1(typeid(error).name()),
                                                QString::fromLocal8Bit(error.what()));
        } catch (...) {
            what = QStringLiteral("an exception that is not a std::exception");
        }
    }
    wlog(QStringLiteral("crash"), QStringLiteral("stopping on an uncaught exception %1").arg(what));
    Logger::instance().flush();
    std::abort();
}

// abort() from anywhere: qFatal, a failed check in a library, or terminate()
// on a thread other than the window's. The terminate handler above is kept
// per thread by the Microsoft runtime, so a worker thread that lets an
// exception escape never reaches it; SIGABRT's handler is shared by all.
void logAbort(int)
{
    wlog(QStringLiteral("crash"), QStringLiteral("stopping: abort() was called on thread %1")
                                      .arg(GetCurrentThreadId()));
    Logger::instance().flush();
}

LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;

// Names a C++ exception straight from its Windows exception record.
//
// An uncaught C++ exception reaches the fault filter before terminate(), and
// std::current_exception() only knows exceptions a catch block is handling.
// The record does know. On x64 it carries the thrown object and the
// compiler's description of its type (a ThrowInfo, as image-relative
// offsets), listing every type it can be caught as. If one of those is
// std::exception, the object's what() is safe to ask.
//
// No C++ objects with destructors here, so a bad record can be caught with
// __try.
struct Pmd { int mdisp, pdisp, vdisp; };
struct CatchableType { unsigned properties; int pType; Pmd thisDisplacement; int size; int copy; };
struct CatchableTypeArray { int count; int types[1]; };
struct ThrowInfo { unsigned attributes; int unwind; int forwardCompat; int catchableTypes; };
struct TypeDescriptor { const void *vtable; void *spare; char name[1]; };

bool describeCxxException(const EXCEPTION_RECORD *record, char *type, size_t typeSize, char *what,
                          size_t whatSize)
{
    if (!record || record->ExceptionCode != 0xE06D7363 || record->NumberParameters < 4)
        return false;
    __try {
        const auto base = static_cast<ULONG_PTR>(record->ExceptionInformation[3]);
        const auto *object = reinterpret_cast<const char *>(record->ExceptionInformation[1]);
        const auto *info = reinterpret_cast<const ThrowInfo *>(record->ExceptionInformation[2]);
        const auto *list = reinterpret_cast<const CatchableTypeArray *>(base + info->catchableTypes);
        for (int i = 0; i < list->count; ++i) {
            const auto *catchable = reinterpret_cast<const CatchableType *>(base + list->types[i]);
            const auto *descriptor = reinterpret_cast<const TypeDescriptor *>(base + catchable->pType);
            if (i == 0)
                strncpy_s(type, typeSize, descriptor->name, _TRUNCATE);
            if (std::strcmp(descriptor->name, ".?AVexception@std@@") == 0) {
                const auto *asException =
                    reinterpret_cast<const std::exception *>(object + catchable->thisDisplacement.mdisp);
                strncpy_s(what, whatSize, asException->what(), _TRUNCATE);
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Any fatal fault on any thread: what and where, then Windows' usual handling.
LONG WINAPI logFatalFault(EXCEPTION_POINTERS *info)
{
    const auto *record = info ? info->ExceptionRecord : nullptr;
    HMODULE module = nullptr;
    wchar_t name[MAX_PATH] = L"?";
    if (record && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                         | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     static_cast<LPCWSTR>(record->ExceptionAddress), &module))
        GetModuleFileNameW(module, name, MAX_PATH);
    wlog(QStringLiteral("crash"),
         QStringLiteral("fatal fault 0x%1 at %2+0x%3 on thread %4")
             .arg(record ? quint32(record->ExceptionCode) : 0u, 8, 16, QLatin1Char('0'))
             .arg(QFileInfo(QString::fromWCharArray(name)).fileName())
             .arg(record && module ? quintptr(record->ExceptionAddress) - quintptr(module) : 0, 0, 16)
             .arg(GetCurrentThreadId()));

    char type[256] = "";
    char what[512] = "";
    if (describeCxxException(record, type, sizeof(type), what, sizeof(what)))
        wlog(QStringLiteral("crash"), QStringLiteral("it was an uncaught C++ exception %1%2")
                                          .arg(QString::fromLatin1(type),
                                               what[0] ? QStringLiteral(": ") + QString::fromLocal8Bit(what)
                                                       : QString()));
    Logger::instance().flush();
    return g_previousFilter ? g_previousFilter(info) : EXCEPTION_CONTINUE_SEARCH;
}

} // namespace
#endif

void CrashLog::install()
{
#ifdef Q_OS_WIN
    std::set_terminate(logUncaughtAndAbort);
    std::signal(SIGABRT, logAbort);
    g_previousFilter = SetUnhandledExceptionFilter(logFatalFault);
#endif
}
