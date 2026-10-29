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
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStringList>
#include <QTimeZone>

#include <algorithm>
#include <memory>
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
	if (sub == QLatin1String("trophies")) {
		return GetTrophies(*game, done);
	}
	if (is_trophy_icon) {
		const auto key = sub.mid(QStringLiteral("trophies/icon/").size()).split(QLatin1Char('-'));
		return done(TrophyIcon(*game->info, QCoreApplication::applicationDirPath(),
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

void HostApi::Library(const Client& client, const std::function<void(HostReply)>& done) const {
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
	QJsonObject reply {{"capabilities", QJsonArray {QStringLiteral("hid-tunnel-v1")}},
	                   {"pc_busy", status.pc_busy},
	                   {"games", games}};
	if (client.scoped()) {
		reply["profile"] = QStringLiteral("device");
	}
	done(Json(200, reply));
}

void HostApi::Metrics(const std::function<void(HostReply)>& done) {
	const auto  status = m_status();
	QJsonObject game;
	if (!status.running_id.isEmpty()) {
		if (const auto* entry = m_library->Find(status.running_id); entry != nullptr) {
			game = QJsonObject {{"title_id", entry->info->title_id},
			                    {"name", entry->info->name},
			                    {"running", true}};
		}
	}
	done(Json(200, m_metrics.Build(game)));
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
	                {"running", m_status().running_id == game.id},
	                {"scope", client.scoped() ? QStringLiteral("device") : QStringLiteral("pc")},
	                {"fields", SettingsSchema(*config)}}));
}

void HostApi::PutSettings(const GameEntry& game, const Client& client, const QJsonObject& body,
                          const std::function<void(HostReply)>& done) {
	if (!m_status().running_id.isEmpty() && m_status().running_id == game.id) {
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

void HostApi::GetTrophies(const GameEntry& game, const std::function<void(HostReply)>& done) const {
	done(Json(200, Trophies(*game.info, QCoreApplication::applicationDirPath())));
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
	QJsonObject   client_cheats;
	if (client.scoped()) {
		bool seeded   = false;
		client_cheats = ClientProfiles::ReadGame(client.id, game.id, &seeded)
		                    .value(QStringLiteral("cheats"))
		                    .toObject();
	}
	if (!bytes.isEmpty()) {
		error     = Cheats::ParseDocument(bytes, &document);
		int index = 0;
		for (const auto& value: document.object().value(QStringLiteral("mods")).toArray()) {
			const auto mod  = value.toObject();
			const auto name = mod.value(QStringLiteral("name")).toString();
			// Device scope shows this client's own selection, seeded from the plan's flag.
			const bool enabled = client.scoped() && client_cheats.contains(name)
			                         ? client_cheats.value(name).toBool()
			                         : mod.value(QStringLiteral("enabled")).toBool(true);
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
	if (!client.scoped()) {
		if (!Cheats::SaveDocument(path, updated, bytes).isEmpty()) {
			return done(Error(409, QStringLiteral("stale")));
		}
		return done(Json(200, {{"revision", CheatRevision(ReadCheatBytes(path))}}));
	}
	// Device scope: store the selection keyed by mod name, write the per-client plan, and NEVER
	// touch _Patches/<TITLE>.json.
	const auto  mods = document.object().value(QStringLiteral("mods")).toArray();
	QJsonObject cheats;
	bool        any_enabled = false;
	for (qsizetype i = 0; i < mods.size(); ++i) {
		cheats[mods[i].toObject().value(QStringLiteral("name")).toString()] = enabled[i];
		any_enabled = any_enabled || enabled[i];
	}
	if (const auto error = SaveClientProfile(client, game, {}, cheats); !error.isEmpty()) {
		return done(Error(400, error));
	}
	const auto plan = any_enabled ? updated.toJson() : QByteArray {};
	if (const auto error = ClientProfiles::WritePatchPlan(client.id, game.id, plan, nullptr);
	    !error.isEmpty()) {
		return done(Error(400, error));
	}
	emit ClientProfileChanged();
	done(Json(200, {{"revision", CheatRevision(bytes)}}));
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
