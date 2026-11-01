#include "agsRustStreamingDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QVBoxLayout>
#ifdef Q_OS_UNIX
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
// Share Rust's stable flock inode. Closing the descriptor releases the kernel
// lock after crashes too; never unlink it or use an unrelated marker lock.
class PairingFileLock {
public:
	explicit PairingFileLock(const QString& directory) {
#ifdef Q_OS_UNIX
		const auto path = QFile::encodeName(QDir(directory).filePath(".pairing.lockfile"));
		m_fd            = ::open(path.constData(), O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
		if (m_fd < 0) {
			m_error = QString("Cannot open private pairing lock: %1")
			              .arg(QString::fromLocal8Bit(std::strerror(errno)));
			return;
		}
		struct stat info {};
		if (::fstat(m_fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != ::geteuid() ||
		    (info.st_mode & 0077) != 0) {
			m_error = "Invalid private pairing lock ownership or permissions";
			return;
		}
		if (::flock(m_fd, LOCK_EX | LOCK_NB) != 0) {
			m_error = "Pairing/settings are busy. Retry after the active operation finishes.";
			return;
		}
		m_locked = true;
#else
		Q_UNUSED(directory);
		m_error = "Pairing settings locking is supported on Linux.";
#endif
	}
	~PairingFileLock() {
#ifdef Q_OS_UNIX
		if (m_fd >= 0) ::close(m_fd);
#endif
	}
	bool           locked() const { return m_locked; }
	const QString& error() const { return m_error; }

private:
	int     m_fd     = -1;
	bool    m_locked = false;
	QString m_error;
};
} // namespace

AgsRustStreamingDialog::AgsRustStreamingDialog(
    QWidget* parent, std::function<QList<StreamingLaunchEntry>()> catalogProvider)
    : QDialog(parent), m_catalog(std::move(catalogProvider)) {
	setWindowTitle(tr("AGS Rust streaming — test controls"));
	resize(760, 850);
	auto* layout = new QVBoxLayout(this);
	auto* note   = new QLabel(tr("Independent streaming test host. Initialize a separate profile, "
	                             "add games, then start hosting. Settings take effect on the next "
	                             "start. The host stops when this window closes."),
	                          this);
	note->setWordWrap(true);
	layout->addWidget(note);
	auto*     form = new QFormLayout;
	QSettings prefs;
	m_binary = new QLineEdit(
	    prefs.value("rustStreaming/binary", QStandardPaths::findExecutable("ags-host")).toString(),
	    this);
	m_binary->setObjectName("rustHostBinary");
	auto* binaryRow = new QHBoxLayout;
	binaryRow->addWidget(m_binary);
	auto* browse = new QPushButton(tr("Browse…"), this);
	binaryRow->addWidget(browse);
	connect(browse, &QPushButton::clicked, this, [this] {
		const auto path =
		    QFileDialog::getOpenFileName(this, tr("AGS host executable"), m_binary->text());
		if (!path.isEmpty()) m_binary->setText(path);
	});
	form->addRow(tr("AGS host executable"), binaryRow);
	m_directory = new QLineEdit(
	    prefs
	        .value("rustStreaming/directory",
			       QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
			           "/ags-rust-streaming")
	        .toString(),
	    this);
	m_directory->setObjectName("rustHostProfile");
	form->addRow(tr("Separate profile directory"), m_directory);
	m_name = new QLineEdit(this);
	form->addRow(tr("Host name"), m_name);
	m_bind = new QLineEdit(this);
	m_bind->setPlaceholderText("0.0.0.0:48200");
	form->addRow(tr("Listen address"), m_bind);
	auto spin = [this](int min, int max) {
		auto* s = new QSpinBox(this);
		s->setRange(min, max);
		return s;
	};
	m_grace = spin(0, 600);
	m_grace->setValue(20);
	m_grace->setSuffix(tr(" s"));
	m_grace->setObjectName("rustDisconnectGrace");
	form->addRow(tr("Disconnect grace"), m_grace);
	m_presentation = new QComboBox(this);
	m_presentation->addItem(tr("Private (no host game window)"), "private");
	m_presentation->addItem(tr("Mirrored (show on host too)"), "mirrored");
	form->addRow(tr("Video presentation"), m_presentation);
	m_local_sink = new QLineEdit(this);
	m_local_sink->setPlaceholderText(
	    tr("Empty = remote sound only; otherwise exact local sink name"));
	form->addRow(tr("Local audio output"), m_local_sink);
	m_audio_codec = new QComboBox(this);
	m_audio_codec->setObjectName("rustAudioCodec");
	m_audio_codec->addItem(tr("Opus (low latency)"), "opus");
	m_audio_codec->addItem(tr("PCM (uncompressed)"), "raw_pcm");
	form->addRow(tr("Stream audio codec"), m_audio_codec);
	m_capture_clock = new QComboBox(this);
	m_capture_clock->setObjectName("rustCaptureClock");
	m_capture_clock->addItem(tr("Compatibility capture (independent timing)"), "legacy");
	m_capture_clock->addItem(tr("Shared capture clock (requires supported producer)"), "common");
	form->addRow(tr("Capture timing"), m_capture_clock);
	m_compositor = new QLineEdit("gamescope", this);
	m_compositor->setObjectName("rustCompositorBinary");
	m_compositor->setToolTip(
	    tr("Select a capture compositor with verified timestamps for shared-clock mode."));
	form->addRow(tr("Capture compositor executable"), m_compositor);

	m_width          = spin(16, 8192);
	m_height         = spin(16, 8192);
	auto* dimensions = new QHBoxLayout;
	dimensions->addWidget(m_width);
	dimensions->addWidget(m_height);
	form->addRow(tr("Stream width / height"), dimensions);
	m_fps = spin(1, 240);
	m_fps->setValue(60);
	form->addRow(tr("Frames per second"), m_fps);
	m_bitrate = spin(100, 200000);
	m_bitrate->setValue(12000);
	m_bitrate->setSuffix(tr(" kbps"));
	form->addRow(tr("Video bitrate"), m_bitrate);
	m_encoder = new QComboBox(this);
	m_encoder->addItem(tr("Automatic (probe usable hardware)"), "auto");
	m_encoder->addItem(tr("NVIDIA NVENC"), "nvenc");
	m_encoder->addItem(tr("AMD / Intel VA-API"), "vaapi");
	m_encoder->addItem(tr("Software"), "software");
	form->addRow(tr("Encoder"), m_encoder);
	m_codec = new QComboBox(this);
	m_codec->addItem(tr("H.264 (baseline)"), "h264");
	m_codec->addItem(tr("HEVC"), "hevc");
	m_codec->addItem(tr("AV1 (requires encoder and decoder support)"), "av1");
	form->addRow(tr("Preferred codec"), m_codec);
	m_fec = new QCheckBox(tr("Packet loss recovery (experimental)"), this);
	m_fec->setObjectName("rustFecEnabled");
	form->addRow(m_fec);
	auto* fecHint =
	    new QLabel(tr("Adds about 25% media bandwidth and a small packet-group delay. Covers "
		              "H.264/HEVC video and audio; AV1 access units are not protected yet."),
		           this);
	fecHint->setWordWrap(true);
	form->addRow(fecHint);
	m_device = new QLineEdit(this);
	m_device->setPlaceholderText("/dev/dri/renderD128");
	form->addRow(tr("VA-API device"), m_device);
	m_devices = new QListWidget(this);
	m_devices->setObjectName("rustPairedDevices");
	m_devices->setMaximumHeight(100);
	form->addRow(tr("Paired devices"), m_devices);
	auto* deviceTools    = new QHBoxLayout;
	auto* refreshDevices = new QPushButton(tr("Refresh devices"), this);
	auto* revokeDevice   = new QPushButton(tr("Unpair selected device"), this);
	revokeDevice->setObjectName("rustUnpairDevice");
	revokeDevice->setToolTip(tr("The host closes a game owned by an unpaired device."));
	deviceTools->addWidget(refreshDevices);
	deviceTools->addWidget(revokeDevice);
	connect(refreshDevices, &QPushButton::clicked, this, &AgsRustStreamingDialog::RefreshDevices);
	connect(revokeDevice, &QPushButton::clicked, this, &AgsRustStreamingDialog::UnpairDevice);
	form->addRow(deviceTools);

	auto* formPanel = new QWidget(this);
	formPanel->setLayout(form);
	auto* formScroll = new QScrollArea(this);
	formScroll->setObjectName("rustSettingsScroll");
	formScroll->setWidgetResizable(true);
	formScroll->setWidget(formPanel);
	formScroll->setMinimumHeight(200);
	formScroll->setMaximumHeight(440);
	layout->addWidget(formScroll);

	auto* catalog = new QLabel(tr("Test game catalog (JSON array: id, name, executable, args, cwd, "
	                              "env). Pairing keys stay private in this profile; transfer "
	                              "client.json and host.der only to trusted devices."),
	                           this);
	catalog->setWordWrap(true);
	layout->addWidget(catalog);
	m_games = new QPlainTextEdit(this);
	m_games->setObjectName("rustGameCatalog");
	m_games->setMaximumBlockCount(10000);
	layout->addWidget(m_games, 1);
	auto* tools  = new QHBoxLayout;
	auto* import = new QPushButton(tr("Import current launcher library"), this);
	import->setObjectName("rustImportLibrary");
	import->setEnabled(bool(m_catalog));
	auto* pairing = new QPushButton(tr("Create pairing bundle (120 s)"), this);
	pairing->setObjectName("rustExportPairing");
	tools->addWidget(import);
	tools->addWidget(pairing);
	layout->addLayout(tools);
	auto* saveNotice =
	    new QLabel(tr("Imported emulator games use fresh isolated test saves/cache under this Rust "
		              "profile. Existing saves are not copied or modified. Prepare required "
		              "runtime assets before testing; importing never launches a game."),
		           this);
	saveNotice->setWordWrap(true);
	layout->addWidget(saveNotice);
	connect(import, &QPushButton::clicked, this, [this] { ImportLibrary(); });
	connect(pairing, &QPushButton::clicked, this, [this] { ExportPairing(); });
	m_pair_process = new QProcess(this);
	m_pair_process->setProcessChannelMode(QProcess::MergedChannels);
	connect(m_pair_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
	        [this](int code, QProcess::ExitStatus) {
		        const auto output =
		            QString::fromUtf8(m_pair_process->readAllStandardOutput()).left(4096);
		        m_status->setText(code == 0
		                              ? tr("Pairing bundle created. Share the owner-only file via "
									       "a trusted channel; it expires in 120 seconds.\n%1")
		                                    .arg(output)
									  : tr("Pairing bundle failed: %1").arg(output));
	        });
	connect(m_pair_process, &QProcess::errorOccurred, this,
	        [this] { m_status->setText(m_pair_process->errorString()); });

	auto* actions = new QHBoxLayout;
	m_init        = new QPushButton(tr("Initialize profile"), this);
	m_init->setObjectName("rustInitializeProfile");
	m_save = new QPushButton(tr("Save settings"), this);
	m_save->setObjectName("rustSaveSettings");
	m_start = new QPushButton(tr("Start Rust host"), this);
	m_start->setObjectName("rustStartHost");
	m_stop = new QPushButton(tr("Stop Rust host"), this);
	for (auto* b: {m_init, m_save, m_start, m_stop})
		actions->addWidget(b);
	auto* reload = new QPushButton(tr("Reload"), this);
	actions->addWidget(reload);
	layout->addLayout(actions);
	m_status = new QLabel(this);
	m_status->setWordWrap(true);
	layout->addWidget(m_status);
	m_logs = new QPlainTextEdit(this);
	m_logs->setReadOnly(true);
	m_logs->setMaximumBlockCount(500);
	m_logs->setMaximumHeight(150);
	layout->addWidget(m_logs);
	m_process = new QProcess(this);
	m_process->setProcessChannelMode(QProcess::MergedChannels);
	connect(m_process, &QProcess::readyReadStandardOutput, this, [this] {
		m_logs->appendPlainText(QString::fromUtf8(m_process->readAllStandardOutput()));
	});
	connect(m_process, &QProcess::started, this, [this] {
		m_status->setText(m_initializing
		                      ? tr("Initializing profile…")
							  : tr("Rust host running. See connection address in the log."));
		Update();
	});
	connect(m_process, &QProcess::errorOccurred, this, [this] {
		m_status->setText(m_process->errorString());
		Update();
	});
	connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
	        [this](int code, QProcess::ExitStatus) {
		        m_status->setText(tr("Host exited (%1)").arg(code));
		        if (m_initializing) {
			        m_initializing = false;
			        Load();
		        }
		        Update();
	        });
	connect(m_init, &QPushButton::clicked, this, [this] { Start(true); });
	connect(m_start, &QPushButton::clicked, this, [this] { Start(false); });
	connect(m_stop, &QPushButton::clicked, this, [this] { Stop(); });
	connect(m_save, &QPushButton::clicked, this, [this] { Save(); });
	connect(reload, &QPushButton::clicked, this, [this] {
		if (m_process->state() == QProcess::NotRunning) Load();
	});
	Load();
	Update();
}
AgsRustStreamingDialog::~AgsRustStreamingDialog() {
	if (m_pair_process->state() != QProcess::NotRunning) {
		m_pair_process->kill();
		m_pair_process->waitForFinished(1000);
	}
	Stop();
}

