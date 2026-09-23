#pragma once

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QStringList>

// Runs the Discord login flow: password, then a second factor if the account
// asks for one, or a check of this new device if Discord wants that instead.
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
        QString loginInstanceId;

        // In the order Discord's own client lists them, read from its
        // AuthenticationStore: security key, authenticator app, backup code,
        // text message. The first one is what it shows; the rest sit behind
        // "Verify with something else".
        QStringList methods;

        bool has(const QString &method) const { return methods.contains(method); }
    };

    explicit AuthClient(QObject *parent = nullptr);

    // Step one. `login` is the email address or phone number on the account.
    void logIn(const QString &login, const QString &password);

    // Step two. `method` is "totp", "backup" or "sms": the same path segment
    // Discord's client posts to, /auth/mfa/<method>.
    void submitCode(const QString &method, const MfaOptions &options, const QString &code);
    void requestSmsCode(const QString &ticket);

    // The new-device check. Discord texted a code to the phone number that was
    // typed as the login; this trades it for a token and approves the device.
    void verifyPhoneForDevice(const QString &phone, const QString &code);
    void resendPhoneCode(const QString &phone);

    // Sends the request that was stopped by a captcha again, with the answer
    // the person gave attached - exactly what Discord's client does for any
    // request that comes back asking for one.
    void retryWithCaptcha(const QString &captchaKey);

    // Asks Discord to email a password reset link.
    void forgotPassword(const QString &login);

    // The email version: the link Discord mailed carries a token, and this
    // approves the device with it, exactly as the page that link opens does.
    void authorizeDevice(const QString &token);

signals:
    // The flow finished and this is the session token.
    void succeeded(const QString &token);

    // The password was right but the account wants a second factor.
    void mfaRequired(const AuthClient::MfaOptions &options);

    // Discord does not recognise this device. It either texted a code to the
    // phone number used to log in, or emailed a link to approve the sign-in.
    void phoneCheckRequired();
    void emailCheckRequired(const QString &message);

    // The device check went through. Signing in again now works.
    void deviceAuthorized();

    // Discord wants a captcha before it will answer. The window shows Discord's
    // own hCaptcha for the person to solve, then calls retryWithCaptcha().
    // Nothing in this program answers a captcha by itself.
    void captchaRequired(const QString &service, const QString &siteKey, const QString &rqdata);

    // The reset email is on its way.
    void passwordResetSent();

    // A text message was sent successfully.
    void smsCodeSent();

    // Anything else: wrong password, rate limit, network trouble.
    void failed(const QString &message);

private:
    QNetworkRequest buildRequest(const QString &path) const;
    void post(const QString &path, const QJsonObject &body, const QString &stepName);
    void send(const QString &path, const QByteArray &payload, const QString &stepName,
              const QString &captchaKey);

    // The last request, kept so it can be sent again after a captcha, and the
    // captcha's own bookkeeping that has to travel back with the answer.
    QString m_lastPath;
    QByteArray m_lastPayload;
    QString m_lastStep;
    QString m_captchaRqtoken;
    QString m_captchaSessionId;
    void handleReply(QNetworkReply *reply, const QString &stepName);
    bool handleCaptcha(const QJsonObject &body);
    bool handleMfa(const QJsonObject &body);
    bool handleDeviceCheck(const QJsonObject &body);
    static QString describeError(int status, const QJsonObject &body, const QString &fallback);

    QNetworkAccessManager m_network;
};

Q_DECLARE_METATYPE(AuthClient::MfaOptions)
