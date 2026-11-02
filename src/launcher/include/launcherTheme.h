#ifndef LAUNCHER_THEME_H
#define LAUNCHER_THEME_H

class QApplication;

namespace LauncherTheme {

void Initialize(QApplication& application);
// Stored values: 0 System, 1 Graphite Dark, 2 Light.
void Apply(QApplication& application, int theme);

} // namespace LauncherTheme

#endif // LAUNCHER_THEME_H
