#include "ui/ProfileArt.h"

#include "core/DiscordIdentity.h"
#include "core/Logger.h"
#include "ui/ApngDecoder.h"

#include <QApplication>
#include <QBuffer>
#include <QCursor>
#include <QEvent>
#include <QImageReader>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QThreadPool>
#include <QtMath>

#include <algorithm>
#include <functional>

namespace {

// The profile card's corner, #LeftCard in the profile window.
constexpr qreal CardRadius = 12;

struct Picture
{
    QList<QImage> frames;
    QList<int> delays;
};

using PictureHandler = std::function<void(const Picture &)>;

// The CDN's pictures, from disk when they were fetched before: an effect is a
// few megabytes, and the same people's profiles are opened again and again.
QNetworkAccessManager &network()
{
    static QNetworkAccessManager *manager = [] {
        auto *made = new QNetworkAccessManager(qApp);
        auto *cache = new QNetworkDiskCache(made);
        cache->setCacheDirectory(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                                 + QStringLiteral("/profile-art"));
        cache->setMaximumCacheSize(256LL * 1024 * 1024);
        made->setCache(cache);
        return made;
    }();
    return *manager;
}

// One thread to unpack on. An effect's frames are unpacked at full size
// before they are shrunk, about sixty megabytes for a big one, so one at a
// time.
QThreadPool &unpackers()
{
    static QThreadPool *pool = [] {
        auto *made = new QThreadPool(qApp);
        made->setMaxThreadCount(1);
        return made;
    }();
    return *pool;
}

// No wider than `width` (never wider than it came), and no taller than
// `tallest` times its width: rows below that would fall off the card.
QImage fit(QImage image, int width, qreal tallest)
{
    image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (width > 0 && image.width() > width)
        image = image.scaled(width, qMax(1, qRound(qreal(image.height()) * width / image.width())),
                             Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const int rows = tallest > 0 ? qCeil(tallest * image.width()) : 0;
    if (rows > 0 && image.height() > rows)
        image = image.copy(0, 0, image.width(), rows);
    return image;
}

Picture unpack(const QByteArray &bytes, int width, qreal tallest)
{
    Picture out;
    // Effects are animated PNGs, which Qt has no reader for.
    if (Apng::isAnimated(bytes)) {
        for (const Apng::Frame &frame : Apng::decode(bytes)) {
            out.frames.append(fit(frame.image, width, tallest));
            out.delays.append(qMax(10, frame.delayMs));
        }
        if (!out.frames.isEmpty())
            return out;
    }
    // Anything else Qt reads, moving or still.
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    while (out.frames.size() < 1000) {
        const QImage image = reader.read();
        if (image.isNull())
            break;
        out.frames.append(fit(image, width, tallest));
        out.delays.append(qMax(10, reader.nextImageDelay()));
        if (!reader.supportsAnimation() || !reader.canRead())
            break;
    }
    return out;
}

QHash<QString, QList<QPair<QPointer<QObject>, PictureHandler>>> &waiting()
{
    static QHash<QString, QList<QPair<QPointer<QObject>, PictureHandler>>> handlers;
    return handlers;
}

void deliver(const QString &key, const Picture &picture)
{
    const auto handlers = waiting().take(key);
    for (const auto &[context, handler] : handlers) {
        if (context)
            handler(picture);
    }
}

// Fetches a picture and unpacks it off the window's thread, then hands it
// over on the window's thread while `context` is still there. Two views
// asking for the same picture share one fetch.
void loadPicture(const QUrl &url, int width, qreal tallest, QObject *context, PictureHandler done)
{
    const QString key = QStringLiteral("%1 %2 %3").arg(url.toString()).arg(width).arg(tallest);
    const bool asked = waiting().contains(key);
    waiting()[key].append({QPointer<QObject>(context), std::move(done)});
    if (asked)
        return;

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, DiscordIdentity::userAgent());
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
    QNetworkReply *reply = network().get(request);
    QObject::connect(reply, &QNetworkReply::finished, qApp, [reply, key, width, tallest]() {
        reply->deleteLater();
        const QByteArray bytes = reply->readAll();
        if (reply->error() != QNetworkReply::NoError || bytes.isEmpty()) {
            wlog(QStringLiteral("looks"), QStringLiteral("profile art %1 could not be fetched: %2")
                                              .arg(reply->url().toString(), reply->errorString()));
            deliver(key, {});
            return;
        }
        unpackers().start([bytes, key, width, tallest]() {
            const Picture picture = unpack(bytes, width, tallest);
            QMetaObject::invokeMethod(qApp, [key, picture]() { deliver(key, picture); }, Qt::QueuedConnection);
        });
    });
}

} // namespace

// --- profile effects ---------------------------------------------------------

ProfileEffectView::ProfileEffectView(QWidget *card)
    : QWidget(card)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setGeometry(card->rect());
    card->installEventFilter(this);

    // Effects run at about twelve frames a second; this looks often and only
    // paints when a frame has changed.
    m_timer.setInterval(30);
    connect(&m_timer, &QTimer::timeout, this, [this]() { tick(); });
}

