#include "shaderPrecompileDialog.h"

#include "configuration.h"
#include "emulatorCommand.h"

#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QTimer>
#include <QVBoxLayout>

#ifdef Q_OS_UNIX
#include <signal.h>
#include <unistd.h>
#endif

ShaderPrecompileDialog::ShaderPrecompileDialog(const QString&       interpreter,
                                               const Configuration& game, QWidget* parent)
    : QDialog(parent) {
	setObjectName("shaderPrecompileDialog");
	setWindowTitle(tr("Precompile shaders — %1").arg(game.name));
	setWindowModality(Qt::WindowModal);
	resize(540, 240);
	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(18, 18, 18, 18);
	layout->setSpacing(12);
	auto* heading = new QLabel(game.name, this);
	heading->setTextFormat(Qt::PlainText);
	heading->setWordWrap(true);
	auto font = heading->font();
	font.setBold(true);
	heading->setFont(font);
	layout->addWidget(heading);
	auto* explanation = new QLabel(tr("Prepares saved shader recipes for this game. New areas may "
	                                  "still compile additional shaders."),
	                               this);
	explanation->setWordWrap(true);
	layout->addWidget(explanation);
	m_status = new QLabel(tr("Opening the game's shader cache…"), this);
	m_status->setObjectName("precompileStatus");
	m_status->setWordWrap(true);
	layout->addWidget(m_status);
	m_progress = new QProgressBar(this);
	m_progress->setObjectName("precompileProgress");
	m_progress->setAccessibleName(tr("Shader preparation progress"));
	m_progress->setRange(0, 0);
	layout->addWidget(m_progress);
	m_details = new QPlainTextEdit(this);
	m_details->setObjectName("precompileDetails");
	m_details->setReadOnly(true);
	m_details->setMaximumBlockCount(100);
	m_details->hide();
	layout->addWidget(m_details);
	layout->addStretch();
	m_button = new QPushButton(tr("Cancel"), this);
	m_button->setObjectName("precompileCancelClose");
	m_button->setAutoDefault(false);
	layout->addWidget(m_button, 0, Qt::AlignRight);
	connect(m_button, &QPushButton::clicked, this, [this] {
		if (m_finished)
			accept();
		else
			Cancel();
	});
	m_process.setProgram(interpreter);
	m_process.setArguments(BuildEmulatorArgs(game));
	m_process.setWorkingDirectory(QFileInfo(interpreter).absolutePath());
	auto environment = QProcessEnvironment::systemEnvironment();
	environment.insert("KYTY_PIPELINE_LIST_PREPARE", "1");
	environment.insert("KYTY_PIPELINE_LIST", "1");
	environment.insert("KYTY_SHADER_PRECOMPILE", "1");
	environment.insert("KYTY_SHADER_PRECOMPILE_REPLAY", "1");
	environment.insert("KYTY_SHADER_PRECOMPILE_MAX", "0");
	environment.insert("KYTY_SHADER_PRECOMPILE_WAIT", "0");
	environment.insert("KYTY_PIPELINE_JOURNAL", "1");
	environment.insert("KYTY_PIPELINE_JOURNAL_REPLAY", "1");
	// A translator update changes module hashes while retaining compatible recipe inputs.
	// Preparation must build the selected game's current pipelines, not only its old keys.
	environment.insert("KYTY_PIPELINE_JOURNAL_RETARGET", "1");
	environment.insert("KYTY_PIPELINE_JOURNAL_WAIT", "0");
	m_process.setProcessEnvironment(environment);
	m_process.setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_UNIX
	// The wrapper and its emulator child form one owned group, including during cancellation.
	m_process.setChildProcessModifier([] { ::setsid(); });
#endif
	connect(&m_process, &QProcess::readyReadStandardOutput, this,
	        &ShaderPrecompileDialog::ReadOutput);
	connect(&m_process, &QProcess::started, this, [this] { m_group = m_process.processId(); });
	connect(&m_process, &QProcess::finished, this, &ShaderPrecompileDialog::Finish);
	connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
		if (error == QProcess::FailedToStart) {
			m_diagnostics += m_process.errorString().toUtf8();
			Finish(-1, QProcess::CrashExit);
		}
	});
	QTimer::singleShot(0, this, [this] {
		if (m_finished) return;
		if (m_process.arguments().isEmpty())
			Finish(-1, QProcess::CrashExit);
		else
			m_process.start();
	});
}

ShaderPrecompileDialog::~ShaderPrecompileDialog() {
	m_process.disconnect(this);
	if (m_process.state() != QProcess::NotRunning) {
		StopProcess(true);
		m_process.waitForFinished(3000);
	}
}

