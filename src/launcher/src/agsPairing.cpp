#include "agsPairing.h"

#include "streamingCatalog.h"
#include "streamingSettings.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace AgsPairing {

namespace {

// RFC 5054 Appendix A, 3072-bit group N (hex), generator g = 5. Identical to
// the host's N_3072_HEX in kyty-streaming-sunshine/src/ags_pairing.cpp.
constexpr const char* N_3072_HEX = "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD1"
                                   "29024E088A67CC74020BBEA63B139B22514A08798E3404DD"
                                   "EF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245"
                                   "E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
                                   "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3D"
                                   "C2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F"
                                   "83655D23DCA3AD961C62F356208552BB9ED529077096966D"
                                   "670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
                                   "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9"
                                   "DE2BCBF6955817183995497CEA956AE515D2261898FA0510"
                                   "15728E5A8AAAC42DAD33170D04507A33A85521ABDF1CBA64"
                                   "ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7"
                                   "ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6B"
                                   "F12FFA06D98A0864D87602733EC86A64521F2B18177B200C"
                                   "BBE117577A615D6C770988C0BAD946E208E24FA074E5AB31"
                                   "43DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF";

QByteArray sha256(const QByteArray& a, const QByteArray& b) {
	EVP_MD_CTX* ctx = EVP_MD_CTX_new();
	EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
	if (!a.isEmpty()) EVP_DigestUpdate(ctx, a.constData(), a.size());
	if (!b.isEmpty()) EVP_DigestUpdate(ctx, b.constData(), b.size());
	QByteArray   out(EVP_MAX_MD_SIZE, Qt::Uninitialized);
	unsigned int len = 0;
	EVP_DigestFinal_ex(ctx, reinterpret_cast<unsigned char*>(out.data()), &len);
	EVP_MD_CTX_free(ctx);
	out.resize(static_cast<int>(len));
	return out;
}

} // namespace

QByteArray ComputeVerifierHex(const QByteArray& salt, const QString& password) {
	// x = H(salt | H("ags" | ":" | P)); v = g^x mod N, PAD()ed to N's byte width.
	const QByteArray identity = QByteArrayLiteral("ags");
	const QByteArray pw       = password.toUtf8();
	QByteArray       inner    = sha256(identity + QByteArrayLiteral(":"), pw);
	QByteArray       x_bytes  = sha256(salt, inner);

	BIGNUM* N = nullptr;
	BN_hex2bn(&N, N_3072_HEX);
	BIGNUM* g = BN_new();
	BN_set_word(g, 5);
	BIGNUM* x   = BN_bin2bn(reinterpret_cast<const unsigned char*>(x_bytes.constData()),
	                        x_bytes.size(), nullptr);
	BIGNUM* v   = BN_new();
	BN_CTX* ctx = BN_CTX_new();
	BN_mod_exp(v, g, x, N, ctx); // v = g^x mod N

	int        width = BN_num_bytes(N);
	QByteArray v_pad(width, '\0');
	BN_bn2binpad(v, reinterpret_cast<unsigned char*>(v_pad.data()), width);

	BN_free(N);
	BN_free(g);
	BN_free(x);
	BN_free(v);
	BN_CTX_free(ctx);

	return v_pad.toHex();
}

QString VerifierFilePath() {
	return QDir(StreamingSettings::ProfileDirectory()).filePath("pairing-srp.json");
}

bool VerifierExists(const QString& path) {
	return QFile::exists(path);
}

bool SetPassword(const QString& path, const QString& password, QString* error) {
	if (password.size() < MINIMUM_PASSWORD_LENGTH) {
		if (error)
			*error = QStringLiteral("Password must be at least %1 characters.")
			             .arg(MINIMUM_PASSWORD_LENGTH);
		return false;
	}

	unsigned char salt_raw[16];
	if (RAND_bytes(salt_raw, sizeof(salt_raw)) != 1) {
		if (error) *error = QStringLiteral("Could not generate a random salt.");
		return false;
	}
	QByteArray salt(reinterpret_cast<const char*>(salt_raw), sizeof(salt_raw));

	QJsonObject record {{"version", 1},
	                    {"group", "rfc5054-3072"},
	                    {"hash", "sha256"},
	                    {"identity", "ags"},
	                    {"salt", QString::fromLatin1(salt.toHex())},
	                    {"verifier", QString::fromLatin1(ComputeVerifierHex(salt, password))}};

	// Ensure the profile directory exists before the atomic write.
	QDir().mkpath(QFileInfo(path).absolutePath());
	QByteArray bytes = QJsonDocument(record).toJson(QJsonDocument::Compact);
	return StreamingCatalog::WritePrivate(path, bytes, error);
}

bool RemovePassword(const QString& path, QString* error) {
	if (!QFile::exists(path)) {
		return true;
	}
	if (!QFile::remove(path)) {
		if (error)
			*error = QStringLiteral("Could not remove the pairing password file %1.").arg(path);
		return false;
	}
	return true;
}

} // namespace AgsPairing
