#pragma once

#include <QElapsedTimer>
#include <QString>
#include <QTimer>
#include <QWidget>

// Covers the window while it fills itself in.
//
// Signing in returns in a moment, but the client is not ready then: forty
// servers, their channels, two thousand voice states and a hundred presences
// all arrive over the following second and a half, and each one redraws part
// of the window. Watching that happen is watching the furniture being carried
// in. This stands in front of it until the room is finished.
//
// It is drawn rather than assembled from widgets because it has to be cheap:
// the whole point is that the machine is busy doing something else.
class LoadingOverlay : public QWidget
{
    Q_OBJECT

public:
    explicit LoadingOverlay(QWidget *parent = nullptr);

    // What is being waited for, under the name.
    void setStep(const QString &step);

    // Fades out and deletes itself.
    //
    // Never instantly, and never before a moment has passed: an overlay that
    // flashes up and vanishes is worse than none at all, and a connection that
    // was already warm can reach this within a few hundred milliseconds.
    void finish();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

    // Follows the window it covers. Done here rather than in the window so
    // this stays one self-contained thing that can be dropped in and deleted.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QString m_step;
    QTimer m_spin;
    QElapsedTimer m_age;
    qreal m_angle = 0.0;
    qreal m_fade = 1.0;
    bool m_leaving = false;
};
