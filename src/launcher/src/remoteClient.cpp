#include "remoteClient.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTimer>
#include <QUuid>

#include <memory>
#ifdef Q_OS_UNIX
#include <signal.h>
#include <sys/stat.h>
#ifdef Q_OS_LINUX
#include <sys/prctl.h>
#endif
#include <unistd.h>
#endif
namespace {
constexpr qint64 MaxReply    = 1024 * 1024;
constexpr qint64 MaxSettings = 32 * MaxReply;
bool             Text(const QString& text, int max) {
	if (text.isEmpty() || text.size() > max) return false;
	for (auto ch: text)
		if (ch.category() == QChar::Other_Control) return false;
	return true;
}
bool Endpoint(const QString& endpoint) {
	const int colon = endpoint.lastIndexOf(':');
	if (colon < 1) return false;
	QString ip = endpoint.left(colon);
	if (ip.startsWith('[') && ip.endsWith(']')) ip = ip.mid(1, ip.size() - 2);
	QHostAddress address(ip);
	bool         ok   = false;
	int          port = endpoint.mid(colon + 1).toInt(&ok);
	return ok && port > 0 && port <= 65535 && !address.isNull() && address != QHostAddress::Any &&
	       address != QHostAddress::AnyIPv4 && address != QHostAddress::AnyIPv6;
}
bool PrivatePath(const QString& path, bool directory, bool secret, qint64 limit = MaxReply) {
	QFileInfo info(path);
	if (!info.isAbsolute() || info.isSymLink() || (directory ? !info.isDir() : !info.isFile()))
		return false;
	for (QString p = info.absoluteFilePath(); p != "/";) {
		QFileInfo component(p);
		if (component.isSymLink()) return false;
		p = component.absolutePath();
	}
#ifdef Q_OS_UNIX
	struct stat st {};
	if (::lstat(QFile::encodeName(path).constData(), &st) != 0 || st.st_uid != ::geteuid())
		return false;
	if (secret && (st.st_mode & 0077)) return false;
#endif
	return directory || (info.size() > 0 && info.size() <= limit);
}
bool ProfileValid(const QString& path) {
	return PrivatePath(path, true, true) &&
	       PrivatePath(QDir(path).filePath("client.json"), false, true) &&
	       PrivatePath(QDir(path).filePath("host.der"), false, false);
}
bool Games(const QJsonArray& array) {
	if (array.size() > 10000) return false;
	QSet<QString> ids;
	for (const auto value: array) {
		const auto game = value.toObject();
		auto       id   = game.value("id").toString();
		if (!Text(id, 512) || !Text(game.value("name").toString(), 512) || ids.contains(id))
			return false;
		ids.insert(id);
	}
	return true;
}
QString Composite(const QString& profile, const QString& game) {
	return "remote:" + profile + ":" + game;
}
bool Bundle(const QByteArray& data, QString* fingerprint, QString* endpoint) {
	const auto object = QJsonDocument::fromJson(data).object();
	const auto cert   = object.value("certificate_der").toArray();
	if (object.value("version").toInt() != 1 || cert.isEmpty() || cert.size() > 16384) return false;
	QByteArray bytes;
	for (const auto value: cert) {
		if (!value.isDouble() || value.toDouble() != value.toInt(-1) || value.toInt(-1) < 0 ||
		    value.toInt() > 255)
			return false;
		bytes.append(static_cast<char>(value.toInt()));
	}
	*fingerprint =
	    QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
	*endpoint = object.value("endpoint").toString();
	return true;
}
} // namespace
QString RemoteClient::Executable() {
	const QDir dir(QCoreApplication::applicationDirPath());
	for (const auto& relative: {"libexec/ags-client", "ags-client", "../libexec/ags-client"}) {
		QFileInfo file(dir.filePath(relative));
		if (file.isFile() && file.isExecutable()) return file.absoluteFilePath();
	}
	return dir.filePath("libexec/ags-client"); // No PATH or environment override.
}
QString RemoteClient::DefaultSettingsFile() {
	return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
	    .filePath("ags/remote-hosts.json");
}
RemoteClient::RemoteClient(QObject* parent)
    : RemoteClient(TestOptions {Executable(), DefaultSettingsFile(), 15000, "/sys/class/input",
	                            "/dev/input"},
	               parent) {}
