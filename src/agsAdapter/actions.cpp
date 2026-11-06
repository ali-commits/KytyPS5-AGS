#include "actions.h"

#include "cheatFile.h"
#include "common/additionalContent.h"
#include "configuration.h"
#include "gameContent.h"
#include "gameLibrary.h"
#include "gameServices.h"
#include "trophyItems.h"

#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>

#include <algorithm>

namespace {

QString CheatTitle(const Configuration& info) {
	return info.title_id.trimmed().toUpper();
}

QJsonArray CheatItems(const QJsonObject& cheats) {
	QJsonArray items;
	for (const auto& v: cheats[QStringLiteral("mods")].toArray()) {
		const auto mod = v.toObject();
		items.append(QJsonObject {{"id", QString::number(mod[QStringLiteral("index")].toInt())},
		                          {"label", mod[QStringLiteral("name")].toString()},
		                          {"on", mod[QStringLiteral("enabled")].toBool()}});
	}
	return items;
}

Reply CheatList(const GameEntry& game, const QJsonObject& params, const QString& root) {
	const auto& info   = *game.info;
	auto        cheats = GameServices::GetCheats(info, root);
	if (!cheats[QStringLiteral("supported")].toBool()) {
		return Reply::Success(
		    {{"items", QJsonArray {}}, {"message", "Cheats are not available for this game."}});
	}
	if (cheats.contains(QStringLiteral("error"))) {
		return Reply::Failed(cheats[QStringLiteral("error")].toString());
	}
	if (params.contains(QStringLiteral("item_id"))) {
		bool       ok    = false;
		const auto index = params[QStringLiteral("item_id")].toString().toInt(&ok);
		const auto mods  = cheats[QStringLiteral("mods")].toArray();
		if (!params[QStringLiteral("on")].isBool()) {
			return Reply::Invalid(QStringLiteral("on must be a boolean."));
		}
		if (!ok || index < 0 || index >= mods.size()) {
			return Reply::NotFound(QStringLiteral("No such cheat."));
		}
		QJsonArray enabled;
		for (qsizetype i = 0; i < mods.size(); ++i) {
			enabled.append(i == index ? params[QStringLiteral("on")].toBool()
			                          : mods[i].toObject()[QStringLiteral("enabled")].toBool());
		}
		const QJsonObject body {{"revision", cheats[QStringLiteral("revision")]},
		                        {"enabled", enabled}};
		if (const auto error = GameServices::PutCheats(info, root, body, nullptr);
		    !error.isEmpty()) {
			return Reply::Failed(
			    QStringLiteral("Cannot save the cheat selection (%1).").arg(error));
		}
		cheats = GameServices::GetCheats(info, root);
	}
	QJsonObject result {{"items", CheatItems(cheats)}};
	if (cheats[QStringLiteral("mods")].toArray().isEmpty()) {
		result[QStringLiteral("message")] = "No cheats are installed. Use Fetch cheats first.";
	}
	return Reply::Success(result);
}

Reply Trophies(const GameEntry& game, const QString& runtime_root, const QString& data_dir) {
	const auto&                     info = *game.info;
	static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
	const auto folder = safe.match(info.title_id).hasMatch() ? info.title_id : game.id;
	return AdapterTrophies::Run(info, runtime_root,
	                            QDir(data_dir).filePath(QStringLiteral("trophies/%1").arg(folder)));
}

Reply Content(const GameEntry& game) {
	const auto&  info = *game.info;
	QJsonArray   items;
	const auto   title_id = info.title_id.trimmed().toUpper();
	const auto   folder   = info.addons_dir;
	for (const auto& entry: Common::FindAddContent(GameContent::ToPath(folder), title_id.toStdString())) {
		QJsonObject item {{"id", QString::fromStdString(entry.content_id)},
		                  {"title", QString::fromStdString(entry.title.empty() ? entry.entitlement_label
		                                                                       : entry.title)}};
		if (!entry.entitlement_label.empty())
			item[QStringLiteral("label")] = QString::fromStdString(entry.entitlement_label);
		items.append(item);
	}
	QJsonObject result {{"items", items}};
	if (!folder.isEmpty() && QDir(folder).exists()) result[QStringLiteral("folder")] = folder;
	return Reply::Success(result);
}

Reply Info(const GameEntry& game) {
	const auto& info = *game.info;
	QJsonArray  items;
	const auto  add = [&](const char* label, const QString& value) {
		if (!value.trimmed().isEmpty())
			items.append(QJsonObject {{"label", label}, {"value", value}});
	};
	add("Title ID", info.title_id);
	add("Version", info.gameVersion);
	add("Required firmware", info.firmwareVer);
	add("Location", info.basedir);
	return Reply::Success({{"items", items}});
}

} // namespace

