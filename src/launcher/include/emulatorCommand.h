#ifndef KYTY_EMULATOR_COMMAND_H
#define KYTY_EMULATOR_COMMAND_H
#include <QString>
#include <QStringList>

#include <optional>
class Configuration;
#if defined(_WIN32)
constexpr char EMULATOR_EXE[] = "kyty_emulator.exe";
#else
constexpr char EMULATOR_EXE[] = "kyty_emulator";
#endif
// patch_plan: nullopt keeps today's PC behaviour (auto-detect _Patches/<TITLE>.json). A value
// forces that exact plan (an empty value means launch with no --game-patch at all), used for the
// per-device cheat plan.
QStringList BuildEmulatorArgs(const Configuration&          info,
                              const std::optional<QString>& patch_plan = std::nullopt);
#endif
