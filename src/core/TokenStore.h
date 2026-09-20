#pragma once

#include <QString>

// Where the sign-in token lives between runs.
//
// This used to sit in the ordinary settings file, and it silently never
// persisted, so every start demanded the password again. Repeated password
// sign-ins are exactly what makes Discord distrust a client, so this is worth
// its own small file that checks its own work.
//
// On Windows the token is sealed with the account's own key, so another user
// on the same machine cannot read it, and copying the file elsewhere gets
// them nothing.
namespace TokenStore {

QString filePath();

// Returns false, having logged the reason, if the token did not persist.
bool save(const QString &token);

QString load();
void clear();

} // namespace TokenStore
