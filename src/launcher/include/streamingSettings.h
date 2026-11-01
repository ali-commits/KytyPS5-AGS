#ifndef KYTY_STREAMING_SETTINGS_H
#define KYTY_STREAMING_SETTINGS_H
#include <QString>
class QSettings;
struct StreamingSettings {
	QString backend;
	int     port       = 48189;
	QString resolution = "Native";
	int     fps        = 60;
	int     bitrate    = 30;
	QString codec      = "HEVC";
	QString adapter;
	QString encoder = "auto";
	QString audio_codec = "opus";
	QString local_audio_sink;
	bool fec_enabled = false;
	bool    local_audio           = false;
	bool    show_local            = false;
	bool    controller_audio      = false;
	int     controller_audio_port = 0;
	// Seconds the host keeps a game running after the stream drops before quitting it; the same
	// client reconnecting within the window resumes the running game. 0 = quit immediately.
	int disconnect_grace = 20;
	// Name mDNS discovery advertises and the phone shows. Empty falls back to the machine hostname.
	QString host_name;
	int     ControllerAudioPort() const {
		return controller_audio_port == 0 ? port + 30 : controller_audio_port;
	}
	// The machine hostname (pretty name preferred), used when host_name is left blank.
	static QString DefaultHostName();
	// host_name with surrounding whitespace removed, or DefaultHostName() when that is empty.
	QString                  EffectiveHostName() const;
	QString                  Validate() const;
	static StreamingSettings Read(QSettings& settings);
	void                     Write(QSettings& settings) const;
	static QString           ProfileDirectory();
};
#endif
