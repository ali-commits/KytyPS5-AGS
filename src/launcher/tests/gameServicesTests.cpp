// Checks for the socket-free per-game helpers: safe runtime paths, cheat plan path and
// selection, trophies without packages, and art lookup. Plain asserts, no framework.
#include "cheatFile.h"
#include "cheatRepository.h"
#include "configuration.h"
#include "gameServices.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
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

namespace {

// Removes the cheat plan fixture on scope exit even if a CHECK() aborts first.
class ScopedCheatFile {
public:
	explicit ScopedCheatFile(QString path): m_path(std::move(path)) {}
	~ScopedCheatFile() { QFile::remove(m_path); }
	ScopedCheatFile(const ScopedCheatFile&)            = delete;
	ScopedCheatFile& operator=(const ScopedCheatFile&) = delete;

private:
	QString m_path;
};

} // namespace

static void SafeRuntimeChecks() {
	QTemporaryDir dir;
	CHECK(dir.isValid());
	const auto root = QFileInfo(dir.path()).canonicalFilePath();
	CHECK(GameServices::SafeRuntime(root));
	CHECK(!GameServices::SafeRuntime(QStringLiteral("relative/path")));
	CHECK(!GameServices::SafeRuntime(root + QStringLiteral("/../") + QFileInfo(root).fileName()));
	CHECK(!GameServices::SafeRuntime(root + QStringLiteral("/missing")));

	CHECK(QDir(root).mkpath(QStringLiteral("_SaveData/slot")));
	CHECK(GameServices::SafeRuntime(root));
	CHECK(QFile::link(root + QStringLiteral("/_SaveData"), root + QStringLiteral("/_Trophies")));
	CHECK(!GameServices::SafeRuntime(root)); // a symlinked state folder is refused
	CHECK(QFile::remove(root + QStringLiteral("/_Trophies")));
	CHECK(QFile::link(root + QStringLiteral("/_SaveData"),
	                  root + QStringLiteral("/_SaveData/slot/loop")));
	CHECK(!GameServices::SafeRuntime(root)); // a symlink nested inside a state folder too
}

static void CheatChecks() {
	CHECK(Cheats::PlanPath(QStringLiteral("not-a-title")).isEmpty());
	const auto path = Cheats::PlanPath(QStringLiteral("ppsa99997"));
	CHECK(path == QDir(QCoreApplication::applicationDirPath())
	                  .filePath(QStringLiteral("_Patches/PPSA99997.json")));

	Configuration unsupported;
	unsupported.title_id = QStringLiteral("CUSA00001");
	CHECK(!GameServices::GetCheats(unsupported).value("supported").toBool());

	Configuration info;
	info.title_id    = QStringLiteral("PPSA99997");
	info.gameVersion = QStringLiteral("01.000.000");
	CHECK(!QFile::exists(path)); // never replace an existing fixture
	const ScopedCheatFile cleanup(path);
	const auto            document = QJsonDocument::fromJson(
	    R"({"id":"PPSA99997","version":"01.000.000","process":"eboot.bin","mods":[
	        {"name":"A","enabled":false,"memory":[{"offset":"10","off":"00","on":"01"}]},
	        {"name":"B","enabled":true,"memory":[{"offset":"20","off":"FF","on":"00"}]}]})");
	CHECK(Cheats::SaveDocument(path, document, {}).isEmpty());

	auto reply = GameServices::GetCheats(info);
	CHECK(reply.value("supported").toBool());
	CHECK(reply.value("mods").toArray().size() == 2);
	const auto revision = reply.value("revision").toString();
	CHECK(!revision.isEmpty());

	QString updated;
	CHECK(GameServices::PutCheats(info, {{"revision", "stale"}, {"enabled", QJsonArray {true, true}}},
	                              &updated) == "stale");
	CHECK(GameServices::PutCheats(info, {{"revision", revision}, {"enabled", QJsonArray {true}}},
	                              &updated) == "selection-size-mismatch");
	CHECK(GameServices::PutCheats(info,
	                              {{"revision", revision}, {"enabled", QJsonArray {true, false}}},
	                              &updated)
	          .isEmpty());
	CHECK(!updated.isEmpty() && updated != revision);
	reply = GameServices::GetCheats(info);
	const auto mods = reply.value("mods").toArray();
	CHECK(mods[0].toObject().value("enabled").toBool());
	CHECK(!mods[1].toObject().value("enabled").toBool());
}

