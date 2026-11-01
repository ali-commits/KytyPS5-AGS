#include "streamingBackend.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QLockFile>
#include <QPointer>
#include <QSize>
#include <QUdpSocket>
#ifdef __linux__
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
// Resolve and preparation each have a 20-second owner deadline. Include startup,
// ownership handoff and complete runtime cleanup before escalating shutdown.
constexpr int MANAGEMENT_TIMEOUT_MS = 90000;
constexpr int FRAME_LIMIT           = 65536;
} // namespace

StreamingBackend::StreamingBackend(const QString& profile, QObject* parent)
    : QObject(parent), m_profile(QDir(profile).absolutePath()),
      m_native_profile(QDir(m_profile).filePath("native")) {
	m_host.setProcessChannelMode(QProcess::MergedChannels);
#ifdef __linux__
	const auto owner = ::getpid();
	m_host.setChildProcessModifier([owner] {
		::umask(0077);
		if (::prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || ::getppid() != owner) ::_exit(127);
	});
	m_tool.setChildProcessModifier([owner] {
		::umask(0077);
		if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || ::getppid() != owner) ::_exit(127);
	});
#endif
	// Raw child diagnostics can contain private launch paths. Only the native
	// management error DTO is forwarded to UI/control.sock status.
	connect(&m_host, &QProcess::readyReadStandardOutput, this,
	        [this] { m_host.readAllStandardOutput(); });
	connect(&m_host, &QProcess::started, this, [this] {
		if (m_state == State::Stopping)
			m_host.terminate();
		else
			Refresh();
	});
	connect(&m_host, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
		if (error == QProcess::FailedToStart) Fail(tr("Cannot start the native AGS host."));
	});
	connect(&m_host, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
		if (m_state != State::Stopping || (code != 0 && !m_startup_cancel && !m_failed)) {
			m_failed = true;
			m_error  = tr("Native AGS host exited (code %1).").arg(code);
		}
		ClearStatus();
		FinishStop();
	});
	m_poll.setInterval(500);
	connect(&m_poll, &QTimer::timeout, this, &StreamingBackend::Refresh);
	m_start_timeout.setSingleShot(true);
	m_start_timeout.setInterval(20000);
	connect(&m_start_timeout, &QTimer::timeout, this,
	        [this] { Fail(tr("Native AGS management readiness timed out.")); });
	m_stop_timeout.setSingleShot(true);
	connect(&m_stop_timeout, &QTimer::timeout, this, [this] {
		m_failed = true;
		m_error  = tr("Native AGS shutdown timed out; waiting for owned worker cleanup.");
		m_host.terminate();
		const int generation = m_generation;
		QTimer::singleShot(5000, this, [this, generation] {
			if (generation == m_generation && m_host.state() != QProcess::NotRunning) m_host.kill();
		});
		emit Changed();
	});
}

StreamingBackend::~StreamingBackend() {
	disconnect(this, nullptr, nullptr, nullptr);
	Stop();
	if (m_host.state() != QProcess::NotRunning || m_tool.state() != QProcess::NotRunning) {
		QEventLoop loop;
		QTimer     timeout;
		timeout.setSingleShot(true);
		connect(this, &StreamingBackend::Changed, &loop, [&] {
			if (m_host.state() == QProcess::NotRunning && m_tool.state() == QProcess::NotRunning)
				loop.quit();
		});
		connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
		timeout.start(MANAGEMENT_TIMEOUT_MS + 7000);
		loop.exec();
	}
	CancelRequests();
	m_host.disconnect(this);
	m_tool.disconnect(this);
	if (m_host.state() != QProcess::NotRunning) {
		m_host.kill();
		m_host.waitForFinished(1000);
	}
	if (m_tool.state() != QProcess::NotRunning) {
		m_tool.kill();
		m_tool.waitForFinished(1000);
	}
}

