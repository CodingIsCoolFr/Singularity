#pragma once

#include "core/AuthClient.h"
#include "core/RemoteAuth.h"

#include <QDialog>
#include <QUrl>

class QLabel;
class QLineEdit;
class QPushButton;
class QCheckBox;
class QComboBox;
class QStackedWidget;
class RestClient;

// Sign-in window.
//
// Page 1 takes the email and password. Page 2 appears only if the account asks
// for a second factor. Page 3 is the token fallback, used when Discord demands
// a captcha that this client cannot answer.
//
// The password lives in the password field and in one network request. It is
// never saved and never logged.
class AuroraWidget;

class LoginDialog : public QDialog
{
    Q_OBJECT

public:
    explicit LoginDialog(RestClient *rest, QWidget *parent = nullptr);

    QString token() const { return m_token; }
    bool shouldRemember() const;

private slots:
    void submitCredentials();
    void submitSecondFactor();
    void onAuthSucceeded(const QString &token);
    void onMfaRequired(const AuthClient::MfaOptions &options);
    void onCaptchaRequired(const QString &service, const QString &siteKey, const QString &rqdata);
    void onAuthFailed(const QString &message);
    void useTokenInstead();
    void useQrInstead();
    void submitDeviceCheck();

    // Shows one second-factor method, the way Discord does: the account's
    // preferred one first, the rest as "use something else" links.
    void selectMfaMethod(const QString &method);
    void showDeviceCheck(bool byPhone, const QString &message);

private:
    // The order they are added to the stack.
    enum Page { CredentialsPage = 0, MfaPage = 1, TokenPage = 2, DevicePage = 3 };

    QWidget *buildCredentialsPage();
    QWidget *buildMfaPage();
    QWidget *buildTokenPage();
    QWidget *buildDevicePage();
    void showPage(Page page);
    void setBusy(bool busy);
    void setStatus(const QString &text, bool isError = false);
    void finishWith(const QString &token);

    RestClient *m_rest = nullptr;
    AuthClient m_auth;
    RemoteAuth m_remote;

    // The black hole behind everything, the same one the main window uses.
    AuroraWidget *m_aurora = nullptr;

    QStackedWidget *m_pages = nullptr;

    // Credentials page
    QLineEdit *m_loginEdit = nullptr;
    QLineEdit *m_passwordEdit = nullptr;
    QCheckBox *m_rememberBox = nullptr;
    QPushButton *m_logInButton = nullptr;

    // Second factor page
    QLabel *m_mfaHeading = nullptr;
    QLineEdit *m_codeEdit = nullptr;
    QPushButton *m_verifyButton = nullptr;
    QPushButton *m_resendSmsLink = nullptr;
    QLabel *m_codeHint = nullptr;
    QWidget *m_otherMethods = nullptr;
    QString m_mfaMethod;
    bool m_smsSent = false;

    // New-device check page
    QLabel *m_deviceHint = nullptr;
    QLineEdit *m_deviceEdit = nullptr;
    QPushButton *m_deviceButton = nullptr;
    QPushButton *m_deviceResendLink = nullptr;
    bool m_deviceByPhone = false;

    // Token page
    QLineEdit *m_tokenEdit = nullptr;
    QPushButton *m_tokenButton = nullptr;

    // QR code, the right half of the front page
    QLabel *m_qrImage = nullptr;
    QLabel *m_qrTitle = nullptr;
    QLabel *m_qrStatus = nullptr;
    QUrl m_scannedAvatar;
    bool m_remoteStarted = false;

    QLabel *m_statusLabel = nullptr;

    AuthClient::MfaOptions m_mfa;
    QString m_token;
};
