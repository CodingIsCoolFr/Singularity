#include "core/ScreenCapture.h"

#include "core/Logger.h"
#include "core/ShareAudio.h"
#include "core/WindowCapture.h"

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>

namespace {

template <typename T>
void releaseCom(T *&p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

QString monitorIdFor(int adapter, int output)
{
    return QStringLiteral("%1:%2").arg(adapter).arg(output);
}

} // namespace

ScreenCapture::~ScreenCapture()
{
    stop();
}

QList<ScreenCapture::Monitor> ScreenCapture::monitors()
{
    QList<Monitor> found;

    IDXGIFactory1 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))))
        return found;

    IDXGIAdapter1 *adapter = nullptr;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        IDXGIOutput *output = nullptr;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop) {
                Monitor m;
                m.id = monitorIdFor(int(a), int(o));
                m.width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
                m.height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
                m.primary = desc.DesktopCoordinates.left == 0 && desc.DesktopCoordinates.top == 0;

                // A person picking a screen thinks in "the left one", not in
                // adapter indices, so the name carries the size and whether
                // it is the main one.
                m.name = QStringLiteral("Screen %1 — %2 x %3%4")
                             .arg(found.size() + 1)
                             .arg(m.width)
                             .arg(m.height)
                             .arg(m.primary ? QStringLiteral(" (main)") : QString());
                found.append(m);
            }
            releaseCom(output);
        }
        releaseCom(adapter);
    }

    releaseCom(factory);
    return found;
}

QList<ScreenCapture::Monitor> ScreenCapture::windows()
{
    QList<Monitor> found;
    for (const ShareAudio::App &app : ShareAudio::apps(false)) {
        Monitor m;
        m.id = QStringLiteral("window:%1").arg(quint64(app.window), 0, 16);
        const QString title = app.title.size() > 60 ? app.title.left(59) + QChar(0x2026) : app.title;
        m.name = app.exeName.isEmpty() ? title : QStringLiteral("%1  ·  %2").arg(title, app.exeName);
        m.processId = app.processId;

        RECT rect{};
        if (GetClientRect(reinterpret_cast<HWND>(app.window), &rect)) {
            m.width = rect.right - rect.left;
            m.height = rect.bottom - rect.top;
        }
        found.append(m);
    }
    return found;
}

bool ScreenCapture::build(const QString &monitorId)
{
    if (isWindowId(monitorId)) {
        bool ok = false;
        const quintptr window = quintptr(monitorId.mid(7).toULongLong(&ok, 16));
        if (!ok)
            return false;
        m_window = new WindowCapture;
        if (!m_window->start(window)) {
            delete m_window;
            m_window = nullptr;
            return false;
        }
        m_width = m_window->width();
        m_height = m_window->height();
        m_monitorId = monitorId;
        return true;
    }

    const QStringList parts = monitorId.split(QLatin1Char(':'));
    if (parts.size() != 2)
        return false;

    const UINT wantAdapter = parts.at(0).toUInt();
    const UINT wantOutput = parts.at(1).toUInt();

    IDXGIFactory1 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))))
        return false;

    IDXGIAdapter1 *adapter = nullptr;
    if (factory->EnumAdapters1(wantAdapter, &adapter) == DXGI_ERROR_NOT_FOUND) {
        releaseCom(factory);
        return false;
    }

    // The device must be made on the same adapter the monitor hangs off.
    // Duplicating an output through a different adapter's device is refused,
    // which is what happens on a laptop with two graphics chips if you just
    // create a default device.
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
    };
    D3D_FEATURE_LEVEL got{};

    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, &m_device, &got,
                                   &m_context);
    if (FAILED(hr)) {
        releaseCom(adapter);
        releaseCom(factory);
        return false;
    }

    IDXGIOutput *output = nullptr;
    if (adapter->EnumOutputs(wantOutput, &output) == DXGI_ERROR_NOT_FOUND) {
        releaseCom(adapter);
        releaseCom(factory);
        release();
        return false;
    }

    DXGI_OUTPUT_DESC desc{};
    output->GetDesc(&desc);
    m_width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
    m_height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;

    IDXGIOutput1 *output1 = nullptr;
    hr = output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void **>(&output1));
    releaseCom(output);

    if (FAILED(hr)) {
        releaseCom(adapter);
        releaseCom(factory);
        release();
        return false;
    }

    hr = output1->DuplicateOutput(m_device, &m_duplication);
    releaseCom(output1);
    releaseCom(adapter);
    releaseCom(factory);

    if (FAILED(hr)) {
        // The usual cause is that something else already holds the
        // duplication, and only one program at a time may.
        wlog(QStringLiteral("share"),
             QStringLiteral("could not duplicate screen %1 (0x%2)")
                 .arg(monitorId)
                 .arg(quint32(hr), 8, 16, QLatin1Char('0')));
        release();
        return false;
    }

    // A texture the CPU can read. The duplicated frame lives in video memory
    // where the encoder could in principle take it directly, but that ties us
    // to one vendor's encoder; copying once here works with every one of them
    // and with the software fallback too.
    D3D11_TEXTURE2D_DESC staging{};
    staging.Width = UINT(m_width);
    staging.Height = UINT(m_height);
    staging.MipLevels = 1;
    staging.ArraySize = 1;
    staging.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    staging.SampleDesc.Count = 1;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    if (FAILED(m_device->CreateTexture2D(&staging, nullptr, &m_staging))) {
        release();
        return false;
    }

    m_monitorId = monitorId;
    wlog(QStringLiteral("share"),
         QStringLiteral("capturing screen %1, %2 x %3").arg(monitorId).arg(m_width).arg(m_height));
    return true;
}

