#include "emulatorCommand.h"

#include "cheatFile.h"
#include "configuration.h"
#include "gameContent.h"

#include <QDir>
#include <QFileInfo>

static QString BoolArg(bool value) {
	return value ? QStringLiteral("true") : QStringLiteral("false");
}

QStringList BuildEmulatorArgs(const Configuration& info) {
	QStringList args;
	auto        r = EnumToText(info.screen_resolution).split('x');

	if (r.size() != 2) {
		return {};
	}

	args << "--screen-width" << r.at(0);
	args << "--screen-height" << r.at(1);
	args << "--user-name" << info.user_name;
	args << "--user-id" << QString::number(info.user_id);
	if (!info.audio_input_device.isEmpty()) {
		args << "--mic" << info.audio_input_device;
	}
	if (!info.controller.color.isEmpty()) {
		args << "--controller-color" << info.controller.color;
	}
	args << "--controller-volume" << QString::number(info.controller.speaker_volume);
	args << "--controller-vibration" << QString::number(info.controller.vibration_intensity);
	args << "--audio-master-volume" << QString::number(info.audio_mix.master);
	args << "--audio-main-volume" << QString::number(info.audio_mix.main);
	args << "--audio-music-volume" << QString::number(info.audio_mix.music);
	args << "--audio-pad-speaker-volume" << QString::number(info.audio_mix.pad_speaker);
	args << "--audio-objects-volume" << QString::number(info.audio_mix.objects);
	args << "--audio-objects" << (info.audio_mix.objects_enabled ? "on" : "off");
	if (info.gpu_occlusion_accurate) {
		// Overrides the bundled preset's KYTY_GPU_OCCLUSION (performance mode by default).
		args << "--gpu-occlusion" << "on";
	}
	args << "--present-mode" << EnumToText(info.present_mode);
	if (info.gpu_index >= 0) {
		args << "--gpu" << QString::number(info.gpu_index);
	}
	if (info.fullscreen_enabled) {
		args << "--fullscreen";
	}
	if (info.hdr_enabled) {
		args << "--hdr";
	}
	if (info.hide_cursor_enabled) {
		args << "--hide-cursor";
	}
	args << "--readback-linear-images" << BoolArg(info.readback_linear_images);
	args << "--sync-raw-image-buffers" << BoolArg(info.sync_raw_image_buffers);
	args << "--trophy-notifications" << BoolArg(info.trophy_enabled);
	args << "--network-connected" << BoolArg(info.network_connected);
	args << "--skip-notice-screen" << BoolArg(info.skip_notice_screen);
	if (info.tessellation_enabled) {
		args << "--tessellation";
	}
	args << "--vblank-frequency" << QString::number(info.vblank_frequency);
	args << "--console-language" << QString::number(info.console_language);
	args << "--vulkan-validation" << BoolArg(info.vulkan_validation_enabled);
	args << "--shader-validation" << BoolArg(info.shader_validation_enabled);
	args << "--shader-optimization-type" << EnumToText(info.shader_optimization_type);
	args << "--shader-log-direction" << EnumToText(info.shader_log_direction);
	args << "--shader-log-folder" << info.shader_log_folder;
	if (qEnvironmentVariable("KYTY_CAPTURE_SHADER_DETAILS") == QStringLiteral("1")) {
		args << "--graphics-debug-dump" << "true";
	}
	args << "--command-buffer-dump" << BoolArg(info.command_buffer_dump_enabled);
	args << "--command-buffer-dump-folder" << info.command_buffer_dump_folder;
	args << "--printf-direction" << EnumToText(info.printf_direction);
	args << "--printf-output-file" << info.printf_output_file;
	if (info.profiler_enabled) {
		args << "--profile";
	}
	args << "--spirv-debug-printf" << "false";
	// Neither flag: the emulator decides from the CPU vendor.
	if (info.amd_cpu_mode == QStringLiteral("on")) {
		args << "--amd-cpu";
	} else if (info.amd_cpu_mode == QStringLiteral("off")) {
		args << "--no-amd-cpu";
	}
#if defined(_WIN32)
	if (info.red_zone_protection_enabled) {
		args << "--redzone";
	}
#endif
	for (const auto& binding: info.host_input_mapping) {
		args << "--keymap" << binding;
	}
	if (info.renderdoc_enabled) {
		args << "--rd";
	}

	QString game = info.basedir;
	if (!info.elf.isEmpty() && !GameContent::IsArchive(info.basedir)) {
		game = QDir(info.basedir).filePath(info.elf);
	}
	args << "--game" << game;
	if (!info.addons_dir.isEmpty()) {
		args << "--addcont-dir" << info.addons_dir;
	}

	const auto patch_plan = Cheats::PlanPath(info.title_id);
	if (QFileInfo::exists(patch_plan)) {
		args << "--game-patch" << patch_plan;
	}

	return args;
}
