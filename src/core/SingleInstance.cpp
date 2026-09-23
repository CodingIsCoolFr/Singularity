#include "core/SingleInstance.h"

#include "core/Logger.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QThread>
#include <QTimer>
#include <QVersionNumber>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace SingleInstance {

namespace {

QLocalServer *g_server = nullptr;
std::function<void()> g_show;
bool g_replaced = false;

// Per Windows user, so two people signed in to the same machine each get
// their own Singularity.
QString serverName()
{
    QString user = qEnvironmentVariable("USERNAME");
    if (user.isEmpty())
        user = QStringLiteral("default");
    return QStringLiteral("Singularity-") + user;
}

// Asks the running copy something. Empty if nobody answered.
QByteArray ask(const QJsonObject &request)
{
    QLocalSocket socket;
    socket.connectToServer(serverName());
    if (!socket.waitForConnected(700))
        return {};
    socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    socket.flush();
    if (!socket.waitForReadyRead(3000))
        return {};
    return socket.readLine().trimmed();
}

void listen()
{
    g_server = new QLocalServer(QCoreApplication::instance());
    // A copy that crashed can leave the name behind; nothing answers on it,
    // so it is safe to clear.
    QLocalServer::removeServer(serverName());
    if (!g_server->listen(serverName())) {
        wlog(QStringLiteral("app"), QStringLiteral("could not claim the single-copy name: %1").arg(g_server->errorString()));
        return;
    }

    QObject::connect(g_server, &QLocalServer::newConnection, g_server, []() {
        while (QLocalSocket *socket = g_server->nextPendingConnection()) {
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QLocalSocket::readyRead, socket, [socket]() {
                if (!socket->canReadLine())
                    return;
                const QJsonObject request = QJsonDocument::fromJson(socket->readLine()).object();
                const QVersionNumber theirs =
                    QVersionNumber::fromString(request.value(QStringLiteral("version")).toString());
                const QVersionNumber ours = QVersionNumber::fromString(QCoreApplication::applicationVersion());
                const bool replace = request.value(QStringLiteral("replace")).toBool();

                if (replace || theirs > ours) {
                    wlog(QStringLiteral("app"),
                         QStringLiteral("another copy (%1) is taking over; this one (%2) is closing")
                             .arg(theirs.toString(), ours.toString()));
                    socket->write("quitting\n");
                    socket->flush();
                    socket->waitForBytesWritten(500);
                    g_replaced = true;
                    // Stop answering at once so the new copy can take the name.
                    g_server->close();
                    QTimer::singleShot(0, qApp, []() { qApp->quit(); });
                    return;
                }

                wlog(QStringLiteral("app"), QStringLiteral("Singularity was opened again; bringing this copy forward"));
                socket->write("showing\n");
                socket->flush();
                if (g_show)
                    g_show();
            });
        }
    });
}

} // namespace

bool claim(const QString &version, bool replaceRunning)
{
#ifdef Q_OS_WIN
    // Lets the running copy bring its window to the front. Windows only lets
    // a program take the foreground when the one the person is using hands it
    // over, and at this moment that is us.
    AllowSetForegroundWindow(ASFW_ANY);
#endif

    const QByteArray answer = ask(QJsonObject{
        {QStringLiteral("version"), version},
        {QStringLiteral("replace"), replaceRunning},
    });

    if (answer == "showing") {
        wlog(QStringLiteral("app"), QStringLiteral("Singularity is already open; showing that copy instead"));
        return false;
    }

    if (answer == "quitting") {
        // Wait for it to be gone before signing in, so two copies never talk
        // to Discord as the same account at once.
        QElapsedTimer waited;
        waited.start();
        while (waited.elapsed() < 10000) {
            QLocalSocket probe;
            probe.connectToServer(serverName());
            if (!probe.waitForConnected(200))
                break;
            probe.abort();
            QThread::msleep(200);
        }
        wlog(QStringLiteral("app"), QStringLiteral("the previous copy handed over after %1 ms").arg(waited.elapsed()));
    }

    listen();
    return true;
}

void setShowHandler(std::function<void()> handler)
{
    g_show = std::move(handler);
}

bool replaced()
{
    return g_replaced;
}

} // namespace SingleInstance
