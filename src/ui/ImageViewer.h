#pragma once

#include <QDialog>
#include <QPointF>
#include <QUrl>

#include <functional>

class AnimatedImage;
class ClipPlayer;
class QLabel;
class QPushButton;

// The big view you get when a picture in chat is clicked.
//
// Scroll to zoom around the pointer, drag to move, double click to flip
// between fitting the window and full size. Escape closes it.
class ImageViewer : public QDialog
{
    Q_OBJECT

public:
    explicit ImageViewer(QWidget *parent = nullptr);

    // `url` must be a Discord host, the same rule the message view follows.
    void showImage(const QUrl &url);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    class Canvas;
    ClipPlayer *m_clip = nullptr;   // when what is shown is a clip, not a picture

    void updateZoomLabel();
    void saveAs();

    Canvas *m_canvas = nullptr;
    QLabel *m_zoomLabel = nullptr;
    QLabel *m_nameLabel = nullptr;
    QUrl m_url;
};
