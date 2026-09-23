#pragma once

#include <QHash>
#include <QImage>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPixmap>
#include <QSet>
#include <QThreadPool>
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
    //
    // Large pictures come back shrunk to something the message column can
    // actually show. The message list draws them at 340 across; keeping a
    // phone photo at full size costs eight megabytes to display a thumbnail,
    // and two hundred and fifty of those is where 2.7 GB came from.
    QImage image(const QUrl &url);

    // Full quality, for the viewer that opens when a picture is clicked.
    // Decoded on demand from the bytes exactly as they arrived, so nothing is
    // held at full size while it is only being shown as a thumbnail.
    QImage fullImage(const QUrl &url);

    // The bytes exactly as they arrived, kept for anything that moves so the
    // caller can hand them to QMovie. Empty for still pictures.
    QByteArray animationData(const QUrl &url) const;

    bool has(const QUrl &url) const;
    static bool isAllowedHost(const QUrl &url);

    // One line for the log, so what the cache is holding is a fact rather
    // than something to be worked out from a task manager.
    QString summary() const;

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
    void forget(const QString &key);

    QHash<QString, QImage> m_images;

    // The bytes exactly as they arrived, for anything that moves and for
    // anything that had to be shrunk. Compressed, so a photo sits here at a
    // few hundred kilobytes rather than the eight megabytes it decodes to.
    QHash<QString, QByteArray> m_originals;
    QSet<QString> m_animated;

    QSet<QString> m_inFlight;
    QSet<QString> m_failed;

    // Least recently used first. Kept so a full cache drops only the oldest
    // pictures instead of wiping every one, which used to make avatars all
    // over the window fall back to grey circles at once.
    QList<QString> m_order;

    // Counted in bytes, not in pictures.
    //
    // The limit used to be a number of pictures, and it exempted avatars and
    // emoji entirely. Both were wrong. A picture is not a unit of memory - two
    // hundred and fifty of them is thirty megabytes or three gigabytes
    // depending on what somebody posted - and an exemption with no ceiling is
    // not a cache, it is a leak with a polite name.
    qint64 m_imageBytes = 0;
    qint64 m_originalBytes = 0;

    void recordFailure(const QString &key, int status, const QString &error, qsizetype bytes,
                       bool arrivedWhole);
    void finishDecode(const QUrl &url, const QString &key, const QByteArray &payload, int status,
                      bool decoded, QImage picture, bool moves, bool shrank, QSize original,
                      qint64 tookMs);

    // Pictures are unpacked here rather than on the window's thread. Declared
    // last so it is destroyed first: it waits for any unpacking still running
    // before the maps those results go into are torn down.
    QThreadPool m_decoders;
};