void StreamingBackend::SetState(State state) {
	m_state = state;
	emit Changed();
}
void StreamingBackend::Log(const QString& text) {
	m_logs += text + '\n';
	if (m_logs.size() > 16000) m_logs.remove(0, m_logs.size() - 16000);
	emit LogAdded(text);
	emit Changed();
}
void StreamingBackend::ClearStatus() {
	m_status  = {};
	m_clients = {};
	m_session.clear();
	m_closing = false;
}
void StreamingBackend::CancelRequests() {
	for (auto* socket: std::as_const(m_requests)) {
		socket->disconnect(this);
		socket->abort();
		socket->deleteLater();
	}
	m_requests.clear();
	m_refreshing = m_reloading = false;
}
void StreamingBackend::Request(const QJsonObject& request, Done done, int timeout_ms) {
	const auto data = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
	if (data.size() > FRAME_LIMIT) {
		if (done) done({}, tr("Native management request is too large."));
		return;
	}
#ifdef __linux__
	struct stat info {};
	if (::lstat(QFile::encodeName(m_socket).constData(), &info) != 0 || !S_ISSOCK(info.st_mode) ||
	    info.st_uid != ::geteuid() || (info.st_mode & 0077) != 0) {
		if (done) done({}, tr("Native management socket is unavailable or unsafe."));
		return;
	}
#endif
	auto* socket = new QLocalSocket(this);
	socket->setReadBufferSize(FRAME_LIMIT + 1);
	m_requests.insert(socket);
	auto* timer = new QTimer(socket);
	timer->setSingleShot(true);
	const int generation = m_generation;
	auto      finish = [this, socket, timer, generation, done](QJsonObject reply, QString error) {
		if (!m_requests.remove(socket)) return;
		timer->stop();
		socket->disconnect(this);
		socket->abort();
		socket->deleteLater();
		if (generation == m_generation && done) done(std::move(reply), std::move(error));
	};
	connect(timer, &QTimer::timeout, this,
	        [finish] { finish({}, tr("Native management request timed out.")); });
	connect(socket, &QLocalSocket::connected, this, [socket, data, finish] {
#ifdef __linux__
		ucred     peer {};
		socklen_t size = sizeof(peer);
		if (::getsockopt(int(socket->socketDescriptor()), SOL_SOCKET, SO_PEERCRED, &peer, &size) !=
		        0 ||
		    peer.uid != ::geteuid()) {
			finish({}, tr("Native management owner verification failed."));
			return;
		}
#endif
		socket->write(data);
	});
	connect(socket, &QLocalSocket::readyRead, this, [socket, finish] {
		if (socket->bytesAvailable() > FRAME_LIMIT) {
			finish({}, tr("Native management reply is too large."));
			return;
		}
		if (!socket->canReadLine()) return;
		const auto      bytes = socket->readLine();
		QJsonParseError error;
		const auto      doc = QJsonDocument::fromJson(bytes, &error);
		if (error.error != QJsonParseError::NoError || !doc.isObject()) {
			finish({}, tr("Invalid native management reply."));
			return;
		}
		const auto reply = doc.object();
		finish(reply,
		       reply.value("ok").toBool()
		           ? QString()
				   : reply.value("error").toString(tr("Native management rejected the request.")));
	});
	connect(socket, &QLocalSocket::errorOccurred, this, [finish](QLocalSocket::LocalSocketError) {
		finish({}, tr("Native management connection failed."));
	});
	connect(socket, &QLocalSocket::disconnected, this,
	        [finish] { finish({}, tr("Native management connection closed before its reply.")); });
	timer->start(timeout_ms);
	socket->connectToServer(m_socket);
}

QString StreamingBackend::PreflightPorts(int port, int) {
	if (port < 1024 || port > 65535) return tr("QUIC port must be between 1024 and 65535.");
	QUdpSocket socket;
	if (!socket.bind(QHostAddress::Any, quint16(port), QUdpSocket::DontShareAddress))
		return tr("UDP port %1 is busy. Choose another QUIC port.").arg(port);
	return {};
}

bool StreamingBackend::PrepareProfile() {
#ifndef __linux__
	Fail(tr("Native AGS hosting currently requires Linux."));
	return false;
#else
	const auto protect = [](const QString& path) {
		struct stat info {};
		return !QFileInfo(path).isSymLink() && QDir().mkpath(path) &&
		       ::lstat(QFile::encodeName(path).constData(), &info) == 0 && S_ISDIR(info.st_mode) &&
		       info.st_uid == ::geteuid() &&
		       ::chmod(QFile::encodeName(path).constData(), 0700) == 0;
	};
	const auto runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
	const auto hash =
	    QCryptographicHash::hash(m_profile.toUtf8(), QCryptographicHash::Sha256).toHex().left(12);
	const auto directory = QDir(runtime).filePath("ags-" + QString::fromLatin1(hash));
	if (runtime.isEmpty() || !protect(m_profile) || !protect(m_native_profile) ||
	    !protect(directory)) {
		Fail(tr("Cannot create an owner-only native host profile/runtime."));
		return false;
	}
	m_socket = QDir(directory).filePath("management.sock");
	if (m_socket.toUtf8().size() > 106 || m_api_socket.isEmpty()) {
		Fail(tr("Native host needs a valid owner API endpoint."));
		return false;
	}
	m_lock = std::make_unique<QLockFile>(QDir(directory).filePath("launcher.lock"));
	if (!m_lock->tryLock(0)) {
		m_lock.reset();
		Fail(tr("Another launcher owns this streaming profile."));
		return false;
	}
	return true;
#endif
}

