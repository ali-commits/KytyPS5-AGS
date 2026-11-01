#ifndef LAUNCHER_INCLUDE_HOST_DAEMON_H_
#define LAUNCHER_INCLUDE_HOST_DAEMON_H_

#include "compatibilityDatabase.h"
#include "gameLibrary.h"
#include "hostApi.h"
#include "hostSocket.h"
#include "streamingBackend.h"

#include <QFileSystemWatcher>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>

// The only Kyty host: owns the streaming backend, the game library, the library API on api.sock
// and the control protocol on control.sock.
class HostDaemon: public QObject {
	Q_OBJECT
public:
	explicit HostDaemon(QObject* parent = nullptr);
	~HostDaemon() override;

	// What a pending restart may do now: start (backend idle), stop first, or wait (a streamed
	// game is running, or the backend is already stopping).
	enum class RestartStep { Wait, StopNow, StartNow };
	[[nodiscard]] static RestartStep PlanRestart(StreamingBackend::State state,
	                                             bool                    stream_game_running);
	// Delay before the next hosting retry after an Error: 5 s, doubling, capped at 60 s.
	[[nodiscard]] static int NextRetryMs(int previous_ms);
	[[nodiscard]] bool       RetryScheduled() const { return m_retry.isActive(); }
	[[nodiscard]] int        RetryIntervalMs() const { return m_retry.interval(); }

	[[nodiscard]] bool Start(QString* error);
	// Stops hosting, then emits Finished() once the backend is Stopped or Error (bounded native shutdown plus cleanup).
	void Shutdown();

	// True while an emulator holds the shared lock $XDG_RUNTIME_DIR/kytyps5/emulator.lock.
	[[nodiscard]] static bool LocalEmulatorRunning();

signals:
	void Finished();

private:
	void Control(const QJsonObject& request, std::function<void(HostReply)> done);
	[[nodiscard]] QJsonObject    StatusObject() const;
	[[nodiscard]] QJsonObject    PreferencesObject() const;
	[[nodiscard]] QJsonObject    SetPreferences(const QJsonObject& request);
	[[nodiscard]] static QString Interpreter();
	[[nodiscard]] bool           LocalGameRunning() const;
	void                         ReloadLibrary();
	void                         WatchSettings();
	void                         OnBackendChanged();
	void                         StartHosting(const StreamingSettings& settings);
	void                         ArmRetry();

	CompatibilityDatabase m_compat;
	GameLibrary           m_library;
	StreamingBackend      m_backend;
	HostApi               m_api;
	HostSocketServer      m_control;
	HostSocketServer      m_api_server;
	QFileSystemWatcher    m_watcher;
	QTimer                m_reload, m_refresh, m_shutdown_cap, m_retry;
	int                   m_retry_ms        = 0;
	int                   m_lock_fd         = -1;
	bool                  m_restart_pending = false;
	bool                  m_shutting_down   = false;
	bool                  m_started         = false;
	StreamingSettings     m_restart_settings;
};

// Applies the u59 preset shipped next to the launcher; returns a warning, or an empty string.
QString ApplyBundledPreset();

int RunHostDaemon(int argc, char** argv);

#endif /* LAUNCHER_INCLUDE_HOST_DAEMON_H_ */
