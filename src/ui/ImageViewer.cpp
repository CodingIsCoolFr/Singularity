#include "ui/ImageViewer.h"

#include "core/AppConfig.h"
#include "core/DiscordIdentity.h"
#include "core/Logger.h"
#include "ui/AnimatedImage.h"
#include "ui/ClipPlayer.h"
#include "ui/MediaCache.h"
#include "ui/Theme.h"

#include <QAudioOutput>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMediaPlayer>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPushButton>
#include <QSaveFile>
#include <QScreen>
#include <QSlider>
#include <QStackedLayout>
#include <QVBoxLayout>
#include <QVideoWidget>
#include <QWheelEvent>

namespace {
constexpr qreal MinScale = 0.05;
constexpr qreal MaxScale = 20.0;
constexpr qreal WheelStep = 1.15;
}

// ---------------------------------------------------------------------------
// The part that draws, zooms and pans
// ---------------------------------------------------------------------------

class ImageViewer::Canvas : public QWidget
{
public:
    explicit Canvas(QWidget *parent = nullptr)
        : QWidget(parent)
        , m_source(new AnimatedImage(this))
    {
        setMouseTracking(true);
        setCursor(Qt::OpenHandCursor);
        connect(m_source, &AnimatedImage::frameChanged, this, [this]() { update(); });
    }

    void setImage(const QImage &still, const QByteArray &animation)
    {
        m_still = still;
        if (animation.isEmpty())
            m_source->clear();
        else
            m_source->setData(animation);

        m_fitted = false;
        fitToWindow();
    }

    // A picture from a playing clip. The first one sizes the view.
    void setVideoFrame(const QImage &frame)
    {
        const bool first = m_video.isNull();
        m_video = frame;
        if (first) {
            m_fitted = false;
            fitToWindow();
        } else {
            update();
        }
    }

    void clearVideo() { m_video = QImage(); }

    bool hasImage() const { return !currentFrame().isNull(); }
    QSize imageSize() const { return currentFrame().size(); }
    qreal scale() const { return m_scale; }

    void fitToWindow()
    {
        const QImage frame = currentFrame();
        if (frame.isNull() || frame.width() == 0 || frame.height() == 0)
            return;

        // Never blow a small picture up just to fill the window.
        const qreal byWidth = qreal(width() - 40) / frame.width();
        const qreal byHeight = qreal(height() - 40) / frame.height();
        m_scale = qBound(MinScale, qMin<qreal>(1.0, qMin(byWidth, byHeight)), MaxScale);

        m_offset = QPointF(0, 0);
        m_fitted = true;
        update();
        notifyZoom();
    }

    void setScaleAround(qreal newScale, const QPointF &anchor)
    {
        newScale = qBound(MinScale, newScale, MaxScale);
        if (qFuzzyCompare(newScale, m_scale))
            return;

        // Keep whatever sits under the pointer under the pointer.
        const QPointF centre(width() / 2.0, height() / 2.0);
        const QPointF before = (anchor - centre - m_offset) / m_scale;
        m_scale = newScale;
        m_offset = anchor - centre - before * m_scale;

        m_fitted = false;
        update();
        notifyZoom();
    }

    void toggleActualSize(const QPointF &anchor)
    {
        if (m_fitted || m_scale < 0.999)
            setScaleAround(1.0, anchor);
        else
            fitToWindow();
    }

    void zoomBy(qreal factor) { setScaleAround(m_scale * factor, QPointF(width() / 2.0, height() / 2.0)); }

