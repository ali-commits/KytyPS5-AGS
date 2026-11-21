// Drives the built kyty-ags-adapter over protocol 2 against a fixture runtime root and
// game folder. Plain checks, no framework.
#include "actions.h"
#include "configuration.h"
#include "framing.h"
#include "gameLibrary.h"
#include "launchPlan.h"
#include "library.h"
#include "page.h"
#include "settings.h"
#include "trophyItems.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
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
#include <tuple>
#include <utility>

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
				m_events.append(message);
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
	QList<QJsonObject> m_events;

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

void TestManifest() {
	QFile file(QDir(KYTY_TEST_SOURCE_ROOT).filePath("ags-extension.toml"));
	EXPECT(file.open(QIODevice::ReadOnly | QIODevice::Text));
	const auto lines = QString::fromUtf8(file.readAll()).split('\n');
	EXPECT(lines.contains("protocol = 2"));
	EXPECT(lines.contains("roles = [\"emulator\"]"));
	bool version_ok = false;
	for (const auto& line: lines) {
		if (line.startsWith("version = \"") && line.endsWith('"')) {
			version_ok = QStringLiteral(KYTY_AGS_ADAPTER_VERSION).startsWith(line.mid(11, line.size() - 12));
		}
	}
	EXPECT(version_ok);
}

void TestHello(Session& s) {
	const auto hello = s.Result("hello", {{"protocol", 2}});
	EXPECT(hello["protocol"].toInt() == 2);
	EXPECT(hello["version"].toString() == QStringLiteral(KYTY_AGS_ADAPTER_VERSION));
	EXPECT(Strings(hello["roles"]) == QStringList({"emulator"}));
	EXPECT(Strings(hello["features"]) ==
	       QStringList({"library", "page", "values", "actions", "native_frames", "metrics"}));
	EXPECT(!hello.contains("capabilities") && !hello.contains("standard"));
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
	EXPECT(g["version"].toString() == "01.000"); // from the fixture's param.json appVersion
	EXPECT(!Strings(g["badges"].toArray()).contains("cheats")); // no cheat file yet
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
	// Writing the cheat plan the game's title id resolves to makes the "cheats" badge appear;
	// removing it again drops the badge, and later tests (no cheat plan yet) expect it gone.
	const auto plan_path = QDir(f.runtime).filePath("_Patches/PPSA01234.json");
	WriteFile(plan_path, "{}");
	const auto after = s.Result("library.list")["games"].toArray().at(0).toObject();
	EXPECT(Strings(after["badges"].toArray()).contains("cheats"));
	CHECK(QFile::remove(plan_path));
	return id;
}

// An icon is a safe relative path to a small SVG that ships in assets/ags.
QSet<QString> g_icons; // every icon the adapter published in this run

void ExpectIcon(const QString& icon) {
	g_icons.insert(icon);
	EXPECT(icon.startsWith("assets/ags/") && icon.endsWith(".svg") && !icon.contains(".."));
	QFileInfo file(QDir(KYTY_TEST_SOURCE_ROOT).filePath(icon));
	EXPECT(file.isFile() && !file.isSymLink() && file.size() > 0 && file.size() <= 256 * 1024);
}

// "One line" (sheet 1.3): no control character, `min` to `max` characters.
bool OneLine(const QJsonValue& v, int min, int max) {
	if (!v.isString()) return false;
	const auto chars = v.toString().toUcs4();
	if (chars.size() < min || chars.size() > max) return false;
	for (const auto c: chars) {
		if (QChar::category(c) == QChar::Other_Control) return false;
	}
	return true;
}

void ExpectNoNull(const QJsonValue& v) {
	EXPECT(!v.isNull());
	if (v.isArray()) {
		for (const auto& e: v.toArray())
			ExpectNoNull(e);
	} else if (v.isObject()) {
		const auto o = v.toObject();
		for (auto it = o.begin(); it != o.end(); ++it)
			ExpectNoNull(it.value());
	}
}

// Entry, row and card lists: optional ids 1 to 128 of [A-Za-z0-9._-], unique within the item.
void ExpectEntryIds(const QJsonArray& list, bool id_required) {
	static const QRegularExpression entry_id(QStringLiteral("^[A-Za-z0-9._-]{1,128}$"));
	QSet<QString> ids;
	for (const auto& v: list) {
		const auto e = v.toObject();
		if (!e.contains("id") && !id_required) continue;
		const auto id = e["id"].toString();
		EXPECT(e["id"].isString() && entry_id.match(id).hasMatch() && !ids.contains(id));
		ids.insert(id);
	}
}

