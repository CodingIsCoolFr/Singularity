#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

// Discord expects every request to look like it came from a real desktop
// client. These helpers build the one identity blob that both the REST client
// and the gateway client must agree on.
namespace DiscordIdentity {

constexpr int ApiVersion = 10;

// Who this client says it is: Discord's desktop app, at its current version.
// Discord reads the build number in the super properties of every request
// and of IDENTIFY to decide what a client may use, and a feature still being
// rolled out answers 404 to builds older than the one it needs. The badge
// directory did exactly that (21 October), against the 419246 and 1.0.9202
// this had carried since the first version - while the phone app had it.
//
// So nothing here is meant to be edited by hand again. refreshIdentity(),
// at start and every six hours after, keeps each number current:
//   web build        discord.com/app (GLOBAL_ENV BUILD_NUMBER)
//   desktop version  Discord's update server (distributions manifest)
//   Chrome, Electron the official app's Discord.exe, when it is installed
//   native build     the official app's own log, when it is installed
//   Windows          this computer's
// Each is remembered for the next start and only ever moves forward; the
// values compiled in are a floor, for a first start with no network.
int clientBuildNumber();
void refreshIdentity();

QString userAgent();

// The "super properties" object. The same object is sent twice: base64 encoded
// in the X-Super-Properties header, and raw inside the gateway IDENTIFY.
QJsonObject superProperties();
QByteArray superPropertiesHeader();

QString restBase();
QString gatewayUrl();

} // namespace DiscordIdentity
