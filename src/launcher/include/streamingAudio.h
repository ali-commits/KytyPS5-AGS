#ifndef KYTY_STREAMING_AUDIO_H
#define KYTY_STREAMING_AUDIO_H
#include <QObject>
#include <QProcessEnvironment>
#include <QStringList>

#include <functional>
#include <memory>
class QProcess;
class QTemporaryDir;
class StreamingAudio: public QObject {
	Q_OBJECT
public:
	explicit StreamingAudio(QObject* parent = nullptr);
	~StreamingAudio() override;
	void        Start(bool monitor);
	void        Stop();
	QString     Sink() const { return m_sink; }
	QString     Device() const { return m_device; }
	QString     Server() const { return m_server; }
	bool        IsStopped() const { return m_stop_complete; }
	void        SetPactl(const QString& program) { m_pactl = program; }
	static bool IsOwnedModule(const QString& arguments, const QString& sink, bool loopback);
signals:
	void Ready();
	void Stopped();
	void Failed(const QString& error);
	void Diagnostic(const QString& text);

private:
	void        Command(const QStringList& arguments, std::function<void(bool, QByteArray)> done);
	void        UnloadNext();
	void        VerifyRoute(bool monitor);
	void        CreateSink(bool monitor);
	void        StartIsolated(bool monitor);
	void        WaitForIsolated(int remaining, int generation);
	void        StopDaemons();
	void        FinishStop();
	QString     m_pactl = "pactl";
	QString     m_sink;
	QString     m_device;
	QString     m_original_sink;
	QString     m_sink_module;
	QString     m_loop_module;
	QStringList m_unload;
	bool        m_stopping       = false;
	QProcess*   m_active         = nullptr;
	QProcess*   m_private_server = nullptr;
	QProcess*   m_private_policy = nullptr;
	std::unique_ptr<QTemporaryDir> m_private_directory;
	QProcessEnvironment            m_environment;
	QString                        m_server;
	bool                           m_isolated   = false;
	bool                           m_unloading  = false;
	int                            m_generation = 0;
	bool                           m_stop_complete  = true;
	bool                           m_sink_attempted = false;
	bool                           m_loop_attempted = false;
};
#endif
