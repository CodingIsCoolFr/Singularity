#pragma once

#include "core/Cosmetics.h"

#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QList>
#include <QMargins>
#include <QPointer>
#include <QTimer>
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
    void tick();
    // The frame of a layer showing `ms` after the effect began, -1 for none.
    int frameAt(const Layer &layer, qint64 ms) const;

    QList<Layer> m_layers;
    int m_generation = 0;
    bool m_running = false;
    QElapsedTimer m_clock;
    QTimer m_timer;
    qreal m_opacity = 1.0;
    QList<int> m_painted;        // the frame each layer showed when last painted
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
