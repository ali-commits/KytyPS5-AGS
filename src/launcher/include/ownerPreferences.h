#ifndef LAUNCHER_OWNER_PREFERENCES_H
#define LAUNCHER_OWNER_PREFERENCES_H
#include <QJsonObject>
#include <QSettings>
namespace OwnerPreferences {
// Local owner protocol only. Never expose through the remotely authenticated feature API.
QJsonObject Read(QSettings& settings);
bool        Write(QSettings& settings, const QJsonObject& patch, QString& error);
} // namespace OwnerPreferences
#endif
