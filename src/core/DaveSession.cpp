#include "core/DaveSession.h"

#include "core/Logger.h"

#include <QHash>

#ifdef SINGULARITY_HAVE_DAVE
#include <dave/dave.h>
#endif

#ifdef SINGULARITY_HAVE_DAVE

namespace {

// The library hands back buffers it allocated itself, which must be handed
// back to it rather than deleted. This turns that pair of steps into one.
QByteArray takeOwned(uint8_t *data, size_t length)
{
    if (!data || length == 0) {
        if (data)
            daveFree(data);
        return {};
    }

    const QByteArray copy(reinterpret_cast<const char *>(data), static_cast<int>(length));
    daveFree(data);
    return copy;
}

// The library wants plain C strings. This keeps the text alive for as long as
// the call needs it.
class UserIdList
{
public:
    explicit UserIdList(const QSet<QString> &ids)
    {
        m_storage.reserve(ids.size());
        m_pointers.reserve(ids.size());
        for (const QString &id : ids) {
            m_storage.append(id.toUtf8());
            m_pointers.append(m_storage.last().constData());
        }
    }

    const char **data() { return m_pointers.isEmpty() ? nullptr : m_pointers.data(); }
    size_t size() const { return static_cast<size_t>(m_pointers.size()); }

private:
    QList<QByteArray> m_storage;
    QList<const char *> m_pointers;
};

void onMlsFailure(const char *source, const char *reason, void *userData)
{
    const QString text = QStringLiteral("%1: %2")
                             .arg(QString::fromUtf8(source ? source : "unknown"),
                                  QString::fromUtf8(reason ? reason : "unknown"));
    wlog(QStringLiteral("dave"), QStringLiteral("group failure, %1").arg(text));

    if (auto *session = static_cast<DaveSession *>(userData))
        emit session->groupFailed(text);
}

} // namespace

#endif // SINGULARITY_HAVE_DAVE

// ---------------------------------------------------------------------------

DaveSession::DaveSession(QObject *parent)
    : QObject(parent)
{
}

DaveSession::~DaveSession()
{
    end();
}

int DaveSession::maxSupportedVersion()
{
#ifdef SINGULARITY_HAVE_DAVE
    return static_cast<int>(daveMaxSupportedProtocolVersion());
#else
    return 0;
#endif
}

bool DaveSession::isAvailable()
{
    return maxSupportedVersion() > 0;
}

bool DaveSession::begin(int version, quint64 groupId, const QString &selfUserId)
{
#ifdef SINGULARITY_HAVE_DAVE
    end();

    m_selfUserId = selfUserId;

    m_session = daveSessionCreate(nullptr, nullptr, &onMlsFailure, this);
    if (!m_session) {
        wlog(QStringLiteral("dave"), QStringLiteral("could not start a key group"));
        return false;
    }

    daveSessionInit(static_cast<DAVESessionHandle>(m_session), static_cast<uint16_t>(version),
                    groupId, selfUserId.toUtf8().constData());

    m_encryptor = daveEncryptorCreate();

    wlog(QStringLiteral("dave"), QStringLiteral("key group started, version %1, channel %2")
                                     .arg(version).arg(groupId));
    return true;
#else
    Q_UNUSED(version) Q_UNUSED(groupId) Q_UNUSED(selfUserId)
    return false;
#endif
}

void DaveSession::releaseKeys()
{
#ifdef SINGULARITY_HAVE_DAVE
    if (m_myKey) {
        daveKeyRatchetDestroy(static_cast<DAVEKeyRatchetHandle>(m_myKey));
        m_myKey = nullptr;
    }
    for (void *key : m_theirKeys)
        daveKeyRatchetDestroy(static_cast<DAVEKeyRatchetHandle>(key));
    m_theirKeys.clear();
#endif
}

void DaveSession::end()
{
#ifdef SINGULARITY_HAVE_DAVE
    for (void *decryptor : m_decryptors)
        daveDecryptorDestroy(static_cast<DAVEDecryptorHandle>(decryptor));
    m_decryptors.clear();

    // After the users of the keys are gone, not before.
    releaseKeys();

    if (m_encryptor) {
        daveEncryptorDestroy(static_cast<DAVEEncryptorHandle>(m_encryptor));
        m_encryptor = nullptr;
    }
    if (m_session) {
        daveSessionDestroy(static_cast<DAVESessionHandle>(m_session));
        m_session = nullptr;
    }
#endif
}

