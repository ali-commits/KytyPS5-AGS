#include "ownerPreferences.h"

#include "configuration.h"
#include "preferenceSchema.h"
#include "preferenceDevices.h"
#include <QLibrary>
#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>
#include <array>

#include <QDir>
#include <QJsonArray>
#include <QSet>

#include <cmath>

namespace {
QStringList GpuNames() {
    // The owner daemon has QCoreApplication, so do not create a Qt Vulkan window.
    QLibrary loader("vulkan");
    const auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(loader.resolve("vkGetInstanceProcAddr"));
    if (!get) return {};
    const auto create = reinterpret_cast<PFN_vkCreateInstance>(get(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!create) return {};
    VkInstanceCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    VkInstance instance = VK_NULL_HANDLE;
    if (create(&info, nullptr, &instance) != VK_SUCCESS) return {};
    const auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(get(instance, "vkDestroyInstance"));
    const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get(instance, "vkEnumeratePhysicalDevices"));
    const auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(get(instance, "vkGetPhysicalDeviceProperties"));
    QStringList names;
    if (enumerate && properties) {
        std::array<VkPhysicalDevice, 64> devices {};
        uint32_t count = devices.size();
        const auto result = enumerate(instance, &count, devices.data());
        if (result == VK_SUCCESS || result == VK_INCOMPLETE) {
            for (uint32_t i = 0; i < count && i < devices.size(); ++i) {
                VkPhysicalDeviceProperties value {};
                properties(devices[i], &value);
                names.append(QString::fromUtf8(value.deviceName));
            }
        }
    }
    if (destroy) destroy(instance, nullptr);
    return names;
}
QStringList MicrophoneNames() {
    QStringList names;
    // Enumerate only: never open a recording device or start capture.
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return names;
    int count = 0;
    auto* devices = SDL_GetAudioRecordingDevices(&count);
    if (devices) {
        for (int i = 0; i < count && i < 256; ++i)
            if (const auto* name = SDL_GetAudioDeviceName(devices[i])) names.append(QString::fromUtf8(name));
    }
    SDL_free(devices);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return names;
}
QJsonObject Globals(const Configuration& c) {
	return {{"controller_color", c.controller.color},
	        {"controller_speaker_volume", c.controller.speaker_volume},
	        {"controller_vibration_intensity", c.controller.vibration_intensity},
	        {"controller_keyboard_player", c.controller.keyboard_player},
	        {"audio_master_volume", c.audio_mix.master},
	        {"audio_main_volume", c.audio_mix.main},
	        {"audio_music_volume", c.audio_mix.music},
	        {"audio_pad_speaker_main_volume", c.audio_mix.pad_speaker},
	        {"audio_objects_volume", c.audio_mix.objects},
	        {"audio_objects_enabled", c.audio_mix.objects_enabled},
	        {"gpu_occlusion_accurate", c.gpu_occlusion_accurate}};
}
void Load(QSettings& s, Configuration& c) {
	s.beginGroup("GlobalConfiguration");
	if (!s.childKeys().isEmpty()) c.ReadSettings(&s);
	c.controller.ReadSettings(&s);
	c.ReadGlobalOnlySettings(&s);
	s.endGroup();
}
bool Strings(const QJsonValue& value, QStringList& result, bool paths) {
	if (!value.isArray() || value.toArray().size() > 512) return false;
	for (const auto& entry: value.toArray()) {
		if (!entry.isString() || entry.toString().size() > 4096 ||
		    entry.toString().contains(QChar('\0')))
			return false;
		if (paths && (entry.toString().isEmpty() || !QDir::isAbsolutePath(entry.toString())))
			return false;
		result << entry.toString();
	}
	return true;
}
} // namespace
QJsonObject OwnerPreferences::Read(QSettings& s) {
	Configuration c;
	Load(s, c);
	auto dirs = s.value("Launcher/game_dirs").toStringList();
	if (dirs.isEmpty()) dirs = s.value("Launcher/game_dir").toStringList();
	auto fields = PreferenceSchema::Fields(c);
    const auto gpus = GpuNames();
    const auto microphones = MicrophoneNames();
	for (auto i = fields.begin(); i != fields.end(); ++i) {
		auto field = i->toObject();
		field["scope"] = "settings";
        if (field["key"] == "gpu_index") field["options"] = PreferenceDevices::GpuOptions(gpus, c.gpu_index);
        if (field["key"] == "audio_input_device") field["options"] = PreferenceDevices::MicrophoneOptions(microphones, c.audio_input_device);
        if (field["key"] == "console_language") {
            QJsonArray options;
            for (qsizetype n = 0; n < PreferenceSchema::CONSOLE_LANGUAGE_NAMES.size(); ++n)
                options.append(QJsonObject {{"value", QString::number(n)}, {"label", PreferenceSchema::CONSOLE_LANGUAGE_NAMES[n]}});
            field["options"] = options;
        }
		// The owner write API keeps the stored enum spelling.
		if (field["key"] == "screen_resolution") {
			QJsonArray choices;
			for (const auto& choice: field["choices"].toArray()) choices.append(PreferenceSchema::RawText(choice.toString()));
			field["choices"] = choices;
			field["value"] = c.GameSettings().value("screen_resolution").toString();
		}
		*i = field;
	}
	const auto globals = Globals(c);
	for (auto i = globals.begin(); i != globals.end(); ++i) {
		auto label = i.key();
		label.replace('_', ' ');
		label[0] = label[0].toUpper();
		QJsonObject field {{"key", i.key()}, {"label", label}, {"scope", "global"},
		                   {"value", i.value()}, {"type", i.value().isBool() ? "bool" : i.value().isString() ? "string" : "int"}};
		if (i.value().isDouble()) {
			field["min"] = 0;
			field["max"] = i.key() == "controller_keyboard_player" ? Config::MAX_PLAYERS : i.key().startsWith("audio_") ? static_cast<int>(Config::MAX_AUDIO_VOLUME) : 100;
		}
        if (i.key() == "controller_keyboard_player") {
            QJsonArray options {QJsonObject {{"value", 0}, {"label", "Shared with player 1"}}};
            for (int player = 1; player <= Config::MAX_PLAYERS; ++player)
                options.append(QJsonObject {{"value", player}, {"label", QString("Player %1").arg(player)}});
            field["options"] = options;
        }
        if (i.key() == "controller_color") field["type"] = "color";
        if (i.value().isDouble() && i.key() != "controller_keyboard_player") {
            field["control"] = "range";
            field["unit"] = "%";
        }
		fields.append(field);
	}
	return {{"fields", fields},
	        {"settings", QJsonObject::fromVariantMap(c.GameSettings())},
	        {"global", Globals(c)},
	        {"game_dirs", QJsonArray::fromStringList(dirs)},
	        {"addons_dir", s.value("Launcher/addons_dir").toString()},
	        {"host_input_mapping", QJsonArray::fromStringList(c.host_input_mapping)}};
}
bool OwnerPreferences::Write(QSettings& s, const QJsonObject& patch, QString& error) {
	const auto fail = [&error](const QString& text) {
		error = text;
		return false;
	};
	const QSet<QString> allowed {"settings", "global", "game_dirs", "addons_dir",
	                             "host_input_mapping"};
	for (auto i = patch.begin(); i != patch.end(); ++i)
		if (!allowed.contains(i.key())) return fail("Unknown emulator preference: " + i.key());
	Configuration c;
	Load(s, c);
	if (patch.contains("settings")) {
		if (!patch["settings"].isObject()) return fail("settings must be an object.");
		if (!c.SetGameSettings(patch["settings"].toObject(), error)) return false;
	}
	auto global = Globals(c);
	if (patch.contains("global")) {
		if (!patch["global"].isObject()) return fail("global must be an object.");
		const auto values = patch["global"].toObject();
		for (auto i = values.begin(); i != values.end(); ++i) {
			if (!global.contains(i.key())) return fail("Unknown global preference: " + i.key());
			if (i.key() == "controller_color") {
				if (!i.value().isString() ||
				    (!i.value().toString().isEmpty() && !QColor(i.value().toString()).isValid()))
					return fail("Invalid controller color.");
			} else if (global[i.key()].isBool()) {
				if (!i.value().isBool()) return fail("Expected boolean: " + i.key());
			} else {
				const auto max = i.key() == "controller_keyboard_player" ? Config::MAX_PLAYERS
				                 : i.key().startsWith("audio_")
				                     ? static_cast<int>(Config::MAX_AUDIO_VOLUME)
				                     : 100;
				const auto n   = i.value().toDouble(-1);
				if (!i.value().isDouble() || !std::isfinite(n) || n < 0 || n > max ||
				    n != std::floor(n))
					return fail("Invalid integer: " + i.key());
			}
			global[i.key()] = i.value();
		}
	}
	QStringList dirs, mappings;
	if (patch.contains("game_dirs") && !Strings(patch["game_dirs"], dirs, true))
		return fail("game_dirs must contain absolute paths.");
	if (patch.contains("host_input_mapping") &&
	    !Strings(patch["host_input_mapping"], mappings, false))
		return fail("Invalid input mapping list.");
	if (patch.contains("addons_dir") &&
	    (!patch["addons_dir"].isString() || patch["addons_dir"].toString().size() > 4096 ||
	     patch["addons_dir"].toString().contains(QChar('\0')) ||
	     (!patch["addons_dir"].toString().isEmpty() &&
	      !QDir::isAbsolutePath(patch["addons_dir"].toString()))))
		return fail("addons_dir must be empty or an absolute path.");
	// Validate the entire patch before touching any settings; preserve game overrides and unrelated
	// keys.
	s.beginGroup("GlobalConfiguration");
	if (patch.contains("settings"))
		for (auto values = c.GameSettings(); auto key: values.keys())
			s.setValue(key, values[key]);
	if (patch.contains("global"))
		for (auto i = global.begin(); i != global.end(); ++i)
			s.setValue(i.key(), i.value().toVariant());
	if (patch.contains("host_input_mapping")) s.setValue("host_input_mapping", mappings);
	s.endGroup();
	if (patch.contains("game_dirs")) {
		s.setValue("Launcher/game_dirs", dirs);
		s.remove("Launcher/game_dir");
	}
	if (patch.contains("addons_dir"))
		s.setValue("Launcher/addons_dir", patch["addons_dir"].toString());
	s.sync();
	return s.status() == QSettings::NoError || fail("Cannot save emulator preferences.");
}
