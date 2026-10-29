#include "AppTheme.h"
#ifdef Q_OS_MACOS
#include "MacTitleBarControls.h"
#endif

#include <QApplication>
#include <QBuffer>
#include <QFile>
#include <QGuiApplication>
#include <QImageReader>
#include <QMap>
#include <QPalette>
#include <QPixmap>
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

WorkspaceColors workspaceColors(bool dark)
{
    if (dark) return {QColor("#14171C"), QColor("#1C2026"), QColor("#20242B"), QColor("#272C34"),
        QColor("#303640"), QColor("#52759E"), QColor("#D7DBE3"), QColor("#929BAA")};
    return {QColor("#F1F2F5"), QColor("#FAFAFB"), QColor("#FFFFFF"), QColor("#F2F3F6"),
        QColor("#E4E6EB"), QColor("#91B5E6"), QColor("#303640"), QColor("#79808E")};
}

TerminalColors terminalColors(bool dark)
{
    const auto theme = workspaceColors(dark);
    if (dark) return {theme.text, theme.canvas, QColor("#8DB8EC"),
        QColor(67, 91, 125, 180), QColor("#E3F3FF"), QColor("#2A3545")};
    return {theme.text, theme.canvas, QColor("#246FC0"),
        QColor(147, 192, 240, 150), QColor("#17233D"), QColor("#E8F3FF")};
}

QIcon terminalToolIcon(const QString &resource)
{
    QFile file(resource);
    if (!file.open(QIODevice::ReadOnly)) return QIcon(resource);
    auto svg = file.readAll();
    if (!isApplicationDarkTheme()) {
        svg.replace("#9FB1C2", "#536D88");
        svg.replace("#53C99C", "#19855C");
    }
    QBuffer buffer(&svg);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "SVG");
    reader.setScaledSize(QSize(48, 48));
    const auto image = reader.read();
    if (image.isNull()) return QIcon(resource);
    auto pixmap = QPixmap::fromImage(image);
    pixmap.setDevicePixelRatio(2);
    return QIcon(pixmap);
}

