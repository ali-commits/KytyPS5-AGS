#include "launchPlan.h"

#include "actions.h"
#include "cheatFile.h"
#include "common/emulatorConfig.h"
#include "common/u59Preset.h"
#include "configuration.h"
#include "emulatorCommand.h"
#include "gameLibrary.h"
#include "settings.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace AdapterLaunch {

namespace {

// One tag per console language, in the order of PreferenceSchema::CONSOLE_LANGUAGE_NAMES.
constexpr std::array<const char*, 30> LANGUAGE_TAGS = {
    "ja-JP",  "en-US", "fr-FR", "es-ES", "de-DE", "it-IT", "nl-NL", "pt-PT", "ru-RU", "ko-KR",
    "zh-TW",  "zh-CN", "fi-FI", "sv-SE", "da-DK", "nb-NO", "pl-PL", "pt-BR", "en-GB", "tr-TR",
    "es-419", "ar-AE", "fr-CA", "cs-CZ", "hu-HU", "el-GR", "ro-RO", "th-TH", "vi-VN", "id-ID",
};

constexpr int MIN_DIMENSION   = 64;
constexpr int MAX_DIMENSION   = 8192;
constexpr int MAX_STREAM_EDGE = 4096; // KYTY_STREAM_WIDTH/HEIGHT (swapchain.cpp)

int EvenClamp(int n, int max) {
	return std::clamp(n, MIN_DIMENSION, max) & ~1;
}

// Flags whose value is a file or folder; relative values are resolved against runtime_root (the
// emulator's working directory) so the plan names absolute paths only.
bool PathFlag(const QString& flag) {
	return flag == QLatin1String("--game") || flag == QLatin1String("--addcont-dir") ||
	       flag == QLatin1String("--game-patch") || flag == QLatin1String("--shader-log-folder") ||
	       flag == QLatin1String("--command-buffer-dump-folder") ||
	       flag == QLatin1String("--printf-output-file");
}

void SetFlag(QStringList& args, const QString& flag, const QString& value) {
	const auto i = args.indexOf(flag);
	if (i >= 0 && i + 1 < args.size()) {
		args[i + 1] = value;
	} else {
		args << flag << value;
	}
}

QJsonObject PresetEnv(const QString& adapter_dir) {
	std::vector<std::filesystem::path> dirs {std::filesystem::path(adapter_dir.toStdU16String())};
	if (QDir parent(adapter_dir); parent.cdUp()) {
		dirs.emplace_back(parent.absolutePath().toStdU16String());
	}
	const auto  preset = Common::U59Preset::Load(dirs);
	QJsonObject env;
	if (preset.status != Common::U59Preset::Status::Loaded) {
		fprintf(stderr, "kyty-ags-adapter: %s\n", Common::U59Preset::Describe(preset).c_str());
		return env;
	}
	for (const auto& entry: preset.entries) {
		env.insert(QString::fromStdString(entry.key), QString::fromStdString(entry.value));
	}
	return env;
}

} // namespace

int LanguageIndex(const QString& tag) {
	const auto t = tag.trimmed().replace(QLatin1Char('_'), QLatin1Char('-')).toLower();
	if (t.isEmpty()) return -1;
	for (int i = 0; i < static_cast<int>(LANGUAGE_TAGS.size()); ++i) {
		if (t == QString::fromLatin1(LANGUAGE_TAGS[i]).toLower()) return i;
	}
	const auto parts   = t.split(QLatin1Char('-'));
	const auto primary = parts.first();
	const auto has     = [&](const char* sub) { return parts.contains(QLatin1String(sub)); };
	if (primary == QLatin1String("zh")) {
		return has("hant") || has("tw") || has("hk") || has("mo") ? 10 : 11;
	}
	if (primary == QLatin1String("no") || primary == QLatin1String("nn")) return 15;
	if (primary == QLatin1String("es") && parts.size() > 1) return 20; // any region but Spain
	if (primary == QLatin1String("en") && parts.size() > 1 &&
	    (has("gb") || has("ie") || has("au") || has("nz") || has("in") || has("za"))) {
		return 18;
	}
	for (int i = 0; i < static_cast<int>(LANGUAGE_TAGS.size()); ++i) {
		if (QString::fromLatin1(LANGUAGE_TAGS[i]).toLower().section(QLatin1Char('-'), 0, 0) ==
		    primary)
			return i;
	}
	return -1;
}

QString LanguageTag(int index) {
	return index >= 0 && index < static_cast<int>(LANGUAGE_TAGS.size())
	           ? QString::fromLatin1(LANGUAGE_TAGS[index])
			   : QString {};
}

