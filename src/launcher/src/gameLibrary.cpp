#include "gameLibrary.h"

#include "clientProfiles.h"
#include "common/archive.h"
#include "compatibilityDatabase.h"
#include "emulatorCommand.h"
#include "gameContent.h"
#include "hostApi.h"
#include "streamingSettings.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <optional>

namespace {
constexpr char CONF_FILE_NAME[] = "Kyty.ini";
constexpr char CONF_ORG_NAME[]  = "Kyty";
constexpr char CONF_APP_NAME[]  = "Kyty";
} // namespace

using namespace GameLibraryFiles;

namespace GameLibraryFiles {

std::unique_ptr<QSettings> OpenSettings() {
	if (QFile(QDir(".").absoluteFilePath(CONF_FILE_NAME)).exists()) {
		return std::make_unique<QSettings>(CONF_FILE_NAME, QSettings::IniFormat);
	}
#ifdef __linux__
	return std::make_unique<QSettings>(QSettings::IniFormat, QSettings::UserScope, CONF_ORG_NAME,
	                                   CONF_APP_NAME);
#else
	return std::make_unique<QSettings>(QSettings::IniFormat, QSettings::SystemScope, CONF_ORG_NAME,
	                                   CONF_APP_NAME);
#endif
}

QString NormalizeGameDirectory(const QString& dir) {
	const auto trimmed = dir.trimmed();
	if (trimmed.isEmpty()) {
		return {};
	}

	return QDir::cleanPath(QDir(trimmed).absolutePath());
}

QString PathKey(const QString& path) {
	auto normalized = NormalizeGameDirectory(path);
	if (normalized.isEmpty()) {
		normalized = QDir::cleanPath(path.trimmed());
	}
	if (normalized.isEmpty()) {
		return {};
	}

	auto canonical = QFileInfo(normalized).canonicalFilePath();
	if (canonical.isEmpty()) {
		canonical = normalized;
	}
	canonical = QDir::cleanPath(canonical);

#ifdef __linux__
	return canonical;
#else
	return canonical.toCaseFolded();
#endif
}

QStringList NormalizeGameDirectories(const QStringList& dirs) {
	QStringList   dirs_ret;
	QSet<QString> seen;

	for (const auto& dir: dirs) {
		const auto normalized = NormalizeGameDirectory(dir);
		const auto key        = PathKey(normalized);
		if (normalized.isEmpty() || key.isEmpty() || seen.contains(key)) {
			continue;
		}

		seen.insert(key);
		dirs_ret.append(normalized);
	}

	return dirs_ret;
}

QStringList SettingsStringList(const QVariant& value) {
	auto list = value.toStringList();
	if (!list.isEmpty()) {
		return list;
	}

	const auto text = value.toString();
	return text.isEmpty() ? QStringList() : QStringList({text});
}

static QString GetJsonString(const QJsonObject& obj, const QString& key) {
	return obj.value(key).toString().trimmed();
}

static QString GetLocalizedTitleName(const QJsonObject& root) {
	const auto localized = root.value(QStringLiteral("localizedParameters")).toObject();
	if (localized.isEmpty()) {
		return {};
	}

	const auto default_language = GetJsonString(localized, QStringLiteral("defaultLanguage"));
	if (!default_language.isEmpty()) {
		const auto title = GetJsonString(localized.value(default_language).toObject(),
		                                 QStringLiteral("titleName"));
		if (!title.isEmpty()) {
			return title;
		}
	}

	const auto english_title = GetJsonString(localized.value(QStringLiteral("en-US")).toObject(),
	                                         QStringLiteral("titleName"));
	if (!english_title.isEmpty()) {
		return english_title;
	}

	for (auto it = localized.constBegin(); it != localized.constEnd(); ++it) {
		const auto title = GetJsonString(it.value().toObject(), QStringLiteral("titleName"));
		if (!title.isEmpty()) {
			return title;
		}
	}

	return {};
}

static QString GetFirmwareVersion(const QJsonObject& root) {
	const auto encoded = GetJsonString(root, QStringLiteral("requiredSystemSoftwareVersion"));
	static const QRegularExpression version_pattern(
	    QStringLiteral("^0[xX]([0-9]{6})[0-9A-Fa-f]{10}$"));
	const auto match = version_pattern.match(encoded);
	if (!match.hasMatch()) {
		return {};
	}

	const auto digits = match.captured(1);
	bool       valid  = false;
	const int  major  = digits.left(2).toInt(&valid, 10);
	if (!valid) {
		return {};
	}

	QString    version = QStringLiteral("%1.%2").arg(major).arg(digits.mid(2, 2));
	const auto patch   = digits.mid(4, 2);
	if (patch != QStringLiteral("00")) {
		version += QStringLiteral(".") + patch;
	}

	return version;
}

GameMetadata GetGameMetadata(const QByteArray& param_data, const QString& fallback) {
	GameMetadata ret;
	ret.title_name = fallback;

	if (param_data.isEmpty()) {
		return ret;
	}

	QJsonParseError parse_error;
	const auto      doc = QJsonDocument::fromJson(param_data, &parse_error);
	if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
		return ret;
	}