// The rules of sheet 1.2.2 and 1.3 on a page.describe result; a host `sanitize` drop fails here.
void ExpectPageRules(const QJsonObject& page, bool game_page) {
	ExpectNoNull(page);
	static const QRegularExpression id_pattern(QStringLiteral("^[a-z0-9][a-z0-9._-]{0,63}$"));
	static const QRegularExpression colour(QStringLiteral("^#[0-9a-f]{6}$"));
	static const QStringList        surfaces {"hero", "panel", "menu", "banner", "overlay"};
	static const QStringList        tabs {"display", "maintenance", "advanced", "about", "source"};
	static const QStringList        value_kinds {"switch", "choice", "slider", "number", "text",
                                          "path",   "list",   "colour", "toggle_list"};
	const auto                      sections = page["sections"].toArray();
	EXPECT(page["sections"].isArray() && sections.size() <= 32);
	QSet<QString> ids;
	const auto    expect_id = [&ids](const QJsonValue& v) {
        const auto id = v.toString();
        EXPECT(v.isString() && id_pattern.match(id).hasMatch() && !id.startsWith("ags.") &&
		          !ids.contains(id));
        ids.insert(id);
	};
	int total = 0;
	for (const auto& sv: sections) {
		const auto section = sv.toObject();
		const auto surface = section["surface"].toString();
		expect_id(section["id"]);
		EXPECT(surfaces.contains(surface));
		if (!game_page) EXPECT(surface == "panel" && !section.contains("tab"));
		if (surface == "panel") {
			if (game_page) EXPECT(tabs.contains(section["tab"].toString()));
		} else {
			EXPECT(!section.contains("tab"));
		}
		if (surface == "panel" || surface == "banner") EXPECT(OneLine(section["title"], 1, 40));
		if (section.contains("icon")) ExpectIcon(section["icon"].toString());
		const auto items = section["items"].toArray();
		EXPECT(items.size() <= 128);
		total += static_cast<int>(items.size());
		for (const auto& iv: items) {
			const auto item = iv.toObject();
			const auto kind = item["kind"].toString();
			expect_id(item["id"]);
			EXPECT(OneLine(item["label"], 1, 80));
			if (item.contains("help")) EXPECT(OneLine(item["help"], 0, 200));
			if (item.contains("disabled_reason")) EXPECT(OneLine(item["disabled_reason"], 1, 80));
			if (item.contains("confirm")) EXPECT(OneLine(item["confirm"], 1, 120));
			if (item.contains("unit")) EXPECT(OneLine(item["unit"], 1, 8));
			if (item.contains("null_label")) {
				EXPECT(OneLine(item["null_label"], 1, 40) && item["nullable"] == true);
			}
			if (surface == "menu") EXPECT(kind == "button");
			if (surface == "banner") EXPECT(kind == "cards" || kind == "rows");
			if (item["launch"] == true) EXPECT(surface == "menu" && kind == "button");
			if (kind == "button" && item.contains("icon")) ExpectIcon(item["icon"].toString());
			const auto options = item["options"].toArray();
			QSet<QString> values;
			for (const auto& o: options) {
				const auto value = o.toObject().value("value");
				EXPECT(OneLine(value, 0, 255) && !values.contains(value.toString()));
				values.insert(value.toString());
				EXPECT(OneLine(o.toObject()["label"], 1, 80));
			}
			if (item["presentation"] == "segmented") EXPECT(options.size() >= 2 && options.size() <= 4);
			const auto palette = item["palette"].toArray();
			EXPECT(palette.size() <= 32);
			for (const auto& p: palette)
				EXPECT(colour.match(p.toObject()["value"].toString()).hasMatch());
			if (!value_kinds.contains(kind)) {
				EXPECT(!item.contains("scope") && !item.contains("default"));
			} else {
				const auto scope = item["scope"].toString();
				EXPECT(game_page ? QStringList({"game_on_device", "device", "game", "machine"}).contains(scope)
				                 : (scope == "device" || scope == "machine"));
				const auto d = item["default"];
				if (kind == "switch") {
					EXPECT(d.isBool());
				} else if (kind == "choice") {
					EXPECT(d.isString() && values.contains(d.toString()));
				} else if (kind == "slider" || kind == "number") {
					EXPECT(d.isDouble() && d.toDouble() == static_cast<double>(d.toInteger()));
					if (item.contains("min")) EXPECT(item["min"].toInteger() <= d.toInteger());
					if (item.contains("max")) EXPECT(d.toInteger() <= item["max"].toInteger());
					if (kind == "slider") EXPECT(item.contains("min") && item.contains("max"));
				} else if (kind == "colour") {
					EXPECT(d.isString() && (d == "" || colour.match(d.toString()).hasMatch()));
				} else if (kind == "list" || kind == "toggle_list") {
					EXPECT(d.isArray());
					for (const auto& e: d.toArray())
						EXPECT(e.isString());
				} else {
					EXPECT(d.isString());
				}
			}
			if (kind == "toggle_list") {
				const auto entries = item["entries"].toArray();
				EXPECT(item["entries"].isArray() && entries.size() <= 512);
				ExpectEntryIds(entries, true);
				for (const auto& e: entries) {
					EXPECT(OneLine(e.toObject()["label"], 1, 80));
					if (e.toObject().contains("description"))
						EXPECT(OneLine(e.toObject()["description"], 0, 300));
				}
			}
			if (kind == "rows") {
				const auto rows = item["rows"].toArray();
				EXPECT(item["rows"].isArray() && rows.size() <= 256);
				ExpectEntryIds(rows, false);
				for (const auto& r: rows) {
					EXPECT(OneLine(r.toObject()["label"], 1, 80));
					EXPECT(OneLine(r.toObject()["value"], 0, 200));
				}
			}
			if (kind == "cards") {
				const auto cards = item["cards"].toArray();
				EXPECT(cards.size() <= 1024);
				ExpectEntryIds(cards, true);
				for (const auto& c: cards) {
					EXPECT(OneLine(c.toObject()["label"], 1, 80));
					if (c.toObject().contains("description"))
						EXPECT(OneLine(c.toObject()["description"], 0, 300));
				}
			}
		}
	}
	EXPECT(total <= 512);
}

// The game page's settings sections (page.describe with a game uses them from Task 4): the
// Kyty.ini fields only, at game_on_device, in tabs.
void TestGameSettingsSections() {
	const auto sections = AdapterPage::SettingsSections(AdapterPage::PageKind::Game);
	ExpectPageRules({{"sections", sections}}, true);
	QHash<QString, QJsonObject> by_id;
	QSet<QString>               items;
	for (const auto& v: sections) {
		const auto section = v.toObject();
		by_id.insert(section["id"].toString(), section);
		for (const auto& i: section["items"].toArray()) {
			EXPECT(i.toObject()["scope"] == "game_on_device");
			items.insert(i.toObject()["id"].toString());
		}
	}
	EXPECT(!by_id.contains("kyty.machine") && by_id.contains("kyty.graphics"));
	EXPECT(by_id["kyty.display"]["tab"] == "display" && by_id["kyty.display"]["title"] == "KytyPS5");
	EXPECT(by_id["kyty.debug"]["tab"] == "advanced" && by_id["kyty.graphics"]["tab"] == "source");
	for (const auto* left_out: {"gpu_index", "runtime_root", "game_dirs", "controller_color",
	                            "audio_master_volume", "gpu_occlusion_accurate"})
		EXPECT(!items.contains(left_out));
	EXPECT(items.contains("present_mode") && items.contains("frame_generation"));
}

// The extension's own page (no game): the protocol 1 schema as panel sections.
void TestExtensionPage(Session& s) {
	const auto page = s.Result("page.describe");
	ExpectPageRules(page, false);
	QStringList                 ids;   // section ids
	QHash<QString, QJsonObject> items; // by item id
	QStringList                 machine;
	for (const auto& sv: page["sections"].toArray()) {
		ids.append(sv.toObject()["id"].toString());
		for (const auto& iv: sv.toObject()["items"].toArray()) {
			const auto item = iv.toObject();
			items.insert(item["id"].toString(), item);
			if (item["scope"] == "machine") {
				machine.append(item["id"].toString());
			} else {
				EXPECT(item["scope"] == "device");
			}
		}
	}
	EXPECT(ids.mid(0, 8) == QStringList({"kyty.display", "kyty.user", "kyty.audio", "kyty.controller",
	                                     "kyty.graphics", "kyty.system", "kyty.debug", "kyty.machine"}));
	EXPECT(ids.size() <= 9 && (ids.size() == 8 || ids[8] == "kyty.other"));
	machine.sort();
	EXPECT(machine == QStringList({"addons_dir", "game_dirs", "gpu_index", "host_input_mapping", "runtime_root"}));
	for (const auto* gone: {"screen_resolution", "vblank_frequency", "stream_aspect_mode", "console_language",
	                        "user_name", "fullscreen_enabled", "hide_cursor_enabled", "hdr_enabled",
	                        "shader_log_folder", "command_buffer_dump_folder", "printf_output_file",
	                        "perf_overlay_enabled"})
		EXPECT(!items.contains(gone));
	const auto light = items["controller_color"];
	EXPECT(light["kind"] == "colour" && light["default"] == "" && light["nullable"] == true &&
	       light["null_label"] == "Game default");
	EXPECT(light["palette"].toArray().at(0).toObject() == QJsonObject({{"value", "#0030ff"}, {"label", "Blue"}}));
	EXPECT(items["audio_master_volume"]["kind"] == "slider" && items["audio_master_volume"]["unit"] == "%");
	EXPECT(items["trophy_duration_seconds"]["kind"] == "number" && items["trophy_duration_seconds"]["unit"] == "s");
	EXPECT(items["controller_keyboard_player"]["kind"] == "choice" &&
	       items["controller_keyboard_player"]["default"] == "0" &&
	       items["controller_keyboard_player"]["options"].toArray().size() == Config::MAX_PLAYERS + 1);
	EXPECT(items["present_mode"]["presentation"] == "segmented");
	EXPECT(items["frame_generation"]["kind"] == "choice" && items["frame_generation"]["default"] == "Off");
	EXPECT(items["frame_generation_flow_scale"]["kind"] == "slider" && items["frame_generation_flow_scale"]["min"] == 25 &&
	       items["frame_generation_flow_scale"]["max"] == 100 && items["frame_generation_flow_scale"]["default"] == 100);
	EXPECT(items["frame_generation_performance_mode"]["kind"] == "switch");
	EXPECT(items["runtime_root"]["kind"] == "path" && items["runtime_root"]["nullable"] == true &&
	       items["runtime_root"]["null_label"] == "Default");
	EXPECT(items["addons_dir"]["null_label"] == "None");
	EXPECT(items["game_dirs"]["kind"] == "list" && items["game_dirs"]["entry"] == "path" && items["game_dirs"]["pick"] == "folder");
	EXPECT(items["host_input_mapping"]["kind"] == "list" && items["host_input_mapping"]["entry"] == "text" &&
	       items["host_input_mapping"]["max_items"] == 256 && items["host_input_mapping"]["max_len"] == 1024);
	EXPECT(s.ErrorCode("settings.schema") == "unknown-method");
	EXPECT(s.ErrorCode("page.describe", {{"game", "no-such-game"}}) == "not-found");
	EXPECT(s.ErrorCode("page.describe", {{"game", 1}}) == "invalid-params");
}