bool AgsRustStreamingDialog::WriteConfiguration(const QString& directory, const QJsonObject& config,
                                                QString* error) {
	const auto filename = QDir(directory).filePath("host.json");
	if (QFileInfo(filename).isSymLink()) {
		if (error) *error = "Refusing a symlink configuration";
		return false;
	}
	QSaveFile file(filename);
	if (!file.open(QIODevice::WriteOnly)) {
		if (error) *error = file.errorString();
		return false;
	}
	if (!file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)) {
		if (error) *error = "Cannot protect configuration permissions";
		return false;
	}
	const auto data = QJsonDocument(config).toJson(QJsonDocument::Indented);
	if (file.write(data) != data.size() || !file.commit()) {
		if (error) *error = file.errorString();
		return false;
	}
	return true;
}
bool AgsRustStreamingDialog::Load() {
	m_config = {};
	m_loaded_directory.clear();
	QFile f(QDir(m_directory->text()).filePath("host.json"));
	if (!f.open(QIODevice::ReadOnly)) {
		m_config = {};
		m_status->setText(tr("Initialize a separate Rust host profile first."));
		Update();
		return false;
	}
	if (f.size() > 1024 * 1024) {
		m_status->setText(tr("Configuration is too large"));
		return false;
	}
	QJsonParseError parse;
	const auto      doc = QJsonDocument::fromJson(f.readAll(), &parse);
	if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
		m_status->setText(tr("Invalid host configuration"));
		return false;
	}
	m_config           = doc.object();
	m_loaded_directory = QDir(m_directory->text()).absolutePath();
	m_name->setText(m_config["name"].toString());
	m_bind->setText(m_config["bind"].toString());
	m_grace->setValue(m_config["disconnect_grace_secs"].toInt(20));
	m_fec->setChecked(m_config["fec_enabled"].toBool(false));
	m_presentation->setCurrentIndex(
	    qMax(0, m_presentation->findData(m_config["presentation"].toString("private"))));
	m_local_sink->setText(m_config["local_audio_sink"].toString());
	m_capture_clock->setCurrentIndex(
	    qMax(0, m_capture_clock->findData(m_config["capture_clock"].toString("legacy"))));
	m_compositor->setText(m_config["compositor_binary"].toString("gamescope"));

	m_audio_codec->setCurrentIndex(
	    qMax(0, m_audio_codec->findData(m_config["audio_codec"].toString("raw_pcm"))));
	const auto v = m_config["video"].toObject();
	m_codec->setCurrentIndex(qMax(0, m_codec->findData(v["codec"].toString("h264"))));
	m_width->setValue(v["width"].toInt(1280));
	m_height->setValue(v["height"].toInt(720));
	m_fps->setValue(v["fps"].toInt(60));
	m_bitrate->setValue(v["bitrate_kbps"].toInt(12000));
	const auto e = v["encoder"].toObject();
	m_encoder->setCurrentIndex(qMax(0, m_encoder->findData(e["kind"].toString("auto"))));
	m_device->setText(e["device"].toString());
	m_games->setPlainText(QString::fromUtf8(QJsonDocument(m_config["games"].toArray()).toJson()));
	RefreshDevices();
	m_status->setText(tr("Settings loaded. Stop hosting before editing."));
	Update();
	return true;
}
bool AgsRustStreamingDialog::Save() {
	if (m_process->state() != QProcess::NotRunning || m_config.isEmpty() ||
	    m_loaded_directory != QDir(m_directory->text()).absolutePath()) {
		m_status->setText(tr("Stop hosting and reload the selected initialized profile first."));
		return false;
	}
	if (m_width->value() % 2 || m_height->value() % 2) {
		m_status->setText(tr("Width and height must be even."));
		return false;
	}
	QJsonParseError parse;
	auto            catalog = QJsonDocument::fromJson(m_games->toPlainText().toUtf8(), &parse);
	if (parse.error != QJsonParseError::NoError || !catalog.isArray()) {
		m_status->setText(tr("Game catalog must be a JSON array: %1").arg(parse.errorString()));
		return false;
	}
	PairingFileLock pairingLock(m_directory->text());
	if (!pairingLock.locked()) {
		m_status->setText(pairingLock.error());
		return false;
	}
	auto config = m_config;
	// Pairing can update this registry while the controls are open. Never restore
	// stale device secrets from the dialog's original snapshot.
	QFile current(QDir(m_directory->text()).filePath("host.json"));
	if (!current.open(QIODevice::ReadOnly) || current.size() > 1024 * 1024) {
		m_status->setText(tr("Cannot reload the current private device registry."));
		return false;
	}
	QJsonParseError registryError;
	const auto      latest = QJsonDocument::fromJson(current.readAll(), &registryError);
	if (registryError.error != QJsonParseError::NoError || !latest.isObject() ||
	    !latest.object()["devices"].isObject()) {
		m_status->setText(tr("Invalid current device registry; settings were not written."));
		return false;
	}
	config                          = latest.object();
	config["devices"]               = latest.object()["devices"];
	config["name"]                  = m_name->text();
	config["bind"]                  = m_bind->text();
	config["disconnect_grace_secs"] = m_grace->value();
	config["fec_enabled"]           = m_fec->isChecked();
	config["audio_codec"]           = m_audio_codec->currentData().toString();
	config["capture_clock"]         = m_capture_clock->currentData().toString();
	config["compositor_binary"]     = m_compositor->text();

	config["presentation"]     = m_presentation->currentData().toString();
	config["local_audio_sink"] = m_local_sink->text().isEmpty() ? QJsonValue(QJsonValue::Null)
	                                                            : QJsonValue(m_local_sink->text());
	config["games"]            = catalog.array();
	auto v                     = config["video"].toObject();
	v["width"]                 = m_width->value();
	v["height"]                = m_height->value();
	v["fps"]                   = m_fps->value();
	v["bitrate_kbps"]          = m_bitrate->value();
	QJsonObject e {{"kind", m_encoder->currentData().toString()}};
	if (m_encoder->currentData() == "vaapi") e["device"] = m_device->text();
	v["encoder"]    = e;
	v["codec"]      = m_codec->currentData().toString();
	v["container"]  = m_codec->currentData() == "av1" ? "ivf" : "mpeg_ts";
	config["video"] = v;
	QString error;
	if (!WriteConfiguration(m_directory->text(), config, &error)) {
		m_status->setText(error);
		return false;
	}
	m_config = config;
	QSettings prefs;
	prefs.setValue("rustStreaming/binary", m_binary->text());
	prefs.setValue("rustStreaming/directory", m_directory->text());
	m_status->setText(tr("Settings saved. Start hosting to apply."));
	return true;
}
void AgsRustStreamingDialog::Start(bool initialize) {
	if (m_process->state() != QProcess::NotRunning) return;
	const auto binary = QFileInfo(m_binary->text());
	if (!binary.isFile() || !binary.isExecutable()) {
		m_status->setText(tr("Choose the built ags-host executable first."));
		return;
	}
	if (initialize && QFileInfo::exists(QDir(m_directory->text()).filePath("host.json"))) {
		m_status->setText(tr("Profile already exists; use Reload."));
		return;
	}
	if (!initialize && !Save()) return;
	m_initializing = initialize;
	m_process->setProgram(binary.absoluteFilePath());
	m_process->setArguments({initialize ? "init" : "serve", m_directory->text()});
	m_process->start();
	m_status->setText(initialize ? tr("Initializing…") : tr("Starting Rust host…"));
	Update();
}
void AgsRustStreamingDialog::Stop() {
	if (m_process->state() == QProcess::NotRunning) return;
#ifdef Q_OS_UNIX
	if (m_process->processId() > 0) ::kill(static_cast<pid_t>(m_process->processId()), SIGINT);
#else
	m_process->terminate();
#endif
	if (!m_process->waitForFinished(10000)) {
		m_process->kill();
		m_process->waitForFinished(1000);
	}
	Update();
}
void AgsRustStreamingDialog::Update() {
	const bool idle = m_process->state() == QProcess::NotRunning;
	m_start->setEnabled(idle && !m_config.isEmpty());
	m_save->setEnabled(idle && !m_config.isEmpty());
	m_init->setEnabled(idle);
	m_stop->setEnabled(!idle);
	for (auto* widget: {static_cast<QWidget*>(m_binary), static_cast<QWidget*>(m_directory),
	                    static_cast<QWidget*>(m_games)})
		widget->setEnabled(idle);
}

