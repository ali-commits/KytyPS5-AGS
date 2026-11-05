#include "adapter.h"

#include "gameLibrary.h"
#include "launchPlan.h"
#include "library.h"
#include "settings.h"

#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>

#ifndef KYTY_AGS_ADAPTER_VERSION
#error "KYTY_AGS_ADAPTER_VERSION must be defined by CMake (project version)"
#endif

namespace {

QString IniSnapshot(const QString& ini) {
	const QFileInfo info(ini);
	return info.exists() ? QStringLiteral("%1:%2")
	                           .arg(info.lastModified().toMSecsSinceEpoch())
	                           .arg(info.size())
	                     : QString {};
}

QJsonObject Metric(const char* key, const char* label, const char* unit, const char* type) {
	return {
	    {"key", key}, {"label", label}, {"unit", unit}, {"type", type}, {"min_interval_ms", 1000}};
}

} // namespace

Adapter::Adapter(const QString& data_dir, const QString& adapter_dir,
                 std::function<void(const QJsonObject&)> notify, QObject* parent)
    : QObject(parent), m_state(data_dir), m_adapter_dir(adapter_dir), m_notify(std::move(notify)) {
	m_debounce.setSingleShot(true);
	m_debounce.setInterval(500);
	connect(&m_watcher, &QFileSystemWatcher::fileChanged, &m_debounce, qOverload<>(&QTimer::start));
	connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_debounce,
	        qOverload<>(&QTimer::start));
	connect(&m_debounce, &QTimer::timeout, this, [this] {
		const auto ini    = AdapterState::IniPath(m_runtime_root);
		const auto before = property("ini_snapshot").toString();
		const auto now    = IniSnapshot(ini);
		Watch(); // a rewrite by rename drops the file from the watcher
		if (now != before) {
			setProperty("ini_snapshot", now);
			m_library_dirty = true;
			m_notify(QJsonObject {{"method", "library.changed"}});
		}
	});
	// Start watching right away when the runtime root is usable, so a change before the first
	// call is still reported.
	QString error;
	m_runtime_root = m_state.RuntimeRoot(&error);
	if (!m_runtime_root.isEmpty()) {
		setProperty("ini_snapshot", IniSnapshot(AdapterState::IniPath(m_runtime_root)));
		Watch();
	}
}

Adapter::~Adapter() = default;

void Adapter::Watch() {
	if (!m_watcher.files().isEmpty()) m_watcher.removePaths(m_watcher.files());
	if (!m_watcher.directories().isEmpty()) m_watcher.removePaths(m_watcher.directories());
	if (m_runtime_root.isEmpty()) return;
	m_watcher.addPath(m_runtime_root);
	if (const auto ini = AdapterState::IniPath(m_runtime_root); QFileInfo::exists(ini)) {
		m_watcher.addPath(ini);
	}
}

Reply Adapter::Hello(const QJsonObject& /*params*/) const {
	const QJsonObject honoured {{"level", "honoured"}};
	return Reply::Success({
	    {"protocol", 1},
	    {"version", QStringLiteral(KYTY_AGS_ADAPTER_VERSION)},
	    {"capabilities", QJsonArray {"library", "settings", "actions", "native_frames", "metrics"}},
	    {"standard",
		 QJsonObject {{"resolution", honoured},
		              {"fps_limit", honoured},
		              {"aspect", honoured},
		              {"language", honoured},
		              {"player_name", honoured},
		              {"controller", QJsonObject {{"level", "fixed"}, {"value", "dualsense"}}}}},
	    {"controllers",
		 QJsonArray {QJsonObject {
	         {"kind", "dualsense"},
	         {"features", QJsonArray {"gyro", "touchpad", "adaptive_triggers", "haptics"}}}}},
	    {"metrics", QJsonArray {Metric("game_fps", "Game FPS", "fps", "number"),
		                        Metric("frame_time_ms", "Frame time", "ms", "number"),
		                        Metric("guest_memory_mb", "Guest memory", "MB", "number"),
		                        Metric("shader_compiles", "Shader compiles", "count", "integer")}},
	});
}

bool Adapter::Ready(Reply* reply) {
	QString    error;
	const auto root = m_state.RuntimeRoot(&error);
	if (root.isEmpty()) {
		*reply = Reply::Failed(error);
		return false;
	}
	if (root != m_runtime_root) {
		m_runtime_root  = root;
		m_library_dirty = true;
		setProperty("ini_snapshot", IniSnapshot(AdapterState::IniPath(root)));
		Watch();
	}
	if (m_library_dirty || m_library == nullptr) {
		auto library = std::make_unique<GameLibrary>();
		library->SetSettingsPath(AdapterState::IniPath(m_runtime_root));
		// No CompatibilityDatabase: it reads the working directory.
		if (!library->Load(nullptr, &error)) {
			*reply =
			    Reply::Failed(error.isEmpty() ? QStringLiteral("Cannot read Kyty.ini.") : error);
			return false;
		}
		m_library       = std::move(library);
		m_library_dirty = false;
	}
	return true;
}