// The light bar colour is #rrggbb in lower case on every path, or empty; the keyboard player is
// an enum on the wire and a number in Kyty.ini.
void TestColourAndKeyboard(Session& s, const Fixture& f, const QString& id) {
	const auto set = [&s](const QJsonObject& values) {
		return s.Result("values.set", {{"scope", "machine"}, {"values", values}})["values"]
		    .toObject();
	};
	auto values = set({{"controller_color", "#0030FF"}, {"controller_keyboard_player", "2"}});
	EXPECT(values["controller_color"] == "#0030ff");
	EXPECT(values["controller_keyboard_player"] == "2");
	{
		QSettings ini(QDir(f.runtime).filePath("Kyty.ini"), QSettings::IniFormat);
		EXPECT(ini.value("GlobalConfiguration/controller_color").toString() == "#0030ff");
		EXPECT(ini.value("GlobalConfiguration/controller_keyboard_player").toString() == "2");
	}
	for (const auto& bad: {QJsonValue("#fff"), QJsonValue("red"), QJsonValue("#80ff0000"),
	                       QJsonValue("0030ff"), QJsonValue(5)}) {
		EXPECT(s.ErrorCode("values.set", {{"scope", "machine"},
		                                    {"values", QJsonObject {{"controller_color", bad}}}}) ==
		       "invalid-params");
	}
	for (const auto& bad: {QJsonValue("5"), QJsonValue(2), QJsonValue("Player 2")}) {
		EXPECT(s.ErrorCode("values.set",
		                   {{"scope", "machine"},
		                    {"values", QJsonObject {{"controller_keyboard_player", bad}}}}) ==
		       "invalid-params");
	}
	EXPECT(s.Result("values.get", {{"scope", "machine"}})["values"].toObject()["controller_color"] ==
	       "#0030ff");

	auto plan = s.Result("launch.plan",
	                     {{"game", id},
	                      {"values", QJsonObject {{"controller_color", "#A0B0C0"},
	                                              {"controller_keyboard_player", "3"}}}});
	auto argv = Strings(plan["argv"]);
	EXPECT(ArgAfter(argv, "--controller-color") == "#a0b0c0");
	EXPECT(ArgAfter(argv, "--keyboard-player") == "3");
	EXPECT(plan["applied"].toObject()["controller_color"] == "#a0b0c0");
	EXPECT(plan["applied"].toObject()["controller_keyboard_player"] == "3");
	for (const auto* bad: {"#fff", "blue", "#80ff0000"}) {
		EXPECT(s.ErrorCode("launch.plan", {{"game", id},
		                                   {"values", QJsonObject {{"controller_color", bad}}}}) ==
		       "invalid-params");
	}
	// Empty lets the game set the colour: no --controller-color at all.
	plan = s.Result("launch.plan", {{"game", id}, {"values", QJsonObject {{"controller_color", ""}}}});
	EXPECT(!Strings(plan["argv"]).contains("--controller-color"));
	values = set({{"controller_color", ""}, {"controller_keyboard_player", "0"}});
	EXPECT(values["controller_color"] == "" && values["controller_keyboard_player"] == "0");
}

void TestValues(Session& s, const Fixture& f, const QString& id) {
	auto values = s.Result("values.get", {{"scope", "machine"}})["values"].toObject();
	EXPECT(values["trophy_enabled"].toBool());
	EXPECT(values["runtime_root"].toString() == f.runtime);
	EXPECT(Strings(values["game_dirs"]) == QStringList({f.games}));
	values =
	    s.Result("values.set", {{"scope", "machine"},
		                          {"values", QJsonObject {{"trophy_enabled", false},
		                                                  {"audio_master_volume", 150},
		                                                  {"present_mode", "Fifo"}}}})["values"]
	        .toObject();
	EXPECT(!values["trophy_enabled"].toBool());
	values = s.Result("values.get", {{"scope", "machine"}})["values"].toObject();
	EXPECT(!values["trophy_enabled"].toBool());
	EXPECT(values["audio_master_volume"].toInt() == 150);
	EXPECT(values["present_mode"].toString() == "Fifo");

	values = s.Result("values.set", {{"scope", "game"},
	                                   {"game", id},
	                                   {"values", QJsonObject {{"support_zmask", true}}}})["values"]
	             .toObject();
	EXPECT(values["support_zmask"].toBool());
	values = s.Result("values.get", {{"scope", "game"}, {"game", id}})["values"].toObject();
	EXPECT(values["support_zmask"].toBool());
	EXPECT(!values.contains("audio_master_volume")); // global only
	// Digit-leading non-enum values are stored as they are, not as RawText ("R10").
	values = s.Result("values.set", {{"scope", "game"},
	                                   {"game", id},
	                                   {"values", QJsonObject {{"user_id", 10}}}})["values"]
	             .toObject();
	EXPECT(values["user_id"].toInt() == 10);
	values = s.Result("values.get", {{"scope", "game"}, {"game", id}})["values"].toObject();
	EXPECT(values["user_id"].toInt() == 10);
	values = s.Result("values.get", {{"scope", "machine"}})["values"].toObject();
	EXPECT(!values["support_zmask"].toBool());

	EXPECT(s.ErrorCode("values.set",
	                   {{"scope", "machine"}, {"values", QJsonObject {{"no_such_key", 1}}}}) ==
	       "invalid-params");
	EXPECT(s.ErrorCode("values.set", {{"scope", "machine"},
	                                    {"values", QJsonObject {{"trophy_enabled", "yes"}}}}) ==
	       "invalid-params");
	EXPECT(s.ErrorCode("values.set", {{"scope", "machine"},
	                                    {"values", QJsonObject {{"present_mode", "Bogus"}}}}) ==
	       "invalid-params");
	EXPECT(s.ErrorCode("values.set", {{"scope", "game"},
	                                    {"game", id},
	                                    {"values", QJsonObject {{"audio_master_volume", 10}}}}) ==
	       "invalid-params");
	// Raw launcher spellings are accepted, stored and read back as the schema option.
	values = s.Result("values.set", {{"scope", "game"},
	                                   {"game", id},
	                                   {"values", QJsonObject {{"present_mode", "Immediate"},
	                                                           {"shader_optimization_type", "Size"}}}})
	             ["values"]
	             .toObject();
	EXPECT(values["present_mode"] == "Immediate" && values["shader_optimization_type"] == "Size");
	EXPECT(s.ErrorCode("values.set", {{"scope", "game"},
	                                    {"game", id},
	                                    {"values", QJsonObject {{"shader_optimization_type", "Huge"},
	                                                            {"present_mode", "Fifo"}}}}) ==
	       "invalid-params");
	values = s.Result("values.get", {{"scope", "game"}, {"game", id}})["values"].toObject();
	EXPECT(values["present_mode"] == "Immediate"); // the rejected request wrote nothing
	EXPECT(s.ErrorCode("values.get", {{"scope", "game"}, {"game", "missing"}}) == "not-found");
	EXPECT(s.ErrorCode("values.get", {{"scope", "device"}}) == "invalid-params");
	EXPECT(s.ErrorCode("values.get", {{"scope", "game"}}) == "invalid-params");
	auto v = s.Result("values.set", {{"scope", "machine"},
	                                {"values", QJsonObject {{"trophy_enabled", QJsonValue::Null},
	                                                        {"addons_dir", QJsonValue::Null}}}})["values"].toObject();
	EXPECT(v["trophy_enabled"].toBool() && v["addons_dir"].toString().isEmpty()); // the defaults
	EXPECT(s.ErrorCode("settings.get", {{"scope", "global"}}) == "unknown-method");
	EXPECT(s.ErrorCode("values.set", {{"scope", "game"}, {"game", id},
	                                  {"values", QJsonObject {{"support_zmask", QJsonValue::Null}}}}) == "invalid-params");
	EXPECT(s.ErrorCode("values.set", {{"scope", "machine"}, {"values", 5}}) == "invalid-params");
}

