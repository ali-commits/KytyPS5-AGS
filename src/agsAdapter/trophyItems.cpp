#include "trophyItems.h"

#include "configuration.h"
#include "gameContent.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>

namespace AdapterTrophies {

namespace {

QString GradeName(int grade) {
	switch (grade) {
		case 1: return QStringLiteral("Platinum");
		case 2: return QStringLiteral("Gold");
		case 3: return QStringLiteral("Silver");
		default: return QStringLiteral("Bronze");
	}
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
		auto description = GradeName(trophy.grade);
		if (!hidden && !trophy.description.empty()) {
			description += QStringLiteral(". ") + QString::fromStdString(trophy.description);
		}
		QJsonObject item {
		    {"id", item_id}, {"label", name}, {"description", description}, {"unlocked", unlocked}};
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