void DaveSession::setExternalSender(const QByteArray &credential)
{
#ifdef SINGULARITY_HAVE_DAVE
    if (!m_session || credential.isEmpty())
        return;

    daveSessionSetExternalSender(static_cast<DAVESessionHandle>(m_session),
                                 reinterpret_cast<const uint8_t *>(credential.constData()),
                                 static_cast<size_t>(credential.size()));

    wlog(QStringLiteral("dave"), QStringLiteral("accepted the group's authority (%1 bytes)")
                                     .arg(credential.size()));
#else
    Q_UNUSED(credential)
#endif
}

QByteArray DaveSession::keyPackage()
{
#ifdef SINGULARITY_HAVE_DAVE
    if (!m_session)
        return {};

    uint8_t *data = nullptr;
    size_t length = 0;
    daveSessionGetMarshalledKeyPackage(static_cast<DAVESessionHandle>(m_session), &data, &length);

    const QByteArray package = takeOwned(data, length);
    wlog(QStringLiteral("dave"), QStringLiteral("our key package is %1 bytes").arg(package.size()));
    return package;
#else
    return {};
#endif
}

QByteArray DaveSession::processProposals(const QByteArray &proposals, const QSet<QString> &knownUserIds)
{
#ifdef SINGULARITY_HAVE_DAVE
    if (!m_session || proposals.isEmpty())
        return {};

    UserIdList ids(knownUserIds);

    uint8_t *data = nullptr;
    size_t length = 0;
    daveSessionProcessProposals(static_cast<DAVESessionHandle>(m_session),
                                reinterpret_cast<const uint8_t *>(proposals.constData()),
                                static_cast<size_t>(proposals.size()), ids.data(), ids.size(),
                                &data, &length);

    const QByteArray reply = takeOwned(data, length);
    wlog(QStringLiteral("dave"), QStringLiteral("handled %1 bytes of membership changes, replying "
                                                "with %2 bytes")
                                     .arg(proposals.size()).arg(reply.size()));
    return reply;
#else
    Q_UNUSED(proposals) Q_UNUSED(knownUserIds)
    return {};
#endif
}

bool DaveSession::processCommit(const QByteArray &commit)
{
#ifdef SINGULARITY_HAVE_DAVE
    if (!m_session || commit.isEmpty())
        return false;

    DAVECommitResultHandle result =
        daveSessionProcessCommit(static_cast<DAVESessionHandle>(m_session),
                                 reinterpret_cast<const uint8_t *>(commit.constData()),
                                 static_cast<size_t>(commit.size()));
    if (!result)
        return false;

    const bool failed = daveCommitResultIsFailed(result);
    const bool ignored = daveCommitResultIsIgnored(result);
    daveCommitResultDestroy(result);

    if (failed) {
        wlog(QStringLiteral("dave"), QStringLiteral("the group's change was rejected"));
        return false;
    }

    wlog(QStringLiteral("dave"), ignored ? QStringLiteral("group change ignored, already applied")
                                         : QStringLiteral("group change applied"));
    return true;
#else
    Q_UNUSED(commit)
    return false;
#endif
}

bool DaveSession::processWelcome(const QByteArray &welcome, const QSet<QString> &knownUserIds)
{
#ifdef SINGULARITY_HAVE_DAVE
    if (!m_session || welcome.isEmpty())
        return false;

    UserIdList ids(knownUserIds);

    DAVEWelcomeResultHandle result =
        daveSessionProcessWelcome(static_cast<DAVESessionHandle>(m_session),
                                  reinterpret_cast<const uint8_t *>(welcome.constData()),
                                  static_cast<size_t>(welcome.size()), ids.data(), ids.size());
    if (!result) {
        wlog(QStringLiteral("dave"), QStringLiteral("the invitation into the group was refused"));
        return false;
    }

    daveWelcomeResultDestroy(result);
    wlog(QStringLiteral("dave"), QStringLiteral("joined the key group"));
    return true;
#else
    Q_UNUSED(welcome) Q_UNUSED(knownUserIds)
    return false;
#endif
}