RemoteClient::RemoteClient(const TestOptions& options, QObject* parent)
    : QObject(parent), m_executable(options.executable), m_settingsFile(options.settingsFile),
      m_inputSysRoot(options.inputSysRoot), m_inputDeviceRoot(options.inputDeviceRoot),
      m_timeoutMs(options.timeoutMs) {
	Load();
#ifdef Q_OS_LINUX
	const pid_t parentPid = ::getpid();
	m_receiver.setChildProcessModifier([parentPid] {
		// Only async-signal-safe syscalls between fork and exec. Parent death ends
		// this owned receiver even if the GUI cannot perform normal authenticated Close.
		if (::prctl(PR_SET_PDEATHSIG, SIGINT) != 0 || ::getppid() != parentPid) ::_exit(125);
	});
#endif
	const auto drain = [this](const QByteArray& bytes) {
		m_receiverOutputBytes = qMin(MaxReply + 1, m_receiverOutputBytes + bytes.size());
		if (m_receiverOutputBytes > MaxReply && !m_stopping) {
			emit Error(tr("Receiver output exceeded its safety limit. Closing the remote game."));
			StopForExit();
		}
	};
	connect(&m_receiver, &QProcess::readyReadStandardOutput, this,
	        [this, drain] { drain(m_receiver.readAllStandardOutput()); });
	connect(&m_receiver, &QProcess::readyReadStandardError, this,
	        [this, drain] { drain(m_receiver.readAllStandardError()); });
	connect(&m_receiver, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
		if (error == QProcess::FailedToStart) {
			m_ownedProfile.clear();
			emit Error(tr("Cannot start the packaged AGS receiver. Check that libexec/ags-client "
			              "is installed and executable."));
			emit Changed();
			if (m_stopping) FinishStop();
		}
	});
	connect(&m_receiver, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
		if (m_stopping) {
			FinishStop();
			return;
		}
		if (code == 0 && status == QProcess::NormalExit)
			m_ownedProfile.clear();
		else
			emit Error(tr("Receiver ended unexpectedly. Use Stop remote game to confirm host "
			              "cleanup. If unreachable, the host disconnect grace policy applies."));
		emit Changed();
	});
}
RemoteClient::~RemoteClient() {
	// Normal GUI exit must await StopForExit; this is only a final owned-process safety net.
	for (auto* process: findChildren<QProcess*>()) {
		process->disconnect(this);
		process->kill();
		process->waitForFinished(1000);
	}
	m_receiver.disconnect(this);
	if (m_receiver.state() != QProcess::NotRunning) {
#ifdef Q_OS_UNIX
		if (m_receiver.processId() > 0) ::kill(static_cast<pid_t>(m_receiver.processId()), SIGINT);
#endif
		if (!m_receiver.waitForFinished(2000)) {
			m_receiver.kill();
			m_receiver.waitForFinished(1000);
		}
	}
}
QStringList RemoteClient::Arguments(const Profile& p) const {
	return {p.endpoint, QDir(p.directory).filePath("host.der"),
	        QDir(p.directory).filePath("client.json")};
}
QProcess* RemoteClient::Request(const QStringList& args, Completion complete) {
	auto* process = new QProcess(this);
	auto* timer   = new QTimer(process);
	timer->setSingleShot(true);
	struct State {
		QByteArray output;
		qint64     bytes = 0;
		QString    error;
		bool       done = false;
	};
	auto state  = std::make_shared<State>();
	auto finish = [process, timer, state, complete](int code, QProcess::ExitStatus status) {
		if (state->done) return;
		state->done = true;
		timer->stop();
		if (state->error.isEmpty() && (code != 0 || status != QProcess::NormalExit))
			state->error = QObject::tr("Native client request failed. Check the host address, "
			                           "trust profile, and host availability.");
		complete(state->output, state->error);
		process->deleteLater();
	};
	connect(process, &QProcess::readyReadStandardOutput, this, [process, state] {
		const auto data = process->readAllStandardOutput();
		state->bytes += data.size();
		if (state->bytes <= MaxReply)
			state->output += data;
		else {
			state->error = QObject::tr("Native client reply exceeded the size limit.");
			process->kill();
		}
	});
	connect(process, &QProcess::readyReadStandardError, this, [process, state] {
		state->bytes += process->readAllStandardError()
		                    .size(); // Never display raw stderr or credential-bearing replies.
		if (state->bytes > MaxReply) {
			state->error = QObject::tr("Native client output exceeded the size limit.");
			process->kill();
		}
	});
	connect(timer, &QTimer::timeout, this, [process, state] {
		state->error = QObject::tr("Host request timed out. Check the connection and try again.");
		process->kill();
	});
	connect(process, &QProcess::finished, this, finish);
	connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
		if (error == QProcess::FailedToStart) finish(-1, QProcess::CrashExit);
	});
	process->setProgram(m_executable);
	process->setArguments(args);
	timer->start(m_timeoutMs);
	process->start();
	return process;
}
QString RemoteClient::AddProfile(const QString& name, const QString& endpoint,
                                 const QString& directory) {
	const QString path = QDir::cleanPath(QFileInfo(directory).absoluteFilePath());
	if (m_stopping || m_profiles.size() >= 32 || !Text(name, 64) || !Endpoint(endpoint) ||
	    !ProfileValid(path)) {
		emit Error(tr("Enter a host name, numeric address with port, and an owner-private paired "
		              "profile directory containing host.der and client.json."));
		return {};
	}
	for (const auto& p: m_profiles)
		if (p.directory == path) {
			emit Error(tr("This profile is already configured."));
			return {};
		}
	Profile p;
	p.id        = QUuid::createUuid().toString(QUuid::WithoutBraces);
	p.name      = name;
	p.endpoint  = endpoint;
	p.directory = path;
	m_profiles.insert(p.id, p);
	Save();
	emit Changed();
	Refresh(p.id);
	return p.id;
}
bool RemoteClient::PairingInfo(const QString& path, QString* fingerprint, QString* endpoint,
                               QString* error) {
	QFile file(path);
	if (!PrivatePath(path, false, true) || !file.open(QIODevice::ReadOnly) ||
	    !Bundle(file.read(MaxReply + 1), fingerprint, endpoint)) {
		if (error)
			*error = tr("Choose a valid owner-only pairing bundle. The native client validates its "
			            "token, expiry and pinned certificate when pairing.");
		return false;
	}
	return true;
}
void RemoteClient::Pair(const QString& path, const QString& fingerprint, const QString& name,
                        const QString& endpoint, const QString& directory,
                        const QString& receiverName) {
	if (m_pair || m_stopping) {
		emit Error(tr("A connection operation is already in progress."));
		return;
	}
	QString actual, bundleEndpoint, error;
	QFile   source(path);
	if (!PrivatePath(path, false, true) || !source.open(QIODevice::ReadOnly)) {
		emit Error(tr("Cannot read private pairing bundle."));
		return;
	}
	const auto bytes = source.read(MaxReply + 1);
	if (!Bundle(bytes, &actual, &bundleEndpoint) || actual != fingerprint || !Text(name, 64) ||
	    !Text(receiverName, 64) || !Endpoint(endpoint)) {
		emit Error(tr("Pairing details changed or are invalid. Review the certificate fingerprint "
		              "and host address again."));
		return;
	}
	// Bind the confirmed certificate to exactly the bytes consumed by the CLI.
	auto snapshot = std::make_shared<QTemporaryFile>();
	if (!snapshot->open() || !snapshot->setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
	    snapshot->write(bytes) != bytes.size() || !snapshot->flush()) {
		emit Error(tr("Cannot create private pairing snapshot."));
		return;
	}
	const QString output   = QDir::cleanPath(QFileInfo(directory).absoluteFilePath());
	const auto    revision = ++m_pairRevision;
	m_pair                 = Request(
	    {"pair", snapshot->fileName(), receiverName, output, "--address", endpoint},
	    [this, snapshot, name, endpoint, output, revision](QByteArray, QString failure) {
		    if (revision != m_pairRevision) return;
		    m_pair = nullptr;
		    if (m_stopping) return;
		    if (!failure.isEmpty()) {
			    emit Error(
			        tr("Pairing failed. The ticket may be expired or consumed; create a new bundle "
					   "on the host. Partial profile files, if any, were retained for recovery."));
			    return;
		    }
		    AddProfile(name, endpoint, output);
	    });
}
void RemoteClient::Refresh(const QString& id) {
	if (!m_profiles.contains(id) || m_stopping) return;
	auto&      p        = m_profiles[id];
	const auto revision = ++p.revision;
	if (m_requests.value(id)) m_requests[id]->kill();
	if (!ProfileValid(p.directory)) {
		p.online = false;
		emit Changed();
		emit Error(tr("The private profile is unavailable or its permissions changed."));
		return;
	}
	m_requests[id] = Request(
	    Arguments(p) + QStringList {"list"}, [this, id, revision](QByteArray data, QString error) {
		    if (!m_profiles.contains(id) || m_profiles[id].revision != revision || m_stopping)
			    return;
		    m_requests.remove(id);
		    auto&      profile = m_profiles[id];
		    const auto reply   = QJsonDocument::fromJson(data).object();
		    if (error.isEmpty() &&
			    (!reply.value("ok").isBool() || !reply.value("ok").toBool() ||
			     !reply.value("games").isArray() || !Games(reply.value("games").toArray())))
			    error = tr("Host returned an invalid library response.");
		    profile.online = error.isEmpty();
		    if (profile.online) {
			    profile.games = {};
			    for (const auto value: reply.value("games").toArray()) {
				    const auto game = value.toObject();
				    profile.games.append(
				        QJsonObject {{"id", game.value("id")}, {"name", game.value("name")}});
			    }
			    Save();
		    }
		    emit Changed();
		    if (!error.isEmpty()) emit Error(profile.name + ": " + error);
	    });
}
void RemoteClient::RefreshAll() {
	for (const auto& id: m_profiles.keys())
		Refresh(id);
}
bool RemoteClient::Forget(const QString& id) {
	if (id == m_ownedProfile || m_stopping || !m_profiles.contains(id)) return false;
	m_profiles.remove(id);
	if (m_requests.value(id)) m_requests.take(id)->kill();
	Save();
	emit Changed();
	return true; // Credentials are deliberately never deleted here.
}
QList<LibraryCard> RemoteClient::Cards() const {
	QList<LibraryCard> result;
	for (const auto& p: m_profiles)
		for (const auto value: p.games) {
			const auto  game = value.toObject();
			LibraryCard card;
			card.id       = Composite(p.id, game.value("id").toString());
			card.name     = game.value("name").toString();
			card.source   = "remote";
			card.sourceId = p.name;
			card.ready    = p.online;
			card.reason =
			    p.online ? tr("Stream from %1. Game source is reported by the host as unknown.")
			                   .arg(p.name)
				         : tr("Host offline. Refresh the connection to try again.");
			result.append(card);
		}
	return result;
}
bool RemoteClient::SetQuality(int width, int height, int fps) {
	if (Busy() || width < 64 || width > 8192 || height < 64 || height > 8192 || fps < 1 ||
	    fps > 240)
		return false;
	m_width  = width;
	m_height = height;
	m_fps    = fps;
	return true;
}
QList<RemoteClient::Controller> RemoteClient::Controllers() const {
	QList<Controller> result;
	if (m_inputSysRoot.isEmpty() || m_inputDeviceRoot.isEmpty()) return result;
	auto read = [](const QString& path, qint64 limit) {
		QFile file(path);
		if (!file.open(QIODevice::ReadOnly)) return QByteArray {};
		const auto bytes = file.read(limit + 1);
		return bytes.size() <= limit ? bytes.trimmed() : QByteArray {};
	};
	auto bit = [](const QByteArray& bitmap, unsigned index) {
		// Linux sysfs prints native unsigned-long words high-to-low, without padding.
		const auto         words    = bitmap.simplified().split(' ');
		constexpr unsigned wordBits = sizeof(unsigned long) * 8;
		const int          word     = words.size() - 1 - int(index / wordBits);
		if (word < 0 || bitmap.isEmpty()) return false;
		bool       valid = false;
		const auto value = words[word].toULongLong(&valid, 16);
		return valid && (value & (quint64(1) << (index % wordBits))) != 0;
	};
	const auto entries = QDir(m_inputSysRoot).entryList({"event*"}, QDir::Dirs, QDir::Name);
	for (const auto& event: entries.mid(0, 256)) {
		if (!QRegularExpression("^event[0-9]{1,6}$").match(event).hasMatch()) continue;
		const auto base = QDir(m_inputSysRoot).filePath(event + "/device/");
		const auto keys = read(base + "capabilities/key", 4096);
		const auto axes = read(base + "capabilities/abs", 4096);
		if (!bit(keys, 0x130) || !bit(axes, 0) || !bit(axes, 1)) continue;
		const auto      path = QDir(m_inputDeviceRoot).filePath(event);
		const QFileInfo node(path);
		if (!node.exists() || node.isSymLink() || !node.isReadable()) continue;
		auto name = QString::fromUtf8(read(base + "name", 256));
		if (!Text(name, 256)) name = tr("Gamepad");
		result.append({name, path});
	}
	return result;
}
bool RemoteClient::SetController(const QString& path) {
	if (Busy()) return false;
	if (path.isEmpty() || path == "@auto") {
		m_controllerSelection = path;
		return true;
	}
	for (const auto& controller: Controllers())
		if (controller.path == path) {
			m_controllerSelection = path;
			return true;
		}
	emit Error(tr("Selected gamepad is unavailable or unreadable. Refresh the gamepad list and "
	              "choose an available controller."));
	return false;
}
bool RemoteClient::Play(const QString& id) {
	if (Busy()) return false;
	for (const auto& p: m_profiles)
		for (const auto value: p.games) {
			const auto game   = value.toObject();
			const auto gameId = game.value("id").toString();
			if (Composite(p.id, gameId) != id) continue;
			if (!p.online || !ProfileValid(p.directory)) {
				emit Error(tr("Remote host is offline or its private profile is unavailable. "
				              "Refresh the connection before Play."));
				return false;
			}
			QString input;
			if (!m_controllerSelection.isEmpty()) {
				const auto controllers = Controllers();
				if (m_controllerSelection == "@auto") {
					if (!controllers.isEmpty()) input = controllers.first().path;
				} else {
					for (const auto& controller: controllers)
						if (controller.path == m_controllerSelection) input = controller.path;
					if (input.isEmpty()) {
						emit Error(tr("Selected gamepad disconnected. Refresh the gamepad list "
						              "before Play."));
						return false;
					}
				}
			}
			m_receiverOutputBytes = 0;
			m_ownedProfile        = p.id;
			m_receiver.setProgram(m_executable);
			m_receiver.setArguments(
			    Arguments(p) + QStringList {"launch", gameId, "--play", "--close-on-exit",
				                            "--codec", "h264", "--width", QString::number(m_width),
				                            "--height", QString::number(m_height), "--fps",
				                            QString::number(m_fps)});
			if (!input.isEmpty())
				m_receiver.setArguments(m_receiver.arguments() + QStringList {"--input", input});
			m_receiver.start();
			emit Changed();
			return true;
		}
	return false;
}
void RemoteClient::StopForExit() {
	if (m_stopping) return;
	m_stopping = true;
	m_stopError.clear();
	m_closeFinished     = false;
	const auto revision = ++m_stopRevision;
	for (auto& p: m_profiles)
		++p.revision;
	for (auto process: m_requests)
		if (process) process->kill();
	m_requests.clear();
	++m_pairRevision;
	if (m_pair) {
		m_pair->kill();
		m_pair = nullptr;
	}
	if (m_ownedProfile.isEmpty()) {
		m_closeFinished = true;
		FinishStop();
		return;
	}
	const auto p = m_profiles.value(m_ownedProfile);
	Request(Arguments(p) + QStringList {"close"}, [this, revision](QByteArray data, QString error) {
		if (revision != m_stopRevision) return;
		if (error.isEmpty() && !QJsonDocument::fromJson(data).object().value("ok").toBool())
			error = tr("Host did not confirm closing the remote game.");
		if (!error.isEmpty())
			m_stopError =
			    error + tr(" Remote game closure is unconfirmed; host disconnect grace is the "
				           "fallback. Retry Stop remote game when the host is reachable.");
		m_closeFinished = true;
		if (m_receiver.state() != QProcess::NotRunning) {
#ifdef Q_OS_UNIX
			if (m_receiver.processId() > 0)
				::kill(static_cast<pid_t>(m_receiver.processId()), SIGINT);
#else
            m_receiver.terminate();
#endif
			QTimer::singleShot(20000, this, [this, revision] {
				if (!m_stopping || revision != m_stopRevision ||
				    m_receiver.state() == QProcess::NotRunning)
					return;
				m_stopError += tr(" Receiver did not drain in time and was forcibly stopped.");
				m_receiver.kill();
			});
		}
		FinishStop();
	});
	emit Changed();
}
void RemoteClient::FinishStop() {
	if (!m_stopping || !m_closeFinished || m_receiver.state() != QProcess::NotRunning) return;
	if (m_stopError.isEmpty()) m_ownedProfile.clear();
	m_stopping = false;
	emit Changed();
	emit Stopped(m_stopError);
}
void RemoteClient::Load() {
	if (!PrivatePath(m_settingsFile, false, true, MaxSettings)) return;
	QFile file(m_settingsFile);
	if (!file.open(QIODevice::ReadOnly)) return;
	const auto array = QJsonDocument::fromJson(file.read(MaxSettings + 1)).array();
	if (array.size() > 32) return;
	QSet<QString> paths;
	for (const auto value: array) {
		const auto object = value.toObject();
		Profile    p;
		p.id        = object.value("id").toString();
		p.name      = object.value("name").toString();
		p.endpoint  = object.value("endpoint").toString();
		p.directory = object.value("directory").toString();
		p.games     = object.value("games").toArray();
		if (QUuid(p.id).isNull() || !Text(p.name, 64) || !Endpoint(p.endpoint) ||
		    !QFileInfo(p.directory).isAbsolute() || !Games(p.games) || paths.contains(p.directory))
			continue;
		paths.insert(p.directory);
		m_profiles.insert(p.id, p);
	}
}
void RemoteClient::Save() {
	const auto directory = QFileInfo(m_settingsFile).absolutePath();
	if (!QDir().mkpath(directory) || !PrivatePath(directory, true, false)) {
		emit Error(tr("Cannot save remote host settings."));
		return;
	}
	QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
	if (!PrivatePath(directory, true, true)) {
		emit Error(tr("Remote host settings directory must be owner-private."));
		return;
	}
	QJsonArray array;
	for (const auto& p: m_profiles)
		array.append(QJsonObject {{"id", p.id},
		                          {"name", p.name},
		                          {"endpoint", p.endpoint},
		                          {"directory", p.directory},
		                          {"games", p.games}});
	if (QFileInfo(m_settingsFile).isSymLink() ||
	    (QFileInfo(m_settingsFile).exists() &&
	     !PrivatePath(m_settingsFile, false, true, MaxSettings))) {
		emit Error(tr("Remote settings must be an owner-private regular file."));
		return;
	}
	QSaveFile  file(m_settingsFile);
	const auto bytes = QJsonDocument(array).toJson();
	if (bytes.size() > MaxSettings) {
		emit Error(tr("Remote library cache exceeds its storage limit."));
		return;
	}
	if (!file.open(QIODevice::WriteOnly) ||
	    !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
	    file.write(bytes) != bytes.size() || !file.commit())
		emit Error(tr("Cannot save remote host settings."));
}
