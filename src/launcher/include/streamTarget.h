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
    if (target.size() != 3) return false;
    for (const auto* key: {"width", "height", "fps"}) {
        const auto number = target[key].toDouble(-1);
        const bool fps = QString::fromLatin1(key) == "fps";
        if (!target[key].isDouble() || !std::isfinite(number) || number != std::floor(number) ||
            number < (fps ? 1 : 64) || number > (fps ? 240 : 8192) || (!fps && static_cast<int>(number) % 2 != 0)) return false;
    }
    return true;
}
inline bool Apply(const QJsonObject& target, QStringList& args) {
    if (!Valid(target)) return false;
    const QStringList flags {"--screen-width", "--screen-height", "--vblank-frequency"};
    const QStringList keys {"width", "height", "fps"};
    QStringList rewritten;
    for (qsizetype i = 0; i < args.size(); ++i) {
        if (flags.contains(args[i])) {
            if (i + 1 >= args.size() || args[i + 1].startsWith("--")) return false;
            ++i;
        } else rewritten.append(args[i]);
    }
    for (qsizetype i = 0; i < keys.size(); ++i) {
        const int n = target[keys[i]].toInt();
        rewritten << flags[i] << QString::number(n);
    }
    args = std::move(rewritten);
    return true;
}
}
#endif
