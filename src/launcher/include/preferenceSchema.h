#ifndef LAUNCHER_PREFERENCE_SCHEMA_H
#define LAUNCHER_PREFERENCE_SCHEMA_H
#include "configuration.h"
#include <QJsonArray>
namespace PreferenceSchema {
inline const QStringList CONSOLE_LANGUAGE_NAMES = {
    "Japanese",
    "English (United States)",
    "French (France)",
    "Spanish (Spain)",
    "German",
    "Italian",
    "Dutch",
    "Portuguese (Portugal)",
    "Russian",
    "Korean",
    "Chinese (Traditional)",
    "Chinese (Simplified)",
    "Finnish",
    "Swedish",
    "Danish",
    "Norwegian",
    "Polish",
    "Portuguese (Brazil)",
    "English (United Kingdom)",
    "Turkish",
    "Spanish (Latin America)",
    "Arabic",
    "French (Canada)",
    "Czech",
    "Hungarian",
    "Greek",
    "Romanian",
    "Thai",
    "Vietnamese",
    "Indonesian",
};
struct Field {
	const char* key;
	const char* label;
	const char* type;
	qint64      min;
	qint64      max;
};

// Every key of Configuration::GameSettings() is listed; a missing one is still emitted as a
// string field, so a new setting never vanishes from the API.
constexpr Field FIELDS[] = {
    {"screen_resolution", "Resolution", "enum", 0, 0},
    {"user_name", "User name", "string", 0, 0},
    {"user_id", "User ID", "int", 0, 2147483647},
    {"audio_input_device", "Audio input device", "string", 0, 0},
    {"stream_aspect_mode", "Stream aspect mode", "enum", 0, 0},
    {"present_mode", "Present mode", "enum", 0, 0},
    {"gpu_index", "GPU index", "int", -1, 2147483647},
    {"fullscreen_enabled", "Fullscreen", "bool", 0, 0},
    {"hdr_enabled", "HDR", "bool", 0, 0},
    {"hide_cursor_enabled", "Hide cursor", "bool", 0, 0},
    {"readback_linear_images", "Read back linear images", "bool", 0, 0},
    {"readback_tiled_images", "Read back tiled images", "bool", 0, 0},
    {"sync_raw_image_buffers", "Sync raw image buffers", "bool", 0, 0},
    {"support_zmask", "Support Z mask", "bool", 0, 0},
    {"tessellation_enabled", "Tessellation", "bool", 0, 0},
    {"trophy_enabled", "Trophy notifications", "bool", 0, 0},
    {"trophy_duration_seconds", "Trophy duration (s)", "int", 5, 10},
    {"perf_overlay_enabled", "Performance overlay", "bool", 0, 0},
    {"skip_notice_screen", "Skip notice screen", "bool", 0, 0},
    {"network_connected", "Network connected", "bool", 0, 0},
    {"vblank_frequency", "VBlank frequency", "int", 30, 360},
    {"console_language", "Console language", "int", 0, 29},
    {"vulkan_validation_enabled", "Vulkan validation", "bool", 0, 0},
    {"shader_validation_enabled", "Shader validation", "bool", 0, 0},
    {"shader_optimization_type", "Shader optimization", "enum", 0, 0},
    {"shader_log_direction", "Shader log output", "enum", 0, 0},
    {"command_buffer_dump_enabled", "Command buffer dump", "bool", 0, 0},
    {"printf_direction", "Printf output", "enum", 0, 0},
    {"profiler_enabled", "Profiler", "bool", 0, 0},
    {"renderdoc_enabled", "RenderDoc", "bool", 0, 0},
    {"amd_cpu_mode", "AMD CPU mode", "enum", 0, 0},
    {"red_zone_protection_enabled", "Red zone protection", "bool", 0, 0},
};

// Filesystem paths are never accepted from, or shown to, a request.
const QStringList PATH_KEYS {QStringLiteral("elf"), QStringLiteral("shader_log_folder"),
                             QStringLiteral("command_buffer_dump_folder"),
                             QStringLiteral("printf_output_file")};

inline QStringList Choices(const QString& key) {
	if (key == QLatin1String("screen_resolution")) {
		return EnumToList<Configuration::Resolution>();
	}
	if (key == QLatin1String("stream_aspect_mode")) {
		return {QStringLiteral("fit"), QStringLiteral("fill"), QStringLiteral("stretch")};
	}
	if (key == QLatin1String("present_mode")) {
		return EnumToList<Configuration::PresentMode>();
	}
	if (key == QLatin1String("shader_optimization_type")) {
		return EnumToList<Configuration::ShaderOptimizationType>();
	}
	if (key == QLatin1String("shader_log_direction") || key == QLatin1String("printf_direction")) {
		return EnumToList<Configuration::LogDirection>();
	}
	return {QStringLiteral("auto"), QStringLiteral("on"), QStringLiteral("off")}; // amd_cpu_mode
}

// GameSettings() stores enums by key ("R1280X720") while the choices use the launcher text
// ("1280x720"); the API speaks the choice text in both directions.
inline QString ChoiceText(const QString& raw) {
	return raw.size() > 2 && raw.startsWith(QLatin1Char('R')) && raw.at(1).isDigit()
	           ? raw.mid(1).toLower()
	           : raw;
}

inline QString RawText(const QString& text) {
	return text.size() > 1 && text.at(0).isDigit() ? QLatin1Char('R') + text.toUpper() : text;
}

inline QJsonArray Fields(const Configuration& info) {
	const auto  values = info.GameSettings();
	QJsonArray  fields;
	QStringList listed;
	const auto  add = [&](const QString& key, const QString& label, const QString& type, qint64 min,
	                      qint64 max) {
		if (!values.contains(key) || PATH_KEYS.contains(key)) {
			return; // e.g. a Windows-only key, or a path setting
		}
		const auto  value = values.value(key).toString();
		QJsonObject field {{"key", key},
		                   {"label", label},
		                   {"type", type},
		                   {"value", type == QLatin1String("enum") ? ChoiceText(value) : value}};
		if (type == QLatin1String("enum")) {
			field["choices"] = QJsonArray::fromStringList(Choices(key));
		} else if (type == QLatin1String("int")) {
			field["min"] = min;
			field["max"] = max;
		}
		fields.append(field);
		listed.append(key);
	};
	for (const auto& f: FIELDS) {
		add(QString::fromLatin1(f.key), QString::fromLatin1(f.label), QString::fromLatin1(f.type),
		    f.min, f.max);
	}
	for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
		if (!listed.contains(it.key())) {
			add(it.key(), it.key(), QStringLiteral("string"), 0, 0);
		}
	}
	return fields;
}

} // namespace PreferenceSchema
#endif