bool Adapter::FindGame(const QJsonObject& params, bool required, const GameEntry** game,
                       Reply* reply) {
	*game = nullptr;
	if (!params.contains(QStringLiteral("game"))) {
		if (required) *reply = Reply::Invalid(QStringLiteral("game is required."));
		return !required;
	}
	if (!params[QStringLiteral("game")].isString()) {
		*reply = Reply::Invalid(QStringLiteral("game must be a string."));
		return false;
	}
	*game = m_library->Find(params[QStringLiteral("game")].toString());
	if (*game == nullptr) {
		*reply = Reply::NotFound(QStringLiteral("No such game."));
		return false;
	}
	return true;
}

Reply Adapter::Call(const QString& method, const QJsonObject& params) {
	if (method == QLatin1String("hello")) return Hello(params);
	if (method == QLatin1String("settings.schema")) {
		return Reply::Success({{"fields", AdapterSettings::Schema()}});
	}
	if (method == QLatin1String("launch.ended")) return Reply::Success();
	const bool known =
	    method == QLatin1String("library.list") || method == QLatin1String("settings.get") ||
	    method == QLatin1String("settings.set") || method == QLatin1String("actions.list") ||
	    method == QLatin1String("actions.run") || method == QLatin1String("launch.plan");
	if (!known) {
		return Reply::Error(QStringLiteral("unknown-method"),
		                    QStringLiteral("Unknown method %1.").arg(method));
	}
	// library.list always scans afresh. settings.set starts from a fresh read of Kyty.ini too, so a
	// stale cache can never overwrite what the launcher saved meanwhile.
	if (method == QLatin1String("library.list") || method == QLatin1String("settings.set")) {
		m_library_dirty = true;
	}
	Reply reply;
	if (method == QLatin1String("settings.set")) {
		// Writing would replace a Kyty.ini we cannot parse: refuse before anything is loaded.
		QString    root_error;
		const auto root = m_state.RuntimeRoot(&root_error);
		if (!root.isEmpty() && !AdapterSettings::IniReadable(AdapterState::IniPath(root))) {
			return Reply::Failed(QString::fromLatin1(AdapterSettings::INI_UNREADABLE));
		}
	}
	if (!Ready(&reply)) return reply;
	const auto       ini  = AdapterState::IniPath(m_runtime_root);
	const GameEntry* game = nullptr;

	if (method == QLatin1String("library.list")) {
		return Reply::Success({{"games", AdapterLibrary::Games(*m_library, m_state.DataDir(),
		                                                       m_runtime_root, m_art_cache)}});
	}
	if (method == QLatin1String("settings.get") || method == QLatin1String("settings.set")) {
		const auto scope = params[QStringLiteral("scope")].toString(QStringLiteral("global"));
		if (scope != QLatin1String("global") && scope != QLatin1String("game")) {
			return Reply::Invalid(QStringLiteral("scope must be global or game."));
		}
		if (scope == QLatin1String("game") && !FindGame(params, true, &game, &reply)) return reply;
		if (method == QLatin1String("settings.get")) {
			return AdapterSettings::Get(m_state, ini, game);
		}
		if (!params[QStringLiteral("values")].isObject()) {
			return Reply::Invalid(QStringLiteral("values must be an object."));
		}
		auto result = AdapterSettings::Set(m_state, ini, *m_library, game,
		                                   params[QStringLiteral("values")].toObject());
		// A global change (or a new runtime root) changes every game's merged values.
		if (game == nullptr) m_library_dirty = true;
		if (result.Ok() &&
		    params[QStringLiteral("values")].toObject().contains(QStringLiteral("runtime_root"))) {
			// Watch the new root's Kyty.ini and tell AGS that the library may differ.
			Reply ignored;
			if (Ready(&ignored)) m_notify(QJsonObject {{"method", "library.changed"}});
		}
		return result;
	}
	if (method == QLatin1String("actions.list")) {
		if (!FindGame(params, false, &game, &reply)) return reply;
		return AdapterActions::List(game);
	}
	if (method == QLatin1String("actions.run")) {
		if (!params[QStringLiteral("action_id")].isString()) {
			return Reply::Invalid(QStringLiteral("action_id is required."));
		}
		if (!FindGame(params, true, &game, &reply)) return reply;
		return AdapterActions::Run(*game, params, m_runtime_root, m_state.DataDir(), m_fetcher);
	}
	// launch.plan
	if (!FindGame(params, true, &game, &reply)) return reply;
	return AdapterLaunch::Plan(*game, params, m_runtime_root, m_adapter_dir);
}
