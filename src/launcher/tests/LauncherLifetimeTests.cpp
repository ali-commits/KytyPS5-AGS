#include "applicationLifetime.h"
#include "hostControlClient.h"
#include "hostSocket.h"
#include "pairingAvailability.h"

#include <QApplication>
#include <QDialog>
#include <QEvent>
#include <QFile>
#include <QLineEdit>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWidget>
class LifetimeTests: public QObject {
	Q_OBJECT
private slots:
    void cleanupPermissionAllowsVisibleWindowToClose() {
        QDialog window;
        ApplicationLifetime lifetime(&window, false, [] {});
        window.show();
        lifetime.Exit();
        QSignalSpy ready(&lifetime, &ApplicationLifetime::ReadyToQuit);
        lifetime.Stopped({});
        QCOMPARE(ready.size(), 1);
        QVERIFY(window.close());
        QVERIFY(!window.isVisible());
    }

	void pairingAvailabilityRequiresPrivateCurrentTicket() {
		QTemporaryDir dir;
		QFile         file(dir.filePath("pairing-ticket.json"));
		QVERIFY(file.open(QIODevice::WriteOnly));
		file.write("{\"expires_at_unix_ms\":100001}");
		file.close();
		QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner));
		QVERIFY(PairingEnrollmentOpen(dir.path(), 100000));
		QVERIFY(!PairingEnrollmentOpen(dir.path(), 100002));
		QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadOther));
		QVERIFY(!PairingEnrollmentOpen(dir.path(), 100000));
		QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner));
		QFile::rename(file.fileName(), dir.filePath("actual"));
		QVERIFY(QFile::link(dir.filePath("actual"), file.fileName()));
		QVERIFY(!PairingEnrollmentOpen(dir.path(), 100000));
	}
	void ownedHostStopsAndRestartsIdempotently() {
		QTemporaryDir dir;
		QFile         script(dir.filePath("fake-host"));
		QVERIFY(script.open(QIODevice::WriteOnly));
		script.write("#!/bin/sh\ntrap 'exit 0' TERM\nwhile :; do sleep 0.05; done\n");
		script.close();
		QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
		HostControlClient owner(dir.filePath("missing.sock"));
		owner.SetLifetimeOwner(script.fileName());
		QSignalSpy stopped(&owner, &HostControlClient::OwnedHostStopped);
		QSignalSpy errors(&owner, &HostControlClient::CommandFailed);
		owner.StartDaemon();
		owner.StartDaemon();
		QTest::qWait(150);
		owner.StopDaemon();
		owner.StopDaemon();
		QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 3000);
		QCOMPARE(stopped.at(0).at(0).toString(), QString());
		QCOMPARE(errors.count(), 0);
		owner.StartDaemon();
		QTest::qWait(150);
		owner.ExitOwnedHost();
		QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 2, 3000);
	}
	void exitCannotBeReversedByRestartOrStart() {
		QTemporaryDir dir;
		QFile         script(dir.filePath("fake-host"));
		QVERIFY(script.open(QIODevice::WriteOnly));
		script.write("#!/bin/sh\necho started >> \"$0.started\"\ntrap 'sleep 0.15; exit 0' "
		             "TERM\nwhile :; do sleep 0.05; done\n");
		script.close();
		QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
		HostControlClient owner(dir.filePath("missing.sock"));
		owner.SetLifetimeOwner(script.fileName());
		QSignalSpy stopped(&owner, &HostControlClient::OwnedHostStopped);
		owner.StartDaemon();
		QTRY_VERIFY(QFile::exists(script.fileName() + ".started"));
		owner.ExitOwnedHost();
		owner.RestartDaemon();
		owner.StartDaemon();
		QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 1500);
		QCOMPARE(stopped.at(0).at(0).toString(), QString());
		owner.RestartDaemon();
		owner.StartDaemon();
		QTest::qWait(200);
		QFile starts(script.fileName() + ".started");
		QVERIFY(starts.open(QIODevice::ReadOnly));
		QCOMPARE(starts.readAll(), QByteArray("started\n"));
	}
	void exitCancelsLateReadinessProbe() {
		QTemporaryDir dir;
		QFile         script(dir.filePath("fake-host"));
		QVERIFY(script.open(QIODevice::WriteOnly));
		script.write(
		    "#!/bin/sh\ntouch \"$0.started\"\ntrap 'exit 0' TERM\nwhile :; do sleep 0.05; done\n");
		script.close();
		QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
		HostSocketServer server([](const QJsonObject&, std::function<void(HostReply)> done) {
			QTimer::singleShot(
			    50, [done] { done({503, "application/json", "{\"error\":\"not ready\"}", {}}); });
		});
		QString          error;
		QVERIFY(server.Listen(dir.filePath("control.sock"), &error));
		HostControlClient owner(dir.filePath("control.sock"));
		owner.SetLifetimeOwner(script.fileName());
		QSignalSpy stopped(&owner, &HostControlClient::OwnedHostStopped);
		owner.StartDaemon();
		owner.ExitOwnedHost();
		owner.RestartDaemon();
		owner.StartDaemon();
		QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 1500);
		QTest::qWait(150);
		QVERIFY(!QFile::exists(script.fileName() + ".started"));
	}
	void failedOwnedShutdownIsReported() {
		QTemporaryDir dir;
		QFile         script(dir.filePath("fake-host"));
		QVERIFY(script.open(QIODevice::WriteOnly));
		script.write("#!/bin/sh\ntrap 'echo cleanup-failed >&2; exit 7' TERM\nwhile :; do sleep "
		             "0.05; done\n");
		script.close();
		QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
		HostControlClient owner(dir.filePath("missing.sock"));
		owner.SetLifetimeOwner(script.fileName());
		QSignalSpy stopped(&owner, &HostControlClient::OwnedHostStopped);
		owner.StartDaemon();
		QTest::qWait(150);
		owner.ExitOwnedHost();
		QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 3000);
		QVERIFY(stopped.at(0).at(0).toString().contains("cleanup-failed"));
	}
	void independentHostIsNeverStopped() {
		QTemporaryDir    dir;
		HostSocketServer server([](const QJsonObject&, std::function<void(HostReply)> done) {
			done({200, "application/json", "{\"state\":\"Ready\"}", {}});
		});
		QString          error;
		QVERIFY(server.Listen(dir.filePath("control.sock"), &error));
		HostControlClient owner(dir.filePath("control.sock"));
		owner.SetLifetimeOwner("/not/executed");
		QSignalSpy errors(&owner, &HostControlClient::CommandFailed);
		QSignalSpy stopped(&owner, &HostControlClient::OwnedHostStopped);
		owner.StartDaemon();
		QTRY_COMPARE(errors.count(), 1);
		QVERIFY(errors.at(0).at(0).toString().contains("independent"));
		owner.ExitOwnedHost();
		QCOMPARE(stopped.count(), 1);
	}
	void failedOwnerStartIsVisible() {
		QTemporaryDir     dir;
		HostControlClient owner(dir.filePath("missing.sock"));
		owner.SetLifetimeOwner("/does/not/exist");
		QSignalSpy errors(&owner, &HostControlClient::CommandFailed);
		owner.StartDaemon();
		QTRY_COMPARE(errors.count(), 1);
		QVERIFY(!errors.at(0).at(0).toString().isEmpty());
	}
	void receiverMergeRetainsLocalPriority() {
        using State = ApplicationLifetime::TrayState;
        auto merge = ApplicationLifetime::MergeReceiverStatus;
        QCOMPARE(ApplicationLifetime::StateForStatus(merge({}, "idle", true)), State::Idle);
        QCOMPARE(ApplicationLifetime::StateForStatus(merge({}, "connecting", true)), State::Idle);
        QCOMPARE(ApplicationLifetime::StateForStatus(merge({}, "stopping", true)), State::Idle);
        QCOMPARE(ApplicationLifetime::StateForStatus(merge({{"state", "Ready"}}, "playing", false)), State::GameRunning);
        QCOMPARE(ApplicationLifetime::StateForStatus(merge({{"local_game_running", true}}, "unconfirmed", false)), State::GameRunning);
        QCOMPARE(ApplicationLifetime::StateForStatus(merge({{"pairing_open", true}}, "playing", false)), State::PairingOpen);
        QCOMPARE(ApplicationLifetime::StateForStatus(merge({{"state", "Error"}}, "idle", false)), State::Unavailable);
    }
    void trayReflectsActualStatus() {
		using State = ApplicationLifetime::TrayState;
		QCOMPARE(ApplicationLifetime::StateForStatus({}), State::Unavailable);
		QCOMPARE(ApplicationLifetime::StateForStatus({{"state", ""}}), State::Unavailable);
		QCOMPARE(ApplicationLifetime::StateForStatus({{"unrelated", true}}), State::Unavailable);
		QCOMPARE(ApplicationLifetime::StateForStatus({{"state", "Ready"}}), State::Idle);
		QCOMPARE(ApplicationLifetime::StateForStatus({{"stream_game_running", true}}),
		         State::GameRunning);
		QCOMPARE(ApplicationLifetime::StateForStatus(
		             {{"pairing_open", true}, {"stream_game_running", true}}),
		         State::PairingOpen);
		for (const auto& state: {"Stopped", "Starting", "Stopping", "Error", "Unknown"}) {
			QCOMPARE(ApplicationLifetime::StateForStatus({{"state", state}}), State::Unavailable);
			QCOMPARE(ApplicationLifetime::StateForStatus(
			             {{"state", state}, {"local_game_running", true}}),
			         State::GameRunning);
			QCOMPARE(ApplicationLifetime::StateForStatus(
			             {{"state", state}, {"stream_game_running", true}}),
			         State::GameRunning);
		}
		QCOMPARE(
		    ApplicationLifetime::StateForStatus({{"state", "Ready"}, {"local_game_running", true}}),
		    State::GameRunning);
		QCOMPARE(ApplicationLifetime::StateForStatus({{"state", "Error"},
		                                              {"local_game_running", true},
		                                              {"stream_game_running", true},
		                                              {"pairing_open", true}}),
		         State::PairingOpen);
		QVERIFY(!QIcon(":/brand/ags-app-icon.png").isNull());
		QList<QImage> rendered;
		for (auto state:
		     {State::Unavailable, State::Idle, State::GameRunning, State::PairingOpen}) {
			const auto icon = ApplicationLifetime::TrayIcon(state);
			QVERIFY(!icon.isNull());
			const auto image = icon.pixmap(22, 22).toImage();
			QVERIFY(!image.isNull());
			for (const auto& previous: rendered)
				QVERIFY(image != previous);
			rendered << image;
		}
		QWidget             window;
		ApplicationLifetime life(&window, true, [] {});
		auto*               tray = life.findChild<QSystemTrayIcon*>();
		QVERIFY(tray);
		QCOMPARE(tray->toolTip(), QString("AGS — Unavailable"));
		life.SetStatus({{"state", "Ready"}, {"pairing_open", true}, {"stream_game_running", true}});
		QCOMPARE(tray->toolTip(), QString("AGS — Remote game running · Pairing open"));
		QCOMPARE(tray->icon().pixmap(22, 22).toImage(), rendered.last());
		life.SetStatus({{"state", "Ready"}});
		QCOMPARE(tray->toolTip(), QString("AGS — Ready"));
		life.SetStatus({{"state", "Ready"}, {"local_game_running", true}});
		QCOMPARE(tray->toolTip(), QString("AGS — Local game running"));
		life.SetStatus({{"state", "Error"},
		                {"local_game_running", true},
		                {"stream_game_running", true},
		                {"pairing_open", true}});
		QCOMPARE(tray->toolTip(),
		         QString("AGS — Local game running · Remote game running · Pairing open · Error"));
		for (const auto& state: {"Stopped", "Starting", "Stopping", "Error"}) {
			life.SetStatus({{"state", state}});
			QCOMPARE(tray->toolTip(), QString("AGS — ") + state);
			QCOMPARE(tray->icon().pixmap(22, 22).toImage(), rendered.first());
		}
		life.SetStatus({{"state", "Error"},
		                {"error", "Steam ownership recovery required"},
		                {"ownership_blocked", true}});
		QCOMPARE(tray->toolTip(), QString("AGS — Error"));
		QCOMPARE(tray->icon().pixmap(22, 22).toImage(), rendered.first());
		life.SetStatus({});
		QCOMPARE(tray->toolTip(), QString("AGS — Unavailable"));
	}
	void closeAndExit() {
		QWidget             window;
		int                 stops = 0;
		ApplicationLifetime life(&window, true, [&] { ++stops; });
		window.show();
		window.close();
		QVERIFY(!window.isVisible());
		QCOMPARE(stops, 0);
		life.Exit();
		life.Exit();
		QCOMPARE(stops, 1);
		life.Stopped("failure");
		QVERIFY(window.isVisible());
		life.Exit();
		QCOMPARE(stops, 2);
	}
	void escapeFromFocusedChildUsesClosePolicy() {
		QDialog             window;
		QLineEdit           child(&window);
		ApplicationLifetime life(&window, true, [] {});
		QSignalSpy          rejected(&window, &QDialog::rejected);
		window.show();
		child.setFocus();
		QTest::keyClick(&child, Qt::Key_Escape);
		QCOMPARE(rejected.count(), 0);
		QVERIFY(!window.isVisible());
	}
	void stoppingHostingDoesNotExitApplication() {
		QWidget             window;
		ApplicationLifetime life(&window, false, [] {});
		QSignalSpy          quit(&life, &ApplicationLifetime::ReadyToQuit);
		life.Stopped({});
		QCOMPARE(quit.count(), 0);
		life.Exit();
		life.Stopped({});
		QCOMPARE(quit.count(), 1);
	}
	void applicationQuitWaitsForOwner() {
		QWidget             window;
		int                 stops = 0;
		ApplicationLifetime life(&window, false, [&] { ++stops; });
		QEvent              event(QEvent::Quit);
		QCoreApplication::sendEvent(qApp, &event);
		QCOMPARE(stops, 1);
	}
	void noTray() {
		QWidget             window;
		ApplicationLifetime life(&window, false, [] {});
		life.ShowInitial(true);
		QVERIFY(window.isVisible());
		window.close();
		QVERIFY(window.isVisible());
	}
	void autostart() {
		QTemporaryDir dir;
		QString       error;
		QVERIFY(ApplicationLifetime::WriteAutostart(dir.path(), "/tmp/a b/quote\"$`%\\ags", true,
		                                            &error));
		QFile file(dir.filePath("autostart/ags.desktop"));
		QVERIFY(file.open(QIODevice::ReadOnly));
		auto       bytes     = file.readAll();
		const auto validator = QStandardPaths::findExecutable("desktop-file-validate");
		if (!validator.isEmpty()) QCOMPARE(QProcess::execute(validator, {file.fileName()}), 0);
		QVERIFY(bytes.contains("--start-minimized"));
		QVERIFY(bytes.contains("%%"));
		QVERIFY(bytes.contains("\\\\$"));
		QVERIFY(ApplicationLifetime::WriteAutostart(dir.path(), "/tmp/ags", false, &error));
		QVERIFY(!file.exists());
		QVERIFY(!ApplicationLifetime::WriteAutostart(dir.path(), "/tmp/bad\nname", true, &error));
	}
};
QTEST_MAIN(LifetimeTests)
#include "LauncherLifetimeTests.moc"
