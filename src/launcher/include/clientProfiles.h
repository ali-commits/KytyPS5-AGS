#ifndef LAUNCHER_INCLUDE_CLIENT_PROFILES_H_
#define LAUNCHER_INCLUDE_CLIENT_PROFILES_H_

#include <QJsonObject>
#include <QString>

// Per-device (per paired Moonlight client) game-settings and cheat-selection store.
//
// Settings are linked to the device: the first read of a (client, game) pair seeds a COPY of the
// PC's current settings and cheat selections; later PC changes do not flow into an existing phone
// profile, and phone changes never touch the PC's Kyty.ini or _Patches/<TITLE>.json. Files are
// owner-only (0600, dirs 0700) and written atomically (temp + rename).
//
// Layout, rooted at StreamingSettings::ProfileDirectory() ($KYTY_STREAMING_CONFIG_DIR):
//   clients/<uuid>/client.json                 {"name","last_seen"}
//   clients/<uuid>/games/<game-id>.json        {"version":1,"settings":{...},"cheats":{...}}
//   clients/<uuid>/games/<game-id>.patch.json   per-client --game-patch plan
namespace ClientProfiles {

// The Sunshine named-device UUID, validated before it ever touches a path.
[[nodiscard]] bool IsValidClientId(const QString& id);

// Absolute clients/<uuid> directory, or empty when id is invalid. Does not create it.
[[nodiscard]] QString ClientDir(const QString& id);

// Record/refresh the client's display name and last-seen time (owner-only, atomic). No-op on an
// invalid id.
void TouchClient(const QString& id, const QString& name);

// The stored per-(client, game) document, or an empty object when none exists yet. seeded is set
// to true when a stored profile already exists (the caller seeds from PC settings when false).
[[nodiscard]] QJsonObject ReadGame(const QString& id, const QString& game_id, bool* seeded);

// Persist the per-(client, game) document (owner-only, atomic). Returns an error string on failure.
[[nodiscard]] QString WriteGame(const QString& id, const QString& game_id,
                                const QJsonObject& document);

// Remove a client's stored profile for a game (used by settings/reset). Returns an error on a real
// failure; a missing file is success.
[[nodiscard]] QString RemoveGame(const QString& id, const QString& game_id);

// Write the per-client cheat plan (the exact --game-patch format) to <game-id>.patch.json, or
// remove it when the plan is empty. Returns an error string on failure; out_path receives the plan
// path when one was written, or empty when none.
[[nodiscard]] QString WritePatchPlan(const QString& id, const QString& game_id,
                                     const QByteArray& plan, QString* out_path);

// Absolute path of the per-client cheat plan file (whether or not it exists), or empty on bad id.
[[nodiscard]] QString PatchPlanPath(const QString& id, const QString& game_id);

} // namespace ClientProfiles

#endif /* LAUNCHER_INCLUDE_CLIENT_PROFILES_H_ */
