#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QTextStream>
#include <QTimer>

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
} // namespace

int main(int argc, char** argv) {
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
	const auto game    = manifest.value("games").toObject().value(app.arguments()[2]).toObject();
	const auto program = game.value("program").toString();
	if (game.isEmpty() || !QFileInfo(program).isAbsolute() || !QFileInfo(program).isExecutable())
		return fail("Catalog entry is missing or its emulator is unavailable.");
	const auto lock_path = QFile::encodeName(manifest.value("lock_file").toString());
	const int  lock      = ::open(lock_path.constData(), O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
	if (lock < 0 || ::fstat(lock, &info) != 0 || info.st_uid != ::getuid() ||
	    !S_ISREG(info.st_mode) || (info.st_mode & 0077) != 0 ||
	    ::flock(lock, LOCK_EX | LOCK_NB) != 0)
		return fail("Another Kyty streaming game is already running, or its lock is unavailable.");
	const auto        env = manifest.value("environment").toObject();
	const QStringList allowed {
	    "KYTY_STREAM_SOCKET", "PULSE_SERVER",       "SDL_AUDIO_DRIVER", "KYTY_AUDIO_OUTPUT_DEVICE",
	    "KYTY_STREAM_WIDTH",  "KYTY_STREAM_HEIGHT", "KYTY_STREAM_FPS"};
	for (const auto& key: allowed)
		qunsetenv(key.toLatin1().constData());
	for (auto it = env.begin(); it != env.end(); ++it) {
		if (!allowed.contains(it.key()) || !it.value().isString() ||
		    it.value().toString().contains(QChar::Null))
			return fail("Invalid launch environment.");
		qputenv(it.key().toLatin1().constData(), it.value().toString().toUtf8());
	}
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
	const auto session_path = manifest.value("session_file").toString();
	QProcess   child;
	child.setProcessChannelMode(QProcess::ForwardedChannels);
	QObject::connect(&child, &QProcess::started, &app, [&] {
		QJsonObject session {{"pid", qint64(::getpid())},
		                     {"game", app.arguments()[2]},
		                     {"width", qEnvironmentVariable("SUNSHINE_CLIENT_WIDTH")},
		                     {"height", qEnvironmentVariable("SUNSHINE_CLIENT_HEIGHT")},
		                     {"fps", qEnvironmentVariable("SUNSHINE_CLIENT_FPS")},
		                     {"client", qEnvironmentVariable("SUNSHINE_CLIENT_NAME")}};
		QSaveFile   status(session_path);
		if (!status.open(QIODevice::WriteOnly) ||
		    !status.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
		    status.write(QJsonDocument(session).toJson()) < 0 || !status.commit()) {
			QTextStream(stderr) << "Cannot publish private session diagnostics.\n";
		}
	});
	QObject::connect(&child, &QProcess::finished, &app, [&](int code, QProcess::ExitStatus status) {
		QFile::remove(session_path);
		app.exit(status == QProcess::NormalExit ? code : 1);
	});
	QObject::connect(&child, &QProcess::errorOccurred, &app, [&](QProcess::ProcessError error) {
		if (error == QProcess::FailedToStart) {
			fail(child.errorString());
			app.exit(1);
		}
	});
	std::signal(SIGTERM, OnSignal);
	std::signal(SIGINT, OnSignal);
	QTimer signal_timer;
	signal_timer.setInterval(100);
	QObject::connect(&signal_timer, &QTimer::timeout, &app, [&] {
		if (stop_requested && child.state() != QProcess::NotRunning) {
			stop_requested = 0;
			child.terminate();
			// Native teardown and the packaged guard's capture footer can take over three seconds.
			QTimer::singleShot(6000, &child, [&] {
				if (child.state() != QProcess::NotRunning) child.kill();
			});
		}
	});
	signal_timer.start();
	child.start(program, arguments);
	const int result = app.exec();
	QFile::remove(session_path);
	::close(lock);
	return result;
#else
	return fail("Native streaming currently requires Linux.");
#endif
}
