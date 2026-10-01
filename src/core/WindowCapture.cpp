// The Windows Runtime headers come first, before anything from Qt. Qt's
// headers define `signals`, `slots` and `emit` as macros, and C++/WinRT uses
// those words as ordinary names.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <roapi.h>

#include <d3d11.h>
#include <dxgi.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <atomic>

#include "core/WindowCapture.h"

#include "core/Logger.h"

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wdx = winrt::Windows::Graphics::DirectX;
namespace wd3d = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace {

constexpr auto PixelFormat = wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized;

QString hresultText(HRESULT hr)
{
    return QStringLiteral("0x%1").arg(quint32(hr), 8, 16, QLatin1Char('0'));
}

} // namespace

struct WindowCapture::Impl
{
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    wd3d::IDirect3DDevice rtDevice{nullptr};

    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    winrt::event_token closedToken{};
    std::atomic<bool> closed{false};

    // Readable copy of the window. Sized to the window's content, which is
    // what the encoder is handed; the pool's own buffers can be larger.
    winrt::com_ptr<ID3D11Texture2D> staging;
    winrt::Windows::Graphics::SizeInt32 poolSize{};
    int width = 0;
    int height = 0;

    bool mapped = false;
    const uchar *lastData = nullptr;
    int lastStride = 0;

    bool makeStaging(int w, int h)
    {
        unmap();
        staging = nullptr;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = UINT(w);
        desc.Height = UINT(h);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, staging.put())))
            return false;

        width = w;
        height = h;
        return true;
    }

    void unmap()
    {
        if (mapped && context && staging)
            context->Unmap(staging.get(), 0);
        mapped = false;
        lastData = nullptr;
        lastStride = 0;
    }
};

WindowCapture::WindowCapture() = default;

WindowCapture::~WindowCapture()
{
    stop();
}

bool WindowCapture::isRunning() const
{
    return d && d->session != nullptr;
}

int WindowCapture::width() const
{
    return d ? d->width : 0;
}

int WindowCapture::height() const
{
    return d ? d->height : 0;
}

bool WindowCapture::start(quintptr window)
{
    return startItem(window, false);
}

bool WindowCapture::startMonitor(quintptr monitor)
{
    return startItem(monitor, true);
}

bool WindowCapture::startItem(quintptr handle, bool monitor)
{
    stop();

    // The capture thread may already be in a COM apartment, which is fine;
    // it only has to be in one.
    RoInitialize(RO_INIT_MULTITHREADED);

    auto hwnd = monitor ? HWND(nullptr) : reinterpret_cast<HWND>(handle);
    if (!monitor && !IsWindow(hwnd)) {
        wlog(QStringLiteral("share"), QStringLiteral("that window is gone"));
        return false;
    }

    try {
        if (!wgc::GraphicsCaptureSession::IsSupported()) {
            wlog(QStringLiteral("share"), QStringLiteral("this Windows cannot capture single windows"));
            return false;
        }

        auto impl = std::make_unique<Impl>();

        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                       D3D11_SDK_VERSION, impl->device.put(), nullptr,
                                       impl->context.put());
        if (FAILED(hr)) {
            wlog(QStringLiteral("share"), QStringLiteral("no Direct3D device for window capture (%1)")
                                              .arg(hresultText(hr)));
            return false;
        }

        // The capture API speaks Windows Runtime; the device is plain D3D11.
        // This wraps one as the other.
        const auto dxgi = impl->device.as<IDXGIDevice>();
        winrt::com_ptr<::IInspectable> inspectable;
        hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put());
        if (FAILED(hr)) {
            wlog(QStringLiteral("share"), QStringLiteral("could not wrap the device (%1)").arg(hresultText(hr)));
            return false;
        }
        impl->rtDevice = inspectable.as<wd3d::IDirect3DDevice>();

        const auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem,
                                                           IGraphicsCaptureItemInterop>();
        hr = monitor ? interop->CreateForMonitor(reinterpret_cast<HMONITOR>(handle),
                                                 winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                 winrt::put_abi(impl->item))
                     : interop->CreateForWindow(hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                winrt::put_abi(impl->item));
        if (FAILED(hr) || !impl->item) {
            wlog(QStringLiteral("share"), QStringLiteral("Windows would not capture that window (%1)")
                                              .arg(hresultText(hr)));
            return false;
        }

        impl->poolSize = impl->item.Size();
        if (impl->poolSize.Width <= 0 || impl->poolSize.Height <= 0) {
            wlog(QStringLiteral("share"), QStringLiteral("that window has no size (minimised?)"));
            return false;
        }

        // Free-threaded, so frames can be pulled from this thread without a
        // Windows message loop driving it.
        impl->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(impl->rtDevice, PixelFormat, 2,
                                                                          impl->poolSize);
        impl->session = impl->pool.CreateCaptureSession(impl->item);

        // Both are newer than the API itself; older Windows throws, and the
        // defaults (pointer shown, yellow border) are acceptable there.
        try {
            impl->session.IsCursorCaptureEnabled(true);
        } catch (...) {
        }
        try {
            impl->session.IsBorderRequired(false);
        } catch (...) {
        }

        Impl *raw = impl.get();
        impl->closedToken = impl->item.Closed([raw](auto &&, auto &&) { raw->closed = true; });

        if (!impl->makeStaging(impl->poolSize.Width, impl->poolSize.Height))
            return false;

        impl->session.StartCapture();
        d = std::move(impl);

        if (monitor) {
            wlog(QStringLiteral("share"), QStringLiteral("capturing the monitor through Windows.Graphics.Capture, %1 x %2")
                                              .arg(d->width)
                                              .arg(d->height));
            return true;
        }
        wchar_t title[256] = {};
        GetWindowTextW(hwnd, title, int(std::size(title)));
        wlog(QStringLiteral("share"), QStringLiteral("capturing window \"%1\", %2 x %3")
                                          .arg(QString::fromWCharArray(title))
                                          .arg(d->width)
                                          .arg(d->height));
        return true;
    } catch (const winrt::hresult_error &error) {
        wlog(QStringLiteral("share"), QStringLiteral("window capture failed to start: %1 (%2)")
                                          .arg(QString::fromWCharArray(error.message().c_str()))
                                          .arg(hresultText(error.code())));
        return false;
    }
}

