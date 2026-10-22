// Animations keep the frame clock's rules (ui/FrameClock.h).
//
// The profile effect once worked its frame out from the time of each paint.
// An animated name under it repainted its own strip on its own beat, the
// effect drew a newer frame in just that strip - a band across the name - and
// counted that frame as shown, so half the frames never reached the rest of
// the card. This holds every animation to the rules that fixed it:
//
//   - a paint shows the frame the last tick chose, whatever the time is by
//     then, so a repaint of one strip matches the rest;
//   - every frame is shown, in order, at the first tick after it is due;
//   - a tick's redraw goes out at once, not at the frame pacer's next beat.
//
// The clock is driven by hand, so the answers are the same on any machine.
// The pictures are animated PNGs made here, one colour per frame, so what was
// painted says which frame it was.

#include <QApplication>
#include <QBuffer>
#include <QByteArray>
#include <QColor>
#include <QCursor>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMargins>
#include <QMovie>
#include <QObject>
#include <QPainter>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QtEndian>

#include <array>
#include <cmath>
#include <cstdio>
#include <functional>

// Their state is read straight from them; only data, so nothing here needs a
// private function (MSVC names those differently and they would not link).
// Every Qt header they use is in above, untouched.
#define _ALLOW_KEYWORD_MACROS
#define private public
#include "ui/AnimatedImage.h"
#include "ui/ProfileArt.h"
#undef private

#include "ui/FrameClock.h"
#include "ui/UiTime.h"

