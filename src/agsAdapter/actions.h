#ifndef AGS_ADAPTER_ACTIONS_H
#define AGS_ADAPTER_ACTIONS_H

// The game page's cheats, info, content, trophies and launch variant sections; the cheats values
// and the Fetch cheats button (action.run).
#include "cheatRepository.h"
#include "fetchResult.h"
#include "reply.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <memory>
#include <optional>

class Configuration;
struct GameEntry;

class CheatFetcher final: public QObject {
	Q_OBJECT

public:
	explicit CheatFetcher(QObject* parent = nullptr);

	// Starts (or continues) fetching the cheat file for `game` and waits up to `wait_ms` for the
	// result. A download still running answers failed and finishes in the background; the next
	// call for the game within 60 s of its completion returns that result, a later call fetches
	// again.
	[[nodiscard]] Reply Run(const GameEntry& game, const QString& cheats_root, int wait_ms);

signals:
	// After a successful import, also one that finished after its call gave up.
	void Imported(const QString& game);

private:
	enum class Phase { Idle, Catalog, File };
	void Finish(const QString& error, const QString& message);

	Cheats::CheatRepository        m_repository;
	Phase                          m_phase = Phase::Idle;
	QString                        m_game;
	std::shared_ptr<Configuration> m_info;
	QString                        m_cheats_root;
	QString                        m_message;
	FetchResultCache               m_results;
	QElapsedTimer                  m_clock;
};

namespace AdapterActions {

// The title id the cheat plan is keyed by: trimmed, upper case. Library, actions and launch.plan share it.
[[nodiscard]] QString CheatTitleId(const Configuration& info);

// The remote cheat file for exactly `version`. Empty with `error` set ("No cheat file for version
// <v>.") when none matches; a file for another version would be refused on import.
[[nodiscard]] std::optional<Cheats::RemoteFile>
PickCheatFile(const QList<Cheats::RemoteFile>& supported, const QString& version, QString* error);

// `launch` action id: launch.plan leaves out --game-patch (the cheat plan).
inline constexpr const char* LAUNCH_WITHOUT_CHEATS = "launch.nocheats";

// Game page sections (sheet 1.2). Each returns an empty object when the section is absent.
[[nodiscard]] QJsonObject InfoSection(const GameEntry& game);
[[nodiscard]] QJsonObject ContentSection(const GameEntry& game);
// Icons are written to <data_dir>/trophies/<title id or game id>.
[[nodiscard]] QJsonObject TrophiesSection(const GameEntry& game, const QString& runtime_root,
                                          const QString& data_dir);
// Only with a cheat plan: without one "Launch without cheats" would be the plain launch.
[[nodiscard]] QJsonObject VariantsSection(const GameEntry& game, const QString& runtime_root);

// kyty.cheats; empty when GameServices::GetCheats says unsupported.
[[nodiscard]] QJsonObject CheatsSection(const GameEntry& game, const QString& runtime_root);
// The ids of the entries that are on (the item's game-scope value).
[[nodiscard]] QJsonArray CheatsOn(const GameEntry& game, const QString& runtime_root);
// SetCheats' checks alone: the reply it would refuse with, success when it would write.
[[nodiscard]] Reply CheckCheats(const GameEntry& game, const QString& runtime_root,
                                const QJsonValue& ids);
// Turns exactly `ids` on (null: none). invalid-params, writing nothing, for a non-array, a
// non-string, a repeated id or an id that is not an entry now; failed when PutCheats fails.
[[nodiscard]] Reply SetCheats(const GameEntry& game, const QString& runtime_root,
                              const QJsonValue& ids);

} // namespace AdapterActions

#endif
