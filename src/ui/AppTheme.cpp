#include "AppTheme.h"

namespace noxshell::ui {

QString applicationStyleSheet()
{
    return QStringLiteral(R"QSS(
        * {
            font-family: "PingFang SC", "Hiragino Sans GB", "Helvetica Neue", Helvetica, Arial;
            font-size: 13px;
            color: #17233D;
        }
        QMainWindow, QWidget#appRoot { background: #F3F6FA; }
        QToolBar#windowControlsToolbar {
            background: transparent; border: 0; spacing: 3px; padding: 1px 6px;
        }
        QToolButton#sidebarToggleButton, QToolButton#monitorToggleButton,
        QToolButton#terminalSettingsButton {
            background: transparent; border: 1px solid transparent; border-radius: 5px;
        }
        QToolButton#sidebarToggleButton:hover, QToolButton#monitorToggleButton:hover,
        QToolButton#terminalSettingsButton:hover {
            background: #E8EDF3; border-color: #CCD7E3;
        }
        QToolButton#sidebarToggleButton:pressed, QToolButton#monitorToggleButton:pressed,
        QToolButton#terminalSettingsButton:pressed { background: #DCE5EE; }
        QLabel#windowToolbarTitle { color: #46566A; font-size: 12px; font-weight: 650; }
        QToolButton#windowMinimizeButton, QToolButton#windowMaximizeButton,
        QToolButton#windowCloseButton {
            border: 0; border-radius: 0; background: transparent; color: #32445A;
            font-family: "Segoe UI Symbol"; font-size: 15px; padding: 0;
        }
        QToolButton#windowMinimizeButton:hover, QToolButton#windowMaximizeButton:hover { background: #E4E9EF; }
        QToolButton#windowCloseButton:hover { background: #E81123; color: white; }
        QLabel#serverAddress { color: #182B43; font-size: 14px; font-weight: 650; }
        QLabel#mutedLabel { color: #738297; font-size: 12px; }
        QToolButton#copyHostAddressButton { border: 0; border-radius: 4px; background: transparent; }
        QToolButton#copyHostAddressButton:hover { background: #E8F3FF; }
        QLabel#onlineBadge {
            color: #008858; background: #E8F8F2; border-radius: 3px;
            padding: 2px 8px; font-size: 11px;
        }
        QLineEdit {
            min-height: 30px; border: 1px solid #D8E0EA; border-radius: 4px;
            background: white; padding: 0 9px; selection-background-color: #006EFF;
        }
        QLineEdit:focus { border: 1px solid #006EFF; }
        QPushButton {
            min-height: 30px; border: 1px solid #D6DEE8; border-radius: 4px;
            background: white; padding: 0 12px; color: #3C4D63;
        }
        QPushButton:hover { border-color: #8BBFFF; color: #0052D9; }
        QPushButton:pressed { background: #E8F3FF; }
        QPushButton#primaryButton { background: #006EFF; border-color: #006EFF; color: white; }
        QPushButton#primaryButton:hover { background: #005FE5; }
        QPushButton#hostAddButton { min-width: 30px; max-width: 30px; padding: 0; font-size: 18px; }
        QFrame#hostSidebar, QFrame#statusBar, QFrame#monitorIdentity { background: white; }
        QFrame#hostSidebar { border-right: 1px solid #DFE6EF; }
        QFrame#monitorIdentity { border-bottom: 1px solid #E4EAF1; }
        QFrame#statusBar { border-top: 1px solid #DFE6EF; }
        QWidget#monitorRail { background: white; border-right: 1px solid #DDE5EE; }
        QWidget#operationsWorkspace, QWidget#terminalWorkspacePane, QWidget#fileWorkspacePane { background: #F3F6FA; }
        QScrollArea#monitorScrollArea, QScrollArea#monitorScrollArea QWidget { background: white; }
        QListWidget { background: white; border: 0; outline: 0; padding: 5px; }
        QListWidget::item { border-radius: 4px; padding: 8px 7px; margin: 1px 0; }
        QListWidget::item:selected { color: #0052D9; background: #E8F3FF; border: 1px solid #CCE3FF; }
        QListWidget::item:hover:!selected { background: #F4F7FA; }
        QTabBar { background: white; }
        QTabBar::tab { min-width: 95px; min-height: 40px; color: #53657B; padding: 0 10px; }
        QTabBar::tab:selected { color: #0052D9; font-weight: 650; border-bottom: 3px solid #006EFF; }
        QTabBar::tab:hover:!selected { background: #F7F9FC; }
        QComboBox {
            min-height: 28px; border: 1px solid #D8E0EA; border-radius: 4px;
            background: white; color: #3C4D63; padding: 0 9px;
        }
        QComboBox:hover { border-color: #8BBFFF; }
        QComboBox::drop-down { border: 0; width: 22px; }
        QComboBox QAbstractItemView { background: white; border: 1px solid #D8E0EA; selection-background-color: #E8F3FF; }
        QFrame#filePanel {
            background: white; border: 1px solid #E1E7EF; border-radius: 5px;
        }
        QFrame#monitorMetricSummary {
            background: white; border: 1px solid #E1E7EF; border-radius: 5px;
        }
        QFrame#monitorSystemSummary { background: transparent; border: 0; border-bottom: 1px solid #EEF2F6; }
        QLabel#monitorSystemTitle { color: #173553; font-size: 12px; font-weight: 650; }
        QLabel#monitorSystemCaption { color: #8794A5; font-size: 10px; }
        QLabel#monitorUptimeValue { color: #173553; font-size: 12px; font-weight: 650; }
        QFrame#metricRow { background: transparent; border: 0; border-bottom: 1px solid #EEF2F6; }
        QFrame#metricRow[lastRow="true"] { border-bottom: 0; }
        QLabel#metricTitle { color: #5F6F82; font-size: 12px; }
        QLabel#metricValue { color: #1C324B; font-size: 17px; font-weight: 650; }
        QLabel#metricDetail { color: #8794A5; font-size: 10px; }
        QFrame#transferQueuePanel { background: #F7F9FC; border-top: 1px solid #D9E2EC; }
        QFrame#transferQueuePanel[popup="true"] { border: 0; background: #F7F9FC; }
        QLabel#transferQueueTitle { color:#1C324B; font-size:14px; font-weight:650; }
        QLabel#transferQueueSummary { color:#5E7187; background:#EAF0F6; border-radius:4px; padding:3px 7px; font-size:10px; }
        QLabel#transferRateTitle { color:#738297; font-size:11px; }
        QLabel#transferQueueEmpty { color:#8A99AA; font-size:12px; }
        QListWidget#transferQueueList { background: transparent; border: 0; padding: 0; outline: 0; }
        QListWidget#transferQueueList::item { background: white; border: 1px solid #DEE6EF; border-radius:6px; margin: 2px 0; padding: 0; }
        QListWidget#transferQueueList::item:selected { background:white; color:#20364E; border-color:#B8D5F4; }
        QWidget#transferTaskRow { background: transparent; }
        QLabel#transferDirection { color:#006EFF; font-size:17px; font-weight:700; }
        QLabel#transferName { color:#20364E; font-size:12px; font-weight:650; }
        QLabel#transferAmount, QLabel#transferPath { color:#7B8C9E; font-size:10px; }
        QProgressBar#transferProgress { border: 0; border-radius: 3px; background: #E5EBF2; }
        QProgressBar#transferProgress::chunk { background: #1684FF; border-radius: 3px; }
        QPushButton#transferCancel { min-height: 23px; padding: 0 6px; font-size:10px; }
        QLabel#alertTitle { color: #9A4E1D; font-weight: 650; }
        QLabel#alertText { color: #8A6A54; font-size: 11px; }
        QWidget#terminalOutput { background: #0C1825; border: 0; }
        QWidget#terminalRecentPage { background: #0C1825; }
        QLabel#recentLoginTitle { color: #F2F6FA; font-size: 18px; font-weight: 650; }
        QLabel#recentLoginHint, QLabel#recentLoginEmpty { color: #7F95AA; }
        QLabel#recentLoginEmpty { font-size: 14px; }
        QTreeWidget#recentLoginList {
            color: #D8E4EF; background: #101E2D; alternate-background-color: #132235;
            border: 1px solid #22364C; border-radius: 5px; padding: 0;
        }
        QTreeWidget#recentLoginList::item { min-height: 34px; padding: 0 8px; border-radius: 0; }
        QTreeWidget#recentLoginList::item:selected { color: white; background: #17466F; border: 0; }
        QTreeWidget#recentLoginList QHeaderView::section {
            color: #8EA3B7; background: #132235; border: 0; border-bottom: 1px solid #22364C;
            min-height: 30px; padding: 0 8px;
        }
        QLineEdit#terminalInput {
            background: #101E2D; border: 0; border-top: 1px solid #22364C;
            border-radius: 0; color: #F2F6FA; font-family: Menlo, Monaco, Consolas, monospace;
            padding-left: 11px;
        }
        QWidget#terminalInputRow { background: #101E2D; border-top: 1px solid #22364C; }
        QWidget#terminalInputRow QLineEdit#terminalInput { border-top: 0; }
        QToolButton#commandHistoryButton, QToolButton#fileWorkspaceToggleButton {
            background: transparent; border: 1px solid transparent; border-radius: 4px; padding: 0;
        }
        QToolButton#commandHistoryButton:hover, QToolButton#fileWorkspaceToggleButton:hover,
        QToolButton#commandHistoryButton:checked {
            background: #1C344A; border-color: #31506B;
        }
        QFrame#commandHistoryPanel {
            background: #101E2D; border: 0; border-top: 1px solid #294159;
        }
        QLabel#commandHistoryTitle { color: #E7EEF5; font-size: 13px; font-weight: 650; }
        QTabBar#commandHistoryTabs { background: transparent; }
        QTabBar#commandHistoryTabs::tab {
            min-width: 48px; min-height: 25px; padding: 0 6px; color: #8EA3B7;
            background: transparent; border: 0; border-bottom: 2px solid transparent;
        }
        QTabBar#commandHistoryTabs::tab:selected { color: #FFFFFF; border-bottom-color: #1684FF; }
        QToolButton#commandHistoryClearButton, QToolButton#commandHistoryCloseButton {
            color: #9FB1C2; background: transparent; border: 1px solid #31506B;
            border-radius: 4px; min-height: 25px; padding: 0 7px;
        }
        QToolButton#commandHistoryCloseButton { min-width: 25px; max-width: 25px; padding: 0; font-size: 16px; }
        QToolButton#commandHistoryClearButton:hover, QToolButton#commandHistoryCloseButton:hover {
            color: #FFFFFF; background: #1C344A;
        }
        QTreeWidget#commandHistoryList {
            color: #D8E4EF; background: #0C1825; alternate-background-color: #101E2D;
            border: 1px solid #22364C; outline: 0;
        }
        QTreeWidget#commandHistoryList::item { min-height: 28px; border-bottom: 1px solid #1A2C3E; }
        QTreeWidget#commandHistoryList::item:selected { color: #FFFFFF; background: #17466F; }
        QTreeWidget#commandHistoryList QHeaderView::section {
            color: #8EA3B7; background: #132235; border: 0; border-bottom: 1px solid #22364C;
            min-height: 25px; padding: 0 6px;
        }
        QWidget#terminalTabToolbar { background: #101E2D; border-bottom: 1px solid #22364C; }
        QTabBar#terminalSessionTabs { background: #101E2D; }
        QTabBar#terminalSessionTabs::tab {
            min-width: 0; min-height: 30px; padding: 0 4px; color: #8EA3B7;
            background: #132235; border-right: 1px solid #22364C;
        }
        QTabBar#terminalSessionTabs::tab:selected { color: #FFFFFF; background: #0C1825; border-bottom: 2px solid #1684FF; }
        QWidget#terminalTabCloseContainer { background: transparent; }
        QToolButton#terminalTabCloseButton {
            color: #91A4B7; background: transparent; border: 0; border-radius: 3px;
            font-size: 16px; padding: 0;
        }
        QToolButton#terminalTabCloseButton:hover { color: #FFFFFF; background: #294159; }
        QToolButton#terminalNewTabButton {
            color: #B9C8D6; background: #132235; border: 1px solid #294159; border-radius: 4px;
            font-size: 18px; padding: 0;
        }
        QToolButton#terminalNewTabButton:hover { color: #FFFFFF; background: #1C344A; }
        QWidget#terminalLoadingOverlay { background: rgba(7, 18, 29, 190); }
        QFrame#terminalLoadingCard {
            background: #13263A; border: 1px solid #31506B; border-radius: 7px;
        }
        QLabel#terminalLoadingTitle { color: #F1F6FA; font-size: 15px; font-weight: 650; }
        QLabel#terminalLoadingDetail { color: #9FB1C2; font-size: 12px; }
        QProgressBar#terminalLoadingProgress { border: 0; border-radius: 2px; background: #263C50; }
        QProgressBar#terminalLoadingProgress::chunk { background: #1684FF; border-radius: 2px; }
        QTreeWidget {
            background: white; border: 0; alternate-background-color: #FAFBFC;
            outline: 0; show-decoration-selected: 1;
        }
        QTreeWidget::item { min-height: 32px; border-bottom: 1px solid #EEF1F5; }
        QTreeWidget::item:selected { background: #E8F3FF; color: #0052D9; }
        QTreeWidget#remoteDirectoryTree {
            background: #FBFCFD; border: 0; border-right: 1px solid #DFE6EE;
            padding: 4px 3px;
        }
        QTreeWidget#remoteDirectoryTree::item { min-height: 27px; border: 0; padding: 0 4px; }
        QTreeWidget#remoteDirectoryTree::item:selected { background: #E8F3FF; color: #0052D9; }
        QHeaderView::section {
            background: #F7F9FB; color: #7B8999; border: 0; border-bottom: 1px solid #E4EAF1;
            padding: 7px 8px; font-size: 11px;
        }
        QSplitter::handle { background: #E4E9EF; width: 4px; }
        QSplitter::handle:hover { background: #8BBFFF; }
        QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
        QScrollBar::handle:vertical { background: #C6D0DB; min-height: 28px; border-radius: 5px; margin: 2px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
    )QSS");
}

} // namespace noxshell::ui
