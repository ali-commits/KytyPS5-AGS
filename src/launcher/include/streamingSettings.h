#ifndef KYTY_STREAMING_SETTINGS_H
#define KYTY_STREAMING_SETTINGS_H
#include <QString>
class QSettings;
struct StreamingSettings {
	QString                  backend;
	int                      port       = 48189;
	QString                  resolution = "Native";
	int                      fps        = 60;
	int                      bitrate    = 30;
	QString                  codec      = "HEVC";
	QString                  adapter;
	bool                     local_audio = true;
	QString                  Validate() const;
	static StreamingSettings Read(QSettings& settings);
	void                     Write(QSettings& settings) const;
	static QString           ProfileDirectory();
};
#endif