QJsonArray AgsRustStreamingDialog::ImportCatalog(const QList<StreamingLaunchEntry>& entries,
                                                 const QString& profile, QString* error) {
	QJsonArray games;
	const auto root = QDir(profile).absoluteFilePath("kyty-test-runtime");
	if (QFileInfo(root).isSymLink()) {
		if (error) *error = "Test runtime root must not be a symlink";
		return {};
	}
	const QRegularExpression safeId("^[A-Za-z0-9_-]{1,128}$");
	for (const auto& entry: entries) {
		if (!safeId.match(entry.id).hasMatch() || !QFileInfo(entry.program).isAbsolute()) {
			if (error) *error = "Catalog contains an invalid game ID or non-absolute emulator path";
			return {};
		}
		const auto cwd = QDir(root).absoluteFilePath(entry.id);
		if (QFileInfo(cwd).isSymLink() || QFileInfo(QDir(cwd).filePath("_SaveData")).isSymLink()) {
			if (error) *error = "Test runtime/save directory must not be a symlink";
			return {};
		}
		if (!QDir().mkpath(QDir(cwd).filePath("_SaveData"))) {
			if (error) *error = "Cannot create isolated test runtime";
			return {};
		}
		auto args = entry.arguments;
		for (const auto& key:
		     {QString("--game"), QString("--addcont-dir"), QString("--game-patch")}) {
			const auto at = args.indexOf(key);
			if (at >= 0 && at + 1 < args.size() && QDir::isRelativePath(args[at + 1]))
				args[at + 1] = QDir(entry.working_directory).absoluteFilePath(args[at + 1]);
		}
		if (args.contains("--streaming-only") || args.contains("--streaming-socket")) {
			if (error)
				*error = "Use ordinary windowed emulator arguments, not the Sunshine export path";
			return {};
		}
		QJsonArray argv;
		for (const auto& arg: args)
			argv.append(arg);
		games.append(QJsonObject {{"id", entry.id},
		                          {"name", entry.name + " (isolated Rust test)"},
		                          {"executable", QFileInfo(entry.program).absoluteFilePath()},
		                          {"args", argv},
		                          {"cwd", cwd},
		                          {"env", QJsonObject {}}});
	}
	return games;
}
void AgsRustStreamingDialog::ImportLibrary() {
	if (!m_catalog || m_process->state() != QProcess::NotRunning || m_config.isEmpty()) {
		m_status->setText(tr("Stop hosting and load an initialized profile before importing."));
		return;
	}
	QString    error;
	const auto games = ImportCatalog(m_catalog(), m_directory->text(), &error);
	if (!error.isEmpty()) {
		m_status->setText(error);
		return;
	}
	if (games.isEmpty()) {
		m_status->setText(
		    tr("The current launcher library is empty, or its emulator path is not ready."));
		return;
	}
	m_games->setPlainText(QString::fromUtf8(QJsonDocument(games).toJson()));
	m_status->setText(tr("Imported %1 game(s). Save settings to publish. These use empty separate "
	                     "test saves; copy only required runtime assets to each test directory "
	                     "before playing. No game was launched.")
	                      .arg(games.size()));
}
void AgsRustStreamingDialog::ExportPairing() {
	if (m_pair_process->state() != QProcess::NotRunning) {
		m_status->setText(tr("Pairing export is already running."));
		return;
	}
	if (m_loaded_directory != QDir(m_directory->text()).absolutePath() || m_config.isEmpty()) {
		m_status->setText(tr("Load an initialized profile first."));
		return;
	}
	if (m_process->state() == QProcess::NotRunning && !Save()) return;
	const QFileInfo executable(m_binary->text());
	if (!executable.isExecutable()) {
		m_status->setText(tr("Choose the built ags-host executable first."));
		return;
	}
	m_pair_process->setProgram(executable.absoluteFilePath());
	m_pair_process->setArguments({"pair-ticket", m_directory->text(), "120"});
	m_pair_process->start();
	m_status->setText(tr("Creating a private short-lived pairing file…"));
}

