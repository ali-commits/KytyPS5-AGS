#include "streamingSettings.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>

QString StreamingSettings::Validate() const {
	if (port < 1024 || port > 65514)
		return QStringLiteral("Base port must be between 1024 and 65514.");
	if (controller_audio) {
		const int audio_port = ControllerAudioPort();
		if (audio_port < 1024 || audio_port > 65535)
			return QStringLiteral("Controller audio port must be between 1024 and 65535.");
		for (int offset: {-5, 0, 1, 9, 10, 11, 21})
			if (audio_port == port + offset)
				return QStringLiteral("Controller audio needs a port separate from GameStream.");
	}
	if (fps != 30 && fps != 60) return QStringLiteral("Frame rate must be 30 or 60 FPS.");
	if (bitrate < 5 || bitrate > 100)
		return QStringLiteral("Bitrate must be between 5 and 100 Mbps.");
	if (resolution != "Native" && resolution != "1080p" && resolution != "1440p")
		return QStringLiteral("Choose Native, 1080p or 1440p capture.");
	if (codec != "HEVC" && codec != "H264") return QStringLiteral("Choose HEVC or H264.");
	if (backend.contains('\n') || backend.contains('\r') || adapter.contains('\n') ||
	    adapter.contains('\r'))
		return QStringLiteral("Settings cannot contain line breaks.");
	return {};
}
StreamingSettings StreamingSettings::Read(QSettings& file) {
	StreamingSettings s;
	s.backend =
	    file.value(
	            "host/backend",
	            QDir(QCoreApplication::applicationDirPath()).filePath("libexec/kyty_stream_host"))
	        .toString();
	s.port                  = file.value("host/port", s.port).toInt();
	s.resolution            = file.value("quality/resolution", s.resolution).toString();
	s.fps                   = file.value("quality/fps", s.fps).toInt();
	s.bitrate               = file.value("quality/bitrate", s.bitrate).toInt();
	s.codec                 = file.value("quality/codec", s.codec).toString();
	s.adapter               = file.value("quality/adapter").toString();
	s.local_audio           = file.value("audio/local_monitor", false).toBool();
	s.show_local            = file.value("display/show_local", false).toBool();
	s.controller_audio      = file.value("controller_audio/enabled", false).toBool();
	s.controller_audio_port = file.value("controller_audio/port", 0).toInt();
	return s;
}
void StreamingSettings::Write(QSettings& file) const {
	file.setValue("host/backend", backend);
	file.setValue("host/port", port);
	file.setValue("quality/resolution", resolution);
	file.setValue("quality/fps", fps);
	file.setValue("quality/bitrate", bitrate);
	file.setValue("quality/codec", codec);
	file.setValue("quality/adapter", adapter);
	file.setValue("audio/local_monitor", local_audio);
	file.setValue("display/show_local", show_local);
	file.setValue("controller_audio/enabled", controller_audio);
	file.setValue("controller_audio/port", controller_audio_port);
}
QString StreamingSettings::ProfileDirectory() {
	const auto override = qEnvironmentVariable("KYTY_STREAMING_CONFIG_DIR");
	return override.isEmpty()
	           ? QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
	                 .filePath("KytyPS5/streaming")
	           : QDir(override).absolutePath();
}
