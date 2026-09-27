#pragma once

#include <QImage>
#include <QPointer>
#include <QVector3D>
#include <QWidget>

class AuroraWidget;
class QMovie;

// What the main window stands on: your picture, or the drawn black hole.
//
// The window used to be built on the black hole's OpenGL widget even when a
// picture was showing, and that one fact cost more than anything else in the
// program. A window with an OpenGL widget in it is put together by the
// graphics card on every update, however small: a speaking ring changing
// colour recomposed all 2560 by 1400 pixels, at about 2.6 ms of processor time
// each. Worse, Qt repaints every widget in the window from scratch whenever
// the OpenGL widget and anything else change in the same update
// (QWidgetRepaintManager::paintAndFlush adds the whole texture widget's rect to
// the dirty region). In a call with a GIF background that was most updates.
//
// A picture - still or animated - is now drawn here with QPainter: 0.8 ms to
// cover the whole window with a 540x304 GIF frame, smoothly. Small updates are
// small again. The OpenGL widget exists only while the black hole is the
// background, as the bottom-most child, and is gone otherwise.
class Backdrop : public QWidget
{
    Q_OBJECT

public:
    explicit Backdrop(QWidget *parent = nullptr);
    ~Backdrop() override;

    // The picture, or an empty path for the black hole. `dimPercent` darkens
    // the picture the same way the old shader did.
    void setBackground(const QString &picturePath, int dimPercent);

    void setRunning(bool on);
    void setHoleColors(const QVector3D &accent, const QVector3D &disk, const QVector3D &grade);

    bool showsHole() const { return m_hole != nullptr; }

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void loadPicture();
    void rebuildShading();
    void shadeFrame();
    QRect coverRect() const;

    // The black hole, only while it is the background.
    QPointer<AuroraWidget> m_hole;
    bool m_running = true;
    QVector3D m_accent, m_disk, m_grade;
    bool m_haveColours = false;

    QString m_picturePath;
    int m_dim = 45;
    QMovie *m_movie = nullptr;
    QImage m_frame;        // the current frame, flattened onto the night colour
    QImage m_shaded;       // the same, darkened and with soft edges - what is drawn

    // The darkening and the soft edges, the old shader's two steps, done to
    // the picture at its own size before it is stretched, not to the window
    // after. Both are straight per-pixel sums, so the result is the same, and
    // a 540x304 GIF frame is a twenty-fifth of the pixels of the window.
    // Darkening the stretched window measured 3 ms of processor time a frame;
    // this is well under half a millisecond.
    //
    // One factor per picture pixel, worked out once per window size and dim:
    // the colour is kept by `keep` and moved towards the night colour by the
    // rest.
    QList<float> m_keep;
    QList<float> m_toward;
    QSize m_shadeWindow;
    QSize m_shadeFrame;
    int m_shadeDim = -1;
};
