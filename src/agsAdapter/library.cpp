#include "library.h"

#include "gameContent.h"
#include "gameLibrary.h"
#include "gameServices.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonObject>
#include <QSaveFile>

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

} // namespace

QString ArtPath(const GameEntry& game, const QString& kind, const QString& data_dir) {
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
	const auto target = QDir(data_dir).filePath(QStringLiteral("art/%1/%2.png").arg(game.id, kind));
	if (QFileInfo(target).isFile()) return target; // written once
	// GameServices::GameArt reads the same files, also from archives (backgrounds as JPEG).
	const auto art =
	    GameServices::GameArt(info, icon ? QStringLiteral("icon") : QStringLiteral("background"));
	if (art.data.isEmpty()) return {};
	QImage image;
	if (!image.loadFromData(art.data)) return {};
	const auto png = EncodePng(image, icon ? 512 : GameServices::MAX_BACKGROUND_WIDTH);
	if (png.isEmpty() || !QDir().mkpath(QFileInfo(target).absolutePath())) return {};
	QSaveFile file(target);
	if (!file.open(QIODevice::WriteOnly) || file.write(png) != png.size() || !file.commit()) {
		return {};
	}
	return target;
}

QJsonArray Games(const GameLibrary& library, const QString& data_dir) {
	QJsonArray games;
	for (const auto& game: library.Games()) {
		const auto& info  = *game.info;
		auto        title = info.name.trimmed();
		if (title.isEmpty()) title = game.id.left(16);
		QJsonObject entry {
		    {"id", game.id}, {"title", title.left(200)}, {"platform", "ps5"}, {"installed", true}};
		const auto title_id = info.title_id.trimmed();
		if (!title_id.isEmpty() && title_id.size() <= 128) {
			entry["identity"] = QJsonObject {{"scheme", "ps5-title"}, {"value", title_id}};
		}
		if (const auto icon = ArtPath(game, QStringLiteral("icon"), data_dir); !icon.isEmpty()) {
			entry["icon"] = icon;
		}
		if (const auto cover = ArtPath(game, QStringLiteral("cover"), data_dir); !cover.isEmpty()) {
			entry["cover"] = cover;
		}
		games.append(entry);
	}
	return games;
}

} // namespace AdapterLibrary
