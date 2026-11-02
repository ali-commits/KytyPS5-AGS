#ifndef AGS_GUI_INSTANCE_H
#define AGS_GUI_INSTANCE_H
#include <QLocalServer>
#include <QLockFile>
#include <QObject>
#include <QPointer>

#include <memory>
class QLocalSocket;
// The lock spans the whole GUI lifetime, including asynchronous host cleanup.
class GuiInstance: public QObject {
	Q_OBJECT
public:
	enum class Result { Owner, Forwarded, Error };
	explicit GuiInstance(const QString& directory, QObject* parent = nullptr);
	static QString DefaultDirectory();
	Result         Start(const QByteArray& command, QString* error, int timeoutMs = 5000);
	void           CompleteExit(const QString& error);
signals:
	void ShowRequested();
	void ExitRequested();

private:
	bool                          Prepare(QString* error);
	void                          Accept();
	QString                       m_directory;
	std::unique_ptr<QLockFile>    m_lock;
	QLocalServer                  m_server;
	QList<QPointer<QLocalSocket>> m_quitters;
};
#endif
