#pragma once

#include <QList>
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

// Every account signed in on this machine that asked to stay signed in, for
// switching between them the way the official client does.
//
// Kept in a second file, sealed the same way. The active session is still the
// single token above; this is the list to pick the next one from. It is only
// ever added to from a session that was itself remembered, so an account
// signed in with "stay signed in" unticked is not quietly kept here either.
struct Account
{
    QString userId;
    QString username;
    QString avatarHash;
    QString token;
};

// Newest first. Discord's own switcher stops at five, and so does this.
QList<Account> accounts();
void rememberAccount(const Account &account);
void forgetAccount(const QString &userId);

} // namespace TokenStore
