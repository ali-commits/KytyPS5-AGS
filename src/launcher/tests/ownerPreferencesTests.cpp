#include "ownerPreferences.h"
#include "preferenceSchema.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QSet>

#define CHECK(expression)                                                                          \
	do {                                                                                           \
		if (!(expression)) qFatal("Check failed at line %d: %s", __LINE__, #expression);           \
	} while (false)
int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	QTemporaryDir    dir;
	QSettings        s(dir.filePath("settings.ini"), QSettings::IniFormat);
	s.setValue("Launcher/unrelated", "preserve");
	s.setValue("GameConfigurations/size", 2);
	QString error;
	CHECK(OwnerPreferences::Write(s,
	                              {{"settings", QJsonObject {{"user_name", "Owner"}}},
	                               {"global", QJsonObject {{"audio_master_volume", 80}}},
	                               {"game_dirs", QJsonArray {dir.path()}},
	                               {"host_input_mapping", QJsonArray {"cross=Space"}}},
	                              error));
	const auto before = OwnerPreferences::Read(s);
	CHECK(before["settings"].toObject()["user_name"] == "Owner");
	CHECK(before["global"].toObject()["audio_master_volume"] == 80);
	CHECK(s.value("Launcher/unrelated") == "preserve");
	CHECK(s.value("GameConfigurations/size").toInt() == 2);
	CHECK(!OwnerPreferences::Write(s,
	                               {{"settings", QJsonObject {{"user_name", "Changed"}}},
	                                {"global", QJsonObject {{"audio_master_volume", -1}}}},
	                               error));
	CHECK(OwnerPreferences::Read(s) == before);
	CHECK(!OwnerPreferences::Write(s, {{"game_dirs", QJsonArray {"relative"}}}, error));
	CHECK(!OwnerPreferences::Write(s, {{"unknown", true}}, error));
	CHECK(!OwnerPreferences::Write(s, {{"global", QJsonObject {{"audio_objects_enabled", "true"}}}},
	                               error));
	const auto fields = before["fields"].toArray();
    CHECK(!fields.isEmpty());
    QSet<QString> declared;
    for (const auto& value: fields) {
        const auto field = value.toObject();
        const auto key = field["key"].toString();
        declared.insert(key);
        CHECK(field["scope"] == "settings" || field["scope"] == "global");
        if (key == "readback_tiled_images" || key == "audio_objects_enabled") CHECK(field["type"] == "bool");
        if (key == "present_mode") CHECK(field["choices"].toArray() == QJsonArray::fromStringList(EnumToList<Configuration::PresentMode>()));
        if (key == "shader_optimization_type") CHECK(field["choices"].toArray() == QJsonArray::fromStringList(EnumToList<Configuration::ShaderOptimizationType>()));
        if (key == "audio_master_volume") CHECK(field["max"].toInt() == Config::MAX_AUDIO_VOLUME);
        if (key == "console_language") CHECK(field["options"].toArray().size() == Configuration::MAX_CONSOLE_LANGUAGE + 1);
    }
    CHECK(declared.contains("readback_tiled_images"));
    CHECK(declared.contains("present_mode"));
    CHECK(!OwnerPreferences::Write(s, {{"fields", fields}}, error));
    auto writable = before;
    writable.remove("fields");
    CHECK(OwnerPreferences::Write(s, writable, error));
	CHECK(OwnerPreferences::Read(s) == before);
}
