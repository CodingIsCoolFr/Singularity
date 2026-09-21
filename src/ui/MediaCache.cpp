#include "ui/MediaCache.h"

#include "core/DiscordIdentity.h"
#include "core/Logger.h"
#include "ui/Theme.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QImageReader>
#include <QNetworkReply>
#include <QPainter>
#include <QPainterPath>

namespace {

// Only Discord's own hosts. Link previews come through images-ext-N.discordapp.net,
// which is Discord re-serving someone else's picture, so the client still never
// contacts a site a stranger chose.
const QStringList kAllowedSuffixes{
    QStringLiteral(".discordapp.com"),
    QStringLiteral(".discordapp.net"),
};

// What the cache is allowed to hold, in bytes.
//
// Decoded pictures are what memory is actually spent on: width times height
// times four, whatever the file on disk weighed. A 4K screenshot is 33 MB the
// moment it is decoded, and the old limit of "250 pictures" let two hundred
// and fifty of those sit in memory at once.
// Both were set once, generously, and never checked against what is on
// screen. The window draws pictures at 340 across and avatars at 40, and a
// channel shows forty messages - so ninety six megabytes of decoded picture
// was room for far more than anybody is ever looking at.
constexpr qint64 MaxImageBytes = 40 * 1024 * 1024;
constexpr qint64 MaxOriginalBytes = 16 * 1024 * 1024;

// Nothing kept for drawing is larger than this on its long edge.
//
// The message column shows pictures at 340 across, and the member list and
// message list show avatars at 40. Storing more than this and then shrinking
// it on every repaint is paying twice for something nobody sees. The full
// quality version is still available to the viewer, decoded from the original
// bytes when a picture is actually opened.
// 640 was a guess. The box a picture is drawn in is 340 by 280, so 420 still
// leaves room for a window scaled up a little, and costs 57 per cent less
// memory per picture than 640 did.
constexpr int StoreLongEdge = 420;

// The circle drawn behind someone's initials when they have no picture.
//
// Greys rather than colours, so a missing avatar sits quietly in the list
// instead of being the brightest thing on screen. Five of them, far enough
// apart that two people beside each other rarely match.
const QStringList kFallbackColours{
    QStringLiteral("#3a3a3a"), QStringLiteral("#4a4a4a"), QStringLiteral("#5a5a5a"),
    QStringLiteral("#6a6a6a"), QStringLiteral("#2e2e2e"),
};

} // namespace

MediaCache::MediaCache() = default;

MediaCache &MediaCache::instance()
{
    static MediaCache cache;
    return cache;
}

bool MediaCache::isAllowedHost(const QUrl &url)
{
    if (url.scheme() != QLatin1String("https"))
        return false;

    const QString host = url.host().toLower();
    for (const QString &suffix : kAllowedSuffixes) {
        if (host.endsWith(suffix))
            return true;
    }
    return false;
}

bool MediaCache::has(const QUrl &url) const
{
    return m_images.contains(url.toString());
}

QByteArray MediaCache::animationData(const QUrl &url) const
{
    const QString key = url.toString();
    if (!m_animated.contains(key))
        return {};
    return m_originals.value(key);
}

QImage MediaCache::fullImage(const QUrl &url)
{
    const QString key = url.toString();

    // Decoded here and handed straight to the caller, never stored. The viewer
    // holds it for as long as it is open and it goes when the window closes.
    const auto original = m_originals.constFind(key);
    if (original != m_originals.constEnd()) {
        QImage picture;
        if (picture.loadFromData(original.value()))
            return picture;
    }

    return image(url);
}

QString MediaCache::summary() const
{
    return QStringLiteral("%1 pictures using %2 MB, %3 originals using %4 MB")
        .arg(m_images.size())
        .arg(m_imageBytes / (1024 * 1024))
        .arg(m_originals.size())
        .arg(m_originalBytes / (1024 * 1024));
}

