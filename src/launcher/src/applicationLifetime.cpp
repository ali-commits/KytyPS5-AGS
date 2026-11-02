#include "applicationLifetime.h"

#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QSaveFile>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QWidget>
ApplicationLifetime::ApplicationLifetime(QWidget* window, bool available,
                                         std::function<void()> stop)
    : QObject(window), m_window(window), m_available(available), m_stop(std::move(stop)) {
	window->installEventFilter(this);
	qApp->installEventFilter(this);
	if (available) {
		m_tray     = new QSystemTrayIcon(QApplication::windowIcon(), this);
		auto* menu = new QMenu(window);
		auto  show = [this] {
			m_window->showNormal();
			m_window->raise();
			m_window->activateWindow();
		};
		menu->addAction(tr("Show AGS"), this, show);
		menu->addAction(tr("Exit AGS"), this, &ApplicationLifetime::Exit);
		m_tray->setContextMenu(menu);
		connect(m_tray, &QSystemTrayIcon::activated, this,
		        [show](QSystemTrayIcon::ActivationReason reason) {
			        if (reason == QSystemTrayIcon::Trigger ||
					    reason == QSystemTrayIcon::DoubleClick)
				        show();
		        });
		m_tray->show();
		auto* timer = new QTimer(this);
		connect(timer, &QTimer::timeout, this, [this] {
			if (!QSystemTrayIcon::isSystemTrayAvailable()) {
				m_available = false;
				m_window->showNormal();
			} else
				m_available = true;
		});
		timer->start(1000);
	}
}
bool ApplicationLifetime::eventFilter(QObject* object, QEvent* event) {
	if (object == qApp && event->type() == QEvent::Quit && !m_allow_quit) {
		Exit();
		return true;
	}
	if (object == m_window && event->type() == QEvent::KeyPress &&
	    static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
		m_window->close();
		return true;
	}
	if (object == m_window && event->type() == QEvent::Close) {
		event->ignore();
		if (m_available)
			m_window->hide();
		else {
			auto* box =
			    new QMessageBox(QMessageBox::Question, tr("Exit AGS"),
				                tr("No system tray is available. Exit AGS and stop hosting?"),
				                QMessageBox::Yes | QMessageBox::No, m_window);
			box->setAttribute(Qt::WA_DeleteOnClose);
			connect(box, &QMessageBox::finished, this, [this](int result) {
				if (result == QMessageBox::Yes) Exit();
			});
			box->open();
		}
		return true;
	}
	return QObject::eventFilter(object, event);
}
void ApplicationLifetime::ShowInitial(bool minimized) {
	if (!minimized || !m_available) m_window->show();
}
void ApplicationLifetime::Exit() {
	if (m_exiting) return;
	m_exiting = true;
	m_stop();
}
void ApplicationLifetime::Stopped(const QString& error) {
	if (error.isEmpty()) {
		if (m_exiting) {
			m_allow_quit = true;
			emit ReadyToQuit();
		}
		return;
	}
	m_exiting = false;
	m_window->showNormal();
	auto* box = new QMessageBox(QMessageBox::Critical, tr("AGS host shutdown failed"), error,
	                            QMessageBox::Ok, m_window);
	box->setAttribute(Qt::WA_DeleteOnClose);
	box->open();
}
void ApplicationLifetime::SetStatus(const QString& status) {
	if (m_tray) m_tray->setToolTip("AGS — " + status);
}
bool ApplicationLifetime::WriteAutostart(const QString& config, const QString& executable,
                                         bool enabled, QString* error) {
	const auto path = QDir(config).filePath("autostart/ags.desktop");
	auto       fail = [error](const QString& text) {
		if (error) *error = text;
		return false;
	};
	for (auto c: executable)
		if (c.unicode() < 32 || c.unicode() == 127) return fail("Invalid executable path.");
	if (!enabled)
		return !QFile::exists(path) || QFile::remove(path) ||
		       fail("Cannot remove AGS login autostart.");
	if (!QDir().mkpath(QFileInfo(path).absolutePath()))
		return fail("Cannot create login autostart directory.");
	QString quoted;
	for (auto c: executable) {
		if (c == '%')
			quoted += "%%";
		else {
			if (c == '"' || c == '`' || c == '$' || c == '\\') quoted += "\\";
			quoted += c;
		}
	}
	// Desktop-entry string escaping precedes Exec argument escaping.
	quoted.replace("\\", "\\\\");
	QSaveFile  file(path);
	const auto data = QString("[Desktop Entry]\nType=Application\nName=AGS\nExec=\"%1\" "
	                          "--start-minimized\nTerminal=false\n")
	                      .arg(quoted)
	                      .toUtf8();
	if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
		return fail("Cannot save AGS login autostart.");
	return true;
}
