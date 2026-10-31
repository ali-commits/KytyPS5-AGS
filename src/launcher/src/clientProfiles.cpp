#include "clientProfiles.h"

#include "streamingSettings.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>

namespace ClientProfiles {

namespace {

// Owner-only (0700), never following a symlink to escape the profile root.
bool EnsurePrivateDir(const QString& path) {
	if (QFileInfo(path).isSymLink()) {
		return false;
	}
	if (!QDir().mkpath(path)) {
		return false;
	}
	return QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

// Atomic (temp + rename) owner-only (0600) write.
QString WritePrivate(const QString& path, const QByteArray& bytes) {
	if (!EnsurePrivateDir(QFileInfo(path).absolutePath())) {
		return QStringLiteral("Cannot create the private client profile directory.");
	}
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly) ||
	    !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
	    file.write(bytes) != bytes.size() || !file.commit()) {
		return QStringLiteral("Cannot write client profile %1: %2").arg(path, file.errorString());
	}
	return {};
}

// A game id is the catalog's SHA-256 hex; validate it before it becomes a file name.
bool IsValidGameId(const QString& game_id) {
	static const QRegularExpression pattern(QStringLiteral("\\A[0-9a-f]{64}\\z"));
	return pattern.match(game_id).hasMatch();
}

QString GamesDir(const QString& id) {
	const auto base = ClientDir(id);
	return base.isEmpty() ? QString {} : QDir(base).filePath(QStringLiteral("games"));
}

QString GameFile(const QString& id, const QString& game_id) {
	if (!IsValidGameId(game_id)) {
		return {};
	}
	const auto games = GamesDir(id);
	return games.isEmpty() ? QString {} : QDir(games).filePath(game_id + QStringLiteral(".json"));
}

} // namespace

bool IsValidClientId(const QString& id) {
	static const QRegularExpression pattern(QStringLiteral(
	    "\\A[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\\z"));
	return pattern.match(id).hasMatch();
}

QString ClientDir(const QString& id) {
	if (!IsValidClientId(id)) {
		return {};
	}
	return QDir(StreamingSettings::ProfileDirectory())
	    .filePath(QStringLiteral("clients/%1").arg(id));
}

void TouchClient(const QString& id, const QString& name) {
	const auto dir = ClientDir(id);
	if (dir.isEmpty()) {
		return;
	}
	const QJsonObject client {
	    {QStringLiteral("name"), name},
	    {QStringLiteral("last_seen"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
	(void)WritePrivate(QDir(dir).filePath(QStringLiteral("client.json")),
	                   QJsonDocument(client).toJson(QJsonDocument::Compact));
}

QJsonObject ReadGame(const QString& id, const QString& game_id, bool* seeded) {
	if (seeded != nullptr) {
		*seeded = false;
	}
	const auto path = GameFile(id, game_id);
	if (path.isEmpty()) {
		return {};
	}
	QFile file(path);
	if (QFileInfo(path).isSymLink() || !file.open(QIODevice::ReadOnly)) {
		return {};
	}
	const auto document = QJsonDocument::fromJson(file.readAll());
	if (!document.isObject()) {
		return {};
	}
	if (seeded != nullptr) {
		*seeded = true;
	}
	return document.object();
}

QString WriteGame(const QString& id, const QString& game_id, const QJsonObject& document) {
	const auto path = GameFile(id, game_id);
	if (path.isEmpty()) {
		return QStringLiteral("invalid-client-or-game");
	}
	return WritePrivate(path, QJsonDocument(document).toJson(QJsonDocument::Compact));
}

QString RemoveGame(const QString& id, const QString& game_id) {
	const auto path = GameFile(id, game_id);
	if (path.isEmpty()) {
		return QStringLiteral("invalid-client-or-game");
	}
	QFile file(path);
	if (file.exists() && !file.remove()) {
		return QStringLiteral("Cannot remove client profile %1: %2").arg(path, file.errorString());
	}
	return {};
}

QString PatchPlanPath(const QString& id, const QString& game_id) {
	if (!IsValidGameId(game_id)) {
		return {};
	}
	const auto games = GamesDir(id);
	return games.isEmpty() ? QString {}
	                       : QDir(games).filePath(game_id + QStringLiteral(".patch.json"));
}

QString WritePatchPlan(const QString& id, const QString& game_id, const QByteArray& plan,
                       QString* out_path) {
	if (out_path != nullptr) {
		out_path->clear();
	}
	const auto path = PatchPlanPath(id, game_id);
	if (path.isEmpty()) {
		return QStringLiteral("invalid-client-or-game");
	}
	if (plan.isEmpty()) {
		QFile file(path);
		if (file.exists() && !file.remove()) {
			return QStringLiteral("Cannot remove client patch plan %1: %2")
			    .arg(path, file.errorString());
		}
		return {};
	}
	if (const auto error = WritePrivate(path, plan); !error.isEmpty()) {
		return error;
	}
	if (out_path != nullptr) {
		*out_path = path;
	}
	return {};
}

} // namespace ClientProfiles
