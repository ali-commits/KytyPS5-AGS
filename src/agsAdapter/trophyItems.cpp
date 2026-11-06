#include "trophyItems.h"

#include "configuration.h"
#include "gameContent.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimeZone>

#include <algorithm>

namespace AdapterTrophies {

namespace {

QString TierName(int grade) {
	switch (grade) {
		case 1: return QStringLiteral("platinum");
		case 2: return QStringLiteral("gold");
		case 3: return QStringLiteral("silver");
		default: return QStringLiteral("bronze");
	}
}

// Unlock times are microseconds (since 0001-01-01); STANDARD.md wants UTC YYYY-MM-DDTHH:MM:SSZ.
QString UnlockedAt(uint64_t tick) {
	if (tick < Common::Trophies::UnixEpochTick) return {};
	const auto secs = static_cast<qint64>((tick - Common::Trophies::UnixEpochTick) / 1000000ULL);
	return QDateTime::fromSecsSinceEpoch(secs, QTimeZone::UTC)
	    .toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'"));
}

void WriteIcon(const QString& path, const std::vector<std::byte>& png) {
	if (const QFileInfo existing(path);
	    existing.isFile() && existing.size() == static_cast<qint64>(png.size())) {
		return;
	}
	if (!QDir().mkpath(QFileInfo(path).absolutePath())) return;
	QSaveFile file(path);
	if (file.open(QIODevice::WriteOnly) &&
	    file.write(reinterpret_cast<const char*>(png.data()), static_cast<qint64>(png.size())) ==
	        static_cast<qint64>(png.size())) {
		(void)file.commit();
	}
}

} // namespace

QString ItemId(unsigned label, int trophy_id) {
	return QStringLiteral("%1-%2").arg(label).arg(trophy_id);
}

QJsonArray Items(const Common::Trophies::Package&    package,
                 const Common::Trophies::UnlockData& unlocks, unsigned label,
                 const QString& icons_dir) {
	QJsonArray items;
	for (const auto& [id, trophy]: package.trophies) {
		const bool unlocked = unlocks.unlocked.contains(id);
		const bool hidden   = trophy.hidden && !unlocked;
		const auto item_id  = ItemId(label, id);
		auto name = hidden ? QStringLiteral("Hidden trophy") : QString::fromStdString(trophy.name);
		if (name.isEmpty()) name = QStringLiteral("Trophy %1").arg(item_id);
		QJsonObject item {{"id", item_id},
		                  {"label", name},
		                  {"unlocked", unlocked},
		                  {"tier", TierName(trophy.grade)},
		                  {"hidden", trophy.hidden}};
		if (!hidden && !trophy.description.empty()) {
			item[QStringLiteral("description")] = QString::fromStdString(trophy.description);
		}
		if (const auto at = unlocks.timestamps.find(id); unlocked && at != unlocks.timestamps.end()) {
			if (const auto text = UnlockedAt(at->second); !text.isEmpty())
				item[QStringLiteral("unlocked_at")] = text;
		}
		if (!hidden && !trophy.icon_png.empty()) {
			const auto path = QDir(icons_dir).filePath(item_id + QStringLiteral(".png"));
			WriteIcon(path, trophy.icon_png);
			if (QFileInfo(path).isFile()) item[QStringLiteral("icon")] = path;
		}
		items.append(item);
	}
	return items;
}

Reply Run(const Configuration& info, const QString& runtime_root, const QString& icons_dir) {
	static const QRegularExpression           file_pattern(QStringLiteral("^trophy(\\d+)\\.ucp$"));
	std::vector<std::pair<unsigned, QString>> packages;
	for (const auto& file: GameContent::ListFiles(
	         info.basedir, QString::fromLatin1(Common::Trophies::PackageDirectory))) {
		if (const auto m = file_pattern.match(QFileInfo(file).fileName()); m.hasMatch()) {
			packages.emplace_back(m.captured(1).toUInt(), file);
		}
	}
	std::sort(packages.begin(), packages.end());
	QJsonArray items;
	for (const auto& [label, file]: packages) {
		const auto package =
		    Common::Trophies::LoadPackage(GameContent::ToPath(file), info.console_language);
		const auto unlocks = Common::Trophies::LoadUnlockDataMigrating(
		    GameContent::ToPath(runtime_root), info.title_id.toStdString(), info.user_id, label);
		for (const auto& item: Items(package, unlocks, label, icons_dir))
			items.append(item);
	}
	return Reply::Success({{"items", items}});
}

} // namespace AdapterTrophies
