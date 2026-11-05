#ifndef AGS_ADAPTER_SETTINGS_H
#define AGS_ADAPTER_SETTINGS_H

// settings.schema, settings.get and settings.set (STANDARD.md sections 5.4 and 6.2), converted
// from PreferenceSchema / OwnerPreferences and stored in Kyty.ini, plus runtime_root in
// adapter.json.
#include "reply.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

class AdapterState;
class Configuration;
class GameLibrary;
struct GameEntry;

namespace AdapterSettings {

// Keys owned by AGS standard settings; never in the schema.
[[nodiscard]] bool CoveredByStandard(const QString& key);

// The schema fields, in order, with groups as flat headings.
[[nodiscard]] const QJsonArray&  Schema();
[[nodiscard]] const QJsonObject* FindField(const QString& key);

// A Kyty.ini string value (GameSettings) as the typed JSON value of `field`.
[[nodiscard]] QJsonValue Typed(const QJsonObject& field, const QString& stored);
// A typed value as the GameSettings string, after a type check; false when the type is wrong.
[[nodiscard]] bool Stored(const QJsonObject& field, const QJsonValue& value, QString* out);

// Applies player values (location "player", already merged by AGS) to a copy of a game's
// configuration for launch.plan. Unknown keys and machine keys are ignored. Returns false with
// `error` set when a known value is invalid; `applied` gets every value used.
[[nodiscard]] bool ApplyPlayer(Configuration& info, const QJsonObject& player, QJsonObject* applied,
                               QString* error);

// The refusal text when Kyty.ini cannot be read; nothing is written then.
inline constexpr char INI_UNREADABLE[] = "Kyty.ini could not be read; not saving";
// True when Kyty.ini is missing or parses without error (QSettings status NoError after load);
// checked on a fresh copy because QSettings reports a parse error once per file name.
[[nodiscard]] bool IniReadable(const QString& ini);

[[nodiscard]] Reply Get(AdapterState& state, const QString& ini, const GameEntry* game);
[[nodiscard]] Reply Set(AdapterState& state, const QString& ini, GameLibrary& library,
                        const GameEntry* game, const QJsonObject& values);

} // namespace AdapterSettings

#endif
