#include "gameServices.h"

#include "cheatFile.h"
#include "common/archive.h"
#include "common/trophies.h"
#include "gameContent.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStringList>
#include <QTimeZone>

namespace GameServices {

namespace {

QString TitleId(const Configuration& info) {
	return info.title_id.trimmed().toUpper();
}

QString Process(const Configuration& info) {
	return QFileInfo(info.elf.isEmpty() ? QStringLiteral("eboot.bin") : info.elf).fileName();
}

// A missing file has an empty revision.
QByteArray ReadCheatBytes(const QString& path) {
	QFile file(path);
	return !path.isEmpty() && file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray {};
}

QString CheatRevision(const QByteArray& bytes) {
	return bytes.isEmpty() ? QString {} :
	                         QString::fromLatin1(QCryptographicHash::hash(
	                                                 bytes, QCryptographicHash::Sha256)
	                                                 .toHex());
}

} // namespace

bool SafeDirectory(const QString& path) {
	if (!QDir::isAbsolutePath(path) || path.split('/').contains("..")) return false;
	QString current;
	for (const auto& part: path.split('/', Qt::SkipEmptyParts)) {
		current += '/' + part;
		const QFileInfo info(current);
		if (!info.isDir() || info.isSymLink()) return false;
	}
	return true;
}

bool SafeRuntime(const QString& path) {
	if (!SafeDirectory(path)) return false;
	for (const auto& name: {"_SaveData", "_Trophies", "_Cache", "_CrashCapture", "_HangTrace",
	                        "_TempData", "_ShaderLogs", "_CommandBuffers"}) {
		const auto child = QDir(path).filePath(name);
		if ((QFileInfo::exists(child) || QFileInfo(child).isSymLink()) && !SafeDirectory(child))
			return false;
		QDirIterator entries(child,
		                     QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
		                     QDirIterator::Subdirectories);
		while (entries.hasNext()) {
			entries.next();
			if (entries.fileInfo().isSymLink()) return false;
		}
	}
	return true;
}

QJsonObject Trophies(const Configuration& info, const QString& runtime_directory) {
	static const QRegularExpression file_pattern(QStringLiteral("^trophy(\\d+)\\.ucp$"));
	const auto                      grade_name = [](int grade) -> QString {
		switch (grade) {
			case 1: return QStringLiteral("platinum");
			case 2: return QStringLiteral("gold");
			case 3: return QStringLiteral("silver");
			case 4: return QStringLiteral("bronze");
			default: return QStringLiteral("bronze");
		}
	};
	QJsonArray  trophies;
	int         unlocked_count = 0;
	int         total          = 0;
	const auto  reader         = Common::OpenArchive(GameContent::ToPath(info.basedir));
	QStringList files;
	for (const auto& file: GameContent::ListFiles(
	         info.basedir, QString::fromLatin1(Common::Trophies::PackageDirectory))) {
		if (file_pattern.match(QFileInfo(file).fileName()).hasMatch()) {
			files.append(file);
		}
	}
	files.sort();
	for (const auto& file: files) {
		const auto label = file_pattern.match(QFileInfo(file).fileName()).captured(1).toUInt();
		const auto package =
		    Common::Trophies::LoadPackage(GameContent::ToPath(file), info.console_language);
		const auto unlocks = Common::Trophies::LoadUnlockDataMigrating(
		    GameContent::ToPath(runtime_directory), info.title_id.toStdString(), info.user_id,
		    label);
		for (const auto& [id, trophy]: package.trophies) {
			const bool is_unlocked = unlocks.unlocked.contains(id);
			++total;
			if (is_unlocked) {
				++unlocked_count;
			}
			QJsonObject entry {{"id", id},
			                   {"name", QString::fromStdString(trophy.name)},
			                   {"detail", QString::fromStdString(trophy.description)},
			                   {"grade", grade_name(trophy.grade)},
			                   {"hidden", trophy.hidden},
			                   {"unlocked", is_unlocked}};
			// Hidden locked trophies get no icon so their art is not revealed.
			if (!trophy.icon_png.empty() && (is_unlocked || !trophy.hidden))
				entry["icon"] = QStringLiteral("%1-%2").arg(label).arg(id);
			if (is_unlocked) {
				const auto found = unlocks.timestamps.find(id);
				if (found != unlocks.timestamps.end()) {
					entry["unlocked_at"] =
					    QDateTime::fromMSecsSinceEpoch(
					        static_cast<qint64>((found->second - Common::Trophies::UnixEpochTick) /
					                            1000),
					        QTimeZone::UTC)
					        .toString(Qt::ISODate);
				}
			}
			trophies.append(entry);
		}
	}
	return {{"trophies", trophies}, {"unlocked", unlocked_count}, {"total", total}};
}

TrophyIconResult TrophyIcon(const Configuration& info, const QString& runtime_directory,
                            int label, int id) {
	static const QRegularExpression file_pattern(QStringLiteral("^trophy(\\d+)\\.ucp$"));
	for (const auto& file: GameContent::ListFiles(
	         info.basedir, QString::fromLatin1(Common::Trophies::PackageDirectory))) {
		const auto match = file_pattern.match(QFileInfo(file).fileName());
		if (!match.hasMatch() || match.captured(1).toInt() != label) continue;
		const auto package =
		    Common::Trophies::LoadPackage(GameContent::ToPath(file), info.console_language);
		const auto found = package.trophies.find(id);
		if (found == package.trophies.end() || found->second.icon_png.empty()) break;
		if (found->second.hidden) {
			const auto unlocks = Common::Trophies::LoadUnlockDataMigrating(
			    GameContent::ToPath(runtime_directory), info.title_id.toStdString(), info.user_id,
			    static_cast<uint32_t>(label));
			if (!unlocks.unlocked.contains(id)) return {{}, QStringLiteral("hidden")};
		}
		return {QByteArray(reinterpret_cast<const char*>(found->second.icon_png.data()),
		                   static_cast<qsizetype>(found->second.icon_png.size())),
		        {}};
	}
	return {{}, QStringLiteral("no-icon")};
}

QJsonObject GetCheats(const Configuration& info, const QString& cheats_root) {
	const auto path = Cheats::PlanPath(cheats_root, TitleId(info));
	if (path.isEmpty()) {
		return {{"supported", false}, {"revision", ""}, {"mods", QJsonArray {}}};
	}
	const auto    bytes = ReadCheatBytes(path);
	QJsonDocument document;
	QJsonArray    mods;
	QString       error;
	if (!bytes.isEmpty()) {
		error     = Cheats::ParseDocument(bytes, &document);
		int index = 0;
		for (const auto& value: document.object().value(QStringLiteral("mods")).toArray()) {
			const auto mod     = value.toObject();
			const auto name    = mod.value(QStringLiteral("name")).toString();
			const bool enabled = mod.value(QStringLiteral("enabled")).toBool(true);
			mods.append(QJsonObject {{"index", index++}, {"name", name}, {"enabled", enabled}});
		}
	}
	QJsonObject reply {{"supported", true}, {"revision", CheatRevision(bytes)}, {"mods", mods}};
	if (!error.isEmpty()) {
		reply["error"] = error;
	}
	return reply;
}

QString PutCheats(const Configuration& info, const QString& cheats_root, const QJsonObject& body,
                  QString* revision) {
	const auto path = Cheats::PlanPath(cheats_root, TitleId(info));
	if (path.isEmpty()) {
		return QStringLiteral("unsupported-title-id");
	}
	const auto bytes = ReadCheatBytes(path);
	if (body.value(QStringLiteral("revision")).toString() != CheatRevision(bytes)) {
		return QStringLiteral("stale");
	}
	QJsonDocument document;
	if (const auto error = Cheats::ParseDocument(bytes, &document); !error.isEmpty()) {
		return error;
	}
	const auto selection = body.value(QStringLiteral("enabled"));
	if (!selection.isArray()) {
		return QStringLiteral("bad-body");
	}
	QList<bool> enabled;
	for (const auto& value: selection.toArray()) {
		if (!value.isBool()) {
			return QStringLiteral("bad-body");
		}
		enabled.append(value.toBool());
	}
	const auto updated = Cheats::WithSelection(document, enabled);
	if (updated.isNull()) {
		return QStringLiteral("selection-size-mismatch");
	}
	if (!Cheats::SaveDocument(path, updated, bytes).isEmpty()) {
		return QStringLiteral("stale");
	}
	if (revision != nullptr) *revision = CheatRevision(ReadCheatBytes(path));
	return {};
}

QJsonObject RemoteCheatList(const Configuration& info, const QString& cheats_root,
                            const QList<Cheats::RemoteFile>& files, const QString& error) {
	const auto path = Cheats::PlanPath(cheats_root, TitleId(info));
	if (path.isEmpty()) {
		return {{"files", QJsonArray {}}, {"error", QStringLiteral("unsupported-title-id")}};
	}
	QJsonDocument local;
	(void)Cheats::ParseDocument(ReadCheatBytes(path), &local);
	const auto local_version = local.object().value(QStringLiteral("version")).toString();
	QJsonArray list;
	for (const auto& file: files) {
		if (!file.IsSupported()) {
			continue;
		}
		list.append(QJsonObject {
		    {"name", file.name},
		    {"title", file.title},
		    {"version", file.version},
		    {"format", file.format},
		    {"installed_version", !local_version.isEmpty() && local_version == file.version}});
	}
	QJsonObject reply {{"files", list}};
	if (!error.isEmpty()) {
		reply["error"] = error;
	}
	return reply;
}

QString ImportRemoteCheat(const Configuration& info, const QString& cheats_root,
                          const QByteArray& data, QString* revision) {
	const auto    title_id = TitleId(info);
	QJsonDocument document;
	auto          error = Cheats::ParseDocument(data, &document);
	if (error.isEmpty()) {
		error = Cheats::ValidateImport(document, title_id, info.gameVersion, Process(info));
	}
	if (!error.isEmpty()) {
		return error;
	}
	const auto path  = Cheats::PlanPath(cheats_root, title_id);
	const auto mods  = document.object().value(QStringLiteral("mods")).toArray();
	const auto saved = Cheats::WithSelection(document, QList<bool>(mods.size(), false));
	if (const auto save_error = Cheats::SaveDocument(path, saved, ReadCheatBytes(path));
	    !save_error.isEmpty()) {
		return save_error;
	}
	if (revision != nullptr) *revision = CheatRevision(ReadCheatBytes(path));
	return {};
}

Image GameArt(const Configuration& info, const QString& kind) {
	const bool        background = kind != QLatin1String("icon");
	const QStringList names =
	    background ? QStringList {QStringLiteral("sce_sys/pic0.png"),
	                              QStringLiteral("sce_sys/pic1.png")} :
	                 QStringList {QStringLiteral("sce_sys/icon0.png")};
	for (const auto& name: names) {
		const auto data = GameContent::ReadFile(info.basedir, name, GameContent::MaxImageSize);
		if (data.isEmpty()) {
			continue;
		}
		if (background) {
			// Game backgrounds are often 4K PNGs (~10 MB); deliver a 1920-wide JPEG.
			QImage image;
			if (image.loadFromData(data)) {
				if (image.width() > MAX_BACKGROUND_WIDTH)
					image = image.scaledToWidth(MAX_BACKGROUND_WIDTH, Qt::SmoothTransformation);
				QByteArray jpeg;
				QBuffer    buffer(&jpeg);
				if (buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "JPG", 85))
					return {jpeg, "image/jpeg"};
			}
		}
		return {data, "image/png"};
	}
	return {};
}

} // namespace GameServices
