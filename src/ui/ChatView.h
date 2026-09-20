#pragma once

#include <QHash>
#include <QPixmap>
#include <QSet>
#include <QTextBrowser>
#include <QTimer>

class AnimatedImage;

// The message view.
//
// A plain QTextBrowser never fetches remote pictures, so avatars, emoji and
// image attachments would stay blank. This subclass routes those requests
// through MediaCache, rounds avatars into circles, shrinks large pictures so
// one photo cannot take over the column, and drives animated pictures frame by
// frame, which QTextBrowser will not do on its own.
class ChatView : public QTextBrowser
{
    Q_OBJECT

public:
    explicit ChatView(QWidget *parent = nullptr);
    ~ChatView() override;

    void clearImageCache();

    // Turning this off freezes every moving picture on the first frame.
    void setAnimationsEnabled(bool enabled);

    static bool isAllowedImageHost(const QUrl &url);

protected:
    QVariant loadResource(int type, const QUrl &name) override;

private:
    void pumpAnimations();
    void adoptAnimation(const QUrl &url);
    QPixmap prepare(const QUrl &url) const;
    QPixmap scaleForDocument(const QUrl &url, const QImage &source) const;
    QSize boxFor(const QUrl &url) const;

    QSet<QString> m_wanted;
    QHash<QString, AnimatedImage *> m_animations;

    // The size each picture settled on, so later frames never change the page
    // height and make the view jump.
    mutable QHash<QString, QSize> m_frameSize;
    QTimer m_animationTimer;
    bool m_animationsEnabled = true;
};
