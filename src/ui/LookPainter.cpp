#include "ui/LookPainter.h"

#include "core/DiscordIdentity.h"
#include "core/Logger.h"
#include "ui/AnimatedImage.h"
#include "ui/MediaCache.h"
#include "ui/Theme.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QLinearGradient>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QtMath>

#include <algorithm>
#include <cmath>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwrite_3.h>
#endif

namespace {

QColor rgb(quint32 value)
{
    return QColor(QRgb(0xff000000u | (value & 0xffffff)));
}

QColor withAlpha(const QColor &colour, int alpha)
{
    QColor out = colour;
    out.setAlpha(alpha);
    return out;
}

// --- colour, as Discord's client works it out ----------------------------

// WCAG relative luminance and contrast, which chroma.js computes for
// Discord's client.
qreal channel(qreal c)
{
    return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

qreal luminance(const QColor &colour)
{
    return 0.2126 * channel(colour.redF()) + 0.7152 * channel(colour.greenF()) + 0.0722 * channel(colour.blueF());
}

qreal contrast(const QColor &a, const QColor &b)
{
    const qreal la = luminance(a);
    const qreal lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

// Discord's minimum contrast for a name colour: on a dark background, five
// steps of lightness at a time until it reads, ten at most.
QColor readable(QColor colour, const QColor &background, qreal ratio)
{
    const bool dark = luminance(background) <= 0.5;
    for (int step = 0; step < 10 && contrast(colour, background) < ratio; ++step) {
        float h, s, l, a;
        colour.getHslF(&h, &s, &l, &a);
        if (dark) {
            if (l >= 0.95f)
                break;
            l += 0.05f;
        } else {
            if (l <= 0.05f)
                break;
            l -= 0.05f;
        }
        colour = QColor::fromHslF(h < 0 ? 0 : h, s, std::min(1.0f, std::max(0.0f, l)));
    }
    return colour;
}

QColor withLightness(const QColor &colour, qreal lightness)
{
    float h, s, l, a;
    colour.getHslF(&h, &s, &l, &a);
    return QColor::fromHslF(h < 0 ? 0 : h, s, float(std::clamp(lightness, 0.0, 1.0)));
}

// Discord's derived shades of a style's main colour ($0 in its client).
struct Shades
{
    QColor main, light1, light2, dark1, dark2, toonStroke, neonStroke;
};

Shades shadesOf(const QColor &main)
{
    float h, s, l, a;
    main.getHslF(&h, &s, &l, &a);
    Shades out;
    out.main = main;
    out.light1 = withLightness(main, std::min(1.0, 1.2 * l));
    out.light2 = withLightness(main, std::min(1.0, 1.6 * l));
    out.dark1 = withLightness(main, std::max(0.0, 0.6 * l));
    out.dark2 = withLightness(main, std::max(0.0, 0.2 * l));
    out.toonStroke = withLightness(main, std::max(0.12, 0.4 * l));
    out.neonStroke = QColor::fromHslF(h < 0 ? 0 : h, std::min(1.0f, 1.2f * s), std::min(0.6f, l + 0.1f));
    return out;
}

// Each effect's default colours, used when a style names none.
QList<quint32> defaultColours(int effect)
{
    switch (effect) {
    case Cosmetics::Gradient:
    case Cosmetics::Glow: return {0x2aaea6, 0xffc390};
    case Cosmetics::Gummy: return {0xe9a9d5, 0xa9ddf0, 0xb9a7ee, 0xb9eacb};
    case Cosmetics::Prism: return {0xfe9242, 0x35ed7e, 0xffe047, 0xff4cd2, 0x5865f2};
    case Cosmetics::Toon: return {0xf42098};
    case Cosmetics::Neon: return {0x691ded};
    case Cosmetics::Pop: return {0x0fcf86};
    default: return {};
    }
}

qreal minContrast(int effect)
{
    switch (effect) {
    case Cosmetics::Gradient:
    case Cosmetics::Glow:
    case Cosmetics::Prism: return 2.5;
    default: return 3.0;
    }
}

// Discord's nameplate palettes: the dark theme's colour for each.
QColor paletteColour(const QString &palette)
{
    static const QHash<QString, QColor> colours{
        {QStringLiteral("crimson"), QColor(0x90, 0x00, 0x07)}, {QStringLiteral("berry"), QColor(0x89, 0x3a, 0x99)},
        {QStringLiteral("sky"), QColor(0x00, 0x80, 0xb7)},     {QStringLiteral("teal"), QColor(0x08, 0x64, 0x60)},
        {QStringLiteral("forest"), QColor(0x2d, 0x54, 0x01)},  {QStringLiteral("bubble_gum"), QColor(0xdc, 0x3e, 0x97)},
        {QStringLiteral("violet"), QColor(0x73, 0x0b, 0xc8)},  {QStringLiteral("cobalt"), QColor(0x01, 0x31, 0xc2)},
        {QStringLiteral("clover"), QColor(0x04, 0x7b, 0x20)},  {QStringLiteral("lemon"), QColor(0xf6, 0xcd, 0x12)},
        {QStringLiteral("white"), QColor(0xff, 0xff, 0xff)},   {QStringLiteral("black"), QColor(0x00, 0x00, 0x00)},
    };
    return colours.value(palette);
}

QUrl collectiblesUrl(const QString &skuId, const char *format)
{
    return QUrl(QStringLiteral("https://cdn.discordapp.com/media/v1/collectibles-shop/%1/%2")
                    .arg(skuId, QLatin1String(format)));
}

QUrl badgeUrl(const Cosmetics::GuildTag &tag, int badgeSize)
{
    if (tag.badge.isEmpty())
        return {};
    // Discord asks for 16 for any chip up to 16, and more for the large ones.
    const int size = badgeSize <= 16 ? 16 : badgeSize <= 24 ? 24 : badgeSize <= 32 ? 32 : 40;
    return QUrl(QStringLiteral("https://cdn.discordapp.com/clan-badges/%1/%2.png?size=%3")
                    .arg(tag.guildId, tag.badge)
                    .arg(size));
}

// Three box blurs, close enough to the gaussian CSS uses for a glow.
QImage blurred(const QImage &source, int radius)
{
    if (radius < 1)
        return source;
    QImage image = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int w = image.width();
    const int h = image.height();
    QList<quint32> line(qMax(w, h));
    for (int pass = 0; pass < 3; ++pass) {
        for (int horizontal = 0; horizontal < 2; ++horizontal) {
            const int count = horizontal ? h : w;
            const int length = horizontal ? w : h;
            for (int i = 0; i < count; ++i) {
                for (int j = 0; j < length; ++j) {
                    const int x = horizontal ? j : i;
                    const int y = horizontal ? i : j;
                    line[j] = reinterpret_cast<const quint32 *>(image.constScanLine(y))[x];
                }
                int sum[4] = {0, 0, 0, 0};
                const int window = 2 * radius + 1;
                const auto add = [&sum](quint32 px, int sign) {
                    sum[0] += sign * int((px >> 24) & 0xff);
                    sum[1] += sign * int((px >> 16) & 0xff);
                    sum[2] += sign * int((px >> 8) & 0xff);
                    sum[3] += sign * int(px & 0xff);
                };
                for (int k = -radius; k <= radius; ++k)
                    add(line[std::clamp(k, 0, length - 1)], 1);
                for (int j = 0; j < length; ++j) {
                    const int x = horizontal ? j : i;
                    const int y = horizontal ? i : j;
                    reinterpret_cast<quint32 *>(image.scanLine(y))[x] =
                        (quint32(sum[0] / window) << 24) | (quint32(sum[1] / window) << 16)
                        | (quint32(sum[2] / window) << 8) | quint32(sum[3] / window);
                    add(line[std::clamp(j - radius, 0, length - 1)], -1);
                    add(line[std::clamp(j + radius + 1, 0, length - 1)], 1);
                }
            }
        }
    }
    return image;
}

// A path filled with one colour into an image the size of `area`, blurred:
// a CSS text-shadow.
QImage glowOf(const QPainterPath &path, const QRectF &area, const QColor &colour, int radius, qreal ratio)
{
    const QSize size((area.size() * ratio).toSize() + QSize(1, 1));
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(ratio, ratio);
        p.translate(-area.topLeft());
        p.fillPath(path, colour);
    }
    image = blurred(image, qMax(1, int(radius * ratio / 2)));
    image.setDevicePixelRatio(ratio);
    return image;
}

// Linear interpolation through keyframes (time 0..1, value).
template <typename T>
T keyed(qreal t, const QList<QPair<qreal, T>> &frames, T (*mix)(const T &, const T &, qreal))
{
    if (t <= frames.first().first)
        return frames.first().second;
    for (int i = 1; i < frames.size(); ++i) {
        if (t <= frames[i].first) {
            const qreal span = frames[i].first - frames[i - 1].first;
            const qreal f = span <= 0 ? 1 : (t - frames[i - 1].first) / span;
            return mix(frames[i - 1].second, frames[i].second, f);
        }
    }
    return frames.last().second;
}

QColor mixColour(const QColor &a, const QColor &b, qreal f)
{
    return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * f), float(a.greenF() + (b.greenF() - a.greenF()) * f),
                            float(a.blueF() + (b.blueF() - a.blueF()) * f));
}

