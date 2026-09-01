#pragma once

#include <QString>

namespace noxshell::ui {

enum class ThemeMode {
    System,
    Light,
    Dark,
};

QString themeModeSettingValue(ThemeMode mode);
ThemeMode themeModeFromSetting(const QString &value);
ThemeMode storedThemeMode();
bool isDarkTheme(ThemeMode mode);
bool isApplicationDarkTheme();
QString applicationStyleSheet(bool dark = false);
void applyApplicationTheme(ThemeMode mode);

} // namespace noxshell::ui
