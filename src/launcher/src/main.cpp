#include "applicationLifetime.h"
#include "hostControlClient.h"
#include "hostDaemon.h"
#include "launcherTheme.h"
#include "mainDialog.h"

#include <QApplication>
#include <QIcon>
#include <QMessageBox>
#include <QSystemTrayIcon>
#include <QTimer>

#include <cstring>

int main(int argc, char* argv[]) {
	for (int i = 1; i < argc; i++) {
		if (std::strcmp(argv[i], "--host") == 0) return RunHostDaemon(argc, argv);
	}
	QApplication a(argc, argv);
	QApplication::setWindowIcon(QIcon(QStringLiteral(":/brand/ags-app-icon.png")));
	const QString preset_warning = ApplyBundledPreset();
	LauncherTheme::Initialize(a);

	MainDialog w;
	if (!preset_warning.isEmpty()) {
		w.SetPresetWarning(preset_warning);
	}

	w.emit Start();

#ifdef Q_OS_LINUX
	a.setQuitOnLastWindowClosed(false);
	HostControlClient owner;
	owner.SetLifetimeOwner();
	ApplicationLifetime lifetime(&w, QSystemTrayIcon::isSystemTrayAvailable(),
	                             [&] { owner.ExitOwnedHost(); });
	QObject::connect(&owner, &HostControlClient::OwnedHostStopped, &lifetime,
	                 &ApplicationLifetime::Stopped);
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
	lifetime.ShowInitial(a.arguments().contains("--start-minimized"));
	QTimer::singleShot(0, &owner, &HostControlClient::StartDaemon);
#else
	w.show();
#endif

	return QApplication::exec();
}
