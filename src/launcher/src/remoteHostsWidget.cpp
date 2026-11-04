#include "remoteHostsWidget.h"

#include "remoteClient.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

RemoteHostsWidget::RemoteHostsWidget(RemoteClient* client, QWidget* parent): QWidget(parent) {
	setObjectName("remoteHosts");
	auto* layout = new QVBoxLayout(this);
	auto* introduction =
	    new QLabel(tr("Play games from your paired PC. Ask its owner for a one-use pairing bundle "
		              "and compare its certificate fingerprint before connecting."),
		           this);
	introduction->setWordWrap(true);
	layout->addWidget(introduction);
	auto* form = new QFormLayout;
	auto* name = new QLineEdit(this);
	name->setObjectName("remoteHostName");
	name->setMaxLength(64);
	name->setPlaceholderText(tr("My PC"));
	auto* endpoint = new QLineEdit(this);
	endpoint->setObjectName("remoteEndpoint");
	endpoint->setMaxLength(128);
	endpoint->setPlaceholderText("192.168.1.10:48200");
	auto* receiverName = new QLineEdit(tr("AGS Linux receiver"), this);
	receiverName->setMaxLength(64);
	receiverName->setObjectName("remoteReceiverName");
	form->addRow(tr("Host name"), name);
	form->addRow(tr("Host address and port"), endpoint);
	form->addRow(tr("This receiver's pairing name"), receiverName);
	layout->addLayout(form);
	auto* pair = new QPushButton(tr("Import pairing bundle…"), this);
	pair->setObjectName("remotePair");
	layout->addWidget(pair);
	auto* existing = new QPushButton(tr("Use existing paired profile…"), this);
	existing->setObjectName("remoteExistingProfile");
	layout->addWidget(existing);
	auto* hosts = new QComboBox(this);
	hosts->setObjectName("remoteProfiles");
	hosts->setPlaceholderText(tr("No remote hosts configured"));
	hosts->setAccessibleName(tr("Configured remote hosts"));
	hosts->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	hosts->setMinimumContentsLength(16);
	layout->addWidget(hosts);
	auto* buttons = new QHBoxLayout;
	auto* refresh = new QPushButton(tr("Refresh games"), this);
	auto* forget  = new QPushButton(tr("Forget connection"), this);
	auto* stop    = new QPushButton(tr("Stop remote game"), this);
	stop->setObjectName("remoteStop");
	buttons->addWidget(refresh);
	buttons->addWidget(forget);
	buttons->addWidget(stop);
	layout->addLayout(buttons);
	auto* quality = new QComboBox(this);
	quality->setObjectName("remoteQuality");
	quality->addItem(tr("720p · 30 fps"), QVariantList {1280, 720, 30});
	quality->addItem(tr("720p · 60 fps"), QVariantList {1280, 720, 60});
	quality->addItem(tr("1080p · 30 fps"), QVariantList {1920, 1080, 30});
	quality->addItem(tr("1080p · 60 fps"), QVariantList {1920, 1080, 60});
	quality->addItem(tr("1440p · 60 fps"), QVariantList {2560, 1440, 60});
	const auto         saved = client->Quality();
	const QVariantList savedData {saved.width, saved.height, saved.fps};
	int                qualityIndex = quality->findData(savedData);
	if (qualityIndex < 0) {
		quality->addItem(
		    tr("%1 × %2 · %3 fps").arg(saved.width).arg(saved.height).arg(saved.fps),
		    savedData);
		qualityIndex = quality->count() - 1;
	}
	quality->setCurrentIndex(qualityIndex);
	auto* qualityForm = new QFormLayout;
	qualityForm->addRow(tr("Resolution"), quality);
	layout->addLayout(qualityForm);
	// One saved set per client device; applied to the next launch.
	auto* bitrate = new QComboBox(this);
	bitrate->setObjectName("remoteBitrate");
	bitrate->addItem(tr("Automatic"), 0);
	for (const int mbps: {5, 10, 20, 30, 50, 80, 100, 150})
		bitrate->addItem(tr("%1 Mbps").arg(mbps), mbps * 1000);
	auto* videoCodec = new QComboBox(this);
	videoCodec->setObjectName("remoteVideoCodec");
	videoCodec->addItem("H.264", "h264");
	videoCodec->addItem("HEVC", "hevc");
	videoCodec->addItem("AV1", "av1");
	auto* audioCodec = new QComboBox(this);
	audioCodec->setObjectName("remoteAudioCodec");
	audioCodec->addItem("Opus", "opus");
	audioCodec->addItem("PCM", "pcm");
	auto* fec = new QCheckBox(tr("Forward error correction"), this);
	fec->setObjectName("remoteFec");
	auto* latency = new QComboBox(this);
	latency->setObjectName("remoteLatency");
	latency->addItem(tr("Low"), "low");
	latency->addItem(tr("Balanced"), "balanced");
	latency->addItem(tr("Quality"), "quality");
	auto* capture = new QComboBox(this);
	capture->setObjectName("remoteCapture");
	capture->addItem(tr("Gamescope"), "compositor");
	capture->addItem(tr("Native"), "direct");
	capture->setToolTip(tr("Steam games always use gamescope."));
	{
		const auto stream = client->StreamOptions();
		const auto select = [](QComboBox* box, const QVariant& value) {
			const int index = box->findData(value);
			if (index >= 0) box->setCurrentIndex(index);
		};
		if (bitrate->findData(stream.bitrateKbps) < 0)
			bitrate->addItem(tr("%1 Mbps").arg(stream.bitrateKbps / 1000.0, 0, 'g', 4),
			                 stream.bitrateKbps);
		select(bitrate, stream.bitrateKbps);
		select(videoCodec, stream.videoCodec);
		select(audioCodec, stream.audioCodec);
		select(latency, stream.latency);
		select(capture, stream.capture);
		fec->setChecked(stream.fec);
	}
	const auto pairRow = [this](QWidget* first, QWidget* second) {
		auto* row    = new QWidget(this);
		auto* layout = new QHBoxLayout(row);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->addWidget(first, 1);
		layout->addWidget(second, 1);
		return row;
	};
	qualityForm->addRow(tr("Bitrate / latency"), pairRow(bitrate, latency));
	qualityForm->addRow(tr("Video / audio codec"), pairRow(videoCodec, audioCodec));
	qualityForm->addRow(tr("Capture method"), pairRow(capture, fec));
	const QList<QWidget*> streamControls {bitrate, videoCodec, audioCodec, fec, latency, capture};
	const auto            saveStream = [client, bitrate, videoCodec, audioCodec, fec, latency,
	                             capture] {
		RemoteClient::Stream stream;
		stream.bitrateKbps = bitrate->currentData().toInt();
		stream.videoCodec  = videoCodec->currentData().toString();
		stream.audioCodec  = audioCodec->currentData().toString();
		stream.fec         = fec->isChecked();
		stream.latency     = latency->currentData().toString();
		stream.capture     = capture->currentData().toString();
		client->SetStream(stream);
	};
	for (auto* box: {bitrate, videoCodec, audioCodec, latency, capture})
		connect(box, &QComboBox::currentIndexChanged, this, saveStream);
	connect(fec, &QCheckBox::toggled, this, saveStream);
	auto* controller = new QComboBox(this);
	controller->setObjectName("remoteController");
	controller->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	controller->setMinimumContentsLength(14);
	controller->setToolTip(
	    tr("Automatic uses the first readable gamepad. With no gamepad connected, keyboard and "
		   "mouse still work inside the receiver window."));
	auto* refreshControllers = new QPushButton(tr("Refresh"), this);
	refreshControllers->setObjectName("remoteRefreshControllers");
	auto* controllerRow    = new QWidget(this);
	auto* controllerLayout = new QHBoxLayout(controllerRow);
	controllerLayout->setContentsMargins(0, 0, 0, 0);
	controllerLayout->addWidget(controller, 1);
	controllerLayout->addWidget(refreshControllers);
	qualityForm->addRow(tr("Gamepad"), controllerRow);
	const auto populateControllers = [client, controller] {
		const QSignalBlocker block(controller);
		const auto           selected = client->ControllerSelection();
		controller->clear();
		controller->addItem(QObject::tr("Automatic gamepad"), "@auto");
		controller->addItem(QObject::tr("Keyboard / mouse only"), "");
		for (const auto& device: client->Controllers())
			controller->addItem(device.name, device.path);
		int index = controller->findData(selected);
		if (index < 0) {
			controller->addItem(QObject::tr("Selected gamepad unavailable"), selected);
			index = controller->count() - 1;
		}
		controller->setCurrentIndex(index);
	};
	populateControllers();
	connect(refreshControllers, &QPushButton::clicked, this, populateControllers);
	connect(controller, &QComboBox::currentIndexChanged, this,
	        [client, controller, populateControllers](int) {
		        if (!client->SetController(controller->currentData().toString()))
			        populateControllers();
	        });
	auto* note =
	    new QLabel(tr("Input stays inside the native receiver window. "
		              "Hiding AGS keeps playback running. Exit AGS closes the remote game; if the "
		              "host is unreachable, its disconnect grace policy is the fallback."),
		           this);
	note->setWordWrap(true);
	layout->addWidget(note);
	auto* status = new QLabel(this);
	status->setObjectName("remoteStatus");
	status->setTextFormat(Qt::PlainText);
	status->setWordWrap(true);
	status->setTextInteractionFlags(Qt::TextSelectableByMouse);
	layout->addWidget(status);
	layout->addStretch();
	auto update = [client, hosts, refresh, forget, stop, quality, controller, refreshControllers,
	              streamControls] {
		const auto     selected = hosts->currentData().toString();
		QSignalBlocker block(hosts);
		hosts->clear();
		for (const auto& profile: client->Profiles())
			hosts->addItem(profile.name + (profile.online ? QObject::tr(" · Connected")
			                                              : QObject::tr(" · Host offline")),
			               profile.id);
		int index = hosts->findData(selected);
		if (index >= 0) hosts->setCurrentIndex(index);
		refresh->setEnabled(hosts->count() > 0);
		forget->setEnabled(hosts->count() > 0 && !client->Busy());
		stop->setEnabled(client->Busy());
		quality->setEnabled(!client->Busy());
		for (auto* control: streamControls) control->setEnabled(!client->Busy());
		controller->setEnabled(!client->Busy());
		refreshControllers->setEnabled(!client->Busy());
	};
	connect(client, &RemoteClient::Changed, this, update);
	update();
	connect(client, &RemoteClient::Error, status, &QLabel::setText);
	connect(client, &RemoteClient::Stopped, status, [status](const QString& error) {
		status->setText(error.isEmpty() ? QObject::tr("Remote game stopped.") : error);
	});
	connect(refresh, &QPushButton::clicked, this, [client, hosts, status] {
		status->clear();
		client->Refresh(hosts->currentData().toString());
	});
	connect(forget, &QPushButton::clicked, this, [client, hosts, status] {
		if (client->Forget(hosts->currentData().toString()))
			status->setText(QObject::tr(
			    "Connection forgotten. Private credentials remain in their profile directory."));
	});
	connect(stop, &QPushButton::clicked, client, &RemoteClient::StopForExit);
	connect(quality, &QComboBox::currentIndexChanged, this, [client, quality](int) {
		const auto limits = quality->currentData().toList();
		if (limits.size() == 3)
			client->SetQuality(limits[0].toInt(), limits[1].toInt(), limits[2].toInt());
	});
	connect(existing, &QPushButton::clicked, this, [this, client, name, endpoint] {
		const auto directory =
		    QFileDialog::getExistingDirectory(this, tr("Select owner-private paired profile"));
		if (!directory.isEmpty())
			client->AddProfile(name->text().trimmed(), endpoint->text().trimmed(), directory);
	});
	connect(
	    pair, &QPushButton::clicked, this, [this, client, name, endpoint, receiverName, status] {
		    const auto bundle = QFileDialog::getOpenFileName(
		        this, tr("Select one-use pairing bundle"), {}, tr("Pairing bundle (*.json)"));
		    if (bundle.isEmpty()) return;
		    QString fingerprint, address, error;
		    if (!RemoteClient::PairingInfo(bundle, &fingerprint, &address, &error)) {
			    status->setText(error);
			    return;
		    }
		    if (endpoint->text().trimmed().isEmpty()) endpoint->setText(address);
		    if (name->text().trimmed().isEmpty()) name->setText(tr("My PC"));
		    const auto  selectedAddress = endpoint->text().trimmed();
		    QMessageBox confirmation(
		        QMessageBox::Question, tr("Confirm host certificate"),
		        tr("Compare this SHA-256 certificate fingerprint with the PC owner through a "
				   "trusted channel.\n\n%1\n\nConnect to %2 and consume this one-use ticket?")
		            .arg(fingerprint, selectedAddress),
		        QMessageBox::Yes | QMessageBox::No, this);
		    confirmation.setTextFormat(Qt::PlainText);
		    confirmation.setDefaultButton(QMessageBox::No);
		    if (confirmation.exec() != QMessageBox::Yes) return;
		    const auto parent = QDir(QFileInfo(RemoteClient::DefaultSettingsFile()).absolutePath())
			                        .filePath("profiles");
		    if (!QDir().mkpath(parent) ||
			    !QFile::setPermissions(parent,
			                           QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
			    status->setText(tr("Cannot create private profile directory."));
			    return;
		    }
		    const auto output =
		        QDir(parent).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces));
		    status->setText(tr("Pairing… Profile will be saved in %1").arg(output));
		    client->Pair(bundle, fingerprint, name->text().trimmed(), selectedAddress, output,
			             receiverName->text().trimmed());
	    });
	QTimer::singleShot(0, client, &RemoteClient::RefreshAll);
}