    std::function<void()> onZoomChanged;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(0, 0, 0, 235));

        const QImage frame = currentFrame();
        if (frame.isNull()) {
            painter.setPen(QColor(Theme::TextMuted));
            painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("Loading..."));
            return;
        }

        painter.setRenderHint(QPainter::SmoothPixmapTransform, m_scale < 4.0);

        const QSizeF drawn(frame.width() * m_scale, frame.height() * m_scale);
        const QPointF topLeft((width() - drawn.width()) / 2.0 + m_offset.x(),
                              (height() - drawn.height()) / 2.0 + m_offset.y());
        painter.drawImage(QRectF(topLeft, drawn), frame);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        if (m_fitted)
            fitToWindow();
    }

    void wheelEvent(QWheelEvent *event) override
    {
        const int ticks = event->angleDelta().y();
        if (ticks == 0)
            return;
        setScaleAround(m_scale * (ticks > 0 ? WheelStep : 1.0 / WheelStep), event->position());
        event->accept();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton)
            return;
        m_dragging = true;
        m_dragFrom = event->position();
        setCursor(Qt::ClosedHandCursor);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_dragging)
            return;
        m_offset += event->position() - m_dragFrom;
        m_dragFrom = event->position();
        m_fitted = false;
        update();
    }

    void mouseReleaseEvent(QMouseEvent *) override
    {
        m_dragging = false;
        setCursor(Qt::OpenHandCursor);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        toggleActualSize(event->position());
    }

private:
    QImage currentFrame() const
    {
        if (!m_video.isNull())
            return m_video;
        const QImage frame = m_source->currentFrame();
        return frame.isNull() ? m_still : frame;
    }

    void notifyZoom()
    {
        if (onZoomChanged)
            onZoomChanged();
    }

    AnimatedImage *m_source = nullptr;
    QImage m_still;
    QImage m_video;
    qreal m_scale = 1.0;
    QPointF m_offset;
    QPointF m_dragFrom;
    bool m_dragging = false;
    bool m_fitted = true;
};

// ---------------------------------------------------------------------------
// The window round it
// ---------------------------------------------------------------------------