void ShaderPrecompileDialog::ReadOutput() {
	const auto bytes = m_process.readAllStandardOutput();
	m_diagnostics    = (m_diagnostics + bytes).right(16384);
	m_buffer += bytes;
	qsizetype newline;
	while ((newline = m_buffer.indexOf('\n')) >= 0) {
		ReadLine(m_buffer.left(newline).trimmed());
		m_buffer.remove(0, newline + 1);
	}
	if (m_buffer.size() > 65536) m_buffer = m_buffer.right(1024);
}

void ShaderPrecompileDialog::ReadLine(const QByteArray& line) {
	if (m_cancelled) return;
	static const QRegularExpression message(
	    QStringLiteral("^KYTY_PREPARE_PROGRESS "
	                   "(shaders|pipelines|compute|raytracing|saving|complete|failed|empty) "
	                   "([0-9]+) ([0-9]+) ([0-9]+)$"));
	const auto match = message.match(QString::fromUtf8(line));
	if (!match.hasMatch()) return;
	bool       done_ok = false, total_ok = false, failed_ok = false;
	const auto done   = match.captured(2).toULongLong(&done_ok);
	const auto total  = match.captured(3).toULongLong(&total_ok);
	const auto failed = match.captured(4).toULongLong(&failed_ok);
	if (!done_ok || !total_ok || !failed_ok || done > total) return;
	m_failures       = qMax(m_failures, failed);
	const auto phase = match.captured(1);
	if (phase == "complete") {
		m_complete = done == total && total > 0; // Also require a successful process exit.
		return;
	}
	if (phase == "empty") {
		m_empty = true;
		m_status->setText(tr("No saved shader recipes are available for this game yet. Play it to "
		                     "record recipes, or add a matching pipeline list."));
		return;
	}
	if (phase == "failed") return;
	if (phase == "shaders" || phase == "pipelines") {
		m_status->setText((phase == "shaders" ? tr("Preparing shaders: %1 of %2")
		                                      : tr("Building graphics pipelines: %1 of %2"))
		                      .arg(done)
		                      .arg(total));
		m_progress->setRange(0, total == 0 ? 0 : 1000);
		if (total != 0) m_progress->setValue(int((static_cast<long double>(done) / total) * 1000));
	} else {
		m_status->setText(phase == "compute"      ? tr("Preparing compute pipelines…")
		                  : phase == "raytracing" ? tr("Preparing lighting shaders…")
		                                          : tr("Saving shader caches…"));
		m_progress->setRange(0, 0);
	}
}

void ShaderPrecompileDialog::StopProcess(bool force) {
#ifdef Q_OS_UNIX
	if (m_group > 0) ::kill(-pid_t(m_group), force ? SIGKILL : SIGTERM);
#endif
	if (m_process.state() != QProcess::NotRunning) {
		if (force)
			m_process.kill();
		else
			m_process.terminate();
	}
}

void ShaderPrecompileDialog::Cancel() {
	if (m_finished || m_cancelled) return;
	m_cancelled = true;
	m_button->setEnabled(false);
	m_status->setText(tr("Stopping preparation…"));
	StopProcess(false);
	if (m_process.state() == QProcess::NotRunning)
		Finish(-1, QProcess::CrashExit);
	else
		QTimer::singleShot(3000, this, [this] {
			if (!m_finished) StopProcess(true);
		});
}

void ShaderPrecompileDialog::reject() {
	if (!m_finished)
		Cancel();
	else
		QDialog::reject();
}

void ShaderPrecompileDialog::Finish(int exit_code, QProcess::ExitStatus status) {
	if (m_finished) return;
	ReadOutput();
	if (!m_buffer.isEmpty()) ReadLine(m_buffer.trimmed());
	m_finished = true;
	if (m_cancelled) StopProcess(true); // also reap a wrapper's surviving descendants
	const bool success = !m_cancelled && status == QProcess::NormalExit && exit_code == 0 &&
	                     m_complete && m_failures == 0;
	m_progress->setRange(0, 1000);
	m_progress->setValue(success ? 1000 : 0);
	if (m_cancelled)
		m_status->setText(tr("Preparation cancelled. Previously saved caches are kept."));
	else if (success)
		m_status->setText(tr("Shader preparation complete. Ready to play."));
	else {
		if (!m_empty)
			m_status->setText(
			    m_failures != 0 ? tr("Preparation incomplete: some recipes could not be prepared.")
			                    : tr("Shader preparation could not finish. See details below."));
		m_details->setPlainText(QString::fromUtf8(m_diagnostics));
		m_details->show();
	}
	m_button->setText(tr("Close"));
	m_button->setEnabled(true);
	emit RunningChanged();
}
