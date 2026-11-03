#include "hostDaemon.h"
#include "clientProfiles.h"
#include <QUuid>
#include <sys/stat.h>

#include "common/u59Preset.h"
#include "emulatorCommand.h"
#include "streamingSettings.h"
#include "pairingAvailability.h"
#include <QDateTime>

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaEnum>
#include <QNetworkInterface>
#include <QProcessEnvironment>
#include <QSettings>
#include <QSocketNotifier>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <signal.h>
#include <sys/file.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr int SHUTDOWN_CAP_MS = 102000;
constexpr int RETRY_FIRST_MS  = 5000;
constexpr int RETRY_MAX_MS    = 60000;

HostReply Json(const QJsonObject& object) {
	return {200, "application/json", QJsonDocument(object).toJson(QJsonDocument::Compact), {}};
}

QJsonObject Fail(const QString& error) {
	return {{"ok", false}, {"error", error}};
}

bool Idle(StreamingBackend::State state) {
	return state == StreamingBackend::State::Stopped || state == StreamingBackend::State::Error;
}

QString LanAddress() {
	for (const auto& interface: QNetworkInterface::allInterfaces()) {
		if (!interface.flags().testFlag(QNetworkInterface::IsUp) ||
		    interface.flags().testFlag(QNetworkInterface::IsLoopBack))
			continue;
		for (const auto& entry: interface.addressEntries())
			if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol)
				return entry.ip().toString();
	}
	return {};
}

StreamingSettings ReadSettings() {
	QSettings file(QDir(StreamingSettings::ProfileDirectory()).filePath("streaming.ini"),
	               QSettings::IniFormat);
	return StreamingSettings::Read(file);
}

bool Same(const StreamingSettings& a, const StreamingSettings& b) {
	return a.port == b.port && a.resolution == b.resolution && a.fps == b.fps &&
	       a.bitrate == b.bitrate && a.codec == b.codec && a.adapter == b.adapter &&
	       a.local_audio == b.local_audio && a.show_local == b.show_local &&
	       a.disconnect_grace == b.disconnect_grace && a.host_name == b.host_name &&
	       a.audio_codec == b.audio_codec && a.encoder == b.encoder &&
	       a.local_audio_sink == b.local_audio_sink && a.fec_enabled == b.fec_enabled;
}

} // namespace

QString ApplyBundledPreset() {
	const QDir                         app_dir(QCoreApplication::applicationDirPath());
	std::vector<std::filesystem::path> dirs {
	    std::filesystem::path(app_dir.absolutePath().toStdU16String())};
	if (QDir parent = app_dir; parent.cdUp()) {
		dirs.emplace_back(parent.absolutePath().toStdU16String());
	}

	const auto result = Common::U59Preset::Load(dirs);
	if (result.status != Common::U59Preset::Status::Loaded) {
		const auto warning = QString::fromStdString(Common::U59Preset::Describe(result));
		qWarning().noquote() << warning;
		return warning;
	}

	for (const QString& key: QProcessEnvironment::systemEnvironment().keys()) {
		if (key == "KYTY_STREAMING_CONFIG_DIR" || key == "KYTY_HOST_NO_AUTOSTART") {
			continue;
		}
		if (key.startsWith("KYTY_", Qt::CaseInsensitive) ||
		    key.startsWith("TRACY_", Qt::CaseInsensitive)) {
			qunsetenv(key.toLocal8Bit().constData());
		}
	}
	for (const auto& entry: result.entries) {
		qputenv(entry.key.c_str(), QByteArray::fromStdString(entry.value));
	}
	return {};
}