ImageViewer::ImageViewer(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Picture"));
    setModal(false);
    resize(1000, 760);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // The picture, or a playing video, in the same place.
    auto *stage = new QWidget(this);
    m_stack = new QStackedLayout(stage);
    m_stack->setContentsMargins(0, 0, 0, 0);
    m_canvas = new Canvas(stage);
    m_stack->addWidget(m_canvas);
    layout->addWidget(stage, 1);

    // A video's controls, over the usual bar. Hidden for a picture.
    m_videoBar = new QWidget(this);
    m_videoBar->setObjectName(QStringLiteral("ViewerBar"));
    m_videoBar->setFixedHeight(42);
    auto *videoLayout = new QHBoxLayout(m_videoBar);
    videoLayout->setContentsMargins(14, 6, 14, 6);
    videoLayout->setSpacing(10);

    m_playButton = new QPushButton(QStringLiteral("Pause"), m_videoBar);
    m_playButton->setFixedHeight(28);
    m_playButton->setFocusPolicy(Qt::NoFocus);
    connect(m_playButton, &QPushButton::clicked, this, &ImageViewer::togglePlay);
    videoLayout->addWidget(m_playButton);

    m_seek = new QSlider(Qt::Horizontal, m_videoBar);
    m_seek->setFocusPolicy(Qt::NoFocus);
    m_seek->setToolTip(QStringLiteral("Drag to skip through the video"));
    connect(m_seek, &QSlider::sliderMoved, this, [this](int ms) {
        if (m_player)
            m_player->setPosition(ms);
    });
    videoLayout->addWidget(m_seek, 1);

    m_timeLabel = new QLabel(QStringLiteral("0:00 / 0:00"), m_videoBar);
    m_timeLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;").arg(QLatin1String(Theme::TextMuted)));
    videoLayout->addWidget(m_timeLabel);

    auto *volumeLabel = new QLabel(QStringLiteral("Vol"), m_videoBar);
    volumeLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;").arg(QLatin1String(Theme::TextFaint)));
    videoLayout->addWidget(volumeLabel);

    m_volume = new QSlider(Qt::Horizontal, m_videoBar);
    m_volume->setRange(0, 100);
    m_volume->setValue(AppConfig::instance().value(QStringLiteral("viewer/volume"), 80).toInt());
    m_volume->setFixedWidth(110);
    m_volume->setFocusPolicy(Qt::NoFocus);
    connect(m_volume, &QSlider::valueChanged, this, [this](int value) {
        if (m_audio)
            m_audio->setVolume(value / 100.0f);
        AppConfig::instance().setValue(QStringLiteral("viewer/volume"), value);
    });
    videoLayout->addWidget(m_volume);

    m_videoBar->hide();
    layout->addWidget(m_videoBar);

    auto *bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("ViewerBar"));
    bar->setFixedHeight(46);

    auto *barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(14, 8, 14, 8);
    barLayout->setSpacing(8);

    m_nameLabel = new QLabel(bar);
    m_nameLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;")
                                   .arg(QLatin1String(Theme::TextFaint)));
    barLayout->addWidget(m_nameLabel, 1);

    // Zoom and fit belong to a picture. A video fits itself.
    m_pictureTools = new QWidget(bar);
    auto *toolsLayout = new QHBoxLayout(m_pictureTools);
    toolsLayout->setContentsMargins(0, 0, 0, 0);
    toolsLayout->setSpacing(8);
    barLayout->addWidget(m_pictureTools);

    auto *zoomOut = new QPushButton(QStringLiteral("−"), bar);
    zoomOut->setFixedSize(30, 28);
    // The app's button padding is wider than 30 pixels, which left these two
    // buttons blank.
    zoomOut->setStyleSheet(QStringLiteral("padding: 0; font-size: 16px;"));
    zoomOut->setToolTip(QStringLiteral("Zoom out"));
    connect(zoomOut, &QPushButton::clicked, this, [this]() { m_canvas->zoomBy(1.0 / WheelStep); });
    toolsLayout->addWidget(zoomOut);

    m_zoomLabel = new QLabel(bar);
    m_zoomLabel->setFixedWidth(52);
    m_zoomLabel->setAlignment(Qt::AlignCenter);
    m_zoomLabel->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(Theme::TextPrimary)));
    toolsLayout->addWidget(m_zoomLabel);

    auto *zoomIn = new QPushButton(QStringLiteral("+"), bar);
    zoomIn->setFixedSize(30, 28);
    zoomIn->setStyleSheet(QStringLiteral("padding: 0; font-size: 16px;"));
    zoomIn->setToolTip(QStringLiteral("Zoom in"));
    connect(zoomIn, &QPushButton::clicked, this, [this]() { m_canvas->zoomBy(WheelStep); });
    toolsLayout->addWidget(zoomIn);

    auto *fit = new QPushButton(QStringLiteral("Fit"), bar);
    fit->setFixedHeight(28);
    connect(fit, &QPushButton::clicked, this, [this]() { m_canvas->fitToWindow(); });
    toolsLayout->addWidget(fit);

    auto *actual = new QPushButton(QStringLiteral("100%"), bar);
    actual->setFixedHeight(28);
    connect(actual, &QPushButton::clicked, this, [this]() {
        m_canvas->setScaleAround(1.0, QPointF(m_canvas->width() / 2.0, m_canvas->height() / 2.0));
    });
    toolsLayout->addWidget(actual);

    auto *save = new QPushButton(QStringLiteral("Save"), bar);
    save->setFixedHeight(28);
    connect(save, &QPushButton::clicked, this, &ImageViewer::saveAs);
    barLayout->addWidget(save);

    auto *open = new QPushButton(QStringLiteral("Open in browser"), bar);
    open->setFixedHeight(28);
    connect(open, &QPushButton::clicked, this, [this]() { QDesktopServices::openUrl(m_url); });
    barLayout->addWidget(open);

    layout->addWidget(bar);

    m_canvas->onZoomChanged = [this]() { updateZoomLabel(); };

    setStyleSheet(QStringLiteral("QDialog { background-color: #000000; } "
                                 "#ViewerBar { background-color: %1; border-top: 1px solid %2; }")
                      .arg(QLatin1String(Theme::SurfaceRail), QLatin1String(Theme::Border)));

    // A late download replaces the placeholder.
    connect(&MediaCache::instance(), &MediaCache::ready, this, [this](const QUrl &url) {
        if (url == m_url && !m_clip && !playingVideo())
            showImage(url);
    });
}

