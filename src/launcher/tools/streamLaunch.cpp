#include "../include/streamingProcess.h"
#include "../include/streamingProfile.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSocketNotifier>
#include <QTextStream>
#include <QTimer>

#include <chrono>
#include <csignal>
#ifdef __linux__
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
volatile std::sig_atomic_t stop_requested = 0;
void                       OnSignal(int) {
	stop_requested = 1;
}
#ifdef __linux__
volatile std::sig_atomic_t group_stop_observed = 0;
void                       OnGuardianSignal(int signal) {
	if (signal != SIGUSR1) group_stop_observed = 1;
	stop_requested = 1;
}
int GuardianMain(int argc, char** argv) {
	if (argc != 7) return 2;
	const int group = std::atoi(argv[2]);
	const int host  = std::atoi(argv[3]);
	const int owner = std::atoi(argv[4]);
	const int ready = std::atoi(argv[5]);
	if (group < 2 || group != ::getpgrp() || group == ::getpid()) return 2;
	// Serialize the handlers, and keep group-shutdown observation monotonic:
	// a later private request must never erase an already delivered group stop.
	sigset_t shutdown_signals;
	::sigemptyset(&shutdown_signals);
	for (const int signal: {SIGTERM, SIGINT, SIGUSR1})
		::sigaddset(&shutdown_signals, signal);
	struct sigaction action {};
	action.sa_handler = OnGuardianSignal;
	action.sa_mask    = shutdown_signals;
	for (const int signal: {SIGTERM, SIGINT, SIGUSR1})
		if (::sigaction(signal, &action, nullptr) != 0) return 2;
	if (::write(ready, "R", 1) != 1) return 2;
	::close(ready);
	while (!stop_requested && !StreamingProcess::Exited(host) && !StreamingProcess::Exited(owner)) {
		pollfd watched[2] {{host, POLLIN, 0}, {owner, POLLIN, 0}};
		::poll(watched, 2, 100);
	}
	// This live guardian reserves the PGID until normal completion or one
	// atomic group SIGKILL. It must exclude itself from the completion test.
	// Hold signal delivery across the check/send decision, including signals
	// queued before blocking. No handler can change the observation mid-decision.
	// Independent external group senders cannot share this critical section;
	// a new external send after sigpending can still race with our broadcast.
	sigset_t previous_mask;
	if (::sigprocmask(SIG_BLOCK, &shutdown_signals, &previous_mask) != 0) return 2;
	sigset_t pending;
	if (::sigpending(&pending) != 0) return 2;
	if (!group_stop_observed && !::sigismember(&pending, SIGTERM) &&
	    !::sigismember(&pending, SIGINT))
		::kill(-group, SIGTERM);
	if (::sigprocmask(SIG_SETMASK, &previous_mask, nullptr) != 0) return 2;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
	while (StreamingProcess::GroupAlive(group, int(::getpid()))) {
		if (std::chrono::steady_clock::now() >= deadline) {
			::kill(-group, SIGKILL);
			return 1;
		}
		::poll(nullptr, 0, 50);
	}
	::unlink(argv[6]);
	return 0;
}
#endif
} // namespace

