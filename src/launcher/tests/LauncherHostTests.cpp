#include "gameLibrary.h"
#include "hostSocket.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <sys/stat.h>
#include <thread>

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
	void partialBodyKeepsSavedFields() {
		GameLibrary library;
		QString     error;
		QVERIFY(library.Load(nullptr, &error));
		const auto  id = library.Games().first().id;
		QJsonObject first;
		first["vblank_frequency"] = "120";
		QCOMPARE(library.SaveGameSettings(id, first), QString());
		QJsonObject second;
		second["trophy_duration_seconds"] = "7";
		QCOMPARE(library.SaveGameSettings(id, second), QString());
		GameLibrary reloaded;
		QVERIFY(reloaded.Load(nullptr, &error));
		const auto saved = reloaded.Find(id)->info->GameSettings();
		QCOMPARE(saved.value("vblank_frequency").toString(), QStringLiteral("120"));
		QCOMPARE(saved.value("trophy_duration_seconds").toString(), QStringLiteral("7"));
	}
	void elfRoundTrips() {
		GameLibrary library;
		QString     error;
		QVERIFY(library.Load(nullptr, &error));
		const auto  id = library.Games().first().id;
		QJsonObject body;
		body["elf"] = "other/game.elf";
		QCOMPARE(library.SaveGameSettings(id, body), QString());
		QCOMPARE(library.Find(id)->info->elf, QStringLiteral("other/game.elf"));
		GameLibrary reloaded;
		QVERIFY(reloaded.Load(nullptr, &error));
		QCOMPARE(reloaded.Find(id)->info->GameSettings().value("elf").toString(),
		         QStringLiteral("other/game.elf"));
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
	void socketRoundTripAndPermissions() {
		QTemporaryDir    dir;
		const auto       path = dir.filePath("kyty-host/api.sock");
		HostSocketServer server([](const QJsonObject& req, auto done) {
			done(
			    {200, "application/json", QJsonDocument(req).toJson(QJsonDocument::Compact), "e1"});
		});
		QString          error;
		QVERIFY2(server.Listen(path, &error), qPrintable(error));
		struct stat st {};
		QVERIFY(::stat(QFile::encodeName(path).constData(), &st) == 0);
		QCOMPARE(st.st_mode & 0777, mode_t {0600});
		QVERIFY(::stat(QFile::encodeName(dir.filePath("kyty-host")).constData(), &st) == 0);
		QCOMPARE(st.st_mode & 0777, mode_t {0700});
		std::optional<HostReply> reply;
		std::atomic<bool>        finished {false};
		QString                  client_error;
		std::thread              t([&] {
			reply    = HostSocketCall(path, {{"type", "status"}}, 2000, &client_error);
			finished = true;
		});
		QTRY_VERIFY_WITH_TIMEOUT(finished.load(), 3000);
		t.join();
		QVERIFY2(reply.has_value(), qPrintable(client_error));
		QCOMPARE(reply->status, 200);
		QCOMPARE(reply->etag, QByteArray("e1"));
		QCOMPARE(QJsonDocument::fromJson(reply->body).object()["type"].toString(),
		         QString("status"));
	}
	void socketRejectsOversizeRequest() {
		QTemporaryDir    dir;
		HostSocketServer server([](const QJsonObject&, auto done) { done({}); });
		QString          error;
		QVERIFY(server.Listen(dir.filePath("s/api.sock"), &error));
		QLocalSocket s;
		s.connectToServer(dir.filePath("s/api.sock"));
		QVERIFY(s.waitForConnected(1000));
		s.write(QByteArray(70 * 1024, 'x'));
		QTRY_COMPARE_WITH_TIMEOUT(s.state(), QLocalSocket::UnconnectedState, 3000);
	}
};

QTEST_MAIN(LauncherHostTests)
#include "LauncherHostTests.moc"
