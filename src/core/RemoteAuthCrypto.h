#pragma once

#include <QByteArray>

// The cryptography behind signing in by scanning a QR code with the phone app.
//
// Discord's "remote auth" hands the desktop nothing in the clear. The desktop
// makes an RSA key, shows its fingerprint as the QR code, and everything the
// phone approves - a proof challenge, who you are, and finally the token -
// comes back encrypted to that key. So this holds a 2048-bit RSA key and does
// the three things the handshake needs: hand out the public half, prove we own
// the private half, and open what was sealed to it.
//
// Built on Windows' own CNG (the same library that backs the system's TLS), so
// there is no third-party crypto to ship for it and nothing new to trust.
class RemoteAuthCrypto
{
public:
    RemoteAuthCrypto();
    ~RemoteAuthCrypto();

    RemoteAuthCrypto(const RemoteAuthCrypto &) = delete;
    RemoteAuthCrypto &operator=(const RemoteAuthCrypto &) = delete;

    // Makes the key. False, having logged why, if the platform refuses.
    bool generate();

    // The public key as the standard SubjectPublicKeyInfo DER, which is what
    // Discord's gateway expects (base64 of exactly these bytes).
    QByteArray publicKeyDer() const { return m_spki; }

    // base64url, unpadded, of SHA-256 of the public key. This is the string in
    // the QR code, and the gateway sends it back so the phone can show it.
    QByteArray fingerprint() const;

    // Opens something RSA-OAEP(SHA-256) sealed to our public key: the proof
    // nonce, the "who you are" payload, and the token. Empty on failure.
    QByteArray decrypt(const QByteArray &cipher) const;

    static QByteArray sha256(const QByteArray &data);

    // base64url without the '=' padding, the form Discord uses everywhere in
    // this flow.
    static QByteArray base64Url(const QByteArray &data);

private:
    void reset();

    // BCRYPT_KEY_HANDLE, kept opaque so windows.h stays out of this header.
    void *m_key = nullptr;
    QByteArray m_spki;
};
