#pragma once

#include <functional>

class QMainWindow;

namespace noxshell::ui {

bool installMacTitleBarControls(QMainWindow *window,
    std::function<void()> toggleSidebar,
    std::function<void()> toggleMonitor,
    std::function<void()> openTerminalSettings);
void updateMacTitleBarControls(QMainWindow *window, bool sidebarVisible, bool monitorVisible);

} // namespace noxshell::ui
