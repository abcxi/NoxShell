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
        QToolButton#sidebarToggleButton, QToolButton#monitorToggleButton {
            background: transparent; border: 1px solid transparent; border-radius: 5px;
        }
        QToolButton#sidebarToggleButton:hover, QToolButton#monitorToggleButton:hover {
            background: #E8EDF3; border-color: #CCD7E3;
        }
        QToolButton#sidebarToggleButton:pressed, QToolButton#monitorToggleButton:pressed { background: #DCE5EE; }
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
        QWidget#monitorHeading { background: #F7F9FC; border-bottom: 1px solid #E4EAF1; }
        QWidget#operationsWorkspace, QWidget#terminalWorkspacePane, QWidget#fileWorkspacePane { background: #F3F6FA; }
        QScrollArea#monitorScrollArea, QScrollArea#monitorScrollArea QWidget { background: white; }
        QToolButton#monitorTrendToggle {
            min-height: 32px; border: 1px solid #D8E0EA; border-radius: 4px;
            background: #F7F9FC; color: #3C4D63; padding: 0 8px;
        }
        QToolButton#monitorTrendToggle:hover { border-color: #8BBFFF; color: #0052D9; }
        QListWidget { background: white; border: 0; outline: 0; padding: 5px; }
        QListWidget::item { border-radius: 4px; padding: 8px 7px; margin: 1px 0; }
        QListWidget::item:selected { color: #0052D9; background: #E8F3FF; border: 1px solid #CCE3FF; }
        QListWidget::item:hover:!selected { background: #F4F7FA; }
        QTabBar { background: white; }
        QTabBar::tab { min-width: 95px; min-height: 40px; color: #53657B; padding: 0 10px; }
        QTabBar::tab:selected { color: #0052D9; font-weight: 650; border-bottom: 3px solid #006EFF; }
        QTabBar::tab:hover:!selected { background: #F7F9FC; }
        QWidget#monitorToolbar {
            background: white; border: 1px solid #E1E7EF; border-radius: 5px;
        }
        QWidget#monitorAlerts {
            background: white; border: 1px solid #E1E7EF; border-radius: 5px;
        }
        QListWidget#monitorAlertList { border-top: 1px solid #EEF1F5; padding: 0; }
        QListWidget#monitorAlertList::item { min-height: 22px; padding: 1px 6px; color: #6A3F27; }
        QComboBox {
            min-height: 28px; border: 1px solid #D8E0EA; border-radius: 4px;
            background: white; color: #3C4D63; padding: 0 9px;
        }
        QComboBox:hover { border-color: #8BBFFF; }
        QComboBox::drop-down { border: 0; width: 22px; }
        QComboBox QAbstractItemView { background: white; border: 1px solid #D8E0EA; selection-background-color: #E8F3FF; }
        QFrame#metricCard, QFrame#filePanel {
            background: white; border: 1px solid #E1E7EF; border-radius: 5px;
        }
        QLabel#metricTitle { color: #5F6F82; font-size: 12px; }
        QLabel#metricValue { color: #1C324B; font-size: 25px; font-weight: 650; }
        QLabel#metricDetail { color: #8794A5; font-size: 11px; }
        QFrame#alertCard { background: #FFFAF6; border: 1px solid #F3D9C7; border-radius: 5px; }
        QFrame#transferQueuePanel { background: #FBFCFD; border-top: 1px solid #DFE6EE; }
        QFrame#transferQueuePanel[popup="true"] { border: 0; background: #FBFCFD; }
        QListWidget#transferQueueList { background: #FBFCFD; border-top: 1px solid #E8EDF3; padding: 2px; }
        QListWidget#transferQueueList::item { background: white; border: 1px solid #E7ECF2; margin: 1px; padding: 0; }
        QLabel#transferName, QLabel#transferState { font-size: 10px; color: #53657B; }
        QProgressBar#transferProgress { border: 0; border-radius: 2px; background: #E8EDF3; }
        QProgressBar#transferProgress::chunk { background: #006EFF; border-radius: 2px; }
        QPushButton#transferCancel { min-height: 23px; padding: 0 5px; font-size: 10px; }
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
        QWidget#terminalStatus { background: #132235; }
        QWidget#terminalTabToolbar { background: #101E2D; border-bottom: 1px solid #22364C; }
        QTabBar#terminalSessionTabs { background: #101E2D; }
        QTabBar#terminalSessionTabs::tab {
            min-width: 110px; min-height: 30px; padding: 0 8px; color: #8EA3B7;
            background: #132235; border-right: 1px solid #22364C;
        }
        QTabBar#terminalSessionTabs::tab:selected { color: #FFFFFF; background: #0C1825; border-bottom: 2px solid #1684FF; }
        QWidget#terminalStatus QLabel { color: #7F95AA; font-size: 11px; }
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
