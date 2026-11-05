#include "ownerPreferences.h"
#include "preferenceSchema.h"
#include "preferenceDevices.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QSet>
#include <cstdio>
#include <cstdlib>

#define CHECK(expression)                                                                          \
	do {                                                                                           \
		if (!(expression)) { fprintf(stderr, "Check failed at line %d: %s\n", __LINE__, #expression); abort(); }           \
	} while (false)
int main(int argc, char** argv) {
    const auto gpuOptions = PreferenceDevices::GpuOptions({"Detected GPU"}, 5);
    CHECK(gpuOptions.size() == 3);
    CHECK(gpuOptions[0].toObject()["value"] == "-1");
    CHECK(gpuOptions[1].toObject()["label"] == "Detected GPU");
    CHECK(gpuOptions[2].toObject()["value"] == "5");
    CHECK(PreferenceDevices::GpuOptions({}, -1).size() == 1);
    const auto microphoneOptions = PreferenceDevices::MicrophoneOptions({"USB Mic", "USB Mic", ""}, "Saved Mic");
    CHECK(microphoneOptions.size() == 3);
    CHECK(microphoneOptions[0].toObject()["value"] == "");
    CHECK(microphoneOptions[1].toObject()["value"] == "USB Mic");
    CHECK(microphoneOptions[2].toObject()["value"] == "Saved Mic");
    CHECK(PreferenceDevices::MicrophoneOptions({}, "Saved Mic").size() == 2);
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
    const auto stored = [](QJsonObject response) { response.remove("fields"); return response; };
	CHECK(before["settings"].toObject()["user_name"] == "Owner");
	CHECK(before["global"].toObject()["audio_master_volume"] == 80);
	CHECK(s.value("Launcher/unrelated") == "preserve");
	CHECK(s.value("GameConfigurations/size").toInt() == 2);
	CHECK(!OwnerPreferences::Write(s,
	                               {{"settings", QJsonObject {{"user_name", "Changed"}}},
	                                {"global", QJsonObject {{"audio_master_volume", -1}}}},
	                               error));
	CHECK(stored(OwnerPreferences::Read(s)) == stored(before));
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
        if (key == "audio_master_volume") {
            CHECK(field["max"].toInt() == Config::MAX_AUDIO_VOLUME);
            CHECK(field["control"] == "range");
            CHECK(field["unit"] == "%");
        }
        if (key == "gpu_index" || key == "audio_input_device") CHECK(!field["options"].toArray().isEmpty());
        if (key == "controller_keyboard_player") {
            CHECK(field["options"].toArray().size() == Config::MAX_PLAYERS + 1);
            CHECK(field["options"].toArray()[0].toObject()["value"].isDouble());
        }
        if (key == "console_language") CHECK(field["options"].toArray().size() == Configuration::MAX_CONSOLE_LANGUAGE + 1);
    }
    CHECK(declared.contains("readback_tiled_images"));
    CHECK(declared.contains("present_mode"));
    CHECK(!OwnerPreferences::Write(s, {{"fields", fields}}, error));
    auto writable = before;
    writable.remove("fields");
    CHECK(OwnerPreferences::Write(s, writable, error));
	CHECK(stored(OwnerPreferences::Read(s)) == stored(before));
}
