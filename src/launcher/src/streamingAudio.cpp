#include "streamingAudio.h"

#include "streamingCatalog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>

StreamingAudio::StreamingAudio(QObject* parent)
    : QObject(parent), m_environment(QProcessEnvironment::systemEnvironment()) {}
StreamingAudio::~StreamingAudio() {
	for (auto* process: {m_private_policy, m_private_server}) {
		if (process && process->state() != QProcess::NotRunning) {
			process->disconnect(this);
			process->kill();
			process->waitForFinished(1000);
		}
	}
}
bool StreamingAudio::IsOwnedModule(const QString& arguments, const QString& sink, bool loopback) {
	const auto tokens = QProcess::splitCommand(arguments);
	return !sink.isEmpty() &&
	       tokens.contains(loopback ? "source=" + sink + ".monitor" : "sink_name=" + sink);
}
void StreamingAudio::Command(const QStringList&                    arguments,
                             std::function<void(bool, QByteArray)> done) {
	auto* process = new QProcess(this);
	m_active      = process;
	process->setProcessEnvironment(m_environment);
	auto* timer = new QTimer(process);
	timer->setSingleShot(true);
	timer->setInterval(4000);
	connect(timer, &QTimer::timeout, process, [process] { process->kill(); });
	auto       completed = std::make_shared<bool>(false);
	const auto finish    = [this, process, timer, completed, done](bool success) {
		if (*completed) return;
		*completed = true;
		timer->stop();
		const auto output =
		    success ? process->readAllStandardOutput() : process->readAllStandardError();
		if (m_active == process) m_active = nullptr;
		process->deleteLater();
		done(success, output);
	};
	connect(process, &QProcess::finished, this, [finish](int code, QProcess::ExitStatus status) {
		finish(code == 0 && status == QProcess::NormalExit);
	});
	connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
		if (error == QProcess::FailedToStart) finish(false);
	});
	process->start(m_pactl, arguments);
	timer->start();
}
void StreamingAudio::Start(bool monitor) {
	if (m_active || !m_sink_module.isEmpty() || m_private_server) {
		emit Failed("Private audio is already active.");
		return;
	}
	++m_generation;
	m_stopping       = false;
	m_unloading      = false;
	m_isolated       = false;
	m_environment    = QProcessEnvironment::systemEnvironment();
	const auto token = QUuid::createUuid().toString(QUuid::Id128).left(12);
	m_sink           = "kyty_stream_" + token;
	m_device         = "Kyty_Stream_" + token;
	Command({"get-default-sink"}, [this, monitor](bool ok, const QByteArray& output) {
		if (!ok || output.trimmed().isEmpty()) {
			emit Failed("PulseAudio/PipeWire is unavailable. Install/start a Pulse-compatible "
			            "server and pactl.");
			return;
		}
		m_original_sink = QString::fromUtf8(output).trimmed();
		if (m_stopping) {
			Stop();
			return;
		}
		if (m_original_sink == "auto_null") {
			StartIsolated(monitor);
			return;
		}
		CreateSink(monitor);
	});
}
void StreamingAudio::CreateSink(bool monitor) {
	Command(
	    {"load-module", "module-null-sink", "sink_name=" + m_sink, "rate=48000", "channels=2",
	     "format=float32le",
	     "sink_properties=device.description=" + m_device +
	         (m_isolated ? QString {}
	                     : QStringLiteral(
	                           " node.virtual=true device.class=filter priority.session=-1000"))},
	    [this, monitor](bool loaded, const QByteArray& output) {
		    const auto id = QString::fromUtf8(output).trimmed();
		    if (!loaded || !QRegularExpression("^[0-9]+$").match(id).hasMatch()) {
			    emit Failed("Could not create a private game-audio sink.");
			    return;
		    }
		    m_sink_module = id;
		    if (m_stopping) {
			    Stop();
			    return;
		    }
		    if (m_isolated)
			    emit Ready();
		    else
			    VerifyRoute(monitor);
	    });
}
void StreamingAudio::StartIsolated(bool monitor) {
	const auto runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
	if (runtime.isEmpty()) {
		emit Failed("A private audio server requires XDG_RUNTIME_DIR from a desktop login.");
		return;
	}
	m_private_directory =
	    std::make_unique<QTemporaryDir>(QDir(runtime).filePath("kyty-audio-XXXXXX"));
	if (!m_private_directory->isValid() ||
	    !QFile::setPermissions(m_private_directory->path(),
	                           QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
		emit Failed("Cannot create a private audio runtime.");
		return;
	}
	const auto path   = m_private_directory->path();
	const auto socket = QDir(path).filePath("pulse.sock");
	if (socket.toUtf8().size() > 106 || socket.contains('"') || socket.contains('\n') ||
	    socket.contains('\r') || socket.contains('\\')) {
		emit Failed("Private audio socket path is unsupported or too long.");
		return;
	}
	QString config =
	    R"PW(context.properties = { core.daemon = true core.name = kyty-private support.dbus = false default.clock.rate = 48000 }
context.spa-libs = { audio.convert.* = audioconvert/libspa-audioconvert support.* = support/libspa-support }
context.modules = [
 { name = libpipewire-module-protocol-native }
 { name = libpipewire-module-metadata }
 { name = libpipewire-module-spa-node-factory }
 { name = libpipewire-module-client-node }
 { name = libpipewire-module-access }
 { name = libpipewire-module-adapter }
 { name = libpipewire-module-link-factory }
 { name = libpipewire-module-protocol-pulse }
]
context.objects = [
 { factory = metadata args = { metadata.name = default } }
 { factory = spa-node-factory args = { factory.name = support.node.driver node.name = Dummy-Driver node.group = pipewire.dummy priority.driver = 20000 } }
]
pulse.properties = { server.address = [ "unix:SOCKET" ] pulse.allow-module-loading = true }
stream.properties = { adapter.auto-port-config = { mode = dsp } }
)PW";
	config.replace("SOCKET", socket);
	QString error;
	if (!StreamingCatalog::WritePrivate(QDir(path).filePath("pipewire.conf"), config.toUtf8(),
	                                    &error)) {
		emit Failed(error);
		return;
	}
	m_server   = "unix:" + socket;
	m_isolated = true;
	m_environment.insert("PIPEWIRE_RUNTIME_DIR", path);
	m_environment.insert("PIPEWIRE_REMOTE", "kyty-private");
	m_environment.insert("PULSE_SERVER", m_server);
	m_environment.insert("XDG_STATE_HOME", QDir(path).filePath("state"));
	m_environment.insert("XDG_CONFIG_HOME", QDir(path).filePath("config"));
	if (monitor)
		emit Diagnostic("Local playback is unavailable while the system has only Dummy Output. "
		                "Moonlight still receives the private game audio.");
	emit Diagnostic("Starting an isolated game-audio server; the system audio graph and default "
	                "sink stay separate.");
	m_private_server = new QProcess(this);
	m_private_server->setProcessEnvironment(m_environment);
	m_private_server->setProcessChannelMode(QProcess::MergedChannels);
	m_private_server->setStandardOutputFile(QDir(path).filePath("pipewire.log"));
	connect(m_private_server, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
		if (error == QProcess::FailedToStart && !m_stopping)
			emit Failed("Cannot start private PipeWire. Install pipewire and wireplumber.");
	});
	connect(m_private_server, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
		if (m_stopping)
			FinishStop();
		else
			emit Failed("Private audio server exited. See its runtime diagnostics.");
	});
	m_private_server->start("pipewire", {"-c", QDir(path).filePath("pipewire.conf")});
	WaitForIsolated(80, m_generation);
}
void StreamingAudio::WaitForIsolated(int remaining, int generation) {
	if (generation != m_generation || m_stopping || !m_private_server) return;
	if (QFileInfo::exists(m_server.mid(5))) {
		m_private_policy = new QProcess(this);
		m_private_policy->setProcessEnvironment(m_environment);
		m_private_policy->setProcessChannelMode(QProcess::MergedChannels);
		m_private_policy->setStandardOutputFile(
		    QDir(m_private_directory->path()).filePath("wireplumber.log"));
		connect(m_private_policy, &QProcess::errorOccurred, this,
		        [this](QProcess::ProcessError error) {
			        if (error == QProcess::FailedToStart && !m_stopping)
				        emit Failed(
				            "Cannot start the private WirePlumber policy. Install wireplumber.");
		        });
		connect(m_private_policy, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
			if (m_stopping)
				FinishStop();
			else
				emit Failed("Private audio policy exited.");
		});
		connect(m_private_policy, &QProcess::started, this, [this, generation] {
			QTimer::singleShot(600, this, [this, generation] {
				if (generation == m_generation && !m_stopping) CreateSink(false);
			});
		});
		m_private_policy->start("wireplumber", {"--profile", "policy"});
		return;
	}
	if (remaining <= 0) {
		emit Failed("Private audio server did not create its Pulse socket in time.");
		return;
	}
	QTimer::singleShot(
	    50, this, [this, remaining, generation] { WaitForIsolated(remaining - 1, generation); });
}
void StreamingAudio::VerifyRoute(bool monitor) {
	Command({"get-default-sink"}, [this, monitor](bool ok, const QByteArray& output) {
		if (!ok || QString::fromUtf8(output).trimmed() != m_original_sink) {
			emit Failed("Audio-server policy changed its default sink when creating the private "
			            "route. Hosting was stopped; choose a stable playback device first.");
			return;
		}
		if (m_stopping) {
			Stop();
			return;
		}
		if (!monitor) {
			emit Ready();
			return;
		}
		Command({"load-module", "module-loopback", "source=" + m_sink + ".monitor",
		         "sink=" + m_original_sink, "latency_msec=35"},
		        [this](bool loaded, const QByteArray& output) {
			        const auto id = QString::fromUtf8(output).trimmed();
			        if (!loaded || !QRegularExpression("^[0-9]+$").match(id).hasMatch()) {
				        emit Failed("Could not enable local audio monitoring. Disable local "
				                    "playback and retry.");
				        return;
			        }
			        m_loop_module = id;
			        if (m_stopping)
				        Stop();
			        else
				        emit Ready();
		        });
	});
}
void StreamingAudio::Stop() {
	if (!m_stopping) ++m_generation;
	m_stopping = true;
	if (m_active || m_unloading) return;
	m_unloading = true;
	if (m_sink_module.isEmpty() && m_loop_module.isEmpty()) {
		StopDaemons();
		return;
	}
	Command({"--format=json", "list", "modules"}, [this](bool ok, const QByteArray& output) {
		m_unload.clear();
		if (ok) {
			for (const auto& value: QJsonDocument::fromJson(output).array()) {
				const auto module    = value.toObject();
				const auto id        = QString::number(module.value("index").toInt(-1));
				const auto arguments = module.value("argument").toString();
				if (id == m_loop_module && module.value("name") == "module-loopback" &&
				    IsOwnedModule(arguments, m_sink, true))
					m_unload.prepend(id);
				if (id == m_sink_module && module.value("name") == "module-null-sink" &&
				    IsOwnedModule(arguments, m_sink, false))
					m_unload.append(id);
			}
		} else
			emit Diagnostic("Cannot verify Pulse module ownership; leaving modules untouched.");
		UnloadNext();
	});
}
void StreamingAudio::UnloadNext() {
	if (m_unload.isEmpty()) {
		m_sink_module.clear();
		m_loop_module.clear();
		StopDaemons();
		return;
	}
	const auto id = m_unload.takeFirst();
	Command({"unload-module", id}, [this](bool ok, const QByteArray&) {
		if (!ok) emit Diagnostic("Pulse module could not be unloaded.");
		UnloadNext();
	});
}

void StreamingAudio::StopDaemons() {
	for (auto* process: {m_private_policy, m_private_server}) {
		if (!process || process->state() == QProcess::NotRunning) continue;
		process->terminate();
		QTimer::singleShot(2000, process, [process] {
			if (process->state() != QProcess::NotRunning) process->kill();
		});
	}
	FinishStop();
}
void StreamingAudio::FinishStop() {
	if (!m_stopping || (m_private_policy && m_private_policy->state() != QProcess::NotRunning) ||
	    (m_private_server && m_private_server->state() != QProcess::NotRunning))
		return;
	if (m_private_policy) {
		m_private_policy->deleteLater();
		m_private_policy = nullptr;
	}
	if (m_private_server) {
		m_private_server->deleteLater();
		m_private_server = nullptr;
	}
	m_private_directory.reset();
	m_server.clear();
	m_sink.clear();
	m_device.clear();
	m_isolated    = false;
	m_unloading   = false;
	m_environment = QProcessEnvironment::systemEnvironment();
	emit Stopped();
}
