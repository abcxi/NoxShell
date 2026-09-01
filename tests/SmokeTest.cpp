#include "../src/core/LinuxMetrics.h"
#include "../src/core/AppLogger.h"
#include "../src/core/CredentialStore.h"
#include "../src/core/FileTransferTask.h"
#include "../src/core/MetricHistory.h"
#include "../src/core/RdpLauncher.h"
#include "../src/core/SshSession.h"
#include "../src/core/ServerRepository.h"
#include "../src/ui/AppTheme.h"
#include "../src/ui/CommandHistoryPanel.h"
#include "../src/ui/FilePanel.h"
#include "../src/ui/FilePermissionDialog.h"
#include "../src/ui/HostSidebar.h"
#include "../src/ui/MainWindow.h"
#include "../src/ui/MetricCard.h"
#include "../src/ui/RemoteFileEditor.h"
#include "../src/ui/RdpDialog.h"
#include "../src/ui/SearchMarkerScrollBar.h"
#include "../src/ui/ServerDialog.h"
#include "../src/ui/SystemDetailPanel.h"
#include "../src/ui/TerminalSettingsDialog.h"
#include "../src/ui/TerminalWorkspace.h"
#include "../src/ui/TransferQueuePanel.h"
#include "../src/ui/TerminalView.h"
#include "../src/ui/VtTerminalModel.h"

