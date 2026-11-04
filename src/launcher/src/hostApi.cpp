#include "steamArtwork.h"
#include "hostApi.h"

#include "cheatFile.h"
#include "clientProfiles.h"
#include "common/archive.h"
#include "common/trophies.h"
#include "configuration.h"
#include "gameContent.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStringList>
#include <QTimeZone>

#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <memory>
#include <limits>
#include <utility>

namespace {
constexpr int MAX_BACKGROUND_WIDTH = 1920;

struct Field {
	const char* key;
	const char* label;
	const char* type;
	qint64      min;
	qint64      max;
};

// Every key of Configuration::GameSettings() is listed; a missing one is still emitted as a
// string field, so a new setting never vanishes from the API.
constexpr Field FIELDS[] = {
    {"screen_resolution", "Resolution", "enum", 0, 0},
    {"user_name", "User name", "string", 0, 0},
    {"user_id", "User ID", "int", 0, 2147483647},
    {"audio_input_device", "Audio input device", "string", 0, 0},
    {"stream_aspect_mode", "Stream aspect mode", "enum", 0, 0},
    {"present_mode", "Present mode", "enum", 0, 0},
    {"gpu_index", "GPU index", "int", -1, 16},
    {"fullscreen_enabled", "Fullscreen", "bool", 0, 0},
    {"hdr_enabled", "HDR", "bool", 0, 0},
    {"hide_cursor_enabled", "Hide cursor", "bool", 0, 0},
    {"readback_linear_images", "Read back linear images", "bool", 0, 0},
    {"sync_raw_image_buffers", "Sync raw image buffers", "bool", 0, 0},
    {"support_zmask", "Support Z mask", "bool", 0, 0},
    {"tessellation_enabled", "Tessellation", "bool", 0, 0},
    {"trophy_enabled", "Trophy notifications", "bool", 0, 0},
    {"trophy_duration_seconds", "Trophy duration (s)", "int", 5, 10},
    {"perf_overlay_enabled", "Performance overlay", "bool", 0, 0},
    {"skip_notice_screen", "Skip notice screen", "bool", 0, 0},
    {"network_connected", "Network connected", "bool", 0, 0},
    {"vblank_frequency", "VBlank frequency", "int", 30, 360},
    {"console_language", "Console language", "int", 0, 29},
    {"vulkan_validation_enabled", "Vulkan validation", "bool", 0, 0},
    {"shader_validation_enabled", "Shader validation", "bool", 0, 0},
    {"shader_optimization_type", "Shader optimization", "enum", 0, 0},
    {"shader_log_direction", "Shader log output", "enum", 0, 0},
    {"command_buffer_dump_enabled", "Command buffer dump", "bool", 0, 0},
    {"printf_direction", "Printf output", "enum", 0, 0},
    {"profiler_enabled", "Profiler", "bool", 0, 0},
    {"renderdoc_enabled", "RenderDoc", "bool", 0, 0},
    {"amd_cpu_mode", "AMD CPU mode", "enum", 0, 0},
    {"red_zone_protection_enabled", "Red zone protection", "bool", 0, 0},
};

// Filesystem paths are never accepted from, or shown to, a request.
const QStringList PATH_KEYS {QStringLiteral("elf"), QStringLiteral("shader_log_folder"),
                             QStringLiteral("command_buffer_dump_folder"),
                             QStringLiteral("printf_output_file")};

QStringList Choices(const QString& key) {
	if (key == QLatin1String("screen_resolution")) {
		return EnumToList<Configuration::Resolution>();
	}
	if (key == QLatin1String("stream_aspect_mode")) {
		return {QStringLiteral("fit"), QStringLiteral("fill"), QStringLiteral("stretch")};
	}
	if (key == QLatin1String("present_mode")) {
		return EnumToList<Configuration::PresentMode>();
	}
	if (key == QLatin1String("shader_optimization_type")) {
		return EnumToList<Configuration::ShaderOptimizationType>();
	}
	if (key == QLatin1String("shader_log_direction") || key == QLatin1String("printf_direction")) {
		return EnumToList<Configuration::LogDirection>();
	}
	return {QStringLiteral("auto"), QStringLiteral("on"), QStringLiteral("off")}; // amd_cpu_mode
}

// GameSettings() stores enums by key ("R1280X720") while the choices use the launcher text
// ("1280x720"); the API speaks the choice text in both directions.
QString ChoiceText(const QString& raw) {
	return raw.size() > 2 && raw.startsWith(QLatin1Char('R')) && raw.at(1).isDigit()
	           ? raw.mid(1).toLower()
	           : raw;
}

QString RawText(const QString& text) {
	return text.size() > 1 && text.at(0).isDigit() ? QLatin1Char('R') + text.toUpper() : text;
}

QString Hex(const QByteArray& data) {
	return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

HostReply Json(int status, const QJsonObject& object) {
	return {status, "application/json", QJsonDocument(object).toJson(QJsonDocument::Compact), {}};
}

HostReply Error(int status, const QString& message) {
	return Json(status, {{QStringLiteral("error"), message}});
}

QString TitleId(const Configuration& info) {
	return info.title_id.trimmed().toUpper();
}

QString Process(const Configuration& info) {
	return QFileInfo(info.elf.isEmpty() ? QStringLiteral("eboot.bin") : info.elf).fileName();
}

// A missing file has an empty revision.
QByteArray ReadCheatBytes(const QString& path) {
	QFile file(path);
	return !path.isEmpty() && file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray {};
}

QString CheatRevision(const QByteArray& bytes) {
	return bytes.isEmpty() ? QString {} : Hex(bytes);
}

} // namespace

HostApi::HostApi(GameLibrary* library, std::function<Status()> status, QObject* parent)
    : QObject(parent), m_library(library), m_status(std::move(status)), m_repository(this) {
	m_runtime_watch.setInterval(250);
	connect(&m_runtime_watch, &QTimer::timeout, this, [this] { ReapRuntime(); });
	m_runtime_watch.start();
	connect(&m_repository, &Cheats::CheatRepository::CatalogLoaded, this,
	        [this](QList<Cheats::RemoteFile> files, const QString& error) {
		        if (auto callback = std::exchange(m_on_catalog, nullptr)) {
			        callback(std::move(files), error);
		        }
	        });
	connect(&m_repository, &Cheats::CheatRepository::FileLoaded, this,
	        [this](const QByteArray& data, const QString& error) {
		        if (auto callback = std::exchange(m_on_file, nullptr)) {
			        callback(data, error);
		        }
	        });
}

namespace {
bool SafeDirectory(const QString& path) {
	if (!QDir::isAbsolutePath(path) || path.split('/').contains("..")) return false;
	QString current;
	for (const auto& part: path.split('/', Qt::SkipEmptyParts)) {
		current += '/' + part;
		const QFileInfo info(current);
		if (!info.isDir() || info.isSymLink()) return false;
	}
	return true;
}
bool SafeRuntime(const QString& path) {
	if (!SafeDirectory(path)) return false;
	for (const auto& name: {"_SaveData", "_Trophies", "_Cache", "_CrashCapture", "_HangTrace", "_TempData", "_ShaderLogs", "_CommandBuffers"}) {
		const auto child = QDir(path).filePath(name);
		if ((QFileInfo::exists(child) || QFileInfo(child).isSymLink()) && !SafeDirectory(child)) return false;
		{
			QDirIterator entries(child, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
			while (entries.hasNext()) { entries.next(); if (entries.fileInfo().isSymLink()) return false; }
		}
	}
	return true;
}
QString ProcessStart(qint64 pid) {
	if (pid <= 0) return {};
	QFile file(QStringLiteral("/proc/%1/stat").arg(pid));
	if (!file.open(QIODevice::ReadOnly)) return {};
	const auto data = file.readAll();
	const auto fields = data.mid(data.lastIndexOf(')') + 2).split(' ');
	if (fields.value(0) == "Z") return {};
	return QString::fromLatin1(fields.value(19));
}
qint64 MonotonicMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
	    std::chrono::steady_clock::now().time_since_epoch()).count();
}
int OpenLock(const QString& path) {
	if (!QDir::isAbsolutePath(path) || path.split('/').contains("..")) return -1;
	const auto parent = QFileInfo(path).absolutePath();
	if (!SafeDirectory(parent)) return -1;
	struct stat st {};
	if (::stat(QFile::encodeName(parent), &st) != 0 || st.st_uid != ::geteuid() || (st.st_mode & 0077)) return -1;
	const int fd = ::open(QFile::encodeName(path), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0) return -1;
	if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid() || (st.st_mode & 0077)) {
		::close(fd); return -1;
	}
	return fd;
}
bool LegacyEmulatorRunning() {
	const QDir proc("/proc");
	for (const auto& name: proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
		bool number = false; name.toLongLong(&number);
		if (!number) continue;
		const QFileInfo process(proc.filePath(name));
		if (process.ownerId() != ::geteuid()) continue;
		QFile comm(proc.filePath(name + "/comm"));
		if (comm.open(QIODevice::ReadOnly) && comm.readAll().trimmed() == "kyty_emulator") return true;
	}
	return false;
}
bool LifetimeHeld(const QString& path) {
	const int fd = OpenLock(path);
	if (fd < 0) return true; // unsafe/missing lock directories fail closed
	const int result = ::flock(fd, LOCK_EX | LOCK_NB);
	if (result == 0) ::flock(fd, LOCK_UN);
	::close(fd);
	return result != 0;
}
}

