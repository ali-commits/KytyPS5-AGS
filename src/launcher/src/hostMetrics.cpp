#include "hostMetrics.h"

#include "common/fpsStats.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <dlfcn.h>

namespace HostMetrics {

namespace {

QByteArray ReadWhole(const QString& path) {
	QFile file(path);
	return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray {};
}

int64_t NowUnixS() {
	return static_cast<int64_t>(QDateTime::currentSecsSinceEpoch());
}

} // namespace

CpuTimes ParseProcStat(const QByteArray& proc_stat) {
	// First line: "cpu  user nice system idle iowait irq softirq steal ...". Busy is everything
	// except idle+iowait; a core-count-independent aggregate.
	for (const auto& line: proc_stat.split('\n')) {
		if (!line.startsWith("cpu ") && line != QByteArray("cpu")) {
			continue;
		}
		const auto fields = QString::fromLatin1(line).split(
		    QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
		if (fields.size() < 5 || fields.first() != QLatin1String("cpu")) {
			return {};
		}
		CpuTimes times;
		uint64_t idle = 0;
		for (int i = 1; i < fields.size(); ++i) {
			bool           ok    = false;
			const uint64_t value = fields.at(i).toULongLong(&ok);
			if (!ok) {
				break;
			}
			times.total += value;
			if (i == 4 || i == 5) { // idle, iowait
				idle += value;
			}
		}
		times.idle = idle;
		times.ok   = times.total > 0;
		return times;
	}
	return {};
}

std::optional<double> CpuUsagePercent(const CpuTimes& before, const CpuTimes& after) {
	if (!before.ok || !after.ok || after.total <= before.total) {
		return std::nullopt;
	}
	const double total_delta = static_cast<double>(after.total - before.total);
	const double idle_delta  = static_cast<double>(after.idle - before.idle);
	const double busy        = 100.0 * (total_delta - idle_delta) / total_delta;
	return std::clamp(busy, 0.0, 100.0);
}

QString ParseCpuName(const QByteArray& proc_cpuinfo) {
	for (const auto& line: proc_cpuinfo.split('\n')) {
		const auto text = QString::fromLatin1(line);
		if (text.startsWith(QLatin1String("model name"))) {
			const auto colon = text.indexOf(QLatin1Char(':'));
			if (colon >= 0) {
				return text.mid(colon + 1).trimmed();
			}
		}
	}
	return {};
}

std::optional<double> PickCpuTemp(const QList<TempReading>& readings) {
	const auto find = [&](auto predicate) -> std::optional<double> {
		for (const auto& reading: readings) {
			if (predicate(reading.name)) {
				return reading.millidegrees / 1000.0;
			}
		}
		return std::nullopt;
	};
	if (auto t = find([](const QString& n) {
		    return n.compare(QLatin1String("Tctl"), Qt::CaseInsensitive) == 0 ||
		           n.compare(QLatin1String("Tdie"), Qt::CaseInsensitive) == 0;
	    })) {
		return t;
	}
	if (auto t = find([](const QString& n) {
		    return n.startsWith(QLatin1String("Package"), Qt::CaseInsensitive);
	    })) {
		return t;
	}
	if (!readings.isEmpty()) {
		return readings.first().millidegrees / 1000.0;
	}
	return std::nullopt;
}

MemInfo ParseMemInfo(const QByteArray& proc_meminfo) {
	MemInfo  info;
	uint64_t total_kb     = 0;
	uint64_t available_kb = 0;
	bool     have_total   = false;
	bool     have_avail   = false;
	for (const auto& line: proc_meminfo.split('\n')) {
		const auto text = QString::fromLatin1(line);
		const auto read = [&](const char* key, uint64_t& out, bool& have) {
			if (text.startsWith(QLatin1String(key))) {
				const auto parts =
				    text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
				if (parts.size() >= 2) {
					bool ok = false;
					out     = parts.at(1).toULongLong(&ok);
					have    = ok;
				}
			}
		};
		read("MemTotal:", total_kb, have_total);
		read("MemAvailable:", available_kb, have_avail);
	}
	if (have_total && have_avail && total_kb >= available_kb) {
		info.ok       = true;
		info.total_mb = total_kb / 1024;
		info.used_mb  = (total_kb - available_kb) / 1024;
	}
	return info;
}

// --- Live host readers ------------------------------------------------------

namespace {

// Walk /sys/class/hwmon/*/tempN_{label,input}, plus thermal zones as a fallback.
QList<TempReading> ReadTemps() {
	QList<TempReading> readings;
	QDir               hwmon(QStringLiteral("/sys/class/hwmon"));
	for (const auto& chip: hwmon.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
		const QDir dir(hwmon.filePath(chip));
		const auto chip_name = QString::fromLatin1(ReadWhole(dir.filePath("name"))).trimmed();
		for (const auto& input: dir.entryList({QStringLiteral("temp*_input")}, QDir::Files)) {
			const auto index = input.mid(4, input.indexOf(QLatin1Char('_')) - 4);
			bool       ok    = false;
			const auto milli =
			    QString::fromLatin1(ReadWhole(dir.filePath(input))).trimmed().toInt(&ok);
			if (!ok) {
				continue;
			}
			auto label = QString::fromLatin1(
			                 ReadWhole(dir.filePath(QStringLiteral("temp%1_label").arg(index))))
			                 .trimmed();
			// k10temp labels its sensors Tctl/Tdie; coretemp labels "Package id 0". When a chip
			// gives no per-sensor label, tag it with the chip name so PickCpuTemp can still choose.
			if (label.isEmpty()) {
				label = chip_name;
			}
			readings.append({label, milli});
		}
	}
	if (readings.isEmpty()) {
		QDir thermal(QStringLiteral("/sys/class/thermal"));
		for (const auto& zone: thermal.entryList({QStringLiteral("thermal_zone*")}, QDir::Dirs)) {
			const QDir dir(thermal.filePath(zone));
			bool       ok = false;
			const auto milli =
			    QString::fromLatin1(ReadWhole(dir.filePath("temp"))).trimmed().toInt(&ok);
			if (ok) {
				readings.append(
				    {QString::fromLatin1(ReadWhole(dir.filePath("type"))).trimmed(), milli});
			}
		}
	}
	return readings;
}

QJsonObject SampleFps(double* frame_time_ms, const QString& fps_file) {
	const auto body = ReadWhole(fps_file.isEmpty() ? QString::fromStdString(Common::FpsStatsFilePath()) : fps_file);
	if (body.isEmpty()) {
		return {};
	}
	const auto window = Common::FpsStatsParse(body.toStdString(), NowUnixS(), 3);
	if (!window || !window->has_samples) {
		return {};
	}
	if (frame_time_ms != nullptr) {
		*frame_time_ms = window->frame_ms;
	}
	return QJsonObject {{"current", window->current},
	                    {"avg", window->avg},
	                    {"min", window->min},
	                    {"max", window->max},
	                    {"window_s", static_cast<int>(window->window_s)}};
}

} // namespace

// NVML, loaded at runtime. A thin binding to just the handful of entry points the contract needs;
// absent library or any failed call yields an all-null GpuInfo.
GpuInfo SampleGpu() {
	struct nvmlMemory_t {
		unsigned long long total;
		unsigned long long free;
		unsigned long long used;
	};
	void* lib = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
	if (lib == nullptr) {
		return {};
	}
	const auto sym      = [&](const char* name) { return dlsym(lib, name); };
	auto       init     = reinterpret_cast<int (*)()>(sym("nvmlInit_v2"));
	auto       shutdown = reinterpret_cast<int (*)()>(sym("nvmlShutdown"));
	auto       get_handle =
	    reinterpret_cast<int (*)(unsigned int, void**)>(sym("nvmlDeviceGetHandleByIndex_v2"));
	auto get_name = reinterpret_cast<int (*)(void*, char*, unsigned int)>(sym("nvmlDeviceGetName"));
	auto get_util = reinterpret_cast<int (*)(void*, void*)>(sym("nvmlDeviceGetUtilizationRates"));
	auto get_mem  = reinterpret_cast<int (*)(void*, nvmlMemory_t*)>(sym("nvmlDeviceGetMemoryInfo"));
	auto get_temp =
	    reinterpret_cast<int (*)(void*, int, unsigned int*)>(sym("nvmlDeviceGetTemperature"));

	GpuInfo info;
	if (init != nullptr && get_handle != nullptr && init() == 0) {
		void* device = nullptr;
		if (get_handle(0, &device) == 0 && device != nullptr) {
			info.ok = true;
			if (get_name != nullptr) {
				char name[96] = {};
				if (get_name(device, name, sizeof(name)) == 0) {
					info.name = QString::fromLatin1(name);
				}
			}
			if (get_util != nullptr) {
				struct {
					unsigned int gpu;
					unsigned int memory;
				} rates {};
				if (get_util(device, &rates) == 0) {
					info.usage_pct = static_cast<double>(rates.gpu);
				}
			}
			if (get_mem != nullptr) {
				nvmlMemory_t memory {};
				if (get_mem(device, &memory) == 0) {
					info.vram_used_mb  = memory.used / (1024 * 1024);
					info.vram_total_mb = memory.total / (1024 * 1024);
				}
			}
			if (get_temp != nullptr) {
				unsigned int celsius = 0;
				if (get_temp(device, 0 /* NVML_TEMPERATURE_GPU */, &celsius) == 0) {
					info.temp_c = static_cast<double>(celsius);
				}
			}
		}
		if (shutdown != nullptr) {
			shutdown();
		}
	}
	dlclose(lib);
	return info;
}

QJsonObject Sampler::Build(const QJsonObject& game, const QString& fps_file) {
	QJsonObject metrics {{"ts", static_cast<double>(NowUnixS())}};

	// CPU. Usage needs two /proc/stat readings a moment apart; we keep the previous one and use
	// the delta against it, so the first poll after start reports no usage.
	const auto  now_cpu = ParseProcStat(ReadWhole(QStringLiteral("/proc/stat")));
	QJsonObject cpu;
	const auto  name = ParseCpuName(ReadWhole(QStringLiteral("/proc/cpuinfo")));
	if (!name.isEmpty()) {
		cpu["name"] = name;
	}
	if (const auto usage = CpuUsagePercent(m_prev_cpu, now_cpu)) {
		cpu["usage_pct"] = *usage;
	}
	m_prev_cpu = now_cpu;
	if (const auto temp = PickCpuTemp(ReadTemps())) {
		cpu["temp_c"] = *temp;
	}
	if (!cpu.isEmpty()) {
		metrics["cpu"] = cpu;
	}

	// RAM.
	if (const auto mem = ParseMemInfo(ReadWhole(QStringLiteral("/proc/meminfo"))); mem.ok) {
		metrics["ram"] = QJsonObject {{"used_mb", static_cast<double>(mem.used_mb)},
		                              {"total_mb", static_cast<double>(mem.total_mb)}};
	}

	// GPU (NVML, runtime-only).
	if (const auto gpu = SampleGpu(); gpu.ok) {
		QJsonObject object;
		if (!gpu.name.isEmpty()) {
			object["name"] = gpu.name;
		}
		if (gpu.usage_pct) {
			object["usage_pct"] = *gpu.usage_pct;
		}
		if (gpu.temp_c) {
			object["temp_c"] = *gpu.temp_c;
		}
		if (gpu.vram_used_mb) {
			object["vram_used_mb"] = static_cast<double>(*gpu.vram_used_mb);
		}
		if (gpu.vram_total_mb) {
			object["vram_total_mb"] = static_cast<double>(*gpu.vram_total_mb);
		}
		metrics["gpu"] = object;
	}

	// Game: the daemon owns running title id/name; FPS comes from the emulator's channel and only
	// when a game is actually running.
	if (!game.isEmpty()) {
		QJsonObject merged = game;
		if (game.value(QStringLiteral("running")).toBool()) {
			double     frame_time_ms = 0.0;
			const auto fps           = SampleFps(&frame_time_ms, fps_file);
			if (!fps.isEmpty()) {
				merged["fps"] = fps;
				if (frame_time_ms > 0.0) {
					merged["frame_time_ms"] = frame_time_ms;
				}
			}
		}
		metrics["game"] = merged;
	}

	return metrics;
}

} // namespace HostMetrics
