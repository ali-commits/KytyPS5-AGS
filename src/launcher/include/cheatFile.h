#ifndef CHEAT_FILE_H
#define CHEAT_FILE_H

#include <QByteArray>
#include <QJsonDocument>
#include <QList>
#include <QString>

namespace Cheats {

[[nodiscard]] bool    IsSupportedTitleId(const QString& title_id);
// <root>/_Patches/<TITLE>.json; empty for an unsupported title id.
[[nodiscard]] QString PlanPath(const QString& root, const QString& title_id);
// Standalone launcher default: the root is the executable directory.
[[nodiscard]] QString PlanPath(const QString& title_id);
// The plan for game `version` next to `plan_path`: `plan_path` itself when it is missing, holds
// that version or no "version", or when `version` is empty; otherwise <stem>-<version>.json
// beside it (which may not exist yet). Empty for an empty `plan_path` or an unsafe `version`.
[[nodiscard]] QString PlanPathForVersion(const QString& plan_path, const QString& version);
// Replaces MC4 (encrypted) or SHN trainer XML with the equivalent cheat JSON.
[[nodiscard]] QString ConvertTrainer(QByteArray* data, bool encrypted);

// Local files retain their existing defaults; remote imports receive additional validation.
[[nodiscard]] QString       ParseDocument(const QByteArray& data, QJsonDocument* document);
[[nodiscard]] QString       ValidateImport(const QJsonDocument& document, const QString& title_id,
                                           const QString& version, const QString& process);
[[nodiscard]] QJsonDocument WithSelection(const QJsonDocument& document,
                                          const QList<bool>&   enabled);
// Refuse to replace a file changed since it was loaded; empty data also permits creation.
[[nodiscard]] QString SaveDocument(const QString& path, const QJsonDocument& document,
                                   const QByteArray& expected_data);

} // namespace Cheats

#endif // CHEAT_FILE_H
