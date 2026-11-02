#include "applicationLifetime.h"
#include "guiInstance.h"

#include <QApplication>
#include <QFile>
#include <QLocalSocket>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWidget>

#include <cstdio>
class GuiInstanceTests: public QObject {
	Q_OBJECT
	void start(QProcess& process, const QString& dir, const QString& mode = "success") {
		process.start(QCoreApplication::applicationFilePath(), {"worker", dir, mode});
		QVERIFY(process.waitForStarted());
		QVERIFY(process.waitForReadyRead());
		QCOMPARE(process.readLine().trimmed(), QByteArray("owner"));
	}
private slots:
	void duplicateAndExit() {
		QTemporaryDir dir;
		QProcess      owner;
		start(owner, dir.path());
		GuiInstance duplicate(dir.path());
		QString     error;
		QCOMPARE(duplicate.Start("show", &error, 2000), GuiInstance::Result::Forwarded);
		QVERIFY(owner.waitForReadyRead());
		QCOMPARE(owner.readLine().trimmed(), QByteArray("show"));
		GuiInstance quiet(dir.path());
		QCOMPARE(quiet.Start("quiet", &error, 2000), GuiInstance::Result::Forwarded);
		QVERIFY(!owner.waitForReadyRead(100));
		GuiInstance quit(dir.path());
		QCOMPARE(quit.Start("quit", &error, 2000), GuiInstance::Result::Forwarded);
		QVERIFY(owner.waitForFinished());
		QCOMPARE(owner.exitCode(), 0);
	}
	void failureAndTimeout() {
		for (const QString& mode: {QString("fail"), QString("timeout"), QString("hold")}) {
			QTemporaryDir dir;
			QProcess      owner;
			start(owner, dir.path(), mode);
			GuiInstance quit(dir.path());
			QString     error;
			QCOMPARE(quit.Start("quit", &error, 250), GuiInstance::Result::Error);
			QVERIFY(!error.isEmpty());
			QCOMPARE(owner.state(), QProcess::Running);
			if (mode == "fail") QVERIFY(error.contains("fake cleanup failed"));
			owner.kill();
			QVERIFY(owner.waitForFinished());
		}
	}
	void concurrentOwnershipAndStaleRecovery() {
		QTemporaryDir dir;
		QProcess      one, two;
		const auto    args = QStringList {"worker", dir.path(), "success"};
		one.start(QCoreApplication::applicationFilePath(), args);
		two.start(QCoreApplication::applicationFilePath(), args);
		QVERIFY(one.waitForReadyRead());
		QVERIFY(two.waitForReadyRead());
		const auto a = one.readLine().trimmed(), b = two.readLine().trimmed();
		QVERIFY((a == "owner" && b == "forwarded") || (b == "owner" && a == "forwarded"));
		QProcess& owner = a == "owner" ? one : two;
		owner.kill();
		QVERIFY(owner.waitForFinished());
		QProcess& forwarded = a == "owner" ? two : one;
		QVERIFY(forwarded.waitForFinished() || forwarded.state() == QProcess::NotRunning);
		GuiInstance replacement(dir.path());
		QString     error;
		QCOMPARE(replacement.Start("quiet", &error, 2000), GuiInstance::Result::Owner);
	}
	void privateDirectoryAndUnknownMessages() {
		QTemporaryDir dir;
		QFile::setPermissions(dir.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
		                                      QFile::ReadOther);
		GuiInstance unsafe(dir.path());
		QString     error;
		QCOMPARE(unsafe.Start("quiet", &error, 100), GuiInstance::Result::Error);
		QFile::setPermissions(dir.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
		QProcess owner;
		start(owner, dir.path());
		QLocalSocket socket;
		socket.connectToServer(dir.filePath("control"));
		QVERIFY(socket.waitForConnected());
		socket.write("run arbitrary-command\n");
		socket.flush();
		QVERIFY(socket.waitForReadyRead());
		QVERIFY(socket.readAll().startsWith("error "));
		owner.kill();
		QVERIFY(owner.waitForFinished());
	}
	void initialWindowBehavior() {
		QWidget             window;
		ApplicationLifetime lifetime(&window, true, [] {});
		lifetime.ShowInitial(true);
		QVERIFY(!window.isVisible());
		lifetime.Show();
		QVERIFY(window.isVisible());
		window.hide();
		lifetime.ShowInitial(false);
		QVERIFY(window.isVisible());
	}
};
int main(int argc, char** argv) {
	QApplication app(argc, argv);
	app.setQuitOnLastWindowClosed(false);
	if (app.arguments().value(1) == "worker") {
		GuiInstance instance(app.arguments().value(2));
		QString     error;
		const auto  result = instance.Start("quiet", &error, 2000);
		if (result != GuiInstance::Result::Owner) {
			puts(result == GuiInstance::Result::Forwarded ? "forwarded" : "error");
			fflush(stdout);
			return result == GuiInstance::Result::Forwarded ? 0 : 1;
		}
		QWidget              window;
		const auto           mode     = app.arguments().value(3);
		ApplicationLifetime* lifetime = nullptr;
		ApplicationLifetime  controller(&window, false, [&] {
			if (mode != "timeout")
				QTimer::singleShot(20, lifetime, [&, mode] {
					lifetime->Stopped(mode == "fail" ? "fake cleanup failed" : "");
				});
		});
		lifetime = &controller;
		QObject::connect(&instance, &GuiInstance::ShowRequested, &controller, [] {
			puts("show");
			fflush(stdout);
		});
		QObject::connect(&instance, &GuiInstance::ExitRequested, &controller,
		                 &ApplicationLifetime::Exit);
		QObject::connect(&controller, &ApplicationLifetime::ExitCompleted, &instance,
		                 &GuiInstance::CompleteExit);
		if (mode != "hold")
			QObject::connect(&controller, &ApplicationLifetime::ReadyToQuit, &app,
			                 &QApplication::quit, Qt::QueuedConnection);
		puts("owner");
		fflush(stdout);
		return app.exec();
	}
	GuiInstanceTests tests;
	return QTest::qExec(&tests, argc, argv);
}
#include "GuiInstanceTests.moc"
