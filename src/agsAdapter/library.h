#ifndef AGS_ADAPTER_LIBRARY_H
#define AGS_ADAPTER_LIBRARY_H

// library.list (STANDARD.md section 5.4): one GameEntry per game of GameLibrary, with cover and
// icon PNG files of at most 2 MiB.
#include <QJsonArray>
#include <QString>

class GameLibrary;
struct GameEntry;

namespace AdapterLibrary {

inline constexpr qint64 MaxArt = qint64 {2} << 20u;

// kind "icon" or "cover". The game's own PNG when it is a regular file in a game folder of at
// most 2 MiB, else a re-encoded PNG written once to <data>/art/<id>/<kind>.png. Empty when the
// game has no such art.
[[nodiscard]] QString ArtPath(const GameEntry& game, const QString& kind, const QString& data_dir);

[[nodiscard]] QJsonArray Games(const GameLibrary& library, const QString& data_dir);

} // namespace AdapterLibrary

#endif
