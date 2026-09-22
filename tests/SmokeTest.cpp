#include "../src/core/LinuxMetrics.h"
#include "../src/core/AppLogger.h"
#include "../src/core/CredentialStore.h"
#include "../src/core/DirectorySizeCommand.h"
#include "../src/core/FileTransferTask.h"
#include "../src/core/MetricHistory.h"
#include "../src/core/MetricsCollectionPolicy.h"
#include "../src/core/RdpLauncher.h"
#include "../src/core/RemoteDirectoryFallback.h"
#include "../src/core/SshSession.h"
#include "../src/core/ServerRepository.h"
#include "../src/ui/AppTheme.h"
#include "../src/ui/Application.h"
#include "../src/ui/CommandHistoryPanel.h"
#include "../src/ui/CredentialInput.h"
#include "../src/ui/FilePanel.h"
#include "../src/ui/FilePermissionDialog.h"
#include "../src/ui/HostSidebar.h"
#include "../src/ui/MainWindow.h"
#include "../src/ui/MetricCard.h"
#include "../src/ui/NetworkRateChart.h"
#include "../src/ui/RemoteFileEditor.h"
#include "../src/ui/RemotePathEdit.h"
#include "../src/ui/RdpDialog.h"
#include "../src/ui/SearchMarkerScrollBar.h"
#include "../src/ui/ServerDialog.h"
#include "../src/ui/SystemDetailPanel.h"
#include "../src/ui/TerminalSettingsDialog.h"
#include "../src/ui/TerminalWorkspace.h"
#include "../src/ui/TransferQueuePanel.h"
#include "../src/ui/TerminalView.h"
#include "../src/ui/TerminalInputSourceScope.h"
#include "../src/ui/VtTerminalModel.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEventLoop>
#include <QDoubleSpinBox>
#include <QDir>
#include <QDesktopServices>
#include <QFile>
#include <QFontComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QTabWidget>
#include <QIcon>
#include <QImage>
#include <QInputMethodEvent>
#include <QLabel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QProgressDialog>
#include <QProcess>
#include <QRadioButton>
#include <QScrollBar>
#include <QScrollArea>
#include <QScopeGuard>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSplitter>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QPushButton>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QTextBlock>
#include <QTextLayout>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QUuid>
#include <QtTest>

#include <algorithm>

#ifdef Q_OS_MACOS
bool nativeProbeTerminalMarkText(QWidget *terminal, const QString &text);
bool nativeProbeTerminalCommitText(QWidget *terminal, const QString &text);
bool nativeProbeTerminalEscape(QWidget *terminal);
#endif
#include <functional>
#include <utility>

#ifdef Q_OS_MACOS
#include <Security/Security.h>
#include <QJsonDocument>
#include <QJsonObject>
#endif

class MemoryCredentialStore final : public noxshell::CredentialStore {
public:
    bool save(const QString &reference, const noxshell::CredentialSecret &secret) override
    {
        ++saveCalls;
        if (failSaves) { error = QStringLiteral("测试存储不可写"); return false; }
        error.clear();
        secrets.insert(reference, secret);
        return true;
    }

    noxshell::CredentialSecret load(const QString &reference) override
    {
        ++loadCalls;
        // Exercise provider reentrancy/cancellation, without real Keychain UI.
        if (auto callback = std::exchange(onLoad, {})) callback();
        error = failLoads ? QStringLiteral("测试凭据库已锁定") : QString{};
        return failLoads ? noxshell::CredentialSecret{} : secrets.value(reference);
    }

    bool remove(const QString &reference) override
    {
        error.clear();
        secrets.remove(reference);
        return true;
    }

    QString lastError() const override { return error; }

    QHash<QString, noxshell::CredentialSecret> secrets;
    bool failLoads{};
    bool failSaves{};
    int saveCalls{};
    int loadCalls{};
    std::function<void()> onLoad;
    QString error;
};

class LocalDirectoryUrlCapture final : public QObject {
    Q_OBJECT
public:
    QList<QUrl> urls;
public slots:
    void capture(const QUrl &url) { urls.append(url); }
};

// Repeated dialogs are answered too, so a regression fails a count assertion
// instead of leaving unattended tests blocked inside a modal event loop.
class CloseDialogResponder {
public:
    explicit CloseDialogResponder(QMessageBox::StandardButton response = QMessageBox::Yes)
        : answer(response)
    {
        QObject::connect(&timer, &QTimer::timeout, &timer, [this] {
            auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!dialog || last == dialog || responding) return;
            responding = true;
            last = dialog;
            ++count;
            names.append(dialog->objectName());
            texts.append(dialog->text());
            safeDefault &= dialog->defaultButton() == dialog->button(QMessageBox::Cancel);
            if (beforeAnswer) beforeAnswer(dialog);
            if (escape) QTest::keyClick(dialog, Qt::Key_Escape);
            else if (auto *button = dialog->button(answer)) button->click();
            else dialog->reject();
            responding = false;
        });
        timer.start(5);
    }
    QTimer timer;
    QPointer<QMessageBox> last;
    QMessageBox::StandardButton answer;
    QStringList names, texts;
    int count{};
    bool safeDefault{true};
    bool escape{};
    bool responding{};
    std::function<void(QMessageBox *)> beforeAnswer;
};

static int answerClose(const std::function<void()> &action)
{
    CloseDialogResponder responder;
    action();
    return responder.count;
}

#ifdef Q_OS_MACOS
void nativeProbeCloseWindow(QWidget *window);
void nativeProbeReopenApplication();
void nativeProbeQuitApplication();
#endif

// Run a real application event loop in a child process: accepted Quit must
// terminate it, but cancellation/reentrancy/dirty child dialogs must not.
static int runWindowLifecycleProbe(noxshell::ui::Application &app)
{
    QTemporaryDir directory;
    if (!directory.isValid()) return 2;
    MemoryCredentialStore credentials;
    noxshell::ui::MainWindow window(directory.filePath(QStringLiteral("lifecycle.sqlite3")), nullptr, &credentials);
    app.setMainWindow(&window);
    auto *workspace = window.findChild<noxshell::ui::TerminalWorkspace *>();
    if (!workspace) return 3;
    noxshell::ServerProfile profile;
    profile.id = QStringLiteral("lifecycle-demo");
    profile.name = QStringLiteral("关闭测试 · 演示连接");
    profile.connectionMode = noxshell::ConnectionMode::Demo;
    workspace->openOrActivate(profile, true);
    auto *session = workspace->findChild<noxshell::SshSession *>();
    auto *terminal = workspace->findChild<noxshell::ui::TerminalView *>();
    if (!session || !terminal) return 4;
    terminal->feedText(QStringLiteral("buffer-survives-window-hide"));
    window.show();
    int phase = 0;
    int confirmations = 0;
    int quitting = 0;
    bool canceledTransfer = false;
    bool completedTransfer = false;
    const auto downloadPath = directory.filePath(QStringLiteral("hidden-window-download.txt"));
    QObject::connect(session, &noxshell::SshSession::transferTaskChanged, &window,
        [&](const noxshell::FileTransferTask &task) {
            canceledTransfer |= task.state == noxshell::TransferState::Canceled;
            completedTransfer |= task.state == noxshell::TransferState::Completed;
        });
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &window, [&] { ++quitting; });
    QSignalSpy sessionClosed(workspace, &noxshell::ui::TerminalWorkspace::sessionClosed);
    const bool native = QGuiApplication::platformName() == QStringLiteral("cocoa");
    const auto quit = [&] {
#ifdef Q_OS_MACOS
        if (native) { nativeProbeQuitApplication(); return; }
#endif
        app.quit();
    };
    class UnsavedDialog : public QDialog {
    public:
        using QDialog::QDialog;
        bool veto{true};
        void closeEvent(QCloseEvent *event) override { event->setAccepted(!veto); }
    } unsaved(&window);
    QTimer driver;
    driver.setInterval(60);
    QObject::connect(&driver, &QTimer::timeout, &window, [&] {
        driver.stop(); // Modal loops must not advance the probe concurrently.
        const auto fail = [&](int code) { app.exit(code); };
        if (phase == 0) {
            if (!session->isConnected()) { driver.start(); return; }
            session->writeFile(QStringLiteral("/root/hidden-window.txt"), QByteArrayLiteral("demo-only-transfer"), true);
            session->downloadFile(QStringLiteral("/root/hidden-window.txt"), downloadPath);
#ifdef Q_OS_MACOS
            if (native) nativeProbeCloseWindow(&window);
            else window.close();
#else
            window.hide();
#endif
        } else if (phase == 1) {
            if (window.isVisible() || !session->isConnected() || !sessionClosed.isEmpty()) { fail(10); return; }
#ifdef Q_OS_MACOS
            if (native) nativeProbeReopenApplication();
            else app.applicationStateChanged(Qt::ApplicationActive);
#else
            window.show();
#endif
        } else if (phase == 2) {
            if (!window.isVisible() || workspace->sessionCount() != 1
                || !terminal->plainText().contains(QStringLiteral("buffer-survives-window-hide"))) { fail(11); return; }
            if (!completedTransfer || canceledTransfer || !QFileInfo::exists(downloadPath)) { fail(16); return; }
            CloseDialogResponder responder(QMessageBox::Cancel);
            bool nestedIgnored = false;
            responder.beforeAnswer = [&](QMessageBox *) {
                QEvent nested(QEvent::Quit);
                QCoreApplication::sendEvent(&app, &nested);
                nestedIgnored = !nested.isAccepted();
            };
            quit();
            confirmations += responder.count;
            if (responder.count != 1 || !responder.safeDefault || !nestedIgnored) { fail(12); return; }
        } else if (phase == 3) {
            if (!window.isVisible() || !session->isConnected() || quitting) { fail(13); return; }
            // A parented, unsaved editor can veto even after application approval.
            unsaved.show();
            CloseDialogResponder responder;
            quit();
            confirmations += responder.count;
            if (responder.count != 1 || !window.isVisible() || !unsaved.isVisible() || quitting) { fail(14); return; }
            unsaved.veto = false;
            unsaved.close();
#ifdef Q_OS_MACOS
            window.close();
#endif
        } else if (phase == 4) {
            CloseDialogResponder responder;
            quit(); // Quit remains available while the main window is hidden.
            confirmations += responder.count;
            if (responder.count != 1 || responder.names != QStringList{QStringLiteral("applicationQuitConfirmation")}) {
                fail(15); return;
            }
            ++phase;
            return;
        }
        ++phase;
        driver.start();
    });
    QTimer::singleShot(8000, &window, [&] { app.exit(20); });
    driver.start();
    const int result = app.exec();
    app.setMainWindow(nullptr);
    if (result) return result;
    if (phase != 5 || confirmations != 3 || quitting != 1 || credentials.loadCalls != 0
        || !sessionClosed.isEmpty() || canceledTransfer || !completedTransfer) return 21;
    qInfo("NOXSHELL_WINDOW_LIFECYCLE_OK");
    return 0;
}

class SmokeTest final : public QObject {
    Q_OBJECT

private slots:
    void asciiCredentialInputNormalizesOnlyNewEdits_data()
    {
        QTest::addColumn<bool>("revealed");
        QTest::newRow("masked") << false;
        QTest::newRow("revealed") << true;
    }

    void asciiCredentialInputNormalizesOnlyNewEdits()
    {
        QFETCH(bool, revealed);
        QLineEdit editor;
        QLabel feedback;
        editor.setEchoMode(revealed ? QLineEdit::Normal : QLineEdit::Password);
        noxshell::ui::configureAsciiCredentialInput(&editor, &feedback);
        QVERIFY(editor.inputMethodHints().testFlag(Qt::ImhLatinOnly));
        QVERIFY(editor.inputMethodHints().testFlag(Qt::ImhSensitiveData));
        QString ascii, fullwidth;
        for (ushort code = 0x21; code <= 0x7e; ++code) {
            ascii.append(QChar(code));
            fullwidth.append(QChar(code + 0xfee0));
        }
        editor.insert(QStringLiteral("  ") + ascii + QStringLiteral("  "));
        QCOMPARE(editor.text(), QStringLiteral("  ") + ascii + QStringLiteral("  "));
        editor.clear();
        editor.insert(fullwidth);
        QCOMPARE(editor.text(), ascii);
        QCOMPARE(editor.cursorPosition(), ascii.size());
        QVERIFY(feedback.text().contains(QStringLiteral("已将")));
        editor.selectAll();
        editor.insert(QStringLiteral("。｡、\u3000\u00a0‘’“”–—…【】"));
        QCOMPARE(editor.text(), QStringLiteral("..,  ''\"\"--...[]"));
        editor.clear();
        QVERIFY(feedback.text().contains(QStringLiteral("仅英文半角")));
        editor.insert(QStringLiteral("Ab cd"));
        editor.setCursorPosition(2);
        editor.insert(QStringLiteral("…"));
        QCOMPARE(editor.text(), QStringLiteral("Ab... cd"));
        QCOMPARE(editor.cursorPosition(), 5);
        editor.setSelection(2, 3);
        editor.insert(QStringLiteral("。"));
        QCOMPARE(editor.text(), QStringLiteral("Ab. cd"));
        QCOMPARE(editor.cursorPosition(), 3);

        // Reject the whole edit, including mixed pastes, without erasing a selection.
        for (const auto &invalid : {QStringLiteral("中文"), QStringLiteral("é"),
                 QStringLiteral("🔑"), QStringLiteral("valid。but中文"),
                 QStringLiteral("line\nbreak"), QStringLiteral("\t")}) {
            editor.setSelection(0, 2);
            editor.insert(invalid);
            QCOMPARE(editor.text(), QStringLiteral("Ab. cd"));
            QVERIFY(feedback.text().contains(QStringLiteral("未写入")));
            QCOMPARE(editor.selectionStart(), 0);
            QCOMPARE(editor.selectedText(), QStringLiteral("Ab"));
        }
        editor.setEchoMode(revealed ? QLineEdit::Password : QLineEdit::Normal);
        QVERIFY(editor.inputMethodHints().testFlag(Qt::ImhLatinOnly));
        editor.selectAll();
        editor.insert(QStringLiteral("Ｓｓｈ１２３。！"));
        QCOMPARE(editor.text(), QStringLiteral("Ssh123.!"));
        editor.insert(QStringLiteral("中"));
        QCOMPARE(editor.text(), QStringLiteral("Ssh123.!"));
        QVERIFY(editor.hasAcceptableInput());
    }

    void asciiCredentialInputHandlesClipboardAndIme()
    {
        QLineEdit editor;
        QLabel feedback;
        editor.setEchoMode(QLineEdit::Password);
        noxshell::ui::configureAsciiCredentialInput(&editor, &feedback);
        const auto *clipboardData = QApplication::clipboard()->mimeData();
        QMap<QString, QByteArray> clipboardBackup;
        if (clipboardData) {
            for (const auto &format : clipboardData->formats()) clipboardBackup.insert(format, clipboardData->data(format));
        }
        const auto restoreClipboard = qScopeGuard([&] {
            auto *restored = new QMimeData;
            for (auto it = clipboardBackup.cbegin(); it != clipboardBackup.cend(); ++it) restored->setData(it.key(), it.value());
            QApplication::clipboard()->setMimeData(restored);
        });
        QApplication::clipboard()->setText(QStringLiteral("  Ａbc。１２３！  "));
        editor.paste();
        QCOMPARE(editor.text(), QStringLiteral("  Abc.123!  "));
        editor.selectAll();
        QApplication::clipboard()->setText(QStringLiteral("abc中文"));
        editor.paste();
        QCOMPARE(editor.text(), QStringLiteral("  Abc.123!  "));
        QVERIFY(feedback.text().contains(QStringLiteral("未写入")));

        editor.clear();
        QInputMethodEvent preedit(QStringLiteral("zhong"), {});
        QApplication::sendEvent(&editor, &preedit);
        QVERIFY(editor.text().isEmpty());
        QInputMethodEvent chineseCommit;
        chineseCommit.setCommitString(QStringLiteral("中文"));
        QApplication::sendEvent(&editor, &chineseCommit);
        QVERIFY(editor.text().isEmpty());
        QVERIFY(feedback.text().contains(QStringLiteral("未写入")));
        QInputMethodEvent punctuationCommit;
        punctuationCommit.setCommitString(QStringLiteral("ＳＳＨ。１２３！"));
        QApplication::sendEvent(&editor, &punctuationCommit);
        QCOMPARE(editor.text(), QStringLiteral("SSH.123!"));
        QCOMPARE(editor.cursorPosition(), 8);
        editor.setEchoMode(QLineEdit::Normal);
        editor.setSelection(3, 1);
        punctuationCommit.setCommitString(QStringLiteral("…"));
        QApplication::sendEvent(&editor, &punctuationCommit);
        QCOMPARE(editor.text(), QStringLiteral("SSH...123!"));
        QCOMPARE(editor.cursorPosition(), 6);
        editor.clear();
        editor.insert(QStringLiteral("base"));
        editor.insert(QStringLiteral("！"));
        QCOMPARE(editor.text(), QStringLiteral("base!"));
        if (editor.isUndoAvailable()) {
            editor.undo();
            QVERIFY(!editor.text().contains(QChar(0xff01)));
            editor.redo();
            QCOMPARE(editor.text(), QStringLiteral("base!"));
        }
    }

    void allCredentialEditorsUseAsciiInputPolicy()
    {
        noxshell::ui::ServerDialog ssh;
        noxshell::ui::RdpDialog rdp;
        MemoryCredentialStore credentials;
        noxshell::ui::TerminalWorkspace workspace(nullptr, &credentials);
        noxshell::ServerProfile profile;
        profile.id = QStringLiteral("ascii-input-only");
        profile.name = profile.id;
        profile.host = QStringLiteral("192.0.2.20");
        profile.connectionMode = noxshell::ConnectionMode::Ssh;
        workspace.openOrActivate(profile, false); // Never connect to an external host.
        const QList<QLineEdit *> editors{
            ssh.findChild<QLineEdit *>(QStringLiteral("passwordEditor")),
            ssh.findChild<QLineEdit *>(QStringLiteral("passphraseEditor")),
            rdp.findChild<QLineEdit *>(QStringLiteral("rdpPasswordEditor")),
            workspace.findChild<QLineEdit *>(QStringLiteral("terminalConnectionPassword"))};
        for (auto *editor : editors) {
            QVERIFY(editor);
            QVERIFY(editor->validator());
            QVERIFY(editor->inputMethodHints().testFlag(Qt::ImhLatinOnly));
            editor->insert(QStringLiteral("Ａbc。１２３！"));
            QCOMPARE(editor->text(), QStringLiteral("Abc.123!"));
            editor->insert(QStringLiteral("中"));
            QCOMPARE(editor->text(), QStringLiteral("Abc.123!"));
        }
    }

    void monitoringScheduleReducesExpensiveRemoteQueries()
    {
        noxshell::MetricsCollectionPolicy policy;
        int processQueries = 0;
        int diskQueries = 0;
        for (int second = 0; second < 60; ++second) {
            const auto command = policy.command(second * 1000);
            QVERIFY(command.contains("/proc/stat"));
            QVERIFY(command.contains("command -v awk"));
            QVERIFY(!command.contains("getconf"));
            QVERIFY(!command.contains("sudo"));
            processQueries += command.contains("ps -eo");
            diskQueries += command.contains("df -Pkl");
        }
        QCOMPARE(processQueries, 12);
        QCOMPARE(diskQueries, 2);
        // An immediate second request must not repeat the expensive queries.
        QVERIFY(!policy.command(59001).contains("ps -eo"));
        policy = {};
        QVERIFY(policy.command(0).contains("df -Pkl"));
    }

    void partialMetricsKeepSlowSectionsWithoutStaleCpuCounters()
    {
        const QByteArray base = "__CPU__\ncpu 10 0 2 50 0 0 0 0\ncpu0 10 0 2 50 0 0 0 0\n"
            "__MEM__\nMemTotal: 1024 kB\nMemAvailable: 512 kB\n__LOAD__\n0.1 0.2 0.3 1/10 22\n";
        noxshell::LinuxMetricsSnapshot previous, current;
        QString error;
        QVERIFY2(noxshell::LinuxMetricsParser::parse(base + "__DISK__\n/dev/test 100 20 80 20% /\n"
            "__PROC__\n42 user 1.0 2.0 10 sh\n", previous, &error), qPrintable(error));
        QCOMPARE(previous.cpuCoreCount, 1); // Derived from /proc/stat, no getconf/nproc process.
        QVERIFY(noxshell::LinuxMetricsParser::parse(base, current));
        current.cpu.user = 25;
        noxshell::LinuxMetricsParser::retainSlowMetrics(current, previous);
        QCOMPARE(current.cpu.user, quint64(25));
        QCOMPARE(current.disks.size(), 1);
        QCOMPARE(current.processes.size(), 1);
        QVERIFY(noxshell::LinuxMetricsParser::parse(base + "__DISK__\n__PROC__\n", current));
        noxshell::LinuxMetricsParser::retainSlowMetrics(current, previous);
        QVERIFY(current.disks.isEmpty());
        QVERIFY(current.processes.isEmpty());
    }