HostDaemon::HostDaemon(QObject* parent)
    : QObject(parent), m_compat(QCoreApplication::arguments().contains(QStringLiteral("--local"))),
      m_backend(StreamingSettings::ProfileDirectory()),
      m_api(&m_library,
	        [this] { return HostApi::Status {LocalGameRunning(), m_backend.RunningGameId()}; }),
      m_control([this](const QJsonObject& request, std::function<void(HostReply)> done) {
	      Control(request, std::move(done));
      }),
      m_api_server([this](const QJsonObject& request, std::function<void(HostReply)> done) {
	      m_api.Handle(request, std::move(done));
      }) {
	const auto run      = qEnvironmentVariable("XDG_RUNTIME_DIR");
	const auto lock_dir = QDir(run).filePath("kytyps5");
	if (!run.isEmpty() && !QFileInfo::exists(lock_dir)) {
		QDir().mkpath(lock_dir);
		QFile::setPermissions(lock_dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
	}
	m_api.ConfigureNativeLaunch(Interpreter(), QDir(lock_dir).filePath("emulator.lock"),
	                            qEnvironmentVariable("AGS_SESSION_RUNTIME_ROOT"));
    m_local_deadline.setSingleShot(true);
    m_local_deadline.setInterval(30000);
    connect(&m_local_deadline, &QTimer::timeout, this, [this] {
        m_local_error = tr("Local game did not finish its launch/cleanup before the deadline.");
        if (m_local_start_done) { auto done = std::move(m_local_start_done); done(Json(Fail(m_local_error))); }
        if (m_local_stop_done) { auto done = std::move(m_local_stop_done); done(Json(Fail(m_local_error))); }
        m_local.terminate(); // Never force-kill the ownership guard or clear unknown ownership.
    });
    connect(&m_local, &QProcess::readyReadStandardError, this, [this] {
        m_local_stderr += QString::fromUtf8(m_local.readAllStandardError());
        m_local_stderr = m_local_stderr.right(8192);
    });
    connect(&m_local, &QProcess::readyReadStandardOutput, this, [this] { m_local.readAllStandardOutput(); });
    connect(&m_local, &QProcess::finished, this, &HostDaemon::LocalFinished);
    connect(&m_local, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_local_error = m_local.errorString();
            LocalFinished(-1, QProcess::CrashExit);
        }
    });
    connect(&m_api, &HostApi::LocalGameSpawned, this, [this](const QString& id) {
        if (id != m_local_id || !m_local_start_done) return;
        m_local_started = true;
        m_local_deadline.stop();
        auto done = std::move(m_local_start_done);
        done(Json({{"ok", true}, {"game", id}}));
    });
	m_reload.setSingleShot(true);
	m_reload.setInterval(500);
	m_refresh.setInterval(1000);
	m_shutdown_cap.setSingleShot(true);
	m_shutdown_cap.setInterval(SHUTDOWN_CAP_MS);
	m_retry.setSingleShot(true);
	connect(&m_reload, &QTimer::timeout, this, &HostDaemon::ReloadLibrary);
	connect(&m_refresh, &QTimer::timeout, this, [this] {
		m_backend.SetLocalGameRunning(LocalGameRunning());
		m_backend.Refresh();
		// A restart deferred by a streamed game runs once that game has ended.
		if (m_restart_pending && !m_shutting_down &&
		    PlanRestart(m_backend.GetState(), m_backend.StreamGameRunning()) ==
		        RestartStep::StopNow) {
			m_backend.Stop();
		}
	});
	connect(&m_retry, &QTimer::timeout, this, [this] {
		if (m_shutting_down || m_restart_pending ||
		    m_backend.GetState() != StreamingBackend::State::Error) {
			return;
		}
		if (LocalGameRunning()) { // Local play keeps exclusive ownership until it exits
			ArmRetry();
			return;
		}
		StartHosting(ReadSettings());
	});
	connect(&m_shutdown_cap, &QTimer::timeout, this, [this] {
		m_shutdown_failed = true;
		qWarning() << "AGS host shutdown exceeded its deadline";
		emit Finished();
	});
	connect(&m_compat, &CompatibilityDatabase::Updated, &m_reload, qOverload<>(&QTimer::start));
	connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] {
		m_reload.start();
		WatchSettings();
	});
	connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] {
		m_reload.start();
		WatchSettings();
	});
	connect(&m_backend, &StreamingBackend::Changed, this, &HostDaemon::OnBackendChanged);
	// The native owner resolves current per-device launch inputs on each Play.
	// Reload its next-launch settings without touching the captured active session.
	connect(&m_api, &HostApi::ClientProfileChanged, this, [this] {
		m_backend.SetClientCatalog(m_library.ClientCatalog(Interpreter()));
		if (m_backend.GetState() == StreamingBackend::State::Ready) m_backend.PublishCatalog();
	});
}

