#include "page.h"

#include "settings.h"

namespace AdapterPage {

namespace {

// Copies the keys of `field` named in `keys` that it has.
void CopyKeys(const QJsonObject& field, QJsonObject& item, std::initializer_list<const char*> keys) {
	for (const auto* key: keys) {
		const auto k = QString::fromLatin1(key);
		if (field.contains(k)) item[k] = field[k];
	}
}

QJsonObject Section(const QJsonObject& heading, PageKind kind) {
	const auto  id = QStringLiteral("kyty.") + heading[QStringLiteral("key")].toString().mid(6);
	QJsonObject section {{"id", id},
	                     {"surface", "panel"},
	                     {"title", heading[QStringLiteral("label")]},
	                     {"icon", heading[QStringLiteral("icon")]},
	                     {"items", QJsonArray {}}};
	if (kind == PageKind::Game) {
		if (id == QLatin1String("kyty.display")) {
			section[QStringLiteral("tab")]   = "display";
			section[QStringLiteral("title")] = "KytyPS5";
		} else {
			section[QStringLiteral("tab")] = id == QLatin1String("kyty.debug") ? "advanced" : "source";
		}
	}
	return section;
}

} // namespace

QJsonObject Item(const QJsonObject& field, const QString& scope) {
	const auto  type = field[QStringLiteral("type")].toString();
	const auto  key  = field[QStringLiteral("key")].toString();
	QJsonObject item {{"id", key},
	                  {"label", field[QStringLiteral("label")]},
	                  {"scope", scope},
	                  {"default", field[QStringLiteral("default")]}};
	if (field.contains(QStringLiteral("hint"))) item[QStringLiteral("help")] = field[QStringLiteral("hint")];
	if (type == QLatin1String("bool")) {
		item[QStringLiteral("kind")] = "switch";
	} else if (type == QLatin1String("enum")) {
		item[QStringLiteral("kind")]    = "choice";
		item[QStringLiteral("options")] = field[QStringLiteral("options")];
		if (field[QStringLiteral("control")] == QLatin1String("segmented"))
			item[QStringLiteral("presentation")] = "segmented";
	} else if (type == QLatin1String("int")) {
		item[QStringLiteral("kind")] =
		    field[QStringLiteral("control")] == QLatin1String("slider") ? "slider" : "number";
		CopyKeys(field, item, {"min", "max", "step", "unit"});
	} else if (type == QLatin1String("string") &&
	           field[QStringLiteral("format")] == QLatin1String("colour")) {
		item[QStringLiteral("kind")] = "colour";
		CopyKeys(field, item, {"nullable", "null_label", "palette"});
	} else if (type == QLatin1String("path")) {
		item[QStringLiteral("kind")]       = "path";
		item[QStringLiteral("pick")]       = "folder";
		item[QStringLiteral("nullable")]   = true;
		item[QStringLiteral("null_label")] = key == QLatin1String("runtime_root") ? "Default" : "None";
	} else if (type == QLatin1String("path_list")) {
		item[QStringLiteral("kind")]  = "list";
		item[QStringLiteral("entry")] = "path";
		item[QStringLiteral("pick")]  = "folder";
	} else if (type == QLatin1String("string_list")) {
		item[QStringLiteral("kind")]  = "list";
		item[QStringLiteral("entry")] = "text";
		CopyKeys(field, item, {"max_items", "max_len"});
	} else {
		item[QStringLiteral("kind")] = "text";
	}
	return item;
}

QJsonArray SettingsSections(PageKind kind) {
	QJsonArray  sections;
	QJsonObject section;
	const auto  flush = [&] {
        if (!section.isEmpty() && !section[QStringLiteral("items")].toArray().isEmpty())
            sections.append(section);
	};
	for (const auto& v: AdapterSettings::Schema()) {
		const auto field = v.toObject();
		if (field[QStringLiteral("type")] == QLatin1String("group")) {
			flush();
			section = Section(field, kind);
			continue;
		}
		QString scope;
		if (kind == PageKind::Extension) {
			scope = field[QStringLiteral("location")] == QLatin1String("machine") ? "machine" : "device";
		} else if (field[QStringLiteral("scope")] == QLatin1String("both")) {
			scope = QStringLiteral("game_on_device"); // the Kyty.ini fields only
		} else {
			continue;
		}
		auto items = section[QStringLiteral("items")].toArray();
		items.append(Item(field, scope));
		section[QStringLiteral("items")] = items;
	}
	flush();
	return sections;
}

QJsonObject Describe(const GameEntry* game, const QString& /*runtime_root*/,
                     const QString& /*data_dir*/) {
	if (game == nullptr) return {{"sections", SettingsSections(PageKind::Extension)}};
	return {{"sections", QJsonArray {}}}; // shortcut: no game page items yet, Task 4 fills them
}

} // namespace AdapterPage
