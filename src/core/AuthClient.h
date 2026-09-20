#pragma once

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

// Runs the Discord login flow: password, then a second factor if the account
// asks for one.
//
// The password is used for exactly one request and is never stored, written to
// settings, or put in a log line. Only the token that comes back is kept.
class AuthClient : public QObject
{
    Q_OBJECT

public:
    // Which second factors the account offers after the password step.
    struct MfaOptions
    {
        QString ticket;
        bool totp = false;      // authenticator app
        bool sms = false;       // text message
        bool backup = false;    // one of the printed backup codes
        bool webauthn = false;  // security key, not supported here
    };

    explicit AuthClient(QObject *parent = nullptr);

    // Step one. `login` is the email address or phone number on the account.
    void logIn(const QString &login, const QString &password);

    // Step two, whichever one the account uses.
    void submitTotp(const QString &ticket, const QString &code);
    void submitBackupCode(const QString &ticket, const QString &code);
    void requestSmsCode(const QString &ticket);
    void submitSmsCode(const QString &ticket, const QString &code);

signals:
    // The flow finished and this is the session token.
    void succeeded(const QString &token);

    // The password was right but the account wants a second factor.
    void mfaRequired(const AuthClient::MfaOptions &options);

    // Discord demanded a captcha. Wisp cannot solve one, so the flow stops
    // here and the person is told what happened.
    void captchaRequired(const QString &service, const QString &siteKey);

    // A text message was sent successfully.
    void smsCodeSent();

    // Anything else: wrong password, rate limit, network trouble.
    void failed(const QString &message);

private:
    QNetworkRequest buildRequest(const QString &path) const;
    void post(const QString &path, const QJsonObject &body, const QString &stepName);
    void handleReply(QNetworkReply *reply, const QString &stepName);
    bool handleCaptcha(const QJsonObject &body);
    bool handleMfa(const QJsonObject &body);
    static QString describeError(int status, const QJsonObject &body, const QString &fallback);

    QNetworkAccessManager m_network;
};

Q_DECLARE_METATYPE(AuthClient::MfaOptions)
