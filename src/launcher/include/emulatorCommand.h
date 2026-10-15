#ifndef KYTY_EMULATOR_COMMAND_H
#define KYTY_EMULATOR_COMMAND_H
#include <QStringList>
class Configuration;
QStringList BuildEmulatorArgs(const Configuration& info);
#endif
