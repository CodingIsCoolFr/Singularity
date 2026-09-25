#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QString>

// The check Discord puts in front of some messages.
//
// It is the same puzzle the official client shows. The person solves it.
// Nothing in here answers the puzzle on its own.
class CaptchaDialog : public QDialog
{
    Q_OBJECT

public:
    // Empty if the window was closed before the check finished.
    static QString solve(QWidget *parent, const QString &sitekey, const QString &rqdata);

    // True when an error body is Discord asking for a check: a captcha_key
    // list and a site key. Whatever the words in captcha_key say. Discord's
    // own client shows the puzzle for every such answer, and the words vary
    // ("captcha-required", "You need to update your app to join this server.").
    static bool isDemand(const QJsonObject &body);

private:
    CaptchaDialog(const QString &sitekey, const QString &rqdata, QWidget *parent);
    ~CaptchaDialog() override;

    void startBrowser();
    void resizeBrowser();
    void inject();
    void resizeEvent(QResizeEvent *event) override;

    QString m_sitekey;
    QString m_rqdata;
    QString m_token;
    class QWidget *m_host = nullptr;

    struct Browser;
    Browser *m_browser = nullptr;
};
