#ifndef LAUNCHER_OWNER_PREFERENCES_H
#define LAUNCHER_OWNER_PREFERENCES_H
#include <QJsonObject>
#include <QJsonArray>
#include <QSettings>
class Configuration;
namespace OwnerPreferences {
// Shared game-setting controls for the owner and authenticated per-game APIs.
QJsonArray SettingsFields(const Configuration& configuration);
// Local owner protocol only. Never expose through the remotely authenticated feature API.
QJsonObject Read(QSettings& settings);
bool        Write(QSettings& settings, const QJsonObject& patch, QString& error);
} // namespace OwnerPreferences
#endif