void ImageViewer::presentWindow()
{
    if (!isVisible()) {
        if (parentWidget())
            move(parentWidget()->geometry().center() - rect().center());
        show();
    }
    raise();
    activateWindow();
}

bool ImageViewer::playingVideo() const
{
    return m_videoWidget && m_stack->currentWidget() == m_videoWidget;
}

void ImageViewer::showVideo(const QUrl &url, qint64 startMs)
{
    if (!MediaCache::isAllowedHost(url)) {
        wlog(QStringLiteral("viewer"), QStringLiteral("refusing %1: not a Discord host").arg(url.toString()));
        return;
    }

    delete m_clip;
    m_clip = nullptr;
    m_canvas->clearVideo();
    m_url = url;
    const QString name = QFileInfo(url.path()).fileName();
    m_nameLabel->setText(name.isEmpty() ? url.toString() : name);
    setWindowTitle(QStringLiteral("Video"));

    // Qt's own player, which plays sound, seeks and streams as it goes. The
    // chat's clip player is muted and loads the whole file first, which is
    // right for a GIF and wrong for something to watch. Its video widget draws
    // frames where they were decoded, so a frame made on the graphics card is
    // never copied back - the copy that once froze the window on AMD cards.
    if (!m_player) {
        m_videoWidget = new QVideoWidget(this);
        m_videoWidget->setAspectRatioMode(Qt::KeepAspectRatio);
        m_stack->addWidget(m_videoWidget);

        m_player = new QMediaPlayer(this);
        m_audio = new QAudioOutput(this);
        m_player->setAudioOutput(m_audio);
        m_player->setVideoOutput(m_videoWidget);

        connect(m_player, &QMediaPlayer::durationChanged, this, [this](qint64 ms) {
            m_seek->setRange(0, int(ms));
            updateTime();
        });
        connect(m_player, &QMediaPlayer::positionChanged, this, [this](qint64 ms) {
            if (!m_seek->isSliderDown())
                m_seek->setValue(int(ms));
            updateTime();
        });
        connect(m_player, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState state) {
            m_playButton->setText(state == QMediaPlayer::PlayingState ? QStringLiteral("Pause")
                                                                      : QStringLiteral("Play"));
        });
        connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
            if (m_pendingStartMs > 0 && (status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia)) {
                m_player->setPosition(m_pendingStartMs);
                m_pendingStartMs = 0;
            }
        });
        connect(m_player, &QMediaPlayer::errorOccurred, this,
                [this](QMediaPlayer::Error, const QString &reason) {
                    wlog(QStringLiteral("viewer"),
                         QStringLiteral("could not play %1: %2").arg(m_url.toString().left(200), reason));
                    m_nameLabel->setText(QStringLiteral("This video could not be played here: %1. "
                                                        "Open in browser plays it there.")
                                             .arg(reason));
                });
    }

    m_audio->setVolume(m_volume->value() / 100.0f);
    m_seek->setRange(0, 0);
    m_seek->setValue(0);
    m_stack->setCurrentWidget(m_videoWidget);
    m_pictureTools->hide();
    m_videoBar->show();
    updateTime();
    presentWindow();

    wlog(QStringLiteral("viewer"), QStringLiteral("playing %1").arg(name));
    // A position asked for before the file is open is ignored, so it waits
    // for the player to have it (mediaStatusChanged, above).
    m_pendingStartMs = qMax<qint64>(0, startMs);
    m_player->setSource(url);
    m_player->play();
}