qreal mixReal(const qreal &a, const qreal &b, qreal f)
{
    return a + (b - a) * f;
}

QByteArray unpackWoff2(const QByteArray &woff2)
{
#ifdef Q_OS_WIN
    IDWriteFactory5 *factory = nullptr;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5),
                                   reinterpret_cast<IUnknown **>(&factory)))
        || !factory)
        return {};
    QByteArray out;
    IDWriteFontFileStream *stream = nullptr;
    if (SUCCEEDED(factory->UnpackFontFile(DWRITE_CONTAINER_TYPE_WOFF2, woff2.constData(), UINT32(woff2.size()),
                                          &stream))
        && stream) {
        UINT64 size = 0;
        if (SUCCEEDED(stream->GetFileSize(&size)) && size > 0 && size < 16 * 1024 * 1024) {
            const void *fragment = nullptr;
            void *context = nullptr;
            if (SUCCEEDED(stream->ReadFileFragment(&fragment, 0, size, &context))) {
                out = QByteArray(static_cast<const char *>(fragment), int(size));
                stream->ReleaseFileFragment(context);
            }
        }
        stream->Release();
    }
    factory->Release();
    return out;
#else
    Q_UNUSED(woff2)
    return {};
#endif
}

QString fontFolder()
{
    const QString path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/fonts");
    QDir().mkpath(path);
    return path;
}

} // namespace

