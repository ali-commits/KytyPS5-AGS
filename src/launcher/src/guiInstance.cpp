#include "guiInstance.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif
GuiInstance::GuiInstance(const QString& directory, QObject* parent)
    : QObject(parent), m_directory(directory) {
	connect(&m_server, &QLocalServer::newConnection, this, &GuiInstance::Accept);
	m_server.setSocketOptions(QLocalServer::UserAccessOption);
	m_server.setMaxPendingConnections(16);
}
QString GuiInstance::DefaultDirectory() {
	return QDir(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation))
	    .filePath("ags-gui");
}
bool GuiInstance::Prepare(QString* error) {
#ifdef Q_OS_UNIX
	const auto path = QFile::encodeName(m_directory);
	if (::mkdir(path.constData(), 0700) != 0 && errno != EEXIST) {
		*error = "Cannot create private AGS GUI directory.";
		return false;
	}
	struct stat st {};
	if (::lstat(path.constData(), &st) || !S_ISDIR(st.st_mode) || st.st_uid != ::geteuid() ||
	    (st.st_mode & 077) != 0) {
		*error = "AGS GUI directory must be an owner-private directory (0700), not a symlink.";
		return false;
	}
#else
	if (!QDir().mkpath(m_directory)) {
		*error = "Cannot create GUI directory.";
		return false;
	}
#endif
	m_lock = std::make_unique<QLockFile>(QDir(m_directory).filePath("owner.lock"));
	// A live GUI must never lose ownership merely because it has been running a long time.
	m_lock->setStaleLockTime(0);
	return true;
}
GuiInstance::Result GuiInstance::Start(const QByteArray& command, QString* error, int timeoutMs) {
	if (command != "show" && command != "quiet" && command != "quit") {
		*error = "Unknown GUI command.";
		return Result::Error;
	}
	if (!Prepare(error)) return Result::Error;
	const auto    endpoint = QDir(m_directory).filePath("control");
	QElapsedTimer elapsed;
	elapsed.start();
	do {
		if (command != "quit" && m_lock->tryLock(0)) {
			// Only the exclusive lock owner may remove a crashed predecessor's socket.
			QLocalServer::removeServer(endpoint);
			if (!m_server.listen(endpoint)) {
				*error = m_server.errorString();
				m_lock->unlock();
				return Result::Error;
			}
			return Result::Owner;
		}
		QLocalSocket socket;
		socket.connectToServer(endpoint);
		if (socket.waitForConnected(qMin(100, timeoutMs))) {
			socket.write(command + '\n');
			socket.flush();
			QByteArray reply;
			while (!reply.contains('\n') && elapsed.elapsed() < timeoutMs) {
				reply += socket.readAll();
				if (reply.size() > 1024) break;
				if (reply.contains('\n')) break;
				socket.waitForReadyRead(qMin(100, qMax(1, timeoutMs - int(elapsed.elapsed()))));
				if (socket.state() == QLocalSocket::UnconnectedState) {
					reply += socket.readAll();
					break;
				}
			}
			if (reply == "ok\n") {
				if (command != "quit") return Result::Forwarded;
				// Cleanup completion alone is not GUI exit: confirm ownership was released too.
				while (elapsed.elapsed() < timeoutMs) {
					if (m_lock->tryLock(0)) {
						m_lock->unlock();
						return Result::Forwarded;
					}
					QThread::msleep(10);
				}
			}
			*error = reply.startsWith("error ")
			             ? QString::fromUtf8(reply.mid(6)).trimmed()
			             : "AGS GUI did not confirm completion before timeout.";
			return Result::Error;
		}
		if (command == "quit" && m_lock->tryLock(0)) {
			m_lock->unlock();
			*error = "No running AGS GUI instance.";
			return Result::Error;
		}
		QThread::msleep(10);
	} while (elapsed.elapsed() < timeoutMs);
	*error = "Timed out contacting the existing AGS GUI.";
	return Result::Error;
}
void GuiInstance::Accept() {
	for (qsizetype i = m_quitters.size(); i > 0; --i)
		if (!m_quitters[i - 1]) m_quitters.removeAt(i - 1);
	while (auto* socket = m_server.nextPendingConnection()) {
		if (findChildren<QLocalSocket*>().size() >= 16) {
			socket->abort();
			socket->deleteLater();
			continue;
		}
		socket->setParent(this);
		socket->setReadBufferSize(65);
		auto* timer = new QTimer(socket);
		timer->setSingleShot(true);
		timer->start(3000);
		connect(timer, &QTimer::timeout, socket, &QLocalSocket::abort);
		connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
		auto data = std::make_shared<QByteArray>();
		connect(socket, &QLocalSocket::readyRead, this, [this, socket, timer, data] {
			*data += socket->readAll();
			if (socket->property("handled").toBool()) {
				socket->abort();
				return;
			}
			if (data->size() > 64) {
				socket->abort();
				return;
			}
			if (!data->contains('\n')) return;
			socket->setProperty("handled", true);
			if (*data == "quit\n") {
				timer->start(125000);
				m_quitters.append(socket);
				emit ExitRequested();
				return;
			}
			if (*data == "show\n") emit ShowRequested();
			socket->write((*data == "show\n" || *data == "quiet\n")
			                  ? "ok\n"
							  : "error Unknown GUI command.\n");
			socket->disconnectFromServer();
		});
	}
}
void GuiInstance::CompleteExit(const QString& error) {
	QByteArray reply = error.isEmpty() ? QByteArray("ok\n")
	                                   : "error " + error.simplified().left(900).toUtf8() + '\n';
	for (const auto& socket: m_quitters)
		if (socket) {
			socket->write(reply);
			socket->flush();
			socket->disconnectFromServer();
		}
	m_quitters.clear();
}
