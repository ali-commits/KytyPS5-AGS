#include "agsPairing.h"
#include "cheatFile.h"
#include "gameLibrary.h"
#include "hostApi.h"
#include "hostDaemon.h"
#include "hostSocket.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

static void WriteFile(const QString& path, const QByteArray& data) {
	QDir().mkpath(QFileInfo(path).absolutePath());
	QFile f(path);
	QVERIFY(f.open(QIODevice::WriteOnly));
	f.write(data);
}

static QByteArray ReadFile(const QString& path) {
	QFile f(path);
	return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray {};
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
	void apiBackgroundIsDownscaledJpeg() {
		const auto id   = library.Games().first().id;
		const auto path = QFileInfo(library.Games().first().info->basedir).absoluteFilePath() +
		                  "/sce_sys/pic0.png";
		QImage     big(3840, 2160, QImage::Format_RGB32);
		big.fill(Qt::blue);
		QVERIFY(big.save(path, "PNG"));
		auto r = Call(api, {{"method", "GET"}, {"path", "/kyty/v1/art/" + id + "/background"}});
		QCOMPARE(r.status, 200);
		QCOMPARE(r.content_type, QByteArray("image/jpeg"));
		QImage sent;
		QVERIFY(sent.loadFromData(r.body));
		QCOMPARE(sent.width(), 1920);
		QCOMPARE(sent.height(), 1080);
		auto again = Call(api, {{"method", "GET"},
		                        {"path", "/kyty/v1/art/" + id + "/background"},
		                        {"if_none_match", QString::fromLatin1(r.etag)}});
		QCOMPARE(again.status, 304);
		QFile::remove(path);
	}
	void apiTrophyIconRoute() {
		const auto id   = library.Games().first().id;
		const auto base = "/kyty/v1/games/" + id + "/trophies/icon/";
		// Routed (not "not-found"), but this fixture game has no trophy package.
		const auto none = Call(api, {{"method", "GET"}, {"path", base + "0-1"}});
		QCOMPARE(none.status, 404);
		QCOMPARE(QJsonDocument::fromJson(none.body).object()["error"].toString(),
		         QString("no-icon"));
		QCOMPARE(Call(api, {{"method", "GET"}, {"path", base + "x-1"}}).status, 404);
		QCOMPARE(
		    Call(api, {{"method", "PUT"}, {"path", base + "0-1"}, {"body", QJsonObject {}}}).status,
		    405);
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
	void localEmulatorLockDetection() {
		QTemporaryDir run;
		qputenv("XDG_RUNTIME_DIR", run.path().toUtf8());
		QVERIFY(!HostDaemon::LocalEmulatorRunning());
		QDir().mkpath(run.filePath("kytyps5"));
		const int fd = ::open(QFile::encodeName(run.filePath("kytyps5/emulator.lock")),
		                      O_CREAT | O_RDWR, 0600);
		QVERIFY(fd >= 0);
		QCOMPARE(::flock(fd, LOCK_EX | LOCK_NB), 0);
		QVERIFY(HostDaemon::LocalEmulatorRunning());
		::close(fd);
		QVERIFY(!HostDaemon::LocalEmulatorRunning());
	}
	void settingsRevisionSurvivesReload() {
		const auto id   = library.Games().first().id;
		const auto path = "/kyty/v1/games/" + id + "/settings";
		const auto rev =
		    QJsonDocument::fromJson(Call(api, {{"method", "GET"}, {"path", path}}).body)
		        .object()["revision"]
		        .toString();
		const auto put = Call(
		    api, {{"method", "PUT"},
		          {"path", path},
		          {"body", QJsonObject {{"revision", rev},
		                                {"values", QJsonObject {{"vblank_frequency", "90"}}}}}});
		QCOMPARE(put.status, 200);
		const auto put_revision = QJsonDocument::fromJson(put.body).object()["revision"].toString();
		QVERIFY(!put_revision.isEmpty());
		GameLibrary reloaded; // what the daemon's watcher does 500 ms after every save
		QString     error;
		QVERIFY2(reloaded.Load(nullptr, &error), qPrintable(error));
		QCOMPARE(HostApi::SettingsRevision(*reloaded.Find(id)->info), put_revision);
	}
	void retryBackoffDoublesToCap() {
		QCOMPARE(HostDaemon::NextRetryMs(0), 5000);
		QCOMPARE(HostDaemon::NextRetryMs(5000), 10000);
		QCOMPARE(HostDaemon::NextRetryMs(10000), 20000);
		QCOMPARE(HostDaemon::NextRetryMs(20000), 40000);
		QCOMPARE(HostDaemon::NextRetryMs(40000), 60000);
		QCOMPARE(HostDaemon::NextRetryMs(60000), 60000);
	}
	void restartIsDeferredWhileAGameStreams() {
		using S = StreamingBackend::State;
		using R = HostDaemon::RestartStep;
		QCOMPARE(HostDaemon::PlanRestart(S::Ready, true), R::Wait);
		QCOMPARE(HostDaemon::PlanRestart(S::Ready, false), R::StopNow);
		QCOMPARE(HostDaemon::PlanRestart(S::Starting, false), R::StopNow);
		QCOMPARE(HostDaemon::PlanRestart(S::Stopping, false), R::Wait);
		QCOMPARE(HostDaemon::PlanRestart(S::Stopped, false), R::StartNow);
		QCOMPARE(HostDaemon::PlanRestart(S::Error, false), R::StartNow);
	}
	void errorSchedulesRetryWithBackoff() {
		QTemporaryDir run, profile;
		qputenv("XDG_RUNTIME_DIR", run.path().toUtf8());
		qputenv("KYTY_STREAMING_CONFIG_DIR", profile.path().toUtf8());
		qputenv("KYTY_HOST_NO_AUTOSTART", "1");
		WriteFile(profile.filePath("streaming.ini"),
		          "[host]\nbackend=/nonexistent/kyty_stream_host\n");
		HostDaemon daemon;
		QString    error;
		QVERIFY2(daemon.Start(&error), qPrintable(error));
		QVERIFY(!daemon.RetryScheduled());
		qunsetenv("KYTY_HOST_NO_AUTOSTART"); // a preference change now starts hosting, which fails
		const auto sock  = run.filePath("kyty-host/control.sock");
		auto       prefs = Control(sock, {{"type", "get-preferences"}});
		prefs["type"]    = "set-preferences";
		prefs["fps"]     = 30;
		QVERIFY(Control(sock, prefs)["ok"].toBool());
		QTRY_VERIFY_WITH_TIMEOUT(daemon.RetryScheduled(), 5000);
		QCOMPARE(daemon.RetryIntervalMs(), 5000);
		QCOMPARE(Control(sock, {{"type", "status"}})["state"].toString(), QString("Error"));
		qputenv("KYTY_HOST_NO_AUTOSTART", "1");
		daemon.Shutdown();
	}
	void secondDaemonIsRefused() {
		QTemporaryDir run, profile;
		qputenv("XDG_RUNTIME_DIR", run.path().toUtf8());
		qputenv("KYTY_STREAMING_CONFIG_DIR", profile.path().toUtf8());
		qputenv("KYTY_HOST_NO_AUTOSTART", "1");
		HostDaemon first;
		QString    error;
		QVERIFY2(first.Start(&error), qPrintable(error));
		const auto sock = run.filePath("kyty-host/control.sock");
		{
			HostDaemon second;
			QVERIFY(!second.Start(&error));
			QVERIFY2(error.contains("already running"), qPrintable(error));
		}
		QCOMPARE(Control(sock, {{"type", "status"}})["state"].toString(), QString("Stopped"));
		first.Shutdown();
	}
	static QJsonObject Control(const QString& sock, const QJsonObject& request) {
		std::optional<HostReply> reply;
		QString                  error;
		std::thread              t([&] { reply = HostSocketCall(sock, request, 2000, &error); });
		for (int i = 0; i < 300 && !reply.has_value(); ++i)
			QTest::qWait(10);
		t.join();
		return reply.has_value() ? QJsonDocument::fromJson(reply->body).object() : QJsonObject {};
	}
	void controlSocketStatusAndPreferences() {
		QTemporaryDir run, profile;
		qputenv("XDG_RUNTIME_DIR", run.path().toUtf8());
		qputenv("KYTY_STREAMING_CONFIG_DIR", profile.path().toUtf8());
		qputenv("KYTY_HOST_NO_AUTOSTART", "1"); // daemon does not start hosting in tests
		HostDaemon daemon;
		QString    error;
		QVERIFY2(daemon.Start(&error), qPrintable(error));
		const auto sock   = run.filePath("kyty-host/control.sock");
		const auto status = Control(sock, {{"type", "status"}});
		QCOMPARE(status["state"].toString(), QString("Stopped"));
		QVERIFY(status["hid"].isObject());
		QVERIFY(!status["local_game_running"].toBool());
		QVERIFY(!Control(sock,
		                 {{"type", "pairing-pin"}, {"id", "x"}, {"pin", "12"}, {"name", "n"}})["ok"]
		             .toBool());
		QVERIFY(QFileInfo::exists(run.filePath("kyty-host/api.sock")));
		auto prefs = Control(sock, {{"type", "get-preferences"}});
		QCOMPARE(prefs["port"].toInt(), 48189);
		QCOMPARE(prefs["codec"].toString(), QString("HEVC"));
		prefs["type"] = "set-preferences";
		prefs["fps"]  = 30;
		QVERIFY(Control(sock, prefs)["ok"].toBool());
		QCOMPARE(Control(sock, {{"type", "get-preferences"}})["fps"].toInt(), 30);
		QSettings file(profile.filePath("streaming.ini"), QSettings::IniFormat);
		QCOMPARE(file.value("quality/fps").toInt(), 30);
		prefs["port"] = 99;
		QVERIFY(!Control(sock, prefs)["ok"].toBool());
		QCOMPARE(Control(sock, {{"type", "get-preferences"}})["port"].toInt(), 48189);
		QVERIFY(!Control(sock, {{"type", "nonsense"}})["ok"].toBool());
		daemon.Shutdown();
	}

	// ---- Per-device (per paired client) profiles ----

	static QJsonObject ClientReq(const QString& method, const QString& path, const QString& uuid,
	                             const QJsonObject& body = {}) {
		QJsonObject req {{"method", method},
		                 {"path", path},
		                 {"client", QJsonObject {{"id", uuid}, {"name", "Phone"}}}};
		if (!body.isEmpty() || method != "GET") {
			req["body"] = body;
		}
		return req;
	}
	static QString SettingsValue(const QJsonObject& get, const QString& key) {
		for (const auto& f: get["fields"].toArray()) {
			if (f.toObject()["key"] == key) {
				return f.toObject()["value"].toString();
			}
		}
		return {};
	}

	void clientGetSeedsFromPcAndPutIsolatesFromPc() {
		QTemporaryDir config;
		qputenv("KYTY_STREAMING_CONFIG_DIR", config.path().toUtf8());
		const QString uuid = "11111111-1111-1111-1111-111111111111";
		const auto    id   = library.Games().first().id;
		const auto    path = "/kyty/v1/games/" + id + "/settings";
		// Device GET seeds from the PC's current settings (same field values), scope=device.
		auto get = QJsonDocument::fromJson(Call(api, ClientReq("GET", path, uuid)).body).object();
		QCOMPARE(get["scope"].toString(), QString("device"));
		QCOMPARE(SettingsValue(get, "vblank_frequency"), QString("60"));
		// Device PUT changes only this client's profile.
		auto put = Call(api, ClientReq("PUT", path, uuid,
		                               {{"revision", get["revision"]},
		                                {"values", QJsonObject {{"vblank_frequency", "120"}}}}));
		QCOMPARE(put.status, 200);
		auto after = QJsonDocument::fromJson(Call(api, ClientReq("GET", path, uuid)).body).object();
		QCOMPARE(SettingsValue(after, "vblank_frequency"), QString("120"));
		// The PC config is untouched (unscoped GET still reads 60).
		auto pc =
		    QJsonDocument::fromJson(Call(api, {{"method", "GET"}, {"path", path}}).body).object();
		QCOMPARE(pc["scope"].toString(), QString("pc"));
		QCOMPARE(SettingsValue(pc, "vblank_frequency"), QString("60"));
		// And the files live under the client's own directory.
		QVERIFY(QFileInfo::exists(config.filePath("clients/" + uuid + "/games/" + id + ".json")));
	}

	void clientPutAcceptsTypedJsonValues() {
		// The phone sends ints and bools as JSON types, not text; they must not be rejected.
		QTemporaryDir config;
		qputenv("KYTY_STREAMING_CONFIG_DIR", config.path().toUtf8());
		const QString a    = "cccccccc-cccc-cccc-cccc-cccccccccccc";
		const auto    id   = library.Games().first().id;
		const auto    path = "/kyty/v1/games/" + id + "/settings";
		const auto    rev  = QJsonDocument::fromJson(Call(api, ClientReq("GET", path, a)).body)
		                         .object()["revision"]
		                         .toString();
		const auto    put =
		    Call(api, ClientReq("PUT", path, a,
		                        {{"revision", rev},
		                         {"values", QJsonObject {{"trophy_duration_seconds", 5},
		                                                 {"perf_overlay_enabled", true},
		                                                 {"vblank_frequency", 90}}}}));
		QCOMPARE(put.status, 200);
		auto got = QJsonDocument::fromJson(Call(api, ClientReq("GET", path, a)).body).object();
		QCOMPARE(SettingsValue(got, "trophy_duration_seconds"), QString("5"));
		QCOMPARE(SettingsValue(got, "perf_overlay_enabled"), QString("true"));
		QCOMPARE(SettingsValue(got, "vblank_frequency"), QString("90"));
	}

	void clientPictureFitReachesLaunchArguments() {
		// The phone's per-device picture-fit (stream_aspect_mode) must travel through the device
		// PUT into the launch.json arguments the emulator is started with.
		QTemporaryDir config;
		qputenv("KYTY_STREAMING_CONFIG_DIR", config.path().toUtf8());
		const QString uuid = "dddddddd-dddd-dddd-dddd-dddddddddddd";
		const auto    id   = library.Games().first().id;
		const auto    path = "/kyty/v1/games/" + id + "/settings";

		const auto aspect_arg = [&] {
			const auto catalog = library.ClientCatalog(QStringLiteral("/opt/kyty/kyty_emulator"));
			const auto entries = catalog.value(uuid);
			for (const auto& e: entries) {
				if (e.id != id) continue;
				const auto i = e.arguments.indexOf(QStringLiteral("--stream-aspect"));
				return i >= 0 && i + 1 < e.arguments.size() ? e.arguments.at(i + 1) : QString {};
			}
			return QString {};
		};

		// Seed the device profile and set picture-fit to "fill" the way the phone does.
		const auto rev = QJsonDocument::fromJson(Call(api, ClientReq("GET", path, uuid)).body)
		                     .object()["revision"]
		                     .toString();
		const auto put = Call(
		    api,
		    ClientReq("PUT", path, uuid,
		              {{"revision", rev},
		               {"values", QJsonObject {{"stream_aspect_mode", QStringLiteral("fill")}}}}));
		QCOMPARE(put.status, 200);
		// It must be stored...
		auto got = QJsonDocument::fromJson(Call(api, ClientReq("GET", path, uuid)).body).object();
		QCOMPARE(SettingsValue(got, "stream_aspect_mode"), QString("fill"));
		// ...and it must reach the launch arguments the emulator actually receives.
		QCOMPARE(aspect_arg(), QString("fill"));

		// Changing it again flows through just as well (stretch), proving the value is not pinned.
		const auto rev2 = QJsonDocument::fromJson(Call(api, ClientReq("GET", path, uuid)).body)
		                      .object()["revision"]
		                      .toString();
		QCOMPARE(Call(api, ClientReq("PUT", path, uuid,
		                             {{"revision", rev2},
		                              {"values", QJsonObject {{"stream_aspect_mode",
		                                                       QStringLiteral("stretch")}}}}))
		             .status,
		         200);
		QCOMPARE(aspect_arg(), QString("stretch"));
	}

	void twoClientsAreIsolated() {
		QTemporaryDir config;
		qputenv("KYTY_STREAMING_CONFIG_DIR", config.path().toUtf8());
		const QString a    = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
		const QString b    = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
		const auto    id   = library.Games().first().id;
		const auto    path = "/kyty/v1/games/" + id + "/settings";
		auto          rev  = [&](const QString& u) {
			return QJsonDocument::fromJson(Call(api, ClientReq("GET", path, u)).body)
			    .object()["revision"]
			    .toString();
		};
		QCOMPARE(Call(api, ClientReq("PUT", path, a,
		                             {{"revision", rev(a)},
		                              {"values", QJsonObject {{"vblank_frequency", "90"}}}}))
		             .status,
		         200);
		QCOMPARE(Call(api, ClientReq("PUT", path, b,
		                             {{"revision", rev(b)},
		                              {"values", QJsonObject {{"vblank_frequency", "144"}}}}))
		             .status,
		         200);
		auto ga = QJsonDocument::fromJson(Call(api, ClientReq("GET", path, a)).body).object();
		auto gb = QJsonDocument::fromJson(Call(api, ClientReq("GET", path, b)).body).object();
		QCOMPARE(SettingsValue(ga, "vblank_frequency"), QString("90"));
		QCOMPARE(SettingsValue(gb, "vblank_frequency"), QString("144"));
	}

	void noClientKeepsOldBehaviour() {
		QTemporaryDir config;
		qputenv("KYTY_STREAMING_CONFIG_DIR", config.path().toUtf8());
		const auto id   = library.Games().first().id;
		const auto path = "/kyty/v1/games/" + id + "/settings";
		auto       get =
		    QJsonDocument::fromJson(Call(api, {{"method", "GET"}, {"path", path}}).body).object();
		QCOMPARE(get["scope"].toString(), QString("pc"));
		auto put = Call(
		    api, {{"method", "PUT"},
		          {"path", path},
		          {"body", QJsonObject {{"revision", get["revision"]},
		                                {"values", QJsonObject {{"vblank_frequency", "90"}}}}}});
		QCOMPARE(put.status, 200);
		// Unscoped writes still hit the PC config (GameLibrary), so a reload persists them.
		GameLibrary reloaded;
		QString     error;
		QVERIFY(reloaded.Load(nullptr, &error));
		QCOMPARE(reloaded.Find(id)->info->GameSettings().value("vblank_frequency").toString(),
		         QString("90"));
		// No client directory was created.
		QVERIFY(!QDir(config.filePath("clients")).exists() ||
		        QDir(config.filePath("clients")).isEmpty());
	}

	void invalidClientIdIsRejectedWithoutTouchingDisk() {
		QTemporaryDir config;
		qputenv("KYTY_STREAMING_CONFIG_DIR", config.path().toUtf8());
		const auto id   = library.Games().first().id;
		const auto path = "/kyty/v1/games/" + id + "/settings";
		for (const auto& bad: {QString("../../x"), QString("not-a-uuid"), QString()}) {
			auto r = Call(api, {{"method", "GET"},
			                    {"path", path},
			                    {"client", QJsonObject {{"id", bad}, {"name", "x"}}}});
			QCOMPARE(r.status, 400);
			QCOMPARE(QJsonDocument::fromJson(r.body).object()["error"].toString(),
			         QString("bad-client"));
		}
		QVERIFY(!QDir(config.filePath("clients")).exists() ||
		        QDir(config.filePath("clients")).isEmpty());
	}

	void clientCheatsNeverModifyPcPlanAndProducePerClientPlan() {
		QTemporaryDir config;
		qputenv("KYTY_STREAMING_CONFIG_DIR", config.path().toUtf8());
		const auto plan = Cheats::PlanPath("PPSA00001");
		WriteFile(
		    plan,
		    R"({"id":"PPSA00001","version":"01.002","process":"eboot.bin","mods":[)"
		    R"({"name":"Inf HP","enabled":false,"memory":[{"offset":"0x10","off":"00","on":"01"}]},)"
		    R"({"name":"Inf MP","enabled":false,"memory":[{"offset":"0x20","off":"00","on":"01"}]}]})");
		const auto    pc_before = ReadFile(plan);
		const QString uuid      = "22222222-2222-2222-2222-222222222222";
		const auto    id        = library.Games().first().id;
		const auto    path      = "/kyty/v1/games/" + id + "/cheats";
		auto get = QJsonDocument::fromJson(Call(api, ClientReq("GET", path, uuid)).body).object();
		QCOMPARE(get["mods"].toArray().size(), 2);
		auto put = Call(
		    api, ClientReq("PUT", path, uuid,
		                   {{"revision", get["revision"]}, {"enabled", QJsonArray {true, false}}}));
		QCOMPARE(put.status, 200);
		// The PC plan file is byte-for-byte unchanged.
		QCOMPARE(ReadFile(plan), pc_before);
		// The device sees its own selection.
		auto after = QJsonDocument::fromJson(Call(api, ClientReq("GET", path, uuid)).body).object();
		QVERIFY(after["mods"].toArray()[0].toObject()["enabled"].toBool());
		QVERIFY(!after["mods"].toArray()[1].toObject()["enabled"].toBool());
		// A per-client plan was written, enabling exactly the first mod.
		const auto patch = config.filePath("clients/" + uuid + "/games/" + id + ".patch.json");
		QVERIFY(QFileInfo::exists(patch));
		auto mods = QJsonDocument::fromJson(ReadFile(patch)).object()["mods"].toArray();
		QVERIFY(mods[0].toObject()["enabled"].toBool());
		QVERIFY(!mods[1].toObject()["enabled"].toBool());
		QFile::remove(plan);
	}

	void settingsResetReseedsFromPc() {
		QTemporaryDir config;
		qputenv("KYTY_STREAMING_CONFIG_DIR", config.path().toUtf8());
		const QString uuid = "33333333-3333-3333-3333-333333333333";
		const auto    id   = library.Games().first().id;
		const auto    path = "/kyty/v1/games/" + id + "/settings";
		auto get = QJsonDocument::fromJson(Call(api, ClientReq("GET", path, uuid)).body).object();
		QCOMPARE(Call(api, ClientReq("PUT", path, uuid,
		                             {{"revision", get["revision"]},
		                              {"values", QJsonObject {{"vblank_frequency", "120"}}}}))
		             .status,
		         200);
		// Reset returns the GET body re-seeded from the PC (back to 60).
		auto reset = Call(api, ClientReq("POST", path + "/reset", uuid, {}));
		QCOMPARE(reset.status, 200);
		auto body = QJsonDocument::fromJson(reset.body).object();
		QCOMPARE(body["scope"].toString(), QString("device"));
		QCOMPARE(SettingsValue(body, "vblank_frequency"), QString("60"));
	}

	void trophiesReturnsContractShape() {
		const auto id = library.Games().first().id;
		auto r = Call(api, {{"method", "GET"}, {"path", "/kyty/v1/games/" + id + "/trophies"}});
		QCOMPARE(r.status, 200);
		auto body = QJsonDocument::fromJson(r.body).object();
		QVERIFY(body.contains("trophies"));
		QVERIFY(body["trophies"].isArray());
		QVERIFY(body.contains("unlocked"));
		QVERIFY(body.contains("total"));
		// No trophy packages in the fixture game: an empty, well-formed payload.
		QCOMPARE(body["total"].toInt(), 0);
		QCOMPARE(body["unlocked"].toInt(), 0);
		QCOMPARE(body["trophies"].toArray().size(), 0);
	}

	// Shared cross-repo vector: this exact (salt, password) -> verifier must match
	// the host's own test (kyty-streaming-sunshine/tests/unit/test_ags_pairing.cpp,
	// AgsSrpSharedVector). If either side's SRP math drifts, both tests break.
	void agsVerifierMatchesSharedVector() {
		QByteArray       salt     = QByteArray::fromHex("000102030405060708090a0b0c0d0e0f");
		const QString    password = "ags-shared-vector-pw";
		const QByteArray expected =
		    "f7fa4eb192fd7e68ba3dcaad41d73f293e5257503498011a9efb162e17f584a6"
		    "69f1b6bb3b5dfb2957feeb6a61f084f02deec7c194cc3072f4e9d1ec2c1ff136"
		    "4382f0d3a7aa64fcee8ee0e7e123daffed2f8dcb394f21ea1761063b99062b2b"
		    "28685c9d0c5507e582995c7404b1d9870be575496a683e602cf4030bf3a20dcf"
		    "3a581ce8ea1944156fee27f697be7f38cbc4c7604a677036710e339b23656d37"
		    "e37d13eae5b2d1fc182bc2fe1a62db640255b8b22c6b23617aa3f41868854c3e"
		    "f5bc5c17b672a1e5d87faca84123df25f704cf6a59571e63d9087ef8153e732d"
		    "815797b0be4d0de79d9aa6697bc6eae0b007f751c1fd16f342c2addbc01a7230"
		    "d168637738b21b21044023377f93e365280aeaca91c08c175520513b06633f2b"
		    "60e3d00c0b801eb2cf59ae6458d7b4e7da1cbac41b870efc2afd59e06ec1d260"
		    "6bca0ea46159de6ea9e972db0c2a4e012a958140821526a8fa2100cd8e1221b1"
		    "b6c67fd860cd4a54fdf9731d27c8d25edd3ff7010dfe00a7377c14ad487afe92";
		QCOMPARE(AgsPairing::ComputeVerifierHex(salt, password), expected);
	}

	// Setting a password writes an owner-only JSON record; removing it deletes the file.
	void agsSetPasswordWritesPrivateFile() {
		QTemporaryDir dir;
		qputenv("KYTY_STREAMING_CONFIG_DIR", dir.path().toUtf8());
		const QString path = AgsPairing::VerifierFilePath();
		QVERIFY(!AgsPairing::VerifierExists(path));

		QString error;
		QVERIFY2(AgsPairing::SetPassword(path, "longenough8", &error), qPrintable(error));
		QVERIFY(AgsPairing::VerifierExists(path));

		// Owner-only permissions (0600).
		auto perms = QFile::permissions(path);
		QCOMPARE(perms &
		             (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther),
		         QFile::Permissions {});
		QVERIFY(perms & QFile::ReadOwner);
		QVERIFY(perms & QFile::WriteOwner);

		// The record carries the production group/hash/identity and hex salt + verifier.
		auto record = QJsonDocument::fromJson(ReadFile(path)).object();
		QCOMPARE(record["group"].toString(), QStringLiteral("rfc5054-3072"));
		QCOMPARE(record["hash"].toString(), QStringLiteral("sha256"));
		QCOMPARE(record["identity"].toString(), QStringLiteral("ags"));
		QCOMPARE(record["salt"].toString().size(), 32);      // 16 bytes hex
		QCOMPARE(record["verifier"].toString().size(), 768); // 384 bytes hex (N width)

		QVERIFY(AgsPairing::RemovePassword(path, &error));
		QVERIFY(!AgsPairing::VerifierExists(path));
		qunsetenv("KYTY_STREAMING_CONFIG_DIR");
	}

	// A too-short password is refused and writes nothing.
	void agsShortPasswordRejected() {
		QTemporaryDir dir;
		qputenv("KYTY_STREAMING_CONFIG_DIR", dir.path().toUtf8());
		const QString path = AgsPairing::VerifierFilePath();
		QString       error;
		QVERIFY(!AgsPairing::SetPassword(path, "short", &error));
		QVERIFY(!error.isEmpty());
		QVERIFY(!AgsPairing::VerifierExists(path));
		qunsetenv("KYTY_STREAMING_CONFIG_DIR");
	}
};

QTEST_MAIN(LauncherHostTests)
#include "LauncherHostTests.moc"
