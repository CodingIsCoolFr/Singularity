#include "core/ShareAudio.h"

#include "core/Logger.h"

#include <QFileInfo>
#include <QSet>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// These need windows.h first.
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <dwmapi.h>
#include <mmdeviceapi.h>
#endif

namespace {

constexpr int SampleRate = 48000;
constexpr int Channels = 2;
constexpr int BytesPerFrame = Channels * 2; // 16 bit

#ifdef Q_OS_WIN

// Windows hands the activated audio client back through a callback object
// rather than a return value. It may call from any thread, which is what
// IAgileObject promises it can.
class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler,
                                public IAgileObject
{
public:
    ActivationHandler() : done(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

    STDMETHODIMP QueryInterface(REFIID riid, void **out) override
    {
        if (!out)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *out = static_cast<IActivateAudioInterfaceCompletionHandler *>(this);
        } else if (riid == __uuidof(IAgileObject)) {
            *out = static_cast<IAgileObject *>(this);
        } else {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    STDMETHODIMP_(ULONG) AddRef() override { return ++refs; }

    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG left = --refs;
        if (left == 0)
            delete this;
        return left;
    }

    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation *operation) override
    {
        HRESULT activation = E_FAIL;
        IUnknown *unknown = nullptr;
        const HRESULT hr = operation->GetActivateResult(&activation, &unknown);
        result = FAILED(hr) ? hr : activation;
        if (SUCCEEDED(result) && unknown)
            unknown->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void **>(&client));
        if (unknown)
            unknown->Release();
        SetEvent(done);
        return S_OK;
    }

    std::atomic<ULONG> refs{1};
    HANDLE done = nullptr;
    HRESULT result = E_PENDING;
    IAudioClient *client = nullptr;

private:
    ~ActivationHandler()
    {
        if (client)
            client->Release();
        if (done)
            CloseHandle(done);
    }
};

QString hresultText(HRESULT hr)
{
    return QStringLiteral("0x%1").arg(quint32(hr), 8, 16, QLatin1Char('0'));
}

QString exeNameOf(DWORD processId)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process)
        return {};
    wchar_t path[MAX_PATH * 2];
    DWORD size = DWORD(std::size(path));
    QString name;
    if (QueryFullProcessImageNameW(process, 0, path, &size))
        name = QFileInfo(QString::fromWCharArray(path, int(size))).fileName();
    CloseHandle(process);
    return name;
}

// A Store app's window belongs to ApplicationFrameHost, but its sound comes
// from the app's own process, which owns a child of that window.
DWORD realOwnerOf(HWND window, DWORD hostId)
{
    struct Search {
        DWORD host;
        DWORD found;
    } search{hostId, 0};

    EnumChildWindows(
        window,
        [](HWND child, LPARAM param) -> BOOL {
            auto *s = reinterpret_cast<Search *>(param);
            DWORD pid = 0;
            GetWindowThreadProcessId(child, &pid);
            if (pid != 0 && pid != s->host) {
                s->found = pid;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));

    return search.found != 0 ? search.found : hostId;
}

#endif

} // namespace

ShareAudio::ShareAudio(QObject *parent)
    : QObject(parent)
{
}

ShareAudio::~ShareAudio()
{
    stop();
}

QList<ShareAudio::App> ShareAudio::apps()
{
    QList<App> found;
#ifdef Q_OS_WIN
    struct Collect {
        QList<App> *apps;
        QSet<quint32> seen;
        DWORD self;
    } collect{&found, {}, GetCurrentProcessId()};

    // Top of the stack first, so the window you were just in leads the list.
    EnumWindows(
        [](HWND window, LPARAM param) -> BOOL {
            auto *c = reinterpret_cast<Collect *>(param);

            if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr)
                return TRUE;
            if (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
                return TRUE;

            // Suspended Store apps and windows on other virtual desktops are
            // "visible" but cloaked. Nobody can see them, so nobody picks them.
            DWORD cloaked = 0;
            DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            if (cloaked)
                return TRUE;

            wchar_t title[256];
            const int length = GetWindowTextW(window, title, int(std::size(title)));
            if (length <= 0)
                return TRUE;

            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            if (pid == 0 || pid == c->self)
                return TRUE;

            QString exe = exeNameOf(pid);
            if (exe.compare(QLatin1String("ApplicationFrameHost.exe"), Qt::CaseInsensitive) == 0) {
                pid = realOwnerOf(window, pid);
                exe = exeNameOf(pid);
            }

            const QString name = QString::fromWCharArray(title, length);
            if (name == QLatin1String("Program Manager"))
                return TRUE;
            if (c->seen.contains(pid))
                return TRUE;
            c->seen.insert(pid);

            c->apps->append(App{pid, name, exe});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&collect));
#endif
    return found;
}