	const auto root  = doc.object();
	const auto title = GetLocalizedTitleName(root);
	if (!title.isEmpty()) {
		ret.title_name = title;
	}

	ret.title_id = GetJsonString(root, QStringLiteral("titleId"));
	if (ret.title_id.isEmpty()) {
		const auto content_id = GetJsonString(root, QStringLiteral("contentId"));
		if (content_id.size() == 36 && content_id[6] == '-' && content_id[16] == '_') {
			ret.title_id = content_id.mid(7, 9);
		}
	}
	ret.gameVersion = GetJsonString(root, QStringLiteral("appVersion"));
	if (ret.gameVersion.isEmpty()) {
		ret.gameVersion = GetJsonString(root, QStringLiteral("contentVersion"));
	}
	ret.firmwareVer = GetFirmwareVersion(root);

	return ret;
}

void SetGameFiles(Configuration& info, const QString& game_dir, const QString& game_path,
                  const GameMetadata& metadata, bool archive) {
	const QFileInfo game(game_dir);

	info.game_path   = game_path;
	info.basedir     = archive ? game.absoluteFilePath() : QDir(game_dir).absolutePath();
	info.name        = metadata.title_name;
	info.title_id    = metadata.title_id;
	info.gameVersion = metadata.gameVersion;
	info.firmwareVer = metadata.firmwareVer;

	if (info.name.isEmpty()) {
		info.name = archive ? game.completeBaseName() : QDir(game_dir).dirName();
	}
}

Configuration* FindCustomInfo(QMap<QString, Configuration*>* custom_infos, const QString& game_path,
                              const QString& legacy_game_path) {
	auto custom = custom_infos->find(game_path);
	if (custom != custom_infos->end()) {
		return custom.value();
	}

	custom = custom_infos->find(legacy_game_path);
	if (custom == custom_infos->end()) {
		return nullptr;
	}

	auto* info = custom.value();
	custom_infos->erase(custom);
	info->game_path = game_path;
	custom_infos->insert(game_path, info);

	return info;
}

QList<GameCandidate> FindCandidates(const QStringList& game_dirs) {
	const QString        eboot_name = QStringLiteral("eboot.bin");
	QList<GameCandidate> out;
	QSet<QString>        seen;
	const auto           add = [&](GameCandidate c) {
		const auto key = PathKey(c.game_path);
		if (key.isEmpty() || seen.contains(key)) {
			return;
		}
		seen.insert(key);
		out.append(std::move(c));
	};
	const auto scan_archives = [&](const QDir& directory, const QDir& root) {
		for (const auto& archive:
		     directory.entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name)) {
			const auto base = archive.absoluteFilePath();
			if (!Common::IsSupportedArchive(GameContent::ToPath(base))) {
				continue;
			}
			add({base, QDir::cleanPath(base), root.relativeFilePath(base),
			     archive.completeBaseName(), true});
		}
	};
	for (const auto& root_path: game_dirs) {
		QDir root(root_path);
		if (root_path.isEmpty() || !root.exists()) {
			continue;
		}
		scan_archives(root, root);
		QList<QDir> pending;
		for (const auto& sub:
		     root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
			pending.append(QDir(sub.absoluteFilePath()));
		}
		while (!pending.isEmpty()) {
			QDir game_dir = pending.takeFirst();
			scan_archives(game_dir, root);
			if (game_dir.exists(eboot_name)) {
				add({game_dir.absolutePath(), NormalizeGameDirectory(game_dir.absolutePath()),
				     root.relativeFilePath(game_dir.absolutePath()), game_dir.dirName(), false});
				continue;
			}
			for (const auto& sub:
			     game_dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
				pending.append(QDir(sub.absoluteFilePath()));
			}
		}
	}
	return out;
}

