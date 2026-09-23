#include "core/AuthClient.h"

#include "core/DiscordIdentity.h"
#include "core/Logger.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QUrl>
#include <QtMath>

namespace {

// Discord's own JSON error codes, from the enum in its web client.
constexpr int PhoneVerificationRequired = 70007;

} // namespace

AuthClient::AuthClient(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<AuthClient::MfaOptions>("AuthClient::MfaOptions");
}

QNetworkRequest AuthClient::buildRequest(const QString &path) const
{
    QNetworkRequest request(QUrl(DiscordIdentity::restBase() + path));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, DiscordIdentity::userAgent());
    // No Authorization header here. That is the whole point of this step.
    request.setRawHeader("X-Super-Properties", DiscordIdentity::superPropertiesHeader());
    request.setRawHeader("X-Discord-Locale", "en-US");
    request.setRawHeader("Accept", "*/*");
    request.setRawHeader("Accept-Language", "en-US,en;q=0.9");
    request.setRawHeader("Origin", "https://discord.com");
    request.setRawHeader("Referer", "https://discord.com/login");
    return request;
}

void AuthClient::post(const QString &path, const QJsonObject &body, const QString &stepName)
{
    send(path, QJsonDocument(body).toJson(QJsonDocument::Compact), stepName, QString());
}

void AuthClient::send(const QString &path, const QByteArray &payload, const QString &stepName,
                      const QString &captchaKey)
{
    m_lastPath = path;
    m_lastPayload = payload;
    m_lastStep = stepName;

    QNetworkRequest request = buildRequest(path);
    if (!captchaKey.isEmpty()) {
        request.setRawHeader("X-Captcha-Key", captchaKey.toUtf8());
        if (!m_captchaRqtoken.isEmpty())
            request.setRawHeader("X-Captcha-Rqtoken", m_captchaRqtoken.toUtf8());
        if (!m_captchaSessionId.isEmpty())
            request.setRawHeader("X-Captcha-Session-Id", m_captchaSessionId.toUtf8());
    }

    QNetworkReply *reply = m_network.post(request, payload);
    connect(reply, &QNetworkReply::finished, this, [this, reply, stepName]() {
        handleReply(reply, stepName);
    });
}

void AuthClient::retryWithCaptcha(const QString &captchaKey)
{
    if (m_lastPath.isEmpty() || captchaKey.isEmpty())
        return;
    send(m_lastPath, m_lastPayload, m_lastStep, captchaKey);
}

void AuthClient::forgotPassword(const QString &login)
{
    if (login.trimmed().isEmpty()) {
        emit failed(QStringLiteral("Type your email or phone number first."));
        return;
    }
    post(QStringLiteral("/auth/forgot"), QJsonObject{{QStringLiteral("login"), login.trimmed()}},
         QStringLiteral("forgot"));
}

void AuthClient::logIn(const QString &login, const QString &password)
{
    if (login.trimmed().isEmpty() || password.isEmpty()) {
        emit failed(QStringLiteral("Fill in both the email and the password."));
        return;
    }

    post(QStringLiteral("/auth/login"),
         QJsonObject{
             {QStringLiteral("login"), login.trimmed()},
             {QStringLiteral("password"), password},
             {QStringLiteral("undelete"), false},
             {QStringLiteral("login_source"), QJsonValue::Null},
             {QStringLiteral("gift_code_sku_id"), QJsonValue::Null},
         },
         QStringLiteral("password"));
}

void AuthClient::submitCode(const QString &method, const MfaOptions &options, const QString &code)
{
    // The same request Discord's loginMFAv2 makes: one endpoint per method,
    // carrying the ticket and the login instance from the password step.
    QJsonObject body{
        {QStringLiteral("code"), code.trimmed()},
        {QStringLiteral("ticket"), options.ticket},
        {QStringLiteral("login_source"), QJsonValue::Null},
        {QStringLiteral("gift_code_sku_id"), QJsonValue::Null},
    };
    if (!options.loginInstanceId.isEmpty())
        body.insert(QStringLiteral("login_instance_id"), options.loginInstanceId);

    post(QStringLiteral("/auth/mfa/") + method, body, QStringLiteral("mfa"));
}

