#ifndef LAUNCHER_INCLUDE_HOST_CONTROL_CLIENT_H_
#define LAUNCHER_INCLUDE_HOST_CONTROL_CLIENT_H_

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class QTimer;
class QProcess;

// Socket client; the GUI lifetime owner controls its own --host child, never a service.
class HostControlClient: public QObject {
	Q_OBJECT
public:
	// Empty path: $KYTY_HOST_SOCKET, else $XDG_RUNTIME_DIR/kyty-host/control.sock.
	explicit HostControlClient(QString socket_path = {}, QObject* parent = nullptr);
	~HostControlClient() override;

	// Async: the blocking socket call runs on a worker thread, `done` runs on this thread.
	void Request(const QJsonObject&                                    request,
	             std::function<void(QJsonObject reply, QString error)> done);

	void SetLifetimeOwner(const QString& executable = {});
	void ExitOwnedHost();
	void StartDaemon();
	void RestartDaemon();
	void StopDaemon();
	void SetAutostart(bool on); // per-user graphical login, minimized
	void QueryAutostart(std::function<void(bool enabled)> done);

	void SetAutostartDirectory(const QString& path) { m_autostart_directory = path; }

signals:
	void OwnedHostStopped(QString error);
	void StatusChanged(QJsonObject status); // 1 s poll; an empty object = daemon not reachable
	void CommandFailed(QString message);    // an owned-host or autostart action failed

private:
	void Poll();

	static HostControlClient* s_owner;
	QProcess*                 m_owned = nullptr;
	QString                   m_executable;
	bool                      m_starting = false;
	bool                      m_restart  = false;
	bool                      m_stopping = false;
	QString                   m_process_error;
	QString                   m_socket;
	QString                   m_autostart_directory;
	QTimer*                   m_timer;
	bool                      m_polling = false;
};

#endif /* LAUNCHER_INCLUDE_HOST_CONTROL_CLIENT_H_ */
