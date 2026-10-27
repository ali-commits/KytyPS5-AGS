#ifndef KYTY_EMULATOR_COMMAND_H
#define KYTY_EMULATOR_COMMAND_H
#include <QStringList>
class Configuration;
#if defined(_WIN32)
constexpr char EMULATOR_EXE[] = "kyty_emulator.exe";
#else
constexpr char EMULATOR_EXE[] = "kyty_emulator";
#endif
QStringList BuildEmulatorArgs(const Configuration& info);
#endif