HostApi::~HostApi() { ClearRuntime(); }
void HostApi::ConfigureNativeLaunch(const QString& interpreter, const QString& lock_path,
                                    const QString& runtime_root) {
	m_interpreter = interpreter;
	m_local_lock = lock_path;
	m_runtime_root = runtime_root;
}
bool HostApi::NativeOwnershipBlocked() const {
    const QFileInfo marker(m_local_lock + ".host-steam-owner");
    return !m_local_lock.isEmpty() && (marker.exists() || marker.isSymLink());
}
bool HostApi::NativeOwnershipRecoveryRequired() const {
    if (!NativeOwnershipBlocked()) return false;
    const auto path = QFile::encodeName(m_local_lock + ".host-steam-owner");
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return true;
    struct stat metadata {};
    if (::fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        metadata.st_uid != ::geteuid() || (metadata.st_mode & 0077) || metadata.st_size > 4096) {
        ::close(fd);
        return true;
    }
    QFile marker;
    if (!marker.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) { ::close(fd); return true; }
    const auto record = QJsonDocument::fromJson(marker.read(4097)).object();
    return record["guard_start"].toString().isEmpty() ||
        ProcessStart(record["guard_pid"].toInteger()) != record["guard_start"].toString();
}
void HostApi::ReapRuntime() {
    if (NativeOwnershipBlocked()) return;
	if (!m_runtime.session_id.isEmpty() &&
	    (ProcessStart(m_runtime.owner_pid) != m_runtime.owner_start ||
	     (!m_runtime.acquired && MonotonicMs() >= m_runtime.deadline_ms) ||
	     (m_runtime.acquired && !LifetimeHeld(m_local_lock)))) ClearRuntime();
}
void HostApi::ClearRuntime() {
	if (m_reservation_fd >= 0) { ::close(m_reservation_fd); m_reservation_fd = -1; }
	m_runtime = {};
}
QString HostApi::RuntimeDirectory(const QString& game_id) const {
	if (!m_runtime.game.isEmpty()) {
		// Never fall back into the normal tree while an isolated launch is pending/running.
		return m_runtime.game == game_id ? m_runtime.directory : QString {};
	}
	if (!m_runtime_root.isEmpty()) return QDir(m_runtime_root).filePath(game_id);
	return m_interpreter.isEmpty() ? QCoreApplication::applicationDirPath() : QFileInfo(m_interpreter).absolutePath();
}
QJsonObject HostApi::ResolveNative(const QString& client_id, const QString& game_id, bool local) const {
	const auto entry = m_library->ResolveLaunch(m_interpreter, client_id, game_id);
	if (!entry || !QDir::isAbsolutePath(entry->program) || !QFileInfo(entry->program).isExecutable()) return {};
	const bool ps5 = m_library->Find(game_id)->info != nullptr;
	if (!ps5) {
		if (!QDir::isAbsolutePath(entry->working_directory) ||
		    !QFileInfo(entry->working_directory).isDir())
			return {};
		auto env = entry->environment;
		if (!local) {
			for (auto i = entry->stream_environment.begin(); i != entry->stream_environment.end(); ++i)
				env.insert(i.key(), i.value());
		}
		return {{"id", entry->id},
		        {"name", entry->name},
		        {"executable", entry->program},
		        {"args", QJsonArray::fromStringList(entry->arguments)},
		        {"cwd", entry->working_directory},
		        {"env", env},
                {"process_visibility", entry->process_visibility}};
	}
	auto        cwd = entry->working_directory;
	QJsonObject env;
	if (!m_runtime_root.isEmpty()) {
		if (!SafeDirectory(m_runtime_root)) return {};
		struct stat root {};
		if (::stat(QFile::encodeName(m_runtime_root), &root) != 0 || root.st_uid != ::geteuid() || (root.st_mode & 0077)) return {};
		cwd = QDir(m_runtime_root).filePath(game_id);
		if (QFileInfo(cwd).isSymLink() || !QDir().mkpath(cwd) || !SafeRuntime(cwd)) return {};
		for (const auto& name: {"_SaveData", "_Trophies", "_Cache", "_CrashCapture", "_HangTrace", "_TempData", "_ShaderLogs", "_CommandBuffers"}) {
			if (!QDir().mkpath(QDir(cwd).filePath(name))) return {};
		}
		env["XDG_CACHE_HOME"] = QDir(cwd).filePath("_Cache");
		env["__GL_SHADER_DISK_CACHE_PATH"] = QDir(cwd).filePath("_Cache");
		env["MESA_SHADER_CACHE_DIR"] = QDir(cwd).filePath("_Cache");
	}
	if (!SafeRuntime(cwd)) return {};
	auto args = entry->arguments;
	for (qsizetype i = 0; i < args.size(); ++i) {
		const auto arg = args[i];
		if (arg.contains(QChar('\0')) || arg == "--streaming-only" || arg.startsWith("--streaming-only=") ||
		    arg == "--streaming-socket" || arg.startsWith("--streaming-socket=")) return {};
		if (!m_runtime_root.isEmpty() && (arg == "--shader-log-folder" || arg == "--command-buffer-dump-folder" || arg == "--printf-output-file")) {
			if (i + 1 >= args.size()) return {};
			// Preserve every option, relocating only mutable diagnostic destinations for
			// the explicitly isolated runtime. The stored PC/device settings are unchanged.
			args[i + 1] = QDir(cwd).filePath(arg == "--shader-log-folder" ? "_ShaderLogs" :
			    arg == "--command-buffer-dump-folder" ? "_CommandBuffers" : "_TempData/printf.log");
		}
		if (arg == "--game" || arg == "--game-patch" || arg == "--addcont-dir") {
			if (i + 1 >= args.size()) return {};
			if (QDir::isRelativePath(args[i + 1])) args[i + 1] = QDir(entry->working_directory).absoluteFilePath(args[i + 1]);
		}
	}
	return {{"id", entry->id}, {"name", entry->name}, {"executable", entry->program},
	        {"args", QJsonArray::fromStringList(args)}, {"cwd", cwd}, {"env", env}};
}
void HostApi::Internal(const QJsonObject& request, const std::function<void(HostReply)>& done) {
	ReapRuntime();
	if (m_interpreter.isEmpty() || m_local_lock.isEmpty()) return done(Error(503, "native-launch-unconfigured"));
	if (request["method"] != "POST") return done(Error(405, "method-not-allowed"));
	const auto client = request["client"].toObject();
	const auto client_id = client["id"].toString();
	if (!ClientProfiles::IsValidClientId(client_id)) return done(Error(400, "bad-client"));
	if (!request["body"].isObject()) return done(Error(400, "bad-body"));
	const auto body = request["body"].toObject();
	if (QJsonDocument(body).toJson(QJsonDocument::Compact).size() > MAX_BODY) return done(Error(413, "body-too-large"));
	const auto game_id = body["game"].toString();
	if (request["path"] == "/ags/internal/resolve-launch" || request["path"] == "/ags/internal/resolve-local-launch") {
        const bool local = request["path"] == "/ags/internal/resolve-local-launch";
        if (local != (client_id == m_local_client_id)) return done(Error(403, "invalid-local-owner"));
        if (local && (!m_library->Find(game_id) || (m_library->Find(game_id)->source != "steam" && m_library->Find(game_id)->source != "ps5"))) return done(Error(400, "invalid-local-source"));
		if (body.size() != 1) return done(Error(400, "bad-body"));
		m_resolved = {};
		const auto launch = ResolveNative(client_id, game_id, local);
		if (launch.isEmpty()) return done(Error(404, "invalid-launch"));
		if (!m_library->Find(game_id)->info) {
			m_resolved = {client_id, launch, {}};
			return done(Json(200, launch));
		}
		auto snapshot = ClientConfig(*m_library->Find(game_id)->info, {});
		if (!snapshot) return done(Error(404, "invalid-launch"));
		// These values come from the exact selected argv, not a second profile read
		// which could already describe a later launch. Other trophy metadata is copied.
		const auto args = launch["args"].toArray();
		const auto integerArgument = [&](const char* flag, int maximum) -> std::optional<int> {
			const auto at = std::find(args.begin(), args.end(), QJsonValue(flag));
			if (at == args.end() || at + 1 == args.end() || std::count(args.begin(), args.end(), QJsonValue(flag)) != 1) return {};
			bool ok = false; const auto value = (at + 1)->toString().toInt(&ok);
			return ok && value >= 0 && value <= maximum ? std::optional<int>(value) : std::nullopt;
		};
		const auto user = integerArgument("--user-id", std::numeric_limits<int>::max());
		const auto language = integerArgument("--console-language", Configuration::MAX_CONSOLE_LANGUAGE);
		if (!user || !language) return done(Error(404, "invalid-launch"));
		snapshot->user_id = *user; snapshot->console_language = *language;
		m_resolved = {client_id, launch, std::move(snapshot)};
		return done(Json(200, launch));
	}
	if (request["path"] != "/ags/internal/session-runtime") return done(Error(404, "not-found"));
	const auto state = body["state"].toString();
	const auto session = body["session_id"].toString();
	const auto token = body["reservation_token"].toString();
	const auto runtime = body["runtime_directory"].toString();
	static const QRegularExpression TOKEN("\\A[0-9a-f]{64}\\z");
	if (!TOKEN.match(session).hasMatch() || !TOKEN.match(token).hasMatch() || body["client_id"] != client_id)
		return done(Error(400, "invalid-runtime"));
	if ((state == "starting" || state == "closed") && NativeOwnershipBlocked())
        return done(Error(409, "host-ownership-blocked"));
    if (state == "closed" && m_runtime.session_id.isEmpty()) return done(Json(200, {{"ok", true}}));
	if (state == "starting") {
		if (!m_runtime.session_id.isEmpty()) return done(Error(409, "host-busy"));
		const auto& launch = m_resolved.game;
		const auto pid = body["owner_pid"].toInteger();
		const auto start = body["owner_start"].toString();
        if (client_id == m_local_client_id && pid != m_local_owner_pid) return done(Error(403, "invalid-local-owner"));
		if (launch.isEmpty() || m_resolved.client_id != client_id || launch["id"] != game_id ||
		    (!m_resolved.trophy_config && m_library->Find(game_id) && m_library->Find(game_id)->info) || !m_library->Find(game_id) || runtime != launch["cwd"].toString() || !(m_library->Find(game_id)->info ? SafeRuntime(runtime) : QFileInfo(runtime).isDir()) ||
		    start.isEmpty() || ProcessStart(pid) != start) return done(Error(400, "invalid-runtime"));
		const int admission = OpenLock(m_local_lock + ".launch");
		if (admission < 0 || ::flock(admission, LOCK_EX | LOCK_NB) != 0) {
			if (admission >= 0) ::close(admission);
			return done(Error(409, "host-busy"));
		}
		if (LifetimeHeld(m_local_lock) || LegacyEmulatorRunning()) { ::close(admission); return done(Error(409, "host-busy")); }
		m_reservation_fd = admission;
		m_local_spawned = false;
		m_runtime = {session, game_id, client_id, runtime, start, token, pid, false, false,
		             MonotonicMs() + 30000, std::move(m_resolved.trophy_config)};
		m_resolved = {};
		return done(Json(200, {{"ok", true}}));
	}
	if (session != m_runtime.session_id || token != m_runtime.token || client_id != m_runtime.client_id ||
	    game_id != m_runtime.game || runtime != m_runtime.directory || m_runtime.owner_start.isEmpty() ||
	    ProcessStart(m_runtime.owner_pid) != m_runtime.owner_start) return done(Error(409, "runtime-mismatch"));
    if (state == "spawned" && NativeRuntimeIsLocal() && m_runtime.acquired && !m_local_spawned && LifetimeHeld(m_local_lock)) {
        m_local_spawned = true;
        emit LocalGameSpawned(game_id);
        return done(Json(200, {{"ok", true}}));
    }
	if (state == "ready") {
        if (NativeOwnershipRecoveryRequired()) return done(Error(409, "host-ownership-blocked"));
        return done(Json(200, {{"acquired", m_runtime.acquired}}));
    }
	if (state == "closed") { ClearRuntime(); return done(Json(200, {{"ok", true}})); }
	if (state == "claim" && !m_runtime.claimed && !m_runtime.acquired && MonotonicMs() < m_runtime.deadline_ms) {
		m_runtime.claimed = true;
		return done(Json(200, {{"ok", true}, {"local_game_lock", m_local_lock}}));
	}
	if (state == "acquired" && m_runtime.claimed && !m_runtime.acquired && LifetimeHeld(m_local_lock)) {
		m_runtime.acquired = true;
		if (m_reservation_fd >= 0) { ::close(m_reservation_fd); m_reservation_fd = -1; }
		return done(Json(200, {{"ok", true}}));
	}
	return done(Error(409, "invalid-runtime-transition"));
}

