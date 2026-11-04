#include "ownerPreferences.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QTemporaryDir>

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
	CHECK(OwnerPreferences::Write(s, before, error));
	CHECK(OwnerPreferences::Read(s) == before);
}