#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFontComboBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QProgressDialog>
#include <QRadioButton>
#include <QScrollBar>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSplitter>
#include <QSpinBox>
#include <QStackedWidget>
#include <QPushButton>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
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

    void applicationThemeModesResolveAndProduceDistinctPalettes()
    {
        using noxshell::ui::ThemeMode;
        QCOMPARE(noxshell::ui::themeModeFromSetting(QStringLiteral("system")), ThemeMode::System);
        QCOMPARE(noxshell::ui::themeModeFromSetting(QStringLiteral("LIGHT")), ThemeMode::Light);
        QCOMPARE(noxshell::ui::themeModeFromSetting(QStringLiteral("dark")), ThemeMode::Dark);
        QCOMPARE(noxshell::ui::themeModeFromSetting(QStringLiteral("invalid")), ThemeMode::System);
        QCOMPARE(noxshell::ui::themeModeSettingValue(ThemeMode::System), QStringLiteral("system"));
        QCOMPARE(noxshell::ui::themeModeSettingValue(ThemeMode::Light), QStringLiteral("light"));
        QCOMPARE(noxshell::ui::themeModeSettingValue(ThemeMode::Dark), QStringLiteral("dark"));
        QVERIFY(noxshell::ui::applicationStyleSheet(false).contains(QStringLiteral("#F3F6FA")));
        const auto darkStyle = noxshell::ui::applicationStyleSheet(true);
        QVERIFY(darkStyle.contains(QStringLiteral("#111820")));
        QVERIFY(darkStyle.contains(QStringLiteral(
            "QTreeWidget#remoteDirectoryTree {\n            color:#D5E0EB; background:#151D25;")));
        QVERIFY(darkStyle.contains(QStringLiteral(
            "QTreeWidget#remoteDirectoryTree::item:selected {\n            color:#FFFFFF; background:#174E78;")));
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

    void terminalViewFollowsApplicationCursorModeForVi()
    {
        noxshell::ui::TerminalView view;
        QSignalSpy inputSpy(&view, &noxshell::ui::TerminalView::inputGenerated);
        view.resize(640, 240);
        view.show();
        view.setFocus();

        // vi/vim enables DECCKM while its full-screen editor is active. In
        // that mode xterm cursor keys use SS3 (ESC O), not CSI (ESC [).
        view.feedText(QStringLiteral("\x1b[?1h"));
        QTest::keyClick(&view, Qt::Key_Up);
        QTest::keyClick(&view, Qt::Key_Down);
        QTest::keyClick(&view, Qt::Key_Right);
        QTest::keyClick(&view, Qt::Key_Left);
        QCOMPARE(inputSpy.at(0).at(0).toByteArray(), QByteArray("\x1bOA"));
        QCOMPARE(inputSpy.at(1).at(0).toByteArray(), QByteArray("\x1bOB"));
        QCOMPARE(inputSpy.at(2).at(0).toByteArray(), QByteArray("\x1bOC"));
        QCOMPARE(inputSpy.at(3).at(0).toByteArray(), QByteArray("\x1bOD"));

        view.feedText(QStringLiteral("\x1b[?1l"));
        QTest::keyClick(&view, Qt::Key_Up);
        QCOMPARE(inputSpy.at(4).at(0).toByteArray(), QByteArray("\x1b[A"));
    }

    void terminalViewSearchesHighlightsAndNavigatesOutput()
    {
        noxshell::ui::TerminalView view;
        view.resize(640, 220);
        view.show();
        view.feedText(QStringLiteral("first needle result\r\nsecond line\r\nthird NEEDLE result"));
        QTest::qWait(20);

#ifdef Q_OS_MAC
        QTest::keyClick(&view, Qt::Key_F, Qt::MetaModifier);
#else
        QTest::keyClick(&view, Qt::Key_F, Qt::ControlModifier);
#endif
        QCoreApplication::processEvents();
        auto *searchInput = view.findChild<QLineEdit *>(QStringLiteral("terminalSearchInput"));
        auto *counter = view.findChild<QLabel *>(QStringLiteral("terminalSearchCounter"));
        auto *previous = view.findChild<QToolButton *>(QStringLiteral("terminalSearchPrevious"));
        auto *next = view.findChild<QToolButton *>(QStringLiteral("terminalSearchNext"));
        auto *close = view.findChild<QToolButton *>(QStringLiteral("terminalSearchClose"));
        auto *searchMarkers = view.findChild<noxshell::ui::SearchMarkerScrollBar *>(
            QStringLiteral("terminalScrollBar"));
        QVERIFY(searchInput);
        QVERIFY(counter);
        QVERIFY(previous);
        QVERIFY(next);
        QVERIFY(close);
        QVERIFY(searchMarkers);
        QVERIFY(searchInput->isVisible());

        searchInput->setText(QStringLiteral("needle"));
        QCOMPARE(view.searchMatchCount(), 2);
        QCOMPARE(view.currentSearchMatch(), 0);
        QCOMPARE(counter->text(), QStringLiteral("1 / 2"));
        QCOMPARE(searchMarkers->searchMarkerCount(), 2);
        QCOMPARE(searchMarkers->currentSearchMarker(), 0);

        QImage rendered(view.size(), QImage::Format_ARGB32);
        view.render(&rendered);
        bool foundHighlight = false;
        bool foundScrollMarker = false;
        for (int y = 0; y < rendered.height() && !foundHighlight; ++y) {
            for (int x = 0; x < rendered.width(); ++x) {
                const auto color = rendered.pixelColor(x, y);
                if (color == QColor(QStringLiteral("#FFB938"))
                    || color == QColor(QStringLiteral("#FFF36A"))) {
                    foundHighlight = true;
                    break;
                }
            }
        }
        for (int y = 0; y < rendered.height() && !foundScrollMarker; ++y) {
            for (int x = qMax(0, rendered.width() - searchMarkers->width()); x < rendered.width(); ++x) {
                const auto color = rendered.pixelColor(x, y);
                if (color == QColor(QStringLiteral("#FFD84D"))
                    || color == QColor(QStringLiteral("#FF8A00"))) {
                    foundScrollMarker = true;
                    break;
                }
            }
        }
        QVERIFY(foundHighlight);
        QVERIFY(foundScrollMarker);

        QTest::mouseClick(searchMarkers, Qt::LeftButton, Qt::NoModifier,
            searchMarkers->searchMarkerRect(1).center());
        QCOMPARE(view.currentSearchMatch(), 1);
        QCOMPARE(counter->text(), QStringLiteral("2 / 2"));
        QCOMPARE(searchMarkers->currentSearchMarker(), 1);

        QTest::mouseClick(next, Qt::LeftButton);
        QCOMPARE(view.currentSearchMatch(), 0);
        QCOMPARE(counter->text(), QStringLiteral("1 / 2"));
        QTest::mouseClick(previous, Qt::LeftButton);
        QCOMPARE(view.currentSearchMatch(), 1);

        searchInput->setText(QStringLiteral("not present"));
        QCOMPARE(view.searchMatchCount(), 0);
        QCOMPARE(searchMarkers->searchMarkerCount(), 0);
        QCOMPARE(counter->text(), QStringLiteral("0 / 0"));
        QVERIFY(!previous->isEnabled());
        QVERIFY(!next->isEnabled());

        QTest::mouseClick(close, Qt::LeftButton);
        QVERIFY(!searchInput->isVisible());
    }

    void terminalCommandBlockHoverCopiesTextAndImageWithoutRelayout()
    {
        noxshell::ui::TerminalView view;
        view.resize(720, 260);
        view.show();
        QTest::qWait(20);
        view.feedText(QStringLiteral(
            "[root@test-host ~]# ls\r\none\r\ntwo\r\n[root@test-host ~]# "));
        QCoreApplication::processEvents();

        auto *tools = view.findChild<QFrame *>(QStringLiteral("terminalCommandBlockTools"));
        auto *copyText = view.findChild<QToolButton *>(QStringLiteral("terminalCommandBlockCopyText"));
        auto *copyImage = view.findChild<QToolButton *>(QStringLiteral("terminalCommandBlockCopyImage"));
        QVERIFY(tools);
        QVERIFY(copyText);
        QVERIFY(copyImage);
        QVERIFY(!tools->isVisible());

        const auto originalSize = view.size();
        const auto originalRows = view.rows();
        QSignalSpy resizeSpy(&view, &noxshell::ui::TerminalView::terminalSizeChanged);
        const auto hoverPoint = view.contentOrigin().toPoint()
            + QPoint(80, qRound(view.cellSize().height() * 1.5));
        QTest::mouseMove(&view, hoverPoint);
        QTRY_VERIFY_WITH_TIMEOUT(tools->isVisible(), 1000);
        QCOMPARE(view.size(), originalSize);
        QCOMPARE(view.rows(), originalRows);
        QCOMPARE(resizeSpy.count(), 0);

        QApplication::clipboard()->clear();
        QTest::mouseClick(copyText, Qt::LeftButton);
        QCOMPARE(QApplication::clipboard()->text(),
            QStringLiteral("[root@test-host ~]# ls\none\ntwo"));
        QTest::mouseClick(copyImage, Qt::LeftButton);
        const auto copiedImage = QApplication::clipboard()->image();
        QVERIFY(!copiedImage.isNull());
        QVERIFY(copiedImage.width() > 300);
        QVERIFY(copiedImage.height() >= qCeil(view.cellSize().height() * 3));

        QTest::mouseMove(&view, QPoint(2, 2));
        QTRY_VERIFY_WITH_TIMEOUT(!tools->isVisible(), 1000);
    }

    void terminalViewSendsTabOncePerPhysicalKeyPress()
    {
        noxshell::ui::TerminalView view;
        QSignalSpy inputSpy(&view, &noxshell::ui::TerminalView::inputGenerated);
        view.resize(640, 240);
        view.show();
        view.setFocus();

        QKeyEvent firstPress(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier, QStringLiteral("\t"));
        QKeyEvent duplicatePress(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier, QStringLiteral("\t"));
        QKeyEvent repeatPress(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier, QStringLiteral("\t"), true, 2);
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_Tab, Qt::NoModifier, QStringLiteral("\t"));
        QApplication::sendEvent(&view, &firstPress);
        QApplication::sendEvent(&view, &duplicatePress);
        QApplication::sendEvent(&view, &repeatPress);
        QApplication::sendEvent(&view, &release);

        QCOMPARE(inputSpy.count(), 1);
        QCOMPARE(inputSpy.first().at(0).toByteArray(), QByteArray("\t"));

        QTest::keyClick(&view, Qt::Key_Tab);
        QCOMPARE(inputSpy.count(), 2);
        QCOMPARE(inputSpy.last().at(0).toByteArray(), QByteArray("\t"));
    }

    void terminalAppearanceUpdatesFontAndLineSpacing()
    {
        const auto originalDefault = noxshell::ui::TerminalView::defaultAppearance();
        noxshell::ui::TerminalView view;
        view.resize(640, 300);
        view.show();
        QTest::qWait(20);

        const auto originalCellHeight = view.cellSize().height();
        auto changed = originalDefault;
        changed.pointSize = qMin(32, qMax(8, originalDefault.pointSize + 2));
        changed.lineSpacing = 1.5;
        view.setAppearance(changed);

        QCOMPARE(view.appearance().pointSize, changed.pointSize);
        QCOMPARE(view.appearance().lineSpacing, changed.lineSpacing);
        QVERIFY(view.cellSize().height() > originalCellHeight);
        noxshell::ui::TerminalView::setDefaultAppearance(originalDefault);
    }

    void terminalSettingsDialogExposesFontSizeAndSpacingControls()
    {
        noxshell::ui::TerminalSettingsDialog dialog(noxshell::ui::TerminalView::defaultAppearance());
        auto *fontFamily = dialog.findChild<QFontComboBox *>(QStringLiteral("terminalFontFamilyCombo"));
        QVERIFY(fontFamily);
        QVERIFY(!fontFamily->isEditable());
        QVERIFY(fontFamily->count() > 0);
        QVERIFY(fontFamily->toolTip().contains(QStringLiteral("系统")));
        QVERIFY(dialog.findChild<QSpinBox *>(QStringLiteral("terminalFontSizeSpin")));
        QVERIFY(dialog.findChild<QDoubleSpinBox *>(QStringLiteral("terminalLineSpacingSpin")));
        QVERIFY(dialog.findChild<QLabel *>(QStringLiteral("terminalAppearancePreview")));
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

        const auto cellSize = view.cellSize();
        const int cellWidth = qCeil(cellSize.width());
        const int cellHeight = qCeil(cellSize.height());
        const auto origin = view.contentOrigin().toPoint();
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier,
            origin + QPoint(cellWidth / 2, cellHeight / 2));
        QTest::mouseMove(&view, origin + QPoint(cellWidth * 6, cellHeight / 2));
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier,
            origin + QPoint(cellWidth * 6, cellHeight / 2));
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
        auto *tree = sidebar.findChild<QTreeWidget *>(QStringLiteral("hostList"));
        QVERIFY(tree);
        QCOMPARE(sidebar.servers().size(), 0);
        QCOMPARE(tree->topLevelItemCount(), 1);
        QVERIFY(tree->topLevelItem(0)->text(0).contains(QStringLiteral("新建分组")));
        QVERIFY(!(tree->topLevelItem(0)->flags() & Qt::ItemIsSelectable));
    }

    void hostSidebarSelectsWithoutConnectingAndActivatesOnDoubleClick()
    {
        noxshell::ServerProfile profile;
        profile.id = QStringLiteral("sidebar-host");
        profile.name = QStringLiteral("sidebar-host");
        profile.host = QStringLiteral("192.0.2.10");
        profile.group = QStringLiteral("生产环境");
        noxshell::ui::HostSidebar sidebar({profile}, {QStringLiteral("生产环境"), QStringLiteral("测试环境")});
        QSignalSpy selectedSpy(&sidebar, &noxshell::ui::HostSidebar::serverSelected);
        QSignalSpy connectSpy(&sidebar, &noxshell::ui::HostSidebar::serverConnectRequested);
        QSignalSpy collapseSpy(&sidebar, &noxshell::ui::HostSidebar::collapseRequested);
        QSignalSpy groupChangedSpy(&sidebar, &noxshell::ui::HostSidebar::serverGroupChanged);
        QSignalSpy addInGroupSpy(&sidebar, &noxshell::ui::HostSidebar::addServerInGroupRequested);
        QSignalSpy addRdpInGroupSpy(&sidebar, &noxshell::ui::HostSidebar::addRdpServerInGroupRequested);
        sidebar.show();
        sidebar.selectFirstServer();
        QCOMPARE(selectedSpy.count(), 1);
        QCOMPARE(connectSpy.count(), 0);

        auto *tree = sidebar.findChild<QTreeWidget *>(QStringLiteral("hostList"));
        QVERIFY(tree);
        QCOMPARE(tree->topLevelItemCount(), 2);
        QCOMPARE(tree->topLevelItem(0)->childCount(), 1);
        QCOMPARE(tree->topLevelItem(1)->childCount(), 0);
        QVERIFY(tree->topLevelItem(0)->isExpanded());
        QVERIFY(tree->dragEnabled());
        QVERIFY(tree->acceptDrops());
        auto *search = sidebar.findChild<QLineEdit *>(QStringLiteral("hostSearch"));
        auto *addButton = sidebar.findChild<QPushButton *>(QStringLiteral("hostAddButton"));
        QVERIFY(search);
        QVERIFY(addButton);
        QVERIFY(addButton->menu());
        QVERIFY(sidebar.findChild<QAction *>(QStringLiteral("hostAddSshAction")));
        QVERIFY(sidebar.findChild<QAction *>(QStringLiteral("hostAddRdpAction")));
        QCOMPARE(search->geometry().y(), addButton->geometry().y());
        QVERIFY(addButton->geometry().x() > search->geometry().x());
        auto *hostItem = tree->topLevelItem(0)->child(0);
        auto *rowWidget = tree->itemWidget(hostItem, 0);
        QVERIFY(rowWidget);
        QCOMPARE(rowWidget->findChild<QLabel *>(QStringLiteral("hostItemName"))->text(), QStringLiteral("sidebar-host"));
        QCOMPARE(rowWidget->findChild<QLabel *>(QStringLiteral("hostItemAddress"))->text(), QStringLiteral("192.0.2.10"));
        QCOMPARE(rowWidget->findChild<QLabel *>(QStringLiteral("hostItemName"))->geometry().y(),
            rowWidget->findChild<QLabel *>(QStringLiteral("hostItemAddress"))->geometry().y());
        QVERIFY(hostItem->sizeHint(0).height() <= 42);
        QVERIFY(!rowWidget->findChild<QLabel *>(QStringLiteral("hostItemState")));
        QVERIFY(!hostItem->toolTip(0).contains(QStringLiteral("离线")));
        QVERIFY(!hostItem->toolTip(0).contains(QStringLiteral("在线")));
        QVERIFY(sidebar.setServerState(profile.id, noxshell::ServerState::Online));
        QVERIFY(!rowWidget->findChild<QLabel *>(QStringLiteral("hostItemState")));
        tree->itemDoubleClicked(hostItem, 0);
        QCOMPARE(connectSpy.count(), 1);
        QCOMPARE(collapseSpy.count(), 1);
        QCOMPARE(qvariant_cast<noxshell::ServerProfile>(connectSpy.first().at(0)).id, profile.id);
        QVERIFY(sidebar.moveServerToGroup(profile.id, QStringLiteral("测试环境")));
        QCOMPARE(groupChangedSpy.count(), 1);
        QCOMPARE(sidebar.servers().first().group, QStringLiteral("测试环境"));
        QCOMPARE(tree->topLevelItem(0)->childCount(), 0);
        QCOMPARE(tree->topLevelItem(1)->childCount(), 1);
        QVERIFY(sidebar.findChild<QAction *>(QStringLiteral("hostConnectAction")));
        QVERIFY(sidebar.findChild<QAction *>(QStringLiteral("hostDuplicateAction")));
        QVERIFY(sidebar.findChild<QAction *>(QStringLiteral("hostDeleteAction")));
        QVERIFY(sidebar.findChild<QAction *>(QStringLiteral("hostNewGroupAction")));
        auto *newConnection = sidebar.findChild<QAction *>(QStringLiteral("hostNewConnectionAction"));
        QVERIFY(newConnection);
        QCOMPARE(newConnection->text(), QStringLiteral("新建连接"));
        tree->customContextMenuRequested(tree->visualItemRect(tree->topLevelItem(1)).center());
        QVERIFY(newConnection->isVisible());
        auto *newSshConnection = sidebar.findChild<QAction *>(QStringLiteral("hostNewSshConnectionAction"));
        auto *newRdpConnection = sidebar.findChild<QAction *>(QStringLiteral("hostNewRdpConnectionAction"));
        QVERIFY(newSshConnection);
        QVERIFY(newRdpConnection);
        newSshConnection->trigger();
        QCOMPARE(addInGroupSpy.count(), 1);
        QCOMPARE(addInGroupSpy.first().at(0).toString(), QStringLiteral("测试环境"));
        newRdpConnection->trigger();
        QCOMPARE(addRdpInGroupSpy.count(), 1);
        QCOMPARE(addRdpInGroupSpy.first().at(0).toString(), QStringLiteral("测试环境"));
        QVERIFY(sidebar.findChild<QAction *>(QStringLiteral("hostRenameGroupAction")));
        QVERIFY(sidebar.findChild<QAction *>(QStringLiteral("hostDeleteGroupAction")));
        QVERIFY(sidebar.findChild<QMenu *>(QStringLiteral("hostMoveGroupMenu")));
    }

    void ungroupedHostsStayAtSidebarTopLevel()
    {
        noxshell::ServerProfile profile;
        profile.id = QStringLiteral("ungrouped-host");
        profile.name = QStringLiteral("无分组主机");
        profile.host = QStringLiteral("192.0.2.30");
        profile.group.clear();
        noxshell::ui::HostSidebar sidebar({profile}, {QStringLiteral("demo")});
        sidebar.show();

        auto *tree = sidebar.findChild<QTreeWidget *>(QStringLiteral("hostList"));
        QVERIFY(tree);
        QCOMPARE(tree->topLevelItemCount(), 2);
        auto *hostItem = tree->topLevelItem(0);
        QVERIFY(hostItem);
        QCOMPARE(hostItem->childCount(), 0);
        QVERIFY(!hostItem->parent());
        auto *rowWidget = tree->itemWidget(hostItem, 0);
        QVERIFY(rowWidget);
        QCOMPARE(rowWidget->findChild<QLabel *>(QStringLiteral("hostItemName"))->text(),
            QStringLiteral("无分组主机"));
        QVERIFY(!tree->topLevelItem(0)->text(0).contains(QStringLiteral("未分组")));
        QVERIFY(tree->topLevelItem(1)->text(0).startsWith(QStringLiteral("demo")));

        sidebar.selectFirstServer();
        QCOMPARE(tree->currentItem(), hostItem);
    }

    void rdpProfilesBuildSafeNativeClientLaunches()
    {
        noxshell::ServerProfile profile;
        profile.name = QStringLiteral("Windows 办公电脑");
        profile.host = QStringLiteral("192.0.2.80");
        profile.port = 3390;
        profile.user = QStringLiteral("DOMAIN\\operator");
        profile.connectionMode = noxshell::ConnectionMode::Rdp;

        QCOMPARE(noxshell::RdpLauncher::endpoint(profile), QStringLiteral("192.0.2.80:3390"));
        const auto windows = noxshell::RdpLauncher::launchSpec(
            profile, noxshell::RdpClientPlatform::Windows);
        QCOMPARE(windows.program, QStringLiteral("mstsc.exe"));
        QVERIFY(windows.arguments.contains(QStringLiteral("/v:192.0.2.80:3390")));
        QVERIFY(windows.arguments.contains(QStringLiteral("/prompt")));
        QVERIFY(!windows.arguments.join(QLatin1Char(' ')).contains(QStringLiteral("password"), Qt::CaseInsensitive));

        const auto mac = noxshell::RdpLauncher::launchSpec(profile, noxshell::RdpClientPlatform::MacOS);
        QCOMPARE(mac.program, QStringLiteral("/usr/bin/open"));
        QCOMPARE(mac.arguments.size(), 3);
        QCOMPARE(mac.arguments.at(0), QStringLiteral("-b"));
        QCOMPARE(mac.arguments.at(1), QStringLiteral("com.microsoft.rdc.macos"));
        QVERIFY(mac.arguments.at(2).startsWith(QStringLiteral("rdp://")));
        QVERIFY(mac.arguments.at(2).contains(QStringLiteral("192.0.2.80:3390")));
        QVERIFY(mac.arguments.at(2).contains(QStringLiteral("DOMAIN")));

        profile.password = QStringLiteral("must-not-appear-in-rdp-file");
        const auto fileContents = noxshell::RdpLauncher::connectionFileContents(profile);
        QVERIFY(fileContents.contains(QStringLiteral("full address:s:192.0.2.80:3390")));
        QVERIFY(fileContents.contains(QStringLiteral("username:s:DOMAIN\\operator")));
        QVERIFY(fileContents.contains(QStringLiteral("prompt for credentials:i:0")));
        QVERIFY(!fileContents.contains(profile.password));

        profile.host = QStringLiteral("2001:db8::10");
        QCOMPARE(noxshell::RdpLauncher::endpoint(profile), QStringLiteral("[2001:db8::10]:3390"));
    }

    void rdpDialogCreatesWindowsDesktopProfileWithoutSecrets()
    {
        noxshell::ui::RdpDialog dialog;
        dialog.setAvailableGroups({QStringLiteral("办公电脑"), QStringLiteral("生产环境")});
        dialog.setInitialGroup(QStringLiteral("办公电脑"));
        auto *name = dialog.findChild<QLineEdit *>(QStringLiteral("rdpNameEditor"));
        auto *host = dialog.findChild<QLineEdit *>(QStringLiteral("rdpHostEditor"));
        auto *port = dialog.findChild<QSpinBox *>(QStringLiteral("rdpPortEditor"));
        auto *user = dialog.findChild<QLineEdit *>(QStringLiteral("rdpUserEditor"));
        auto *password = dialog.findChild<QLineEdit *>(QStringLiteral("rdpPasswordEditor"));
        auto *passwordReveal = dialog.findChild<QAction *>(QStringLiteral("rdpPasswordRevealAction"));
        auto *group = dialog.findChild<QComboBox *>(QStringLiteral("rdpGroupEditor"));
        QVERIFY(name);
        QVERIFY(host);
        QVERIFY(port);
        QVERIFY(user);
        QVERIFY(password);
        QVERIFY(passwordReveal);
        QVERIFY(group);
        QCOMPARE(port->value(), 3389);
        QCOMPARE(group->currentData().toString(), QStringLiteral("办公电脑"));
        name->setText(QStringLiteral("win-01"));
        host->setText(QStringLiteral("203.0.113.20"));
        user->setText(QStringLiteral("Administrator"));
        password->setText(QStringLiteral("rdp-secret"));
        QCOMPARE(password->echoMode(), QLineEdit::Password);
        passwordReveal->trigger();
        QCOMPARE(password->echoMode(), QLineEdit::Normal);
        const auto profile = dialog.profile();
        QCOMPARE(profile.connectionMode, noxshell::ConnectionMode::Rdp);
        QCOMPARE(profile.os, QStringLiteral("windows"));
        QCOMPARE(profile.port, static_cast<quint16>(3389));
        QCOMPARE(profile.user, QStringLiteral("Administrator"));
        QCOMPARE(profile.password, QStringLiteral("rdp-secret"));
        QVERIFY(profile.credentialRef.isEmpty());

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        noxshell::ServerRepository repository(directory.filePath(QStringLiteral("rdp.sqlite3")), false);
        QVERIFY2(repository.initialize(), qPrintable(repository.lastError()));
        auto saved = profile;
        QVERIFY2(repository.saveServer(saved), qPrintable(repository.lastError()));
        const auto loaded = repository.loadServers();
        QCOMPARE(loaded.size(), 1);
        QCOMPARE(loaded.first().connectionMode, noxshell::ConnectionMode::Rdp);
        QCOMPARE(loaded.first().port, static_cast<quint16>(3389));
        QCOMPARE(loaded.first().user, QStringLiteral("Administrator"));
        QVERIFY(loaded.first().password.isEmpty());
    }

    void terminalTabContextMenuDuplicatesAndClosesSessions()
    {
        noxshell::ui::TerminalWorkspace workspace(nullptr, nullptr);
        workspace.resize(900, 500);

        auto *viewStack = workspace.findChild<QStackedWidget *>(QStringLiteral("terminalWorkspaceViewStack"));
        auto *recentPage = workspace.findChild<QWidget *>(QStringLiteral("terminalRecentPage"));
        auto *sessionsPage = workspace.findChild<QWidget *>(QStringLiteral("terminalSessionsPage"));
        auto *recentEmpty = workspace.findChild<QLabel *>(QStringLiteral("recentLoginEmpty"));
        QVERIFY(viewStack);
        QVERIFY(recentPage);
        QVERIFY(sessionsPage);
        QVERIFY(recentEmpty);
        QCOMPARE(viewStack->currentWidget(), recentPage);
        auto *tabToolbar = workspace.findChild<QWidget *>(QStringLiteral("terminalTabToolbar"));
        QVERIFY(tabToolbar);
        QVERIFY(tabToolbar->isHidden());

        noxshell::ServerProfile first;
        first.id = QStringLiteral("tab-context-first");
        first.name = QStringLiteral("第一台超长服务器名称甲");
        first.connectionMode = noxshell::ConnectionMode::Demo;
        noxshell::ServerProfile second;
        second.id = QStringLiteral("tab-context-second");
        second.name = QStringLiteral("第二台");
        second.connectionMode = noxshell::ConnectionMode::Demo;
        workspace.openOrActivate(first, false);
        workspace.openOrActivate(second, false);
        QCOMPARE(viewStack->currentWidget(), sessionsPage);

        auto *tabs = workspace.findChild<QTabBar *>(QStringLiteral("terminalSessionTabs"));
        auto *connectAction = workspace.findChild<QAction *>(QStringLiteral("terminalConnectAction"));
        auto *disconnectAction = workspace.findChild<QAction *>(QStringLiteral("terminalDisconnectAction"));
        auto *clearAction = workspace.findChild<QAction *>(QStringLiteral("terminalClearAction"));
        auto *duplicateAction = workspace.findChild<QAction *>(QStringLiteral("terminalDuplicateAction"));
        auto *closeCurrentAction = workspace.findChild<QAction *>(QStringLiteral("terminalCloseCurrentAction"));
        auto *closeOthersAction = workspace.findChild<QAction *>(QStringLiteral("terminalCloseOthersAction"));
        auto *closeAllAction = workspace.findChild<QAction *>(QStringLiteral("terminalCloseAllAction"));
        auto *newTabButton = workspace.findChild<QToolButton *>(QStringLiteral("terminalNewTabButton"));
        QVERIFY(tabs);
        QVERIFY(connectAction);
        QVERIFY(disconnectAction);
        QVERIFY(clearAction);
        QVERIFY(duplicateAction);
        QVERIFY(closeCurrentAction);
        QVERIFY(closeOthersAction);
        QVERIFY(closeAllAction);
        QVERIFY(newTabButton);
        QSignalSpy sidebarVisibilitySpy(&workspace,
            &noxshell::ui::TerminalWorkspace::hostSidebarVisibilityRequested);
        QVERIFY(!workspace.findChild<QPushButton *>(QStringLiteral("clearTerminalButton")));
        QVERIFY(!workspace.findChild<QPushButton *>(QStringLiteral("duplicateTerminalButton")));
        QVERIFY(!tabToolbar->isHidden());
        auto *tabToolbarLayout = qobject_cast<QHBoxLayout *>(tabToolbar->layout());
        QVERIFY(tabToolbarLayout);
        QCOMPARE(tabToolbarLayout->indexOf(tabs), 0);
        QCOMPARE(tabToolbarLayout->count(), 1);
        QCOMPARE(tabToolbarLayout->stretch(0), 1);
        QVERIFY(tabs->sizeHint().width() >= 100);
        tabToolbarLayout->activate();
        QVERIFY(tabs->width() >= 100);
        tabs->resize(600, tabs->height());
        QVERIFY(tabs->tabRect(0).width() >= 95);
        QVERIFY(tabs->tabRect(0).left() < 20);
        QCOMPARE(tabs->count(), 2);
        QCOMPARE(tabs->tabText(0).size(), 10);
        QVERIFY(tabs->tabText(0).endsWith(QChar(0x2026)));
        QVERIFY(tabs->tabToolTip(0).contains(first.name));
        auto *firstCloseContainer = tabs->tabButton(0, QTabBar::RightSide);
        QVERIFY(firstCloseContainer);
        auto *firstCloseButton = firstCloseContainer->findChild<QToolButton *>(
            QStringLiteral("terminalTabCloseButton"));
        QVERIFY(firstCloseButton);
        QVERIFY(!tabs->tabButton(0, QTabBar::LeftSide));
        QCoreApplication::processEvents();
        const QRect firstTabRect = tabs->tabRect(0);
        const int closeRight = firstCloseButton->mapTo(tabs, firstCloseButton->rect().topRight()).x();
        QVERIFY(firstTabRect.right() - closeRight >= 5);
        const int lastTabRight = tabs->geometry().left() + tabs->tabRect(1).right();
        QVERIFY(newTabButton->x() >= lastTabRight);
        QVERIFY(newTabButton->x() <= lastTabRight + 8);
        QVERIFY(tabs->height() >= 30);
        QVERIFY(tabs->currentIndex() >= 0);
        QVERIFY(!tabs->tabText(tabs->currentIndex()).isEmpty());
        QVERIFY(!tabs->tabIcon(0).isNull());
        QVERIFY(tabs->tabToolTip(0).contains(QStringLiteral("未连接")));

        auto *sessionStack = workspace.findChild<QStackedWidget *>(QStringLiteral("terminalSessionStack"));
        QVERIFY(sessionStack);
        QCOMPARE(sessionStack->count(), 2);
        auto *firstOutput = sessionStack->widget(0)->findChild<noxshell::ui::TerminalView *>();
        auto *secondOutput = sessionStack->widget(1)->findChild<noxshell::ui::TerminalView *>();
        QVERIFY(firstOutput);
        QVERIFY(secondOutput);
        firstOutput->feedText(QStringLiteral("first-session-buffer"));
        secondOutput->feedText(QStringLiteral("second-session-buffer"));

        newTabButton->click();
        QCOMPARE(viewStack->currentWidget(), recentPage);
        QCOMPARE(tabs->count(), 2);
        QCOMPARE(sidebarVisibilitySpy.count(), 1);
        QCOMPARE(sidebarVisibilitySpy.takeFirst().at(0).toBool(), true);
        tabs->tabBarClicked(1);
        QCOMPARE(viewStack->currentWidget(), sessionsPage);
        QCOMPARE(tabs->currentIndex(), 1);

        const auto prepareContext = [&workspace](int index) {
            bool prepared = false;
            const bool invoked = QMetaObject::invokeMethod(&workspace, "prepareTabContextMenu",
                Qt::DirectConnection, Q_RETURN_ARG(bool, prepared), Q_ARG(int, index));
            return invoked && prepared;
        };

        QVERIFY(prepareContext(1));
        clearAction->trigger();
        QVERIFY(firstOutput->plainText().contains(QStringLiteral("first-session-buffer")));
        QVERIFY(!secondOutput->plainText().contains(QStringLiteral("second-session-buffer")));

        QVERIFY(prepareContext(0));
        QVERIFY(connectAction->isEnabled());
        QVERIFY(!disconnectAction->isEnabled());
        connectAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(tabs->tabToolTip(0).contains(QStringLiteral("连接成功")), 1000);
        QVERIFY(prepareContext(0));
        QVERIFY(!connectAction->isEnabled());
        QVERIFY(disconnectAction->isEnabled());
        disconnectAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(tabs->tabToolTip(0).contains(QStringLiteral("未连接")), 1000);

        QVERIFY(prepareContext(0));
        duplicateAction->trigger();
        QCOMPARE(tabs->count(), 3);

        QVERIFY(prepareContext(1));
        closeOthersAction->trigger();
        QCOMPARE(tabs->count(), 1);
        QCOMPARE(tabs->tabText(0), QStringLiteral("第二台"));

        QVERIFY(prepareContext(0));
        duplicateAction->trigger();
        QCOMPARE(tabs->count(), 2);
        auto *secondCloseButton = tabs->tabButton(1, QTabBar::RightSide)
                                      ->findChild<QToolButton *>(QStringLiteral("terminalTabCloseButton"));
        QVERIFY(secondCloseButton);
        secondCloseButton->click();
        QCOMPARE(tabs->count(), 1);

        QVERIFY(prepareContext(0));
        duplicateAction->trigger();
        QCOMPARE(tabs->count(), 2);
        QVERIFY(prepareContext(0));
        closeAllAction->trigger();
        QCOMPARE(tabs->count(), 0);
        QCOMPARE(viewStack->currentWidget(), recentPage);
    }

    void editingServerKeepsOldConnectionSnapshotAndNextConnectUsesNewProfile()
    {
        noxshell::ui::TerminalWorkspace workspace(nullptr, nullptr);
        workspace.resize(900, 500);

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

        const auto cellSize = view.cellSize();
        const int cellWidth = qCeil(cellSize.width());
        const int cellHeight = qCeil(cellSize.height());
        const auto origin = view.contentOrigin().toPoint();
        QVERIFY(origin.x() >= 14);
        QVERIFY(origin.y() >= 10);
        const QPoint start = origin + QPoint(cellWidth / 2, cellHeight / 2);
        const QPoint end = origin + QPoint(cellWidth * 5 + cellWidth / 2, cellHeight / 2);
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

    void metricCardUsesThinTrackAndExpandsCpuCoresOnHover()
    {
        noxshell::ui::MetricCard card(QStringLiteral("CPU"), QColor(QStringLiteral("#006EFF")));
        card.resize(220, 38);
        card.setValue(QStringLiteral("36.3%"), QStringLiteral("内核态 15.8%"), 36);
        card.setCoreValues({12.0, 34.0, 56.0, 78.0});
        card.show();
        QCoreApplication::processEvents();

        auto *summaryProgress = card.findChild<QProgressBar *>(QStringLiteral("metricProgress"));
        auto *corePanel = card.findChild<QFrame *>(QStringLiteral("metricCorePanel"));
        QVERIFY(summaryProgress);
        QVERIFY(corePanel);
        QCOMPARE(summaryProgress->height(), 24);
        QVERIFY(summaryProgress->format().contains(QStringLiteral("36.3%")));
        QVERIFY(summaryProgress->styleSheet().contains(QStringLiteral("stop:0.39")));
        QEvent initialLeaveEvent(QEvent::Leave);
        QApplication::sendEvent(&card, &initialLeaveEvent);
        QVERIFY(!corePanel->isVisible());

        QEnterEvent enterEvent(QPointF(10, 10), QPointF(10, 10), QPointF(10, 10));
        QApplication::sendEvent(&card, &enterEvent);
        QVERIFY(corePanel->isVisible());
        QVERIFY(card.height() > 38);
        const auto coreProgresses = corePanel->findChildren<QProgressBar *>(QStringLiteral("metricCoreProgress"));
        QCOMPARE(coreProgresses.size(), 4);
        for (int index = 1; index < coreProgresses.size(); ++index) {
            QCOMPARE(coreProgresses.at(index)->x(), coreProgresses.first()->x());
            QVERIFY(coreProgresses.at(index)->parentWidget()->y()
                    > coreProgresses.at(index - 1)->parentWidget()->y());
        }
        const int expandedHeight = card.height();
        card.setCoreValues({22.0, 44.0, 66.0, 88.0});
        QCoreApplication::processEvents();
        QCOMPARE(card.height(), expandedHeight);
        const auto updatedProgresses = corePanel->findChildren<QProgressBar *>(QStringLiteral("metricCoreProgress"));
        QCOMPARE(updatedProgresses.size(), coreProgresses.size());
        for (int index = 0; index < coreProgresses.size(); ++index) {
            QCOMPARE(updatedProgresses.at(index), coreProgresses.at(index));
        }
        QCOMPARE(updatedProgresses.last()->value(), 88);

        QEvent leaveEvent(QEvent::Leave);
        QApplication::sendEvent(&card, &leaveEvent);
        QVERIFY(!corePanel->isVisible());
        QCOMPARE(card.height(), 38);
    }

    void parsesLinuxMetricsAndCalculatesCpuDelta()
    {
        const QByteArray firstPayload =
            "__CPU__\n"
            "cpu 100 0 50 800 10 0 0 0\n"
            "cpu0 25 0 13 200 2 0 0 0\n"
            "cpu1 25 0 12 200 3 0 0 0\n"
            "cpu2 25 0 13 200 2 0 0 0\n"
            "cpu3 25 0 12 200 3 0 0 0\n"
            "__MEM__\n"
            "MemTotal:       1048576 kB\n"
            "MemAvailable:    419430 kB\n"
            "__LOAD__\n"
            "1.25 0.90 0.75 1/100 123\n"
            "__CORES__\n"
            "4\n"
            "__DISK__\n"
            "Filesystem 1024-blocks Used Available Capacity Mounted on\n"
            "/dev/vda1 104857600 76546048 28311552 73% /\n"
            "tmpfs 2048000 1024 2046976 1% /run\n"
            "__UPTIME__\n86461.23 0.0\n"
            "__NET__\n"
            "eth0: 100000 1 0 0 0 0 0 0 50000 1 0 0 0 0 0 0\n"
            "lo: 1000 1 0 0 0 0 0 0 1000 1 0 0 0 0 0 0\n"
            "__PROC__\n"
            "101 root 12.5 4.0 2048 nginx\n"
            "202 mysql 3.0 18.0 4096 mysqld\n";
        const QByteArray secondPayload =
            "__CPU__\n"
            "cpu 150 0 70 850 10 0 0 0\n"
            "cpu0 40 0 18 210 2 0 0 0\n"
            "cpu1 35 0 17 215 3 0 0 0\n"
            "cpu2 45 0 15 205 5 0 0 0\n"
            "cpu3 30 0 20 217 3 0 0 0\n"
            "__MEM__\n"
            "MemTotal:       1048576 kB\n"
            "MemAvailable:    419430 kB\n"
            "__LOAD__\n"
            "1.50 1.00 0.80 1/100 456\n"
            "__CORES__\n"
            "4\n"
            "__DISK__\n"
            "Filesystem 1024-blocks Used Available Capacity Mounted on\n"
            "/dev/vda1 104857600 76546048 28311552 73% /\n"
            "tmpfs 2048000 2048 2045952 1% /run\n"
            "__UPTIME__\n86462.23 0.0\n"
            "__NET__\n"
            "eth0: 112288 1 0 0 0 0 0 0 56144 1 0 0 0 0 0 0\n"
            "lo: 2000 1 0 0 0 0 0 0 2000 1 0 0 0 0 0 0\n"
            "__PROC__\n"
            "101 root 14.5 4.2 2200 nginx\n";

        noxshell::LinuxMetricsSnapshot first;
        noxshell::LinuxMetricsSnapshot second;
        QString error;
        QVERIFY2(noxshell::LinuxMetricsParser::parse(firstPayload, first, &error), qPrintable(error));
        QVERIFY2(noxshell::LinuxMetricsParser::parse(secondPayload, second, &error), qPrintable(error));
        first.capturedAt = QDateTime::fromMSecsSinceEpoch(1000);
        second.capturedAt = QDateTime::fromMSecsSinceEpoch(2000);

        const auto baseline = noxshell::LinuxMetricsParser::calculate(first);
        QVERIFY(!baseline.cpuReady);
        const auto sample = noxshell::LinuxMetricsParser::calculate(second, &first);
        QVERIFY(sample.cpuReady);
        QVERIFY(qAbs(sample.cpuPercent - 58.333) < 0.01);
        QVERIFY(qAbs(sample.kernelPercent - 16.666) < 0.01);
        QCOMPARE(sample.cpuCorePercents.size(), 4);
        QVERIFY(qAbs(sample.cpuCorePercents.at(0) - 66.666) < 0.01);
        QVERIFY(qAbs(sample.cpuCorePercents.at(1) - 50.0) < 0.01);
        QVERIFY(qAbs(sample.memoryPercent - 60.0) < 0.01);
        QCOMPARE(sample.cpuCoreCount, 4);
        QCOMPARE(sample.primaryDisk.fileSystem, QStringLiteral("/dev/vda1"));
        QCOMPARE(sample.primaryDisk.usagePercent, 73);
        QCOMPARE(sample.primaryDisk.totalBytes, quint64{104857600} * 1024);
        QCOMPARE(sample.uptimeSeconds, quint64{86462});
        QCOMPARE(sample.disks.size(), 2);
        QCOMPARE(sample.networkRates.size(), 2);
        QCOMPARE(sample.networkRates.first().interfaceName, QStringLiteral("eth0"));
        QCOMPARE(sample.networkRates.first().receivedBytesPerSecond, 12288.0);
        QCOMPARE(sample.networkRates.first().transmittedBytesPerSecond, 6144.0);
        QCOMPARE(sample.processes.size(), 1);
        QCOMPARE(sample.processes.first().command, QStringLiteral("nginx"));
        QCOMPARE(sample.processes.first().residentBytes, quint64{2200} * 1024);
    }

    void systemDetailPanelShowsRealtimeSystemData()
    {
        noxshell::ui::SystemDetailPanel panel;
        noxshell::MetricSample sample;
        sample.cpuReady = true;
        sample.cpuPercent = 27.5;
        sample.cpuCoreCount = 8;
        sample.memoryUsedBytes = 6ULL * 1024 * 1024 * 1024;
        sample.memoryTotalBytes = 16ULL * 1024 * 1024 * 1024;
        sample.memoryPercent = 37.5;
        sample.uptimeSeconds = 90061;
        sample.networkRates = {
            {QStringLiteral("lo"), 1024.0, 1024.0},
            {QStringLiteral("eth0"), 64.0 * 1024.0, 32.0 * 1024.0},
        };
        sample.processes = {
            {17, QStringLiteral("root"), 24.0, 2.0, 64ULL * 1024 * 1024, QStringLiteral("agent")},
            {33, QStringLiteral("www"), 4.0, 8.0, 256ULL * 1024 * 1024, QStringLiteral("worker")},
            {41, QStringLiteral("root"), 3.0, 4.0, 128ULL * 1024 * 1024, QStringLiteral("cache")},
            {52, QStringLiteral("ops"), 2.0, 3.0, 96ULL * 1024 * 1024, QStringLiteral("logger")},
            {63, QStringLiteral("ops"), 1.0, 1.0, 48ULL * 1024 * 1024, QStringLiteral("watcher")},
            {74, QStringLiteral("root"), 0.5, 0.5, 32ULL * 1024 * 1024, QStringLiteral("helper")},
        };
        sample.disks = {
            {QStringLiteral("/dev/vda1"), QStringLiteral("/"), 100ULL * 1024 * 1024 * 1024,
                40ULL * 1024 * 1024 * 1024, 60ULL * 1024 * 1024 * 1024, 40},
            {QStringLiteral("tmpfs"), QStringLiteral("/dev"), 2ULL * 1024 * 1024 * 1024,
                1ULL * 1024 * 1024 * 1024, 1ULL * 1024 * 1024 * 1024, 50},
            {QStringLiteral("tmpfs"), QStringLiteral("/run"), 2ULL * 1024 * 1024 * 1024,
                1ULL * 1024 * 1024 * 1024, 1ULL * 1024 * 1024 * 1024, 50},
            {QStringLiteral("/dev/vdb1"), QStringLiteral("/data"), 200ULL * 1024 * 1024 * 1024,
                20ULL * 1024 * 1024 * 1024, 180ULL * 1024 * 1024 * 1024, 10},
            {QStringLiteral("tmpfs"), QStringLiteral("/run/user/0"), 512ULL * 1024 * 1024,
                256ULL * 1024 * 1024, 256ULL * 1024 * 1024, 50},
        };
        panel.setSample(sample);
        auto *interfaces = panel.findChild<QComboBox *>(QStringLiteral("networkInterfaceCombo"));
        auto *upload = panel.findChild<QLabel *>(QStringLiteral("networkUploadRate"));
        auto *processes = panel.findChild<QTreeWidget *>(QStringLiteral("realtimeProcessList"));
        auto *fileSystems = panel.findChild<QTreeWidget *>(QStringLiteral("fileSystemUsageList"));
        QVERIFY(interfaces);
        QVERIFY(upload);
        QVERIFY(processes);
        QVERIFY(fileSystems);
        QVERIFY(panel.findChild<QFrame *>(QStringLiteral("networkSectionCard")));
        QVERIFY(panel.findChild<QFrame *>(QStringLiteral("processSectionCard")));
        QVERIFY(panel.findChild<QFrame *>(QStringLiteral("fileSystemSectionCard")));
        QCOMPARE(interfaces->count(), 3);
        QCOMPARE(interfaces->currentText(), QStringLiteral("全部网卡"));
        QVERIFY(interfaces->width() >= 112);
        QVERIFY(upload->text().contains(QStringLiteral("33.0 KB/s")));
        QVERIFY(panel.findChild<QWidget *>(QStringLiteral("networkRateChart")));
        interfaces->setCurrentIndex(interfaces->findData(QStringLiteral("eth0")));
        QVERIFY(upload->text().contains(QStringLiteral("32.0 KB/s")));
        QCOMPARE(processes->topLevelItemCount(), 4);
        QCOMPARE(processes->topLevelItem(0)->text(0), QStringLiteral("agent"));
        QCOMPARE(processes->verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
        QCOMPARE(fileSystems->topLevelItemCount(), 5);
        QCOMPARE(fileSystems->topLevelItem(0)->text(0), QStringLiteral("/"));
        QCOMPARE(fileSystems->verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
        QVERIFY(fileSystems->height() >= 24 + fileSystems->topLevelItemCount() * 21);
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
            QCOMPARE(servers.first().group, QStringLiteral("test"));
            QCOMPARE(servers.first().credentialRef, QStringLiteral("server/test-reference"));
            QVERIFY(repository.loadServerGroups().contains(QStringLiteral("test")));
            QVERIFY(repository.saveServerGroup(QStringLiteral("空分组")));
            QVERIFY(repository.loadServerGroups().contains(QStringLiteral("空分组")));
            QVERIFY(repository.renameServerGroup(QStringLiteral("test"), QStringLiteral("生产环境")));
            QCOMPARE(repository.loadServers().first().group, QStringLiteral("生产环境"));
            QVERIFY(repository.deleteServerGroup(QStringLiteral("生产环境")));
            QCOMPARE(repository.loadServers().first().group, QString{});
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

    void commandHistoryPersistsFavoritesNotesAndManagement()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        noxshell::ServerRepository repository(directory.filePath(QStringLiteral("command-history.sqlite3")), false);
        QVERIFY2(repository.initialize(), qPrintable(repository.lastError()));
        noxshell::ServerProfile profile;
        profile.name = QStringLiteral("history-host");
        profile.host = QStringLiteral("192.0.2.20");
        profile.user = QStringLiteral("root");
        QVERIFY(repository.saveServer(profile));
        noxshell::ServerProfile secondProfile;
        secondProfile.name = QStringLiteral("other-host");
        secondProfile.host = QStringLiteral("192.0.2.21");
        secondProfile.user = QStringLiteral("ops");
        QVERIFY(repository.saveServer(secondProfile));

        const auto older = QDateTime::currentDateTime().addSecs(-60);
        const auto newer = QDateTime::currentDateTime();
        QVERIFY(repository.recordCommand(profile.id, QStringLiteral("  pwd  "), older));
        QVERIFY(repository.recordCommand(profile.id, QStringLiteral("ls -la"), older));
        QVERIFY(repository.recordCommand(profile.id, QStringLiteral("pwd"), newer));
        auto entries = repository.loadCommandHistory();
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries.first().command, QStringLiteral("pwd"));
        QCOMPARE(entries.first().serverName, QStringLiteral("history-host"));
        const auto pwdId = entries.first().id;
        QVERIFY(repository.setCommandFavorite(pwdId, true));
        QVERIFY(repository.setCommandNote(pwdId, QStringLiteral("查看当前目录")));
        // The same command remains one device-wide favorite when executed on
        // another host; only its latest source context is updated.
        QVERIFY(repository.recordCommand(secondProfile.id, QStringLiteral("pwd"), newer.addSecs(1)));
        entries = repository.loadCommandHistory(true);
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first().note, QStringLiteral("查看当前目录"));
        QVERIFY(entries.first().favorite);
        QCOMPARE(entries.first().serverName, QStringLiteral("other-host"));
        QVERIFY(repository.clearCommandHistory());
        entries = repository.loadCommandHistory();
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first().command, QStringLiteral("pwd"));
        QCOMPARE(entries.first().note, QStringLiteral("查看当前目录"));
        QVERIFY(entries.first().favorite);
        QCOMPARE(repository.loadCommandHistory(true).size(), 1);

        QVERIFY(repository.clearCommandFavorites());
        QVERIFY(repository.loadCommandHistory(true).isEmpty());
        entries = repository.loadCommandHistory();
        QCOMPARE(entries.size(), 1);
        QVERIFY(!entries.first().favorite);
        QVERIFY(repository.recordCommand(profile.id, QStringLiteral("whoami")));
        entries = repository.loadCommandHistory();
        QCOMPARE(entries.size(), 2);
        QVERIFY(repository.deleteCommandHistory(entries.last().id));
        QCOMPARE(repository.loadCommandHistory().size(), 1);
        QVERIFY(repository.clearCommandHistory());
        QVERIFY(repository.loadCommandHistory().isEmpty());
    }

    void commandHistoryMigratesExistingPerServerDataWithoutLoss()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto databasePath = directory.filePath(QStringLiteral("legacy-command-history.sqlite3"));
        QString firstServerId;
        QString secondServerId;
        {
            noxshell::ServerRepository repository(databasePath, false);
            QVERIFY2(repository.initialize(), qPrintable(repository.lastError()));
            noxshell::ServerProfile first;
            first.name = QStringLiteral("first-host");
            first.host = QStringLiteral("192.0.2.30");
            first.user = QStringLiteral("root");
            QVERIFY(repository.saveServer(first));
            firstServerId = first.id;
            noxshell::ServerProfile second;
            second.name = QStringLiteral("second-host");
            second.host = QStringLiteral("192.0.2.31");
            second.user = QStringLiteral("root");
            QVERIFY(repository.saveServer(second));
            secondServerId = second.id;
        }

        const auto seedConnection = QStringLiteral("legacy-command-history-seed");
        {
            auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), seedConnection);
            database.setDatabaseName(databasePath);
            QVERIFY(database.open());
            QSqlQuery query(database);
            QVERIFY(query.exec(QStringLiteral("DROP TABLE command_history")));
            QVERIFY(query.exec(QStringLiteral(
                "CREATE TABLE command_history (id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "server_id TEXT NOT NULL,command TEXT NOT NULL,note TEXT NOT NULL DEFAULT '',"
                "favorite INTEGER NOT NULL DEFAULT 0,executed_at TEXT NOT NULL,"
                "UNIQUE(server_id,command),FOREIGN KEY(server_id) REFERENCES servers(id) ON DELETE CASCADE)")));
            query.prepare(QStringLiteral(
                "INSERT INTO command_history(server_id,command,note,favorite,executed_at) VALUES(?,?,?,?,?)"));
            const auto insert = [&](const QString &serverId, const QString &command, const QString &note,
                                    bool favorite, const QString &time) {
                query.bindValue(0, serverId);
                query.bindValue(1, command);
                query.bindValue(2, note);
                query.bindValue(3, favorite ? 1 : 0);
                query.bindValue(4, time);
                return query.exec();
            };
            QVERIFY(insert(firstServerId, QStringLiteral("systemctl restart app"),
                QStringLiteral("发布后重启"), true, QStringLiteral("2026-08-27T09:00:00.000Z")));
            QVERIFY(insert(secondServerId, QStringLiteral("systemctl restart app"),
                QStringLiteral(""), false, QStringLiteral("2026-08-28T09:00:00.000Z")));
            QVERIFY(insert(firstServerId, QStringLiteral("uptime"),
                QStringLiteral(""), false, QStringLiteral("2026-08-27T08:00:00.000Z")));
            database.close();
        }
        QSqlDatabase::removeDatabase(seedConnection);

        noxshell::ServerRepository migrated(databasePath, false);
        QVERIFY2(migrated.initialize(), qPrintable(migrated.lastError()));
        const auto entries = migrated.loadCommandHistory();
        QCOMPARE(entries.size(), 2);
        const auto restart = std::find_if(entries.cbegin(), entries.cend(), [](const auto &entry) {
            return entry.command == QStringLiteral("systemctl restart app");
        });
        QVERIFY(restart != entries.cend());
        QVERIFY(restart->favorite);
        QCOMPARE(restart->note, QStringLiteral("发布后重启"));
        QCOMPARE(restart->serverName, QStringLiteral("second-host"));
        QVERIFY(migrated.deleteServer(secondServerId));
        QCOMPARE(migrated.loadCommandHistory().size(), 2);
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
        QTRY_VERIFY_WITH_TIMEOUT(!outputSpy.isEmpty(), 10000);
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
        auto *backButton = panel.findChild<QToolButton *>(QStringLiteral("fileBackButton"));
        auto *upButton = panel.findChild<QToolButton *>(QStringLiteral("fileUpButton"));
        auto *refreshButton = panel.findChild<QToolButton *>(QStringLiteral("fileRefreshButton"));
        auto *contextDownload = panel.findChild<QAction *>(QStringLiteral("fileContextDownloadAction"));
        auto *newFileAction = panel.findChild<QAction *>(QStringLiteral("fileNewFileAction"));
        auto *newDirectoryAction = panel.findChild<QAction *>(QStringLiteral("fileNewDirectoryAction"));
        auto *queueButton = panel.findChild<QToolButton *>(QStringLiteral("transferQueueButton"));
        auto *queueMenu = panel.findChild<QMenu *>(QStringLiteral("transferQueueMenu"));
        auto *transferPanel = panel.findChild<noxshell::ui::TransferQueuePanel *>(QStringLiteral("transferQueuePanel"));
        auto *fileToolbar = panel.findChild<QWidget *>(QStringLiteral("fileToolbar"));
        auto *fileStatus = panel.findChild<QLabel *>(QStringLiteral("fileStatusLabel"));
        auto *fileLoadingOverlay = panel.findChild<QWidget *>(QStringLiteral("fileLoadingOverlay"));
        auto *fileLoadingTitle = panel.findChild<QLabel *>(QStringLiteral("fileLoadingTitle"));
        auto *fileLoadingDetail = panel.findChild<QLabel *>(QStringLiteral("fileLoadingDetail"));
        auto *fileLoadingProgress = panel.findChild<QProgressBar *>(QStringLiteral("fileLoadingProgress"));
        QVERIFY(tree);
        QVERIFY(directoryTree);
        QVERIFY(browserSplitter);
        QVERIFY(pathEdit);
        QVERIFY(backButton);
        QVERIFY(upButton);
        QVERIFY(refreshButton);
        QVERIFY(contextDownload);
        QVERIFY(newFileAction);
        QVERIFY(newDirectoryAction);
        QVERIFY(queueButton);
        QVERIFY(queueMenu);
        QVERIFY(transferPanel);
        auto *transferList = transferPanel->findChild<QListWidget *>(QStringLiteral("transferQueueList"));
        auto *transferEmpty = transferPanel->findChild<QLabel *>(QStringLiteral("transferQueueEmpty"));
        QVERIFY(transferList);
        QVERIFY(transferEmpty);
        QVERIFY(transferPanel->minimumHeight() >= 220);
        QVERIFY(transferList->isHidden());
        QVERIFY(!transferEmpty->isHidden());
        QVERIFY(fileToolbar);
        QVERIFY(fileStatus);
        QVERIFY(fileLoadingOverlay);
        QVERIFY(fileLoadingTitle);
        QVERIFY(fileLoadingDetail);
        QVERIFY(fileLoadingProgress);
        QCOMPARE(fileLoadingTitle->text(), QStringLiteral("正在加载文件"));
        QCOMPARE(fileLoadingProgress->minimum(), 0);
        QCOMPARE(fileLoadingProgress->maximum(), 0);
        QVERIFY(!queueButton->icon().isNull());
        for (auto *button : {backButton, upButton, refreshButton, queueButton}) {
            QCOMPARE(button->size(), QSize(26, 26));
            QCOMPARE(button->iconSize(), QSize(16, 16));
            QVERIFY(!button->icon().isNull());
        }
        QCOMPARE(fileStatus->parentWidget(), fileToolbar);
        QCOMPARE(pathEdit->parentWidget(), fileToolbar);
        QCOMPARE(fileToolbar->height(), 40);
        QCOMPARE(pathEdit->height(), 26);
        QVERIFY(std::abs(pathEdit->geometry().center().y() - fileToolbar->rect().center().y()) <= 1);
        QVERIFY(pathEdit->geometry().top() > fileToolbar->rect().top());
        QVERIFY(pathEdit->geometry().bottom() < fileToolbar->rect().bottom());
        QVERIFY(!panel.findChild<QPushButton *>(QStringLiteral("fileUploadButton")));
        QVERIFY(!panel.findChild<QPushButton *>(QStringLiteral("fileDownloadButton")));
        QVERIFY(!panel.findChild<QPushButton *>(QStringLiteral("fileNewDirectoryButton")));
        QVERIFY(!panel.findChild<QToolButton *>(QStringLiteral("fileMoreButton")));
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
        QVERIFY(!fileLoadingOverlay->isVisible());
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
        auto *fileSearchMarkers = fileEditor->findChild<noxshell::ui::SearchMarkerScrollBar *>(
            QStringLiteral("remoteFileSearchMarkerBar"));
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
        QVERIFY(fileSearchMarkers);
        QVERIFY(!fileEditor->findChild<QPushButton *>(QStringLiteral("remoteFileEditorSave")));
        QVERIFY(!fileEditor->findChild<QPushButton *>(QStringLiteral("remoteFileEditorClose")));
        QCOMPARE(editorTabs->count(), 1);
        QVERIFY(editorTabs->tabText(0).contains(QStringLiteral("demo-sftp")));
        QVERIFY(editorTabs->tabText(0).contains(firstFileName));
        QCOMPARE(editorTabs->height(), 30);
        QVERIFY(!editorTabs->tabButton(0, QTabBar::LeftSide));
        auto *firstEditorCloseContainer = editorTabs->tabButton(0, QTabBar::RightSide);
        QVERIFY(firstEditorCloseContainer);
        auto *firstEditorCloseButton = firstEditorCloseContainer->findChild<QToolButton *>(
            QStringLiteral("remoteFileTabCloseButton"));
        QVERIFY(firstEditorCloseButton);
        QVERIFY(firstEditorCloseButton->mapTo(editorTabs, firstEditorCloseButton->rect().center()).x()
            > editorTabs->tabRect(0).center().x());
        QTRY_VERIFY_WITH_TIMEOUT(editorText->isEnabled(), 1000);
        QVERIFY(!editorText->toPlainText().isEmpty());
        QVERIFY(!findPanel->isVisible());

        editorText->setPlainText(QStringLiteral("alpha beta\nother line\nalpha\n"));
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
        QTRY_COMPARE_WITH_TIMEOUT(fileSearchMarkers->searchMarkerCount(), 2, 1000);
        QCOMPARE(fileSearchMarkers->currentSearchMarker(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(editorText->extraSelections().size(), 3, 1000);
        int allMatchHighlights = 0;
        int currentMatchHighlights = 0;
        for (const auto &selection : editorText->extraSelections()) {
            const auto background = selection.format.background().color();
            if (background == QColor(QStringLiteral("#FFF36A"))) ++allMatchHighlights;
            if (background == QColor(QStringLiteral("#FFB938"))) ++currentMatchHighlights;
        }
        QCOMPARE(allMatchHighlights, 1);
        QCOMPARE(currentMatchHighlights, 1);
        QTest::mouseClick(fileSearchMarkers, Qt::LeftButton, Qt::NoModifier,
            fileSearchMarkers->searchMarkerRect(1).center());
        QCOMPARE(findStatus->text(), QStringLiteral("2 / 2"));
        QCOMPARE(fileSearchMarkers->currentSearchMarker(), 1);
        QTest::mouseClick(fileSearchMarkers, Qt::LeftButton, Qt::NoModifier,
            fileSearchMarkers->searchMarkerRect(0).center());
        QCOMPARE(findStatus->text(), QStringLiteral("1 / 2"));
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
        QCOMPARE(editorText->extraSelections().size(), 1);
        QCOMPARE(fileSearchMarkers->searchMarkerCount(), 0);

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
        QVERIFY(!editorTabs->tabButton(1, QTabBar::LeftSide));
        QVERIFY(editorTabs->tabButton(1, QTabBar::RightSide));
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
        QTRY_COMPARE_WITH_TIMEOUT(transferList->count(), 1, 1000);
        QVERIFY(!transferList->isHidden());
        QVERIFY(transferEmpty->isHidden());
        QVERIFY(transferList->item(0)->sizeHint().height() >= 78);
        auto *transferRow = transferList->itemWidget(transferList->item(0));
        QVERIFY(transferRow);
        auto *transferName = transferRow->findChild<QLabel *>(QStringLiteral("transferName"));
        auto *transferAmount = transferRow->findChild<QLabel *>(QStringLiteral("transferAmount"));
        auto *transferPath = transferRow->findChild<QLabel *>(QStringLiteral("transferPath"));
        auto *transferState = transferRow->findChild<QLabel *>(QStringLiteral("transferState"));
        auto *transferProgress = transferRow->findChild<QProgressBar *>(QStringLiteral("transferProgress"));
        QVERIFY(transferName);
        QVERIFY(transferAmount);
        QVERIFY(transferPath);
        QVERIFY(transferState);
        QVERIFY(transferProgress);
        QCOMPARE(transferName->text(), QStringLiteral("dragged.txt"));
        QVERIFY(transferAmount->text().contains(QStringLiteral("14 B / 14 B")));
        QVERIFY(transferPath->text().contains(QStringLiteral("/var/www/app/dragged.txt")));
        QCOMPARE(transferState->text(), QStringLiteral("已完成"));
        QCOMPARE(transferProgress->value(), 100);
        QTRY_VERIFY_WITH_TIMEOUT(queueMenu->isVisible(), 1000);
        queueMenu->hide();

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

    void filePermissionDialogMapsModesAndRecursiveScope()
    {
        noxshell::RemoteFileEntry file;
        file.name = QStringLiteral("deploy.sh");
        file.path = QStringLiteral("/var/www/app/deploy.sh");
        file.permissions = 0100640;
        noxshell::ui::FilePermissionDialog fileDialog(file);
        QCOMPARE(fileDialog.permissions(), quint32{0640});
        auto *ownerExecute = fileDialog.findChild<QCheckBox *>(QStringLiteral("permissionCheck_0_2"));
        auto *otherRead = fileDialog.findChild<QCheckBox *>(QStringLiteral("permissionCheck_2_0"));
        QVERIFY(ownerExecute);
        QVERIFY(otherRead);
        ownerExecute->setChecked(true);
        otherRead->setChecked(true);
        QCOMPARE(fileDialog.permissions(), quint32{0744});
        QVERIFY(!fileDialog.recursive());

        noxshell::RemoteFileEntry directory = file;
        directory.name = QStringLiteral("app");
        directory.path = QStringLiteral("/var/www/app");
        directory.directory = true;
        directory.permissions = 0040755;
        noxshell::ui::FilePermissionDialog directoryDialog(directory);
        auto *recursive = directoryDialog.findChild<QCheckBox *>(QStringLiteral("recursivePermissionCheck"));
        auto *filesOnly = directoryDialog.findChild<QRadioButton *>(QStringLiteral("permissionScopeFiles"));
        QVERIFY(recursive);
        QVERIFY(filesOnly);
        QVERIFY(!filesOnly->isEnabled());
        recursive->setChecked(true);
        QVERIFY(filesOnly->isEnabled());
        filesOnly->setChecked(true);
        QVERIFY(directoryDialog.recursive());
        QCOMPARE(directoryDialog.scope(), noxshell::PermissionScope::FilesOnly);
    }

    void filePanelChangesPermissionsFromContextAction()
    {
        noxshell::SshSession session(nullptr, nullptr);
        noxshell::ui::FilePanel panel(&session);
        panel.resize(920, 640);
        panel.show();
        noxshell::ServerProfile profile;
        profile.id = QStringLiteral("demo-permission-panel");
        profile.name = QStringLiteral("demo-permission-panel");
        profile.user = QStringLiteral("root");
        profile.connectionMode = noxshell::ConnectionMode::Demo;
        panel.setServer(profile);
        session.connectTo(profile);

        auto *tree = panel.findChild<QTreeWidget *>(QStringLiteral("remoteFileTree"));
        auto *action = panel.findChild<QAction *>(QStringLiteral("filePermissionsAction"));
        QVERIFY(tree);
        QVERIFY(action);
        QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 8, 1000);
        QTreeWidgetItem *target = nullptr;
        for (int row = 0; row < tree->topLevelItemCount(); ++row) {
            if (tree->topLevelItem(row)->text(0) == QStringLiteral("deploy.sh")) target = tree->topLevelItem(row);
        }
        QVERIFY(target);
        tree->setCurrentItem(target);
        QVERIFY(action->isEnabled());

        QTimer::singleShot(0, [] {
            auto *dialog = qobject_cast<noxshell::ui::FilePermissionDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto *ownerExecute = dialog->findChild<QCheckBox *>(QStringLiteral("permissionCheck_0_2"));
            QVERIFY(ownerExecute);
            ownerExecute->setChecked(true);
            dialog->accept();
        });
        action->trigger();
        QTRY_VERIFY_WITH_TIMEOUT([tree] {
            for (int row = 0; row < tree->topLevelItemCount(); ++row) {
                auto *item = tree->topLevelItem(row);
                if (item->text(0) == QStringLiteral("deploy.sh")) return item->text(4) == QStringLiteral("-rwxr--r--");
            }
            return false;
        }(), 1000);
    }

    void filePanelDeletesMultipleSelectedEntries()
    {
        noxshell::SshSession session(nullptr, nullptr);
        QSignalSpy operationSpy(&session, &noxshell::SshSession::fileOperationFinished);
        noxshell::ui::FilePanel panel(&session);
        panel.resize(920, 640);
        panel.show();
        noxshell::ServerProfile profile;
        profile.id = QStringLiteral("demo-batch-delete-panel");
        profile.name = QStringLiteral("demo-batch-delete-panel");
        profile.user = QStringLiteral("root");
        profile.connectionMode = noxshell::ConnectionMode::Demo;
        panel.setServer(profile);
        session.connectTo(profile);

        auto *tree = panel.findChild<QTreeWidget *>(QStringLiteral("remoteFileTree"));
        auto *removeAction = panel.findChild<QAction *>(QStringLiteral("fileRemoveAction"));
        QVERIFY(tree);
        QVERIFY(removeAction);
        QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 8, 1000);
        const QStringList names{QStringLiteral(".env.production"), QStringLiteral("deploy.sh"), QStringLiteral("README.md")};
        for (int row = 0; row < tree->topLevelItemCount(); ++row) {
            auto *item = tree->topLevelItem(row);
            if (names.contains(item->text(0))) item->setSelected(true);
        }
        QCOMPARE(tree->selectedItems().size(), 3);
        QVERIFY(removeAction->isEnabled());

        QTimer::singleShot(0, [] {
            auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            QVERIFY(dialog->text().contains(QStringLiteral("3 个项目")));
            auto *yesButton = dialog->button(QMessageBox::Yes);
            QVERIFY(yesButton);
            yesButton->click();
        });
        removeAction->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 3, 1500);
        QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 5, 1500);
        for (int row = 0; row < tree->topLevelItemCount(); ++row) {
            QVERIFY(!names.contains(tree->topLevelItem(row)->text(0)));
        }
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
        QCOMPARE(qvariant_cast<noxshell::RemoteFileOperation>(operationSpy.last().at(0)), noxshell::RemoteFileOperation::MakeDirectory);

        session.changePermissions(created, 0700);
        QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 2, 1000);
        QCOMPARE(qvariant_cast<noxshell::RemoteFileOperation>(operationSpy.last().at(0)), noxshell::RemoteFileOperation::ChangePermissions);
        const auto permissionListingCount = directorySpy.count();
        session.listDirectory(base);
        QTRY_COMPARE_WITH_TIMEOUT(directorySpy.count(), permissionListingCount + 1, 1000);
        auto permissionEntries = qvariant_cast<noxshell::RemoteFileEntries>(directorySpy.last().at(1));
        auto permissionEntry = std::find_if(permissionEntries.cbegin(), permissionEntries.cend(), [&created](const noxshell::RemoteFileEntry &entry) {
            return entry.path == created;
        });
        QVERIFY(permissionEntry != permissionEntries.cend());
        QCOMPARE(permissionEntry->permissions & 0777U, quint32{0700});

        session.renamePath(created, renamed);
        QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 3, 1000);
        session.removePath(renamed, true);
        QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 4, 1000);

        const auto remoteFile = base + QStringLiteral("/release.txt");
        session.uploadFile(uploadSource, remoteFile);
        QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 5, 1000);
        const auto uploadListingCount = directorySpy.count();
        session.listDirectory(base);
        QTRY_COMPARE_WITH_TIMEOUT(directorySpy.count(), uploadListingCount + 1, 1000);
        const auto entries = qvariant_cast<noxshell::RemoteFileEntries>(directorySpy.last().at(1));
        const auto uploaded = std::find_if(entries.cbegin(), entries.cend(), [&remoteFile](const noxshell::RemoteFileEntry &entry) {
            return entry.path == remoteFile;
        });
        QVERIFY(uploaded != entries.cend());
        QCOMPARE(uploaded->size, quint64{15});

        const auto downloadTarget = directory.filePath(QStringLiteral("downloaded.txt"));
        session.downloadFile(remoteFile, downloadTarget);
        QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 6, 1000);
        QFile downloaded(downloadTarget);
        QVERIFY(downloaded.open(QIODevice::ReadOnly));
        QCOMPARE(downloaded.readAll(), QByteArray("release payload"));

        session.removePath(remoteFile, false);
        QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 7, 1000);
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

        auto *hosts = window.findChild<QTreeWidget *>(QStringLiteral("hostList"));
        QVERIFY(hosts);
        QCOMPARE(hosts->topLevelItemCount(), 2);
        QCOMPARE(hosts->topLevelItem(0)->childCount() + hosts->topLevelItem(1)->childCount(), 5);
        QTRY_VERIFY_WITH_TIMEOUT(hosts->currentItem() != nullptr, 1000);
        QVERIFY(hosts->currentItem()->parent() != nullptr);

        auto *sidebar = window.findChild<noxshell::ui::HostSidebar *>(QStringLiteral("hostSidebar"));
        auto *sidebarToggle = window.findChild<QToolButton *>(QStringLiteral("sidebarToggleButton"));
        auto *monitorToggle = window.findChild<QToolButton *>(QStringLiteral("monitorToggleButton"));
        auto *settingsButton = window.findChild<QToolButton *>(QStringLiteral("terminalSettingsButton"));
        auto *themeButton = window.findChild<QToolButton *>(QStringLiteral("themeModeButton"));
        auto *windowToolbar = window.findChild<QToolBar *>(QStringLiteral("windowControlsToolbar"));
        QVERIFY(sidebar);
        QVERIFY(sidebarToggle);
        QVERIFY(monitorToggle);
        QVERIFY(settingsButton);
        QVERIFY(themeButton);
        QVERIFY(windowToolbar);
        QCOMPARE(sidebarToggle->parentWidget(), monitorToggle->parentWidget());
        QCOMPARE(sidebarToggle->parentWidget(), settingsButton->parentWidget());
        QCOMPARE(themeButton->parentWidget(), settingsButton->parentWidget());
        QVERIFY(themeButton->menu());
        auto *systemTheme = themeButton->menu()->findChild<QAction *>(QStringLiteral("themeSystemAction"));
        auto *lightTheme = themeButton->menu()->findChild<QAction *>(QStringLiteral("themeLightAction"));
        auto *darkTheme = themeButton->menu()->findChild<QAction *>(QStringLiteral("themeDarkAction"));
        QVERIFY(systemTheme);
        QVERIFY(lightTheme);
        QVERIFY(darkTheme);
        QCOMPARE(static_cast<int>(systemTheme->isChecked()) + static_cast<int>(lightTheme->isChecked())
                + static_cast<int>(darkTheme->isChecked()), 1);
        QSettings themeSettings;
        const bool hadStoredTheme = themeSettings.contains(QStringLiteral("ui/themeMode"));
        const auto previousStoredTheme = themeSettings.value(QStringLiteral("ui/themeMode"));
        darkTheme->trigger();
        QVERIFY(QApplication::instance()->property("noxshellDarkTheme").toBool());
        QCOMPARE(themeSettings.value(QStringLiteral("ui/themeMode")).toString(), QStringLiteral("dark"));
        QVERIFY(themeButton->toolTip().contains(QStringLiteral("暗色")));
        lightTheme->trigger();
        QVERIFY(!QApplication::instance()->property("noxshellDarkTheme").toBool());
        QCOMPARE(themeSettings.value(QStringLiteral("ui/themeMode")).toString(), QStringLiteral("light"));
        if (hadStoredTheme) {
            themeSettings.setValue(QStringLiteral("ui/themeMode"), previousStoredTheme);
            const auto restored = noxshell::ui::themeModeFromSetting(previousStoredTheme.toString());
            (restored == noxshell::ui::ThemeMode::Dark ? darkTheme
                : restored == noxshell::ui::ThemeMode::Light ? lightTheme : systemTheme)->trigger();
        } else {
            systemTheme->trigger();
            themeSettings.remove(QStringLiteral("ui/themeMode"));
        }
