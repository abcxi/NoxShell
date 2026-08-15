#include "../src/core/LinuxMetrics.h"
#include "../src/core/AppLogger.h"
#include "../src/core/CredentialStore.h"
#include "../src/core/FileTransferTask.h"
#include "../src/core/MetricHistory.h"
#include "../src/core/SshSession.h"
#include "../src/core/ServerRepository.h"
#include "../src/ui/AppTheme.h"
#include "../src/ui/FilePanel.h"
#include "../src/ui/HostSidebar.h"
#include "../src/ui/MainWindow.h"
#include "../src/ui/RemoteFileEditor.h"
#include "../src/ui/ServerDialog.h"
#include "../src/ui/TerminalWorkspace.h"
#include "../src/ui/TrendChart.h"
#include "../src/ui/TransferQueuePanel.h"
#include "../src/ui/TerminalView.h"
#include "../src/ui/VtTerminalModel.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QMenu>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSplitter>
#include <QSpinBox>
#include <QStackedWidget>
#include <QPushButton>
#include <QTabBar>
#include <QTemporaryDir>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QtTest>

#include <algorithm>

class SmokeTest final : public QObject {
    Q_OBJECT

private slots:
    void applicationBrandIsNoxShell()
    {
        QCOMPARE(QApplication::applicationName(), QStringLiteral("玄壳"));
        QCOMPARE(QApplication::organizationName(), QStringLiteral("NoxShell"));
        QCOMPARE(QApplication::applicationVersion(), QString::fromLatin1(NOXSHELL_APP_VERSION));
    }

    void applicationIconResourceLoadsAtDockSize()
    {
        const QImage image(QStringLiteral(":/assets/app-icon.png"));
        QVERIFY(!image.isNull());
        QVERIFY(image.hasAlphaChannel());
        QCOMPARE(image.pixelColor(0, 0).alpha(), 0);
        const QIcon icon(QStringLiteral(":/assets/app-icon.png"));
        QVERIFY(!icon.isNull());
        const auto pixmap = icon.pixmap(QSize(128, 128));
        QVERIFY(!pixmap.isNull());
        QCOMPARE(pixmap.size(), QSize(128, 128));
    }

    void loggerRedactsCommonSecrets()
    {
        const auto sanitized = noxshell::AppLogger::sanitize(
            QStringLiteral("password=hunter2 token:abc123 Authorization=BearerValue Bearer ey.secret.token"));
        QVERIFY(!sanitized.contains(QStringLiteral("hunter2")));
        QVERIFY(!sanitized.contains(QStringLiteral("abc123")));
        QVERIFY(!sanitized.contains(QStringLiteral("BearerValue")));
        QVERIFY(!sanitized.contains(QStringLiteral("ey.secret.token")));
        QVERIFY(sanitized.contains(QStringLiteral("<redacted>")));
    }

    void vtTerminalParsesAnsiCursorColorAndAlternateScreen()
    {
        noxshell::ui::VtTerminalModel model(12, 4);
        model.feed(QStringLiteral("hello"));
        QCOMPARE(model.plainText(), QStringLiteral("hello"));
        model.feed(QStringLiteral("\x1b[31;1mR\x1b[0m"));
        QCOMPARE(model.cell(0, 5).text, QStringLiteral("R"));
        QVERIFY(model.cell(0, 5).bold);
        QCOMPARE(model.cell(0, 5).foreground, QColor(QStringLiteral("#E06C75")));

        model.feed(QStringLiteral("\x1b[2;3H中"));
        QCOMPARE(model.cell(1, 2).text, QStringLiteral("中"));
        QVERIFY(model.cell(1, 3).wideContinuation);
        model.feed(QStringLiteral("\x1b[?25l"));
        QVERIFY(!model.cursorVisible());
        model.feed(QStringLiteral("\x1b[?5h"));
        QVERIFY(model.reverseVideo());
        model.feed(QStringLiteral("\x1b[?5l"));
        QVERIFY(!model.reverseVideo());
        model.feed(QStringLiteral("\x1b[?1049hALT"));
        QVERIFY(model.plainText().contains(QStringLiteral("ALT")));
        QVERIFY(!model.plainText().contains(QStringLiteral("hello")));
        model.feed(QStringLiteral("\x1b[?1049l"));
        QVERIFY(model.plainText().contains(QStringLiteral("helloR")));

        model.feed(QStringLiteral("\x1b[2J\x1b[Hone\r\ntwo"));
        QCOMPARE(model.plainText(), QStringLiteral("one\ntwo"));
    }

    void terminalViewGeneratesRawKeySequencesAndResizes()
    {
        noxshell::ui::TerminalView view;
        QSignalSpy inputSpy(&view, &noxshell::ui::TerminalView::inputGenerated);
        QSignalSpy sizeSpy(&view, &noxshell::ui::TerminalView::terminalSizeChanged);
        view.resize(640, 240);
        view.show();
        QTest::qWait(20);
        view.setFocus();
        QTest::keyClick(&view, Qt::Key_Up);
        QTest::keyClick(&view, Qt::Key_C, Qt::ControlModifier);
        QTest::keyClick(&view, Qt::Key_C, Qt::MetaModifier);
        QTest::keyClick(&view, Qt::Key_Tab);
        QTest::keyClick(&view, Qt::Key_Backtab);
        QTRY_COMPARE_WITH_TIMEOUT(inputSpy.count(), 5, 1000);
        QCOMPARE(inputSpy.at(0).at(0).toByteArray(), QByteArray("\x1b[A"));
        QCOMPARE(inputSpy.at(1).at(0).toByteArray(), QByteArray(1, '\x03'));
        QCOMPARE(inputSpy.at(2).at(0).toByteArray(), QByteArray(1, '\x03'));
        QCOMPARE(inputSpy.at(3).at(0).toByteArray(), QByteArray("\t"));
        QCOMPARE(inputSpy.at(4).at(0).toByteArray(), QByteArray("\x1b[Z"));
        QVERIFY(view.hasFocus());
        QVERIFY(!sizeSpy.isEmpty());
    }

    void terminalViewReportsSubmittedCommandAfterEditing()
    {
        noxshell::ui::TerminalView view;
        QSignalSpy commandSpy(&view, &noxshell::ui::TerminalView::commandSubmitted);
        view.resize(640, 240);
        view.show();
        view.setFocus();
        QTest::keyClicks(&view, QStringLiteral("cd /vaz"));
        QTest::keyClick(&view, Qt::Key_Backspace);
        QTest::keyClick(&view, Qt::Key_R);
        QTest::keyClick(&view, Qt::Key_Return);
        QCOMPARE(commandSpy.count(), 1);
        QCOMPARE(commandSpy.first().at(0).toString(), QStringLiteral("cd /var"));
    }

    void terminalInputClearsSelectionAndUsesThinCursor()
    {
        noxshell::ui::TerminalView view;
        view.resize(480, 180);
        view.show();
        view.feedText(QStringLiteral("[root@linux ~]# wrong"));
        QTest::qWait(20);

        const QFontMetrics metrics(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        const int cellWidth = qCeil(metrics.horizontalAdvance(QLatin1Char('M')));
        const int cellHeight = qCeil(metrics.height() + 1.0);
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, QPoint(cellWidth / 2, cellHeight / 2));
        QTest::mouseMove(&view, QPoint(cellWidth * 6, cellHeight / 2));
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, QPoint(cellWidth * 6, cellHeight / 2));
        QVERIFY(!view.selectedText().isEmpty());