int NearestCaptureFps(int fps) {
	int best = 30;
	for (const int rate: {30, 60, 90, 120}) {
		if (std::abs(rate - fps) < std::abs(best - fps)) best = rate;
	}
	return best;
}

bool IsReservedEnvName(const QString& name) {
	static const QStringList reserved = {QStringLiteral("DISPLAY"),
	                                     QStringLiteral("WAYLAND_DISPLAY"),
	                                     QStringLiteral("WAYLAND_SOCKET"),
	                                     QStringLiteral("XDG_RUNTIME_DIR"),
	                                     QStringLiteral("GAMESCOPE_EIS_SOCKET"),
	                                     QStringLiteral("PULSE_SERVER"),
	                                     QStringLiteral("PULSE_SINK"),
	                                     QStringLiteral("PIPEWIRE_REMOTE"),
	                                     QStringLiteral("PIPEWIRE_RUNTIME_DIR")};
	return name.startsWith(QLatin1String("AGS_")) || reserved.contains(name);
}

QString CutUtf8(const QString& text, int max_bytes) {
	QString out;
	for (const auto c: text.toUcs4()) {
		const auto next = out + QString::fromUcs4(&c, 1);
		if (next.toUtf8().size() > max_bytes) break;
		out = next;
	}
	return out;
}

