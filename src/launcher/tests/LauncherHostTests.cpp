#include "gameLibrary.h"

#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

static void WriteFile(const QString& path, const QByteArray& data) {
	QDir().mkpath(QFileInfo(path).absolutePath());
	QFile f(path);
	QVERIFY(f.open(QIODevice::WriteOnly));
	f.write(data);
}

class LauncherHostTests: public QObject {
	Q_OBJECT
	QTemporaryDir m_dir;
	QString       m_old_cwd;
	void          MakeGame(const QString& folder, const QByteArray& title_id) {
		WriteFile(m_dir.filePath("games/" + folder + "/eboot.bin"), "elf");
		WriteFile(
		    m_dir.filePath("games/" + folder + "/sce_sys/param.json"),
		    R"({"titleId":")" + title_id +
		        R"(","appVersion":"01.002",)"
		        R"("localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"Game )" +
		        title_id + R"("}}})");
		WriteFile(m_dir.filePath("games/" + folder + "/sce_sys/icon0.png"), "PNGDATA");
	}
private slots:
	void init() {
		m_old_cwd = QDir::currentPath();
		QDir::setCurrent(m_dir.path());
		WriteFile(m_dir.filePath("Kyty.ini"),
		          "[Launcher]\ngame_dirs=" + m_dir.filePath("games").toUtf8() + "\n");
		MakeGame("A", "PPSA00001");
		MakeGame("nested/B", "PPSA00002");
	}
	void cleanup() { QDir::setCurrent(m_old_cwd); }
	void loadsNestedGamesWithStableIds() {
		GameLibrary library;
		QString     error;
		QVERIFY2(library.Load(nullptr, &error), qPrintable(error));
		QCOMPARE(library.Games().size(), 2);
		const auto  path = GameLibraryFiles::NormalizeGameDirectory(m_dir.filePath("games/A"));
		const auto* a    = library.Find(GameLibraryFiles::CatalogId(path));
		QVERIFY(a != nullptr);
		QCOMPARE(a->info->title_id, QStringLiteral("PPSA00001"));
		QCOMPARE(a->info->gameVersion, QStringLiteral("01.002"));
		QCOMPARE(a->info->name, QStringLiteral("Game PPSA00001"));
		QCOMPARE(a->id.size(), 64);
		QVERIFY(!a->custom);
	}
	void savesCustomSettingsAndReloads() {
		GameLibrary library;
		QString     error;
		QVERIFY(library.Load(nullptr, &error));
		const auto id       = library.Games().first().id;
		auto       settings = QJsonObject::fromVariantMap(library.Find(id)->info->GameSettings());
		settings["vblank_frequency"] = "120";
		QCOMPARE(library.SaveGameSettings(id, settings), QString());
		GameLibrary reloaded;
		QVERIFY(reloaded.Load(nullptr, &error));
		QVERIFY(reloaded.Find(id)->custom);
		QCOMPARE(reloaded.Find(id)->info->GameSettings().value("vblank_frequency").toString(),
		         QStringLiteral("120"));
	}
	void rejectsInvalidSettings() {
		GameLibrary library;
		QString     error;
		QVERIFY(library.Load(nullptr, &error));
		const auto id       = library.Games().first().id;
		auto       settings = QJsonObject::fromVariantMap(library.Find(id)->info->GameSettings());
		settings["present_mode"] = "Sideways";
		QVERIFY(!library.SaveGameSettings(id, settings).isEmpty());
		QVERIFY(!library.SaveGameSettings(QString(64, 'f'), settings).isEmpty());
	}
	void catalogMatchesIds() {
		GameLibrary library;
		QString     error;
		QVERIFY(library.Load(nullptr, &error));
		const auto catalog = library.Catalog(m_dir.filePath("kyty_emulator"));
		QCOMPARE(catalog.size(), 2);
		QVERIFY(library.Find(catalog.first().id) != nullptr);
		QVERIFY(catalog.first().image_path.endsWith("sce_sys/icon0.png"));
	}
};

QTEST_MAIN(LauncherHostTests)
#include "LauncherHostTests.moc"