namespace {
// Workspace surfaces are deliberately less saturated than status/accent
// colours. Keep these adjoining regions together instead of tuning each
// panel independently in the legacy control/dialog stylesheet below.
QString workspaceStyleSheet(bool dark)
{
    auto style = QStringLiteral(R"QSS(
        QMainWindow, QWidget#appRoot, QWidget#operationsWorkspace,
        QWidget#terminalWorkspacePane, QWidget#terminalSessionsPage,
        QWidget#fileWorkspacePane { background:@background@; }
        QWidget#monitorRail, QScrollArea#monitorScrollArea, QWidget#monitorScrollContent {
            background:@background@; border:0;
        }
        QFrame#monitorIdentity { background:@background@; border:0; border-bottom:1px solid @line@; }
        QFrame#statusBar { background:@surface@; border:0; border-top:1px solid @line@; }
        QLabel#mutedLabel, QLabel#fileStatusLabel, QLabel#fileServerLabel { color:@muted@; }
        QLabel#serverAddress { color:@text@; }
        QFrame#monitorIdentity QLabel#serverAddress { font-size:12px; }
        QLabel#serverLatency { color:@muted@; font-size:10px; background:transparent; }
        QFrame#monitorMetricSummary, QFrame#networkSectionCard,
        QFrame#processSectionCard, QFrame#fileSystemSectionCard {
            background:@surface@; border:1px solid @line@; border-radius:7px;
        }
        QFrame#monitorSystemSummary { border:0; border-bottom:1px solid @line@; }
        QLabel#monitorSystemTitle, QLabel#monitorUptimeValue,
        QLabel#detailSectionTitle, QLabel#metricValue { color:@text@; }
        QLabel#metricTitle, QLabel#metricDetail, QLabel#monitorSystemCaption { color:@muted@; }
        QTabBar#processMetricTabs { background:transparent; }
        QTabBar#processMetricTabs::tab { background:@canvas@; color:@muted@; }
        QTabBar#processMetricTabs::tab:selected { background:@selection@; color:@accent@; }
        QTreeWidget#realtimeProcessList, QTreeWidget#fileSystemUsageList {
            background:@surface@; alternate-background-color:@canvas@; color:@text@;
        }
        QTreeWidget#realtimeProcessList QHeaderView::section,
        QTreeWidget#fileSystemUsageList QHeaderView::section {
            background:@surface@; color:@muted@; border:0;
        }
        QSplitter#mainWorkspaceSplitter::handle,
        QSplitter#terminalPaneSplitter::handle,
        QSplitter#terminalFileSplitter::handle,
        QSplitter#paneEditorSplitter::handle { background:@background@; border:0; }
        QSplitter#fileBrowserSplitter::handle { background:@line@; border:0; }
        QSplitter#mainWorkspaceSplitter::handle:hover,
        QSplitter#terminalPaneSplitter::handle:hover,
        QSplitter#terminalFileSplitter::handle:hover,
        QSplitter#paneEditorSplitter::handle:hover,
        QSplitter#fileBrowserSplitter::handle:hover { background:@focusLine@; }
        QScrollBar::handle:vertical { background:@scrollThumb@; }
        QFrame#filePanel { background:@surface@; border:0; border-radius:0; }
        QWidget#fileToolbar { background:@surface@; border:0; border-bottom:1px solid @line@; }
        QLineEdit#remotePathEdit {
            background:@canvas@; color:@text@; border:1px solid @line@; border-radius:5px;
        }
        QLineEdit#remotePathEdit:focus { border-color:@focusLine@; }
        QToolButton#remotePathSegment { color:@muted@; }
        QToolButton#remotePathSegment[current="true"] { color:@text@; }
        QTreeWidget#remoteDirectoryTree {
            background:@surface@; color:@text@; border:0;
        }
        QTreeWidget#remoteFileTree {
            background:@surface@; color:@text@; alternate-background-color:@canvas@; border:0;
        }
        QTreeWidget#remoteFileTree::item { border:0; }
        QTreeWidget#remoteFileTree::item:selected,
        QTreeWidget#remoteDirectoryTree::item:selected { background:@selection@; color:@accent@; }
        QTreeWidget#remoteFileTree QHeaderView::section {
            background:@surface@; color:@muted@; border:0; border-bottom:1px solid @line@;
        }
        QWidget#remoteFileEditor { background:@surface@; }
        QTabBar#remoteFileEditorTabs { background:@canvas@; border:0; border-bottom:1px solid @line@; }
        QTabBar#remoteFileEditorTabs::tab {
            background:@canvas@; color:@muted@; border:0; border-bottom:2px solid transparent;
        }
        QTabBar#remoteFileEditorTabs::tab:selected {
            background:@surface@; color:@text@; border:0; border-bottom:2px solid @accent@;
        }
        QTabBar#remoteFileEditorTabs::tab:hover:!selected { background:@raised@; }
        QFrame#remoteFileEditorToolbar, QFrame#remoteFileEditorStatusBar {
            background:@surface@; border:0;
        }
        QFrame#remoteFileEditorToolbar { border-bottom:1px solid @line@; }
        QFrame#remoteFileEditorStatusBar { border-top:1px solid @line@; }
        QLabel#remoteFileEditorPath, QLabel#remoteFileEditorStatus,
        QLabel#remoteFileEditorPosition, QLabel#remoteFileEditorLanguage { color:@muted@; }
        QPlainTextEdit#remoteFileEditorText { background:@surface@; color:@text@; }
        QWidget#paneAgentPanel { background:@canvas@; }
        QLabel#agentStatus { color:@muted@; font-size:11px; }
        QLabel#agentLanguageHint { color:@muted@; font-size:11px; background:transparent; }
        QWidget#agentToolbar QPushButton { min-height:24px; padding:0 8px; font-size:11px; }
        QWidget#agentToolbar QComboBox { min-height:24px; padding:0 24px 0 8px; font-size:11px; }
        QTextBrowser#agentChineseGuide { color:@text@; background:@surface@; border:1px solid @line@; border-radius:5px; padding:8px; font-size:12px; }
        QTreeWidget#availableSoftwareList { color:@text@; background:@canvas@; alternate-background-color:@surface@; border:1px solid @line@; font-weight:400; }
        QTreeWidget#availableSoftwareList::item { border:0; border-bottom:1px solid @line@; padding:3px 0; }
        QPlainTextEdit#softwareInstallLog { color:@text@; background:@canvas@; border:1px solid @line@; border-radius:4px; font-weight:400; }
        QLabel#softwareSafetyNote, QLabel#softwareInstallStatus { color:@muted@; font-size:11px; font-weight:400; }
        QWidget#terminalRecentPage { background:@surface@; }
        QLabel#recentLoginTitle, QLabel#recentLoginHint { color:@text@; }
        QLabel#recentLoginEmpty, QLabel#credentialManagerStatus { color:@muted@; }
        QTabWidget#connectionHomeTabs::pane { background:@surface@; border:0; }
        QTabBar#connectionHomeTabBar { background:@surface@; }
        QTabBar#connectionHomeTabBar::tab { color:@muted@; background:transparent; }
        QTabBar#connectionHomeTabBar::tab:selected { color:@text@; background:transparent; border-bottom-color:@accent@; }
        QTabWidget#connectionHomeTabs QTreeWidget#hostList, QTreeWidget#recentLoginList,
        QTreeWidget#savedCredentialList {
            background:@surface@; color:@text@; alternate-background-color:@canvas@; border:1px solid @line@;
        }
        QTabWidget#connectionHomeTabs QTreeWidget::item { border:0; }
        QTabWidget#connectionHomeTabs QTreeWidget::item:selected,
        QTabWidget#connectionHomeTabs QTreeWidget::branch:selected {
            background:@selection@; color:@accent@;
        }
        QTabWidget#connectionHomeTabs QTreeWidget QHeaderView::section {
            background:@canvas@; color:@muted@; border:0; border-bottom:1px solid @line@;
        }
        QTabWidget#connectionHomeTabs QFrame#hostSidebar { background:@surface@; border:0; }
        QTabWidget#connectionHomeTabs QLabel#hostManagementHint { color:@muted@; }
        QTabWidget#connectionHomeTabs QLineEdit#hostSearch {
            background:@canvas@; color:@text@; border:1px solid @line@; border-radius:5px;
        }
        QTabWidget#connectionHomeTabs QLineEdit#hostSearch:focus { border-color:@focusLine@; }
        QTabWidget#connectionHomeTabs QPushButton#hostAddButton {
            background:@accent@; color:white; border:1px solid @accent@;
        }
        QTabWidget#connectionHomeTabs QPushButton#hostAddButton:hover { background:@focusLine@; }
        QTabBar#connectionHomeTabBar::tab:hover { background:@canvas@; color:@text@; }
        QPushButton#editSavedCredential, QPushButton#deleteSavedCredential {
            color:@text@; background:@surface@; border:1px solid @line@;
        }
        QPushButton#editSavedCredential:hover, QPushButton#deleteSavedCredential:hover { background:@raised@; }
    )QSS");
    const auto c = workspaceColors(dark);
    const QMap<QString, QColor> tokens{{"background",c.background},{"canvas",c.canvas},{"surface",c.surface},
        {"raised",c.raised},{"line",c.line},{"focusLine",c.focusLine},{"text",c.text},{"muted",c.muted},
        {"selection",QColor(dark ? "#293C53" : "#EAF1FC")},{"accent",QColor(dark ? "#8BB8EE" : "#3978C8")},
        {"scrollThumb",QColor(dark ? "#4B535F" : "#C8CDD5")}};
    for (auto it=tokens.cbegin();it!=tokens.cend();++it) style.replace("@"+it.key()+"@",it.value().name());
    return style;
}

