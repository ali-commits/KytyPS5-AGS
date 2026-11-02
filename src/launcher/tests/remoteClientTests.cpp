#include "applicationLifetime.h"
#include "remoteClient.h"
#include "remoteHostsWidget.h"

#include <QApplication>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

class RemoteClientTests: public QObject {
	Q_OBJECT
	QTemporaryDir temp;
	QString       script, settings, profile;
	static void   write(const QString& path, const QByteArray& data, bool executable = false) {
		QFile file(path);
		QVERIFY(file.open(QIODevice::WriteOnly));
		QCOMPARE(file.write(data), data.size());
		file.close();
		QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner |
		                            (executable ? QFile::ExeOwner : QFile::Permissions {})));
	}
private slots:
	void init() {
		QVERIFY(temp.isValid());
		const auto root = temp.path() + "/" + QString::number(QDateTime::currentMSecsSinceEpoch());
		QVERIFY(QDir().mkpath(root));
		script   = root + "/fake client";
		settings = root + "/settings.json";
		profile  = root + "/profile space 日本";
		QVERIFY(QDir().mkpath(profile));
		QFile::setPermissions(profile, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
		write(profile + "/host.der", "fixture certificate");
		write(profile + "/client.json", "private fixture");
		write(script, R"PY(#!/usr/bin/python3
import sys,json,time,signal,os
args=sys.argv[1:]
with open(__file__+'.args','a') as f: f.write(json.dumps(args)+'\n')
if args[0]=='pair':
 os.mkdir(args[3],0o700)
 for name in ['client.json','host.der']:
  with open(args[3]+'/'+name,'w') as f:f.write('fixture')
  os.chmod(args[3]+'/'+name,0o600)
 sys.exit(0)
command=args[3]
mode=open(__file__+'.mode').read() if os.path.exists(__file__+'.mode') else 'ok'
if command=='list':
 if mode=='delay':time.sleep(1)
 if mode=='fail':sys.exit(1)
 if mode=='oversize':print('x'*1100000);sys.exit(0)
 if mode=='malformed':print('{}');sys.exit(0)
 print(json.dumps({'ok':True,'games':[{'id':'same id','name':'Game 日本'}]}))
elif command=='launch':
 if mode=='fail':sys.exit(1)
 def interrupted(*a):
  with open(__file__+'.interrupted','w') as f:f.write('SIGINT')
  sys.exit(0)
 signal.signal(signal.SIGINT,interrupted)
 with open(__file__+'.receiver-pid','w') as f:f.write(str(os.getpid()))
 print('{"ok":true}',flush=True)
 while True:time.sleep(.02)
elif command=='close':
 if mode=='fail':sys.exit(1)
 print('{"ok":true}')
)PY",
		      true);
	}
	void offlineCacheAndCompositeIdentity() {
		RemoteClient client(RemoteClient::TestOptions {script, settings, 2500});
		QString      id = client.AddProfile("PC", "127.0.0.1:4444", profile);
		QVERIFY(!id.isEmpty());
		QTRY_COMPARE(client.Cards().size(), 1);
		QVERIFY(client.Cards()[0].ready);
		QVERIFY(client.Cards()[0].id.startsWith("remote:" + id + ":"));
		write(script + ".mode", "fail");
		client.Refresh(id);
		QTRY_VERIFY(!client.Cards()[0].ready);
		QCOMPARE(client.Cards()[0].reason,
		         QString("Host offline. Refresh the connection to try again."));
		RemoteClient restored(RemoteClient::TestOptions {script, settings, 2500});
		QCOMPARE(restored.Cards().size(), 1);
		QVERIFY(!restored.Cards()[0].ready);
		QCOMPARE(restored.Cards()[0].id, client.Cards()[0].id);
		QVERIFY(client.AddProfile("Duplicate", "127.0.0.2:4444", profile).isEmpty());
	}
	void malformedAndOversizedKeepCache() {
		RemoteClient client(RemoteClient::TestOptions {script, settings, 2500});
		auto         id = client.AddProfile("PC", "127.0.0.1:4444", profile);
		QTRY_COMPARE(client.Cards().size(), 1);
		QSignalSpy errors(&client, &RemoteClient::Error);
		for (const auto& mode: {"malformed", "oversize"}) {
			const auto before = errors.size();
			write(script + ".mode", mode);
			client.Refresh(id);
			QTRY_COMPARE(errors.size(), before + 1);
			QVERIFY(!client.Cards()[0].ready);
			QCOMPARE(client.Cards().size(), 1);
		}
	}
	void forgottenRequestCannotResurrect() {
		RemoteClient client(RemoteClient::TestOptions {script, settings, 2500});
		write(script + ".mode", "delay");
		auto id = client.AddProfile("PC", "127.0.0.1:4444", profile);
		QVERIFY(client.Forget(id));
		QTest::qWait(1200);
		QVERIFY(client.Profiles().isEmpty());
		QVERIFY(client.Cards().isEmpty());
	}
	void profilesCannotCollideAndRefreshRevisionWins() {
		RemoteClient client(RemoteClient::TestOptions {script, settings, 2500});
		auto         first      = client.AddProfile("Same name", "127.0.0.1:4444", profile);
		const auto   secondPath = profile + " second";
		QVERIFY(QDir().mkpath(secondPath));
		QFile::setPermissions(secondPath, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
		write(secondPath + "/host.der", "fixture");
		write(secondPath + "/client.json", "fixture");
		auto second = client.AddProfile("Same name", "127.0.0.2:4444", secondPath);
		QTRY_COMPARE(client.Cards().size(), 2);
		QVERIFY(first != second);
		QVERIFY(client.Cards()[0].id != client.Cards()[1].id);
		write(script + ".mode", "delay");
		client.Refresh(first);
		QTest::qWait(50);
		write(script + ".mode", "fail");
		client.Refresh(first);
		QTRY_VERIFY(!client.Cards().first().ready || !client.Cards().last().ready);
		QTest::qWait(1100);
		int online = 0;
		for (const auto& card: client.Cards())
			online += card.ready;
		QCOMPARE(online, 1);
		QVERIFY(!client.Forget("unknown profile"));
	}
	void requestTimeoutAndReceiverFailureAreVisible() {
		RemoteClient client(RemoteClient::TestOptions {script, settings, 100});
		write(script + ".mode", "delay");
		QSignalSpy errors(&client, &RemoteClient::Error);
		auto       id = client.AddProfile("PC", "127.0.0.1:4444", profile);
		QTRY_VERIFY(!errors.isEmpty());
		QVERIFY(client.Cards().isEmpty());
		write(script + ".mode", "ok");
		client.Refresh(id);
		QTRY_COMPARE(client.Cards().size(), 1);
		write(script + ".mode", "fail");
		QVERIFY(client.Play(client.Cards()[0].id));
		QTRY_VERIFY(errors.size() >= 2);
		QVERIFY(client.Busy()); // unconfirmed remote ownership stays visible
		write(script + ".mode", "ok");
		QSignalSpy stopped(&client, &RemoteClient::Stopped);
		client.StopForExit();
		QTRY_COMPARE(stopped.size(), 1);
		QVERIFY(!client.Busy());
	}
	void controllerInventoryAndExplicitArgvStayGamepadOnly() {
		const auto sysRoot = profile + "/sys";
		const auto devRoot = profile + "/dev";
		QVERIFY(QDir().mkpath(devRoot));
		auto device = [&](const QString& event, const QString& name, const QByteArray& keys,
		                  const QByteArray& axes) {
			const auto devicePath = sysRoot + "/" + event + "/device";
			QDir().mkpath(devicePath + "/capabilities");
			write(devicePath + "/name", name.toUtf8());
			write(devicePath + "/capabilities/key", keys);
			write(devicePath + "/capabilities/abs", axes);
			write(devRoot + "/" + event, "fixture node; never opened as input");
		};
		device("event5", "Gamepad 日本", "1000000000000 0 0 0 0", "3");
		device("event6", "Keyboard", "1", "0");
		// Linux bitmap words are printed high-to-low, without zero-padding. BTN_GAMEPAD 304 = word4
		// bit48.
		write(sysRoot + "/event5/device/capabilities/key", "1000000000000 0 0 0 0");
		RemoteClient client(RemoteClient::TestOptions {script, settings, 2500, sysRoot, devRoot});
		QCOMPARE(client.Controllers().size(), 1);
		QCOMPARE(client.Controllers()[0].name, QString("Gamepad 日本"));
		QVERIFY(!client.SetController(devRoot + "/event6"));
		QVERIFY(!client.SetController("/dev/input/event999999"));
		QVERIFY(client.SetController(devRoot + "/event5"));
		client.AddProfile("PC", "127.0.0.1:4444", profile);
		QTRY_COMPARE(client.Cards().size(), 1);
		QVERIFY(client.Play(client.Cards()[0].id));
		QTRY_VERIFY(QFile::exists(script + ".receiver-pid"));
		QFile log(script + ".args");
		QVERIFY(log.open(QIODevice::ReadOnly));
		const auto lines = log.readAll().trimmed().split('\n');
		const auto args  = QJsonDocument::fromJson(lines.last()).array();
		const auto input = args.toVariantList().indexOf("--input");
		QVERIFY(input >= 0);
		QCOMPARE(args[input + 1].toString(), devRoot + "/event5");
		QVERIFY(!args.contains("--raw-input-proxy"));
		QSignalSpy stopped(&client, &RemoteClient::Stopped);
		client.StopForExit();
		QTRY_COMPARE(stopped.size(), 1);
		QVERIFY(QFile::remove(devRoot + "/event5"));
		QVERIFY(!client.Play(
		    client.Cards()[0].id)); // explicit unplug cannot silently become keyboard-only
		QVERIFY(client.SetController("@auto"));
		QVERIFY(client.Play(client.Cards()[0].id));
		QTest::qWait(50);
		client.StopForExit();
		QTRY_COMPARE(stopped.size(), 2);
	}
	void receiverOwnedOnceAndClosedBeforeStop() {
		RemoteClient client(RemoteClient::TestOptions {script, settings, 2500});
		auto         id = client.AddProfile("PC", "127.0.0.1:4444", profile);
		QTRY_COMPARE(client.Cards().size(), 1);
		QVERIFY(!client.Play("unknown"));
		QVERIFY(client.Play(client.Cards()[0].id));
		QVERIFY(client.Busy());
		QVERIFY(!client.Play(client.Cards()[0].id));
		QVERIFY(!client.Forget(id));
		QTest::qWait(100); // allow the fixture to install SIGINT handler
		QSignalSpy stopped(&client, &RemoteClient::Stopped);
		client.StopForExit();
		QTRY_COMPARE_WITH_TIMEOUT(stopped.size(), 1, 5000);
		QCOMPARE(stopped[0][0].toString(), QString());
		QVERIFY(!client.Busy());
		QFile args(script + ".args");
		QVERIFY(args.open(QIODevice::ReadOnly));
		auto lines = args.readAll().trimmed().split('\n');
		QCOMPARE(lines.size(), 3);
		const auto launch = QJsonDocument::fromJson(lines[1]).array();
		QCOMPARE(launch[1].toString(), profile + "/host.der");
		QCOMPARE(launch[4].toString(), QString("same id"));
		QVERIFY(launch.contains("--close-on-exit"));
		QCOMPARE(QJsonDocument::fromJson(lines[2]).array()[3].toString(), QString("close"));
	}
	void lifetimeKeepsPlaybackWhenHiddenAndWaitsForClose() {
		QWidget      window;
		RemoteClient client(RemoteClient::TestOptions {script, settings, 2500});
		auto         id = client.AddProfile("PC", "127.0.0.1:4444", profile);
		QTRY_COMPARE(client.Cards().size(), 1);
		QVERIFY(client.Play(client.Cards()[0].id));
		QTRY_VERIFY(QFile::exists(script + ".receiver-pid"));
		ApplicationLifetime lifetime(&window, false, [&client] { client.StopForExit(); });
		connect(&client, &RemoteClient::Stopped, &lifetime, &ApplicationLifetime::Stopped);
		QSignalSpy ready(&lifetime, &ApplicationLifetime::ReadyToQuit);
		window.show();
		window.showMinimized();
		QTest::qWait(30);
		QVERIFY(client.Busy());
		window.hide();
		QTest::qWait(30);
		QVERIFY(client.Busy());
		lifetime.Exit();
		QCOMPARE(ready.size(), 0);
		QTRY_COMPARE(ready.size(), 1);
		QVERIFY(!client.Busy());
		QVERIFY(QFile::exists(script + ".interrupted"));
	}
	void parentDeathInterruptsOwnedReceiver() {
#ifdef Q_OS_LINUX
		QProcess parent;
		auto     env = QProcessEnvironment::systemEnvironment();
		env.insert("AGS_REMOTE_TEST_PARENT", "1");
		env.insert("AGS_REMOTE_TEST_SCRIPT", script);
		env.insert("AGS_REMOTE_TEST_SETTINGS", settings);
		env.insert("AGS_REMOTE_TEST_PROFILE", profile);
		parent.setProcessEnvironment(env);
		parent.start(QCoreApplication::applicationFilePath(), {});
		QVERIFY(parent.waitForStarted());
		QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(script + ".receiver-pid"), 5000);
		parent.kill();
		QVERIFY(parent.waitForFinished(3000));
		QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(script + ".interrupted"), 3000);
