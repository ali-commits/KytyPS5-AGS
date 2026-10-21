#include "hostApi.h"

#include "cheatFile.h"
#include "configuration.h"
#include "gameContent.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <utility>

namespace {

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
    {"shader_log_folder", "Shader log folder", "string", 0, 0},
    {"command_buffer_dump_enabled", "Command buffer dump", "bool", 0, 0},
    {"command_buffer_dump_folder", "Command buffer dump folder", "string", 0, 0},
    {"printf_direction", "Printf output", "enum", 0, 0},
    {"printf_output_file", "Printf output file", "string", 0, 0},
    {"profiler_enabled", "Profiler", "bool", 0, 0},
    {"renderdoc_enabled", "RenderDoc", "bool", 0, 0},
    {"amd_cpu_mode", "AMD CPU mode", "enum", 0, 0},
    {"red_zone_protection_enabled", "Red zone protection", "bool", 0, 0},
    {"elf", "Executable", "string", 0, 0},
};

QStringList Choices(const QString& key) {
	if (key == QLatin1String("screen_resolution")) {
		return EnumToList<Configuration::Resolution>();
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
		if (!values.contains(key)) {
			return; // e.g. a Windows-only key
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
	static const QRegularExpression ROUTE(QStringLiteral(
	    R"(\A/kyty/v1/(?:(library)|art/([0-9a-f]{64})/(icon|background)|games/([0-9a-f]{64})/)"
	    R"((settings|cheats|cheats/remote|cheats/remote/fetch))\z)"));

	const auto method = request.value(QStringLiteral("method")).toString();
	const auto match  = ROUTE.match(request.value(QStringLiteral("path")).toString());
	if (!match.hasMatch()) {
		return done(Error(404, QStringLiteral("not-found")));
	}
	const bool    is_library = !match.captured(1).isEmpty();
	const bool    is_art     = !match.captured(2).isEmpty();
	const QString sub        = match.captured(5);
	const bool    reads      = is_library || is_art || sub == QLatin1String("cheats/remote");
	const bool    writes     = sub == QLatin1String("settings") || sub == QLatin1String("cheats");
	const bool    posts      = sub == QLatin1String("cheats/remote/fetch");
	if (!((method == QLatin1String("GET") && (reads || writes)) ||
	      (method == QLatin1String("PUT") && writes) ||
	      (method == QLatin1String("POST") && posts))) {
		return done(Error(405, QStringLiteral("method-not-allowed")));
	}
	if (is_library) {
		return Library(done);
	}
	if (is_art) {
		return Art(match.captured(2), match.captured(3),
		           request.value(QStringLiteral("if_none_match")).toString(), done);
	}
	const auto* game = m_library->Find(match.captured(4));
	if (game == nullptr) {
		return done(Error(404, QStringLiteral("unknown-game")));
	}
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
		return method == QLatin1String("GET") ? GetSettings(*game, done)
		                                      : PutSettings(*game, body, done);
	}
	if (sub == QLatin1String("cheats")) {
		return method == QLatin1String("GET") ? GetCheats(*game, done)
		                                      : PutCheats(*game, body, done);
	}
	if (sub == QLatin1String("cheats/remote")) {
		return GetRemote(*game, std::move(done));
	}
	FetchRemote(*game, body, std::move(done));
}

void HostApi::Library(const std::function<void(HostReply)>& done) const {
	const auto status = m_status();
	QJsonArray games;
	for (const auto& game: m_library->Games()) {
		const auto& info = *game.info;
		games.append(QJsonObject {
		    {"id", game.id},
		    {"title_id", info.title_id},
		    {"name", info.name},
		    {"version", info.gameVersion},
		    {"status", EnumToText(info.game_status)},
		    {"has_cheats", QFile::exists(Cheats::PlanPath(TitleId(info)))},
		    {"running", !status.running_id.isEmpty() && status.running_id == game.id},
		});
	}
	done(Json(200, {{"capabilities", QJsonArray {QStringLiteral("hid-tunnel-v1")}},
	                {"pc_busy", status.pc_busy},
	                {"games", games}}));
}

void HostApi::Art(const QString& id, const QString& kind, const QString& if_none_match,
                  const std::function<void(HostReply)>& done) const {
	const auto* game = m_library->Find(id);
	if (game == nullptr) {
		return done(Error(404, QStringLiteral("unknown-game")));
	}
	const QStringList names =
	    kind == QLatin1String("icon")
	        ? QStringList {QStringLiteral("sce_sys/icon0.png")}
	        : QStringList {QStringLiteral("sce_sys/pic0.png"), QStringLiteral("sce_sys/pic1.png")};
	for (const auto& name: names) {
		const auto data =
		    GameContent::ReadFile(game->info->basedir, name, GameContent::MaxImageSize);
		if (data.isEmpty()) {
			continue;
		}
		const auto etag =
		    QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex().left(16);
		if (if_none_match.toLatin1() == etag) {
			return done({304, "image/png", {}, etag});
		}
		return done({200, "image/png", data, etag});
	}
	done(Error(404, QStringLiteral("no-art")));
}

void HostApi::GetSettings(const GameEntry& game, const std::function<void(HostReply)>& done) const {
	done(Json(200, {{"revision", SettingsRevision(*game.info)},
	                {"running", m_status().running_id == game.id},
	                {"fields", SettingsSchema(*game.info)}}));
}

void HostApi::PutSettings(const GameEntry& game, const QJsonObject& body,
                          const std::function<void(HostReply)>& done) {
	if (!m_status().running_id.isEmpty() && m_status().running_id == game.id) {
		return done(Error(409, QStringLiteral("game-running")));
	}
	if (body.value(QStringLiteral("revision")).toString() != SettingsRevision(*game.info)) {
		return done(Error(409, QStringLiteral("stale")));
	}
	const auto values = body.value(QStringLiteral("values"));
	if (!values.isObject()) {
		return done(Error(400, QStringLiteral("bad-body")));
	}
	// SaveGameSettings starts from the game's current values, so only the changed keys are sent.
	auto changes = values.toObject();
	for (const auto& f: FIELDS) {
		const auto key = QString::fromLatin1(f.key);
		if (QLatin1String(f.type) == QLatin1String("enum") && changes.value(key).isString()) {
			changes[key] = RawText(changes.value(key).toString());
		}
	}
	const auto error = m_library->SaveGameSettings(game.id, changes);
	if (!error.isEmpty()) {
		return done(Error(400, error));
	}
	done(Json(200, {{"revision", SettingsRevision(*game.info)}}));
}

void HostApi::GetCheats(const GameEntry& game, const std::function<void(HostReply)>& done) const {
	const auto path = Cheats::PlanPath(TitleId(*game.info));
	if (path.isEmpty()) {
		return done(Json(200, {{"supported", false}, {"revision", ""}, {"mods", QJsonArray {}}}));
	}
	const auto    bytes = ReadCheatBytes(path);
	QJsonDocument document;
	QJsonArray    mods;
	QString       error;
	if (!bytes.isEmpty()) {
		error     = Cheats::ParseDocument(bytes, &document);
		int index = 0;
		for (const auto& value: document.object().value(QStringLiteral("mods")).toArray()) {
			const auto mod = value.toObject();
			mods.append(
			    QJsonObject {{"index", index++},
			                 {"name", mod.value(QStringLiteral("name")).toString()},
			                 {"enabled", mod.value(QStringLiteral("enabled")).toBool(true)}});
		}
	}
	QJsonObject reply {{"supported", true}, {"revision", CheatRevision(bytes)}, {"mods", mods}};
	if (!error.isEmpty()) {
		reply["error"] = error;
	}
	done(Json(200, reply));
}

void HostApi::PutCheats(const GameEntry& game, const QJsonObject& body,
                        const std::function<void(HostReply)>& done) const {
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
	if (!Cheats::SaveDocument(path, updated, bytes).isEmpty()) {
		return done(Error(409, QStringLiteral("stale")));
	}
	done(Json(200, {{"revision", CheatRevision(ReadCheatBytes(path))}}));
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
