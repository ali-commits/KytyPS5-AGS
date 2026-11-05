#ifndef LAUNCHER_INCLUDE_GAME_SERVICES_H_
#define LAUNCHER_INCLUDE_GAME_SERVICES_H_

#include "cheatRepository.h"
#include "configuration.h"

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

// Per-game helpers with no sockets and no daemon state: safe runtime checks, trophies, cheats and
// art. Errors are short kebab-case strings (empty means success).
namespace GameServices {

constexpr int MAX_BACKGROUND_WIDTH = 1920;

struct Image {
	QByteArray data; // empty when the game has no such art
	QByteArray type; // image/png or image/jpeg
};

struct TrophyIconResult {
	QByteArray png;
	QString    error; // "hidden" or "no-icon" when png is empty
};

// An absolute path without "..", where every component is a real directory (no symlinks).
[[nodiscard]] bool SafeDirectory(const QString& path);
// SafeDirectory plus: the well-known state folders under it contain no symlinks.
[[nodiscard]] bool SafeRuntime(const QString& path);

// {"trophies":[...],"unlocked":n,"total":n}; unlock state is read from runtime_directory.
[[nodiscard]] QJsonObject Trophies(const Configuration& info, const QString& runtime_directory);
[[nodiscard]] TrophyIconResult TrophyIcon(const Configuration& info,
                                          const QString& runtime_directory, int label, int id);

// {"supported","revision","mods":[{index,name,enabled}],"error"?}
[[nodiscard]] QJsonObject GetCheats(const Configuration& info);
// body: {"revision":"...","enabled":[bool,...]}; replaces the whole selection.
[[nodiscard]] QString PutCheats(const Configuration& info, const QJsonObject& body,
                                QString* revision);
// {"files":[{name,title,version,format,installed_version}],"error"?} from a loaded catalog.
// Returns {"files":[],"error":"unsupported-title-id"} without inspecting files when the game
// has no cheat plan; the caller should still keep its own supported-only filtered list (the one
// it cached before calling) for the matching fetch, since this function only formats a reply.
[[nodiscard]] QJsonObject RemoteCheatList(const Configuration&                  info,
                                          const QList<Cheats::RemoteFile>&      files,
                                          const QString&                        error);
// Validates a downloaded cheat file against the game and stores it with every mod disabled.
[[nodiscard]] QString ImportRemoteCheat(const Configuration& info, const QByteArray& data,
                                        QString* revision);

// kind: "icon" (sce_sys/icon0.png as is) or "background" (pic0/pic1, JPEG at most 1920 wide).
[[nodiscard]] Image GameArt(const Configuration& info, const QString& kind);

} // namespace GameServices

#endif /* LAUNCHER_INCLUDE_GAME_SERVICES_H_ */
