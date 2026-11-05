#include "library.h"

#include "cheatFile.h"
#include "configuration.h"
#include "gameContent.h"
#include "gameLibrary.h"
#include "gameServices.h"

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>

namespace AdapterLibrary {

namespace {

bool IsPngFile(const QString& path) {
	const QFileInfo info(path);
	if (!info.isFile() || info.isSymLink() || info.size() > MaxArt || info.size() < 8) return false;
	QFile file(path);
	return file.open(QIODevice::ReadOnly) && file.read(8) == QByteArray("\x89PNG\r\n\x1a\n", 8);
}

// A PNG of at most MaxArt bytes, shrinking the image until it fits.
QByteArray EncodePng(QImage image, int max_width) {
	if (image.width() > max_width) image = image.scaledToWidth(max_width, Qt::SmoothTransformation);
	for (int attempt = 0; attempt < 8 && !image.isNull(); ++attempt) {
		QByteArray png;
		QBuffer    buffer(&png);
		if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) return {};
		if (png.size() <= MaxArt) return png;
		image = image.scaledToWidth(image.width() * 3 / 4, Qt::SmoothTransformation);
	}
	return {};
}

// When the art source last changed: the newest of the candidate files in a game folder, or the
// archive file itself.
QDateTime SourceTime(const Configuration& info, const QStringList& names) {
	if (GameContent::IsArchive(info.basedir)) return QFileInfo(info.basedir).lastModified();
	QDateTime newest;
	for (const auto& name: names) {
		if (const QFileInfo file(QDir(info.basedir).filePath(name)); file.exists()) {
			newest = std::max(newest, file.lastModified());
		}
	}
	return newest;
}

} // namespace

QString DropControls(const QString& text) {
	QString out;
	for (const auto c: text.toUcs4()) {
		if (QChar::category(c) != QChar::Other_Control) out += QString::fromUcs4(&c, 1);
	}
	return out;
}

QString CutChars(const QString& text, int max_chars) {
	QString out;
	int     count = 0;
	for (qsizetype i = 0; i < text.size() && count < max_chars; ++count) {
		const auto length =
		    text[i].isHighSurrogate() && i + 1 < text.size() && text[i + 1].isLowSurrogate() ? 2
			                                                                                 : 1;
		out += text.mid(i, length);
		i += length;
	}
	return out;
}

QString ArtPath(const GameEntry& game, const QString& kind, const QString& data_dir,
                ArtCache& cache) {
	const bool  icon = kind == QLatin1String("icon");
	const auto& info = *game.info;
	const auto  names =
	    icon ? QStringList {QStringLiteral("sce_sys/icon0.png")}
		     : QStringList {QStringLiteral("sce_sys/pic0.png"), QStringLiteral("sce_sys/pic1.png")};
	if (!GameContent::IsArchive(info.basedir)) {
		for (const auto& name: names) {
			const auto path = QDir(info.basedir).filePath(name);
			if (IsPngFile(path)) return QFileInfo(path).absoluteFilePath();
		}
	}
	if (cache.Missing(game.id, kind)) return {};
	const auto target = QDir(data_dir).filePath(QStringLiteral("art/%1/%2.png").arg(game.id, kind));
	if (const QFileInfo done(target);
	    done.isFile() && done.lastModified() >= SourceTime(info, names)) {
		return target; // up to date: written after the source file last changed
	}
	// GameServices::GameArt reads the same files, also from archives (backgrounds as JPEG).
	const auto art =
	    GameServices::GameArt(info, icon ? QStringLiteral("icon") : QStringLiteral("background"));
	QImage     image;
	const auto png = art.data.isEmpty() || !image.loadFromData(art.data)
	                     ? QByteArray {}
	                     : EncodePng(image, icon ? 512 : GameServices::MAX_BACKGROUND_WIDTH);
	if (png.isEmpty()) {
		cache.SetMissing(game.id, kind);
		return {};
	}
	if (!QDir().mkpath(QFileInfo(target).absolutePath())) return {};
	QSaveFile file(target);
	if (!file.open(QIODevice::WriteOnly) || file.write(png) != png.size() || !file.commit()) {
		return {};
	}
	return target;
}

QJsonArray Games(const GameLibrary& library, const QString& data_dir, const QString& runtime_root,
                 ArtCache& cache) {
	QJsonArray games;
	for (const auto& game: library.Games()) {
		const auto& info  = *game.info;
		auto        title = info.name.trimmed();
		if (title.isEmpty()) title = game.id.left(16);
		QJsonObject entry {{"id", game.id},
		                   {"title", CutChars(title, 200)},
		                   {"platform", "ps5"},
		                   {"installed", true}};
		const auto  title_id = DropControls(info.title_id).trimmed();
		if (!title_id.isEmpty() && title_id.toUcs4().size() <= 128) {
			entry["identity"] = QJsonObject {{"scheme", "ps5-title"}, {"value", title_id}};
		}
		if (const auto icon = ArtPath(game, QStringLiteral("icon"), data_dir, cache);
		    !icon.isEmpty()) {
			entry["icon"] = icon;
		}
		if (const auto cover = ArtPath(game, QStringLiteral("cover"), data_dir, cache);
		    !cover.isEmpty()) {
			entry["cover"] = cover;
		}
		if (const auto version = CutChars(DropControls(info.gameVersion).trimmed(), 32);
		    !version.isEmpty()) {
			entry["version"] = version;
		}
		if (const auto plan = Cheats::PlanPath(runtime_root, info.title_id);
		    !plan.isEmpty() && QFileInfo::exists(plan)) {
			entry["badges"] = QJsonArray {"cheats"};
		}
		games.append(entry);
	}
	return games;
}

} // namespace AdapterLibrary
