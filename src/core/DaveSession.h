#pragma once

#include <QByteArray>
#include <QObject>
#include <QSet>
#include <QString>

// Discord's end-to-end encryption for calls, wrapped so the rest of the client
// never touches the raw library.
//
// How a call becomes encrypted:
//   1. Everyone in the call forms a key group. Discord carries the messages
//      but cannot read them.
//   2. We publish a key package saying how to include us.
//   3. The group sends proposals as people come and go; we answer with a
//      commit, or accept a welcome that adds us.
//   4. Each member ends up with a key of their own, ratcheted forward, used
//      to encrypt the audio they send.
//
// Every step is driven by the voice socket, so this class only ever reacts.
class DaveSession : public QObject
{
    Q_OBJECT

public:
    explicit DaveSession(QObject *parent = nullptr);
    ~DaveSession() override;

    // Zero when the library is missing, which means calls cannot be joined.
    static int maxSupportedVersion();
    static bool isAvailable();

    // Starts a group for one call. `groupId` is the channel id.
    bool begin(int version, quint64 groupId, const QString &selfUserId);
    void end();
    bool isActive() const { return m_session != nullptr; }

    // Who Discord says may speak for the group. Arrives as opcode 25.
    void setExternalSender(const QByteArray &credential);

    // Our own key package, to be sent back as opcode 26.
    QByteArray keyPackage();

    // People joining or leaving, opcode 27. Returns the commit and welcome to
    // send back as opcode 28, or empty if there is nothing to say.
    QByteArray processProposals(const QByteArray &proposals, const QSet<QString> &knownUserIds);

    // A commit the group agreed on, opcode 29. False means it was rejected,
    // and the caller should ask to be added again with opcode 31.
    bool processCommit(const QByteArray &commit);

    // Our invitation into the group, opcode 30.
    bool processWelcome(const QByteArray &welcome, const QSet<QString> &knownUserIds);

    // Hands the current keys to the encryptor and decryptors.
    bool applyKeys(const QString &selfUserId, const QSet<QString> &otherUserIds);

    // Tells the encryptor that pictures on this stream are H.264.
    //
    // Without it the library does not know which bytes a packetiser has to be
    // able to read, and it seals the whole picture including those. The far
    // end then cannot find where each piece begins.
    void useH264(quint32 ssrc);

    // Wraps one outgoing frame. Returns empty on failure, which the caller
    // should treat as "do not send this one".
    //
    // For a picture, `frame` is the whole picture in Annex B, a start code
    // then each piece, and not one packet of it. The library decides, from
    // the piece types, which bytes stay readable and seals the rest as one
    // block. Splitting into packets happens after that, or the far end glues
    // the packets back together and finds a seal in the middle of the picture.
    //
    // `video` must say which kind it is, and not as a formality: the library
    // keeps a separate counter for each, and it leaves different parts of the
    // frame in the clear because a decoder has to read a little of an H.264
    // picture before it can know what to do with the rest. Sealing a picture as
    // though it were sound produces something the far end refuses.
    QByteArray encrypt(const QByteArray &frame, quint32 ssrc, bool video = false);

    // Unwraps one incoming frame for a given person.
    //
    // `video` must match the kind of frame: Discord's library keeps separate
    // counters for the two, and a picture handed over as sound is refused.
    QByteArray decrypt(const QString &userId, const QByteArray &frame, bool video = false);

signals:
    // Raised when the library reports the group has gone wrong. The right
    // answer is to ask Discord to add us again.
    void groupFailed(const QString &reason);

private:
    void *m_session = nullptr;    // DAVESessionHandle
    void *m_encryptor = nullptr;  // DAVEEncryptorHandle
    QString m_selfUserId;

    // One decryptor per person, because each has their own key.
    QHash<QString, void *> m_decryptors;

    // The keys themselves. The encryptor and decryptors only borrow these, so
    // this class has to keep them alive for as long as they are in use and
    // free them afterwards.
    void *m_myKey = nullptr;                // DAVEKeyRatchetHandle
    QHash<QString, void *> m_theirKeys;

    void releaseKeys();
};
