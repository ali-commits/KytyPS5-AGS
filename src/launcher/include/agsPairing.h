#ifndef KYTY_AGS_PAIRING_H
#define KYTY_AGS_PAIRING_H

// AGS password pairing: launcher side. Computes the SRP-6a verifier for a
// password and writes the owner-only verifier file that the Sunshine host reads.
// The math (RFC 5054 3072-bit group, g = 5, SHA-256, identity "ags") matches
// the host's src/ags_pairing.cpp byte for byte so a password set here pairs
// there. See docs/superpowers/specs/2026-10-28-ags-pairing-protocol.md.

#include <QByteArray>
#include <QString>

namespace AgsPairing {

/// Minimum password length enforced in the UI and by SetPassword.
inline constexpr int MINIMUM_PASSWORD_LENGTH = 8;

/// Path of the verifier file inside the streaming profile directory.
QString VerifierFilePath();

/// True when a verifier file already exists at @p path.
bool VerifierExists(const QString& path);

/// Lowercase-hex SRP verifier v = g^x mod N for @p password with @p salt.
/// Exposed for the shared cross-repo test vector. @p salt is raw bytes.
QByteArray ComputeVerifierHex(const QByteArray& salt, const QString& password);

/// Write the verifier file (salt + verifier, mode 0600, atomic) for @p password.
/// A fresh random 16-byte salt is generated. Returns false and sets @p error on
/// failure (too-short password, crypto failure, or file write error). The
/// plaintext password is never stored or logged.
bool SetPassword(const QString& path, const QString& password, QString* error);

/// Remove the verifier file so password pairing is disabled. Returns true when
/// the file is gone afterwards (including when it was already absent).
bool RemovePassword(const QString& path, QString* error);

} // namespace AgsPairing

#endif
