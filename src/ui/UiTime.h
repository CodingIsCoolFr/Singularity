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
};
