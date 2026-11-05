#ifndef AGS_ADAPTER_LIBRARY_H
#define AGS_ADAPTER_LIBRARY_H

// library.list (STANDARD.md section 5.4): one GameEntry per game of GameLibrary, with cover and
// icon PNG files of at most 2 MiB.
#include <QJsonArray>
#include <QSet>
#include <QString>

class GameLibrary;
struct GameEntry;

namespace AdapterLibrary {

inline constexpr qint64 MaxArt = qint64 {2} << 20u;

// Games whose art could not be produced, for the lifetime of the adapter process, so a library
// scan does not read and decode an archive again for every call.
class ArtCache {
public:
	[[nodiscard]] bool Missing(const QString& id, const QString& kind) const {
		return m_missing.contains(id + QLatin1Char('/') + kind);
	}
	void SetMissing(const QString& id, const QString& kind) {
		m_missing.insert(id + QLatin1Char('/') + kind);
	}

private:
	QSet<QString> m_missing;
};

// kind "icon" or "cover". The game's own PNG when it is a regular file in a game folder of at
// most 2 MiB, else a re-encoded PNG at <data>/art/<id>/<kind>.png, written again when the source
// file is newer. Empty when the game has no such art.
[[nodiscard]] QString ArtPath(const GameEntry& game, const QString& kind, const QString& data_dir,
                              ArtCache& cache);

[[nodiscard]] QJsonArray Games(const GameLibrary& library, const QString& data_dir,
                               ArtCache& cache);

// `text` without control characters (Unicode category Cc), as GameEntry::validate requires.
[[nodiscard]] QString DropControls(const QString& text);
// `text` cut to at most `max_chars` Unicode characters, never inside a surrogate pair.
[[nodiscard]] QString CutChars(const QString& text, int max_chars);

} // namespace AdapterLibrary

#endif
