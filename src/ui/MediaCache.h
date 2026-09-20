#pragma once

#include <QHash>
#include <QImage>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPixmap>
#include <QSet>
#include <QUrl>

// One downloader for every picture the app shows: avatars, server icons and
// pictures inside messages.
//
// Ask for an image. If it is already here you get it at once. If not you get
// nothing, a download starts, and `ready` fires when it lands. Callers listen
// for `ready` and draw again.
//
// Only Discord's own media hosts are fetched, so a message cannot make the
// client call out to an address a stranger chose.
class MediaCache : public QObject
{
    Q_OBJECT

public:
    static MediaCache &instance();

    // Empty while the download is still running. This is the first frame only,
    // which is all a still picture needs.
    QImage image(const QUrl &url);

    // The bytes exactly as they arrived, kept for anything that moves so the
    // caller can hand them to QMovie. Empty for still pictures.
    QByteArray animationData(const QUrl &url) const;

    bool has(const QUrl &url) const;
    static bool isAllowedHost(const QUrl &url);

    // Picture helpers.
    static QPixmap circular(const QImage &source, int size);
    static QPixmap initialsAvatar(const QString &name, int size);
    static QPixmap brandMark(int size);

    // Discord picture addresses.
    static QUrl avatarUrl(const QString &userId, const QString &avatarHash, int size = 80);
    static QUrl guildIconUrl(const QString &guildId, const QString &iconHash, int size = 96);
    static QUrl bannerUrl(const QString &userId, const QString &bannerHash, int size = 480);
    static QUrl decorationUrl(const QString &asset);

    // The picture a game shows next to "Playing ...". Rich presence assets
    // come either as an app asset id or as an "mp:" proxy path.
    static QUrl activityAssetUrl(const QString &applicationId, const QString &assetKey);

signals:
    void ready(const QUrl &url);

private:
    MediaCache();
    void fetch(const QUrl &url);

    QNetworkAccessManager m_network;
    void touch(const QString &key);
    void evictIfNeeded();

    QHash<QString, QImage> m_images;
    QHash<QString, QByteArray> m_animations;
    QSet<QString> m_inFlight;
    QSet<QString> m_failed;

    // Least recently used first. Kept so a full cache drops only the oldest
    // pictures instead of wiping every one, which used to make avatars all
    // over the window fall back to grey circles at once.
    QList<QString> m_order;
};
