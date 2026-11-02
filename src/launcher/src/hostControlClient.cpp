#include "hostControlClient.h"

#include "applicationLifetime.h"
#include "hostSocket.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>
#include <QtConcurrent>

#include <utility>

namespace {
constexpr int REQUEST_TIMEOUT_MS = 3000;
constexpr int POLL_INTERVAL_MS   = 1000;

struct CallResult {
	QJsonObject reply;
	QString     error;
};

CallResult Call(const QString& socket, const QJsonObject& request) {
	CallResult result;
	const auto type    = request.value("type").toString();
	const int  timeout = type == "stop-game" || type == "set-preferences" || type == "unpair" ||
	                             type == "pair-ticket"
	                         ? 95000
	                         : REQUEST_TIMEOUT_MS;
	const auto reply   = HostSocketCall(socket, request, timeout, &result.error);
	if (!reply) return result;
	const auto doc = QJsonDocument::fromJson(reply->body);
	if (!doc.isObject()) {
		result.error = QStringLiteral("Invalid reply from the host daemon");
	} else if (reply->status != 200) {
		result.error = doc.object().value("error").toString(
		    QStringLiteral("Host daemon returned status %1").arg(reply->status));
	} else {
		result.reply = doc.object();
	}
	return result;
}
} // namespace

HostControlClient* HostControlClient::s_owner = nullptr;

HostControlClient::HostControlClient(QString socket_path, QObject* parent)
    : QObject(parent), m_socket(std::move(socket_path)), m_timer(new QTimer(this)) {
	if (m_socket.isEmpty()) {
		m_socket = qEnvironmentVariable("KYTY_HOST_SOCKET",
		                                HostRuntimeDirectory() + QStringLiteral("/control.sock"));
	}
	m_timer->setInterval(POLL_INTERVAL_MS);
	connect(m_timer, &QTimer::timeout, this, &HostControlClient::Poll);
	m_timer->start();
	QTimer::singleShot(0, this, &HostControlClient::Poll);
}

HostControlClient::~HostControlClient() {
	if (s_owner == this) s_owner = nullptr;
}

void HostControlClient::Request(const QJsonObject&                                    request,
                                std::function<void(QJsonObject reply, QString error)> done) {
	auto* watcher = new QFutureWatcher<CallResult>(this);
	connect(watcher, &QFutureWatcher<CallResult>::finished, this,
	        [watcher, done = std::move(done)] {
		        auto result = watcher->result();
		        watcher->deleteLater();
		        if (done) done(std::move(result.reply), std::move(result.error));
	        });
	watcher->setFuture(QtConcurrent::run(Call, m_socket, request));
}

void HostControlClient::Poll() {
	if (m_polling) return; // never stack calls behind a slow daemon
	m_polling = true;
	Request({{"type", "status"}}, [this](QJsonObject reply, QString error) {
		m_polling = false;
		emit StatusChanged(error.isEmpty() ? reply : QJsonObject {});
	});
}

void HostControlClient::SetLifetimeOwner(const QString& executable) {
	s_owner      = this;
	m_executable = executable.isEmpty() ? QCoreApplication::applicationFilePath() : executable;
}
void HostControlClient::StartDaemon() {
	if (s_owner && s_owner != this) {
		s_owner->StartDaemon();
		return;
	}
	if (!s_owner) {
		emit CommandFailed(tr("Open AGS to start its owned host."));
		return;
	}
	if (m_owned || m_starting) return;
	m_starting = true;
	Request({{"type", "status"}}, [this](QJsonObject, QString error) {
		m_starting = false;
		if (error.isEmpty()) {
			emit CommandFailed(tr("An independent AGS host is already running. Stop its existing "
			                      "service or diagnostic --host process, then select Start. AGS "
			                      "will not change that service or stop an unowned host."));
			return;
		}
		m_process_error.clear();
		m_owned       = new QProcess(this);
		auto* process = m_owned;
		connect(process, &QProcess::readyReadStandardError, this, [this, process] {
			m_process_error += QString::fromUtf8(process->readAllStandardError());
			m_process_error = m_process_error.right(8192);
		});
		auto finish = [this, process](int code, QProcess::ExitStatus status) {
			if (m_owned != process) return;
			m_owned = nullptr;
			process->deleteLater();
			const auto error =
			    code == 0 && status == QProcess::NormalExit
			        ? QString()
			        : tr("AGS host did not shut down cleanly. %1").arg(m_process_error.trimmed());
			const bool stopping = m_stopping;
			m_stopping          = false;
			if (m_restart && error.isEmpty()) {
				m_restart = false;
				StartDaemon();
			} else {
				m_restart = false;
				if (stopping)
					emit OwnedHostStopped(error);
				else
					emit CommandFailed(
					    error.isEmpty()
					        ? tr("AGS host stopped unexpectedly. Select Start to retry.")
							: error);
			}
		};
		connect(process, &QProcess::finished, this, finish);
		connect(process, &QProcess::errorOccurred, this,
		        [this, process, finish](QProcess::ProcessError error) {
			        if (error == QProcess::FailedToStart) {
				        m_process_error = process->errorString();
				        finish(-1, QProcess::CrashExit);
			        }
		        });
		connect(process, &QProcess::readyReadStandardOutput, this,
		        [process] { process->readAllStandardOutput(); });
		process->setProcessChannelMode(QProcess::SeparateChannels);
		process->start(m_executable, {"--host"});
	});
}
void HostControlClient::RestartDaemon() {
	if (s_owner && s_owner != this) {
		s_owner->RestartDaemon();
		return;
	}
	if (!m_owned) {
		StartDaemon();
		return;
	}
	m_restart = true;
	StopDaemon();
}
void HostControlClient::StopDaemon() {
	if (s_owner && s_owner != this) {
		s_owner->StopDaemon();
		return;
	}
	if (!m_owned) {
		emit CommandFailed(
		    tr("No GUI-owned host is running. Independent hosts are left unchanged."));
		return;
	}
	if (m_stopping) return;
	m_stopping = true;
	m_owned->terminate();
	QPointer<QProcess> process(m_owned);
	QTimer::singleShot(110000, this, [this, process] {
		if (process && m_owned == process && m_stopping) {
			m_stopping = false;
			m_restart  = false;
			emit OwnedHostStopped(tr("The host has not acknowledged shutdown. AGS remains open; "
			                         "retry Exit after checking the host."));
		}
	});
}
void HostControlClient::ExitOwnedHost() {
	m_restart = false;
	if (m_starting) {
		QTimer::singleShot(100, this, &HostControlClient::ExitOwnedHost);
		return;
	}
	if (!m_owned) {
		emit OwnedHostStopped({});
		return;
	}
	StopDaemon();
}
void HostControlClient::SetAutostart(bool on) {
	QString error;
	if (!ApplicationLifetime::WriteAutostart(
	        (m_autostart_directory.isEmpty()
	             ? QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
				 : m_autostart_directory),
	        QCoreApplication::applicationFilePath(), on, &error))
		emit CommandFailed(error);
}
void HostControlClient::QueryAutostart(std::function<void(bool)> done) {
	done(QFileInfo::exists(
	    QDir((m_autostart_directory.isEmpty()
	              ? QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
				  : m_autostart_directory))
	        .filePath("autostart/ags.desktop")));
}
