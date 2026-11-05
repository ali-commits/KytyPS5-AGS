#ifndef LAUNCHER_STREAM_TARGET_H
#define LAUNCHER_STREAM_TARGET_H
#include <QJsonObject>
#include <QStringList>
#include <cmath>

// Session-only display authority. Never persist these values into game preferences.
namespace StreamTarget {
inline bool Valid(const QJsonValue& value) {
    if (!value.isObject()) return false;
    const auto target = value.toObject();
    if (target.size() != (target.contains("aspect") ? 4 : 3)) return false;
    if (target.contains("aspect") &&
        (!target["aspect"].isString() || !QStringList {"fit", "fill", "stretch"}.contains(target["aspect"].toString()))) return false;
    for (const auto* key: {"width", "height", "fps"}) {
        const auto number = target[key].toDouble(-1);
        const bool fps = QString::fromLatin1(key) == "fps";
        // Stream FPS intersects media support (1..240) and emulator vblank (30..360).
        if (!target[key].isDouble() || !std::isfinite(number) || number != std::floor(number) ||
            number < (fps ? 30 : 64) || number > (fps ? 240 : 8192) || (!fps && static_cast<int>(number) % 2 != 0)) return false;
    }
    return true;
}
inline bool Apply(const QJsonObject& target, QStringList& args) {
    if (!Valid(target)) return false;
    const QStringList flags {"--screen-width", "--screen-height", "--vblank-frequency"};
    const QStringList keys {"width", "height", "fps"};
    QStringList rewritten;
    for (qsizetype i = 0; i < args.size(); ++i) {
        if (flags.contains(args[i]) || (target.contains("aspect") && args[i] == "--stream-aspect")) {
            if (i + 1 >= args.size() || args[i + 1].startsWith("--")) return false;
            ++i;
        } else rewritten.append(args[i]);
    }
    for (qsizetype i = 0; i < keys.size(); ++i) {
        const int n = target[keys[i]].toInt();
        rewritten << flags[i] << QString::number(n);
    }
    if (target.contains("aspect")) rewritten << "--stream-aspect" << target["aspect"].toString();
    args = std::move(rewritten);
    return true;
}
}
#endif
