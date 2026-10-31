#include "hostDaemon.h"
#include "launcherTheme.h"
#include "mainDialog.h"

#include <QApplication>
#include <QIcon>

#include <cstring>

int main(int argc, char* argv[]) {
	for (int i = 1; i < argc; i++) {
		if (std::strcmp(argv[i], "--host") == 0) return RunHostDaemon(argc, argv);
	}
	QApplication a(argc, argv);
	// Title bars and taskbars get the bolder small-size artwork.
	const QIcon small_art(QStringLiteral(":/icons/kyty-small.svg"));
	const QIcon large_art(QStringLiteral(":/icons/kyty.svg"));
	QIcon       icon;
	for (int size: {16, 24, 32, 48, 64}) {
		icon.addPixmap(small_art.pixmap(QSize(size, size), 1.0));
	}
	for (int size: {128, 256}) {
		icon.addPixmap(large_art.pixmap(QSize(size, size), 1.0));
	}
	QApplication::setWindowIcon(icon);
	const QString preset_warning = ApplyBundledPreset();
	LauncherTheme::Initialize(a);

	MainDialog w;
	if (!preset_warning.isEmpty()) {
		w.SetPresetWarning(preset_warning);
	}

	w.emit Start();

	w.show();

	return QApplication::exec();
}
