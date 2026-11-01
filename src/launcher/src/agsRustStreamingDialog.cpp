#include "agsRustStreamingDialog.h"

#include "hostControlClient.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

AgsRustStreamingDialog::AgsRustStreamingDialog(QWidget* parent,
                                               std::function<QList<StreamingLaunchEntry>()>,
                                               HostControlClient* client)
    : QDialog(parent), m_host(client ? client : new HostControlClient({}, this)) {
	setWindowTitle(tr("Native AGS host diagnostics"));
	resize(760, 780);
	auto* layout = new QVBoxLayout(this);
	auto* note   = new QLabel(
	    tr("These controls use the running AGS daemon. Library games and per-device settings are "
		   "resolved by its owner. Closing this window leaves hosting unchanged. Direct-session "
		   "input capabilities are reported below; separate controller companions and managed-TV "
		   "control are unavailable until their native service port."),
	    this);
	note->setWordWrap(true);
	layout->addWidget(note);
	auto* form     = new QFormLayout;
	m_presentation = new QComboBox(this);
	m_presentation->setObjectName("rustPresentation");
	m_presentation->addItem(tr("Private / headless"), false);
	m_presentation->addItem(tr("Mirrored / show on host"), true);
	form->addRow(tr("Presentation"), m_presentation);
	m_local_audio = new QCheckBox(tr("Also play game audio locally"), this);
	m_local_audio->setObjectName("rustLocalAudio");
	form->addRow(m_local_audio);
	m_local_sink = new QLineEdit(this);
	m_local_sink->setObjectName("rustLocalAudioSink");
	m_local_sink->setPlaceholderText(tr("Default output"));
	form->addRow(tr("Local audio sink"), m_local_sink);
	m_grace = new QSpinBox(this);
	m_grace->setObjectName("rustDisconnectGrace");
	m_grace->setRange(0, 600);
	m_grace->setValue(20);
	m_grace->setSuffix(tr(" s"));
	form->addRow(tr("Disconnect grace"), m_grace);
	m_encoder = new QComboBox(this);
	m_encoder->setObjectName("rustEncoder");
	m_encoder->addItems({"auto", "nvenc", "vaapi", "software"});
	form->addRow(tr("Encoder preference"), m_encoder);
	m_fec = new QCheckBox(tr("Forward error correction"), this);
	m_fec->setObjectName("rustFec");
	form->addRow(m_fec);
	layout->addLayout(form);
	m_save = new QPushButton(tr("Apply to next game"), this);
	m_save->setObjectName("rustSaveSettings");
	m_save->setEnabled(false);
	layout->addWidget(m_save);
	connect(m_save, &QPushButton::clicked, this, &AgsRustStreamingDialog::Save);
	m_devices = new QListWidget(this);
	m_devices->setObjectName("rustPairedDevices");
	layout->addWidget(m_devices);
	auto* unpair = new QPushButton(tr("Revoke selected device"), this);
	unpair->setObjectName("rustUnpairDevice");
	layout->addWidget(unpair);
	connect(unpair, &QPushButton::clicked, this, [this] {
		if (!m_devices->currentItem()) return;
		m_host->Request(
		    {{"type", "unpair"}, {"uuid", m_devices->currentItem()->data(Qt::UserRole).toString()}},
		    [self = QPointer(this)](QJsonObject reply, QString error) {
			    if (!self) return;
			    if (error.isEmpty() && !reply.value("ok").toBool())
				    error = reply.value("error").toString();
			    self->m_status->setText(error.isEmpty() ? tr("Device revoked.") : error);
			    self->Refresh();
		    });
	});
	auto* pair = new QPushButton(tr("Export one-use pairing bundle…"), this);
	pair->setObjectName("rustExportPairing");
	layout->addWidget(pair);
	connect(pair, &QPushButton::clicked, this, [this] {
		m_host->Request(
		    {{"type", "pair-ticket"}}, [self = QPointer(this)](QJsonObject reply, QString error) {
			    if (!self) return;
			    if (error.isEmpty() && !reply.value("ok").toBool())
				    error = reply.value("error").toString();
			    self->m_status->setText(error.isEmpty()
			                                ? tr("Owner-only pairing bundle: %1\nShare through a "
											     "trusted channel within 120 seconds.")
			                                      .arg(reply.value("path").toString())
											: error);
		    });
	});
	auto* refresh = new QPushButton(tr("Refresh owner status"), this);
	layout->addWidget(refresh);
	connect(refresh, &QPushButton::clicked, this, &AgsRustStreamingDialog::Refresh);
	m_status = new QLabel(this);
	m_status->setObjectName("rustStatus");
	m_status->setWordWrap(true);
	m_status->setTextFormat(Qt::PlainText);
	layout->addWidget(m_status);
	m_details = new QPlainTextEdit(this);
	m_details->setObjectName("rustOwnerStatus");
	m_details->setReadOnly(true);
	layout->addWidget(m_details);
	connect(m_host, &HostControlClient::StatusChanged, this, &AgsRustStreamingDialog::Display);
	m_host->Request(
	    {{"type", "get-preferences"}}, [self = QPointer(this)](QJsonObject p, QString error) {
		    if (!self || !error.isEmpty() || p.isEmpty()) return;
		    self->m_presentation->setCurrentIndex(p.value("show_local").toBool() ? 1 : 0);
		    self->m_local_audio->setChecked(p.value("local_audio").toBool());
		    self->m_local_sink->setText(p.value("local_audio_sink").toString());
		    self->m_grace->setValue(p.value("disconnect_grace").toInt(20));
		    self->m_encoder->setCurrentText(p.value("encoder").toString("auto"));
		    self->m_fec->setChecked(p.value("fec_enabled").toBool());
		    self->m_loaded = true;
		    self->m_save->setEnabled(true);
	    });
	Refresh();
}
void AgsRustStreamingDialog::Refresh() {
	m_host->Request({{"type", "status"}},
	                [self = QPointer(this)](QJsonObject status, QString error) {
		                if (!self) return;
		                if (!error.isEmpty())
			                self->m_status->setText(error);
		                else
			                self->Display(status);
	                });
}
void AgsRustStreamingDialog::Display(const QJsonObject& status) {
	const auto selected = m_devices->currentItem()
	                          ? m_devices->currentItem()->data(Qt::UserRole).toString()
	                          : QString();
	m_devices->clear();
	for (const auto& value: status.value("clients").toArray()) {
		const auto device = value.toObject();
		auto*      item   = new QListWidgetItem(device.value("name").toString(), m_devices);
		item->setData(Qt::UserRole, device.value("uuid"));
		item->setToolTip(device.value("uuid").toString());
		if (device.value("uuid").toString() == selected) m_devices->setCurrentItem(item);
	}
	// The daemon supplies only safe DTOs: no argv, private registry or credentials.
	m_details->setPlainText(
	    QString::fromUtf8(QJsonDocument(status.value("native").toObject()).toJson()));
}
void AgsRustStreamingDialog::Save() {
	if (!m_loaded) return;
	m_save->setEnabled(false);
	m_host->Request({{"type", "set-preferences"},
	                 {"show_local", m_presentation->currentData().toBool()},
	                 {"local_audio", m_local_audio->isChecked()},
	                 {"local_audio_sink", m_local_sink->text()},
	                 {"disconnect_grace", m_grace->value()},
	                 {"fec_enabled", m_fec->isChecked()},
	                 {"encoder", m_encoder->currentText()}},
	                [self = QPointer(this)](QJsonObject reply, QString error) {
		                if (!self) return;
		                self->m_save->setEnabled(true);
		                if (error.isEmpty() && !reply.value("ok").toBool())
			                error = reply.value("error").toString();
		                self->m_status->setText(error.isEmpty()
		                                            ? tr("Settings accepted for the next game. "
													     "Active session settings remain captured.")
													: error);
	                });
}
