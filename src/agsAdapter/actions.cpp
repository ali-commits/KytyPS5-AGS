#include "actions.h"

#include "cheatFile.h"
#include "configuration.h"
#include "gameLibrary.h"
#include "gameServices.h"

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

// Version strings such as "01.020" compared part by part as numbers.
bool VersionLess(const QString& a, const QString& b) {
	const auto pa = a.split(QLatin1Char('.'));
	const auto pb = b.split(QLatin1Char('.'));
	for (qsizetype i = 0; i < std::max(pa.size(), pb.size()); ++i) {
		const auto x = i < pa.size() ? pa[i].toLongLong() : 0;
		const auto y = i < pb.size() ? pb[i].toLongLong() : 0;
		if (x != y) return x < y;
	}
	return false;
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

QString GradeName(const QString& grade) {
	return grade.isEmpty() ? grade : grade.left(1).toUpper() + grade.mid(1);
}

Reply Trophies(const GameEntry& game, const QString& runtime_root, const QString& data_dir) {
	const auto&                     info = *game.info;
	static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
	const auto folder = safe.match(info.title_id).hasMatch() ? info.title_id : game.id;
	const QDir icons(QDir(data_dir).filePath(QStringLiteral("trophies/%1").arg(folder)));
	const auto list = GameServices::Trophies(info, runtime_root);
	QJsonArray items;
	for (const auto& v: list[QStringLiteral("trophies")].toArray()) {
		const auto t        = v.toObject();
		const bool unlocked = t[QStringLiteral("unlocked")].toBool();
		const bool hidden   = t[QStringLiteral("hidden")].toBool() && !unlocked;
		const auto grade    = GradeName(t[QStringLiteral("grade")].toString());
		const auto icon_ref = t[QStringLiteral("icon")].toString();
		const auto id =
		    icon_ref.isEmpty() ? QString::number(t[QStringLiteral("id")].toInt()) : icon_ref;
		auto label =
		    hidden ? QStringLiteral("Hidden trophy") : t[QStringLiteral("name")].toString();
		if (label.isEmpty()) label = QStringLiteral("Trophy %1").arg(id);
		auto description = grade;
		if (!hidden && !t[QStringLiteral("detail")].toString().isEmpty()) {
			description += QStringLiteral(". ") + t[QStringLiteral("detail")].toString();
		}
		QJsonObject item {{"id", id}, {"label", label}, {"unlocked", unlocked}};
		if (!description.isEmpty()) item[QStringLiteral("description")] = description;
		if (!icon_ref.isEmpty()) {
			const auto parts = icon_ref.split(QLatin1Char('-'));
			const auto path  = icons.filePath(icon_ref + QStringLiteral(".png"));
			if (!QFileInfo(path).isFile() && parts.size() == 2) {
				const auto png = GameServices::TrophyIcon(info, runtime_root, parts[0].toInt(),
				                                          parts[1].toInt());
				QSaveFile  file(path);
				if (!png.png.isEmpty() && icons.mkpath(QStringLiteral(".")) &&
				    file.open(QIODevice::WriteOnly) && file.write(png.png) == png.png.size()) {
					(void)file.commit();
				}
			}
			if (QFileInfo(path).isFile()) item[QStringLiteral("icon")] = path;
		}
		items.append(item);
	}
	return Reply::Success({{"items", items}});
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
		    // The file for the installed version, else the newest.
		    const auto version = m_info->gameVersion;
		    auto       pick    = std::find_if(supported.begin(), supported.end(),
			                                  [&](const auto& f) { return f.version == version; });
		    if (pick == supported.end()) {
			    pick = std::max_element(
			        supported.begin(), supported.end(),
			        [](const auto& a, const auto& b) { return VersionLess(a.version, b.version); });
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
	m_error   = error;
	m_message = message;
	m_phase   = Phase::Done;
}

Reply CheatFetcher::Run(const GameEntry& game, const QString& cheats_root, int wait_ms) {
	if (Cheats::PlanPath(cheats_root, CheatTitle(*game.info)).isEmpty()) {
		return Reply::Failed(QStringLiteral("Cheats are not available for this game."));
	}
	if (m_phase != Phase::Idle && m_phase != Phase::Done && m_game != game.id) {
		return Reply::Failed(
		    QStringLiteral("Cheats for another game are downloading; try again in a moment."));
	}
	if (m_phase == Phase::Idle || (m_phase == Phase::Done && m_game != game.id)) {
		m_game        = game.id;
		m_info        = game.info;
		m_cheats_root = cheats_root;
		m_error.clear();
		m_message.clear();
		m_phase = Phase::Catalog;
		m_repository.Load(CheatTitle(*game.info));
	}
	if (m_phase != Phase::Done) {
		QEventLoop loop;
		QTimer     timer;
		timer.setSingleShot(true);
		connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
		QTimer poll;
		connect(&poll, &QTimer::timeout, &loop, [&] {
			if (m_phase == Phase::Done) loop.quit();
		});
		timer.start(wait_ms);
		poll.start(25);
		if (m_phase != Phase::Done) loop.exec();
	}
	if (m_phase != Phase::Done) {
		return Reply::Failed(
		    QStringLiteral("Cheats are still downloading. Run Fetch cheats again in a moment."));
	}
	m_phase = Phase::Idle;
	if (!m_error.isEmpty()) return Reply::Failed(m_error);
	return Reply::Success({{"message", m_message}});
}

namespace AdapterActions {

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
	      }}});
}

Reply Run(const GameEntry& game, const QJsonObject& params, const QString& runtime_root,
          const QString& data_dir, CheatFetcher& fetcher) {
	const auto action = params[QStringLiteral("action_id")].toString();
	if (action == QLatin1String("cheats")) return CheatList(game, params, runtime_root);
	if (action == QLatin1String("cheats.fetch")) return fetcher.Run(game, runtime_root, 3500);
	if (action == QLatin1String("trophies")) return Trophies(game, runtime_root, data_dir);
	if (action == QLatin1String("info")) return Info(game);
	return Reply::NotFound(QStringLiteral("Unknown action %1.").arg(action));
}

} // namespace AdapterActions