void MediaCache::touch(const QString &key)
{
    m_order.removeOne(key);
    m_order.append(key);
}

void MediaCache::forget(const QString &key)
{
    const auto picture = m_images.constFind(key);
    if (picture != m_images.constEnd()) {
        m_imageBytes -= picture.value().sizeInBytes();
        m_images.erase(picture);
    }

    const auto original = m_originals.constFind(key);
    if (original != m_originals.constEnd()) {
        m_originalBytes -= original.value().size();
        m_originals.erase(original);
    }

    m_animated.remove(key);
}

void MediaCache::evictIfNeeded()
{
    // Oldest first, and nothing is exempt.
    //
    // Avatars and emoji used to be exempt on the grounds that they are small
    // and always on screen. They are small, but a large server has thousands
    // of distinct faces, and an exemption with no ceiling means the cache only
    // ever grows. The budget is large enough that faces on screen are not
    // going anywhere; it is the pictures nobody has looked at for a while that
    // go first, which is what least recently used means.
    int i = 0;
    while (i < m_order.size() && (m_imageBytes > MaxImageBytes || m_originalBytes > MaxOriginalBytes)) {
        const QString key = m_order.at(i);

        // Leave the most recent alone. Dropping what is on screen right now
        // would only make it be fetched again immediately.
        if (m_order.size() - i <= 64)
            break;

        if (!m_images.contains(key) && !m_originals.contains(key)) {
            m_order.removeAt(i);
            continue;
        }

        forget(key);
        m_order.removeAt(i);
    }
}

namespace {

// Whether this address is worth fetching as a picture at all.
//
// Judged on the extension, before anything is downloaded. Discord serves
// video and audio attachments from the same host as images, and a video
// arrives with a perfectly good 200 - it simply is not a picture, which is
// only discovered after the whole file is on the wire.
bool looksLikeAPicture(const QUrl &url)
{
    const QString path = url.path().toLower();

    // Avatars, icons, banners and emoji have no extension at all. They are
    // always pictures.
    const int dot = path.lastIndexOf(QLatin1Char('.'));
    if (dot < 0 || dot < path.lastIndexOf(QLatin1Char('/')))
        return true;

    const QString suffix = path.mid(dot + 1);

    static const QSet<QString> pictures = {
        QStringLiteral("png"),  QStringLiteral("jpg"),  QStringLiteral("jpeg"),
        QStringLiteral("gif"),  QStringLiteral("webp"), QStringLiteral("bmp"),
        QStringLiteral("apng"), QStringLiteral("avif"), QStringLiteral("jfif"),
        QStringLiteral("ico"),  QStringLiteral("tif"),  QStringLiteral("tiff"),
    };

    return pictures.contains(suffix);
}

} // namespace

QImage MediaCache::image(const QUrl &url)
{
    const QString key = url.toString();

    const auto it = m_images.constFind(key);
    if (it != m_images.constEnd()) {
        touch(key);
        return it.value();
    }

    // A picture that already failed is never retried, or a dead link would
    // start a download on every repaint.
    if (m_failed.contains(key) || m_inFlight.contains(key) || !isAllowedHost(url))
        return {};

    // Films are not pictures, and fetching ten megabytes to find that out is
    // a poor way to learn it. A channel with a few videos in it was pulling
    // them down again and again for nothing.
    if (!looksLikeAPicture(url)) {
        m_failed.insert(key);
        return {};
    }

    fetch(url);
    return {};
}

