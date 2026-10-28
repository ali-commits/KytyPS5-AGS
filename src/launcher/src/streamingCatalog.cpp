#include "streamingCatalog.h"

#include <QFile>
#include <QJsonArray>
#include <QSaveFile>

QString StreamingCatalog::QuoteBackendToken(QString value) {
	// Sunshine first expands $(ENV), then Boost parses a command line without a shell.
	value.replace('$', "$$");
	value.replace('\\', "\\\\");
	value.replace('"', "\\\"");
	return '"' + value + '"';
}
QJsonObject
StreamingCatalog::Manifest(const QList<StreamingLaunchEntry>& entries, const QString& socket,
                           const QString& audio_device, const QString& lock_file,
                           const QString& resolution, int fps, bool show_local,
                           const QHash<QString, QList<StreamingLaunchEntry>>& client_games) {
	const auto games_object = [](const QList<StreamingLaunchEntry>& list) {
		QJsonObject games;
		for (const auto& entry: list) {
			games.insert(entry.id,
			             QJsonObject {{"program", entry.program},
			                          {"arguments", QJsonArray::fromStringList(entry.arguments)},
			                          {"working_directory", entry.working_directory}});
		}
		return games;
	};
	QJsonObject env {{"KYTY_STREAM_SOCKET", socket},
	                 {"SDL_AUDIO_DRIVER", "pulseaudio"},
	                 {"KYTY_AUDIO_OUTPUT_DEVICE", audio_device},
	                 {"KYTY_STREAM_FPS", QString::number(fps)}};
	if (!show_local) env.insert("KYTY_STREAM_ONLY", "1");
	if (resolution != "Native") {
		env.insert("KYTY_STREAM_WIDTH", resolution == "1080p" ? "1920" : "2560");
		env.insert("KYTY_STREAM_HEIGHT", resolution == "1080p" ? "1080" : "1440");
	}
	QJsonObject clients;
	for (auto it = client_games.constBegin(); it != client_games.constEnd(); ++it) {
		clients.insert(it.key(), games_object(it.value()));
	}
	return {{"version", 1},
	        {"games", games_object(entries)},
	        {"client_games", clients},
	        {"environment", env},
	        {"lock_file", lock_file}};
}
QJsonObject StreamingCatalog::Apps(const QList<StreamingLaunchEntry>& entries,
                                   const QString& wrapper, const QString& manifest) {
	QJsonArray apps;
	for (const auto& entry: entries) {
		const auto command = QuoteBackendToken(wrapper) + ' ' + QuoteBackendToken(manifest) + ' ' +
		                     QuoteBackendToken(entry.id);
		// Titles are subject to Sunshine's env expansion as well.
		auto working_directory = entry.working_directory;
		working_directory.replace('$', "$$");
		auto name = entry.name;
		name.replace('$', "$$");
		QJsonObject app {{"name", name},
		                 {"cmd", command},
		                 {"working-dir", working_directory},
		                 {"auto-detach", false},
		                 {"wait-all", true},
		                 {"exit-timeout", 5},
		                 {"exclude-global-prep-cmd", true}};
		if (!entry.image_path.isEmpty()) {
			auto image = entry.image_path;
			image.replace('$', "$$");
			app.insert("image-path", image);
		}
		apps.append(app);
	}
	return {{"env", QJsonObject {}}, {"apps", apps}};
}
bool StreamingCatalog::WritePrivate(const QString& path, const QByteArray& bytes, QString* error) {
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly) ||
	    !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
	    file.write(bytes) != bytes.size() || !file.commit()) {
		if (error)
			*error =
			    QStringLiteral("Cannot write private file %1: %2").arg(path, file.errorString());
		return false;
	}
	return true;
}