void AuthClient::requestSmsCode(const QString &ticket)
{
    post(QStringLiteral("/auth/mfa/sms/send"),
         QJsonObject{{QStringLiteral("ticket"), ticket}},
         QStringLiteral("sms-send"));
}

void AuthClient::verifyPhoneForDevice(const QString &phone, const QString &code)
{
    post(QStringLiteral("/phone-verifications/verify"),
         QJsonObject{
             {QStringLiteral("phone"), phone.trimmed()},
             {QStringLiteral("code"), code.trimmed()},
         },
         QStringLiteral("phone-verify"));
}

void AuthClient::resendPhoneCode(const QString &phone)
{
    post(QStringLiteral("/phone-verifications/resend"),
         QJsonObject{{QStringLiteral("phone"), phone.trimmed()}},
         QStringLiteral("phone-resend"));
}

void AuthClient::authorizeDevice(const QString &token)
{
    post(QStringLiteral("/auth/authorize-ip"),
         QJsonObject{{QStringLiteral("token"), token}},
         QStringLiteral("authorize-ip"));
}

bool AuthClient::handleCaptcha(const QJsonObject &body)
{
    if (!body.contains(QStringLiteral("captcha_key")))
        return false;

    const QString service = body.value(QStringLiteral("captcha_service")).toString(QStringLiteral("hcaptcha"));
    const QString siteKey = body.value(QStringLiteral("captcha_sitekey")).toString();
    m_captchaRqtoken = body.value(QStringLiteral("captcha_rqtoken")).toString();
    m_captchaSessionId = body.value(QStringLiteral("captcha_session_id")).toString();
    wlog(QStringLiteral("login"), QStringLiteral("Discord wants a %1 check before signing in").arg(service));
    emit captchaRequired(service, siteKey, body.value(QStringLiteral("captcha_rqdata")).toString());
    return true;
}

bool AuthClient::handleMfa(const QJsonObject &body)
{
    if (!body.value(QStringLiteral("mfa")).toBool(false))
        return false;

    MfaOptions options;
    options.ticket = body.value(QStringLiteral("ticket")).toString();
    options.loginInstanceId = body.value(QStringLiteral("login_instance_id")).toString();

    // Exactly the order Discord's AuthenticationStore builds its list in.
    const QJsonValue webauthn = body.value(QStringLiteral("webauthn"));
    if (webauthn.isString() && !webauthn.toString().isEmpty())
        options.methods << QStringLiteral("webauthn");
    if (body.value(QStringLiteral("totp")).toBool(false))
        options.methods << QStringLiteral("totp");
    if (body.value(QStringLiteral("backup")).toBool(false))
        options.methods << QStringLiteral("backup");
    if (body.value(QStringLiteral("sms")).toBool(false))
        options.methods << QStringLiteral("sms");

    // Which ones, never the ticket. This is the line to read when someone
    // says the check asked for the wrong thing.
    wlog(QStringLiteral("login"),
         QStringLiteral("second factor needed, account offers: %1")
             .arg(options.methods.isEmpty() ? QStringLiteral("nothing named") : options.methods.join(QStringLiteral(", "))));

    emit mfaRequired(options);
    return true;
}