void ProfileEffectView::setEffect(const Cosmetics::ProfileEffect &effect)
{
    ++m_generation;
    m_layers.clear();
    m_painted.clear();
    m_running = false;
    m_timer.stop();
    m_opacity = 1.0;
    update();
    if (!effect.isSet())
        return;

    // A layer with random pictures shows one of them, the same choice for
    // every such layer, as Discord picks.
    int choices = 0;
    for (const Cosmetics::EffectLayer &layer : effect.layers) {
        const int count = int(layer.randomSources.size());
        if (count > 0)
            choices = choices == 0 ? count : qMin(choices, count);
    }
    const int pick = choices > 0 ? int(QRandomGenerator::global()->bounded(choices)) : -1;

    for (const Cosmetics::EffectLayer &config : effect.layers) {
        Layer layer;
        layer.config = config;
        m_layers.append(layer);
    }

    const int generation = m_generation;
    const int across = qCeil(width() * devicePixelRatioF());
    for (int i = 0; i < m_layers.size(); ++i) {
        const Cosmetics::EffectLayer &config = m_layers.at(i).config;
        const QString src = pick >= 0 && !config.randomSources.isEmpty() ? config.randomSources.at(pick) : config.src;
        // Drawn as wide as the card; only what lands on the card is kept.
        const qreal tallest = width() > 0 ? qMax<qreal>(0.05, qreal(height() - config.y + 2) / width()) : 0;
        loadPicture(QUrl(src), across, tallest, this, [this, generation, i](const Picture &picture) {
            arrived(generation, i, picture.frames, picture.delays);
        });
    }
}

void ProfileEffectView::arrived(int generation, int index, const QList<QImage> &frames, const QList<int> &delays)
{
    if (generation != m_generation || index < 0 || index >= m_layers.size())
        return;
    Layer &layer = m_layers[index];
    layer.frames = frames;
    layer.ends.clear();
    int at = 0;
    for (int delay : delays) {
        at += delay;
        layer.ends.append(at);
    }
    layer.arrived = true;
    for (const Layer &each : std::as_const(m_layers)) {
        if (!each.arrived)
            return;
    }

    // Every picture is in: it begins.
    m_running = true;
    m_clock.start();
    if (isVisible())
        m_timer.start();
    update();
}

int ProfileEffectView::frameAt(const Layer &layer, qint64 ms) const
{
    // Discord holds the start back half a second.
    const qint64 t = ms - 500;
    const Cosmetics::EffectLayer &config = layer.config;
    if (layer.frames.isEmpty() || layer.ends.isEmpty() || t < config.start)
        return -1;
    qint64 local = t - config.start;
    if (!config.loop && local > config.duration)
        return -1;
    // A loop with a pause: shown for its duration, hidden for the pause,
    // and started again from its first frame.
    if (config.loop && config.loopDelay > 0) {
        local %= qint64(config.duration) + config.loopDelay;
        if (local > config.duration)
            return -1;
    }
    // The picture itself runs round, as a browser plays it.
    const int total = layer.ends.last();
    if (total > 0)
        local %= total;
    const auto next = std::upper_bound(layer.ends.cbegin(), layer.ends.cend(), int(local));
    return qMin(int(next - layer.ends.cbegin()), int(layer.frames.size()) - 1);
}

void ProfileEffectView::tick()
{
    if (!m_running)
        return;
    bool changed = false;

    // A third as strong while the pointer is on the card, easing there and
    // back over 0.6 seconds, as Discord's does.
    const QWidget *card = parentWidget();
    const bool over = card && card->rect().contains(card->mapFromGlobal(QCursor::pos()));
    const qreal target = over ? 0.35 : 1.0;
    if (m_opacity != target) {
        const qreal step = (1.0 - 0.35) * m_timer.interval() / 600.0;
        m_opacity = m_opacity > target ? qMax(target, m_opacity - step) : qMin(target, m_opacity + step);
        changed = true;
    }

    const qint64 ms = m_clock.elapsed();
    for (int i = 0; i < m_layers.size() && !changed; ++i)
        changed = m_painted.value(i, -2) != frameAt(m_layers.at(i), ms);
    if (changed)
        update();
}

void ProfileEffectView::paintEvent(QPaintEvent *)
{
    m_painted.clear();
    if (!m_running)
        return;

    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setOpacity(m_opacity);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(rect()), CardRadius, CardRadius);
    painter.setClipPath(clip);

    const qint64 ms = m_clock.elapsed();
    for (const Layer &layer : std::as_const(m_layers)) {
        const int index = frameAt(layer, ms);
        m_painted.append(index);
        if (index < 0)
            continue;
        const QImage &frame = layer.frames.at(index);
        if (frame.width() <= 0)
            continue;
        // As wide as the card, where the layer says: Discord's "width: 100%".
        const qreal scale = qreal(width()) / frame.width();
        painter.drawImage(QRectF(layer.config.x, layer.config.y, width(), frame.height() * scale), frame);
    }
}

