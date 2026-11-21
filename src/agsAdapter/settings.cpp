#include "settings.h"

#include "adapterState.h"
#include "configuration.h"
#include "gameLibrary.h"
#include "ownerPreferences.h"
#include "preferenceSchema.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QSettings>
#include <QTemporaryDir>

#include <cmath>

namespace AdapterSettings {

namespace {

// Global-only values that are not in Configuration::GameSettings() (ownerPreferences.cpp Globals).
struct GlobalDef {
	const char* key;
	const char* label;
	const char* type; // bool, int, enum or string
	int         max;
	const char* unit; // int only: a percentage is drawn as a slider
};

constexpr char PERCENT[]         = "%";
constexpr char KEYBOARD_PLAYER[] = "controller_keyboard_player";

const GlobalDef GLOBALS[] = {
    {"controller_color", "Controller light bar colour", "string", 0, nullptr},
    {"controller_speaker_volume", "Controller speaker volume", "int", 100, PERCENT},
    {"controller_vibration_intensity", "Controller vibration", "int", 100, PERCENT},
    // Stored as the player number (0 shares player 1); offered as an enum of that number's text.
    {KEYBOARD_PLAYER, "Keyboard player", "enum", Config::MAX_PLAYERS, nullptr},
    {"audio_master_volume", "Master volume", "int", static_cast<int>(Config::MAX_AUDIO_VOLUME),
     PERCENT},
    {"audio_main_volume", "Main volume", "int", static_cast<int>(Config::MAX_AUDIO_VOLUME), PERCENT},
    {"audio_music_volume", "Music volume", "int", static_cast<int>(Config::MAX_AUDIO_VOLUME),
     PERCENT},
    {"audio_pad_speaker_main_volume", "Controller speaker on main output", "int",
	 static_cast<int>(Config::MAX_AUDIO_VOLUME), PERCENT},
    {"audio_objects_volume", "Audio objects volume", "int",
	 static_cast<int>(Config::MAX_AUDIO_VOLUME), PERCENT},
    {"audio_objects_enabled", "Audio objects", "bool", 0, nullptr},
    {"gpu_occlusion_accurate", "Accurate GPU occlusion", "bool", 0, nullptr},
};

// Suggested light bar colours (STANDARD.md section 6.2 `palette`); any #rrggbb is allowed.
const char* const PALETTE[][2] = {
    {"#0030ff", "Blue"},  {"#ff0000", "Red"},    {"#00ff00", "Green"},  {"#ff40a0", "Pink"},
    {"#ffffff", "White"}, {"#ffa000", "Orange"}, {"#8000ff", "Purple"}, {"#00e0ff", "Cyan"},
};

// Enums drawn as a segmented control (2 to 4 short options).
bool Segmented(const QString& key) {
	return key == QLatin1String("present_mode") || key == QLatin1String("amd_cpu_mode");
}

QJsonValue GlobalValue(const Configuration& c, const QString& key) {
	if (key == QLatin1String("controller_color")) return c.controller.color;
	if (key == QLatin1String("controller_speaker_volume")) return c.controller.speaker_volume;
	if (key == QLatin1String("controller_vibration_intensity"))
		return c.controller.vibration_intensity;
	if (key == QLatin1String(KEYBOARD_PLAYER))
		return QString::number(c.controller.keyboard_player);
	if (key == QLatin1String("audio_master_volume")) return c.audio_mix.master;
	if (key == QLatin1String("audio_main_volume")) return c.audio_mix.main;
	if (key == QLatin1String("audio_music_volume")) return c.audio_mix.music;
	if (key == QLatin1String("audio_pad_speaker_main_volume")) return c.audio_mix.pad_speaker;
	if (key == QLatin1String("audio_objects_volume")) return c.audio_mix.objects;
	if (key == QLatin1String("audio_objects_enabled")) return c.audio_mix.objects_enabled;
	if (key == QLatin1String("gpu_occlusion_accurate")) return c.gpu_occlusion_accurate;
	return {};
}

void SetGlobalValue(Configuration& c, const QString& key, const QJsonValue& v) {
	const int n = v.toInt();
	if (key == QLatin1String("controller_color"))
		c.controller.color = v.toString();
	else if (key == QLatin1String("controller_speaker_volume"))
		c.controller.speaker_volume = n;
	else if (key == QLatin1String("controller_vibration_intensity"))
		c.controller.vibration_intensity = n;
	else if (key == QLatin1String(KEYBOARD_PLAYER))
		c.controller.keyboard_player = v.isString() ? v.toString().toInt() : n;
	else if (key == QLatin1String("audio_master_volume"))
		c.audio_mix.master = n;
	else if (key == QLatin1String("audio_main_volume"))
		c.audio_mix.main = n;
	else if (key == QLatin1String("audio_music_volume"))
		c.audio_mix.music = n;
	else if (key == QLatin1String("audio_pad_speaker_main_volume"))
		c.audio_mix.pad_speaker = n;
	else if (key == QLatin1String("audio_objects_volume"))
		c.audio_mix.objects = n;
	else if (key == QLatin1String("audio_objects_enabled"))
		c.audio_mix.objects_enabled = v.toBool();
	else if (key == QLatin1String("gpu_occlusion_accurate"))
		c.gpu_occlusion_accurate = v.toBool();
}

// Group headings and the keys under each, in order. Keys of GameSettings() that are not listed
// go under "Other" as strings, so a new setting never vanishes.
struct Group {
	const char*              key;
	const char*              label;
	std::vector<const char*> fields;
};

// Install-relative SVG under assets/ags/sections/ named after the group key.
QString SectionIcon(const QString& group_key) {
	return QStringLiteral("assets/ags/sections/%1.svg").arg(group_key.mid(6));
}

const std::vector<Group>& Groups() {
	static const std::vector<Group> groups = {
	    {"group.display",
		 "Display",
		 {"present_mode", "gpu_index", "low_latency_enabled", "low_latency_fps_limit"}},
	    {"group.user", "User", {"user_id"}},
	    {"group.audio",
		 "Audio",
		 {"audio_input_device", "audio_master_volume", "audio_main_volume", "audio_music_volume",
		  "audio_pad_speaker_main_volume", "audio_objects_volume", "audio_objects_enabled"}},
	    {"group.controller",
		 "Controller",
		 {"controller_color", "controller_speaker_volume", "controller_vibration_intensity",
		  "controller_keyboard_player"}},
	    {"group.graphics",
		 "Graphics",
		 {"upscaler", "dlss_mode", "frame_generation", "frame_generation_flow_scale",
		  "frame_generation_performance_mode", "native_resolution_enabled",
		  "readback_linear_images", "readback_tiled_images", "sync_raw_image_buffers",
		  "support_zmask", "tessellation_enabled", "gpu_occlusion_accurate",
		  "shader_optimization_type"}},
	    {"group.system",
		 "System",
		 {"trophy_enabled", "trophy_duration_seconds", "skip_notice_screen", "network_connected",
		  "amd_cpu_mode", "red_zone_protection_enabled"}},
	    {"group.debug",
		 "Debugging",
		 {"vulkan_validation_enabled", "shader_validation_enabled", "shader_log_direction",
		  "command_buffer_dump_enabled", "printf_direction", "profiler_enabled",
		  "renderdoc_enabled"}},
	    {"group.machine",
		 "This machine",
		 {"runtime_root", "game_dirs", "addons_dir", "host_input_mapping"}},
	};
	return groups;
}

// Keys never offered: standard settings, path settings (PreferenceSchema::PATH_KEYS) and the
// performance overlay, which a launch by AGS always turns off (STANDARD.md section 8.2).
bool Hidden(const QString& key) {
	return CoveredByStandard(key) || key == QLatin1String("elf") ||
	       key == QLatin1String("shader_log_folder") ||
	       key == QLatin1String("command_buffer_dump_folder") ||
	       key == QLatin1String("printf_output_file") ||
	       key == QLatin1String("perf_overlay_enabled");
}

bool IsGameKey(const QString& key) {
	return Configuration().GameSettings().contains(key);
}

// An enum whose options are fixed (not the GPU or microphone list).
bool IsClosedEnum(const QJsonObject& field, const QString& key) {
	return field[QStringLiteral("type")].toString() == QLatin1String("enum") &&
	       key != QLatin1String("gpu_index") && key != QLatin1String("audio_input_device");
}

bool IsGlobalKey(const QString& key) {
	for (const auto& g: GLOBALS) {
		if (key == QLatin1String(g.key)) return true;
	}
	return false;
}

// Value that means "let the system choose" for a device-list field: "-1" for gpu_index
// (Configuration's auto GPU), "" for audio_input_device (no microphone selected).
bool IsAutoDeviceValue(const QString& key, const QString& value) {
	return (key == QLatin1String("gpu_index") && value == QLatin1String("-1")) ||
	       (key == QLatin1String("audio_input_device") && value.isEmpty());
}

QJsonArray Options(const QJsonObject& field, const QString& key) {
	QJsonArray options;
	if (field.contains(QStringLiteral("options"))) {
		for (const auto& o: field[QStringLiteral("options")].toArray()) {
			const auto obj   = o.toObject();
			const auto v     = obj[QStringLiteral("value")];
			const auto value = v.isString() ? v.toString() : QString::number(v.toInteger());
			auto       label = obj[QStringLiteral("label")].toString();
			if (label.isEmpty() || IsAutoDeviceValue(key, value)) label = QStringLiteral("Default");
			options.append(QJsonObject {{"value", value}, {"label", label.left(80)}});
		}
	} else {
		for (const auto& c: field[QStringLiteral("choices")].toArray()) {
			options.append(QJsonObject {{"value", c.toString()}, {"label", c.toString()}});
		}
	}
	return options;
}

// One OwnerPreferences::SettingsFields entry (value = the configuration.h default) as a schema
// field.
QJsonObject Convert(const QJsonObject& in) {
	const auto  key  = in[QStringLiteral("key")].toString();
	auto        type = in[QStringLiteral("type")].toString();
	const auto  raw  = in[QStringLiteral("value")].toString();
	QJsonObject out {{"key", key},
	                 {"label", in[QStringLiteral("label")].toString().left(80)},
	                 {"scope", "both"},
	                 {"location", key == QLatin1String("gpu_index") ? "machine" : "player"}};
	// Device lists (GPU, microphone) arrive as options on int/string fields: offer them as enums.
	if (in.contains(QStringLiteral("options")) || type == QLatin1String("enum")) {
		type                           = QStringLiteral("enum");
		out[QStringLiteral("options")] = Options(in, key);
	}
	out[QStringLiteral("type")] = type;
	if (type == QLatin1String("int")) {
		out[QStringLiteral("min")]  = in[QStringLiteral("min")].toInteger();
		out[QStringLiteral("max")]  = in[QStringLiteral("max")].toInteger();
		out[QStringLiteral("step")] = 1;
	}
	if (key == QLatin1String("gpu_index")) {
		out[QStringLiteral("scope")] = "global";
		out[QStringLiteral("hint")]  = "Vulkan device the emulator renders on.";
	}
	if (key == QLatin1String("trophy_duration_seconds")) out[QStringLiteral("unit")] = "s";
	if (key == QLatin1String("frame_generation")) {
		out[QStringLiteral("hint")] =
		    "Linux, with lsfg-vk and Lossless Scaling installed: generated frames between the "
		    "game's own (Double: 30 to 60 FPS). Turns on V-Sync. Off also turns off an lsfg-vk "
		    "profile for the emulator.";
	}
	if (key == QLatin1String("frame_generation_flow_scale")) {
		out[QStringLiteral("unit")]    = "%";
		out[QStringLiteral("control")] = "slider";
		out[QStringLiteral("hint")] =
		    "Resolution of the motion estimate frames are generated from. Lower is faster.";
	}
	if (key == QLatin1String("frame_generation_performance_mode")) {
		out[QStringLiteral("hint")] = "A lighter frame model: faster, with more visible artefacts.";
	}
	if (key == QLatin1String("low_latency_fps_limit")) {
		out[QStringLiteral("unit")] = "FPS";
		out[QStringLiteral("hint")] =
		    "With low latency on: the driver paces frames to this rate before "
		    "the game starts each one. 0 for no limit.";
	}
	const auto count = out.value(QStringLiteral("options")).toArray().size();
	if (type == QLatin1String("enum") && Segmented(key) && count >= 2 && count <= 4)
		out[QStringLiteral("control")] = "segmented";
	out[QStringLiteral("default")] = Typed(out, raw);
	return out;
}

QJsonObject GlobalField(const GlobalDef& g) {
	QJsonObject out {{"key", g.key},
	                 {"label", g.label},
	                 {"type", g.type},
	                 {"scope", "global"},
	                 {"location", "player"}};
	if (QLatin1String(g.type) == QLatin1String("int")) {
		out[QStringLiteral("min")]  = 0;
		out[QStringLiteral("max")]  = g.max;
		out[QStringLiteral("step")] = 1;
		if (g.unit != nullptr) {
			out[QStringLiteral("unit")]    = g.unit;
			out[QStringLiteral("control")] = "slider";
		}
	}
	if (QLatin1String(g.key) == QLatin1String(KEYBOARD_PLAYER)) {
		// The labels of the Qt launcher's dialog and the old daemon.
		QJsonArray options {QJsonObject {{"value", "0"}, {"label", "Shared with player 1"}}};
		for (int player = 1; player <= g.max; ++player) {
			options.append(QJsonObject {{"value", QString::number(player)},
			                            {"label", QStringLiteral("Player %1").arg(player)}});
		}
		out[QStringLiteral("options")] = options;
	}
	if (QLatin1String(g.key) == QLatin1String("controller_color")) {
		out[QStringLiteral("hint")]       = "Empty lets the game set the colour.";
		out[QStringLiteral("format")]     = "colour";
		out[QStringLiteral("nullable")]   = true;
		out[QStringLiteral("null_label")] = "Game default";
		QJsonArray palette;
		for (const auto& p: PALETTE)
			palette.append(QJsonObject {{"value", p[0]}, {"label", p[1]}});
		out[QStringLiteral("palette")] = palette;
	}
	out[QStringLiteral("default")] = GlobalValue(Configuration(), QString::fromLatin1(g.key));
	return out;
}

constexpr int MAPPING_MAX_ITEMS = 256;
constexpr int MAPPING_MAX_LEN   = 1024;

// A string_list value within the schema limits; false when it is not a value of the field.
bool MappingList(const QJsonValue& value) {
	if (!value.isArray() || value.toArray().size() > MAPPING_MAX_ITEMS) return false;
	for (const auto& v: value.toArray()) {
		if (!v.isString() || v.toString().toUcs4().size() > MAPPING_MAX_LEN ||
		    v.toString().contains(QChar(u'\0')))
			return false;
	}
	return true;
}

QJsonObject MachineField(const QString& key) {
	QJsonObject out {{"key", key}, {"scope", "global"}, {"location", "machine"}};
	if (key == QLatin1String("runtime_root")) {
		out[QStringLiteral("type")]    = "path";
		out[QStringLiteral("label")]   = "Runtime folder";
		out[QStringLiteral("hint")]    = "Holds Kyty.ini, saves, trophies, caches and cheats.";
		out[QStringLiteral("default")] = "";
		out[QStringLiteral("restart_required")] = true;
	} else if (key == QLatin1String("game_dirs")) {
		out[QStringLiteral("type")]    = "path_list";
		out[QStringLiteral("label")]   = "Game folders";
		out[QStringLiteral("default")] = QJsonArray {};
	} else if (key == QLatin1String("addons_dir")) {
		out[QStringLiteral("type")]    = "path";
		out[QStringLiteral("label")]   = "Additional content folder";
		out[QStringLiteral("default")] = "";
	} else {
		out[QStringLiteral("type")]      = "string_list";
		out[QStringLiteral("label")]     = "Input mapping";
		out[QStringLiteral("default")]   = QJsonArray::fromStringList(Configuration().host_input_mapping);
		out[QStringLiteral("max_items")] = MAPPING_MAX_ITEMS;
		out[QStringLiteral("max_len")]   = MAPPING_MAX_LEN;
	}
	return out;
}

QJsonArray BuildSchema() {
	QHash<QString, QJsonObject> by_key;
	QStringList                 game_order;
	for (const auto& v: OwnerPreferences::SettingsFields(Configuration())) {
		const auto f   = v.toObject();
		const auto key = f[QStringLiteral("key")].toString();
		if (Hidden(key)) continue;
		by_key.insert(key, Convert(f));
		game_order.append(key);
	}
	for (const auto& g: GLOBALS) {
		by_key.insert(QString::fromLatin1(g.key), GlobalField(g));
	}
	for (const auto* key: {"runtime_root", "game_dirs", "addons_dir", "host_input_mapping"}) {
		by_key.insert(QString::fromLatin1(key), MachineField(QString::fromLatin1(key)));
	}
	QJsonArray  fields;
	QStringList placed;
	for (const auto& group: Groups()) {
		QJsonArray members;
		for (const auto* name: group.fields) {
			const auto key = QString::fromLatin1(name);
			if (by_key.contains(key)) {
				members.append(by_key.value(key));
				placed.append(key);
			}
		}
		if (members.isEmpty()) continue;
		fields.append(QJsonObject {{"key", group.key},
		                           {"label", group.label},
		                           {"type", "group"},
		                           {"icon", SectionIcon(QString::fromLatin1(group.key))}});
		for (const auto& m: members)
			fields.append(m);
	}
	QJsonArray other;
	for (const auto& key: game_order) {
		if (!placed.contains(key)) other.append(by_key.value(key));
	}
	if (!other.isEmpty()) {
		fields.append(QJsonObject {{"key", "group.other"},
		                           {"label", "Other"},
		                           {"type", "group"},
		                           {"icon", SectionIcon(QStringLiteral("group.other"))}});
		for (const auto& m: other)
			fields.append(m);
	}
	return fields;
}

void LoadGlobal(QSettings& s, Configuration& c) {
	// Same as OwnerPreferences::Read.
	s.beginGroup(QStringLiteral("GlobalConfiguration"));
	if (!s.childKeys().isEmpty()) c.ReadSettings(&s);
	c.controller.ReadSettings(&s);
	c.ReadGlobalOnlySettings(&s);
	s.endGroup();
}


QString FieldScope(const QJsonObject& f) {
	return f[QStringLiteral("scope")].toString(QStringLiteral("global"));
}

} // namespace

bool CoveredByStandard(const QString& key) {
	return key == QLatin1String("screen_resolution") || key == QLatin1String("vblank_frequency") ||
	       key == QLatin1String("stream_aspect_mode") || key == QLatin1String("console_language") ||
	       key == QLatin1String("user_name") ||
	       key == QLatin1String("fullscreen_enabled") || key == QLatin1String("hide_cursor_enabled") ||
	       key == QLatin1String("hdr_enabled");
}

const QJsonArray& Schema() {
	static const QJsonArray schema = BuildSchema();
	return schema;
}

const QJsonObject* FindField(const QString& key) {
	static const QHash<QString, QJsonObject> index = [] {
		QHash<QString, QJsonObject> h;
		for (const auto& v: Schema()) {
			const auto f = v.toObject();
			if (f[QStringLiteral("type")].toString() != QLatin1String("group"))
				h.insert(f[QStringLiteral("key")].toString(), f);
		}
		return h;
	}();
	const auto it = index.constFind(key);
	return it == index.constEnd() ? nullptr : &it.value();
}

bool OptionValue(const QJsonObject& field, const QString& text, QString* out) {
	const auto options = field[QStringLiteral("options")].toArray();
	// The launcher's raw spelling ("R1920X1080") is an alias of the choice text ("1920x1080").
	for (const auto& candidate: {text, PreferenceSchema::ChoiceText(text)}) {
		for (const auto& o: options) {
			if (o.toObject()[QStringLiteral("value")].toString() == candidate) {
				*out = candidate;
				return true;
			}
		}
	}
	return false;
}

QJsonValue Typed(const QJsonObject& field, const QString& stored) {
	const auto type = field[QStringLiteral("type")].toString();
	if (type == QLatin1String("enum")) {
		QString option;
		return OptionValue(field, stored, &option) ? option : stored;
	}
	if (type == QLatin1String("bool")) return stored == QLatin1String("true");
	if (type == QLatin1String("int")) return stored.toLongLong();
	return stored;
}

bool Stored(const QJsonObject& field, const QJsonValue& value, QString* out) {
	const auto type = field[QStringLiteral("type")].toString();
	if (type == QLatin1String("bool")) {
		if (!value.isBool()) return false;
		*out = value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
		return true;
	}
	if (type == QLatin1String("int")) {
		const double n = value.toDouble(std::nan(""));
		if (!value.isDouble() || !std::isfinite(n) || n != std::floor(n)) return false;
		const auto i = static_cast<qint64>(n);
		if (i < field[QStringLiteral("min")].toInteger() ||
		    i > field[QStringLiteral("max")].toInteger())
			return false;
		*out = QString::number(i);
		return true;
	}
	if (!value.isString() || value.toString().size() > 4096 ||
	    value.toString().contains(QChar(u'\0')))
		return false;
	*out = value.toString();
	return true;
}

bool ApplyPlayer(Configuration& info, const QJsonObject& player, QJsonObject* applied,
                 QString* error) {
	QJsonObject game_values;
	for (auto it = player.constBegin(); it != player.constEnd(); ++it) {
		const auto* field = FindField(it.key());
		if (field == nullptr ||
		    (*field)[QStringLiteral("location")].toString() != QLatin1String("player")) {
			continue; // unknown here, or not a player value
		}
		QString text;
		if (!Stored(*field, it.value(), &text)) {
			*error = QStringLiteral("Invalid value for %1.").arg(it.key());
			return false;
		}
		QJsonValue value = it.value();
		if (IsClosedEnum(*field, it.key())) {
			if (!OptionValue(*field, text, &text)) {
				*error = QStringLiteral("Invalid value for %1.").arg(it.key());
				return false;
			}
			value = text;
		}
		if (IsGlobalKey(it.key())) {
			if (it.key() == QLatin1String("controller_color")) {
				// --controller-color takes exactly #rrggbb; anything else stops the emulator.
				if (!NormalControllerColor(text, &text)) {
					*error = QStringLiteral("Invalid controller colour.");
					return false;
				}
				value = text;
			}
			SetGlobalValue(info, it.key(), value);
		} else {
			game_values.insert(it.key(), IsClosedEnum(*field, it.key()) ? PreferenceSchema::RawText(text) : text);
		}
		applied->insert(it.key(), value);
	}
	if (!game_values.isEmpty()) {
		Configuration candidate;
		candidate.CopyEmulatorSettingsFrom(info);
		candidate.elf = info.elf;
		if (!candidate.SetGameSettings(game_values, *error)) return false;
		info.CopyEmulatorSettingsFrom(candidate);
	}
	return true;
}

bool IniReadable(const QString& ini) {
	if (!QFileInfo::exists(ini)) return true; // QSettings creates it
	// QSettings reports a parse error only the first time a file name is opened in a process, so
	// parse a private copy under a new name every time.
	const QTemporaryDir copy;
	const auto          path = copy.filePath(QStringLiteral("check.ini"));
	if (!copy.isValid() || !QFile::copy(ini, path)) return false;
	QSettings s(path, QSettings::IniFormat);
	(void)s.allKeys(); // loads the file
	return s.status() == QSettings::NoError;
}

Reply Get(AdapterState& state, const QString& ini, const GameEntry* game) {
	QJsonObject values;
	if (game != nullptr) {
		const auto stored = game->info->GameSettings();
		for (auto it = stored.constBegin(); it != stored.constEnd(); ++it) {
			const auto* field = FindField(it.key());
			if (field != nullptr && FieldScope(*field) != QLatin1String("global"))
				values.insert(it.key(), Typed(*field, it.value().toString()));
		}
		return Reply::Success({{"values", values}});
	}
	auto          s = GameLibraryFiles::OpenSettings(ini);
	Configuration c;
	LoadGlobal(*s, c);
	const auto stored = c.GameSettings();
	for (auto it = stored.constBegin(); it != stored.constEnd(); ++it) {
		if (const auto* field = FindField(it.key()); field != nullptr)
			values.insert(it.key(), Typed(*field, it.value().toString()));
	}
	for (const auto& g: GLOBALS) {
		values.insert(QString::fromLatin1(g.key), GlobalValue(c, QString::fromLatin1(g.key)));
	}
	auto dirs = s->value(QStringLiteral("Launcher/game_dirs")).toStringList();
	if (dirs.isEmpty()) dirs = s->value(QStringLiteral("Launcher/game_dir")).toStringList();
	values.insert(QStringLiteral("game_dirs"), QJsonArray::fromStringList(dirs));
	values.insert(QStringLiteral("addons_dir"),
	              s->value(QStringLiteral("Launcher/addons_dir")).toString());
	values.insert(QStringLiteral("host_input_mapping"),
	              QJsonArray::fromStringList(c.host_input_mapping));
	values.insert(QStringLiteral("runtime_root"), state.StoredRuntimeRoot());
	return Reply::Success({{"values", values}});
}

Reply Set(AdapterState& state, const QString& ini, GameLibrary& library, const GameEntry* game,
          const QJsonObject& values) {
	// Every key and value is validated before anything is written, so a request is applied
	// whole or not at all. An unreadable Kyty.ini would be replaced by what we write: refuse.
	if (!IniReadable(ini)) return Reply::Failed(QString::fromLatin1(INI_UNREADABLE));
	QJsonObject settings;
	QJsonObject globals;
	QJsonObject patch;
	QString     runtime_root;
	bool        set_runtime_root = false;
	for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
		const auto* field = FindField(it.key());
		if (field == nullptr) {
			return Reply::Invalid(QStringLiteral("Unknown setting %1.").arg(it.key()));
		}
		const auto scope = FieldScope(*field);
		if (game != nullptr && scope == QLatin1String("global")) {
			return Reply::Invalid(QStringLiteral("%1 cannot be set per game.").arg(it.key()));
		}
		QJsonValue value = it.value();
		if (value.isNull()) { // machine: back to the item's default
			if (game != nullptr) {
				return Reply::Invalid(QStringLiteral("%1 cannot be reset per game.").arg(it.key()));
			}
			value = (*field)[QStringLiteral("default")];
		}
		const auto type = (*field)[QStringLiteral("type")].toString();
		if (type == QLatin1String("path_list")) {
			if (!value.isArray()) {
				return Reply::Invalid(QStringLiteral("%1 must be a list of paths.").arg(it.key()));
			}
			patch.insert(it.key(), value);
			continue;
		}
		if (type == QLatin1String("string_list")) {
			if (!MappingList(value)) {
				return Reply::Invalid(QStringLiteral("Invalid value for %1.").arg(it.key()));
			}
			patch.insert(it.key(), value);
			continue;
		}
		QString text;
		if (!Stored(*field, value, &text)) {
			return Reply::Invalid(QStringLiteral("Invalid value for %1.").arg(it.key()));
		}
		if (IsClosedEnum(*field, it.key()) && !OptionValue(*field, text, &text)) {
			return Reply::Invalid(QStringLiteral("Invalid value for %1.").arg(it.key()));
		}
		if (type == QLatin1String("path") && !text.isEmpty() && !QDir::isAbsolutePath(text)) {
			return Reply::Invalid(QStringLiteral("%1 must be an absolute path.").arg(it.key()));
		}
		if (it.key() == QLatin1String("runtime_root")) {
			QString error;
			if (!AdapterState::CheckRuntimeRoot(text, &error)) return Reply::Invalid(error);
			runtime_root     = text;
			set_runtime_root = true;
		} else if (it.key() == QLatin1String("addons_dir")) {
			patch.insert(it.key(), text);
		} else if (it.key() == QLatin1String("controller_color")) {
			if (!NormalControllerColor(text, &text)) {
				return Reply::Invalid(QStringLiteral("Invalid value for %1.").arg(it.key()));
			}
			globals.insert(it.key(), text);
		} else if (it.key() == QLatin1String(KEYBOARD_PLAYER)) {
			globals.insert(it.key(), text.toInt()); // Kyty.ini keeps the player number
		} else if (IsGlobalKey(it.key())) {
			globals.insert(it.key(), value);
		} else if (IsGameKey(it.key())) {
			settings.insert(it.key(), IsClosedEnum(*field, it.key()) ? PreferenceSchema::RawText(text) : text);
		}
	}
	if (game != nullptr) {
		if (!settings.isEmpty()) {
			const auto error = library.SaveGameSettings(game->id, settings);
			if (!error.isEmpty()) {
				return error == QLatin1String("write-failed")
				           ? Reply::Failed(QStringLiteral("Cannot write %1.").arg(ini))
						   : Reply::Invalid(error);
			}
		}
		return Get(state, ini, library.Find(game->id));
	}
	if (!settings.isEmpty()) patch.insert(QStringLiteral("settings"), settings);
	if (!globals.isEmpty()) patch.insert(QStringLiteral("global"), globals);
	if (!patch.isEmpty()) {
		auto    s = GameLibraryFiles::OpenSettings(ini);
		QString error;
		if (!OwnerPreferences::Write(*s, patch, error)) {
			return error.startsWith(QLatin1String("Cannot save")) ? Reply::Failed(error)
			                                                      : Reply::Invalid(error);
		}
	}
	auto reply_ini = ini;
	if (set_runtime_root) {
		QString error;
		if (!state.SetRuntimeRoot(runtime_root, &error)) return Reply::Failed(error);
		// The reply shows the new root's Kyty.ini.
		const auto root = state.RuntimeRoot(&error);
		if (root.isEmpty()) return Reply::Failed(error);
		reply_ini = AdapterState::IniPath(root);
	}
	return Get(state, reply_ini, nullptr);
}

} // namespace AdapterSettings
