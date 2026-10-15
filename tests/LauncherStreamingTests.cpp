#include "streamingApi.h"
#include "streamingAudio.h"
#include "streamingBackend.h"
#include "streamingCatalog.h"
#include "streamingSettings.h"
#include "streamingWidget.h"

#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSslKey>
#include <QSslSocket>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <cmath>
#include <cstring>
class LocalTlsServer: public QTcpServer {
public:
	QSslCertificate    certificate;
	QSslKey            key;
	QJsonObject        response {{"status", true}};
	QList<QJsonObject> requests;
	int                response_delay_ms = 0;

protected:
	void incomingConnection(qintptr descriptor) override {
		auto* socket = new QSslSocket(this);
		socket->setSocketDescriptor(descriptor);
		socket->setLocalCertificate(certificate);
		socket->setPrivateKey(key);
		connect(socket, &QSslSocket::disconnected, socket, &QObject::deleteLater);
		connect(socket, &QSslSocket::readyRead, this, [this, socket] {
			auto buffer = socket->property("buffer").toByteArray() + socket->readAll();
			socket->setProperty("buffer", buffer);
			const auto separator = buffer.indexOf("\r\n\r\n");
			if (separator < 0) return;
			const auto header = buffer.left(separator);
			int        length = 0;
			for (const auto& line: header.split('\n'))
				if (line.toLower().startsWith("content-length:"))
					length = line.mid(15).trimmed().toInt();
			if (buffer.size() < separator + 4 + length || socket->property("responded").toBool())
				return;
			socket->setProperty("responded", true);
			requests.append(
			    {{"line", QString::fromUtf8(header.split('\n').first().trimmed())},
			     {"body", QJsonDocument::fromJson(buffer.mid(separator + 4, length)).object()},
			     {"headers", QString::fromUtf8(header)}});
			const auto body = QJsonDocument(response).toJson(QJsonDocument::Compact);
			const auto send = [socket, body] {
				socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: "
				              "close\r\nContent-Length: " +
				              QByteArray::number(body.size()) + "\r\n\r\n" + body);
				socket->disconnectFromHost();
			};
			if (response_delay_ms > 0)
				QTimer::singleShot(response_delay_ms, socket, send);
			else
				send();
		});
		socket->startServerEncryption();
	}
};

class FixtureEnvironment {
public:
	explicit FixtureEnvironment(const QString& directory): m_path(qgetenv("PATH")) {
		qputenv("PATH", QFile::encodeName(directory) + ':' + m_path);
	}
	~FixtureEnvironment() { qputenv("PATH", m_path); }

private:
	QByteArray m_path;
};