Reply Plan(const GameEntry& game, const QJsonObject& params, const QString& runtime_root,
           const QString& adapter_dir) {
	const auto exe = QDir(adapter_dir).filePath(QString::fromLatin1(EMULATOR_EXE));
	if (!QFileInfo(exe).isFile()) {
		return Reply::Failed(QStringLiteral("kyty_emulator is missing next to the adapter."));
	}
	for (const auto* key: {"standard", "player"}) {
		if (params.contains(QLatin1String(key)) && !params[QLatin1String(key)].isObject()) {
			return Reply::Invalid(QStringLiteral("%1 must be an object.").arg(QLatin1String(key)));
		}
	}
	// launch variant (STANDARD.md launch actions): only the ones actions.list offers.
	bool no_cheats = false;
	if (params.contains(QStringLiteral("action"))) {
		const auto action = params[QStringLiteral("action")];
		if (!action.isString() ||
		    action.toString() != QLatin1String(AdapterActions::LAUNCH_WITHOUT_CHEATS)) {
			return Reply::Invalid(QStringLiteral("Unknown launch action."));
		}
		no_cheats = true;
	}
	const auto standard = params[QStringLiteral("standard")].toObject();
	const bool native   = params[QStringLiteral("capture")].toString() == QLatin1String("native");

	Configuration info;
	info.CopyGameInfoFrom(*game.info);
	info.CopyEmulatorSettingsFrom(*game.info);
	info.CopyGlobalOnlySettingsFrom(*game.info);
	info.elf        = game.info->elf;
	info.addons_dir = game.info->addons_dir;

	QJsonObject applied;
	QString     error;
	if (!AdapterSettings::ApplyPlayer(info, params[QStringLiteral("player")].toObject(), &applied,
	                                  &error)) {
		return Reply::Invalid(error);
	}

	// resolution: "WIDTHxHEIGHT", even 64..8192; the game's own setting when absent or invalid.
	const auto configured = EnumToText(info.screen_resolution).split(QLatin1Char('x'));
	int        width      = configured.value(0).toInt();
	int        height     = configured.value(1).toInt();
	static const QRegularExpression resolution(QStringLiteral("^([0-9]{1,5})x([0-9]{1,5})$"));
	if (const auto m = resolution.match(standard[QStringLiteral("resolution")].toString());
	    m.hasMatch()) {
		width  = m.captured(1).toInt();
		height = m.captured(2).toInt();
	}
	width  = EvenClamp(width, MAX_DIMENSION);
	height = EvenClamp(height, MAX_DIMENSION);

	// fps_limit: the nearest rate the emulator can export for native capture, else 30..360.
	int fps = info.vblank_frequency;
	if (const auto v = standard[QStringLiteral("fps_limit")]; v.isDouble()) {
		fps = static_cast<int>(std::lround(v.toDouble()));
	}
	fps                   = native ? NearestCaptureFps(fps) : std::clamp(fps, 30, 360);
	info.vblank_frequency = fps;

	if (const auto aspect = standard[QStringLiteral("aspect")].toString();
	    aspect == QLatin1String("fit") || aspect == QLatin1String("fill") ||
	    aspect == QLatin1String("stretch")) {
		info.stream_aspect_mode = aspect;
	}
	if (const auto index = LanguageIndex(standard[QStringLiteral("language")].toString());
	    index >= 0) {
		info.console_language = index;
	}
	if (const auto name = CutUtf8(standard[QStringLiteral("player_name")].toString().trimmed(),
	                              static_cast<int>(Config::MAX_USER_NAME_LENGTH));
	    !name.isEmpty()) {
		info.user_name = name;
	}
	// fullscreen, hide_cursor, hdr: bool; the game's own setting when absent or not a bool.
	if (const auto v = standard[QStringLiteral("fullscreen")]; v.isBool()) info.fullscreen_enabled = v.toBool();
	if (const auto v = standard[QStringLiteral("hide_cursor")]; v.isBool()) info.hide_cursor_enabled = v.toBool();
	if (const auto v = standard[QStringLiteral("hdr")]; v.isBool()) info.hdr_enabled = v.toBool();
	info.perf_overlay_enabled = false; // AGS draws the overlay (STANDARD.md section 8.2)

	const auto plan_path = Cheats::PlanPath(runtime_root, AdapterActions::CheatTitleId(info));
	auto       args      = BuildEmulatorArgs(info, no_cheats ? QString {} : plan_path);
	if (args.isEmpty()) {
		return Reply::Failed(QStringLiteral("Cannot build the emulator command for this game."));
	}
	SetFlag(args, QStringLiteral("--screen-width"), QString::number(width));
	SetFlag(args, QStringLiteral("--screen-height"), QString::number(height));
	SetFlag(args, QStringLiteral("--perf-overlay"), QStringLiteral("false"));
	const QDir root(runtime_root);
	for (qsizetype i = 0; i + 1 < args.size(); ++i) {
		if (PathFlag(args[i]) && !QDir::isAbsolutePath(args[i + 1])) {
			args[i + 1] = QDir::cleanPath(root.absoluteFilePath(args[i + 1]));
		}
	}

	auto env = PresetEnv(adapter_dir);
	if (native) {
		// The capture size keeps the render aspect within the exporter's 4096 limit.
		const double scale =
		    std::min(1.0, static_cast<double>(MAX_STREAM_EDGE) / std::max(width, height));
		env.insert(QStringLiteral("KYTY_STREAM_ONLY"), QStringLiteral("1"));
		env.insert(QStringLiteral("KYTY_STREAM_WIDTH"),
		           QString::number(static_cast<int>(width * scale) & ~1));
		env.insert(QStringLiteral("KYTY_STREAM_HEIGHT"),
		           QString::number(static_cast<int>(height * scale) & ~1));
		env.insert(QStringLiteral("KYTY_STREAM_FPS"), QString::number(fps));
	} else {
		// Gamescope captures the frames: the emulator connects for metrics only.
		for (const auto& key: env.keys()) {
			if (key.startsWith(QLatin1String("KYTY_STREAM_"))) env.remove(key);
		}
		env.insert(QStringLiteral("KYTY_STREAM_FRAMES"), QStringLiteral("0"));
	}
	for (const auto& key: env.keys()) {
		if (!IsReservedEnvName(key)) continue;
		fprintf(stderr, "kyty-ags-adapter: not passing reserved environment name %s\n",
		        qPrintable(key));
		env.remove(key);
	}

	QJsonArray argv {exe};
	for (const auto& a: args)
		argv.append(a);
	const auto players =
	    std::clamp(static_cast<int>(params[QStringLiteral("controllers")].toArray().size()), 1,
		           Config::MAX_PLAYERS);
	QJsonArray controllers;
	for (int i = 0; i < players; ++i)
		controllers.append(QStringLiteral("dualsense"));

	applied.insert(QStringLiteral("resolution"), QStringLiteral("%1x%2").arg(width).arg(height));
	applied.insert(QStringLiteral("fps_limit"), fps);
	applied.insert(QStringLiteral("aspect"), info.stream_aspect_mode);
	applied.insert(QStringLiteral("language"), LanguageTag(info.console_language));
	applied.insert(QStringLiteral("player_name"), info.user_name);
	applied.insert(QStringLiteral("controller"), QStringLiteral("dualsense"));
	applied.insert(QStringLiteral("fullscreen"), info.fullscreen_enabled);
	applied.insert(QStringLiteral("hide_cursor"), info.hide_cursor_enabled);
	applied.insert(QStringLiteral("hdr"), info.hdr_enabled);

	return Reply::Success({{"argv", argv},
	                       {"env", env},
	                       {"cwd", runtime_root},
	                       {"writable", QJsonArray {runtime_root}},
	                       {"native_frames", native},
	                       {"controllers", controllers},
	                       {"applied", applied}});
}

} // namespace AdapterLaunch
