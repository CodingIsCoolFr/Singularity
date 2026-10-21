#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

// Discord expects every request to look like it came from a real desktop
// client. These helpers build the one identity blob that both the REST client
// and the gateway client must agree on.
namespace DiscordIdentity {

constexpr int ApiVersion = 10;

// Discord's web client build number, sent in the super properties of every
// request and of IDENTIFY. Discord reads it to decide what a client may use:
// a feature still being rolled out answers 404 to builds older than the one
// it needs. The badge directory did exactly that (21 October), against the
// 419246 this had carried since the first version - while the phone app,
// on a current build, had it.
//
// Never lower than the build this version was made against. Start-up hands
// in the one seen last time, and refreshClientBuild() keeps it current.
int clientBuildNumber();
void setClientBuildNumber(int build);

// Asks discord.com which build its web app is on now, keeps it for this run
// and for the next start (AppConfig "identity/clientBuild"). Does nothing
// when the page cannot be read; the build already in use stays.
void refreshClientBuild();

QString userAgent();

// The "super properties" object. The same object is sent twice: base64 encoded
// in the X-Super-Properties header, and raw inside the gateway IDENTIFY.
QJsonObject superProperties();
QByteArray superPropertiesHeader();

QString restBase();
QString gatewayUrl();

} // namespace DiscordIdentity