void ShareAudio::start(Source source, quint32 processId)
{
    stop();
    if (source == Source::Nothing)
        return;

    m_stop.store(false);
    m_thread = std::thread([this, source, processId]() { run(source, processId); });
}

void ShareAudio::stop()
{
    m_stop.store(true);
    if (m_thread.joinable())
        m_thread.join();
}

void ShareAudio::run(Source source, quint32 processId)
{
#ifdef Q_OS_WIN
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    const QString what = source == Source::OneApp
                             ? QStringLiteral("process %1 and its children").arg(processId)
                             : QStringLiteral("everything except us");

    auto fail = [&](const QString &step, HRESULT hr) {
        wlog(QStringLiteral("share"),
             QStringLiteral("screen sound (%1) failed at %2: %3").arg(what, step, hresultText(hr)));
        emit failed(QStringLiteral("Sound could not be shared. The picture still goes out."));
    };

    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    if (source == Source::OneApp) {
        params.ProcessLoopbackParams.TargetProcessId = processId;
        params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    } else {
        // Leaving ourselves out is what keeps the call from being sent back
        // to the people in it.
        params.ProcessLoopbackParams.TargetProcessId = GetCurrentProcessId();
        params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
    }

    PROPVARIANT activation{};
    activation.vt = VT_BLOB;
    activation.blob.cbSize = sizeof(params);
    activation.blob.pBlobData = reinterpret_cast<BYTE *>(&params);

    auto *handler = new ActivationHandler;
    IActivateAudioInterfaceAsyncOperation *operation = nullptr;
    IAudioClient *client = nullptr;
    IAudioCaptureClient *capture = nullptr;
    HANDLE ready = nullptr;
    qint64 capturedBytes = 0;

    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                             __uuidof(IAudioClient), &activation, handler,
                                             &operation);
    if (FAILED(hr)) {
        fail(QStringLiteral("activation"), hr);
        goto done;
    }

    if (WaitForSingleObject(handler->done, 5000) != WAIT_OBJECT_0) {
        fail(QStringLiteral("activation wait"), HRESULT_FROM_WIN32(WAIT_TIMEOUT));
        goto done;
    }
    if (FAILED(handler->result) || !handler->client) {
        fail(QStringLiteral("activation result"), FAILED(handler->result) ? handler->result : E_NOINTERFACE);
        goto done;
    }
    client = handler->client;
    client->AddRef();

    {
        // A process loopback client has no mix format of its own to ask for,
        // so it is told the one wanted and converts to it.
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = Channels;
        format.nSamplesPerSec = SampleRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = BytesPerFrame;
        format.nAvgBytesPerSec = SampleRate * BytesPerFrame;

        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                                    | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                200000, // 20 ms, in 100 ns units
                                0, &format, nullptr);
        if (FAILED(hr)) {
            fail(QStringLiteral("initialise"), hr);
            goto done;
        }
    }

    ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    hr = client->SetEventHandle(ready);
    if (FAILED(hr)) {
        fail(QStringLiteral("event"), hr);
        goto done;
    }

    hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void **>(&capture));
    if (FAILED(hr)) {
        fail(QStringLiteral("capture service"), hr);
        goto done;
    }

    hr = client->Start();
    if (FAILED(hr)) {
        fail(QStringLiteral("start"), hr);
        goto done;
    }

    wlog(QStringLiteral("share"), QStringLiteral("screen sound running: %1").arg(what));

    while (!m_stop.load()) {
        // A timeout rather than forever, so stop() is heard even while the
        // program being captured is silent and nothing arrives.
        if (WaitForSingleObject(ready, 100) != WAIT_OBJECT_0)
            continue;

        UINT32 packet = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE *data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr))
                break;

            QByteArray pcm;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                pcm = QByteArray(int(frames) * BytesPerFrame, '\0');
            else
                pcm = QByteArray(reinterpret_cast<const char *>(data), int(frames) * BytesPerFrame);
            capture->ReleaseBuffer(frames);

            capturedBytes += pcm.size();
            if (m_sink && !pcm.isEmpty())
                m_sink(pcm);
        }
    }

    client->Stop();
    wlog(QStringLiteral("share"),
         QStringLiteral("screen sound stopped after %1 s of sound")
             .arg(double(capturedBytes) / (SampleRate * BytesPerFrame), 0, 'f', 1));

done:
    if (capture)
        capture->Release();
    if (client)
        client->Release();
    if (ready)
        CloseHandle(ready);
    if (operation)
        operation->Release();
    handler->Release();
    if (SUCCEEDED(com))
        CoUninitialize();
#else
    Q_UNUSED(source);
    Q_UNUSED(processId);
    emit failed(QStringLiteral("Sharing sound needs Windows."));
#endif
}
