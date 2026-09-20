#include "core/AppConfig.h"

#include "core/Logger.h"
#include "core/TokenStore.h"

#include <QFileInfo>

namespace {

// Turns a QSettings status into something a person can act on.
QString describe(QSettings::Status status)
{
    switch (status) {
    case QSettings::NoError:     return QStringLiteral("ok");
    case QSettings::AccessError: return QStringLiteral("cannot write the file");
    case QSettings::FormatError: return QStringLiteral("the file is damaged");
    }
    return QStringLiteral("unknown");
}

} // namespace

AppConfig::AppConfig()
    : m_settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("Singularity"), QStringLiteral("Singularity"))
{
    const QFileInfo info(m_settings.fileName());
    wlog(QStringLiteral("config"), QStringLiteral("settings file %1 (%2, folder writable: %3)")
                                       .arg(m_settings.fileName(), describe(m_settings.status()))
                                       .arg(QFileInfo(info.absolutePath()).isWritable()));
}

bool AppConfig::write(const QString &key, const QVariant &value, const char *what)
{
    m_settings.setValue(key, value);
    m_settings.sync();

    const QSettings::Status status = m_settings.status();
    if (status != QSettings::NoError) {
        wlog(QStringLiteral("config"), QStringLiteral("could not save %1: %2")
                                           .arg(QLatin1String(what), describe(status)));
        return false;
    }

    // Read it straight back. A silent failure to persist is the worst kind,
    // because everything looks fine until the next start.
    if (!m_settings.contains(key)) {
        wlog(QStringLiteral("config"), QStringLiteral("saved %1 but it did not stick")
                                           .arg(QLatin1String(what)));
        return false;
    }

    return true;
}

AppConfig &AppConfig::instance()
{
    static AppConfig config;
    return config;
}

// The token deliberately does not live in this file. See TokenStore for why.
QString AppConfig::token() const
{
    return TokenStore::load();
}

bool AppConfig::setToken(const QString &token)
{
    return TokenStore::save(token);
}

void AppConfig::clearToken()
{
    TokenStore::clear();

    // Clean up the old home of the token, from before it had its own file.
    if (m_settings.contains(QStringLiteral("auth/token"))) {
        m_settings.remove(QStringLiteral("auth/token"));
        m_settings.sync();
    }
}

bool AppConfig::pluginEnabled(const QString &pluginId, bool fallback) const
{
    return m_settings.value(QStringLiteral("plugins/%1/enabled").arg(pluginId), fallback).toBool();
}

void AppConfig::setPluginEnabled(const QString &pluginId, bool enabled)
{
    write(QStringLiteral("plugins/%1/enabled").arg(pluginId), enabled, "a plugin switch");
}

QVariant AppConfig::pluginValue(const QString &pluginId, const QString &key, const QVariant &fallback) const
{
    return m_settings.value(QStringLiteral("plugins/%1/%2").arg(pluginId, key), fallback);
}

void AppConfig::setPluginValue(const QString &pluginId, const QString &key, const QVariant &value)
{
    write(QStringLiteral("plugins/%1/%2").arg(pluginId, key), value, "a plugin setting");
}

QVariant AppConfig::value(const QString &key, const QVariant &fallback) const
{
    return m_settings.value(key, fallback);
}

void AppConfig::setValue(const QString &key, const QVariant &value)
{
    write(key, value, "a setting");
}

void AppConfig::sync()
{
    m_settings.sync();
}