static void RemoteCheatListChecks() {
	Configuration unsupported;
	unsupported.title_id = QStringLiteral("CUSA00001");
	const auto reply =
	    GameServices::RemoteCheatList(unsupported, QList<Cheats::RemoteFile> {}, QString {});
	CHECK(reply.value("files").toArray().isEmpty());
	CHECK(reply.value("error").toString() == "unsupported-title-id");

	Configuration info;
	info.title_id    = QStringLiteral("PPSA99997");
	info.gameVersion = QStringLiteral("01.000.000");
	const auto path  = Cheats::PlanPath(QStringLiteral("PPSA99997"));
	CHECK(!QFile::exists(path));
	const ScopedCheatFile cleanup(path);

	QList<Cheats::RemoteFile> files {
	    {QStringLiteral("PPSA99997_01.000.000_mycheats.json"), QStringLiteral("My Cheats"),
	     QStringLiteral("01.000.000"), QStringLiteral("json")},
	    {QStringLiteral("PPSA99997_01.000.000_other.mc4"), QStringLiteral("Other"),
	     QStringLiteral("01.000.000"), QStringLiteral("mc4")},
	};
	const auto list_reply = GameServices::RemoteCheatList(info, files, QString {});
	const auto list       = list_reply.value("files").toArray();
	CHECK(list.size() == 1); // the mc4 entry is filtered out as unsupported
	CHECK(list[0].toObject().value("name").toString() == files[0].name);
	CHECK(!list[0].toObject().value("installed_version").toBool()); // no local plan yet
	CHECK(!list_reply.contains("error"));

	const auto error_reply = GameServices::RemoteCheatList(info, {}, QStringLiteral("boom"));
	CHECK(error_reply.value("error").toString() == "boom");
}

static void ImportRemoteCheatChecks() {
	Configuration info;
	info.title_id    = QStringLiteral("PPSA99997");
	info.gameVersion = QStringLiteral("01.000.000");
	const auto path  = Cheats::PlanPath(QStringLiteral("PPSA99997"));
	CHECK(!QFile::exists(path));
	const ScopedCheatFile cleanup(path);

	const auto valid = QJsonDocument::fromJson(
	    R"({"id":"PPSA99997","version":"01.000.000","process":"eboot.bin","mods":[
	        {"name":"A","enabled":true,"memory":[{"offset":"10","off":"00","on":"01"}]},
	        {"name":"B","enabled":true,"memory":[{"offset":"20","off":"FF","on":"00"}]}]})");

	QString revision;
	CHECK(GameServices::ImportRemoteCheat(info, valid.toJson(), &revision).isEmpty());
	CHECK(!revision.isEmpty());
	CHECK(QFile::exists(path));
	const auto saved = GameServices::GetCheats(info);
	const auto mods  = saved.value("mods").toArray();
	CHECK(mods.size() == 2);
	for (const auto& mod: mods) {
		CHECK(!mod.toObject().value("enabled").toBool()); // all mods disabled on import
	}

	const auto wrong_title = QJsonDocument::fromJson(
	    R"({"id":"CUSA00001","version":"01.000.000","process":"eboot.bin","mods":[
	        {"name":"A","enabled":true,"memory":[{"offset":"10","off":"00","on":"01"}]}]})");
	CHECK(!GameServices::ImportRemoteCheat(info, wrong_title.toJson(), nullptr).isEmpty());
}

static void TrophyAndArtChecks() {
	QTemporaryDir dir;
	CHECK(dir.isValid());
	Configuration info;
	info.basedir = dir.path();
	const auto trophies = GameServices::Trophies(info, dir.path());
	CHECK(trophies.value("total").toInt() == 0);
	CHECK(GameServices::TrophyIcon(info, dir.path(), 0, 1).error == "no-icon");

	CHECK(GameServices::GameArt(info, "icon").data.isEmpty());
	QImage icon(8, 8, QImage::Format_RGB32);
	icon.fill(Qt::red);
	CHECK(QDir(dir.path()).mkpath("sce_sys"));
	CHECK(icon.save(dir.filePath("sce_sys/icon0.png")));
	const auto art = GameServices::GameArt(info, "icon");
	CHECK(!art.data.isEmpty() && art.type == "image/png");
	CHECK(GameServices::GameArt(info, "background").data.isEmpty());
	CHECK(icon.save(dir.filePath("sce_sys/pic0.png")));
	CHECK(GameServices::GameArt(info, "background").type == "image/jpeg");
}

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	SafeRuntimeChecks();
	CheatChecks();
	RemoteCheatListChecks();
	ImportRemoteCheatChecks();
	TrophyAndArtChecks();
	puts("game services checks passed");
	return 0;
}
