#ifndef KYTY_STREAMING_BACKEND_H
#define KYTY_STREAMING_BACKEND_H
#include "streamingCatalog.h"
#include "streamingSettings.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QTimer>

#include <functional>
#include <memory>
class QLockFile;
class QLocalSocket;

// The Qt owner adapts its existing control.sock API to one native AGS worker.
class StreamingBackend: public QObject {
	Q_OBJECT
public:
	enum class State { Stopped, Starting, Ready, Stopping, Error, Blocked };
	Q_ENUM(State)
	using Done = std::function<void(QJsonObject, QString)>;
	explicit StreamingBackend(const QString& profile, QObject* parent = nullptr);
	~StreamingBackend() override;
	State          GetState() const { return m_state; }
	QString        Error() const { return m_error; }
	QString        Version() const { return QStringLiteral("Native AGS"); }
	QString        Logs() const { return m_logs; }
	QString        Session() const { return m_session; }
	QString        Profile() const { return m_profile; }
	QString        NativeProfile() const { return m_native_profile; }
	QString        Socket() const { return m_socket; }
	QJsonArray     Pairings() const { return {}; }
	QJsonArray     Clients() const { return m_clients; }
	QJsonArray     ControllerAudioReceivers() const { return {}; }
	QJsonObject    ControllerAudioStatus() const;
	QJsonObject    NativeStatus() const { return m_status; }
	QJsonObject    Hid() const { return m_status.value("input_capabilities").toObject(); }
	void           SetCatalog(const QList<StreamingLaunchEntry>& catalog);
	void           SetClientCatalog(const QHash<QString, QList<StreamingLaunchEntry>>& catalog);
	void           SetLocalGameRunning(bool running) { m_local_game = running; }
	void           Start(const StreamingSettings& settings);
	void           Stop();
	void           Refresh();
	void           Pair(const QString& id, const QString& pin, const QString& name);
	void           ExportPairing(const QString& address, Done done);
	void           Unpair(const QString& uuid, Done done = {});
	void           CloseSession(Done done = {});
	void           ReloadSettings(const StreamingSettings& settings, Done done = {});
	void           SetControllerAudioReceiverEnabled(const QString& id, bool enabled);
	void           RemoveControllerAudioReceiver(const QString& id);
	static QString PreflightPorts(int port, int unused_controller_audio_port = 0);
	bool           StreamGameRunning() const;
	bool           ClosingSession() const { return m_closing; }
	QString        RunningGameId() const;
	void           PublishCatalog();
	void           SetApiSocket(const QString& path) { m_api_socket = path; }
	QString        ApiSocket() const { return m_api_socket; }

signals:
	void Changed();
	void LogAdded(const QString& text);

private:
	void        Request(const QJsonObject& request, Done done, int timeout_ms = 90000);
	void        Tool(const QStringList& arguments, const QByteArray& input,
	                 std::function<void(bool)> done);
	QJsonObject Configuration() const;
	bool        PrepareProfile();
	void        StartProcess();
	void        AcceptStatus(const QJsonObject& status);
	void        ClearStatus();
	void        Fail(const QString& error);
	void        Log(const QString& text);
	void        SetState(State state);
	void        FinishStop();
	void        CancelRequests();
	QString     m_profile, m_native_profile, m_socket, m_api_socket, m_error, m_logs, m_session;
	StreamingSettings          m_settings;
	QJsonObject                m_status;
	QJsonArray                 m_clients;
	State                      m_state = State::Stopped;
	QProcess                   m_host, m_tool;
	QTimer                     m_poll, m_start_timeout, m_stop_timeout;
	std::unique_ptr<QLockFile> m_lock;
	QSet<QLocalSocket*>        m_requests;
	bool m_local_game = false, m_refreshing = false, m_failed = false, m_closing = false;
	bool m_catalog_dirty = false, m_reloading = false;
	bool m_startup_cancel = false;
	int  m_generation = 0;
};
#endif
