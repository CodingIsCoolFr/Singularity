#pragma once

#include "core/RemoteAuthCrypto.h"

#include <QImage>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QWebSocket>

// Signing in by scanning a QR code with the Discord phone app.
//
// This is the path for anyone who cannot use the password box: an account with
// a security key, one that keeps hitting a captcha, or a friend who simply does
// not want to type their password into a third-party program. The phone does
// the trusting; this program only ever holds a key nobody else has seen.
//
// The shape of it: make an RSA key, tell Discord's remote-auth gateway about
// it, and show its fingerprint as a QR code. The phone scans it, shows who is
// about to sign in, and on approval the gateway hands over the account token -
// encrypted to our key, so the gateway itself never sees it either.
//
// The handshake is split from the socket so it can be tested without a live
// connection: handleMessage() takes one gateway message and drives everything,
// calling sendText() and exchangeTicket(), both overridable.
class RemoteAuth : public QObject
{
    Q_OBJECT

public:
    explicit RemoteAuth(QObject *parent = nullptr);
    ~RemoteAuth() override;

    // Opens the connection and begins the handshake.
    void start();

    // Drops the connection and forgets the key. Safe to call at any point.
    void stop();

signals:
    // The QR code to show, already drawn, plus the address inside it for anyone
    // who would rather type it. Fires once the gateway has accepted our key.
    void qrCodeReady(const QImage &code, const QString &url);

    // The phone scanned the code and is now showing this account, waiting for
    // the person to approve. Purely so the desktop can say "check your phone".
    void scanned(const QString &username);

    // Approved on the phone; this is the account token.
    void succeeded(const QString &token);

    // The phone said no.
    void declined();

    // The code sat unscanned until the gateway gave up. A fresh start() gets a
    // new one.
    void expired();

    // Anything else: the gateway was unreachable, or spoke in a way we did not
    // expect. The password box is still there to fall back to.
    void failed(const QString &reason);

protected:
    // The two seams the test drives. The real class writes to the socket and
    // makes the REST call; a test captures the text and feeds a reply.
    virtual void sendText(const QString &text);
    virtual void exchangeTicket(const QString &ticket);

    // One decoded gateway message. Public behaviour lives here so a test can
    // call it directly with canned messages.
    void handleMessage(const QByteArray &json);

    // Finishes the ticket exchange once the encrypted token is in hand.
    void onEncryptedToken(const QByteArray &encryptedTokenBase64);

    RemoteAuthCrypto m_crypto;

private:
    void sendJson(const QJsonObject &object);

    QWebSocket m_socket;
    QNetworkAccessManager m_network;
    QTimer m_heartbeat;
    bool m_started = false;
    QString m_fingerprint;
};