QJsonObject StreamingBackend::Configuration() const {
	const auto  size = m_settings.resolution == "1440p" ? QSize(2560, 1440) : QSize(1920, 1080);
	QJsonObject encoder {{"kind", m_settings.encoder}};
	if (m_settings.encoder == "vaapi")
		encoder["device"] =
		    m_settings.adapter.isEmpty() ? "/dev/dri/renderD128" : m_settings.adapter;
	const auto  codec = m_settings.codec.toLower();
	QJsonObject config {
	    {"name", m_settings.EffectiveHostName()},
	    {"bind", QStringLiteral("0.0.0.0:%1").arg(m_settings.port)},
	    {"disconnect_grace_secs", m_settings.disconnect_grace},
	    {"games", QJsonArray {}},
	    {"devices", QJsonObject {}},
	    {"presentation", m_settings.show_local ? "mirrored" : "private"},
	    {"local_audio_sink",
		 m_settings.local_audio
	         ? QJsonValue(m_settings.local_audio_sink.isEmpty() ? "@DEFAULT_SINK@"
			                                                    : m_settings.local_audio_sink)
			 : QJsonValue(QJsonValue::Null)},
	    {"fec_enabled", m_settings.fec_enabled},
	    {"audio_codec", m_settings.audio_codec},
	    {"capture_clock", "common"},
	    {"video", QJsonObject {{"width", size.width()},
		                       {"height", size.height()},
		                       {"fps", m_settings.fps},
		                       {"bitrate_kbps", m_settings.bitrate * 1000},
		                       {"codec", codec},
		                       {"container", codec == "av1" ? "ivf" : "mpeg_ts"},
		                       {"encoder", encoder}}},
	    {"integration",
		 QJsonObject {
	         {"api_socket", m_api_socket},
	         {"management_socket", m_socket},
	         {"local_game_lock",
			  QDir(qEnvironmentVariable("XDG_RUNTIME_DIR")).filePath("kytyps5/emulator.lock")}}}};
	// Preserve the native owner's compositor selection (including its bundled PTS build).
	const auto compositor = m_status.value("configured").toObject().value("compositor_binary");
	if (compositor.isString()) config["compositor_binary"] = compositor;
	return config;
}

void StreamingBackend::Tool(const QStringList& arguments, const QByteArray& input,
                            std::function<void(bool)> done) {
	m_tool.disconnect(this);
	const int generation = m_generation;
	auto      finished   = std::make_shared<bool>(false);
	auto      finish     = [this, generation, done, finished](bool ok) {
		if (*finished) return;
		*finished = true;
		m_tool.readAllStandardOutput();
		m_tool.readAllStandardError();
		if (m_state == State::Stopping) {
			FinishStop();
			return;
		}
		if (generation == m_generation) done(ok);
	};
	connect(&m_tool, &QProcess::finished, this, [finish](int code, QProcess::ExitStatus status) {
		finish(code == 0 && status == QProcess::NormalExit);
	});
	connect(&m_tool, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
		if (error == QProcess::FailedToStart) finish(false);
	});
	connect(&m_tool, &QProcess::started, this, [this, input] {
		m_tool.write(input);
		m_tool.closeWriteChannel();
	});
	m_tool.start(m_settings.backend, arguments);
}