    void lightweightAwkCollectorParsesSyntheticProcFiles()
    {
        const auto awk = QStandardPaths::findExecutable(QStringLiteral("awk"));
        if (awk.isEmpty()) QSKIP("Local awk unavailable; remote command has a grep/cat fallback");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(QDir().mkpath(directory.filePath(QStringLiteral("net"))));
        const QList<QPair<QString, QByteArray>> files{
            {"stat", "cpu 10 0 2 50 0 0 0 0\ncpu0 10 0 2 50 0 0 0 0\nintr 123456\n"},
            {"meminfo", "MemTotal: 1024 kB\nMemAvailable: 512 kB\n"},
            {"loadavg", "0.1 0.2 0.3 1/10 22\n"},
            {"uptime", "42.5 30.0\n"},
            {"net/dev", "eth0: 100 0 0 0 0 0 0 0 200 0 0 0 0 0 0 0\n"},
        };
        QStringList paths;
        for (const auto &item : files) {
            const auto path = directory.filePath(item.first);
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(item.second), item.second.size());
            paths.append(path);
        }
        noxshell::MetricsCollectionPolicy policy;
        const auto command = policy.command(0);
        const auto start = command.indexOf("awk '") + 5;
        const auto end = command.indexOf("' /proc/stat", start);
        QVERIFY(start >= 5 && end > start);
        auto program = command.mid(start, end - start);
        auto prefix = directory.path().toUtf8();
        prefix.replace("\\", "\\\\").replace("\"", "\\\"");
        program.replace("/proc", prefix);
        QProcess process;
        process.start(awk, QStringList{QString::fromUtf8(program)} + paths);
        QVERIFY(process.waitForFinished(5000));
        QCOMPARE(process.exitCode(), 0);
        const auto payload = process.readAllStandardOutput();
        QVERIFY(!payload.contains("intr 123456"));
        noxshell::LinuxMetricsSnapshot snapshot;
        QString error;
        QVERIFY2(noxshell::LinuxMetricsParser::parse(payload, snapshot, &error), qPrintable(error));
        QCOMPARE(snapshot.cpuCoreCount, 1);
        QCOMPARE(snapshot.uptimeSeconds, quint64(42));
        QCOMPARE(snapshot.networks.size(), 1);
        QCOMPARE(snapshot.networks.first().transmittedBytes, quint64(200));
    }

    void duplicateDirectoryAndMetricsRequestsAreCoalesced()
    {
        noxshell::SshSession session;
        noxshell::ServerProfile profile;
        profile.name = QStringLiteral("synthetic-request-test");
        profile.host = QStringLiteral("example.invalid");
        profile.user = QStringLiteral("test");
        profile.password = QStringLiteral("synthetic-secret");
        profile.keyPassphrase = QStringLiteral("synthetic-passphrase");
        session.connectTo(profile);
        QTRY_VERIFY(session.isConnected());
        QVERIFY(session.profile().password.isEmpty());
        QVERIFY(session.profile().keyPassphrase.isEmpty());
        QSignalSpy directories(&session, &noxshell::SshSession::directoryListed);
        session.listDirectory(QStringLiteral("/tmp"));
        session.listDirectory(QStringLiteral("/tmp/"));
        QTRY_COMPARE(directories.size(), 1);
        session.listDirectory(QStringLiteral("/tmp"));
        QTRY_COMPARE(directories.size(), 2); // Explicit refresh after completion still works.
        QSignalSpy samples(&session, &noxshell::SshSession::metricSampleReceived);
        session.requestMetrics();
        QTRY_COMPARE(samples.size(), 1);
        QVERIFY(session.lastMetricSample().has_value());
        QCOMPARE(session.lastMetricSample()->capturedAt,
            qvariant_cast<noxshell::MetricSample>(samples.first().first()).capturedAt);
        session.requestMetrics();
        QTest::qWait(30);
        QCOMPARE(samples.size(), 1);
        session.listDirectory(QStringLiteral("/old-host"));
        session.disconnectFromHost();
        QVERIFY(!session.lastMetricSample().has_value());
        QTest::qWait(50);
        QCOMPARE(directories.size(), 2);
    }

    void streamedTerminalSearchRefreshesInBatches()
    {
        noxshell::ui::TerminalView view;
        view.resize(800, 400);
        view.show();
        view.showSearch();
        auto *input = view.findChild<QLineEdit *>(QStringLiteral("terminalSearchInput"));
        auto *timer = view.findChild<QTimer *>(QStringLiteral("terminalSearchRefreshTimer"));
        QVERIFY(input);
        QVERIFY(timer);
        input->setText(QStringLiteral("needle"));
        QSignalSpy refreshes(timer, &QTimer::timeout);
        for (int index = 0; index < 200; ++index) view.feedData("needle\r\n");
        QCOMPARE(refreshes.size(), 0);
        QCOMPARE(view.searchMatchCount(), 0);
        QTRY_COMPARE(view.searchMatchCount(), 200);
        QCOMPARE(refreshes.size(), 1);
        view.feedData("needle\r\n");
        view.findNext(); // Navigation must flush pending matches immediately.
        QCOMPARE(view.searchMatchCount(), 201);
        QVERIFY(!timer->isActive());
        view.feedData("needle\r\n");
        view.hideSearch();
        QVERIFY(!timer->isActive());
        QCOMPARE(view.searchMatchCount(), 0);
    }

    void localCredentialRoundTripWithSyntheticEntryOnly()
    {
#ifdef Q_OS_MACOS
        QTemporaryDir vaultDirectory;
        QVERIFY(vaultDirectory.isValid());
        noxshell::CredentialStore store(vaultDirectory.path(), noxshell::CredentialStore::LegacyReader{}, nullptr);
        const auto reference = QStringLiteral("noxshell-test-owned-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto cleanup = qScopeGuard([&] { store.remove(reference); });
        // Each step runs in a fresh bounded process: verifies storage across app
        // restarts, not a memory cache. Every operation starts with interaction ON.
        for (const auto &operation : {"create", "read", "read", "read", "update", "read-updated", "delete", "delete"}) {
            QProcess probe;
            probe.start(QCoreApplication::applicationFilePath(),
                {QStringLiteral("--owned-vault-probe"), reference, QString::fromLatin1(operation), vaultDirectory.path()});
            if (!probe.waitForFinished(5000)) {
                probe.kill();
                probe.waitForFinished(1000);
                QFAIL("Local vault operation exceeded five seconds");
            }
            QCOMPARE(probe.exitStatus(), QProcess::NormalExit);
            QVERIFY2(probe.exitCode() == 0, qPrintable(QStringLiteral("%1: exit %2").arg(QString::fromLatin1(operation)).arg(probe.exitCode())));
        }
#else
        QSKIP("macOS-only local credential backend");
#endif
    }

    void inaccessibleLegacyCredentialNeverOpensAuthorization()
    {
#ifdef Q_OS_MACOS
        if (!qEnvironmentVariableIsSet("NOXSHELL_TEST_NATIVE_KEYCHAIN")) {
            QSKIP("Legacy Keychain compatibility test uses one opt-in synthetic entry only");
        }
        const auto reference = QStringLiteral("noxshell-test-legacy-")
            + QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto runSecurity = [](const QStringList &arguments) {
            QProcess process;
            process.start(QStringLiteral("/usr/bin/security"), arguments);
            if (!process.waitForFinished(10000)) {
                process.kill();
                process.waitForFinished(1000);
                return false;
            }
            return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
        };
        const auto cleanup = qScopeGuard([&] {
            runSecurity({QStringLiteral("delete-generic-password"), QStringLiteral("-s"),
                QStringLiteral("com.noxshell.ops.ssh"), QStringLiteral("-a"), reference});
        });
        // Reproduce the pre-0.2.62 creator/ACL. The only argv secret here is a
        // fixed, synthetic test value; production writes continue using SecItem.
        const auto password = QStringLiteral("legacy-synthetic-only 中文 $();");
        const auto keyPassphrase = QStringLiteral("legacy-synthetic-passphrase");
        const auto payload = QJsonDocument(QJsonObject{
            {QStringLiteral("password"), password}, {QStringLiteral("keyPassphrase"), keyPassphrase}})
            .toJson(QJsonDocument::Compact).toBase64();
        QVERIFY(runSecurity({QStringLiteral("add-generic-password"), QStringLiteral("-s"),
            QStringLiteral("com.noxshell.ops.ssh"), QStringLiteral("-a"), reference,
            QStringLiteral("-w"), QString::fromLatin1(payload)}));
        // A separate bounded process catches accidental dialogs/hangs without
        // blocking the test runner or skipping cleanup of the synthetic item.
        QProcess probe;
        QTemporaryDir vaultDirectory;
        QVERIFY(vaultDirectory.isValid());
        probe.start(QCoreApplication::applicationFilePath(),
            {QStringLiteral("--silent-keychain-probe"), reference, vaultDirectory.path()});
        if (!probe.waitForFinished(5000)) {
            probe.kill();
            probe.waitForFinished(1000);
            QFAIL("Silent credential read exceeded 5 seconds; possible unexpected authorization UI");
        }
        QCOMPARE(probe.exitStatus(), QProcess::NormalExit);
        QCOMPARE(probe.exitCode(), 0);
        noxshell::CredentialStore store(vaultDirectory.filePath(QStringLiteral("missing")), nullptr);
        runSecurity({QStringLiteral("delete-generic-password"), QStringLiteral("-s"),
            QStringLiteral("com.noxshell.ops.ssh"), QStringLiteral("-a"), reference});
        QVERIFY(store.load(reference).password.isEmpty());
        QVERIFY(!store.lastError().isEmpty());
        QVERIFY(!store.authorizationRequired());
#else
        QSKIP("macOS-only legacy Keychain compatibility");
#endif
    }

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

    void numericEditorsRenderAcrossThemeChanges()
    {
        using namespace noxshell::ui;
        const auto restoreTheme = qScopeGuard([] { applyApplicationTheme(ThemeMode::Light); });
        applyApplicationTheme(ThemeMode::System);
        const bool systemWasDark = isApplicationDarkTheme();
        ServerDialog ssh;
        ssh.setObjectName(QStringLiteral("sshServerDialog"));
        RdpDialog rdp;
        TerminalSettingsDialog terminal(TerminalAppearance{});
        const QList<QDialog *> dialogs{&ssh, &rdp, &terminal};
        const auto captureDir = qEnvironmentVariable("NOXSHELL_THEME_CAPTURE_DIR");
        if (!captureDir.isEmpty()) QVERIFY(QDir().mkpath(captureDir));
        QHash<QWidget *, QSize> sizes;
        // Exercise existing controls as well as controls created after a theme change.
        for (const auto mode : {ThemeMode::Light, ThemeMode::Dark, ThemeMode::Light,
                 ThemeMode::Dark, ThemeMode::System}) {
            applyApplicationTheme(mode);
            if (mode == ThemeMode::System) QCOMPARE(isApplicationDarkTheme(), systemWasDark);
            for (auto *dialog : dialogs) {
                dialog->show();
                QTest::qWait(30);
                if (!captureDir.isEmpty()) {
                    QVERIFY(dialog->grab().save(captureDir + QLatin1Char('/')
                        + dialog->objectName() + QLatin1Char('-') + themeModeSettingValue(mode)
                        + QStringLiteral(".png")));
                }
                const bool dark = isApplicationDarkTheme();
                for (auto *spin : dialog->findChildren<QAbstractSpinBox *>()) {
                    const auto label = spin->objectName().toUtf8();
                    auto *editor = spin->findChild<QLineEdit *>();
                    QVERIFY2(editor, label.constData());
                    QVERIFY2(editor->height() >= editor->fontMetrics().height(), label.constData());
                    if (sizes.contains(spin)) QCOMPARE(spin->size(), sizes.value(spin));
                    sizes.insert(spin, spin->size());
                    // Inspect actual pixels: macOS can paint a native white bezel even
                    // when the widget's palette reports a dark Base color.
                    const auto pixels = spin->grab().toImage();
                    int bright = 0;
                    for (int y = 2; y < pixels.height() - 2; ++y) {
                        for (int x = 2; x < pixels.width() - 2; ++x) {
                            if (pixels.pixelColor(x, y).lightness() > 230) ++bright;
                        }
                    }
                    const double brightRatio = double(bright)
                        / ((pixels.width() - 4) * (pixels.height() - 4));
                    QVERIFY2(dark ? brightRatio < 0.1 : brightRatio > 0.5, label.constData());
                    QVERIFY2(dark ? editor->palette().color(QPalette::Text).lightness() > 180
                                  : editor->palette().color(QPalette::Text).lightness() < 100, label.constData());
                }
                dialog->hide();
            }
            QDoubleSpinBox fresh;
            fresh.setSuffix(QStringLiteral(" %"));
            fresh.ensurePolished();
            QCOMPARE(fresh.palette().color(QPalette::Base).lightness() < 128,
                isApplicationDarkTheme());
        }
        auto *port = ssh.findChild<QSpinBox *>(QStringLiteral("portEditor"));
        QVERIFY(port);
        ssh.show();
        port->setValue(22);
        QTest::mouseClick(port, Qt::LeftButton, Qt::NoModifier, QPoint(port->width() - 10, 8));
        QCOMPARE(port->value(), 23);
        QTest::mouseClick(port, Qt::LeftButton, Qt::NoModifier,
            QPoint(port->width() - 10, port->height() - 8));
        QCOMPARE(port->value(), 22);
        port->setFocus();
        port->selectAll();
        QTest::keyClicks(port, "65535");
        QTest::keyClick(port, Qt::Key_Tab);
        QCOMPARE(port->value(), 65535);
        port->stepUp();
        QCOMPARE(port->value(), 65535);
        port->stepDown();
        QCOMPARE(port->value(), 65534);
        port->setValue(1);
        port->stepDown();
        QCOMPARE(port->value(), 1);
    }

    void ubuntuDirectoryFallbackQuotesPathsAndParsesFindOutput()
    {
        const auto command = noxshell::detail::fallbackDirectoryListingCommand(
            QStringLiteral("/home/user/a'b"));
        QVERIFY(command.contains("find -- '/home/user/a'\\''b'"));
        QVERIFY(command.contains("-printf '%f\\0%y\\0%s\\0%T@\\0%m\\0%u\\0%g\\0'"));

        QByteArray payload;
        const auto appendEntry = [&payload](const QByteArray &name, const QByteArray &type,
                                     const QByteArray &size, const QByteArray &modified,
                                     const QByteArray &mode, const QByteArray &owner,
                                     const QByteArray &group) {
            for (const auto &field : {name, type, size, modified, mode, owner, group}) {
                payload.append(field);
                payload.append('\0');
            }
        };
        appendEntry("项目 文件", "d", "4096", "1788249600.25", "755", "ubuntu", "ubuntu");
        appendEntry("release.txt", "f", "128", "1788249660.5", "640", "ubuntu", "deploy");
        appendEntry("current", "l", "11", "1788249700", "777", "ubuntu", "ubuntu");

        noxshell::RemoteFileEntries entries;
        QString failure;
        QVERIFY(noxshell::detail::parseFallbackDirectoryListing(
            QStringLiteral("/home/ubuntu"), payload, entries, failure));
        QVERIFY(failure.isEmpty());
        QCOMPARE(entries.size(), 3);
        QCOMPARE(entries.at(0).name, QStringLiteral("项目 文件"));
        QCOMPARE(entries.at(0).path, QStringLiteral("/home/ubuntu/项目 文件"));
        QVERIFY(entries.at(0).directory);
        QCOMPARE(entries.at(0).permissions & 0777U, quint32{0755});
        QCOMPARE(entries.at(1).size, quint64{128});
        QCOMPARE(entries.at(1).owner, QStringLiteral("ubuntu"));
        QCOMPARE(entries.at(1).group, QStringLiteral("deploy"));
        QVERIFY(entries.at(2).symbolicLink);

        auto malformed = payload;
        malformed.append("extra");
        QVERIFY(!noxshell::detail::parseFallbackDirectoryListing(
            QStringLiteral("/home/ubuntu"), malformed, entries, failure));
        QVERIFY(!failure.isEmpty());
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
        auto *autoEnglish = dialog.findChild<QCheckBox *>(QStringLiteral("terminalAutoEnglishInputCheck"));
        QVERIFY(autoEnglish);
        QSignalSpy preview(&dialog, &noxshell::ui::TerminalSettingsDialog::appearancePreviewRequested);
        autoEnglish->setChecked(false);
        QVERIFY(!dialog.appearance().autoEnglishInput);
        QVERIFY(!preview.isEmpty());
        QCOMPARE(preview.last().at(3).toBool(), false);
        dialog.findChild<QPushButton *>(QStringLiteral("terminalSettingsResetButton"))->click();
        QVERIFY(dialog.appearance().autoEnglishInput);
    }

    void terminalPreeditIsVisibleButNeverSentUntilCommitted()
    {
        noxshell::ui::TerminalView view;
        view.resize(640, 260);
        view.show();
        view.activateWindow();
        view.setFocus();
        QTest::qWait(30);
        view.feedText(QStringLiteral("\x1b[?1049h\x1b[2J\x1b[Hvi buffer\r\n"));
        const auto contents = view.plainText();
        const auto before = view.grab().toImage();
        QSignalSpy input(&view, &noxshell::ui::TerminalView::inputGenerated);
        QSignalSpy commands(&view, &noxshell::ui::TerminalView::commandSubmitted);
        QTextCharFormat marked;
        marked.setBackground(Qt::darkBlue);
        QInputMethodEvent composing(QStringLiteral("zhongwen"), {
            {QInputMethodEvent::Cursor, 5, 1, {}},
            {QInputMethodEvent::TextFormat, 0, 5, marked}});
        QApplication::sendEvent(&view, &composing);
        QVERIFY(view.grab().toImage() != before);
        QCOMPARE(view.plainText(), contents);
        QCOMPARE(input.count(), 0);
        QCOMPARE(commands.count(), 0);

        QInputMethodQueryEvent query(Qt::ImQueryInput | Qt::ImAbsolutePosition | Qt::ImTextBeforeCursor | Qt::ImTextAfterCursor);
        QApplication::sendEvent(&view, &query);
        QVERIFY(query.value(Qt::ImSurroundingText).isValid());
        QVERIFY(query.value(Qt::ImSurroundingText).toString().isEmpty());
        QCOMPARE(query.value(Qt::ImCursorPosition).toInt(), 0);
        QVERIFY(query.value(Qt::ImCursorPosition).isValid());
        QVERIFY(query.value(Qt::ImAnchorPosition).isValid());
        QVERIFY(query.value(Qt::ImAbsolutePosition).isValid());
        QVERIFY(view.rect().contains(query.value(Qt::ImCursorRectangle).toRectF().toAlignedRect()));

        QInputMethodEvent committed;
        committed.setCommitString(QStringLiteral("中文"));
        QApplication::sendEvent(&view, &committed);
        QCOMPARE(input.count(), 1);
        QCOMPARE(input.first().at(0).toByteArray(), QStringLiteral("中文").toUtf8());
        QCOMPARE(view.plainText(), contents);
        QCOMPARE(view.grab().toImage(), before);
    }

    void terminalPreeditCancelDoesNotLeakAndViKeysStillWork()
    {
        noxshell::ui::TerminalView view;
        view.resize(640, 260);
        view.show();
        view.activateWindow();
        view.setFocus();
        QTest::qWait(20);
        QSignalSpy input(&view, &noxshell::ui::TerminalView::inputGenerated);
        const auto before = view.grab().toImage();
        QInputMethodEvent composing(QStringLiteral("weiwancheng"), {});
        QApplication::sendEvent(&view, &composing);
        QTest::keyClick(&view, Qt::Key_Escape);
        QCOMPARE(input.count(), 1);
        QCOMPARE(input.takeFirst().at(0).toByteArray(), QByteArray("\x1b"));
        QCOMPARE(view.grab().toImage(), before);

        QApplication::sendEvent(&view, &composing);
        view.clear();
        QCOMPARE(input.count(), 0);
        QCOMPARE(view.grab().toImage(), before);
        QApplication::sendEvent(&view, &composing);
        view.setEnabled(false);
        view.setEnabled(true);
        view.setFocus();
        QCOMPARE(input.count(), 0);
        QCOMPARE(view.grab().toImage(), before);

        QTest::keyClicks(&view, QStringLiteral("ihello"));
        QTest::keyClick(&view, Qt::Key_Escape);
        QTest::keyClicks(&view, QStringLiteral(":wq"));
        QTest::keyClick(&view, Qt::Key_Return);
        QByteArray bytes;
        for (const auto &event : input) bytes += event.at(0).toByteArray();
        QCOMPARE(bytes, QByteArray("ihello\x1b:wq\r"));
    }

    void terminalPreeditLongTextAndResizeKeepCandidateVisible()
    {
        noxshell::ui::TerminalView view;
        view.resize(480, 180);
        view.show();
        view.setFocus();
        view.feedText(QStringLiteral("\x1b[99;99H"));
        QInputMethodEvent composing(QString(200, QChar('x')), {{QInputMethodEvent::Cursor, 198, 1, {}}});
        QApplication::sendEvent(&view, &composing);
        for (const int width : {640, 400}) {
            view.resize(width, 240);
            QInputMethodQueryEvent query(Qt::ImCursorRectangle);
            QApplication::sendEvent(&view, &query);
            QVERIFY(view.rect().contains(query.value(Qt::ImCursorRectangle).toRectF().toAlignedRect()));
            QVERIFY(!view.grab().isNull());
        }
    }

    void terminalPreeditConfirmationAndFocusLossStayLocal()
    {
        QWidget window;
        auto *layout = new QVBoxLayout(&window);
        auto *view = new noxshell::ui::TerminalView;
        auto *other = new QLineEdit;
        layout->addWidget(view);
        layout->addWidget(other);
        window.resize(640, 300);
        window.show();
        window.activateWindow();
        view->setFocus();
        QTest::qWait(30);
        QSignalSpy input(view, &noxshell::ui::TerminalView::inputGenerated);
        const auto before = view->grab().toImage();
        QInputMethodEvent composing(QStringLiteral("unfinished"), {});
        QApplication::sendEvent(view, &composing);
        QTest::keyClick(view, Qt::Key_Return);
        for (const auto &event : input) QVERIFY(!event.at(0).toByteArray().contains('\r'));
        input.clear();
        QApplication::sendEvent(view, &composing);
        other->setFocus();
        view->setFocus();
        QCOMPARE(input.count(), 0);
        QCOMPARE(view->grab().toImage(), before);
        QApplication::sendEvent(view, &composing);
        QTest::keyClick(view, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(input.count(), 1);
        QCOMPARE(input.takeFirst().at(0).toByteArray(), QByteArray(1, '\x03'));
        QApplication::sendEvent(view, &composing);
        QApplication::clipboard()->setText(QStringLiteral("paste"));
        view->feedText(QStringLiteral("\x1b[?2004h"));
        QTest::keyClick(view, Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(input.count(), 1);
        QCOMPARE(input.first().at(0).toByteArray(), QByteArray("\x1b[200~paste\x1b[201~"));
        QCOMPARE(view->grab().toImage(), before);
    }

    void terminalInputSettingPersistsAndCancelRestores()
    {
        if (QApplication::platformName() == QStringLiteral("cocoa"))
            QSKIP("Uses the offscreen toolbar; native text input has its own probe");
        const auto original = noxshell::ui::TerminalView::defaultAppearance();
        QSettings settings;
        const auto key = QStringLiteral("terminal/autoEnglishInput");
        const auto stored = settings.value(key);
        const auto cleanup = qScopeGuard([&] {
            if (stored.isValid()) settings.setValue(key, stored); else settings.remove(key);
            noxshell::ui::TerminalView::setDefaultAppearance(original);
        });
        settings.setValue(key, true);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto database = directory.filePath(QStringLiteral("settings.sqlite"));
        MemoryCredentialStore credentials;
        {
            noxshell::ui::MainWindow window(database, nullptr, &credentials);
            auto *button = window.findChild<QToolButton *>(QStringLiteral("terminalSettingsButton"));
            QVERIFY(button);
            bool found = false;
            QTimer::singleShot(0, &window, [&] {
                auto *dialog = window.findChild<noxshell::ui::TerminalSettingsDialog *>();
                if (!dialog) return;
                auto *check = dialog->findChild<QCheckBox *>(QStringLiteral("terminalAutoEnglishInputCheck"));
                found = check != nullptr;
                if (check) check->setChecked(false);
                dialog->accept();
            });
            button->click();
            QVERIFY(found);
            QVERIFY(!settings.value(key).toBool());
            QVERIFY(!noxshell::ui::TerminalView::defaultAppearance().autoEnglishInput);
            QTimer::singleShot(0, &window, [&] {
                auto *dialog = window.findChild<noxshell::ui::TerminalSettingsDialog *>();
                if (!dialog) return;
                dialog->findChild<QCheckBox *>(QStringLiteral("terminalAutoEnglishInputCheck"))->setChecked(true);
                dialog->reject();
            });
            button->click();
            QVERIFY(!settings.value(key).toBool());
            QVERIFY(!noxshell::ui::TerminalView::defaultAppearance().autoEnglishInput);
        }
        noxshell::ui::TerminalView::setDefaultAppearance(original);
        noxshell::ui::MainWindow reopened(database, nullptr, &credentials);
        QVERIFY(!noxshell::ui::TerminalView::defaultAppearance().autoEnglishInput);
    }

    void terminalInputSourcePolicyRespectsManualChoiceAndFailures()
    {
        using namespace noxshell::ui;
        QString current = QStringLiteral("pinyin");
        QString ascii = QStringLiteral("abc");
        QStringList selections;
        bool succeeds = true;
        TerminalInputSourceBackend backend{
            [&] { return current; }, [&] { return current == ascii; }, [&] { return ascii; },
            [&](const QString &source) { selections << source; if (succeeds) current = source; return succeeds; }};
        TerminalInputSourceScope scope;
        scope.enter(false, backend);
        QVERIFY(selections.isEmpty());
        scope.enter(true, backend);
        QCOMPARE(current, ascii);
        scope.enter(true, backend);
        QCOMPARE(selections.size(), 1);
        scope.leave(true, backend);
        QCOMPARE(current, QStringLiteral("pinyin"));
        scope.enter(true, backend);
        current = QStringLiteral("manual-chinese");
        scope.leave(true, backend);
        QCOMPARE(current, QStringLiteral("manual-chinese"));
        scope.enter(true, backend);
        scope.leave(false, backend); // Never change another app's input source.
        QCOMPARE(current, ascii);
        selections.clear();
        scope.enter(true, backend); // Already ASCII: no unnecessary selection.
        scope.leave(true, backend);
        QVERIFY(selections.isEmpty());
        current = QStringLiteral("pinyin");
        succeeds = false;
        scope.enter(true, backend);
        scope.leave(true, backend);
        QCOMPARE(selections.size(), 1);
        QCOMPARE(current, QStringLiteral("pinyin"));
        selections.clear();
        ascii.clear();
        scope.enter(true, backend);
        QVERIFY(selections.isEmpty());
    }

    void terminalNativeMarkedTextEscapeAndCommit()
    {
#ifdef Q_OS_MACOS
        if (QApplication::platformName() != QStringLiteral("cocoa")) QSKIP("Requires native Cocoa text input");
        noxshell::ui::TerminalView view;
        view.resize(640, 260);
        view.show();
        view.activateWindow();
        view.setFocus();
        QVERIFY(QTest::qWaitForWindowActive(&view));
        QSignalSpy input(&view, &noxshell::ui::TerminalView::inputGenerated);
        const auto before = view.grab().toImage();
        QVERIFY(nativeProbeTerminalMarkText(&view, QStringLiteral("pinyin")));
        QCOMPARE(input.count(), 0);
        QVERIFY(view.grab().toImage() != before);
        QVERIFY(nativeProbeTerminalEscape(&view));
        QCOMPARE(input.count(), 1);
        QCOMPARE(input.takeFirst().at(0).toByteArray(), QByteArray("\x1b"));
        QCOMPARE(view.grab().toImage(), before);
        QVERIFY(nativeProbeTerminalMarkText(&view, QStringLiteral("zhongwen")));
        QVERIFY(nativeProbeTerminalCommitText(&view, QStringLiteral("中文")));
        QCOMPARE(input.count(), 1);
        QCOMPARE(input.first().at(0).toByteArray(), QStringLiteral("中文").toUtf8());
        input.clear();
        // The native route must preserve Qt object filters: an open history
        // panel consumes its first Esc; only the next Esc reaches the session.
        class ConsumeEscape final : public QObject {
            bool eventFilter(QObject *, QEvent *event) override {
                return event->type() == QEvent::KeyPress
                    && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape;
            }
        } filter;
        view.installEventFilter(&filter);
        QVERIFY(nativeProbeTerminalEscape(&view));
        QCOMPARE(input.count(), 0);
        view.removeEventFilter(&filter);
        QVERIFY(nativeProbeTerminalEscape(&view));
        QCOMPARE(input.count(), 1);
        QCOMPARE(input.first().at(0).toByteArray(), QByteArray("\x1b"));
#else
        QSKIP("Requires macOS");
#endif
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
        QCOMPARE(tree->columnCount(), 7);
        QVERIFY(!tree->isHeaderHidden());
        QCOMPARE(hostItem->text(0), QStringLiteral("sidebar-host"));
        QCOMPARE(hostItem->text(1), QStringLiteral("192.0.2.10"));
        QCOMPARE(hostItem->text(2), QStringLiteral("22"));
        QCOMPARE(hostItem->text(4), QStringLiteral("SSH"));
        QCOMPARE(hostItem->text(5), QStringLiteral("密码"));
        QVERIFY(hostItem->sizeHint(0).height() <= 42);
        QVERIFY(!hostItem->toolTip(0).contains(QStringLiteral("离线")));
        QVERIFY(!hostItem->toolTip(0).contains(QStringLiteral("在线")));
        QVERIFY(sidebar.setServerState(profile.id, noxshell::ServerState::Online));
        tree->itemDoubleClicked(hostItem, 1);
        QCOMPARE(connectSpy.count(), 1);
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
        QCOMPARE(hostItem->text(0), QStringLiteral("无分组主机"));
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

    void terminalCloseConfirmation_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }

    void terminalCloseConfirmation()
    {
        QFETCH(bool, dark);
        const auto previousTheme = noxshell::ui::storedThemeMode();
        const auto restoreTheme = qScopeGuard([previousTheme] { noxshell::ui::applyApplicationTheme(previousTheme); });
        noxshell::ui::applyApplicationTheme(dark ? noxshell::ui::ThemeMode::Dark : noxshell::ui::ThemeMode::Light);
        noxshell::ui::TerminalWorkspace workspace(nullptr, nullptr);
        workspace.resize(900, 500);
        workspace.show();
        QList<noxshell::ServerProfile> profiles;
        for (int i = 0; i < 5; ++i) {
            noxshell::ServerProfile profile;
            profile.id = QStringLiteral("close-confirm-%1").arg(i);
            profile.name = QStringLiteral("服务器 <b>%1</b>").arg(i);
            profile.connectionMode = noxshell::ConnectionMode::Demo;
            profiles.append(profile);
            if (i < 3) workspace.openOrActivate(profile, true);
        }
        auto *tabs = workspace.findChild<QTabBar *>(QStringLiteral("terminalSessionTabs"));
        auto *stack = workspace.findChild<QStackedWidget *>(QStringLiteral("terminalSessionStack"));
        QVERIFY(tabs);
        QVERIFY(stack);
        auto *firstSession = stack->widget(0)->findChild<noxshell::SshSession *>();
        auto *firstOutput = stack->widget(0)->findChild<noxshell::ui::TerminalView *>();
        QVERIFY(firstSession);
        QVERIFY(firstOutput);
        QTRY_VERIFY_WITH_TIMEOUT(firstSession->isConnected(), 1000);
        firstOutput->feedText(QStringLiteral("must-survive-cancel"));
        QSignalSpy closed(&workspace, &noxshell::ui::TerminalWorkspace::sessionClosed);
        auto *firstClose = tabs->tabButton(0, QTabBar::RightSide)
            ->findChild<QToolButton *>(QStringLiteral("terminalTabCloseButton"));
        QVERIFY(firstClose);
        {
            CloseDialogResponder responder(QMessageBox::Cancel);
            bool captured = true;
            responder.beforeAnswer = [&](QMessageBox *dialog) {
                const auto captureDir = qEnvironmentVariable("NOXSHELL_CLOSE_SCREENSHOT_DIR");
                if (captureDir.isEmpty()) return;
                captured = QDir().mkpath(captureDir) && dialog->grab().save(QDir(captureDir).filePath(
                    dark ? QStringLiteral("tab-close-dark.png") : QStringLiteral("tab-close-light.png")));
            };
            firstClose->click();
            QCOMPARE(responder.count, 1);
            QVERIFY(captured);
            QVERIFY(responder.safeDefault);
            QCOMPARE(responder.names, QStringList{QStringLiteral("terminalCloseConfirmation")});
            QVERIFY(responder.texts.first().contains(profiles.first().name));
        }
        {
            CloseDialogResponder responder;
            responder.escape = true;
            tabs->tabCloseRequested(0);
            QCOMPARE(responder.count, 1);
        }
        QCOMPARE(tabs->count(), 3);
        QVERIFY(firstSession->isConnected());
        QVERIFY(firstOutput->plainText().contains(QStringLiteral("must-survive-cancel")));
        QVERIFY(closed.isEmpty());
        const auto prepare = [&workspace](int index) {
            bool prepared = false;
            return QMetaObject::invokeMethod(&workspace, "prepareTabContextMenu", Qt::DirectConnection,
                Q_RETURN_ARG(bool, prepared), Q_ARG(int, index)) && prepared;
        };
        auto *closeCurrent = workspace.findChild<QAction *>(QStringLiteral("terminalCloseCurrentAction"));
        auto *closeOthers = workspace.findChild<QAction *>(QStringLiteral("terminalCloseOthersAction"));
        auto *closeAll = workspace.findChild<QAction *>(QStringLiteral("terminalCloseAllAction"));
        QVERIFY(closeCurrent);
        QVERIFY(closeOthers);
        QVERIFY(closeAll);
        QVERIFY(prepare(0));
        for (auto *action : {closeCurrent, closeOthers, closeAll}) {
            CloseDialogResponder responder(QMessageBox::Cancel);
            action->trigger();
            QCOMPARE(responder.count, 1);
            QCOMPARE(tabs->count(), 3);
            QVERIFY(closed.isEmpty());
            QVERIFY(firstSession->isConnected());
        }
        {
            CloseDialogResponder responder;
            bool plainText = false;
            responder.beforeAnswer = [&](QMessageBox *dialog) {
                plainText = dialog->textFormat() == Qt::PlainText;
                // A repeated close while confirming cannot create another dialog.
                tabs->tabCloseRequested(1);
                workspace.openOrActivate(profiles.at(3), false);
            };
            closeCurrent->trigger();
            QCOMPARE(responder.count, 1);
            QVERIFY(plainText);
        }
        QCOMPARE(tabs->count(), 3);
        QCOMPARE(closed.size(), 1);
        QCOMPARE(tabs->tabData(0).toString(), profiles.at(1).id);
        QCOMPARE(tabs->tabData(2).toString(), profiles.at(3).id);
        QVERIFY(prepare(0));
        {
            CloseDialogResponder responder;
            responder.beforeAnswer = [&](QMessageBox *) {
                // Asynchronous removal shifts indices; new tabs are not part of
                // the batch the user agreed to close.
                workspace.closeServer(profiles.at(2).id);
                workspace.openOrActivate(profiles.at(4), false);
            };
            closeOthers->trigger();
            QCOMPARE(responder.count, 1);
        }
        QCOMPARE(tabs->count(), 2);
        QCOMPARE(tabs->tabData(0).toString(), profiles.at(1).id);
        QCOMPARE(tabs->tabData(1).toString(), profiles.at(4).id);
        QCOMPARE(closed.size(), 3);
        QVERIFY(prepare(0));
        QCOMPARE(answerClose([&] { closeAll->trigger(); }), 1);
        QCOMPARE(tabs->count(), 0);
        QCOMPARE(closed.size(), 5);
    }

    void applicationWindowLifecycle()
    {
        QProcess probe;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.remove(QStringLiteral("NOXSHELL_SSH_TEST_ENDPOINT"));
        environment.remove(QStringLiteral("NOXSHELL_TEST_NATIVE_KEYCHAIN"));
        probe.setProcessEnvironment(environment);
        probe.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--window-lifecycle-probe")});
        QVERIFY(probe.waitForStarted(2000));
        QVERIFY(probe.waitForFinished(12000));
        const QByteArray output = probe.readAllStandardOutput() + probe.readAllStandardError();
        QCOMPARE(probe.exitStatus(), QProcess::NormalExit);
        QVERIFY2(probe.exitCode() == 0, qPrintable(QStringLiteral("exit %1: %2").arg(probe.exitCode()).arg(QString::fromUtf8(output))));
        QVERIFY(output.contains("NOXSHELL_WINDOW_LIFECYCLE_OK"));
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
        auto *homeTabs = workspace.findChild<QTabWidget *>(QStringLiteral("connectionHomeTabs"));
        QVERIFY(homeTabs);
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
        QCOMPARE(homeTabs->currentIndex(), 1);
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
        QCOMPARE(answerClose([&] { closeOthersAction->trigger(); }), 1);
        QCOMPARE(tabs->count(), 1);
        QCOMPARE(tabs->tabText(0), QStringLiteral("第二台"));

        QVERIFY(prepareContext(0));
        duplicateAction->trigger();
        QCOMPARE(tabs->count(), 2);
        auto *secondCloseButton = tabs->tabButton(1, QTabBar::RightSide)
                                      ->findChild<QToolButton *>(QStringLiteral("terminalTabCloseButton"));
        QVERIFY(secondCloseButton);
        QCOMPARE(answerClose([&] { secondCloseButton->click(); }), 1);
        QCOMPARE(tabs->count(), 1);

        QVERIFY(prepareContext(0));
        duplicateAction->trigger();
        QCOMPARE(tabs->count(), 2);
        QVERIFY(prepareContext(0));
        QCOMPARE(answerClose([&] { closeAllAction->trigger(); }), 1);
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

    void reconnectFromOldTabUsesEditedCredentialReference()
    {
        MemoryCredentialStore credentials;
        noxshell::ui::TerminalWorkspace workspace(nullptr, &credentials);
        noxshell::ServerProfile original;
        original.id = QStringLiteral("edited-password-reconnect");
        original.name = original.id;
        original.host = QStringLiteral("192.0.2.10");
        original.user = QStringLiteral("test");
        original.connectionMode = noxshell::ConnectionMode::Ssh;
        original.credentialRef = QStringLiteral("old-password-reference");
        credentials.secrets.insert(original.credentialRef, {QStringLiteral("old-test-password"), {}});
        workspace.openOrActivate(original, false);
        auto *session = workspace.findChild<noxshell::SshSession *>();
        QVERIFY(session);
        QObject::disconnect(session, &noxshell::SshSession::connectRequested, nullptr, nullptr);
        QSignalSpy requests(session, &noxshell::SshSession::connectRequested);
        workspace.openOrActivate(original, true);
        QCOMPARE(requests.size(), 1);
        session->disconnectFromHost();

        auto edited = original;
        edited.credentialRef = QStringLiteral("new-password-reference");
        credentials.secrets.insert(edited.credentialRef, {QStringLiteral("new-test-password"), {}});
        workspace.updateServer(edited);
        bool prepared = false;
        QVERIFY(QMetaObject::invokeMethod(&workspace, "prepareTabContextMenu", Qt::DirectConnection,
            Q_RETURN_ARG(bool, prepared), Q_ARG(int, 0)));
        QVERIFY(prepared);
        auto *connectAction = workspace.findChild<QAction *>(QStringLiteral("terminalConnectAction"));
        QVERIFY(connectAction && connectAction->isEnabled());
        connectAction->trigger();
        QCOMPARE(requests.size(), 2);
        QCOMPARE(qvariant_cast<noxshell::ServerProfile>(requests.last().at(0)).password, QStringLiteral("new-test-password"));
        QCOMPARE(qvariant_cast<noxshell::ServerProfile>(requests.last().at(0)).credentialRef, edited.credentialRef);
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
        card.resize(220, 44);
        card.setValue(QStringLiteral("36.3%"), QStringLiteral("内核态 15.8%"), 36);
        card.setCoreValues({12.0, 34.0, 56.0, 78.0});
        card.show();
        QCoreApplication::processEvents();

        auto *summaryProgress = card.findChild<QProgressBar *>(QStringLiteral("metricProgress"));
        auto *corePanel = card.findChild<QFrame *>(QStringLiteral("metricCorePanel"));
        QVERIFY(summaryProgress);
        QVERIFY(corePanel);
        QCOMPARE(summaryProgress->height(), 4);
        QVERIFY(!summaryProgress->isTextVisible());
        auto *value = card.findChild<QLabel *>(QStringLiteral("metricValue"));
        auto *detail = card.findChild<QLabel *>(QStringLiteral("metricDetail"));
        auto *title = card.findChild<QLabel *>(QStringLiteral("metricTitle"));
        QVERIFY(value);
        QVERIFY(detail);
        QVERIFY(title);
        QCOMPARE(value->text(), QStringLiteral("36.3%"));
        QCOMPARE(detail->text(), QStringLiteral("内核态 15.8%"));
        QCOMPARE(title->geometry().center().y(), detail->geometry().center().y());
        QCOMPARE(value->geometry().center().y(), detail->geometry().center().y());
        QVERIFY(title->geometry().right() < detail->geometry().left());
        QVERIFY(detail->geometry().right() < value->geometry().left());
        QVERIFY(detail->alignment() & Qt::AlignLeft);
        QVERIFY(detail->geometry().bottom() < summaryProgress->geometry().top());
        QVERIFY(summaryProgress->format().contains(QStringLiteral("36.3%")));
        QVERIFY(summaryProgress->styleSheet().contains(QStringLiteral("border-radius:2px")));
        QEvent initialLeaveEvent(QEvent::Leave);
        QApplication::sendEvent(&card, &initialLeaveEvent);
        QVERIFY(!corePanel->isVisible());

        QEnterEvent enterEvent(QPointF(10, 10), QPointF(10, 10), QPointF(10, 10));
        QApplication::sendEvent(&card, &enterEvent);
        QVERIFY(corePanel->isVisible());
        QVERIFY(card.height() > 44);
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
        QCOMPARE(card.height(), 44);

        // A changing numeric value must reallocate inline detail space even
        // when the card width stays unchanged between live samples.
        const auto longDetail = QStringLiteral("内核态 99.9% · 较长说明用于验证省略");
        const int cardWidth = card.width();
        card.setValue(QStringLiteral("100.0%"), longDetail, 100);
        QCoreApplication::processEvents();
        const int narrowDetailWidth = detail->width();
        QCOMPARE(detail->toolTip(), longDetail);
        QVERIFY(detail->fontMetrics().horizontalAdvance(detail->text()) <= detail->width());
        QVERIFY(detail->geometry().right() < value->geometry().left());
        card.setValue(QStringLiteral("1%"), longDetail, 1);
        QCoreApplication::processEvents();
        QCOMPARE(card.width(), cardWidth);
        QVERIFY(detail->width() > narrowDetailWidth);
        QVERIFY(detail->fontMetrics().horizontalAdvance(detail->text()) <= detail->width());
        QCOMPARE(value->text(), QStringLiteral("1%"));
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
        QVERIFY(interfaces->width() >= 100);
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

    void networkChartUsesTimeWindowAndKeepsSamplesUnchanged()
    {
        noxshell::ui::SystemDetailPanel panel;
        auto *widget = panel.findChild<QWidget *>(QStringLiteral("networkRateChart"));
        auto *chart = static_cast<noxshell::ui::NetworkRateChart *>(widget);
        QVERIFY(chart);
        noxshell::MetricSample sample;
        const auto start = QDateTime::fromMSecsSinceEpoch(100000);
        for (int i = 0; i <= 20; ++i) {
            sample.capturedAt = start.addSecs(i * 5);
            sample.networkRates = {{QStringLiteral("eth0"), 1000.0 + i, 2000.0 + i}};
            panel.setSample(sample);
        }
        QCOMPARE(chart->rates().size(), 13); // 60 seconds, not 60 samples (300 seconds).
        QCOMPARE(chart->rates().first().timestampMs, sample.capturedAt.addSecs(-60).toMSecsSinceEpoch());
        QCOMPARE(chart->rates().last().upload, 2020.0);
        panel.setSample(sample);
        QCOMPARE(chart->rates().size(), 13); // Repainting the same sample must not append a point.
        sample.capturedAt = start;
        panel.setSample(sample);
        QCOMPARE(chart->rates().size(), 1); // Clock/reset going backwards does not draw backwards.
        panel.reset(QStringLiteral("未连接"));
        QVERIFY(chart->rates().isEmpty());

        chart->resize(236, 92);
        chart->setRates({{100000, 100, 80}, {120000, 900, 500}, {140000, 200, 100}, {160000, 1200, 900}}, 160000);
        QCOMPARE(chart->rates().at(1).upload, 900.0); // No numeric smoothing or peak loss.
        const auto rendered = chart->grab().toImage();
        int coloredPixels = 0;
        for (int y = 8; y < 72; ++y) for (int x = 49; x < 226; ++x) {
            const auto color = rendered.pixelColor(x, y);
            if (color.blue() > color.red() + 45 || color.green() > color.red() + 45) ++coloredPixels;
        }
        QVERIFY(coloredPixels > 200); // A connected curve, not just one end marker.
    }

    void monitorRailVisualLayout_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<int>("railWidth");
        for (const bool dark : {false, true}) for (const int width : {236, 252, 360})
            QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(dark ? "dark" : "light").arg(width))) << dark << width;
    }

    void monitorRailVisualLayout()
    {
        QFETCH(bool, dark);
        QFETCH(int, railWidth);
        const auto previous = noxshell::ui::isApplicationDarkTheme();
        auto restore = qScopeGuard([&] { noxshell::ui::applyApplicationTheme(previous
            ? noxshell::ui::ThemeMode::Dark : noxshell::ui::ThemeMode::Light); });
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        MemoryCredentialStore credentials;
        noxshell::ui::MainWindow window(directory.filePath(QStringLiteral("preview.sqlite")), nullptr, &credentials);
        noxshell::ui::applyApplicationTheme(dark ? noxshell::ui::ThemeMode::Dark : noxshell::ui::ThemeMode::Light);
        window.resize(1000, 1000);
        auto *rail = window.findChild<QWidget *>(QStringLiteral("monitorRail"));
        QVERIFY(rail);
        rail->setFixedWidth(railWidth);
        window.findChild<QLabel *>(QStringLiteral("serverAddress"))->setText(QStringLiteral("203.0.113.8"));
        window.findChild<QLabel *>(QStringLiteral("monitorUptimeValue"))->setText(QStringLiteral("704 天 23 小时"));
        auto *summary = window.findChild<QFrame *>(QStringLiteral("monitorMetricSummary"));
        auto rows = summary->findChildren<noxshell::ui::MetricCard *>(QStringLiteral("metricRow"), Qt::FindDirectChildrenOnly);
        QCOMPARE(rows.size(), 3);
        rows[0]->setValue(QStringLiteral("19.1%"), QStringLiteral("内核态 2.0%"), 19);
        rows[1]->setValue(QStringLiteral("25.9%"), QStringLiteral("0.5 / 2.0 GiB"), 26);
        rows[2]->setValue(QStringLiteral("0.15"), QStringLiteral("1m / 1 核 · 5m 0.17"), 15);
        auto *panel = window.findChild<noxshell::ui::SystemDetailPanel *>();
        noxshell::MetricSample sample;
        const auto start = QDateTime::fromMSecsSinceEpoch(100000);
        sample.processes = {
            {20790, "root", 3.0, 2.0, 1024, "sshd"}, {22046, "root", 1.5, 4.0, 2048, "worker"},
            {22038, "root", 0.5, 1.0, 1024, "nginx"}, {22035, "root", 0.4, 1.0, 1024, "cache"}};
        for (const auto &path : {"/", "/dev", "/dev/shm", "/run", "/run/user/0", "/sys/fs/cgroup"})
            sample.disks.append({"ext4", QString::fromLatin1(path), 40ULL << 30, 3ULL << 30, 37ULL << 30, 8});
        for (int i = 0; i <= 60; ++i) {
            sample.capturedAt = start.addSecs(i);
            const double peak = i % 15 == 0 ? 50000 : 0;
            sample.networkRates = {{"eth0", 10000.0 + (i % 12) * 3000 + peak, 8000.0 + (i % 9) * 2400 + peak}};
            panel->setSample(sample);
        }
        window.show();
        QTest::qWait(25);
        for (auto *row : rows) {
            auto *detail = row->findChild<QLabel *>(QStringLiteral("metricDetail"));
            auto *title = row->findChild<QLabel *>(QStringLiteral("metricTitle"));
            auto *value = row->findChild<QLabel *>(QStringLiteral("metricValue"));
            auto *progress = row->findChild<QProgressBar *>(QStringLiteral("metricProgress"));
            QCOMPARE(row->height(), 44);
            QCOMPARE(title->geometry().center().y(), detail->geometry().center().y());
            QCOMPARE(value->geometry().center().y(), detail->geometry().center().y());
            QVERIFY(title->geometry().right() < detail->geometry().left());
            QVERIFY(detail->geometry().right() < value->geometry().left());
            QVERIFY(value->width() >= value->sizeHint().width());
            QVERIFY(detail->fontMetrics().horizontalAdvance(detail->text()) <= detail->width());
            QVERIFY(detail->geometry().bottom() < progress->geometry().top());
            QVERIFY(progress->width() > 120);
        }
        const auto *network = panel->findChild<QFrame *>(QStringLiteral("networkSectionCard"));
        QVERIFY(panel->findChildren<QLabel *>(QStringLiteral("networkRateCaption")).isEmpty());
        const auto *upload = panel->findChild<QLabel *>(QStringLiteral("networkUploadRate"));
        const auto *download = panel->findChild<QLabel *>(QStringLiteral("networkDownloadRate"));
        QCOMPARE(upload->y(), download->y());
        QCOMPARE(upload->parentWidget(), download->parentWidget());
        QVERIFY(upload->parentWidget()->height() <= 24);
        QCOMPARE(upload->accessibleName(), QStringLiteral("上传速率"));
        QCOMPARE(download->accessibleName(), QStringLiteral("下载速率"));
        const auto *processes = panel->findChild<QTreeWidget *>(QStringLiteral("realtimeProcessList"));
        QCOMPARE(network->mapTo(rail, QPoint()).x(), summary->mapTo(rail, QPoint()).x());
        QCOMPARE(network->width(), summary->width());
        QVERIFY(processes->columnWidth(0) >= 60);
        QVERIFY(processes->topLevelItem(0)->textAlignment(1) & Qt::AlignRight);
        const auto screenshots = qEnvironmentVariable("NOXSHELL_MONITOR_SCREENSHOT_DIR");
        if (!screenshots.isEmpty()) QVERIFY(rail->grab().save(QDir(screenshots).filePath(
            QStringLiteral("monitor-%1-%2.png").arg(dark ? "dark" : "light").arg(railWidth))));
        rows[0]->setValue(QStringLiteral("--"), QStringLiteral("等待连接：这是一段用于验证窄窗口省略与提示的较长状态说明").repeated(3), 0);
        QCoreApplication::processEvents();
        auto *detail = rows[0]->findChild<QLabel *>(QStringLiteral("metricDetail"));
        QVERIFY(detail->text().size() < detail->toolTip().size());
        QVERIFY(rows[0]->width() <= railWidth);
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

    void remoteEditorInitialLoadKeepsTextOutsideGutter_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }

    void remoteEditorInitialLoadKeepsTextOutsideGutter()
    {
        QFETCH(bool, dark);
        noxshell::ui::applyApplicationTheme(dark ? noxshell::ui::ThemeMode::Dark : noxshell::ui::ThemeMode::Light);
        const auto themeReset = qScopeGuard([] { noxshell::ui::applyApplicationTheme(noxshell::ui::ThemeMode::Light); });
        noxshell::SshSession session(nullptr, nullptr);
        noxshell::ServerProfile profile;
        profile.name = QStringLiteral("演示服务器");
        profile.connectionMode = noxshell::ConnectionMode::Demo;
        session.connectTo(profile);
        QTRY_VERIFY_WITH_TIMEOUT(session.isConnected(), 1000);
        const QString path = QStringLiteral("/var/www/app/config.yml");
        QByteArray content("services:\n  - name: proxy.8001\n    addr: \"127.0.0.1:8001\"\n    enabled: true\n    # Local display fixture only\n");
        for (int line = 0; line < 1000; ++line) content += "    item_" + QByteArray::number(line) + ": 42\n";
        QSignalSpy written(&session, &noxshell::SshSession::remoteFileWritten);
        session.writeFile(path, content, false);
        QTRY_COMPARE_WITH_TIMEOUT(written.count(), 1, 1000);
        noxshell::ui::RemoteFileEditor window(&session, profile.name, path);
        window.setAttribute(Qt::WA_DeleteOnClose, false);
        auto *editor = window.findChild<QPlainTextEdit *>(QStringLiteral("remoteFileEditorText"));
        auto *gutter = window.findChild<QWidget *>(QStringLiteral("remoteFileEditorLineNumbers"));
        QVERIFY(editor);
        QVERIFY(gutter);
        bool initialGeometryChecked = false;
        bool initialGeometryCorrect = false;
        connect(&session, &noxshell::SshSession::remoteFileRead, &window, [&](quint64, const QString &readPath, const QByteArray &) {
            if (readPath != path) return;
            initialGeometryChecked = true;
            // Check synchronously after the real load callback, BEFORE scrolling,
            // resizing, grabbing, or another event can accidentally repair it.
            const int minimumGutter = 14 + editor->fontMetrics().horizontalAdvance(QString::number(editor->blockCount()));
            initialGeometryCorrect = editor->viewport()->geometry().left() > gutter->geometry().right()
                && gutter->width() >= minimumGutter;
            qInfo() << "initial editor viewport/gutter" << editor->viewport()->geometry() << gutter->geometry();
        });
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(initialGeometryChecked, 1000);
        QCOMPARE(editor->toPlainText().toUtf8(), content);
        QCOMPARE(editor->verticalScrollBar()->value(), 0);
        QCOMPARE(editor->horizontalScrollBar()->value(), 0);
        QVERIFY2(initialGeometryCorrect, "The line-number gutter overlaps the first characters on initial load");
        QCoreApplication::processEvents();
        QVERIFY(editor->viewport()->geometry().left() > gutter->geometry().right());
        QVERIFY(editor->cursorRect().left() >= 0);
        QVERIFY(editor->cursorRect().top() >= 0);
        QCOMPARE(editor->fontMetrics().horizontalAdvance(QLatin1Char('i')),
            editor->fontMetrics().horizontalAdvance(QLatin1Char('W')));
        auto *save = window.findChild<QPushButton *>(QStringLiteral("remoteFileEditorSave"));
        auto *undo = window.findChild<QToolButton *>(QStringLiteral("remoteFileEditorUndo"));
        auto *redo = window.findChild<QToolButton *>(QStringLiteral("remoteFileEditorRedo"));
        auto *wrap = window.findChild<QToolButton *>(QStringLiteral("remoteFileEditorWrap"));
        auto *find = window.findChild<QToolButton *>(QStringLiteral("remoteFileEditorFind"));
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("remoteFileEditorTabs"));
        auto *position = window.findChild<QLabel *>(QStringLiteral("remoteFileEditorPosition"));
        auto *state = window.findChild<QLabel *>(QStringLiteral("remoteFileEditorState"));
        QVERIFY(save && undo && redo && wrap && find && tabs && position && state);
        QVERIFY(!save->isEnabled());
        QVERIFY(tabs->tabIcon(0).isNull());
        QVERIFY(position->text().contains(QStringLiteral("行 1，列 1")));
        QVERIFY(!editor->document()->firstBlock().layout()->formats().isEmpty());
        const auto captureDir = qEnvironmentVariable("NOXSHELL_EDITOR_CAPTURE_DIR");
        if (!captureDir.isEmpty()) {
            QVERIFY(QDir().mkpath(captureDir));
            QVERIFY(window.grab().save(captureDir + (dark ? QStringLiteral("/dark.png") : QStringLiteral("/light.png"))));
        }
        // A live theme change must update the gutter/highlight without editing
        // content, scrolling the document, or showing an unsaved marker.
        noxshell::ui::applyApplicationTheme(dark ? noxshell::ui::ThemeMode::Light : noxshell::ui::ThemeMode::Dark);
        QCoreApplication::processEvents();
        QCOMPARE(editor->toPlainText().toUtf8(), content);
        QVERIFY(tabs->tabIcon(0).isNull());
        QVERIFY(!save->isEnabled());
        QVERIFY(editor->viewport()->geometry().left() > gutter->geometry().right());
        noxshell::ui::applyApplicationTheme(dark ? noxshell::ui::ThemeMode::Dark : noxshell::ui::ThemeMode::Light);
        window.resize(600, 420);
        QCoreApplication::processEvents();
        QVERIFY(editor->viewport()->geometry().left() > gutter->geometry().right());
        QVERIFY(save->isVisible());
        QVERIFY(window.rect().contains(save->mapTo(&window, save->rect().bottomRight())));
        QTest::mouseClick(wrap, Qt::LeftButton);
        QCOMPARE(editor->lineWrapMode(), QPlainTextEdit::WidgetWidth);
        QCOMPARE(editor->toPlainText().toUtf8(), content);
        QVERIFY(!save->isEnabled());
        QTest::mouseClick(wrap, Qt::LeftButton);
        QTest::mouseClick(find, Qt::LeftButton);
        QVERIFY(window.findChild<QWidget *>(QStringLiteral("remoteFileFindPanel"))->isVisible());
        QTest::mouseClick(window.findChild<QToolButton *>(QStringLiteral("remoteFileFindClose")), Qt::LeftButton);
        editor->insertPlainText(QStringLiteral("# edited\n"));
        QVERIFY(save->isEnabled());
        QVERIFY(undo->isEnabled());
        QCOMPARE(state->text(), QStringLiteral("未保存"));
        QTest::mouseClick(undo, Qt::LeftButton);
        QCOMPARE(editor->toPlainText().toUtf8(), content);
        QVERIFY(redo->isEnabled());
        QTest::mouseClick(redo, Qt::LeftButton);
        QVERIFY(editor->toPlainText().startsWith(QStringLiteral("# edited\n")));
        QTest::mouseClick(save, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!save->isEnabled() && tabs->tabIcon(0).isNull(), 1000);
        QCOMPARE(state->text(), QStringLiteral("已同步"));
        // Crossing line-number digit boundaries must stay aligned immediately.
        for (const int lines : {9, 10, 99, 100, 999, 1000}) {
            editor->setPlainText(QStringLiteral("key: value\n").repeated(lines - 1) + QStringLiteral("last: true"));
            QCOMPARE(editor->blockCount(), lines);
            QVERIFY(editor->viewport()->geometry().left() > gutter->geometry().right());
            QVERIFY(gutter->width() >= 14 + editor->fontMetrics().horizontalAdvance(QString::number(lines)));
        }
    }

    void remoteEditorHighlightingIsBoundedAndDoesNotRewriteFiles()
    {
        noxshell::SshSession session(nullptr, nullptr);
        noxshell::ServerProfile profile;
        profile.name = QStringLiteral("syntax-fixture");
        profile.connectionMode = noxshell::ConnectionMode::Demo;
        session.connectTo(profile);
        QTRY_VERIFY_WITH_TIMEOUT(session.isConnected(), 1000);
        const QList<QPair<QString, QByteArray>> fixtures{
            {QStringLiteral("/var/www/app/sample.json"), QByteArray("{\"url\": \"https://example.invalid/#keep\", \"enabled\": true, \"limit\": 42}\n")},
            {QStringLiteral("/var/www/app/sample.ini"), QByteArray("[network]\nport = 8001\npassword = \"not#comment\"\n# comment\n")},
            {QStringLiteral("/var/www/app/sample.log"), QByteArray("plain text: no syntax modifications\n")},
            {QStringLiteral("/var/www/app/long-line.yml"), QByteArray("key: ") + QByteArray(9000, 'a')},
            {QStringLiteral("/var/www/app/large.yml"), QByteArray("key: true\n").repeated(60000)}
        };
        QSignalSpy writes(&session, &noxshell::SshSession::remoteFileWritten);
        for (const auto &fixture : fixtures) {
            const int before = writes.count();
            session.writeFile(fixture.first, fixture.second, false);
            QTRY_COMPARE_WITH_TIMEOUT(writes.count(), before + 1, 1000);
        }
        for (int index = 0; index < fixtures.size(); ++index) {
            const auto &fixture = fixtures.at(index);
            noxshell::ui::RemoteFileEditor window(&session, profile.name, fixture.first);
            window.setAttribute(Qt::WA_DeleteOnClose, false);
            window.show();
            auto *editor = window.findChild<QPlainTextEdit *>(QStringLiteral("remoteFileEditorText"));
            auto *save = window.findChild<QPushButton *>(QStringLiteral("remoteFileEditorSave"));
            QTRY_VERIFY_WITH_TIMEOUT(editor->isEnabled(), 3000);
            QCoreApplication::processEvents();
            QCOMPARE(editor->toPlainText().toUtf8(), fixture.second);
            QVERIFY(!save->isEnabled());
            const auto formats = editor->document()->firstBlock().layout()->formats();
            if (index < 2) QVERIFY(!formats.isEmpty());
            else QVERIFY(formats.isEmpty()); // Logs, huge lines and huge files stay cheap.
            QTest::mouseClick(window.findChild<QToolButton *>(QStringLiteral("remoteFileEditorWrap")), Qt::LeftButton);
            QCOMPARE(editor->toPlainText().toUtf8(), fixture.second);
            QVERIFY(!save->isEnabled());
        }
        QCOMPARE(writes.count(), fixtures.size()); // Viewing/styling never writes to the server.
    }

    void directorySizeCommandQuotesPathsAndParsesSafely()
    {
        quint64 bytes = 99;
        QVERIFY(noxshell::detail::parseDirectorySize("123\t/path\nwith newline\n", bytes));
        QCOMPARE(bytes, quint64(123 * 1024));
        QVERIFY(noxshell::detail::parseDirectorySize("0\t/empty\n", bytes));
        QCOMPARE(bytes, quint64(0));
        for (const auto &invalid : {QByteArray("-1\t/path"), QByteArray("oops\n12\t/path"),
                 QByteArray("18446744073709551615\t/path"), QByteArray("123"), QByteArray("12 MB")})
            QVERIFY(!noxshell::detail::parseDirectorySize(invalid, bytes));
        QVERIFY(noxshell::detail::directorySizeCommand(QStringLiteral("relative")).isEmpty());
        QVERIFY(noxshell::detail::directorySizeCommand(QStringLiteral("/null") + QChar::Null).isEmpty());
        const auto command = noxshell::detail::directorySizeCommand(QStringLiteral("/a'b $();\n目录"));
        QVERIFY(command.contains("p='/a'\\''b $();\n"));
        QVERIFY(command.contains("du -skx --"));
        QVERIFY(command.contains("nice -n 19"));
        QVERIFY(command.contains("ionice -c 3"));
        QVERIFY(command.contains("timeout -k 2 60"));
        QVERIFY(command.contains("set -- sh -c")); // Probing is bounded along with du itself.
        QVERIFY(command.contains("read -r cancel <&3"));
        QVERIFY(!command.contains("sudo"));
    }

    void directorySizeRemoteWrapperStopsChildOnCancel()
    {
#ifdef Q_OS_WIN
        QSKIP("POSIX remote wrapper tested on Unix hosts");
#else
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto bin = directory.filePath(QStringLiteral("bin"));
        QVERIFY(QDir().mkpath(bin));
        const auto script = [&](const QString &name, const QByteArray &body) {
            QFile file(bin + QLatin1Char('/') + name);
            if (!file.open(QIODevice::WriteOnly) || file.write("#!/bin/sh\n" + body) < 0) return false;
            file.close();
            return file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        };
        // Test supervision and exact argument boundaries without Linux tools,
        // real mounts, a filesystem scan, root privileges or remote credentials.
        QVERIFY(script(QStringLiteral("timeout"), "shift 3\nexec \"$@\"\n"));
        QVERIFY(script(QStringLiteral("nice"), "shift 2\nexec \"$@\"\n"));
        QVERIFY(script(QStringLiteral("ionice"), "shift 2\nexec \"$@\"\n"));
        QVERIFY(script(QStringLiteral("stat"), "echo ext4\n"));
        QVERIFY(script(QStringLiteral("du"),
            "printf '%s' \"$3\" > \"$SCAN_CAPTURE\"\n"
            "if [ \"$SCAN_MODE\" = wait ]; then\n"
            " trap 'echo stopped > \"$SCAN_STOP\"; exit 130' TERM\n"
            " echo started > \"$SCAN_START\"\n"
            " while :; do sleep 0.05; done\n"
            "fi\nprintf '42\\t%s\\n' \"$3\"\n"));
        const auto target = directory.filePath(QStringLiteral("a'b;$(false)\n目录"));
        QVERIFY(QDir().mkpath(target));
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("PATH"), bin + QStringLiteral(":/usr/bin:/bin"));
        environment.insert(QStringLiteral("SCAN_CAPTURE"), directory.filePath(QStringLiteral("capture")));
        environment.insert(QStringLiteral("SCAN_START"), directory.filePath(QStringLiteral("started")));
        environment.insert(QStringLiteral("SCAN_STOP"), directory.filePath(QStringLiteral("stopped")));
        QProcess process;
        const auto cleanup = qScopeGuard([&] {
            process.closeWriteChannel();
            if (!process.waitForFinished(2000)) { process.kill(); process.waitForFinished(2000); }
        });
        process.setProcessEnvironment(environment);
        const QStringList arguments{QStringLiteral("-c"), QString::fromUtf8(noxshell::detail::directorySizeCommand(target))};
        process.start(QStringLiteral("/bin/sh"), arguments);
        QVERIFY(process.waitForFinished(3000));
        QCOMPARE(process.exitCode(), 0);
        QVERIFY(process.readAllStandardError().isEmpty());
        quint64 bytes = 0;
        QVERIFY(noxshell::detail::parseDirectorySize(process.readAllStandardOutput(), bytes));
        QCOMPARE(bytes, quint64(42 * 1024));
        QFile capture(directory.filePath(QStringLiteral("capture")));
        QVERIFY(capture.open(QIODevice::ReadOnly));
        QCOMPARE(capture.readAll(), target.toUtf8());
        capture.close();

        environment.insert(QStringLiteral("SCAN_MODE"), QStringLiteral("wait"));
        process.setProcessEnvironment(environment);
        process.start(QStringLiteral("/bin/sh"), arguments);
        QVERIFY(process.waitForStarted(1000));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(directory.filePath(QStringLiteral("started"))), 2000);
        process.closeWriteChannel();
        QVERIFY(process.waitForFinished(3000));
        QVERIFY(QFileInfo::exists(directory.filePath(QStringLiteral("stopped"))));
        QVERIFY(process.exitCode() != 0);