void ImageViewer::stopVideo()
{
    if (m_player) {
        m_player->stop();
        m_player->setSource(QUrl());
    }
    m_stack->setCurrentWidget(m_canvas);
    m_videoBar->hide();
    m_pictureTools->show();
    setWindowTitle(QStringLiteral("Picture"));
}

void ImageViewer::togglePlay()
{
    if (!m_player)
        return;
    if (m_player->playbackState() == QMediaPlayer::PlayingState)
        m_player->pause();
    else
        m_player->play();
}

void ImageViewer::updateTime()
{
    const auto clock = [](qint64 ms) {
        const qint64 seconds = qMax<qint64>(0, ms / 1000);
        return seconds >= 3600
            ? QStringLiteral("%1:%2:%3").arg(seconds / 3600).arg((seconds / 60) % 60, 2, 10, QLatin1Char('0'))
                  .arg(seconds % 60, 2, 10, QLatin1Char('0'))
            : QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
    };
    const qint64 position = m_player ? m_player->position() : 0;
    const qint64 duration = m_player ? m_player->duration() : 0;
    m_timeLabel->setText(QStringLiteral("%1 / %2").arg(clock(position), clock(duration)));
}

void ImageViewer::saveVideo()
{
    QString suggested = QFileInfo(m_url.path()).fileName();
    if (suggested.isEmpty())
        suggested = QStringLiteral("video.mp4");
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save video"), suggested);
    if (path.isEmpty())
        return;

    // Written as it arrives rather than held whole in memory: a video can be
    // hundreds of megabytes.
    auto *file = new QSaveFile(path, this);
    if (!file->open(QIODevice::WriteOnly)) {
        m_nameLabel->setText(QStringLiteral("Could not write %1").arg(QFileInfo(path).fileName()));
        delete file;
        return;
    }
    if (!m_network)
        m_network = new QNetworkAccessManager(this);
    QNetworkRequest request(m_url);
    request.setHeader(QNetworkRequest::UserAgentHeader, DiscordIdentity::userAgent());
    QNetworkReply *reply = m_network->get(request);
    m_nameLabel->setText(QStringLiteral("Saving %1...").arg(QFileInfo(path).fileName()));
    connect(reply, &QNetworkReply::readyRead, file, [reply, file]() { file->write(reply->readAll()); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, file, path]() {
        reply->deleteLater();
        file->write(reply->readAll());
        const QString shown = QFileInfo(path).fileName();
        if (reply->error() == QNetworkReply::NoError && file->commit()) {
            m_nameLabel->setText(QStringLiteral("Saved %1").arg(shown));
        } else {
            file->cancelWriting();
            m_nameLabel->setText(QStringLiteral("Could not save %1: %2").arg(shown, reply->errorString()));
        }
        file->deleteLater();
    });
}

