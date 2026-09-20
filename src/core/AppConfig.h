#pragma once

#include <QSettings>
#include <QString>

// Small wrapper over QSettings so every part of the app agrees on where
// settings live. Nothing here ever leaves the machine.
class AppConfig
{
public:
    static AppConfig &instance();

    QString token() const;

    // False, with the reason logged, if the token did not persist. Worth
    // telling the person about: it means the next start will ask again.
    bool setToken(const QString &token);

    void clearToken();

    bool pluginEnabled(const QString &pluginId, bool fallback) const;
    void setPluginEnabled(const QString &pluginId, bool enabled);

    QVariant pluginValue(const QString &pluginId, const QString &key, const QVariant &fallback = {}) const;
    void setPluginValue(const QString &pluginId, const QString &key, const QVariant &value);

    // General settings, used by the settings window. Keys look like
    // "voice/inputDevice" or "appearance/fontSize".
    QVariant value(const QString &key, const QVariant &fallback = {}) const;
    void setValue(const QString &key, const QVariant &value);

    void sync();

private:
    AppConfig();

    // Writes, flushes, and checks it actually landed. Returns false and logs
    // the reason when it did not, because a setting that silently fails to
    // save looks fine until the next start.
    bool write(const QString &key, const QVariant &value, const char *what);

    QSettings m_settings;
};
