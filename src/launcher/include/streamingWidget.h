#ifndef KYTY_STREAMING_WIDGET_H
#define KYTY_STREAMING_WIDGET_H
#include "streamingSettings.h"

#include <QWidget>
class StreamingBackend;
class QLineEdit;
class QLabel;
class QComboBox;
class QCheckBox;
class QSpinBox;
class QPushButton;
class QPlainTextEdit;
class QListWidget;
class QGroupBox;
class StreamingWidget: public QWidget {
	Q_OBJECT
public:
	explicit StreamingWidget(StreamingBackend* backend, QWidget* parent = nullptr);
	StreamingSettings Settings() const;

private:
	void              Update();
	bool              Save();
	StreamingBackend* m_backend;
	QLineEdit*        m_path;
	QSpinBox*         m_port;
	QComboBox*        m_resolution;
	QComboBox*        m_fps;
	QSpinBox*         m_bitrate;
	QComboBox*        m_codec;
	QLineEdit*        m_adapter;
	QCheckBox*        m_monitor;
	QCheckBox*        m_show_local;
	QCheckBox*        m_controller_audio;
	QSpinBox*         m_controller_audio_port;
	QLabel*           m_status;
	QLabel*           m_error;
	QLabel*           m_version;
	QLabel*           m_session;
	QLabel*           m_controller_audio_status;
	QListWidget*      m_receivers;
	QPushButton*      m_receiver_toggle;
	QPushButton*      m_receiver_remove;
	QComboBox*        m_address;
	QPushButton*      m_start;
	QPushButton*      m_stop;
	QPushButton*      m_pair;
	QPushButton*      m_forget;
	QPushButton*      m_close;
	QPushButton*      m_save;
	QPushButton*      m_browse;
	QListWidget*      m_pending;
	QListWidget*      m_clients;
	QLineEdit*        m_pin;
	QLineEdit*        m_name;
	QPlainTextEdit*   m_logs;
	QGroupBox*        m_quality;
};
#endif