#ifdef Q_OS_WIN
        QVERIFY(window.windowFlags().testFlag(Qt::FramelessWindowHint));
        QVERIFY(window.findChild<QToolButton *>(QStringLiteral("windowMinimizeButton")));
        QVERIFY(window.findChild<QToolButton *>(QStringLiteral("windowMaximizeButton")));
        QVERIFY(window.findChild<QToolButton *>(QStringLiteral("windowCloseButton")));
#endif
        const auto hostItemForName = [hosts](const QString &name) -> QTreeWidgetItem * {
            for (QTreeWidgetItemIterator iterator(hosts); *iterator; ++iterator) {
                auto *item = *iterator;
                auto *row = hosts->itemWidget(item, 0);
                auto *label = row ? row->findChild<QLabel *>(QStringLiteral("hostItemName")) : nullptr;
                if (label && label->text() == name) return item;
            }
            return nullptr;
        };
        const auto currentHostName = [hosts] {
            auto *row = hosts->itemWidget(hosts->currentItem(), 0);
            auto *label = row ? row->findChild<QLabel *>(QStringLiteral("hostItemName")) : nullptr;
            return label ? label->text() : QString{};
        };
        QCOMPARE(window.toolBarArea(windowToolbar), Qt::TopToolBarArea);
        QVERIFY(!window.findChild<QWidget *>(QStringLiteral("topBar")));
        QVERIFY(!window.findChild<QLineEdit *>(QStringLiteral("globalSearch")));
        QVERIFY(!sidebar->isVisible());
        QVERIFY(!sidebarToggle->icon().isNull());
        QVERIFY(!monitorToggle->icon().isNull());
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

        auto *systemDetails = window.findChild<noxshell::ui::SystemDetailPanel *>(QStringLiteral("systemDetailPanel"));
        auto *detailProcesses = window.findChild<QTreeWidget *>(QStringLiteral("realtimeProcessList"));
        auto *metricSummary = window.findChild<QFrame *>(QStringLiteral("monitorMetricSummary"));
        auto *monitorDetails = window.findChild<QWidget *>(QStringLiteral("monitorDetails"));
        QVERIFY(systemDetails);
        QVERIFY(detailProcesses);
        QVERIFY(metricSummary);
        QVERIFY(monitorDetails);
        QVERIFY(!window.findChild<QWidget *>(QStringLiteral("monitorHeading")));
        QVERIFY(!window.findChild<QToolButton *>(QStringLiteral("monitorTrendToggle")));
        const auto metricRows = metricSummary->findChildren<noxshell::ui::MetricCard *>(
            QStringLiteral("metricRow"), Qt::FindDirectChildrenOnly);
        QCOMPARE(metricRows.size(), 3);
        for (auto *metricRow : metricRows) {
            QCOMPARE(metricRow->height(), 38);
            auto *progress = metricRow->findChild<QProgressBar *>();
            QVERIFY(progress);
            QCOMPARE(progress->orientation(), Qt::Horizontal);
            QCOMPARE(progress->height(), 24);
            QVERIFY(progress->isTextVisible());
        }
        metricRows.at(0)->setCoreValues({12.0, 34.0, 56.0, 78.0});
        QEnterEvent metricEnterEvent(QPointF(10, 10), QPointF(10, 10), QPointF(10, 10));
        QApplication::sendEvent(metricRows.at(0), &metricEnterEvent);
        QCoreApplication::processEvents();
        QVERIFY(metricRows.at(0)->height() > 38);
        QVERIFY(metricRows.at(0)->geometry().bottom() < metricRows.at(1)->geometry().top());
        QVERIFY(metricRows.at(1)->geometry().bottom() < metricRows.at(2)->geometry().top());
        QEvent metricLeaveEvent(QEvent::Leave);
        QApplication::sendEvent(metricRows.at(0), &metricLeaveEvent);
        QCoreApplication::processEvents();
        QCOMPARE(metricRows.at(0)->height(), 38);
        QVERIFY(monitorDetails->isVisible());
        QCOMPARE(window.findChildren<noxshell::ui::TransferQueuePanel *>().size(), 0);
        QVERIFY(!window.findChild<QComboBox *>(QStringLiteral("historyRange")));
        QVERIFY(!window.findChild<QWidget *>(QStringLiteral("monitorAlerts")));
        QVERIFY(!window.findChild<QWidget *>(QStringLiteral("cpuTrendChart")));

        QTest::qWait(350);
        QVERIFY(!window.findChild<QLineEdit *>(QStringLiteral("terminalInput")));
        QVERIFY(!window.findChild<noxshell::ui::TerminalView *>(QStringLiteral("terminalOutput")));

        auto *address = window.findChild<QLabel *>(QStringLiteral("serverAddress"));
        auto *onlineBadge = window.findChild<QLabel *>(QStringLiteral("onlineBadge"));
        auto *copyAddress = window.findChild<QToolButton *>(QStringLiteral("copyHostAddressButton"));
        auto *clearTerminal = window.findChild<QAction *>(QStringLiteral("terminalClearAction"));
        QVERIFY(address);
        QVERIFY(onlineBadge);
        QVERIFY(copyAddress);
        QVERIFY(clearTerminal);
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("clearTerminalButton")));
        QCOMPARE(address->text(), QStringLiteral("未选择主机"));
        QVERIFY(onlineBadge->text().contains(QStringLiteral("待连接")));
        QApplication::clipboard()->clear();
        QTest::mouseClick(copyAddress, Qt::LeftButton);
        QVERIFY(QApplication::clipboard()->text().isEmpty());

        QVERIFY(hostItemForName(QStringLiteral("prod-web-01")));
        hosts->itemDoubleClicked(hostItemForName(QStringLiteral("prod-web-01")), 0);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 1000);
        QTRY_VERIFY_WITH_TIMEOUT(!sidebar->isVisible(), 1000);
        QCOMPARE(tabs->tabText(0), QStringLiteral("prod-web-…"));
        QVERIFY(!tabs->tabText(0).contains(QLatin1Char('@')));
        QVERIFY(!recentPage->isVisible());
        QVERIFY(sessionsPage->isVisible());
        auto *input = window.findChild<QLineEdit *>(QStringLiteral("terminalInput"));
        auto *output = window.findChild<noxshell::ui::TerminalView *>(QStringLiteral("terminalOutput"));
        auto *outputContainer = window.findChild<QWidget *>(QStringLiteral("terminalOutputContainer"));
        auto *loadingOverlay = window.findChild<QWidget *>(QStringLiteral("terminalLoadingOverlay"));
        auto *historyButton = window.findChild<QToolButton *>(QStringLiteral("commandHistoryButton"));
        auto *fileToggleButton = window.findChild<QToolButton *>(QStringLiteral("fileWorkspaceToggleButton"));
        auto *historyPanel = window.findChild<noxshell::ui::CommandHistoryPanel *>(
            QStringLiteral("commandHistoryPanel"));
        QVERIFY(input);
        QVERIFY(output);
        QVERIFY(historyButton);
        QVERIFY(fileToggleButton);
        QVERIFY(historyPanel);
        QVERIFY(!historyButton->icon().isNull());
        QVERIFY(!fileToggleButton->icon().isNull());
        QVERIFY(!historyPanel->isVisible());
        QVERIFY(outputContainer);
        QVERIFY(outputContainer->layout());
        QCOMPARE(outputContainer->layout()->contentsMargins(), QMargins(0, 0, 0, 0));
        QCOMPARE(output->contentOrigin(), QPointF(14, 10));
        QVERIFY(!window.findChild<QWidget *>(QStringLiteral("terminalStatus")));
        QVERIFY(loadingOverlay);
        QVERIFY(loadingOverlay->isVisible());
        QVERIFY(onlineBadge->text().contains(QStringLiteral("连接中")));
        QTRY_VERIFY_WITH_TIMEOUT(input->isEnabled(), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(!loadingOverlay->isVisible(), 1000);
        QVERIFY(onlineBadge->text().contains(QStringLiteral("在线")));
        QTRY_VERIFY_WITH_TIMEOUT(detailProcesses->topLevelItemCount() > 0, 1000);
        QCOMPARE(window.findChildren<noxshell::ui::TransferQueuePanel *>().size(), 1);

        auto *newTabButton = window.findChild<QToolButton *>(QStringLiteral("terminalNewTabButton"));
        QVERIFY(newTabButton);
        QTest::mouseClick(newTabButton, Qt::LeftButton);
        QVERIFY(recentPage->isVisible());
        QVERIFY(sidebar->isVisible());
        QTreeWidgetItem *currentServerLogin = nullptr;
        for (int index = 0; index < recentLogins->topLevelItemCount(); ++index) {
            if (recentLogins->topLevelItem(index)->text(0) == QStringLiteral("prod-web-01")) {
                currentServerLogin = recentLogins->topLevelItem(index);
                break;
            }
        }
        QVERIFY(currentServerLogin);
        recentLogins->itemDoubleClicked(currentServerLogin, 0);
        QTRY_VERIFY_WITH_TIMEOUT(sessionsPage->isVisible(), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(!sidebar->isVisible(), 1000);
        QCOMPARE(tabs->count(), 1);

        input->setFocus();
        QTest::keyClicks(input, QStringLiteral("pwd"));
        QTest::keyClick(input, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(output->hasFocus(), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(output->plainText().contains(QStringLiteral("/var/www/app")), 1000);
        QTest::mouseClick(historyButton, Qt::LeftButton);
        QVERIFY(historyPanel->isVisible());
        auto *commandHistory = historyPanel->findChild<QTreeWidget *>(QStringLiteral("commandHistoryList"));
        auto *commandHistoryTabs = historyPanel->findChild<QTabBar *>(QStringLiteral("commandHistoryTabs"));
        auto *commandHistoryClear = historyPanel->findChild<QToolButton *>(QStringLiteral("commandHistoryClearButton"));
        auto *favoriteAction = historyPanel->findChild<QAction *>(QStringLiteral("commandHistoryFavoriteAction"));
        QVERIFY(commandHistory);
        QVERIFY(commandHistoryTabs);
        QVERIFY(commandHistoryClear);
        QVERIFY(favoriteAction);
        QCOMPARE(commandHistoryClear->text(), QStringLiteral("清空历史"));
        QVERIFY(commandHistoryClear->toolTip().contains(QStringLiteral("已收藏命令保持不变")));
        QTRY_COMPARE_WITH_TIMEOUT(commandHistory->topLevelItemCount(), 1, 1000);
        QCOMPARE(commandHistory->topLevelItem(0)->text(1), QStringLiteral("pwd"));
        input->setText(QStringLiteral("draft command to replace"));
        commandHistory->itemDoubleClicked(commandHistory->topLevelItem(0), 1);
        QCOMPARE(input->text(), QStringLiteral("pwd"));
        QVERIFY(!historyPanel->isVisible());
        QTest::mouseClick(historyButton, Qt::LeftButton);
        QVERIFY(historyPanel->isVisible());
        bool historyPrepared = false;
        QVERIFY(QMetaObject::invokeMethod(historyPanel, "prepareItemActions", Qt::DirectConnection,
            Q_RETURN_ARG(bool, historyPrepared), Q_ARG(int, 0)));
        QVERIFY(historyPrepared);
        favoriteAction->trigger();
        commandHistoryTabs->setCurrentIndex(1);
        QTRY_COMPARE_WITH_TIMEOUT(commandHistory->topLevelItemCount(), 1, 1000);
        QCOMPARE(commandHistoryClear->text(), QStringLiteral("清空收藏"));
        QVERIFY(commandHistoryClear->toolTip().contains(QStringLiteral("保留在历史")));
        QSignalSpy escapeInputSpy(output, &noxshell::ui::TerminalView::inputGenerated);
        output->setFocus();
        QTest::keyClick(output, Qt::Key_Escape);
        QVERIFY(!historyPanel->isVisible());
        QCOMPARE(escapeInputSpy.count(), 0);
        QTest::keyClick(output, Qt::Key_Escape);
        QCOMPARE(escapeInputSpy.count(), 1);
        QCOMPARE(escapeInputSpy.first().at(0).toByteArray(), QByteArray("\x1b"));

        QVERIFY(filePane->isVisible());
        QTest::mouseClick(fileToggleButton, Qt::LeftButton);
        QVERIFY(!filePane->isVisible());
        QVERIFY(fileToggleButton->toolTip().contains(QStringLiteral("显示")));
        QTest::mouseClick(fileToggleButton, Qt::LeftButton);
        QVERIFY(filePane->isVisible());
        QVERIFY(fileToggleButton->toolTip().contains(QStringLiteral("隐藏")));
        tabs->customContextMenuRequested(tabs->tabRect(0).center());
        clearTerminal->trigger();
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
        QVERIFY(hostItemForName(QStringLiteral("db-master-01")));
        hosts->setCurrentItem(hostItemForName(QStringLiteral("db-master-01")));
        QCOMPARE(address->text(), QStringLiteral("10.0.0.11"));
        QVERIFY(onlineBadge->text().contains(QStringLiteral("在线")));
        QCOMPARE(tabs->count(), 1);
        QCOMPARE(tabs->currentIndex(), 0);
        const auto terminalInputs = window.findChildren<QLineEdit *>(QStringLiteral("terminalInput"));
        QCOMPARE(std::count_if(terminalInputs.cbegin(), terminalInputs.cend(),
                     [](const QLineEdit *editor) { return editor->isEnabled(); }),
            1);

        hosts->itemDoubleClicked(hostItemForName(QStringLiteral("db-master-01")), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!sidebar->isVisible(), 1000);
        QTRY_COMPARE_WITH_TIMEOUT([&window] {
            const auto inputs = window.findChildren<QLineEdit *>(QStringLiteral("terminalInput"));
            return std::count_if(inputs.cbegin(), inputs.cend(), [](const auto *editor) {
                return editor->isEnabled();
            });
        }(), 2, 1000);
        QCOMPARE(tabs->tabText(1), QStringLiteral("db-master…"));
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
        QTRY_COMPARE_WITH_TIMEOUT(currentHostName(), QStringLiteral("prod-web-01"), 1000);
        QCOMPARE(address->text(), QStringLiteral("10.0.0.11"));
        QTRY_VERIFY_WITH_TIMEOUT(detailProcesses->topLevelItemCount() > 0, 1000);
        QVERIFY(activeFileServer());
        QVERIFY(activeFileServer()->text().contains(QStringLiteral("prod-web-01")));
        auto *firstPath = fileStack->currentWidget()->findChild<QLineEdit *>(QStringLiteral("remotePathEdit"));
        QVERIFY(firstPath);
        firstPath->setText(QStringLiteral("/var/www/app"));
        QTest::keyClick(firstPath, Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(firstPath->text(), QStringLiteral("/var/www/app"), 1000);

        tabs->setCurrentIndex(1);
        QTRY_COMPARE_WITH_TIMEOUT(currentHostName(), QStringLiteral("db-master-01"), 1000);
        QCOMPARE(address->text(), QStringLiteral("10.0.0.21"));
        QTRY_VERIFY_WITH_TIMEOUT(detailProcesses->topLevelItemCount() > 0, 1000);
        QVERIFY(activeFileServer()->text().contains(QStringLiteral("db-master-01")));

        // 左侧主机列表不再兼任标签导航，单击只保留列表选中项。
        hosts->setCurrentItem(hostItemForName(QStringLiteral("prod-web-01")));
        QCOMPARE(tabs->currentIndex(), 1);
        QVERIFY(activeFileServer()->text().contains(QStringLiteral("db-master-01")));

        // 终端、监控和 SFTP 只通过终端标签联动，切换不得再次握手或重置文件目录。
        tabs->setCurrentIndex(0);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->currentIndex(), 0, 1000);
        QVERIFY(firstSession->isConnected());
        QVERIFY(secondSession->isConnected());
        QCOMPARE(firstReconnectSpy.count(), 0);
        QCOMPARE(secondReconnectSpy.count(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(detailProcesses->topLevelItemCount() > 0, 1000);
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
        auto *closedHostRow = hosts->itemWidget(hostItemForName(QStringLiteral("db-master-01")), 0);
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
        auto *passwordReveal = addDialog.findChild<QAction *>(QStringLiteral("passwordRevealAction"));
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
        QVERIFY(!passwordReveal->icon().isNull());
        passwordReveal->trigger();
        QCOMPARE(passwordEditor->echoMode(), QLineEdit::Normal);
        QVERIFY(passwordHint->text().contains(QStringLiteral("当前输入")));
        QVERIFY(!addTestButton->isEnabled());
        addDialog.setConnectionServices(&repository, &credentialStore);
        addDialog.setAvailableGroups({QStringLiteral("生产环境"), QStringLiteral("测试环境")});
        addDialog.setInitialGroup(QStringLiteral("测试环境"));
        QVERIFY(addTestButton->isEnabled());
        QCOMPARE(addDialog.findChild<QPushButton *>(QStringLiteral("primaryButton"))->text(), QStringLiteral("保存主机"));

        auto *nameEditor = addDialog.findChild<QLineEdit *>(QStringLiteral("nameEditor"));
        auto *hostEditorForAdd = addDialog.findChild<QLineEdit *>(QStringLiteral("hostEditor"));
        auto *portEditorForAdd = addDialog.findChild<QSpinBox *>(QStringLiteral("portEditor"));
        auto *userEditor = addDialog.findChild<QLineEdit *>(QStringLiteral("userEditor"));
        auto *authenticationEditor = addDialog.findChild<QComboBox *>(QStringLiteral("authenticationEditor"));
        auto *groupEditor = addDialog.findChild<QComboBox *>(QStringLiteral("groupEditor"));
        QVERIFY(nameEditor);
        QVERIFY(hostEditorForAdd);
        QVERIFY(portEditorForAdd);
        QVERIFY(userEditor);
        QVERIFY(authenticationEditor);
        QVERIFY(groupEditor);
        QVERIFY(!groupEditor->isEditable());
        QCOMPARE(groupEditor->currentText(), QStringLiteral("测试环境"));
        QCOMPARE(groupEditor->currentData().toString(), QStringLiteral("测试环境"));
        QVERIFY(groupEditor->findText(QStringLiteral("不设置分组（可选）")) >= 0);
        QVERIFY(groupEditor->findText(QStringLiteral("生产环境")) >= 0);
        QVERIFY(groupEditor->findText(QStringLiteral("测试环境")) >= 0);
        addDialog.show();
        QCoreApplication::processEvents();
        auto *endpointRow = addDialog.findChild<QWidget *>(QStringLiteral("endpointEditorRow"));
        QVERIFY(endpointRow);
        QCOMPARE(hostEditorForAdd->mapTo(endpointRow, QPoint{}).y(),
            portEditorForAdd->mapTo(endpointRow, QPoint{}).y());
        QVERIFY(hostEditorForAdd->geometry().right() < portEditorForAdd->geometry().left());
        QCOMPARE(endpointRow->geometry().left(), authenticationEditor->geometry().left());
        QCOMPARE(endpointRow->width(), authenticationEditor->width());

        // 失败完成后测试按钮必须立即恢复，并允许下一次点击继续创建测试会话。
        nameEditor->setText(QStringLiteral("retry-test"));
        hostEditorForAdd->setText(QStringLiteral("127.0.0.1"));
        portEditorForAdd->setValue(1);
        userEditor->setText(QStringLiteral("root"));
        passwordEditor->setText(QStringLiteral("retry-password"));
        QSignalSpy testClicks(addTestButton, &QPushButton::clicked);
        for (int attempt = 1; attempt <= 2; ++attempt) {
            QTest::mouseClick(addTestButton, Qt::LeftButton);
            QCOMPARE(testClicks.count(), attempt);
            QVERIFY(!addTestButton->isEnabled());
            auto *progress = addDialog.findChild<QProgressDialog *>();
            QVERIFY(progress);
            auto *testSession = progress->findChild<noxshell::SshSession *>();
            QVERIFY(testSession);
            QTimer::singleShot(0, [] {
                for (auto *widget : QApplication::topLevelWidgets()) {
                    if (auto *message = qobject_cast<QMessageBox *>(widget)) message->accept();
                }
            });
            QVERIFY(QMetaObject::invokeMethod(testSession, "connectionChanged", Qt::DirectConnection,
                Q_ARG(bool, false), Q_ARG(QString, QStringLiteral("SSH 连接失败：回归测试"))));
            QTRY_VERIFY_WITH_TIMEOUT(addTestButton->isEnabled(), 1000);
            QCOMPARE(addTestButton->text(), QStringLiteral("连接测试"));
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
        addDialog.hide();

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
    noxshell::ui::applyApplicationTheme(noxshell::ui::ThemeMode::Light);
    SmokeTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "SmokeTest.moc"