void ImageViewer::showImage(const QUrl &url)
{
    if (!MediaCache::isAllowedHost(url)) {
        wlog(QStringLiteral("viewer"), QStringLiteral("refusing %1: not a Discord host").arg(url.toString()));
        return;
    }

    if (playingVideo())
        stopVideo();
    m_url = url;

    const QString name = QFileInfo(url.path()).fileName();
    m_nameLabel->setText(name.isEmpty() ? url.toString() : name);

    // A gif from Tenor is really a small mp4. The cache cannot decode that as
    // a picture, so the viewer sat on "Loading..." for ever; it plays instead.
    delete m_clip;
    m_clip = nullptr;
    m_canvas->clearVideo();
    const QString path = url.path().toLower();
    if (path.endsWith(QLatin1String(".mp4")) || path.endsWith(QLatin1String(".webm"))
        || path.endsWith(QLatin1String(".mov"))) {
        m_clip = new ClipPlayer(url, QSize(1920, 1080), this);
        connect(m_clip, &ClipPlayer::frameReady, m_canvas, [this](const QImage &frame) {
            m_canvas->setVideoFrame(frame);
            updateZoomLabel();
        });
        connect(m_clip, &ClipPlayer::failed, this, [this](const QString &reason) {
            wlog(QStringLiteral("viewer"),
                 QStringLiteral("could not play %1: %2").arg(m_url.toString(), reason));
        });
        m_canvas->setImage(QImage(), QByteArray());
        updateZoomLabel();
        if (!isVisible()) {
            if (parentWidget())
                move(parentWidget()->geometry().center() - rect().center());
            show();
        }
        raise();
        activateWindow();
        return;
    }

    // Asking starts the download if it is not already here.
    //
    // Full quality, decoded for this window only. The message list is shown a
    // shrunk copy, because that is all it draws.
    const QImage still = MediaCache::instance().fullImage(url);
    m_canvas->setImage(still, MediaCache::instance().animationData(url));
    updateZoomLabel();

    if (!isVisible()) {
        if (parentWidget())
            move(parentWidget()->geometry().center() - rect().center());
        show();
    }
    raise();
    activateWindow();
}

void ImageViewer::updateZoomLabel()
{
    const int percent = qRound(m_canvas->scale() * 100.0);
    m_zoomLabel->setText(QStringLiteral("%1%").arg(percent));

    if (m_canvas->hasImage()) {
        const QSize size = m_canvas->imageSize();
        m_nameLabel->setToolTip(QStringLiteral("%1 by %2").arg(size.width()).arg(size.height()));
    }
}

void ImageViewer::saveAs()
{
    if (playingVideo()) {
        saveVideo();
        return;
    }

    const QByteArray animation = m_clip ? m_clip->data() : MediaCache::instance().animationData(m_url);
    // Saving hands over what arrived, not the copy made for the message list.
    const QImage still = MediaCache::instance().fullImage(m_url);
    if (animation.isEmpty() && still.isNull())
        return;

    QString suggested = QFileInfo(m_url.path()).fileName();
    if (suggested.isEmpty())
        suggested = QStringLiteral("picture.png");

    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save picture"), suggested);
    if (path.isEmpty())
        return;

    // An animation is written back byte for byte so it keeps moving. A still
    // picture is re-encoded from the decoded frame.
    if (!animation.isEmpty()) {
        QSaveFile file(path);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(animation);
            file.commit();
        }
        return;
    }

    still.save(path);
}

void ImageViewer::hideEvent(QHideEvent *event)
{
    // A closed viewer is only hidden; a clip left running would go on
    // decoding full size for nobody.
    delete m_clip;
    m_clip = nullptr;
    m_canvas->clearVideo();
    // Closing a video stops its sound with it.
    if (playingVideo())
        stopVideo();
    QDialog::hideEvent(event);
}

void ImageViewer::keyPressEvent(QKeyEvent *event)
{
    if (playingVideo()) {
        switch (event->key()) {
        case Qt::Key_Space:
        case Qt::Key_K:
            togglePlay();
            return;
        case Qt::Key_Left:
            m_player->setPosition(qMax<qint64>(0, m_player->position() - 5000));
            return;
        case Qt::Key_Right:
            m_player->setPosition(qMin(m_player->duration(), m_player->position() + 5000));
            return;
        case Qt::Key_Escape:
            close();
            return;
        default:
            QDialog::keyPressEvent(event);
            return;
        }
    }

    switch (event->key()) {
    case Qt::Key_Escape:
        close();
        return;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        m_canvas->zoomBy(WheelStep);
        return;
    case Qt::Key_Minus:
        m_canvas->zoomBy(1.0 / WheelStep);
        return;
    case Qt::Key_0:
        m_canvas->fitToWindow();
        return;
    default:
        break;
    }
    QDialog::keyPressEvent(event);
}
