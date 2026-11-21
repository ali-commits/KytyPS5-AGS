#ifndef AGS_ADAPTER_TROPHY_ITEMS_H
#define AGS_ADAPTER_TROPHY_ITEMS_H

// The game page's trophies section (`cards`, sheet 1.2). Each trophy package is loaded once per
// call and its icons are written from that same package.
#include "common/trophies.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

class Configuration;

namespace AdapterTrophies {

// Card id: unique across the game's packages, as `<service label>-<trophy id>`.
[[nodiscard]] QString ItemId(unsigned label, int trophy_id);

// Cards of one package. Icons are written to `<icons_dir>/<card id>.png` when missing (`image`);
// a hidden trophy that is still locked gets neither its name, its description nor its icon.
// Label and description are cut to the card limits (80, 300) without control characters.
[[nodiscard]] QJsonArray Cards(const Common::Trophies::Package&    package,
                               const Common::Trophies::UnlockData& unlocks, unsigned label,
                               const QString& icons_dir);

// The `kyty.trophies` banner with the cards of all packages of the game (at most 1024), read
// from `info.basedir` (folder or archive); unlock state is read from `runtime_root`. Empty
// without cards.
[[nodiscard]] QJsonObject Section(const Configuration& info, const QString& runtime_root,
                                  const QString& icons_dir);

} // namespace AdapterTrophies

#endif