QString HostApi::SettingsRevision(const Configuration& info) {
	return Hex(QJsonDocument(QJsonObject::fromVariantMap(info.GameSettings()))
	               .toJson(QJsonDocument::Compact));
}

QJsonArray HostApi::SettingsSchema(const Configuration& info) {
	const auto  values = info.GameSettings();
	QJsonArray  fields;
	QStringList listed;
	const auto  add = [&](const QString& key, const QString& label, const QString& type, qint64 min,
	                      qint64 max) {
		if (!values.contains(key) || PATH_KEYS.contains(key)) {
			return; // e.g. a Windows-only key, or a path setting
		}
		const auto  value = values.value(key).toString();
		QJsonObject field {{"key", key},
		                   {"label", label},
		                   {"type", type},
		                   {"value", type == QLatin1String("enum") ? ChoiceText(value) : value}};
		if (type == QLatin1String("enum")) {
			field["choices"] = QJsonArray::fromStringList(Choices(key));
		} else if (type == QLatin1String("int")) {
			field["min"] = min;
			field["max"] = max;
		}
		fields.append(field);
		listed.append(key);
	};
	for (const auto& f: FIELDS) {
		add(QString::fromLatin1(f.key), QString::fromLatin1(f.label), QString::fromLatin1(f.type),
		    f.min, f.max);
	}
	for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
		if (!listed.contains(it.key())) {
			add(it.key(), it.key(), QStringLiteral("string"), 0, 0);
		}
	}
	return fields;
}