QString CatalogId(const QString& game_path) {
	return QString::fromLatin1(
	    QCryptographicHash::hash(game_path.toUtf8(), QCryptographicHash::Sha256).toHex());
}

} // namespace GameLibraryFiles

GameLibrary::~GameLibrary() {
	qDeleteAll(m_custom);
}

bool GameLibrary::Load(const CompatibilityDatabase* compatibility, QString* error) {
	auto s          = OpenSettings();
	m_settings_file = s->fileName();
	s->beginGroup(CONF_LAUNCHER);
	m_game_dirs = NormalizeGameDirectories(SettingsStringList(s->value(CONF_GAME_DIRS)));
	if (m_game_dirs.isEmpty()) {
		m_game_dirs = NormalizeGameDirectories(SettingsStringList(s->value(CONF_GAME_DIR)));
	}
	m_addons_dir = NormalizeGameDirectory(s->value(CONF_ADDONS_DIR).toString());
	s->endGroup();

	s->beginGroup(CONF_GLOBAL);
	if (!s->childKeys().isEmpty()) {
		m_global.ReadSettings(s.get());
	}
	m_global.controller.ReadSettings(s.get());
	m_global.ReadGlobalOnlySettings(s.get());
	s->endGroup();

	qDeleteAll(m_custom);
	m_custom.clear();
	const int size = s->beginReadArray(CONF_SECTION_NAME);
	for (int i = 0; i < size; i++) {
		s->setArrayIndex(i);
		auto* info = new Configuration;
		info->ReadSettings(s.get());
		info->custom_settings = true;
		if (info->game_path.isEmpty()) {
			delete info;
			continue;
		}
		m_custom.insert(info->game_path, info);
	}
	s->endArray();
	if (s->status() != QSettings::NoError) {
		if (error != nullptr) {
			*error = QStringLiteral("Cannot read %1").arg(m_settings_file);
		}
		return false;
	}

	m_games.clear();
	for (const auto& c: FindCandidates(m_game_dirs)) {
		// Keep the archive index alive through validation and metadata reads.
		const auto reader = c.archive ? Common::OpenArchive(GameContent::ToPath(c.base)) : nullptr;
		if (c.archive &&
		    (reader == nullptr || !GameContent::FileExists(c.base, QStringLiteral("eboot.bin")))) {
			continue;
		}
		const auto metadata =
		    GetGameMetadata(GameContent::ReadFile(c.base, QStringLiteral("sce_sys/param.json"),
		                                          GameContent::MaxMetadataSize),
		                    c.fallback);
		auto game = std::make_shared<Configuration>();
		SetGameFiles(*game, c.base, c.game_path, metadata, c.archive);
		const auto* custom = FindCustomInfo(&m_custom, c.game_path, c.legacy_game_path);
		// Same merge order as ConfigurationListWidget::CreateConfiguration.
		game->CopyEmulatorSettingsFrom(custom != nullptr ? *custom : m_global);
		game->CopyGlobalOnlySettingsFrom(m_global);
		if (custom != nullptr && !custom->elf.isEmpty()) {
			game->elf = custom->elf;
		}
		game->host_input_mapping = m_global.host_input_mapping;
		game->addons_dir         = m_addons_dir;
		game->custom_settings    = custom != nullptr;
		if (compatibility != nullptr) {
			if (const auto* entry = compatibility->Find(game->title_id); entry != nullptr) {
				game->game_status  = entry->status;
				game->game_comment = entry->comment;
			}
		}
		m_games.append({CatalogId(game->game_path), game, custom != nullptr});
	}
	std::sort(m_games.begin(), m_games.end(), [](const GameEntry& a, const GameEntry& b) {
		return QString::localeAwareCompare(a.info->name, b.info->name) < 0;
	});
	return true;
}

const GameEntry* GameLibrary::Find(const QString& id) const {
	for (const auto& g: m_games) {
		if (g.id == id) {
			return &g;
		}
	}
	return nullptr;
}

QList<StreamingLaunchEntry> GameLibrary::Catalog(const QString& interpreter) const {
	QList<StreamingLaunchEntry> out;
	for (const auto& g: m_games) {
		auto arguments = BuildEmulatorArgs(*g.info);
		if (arguments.isEmpty()) {
			continue;
		}
		const auto icon = GameContent::IsArchive(g.info->basedir)
		                      ? QString()
		                      : QDir(g.info->basedir).filePath("sce_sys/icon0.png");
		out.append({g.id, g.info->name, interpreter, arguments,
		            QFileInfo(interpreter).absolutePath(), QFile::exists(icon) ? icon : QString()});
	}
	return out;
}