#else
		QSKIP("Linux parent death contract");
#endif
	}
	void pairPinsSnapshotAndImportsOnlyAfterSuccess() {
		RemoteClient client(RemoteClient::TestOptions {script, settings, 2500});
		const auto   bundle = temp.path() + "/pair bundle.json";
		write(
		    bundle,
		    R"({"version":1,"certificate_der":[1,2,3],"endpoint":"127.0.0.1:4444","token":"not interpreted by Qt"})");
		QString fingerprint, endpoint, error;
		QVERIFY(RemoteClient::PairingInfo(bundle, &fingerprint, &endpoint, &error));
		QCOMPARE(fingerprint.size(), 64);
		QSignalSpy errors(&client, &RemoteClient::Error);
		const auto output = profile + " paired";
		client.Pair(bundle, "wrong fingerprint", "My laptop", endpoint, output);
		QCOMPARE(errors.size(), 1);
		QVERIFY(!QFile::exists(output));
		client.Pair(bundle, fingerprint, "PC", endpoint, output, "My laptop 日本");
		QTRY_COMPARE(client.Profiles().size(), 1);
		QTRY_COMPARE(client.Cards().size(), 1);
		QCOMPARE(client.Profiles()[0].directory, output);
		QFile log(script + ".args");
		QVERIFY(log.open(QIODevice::ReadOnly));
		auto args = QJsonDocument::fromJson(log.readLine()).array();
		QCOMPARE(args[0].toString(), QString("pair"));
		QCOMPARE(args[2].toString(), QString("My laptop 日本"));
		QVERIFY(args[1].toString() != bundle); // private immutable snapshot binds fingerprint
		QFile saved(settings);
		QVERIFY(saved.open(QIODevice::ReadOnly));
		QVERIFY(!saved.readAll().contains("token"));
	}
	void failedPairDoesNotAddProfile() {
		write(script, "#!/bin/sh\nexit 1\n", true);
		const auto bundle = temp.path() + "/failed pair.json";
		write(bundle, R"({"version":1,"certificate_der":[1],"endpoint":"127.0.0.1:4444"})");
		QString fingerprint, endpoint, error;
		QVERIFY(RemoteClient::PairingInfo(bundle, &fingerprint, &endpoint, &error));
		RemoteClient client(RemoteClient::TestOptions {script, settings, 2500});
		QSignalSpy   errors(&client, &RemoteClient::Error);
		client.Pair(bundle, fingerprint, "Laptop", endpoint, profile + " failed");
		QTRY_COMPARE(errors.size(), 1);
		QVERIFY(client.Profiles().isEmpty());
	}
	void receiverSetupFitsSmallWindow() {
		RemoteClient      client(RemoteClient::TestOptions {script, settings, 2500});
		RemoteHostsWidget page(&client);
		for (const QSize size: {QSize(520, 480), QSize(1280, 720)}) {
			page.resize(size);
			page.show();
			QTest::qWait(20);
			QCOMPARE(page.size(), size);
			QVERIFY(page.findChild<QPushButton*>("remotePair")->isVisible());
			QVERIFY(page.findChild<QPushButton*>("remoteExistingProfile")->isVisible());
			QVERIFY(page.grab().save(QString("/tmp/ags-remote-setup-%1.png").arg(size.width())));
		}
	}
	void privateProfilesAndAddressValidation() {
		RemoteClient client(RemoteClient::TestOptions {script, settings, 2500});
		QVERIFY(client.AddProfile("PC", "0.0.0.0:4444", profile).isEmpty());
		QVERIFY(client.AddProfile("PC", "127.0.0.1:0", profile).isEmpty());
		QVERIFY(client.AddProfile("PC", "hostname:4444", profile).isEmpty());
		QFile::setPermissions(profile + "/client.json",
		                      QFile::ReadOwner | QFile::WriteOwner | QFile::ReadOther);
		QVERIFY(client.AddProfile("PC", "127.0.0.1:4444", profile).isEmpty());
		QFile::setPermissions(profile + "/client.json", QFile::ReadOwner | QFile::WriteOwner);
		const auto alias = profile + " alias";
		QVERIFY(QFile::link(profile, alias));
		QVERIFY(client.AddProfile("PC", "127.0.0.1:4444", alias).isEmpty());
		QVERIFY(!client.SetQuality(63, 720, 60));
		QVERIFY(!client.SetQuality(1280, 720, 241));
	}
	void failedCloseDoesNotClaimSuccess() {
		RemoteClient client(RemoteClient::TestOptions {script, settings, 2500});
		auto         id = client.AddProfile("PC", "127.0.0.1:4444", profile);
		QTRY_COMPARE(client.Cards().size(), 1);
		QVERIFY(client.Play(client.Cards()[0].id));
		QTest::qWait(100);
		write(script + ".mode", "fail");
		QSignalSpy stopped(&client, &RemoteClient::Stopped);
		client.StopForExit();
		QTRY_COMPARE_WITH_TIMEOUT(stopped.size(), 1, 5000);
		QVERIFY(!stopped[0][0].toString().isEmpty());
		QVERIFY(client.Busy());
		write(script + ".mode", "ok");
		client.StopForExit();
		QTRY_COMPARE(stopped.size(), 2);
		QVERIFY(stopped[1][0].toString().isEmpty());
		QVERIFY(!client.Busy());
	}
};
int main(int argc, char** argv) {
	QApplication app(argc, argv);
	if (qEnvironmentVariableIsSet("AGS_REMOTE_TEST_PARENT")) {
		RemoteClient client(
		    RemoteClient::TestOptions {qEnvironmentVariable("AGS_REMOTE_TEST_SCRIPT"),
			                           qEnvironmentVariable("AGS_REMOTE_TEST_SETTINGS"), 2500});
		bool played = false;
		QObject::connect(&client, &RemoteClient::Changed, &app, [&] {
			if (!played && !client.Cards().isEmpty() && client.Cards()[0].ready) {
				played = true;
				client.Play(client.Cards()[0].id);
			}
		});
		client.AddProfile("Fixture PC", "127.0.0.1:4444",
		                  qEnvironmentVariable("AGS_REMOTE_TEST_PROFILE"));
		QTimer::singleShot(15000, &app, &QApplication::quit);
		return app.exec();
	}
	RemoteClientTests tests;
	return QTest::qExec(&tests, argc, argv);
}
#include "remoteClientTests.moc"