void ProfileEffectView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // Opened again: it plays from the start, as it does in Discord.
    if (m_running) {
        m_clock.start();
        m_timer.start();
    }
}

void ProfileEffectView::hideEvent(QHideEvent *event)
{
    m_timer.stop();
    QWidget::hideEvent(event);
}

bool ProfileEffectView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize)
        setGeometry(parentWidget()->rect());
    return QWidget::eventFilter(watched, event);
}

// --- profile frames --------------------------------------------------------

ProfileFrameView::ProfileFrameView(QWidget *card, QWidget *parent, bool front)
    : QWidget(parent)
    , m_card(card)
    , m_front(front)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    // The card moves inside whatever holds it when the frame makes room.
    card->installEventFilter(this);
    if (card->parentWidget() && card->parentWidget() != parent)
        card->parentWidget()->installEventFilter(this);
    hide();
}

QMargins ProfileFrameView::overflow(const Cosmetics::ProfileFrame &frame, int cardWidth)
{
    if (!frame.isSet() || cardWidth <= 0)
        return {};
    const qreal scale = qreal(cardWidth) / frame.innerWidth;
    const int side = qCeil(frame.overflowHorizontal * scale);
    return QMargins(side, qCeil(frame.overflowTop * scale), side, qCeil(frame.overflowBottom * scale));
}

void ProfileFrameView::setFrame(const Cosmetics::ProfileFrame &frame)
{
    ++m_generation;
    m_frame = frame;
    m_pictures.clear();
    if (!frame.isSet() || !m_card) {
        hide();
        return;
    }
    place();
    show();

    // Every layer is as wide as the frame, which is the card's width plus
    // the overflow either side.
    const int generation = m_generation;
    const int across = qCeil(width() * devicePixelRatioF());
    for (const Cosmetics::FrameLayer &layer : frame.layers) {
        if (layer.front != m_front)
            continue;
        const QUrl url(QStringLiteral("https://cdn.discordapp.com/media/v1/collectibles-shop/%1/%2/static")
                           .arg(frame.skuId, layer.id));
        const QString id = layer.id;
        loadPicture(url, across, 0, this, [this, generation, id](const Picture &picture) {
            if (generation != m_generation || picture.frames.isEmpty())
                return;
            m_pictures.insert(id, picture.frames.first());
            update();
        });
    }
}

void ProfileFrameView::place()
{
    if (!m_card || !parentWidget() || !m_frame.isSet())
        return;
    const QRect card(m_card->mapTo(parentWidget(), QPoint(0, 0)), m_card->size());
    setGeometry(card.marginsAdded(overflow(m_frame, m_card->width())));
    // In front of the card, or behind it.
    if (m_front)
        raise();
    else
        lower();
}

void ProfileFrameView::paintEvent(QPaintEvent *)
{
    if (!m_card || !m_frame.isSet())
        return;
    const QMargins out = overflow(m_frame, m_card->width());
    const QRectF card(out.left(), out.top(), m_card->width(), m_card->height());
    const qreal across = width();
    // A responsive rail shows only on a card at least 480 tall for 300 across.
    const bool tall = card.height() > 0 && card.width() / card.height() < 300.0 / 480.0;

    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    for (const Cosmetics::FrameLayer &layer : std::as_const(m_frame.layers)) {
        if (layer.front != m_front)
            continue;
        const QImage picture = m_pictures.value(layer.id);
        if (picture.isNull() || picture.width() <= 0)
            continue;
        const qreal down = across * picture.height() / picture.width();

        if (layer.type == QLatin1String("staple")) {
            // Whole: its top overflowTop above the card, its bottom
            // overflowBottom below it, or otherwise where the card starts.
            qreal top = card.top();
            if (layer.anchor == QLatin1String("top"))
                top = 0;
            else if (layer.anchor == QLatin1String("bottom"))
                top = height() - down;
            painter.drawImage(QRectF(0, top, across, down), picture);
            continue;
        }

        const bool rail = layer.type == QLatin1String("rail");
        if (!rail && layer.type != QLatin1String("border"))
            continue;
        if (rail && layer.responsive && !tall)
            continue;
        painter.save();
        painter.setClipRect(QRectF(0, card.top(), across, card.height()));
        if (rail) {
            // As tall as the card, its picture held to the anchor.
            qreal top = card.top();
            if (layer.anchor == QLatin1String("bottom"))
                top = card.bottom() - down;
            else if (layer.anchor == QLatin1String("center"))
                top = card.center().y() - down / 2;
            painter.drawImage(QRectF(0, top, across, down), picture);
        } else {
            // A border: its picture again and again down the card.
            for (qreal y = card.top(); down >= 1 && y < card.bottom(); y += down)
                painter.drawImage(QRectF(0, y, across, down), picture);
        }
        painter.restore();
    }
}

bool ProfileFrameView::eventFilter(QObject *watched, QEvent *event)
{
    const QEvent::Type type = event->type();
    if (type == QEvent::Move || type == QEvent::Resize || type == QEvent::Show)
        place();
    return QWidget::eventFilter(watched, event);
}
