#include "hostControlClient.h"

#include "hostSocket.h"

#include <QFutureWatcher>
#include <QJsonDocument>
#include <QPointer>
#include <QProcess>
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

HostControlClient::HostControlClient(QString socket_path, QObject* parent)
    : QObject(parent), m_socket(std::move(socket_path)),
      m_systemctl(qEnvironmentVariable("KYTY_HOST_SYSTEMCTL", QStringLiteral("systemctl"))),
      m_timer(new QTimer(this)) {
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
	// A systemctl action started just before exit must not be killed mid-flight.
	for (auto* process: findChildren<QProcess*>()) {
		process->blockSignals(true); // no callbacks into objects that may already be gone
		process->waitForFinished(500);
	}
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

void HostControlClient::RunSystemctl(const QStringList& arguments, bool report,
                                     std::function<void(bool)> done) {
	auto* process = new QProcess(this);
	auto  finish  = [this, process, report, done = std::move(done)](bool ok, const QString& what) {
		if (process->property("done").toBool()) return;
		process->setProperty("done", true);
		const auto detail = QString::fromUtf8(process->readAllStandardError()).trimmed();
		process->deleteLater();
		if (!ok && report)
			emit CommandFailed(detail.isEmpty() ? what : what + QStringLiteral(": ") + detail);
		if (done) done(ok);
	};
	connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError) {
		finish(false, QStringLiteral("Cannot run systemctl"));
	});
	connect(process, &QProcess::finished, this,
	        [finish, arguments](int code, QProcess::ExitStatus) {
		        finish(code == 0, QStringLiteral("systemctl %1 failed").arg(arguments.join(' ')));
	        });
	process->start(m_systemctl, arguments);
}

void HostControlClient::StartDaemon() {
	RunSystemctl({"--user", "start", UNIT}, true, {});
}

void HostControlClient::RestartDaemon() {
	RunSystemctl({"--user", "restart", UNIT}, true, {});
}

void HostControlClient::StopDaemon() {
	RunSystemctl({"--user", "stop", UNIT}, true, {});
}

void HostControlClient::SetAutostart(bool on) {
	RunSystemctl({"--user", on ? "enable" : "disable", UNIT}, true, {});
}

void HostControlClient::QueryAutostart(std::function<void(bool enabled)> done) {
	// is-enabled exits non-zero for "disabled": that is an answer, not a failure to report.
	RunSystemctl({"--user", "is-enabled", UNIT}, false, std::move(done));
}