void WindowCapture::stop()
{
    if (!d)
        return;

    d->unmap();
    try {
        if (d->item)
            d->item.Closed(d->closedToken);
        if (d->session)
            d->session.Close();
        if (d->pool)
            d->pool.Close();
    } catch (...) {
    }
    d.reset();
}

WindowCapture::Result WindowCapture::grab(const uchar **data, int *stride)
{
    if (!d || !d->session)
        return Result::Failed;
    if (d->closed)
        return Result::Failed;

    const auto handBackLast = [&]() {
        if (d->mapped) {
            if (data)
                *data = d->lastData;
            if (stride)
                *stride = d->lastStride;
        }
        return Result::NoChange;
    };

    try {
        const wgc::Direct3D11CaptureFrame frame = d->pool.TryGetNextFrame();
        if (!frame)
            return handBackLast();   // nothing new, or the window is minimised

        // The window changed size. The pool is rebuilt at the new size and
        // this frame, drawn for the old one, is dropped.
        const auto size = frame.ContentSize();
        if (size.Width != d->poolSize.Width || size.Height != d->poolSize.Height) {
            frame.Close();
            if (size.Width <= 0 || size.Height <= 0)
                return handBackLast();
            d->poolSize = size;
            d->pool.Recreate(d->rtDevice, PixelFormat, 2, size);
            if (!d->makeStaging(size.Width, size.Height))
                return Result::Failed;
            wlog(QStringLiteral("share"),
                 QStringLiteral("shared window is now %1 x %2").arg(size.Width).arg(size.Height));
            return Result::Lost;
        }

        const auto access = frame.Surface()
                                .as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> texture;
        if (FAILED(access->GetInterface(IID_PPV_ARGS(texture.put())))) {
            frame.Close();
            return Result::Failed;
        }

        // The old picture stays readable until the new one is ready.
        d->unmap();

        D3D11_BOX box{0, 0, 0, UINT(d->width), UINT(d->height), 1};
        d->context->CopySubresourceRegion(d->staging.get(), 0, 0, 0, 0, texture.get(), 0, &box);
        frame.Close();

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(d->context->Map(d->staging.get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return Result::Failed;

        d->mapped = true;
        d->lastData = static_cast<const uchar *>(mapped.pData);
        d->lastStride = int(mapped.RowPitch);
        if (data)
            *data = d->lastData;
        if (stride)
            *stride = d->lastStride;
        return Result::Frame;
    } catch (const winrt::hresult_error &error) {
        wlog(QStringLiteral("share"), QStringLiteral("window capture failed: %1")
                                          .arg(hresultText(error.code())));
        return Result::Failed;
    }
}