// Discord sometimes stops a correct password to check the device instead.
//
// Signing in with a phone number from somewhere new fails with code 70007 and
// a text is sent; Discord's client then shows a code box. Signing in by email
// fails with a field error saying a new login location was detected, and the
// approval arrives as an emailed link. Neither is a wrong password, and
// treating them as one left people typing the same password over and over.
bool AuthClient::handleDeviceCheck(const QJsonObject &body)
{
    if (body.value(QStringLiteral("code")).toInt() == PhoneVerificationRequired) {
        wlog(QStringLiteral("login"), QStringLiteral("Discord wants this device checked by text message"));
        emit phoneCheckRequired();
        return true;
    }

    const QJsonObject errors = body.value(QStringLiteral("errors")).toObject();
    for (auto field = errors.constBegin(); field != errors.constEnd(); ++field) {
        const QJsonArray list = field.value().toObject().value(QStringLiteral("_errors")).toArray();
        for (const QJsonValue &entry : list) {
            const QString code = entry.toObject().value(QStringLiteral("code")).toString();
            const QString message = entry.toObject().value(QStringLiteral("message")).toString();
            if (code.contains(QLatin1String("VERIFICATION_EMAIL"))
                || message.contains(QLatin1String("new login location"), Qt::CaseInsensitive)) {
                wlog(QStringLiteral("login"), QStringLiteral("Discord wants this device checked by email"));
                emit emailCheckRequired(message);
                return true;
            }
        }
    }
    return false;
}

QString AuthClient::describeError(int status, const QJsonObject &body, const QString &fallback)
{
    // Discord nests field errors under "errors".
    const QJsonObject errors = body.value(QStringLiteral("errors")).toObject();
    for (auto it = errors.constBegin(); it != errors.constEnd(); ++it) {
        const QJsonArray list = it.value().toObject().value(QStringLiteral("_errors")).toArray();
        if (list.isEmpty())
            continue;
        const QString message = list.first().toObject().value(QStringLiteral("message")).toString();
        if (!message.isEmpty())
            return message;
    }

    const QString top = body.value(QStringLiteral("message")).toString();
    if (!top.isEmpty()) {
        if (status == 429) {
            const double retry = body.value(QStringLiteral("retry_after")).toDouble();
            if (retry > 0)
                return QStringLiteral("Too many tries. Wait about %1 seconds.").arg(qCeil(retry));
        }
        return top;
    }

    if (status == 429)
        return QStringLiteral("Too many tries. Wait a minute and try again.");

    return fallback;
}

void AuthClient::handleReply(QNetworkReply *reply, const QString &stepName)
{
    reply->deleteLater();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray payload = reply->readAll();
    const QJsonObject body = QJsonDocument::fromJson(payload).object();

    // A captcha demand can arrive on any step, with a 200 or a 400.
    if (handleCaptcha(body))
        return;

    if (status >= 200 && status < 300) {
        if (stepName == QLatin1String("sms-send")) {
            emit smsCodeSent();
            return;
        }
        if (stepName == QLatin1String("phone-resend")) {
            emit smsCodeSent();
            return;
        }
        if (stepName == QLatin1String("forgot")) {
            emit passwordResetSent();
            return;
        }
        if (stepName == QLatin1String("phone-verify")) {
            // A verified phone code comes back as a token that approves the
            // device, the same kind the emailed link carries.
            const QString token = body.value(QStringLiteral("token")).toString();
            if (token.isEmpty()) {
                emit failed(QStringLiteral("Discord accepted the code but sent nothing back to approve this device."));
                return;
            }
            authorizeDevice(token);
            return;
        }
        if (stepName == QLatin1String("authorize-ip")) {
            wlog(QStringLiteral("login"), QStringLiteral("this device is approved, signing in again"));
            emit deviceAuthorized();
            return;
        }

        // The password step can answer with an MFA challenge instead of a token.
        if (handleMfa(body))
            return;

        const QString token = body.value(QStringLiteral("token")).toString();
        if (!token.isEmpty()) {
            emit succeeded(token);
            return;
        }

        emit failed(QStringLiteral("Discord replied but sent no token."));
        return;
    }

    if (status == 0) {
        emit failed(QStringLiteral("Could not reach discord.com: %1").arg(reply->errorString()));
        return;
    }

    if (stepName == QLatin1String("password") && handleDeviceCheck(body))
        return;

    QString fallback = QStringLiteral("Login failed (HTTP %1).").arg(status);
    if (stepName == QLatin1String("password") && status == 400)
        fallback = QStringLiteral("That email or password is not right.");
    else if (stepName != QLatin1String("password") && status == 400)
        fallback = QStringLiteral("That code is not right.");

    emit failed(describeError(status, body, fallback));
}