void MediaCache::fetch(const QUrl &url)
{
    const QString key = url.toString();
    m_inFlight.insert(key);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, DiscordIdentity::userAgent());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, url, key]() {
        reply->deleteLater();
        m_inFlight.remove(key);

        const QByteArray payload = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        QImage picture;
        if (reply->error() != QNetworkReply::NoError || !picture.loadFromData(payload)) {
            // Only give up for good when trying again could not possibly help.
            //
            // A rate limit or a dropped connection deserves another try later,
            // otherwise one bad moment leaves a grey circle forever. But a
            // file that arrived whole and simply is not a picture will never
            // become one, and that case was being treated as temporary.
            //
            // The cost of that was not small. Every repaint asked again, the
            // request succeeded again, the decode failed again, and with half
            // a dozen videos in a channel the client spent the rest of the
            // session downloading the same few megabytes over and over, many
            // times a second. That is what made everything feel slow.
            const bool arrivedWhole = reply->error() == QNetworkReply::NoError
                && status >= 200 && status < 300 && !payload.isEmpty();

            const bool permanent = arrivedWhole || status == 403 || status == 404
                || status == 410 || status == 400 || status == 401;

            if (permanent)
                m_failed.insert(key);

            wlog(QStringLiteral("media"), QStringLiteral("failed %1: HTTP %2 %3 (%4 bytes,%5 retryable)")
                                              .arg(key)
                                              .arg(status)
                                              .arg(reply->errorString())
                                              .arg(payload.size())
                                              .arg(permanent ? QStringLiteral(" not") : QString()));
            return;
        }

        bool moves = false;
        {
            QBuffer buffer;
            buffer.setData(payload);
            buffer.open(QIODevice::ReadOnly);
            QImageReader reader(&buffer);
            moves = reader.supportsAnimation() && reader.imageCount() > 1;
        }

        // Shrunk here, once, rather than at full size for ever.
        //
        // The old code stored whatever arrived. A photo from a phone is
        // 4032 by 3024, which is 48 MB of memory to draw a thumbnail 340
        // across, and it was kept at that size for the rest of the session.
        const int longEdge = qMax(picture.width(), picture.height());
        const bool shrank = longEdge > StoreLongEdge;
        if (shrank) {
            picture = longEdge == picture.width()
                ? picture.scaledToWidth(StoreLongEdge, Qt::SmoothTransformation)
                : picture.scaledToHeight(StoreLongEdge, Qt::SmoothTransformation);
        }

        forget(key);

        m_images.insert(key, picture);
        m_imageBytes += picture.sizeInBytes();

        // The bytes as they arrived are kept for two reasons: anything that
        // moves needs them to play, and anything that was shrunk needs them so
        // the viewer can still open it at full quality. They are compressed,
        // so this is a fraction of what holding the decoded picture cost.
        if (moves || shrank) {
            m_originals.insert(key, payload);
            m_originalBytes += payload.size();
            if (moves)
                m_animated.insert(key);
        }

        touch(key);
        evictIfNeeded();

        emit ready(url);
    });
}

QPixmap MediaCache::circular(const QImage &source, int size)
{
    if (source.isNull() || size <= 0)
        return {};

    // Scale so the short edge fills the circle, then centre-crop.
    const QImage scaled = source.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const int x = (scaled.width() - size) / 2;
    const int y = (scaled.height() - size) / 2;

    QPixmap result(size, size);
    result.fill(Qt::transparent);

    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    QPainterPath clip;
    clip.addEllipse(0, 0, size, size);
    painter.setClipPath(clip);
    painter.drawImage(-x, -y, scaled);

    return result;
}

QPixmap MediaCache::initialsAvatar(const QString &name, int size)
{
    if (size <= 0)
        return {};

    QString initials;
    const QStringList parts = name.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        // Skip leading emoji and punctuation so the letter is a real letter.
        for (const QChar ch : part) {
            if (ch.isLetterOrNumber()) {
                initials += ch.toUpper();
                break;
            }
        }
        if (initials.size() >= 2)
            break;
    }
    if (initials.isEmpty())
        initials = QStringLiteral("#");

    // Same name always gets the same colour.
    const QByteArray digest = QCryptographicHash::hash(name.toUtf8(), QCryptographicHash::Md5);
    const int index = static_cast<quint8>(digest.at(0)) % kFallbackColours.size();

    QPixmap result(size, size);
    result.fill(Qt::transparent);

    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing, true);

    painter.setBrush(QColor(kFallbackColours.at(index)));
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(0, 0, size, size);

    QFont font = painter.font();
    font.setPixelSize(size * 4 / 10);
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);
    painter.setPen(QColor(Theme::Dark));
    painter.drawText(QRect(0, 0, size, size), Qt::AlignCenter, initials);

    return result;
}