#endif
    }

    void fileDirectorySizesQueueCancelAndSort_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }

    void fileDirectorySizesQueueCancelAndSort()
    {
        QFETCH(bool, dark);
        noxshell::ui::applyApplicationTheme(dark ? noxshell::ui::ThemeMode::Dark : noxshell::ui::ThemeMode::Light);
        const auto restoreTheme = qScopeGuard([] { noxshell::ui::applyApplicationTheme(noxshell::ui::ThemeMode::Light); });
        MemoryCredentialStore credentials;
        noxshell::SshSession session(nullptr, &credentials);
        QObject::disconnect(&session, &noxshell::SshSession::connectRequested, nullptr, nullptr);
        QObject::disconnect(&session, &noxshell::SshSession::homeDirectoryRequested, nullptr, nullptr);
        QObject::disconnect(&session, &noxshell::SshSession::listDirectoryRequested, nullptr, nullptr);
        QObject::disconnect(&session, &noxshell::SshSession::directorySizeRequested, nullptr, nullptr);
        QSignalSpy connectRequests(&session, &noxshell::SshSession::connectRequested);
        QSignalSpy homeRequests(&session, &noxshell::SshSession::homeDirectoryRequested);
        QSignalSpy sizeRequests(&session, &noxshell::SshSession::directorySizeRequested);
        QSignalSpy cancels(&session, &noxshell::SshSession::directorySizeCanceled);
        noxshell::ui::FilePanel panel(&session);
        panel.resize(1100, 440);
        panel.show();
        noxshell::ServerProfile profile;
        profile.id = profile.name = QStringLiteral("directory-size-test");
        profile.host = QStringLiteral("192.0.2.20");
        profile.user = QStringLiteral("fixture");
        profile.password = QStringLiteral("synthetic-only");
        profile.connectionMode = noxshell::ConnectionMode::Ssh;
        panel.setServer(profile);
        session.connectTo(profile);
        QCOMPARE(connectRequests.size(), 1);
        QVERIFY(QMetaObject::invokeMethod(&session, "handleConnectionChanged", Qt::DirectConnection,
            Q_ARG(bool, true), Q_ARG(QString, QStringLiteral("fixture connected")),
            Q_ARG(quint64, connectRequests.first().at(1).toULongLong())));
        QTRY_COMPARE(homeRequests.size(), 1);
        session.homeDirectoryResolved(QStringLiteral("/fixture"));
        noxshell::RemoteFileEntries entries;
        const auto entry = [](const QString &name, quint64 size, bool directory) {
            noxshell::RemoteFileEntry value;
            value.name = name;
            value.path = QStringLiteral("/fixture/") + name;
            value.directory = directory;
            value.size = size;
            return value;
        };
        entries << entry(QStringLiteral("b-dir"), 0, true) << entry(QStringLiteral("a-dir"), 0, true)
                << entry(QStringLiteral("small"), 900, false) << entry(QStringLiteral("large"), 2048, false);
        session.directoryListed(QStringLiteral("/fixture"), entries);
        auto *tree = panel.findChild<QTreeWidget *>(QStringLiteral("remoteFileTree"));
        auto *automatic = panel.findChild<QCheckBox *>(QStringLiteral("fileAutoDirectorySize"));
        auto *pathEdit = panel.findChild<QLineEdit *>(QStringLiteral("remotePathEdit"));
        QVERIFY(tree && automatic && pathEdit);
        QVERIFY(!panel.findChild<QComboBox *>(QStringLiteral("fileSortMode")));
        QCOMPARE(tree->header()->objectName(), QStringLiteral("remoteFileHeader"));
        const auto clickHeader = [tree](int column) {
            QTest::mouseClick(tree->header()->viewport(), Qt::LeftButton, Qt::NoModifier,
                QPoint(tree->header()->sectionViewportPosition(column) + tree->columnWidth(column) / 2,
                    tree->header()->height() / 2));
        };
        const auto sizePoint = [tree](QTreeWidgetItem *item, bool action) {
            return QPoint(tree->header()->sectionViewportPosition(1) + (action ? 16 : tree->columnWidth(1) - 16),
                tree->visualItemRect(item).center().y());
        };
        const auto clickSizeAction = [tree, &sizePoint](QTreeWidgetItem *item) {
            QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, sizePoint(item, true));
        };
        QVERIFY(!automatic->isChecked());
        QCOMPARE(sizeRequests.size(), 0);
        QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("a-dir"));
        auto *a = tree->topLevelItem(0);
        auto *b = tree->topLevelItem(1);
        QCOMPARE(a->text(1), QStringLiteral("—"));
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, sizePoint(a, false));
        QCoreApplication::processEvents();
        QCOMPARE(sizeRequests.size(), 0); // Selecting the value does not start a scan.
        QTest::mousePress(tree->viewport(), Qt::LeftButton, Qt::NoModifier, sizePoint(a, false));
        QTest::mouseRelease(tree->viewport(), Qt::LeftButton, Qt::NoModifier, sizePoint(a, true));
        QCoreApplication::processEvents();
        QCOMPARE(sizeRequests.size(), 0); // Dragging into the icon is not a click.
        clickSizeAction(a);
        QTRY_COMPARE(sizeRequests.size(), 1);
        QCOMPARE(sizeRequests.last().at(1).toString(), QStringLiteral("/fixture/a-dir"));
        QCOMPARE(a->text(1), QStringLiteral("计算中…"));
        const auto first = sizeRequests.last().at(0).toULongLong();
        session.directorySizeCalculated(first, QStringLiteral("/fixture/a-dir"), 1024, {});
        QCOMPARE(a->text(1), QStringLiteral("1.0 KB"));
        clickHeader(1);
        QCOMPARE(tree->header()->sortIndicatorSection(), 1);
        QCOMPARE(tree->header()->sortIndicatorOrder(), Qt::AscendingOrder);
        QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("small"));
        QCOMPARE(tree->topLevelItem(1), a);
        QCOMPARE(tree->topLevelItem(3), b);
        clickHeader(1);
        QCOMPARE(tree->header()->sortIndicatorOrder(), Qt::DescendingOrder);
        QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("large"));
        QCOMPARE(tree->topLevelItem(3), b);
        clickHeader(2); // Other headers do not change the selected sort.
        QCOMPARE(tree->sortColumn(), 1);
        QCOMPARE(tree->header()->sortIndicatorOrder(), Qt::DescendingOrder);
        clickHeader(0);
        QCOMPARE(tree->sortColumn(), 0);
        QCOMPARE(tree->header()->sortIndicatorOrder(), Qt::AscendingOrder);
        clickHeader(0);
        QCOMPARE(tree->header()->sortIndicatorOrder(), Qt::DescendingOrder);
        QCOMPARE(tree->topLevelItem(0), b);
        clickHeader(0);
        automatic->setChecked(true);
        QTRY_COMPARE(sizeRequests.size(), 2);
        QCOMPARE(sizeRequests.last().at(1).toString(), QStringLiteral("/fixture/b-dir"));
        const auto obsolete = sizeRequests.last().at(0).toULongLong();
        const int cancelCount = cancels.size();
        pathEdit->setText(QStringLiteral("/new"));
        QTest::keyClick(pathEdit, Qt::Key_Return);
        QVERIFY(cancels.size() > cancelCount);
        noxshell::RemoteFileEntries next{entry(QStringLiteral("d-dir"), 0, true), entry(QStringLiteral("c-dir"), 0, true)};
        for (auto &value : next) value.path = QStringLiteral("/new/") + value.name;
        session.directoryListed(QStringLiteral("/new"), next);
        QTRY_COMPARE(sizeRequests.size(), 3);
        QCOMPARE(sizeRequests.last().at(1).toString(), QStringLiteral("/new/c-dir"));
        QCOMPARE(tree->topLevelItem(1)->text(1), QStringLiteral("等待计算…"));
        session.directorySizeCalculated(obsolete, QStringLiteral("/fixture/b-dir"), 99999999, {});
        QCOMPARE(tree->topLevelItem(0)->text(1), QStringLiteral("计算中…"));
        QCOMPARE(sizeRequests.size(), 3);
        const auto current = sizeRequests.last().at(0).toULongLong();
        session.directorySizeCalculated(current, QStringLiteral("/new/c-dir"), 0, QStringLiteral("Permission denied"));
        QCOMPARE(tree->topLevelItem(0)->text(1), QStringLiteral("未完成"));
        QTRY_COMPARE(sizeRequests.size(), 4);
        QCOMPARE(sizeRequests.last().at(1).toString(), QStringLiteral("/new/d-dir"));
        automatic->setChecked(false);
        QCOMPARE(tree->topLevelItem(1)->text(1), QStringLiteral("—"));
        session.directorySizeCalculated(sizeRequests.last().at(0).toULongLong(), QStringLiteral("/new/d-dir"), 4096, {});
        QCOMPARE(tree->topLevelItem(1)->text(1), QStringLiteral("—"));
        QTest::qWait(230);
        QCOMPARE(sizeRequests.size(), 4);
        auto *recalculated = tree->topLevelItem(0);
        clickSizeAction(recalculated);
        QTRY_COMPARE(sizeRequests.size(), 5);
        session.directorySizeCalculated(sizeRequests.last().at(0).toULongLong(), QStringLiteral("/new/c-dir"), 2048, {});
        QCOMPARE(recalculated->text(1), QStringLiteral("2.0 KB"));
        clickSizeAction(recalculated);
        clickSizeAction(recalculated); // Rapid repeated clicks do not create duplicate scans.
        QTRY_COMPARE(sizeRequests.size(), 6);
        tree->itemDoubleClicked(recalculated, 1);
        QCOMPARE(panel.currentPath(), QStringLiteral("/new"));
        session.directorySizeCalculated(sizeRequests.last().at(0).toULongLong(), QStringLiteral("/new/c-dir"), 8192, {});
        QCOMPARE(recalculated->text(1), QStringLiteral("8.0 KB"));
        QCOMPARE(recalculated->textAlignment(1), Qt::AlignRight | Qt::AlignVCenter);
        QVERIFY(recalculated->toolTip(1).contains(QStringLiteral("左侧图标")));
        tree->setColumnWidth(1, 96);
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, sizePoint(recalculated, false));
        QCoreApplication::processEvents();
        QCOMPARE(sizeRequests.size(), 6);
        tree->setColumnWidth(1, 132);
        noxshell::ui::applyApplicationTheme(dark ? noxshell::ui::ThemeMode::Light : noxshell::ui::ThemeMode::Dark);
        QCoreApplication::processEvents();
        QCOMPARE(recalculated->text(1), QStringLiteral("8.0 KB"));
        noxshell::ui::applyApplicationTheme(dark ? noxshell::ui::ThemeMode::Dark : noxshell::ui::ThemeMode::Light);
        QCoreApplication::processEvents();
        tree->clearSelection();
        const auto captureDir = qEnvironmentVariable("NOXSHELL_SIZE_CAPTURE_DIR");
        if (!captureDir.isEmpty()) {
            QVERIFY(QDir().mkpath(captureDir));
            QVERIFY(panel.grab().save(captureDir + (dark ? QStringLiteral("/dark.png") : QStringLiteral("/light.png"))));
            recalculated->setSelected(true);
            QVERIFY(panel.grab().save(captureDir + (dark ? QStringLiteral("/dark-selected.png") : QStringLiteral("/light-selected.png"))));
        }
    }

    void remotePathBreadcrumbs_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }

    void remotePathBreadcrumbs()
    {
        QFETCH(bool, dark);
        noxshell::ui::applyApplicationTheme(dark ? noxshell::ui::ThemeMode::Dark : noxshell::ui::ThemeMode::Light);
        const auto restoreTheme = qScopeGuard([] { noxshell::ui::applyApplicationTheme(noxshell::ui::ThemeMode::Light); });
        QWidget window;
        auto *layout = new QVBoxLayout(&window);
        auto *outside = new QLineEdit;
        auto *path = new noxshell::ui::RemotePathEdit;
        layout->addWidget(outside);
        layout->addWidget(path);
        window.resize(760, 140);
        window.show();
        window.activateWindow();
        outside->setFocus();
        QTRY_VERIFY(outside->hasFocus());
        const QString initial = QStringLiteral("/var/lib/docker/volumes/zyb_logs/_data");
        path->setPath(initial);
        QSignalSpy activated(path, &noxshell::ui::RemotePathEdit::pathActivated);
        QSignalSpy submitted(path, &QLineEdit::returnPressed);
        const auto segment = [path](const QString &destination) -> QToolButton * {
            for (auto *button : path->findChildren<QToolButton *>(QStringLiteral("remotePathSegment"))) {
                if (button->isVisible() && button->property("remotePath").toString() == destination) return button;
            }
            return nullptr;
        };
        QTRY_VERIFY(!path->isEditing());
        auto *lib = segment(QStringLiteral("/var/lib"));
        QVERIFY(lib);
        QTest::mouseClick(lib, Qt::LeftButton);
        QCOMPARE(activated.size(), 1);
        QCOMPARE(activated.last().first().toString(), QStringLiteral("/var/lib"));
        QVERIFY(!path->isEditing());
        // Selecting a breadcrumb submits exactly once, not a text edit/Enter.
        QCOMPARE(submitted.size(), 0);
        path->setPath(QStringLiteral("/var/lib"));
        QTest::mouseClick(segment(QStringLiteral("/var/lib")), Qt::LeftButton);
        QCOMPARE(activated.size(), 1); // Clicking the current directory is a no-op.
        QTest::mouseClick(segment(QStringLiteral("/")), Qt::LeftButton);
        QCOMPARE(activated.last().first().toString(), QStringLiteral("/"));

        path->setPath(initial);
        QTest::mouseClick(path, Qt::LeftButton, Qt::NoModifier, QPoint(path->width() - 45, 13));
        QVERIFY(path->isEditing());
        QTRY_VERIFY(path->hasFocus());
        QCOMPARE(path->text(), initial);
        QVERIFY(!segment(QStringLiteral("/var/lib")));
        path->setText(QStringLiteral("/not-submitted"));
        QTest::mouseClick(outside, Qt::LeftButton);
        QTRY_VERIFY(outside->hasFocus());
        QTRY_VERIFY(!path->isEditing());
        QCOMPARE(path->text(), initial);
        QCOMPARE(submitted.size(), 0);

        auto *editButton = path->findChild<QToolButton *>(QStringLiteral("remotePathEditButton"));
        QVERIFY(editButton && !editButton->icon().isNull());
        QTest::mouseClick(editButton, Qt::LeftButton);
        QVERIFY(path->isEditing());
        path->setText(QStringLiteral("/also-not-submitted"));
        QFocusEvent popupFocusOut(QEvent::FocusOut, Qt::PopupFocusReason);
        QApplication::sendEvent(path, &popupFocusOut);
        QVERIFY(path->isEditing());
        QCOMPARE(path->text(), QStringLiteral("/also-not-submitted"));
        QTest::keyClick(path, Qt::Key_Escape);
        QVERIFY(!path->isEditing());
        QCOMPARE(path->text(), initial);
        QCOMPARE(submitted.size(), 0);

        path->beginEditing();
        const QString unusual = QStringLiteral("/目录/my files/a&b/100%/#data;$(literal)");
        path->setText(unusual);
        connect(path, &QLineEdit::returnPressed, path, [path] { path->setPath(path->text()); });
        QTest::keyClick(path, Qt::Key_Return);
        QCOMPARE(submitted.size(), 1);
        QCOMPARE(path->text(), unusual);
        QVERIFY(!path->isEditing());
        auto *ampersand = segment(QStringLiteral("/目录/my files/a&b"));
        QVERIFY(ampersand);
        QCOMPARE(ampersand->text(), QStringLiteral("a&&b"));
        QTest::mouseClick(ampersand, Qt::LeftButton);
        QCOMPARE(activated.last().first().toString(), QStringLiteral("/目录/my files/a&b"));

        const QString longPath = QStringLiteral("/var/lib/docker/volumes/") + QString(180, QLatin1Char('x')) + QStringLiteral("/_data");
        path->setPath(longPath);
        window.resize(260, 140);
        QCoreApplication::processEvents();
        auto *overflow = path->findChild<QToolButton *>(QStringLiteral("remotePathOverflow"));
        QVERIFY(overflow && overflow->isVisible());
        QVERIFY(overflow->menu() && !overflow->menu()->actions().isEmpty());
        QVERIFY(segment(QStringLiteral("/")));
        QVERIFY(segment(longPath));
        for (auto *button : path->findChildren<QToolButton *>()) {
            if (button->isVisible()) QVERIFY(path->rect().contains(button->geometry()));
        }
        const auto action = overflow->menu()->actions().first();
        QCOMPARE(action->data().toString(), QStringLiteral("/var"));
        bool menuOpened = false;
        QTimer::singleShot(0, overflow->menu(), [menu = overflow->menu(), &menuOpened] {
            menuOpened = menu->isVisible();
            menu->setActiveAction(menu->actions().first());
            QTest::keyClick(menu, Qt::Key_Return);
            menu->close();
        });
        QTest::mouseClick(overflow, Qt::LeftButton);
        QTRY_VERIFY(menuOpened);
        QCOMPARE(activated.last().first().toString(), QStringLiteral("/var"));
        path->beginEditing();
        QCOMPARE(path->text(), longPath); // Never copy an ellipsis into the path.
        QTest::keyClick(path, Qt::Key_Escape);
        const auto longLeaf = QStringLiteral("/") + QString(240, QLatin1Char('z'));
        path->setPath(longLeaf);
        QVERIFY(segment(longLeaf));
        QVERIFY(segment(longLeaf)->text().contains(QChar(0x2026)));
        QCOMPARE(segment(longLeaf)->toolTip(), longLeaf);
        QVERIFY(path->rect().contains(segment(longLeaf)->geometry()));
        path->setPath(QStringLiteral("/"));
        QVERIFY(segment(QStringLiteral("/")));
        QVERIFY(!overflow->isVisible());

        path->setPath(initial);
        window.resize(760, 140);
        QCoreApplication::processEvents();
        const auto capture = qEnvironmentVariable("NOXSHELL_BREADCRUMB_CAPTURE_DIR");
        if (!capture.isEmpty()) {
            QDir().mkpath(capture);
            QVERIFY(window.grab().save(capture + (dark ? QStringLiteral("/dark.png") : QStringLiteral("/light.png"))));
        }
        noxshell::ui::applyApplicationTheme(dark ? noxshell::ui::ThemeMode::Light : noxshell::ui::ThemeMode::Dark);
        QCoreApplication::processEvents();
        QVERIFY(!path->isEditing());
        QVERIFY(segment(QStringLiteral("/var/lib")));
        QCOMPARE(path->text(), initial);
    }

    void remotePathBreadcrumbNavigation_data() { remotePathBreadcrumbs_data(); }

    void remotePathBreadcrumbNavigation()
    {
        QFETCH(bool, dark);
        noxshell::ui::applyApplicationTheme(dark ? noxshell::ui::ThemeMode::Dark : noxshell::ui::ThemeMode::Light);
        const auto restoreTheme = qScopeGuard([] { noxshell::ui::applyApplicationTheme(noxshell::ui::ThemeMode::Light); });
        MemoryCredentialStore credentials;
        noxshell::SshSession session(nullptr, &credentials);
        QObject::disconnect(&session, &noxshell::SshSession::connectRequested, nullptr, nullptr);
        QObject::disconnect(&session, &noxshell::SshSession::homeDirectoryRequested, nullptr, nullptr);
        QObject::disconnect(&session, &noxshell::SshSession::listDirectoryRequested, nullptr, nullptr);
        QSignalSpy connections(&session, &noxshell::SshSession::connectRequested);
        QSignalSpy homes(&session, &noxshell::SshSession::homeDirectoryRequested);
        QSignalSpy listings(&session, &noxshell::SshSession::listDirectoryRequested);
        noxshell::ui::FilePanel panel(&session);
        panel.resize(1200, 430);
        panel.show();
        panel.activateWindow();
        noxshell::ServerProfile profile;
        profile.id = profile.name = QStringLiteral("breadcrumb-fixture");
        profile.host = QStringLiteral("192.0.2.20");
        profile.user = QStringLiteral("fixture");
        profile.password = QStringLiteral("synthetic-only");
        profile.connectionMode = noxshell::ConnectionMode::Ssh;
        panel.setServer(profile);
        session.connectTo(profile);
        QCOMPARE(connections.size(), 1);
        QVERIFY(QMetaObject::invokeMethod(&session, "handleConnectionChanged", Qt::DirectConnection,
            Q_ARG(bool, true), Q_ARG(QString, QStringLiteral("fixture connected")),
            Q_ARG(quint64, connections.first().at(1).toULongLong())));
        QTRY_COMPARE(homes.size(), 1);
        const QString initial = QStringLiteral("/var/lib/docker/volumes/zyb_logs/_data");
        session.homeDirectoryResolved(initial);
        session.directoryListed(initial, {});
        auto *path = panel.findChild<noxshell::ui::RemotePathEdit *>();
        QVERIFY(path);
        QVERIFY(!path->isEditing());
        const auto clickSegment = [path](const QString &destination) {
            for (auto *button : path->findChildren<QToolButton *>(QStringLiteral("remotePathSegment"))) {
                if (button->isVisible() && button->property("remotePath").toString() == destination) {
                    QTest::mouseClick(button, Qt::LeftButton);
                    return true;
                }
            }
            return false;
        };
        QVERIFY(clickSegment(QStringLiteral("/var/lib")));
        QCOMPARE(panel.currentPath(), QStringLiteral("/var/lib"));
        QCOMPARE(path->text(), panel.currentPath());
        QVERIFY(!path->isEditing());
        session.directoryListed(panel.currentPath(), {});
        auto *back = panel.findChild<QToolButton *>(QStringLiteral("fileBackButton"));
        QVERIFY(back && back->isEnabled());
        QTest::mouseClick(back, Qt::LeftButton);
        QCOMPARE(panel.currentPath(), initial);
        session.directoryListed(initial, {});
        auto *tree = panel.findChild<QTreeWidget *>(QStringLiteral("remoteFileTree"));
        QVERIFY(tree);
        tree->setFocus();
        QTest::keyClick(tree, Qt::Key_L, Qt::ControlModifier);
        QTRY_VERIFY(path->isEditing());
        QCOMPARE(path->selectedText(), initial);
        path->setText(QStringLiteral("/var"));
        QTest::keyClick(path, Qt::Key_Return);
        QCOMPARE(panel.currentPath(), QStringLiteral("/var"));
        QVERIFY(!path->isEditing());
        session.directoryListed(panel.currentPath(), {});
        const int count = listings.size();
        path->beginEditing();
        path->setText(QStringLiteral("/unsubmitted"));
        QTest::mouseClick(tree->viewport(), Qt::LeftButton);
        QTRY_VERIFY(!path->isEditing());
        QCOMPARE(panel.currentPath(), QStringLiteral("/var"));
        QCOMPARE(path->text(), panel.currentPath());
        QCOMPARE(listings.size(), count);
        QVERIFY(clickSegment(QStringLiteral("/")));
        QCOMPARE(panel.currentPath(), QStringLiteral("/"));
        session.directoryListed(panel.currentPath(), {});
        QVERIFY(!panel.findChild<QToolButton *>(QStringLiteral("fileUpButton"))->isEnabled());

        session.homeDirectoryResolved(initial);
        noxshell::RemoteFileEntries entries;
        for (const auto &name : {QStringLiteral("archive"), QStringLiteral("config.yml"), QStringLiteral("server.log")}) {
            noxshell::RemoteFileEntry entry;
            entry.name = name;
            entry.path = initial + QLatin1Char('/') + name;
            entry.directory = name == QStringLiteral("archive");
            entry.size = 4096;
            entries.append(entry);
        }
        session.directoryListed(initial, entries);
        QCoreApplication::processEvents();
        QVERIFY(!path->isEditing()); // Deferred deletion/focus changes must not reopen editing.
        const auto capture = qEnvironmentVariable("NOXSHELL_BREADCRUMB_CAPTURE_DIR");
        if (!capture.isEmpty()) {
            QDir().mkpath(capture);
            QVERIFY(panel.grab().save(capture + (dark ? QStringLiteral("/panel-dark.png") : QStringLiteral("/panel-light.png"))));
        }
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
        QVERIFY(fileEditor->findChild<QPushButton *>(QStringLiteral("remoteFileEditorSave")));
        QVERIFY(!fileEditor->findChild<QPushButton *>(QStringLiteral("remoteFileEditorClose")));
        QCOMPARE(editorTabs->count(), 1);
        QVERIFY(editorTabs->tabText(0).contains(QStringLiteral("demo-sftp")));
        QVERIFY(editorTabs->tabText(0).contains(firstFileName));
        QCOMPARE(editorTabs->height(), 38);
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

    void completedDownloadOpensOnlyItsLocalDirectory_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }

    void completedDownloadOpensOnlyItsLocalDirectory()
    {
        QFETCH(bool, dark);
        noxshell::ui::applyApplicationTheme(dark ? noxshell::ui::ThemeMode::Dark : noxshell::ui::ThemeMode::Light);
        const auto restoreTheme = qScopeGuard([] { noxshell::ui::applyApplicationTheme(noxshell::ui::ThemeMode::Light); });
        // Intercept desktop requests: this test must not launch Finder or open a
        // real download. Include characters that must remain part of a file URL.
        LocalDirectoryUrlCapture capture;
        QDesktopServices::setUrlHandler(QStringLiteral("file"), &capture, "capture");
        const auto resetHandler = qScopeGuard([] { QDesktopServices::unsetUrlHandler(QStringLiteral("file")); });
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto destination = directory.filePath(QStringLiteral("下载 ' # & 空格"));
        QVERIFY(QDir().mkpath(destination));
        MemoryCredentialStore credentials;
        noxshell::SshSession session(nullptr, &credentials);
        noxshell::ui::TransferQueuePanel panel(&session);
        panel.resize(520, 250);
        panel.show();
        noxshell::FileTransferTask task;
        task.id = 81;
        task.operation = noxshell::RemoteFileOperation::Download;
        task.localPath = destination + QStringLiteral("/test.command");
        task.remotePath = QStringLiteral("/remote/test.command");
        task.total = 4096;
        session.transferTaskChanged(task);
        auto *list = panel.findChild<QListWidget *>(QStringLiteral("transferQueueList"));
        QVERIFY(list);
        QCOMPARE(list->count(), 1);
        auto *row = list->itemWidget(list->item(0));
        auto *open = row->findChild<QToolButton *>(QStringLiteral("transferOpenDirectory"));
        auto *cancel = row->findChild<QPushButton *>(QStringLiteral("transferCancel"));
        QVERIFY(open && cancel);
        for (const auto state : {noxshell::TransferState::Queued, noxshell::TransferState::Running,
                 noxshell::TransferState::Failed, noxshell::TransferState::Canceled}) {
            task.state = state;
            session.transferTaskChanged(task);
            QVERIFY(open->isHidden());
            QVERIFY(!open->isEnabled());
        }
        task.state = noxshell::TransferState::Completed;
        task.completed = task.total;
        session.transferTaskChanged(task);
        QTRY_VERIFY(open->isVisible());
        QVERIFY(open->isEnabled());
        QVERIFY(cancel->isHidden());
        QVERIFY(!open->icon().isNull());
        QCOMPARE(open->accessibleName(), QStringLiteral("打开所在目录"));
        QVERIFY(open->toolTip().contains(destination));
        QVERIFY(capture.urls.isEmpty()); // Completion alone never opens a folder.
        QTest::mouseClick(open, Qt::LeftButton);
        QCOMPARE(capture.urls.size(), 1);
        QVERIFY(capture.urls.last().isLocalFile());
        QCOMPARE(capture.urls.last().toLocalFile(), destination); // Never the .command file.
        QCoreApplication::processEvents();
        QVERIFY(row->findChild<QLabel *>(QStringLiteral("transferState"))->geometry().right() < open->geometry().left());
        const auto captureDir = qEnvironmentVariable("NOXSHELL_TRANSFER_CAPTURE_DIR");
        if (!captureDir.isEmpty()) {
            QVERIFY(QDir().mkpath(captureDir));
            QVERIFY(panel.grab().save(captureDir + (dark ? QStringLiteral("/dark.png") : QStringLiteral("/light.png"))));
        }
        auto upload = task;
        upload.id = 82;
        upload.operation = noxshell::RemoteFileOperation::Upload;
        session.transferTaskChanged(upload);
        auto *uploadButton = list->itemWidget(list->item(1))->findChild<QToolButton *>(QStringLiteral("transferOpenDirectory"));
        QVERIFY(uploadButton && uploadButton->isHidden() && !uploadButton->isEnabled());
        const auto updatedDestination = directory.filePath(QStringLiteral("new destination"));
        QVERIFY(QDir().mkpath(updatedDestination));
        task.localPath = updatedDestination + QStringLiteral("/new.txt");
        session.transferTaskChanged(task);
        QTest::mouseClick(open, Qt::LeftButton);
        QCOMPARE(capture.urls.size(), 2);
        QCOMPARE(capture.urls.last().toLocalFile(), updatedDestination); // Resolve current task, not a stale path.
        task.localPath = directory.filePath(QStringLiteral("missing-directory/gone.txt"));
        session.transferTaskChanged(task);
        QTest::mouseClick(open, Qt::LeftButton);
        QCOMPARE(capture.urls.size(), 2);
        QVERIFY(row->findChild<QLabel *>(QStringLiteral("transferPath"))->text().contains(QStringLiteral("不存在")));
        for (const auto &invalidPath : {QString{}, QStringLiteral("relative.txt"), QStringLiteral("https://example.invalid/file")}) {
            task.localPath = invalidPath;
            session.transferTaskChanged(task);
            QVERIFY(open->isHidden());
            QVERIFY(!open->isEnabled());
        }
        session.transferQueueReset();
        QCOMPARE(list->count(), 0);
        QCOMPARE(credentials.loadCalls, 0);
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
        MemoryCredentialStore credentials;
        noxshell::ui::MainWindow window(databasePath, nullptr, &credentials);
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
        QVERIFY(!sidebarToggle);
        QVERIFY(monitorToggle);
        QVERIFY(settingsButton);
        QVERIFY(themeButton);
        QVERIFY(windowToolbar);
        QCOMPARE(monitorToggle->parentWidget(), settingsButton->parentWidget());
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
                if (item->text(0) == name) return item;
            }
            return nullptr;
        };
        const auto currentHostName = [hosts] {
            return hosts->currentItem() ? hosts->currentItem()->text(0) : QString{};
        };
        QCOMPARE(window.toolBarArea(windowToolbar), Qt::TopToolBarArea);
        QVERIFY(!window.findChild<QWidget *>(QStringLiteral("topBar")));
        QVERIFY(!window.findChild<QLineEdit *>(QStringLiteral("globalSearch")));
        QVERIFY(!sidebar->isVisible());
        QVERIFY(!monitorToggle->icon().isNull());
        auto *homeTabs = window.findChild<QTabWidget *>(QStringLiteral("connectionHomeTabs"));
        QVERIFY(homeTabs);
        QCOMPARE(homeTabs->count(), 2);
        QCOMPARE(homeTabs->tabText(0), QStringLiteral("访问历史"));
        QCOMPARE(homeTabs->tabText(1), QStringLiteral("服务器管理"));
        homeTabs->setCurrentIndex(1);
        QVERIFY(sidebar->isVisible());

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
        QCOMPARE(monitorPosition.x(), 0);
        QVERIFY(homeTabs->isAncestorOf(sidebar));
        QVERIFY(sidebar->width() > 700);
        QVERIFY(monitorPosition.x() < terminalPosition.x());
        QVERIFY(filePane->isHidden());
        QCOMPARE(terminalPane->height(), terminalFileSplitter->height());

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
            QCOMPARE(metricRow->height(), 44);
            auto *progress = metricRow->findChild<QProgressBar *>();
            QVERIFY(progress);
            QCOMPARE(progress->orientation(), Qt::Horizontal);
            QCOMPARE(progress->height(), 4);
            QVERIFY(!progress->isTextVisible());
        }
        metricRows.at(0)->setCoreValues({12.0, 34.0, 56.0, 78.0});
        QEnterEvent metricEnterEvent(QPointF(10, 10), QPointF(10, 10), QPointF(10, 10));
        QApplication::sendEvent(metricRows.at(0), &metricEnterEvent);
        QCoreApplication::processEvents();
        QVERIFY(metricRows.at(0)->height() > 44);
        QVERIFY(metricRows.at(0)->geometry().bottom() < metricRows.at(1)->geometry().top());
        QVERIFY(metricRows.at(1)->geometry().bottom() < metricRows.at(2)->geometry().top());
        QEvent metricLeaveEvent(QEvent::Leave);
        QApplication::sendEvent(metricRows.at(0), &metricLeaveEvent);
        QCoreApplication::processEvents();
        QCOMPARE(metricRows.at(0)->height(), 44);
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
        homeTabs->setCurrentIndex(0);
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
        QTest::mouseClick(newTabButton, Qt::LeftButton);
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
        QCOMPARE(answerClose([&] { tabs->tabCloseRequested(1); }), 1);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 1000);
        auto *closedHostRow = hostItemForName(QStringLiteral("db-master-01"));
        QVERIFY(closedHostRow);
        QVERIFY(!closedHostRow->toolTip(0).contains(QStringLiteral("离线")));

        auto *duplicate = window.findChild<QAction *>(QStringLiteral("terminalDuplicateAction"));
        QVERIFY(duplicate);
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("duplicateTerminalButton")));
        tabs->customContextMenuRequested(tabs->tabRect(0).center());
        duplicate->trigger();
        QCOMPARE(tabs->count(), 2);
        QCOMPARE(answerClose([&] { tabs->tabCloseRequested(tabs->currentIndex()); }), 1);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(window.findChildren<noxshell::ui::TransferQueuePanel *>().size(), 1, 1000);
