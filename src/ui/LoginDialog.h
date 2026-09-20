#pragma once

#include "core/AuthClient.h"

#include <QDialog>

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
    void onCaptchaRequired(const QString &service, const QString &siteKey);
    void onAuthFailed(const QString &message);
    void useTokenInstead();

private:
    enum Page { CredentialsPage = 0, MfaPage = 1, TokenPage = 2 };

    QWidget *buildCredentialsPage();
    QWidget *buildMfaPage();
    QWidget *buildTokenPage();
    void showPage(Page page);
    void setBusy(bool busy);
    void setStatus(const QString &text, bool isError = false);
    void finishWith(const QString &token);
    void refreshMfaMethodUi();

    RestClient *m_rest = nullptr;
    AuthClient m_auth;

    // The black hole behind everything, the same one the main window uses.
    AuroraWidget *m_aurora = nullptr;

    QStackedWidget *m_pages = nullptr;

    // Credentials page
    QLineEdit *m_loginEdit = nullptr;
    QLineEdit *m_passwordEdit = nullptr;
    QCheckBox *m_rememberBox = nullptr;
    QPushButton *m_logInButton = nullptr;

    // Second factor page
    QComboBox *m_methodBox = nullptr;
    QLineEdit *m_codeEdit = nullptr;
    QPushButton *m_verifyButton = nullptr;
    QPushButton *m_sendSmsButton = nullptr;
    QLabel *m_codeHint = nullptr;

    // Token page
    QLineEdit *m_tokenEdit = nullptr;
    QPushButton *m_tokenButton = nullptr;

    QLabel *m_statusLabel = nullptr;

    AuthClient::MfaOptions m_mfa;
    QString m_token;
};
