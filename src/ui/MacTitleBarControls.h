#pragma once

#include <functional>

class QMainWindow;

namespace noxshell::ui {

void applyMacApplicationAppearance(int themeMode);
bool installMacTitleBarControls(QMainWindow *window,
    std::function<void()> toggleMonitor,
    std::function<void()> openTerminalSettings,
    std::function<void(int)> selectTheme,
    int themeMode);
void updateMacTitleBarControls(QMainWindow *window, bool monitorVisible, int themeMode);

} // namespace noxshell::ui
