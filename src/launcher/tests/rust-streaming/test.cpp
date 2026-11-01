#include "agsRustStreamingDialog.h"
#include "hostControlClient.h"
#include "hostSocket.h"

#include <QCheckBox>
#include <QComboBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QtTest>

class ControlsTest: public QObject {
	Q_OBJECT
private slots:
	void diagnosticUsesExistingOwnerAndNeverWritesRegistryOrStartsProcess() {
		QTemporaryDir      dir;
		QList<QJsonObject> requests;
		QJsonObject        preferences {
		    {"show_local", false},    {"local_audio", true}, {"local_audio_sink", "fixture-output"},
		    {"disconnect_grace", 20}, {"encoder", "auto"},   {"fec_enabled", false}};
		QJsonObject      native {{"active", QJsonObject {{"bind", "127.0.0.1:48231"}}},
		                         {"configured", QJsonObject {{"bind", "127.0.0.1:48232"}}},
		                         {"restart_required", true}};
		const QString    uuid = "00000000-0000-4000-8000-000000000001";
		HostSocketServer server([&](const QJsonObject& request, auto done) {
			requests.append(request);
			QJsonObject reply {{"ok", true}};
			const auto  type = request.value("type").toString();
			if (type == "get-preferences") reply = preferences;
			if (type == "status")
				reply = {{"state", "Ready"},
				         {"native", native},
				         {"clients",
				          QJsonArray {QJsonObject {{"uuid", uuid}, {"name", "Fixture tablet"}}}}};
			if (type == "pair-ticket") reply["path"] = "/private/fixture-bundle.json";
			done({200, "application/json", QJsonDocument(reply).toJson(), {}});
		});
		QString          error;
		QVERIFY2(server.Listen(dir.filePath("control.sock"), &error), qPrintable(error));
		HostControlClient      client(dir.filePath("control.sock"));
		AgsRustStreamingDialog dialog(nullptr, {}, &client);
		auto*                  save = dialog.findChild<QPushButton*>("rustSaveSettings");
		QTRY_VERIFY(save->isEnabled());
		QVERIFY(dialog.findChildren<QProcess*>().isEmpty());
		QVERIFY(!dialog.findChild<QPushButton*>("rustStartHost"));
		QVERIFY(!dialog.findChild<QLineEdit*>("rustHostBinary"));
		auto* devices = dialog.findChild<QListWidget*>("rustPairedDevices");
		QTRY_COMPARE(devices->count(), 1);
		QCOMPARE(devices->item(0)->text(), "Fixture tablet");
		QCOMPARE(devices->item(0)->data(Qt::UserRole).toString(), uuid);
		QVERIFY(
		    dialog.findChild<QPlainTextEdit*>("rustOwnerStatus")->toPlainText().contains("48231"));
		dialog.findChild<QComboBox*>("rustPresentation")->setCurrentIndex(1);
		dialog.findChild<QCheckBox*>("rustLocalAudio")->setChecked(false);
		dialog.findChild<QSpinBox*>("rustDisconnectGrace")->setValue(0);
		dialog.findChild<QCheckBox*>("rustFec")->setChecked(true);
		save->click();
		const auto find = [&](const QString& type) {
			for (const auto& r: requests)
				if (r.value("type") == type) return r;
			return QJsonObject {};
		};
		QTRY_VERIFY(!find("set-preferences").isEmpty());
		const auto updated = find("set-preferences");
		QVERIFY(updated.value("show_local").toBool());
		QVERIFY(!updated.value("local_audio").toBool());
		QCOMPARE(updated.value("disconnect_grace").toInt(-1), 0);
		QVERIFY(updated.value("fec_enabled").toBool());
		QVERIFY(!updated.contains("devices"));
		devices->setCurrentRow(0);
		dialog.findChild<QPushButton*>("rustUnpairDevice")->click();
		QTRY_COMPARE(find("unpair").value("uuid").toString(), uuid);
		dialog.findChild<QPushButton*>("rustExportPairing")->click();
		QTRY_VERIFY(!find("pair-ticket").isEmpty());
		QTRY_VERIFY(dialog.findChild<QLabel*>("rustStatus")->text().contains("120 seconds"));
		QVERIFY(!QFile::exists(dir.filePath("host.json")));
	}
};
QTEST_MAIN(ControlsTest)
#include "test.moc"
