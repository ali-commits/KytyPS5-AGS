#ifndef AGS_APPLICATION_LIFETIME_H
#define AGS_APPLICATION_LIFETIME_H
#include <QIcon>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>
class QWidget;
class QSystemTrayIcon;
class ApplicationLifetime: public QObject {
	Q_OBJECT
public:
	ApplicationLifetime(QWidget* window, bool trayAvailable, std::function<void()> stop);
	void ShowInitial(bool minimized);
	void Exit();
	void Stopped(const QString& error);
	enum class TrayState { Unavailable, Idle, GameRunning, PairingOpen };
	void             SetStatus(const QJsonObject& status);
	static TrayState StateForStatus(const QJsonObject& status);
	static QIcon     TrayIcon(TrayState state);
	static bool      WriteAutostart(const QString& config, const QString& executable, bool enabled,
	                                QString* error);
signals:
	void ReadyToQuit();

protected:
	bool eventFilter(QObject*, QEvent*) override;

private:
	QWidget*              m_window;
	QSystemTrayIcon*      m_tray = nullptr;
	bool                  m_available;
	bool                  m_exiting    = false;
	bool                  m_allow_quit = false;
	std::function<void()> m_stop;
};
#endif
