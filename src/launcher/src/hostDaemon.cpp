#include "hostDaemon.h"

#include "common/u59Preset.h"
#include "emulatorCommand.h"
#include "streamingApi.h"
#include "streamingSettings.h"

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

constexpr int SHUTDOWN_CAP_MS = 20000;
constexpr int RETRY_FIRST_MS  = 5000;
constexpr int RETRY_MAX_MS    = 60000;

HostReply Json(const QJsonObject& object) {
	return {200, "application/json", QJsonDocument(object).toJson(QJsonDocument::Compact), {}};
}

QJsonObject Fail(const QString& error) {
	return {{"ok", false}, {"error", error}};
}

bool Active(StreamingBackend::State state) {
	return state == StreamingBackend::State::Starting || state == StreamingBackend::State::Ready ||
	       state == StreamingBackend::State::Stopping;
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
	       a.local_audio == b.local_audio && a.show_local == b.show_local;
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
		if (LocalGameRunning()) { // Moonlight launches from the shared library: wait for the game
			ArmRetry();
			return;
		}
		StartHosting(ReadSettings());
	});
	connect(&m_shutdown_cap, &QTimer::timeout, this, &HostDaemon::Finished);
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
}

HostDaemon::~HostDaemon() {
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
	return LocalEmulatorRunning() && !m_backend.StreamGameRunning();
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
			             ? QStringLiteral("Kyty host is already running")
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
	if (!m_backend.StreamGameRunning()) m_backend.PublishCatalog();
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
	if (m_shutting_down && Idle(state)) {
		m_shutdown_cap.stop();
		emit Finished();
		return;
	}
	// Sunshine died, a local game blocked the start or a restart was refused: keep trying.
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
	if (Idle(m_backend.GetState())) {
		QTimer::singleShot(0, this, &HostDaemon::Finished);
		return;
	}
	m_shutdown_cap.start();
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
	return {{"state", QString::fromLatin1(name)},
	        {"error", m_backend.Error()},
	        {"address", LanAddress()},
	        {"port", ReadSettings().port},
	        {"session", m_backend.Session()},
	        {"stream_game_running", m_backend.StreamGameRunning()},
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
	        {"local_audio", s.local_audio},
	        {"show_local", s.show_local}};
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
	      flag("local_audio", &next.local_audio) && flag("show_local", &next.show_local))) {
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
	const bool autostart = !qEnvironmentVariableIsSet("KYTY_HOST_NO_AUTOSTART");
	if (!Same(current, next) && !m_shutting_down && (Active(m_backend.GetState()) || autostart)) {
		m_restart_pending  = true;
		m_restart_settings = next;
		m_retry.stop();
		const auto state = m_backend.GetState();
		const auto step  = PlanRestart(state, m_backend.StreamGameRunning());
		if (step == RestartStep::StartNow) {
			OnBackendChanged();
		} else if (step == RestartStep::StopNow) {
			m_backend.Stop();
		} else if (state != StreamingBackend::State::Stopping) {
			return {{"ok", true}, {"deferred", true}}; // the refresh tick stops after the game
		}
	}
	return {{"ok", true}};
}

void HostDaemon::Control(const QJsonObject& request, std::function<void(HostReply)> done) {
	const auto type = request.value("type").toString();
	if (type == "status") return done(Json(StatusObject()));
	if (type == "get-preferences") return done(Json(PreferencesObject()));
	if (type == "set-preferences") return done(Json(SetPreferences(request)));
	if (type == "stop-game") {
		m_backend.CloseSession();
		return done(Json({{"ok", true}}));
	}
	if (type == "pairing-pin") {
		const auto id = request.value("id").toString(), pin = request.value("pin").toString(),
		           name = request.value("name").toString();
		if (const auto problem = StreamingApi::ValidatePin(id, pin, name); !problem.isEmpty())
			return done(Json(Fail(problem)));
		if (m_backend.GetState() != StreamingBackend::State::Ready)
			return done(Json(Fail(QStringLiteral("Hosting is not ready."))));
		m_backend.Pair(id, pin, name);
		return done(Json({{"ok", true}}));
	}
	if (type == "unpair") {
		const auto uuid = request.value("uuid").toString();
		const bool ok   = m_backend.GetState() == StreamingBackend::State::Ready && !uuid.isEmpty();
		if (ok) m_backend.Unpair(uuid);
		return done(Json({{"ok", ok}}));
	}
	done(Json(Fail(QStringLiteral("unknown-request"))));
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
	return app.exec();
}