QString terminalStyleSheet(bool dark)
{
    auto style = QStringLiteral(R"QSS(
        QFrame#terminalSplitPane { border:1px solid @border@; border-radius:6px; background:@canvas@; }
        QFrame#terminalSplitPane[activeSplitPane="true"] { border-color:@focusLine@; }
        QFrame#terminalPanel, QFrame#terminalPanel[activeSplitPane="true"] {
            border:0; border-radius:0; background:@canvas@;
        }
        QWidget#terminalOutput { background: @canvas@; border: 0; }
        QFrame#terminalCommandBlockTools {
            background: @overlay@; border: 1px solid @controlBorder@; border-radius: 5px;
        }
        QToolButton#terminalCommandBlockCopyText,
        QToolButton#terminalCommandBlockCopyImage {
            background: transparent; border: 0; border-radius: 3px; padding: 0;
        }
        QToolButton#terminalCommandBlockCopyText:hover,
        QToolButton#terminalCommandBlockCopyImage:hover { background: @hover@; }
        QFrame#terminalSearchBar {
            background: @overlay@; border: 1px solid @controlBorder@; border-radius: 5px;
        }
        QLineEdit#terminalSearchInput {
            color: @text@; background: @canvas@; border: 1px solid @controlBorder@;
            border-radius: 4px; min-height: 25px; padding: 0 7px;
            selection-color: #17233D; selection-background-color: #FFF36A;
        }
        QLineEdit#terminalSearchInput:focus { border-color: #69A8D8; }
        QLabel#terminalSearchCounter { color: @muted@; font-size: 11px; }
        QToolButton#terminalSearchPrevious, QToolButton#terminalSearchNext,
        QToolButton#terminalSearchClose {
            color: @muted@; background: transparent; border: 0; border-radius: 3px;
            padding: 0; font-size: 15px;
        }
        QToolButton#terminalSearchPrevious:hover, QToolButton#terminalSearchNext:hover,
        QToolButton#terminalSearchClose:hover { color: @text@; background: @hover@; }
        QToolButton#terminalSearchPrevious:disabled, QToolButton#terminalSearchNext:disabled { color: @disabled@; }
        QLineEdit#terminalInput {
            background: @canvas@; border: 0;
            border-radius: 0; color: @text@; font-family: Menlo, Monaco, Consolas, monospace;
            padding-left: 11px;
        }
        QWidget#terminalInputRow { background: @canvas@; border:0; }
        QWidget#terminalInputRow QLineEdit#terminalInput { border-top: 0; }
        QToolButton[terminalFooterButton="true"] {
            background: transparent; border: 1px solid transparent; border-radius: 4px; padding: 0;
        }
        QToolButton[terminalFooterButton="true"]:hover,
        QToolButton[terminalFooterButton="true"]:checked {
            background: @pressed@; border-color: @controlBorder@;
        }
        QFrame#commandHistoryPanel {
            background: @surface@; border: 0; border-top: 1px solid @controlBorder@;
        }
        QLabel#commandHistoryTitle { color: @text@; font-size: 13px; font-weight: 650; }
        QLabel#commandHistoryScope {
            color: @muted@; background: @active@; border: 1px solid @controlBorder@;
            border-radius: 3px; padding: 2px 5px; font-size: 10px;
        }
        QTabBar#commandHistoryTabs { background: transparent; }
        QTabBar#commandHistoryTabs::tab {
            min-width: 48px; min-height: 25px; padding: 0 6px; color: @muted@;
            background: transparent; border: 0; border-bottom: 2px solid transparent;
        }
        QTabBar#commandHistoryTabs::tab:selected { color: @selectedText@; border-bottom-color: #1684FF; }
        QLineEdit#commandHistorySearch {
            color: @text@; background: @canvas@; border: 1px solid @controlBorder@;
            border-radius: 4px; min-height: 25px; padding: 0 7px;
            selection-color: @selectedText@; selection-background-color: @selected@;
        }
        QLineEdit#commandHistorySearch:focus { border-color: #1684FF; }
        QToolButton#commandHistoryClearButton, QToolButton#commandHistoryCloseButton,
        QToolButton#commandHistoryClearSearchButton {
            color: @muted@; background: transparent; border: 1px solid @controlBorder@;
            border-radius: 4px; min-height: 25px; padding: 0 7px;
        }
        QToolButton#commandHistoryCloseButton, QToolButton#commandHistoryClearSearchButton {
            min-width: 25px; max-width: 25px; padding: 0; font-size: 16px;
        }
        QToolButton#commandHistoryClearSearchButton:disabled { color: @disabled@; border-color: @border@; }
        QToolButton#commandHistoryClearButton:hover, QToolButton#commandHistoryCloseButton:hover,
        QToolButton#commandHistoryClearSearchButton:hover {
            color: @text@; background: @pressed@;
        }
        QTreeWidget#commandHistoryList {
            color: @text@; background: @canvas@; alternate-background-color: @surface@;
            border: 1px solid @border@; outline: 0;
        }
        QTreeWidget#commandHistoryList::item { min-height: 28px; border-bottom: 1px solid @border@; }
        QTreeWidget#commandHistoryList::item:selected { color: @selectedText@; background: @selected@; }
        QTreeWidget#commandHistoryList QHeaderView::section {
            color: @muted@; background: @raised@; border: 0; border-bottom: 1px solid @border@;
            min-height: 25px; padding: 0 6px;
        }
        QFrame#terminalPaneHeader, QFrame#terminalPaneHeader[activeSplitPane="true"] {
            background:@canvas@; border:0; border-radius:0;
        }
        QLabel#terminalPaneIdentity { color:@muted@; font-size:11px; }
        QTabBar#terminalModeTabs { background:transparent; border:0; }
        QTabBar#terminalModeTabs::tab {
            min-width:0; min-height:14px; max-height:14px; padding:1px 7px; margin:1px 2px;
            color:@muted@; background:transparent; border:1px solid transparent; border-radius:9px;
            font-size:11px; font-weight:400;
        }
        QTabBar#terminalModeTabs::tab:selected { color:@selectedText@; background:@canvas@; border:1px solid #73B4F6; font-weight:600; }
        QTabBar#terminalModeTabs::tab:hover:!selected { background:@hover@; border-color:@border@; }
        QFrame#terminalPaneHeader QToolButton { color:@muted@; background:transparent; border:0; border-radius:4px; padding:0; font-size:14px; }
        QFrame#terminalPaneHeader QToolButton:hover { color:@text@; background:@hover@; }
        QToolButton#terminalPaneCloseButton:hover { color:#DC5265; background:rgba(220,82,101,24); }
        QFrame#terminalPaneHeader QToolButton::menu-indicator { image:none; }
        QWidget#titlebarTabsHost QWidget#terminalTabToolbar { background:transparent; border:0; }
        QWidget#titlebarTabsHost QTabBar#terminalSessionTabs { background:transparent; }
        QWidget#terminalTabToolbar { background: @surface@; border-bottom: 1px solid @border@; }
        QTabBar#terminalSessionTabs { background: @surface@; }
        QTabBar#terminalSessionTabs::tab {
            min-width: 0; min-height: 30px; padding: 0 4px; color: @muted@;
            background: @raised@; border-right: 1px solid @border@;
        }
        QTabBar#terminalSessionTabs::tab:selected { color: @selectedText@; background: @canvas@; border-bottom: 2px solid #1684FF; }
        QWidget#terminalTabCloseContainer { background: transparent; }
        QToolButton#terminalTabCloseButton {
            color: @muted@; background: transparent; border: 0; border-radius: 3px;
            font-size: 16px; padding: 0;
        }
        QToolButton#terminalTabCloseButton:hover { color: @selectedText@; background: @controlBorder@; }
        QToolButton#terminalNewTabButton {
            color: @muted@; background: @raised@; border: 1px solid @controlBorder@; border-radius: 4px;
            font-size: 18px; padding: 0;
        }
        QToolButton#terminalNewTabButton:hover { color: @text@; background: @pressed@; }
        QWidget#terminalLoadingOverlay { background: @overlayTint@; }
        QFrame#terminalLoadingCard {
            background: @overlay@; border: 1px solid @controlBorder@; border-radius: 7px;
        }
        QLabel#terminalLoadingTitle { color: @text@; font-size: 15px; font-weight: 650; }
        QLabel#terminalLoadingDetail { color: @muted@; font-size: 12px; }
        QLineEdit#terminalConnectionPassword {
            background: @canvas@; color: @text@; border: 1px solid @controlBorder@;
            border-radius: 4px; padding: 7px; selection-background-color: #007DB8;
        }
        QCheckBox#terminalRememberPassword { color: @muted@; background: transparent; }
        QPushButton#terminalPasswordConnectButton {
            background: #007DB8; color: white; border: 1px solid #168FC3;
            border-radius: 4px; padding: 6px 10px;
        }
        QPushButton#terminalPasswordConnectButton:hover { background: #168DC3; }
        QPushButton#terminalPasswordConnectButton:disabled { background: @raised@; color: @disabled@; }
        QProgressBar#terminalLoadingProgress { border: 0; border-radius: 2px; background: @border@; }
        QProgressBar#terminalLoadingProgress::chunk { background: #1684FF; border-radius: 2px; }
        QWidget#terminalLoadingOverlay QLabel#asciiCredentialHint { color:@warning@; }
    )QSS");
    const auto colors = terminalColors(dark);
    style.replace(QStringLiteral("@canvas@"), colors.background.name());
    style.replace(QStringLiteral("@text@"), colors.foreground.name());
    const auto theme = workspaceColors(dark);
    style.replace(QStringLiteral("@surface@"), theme.surface.name());
    style.replace(QStringLiteral("@raised@"), theme.raised.name());
    style.replace(QStringLiteral("@overlay@"), dark ? QStringLiteral("#102A40") : QStringLiteral("#F4F8FD"));
    style.replace(QStringLiteral("@border@"), theme.line.name());
    style.replace(QStringLiteral("@focusLine@"), theme.focusLine.name());
    style.replace(QStringLiteral("@controlBorder@"), dark ? QStringLiteral("#31506B") : QStringLiteral("#B8CADC"));
    style.replace(QStringLiteral("@hover@"), dark ? QStringLiteral("#294159") : QStringLiteral("#E2EDF9"));
    style.replace(QStringLiteral("@pressed@"), dark ? QStringLiteral("#1C344A") : QStringLiteral("#DEECFB"));
    style.replace(QStringLiteral("@active@"), dark ? QStringLiteral("#17354A") : QStringLiteral("#E8F3FF"));
    style.replace(QStringLiteral("@muted@"), theme.muted.name());
    style.replace(QStringLiteral("@disabled@"), dark ? QStringLiteral("#536B80") : QStringLiteral("#9AAABC"));
    style.replace(QStringLiteral("@selected@"), dark ? QStringLiteral("#17466F") : QStringLiteral("#DDEEFF"));
    style.replace(QStringLiteral("@selectedText@"), dark ? QStringLiteral("#FFFFFF") : QStringLiteral("#145CA8"));
    style.replace(QStringLiteral("@overlayTint@"), dark ? QStringLiteral("rgba(7, 18, 29, 190)") : QStringLiteral("rgba(243, 246, 250, 220)"));
    style.replace(QStringLiteral("@warning@"), dark ? QStringLiteral("#E2AB64") : QStringLiteral("#A75B00"));
    return style;
}
} // namespace