void TestLaunch(Session& s, const Fixture& f, const QString& id) {
	auto       plan = s.Result("launch.plan",
	                           {{"game", id},
	                            {"standard", QJsonObject {{"display.resolution", "1919x1081"},
	                                                      {"display.fps", 144},
	                                                      {"display.aspect", "fill"},
	                                                      {"language", "de-DE"},
	                                                      {"player_name", "Ünïcödé player name"},
	                                                      {"fullscreen", true},
	                                                      {"display.hide_cursor", true},
	                                                      {"display.hdr", true}}},
	                            {"values", QJsonObject {{"hdr_enabled", false}, {"gpu_index", "3"}}},
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
	EXPECT(argv.contains("--hdr"));  // standard value applied
	EXPECT(argv.contains("--fullscreen") && argv.contains("--hide-cursor"));
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
	EXPECT(applied["display.resolution"].toString() == "1918x1080");
	EXPECT(applied["display.fps"].toInt() == 120);
	EXPECT(applied["display.aspect"].toString() == "fill");
	EXPECT(applied["language"].toString() == "de-DE");
	EXPECT(applied["player_name"].toString() == name);
	EXPECT(applied["input.controller"].toString() == "dualsense");
	EXPECT(applied["display.hdr"].toBool() && applied["fullscreen"].toBool() && applied["display.hide_cursor"].toBool());
	EXPECT(!applied.contains("hdr_enabled"));
	EXPECT(!applied.contains("gpu_index"));

	plan = s.Result("launch.plan",
	                {{"game", id},
	                 {"standard", QJsonObject {{"display.resolution", "9001x10"}, {"display.fps", 144}}},
	                 {"capture", "gamescope"}});
	const auto argv2 = Strings(plan["argv"]);
	EXPECT(ArgAfter(argv2, "--screen-width") == "8192");
	EXPECT(ArgAfter(argv2, "--screen-height") == "64");
	EXPECT(ArgAfter(argv2, "--vblank-frequency") == "144");
	EXPECT(!plan["native_frames"].toBool());
	EXPECT(!plan["env"].toObject().contains("KYTY_STREAM_ONLY"));
	EXPECT(plan["env"].toObject()["KYTY_STREAM_FRAMES"].toString() == "0");
	for (const char* name: {"KYTY_STREAM_WIDTH", "KYTY_STREAM_HEIGHT", "KYTY_STREAM_FPS",
	                        "KYTY_STREAM_ASPECT", "KYTY_STREAM_SOCKET"})
		EXPECT(!plan["env"].toObject().contains(name));
	EXPECT(ArgAfter(argv2, "--perf-overlay") == "false");
	// absent keys keep the game's own setting (default off)
	EXPECT(!argv2.contains("--fullscreen") && !argv2.contains("--hide-cursor") && !argv2.contains("--hdr"));
	EXPECT(!plan["applied"].toObject()["display.hdr"].toBool());
	EXPECT(!env.contains("KYTY_STREAM_FRAMES")); // native plans leave the emulator its frame pool

	plan = s.Result("launch.plan", {{"game", id},
	                                {"values", QJsonObject {{"present_mode", "Fifo"},
	                                                        {"shader_optimization_type", "Size"}}}});
	const auto argv3 = Strings(plan["argv"]);
	EXPECT(ArgAfter(argv3, "--present-mode") == "Fifo");
	EXPECT(ArgAfter(argv3, "--shader-optimization-type") == "Size");
	EXPECT(plan["applied"].toObject()["present_mode"] == "Fifo");
	EXPECT(s.ErrorCode("launch.plan", {{"game", id}, {"values", QJsonObject {{"present_mode", "x"}}}}) ==
	       "invalid-params");

	plan = s.Result("launch.plan",
	                {{"game", id},
	                 {"values", QJsonObject {{"frame_generation", "Double"},
	                                         {"frame_generation_flow_scale", 80},
	                                         {"frame_generation_performance_mode", true}}}});
	const auto argv4 = Strings(plan["argv"]);
	EXPECT(ArgAfter(argv4, "--frame-generation") == "Double");
	EXPECT(ArgAfter(argv4, "--frame-generation-flow-scale") == "80");
	EXPECT(ArgAfter(argv4, "--frame-generation-performance") == "true");
	EXPECT(ArgAfter(argv3, "--frame-generation") == "Off"); // the default
	EXPECT(s.ErrorCode(
	           "launch.plan",
	           {{"game", id}, {"values", QJsonObject {{"frame_generation_flow_scale", 10}}}}) ==
	       "invalid-params");

	EXPECT(s.ErrorCode("launch.plan", {{"game", "nope"}}) == "not-found");
	EXPECT(s.ErrorCode("launch.plan",
	                   {{"game", id}, {"values", QJsonObject {{"trophy_duration_seconds", 99}}}}) ==
	       "invalid-params");

	// "Launch without cheats": offered only with a cheat plan, drops --game-patch, refused otherwise.
	const auto plan_path = QDir(f.runtime).filePath("_Patches/PPSA01234.json");
	EXPECT(s.ErrorCode("launch.plan", {{"game", id}, {"variant", "bogus"}}) == "invalid-params");
	EXPECT(s.ErrorCode("launch.plan", {{"game", id}, {"variant", 5}}) == "invalid-params");
	WriteFile(plan_path, "{}");
	EXPECT(Strings(s.Result("launch.plan", {{"game", id}})["argv"]).contains("--game-patch"));
	const auto plain = Strings(s.Result("launch.plan", {{"game", id}, {"variant", "launch.nocheats"}})["argv"]);
	EXPECT(!plain.contains("--game-patch") && plain.contains("--game"));
	CHECK(QFile::remove(plan_path));

	for (const auto* name: {"german", "koreana"}) {
		const auto want = QString(name) == "german" ? "4" : "9";
		EXPECT(ArgAfter(Strings(s.Result("launch.plan", {{"game", id},
		           {"standard", QJsonObject {{"language", name}}}})["argv"]), "--console-language") == want);
	}
	EXPECT(s.ErrorCode("launch.plan", {{"game", id}, {"variant", "launch.cheats"}}) == "invalid-params");
}

// Every default the pages declare must launch (Review Focus 1), and so must the values the host
// resolves with nothing stored on a device (sheets 2.8 and 3.4).
void TestLaunchWithPageDefaults(Session& s, const QString& id) {
	const auto game_page = s.Result("page.describe", {{"game", id}});
	const auto ext_page  = s.Result("page.describe");
	static const QStringList value_kinds {"switch", "choice", "slider", "number", "text",
	                                      "path",   "list",   "colour", "toggle_list"};
	const auto stored_game    = s.Result("values.get", {{"scope", "game"}, {"game", id}})["values"].toObject();
	const auto stored_machine = s.Result("values.get", {{"scope", "machine"}})["values"].toObject();
	QJsonObject defaults, resolved;
	for (const auto* page: {&game_page, &ext_page}) {
		const bool is_game = page == &game_page;
		for (const auto& sv: (*page)["sections"].toArray()) {
			for (const auto& iv: sv.toObject()["items"].toArray()) {
				const auto item  = iv.toObject();
				const auto key   = item["id"].toString();
				if (!value_kinds.contains(item["kind"].toString())) continue;
				if (!is_game && item["scope"] != "device") continue;
				const auto def = item["default"];
				if (!def.isNull() && !def.isUndefined() && !defaults.contains(key)) defaults.insert(key, def);
				auto value = def;
				if (stored_game.contains(key) && is_game) value = stored_game[key];
				else if (stored_machine.contains(key)) value = stored_machine[key];
				if (!value.isNull() && !value.isUndefined() && !resolved.contains(key)) resolved.insert(key, value);
			}
		}
	}
	for (const auto& values: {defaults, resolved}) {
		const auto plan = s.Result("launch.plan", {{"game", id}, {"values", values}});
		EXPECT(ArgAfter(Strings(plan["argv"]), "--frame-generation") == "Off");
	}
	EXPECT(s.ErrorCode("launch.plan", {{"game", id}, {"variant", 5}}) == "invalid-params");
	EXPECT(s.ErrorCode("launch.plan", {{"game", id}, {"values", 5}}) == "invalid-params");
}

// page.describe with a game: cheats, the Kyty.ini settings, info, content, trophies, the variant.
void TestGamePage(Session& s, const Fixture& f, const QString& id) {
	const auto describe = [&] {
		const auto page = s.Result("page.describe", {{"game", id}});
		ExpectPageRules(page, true);
		QHash<QString, QJsonObject> by_id;
		QStringList                 order;
		for (const auto& v: page["sections"].toArray()) {
			by_id.insert(v.toObject()["id"].toString(), v.toObject());
			order.append(v.toObject()["id"].toString());
		}
		return std::make_pair(order, by_id);
	};
	auto [order, by_id] = describe();
	QStringList expected;
	for (const auto& v: AdapterPage::SettingsSections(AdapterPage::PageKind::Game))
		expected.append(v.toObject()["id"].toString());
	EXPECT(!expected.isEmpty() && !expected.contains("kyty.machine"));
	expected.prepend("kyty.cheats");
	expected.append("kyty.info");
	EXPECT(order == expected);
	for (const auto& section: by_id) {
		if (section["id"] == "kyty.cheats") continue; // game scope, checked in TestCheats
		for (const auto& i: section["items"].toArray()) {
			const auto item = i.toObject();
			if (item.contains("scope")) EXPECT(item["scope"] == "game_on_device");
			for (const auto* device_wide: {"controller_color", "controller_speaker_volume",
			                               "audio_master_volume", "controller_keyboard_player",
			                               "runtime_root", "game_dirs", "addons_dir", "gpu_index"})
				EXPECT(item["id"] != device_wide);
		}
	}
	EXPECT(by_id["kyty.display"]["tab"] == "display" &&
	       by_id["kyty.display"]["title"] == "KytyPS5");
	EXPECT(by_id["kyty.debug"]["tab"] == "advanced" && by_id["kyty.graphics"]["tab"] == "source");
	const auto info = by_id["kyty.info"];
	EXPECT(info["surface"] == "panel" && info["tab"] == "advanced" &&
	       info["title"] == "Game info" && info["icon"] == "assets/ags/actions/info.svg");
	const auto info_item = info["items"].toArray().at(0).toObject();
	EXPECT(info["items"].toArray().size() == 1 && info_item["id"] == "info" &&
	       info_item["kind"] == "rows" && info_item["label"] == "Game info");
	const auto rows = info_item["rows"].toArray();
	EXPECT(rows.size() >= 2 &&
	       rows.at(0).toObject() == QJsonObject({{"label", "Title ID"}, {"value", "PPSA01234"}}) &&
	       rows.at(1).toObject() == QJsonObject({{"label", "Version"}, {"value", "01.000"}}));
	for (const auto& r: rows) {
		EXPECT(r.toObject()["label"] != "Location" && !r.toObject().contains("id"));
		EXPECT(!r.toObject()["value"].toString().contains(f.game));
	}
	EXPECT(s.ErrorCode("page.describe", {{"game", "missing"}}) == "not-found");

	// Additional content, from today's add-on fixture.
	const auto addons = QDir(f.dir.path()).filePath("addons");
	WriteFile(QDir(addons).filePath("dlc1/sce_sys/param.json"),
	          R"({"contentId":"EP0000-PPSA01234_00-ABCDEFGH12345678","titleName":"Extra Pack"})");
	// Another title, and an application, are not additional content for this game.
	WriteFile(QDir(addons).filePath("other/sce_sys/param.json"),
	          R"({"contentId":"EP0000-PPSA09999_00-ABCDEFGH12345678","titleName":"Other"})");
	// A param.json symlinked from outside the addons folder is ignored.
	WriteFile(QDir(f.dir.path()).filePath("outside/param.json"),
	          R"({"contentId":"EP0000-PPSA01234_00-ZZZZZZZZ12345678","titleName":"Escaped"})");
	QDir().mkpath(QDir(addons).filePath("dlc2/sce_sys"));
	CHECK(QFile::link(QDir(f.dir.path()).filePath("outside/param.json"),
	                  QDir(addons).filePath("dlc2/sce_sys/param.json")));
	CHECK(s.Result("values.set",
	               {{"scope", "machine"}, {"values", QJsonObject {{"addons_dir", addons}}}})
	          .contains("values"));
	std::tie(order, by_id) = describe();
	EXPECT(order.endsWith("kyty.content") &&
	       order.indexOf("kyty.content") == order.indexOf("kyty.info") + 1);
	EXPECT(by_id["kyty.content"] ==
	       QJsonObject({{"id", "kyty.content"},
	                    {"surface", "panel"},
	                    {"tab", "about"},
	                    {"title", "Additional content"},
	                    {"icon", "assets/ags/actions/content.svg"},
	                    {"items", QJsonArray {QJsonObject {
	                                  {"id", "content"},
	                                  {"kind", "rows"},
	                                  {"label", "Additional content"},
	                                  {"alias", "content"},
	                                  {"rows", QJsonArray {QJsonObject {
	                                               {"id", "EP0000-PPSA01234_00-ABCDEFGH12345678"},
	                                               {"label", "Extra Pack"},
	                                               {"value", "ABCDEFGH12345678"}}}},
	                                  {"folder", addons}}}}}));
	CHECK(s.Result("values.set", {{"scope", "machine"},
	                                {"values", QJsonObject {{"addons_dir", ""}}}})
	          .contains("values"));

	// The launch variant, only with a cheat plan.
	const auto plan_path = QDir(f.runtime).filePath("_Patches/PPSA01234.json");
	WriteFile(plan_path, "{}");
	std::tie(order, by_id) = describe();
	EXPECT(order.endsWith("kyty.variants") && !order.contains("kyty.content"));
	const auto variants = by_id["kyty.variants"];
	EXPECT(variants["surface"] == "menu" && !variants.contains("tab") &&
	       variants["items"].toArray().size() == 1);
	const auto variant = variants["items"].toArray().at(0).toObject();
	EXPECT(variant["id"] == "launch.nocheats" && variant["kind"] == "button" &&
	       variant["label"] == "Launch without cheats" && variant["launch"] == true);
	ExpectIcon(variant["icon"].toString());
	EXPECT(variant["icon"] == "assets/ags/actions/launch-nocheats.svg");
	CHECK(QFile::remove(plan_path));
	EXPECT(!describe().second.contains("kyty.variants"));
}

// kyty.cheats: the toggle list, its game-scope values and the Fetch cheats button.
void TestCheats(Session& s, const Fixture& f, const QString& id) {
	const auto cheats_section = [&] {
		const auto page = s.Result("page.describe", {{"game", id}});
		ExpectPageRules(page, true);
		const auto sections = page["sections"].toArray();
		EXPECT(sections.at(0).toObject()["id"] == "kyty.cheats");
		return sections.at(0).toObject();
	};
	const auto on = [&] {
		const auto values =
		    s.Result("values.get", {{"scope", "game"}, {"game", id}})["values"].toObject();
		EXPECT(values["cheats"].isArray());
		return values["cheats"].toArray();
	};
	// No plan file yet: no entries, a hint to fetch, and the button.
	auto section = cheats_section();
	EXPECT(section["surface"] == "panel" && section["tab"] == "source" &&
	       section["title"] == "Cheats" && section["icon"] == "assets/ags/actions/cheats.svg");
	auto items = section["items"].toArray();
	EXPECT(items.size() == 2);
	auto list = items.at(0).toObject();
	EXPECT(list["id"] == "cheats" && list["kind"] == "toggle_list" && list["label"] == "Cheats" &&
	       list["alias"] == "cheats" && list["scope"] == "game" && list["default"].isArray() &&
	       list["default"].toArray().isEmpty());
	EXPECT(list["entries"].toArray().isEmpty() && !list["help"].toString().isEmpty() &&
	       !list.contains("disabled_reason"));
	EXPECT(items.at(1).toObject() ==
	       QJsonObject({{"id", "cheats.fetch"},
	                    {"kind", "button"},
	                    {"label", "Fetch cheats"},
	                    {"alias", "cheats_fetch"},
	                    {"confirm", "Download the cheat file for this game?"},
	                    {"icon", "assets/ags/actions/fetch.svg"}}));
	EXPECT(on() == QJsonArray {});
	// The declared default with another game key, never fetched: both written, no plan file made.
	const auto defaults =
	    s.Result("values.set",
		         {{"scope", "game"},
		          {"game", id},
		          {"values", QJsonObject {{"cheats", QJsonArray {}}, {"user_id", 11}}}})["values"]
	        .toObject();
	EXPECT(defaults["user_id"].toInt() == 11 && defaults["cheats"].toArray().isEmpty());
	EXPECT(!QFile::exists(QDir(f.runtime).filePath("_Patches/PPSA01234.json")));

	// A cheat file that does not parse: no entries, disabled, nothing written.
	const auto plan = QDir(f.runtime).filePath("_Patches/PPSA01234.json");
	WriteFile(plan, "{}");
	list = cheats_section()["items"].toArray().at(0).toObject();
	EXPECT(list["entries"].toArray().isEmpty() &&
	       list["disabled_reason"] == "Cannot read the cheat file.");
	EXPECT(on() == QJsonArray {});
	EXPECT(s.ErrorCode("values.set", {{"scope", "game"},
	                                  {"game", id},
	                                  {"values", QJsonObject {{"cheats", QJsonArray {}}}}}) ==
	       "failed");
	EXPECT(ReadAll(plan) == "{}");

	// The plan document of src/launcher/tests/gameServicesTests.cpp for this game.
	const auto mod = [](const QString& name, bool on, const char* offset) {
		return QJsonObject {
		    {"name", name},
		    {"enabled", on},
		    {"memory", QJsonArray {QJsonObject {{"offset", offset}, {"off", "00"}, {"on", "01"}}}}};
	};
	WriteFile(plan,
	          QJsonDocument(QJsonObject {{"id", "PPSA01234"},
	                                     {"version", "01.000"},
	                                     {"process", "eboot.bin"},
	                                     {"mods", QJsonArray {mod("A", false, "10"),
	                                                          mod(QString(100, 'B'), true, "20"),
	                                                          mod("\n", false, "30")}}})
	              .toJson());
	list = cheats_section()["items"].toArray().at(0).toObject();
	EXPECT(list["entries"].toArray() ==
	       QJsonArray({QJsonObject {{"id", "0"}, {"label", "A"}},
	                   QJsonObject {{"id", "1"}, {"label", QString(80, 'B')}},
	                   QJsonObject {{"id", "2"}, {"label", "Cheat 3"}}}));
	EXPECT(!list.contains("help") && !list.contains("disabled_reason"));
	EXPECT(on() == QJsonArray({"1"}));
	// An id that is not an entry now, a repeated id, a non-string, a non-array, or a valid
	// selection next to a bad setting: refused whole.
	const auto before  = ReadAll(plan);
	const auto refused = [&](const QJsonObject& values) {
		return s.ErrorCode("values.set", {{"scope", "game"}, {"game", id}, {"values", values}});
	};
	EXPECT(refused({{"cheats", QJsonArray {"0", "7"}}}) == "invalid-params");
	EXPECT(refused({{"cheats", QJsonArray {"0", "0"}}}) == "invalid-params");
	EXPECT(refused({{"cheats", QJsonArray {0}}}) == "invalid-params");
	EXPECT(refused({{"cheats", "0"}}) == "invalid-params");
	EXPECT(refused({{"cheats", QJsonArray {"0"}}, {"nope", true}}) == "invalid-params");
	EXPECT(ReadAll(plan) == before);
	const auto set = s.Result(
	    "values.set",
	    {{"scope", "game"}, {"game", id}, {"values", QJsonObject {{"cheats", QJsonArray {"0"}}}}});
	EXPECT(set["values"].toObject()["cheats"].toArray() == QJsonArray({"0"}));
	EXPECT(
	    s.Result("values.set", {{"scope", "game"},
		                        {"game", id},
		                        {"values", QJsonObject {{"cheats", QJsonValue::Null}}}})["values"]
	        .toObject()["cheats"]
	        .toArray()
	        .isEmpty());
	EXPECT(on() == QJsonArray {});

	// action.run: only cheats.fetch (not pressed here: it needs the network).
	EXPECT(s.ErrorCode("action.run", {{"game", id}, {"item", "nope"}}) == "not-found");
	EXPECT(s.ErrorCode("action.run", {{"item", "cheats.fetch"}}) == "not-found");
	EXPECT(s.ErrorCode("action.run", {{"game", id}}) == "invalid-params");
	EXPECT(s.ErrorCode("action.run", {{"game", id}, {"item", 1}}) == "invalid-params");
	EXPECT(s.ErrorCode("actions.list", {{"game", id}}) == "unknown-method");
	EXPECT(s.ErrorCode("actions.run", {{"game", id}, {"action_id", "cheats"}}) == "unknown-method");
	CHECK(QFile::remove(plan));
}

void TestInputMapping(Session& s) {
	const auto get = [&] {
		return s.Result("values.get", {{"scope", "machine"}})["values"].toObject();
	};
	const auto set = [&](const QJsonValue& v) {
		return s.ErrorCode("values.set", {{"scope", "machine"},
		                                    {"values", QJsonObject {{"host_input_mapping", v}}}});
	};
	EXPECT(get()["host_input_mapping"].isArray() && get()["host_input_mapping"].toArray().isEmpty());
	const QJsonArray mapping {"Cross=Key:Z", "Circle=Key:X, Shift", ""};
	const auto       saved = s.Result("values.set", {{"scope", "machine"},
	                                                   {"values", QJsonObject {{"host_input_mapping", mapping}}}})["values"]
	                       .toObject();
	EXPECT(saved["host_input_mapping"].toArray() == mapping);
	EXPECT(get()["host_input_mapping"].toArray() == mapping);
	QJsonArray too_many;
	for (int i = 0; i < 257; ++i)
		too_many.append("a");
	EXPECT(set(too_many) == "invalid-params");
	EXPECT(set(QJsonArray {QString(1025, 'a')}) == "invalid-params");
	EXPECT(set(QJsonArray {1}) == "invalid-params");
	// max_len counts characters: 600 non-BMP characters are 1200 UTF-16 units but fit.
	const auto emoji = [](int n) { return QString::fromUcs4(std::u32string(n, U'\U0001F600').c_str()); };
	EXPECT(set(QJsonArray {emoji(1025)}) == "invalid-params");
	EXPECT(set("Cross=Key:Z") == "invalid-params");
	EXPECT(get()["host_input_mapping"].toArray() == mapping); // rejected requests changed nothing
	CHECK(s.Result("values.set", {{"scope", "machine"},
	                                {"values", QJsonObject {{"host_input_mapping", QJsonArray {emoji(600)}}}}})
	          .contains("values"));
	QJsonArray max_ok;
	for (int i = 0; i < 256; ++i)
		max_ok.append(QString(1024, 'b'));
	EXPECT(s.Result("values.set", {{"scope", "machine"},
	                                 {"values", QJsonObject {{"host_input_mapping", max_ok}}}})["values"]
	           .toObject()["host_input_mapping"].toArray() == max_ok);
	EXPECT(s.Result("values.set", {{"scope", "machine"},
	                                 {"values", QJsonObject {{"host_input_mapping", QJsonArray {}}}}})
	           .contains("values"));
}

void TestSessionEvents(Session& s, const QString& id) {
	EXPECT(s.Result("session.started", {{"game", id}, {"session", "launch-1"}}).isEmpty());
	s.m_events.clear();
	EXPECT(s.Result("session.ended", {{"game", id}, {"session", "launch-1"}, {"exit_code", 0}}).isEmpty());
	EXPECT(s.m_events.contains(QJsonObject({{"method", "page.changed"}, {"params", QJsonObject {{"game", id}}}})));
	for (const auto* gone: {"launch.ended", "settings.schema", "settings.get", "settings.set", "actions.list", "actions.run"})
		EXPECT(s.ErrorCode(gone, {{"game", id}}) == "unknown-method");
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
	    "values.set",
	    {{"scope", "game"}, {"game", id}, {"values", QJsonObject {{"support_zmask", false}}}});
	EXPECT(GamePaths(ini_path).contains("/launcher/saved/Other Game"));
	EXPECT(GamePaths(ini_path).contains(f.game));
	// A global set rewrites no per-game data, and keeps both.
	(void)s.Result("values.set",
	               {{"scope", "machine"}, {"values", QJsonObject {{"trophy_enabled", true}}}});
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
	    "values.set", {{"scope", "machine"}, {"values", QJsonObject {{"trophy_enabled", false}}}});
	EXPECT(global["error"]["code"].toString() == "failed");
	EXPECT(global["error"]["message"].toString() == "Kyty.ini could not be read; not saving");
	const auto game = s.Call(
	    "values.set",
	    {{"scope", "game"}, {"game", "0123"}, {"values", QJsonObject {{"support_zmask", true}}}});
	EXPECT(game["error"]["code"].toString() == "failed");
	EXPECT(game["error"]["message"].toString() == "Kyty.ini could not be read; not saving");
	const auto runtime_only = s.Call(
	    "values.set", {{"scope", "machine"}, {"values", QJsonObject {{"runtime_root", runtime}}}});
	EXPECT(runtime_only["error"]["code"].toString() == "failed");
	EXPECT(ReadAll(ini_path) == bad); // byte for byte
	QStringList names;
	for (const auto& e: QDir(runtime).entryList(QDir::Files | QDir::Hidden))
		names.append(e);
	EXPECT(names == QStringList({"Kyty.ini"})); // no temporary or backup file either
}