QHash<QString, QList<StreamingLaunchEntry>>
GameLibrary::ClientCatalog(const QString& interpreter) const {
	QHash<QString, QList<StreamingLaunchEntry>> out;
	const QDir clients(QDir(StreamingSettings::ProfileDirectory()).filePath("clients"));
	if (!clients.exists()) {
		return out;
	}
	for (const auto& uuid: clients.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
		if (!ClientProfiles::IsValidClientId(uuid)) {
			continue; // ignore anything that is not a validated device directory
		}
		QList<StreamingLaunchEntry> entries;
		for (const auto& g: m_games) {
			bool       seeded  = false;
			const auto profile = ClientProfiles::ReadGame(uuid, g.id, &seeded);
			if (!seeded) {
				continue; // no device profile yet: launch uses the PC catalog entry
			}
			const auto config = HostApi::ClientConfig(
			    *g.info, profile.value(QStringLiteral("settings")).toObject());
			if (config == nullptr) {
				continue;
			}
			// Use the per-client cheat plan when it exists, and no --game-patch otherwise.
			const auto                   plan_path = ClientProfiles::PatchPlanPath(uuid, g.id);
			const std::optional<QString> patch_plan =
			    !plan_path.isEmpty() && QFileInfo::exists(plan_path)
			        ? std::optional<QString>(plan_path)
			        : std::optional<QString>(QString {});
			auto arguments = BuildEmulatorArgs(*config, patch_plan);
			if (arguments.isEmpty()) {
				continue;
			}
			const auto icon = GameContent::IsArchive(g.info->basedir)
			                      ? QString()
			                      : QDir(g.info->basedir).filePath("sce_sys/icon0.png");
			entries.append({g.id, g.info->name, interpreter, arguments,
			                QFileInfo(interpreter).absolutePath(),
			                QFile::exists(icon) ? icon : QString()});
		}
		if (!entries.isEmpty()) {
			out.insert(uuid, entries);
		}
	}
	return out;
}

std::optional<StreamingLaunchEntry> GameLibrary::ResolveLaunch(
    const QString& interpreter, const QString& client_id, const QString& game_id) const {
	if (!ClientProfiles::IsValidClientId(client_id) || Find(game_id) == nullptr) return std::nullopt;
	const auto clients = ClientCatalog(interpreter);
	for (const auto& entry: clients.value(client_id)) {
		if (entry.id == game_id) return entry;
	}
	for (const auto& entry: Catalog(interpreter)) {
		if (entry.id == game_id) return entry;
	}
	return std::nullopt;
}

QString GameLibrary::SaveGameSettings(const QString& id, const QJsonObject& settings) {
	const auto* game = Find(id);
	if (game == nullptr) {
		return QStringLiteral("unknown-game");
	}
	// SetGameSettings starts from the candidate's current values, so seed it with the game's.
	auto    candidate = std::make_unique<Configuration>();
	QString error;
	candidate->CopyGameInfoFrom(*game->info);
	candidate->CopyEmulatorSettingsFrom(*game->info);
	candidate->elf = game->info->elf;
	if (!candidate->SetGameSettings(settings, error)) {
		return error.isEmpty() ? QStringLiteral("invalid") : error;
	}
	candidate->custom_settings = true;

	auto* stored = m_custom.value(game->info->game_path);
	if (stored == nullptr) {
		stored = new Configuration;
		m_custom.insert(game->info->game_path, stored);
	}
	stored->CopyGameInfoFrom(*candidate);
	stored->CopyEmulatorSettingsFrom(*candidate);
	stored->elf             = candidate->elf;
	stored->custom_settings = true;

	auto s = OpenSettings();
	s->remove(CONF_SECTION_NAME);
	s->beginWriteArray(CONF_SECTION_NAME);
	int i = 0;
	for (auto it = m_custom.constBegin(); it != m_custom.constEnd(); ++it) {
		s->setArrayIndex(i++);
		it.value()->WriteSettings(s.get());
	}
	s->endArray();
	s->sync();
	if (s->status() != QSettings::NoError) {
		return QStringLiteral("write-failed");
	}
	// m_games entries hold shared_ptrs, so the live configuration is updated in place.
	game->info->CopyEmulatorSettingsFrom(*candidate);
	game->info->elf                      = candidate->elf;
	game->info->custom_settings          = true;
	const_cast<GameEntry*>(game)->custom = true;
	return {};
}
