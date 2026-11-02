#include "agsPairing.h"
#include "cheatFile.h"
#include "clientProfiles.h"
#include "common/fpsStats.h"
#include "common/trophies.h"
#include "gameLibrary.h"
#include "hostApi.h"
#include "hostDaemon.h"
#include "hostMetrics.h"
#include "hostSocket.h"
#include "steamSource.h"

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
#include <sys/wait.h>
#include <signal.h>
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

static QJsonObject RuntimeBody(const QString& game, const QString& client, const QString& directory,
                               qint64 pid = ::getpid()) {
	const auto stat = ReadFile(QStringLiteral("/proc/%1/stat").arg(pid));
	const auto start = QString::fromLatin1(stat.mid(stat.lastIndexOf(')') + 2).split(' ').value(19));
	return {{"session_id", QString(64, '1')}, {"reservation_token", QString(64, '2')},
	        {"game", game}, {"client_id", client}, {"runtime_directory", directory},
	        {"owner_pid", pid}, {"owner_start", start}, {"state", "starting"}};
}

static void WriteTrophyFixture(const QString& path) {
	const QMap<QByteArray, QByteArray> files {
	    {"tropconf.json", R"({"defaultLanguage":"en-US","trophies":[{"id":"0","grade":"B","hidden":true},{"id":"1","grade":"B","hidden":true}]})"},
	    {"tropmeta_en-US.json", R"({"metadata":{"titleMetadata":{"name":"Fixture"},"trophyMetadata":[]}})"},
	    {"trop0000.png", "ICON0"}, {"trop0001.png", "ICON1"}};
	qsizetype offset = 0x40 + files.size() * 0x40;
	qsizetype size = offset; for (const auto& bytes: files) size += bytes.size();
	QByteArray bytes(size, '\0');
	const auto number = [&](qsizetype at, quint64 value, int count) {
		for (int i = count - 1; i >= 0; --i) { bytes[at + i] = char(value & 255); value >>= 8; }
	};
	number(0, 0xb228c60a, 4); number(4, 1, 4); number(8, size, 8); number(0x10, files.size(), 4); number(0x14, 0x20, 4);
	qsizetype entry = 0x40;
	for (auto file = files.cbegin(); file != files.cend(); ++file) {
		std::copy(file.key().begin(), file.key().end(), bytes.begin() + entry);
		number(entry + 0x20, offset, 8); number(entry + 0x28, file.value().size(), 8);
		std::copy(file.value().begin(), file.value().end(), bytes.begin() + offset);
		offset += file.value().size(); entry += 0x40;
	}
	WriteFile(path, bytes);
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
		          "[Launcher]\ngame_dirs=" + m_dir.filePath("games").toUtf8() + "\nsteam_roots=" + m_dir.filePath("no-steam").toUtf8() + "\n");
		MakeGame("A", "PPSA00001");
		MakeGame("nested/B", "PPSA00002");
		QString error;
		QVERIFY2(library.Load(nullptr, &error), qPrintable(error));
	}
	void cleanup() { QDir::setCurrent(m_old_cwd); }
	void steamReadsRejectSpecialAndOversizedFiles() {
		QTemporaryDir fixture;
		QDir().mkpath(fixture.filePath("steamapps"));
		const auto fifo = fixture.filePath("steamapps/libraryfolders.vdf");
		QCOMPARE(::mkfifo(QFile::encodeName(fifo), 0600), 0);
		const auto child = ::fork();
		QVERIFY(child >= 0);
		if (child == 0) ::_exit(SteamSource::Discover({fixture.path()}, {}).isEmpty() ? 0 : 1);
		int   status = 0;
		pid_t result = 0;
		for (int attempt = 0; attempt < 100 && result == 0; ++attempt) {
			result = ::waitpid(child, &status, WNOHANG);
			if (result == 0) QTest::qWait(10);
		}
		if (result == 0) {
			::kill(child, SIGKILL);
			::waitpid(child, &status, 0);
		}
		QVERIFY2(result == child, "FIFO discovery did not complete within one second");
		QVERIFY(WIFEXITED(status));
		QCOMPARE(WEXITSTATUS(status), 0);
		QVERIFY(SteamSource::Read("/dev/zero").isEmpty());
		const auto large = fixture.filePath("appmanifest_1.acf");
		WriteFile(large, QByteArray(1024 * 1024 + 1, 'x'));
		QVERIFY(SteamSource::Read(large).isEmpty());
		const auto regular = fixture.filePath("regular");
		WriteFile(regular, "fixture");
		const auto link = fixture.filePath("file-link");
		QVERIFY(QFile::link(regular, link));
		QVERIFY(SteamSource::Read(link).isEmpty());
		const auto alias = fixture.filePath("root-alias");
		QVERIFY(QFile::link(fixture.path(), alias));
		QCOMPARE(SteamSource::Read(alias + "/regular"), QByteArray("fixture"));
		// procfs files report size zero despite having content: the actual read must
		// still be bounded, rather than treating stat size as the read limit.
		const auto proc = SteamSource::Read("/proc/version");
		QVERIFY(!proc.isEmpty());
		QVERIFY(proc.size() <= 1024 * 1024);
	}
	void steamDiscoveryAndOwnedAdmission() {
		QTemporaryDir fixture;
		const auto    root = fixture.filePath("steam"), second = fixture.filePath("second");
		const auto    manifest = [](const char* id, const char* name, const char* dir,
		                            const char* state = "4") {
			return QByteArray("\"AppState\" { \"appid\" \"") + id + "\" \"name\" \"" + name +
			       "\" \"installdir\" \"" + dir + "\" \"StateFlags\" \"" + state + "\" }";
		};
		WriteFile(root + "/steamapps/libraryfolders.vdf",
		          "\"libraryfolders\" { \"0\" { \"path\" \"" + second.toUtf8() + "\" } }");
		WriteFile(root + "/steamapps/appmanifest_620.acf",
		          manifest("620", "Native Fixture", "native"));
		WriteFile(second + "/steamapps/appmanifest_268910.acf",
		          manifest("268910", "Windows Fixture", "windows"));
		WriteFile(root + "/steamapps/appmanifest_99.acf",
		          manifest("99", "Steam Linux Runtime 3.0", "runtime"));
		WriteFile(root + "/steamapps/appmanifest_88.acf",
		          manifest("88", "Incomplete", "pending", "2"));
		WriteFile(root + "/steamapps/appmanifest_77.acf", "\"AppState\" { \"appid\" \"77\"");
		QDir().mkpath(root + "/steamapps/common/native");
		QDir().mkpath(second + "/steamapps/common/windows");
		const auto        cwd = QFileInfo(fixture.path()).canonicalFilePath();
		const QJsonObject recipe {{"executable", "/usr/bin/true"},
		                          {"args", QJsonArray {"literal ; $ text"}},
		                          {"cwd", cwd},
		                          {"env", QJsonObject {{"SteamAppId", "620"}}}};
		QVERIFY(SteamSource::Parse("\"broken\" { \"key\" \"value\"").isEmpty());
		QVERIFY(SteamSource::Parse("\"key\" \"a\" \"key\" \"b\"").isEmpty());
		auto records = SteamSource::Discover({root, root}, {{"620", recipe}});
		QCOMPARE(records.size(), 3);
		QCOMPARE(SteamSource::Discover({second}, {}).first().id, SteamSource::Id("268910"));
		auto found =
		    std::find_if(records.begin(), records.end(), [](auto r) { return r.app_id == "620"; });
		QVERIFY(found != records.end());
		QVERIFY(found->installed);
		QVERIFY(found->launch);
		QCOMPARE(found->id, SteamSource::Id("620"));
        QCOMPARE(found->launch->process_visibility, QString("private"));
        auto hostRecipe = recipe;
        hostRecipe["process_visibility"] = "host_steam";
        QCOMPARE(SteamSource::Discover({root}, {{"620", hostRecipe}}).first().launch->process_visibility, QString("host_steam"));
        for (const auto value : {QJsonValue("host"), QJsonValue(true)}) {
            auto invalid = recipe;
            invalid["process_visibility"] = value;
            QVERIFY(!SteamSource::Discover({root}, {{"620", invalid}}).first().launch);
        }
		auto bad   = recipe;
		bad["env"] = QJsonObject {{"DISPLAY", ":0"}};
		QVERIFY(!SteamSource::Discover({root}, {{"620", bad}}).first().launch);
		auto incomplete =
		    std::find_if(records.begin(), records.end(), [](auto r) { return r.app_id == "88"; });
		QVERIFY(!incomplete->installed);
		const auto recipePath = fixture.filePath("recipes.json");
		WriteFile(recipePath, QJsonDocument(QJsonObject {{"620", hostRecipe}}).toJson());
		auto settings = GameLibraryFiles::OpenSettings();
		settings->setValue("Launcher/steam_roots", root);
		settings->setValue("Launcher/steam_recipes", recipePath);
		settings->sync();
		QString error;
		QVERIFY(library.Load(nullptr, &error));
		const auto id = SteamSource::Id("620");
		QVERIFY(!library.Find(id)->info);
		HostApi resolver {&library, [] { return HostApi::Status {}; }};
		resolver.ConfigureNativeLaunch("/usr/bin/true", fixture.filePath("emulator.lock"));
		const QString uuid = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
		auto          reply =
		    Call(resolver, ClientReq("POST", "/ags/internal/resolve-launch", uuid, {{"game", id}}));
		QCOMPARE(reply.status, 200);
        QCOMPARE(QJsonDocument::fromJson(reply.body).object()["process_visibility"].toString(), QString("host_steam"));
		QCOMPARE(
		    QJsonDocument::fromJson(reply.body).object()["env"].toObject()["SteamAppId"].toString(),
		    QString("620"));
		auto body = RuntimeBody(id, uuid, cwd);
		QCOMPARE(
		    Call(resolver, ClientReq("POST", "/ags/internal/session-runtime", uuid, body)).status,
		    200);
		body["state"] = "closed";
		QCOMPARE(
		    Call(resolver, ClientReq("POST", "/ags/internal/session-runtime", uuid, body)).status,
		    200);
		QCOMPARE(
		    Call(resolver, ClientReq("GET", "/kyty/v1/games/" + id + "/settings", uuid)).status,
		    409);
	}
	void isolatedDiagnosticTraversalCannotTouchExternalSentinels() {
		QTemporaryDir config;
		qputenv("KYTY_STREAMING_CONFIG_DIR", config.path().toUtf8());
		const QString uuid = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
		const auto    id   = library.Games().first().id;
		auto*         info = library.Find(id)->info.get();
		QString error;
		QVERIFY(info->SetGameSettings({{"printf_direction", "File"}, {"printf_output_file", "../normal/printf.log"},
		    {"shader_log_direction", "File"}, {"shader_log_folder", "../normal/shaders"},
		    {"command_buffer_dump_enabled", "true"}, {"command_buffer_dump_folder", "../normal/commands"}}, error));
		const auto outside = config.filePath("normal");
		for (const auto& name: {"printf.log", "shaders/sentinel", "commands/sentinel"}) WriteFile(QDir(outside).filePath(name), "unchanged");
		HostApi resolver {&library, [] { return HostApi::Status {}; }};
		resolver.ConfigureNativeLaunch("/usr/bin/true", config.filePath("emulator.lock"), config.path());
		const auto reply = Call(resolver, ClientReq("POST", "/ags/internal/resolve-launch", uuid, {{"game", id}}));
		QCOMPARE(reply.status, 200);
		const auto game = QJsonDocument::fromJson(reply.body).object();
		const auto args = game["args"].toArray();
		const auto cwd = game["cwd"].toString();
		for (const auto& flag: {"--printf-output-file", "--shader-log-folder", "--command-buffer-dump-folder"}) {
			const auto at = std::find(args.begin(), args.end(), QJsonValue(flag));
			QVERIFY(at != args.end());
			auto target = QDir(cwd).absoluteFilePath((at + 1)->toString());
			if (QString(flag) != "--printf-output-file") target = QDir(target).filePath("sentinel");
			WriteFile(target, "fixture diagnostic output");
		}
		for (const auto& name: {"printf.log", "shaders/sentinel", "commands/sentinel"})
			QCOMPARE(ReadFile(QDir(outside).filePath(name)), QByteArray("unchanged"));
	}

	void isolatedDiagnosticSymlinksReject_data() {
		QTest::addColumn<QString>("target");
		QTest::newRow("shader directory") << "_ShaderLogs";
		QTest::newRow("command buffer directory") << "_CommandBuffers";
		QTest::newRow("printf existing target") << "_TempData/printf.log";
		QTest::newRow("nested diagnostic ancestor") << "_ShaderLogs/nested";
	}
	void isolatedDiagnosticSymlinksReject() {
		QFETCH(QString, target);
		QTemporaryDir config, outside;
		qputenv("KYTY_STREAMING_CONFIG_DIR", config.path().toUtf8());
		const QString uuid = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
		const auto id = library.Games().first().id;
		const auto cwd = config.filePath(id);
		QDir().mkpath(QDir(cwd).filePath("_TempData"));
		WriteFile(outside.filePath("sentinel"), "unchanged");
		QDir().mkpath(QFileInfo(QDir(cwd).filePath(target)).absolutePath());
		QVERIFY(QFile::link(target.endsWith(".log") ? outside.filePath("sentinel") : outside.path(), QDir(cwd).filePath(target)));
		HostApi resolver {&library, [] { return HostApi::Status {}; }};
		resolver.ConfigureNativeLaunch("/usr/bin/true", config.filePath("emulator.lock"), config.path());
		const auto reply = Call(resolver, ClientReq("POST", "/ags/internal/resolve-launch", uuid, {{"game", id}}));
		QCOMPARE(reply.status, 404);
		QCOMPARE(ReadFile(outside.filePath("sentinel")), QByteArray("unchanged"));
	}

	void nativeTrophiesUseResolvedDeviceSnapshot_data() {
		QTest::addColumn<bool>("icons");
		QTest::newRow("progress") << false;
		QTest::newRow("hidden icons") << true;
	}
	void nativeTrophiesUseResolvedDeviceSnapshot() {
		QFETCH(bool, icons);
		QTemporaryDir config;
		qputenv("KYTY_STREAMING_CONFIG_DIR", config.path().toUtf8());
		const QString uuid = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
		const QString other = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
		const auto id = library.Games().first().id;
		auto* info = library.Find(id)->info.get(); info->user_id = 1;
		const auto package = QDir(info->basedir).filePath("sce_sys/trophy2/trophy00.ucp");
		WriteTrophyFixture(package);
		HostApi resolver {&library, [] { return HostApi::Status {}; }};
		resolver.ConfigureNativeLaunch("/usr/bin/true", config.filePath("emulator.lock"), config.path());
		const auto settingsPath = "/kyty/v1/games/" + id + "/settings";
		auto get = QJsonDocument::fromJson(Call(resolver, ClientReq("GET", settingsPath, uuid)).body).object();
		QCOMPARE(Call(resolver, ClientReq("PUT", settingsPath, uuid, {{"revision", get["revision"]}, {"values", QJsonObject {{"user_id", "2"}}}})).status, 200);
		QCOMPARE(Call(resolver, ClientReq("POST", "/ags/internal/resolve-launch", uuid, {{"game", id}})).status, 200);
		const auto runtime = config.filePath(id);
		const auto user1 = Common::Trophies::UnlocksPath(runtime.toStdString(), info->title_id.toStdString(), 1, 0);
		const auto user2 = Common::Trophies::UnlocksPath(runtime.toStdString(), info->title_id.toStdString(), 2, 0);
		QVERIFY(Common::Trophies::SaveUnlockData(user1, {{0}, {}}));
		QVERIFY(Common::Trophies::SaveUnlockData(user2, {{1}, {}}));
		const auto user1Before = ReadFile(QString::fromStdString(user1.string()));
		const auto trophiesPath = "/kyty/v1/games/" + id + "/trophies";
		const auto assertUser = [&](int user) {
			if (icons) {
				QCOMPARE(Call(resolver, ClientReq("GET", trophiesPath + "/icon/0-0", uuid)).status, user == 1 ? 200 : 404);
				QCOMPARE(Call(resolver, ClientReq("GET", trophiesPath + "/icon/0-1", uuid)).status, user == 2 ? 200 : 404);
			} else {
				const auto reply = Call(resolver, ClientReq("GET", trophiesPath, uuid)); QCOMPARE(reply.status, 200);
				const auto trophies = QJsonDocument::fromJson(reply.body).object()["trophies"].toArray();
				QCOMPARE(trophies.size(), 2);
				QCOMPARE(trophies[0].toObject()["unlocked"].toBool(), user == 1);
				QCOMPARE(trophies[1].toObject()["unlocked"].toBool(), user == 2);
			}
		};
		assertUser(2); // pre-launch reads use the trusted device's current profile
		// An external owner edit between resolve and prepare must not change the launch snapshot.
		bool seeded = false; auto profile = ClientProfiles::ReadGame(uuid, id, &seeded); QVERIFY(seeded);
		auto values = profile["settings"].toObject(); values["user_id"] = "1"; profile["settings"] = values;
		QCOMPARE(ClientProfiles::WriteGame(uuid, id, profile), QString());
		auto body = RuntimeBody(id, uuid, runtime);
		QCOMPARE(Call(resolver, ClientReq("POST", "/ags/internal/session-runtime", uuid, body)).status, 200);
		assertUser(2); // admitted launch is pinned to the resolved user, including hidden icons
		QCOMPARE(Call(resolver, ClientReq("GET", trophiesPath, other)).status, 409);
		get = QJsonDocument::fromJson(Call(resolver, ClientReq("GET", settingsPath, uuid)).body).object();
		QVERIFY(get["running"].toBool());
		QCOMPARE(Call(resolver, ClientReq("PUT", settingsPath, uuid, {{"revision", get["revision"]}, {"values", QJsonObject {{"user_id", "3"}}}})).status, 409);
		QCOMPARE(Call(resolver, ClientReq("POST", settingsPath + "/reset", uuid, {})).status, 409);
		body["state"] = "closed";
		QCOMPARE(Call(resolver, ClientReq("POST", "/ags/internal/session-runtime", uuid, body)).status, 200);
		assertUser(1); // next-launch profile takes effect only after the previous runtime closes
		QCOMPARE(ReadFile(QString::fromStdString(user1.string())), user1Before);
		QFile::remove(package);
	}

	void nativeResolverPreservesCompleteDeviceArguments() {
		QTemporaryDir config;
		qputenv("KYTY_STREAMING_CONFIG_DIR", config.path().toUtf8());
		const QString a = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
		const QString b = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
		const auto id = library.Games().first().id;
		const auto path = "/kyty/v1/games/" + id + "/settings";
		const auto pcBefore = ReadFile(m_dir.filePath("Kyty.ini"));
		HostApi resolver {&library, [] { return HostApi::Status {}; }};
		resolver.ConfigureNativeLaunch("/usr/bin/true", config.filePath("emulator.lock"), config.path());
		for (const auto& uuid: {a, b}) {
			const auto settings = QJsonDocument::fromJson(Call(api, ClientReq("GET", path, uuid)).body).object();
			QCOMPARE(Call(api, ClientReq("PUT", path, uuid, {{"revision", settings["revision"]},
			    {"values", QJsonObject {{"vblank_frequency", uuid == a ? "90" : "120"}, {"user_name", "Fixture Player"}}}})).status, 200);
		}
		const auto first = Call(resolver, ClientReq("POST", "/ags/internal/resolve-launch", a, {{"game", id}}));
		QCOMPARE(first.status, 200);
		const auto second = Call(resolver, ClientReq("POST", "/ags/internal/resolve-launch", b, {{"game", id}}));
		QCOMPARE(second.status, 200);
		const auto aa = QJsonDocument::fromJson(first.body).object()["args"].toArray();
		const auto bb = QJsonDocument::fromJson(second.body).object()["args"].toArray();
		QCOMPARE(std::count(aa.begin(), aa.end(), QJsonValue("--game")), 1);
		QVERIFY(aa != bb);
		QVERIFY(aa.contains(QStringLiteral("Fixture Player")));
		QCOMPARE(ReadFile(m_dir.filePath("Kyty.ini")), pcBefore);
	}
	void nativeReservationExcludesLocalAndRemoteAndRejectsReplay() {
		QTemporaryDir config;
		QFile::setPermissions(config.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
		qputenv("KYTY_STREAMING_CONFIG_DIR", config.path().toUtf8());
		HostApi resolver {&library, [] { return HostApi::Status {}; }};
		const auto lockPath = config.filePath("emulator.lock");
		resolver.ConfigureNativeLaunch("/usr/bin/true", lockPath, config.path());
		const QString uuid = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
		const auto id = library.Games().first().id;
		QCOMPARE(Call(resolver, ClientReq("POST", "/ags/internal/resolve-launch", uuid, {{"game", id}})).status, 200);
		auto body = RuntimeBody(id, uuid, config.filePath(id));
		const auto send = [&](const char* state) {
			body["state"] = state;
			return Call(resolver, ClientReq("POST", "/ags/internal/session-runtime", uuid, body));
		};
		const int local = ::open(QFile::encodeName(lockPath), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
		QVERIFY(local >= 0);
		QVERIFY(::flock(local, LOCK_EX | LOCK_NB) == 0);
		QCOMPARE(send("starting").status, 409);
		::flock(local, LOCK_UN);
		QCOMPARE(send("starting").status, 200);
		QCOMPARE(send("starting").status, 409);
		const int admission = ::open(QFile::encodeName(lockPath + ".launch"), O_RDWR | O_CLOEXEC);
		QVERIFY(admission >= 0);
		QVERIFY(::flock(admission, LOCK_EX | LOCK_NB) != 0);
		// The daemon reserves admission only: the intended child can acquire lifetime ownership.
		QCOMPARE(send("claim").status, 200);
		QCOMPARE(send("claim").status, 409);
		QVERIFY(::flock(local, LOCK_EX | LOCK_NB) == 0);
		QCOMPARE(send("acquired").status, 200);
		QVERIFY(::flock(admission, LOCK_EX | LOCK_NB) == 0);
		::flock(admission, LOCK_UN);
		QCOMPARE(send("claim").status, 409);
		QCOMPARE(send("closed").status, 200);
		::close(local); ::close(admission);
	}

	void nativeOwnerDeathClearsReservationAndUnsafeRootsFailClosed() {
		QTemporaryDir config;
		QFile::setPermissions(config.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
		qputenv("KYTY_STREAMING_CONFIG_DIR", config.path().toUtf8());
		HostApi resolver {&library, [] { return HostApi::Status {}; }};
		const auto lockPath = config.filePath("emulator.lock");
		resolver.ConfigureNativeLaunch("/usr/bin/true", lockPath, config.path());
		const QString uuid = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
		const auto id = library.Games().first().id;
		QCOMPARE(Call(resolver, ClientReq("POST", "/ags/internal/resolve-launch", uuid, {{"game", id}})).status, 200);
		QProcess owner; owner.start("/usr/bin/sleep", {"60"}); QVERIFY(owner.waitForStarted());
		auto body = RuntimeBody(id, uuid, config.filePath(id), owner.processId());
		body["runtime_directory"] = "/tmp";
		QCOMPARE(Call(resolver, ClientReq("POST", "/ags/internal/session-runtime", uuid, body)).status, 400);
		body["runtime_directory"] = config.filePath(id);
		QCOMPARE(Call(resolver, ClientReq("POST", "/ags/internal/session-runtime", uuid, body)).status, 200);
		owner.kill(); QVERIFY(owner.waitForFinished());
		QTest::qWait(350);
		const int admission = ::open(QFile::encodeName(lockPath + ".launch"), O_RDWR | O_CLOEXEC);
		QVERIFY(admission >= 0); QVERIFY(::flock(admission, LOCK_EX | LOCK_NB) == 0); ::close(admission);
		QVERIFY(QDir(config.filePath(id + "/_SaveData")).removeRecursively());
		QVERIFY(QFile::link(config.path(), config.filePath(id + "/_SaveData")));
		QCOMPARE(Call(resolver, ClientReq("POST", "/ags/internal/resolve-launch", uuid, {{"game", id}})).status, 404);
	}

	void isolatedTrophyGetMigratesOnlySelectedRuntimeBeforeChildStarts() {
		QTemporaryDir config;
		QTemporaryDir normal;
		QFile::setPermissions(config.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
		qputenv("KYTY_STREAMING_CONFIG_DIR", config.path().toUtf8());
		const auto interpreter = normal.filePath("fixture-executable");
		QVERIFY(QFile::copy("/usr/bin/true", interpreter));
		QVERIFY(QFile::setPermissions(interpreter, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
		HostApi resolver {&library, [] { return HostApi::Status {}; }};
		resolver.ConfigureNativeLaunch(interpreter, config.filePath("emulator.lock"), config.path());
		const QString uuid = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
		const auto* entry = &library.Games().first();
		const auto id = entry->id;
		const auto info = entry->info.get();
		const auto title = info->title_id.toStdString();
		const auto normalLegacy = Common::Trophies::LegacyUnlocksPath(normal.path().toStdString(), title, info->user_id, 0);
		const auto normalTrophy = Common::Trophies::UnlocksPath(normal.path().toStdString(), title, info->user_id, 0);
		QVERIFY(Common::Trophies::SaveUnlockData(normalLegacy, {{1}, {}}));
		const auto normalBefore = ReadFile(QString::fromStdString(normalLegacy.string()));
		// Even an empty package exercises the GET migration code, independently of rendering.
		WriteFile(QDir(info->basedir).filePath("sce_sys/trophy2/trophy00.ucp"), "fixture");
		QCOMPARE(Call(resolver, ClientReq("POST", "/ags/internal/resolve-launch", uuid, {{"game", id}})).status, 200);
		const auto runtime = config.filePath(id);
		const auto isolatedLegacy = Common::Trophies::LegacyUnlocksPath(runtime.toStdString(), title, info->user_id, 0);
		const auto isolatedTrophy = Common::Trophies::UnlocksPath(runtime.toStdString(), title, info->user_id, 0);
		QVERIFY(Common::Trophies::SaveUnlockData(isolatedLegacy, {{2}, {}}));
		// The isolated daemon setting also governs reads before launch and after Close.
		const auto trophiesPath = "/kyty/v1/games/" + id + "/trophies";
		QCOMPARE(Call(resolver, ClientReq("GET", trophiesPath, uuid)).status, 200);
		QVERIFY(QFileInfo::exists(QString::fromStdString(isolatedTrophy.string())));
		QVERIFY(!QFileInfo::exists(QString::fromStdString(normalTrophy.string())));
		QVERIFY(QFile::remove(QString::fromStdString(isolatedTrophy.string())));
		auto body = RuntimeBody(id, uuid, runtime);
		QCOMPARE(Call(resolver, ClientReq("POST", "/ags/internal/session-runtime", uuid, body)).status, 200);
		QCOMPARE(Call(resolver, ClientReq("GET", "/kyty/v1/games/" + id + "/trophies", uuid)).status, 200);
		QVERIFY(QFileInfo::exists(QString::fromStdString(isolatedTrophy.string())));
		QVERIFY(!QFileInfo::exists(QString::fromStdString(normalTrophy.string())));
		QCOMPARE(ReadFile(QString::fromStdString(normalLegacy.string())), normalBefore);
		const auto fpsPath = QDir(runtime).filePath("_TempData/metrics-fps.json");
		WriteFile(fpsPath, QJsonDocument(QJsonObject {{"ts", QDateTime::currentSecsSinceEpoch()},
		    {"current", 25}, {"avg", 25}, {"min", 25}, {"max", 25}, {"window_s", 10}, {"frame_ms", 40}}).toJson());
		const auto metrics = QJsonDocument::fromJson(Call(resolver, ClientReq("GET", "/kyty/v1/host/metrics", uuid)).body).object();
		QCOMPARE(metrics["game"].toObject()["fps"].toObject()["current"].toInt(), 25);
		body["state"] = "closed";
		QCOMPARE(Call(resolver, ClientReq("POST", "/ags/internal/session-runtime", uuid, body)).status, 200);
		QVERIFY(QFile::remove(QString::fromStdString(isolatedTrophy.string())));
		QCOMPARE(Call(resolver, ClientReq("GET", trophiesPath, uuid)).status, 200);
		QVERIFY(QFileInfo::exists(QString::fromStdString(isolatedTrophy.string())));
		QVERIFY(!QFileInfo::exists(QString::fromStdString(normalTrophy.string())));
		QCOMPARE(ReadFile(QString::fromStdString(normalLegacy.string())), normalBefore);
		QFile::remove(QDir(info->basedir).filePath("sce_sys/trophy2/trophy00.ucp"));
	}

	void nativeGuardIntegrationUsesOwnerReservation() {
		const auto binary = qEnvironmentVariable("AGS_TEST_HOST_BINARY");
		if (binary.isEmpty()) QSKIP("Set AGS_TEST_HOST_BINARY to the built native host for the IPC integration fixture");
		QVERIFY(QFileInfo(binary).isExecutable());
		QTemporaryDir config;
		QTemporaryDir prefix;
		QFile::setPermissions(config.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
		qputenv("KYTY_STREAMING_CONFIG_DIR", config.path().toUtf8());
		const auto interpreter = prefix.filePath("fixture-native");
		WriteFile(interpreter, "#!/usr/bin/python3\nimport json,sys,time\nfrom pathlib import Path\nPath('args.json').write_text(json.dumps(sys.argv[1:]))\ntime.sleep(20)\n");
		QVERIFY(QFile::setPermissions(interpreter, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
		const QString uuid = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
		const auto id = library.Games().first().id;
		QDir().mkpath(config.filePath("kytyps5"));
		QFile::setPermissions(config.filePath("kytyps5"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
		const auto lockPath = config.filePath("kytyps5/emulator.lock");
		HostApi resolver {&library, [] { return HostApi::Status {}; }};
		resolver.ConfigureNativeLaunch(interpreter, lockPath, config.path());
		HostSocketServer server([&](const QJsonObject& request, auto done) { resolver.Handle(request, done); });
		QString error;
		const auto socket = config.filePath("api.sock");
		QVERIFY2(server.Listen(socket, &error), qPrintable(error));
		const auto launch = QJsonDocument::fromJson(Call(resolver, ClientReq("POST", "/ags/internal/resolve-launch", uuid, {{"game", id}})).body).object();
		QVERIFY(!launch.isEmpty());
		auto body = RuntimeBody(id, uuid, launch["cwd"].toString());
		QCOMPARE(Call(resolver, ClientReq("POST", "/ags/internal/session-runtime", uuid, body)).status, 200);
		QJsonObject context = body;
		context.remove("state"); context.remove("client_id");
		context["api_socket"] = socket; context["local_game_lock"] = lockPath;
		context["client"] = QJsonObject {{"id", uuid}, {"name", "Fixture"}};
		const auto bytes = QJsonDocument(QJsonObject {{"context", context}, {"game", launch}}).toJson(QJsonDocument::Compact);
		const auto request = config.filePath("launch.json");
		const auto replay = config.filePath("replay.json");
		WriteFile(request, bytes); WriteFile(replay, bytes);
		QFile::setPermissions(request, QFile::ReadOwner | QFile::WriteOwner);
		QFile::setPermissions(replay, QFile::ReadOwner | QFile::WriteOwner);
		// A simultaneous ordinary local guard must lose admission before it launches any child.
		const auto guardSource = QStringLiteral(AGS_TEST_GUARD_SOURCE);
		const auto localGuard = prefix.filePath("kyty_emulator");
		QVERIFY(QFile::copy(guardSource, localGuard));
		QDir().mkpath(prefix.filePath("libexec"));
		QVERIFY(QFile::copy(interpreter, prefix.filePath("libexec/kyty_emulator")));
		QProcess local;
		auto environment = QProcessEnvironment::systemEnvironment();
		environment.insert("XDG_RUNTIME_DIR", config.path());
		local.setProcessEnvironment(environment); local.setWorkingDirectory(prefix.path());
		local.start("/usr/bin/python3", {localGuard, "--game", "fixture with spaces"});
		QVERIFY(local.waitForFinished(5000)); QVERIFY(local.exitCode() != 0);
		QVERIFY(!QFileInfo::exists(prefix.filePath("args.json")));
		QProcess remote; remote.start(binary, {"launch-guard", request});
		QVERIFY(remote.waitForStarted());
		const auto arguments = QDir(launch["cwd"].toString()).filePath("args.json");
		QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(arguments), 5000);
		QCOMPARE(QJsonDocument::fromJson(ReadFile(arguments)).array(), launch["args"].toArray());
		QVERIFY(!QFileInfo::exists(request));
		QProcess repeat; repeat.start(binary, {"launch-guard", replay});
		QTRY_VERIFY_WITH_TIMEOUT(repeat.state() == QProcess::NotRunning, 5000);
		QVERIFY(repeat.exitCode() != 0);
		local.start("/usr/bin/python3", {localGuard, "--game", "fixture with spaces"});
		QVERIFY(local.waitForFinished(5000)); QVERIFY(local.exitCode() != 0);
		QVERIFY(!QFileInfo::exists(prefix.filePath("args.json")));
		remote.terminate();
		QTRY_VERIFY_WITH_TIMEOUT(remote.state() == QProcess::NotRunning, 5000);
		const int lock = ::open(QFile::encodeName(lockPath), O_RDWR | O_CLOEXEC);
		QVERIFY(lock >= 0); QVERIFY(::flock(lock, LOCK_EX | LOCK_NB) == 0); ::close(lock);
	}

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
	void pendingNativeRuntimeIsNotReportedAsLocalPlay() {
		QTemporaryDir run, profile;
		qputenv("XDG_RUNTIME_DIR", run.path().toUtf8());
		qputenv("KYTY_STREAMING_CONFIG_DIR", profile.path().toUtf8());
		qputenv("KYTY_HOST_NO_AUTOSTART", "1");
		HostDaemon daemon;
		QString error; QVERIFY2(daemon.Start(&error), qPrintable(error));
		const auto control = run.filePath("kyty-host/control.sock");
		const auto apiSocket = run.filePath("kyty-host/api.sock");
		const QString uuid = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
		const auto game = library.Games().first().id;
		const auto launch = Control(apiSocket, ClientReq("POST", "/ags/internal/resolve-launch", uuid, {{"game", game}}));
		QVERIFY2(launch.contains("cwd"), QJsonDocument(launch).toJson().constData());
		auto body = RuntimeBody(game, uuid, launch.value("cwd").toString());
		QVERIFY(Control(apiSocket, ClientReq("POST", "/ags/internal/session-runtime", uuid, body)).value("ok").toBool());
		auto status = Control(control, {{"type", "status"}});
		QVERIFY(status.value("stream_game_running").toBool());
		QVERIFY(!status.value("local_game_running").toBool());
		QCOMPARE(status.value("session").toString(), "Starting game…");
		const auto lockPath = run.filePath("kytyps5/emulator.lock");
		const int lock = ::open(QFile::encodeName(lockPath).constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
		QVERIFY(lock >= 0); QCOMPARE(::flock(lock, LOCK_EX | LOCK_NB), 0);
		status = Control(control, {{"type", "status"}});
		const bool falselyLocal = status.value("local_game_running").toBool();
		::close(lock);
		QVERIFY(!falselyLocal);
		body["state"] = "closed";
		QVERIFY(Control(apiSocket, ClientReq("POST", "/ags/internal/session-runtime", uuid, body)).value("ok").toBool());
		QVERIFY(!Control(control, {{"type", "status"}}).value("stream_game_running").toBool());
		daemon.Shutdown();
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

	// --- host metrics -------------------------------------------------------

	void metricsParsesProcStat() {
		// Window 1: total 1000 (busy 200, idle 800). Window 2: total 1800 (busy 600, idle 1200).
		// Delta total 800, delta idle 400 -> busy = (800-400)/800 = 50%.
		const auto a = HostMetrics::ParseProcStat("cpu  100 0 100 800 0 0 0 0\ncpu0 1 2 3 4\n");
		const auto b = HostMetrics::ParseProcStat("cpu  300 0 300 1200 0 0 0 0\n");
		QVERIFY(a.ok && b.ok);
		QCOMPARE(a.total, 1000ull);
		QCOMPARE(a.idle, 800ull);
		const auto usage = HostMetrics::CpuUsagePercent(a, b);
		QVERIFY(usage.has_value());
		QCOMPARE(qRound(*usage), 50);
		// Garbage and a first-reading (equal totals) yield nothing.
		QVERIFY(!HostMetrics::ParseProcStat("nonsense").ok);
		QVERIFY(!HostMetrics::CpuUsagePercent(a, a).has_value());
	}

	void metricsCpuNameAndMem() {
		QCOMPARE(HostMetrics::ParseCpuName("processor\t: 0\nmodel name\t: AMD Ryzen 9 5900X\n"),
		         QString("AMD Ryzen 9 5900X"));
		QVERIFY(HostMetrics::ParseCpuName("vendor_id\t: x").isEmpty());
		const auto mem = HostMetrics::ParseMemInfo(
		    "MemTotal:       32768000 kB\nMemFree: 1 kB\nMemAvailable:   20480000 kB\n");
		QVERIFY(mem.ok);
		QCOMPARE(mem.total_mb, 32000ull);
		QCOMPARE(mem.used_mb, 12000ull);                           // (32768000-20480000)/1024
		QVERIFY(!HostMetrics::ParseMemInfo("MemTotal: 10 kB").ok); // no MemAvailable
	}

	void metricsPicksCpuTemp() {
		// k10temp Tctl wins over a coretemp Package and a bare chip reading.
		const QList<HostMetrics::TempReading> amd {
		    {"other", 40000}, {"Package id 0", 55000}, {"Tctl", 61500}};
		QCOMPARE(qRound(*HostMetrics::PickCpuTemp(amd)), 62);
		const QList<HostMetrics::TempReading> intel {{"Core 0", 50000}, {"Package id 0", 58000}};
		QCOMPARE(qRound(*HostMetrics::PickCpuTemp(intel)), 58);
		QVERIFY(!HostMetrics::PickCpuTemp({}).has_value());
	}

	void metricsFpsWindowAndFile() {
		const std::array<uint32_t, 10> per_second {60, 48, 59, 60, 60};
		const auto                     w = Common::FpsStatsAggregate(per_second, 5, 16.7);
		QVERIFY(w.has_samples);
		QCOMPARE(w.current, 60.0); // last entry
		QCOMPARE(w.min, 48.0);
		QCOMPARE(w.max, 60.0);
		QCOMPARE(w.window_s, 10u);
		QVERIFY(qAbs(w.frame_ms - 16.7) < 1e-6);
		QVERIFY(qAbs(w.avg - (60 + 48 + 59 + 60 + 60) / 5.0) < 1e-6);

		// Round-trips through the file encoding (shape), then staleness via an explicit ts.
		const auto body = Common::FpsStatsEncode(w);
		QVERIFY(body.find("\"current\":60") != std::string::npos);
		QVERIFY(body.find("\"window_s\":10") != std::string::npos);
		const auto fresh = Common::FpsStatsParse(
		    R"({"ts":100,"current":60,"avg":57,"min":48,"max":60,"frame_ms":16.7,"window_s":10})",
		    /*now=*/101, /*max_age=*/3);
		QVERIFY(fresh.has_value());
		QCOMPARE(fresh->current, 60.0);
		QVERIFY(qAbs(fresh->frame_ms - 16.7) < 1e-6);
		const auto stale = Common::FpsStatsParse(R"({"ts":100,"current":60})", /*now=*/200, 3);
		QVERIFY(!stale.has_value());
		QVERIFY(!Common::FpsStatsParse(R"({"ts":100})", 100, 3).has_value()); // ts-only, no samples
		QVERIFY(!Common::FpsStatsParse("garbage", 100, 3).has_value());
	}

	void metricsRouteShape() {
		GameLibrary lib;
		{
			QString error;
			QVERIFY2(lib.Load(nullptr, &error), qPrintable(error));
		}
		const auto running = lib.Games().first().id;
		HostApi    local {&lib, [running] { return HostApi::Status {false, running}; }};
		auto       r = Call(local, {{"method", "GET"}, {"path", "/kyty/v1/host/metrics"}});
		QCOMPARE(r.status, 200);
		QCOMPARE(r.content_type, QByteArray("application/json"));
		const auto doc = QJsonDocument::fromJson(r.body).object();
		QVERIFY(doc.contains("ts"));
		// The game section carries the running title id and name from the library.
		const auto game = doc["game"].toObject();
		QVERIFY(game["running"].toBool());
		QCOMPARE(game["title_id"].toString(), QString("PPSA00001"));
		QVERIFY(!game["name"].toString().isEmpty());
		// RAM always reads on Linux CI; cpu/gpu/fps may be absent, which the contract allows.
		QVERIFY(doc.contains("ram"));
		// Only GET is allowed.
		QCOMPARE(Call(local, {{"method", "PUT"}, {"path", "/kyty/v1/host/metrics"}}).status, 405);
	}
};

QTEST_MAIN(LauncherHostTests)
#include "LauncherHostTests.moc"
