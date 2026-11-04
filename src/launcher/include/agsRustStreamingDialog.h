#ifndef AGS_RUST_STREAMING_DIALOG_H
#define AGS_RUST_STREAMING_DIALOG_H
#include "streamingCatalog.h"

#include <QDialog>
#include <QJsonObject>

#include <functional>
class HostControlClient;
class QLabel;
class QPlainTextEdit;
class QListWidget;
class QComboBox;
class QLineEdit;
class QSpinBox;
class QCheckBox;
class QPushButton;
// Diagnostics and advanced settings for the existing daemon; never a second worker.
class AgsRustStreamingDialog: public QDialog {
public:
	explicit AgsRustStreamingDialog(QWidget*                                     parent  = nullptr,
	                                std::function<QList<StreamingLaunchEntry>()> catalog = {},
	                                HostControlClient*                           client  = nullptr);
	~AgsRustStreamingDialog() override = default;

private:
	void               Refresh();
	void               Display(const QJsonObject& status);
	void               Save();
	HostControlClient* m_host;
	QLabel*            m_status;
	QPlainTextEdit*    m_details;
	QListWidget*       m_devices;
	QComboBox*         m_presentation;
	QComboBox*         m_encoder;
	QLineEdit*         m_local_sink;
	QSpinBox*          m_grace;
	QCheckBox*         m_local_audio;
	QPushButton*       m_save;
	bool               m_loaded = false;
};
#endif