#ifndef Q_OS_MACOS
        window.setQuitInProgress(true); // macOS alone uses close-to-hide.
#endif
        window.close();
        QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 1000);
    }

    void connectionHomeCombinesHistoryAndServerManagement_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }

    void connectionHomeCombinesHistoryAndServerManagement()
    {
        QFETCH(bool, dark);
        const auto previousTheme = noxshell::ui::storedThemeMode();
        const auto restoreTheme = qScopeGuard([previousTheme] {
            noxshell::ui::applyApplicationTheme(previousTheme);
        });
        QTemporaryDir directory;
        const auto database = directory.filePath(QStringLiteral("connection-home.sqlite3"));
        noxshell::ServerRepository repository(database, true);
        QVERIFY(repository.initialize());
        auto servers = repository.loadServers();
        QVERIFY(!servers.isEmpty());
        QVERIFY(repository.recordSuccessfulLogin(servers.first().id));
        MemoryCredentialStore credentials;
        noxshell::ui::MainWindow window(database, nullptr, &credentials);
        noxshell::ui::applyApplicationTheme(dark ? noxshell::ui::ThemeMode::Dark : noxshell::ui::ThemeMode::Light);
        window.resize(1440, 900);
        window.show();
        QTest::qWait(80);

        auto *workspace = window.findChild<noxshell::ui::TerminalWorkspace *>();
        auto *home = window.findChild<QTabWidget *>(QStringLiteral("connectionHomeTabs"));
        auto *manager = window.findChild<noxshell::ui::HostSidebar *>();
        auto *hosts = window.findChild<QTreeWidget *>(QStringLiteral("hostList"));
        auto *history = window.findChild<QTreeWidget *>(QStringLiteral("recentLoginList"));
        auto *monitor = window.findChild<QWidget *>(QStringLiteral("monitorRail"));
        auto *search = window.findChild<QLineEdit *>(QStringLiteral("hostSearch"));
        auto *splitter = window.findChild<QSplitter *>(QStringLiteral("terminalFileSplitter"));
        auto *filePane = window.findChild<QWidget *>(QStringLiteral("fileWorkspacePane"));
        auto *terminalPane = window.findChild<QWidget *>(QStringLiteral("terminalWorkspacePane"));
        QVERIFY(workspace && home && manager && hosts && history && monitor && search);
        QVERIFY(splitter && filePane && terminalPane);
        QVERIFY(filePane->isHidden());
        QVERIFY(splitter->handle(1)->isHidden());
        QCOMPARE(terminalPane->height(), splitter->height());
        QVERIFY(!window.findChild<QToolButton *>(QStringLiteral("sidebarToggleButton")));
        QVERIFY(home->isAncestorOf(manager));
        QCOMPARE(monitor->mapTo(&window, QPoint{}).x(), 0);
        QCOMPARE(home->currentIndex(), 0);
        QVERIFY(history->isVisible());
        QVERIFY(!manager->isVisible());
        const auto terminalWidth = workspace->width();
        const auto screenshotDir = qEnvironmentVariable("NOXSHELL_HOME_SCREENSHOT_DIR");
        const auto capture = [&](const QString &page) {
            if (screenshotDir.isEmpty()) return true;
            return window.grab().save(QDir(screenshotDir).filePath(
                QStringLiteral("home-%1-%2.png").arg(dark ? QStringLiteral("dark") : QStringLiteral("light"), page)));
        };
        QVERIFY(capture(QStringLiteral("history")));
        QTest::mouseClick(home->tabBar(), Qt::LeftButton, Qt::NoModifier, home->tabBar()->tabRect(1).center());
        QVERIFY(manager->isVisible());
        QCOMPARE(workspace->width(), terminalWidth);
        QCOMPARE(terminalPane->height(), splitter->height());
        QVERIFY(manager->width() > 1000);
        QCOMPARE(hosts->columnCount(), 7);
        QVERIFY(!hosts->isHeaderHidden());
        QVERIFY(hosts->palette().color(QPalette::Base).lightness() < 128);
        QVERIFY(hosts->palette().color(QPalette::Text).lightness() > 128);
        QVERIFY(capture(QStringLiteral("servers")));

        // Filtering and selecting records only read local configuration.
        search->setText(QStringLiteral("does-not-match-any-host"));
        for (int index = 0; index < hosts->topLevelItemCount(); ++index)
            QVERIFY(hosts->topLevelItem(index)->isHidden());
        search->setText(servers.first().group);
        QVERIFY(manager->selectServerById(servers.first().id));
        auto *first = hosts->currentItem();
        QVERIFY(first && !first->isHidden());
        QCOMPARE(first->text(1), servers.first().host);
        QCOMPARE(first->text(2), QString::number(servers.first().port));
        QCOMPARE(first->text(3), servers.first().user);
        search->clear();
        QCOMPARE(credentials.loadCalls, 0);
        QCOMPARE(credentials.saveCalls, 0);
        QCOMPARE(workspace->sessionCount(), 0);
        QVERIFY(!window.findChild<noxshell::SshSession *>());

        window.resize(1180, 720);
        QTest::qWait(50);
        QVERIFY(manager->width() > 700);
        QVERIFY(hosts->header()->sectionViewportPosition(6) + hosts->columnWidth(6) <= hosts->viewport()->width());
        QVERIFY(capture(QStringLiteral("compact")));
        hosts->itemDoubleClicked(first, 1);
        QTRY_COMPARE_WITH_TIMEOUT(workspace->sessionCount(), 1, 1000);
        auto *session = workspace->findChild<noxshell::SshSession *>();
        QVERIFY(session);
        QTRY_VERIFY_WITH_TIMEOUT(session->isConnected(), 1000);
        QVERIFY(filePane->isVisible());
        QVERIFY(splitter->handle(1)->isVisible());
        window.resize(1440, 900);
        QTest::qWait(50);
        splitter->setSizes({450, 410});
        const auto previousSplit = splitter->sizes();
        const auto splitRestored = [&] {
            const auto sizes = splitter->sizes();
            return sizes.size() == 2 && qAbs(sizes.at(0) - previousSplit.at(0)) <= 2
                && qAbs(sizes.at(1) - previousSplit.at(1)) <= 2;
        };
        auto *fileStack = window.findChild<QStackedWidget *>(QStringLiteral("fileWorkspaceStack"));
        QVERIFY(fileStack);
        auto *originalFilePanel = fileStack->currentWidget();
        QSignalSpy connectionChanges(session, &noxshell::SshSession::connectionChanged);
        auto *terminal = workspace->findChild<noxshell::ui::TerminalView *>();
        QVERIFY(terminal);
        const auto buffer = terminal->plainText();
        auto *newTab = workspace->findChild<QToolButton *>(QStringLiteral("terminalNewTabButton"));
        auto *sessions = workspace->findChild<QTabBar *>(QStringLiteral("terminalSessionTabs"));
        QVERIFY(newTab && sessions);
        newTab->click();
        QVERIFY(filePane->isHidden());
        QTRY_COMPARE(terminalPane->height(), splitter->height());
        QCOMPARE(home->currentIndex(), 1);
        QVERIFY(manager->isVisible());
        home->setCurrentIndex(0);
        QVERIFY(history->isVisible());
        sessions->tabBarClicked(0);
        QVERIFY(terminal->isVisible());
        QVERIFY(filePane->isVisible());
        QTRY_VERIFY(splitRestored());
        QCOMPARE(fileStack->currentWidget(), originalFilePanel);
        QCOMPARE(terminal->plainText(), buffer);
        QVERIFY(session->isConnected());
        QCOMPARE(connectionChanges.count(), 0);
        QCOMPARE(credentials.loadCalls, 0);

        // Repeated navigation must not accumulate splitter-rounding drift.
        for (int index = 0; index < 4; ++index) {
            newTab->click();
            sessions->tabBarClicked(0);
            QTRY_VERIFY(splitRestored());
        }

        // Explicitly hiding files must remain in effect after visiting home.
        auto *fileToggle = workspace->findChild<QToolButton *>(QStringLiteral("fileWorkspaceToggleButton"));
        QVERIFY(fileToggle);
        fileToggle->click();
        QVERIFY(filePane->isHidden());
        newTab->click();
        sessions->tabBarClicked(0);
        QVERIFY(filePane->isHidden());
        fileToggle->click();
        QVERIFY(filePane->isVisible());
        QTRY_VERIFY(splitRestored());
        QCOMPARE(answerClose([&] { sessions->tabCloseRequested(0); }), 1);
        QTRY_COMPARE(workspace->sessionCount(), 0);
        QVERIFY(workspace->isHomePageVisible());
        QVERIFY(filePane->isHidden());
        QTRY_COMPARE(terminalPane->height(), splitter->height());
    }

    void startupAndHostBrowsingNeverReadCredentials()
    {
        QTemporaryDir directory;
        const auto database = directory.filePath(QStringLiteral("silent-startup.sqlite3"));
        noxshell::ServerRepository repository(database, false);
        QVERIFY(repository.initialize());
        noxshell::ServerProfile profile;
        profile.id = QStringLiteral("silent-startup");
        profile.name = QStringLiteral("silent-startup");
        profile.host = QStringLiteral("192.0.2.10");
        profile.user = QStringLiteral("test");
        profile.connectionMode = noxshell::ConnectionMode::Ssh;
        profile.credentialRef = QStringLiteral("server/silent-startup");
        QVERIFY(repository.saveServer(profile));
        QVERIFY(repository.recordSuccessfulLogin(profile.id));
        MemoryCredentialStore credentials;
        credentials.failLoads = true;
        for (int startup = 0; startup < 3; ++startup) {
            noxshell::ui::MainWindow window(database, nullptr, &credentials);
            window.show();
            QTest::qWait(80);
            auto *sidebar = window.findChild<noxshell::ui::HostSidebar *>();
            QVERIFY(sidebar);
            sidebar->selectFirstServer();
            QVERIFY(sidebar->selectServerById(profile.id));
            auto *recent = window.findChild<QTreeWidget *>(QStringLiteral("recentLoginList"));
            QVERIFY(recent);
            QCOMPARE(recent->topLevelItemCount(), 1);
            QCOMPARE(credentials.loadCalls, 0);
            QCOMPARE(credentials.saveCalls, 0);
        }
    }

    void unreadableCredentialUsesSshPasswordWithoutAuthorization()
    {
        QTemporaryDir directory;
        noxshell::ServerRepository repository(directory.filePath(QStringLiteral("password-entry.sqlite3")), false);
        QVERIFY(repository.initialize());
        MemoryCredentialStore credentials;
        credentials.failLoads = true;
        noxshell::ServerProfile profile;
        profile.id = QStringLiteral("password-entry");
        profile.name = profile.id;
        profile.host = QStringLiteral("192.0.2.10");
        profile.user = QStringLiteral("test");
        profile.connectionMode = noxshell::ConnectionMode::Ssh;
        profile.credentialRef = QStringLiteral("server/old-entry");
        QVERIFY(repository.saveServer(profile));
        credentials.secrets.insert(profile.credentialRef, {QStringLiteral("old-synthetic-only"), {}});
        noxshell::ui::TerminalWorkspace workspace(&repository, &credentials);
        workspace.openOrActivate(profile, false);
        workspace.show();
        auto *session = workspace.findChild<noxshell::SshSession *>();
        QVERIFY(session);
        QObject::disconnect(session, &noxshell::SshSession::connectRequested, nullptr, nullptr);
        QSignalSpy requests(session, &noxshell::SshSession::connectRequested);
        QSignalSpy saved(session, &noxshell::SshSession::credentialReferenceChanged);
        auto *password = workspace.findChild<QLineEdit *>(QStringLiteral("terminalConnectionPassword"));
        auto *submit = workspace.findChild<QPushButton *>(QStringLiteral("terminalPasswordConnectButton"));
        auto *remember = workspace.findChild<QCheckBox *>(QStringLiteral("terminalRememberPassword"));
        QVERIFY(password && submit && remember);
        QVERIFY(!workspace.findChild<QPushButton *>(QStringLiteral("terminalAuthorizeCredentialsButton")));
        for (int attempt = 0; attempt < 3; ++attempt) workspace.openOrActivate(profile, true);
        QCOMPARE(requests.size(), 0);
        QVERIFY(password->isVisible());
        QCOMPARE(password->echoMode(), QLineEdit::Password);
        const auto screenshot = qEnvironmentVariable("NOXSHELL_AUTH_UI_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            workspace.resize(900, 480);
            QTest::qWait(50);
            QVERIFY(workspace.grab().save(screenshot));
        }
        submit->click(); // Do not try an empty password.
        QCOMPARE(requests.size(), 0);
        const auto reads = credentials.loadCalls;
        const auto exact = QStringLiteral("  SSH.password $();  ");
        password->insert(QStringLiteral("  ＳＳＨ。password $();  "));
        QCOMPARE(password->text(), exact);
        submit->click();
        submit->click(); // Duplicate submit cannot enqueue another handshake.
        QCOMPARE(credentials.loadCalls, reads);
        QCOMPARE(credentials.saveCalls, 0); // Not remembered before authentication.
        QCOMPARE(requests.size(), 1);
        QCOMPARE(qvariant_cast<noxshell::ServerProfile>(requests.first().at(0)).password, exact);
        QVERIFY(password->text().isEmpty());
        QVERIFY(session->profile().password.isEmpty());
        QVERIFY(!password->isVisible());
        const auto generation = requests.first().at(1).toULongLong();
        QVERIFY(QMetaObject::invokeMethod(session, "handleConnectionChanged", Qt::DirectConnection,
            Q_ARG(bool, true), Q_ARG(QString, QStringLiteral("SSH 已连接")), Q_ARG(quint64, generation)));
        QCOMPARE(saved.size(), 1);
        QCOMPARE(credentials.saveCalls, 1);
        const auto current = repository.loadServers().first();
        QVERIFY(current.credentialRef != profile.credentialRef);
        QCOMPARE(credentials.secrets.value(profile.credentialRef).password, QStringLiteral("old-synthetic-only"));
        QCOMPARE(credentials.secrets.value(current.credentialRef).password, exact);
        QCOMPARE(session->profile().credentialRef, current.credentialRef);

        // A new session reads the new reference; no additional password entry.
        credentials.failLoads = false;
        noxshell::SshSession restarted(&repository, &credentials);
        QObject::disconnect(&restarted, &noxshell::SshSession::connectRequested, nullptr, nullptr);
        QSignalSpy restartedRequests(&restarted, &noxshell::SshSession::connectRequested);
        restarted.connectTo(current);
        QCOMPARE(restartedRequests.size(), 1);
        QCOMPARE(qvariant_cast<noxshell::ServerProfile>(restartedRequests.first().at(0)).password, exact);
    }

    void credentialReferenceUpdatePreservesEditsAndNeverResurrectsDeletedHost()
    {
        QTemporaryDir directory;
        noxshell::ServerRepository repository(directory.filePath(QStringLiteral("reference-cas.sqlite3")), false);
        QVERIFY(repository.initialize());
        noxshell::ServerProfile original;
        original.id = QStringLiteral("reference-cas");
        original.host = QStringLiteral("192.0.2.10");
        original.user = QStringLiteral("test");
        original.credentialRef = QStringLiteral("old-reference");
        QVERIFY(repository.saveServer(original));
        auto edited = original;
        edited.name = QStringLiteral("renamed-during-login");
        QVERIFY(repository.saveServer(edited));
        QVERIFY(repository.replaceCredentialReference(original, QStringLiteral("new-reference")));
        QCOMPARE(repository.loadServers().first().name, edited.name);
        QCOMPARE(repository.loadServers().first().credentialRef, QStringLiteral("new-reference"));
        QVERIFY(!repository.replaceCredentialReference(original, QStringLiteral("stale-reference")));
        edited.credentialRef = QStringLiteral("new-reference");
        QVERIFY(repository.deleteServer(original.id));
        QVERIFY(!repository.replaceCredentialReference(edited, QStringLiteral("deleted-reference")));
        QVERIFY(repository.loadServers().isEmpty());
    }

    void privateKeyRecoveryCanExplicitlyUseEmptyPassphrase()
    {
        MemoryCredentialStore credentials;
        credentials.failLoads = true;
        noxshell::SshSession session(nullptr, &credentials);
        QObject::disconnect(&session, &noxshell::SshSession::connectRequested, nullptr, nullptr);
        QSignalSpy requests(&session, &noxshell::SshSession::connectRequested);
        QSignalSpy passwordRequired(&session, &noxshell::SshSession::passwordRequired);
        noxshell::ServerProfile profile;
        profile.connectionMode = noxshell::ConnectionMode::Ssh;
        profile.authentication = noxshell::AuthenticationMethod::PrivateKey;
        profile.privateKeyPath = QStringLiteral("/synthetic/key");
        profile.credentialRef = QStringLiteral("unreadable-old-key");
        session.connectTo(profile);
        QCOMPARE(passwordRequired.size(), 1);
        QCOMPARE(requests.size(), 0);
        session.connectWithPassword({}, false);
        QCOMPARE(credentials.loadCalls, 1);
        QCOMPARE(requests.size(), 1);
        const auto requested = qvariant_cast<noxshell::ServerProfile>(requests.first().at(0));
        QVERIFY(requested.keyPassphrase.isEmpty());
        QCOMPARE(requested.privateKeyPath, profile.privateKeyPath);
    }

    void normalizedPasswordReachesLocalSshServer()
    {
        const auto executable = qEnvironmentVariable("NOXSHELL_AUTH_TEST_SERVER");
        if (executable.isEmpty()) QSKIP("Requires the optional loopback-only synthetic SSH fixture");
        QProcess server;
        server.start(executable, {});
        const auto cleanup = qScopeGuard([&] {
            if (server.state() != QProcess::NotRunning) { server.kill(); server.waitForFinished(2000); }
        });
        QVERIFY(server.waitForStarted(3000));
        QTRY_VERIFY_WITH_TIMEOUT(server.canReadLine(), 5000);
        const auto endpoint = QJsonDocument::fromJson(server.readLine()).object();
        QCOMPARE(endpoint.value(QStringLiteral("event")).toString(), QStringLiteral("listening"));
        const auto port = endpoint.value(QStringLiteral("port")).toInt();
        QVERIFY(port > 0 && port <= 65535);
        QTemporaryDir directory;
        noxshell::ServerRepository repository(directory.filePath(QStringLiteral("ui-ssh.sqlite3")), false);
        QVERIFY(repository.initialize());
        MemoryCredentialStore credentials;
        credentials.failLoads = true;
        noxshell::ServerProfile profile;
        profile.id = QStringLiteral("ui-ssh-synthetic");
        profile.name = profile.id;
        profile.host = QStringLiteral("127.0.0.1");
        profile.port = static_cast<quint16>(port);
        profile.user = QStringLiteral("fixture-user");
        profile.connectionMode = noxshell::ConnectionMode::Ssh;
        profile.credentialRef = QStringLiteral("old-unreadable-test-entry");
        profile.expectedFingerprint = endpoint.value(QStringLiteral("fingerprint")).toString();
        QVERIFY(profile.expectedFingerprint.startsWith(QStringLiteral("SHA256:")));
        QVERIFY(repository.saveServer(profile));
        noxshell::ui::TerminalWorkspace workspace(&repository, &credentials);
        workspace.show();
        workspace.openOrActivate(profile, true);
        auto *session = workspace.findChild<noxshell::SshSession *>();
        auto *password = workspace.findChild<QLineEdit *>(QStringLiteral("terminalConnectionPassword"));
        QVERIFY(session && password && password->isVisible());
        QSignalSpy requests(session, &noxshell::SshSession::connectRequested);
        const auto exact = QStringLiteral("noxshell-integration-test-only");
        password->insert(QStringLiteral("ｎｏｘｓｈｅｌｌ－ｉｎｔｅｇｒａｔｉｏｎ－ｔｅｓｔ－ｏｎｌｙ"));
        QCOMPARE(password->text(), exact);
        QTest::keyClick(password, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(session->isConnected(), 8000);
        QCOMPARE(requests.size(), 1);
        QCOMPARE(credentials.loadCalls, 1);
        QCOMPARE(credentials.saveCalls, 1);
        const auto saved = repository.loadServers().first();
        QVERIFY(saved.credentialRef != profile.credentialRef);
        QCOMPARE(credentials.secrets.value(saved.credentialRef).password, exact);
        QVERIFY(session->profile().password.isEmpty());
        QVERIFY(password->text().isEmpty());
        session->disconnectFromHost();
    }

    void rejectedStoredPasswordCanBeReplacedWithoutRereadingIt()
    {
        MemoryCredentialStore credentials;
        noxshell::ServerProfile profile;
        profile.id = QStringLiteral("rejected-stored-password");
        profile.host = QStringLiteral("192.0.2.10");
        profile.port = 2222;
        profile.user = QStringLiteral("test");
        profile.connectionMode = noxshell::ConnectionMode::Ssh;
        profile.credentialRef = QStringLiteral("readable-but-rejected");
        credentials.secrets.insert(profile.credentialRef, {QStringLiteral("old-test-password"), {}});
        noxshell::ui::TerminalWorkspace workspace(nullptr, &credentials);
        workspace.openOrActivate(profile, false);
        workspace.show();
        auto *session = workspace.findChild<noxshell::SshSession *>();
        QVERIFY(session);
        QObject::disconnect(session, &noxshell::SshSession::connectRequested, nullptr, nullptr);
        QSignalSpy requests(session, &noxshell::SshSession::connectRequested);
        workspace.openOrActivate(profile, true);
        QCOMPARE(requests.size(), 1);
        const auto generation = requests.last().at(1).toULongLong();
        QVERIFY(QMetaObject::invokeMethod(session, "handleConnectionChanged", Qt::DirectConnection,
            Q_ARG(bool, false), Q_ARG(QString, QStringLiteral("SSH 认证失败")), Q_ARG(quint64, generation)));
        QVERIFY(QMetaObject::invokeMethod(session, "handlePasswordRejected", Qt::DirectConnection, Q_ARG(quint64, generation)));
        auto *password = workspace.findChild<QLineEdit *>(QStringLiteral("terminalConnectionPassword"));
        auto *button = workspace.findChild<QPushButton *>(QStringLiteral("terminalPasswordConnectButton"));
        auto *detail = workspace.findChild<QLabel *>(QStringLiteral("terminalLoadingDetail"));
        QVERIFY(password && button && detail && password->isVisible());
        QVERIFY(detail->text().contains(QStringLiteral("test@192.0.2.10:2222")));
        QCOMPARE(requests.size(), 1); // No automatic network retry.
        password->setText(QStringLiteral("corrected-test-password"));
        button->click();
        QCOMPARE(requests.size(), 2);
        QCOMPARE(credentials.loadCalls, 1);
        QCOMPARE(qvariant_cast<noxshell::ServerProfile>(requests.last().at(0)).password, QStringLiteral("corrected-test-password"));
        QVERIFY(!password->isVisible());
        QVERIFY(QMetaObject::invokeMethod(session, "handlePasswordRejected", Qt::DirectConnection, Q_ARG(quint64, generation)));
        QVERIFY(!password->isVisible()); // Late rejection from the previous attempt is discarded.
        session->disconnectFromHost();
    }

    void passwordRecoveryDoesNotSaveFailuresOrCanceledAttempts()
    {
        QTemporaryDir directory;
        noxshell::ServerRepository repository(directory.filePath(QStringLiteral("password-failure.sqlite3")), false);
        QVERIFY(repository.initialize());
        MemoryCredentialStore credentials;
        credentials.failLoads = true;
        noxshell::ServerProfile profile;
        profile.id = QStringLiteral("password-failure");
        profile.name = profile.id;
        profile.host = QStringLiteral("192.0.2.10");
        profile.user = QStringLiteral("test");
        profile.connectionMode = noxshell::ConnectionMode::Ssh;
        profile.credentialRef = QStringLiteral("server/old-failure");
        QVERIFY(repository.saveServer(profile));
        noxshell::SshSession session(&repository, &credentials);
        QObject::disconnect(&session, &noxshell::SshSession::connectRequested, nullptr, nullptr);
        QSignalSpy requests(&session, &noxshell::SshSession::connectRequested);
        QSignalSpy notices(&session, &noxshell::SshSession::credentialSaveNotice);
        const auto begin = [&] {
            session.connectTo(profile);
            session.connectWithPassword(QStringLiteral("synthetic-only"), true);
            return requests.last().at(1).toULongLong();
        };
        auto generation = begin();
        QVERIFY(QMetaObject::invokeMethod(&session, "handleConnectionChanged", Qt::DirectConnection,
            Q_ARG(bool, false), Q_ARG(QString, QStringLiteral("SSH 认证失败")), Q_ARG(quint64, generation)));
        QCOMPARE(credentials.saveCalls, 0);
        generation = begin();
        session.disconnectFromHost();
        QVERIFY(QMetaObject::invokeMethod(&session, "handleConnectionChanged", Qt::DirectConnection,
            Q_ARG(bool, true), Q_ARG(QString, QStringLiteral("SSH 已连接")), Q_ARG(quint64, generation)));
        QCOMPARE(credentials.saveCalls, 0);
        const auto count = requests.size();
        session.connectWithPassword(QStringLiteral("stale-password"), true);
        QCOMPARE(requests.size(), count);
        generation = begin();
        credentials.failSaves = true;
        QVERIFY(QMetaObject::invokeMethod(&session, "handleConnectionChanged", Qt::DirectConnection,
            Q_ARG(bool, true), Q_ARG(QString, QStringLiteral("SSH 已连接")), Q_ARG(quint64, generation)));
        QVERIFY(session.isConnected()); // Remembering is optional, login is not blocked.
        QCOMPARE(notices.size(), 1);
        QCOMPARE(repository.loadServers().first().credentialRef, profile.credentialRef);
        credentials.failSaves = false;
        session.connectTo(profile);
        session.connectWithPassword(QStringLiteral("session-only"), false);
        generation = requests.last().at(1).toULongLong();
        const auto saves = credentials.saveCalls;
        QVERIFY(QMetaObject::invokeMethod(&session, "handleConnectionChanged", Qt::DirectConnection,
            Q_ARG(bool, true), Q_ARG(QString, QStringLiteral("SSH 已连接")), Q_ARG(quint64, generation)));
        QCOMPARE(credentials.saveCalls, saves);
        generation = begin();
        profile.host = QStringLiteral("192.0.2.20");
        QVERIFY(repository.saveServer(profile)); // Host edited while authentication was in flight.
        QVERIFY(QMetaObject::invokeMethod(&session, "handleConnectionChanged", Qt::DirectConnection,
            Q_ARG(bool, true), Q_ARG(QString, QStringLiteral("SSH 已连接")), Q_ARG(quint64, generation)));
        QCOMPARE(credentials.saveCalls, saves);
        QCOMPARE(repository.loadServers().first().host, profile.host);
    }

    void connectionTestRequestsSshPasswordInline()
    {
        QTemporaryDir directory;
        noxshell::ServerRepository repository(directory.filePath(QStringLiteral("silent-test.sqlite3")), false);
        QVERIFY(repository.initialize());
        MemoryCredentialStore credentials;
        credentials.failLoads = true;
        noxshell::ServerProfile profile;
        profile.id = QStringLiteral("silent-dialog");
        profile.name = profile.id;
        profile.host = QStringLiteral("192.0.2.10");
        profile.user = QStringLiteral("test");
        profile.connectionMode = noxshell::ConnectionMode::Ssh;
        profile.credentialRef = QStringLiteral("server/silent-dialog");
        noxshell::ui::ServerDialog dialog(profile);
        dialog.setConnectionServices(&repository, &credentials);
        dialog.show();
        auto *test = dialog.findChild<QPushButton *>(QStringLiteral("dialogTestConnectionButton"));
        auto *password = dialog.findChild<QLineEdit *>(QStringLiteral("passwordEditor"));
        QVERIFY(test && password);
        QVERIFY(!dialog.findChild<QPushButton *>(QStringLiteral("dialogAuthorizeCredentialsButton")));
        QCOMPARE(credentials.loadCalls, 0);
        test->click();
        QCOMPARE(credentials.loadCalls, 1);
        QVERIFY(test->isEnabled());
        QVERIFY(!dialog.findChild<QProgressDialog *>());
        QVERIFY(!dialog.findChild<QMessageBox *>());
        QVERIFY(password->text().isEmpty());
    }

    void rdpUnreadableCredentialOffersRemotePasswordEditor()
    {
        QTemporaryDir directory;
        MemoryCredentialStore credentials;
        credentials.failLoads = true;
        noxshell::ui::MainWindow window(directory.filePath(QStringLiteral("silent-rdp.sqlite3")), nullptr, &credentials);
        window.show();
        auto *sidebar = window.findChild<noxshell::ui::HostSidebar *>();
        QVERIFY(sidebar);
        noxshell::ServerProfile profile;
        profile.id = QStringLiteral("silent-rdp");
        profile.name = profile.id;
        profile.host = QStringLiteral("192.0.2.10");
        profile.user = QStringLiteral("test");
        profile.connectionMode = noxshell::ConnectionMode::Rdp;
        profile.credentialRef = QStringLiteral("rdp/silent-rdp");
        auto *enter = window.findChild<QPushButton *>(QStringLiteral("rdpEnterPasswordButton"));
        QVERIFY(enter);
        sidebar->serverConnectRequested(profile);
        QCOMPARE(credentials.loadCalls, 1);
        QVERIFY(enter->isVisible());
        bool edited = false;
        QTimer::singleShot(0, &window, [&] {
            if (auto *dialog = window.findChild<noxshell::ui::RdpDialog *>()) {
                edited = true;
                dialog->reject(); // Never launch an actual RDP client.
            }
        });
        enter->click();
        QVERIFY(edited);
        QCOMPARE(credentials.loadCalls, 1);
        QCOMPARE(credentials.saveCalls, 0);
    }

    void repeatedConnectDuringCredentialAuthorizationUsesOneRequest()
    {
        MemoryCredentialStore credentials;
        noxshell::ServerProfile profile;
        profile.id = QStringLiteral("authorization-test");
        profile.name = QStringLiteral("授权测试");
        profile.host = QStringLiteral("192.0.2.10");
        profile.user = QStringLiteral("test-user");
        profile.connectionMode = noxshell::ConnectionMode::Ssh;
        profile.credentialRef = QStringLiteral("server/authorization-test");
        credentials.secrets.insert(profile.credentialRef, {QStringLiteral("synthetic-only"), {}});
        noxshell::ui::TerminalWorkspace workspace(nullptr, &credentials);
        workspace.openOrActivate(profile, false);
        workspace.show();
        auto *session = workspace.findChild<noxshell::SshSession *>();
        QVERIFY(session);
        QObject::disconnect(session, &noxshell::SshSession::connectRequested, nullptr, nullptr);
        QSignalSpy requests(session, &noxshell::SshSession::connectRequested);
        bool connectingDuringAuthorization = false;
        credentials.onLoad = [&] {
            auto *stack = workspace.findChild<QStackedWidget *>(QStringLiteral("terminalSessionStack"));
            connectingDuringAuthorization = stack && stack->currentWidget()
                && stack->currentWidget()->property("terminalConnectionPhase").toInt() == 1;
            QEventLoop authorization;
            QTimer::singleShot(0, &authorization, [&] {
                workspace.openOrActivate(profile, true);
                authorization.quit();
            });
            authorization.exec();
        };
        workspace.openOrActivate(profile, true);
        QCOMPARE(credentials.loadCalls, 1);
        QCOMPARE(requests.size(), 1);
        QVERIFY(connectingDuringAuthorization);
        QVERIFY(workspace.hasConnectingSession(profile.id));
        // Repeated clicks during TCP/authentication must not restart the attempt.
        workspace.openOrActivate(profile, true);
        QCOMPARE(credentials.loadCalls, 1);
        QCOMPARE(requests.size(), 1);
    }

    void canceledCredentialAuthorizationCannotStartSshLater()
    {
        MemoryCredentialStore credentials;
        noxshell::SshSession session(nullptr, &credentials);
        QObject::disconnect(&session, &noxshell::SshSession::connectRequested, nullptr, nullptr);
        QSignalSpy requests(&session, &noxshell::SshSession::connectRequested);
        noxshell::ServerProfile profile;
        profile.connectionMode = noxshell::ConnectionMode::Ssh;
        profile.host = QStringLiteral("192.0.2.10");
        profile.user = QStringLiteral("test-user");
        profile.credentialRef = QStringLiteral("server/canceled-authorization");
        credentials.secrets.insert(profile.credentialRef, {QStringLiteral("synthetic-only"), {}});
        credentials.onLoad = [&] { session.disconnectFromHost(); };
        session.connectTo(profile);
        QCOMPARE(requests.size(), 0);
        session.connectTo(profile);
        QCOMPARE(requests.size(), 1);
        QCOMPARE(credentials.loadCalls, 2);
    }

    void closingTabDuringAuthorizationDiscardsItsResult()
    {
        MemoryCredentialStore credentials;
        noxshell::ServerProfile profile;
        profile.id = QStringLiteral("closed-authorization");
        profile.host = QStringLiteral("192.0.2.10");
        profile.user = QStringLiteral("test-user");
        profile.connectionMode = noxshell::ConnectionMode::Ssh;
        profile.credentialRef = QStringLiteral("server/closed-authorization");
        credentials.secrets.insert(profile.credentialRef, {QStringLiteral("synthetic-only"), {}});
        noxshell::ui::TerminalWorkspace workspace(nullptr, &credentials);
        workspace.openOrActivate(profile, false);
        auto *session = workspace.findChild<noxshell::SshSession *>();
        QVERIFY(session);
        QObject::disconnect(session, &noxshell::SshSession::connectRequested, nullptr, nullptr);
        QSignalSpy requests(session, &noxshell::SshSession::connectRequested);
        credentials.onLoad = [&] {
            workspace.closeServer(profile.id);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        };
        workspace.openOrActivate(profile, true);
        QCOMPARE(requests.size(), 0);
        QCOMPARE(workspace.sessionCount(), 0);
    }

    void localRememberedCredentialSurvivesRepositoryAndSessionRestart()
    {
#ifdef Q_OS_MACOS
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto database = directory.filePath(QStringLiteral("local-restart.sqlite3"));
        const auto vault = directory.filePath(QStringLiteral("vault"));
        const auto password = QStringLiteral("synthetic-upgrade-once");
        QString savedReference;
        {
            noxshell::ServerRepository repository(database, false);
            QVERIFY(repository.initialize());
            noxshell::CredentialStore credentials(vault, noxshell::CredentialStore::LegacyReader{}, nullptr);
            noxshell::ServerProfile profile;
            profile.id = QStringLiteral("local-restart");
            profile.name = profile.id;
            profile.host = QStringLiteral("192.0.2.10");
            profile.user = QStringLiteral("test");
            profile.connectionMode = noxshell::ConnectionMode::Ssh;
            profile.credentialRef = QStringLiteral("old-unreadable-reference");
            QVERIFY(repository.saveServer(profile));
            noxshell::SshSession session(&repository, &credentials);
            QObject::disconnect(&session, &noxshell::SshSession::connectRequested, nullptr, nullptr);
            QSignalSpy requests(&session, &noxshell::SshSession::connectRequested);
            QSignalSpy prompts(&session, &noxshell::SshSession::passwordRequired);
            QSignalSpy notices(&session, &noxshell::SshSession::credentialSaveNotice);
            session.connectTo(profile);
            QCOMPARE(prompts.size(), 1);
            session.connectWithPassword(password, true);
            QCOMPARE(requests.size(), 1);
            // Simulated authentication result; transport is disconnected above.
            const auto generation = requests.last().at(1).toULongLong();
            QVERIFY(QMetaObject::invokeMethod(&session, "handleConnectionChanged", Qt::DirectConnection,
                Q_ARG(bool, true), Q_ARG(QString, QStringLiteral("SSH 已连接")), Q_ARG(quint64, generation)));
            QCOMPARE(notices.size(), 0);
            savedReference = repository.loadServers().first().credentialRef;
            QVERIFY(savedReference != profile.credentialRef);
            QCOMPARE(credentials.load(savedReference).password, password);
        }
        // No shared store/session/repository objects and no cached password.
        for (int restart = 0; restart < 2; ++restart) {
            noxshell::ServerRepository repository(database, false);
            QVERIFY(repository.initialize());
            int legacyCalls = 0;
            noxshell::CredentialStore credentials(vault, [&](const QString &, QByteArray &, QString &, bool &) { ++legacyCalls; return false; });
            const auto profile = repository.loadServers().first();
            QCOMPARE(profile.credentialRef, savedReference);
            QVERIFY(profile.password.isEmpty());
            noxshell::SshSession session(&repository, &credentials);
            QObject::disconnect(&session, &noxshell::SshSession::connectRequested, nullptr, nullptr);
            QSignalSpy requests(&session, &noxshell::SshSession::connectRequested);
            QSignalSpy prompts(&session, &noxshell::SshSession::passwordRequired);
            session.connectTo(profile);
            QCOMPARE(prompts.size(), 0);
            QCOMPARE(legacyCalls, 0);
            QCOMPARE(requests.size(), 1);
            QCOMPARE(qvariant_cast<noxshell::ServerProfile>(requests.first().at(0)).password, password);
        }
        QFile databaseFile(database);
        QVERIFY(databaseFile.open(QIODevice::ReadOnly));
        QVERIFY(!databaseFile.readAll().contains(password.toUtf8()));
#else
        QSKIP("macOS-only local credential backend");
#endif
    }

    void credentialFailureIsVisibleAndCanBeRetried()
    {
        MemoryCredentialStore credentials;
        credentials.failLoads = true;
        noxshell::ServerProfile profile;
        profile.id = QStringLiteral("denied-authorization");
        profile.host = QStringLiteral("192.0.2.10");
        profile.user = QStringLiteral("test-user");
        profile.connectionMode = noxshell::ConnectionMode::Ssh;
        profile.credentialRef = QStringLiteral("server/denied-authorization");
        noxshell::ui::TerminalWorkspace workspace(nullptr, &credentials);
        workspace.openOrActivate(profile, false);
        auto *session = workspace.findChild<noxshell::SshSession *>();
        auto *output = workspace.findChild<noxshell::ui::TerminalView *>();
        QVERIFY(session);
        QVERIFY(output);
        QObject::disconnect(session, &noxshell::SshSession::connectRequested, nullptr, nullptr);
        QSignalSpy requests(session, &noxshell::SshSession::connectRequested);
        workspace.openOrActivate(profile, true);
        QCOMPARE(requests.size(), 0);
        QVERIFY(workspace.findChild<QLineEdit *>(QStringLiteral("terminalConnectionPassword")));
        credentials.failLoads = false;
        credentials.secrets.insert(profile.credentialRef, {QStringLiteral("synthetic-only"), {}});
        workspace.openOrActivate(profile, true);
        QCOMPARE(requests.size(), 1);
        QCOMPARE(credentials.loadCalls, 2);
    }

    void sshCredentialLoadingPreservesExplicitInputAndStopsOnFailure()
    {
        MemoryCredentialStore credentials;
        const auto exactPassword = QStringLiteral("  pass。中文é！‘’\u00a0🔑  ");
        const auto reference = QStringLiteral("server/credential-regression");
        credentials.secrets.insert(reference, {exactPassword, QStringLiteral("密钥。口令 ")});
        noxshell::SshSession session(nullptr, &credentials);
        // Observe the request before the transport without opening any socket.
        QObject::disconnect(&session, &noxshell::SshSession::connectRequested, nullptr, nullptr);
        QSignalSpy requests(&session, &noxshell::SshSession::connectRequested);
        QSignalSpy states(&session, &noxshell::SshSession::connectionChanged);
        noxshell::ServerProfile profile;
        profile.connectionMode = noxshell::ConnectionMode::Ssh;
        profile.authentication = noxshell::AuthenticationMethod::Password;
        profile.host = QStringLiteral("192.0.2.10");
        profile.user = QStringLiteral("root");
        profile.credentialRef = reference;
        profile.password = QStringLiteral("explicit。新密码 ");
        credentials.failLoads = true;
        session.connectTo(profile);
        QCOMPARE(credentials.loadCalls, 0);
        QCOMPARE(requests.count(), 1);
        QCOMPARE(qvariant_cast<noxshell::ServerProfile>(requests.takeFirst().at(0)).password.toUtf8(), profile.password.toUtf8());

        profile.password.clear();
        credentials.failLoads = false;
        session.connectTo(profile);
        QCOMPARE(credentials.loadCalls, 1);
        QCOMPARE(requests.count(), 1);
        QCOMPARE(qvariant_cast<noxshell::ServerProfile>(requests.takeFirst().at(0)).password.toUtf8(), exactPassword.toUtf8());

        credentials.failLoads = true;
        session.connectTo(profile);
        QCOMPARE(requests.count(), 0);
        QVERIFY(states.last().at(1).toString().contains(QStringLiteral("SSH 密码")));

        profile.authentication = noxshell::AuthenticationMethod::PrivateKey;
        profile.keyPassphrase = QStringLiteral("explicit。私钥 ");
        const auto loadsBeforeExplicitKey = credentials.loadCalls;
        session.connectTo(profile);
        QCOMPARE(credentials.loadCalls, loadsBeforeExplicitKey);
        QCOMPARE(qvariant_cast<noxshell::ServerProfile>(requests.takeFirst().at(0)).keyPassphrase, profile.keyPassphrase);

        profile.keyPassphrase.clear();
        credentials.failLoads = false;
        session.connectTo(profile);
        QCOMPARE(qvariant_cast<noxshell::ServerProfile>(requests.takeFirst().at(0)).keyPassphrase,
            QStringLiteral("密钥。口令 "));

        credentials.failLoads = true;
        profile.authentication = noxshell::AuthenticationMethod::SshAgent;
        const auto loadsBeforeAgent = credentials.loadCalls;
        session.connectTo(profile);
        QCOMPARE(credentials.loadCalls, loadsBeforeAgent);
        QCOMPARE(requests.count(), 1);
    }

    void newServerCredentialsRoundTripAndFailedEditDoesNotOverwrite()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        MemoryCredentialStore credentials;
        noxshell::ui::MainWindow window(directory.filePath(QStringLiteral("credentials.sqlite3")), nullptr, &credentials);
        auto *sidebar = window.findChild<noxshell::ui::HostSidebar *>();
        auto *repository = window.findChild<noxshell::ServerRepository *>();
        QVERIFY(sidebar);
        QVERIFY(repository);
        const auto typedPassword = QStringLiteral("  ＳＳＨ。password！‘’\u00a0  ");
        const auto exactPassword = QStringLiteral("  SSH.password!''   ");
        bool populated = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<noxshell::ui::ServerDialog *>();
            if (!dialog) return;
            dialog->findChild<QLineEdit *>(QStringLiteral("nameEditor"))->setText(QStringLiteral("credential-round-trip"));
            dialog->findChild<QLineEdit *>(QStringLiteral("hostEditor"))->setText(QStringLiteral("192.0.2.11"));
            dialog->findChild<QLineEdit *>(QStringLiteral("userEditor"))->setText(QStringLiteral("root"));
            dialog->findChild<QLineEdit *>(QStringLiteral("passwordEditor"))->insert(typedPassword);
            populated = true;
            dialog->findChild<QPushButton *>(QStringLiteral("primaryButton"))->click();
        });
        sidebar->addServerRequested();
        QVERIFY(populated);
        QCOMPARE(credentials.saveCalls, 1);
        const auto profiles = repository->loadServers();
        const auto found = std::find_if(profiles.cbegin(), profiles.cend(), [](const auto &profile) {
            return profile.name == QStringLiteral("credential-round-trip");
        });
        QVERIFY(found != profiles.cend());
        const auto saved = *found;
        QVERIFY(!saved.id.isEmpty());
        QVERIFY(!saved.credentialRef.isEmpty());
        QVERIFY(saved.password.isEmpty());
        QCOMPARE(credentials.secrets.value(saved.credentialRef).password.toUtf8(), exactPassword.toUtf8());

        noxshell::SshSession session(repository, &credentials);
        QObject::disconnect(&session, &noxshell::SshSession::connectRequested, nullptr, nullptr);
        QSignalSpy requests(&session, &noxshell::SshSession::connectRequested);
        session.connectTo(saved);
        QCOMPARE(requests.count(), 1);
        QCOMPARE(qvariant_cast<noxshell::ServerProfile>(requests.takeFirst().at(0)).password.toUtf8(), exactPassword.toUtf8());

        credentials.failLoads = true;
        bool sawFailure = false;
        const auto readsBeforeMetadataEdit = credentials.loadCalls;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<noxshell::ui::ServerDialog *>();
            if (!dialog) return;
            dialog->findChild<QLineEdit *>(QStringLiteral("nameEditor"))->setText(QStringLiteral("renamed-without-keychain"));
            QTimer::singleShot(0, &window, [&] {
                for (auto *widget : QApplication::topLevelWidgets()) {
                    if (auto *message = qobject_cast<QMessageBox *>(widget)) {
                        sawFailure = message->text().contains(QStringLiteral("未覆盖"));
                        message->accept();
                    }
                }
            });
            dialog->findChild<QPushButton *>(QStringLiteral("primaryButton"))->click();
        });
        sidebar->serverEditRequested(saved);
        QVERIFY(!sawFailure);
        QCOMPARE(credentials.loadCalls, readsBeforeMetadataEdit);
        QCOMPARE(credentials.saveCalls, 1);
        QCOMPARE(credentials.secrets.value(saved.credentialRef).password.toUtf8(), exactPassword.toUtf8());
        for (const auto &profile : repository->loadServers()) {
            if (profile.id == saved.id) {
                QCOMPARE(profile.name, QStringLiteral("renamed-without-keychain"));
                QCOMPARE(profile.credentialRef, saved.credentialRef);
            }
        }
        const auto replacement = QStringLiteral("new synthetic SSH password");
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<noxshell::ui::ServerDialog *>();
            if (!dialog) return;
            dialog->findChild<QLineEdit *>(QStringLiteral("passwordEditor"))->setText(replacement);
            dialog->findChild<QPushButton *>(QStringLiteral("primaryButton"))->click();
        });
        sidebar->serverEditRequested(saved);
        QCOMPARE(credentials.loadCalls, readsBeforeMetadataEdit);
        QCOMPARE(credentials.saveCalls, 2);
        QCOMPARE(credentials.secrets.value(saved.credentialRef).password, exactPassword);
        for (const auto &profile : repository->loadServers()) {
            if (profile.id != saved.id) continue;
            QVERIFY(profile.credentialRef != saved.credentialRef);
            QCOMPARE(credentials.secrets.value(profile.credentialRef).password, replacement);
        }
    }

    void agentCanSwitchToUnencryptedPrivateKeyWithoutLoadingMissingCredentials()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        MemoryCredentialStore credentials;
        credentials.failLoads = true;
        noxshell::ui::MainWindow window(directory.filePath(QStringLiteral("agent-to-key.sqlite3")), nullptr, &credentials);
        auto *sidebar = window.findChild<noxshell::ui::HostSidebar *>();
        auto *repository = window.findChild<noxshell::ServerRepository *>();
        QVERIFY(sidebar);
        QVERIFY(repository);
        noxshell::ServerProfile agent;
        agent.id = QStringLiteral("agent-to-key");
        agent.name = QStringLiteral("agent-to-key");
        agent.host = QStringLiteral("127.0.0.1");
        agent.port = 1;
        agent.user = QStringLiteral("root");
        agent.connectionMode = noxshell::ConnectionMode::Ssh;
        agent.authentication = noxshell::AuthenticationMethod::SshAgent;
        agent.privateKeyPath = directory.filePath(QStringLiteral("unencrypted-key"));
        agent.credentialRef = QStringLiteral("server/agent-to-key");
        QVERIFY(repository->saveServer(agent));
        sidebar->addServer(agent);

        bool unexpectedFailure = false;
        QTimer dismissMessages;
        connect(&dismissMessages, &QTimer::timeout, &window, [&] {
            for (auto *widget : QApplication::topLevelWidgets()) {
                if (auto *message = qobject_cast<QMessageBox *>(widget)) {
                    unexpectedFailure = true;
                    message->accept();
                }
            }
        });
        dismissMessages.start(1);

        noxshell::ui::ServerDialog testDialog(agent);
        testDialog.setConnectionServices(repository, &credentials);
        auto *testAuthentication = testDialog.findChild<QComboBox *>(QStringLiteral("authenticationEditor"));
        testAuthentication->setCurrentIndex(testAuthentication->findData(static_cast<int>(noxshell::AuthenticationMethod::PrivateKey)));
        auto *testButton = testDialog.findChild<QPushButton *>(QStringLiteral("dialogTestConnectionButton"));
        testButton->click();
        QVERIFY(!unexpectedFailure);
        QCOMPARE(credentials.loadCalls, 0);
        auto *progress = testDialog.findChild<QProgressDialog *>();
        QVERIFY(progress);
        auto *session = progress->findChild<noxshell::SshSession *>();
        QVERIFY(session);
        QCOMPARE(session->profile().authentication, noxshell::AuthenticationMethod::PrivateKey);
        QVERIFY(session->profile().keyPassphrase.isEmpty());
        QVERIFY(session->profile().credentialRef.isEmpty());
        QVERIFY(QMetaObject::invokeMethod(progress, "canceled", Qt::DirectConnection));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

        bool edited = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<noxshell::ui::ServerDialog *>();
            if (!dialog) return;
            auto *authentication = dialog->findChild<QComboBox *>(QStringLiteral("authenticationEditor"));
            authentication->setCurrentIndex(authentication->findData(static_cast<int>(noxshell::AuthenticationMethod::PrivateKey)));
            edited = true;
            dialog->findChild<QPushButton *>(QStringLiteral("primaryButton"))->click();
        });
        sidebar->serverEditRequested(agent);
        QVERIFY(edited);
        QVERIFY(!unexpectedFailure);
        QCOMPARE(credentials.loadCalls, 0);
        QCOMPARE(credentials.saveCalls, 1);
        auto privateKeyProfile = agent;
        for (const auto &profile : repository->loadServers()) {
            if (profile.id == agent.id) privateKeyProfile = profile;
        }
        QCOMPARE(privateKeyProfile.authentication, noxshell::AuthenticationMethod::PrivateKey);
        QVERIFY(privateKeyProfile.credentialRef != agent.credentialRef);
        QVERIFY(credentials.secrets.contains(privateKeyProfile.credentialRef));
        QVERIFY(credentials.secrets.value(privateKeyProfile.credentialRef).keyPassphrase.isEmpty());

        // An existing private key still needs its saved passphrase; a genuine
        // credential-store read error must not be treated as an empty passphrase.
        noxshell::ui::ServerDialog existingKeyDialog(privateKeyProfile);
        existingKeyDialog.setConnectionServices(repository, &credentials);
        existingKeyDialog.findChild<QPushButton *>(QStringLiteral("dialogTestConnectionButton"))->click();
        QVERIFY(!unexpectedFailure); // Inline server-password guidance, no modal authorization.
        QCOMPARE(credentials.loadCalls, 1);
        QVERIFY(!existingKeyDialog.findChild<QProgressDialog *>());
        QCOMPARE(credentials.saveCalls, 1);

        unexpectedFailure = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<noxshell::ui::ServerDialog *>();
            if (!dialog) return;
            dialog->findChild<QLineEdit *>(QStringLiteral("nameEditor"))->setText(QStringLiteral("new-unencrypted-key"));
            dialog->findChild<QLineEdit *>(QStringLiteral("hostEditor"))->setText(QStringLiteral("127.0.0.1"));
            dialog->findChild<QLineEdit *>(QStringLiteral("userEditor"))->setText(QStringLiteral("root"));
            auto *authentication = dialog->findChild<QComboBox *>(QStringLiteral("authenticationEditor"));
            authentication->setCurrentIndex(authentication->findData(static_cast<int>(noxshell::AuthenticationMethod::PrivateKey)));
            dialog->findChild<QPushButton *>(QStringLiteral("primaryButton"))->click();
        });
        sidebar->addServerRequested();
        QVERIFY(!unexpectedFailure);
        QCOMPARE(credentials.loadCalls, 1);
        QCOMPARE(credentials.saveCalls, 2);
        dismissMessages.stop();
    }

    void serverDialogOffersConnectionTestBeforeSave()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        noxshell::ServerRepository repository(directory.filePath(QStringLiteral("dialog-test.sqlite3")), false);
        QVERIFY(repository.initialize());
        MemoryCredentialStore credentialStore;

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
        const auto exactPassword = QStringLiteral("  abc.SSH!'' 123  ");
        passwordEditor->insert(QStringLiteral("  abc。ＳＳＨ！‘’\u00a0１２３  "));
        QCOMPARE(passwordEditor->text(), exactPassword);
        QCOMPARE(addDialog.profile().password.toUtf8(), exactPassword.toUtf8());
        QVERIFY(passwordHint->text().contains(QStringLiteral("半角空格不变")));
        auto *passphraseEditor = addDialog.findChild<QLineEdit *>(QStringLiteral("passphraseEditor"));
        QVERIFY(passphraseEditor);
        passphraseEditor->insert(QStringLiteral("  abc。ＳＳＨ！‘’\u00a0１２３  "));
        QCOMPARE(addDialog.profile().keyPassphrase.toUtf8(), exactPassword.toUtf8());
        passwordEditor->clear();
        passwordEditor->insert(QStringLiteral("temporary-password"));
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
        QVERIFY(editDialog.findChild<QLabel *>(QStringLiteral("passwordSourceHint"))->text().contains(QStringLiteral("凭据库")));
        QCOMPARE(editDialog.findChild<QPushButton *>(QStringLiteral("primaryButton"))->text(), QStringLiteral("保存修改"));
    }
};

