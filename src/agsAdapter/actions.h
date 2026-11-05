#ifndef AGS_ADAPTER_ACTIONS_H
#define AGS_ADAPTER_ACTIONS_H

// actions.list and actions.run (STANDARD.md section 5.4): cheats (toggle_list), cheats.fetch
// (command), trophies (progress_list) and info.
#include "cheatRepository.h"
#include "fetchResult.h"
#include "reply.h"

#include <QElapsedTimer>
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

// The remote cheat file for exactly `version`. Empty with `error` set ("No cheat file for version
// <v>.") when none matches; a file for another version would be refused on import.
[[nodiscard]] std::optional<Cheats::RemoteFile>
PickCheatFile(const QList<Cheats::RemoteFile>& supported, const QString& version, QString* error);

[[nodiscard]] Reply List(const GameEntry* game);
[[nodiscard]] Reply Run(const GameEntry& game, const QJsonObject& params,
                        const QString& runtime_root, const QString& data_dir,
                        CheatFetcher& fetcher);

} // namespace AdapterActions

#endif
