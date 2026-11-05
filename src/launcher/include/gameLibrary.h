#ifndef LAUNCHER_INCLUDE_GAME_LIBRARY_H_
#define LAUNCHER_INCLUDE_GAME_LIBRARY_H_

#include "configuration.h"

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <memory>

class CompatibilityDatabase;

struct GameMetadata {
	QString title_name;
	QString title_id;
	QString gameVersion;
	QString firmwareVer;
};

struct GameCandidate {
	QString base;
	QString game_path;
	QString legacy_game_path;
	QString fallback;
	bool    archive = false;
};

struct GameEntry {
	QString                        id;
	std::shared_ptr<Configuration> info;
	bool                           custom = false;
};

namespace GameLibraryFiles {

inline constexpr char CONF_SECTION_NAME[] = "GameConfigurations";
inline constexpr char CONF_LAUNCHER[]     = "Launcher";
inline constexpr char CONF_GAME_DIR[]     = "game_dir";
inline constexpr char CONF_GAME_DIRS[]    = "game_dirs";
inline constexpr char CONF_ADDONS_DIR[]   = "addons_dir";
inline constexpr char CONF_GLOBAL[]       = "GlobalConfiguration";

// Standalone lookup: ./Kyty.ini in the working directory, else the user config.
[[nodiscard]] std::unique_ptr<QSettings> OpenSettings();
// An explicit settings file (INI format); an empty path falls back to the standalone lookup.
[[nodiscard]] std::unique_ptr<QSettings> OpenSettings(const QString& ini_path);
[[nodiscard]] QString                    NormalizeGameDirectory(const QString& dir);
[[nodiscard]] QStringList                NormalizeGameDirectories(const QStringList& dirs);
[[nodiscard]] QStringList                SettingsStringList(const QVariant& value);
[[nodiscard]] QString                    PathKey(const QString& path);
[[nodiscard]] GameMetadata GetGameMetadata(const QByteArray& param_data, const QString& fallback);
void           SetGameFiles(Configuration& info, const QString& game_dir, const QString& game_path,
                            const GameMetadata& metadata, bool archive);
Configuration* FindCustomInfo(QMap<QString, Configuration*>* custom_infos, const QString& game_path,
                              const QString& legacy_game_path);
[[nodiscard]] QList<GameCandidate> FindCandidates(const QStringList& game_dirs);
[[nodiscard]] QString              CatalogId(const QString& game_path); // hex SHA-256 of UTF-8 path

} // namespace GameLibraryFiles

class GameLibrary {
public:
	GameLibrary() = default;
	~GameLibrary();
	GameLibrary(const GameLibrary&)            = delete;
	GameLibrary& operator=(const GameLibrary&) = delete;

	[[nodiscard]] bool Load(const CompatibilityDatabase* compatibility, QString* error);
	[[nodiscard]] const QList<GameEntry>&     Games() const { return m_games; }
	[[nodiscard]] const GameEntry*            Find(const QString& id) const;
	// Validates through Configuration::SetGameSettings, stores as a custom entry, rewrites
	// Kyty.ini.
	[[nodiscard]] QString SaveGameSettings(const QString& id, const QJsonObject& settings);
	[[nodiscard]] QString SettingsFile() const { return m_settings_file; }
	// Explicit Kyty.ini to read and write; empty (the default) keeps the standalone lookup.
	void SetSettingsPath(const QString& ini_path) { m_settings_path = ini_path; }

private:
	Configuration                 m_global;
	QMap<QString, Configuration*> m_custom; // owned
	QStringList                   m_game_dirs;
	QString                       m_addons_dir;
	QString                       m_settings_file;
	QString                       m_settings_path;
	QList<GameEntry>              m_games;
};

#endif /* LAUNCHER_INCLUDE_GAME_LIBRARY_H_ */