LookPainter &LookPainter::instance()
{
    static LookPainter painter;
    return painter;
}

LookPainter::LookPainter()
{
    m_clock.start();
    connect(&MediaCache::instance(), &MediaCache::ready, this, [this](const QUrl &url) {
        const QString host = url.host();
        const QString path = url.path();
        if (host == QLatin1String("cdn.discordapp.com")
            && (path.startsWith(QLatin1String("/media/v1/collectibles-shop/"))
                || path.startsWith(QLatin1String("/clan-badges/"))
                || path.startsWith(QLatin1String("/avatar-decoration-presets/"))))
            emit changed();
    });

    // Players nobody has asked for a frame lately stop, and later go.
    auto *prune = new QTimer(this);
    prune->setInterval(1500);
    connect(prune, &QTimer::timeout, this, [this]() {
        const qint64 at = now();
        for (auto it = m_players.begin(); it != m_players.end();) {
            const qint64 last = it.value() ? it.value()->property("lastUse").toLongLong() : 0;
            if (!it.value() || at - last > 30000) {
                if (it.value())
                    it.value()->deleteLater();
                it = m_players.erase(it);
                continue;
            }
            if (at - last > 1000)
                it.value()->setPlaying(false);
            ++it;
        }
    });
    prune->start();
}

PersonLook LookPainter::look(const QString &userId, const QString &guildId) const
{
    return m_store ? m_store->look(userId, guildId) : PersonLook();
}

// --- fonts --------------------------------------------------------------

const LookPainter::Font *LookPainter::fontInfo(int fontId)
{
    // Discord's display name fonts and the files its client loads them
    // from (font_id -> @font-face, build 643186). Ids it has but does not
    // map to a font draw in its ordinary one, and so do they here.
    static const QHash<int, Font> fonts{
        {3, {"Sakura", "33d4f12a85e1f736.woff2", 0.04, 400}},
        {4, {"Jellybean", "e6f5f44abb520735.woff2", 0, 400}},
        {6, {"Modern", "c560709c3470bb66.woff2", 0.01, 500}},
        {7, {"Medieval", "52b541f86401a5b6.woff2", 0.02, 400}},
        {8, {"8Bit", "69c735ca5c604de7.woff2", 0.02, 400}},
        {10, {"Vampyre", "8f20cb550d739cea.woff2", 0.01, 400}},
        {12, {"Tempo", "27b34a77ba5d693e.woff2", 0.03, 600}},
        {13, {"Monkey Bars", "45ef50dce38a931a.woff2", 0, 700}},
        {14, {"Mainframe", "4841dace333e9054.woff2", 0.03, 700}},
        {15, {"Headbang", "c00df4f25667809b.woff2", 0, 400}},
        {16, {"Journal", "aa59ae54ce7e540a.woff2", 0, 700}},
    };
    const auto it = fonts.constFind(fontId);
    return it == fonts.constEnd() ? nullptr : &it.value();
}