bool ScreenCapture::start(const QString &monitorId)
{
    stop();
    return build(monitorId);
}

void ScreenCapture::release()
{
    if (m_mapped && m_context && m_staging) {
        m_context->Unmap(m_staging, 0);
        m_mapped = false;
    }
    m_lastData = nullptr;
    m_lastStride = 0;

    delete m_window;
    m_window = nullptr;

    releaseCom(m_staging);
    releaseCom(m_duplication);
    releaseCom(m_context);
    releaseCom(m_device);
}

void ScreenCapture::stop()
{
    release();
    m_monitorId.clear();
    m_width = m_height = 0;
}

ScreenCapture::Result ScreenCapture::grab(int timeoutMs, const uchar **data, int *stride)
{
    if (m_window) {
        const WindowCapture::Result result = m_window->grab(data, stride);
        m_width = m_window->width();
        m_height = m_window->height();
        switch (result) {
        case WindowCapture::Result::Frame:    return Result::Frame;
        case WindowCapture::Result::NoChange: return Result::NoChange;
        case WindowCapture::Result::Lost:     return Result::Lost;
        case WindowCapture::Result::Failed:   return Result::Failed;
        }
        return Result::Failed;
    }

    if (!m_duplication || !m_staging || !m_context)
        return Result::Failed;

    // The previous frame is deliberately left mapped, and NoChange hands it
    // back again. A still screen then costs nothing in the normal case, and
    // the caller can still re-encode the same pixels when it has a reason to -
    // somebody new starting to watch, and needing a keyframe before anything
    // they see means anything.
    const auto handBackLast = [&]() -> Result {
        if (!m_mapped)
            return Result::NoChange;
        if (data)
            *data = m_lastData;
        if (stride)
            *stride = m_lastStride;
        return Result::NoChange;
    };

    DXGI_OUTDUPL_FRAME_INFO info{};
    IDXGIResource *resource = nullptr;
    HRESULT hr = m_duplication->AcquireNextFrame(UINT(timeoutMs), &info, &resource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // Nothing on screen moved. This is the common case on a desktop and
        // costs nothing, which is the whole reason for using this API.
        return handBackLast();
    }

    if (FAILED(hr)) {
        // Access is lost on a resolution change, a full screen program
        // starting, a driver update, or the screen locking. None of those are
        // errors; they mean build it again.
        releaseCom(resource);
        wlog(QStringLiteral("share"),
             QStringLiteral("screen capture was interrupted (0x%1), starting it again")
                 .arg(quint32(hr), 8, 16, QLatin1Char('0')));

        const QString id = m_monitorId;
        release();
        if (!build(id))
            return Result::Failed;
        return Result::Lost;
    }

    // LastPresentTime of zero means only the mouse pointer moved. The pointer
    // is not in the duplicated picture anyway, so there is nothing new to send.
    if (info.LastPresentTime.QuadPart == 0) {
        releaseCom(resource);
        m_duplication->ReleaseFrame();
        return handBackLast();
    }

    ID3D11Texture2D *texture = nullptr;
    hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&texture));
    releaseCom(resource);

    if (FAILED(hr)) {
        m_duplication->ReleaseFrame();
        return Result::Failed;
    }

    // Only now is the old mapping let go: it had to stay readable right up to
    // the moment a newer picture was ready to replace it.
    if (m_mapped) {
        m_context->Unmap(m_staging, 0);
        m_mapped = false;
    }

    m_context->CopyResource(m_staging, texture);
    releaseCom(texture);
    m_duplication->ReleaseFrame();

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(m_context->Map(m_staging, 0, D3D11_MAP_READ, 0, &mapped)))
        return Result::Failed;

    m_mapped = true;
    m_lastData = static_cast<const uchar *>(mapped.pData);
    m_lastStride = int(mapped.RowPitch);
    if (data)
        *data = static_cast<const uchar *>(mapped.pData);
    if (stride)
        *stride = int(mapped.RowPitch);

    return Result::Frame;
}
