#include "streamingSettings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHostInfo>
#include <QSettings>
#include <QStandardPaths>

QString StreamingSettings::Validate() const {
	if (port < 1024 || port > 65535)
		return QStringLiteral("QUIC port must be between 1024 and 65535.");
	if (fps != 30 && fps != 60 && fps != 90 && fps != 120)
		return QStringLiteral("Frame rate must be 30, 60, 90 or 120 FPS.");
	if (bitrate < 1 || bitrate > 150)
		return QStringLiteral("Bitrate must be between 1 and 150 Mbps.");
	if (resolution != "Native" && resolution != "1080p" && resolution != "1440p")
		return QStringLiteral("Choose Native, 1080p or 1440p capture.");
	if (codec != "HEVC" && codec != "H264" && codec != "AV1")
		return QStringLiteral("Choose HEVC, H264 or AV1.");
	if (encoder != "auto" && encoder != "nvenc" && encoder != "vaapi" && encoder != "software")
		return QStringLiteral("Choose an available native encoder.");
	if (audio_codec != "opus" && audio_codec != "raw_pcm")
		return QStringLiteral("Choose Opus or PCM audio.");
	if (disconnect_grace < 0 || disconnect_grace > 600)
		return QStringLiteral("Close game after disconnect must be between 0 and 600 seconds.");
	// host_name empty is valid: it falls back to the machine hostname. A set name is trimmed to
	// 1-40 characters with no line break or '=' that would corrupt the generated config file.
	if (!host_name.trimmed().isEmpty()) {
		const auto trimmed = host_name.trimmed();
		if (trimmed.size() > 40) return QStringLiteral("Host name must be 1-40 characters.");
		if (trimmed.contains('\n') || trimmed.contains('\r') || trimmed.contains('='))
			return QStringLiteral("Host name cannot contain line breaks or '='.");
	}
	if (backend.contains('\n') || backend.contains('\r') || adapter.contains('\n') ||
	    adapter.contains('\r'))
		return QStringLiteral("Settings cannot contain line breaks.");
	return {};
}
StreamingSettings StreamingSettings::Read(QSettings& file) {
	StreamingSettings s;
	s.audio_codec = file.value("audio/codec", s.audio_codec).toString();
	s.backend =
	    file.value("host/backend",
		           QDir(QCoreApplication::applicationDirPath()).filePath("libexec/ags-host"))
	        .toString();
	// Existing installs retain preferences but the old executable is never invoked.
	if (QFileInfo(s.backend).fileName() == "kyty_stream_host")
		s.backend = QDir(QCoreApplication::applicationDirPath()).filePath("libexec/ags-host");
	s.encoder               = file.value("quality/encoder", s.encoder).toString();
	s.local_audio_sink      = file.value("audio/local_sink").toString();
	s.fec_enabled           = file.value("quality/fec", false).toBool();
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
	s.disconnect_grace      = file.value("host/disconnect_grace", s.disconnect_grace).toInt();
	s.host_name             = file.value("host/name").toString();
	return s;
}
void StreamingSettings::Write(QSettings& file) const {
	file.setValue("host/backend", backend);
	file.setValue("quality/encoder", encoder);
	file.setValue("audio/codec", audio_codec);
	file.setValue("audio/local_sink", local_audio_sink);
	file.setValue("quality/fec", fec_enabled);
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
	file.setValue("host/disconnect_grace", disconnect_grace);
	file.setValue("host/name", host_name);
}
QString StreamingSettings::DefaultHostName() {
	// Prefer the pretty hostname (/etc/hostname is the static one; QHostInfo gives the resolved
	// name). Fall back to a generic label so the config line is never empty.
	auto name = QHostInfo::localHostName().trimmed();
	if (name.isEmpty()) name = QStringLiteral("Kyty Game Station");
	// Clamp to the same 40-character ceiling the field validates against.
	return name.left(40);
}
QString StreamingSettings::EffectiveHostName() const {
	const auto trimmed = host_name.trimmed();
	return trimmed.isEmpty() ? DefaultHostName() : trimmed;
}
QString StreamingSettings::ProfileDirectory() {
	const auto override = qEnvironmentVariable("KYTY_STREAMING_CONFIG_DIR");
	return override.isEmpty()
	           ? QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
	                 .filePath("KytyPS5/streaming")
			   : QDir(override).absolutePath();
}
