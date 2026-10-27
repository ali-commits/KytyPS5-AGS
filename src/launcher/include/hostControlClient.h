#ifndef LAUNCHER_INCLUDE_HOST_CONTROL_CLIENT_H_
#define LAUNCHER_INCLUDE_HOST_CONTROL_CLIENT_H_

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class QTimer;

// Client of the headless Kyty host daemon: control.sock requests plus the systemd user unit.
class HostControlClient: public QObject {
	Q_OBJECT
public:
	// Empty path: $KYTY_HOST_SOCKET, else $XDG_RUNTIME_DIR/kyty-host/control.sock.
	explicit HostControlClient(QString socket_path = {}, QObject* parent = nullptr);
	~HostControlClient() override;

	// Async: the blocking socket call runs on a worker thread, `done` runs on this thread.
	void Request(const QJsonObject&                                    request,
	             std::function<void(QJsonObject reply, QString error)> done);

	void StartDaemon();         // systemctl --user start kyty-host.service
	void RestartDaemon();       // systemctl --user restart kyty-host.service
	void StopDaemon();          // systemctl --user stop kyty-host.service
	void SetAutostart(bool on); // systemctl --user enable|disable kyty-host.service
	void QueryAutostart(std::function<void(bool enabled)> done); // systemctl --user is-enabled

	// Tests inject a fake script. Default: $KYTY_HOST_SYSTEMCTL, else "systemctl".
	void SetSystemctl(const QString& program) { m_systemctl = program; }

	static constexpr char UNIT[] = "kyty-host.service";

signals:
	void StatusChanged(QJsonObject status); // 1 s poll; an empty object = daemon not reachable
	void CommandFailed(QString message);    // a systemctl action did not succeed

private:
	void Poll();
	void RunSystemctl(const QStringList& arguments, bool report, std::function<void(bool ok)> done);

	QString m_socket;
	QString m_systemctl;
	QTimer* m_timer;
	bool    m_polling = false;
};

#endif /* LAUNCHER_INCLUDE_HOST_CONTROL_CLIENT_H_ */
