#include "ui/CaptchaDialog.h"

#include "core/Logger.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QResizeEvent>
#include <QVBoxLayout>
#include <QWidget>

#ifdef SINGULARITY_HAVE_WEBVIEW2
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#ifndef interface
#define interface struct
#endif
#include <WebView2.h>

#include <QStandardPaths>
#include <QDir>

namespace {

template <typename Iface>
class Handler : public Iface
{
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG left = --m_refs;
        if (left == 0)
            delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override
    {
        if (id == __uuidof(Iface) || id == IID_IUnknown) {
            *out = static_cast<Iface *>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

protected:
    virtual ~Handler() = default;

private:
    ULONG m_refs = 1;
};

} // namespace

struct CaptchaDialog::Browser
{
    ICoreWebView2Controller *controller = nullptr;
    ICoreWebView2 *view = nullptr;

    ~Browser()
    {
        if (controller)
            controller->Release();
        if (view)
            view->Release();
    }
};
#endif

CaptchaDialog::CaptchaDialog(const QString &sitekey, const QString &rqdata, QWidget *parent)
    : QDialog(parent)
    , m_sitekey(sitekey)
    , m_rqdata(rqdata)
{
    setWindowTitle(QStringLiteral("Discord needs a check"));
    setModal(true);
    resize(440, 620);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);

    auto *note = new QLabel(
        QStringLiteral("Discord asked for a check before that message can be sent. "
                       "Finish it here and the message goes out."),
        this);
    note->setWordWrap(true);
    layout->addWidget(note);

    m_host = new QWidget(this);
    m_host->setAttribute(Qt::WA_NativeWindow);
    m_host->setMinimumHeight(480);
    layout->addWidget(m_host, 1);
}

CaptchaDialog::~CaptchaDialog()
{
#ifdef SINGULARITY_HAVE_WEBVIEW2
    delete m_browser;
#endif
}

QString CaptchaDialog::solve(QWidget *parent, const QString &sitekey, const QString &rqdata)
{
    if (sitekey.isEmpty())
        return {};

    CaptchaDialog dialog(sitekey, rqdata, parent);
    dialog.show();
    dialog.startBrowser();
    dialog.exec();
    return dialog.m_token;
}

void CaptchaDialog::startBrowser()
{
#ifndef SINGULARITY_HAVE_WEBVIEW2
    wlog(QStringLiteral("captcha"), QStringLiteral("no browser component, the check cannot be shown"));
    return;
#else
    m_browser = new Browser;
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/WebView2");
    QDir().mkpath(folder);
    const std::wstring userData = folder.toStdWString();

    struct EnvDone : Handler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>
    {
        CaptchaDialog *dialog = nullptr;
        explicit EnvDone(CaptchaDialog *d) : dialog(d) {}
        HRESULT STDMETHODCALLTYPE Invoke(HRESULT error, ICoreWebView2Environment *env) override
        {
            if (FAILED(error) || !env || !dialog)
                return error;
            struct CtrlDone : Handler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>
            {
                CaptchaDialog *dialog = nullptr;
                explicit CtrlDone(CaptchaDialog *d) : dialog(d) {}
                HRESULT STDMETHODCALLTYPE Invoke(HRESULT error, ICoreWebView2Controller *controller) override
                {
                    if (FAILED(error) || !controller || !dialog || !dialog->m_browser)
                        return error;
                    dialog->m_browser->controller = controller;
                    controller->AddRef();
                    controller->put_IsVisible(TRUE);
                    controller->get_CoreWebView2(&dialog->m_browser->view);
                    dialog->resizeBrowser();

                    struct NavDone : Handler<ICoreWebView2NavigationCompletedEventHandler>
                    {
                        CaptchaDialog *dialog = nullptr;
                        explicit NavDone(CaptchaDialog *d) : dialog(d) {}
                        HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *,
                                                         ICoreWebView2NavigationCompletedEventArgs *) override
                        {
                            if (dialog)
                                dialog->inject();
                            return S_OK;
                        }
                    };
                    struct MsgDone : Handler<ICoreWebView2WebMessageReceivedEventHandler>
                    {
                        CaptchaDialog *dialog = nullptr;
                        explicit MsgDone(CaptchaDialog *d) : dialog(d) {}
                        HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *,
                                                         ICoreWebView2WebMessageReceivedEventArgs *args) override
                        {
                            if (!dialog || !args)
                                return S_OK;
                            LPWSTR raw = nullptr;
                            if (FAILED(args->TryGetWebMessageAsString(&raw)) || !raw)
                                return S_OK;
                            dialog->m_token = QString::fromWCharArray(raw);
                            CoTaskMemFree(raw);
                            dialog->accept();
                            return S_OK;
                        }
                    };

                    EventRegistrationToken token;
                    dialog->m_browser->view->add_NavigationCompleted(new NavDone(dialog), &token);
                    dialog->m_browser->view->add_WebMessageReceived(new MsgDone(dialog), &token);
                    // The puzzle has to run on discord.com. The site key is
                    // registered to that name, and a token from anywhere else
                    // is refused.
                    dialog->m_browser->view->Navigate(L"https://discord.com/login");
                    return S_OK;
                }
            };
            env->CreateCoreWebView2Controller(reinterpret_cast<HWND>(dialog->m_host->winId()),
                                             new CtrlDone(dialog));
            return S_OK;
        }
    };

    CreateCoreWebView2EnvironmentWithOptions(nullptr, userData.c_str(), nullptr, new EnvDone(this));
#endif
}

void CaptchaDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    resizeBrowser();
}

void CaptchaDialog::resizeBrowser()
{
#ifdef SINGULARITY_HAVE_WEBVIEW2
    if (!m_browser || !m_browser->controller || !m_host)
        return;
    RECT bounds{0, 0, m_host->width(), m_host->height()};
    m_browser->controller->put_Bounds(bounds);
#endif
}

void CaptchaDialog::inject()
{
#ifdef SINGULARITY_HAVE_WEBVIEW2
    if (!m_browser || !m_browser->view)
        return;

    const QString sitekey = m_sitekey;
    const QString rqdata = m_rqdata;
    const QString script = QStringLiteral(R"JS(
(function () {
  if (window.__singularityCaptcha) return;
  window.__singularityCaptcha = true;
  const sitekey = %1;
  const rqdata = %2;
  function start() {
    const box = document.createElement('div');
    box.style.cssText = 'position:fixed;inset:0;background:#0b0d12;z-index:2147483647;display:flex;align-items:center;justify-content:center;';
    document.body.appendChild(box);
    const id = hcaptcha.render(box, {
      sitekey: sitekey,
      theme: 'dark',
      callback: function (token) { window.chrome.webview.postMessage(token); }
    });
    if (rqdata) hcaptcha.setData(id, { rqdata: rqdata });
  }
  if (window.hcaptcha) { start(); return; }
  const tag = document.createElement('script');
  tag.src = 'https://js.hcaptcha.com/1/api.js?render=explicit';
  tag.onload = start;
  document.head.appendChild(tag);
})();
)JS")
                             .arg(QString::fromUtf8(QJsonDocument(QJsonArray{sitekey}).toJson(QJsonDocument::Compact)).mid(1).chopped(1),
                                  QString::fromUtf8(QJsonDocument(QJsonArray{rqdata}).toJson(QJsonDocument::Compact)).mid(1).chopped(1));

    m_browser->view->ExecuteScript(reinterpret_cast<LPCWSTR>(script.utf16()), nullptr);
#endif
}