// A broken runtime folder must not hide the page that fixes it.
void TestExtensionPageWithoutRuntime(const Fixture& f) {
	const auto data = QDir(f.dir.path()).filePath("data-no-runtime");
	CHECK(QDir().mkpath(data));
	WriteFile(QDir(data).filePath("adapter.json"),
	          QJsonDocument(QJsonObject {{"runtime_root", QDir(f.dir.path()).filePath("missing-runtime")}})
	              .toJson());
	Session s(f.adapter, data, f.home);
	EXPECT(!s.Result("page.describe")["sections"].toArray().isEmpty());
	EXPECT(s.ErrorCode("values.get", {{"scope", "machine"}}) == "failed");
	const auto fixed = QDir(f.dir.path()).filePath("runtime-fixed");
	CHECK(QDir().mkpath(fixed));
	EXPECT(s.ErrorCode("values.set", {{"scope", "machine"}, {"values", QJsonObject {{"runtime_root", 5}}}}) ==
	       "invalid-params");
	EXPECT(s.ErrorCode("values.get", {{"scope", "machine"}}) == "failed"); // adapter.json untouched
	EXPECT(s.Result("values.set", {{"scope", "machine"},
	                              {"values", QJsonObject {{"runtime_root", fixed}}}})["values"]
	           .toObject()["runtime_root"] == fixed);
	EXPECT(!s.Result("values.get", {{"scope", "machine"}})["values"].toObject().isEmpty());
}

