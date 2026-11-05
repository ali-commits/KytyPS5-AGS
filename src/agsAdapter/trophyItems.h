#ifndef AGS_ADAPTER_TROPHY_ITEMS_H
#define AGS_ADAPTER_TROPHY_ITEMS_H

// The trophies action (progress_list items, STANDARD.md section 5.4). Each trophy package is
// loaded once per call and its icons are written from that same package.
#include "common/trophies.h"
#include "reply.h"

#include <QJsonArray>
#include <QString>

class Configuration;

namespace AdapterTrophies {

// Item id: unique across the game's packages, as `<service label>-<trophy id>`.
[[nodiscard]] QString ItemId(unsigned label, int trophy_id);

// Items of one package. Icons are written to `<icons_dir>/<item id>.png` when missing; a hidden
// trophy that is still locked gets neither its name, its description nor its icon.
[[nodiscard]] QJsonArray Items(const Common::Trophies::Package&    package,
                               const Common::Trophies::UnlockData& unlocks, unsigned label,
                               const QString& icons_dir);

// All packages of the game, read from `info.basedir` (folder or archive); unlock state is read
// from `runtime_root`.
[[nodiscard]] Reply Run(const Configuration& info, const QString& runtime_root,
                        const QString& icons_dir);

} // namespace AdapterTrophies

#endif
