#pragma once

#include "core/SettingsProto.h"

#include <QDateTime>
#include <QList>
#include <QString>

// Copies of your server folders kept by Discord's own desktop app.
//
// The app caches the account's settings blob in its local storage, a LevelDB
// folder under %AppData%\discord. Its older files hold older copies, and they
// stay on disk until the app next runs and tidies them away. So when the rail
// loses its folders - a bad save from any client, which Discord then hands to
// every device - an earlier arrangement can usually still be read back from
// there (21 October: a copy from five days before had all nine folders).
//
// Only read, never written. Discord does not have to be closed.
namespace DiscordAppCache {

struct FolderSnapshot
{
    QString app;          // "Discord", "Discord PTB" or "Discord Canary"
    QDateTime savedAt;    // when the file holding it was last written
    QList<DiscordFolder> folders;   // in rail order, as the settings blob has it

    int folderCount() const;   // real folders, not loose servers
    int serverCount() const;
};

// Every distinct arrangement found, newest file first. Empty when the app is
// not installed or has never cached any.
QList<FolderSnapshot> folderSnapshots();

} // namespace DiscordAppCache