void LookPainter::loadFont(int fontId) const
{
    const Font *info = fontInfo(fontId);
    if (!info || m_fontsAsked.contains(fontId))
        return;
    m_fontsAsked.insert(fontId);
    auto *self = const_cast<LookPainter *>(this);

    // Kept unpacked after the first time.
    const QString kept = fontFolder() + QLatin1Char('/') + QFileInfo(QLatin1String(info->file)).completeBaseName()
        + QStringLiteral(".ttf");
    QFile cached(kept);
    if (cached.open(QIODevice::ReadOnly)) {
        const QByteArray ttf = cached.readAll();
        const int id = QFontDatabase::addApplicationFontFromData(ttf);
        if (id >= 0 && !QFontDatabase::applicationFontFamilies(id).isEmpty()) {
            self->m_families.insert(fontId, QFontDatabase::applicationFontFamilies(id).first());
            return;
        }
    }

    QNetworkRequest request(QUrl(QStringLiteral("https://discord.com/assets/%1").arg(QLatin1String(info->file))));
    request.setHeader(QNetworkRequest::UserAgentHeader, DiscordIdentity::userAgent());
    QNetworkReply *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, self, [self, reply, fontId]() {
        reply->deleteLater();
        const QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError || !body.startsWith("wOF2")) {
            wlog(QStringLiteral("looks"), QStringLiteral("name font %1 could not be fetched: %2")
                                              .arg(fontId)
                                              .arg(reply->errorString()));
            return;
        }
        self->installFont(fontId, body);
    });
}

void LookPainter::installFont(int fontId, const QByteArray &woff2)
{
    const Font *info = fontInfo(fontId);
    // Qt reads no WOFF2. Windows' own DirectWrite unpacks it to the OpenType
    // font inside, which Qt reads.
    const QByteArray ttf = unpackWoff2(woff2);
    const int id = ttf.isEmpty() ? -1 : QFontDatabase::addApplicationFontFromData(ttf);
    if (id < 0 || QFontDatabase::applicationFontFamilies(id).isEmpty()) {
        wlog(QStringLiteral("looks"), QStringLiteral("name font %1 could not be read").arg(fontId));
        return;
    }
    QSaveFile out(fontFolder() + QLatin1Char('/') + QFileInfo(QLatin1String(info->file)).completeBaseName()
                  + QStringLiteral(".ttf"));
    if (out.open(QIODevice::WriteOnly)) {
        out.write(ttf);
        out.commit();
    }
    m_families.insert(fontId, QFontDatabase::applicationFontFamilies(id).first());
    wlog(QStringLiteral("looks"), QStringLiteral("name font %1 ready: %2").arg(fontId).arg(m_families.value(fontId)));
    emit changed();
}

QFont LookPainter::fontFor(const QFont &base, int fontId) const
{
    const Font *info = fontInfo(fontId);
    if (!info)
        return base;
    loadFont(fontId);
    const QString family = m_families.value(fontId);
    if (family.isEmpty())
        return base;
    QFont font = base;
    font.setFamilies({family});
    // The one weight each is made in. Asking for another makes a fake bold,
    // which Discord turns off (font-synthesis: none). A letter the font does
    // not have - these are cut down to Latin, and some have no "@" or even no
    // space - comes from the ordinary font, as the browser's fallback does.
    font.setWeight(QFont::Weight(info->weight));
    const qreal px = base.pixelSize() > 0 ? base.pixelSize() : base.pointSizeF() * 96.0 / 72.0;
    if (info->letterSpacing > 0)
        font.setLetterSpacing(QFont::AbsoluteSpacing, info->letterSpacing * px);
    return font;
}

// --- pictures -------------------------------------------------------------

QImage LookPainter::picture(const QUrl &url) const
{
    return url.isEmpty() ? QImage() : MediaCache::instance().image(url);
}

QImage LookPainter::frame(const QUrl &url)
{
    if (url.isEmpty())
        return {};
    const QString key = url.toString();
    QPointer<AnimatedImage> &player = m_players[key];
    if (!player) {
        const QByteArray bytes = MediaCache::instance().animationData(url);
        if (bytes.isEmpty()) {
            m_players.remove(key);
            return {};
        }
        player = new AnimatedImage(this);
        if (!player->setData(bytes) || !player->isAnimated()) {
            player->deleteLater();
            m_players.remove(key);
            return {};
        }
    }
    player->setProperty("lastUse", now());
    player->setPlaying(true);
    return player->currentFrame();
}

// --- nameplates ---------------------------------------------------------------

