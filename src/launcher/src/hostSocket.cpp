#include "hostSocket.h"

#include "gameContent.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QPointer>

#include <chrono>
#include <utility>

#ifdef __linux__
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace {
constexpr qint64 MAX_HEADER = 4096;
constexpr qint64 MAX_REPLY  = static_cast<qint64>(GameContent::MaxImageSize) + 65536;
} // namespace

HostSocketServer::HostSocketServer(Handler handler, QObject* parent)
    : QObject(parent), m_handler(std::move(handler)) {}

QString HostRuntimeDirectory() {
	const auto runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
	return runtime.isEmpty() ? QString() : runtime + QStringLiteral("/kyty-host");
}

#ifdef __linux__

bool HostSocketServer::Listen(const QString& path, QString* error) {
	if (m_server.isListening()) {
		return true;
	}
	const auto  dir = QFileInfo(path).absolutePath();
	struct stat st {};
	if (!QDir().mkpath(dir) || ::lstat(QFile::encodeName(dir).constData(), &st) != 0 ||
	    !S_ISDIR(st.st_mode) || st.st_uid != ::geteuid()) {
		if (error != nullptr) {
			*error = QStringLiteral("%1 is not a directory owned by this user").arg(dir);
		}
		return false;
	}
	if (::chmod(QFile::encodeName(dir).constData(), 0700) != 0) {
		if (error != nullptr) {
			*error = QStringLiteral("Cannot make %1 private").arg(dir);
		}
		return false;
	}
	QLocalServer::removeServer(path); // stale socket from a killed daemon; the directory is ours
	m_server.setSocketOptions(QLocalServer::UserAccessOption);
	if (!m_server.listen(path) || ::chmod(QFile::encodeName(path).constData(), 0600) != 0) {
		if (error != nullptr) {
			*error = m_server.errorString();
		}
		m_server.close();
		return false;
	}
	connect(&m_server, &QLocalServer::newConnection, this, &HostSocketServer::Accept);
	return true;
}

void HostSocketServer::Accept() {
	while (auto* socket = m_server.nextPendingConnection()) {
		ucred     cred {};
		socklen_t len = sizeof(cred);
		if (::getsockopt(static_cast<int>(socket->socketDescriptor()), SOL_SOCKET, SO_PEERCRED,
		                 &cred, &len) != 0 ||
		    cred.uid != ::geteuid()) {
			socket->abort();
			socket->deleteLater();
			continue;
		}
		connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
		connect(socket, &QLocalSocket::readyRead, socket, [this, socket] {
			if (socket->property("kyty_done").toBool()) {
				return;
			}
			if (socket->bytesAvailable() > MAX_REQUEST) {
				socket->abort();
				return;
			}
			if (!socket->canReadLine()) {
				return;
			}
			socket->setProperty("kyty_done", true);
			const auto doc = QJsonDocument::fromJson(socket->readLine().trimmed());
			if (!doc.isObject()) {
				Send(socket, {400, "application/json", R"({"error":"invalid-json"})", {}});
				return;
			}
			QPointer<QLocalSocket> guard(socket);
			m_handler(doc.object(), [guard](HostReply r) {
				if (guard) {
					Send(guard, r);
				}
			});
		});
	}
}

void HostSocketServer::Send(QLocalSocket* socket, const HostReply& r) {
	if (socket->property("kyty_sent").toBool()) {
		return;
	}
	socket->setProperty("kyty_sent", true);
	const QJsonObject header {{"status", r.status},
	                          {"content_type", QString::fromLatin1(r.content_type)},
	                          {"length", static_cast<qint64>(r.body.size())},
	                          {"etag", QString::fromLatin1(r.etag)}};
	socket->write(QJsonDocument(header).toJson(QJsonDocument::Compact) + '\n' + r.body);
	socket->disconnectFromServer(); // flushes the pending write, then closes
}

namespace {

using Clock = std::chrono::steady_clock;

// Waits for `events` on fd until the deadline; false on timeout or error.
bool WaitFd(int fd, short events, Clock::time_point deadline) {
	for (;;) {
		const auto left =
		    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
		if (left.count() <= 0) {
			return false;
		}
		pollfd    p {fd, events, 0};
		const int r = ::poll(&p, 1, static_cast<int>(left.count()));
		if (r > 0) {
			return true;
		}
		if (r == 0 || errno != EINTR) {
			return false;
		}
	}
}

struct Fd {
	int fd;
	explicit Fd(int f): fd(f) {}
	~Fd() {
		if (fd >= 0) ::close(fd);
	}
	Fd(const Fd&)            = delete;
	Fd& operator=(const Fd&) = delete;
};

} // namespace