void HostApi::Handle(const QJsonObject& request, std::function<void(HostReply)> done) {
	if (request["path"].toString().startsWith("/ags/internal/")) return Internal(request, done);
	static const QRegularExpression ROUTE(QStringLiteral(
	    R"(\A/kyty/v1/(?:(library)|(host/metrics)|art/([0-9a-f]{64})/(icon|background)|games/([0-9a-f]{64})/)"
	    R"((settings|settings/reset|trophies|trophies/icon/[0-9]{1,3}-[0-9]{1,4}|cheats|cheats/remote|)"
	    R"(cheats/remote/fetch))\z)"));

	const auto method = request.value(QStringLiteral("method")).toString();
	const auto match  = ROUTE.match(request.value(QStringLiteral("path")).toString());
	if (!match.hasMatch()) {
		return done(Error(404, QStringLiteral("not-found")));
	}

	// Resolve the device identity before any path is touched. A present-but-invalid id is a hard
	// 400 and NOTHING is created; an absent client keeps today's PC behaviour.
	Client     client;
	const auto client_value = request.value(QStringLiteral("client"));
	if (!client_value.isUndefined()) {
		if (!client_value.isObject()) {
			return done(Error(400, QStringLiteral("bad-client")));
		}
		const auto object = client_value.toObject();
		const auto id     = object.value(QStringLiteral("id")).toString();
		if (!ClientProfiles::IsValidClientId(id)) {
			return done(Error(400, QStringLiteral("bad-client")));
		}
		client.id   = id;
		client.name = object.value(QStringLiteral("name")).toString();
		ClientProfiles::TouchClient(client.id, client.name);
	}

	const bool    is_library     = !match.captured(1).isEmpty();
	const bool    is_metrics     = !match.captured(2).isEmpty();
	const bool    is_art         = !match.captured(3).isEmpty();
	const QString sub            = match.captured(6);
	const bool    is_trophy_icon = sub.startsWith(QLatin1String("trophies/icon/"));
	const bool reads  = is_library || is_metrics || is_art ||
	                    sub == QLatin1String("cheats/remote") || sub == QLatin1String("trophies") ||
	                    is_trophy_icon;
	const bool writes = sub == QLatin1String("settings") || sub == QLatin1String("cheats");
	const bool posts =
	    sub == QLatin1String("cheats/remote/fetch") || sub == QLatin1String("settings/reset");
	if (!((method == QLatin1String("GET") && (reads || writes)) ||
	      (method == QLatin1String("PUT") && writes) ||
	      (method == QLatin1String("POST") && posts))) {
		return done(Error(405, QStringLiteral("method-not-allowed")));
	}
	if (is_library) {
		return Library(client, done);
	}
	if (is_metrics) {
		return Metrics(done);
	}
	if (is_art) {
		return Art(match.captured(3), match.captured(4),
		           request.value(QStringLiteral("if_none_match")).toString(), done);
	}
	const auto* game = m_library->Find(match.captured(5));
	if (game == nullptr) {
		return done(Error(404, QStringLiteral("unknown-game")));
	}
	if (!game->info) return done(Error(409, "unsupported-source-action"));
	QJsonObject body;
	if (method != QLatin1String("GET")) {
		const auto value = request.value(QStringLiteral("body"));
		if (!value.isObject()) {
			return done(Error(400, QStringLiteral("bad-body")));
		}
		body = value.toObject();
		if (QJsonDocument(body).toJson(QJsonDocument::Compact).size() > MAX_BODY) {
			return done(Error(413, QStringLiteral("body-too-large")));
		}
	}
	if (sub == QLatin1String("settings")) {
		return method == QLatin1String("GET") ? GetSettings(*game, client, done)
		                                      : PutSettings(*game, client, body, done);
	}
	if (sub == QLatin1String("settings/reset")) {
		return ResetSettings(*game, client, done);
	}
	if ((sub == QLatin1String("trophies") || is_trophy_icon) && !SafeRuntime(RuntimeDirectory(game->id)))
		return done(Error(409, "runtime-unavailable"));
	if (sub == QLatin1String("trophies")) {
		return GetTrophies(*game, client, done);
	}
	if (is_trophy_icon) {
		const auto key = sub.mid(QStringLiteral("trophies/icon/").size()).split(QLatin1Char('-'));
		const auto info = TrophyConfiguration(*game, client);
		if (!info) return done(Error(409, "runtime-unavailable"));
		return done(TrophyIcon(*info, RuntimeDirectory(game->id),
		                       key.value(0).toInt(), key.value(1).toInt(),
		                       request.value(QStringLiteral("if_none_match")).toString()));
	}
	if (sub == QLatin1String("cheats")) {
		return method == QLatin1String("GET") ? GetCheats(*game, client, done)
		                                      : PutCheats(*game, client, body, done);
	}
	if (sub == QLatin1String("cheats/remote")) {
		return GetRemote(*game, std::move(done));
	}
	FetchRemote(*game, body, std::move(done));
}