void AgsRustStreamingDialog::RefreshDevices() {
	const auto selected = m_devices->currentItem() ? m_devices->currentItem()->text() : QString();
	m_devices->clear();
	if (m_loaded_directory != QDir(m_directory->text()).absolutePath()) return;
	QFile file(QDir(m_loaded_directory).filePath("host.json"));
	if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) return;
	const auto value = QJsonDocument::fromJson(file.readAll()).object();
	for (const auto& id: value["devices"].toObject().keys())
		m_devices->addItem(id);
	const auto matches = m_devices->findItems(selected, Qt::MatchExactly);
	if (!matches.isEmpty())
		m_devices->setCurrentItem(matches.first());
	else if (m_devices->count())
		m_devices->setCurrentRow(0);
}
void AgsRustStreamingDialog::UnpairDevice() {
	if (m_loaded_directory != QDir(m_directory->text()).absolutePath() ||
	    !m_devices->currentItem()) {
		m_status->setText(tr("Load a profile and select a paired device first."));
		return;
	}
	PairingFileLock lock(m_loaded_directory);
	if (!lock.locked()) {
		m_status->setText(lock.error());
		return;
	}
	QFile file(QDir(m_loaded_directory).filePath("host.json"));
	if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) {
		m_status->setText(tr("Cannot read the current device registry."));
		return;
	}
	QJsonParseError parse;
	const auto      document = QJsonDocument::fromJson(file.readAll(), &parse);
	if (parse.error != QJsonParseError::NoError || !document.isObject() ||
	    !document.object()["devices"].isObject()) {
		m_status->setText(tr("Invalid device registry."));
		return;
	}
	auto config  = document.object();
	auto devices = config["devices"].toObject();
	devices.remove(m_devices->currentItem()->text());
	config["devices"] = devices;
	QString error;
	if (!WriteConfiguration(m_loaded_directory, config, &error)) {
		m_status->setText(error);
		return;
	}
	m_config["devices"] = devices;
	RefreshDevices();
	m_status->setText(
	    tr("Device unpaired. The host closes any game owned by this device."));
}
