#include "hostControlClient.h"
#include "agsRustStreamingDialog.h"
#include "hostSocket.h"
#include "streamingApi.h"
#include "streamingAudio.h"
#include "streamingBackend.h"
#include "streamingCatalog.h"
#include "streamingProcess.h"
#include "streamingProfile.h"
#include "streamingSettings.h"
#include "streamingWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QSslKey>
#include <QSslSocket>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QUdpSocket>
#include <QtTest>

#include <cmath>
#include <cstring>
#ifdef __linux__
#include <QThread>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <unistd.h>
#endif
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

class FixtureGameGroup {
public:
	qint64 pid = 0;
	~FixtureGameGroup() {
#ifdef __linux__
		if (pid > 1 && pid != ::getpgrp()) ::kill(-pid_t(pid), SIGKILL);
#endif
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

// Scripted control.sock peer: answers status/get-preferences/set-preferences and records requests.
class FakeHostDaemon {
public:
	explicit FakeHostDaemon(const QString& directory)
	    : m_path(QDir(directory).filePath("host/control.sock")),
	      m_server([this](const QJsonObject& request, std::function<void(HostReply)> done) {
		      requests.append(request);
		      const auto  type = request.value("type").toString();
		      QJsonObject reply {{"ok", true}};
		      if (type == "status") {
			      reply = status;
		      } else if (type == "get-preferences") {
			      reply = preferences;
			      if (preference_reply_delay_ms > 0) {
				      QTimer::singleShot(preference_reply_delay_ms, &m_server, [reply, done] {
					      done({200, "application/json", QJsonDocument(reply).toJson(QJsonDocument::Compact), {}});
				      });
				      return;
			      }
		      } else if (type == "set-preferences") {
			      for (auto it = request.begin(); it != request.end(); ++it)
				      if (it.key() != "type") preferences.insert(it.key(), it.value());
			      if (apply_preferences) { apply_preferences(preferences, std::move(done)); return; }
		      }
		      done({200, "application/json", QJsonDocument(reply).toJson(QJsonDocument::Compact),
		            {}});
	      }) {
		QString error;
		m_listening = m_server.Listen(m_path, &error);
		Q_ASSERT_X(m_listening, "FakeHostDaemon", qPrintable(error));
	}
	[[nodiscard]] QString Path() const { return m_path; }
	[[nodiscard]] QList<QJsonObject> Requests(const QString& type) const {
		QList<QJsonObject> found;
		for (const auto& request: requests)
			if (request.value("type").toString() == type) found.append(request);
		return found;
	}
	static QJsonObject ReadyStatus() {
		return {{"state", "Ready"},
		        {"error", ""},
		        {"address", "192.168.1.3"},
		        {"port", 48189},
		        {"session", "No game running"},
		        {"stream_game_running", false},
		        {"local_game_running", false},
		        {"pairings", QJsonArray {QJsonObject {{"id", QString(32, 'a')}, {"name", "Living-room TV"}}}},
		        {"clients", QJsonArray {QJsonObject {{"uuid", "client-1"}, {"name", "Paired TV"}}}},
		        {"hid", QJsonObject {}}};
	}
	static QJsonObject DefaultPreferences() {
		return {{"port", 48189},    {"resolution", "Native"}, {"fps", 60},
		        {"bitrate", 30},    {"codec", "HEVC"},        {"adapter", ""},
		        {"local_audio", true}, {"show_local", true}};
	}

	QJsonObject        status      = ReadyStatus();
	QJsonObject        preferences = DefaultPreferences();
	QList<QJsonObject> requests;
	int preference_reply_delay_ms = 0;
	std::function<void(const QJsonObject&, std::function<void(HostReply)>)> apply_preferences;

private:
	QString          m_path;
	bool             m_listening = false;
	HostSocketServer m_server;
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
	static bool AttachHostOwner(QJsonObject* manifest, const QString& directory) {
#ifdef __linux__
		StreamingProcess::Identity identity;
		if (!StreamingProcess::ReadIdentity(int(QCoreApplication::applicationPid()), &identity))
			return false;
		const auto nonce = QUuid::createUuid().toString(QUuid::Id128);
		const auto owner = QDir(directory).filePath("owner.json");
		QString    error;
		if (!StreamingCatalog::WritePrivate(
		        owner,
		        QJsonDocument(
		            QJsonObject {{"nonce", nonce},
		                         {"pid", qint64(identity.pid)},
		                         {"start_time", QString::number(qulonglong(identity.start))},
		                         { "executable",
			                       QString::fromLocal8Bit(identity.executable.c_str()) }})
		            .toJson(),
		        &error))
			return false;
		manifest->insert("host_nonce", nonce);
		manifest->insert("host_owner_file", owner);
		return true;
#else
		return false;
#endif
	}
	static void HostEnvironment(QProcess* process, const QJsonObject& manifest) {
		auto environment = QProcessEnvironment::systemEnvironment();
		environment.insert("KYTY_STREAM_HOST_NONCE", manifest.value("host_nonce").toString());
		process->setProcessEnvironment(environment);
	}
	static bool ProcessRunning(qint64 pid) {
		if (pid < 2) return false;
		const auto stat  = Read(QStringLiteral("/proc/%1/stat").arg(pid));
		const auto close = stat.lastIndexOf(") ");
		return close >= 0 && stat.size() > close + 2 && stat[close + 2] != 'Z' &&
		       stat[close + 2] != 'X';
	}
	static int FreeBasePort() {
		for (int port = 50000; port < 64000; port += 37)
			if (StreamingBackend::PreflightPorts(port).isEmpty()) return port;
		return 0;
	}
	// A systemctl stand-in that appends its arguments to <directory>/systemctl.log and succeeds.
	static QString MakeFakeSystemctl(const QString& directory) {
		const auto script = QDir(directory).filePath("fake-systemctl");
		const auto log    = QDir(directory).filePath("systemctl.log");
		if (!Put(script, "#!/bin/sh\necho \"$@\" >> '" + log.toUtf8() + "'\nexit 0\n")) return {};
		QFile::setPermissions(script, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
		return script;
	}
	static QByteArray SystemctlLog(const QString& directory) {
		return Read(QDir(directory).filePath("systemctl.log"));
	}
private slots:
	void nativeActualHostNoGameLifecycle() {
		const auto binary = qEnvironmentVariable("AGS_TEST_HOST_BINARY");
		if (binary.isEmpty()) QSKIP("Set AGS_TEST_HOST_BINARY to exercise the actual native binary.");
		QTemporaryDir dir;
		QVERIFY(QDir().mkpath(dir.filePath("profile")));
		QVERIFY(Put(dir.filePath("profile/streaming.ini"), "[host]\nport=48200\n"));
		StreamingBackend backend(dir.filePath("profile")); backend.SetApiSocket(dir.filePath("api.sock"));
		StreamingSettings settings; settings.backend = binary; settings.port = FreeBasePort();
		backend.Start(settings);
		QTRY_VERIFY_WITH_TIMEOUT(backend.GetState() == StreamingBackend::State::Ready || backend.GetState() == StreamingBackend::State::Error, 10000);
		QCOMPARE(backend.GetState(), StreamingBackend::State::Ready);
		QCOMPARE(Read(dir.filePath("profile/streaming.ini")), QByteArray("[host]\nport=48200\n"));
		QVERIFY(!backend.StreamGameRunning());
		QVERIFY(!backend.NativeStatus().value("address").toString().isEmpty());
		bool exported = false;
		backend.ExportPairing("127.0.0.1:" + QString::number(settings.port), [&](QJsonObject reply, QString error) {
			exported = error.isEmpty() && QFile::exists(reply.value("path").toString());
		});
		QTRY_VERIFY(exported);
		backend.Stop();
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Stopped, 10000);
	}

	void nativeLocalBusyDoesNotStartWorker() {
		QTemporaryDir dir;
		StreamingBackend backend(dir.filePath("profile"));
		StreamingSettings settings; settings.backend = "/bin/true";
		backend.SetLocalGameRunning(true); backend.Start(settings);
		QCOMPARE(backend.GetState(), StreamingBackend::State::Error);
		QVERIFY(backend.Error().contains("locally running"));
		QVERIFY(!QFile::exists(dir.filePath("profile/native/host.json")));
	}
	void nativeCancelStartupIsNotFailure() {
		QTemporaryDir dir;
		QVERIFY(Put(dir.filePath("native-host"), Read(QFileInfo(QStringLiteral(KYTY_LAUNCHER_FIXTURE)).dir().filePath("LauncherNativeHostFixture.py"))));
		QVERIFY(QFile::setPermissions(dir.filePath("native-host"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
		QVERIFY(Put(dir.filePath("ready-delay"), "2"));
		StreamingBackend backend(dir.filePath("profile")); backend.SetApiSocket(dir.filePath("api.sock"));
		StreamingSettings settings; settings.backend = dir.filePath("native-host"); settings.port = FreeBasePort();
		backend.Start(settings); QTRY_VERIFY(QFile::exists(dir.filePath("native-pid")));
		QCOMPARE(backend.GetState(), StreamingBackend::State::Starting);
		backend.Stop();
		QTRY_VERIFY(backend.GetState() != StreamingBackend::State::Stopping);
		QCOMPARE(backend.GetState(), StreamingBackend::State::Stopped);
		QVERIFY(backend.Error().isEmpty());
	}

	void nativeProcessAloneNeverBecomesReady() {
		QTemporaryDir dir;
		QVERIFY(Put(dir.filePath("native-host"), Read(QFileInfo(QStringLiteral(KYTY_LAUNCHER_FIXTURE)).dir().filePath("LauncherNativeHostFixture.py"))));
		QVERIFY(QFile::setPermissions(dir.filePath("native-host"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
		QVERIFY(Put(dir.filePath("never-ready"), "true"));
		StreamingBackend backend(dir.filePath("profile")); backend.SetApiSocket(dir.filePath("api.sock"));
		StreamingSettings settings; settings.backend = dir.filePath("native-host"); settings.port = FreeBasePort();
		backend.Start(settings);
		QTRY_VERIFY(QFile::exists(dir.filePath("native-pid")));
		QCOMPARE(backend.GetState(), StreamingBackend::State::Starting);
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Error, 24000);
		QVERIFY(backend.NativeStatus().isEmpty());
		QVERIFY(!ProcessRunning(Read(dir.filePath("native-pid")).trimmed().toLongLong()));
	}

	void nativeReadinessReloadAndLifecycle() {
		QTemporaryDir dir;
		const auto source = QFileInfo(QStringLiteral(KYTY_LAUNCHER_FIXTURE)).dir().filePath("LauncherNativeHostFixture.py");
		QVERIFY(Put(dir.filePath("native-host"), Read(source)));
		QVERIFY(QFile::setPermissions(dir.filePath("native-host"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
		QVERIFY(Put(dir.filePath("ready-delay"), "0.25"));
		StreamingBackend backend(dir.filePath("profile"));
		backend.SetApiSocket(dir.filePath("api.sock"));
		StreamingSettings settings;
		settings.backend = dir.filePath("native-host");
		settings.port = FreeBasePort();
		backend.Start(settings);
		QCOMPARE(backend.GetState(), StreamingBackend::State::Starting);
		QTest::qWait(100);
		QCOMPARE(backend.GetState(), StreamingBackend::State::Starting);
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Ready, 5000);
		QVERIFY(!Read(dir.filePath("native-invocations.jsonl")).contains("--kyty-capabilities"));
		QVERIFY(!QFile::exists(dir.filePath("profile/host.conf")));
		QVERIFY(!QFile::exists(dir.filePath("profile/launch.json")));
		QVERIFY(!QFile::exists(dir.filePath("profile/apps.json")));
		// Simulate a pairing committed after the settings snapshot was loaded.
		auto disk = QJsonDocument::fromJson(Read(dir.filePath("profile/native/host.json"))).object();
		const QString uuid = "00000000-0000-4000-8000-000000000001";
		disk["devices"] = QJsonObject {{uuid, "Fixture tablet"}};
		QVERIFY(Put(dir.filePath("profile/native/host.json"), QJsonDocument(disk).toJson()));
		backend.SetCatalog({{"stable-game", "Game", "/bin/true", {}, dir.path()}});
		backend.PublishCatalog();
		QTRY_COMPARE_WITH_TIMEOUT(backend.Clients().size(), 1, 3000);
		QCOMPARE(QJsonDocument::fromJson(Read(dir.filePath("profile/native/host.json"))).object()["devices"].toObject().size(), 1);
		QVERIFY(Put(dir.filePath("native-state.json"), R"({"running":"stable-game","media_state":"starting"})"));
		backend.Refresh();
		QTRY_VERIFY(backend.StreamGameRunning());
		QCOMPARE(backend.RunningGameId(), "stable-game");
		QVERIFY(Put(dir.filePath("close-delay"), "0.25"));
		backend.CloseSession();
		QTest::qWait(80);
		QVERIFY(backend.StreamGameRunning());
		QTRY_VERIFY(!backend.StreamGameRunning());
		bool paired = false;
		backend.ExportPairing("127.0.0.1:48231", [&](QJsonObject reply, QString error) {
			paired = error.isEmpty() && QFile::exists(reply.value("path").toString());
		});
		QTRY_VERIFY(paired);
		settings.port += 1; settings.disconnect_grace = 0; settings.show_local = true;
		settings.local_audio = false; settings.fec_enabled = true; settings.encoder = "software"; settings.audio_codec = "raw_pcm";
		bool reloaded = false;
		backend.ReloadSettings(settings, [&](QJsonObject reply, QString error) {
			reloaded = error.isEmpty() && reply.value("restart_required").toBool();
		});
		QTRY_VERIFY(reloaded);
		QCOMPARE(backend.NativeStatus().value("address").toString(), "127.0.0.1:48231");
		QCOMPARE(backend.NativeStatus().value("configured").toObject().value("disconnect_grace_secs").toInt(), 0);
		QVERIFY(backend.NativeStatus().value("configured").toObject().value("local_audio_sink").isNull());
		QCOMPARE(backend.NativeStatus().value("configured").toObject().value("presentation").toString(), "mirrored");
		QCOMPARE(backend.NativeStatus().value("configured").toObject().value("audio_codec").toString(), "raw_pcm");
		backend.Unpair(uuid);
		QTRY_VERIFY(backend.Clients().isEmpty());
		backend.Stop();
		QCOMPARE(backend.GetState(), StreamingBackend::State::Stopping);
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Stopped, 3000);
		backend.Start(settings);
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Ready, 5000);
		const auto pid = Read(dir.filePath("native-pid")).trimmed().toLongLong();
		QVERIFY(pid > 1);
		::kill(pid_t(pid), SIGKILL);
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Error, 3000);
		QVERIFY(!backend.StreamGameRunning());
		QVERIFY(backend.Hid().isEmpty());
	}

	void negotiatedTvResolutionIsolatedFromLocalProfile() {
		const QStringList local{
				"--screen-width",     "2560", "--screen-height", "1440",
				"--vblank-frequency", "60",   "--game",          "/games/Astro"};
		for (const auto &mode :
				 QList<QPair<QString, QString>>{{"1920", "1080"}, {"2560", "1440"}}) {
			QProcessEnvironment client;
			client.insert("SUNSHINE_CLIENT_WIDTH", mode.first);
			client.insert("SUNSHINE_CLIENT_HEIGHT", mode.second);
			client.insert("SUNSHINE_CLIENT_FPS", "30");
			QProcessEnvironment output;
			output.insert("KYTY_STREAM_WIDTH", "1280");
			QStringList stream = local;
			QString error;
			QVERIFY2(StreamingProfile::Apply(true, client, &stream, &output, &error),
							 qPrintable(error));
			QCOMPARE(stream.at(stream.indexOf("--screen-width") + 1), mode.first);
			QCOMPARE(stream.at(stream.indexOf("--screen-height") + 1), mode.second);
			QCOMPARE(stream.at(stream.indexOf("--vblank-frequency") + 1),
							 QString("60"));
			QCOMPARE(output.value("KYTY_STREAM_WIDTH"), mode.first);
			QCOMPARE(output.value("KYTY_STREAM_HEIGHT"), mode.second);
			QCOMPARE(output.value("KYTY_STREAM_FPS"), QString("30"));
			QCOMPARE(output.value("KYTY_STREAM_ONLY"), QString("1"));
			QCOMPARE(local.at(1), QString("2560"));
			QCOMPARE(local.at(3), QString("1440"));
		}
	}
	void ordinaryTvGameStreamWrapperUsesNegotiatedProfile() {
		QTemporaryDir dir;
		const auto wrapper = QDir(QCoreApplication::applicationDirPath())
														 .filePath("libexec/kyty_stream_launch");
		const auto local = dir.filePath("local.ini");
		const QByteArray local_settings("resolution=2560x1440\n");
		QVERIFY(Put(local, local_settings));
		const StreamingLaunchEntry game{
				"profile",
				"Synthetic profile probe",
				"/usr/bin/python3",
				{"-c",
				 "import "
				 "os,json,sys;print(json.dumps({'args':sys.argv[1:],'width':os.getenv('"
				 "KYTY_STREAM_WIDTH'),'height':os.getenv('KYTY_STREAM_HEIGHT'),'fps':"
				 "os.getenv('KYTY_STREAM_FPS'),'headless':os.getenv('KYTY_STREAM_ONLY')"
				 "}))",
				 "--screen-width", "2560", "--screen-height", "1440"},
				dir.path()};
		auto manifest = StreamingCatalog::Manifest(
				{game}, dir.filePath("frames.sock"), "private",
				dir.filePath("game.lock"), "1440p", 60);
		manifest.insert("unified_stream_profile", true);
		manifest.insert("session_file", dir.filePath("session.json"));
		QVERIFY(AttachHostOwner(&manifest, dir.path()));
		QString error;
		QVERIFY(StreamingCatalog::WritePrivate(
				dir.filePath("launch.json"), QJsonDocument(manifest).toJson(), &error));
		for (const auto& mode: QList<QStringList>{{"1920", "1080", "60"},
		                                        {"2560", "1440", "30"}}) {
			QProcess process;
			HostEnvironment(&process, manifest);
			auto env = process.processEnvironment();
			env.insert("SUNSHINE_CLIENT_WIDTH", mode[0]);
			env.insert("SUNSHINE_CLIENT_HEIGHT", mode[1]);
			env.insert("SUNSHINE_CLIENT_FPS", mode[2]);
			env.insert("SUNSHINE_CLIENT_NAME", "Living-room TV");
			process.setProcessEnvironment(env);
			process.start(wrapper, {dir.filePath("launch.json"), "profile"});
			QVERIFY(process.waitForFinished(5000));
			const auto diagnostic = process.readAllStandardError();
			QVERIFY2(process.exitCode() == 0, diagnostic.constData());
			QVERIFY(diagnostic.contains("Kyty streaming profile: game=profile requested="));
			QVERIFY(diagnostic.contains("effective=" + mode[0].toUtf8() + "x" + mode[1].toUtf8() + "@" + mode[2].toUtf8()));
			QVERIFY(diagnostic.contains("local_profile=unchanged"));
			const auto result =
					QJsonDocument::fromJson(process.readAllStandardOutput()).object();
			QCOMPARE(result.value("width").toString(), mode[0]);
			QCOMPARE(result.value("height").toString(), mode[1]);
			QCOMPARE(result.value("fps").toString(), mode[2]);
			QCOMPARE(result.value("headless").toString(), QString("1"));
			const auto args = result.value("args").toArray();
			QCOMPARE(args, QJsonArray({"--screen-width", mode[0],
																 "--screen-height", mode[1]}));
			QCOMPARE(Read(local), local_settings);
			QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(dir.filePath("session.json")),
															 1000);
		}
	}
	void unifiedProfileMatchesNativeCaptureFrameRates() {
		for (const auto fps: {30, 60, 90, 120}) {
			QProcessEnvironment client;
			client.insert("SUNSHINE_CLIENT_WIDTH", "1920");
			client.insert("SUNSHINE_CLIENT_HEIGHT", "1080");
			client.insert("SUNSHINE_CLIENT_FPS", QString::number(fps));
			QStringList args;
			QProcessEnvironment output;
			QString error;
			QVERIFY(StreamingProfile::Apply(true, client, &args, &output, &error));
			QVERIFY(Streaming::IsCaptureFrameRateSupported(output.value("KYTY_STREAM_FPS").toUInt()));
			QCOMPARE(output.value("KYTY_STREAM_FPS"), QString::number(fps));
		}
	}
	void unifiedProfileRejectsInvalidOrMissingModeWithoutMutation() {
		for (const auto &mode : QList<QStringList>{{},
		                                           {"1921", "1080", "60"},  // odd width
		                                           {"1920", "1081", "60"},  // odd height
		                                           {"0", "0", "60"},        // zero
		                                           {"640", "358", "60"},    // below min height
		                                           {"3842", "1080", "60"},  // above max width
		                                           {"1920", "1080", "999"}, // unsupported fps
		                                           {"1920bad", "1080", "60"}}) {
			QProcessEnvironment client;
			if (mode.size() == 3) {
				client.insert("SUNSHINE_CLIENT_WIDTH", mode[0]);
				client.insert("SUNSHINE_CLIENT_HEIGHT", mode[1]);
				client.insert("SUNSHINE_CLIENT_FPS", mode[2]);
			}
			QStringList stream{"--screen-width", "2560"};
			QProcessEnvironment output;
			output.insert("KYTY_STREAM_WIDTH", "2560");
			QString error;
			QVERIFY(!StreamingProfile::Apply(true, client, &stream, &output, &error));
			QVERIFY(!error.isEmpty());
			QCOMPARE(stream, QStringList({"--screen-width", "2560"}));
			QCOMPARE(output.value("KYTY_STREAM_WIDTH"), QString("2560"));
		}
		QStringList args{"--screen-width", "2560", "--screen-height", "1440"};
		QProcessEnvironment untouched;
		QString error;
		QVERIFY(StreamingProfile::Apply(false, {}, &args, &untouched, &error));
		QCOMPARE(args.at(1), QString("2560"));
		QVERIFY(untouched.isEmpty());
	}
	void unifiedProfileAcceptsNonSixteenNineAndCarriesAspect() {
		// A 20:9 phone panel (2412 is odd, so the host negotiates the even 2412->2412? keep even):
		// verify a genuinely non-16:9 even mode is accepted and the aspect mode is propagated.
		struct Case { const char* w; const char* h; const char* in; const char* out; };
		const Case cases[] = {
		    {"2412", "1080", "", "fit"},        // default when unset
		    {"2412", "1080", "fill", "fill"},   // explicit fill
		    {"2560", "1080", "stretch", "stretch"},
		    {"1080", "1920", "bogus", "fit"},   // portrait (within bounds), unknown -> fit
		};
		for (const auto& c : cases) {
			QProcessEnvironment client;
			client.insert("SUNSHINE_CLIENT_WIDTH", c.w);
			client.insert("SUNSHINE_CLIENT_HEIGHT", c.h);
			client.insert("SUNSHINE_CLIENT_FPS", "60");
			QStringList args;
			QProcessEnvironment output;
			if (*c.in) output.insert("KYTY_STREAM_ASPECT", c.in);
			QString error;
			QVERIFY2(StreamingProfile::Apply(true, client, &args, &output, &error),
			         qPrintable(error));
			QCOMPARE(output.value("KYTY_STREAM_WIDTH"), QString(c.w));
			QCOMPARE(output.value("KYTY_STREAM_HEIGHT"), QString(c.h));
			QCOMPARE(output.value("KYTY_STREAM_ASPECT"), QString(c.out));
			QCOMPARE(args.at(args.indexOf("--screen-width") + 1), QString(c.w));
			QCOMPARE(args.at(args.indexOf("--screen-height") + 1), QString(c.h));
		}
	}
	void nativeProfileControlsAreAvailable() {
		QTemporaryDir      dir;
		HostControlClient  client(dir.filePath("none/control.sock"));
		client.SetAutostartDirectory(dir.path());
		StreamingWidget widget(&client);
		QVERIFY(!widget.findChild<QComboBox*>("streamResolution")->isHidden());
		QVERIFY(!widget.findChild<QComboBox*>("streamFps")->isHidden());
	}
	void controllerAudioReceiverApiUsesPinnedRoutes() {
		QTemporaryDir dir;
		QVERIFY(GenerateCertificate(dir.path()));
		LocalTlsServer server;
		server.certificate = QSslCertificate(Read(dir.filePath("cert.pem")));
		server.key         = QSslKey(Read(dir.filePath("key.pem")), QSsl::Rsa);
		QVERIFY(server.listen(QHostAddress::LocalHost));
		StreamingApi api;
		api.Configure(server.serverPort(), server.certificate, "fixture", "private");
		int        completed = 0;
		QString    error;
		const auto done = [&](const QJsonObject&, const QString& e) {
			error = e;
			++completed;
		};
		api.ListControllerAudioReceivers(done);
		QTRY_COMPARE_WITH_TIMEOUT(completed, 1, 3000);
		QVERIFY2(error.isEmpty(), qPrintable(error));
		const auto pem = QString::fromUtf8(server.certificate.toPem());
		api.RegisterControllerAudioReceiver("paired-client", "Thinker", pem, done);
		QTRY_COMPARE_WITH_TIMEOUT(completed, 2, 3000);
		QVERIFY2(error.isEmpty(), qPrintable(error));
		const QString id(64, 'a');
		api.SetControllerAudioReceiverEnabled(id, false, done);
		QTRY_COMPARE_WITH_TIMEOUT(completed, 3, 3000);
		QVERIFY2(error.isEmpty(), qPrintable(error));
		api.RemoveControllerAudioReceiver(id, done);
		QTRY_COMPARE_WITH_TIMEOUT(completed, 4, 3000);
		QVERIFY2(error.isEmpty(), qPrintable(error));
		QCOMPARE(server.requests.size(), 4);
		QCOMPARE(server.requests[0].value("line").toString(),
		         QString("GET /api/kyty/controller-audio/receivers HTTP/1.1"));
		QCOMPARE(server.requests[1].value("line").toString(),
		         QString("POST /api/kyty/controller-audio/receivers HTTP/1.1"));
		QCOMPARE(server.requests[1].value("body").toObject(),
		         (QJsonObject {{"parent_client_id", "paired-client"},
		                       {"name", "Thinker"},
		                       {"certificate_pem", pem}}));
		QCOMPARE(server.requests[2].value("line").toString(),
		         "PUT /api/kyty/controller-audio/receivers/" + id + " HTTP/1.1");
		QCOMPARE(server.requests[2].value("body").toObject(), (QJsonObject {{"enabled", false}}));
		QCOMPARE(server.requests[3].value("line").toString(),
		         "DELETE /api/kyty/controller-audio/receivers/" + id + " HTTP/1.1");
		api.RemoveControllerAudioReceiver("../clients/unpair", done);
		QCOMPARE(completed, 5);
		QVERIFY(!error.isEmpty());
		api.RegisterControllerAudioReceiver("paired-client", "Thinker", "not a certificate", done);
		QCOMPARE(completed, 6);
		QVERIFY(!error.isEmpty());
		api.RegisterControllerAudioReceiver("paired-client", QString(23, QChar(0x65e5)), pem, done);
		QCOMPARE(completed, 7);
		QVERIFY(!error.isEmpty());
		QCOMPARE(server.requests.size(), 4);
	}
	void qualityPreferencesSurviveUnrelatedSave_data() {
		QTest::addColumn<int>("fps");
		for (int fps: {30, 60, 90, 120, 999}) QTest::newRow(qPrintable(QString::number(fps))) << fps;
	}
	void qualityPreferencesSurviveUnrelatedSave() {
		QFETCH(int, fps);
		QTemporaryDir dir;
		FakeHostDaemon fake(dir.path());
		fake.preferences.insert("fps", fps);
		fake.preferences.insert("resolution", "Native");
		HostControlClient client(fake.Path());
		client.SetAutostartDirectory(dir.path());
		StreamingWidget widget(&client);
		auto* rate = widget.findChild<QComboBox*>("streamFps");
		auto* resolution = widget.findChild<QComboBox*>("streamResolution");
		auto* local = widget.findChild<QCheckBox*>("streamShowLocal");
		QVERIFY(rate && resolution && local);
		QTRY_VERIFY(!fake.Requests("get-preferences").isEmpty());
		const int expected = fps == 999 ? 60 : fps;
		QTRY_COMPARE(rate->currentData().toInt(), expected);
		QCOMPARE(resolution->currentText(), QString("Default (1080p)"));
		QCOMPARE(widget.Settings().resolution, QString("Native"));
		local->setChecked(!local->isChecked());
		QTRY_VERIFY(!fake.Requests("set-preferences").isEmpty());
		QCOMPARE(widget.Settings().fps, expected);
		QCOMPARE(fake.preferences.value("fps").toInt(), fps);
		QCOMPARE(fake.preferences.value("resolution").toString(), QString("Native"));
	}
	void streamingOnlyDisplayRoundTripsThroughDaemonPreferences() {
		QTemporaryDir dir;
		FakeHostDaemon fake(dir.path());
		HostControlClient client(fake.Path());
		client.SetAutostartDirectory(dir.path());
		StreamingWidget widget(&client);
		auto* local = widget.findChild<QCheckBox*>("streamShowLocal");
		QVERIFY2(local, "Streaming-only needs an explicit local-display control.");
		QTRY_VERIFY(!fake.Requests("get-preferences").isEmpty());
		QTRY_VERIFY(local->isChecked()); // the fake daemon reports show_local = true
		local->setChecked(false); // saved automatically, no Apply button
		QTRY_VERIFY(!fake.Requests("set-preferences").isEmpty());
		QVERIFY(fake.Requests("set-preferences").last().contains("show_local"));
		QVERIFY(!fake.Requests("set-preferences").last().value("show_local").toBool());
		QVERIFY(!fake.preferences.value("show_local").toBool());
		HostControlClient restored_client(fake.Path());
		restored_client.SetAutostartDirectory(dir.path());
		StreamingWidget restored(&restored_client);
		auto*           restored_local = restored.findChild<QCheckBox*>("streamShowLocal");
		QVERIFY(restored_local);
		QTRY_VERIFY(!restored_local->isChecked());
		const auto stream_only =
		    StreamingCatalog::Manifest({}, "/private/frames.sock", "private", "/private/game.lock",
		                               "1080p", 60, restored.Settings().show_local);
		QCOMPARE(stream_only.value("environment").toObject().value("KYTY_STREAM_ONLY").toString(),
		         QString("1"));
		local->setChecked(true);
		const auto local_display =
		    StreamingCatalog::Manifest({}, "/private/frames.sock", "private", "/private/game.lock",
		                               "1080p", 60, widget.Settings().show_local);
		QVERIFY(!local_display.value("environment").toObject().contains("KYTY_STREAM_ONLY"));
	}
	void mainTabReconcilesEditsDuringSave_data() {
		QTest::addColumn<int>("first_result"); // 0 success, 1 rejected reload, 2 failed reply after apply
		QTest::addColumn<bool>("revert");
		QTest::newRow("successful-save-revert") << 0 << true;
		QTest::newRow("rejected-reload-revert") << 1 << true;
		QTest::newRow("failed-reply-after-apply-revert") << 2 << true;
		QTest::newRow("unchanged-failure-no-retry-loop") << 2 << false;
	}
	void mainTabReconcilesEditsDuringSave() {
		QFETCH(int, first_result);
		QFETCH(bool, revert);
		const auto binary = qEnvironmentVariable("AGS_TEST_HOST_BINARY");
		if (binary.isEmpty()) QSKIP("Set AGS_TEST_HOST_BINARY to check the native configured DTO.");
		QTemporaryDir dir;
		StreamingBackend backend(dir.filePath("profile"));
		backend.SetApiSocket(dir.filePath("api.sock"));
		StreamingSettings nativeSettings;
		nativeSettings.backend = binary;
		nativeSettings.port = FreeBasePort();
		backend.Start(nativeSettings);
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Ready, 10000);
		FakeHostDaemon daemon(dir.path());
		daemon.preferences["port"] = nativeSettings.port;
		std::function<void()> releaseFirst;
		int writes = 0;
		daemon.apply_preferences = [&](const QJsonObject& preferences, std::function<void(HostReply)> done) {
			const bool first = ++writes == 1;
			const auto finish = [&, first, done](QJsonObject reply, QString error) {
				if (!error.isEmpty()) reply = {{"ok", false}, {"error", error}};
				if (first && first_result != 0) reply = {{"ok", false}, {"error", "fixture preference failure"}};
				auto respond = [done, reply] { done({200, "application/json", QJsonDocument(reply).toJson(QJsonDocument::Compact), {}}); };
				if (first) releaseFirst = std::move(respond); else respond();
			};
			if (first && first_result == 1) { finish({}, {}); return; }
			nativeSettings.bitrate = preferences.value("bitrate").toInt();
			backend.ReloadSettings(nativeSettings, finish);
		};
		HostControlClient client(daemon.Path());
		client.SetAutostartDirectory(dir.path());
		StreamingWidget widget(&client);
		auto* bitrate = widget.findChild<QSpinBox*>("streamBitrate");
		QTRY_COMPARE(widget.findChild<QSpinBox*>("streamPort")->value(), nativeSettings.port);
		for (auto* timer: client.findChildren<QTimer*>()) timer->stop();
		QCOMPARE(bitrate->value(), 30);
		bitrate->setValue(60);
		QTRY_VERIFY(bool(releaseFirst));
		QCOMPARE(daemon.Requests("set-preferences").size(), 1);
		QCOMPARE(daemon.preferences.value("bitrate").toInt(), 60);
		QCOMPARE(backend.NativeStatus().value("configured").toObject().value("video").toObject().value("bitrate_kbps").toInt(), first_result == 1 ? 30000 : 60000);
		if (revert) bitrate->setValue(30);
		QTest::qWait(750); // second debounce expires while acknowledgement is still withheld
		QCOMPARE(daemon.Requests("set-preferences").size(), 1);
		releaseFirst();
		releaseFirst = {};
		if (revert) {
			QTRY_COMPARE(daemon.Requests("set-preferences").size(), 2);
			QTRY_COMPARE(daemon.preferences.value("bitrate").toInt(), 30);
			QTRY_COMPARE(backend.NativeStatus().value("configured").toObject().value("video").toObject().value("bitrate_kbps").toInt(), 30000);
			QCOMPARE(bitrate->value(), 30);
			QTest::qWait(750);
			QCOMPARE(daemon.Requests("set-preferences").size(), 2);
		} else {
			QTRY_VERIFY(widget.findChild<QLabel*>("streamError")->text().contains("fixture preference failure"));
			QTest::qWait(1400); // a failed write with no newer edits must not retry itself
			QCOMPARE(daemon.Requests("set-preferences").size(), 1);
			QCOMPARE(bitrate->value(), 60);
			bitrate->setValue(45); // a later explicit edit may try again
			QTRY_COMPARE(daemon.Requests("set-preferences").size(), 2);
			QTRY_COMPARE(backend.NativeStatus().value("configured").toObject().value("video").toObject().value("bitrate_kbps").toInt(), 45000);
		}
		backend.Stop();
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Stopped, 10000);
	}

	void diagnosticChangesSurviveImmediateMainTabEdit() {
		const auto binary = qEnvironmentVariable("AGS_TEST_HOST_BINARY");
		if (binary.isEmpty()) QSKIP("Set AGS_TEST_HOST_BINARY to check the native configured DTO.");
		QTemporaryDir dir;
		StreamingBackend backend(dir.filePath("profile"));
		backend.SetApiSocket(dir.filePath("api.sock"));
		StreamingSettings nativeSettings;
		nativeSettings.backend = binary;
		nativeSettings.port = FreeBasePort();
		backend.Start(nativeSettings);
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Ready, 10000);
		FakeHostDaemon daemon(dir.path());
		daemon.preferences["port"] = nativeSettings.port;
		daemon.preferences["show_local"] = false;
		daemon.preferences["fec_enabled"] = false;
		daemon.apply_preferences = [&](const QJsonObject& preferences, std::function<void(HostReply)> done) {
			nativeSettings.show_local = preferences.value("show_local").toBool();
			nativeSettings.fec_enabled = preferences.value("fec_enabled").toBool();
			nativeSettings.bitrate = preferences.value("bitrate").toInt();
			backend.ReloadSettings(nativeSettings, [done](QJsonObject reply, QString error) {
				if (!error.isEmpty()) reply = {{"ok", false}, {"error", error}};
				done({200, "application/json", QJsonDocument(reply).toJson(QJsonDocument::Compact), {}});
			});
		};
		HostControlClient client(daemon.Path());
		client.SetAutostartDirectory(dir.path());
		StreamingWidget widget(&client);
		auto* bitrate = widget.findChild<QSpinBox*>("streamBitrate");
		QTRY_COMPARE(widget.findChild<QSpinBox*>("streamPort")->value(), nativeSettings.port);
		QVERIFY(!widget.findChild<QCheckBox*>("streamShowLocal")->isChecked());
		QVERIFY(!widget.findChild<QCheckBox*>("streamFec")->isChecked());
		// No routine status poll may repair stale main-tab state for this regression.
		for (auto* timer: client.findChildren<QTimer*>()) timer->stop();
		QTimer interaction;
		QElapsedTimer deadline;
		deadline.start();
		int stage = 0;
		bool accepted = false;
		connect(&interaction, &QTimer::timeout, &widget, [&] {
			auto* dialog = widget.findChild<QDialog*>();
			if (!dialog) return;
			if (deadline.elapsed() > 5000) { dialog->reject(); return; }
			auto* save = dialog->findChild<QPushButton*>("rustSaveSettings");
			if (stage == 0 && save->isEnabled()) {
				dialog->findChild<QComboBox*>("rustPresentation")->setCurrentIndex(1);
				dialog->findChild<QCheckBox*>("rustFec")->setChecked(true);
				save->click();
				stage = 1;
			} else if (stage == 1 && save->isEnabled()) {
				accepted = dialog->findChild<QLabel*>("rustStatus")->text().contains("Settings accepted");
				// Make a return-triggered refresh slower than the 600 ms autosave.
				// Its old bitrate reply must not undo the user's immediate next edit.
				daemon.preference_reply_delay_ms = 1100;
				dialog->accept();
			}
		});
		interaction.start(10);
		widget.findChild<QPushButton*>("agsRustStreamingControls")->click();
		interaction.stop();
		QVERIFY(accepted);
		QVERIFY(daemon.preferences.value("show_local").toBool());
		QVERIFY(daemon.preferences.value("fec_enabled").toBool());
		// Synchronous edit immediately after modal return, before any refresh can finish.
		bitrate->setValue(61);
		QTRY_COMPARE(daemon.Requests("set-preferences").size(), 2);
		QTRY_COMPARE(backend.NativeStatus().value("configured").toObject().value("video").toObject().value("bitrate_kbps").toInt(), 61000);
		QVERIFY(daemon.preferences.value("show_local").toBool());
		QVERIFY(daemon.preferences.value("fec_enabled").toBool());
		const auto configured = backend.NativeStatus().value("configured").toObject();
		QCOMPARE(configured.value("presentation").toString(), "mirrored");
		QVERIFY(configured.value("fec_enabled").toBool());
		const auto mainSave = daemon.Requests("set-preferences").last();
		QVERIFY(!mainSave.contains("show_local"));
		QVERIFY(!mainSave.contains("fec_enabled"));
		QTRY_VERIFY(widget.findChild<QCheckBox*>("streamShowLocal")->isChecked());
		QTRY_VERIFY(widget.findChild<QCheckBox*>("streamFec")->isChecked());
		QCOMPARE(bitrate->value(), 61);
		QTest::qWait(700); // applying the refresh must not schedule another autosave
		QCOMPARE(daemon.Requests("set-preferences").size(), 2);
		QCOMPARE(bitrate->value(), 61);
		backend.Stop();
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Stopped, 10000);
	}

	void widgetPreferencesLoadEditAndApplyThroughDaemon() {
		QTemporaryDir  dir;
		FakeHostDaemon fake(dir.path());
		fake.preferences = {{"port", 48300},      {"resolution", "1080p"}, {"fps", 30},
		                    {"bitrate", 45},      {"codec", "H264"},       {"adapter", "RTX 3090"},
		                    {"local_audio", false}, {"show_local", false}};
		HostControlClient client(fake.Path());
		client.SetAutostartDirectory(dir.path());
		StreamingWidget widget(&client);
		auto*           port    = widget.findChild<QSpinBox*>("streamPort");
		auto*           bitrate = widget.findChild<QSpinBox*>("streamBitrate");
		auto*           codec   = widget.findChild<QComboBox*>("streamCodec");
		QTRY_COMPARE(port->value(), 48300);
		QCOMPARE(widget.findChild<QComboBox*>("streamResolution")->currentText(), QString("1080p"));
		QCOMPARE(widget.findChild<QComboBox*>("streamFps")->currentData().toInt(), 30);
		QCOMPARE(bitrate->value(), 45);
		QCOMPARE(codec->currentText(), QString("H264"));
		QCOMPARE(widget.findChild<QLineEdit*>("streamAdapter")->text(), QString("RTX 3090"));
		QVERIFY(!widget.findChild<QCheckBox*>("streamLocalAudio")->isChecked());
		port->setValue(48400);
		bitrate->setValue(60);
		codec->setCurrentText("HEVC");
		widget.findChild<QCheckBox*>("streamLocalAudio")->setChecked(true);
		QTest::qWait(100);
		QVERIFY2(fake.Requests("set-preferences").isEmpty(), "edits are debounced into one save");
		QTRY_VERIFY(!fake.Requests("set-preferences").isEmpty());
		QTest::qWait(800);
		QCOMPARE(fake.Requests("set-preferences").size(), 1);
		const auto sent = fake.Requests("set-preferences").last();
		QCOMPARE(sent.value("port").toInt(), 48400);
		QVERIFY(!sent.contains("resolution"));
		QVERIFY(!sent.contains("fps"));
		QCOMPARE(fake.preferences.value("resolution").toString(), QString("1080p"));
		QCOMPARE(fake.preferences.value("fps").toInt(), 30);
		QCOMPARE(sent.value("bitrate").toInt(), 60);
		QCOMPARE(sent.value("codec").toString(), QString("HEVC"));
		QVERIFY(!sent.contains("adapter"));
		QCOMPARE(fake.preferences.value("adapter").toString(), QString("RTX 3090"));
		QVERIFY(sent.value("local_audio").toBool());
		QVERIFY(!sent.contains("show_local"));
		QVERIFY(!fake.preferences.value("show_local").toBool());
		HostControlClient restored_client(fake.Path());
		restored_client.SetAutostartDirectory(dir.path());
		StreamingWidget restored(&restored_client);
		QTRY_COMPARE(restored.findChild<QSpinBox*>("streamPort")->value(), 48400);
		QCOMPARE(restored.findChild<QSpinBox*>("streamBitrate")->value(), 60);
		QCOMPARE(restored.findChild<QComboBox*>("streamCodec")->currentText(), QString("HEVC"));
	}
	void widgetAutostartUsesLoginFileAndDoesNotControlIndependentHosts() {
 QTemporaryDir dir;
 FakeHostDaemon fake(dir.path());
 HostControlClient client(fake.Path());
 client.SetAutostartDirectory(dir.path());
 QSignalSpy errors(&client, &HostControlClient::CommandFailed);
 StreamingWidget widget(&client);
 auto* autostart = widget.findChild<QCheckBox*>("streamAutostart");
 QVERIFY(autostart); QVERIFY(!autostart->isChecked());
 autostart->setChecked(true);
 QVERIFY(QFile::exists(dir.filePath("autostart/ags.desktop")));
 autostart->setChecked(false);
 QVERIFY(!QFile::exists(dir.filePath("autostart/ags.desktop")));
 client.StopDaemon(); QCOMPARE(errors.count(), 1);
 QVERIFY(SystemctlLog(dir.path()).isEmpty());
 }
	void widgetStartRestartsAnErroredDaemon() {
		QTemporaryDir  dir;
		FakeHostDaemon fake(dir.path());
		fake.status["state"] = "Error";
		fake.status["error"] = "boom";
		HostControlClient client(fake.Path());
		client.SetAutostartDirectory(dir.path());
		StreamingWidget widget(&client);
		auto*           start = widget.findChild<QPushButton*>("streamStart");
		QVERIFY(start);
		QTRY_VERIFY(widget.findChild<QLabel*>("streamStatus")->text().contains("error"));
		QVERIFY(start->isEnabled());
		QTest::mouseClick(start, Qt::LeftButton);
		QTRY_VERIFY(widget.findChild<QLabel*>("streamError")->text().contains("Open AGS"));
		QVERIFY(!SystemctlLog(dir.path()).contains("--user start kyty-host.service"));
	}
	void widgetShowsDaemonStatusAddressAndLists() {
		QTemporaryDir  dir;
		FakeHostDaemon fake(dir.path());
		HostControlClient client(fake.Path());
		client.SetAutostartDirectory(dir.path());
		StreamingWidget widget(&client);
		auto*           address = widget.findChild<QComboBox*>("streamLanAddress");
		// The first status arrives asynchronously; before it the widget lists local interfaces.
		QTRY_VERIFY(widget.findChild<QLabel*>("streamStatus")->text().contains("Hosting ready"));
		QCOMPARE(address->currentText(), QString("192.168.1.3:48189"));
		QCOMPARE(address->count(), 1);
		QVERIFY(!widget.findChild<QListWidget*>("streamPending"));
		QCOMPARE(widget.findChild<QListWidget*>("streamClients")->count(), 1);
		QVERIFY(widget.findChild<QPushButton*>("streamStop")->isEnabled());
		QVERIFY(!widget.findChild<QPushButton*>("streamStart")->isEnabled());
	}

    void widgetOwnedLocalGameStopAndTitle_data() {
        QTest::addColumn<QString>("state");
        QTest::addColumn<bool>("local");
        QTest::addColumn<bool>("owned");
        QTest::addColumn<bool>("remote");
        QTest::addColumn<bool>("canStop");
        for (const auto& state: {QString("Ready"), QString("Stopped"), QString("Starting"), QString("Error")})
            QTest::newRow(qPrintable("local-" + state)) << state << true << true << false << true;
        QTest::newRow("legacy-local-ready") << QString("Ready") << true << false << false << false;
        QTest::newRow("remote-ready") << QString("Ready") << false << false << true << true;
        QTest::newRow("remote-stopped") << QString("Stopped") << false << false << true << false;
        QTest::newRow("no-game") << QString("Ready") << false << false << false << false;
    }
    void widgetOwnedLocalGameStopAndTitle() {
        QFETCH(QString, state); QFETCH(bool, local); QFETCH(bool, owned);
        QFETCH(bool, remote); QFETCH(bool, canStop);
        QTemporaryDir dir;
        FakeHostDaemon daemon(dir.path());
        daemon.status["state"] = state;
        daemon.status["local_game_running"] = local;
        daemon.status["local_game_id"] = owned ? QString(64, 'a') : QString();
        daemon.status["local_game_title"] = owned ? QString("Local fixture") : QString();
        daemon.status["stream_game_running"] = remote;
        daemon.status["session"] = owned ? "Local: Local fixture" : remote ? "Remote fixture" : "No game running";
        daemon.status["error"] = state == "Error" ? "Local cleanup failed" : "";
        HostControlClient client(daemon.Path());
        client.SetAutostartDirectory(dir.path());
        StreamingWidget widget(&client);
        QSignalSpy status(&client, &HostControlClient::StatusChanged);
        QTRY_VERIFY(status.count() > 0);
        auto* stop = widget.findChild<QPushButton*>("streamStopGame");
        QCOMPARE(stop->isEnabled(), canStop);
        auto* session = widget.findChild<QLabel*>("streamSession");
        QVERIFY(session);
        if (owned) QCOMPARE(session->text(), QString("Local: Local fixture"));
        if (state == "Error") QCOMPARE(widget.findChild<QLabel*>("streamError")->text(), QString("Local cleanup failed"));
        stop->click();
        if (canStop) {
            QVERIFY(!stop->isEnabled());
            client.StatusChanged(daemon.status); // Polls cannot reenable a pending Stop.
            QVERIFY(!stop->isEnabled());
        }
        stop->click(); // Pending owner request cannot dispatch twice.
        if (canStop) {
            QTRY_COMPARE(daemon.Requests("stop-game").size(), 1);
        } else {
            QCOMPARE(daemon.Requests("stop-game").size(), 0);
        }
        client.StatusChanged({});
        QVERIFY(!stop->isEnabled());
        QCOMPARE(session->text(), QString("No managed stream session"));
    }
	void widgetPairsUnpairsAndStopsGameThroughDaemon() {
		QTemporaryDir dir;
		FakeHostDaemon daemon(dir.path());
		daemon.status["stream_game_running"] = true;
		HostControlClient client(daemon.Path());
		StreamingWidget widget(&client);
		auto* pair = widget.findChild<QPushButton*>("streamPair");
		QTRY_VERIFY(pair->isEnabled()); pair->click();
		QTRY_COMPARE(daemon.Requests("pair-ticket").size(), 1);
		QVERIFY(daemon.Requests("pairing-pin").isEmpty());
		QVERIFY(!widget.findChild<QLineEdit*>("streamPin"));
		QVERIFY(!widget.findChild<QLineEdit*>("streamAgsPassword"));
		auto* clients = widget.findChild<QListWidget*>("streamClients");
		QTRY_COMPARE(clients->count(), 1); clients->setCurrentRow(0);
		widget.findChild<QPushButton*>("streamUnpair")->click();
		QTRY_COMPARE(daemon.Requests("unpair").size(), 1);
		QCOMPARE(daemon.Requests("unpair").first().value("uuid").toString(), "client-1");
		widget.findChild<QPushButton*>("streamStopGame")->click();
		QTRY_COMPARE(daemon.Requests("stop-game").size(), 1);
	}

	void launchWrapperValidatesAndClearsStreamingOnly() {
		QTemporaryDir dir;
		const auto    wrapper =
		    QDir(QCoreApplication::applicationDirPath()).filePath("libexec/kyty_stream_launch");
		const StreamingLaunchEntry game {
		    "display",
		    "Display probe",
		    "/usr/bin/python3",
		    {"-c", "import os; print(os.getenv('KYTY_STREAM_ONLY','unset'))"},
		    dir.path()};
		auto manifest = StreamingCatalog::Manifest({game}, dir.filePath("frames.sock"), "private",
		                                           dir.filePath("game.lock"), "1080p", 60);
		manifest.insert("session_file", dir.filePath("session.json"));
		QVERIFY(AttachHostOwner(&manifest, dir.path()));
		for (const auto& value: QList<QString> {"1", "yes", ""}) {
			auto env = manifest.value("environment").toObject();
			if (value.isEmpty())
				env.remove("KYTY_STREAM_ONLY");
			else
				env.insert("KYTY_STREAM_ONLY", value);
			manifest.insert("environment", env);
			QString error;
			QVERIFY(StreamingCatalog::WritePrivate(dir.filePath("launch.json"),
			                                       QJsonDocument(manifest).toJson(), &error));
			QProcess launch;
			HostEnvironment(&launch, manifest);
			auto inherited = launch.processEnvironment();
			inherited.insert("KYTY_STREAM_ONLY", "1");
			launch.setProcessEnvironment(inherited);
			launch.start(wrapper, {dir.filePath("launch.json"), "display"});
			QVERIFY(launch.waitForFinished(5000));
			if (value == "yes") {
				QCOMPARE(launch.exitCode(), 1);
				QVERIFY(launch.readAllStandardError().contains("Invalid launch environment"));
			} else {
				QVERIFY2(
				    launch.exitCode() == 0,
				    qPrintable(value + ": " + QString::fromUtf8(launch.readAllStandardError())));
				QCOMPARE(launch.exitCode(), 0);
				QCOMPARE(launch.readAllStandardOutput().trimmed(),
				         value.isEmpty() ? QByteArray("unset") : QByteArray("1"));
				QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(dir.filePath("session.json")), 1000);
			}
		}
	}
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
	void wrapperRejectsDeadOrStaleHostBeforeLockOrGame_data() {
		QTest::addColumn<bool>("dead_host");
		QTest::newRow("host-already-dead") << true;
		QTest::newRow("stale-start-time") << false;
	}
	void wrapperRejectsDeadOrStaleHostBeforeLockOrGame() {
#ifdef __linux__
		QFETCH(bool, dead_host);
		QTemporaryDir dir;
		QProcess      owner;
		owner.start("/usr/bin/sleep", {"60"});
		QVERIFY(owner.waitForStarted(1000));
		StreamingProcess::Identity identity;
		QVERIFY(StreamingProcess::ReadIdentity(int(owner.processId()), &identity));
		const StreamingLaunchEntry game {
		    "dead", "Game", "/usr/bin/touch", {dir.filePath("game-was-launched")}, dir.path()};
		auto manifest = StreamingCatalog::Manifest({game}, dir.filePath("frames.sock"), "Private",
		                                           dir.filePath("game.lock"), "Native", 60);
		manifest.insert("session_file", dir.filePath("session.json"));
		QVERIFY(AttachHostOwner(&manifest, dir.path()));
		const QJsonObject record {
		    {"nonce", manifest.value("host_nonce")},
		    {"pid", qint64(identity.pid)},
		    {"start_time", QString::number(qulonglong(identity.start + (dead_host ? 0 : 1)))},
		    { "executable",
			  QString::fromLocal8Bit(identity.executable.c_str()) }};
		QString error;
		QVERIFY(StreamingCatalog::WritePrivate(dir.filePath("owner.json"),
		                                       QJsonDocument(record).toJson(), &error));
		QVERIFY(StreamingCatalog::WritePrivate(dir.filePath("launch.json"),
		                                       QJsonDocument(manifest).toJson(), &error));
		if (dead_host) {
			owner.terminate();
			QVERIFY(owner.waitForFinished(1000));
		}
		QProcess wrapper;
		HostEnvironment(&wrapper, manifest);
		wrapper.start(
		    QDir(QCoreApplication::applicationDirPath()).filePath("libexec/kyty_stream_launch"),
		    {dir.filePath("launch.json"), "dead"});
		QVERIFY(wrapper.waitForFinished(3000));
		QCOMPARE(wrapper.exitCode(), 1);
		QVERIFY(wrapper.readAllStandardError().contains("already exited or changed identity"));
		QVERIFY(!QFile::exists(dir.filePath("game-was-launched")));
		QVERIFY(!QFile::exists(dir.filePath("game.lock")));
		QVERIFY(!QFile::exists(dir.filePath("session.json")));
		if (!dead_host) {
			QVERIFY(ProcessRunning(owner.processId()));
			owner.terminate();
			QVERIFY(owner.waitForFinished(1000));
		}
#else
		QSKIP("Linux process supervision");
#endif
	}
	void backendReportsMissingHelperWithoutWritingHostConf() {
		QTemporaryDir dir;
		const auto    profile = dir.filePath("profile");
		QVERIFY(QDir().mkpath(profile));
		StreamingSettings settings;
		settings.backend = dir.filePath("no-such-helper");
		settings.port    = FreeBasePort();
		StreamingBackend backend(profile);
		backend.Start(settings);
		QCOMPARE(backend.GetState(), StreamingBackend::State::Error);
		QVERIFY(backend.Error().contains("unavailable"));
		QVERIFY(!QFile::exists(QDir(profile).filePath("host.conf")));
		QVERIFY(!QFile::exists(QDir(dir.path()).filePath("host.conf")));
	}
	void widgetApplyRejectsInvalidSettingsWithoutContactingDaemon() {
		QTemporaryDir  dir;
		FakeHostDaemon fake(dir.path());
		HostControlClient client(fake.Path());
		client.SetAutostartDirectory(dir.path());
		StreamingWidget widget(&client);
		QTRY_VERIFY(!fake.Requests("get-preferences").isEmpty());
		QTest::qWait(200); // let the stored preferences arrive
		auto* adapter = widget.findChild<QLineEdit*>("streamAdapter");
		adapter->setText(QStringLiteral("invalid\nadapter"));
		QTRY_VERIFY(widget.findChild<QLabel*>("streamError")->text().contains("line breaks"));
		QTest::qWait(50);
		QVERIFY(fake.Requests("set-preferences").isEmpty());
	}
	void widgetShowsDaemonNotRunningAndKeepsStartAvailable() {
		QTemporaryDir     dir;
		HostControlClient client(dir.filePath("none/control.sock")); // nothing listens here
		client.SetAutostartDirectory(dir.path());
		StreamingWidget widget(&client);
		widget.resize(940, 900);
		widget.show();
		auto* start = widget.findChild<QPushButton*>("streamStart");
		auto* stop  = widget.findChild<QPushButton*>("streamStop");
		QVERIFY(start && stop);
		QTRY_VERIFY(
		    widget.findChild<QLabel*>("streamStatus")->text().contains("Host daemon is not running"));
		QVERIFY(start->isEnabled());
		QVERIFY(!stop->isEnabled());
		QVERIFY(!widget.findChild<QPushButton*>("streamApply")); // preferences auto-save
		QVERIFY(widget.findChild<QLineEdit*>("streamBackendPath")->isHidden());
		QTest::mouseClick(start, Qt::LeftButton);
		QTRY_VERIFY(widget.findChild<QLabel*>("streamError")->text().contains("Open AGS"));
		QTest::qWait(100); // let the fake systemctl exit before the client is destroyed
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
		QVERIFY(AttachHostOwner(&manifest, dir.path()));
		QVERIFY(StreamingCatalog::WritePrivate(dir.filePath("launch.json"),
		                                       QJsonDocument(manifest).toJson(), &error));
		QProcess launch;
		HostEnvironment(&launch, manifest);
		launch.start(wrapper, {dir.filePath("launch.json"), "game1"});
		QVERIFY(launch.waitForFinished(5000));
		QCOMPARE(launch.exitCode(), 0);
		const auto output = QJsonDocument::fromJson(launch.readAllStandardOutput()).object();
		QCOMPARE(output.value("argv").toArray().at(0).toString(), tricky);
		QCOMPARE(output.value("audio").toString(), QString("Kyty_Private_Audio"));
		QCOMPARE(output.value("driver").toString(), QString("pulseaudio"));
		QCOMPARE(output.value("width").toString(), QString("2560"));
		QVERIFY(!QFile::exists(dir.filePath("NEVER")));
		QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(dir.filePath("session.json")), 1000);
		game.program   = "/usr/bin/sleep";
		game.arguments = {"10"};
		manifest       = StreamingCatalog::Manifest({game}, dir.filePath("frames.sock"), "private",
		                                            dir.filePath("game.lock"), "Native", 30);
		manifest.insert("session_file", dir.filePath("session.json"));
		QVERIFY(AttachHostOwner(&manifest, dir.path()));
		QVERIFY(StreamingCatalog::WritePrivate(dir.filePath("launch.json"),
		                                       QJsonDocument(manifest).toJson(), &error));
		QProcess first;
		HostEnvironment(&first, manifest);
		first.start(wrapper, {dir.filePath("launch.json"), "game1"});
		QVERIFY(first.waitForStarted(1000));
		QTRY_VERIFY(QFile::exists(dir.filePath("session.json")));
		QProcess second;
		HostEnvironment(&second, manifest);
		second.start(wrapper, {dir.filePath("launch.json"), "game1"});
		QVERIFY(second.waitForFinished(2000));
		QCOMPARE(second.exitCode(), 1);
		QVERIFY(second.readAllStandardError().contains("already running"));
		first.terminate();
		QVERIFY(first.waitForFinished(5000));
		QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(dir.filePath("session.json")), 1000);
	}
	void launchWrapperAllowsGuardToFinishCaptureMetadata_data() {
		QTest::addColumn<bool>("stop_group");
		QTest::newRow("wrapper-stop") << false;
#ifdef __linux__
		QTest::newRow("whole-group-stop") << true;
#endif
	}
	void launchWrapperAllowsGuardToFinishCaptureMetadata() {
		QFETCH(bool, stop_group);
		QTemporaryDir dir;
		const auto    wrapper =
		    QDir(QCoreApplication::applicationDirPath()).filePath("libexec/kyty_stream_launch");
		QVERIFY(QFileInfo(wrapper).isExecutable());
		const auto guard = dir.filePath("guard.py");
		const auto ended = dir.filePath("ended");
		const auto ready = dir.filePath("guard-ready");
		const auto stop_log = dir.filePath("guard-signals");
		QVERIFY(Put(guard, R"PY(import pathlib, signal, sys, time

def stop(signum, frame):
    with pathlib.Path(sys.argv[3]).open("a") as log:
        log.write("stop\n")
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
		    "guard", "Astro", "/usr/bin/python3", {guard, ended, ready, stop_log}, dir.path()};
		auto manifest =
		    StreamingCatalog::Manifest({game}, dir.filePath("frames.sock"), "Kyty_Private_Audio",
		                               dir.filePath("game.lock"), "Native", 60);
		manifest.insert("session_file", dir.filePath("session.json"));
		QString error;
		QVERIFY(AttachHostOwner(&manifest, dir.path()));
		QVERIFY(StreamingCatalog::WritePrivate(dir.filePath("launch.json"),
		                                       QJsonDocument(manifest).toJson(), &error));
		QProcess launch;
		HostEnvironment(&launch, manifest);
		launch.start(wrapper, {dir.filePath("launch.json"), "guard"});
		QVERIFY(launch.waitForStarted(1000));
		QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(ready), 3000);
#ifdef __linux__
		if (stop_group) {
			const auto pid = pid_t(launch.processId());
			QVERIFY(pid > 1 && ::getpgid(pid) == pid);
			QCOMPARE(::kill(-pid, SIGTERM), 0);
		} else
#endif
			launch.terminate();
		QVERIFY(launch.waitForFinished(7000));
		QCOMPARE(Read(ended), QByteArray("capture complete"));
		QCOMPARE(Read(stop_log), QByteArray("stop\n"));
		QCOMPARE(launch.exitCode(), 0);
		QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(dir.filePath("session.json")), 1000);
	}
	void guardianDoesNotRebroadcastGroupStop_data() {
		QTest::addColumn<int>("group_signal");
		QTest::addColumn<bool>("queue_while_blocked");
#ifdef __linux__
		QTest::newRow("observed-SIGTERM") << int(SIGTERM) << false;
		QTest::newRow("observed-SIGINT") << int(SIGINT) << false;
		QTest::newRow("pending-SIGINT") << int(SIGINT) << true;
#endif
	}
	void guardianDoesNotRebroadcastGroupStop() {
#ifdef __linux__
		QFETCH(int, group_signal);
		QFETCH(bool, queue_while_blocked);
		QTemporaryDir dir;
		const auto wrapper =
		    QDir(QCoreApplication::applicationDirPath()).filePath("libexec/kyty_stream_launch");
		QVERIFY(QFileInfo(wrapper).isExecutable());
		const auto fixture = dir.filePath("guardian-race.py");
		// Trace the real guardian at the signal handler's return syscall. Queue
		// the private request after the group signal was handled, with no chance
		// for the shutdown decision to run between them. No timing sleeps or
		// production test hooks are needed to expose the overwritten observation.
		QVERIFY(Put(fixture, R"PY(import ctypes, errno, os, platform, signal, struct, sys

libc = ctypes.CDLL(None, use_errno=True)
libc.ptrace.restype = ctypes.c_long
ATTACH, CONT, SYSCALL = 16, 7, 24
SETOPTIONS, GET_SYSCALL_INFO = 0x4200, 0x420e
syscalls = {"x86_64": (15, 14), "aarch64": (139, 135)}.get(platform.machine())
if syscalls is None:
    print("ptrace signal-return fixture requires x86_64 or aarch64")
    sys.exit(77)
sigreturn, sigprocmask = syscalls
pending_case = sys.argv[4] == "pending"

def trace(request, pid, address=0, data=0):
    result = libc.ptrace(request, pid, ctypes.c_void_p(address), ctypes.c_void_p(data))
    if result == -1:
        raise OSError(ctypes.get_errno(), "ptrace")
    return result

def stopped(pid, expected):
    waited, status = os.waitpid(pid, 0)
    assert waited == pid and os.WIFSTOPPED(status), (waited, status)
    assert os.WSTOPSIG(status) == expected, (os.WSTOPSIG(status), expected)

# A disposable group leader reserves an isolated fixture group. The controller
# remains outside it, so a broken guardian cannot signal the Qt test runner.
leader_ready, leader_ready_write = os.pipe()
release, release_write = os.pipe()
leader = os.fork()
if leader == 0:
    os.close(release_write)
    os.setpgid(0, 0)
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    os.write(leader_ready_write, b"R")
    os.read(release, 1)
    os._exit(0)

os.close(release)
assert os.read(leader_ready, 1) == b"R"
ready, ready_write = os.pipe()
owner = os.pidfd_open(os.getpid())
guardian = os.fork()
if guardian == 0:
    os.setpgid(0, leader)
    os.close(release_write)
    for fd in (owner, ready_write):
        os.set_inheritable(fd, True)
    os.execv(sys.argv[1], [sys.argv[1], "--guardian", str(leader), str(owner),
                          str(owner), str(ready_write), sys.argv[3]])

def timed_out(signum, frame):
    raise TimeoutError("guardian signal interleaving did not complete")

signal.signal(signal.SIGALRM, timed_out)
signal.alarm(8)
try:
    assert os.read(ready, 1) == b"R"
    try:
        trace(ATTACH, guardian)
    except OSError as error:
        if error.errno == errno.EPERM:
            print("ptrace denied by this environment")
            sys.exit(77)
        raise
    stopped(guardian, signal.SIGSTOP)
    trace(SETOPTIONS, guardian, data=1)  # TRACESYSGOOD
    group_signal = int(sys.argv[2])
    first_signal = signal.SIGUSR1 if pending_case else group_signal
    if pending_case:
        os.kill(guardian, first_signal)
    else:
        os.killpg(leader, first_signal)
    trace(CONT, guardian)
    stopped(guardian, first_signal)
    trace(SYSCALL, guardian, data=first_signal)
    stopped(guardian, signal.SIGTRAP | 0x80)
    info = ctypes.create_string_buffer(128)
    trace(GET_SYSCALL_INFO, guardian, len(info), ctypes.addressof(info))
    assert info.raw[0] == 1 and struct.unpack_from("Q", info.raw, 24)[0] == sigreturn
    if pending_case:
        # Stop just after the guardian blocks shutdown signals. The group INT
        # is pending, not observed by its handler; an erroneous TERM broadcast
        # would deliver both signals on unblocking (they cannot coalesce).
        while True:
            trace(SYSCALL, guardian)
            stopped(guardian, signal.SIGTRAP | 0x80)
            trace(GET_SYSCALL_INFO, guardian, len(info), ctypes.addressof(info))
            if info.raw[0] == 1 and struct.unpack_from("QQ", info.raw, 24) == (sigprocmask, 0):
                break
        trace(SYSCALL, guardian)
        stopped(guardian, signal.SIGTRAP | 0x80)
        os.killpg(leader, group_signal)
        os.kill(guardian, signal.SIGUSR1)
    else:
        # The first handler has written its state, but has not returned to main.
        os.kill(guardian, signal.SIGUSR1)
        trace(CONT, guardian)
        stopped(guardian, signal.SIGUSR1)
    # Let the only other group member exit; the guardian still reserves the
    # PGID and receives its own broadcast if the bug causes a second one.
    os.write(release_write, b"R")
    os.waitpid(leader, 0)
    leader = 0
    trace(CONT, guardian, data=0 if pending_case else signal.SIGUSR1)
    group_deliveries = 0
    while True:
        _, status = os.waitpid(guardian, 0)
        if os.WIFEXITED(status):
            assert os.WEXITSTATUS(status) == 0, status
            guardian = 0
            break
        assert os.WIFSTOPPED(status), status
        delivered = os.WSTOPSIG(status)
        assert delivered in (signal.SIGTERM, signal.SIGINT, signal.SIGUSR1), delivered
        group_deliveries += delivered != signal.SIGUSR1
        trace(CONT, guardian, data=delivered)
    duplicates = group_deliveries - int(pending_case)
    assert duplicates == 0, f"guardian rebroadcast shutdown {duplicates} time(s)"
    print("group shutdown survived private request; no rebroadcast")
finally:
    for child in (guardian, leader):
        if child:
            try:
                os.kill(child, signal.SIGKILL)
                os.waitpid(child, 0)
            except ProcessLookupError:
                pass
)PY"));
		QProcess race;
		race.start("/usr/bin/python3", {fixture, wrapper, QString::number(group_signal),
		                              dir.filePath("session.json"),
		                              queue_while_blocked ? "pending" : "observed"});
		QVERIFY(race.waitForFinished(10000));
		const auto output = race.readAllStandardOutput() + race.readAllStandardError();
		if (race.exitCode() == 77) QSKIP(output.constData());
		QCOMPARE(race.exitStatus(), QProcess::NormalExit);
		QVERIFY2(race.exitCode() == 0, output.constData());
#else
		QSKIP("Linux process supervision");
#endif
	}
	void portPreflightDetectsConflict() {
		const int base = FreeBasePort();
		QVERIFY(base > 0);
		QUdpSocket occupied;
		QVERIFY(occupied.bind(QHostAddress::Any, quint16(base), QUdpSocket::DontShareAddress));
		QVERIFY(StreamingBackend::PreflightPorts(base).contains(QString::number(base)));
	}

	void appsIncludeImagePath() {
		StreamingLaunchEntry entry {"id1", "Game", "/bin/emu", {"--game"}, "/bin", "/g/sce_sys/icon0.png"};
		const auto           apps = StreamingCatalog::Apps({entry}, "/w", "/m.json");
		QCOMPARE(apps["apps"].toArray()[0].toObject()["image-path"].toString(),
		         QStringLiteral("/g/sce_sys/icon0.png"));
		entry.image_path.clear();
		const auto plain = StreamingCatalog::Apps({entry}, "/w", "/m.json");
		QVERIFY(!plain["apps"].toArray()[0].toObject().contains("image-path"));
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
		s.bitrate = 151;
		QVERIFY(!s.Validate().isEmpty());
		s.bitrate = 5;
		s.port    = 65536;
		QVERIFY(!s.Validate().isEmpty());
		s.port = 48189;
		s.fps  = 121;
		QVERIFY(!s.Validate().isEmpty());
		s.fps        = 30;
		s.resolution = "4K";
		QVERIFY(!s.Validate().isEmpty());
		s.resolution = "1440p";
		s.codec      = "AV1";
		QVERIFY(s.Validate().isEmpty());
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
		s.audio_codec = "raw_pcm"; s.encoder = "software"; s.fec_enabled = true; s.local_audio_sink = "fixture-sink";
		s.Write(file);
		file.sync();
		const auto read = StreamingSettings::Read(file);
		QCOMPARE(read.backend, s.backend);
		QCOMPARE(read.audio_codec, s.audio_codec);
		QCOMPARE(read.encoder, s.encoder);
		QCOMPARE(read.fec_enabled, s.fec_enabled);
		QCOMPARE(read.local_audio_sink, s.local_audio_sink);
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

	void manifestAndWrapperPickClientGamesByUuid() {
#ifdef __linux__
		QTemporaryDir dir;
		const auto    wrapper =
		    QDir(QCoreApplication::applicationDirPath()).filePath("libexec/kyty_stream_launch");
		const QString uuid = "44444444-4444-4444-4444-444444444444";
		// PC and device entries differ only by a marker argument the program echoes back.
		const QString              marker_prog = "/usr/bin/python3";
		const StreamingLaunchEntry pc {"g1", "Game", marker_prog,
		                               {"-c", "import sys; print(sys.argv[1])", "PC"}, dir.path()};
		const StreamingLaunchEntry device {
		    "g1", "Game", marker_prog, {"-c", "import sys; print(sys.argv[1])", "DEVICE"},
		    dir.path()};
		QHash<QString, QList<StreamingLaunchEntry>> client_games;
		client_games.insert(uuid, {device});
		// client_games lands in the manifest keyed by uuid.
		auto manifest = StreamingCatalog::Manifest({pc}, dir.filePath("frames.sock"), "private",
		                                           dir.filePath("game.lock"), "1080p", 60, true,
		                                           client_games);
		QVERIFY(manifest.value("client_games").toObject().contains(uuid));
		QVERIFY(manifest.value("client_games")
		            .toObject()
		            .value(uuid)
		            .toObject()
		            .contains("g1"));
		manifest.insert("session_file", dir.filePath("session.json"));
		QVERIFY(AttachHostOwner(&manifest, dir.path()));
		QString error;
		QVERIFY(StreamingCatalog::WritePrivate(dir.filePath("launch.json"),
		                                       QJsonDocument(manifest).toJson(), &error));
		const auto run = [&](const QString& client_uuid) {
			QProcess launch;
			HostEnvironment(&launch, manifest);
			auto env = launch.processEnvironment();
			env.insert("KYTY_STREAM_ONLY", "1");
			if (!client_uuid.isEmpty())
				env.insert("SUNSHINE_CLIENT_UUID", client_uuid);
			else
				env.remove("SUNSHINE_CLIENT_UUID");
			launch.setProcessEnvironment(env);
			launch.start(wrapper, {dir.filePath("launch.json"), "g1"});
			if (!launch.waitForFinished(5000)) return QByteArray("timeout");
			// The wrapper's guardian outlives it briefly while it holds the stream lock; wait
			// for the lock so the next run is not refused as a second concurrent game.
			const auto lock_path = QFile::encodeName(dir.filePath("game.lock"));
			for (int i = 0; i < 300; ++i) {
				const int fd = ::open(lock_path.constData(), O_RDWR | O_CLOEXEC);
				const bool free = fd >= 0 && ::flock(fd, LOCK_EX | LOCK_NB) == 0;
				if (fd >= 0) ::close(fd);
				if (free) break;
				QThread::msleep(10);
			}
			return launch.readAllStandardOutput().trimmed();
		};
		// Matching uuid picks the device entry; absence (and an unknown uuid) falls back to PC.
		QCOMPARE(run(uuid), QByteArray("DEVICE"));
		QCOMPARE(run(QString()), QByteArray("PC"));
		QCOMPARE(run("99999999-9999-9999-9999-999999999999"), QByteArray("PC"));
#endif
	}
	void hostNameAndGraceValidation() {
		StreamingSettings s;
		QVERIFY(s.Validate().isEmpty()); // defaults: blank name, grace 20
		s.host_name = "  Trimmed name  ";
		QVERIFY(s.Validate().isEmpty());
		QCOMPARE(s.EffectiveHostName(), QString("Trimmed name"));
		s.host_name = QString(41, 'x'); // one over the 40-char ceiling
		QVERIFY(!s.Validate().isEmpty());
		s.host_name = "bad=name";
		QVERIFY(!s.Validate().isEmpty());
		s.host_name = "line\nbreak";
		QVERIFY(!s.Validate().isEmpty());
		s.host_name.clear();
		QCOMPARE(s.EffectiveHostName(), StreamingSettings::DefaultHostName());
		QVERIFY(s.Validate().isEmpty());
		s.disconnect_grace = 4; // every value in 0..600 is supported
		QVERIFY(s.Validate().isEmpty());
		s.disconnect_grace = 601;
		QVERIFY(!s.Validate().isEmpty());
		s.disconnect_grace = 0; // immediate is allowed at the config level
		QVERIFY(s.Validate().isEmpty());
		s.disconnect_grace = 600;
		QVERIFY(s.Validate().isEmpty());
	}
	void hostNameAndGraceRoundTripThroughDaemonPreferences() {
		QTemporaryDir     dir;
		FakeHostDaemon    fake(dir.path());
		fake.preferences  = FakeHostDaemon::DefaultPreferences();
		fake.preferences.insert("disconnect_grace", 30);
		fake.preferences.insert("host_name", "Studio");
		HostControlClient client(fake.Path());
		client.SetAutostartDirectory(dir.path());
		StreamingWidget widget(&client);
		auto*           grace = widget.findChild<QSpinBox*>("streamDisconnectGrace");
		auto*           name  = widget.findChild<QLineEdit*>("streamHostName");
		QVERIFY(grace);
		QVERIFY(name);
		QTRY_COMPARE(grace->value(), 30);
		QTRY_COMPARE(name->text(), QString("Studio"));
		grace->setValue(90);
		name->setText("Den");
		QTRY_VERIFY(!fake.Requests("set-preferences").isEmpty());
		QTRY_VERIFY(fake.preferences.value("disconnect_grace").toInt() == 90 &&
		            fake.preferences.value("host_name").toString() == "Den");
		const auto last = fake.Requests("set-preferences").last();
		QCOMPARE(last.value("disconnect_grace").toInt(), 90);
		QCOMPARE(last.value("host_name").toString(), QString("Den"));
	}
};
QTEST_MAIN(LauncherStreamingTests)
#include "LauncherStreamingTests.moc"
