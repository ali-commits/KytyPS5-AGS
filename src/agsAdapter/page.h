#ifndef AGS_ADAPTER_PAGE_H
#define AGS_ADAPTER_PAGE_H

// page.describe (protocol 2): the protocol 1 schema of AdapterSettings::Schema() as ags_page
// sections, for the extension's own page and a game's page.
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

struct GameEntry;

namespace AdapterPage {

enum class PageKind { Game, Extension };

// One protocol 1 field of Schema() as an ags_page item (sheet 1.2) at `scope`
// ("game_on_device", "device" or "machine").
[[nodiscard]] QJsonObject Item(const QJsonObject& field, const QString& scope);
// The panel sections built from Schema()'s groups for that page, empty ones left out.
[[nodiscard]] QJsonArray SettingsSections(PageKind kind);
// {"sections": [...]}: the extension page for `game == nullptr`, else the game page.
[[nodiscard]] QJsonObject Describe(const GameEntry* game, const QString& runtime_root,
                                   const QString& data_dir);

} // namespace AdapterPage

#endif
