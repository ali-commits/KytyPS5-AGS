#ifndef LAUNCHER_INCLUDE_HOST_SOCKET_H_
#define LAUNCHER_INCLUDE_HOST_SOCKET_H_

#include <QByteArray>
#include <QJsonObject>
#include <QLocalServer>
#include <QObject>
#include <QString>

#include <functional>
#include <optional>

class QLocalSocket;

struct HostReply {
	int        status       = 200;
	QByteArray content_type = "application/json";
	QByteArray body;
	QByteArray etag;
};

// Private line-framed JSON socket. The request is one JSON object terminated by '\n' (at most
// MAX_REQUEST bytes). The reply is a JSON header line {"status","content_type","length","etag"}
// followed by exactly `length` body bytes, after which the server closes the connection.
class HostSocketServer: public QObject {
	Q_OBJECT
public:
	using Handler =
	    std::function<void(const QJsonObject& request, std::function<void(HostReply)> done)>;

	explicit HostSocketServer(Handler handler, QObject* parent = nullptr);

	// Creates a 0700 directory and a 0600 socket; only connections from this user are served.
	[[nodiscard]] bool Listen(const QString& path, QString* error);

	static constexpr qint64 MAX_REQUEST = 64 * 1024;

private:
	void        Accept();
	static void Send(QLocalSocket* socket, const HostReply& reply);

	Handler      m_handler;
	QLocalServer m_server;
};

// Blocking client (poll based, needs no event loop) used by tests and the launcher.
[[nodiscard]] std::optional<HostReply>
HostSocketCall(const QString& path, const QJsonObject& request, int timeout_ms, QString* error);

// $XDG_RUNTIME_DIR/kyty-host, or an empty string when XDG_RUNTIME_DIR is unset.
[[nodiscard]] QString HostRuntimeDirectory();

#endif /* LAUNCHER_INCLUDE_HOST_SOCKET_H_ */
