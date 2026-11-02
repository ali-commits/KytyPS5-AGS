#include "applicationLifetime.h"
#include "guiInstance.h"
#include "hostControlClient.h"
#include "hostDaemon.h"
#include "launcherTheme.h"
#include "mainDialog.h"
#include "remoteClient.h"

#include <QApplication>
#include <QIcon>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSystemTrayIcon>
#include <QTextStream>
#include <QTimer>

#include <cstring>

int main(int argc, char* argv[]) {
	for (int i = 1; i < argc; i++) {
		if (std::strcmp(argv[i], "--host") == 0) return RunHostDaemon(argc, argv);
	}
#ifdef Q_OS_LINUX
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--quit") == 0) {
			QCoreApplication app(argc, argv);
			GuiInstance      instance(GuiInstance::DefaultDirectory());
			QString          error;
			if (instance.Start("quit", &error, 120000) != GuiInstance::Result::Forwarded) {
				QTextStream(stderr) << error << Qt::endl;
				return 1;
			}
			QTextStream(stdout) << "AGS shutdown confirmed." << Qt::endl;
			return 0;
		}
	}
#endif
	QApplication a(argc, argv);
#ifdef Q_OS_LINUX
	GuiInstance instance(GuiInstance::DefaultDirectory());
	QString     instanceError;
	const auto  result = instance.Start(
	    a.arguments().contains("--start-minimized") ? "quiet" : "show", &instanceError);
	if (result == GuiInstance::Result::Forwarded) return 0;
	if (result == GuiInstance::Result::Error) {
		QTextStream(stderr) << instanceError << Qt::endl;
		return 1;
	}
#endif
	QApplication::setWindowIcon(QIcon(QStringLiteral(":/brand/ags-app-icon.png")));
	const bool    clientOnly     = a.arguments().contains("--client-only");
	const QString preset_warning = clientOnly ? QString {} : ApplyBundledPreset();
	LauncherTheme::Initialize(a);

	MainDialog w;
	if (!preset_warning.isEmpty()) {
		w.SetPresetWarning(preset_warning);
	}

	w.emit Start();

#ifdef Q_OS_LINUX
	a.setQuitOnLastWindowClosed(false);
	HostControlClient owner;
	if (!clientOnly) owner.SetLifetimeOwner();
	bool                exitPending = false, hostStopped = false, receiverStopped = false;
	QString             hostStopError, receiverStopError;
	ApplicationLifetime lifetime(&w, QSystemTrayIcon::isSystemTrayAvailable(), [&] {
		exitPending     = true;
		hostStopped     = false;
		receiverStopped = false;
		hostStopError.clear();
		receiverStopError.clear();
		w.setEnabled(false);
		w.Remote()->StopForExit();
		owner.ExitOwnedHost();
	});
	const auto          finishExit = [&] {
		if (!exitPending || !hostStopped || !receiverStopped) return;
		exitPending       = false;
		const auto errors = QStringList {hostStopError, receiverStopError}
		                        .filter(QRegularExpression(".+"))
		                        .join("\n");
		if (!errors.isEmpty()) w.setEnabled(true);
		lifetime.Stopped(errors);
	};
	QObject::connect(&instance, &GuiInstance::ShowRequested, &lifetime, &ApplicationLifetime::Show);
	QObject::connect(&instance, &GuiInstance::ExitRequested, &lifetime, &ApplicationLifetime::Exit);
	QObject::connect(&lifetime, &ApplicationLifetime::ExitCompleted, &instance,
	                 &GuiInstance::CompleteExit);
	QObject::connect(&owner, &HostControlClient::OwnedHostStopped, &lifetime,
	                 [&](const QString& error) {
		                 if (!exitPending) return;
		                 hostStopped   = true;
		                 hostStopError = error;
		                 finishExit();
	                 });
	QObject::connect(w.Remote(), &RemoteClient::Stopped, &lifetime, [&](const QString& error) {
		if (!exitPending) return;
		receiverStopped   = true;
		receiverStopError = error;
		finishExit();
	});
	QObject::connect(&lifetime, &ApplicationLifetime::ReadyToQuit, &a, &QApplication::quit,
	                 Qt::QueuedConnection);
	QObject::connect(&owner, &HostControlClient::CommandFailed, &w, [&](const QString& error) {
		w.showNormal();
		auto* box = new QMessageBox(QMessageBox::Warning, "AGS host", error, QMessageBox::Ok, &w);
		box->setAttribute(Qt::WA_DeleteOnClose);
		box->open();
	});
	QObject::connect(&owner, &HostControlClient::StatusChanged, &lifetime,
	                 [&](const QJsonObject& status) { lifetime.SetStatus(status); });
	QObject::connect(w.Remote(), &RemoteClient::Changed, &lifetime, [&] {
        lifetime.SetReceiverActivity(w.Remote()->Activity(), clientOnly);
    });
    lifetime.SetReceiverActivity(w.Remote()->Activity(), clientOnly);
	lifetime.ShowInitial(a.arguments().contains("--start-minimized"));
	if (!clientOnly) QTimer::singleShot(0, &owner, &HostControlClient::StartDaemon);
#else
	w.show();
#endif

	return QApplication::exec();
}
