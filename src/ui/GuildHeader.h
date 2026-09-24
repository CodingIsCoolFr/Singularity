#pragma once

#include <QImage>
#include <QString>
#include <QUrl>
#include <QWidget>

struct GuildInfo;

// The top of the channel list, the way Discord draws it: the server's banner
// with its name over it, and under that the boost goal bar - "Boost Goal",
// a filling pill, and "20/36 Boosts".
//
// A server without a banner gets the plain name row it always had, and one
// whose owner switched the progress bar off gets no bar, both as in Discord.
class GuildHeader : public QWidget
{
    Q_OBJECT

public:
    explicit GuildHeader(QWidget *parent = nullptr);

    void setDirectMessages();
    void setGuild(const GuildInfo &guild);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

    // How many boosts the bar counts toward. Public so it can be checked on
    // its own.
    static int boostGoal(const GuildInfo &guild);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    int bannerHeight() const;
    int boostRowHeight() const;

    QString m_title;
    QUrl m_bannerUrl;
    QImage m_banner;
    bool m_showBoosts = false;
    int m_boosts = 0;
    int m_goal = 0;
};