HostDaemon::~HostDaemon() {
    // Normal exit uses Shutdown. A forced teardown must not invoke callbacks after
    // other owner members have been destroyed; any uncertain guard keeps its marker.
    disconnect(&m_local, nullptr, this, nullptr);
	if (m_lock_fd >= 0) ::close(m_lock_fd);
}

HostDaemon::RestartStep HostDaemon::PlanRestart(StreamingBackend::State state,
                                                bool                    stream_game_running) {
	if (Idle(state)) return RestartStep::StartNow;
	if (state == StreamingBackend::State::Stopping || stream_game_running) return RestartStep::Wait;
	return RestartStep::StopNow;
}

int HostDaemon::NextRetryMs(int previous_ms) {
	return previous_ms < RETRY_FIRST_MS ? RETRY_FIRST_MS : std::min(previous_ms * 2, RETRY_MAX_MS);
}

void HostDaemon::ArmRetry() {
	m_retry_ms = NextRetryMs(m_retry_ms);
	m_retry.start(m_retry_ms);
}

bool HostDaemon::LocalEmulatorRunning() {
	const auto run = qEnvironmentVariable("XDG_RUNTIME_DIR");
	if (run.isEmpty()) return false;
	const int fd =
	    ::open(QFile::encodeName(run + "/kytyps5/emulator.lock").constData(), O_RDONLY | O_CLOEXEC);
	if (fd < 0) return false;
	const bool held =
	    ::flock(fd, LOCK_SH | LOCK_NB) != 0; // the guard holds LOCK_EX while a game runs
	if (!held) ::flock(fd, LOCK_UN);
	::close(fd);
	return held;
}

bool HostDaemon::LocalGameRunning() const {
	return m_local.state() != QProcess::NotRunning || m_api.NativeRuntimeIsLocal() ||
           (LocalEmulatorRunning() && !m_backend.StreamGameRunning() && m_api.NativeRuntimeGameId().isEmpty());
}

QString HostDaemon::Interpreter() {
	QDir search_dir(QCoreApplication::applicationDirPath());
	auto interpreter = search_dir.absoluteFilePath(EMULATOR_EXE);
	if (!QFile::exists(interpreter) && search_dir.cdUp()) {
		interpreter = search_dir.absoluteFilePath(EMULATOR_EXE);
	}
	return interpreter;
}