std::unique_ptr<Configuration> HostApi::ClientConfig(const Configuration& game,
                                                     const QJsonObject&   settings) {
	auto config = std::make_unique<Configuration>();
	config->CopyGameInfoFrom(game);
	config->CopyEmulatorSettingsFrom(game);
	config->CopyGlobalOnlySettingsFrom(game);
	config->elf        = game.elf;
	config->addons_dir = game.addons_dir;
	config->basedir    = game.basedir;
	if (!settings.isEmpty()) {
		QString error;
		if (!config->SetGameSettings(settings, error)) {
			return nullptr; // a stored/seeded profile should always validate
		}
	}
	return config;
}

namespace {

// The raw GameSettings map (the values info.GameSettings() emits) a client profile stores for one
// game, seeding a COPY of the PC's current settings on first access. Never writes the PC config.
QJsonObject ClientSettings(const HostApi::Client& client, const GameEntry& game) {
	bool       seeded = false;
	const auto stored = ClientProfiles::ReadGame(client.id, game.id, &seeded);
	if (seeded) {
		return stored.value(QStringLiteral("settings")).toObject();
	}
	return QJsonObject::fromVariantMap(game.info->GameSettings());
}

// A Configuration carrying this client's effective settings, or the game's own when unscoped.
std::unique_ptr<Configuration> EffectiveConfig(const HostApi::Client& client,
                                               const GameEntry&       game) {
	if (!client.scoped()) {
		auto config = std::make_unique<Configuration>();
		config->CopyGameInfoFrom(*game.info);
		config->CopyEmulatorSettingsFrom(*game.info);
		config->CopyGlobalOnlySettingsFrom(*game.info);
		config->elf        = game.info->elf;
		config->addons_dir = game.info->addons_dir;
		config->basedir    = game.info->basedir;
		return config;
	}
	return HostApi::ClientConfig(*game.info, ClientSettings(client, game));
}

// Persist a client's game profile (settings + cheat selection), leaving the other half untouched.
QString SaveClientProfile(const HostApi::Client& client, const GameEntry& game,
                          const QJsonObject& settings, const QJsonObject& cheats) {
	bool seeded                        = false;
	auto current                       = ClientProfiles::ReadGame(client.id, game.id, &seeded);
	current[QStringLiteral("version")] = 1;
	current[QStringLiteral("settings")] =
	    settings.isEmpty() ? current.value(QStringLiteral("settings")) : settings;
	current[QStringLiteral("cheats")] =
	    cheats.isEmpty() ? current.value(QStringLiteral("cheats")) : cheats;
	return ClientProfiles::WriteGame(client.id, game.id, current);
}

} // namespace

bool HostApi::GameRunning(const QString& game_id) const {
	return m_runtime.game == game_id || m_status().running_id == game_id;
}

std::unique_ptr<Configuration> HostApi::TrophyConfiguration(const GameEntry& game, const Client& client) const {
	if (!m_runtime.game.isEmpty()) {
		if (m_runtime.game != game.id || m_runtime.client_id != client.id || !m_runtime.trophy_config) return {};
		return ClientConfig(*m_runtime.trophy_config, {});
	}
	return EffectiveConfig(client, game);
}

void HostApi::Library(const Client& client, const std::function<void(HostReply)>& done) const {
	auto status = m_status();
    if (NativeOwnershipBlocked()) status.pc_busy = true;
	if (!m_runtime.game.isEmpty()) { status.running_id = NativeRuntimeIsLocal() ? QString {} : m_runtime.game; status.pc_busy = true; }
	QJsonArray games;
	for (const auto& game: m_library->Games()) {
		if (!game.info) {
			games.append(QJsonObject {{"id", game.id},
			                          {"title_id", game.source_id},
			                          {"name", game.name},
			                          {"source", game.source},
			                          {"source_id", game.source_id},
			                          {"installed", game.installed},
			                          {"launch_ready", game.owned_launch.has_value()},
			                          {"readiness_reason", game.readiness_reason},
			                          {"capabilities", QJsonArray {}},
			                          {"has_cheats", false},
			                          {"local_running", NativeRuntimeIsLocal() && m_runtime.game == game.id},
		                          {"running", status.running_id == game.id}});
			continue;
		}
		const auto& info = *game.info;
		games.append(QJsonObject {
		    {"id", game.id},
		    {"source", "ps5"},
		    {"source_id", info.title_id},
		    {"installed", true},
		    {"launch_ready", true},
		    {"capabilities", QJsonArray {"settings", "cheats", "trophies"}},
		    {"title_id", info.title_id},
		    {"name", info.name},
		    {"version", info.gameVersion},
		    {"status", EnumToText(info.game_status)},
		    {"has_cheats", QFile::exists(Cheats::PlanPath(TitleId(info)))},
		    {"local_running", NativeRuntimeIsLocal() && m_runtime.game == game.id},
		    {"running", !status.running_id.isEmpty() && status.running_id == game.id},
		});
	}
	QJsonObject reply {{"capabilities", QJsonArray {QStringLiteral("hid-tunnel-v1")}},
	                   {"pc_busy", status.pc_busy},
	                   {"games", games}};
	if (client.scoped()) {
		reply["profile"] = QStringLiteral("device");
	}
	done(Json(200, reply));
}

