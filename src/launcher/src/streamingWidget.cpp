#include "streamingWidget.h"

#include "agsRustStreamingDialog.h"
#include "hostControlClient.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QNetworkInterface>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

StreamingWidget::StreamingWidget(HostControlClient* client, QWidget* parent)
    : QWidget(parent), m_host(client) {
	setObjectName("streamingWidget");
	// Defaults until get-preferences answers: the daemon owns the stored preferences.
	const StreamingSettings settings;
	auto*                   outer  = new QVBoxLayout(this);
	auto*                   scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	auto* content = new QWidget(scroll);
	auto* layout  = new QVBoxLayout(content);
	layout->setSpacing(14);
	outer->addWidget(scroll);
	scroll->setWidget(content);
	auto* title = new QLabel(tr("Game-only streaming"), content);
	auto  font  = title->font();
	font.setPointSize(font.pointSize() + 4);
	font.setBold(true);
	title->setFont(font);
	layout->addWidget(title);
	auto* rustControls = new QPushButton(tr("Native AGS diagnostics…"), content);
	rustControls->setObjectName("agsRustStreamingControls");
	layout->addWidget(rustControls);
	connect(rustControls, &QPushButton::clicked, this, [this] {
		AgsRustStreamingDialog dialog(this, m_rust_catalog, m_host);
		dialog.exec();
	});
	auto* description = new QLabel(
	    tr("Start hosting here, then choose this host and a game on a client. Video comes from "
		   "the game before local window scaling; only private game audio is captured."),
	    content);
	description->setWordWrap(true);
	layout->addWidget(description);
	auto* host      = new QGroupBox(tr("Host"), content);
	auto* host_form = new QFormLayout(host);
	host_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	auto* path_row = new QHBoxLayout;
	m_path         = new QLineEdit(settings.backend, host);
	m_path->setObjectName("streamBackendPath");
	m_path->setReadOnly(true);
	path_row->addWidget(m_path);
	m_browse = new QPushButton(tr("Browse…"), host);
	m_browse->setEnabled(false);
	path_row->addWidget(m_browse);
	host_form->addRow(tr("Streaming helper"), path_row);
	// The daemon owns the helper path, so the control is read-only (and hidden).
	host_form->setRowVisible(path_row, false);
	m_version = new QLabel(tr("Native AGS · owner management handshake"), host);
	m_version->setWordWrap(true);
	host_form->addRow(tr("Backend"), m_version);
	auto* address_row = new QHBoxLayout;
	m_address         = new QComboBox(host);
	m_address->setObjectName("streamLanAddress");
	m_address->setMinimumContentsLength(18);
	address_row->addWidget(m_address, 1);
	auto* copy = new QPushButton(tr("Copy address"), host);
	address_row->addWidget(copy);
	host_form->addRow(tr("AGS address"), address_row);
	auto* actions = new QHBoxLayout;
	m_start       = new QPushButton(tr("Start hosting"), host);
	m_start->setObjectName("streamStart");
	m_start->setDefault(false);
	m_stop = new QPushButton(tr("Stop hosting"), host);
	m_stop->setObjectName("streamStop");
	m_restart = new QPushButton(tr("Restart hosting"), host);
	m_restart->setObjectName("streamRestart");
	m_status = new QLabel(host);
	m_status->setObjectName("streamStatus");
	m_status->setWordWrap(true);
	actions->addWidget(m_start);
	actions->addWidget(m_stop);
	actions->addWidget(m_restart);
	actions->addWidget(m_status, 1);
	host_form->addRow(actions);
	m_autostart = new QCheckBox(tr("Start at login"), host);
	m_autostart->setObjectName("streamAutostart");
	host_form->addRow(m_autostart);
	m_error = new QLabel(host);
	m_error->setObjectName("streamError");
	m_error->setWordWrap(true);
	m_error->setTextInteractionFlags(Qt::TextSelectableByMouse);
	m_error->hide();
	host_form->addRow(m_error);
	layout->addWidget(host);
	m_quality          = new QGroupBox(tr("Next game preferences"), content);
	auto* quality_form = new QFormLayout(m_quality);
	auto* profile_row  = new QHBoxLayout;
	m_resolution       = new QComboBox(m_quality);
	m_resolution->setObjectName("streamResolution");
	m_resolution->addItems({"Native", "1080p", "1440p"});
	m_resolution->setCurrentText(settings.resolution);
	profile_row->addWidget(m_resolution);
	m_fps = new QComboBox(m_quality);
	m_fps->setObjectName("streamFps");
	m_fps->addItem("30 FPS", 30);
	m_fps->addItem("60 FPS", 60);
	m_fps->addItem("90 FPS", 90);
	m_fps->addItem("120 FPS", 120);
	m_fps->setCurrentIndex(settings.fps == 30 ? 0 : 1);
	profile_row->addWidget(m_fps);
	m_bitrate = new QSpinBox(m_quality);
	m_bitrate->setObjectName("streamBitrate");
	m_bitrate->setRange(1, 150);
	m_bitrate->setSuffix(" Mbps");
	m_bitrate->setValue(settings.bitrate);
	profile_row->addWidget(m_bitrate);
	m_codec = new QComboBox(m_quality);
	m_codec->setObjectName("streamCodec");
	m_codec->addItems({"HEVC", "H264", "AV1"});
	m_codec->setCurrentText(settings.codec);
	profile_row->addWidget(m_codec);
	quality_form->addRow(tr("Preferred output"), profile_row);
	quality_form->setRowVisible(profile_row, true);
	auto* hint = new QLabel(
	    tr("These are host output preferences. The active negotiated codec and input capabilities "
		   "appear in diagnostics. Hardware-dependent modes report errors when unavailable. Native "
		   "currently uses a 1080p host default; client negotiation can select another size."),
	    m_quality);
	hint->setWordWrap(true);
	quality_form->addRow(hint);
	m_show_local = new QCheckBox(tr("Show game window on this host"), m_quality);
	m_show_local->setObjectName("streamShowLocal");
	m_show_local->setChecked(settings.show_local);
	quality_form->addRow(m_show_local);
	quality_form->setRowVisible(m_show_local, true);
	auto* display_hint =
	    new QLabel(tr("Private/headless hides the game window. Mirrored shows it locally. "
		              "The host still runs and renders the game."),
		           m_quality);
	display_hint->setWordWrap(true);
	quality_form->addRow(display_hint);
	quality_form->setRowVisible(display_hint, true);
	m_adapter = new QLineEdit(settings.adapter, m_quality);
	m_adapter->setObjectName("streamAdapter");
	m_adapter->setPlaceholderText(tr("/dev/dri/renderD128 (VAAPI only)"));
	quality_form->addRow(tr("VAAPI device"), m_adapter);
	m_encoder = new QComboBox(m_quality);
	m_encoder->setObjectName("streamEncoder");
	m_encoder->addItems({"auto", "nvenc", "vaapi", "software"});
	quality_form->addRow(tr("Encoder"), m_encoder);
	m_audio_codec = new QComboBox(m_quality);
	m_audio_codec->setObjectName("streamAudioCodec");
	m_audio_codec->addItem(tr("Opus"), "opus");
	m_audio_codec->addItem(tr("PCM"), "raw_pcm");
	quality_form->addRow(tr("Audio codec"), m_audio_codec);
	m_fec = new QCheckBox(tr("Forward error correction"), m_quality);
	m_fec->setObjectName("streamFec");
	quality_form->addRow(m_fec);
	m_local_sink = new QLineEdit(m_quality);
	m_local_sink->setObjectName("streamLocalAudioSink");
	m_local_sink->setPlaceholderText(tr("Default output"));
	quality_form->addRow(tr("Local audio sink"), m_local_sink);
	m_port = new QSpinBox(m_quality);
	m_port->setObjectName("streamPort");
	m_port->setRange(1024, 65535);
	m_port->setValue(settings.port);
	quality_form->addRow(tr("QUIC port"), m_port);
	m_host_name = new QLineEdit(settings.host_name, m_quality);
	m_host_name->setObjectName("streamHostName");
	m_host_name->setMaxLength(40);
	m_host_name->setPlaceholderText(StreamingSettings::DefaultHostName());
	quality_form->addRow(tr("Host name"), m_host_name);
	auto* host_name_hint =
	    new QLabel(tr("The name AGS clients show for this host. Leave blank to use the "
		              "machine name."),
		           m_quality);
	host_name_hint->setWordWrap(true);
	quality_form->addRow(host_name_hint);
	m_disconnect_grace = new QSpinBox(m_quality);
	m_disconnect_grace->setObjectName("streamDisconnectGrace");
	m_disconnect_grace->setRange(0, 600);
	m_disconnect_grace->setValue(settings.disconnect_grace);
	m_disconnect_grace->setSuffix(tr(" s"));
	quality_form->addRow(tr("Close game after disconnect"), m_disconnect_grace);
	auto* grace_hint = new QLabel(
	    tr("Keep the game running this long after the stream drops so the same client can "
		   "reconnect and resume the running game."),
	    m_quality);
	grace_hint->setWordWrap(true);
	quality_form->addRow(grace_hint);
	m_monitor = new QCheckBox(tr("Also play game audio on this host"), m_quality);
	m_monitor->setObjectName("streamLocalAudio");
	m_monitor->setChecked(settings.local_audio);
	quality_form->addRow(m_monitor);
	m_controller_audio = new QCheckBox(tr("Client controller audio companion"), m_quality);
	m_controller_audio->setObjectName("streamControllerAudio");
	m_controller_audio->setChecked(settings.controller_audio);
	m_controller_audio->setEnabled(true);
	quality_form->addRow(m_controller_audio);
	quality_form->setRowVisible(m_controller_audio, false); // not a daemon preference
	auto* controller_hint =
	    new QLabel(tr("Send native haptic and controller-speaker audio to a registered client "
		              "companion while video and game audio remain on Kyty TV."),
		           m_quality);
	controller_hint->setWordWrap(true);
	quality_form->addRow(controller_hint);
	quality_form->setRowVisible(controller_hint, false);
	m_controller_audio_port = new QSpinBox(m_quality);
	m_controller_audio_port->setObjectName("streamControllerAudioPort");
	m_controller_audio_port->setRange(0, 65535);
	m_controller_audio_port->setSpecialValueText(tr("Automatic"));
	m_controller_audio_port->setValue(settings.controller_audio_port);
	m_controller_audio_port->setEnabled(true);
	quality_form->addRow(tr("Controller audio port"), m_controller_audio_port);
	quality_form->setRowVisible(m_controller_audio_port, false);
	// Preferences save themselves shortly after the last edit; no Apply button.
	m_autosave = new QTimer(this);
	m_autosave->setSingleShot(true);
	m_autosave->setInterval(600);
	connect(m_autosave, &QTimer::timeout, this, [this] { Apply(); });
	layout->addWidget(m_quality);
	auto* sessions       = new QGroupBox(tr("Session"), content);
	auto* session_layout = new QHBoxLayout(sessions);
	m_session            = new QLabel(sessions);
	m_session->setWordWrap(true);
	session_layout->addWidget(m_session, 1);
	m_close = new QPushButton(tr("Stop game"), sessions);
	m_close->setObjectName("streamStopGame");
	session_layout->addWidget(m_close);
	layout->addWidget(sessions);
	auto* devices     = new QHBoxLayout;
	auto* pairing     = new QGroupBox(tr("Native pairing"), content);
	auto* pair_layout = new QVBoxLayout(pairing);
	auto* pairing_hint =
	    new QLabel(tr("Export a pinned, one-use pairing bundle and transfer it through a trusted "
		              "channel. Import it in AGS within 120 seconds."),
		           pairing);
	pairing_hint->setWordWrap(true);
	pair_layout->addWidget(pairing_hint);
	m_pair = new QPushButton(tr("Export pairing bundle…"), pairing);
	m_pair->setObjectName("streamPair");
	pair_layout->addWidget(m_pair);
	m_pairing_path = new QLabel(pairing);
	m_pairing_path->setObjectName("streamPairingPath");
	m_pairing_path->setTextFormat(Qt::PlainText);
	m_pairing_path->setWordWrap(true);
	m_pairing_path->setTextInteractionFlags(Qt::TextSelectableByMouse);
	pair_layout->addWidget(m_pairing_path);
	devices->addWidget(pairing, 1);
	auto* clients        = new QGroupBox(tr("Paired clients"), content);
	auto* clients_layout = new QVBoxLayout(clients);
	m_clients            = new QListWidget(clients);
	m_clients->setObjectName("streamClients");
	clients_layout->addWidget(m_clients);
	m_forget = new QPushButton(tr("Forget selected client"), clients);
	m_forget->setObjectName("streamUnpair");
	clients_layout->addWidget(m_forget);
	devices->addWidget(clients, 1);
	layout->addLayout(devices);
	auto* receiver_group = new QGroupBox(tr("Controller audio companions"), content);
	receiver_group->setEnabled(true);
	receiver_group->setVisible(false); // companion management is not part of the daemon yet
	auto* receiver_layout     = new QVBoxLayout(receiver_group);
	m_controller_audio_status = new QLabel(receiver_group);
	m_controller_audio_status->setObjectName("streamControllerAudioStatus");
	m_controller_audio_status->setWordWrap(true);
	m_controller_audio_status->setTextFormat(Qt::PlainText);
	receiver_layout->addWidget(m_controller_audio_status);
	m_receivers = new QListWidget(receiver_group);
	m_receivers->setObjectName("streamControllerAudioReceivers");
	m_receivers->setMaximumHeight(100);
	receiver_layout->addWidget(m_receivers);
	auto* receiver_buttons = new QHBoxLayout;
	m_receiver_toggle      = new QPushButton(tr("Disable selected companion"), receiver_group);
	m_receiver_toggle->setObjectName("streamControllerAudioToggle");
	m_receiver_remove = new QPushButton(tr("Remove selected companion"), receiver_group);
	m_receiver_remove->setObjectName("streamControllerAudioRemove");
	receiver_buttons->addWidget(m_receiver_toggle);
	receiver_buttons->addWidget(m_receiver_remove);
	receiver_layout->addLayout(receiver_buttons);
	layout->addWidget(receiver_group);
	auto* logs        = new QGroupBox(tr("Diagnostics"), content);
	auto* logs_layout = new QVBoxLayout(logs);
	m_logs            = new QPlainTextEdit(logs);
	m_logs->setObjectName("streamLogs");
	m_logs->setReadOnly(true);
	m_logs->setMaximumBlockCount(500);
	m_logs->setMinimumHeight(100);
	m_logs->setMaximumHeight(170);
	logs_layout->addWidget(m_logs);
	auto* private_path =
	    new QLabel(tr("Private native profile: %1/native\nManagement: owner-only local socket")
		               .arg(StreamingSettings::ProfileDirectory()),
		           logs);
	private_path->setWordWrap(true);
	private_path->setTextInteractionFlags(Qt::TextSelectableByMouse);
	logs_layout->addWidget(private_path);
	layout->addWidget(logs);
	connect(copy, &QPushButton::clicked, this,
	        [this] { QApplication::clipboard()->setText(m_address->currentText()); });
	connect(m_start, &QPushButton::clicked, this, &StreamingWidget::StartHosting);
	connect(m_stop, &QPushButton::clicked, m_host, &HostControlClient::StopDaemon);
	connect(m_restart, &QPushButton::clicked, m_host, &HostControlClient::RestartDaemon);
	connect(m_close, &QPushButton::clicked, this, [this] {
		m_host->Request({{"type", "stop-game"}},
		                [self = QPointer(this)](QJsonObject reply, QString error) {
			                if (!self) return;
			                if (error.isEmpty() && !reply.value("ok").toBool())
				                error = reply.value("error").toString();
			                if (!error.isEmpty()) {
				                self->m_error->setText(tr("Close failed: %1").arg(error));
				                self->m_error->show();
			                }
		                });
	});
	connect(m_pair, &QPushButton::clicked, this, [this] {
		m_pair->setEnabled(false);
		m_host->Request({{"type", "pair-ticket"}}, [self = QPointer(this)](QJsonObject reply,
		                                                                   QString     error) {
			if (!self) return;
			if (error.isEmpty() && !reply.value("ok").toBool())
				error = reply.value("error").toString(tr("Pairing export failed."));
			self->m_pairing_path->setText(error.isEmpty()
			                                  ? tr("Owner-only bundle: %1\nExpires in 120 seconds.")
			                                        .arg(reply.value("path").toString())
											  : error);
			self->m_pair->setEnabled(self->m_host_status.value("state") == "Ready");
		});
	});
	connect(m_forget, &QPushButton::clicked, this, [this] {
		if (auto* item = m_clients->currentItem())
			m_host->Request({{"type", "unpair"}, {"uuid", item->data(Qt::UserRole).toString()}},
			                [self = QPointer(this)](QJsonObject reply, QString error) {
				                if (!self) return;
				                if (error.isEmpty() && !reply.value("ok").toBool())
					                error = reply.value("error").toString();
				                if (!error.isEmpty()) {
					                self->m_error->setText(tr("Revoke failed: %1").arg(error));
					                self->m_error->show();
				                }
			                });
	});
	const auto ready = [this] { return m_host_status.value("state").toString() == "Ready"; };
	connect(m_clients, &QListWidget::currentRowChanged, this,
	        [this, ready] { m_forget->setEnabled(ready() && m_clients->currentItem()); });
	connect(m_port, &QSpinBox::valueChanged, this, [this] { Update(); });
	for (auto* box: {m_resolution, m_fps, m_codec, m_encoder, m_audio_codec})
		connect(box, &QComboBox::currentIndexChanged, this, &StreamingWidget::ScheduleApply);
	for (auto* spin: {m_port, m_bitrate})
		connect(spin, &QSpinBox::valueChanged, this, &StreamingWidget::ScheduleApply);
	connect(m_disconnect_grace, &QSpinBox::valueChanged, this, &StreamingWidget::ScheduleApply);
	connect(m_host_name, &QLineEdit::textChanged, this, &StreamingWidget::ScheduleApply);
	for (auto* check: {m_monitor, m_show_local, m_fec})
		connect(check, &QCheckBox::toggled, this, &StreamingWidget::ScheduleApply);
	connect(m_adapter, &QLineEdit::textChanged, this, &StreamingWidget::ScheduleApply);
	connect(m_local_sink, &QLineEdit::textChanged, this, &StreamingWidget::ScheduleApply);
	connect(m_autostart, &QCheckBox::toggled, this, [this](bool on) {
		m_autostart_touched = true;
		m_host->SetAutostart(on);
	});
	m_host->QueryAutostart([self = QPointer(this)](bool enabled) {
		if (!self || self->m_autostart_touched) return; // gone, or the user already chose
		const QSignalBlocker blocker(self->m_autostart);
		self->m_autostart->setChecked(enabled);
	});
	connect(m_host, &HostControlClient::CommandFailed, this, [this](const QString& message) {
		m_error->setText(message);
		m_error->show();
	});
	connect(m_host, &HostControlClient::StatusChanged, this, &StreamingWidget::OnStatus);
	LoadPreferences();
	Update();
}
StreamingSettings StreamingWidget::Settings() const {
	StreamingSettings s;
	s.backend               = m_path->text();
	s.port                  = m_port->value();
	s.resolution            = m_resolution->currentText();
	s.fps                   = m_fps->currentData().toInt();
	s.bitrate               = m_bitrate->value();
	s.codec                 = m_codec->currentText();
	s.adapter               = m_adapter->text();
	s.encoder               = m_encoder->currentText();
	s.audio_codec           = m_audio_codec->currentData().toString();
	s.local_audio_sink      = m_local_sink->text();
	s.fec_enabled           = m_fec->isChecked();
	s.local_audio           = m_monitor->isChecked();
	s.show_local            = m_show_local->isChecked();
	s.disconnect_grace      = m_disconnect_grace->value();
	s.host_name             = m_host_name->text();
	s.controller_audio      = m_controller_audio->isChecked();
	s.controller_audio_port = m_controller_audio_port->value();
	return s;
}
void StreamingWidget::StartHosting() {
	// `systemctl start` is a no-op on an active unit, so an idle or failed daemon needs a restart.
	if (m_host_status.isEmpty())
		m_host->StartDaemon();
	else
		m_host->RestartDaemon();
}
void StreamingWidget::LoadPreferences() {
	m_host->Request({{"type", "get-preferences"}},
	                [self = QPointer(this)](QJsonObject reply, QString error) {
		                if (self && error.isEmpty()) self->ApplyPreferences(reply);
	                });
}
void StreamingWidget::ApplyPreferences(const QJsonObject& p) {
	if (p.isEmpty()) return;
	m_prefs_loaded  = true;
	m_loading_prefs = true; // values from the daemon are not user edits
	if (p.contains("port")) m_port->setValue(p.value("port").toInt());
	if (p.contains("resolution")) m_resolution->setCurrentText(p.value("resolution").toString());
	if (p.contains("fps")) m_fps->setCurrentIndex(m_fps->findData(p.value("fps").toInt()));
	if (p.contains("bitrate")) m_bitrate->setValue(p.value("bitrate").toInt());
	if (p.contains("codec")) m_codec->setCurrentText(p.value("codec").toString());
	if (p.contains("audio_codec"))
		m_audio_codec->setCurrentIndex(m_audio_codec->findData(p.value("audio_codec").toString()));
	if (p.contains("encoder")) m_encoder->setCurrentText(p.value("encoder").toString());
	if (p.contains("fec_enabled")) m_fec->setChecked(p.value("fec_enabled").toBool());
	if (p.contains("local_audio_sink"))
		m_local_sink->setText(p.value("local_audio_sink").toString());
	if (p.contains("adapter")) m_adapter->setText(p.value("adapter").toString());
	if (p.contains("local_audio")) m_monitor->setChecked(p.value("local_audio").toBool());
	if (p.contains("show_local")) m_show_local->setChecked(p.value("show_local").toBool());
	if (p.contains("disconnect_grace")) {
		const int grace = p.value("disconnect_grace").toInt();
		m_disconnect_grace->setValue(grace);
	}
	if (p.contains("host_name")) m_host_name->setText(p.value("host_name").toString());
	m_loading_prefs = false;
	Update();
}
void StreamingWidget::ScheduleApply() {
	// Before the daemon's stored values arrive, an edit would overwrite them with defaults.
	if (m_loading_prefs || !m_prefs_loaded) return;
	m_autosave->start();
}
void StreamingWidget::OnStatus(const QJsonObject& status) {
	m_host_status = status;
	// The daemon may start after this widget: fetch its stored preferences once it answers.
	if (!status.isEmpty() && !m_prefs_loaded) LoadPreferences();
	Update();
}
bool StreamingWidget::Apply() {
	const auto settings = Settings();
	const auto error    = settings.Validate();
	if (!error.isEmpty()) {
		m_error->setText(error);
		m_error->show();
		return false;
	}
	m_host->Request({{"type", "set-preferences"},
	                 {"port", settings.port},
	                 {"resolution", settings.resolution},
	                 {"fps", settings.fps},
	                 {"bitrate", settings.bitrate},
	                 {"codec", settings.codec},
	                 {"adapter", settings.adapter},
	                 {"encoder", settings.encoder},
	                 {"audio_codec", settings.audio_codec},
	                 {"fec_enabled", settings.fec_enabled},
	                 {"local_audio_sink", settings.local_audio_sink},
	                 {"local_audio", settings.local_audio},
	                 {"show_local", settings.show_local},
	                 {"disconnect_grace", settings.disconnect_grace},
	                 {"host_name", settings.host_name}},
	                [self = QPointer(this)](QJsonObject reply, QString failure) {
		                if (!self) return;
		                if (failure.isEmpty() && !reply.value("ok").toBool())
			                failure = reply.value("error").toString(tr("Cannot save preferences."));
		                if (failure.isEmpty() && reply.value("deferred").toBool()) {
			                self->m_error->setText(tr(
			                    "Accepted for next launch; endpoint changes require host restart"));
			                self->m_error->show();
			                return;
		                }
		                self->m_error->setText(failure);
		                self->m_error->setVisible(!failure.isEmpty());
	                });
	return true;
}
void StreamingWidget::Update() {
	const bool reachable = !m_host_status.isEmpty();
	const auto state     = m_host_status.value("state").toString();
	const bool ready     = reachable && state == "Ready";
	const bool stopped   = !reachable || state == "Stopped" || state == "Error";
	m_start->setEnabled(stopped);
	m_stop->setEnabled(reachable && state != "Stopping");
	m_restart->setEnabled(ready);
	if (!reachable) {
		m_status->setText(tr("Host daemon is not running"));
	} else if (state == "Starting") {
		m_status->setText(tr("Starting · waiting for native owner readiness…"));
	} else if (state == "Ready") {
		m_status->setText(tr("Hosting ready · choose a game on a client"));
	} else if (state == "Stopping") {
		m_status->setText(tr("Stopping native host and draining its game…"));
	} else if (state == "Error") {
		m_status->setText(tr("Host stopped after an error"));
	} else {
		m_status->setText(tr("Stopped"));
	}
	const auto error = m_host_status.value("error").toString();
	// Command and preference failures set m_error themselves; the daemon's error wins only
	// while one is reported, so a transient local message is not wiped by the 1 s poll.
	if (error != m_daemon_error) {
		if (error.isEmpty() && m_error->text() == m_daemon_error) {
			m_error->clear();
			m_error->hide();
		} else if (!error.isEmpty()) {
			m_error->setText(error);
			m_error->show();
		}
		m_daemon_error = error;
	}
	m_version->setText(m_host_status.value("backend").toString(tr("Native AGS")));
	const auto native = m_host_status.value("native").toObject();
	const auto video  = native.value("video").toObject();
	if (!video.isEmpty())
		m_version->setText(tr("Native AGS · %1×%2 · %3 FPS · %4")
		                       .arg(video.value("width").toInt())
		                       .arg(video.value("height").toInt())
		                       .arg(video.value("fps").toInt())
		                       .arg(video.value("codec").toString()));
	const auto session = m_host_status.value("session").toString();
	m_session->setText(ready ? session : tr("No managed stream session"));
	m_close->setEnabled(ready && m_host_status.value("stream_game_running").toBool());
	QStringList addresses;
	const auto  host_address = m_host_status.value("address").toString();
	if (!host_address.isEmpty()) {
		addresses.append(host_address + ':' + QString::number(m_host_status.value("port").toInt()));
	} else {
		for (const auto& interface: QNetworkInterface::allInterfaces()) {
			if (!interface.flags().testFlag(QNetworkInterface::IsUp) ||
			    interface.flags().testFlag(QNetworkInterface::IsLoopBack))
				continue;
			for (const auto& entry: interface.addressEntries())
				if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol)
					addresses.append(entry.ip().toString() + ':' +
					                 QString::number(m_port->value()));
		}
	}
	if (addresses.isEmpty()) addresses.append(tr("No LAN IPv4 address available"));
	const auto chosen = m_address->currentText();
	if (addresses != [this] {
		    QStringList list;
		    for (int i = 0; i < m_address->count(); ++i)
			    list << m_address->itemText(i);
		    return list;
	    }()) {
		m_address->clear();
		m_address->addItems(addresses);
		const int index = m_address->findText(chosen);
		if (index >= 0) m_address->setCurrentIndex(index);
	}
	const auto refresh_list = [](QListWidget* list, const QJsonArray& entries, bool paired) {
		const auto selected =
		    list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString {};
		QSignalBlocker blocker(list);
		list->clear();
		for (const auto& value: entries) {
			const auto entry = value.toObject();
			const auto id    = entry.value(paired ? "uuid" : "id").toString();
			auto*      item  = new QListWidgetItem(entry.value("name").toString(), list);
			item->setData(Qt::UserRole, id);
			if (id == selected) list->setCurrentItem(item);
		}
	};
	refresh_list(m_clients, m_host_status.value("clients").toArray(), true);
	m_pair->setEnabled(ready);
	m_forget->setEnabled(ready && m_clients->currentItem());
	const auto log = error.isEmpty()
	                     ? tr("Logs: journalctl --user -u kyty-host.service")
	                     : error + "\n" + tr("Logs: journalctl --user -u kyty-host.service");
	if (m_logs->toPlainText() != log) m_logs->setPlainText(log);
}