void StreamingBackend::Start(const StreamingSettings& settings) {
	if (m_state == State::Starting || m_state == State::Ready || m_state == State::Stopping) return;
	++m_generation;
	m_failed = false;
	m_startup_cancel = false;
	m_error.clear();
	ClearStatus();
	m_settings = settings;
	SetState(State::Starting);
	auto error = settings.Validate();
	if (m_local_game) error = tr("Close the locally running game before starting hosting.");
	if (error.isEmpty() &&
	    (!QFileInfo(settings.backend).isAbsolute() || !QFileInfo(settings.backend).isExecutable()))
		error = tr("Native AGS host is unavailable. Install libexec/ags-host.");
	if (error.isEmpty()) error = PreflightPorts(settings.port);
	if (!error.isEmpty()) {
		Fail(error);
		return;
	}
	if (!PrepareProfile()) return;
	m_start_timeout.start();
	const auto configure = [this] {
		Tool({"configure", m_native_profile},
		     QJsonDocument(Configuration()).toJson(QJsonDocument::Compact), [this](bool ok) {
			     if (!ok) {
				     Fail(tr("Native owner could not accept host configuration."));
				     return;
			     }
			     StartProcess();
		     });
	};
	if (QFileInfo::exists(QDir(m_native_profile).filePath("host.json")))
		configure();
	else
		Tool({"init", m_native_profile}, {}, [this, configure](bool ok) {
			if (!ok) {
				Fail(tr("Native owner could not initialize its private profile."));
				return;
			}
			configure();
		});
}
void StreamingBackend::StartProcess() {
	if (m_state != State::Starting) return;
	// Keep the production working directory; individual games get the resolver's exact cwd.
	m_host.start(m_settings.backend, {"serve", m_native_profile});
	m_poll.start();
}
void StreamingBackend::Fail(const QString& error) {
	m_failed = true;
	m_error  = error;
	Log(error);
	Stop();
}
void StreamingBackend::Stop() {
	if (m_state == State::Stopped || m_state == State::Stopping) return;
	m_startup_cancel = m_state == State::Starting && !m_failed;
	++m_generation;
	m_poll.stop();
	m_start_timeout.stop();
	CancelRequests();
	SetState(State::Stopping);
	if (m_tool.state() != QProcess::NotRunning) {
		m_tool.kill();
		return;
	}
	if (m_host.state() == QProcess::NotRunning) {
		FinishStop();
		return;
	}
	if (m_startup_cancel && !QFileInfo::exists(m_socket)) {
		// Cancel before readiness without turning an intentional Stop into an error.
		// Native serve handles TERM by draining its independent guardian first.
		m_stop_timeout.start(10000);
		m_host.terminate();
		return;
	}
	m_stop_timeout.start(MANAGEMENT_TIMEOUT_MS);
	Request({{"op", "shutdown"}}, [this](QJsonObject reply, QString error) {
		if (!error.isEmpty()) {
			if (!m_failed) m_error = error;
			m_failed = true;
			m_host.terminate();
			m_stop_timeout.start(5000);
		} else if (reply.value("state") != "stopped") {
			m_error  = tr("Native host did not acknowledge completed shutdown.");
			m_failed = true;
		}
		emit Changed();
	});
}
void StreamingBackend::FinishStop() {
	if (m_host.state() != QProcess::NotRunning || m_tool.state() != QProcess::NotRunning) return;
	m_poll.stop();
	m_start_timeout.stop();
	m_stop_timeout.stop();
	CancelRequests();
	ClearStatus();
	m_lock.reset();
	SetState(m_failed ? State::Error : State::Stopped);
}
void StreamingBackend::AcceptStatus(const QJsonObject& status) {
	m_status  = status;
	m_clients = {};
	for (const auto& value: status.value("paired_devices").toArray()) {
		const auto device = value.toObject();
		m_clients.append(
		    QJsonObject {{"uuid", device.value("id")}, {"name", device.value("name")}});
	}
	m_error   = status.value("error").toString();
	m_session = m_closing                   ? tr("Stopping game…")
	            : RunningGameId().isEmpty() ? tr("No game running · choose a game in AGS")
	            : status.value("media_state") == "starting" ? tr("Starting game…")
	                                                        : tr("Game running");
	if (status.value("restart_required").toBool())
		m_session += tr(" · host endpoint changes require restart");
	emit Changed();
}
void StreamingBackend::Refresh() {
	if (m_refreshing || m_reloading || m_closing ||
	    (m_state != State::Starting && m_state != State::Ready) ||
	    m_host.state() != QProcess::Running)
		return;
	m_refreshing = true;
	Request({{"op", "status"}}, [this](QJsonObject status, QString error) {
		m_refreshing = false;
		if (!error.isEmpty()) {
			if (m_state == State::Ready) Fail(error);
			return;
		}
		if (status.value("state") != "ready" || !status.value("paired_devices").isArray() ||
		    !status.value("active").isObject() || !status.value("configured").isObject()) {
			Fail(tr("Invalid native owner readiness response."));
			return;
		}
		AcceptStatus(status);
		if (m_state == State::Starting) {
			m_start_timeout.stop();
			SetState(State::Ready);
		}
		if (m_catalog_dirty) PublishCatalog();
	});
}
void StreamingBackend::ReloadSettings(const StreamingSettings& settings, Done done) {
	if (m_state == State::Starting && settings.Validate().isEmpty()) {
		m_settings      = settings;
		m_catalog_dirty = true;
		if (done) done({{"ok", true}, {"deferred", true}}, {});
		return;
	}
	if (m_state != State::Ready) {
		if (done) done({}, tr("Hosting is not ready."));
		return;
	}
	if (const auto error = settings.Validate(); !error.isEmpty()) {
		if (done) done({}, error);
		return;
	}
	m_settings  = settings;
	m_reloading = true;
	Request({{"op", "reload"}, {"config", Configuration()}},
	        [this, done](QJsonObject reply, QString error) {
		        m_reloading = false;
		        if (error.isEmpty())
			        AcceptStatus(reply);
		        else {
			        m_error = error;
			        emit Changed();
		        }
		        if (done) done(reply, error);
	        });
}
void StreamingBackend::SetCatalog(const QList<StreamingLaunchEntry>&) {
	m_catalog_dirty = true;
}
void StreamingBackend::SetClientCatalog(const QHash<QString, QList<StreamingLaunchEntry>>&) {
	m_catalog_dirty = true;
}
void StreamingBackend::PublishCatalog() {
	if (m_state != State::Ready || m_reloading) return;
	m_catalog_dirty = false;
	ReloadSettings(m_settings);
}
bool StreamingBackend::StreamGameRunning() const {
	return !RunningGameId().isEmpty();
}
QString StreamingBackend::RunningGameId() const {
	return m_status.value("running").toString();
}
void StreamingBackend::CloseSession(Done done) {
	if (m_state != State::Ready || m_closing) {
		if (done) done({}, tr("Hosting is not ready or game Close is pending."));
		return;
	}
	m_closing = true;
	m_session = tr("Stopping game…");
	emit Changed();
	Request({{"op", "close"}}, [this, done](QJsonObject reply, QString error) {
		m_closing = false;
		if (error.isEmpty())
			AcceptStatus(reply);
		else {
			m_error = error;
			emit Changed();
		}
		if (done) done(reply, error);
	});
}
void StreamingBackend::Unpair(const QString& uuid, Done done) {
	if (m_state != State::Ready) {
		if (done) done({}, tr("Hosting is not ready."));
		return;
	}
	Request({{"op", "revoke"}, {"device_id", uuid}},
	        [this, done](QJsonObject reply, QString error) {
		        if (error.isEmpty())
			        AcceptStatus(reply);
		        else {
			        m_error = error;
			        emit Changed();
		        }
		        if (done) done(reply, error);
	        });
}
void StreamingBackend::ExportPairing(const QString& address, Done done) {
	if (m_state != State::Ready) {
		if (done) done({}, tr("Hosting is not ready."));
		return;
	}
	QJsonObject request {{"op", "pair-ticket"}, {"seconds", 120}};
	if (!address.isEmpty()) request["address"] = address;
	Request(request, std::move(done));
}
void StreamingBackend::Pair(const QString&, const QString&, const QString&) {
	m_error = tr("Native AGS uses a one-use pairing bundle. Export one from the host controls.");
	emit Changed();
}
QJsonObject StreamingBackend::ControllerAudioStatus() const {
	return {{"state", "unavailable"},
	        {"last_error", "Separate controller companions require a native service port."}};
}
void StreamingBackend::SetControllerAudioReceiverEnabled(const QString&, bool) {
	m_error = ControllerAudioStatus().value("last_error").toString();
	emit Changed();
}
void StreamingBackend::RemoveControllerAudioReceiver(const QString&) {
	m_error = ControllerAudioStatus().value("last_error").toString();
	emit Changed();
}
