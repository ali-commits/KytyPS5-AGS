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
#include <QtTest>

#include <cmath>
#include <cstring>
#ifdef __linux__
#include <signal.h>
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
private slots:
	void unifiedPhoneResolutionIsolatedFromLocalProfile() {
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
	void unifiedWrapperAppliesOneOutputSizeAtProcessBoundary() {
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
		for (const auto &size :
				 QList<QPair<QString, QString>>{{"1920", "1080"}, {"2560", "1440"}}) {
			QProcess process;
			HostEnvironment(&process, manifest);
			auto env = process.processEnvironment();
			env.insert("SUNSHINE_CLIENT_WIDTH", size.first);
			env.insert("SUNSHINE_CLIENT_HEIGHT", size.second);
			env.insert("SUNSHINE_CLIENT_FPS", "60");
			process.setProcessEnvironment(env);
			process.start(wrapper, {dir.filePath("launch.json"), "profile"});
			QVERIFY(process.waitForFinished(5000));
			const auto diagnostic = process.readAllStandardError();
			QVERIFY2(process.exitCode() == 0, diagnostic.constData());
			QVERIFY(diagnostic.contains("Kyty streaming profile: game=profile requested="));
			QVERIFY(diagnostic.contains("effective=" + size.first.toUtf8() + "x" + size.second.toUtf8() + "@60"));
			QVERIFY(diagnostic.contains("local_profile=unchanged"));
			const auto result =
					QJsonDocument::fromJson(process.readAllStandardOutput()).object();
			QCOMPARE(result.value("width").toString(), size.first);
			QCOMPARE(result.value("height").toString(), size.second);
			QCOMPARE(result.value("headless").toString(), QString("1"));
			const auto args = result.value("args").toArray();
			QCOMPARE(args, QJsonArray({"--screen-width", size.first,
																 "--screen-height", size.second}));
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
																							 {"1920", "1440", "60"},
																							 {"0", "0", "60"},
																							 {"1920", "1080", "999"},
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
	void controllerAudioLifecycleAndReceiverControls() {
		QTemporaryDir dir;
		QVERIFY(GenerateCertificate(dir.path()));
		const auto fixture = Read(QStringLiteral(KYTY_LAUNCHER_FIXTURE));
		for (const auto& name: {"pactl", "host"}) {
			QVERIFY(Put(dir.filePath(name), fixture));
			QVERIFY(QFile::setPermissions(dir.filePath(name),
			                              QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
		}
		const QString id(64, 'b');
		QVERIFY(Put(dir.filePath("controller-audio.json"),
		            QJsonDocument(QJsonObject {{"receivers", QJsonArray {QJsonObject {
		                                                         {"id", id},
		                                                         {"certificate_sha256", id},
		                                                         {"name", "Thinker"},
		                                                         {"parent_client_id", "client-1"},
		                                                         {"enabled", true}}}}})
		                .toJson()));
		FixtureEnvironment environment(dir.path());
		StreamingBackend   backend(dir.filePath("profile"));
		StreamingWidget    widget(&backend);
		auto* receivers = widget.findChild<QListWidget*>("streamControllerAudioReceivers");
		auto* status    = widget.findChild<QLabel*>("streamControllerAudioStatus");
		auto* toggle    = widget.findChild<QPushButton*>("streamControllerAudioToggle");
		auto* remove    = widget.findChild<QPushButton*>("streamControllerAudioRemove");
		QVERIFY(receivers && status && toggle && remove);
		StreamingSettings settings;
		settings.backend          = dir.filePath("host");
		settings.port             = FreeBasePort();
		settings.controller_audio = true;
		backend.Start(settings);
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Ready, 10000);
		const auto config = Read(dir.filePath("profile/host.conf"));
		QVERIFY(config.contains("kyty_controller_audio = true\n"));
		QVERIFY(config.contains("kyty_controller_audio_port = " +
		                        QByteArray::number(settings.ControllerAudioPort())));
		QVERIFY(config.contains(
		    "kyty_controller_audio_clients = " +
		    QFile::encodeName(dir.filePath("profile/controller-audio-clients.json"))));
		QTRY_COMPARE_WITH_TIMEOUT(receivers->count(), 1, 5000);
		QTRY_VERIFY_WITH_TIMEOUT(status->text().contains("waiting", Qt::CaseInsensitive), 5000);
		receivers->setCurrentRow(0);
		QVERIFY(toggle->isEnabled());
		toggle->click();
		QTRY_VERIFY_WITH_TIMEOUT(
		    Read(dir.filePath("api-requests.jsonl")).contains("\"enabled\": false"), 5000);
		QTRY_VERIFY_WITH_TIMEOUT(receivers->item(0)->text().contains("disabled"), 5000);
		remove->click();
		QTRY_COMPARE_WITH_TIMEOUT(receivers->count(), 0, 5000);
		backend.Stop();
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Stopped, 7000);
		QCOMPARE(QJsonDocument::fromJson(Read(dir.filePath("modules.json"))).array().size(), 0);
	}
	void controllerAudioStatusRejectsMalformedPinnedHostResponses() {
		QTemporaryDir dir;
		QVERIFY(GenerateCertificate(dir.path()));
		const auto fixture = Read(QStringLiteral(KYTY_LAUNCHER_FIXTURE));
		for (const auto& name: {"pactl", "host"}) {
			QVERIFY(Put(dir.filePath(name), fixture));
			QVERIFY(QFile::setPermissions(dir.filePath(name),
			                              QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
		}
		QVERIFY(Put(dir.filePath("controller-audio.json"), "{\"receivers\": []}"));
		QJsonObject valid {
		    {"configured", true},   {"state", "waiting"}, {"receiver_name", "Thinker"},
		    {"port", 65535},        {"bytes", 17},        {"queue_us", 250},
		    {"discontinuities", 0}, {"last_error", ""}};
		const auto status_file = dir.filePath("controller-audio-status.json");
		QVERIFY(Put(status_file, QJsonDocument(valid).toJson()));
		FixtureEnvironment environment(dir.path());
		StreamingBackend   backend(dir.filePath("profile"));
		StreamingSettings  settings;
		settings.backend          = dir.filePath("host");
		settings.port             = FreeBasePort();
		settings.controller_audio = true;
		backend.Start(settings);
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Ready, 10000);
		QTRY_COMPARE_WITH_TIMEOUT(backend.ControllerAudioStatus().value("state").toString(),
		                          QString("waiting"), 5000);
		const auto         cached = backend.ControllerAudioStatus();
		QList<QJsonObject> malformed {{{"configured", true}}};
		for (const auto& field: {"configured", "state", "receiver_name", "port", "bytes",
		                         "queue_us", "discontinuities", "last_error"}) {
			auto missing = valid;
			missing.remove(field);
			malformed.append(missing);
			for (const auto& bad:
			     {QJsonValue(), QJsonValue(QJsonObject {}), QJsonValue(QJsonArray {})}) {
				auto wrong_type = valid;
				wrong_type.insert(field, bad);
				malformed.append(wrong_type);
			}
		}
		const auto add_bad = [&](const QString& field, const QJsonValue& value) {
			auto status = valid;
			status.insert(field, value);
			malformed.append(status);
		};
		add_bad("configured", "true");
		add_bad("configured", 1);
		for (const auto& field: {"state", "receiver_name", "last_error"}) {
			add_bad(field, true);
			add_bad(field, 12);
		}
		add_bad("state", "");
		add_bad("state", "unknown");
		add_bad("state", QString(17, 'a'));
		add_bad("receiver_name", QString(22, QChar(0x65e5)));
		add_bad("last_error", QString(43, QChar(0x65e5)));
		for (const auto& field: {"bytes", "queue_us", "discontinuities"}) {
			add_bad(field, "17");
			add_bad(field, true);
			add_bad(field, -1);
			add_bad(field, 0.5);
			add_bad(field, 18446744073709551616.0);
		}
		for (const auto& bad: {QJsonValue("65535"), QJsonValue(true), QJsonValue(0), QJsonValue(-1),
		                       QJsonValue(65536), QJsonValue(1.5)})
			add_bad("port", bad);
		QSignalSpy logs(&backend, &StreamingBackend::LogAdded);
		for (int index = 0; index < malformed.size(); ++index) {
			QVERIFY(Put(status_file, QJsonDocument(malformed[index]).toJson()));
			const auto previous = logs.size();
			backend.Refresh();
			QTRY_VERIFY_WITH_TIMEOUT(logs.size() > previous, 5000);
			QVERIFY2(backend.Error().contains("invalid controller audio status"),
			         qPrintable(
			             QString("malformed status case %1: %2").arg(index).arg(backend.Error())));
			QCOMPARE(backend.ControllerAudioStatus(), cached);
		}
		for (const auto& state: {"preparing", "active", "error", "waiting"}) {
			valid.insert("state", state);
			valid.insert("configured", false);
			valid.insert("port", 1);
			valid.insert("receiver_name", QString(21, QChar(0x65e5)) + 'a');
			valid.insert("last_error", QString(42, QChar(0x65e5)) + "ab");
			valid.insert("bytes", 18446744073709549568.0);
			valid.insert("queue_us", 0);
			valid.insert("discontinuities", 18446744073709549568.0);
			valid.insert("unrelated", "never cache host extras");
			QVERIFY(Put(status_file, QJsonDocument(valid).toJson()));
			backend.Refresh();
			QTRY_COMPARE_WITH_TIMEOUT(backend.ControllerAudioStatus().value("state").toString(),
			                          QString(state), 5000);
			QCOMPARE(backend.ControllerAudioStatus(),
			         (QJsonObject {{"state", state},
			                       {"receiver_name", valid.value("receiver_name")},
			                       {"bytes", 18446744073709549568.0},
			                       {"queue_us", 0},
			                       {"last_error", valid.value("last_error")}}));
		}
		backend.Stop();
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Stopped, 7000);
		QVERIFY(backend.ControllerAudioStatus().isEmpty());
		QCOMPARE(QJsonDocument::fromJson(Read(dir.filePath("modules.json"))).array().size(), 0);
	}
	void controllerAudioPortConflictIsCaught() {
		const int  base = FreeBasePort();
		QTcpServer busy;
		QVERIFY(busy.listen(QHostAddress::Any, base + 30));
		QVERIFY(StreamingBackend::PreflightPorts(base).isEmpty());
		QVERIFY(!StreamingBackend::PreflightPorts(base, base + 30).isEmpty());
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
	void controllerAudioMissingCapabilityFailsBeforeAudioSetup() {
		QTemporaryDir dir;
		for (const auto& tool: {"pactl", "pipewire", "pipewire-pulse", "wireplumber"}) {
			const auto path = dir.filePath(tool);
			QVERIFY(Put(path, "#!/bin/sh\nexit 1\n"));
			QVERIFY(QFile::setPermissions(path,
			                              QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
		}
		FixtureEnvironment environment(dir.path());
		const auto         program = dir.filePath("host");
		QVERIFY(Put(
		    program,
		    "#!/bin/sh\nprintf '%s\\n' "
		    "'{\"protocol_version\":1,\"capture\":\"kyty\",\"gpu_transport\":\"vulkan-buffer-fd\","
		    "\"private_audio\":true,\"private_input\":true,\"credentials_stdin\":true}'\n"));
		QVERIFY(
		    QFile::setPermissions(program, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
		StreamingBackend  backend(dir.filePath("profile"));
		StreamingSettings settings;
		settings.backend = program;
		settings.port    = FreeBasePort();
		QVERIFY(settings.port > 0);
		settings.controller_audio = true;
		backend.Start(settings);
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Error, 5000);
		QVERIFY2(backend.Error().contains("controller audio", Qt::CaseInsensitive),
		         qPrintable(backend.Error()));
		QVERIFY(!QFile::exists(dir.filePath("profile/host.conf")));
	}
	void controllerAudioSettingsMigrateAndPersist() {
		QTemporaryDir    dir;
		StreamingBackend backend(dir.path());
		StreamingWidget  widget(&backend);
		auto*            enabled = widget.findChild<QCheckBox*>("streamControllerAudio");
		auto*            port    = widget.findChild<QSpinBox*>("streamControllerAudioPort");
		QVERIFY2(enabled, "Controller audio needs an explicit opt-in control.");
		QVERIFY(port);
		QVERIFY(!enabled->isChecked());
		QCOMPARE(port->value(), 0);
		enabled->setChecked(true);
		port->setValue(48219);
		QSettings saved(dir.filePath("streaming.ini"), QSettings::IniFormat);
		widget.Settings().Write(saved);
		saved.sync();
		QVERIFY(saved.value("controller_audio/enabled").toBool());
		QCOMPARE(saved.value("controller_audio/port").toInt(), 48219);
		StreamingWidget restored(&backend);
		QVERIFY(restored.findChild<QCheckBox*>("streamControllerAudio")->isChecked());
		QCOMPARE(restored.findChild<QSpinBox*>("streamControllerAudioPort")->value(), 48219);
		QVERIFY(restored.Settings().Validate().isEmpty());
		enabled->setChecked(false);
		widget.Settings().Write(saved);
		saved.sync();
		QVERIFY(!saved.value("controller_audio/enabled").toBool());
	}
	void controllerAudioPortValidationPreservesLegacy() {
		QTemporaryDir    dir;
		StreamingBackend backend(dir.path());
		StreamingWidget  widget(&backend);
		auto*            enabled    = widget.findChild<QCheckBox*>("streamControllerAudio");
		auto*            audio_port = widget.findChild<QSpinBox*>("streamControllerAudioPort");
		auto*            base       = widget.findChild<QSpinBox*>("streamPort");
		QVERIFY(enabled);
		QVERIFY(audio_port);
		QVERIFY(base);
		base->setValue(65514);
		QVERIFY(widget.Settings().Validate().isEmpty());
		enabled->setChecked(true);
		QVERIFY(!widget.Settings().Validate().isEmpty());
		audio_port->setValue(65000);
		QVERIFY(widget.Settings().Validate().isEmpty());
		base->setValue(48189);
		audio_port->setValue(48190);
		QVERIFY(!widget.Settings().Validate().isEmpty());
		audio_port->setValue(500);
		QVERIFY(!widget.Settings().Validate().isEmpty());
		enabled->setChecked(false);
		QVERIFY(widget.Settings().Validate().isEmpty());
	}
	void streamingOnlyDisplayMigratesAndPersists() {
		QTemporaryDir    dir;
		StreamingBackend backend(dir.path());
		StreamingWidget  widget(&backend);
		auto*            local = widget.findChild<QCheckBox*>("streamShowLocal");
		QVERIFY2(local, "Streaming-only needs an explicit local-display control.");
		QVERIFY(local->isChecked());
		QSettings saved(dir.filePath("streaming.ini"), QSettings::IniFormat);
		local->setChecked(false);
		widget.Settings().Write(saved);
		saved.sync();
		QVERIFY(saved.contains("display/show_local"));
		QVERIFY(!saved.value("display/show_local").toBool());
		StreamingWidget restored(&backend);
		auto*           restored_local = restored.findChild<QCheckBox*>("streamShowLocal");
		QVERIFY(restored_local);
		QVERIFY(!restored_local->isChecked());
		const auto stream_only =
		    StreamingCatalog::Manifest({}, "/private/frames.sock", "private", "/private/game.lock",
		                               "1080p", 60, restored.Settings().show_local);
		QCOMPARE(stream_only.value("environment").toObject().value("KYTY_STREAM_ONLY").toString(),
		         QString("1"));
		local->setChecked(true);
		widget.Settings().Write(saved);
		saved.sync();
		QVERIFY(saved.value("display/show_local").toBool());
		const auto local_display =
		    StreamingCatalog::Manifest({}, "/private/frames.sock", "private", "/private/game.lock",
		                               "1080p", 60, widget.Settings().show_local);
		QVERIFY(!local_display.value("environment").toObject().contains("KYTY_STREAM_ONLY"));
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
	void ownerDeathStopsWholeGameGroup_data() {
		QTest::addColumn<bool>("kill_wrapper");
		QTest::newRow("managed-host-sigkill") << false;
		QTest::newRow("wrapper-sigkill") << true;
	}
	void ownerDeathStopsWholeGameGroup() {
#ifdef __linux__
		QFETCH(bool, kill_wrapper);
		QTemporaryDir dir;
		QVERIFY(GenerateCertificate(dir.path()));
		const auto fixture = Read(QStringLiteral(KYTY_LAUNCHER_FIXTURE));
		for (const auto& name: {"host", "pactl"}) {
			QVERIFY(Put(dir.filePath(name), fixture));
			QFile::setPermissions(dir.filePath(name),
			                      QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
		}
		FixtureEnvironment environment(dir.path());
		const auto         guard     = dir.filePath("guard.py");
		const auto         pids_file = dir.filePath("game-pids.json");
		QVERIFY(Put(guard, R"PY(import json, os, pathlib, signal, subprocess, sys, time
signal.signal(signal.SIGTERM, signal.SIG_IGN)
child = subprocess.Popen(["/usr/bin/sleep", "60"])
pathlib.Path(sys.argv[1]).write_text(json.dumps({"guard":os.getpid(), "descendant":child.pid}))
while True: time.sleep(0.05)
)PY"));
		const auto wrapper =
		    QDir(QCoreApplication::applicationDirPath()).filePath("libexec/kyty_stream_launch");
		QVERIFY(Put(dir.filePath("launch-game.json"), QJsonDocument(QJsonObject {{
			                                              "wrapper",
			                                              wrapper
		                                              }}).toJson()));
		StreamingBackend backend(dir.filePath("profile"));
		FixtureGameGroup cleanup;
		backend.SetCatalog(
		    {{ "supervised",
			   "Game",
			   "/usr/bin/python3",
			   {guard, pids_file},
			   dir.path() }});
		StreamingSettings settings;
		settings.backend = dir.filePath("host");
		settings.port    = FreeBasePort();
		backend.Start(settings);
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Ready, 7000);
		QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(pids_file), 3000);
		cleanup.pid = QString::fromUtf8(Read(dir.filePath("wrapper-pid"))).trimmed().toLongLong();
		const auto pids       = QJsonDocument::fromJson(Read(pids_file)).object();
		const auto guard_pid  = pids.value("guard").toInteger();
		const auto descendant = pids.value("descendant").toInteger();
		const auto host_pid =
		    QString::fromUtf8(Read(dir.filePath("host-pid"))).trimmed().toLongLong();
		QVERIFY(cleanup.pid > 1 && host_pid > 1 && guard_pid > 1 && descendant > 1);
		QCOMPARE(::kill(pid_t(kill_wrapper ? cleanup.pid : host_pid), SIGKILL), 0);
		QTest::qWait(200);
		QVERIFY(QFile::exists(dir.filePath("profile/launcher.lock")));
		QVERIFY(QJsonDocument::fromJson(Read(dir.filePath("modules.json"))).array().size() > 0);
		if (!kill_wrapper) QCOMPARE(backend.GetState(), StreamingBackend::State::Stopping);
		QTRY_VERIFY_WITH_TIMEOUT(!ProcessRunning(guard_pid) && !ProcessRunning(descendant) &&
		                             !ProcessRunning(cleanup.pid),
		                         9000);
		if (!kill_wrapper) {
			QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Error, 3000);
			QCOMPARE(QJsonDocument::fromJson(Read(dir.filePath("modules.json"))).array().size(), 0);
			QVERIFY(!QFile::exists(dir.filePath("profile/launcher.lock")));
			QFile::remove(dir.filePath("launch-game.json"));
			backend.Start(settings);
			QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Ready, 7000);
		}
		backend.Stop();
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Stopped, 5000);
#else
		QSKIP("Linux process supervision");
#endif
	}
	void hostDeathBeforeWrapperExecCannotLaunchAnUnmanagedGame() {
#ifdef __linux__
		QTemporaryDir dir;
		QVERIFY(GenerateCertificate(dir.path()));
		const auto fixture = Read(QStringLiteral(KYTY_LAUNCHER_FIXTURE));
		for (const auto& name: {"host", "pactl"}) {
			QVERIFY(Put(dir.filePath(name), fixture));
			QFile::setPermissions(dir.filePath(name),
			                      QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
		}
		FixtureEnvironment environment(dir.path());
		const auto         wrapper =
		    QDir(QCoreApplication::applicationDirPath()).filePath("libexec/kyty_stream_launch");
		QVERIFY(
		    Put(dir.filePath("launch-game.json"), QJsonDocument(QJsonObject {{"wrapper", wrapper},
		                                                                     { "stop_before_exec",
			                                                                   true }})
		                                              .toJson()));
		StreamingBackend backend(dir.filePath("profile"));
		FixtureGameGroup cleanup;
		backend.SetCatalog(
		    {{ "supervised",
			   "Game",
			   "/usr/bin/touch",
			   {dir.filePath("game-was-launched")},
			   dir.path() }});
		StreamingSettings settings;
		settings.backend = dir.filePath("host");
		settings.port    = FreeBasePort();
		backend.Start(settings);
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Ready, 7000);
		cleanup.pid = QString::fromUtf8(Read(dir.filePath("wrapper-pid"))).trimmed().toLongLong();
		const auto host_pid =
		    QString::fromUtf8(Read(dir.filePath("host-pid"))).trimmed().toLongLong();
		QVERIFY(cleanup.pid > 1 && host_pid > 1);
		QVERIFY(!QFile::exists(QFileInfo(backend.Socket()).dir().filePath("session.json")));
		QCOMPARE(::kill(pid_t(host_pid), SIGKILL), 0);
		QTest::qWait(200);
		QCOMPARE(backend.GetState(), StreamingBackend::State::Stopping);
		QVERIFY(QFile::exists(dir.filePath("profile/launcher.lock")));
		::kill(pid_t(cleanup.pid), SIGCONT);
		QTRY_VERIFY_WITH_TIMEOUT(!ProcessRunning(cleanup.pid), 5000);
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Error, 3000);
		QVERIFY(!QFile::exists(dir.filePath("game-was-launched")));
#else
		QSKIP("Linux process supervision");
#endif
	}
	void forgedSessionRecordCannotKillForeignProcessGroup_data() {
		QTest::addColumn<bool>("foreign_wrapper");
		QTest::newRow("foreign-sleep-group") << false;
		QTest::newRow("foreign-wrapper-with-different-nonce") << true;
	}
	void forgedSessionRecordCannotKillForeignProcessGroup() {
#ifdef __linux__
		QTemporaryDir dir;
		QVERIFY(GenerateCertificate(dir.path()));
		const auto fixture = Read(QStringLiteral(KYTY_LAUNCHER_FIXTURE));
		for (const auto& name: {"host", "pactl"}) {
			QVERIFY(Put(dir.filePath(name), fixture));
			QFile::setPermissions(dir.filePath(name),
			                      QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
		}
		FixtureEnvironment environment(dir.path());
		QProcess           foreign;
		QFETCH(bool, foreign_wrapper);
		foreign.setChildProcessModifier([] { ::setsid(); });
		if (foreign_wrapper) {
			const StreamingLaunchEntry game {
			    "foreign", "Foreign", "/usr/bin/sleep", {"60"}, dir.path()};
			auto manifest =
			    StreamingCatalog::Manifest({game}, dir.filePath("foreign.sock"), "Private",
			                               dir.filePath("foreign.lock"), "Native", 60);
			manifest.insert("session_file", dir.filePath("foreign-session.json"));
			QVERIFY(AttachHostOwner(&manifest, dir.path()));
			QString error;
			QVERIFY(StreamingCatalog::WritePrivate(dir.filePath("foreign-launch.json"),
			                                       QJsonDocument(manifest).toJson(), &error));
			HostEnvironment(&foreign, manifest);
			foreign.start(
			    QDir(QCoreApplication::applicationDirPath()).filePath("libexec/kyty_stream_launch"),
			    {dir.filePath("foreign-launch.json"), "foreign"});
		} else
			foreign.start("/usr/bin/sleep", {"60"});
		QVERIFY(foreign.waitForStarted(1000));
		if (foreign_wrapper)
			QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(dir.filePath("foreign-session.json")), 2000);
		StreamingBackend  backend(dir.filePath("profile"));
		StreamingSettings settings;
		settings.backend = dir.filePath("host");
		settings.port    = FreeBasePort();
		backend.Start(settings);
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Ready, 7000);
		const auto        nonce = QJsonDocument::fromJson(Read(dir.filePath("profile/launch.json")))
		                              .object()
		                              .value("host_nonce");
		const QJsonObject forged {{"pid", foreign.processId()},
		                          {"pgid", foreign.processId()},
		                          {"guardian_pid", foreign.processId()},
		                          {"host_nonce", nonce},
		                          {"start_time", "stale-or-forged"}};
		QString           error;
		QVERIFY(StreamingCatalog::WritePrivate(
		    QFileInfo(backend.Socket()).dir().filePath("session.json"),
		    QJsonDocument(forged).toJson(), &error));
		const auto host_pid =
		    QString::fromUtf8(Read(dir.filePath("host-pid"))).trimmed().toLongLong();
		QCOMPARE(::kill(pid_t(host_pid), SIGKILL), 0);
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Error, 5000);
		QVERIFY(ProcessRunning(foreign.processId()));
		foreign.terminate();
		QVERIFY(foreign.waitForFinished(2000));
#else
		QSKIP("Linux process supervision");
#endif
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
	void explicitWidgetStartHostsSavedSettingsWithoutLaunchingGame() {
		QTemporaryDir dir;
		QVERIFY(GenerateCertificate(dir.path()));
		const auto fixture = Read(QStringLiteral(KYTY_LAUNCHER_FIXTURE));
		for (const auto& name: {"pactl", "host"}) {
			QVERIFY(Put(dir.filePath(name), fixture));
			QVERIFY(QFile::setPermissions(dir.filePath(name),
			                              QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
		}
		const auto profile = dir.filePath("profile");
		QVERIFY(QDir().mkpath(profile));
		StreamingSettings settings;
		settings.backend     = dir.filePath("host");
		settings.port        = FreeBasePort();
		settings.local_audio = false;
		{
			QSettings file(QDir(profile).filePath("streaming.ini"), QSettings::IniFormat);
			settings.Write(file);
			file.sync();
			QCOMPARE(file.status(), QSettings::NoError);
		}
		FixtureEnvironment environment(dir.path());
		StreamingBackend   backend(profile);
		backend.SetCatalog(
		    {{"idle",
		      "Idle fixture",
		      "/usr/bin/python3",
		      {"-c", "import pathlib,sys;pathlib.Path(sys.argv[1]).write_text('unexpected')",
		       dir.filePath("game-started")},
		      dir.path()}});
		StreamingWidget widget(&backend);
		QCOMPARE(backend.GetState(), StreamingBackend::State::Stopped);
		QVERIFY(!QFile::exists(dir.filePath("host-pid")));
		widget.StartHosting();
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Ready, 10000);
		QTRY_COMPARE_WITH_TIMEOUT(backend.Clients().size(), 1, 5000);
		QVERIFY(backend.Session().startsWith("No game running"));
		QVERIFY(QFile::exists(dir.filePath("stdin-credentials-used")));
		const auto manifest =
		    QJsonDocument::fromJson(Read(QDir(profile).filePath("launch.json"))).object();
		QVERIFY(manifest.value("games").toObject().contains("idle"));
		QVERIFY(!QFile::exists(dir.filePath("game-started")));
		QVERIFY(!QFile::exists(dir.filePath("wrapper-pid")));
		QVERIFY(!QFile::exists(dir.filePath("api-requests.jsonl")));
		QVERIFY(Read(QDir(profile).filePath("host.conf"))
		            .contains("port = " + QByteArray::number(settings.port) + "\n"));
		backend.Stop();
		QTRY_COMPARE_WITH_TIMEOUT(backend.GetState(), StreamingBackend::State::Stopped, 7000);
		QVERIFY(!QFile::exists(dir.filePath("game-started")));
		QCOMPARE(QJsonDocument::fromJson(Read(dir.filePath("modules.json"))).array().size(), 0);
	}
	void explicitWidgetStartRejectsInvalidSettings() {
		QTemporaryDir     dir;
		QSettings         file(dir.filePath("streaming.ini"), QSettings::IniFormat);
		StreamingSettings settings;
		settings.controller_audio      = true;
		settings.controller_audio_port = settings.port;
		settings.Write(file);
		file.sync();
		StreamingBackend backend(dir.path());
		StreamingWidget  widget(&backend);
		widget.StartHosting();
		QCOMPARE(backend.GetState(), StreamingBackend::State::Stopped);
		QVERIFY(backend.Logs().isEmpty());
		QVERIFY(
		    widget.findChild<QLabel*>("streamError")->text().contains("separate from GameStream"));
		QVERIFY(!QFile::exists(dir.filePath("host.conf")));
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
		QVERIFY(AttachHostOwner(&manifest, dir.path()));
		QVERIFY(StreamingCatalog::WritePrivate(dir.filePath("launch.json"),
		                                       QJsonDocument(manifest).toJson(), &error));
		QProcess launch;
		HostEnvironment(&launch, manifest);
		launch.start(wrapper, {dir.filePath("launch.json"), "guard"});
		QVERIFY(launch.waitForStarted(1000));
		QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(ready), 3000);
		launch.terminate();
		QVERIFY(launch.waitForFinished(7000));
		QCOMPARE(Read(ended), QByteArray("capture complete"));
		QCOMPARE(launch.exitCode(), 0);
		QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(dir.filePath("session.json")), 1000);
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
