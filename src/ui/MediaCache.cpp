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

// Counts only large pictures. Avatars, icons and emoji are never evicted.
constexpr int MaxCachedImages = 250;

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
    return m_animations.value(url.toString());
}

void MediaCache::touch(const QString &key)
{
    m_order.removeOne(key);
    m_order.append(key);
}

void MediaCache::evictIfNeeded()
{
    // Avatars and server icons are tiny and are on screen constantly, so they
    // are kept out of the count. Only the big pictures are worth dropping.
    const auto isSmall = [](const QString &key) {
        return key.contains(QLatin1String("/avatars/")) || key.contains(QLatin1String("/icons/"))
            || key.contains(QLatin1String("/embed/avatars/")) || key.contains(QLatin1String("/emojis/"));
    };

    int large = 0;
    for (auto it = m_images.constBegin(); it != m_images.constEnd(); ++it) {
        if (!isSmall(it.key()))
            ++large;
    }
    if (large <= MaxCachedImages)
        return;

    // Drop the least recently used large pictures until back under the limit.
    for (int i = 0; i < m_order.size() && large > MaxCachedImages;) {
        const QString key = m_order.at(i);
        if (isSmall(key) || !m_images.contains(key)) {
            ++i;
            continue;
        }
        m_images.remove(key);
        m_animations.remove(key);
        m_order.removeAt(i);
        --large;
    }
}

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
            // Only give up for good when the picture genuinely is not there.
            // A rate limit or a dropped connection deserves another try later,
            // otherwise one bad moment leaves a grey circle forever.
            const bool permanent = status == 403 || status == 404 || status == 410;
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

        m_images.insert(key, picture);
        touch(key);
        evictIfNeeded();

        // Keep the raw bytes only for things that move. Everything else would
        // just double the memory for no gain.
        {
            QBuffer buffer;
            buffer.setData(payload);
            buffer.open(QIODevice::ReadOnly);
            QImageReader reader(&buffer);
            if (reader.supportsAnimation() && reader.imageCount() > 1)
                m_animations.insert(key, payload);
        }

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
