#pragma once

#include <QDialog>
#include <QProcess>

class Configuration;
class QLabel;
class QProgressBar;
class QPushButton;
class QPlainTextEdit;

class ShaderPrecompileDialog: public QDialog {
	Q_OBJECT
public:
	ShaderPrecompileDialog(const QString& interpreter, const Configuration& game,
	                       QWidget* parent = nullptr);
	~ShaderPrecompileDialog() override;
	bool IsRunning() const { return !m_finished; }

signals:
	void RunningChanged();

protected:
	void reject() override;

private:
	void ReadOutput();
	void ReadLine(const QByteArray& line);
	void Cancel();
	void StopProcess(bool force);
	void Finish(int exit_code, QProcess::ExitStatus status);

	QProcess        m_process;
	QByteArray      m_buffer;
	QByteArray      m_diagnostics;
	QLabel*         m_status    = nullptr;
	QProgressBar*   m_progress  = nullptr;
	QPushButton*    m_button    = nullptr;
	QPlainTextEdit* m_details   = nullptr;
	bool            m_finished  = false;
	bool            m_cancelled = false;
	bool            m_complete  = false;
	bool            m_empty     = false;
	quint64         m_failures  = 0;
	qint64          m_group     = 0;
};
