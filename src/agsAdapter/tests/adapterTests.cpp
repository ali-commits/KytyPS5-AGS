// Drives the built kyty-ags-adapter over protocol 1 framing against a fixture runtime root and
// game folder. Plain checks, no framework.
#include "actions.h"
#include "configuration.h"
#include "framing.h"
#include "gameLibrary.h"
#include "launchPlan.h"
#include "library.h"
#include "trophyItems.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <cstdlib>

#define CHECK(expression)                                                                          \
	do {                                                                                           \
		if (!(expression)) {                                                                       \
			fprintf(stderr, "Check failed at line %d: %s\n", __LINE__, #expression);               \
			abort();                                                                               \
		}                                                                                          \
	} while (false)

namespace {

int g_checks = 0;
#define EXPECT(expression)                                                                         \
	do {                                                                                           \
		CHECK(expression);                                                                         \
		++g_checks;                                                                                \
	} while (false)

void WriteFile(const QString& path, const QByteArray& data) {
	CHECK(QDir().mkpath(QFileInfo(path).absolutePath()));
	QFile file(path);
	CHECK(file.open(QIODevice::WriteOnly));
	CHECK(file.write(data) == data.size());
}

// Noise does not compress: a PNG well over 2 MiB that must be re-encoded.
void WriteNoisePng(const QString& path) {
	QImage pic(1200, 1200, QImage::Format_RGB32);
	for (int y = 0; y < pic.height(); ++y) {
		auto* line = reinterpret_cast<quint32*>(pic.scanLine(y));
		for (int x = 0; x < pic.width(); ++x)
			line[x] = QRandomGenerator::global()->generate() | 0xff000000u;
	}
	CHECK(QDir().mkpath(QFileInfo(path).absolutePath()));
	CHECK(pic.save(path, "PNG"));
	CHECK(QFileInfo(path).size() > (2 << 20));
}

QByteArray ReadAll(const QString& path) {
	QFile file(path);
	CHECK(file.open(QIODevice::ReadOnly));
	return file.readAll();
}

QStringList Strings(const QJsonValue& v) {
	QStringList out;
	for (const auto& s: v.toArray())
		out.append(s.toString());
	return out;
}

QString ArgAfter(const QStringList& argv, const QString& flag) {
	const auto i = argv.indexOf(flag);
	return i >= 0 && i + 1 < argv.size() ? argv[i + 1] : QString {};
}

class Session {
public:
	Session(const QString& program, const QString& cwd, const QString& home) {
		QProcessEnvironment env;
		env.insert(QStringLiteral("PATH"), qEnvironmentVariable("PATH"));
		env.insert(QStringLiteral("HOME"), home);
		env.insert(QStringLiteral("LANG"), QStringLiteral("C.UTF-8"));
		m_process.setProcessEnvironment(env);
		m_process.setWorkingDirectory(cwd);
		m_process.setProcessChannelMode(QProcess::SeparateChannels);
		m_process.setReadChannel(QProcess::StandardOutput);
		m_process.start(program, {});
		CHECK(m_process.waitForStarted(10000));
	}

	~Session() {
		if (m_process.state() != QProcess::NotRunning) {
			m_process.kill();
			m_process.waitForFinished(5000);
		}
	}

	QProcess& Process() { return m_process; }

	void WriteRaw(const QByteArray& data) {
		CHECK(m_process.write(data) == data.size());
		CHECK(m_process.waitForBytesWritten(5000));
	}

	// Next message from stdout, or an empty object on timeout.
	QJsonObject Read(int timeout_ms) {
		QElapsedTimer timer;
		timer.start();
		for (;;) {
			QByteArray payload;
			const auto next = AgsFrame::Take(m_buffer, payload);
			CHECK(next != AgsFrame::Next::TooLarge);
			if (next == AgsFrame::Next::Message) {
				const auto doc = QJsonDocument::fromJson(payload);
				CHECK(doc.isObject());
				return doc.object();
			}
			const auto left = timeout_ms - timer.elapsed();
			if (left <= 0 || m_process.state() == QProcess::NotRunning) return {};
			m_process.waitForReadyRead(static_cast<int>(std::min<qint64>(left, 200)));
			m_buffer.append(m_process.readAllStandardOutput());
		}
	}

	QJsonObject Call(const QString& method, const QJsonObject& params = {},
	                 int timeout_ms = 30000) {
		const int id = ++m_id;
		WriteRaw(AgsFrame::Encode({{"id", id}, {"method", method}, {"params", params}}));
		for (;;) {
			const auto message = Read(timeout_ms);
			CHECK(!message.isEmpty());
			if (!message.contains(QStringLiteral("id"))) {
				m_notices.append(message[QStringLiteral("method")].toString());
				continue;
			}
			CHECK(message[QStringLiteral("id")].toInt() == id);
			return message;
		}
	}

	// The request written in pieces with pauses, so the adapter sees partial reads.
	QJsonObject CallInPieces(const QString& method, const QJsonObject& params,
	                         const QList<int>& cuts) {
		const int  id    = ++m_id;
		const auto frame = AgsFrame::Encode({{"id", id}, {"method", method}, {"params", params}});
		qsizetype  from  = 0;
		for (const int cut: cuts) {
			WriteRaw(frame.mid(from, cut - from));
			QThread::msleep(150);
			from = cut;
		}
		WriteRaw(frame.mid(from));
		const auto message = Read(30000);
		CHECK(message[QStringLiteral("id")].toInt() == id);
		return message;
	}

	QJsonObject Result(const QString& method, const QJsonObject& params = {}) {
		const auto reply = Call(method, params);
		if (reply.contains(QStringLiteral("error"))) {
			fprintf(stderr, "%s failed: %s\n", qPrintable(method),
			        QJsonDocument(reply).toJson(QJsonDocument::Compact).constData());
		}
		CHECK(reply.contains(QStringLiteral("result")));
		return reply[QStringLiteral("result")].toObject();
	}

	QString ErrorCode(const QString& method, const QJsonObject& params = {}) {
		const auto reply = Call(method, params);
		CHECK(reply.contains(QStringLiteral("error")));
		CHECK(!reply[QStringLiteral("error")][QStringLiteral("message")].toString().isEmpty());
		return reply[QStringLiteral("error")][QStringLiteral("code")].toString();
	}

	QStringList m_notices;

private:
	QProcess   m_process;
	QByteArray m_buffer;
	int        m_id = 0;
};

struct Fixture {
	QTemporaryDir dir;
	QString       install, data, runtime, games, game, home, adapter;

	Fixture() {
		CHECK(dir.isValid());
		const QDir root(dir.path());
		install = root.filePath("install");
		data    = root.filePath("data");
		runtime = root.filePath("runtime");
		games   = root.filePath("games");
		home    = root.filePath("home");
		game    = QDir::cleanPath(QDir(games).filePath("Fixture Game"));
		for (const auto& d: {install, data, runtime, games, home})
			CHECK(QDir().mkpath(d));
		// An install folder like the release package: the adapter, kyty_emulator, the preset.
		adapter = QDir(install).filePath("kyty-ags-adapter");
		CHECK(QFile::copy(QStringLiteral(KYTY_TEST_ADAPTER), adapter));
		WriteFile(QDir(install).filePath("kyty_emulator"), "#!/bin/sh\nexit 0\n");
		for (const auto& exe: {adapter, QDir(install).filePath("kyty_emulator")}) {
			CHECK(QFile::setPermissions(exe, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
			                                     QFileDevice::ExeOwner));
		}
		CHECK(QFile::copy(QStringLiteral(KYTY_TEST_PRESET),
		                  QDir(install).filePath("u59-preset.json")));

		WriteFile(QDir(game).filePath("eboot.bin"), "ELF");
		WriteFile(QDir(game).filePath("sce_sys/param.json"),
		          R"({"titleId":"PPSA01234","appVersion":"01.000",
		              "localizedParameters":{"defaultLanguage":"en-US",
		              "en-US":{"titleName":"Fixture Game"}}})");
		QImage icon(64, 64, QImage::Format_RGB32);
		icon.fill(Qt::blue);
		CHECK(icon.save(QDir(game).filePath("sce_sys/icon0.png"), "PNG"));
		WriteNoisePng(QDir(game).filePath("sce_sys/pic0.png"));

		QSettings ini(QDir(runtime).filePath("Kyty.ini"), QSettings::IniFormat);
		ini.setValue("Launcher/game_dirs", QStringList {games});
		ini.sync();
		CHECK(ini.status() == QSettings::NoError);
		WriteFile(QDir(data).filePath("adapter.json"),
		          QJsonDocument(QJsonObject {{"runtime_root", runtime}}).toJson());
	}
};

void TestHello(Session& s) {
	const auto hello = s.Result("hello", {{"protocol", 1}});
	EXPECT(hello["protocol"].toInt() == 1);
	EXPECT(hello["version"].toString() == QStringLiteral(KYTY_AGS_ADAPTER_VERSION));
	EXPECT(Strings(hello["capabilities"]) ==
	       QStringList({"library", "settings", "actions", "native_frames", "metrics"}));
	const auto standard = hello["standard"].toObject();
	EXPECT(standard.size() == 6);
	for (const auto* key: {"resolution", "fps_limit", "aspect", "language", "player_name"}) {
		EXPECT(standard[key].toObject()["level"].toString() == "honoured");
	}
	EXPECT(standard["controller"].toObject() ==
	       QJsonObject({{"level", "fixed"}, {"value", "dualsense"}}));
	const auto controller = hello["controllers"].toArray().at(0).toObject();
	EXPECT(controller["kind"].toString() == "dualsense");
	EXPECT(Strings(controller["features"]) ==
	       QStringList({"gyro", "touchpad", "adaptive_triggers", "haptics"}));
	const auto metrics = hello["metrics"].toArray();
	EXPECT(metrics.size() == 4);
	QStringList keys;
	for (const auto& m: metrics) {
		keys.append(m.toObject()["key"].toString());
		EXPECT(m.toObject()["min_interval_ms"].toInt() == 1000);
	}
	EXPECT(keys ==
	       QStringList({"game_fps", "frame_time_ms", "guest_memory_mb", "shader_compiles"}));
	EXPECT(metrics.at(3).toObject()["type"].toString() == "integer");
}

QString TestLibrary(Session& s, const Fixture& f) {
	const auto games = s.Result("library.list")["games"].toArray();
	EXPECT(games.size() == 1);
	const auto g  = games.at(0).toObject();
	const auto id = QString::fromLatin1(
	    QCryptographicHash::hash(f.game.toUtf8(), QCryptographicHash::Sha256).toHex());
	EXPECT(g["id"].toString() == id);
	EXPECT(g["title"].toString() == "Fixture Game");
	EXPECT(g["platform"].toString() == "ps5");
	EXPECT(g["installed"].toBool());
	EXPECT(g["identity"].toObject() ==
	       QJsonObject({{"scheme", "ps5-title"}, {"value", "PPSA01234"}}));
	// Icon: the game's own small PNG, inside the game folder (a user folder for AGS).
	EXPECT(g["icon"].toString() == QDir(f.game).filePath("sce_sys/icon0.png"));
	// Cover: pic0.png is over 2 MiB, so a re-encoded PNG in the data folder.
	const auto cover = g["cover"].toString();
	EXPECT(cover == QDir(f.data).filePath(QStringLiteral("art/%1/cover.png").arg(id)));
	QFile file(cover);
	EXPECT(file.open(QIODevice::ReadOnly));
	EXPECT(file.size() <= (2 << 20) && file.size() > 0);
	EXPECT(file.read(8) == QByteArray("\x89PNG\r\n\x1a\n", 8));
	for (const auto& path: {g["icon"].toString(), cover}) {
		EXPECT(QDir::isAbsolutePath(path));
		EXPECT(path.startsWith(f.game + '/') || path.startsWith(f.data + '/'));
	}
	return id;
}

void TestSchema(Session& s) {
	const auto fields = s.Result("settings.schema")["fields"].toArray();
	EXPECT(!fields.isEmpty() && fields.size() <= 512);
	static const QRegularExpression key_pattern(QStringLiteral("^[a-z][a-z0-9_.]{0,63}$"));
	QStringList                     keys;
	QStringList                     machine;
	for (const auto& v: fields) {
		const auto f     = v.toObject();
		const auto key   = f["key"].toString();
		const auto type  = f["type"].toString();
		const auto label = f["label"].toString();
		EXPECT(key_pattern.match(key).hasMatch());
		EXPECT(!keys.contains(key));
		keys.append(key);
		EXPECT(!label.isEmpty() && label.size() <= 80);
		EXPECT(f["hint"].toString().size() <= 200 && !f["hint"].toString().contains('\n'));
		EXPECT(QStringList({"bool", "enum", "int", "string", "path", "path_list", "group"})
		           .contains(type));
		EXPECT(QStringList({"", "global", "per_game", "both"}).contains(f["scope"].toString()));
		EXPECT(QStringList({"", "player", "machine"}).contains(f["location"].toString()));
		EXPECT(!f.contains("visible_if"));
		if (f["location"].toString() == "machine") machine.append(key);
		const auto d = f["default"];
		if (type == "group") {
			EXPECT(!f.contains("default") && !f.contains("fields") && !f.contains("title"));
		} else if (type == "bool") {
			EXPECT(d.isBool());
		} else if (type == "int") {
			EXPECT(d.isDouble() && f["min"].toInteger() <= d.toInteger() &&
			       d.toInteger() <= f["max"].toInteger());
		} else if (type == "enum") {
			const auto options = f["options"].toArray();
			EXPECT(!options.isEmpty());
			bool found = false;
			for (const auto& o: options) {
				EXPECT(o.toObject()["value"].isString() &&
				       !o.toObject()["label"].toString().isEmpty());
				found = found || o.toObject()["value"] == d;
			}
			EXPECT(found);
		} else if (type == "path_list") {
			EXPECT(d.isArray());
		} else {
			EXPECT(d.isString());
		}
	}
	for (const auto* omitted: {"screen_resolution", "vblank_frequency", "stream_aspect_mode",
	                           "console_language", "user_name"}) {
		EXPECT(!keys.contains(omitted));
	}
	machine.sort();
	EXPECT(machine == QStringList({"addons_dir", "game_dirs", "gpu_index", "host_input_mapping",
	                               "runtime_root"}));
	EXPECT(keys.contains("present_mode") && keys.contains("controller_color"));
}

void TestSettings(Session& s, const Fixture& f, const QString& id) {
	auto values = s.Result("settings.get", {{"scope", "global"}})["values"].toObject();
	EXPECT(values["trophy_enabled"].toBool());
	EXPECT(values["runtime_root"].toString() == f.runtime);
	EXPECT(Strings(values["game_dirs"]) == QStringList({f.games}));
	values =
	    s.Result("settings.set", {{"scope", "global"},
		                          {"values", QJsonObject {{"trophy_enabled", false},
		                                                  {"audio_master_volume", 150},
		                                                  {"present_mode", "Fifo"}}}})["values"]
	        .toObject();
	EXPECT(!values["trophy_enabled"].toBool());
	values = s.Result("settings.get", {{"scope", "global"}})["values"].toObject();
	EXPECT(!values["trophy_enabled"].toBool());
	EXPECT(values["audio_master_volume"].toInt() == 150);
	EXPECT(values["present_mode"].toString() == "Fifo");

	values = s.Result("settings.set", {{"scope", "game"},
	                                   {"game", id},
	                                   {"values", QJsonObject {{"support_zmask", true}}}})["values"]
	             .toObject();
	EXPECT(values["support_zmask"].toBool());
	values = s.Result("settings.get", {{"scope", "game"}, {"game", id}})["values"].toObject();
	EXPECT(values["support_zmask"].toBool());
	EXPECT(!values.contains("audio_master_volume")); // global only
	values = s.Result("settings.get", {{"scope", "global"}})["values"].toObject();
	EXPECT(!values["support_zmask"].toBool());

	EXPECT(s.ErrorCode("settings.set",
	                   {{"scope", "global"}, {"values", QJsonObject {{"no_such_key", 1}}}}) ==
	       "invalid-params");
	EXPECT(s.ErrorCode("settings.set", {{"scope", "global"},
	                                    {"values", QJsonObject {{"trophy_enabled", "yes"}}}}) ==
	       "invalid-params");
	EXPECT(s.ErrorCode("settings.set", {{"scope", "global"},
	                                    {"values", QJsonObject {{"present_mode", "Bogus"}}}}) ==
	       "invalid-params");
	EXPECT(s.ErrorCode("settings.set", {{"scope", "game"},
	                                    {"game", id},
	                                    {"values", QJsonObject {{"audio_master_volume", 10}}}}) ==
	       "invalid-params");
	EXPECT(s.ErrorCode("settings.get", {{"scope", "game"}, {"game", "missing"}}) == "not-found");
}

void TestLaunch(Session& s, const Fixture& f, const QString& id) {
	auto       plan = s.Result("launch.plan",
	                           {{"game", id},
	                            {"standard", QJsonObject {{"resolution", "1919x1081"},
	                                                      {"fps_limit", 144},
	                                                      {"aspect", "fill"},
	                                                      {"language", "de-DE"},
	                                                      {"player_name", "Ünïcödé player name"}}},
	                            {"player", QJsonObject {{"hdr_enabled", true}, {"gpu_index", "3"}}},
	                            {"capture", "native"},
	                            {"controllers", QJsonArray {"dualsense"}}});
	const auto argv = Strings(plan["argv"]);
	EXPECT(argv.value(0) == QDir(f.install).filePath("kyty_emulator"));
	EXPECT(ArgAfter(argv, "--screen-width") == "1918");
	EXPECT(ArgAfter(argv, "--screen-height") == "1080");
	EXPECT(ArgAfter(argv, "--vblank-frequency") == "120");
	EXPECT(ArgAfter(argv, "--stream-aspect") == "fill");
	EXPECT(ArgAfter(argv, "--console-language") == "4");
	EXPECT(ArgAfter(argv, "--perf-overlay") == "false");
	EXPECT(argv.contains("--hdr"));  // player value applied
	EXPECT(!argv.contains("--gpu")); // machine value from a player is ignored
	const auto name = ArgAfter(argv, "--user-name");
	EXPECT(!name.isEmpty() && name.toUtf8().size() <= 16 &&
	       QString("Ünïcödé player name").startsWith(name));
	EXPECT(ArgAfter(argv, "--game") == QDir(f.game).filePath("eboot.bin"));
	EXPECT(ArgAfter(argv, "--shader-log-folder") == QDir(f.runtime).filePath("_Shaders"));
	EXPECT(ArgAfter(argv, "--printf-output-file") == QDir(f.runtime).filePath("_kyty.txt"));
	EXPECT(argv.indexOf("--game-patch") < 0); // no cheat plan yet
	EXPECT(plan["cwd"].toString() == f.runtime);
	EXPECT(Strings(plan["writable"]) == QStringList({f.runtime}));
	EXPECT(plan["native_frames"].toBool());
	EXPECT(Strings(plan["controllers"]) == QStringList({"dualsense"}));
	auto env = plan["env"].toObject();
	EXPECT(env["KYTY_STREAM_ONLY"].toString() == "1");
	EXPECT(env["KYTY_STREAM_WIDTH"].toString() == "1918");
	EXPECT(env["KYTY_STREAM_HEIGHT"].toString() == "1080");
	EXPECT(env["KYTY_STREAM_FPS"].toString() == "120");
	QFile preset(QStringLiteral(KYTY_TEST_PRESET));
	CHECK(preset.open(QIODevice::ReadOnly));
	const auto preset_env = QJsonDocument::fromJson(preset.readAll()).object();
	EXPECT(!preset_env.isEmpty());
	for (auto it = preset_env.begin(); it != preset_env.end(); ++it) {
		EXPECT(env[it.key()] == it.value());
	}
	for (const auto& key: env.keys())
		EXPECT(!key.startsWith("AGS_"));
	const auto applied = plan["applied"].toObject();
	EXPECT(applied["resolution"].toString() == "1918x1080");
	EXPECT(applied["fps_limit"].toInt() == 120);
	EXPECT(applied["aspect"].toString() == "fill");
	EXPECT(applied["language"].toString() == "de-DE");
	EXPECT(applied["player_name"].toString() == name);
	EXPECT(applied["controller"].toString() == "dualsense");
	EXPECT(applied["hdr_enabled"].toBool());
	EXPECT(!applied.contains("gpu_index"));

	plan = s.Result("launch.plan",
	                {{"game", id},
	                 {"standard", QJsonObject {{"resolution", "9001x10"}, {"fps_limit", 144}}},
	                 {"capture", "gamescope"}});
	const auto argv2 = Strings(plan["argv"]);
	EXPECT(ArgAfter(argv2, "--screen-width") == "8192");
	EXPECT(ArgAfter(argv2, "--screen-height") == "64");
	EXPECT(ArgAfter(argv2, "--vblank-frequency") == "144");
	EXPECT(!plan["native_frames"].toBool());
	EXPECT(!plan["env"].toObject().contains("KYTY_STREAM_ONLY"));

	EXPECT(s.ErrorCode("launch.plan", {{"game", "nope"}}) == "not-found");
	EXPECT(s.ErrorCode("launch.plan",
	                   {{"game", id}, {"player", QJsonObject {{"trophy_duration_seconds", 99}}}}) ==
	       "invalid-params");
	EXPECT(s.Result("launch.ended", {{"game", id}, {"exit_code", 0}}).isEmpty());
}

void TestActions(Session& s, const QString& id) {
	const auto  actions = s.Result("actions.list", {{"game", id}})["actions"].toArray();
	QStringList ids;
	for (const auto& a: actions)
		ids.append(a.toObject()["id"].toString());
	EXPECT(ids == QStringList({"cheats", "cheats.fetch", "trophies", "info"}));
	EXPECT(actions.at(1).toObject()["kind"].toString() == "command");
	EXPECT(actions.at(1).toObject()["confirm"].toBool());
	EXPECT(s.Result("actions.list")["actions"].toArray().isEmpty());
	const auto info =
	    s.Result("actions.run", {{"game", id}, {"action_id", "info"}})["items"].toArray();
	EXPECT(info.at(0).toObject() == QJsonObject({{"label", "Title ID"}, {"value", "PPSA01234"}}));
	const auto cheats = s.Result("actions.run", {{"game", id}, {"action_id", "cheats"}});
	EXPECT(cheats["items"].toArray().isEmpty() && !cheats["message"].toString().isEmpty());
	EXPECT(s.Result("actions.run", {{"game", id}, {"action_id", "trophies"}})["items"]
	           .toArray()
	           .isEmpty());
	EXPECT(s.ErrorCode("actions.run", {{"game", id}, {"action_id", "nope"}}) == "not-found");
}

void TestChangedAndEof(Session& s, const Fixture& f) {
	// Drop notifications caused by the settings.set calls above, then change Kyty.ini from outside.
	while (!s.Read(1000).isEmpty()) {
	}
	s.m_notices.clear();
	{
		QSettings ini(QDir(f.runtime).filePath("Kyty.ini"), QSettings::IniFormat);
		ini.setValue("Launcher/addons_dir", f.home);
		ini.sync();
	}
	bool seen = s.m_notices.contains("library.changed");
	for (int i = 0; i < 20 && !seen; ++i) {
		const auto m = s.Read(250);
		seen         = m["method"].toString() == "library.changed" && !m.contains("id");
	}
	EXPECT(seen);
	EXPECT(s.ErrorCode("no.such.method") == "unknown-method");
	s.Process().closeWriteChannel();
	EXPECT(s.Process().waitForFinished(10000));
	EXPECT(s.Process().exitStatus() == QProcess::NormalExit && s.Process().exitCode() == 0);
}

void TestOversized(const Fixture& f) {
	Session       s(f.adapter, f.data, f.home);
	QByteArray    header(4, '\0');
	const quint32 n = (1u << 20u) + 1u;
	header[0]       = static_cast<char>(n >> 24u);
	header[1]       = static_cast<char>((n >> 16u) & 0xffu);
	header[2]       = static_cast<char>((n >> 8u) & 0xffu);
	header[3]       = static_cast<char>(n & 0xffu);
	s.WriteRaw(header + QByteArray(64, 'x'));
	EXPECT(s.Process().waitForFinished(10000));
	EXPECT(s.Process().exitCode() == 2);
	EXPECT(s.Process().readAllStandardOutput().isEmpty());
}

// Reads Kyty.ini entries the way the launcher does, to see what survived a write.
QStringList GamePaths(const QString& ini_path) {
	QSettings   ini(ini_path, QSettings::IniFormat);
	QStringList paths;
	const int   n = ini.beginReadArray("GameConfigurations");
	for (int i = 0; i < n; ++i) {
		ini.setArrayIndex(i);
		paths.append(ini.value("game_path").toString());
	}
	ini.endArray();
	return paths;
}

// I1: the launcher saved per-game settings for another game after the adapter loaded Kyty.ini; a
// per-game settings.set must keep them.
void TestStaleCache(Session& s, const Fixture& f, const QString& id) {
	(void)s.Result("library.list"); // the adapter's cache is now loaded
	const auto ini_path = QDir(f.runtime).filePath("Kyty.ini");
	{
		QSettings ini(ini_path, QSettings::IniFormat);
		const int n = ini.beginReadArray("GameConfigurations");
		ini.endArray();
		ini.beginWriteArray("GameConfigurations", n + 1);
		ini.setArrayIndex(n);
		ini.setValue("game_path", "/launcher/saved/Other Game");
		ini.setValue("name", "Other Game");
		ini.setValue("basedir", "/launcher/saved/Other Game");
		ini.setValue("custom_settings", "true");
		ini.endArray();
		ini.sync();
		CHECK(ini.status() == QSettings::NoError);
	}
	EXPECT(GamePaths(ini_path).contains("/launcher/saved/Other Game"));
	// Straight away, before the file watcher's debounce could reload the cache.
	(void)s.Result(
	    "settings.set",
	    {{"scope", "game"}, {"game", id}, {"values", QJsonObject {{"support_zmask", false}}}});
	EXPECT(GamePaths(ini_path).contains("/launcher/saved/Other Game"));
	EXPECT(GamePaths(ini_path).contains(f.game));
	// A global set rewrites no per-game data, and keeps both.
	(void)s.Result("settings.set",
	               {{"scope", "global"}, {"values", QJsonObject {{"trophy_enabled", true}}}});
	EXPECT(GamePaths(ini_path).contains("/launcher/saved/Other Game"));
}

// Kyty.ini that QSettings cannot parse: every settings.set is refused and the file is untouched.
void TestMalformedIni(const Fixture& f) {
	const QDir root(f.dir.path());
	const auto data    = root.filePath("data-malformed");
	const auto runtime = root.filePath("runtime-malformed");
	CHECK(QDir().mkpath(data) && QDir().mkpath(runtime));
	WriteFile(QDir(data).filePath("adapter.json"),
	          QJsonDocument(QJsonObject {{"runtime_root", runtime}}).toJson());
	const auto       ini_path = QDir(runtime).filePath("Kyty.ini");
	const QByteArray bad = "this line has no equals sign\n[Launcher]\ngame_dirs=\"unterminated\n";
	WriteFile(ini_path, bad);
	{
		QSettings probe(ini_path, QSettings::IniFormat);
		(void)probe.allKeys();
		CHECK(probe.status() != QSettings::NoError); // the fixture really is malformed for Qt
	}
	Session    s(f.adapter, data, f.home);
	const auto global = s.Call(
	    "settings.set", {{"scope", "global"}, {"values", QJsonObject {{"trophy_enabled", false}}}});
	EXPECT(global["error"]["code"].toString() == "failed");
	EXPECT(global["error"]["message"].toString() == "Kyty.ini could not be read; not saving");
	const auto game = s.Call(
	    "settings.set",
	    {{"scope", "game"}, {"game", "0123"}, {"values", QJsonObject {{"support_zmask", true}}}});
	EXPECT(game["error"]["code"].toString() == "failed");
	EXPECT(game["error"]["message"].toString() == "Kyty.ini could not be read; not saving");
	const auto runtime_only = s.Call(
	    "settings.set", {{"scope", "global"}, {"values", QJsonObject {{"runtime_root", runtime}}}});
	EXPECT(runtime_only["error"]["code"].toString() == "failed");
	EXPECT(ReadAll(ini_path) == bad); // byte for byte
	QStringList names;
	for (const auto& e: QDir(runtime).entryList(QDir::Files | QDir::Hidden))
		names.append(e);
	EXPECT(names == QStringList({"Kyty.ini"})); // no temporary or backup file either
}

// M3: a request is validated whole before anything is written; a runtime_root change is answered
// from the new root.
void TestSettingsAtomic(Session& s, const Fixture& f) {
	const auto before     = s.Result("settings.get", {{"scope", "global"}})["values"].toObject();
	const auto ini_before = ReadAll(QDir(f.runtime).filePath("Kyty.ini"));
	const QDir root(f.dir.path());
	const auto file = root.filePath("not-a-folder");
	WriteFile(file, "x");
	const auto link = root.filePath("link-to-runtime");
	CHECK(QFile::link(f.runtime, link));
	const QJsonObject good {{"audio_master_volume", 77}};
	for (const auto& bad: {QString("relative/path"), root.filePath("missing"), file, link}) {
		auto values            = good;
		values["runtime_root"] = bad;
		EXPECT(s.ErrorCode("settings.set", {{"scope", "global"}, {"values", values}}) ==
		       "invalid-params");
		EXPECT(ReadAll(QDir(f.runtime).filePath("Kyty.ini")) == ini_before);
	}
	EXPECT(s.Result("settings.get", {{"scope", "global"}})["values"].toObject() == before);

	const auto other = root.filePath("runtime-other");
	CHECK(QDir().mkpath(other));
	{
		QSettings ini(QDir(other).filePath("Kyty.ini"), QSettings::IniFormat);
		ini.setValue("GlobalConfiguration/audio_master_volume", 33);
		ini.sync();
	}
	auto values =
	    s.Result("settings.set",
		         {{"scope", "global"}, {"values", QJsonObject {{"runtime_root", other}}}})["values"]
	        .toObject();
	EXPECT(values["runtime_root"].toString() == other);
	EXPECT(values["audio_master_volume"].toInt() == 33); // read from the new root's Kyty.ini
	EXPECT(s.Result("settings.get", {{"scope", "global"}})["values"]
	           .toObject()["audio_master_volume"]
	           .toInt() == 33);
	values =
	    s.Result("settings.set", {{"scope", "global"},
		                          {"values", QJsonObject {{"runtime_root", f.runtime}}}})["values"]
	        .toObject();
	EXPECT(values["runtime_root"].toString() == f.runtime);
	EXPECT(values["audio_master_volume"].toInt() == before["audio_master_volume"].toInt());
}

// M8: framing edge cases.
void TestFraming(const Fixture& f) {
	{
		Session s(f.adapter, f.data, f.home);
		// A frame written in several pieces: inside the length, then inside the JSON.
		auto reply = s.CallInPieces("hello", {{"protocol", 1}}, {2, 7, 20});
		EXPECT(reply["result"].toObject()["protocol"].toInt() == 1);
		// Two requests in one write are both answered, in order.
		const auto two = AgsFrame::Encode({{"id", 101}, {"method", "launch.ended"}}) +
		                 AgsFrame::Encode({{"id", 102}, {"method", "no.such"}});
		s.WriteRaw(two);
		const auto first  = s.Read(10000);
		const auto second = s.Read(10000);
		EXPECT(first["id"].toInt() == 101 && first.contains("result"));
		EXPECT(second["id"].toInt() == 102 &&
		       second["error"]["code"].toString() == "unknown-method");
		// An id that is not an integer is answered with invalid-params, echoing the id.
		for (const QJsonValue& id:
		     {QJsonValue("abc"), QJsonValue(1.5), QJsonValue(QJsonValue::Null)}) {
			s.WriteRaw(AgsFrame::Encode({{"id", id}, {"method", "hello"}}));
			const auto bad = s.Read(10000);
			EXPECT(bad.contains("id") && QJsonValue(bad["id"]) == id);
			EXPECT(bad["error"]["code"].toString() == "invalid-params");
		}
		// A message without id is a notification: no reply, and the adapter carries on.
		s.WriteRaw(AgsFrame::Encode({{"method", "hello"}}));
		EXPECT(s.Result("hello", {{"protocol", 1}})["protocol"].toInt() == 1);
		// params that is not an object.
		s.WriteRaw(AgsFrame::Encode({{"id", 7}, {"method", "hello"}, {"params", 5}}));
		const auto params = s.Read(10000);
		EXPECT(params["id"].toInt() == 7 && params["error"]["code"].toString() == "invalid-params");
	}
	{
		// A frame that is not JSON cannot be answered: the adapter stops with exit code 2 and
		// writes nothing to stdout.
		Session          s(f.adapter, f.data, f.home);
		const QByteArray junk = "{this is not json";
		QByteArray       frame(4, '\0');
		frame[3] = static_cast<char>(junk.size());
		s.WriteRaw(frame + junk);
		EXPECT(s.Process().waitForFinished(10000));
		EXPECT(s.Process().exitCode() == 2);
		EXPECT(s.Process().readAllStandardOutput().isEmpty());
	}
	{
		// Valid JSON that is not an object: same.
		Session          s(f.adapter, f.data, f.home);
		const QByteArray junk = "[1,2,3]";
		QByteArray       frame(4, '\0');
		frame[3] = static_cast<char>(junk.size());
		s.WriteRaw(frame + junk);
		EXPECT(s.Process().waitForFinished(10000));
		EXPECT(s.Process().exitCode() == 2);
	}
}

// In-process checks of the helpers behind M1, M2, M4, M5, M6, M7 and M9.
void TestUnits(const Fixture& f) {
	// M9: control characters and code point cuts.
	EXPECT(AdapterLibrary::DropControls(QString("a\x01") + "b" + QChar(0x7f) + QChar(0x85) +
	                                    "c\n") == "abc");
	EXPECT(AdapterLibrary::DropControls(QString::fromUtf8("PPSA\xF0\x9F\x98\x80\x01")) ==
	       QString::fromUtf8("PPSA\xF0\x9F\x98\x80"));
	EXPECT(AdapterLibrary::DropControls("\x01\x02").isEmpty());
	const auto emoji = QString::fromUtf8("\xF0\x9F\x98\x80"); // one character, two UTF-16 units
	EXPECT(emoji.size() == 2);
	EXPECT(AdapterLibrary::CutChars("a" + emoji + "b", 2) == "a" + emoji);
	EXPECT(AdapterLibrary::CutChars("a" + emoji + "b", 1) == "a");
	const auto cut = AdapterLibrary::CutChars(emoji.repeated(300), 200);
	EXPECT(cut.toUcs4().size() == 200 && cut.size() == 400);
	for (const auto c: cut.toUcs4())
		EXPECT(c == 0x1F600);
	EXPECT(AdapterLibrary::CutChars("short", 200) == "short");

	// M7: names AGS reserves.
	for (const auto* name:
	     {"AGS_X", "AGS_SESSION_RUNTIME", "DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET",
	      "XDG_RUNTIME_DIR", "GAMESCOPE_EIS_SOCKET", "PULSE_SERVER", "PULSE_SINK",
	      "PIPEWIRE_REMOTE", "PIPEWIRE_RUNTIME_DIR"}) {
		EXPECT(AdapterLaunch::IsReservedEnvName(name));
	}
	for (const auto* name:
	     {"KYTY_STREAM_ONLY", "TRACY_NO_EXIT", "PATH", "XDG_DATA_HOME", "display"}) {
		EXPECT(!AdapterLaunch::IsReservedEnvName(name));
	}

	// M1: only the file for the installed version.
	QList<Cheats::RemoteFile> files;
	for (const auto* version: {"01.000", "01.001"}) {
		Cheats::RemoteFile file;
		file.name    = QString("PPSA01234_%1.json").arg(version);
		file.title   = "PPSA01234";
		file.version = version;
		file.format  = "json";
		files.append(file);
	}
	QString error;
	auto    pick = AdapterActions::PickCheatFile(files, "01.001", &error);
	EXPECT(pick && pick->name == "PPSA01234_01.001.json" && error.isEmpty());
	pick = AdapterActions::PickCheatFile(files, "01.002", &error);
	EXPECT(!pick && error == "No cheat file for version 01.002.");
	pick = AdapterActions::PickCheatFile({}, "01.000", &error);
	EXPECT(!pick && error == "No cheat file for version 01.000.");

	// M2: a finished background result is served once, within 60 s.
	{
		FetchResultCache cache;
		cache.Store({"g1", {}, "done"}, 1000);
		auto got = cache.Take("g1", 1000 + FetchResultCache::MaxAgeMs);
		EXPECT(got && got->message == "done");
		EXPECT(!cache.Take("g1", 1000 + 1)); // served once
		cache.Store({"g1", "failed", {}}, 1000);
		EXPECT(!cache.Take("g1", 1000 + FetchResultCache::MaxAgeMs + 1)); // too old: discarded
		EXPECT(!cache.Take("g1", 1000 + 2));                              // and gone
		cache.Store({"g1", {}, "done"}, 1000);
		EXPECT(!cache.Take("g2", 1001)); // another game's result is never returned
		EXPECT(!cache.Take("g1", 1002));
	}

	// M5, M6: items come from one loaded package, ids are `<label>-<id>`.
	{
		QImage image(4, 4, QImage::Format_RGB32);
		image.fill(Qt::red);
		QByteArray png;
		QBuffer    buffer(&png);
		CHECK(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"));
		const auto bytes = [&](const QByteArray& b) {
			std::vector<std::byte> out;
			for (const char c: b)
				out.push_back(static_cast<std::byte>(c));
			return out;
		};
		Common::Trophies::Package package;
		package.trophies[1].id          = 1;
		package.trophies[1].name        = "First";
		package.trophies[1].grade       = 2;
		package.trophies[1].icon_png    = bytes(png);
		package.trophies[2].id          = 2; // no icon
		package.trophies[2].name        = "Plain";
		package.trophies[2].description = "No picture";
		package.trophies[3].id          = 3;
		package.trophies[3].name        = "Secret one";
		package.trophies[3].description = "Do not tell";
		package.trophies[3].hidden      = true;
		package.trophies[3].icon_png    = bytes(png);
		package.trophies[4].id          = 4;
		package.trophies[4].name        = "Secret opened";
		package.trophies[4].hidden      = true;
		package.trophies[4].icon_png    = bytes(png);
		Common::Trophies::UnlockData unlocks;
		unlocks.unlocked.insert(1);
		unlocks.unlocked.insert(4);
		const auto icons  = QDir(f.dir.path()).filePath("trophy-icons");
		const auto items0 = AdapterTrophies::Items(package, unlocks, 0, icons);
		const auto items1 = AdapterTrophies::Items(package, unlocks, 1, icons);
		EXPECT(items0.size() == 4 && items1.size() == 4);
		QStringList ids;
		for (const auto& v: items0)
			ids.append(v.toObject()["id"].toString());
		for (const auto& v: items1)
			ids.append(v.toObject()["id"].toString());
		EXPECT(ids == QStringList({"0-1", "0-2", "0-3", "0-4", "1-1", "1-2", "1-3", "1-4"}));
		EXPECT(QSet<QString>(ids.begin(), ids.end()).size() == ids.size());
		const auto first = items0.at(0).toObject();
		EXPECT(first["label"] == "First" && first["unlocked"].toBool());
		EXPECT(first["description"].toString().startsWith("Gold"));
		EXPECT(first["icon"].toString() == QDir(icons).filePath("0-1.png"));
		EXPECT(ReadAll(first["icon"].toString()) == png);
		const auto plain = items0.at(1).toObject();
		EXPECT(plain["id"] == "0-2" && !plain.contains("icon"));
		EXPECT(plain["description"].toString().endsWith("No picture"));
		const auto secret = items0.at(2).toObject(); // hidden and locked: nothing revealed
		EXPECT(secret["label"] == "Hidden trophy" && !secret.contains("icon"));
		EXPECT(!secret["description"].toString().contains("tell"));
		EXPECT(!QFileInfo::exists(QDir(icons).filePath("0-3.png")));
		const auto opened = items0.at(3).toObject(); // hidden but unlocked: shown
		EXPECT(opened["label"] == "Secret opened" && opened.contains("icon"));
	}

	// M4: a game without art is remembered; art that changes is written again.
	{
		const QDir root(f.dir.path());
		const auto basedir = root.filePath("art-game");
		const auto data    = root.filePath("art-data");
		const auto pic0    = QDir(basedir).filePath("sce_sys/pic0.png");
		CHECK(QDir().mkpath(data));
		WriteFile(pic0, QByteArray(3 << 20, '\x5a')); // over 2 MiB and not an image
		GameEntry game;
		game.id            = "feedface";
		game.info          = std::make_shared<Configuration>();
		game.info->basedir = basedir;
		AdapterLibrary::ArtCache cache;
		EXPECT(AdapterLibrary::ArtPath(game, "cover", data, cache).isEmpty());
		EXPECT(cache.Missing("feedface", "cover") && !cache.Missing("feedface", "icon"));
		WriteNoisePng(pic0);
		EXPECT(AdapterLibrary::ArtPath(game, "cover", data, cache).isEmpty()); // remembered
		AdapterLibrary::ArtCache fresh;
		const auto               target = AdapterLibrary::ArtPath(game, "cover", data, fresh);
		EXPECT(target == QDir(data).filePath("art/feedface/cover.png"));
		EXPECT(QFileInfo(target).size() > 0 && QFileInfo(target).size() <= (2 << 20));
		// Up to date: left alone.
		const auto written = QFileInfo(target).lastModified();
		QThread::msleep(1100);
		EXPECT(AdapterLibrary::ArtPath(game, "cover", data, fresh) == target);
		EXPECT(QFileInfo(target).lastModified() == written);
		// The source is newer than the written art: regenerated.
		QFile source(pic0);
		CHECK(source.open(QIODevice::ReadWrite));
		CHECK(source.setFileTime(written.addSecs(5), QFileDevice::FileModificationTime));
		source.close();
		EXPECT(AdapterLibrary::ArtPath(game, "cover", data, fresh) == target);
		EXPECT(QFileInfo(target).lastModified() > written);
	}
}

} // namespace

int main(int argc, char* argv[]) {
	QCoreApplication app(argc, argv);
	Fixture          f;
	{
		Session s(f.adapter, f.data, f.home);
		TestHello(s);
		const auto id = TestLibrary(s, f);
		TestSchema(s);
		TestSettings(s, f, id);
		TestLaunch(s, f, id);
		TestActions(s, id);
		TestStaleCache(s, f, id);
		TestSettingsAtomic(s, f);
		TestChangedAndEof(s, f);
	}
	TestOversized(f);
	TestMalformedIni(f);
	TestFraming(f);
	TestUnits(f);
	printf("kyty_ags_adapter: %d checks passed\n", g_checks);
	return 0;
}
