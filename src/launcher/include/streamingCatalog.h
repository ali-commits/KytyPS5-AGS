#ifndef KYTY_STREAMING_CATALOG_H
#define KYTY_STREAMING_CATALOG_H
#include <QHash>
#include <QJsonObject>
#include <QStringList>
struct StreamingLaunchEntry {
	QString     id;
	QString     name;
	QString     program;
	QStringList arguments;
	QString     working_directory;
	QString     image_path;
	QJsonObject environment;
	QString     process_visibility = "private";
	bool        operator==(const StreamingLaunchEntry&) const = default;
};
namespace StreamingCatalog {
QString     QuoteBackendToken(QString value);
QJsonObject Manifest(const QList<StreamingLaunchEntry>& entries, const QString& socket,
                     const QString& audio_device, const QString& lock_file,
                     const QString& resolution, int fps, bool show_local = true,
                     const QHash<QString, QList<StreamingLaunchEntry>>& client_games = {});
QJsonObject Apps(const QList<StreamingLaunchEntry>& entries, const QString& wrapper,
                 const QString& manifest);
bool        WritePrivate(const QString& path, const QByteArray& bytes, QString* error);
} // namespace StreamingCatalog
#endif
