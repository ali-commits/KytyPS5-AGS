#ifndef LAUNCHER_INCLUDE_HOST_METRICS_H_
#define LAUNCHER_INCLUDE_HOST_METRICS_H_

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <cstdint>
#include <optional>

// Reads host CPU/RAM/GPU/FPS for GET /kyty/v1/host/metrics. Pure parsers take fixture text so the
// contract shape and each reader can be tested without the real host; the live sampler reads
// /proc, hwmon and (if present at runtime) NVML, caching at most one reading per second.
namespace HostMetrics {

// Totals from one /proc/stat "cpu ..." line; usage is the busy fraction between two readings.
struct CpuTimes {
	bool     ok    = false;
	uint64_t idle  = 0; // idle + iowait
	uint64_t total = 0;
};

[[nodiscard]] CpuTimes              ParseProcStat(const QByteArray& proc_stat);
[[nodiscard]] std::optional<double> CpuUsagePercent(const CpuTimes& before, const CpuTimes& after);
[[nodiscard]] QString               ParseCpuName(const QByteArray& proc_cpuinfo);

// One hwmon/thermal reading: the label text (e.g. "Tctl", "Package id 0") and millidegrees.
struct TempReading {
	QString name;
	int     millidegrees = 0;
};
// Pick the host CPU temperature from readings gathered across hwmon chips: prefer k10temp
// Tctl/Tdie, then coretemp Package, else the first reading. nullopt when there are none.
[[nodiscard]] std::optional<double> PickCpuTemp(const QList<TempReading>& readings);

// used = MemTotal - MemAvailable, both in kB in /proc/meminfo.
struct MemInfo {
	bool     ok       = false;
	uint64_t used_mb  = 0;
	uint64_t total_mb = 0;
};
[[nodiscard]] MemInfo ParseMemInfo(const QByteArray& proc_meminfo);

// A GPU reading via NVML, loaded with dlopen at runtime (no build dependency). All-null when NVML
// is absent or no device is found.
struct GpuInfo {
	bool                    ok = false;
	QString                 name;
	std::optional<double>   usage_pct;
	std::optional<double>   temp_c;
	std::optional<uint64_t> vram_used_mb;
	std::optional<uint64_t> vram_total_mb;
};
[[nodiscard]] GpuInfo SampleGpu();

// The live host sampler. Build() returns the metrics object; readings are cached for one second so
// repeated polls stay cheap. game is merged in verbatim (the daemon owns running title id/name).
class Sampler {
public:
	[[nodiscard]] QJsonObject Build(const QJsonObject& game);

private:
	CpuTimes m_prev_cpu;
};

} // namespace HostMetrics

#endif /* LAUNCHER_INCLUDE_HOST_METRICS_H_ */