void LookPainter::paintNameplate(QPainter *painter, const QRect &row, const Cosmetics::Nameplate &plate,
                                 int contentRight, Placement placement, bool hovered, bool selected, qreal radius)
{
    if (!plate.isSet() || row.isEmpty())
        return;

    painter->save();
    QPainterPath clip;
    clip.addRoundedRect(QRectF(row), radius, radius);
    painter->setClipPath(clip, Qt::IntersectClip);

    // The wash: the palette's colour, strongest at the right edge.
    const QColor colour = paletteColour(plate.palette);
    if (colour.isValid()) {
        QLinearGradient wash(row.topLeft(), row.topRight());
        if (placement == Placement::Account) {
            wash.setColorAt(0, withAlpha(colour, 0x1a));
            wash.setColorAt(1, withAlpha(colour, 0x66));
        } else {
            const int edge = selected ? 0x80 : (hovered && placement == Placement::MemberList) ? 0x4d : 0x33;
            wash.setColorAt(0, withAlpha(colour, 0));
            wash.setColorAt(0.2, withAlpha(colour, 0x14));
            wash.setColorAt(0.5, withAlpha(colour, 0x14));
            wash.setColorAt(1, withAlpha(colour, edge));
        }
        painter->fillRect(row, wash);
    }

    // The picture: as tall as the row, against its right edge. It moves
    // while the row is hovered or chosen, as Discord's does.
    QImage image;
    if (hovered || selected)
        image = frame(collectiblesUrl(plate.skuId, "animated"));
    if (image.isNull())
        image = picture(collectiblesUrl(plate.skuId, "static"));
    if (!image.isNull() && image.height() > 0) {
        qreal opacity = 0.6;
        switch (placement) {
        case Placement::Account: opacity = 0.4; break;
        case Placement::Channel: opacity = selected ? 0.9 : hovered ? 0.4 : 0.6; break;
        default: opacity = selected ? 0.9 : 0.6; break;
        }

        const qreal ratio = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
        const int height = row.height();
        const int width = qMax(1, qRound(qreal(image.width()) * height / image.height()));
        const QRect target(row.right() + 1 - width, row.top(), width, height);

        // Faint where the row's own content is, full beyond: Discord's mask.
        QImage layer = image.scaled(QSize(width, height) * ratio, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                           .convertToFormat(QImage::Format_ARGB32_Premultiplied);
        {
            QPainter mask(&layer);
            mask.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            const qreal fadeStart = (row.left() + contentRight + (placement == Placement::MemberList ? 10 : -5)
                                     - target.left()) * ratio;
            QLinearGradient fade(QPointF(fadeStart, 0), QPointF(fadeStart + 50 * ratio, 0));
            fade.setColorAt(0, QColor(0, 0, 0, 77));
            fade.setColorAt(1, QColor(0, 0, 0, 255));
            mask.fillRect(layer.rect(), fade);
        }
        layer.setDevicePixelRatio(ratio);
        painter->setOpacity(painter->opacity() * opacity);
        painter->drawImage(target.topLeft(), layer);
    }
    painter->restore();
}

// --- decorations --------------------------------------------------------------

void LookPainter::paintDecoration(QPainter *painter, const QRect &avatar, const Cosmetics::Decoration &decoration,
                                  bool animate)
{
    if (!decoration.isSet())
        return;
    const QUrl url = MediaCache::decorationUrl(decoration.asset);
    QImage image;
    if (animate)
        image = frame(url);
    if (image.isNull())
        image = picture(url);
    if (image.isNull())
        return;
    // Discord's decoration is 1.2 times the picture, centred on it.
    const qreal size = avatar.width() * 1.2;
    const QRectF target(avatar.center().x() + 0.5 - size / 2, avatar.center().y() + 0.5 - size / 2, size, size);
    painter->save();
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter->drawImage(target, image);
    painter->restore();
}

// --- server tags ----------------------------------------------------------------

namespace {

QFont tagFont()
{
    QFont font;
    font.setPixelSize(12);
    font.setWeight(QFont::DemiBold);
    return font;
}

} // namespace

int LookPainter::tagWidth(const Cosmetics::GuildTag &tag, int badgeSize) const
{
    if (!tag.isSet())
        return 0;
    const QFontMetricsF metrics(tagFont());
    const qreal text = std::min<qreal>(70, metrics.horizontalAdvance(tag.tag));
    return qCeil(4 + (tag.badge.isEmpty() ? 0 : badgeSize + 2) + text + 4);
}

int LookPainter::paintTag(QPainter *painter, const QPoint &left, const Cosmetics::GuildTag &tag, int badgeSize)
{
    if (!tag.isSet())
        return 0;
    const int width = tagWidth(tag, badgeSize);
    const QRectF chip(left.x(), left.y() - 8, width, 16);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter->setPen(Qt::NoPen);
    // Discord's --background-mod-strong on its dark theme.
    painter->setBrush(QColor(255, 255, 255, 30));
    painter->drawRoundedRect(chip, 4, 4);

    qreal x = chip.left() + 4;
    if (!tag.badge.isEmpty()) {
        const QImage badge = picture(badgeUrl(tag, badgeSize));
        if (!badge.isNull())
            painter->drawImage(QRectF(x, chip.center().y() - badgeSize / 2.0, badgeSize, badgeSize), badge);
        x += badgeSize + 2;
    }
    const QFont font = tagFont();
    painter->setFont(font);
    painter->setPen(QColor(Theme::TextPrimary));
    const QFontMetricsF metrics(font);
    const QString text = metrics.elidedText(tag.tag, Qt::ElideRight, 70);
    painter->drawText(QRectF(x, chip.top(), chip.right() - x, chip.height()), Qt::AlignLeft | Qt::AlignVCenter, text);
    painter->restore();
    return width;
}

QImage LookPainter::renderTag(const Cosmetics::GuildTag &tag, int badgeSize, qreal ratio)
{
    const int width = tagWidth(tag, badgeSize);
    if (width <= 0)
        return {};
    QImage image(QSize(width, 16) * ratio, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(ratio);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    paintTag(&painter, QPoint(0, 8), tag, badgeSize);
    return image;
}

// --- names ----------------------------------------------------------------------

QList<QColor> LookPainter::styleColours(const Cosmetics::NameStyle &style) const
{
    const int effect = style.effectId == 0 ? int(Cosmetics::Solid) : style.effectId;
    QList<quint32> raw = style.colors;
    if (raw.isEmpty())
        raw = defaultColours(effect);
    const QColor background = effect == Cosmetics::Toon ? QColor(0x33, 0x33, 0x33) : QColor(Theme::SurfaceChat);
    QList<QColor> out;
    for (quint32 value : std::as_const(raw))
        out.append(readable(rgb(value), background, minContrast(effect)));
    if (out.isEmpty())
        out.append(QColor(Theme::TextPrimary));
    return out;
}

LookPainter::Name LookPainter::nameFor(const QString &text, const QFont &font, const PersonLook &look,
                                       EffectMode mode, const QColor &fallback) const
{
    Name name;
    name.text = text;
    name.font = font;
    name.style = look.nameStyle;
    name.mode = mode;
    name.roleColour = look.roleColour;
    name.roleColours = look.roleColours;
    name.roleGradient = look.roleGradient;
    name.fallback = fallback;
    return name;
}

bool LookPainter::moving(const Name &name)
{
    if (name.mode == EffectMode::Animated && name.style.effectId != 0 && name.style.effectId != Cosmetics::Solid
        && name.style.effectId != Cosmetics::Gradient && name.style.effectId != Cosmetics::Glow)
        return true;
    return name.mode == EffectMode::FontOnly && name.roleGradient && name.animateRole;
}

int LookPainter::nameWidth(const Name &name) const
{
    const QFont font = name.style.fontId ? fontFor(name.font, name.style.fontId) : name.font;
    return qCeil(QFontMetricsF(font).horizontalAdvance(name.text));
}

int LookPainter::paintName(QPainter *painter, const QRect &box, const Name &name)
{
    const QFont font = name.style.fontId ? fontFor(name.font, name.style.fontId) : name.font;
    const QFontMetricsF metrics(font);
    const QString text = metrics.elidedText(name.text, Qt::ElideRight, box.width());
    const qreal width = metrics.horizontalAdvance(text);
    const QPointF baseline(box.left(), box.center().y() + 0.5 + (metrics.ascent() - metrics.descent()) / 2);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::TextAntialiasing, true);

    const bool effect = (name.mode == EffectMode::Static || name.mode == EffectMode::Animated)
        && name.style.isSet() && (name.style.effectId != 0 || !name.style.colors.isEmpty());

    if (effect) {
        paintEffect(painter, baseline, font, name, text);
    } else if (name.mode == EffectMode::FontOnly && name.roleGradient) {
        // A gradient role: its colours across the name, repeating every 100
        // pixels, sliding along while hovered, with a soft glow under it.
        QPainterPath path;
        path.addText(baseline, font, text);
        const qreal shift = name.animateRole ? (now() % 1500) / 1500.0 * 100.0 : 0.0;
        QLinearGradient gradient(QPointF(box.left() + shift, 0), QPointF(box.left() + shift + 100, 0));
        gradient.setSpread(QGradient::RepeatSpread);
        const QColor c1 = rgb(name.roleColours.primary);
        const QColor c2 = rgb(name.roleColours.secondary);
        if (name.roleColours.hasTertiary) {
            gradient.setColorAt(0, c1);
            gradient.setColorAt(1.0 / 3, c2);
            gradient.setColorAt(2.0 / 3, rgb(name.roleColours.tertiary));
            gradient.setColorAt(1, c1);
        } else {
            gradient.setColorAt(0, c1);
            gradient.setColorAt(0.5, c2);
            gradient.setColorAt(1, c1);
        }
        if (name.animateRole) {
            const qreal ratio = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
            const QRectF area = path.boundingRect().adjusted(-8, -8, 8, 8);
            QImage glow(QSize(qCeil(area.width() * ratio), qCeil(area.height() * ratio)), QImage::Format_ARGB32_Premultiplied);
            glow.fill(Qt::transparent);
            {
                QPainter g(&glow);
                g.setRenderHint(QPainter::Antialiasing);
                g.scale(ratio, ratio);
                g.translate(-area.topLeft());
                g.fillPath(path, gradient);
            }
            glow = blurred(glow, qMax(1, int(2 * ratio)));
            glow.setDevicePixelRatio(ratio);
            painter->setOpacity(painter->opacity() * 0.7);
            painter->drawImage(area.topLeft(), glow);
            painter->setOpacity(painter->opacity() / 0.7);
        }
        painter->fillPath(path, gradient);
    } else {
        QColor colour = name.fallback.isValid() ? name.fallback : QColor(Theme::TextPrimary);
        if (name.mode == EffectMode::FontOnly && name.roleColour != 0)
            colour = rgb(name.roleColour);
        painter->setFont(font);
        painter->setPen(colour);
        painter->drawText(baseline, text);
    }
    painter->restore();
    return qCeil(width);
}

void LookPainter::paintEffect(QPainter *painter, const QPointF &baseline, const QFont &font, const Name &name,
                              const QString &text)
{
    const QList<QColor> colours = styleColours(name.style);
    const Shades shade = shadesOf(colours.first());
    const int effect = name.style.effectId == 0 ? int(Cosmetics::Solid) : name.style.effectId;
    const bool animated = name.mode == EffectMode::Animated;
    const QFontMetricsF metrics(font);
    const qreal em = font.pixelSize() > 0 ? font.pixelSize() : font.pointSizeF() * 96.0 / 72.0;
    const QRectF box(baseline.x(), baseline.y() - metrics.ascent(), metrics.horizontalAdvance(text),
                     metrics.ascent() + metrics.descent());
    const qreal ratio = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
    const qreal t = animated ? now() : 0;

    QPainterPath path;
    path.addText(baseline, font, text);

    switch (effect) {
    case Cosmetics::Gradient:
    case Cosmetics::Glow: {
        // Corner to corner, the stops spread from 10% to 90%.
        QLinearGradient gradient(box.topLeft(), box.bottomRight());
        if (colours.size() == 1) {
            gradient.setColorAt(0, colours.first());
            gradient.setColorAt(1, colours.first());
        } else {
            for (int i = 0; i < colours.size(); ++i)
                gradient.setColorAt(0.1 + 0.8 * i / (colours.size() - 1), colours.at(i));
            gradient.setColorAt(0, colours.first());
            gradient.setColorAt(1, colours.last());
        }
        painter->fillPath(path, gradient);
        break;
    }
    case Cosmetics::Prism: {
        // Bands of every colour across the name, scrolling left while it
        // plays: one full cycle per name width, or 38 pixels at least.
        const qreal cycle = std::max<qreal>(38.0, text.size() * metrics.horizontalAdvance(QLatin1Char('0')));
        const qreal shift = animated ? -std::fmod(t, 2000.0) / 2000.0 * cycle : 0.0;
        QLinearGradient gradient(QPointF(box.left() + shift, 0), QPointF(box.left() + shift + cycle, 0));
        gradient.setSpread(QGradient::RepeatSpread);
        for (int i = 0; i <= colours.size(); ++i)
            gradient.setColorAt(qreal(i) / colours.size(), colours.at(i % colours.size()));
        painter->fillPath(path, gradient);
        break;
    }
    case Cosmetics::Neon: {
        // White letters with a thin outline in the colour, glowing.
        const qreal glowRadius = 4 + 0.12 * em;
        const QRectF area = box.adjusted(-glowRadius * 2, -glowRadius * 2, glowRadius * 2, glowRadius * 2);
        const QImage glow = glowOf(path, area, shade.main, int(glowRadius), ratio);
        painter->drawImage(area.topLeft(), glow);
        painter->strokePath(path, QPen(shade.neonStroke, 1 + 0.04 * em, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QColor fill(Qt::white);
        if (animated) {
            float h, s, l, a;
            shade.main.getHslF(&h, &s, &l, &a);
            const QColor flicker = QColor::fromHslF(h < 0 ? 0 : h, std::min(1.0f, s * 1.1f + 0.1f), 0.85f);
            const qreal p = std::fmod(t, 4000.0) / 4000.0;
            const QColor w(Qt::white);
            fill = keyed<QColor>(p,
                                 {{0, w}, {0.15, w}, {0.16, flicker}, {0.18, w}, {0.20, w}, {0.22, flicker},
                                  {0.23, w}, {0.25, w}, {0.28, flicker}, {0.50, w}, {1, w}},
                                 mixColour);
        }
        painter->fillPath(path, fill);
        break;
    }
    case Cosmetics::Toon: {
        // A dark outline, and the letters filled with bands of the colour's
        // lighter shades, which roll down while it plays.
        const qreal stroke = 1.6 + 0.04 * em;
        painter->strokePath(path, QPen(shade.toonStroke, stroke * 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->fillPath(path, shade.toonStroke);
        const qreal span = box.height() * 4;
        qreal position = 0;
        if (animated) {
            const qreal p = std::fmod(t, 4000.0) / 4000.0;
            position = keyed<qreal>(p, {{0, 0}, {0.05, 0}, {0.55, 1}, {1, 1}}, mixReal);
        }
        const qreal top = box.top() - position * (span - box.height());
        QLinearGradient bands(QPointF(0, top), QPointF(0, top + span));
        const QColor white(Qt::white);
        bands.setColorAt(0, white);
        bands.setColorAt(0.08, shade.light2);
        bands.setColorAt(0.15, shade.light1);
        bands.setColorAt(0.25, shade.main);
        bands.setColorAt(0.45, shade.light2);
        bands.setColorAt(0.55, shade.main);
        bands.setColorAt(0.75, white);
        bands.setColorAt(0.83, shade.light2);
        bands.setColorAt(0.90, shade.light1);
        bands.setColorAt(1, shade.main);
        painter->fillPath(path, bands);
        break;
    }
    case Cosmetics::Pop: {
        // White letters outlined in the darkest shade, sitting on a coloured
        // copy of themselves a little lower down, bouncing while it plays.
        const qreal stroke = 1.2 + 0.04 * em;
        qreal mainY = 0;
        qreal shadowY = 0.08 * em;
        if (animated) {
            const qreal p = std::fmod(t, 4000.0) / 4000.0;
            mainY = keyed<qreal>(p, {{0, 0}, {0.18, -0.05 * em}, {0.35, 0.08 * em}, {0.5, 0}, {1, 0}}, mixReal);
            shadowY = keyed<qreal>(p, {{0, 0.08 * em}, {0.18, 0.13 * em}, {0.35, 0}, {0.5, 0.08 * em}, {1, 0.08 * em}},
                                   mixReal);
        }
        QPainterPath shadow = path.translated(0, shadowY + stroke / 2);
        QLinearGradient shine(box.topRight(), box.bottomLeft());
        shine.setColorAt(0, shade.light1);
        shine.setColorAt(0.12, shade.light1);
        shine.setColorAt(0.4, shade.main);
        shine.setColorAt(1, shade.main);
        painter->strokePath(shadow, QPen(QBrush(shine), stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->fillPath(shadow, shine);
        const QPainterPath letters = path.translated(0, mainY);
        painter->strokePath(letters, QPen(shade.dark2, stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->fillPath(letters, Qt::white);
        break;
    }
    case Cosmetics::Gummy: {
        // Each letter its own colour, in turn; squashing one after another
        // while it plays.
        qreal x = baseline.x();
        int index = 0;
        for (int i = 0; i < text.size(); ++i) {
            QString letter = text.mid(i, 1);
            if (text.at(i).isHighSurrogate() && i + 1 < text.size()) {
                letter = text.mid(i, 2);
                ++i;
            }
            const qreal advance = metrics.horizontalAdvance(letter);
            if (letter.trimmed().isEmpty()) {
                x += advance;
                continue;
            }
            QPainterPath glyph;
            glyph.addText(QPointF(x, baseline.y()), font, letter);
            painter->save();
            if (animated) {
                const qreal p = std::fmod(t - index * 50.0 + 400000.0, 4000.0) / 4000.0;
                const qreal sx = keyed<qreal>(p, {{0, 1}, {0.07, 1.28}, {0.14, 0.82}, {0.21, 1.1}, {0.28, 0.96}, {0.35, 1}, {1, 1}},
                                              mixReal);
                const qreal sy = keyed<qreal>(p, {{0, 1}, {0.07, 0.72}, {0.14, 1.18}, {0.21, 0.9}, {0.28, 1.04}, {0.35, 1}, {1, 1}},
                                              mixReal);
                const QPointF anchor(x + advance / 2, baseline.y());
                painter->translate(anchor);
                painter->scale(sx, sy);
                painter->translate(-anchor);
            }
            painter->fillPath(glyph, colours.at(index % colours.size()));
            painter->restore();
            x += advance;
            ++index;
        }
        break;
    }
    case Cosmetics::Solid:
    default:
        painter->fillPath(path, colours.first());
        break;
    }
}

QImage LookPainter::renderName(const Name &name, qreal ratio, int pad)
{
    const QFont font = name.style.fontId ? fontFor(name.font, name.style.fontId) : name.font;
    const QFontMetricsF metrics(font);
    const int width = qCeil(metrics.horizontalAdvance(name.text)) + 2 * pad;
    const int height = qCeil(metrics.height()) + 2 * pad;
    QImage image(QSize(width, height) * ratio, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(ratio);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    paintName(&painter, QRect(pad, 0, width - 2 * pad, height), name);
    return image;
}
