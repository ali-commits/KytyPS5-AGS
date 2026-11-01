#include "agsRustStreamingDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
class ControlsTest: public QObject {
	Q_OBJECT
private slots:

	void unpairUsesCurrentRegistryWithoutExposingOtherDeviceSecrets() {
		QTemporaryDir profile;
		QTemporaryDir prefsDir;
		QSettings::setDefaultFormat(QSettings::IniFormat);
		QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, prefsDir.path());
		QSettings prefs;
		prefs.setValue("rustStreaming/directory", profile.path());
		prefs.sync();
		QString     error;
		QJsonObject config {
		    {"name", "Fixture"},
		    {"bind", "127.0.0.1:48200"},
		    {"games", QJsonArray {}},
		    {"devices", QJsonObject {{"first", "secret-one"}, {"second", "secret-two"}}}};
		QVERIFY(AgsRustStreamingDialog::WriteConfiguration(profile.path(), config, &error));
		AgsRustStreamingDialog dialog;
		auto*                  devices = dialog.findChild<QListWidget*>("rustPairedDevices");
		QVERIFY(devices);
		QCOMPARE(devices->count(), 2);
		devices->setCurrentItem(devices->findItems("first", Qt::MatchExactly).first());
		config["devices"] =
		    QJsonObject {{"first", "secret-one"}, {"second", "secret-two"}, {"new", "new-secret"}};
		config["new_field"] = 42;
		QVERIFY(AgsRustStreamingDialog::WriteConfiguration(profile.path(), config, &error));
		dialog.findChild<QPushButton*>("rustUnpairDevice")->click();
		QFile saved(profile.filePath("host.json"));
		QVERIFY(saved.open(QIODevice::ReadOnly));
		const auto result = QJsonDocument::fromJson(saved.readAll()).object();
		QVERIFY(!result["devices"].toObject().contains("first"));
		QCOMPARE(result["devices"].toObject()["second"].toString(), QString("secret-two"));
		QCOMPARE(result["devices"].toObject()["new"].toString(), QString("new-secret"));
		QCOMPARE(result["new_field"].toInt(), 42);
		for (auto* label: dialog.findChildren<QLabel*>())
			QVERIFY(!label->text().contains("secret"));
	}

	void sharedClockBackendSettingsRoundTrip() {
		QTemporaryDir profile;
		QTemporaryDir prefsDir;
		QSettings::setDefaultFormat(QSettings::IniFormat);
		QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, prefsDir.path());
		QSettings prefs;
		prefs.setValue("rustStreaming/directory", profile.path());
		prefs.sync();
		QString     error;
		QJsonObject config {{"name", "Fixture"},
		                    {"bind", "127.0.0.1:48200"},
		                    {"devices", QJsonObject {}},
		                    {"games", QJsonArray {}}};
		QVERIFY(AgsRustStreamingDialog::WriteConfiguration(profile.path(), config, &error));
		AgsRustStreamingDialog dialog;
		auto*                  clock  = dialog.findChild<QComboBox*>("rustCaptureClock");
		auto*                  binary = dialog.findChild<QLineEdit*>("rustCompositorBinary");
		QVERIFY(clock && binary);
		QCOMPARE(clock->currentData().toString(), QString("legacy"));
		dialog.resize(760, 650);
		dialog.show();
		QApplication::processEvents();
		auto* scroll = dialog.findChild<QScrollArea*>("rustSettingsScroll");
		QVERIFY(scroll);
		QVERIFY(scroll->verticalScrollBar()->maximum() > 0);

		clock->setCurrentIndex(clock->findData("common"));
		binary->setText("/tmp/ags-owned-gamescope");
		dialog.findChild<QPushButton*>("rustSaveSettings")->click();
		QFile saved(profile.filePath("host.json"));
		QVERIFY(saved.open(QIODevice::ReadOnly));
		const auto value = QJsonDocument::fromJson(saved.readAll()).object();
		QCOMPARE(value["capture_clock"].toString(), QString("common"));
		QCOMPARE(value["compositor_binary"].toString(), QString("/tmp/ags-owned-gamescope"));
	}
	void settingsSaveSharesPairingKernelLock() {
#ifdef Q_OS_UNIX
		QTemporaryDir profile;
		QTemporaryDir prefsDir;
		QString       error;
		QSettings::setDefaultFormat(QSettings::IniFormat);
		QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, prefsDir.path());
		QSettings prefs;
		prefs.setValue("rustStreaming/directory", profile.path());
		prefs.sync();
		QJsonObject config {{"name", "Fixture"},
		                    {"bind", "127.0.0.1:48200"},
		                    {"devices", QJsonObject {{"old", "preserve-token"}}},
		                    {"games", QJsonArray {}}};
		QVERIFY(AgsRustStreamingDialog::WriteConfiguration(profile.path(), config, &error));
		AgsRustStreamingDialog dialog;
		auto*                  save = dialog.findChild<QPushButton*>("rustSaveSettings");
		QVERIFY(save);
		const auto lockPath = QFile::encodeName(profile.filePath(".pairing.lockfile"));
		const int  fd =
		    ::open(lockPath.constData(), O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
		QVERIFY(fd >= 0);
		QVERIFY(::flock(fd, LOCK_EX | LOCK_NB) == 0);
		save->click();
		QFile current(profile.filePath("host.json"));
		QVERIFY(current.open(QIODevice::ReadOnly));
		QVERIFY(!QJsonDocument::fromJson(current.readAll()).object().contains("audio_codec"));
		current.close();
		bool busy = false;
		for (auto* label: dialog.findChildren<QLabel*>())
			if (label->text().contains("Pairing") && label->text().contains("busy")) busy = true;
		QVERIFY(busy);
		config["devices"]          = QJsonObject {{"new", "fresh-token"}};
		config["added_while_open"] = 99;
		QVERIFY(AgsRustStreamingDialog::WriteConfiguration(profile.path(), config, &error));
		QVERIFY(::flock(fd, LOCK_UN) == 0);
		::close(fd);
		save->click();
		QVERIFY(current.open(QIODevice::ReadOnly));
		const auto saved = QJsonDocument::fromJson(current.readAll()).object();
		QVERIFY(saved.contains("audio_codec"));
		QCOMPARE(saved["devices"].toObject()["new"].toString(), QString("fresh-token"));
		QCOMPARE(saved["added_while_open"].toInt(), 99);
		QVERIFY(QFileInfo::exists(profile.filePath(".pairing.lockfile")));
#endif
	}

	void audioCodecOldProfileDefaultsPcmAndRoundtrips() {
		QTemporaryDir profile;
		QTemporaryDir prefsDir;
		QString       error;
		QSettings::setDefaultFormat(QSettings::IniFormat);
		QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, prefsDir.path());
		QSettings prefs;
		prefs.setValue("rustStreaming/directory", profile.path());
		prefs.sync();
		const QJsonObject config {{"name", "Fixture"},
		                          {"bind", "127.0.0.1:48200"},
		                          {"devices", QJsonObject {{"phone", "preserve-token"}}},
		                          {"games", QJsonArray {}},
		                          {"future_setting", 42}};
		QVERIFY(AgsRustStreamingDialog::WriteConfiguration(profile.path(), config, &error));
		AgsRustStreamingDialog dialog;
		auto*                  codec = dialog.findChild<QComboBox*>("rustAudioCodec");
		auto*                  save  = dialog.findChild<QPushButton*>("rustSaveSettings");
		QVERIFY(codec);
		QVERIFY(save);
		QCOMPARE(codec->currentData().toString(), QString("raw_pcm"));
		for (const auto& value: {QString("opus"), QString("raw_pcm")}) {
			codec->setCurrentIndex(codec->findData(value));
			save->click();
			QFile file(profile.filePath("host.json"));
			QVERIFY(file.open(QIODevice::ReadOnly));
			const auto saved = QJsonDocument::fromJson(file.readAll()).object();
			QCOMPARE(saved["audio_codec"].toString(), value);
			QCOMPARE(saved["future_setting"].toInt(), 42);
			QCOMPARE(saved["devices"].toObject()["phone"].toString(), QString("preserve-token"));
		}
	}

	void packetLossSettingDefaultsOffAndRoundtrips() {
		QTemporaryDir profile;
		QTemporaryDir prefsDir;
		QString       error;
		QSettings::setDefaultFormat(QSettings::IniFormat);
		QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, prefsDir.path());
		QSettings prefs;
		prefs.setValue("rustStreaming/directory", profile.path());
		prefs.sync();
		const QJsonObject config {{"name", "Fixture"},
		                          {"bind", "127.0.0.1:48200"},
		                          {"devices", QJsonObject {{"phone", "preserve-token"}}},
		                          {"games", QJsonArray {}},
		                          {"disconnect_grace_secs", 20},
		                          {"future_setting", 42}};
		QVERIFY(AgsRustStreamingDialog::WriteConfiguration(profile.path(), config, &error));
		AgsRustStreamingDialog dialog;
		auto*                  fec  = dialog.findChild<QCheckBox*>("rustFecEnabled");
		auto*                  save = dialog.findChild<QPushButton*>("rustSaveSettings");
		QVERIFY(fec);
		QVERIFY(save);
		QVERIFY(!fec->isChecked());
		for (bool enabled: {false, true}) {
			fec->setChecked(enabled);
			save->click();
			QFile file(profile.filePath("host.json"));
			QVERIFY(file.open(QIODevice::ReadOnly));
			const auto saved = QJsonDocument::fromJson(file.readAll()).object();
			QCOMPARE(saved["fec_enabled"].toBool(), enabled);
			QCOMPARE(saved["future_setting"].toInt(), 42);
			QCOMPARE(saved["devices"].toObject()["phone"].toString(), QString("preserve-token"));
		}
	}

	void pairingExportKeepsNewDeviceRegistryAndDoesNotLogToken() {
		QTemporaryDir profile;
		QTemporaryDir prefsDir;
		QString       error;
		QSettings::setDefaultFormat(QSettings::IniFormat);
		QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, prefsDir.path());
		QFile helper(profile.filePath("ags-host-fixture"));
		QVERIFY(helper.open(QIODevice::WriteOnly));
		helper.write(
		    "#!/bin/sh\nif [ \"$1\" = pair-ticket ]; then\n umask 077\n printf '%s' "
		    "'{\"token\":\"never-log-this-secret\"}' > \"$2/pairing-bundle-fixture.json\"\n printf "
		    "'Pairing bundle: %s/pairing-bundle-fixture.json\\n' \"$2\"\nfi\n");
		helper.close();
		QVERIFY(helper.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
		QSettings prefs;
		prefs.setValue("rustStreaming/binary", helper.fileName());
		prefs.setValue("rustStreaming/directory", profile.path());
		prefs.sync();
		QJsonObject config {{"name", "Fixture"},
		                    {"bind", "127.0.0.1:48200"},
		                    {"devices", QJsonObject {{"old", "old-secret"}}},
		                    {"games", QJsonArray {}},
		                    {"disconnect_grace_secs", 20}};
		QVERIFY(AgsRustStreamingDialog::WriteConfiguration(profile.path(), config, &error));
		AgsRustStreamingDialog dialog;
		config["devices"] = QJsonObject {{"old", "old-secret"}, {"new", "fresh-secret"}};
		QVERIFY(AgsRustStreamingDialog::WriteConfiguration(profile.path(), config, &error));
		auto* button = dialog.findChild<QPushButton*>("rustExportPairing");
		QVERIFY(button);
		button->click();
		QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(profile.filePath("pairing-bundle-fixture.json")),
		                         3000);
		QFile current(profile.filePath("host.json"));
		QVERIFY(current.open(QIODevice::ReadOnly));
		QVERIFY(QJsonDocument::fromJson(current.readAll())
		            .object()["devices"]
		            .toObject()
		            .contains("new"));
		for (auto* log: dialog.findChildren<QPlainTextEdit*>())
			QVERIFY(!log->toPlainText().contains("never-log-this-secret"));
		for (auto* label: dialog.findChildren<QLabel*>())
			QVERIFY(!label->text().contains("never-log-this-secret"));
	}

	void importsOrdinaryArgumentsAndIsolatesSaves() {
		QTemporaryDir                     dir;
		QString                           error;
		const QStringList                 args    = {"--screen-width",     "1280",      "--game",
		                                             "games/Example Game", "--user-id", "1"};
		const QList<StreamingLaunchEntry> entries = {{"abc123",
		                                              "Example Game",
		                                              "/tmp/original-runtime/kyty_emulator",
		                                              args,
		                                              "/tmp/original-runtime",
		                                              {}}};
		const auto games = AgsRustStreamingDialog::ImportCatalog(entries, dir.path(), &error);
		QVERIFY2(error.isEmpty(), qPrintable(error));
		QCOMPARE(games.size(), 1);
		const auto game = games[0].toObject();
		QCOMPARE(game["executable"].toString(), QString("/tmp/original-runtime/kyty_emulator"));
		const auto cwd = game["cwd"].toString();
		QVERIFY(cwd.startsWith(dir.path() + "/"));
		QVERIFY(QDir(cwd + "/_SaveData").exists());
		const auto imported = game["args"].toArray();
		QCOMPARE(imported[3].toString(), QString("/tmp/original-runtime/games/Example Game"));
		QVERIFY(!imported.contains("--streaming-only"));
	}
	void refusesSaveDirectorySymlink() {
		QTemporaryDir dir;
		QTemporaryDir normal;
		QVERIFY(QDir().mkpath(dir.filePath("kyty-test-runtime/game")));
		QVERIFY(QFile::link(normal.path(), dir.filePath("kyty-test-runtime/game/_SaveData")));
		QString                           error;
		const QList<StreamingLaunchEntry> entries = {
		    {"game", "Game", "/tmp/kyty", {"--game", "/tmp/game"}, "/tmp", {}}};
		QVERIFY(AgsRustStreamingDialog::ImportCatalog(entries, dir.path(), &error).isEmpty());
		QVERIFY(!error.isEmpty());
		QVERIFY(QDir(normal.path()).isEmpty());
	}

	void writePreservesPrivateFields() {
		QTemporaryDir dir;
		QString       error;
		QJsonObject   config {{"devices", QJsonObject {{"phone", "private-token"}}},
		                      {"disconnect_grace_secs", 20}};
		QVERIFY(AgsRustStreamingDialog::WriteConfiguration(dir.path(), config, &error));
		QFile file(dir.filePath("host.json"));
		QVERIFY(file.open(QIODevice::ReadOnly));
		QCOMPARE(QJsonDocument::fromJson(file.readAll()).object(), config);
		QVERIFY(!(file.permissions() &
		          (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther)));
		config["disconnect_grace_secs"] = 45;
		QVERIFY(AgsRustStreamingDialog::WriteConfiguration(dir.path(), config, &error));
		file.close();
		QVERIFY(file.open(QIODevice::ReadOnly));
		QCOMPARE(QJsonDocument::fromJson(file.readAll())
		             .object()["devices"]
		             .toObject()["phone"]
		             .toString(),
		         QString("private-token"));
	}
	void rejectsSymlink() {
		QTemporaryDir dir;
		QFile         target(dir.filePath("target"));
		QVERIFY(target.open(QIODevice::WriteOnly));
		target.write("original");
		target.close();
		QVERIFY(QFile::link(target.fileName(), dir.filePath("host.json")));
		QString error;
		QVERIFY(!AgsRustStreamingDialog::WriteConfiguration(dir.path(), QJsonObject {}, &error));
		QVERIFY(!error.isEmpty());
		QVERIFY(target.open(QIODevice::ReadOnly));
		QCOMPARE(target.readAll(), QByteArray("original"));
	}
	void rendersWithoutLaunchingHost() {
		AgsRustStreamingDialog dialog;
		dialog.show();
		QTest::qWait(20);
		QVERIFY(dialog.isVisible());
		if (qEnvironmentVariableIsSet("AGS_CONTROLS_SCREENSHOT"))
			dialog.grab().save("/tmp/ags-rust-controls.png");
	}
};
QTEST_MAIN(ControlsTest)
#include "test.moc"