void HostApi::Metrics(const std::function<void(HostReply)>& done) {
	auto status = m_status();
	if (!m_runtime.game.isEmpty()) status.running_id = m_runtime.game;
	QJsonObject game;
	if (!status.running_id.isEmpty()) {
		if (const auto* entry = m_library->Find(status.running_id); entry != nullptr) {
			game = QJsonObject {{"title_id", entry->info ? entry->info->title_id : entry->source_id},
			                    {"name", entry->info ? entry->info->name : entry->name},
			                    {"running", true}};
		}
	}
	done(Json(200, m_metrics.Build(game, m_runtime.game.isEmpty() ? QString {} :
	    QDir(m_runtime.directory).filePath("_TempData/metrics-fps.json"))));
}

void HostApi::Art(const QString& id, const QString& kind, const QString& if_none_match,
                  const std::function<void(HostReply)>& done) const {
	const auto* game = m_library->Find(id);
	if (game == nullptr) {
		return done(Error(404, QStringLiteral("unknown-game")));
	}
	if (game->source == "steam") {
        const auto data = SteamArtwork::Load(game->artwork_roots, game->source_id, kind != "icon");
        if (data.isEmpty()) return done(Error(404, "no-art"));
        const auto etag = QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex().left(16);
        const auto type = data.startsWith("\x89PNG") ? QByteArray("image/png") : QByteArray("image/jpeg");
        if (if_none_match.toLatin1() == etag) return done({304, type, {}, etag});
        return done({200, type, data, etag});
    }
	if (!game->info) return done(Error(404, "no-art"));
	const QStringList names =
	    kind == QLatin1String("icon")
	        ? QStringList {QStringLiteral("sce_sys/icon0.png")}
	        : QStringList {QStringLiteral("sce_sys/pic0.png"), QStringLiteral("sce_sys/pic1.png")};
	const bool background = kind != QLatin1String("icon");
	for (const auto& name: names) {
		const auto data =
		    GameContent::ReadFile(game->info->basedir, name, GameContent::MaxImageSize);
		if (data.isEmpty()) {
			continue;
		}
		// The ETag covers the source bytes plus the delivery format, so a cached phone copy
		// revalidates without re-encoding.
		auto hash = QCryptographicHash(QCryptographicHash::Sha256);
		hash.addData(data);
		if (background) hash.addData(QByteArrayView("jpeg-1920"));
		const auto       etag = hash.result().toHex().left(16);
		const QByteArray png_type("image/png");
		if (if_none_match.toLatin1() == etag) {
			return done({304, background ? QByteArray("image/jpeg") : png_type, {}, etag});
		}
		if (background) {
			// Game backgrounds are often 4K PNGs (~10 MB); phones get a 1920-wide JPEG.
			QImage image;
			if (image.loadFromData(data)) {
				if (image.width() > MAX_BACKGROUND_WIDTH)
					image = image.scaledToWidth(MAX_BACKGROUND_WIDTH, Qt::SmoothTransformation);
				QByteArray jpeg;
				QBuffer    buffer(&jpeg);
				if (buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "JPG", 85))
					return done({200, "image/jpeg", jpeg, etag});
			}
		}
		return done({200, png_type, data, etag});
	}
	done(Error(404, QStringLiteral("no-art")));
}

void HostApi::GetSettings(const GameEntry& game, const Client& client,
                          const std::function<void(HostReply)>& done) const {
	const auto config = EffectiveConfig(client, game);
	if (config == nullptr) {
		return done(Error(500, QStringLiteral("profile-corrupt")));
	}
	// Seed the device profile on first read so later PC changes do not flow into it.
	if (client.scoped()) {
		bool seeded = false;
		(void)ClientProfiles::ReadGame(client.id, game.id, &seeded);
		if (!seeded) {
			(void)SaveClientProfile(client, game,
			                        QJsonObject::fromVariantMap(config->GameSettings()), {});
		}
	}
	done(Json(200, {{"revision", SettingsRevision(*config)},
	                {"running", GameRunning(game.id)},
	                {"scope", client.scoped() ? QStringLiteral("device") : QStringLiteral("pc")},
	                {"fields", SettingsSchema(*config)}}));
}

void HostApi::PutSettings(const GameEntry& game, const Client& client, const QJsonObject& body,
                          const std::function<void(HostReply)>& done) {
	if (GameRunning(game.id)) {
		return done(Error(409, QStringLiteral("game-running")));
	}
	const auto config = EffectiveConfig(client, game);
	if (config == nullptr) {
		return done(Error(500, QStringLiteral("profile-corrupt")));
	}
	if (body.value(QStringLiteral("revision")).toString() != SettingsRevision(*config)) {
		return done(Error(409, QStringLiteral("stale")));
	}
	const auto values = body.value(QStringLiteral("values"));
	if (!values.isObject()) {
		return done(Error(400, QStringLiteral("bad-body")));
	}
	auto changes = values.toObject();
	for (const auto& key: PATH_KEYS) {
		if (changes.contains(key)) {
			return done(Error(400, QStringLiteral("read-only-setting")));
		}
	}
	// Settings are stored as text; clients may send typed JSON (true, 5). Normalise scalars here.
	for (auto it = changes.begin(); it != changes.end(); ++it) {
		const auto v = it.value();
		if (v.isBool()) {
			it.value() = v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
		} else if (v.isDouble()) {
			const double d = v.toDouble();
			it.value()     = d == static_cast<double>(static_cast<qint64>(d))
			                     ? QString::number(static_cast<qint64>(d))
			                     : QString::number(d);
		}
	}
	// Both scopes speak choice text for enums; store the raw ("R1280X720") form.
	for (const auto& f: FIELDS) {
		const auto key = QString::fromLatin1(f.key);
		if (QLatin1String(f.type) == QLatin1String("enum") && changes.value(key).isString()) {
			changes[key] = RawText(changes.value(key).toString());
		}
	}
	if (!client.scoped()) {
		// SaveGameSettings starts from the game's current values, so only the changed keys are
		// sent.
		const auto error = m_library->SaveGameSettings(game.id, changes);
		if (!error.isEmpty()) {
			return done(Error(400, error));
		}
		return done(Json(200, {{"revision", SettingsRevision(*game.info)}}));
	}
	// Device scope: overlay onto the client's effective settings, validate, and store the whole
	// map. The PC's Kyty.ini is never touched.
	auto next = QJsonObject::fromVariantMap(config->GameSettings());
	for (auto it = changes.constBegin(); it != changes.constEnd(); ++it) {
		next[it.key()] = it.value();
	}
	const auto validated = ClientConfig(*game.info, next);
	if (validated == nullptr) {
		return done(Error(400, QStringLiteral("invalid")));
	}
	const auto canonical = QJsonObject::fromVariantMap(validated->GameSettings());
	if (const auto error = SaveClientProfile(client, game, canonical, {}); !error.isEmpty()) {
		return done(Error(400, error));
	}
	emit ClientProfileChanged();
	done(Json(200, {{"revision", SettingsRevision(*validated)}}));
}

