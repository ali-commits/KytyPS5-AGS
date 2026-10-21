#include "cheatFile.h"
#include "gameLibrary.h"
#include "hostApi.h"
#include "hostSocket.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
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

static HostReply Call(HostApi& api, const QJsonObject& request) {
	std::optional<HostReply> reply;
	api.Handle(request, [&reply](HostReply r) { reply = std::move(r); });
	for (int i = 0; i < 300 && !reply.has_value(); ++i) {
		QTest::qWait(10);
	}
	if (!reply.has_value()) {
		QTest::qFail("HostApi did not reply", __FILE__, __LINE__);
		return {500, "application/json", {}, {}};
	}
	return *reply;
}

class LauncherHostTests: public QObject {
	Q_OBJECT
	QTemporaryDir m_dir;
	QString       m_old_cwd;
	GameLibrary   library;
	HostApi       api {&library, [] { return HostApi::Status {}; }};
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
		QString error;
		QVERIFY2(library.Load(nullptr, &error), qPrintable(error));
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
	void apiLibraryListsGames() {
		auto r   = Call(api, {{"method", "GET"}, {"path", "/kyty/v1/library"}});
		auto doc = QJsonDocument::fromJson(r.body).object();
		QCOMPARE(r.status, 200);
		QCOMPARE(doc["games"].toArray().size(), 2);
		QCOMPARE(doc["capabilities"].toArray()[0].toString(), QString("hid-tunnel-v1"));
	}
	void apiArtEtagAnd304() {
		const auto id = library.Games().first().id;
		auto       r  = Call(api, {{"method", "GET"}, {"path", "/kyty/v1/art/" + id + "/icon"}});
		QCOMPARE(r.status, 200);
		QCOMPARE(r.body, QByteArray("PNGDATA"));
		QCOMPARE(r.content_type, QByteArray("image/png"));
		auto again = Call(api, {{"method", "GET"},
		                        {"path", "/kyty/v1/art/" + id + "/icon"},
		                        {"if_none_match", QString::fromLatin1(r.etag)}});
		QCOMPARE(again.status, 304);
		QCOMPARE(
		    Call(api, {{"method", "GET"}, {"path", "/kyty/v1/art/" + id + "/background"}}).status,
		    404);
	}
	void apiRejectsTraversalAndUnknown() {
		QCOMPARE(
		    Call(api, {{"method", "GET"}, {"path", "/kyty/v1/art/../../etc/passwd/icon"}}).status,
		    404);
		QCOMPARE(Call(api, {{"method", "GET"},
		                    {"path", "/kyty/v1/games/" + QString(64, 'a') + "/settings"}})
		             .status,
		         404);
		QCOMPARE(Call(api, {{"method", "DELETE"}, {"path", "/kyty/v1/library"}}).status, 405);
	}
	void apiSettingsRevisionConflict() {
		const auto id   = library.Games().first().id;
		const auto path = "/kyty/v1/games/" + id + "/settings";
		auto       get =
		    QJsonDocument::fromJson(Call(api, {{"method", "GET"}, {"path", path}}).body).object();
		const auto rev = get["revision"].toString();
		QVERIFY(!get["fields"].toArray().isEmpty());
		auto put = Call(
		    api, {{"method", "PUT"},
		          {"path", path},
		          {"body", QJsonObject {{"revision", rev},
		                                {"values", QJsonObject {{"vblank_frequency", "90"}}}}}});
		QCOMPARE(put.status, 200);
		QVERIFY(QJsonDocument::fromJson(put.body).object()["revision"].toString() != rev);
		auto stale = Call(
		    api, {{"method", "PUT"},
		          {"path", path},
		          {"body", QJsonObject {{"revision", rev},
		                                {"values", QJsonObject {{"vblank_frequency", "60"}}}}}});
		QCOMPARE(stale.status, 409);
	}
	void apiSettingsRejectsInvalidValue() {
		const auto id   = library.Games().first().id;
		const auto path = "/kyty/v1/games/" + id + "/settings";
		const auto rev =
		    QJsonDocument::fromJson(Call(api, {{"method", "GET"}, {"path", path}}).body)
		        .object()["revision"]
		        .toString();
		auto r = Call(
		    api, {{"method", "PUT"},
		          {"path", path},
		          {"body", QJsonObject {{"revision", rev},
		                                {"values", QJsonObject {{"present_mode", "Sideways"}}}}}});
		QCOMPARE(r.status, 400);
		QVERIFY(!QJsonDocument::fromJson(r.body).object()["error"].toString().isEmpty());
	}
	void apiSettingsFieldsCoverEverySetting() {
		const auto id = library.Games().first().id;
		auto       get =
		    QJsonDocument::fromJson(
		        Call(api, {{"method", "GET"}, {"path", "/kyty/v1/games/" + id + "/settings"}}).body)
		        .object();
		QSet<QString> keys;
		for (const auto& f: get["fields"].toArray()) {
			const auto field = f.toObject();
			keys.insert(field["key"].toString());
			if (field["type"] == "enum") {
				QVERIFY2(field["choices"].toArray().contains(field["value"]),
				         qPrintable(field["key"].toString()));
			}
		}
		const QStringList path_keys {"elf", "shader_log_folder", "command_buffer_dump_folder",
		                             "printf_output_file"};
		for (const auto& key: library.Find(id)->info->GameSettings().keys()) {
			QCOMPARE(keys.contains(key), !path_keys.contains(key));
		}
		for (const auto& key: path_keys) {
			QVERIFY2(!keys.contains(key), qPrintable(key));
		}
	}
	void apiSettingsRejectsPathKeys() {
		const auto id   = library.Games().first().id;
		const auto path = "/kyty/v1/games/" + id + "/settings";
		const auto get =
		    QJsonDocument::fromJson(Call(api, {{"method", "GET"}, {"path", path}}).body).object();
		const auto before = library.Find(id)->info->GameSettings();
		for (const auto& [key, value]:
		     {std::pair<QString, QString> {"printf_output_file", "/tmp/x"},
		      std::pair<QString, QString> {"elf", "../evil"}}) {
			auto r = Call(api, {{"method", "PUT"},
			                    {"path", path},
			                    {"body", QJsonObject {{"revision", get["revision"]},
			                                          {"values", QJsonObject {{key, value}}}}}});
			QCOMPARE(r.status, 400);
			QCOMPARE(QJsonDocument::fromJson(r.body).object()["error"].toString(),
			         QString("read-only-setting"));
		}
		QCOMPARE(library.Find(id)->info->GameSettings(), before);
		auto again =
		    QJsonDocument::fromJson(Call(api, {{"method", "GET"}, {"path", path}}).body).object();
		QCOMPARE(again["revision"], get["revision"]);
	}
	void apiSettingsEnumUsesChoiceText() {
		const auto id   = library.Games().first().id;
		const auto path = "/kyty/v1/games/" + id + "/settings";
		auto       get =
		    QJsonDocument::fromJson(Call(api, {{"method", "GET"}, {"path", path}}).body).object();
		auto put = Call(
		    api,
		    {{"method", "PUT"},
		     {"path", path},
		     {"body", QJsonObject {{"revision", get["revision"]},
		                           {"values", QJsonObject {{"screen_resolution", "1920x1080"}}}}}});
		QCOMPARE(put.status, 200);
		get = QJsonDocument::fromJson(Call(api, {{"method", "GET"}, {"path", path}}).body).object();
		for (const auto& f: get["fields"].toArray()) {
			if (f.toObject()["key"] == "screen_resolution") {
				QCOMPARE(f.toObject()["value"].toString(), QString("1920x1080"));
			}
		}
	}
	void apiSettingsRefusedWhileRunning() {
		const auto id = library.Games().first().id;
		HostApi    running(&library, [id] { return HostApi::Status {true, id}; });
		const auto path = "/kyty/v1/games/" + id + "/settings";
		const auto rev =
		    QJsonDocument::fromJson(Call(running, {{"method", "GET"}, {"path", path}}).body)
		        .object()["revision"]
		        .toString();
		auto r =
		    Call(running, {{"method", "PUT"},
		                   {"path", path},
		                   {"body", QJsonObject {{"revision", rev}, {"values", QJsonObject {}}}}});
		QCOMPARE(r.status, 409);
		QCOMPARE(QJsonDocument::fromJson(r.body).object()["error"].toString(),
		         QString("game-running"));
	}
	void apiCheatSelection() {
		const auto plan = Cheats::PlanPath("PPSA00001");
		WriteFile(plan, R"({"id":"PPSA00001","version":"01.002","process":"eboot.bin","mods":[)"
		                R"({"name":"Inf HP","memory":[{"offset":"0x10","off":"00","on":"01"}]},)"
		                R"({"name":"Inf MP","memory":[{"offset":"0x20","off":"00","on":"01"}]}]})");
		const auto id = library.Games().first().id;
		QCOMPARE(library.Find(id)->info->title_id, QString("PPSA00001"));
		const auto path = "/kyty/v1/games/" + id + "/cheats";
		auto       get =
		    QJsonDocument::fromJson(Call(api, {{"method", "GET"}, {"path", path}}).body).object();
		QCOMPARE(get["mods"].toArray().size(), 2);
		auto put = Call(api, {{"method", "PUT"},
		                      {"path", path},
		                      {"body", QJsonObject {{"revision", get["revision"]},
		                                            {"enabled", QJsonArray {true, false}}}}});
		QCOMPARE(put.status, 200);
		auto after =
		    QJsonDocument::fromJson(Call(api, {{"method", "GET"}, {"path", path}}).body).object();
		QVERIFY(after["mods"].toArray()[0].toObject()["enabled"].toBool());
		QVERIFY(!after["mods"].toArray()[1].toObject()["enabled"].toBool());
		auto stale = Call(api, {{"method", "PUT"},
		                        {"path", path},
		                        {"body", QJsonObject {{"revision", get["revision"]},
		                                              {"enabled", QJsonArray {true, true}}}}});
		QCOMPARE(stale.status, 409);
		auto bad = Call(api, {{"method", "PUT"},
		                      {"path", path},
		                      {"body", QJsonObject {{"revision", after["revision"]},
		                                            {"enabled", QJsonArray {true}}}}});
		QCOMPARE(bad.status, 400);
		auto unknown = Call(api, {{"method", "POST"},
		                          {"path", path + "/remote/fetch"},
		                          {"body", QJsonObject {{"name", "missing.json"}}}});
		QCOMPARE(unknown.status, 404);
		QFile::remove(plan);
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
