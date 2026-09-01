#include "AppTheme.h"

#include <QApplication>
#include <QGuiApplication>
#include <QPalette>
#include <QSettings>
#include <QStyle>
#include <QStyleHints>

namespace noxshell::ui {

QString themeModeSettingValue(ThemeMode mode)
{
    switch (mode) {
    case ThemeMode::Light: return QStringLiteral("light");
    case ThemeMode::Dark: return QStringLiteral("dark");
    case ThemeMode::System: return QStringLiteral("system");
    }
    return QStringLiteral("system");
}

ThemeMode themeModeFromSetting(const QString &value)
{
    if (value.compare(QStringLiteral("light"), Qt::CaseInsensitive) == 0) return ThemeMode::Light;
    if (value.compare(QStringLiteral("dark"), Qt::CaseInsensitive) == 0) return ThemeMode::Dark;
    return ThemeMode::System;
}

ThemeMode storedThemeMode()
{
    const auto environmentOverride = qEnvironmentVariable("NOXSHELL_THEME").trimmed();
    if (!environmentOverride.isEmpty()) return themeModeFromSetting(environmentOverride);
    return themeModeFromSetting(QSettings().value(
        QStringLiteral("ui/themeMode"), QStringLiteral("system")).toString());
}

bool isDarkTheme(ThemeMode mode)
{
    if (mode == ThemeMode::Dark) return true;
    if (mode == ThemeMode::Light) return false;
    if (const auto *hints = QGuiApplication::styleHints()) {
        if (hints->colorScheme() == Qt::ColorScheme::Dark) return true;
        if (hints->colorScheme() == Qt::ColorScheme::Light) return false;
    }
    if (auto *application = qobject_cast<QApplication *>(QCoreApplication::instance())) {
        return application->style()->standardPalette().color(QPalette::Window).lightness() < 128;
    }
    return false;
}

bool isApplicationDarkTheme()
{
    return qApp && qApp->property("noxshellDarkTheme").toBool();
}

QString applicationStyleSheet(bool dark)
{
    auto style = QStringLiteral(R"QSS(
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
        QToolButton#terminalSettingsButton, QToolButton#themeModeButton {
            background: transparent; border: 1px solid transparent; border-radius: 5px;
        }
        QToolButton#themeModeButton { font-size:16px; font-family:"Helvetica Neue", Arial; }
        QToolButton#sidebarToggleButton:hover, QToolButton#monitorToggleButton:hover,
        QToolButton#terminalSettingsButton:hover, QToolButton#themeModeButton:hover {
            background: #E8EDF3; border-color: #CCD7E3;
        }
        QToolButton#sidebarToggleButton:pressed, QToolButton#monitorToggleButton:pressed,
        QToolButton#terminalSettingsButton:pressed, QToolButton#themeModeButton:pressed { background: #DCE5EE; }
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
        QLabel#hostItemProtocol {
            color:#7A4BC2; background:#F1EAFE; border:1px solid #DDCEFA;
            border-radius:3px; padding:1px 4px; font-size:9px; font-weight:650;
        }
        QLabel#serverStorageNotice {
            color:#6B7C91; background:#F4F7FA; border:1px solid #DFE6EF;
            padding:9px; border-radius:4px;
        }
        QLabel#portInlineLabel { color:#53657B; }
        QLabel#permissionFileName { color:#18324F; font-weight:650; }
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
        QMenu {
            background: white; border: 1px solid #D8E0EA; border-radius: 5px; padding: 5px;
        }
        QMenu::item { min-height: 26px; padding: 0 28px 0 9px; border-radius: 3px; }
        QMenu::item:selected { color: #0052D9; background: #E8F3FF; }
        QMenu::indicator { left: 8px; }
        QFrame#filePanel {
            background: white; border: 1px solid #E1E7EF; border-radius: 5px;
        }
        QWidget#fileToolbar { border-bottom:1px solid #E5EAF0; background:#FBFCFD; }
        QLabel#fileWorkspacePlaceholder { color:#8B9AAF; background:#FBFCFD; }
        QLabel#filePanelTitle { font-weight:650; }
        QLabel#fileServerLabel { color:#738297; font-size:12px; }
        QLabel#fileStatusLabel { color:#738297; font-size:11px; }
        QWidget#fileLoadingOverlay { background:rgba(248, 250, 253, 218); }
        QFrame#fileLoadingCard { background:#FFFFFF; border:1px solid #D8E4EF; border-radius:8px; }
        QLabel#fileLoadingTitle { color:#28445F; font-size:14px; font-weight:650; }
        QLabel#fileLoadingDetail { color:#7B8FA3; font-size:11px; }
        QProgressBar#fileLoadingProgress { border:0; border-radius:2px; background:#E5EDF5; }
        QProgressBar#fileLoadingProgress::chunk { background:#1684FF; border-radius:2px; }
        QToolButton#fileBackButton, QToolButton#fileUpButton, QToolButton#fileRefreshButton,
        QToolButton#transferQueueButton {
            border:1px solid transparent; border-radius:4px; padding:0; background:transparent;
        }
        QToolButton#fileBackButton:hover, QToolButton#fileUpButton:hover,
        QToolButton#fileRefreshButton:hover, QToolButton#transferQueueButton:hover {
            background:#F0F5FA; border-color:#D5DFEA;
        }
        QToolButton#fileBackButton:pressed, QToolButton#fileUpButton:pressed,
        QToolButton#fileRefreshButton:pressed, QToolButton#transferQueueButton:pressed {
            background:#E7EFF7; border-color:#BFCEDF;
        }
        QToolButton#transferQueueButton[active="true"] { background:#E8F3FF; border-color:#8BBFFF; }
        QToolButton#transferQueueButton::menu-indicator { image:none; }
        QMenu#transferQueueMenu { padding:0; }
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
        QFrame#metricCorePanel { background:#F8FAFC; border:0; border-top:1px solid #EEF2F6; }
        QLabel#metricCoreName, QLabel#metricCoreValue, QLabel#metricCoreMore { color:#708297; font-size:9px; }
        QLabel#hostItemName { color:#20344B; font-weight:650; }
        QLabel#hostItemAddress { color:#60748A; font-size:12px; }
        QPushButton#credentialSettingsButton {
            text-align:left; color:#66768A; border:0; border-top:1px solid #DFE6EF;
            border-radius:0; background:transparent;
        }
        QTreeWidget#hostList { border:0; background:white; outline:0; }
        QTreeWidget#hostList::item { border-radius:3px; }
        QTreeWidget#hostList::item:selected { background:#E8F3FF; color:#0052D9; }
        QTreeWidget#hostList::branch { background:transparent; }
        QFrame#systemDetailPanel { border:0; background:transparent; }
        QFrame#networkSectionCard, QFrame#processSectionCard, QFrame#fileSystemSectionCard {
            border:1px solid #D7E1EB; border-radius:5px; background:#FFFFFF;
        }
        QLabel#detailSectionTitle { font-weight:650; color:#173553; }
        QLabel#detailMuted { color:#8B9AAF; font-size:10px; }
        QWidget#networkRateRow { background:#F7FAFC; border-radius:3px; }
        QLabel#networkUploadRate { color:#D94841; font-weight:650; }
        QLabel#networkDownloadRate { color:#008858; font-weight:650; }
        QComboBox#networkInterfaceCombo { min-height:27px; padding:0 2px 0 8px; }
        QComboBox#networkInterfaceCombo::drop-down { width:26px; border:0; border-left:1px solid #E1E7EF; }
        QComboBox#networkInterfaceCombo::down-arrow { image:url(:/assets/chevron-down.svg); width:12px; height:12px; }
        QComboBox#networkInterfaceCombo QAbstractItemView { outline:0; padding:3px; }
        QComboBox#networkInterfaceCombo QAbstractItemView::item { min-height:26px; padding:0 7px; border-radius:3px; }
        QTabBar#processMetricTabs::tab {
            min-width:44px; min-height:25px; max-height:25px; padding:0 5px;
            color:#60758B; background:#F5F8FB; font-size:10px;
        }
        QTabBar#processMetricTabs::tab:selected {
            color:#006EFF; background:#FFFFFF; border-bottom:2px solid #006EFF;
        }
        QTreeWidget#realtimeProcessList, QTreeWidget#fileSystemUsageList {
            border:1px solid #E2E8F0; background:#FFFFFF; alternate-background-color:#F8FAFC;
            font-size:10px;
        }
        QTreeWidget#realtimeProcessList::item, QTreeWidget#fileSystemUsageList::item {
            min-height:21px; max-height:21px; padding:0 3px;
        }
        QTreeWidget#realtimeProcessList QHeaderView::section,
        QTreeWidget#fileSystemUsageList QHeaderView::section {
            height:22px; background:#F5F8FB; border:0; border-bottom:1px solid #E2E8F0;
            padding:0 4px; color:#738297; font-size:9px;
        }
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
        QFrame#terminalCommandBlockTools {
            background: #102A40; border: 1px solid #3A6688; border-radius: 5px;
        }
        QToolButton#terminalCommandBlockCopyText,
        QToolButton#terminalCommandBlockCopyImage {
            background: transparent; border: 0; border-radius: 3px; padding: 0;
        }
        QToolButton#terminalCommandBlockCopyText:hover,
        QToolButton#terminalCommandBlockCopyImage:hover { background: #294B68; }
        QFrame#terminalSearchBar {
            background: #102A40; border: 1px solid #31506B; border-radius: 5px;
        }
        QLineEdit#terminalSearchInput {
            color: #E8F1F8; background: #0C1825; border: 1px solid #3B5B76;
            border-radius: 4px; min-height: 25px; padding: 0 7px;
            selection-color: #17233D; selection-background-color: #FFF36A;
        }
        QLineEdit#terminalSearchInput:focus { border-color: #69A8D8; }
        QLabel#terminalSearchCounter { color: #A9BDCF; font-size: 11px; }
        QToolButton#terminalSearchPrevious, QToolButton#terminalSearchNext,
        QToolButton#terminalSearchClose {
            color: #B8C9D8; background: transparent; border: 0; border-radius: 3px;
            padding: 0; font-size: 15px;
        }
        QToolButton#terminalSearchPrevious:hover, QToolButton#terminalSearchNext:hover,
        QToolButton#terminalSearchClose:hover { color: white; background: #294159; }
        QToolButton#terminalSearchPrevious:disabled, QToolButton#terminalSearchNext:disabled { color: #536B80; }
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
        QLabel#commandHistoryScope {
            color: #8FB3D1; background: #182C3E; border: 1px solid #29445B;
            border-radius: 3px; padding: 2px 5px; font-size: 10px;
        }
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
        QTabBar#remoteFileEditorTabs { background:#F4F6F8; border-bottom:1px solid #DCE3EA; }
        QTabBar#remoteFileEditorTabs::tab {
            min-width:150px; max-width:260px; min-height:28px; max-height:28px;
            padding:0 6px; color:#44566C; background:#F4F6F8;
        }
        QTabBar#remoteFileEditorTabs::tab:selected {
            background:#FFFFFF; color:#172B43; font-weight:650; border-bottom:2px solid #1684FF;
        }
        QTabBar#remoteFileEditorTabs::tab:hover:!selected { background:#EAF0F6; }
        QWidget#remoteFileTabCloseContainer { background:transparent; }
        QToolButton#remoteFileTabCloseButton {
            color:#718398; background:transparent; border:0; border-radius:3px; font-size:15px; padding:0;
        }
        QToolButton#remoteFileTabCloseButton:hover { color:#172B43; background:#DCE7F2; }
        QFrame#remoteFileFindPanel { background:#F7F9FC; border-bottom:1px solid #DCE3EA; }
        QFrame#remoteFileFindPanel QLineEdit {
            min-height:28px; border:1px solid #C9D4E2; border-radius:3px; background:#FFFFFF; padding:0 7px;
        }
        QFrame#remoteFileFindPanel QLineEdit:focus { border-color:#1684FF; }
        QFrame#remoteFileFindPanel QPushButton, QFrame#remoteFileFindPanel QToolButton {
            min-height:27px; border:1px solid #C9D4E2; border-radius:3px; background:#FFFFFF; padding:0 8px;
        }
        QFrame#remoteFileFindPanel QPushButton:hover, QFrame#remoteFileFindPanel QToolButton:hover {
            border-color:#8EA4BC; background:#F1F6FC;
        }
        QFrame#remoteFileFindPanel QToolButton:checked { color:#006EFF; border-color:#1684FF; background:#EAF3FF; }
        QLabel#remoteFileEditorStatus {
            border-top:1px solid #DCE3EA; background:#F8FAFC; color:#738297;
            padding:0 10px; font-size:11px;
        }
    )QSS");

    if (!dark) return style;
    style += QStringLiteral(R"QSS(
        * { color: #D7E2EE; }
        QMainWindow, QDialog, QMessageBox, QInputDialog, QWidget#appRoot,
        QWidget#operationsWorkspace, QWidget#terminalWorkspacePane, QWidget#fileWorkspacePane {
            background: #111820;
        }
        QToolBar#windowControlsToolbar { background: #171F29; border-bottom: 1px solid #2A3542; }
        QToolButton#sidebarToggleButton:hover, QToolButton#monitorToggleButton:hover,
        QToolButton#terminalSettingsButton:hover, QToolButton#themeModeButton:hover {
            background: #263241; border-color: #3A495A;
        }
        QToolButton#sidebarToggleButton:pressed, QToolButton#monitorToggleButton:pressed,
        QToolButton#terminalSettingsButton:pressed, QToolButton#themeModeButton:pressed {
            background: #303E4E;
        }
        QLabel#windowToolbarTitle { color: #AEBBC9; }
        QToolButton#windowMinimizeButton, QToolButton#windowMaximizeButton,
        QToolButton#windowCloseButton { color: #CBD6E2; }
        QToolButton#windowMinimizeButton:hover, QToolButton#windowMaximizeButton:hover { background: #2A3542; }
        QLabel#serverAddress, QLabel#monitorSystemTitle, QLabel#monitorUptimeValue,
        QLabel#metricValue, QLabel#detailSectionTitle, QLabel#permissionFileName { color: #E3ECF5; }
        QLabel#mutedLabel, QLabel#metricTitle, QLabel#metricDetail, QLabel#monitorSystemCaption,
        QLabel#hostItemAddress, QLabel#detailMuted { color: #8FA0B2; }
        QLabel#hostItemName { color: #DCE7F2; }
        QLabel#hostItemProtocol {
            color:#C6A6FF; background:#30244A; border-color:#59427D;
        }
        QLabel#serverStorageNotice {
            color:#A8B7C6; background:#1B2631; border-color:#354252;
        }
        QLabel#portInlineLabel { color:#9AABBD; }
        QLabel#onlineBadge { color: #55D6A9; background: #15372F; }
        QLineEdit, QTextEdit, QPlainTextEdit, QSpinBox, QDoubleSpinBox, QComboBox {
            color: #DCE6F0; background: #18212B; border-color: #354252;
            selection-color: white; selection-background-color: #1769AA;
        }
        QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus, QSpinBox:focus,
        QDoubleSpinBox:focus, QComboBox:focus { border-color: #3F9BFF; }
        QPushButton {
            color: #C9D5E1; background: #1B2530; border-color: #354252;
        }
        QPushButton:hover { color: #76B8FF; border-color: #528AC1; background: #222F3D; }
        QPushButton:pressed { background: #293949; }
        QPushButton#primaryButton { color: white; background: #1677D2; border-color: #2588E5; }
        QPushButton#primaryButton:hover { background: #2388E8; }
        QMenu { color:#DCE6F0; background:#1A232D; border-color:#3A4654; }
        QMenu::item:selected { color:white; background:#174E78; }
        QToolTip { color:#E8F0F7; background:#202B36; border:1px solid #465566; }
        QFrame#hostSidebar, QFrame#statusBar, QFrame#monitorIdentity,
        QWidget#monitorRail, QScrollArea#monitorScrollArea,
        QScrollArea#monitorScrollArea QWidget { background: #171F28; }
        QFrame#hostSidebar, QWidget#monitorRail { border-color: #2B3744; }
        QFrame#monitorIdentity, QFrame#statusBar { border-color: #2B3744; }
        QListWidget, QTreeWidget, QTreeView, QTableView {
            color:#D5E0EB; background:#171F28; alternate-background-color:#1B2530;
            border-color:#2F3B49;
        }
        QListWidget::item:selected, QTreeWidget::item:selected, QTreeView::item:selected,
        QTableView::item:selected { color:#FFFFFF; background:#174E78; border-color:#28699A; }
        QListWidget::item:hover:!selected, QTreeWidget::item:hover:!selected { background:#202B36; }
        QTreeWidget#remoteDirectoryTree {
            color:#D5E0EB; background:#151D25; border:0; border-right:1px solid #303D4A;
        }
        QTreeWidget#remoteDirectoryTree::item {
            color:#D5E0EB; background:transparent; border:0;
        }
        QTreeWidget#remoteDirectoryTree::item:hover:!selected {
            color:#E7EFF7; background:#202B36;
        }
        QTreeWidget#remoteDirectoryTree::item:selected {
            color:#FFFFFF; background:#174E78;
        }
        QTreeWidget#remoteDirectoryTree::item:disabled { color:#697A8C; }
        QTreeWidget#remoteDirectoryTree::branch { background:transparent; }
        QWidget#fileListContainer { background:#151D25; }
        QTabBar { background:#171F28; }
        QTabBar::tab { color:#98AABD; background:#171F28; }
        QTabBar::tab:selected { color:#70B7FF; background:#1E2A36; border-bottom-color:#3F9BFF; }
        QTabBar::tab:hover:!selected { background:#202B36; }
        QComboBox QAbstractItemView { color:#DCE6F0; background:#1A232D; border-color:#3A4654; }
        QHeaderView::section { color:#8FA0B2; background:#1C2631; border-bottom-color:#33404E; }
        QFrame#filePanel, QFrame#monitorMetricSummary,
        QFrame#networkSectionCard, QFrame#processSectionCard, QFrame#fileSystemSectionCard {
            background:#171F28; border-color:#303D4A;
        }
        QWidget#fileToolbar { background:#18212B; border-bottom-color:#303D4A; }
        QLabel#fileWorkspacePlaceholder { color:#7F91A4; background:#151D25; }
        QLabel#fileServerLabel, QLabel#fileStatusLabel { color:#8FA0B2; }
        QWidget#fileLoadingOverlay { background:rgba(21, 29, 37, 218); }
        QFrame#fileLoadingCard { background:#1E2A36; border-color:#3A4A5A; }
        QLabel#fileLoadingTitle { color:#DCE7F2; }
        QLabel#fileLoadingDetail { color:#91A3B6; }
        QProgressBar#fileLoadingProgress { background:#303D4A; }
        QProgressBar#fileLoadingProgress::chunk { background:#3F9BFF; }
        QToolButton#fileBackButton:hover, QToolButton#fileUpButton:hover,
        QToolButton#fileRefreshButton:hover, QToolButton#transferQueueButton:hover {
            background:#24313E; border-color:#3A4A5A;
        }
        QToolButton#fileBackButton:pressed, QToolButton#fileUpButton:pressed,
        QToolButton#fileRefreshButton:pressed, QToolButton#transferQueueButton:pressed {
            background:#2B3A49; border-color:#4A6075;
        }
        QToolButton#transferQueueButton[active="true"] { background:#173D5D; border-color:#3F83B8; }
        QFrame#monitorSystemSummary, QFrame#metricRow { border-bottom-color:#2A3542; }
        QFrame#metricCorePanel { background:#1A232D; border-top-color:#2A3542; }
        QLabel#metricCoreName, QLabel#metricCoreValue, QLabel#metricCoreMore { color:#91A3B6; }
        QTreeWidget#hostList { background:#171F28; }
        QTreeWidget#hostList::item:selected { color:#FFFFFF; background:#174E78; }
        QPushButton#credentialSettingsButton { color:#8FA0B2; border-top-color:#303D4A; }
        QWidget#networkRateRow { background:#1C2631; }
        QComboBox#networkInterfaceCombo::drop-down { border-left-color:#354252; }
        QTabBar#processMetricTabs::tab { color:#91A3B6; background:#1C2631; }
        QTabBar#processMetricTabs::tab:selected { color:#70B7FF; background:#171F28; border-bottom-color:#3F9BFF; }
        QTreeWidget#realtimeProcessList, QTreeWidget#fileSystemUsageList {
            background:#151D25; alternate-background-color:#1A232D; border-color:#303D4A;
        }
        QTreeWidget#realtimeProcessList QHeaderView::section,
        QTreeWidget#fileSystemUsageList QHeaderView::section {
            color:#8FA0B2; background:#1C2631; border-bottom-color:#303D4A;
        }
        QFrame#transferQueuePanel, QFrame#transferQueuePanel[popup="true"] {
            background:#18212B; border-color:#303D4A;
        }
        QLabel#transferQueueTitle, QLabel#transferName { color:#DCE7F2; }
        QLabel#transferQueueSummary { color:#A7B6C5; background:#263240; }
        QListWidget#transferQueueList::item { background:#1D2732; border-color:#354252; }
        QListWidget#transferQueueList::item:selected { color:white; background:#233241; border-color:#467198; }
        QProgressBar#transferProgress { background:#303B48; }
        QSplitter::handle { background:#2A3542; }
        QSplitter::handle:hover { background:#477AA6; }
        QScrollBar::handle:vertical { background:#536273; }
        QGroupBox { border:1px solid #354252; border-radius:5px; margin-top:8px; padding-top:8px; }
        QGroupBox::title { subcontrol-origin:margin; left:9px; padding:0 4px; color:#B9C7D4; }
        QFrame#remoteFileFindPanel { background:#18212B; border-bottom:1px solid #354252; }
        QFrame#remoteFileFindPanel QLineEdit,
        QFrame#remoteFileFindPanel QPushButton, QFrame#remoteFileFindPanel QToolButton {
            color:#DCE6F0; background:#1B2530; border-color:#3A495A;
        }
        QFrame#remoteFileFindPanel QPushButton:hover, QFrame#remoteFileFindPanel QToolButton:hover {
            color:#FFFFFF; background:#263442; border-color:#52708C;
        }
        QFrame#remoteFileFindPanel QToolButton:checked { color:#70B7FF; background:#173D5D; border-color:#3F83B8; }
        QTabBar#remoteFileEditorTabs { background:#18212B; border-bottom:1px solid #354252; }
        QTabBar#remoteFileEditorTabs::tab { color:#9AABBD; background:#18212B; }
        QTabBar#remoteFileEditorTabs::tab:selected { color:#E4EDF6; background:#111820; }
        QToolButton#remoteFileTabCloseButton { color:#91A3B6; }
        QToolButton#remoteFileTabCloseButton:hover { color:#FFFFFF; background:#2A3948; }
        QLabel#remoteFileEditorStatus { color:#8FA0B2; background:#18212B; border-top:1px solid #354252; }
    )QSS");
    return style;
}

void applyApplicationTheme(ThemeMode mode)
{
    auto *application = qobject_cast<QApplication *>(QCoreApplication::instance());
    if (!application) return;
    const bool dark = isDarkTheme(mode);
    QPalette palette = application->style()->standardPalette();
    if (dark) {
        palette.setColor(QPalette::Window, QColor(QStringLiteral("#111820")));
        palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#D7E2EE")));
        palette.setColor(QPalette::Base, QColor(QStringLiteral("#171F28")));
        palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#1B2530")));
        palette.setColor(QPalette::Text, QColor(QStringLiteral("#D7E2EE")));
        palette.setColor(QPalette::Button, QColor(QStringLiteral("#1B2530")));
        palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#D7E2EE")));
        palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#1769AA")));
        palette.setColor(QPalette::HighlightedText, Qt::white);
        palette.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#202B36")));
        palette.setColor(QPalette::ToolTipText, QColor(QStringLiteral("#E8F0F7")));
        palette.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#708194")));
        palette.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#647384")));
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#647384")));
    } else {
        palette.setColor(QPalette::Window, QColor(QStringLiteral("#F3F6FA")));
        palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#17233D")));
        palette.setColor(QPalette::Base, Qt::white);
        palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#FAFBFC")));
        palette.setColor(QPalette::Text, QColor(QStringLiteral("#17233D")));
        palette.setColor(QPalette::Button, Qt::white);
        palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#3C4D63")));
        palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#006EFF")));
        palette.setColor(QPalette::HighlightedText, Qt::white);
        palette.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#FFFFFF")));
        palette.setColor(QPalette::ToolTipText, QColor(QStringLiteral("#17233D")));
        palette.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#8A99AA")));
        palette.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#9AA7B6")));
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#9AA7B6")));
    }
    application->setProperty("noxshellDarkTheme", dark);
    application->setPalette(palette);
    application->setStyleSheet(applicationStyleSheet(dark));
}

} // namespace noxshell::ui
