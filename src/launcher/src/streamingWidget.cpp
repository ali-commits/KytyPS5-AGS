#include "streamingWidget.h"

#include "streamingBackend.h"
#include "streamingCatalog.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkInterface>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

StreamingWidget::StreamingWidget(StreamingBackend* backend, QWidget* parent)
    : QWidget(parent), m_backend(backend) {
	setObjectName("streamingWidget");
	QSettings  file(QDir(backend->Profile()).filePath("streaming.ini"), QSettings::IniFormat);
	const auto settings = StreamingSettings::Read(file);
	auto*      outer    = new QVBoxLayout(this);
	auto*      scroll   = new QScrollArea(this);
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
	auto* description =
	    new QLabel(tr("Start hosting here, then choose a PC and game in Kyty TV. Video comes from "
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
	path_row->addWidget(m_path);
	m_browse = new QPushButton(tr("Browse…"), host);
	path_row->addWidget(m_browse);
	host_form->addRow(tr("Streaming helper"), path_row);
	m_version = new QLabel(tr("Version checked when hosting starts"), host);
	m_version->setWordWrap(true);
	host_form->addRow(tr("Backend"), m_version);
	auto* address_row = new QHBoxLayout;
	m_address         = new QComboBox(host);
	m_address->setObjectName("streamLanAddress");
	m_address->setMinimumContentsLength(18);
	address_row->addWidget(m_address, 1);
	auto* copy = new QPushButton(tr("Copy address"), host);
	address_row->addWidget(copy);
	host_form->addRow(tr("Moonlight address"), address_row);
	auto* actions = new QHBoxLayout;
	m_start       = new QPushButton(tr("Start hosting"), host);
	m_start->setObjectName("streamStart");
	m_start->setDefault(false);
	m_stop = new QPushButton(tr("Stop hosting"), host);
	m_stop->setObjectName("streamStop");
	m_status = new QLabel(host);
	m_status->setObjectName("streamStatus");
	m_status->setWordWrap(true);
	actions->addWidget(m_start);
	actions->addWidget(m_stop);
	actions->addWidget(m_status, 1);
	host_form->addRow(actions);
	m_error = new QLabel(host);
	m_error->setObjectName("streamError");
	m_error->setWordWrap(true);
	m_error->setTextInteractionFlags(Qt::TextSelectableByMouse);
	host_form->addRow(m_error);
	layout->addWidget(host);
	m_quality          = new QGroupBox(tr("Streaming host connection"), content);
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
	m_fps->setCurrentIndex(settings.fps == 30 ? 0 : 1);
	profile_row->addWidget(m_fps);
	m_bitrate = new QSpinBox(m_quality);
	m_bitrate->setObjectName("streamBitrate");
	m_bitrate->setRange(5, 100);
	m_bitrate->setSuffix(" Mbps");
	m_bitrate->setValue(settings.bitrate);
	profile_row->addWidget(m_bitrate);
	m_codec = new QComboBox(m_quality);
	m_codec->setObjectName("streamCodec");
	m_codec->addItems({"HEVC", "H264"});
	m_codec->setCurrentText(settings.codec);
	profile_row->addWidget(m_codec);
	quality_form->addRow(tr("Preferred output"), profile_row);
	quality_form->setRowVisible(profile_row, false);
	auto* hint = new QLabel(
	    tr("Choose resolution, frame rate, quality and bitrate in Kyty TV. "
	       "The negotiated TV profile controls host capture and emulator streaming output. "
	       "Local Library game settings stay separate. Streaming is headless."),
	    m_quality);
	hint->setWordWrap(true);
	quality_form->addRow(hint);
	m_show_local = new QCheckBox(tr("Show game window on this PC"), m_quality);
	m_show_local->setObjectName("streamShowLocal");
	m_show_local->setChecked(settings.show_local);
	quality_form->addRow(m_show_local);
	quality_form->setRowVisible(m_show_local, false);
	auto* display_hint =
	    new QLabel(tr("Turn this off to show the game and system dialogs only in Moonlight. "
	                  "The PC still runs and renders the game."),
	               m_quality);
	display_hint->setWordWrap(true);
	quality_form->addRow(display_hint);
	quality_form->setRowVisible(display_hint, false);
	m_adapter = new QLineEdit(settings.adapter, m_quality);
	m_adapter->setObjectName("streamAdapter");
	m_adapter->setPlaceholderText(tr("Automatic NVIDIA device (RTX 3090)"));
	quality_form->addRow(tr("NVENC adapter name"), m_adapter);
	m_port = new QSpinBox(m_quality);
	m_port->setObjectName("streamPort");
	m_port->setRange(1024, 65514);
	m_port->setValue(settings.port);
	quality_form->addRow(tr("GameStream base port"), m_port);
	m_monitor = new QCheckBox(tr("Also play game audio on this computer"), m_quality);
	m_monitor->setObjectName("streamLocalAudio");
	m_monitor->setChecked(settings.local_audio);
	quality_form->addRow(m_monitor);
	m_controller_audio = new QCheckBox(tr("Laptop controller audio companion"), m_quality);
	m_controller_audio->setObjectName("streamControllerAudio");
	m_controller_audio->setChecked(settings.controller_audio);
	m_controller_audio->setEnabled(true);
	quality_form->addRow(m_controller_audio);
	auto* controller_hint =
	    new QLabel(tr("Send native haptic and controller-speaker audio to a registered laptop "
	                  "companion while video and game audio remain on Kyty TV."),
	               m_quality);
	controller_hint->setWordWrap(true);
	quality_form->addRow(controller_hint);
	m_controller_audio_port = new QSpinBox(m_quality);
	m_controller_audio_port->setObjectName("streamControllerAudioPort");
	m_controller_audio_port->setRange(0, 65535);
	m_controller_audio_port->setSpecialValueText(tr("Automatic"));
	m_controller_audio_port->setValue(settings.controller_audio_port);
	m_controller_audio_port->setEnabled(true);
	quality_form->addRow(tr("Controller audio port"), m_controller_audio_port);
	m_save = new QPushButton(tr("Save preferences"), m_quality);
	quality_form->addRow(m_save);
	layout->addWidget(m_quality);
	auto* sessions       = new QGroupBox(tr("Session"), content);
	auto* session_layout = new QHBoxLayout(sessions);
	m_session            = new QLabel(sessions);
	m_session->setWordWrap(true);
	session_layout->addWidget(m_session, 1);
	m_close = new QPushButton(tr("Stop game"), sessions);
	session_layout->addWidget(m_close);
	layout->addWidget(sessions);
	auto* devices     = new QHBoxLayout;
	auto* pairing     = new QGroupBox(tr("Pending pairing requests"), content);
	auto* pair_layout = new QVBoxLayout(pairing);
	m_pending         = new QListWidget(pairing);
	m_pending->setObjectName("streamPending");
	m_pending->setMaximumHeight(110);
	pair_layout->addWidget(m_pending);
	auto* pairing_hint = new QLabel(tr("Add this host in Moonlight, select the request, then enter "
	                                   "the PIN shown by that client."),
	                                pairing);
	pairing_hint->setWordWrap(true);
	pair_layout->addWidget(pairing_hint);
	auto* pair_form = new QFormLayout;
	m_pin           = new QLineEdit(pairing);
	m_pin->setObjectName("streamPin");
	m_pin->setMaxLength(4);
	m_pin->setEchoMode(QLineEdit::Password);
	m_pin->setValidator(new QIntValidator(0, 9999, m_pin));
	m_pin->setPlaceholderText("0000");
	pair_form->addRow(tr("PIN"), m_pin);
	m_name = new QLineEdit(pairing);
	m_name->setObjectName("streamClientName");
	m_name->setMaxLength(128);
	m_name->setPlaceholderText(tr("Living-room TV"));
	pair_form->addRow(tr("Client name"), m_name);
	pair_layout->addLayout(pair_form);
	m_pair = new QPushButton(tr("Pair selected client"), pairing);
	m_pair->setObjectName("streamPair");
	pair_layout->addWidget(m_pair);
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
	connect(m_receiver_toggle, &QPushButton::clicked, this, [this] {
		if (auto* item = m_receivers->currentItem())
			m_backend->SetControllerAudioReceiverEnabled(item->data(Qt::UserRole).toString(),
			                                             !item->data(Qt::UserRole + 1).toBool());
	});
	connect(m_receiver_remove, &QPushButton::clicked, this, [this] {
		if (auto* item = m_receivers->currentItem())
			m_backend->RemoveControllerAudioReceiver(item->data(Qt::UserRole).toString());
	});
	connect(m_receivers, &QListWidget::currentRowChanged, this, [this] {
		const bool  ready = m_backend->GetState() == StreamingBackend::State::Ready;
		const auto* item  = m_receivers->currentItem();
		m_receiver_toggle->setEnabled(ready && item);
		m_receiver_remove->setEnabled(ready && item);
		m_receiver_toggle->setText(item && !item->data(Qt::UserRole + 1).toBool()
		                               ? tr("Enable selected companion")
		                               : tr("Disable selected companion"));
	});
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
	    new QLabel(tr("Private profile: %1\nManagement: HTTPS on 127.0.0.1 only (base port + 1)")
	                   .arg(backend->Profile()),
	               logs);
	private_path->setWordWrap(true);
	private_path->setTextInteractionFlags(Qt::TextSelectableByMouse);
	logs_layout->addWidget(private_path);
	layout->addWidget(logs);
	connect(m_browse, &QPushButton::clicked, this, [this] {
		const auto path =
		    QFileDialog::getOpenFileName(this, tr("Choose Kyty streaming helper"), m_path->text());
		if (!path.isEmpty()) m_path->setText(path);
	});
	connect(copy, &QPushButton::clicked, this,
	        [this] { QApplication::clipboard()->setText(m_address->currentText()); });
	connect(m_save, &QPushButton::clicked, this, [this] { Save(); });
	connect(m_start, &QPushButton::clicked, this, &StreamingWidget::StartHosting);
	connect(m_stop, &QPushButton::clicked, backend, &StreamingBackend::Stop);
	connect(m_close, &QPushButton::clicked, backend, &StreamingBackend::CloseSession);
	connect(m_pair, &QPushButton::clicked, this, [this] {
		if (auto* item = m_pending->currentItem()) {
			m_backend->Pair(item->data(Qt::UserRole).toString(), m_pin->text(), m_name->text());
			m_pin->clear();
		}
	});
	connect(m_forget, &QPushButton::clicked, this, [this] {
		if (auto* item = m_clients->currentItem())
			m_backend->Unpair(item->data(Qt::UserRole).toString());
	});
	connect(m_pending, &QListWidget::currentRowChanged, this, [this] {
		m_pair->setEnabled(m_backend->GetState() == StreamingBackend::State::Ready &&
		                   m_pending->currentItem());
	});
	connect(m_clients, &QListWidget::currentRowChanged, this, [this] {
		m_forget->setEnabled(m_backend->GetState() == StreamingBackend::State::Ready &&
		                     m_clients->currentItem());
	});
	connect(m_port, &QSpinBox::valueChanged, this, [this] { Update(); });
	connect(backend, &StreamingBackend::Changed, this, &StreamingWidget::Update);
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
	s.local_audio           = m_monitor->isChecked();
	s.show_local            = m_show_local->isChecked();
	s.controller_audio      = m_controller_audio->isChecked();
	s.controller_audio_port = m_controller_audio_port->value();
	return s;
}
void StreamingWidget::StartHosting() {
	if (!Save()) return;
	auto settings = Settings();
	// Input stays on the selected client; controller PCM uses its separate companion.
	m_backend->Start(settings);
}
bool StreamingWidget::Save() {
	const auto settings = Settings();
	const auto error    = settings.Validate();
	if (!error.isEmpty()) {
		m_error->setText(error);
		m_error->show();
		return false;
	}
	if (!QDir().mkpath(m_backend->Profile()) ||
	    !QFile::setPermissions(m_backend->Profile(),
	                           QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
		m_error->setText(tr("Cannot create private settings directory."));
		m_error->show();
		return false;
	}
	const auto path = QDir(m_backend->Profile()).filePath("streaming.ini");
	QSettings  file(path, QSettings::IniFormat);
	settings.Write(file);
	file.sync();
	if (file.status() != QSettings::NoError ||
	    !QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner)) {
		m_error->setText(tr("Cannot save streaming preferences."));
		m_error->show();
		return false;
	}
	return true;
}
void StreamingWidget::Update() {
	const auto state = m_backend->GetState();
	const bool ready = state == StreamingBackend::State::Ready;
	const bool stopped =
	    state == StreamingBackend::State::Stopped || state == StreamingBackend::State::Error;
	m_start->setEnabled(stopped);
	m_stop->setEnabled(!stopped && state != StreamingBackend::State::Stopping);
	m_quality->setEnabled(stopped);
	m_path->setEnabled(stopped);
	m_browse->setEnabled(stopped);
	switch (state) {
		case StreamingBackend::State::Stopped: m_status->setText(tr("Stopped")); break;
		case StreamingBackend::State::Starting:
			m_status->setText(tr("Starting · checking host and private audio…"));
			break;
		case StreamingBackend::State::Ready:
			m_status->setText(tr("Hosting ready · choose a game in Kyty TV"));
			break;
		case StreamingBackend::State::Stopping:
			m_status->setText(tr("Stopping host and owned audio routes…"));
			break;
		case StreamingBackend::State::Error:
			m_status->setText(tr("Host stopped after an error"));
			break;
	}
	m_error->setText(m_backend->Error());
	m_error->setVisible(!m_backend->Error().isEmpty());
	if (!m_backend->Version().isEmpty()) m_version->setText(m_backend->Version());
	m_session->setText(ready ? m_backend->Session() : tr("No managed stream session"));
	m_close->setEnabled(ready && m_backend->Session().startsWith("Game running"));
	QStringList addresses;
	for (const auto& interface: QNetworkInterface::allInterfaces()) {
		if (!interface.flags().testFlag(QNetworkInterface::IsUp) ||
		    interface.flags().testFlag(QNetworkInterface::IsLoopBack))
			continue;
		for (const auto& entry: interface.addressEntries())
			if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol)
				addresses.append(entry.ip().toString() + ':' + QString::number(m_port->value()));
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
			const auto text =
			    paired ? entry.value("name").toString()
			           : entry.value("name").toString() + " · " + entry.value("address").toString();
			auto* item = new QListWidgetItem(text, list);
			item->setData(Qt::UserRole, id);
			if (id == selected) list->setCurrentItem(item);
		}
	};
	refresh_list(m_pending, m_backend->Pairings(), false);
	refresh_list(m_clients, m_backend->Clients(), true);
	m_pair->setEnabled(ready && m_pending->currentItem());
	m_forget->setEnabled(ready && m_clients->currentItem());
	const auto audio = m_backend->ControllerAudioStatus();
	m_controller_audio_status->setText(
	    audio.isEmpty()
	        ? tr("Enable laptop controller audio and connect a registered companion "
	             "while the TV game is running.")
	        : tr("%1 · %2 · %3 bytes · %4 ms queued%5")
	              .arg(audio.value("state").toString(), audio.value("receiver_name").toString())
	              .arg(audio.value("bytes").toVariant().toULongLong())
	              .arg(audio.value("queue_us").toDouble() / 1000.0, 0, 'f', 1)
	              .arg(audio.value("last_error").toString().isEmpty()
	                       ? QString()
	                       : " · " + audio.value("last_error").toString()));
	const auto selected_receiver = m_receivers->currentItem()
	                                   ? m_receivers->currentItem()->data(Qt::UserRole).toString()
	                                   : QString();
	const QSignalBlocker receiver_blocker(m_receivers);
	m_receivers->clear();
	for (const auto& value: m_backend->ControllerAudioReceivers()) {
		const auto entry = value.toObject();
		auto*      item  = new QListWidgetItem(
		    entry.value("name").toString() + " · " +
		        (entry.value("enabled").toBool() ? tr("enabled") : tr("disabled")),
		    m_receivers);
		item->setData(Qt::UserRole, entry.value("id").toString());
		item->setData(Qt::UserRole + 1, entry.value("enabled").toBool());
		if (entry.value("id").toString() == selected_receiver) m_receivers->setCurrentItem(item);
	}
	const auto* receiver = m_receivers->currentItem();
	m_receiver_toggle->setEnabled(ready && receiver);
	m_receiver_remove->setEnabled(ready && receiver);
	m_receiver_toggle->setText(receiver && !receiver->data(Qt::UserRole + 1).toBool()
	                               ? tr("Enable selected companion")
	                               : tr("Disable selected companion"));
	m_pin->setEnabled(ready);
	m_name->setEnabled(ready);
	if (m_logs->toPlainText() != m_backend->Logs()) {
		m_logs->setPlainText(m_backend->Logs());
		m_logs->moveCursor(QTextCursor::End);
	}
}