void HostApi::ResetSettings(const GameEntry& game, const Client& client,
                            const std::function<void(HostReply)>& done) {
	if (GameRunning(game.id)) return done(Error(409, QStringLiteral("game-running")));
	if (client.scoped()) {
		if (const auto error = ClientProfiles::RemoveGame(client.id, game.id); !error.isEmpty()) {
			return done(Error(400, error));
		}
		(void)ClientProfiles::WritePatchPlan(client.id, game.id, {}, nullptr);
		emit ClientProfileChanged();
	}
	// Returns the GET body; GetSettings re-seeds the now-missing device profile from the PC.
	GetSettings(game, client, done);
}

void HostApi::GetTrophies(const GameEntry& game, const Client& client, const std::function<void(HostReply)>& done) const {
	const auto info = TrophyConfiguration(game, client);
	if (!info) return done(Error(409, "runtime-unavailable"));
	done(Json(200, Trophies(*info, RuntimeDirectory(game.id))));
}

void HostApi::GetCheats(const GameEntry& game, const Client& client,
                        const std::function<void(HostReply)>& done) const {
	const auto path = Cheats::PlanPath(TitleId(*game.info));
	if (path.isEmpty()) {
		return done(Json(200, {{"supported", false}, {"revision", ""}, {"mods", QJsonArray {}}}));
	}
	const auto    bytes = ReadCheatBytes(path);
	QJsonDocument document;
	QJsonArray    mods;
	QString       error;
	// Cheats change the emulator, not the device: every client sees the host's selection.
	(void)client;
	if (!bytes.isEmpty()) {
		error     = Cheats::ParseDocument(bytes, &document);
		int index = 0;
		for (const auto& value: document.object().value(QStringLiteral("mods")).toArray()) {
			const auto mod  = value.toObject();
			const auto name = mod.value(QStringLiteral("name")).toString();
			const bool enabled = mod.value(QStringLiteral("enabled")).toBool(true);
			mods.append(QJsonObject {{"index", index++}, {"name", name}, {"enabled", enabled}});
		}
	}
	QJsonObject reply {{"supported", true}, {"revision", CheatRevision(bytes)}, {"mods", mods}};
	if (!error.isEmpty()) {
		reply["error"] = error;
	}
	done(Json(200, reply));
}

void HostApi::PutCheats(const GameEntry& game, const Client& client, const QJsonObject& body,
                        const std::function<void(HostReply)>& done) {
	const auto path = Cheats::PlanPath(TitleId(*game.info));
	if (path.isEmpty()) {
		return done(Error(400, QStringLiteral("unsupported-title-id")));
	}
	const auto bytes = ReadCheatBytes(path);
	if (body.value(QStringLiteral("revision")).toString() != CheatRevision(bytes)) {
		return done(Error(409, QStringLiteral("stale")));
	}
	QJsonDocument document;
	if (const auto error = Cheats::ParseDocument(bytes, &document); !error.isEmpty()) {
		return done(Error(400, error));
	}
	const auto selection = body.value(QStringLiteral("enabled"));
	if (!selection.isArray()) {
		return done(Error(400, QStringLiteral("bad-body")));
	}
	QList<bool> enabled;
	for (const auto& value: selection.toArray()) {
		if (!value.isBool()) {
			return done(Error(400, QStringLiteral("bad-body")));
		}
		enabled.append(value.toBool());
	}
	const auto updated = Cheats::WithSelection(document, enabled);
	if (updated.isNull()) {
		return done(Error(400, QStringLiteral("selection-size-mismatch")));
	}
	// Cheats are host-wide: a device's change updates the host's _Patches/<TITLE>.json.
	if (!Cheats::SaveDocument(path, updated, bytes).isEmpty()) {
		return done(Error(409, QStringLiteral("stale")));
	}
	if (client.scoped()) {
		// Retire any earlier per-device plan so it can never shadow the host's.
		(void)ClientProfiles::WritePatchPlan(client.id, game.id, {}, nullptr);
		emit ClientProfileChanged();
	}
	return done(Json(200, {{"revision", CheatRevision(ReadCheatBytes(path))}}));
}

