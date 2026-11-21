#include "actions.h"

#include "cheatFile.h"
#include "common/additionalContent.h"
#include "configuration.h"
#include "gameContent.h"
#include "gameLibrary.h"
#include "gameServices.h"
#include "library.h"
#include "trophyItems.h"

#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTimer>

#include <algorithm>

namespace {

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

// A row label or value, one line within `max_chars`.
QString RowText(const QString& text, int max_chars) {
	return AdapterLibrary::CutChars(AdapterLibrary::DropControls(text), max_chars);
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
	if (Cheats::PlanPath(cheats_root, AdapterActions::CheatTitleId(*game.info)).isEmpty()) {
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
		m_repository.Load(AdapterActions::CheatTitleId(*game.info));
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

QString CheatTitleId(const Configuration& info) {
	return info.title_id.trimmed().toUpper();
}

std::optional<Cheats::RemoteFile> PickCheatFile(const QList<Cheats::RemoteFile>& supported,
                                                const QString& version, QString* error) {
	const auto pick = std::find_if(supported.begin(), supported.end(),
	                               [&](const auto& f) { return f.version == version; });
	if (pick != supported.end()) return *pick;
	*error = QStringLiteral("No cheat file for version %1.")
	             .arg(version.isEmpty() ? QStringLiteral("unknown") : version);
	return std::nullopt;
}

bool HasCheatPlan(const GameEntry& game, const QString& runtime_root) {
	const auto plan = GameServices::CheatPlanPath(*game.info, runtime_root);
	return !plan.isEmpty() && QFileInfo::exists(plan);
}

QJsonObject InfoSection(const GameEntry& game) {
	const auto& info = *game.info;
	QJsonArray  rows;
	const auto  add = [&](const char* label, const QString& value) {
		if (const auto text = RowText(value, 200); !text.trimmed().isEmpty())
			rows.append(QJsonObject {{"label", label}, {"value", text}});
	};
	add("Title ID", info.title_id);
	add("Version", info.gameVersion);
	add("Required firmware", info.firmwareVer);
	// No "Location" row: a host path must not reach a paired device (sheet 3.6).
	if (rows.isEmpty()) return {};
	return {
	    {"id", "kyty.info"},
	    {"surface", "panel"},
	    {"tab", "advanced"},
	    {"title", "Game info"},
	    {"icon", "assets/ags/actions/info.svg"},
	    {"items", QJsonArray {QJsonObject {
	                  {"id", "info"}, {"kind", "rows"}, {"label", "Game info"}, {"rows", rows}}}}};
}

QJsonObject ContentSection(const GameEntry& game) {
	static const QRegularExpression entry_id(QStringLiteral("^[A-Za-z0-9._-]{1,128}$"));
	const auto&                     info     = *game.info;
	const auto                      title_id = info.title_id.trimmed().toUpper();
	const auto                      folder   = info.addons_dir;
	QJsonArray                      rows;
	QSet<QString>                   ids;
	for (const auto& entry:
	     Common::FindAddContent(GameContent::ToPath(folder), title_id.toStdString())) {
		if (rows.size() >= 256) break;
		const auto id    = QString::fromStdString(entry.content_id);
		auto       label = RowText(
		    QString::fromStdString(entry.title.empty() ? entry.entitlement_label : entry.title),
		    80);
		if (label.trimmed().isEmpty()) label = RowText(id, 80);
		if (label.trimmed().isEmpty()) continue; // nothing to show; a blank label drops the item
		QJsonObject row {{"label", label},
		                 {"value", RowText(QString::fromStdString(entry.entitlement_label), 200)}};
		if (entry_id.match(id).hasMatch() && !ids.contains(id)) {
			row[QStringLiteral("id")] = id;
			ids.insert(id);
		}
		rows.append(row);
	}
	QJsonObject item {{"id", "content"},
	                  {"kind", "rows"},
	                  {"label", "Additional content"},
	                  {"alias", "content"},
	                  {"rows", rows}};
	if (!folder.isEmpty() && QDir(folder).exists()) item[QStringLiteral("folder")] = folder;
	if (rows.isEmpty() && !item.contains(QStringLiteral("folder"))) return {};
	return {{"id", "kyty.content"},
	        {"surface", "panel"},
	        {"tab", "about"},
	        {"title", "Additional content"},
	        {"icon", "assets/ags/actions/content.svg"},
	        {"items", QJsonArray {item}}};
}

QJsonObject TrophiesSection(const GameEntry& game, const QString& runtime_root,
                            const QString& data_dir) {
	const auto&                     info = *game.info;
	static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
	const auto folder = safe.match(info.title_id).hasMatch() ? info.title_id : game.id;
	return AdapterTrophies::Section(
	    info, runtime_root, QDir(data_dir).filePath(QStringLiteral("trophies/%1").arg(folder)));
}

QJsonObject VariantsSection(const GameEntry& game, const QString& runtime_root) {
	if (!HasCheatPlan(game, runtime_root)) return {};
	return {{"id", "kyty.variants"},
	        {"surface", "menu"},
	        {"items", QJsonArray {QJsonObject {{"id", LAUNCH_WITHOUT_CHEATS},
	                                           {"kind", "button"},
	                                           {"label", "Launch without cheats"},
	                                           {"icon", "assets/ags/actions/launch-nocheats.svg"},
	                                           {"launch", true}}}}};
}

Reply List(const GameEntry* game, const QString& /*runtime_root*/) {
	if (game == nullptr) return Reply::Success({{"actions", QJsonArray {}}});
	return Reply::Success({{"actions", QJsonArray {
	                                       QJsonObject {{"id", "cheats"},
										                {"kind", "toggle_list"},
										                {"label", "Cheats"},
										                {"icon", "assets/ags/actions/cheats.svg"}},
	                                       QJsonObject {{"id", "cheats.fetch"},
										                {"kind", "command"},
										                {"label", "Fetch cheats"},
										                {"confirm", true},
										                {"menu", true},
										                {"icon", "assets/ags/actions/fetch.svg"}},
	                                   }}});
}

Reply Run(const GameEntry& game, const QJsonObject& params, const QString& runtime_root,
          const QString& /*data_dir*/, CheatFetcher& fetcher) {
	const auto action = params[QStringLiteral("action_id")].toString();
	if (action == QLatin1String("cheats")) return CheatList(game, params, runtime_root);
	if (action == QLatin1String("cheats.fetch")) return fetcher.Run(game, runtime_root, 3500);
	return Reply::NotFound(QStringLiteral("Unknown action %1.").arg(action));
}

} // namespace AdapterActions