class LauncherStreamingTests: public QObject {
	Q_OBJECT
	static bool GenerateCertificate(const QString& dir) {
		QProcess command;
		command.setStandardErrorFile(QProcess::nullDevice());
		command.start("openssl",
		              {"req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2", "-subj",
		               "/CN=localhost", "-keyout", QDir(dir).filePath("key.pem"), "-out",
		               QDir(dir).filePath("cert.pem")});
		return command.waitForFinished(10000) && command.exitCode() == 0;
	}
	static QByteArray Read(const QString& path) {
		QFile file(path);
		if (!file.open(QIODevice::ReadOnly)) return {};
		return file.readAll();
	}
	static bool Put(const QString& path, const QByteArray& bytes) {
		QFile file(path);
		if (!file.open(QIODevice::WriteOnly)) return false;
		return file.write(bytes) == bytes.size();
	}
	static int FreeBasePort() {
		for (int port = 50000; port < 64000; port += 37)
			if (StreamingBackend::PreflightPorts(port).isEmpty()) return port;
		return 0;
	}
private slots:
	void pinValidationRequiresCurrentIdAndUtf8Bounds() {
		QVERIFY(!StreamingApi::ValidatePin("", "1234", "TV").isEmpty());
		QVERIFY(!StreamingApi::ValidatePin(QString(32, 'a'), "12x4", "TV").isEmpty());
		QVERIFY(!StreamingApi::ValidatePin(QString(32, 'a'), "1234", QString(60, QChar(0x65e5)))
		             .isEmpty());
		QVERIFY(StreamingApi::ValidatePin(QString(32, 'a'), "0123", "Living-room TV").isEmpty());
	}
	void tlsApiPreservesPairingIdAndRejectsErrorsAndWrongPin() {
		QTemporaryDir certs;
		QVERIFY(GenerateCertificate(certs.path()));
		LocalTlsServer server;
		server.certificate = QSslCertificate(Read(certs.filePath("cert.pem")));
		server.key         = QSslKey(Read(certs.filePath("key.pem")), QSsl::Rsa);
		QVERIFY(server.listen(QHostAddress::LocalHost, 0));
		StreamingApi api;
		api.Configure(server.serverPort(), server.certificate, "private-user", "private-password");
		bool    finished = false;
		QString error;
		api.SubmitPin(QString(32, 'a'), "0123", "TV",
		              [&](const QJsonObject&, const QString& result) {
			              error    = result;
			              finished = true;
		              });
		QTRY_VERIFY_WITH_TIMEOUT(finished, 5000);
		QVERIFY2(error.isEmpty(), qPrintable(error));
		QCOMPARE(server.requests.size(), 1);
		QCOMPARE(server.requests[0].value("body").toObject().value("pairing_id").toString(),
		         QString(32, 'a'));
		QVERIFY(server.requests[0].value("headers").toString().contains(
		    "Authorization: Basic " +
		    QString::fromLatin1(QByteArray("private-user:private-password").toBase64())));
		server.response = {{"status", false}, {"error", "unknown client"}};
		finished        = false;
		api.Request("POST", "/api/clients/unpair", {{"uuid", "missing"}},
		            [&](const QJsonObject&, const QString& result) {
			            error    = result;
			            finished = true;
		            });
		QTRY_VERIFY_WITH_TIMEOUT(finished, 5000);
		QCOMPARE(error, QString("unknown client"));
		QCOMPARE(server.requests.last().value("body").toObject().value("uuid").toString(),
		         QString("missing"));
		QTemporaryDir other;
		QVERIFY(GenerateCertificate(other.path()));
		api.Configure(server.serverPort(), QSslCertificate(Read(other.filePath("cert.pem"))),
		              "user", "password");
		finished = false;
		api.Request("GET", "/api/pin", {}, [&](const QJsonObject&, const QString& result) {
			error    = result;
			finished = true;
		});
		QTRY_VERIFY_WITH_TIMEOUT(finished, 5000);
		QVERIFY(!error.isEmpty());
		QCOMPARE(server.requests.size(), 2);
	}
	void closeApiWaitsForGracefulShutdownOnly_data() {
		QTest::addColumn<QByteArray>("method");
		QTest::addColumn<QString>("path");
		QTest::addColumn<bool>("expect_timeout");
		QTest::newRow("graceful-app-close")
		    << QByteArray("POST") << QString("/api/apps/close") << false;
		QTest::newRow("ordinary-poll-deadline") << QByteArray("GET") << QString("/api/pin") << true;
	}
	void closeApiWaitsForGracefulShutdownOnly() {
		QFETCH(QByteArray, method);
		QFETCH(QString, path);
		QFETCH(bool, expect_timeout);
		QTemporaryDir certs;
		QVERIFY(GenerateCertificate(certs.path()));
		LocalTlsServer server;
		server.certificate       = QSslCertificate(Read(certs.filePath("cert.pem")));
		server.key               = QSslKey(Read(certs.filePath("key.pem")), QSsl::Rsa);
		server.response_delay_ms = 3100;
		QVERIFY(server.listen(QHostAddress::LocalHost, 0));
		StreamingApi api;
		api.Configure(server.serverPort(), server.certificate, "user", "password");
		bool          finished = false;
		QString       error;
		QElapsedTimer elapsed;
		elapsed.start();
		api.Request(method, path, {}, [&](const QJsonObject&, const QString& result) {
			error    = result;
			finished = true;
		});
		QTRY_VERIFY_WITH_TIMEOUT(finished, 6000);
		if (expect_timeout)
			QVERIFY(!error.isEmpty());
		else {
			QVERIFY2(error.isEmpty(), qPrintable(error));
			QVERIFY(elapsed.elapsed() >= 3000);
		}
		QCOMPARE(server.requests.size(), 1);
	}
	void audioOwnershipRejectsReusedIdsAndUnloadsOnlyItsModules() {
		QVERIFY(
		    StreamingAudio::IsOwnedModule("sink_name=kyty_owned rate=48000", "kyty_owned", false));
		QVERIFY(!StreamingAudio::IsOwnedModule("sink_name=kyty_owned_other", "kyty_owned", false));
		QVERIFY(!StreamingAudio::IsOwnedModule("sink_name=foreign", "kyty_owned", false));
		QVERIFY(StreamingAudio::IsOwnedModule("source=kyty_owned.monitor sink=original",
		                                      "kyty_owned", true));
		QTemporaryDir dir;
		const auto    fixture = Read(QStringLiteral(KYTY_LAUNCHER_FIXTURE));
		QVERIFY(!fixture.isEmpty());
		const auto pactl = dir.filePath("pactl");
		QVERIFY(Put(pactl, fixture));
		QFile::setPermissions(pactl, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
		StreamingAudio audio;
		audio.SetPactl(pactl);
		QSignalSpy ready(&audio, &StreamingAudio::Ready);
		QSignalSpy stopped(&audio, &StreamingAudio::Stopped);
		audio.Start(true);
		QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 5000);
		const auto sink = audio.Sink();
		QVERIFY(!sink.isEmpty());
		auto modules = QJsonDocument::fromJson(Read(dir.filePath("modules.json"))).array();
		QCOMPARE(modules.size(), 2);
		modules.append(QJsonObject {
		    {"index", 9999}, {"name", "module-null-sink"}, {"argument", "sink_name=foreign"}});
		QVERIFY(Put(dir.filePath("modules.json"), QJsonDocument(modules).toJson()));
		audio.Stop();
		QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
		modules = QJsonDocument::fromJson(Read(dir.filePath("modules.json"))).array();
		QCOMPARE(modules.size(), 1);
		QCOMPARE(modules[0].toObject().value("index").toInt(), 9999);
		const auto commands = Read(dir.filePath("pactl-commands.jsonl"));
		QVERIFY(!commands.contains("set-default-sink"));
		QVERIFY(commands.indexOf("4102") < commands.lastIndexOf("4101"));
	}
	void isolatedAudioPreservesSystemDefaultAndCapturesTone() {
		if (QStandardPaths::findExecutable("pipewire").isEmpty() ||
		    QStandardPaths::findExecutable("wireplumber").isEmpty() ||
		    QStandardPaths::findExecutable("pacat").isEmpty())
			QSKIP("Private audio tools are not installed.");
		const auto default_sink = [] {
			QProcess process;
			process.start("pactl", {"get-default-sink"});
			if (!process.waitForFinished(3000)) return QByteArray {};
			return process.readAllStandardOutput().trimmed();
		};
		const auto before = default_sink();
		if (before != "auto_null")
			QSKIP("This fallback test requires the system's automatic Dummy Output.");
		StreamingAudio audio;
		QSignalSpy     ready(&audio, &StreamingAudio::Ready);
		QSignalSpy     stopped(&audio, &StreamingAudio::Stopped);
		QSignalSpy     failed(&audio, &StreamingAudio::Failed);
		audio.Start(true);
		QTRY_VERIFY_WITH_TIMEOUT(ready.count() == 1 || failed.count() > 0, 8000);
		QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed[0][0].toString()));
		QCOMPARE(ready.count(), 1);
		QVERIFY(!audio.Server().isEmpty());
		QCOMPARE(default_sink(), before);
		auto environment = QProcessEnvironment::systemEnvironment();
		environment.insert("PULSE_SERVER", audio.Server());
		QProcess record;
		record.setProcessEnvironment(environment);
		record.start("parec", {"--raw", "--format=s16le", "--rate=48000", "--channels=2",
		                       "--device=" + audio.Sink() + ".monitor", "--latency-msec=20"});
		QVERIFY(record.waitForStarted(2000));
		QByteArray tone(48000 * 4, Qt::Uninitialized);
		for (int i = 0; i < 48000; ++i) {
			const auto sample =
			    qint16(6000.0 * std::sin(2.0 * 3.141592653589793 * 440.0 * i / 48000.0));
			std::memcpy(tone.data() + i * 4, &sample, 2);
			std::memcpy(tone.data() + i * 4 + 2, &sample, 2);
		}
		QProcess play;
		play.setProcessEnvironment(environment);
		play.start("pacat", {"--playback", "--raw", "--format=s16le", "--rate=48000",
		                     "--channels=2", "--device=" + audio.Sink(), "--latency-msec=20"});
		QVERIFY(play.waitForStarted(2000));
		play.write(tone);
		play.closeWriteChannel();
		const bool finished = play.waitForFinished(10000);
		if (!finished) {
			play.kill();
			play.waitForFinished(1000);
			record.terminate();
			record.waitForFinished(1000);
			qWarning() << "Private audio logs"
			           << Read(QFileInfo(audio.Server().mid(5)).dir().filePath("wireplumber.log"))
			           << Read(QFileInfo(audio.Server().mid(5)).dir().filePath("pipewire.log"));
		}
		QVERIFY(finished);
		QCOMPARE(play.exitCode(), 0);
		QTest::qWait(200);
		record.terminate();
		QVERIFY(record.waitForFinished(3000));
		const auto samples = record.readAllStandardOutput();
		QVERIFY(samples.size() > 10000);
		double squares = 0;
		for (qsizetype i = 0; i + 1 < samples.size(); i += 2) {
			qint16 value;
			std::memcpy(&value, samples.constData() + i, 2);
			squares += double(value) * value;
		}
		const auto rms = std::sqrt(squares / double(samples.size() / 2));
		qInfo() << "Isolated Pulse capture:" << samples.size() << "bytes; RMS" << rms
		        << "; system default" << before;
		QVERIFY(rms > 1000.0);
		audio.Stop();
		QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 7000);
		QVERIFY(audio.Server().isEmpty());
		QCOMPARE(default_sink(), before);
	}
	void stopDuringFailedAudioCommand_data() {
		QTest::addColumn<QStringList>("prefix");
		QTest::addColumn<bool>("after_load");
		QTest::newRow("default-query") << QStringList {"get-default-sink"} << false;
		QTest::newRow("sink-created-without-id")
		    << QStringList {"load-module", "module-null-sink"} << false;
		QTest::newRow("verify-default") << QStringList {"get-default-sink"} << true;
		QTest::newRow("loopback-created-without-id")
		    << QStringList {"load-module", "module-loopback"} << false;
	}
	void stopDuringFailedAudioCommand() {
		QFETCH(QStringList, prefix);
		QFETCH(bool, after_load);
		QTemporaryDir dir;
		QVERIFY(GenerateCertificate(dir.path()));
		const auto fixture = Read(QStringLiteral(KYTY_LAUNCHER_FIXTURE));
		for (const auto& name: {"host", "pactl"}) {
			QVERIFY(Put(dir.filePath(name), fixture));
			QFile::setPermissions(dir.filePath(name),
			                      QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
		}
		FixtureEnvironment environment(dir.path());
		QVERIFY(Put(dir.filePath("pactl-behavior.json"),
		            QJsonDocument(QJsonObject {{"prefix", QJsonArray::fromStringList(prefix)},
		                                       {"after_load", after_load},
		                                       {"delay", 0.35},
		                                       {"fail", true}})
		                .toJson()));
		StreamingBackend  backend(dir.filePath("profile"));
		StreamingSettings settings;
		settings.backend = dir.filePath("host");
		settings.port    = FreeBasePort();
		backend.Start(settings);
		QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(dir.filePath("command-pending")), 3000);
		backend.Stop();
		QTRY_VERIFY_WITH_TIMEOUT(backend.GetState() != StreamingBackend::State::Stopping, 2000);
		QCOMPARE(QJsonDocument::fromJson(Read(dir.filePath("modules.json"))).array().size(), 0);
		QFile::remove(dir.filePath("pactl-behavior.json"));
		backend.Start(settings);
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Ready, 7000);
		backend.Stop();
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Stopped, 5000);
	}
	void closeDuringModuleCreationCapturesIdAndCleansUp() {
		QTemporaryDir dir;
		const auto    fixture = Read(QStringLiteral(KYTY_LAUNCHER_FIXTURE));
		for (const auto& name: {"host", "pactl"}) {
			QVERIFY(Put(dir.filePath(name), fixture));
			QFile::setPermissions(dir.filePath(name),
			                      QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
		}
		FixtureEnvironment environment(dir.path());
		QVERIFY(Put(
		    dir.filePath("pactl-behavior.json"),
		    QJsonDocument(QJsonObject {{"prefix", QJsonArray {"load-module", "module-null-sink"}},
		                               {"delay", 0.35}})
		        .toJson()));
		auto              backend = std::make_unique<StreamingBackend>(dir.filePath("profile"));
		StreamingSettings settings;
		settings.backend = dir.filePath("host");
		settings.port    = FreeBasePort();
		backend->Start(settings);
		QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(dir.filePath("command-pending")), 3000);
		QCOMPARE(QJsonDocument::fromJson(Read(dir.filePath("modules.json"))).array().size(), 1);
		backend.reset();
		QCOMPARE(QJsonDocument::fromJson(Read(dir.filePath("modules.json"))).array().size(), 0);
		QVERIFY(!QFile::exists(dir.filePath("profile/launcher.lock")));
	}
	void stopReapsStartupToolsBeforeUnlocking_data() {
		QTest::addColumn<QString>("stage");
		QTest::newRow("capability-helper") << QString("capability");
		QTest::newRow("credential-writer") << QString("credential");
	}
	void stopReapsStartupToolsBeforeUnlocking() {
		QFETCH(QString, stage);
		QTemporaryDir dir;
		QVERIFY(GenerateCertificate(dir.path()));
		const auto fixture = Read(QStringLiteral(KYTY_LAUNCHER_FIXTURE));
		for (const auto& name: {"host", "pactl"}) {
			QVERIFY(Put(dir.filePath(name), fixture));
			QFile::setPermissions(dir.filePath(name),
			                      QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
		}
		FixtureEnvironment environment(dir.path());
		QVERIFY(Put(dir.filePath(stage + "-delay"), "2.0"));
		QVERIFY(Put(dir.filePath("ignore-termination"), "yes"));
		StreamingBackend  backend(dir.filePath("profile"));
		StreamingSettings settings;
		settings.backend = dir.filePath("host");
		settings.port    = FreeBasePort();
		backend.Start(settings);
		QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(dir.filePath(stage + "-active")), 3000);
		const auto pid = QString::fromUtf8(Read(dir.filePath(stage + "-active"))).trimmed();
		QVERIFY(!pid.isEmpty());
		backend.Stop();
		QTest::qWait(100);
		QCOMPARE(backend.GetState(), StreamingBackend::State::Stopping);
		QVERIFY(QFile::exists(dir.filePath("profile/launcher.lock")));
		QVERIFY(QFileInfo::exists("/proc/" + pid));
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Stopped, 5000);
		QVERIFY2(!QFileInfo::exists("/proc/" + pid),
		         "Startup helper was still alive after profile unlock");
		QVERIFY(!QFile::exists(dir.filePath("profile/launcher.lock")));
		QFile::remove(dir.filePath(stage + "-delay"));
		QFile::remove(dir.filePath("ignore-termination"));
		backend.Start(settings);
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Ready, 7000);
		QVERIFY(!QFile::exists(dir.filePath("credential-overlap")));
		backend.Stop();
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Stopped, 5000);
	}
	void hostLifecycleUsesRealAsyncApiAndRestartsAfterFailure() {
		QTemporaryDir dir;
		QVERIFY(GenerateCertificate(dir.path()));
		const auto fixture = Read(QStringLiteral(KYTY_LAUNCHER_FIXTURE));
		for (const auto& name: {"pactl", "host"}) {
			QVERIFY(Put(dir.filePath(name), fixture));
			QFile::setPermissions(dir.filePath(name),
			                      QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
		}
		const auto previous_path = qgetenv("PATH");
		qputenv("PATH", QFile::encodeName(dir.path()) + ':' + previous_path);
		StreamingBackend  backend(dir.filePath("profile"));
		StreamingSettings settings;
		settings.backend     = dir.filePath("host");
		settings.port        = FreeBasePort();
		settings.local_audio = true;
		backend.Start(settings);
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Ready, 10000);
		QVERIFY(Read(dir.filePath("stdin-credentials-used")) == "yes");
		QTRY_COMPARE_WITH_TIMEOUT(backend.Clients().size(), 1, 5000);
		QCOMPARE(backend.Pairings().size(), 1);
		QVERIFY(!backend.Session().contains("Game running"));
		const auto config = Read(dir.filePath("profile/host.conf"));
		QVERIFY(config.contains("management_bind_address = 127.0.0.1"));
		QVERIFY(config.contains("capture = kyty"));
		QVERIFY(config.contains("kyty_private_audio = true"));
		QVERIFY(config.contains("audio_sink = kyty_stream_"));
		QVERIFY(!config.contains(".monitor"));
		backend.Pair(QString(32, 'a'), "1234", "TV");
		backend.Unpair("client-1");
		QTRY_VERIFY_WITH_TIMEOUT(Read(dir.filePath("api-requests.jsonl")).contains("pairing_id"),
		                         5000);
		QTRY_VERIFY_WITH_TIMEOUT(Read(dir.filePath("api-requests.jsonl")).contains("client-1"),
		                         5000);
		backend.Stop();
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Stopped, 7000);
		QCOMPARE(QJsonDocument::fromJson(Read(dir.filePath("modules.json"))).array().size(), 0);
		QVERIFY(Put(dir.filePath("crash-host"), "yes"));
		backend.Start(settings);
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Error, 7000);
		QVERIFY(backend.Error().contains("42"));
		QFile::remove(dir.filePath("crash-host"));
		backend.Start(settings);
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Ready, 10000);
		backend.Stop();
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Stopped, 7000);
		qputenv("PATH", previous_path);
	}
	void nativeWidgetDoesNotStartHostAndShowsActionableError() {
		QTemporaryDir    dir;
		StreamingBackend backend(dir.path());
		StreamingWidget  widget(&backend);
		widget.resize(940, 900);
		widget.show();
		auto* start = widget.findChild<QPushButton*>("streamStart");
		auto* stop  = widget.findChild<QPushButton*>("streamStop");
		QVERIFY(start && stop);
		QVERIFY(start->isEnabled());
		QVERIFY(!stop->isEnabled());
		QCOMPARE(backend.GetState(), StreamingBackend::State::Stopped);
		QVERIFY(backend.Logs().isEmpty());
		auto* path = widget.findChild<QLineEdit*>("streamBackendPath");
		path->setText(dir.filePath("missing-helper"));
		QTest::mouseClick(start, Qt::LeftButton);
		QTRY_COMPARE(backend.GetState(), StreamingBackend::State::Error);
		QVERIFY(widget.findChild<QLabel*>("streamError")->text().contains("unavailable"));
		QVERIFY(start->isEnabled());
		QVERIFY(!stop->isEnabled());
		const auto screenshot = qEnvironmentVariable("KYTY_STREAM_UI_SCREENSHOT");
		if (!screenshot.isEmpty()) {
			QTest::qWait(100);
			QVERIFY(widget.grab().save(screenshot));
			auto* scroll = widget.findChild<QScrollArea*>();
			QVERIFY(scroll->widget()->grab().save(screenshot + ".full.png"));
		}
	}
	void launchWrapperPreservesArgvAndEnforcesOneGame() {
		QTemporaryDir dir;
		const auto    wrapper =
		    QDir(QCoreApplication::applicationDirPath()).filePath("libexec/kyty_stream_launch");
		QVERIFY(QFileInfo(wrapper).isExecutable());
		const QString        tricky = "space ' quote \" $() 日本 ; touch NEVER";
		StreamingLaunchEntry game {
		    "game1",
		    "Astro",
		    "/usr/bin/python3",
		    {"-c",
		     "import json,os,sys; "
		     "print(json.dumps({'argv':sys.argv[1:],'audio':os.getenv('KYTY_AUDIO_OUTPUT_DEVICE'),'"
		     "driver':os.getenv('SDL_AUDIO_DRIVER'),'width':os.getenv('KYTY_STREAM_WIDTH')}))",
		     tricky},
		    dir.path()};
		auto manifest =
		    StreamingCatalog::Manifest({game}, dir.filePath("frames.sock"), "Kyty_Private_Audio",
		                               dir.filePath("game.lock"), "1440p", 60);
		manifest.insert("session_file", dir.filePath("session.json"));
		QString error;
		QVERIFY(StreamingCatalog::WritePrivate(dir.filePath("launch.json"),
		                                       QJsonDocument(manifest).toJson(), &error));
		QProcess launch;
		launch.start(wrapper, {dir.filePath("launch.json"), "game1"});
		QVERIFY(launch.waitForFinished(5000));
		QCOMPARE(launch.exitCode(), 0);
		const auto output = QJsonDocument::fromJson(launch.readAllStandardOutput()).object();
		QCOMPARE(output.value("argv").toArray().at(0).toString(), tricky);
		QCOMPARE(output.value("audio").toString(), QString("Kyty_Private_Audio"));
		QCOMPARE(output.value("driver").toString(), QString("pulseaudio"));
		QCOMPARE(output.value("width").toString(), QString("2560"));
		QVERIFY(!QFile::exists(dir.filePath("NEVER")));
		QVERIFY(!QFile::exists(dir.filePath("session.json")));
		game.program   = "/usr/bin/sleep";
		game.arguments = {"10"};
		manifest       = StreamingCatalog::Manifest({game}, dir.filePath("frames.sock"), "private",
		                                            dir.filePath("game.lock"), "Native", 30);
		manifest.insert("session_file", dir.filePath("session.json"));
		QVERIFY(StreamingCatalog::WritePrivate(dir.filePath("launch.json"),
		                                       QJsonDocument(manifest).toJson(), &error));
		QProcess first;
		first.start(wrapper, {dir.filePath("launch.json"), "game1"});
		QVERIFY(first.waitForStarted(1000));
		QTRY_VERIFY(QFile::exists(dir.filePath("session.json")));
		QProcess second;
		second.start(wrapper, {dir.filePath("launch.json"), "game1"});
		QVERIFY(second.waitForFinished(2000));
		QCOMPARE(second.exitCode(), 1);
		QVERIFY(second.readAllStandardError().contains("already running"));
		first.terminate();
		QVERIFY(first.waitForFinished(5000));
		QVERIFY(!QFile::exists(dir.filePath("session.json")));
	}
	void launchWrapperAllowsGuardToFinishCaptureMetadata() {
		QTemporaryDir dir;
		const auto    wrapper =
		    QDir(QCoreApplication::applicationDirPath()).filePath("libexec/kyty_stream_launch");
		QVERIFY(QFileInfo(wrapper).isExecutable());
		const auto guard = dir.filePath("guard.py");
		const auto ended = dir.filePath("ended");
		const auto ready = dir.filePath("guard-ready");
		QVERIFY(Put(guard, R"PY(import pathlib, signal, sys, time

def stop(signum, frame):
    time.sleep(3.5)
    pathlib.Path(sys.argv[1]).write_text("capture complete")
    sys.exit(0)

signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)
pathlib.Path(sys.argv[2]).write_text("ready")
while True:
    time.sleep(0.05)
)PY"));
		const StreamingLaunchEntry game {
		    "guard", "Astro", "/usr/bin/python3", {guard, ended, ready}, dir.path()};
		auto manifest =
		    StreamingCatalog::Manifest({game}, dir.filePath("frames.sock"), "Kyty_Private_Audio",
		                               dir.filePath("game.lock"), "Native", 60);
		manifest.insert("session_file", dir.filePath("session.json"));
		QString error;
		QVERIFY(StreamingCatalog::WritePrivate(dir.filePath("launch.json"),
		                                       QJsonDocument(manifest).toJson(), &error));
		QProcess launch;
		launch.start(wrapper, {dir.filePath("launch.json"), "guard"});
		QVERIFY(launch.waitForStarted(1000));
		QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(ready), 3000);
		launch.terminate();
		QVERIFY(launch.waitForFinished(7000));
		QCOMPARE(Read(ended), QByteArray("capture complete"));
		QCOMPARE(launch.exitCode(), 0);
		QVERIFY(!QFile::exists(dir.filePath("session.json")));
	}
	void portPreflightDetectsConflict() {
		const int base = FreeBasePort();
		QVERIFY(base > 0);
		QTcpServer occupied;
		QVERIFY(occupied.listen(QHostAddress::LocalHost, quint16(base + 1)));
		QVERIFY(StreamingBackend::PreflightPorts(base).contains(QString::number(base + 1)));
	}

	void catalogKeepsArgumentsOutOfBackendCommand() {
		const StreamingLaunchEntry game {"a55",
		                                 "Astro $() ' 日本",
		                                 "/game/emulator",
		                                 {"--game", "/games/a '$(touch NEVER)'/eboot.bin"},
		                                 "/game"};
		const auto                 manifest =
		    StreamingCatalog::Manifest({game}, "/run/kyty/frames.sock", "Kyty private",
		                               "/run/kyty/emulator.lock", "1440p", 60);
		const auto arguments =
		    manifest.value("games").toObject().value("a55").toObject().value("arguments").toArray();
		QCOMPARE(arguments.size(), 2);
		QCOMPARE(arguments.at(1).toString(), game.arguments[1]);
		QCOMPARE(manifest["environment"].toObject()["SDL_AUDIO_DRIVER"].toString(),
		         QString("pulseaudio"));
		QCOMPARE(manifest["environment"].toObject()["KYTY_AUDIO_OUTPUT_DEVICE"].toString(),
		         QString("Kyty private"));
		const auto apps =
		    StreamingCatalog::Apps({game}, "/app/wrapper", "/profile/manifest.json")["apps"]
		        .toArray();
		QCOMPARE(apps.size(), 1);
		const auto app = apps[0].toObject();
		QVERIFY(!app["cmd"].toString().contains("touch NEVER"));
		QVERIFY(!app["auto-detach"].toBool(true));
		QVERIFY(app["wait-all"].toBool());
	}
	void catalogEscapesSunshineExpansionAndWritesPrivate() {
		QCOMPARE(StreamingCatalog::QuoteBackendToken("a $(HOME) \" b"),
		         QString("\"a $$(HOME) \\\" b\""));
		QTemporaryDir dir;
		QString       error;
		const auto    path = dir.filePath("private.json");
		QVERIFY(StreamingCatalog::WritePrivate(path, "secret", &error));
		QCOMPARE(QFile::permissions(path) &
		             (QFile::ReadGroup | QFile::ReadOther | QFile::WriteGroup | QFile::WriteOther),
		         QFile::Permissions());
	}
	void settingsRejectInvalidRanges() {
		StreamingSettings s;
		QVERIFY(s.Validate().isEmpty());
		s.bitrate = 101;
		QVERIFY(!s.Validate().isEmpty());
		s.bitrate = 5;
		s.port    = 65520;
		QVERIFY(!s.Validate().isEmpty());
		s.port = 48189;
		s.fps  = 120;
		QVERIFY(!s.Validate().isEmpty());
		s.fps        = 30;
		s.resolution = "4K";
		QVERIFY(!s.Validate().isEmpty());
		s.resolution = "1440p";
		s.codec      = "AV1";
		QVERIFY(!s.Validate().isEmpty());
	}
	void settingsRoundTripSeparateFile() {
		QTemporaryDir dir;
		QFile         original(dir.filePath("Kyty.ini"));
		QVERIFY(original.open(QIODevice::WriteOnly));
		original.write("[Library]\nuntouched=yes\n");
		original.close();
		QSettings         file(dir.filePath("streaming.ini"), QSettings::IniFormat);
		StreamingSettings s;
		s.backend     = "/private/host path";
		s.port        = 49189;
		s.resolution  = "1440p";
		s.fps         = 30;
		s.bitrate     = 77;
		s.codec       = "H264";
		s.adapter     = "RTX 3090";
		s.local_audio = false;
		s.Write(file);
		file.sync();
		const auto read = StreamingSettings::Read(file);
		QCOMPARE(read.backend, s.backend);
		QCOMPARE(read.port, s.port);
		QCOMPARE(read.resolution, s.resolution);
		QCOMPARE(read.fps, s.fps);
		QCOMPARE(read.bitrate, s.bitrate);
		QCOMPARE(read.codec, s.codec);
		QCOMPARE(read.adapter, s.adapter);
		QCOMPARE(read.local_audio, s.local_audio);
		QVERIFY(original.open(QIODevice::ReadOnly));
		QCOMPARE(original.readAll(), QByteArray("[Library]\nuntouched=yes\n"));
	}
};
QTEST_MAIN(LauncherStreamingTests)
#include "LauncherStreamingTests.moc"