QString applicationStyleSheet(bool dark)
{
    auto style = QStringLiteral(R"QSS(
        * {
            font-family: "PingFang SC", "Hiragino Sans GB", "Helvetica Neue", Helvetica, Arial;
            font-size: 13px;
            font-weight: 400;
            color: #303640;
        }
        QMainWindow, QWidget#appRoot { background: #F3F6FA; }
        QToolBar#windowControlsToolbar {
            background: transparent; border: 0; spacing: 3px; padding: 1px 6px;
        }
        QToolButton#newConnectionButton, QToolButton#monitorToggleButton,
        QToolButton#terminalSettingsButton, QToolButton#themeModeButton {
            background: transparent; border: 1px solid transparent; border-radius: 5px;
        }
        QToolButton#themeModeButton { font-size:16px; font-family:"Helvetica Neue", Arial; }
        QToolButton#newConnectionButton::menu-indicator { image: none; }
        QToolButton#newConnectionButton:hover, QToolButton#monitorToggleButton:hover,
        QToolButton#terminalSettingsButton:hover, QToolButton#themeModeButton:hover {
            background: #E8EDF3; border-color: #CCD7E3;
        }
        QToolButton#newConnectionButton:pressed, QToolButton#monitorToggleButton:pressed,
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
        QLabel#asciiCredentialHint { color:#A75B00; font-size:11px; }
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
        QSpinBox, QDoubleSpinBox {
            min-height:30px; border:1px solid #D8E0EA; border-radius:4px;
            background:#FFFFFF; padding:0 24px 0 8px;
            selection-color:white; selection-background-color:#006EFF;
        }
        QSpinBox:focus, QDoubleSpinBox:focus { border-color:#006EFF; }
        /* The embedded editor must not inherit QLineEdit's frame, padding or
           minimum height. A full border also disables the native macOS bezel. */
        QAbstractSpinBox QLineEdit {
            min-height:0; border:0; border-radius:0; padding:0; background:transparent;
        }
        QSpinBox::up-button, QDoubleSpinBox::up-button {
            subcontrol-origin:border; subcontrol-position:top right;
            width:20px; border:0; border-left:1px solid #D8E0EA;
            border-top-right-radius:4px; background:#F4F7FA;
        }
        QSpinBox::down-button, QDoubleSpinBox::down-button {
            subcontrol-origin:border; subcontrol-position:bottom right;
            width:20px; border:0; border-left:1px solid #D8E0EA;
            border-bottom-right-radius:4px; background:#F4F7FA;
        }
        QSpinBox::up-button:hover, QSpinBox::down-button:hover,
        QDoubleSpinBox::up-button:hover, QDoubleSpinBox::down-button:hover { background:#E4EDF7; }
        QSpinBox::up-button:pressed, QSpinBox::down-button:pressed,
        QDoubleSpinBox::up-button:pressed, QDoubleSpinBox::down-button:pressed { background:#D5E4F5; }
        QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {
            image:url(:/assets/spin-up.svg); width:10px; height:10px;
        }
        QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {
            image:url(:/assets/spin-down.svg); width:10px; height:10px;
        }
        QSpinBox:disabled, QDoubleSpinBox:disabled { color:#9AA7B6; background:#F4F7FA; }
        QAbstractSpinBox QLineEdit:disabled { color:#9AA7B6; }
        QPushButton {
            min-height: 30px; border: 1px solid #D6DEE8; border-radius: 4px;
            background: white; padding: 0 12px; color: #3C4D63;
        }
        QPushButton:hover { border-color: #8BBFFF; color: #0052D9; }
        QPushButton:pressed { background: #E8F3FF; }
        QPushButton#cancelServerTools:disabled { color:#8A9BAF; background:#EDF2F7; border-color:#DFE6EF; }
        QPushButton#primaryButton { background: #006EFF; border-color: #006EFF; color: white; }
        QPushButton#primaryButton:hover { background: #005FE5; }
        QPushButton#hostAddButton { min-width: 100px; padding: 0 12px; }
        QFrame#hostSidebar, QFrame#statusBar, QFrame#monitorIdentity { background: white; }
        QFrame#hostSidebar { border-right: 1px solid #DFE6EF; }
        QFrame#monitorIdentity { border-bottom: 1px solid #E4EAF1; }
        QFrame#statusBar { border-top: 1px solid #DFE6EF; }
        QWidget#monitorRail { background:#F4F7FB; border-right:1px solid #E2E8F0; }
        QWidget#operationsWorkspace, QWidget#terminalWorkspacePane, QWidget#fileWorkspacePane { background: #F3F6FA; }
        QScrollArea#monitorScrollArea, QWidget#monitorScrollContent { background:#F4F7FB; }
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
        QComboBox#groupEditor, QComboBox#terminalFontFamilyCombo { padding-right:32px; }
        QComboBox#groupEditor::drop-down, QComboBox#terminalFontFamilyCombo::drop-down {
            width:30px; border-left:1px solid #E1E7EF;
        }
        QComboBox#groupEditor::down-arrow, QComboBox#terminalFontFamilyCombo::down-arrow {
            image:url(:/assets/spin-down.svg); width:12px; height:12px;
        }
        QComboBox QAbstractItemView { background: white; border: 1px solid #D8E0EA; selection-background-color: #E8F3FF; }
        QMenu {
            background: white; border: 1px solid #D8E0EA; border-radius: 5px; padding: 5px;
        }
        QMenu::item { min-height: 26px; padding: 0 28px 0 9px; border-radius: 3px; }
        QMenu::item:selected { color: #0052D9; background: #E8F3FF; }
        QMenu::indicator { left: 8px; }
        QFrame#filePanel {
            background:white; border:1px solid #E1E7EF; border-radius:8px;
        }
        QWidget#fileToolbar { border-bottom:1px solid #E5EAF0; background:#FBFCFD; }
        QLabel#fileWorkspacePlaceholder { color:#8B9AAF; background:#FBFCFD; }
        QLabel#filePanelTitle { font-weight:650; }
        QLabel#fileServerLabel { color:#738297; font-size:12px; }
        QLabel#fileStatusLabel { color:#738297; font-size:11px; }
        QToolButton#remotePathSegment, QToolButton#remotePathOverflow, QToolButton#remotePathEditButton {
            color:#53677E; background:transparent; border:0; border-radius:3px; padding:0;
            font-size:12px;
        }
        QToolButton#remotePathSegment[current="true"] { color:#183B60; font-weight:600; }
        QToolButton#remotePathSegment:hover, QToolButton#remotePathOverflow:hover,
        QToolButton#remotePathEditButton:hover { color:#006EFF; background:#E8F3FF; }
        QToolButton#remotePathSegment:pressed, QToolButton#remotePathOverflow:pressed,
        QToolButton#remotePathEditButton:pressed { background:#D5E8FC; }
        QToolButton#remotePathOverflow::menu-indicator { image:none; }
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
        QLabel#monitorSystemTitle { color:#263E55; font-size:12px; font-weight:600; }
        QLabel#monitorSystemCaption { color:#738398; font-size:11px; }
        QLabel#monitorUptimeValue { color:#263E55; font-size:11px; font-weight:500; }
        QFrame#metricRow { background:transparent; border:0; }
        QFrame#metricRow[lastRow="true"] { border-bottom: 0; }
        QLabel#metricTitle { color: #5F6F82; font-size: 12px; }
        QLabel#metricValue { color:#243B53; font-size:15px; font-weight:600; }
        QLabel#metricDetail { color:#78899B; font-size:11px; }
        QFrame#metricCorePanel { background:#F8FAFC; border:1px solid #D9E4EF; border-radius:5px; }
        QLabel#metricCoreName, QLabel#metricCoreValue, QLabel#metricCoreMore { color:#708297; font-size:10px; }
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
            border:1px solid #E1E7EF; border-radius:8px; background:#FFFFFF;
        }
        QLabel#detailSectionTitle { font-size:12px; font-weight:600; color:#263E55; }
        QLabel#detailMuted { color:#8B9AAF; font-size:10px; }
        QWidget#networkRateRow { background:transparent; }
        QLabel#networkUploadRate { color:#2F6FD6; font-size:10px; font-weight:500; }
        QLabel#networkDownloadRate { color:#B8600B; font-size:10px; font-weight:500; }
        QComboBox#networkInterfaceCombo { min-height:25px; padding:0 2px 0 8px; font-size:11px; }
        QComboBox#networkInterfaceCombo::drop-down { width:26px; border:0; border-left:1px solid #E1E7EF; }
        QComboBox#networkInterfaceCombo::down-arrow { image:url(:/assets/chevron-down.svg); width:12px; height:12px; }
        QComboBox#networkInterfaceCombo QAbstractItemView {
            color:#33485E; background:#FFFFFF; font-size:12px; outline:0; padding:3px;
            selection-background-color:#E8F1FF; selection-color:#1E5BAA;
        }
        QComboBox#networkInterfaceCombo QAbstractItemView::item { min-height:26px; padding:0 7px; border-radius:3px; }
        QComboBox#networkInterfaceCombo QAbstractItemView::item:selected { color:#1E5BAA; background:#E8F1FF; }
        QTabBar#processMetricTabs::tab {
            min-width:40px; min-height:27px; max-height:27px; padding:0 5px;
            color:#60758B; background:#F3F6FA; font-size:11px; border:0; border-radius:5px;
        }
        QTabBar#processMetricTabs::tab:selected {
            color:#296FD6; background:#E9F1FF; border:0; font-weight:600;
        }
        QTreeWidget#realtimeProcessList, QTreeWidget#fileSystemUsageList {
            border:0; background:#FFFFFF; alternate-background-color:#F7F9FC;
            font-size:11px;
        }
        QTreeWidget#realtimeProcessList::item, QTreeWidget#fileSystemUsageList::item {
            min-height:24px; max-height:24px; padding:0 4px; border:0;
        }
        QTreeWidget#realtimeProcessList QHeaderView::section,
        QTreeWidget#fileSystemUsageList QHeaderView::section {
            height:24px; background:#FFFFFF; border:0;
            padding:0 4px; color:#8190A2; font-size:10px; font-weight:400;
        }
    )QSS");
    style += QStringLiteral(R"QSS(
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
        QToolButton#transferOpenDirectory { background:transparent; border:0; border-radius:4px; padding:0; }
        QToolButton#transferOpenDirectory:hover { background:#E8F3FF; }
        QToolButton#transferOpenDirectory:pressed { background:#D5E8FC; }
        QToolButton#transferOpenDirectory:focus { border:1px solid #1684FF; }
        QLabel#alertTitle { color: #9A4E1D; font-weight: 650; }
        QLabel#alertText { color: #8A6A54; font-size: 11px; }
        QWidget#terminalRecentPage { background: #0C1825; }
        QLabel#recentLoginTitle { color: #F2F6FA; font-size: 18px; font-weight: 650; }
        QLabel#recentLoginHint, QLabel#recentLoginEmpty { color: #7F95AA; }
        QLabel#credentialManagerStatus { color: #8EA3B7; }
        QPushButton#addSavedCredential {
            color: #E5F3FF; background: #0C5577; border: 1px solid #1A718B;
        }
        QPushButton#editSavedCredential, QPushButton#deleteSavedCredential {
            color: #D0DBE5; background: #152B3B; border: 1px solid #2A4558;
        }
        QPushButton#addSavedCredential:hover, QPushButton#editSavedCredential:hover,
        QPushButton#deleteSavedCredential:hover { background: #1C4560; }
        QLabel#recentLoginEmpty { font-size: 14px; }
        QTreeWidget#recentLoginList, QTreeWidget#savedCredentialList {
            color: #D8E4EF; background: #101E2D; alternate-background-color: #132235;
            border: 1px solid #22364C; border-radius: 5px; padding: 0;
        }
        QTreeWidget#recentLoginList::item, QTreeWidget#savedCredentialList::item { min-height: 34px; padding: 0 8px; border-radius: 0; }
        QTreeWidget#recentLoginList::item:selected, QTreeWidget#savedCredentialList::item:selected { color: white; background: #17466F; border: 0; }
        QTreeWidget#recentLoginList QHeaderView::section, QTreeWidget#savedCredentialList QHeaderView::section {
            color: #8EA3B7; background: #132235; border: 0; border-bottom: 1px solid #22364C;
            min-height: 30px; padding: 0 8px;
        }
        QTabWidget#connectionHomeTabs::pane { border: 0; background: #0C1825; }
        QTabBar#connectionHomeTabBar { background: #0C1825; }
        QTabBar#connectionHomeTabBar::tab {
            color: #91A6BA; background: transparent; border: 0;
            border-bottom: 2px solid transparent; padding: 10px 18px; font-size: 14px;
        }
        QTabBar#connectionHomeTabBar::tab:selected {
            color: #EFF6FC; border-bottom-color: #00A9E8; font-weight: 650;
        }
        QTabBar#connectionHomeTabBar::tab:hover { color: #EFF6FC; background: #132235; }
        QTabWidget#connectionHomeTabs QFrame#hostSidebar { background: #0C1825; border: 0; }
        QTabWidget#connectionHomeTabs QLabel#hostManagementHint { color: #7F95AA; font-size: 12px; }
        QTabWidget#connectionHomeTabs QLineEdit#hostSearch {
            color: #D8E4EF; background: #101E2D; border: 1px solid #32495E;
        }
        QTabWidget#connectionHomeTabs QLineEdit#hostSearch:focus { border-color: #00A9E8; }
        QTabWidget#connectionHomeTabs QPushButton#hostAddButton {
            color: #EFF6FC; background: #17466F; border: 1px solid #2A6085;
        }
        QTabWidget#connectionHomeTabs QPushButton#hostAddButton:hover { background: #205B85; }
        QTabWidget#connectionHomeTabs QTreeWidget#hostList {
            color: #D8E4EF; background: #101E2D; alternate-background-color: #132235;
            border: 1px solid #22364C; border-radius: 5px; outline: 0; padding: 0;
        }
        QTabWidget#connectionHomeTabs QTreeWidget::item {
            min-height: 34px; padding: 0 8px; border: 0; border-radius: 0;
        }
        QTabWidget#connectionHomeTabs QTreeWidget::item:selected { color: white; background: #17466F; }
        QTabWidget#connectionHomeTabs QTreeWidget::branch { background: transparent; }
        QTabWidget#connectionHomeTabs QTreeWidget::branch:selected { background: #17466F; }
        QTabWidget#connectionHomeTabs QTreeWidget QHeaderView::section {
            color: #8EA3B7; background: #132235; border: 0; border-bottom: 1px solid #22364C;
            min-height: 30px; padding: 0 8px;
        }
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
        QWidget#remoteFileEditor { background:#F3F6FA; }
        QToolButton#fileWorkspaceExpandButton, QToolButton#fileWorkspaceCloseButton {
            color:#536D88; background:transparent; border:1px solid transparent; border-radius:4px;
            font-size:17px; padding:0;
        }
        QToolButton#fileWorkspaceExpandButton:hover, QToolButton#fileWorkspaceExpandButton:checked,
        QToolButton#fileWorkspaceCloseButton:hover { color:#2469BA; background:#E8F3FF; border-color:#C4DBF8; }
        QSplitter#paneEditorSplitter::handle { background:#D8E2ED; }
        QSplitter#paneEditorSplitter::handle:hover { background:#8BBDF4; }
        QPlainTextEdit#remoteFileEditorText {
            color:#25354A; background:#FFFFFF; border:0; border-radius:0; padding:0;
            selection-background-color:#D7E9FF; selection-color:#173A64;
        }
        QPlainTextEdit#remoteFileEditorText:disabled { color:#8190A2; background:#FAFBFD; }
        QTabBar#remoteFileEditorTabs { background:#EDF1F6; border-bottom:1px solid #DCE4EE; }
        QTabBar#remoteFileEditorTabs::tab {
            min-width:150px; max-width:260px; min-height:35px; max-height:35px;
            padding:0 12px; color:#718195; background:#EDF1F6;
            border:0; border-right:1px solid #E0E6EF; border-bottom:2px solid transparent;
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
        QFrame#remoteFileEditorToolbar { background:#FFFFFF; border-bottom:1px solid #E2E9F2; }
        QLabel#remoteFileEditorPath { color:#63788F; font-size:12px; }
        QLabel#remoteFileEditorState { color:#698493; background:#F0F5F8; padding:3px 7px; border-radius:4px; font-size:11px; }
        QLabel#remoteFileEditorState[dirty="true"] { color:#A36A1B; background:#FFF3DE; }
        QFrame#remoteFileEditorToolbar QToolButton {
            border:1px solid transparent; border-radius:5px; padding:0 7px;
            color:#536D88; background:transparent; font-size:12px;
        }
        QFrame#remoteFileEditorToolbar QToolButton:hover { background:#EDF4FD; color:#2469BA; }
        QFrame#remoteFileEditorToolbar QToolButton:checked { background:#E5F0FF; color:#2469BA; border-color:#C4DBF8; }
        QFrame#remoteFileEditorToolbar QToolButton:disabled { color:#B8C5D3; }
        QFrame#remoteFileEditorToolbar QToolButton#remoteFileEditorUndo,
        QFrame#remoteFileEditorToolbar QToolButton#remoteFileEditorRedo { font-size:19px; padding:0; }
        QPushButton#remoteFileEditorSave { background:#2678DA; color:white; border:1px solid #2678DA; border-radius:5px; padding:0 18px; font-size:12px; }
        QPushButton#remoteFileEditorSave:hover { background:#1769C5; }
        QPushButton#remoteFileEditorSave:disabled { background:#EFF4FA; color:#8FA6BE; border-color:#DCE6F1; }
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
        QFrame#remoteFileEditorStatusBar { border-top:1px solid #E1E8F1; background:#F6F9FC; }
        QLabel#remoteFileEditorStatus, QLabel#remoteFileEditorPosition { color:#7B8FA5; font-size:11px; background:transparent; }
        QLabel#remoteFileEditorLanguage { color:#476885; font-size:11px; background:transparent; }
    )QSS");

    if (!dark) return style + workspaceStyleSheet(false) + terminalStyleSheet(false);
    style += QStringLiteral(R"QSS(
        * { color: #D7DBE3; }
        QLabel#asciiCredentialHint { color:#E2AB64; }
        QMainWindow, QDialog, QMessageBox, QInputDialog, QWidget#appRoot,
        QWidget#operationsWorkspace, QWidget#terminalWorkspacePane, QWidget#fileWorkspacePane {
            background: #111820;
        }
        QToolBar#windowControlsToolbar { background: #171F29; border-bottom: 1px solid #2A3542; }
        QToolButton#newConnectionButton:hover, QToolButton#monitorToggleButton:hover,
        QToolButton#terminalSettingsButton:hover, QToolButton#themeModeButton:hover {
            background: #263241; border-color: #3A495A;
        }
        QToolButton#newConnectionButton:pressed, QToolButton#monitorToggleButton:pressed,
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
        QAbstractSpinBox QLineEdit { color:#DCE6F0; background:transparent; }
        QSpinBox::up-button, QSpinBox::down-button,
        QDoubleSpinBox::up-button, QDoubleSpinBox::down-button {
            background:#1B2530; border-left-color:#354252;
        }
        QSpinBox::up-button:hover, QSpinBox::down-button:hover,
        QDoubleSpinBox::up-button:hover, QDoubleSpinBox::down-button:hover { background:#293949; }
        QSpinBox::up-button:pressed, QSpinBox::down-button:pressed,
        QDoubleSpinBox::up-button:pressed, QDoubleSpinBox::down-button:pressed { background:#344B61; }
        QSpinBox:disabled, QDoubleSpinBox:disabled { color:#647384; background:#151D25; }
        QAbstractSpinBox QLineEdit:disabled { color:#647384; }
        QComboBox#groupEditor::drop-down, QComboBox#terminalFontFamilyCombo::drop-down {
            border-left-color:#354252;
        }
        QPushButton {
            color: #C9D5E1; background: #1B2530; border-color: #354252;
        }
        QPushButton:hover { color: #76B8FF; border-color: #528AC1; background: #222F3D; }
        QPushButton:pressed { background: #293949; }
        QPushButton#cancelServerTools:disabled { color:#738397; background:#1B2632; border-color:#2D3A49; }
        QPushButton#primaryButton { color: white; background: #1677D2; border-color: #2588E5; }
        QPushButton#primaryButton:hover { background: #2388E8; }
        QMenu { color:#DCE6F0; background:#1A232D; border-color:#3A4654; }
        QMenu::item:selected { color:white; background:#174E78; }
        QToolTip { color:#E8F0F7; background:#202B36; border:1px solid #465566; }
        QFrame#hostSidebar, QFrame#statusBar, QFrame#monitorIdentity { background:#171F28; }
        QWidget#monitorRail, QScrollArea#monitorScrollArea, QWidget#monitorScrollContent { background:#111A24; }
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
        QToolButton#remotePathSegment, QToolButton#remotePathOverflow, QToolButton#remotePathEditButton { color:#A7BBCE; }
        QToolButton#remotePathSegment[current="true"] { color:#E0EDF7; }
        QToolButton#remotePathSegment:hover, QToolButton#remotePathOverflow:hover,
        QToolButton#remotePathEditButton:hover { color:#80C4FF; background:#293E51; }
        QToolButton#remotePathSegment:pressed, QToolButton#remotePathOverflow:pressed,
        QToolButton#remotePathEditButton:pressed { background:#35536E; }
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
        QFrame#metricCorePanel { background:#1A232D; border-color:#35485B; }
        QLabel#metricCoreName, QLabel#metricCoreValue, QLabel#metricCoreMore { color:#91A3B6; }
        QTreeWidget#hostList { background:#171F28; }
        QTreeWidget#hostList::item:selected { color:#FFFFFF; background:#174E78; }
        QPushButton#credentialSettingsButton { color:#8FA0B2; border-top-color:#303D4A; }
        QWidget#networkRateRow { background:transparent; }
        QLabel#networkUploadRate { color:#7BAAFF; }
        QLabel#networkDownloadRate { color:#FFB15C; }
        QComboBox#networkInterfaceCombo::drop-down { border-left-color:#354252; }
        QComboBox#networkInterfaceCombo QAbstractItemView {
            color:#DCE6F0; background:#1A232D; selection-background-color:#243F5E; selection-color:#E3EFFF;
        }
        QComboBox#networkInterfaceCombo QAbstractItemView::item:selected { color:#E3EFFF; background:#243F5E; }
        QTabBar#processMetricTabs::tab { color:#91A3B6; background:#1C2631; }
        QTabBar#processMetricTabs::tab:selected { color:#8ABBFF; background:#243950; border:0; }
        QTreeWidget#realtimeProcessList, QTreeWidget#fileSystemUsageList {
            background:#171F28; alternate-background-color:#1C2733; border:0;
        }
        QTreeWidget#realtimeProcessList QHeaderView::section,
        QTreeWidget#fileSystemUsageList QHeaderView::section {
            color:#91A3B6; background:#171F28; border:0;
        }
        QFrame#transferQueuePanel, QFrame#transferQueuePanel[popup="true"] {
            background:#18212B; border-color:#303D4A;
        }
        QLabel#transferQueueTitle, QLabel#transferName { color:#DCE7F2; }
        QLabel#transferQueueSummary { color:#A7B6C5; background:#263240; }
        QListWidget#transferQueueList::item { background:#1D2732; border-color:#354252; }
        QListWidget#transferQueueList::item:selected { color:white; background:#233241; border-color:#467198; }
        QProgressBar#transferProgress { background:#303B48; }
        QToolButton#transferOpenDirectory:hover { background:#2B4054; }
        QToolButton#transferOpenDirectory:pressed { background:#35536E; }
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
        QWidget#remoteFileEditor { background:#141D28; }
        QToolButton#fileWorkspaceExpandButton, QToolButton#fileWorkspaceCloseButton { color:#A8BCD1; }
        QToolButton#fileWorkspaceExpandButton:hover, QToolButton#fileWorkspaceExpandButton:checked,
        QToolButton#fileWorkspaceCloseButton:hover { color:#DFECFF; background:#263F5E; border-color:#3B5A7C; }
        QSplitter#paneEditorSplitter::handle { background:#303D4A; }
        QSplitter#paneEditorSplitter::handle:hover { background:#3F83B8; }
        QPlainTextEdit#remoteFileEditorText { color:#D5DFED; background:#18222F; border:0; selection-background-color:#304F76; selection-color:#F0F6FF; }
        QPlainTextEdit#remoteFileEditorText:disabled { color:#7D8EA4; background:#17202B; }
        QTabBar#remoteFileEditorTabs { background:#121B26; border-bottom-color:#2C3A4B; }
        QTabBar#remoteFileEditorTabs::tab { color:#869BB2; background:#121B26; border-right-color:#273546; }
        QTabBar#remoteFileEditorTabs::tab:selected { color:#E4EDF6; background:#1B2838; border-bottom-color:#60A7FC; }
        QTabBar#remoteFileEditorTabs::tab:hover:!selected { background:#1A2736; }
        QToolButton#remoteFileTabCloseButton { color:#91A3B6; }
        QToolButton#remoteFileTabCloseButton:hover { color:#FFFFFF; background:#2A3948; }
        QFrame#remoteFileEditorToolbar { background:#1B2838; border-bottom-color:#2C3B4F; }
        QLabel#remoteFileEditorPath { color:#91A7BF; }
        QLabel#remoteFileEditorState { color:#92ADBC; background:#243647; }
        QLabel#remoteFileEditorState[dirty="true"] { color:#E9BD79; background:#413527; }
        QFrame#remoteFileEditorToolbar QToolButton { color:#A8BCD1; }
        QFrame#remoteFileEditorToolbar QToolButton:hover { color:#DFECFF; background:#2A4059; }
        QFrame#remoteFileEditorToolbar QToolButton:checked { color:#91C4FF; background:#263F5E; border-color:#3B5A7C; }
        QFrame#remoteFileEditorToolbar QToolButton:disabled { color:#52677E; }
        QPushButton#remoteFileEditorSave { background:#337FD9; border-color:#337FD9; }
        QPushButton#remoteFileEditorSave:hover { background:#4390E9; }
        QPushButton#remoteFileEditorSave:disabled { background:#223448; color:#67839E; border-color:#30465E; }
        QFrame#remoteFileEditorStatusBar { background:#151F2C; border-top-color:#2C394C; }
        QLabel#remoteFileEditorStatus, QLabel#remoteFileEditorPosition { color:#8198B0; }
        QLabel#remoteFileEditorLanguage { color:#A0B9D4; }
    )QSS");
    return style + workspaceStyleSheet(true) + terminalStyleSheet(true);
}

void applyApplicationTheme(ThemeMode mode)
{
    auto *application = qobject_cast<QApplication *>(QCoreApplication::instance());
    if (!application) return;
#ifdef Q_OS_MACOS
    // Set the application appearance, not only the main window: new dialogs
    // and their native title bars must inherit the user's theme too.
    if (QGuiApplication::platformName() == QStringLiteral("cocoa")) {
        applyMacApplicationAppearance(static_cast<int>(mode));
    }
#endif
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
        palette.setColor(QPalette::Link, QColor(QStringLiteral("#62C3FF")));
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
        palette.setColor(QPalette::Link, QColor(QStringLiteral("#007AFF")));
        palette.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#9AA7B6")));
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#9AA7B6")));
    }
    const auto theme = workspaceColors(dark);
    palette.setColor(QPalette::Window, theme.background);
    palette.setColor(QPalette::WindowText, theme.text);
    palette.setColor(QPalette::Base, theme.surface);
    palette.setColor(QPalette::AlternateBase, theme.canvas);
    palette.setColor(QPalette::Text, theme.text);
    palette.setColor(QPalette::Button, theme.surface);
    palette.setColor(QPalette::ButtonText, theme.text);
    application->setProperty("noxshellDarkTheme", dark);
    application->setPalette(palette);
    application->setStyleSheet(applicationStyleSheet(dark));
}

} // namespace noxshell::ui
