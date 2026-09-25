#pragma once

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QSize>
#include <QUrl>

class QNetworkReply;
class ClipDecoder;

// Plays one short clip in a message - a gifv card, an mp4 or a webm - muted
// and looping, as small pictures the size the chat draws them.
//
// This replaced a QMediaPlayer per clip. Qt's player decodes on the graphics
// card and copies every frame back on the window thread, which froze the
// window for up to 600 ms at a time on an AMD machine; told to decode on the
// CPU instead, it started one decoding thread per processor core for every
// clip - 64 threads and 150 MB for two gifs on a 32-thread CPU (2026-09-25).
//
// Here every clip is decoded on one shared background thread, with two helper
// threads each, and every frame is shrunk to its size in the chat before it
// leaves that thread. The window only ever receives a small finished picture.
class ClipPlayer : public QObject
{
    Q_OBJECT

public:
    // `box` is the largest the picture will be drawn; frames are never bigger.
    ClipPlayer(const QUrl &url, const QSize &box, QObject *parent = nullptr);
    ~ClipPlayer() override;

    // Stops decoding while nobody can see the clip, and picks up where it was.
    void setPaused(bool paused);

    // The file as downloaded, for saving. Empty until it has arrived.
    QByteArray data() const { return m_data; }

signals:
    void frameReady(const QImage &frame);
    void failed(const QString &reason);

private:
    void onDownloaded();

    QUrl m_url;
    QSize m_box;
    QByteArray m_data;
    bool m_paused = false;
    QPointer<QNetworkReply> m_reply;
    ClipDecoder *m_decoder = nullptr;
};