namespace {

int g_failed = 0;

bool check(bool ok, const QString &what)
{
    std::printf("%s  %s\n", ok ? "pass" : "FAIL", qPrintable(what));
    if (!ok)
        ++g_failed;
    return ok;
}

// --- animated PNGs, made here --------------------------------------------------

quint32 crc32(const QByteArray &bytes)
{
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> t{};
        for (quint32 n = 0; n < 256; ++n) {
            quint32 c = n;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    quint32 c = 0xFFFFFFFFu;
    for (const char byte : bytes)
        c = table[(c ^ quint8(byte)) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

QByteArray be32(quint32 value)
{
    char out[4];
    qToBigEndian(value, out);
    return QByteArray(out, 4);
}

QByteArray chunk(const QByteArray &type, const QByteArray &data)
{
    return be32(quint32(data.size())) + type + data + be32(crc32(type + data));
}

// The IHDR and the joined IDAT data of an ordinary PNG.
QPair<QByteArray, QByteArray> pngParts(const QImage &image)
{
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    QByteArray header, data;
    qsizetype at = 8;
    while (at + 12 <= png.size()) {
        const quint32 length = qFromBigEndian<quint32>(png.constData() + at);
        const QByteArray type = png.mid(at + 4, 4);
        if (type == "IHDR")
            header = png.mid(at + 8, length);
        else if (type == "IDAT")
            data += png.mid(at + 8, length);
        at += 12 + length;
    }
    return {header, data};
}

QColor colourOf(int frame, int count)
{
    return QColor::fromHsv(frame * 300 / count, 255, 255);
}

// `count` frames of one colour each, every one `delayMs` long, looping.
QByteArray makeApng(int count, int delayMs, QSize size)
{
    QByteArray out("\x89PNG\r\n\x1a\n", 8);
    quint32 sequence = 0;
    for (int i = 0; i < count; ++i) {
        QImage frame(size, QImage::Format_ARGB32);
        frame.fill(colourOf(i, count));
        const auto [header, data] = pngParts(frame);
        if (i == 0) {
            out += chunk("IHDR", header);
            out += chunk("acTL", be32(quint32(count)) + be32(0));
        }
        QByteArray control = be32(sequence++) + be32(quint32(size.width())) + be32(quint32(size.height())) + be32(0)
            + be32(0);
        char delay[4];
        qToBigEndian<quint16>(quint16(delayMs), delay);
        qToBigEndian<quint16>(1000, delay + 2);
        control += QByteArray(delay, 4) + QByteArray("\0\0", 2);
        out += chunk("fcTL", control);
        if (i == 0)
            out += chunk("IDAT", data);
        else
            out += chunk("fdAT", be32(sequence++) + data);
    }
    out += chunk("IEND", {});
    return out;
}

void le16(QByteArray &out, int value)
{
    out.append(char(value & 0xFF));
    out.append(char((value >> 8) & 0xFF));
}

// The same as a looping GIF, which Qt cannot write. Each pixel goes out as
// its own nine-bit code, with a clear code often enough that the codes never
// grow: plain to write, and any reader takes it.
QByteArray makeGif(int count, int delayMs, QSize size)
{
    QByteArray out("GIF89a");
    le16(out, size.width());
    le16(out, size.height());
    out.append(char(0xF3));   // a 16 colour table follows
    out.append(char(0));
    out.append(char(0));
    for (int i = 0; i < 16; ++i) {
        const QColor colour = i < count ? colourOf(i, count) : QColor(Qt::black);
        out.append(char(colour.red()));
        out.append(char(colour.green()));
        out.append(char(colour.blue()));
    }
    out.append(QByteArray("\x21\xFF\x0BNETSCAPE2.0\x03\x01\x00\x00\x00", 19));   // loop for ever

    for (int i = 0; i < count; ++i) {
        out.append(QByteArray("\x21\xF9\x04\x04", 4));
        le16(out, delayMs / 10);
        out.append(QByteArray("\x00\x00", 2));
        out.append(char(0x2C));
        le16(out, 0);
        le16(out, 0);
        le16(out, size.width());
        le16(out, size.height());
        out.append(char(0));

        QByteArray codes;
        quint32 bits = 0;
        int held = 0;
        const auto put = [&](int code) {
            bits |= quint32(code) << held;
            held += 9;
            while (held >= 8) {
                codes.append(char(bits & 0xFF));
                bits >>= 8;
                held -= 8;
            }
        };
        const int pixels = size.width() * size.height();
        for (int p = 0; p < pixels; ++p) {
            if (p % 250 == 0)
                put(256);   // clear
            put(i);
        }
        put(257);   // end
        if (held > 0)
            codes.append(char(bits & 0xFF));

        out.append(char(8));   // LZW minimum code size
        for (qsizetype at = 0; at < codes.size(); at += 255) {
            const QByteArray block = codes.mid(at, 255);
            out.append(char(block.size()));
            out.append(block);
        }
        out.append(char(0));
    }
    out.append(char(0x3B));
    return out;
}

// Which frame a colour is, -1 for none.
int frameOfColour(QRgb pixel, int count)
{
    for (int i = 0; i < count; ++i) {
        const QColor want = colourOf(i, count);
        if (qAbs(qRed(pixel) - want.red()) <= 2 && qAbs(qGreen(pixel) - want.green()) <= 2
            && qAbs(qBlue(pixel) - want.blue()) <= 2)
            return i;
    }
    return -1;
}

// The tick something due at `ms` is shown on: the first of the grid on or
// after it, a millisecond early allowed.
qint64 tickOf(qint64 ms)
{
    return std::llround(std::ceil(double(ms - 1) / FrameClock::TickMs) * FrameClock::TickMs);
}

bool waitFor(const std::function<bool()> &done, int ms = 10000)
{
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < ms)
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

// The frame a widget paints right now, by its colour in the middle.
int paintedFrame(QWidget *widget, int count)
{
    QImage image(widget->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    widget->render(&image, QPoint(), QRegion(), QWidget::DrawChildren);
    return frameOfColour(image.pixel(image.width() / 2, image.height() / 2), count);
}

// A widget that counts its paints.
class Counter : public QWidget
{
public:
    using QWidget::QWidget;
    int paints = 0;

protected:
    void paintEvent(QPaintEvent *) override
    {
        ++paints;
        QPainter(this).fillRect(rect(), QColor(0x23, 0x24, 0x28));
    }
};

// --- the clock itself -----------------------------------------------------------

void clockHoldsStill()
{
    std::printf("\nthe clock\n");
    FrameClock::setManualTime(5000);

    class Reader : public QWidget
    {
    public:
        qint64 first = -1, second = -1;

    protected:
        void paintEvent(QPaintEvent *) override
        {
            first = FrameClock::now();
            FrameClock::setManualTime(first + 100);
            second = FrameClock::now();
        }
    } reader;
    reader.resize(40, 40);
    reader.show();
    waitFor([&] { return reader.first >= 0; }, 3000);
    check(reader.first >= 0 && reader.first == reader.second, QStringLiteral("time holds still through a paint"));
    check(FrameClock::now() == reader.first + 100, QStringLiteral("and moves again once the paint is done"));
    reader.hide();

    FrameClock::setManualTime(6000);
    QObject owner;
    qint64 inTickFirst = -1, inTickSecond = -1;
    FrameClock::wakeAt(&owner, 6050, [&] {
        inTickFirst = FrameClock::now();
        FrameClock::setManualTime(inTickFirst + 30);
        inTickSecond = FrameClock::now();
    });
    FrameClock::advanceTo(6049);
    check(inTickFirst < 0, QStringLiteral("a wake does not come early"));
    FrameClock::advanceTo(6100);
    check(inTickFirst >= 6049 && inTickFirst <= 6050 + qint64(FrameClock::TickMs) + 1,
          QStringLiteral("a wake comes at the first tick on or after it (%1 for 6050)").arg(inTickFirst));
    check(inTickFirst == inTickSecond, QStringLiteral("time holds still through a tick"));

    // Ticks fall on the grid, so wakes due close together come together.
    QObject a, b;
    qint64 atA = -1, atB = -1;
    FrameClock::setManualTime(7000);
    FrameClock::wakeAt(&a, 7003, [&] { atA = FrameClock::now(); });
    FrameClock::wakeAt(&b, 7009, [&] { atB = FrameClock::now(); });
    FrameClock::advanceTo(7100);
    check(atA >= 0 && atA == atB, QStringLiteral("wakes within one tick run on the same tick"));
}

// --- the profile effect ------------------------------------------------------------

void effectKeepsItsFrames(const QString &apngPath, int count, int delayMs)
{
    std::printf("\nthe profile effect\n");
    FrameClock::setManualTime(10000);

    // Away from the pointer: on the card, the effect fades to a third.
    QCursor::setPos(1, 1);
    QWidget window;
    window.resize(320, 420);
    window.move(300, 100);
    auto *layout = new QVBoxLayout(&window);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *card = new Counter(&window);
    layout->addWidget(card);
    auto *name = new Counter(card);   // an animated name under the effect
    name->setGeometry(20, 180, 280, 40);
    window.show();
    auto *view = new ProfileEffectView(card);
    view->raise();
    view->show();

    Cosmetics::ProfileEffect effect;
    Cosmetics::EffectLayer layer;
    layer.src = QUrl::fromLocalFile(apngPath).toString();
    layer.loop = true;
    layer.duration = 1000000;
    effect.layers << layer;
    view->setEffect(effect);
    if (!check(waitFor([&] { return view->m_running; }), QStringLiteral("the effect's pictures arrive")))
        return;
    const qint64 start = view->m_startedAt;
    const ProfileEffectView::Layer &loaded = view->m_layers.first();
    check(loaded.frames.size() == count, QStringLiteral("all %1 frames decoded").arg(count));

    // Discord holds the start back half a second.
    const qint64 first = start + 500;
    FrameClock::advanceTo(first - 2);
    QApplication::processEvents();
    check(view->m_showing.value(0, -1) == -1 && paintedFrame(view, count) == -1,
          QStringLiteral("nothing for the first half second"));

    // Frame 2 on show; then time reaches frame 3, but no tick has come. A
    // repaint of the name strip now must not draw frame 3 there.
    FrameClock::advanceTo(tickOf(first + 2 * delayMs));
    QApplication::processEvents();
    const int before = view->m_showing.value(0, -1);
    check(before == 2, QStringLiteral("frame 2 shows at its tick (showing %1)").arg(before));
    FrameClock::setManualTime(first + 3 * delayMs + 1);
    name->update();
    QApplication::processEvents();
    const int between = paintedFrame(view, count);
    check(between == 2, QStringLiteral("a repaint between ticks draws the last tick's frame, not one worked out "
                                       "from the time (drew %1)")
                            .arg(between));
    check(view->m_showing.value(0, -1) == 2, QStringLiteral("and a repaint does not count as showing a new frame"));
    FrameClock::advanceTo(tickOf(first + 3 * delayMs));
    QApplication::processEvents();
    check(paintedFrame(view, count) == 3, QStringLiteral("its tick then shows frame 3 everywhere"));

    // Three loops, a millisecond at a time: every frame, in order, each at
    // the first tick after it was due.
    const qint64 loopMs = qint64(count) * delayMs;
    int last = 3, skipped = 0, early = 0, late = 0;
    qint64 worst = 0;
    for (qint64 t = first + 3 * delayMs + 6; t < first + 3 * loopMs; ++t) {
        FrameClock::advanceTo(t);
        const int now = view->m_showing.value(0, -1);
        if (now == last)
            continue;
        if (now != (last + 1) % count)
            ++skipped;
        const qint64 into = (t - first) % loopMs;
        const qint64 due = qint64(now) * delayMs;
        const qint64 lateBy = into - due;
        worst = qMax(worst, lateBy);
        if (lateBy < -1)
            ++early;
        if (lateBy > qint64(FrameClock::TickMs) + 1)
            ++late;
        last = now;
    }
    check(skipped == 0, QStringLiteral("no frame skipped over three loops (%1 were)").arg(skipped));
    check(early == 0 && late == 0,
          QStringLiteral("each frame at the first tick after it is due (worst %1 ms late)").arg(worst));

    // Hidden, it stops; shown again, it starts over, as Discord's does.
    window.hide();
    check(!FrameClock::booked(view), QStringLiteral("a hidden effect books nothing"));
    window.show();
    QApplication::processEvents();
    check(FrameClock::booked(view) && view->m_startedAt == FrameClock::now(),
          QStringLiteral("shown again it starts from the beginning"));
}

// --- animated pictures: decorations, nameplates, avatars, banners -----------------

void picturesStepOnTheClock(const QByteArray &apng, int count, int delayMs)
{
    std::printf("\nanimated pictures\n");
    FrameClock::setManualTime(50000);

    AnimatedImage player;
    int changes = 0;
    QObject::connect(&player, &AnimatedImage::frameChanged, [&] { ++changes; });
    player.setData(apng);
    if (!check(waitFor([&] { return player.m_apngFrames.size() == count; }),
               QStringLiteral("all %1 frames decoded").arg(count)))
        return;
    const auto shown = [&] { return frameOfColour(player.currentFrame().pixel(1, 1), count); };
    check(shown() == 0, QStringLiteral("it starts on frame 0"));

    // Frames change only on ticks, in order, on time.
    const qint64 start = player.m_apngDueAt - delayMs;
    FrameClock::setManualTime(start + delayMs - 3);
    check(shown() == 0, QStringLiteral("time passing alone changes nothing; only a tick does"));
    int last = 0, skipped = 0, off = 0;
    for (qint64 t = start; t < start + 3 * qint64(count) * delayMs; ++t) {
        FrameClock::advanceTo(t);
        const int now = shown();
        if (now == last)
            continue;
        if (now != (last + 1) % count)
            ++skipped;
        const qint64 lateBy = (t - start) % (qint64(count) * delayMs) - qint64(now) * delayMs;
        if (lateBy < -1 || lateBy > qint64(FrameClock::TickMs) + 1)
            ++off;
        last = now;
    }
    check(skipped == 0, QStringLiteral("no frame skipped over three loops (%1 were)").arg(skipped));
    check(off == 0, QStringLiteral("each frame at the first tick after it is due (%1 were not)").arg(off));

    // Paused part way through a frame, it keeps the time that frame had left.
    const int at = shown();
    const qint64 due = player.m_apngDueAt;
    FrameClock::setManualTime(due - 30);
    player.setPlaying(false);
    FrameClock::advanceTo(due + 5000);
    check(shown() == at, QStringLiteral("paused, it holds its frame"));
    player.setPlaying(true);
    FrameClock::advanceTo(due + 5000 + 25);
    check(shown() == at, QStringLiteral("resumed, the frame gets the rest of its time"));
    FrameClock::advanceTo(due + 5000 + 30 + qint64(FrameClock::TickMs) + 1);
    check(shown() == (at + 1) % count, QStringLiteral("and then the next frame comes"));
}

void moviesShowFramesOnTicks(const QByteArray &gif, int count)
{
    std::printf("\nGIF and WebP pictures\n");
    FrameClock::setManualTime(70000);

    // QMovie reads frames on a timer of its own, in real time; the player
    // shows each from the next tick of the frame clock, and only then.
    AnimatedImage player;
    if (!check(player.setData(gif) && player.m_movie, QStringLiteral("a GIF plays through QMovie")))
        return;
    const auto colour = [count](const QImage &image) {
        return image.isNull() ? -1 : frameOfColour(image.pixel(1, 1), count);
    };
    check(colour(player.currentFrame()) == 0, QStringLiteral("it starts on frame 0"));
    waitFor([&] { return colour(player.m_movie->currentImage()) > 0; }, 3000);
    const int read = colour(player.m_movie->currentImage());
    check(read > 0 && colour(player.currentFrame()) == 0,
          QStringLiteral("a frame QMovie has read waits for a tick (read %1, showing %2)")
              .arg(read)
              .arg(colour(player.currentFrame())));
    FrameClock::advanceTo(FrameClock::now() + qint64(FrameClock::TickMs) + 1);
    check(colour(player.currentFrame()) == colour(player.m_movie->currentImage()) && colour(player.currentFrame()) > 0,
          QStringLiteral("and shows at the tick"));
}

// --- the frame pacer --------------------------------------------------------------

void ticksAreNotHeldBack()
{
    std::printf("\nthe frame pacer\n");
    FrameClock::setManualTime(90000);
    SingularityApplication::setFramePace(200);

    Counter widget;
    widget.resize(60, 60);
    widget.show();
    waitFor([&] { return widget.paints > 0; }, 3000);
    // A redraw just now, so the pacer would hold the next for 200 ms.
    widget.update();
    QApplication::processEvents();
    widget.update();
    QApplication::processEvents();
    const int before = widget.paints;

    QObject owner;
    FrameClock::wakeAt(&owner, FrameClock::now(), [&] { widget.update(); });
    FrameClock::advanceTo(FrameClock::now() + qint64(FrameClock::TickMs) + 1);
    QApplication::processEvents();
    QApplication::processEvents();
    check(widget.paints > before, QStringLiteral("a redraw asked for in a tick goes out at once"));
    SingularityApplication::setFramePace(0);
}

} // namespace

int main(int argc, char **argv)
{
    // No window on anyone's screen, and the same everywhere.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    SingularityApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("SingularityFrameTest"));

    QTemporaryDir dir;
    const int count = 12;
    const int delayMs = 70;   // not a multiple of the grid, on purpose
    const QByteArray apng = makeApng(count, delayMs, QSize(64, 64));
    const QString path = dir.filePath(QStringLiteral("effect.png"));
    {
        QFile file(path);
        file.open(QIODevice::WriteOnly);
        file.write(apng);
    }

    clockHoldsStill();
    effectKeepsItsFrames(path, count, delayMs);
    picturesStepOnTheClock(apng, count, delayMs);
    moviesShowFramesOnTicks(makeGif(count, delayMs, QSize(16, 16)), count);
    ticksAreNotHeldBack();
    FrameClock::setManualTime(-1);

    std::printf("\n%d failed\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