// M3: a request is validated whole before anything is written; a runtime_root change is answered
// from the new root.
void TestSettingsAtomic(Session& s, const Fixture& f) {
	const auto before     = s.Result("values.get", {{"scope", "machine"}})["values"].toObject();
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
		EXPECT(s.ErrorCode("values.set", {{"scope", "machine"}, {"values", values}}) ==
		       "invalid-params");
		EXPECT(ReadAll(QDir(f.runtime).filePath("Kyty.ini")) == ini_before);
	}
	EXPECT(s.Result("values.get", {{"scope", "machine"}})["values"].toObject() == before);

	const auto other = root.filePath("runtime-other");
	CHECK(QDir().mkpath(other));
	{
		QSettings ini(QDir(other).filePath("Kyty.ini"), QSettings::IniFormat);
		ini.setValue("GlobalConfiguration/audio_master_volume", 33);
		ini.sync();
	}
	auto values =
	    s.Result("values.set",
		         {{"scope", "machine"}, {"values", QJsonObject {{"runtime_root", other}}}})["values"]
	        .toObject();
	EXPECT(values["runtime_root"].toString() == other);
	EXPECT(values["audio_master_volume"].toInt() == 33); // read from the new root's Kyty.ini
	EXPECT(s.Result("values.get", {{"scope", "machine"}})["values"]
	           .toObject()["audio_master_volume"]
	           .toInt() == 33);
	values =
	    s.Result("values.set", {{"scope", "machine"},
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
		auto reply = s.CallInPieces("hello", {{"protocol", 2}}, {2, 7, 20});
		EXPECT(reply["result"].toObject()["protocol"].toInt() == 2);
		// Two requests in one write are both answered, in order.
		const auto two = AgsFrame::Encode({{"id", 101}, {"method", "session.started"}}) +
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
		EXPECT(s.Result("hello", {{"protocol", 2}})["protocol"].toInt() == 2);
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
	// The cheat plan key is the same trimmed upper-case id everywhere (library, actions, launch.plan).
	Configuration padded;
	padded.title_id = QStringLiteral("  ppsa01234\t");
	EXPECT(AdapterActions::CheatTitleId(padded) == "PPSA01234");
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
		// 2024-03-05T01:02:03Z, stored as microseconds since 0001-01-01
		unlocks.timestamps[1] = Common::Trophies::UnixEpochTick + 1709600523ULL * 1000000ULL;
		unlocks.timestamps[4] = Common::Trophies::UnixEpochTick + 1709600523ULL * 1000000ULL + 999999ULL;
		unlocks.timestamps[2] = Common::Trophies::UnixEpochTick - 1ULL;
		unlocks.unlocked.insert(2);
		const auto icons  = QDir(f.dir.path()).filePath("trophy-icons");
		const auto items0 = AdapterTrophies::Cards(package, unlocks, 0, icons);
		const auto items1 = AdapterTrophies::Cards(package, unlocks, 1, icons);
		// The banner around them; its icon is installed (the fixture game has no trophies).
		EXPECT(AdapterTrophies::Banner({}).isEmpty());
		const auto banner = AdapterTrophies::Banner(items0);
		EXPECT(banner["id"] == "kyty.trophies" && banner["surface"] == "banner" &&
		       banner["title"] == "Trophies" && !banner.contains("tab"));
		const auto trophies = banner["items"].toArray();
		EXPECT(trophies.size() == 1 && trophies.at(0).toObject()["id"] == "trophies" &&
		       trophies.at(0).toObject()["kind"] == "cards" &&
		       trophies.at(0).toObject()["alias"] == "trophies" &&
		       trophies.at(0).toObject()["cards"].toArray() == items0);
		ExpectIcon(banner["icon"].toString());
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
		EXPECT(first["tier"] == "gold" && !first["hidden"].toBool() && !first.contains("description"));
		EXPECT(first["unlocked_at"] == "2024-03-05T01:02:03Z");
		EXPECT(first["image"].toString() == QDir(icons).filePath("0-1.png"));
		EXPECT(ReadAll(first["image"].toString()) == png);
		const auto plain = items0.at(1).toObject();
		EXPECT(plain["id"] == "0-2" && !plain.contains("image"));
		EXPECT(plain["description"] == "No picture" && plain["tier"] == "bronze");
		EXPECT(!plain.contains("unlocked_at")); // tick below the epoch: omitted
		const auto secret = items0.at(2).toObject(); // hidden and locked: nothing revealed
		EXPECT(secret["label"] == "Hidden trophy" && !secret.contains("image"));
		EXPECT(secret["hidden"].toBool() && !secret.contains("description"));
		EXPECT(!secret.contains("unlocked_at"));
		EXPECT(!QFileInfo::exists(QDir(icons).filePath("0-3.png")));
		const auto opened = items0.at(3).toObject(); // hidden but unlocked: shown
		EXPECT(opened["label"] == "Secret opened" && opened.contains("image") &&
		       opened["hidden"].toBool());
		EXPECT(opened["unlocked_at"] == "2024-03-05T01:02:03Z"); // sub-second part truncates
		// Text from the game's files is cut and stripped to the card limits (sheet 1.3).
		Common::Trophies::Package long_text;
		long_text.trophies[5].id          = 5;
		long_text.trophies[5].name        = std::string(60, 'n') + "\n" + std::string(59, 'm');
		long_text.trophies[5].description = std::string(400, 'd');
		const auto card = AdapterTrophies::Cards(long_text, {}, 0, icons).at(0).toObject();
		EXPECT(OneLine(card["label"], 80, 80) && OneLine(card["description"], 300, 300));
		// A name of control characters only leaves a blank label: the "Trophy <id>" fallback.
		long_text.trophies[5].name = "\n\t";
		EXPECT(AdapterTrophies::Cards(long_text, {}, 0, icons).at(0).toObject()["label"] ==
		       "Trophy 0-5");
	}

	// Raw enum spellings are aliases of the schema options.
	{
		const QJsonObject resolution {
		    {"type", "enum"},
		    {"options", QJsonArray {QJsonObject {{"value", "1280x720"}}, QJsonObject {{"value", "1920x1080"}}}}};
		const QJsonObject mode {
		    {"type", "enum"},
		    {"options", QJsonArray {QJsonObject {{"value", "auto"}}, QJsonObject {{"value", "2x2"}}}}};
		QString out;
		EXPECT(AdapterSettings::OptionValue(resolution, "R1920X1080", &out) && out == "1920x1080");
		EXPECT(AdapterSettings::OptionValue(resolution, "1280x720", &out) && out == "1280x720");
		EXPECT(AdapterSettings::OptionValue(mode, "R2X2", &out) && out == "2x2");
		EXPECT(AdapterSettings::OptionValue(mode, "auto", &out) && out == "auto");
		EXPECT(!AdapterSettings::OptionValue(resolution, "R640X480", &out));
		EXPECT(AdapterSettings::Typed(resolution, "R1920X1080") == "1920x1080");
		EXPECT(AdapterSettings::Typed(mode, "R2X2") == "2x2");
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
		TestManifest();
		Session s(f.adapter, f.data, f.home);
		TestHello(s);
		const auto id = TestLibrary(s, f);
		TestExtensionPage(s);
		TestGameSettingsSections();
		TestValues(s, f, id);
		TestColourAndKeyboard(s, f, id);
		TestLaunch(s, f, id);
		TestLaunchWithPageDefaults(s, id);
		TestGamePage(s, f, id);
		TestCheats(s, f, id);
		TestInputMapping(s);
		TestStaleCache(s, f, id);
		TestSettingsAtomic(s, f);
		TestSessionEvents(s, id);
		TestChangedAndEof(s, f);
	}
	TestOversized(f);
	TestMalformedIni(f);
	TestExtensionPageWithoutRuntime(f);
	TestFraming(f);
	TestUnits(f);
	// With KYTY_TEST_INSTALL_ROOT (a `cmake --install` tree) every icon published above must be in it.
	if (const auto root = qEnvironmentVariable("KYTY_TEST_INSTALL_ROOT"); !root.isEmpty()) {
		EXPECT(g_icons.size() >= 10);
		for (const auto& icon: g_icons) {
			QFileInfo file(QDir(root).filePath(icon));
			if (!file.isFile() || file.isSymLink() || file.size() == 0) {
				printf("missing from the installed tree: %s\n", qPrintable(icon));
				EXPECT(false);
			}
		}
	}
	printf("kyty_ags_adapter: %d checks passed\n", g_checks);
	return 0;
}