QJsonObject HostApi::Trophies(const Configuration& info, const QString& runtime_directory) {
	static const QRegularExpression file_pattern(QStringLiteral("^trophy(\\d+)\\.ucp$"));
	const auto                      grade_name = [](int grade) -> QString {
		switch (grade) {
			case 1: return QStringLiteral("platinum");
			case 2: return QStringLiteral("gold");
			case 3: return QStringLiteral("silver");
			case 4: return QStringLiteral("bronze");
			default: return QStringLiteral("bronze");
		}
	};
	QJsonArray  trophies;
	int         unlocked_count = 0;
	int         total          = 0;
	const auto  reader         = Common::OpenArchive(GameContent::ToPath(info.basedir));
	QStringList files;
	for (const auto& file: GameContent::ListFiles(
	         info.basedir, QString::fromLatin1(Common::Trophies::PackageDirectory))) {
		if (file_pattern.match(QFileInfo(file).fileName()).hasMatch()) {
			files.append(file);
		}
	}
	files.sort();
	for (const auto& file: files) {
		const auto label = file_pattern.match(QFileInfo(file).fileName()).captured(1).toUInt();
		const auto package =
		    Common::Trophies::LoadPackage(GameContent::ToPath(file), info.console_language);
		const auto unlocks = Common::Trophies::LoadUnlockDataMigrating(
		    GameContent::ToPath(runtime_directory), info.title_id.toStdString(), info.user_id,
		    label);
		for (const auto& [id, trophy]: package.trophies) {
			const bool is_unlocked = unlocks.unlocked.contains(id);
			++total;
			if (is_unlocked) {
				++unlocked_count;
			}
			QJsonObject entry {{"id", id},
			                   {"name", QString::fromStdString(trophy.name)},
			                   {"detail", QString::fromStdString(trophy.description)},
			                   {"grade", grade_name(trophy.grade)},
			                   {"hidden", trophy.hidden},
			                   {"unlocked", is_unlocked}};
			// Icon is fetched separately (GET .../trophies/icon/<key>); hidden locked trophies
			// get none so their art is not revealed.
			if (!trophy.icon_png.empty() && (is_unlocked || !trophy.hidden))
				entry["icon"] = QStringLiteral("%1-%2").arg(label).arg(id);
			if (is_unlocked) {
				const auto found = unlocks.timestamps.find(id);
				if (found != unlocks.timestamps.end()) {
					entry["unlocked_at"] =
					    QDateTime::fromMSecsSinceEpoch(
					        static_cast<qint64>((found->second - Common::Trophies::UnixEpochTick) /
					                            1000),
					        QTimeZone::UTC)
					        .toString(Qt::ISODate);
				}
			}
			trophies.append(entry);
		}
	}
	return {{"trophies", trophies}, {"unlocked", unlocked_count}, {"total", total}};
}

HostReply HostApi::TrophyIcon(const Configuration& info, const QString& runtime_directory,
                              int label, int id, const QString& if_none_match) {
	static const QRegularExpression file_pattern(QStringLiteral("^trophy(\\d+)\\.ucp$"));
	for (const auto& file: GameContent::ListFiles(
	         info.basedir, QString::fromLatin1(Common::Trophies::PackageDirectory))) {
		const auto match = file_pattern.match(QFileInfo(file).fileName());
		if (!match.hasMatch() || match.captured(1).toInt() != label) continue;
		const auto package =
		    Common::Trophies::LoadPackage(GameContent::ToPath(file), info.console_language);
		const auto found = package.trophies.find(id);
		if (found == package.trophies.end() || found->second.icon_png.empty()) break;
		if (found->second.hidden) {
			const auto unlocks = Common::Trophies::LoadUnlockDataMigrating(
			    GameContent::ToPath(runtime_directory), info.title_id.toStdString(), info.user_id,
			    static_cast<uint32_t>(label));
			if (!unlocks.unlocked.contains(id)) return Error(404, QStringLiteral("hidden"));
		}
		const QByteArray data(reinterpret_cast<const char*>(found->second.icon_png.data()),
		                      static_cast<qsizetype>(found->second.icon_png.size()));
		const auto       etag =
		    QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex().left(16);
		if (if_none_match.toLatin1() == etag) return {304, "image/png", {}, etag};
		return {200, "image/png", data, etag};
	}
	return Error(404, QStringLiteral("no-icon"));
}

void HostApi::Enqueue(std::function<void()> job) {
	m_pending.append(std::move(job));
	if (!m_busy) {
		Finish();
	}
}

void HostApi::Finish() {
	if (m_pending.isEmpty()) {
		m_busy = false;
		return;
	}
	m_busy   = true;
	auto job = m_pending.takeFirst();
	job();
}

void HostApi::GetRemote(const GameEntry& game, std::function<void(HostReply)> done) {
	const auto title_id = TitleId(*game.info);
	const auto path     = Cheats::PlanPath(title_id);
	if (path.isEmpty()) {
		return done(Json(200, {{"files", QJsonArray {}}, {"error", "unsupported-title-id"}}));
	}
	Enqueue([this, title_id, path, done = std::move(done)] {
		m_on_catalog = [this, title_id, path, done](QList<Cheats::RemoteFile> files,
		                                            const QString&            error) {
			QJsonDocument local;
			(void)Cheats::ParseDocument(ReadCheatBytes(path), &local);
			const auto local_version = local.object().value(QStringLiteral("version")).toString();
			QList<Cheats::RemoteFile> supported;
			QJsonArray                list;
			for (const auto& file: files) {
				if (!file.IsSupported()) {
					continue;
				}
				supported.append(file);
				list.append(QJsonObject {{"name", file.name},
				                         {"title", file.title},
				                         {"version", file.version},
				                         {"format", file.format},
				                         {"installed_version", !local_version.isEmpty() &&
				                                                   local_version == file.version}});
			}
			m_remote.insert(title_id, supported);
			QJsonObject reply {{"files", list}};
			if (!error.isEmpty()) {
				reply["error"] = error;
			}
			done(Json(200, reply));
			Finish();
		};
		m_repository.Load(title_id);
	});
}

void HostApi::FetchRemote(const GameEntry& game, const QJsonObject& body,
                          std::function<void(HostReply)> done) {
	const auto title_id = TitleId(*game.info);
	const auto name     = body.value(QStringLiteral("name")).toString();
	const auto cached   = m_remote.value(title_id);
	const auto found    = std::find_if(cached.begin(), cached.end(),
	                                   [&name](const auto& file) { return file.name == name; });
	if (name.isEmpty() || found == cached.end()) {
		return done(Error(404, QStringLiteral("unknown-file")));
	}
	const auto file    = *found;
	const auto version = game.info->gameVersion;
	const auto process = Process(*game.info);
	Enqueue([this, file, title_id, version, process, done = std::move(done)] {
		m_on_file = [this, title_id, version, process, done](const QByteArray& data,
		                                                     const QString&    fetch_error) {
			const auto finish = [&](HostReply reply) {
				done(std::move(reply));
				Finish();
			};
			if (!fetch_error.isEmpty()) {
				return finish(Error(502, fetch_error));
			}
			QJsonDocument document;
			auto          error = Cheats::ParseDocument(data, &document);
			if (error.isEmpty()) {
				error = Cheats::ValidateImport(document, title_id, version, process);
			}
			if (!error.isEmpty()) {
				return finish(Error(400, error));
			}
			const auto path  = Cheats::PlanPath(title_id);
			const auto mods  = document.object().value(QStringLiteral("mods")).toArray();
			const auto saved = Cheats::WithSelection(document, QList<bool>(mods.size(), false));
			if (const auto save_error = Cheats::SaveDocument(path, saved, ReadCheatBytes(path));
			    !save_error.isEmpty()) {
				return finish(Error(400, save_error));
			}
			finish(Json(200, {{"revision", CheatRevision(ReadCheatBytes(path))}}));
		};
		m_repository.Fetch(file);
	});
}
