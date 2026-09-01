#pragma once

#include <functional>

class QMainWindow;

namespace noxshell::ui {

bool installMacTitleBarControls(QMainWindow *window,
    std::function<void()> toggleSidebar,
    std::function<void()> toggleMonitor,
    std::function<void()> openTerminalSettings,
    std::function<void(int)> selectTheme,
    int themeMode);
void updateMacTitleBarControls(QMainWindow *window, bool sidebarVisible, bool monitorVisible, int themeMode);

} // namespace noxshell::ui
