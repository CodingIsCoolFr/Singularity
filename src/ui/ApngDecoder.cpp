#include "ui/ApngDecoder.h"

#include <QPainter>
#include <QtEndian>

#include <array>

namespace Apng {

namespace {

const QByteArray Signature("\x89PNG\r\n\x1a\n", 8);

quint32 crc32(const QByteArray &typeAndData)
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
    for (const char byte : typeAndData)
        c = table[(c ^ quint8(byte)) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

QByteArray chunk(const QByteArray &type, const QByteArray &data)
{
    QByteArray out;
    out.reserve(data.size() + 12);
    char length[4];
    qToBigEndian<quint32>(quint32(data.size()), length);
    out.append(length, 4);
    const QByteArray body = type + data;
    out.append(body);
    char crc[4];
    qToBigEndian<quint32>(crc32(body), crc);
    out.append(crc, 4);
    return out;
}

struct Chunk
{
    QByteArray type;
    QByteArray data;
};

// The chunks in order, or empty if anything is malformed.
QList<Chunk> chunks(const QByteArray &bytes)
{
    QList<Chunk> list;
    if (!bytes.startsWith(Signature))
        return list;
    qsizetype at = Signature.size();
    while (at + 12 <= bytes.size()) {
        const quint32 length = qFromBigEndian<quint32>(bytes.constData() + at);
        if (length > quint32(bytes.size()) || at + 12 + qsizetype(length) > bytes.size())
            return {};
        list.append(Chunk{bytes.mid(at + 4, 4), bytes.mid(at + 8, length)});
        at += 12 + length;
        if (list.last().type == "IEND")
            break;
    }
    return list;
}

struct Control
{
    quint32 width = 0;
    quint32 height = 0;
    quint32 x = 0;
    quint32 y = 0;
    int delayMs = 100;
    quint8 dispose = 0;   // 0 none, 1 background, 2 previous
    quint8 blend = 0;     // 0 source, 1 over
};

Control readControl(const QByteArray &data)
{
    Control c;
    if (data.size() < 26)
        return c;
    const char *p = data.constData();
    c.width = qFromBigEndian<quint32>(p + 4);
    c.height = qFromBigEndian<quint32>(p + 8);
    c.x = qFromBigEndian<quint32>(p + 12);
    c.y = qFromBigEndian<quint32>(p + 16);
    const quint16 num = qFromBigEndian<quint16>(p + 20);
    quint16 den = qFromBigEndian<quint16>(p + 22);
    if (den == 0)
        den = 100;
    c.delayMs = int(qint64(num) * 1000 / den);
    // Browsers treat a near-zero delay as "as fast as allowed", which on a
    // decoration means a blur; 20 ms (50 fps) is the floor they use.
    if (c.delayMs < 20)
        c.delayMs = 100;
    c.dispose = quint8(p[24]);
    c.blend = quint8(p[25]);
    return c;
}

} // namespace

bool isAnimated(const QByteArray &bytes)
{
    if (!bytes.startsWith(Signature))
        return false;
    // acTL must come before the first IDAT; a quick scan is enough.
    qsizetype at = Signature.size();
    while (at + 12 <= bytes.size()) {
        const quint32 length = qFromBigEndian<quint32>(bytes.constData() + at);
        const QByteArray type = bytes.mid(at + 4, 4);
        if (type == "acTL") {
            if (length < 8 || at + 16 > bytes.size())
                return false;
            return qFromBigEndian<quint32>(bytes.constData() + at + 8) > 1;
        }
        if (type == "IDAT" || type == "IEND")
            return false;
        at += 12 + qsizetype(length);
    }
    return false;
}

QList<Frame> decode(const QByteArray &bytes)
{
    const QList<Chunk> all = chunks(bytes);
    if (all.isEmpty() || all.first().type != "IHDR" || all.first().data.size() != 13)
        return {};

    const QByteArray header = all.first().data;
    const quint32 canvasWidth = qFromBigEndian<quint32>(header.constData());
    const quint32 canvasHeight = qFromBigEndian<quint32>(header.constData() + 4);
    if (canvasWidth == 0 || canvasHeight == 0 || canvasWidth > 4096 || canvasHeight > 4096)
        return {};

    // Palette, transparency, colour space: copied into every frame's PNG.
    QByteArray shared;
    for (const Chunk &c : all) {
        if (c.type == "IDAT" || c.type == "fdAT")
            break;
        if (c.type == "IHDR" || c.type == "acTL" || c.type == "fcTL")
            continue;
        shared += chunk(c.type, c.data);
    }

    // Group the data chunks by the fcTL in front of them.
    struct Pending
    {
        Control control;
        QByteArray data;   // concatenated IDAT / fdAT payloads (fdAT without its sequence number)
    };
    QList<Pending> pending;
    bool inFrame = false;
    for (const Chunk &c : all) {
        if (c.type == "fcTL") {
            pending.append(Pending{readControl(c.data), {}});
            inFrame = true;
        } else if (c.type == "IDAT" && inFrame) {
            pending.last().data += c.data;
        } else if (c.type == "fdAT" && inFrame && c.data.size() > 4) {
            pending.last().data += c.data.mid(4);
        }
        // An IDAT before any fcTL is the default image, not part of the
        // animation; it is skipped.
    }
    if (pending.size() < 2)
        return {};

    QList<Frame> frames;
    QImage canvas(int(canvasWidth), int(canvasHeight), QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);

    for (int i = 0; i < pending.size(); ++i) {
        const Control &control = pending.at(i).control;
        if (control.width == 0 || control.height == 0 || control.x + control.width > canvasWidth
            || control.y + control.height > canvasHeight)
            return {};

        QByteArray frameHeader = header;
        qToBigEndian<quint32>(control.width, frameHeader.data());
        qToBigEndian<quint32>(control.height, frameHeader.data() + 4);
        const QByteArray png = Signature + chunk("IHDR", frameHeader) + shared
            + chunk("IDAT", pending.at(i).data) + chunk("IEND", {});
        QImage piece;
        if (!piece.loadFromData(png, "PNG"))
            return {};

        const QRect area(int(control.x), int(control.y), int(control.width), int(control.height));
        const QImage before = control.dispose == 2 ? canvas.copy(area) : QImage();

        {
            QPainter painter(&canvas);
            painter.setCompositionMode(control.blend == 0 ? QPainter::CompositionMode_Source
                                                          : QPainter::CompositionMode_SourceOver);
            painter.drawImage(area.topLeft(), piece);
        }
        frames.append(Frame{canvas.copy(), control.delayMs});

        // How the area is left for the next frame. "Previous" on the very
        // first frame means "background", as the spec says.
        if (control.dispose == 1 || (control.dispose == 2 && i == 0)) {
            QPainter painter(&canvas);
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            painter.fillRect(area, Qt::transparent);
        } else if (control.dispose == 2) {
            QPainter painter(&canvas);
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            painter.drawImage(area.topLeft(), before);
        }
    }
    return frames;
}

} // namespace Apng