int main(int argc, char **argv)
{
    // Native probes exercise composition/Esc only; source policy uses a fake
    // backend above, so tests never switch the user's real keyboard layout.
    qputenv("NOXSHELL_DISABLE_TERMINAL_INPUT_SOURCE_SWITCHING", "1");
    noxshell::ui::Application app(argc, argv);
#ifdef Q_OS_MACOS
    if (app.arguments().size() == 4 && app.arguments().at(1) == QStringLiteral("--silent-keychain-probe")) {
        const auto reference = app.arguments().at(2);
        if (!reference.startsWith(QStringLiteral("noxshell-test-legacy-"))
            || QUuid(reference.mid(21)).isNull()) return 2;
        Boolean before = false;
        if (SecKeychainGetUserInteractionAllowed(&before) != errSecSuccess || !before) return 3;
        noxshell::CredentialStore store(app.arguments().at(3), nullptr);
        QElapsedTimer elapsed;
        elapsed.start();
        for (int attempt = 0; attempt < 3; ++attempt) {
            const auto secret = store.load(reference);
            if (!secret.password.isEmpty() || !secret.keyPassphrase.isEmpty() || !store.authorizationRequired()) return 4;
            Boolean after = false;
            if (SecKeychainGetUserInteractionAllowed(&after) != errSecSuccess || after != before) return 5;
        }
        // Writes/deletes must not prompt either, even on an inaccessible item.
        store.save(reference, {QStringLiteral("synthetic-update-only"), {}});
        Boolean after = false;
        if (SecKeychainGetUserInteractionAllowed(&after) != errSecSuccess || after != before) return 7;
        store.remove(reference);
        if (SecKeychainGetUserInteractionAllowed(&after) != errSecSuccess || after != before) return 8;
        return elapsed.elapsed() < 2000 ? 0 : 6;
    }
    if (app.arguments().size() == 5 && app.arguments().at(1) == QStringLiteral("--owned-vault-probe")) {
        const auto reference = app.arguments().at(2);
        if (!reference.startsWith(QStringLiteral("noxshell-test-owned-")) || QUuid(reference.mid(20)).isNull()) return 2;
        Boolean before = false;
        if (SecKeychainGetUserInteractionAllowed(&before) != errSecSuccess || !before) return 3;
        noxshell::CredentialStore store(app.arguments().at(4), noxshell::CredentialStore::LegacyReader{}, nullptr);
        const auto operation = app.arguments().at(3);
        const auto password = QStringLiteral(" synthetic \"你好\" $(); ");
        const auto passphrase = QStringLiteral("synthetic-passphrase");
        bool ok = false;
        if (operation == QStringLiteral("create")) ok = store.save(reference, {password, passphrase});
        else if (operation == QStringLiteral("update")) ok = store.save(reference, {QStringLiteral("updated-synthetic-only"), passphrase});
        else if (operation == QStringLiteral("delete")) ok = store.remove(reference);
        else if (operation == QStringLiteral("read") || operation == QStringLiteral("read-updated")) {
            const auto secret = store.load(reference);
            ok = store.lastError().isEmpty() && secret.keyPassphrase == passphrase
                && secret.password == (operation == QStringLiteral("read") ? password : QStringLiteral("updated-synthetic-only"));
        }
        if (!ok) return 4;
        Boolean after = false;
        if (SecKeychainGetUserInteractionAllowed(&after) != errSecSuccess || after != before) return 5;
        return 0;
    }
#endif
    QApplication::setApplicationName(QStringLiteral("玄壳"));
    QApplication::setOrganizationName(QStringLiteral("NoxShell"));
    QApplication::setApplicationVersion(QString::fromLatin1(NOXSHELL_APP_VERSION));
    QTemporaryDir settingsDirectory;
    if (!settingsDirectory.isValid()) return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDirectory.path());
    noxshell::ui::applyApplicationTheme(noxshell::ui::ThemeMode::Light);
    if (app.arguments().contains(QStringLiteral("--window-lifecycle-probe"))) return runWindowLifecycleProbe(app);
    SmokeTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "SmokeTest.moc"