CheatFetcher::CheatFetcher(QObject* parent): QObject(parent) {
	m_clock.start();
	connect(
	    &m_repository, &Cheats::CheatRepository::CatalogLoaded, this,
	    [this](const QList<Cheats::RemoteFile>& files, const QString& error) {
		    if (m_phase != Phase::Catalog) return;
		    QList<Cheats::RemoteFile> supported;
		    for (const auto& f: files) {
			    if (f.IsSupported()) supported.append(f);
		    }
		    if (supported.isEmpty()) {
			    Finish(error.isEmpty() ? QStringLiteral("No cheat file is available for this game.")
				                       : error,
				       {});
			    return;
		    }
		    // Only the file for the installed version; nothing is downloaded otherwise.
		    QString    pick_error;
		    const auto pick =
		        AdapterActions::PickCheatFile(supported, m_info->gameVersion, &pick_error);
		    if (!pick) {
			    Finish(pick_error, {});
			    return;
		    }
		    m_message = QStringLiteral("Installed cheats %1 (%2).").arg(pick->version, pick->name);
		    m_phase   = Phase::File;
		    m_repository.Fetch(*pick);
	    });
	connect(&m_repository, &Cheats::CheatRepository::FileLoaded, this,
	        [this](const QByteArray& data, const QString& error) {
		        if (m_phase != Phase::File) return;
		        if (!error.isEmpty()) {
			        Finish(error, {});
			        return;
		        }
		        const auto import_error =
		            GameServices::ImportRemoteCheat(*m_info, m_cheats_root, data, nullptr);
		        Finish(import_error, m_message);
	        });
}

void CheatFetcher::Finish(const QString& error, const QString& message) {
	m_phase = Phase::Idle;
	m_results.Store({m_game, error, message}, m_clock.elapsed());
}

Reply CheatFetcher::Run(const GameEntry& game, const QString& cheats_root, int wait_ms) {
	if (Cheats::PlanPath(cheats_root, CheatTitle(*game.info)).isEmpty()) {
		return Reply::Failed(QStringLiteral("Cheats are not available for this game."));
	}
	if (m_phase != Phase::Idle && m_game != game.id) {
		return Reply::Failed(
		    QStringLiteral("Cheats for another game are downloading; try again in a moment."));
	}
	const auto answer = [](const FetchOutcome& outcome) {
		return outcome.error.isEmpty() ? Reply::Success({{"message", outcome.message}})
		                               : Reply::Failed(outcome.error);
	};
	if (m_phase == Phase::Idle) {
		// A result that finished after an earlier call gave up, if still fresh; older ones are
		// discarded here.
		if (const auto done = m_results.Take(game.id, m_clock.elapsed())) return answer(*done);
		m_game        = game.id;
		m_info        = game.info;
		m_cheats_root = cheats_root;
		m_message.clear();
		m_phase = Phase::Catalog;
		m_repository.Load(CheatTitle(*game.info));
	}
	if (m_phase != Phase::Idle) {
		QEventLoop loop;
		QTimer     timer;
		timer.setSingleShot(true);
		connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
		QTimer poll;
		connect(&poll, &QTimer::timeout, &loop, [&] {
			if (m_phase == Phase::Idle) loop.quit();
		});
		timer.start(wait_ms);
		poll.start(25);
		if (m_phase != Phase::Idle) loop.exec();
	}
	if (m_phase != Phase::Idle) {
		return Reply::Failed(
		    QStringLiteral("Cheats are still downloading. Run Fetch cheats again in a moment."));
	}
	const auto done = m_results.Take(game.id, m_clock.elapsed());
	return done ? answer(*done) : Reply::Failed(QStringLiteral("The cheat download was lost."));
}

namespace AdapterActions {

std::optional<Cheats::RemoteFile> PickCheatFile(const QList<Cheats::RemoteFile>& supported,
                                                const QString& version, QString* error) {
	const auto pick = std::find_if(supported.begin(), supported.end(),
	                               [&](const auto& f) { return f.version == version; });
	if (pick != supported.end()) return *pick;
	*error = QStringLiteral("No cheat file for version %1.")
	             .arg(version.isEmpty() ? QStringLiteral("unknown") : version);
	return std::nullopt;
}

Reply List(const GameEntry* game) {
	if (game == nullptr) return Reply::Success({{"actions", QJsonArray {}}});
	return Reply::Success(
	    {{"actions",
		  QJsonArray {
	          QJsonObject {{"id", "cheats"}, {"kind", "toggle_list"}, {"label", "Cheats"}},
	          QJsonObject {{"id", "cheats.fetch"},
			               {"kind", "command"},
			               {"label", "Fetch cheats"},
			               {"confirm", true}},
	          QJsonObject {{"id", "trophies"}, {"kind", "progress_list"}, {"label", "Trophies"}},
	          QJsonObject {{"id", "info"}, {"kind", "info"}, {"label", "Game info"}},
	          QJsonObject {{"id", "content"}, {"kind", "list"}, {"label", "Additional content"}},
	      }}});
}

Reply Run(const GameEntry& game, const QJsonObject& params, const QString& runtime_root,
          const QString& data_dir, CheatFetcher& fetcher) {
	const auto action = params[QStringLiteral("action_id")].toString();
	if (action == QLatin1String("cheats")) return CheatList(game, params, runtime_root);
	if (action == QLatin1String("cheats.fetch")) return fetcher.Run(game, runtime_root, 3500);
	if (action == QLatin1String("trophies")) return Trophies(game, runtime_root, data_dir);
	if (action == QLatin1String("info")) return Info(game);
	if (action == QLatin1String("content")) return Content(game);
	return Reply::NotFound(QStringLiteral("Unknown action %1.").arg(action));
}

} // namespace AdapterActions
