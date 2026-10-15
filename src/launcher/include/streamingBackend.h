#ifndef KYTY_STREAMING_BACKEND_H
#define KYTY_STREAMING_BACKEND_H
#include "streamingApi.h"
#include "streamingAudio.h"
#include "streamingCatalog.h"
#include "streamingSettings.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QMap>
#include <QProcess>
#include <QSet>
#include <QTimer>

#include <memory>
class QLockFile;
class StreamingBackend: public QObject {
	Q_OBJECT
public:
	enum class State { Stopped, Starting, Ready, Stopping, Error };
	Q_ENUM(State)
	explicit StreamingBackend(const QString& profile, QObject* parent = nullptr);
	~StreamingBackend() override;
	State          GetState() const { return m_state; }
	QString        Error() const { return m_error; }
	QString        Version() const { return m_version; }
	QString        Logs() const { return m_logs; }
	QString        Session() const { return m_session; }
	QString        Profile() const { return m_profile; }
	QString        Socket() const { return m_socket; }
	QJsonArray     Pairings() const { return m_pairings; }
	QJsonArray     Clients() const { return m_clients; }
	void           SetCatalog(const QList<StreamingLaunchEntry>& catalog);
	void           SetLocalGameRunning(bool running) { m_local_game = running; }
	void           Start(const StreamingSettings& settings);
	void           Stop();
	void           Refresh();
	void           Pair(const QString& id, const QString& pin, const QString& name);
	void           Unpair(const QString& uuid);
	void           CloseSession();
	static QString PreflightPorts(int base);
signals:
	void Changed();
	void LogAdded(const QString& text);

private:
	using ToolDone = std::function<void(bool, QByteArray)>;
	void    Tool(const QString& program, const QStringList& arguments, ToolDone done,
	             const QByteArray& input = {});
	void    Fail(const QString& error);
	void    Log(const QString& text);
	bool    PrepareProfile();
	bool    WriteConfiguration();
	void    StartProcess();
	void    AfterHostExit();
	void    WriteHostIdentity();
	void    DrainOwnedGames();
	void    CancelTools();
	void    TryFinishStop();
	void    PublishCatalog();
	void    ApiError(const QString& error);
	void    SetState(State state);
	QString m_profile, m_runtime, m_socket, m_username, m_password, m_version, m_error, m_logs,
	    m_session;
	StreamingSettings           m_settings;
	QList<StreamingLaunchEntry> m_catalog;
	QJsonArray                  m_pairings, m_clients;
	State                       m_state = State::Stopped;
	QProcess                    m_host;
	StreamingAudio              m_audio;
	StreamingApi                m_api;
	QTimer                      m_poll, m_start_timeout;
	std::unique_ptr<QLockFile>  m_lock;
	bool                        m_local_game = false, m_refreshing = false, m_catalog_dirty = false;
	bool                        m_failed     = false;
	int                         m_generation = 0;
	QSet<QProcess*>             m_tools;
	QString                     m_host_nonce;
	QString                     m_host_executable;
	QTimer                      m_game_stop_poll;
	QElapsedTimer               m_game_stop_elapsed;
	QMap<qint64, int>           m_game_pidfds;
	QSet<qint64>                m_game_groups;
	QSet<qint64>                m_game_preexec_pids;
	bool                        m_games_stopping = false;
};
#endif