int main(int argc, char** argv) {
#ifdef __linux__
	if (argc > 1 && std::strcmp(argv[1], "--guardian") == 0) return GuardianMain(argc, argv);
#endif
	QCoreApplication app(argc, argv);
	const auto       fail = [](const QString& text) {
		QTextStream(stderr) << "Kyty streaming: " << text << '\n';
		return 1;
	};
	if (argc != 3) return fail("Expected a private manifest and catalog game ID.");
#ifdef __linux__
	const auto  manifest_path = QFile::encodeName(app.arguments()[1]);
	const int   fd = ::open(manifest_path.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	struct stat info {};
	if (fd < 0) return fail("Cannot open the launch manifest.");
	if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != ::getuid() ||
	    (info.st_mode & 0077) != 0) {
		::close(fd);
		return fail("Launch manifest must be an owner-only regular file.");
	}
	QFile file;
	if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle) ||
	    file.size() > 4 * 1024 * 1024)
		return fail("Invalid launch manifest.");
	const auto manifest = QJsonDocument::fromJson(file.readAll()).object();
	if (manifest.value("version").toInt() != 1) return fail("Unsupported manifest version.");
	// Prefer this device's own catalog entry (client_games[uuid][game]); fall back to the shared
	// PC entry (games[game]) exactly as before. The id is validated before it indexes the manifest.
	const auto                      game_id     = app.arguments()[2];
	const auto                      client_uuid = qEnvironmentVariable("SUNSHINE_CLIENT_UUID");
	static const QRegularExpression uuid_pattern(QStringLiteral(
	    "\\A[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\\z"));
	QJsonObject                     game;
	if (!client_uuid.isEmpty() && uuid_pattern.match(client_uuid).hasMatch()) {
		game = manifest.value("client_games")
		           .toObject()
		           .value(client_uuid)
		           .toObject()
		           .value(game_id)
		           .toObject();
	}
	if (game.isEmpty()) {
		game = manifest.value("games").toObject().value(game_id).toObject();
	}
	const auto program = game.value("program").toString();
	if (game.isEmpty() || !QFileInfo(program).isAbsolute() || !QFileInfo(program).isExecutable())
		return fail("Catalog entry is missing or its emulator is unavailable.");
	const auto nonce      = manifest.value("host_nonce").toString();
	const auto owner_path = QFile::encodeName(manifest.value("host_owner_file").toString());
	if (nonce.size() != 32 || qEnvironmentVariable("KYTY_STREAM_HOST_NONCE") != nonce ||
	    owner_path.isEmpty())
		return fail("Managed host ownership is missing.");
	int host_pidfd = -1;
	for (int attempt = 0; attempt < 100 && host_pidfd < 0; ++attempt) {
		const int owner_file = ::open(owner_path.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
		if (owner_file >= 0) {
			QFile       owner;
			struct stat owner_info {};
			if (::fstat(owner_file, &owner_info) != 0 || !S_ISREG(owner_info.st_mode) ||
			    owner_info.st_uid != ::getuid() || (owner_info.st_mode & 0077) != 0 ||
			    owner_info.st_size > 8192 ||
			    !owner.open(owner_file, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
				::close(owner_file);
				return fail("Invalid private host ownership file.");
			}
			const auto                 data = QJsonDocument::fromJson(owner.readAll()).object();
			StreamingProcess::Identity identity;
			bool                       parsed = false;
			const auto start     = data.value("start_time").toString().toULongLong(&parsed);
			const auto owner_pid = data.value("pid").toInteger();
			if (data.value("nonce").toString() == nonce && parsed && owner_pid > 1 &&
			    owner_pid <= std::numeric_limits<int>::max() &&
			    StreamingProcess::ReadIdentity(int(owner_pid), &identity) &&
			    identity.start == start &&
			    identity.executable ==
			        data.value("executable").toString().toLocal8Bit().toStdString())
				host_pidfd = StreamingProcess::Open(identity);
			if (data.value("nonce").toString() == nonce && host_pidfd < 0)
				return fail("The managed host has already exited or changed identity.");
		}
		if (host_pidfd < 0) ::poll(nullptr, 0, 10);
	}
	if (host_pidfd < 0) return fail("Managed host ownership was not published in time.");
	if (::getpgrp() != ::getpid() && ::setpgid(0, 0) != 0) {
		::close(host_pidfd);
		return fail("Cannot isolate the streaming game process group.");
	}
	const auto lock_path = QFile::encodeName(manifest.value("lock_file").toString());
	const int  lock      = ::open(lock_path.constData(), O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
	if (lock < 0 || ::fstat(lock, &info) != 0 || info.st_uid != ::getuid() ||
	    !S_ISREG(info.st_mode) || (info.st_mode & 0077) != 0 ||
	    ::flock(lock, LOCK_EX | LOCK_NB) != 0)
		return fail("Another Kyty streaming game is already running, or its lock is unavailable.");
	const auto        env = manifest.value("environment").toObject();
	const QStringList allowed {
	    "KYTY_STREAM_SOCKET", "PULSE_SERVER",       "SDL_AUDIO_DRIVER", "KYTY_AUDIO_OUTPUT_DEVICE",
	    "KYTY_STREAM_WIDTH",  "KYTY_STREAM_HEIGHT", "KYTY_STREAM_FPS",  "KYTY_STREAM_ONLY"};
	for (const auto& key: allowed)
		qunsetenv(key.toLatin1().constData());
	for (auto it = env.begin(); it != env.end(); ++it) {
		if (!allowed.contains(it.key()) || !it.value().isString() ||
		    it.value().toString().contains(QChar::Null))
			return fail("Invalid launch environment.");
		qputenv(it.key().toLatin1().constData(), it.value().toString().toUtf8());
	}
	if (env.contains("KYTY_STREAM_ONLY") && env.value("KYTY_STREAM_ONLY") != "1")
		return fail("Invalid launch environment.");
	if (env.value("KYTY_AUDIO_OUTPUT_DEVICE").toString().isEmpty() ||
	    env.value("KYTY_STREAM_SOCKET").toString().isEmpty())
		return fail("Private capture/audio route is missing.");
	const auto directory = QFile::encodeName(game.value("working_directory").toString());
	if (::chdir(directory.constData()) != 0) return fail("Game working directory is unavailable.");
	QStringList arguments;
	for (const auto& arg: game.value("arguments").toArray()) {
		if (!arg.isString() || arg.toString().contains(QChar::Null))
			return fail("Invalid emulator argument.");
		arguments.append(arg.toString());
	}
	auto    child_environment = QProcessEnvironment::systemEnvironment();
	QString profile_error;
	if (!StreamingProfile::Apply(manifest.value("unified_stream_profile").toBool(),
	                             QProcessEnvironment::systemEnvironment(), &arguments,
	                             &child_environment, &profile_error))
		return fail(profile_error);
	const auto                 session_path = manifest.value("session_file").toString();
	StreamingProcess::Identity self;
	if (!StreamingProcess::ReadIdentity(int(::getpid()), &self))
		return fail("Cannot verify game supervisor identity.");
	const int self_pidfd = StreamingProcess::Open(self);
	if (self_pidfd < 0) return fail("Linux pidfd supervision is unavailable.");
	int handshake[2];
	if (::pipe2(handshake, O_CLOEXEC) != 0)
		return fail("Cannot create guardian startup handshake.");
	const auto executable  = QFile::encodeName(QCoreApplication::applicationFilePath());
	const auto group_arg   = QByteArray::number(::getpgrp());
	const auto host_arg    = QByteArray::number(host_pidfd);
	const auto self_arg    = QByteArray::number(self_pidfd);
	const auto ready_arg   = QByteArray::number(handshake[1]);
	const auto session_arg = QFile::encodeName(session_path);
	std::signal(SIGTERM, OnSignal);
	std::signal(SIGINT, OnSignal);
	const char* native_exe     = executable.constData();
	const char* native_group   = group_arg.constData();
	const char* native_host    = host_arg.constData();
	const char* native_self    = self_arg.constData();
	const char* native_ready   = ready_arg.constData();
	const char* native_session = session_arg.constData();
	// The guardian intentionally inherits this stream-lock FD until the entire
	// owned game group is gone, even if its original wrapper dies first.
	const pid_t guardian = ::fork();
	if (guardian == 0) {
		// Only async-signal-safe operations between fork and exec.
		::close(handshake[0]);
		::fcntl(host_pidfd, F_SETFD, 0);
		::fcntl(self_pidfd, F_SETFD, 0);
		::fcntl(handshake[1], F_SETFD, 0);
		::execl(native_exe, native_exe, "--guardian", native_group, native_host, native_self,
		        native_ready, native_session, static_cast<char*>(nullptr));
		::_exit(127);
	}
	::close(handshake[1]);
	::close(self_pidfd);
	pollfd     ready {handshake[0], POLLIN, 0};
	char       acknowledgement = 0;
	const bool armed = guardian > 0 && ::poll(&ready, 1, 3000) > 0 &&
	                   ::read(handshake[0], &acknowledgement, 1) == 1 && acknowledgement == 'R';
	::close(handshake[0]);
	if (!armed) return fail("Cannot arm the independent game guardian.");
	StreamingProcess::Identity guardian_identity;
	if (!StreamingProcess::ReadIdentity(int(guardian), &guardian_identity))
		return fail("Game guardian exited before launch.");
	const int guardian_pidfd = StreamingProcess::Open(guardian_identity);
	if (guardian_pidfd < 0) return fail("Cannot retain game guardian identity.");
	QJsonObject session {
	    {"pid", qint64(::getpid())},
	    {"pgid", qint64(::getpgrp())},
	    {"start_time", QString::number(qulonglong(self.start))},
	    {"guardian_pid", qint64(guardian)},
	    {"guardian_start_time", QString::number(qulonglong(guardian_identity.start))},
	    {"host_nonce", nonce},
	    {"game", app.arguments()[2]},
	    {"phase", "starting"},
	    {"profile", manifest.value("unified_stream_profile").toBool() ? "streaming" : "legacy"},
	    {"effective_width", child_environment.value("KYTY_STREAM_WIDTH")},
	    {"effective_height", child_environment.value("KYTY_STREAM_HEIGHT")},
	    {"effective_fps", child_environment.value("KYTY_STREAM_FPS")},
	    {"width", qEnvironmentVariable("SUNSHINE_CLIENT_WIDTH")},
	    {"height", qEnvironmentVariable("SUNSHINE_CLIENT_HEIGHT")},
	    {"fps", qEnvironmentVariable("SUNSHINE_CLIENT_FPS")},
	    {"client", qEnvironmentVariable("SUNSHINE_CLIENT_NAME")}};
	const auto publish = [&] {
		QSaveFile status(session_path);
		return status.open(QIODevice::WriteOnly) &&
		       status.setPermissions(QFile::ReadOwner | QFile::WriteOwner) &&
		       status.write(QJsonDocument(session).toJson()) >= 0 && status.commit();
	};
	if (!publish()) return fail("Cannot publish private game ownership before launch.");
	if (manifest.value("unified_stream_profile").toBool()) {
		QTextStream(stderr) << "Kyty streaming profile: game=" << app.arguments()[2]
		                    << " requested=" << session.value("width").toString() << 'x'
		                    << session.value("height").toString() << '@'
		                    << session.value("fps").toString()
		                    << " effective=" << child_environment.value("KYTY_STREAM_WIDTH") << 'x'
		                    << child_environment.value("KYTY_STREAM_HEIGHT") << '@'
		                    << child_environment.value("KYTY_STREAM_FPS")
		                    << " headless=1 local_profile=unchanged\n";
	}

	if (StreamingProcess::Exited(host_pidfd) || stop_requested)
		return fail("Managed host exited before game launch.");
	QProcess child;
	child.setProcessEnvironment(child_environment);
	child.setProcessChannelMode(QProcess::ForwardedChannels);
	QObject::connect(&child, &QProcess::started, &app, [&] {
		session.insert("phase", "running");
		publish();
	});
	QSocketNotifier host_death(host_pidfd, QSocketNotifier::Read, &app);
	QObject::connect(&host_death, &QSocketNotifier::activated, &app, [&] {
		host_death.setEnabled(false);
		stop_requested = 1;
	});
	QObject::connect(&child, &QProcess::finished, &app, [&](int code, QProcess::ExitStatus status) {
		app.exit(status == QProcess::NormalExit ? code : 1);
	});
	QObject::connect(&child, &QProcess::errorOccurred, &app, [&](QProcess::ProcessError error) {
		if (error == QProcess::FailedToStart) {
			fail(child.errorString());
			app.exit(1);
		}
	});
	bool   stopping = false;
	QTimer signal_timer;
	signal_timer.setInterval(100);
	QObject::connect(&signal_timer, &QTimer::timeout, &app, [&] {
		if (stop_requested && !stopping) {
			stopping       = true;
			stop_requested = 0;
			StreamingProcess::Signal(guardian_pidfd, SIGUSR1);
			// Native teardown and the packaged guard's capture footer can take over three seconds.
			QTimer::singleShot(6000, &child, [&] {
				if (child.state() != QProcess::NotRunning) ::kill(-::getpgrp(), SIGKILL);
			});
		}
	});
	signal_timer.start();
	child.start(program, arguments);
	const int result = app.exec();
	::close(guardian_pidfd);
	::close(host_pidfd);
	::close(lock);
	return result;
#else
	return fail("Native streaming currently requires Linux.");
#endif
}
