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
// per-device cheat plan. upscaler_profile: nullopt looks the game's profile up next to the
// executable (GameServices::UpscalerProfilePath); a value is used as it is (empty: none). It is
// passed only with an upscaler selected.
QStringList BuildEmulatorArgs(const Configuration&          info,
                              const std::optional<QString>& patch_plan       = std::nullopt,
                              const std::optional<QString>& upscaler_profile = std::nullopt);
#endif
