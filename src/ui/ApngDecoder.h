#pragma once

#include <QByteArray>
#include <QImage>
#include <QList>

// Animated PNG, which Qt has no reader for.
//
// Discord's avatar decorations are APNGs (the CDN's passthrough=true), so
// without this they could only be shown as their first frame. An APNG is an
// ordinary PNG with extra chunks: acTL says how many frames, each fcTL says
// where a frame goes, for how long, and how it is cleared afterwards, and the
// frame data sits in IDAT (the first frame) or fdAT (the rest, with a
// sequence number in front).
//
// Each frame is decoded by building a tiny stand-alone PNG out of it - the
// file's own header resized to the frame, its palette and transparency
// chunks, the frame's data as IDAT - and handing that to Qt's PNG reader.
// The frames are then drawn onto a canvas following the spec's blend and
// dispose rules (https://wiki.mozilla.org/APNG_Specification), so what comes
// out is a list of full pictures ready to show.
namespace Apng {

struct Frame
{
    QImage image;   // the whole canvas at this frame
    int delayMs = 100;
};

// True for a PNG that carries an acTL chunk before its image data.
bool isAnimated(const QByteArray &bytes);

// Every frame, composed. Empty if the file is not an APNG or is broken.
QList<Frame> decode(const QByteArray &bytes);

} // namespace Apng
