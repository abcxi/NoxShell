#pragma once

#include <functional>

class QMainWindow;
class QTabBar;

namespace noxshell::ui {

void applyMacApplicationAppearance(int themeMode);
bool installMacTitleBarControls(QMainWindow *window,
    std::function<void()> addConnection,
    std::function<void()> openCredentialManager,
    std::function<void()> openServerManager,
    std::function<void()> toggleMonitor,
    std::function<void()> openTerminalSettings,
    std::function<void(int)> selectTheme,
    int themeMode);
void updateMacTitleBarSessionTabs(QMainWindow *window, QTabBar *tabs);
void updateMacTitleBarControls(QMainWindow *window, bool monitorVisible, int themeMode);

} // namespace noxshell::ui