bool DaveSession::applyKeys(const QString &selfUserId, const QSet<QString> &otherUserIds)
{
#ifdef SINGULARITY_HAVE_DAVE
    if (!m_session || !m_encryptor)
        return false;

    // The old keys are only safe to free once the new ones are in place, so
    // the previous set is collected and released at the end.
    void *const oldMine = m_myKey;
    const QHash<QString, void *> oldTheirs = m_theirKeys;
    m_theirKeys.clear();

    // Our own key, used for everything we send.
    DAVEKeyRatchetHandle mine =
        daveSessionGetKeyRatchet(static_cast<DAVESessionHandle>(m_session),
                                 selfUserId.toUtf8().constData());
    if (!mine) {
        wlog(QStringLiteral("dave"), QStringLiteral("no key for us yet"));
        m_theirKeys = oldTheirs;
        return false;
    }

    m_myKey = mine;
    daveEncryptorSetKeyRatchet(static_cast<DAVEEncryptorHandle>(m_encryptor), mine);

    // One key per other person, so each of their streams can be read.
    for (const QString &userId : otherUserIds) {
        if (userId == selfUserId)
            continue;

        DAVEKeyRatchetHandle theirs =
            daveSessionGetKeyRatchet(static_cast<DAVESessionHandle>(m_session),
                                     userId.toUtf8().constData());
        if (!theirs)
            continue;

        void *&decryptor = m_decryptors[userId];
        if (!decryptor)
            decryptor = daveDecryptorCreate();

        daveDecryptorTransitionToKeyRatchet(static_cast<DAVEDecryptorHandle>(decryptor), theirs);
        m_theirKeys.insert(userId, theirs);
    }

    // Now nothing points at the previous set.
    if (oldMine)
        daveKeyRatchetDestroy(static_cast<DAVEKeyRatchetHandle>(oldMine));
    for (void *key : oldTheirs)
        daveKeyRatchetDestroy(static_cast<DAVEKeyRatchetHandle>(key));

    wlog(QStringLiteral("dave"), QStringLiteral("keys in place for us and %1 others")
                                     .arg(m_decryptors.size()));
    return true;
#else
    Q_UNUSED(selfUserId) Q_UNUSED(otherUserIds)
    return false;
#endif
}

QByteArray DaveSession::encrypt(const QByteArray &frame, quint32 ssrc, bool video)
{
#ifdef SINGULARITY_HAVE_DAVE
    if (!m_encryptor || frame.isEmpty())
        return {};

    auto *encryptor = static_cast<DAVEEncryptorHandle>(m_encryptor);
    if (!daveEncryptorHasKeyRatchet(encryptor))
        return {};

    const DAVEMediaType media = video ? DAVE_MEDIA_TYPE_VIDEO : DAVE_MEDIA_TYPE_AUDIO;

    const size_t room = daveEncryptorGetMaxCiphertextByteSize(
        encryptor, media, static_cast<size_t>(frame.size()));

    QByteArray out(static_cast<int>(room), '\0');
    size_t written = 0;

    const DAVEEncryptorResultCode code =
        daveEncryptorEncrypt(encryptor, media, ssrc,
                             reinterpret_cast<const uint8_t *>(frame.constData()),
                             static_cast<size_t>(frame.size()),
                             reinterpret_cast<uint8_t *>(out.data()), room, &written);

    if (code != DAVE_ENCRYPTOR_RESULT_CODE_SUCCESS || written == 0)
        return {};

    out.resize(static_cast<int>(written));
    return out;
#else
    Q_UNUSED(frame) Q_UNUSED(ssrc) Q_UNUSED(video)
    return {};
#endif
}

QByteArray DaveSession::decrypt(const QString &userId, const QByteArray &frame, bool video)
{
#ifdef SINGULARITY_HAVE_DAVE
    const auto it = m_decryptors.constFind(userId);
    if (it == m_decryptors.constEnd() || frame.isEmpty())
        return {};

    auto *decryptor = static_cast<DAVEDecryptorHandle>(it.value());
    const DAVEMediaType media = video ? DAVE_MEDIA_TYPE_VIDEO : DAVE_MEDIA_TYPE_AUDIO;

    const size_t room = daveDecryptorGetMaxPlaintextByteSize(
        decryptor, media, static_cast<size_t>(frame.size()));

    QByteArray out(static_cast<int>(room), '\0');
    size_t written = 0;

    const DAVEDecryptorResultCode code =
        daveDecryptorDecrypt(decryptor, media,
                             reinterpret_cast<const uint8_t *>(frame.constData()),
                             static_cast<size_t>(frame.size()),
                             reinterpret_cast<uint8_t *>(out.data()), room, &written);

    if (code != DAVE_DECRYPTOR_RESULT_CODE_SUCCESS || written == 0)
        return {};

    out.resize(static_cast<int>(written));
    return out;
#else
    Q_UNUSED(userId) Q_UNUSED(frame) Q_UNUSED(video)
    return {};
#endif
}
