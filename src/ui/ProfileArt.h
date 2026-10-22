#pragma once

#include "core/Cosmetics.h"

#include <QHash>
#include <QImage>
#include <QList>
#include <QMargins>
#include <QPointer>
#include <QWidget>

// Profile effects and profile frames, the cosmetics worn on a profile itself,
// drawn the way Discord's client draws them (web build 643186: its profile
// effect player and profile frame components, and their stylesheets).
//
// Their pictures are fetched from Discord's CDN and kept on disk by these
// views, not through MediaCache: a frame's pictures are 1312 pixels across
// and an effect's are animations of a few megabytes each, far past what
// MediaCache keeps for drawing. They are unpacked off the window's thread
// and shrunk to the size they are drawn at.

// An effect over a whole profile card: a child of the card, as large as it,
// letting every click through. Like Discord's it starts half a second after
// every picture is in, and fades to a third while the pointer is on the card
// so what is under it can be read.
//
// It runs on the frame clock (ui/FrameClock.h): each tick picks the frames
// due and books the next tick for when the next frame is, and every paint
// draws the frames that tick picked. Widgets under the effect repaint their
// own parts on their own beat, and each such repaint paints the effect over
// that part too, so it has to be the same frame as the rest of the card.
class ProfileEffectView : public QWidget
{
public:
    explicit ProfileEffectView(QWidget *card);

    // An empty effect clears it.
    void setEffect(const Cosmetics::ProfileEffect &effect);

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Layer
    {
        Cosmetics::EffectLayer config;
        QList<QImage> frames;    // as wide as the card, cut off below it
        QList<int> ends;         // when each frame ends, from the picture's start
        bool arrived = false;    // in, or given up on
    };

    void arrived(int generation, int index, const QList<QImage> &frames, const QList<int> &delays);
    // On the frame clock: picks the frames due and the fade, and books the
    // next tick.
    void tick();
    // The frame of a layer showing `ms` after the effect began, -1 for none.
    int frameAt(const Layer &layer, qint64 ms) const;
    // When, after the effect began, that changes next; -1 for never.
    qint64 nextChange(const Layer &layer, qint64 ms) const;

    QList<Layer> m_layers;
    int m_generation = 0;
    bool m_running = false;
    qint64 m_startedAt = 0;      // frame clock time it began
    qreal m_opacity = 1.0;
    qint64 m_fadedAt = -1;       // frame clock time of the last fade step, -1 while still
    QList<int> m_showing;        // the frame each layer shows, picked at the last tick
};

// A profile frame round a card. Two of these share one frame: one behind the
// card draws the frame's back layers, one in front its front layers. Both are
// children of whatever holds the card, kept over the card's rectangle plus
// how far the frame reaches past it.
class ProfileFrameView : public QWidget
{
public:
    ProfileFrameView(QWidget *card, QWidget *parent, bool front);

    // An empty frame clears it.
    void setFrame(const Cosmetics::ProfileFrame &frame);

    // How far a frame reaches past a card this wide.
    static QMargins overflow(const Cosmetics::ProfileFrame &frame, int cardWidth);

protected:
    void paintEvent(QPaintEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void place();

    QPointer<QWidget> m_card;
    bool m_front = true;
    Cosmetics::ProfileFrame m_frame;
    QHash<QString, QImage> m_pictures;   // layer id -> its picture, as wide as the frame
    int m_generation = 0;
};