QPixmap MediaCache::brandMark(int size)
{
    if (size <= 0)
        return {};

    static QPixmap source;
    if (source.isNull())
        source = QPixmap(QStringLiteral(":/brand/singularity.png"));
    if (source.isNull())
        return initialsAvatar(QStringLiteral("W"), size);

    return source.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

QUrl MediaCache::avatarUrl(const QString &userId, const QString &avatarHash, int size)
{
    if (!avatarHash.isEmpty()) {
        const QString ext = avatarHash.startsWith(QStringLiteral("a_")) ? QStringLiteral("gif")
                                                                       : QStringLiteral("png");
        return QUrl(QStringLiteral("https://cdn.discordapp.com/avatars/%1/%2.%3?size=%4")
                        .arg(userId, avatarHash, ext)
                        .arg(size));
    }

    if (userId.isEmpty())
        return {};

    // Discord's built-in avatars. For current usernames the picture is chosen
    // from bits 22 and up of the account id.
    bool ok = false;
    const quint64 id = userId.toULongLong(&ok);
    const int index = ok ? static_cast<int>((id >> 22) % 6) : 0;
    return QUrl(QStringLiteral("https://cdn.discordapp.com/embed/avatars/%1.png").arg(index));
}

QUrl MediaCache::bannerUrl(const QString &userId, const QString &bannerHash, int size)
{
    if (userId.isEmpty() || bannerHash.isEmpty())
        return {};
    const QString ext = bannerHash.startsWith(QStringLiteral("a_")) ? QStringLiteral("gif")
                                                                    : QStringLiteral("png");
    return QUrl(QStringLiteral("https://cdn.discordapp.com/banners/%1/%2.%3?size=%4")
                    .arg(userId, bannerHash, ext)
                    .arg(size));
}

QUrl MediaCache::decorationUrl(const QString &asset)
{
    if (asset.isEmpty())
        return {};

    // passthrough=true hands back an animated PNG, and Qt ships no reader for
    // those, so the picture came out empty. passthrough=false asks the CDN to
    // flatten it into an ordinary PNG we can actually draw.
    return QUrl(QStringLiteral("https://cdn.discordapp.com/avatar-decoration-presets/%1.png"
                               "?size=160&passthrough=false")
                    .arg(asset));
}

QUrl MediaCache::activityAssetUrl(const QString &applicationId, const QString &assetKey)
{
    if (assetKey.isEmpty())
        return {};

    // "mp:external/..." means Discord is proxying a picture from elsewhere.
    if (assetKey.startsWith(QStringLiteral("mp:")))
        return QUrl(QStringLiteral("https://media.discordapp.net/") + assetKey.mid(3));

    if (applicationId.isEmpty())
        return {};

    return QUrl(QStringLiteral("https://cdn.discordapp.com/app-assets/%1/%2.png?size=160")
                    .arg(applicationId, assetKey));
}

QUrl MediaCache::guildIconUrl(const QString &guildId, const QString &iconHash, int size)
{
    if (guildId.isEmpty() || iconHash.isEmpty())
        return {};
    const QString ext = iconHash.startsWith(QStringLiteral("a_")) ? QStringLiteral("gif")
                                                                  : QStringLiteral("png");
    return QUrl(QStringLiteral("https://cdn.discordapp.com/icons/%1/%2.%3?size=%4")
                    .arg(guildId, iconHash, ext)
                    .arg(size));
}
