#pragma once

#include <QString>
#include <QColor>
#include <QIcon>

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
// Shared neutral surfaces keep adjacent panels coherent in both themes.
struct WorkspaceColors {
    QColor background, canvas, surface, raised, line, focusLine, text, muted;
};
WorkspaceColors workspaceColors(bool dark);
struct TerminalColors {
    QColor foreground;
    QColor background;
    QColor cursor;
    QColor selection;
    QColor preeditForeground;
    QColor preeditBackground;
};
TerminalColors terminalColors(bool dark);
QIcon terminalToolIcon(const QString &resource);
QString applicationStyleSheet(bool dark = false);
void applyApplicationTheme(ThemeMode mode);

} // namespace noxshell::ui
