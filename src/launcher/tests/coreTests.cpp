// Checks for the explicit roots of kyty_launcher_core: settings file path and cheat plan path.
// Plain asserts, no framework.
#include "cheatFile.h"
#include "configuration.h"
#include "gameLibrary.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdio>
#include <cstdlib>

#define CHECK(expression)                                                                          \
	do {                                                                                           \
		if (!(expression)) {                                                                       \
			fprintf(stderr, "Check failed at line %d: %s\n", __LINE__, #expression);               \
			abort();                                                                               \
		}                                                                                          \
	} while (false)

static void WriteFile(const QString& path, const QByteArray& data) {
	CHECK(QDir().mkpath(QFileInfo(path).absolutePath()));
	QFile file(path);
	CHECK(file.open(QIODevice::WriteOnly));
	CHECK(file.write(data) == data.size());
}

static void OpenSettingsReadsTheGivenFile() {
	QTemporaryDir dir;
	CHECK(dir.isValid());
	const auto ini = dir.filePath(QStringLiteral("custom.ini"));
	{
		QSettings out(ini, QSettings::IniFormat);
		out.setValue(QStringLiteral("Launcher/marker"), QStringLiteral("explicit"));
		out.sync();
	}
	const auto in = GameLibraryFiles::OpenSettings(ini);
	CHECK(QFileInfo(in->fileName()) == QFileInfo(ini));
	CHECK(in->value(QStringLiteral("Launcher/marker")).toString() == QStringLiteral("explicit"));

	// An empty path keeps the standalone lookup: ./Kyty.ini when present, else the user config.
	const auto cwd = QDir::currentPath();
	CHECK(QDir::setCurrent(dir.path()));
	WriteFile(dir.filePath(QStringLiteral("Kyty.ini")), "[Launcher]\nmarker=cwd\n");
	const auto local = GameLibraryFiles::OpenSettings(QString {});
	CHECK(local->value(QStringLiteral("Launcher/marker")).toString() == QStringLiteral("cwd"));
	CHECK(QDir::setCurrent(cwd));
}

static void GameLibraryLoadsFromAnExplicitFile() {
	QTemporaryDir dir;
	CHECK(dir.isValid());
	const auto games = dir.filePath(QStringLiteral("games"));
	WriteFile(games + QStringLiteral("/Fixture/eboot.bin"), "x");
	WriteFile(games + QStringLiteral("/Fixture/sce_sys/param.json"),
	          R"({"titleId":"PPSA99997","appVersion":"01.000.000"})");
	const auto ini = dir.filePath(QStringLiteral("runtime/Kyty.ini"));
	CHECK(QDir().mkpath(dir.filePath(QStringLiteral("runtime"))));
	{
		QSettings out(ini, QSettings::IniFormat);
		out.setValue(QStringLiteral("Launcher/game_dirs"), QStringList {games});
		out.sync();
	}

	GameLibrary library;
	library.SetSettingsPath(ini);
	QString error;
	CHECK(library.Load(nullptr, &error));
	CHECK(QFileInfo(library.SettingsFile()) == QFileInfo(ini));
	CHECK(library.Games().size() == 1);
	CHECK(library.Games().first().info->title_id == QStringLiteral("PPSA99997"));
	CHECK(library.Games().first().id == GameLibraryFiles::CatalogId(
	          library.Games().first().info->game_path));

	// Saving per-game settings rewrites the same explicit file.
	CHECK(library.SaveGameSettings(library.Games().first().id, {{"user_name", "Tester"}}).isEmpty());
	QSettings check(ini, QSettings::IniFormat);
	CHECK(check.beginReadArray(QString::fromLatin1(GameLibraryFiles::CONF_SECTION_NAME)) == 1);
}

static void PlanPathUsesTheGivenRoot() {
	QTemporaryDir dir;
	CHECK(dir.isValid());
	const auto path = Cheats::PlanPath(dir.path(), QStringLiteral(" ppsa99997 "));
	CHECK(path == QDir(dir.path()).filePath(QStringLiteral("_Patches/PPSA99997.json")));
	CHECK(Cheats::PlanPath(dir.path(), QStringLiteral("CUSA00001")).isEmpty());
	// The single-argument form keeps the executable directory as the root.
	CHECK(Cheats::PlanPath(QStringLiteral("PPSA99997")) ==
	      QDir(QCoreApplication::applicationDirPath())
	          .filePath(QStringLiteral("_Patches/PPSA99997.json")));
}

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	OpenSettingsReadsTheGivenFile();
	GameLibraryLoadsFromAnExplicitFile();
	PlanPathUsesTheGivenRoot();
	puts("core checks passed");
	return 0;
}
