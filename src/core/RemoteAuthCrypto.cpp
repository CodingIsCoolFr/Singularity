#include "core/RemoteAuthCrypto.h"

#include "core/Logger.h"

#include <QtGlobal>

#ifdef Q_OS_WIN
#include <windows.h>
// bcrypt.h has to come after windows.h.
#include <bcrypt.h>
#endif

namespace {

#ifdef Q_OS_WIN
bool ok(NTSTATUS status)
{
    return status >= 0;
}
#endif

// Wraps a raw RSA public key (its modulus and exponent) in the DER that every
// other program means by "a public key": SubjectPublicKeyInfo. CNG hands back
// the modulus and exponent as plain numbers, but Discord's gateway, like a
// browser's WebCrypto, wants them inside this standard envelope.
//
// Written out by hand rather than pulled from a library because it is a few
// nested length-prefixed fields and the alternative is a second crypto stack.
QByteArray derLength(int length)
{
    QByteArray out;
    if (length < 0x80) {
        out.append(char(length));
    } else if (length < 0x100) {
        out.append(char(0x81));
        out.append(char(length));
    } else {
        out.append(char(0x82));
        out.append(char((length >> 8) & 0xFF));
        out.append(char(length & 0xFF));
    }
    return out;
}

QByteArray derTag(quint8 tag, const QByteArray &body)
{
    QByteArray out;
    out.append(char(tag));
    out.append(derLength(int(body.size())));
    out.append(body);
    return out;
}

// An INTEGER carries a leading zero byte when its top bit is set, so a positive
// number is never read as negative.
QByteArray derInteger(QByteArray value)
{
    if (!value.isEmpty() && (quint8(value.at(0)) & 0x80))
        value.prepend(char(0));
    return derTag(0x02, value);
}

QByteArray spkiFromModulusExponent(const QByteArray &modulus, const QByteArray &exponent)
{
    const QByteArray rsaKey = derTag(0x30, derInteger(modulus) + derInteger(exponent));

    // AlgorithmIdentifier for rsaEncryption (1.2.840.113549.1.1.1), NULL params.
    static const unsigned char kRsaOid[] = {0x06, 0x09, 0x2a, 0x86, 0x48, 0x86,
                                            0xf7, 0x0d, 0x01, 0x01, 0x01, 0x05, 0x00};
    const QByteArray algorithm =
        derTag(0x30, QByteArray(reinterpret_cast<const char *>(kRsaOid), sizeof(kRsaOid)));

    // The key goes inside a BIT STRING, which starts with a "0 unused bits" byte.
    const QByteArray bitString = derTag(0x03, QByteArray(1, 0) + rsaKey);

    return derTag(0x30, algorithm + bitString);
}

} // namespace

RemoteAuthCrypto::RemoteAuthCrypto() = default;

RemoteAuthCrypto::~RemoteAuthCrypto()
{
    reset();
}

void RemoteAuthCrypto::reset()
{
#ifdef Q_OS_WIN
    if (m_key) {
        BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(m_key));
        m_key = nullptr;
    }
#endif
    m_spki.clear();
}

bool RemoteAuthCrypto::generate()
{
    reset();

#ifdef Q_OS_WIN
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!ok(BCryptOpenAlgorithmProvider(&alg, BCRYPT_RSA_ALGORITHM, nullptr, 0))) {
        wlog(QStringLiteral("qr"), QStringLiteral("could not open the RSA provider"));
        return false;
    }

    BCRYPT_KEY_HANDLE key = nullptr;
    bool good = ok(BCryptGenerateKeyPair(alg, &key, 2048, 0)) && ok(BCryptFinalizeKeyPair(key, 0));

    if (good) {
        ULONG size = 0;
        good = ok(BCryptExportKey(key, nullptr, BCRYPT_RSAPUBLIC_BLOB, nullptr, 0, &size, 0)) && size > 0;
        QByteArray blob(int(size), Qt::Uninitialized);
        good = good
            && ok(BCryptExportKey(key, nullptr, BCRYPT_RSAPUBLIC_BLOB,
                                  reinterpret_cast<PUCHAR>(blob.data()), size, &size, 0));

        if (good) {
            // The blob is a header then the exponent then the modulus, both
            // big-endian, which is the order DER wants them in anyway.
            const auto *header = reinterpret_cast<const BCRYPT_RSAKEY_BLOB *>(blob.constData());
            const int headerSize = int(sizeof(BCRYPT_RSAKEY_BLOB));
            const int expLen = int(header->cbPublicExp);
            const int modLen = int(header->cbModulus);
            if (blob.size() >= headerSize + expLen + modLen) {
                const QByteArray exponent = blob.mid(headerSize, expLen);
                const QByteArray modulus = blob.mid(headerSize + expLen, modLen);
                m_spki = spkiFromModulusExponent(modulus, exponent);
            } else {
                good = false;
            }
        }
    }

    if (good) {
        m_key = key;
    } else {
        if (key)
            BCryptDestroyKey(key);
        wlog(QStringLiteral("qr"), QStringLiteral("could not make the sign-in key"));
    }

    BCryptCloseAlgorithmProvider(alg, 0);
    return good && !m_spki.isEmpty();
#else
    return false;
#endif
}

QByteArray RemoteAuthCrypto::fingerprint() const
{
    return base64Url(sha256(m_spki));
}

QByteArray RemoteAuthCrypto::decrypt(const QByteArray &cipher) const
{
#ifdef Q_OS_WIN
    if (!m_key || cipher.isEmpty())
        return {};

    BCRYPT_OAEP_PADDING_INFO padding{};
    padding.pszAlgId = BCRYPT_SHA256_ALGORITHM;
    padding.pbLabel = nullptr;
    padding.cbLabel = 0;

    ULONG produced = 0;
    NTSTATUS status = BCryptDecrypt(
        static_cast<BCRYPT_KEY_HANDLE>(m_key),
        reinterpret_cast<PUCHAR>(const_cast<char *>(cipher.constData())), ULONG(cipher.size()), &padding,
        nullptr, 0, nullptr, 0, &produced, BCRYPT_PAD_OAEP);
    if (!ok(status) || produced == 0)
        return {};

    QByteArray plain(int(produced), Qt::Uninitialized);
    status = BCryptDecrypt(
        static_cast<BCRYPT_KEY_HANDLE>(m_key),
        reinterpret_cast<PUCHAR>(const_cast<char *>(cipher.constData())), ULONG(cipher.size()), &padding,
        nullptr, 0, reinterpret_cast<PUCHAR>(plain.data()), produced, &produced, BCRYPT_PAD_OAEP);
    if (!ok(status))
        return {};

    plain.resize(int(produced));
    return plain;
#else
    Q_UNUSED(cipher);
    return {};
#endif
}

QByteArray RemoteAuthCrypto::sha256(const QByteArray &data)
{
#ifdef Q_OS_WIN
    QByteArray digest(32, Qt::Uninitialized);
    const NTSTATUS status = BCryptHash(
        BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
        reinterpret_cast<PUCHAR>(const_cast<char *>(data.constData())), ULONG(data.size()),
        reinterpret_cast<PUCHAR>(digest.data()), 32);
    if (!ok(status))
        return {};
    return digest;
#else
    Q_UNUSED(data);
    return {};
#endif
}

QByteArray RemoteAuthCrypto::base64Url(const QByteArray &data)
{
    return data.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}
