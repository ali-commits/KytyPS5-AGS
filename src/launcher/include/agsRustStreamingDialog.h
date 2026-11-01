#ifndef AGS_RUST_STREAMING_DIALOG_H
#define AGS_RUST_STREAMING_DIALOG_H
#include "streamingCatalog.h"

#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>

#include <functional>
class QLineEdit;
class QSpinBox;
class QComboBox;
class QCheckBox;
class QPlainTextEdit;
class QPushButton;
class QLabel;
class QProcess;
class QListWidget;
// Temporary test controls in the existing GPL launcher. Independent Rust code lives separately.
class AgsRustStreamingDialog: public QDialog {
public:
	explicit AgsRustStreamingDialog(QWidget*                                     parent  = nullptr,
	                                std::function<QList<StreamingLaunchEntry>()> catalog = {});
	~AgsRustStreamingDialog() override;
	static bool WriteConfiguration(const QString& directory, const QJsonObject& config,
	                               QString* error);

	static QJsonArray ImportCatalog(const QList<StreamingLaunchEntry>& entries,
	                                const QString& profile, QString* error);

private:
	void                                         ImportLibrary();
	void                                         ExportPairing();
	void                                         RefreshDevices();
	void                                         UnpairDevice();
	std::function<QList<StreamingLaunchEntry>()> m_catalog;
	QProcess*                                    m_pair_process;
	bool                                         Load();
	bool                                         Save();
	void                                         Start(bool initialize);
	void                                         Stop();
	void                                         Update();
	QLineEdit *m_binary, *m_directory, *m_bind, *m_name, *m_device, *m_local_sink, *m_compositor;
	QSpinBox * m_grace, *m_width, *m_height, *m_fps, *m_bitrate;
	QComboBox *m_encoder, *m_presentation, *m_codec, *m_audio_codec, *m_capture_clock;
	QCheckBox* m_fec;
	QPlainTextEdit *m_games, *m_logs;
	QPushButton *   m_init, *m_save, *m_start, *m_stop;
	QLabel*         m_status;
	QProcess*       m_process;
	QListWidget*    m_devices;
	QJsonObject     m_config;
	QString         m_loaded_directory;
	bool            m_initializing = false;
};
#endif
