#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

// Discord expects every request to look like it came from a real desktop
// client. These helpers build the one identity blob that both the REST client
// and the gateway client must agree on.
namespace DiscordIdentity {

constexpr int ApiVersion = 10;

QString userAgent();

// The "super properties" object. The same object is sent twice: base64 encoded
// in the X-Super-Properties header, and raw inside the gateway IDENTIFY.
QJsonObject superProperties();
QByteArray superPropertiesHeader();

QString restBase();
QString gatewayUrl();

} // namespace DiscordIdentity