std::optional<HostReply> HostSocketCall(const QString& path, const QJsonObject& request,
                                        int timeout_ms, QString* error) {
	const auto fail = [error](const QString& what) -> std::optional<HostReply> {
		if (error != nullptr) {
			*error = what;
		}
		return std::nullopt;
	};
	const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
	const auto line     = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
	if (line.size() > HostSocketServer::MAX_REQUEST) {
		return fail(QStringLiteral("Request is too large"));
	}
	sockaddr_un addr {};
	const auto  encoded = QFile::encodeName(path);
	if (encoded.isEmpty() || static_cast<size_t>(encoded.size()) >= sizeof(addr.sun_path)) {
		return fail(QStringLiteral("Invalid socket path"));
	}
	addr.sun_family = AF_UNIX;
	std::memcpy(addr.sun_path, encoded.constData(), static_cast<size_t>(encoded.size()));
	Fd s(::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
	if (s.fd < 0) {
		return fail(QStringLiteral("Cannot create socket"));
	}
	if (::connect(s.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
		int       err = errno;
		socklen_t len = sizeof(err);
		if (err != EINPROGRESS || !WaitFd(s.fd, POLLOUT, deadline) ||
		    ::getsockopt(s.fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
			return fail(QStringLiteral("Cannot connect to %1").arg(path));
		}
	}
	for (qsizetype sent = 0; sent < line.size();) {
		const auto n = ::send(s.fd, line.constData() + sent,
		                      static_cast<size_t>(line.size() - sent), MSG_NOSIGNAL);
		if (n > 0) {
			sent += n;
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else if ((n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) &&
		           WaitFd(s.fd, POLLOUT, deadline)) {
			continue;
		} else {
			return fail(QStringLiteral("Cannot send request"));
		}
	}
	QByteArray buffer;
	qint64     body_length = -1;
	qsizetype  header_end  = -1;
	HostReply  reply;
	for (;;) {
		if (header_end < 0) {
			header_end = buffer.indexOf('\n');
			if (header_end < 0 && buffer.size() > MAX_HEADER) {
				return fail(QStringLiteral("Reply header is too large"));
			}
			if (header_end > MAX_HEADER) {
				return fail(QStringLiteral("Reply header is too large"));
			}
			if (header_end >= 0) {
				const auto doc = QJsonDocument::fromJson(buffer.left(header_end));
				if (!doc.isObject()) {
					return fail(QStringLiteral("Invalid reply header"));
				}
				const auto h = doc.object();
				body_length  = h["length"].toVariant().toLongLong();
				reply.status = h["status"].toInt(0);
				if (body_length < 0 || body_length > MAX_REPLY || reply.status < 100 ||
				    reply.status > 599) {
					return fail(QStringLiteral("Invalid reply header"));
				}
				reply.content_type = h["content_type"].toString().toLatin1();
				reply.etag         = h["etag"].toString().toLatin1();
			}
		}
		if (header_end >= 0 && buffer.size() - (header_end + 1) >= body_length) {
			reply.body = buffer.mid(header_end + 1, static_cast<qsizetype>(body_length));
			return reply;
		}
		char       chunk[16384];
		const auto n = ::recv(s.fd, chunk, sizeof(chunk), 0);
		if (n > 0) {
			buffer.append(chunk, static_cast<qsizetype>(n));
		} else if (n == 0) {
			return fail(QStringLiteral("Connection closed before the reply was complete"));
		} else if (errno == EINTR) {
			continue;
		} else if ((errno == EAGAIN || errno == EWOULDBLOCK) && WaitFd(s.fd, POLLIN, deadline)) {
			continue;
		} else {
			return fail(QStringLiteral("Timed out waiting for the reply"));
		}
	}
}

#else // The host daemon is Linux only; other platforms build the symbols but never serve.

bool HostSocketServer::Listen(const QString&, QString* error) {
	if (error != nullptr) {
		*error = QStringLiteral("The host socket is only supported on Linux");
	}
	return false;
}
void                     HostSocketServer::Accept() {}
void                     HostSocketServer::Send(QLocalSocket*, const HostReply&) {}
std::optional<HostReply> HostSocketCall(const QString&, const QJsonObject&, int, QString* error) {
	if (error != nullptr) {
		*error = QStringLiteral("The host socket is only supported on Linux");
	}
	return std::nullopt;
}

#endif