        QSignalSpy inputSpy(&view, &noxshell::ui::TerminalView::inputGenerated);
        view.setFocus();
        QTest::keyClick(&view, Qt::Key_Backspace);
        QCOMPARE(inputSpy.count(), 1);
        QCOMPARE(inputSpy.first().at(0).toByteArray(), QByteArray(1, '\x7f'));
        QVERIFY(view.selectedText().isEmpty());
    }

    void terminalBackspaceEchoErasesOneCharacterWithoutLeavingReverseVideo()
    {
        noxshell::ui::VtTerminalModel model(40, 3);
        model.feed(QStringLiteral("[root@localhost ~]# 123"));
        model.feed(QStringLiteral("\b\x1b[K\b\x1b[K\b\x1b[K"));
        QCOMPARE(model.plainText(), QStringLiteral("[root@localhost ~]#"));
        QCOMPARE(model.cursorColumn(), 20);
        QVERIFY(!model.reverseVideo());

        model.feed(QStringLiteral("X\x1b[1K"));
        QVERIFY(model.plainText().isEmpty());

        model.feed(QStringLiteral("\x1b[?5h\x1b[?5l"));
        QVERIFY(!model.reverseVideo());
        for (int column = model.cursorColumn(); column < model.columns(); ++column) {
            QCOMPARE(model.cell(0, column).background, QColor(QStringLiteral("#0C1825")));
            QVERIFY(!model.cell(0, column).inverse);
        }
    }

    void emptyHostSidebarShowsAddHintInsteadOfBlankList()
    {
        noxshell::ui::HostSidebar sidebar;
        sidebar.show();
        auto *list = sidebar.findChild<QListWidget *>();
        QVERIFY(list);
        QCOMPARE(sidebar.servers().size(), 0);
        QCOMPARE(list->count(), 1);
        QVERIFY(list->item(0)->text().contains(QStringLiteral("添加")));
        QVERIFY(!(list->item(0)->flags() & Qt::ItemIsSelectable));
    }

    void hostSidebarSelectsWithoutConnectingAndActivatesOnDoubleClick()
    {
        noxshell::ServerProfile profile;
        profile.id = QStringLiteral("sidebar-host");
        profile.name = QStringLiteral("sidebar-host");
        profile.host = QStringLiteral("192.0.2.10");
        noxshell::ui::HostSidebar sidebar({profile});
        QSignalSpy selectedSpy(&sidebar, &noxshell::ui::HostSidebar::serverSelected);
        QSignalSpy connectSpy(&sidebar, &noxshell::ui::HostSidebar::serverConnectRequested);
        QSignalSpy collapseSpy(&sidebar, &noxshell::ui::HostSidebar::collapseRequested);
        sidebar.show();
        sidebar.selectFirstServer();
        QCOMPARE(selectedSpy.count(), 1);
        QCOMPARE(connectSpy.count(), 0);

        auto *list = sidebar.findChild<QListWidget *>(QStringLiteral("hostList"));
        QVERIFY(list);
        auto *search = sidebar.findChild<QLineEdit *>(QStringLiteral("hostSearch"));
        auto *addButton = sidebar.findChild<QPushButton *>(QStringLiteral("hostAddButton"));
        QVERIFY(search);
        QVERIFY(addButton);
        QCOMPARE(search->geometry().y(), addButton->geometry().y());
        QVERIFY(addButton->geometry().x() > search->geometry().x());
        auto *rowWidget = list->itemWidget(list->item(0));
        QVERIFY(rowWidget);
        QCOMPARE(rowWidget->findChild<QLabel *>(QStringLiteral("hostItemName"))->text(), QStringLiteral("sidebar-host"));
        QCOMPARE(rowWidget->findChild<QLabel *>(QStringLiteral("hostItemAddress"))->text(), QStringLiteral("192.0.2.10"));
        QCOMPARE(rowWidget->findChild<QLabel *>(QStringLiteral("hostItemName"))->geometry().y(),
            rowWidget->findChild<QLabel *>(QStringLiteral("hostItemAddress"))->geometry().y());
        QVERIFY(list->item(0)->sizeHint().height() <= 42);
        QVERIFY(!rowWidget->findChild<QLabel *>(QStringLiteral("hostItemState")));
        QVERIFY(!list->item(0)->toolTip().contains(QStringLiteral("离线")));
        QVERIFY(!list->item(0)->toolTip().contains(QStringLiteral("在线")));
        QVERIFY(sidebar.setServerState(profile.id, noxshell::ServerState::Online));
        QVERIFY(!rowWidget->findChild<QLabel *>(QStringLiteral("hostItemState")));
        list->itemDoubleClicked(list->item(0));
        QCOMPARE(connectSpy.count(), 1);
        QCOMPARE(collapseSpy.count(), 1);
        QCOMPARE(qvariant_cast<noxshell::ServerProfile>(connectSpy.first().at(0)).id, profile.id);
        QVERIFY(sidebar.findChild<QAction *>(QStringLiteral("hostConnectAction")));
        QVERIFY(sidebar.findChild<QAction *>(QStringLiteral("hostDuplicateAction")));
        QVERIFY(sidebar.findChild<QAction *>(QStringLiteral("hostDeleteAction")));
    }

    void terminalTabContextMenuDuplicatesAndClosesSessions()
    {
        noxshell::ui::TerminalWorkspace workspace(nullptr, nullptr);
        workspace.resize(900, 500);
        workspace.show();

        auto *recentPage = workspace.findChild<QWidget *>(QStringLiteral("terminalRecentPage"));
        auto *sessionsPage = workspace.findChild<QWidget *>(QStringLiteral("terminalSessionsPage"));
        auto *recentEmpty = workspace.findChild<QLabel *>(QStringLiteral("recentLoginEmpty"));
        QVERIFY(recentPage);
        QVERIFY(sessionsPage);
        QVERIFY(recentEmpty);
        QVERIFY(recentPage->isVisible());
        QVERIFY(recentEmpty->isVisible());
        QVERIFY(!sessionsPage->isVisible());

        noxshell::ServerProfile first;
        first.id = QStringLiteral("tab-context-first");
        first.name = QStringLiteral("第一台");
        first.connectionMode = noxshell::ConnectionMode::Demo;
        noxshell::ServerProfile second;
        second.id = QStringLiteral("tab-context-second");
        second.name = QStringLiteral("第二台");
        second.connectionMode = noxshell::ConnectionMode::Demo;
        workspace.openOrActivate(first, false);
        workspace.openOrActivate(second, false);
        QVERIFY(!recentPage->isVisible());
        QVERIFY(sessionsPage->isVisible());

        auto *tabs = workspace.findChild<QTabBar *>(QStringLiteral("terminalSessionTabs"));
        auto *connectAction = workspace.findChild<QAction *>(QStringLiteral("terminalConnectAction"));
        auto *disconnectAction = workspace.findChild<QAction *>(QStringLiteral("terminalDisconnectAction"));
        auto *duplicateAction = workspace.findChild<QAction *>(QStringLiteral("terminalDuplicateAction"));
        auto *closeCurrentAction = workspace.findChild<QAction *>(QStringLiteral("terminalCloseCurrentAction"));
        auto *closeOthersAction = workspace.findChild<QAction *>(QStringLiteral("terminalCloseOthersAction"));
        auto *closeAllAction = workspace.findChild<QAction *>(QStringLiteral("terminalCloseAllAction"));
        QVERIFY(tabs);
        QVERIFY(connectAction);
        QVERIFY(disconnectAction);
        QVERIFY(duplicateAction);
        QVERIFY(closeCurrentAction);
        QVERIFY(closeOthersAction);
        QVERIFY(closeAllAction);
        QVERIFY(!workspace.findChild<QPushButton *>(QStringLiteral("duplicateTerminalButton")));
        QCOMPARE(tabs->count(), 2);
        QVERIFY(tabs->isVisible());
        QVERIFY(tabs->height() >= 30);
        QVERIFY(tabs->tabRect(0).width() >= 110);
        QVERIFY(tabs->currentIndex() >= 0);
        QVERIFY(!tabs->tabText(tabs->currentIndex()).isEmpty());
        QVERIFY(!tabs->tabIcon(0).isNull());
        QVERIFY(tabs->tabToolTip(0).contains(QStringLiteral("未连接")));

        tabs->customContextMenuRequested(tabs->tabRect(0).center());
        QVERIFY(connectAction->isEnabled());
        QVERIFY(!disconnectAction->isEnabled());
        connectAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(tabs->tabToolTip(0).contains(QStringLiteral("连接成功")), 1000);
        tabs->customContextMenuRequested(tabs->tabRect(0).center());
        QVERIFY(!connectAction->isEnabled());
        QVERIFY(disconnectAction->isEnabled());
        disconnectAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(tabs->tabToolTip(0).contains(QStringLiteral("未连接")), 1000);

        tabs->customContextMenuRequested(tabs->tabRect(0).center());
        duplicateAction->trigger();
        QCOMPARE(tabs->count(), 3);

        tabs->customContextMenuRequested(tabs->tabRect(1).center());
        closeOthersAction->trigger();
        QCOMPARE(tabs->count(), 1);
        QCOMPARE(tabs->tabText(0), QStringLiteral("第二台"));

        tabs->customContextMenuRequested(tabs->tabRect(0).center());
        duplicateAction->trigger();
        QCOMPARE(tabs->count(), 2);
        tabs->customContextMenuRequested(tabs->tabRect(1).center());
        closeCurrentAction->trigger();
        QCOMPARE(tabs->count(), 1);

        tabs->customContextMenuRequested(tabs->tabRect(0).center());
        duplicateAction->trigger();
        QCOMPARE(tabs->count(), 2);
        tabs->customContextMenuRequested(tabs->tabRect(0).center());
        closeAllAction->trigger();
        QCOMPARE(tabs->count(), 0);
        QVERIFY(recentPage->isVisible());
        QVERIFY(!sessionsPage->isVisible());
    }

    void editingServerKeepsOldConnectionSnapshotAndNextConnectUsesNewProfile()
    {
        noxshell::ui::TerminalWorkspace workspace(nullptr, nullptr);
        workspace.resize(900, 500);
        workspace.show();

        noxshell::ServerProfile original;
        original.id = QStringLiteral("edited-live-host");
        original.name = QStringLiteral("旧会话");
        original.host = QStringLiteral("192.0.2.10");
        original.port = 22;
        original.user = QStringLiteral("root");
        original.connectionMode = noxshell::ConnectionMode::Demo;

        workspace.openOrActivate(original, true);
        auto *tabs = workspace.findChild<QTabBar *>(QStringLiteral("terminalSessionTabs"));
        QVERIFY(tabs);
        QTRY_COMPARE_WITH_TIMEOUT(workspace.sessionCount(), 1, 1000);
        auto sessions = workspace.findChildren<noxshell::SshSession *>();
        QCOMPARE(sessions.size(), 1);
        auto *oldSession = sessions.first();
        QTRY_VERIFY_WITH_TIMEOUT(oldSession->isConnected(), 1000);
        QCOMPARE(oldSession->profile().host, original.host);
        QSignalSpy oldConnectionSpy(oldSession, &noxshell::SshSession::connectionChanged);

        auto edited = original;
        edited.name = QStringLiteral("新配置");
        edited.host = QStringLiteral("198.51.100.25");
        edited.port = 2224;
        workspace.updateServer(edited);

        QCOMPARE(workspace.sessionCount(), 1);
        QCOMPARE(tabs->tabText(0), original.name);
        QCOMPARE(oldSession->profile().host, original.host);
        QCOMPARE(oldSession->profile().port, original.port);
        QVERIFY(oldSession->isConnected());
        QCOMPARE(oldConnectionSpy.count(), 0);

        workspace.openOrActivate(edited, true);
        QTRY_COMPARE_WITH_TIMEOUT(workspace.sessionCount(), 2, 1000);
        QCOMPARE(tabs->tabText(0), original.name);
        QCOMPARE(tabs->tabText(1), edited.name);
        sessions = workspace.findChildren<noxshell::SshSession *>();
        QCOMPARE(sessions.size(), 2);
        const auto newSession = std::find_if(sessions.cbegin(), sessions.cend(), [&edited](const auto *session) {
            return session->profile().host == edited.host && session->profile().port == edited.port;
        });
        QVERIFY(newSession != sessions.cend());
        QTRY_VERIFY_WITH_TIMEOUT((*newSession)->isConnected(), 1000);
        QVERIFY(oldSession->isConnected());
        QCOMPARE(oldConnectionSpy.count(), 0);

        // Further Connect actions activate the current configuration instead
        // of creating another tab or reconnecting the preserved old channel.
        workspace.openOrActivate(edited, true);
        QCOMPARE(workspace.sessionCount(), 2);
        QCOMPARE(oldConnectionSpy.count(), 0);
    }

    void vtTerminalKeepsBoundedScrollbackAndMouseModes()
    {
        noxshell::ui::VtTerminalModel model(16, 3);
        for (int line = 0; line < 5010; ++line) {
            model.feed(QStringLiteral("line-%1\r\n").arg(line, 4, 10, QLatin1Char('0')));
        }
        QCOMPARE(model.historyLineCount(), 5000);
        QVERIFY(model.plainText().contains(QStringLiteral("line-5009")));
        QVERIFY(!model.plainText().contains(QStringLiteral("line-0000")));

        model.feed(QStringLiteral("\x1b[?1002h\x1b[?1006h"));
        QCOMPARE(model.mouseTracking(), noxshell::ui::VtTerminalModel::MouseTracking::ButtonMotion);
        QVERIFY(model.sgrMouseEncoding());
        model.feed(QStringLiteral("\x1b[?1002l\x1b[?1006l"));
        QCOMPARE(model.mouseTracking(), noxshell::ui::VtTerminalModel::MouseTracking::None);
        QVERIFY(!model.sgrMouseEncoding());

        const int historyBeforeAlternate = model.historyLineCount();
        model.feed(QStringLiteral("\x1b[?1049h"));
        for (int line = 0; line < 20; ++line) model.feed(QStringLiteral("alt\r\n"));
        QCOMPARE(model.historyLineCount(), historyBeforeAlternate);
        model.feed(QStringLiteral("\x1b[?1049l"));
    }

    void terminalViewSelectsTextScrollsAndReportsMouse()
    {
        noxshell::ui::TerminalView view;
        view.resize(480, 180);
        view.show();
        view.feedText(QStringLiteral("alpha beta gamma\r\nsecond line"));
        QTest::qWait(20);

        const QFontMetrics metrics(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        const int cellWidth = qCeil(metrics.horizontalAdvance(QLatin1Char('M')));
        const int cellHeight = qCeil(metrics.height() + 1.0);
        const QPoint start(cellWidth / 2, cellHeight / 2);
        const QPoint end(cellWidth * 5 + cellWidth / 2, cellHeight / 2);
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(&view, end);
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, end);
        QCOMPARE(view.selectedText(), QStringLiteral("alpha"));

        QSignalSpy inputSpy(&view, &noxshell::ui::TerminalView::inputGenerated);
        QApplication::clipboard()->clear();
        view.setFocus();
        QTest::keyClick(&view, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("alpha"));
        QCOMPARE(inputSpy.count(), 0);

        QContextMenuEvent contextEvent(QContextMenuEvent::Mouse, end, view.mapToGlobal(end));
        QApplication::sendEvent(&view, &contextEvent);
        auto *copyAction = view.findChild<QAction *>(QStringLiteral("terminalCopyAction"));
        auto *pasteAction = view.findChild<QAction *>(QStringLiteral("terminalPasteAction"));
        auto *selectAllAction = view.findChild<QAction *>(QStringLiteral("terminalSelectAllAction"));
        QVERIFY(copyAction);
        QVERIFY(pasteAction);
        QVERIFY(selectAllAction);
        QVERIFY(copyAction->isEnabled());
        copyAction->trigger();
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("alpha"));

        QApplication::clipboard()->setText(QStringLiteral("echo pasted"));
        QVERIFY(pasteAction->isEnabled());
        pasteAction->trigger();
        QCOMPARE(inputSpy.count(), 1);
        QCOMPARE(inputSpy.last().at(0).toByteArray(), QByteArray("echo pasted"));
        QVERIFY(view.selectedText().isEmpty());

        QApplication::clipboard()->setText(QStringLiteral("ctrl-v pasted"));
        QTest::keyClick(&view, Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(inputSpy.count(), 2);
        QCOMPARE(inputSpy.last().at(0).toByteArray(), QByteArray("ctrl-v pasted"));

        view.feedText(QStringLiteral("\x1b[?1000h\x1b[?1006h"));
        const int mouseReportStart = inputSpy.count();
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, start);
        QTRY_VERIFY_WITH_TIMEOUT(inputSpy.count() >= mouseReportStart + 2, 1000);
        QVERIFY(inputSpy.at(mouseReportStart).at(0).toByteArray().startsWith(QByteArray("\x1b[<0;1;1M")));
        QVERIFY(inputSpy.last().at(0).toByteArray().endsWith('m'));

        view.feedText(QStringLiteral("\x1b[?1000l"));
        for (int line = 0; line < 30; ++line) view.feedText(QStringLiteral("\r\nrow"));
        auto *scrollBar = view.findChild<QScrollBar *>(QStringLiteral("terminalScrollBar"));
        QVERIFY(scrollBar);
        QVERIFY(scrollBar->maximum() > 0);
    }

    void metricHistoryKeepsNewestPointsWithinCapacityAndWindow()
    {
        noxshell::MetricHistory history(3);
        const auto start = QDateTime::fromString(QStringLiteral("2026-08-13T10:00:00"), Qt::ISODate);
        for (int index = 0; index < 5; ++index) {
            noxshell::MetricSample sample;
            sample.capturedAt = start.addSecs(index * 30);
            sample.cpuReady = index > 0;
            sample.cpuPercent = 20.0 + index;
            sample.memoryPercent = 40.0 + index;
            sample.load1 = 2.0;
            sample.cpuCoreCount = 4;
            sample.primaryDisk.usagePercent = 60 + index;
            history.append(sample);
        }

        QCOMPARE(history.capacity(), qsizetype{3});
        QCOMPARE(history.size(), qsizetype{3});
        const auto all = history.points(3600);
        QCOMPARE(all.size(), 3);
        QCOMPARE(all.first().capturedAt, start.addSecs(60));
        QCOMPARE(all.last().capturedAt, start.addSecs(120));
        QCOMPARE(all.last().loadPercent, 50.0);
        const auto lastMinute = history.points(60);
        QCOMPARE(lastMinute.size(), 3);
        const auto lastThirtySeconds = history.points(30);
        QCOMPARE(lastThirtySeconds.size(), 2);
        history.clear();
        QVERIFY(history.isEmpty());
    }

    void parsesLinuxMetricsAndCalculatesCpuDelta()
    {
        const QByteArray firstPayload =
            "__CPU__\n"
            "cpu 100 0 50 800 10 0 0 0\n"
            "__MEM__\n"
            "MemTotal:       1048576 kB\n"
            "MemAvailable:    419430 kB\n"
            "__LOAD__\n"
            "1.25 0.90 0.75 1/100 123\n"
            "__CORES__\n"
            "4\n"
            "__DISK__\n"
            "Filesystem 1024-blocks Used Available Capacity Mounted on\n"
            "/dev/vda1 104857600 76546048 28311552 73% /\n";
        const QByteArray secondPayload =
            "__CPU__\n"
            "cpu 150 0 70 850 10 0 0 0\n"
            "__MEM__\n"
            "MemTotal:       1048576 kB\n"
            "MemAvailable:    419430 kB\n"
            "__LOAD__\n"
            "1.50 1.00 0.80 1/100 456\n"
            "__CORES__\n"
            "4\n"
            "__DISK__\n"
            "Filesystem 1024-blocks Used Available Capacity Mounted on\n"
            "/dev/vda1 104857600 76546048 28311552 73% /\n";

        noxshell::LinuxMetricsSnapshot first;
        noxshell::LinuxMetricsSnapshot second;
        QString error;
        QVERIFY2(noxshell::LinuxMetricsParser::parse(firstPayload, first, &error), qPrintable(error));
        QVERIFY2(noxshell::LinuxMetricsParser::parse(secondPayload, second, &error), qPrintable(error));

        const auto baseline = noxshell::LinuxMetricsParser::calculate(first);
        QVERIFY(!baseline.cpuReady);
        const auto sample = noxshell::LinuxMetricsParser::calculate(second, &first);
        QVERIFY(sample.cpuReady);
        QVERIFY(qAbs(sample.cpuPercent - 58.333) < 0.01);
        QVERIFY(qAbs(sample.kernelPercent - 16.666) < 0.01);
        QVERIFY(qAbs(sample.memoryPercent - 60.0) < 0.01);
        QCOMPARE(sample.cpuCoreCount, 4);
        QCOMPARE(sample.primaryDisk.fileSystem, QStringLiteral("/dev/vda1"));
        QCOMPARE(sample.primaryDisk.usagePercent, 73);
        QCOMPARE(sample.primaryDisk.totalBytes, quint64{104857600} * 1024);
    }

    void parsesLegacyMemoryAvailabilityFallback()
    {
        const QByteArray payload =
            "__CPU__\ncpu 1 2 3 4 5 6 7 8\n"
            "__MEM__\nMemTotal: 1000 kB\nMemFree: 100 kB\nBuffers: 50 kB\nCached: 200 kB\nSReclaimable: 20 kB\nShmem: 10 kB\n"
            "__LOAD__\n0.10 0.20 0.30 1/1 1\n"
            "__CORES__\n2\n"
            "__DISK__\n";
        noxshell::LinuxMetricsSnapshot snapshot;
        QString error;
        QVERIFY2(noxshell::LinuxMetricsParser::parse(payload, snapshot, &error), qPrintable(error));
        QCOMPARE(snapshot.memoryAvailableBytes, quint64{360} * 1024);
        const auto sample = noxshell::LinuxMetricsParser::calculate(snapshot);
        QVERIFY(qAbs(sample.memoryPercent - 64.0) < 0.01);
    }

    void repositoryPersistsServersAndKnownHosts()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto databasePath = directory.filePath(QStringLiteral("test.sqlite3"));

        QString serverId;
        QDateTime newerLogin;
        {
            noxshell::ServerRepository repository(databasePath, false);
            QVERIFY2(repository.initialize(), qPrintable(repository.lastError()));
            noxshell::ServerProfile profile;
            profile.name = QStringLiteral("persisted-host");
            profile.host = QStringLiteral("192.0.2.10");
            profile.user = QStringLiteral("ops");
            profile.os = QStringLiteral("linux");
            profile.group = QStringLiteral("test");
            profile.connectionMode = noxshell::ConnectionMode::Ssh;
            profile.authentication = noxshell::AuthenticationMethod::PrivateKey;
            profile.privateKeyPath = QStringLiteral("~/.ssh/id_ed25519");
            profile.credentialRef = QStringLiteral("server/test-reference");
            QVERIFY2(repository.saveServer(profile), qPrintable(repository.lastError()));
            serverId = profile.id;
            QVERIFY(repository.saveKnownHost(profile.host, 22, QStringLiteral("ED25519"), QStringLiteral("SHA256:test-fingerprint")));
            QVERIFY(repository.saveTerminalState({profile.id, profile.id}, 1));
            const auto olderLogin = QDateTime::currentDateTime().addSecs(-120);
            newerLogin = QDateTime::currentDateTime().addSecs(-30);
            QVERIFY(repository.recordSuccessfulLogin(profile.id, olderLogin));
            QVERIFY(repository.recordSuccessfulLogin(profile.id, newerLogin));
            QVERIFY(repository.recordSuccessfulLogin(profile.id, olderLogin));
            noxshell::MonitoringThresholds thresholds{70.0, 71.0, 72.0, 73.0};
            QVERIFY(repository.saveMonitoringThresholds(profile.id, thresholds));
            noxshell::MetricSample metric;
            metric.capturedAt = QDateTime::currentDateTime().addSecs(-5);
            metric.cpuReady = true;
            metric.cpuPercent = 42.0;
            metric.memoryPercent = 51.0;
            metric.load1 = 1.0;
            metric.cpuCoreCount = 2;
            metric.primaryDisk.totalBytes = 100;
            metric.primaryDisk.usagePercent = 64;
            QVERIFY(repository.saveMetricSample(profile.id, metric));
            noxshell::MonitoringAlert alert{0, profile.id, QStringLiteral("CPU"), 90.0, 70.0, QDateTime::currentDateTime()};
            QVERIFY(repository.recordMonitoringAlert(alert));
            noxshell::FileTransferTask transfer;
            transfer.id = 7;
            transfer.localPath = QStringLiteral("/tmp/local.bin");
            transfer.remotePath = QStringLiteral("/tmp/remote.bin");
            transfer.completed = 10;
            transfer.total = 100;
            transfer.state = noxshell::TransferState::Failed;
            transfer.message = QStringLiteral("network error");
            QVERIFY(repository.saveTransferTask(profile.id, transfer));
            profile.name = QStringLiteral("persisted-host-edited");
            profile.port = 2222;
            QVERIFY2(repository.saveServer(profile), qPrintable(repository.lastError()));
        }
        {
            noxshell::ServerRepository repository(databasePath, false);
            QVERIFY2(repository.initialize(), qPrintable(repository.lastError()));
            const auto servers = repository.loadServers();
            QCOMPARE(servers.size(), 1);
            QCOMPARE(servers.first().id, serverId);
            QCOMPARE(servers.first().name, QStringLiteral("persisted-host-edited"));
            QCOMPARE(servers.first().port, static_cast<quint16>(2222));
            QCOMPARE(servers.first().credentialRef, QStringLiteral("server/test-reference"));
            QVERIFY(servers.first().password.isEmpty());
            QCOMPARE(repository.knownHostFingerprint(QStringLiteral("192.0.2.10"), 22), QStringLiteral("SHA256:test-fingerprint"));
            const auto terminalState = repository.loadTerminalState();
            QCOMPARE(terminalState.serverIds, QStringList({serverId, serverId}));
            QCOMPARE(terminalState.currentIndex, 1);
            const auto recentLogins = repository.loadRecentLogins();
            QCOMPARE(recentLogins.size(), 1);
            QCOMPARE(recentLogins.at(0).serverId, serverId);
            QCOMPARE(recentLogins.at(0).serverName, QStringLiteral("persisted-host-edited"));
            QCOMPARE(recentLogins.at(0).connectedAt.toUTC(), newerLogin.toUTC());
            const auto thresholds = repository.loadMonitoringThresholds(serverId);
            QCOMPARE(thresholds.cpuPercent, 70.0);
            QCOMPARE(thresholds.diskPercent, 73.0);
            const auto metrics = repository.loadMetricHistory(serverId, QDateTime::currentDateTime().addSecs(-60));
            QCOMPARE(metrics.size(), 1);
            QCOMPARE(metrics.first().cpuPercent, 42.0);
            QCOMPARE(metrics.first().diskPercent, 64.0);
            const auto alerts = repository.loadMonitoringAlerts(serverId);
            QCOMPARE(alerts.size(), 1);
            QCOMPARE(alerts.first().metric, QStringLiteral("CPU"));
            const auto transfers = repository.loadTransferTasks(serverId);
            QCOMPARE(transfers.size(), 1);
            QCOMPARE(transfers.first().id, quint64{7});
            QCOMPARE(transfers.first().state, noxshell::TransferState::Failed);
            QCOMPARE(transfers.first().message, QStringLiteral("network error"));
            QVERIFY2(repository.deleteServer(serverId), qPrintable(repository.lastError()));
            QVERIFY(repository.loadServers().isEmpty());
            QVERIFY(repository.loadRecentLogins().isEmpty());
            QVERIFY(!repository.deleteServer(serverId));
        }
    }

    void loginHistoryMigrationKeepsOnlyLatestRecordPerServer()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto databasePath = directory.filePath(QStringLiteral("legacy-history.sqlite3"));
        const auto seedConnection = QStringLiteral("legacy-history-seed");
        {
            auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), seedConnection);
            database.setDatabaseName(databasePath);
            QVERIFY(database.open());
            QSqlQuery query(database);
            QVERIFY(query.exec(QStringLiteral(
                "CREATE TABLE login_history (id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "server_id TEXT NOT NULL,connected_at TEXT NOT NULL)")));
            const auto insertLogin = [&](const QString &serverId, const QString &connectedAt) {
                QSqlQuery insert(database);
                insert.prepare(QStringLiteral("INSERT INTO login_history(server_id,connected_at) VALUES(?,?)"));
                insert.addBindValue(serverId);
                insert.addBindValue(connectedAt);
                return insert.exec();
            };
            QVERIFY(insertLogin(QStringLiteral("server-a"), QStringLiteral("2026-08-15T10:00:00.000Z")));
            QVERIFY(insertLogin(QStringLiteral("server-a"), QStringLiteral("2026-08-15T12:00:00.000Z")));
            QVERIFY(insertLogin(QStringLiteral("server-a"), QStringLiteral("2026-08-15T11:00:00.000Z")));
            QVERIFY(insertLogin(QStringLiteral("server-b"), QStringLiteral("2026-08-15T09:00:00.000Z")));
            database.close();
        }
        QSqlDatabase::removeDatabase(seedConnection);

        {
            noxshell::ServerRepository repository(databasePath, false);
            QVERIFY2(repository.initialize(), qPrintable(repository.lastError()));
        }

        const auto verifyConnection = QStringLiteral("legacy-history-verify");
        {
            auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), verifyConnection);
            database.setDatabaseName(databasePath);
            QVERIFY(database.open());
            QSqlQuery query(database);
            QVERIFY(query.exec(QStringLiteral(
                "SELECT server_id,connected_at FROM login_history ORDER BY server_id")));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toString(), QStringLiteral("server-a"));
            QCOMPARE(query.value(1).toString(), QStringLiteral("2026-08-15T12:00:00.000Z"));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toString(), QStringLiteral("server-b"));
            QVERIFY(!query.next());

            QSqlQuery duplicate(database);
            duplicate.prepare(QStringLiteral("INSERT INTO login_history(server_id,connected_at) VALUES(?,?)"));
            duplicate.addBindValue(QStringLiteral("server-a"));
            duplicate.addBindValue(QStringLiteral("2026-08-15T13:00:00.000Z"));
            QVERIFY(!duplicate.exec());
            database.close();
        }
        QSqlDatabase::removeDatabase(verifyConnection);
    }

    void realSshFailureIsReportedWithoutBlockingUi()
    {
        noxshell::SshSession session(nullptr, nullptr);
        QSignalSpy statusSpy(&session, &noxshell::SshSession::connectionChanged);
        QSignalSpy outputSpy(&session, &noxshell::SshSession::outputReceived);

        noxshell::ServerProfile profile;
        profile.name = QStringLiteral("unreachable-test");
        profile.host = QStringLiteral("127.0.0.1");
        profile.port = 1;
        profile.user = QStringLiteral("nobody");
        profile.connectionMode = noxshell::ConnectionMode::Ssh;
        profile.authentication = noxshell::AuthenticationMethod::Password;
        profile.password = QStringLiteral("not-a-real-password");

        session.connectTo(profile);
        QTRY_VERIFY_WITH_TIMEOUT(!outputSpy.isEmpty(), 3000);
        const auto output = outputSpy.last().at(0).toString();
        QVERIFY(output.contains(QStringLiteral("TCP失败")) || output.contains(QStringLiteral("SSH 握手失败")));
        QVERIFY(!session.isConnected());
        QVERIFY(statusSpy.count() >= 2);
    }

    void configuredRealSshHandshakeReachesAuthentication()
    {
        const auto endpoint = qEnvironmentVariable("NOXSHELL_SSH_TEST_ENDPOINT");
        if (endpoint.isEmpty()) QSKIP("NOXSHELL_SSH_TEST_ENDPOINT 未配置，跳过真实 SSH 握手测试");
        const auto separator = endpoint.lastIndexOf(QLatin1Char(':'));
        QVERIFY2(separator > 0, "测试地址格式必须为 host:port");
        bool portValid = false;
        const auto port = endpoint.mid(separator + 1).toUShort(&portValid);
        QVERIFY(portValid);

        noxshell::SshSession session(nullptr, nullptr);
        QSignalSpy statusSpy(&session, &noxshell::SshSession::connectionChanged);
        connect(&session, &noxshell::SshSession::hostKeyVerificationRequired, &session,
            [&session](const QString &, const QString &, const QString &) { session.approveHostKey(true); });
        noxshell::ServerProfile profile;
        profile.id = QStringLiteral("configured-real-handshake");
        profile.name = QStringLiteral("configured-real-handshake");
        profile.host = endpoint.left(separator);
        profile.port = port;
        profile.user = QStringLiteral("root");
        profile.connectionMode = noxshell::ConnectionMode::Ssh;
        profile.authentication = noxshell::AuthenticationMethod::Password;
        session.connectTo(profile);

        QTRY_VERIFY_WITH_TIMEOUT([&] {
            for (const auto &arguments : statusSpy) {
                const auto message = arguments.at(1).toString();
                if (message.contains(QStringLiteral("SSH 认证失败"))) return true;
                if (message.contains(QStringLiteral("SSH 握手失败"))) return true;
            }
            return false;
        }(), 12000);
        const auto finalMessage = statusSpy.last().at(1).toString();
        QVERIFY2(finalMessage.contains(QStringLiteral("SSH 认证失败")), qPrintable(finalMessage));
    }

    void demoSessionProducesMetricSamples()
    {
        noxshell::SshSession session(nullptr, nullptr);
        QSignalSpy metricSpy(&session, &noxshell::SshSession::metricSampleReceived);

        noxshell::ServerProfile profile;
        profile.name = QStringLiteral("demo-metrics");
        profile.host = QStringLiteral("10.0.0.11");
        profile.user = QStringLiteral("root");
        profile.connectionMode = noxshell::ConnectionMode::Demo;
        session.connectTo(profile);
        QTRY_VERIFY_WITH_TIMEOUT(session.isConnected(), 1000);
        session.requestMetrics();
        QTRY_COMPARE_WITH_TIMEOUT(metricSpy.count(), 1, 1000);

        const auto sample = qvariant_cast<noxshell::MetricSample>(metricSpy.first().at(0));
        QVERIFY(sample.cpuReady);
        QVERIFY(sample.cpuPercent > 0.0);
        QCOMPARE(sample.cpuCoreCount, 4);
        QCOMPARE(sample.primaryDisk.mountPoint, QStringLiteral("/"));
    }

    void demoRemoteFileCanBeCreatedEditedAndRead()
    {
        noxshell::SshSession session(nullptr, nullptr);
        noxshell::ServerProfile profile;
        profile.id = QStringLiteral("demo-file-content");
        profile.name = QStringLiteral("demo-file-content");
        profile.user = QStringLiteral("root");
        profile.connectionMode = noxshell::ConnectionMode::Demo;
        session.connectTo(profile);
        QTRY_VERIFY_WITH_TIMEOUT(session.isConnected(), 1000);

        const auto path = QStringLiteral("/var/www/app/created.conf");
        QSignalSpy writeSpy(&session, &noxshell::SshSession::remoteFileWritten);
        QSignalSpy writeFailureSpy(&session, &noxshell::SshSession::remoteFileWriteFailed);
        QSignalSpy readSpy(&session, &noxshell::SshSession::remoteFileRead);

        const auto createId = session.writeFile(path, QByteArray("enabled=true\n"), false);
        QTRY_COMPARE_WITH_TIMEOUT(writeSpy.count(), 1, 1000);
        QCOMPARE(writeSpy.last().at(0).toULongLong(), createId);
        QCOMPARE(writeSpy.last().at(1).toString(), path);

        session.readFile(path);
        QTRY_COMPARE_WITH_TIMEOUT(readSpy.count(), 1, 1000);
        QCOMPARE(readSpy.last().at(2).toByteArray(), QByteArray("enabled=true\n"));

        session.writeFile(path, QByteArray("must-not-overwrite"), false);
        QTRY_COMPARE_WITH_TIMEOUT(writeFailureSpy.count(), 1, 1000);
        QVERIFY(writeFailureSpy.last().at(2).toString().contains(QStringLiteral("已存在")));

        session.writeFile(path, QByteArray("enabled=false\n"), true);
        QTRY_COMPARE_WITH_TIMEOUT(writeSpy.count(), 2, 1000);
        session.readFile(path);
        QTRY_COMPARE_WITH_TIMEOUT(readSpy.count(), 2, 1000);
        QCOMPARE(readSpy.last().at(2).toByteArray(), QByteArray("enabled=false\n"));
    }

    void demoSftpListsAndNavigatesDirectories()
    {
        noxshell::SshSession session(nullptr, nullptr);
        QSignalSpy failureSpy(&session, &noxshell::SshSession::directoryListingFailed);
        QSignalSpy operationSpy(&session, &noxshell::SshSession::fileOperationFinished);
        session.listDirectory(QStringLiteral("/var/www/app"));
        QCOMPARE(failureSpy.count(), 1);
        QCOMPARE(failureSpy.first().at(0).toString(), QStringLiteral("/var/www/app"));
        QVERIFY(failureSpy.first().at(1).toString().contains(QStringLiteral("未连接")));

        noxshell::ui::FilePanel panel(&session);
        panel.resize(1000, 700);
        panel.show();

        noxshell::ServerProfile profile;
        profile.id = QStringLiteral("demo-sftp");
        profile.name = QStringLiteral("demo-sftp");
        profile.host = QStringLiteral("10.0.0.11");
        profile.user = QStringLiteral("root");
        profile.connectionMode = noxshell::ConnectionMode::Demo;
        panel.setServer(profile);
        session.connectTo(profile);

        auto *tree = panel.findChild<QTreeWidget *>(QStringLiteral("remoteFileTree"));
        auto *directoryTree = panel.findChild<QTreeWidget *>(QStringLiteral("remoteDirectoryTree"));
        auto *browserSplitter = panel.findChild<QSplitter *>(QStringLiteral("fileBrowserSplitter"));
        auto *pathEdit = panel.findChild<QLineEdit *>(QStringLiteral("remotePathEdit"));
        auto *backButton = panel.findChild<QPushButton *>(QStringLiteral("fileBackButton"));
        auto *contextDownload = panel.findChild<QAction *>(QStringLiteral("fileContextDownloadAction"));
        auto *newFileAction = panel.findChild<QAction *>(QStringLiteral("fileNewFileAction"));
        auto *newDirectoryAction = panel.findChild<QAction *>(QStringLiteral("fileNewDirectoryAction"));
        auto *queueButton = panel.findChild<QToolButton *>(QStringLiteral("transferQueueButton"));
        auto *queueMenu = panel.findChild<QMenu *>(QStringLiteral("transferQueueMenu"));
        auto *transferPanel = panel.findChild<noxshell::ui::TransferQueuePanel *>(QStringLiteral("transferQueuePanel"));
        auto *fileToolbar = panel.findChild<QWidget *>(QStringLiteral("fileToolbar"));
        auto *fileStatus = panel.findChild<QLabel *>(QStringLiteral("fileStatusLabel"));
        QVERIFY(tree);
        QVERIFY(directoryTree);
        QVERIFY(browserSplitter);
        QVERIFY(pathEdit);
        QVERIFY(backButton);
        QVERIFY(contextDownload);
        QVERIFY(newFileAction);
        QVERIFY(newDirectoryAction);
        QVERIFY(queueButton);
        QVERIFY(queueMenu);
        QVERIFY(transferPanel);
        QVERIFY(fileToolbar);
        QVERIFY(fileStatus);
        QVERIFY(!queueButton->icon().isNull());
        QCOMPARE(fileStatus->parentWidget(), fileToolbar);
        QCOMPARE(pathEdit->parentWidget(), fileToolbar);
        QVERIFY(!panel.findChild<QPushButton *>(QStringLiteral("fileUploadButton")));
        QVERIFY(!panel.findChild<QPushButton *>(QStringLiteral("fileDownloadButton")));
        QVERIFY(!panel.findChild<QPushButton *>(QStringLiteral("fileNewDirectoryButton")));
        QCOMPARE(newDirectoryAction->text(), QStringLiteral("新建目录"));
        QVERIFY(panel.layout()->indexOf(transferPanel) < 0);
        QVERIFY(!transferPanel->isVisible());
        queueMenu->popup(queueButton->mapToGlobal(QPoint(0, queueButton->height())));
        QTRY_VERIFY_WITH_TIMEOUT(queueMenu->isVisible(), 1000);
        QVERIFY(transferPanel->isVisible());
        queueMenu->hide();
        QCOMPARE(browserSplitter->orientation(), Qt::Horizontal);
        QCOMPARE(browserSplitter->count(), 2);
        QCOMPARE(tree->selectionMode(), QAbstractItemView::ExtendedSelection);
        QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 8, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(directoryTree->topLevelItemCount(), 1, 1000);
        QCOMPARE(directoryTree->topLevelItem(0)->text(0), QStringLiteral("/"));
        QTRY_VERIFY_WITH_TIMEOUT(directoryTree->currentItem() != nullptr, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(directoryTree->currentItem()->toolTip(0), QStringLiteral("/var/www/app"), 1000);
        QCOMPARE(pathEdit->text(), QStringLiteral("/var/www/app"));
        QCOMPARE(tree->columnCount(), 6);
        QCOMPARE(tree->headerItem()->text(0), QStringLiteral("文件名"));
        QCOMPARE(tree->headerItem()->text(4), QStringLiteral("权限"));
        QCOMPARE(tree->headerItem()->text(5), QStringLiteral("用户/用户组"));
        QCOMPARE(tree->topLevelItem(0)->text(2), QStringLiteral("文件夹"));
        QCOMPARE(tree->topLevelItem(0)->text(4), QStringLiteral("drwxr-xr-x"));
        QCOMPARE(tree->topLevelItem(0)->text(5), QStringLiteral("root/root"));
        QVERIFY(!tree->topLevelItem(0)->icon(0).isNull());

        QTreeWidgetItem *firstFile = nullptr;
        QTreeWidgetItem *secondFile = nullptr;
        for (int row = 0; row < tree->topLevelItemCount(); ++row) {
            auto *item = tree->topLevelItem(row);
            if (item->text(2) != QStringLiteral("文件")) continue;
            if (!firstFile) firstFile = item;
            else if (!secondFile) secondFile = item;
        }
        QVERIFY(firstFile);
        QVERIFY(secondFile);
        const auto firstFileName = firstFile->text(0);
        const auto secondFileName = secondFile->text(0);
        tree->clearSelection();
        firstFile->setSelected(true);
        secondFile->setSelected(true);
        QCOMPARE(tree->selectedItems().size(), 2);
        QVERIFY(contextDownload->isEnabled());
        QVERIFY(newFileAction->isEnabled());

        tree->itemDoubleClicked(firstFile, 0);
        QTRY_VERIFY_WITH_TIMEOUT(panel.findChild<noxshell::ui::RemoteFileEditor *>() != nullptr, 1000);
        auto *fileEditor = panel.findChild<noxshell::ui::RemoteFileEditor *>();
        auto *editorText = fileEditor->findChild<QPlainTextEdit *>(QStringLiteral("remoteFileEditorText"));
        auto *editorStatus = fileEditor->findChild<QLabel *>(QStringLiteral("remoteFileEditorStatus"));
        auto *editorTabs = fileEditor->findChild<QTabBar *>(QStringLiteral("remoteFileEditorTabs"));
        auto *lineNumbers = fileEditor->findChild<QWidget *>(QStringLiteral("remoteFileEditorLineNumbers"));
        auto *findPanel = fileEditor->findChild<QWidget *>(QStringLiteral("remoteFileFindPanel"));
        auto *findEdit = fileEditor->findChild<QLineEdit *>(QStringLiteral("remoteFileFindEdit"));
        auto *replaceEdit = fileEditor->findChild<QLineEdit *>(QStringLiteral("remoteFileReplaceEdit"));
        auto *replaceRow = fileEditor->findChild<QWidget *>(QStringLiteral("remoteFileReplaceRow"));
        auto *findStatus = fileEditor->findChild<QLabel *>(QStringLiteral("remoteFileFindStatus"));
        auto *findNext = fileEditor->findChild<QToolButton *>(QStringLiteral("remoteFileFindNext"));
        auto *replaceToggle = fileEditor->findChild<QToolButton *>(QStringLiteral("remoteFileReplaceToggle"));
        auto *replaceOne = fileEditor->findChild<QPushButton *>(QStringLiteral("remoteFileReplaceOne"));
        auto *replaceAll = fileEditor->findChild<QPushButton *>(QStringLiteral("remoteFileReplaceAll"));
        auto *findClose = fileEditor->findChild<QToolButton *>(QStringLiteral("remoteFileFindClose"));
        QVERIFY(editorText);
        QVERIFY(editorStatus);
        QVERIFY(editorTabs);
        QVERIFY(lineNumbers);
        QVERIFY(findPanel);
        QVERIFY(findEdit);
        QVERIFY(replaceEdit);
        QVERIFY(replaceRow);
        QVERIFY(findStatus);
        QVERIFY(findNext);
        QVERIFY(replaceToggle);
        QVERIFY(replaceOne);
        QVERIFY(replaceAll);
        QVERIFY(findClose);
        QVERIFY(!fileEditor->findChild<QPushButton *>(QStringLiteral("remoteFileEditorSave")));
        QVERIFY(!fileEditor->findChild<QPushButton *>(QStringLiteral("remoteFileEditorClose")));
        QCOMPARE(editorTabs->count(), 1);
        QVERIFY(editorTabs->tabText(0).contains(QStringLiteral("demo-sftp")));
        QVERIFY(editorTabs->tabText(0).contains(firstFileName));
        QTRY_VERIFY_WITH_TIMEOUT(editorText->isEnabled(), 1000);
        QVERIFY(!editorText->toPlainText().isEmpty());
        QVERIFY(!findPanel->isVisible());

        editorText->setPlainText(QStringLiteral("alpha beta alpha\n"));
        editorText->setFocus();
        QTest::keyClick(editorText, Qt::Key_F, Qt::MetaModifier);
        QTRY_VERIFY_WITH_TIMEOUT(findPanel->isVisible(), 1000);
        QVERIFY(!replaceRow->isVisible());
        QVERIFY(replaceToggle->isVisible());
        QCOMPARE(replaceToggle->text(), QStringLiteral("替换"));
        QTest::mouseClick(replaceToggle, Qt::LeftButton);
        QVERIFY(replaceRow->isVisible());
        QVERIFY(replaceToggle->isChecked());
        QCOMPARE(replaceToggle->text(), QStringLiteral("收起替换"));
        QTest::mouseClick(replaceToggle, Qt::LeftButton);
        QVERIFY(!replaceRow->isVisible());
        findEdit->setText(QStringLiteral("alpha"));
        QTRY_COMPARE_WITH_TIMEOUT(findStatus->text(), QStringLiteral("1 / 2"), 1000);
        QTest::mouseClick(findNext, Qt::LeftButton);
        QCOMPARE(findStatus->text(), QStringLiteral("2 / 2"));
        QTest::keyClick(findEdit, Qt::Key_F, Qt::MetaModifier | Qt::AltModifier);
        QTRY_VERIFY_WITH_TIMEOUT(replaceRow->isVisible(), 1000);
        QVERIFY(replaceToggle->isChecked());
        replaceEdit->setText(QStringLiteral("omega"));
        QTest::mouseClick(replaceOne, Qt::LeftButton);
        QCOMPARE(editorText->toPlainText().count(QStringLiteral("omega")), 1);
        QCOMPARE(editorText->toPlainText().count(QStringLiteral("alpha")), 1);
        QTest::mouseClick(replaceAll, Qt::LeftButton);
        QCOMPARE(editorText->toPlainText().count(QStringLiteral("omega")), 2);
        QCOMPARE(editorText->toPlainText().count(QStringLiteral("alpha")), 0);
        QVERIFY(findStatus->text().contains(QStringLiteral("已替换 1 处")));
        QVERIFY(!editorTabs->tabIcon(0).isNull());
        QTest::mouseClick(findClose, Qt::LeftButton);
        QVERIFY(!findPanel->isVisible());

        editorText->setPlainText(QStringLiteral("edited from standalone editor\n"));
        QVERIFY(!editorTabs->tabIcon(0).isNull());
        editorText->selectAll();
        QTest::keyClick(editorText, Qt::Key_Slash, Qt::ControlModifier);
        QCOMPARE(editorText->toPlainText(), QStringLiteral("# edited from standalone editor\n"));
        QTest::keyClick(editorText, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(editorText->toPlainText(), QStringLiteral("edited from standalone editor\n"));
        QTest::keyClick(editorText, Qt::Key_S, Qt::MetaModifier);
        QTRY_VERIFY_WITH_TIMEOUT(editorStatus->text().contains(QStringLiteral("已保存")), 1000);
        QVERIFY(editorTabs->tabIcon(0).isNull());

        editorText->setPlainText(QStringLiteral("edited with mac control save\n"));
        QTest::keyClick(editorText, Qt::Key_S, Qt::ControlModifier);
        QTRY_VERIFY_WITH_TIMEOUT(editorStatus->text().contains(QStringLiteral("已保存")), 1000);
        QVERIFY(editorTabs->tabIcon(0).isNull());
        const auto editedPath = fileEditor->remotePath();
        QSignalSpy editedReadSpy(&session, &noxshell::SshSession::remoteFileRead);
        session.readFile(editedPath);
        QTRY_COMPARE_WITH_TIMEOUT(editedReadSpy.count(), 1, 1000);
        QCOMPARE(editedReadSpy.last().at(2).toByteArray(), QByteArray("edited with mac control save\n"));

        QTreeWidgetItem *secondFileAfterRefresh = nullptr;
        for (int row = 0; row < tree->topLevelItemCount(); ++row) {
            if (tree->topLevelItem(row)->text(0) == secondFileName) secondFileAfterRefresh = tree->topLevelItem(row);
        }
        QVERIFY(secondFileAfterRefresh);
        tree->itemDoubleClicked(secondFileAfterRefresh, 0);
        QTRY_COMPARE_WITH_TIMEOUT(editorTabs->count(), 2, 1000);
        QCOMPARE(panel.findChildren<noxshell::ui::RemoteFileEditor *>().size(), 1);
        QVERIFY(editorTabs->tabText(1).contains(QStringLiteral("demo-sftp")));
        QVERIFY(editorTabs->tabText(1).contains(secondFileName));
        fileEditor->close();

        QTreeWidgetItem *varDirectory = nullptr;
        for (int row = 0; row < directoryTree->topLevelItem(0)->childCount(); ++row) {
            auto *item = directoryTree->topLevelItem(0)->child(row);
            if (item->text(0) == QStringLiteral("var")) varDirectory = item;
        }
        QVERIFY(varDirectory);
        directoryTree->itemClicked(varDirectory, 0);
        QTRY_COMPARE_WITH_TIMEOUT(pathEdit->text(), QStringLiteral("/var"), 1000);
        QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 3, 1000);

        panel.syncDirectoryFromTerminalCommand(QStringLiteral("cd /var"));
        QCOMPARE(pathEdit->text(), QStringLiteral("/var"));
        panel.syncDirectoryFromTerminalCommand(QStringLiteral("cd www"));
        QCOMPARE(pathEdit->text(), QStringLiteral("/var/www"));
        panel.syncDirectoryFromTerminalCommand(QStringLiteral("cd ~"));
        QTRY_COMPARE_WITH_TIMEOUT(pathEdit->text(), QStringLiteral("/var/www/app"), 1000);
        QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 8, 1000);

        tree->itemDoubleClicked(tree->topLevelItem(0), 0);
        QTRY_COMPARE_WITH_TIMEOUT(pathEdit->text(), QStringLiteral("/var/www/app/app"), 1000);
        QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 2, 1000);
        QVERIFY(backButton->isEnabled());
        QTest::mouseClick(backButton, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(pathEdit->text(), QStringLiteral("/var/www/app"), 1000);
        QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 8, 1000);

        QTemporaryDir uploadDirectory;
        QVERIFY(uploadDirectory.isValid());
        const auto draggedPath = uploadDirectory.filePath(QStringLiteral("dragged.txt"));
        QFile draggedFile(draggedPath);
        QVERIFY(draggedFile.open(QIODevice::WriteOnly));
        QCOMPARE(draggedFile.write("dragged upload"), qint64{14});
        draggedFile.close();
        QMimeData mimeData;
        mimeData.setUrls({QUrl::fromLocalFile(draggedPath)});
        QDragEnterEvent dragEnter(QPoint(10, 10), Qt::CopyAction, &mimeData, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &dragEnter);
        QVERIFY(dragEnter.isAccepted());
        QDropEvent drop(QPointF(tree->viewport()->width() - 4, tree->viewport()->height() - 4),
            Qt::CopyAction, &mimeData, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &drop);
        QVERIFY(drop.isAccepted());
        QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 1, 1000);

        QTimer::singleShot(0, [] {
            auto *dialog = QApplication::activeModalWidget();
            QVERIFY(dialog);
            auto *nameEdit = dialog->findChild<QLineEdit *>();
            QVERIFY(nameEdit);
            nameEdit->setText(QStringLiteral("created-from-menu.txt"));
            QTest::keyClick(nameEdit, Qt::Key_Return);
        });
        newFileAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT([tree] {
            for (int row = 0; row < tree->topLevelItemCount(); ++row) {
                if (tree->topLevelItem(row)->text(0) == QStringLiteral("created-from-menu.txt")) return true;
            }
            return false;
        }(), 1000);
    }

    void filePanelRenamesInlineAndCancelsWithEscape()
    {
        noxshell::SshSession session(nullptr, nullptr);
        QSignalSpy operationSpy(&session, &noxshell::SshSession::fileOperationFinished);
        noxshell::ui::FilePanel panel(&session);
        panel.resize(920, 640);
        panel.show();

        noxshell::ServerProfile profile;
        profile.id = QStringLiteral("demo-inline-rename");
        profile.name = QStringLiteral("demo-inline-rename");
        profile.user = QStringLiteral("root");
        profile.connectionMode = noxshell::ConnectionMode::Demo;
        panel.setServer(profile);
        session.connectTo(profile);

        auto *tree = panel.findChild<QTreeWidget *>(QStringLiteral("remoteFileTree"));
        auto *renameAction = panel.findChild<QAction *>(QStringLiteral("fileRenameAction"));
        QVERIFY(tree);
        QVERIFY(renameAction);
        QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 8, 1000);

        QTreeWidgetItem *target = nullptr;
        for (int row = 0; row < tree->topLevelItemCount(); ++row) {
            if (tree->topLevelItem(row)->text(0) == QStringLiteral("deploy.sh")) target = tree->topLevelItem(row);
        }
        QVERIFY(target);
        tree->setCurrentItem(target);
        renameAction->trigger();
        auto *editor = panel.findChild<QLineEdit *>(QStringLiteral("inlineRenameEditor"));
        QVERIFY(editor);
        QCOMPARE(editor->text(), QStringLiteral("deploy.sh"));
        editor->setText(QStringLiteral("deploy-canceled.sh"));
        QTest::keyClick(editor, Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(panel.findChild<QLineEdit *>(QStringLiteral("inlineRenameEditor")) == nullptr, 1000);
        QCOMPARE(target->text(0), QStringLiteral("deploy.sh"));
        QCOMPARE(operationSpy.count(), 0);

        tree->setCurrentItem(target);
        renameAction->trigger();
        editor = panel.findChild<QLineEdit *>(QStringLiteral("inlineRenameEditor"));
        QVERIFY(editor);
        editor->setText(QStringLiteral("deploy-renamed.sh"));
        QTest::keyClick(editor, Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 1, 1000);
        QTRY_VERIFY_WITH_TIMEOUT([tree] {
            for (int row = 0; row < tree->topLevelItemCount(); ++row) {
                if (tree->topLevelItem(row)->text(0) == QStringLiteral("deploy-renamed.sh")) return true;
            }
            return false;
        }(), 1000);
    }

    void demoSftpOperationsMutateAndTransferFiles()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto uploadSource = directory.filePath(QStringLiteral("release.txt"));
        {
            QFile source(uploadSource);
            QVERIFY(source.open(QIODevice::WriteOnly));
            QCOMPARE(source.write("release payload"), qint64{15});
        }

        noxshell::SshSession session(nullptr, nullptr);
        QSignalSpy operationSpy(&session, &noxshell::SshSession::fileOperationFinished);
        QSignalSpy failureSpy(&session, &noxshell::SshSession::fileOperationFailed);
        QSignalSpy directorySpy(&session, &noxshell::SshSession::directoryListed);
        noxshell::ServerProfile profile;
        profile.id = QStringLiteral("demo-sftp-operations");
        profile.name = QStringLiteral("demo-sftp-operations");
        profile.host = QStringLiteral("10.0.0.12");
        profile.user = QStringLiteral("root");
        profile.connectionMode = noxshell::ConnectionMode::Demo;
        session.connectTo(profile);
        QTRY_VERIFY_WITH_TIMEOUT(session.isConnected(), 1000);

        const QString base = QStringLiteral("/var/www/app");
        const QString created = base + QStringLiteral("/releases");
        const QString renamed = base + QStringLiteral("/archives");
        session.createDirectory(created);
        QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 1, 1000);
        QCOMPARE(qvariant_cast<noxshell::RemoteFileOperation>(operationSpy.last().at(0)), noxshell::RemoteFileOperation::CreateDirectory);

        session.renamePath(created, renamed);
        QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 2, 1000);
        session.removePath(renamed, true);
        QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 3, 1000);

        const auto remoteFile = base + QStringLiteral("/release.txt");
        session.uploadFile(uploadSource, remoteFile);
        QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 4, 1000);
        session.listDirectory(base);
        QTRY_VERIFY_WITH_TIMEOUT(!directorySpy.isEmpty(), 1000);
        const auto entries = qvariant_cast<noxshell::RemoteFileEntries>(directorySpy.last().at(1));
        const auto uploaded = std::find_if(entries.cbegin(), entries.cend(), [&remoteFile](const noxshell::RemoteFileEntry &entry) {
            return entry.path == remoteFile;
        });
        QVERIFY(uploaded != entries.cend());
        QCOMPARE(uploaded->size, quint64{15});

        const auto downloadTarget = directory.filePath(QStringLiteral("downloaded.txt"));
        session.downloadFile(remoteFile, downloadTarget);
        QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 5, 1000);
        QFile downloaded(downloadTarget);
        QVERIFY(downloaded.open(QIODevice::ReadOnly));
        QCOMPARE(downloaded.readAll(), QByteArray("release payload"));

        session.removePath(remoteFile, false);
        QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 6, 1000);
        QVERIFY(failureSpy.isEmpty());

        session.removePath(base + QStringLiteral("/config"), true);
        QTRY_COMPARE_WITH_TIMEOUT(failureSpy.count(), 1, 1000);
        QVERIFY(failureSpy.last().at(2).toString().contains(QStringLiteral("空目录")));
    }

    void demoSftpRejectsRepeatedDeleteOfTheSameEntry()
    {
        noxshell::SshSession session(nullptr, nullptr);
        QSignalSpy operationSpy(&session, &noxshell::SshSession::fileOperationFinished);
        QSignalSpy failureSpy(&session, &noxshell::SshSession::fileOperationFailed);
        QSignalSpy directorySpy(&session, &noxshell::SshSession::directoryListed);
        noxshell::ServerProfile profile;
        profile.id = QStringLiteral("demo-repeat-delete");
        profile.name = QStringLiteral("demo-repeat-delete");
        profile.connectionMode = noxshell::ConnectionMode::Demo;
        session.connectTo(profile);
        QTRY_VERIFY_WITH_TIMEOUT(session.isConnected(), 1000);

        const auto target = QStringLiteral("/README.txt");
        session.removePath(target, false);
        QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 1, 1000);
        session.removePath(target, false);
        QCOMPARE(failureSpy.count(), 1);
        QVERIFY(failureSpy.last().at(2).toString().contains(QStringLiteral("不存在")));

        session.listDirectory(QStringLiteral("/"));
        QTRY_COMPARE_WITH_TIMEOUT(directorySpy.count(), 1, 1000);
        const auto entries = qvariant_cast<noxshell::RemoteFileEntries>(directorySpy.last().at(1));
        QCOMPARE(entries.size(), 3);
        QVERIFY(std::none_of(entries.cbegin(), entries.cend(), [&target](const noxshell::RemoteFileEntry &entry) {
            return entry.path == target;
        }));
    }

    void transferQueueSerializesCancelsAndContinues()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto firstLocal = directory.filePath(QStringLiteral("first.bin"));
        const auto secondLocal = directory.filePath(QStringLiteral("second.bin"));
        for (const auto &path : {firstLocal, secondLocal}) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(QByteArray(4096, 'x')), qint64{4096});
        }

        noxshell::SshSession session(nullptr, nullptr);
        QSignalSpy taskSpy(&session, &noxshell::SshSession::transferTaskChanged);
        noxshell::ServerProfile profile;
        profile.id = QStringLiteral("demo-transfer-queue");
        profile.name = QStringLiteral("demo-transfer-queue");
        profile.connectionMode = noxshell::ConnectionMode::Demo;
        session.connectTo(profile);
        QTRY_VERIFY_WITH_TIMEOUT(session.isConnected(), 1000);

        const auto firstRemote = QStringLiteral("/var/www/app/first.bin");
        const auto secondRemote = QStringLiteral("/var/www/app/second.bin");
        session.uploadFile(firstLocal, firstRemote);
        session.uploadFile(secondLocal, secondRemote);
        QTRY_VERIFY_WITH_TIMEOUT(taskSpy.count() >= 3, 1000);

        quint64 firstId = 0;
        quint64 secondId = 0;
        noxshell::TransferState firstState = noxshell::TransferState::Queued;
        noxshell::TransferState secondState = noxshell::TransferState::Queued;
        for (const auto &arguments : taskSpy) {
            const auto task = qvariant_cast<noxshell::FileTransferTask>(arguments.at(0));
            if (task.remotePath == firstRemote) {
                firstId = task.id;
                firstState = task.state;
            } else if (task.remotePath == secondRemote) {
                secondId = task.id;
                secondState = task.state;
            }
        }
        QVERIFY(firstId != 0);
        QVERIFY(secondId != 0);
        QCOMPARE(firstState, noxshell::TransferState::Running);
        QCOMPARE(secondState, noxshell::TransferState::Queued);

        session.cancelTransfer(secondId);
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            for (auto iterator = taskSpy.crbegin(); iterator != taskSpy.crend(); ++iterator) {
                const auto task = qvariant_cast<noxshell::FileTransferTask>(iterator->at(0));
                if (task.id == secondId) return task.state == noxshell::TransferState::Canceled;
            }
            return false;
        }(), 1000);
        session.retryTransfer(secondId);
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            for (auto iterator = taskSpy.crbegin(); iterator != taskSpy.crend(); ++iterator) {
                const auto task = qvariant_cast<noxshell::FileTransferTask>(iterator->at(0));
                if (task.id == secondId) return task.state == noxshell::TransferState::Completed;
            }
            return false;
        }(), 1000);

        session.uploadFile(firstLocal, firstRemote);
        quint64 retryId = 0;
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            for (auto iterator = taskSpy.crbegin(); iterator != taskSpy.crend(); ++iterator) {
                const auto task = qvariant_cast<noxshell::FileTransferTask>(iterator->at(0));
                if (task.remotePath == firstRemote && task.id != firstId) {
                    retryId = task.id;
                    return task.state == noxshell::TransferState::Running;
                }
            }
            return false;
        }(), 1000);
        session.cancelTransfer(retryId);
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            for (auto iterator = taskSpy.crbegin(); iterator != taskSpy.crend(); ++iterator) {
                const auto task = qvariant_cast<noxshell::FileTransferTask>(iterator->at(0));
                if (task.id == retryId) return task.state == noxshell::TransferState::Canceled;
            }
            return false;
        }(), 1000);

        session.retryTransfer(retryId);
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            for (auto iterator = taskSpy.crbegin(); iterator != taskSpy.crend(); ++iterator) {
                const auto task = qvariant_cast<noxshell::FileTransferTask>(iterator->at(0));
                if (task.id == retryId) return task.state == noxshell::TransferState::Completed;
            }
            return false;
        }(), 1000);
    }

    void mainWindowStartsAndCoreInteractionsWork()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto databasePath = directory.filePath(QStringLiteral("ui-test.sqlite3"));
        {
            noxshell::ServerRepository repository(databasePath, true);
            QVERIFY(repository.initialize());
            const auto servers = repository.loadServers();
            QCOMPARE(servers.size(), 5);
            QVERIFY(repository.saveTerminalState({servers.at(0).id, servers.at(2).id}, 1));
            QVERIFY(repository.recordSuccessfulLogin(servers.at(0).id, QDateTime::currentDateTime().addSecs(-120)));
            QVERIFY(repository.recordSuccessfulLogin(servers.at(2).id, QDateTime::currentDateTime().addSecs(-30)));
        }
        noxshell::ui::MainWindow window(databasePath);
        window.show();
        QTest::qWait(50);
        QVERIFY(window.isVisible());

        auto *hosts = window.findChild<QListWidget *>();
        QVERIFY(hosts);
        QCOMPARE(hosts->count(), 5);
        QTRY_COMPARE_WITH_TIMEOUT(hosts->currentRow(), 0, 1000);

        auto *sidebar = window.findChild<noxshell::ui::HostSidebar *>(QStringLiteral("hostSidebar"));
        auto *sidebarToggle = window.findChild<QToolButton *>(QStringLiteral("sidebarToggleButton"));
        auto *monitorToggle = window.findChild<QToolButton *>(QStringLiteral("monitorToggleButton"));
        auto *windowToolbar = window.findChild<QToolBar *>(QStringLiteral("windowControlsToolbar"));
        QVERIFY(sidebar);
        QVERIFY(sidebarToggle);
        QVERIFY(monitorToggle);
        QVERIFY(windowToolbar);
        QCOMPARE(window.toolBarArea(windowToolbar), Qt::TopToolBarArea);
        QVERIFY(!window.findChild<QWidget *>(QStringLiteral("topBar")));
        QVERIFY(!window.findChild<QLineEdit *>(QStringLiteral("globalSearch")));
        QVERIFY(sidebar->isVisible());
        QVERIFY(!sidebarToggle->icon().isNull());
        QVERIFY(!monitorToggle->icon().isNull());
        QTest::mouseClick(sidebarToggle, Qt::LeftButton);
        QVERIFY(!sidebar->isVisible());
        QVERIFY(sidebarToggle->toolTip().contains(QStringLiteral("显示")));
        QTest::mouseClick(sidebarToggle, Qt::LeftButton);
        QVERIFY(sidebar->isVisible());
        QVERIFY(sidebarToggle->toolTip().contains(QStringLiteral("隐藏")));

        auto *mainSplitter = window.findChild<QSplitter *>(QStringLiteral("mainWorkspaceSplitter"));
        auto *terminalFileSplitter = window.findChild<QSplitter *>(QStringLiteral("terminalFileSplitter"));
        QVERIFY(mainSplitter);
        QVERIFY(terminalFileSplitter);
        QCOMPARE(mainSplitter->orientation(), Qt::Horizontal);
        QCOMPARE(terminalFileSplitter->orientation(), Qt::Vertical);
        QCOMPARE(mainSplitter->count(), 2);
        QCOMPARE(terminalFileSplitter->count(), 2);
        auto *monitorRail = window.findChild<QWidget *>(QStringLiteral("monitorRail"));
        auto *terminalPane = window.findChild<QWidget *>(QStringLiteral("terminalWorkspacePane"));
        auto *filePane = window.findChild<QWidget *>(QStringLiteral("fileWorkspacePane"));
        QVERIFY(monitorRail);
        QVERIFY(monitorRail->isVisible());
        QTest::mouseClick(monitorToggle, Qt::LeftButton);
        QVERIFY(!monitorRail->isVisible());
        QVERIFY(monitorToggle->toolTip().contains(QStringLiteral("显示")));
        QTest::mouseClick(monitorToggle, Qt::LeftButton);
        QVERIFY(monitorRail->isVisible());
        QVERIFY(monitorToggle->toolTip().contains(QStringLiteral("隐藏")));
        QVERIFY(terminalPane);
        QVERIFY(filePane);
        const auto monitorPosition = monitorRail->mapTo(&window, QPoint{});
        const auto terminalPosition = terminalPane->mapTo(&window, QPoint{});
        const auto filePosition = filePane->mapTo(&window, QPoint{});
        QVERIFY(monitorPosition.x() < terminalPosition.x());
        QVERIFY(terminalPosition.y() < filePosition.y());
        QCOMPARE(terminalPosition.x(), filePosition.x());
        QVERIFY(monitorRail->height() > terminalPane->height());

        QVERIFY(!window.findChild<QWidget *>(QStringLiteral("serverHeader")));
        QVERIFY(!window.findChild<QWidget *>(QStringLiteral("terminalHeader")));
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("testConnectionButton")));
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("editServerButton")));
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("deleteServerButton")));

        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("terminalSessionTabs"));
        auto *recentPage = window.findChild<QWidget *>(QStringLiteral("terminalRecentPage"));
        auto *sessionsPage = window.findChild<QWidget *>(QStringLiteral("terminalSessionsPage"));
        auto *recentLogins = window.findChild<QTreeWidget *>(QStringLiteral("recentLoginList"));
        QVERIFY(tabs);
        QVERIFY(recentPage);
        QVERIFY(sessionsPage);
        QVERIFY(recentLogins);
        QCOMPARE(tabs->count(), 0);
        QCOMPARE(tabs->currentIndex(), -1);
        QVERIFY(recentPage->isVisible());
        QVERIFY(!sessionsPage->isVisible());
        QCOMPARE(recentLogins->topLevelItemCount(), 2);
        QCOMPARE(recentLogins->topLevelItem(0)->text(0), QStringLiteral("db-master-01"));
        QCOMPARE(recentLogins->topLevelItem(1)->text(0), QStringLiteral("prod-web-01"));
        auto *hostSearch = sidebar->findChild<QLineEdit *>(QStringLiteral("hostSearch"));
        auto *hostAdd = sidebar->findChild<QPushButton *>(QStringLiteral("hostAddButton"));
        QVERIFY(hostSearch);
        QVERIFY(hostAdd);
        QCOMPARE(hostSearch->geometry().y(), hostAdd->geometry().y());
        QVERIFY(hostAdd->geometry().x() > hostSearch->geometry().x());

        auto *range = window.findChild<QComboBox *>(QStringLiteral("historyRange"));
        auto *cpuTrend = window.findChild<noxshell::ui::TrendChart *>(QStringLiteral("cpuTrendChart"));
        QVERIFY(range);
        QVERIFY(cpuTrend);
        QCOMPARE(window.findChildren<noxshell::ui::TransferQueuePanel *>().size(), 0);
        QCOMPARE(range->count(), 3);

        QTest::qWait(350);
        QVERIFY(!window.findChild<QLineEdit *>(QStringLiteral("terminalInput")));
        QVERIFY(!window.findChild<noxshell::ui::TerminalView *>(QStringLiteral("terminalOutput")));
        QCOMPARE(cpuTrend->pointCount(), 0);

        auto *address = window.findChild<QLabel *>(QStringLiteral("serverAddress"));
        auto *onlineBadge = window.findChild<QLabel *>(QStringLiteral("onlineBadge"));
        auto *copyAddress = window.findChild<QToolButton *>(QStringLiteral("copyHostAddressButton"));
        auto *clearTerminal = window.findChild<QPushButton *>(QStringLiteral("clearTerminalButton"));
        QVERIFY(address);
        QVERIFY(onlineBadge);
        QVERIFY(copyAddress);
        QVERIFY(clearTerminal);
        QCOMPARE(address->text(), QStringLiteral("未选择主机"));
        QVERIFY(onlineBadge->text().contains(QStringLiteral("待连接")));
        QApplication::clipboard()->clear();
        QTest::mouseClick(copyAddress, Qt::LeftButton);
        QVERIFY(QApplication::clipboard()->text().isEmpty());

        hosts->itemDoubleClicked(hosts->item(0));
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 1000);
        QTRY_VERIFY_WITH_TIMEOUT(!sidebar->isVisible(), 1000);
        QCOMPARE(tabs->tabText(0), QStringLiteral("prod-web-01"));
        QVERIFY(!tabs->tabText(0).contains(QLatin1Char('@')));
        QVERIFY(!recentPage->isVisible());
        QVERIFY(sessionsPage->isVisible());
        auto *input = window.findChild<QLineEdit *>(QStringLiteral("terminalInput"));
        auto *output = window.findChild<noxshell::ui::TerminalView *>(QStringLiteral("terminalOutput"));
        auto *loadingOverlay = window.findChild<QWidget *>(QStringLiteral("terminalLoadingOverlay"));
        QVERIFY(input);
        QVERIFY(output);
        QVERIFY(loadingOverlay);
        QVERIFY(loadingOverlay->isVisible());
        QVERIFY(onlineBadge->text().contains(QStringLiteral("连接中")));
        QTRY_VERIFY_WITH_TIMEOUT(input->isEnabled(), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(!loadingOverlay->isVisible(), 1000);
        QVERIFY(onlineBadge->text().contains(QStringLiteral("在线")));
        QTRY_VERIFY_WITH_TIMEOUT(cpuTrend->pointCount() > 0, 1000);
        QCOMPARE(window.findChildren<noxshell::ui::TransferQueuePanel *>().size(), 1);

        input->setFocus();
        QTest::keyClicks(input, QStringLiteral("pwd"));
        QTest::keyClick(input, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(output->hasFocus(), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(output->plainText().contains(QStringLiteral("/var/www/app")), 1000);
        QTest::mouseClick(clearTerminal, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(output->plainText().contains(QStringLiteral("root@prod-web-01:/var/www/app#")), 1000);
        QVERIFY(!output->plainText().contains(QStringLiteral("Last login")));

        input->setFocus();
        QTest::keyClick(input, Qt::Key_C, Qt::ControlModifier);
        QTRY_VERIFY_WITH_TIMEOUT(output->plainText().contains(QStringLiteral("^C")), 1000);
        QVERIFY(output->hasFocus());
        input->setFocus();
        QTest::keyClick(input, Qt::Key_C, Qt::MetaModifier);
        QTRY_VERIFY_WITH_TIMEOUT(output->plainText().count(QStringLiteral("^C")) >= 2, 1000);
        QVERIFY(output->hasFocus());

        // 主机列表只负责选择连接：单击另一台主机不得切换当前终端或监控对象。
        QTest::mouseClick(sidebarToggle, Qt::LeftButton);
        QVERIFY(sidebar->isVisible());
        hosts->setCurrentRow(2);
        QCOMPARE(address->text(), QStringLiteral("10.0.0.11:22"));
        QVERIFY(onlineBadge->text().contains(QStringLiteral("在线")));
        QCOMPARE(tabs->count(), 1);
        QCOMPARE(tabs->currentIndex(), 0);
        const auto terminalInputs = window.findChildren<QLineEdit *>(QStringLiteral("terminalInput"));
        QCOMPARE(std::count_if(terminalInputs.cbegin(), terminalInputs.cend(),
                     [](const QLineEdit *editor) { return editor->isEnabled(); }),
            1);

        hosts->itemDoubleClicked(hosts->item(2));
        QTRY_VERIFY_WITH_TIMEOUT(!sidebar->isVisible(), 1000);
        QTRY_COMPARE_WITH_TIMEOUT([&window] {
            const auto inputs = window.findChildren<QLineEdit *>(QStringLiteral("terminalInput"));
            return std::count_if(inputs.cbegin(), inputs.cend(), [](const auto *editor) {
                return editor->isEnabled();
            });
        }(), 2, 1000);
        QCOMPARE(tabs->tabText(1), QStringLiteral("db-master-01"));
        auto *terminalWorkspace = window.findChild<noxshell::ui::TerminalWorkspace *>();
        QVERIFY(terminalWorkspace);
        const auto terminalSessions = terminalWorkspace->findChildren<noxshell::SshSession *>();
        QCOMPARE(terminalSessions.size(), 2);
        noxshell::SshSession *firstSession = nullptr;
        noxshell::SshSession *secondSession = nullptr;
        for (auto *session : terminalSessions) {
            if (session->profile().id == sidebar->servers().at(0).id) firstSession = session;
            if (session->profile().id == sidebar->servers().at(2).id) secondSession = session;
        }
        QVERIFY(firstSession);
        QVERIFY(secondSession);
        QSignalSpy firstReconnectSpy(firstSession, &noxshell::SshSession::connectionChanged);
        QSignalSpy secondReconnectSpy(secondSession, &noxshell::SshSession::connectionChanged);

        auto *fileStack = window.findChild<QStackedWidget *>(QStringLiteral("fileWorkspaceStack"));
        QVERIFY(fileStack);
        const auto activeFileServer = [fileStack] {
            return fileStack->currentWidget()->findChild<QLabel *>(QStringLiteral("fileServerLabel"));
        };
        tabs->setCurrentIndex(0);
        QTRY_COMPARE_WITH_TIMEOUT(hosts->currentRow(), 0, 1000);
        QCOMPARE(address->text(), QStringLiteral("10.0.0.11:22"));
        QTRY_VERIFY_WITH_TIMEOUT(cpuTrend->pointCount() > 0, 1000);
        QVERIFY(activeFileServer());
        QVERIFY(activeFileServer()->text().contains(QStringLiteral("prod-web-01")));
        auto *firstPath = fileStack->currentWidget()->findChild<QLineEdit *>(QStringLiteral("remotePathEdit"));
        QVERIFY(firstPath);
        firstPath->setText(QStringLiteral("/var/www/app"));
        QTest::keyClick(firstPath, Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(firstPath->text(), QStringLiteral("/var/www/app"), 1000);

        tabs->setCurrentIndex(1);
        QTRY_COMPARE_WITH_TIMEOUT(hosts->currentRow(), 2, 1000);
        QCOMPARE(address->text(), QStringLiteral("10.0.0.21:22"));
        QTRY_VERIFY_WITH_TIMEOUT(cpuTrend->pointCount() > 0, 1000);
        QVERIFY(activeFileServer()->text().contains(QStringLiteral("db-master-01")));

        // 左侧主机列表不再兼任标签导航，单击只保留列表选中项。
        hosts->setCurrentRow(0);
        QCOMPARE(tabs->currentIndex(), 1);
        QVERIFY(activeFileServer()->text().contains(QStringLiteral("db-master-01")));

        // 终端、监控和 SFTP 只通过终端标签联动，切换不得再次握手或重置文件目录。
        tabs->setCurrentIndex(0);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->currentIndex(), 0, 1000);
        QVERIFY(firstSession->isConnected());
        QVERIFY(secondSession->isConnected());
        QCOMPARE(firstReconnectSpy.count(), 0);
        QCOMPARE(secondReconnectSpy.count(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(cpuTrend->pointCount() > 0, 1000);
        QVERIFY(activeFileServer()->text().contains(QStringLiteral("prod-web-01")));
        QCOMPARE(fileStack->currentWidget()->findChild<QLineEdit *>(QStringLiteral("remotePathEdit"))->text(),
            QStringLiteral("/var/www/app"));
        auto *remoteFiles = fileStack->currentWidget()->findChild<QTreeWidget *>(QStringLiteral("remoteFileTree"));
        QVERIFY(remoteFiles);
        QTRY_VERIFY_WITH_TIMEOUT(remoteFiles->topLevelItemCount() > 0, 1000);
        QVERIFY(window.findChildren<noxshell::SshSession *>(QString{}, Qt::FindDirectChildrenOnly).isEmpty());

        // 左侧不展示会话状态，关闭标签只改变终端工作区。
        tabs->tabCloseRequested(1);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 1000);
        auto *closedHostRow = hosts->itemWidget(hosts->item(2));
        QVERIFY(closedHostRow);
        QVERIFY(!closedHostRow->findChild<QLabel *>(QStringLiteral("hostItemState")));

        auto *duplicate = window.findChild<QAction *>(QStringLiteral("terminalDuplicateAction"));
        QVERIFY(duplicate);
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("duplicateTerminalButton")));
        tabs->customContextMenuRequested(tabs->tabRect(0).center());
        duplicate->trigger();
        QCOMPARE(tabs->count(), 2);
        tabs->tabCloseRequested(tabs->currentIndex());
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(window.findChildren<noxshell::ui::TransferQueuePanel *>().size(), 1, 1000);
        window.close();
        QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 1000);
    }

    void serverDialogOffersConnectionTestBeforeSave()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        noxshell::ServerRepository repository(directory.filePath(QStringLiteral("dialog-test.sqlite3")), false);
        QVERIFY(repository.initialize());
        noxshell::CredentialStore credentialStore;

        noxshell::ui::ServerDialog addDialog;
        auto *addTestButton = addDialog.findChild<QPushButton *>(QStringLiteral("dialogTestConnectionButton"));
        auto *addStatus = addDialog.findChild<QLabel *>(QStringLiteral("connectionTestStatus"));
        auto *passwordEditor = addDialog.findChild<QLineEdit *>(QStringLiteral("passwordEditor"));
        auto *passwordReveal = addDialog.findChild<QToolButton *>(QStringLiteral("passwordRevealButton"));
        auto *passwordHint = addDialog.findChild<QLabel *>(QStringLiteral("passwordSourceHint"));
        QVERIFY(addTestButton);
        QVERIFY(addStatus);
        QVERIFY(passwordEditor);
        QVERIFY(passwordReveal);
        QVERIFY(passwordHint);
        QCOMPARE(passwordEditor->echoMode(), QLineEdit::Password);
        QVERIFY(passwordEditor->inputMethodHints().testFlag(Qt::ImhLatinOnly));
        passwordEditor->insert(QStringLiteral("abc。123"));
        QCOMPARE(passwordEditor->text(), QStringLiteral("abc.123"));
        QTRY_VERIFY(passwordHint->text().contains(QStringLiteral("自动转换")));
        passwordEditor->insert(QStringLiteral("中文"));
        QCOMPARE(passwordEditor->text(), QStringLiteral("abc.123"));
        QTRY_VERIFY(passwordHint->text().contains(QStringLiteral("已忽略")));
        passwordEditor->clear();
        passwordEditor->setText(QStringLiteral("临时密码"));
        QTest::mouseClick(passwordReveal, Qt::LeftButton);
        QCOMPARE(passwordEditor->echoMode(), QLineEdit::Normal);
        QVERIFY(passwordHint->text().contains(QStringLiteral("当前输入")));
        QVERIFY(!addTestButton->isEnabled());
        addDialog.setConnectionServices(&repository, &credentialStore);
        QVERIFY(addTestButton->isEnabled());
        QCOMPARE(addDialog.findChild<QPushButton *>(QStringLiteral("primaryButton"))->text(), QStringLiteral("保存主机"));

        noxshell::ServerProfile profile;
        profile.id = QStringLiteral("edit-test");
        profile.name = QStringLiteral("test-host");
        profile.host = QStringLiteral("192.0.2.10");
        profile.port = 22;
        profile.user = QStringLiteral("root");
        profile.authentication = noxshell::AuthenticationMethod::Password;
        profile.credentialRef = QStringLiteral("server/edit-test");
        profile.expectedFingerprint = QStringLiteral("SHA256:old-endpoint-fingerprint");
        noxshell::ui::ServerDialog editDialog(profile);
        editDialog.setConnectionServices(&repository, &credentialStore);
        auto *editTestButton = editDialog.findChild<QPushButton *>(QStringLiteral("dialogTestConnectionButton"));
        QVERIFY(editTestButton);
        QVERIFY(editTestButton->isEnabled());
        auto *hostEditor = editDialog.findChild<QLineEdit *>(QStringLiteral("hostEditor"));
        auto *portEditor = editDialog.findChild<QSpinBox *>(QStringLiteral("portEditor"));
        auto *fingerprintEditor = editDialog.findChild<QLineEdit *>(QStringLiteral("fingerprintEditor"));
        QVERIFY(hostEditor);
        QVERIFY(portEditor);
        QVERIFY(fingerprintEditor);
        QCOMPARE(fingerprintEditor->text(), profile.expectedFingerprint);
        hostEditor->setText(QStringLiteral("198.51.100.25"));
        QVERIFY(fingerprintEditor->text().isEmpty());
        QVERIFY(editDialog.profile().expectedFingerprint.isEmpty());
        QVERIFY(fingerprintEditor->placeholderText().contains(QStringLiteral("重新确认")));
        QVERIFY(editDialog.findChild<QLabel *>(QStringLiteral("passwordSourceHint"))->text().contains(QStringLiteral("Keychain")));
        QCOMPARE(editDialog.findChild<QPushButton *>(QStringLiteral("primaryButton"))->text(), QStringLiteral("保存修改"));
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("玄壳"));
    QApplication::setOrganizationName(QStringLiteral("NoxShell"));
    QApplication::setApplicationVersion(QString::fromLatin1(NOXSHELL_APP_VERSION));
    app.setStyleSheet(noxshell::ui::applicationStyleSheet());
    SmokeTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "SmokeTest.moc"
