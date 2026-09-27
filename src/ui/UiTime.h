#pragma once

#include <QApplication>
#include <QString>

// Where the window thread's time goes.
//
// Every event the window thread delivers - a paint, a timer, a queued signal -
// is timed here, and the time is charged to the kind of object that handled
// it, minus whatever that handler spent delivering events inside itself (so a
// window's repaint is not counted again for each widget in it). Once a minute
// the log gets the biggest few, as a share of one core:
//
//   window thread: 19.5% of a core; ChatView paint 7.2%, CallView paint 4.1%, ...
//
// "Singularity uses more CPU than Discord" is a question about exactly this,
// and it used to be answered by guessing.
class SingularityApplication : public QApplication
{
public:
    SingularityApplication(int &argc, char **argv);

    bool notify(QObject *receiver, QEvent *event) override;

    // The line described above, for the time since the last call, and starts
    // counting again.
    static QString takeReport();

    // The frame pacer.
    //
    // Every widget update posts one "redraw the window" request, and nothing
    // lines them up: a GIF background, someone's stream, a speaking ring and
    // an animated emoji, each ticking on its own clock, made four separate
    // redraws where one would do. Because the background is drawn by the
    // graphics card, each of those redraws composes the whole window again -
    // measured at about 2.6 ms of processor time each, and the window thread
    // spent two thirds of every second inside them.
    //
    // So animation redraws wait for a shared beat and go out together, at most
    // once per `ms`. Anything a person does - a key, a click, the wheel, the
    // pointer moving - lifts the pacer for a moment, so typing, hovering and
    // scrolling are never held back. 0 turns it off.
    static void setFramePace(int ms);
    static int framePace();
};
