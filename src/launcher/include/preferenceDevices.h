#ifndef LAUNCHER_PREFERENCE_DEVICES_H
#define LAUNCHER_PREFERENCE_DEVICES_H
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

namespace PreferenceDevices {
inline QJsonArray GpuOptions(const QStringList& names, int selected) {
    QJsonArray options {QJsonObject {{"value", "-1"}, {"label", "Auto"}}};
    for (qsizetype i = 0; i < names.size(); ++i)
        options.append(QJsonObject {{"value", QString::number(i)}, {"label", names[i]}});
    if (selected >= names.size())
        options.append(QJsonObject {{"value", QString::number(selected)}, {"label", QString("GPU %1 (unavailable)").arg(selected)}});
    return options;
}
inline QJsonArray MicrophoneOptions(const QStringList& names, const QString& selected) {
    QJsonArray options {QJsonObject {{"value", ""}, {"label", "None"}}};
    QStringList seen;
    for (const auto& name: names) {
        if (name.isEmpty() || seen.contains(name)) continue;
        seen.append(name);
        options.append(QJsonObject {{"value", name}, {"label", name}});
    }
    if (!selected.isEmpty() && !seen.contains(selected))
        options.append(QJsonObject {{"value", selected}, {"label", selected + " (unavailable)"}});
    return options;
}
} // namespace PreferenceDevices
#endif