bool HostDaemon::Start(QString* error) {
	if (m_started) return true;
	const auto directory = HostRuntimeDirectory();
	if (directory.isEmpty()) {
		if (error != nullptr) *error = QStringLiteral("XDG_RUNTIME_DIR is not set.");
		return false;
	}
	const bool autostart = !qEnvironmentVariableIsSet("KYTY_HOST_NO_AUTOSTART");
	// One daemon per user: a second one would unlink the live sockets and orphan the first.
	if (!PrepareHostDirectory(directory, error)) return false;
	m_lock_fd = ::open(QFile::encodeName(directory + "/daemon.lock").constData(),
	                   O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (m_lock_fd < 0 || ::flock(m_lock_fd, LOCK_EX | LOCK_NB) != 0) {
		if (error != nullptr) {
			*error = (m_lock_fd >= 0 && errno == EWOULDBLOCK)
			             ? QStringLiteral("AGS host is already running")
			             : QStringLiteral("Cannot lock %1/daemon.lock.").arg(directory);
		}
		if (m_lock_fd >= 0) ::close(m_lock_fd);
		m_lock_fd = -1;
		return false;
	}
	if (!m_control.Listen(directory + "/control.sock", error)) return false;
	if (!m_api_server.Listen(directory + "/api.sock", error)) return false;
	m_started = true;

	QString load_error;
	if (!m_library.Load(&m_compat, &load_error)) {
		qWarning().noquote() << "kyty-host:" << load_error;
	}
	m_backend.SetApiSocket(directory + "/api.sock");
	m_backend.SetCatalog(m_library.Catalog(Interpreter()));
	m_backend.SetClientCatalog(m_library.ClientCatalog(Interpreter()));
	WatchSettings();
	m_refresh.start();
	if (autostart) {
		m_compat.Load(); // remote fetch; Updated() reloads the catalog with the new statuses
		StartHosting(ReadSettings());
	}
	return true;
}

void HostDaemon::StartHosting(const StreamingSettings& settings) {
	m_backend.SetLocalGameRunning(LocalGameRunning());
	m_backend.Start(settings);
}

void HostDaemon::WatchSettings() {
	const auto file = m_library.SettingsFile();
	if (file.isEmpty()) return;
	// QSettings replaces the file atomically, so the watch has to be re-armed after each change.
	const auto directory = QFileInfo(file).absolutePath();
	if (!m_watcher.directories().contains(directory)) m_watcher.addPath(directory);
	if (QFile::exists(file) && !m_watcher.files().contains(file)) m_watcher.addPath(file);
}

void HostDaemon::ReloadLibrary() {
	QString error;
	if (!m_library.Load(&m_compat, &error)) {
		qWarning().noquote() << "kyty-host:" << error;
		return;
	}
	m_backend.SetCatalog(m_library.Catalog(Interpreter()));
	m_backend.SetClientCatalog(m_library.ClientCatalog(Interpreter()));
	m_backend.PublishCatalog();
}

void HostDaemon::OnBackendChanged() {
	const auto state = m_backend.GetState();
	if (state == StreamingBackend::State::Ready) {
		m_retry_ms = 0;
		m_retry.stop();
	}
	if (m_restart_pending && Idle(state)) {
		m_restart_pending = false;
		StartHosting(m_restart_settings);
		return;
	}
	if (m_shutting_down && Idle(state) && m_local.state() == QProcess::NotRunning) {
		m_shutdown_failed = m_shutdown_failed || state == StreamingBackend::State::Error || !m_local_error.isEmpty() || m_api.NativeOwnershipBlocked();
		if (m_shutdown_failed) qWarning().noquote() << m_backend.Error();
		m_shutdown_cap.stop();
		emit Finished();
		return;
	}
	// Retry a failed worker or a start blocked by local play.
	if (state == StreamingBackend::State::Error && !m_shutting_down && !m_retry.isActive() &&
	    !qEnvironmentVariableIsSet("KYTY_HOST_NO_AUTOSTART")) {
		ArmRetry();
	}
}

void HostDaemon::Shutdown() {
	if (m_shutting_down) return;
	m_shutting_down   = true;
	m_restart_pending = false;
	m_retry.stop();
	m_refresh.stop();
	m_reload.stop();
	m_shutdown_cap.start();
    if (m_local.state() != QProcess::NotRunning) m_local.terminate();
    if (Idle(m_backend.GetState())) {
        QTimer::singleShot(0, this, &HostDaemon::OnBackendChanged);
        return;
    }
	m_backend.Stop();
}

QJsonObject HostDaemon::StatusObject() const {
	QJsonArray pairings, clients;
	for (const auto& value: m_backend.Pairings()) {
		const auto entry = value.toObject();
		pairings.append(QJsonObject {{"id", entry.value("id").toString()},
		                             {"name", entry.value("name").toString()}});
	}
	for (const auto& value: m_backend.Clients()) {
		const auto entry = value.toObject();
		clients.append(QJsonObject {{"uuid", entry.value("uuid").toString()},
		                            {"name", entry.value("name").toString()}});
	}
	const auto state = m_backend.GetState();
	const auto name  = QMetaEnum::fromType<StreamingBackend::State>().valueToKey(int(state));
    const bool recovery = m_api.NativeOwnershipRecoveryRequired();
	// A retained ownership marker adds recovery detail; it must not make a live,
	// admission-blocked worker look stopped or enable the UI's Start action.
	const bool error_state = state != StreamingBackend::State::Blocked && (recovery || !m_local_error.isEmpty());
	return {{"state", error_state ? QStringLiteral("Error") : QString::fromLatin1(name)},
	        {"error", recovery ? QStringLiteral("Steam ownership recovery required") : !m_local_error.isEmpty() ? m_local_error : m_backend.Error()},
	        {"ownership_blocked", m_api.NativeOwnershipBlocked()},
	        {"address", LanAddress()},
	        {"port", m_backend.NativeStatus().value("address").toString().section(':', -1).toInt()},
	        {"native", m_backend.NativeStatus()},
	        {"pairing_open", state == StreamingBackend::State::Ready &&
	                             PairingEnrollmentOpen(m_backend.NativeProfile(), QDateTime::currentMSecsSinceEpoch())},
	        {"backend", m_backend.Version()},
	        {"local_game_id", m_local_id},
        {"local_game_title", m_local_title},
        {"local_game_error", m_local_error},
        {"session", LocalGameRunning() && !m_local_title.isEmpty() ? tr("Local: %1").arg(m_local_title) : recovery ? QStringLiteral("Steam ownership recovery required") : !m_api.NativeRuntimeGameId().isEmpty() && !m_backend.StreamGameRunning() &&
	                            !m_backend.ClosingSession()
	                        ? QStringLiteral("Starting game…")
							: m_backend.Session()},
	        {"stream_game_running",
	         !recovery && (m_backend.StreamGameRunning() || (!m_api.NativeRuntimeGameId().isEmpty() && !m_api.NativeRuntimeIsLocal()))},
	        {"local_game_running", LocalGameRunning()},
	        {"pairings", pairings},
	        {"clients", clients},
	        {"hid", state == StreamingBackend::State::Ready ? m_backend.Hid() : QJsonObject {}}};
}

QJsonObject HostDaemon::PreferencesObject() const {
	const auto s = ReadSettings();
	return {{"port", s.port},
	        {"resolution", s.resolution},
	        {"fps", s.fps},
	        {"bitrate", s.bitrate},
	        {"codec", s.codec},
	        {"adapter", s.adapter},
	        {"encoder", s.encoder},
	        {"audio_codec", s.audio_codec},
	        {"local_audio_sink", s.local_audio_sink},
	        {"fec_enabled", s.fec_enabled},
	        {"local_audio", s.local_audio},
	        {"show_local", s.show_local},
	        {"disconnect_grace", s.disconnect_grace},
	        {"host_name", s.host_name}};
}

QJsonObject HostDaemon::SetPreferences(const QJsonObject& request) {
	const auto        current = ReadSettings();
	StreamingSettings next    = current;
	const auto        number  = [&request](const char* key, int* target) {
		const auto value = request.value(QLatin1String(key));
		if (value.isUndefined()) return true;
		if (!value.isDouble()) return false;
		*target = value.toInt();
		return true;
	};
	const auto text = [&request](const char* key, QString* target) {
		const auto value = request.value(QLatin1String(key));
		if (value.isUndefined()) return true;
		if (!value.isString()) return false;
		*target = value.toString();
		return true;
	};
	const auto flag = [&request](const char* key, bool* target) {
		const auto value = request.value(QLatin1String(key));
		if (value.isUndefined()) return true;
		if (!value.isBool()) return false;
		*target = value.toBool();
		return true;
	};
	if (!(number("port", &next.port) && text("resolution", &next.resolution) &&
	      number("fps", &next.fps) && number("bitrate", &next.bitrate) &&
	      text("codec", &next.codec) && text("adapter", &next.adapter) &&
	      text("audio_codec", &next.audio_codec) && text("encoder", &next.encoder) &&
	      text("local_audio_sink", &next.local_audio_sink) &&
	      flag("fec_enabled", &next.fec_enabled) && flag("local_audio", &next.local_audio) &&
	      flag("show_local", &next.show_local) &&
	      number("disconnect_grace", &next.disconnect_grace) &&
	      text("host_name", &next.host_name))) {
		return Fail(QStringLiteral("Invalid preference type."));
	}
	if (const auto problem = next.Validate(); !problem.isEmpty()) return Fail(problem);

	const auto profile = StreamingSettings::ProfileDirectory();
	const auto path    = QDir(profile).filePath("streaming.ini");
	if (!QDir().mkpath(profile) ||
	    !QFile::setPermissions(profile, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
		return Fail(QStringLiteral("Cannot create private settings directory."));
	}
	{
		QSettings file(path, QSettings::IniFormat);
		next.Write(file);
		file.sync();
		if (file.status() != QSettings::NoError ||
		    !QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner)) {
			return Fail(QStringLiteral("Cannot save streaming preferences."));
		}
	}
	if (!Same(current, next) && !m_shutting_down && Idle(m_backend.GetState()) &&
	    !qEnvironmentVariableIsSet("KYTY_HOST_NO_AUTOSTART"))
		StartHosting(next);

	return {{"ok", true}};
}

void HostDaemon::Control(const QJsonObject& request, std::function<void(HostReply)> done) {
	const auto type = request.value("type").toString();
	if (type == "status") return done(Json(StatusObject()));
	if (type == "get-preferences") return done(Json(PreferencesObject()));
	if (type == "set-preferences") {
		const auto result = SetPreferences(request);
		if (!result.value("ok").toBool() ||
		    (m_backend.GetState() != StreamingBackend::State::Ready &&
		     m_backend.GetState() != StreamingBackend::State::Starting))
			return done(Json(result));
		m_backend.ReloadSettings(ReadSettings(), [done](QJsonObject reply, QString error) {
			done(Json(error.isEmpty()
			              ? QJsonObject {{"ok", true},
						                 {"deferred", true},
						                 {"restart_required", reply.value("restart_required")}}
						  : Fail(error)));
		});
		return;
	}
	if (type == "reload-library") {
		m_reload.start();
		return done(Json({{"ok", true}}));
	}
	const auto complete = [done](QJsonObject reply, QString error) {
		done(Json(error.isEmpty() ? reply : Fail(error)));
	};
	if (type == "native-status") return done(Json(m_backend.NativeStatus()));
    if (type == "start-local-game") return StartLocalGame(request, std::move(done));
	if (type == "stop-game") {
        if (m_local.state() != QProcess::NotRunning) {
            if (m_local_stop_done) return done(Json(Fail(tr("Local game is already stopping."))));
            m_local_stop_done = std::move(done);
            m_local_deadline.start();
            m_local.terminate();
            return;
        }
        if (m_api.NativeRuntimeIsLocal() || m_api.NativeOwnershipRecoveryRequired()) return done(Json(Fail(tr("Game ownership cleanup is not complete."))));
		m_backend.CloseSession(complete);
		return;
	}
	if (type == "pair-ticket") {
		const auto native  = m_backend.NativeStatus();
		const auto port    = native.value("address").toString().section(':', -1);
		const auto address = LanAddress();
		if (address.isEmpty())
			return done(Json(Fail(QStringLiteral("No reachable LAN address is available."))));
		m_backend.ExportPairing(address + ':' + port, complete);
		return;
	}
	if (type == "unpair") {
		m_backend.Unpair(request.value("uuid").toString(), complete);
		return;
	}
	if (type == "pairing-pin")
		return done(Json(Fail(QStringLiteral("Native AGS requires a one-use pairing bundle."))));

	done(Json(Fail(QStringLiteral("unknown-request"))));
}

void HostDaemon::StartLocalGame(const QJsonObject& request, std::function<void(HostReply)> done) {
    if (request.size() != 2 || !request.value("game").isString()) return done(Json(Fail(tr("Invalid local launch request."))));
    const auto id = request.value("game").toString();
    const auto* game = m_library.Find(id);
    if (!game || game->source != "steam" || !game->owned_launch) return done(Json(Fail(tr("No owned Steam launch recipe is available."))));
    if (m_shutting_down || LocalGameRunning() || m_backend.StreamGameRunning() || !m_api.NativeRuntimeGameId().isEmpty() || m_api.NativeOwnershipBlocked())
        return done(Json(Fail(tr("A game already owns this computer."))));
    const auto profile = QDir(StreamingSettings::ProfileDirectory()).filePath("local");
    QString error;
    if (!PrepareHostDirectory(profile, &error)) return done(Json(Fail(error)));
    const auto identity_path = QDir(profile).filePath("identity");
    const int fd = ::open(QFile::encodeName(identity_path), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    struct stat meta {};
    if (fd < 0) return done(Json(Fail(tr("Cannot open local PC identity."))));
    if (::fstat(fd, &meta) != 0 || !S_ISREG(meta.st_mode) || meta.st_uid != ::geteuid() || (meta.st_mode & 0077) || meta.st_size > 36) {
        ::close(fd); return done(Json(Fail(tr("Unsafe local PC identity."))));
    }
    char bytes[37] {};
    const auto count = ::read(fd, bytes, 36);
    QString uuid = count >= 0 ? QString::fromLatin1(bytes, count) : QString();
    if (count == 0) {
        uuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        const auto encoded = uuid.toLatin1();
        if (::write(fd, encoded.constData(), encoded.size()) != encoded.size() || ::fsync(fd) != 0) uuid.clear();
    }
    ::close(fd);
    if (!ClientProfiles::IsValidClientId(uuid)) return done(Json(Fail(tr("Invalid local PC identity."))));
    m_api.ConfigureLocalOwner(uuid, QCoreApplication::applicationPid());
    m_local_id = id;
    m_local_title = game->name;
    m_local_error.clear();
    m_local_stderr.clear();
    m_local_started = false;
    m_local_start_done = std::move(done);
    m_local_deadline.start();
    m_local.start(ReadSettings().backend, {"local-launch", profile, HostRuntimeDirectory() + "/api.sock",
        QDir(qEnvironmentVariable("XDG_RUNTIME_DIR")).filePath("kytyps5/emulator.lock"), uuid, id,
        QString::number(QCoreApplication::applicationPid())});
}

void HostDaemon::LocalFinished(int code, QProcess::ExitStatus status) {
    m_local_deadline.stop();
    m_local_stderr += QString::fromUtf8(m_local.readAllStandardError());
    if ((status != QProcess::NormalExit || code != 0 || !m_local_started) && m_local_error.isEmpty())
        m_local_error = m_local_stderr.trimmed().isEmpty() ? tr("Local game exited unsuccessfully.") : m_local_stderr.trimmed().right(8192);
    const bool local_blocked = m_api.NativeOwnershipRecoveryRequired() || (m_api.NativeRuntimeIsLocal() && m_api.NativeOwnershipBlocked());
    if (local_blocked && m_local_error.isEmpty()) m_local_error = tr("Game ownership cleanup is not complete.");
    if (m_local_start_done) { auto done = std::move(m_local_start_done); done(Json(Fail(m_local_error))); }
    if (m_local_stop_done) { auto done = std::move(m_local_stop_done); done(Json(m_local_error.isEmpty() ? QJsonObject{{"ok", true}} : Fail(m_local_error))); }
    if (!local_blocked) {
        m_local_id.clear();
        m_local_title.clear();
    }
    if (m_shutting_down) OnBackendChanged();
}

int RunHostDaemon(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	ApplyBundledPreset(); // keeps KYTY_STREAMING_CONFIG_DIR handling identical to the GUI
	HostDaemon daemon;
	QString    error;
	if (!daemon.Start(&error)) {
		fprintf(stderr, "kyty-host: %s\n", qPrintable(error));
		return 1;
	}
	// Graceful stop on SIGTERM/SIGINT: stop hosting, then quit once the backend is Stopped.
	static int pipe_fds[2];
	if (::pipe2(pipe_fds, O_CLOEXEC | O_NONBLOCK) != 0) return 1;
	auto handler = [](int) {
		const char c = 1;
		(void)!::write(pipe_fds[1], &c, 1);
	};
	::signal(SIGTERM, handler);
	::signal(SIGINT, handler);
	QSocketNotifier notifier(pipe_fds[0], QSocketNotifier::Read);
	QObject::connect(&notifier, &QSocketNotifier::activated, [&] {
		char c;
		(void)!::read(pipe_fds[0], &c, 1);
		daemon.Shutdown();
	});
	QObject::connect(&daemon, &HostDaemon::Finished, &app, &QCoreApplication::quit);
	app.exec();
	return daemon.ShutdownExitCode();
}
