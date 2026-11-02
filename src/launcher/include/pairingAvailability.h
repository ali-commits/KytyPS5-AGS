#ifndef AGS_PAIRING_AVAILABILITY_H
#define AGS_PAIRING_AVAILABILITY_H
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
// Inspect only the private, current one-use enrollment ticket. Never return its token.
inline bool PairingEnrollmentOpen(const QString& profile, qint64 now_ms) {
	const auto path = QFile::encodeName(QDir(profile).filePath("pairing-ticket.json"));
	const int  fd   = ::open(path.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0) return false;
	struct stat info {};
	const bool  valid = ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
	                    info.st_uid == ::geteuid() && (info.st_mode & 077) == 0 &&
	                    info.st_size > 0 && info.st_size <= 4096;
	QByteArray  data;
	if (valid) {
		data.resize(static_cast<int>(info.st_size));
		if (::read(fd, data.data(), data.size()) != data.size()) data.clear();
	}
	::close(fd);
	if (data.isEmpty()) return false;
	const auto ticket = QJsonDocument::fromJson(data).object();
	const auto expiry = ticket.value("expires_at_unix_ms").toDouble(0);
	return expiry > now_ms && expiry <= now_ms + 120000;
}
#endif
