#include "core/AuthClient.h"

#include "core/DiscordIdentity.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QUrl>
#include <QtMath>

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
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    QNetworkReply *reply = m_network.post(buildRequest(path), payload);
    connect(reply, &QNetworkReply::finished, this, [this, reply, stepName]() {
        handleReply(reply, stepName);
    });
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

void AuthClient::submitTotp(const QString &ticket, const QString &code)
{
    post(QStringLiteral("/auth/mfa/totp"),
         QJsonObject{
             {QStringLiteral("code"), code.trimmed()},
             {QStringLiteral("ticket"), ticket},
             {QStringLiteral("login_source"), QJsonValue::Null},
             {QStringLiteral("gift_code_sku_id"), QJsonValue::Null},
         },
         QStringLiteral("totp"));
}

void AuthClient::submitBackupCode(const QString &ticket, const QString &code)
{
    // Backup codes go to the same endpoint as the authenticator app.
    submitTotp(ticket, code);
}

void AuthClient::requestSmsCode(const QString &ticket)
{
    post(QStringLiteral("/auth/mfa/sms/send"),
         QJsonObject{{QStringLiteral("ticket"), ticket}},
         QStringLiteral("sms-send"));
}

void AuthClient::submitSmsCode(const QString &ticket, const QString &code)
{
    post(QStringLiteral("/auth/mfa/sms"),
         QJsonObject{
             {QStringLiteral("code"), code.trimmed()},
             {QStringLiteral("ticket"), ticket},
             {QStringLiteral("login_source"), QJsonValue::Null},
             {QStringLiteral("gift_code_sku_id"), QJsonValue::Null},
         },
         QStringLiteral("sms"));
}

bool AuthClient::handleCaptcha(const QJsonObject &body)
{
    if (!body.contains(QStringLiteral("captcha_key")))
        return false;

    const QString service = body.value(QStringLiteral("captcha_service")).toString(QStringLiteral("hcaptcha"));
    const QString siteKey = body.value(QStringLiteral("captcha_sitekey")).toString();
    emit captchaRequired(service, siteKey);
    return true;
}

bool AuthClient::handleMfa(const QJsonObject &body)
{
    if (!body.value(QStringLiteral("mfa")).toBool(false))
        return false;

    MfaOptions options;
    options.ticket = body.value(QStringLiteral("ticket")).toString();
    options.totp = body.value(QStringLiteral("totp")).toBool(false);
    options.sms = body.value(QStringLiteral("sms")).toBool(false);
    options.backup = body.value(QStringLiteral("backup")).toBool(false);
    options.webauthn = !body.value(QStringLiteral("webauthn")).isNull()
        && body.contains(QStringLiteral("webauthn"));

    // If Discord says MFA but names no method, assume the authenticator app.
    if (!options.totp && !options.sms && !options.backup)
        options.totp = true;

    emit mfaRequired(options);
    return true;
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

    QString fallback = QStringLiteral("Login failed (HTTP %1).").arg(status);
    if (stepName == QLatin1String("password") && status == 400)
        fallback = QStringLiteral("That email or password is not right.");
    else if (stepName != QLatin1String("password") && status == 400)
        fallback = QStringLiteral("That code is not right.");

    emit failed(describeError(status, body, fallback));
}
