#include "streamingBackend.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QStandardPaths>
#include <QTcpServer>
#include <QUdpSocket>
#include <QUuid>
#ifdef __linux__
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

StreamingBackend::StreamingBackend(const QString& profile, QObject* parent)
    : QObject(parent), m_profile(QDir(profile).absolutePath()), m_audio(this), m_api(this) {
	m_host.setProcessChannelMode(QProcess::MergedChannels);
#ifdef __linux__
	m_host.setChildProcessModifier([] { ::umask(0077); });
#endif
	connect(&m_host, &QProcess::readyReadStandardOutput, this,
	        [this] { Log(QString::fromUtf8(m_host.readAllStandardOutput())); });
	connect(&m_host, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
		if (error == QProcess::FailedToStart)
			Fail("Cannot start the streaming helper: " + m_host.errorString());
	});
	connect(&m_host, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
		if (m_state != State::Stopping) {
			m_failed = true;
			m_error  = QStringLiteral(
			               "Streaming helper exited (code %1). Check diagnostics before restarting.")
			               .arg(code);
			Stop();
		} else
			AfterHostExit();
	});
	connect(&m_audio, &StreamingAudio::Ready, this, [this] {
		if (m_state != State::Starting) {
			m_audio.Stop();
			return;
		}
		if (!WriteConfiguration()) return;
		const auto credentials =
		    QJsonDocument(QJsonObject {{"username", m_username}, {"password", m_password}})
		        .toJson(QJsonDocument::Compact) +
		    '\n';
		Tool(
		    m_settings.backend, {QDir(m_profile).filePath("host.conf"), "--creds-stdin"},
		    [this](bool ok, const QByteArray&) {
			    if (!ok) {
				    Fail("Streaming helper could not initialize private credentials.");
				    return;
			    }
			    StartProcess();
		    },
		    credentials);
	});
	connect(&m_audio, &StreamingAudio::Failed, this, &StreamingBackend::Fail);
	connect(&m_audio, &StreamingAudio::Diagnostic, this, &StreamingBackend::Log);
	connect(&m_audio, &StreamingAudio::Stopped, this, &StreamingBackend::TryFinishStop);
	m_poll.setInterval(1500);
	connect(&m_poll, &QTimer::timeout, this, &StreamingBackend::Refresh);
	m_start_timeout.setSingleShot(true);
	m_start_timeout.setInterval(20000);
	connect(&m_start_timeout, &QTimer::timeout, this, [this] {
		Fail("Host startup timed out. Check NVENC, ports and the private audio service in "
		     "diagnostics.");
	});
}
StreamingBackend::~StreamingBackend() {
	// Keep the internal completion handlers alive while shutdown drains. In
	// particular, pactl may already have created a module whose ID is in flight.
	disconnect(this, nullptr, nullptr, nullptr);
	Stop();
	const auto complete = [this] {
		return m_tools.isEmpty() && m_host.state() == QProcess::NotRunning && m_audio.IsStopped();
	};
	if (!complete()) {
		QEventLoop loop;
		QTimer     timeout;
		timeout.setSingleShot(true);
		connect(this, &StreamingBackend::Changed, &loop, [&] {
			if (complete()) loop.quit();
		});
		connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
		// Setup/cleanup commands have individual four-second deadlines; host
		// termination and private audio-daemon shutdown are bounded as well.
		timeout.start(25000);
		loop.exec();
	}
	disconnect(&m_host, nullptr, this, nullptr);
	disconnect(&m_audio, nullptr, this, nullptr);
	for (auto* process: m_tools.values()) {
		process->kill();
		process->waitForFinished(1000);
	}
}
void StreamingBackend::SetState(State state) {
	m_state = state;
	emit Changed();
}
void StreamingBackend::Log(const QString& text) {
	auto safe = text;
	if (!m_password.isEmpty()) safe.replace(m_password, "[private]");
	m_logs += safe.trimmed() + '\n';
	if (m_logs.size() > 100000) m_logs.remove(0, m_logs.size() - 100000);
	emit LogAdded(safe);
	emit Changed();
}
void StreamingBackend::Tool(const QString& program, const QStringList& arguments, ToolDone done,
                            const QByteArray& input) {
	const int generation = m_generation;
	auto*     process    = new QProcess(this);
	m_tools.insert(process);
#ifdef __linux__
	process->setChildProcessModifier([] { ::umask(0077); });
#endif
	auto* timeout = new QTimer(process);
	timeout->setSingleShot(true);
	auto       completed = std::make_shared<bool>(false);
	const auto finish    = [this, process, timeout, completed, generation, done](bool ok) {
		if (*completed) return;
		*completed = true;
		timeout->stop();
		const auto output = process->readAllStandardOutput();
		m_tools.remove(process);
		process->deleteLater();
		if (generation == m_generation && m_state != State::Stopping) done(ok, output);
		TryFinishStop();
	};
	connect(timeout, &QTimer::timeout, process, [process] { process->kill(); });
	connect(process, &QProcess::finished, this, [finish](int code, QProcess::ExitStatus status) {
		finish(code == 0 && status == QProcess::NormalExit);
	});
	connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
		if (error == QProcess::FailedToStart) finish(false);
	});
	connect(process, &QProcess::started, this, [this, process, input, generation] {
		if (m_state == State::Stopping || generation != m_generation) {
			process->kill();
			return;
		}
		if (!input.isEmpty()) process->write(input);
		process->closeWriteChannel();
	});
	process->start(program, arguments);
	timeout->start(6000);
}
QString StreamingBackend::PreflightPorts(int base) {
	if (base < 1024 || base > 65514) return "Invalid base port.";
	QList<std::shared_ptr<QTcpServer>> tcp;
	QList<std::shared_ptr<QUdpSocket>> udp;
	for (int offset: {-5, 0, 1, 21}) {
		auto server = std::make_shared<QTcpServer>();
		if (!server->listen(QHostAddress::Any, quint16(base + offset)))
			return QStringLiteral("TCP port %1 is busy. Stop the conflicting service or choose "
			                      "another base port.")
			    .arg(base + offset);
		tcp.append(server);
	}
	for (int offset: {9, 10, 11}) {
		auto socket = std::make_shared<QUdpSocket>();
		if (!socket->bind(QHostAddress::Any, quint16(base + offset), QUdpSocket::DontShareAddress))
			return QStringLiteral("UDP port %1 is busy. Stop the conflicting service or choose "
			                      "another base port.")
			    .arg(base + offset);
		udp.append(socket);
	}
	return {};
}
bool StreamingBackend::PrepareProfile() {
#ifndef __linux__
	Fail("Native streaming currently requires Linux, NVIDIA NVENC and PulseAudio/PipeWire.");
	return false;
#else
	const auto private_directory = [](const QString& path) {
		if (QFileInfo(path).isSymLink() || !QDir().mkpath(path)) return false;
		struct stat info {};
		const auto  encoded = QFile::encodeName(path);
		return ::lstat(encoded.constData(), &info) == 0 && S_ISDIR(info.st_mode) &&
		       info.st_uid == ::getuid() && ::chmod(encoded.constData(), 0700) == 0;
	};
	if (m_profile.contains('\n') || m_profile.contains('\r') || !private_directory(m_profile)) {
		Fail("Cannot create an owner-only streaming profile.");
		return false;
	}
	const auto runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
	if (runtime.isEmpty() || !QFileInfo(runtime).isDir()) {
		Fail("XDG_RUNTIME_DIR is unavailable. Start the launcher inside a normal desktop login "
		     "session.");
		return false;
	}
	const auto identity = QString::fromLatin1(
	    QCryptographicHash::hash(m_profile.toUtf8(), QCryptographicHash::Sha256).toHex().left(12));
	const auto base = QDir(runtime).filePath("kyty-stream");
	m_runtime       = QDir(base).filePath(identity);
	m_socket        = QDir(m_runtime).filePath("frames.sock");
	if (m_socket.toUtf8().size() > 106 || !private_directory(base) ||
	    !private_directory(m_runtime)) {
		Fail("Cannot create a private runtime endpoint (or the path is too long).");
		return false;
	}
	m_lock = std::make_unique<QLockFile>(QDir(m_profile).filePath("launcher.lock"));
	if (!m_lock->tryLock(0)) {
		m_lock.reset();
		Fail("Another launcher owns this streaming profile.");
		return false;
	}
	const auto credentials_path = QDir(m_profile).filePath("controller-auth.json");
	QFile      credentials(credentials_path);
	if (credentials.exists()) {
		if (QFileInfo(credentials_path).isSymLink() ||
		    QFileInfo(credentials_path).ownerId() != ::getuid() ||
		    !credentials.open(QIODevice::ReadOnly)) {
			Fail("Cannot read private management credentials.");
			return false;
		}
		const auto data = QJsonDocument::fromJson(credentials.readAll()).object();
		m_username      = data.value("username").toString();
		m_password      = data.value("password").toString();
		credentials.close();
		QFile::setPermissions(credentials_path, QFile::ReadOwner | QFile::WriteOwner);
		if (m_username.isEmpty() || m_password.size() < 32) {
			Fail("Private management credentials are invalid. Remove controller-auth.json while "
			     "the host is stopped to regenerate them.");
			return false;
		}
	} else {
		m_username = "kyty";
		m_password =
		    QUuid::createUuid().toString(QUuid::Id128) + QUuid::createUuid().toString(QUuid::Id128);
		QString error;
		if (!StreamingCatalog::WritePrivate(
		        credentials_path,
		        QJsonDocument(QJsonObject {{"username", m_username}, {"password", m_password}})
		            .toJson(),
		        &error)) {
			Fail(error);
			return false;
		}
	}
	return true;
#endif
}
void StreamingBackend::Start(const StreamingSettings& settings) {
	if (m_state == State::Starting || m_state == State::Ready || m_state == State::Stopping) return;
	m_failed = false;
	m_error.clear();
	m_version.clear();
	m_settings = settings;
	++m_generation;
	SetState(State::Starting);
	auto error = settings.Validate();
	if (m_local_game)
		error = "Close the locally running game before starting hosting. Moonlight launches games "
		        "from the shared library.";
	if (error.isEmpty() &&
	    (!QFileInfo(settings.backend).isAbsolute() || !QFileInfo(settings.backend).isExecutable()))
		error = "Streaming helper is unavailable. Choose the packaged libexec/kyty_stream_host "
		        "executable.";
	if (error.isEmpty()) error = PreflightPorts(settings.port);
	if (!error.isEmpty()) {
		Fail(error);
		return;
	}
	const auto wrapper =
	    QDir(QCoreApplication::applicationDirPath()).filePath("libexec/kyty_stream_launch");
	if (!QFileInfo(wrapper).isExecutable()) {
		Fail("Missing libexec/kyty_stream_launch. Install the complete streaming package.");
		return;
	}
	if (!PrepareProfile()) return;
	m_start_timeout.start();
	Tool(settings.backend, {"--kyty-capabilities"}, [this](bool ok, const QByteArray& output) {
		QJsonObject capabilities;
		for (const auto& line: output.split('\n')) {
			const auto object = QJsonDocument::fromJson(line).object();
			if (!object.isEmpty()) capabilities = object;
		}
		if (!ok || capabilities.value("protocol_version").toInt() != 1 ||
		    capabilities.value("capture") != "kyty" ||
		    capabilities.value("gpu_transport") != "vulkan-buffer-fd" ||
		    !capabilities.value("private_audio").toBool() ||
		    !capabilities.value("private_input").toBool() ||
		    !capabilities.value("credentials_stdin").toBool()) {
			Fail("This executable does not support the Kyty private GPU/audio/input host protocol. "
			     "Choose the experimental streaming helper.");
			return;
		}
		m_version = capabilities.value("version").toString(
		    "Kyty host protocol 1 · Vulkan buffer FD / NVENC");
		m_audio.Start(m_settings.local_audio);
		emit Changed();
	});
}
bool StreamingBackend::WriteConfiguration() {
	const QDir profile(m_profile);
	QString    error;
	const auto manifest = StreamingCatalog::Manifest(m_catalog, m_socket, m_audio.Device(),
	                                                 QDir(m_runtime).filePath("emulator.lock"),
	                                                 m_settings.resolution, m_settings.fps);
	auto       actual_manifest = manifest;
	if (!m_audio.Server().isEmpty()) {
		auto env = actual_manifest.value("environment").toObject();
		env.insert("PULSE_SERVER", m_audio.Server());
		actual_manifest.insert("environment", env);
	}
	actual_manifest.insert("session_file", QDir(m_runtime).filePath("session.json"));
	const auto apps = StreamingCatalog::Apps(
	    m_catalog,
	    QDir(QCoreApplication::applicationDirPath()).filePath("libexec/kyty_stream_launch"),
	    profile.filePath("launch.json"));
	if (!StreamingCatalog::WritePrivate(profile.filePath("launch.json"),
	                                    QJsonDocument(actual_manifest).toJson(), &error) ||
	    !StreamingCatalog::WritePrivate(profile.filePath("apps.json"), QJsonDocument(apps).toJson(),
	                                    &error)) {
		Fail(error);
		return false;
	}
	QString config = "capture = kyty\nencoder = nvenc\nkyty_private_audio = true\nkyty_input = "
	                 "true\nmanagement_bind_address = 127.0.0.1\nupnp = disabled\nsystem_tray = "
	                 "disabled\norigin_web_ui_allowed = pc\nsunshine_name = Kyty PS5 "
	                 "Streaming\nstream_audio = enabled\nav1_mode = 1\n";
	config += QStringLiteral("port = %1\nmax_bitrate = %2\nhevc_mode = %3\n")
	              .arg(m_settings.port)
	              .arg(m_settings.bitrate * 1000)
	              .arg(m_settings.codec == "H264" ? 1 : 2);
	config += "kyty_socket = " + m_socket + '\n';
	config += "audio_sink = " + m_audio.Sink() + '\n';
	if (!m_settings.adapter.isEmpty()) config += "adapter_name = " + m_settings.adapter + '\n';
	for (const auto& item: QList<QPair<QString, QString>> {{"file_apps", "apps.json"},
	                                                       {"file_state", "host-state.json"},
	                                                       {"credentials_file", "host-auth.json"},
	                                                       {"pkey", "host-key.pem"},
	                                                       {"cert", "host-cert.pem"},
	                                                       {"log_path", "host.log"}})
		config += item.first + " = " + profile.filePath(item.second) + '\n';
	if (!StreamingCatalog::WritePrivate(profile.filePath("host.conf"), config.toUtf8(), &error)) {
		Fail(error);
		return false;
	}
	m_catalog_dirty = false;
	return true;
}
void StreamingBackend::StartProcess() {
	if (m_state != State::Starting) return;
	m_host.setWorkingDirectory(m_profile);
	auto environment = QProcessEnvironment::systemEnvironment();
	if (!m_audio.Server().isEmpty()) environment.insert("PULSE_SERVER", m_audio.Server());
	m_host.setProcessEnvironment(environment);
	m_host.start(m_settings.backend, {QDir(m_profile).filePath("host.conf")});
	m_poll.start();
	Log("Starting private Kyty host. Waiting for authenticated loopback management readiness.");
}
void StreamingBackend::Fail(const QString& error) {
	m_error  = error;
	m_failed = true;
	Log(error);
	Stop();
}
void StreamingBackend::Stop() {
	if (m_state == State::Stopped) return;
	if (m_state == State::Stopping) {
		CancelTools();
		if (m_host.state() == QProcess::NotRunning) m_audio.Stop();
		TryFinishStop();
		return;
	}
	++m_generation;
	m_poll.stop();
	m_start_timeout.stop();
	m_api.Abort();
	m_refreshing = false;
	SetState(State::Stopping);
	CancelTools();
	if (m_host.state() == QProcess::NotRunning) {
		AfterHostExit();
		return;
	}
	const int  generation = m_generation;
	const auto terminate  = [this, generation] {
		if (generation != m_generation) return;
		const auto pid = m_host.processId();
		if (m_host.state() != QProcess::NotRunning) m_host.terminate();
		QTimer::singleShot(3000, this, [this, pid, generation] {
			if (generation == m_generation && pid > 0 && m_host.processId() == pid) m_host.kill();
		});
	};
	m_api.Request("POST", "/api/apps/close", {},
	              [terminate](const QJsonObject&, const QString&) { terminate(); });
}
void StreamingBackend::AfterHostExit() {
	m_poll.stop();
	m_start_timeout.stop();
	m_refreshing = false;
	m_api.Abort();
	if (!m_runtime.isEmpty()) QFile::remove(QDir(m_runtime).filePath("session.json"));
	m_audio.Stop();
	TryFinishStop();
}
void StreamingBackend::CancelTools() {
	for (auto* process: m_tools.values()) {
		if (process->state() == QProcess::NotRunning) continue;
		process->terminate();
		QTimer::singleShot(1000, process, [process] {
			if (process->state() != QProcess::NotRunning) process->kill();
		});
	}
}
void StreamingBackend::TryFinishStop() {
	if (m_state != State::Stopping || !m_tools.isEmpty() ||
	    m_host.state() != QProcess::NotRunning || !m_audio.IsStopped())
		return;
	m_lock.reset();
	m_session.clear();
	m_pairings = {};
	m_clients  = {};
	SetState(m_failed ? State::Error : State::Stopped);
}
void StreamingBackend::Refresh() {
	if (m_refreshing || (m_state != State::Starting && m_state != State::Ready) ||
	    m_host.state() != QProcess::Running)
		return;
	const int generation = m_generation;
	if (m_state == State::Starting) {
		QFile cert_file(QDir(m_profile).filePath("host-cert.pem"));
		if (!cert_file.open(QIODevice::ReadOnly)) return;
		const QSslCertificate certificate(cert_file.readAll());
		if (certificate.isNull()) return;
		m_api.Configure(m_settings.port + 1, certificate, m_username, m_password);
	}
	m_refreshing = true;
	m_api.Request(
	    "GET", "/api/pin", {}, [this, generation](const QJsonObject& data, const QString& error) {
		    if (generation != m_generation) return;
		    m_refreshing = false;
		    if (!error.isEmpty()) {
			    if (m_state == State::Ready)
				    ApiError(error);
			    else if (error.contains("HTTP 401") || error.contains("SSL", Qt::CaseInsensitive) ||
			             error.contains("TLS", Qt::CaseInsensitive))
				    Fail("Private host authentication/TLS failed: " + error);
			    return;
		    }
		    if (!data.value("pairings").isArray()) {
			    ApiError("Host returned an invalid pending-pairing response.");
			    return;
		    }
		    m_pairings = data.value("pairings").toArray();
		    if (m_state == State::Starting) {
			    m_start_timeout.stop();
			    m_error.clear();
			    SetState(State::Ready);
			    Log("Hosting ready. Add the displayed LAN address in Moonlight, pair, then choose "
			        "a library game.");
		    }
		    m_api.Request("GET", "/api/clients/list", {},
		                  [this, generation](const QJsonObject& clients, const QString& error) {
			                  if (generation != m_generation) return;
			                  if (!error.isEmpty())
				                  ApiError(error);
			                  else if (!clients.value("named_certs").isArray())
				                  ApiError("Host returned an invalid client list.");
			                  else {
				                  m_clients = clients.value("named_certs").toArray();
				                  emit Changed();
			                  }
		                  });
		    m_session = "No game running · choose a game in Moonlight";
		    QFile session(QDir(m_runtime).filePath("session.json"));
		    if (session.open(QIODevice::ReadOnly)) {
			    const auto current = QJsonDocument::fromJson(session.readAll()).object();
#ifdef __linux__
			    const auto pid = current.value("pid").toInteger();
			    const auto executable =
			        QFileInfo(QStringLiteral("/proc/%1/exe").arg(pid)).symLinkTarget();
			    if (pid > 1 && ::kill(pid_t(pid), 0) == 0 &&
			        QFileInfo(executable).fileName() == "kyty_stream_launch")
#endif
				    m_session =
				        QStringLiteral("Game running · Moonlight requested %1×%2 at %3 FPS · %4")
				            .arg(current.value("width").toString(),
				                 current.value("height").toString(),
				                 current.value("fps").toString(),
				                 current.value("client").toString());
		    }
		    if (m_catalog_dirty && m_session.startsWith("No game")) PublishCatalog();
		    emit Changed();
	    });
}
void StreamingBackend::ApiError(const QString& error) {
	m_error = error;
	Log(error);
	emit Changed();
}
void StreamingBackend::Pair(const QString& id, const QString& pin, const QString& name) {
	if (m_state != State::Ready) return;
	m_api.SubmitPin(id, pin, name, [this](const QJsonObject&, const QString& error) {
		if (!error.isEmpty())
			ApiError(error);
		else {
			m_error.clear();
			Refresh();
		}
	});
}
void StreamingBackend::Unpair(const QString& uuid) {
	if (m_state != State::Ready || uuid.isEmpty() || uuid.size() > 128) return;
	m_api.Request("POST", "/api/clients/unpair", {{"uuid", uuid}},
	              [this](const QJsonObject&, const QString& error) {
		              if (!error.isEmpty())
			              ApiError(error);
		              else
			              Refresh();
	              });
}
void StreamingBackend::CloseSession() {
	if (m_state != State::Ready) return;
	m_api.Request("POST", "/api/apps/close", {}, [this](const QJsonObject&, const QString& error) {
		if (!error.isEmpty())
			ApiError(error);
		else
			Refresh();
	});
}
void StreamingBackend::SetCatalog(const QList<StreamingLaunchEntry>& catalog) {
	if (m_catalog == catalog) return;
	m_catalog       = catalog;
	m_catalog_dirty = true;
}
void StreamingBackend::PublishCatalog() {
	// Replacing proc while an app runs invalidates upstream session ownership. Defer until idle.
	if (m_catalog_dirty)
		Log("Library changed. Stop and restart hosting to publish the refreshed catalog safely.");
	m_catalog_dirty = false;
}
