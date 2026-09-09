#include "../src/core/Libssh2Worker.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTest>
#include <QTimer>

#include <libssh2.h>

#include <chrono>
#include <thread>

namespace {
constexpr auto kSyntheticPassword = "noxshell-integration-test-only";

// Every case starts a fresh local server so a late authentication reply from
// one case cannot accidentally make the next case pass.
class LocalSshServer {
public:
    ~LocalSshServer()
    {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(3000);
        }
    }

    bool start(const QStringList &arguments)
    {
        process.setProgram(qEnvironmentVariable("NOXSHELL_AUTH_TEST_SERVER"));
        process.setArguments(arguments);
        process.start();
        if (!process.waitForStarted(5000)) return false;
        QElapsedTimer elapsed;
        elapsed.start();
        while (!port && elapsed.elapsed() < 5000) {
            readEvents();
            if (port || process.state() == QProcess::NotRunning) break;
            process.waitForReadyRead(100);
        }
        return port != 0;
    }

    void readEvents()
    {
        // connectTo runs synchronously in this harness, so QProcess has not
        // received event-loop notifications while the SSH exchange was pending.
        process.waitForReadyRead(20);
        pending.append(process.readAllStandardOutput());
        for (;;) {
            const auto newline = pending.indexOf('\n');
            if (newline < 0) break;
            const auto event = QJsonDocument::fromJson(pending.left(newline)).object();
            pending.remove(0, newline + 1);
            events.append(event);
            if (event.value("event") == "listening") port = static_cast<quint16>(event.value("port").toInt());
        }
    }

    QStringList startedMethods()
    {
        readEvents();
        QStringList methods;
        for (const auto &event : events) {
            if (event.value("event") == "method_started") methods.append(event.value("method").toString());
        }
        return methods;
    }

    QByteArray error() { return process.readAllStandardError(); }

    quint16 port{};
    QProcess process;
    QByteArray pending;
    QList<QJsonObject> events;
};

struct ConnectionAttempt {
    bool connected{};
    QStringList messages;
    qint64 elapsedMs{};
    qint64 firstOutputMs{-1};
};

noxshell::ServerProfile fixtureProfile(quint16 port)
{
    noxshell::ServerProfile profile;
    profile.name = QStringLiteral("Local authentication fixture");
    profile.connectionMode = noxshell::ConnectionMode::Ssh;
    profile.authentication = noxshell::AuthenticationMethod::Password;
    profile.host = QStringLiteral("127.0.0.1");
    profile.port = port;
    profile.user = QStringLiteral("fixture-user");
    profile.password = QString::fromLatin1(kSyntheticPassword);
    return profile;
}

void trustLocalFixture(noxshell::Libssh2Worker &worker)
{
    // Trust is limited to this ephemeral loopback fixture, never real servers.
    QObject::connect(&worker, &noxshell::Libssh2Worker::hostKeyVerificationRequired,
        &worker, [&worker](const QString &, const QString &) { worker.approveHostKey(true); });
}

ConnectionAttempt attemptConnection(LocalSshServer &server, int timeoutMs,
    const QString &password = QString::fromLatin1(kSyntheticPassword), int cancelAfterMs = -1)
{
    noxshell::Libssh2Worker worker(nullptr, timeoutMs);
    ConnectionAttempt attempt;
    QElapsedTimer elapsed;
    QObject::connect(&worker, &noxshell::Libssh2Worker::rawOutputReceived,
        &worker, [&](const QByteArray &) {
            if (attempt.firstOutputMs < 0) attempt.firstOutputMs = elapsed.elapsed();
        });
    std::thread cancellation;
    QObject::connect(&worker, &noxshell::Libssh2Worker::connectionChanged,
        &worker, [&](bool connected, const QString &message) {
            attempt.connected = attempt.connected || connected;
            attempt.messages.append(message);
            if (cancelAfterMs >= 0 && !cancellation.joinable()
                && message.startsWith(QStringLiteral("正在查询"))) {
                cancellation = std::thread([&worker, cancelAfterMs] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(cancelAfterMs));
                    worker.cancelConnection();
                });
            }
        });
    // The fixture uses an ephemeral key and listens exclusively on loopback.
    // Approval is confined to this synthetic test; it does not consult known_hosts.
    trustLocalFixture(worker);
    auto profile = fixtureProfile(server.port);
    profile.password = password;
    elapsed.start();
    worker.connectTo(profile);
    attempt.elapsedMs = elapsed.elapsed();
    if (cancellation.joinable()) cancellation.join();
    worker.disconnectFromHost();
    server.readEvents();
    return attempt;
}
} // namespace

class AuthIntegrationTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        const auto server = qEnvironmentVariable("NOXSHELL_AUTH_TEST_SERVER");
        if (server.isEmpty()) QSKIP("Set NOXSHELL_AUTH_TEST_SERVER to the optional local Go SSH fixture executable.");
        QVERIFY2(QFileInfo(server).isExecutable(), qPrintable(QStringLiteral("Fixture is not executable: %1").arg(server)));
    }

    void validCredentials_data()
    {
        QTest::addColumn<QStringList>("arguments");
        QTest::addColumn<QStringList>("expectedMethods");
        QTest::newRow("ordinary-password") << QStringList{"-auth", "password"} << QStringList{"password"};
        QTest::newRow("interactive-password") << QStringList{"-auth", "interactive"} << QStringList{"keyboard-interactive"};
        QTest::newRow("explicit-rejection-allows-interactive-fallback")
            << QStringList{"-auth", "both", "-reject-password"}
            << QStringList{"password", "keyboard-interactive"};
        // This exceeds the original 8-second timeout that triggered the bug.
        QTest::newRow("slow-discovery-over-eight-seconds")
            << QStringList{"-auth", "password", "-none-delay-ms", "9000"}
            << QStringList{"password"};
    }

    void validCredentials()
    {
        QFETCH(QStringList, arguments);
        QFETCH(QStringList, expectedMethods);
        LocalSshServer server;
        QVERIFY2(server.start(arguments), server.error().constData());
        const auto attempt = attemptConnection(server, 30000);
        QVERIFY2(attempt.connected, qPrintable(attempt.messages.join('\n')));
        QCOMPARE(server.startedMethods(), expectedMethods);
    }

    void algorithmCompatibility_data()
    {
        QTest::addColumn<QStringList>("arguments");
        QTest::newRow("ed25519-curve25519-aes128gcm") << QStringList{
            "-host-key", "ed25519", "-kex", "curve25519-sha256", "-cipher", "aes128-gcm@openssh.com"};
        QTest::newRow("ecdsa-p256-aes256ctr") << QStringList{
            "-host-key", "ecdsa", "-kex", "ecdh-sha2-nistp256", "-cipher", "aes256-ctr"};
        QTest::newRow("rsa-sha256-group14-aes128ctr") << QStringList{
            "-host-key", "rsa256", "-kex", "diffie-hellman-group14-sha256", "-cipher", "aes128-ctr"};
        QTest::newRow("rsa-sha512-curve25519-aes256gcm") << QStringList{
            "-host-key", "rsa512", "-kex", "curve25519-sha256", "-cipher", "aes256-gcm@openssh.com"};
    }

    void algorithmCompatibility()
    {
        QFETCH(QStringList, arguments);
        LocalSshServer server;
        QVERIFY2(server.start(arguments), server.error().constData());
        const auto attempt = attemptConnection(server, 5000);
        QVERIFY2(attempt.connected, qPrintable(attempt.messages.join('\n')));
        QVERIFY(attempt.firstOutputMs >= 0);
        qInfo("Local connect: %lld ms; first terminal output: %lld ms", attempt.elapsedMs, attempt.firstOutputMs);
    }

    void hostKeyMismatchNeverSendsCredentials()
    {
        LocalSshServer server;
        QVERIFY2(server.start({}), server.error().constData());
        noxshell::Libssh2Worker worker;
        QSignalSpy changes(&worker, &noxshell::Libssh2Worker::connectionChanged);
        QSignalSpy verification(&worker, &noxshell::Libssh2Worker::hostKeyVerificationRequired);
        auto profile = fixtureProfile(server.port);
        profile.expectedFingerprint = QStringLiteral("SHA256:deliberately-wrong-test-fingerprint");
        worker.connectTo(profile);
        QVERIFY(!changes.isEmpty());
        QVERIFY(!changes.last().at(0).toBool());
        QVERIFY(changes.last().at(1).toString().contains(QStringLiteral("指纹")));
        QCOMPARE(verification.count(), 0);
        QVERIFY(server.startedMethods().isEmpty());
    }

    void reconnectAndEchoWithRekey()
    {
        noxshell::Libssh2Worker worker;
        trustLocalFixture(worker);
        QSignalSpy changes(&worker, &noxshell::Libssh2Worker::connectionChanged);
        QByteArray output;
        QObject::connect(&worker, &noxshell::Libssh2Worker::rawOutputReceived,
            &worker, [&](const QByteArray &chunk) { output.append(chunk); });
        for (int iteration = 0; iteration < 5; ++iteration) {
            LocalSshServer server;
            QVERIFY2(server.start({"-shell-mode", "echo", "-rekey-bytes", "65536"}), server.error().constData());
            worker.connectTo(fixtureProfile(server.port));
            QVERIFY2(changes.last().at(0).toBool(), qPrintable(changes.last().at(1).toString()));
            QTRY_VERIFY_WITH_TIMEOUT(output.contains("fixture-user@localtest:~$ "), 2000);
            output.clear();
            const QByteArray input = QByteArray::number(iteration) + QByteArray(256 * 1024, 'x') + "end-marker\n";
            worker.sendInput(input);
            QTRY_COMPARE_WITH_TIMEOUT(output, input, 5000);
            worker.disconnectFromHost();
            QVERIFY(!changes.last().at(0).toBool());
            output.clear();
        }
    }

    void blockedInput_data()
    {
        QTest::addColumn<bool>("cancel");
        QTest::newRow("bounded-stall-timeout") << false;
        QTest::newRow("cancel-stalled-write") << true;
    }

    void blockedInput()
    {
        QFETCH(bool, cancel);
        LocalSshServer server;
        QVERIFY2(server.start({"-shell-mode", "no-read"}), server.error().constData());
        noxshell::Libssh2Worker worker(nullptr, 5000, cancel ? 8000 : 1000);
        trustLocalFixture(worker);
        QSignalSpy changes(&worker, &noxshell::Libssh2Worker::connectionChanged);
        worker.connectTo(fixtureProfile(server.port));
        QVERIFY(changes.last().at(0).toBool());
        std::thread cancellation;
        if (cancel) {
            cancellation = std::thread([&worker] {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                worker.cancelConnection();
            });
        }
        QElapsedTimer elapsed;
        elapsed.start();
        // Larger than the server's receive window; the server never consumes it.
        worker.sendInput(QByteArray(8 * 1024 * 1024, 'x'));
        const auto duration = elapsed.elapsed();
        if (cancellation.joinable()) cancellation.join();
        QVERIFY(!changes.last().at(0).toBool());
        const auto message = changes.last().at(1).toString();
        QVERIFY2(message.contains(cancel ? QStringLiteral("连接已取消") : QStringLiteral("不会自动重发")), qPrintable(message));
        QVERIFY2(duration < (cancel ? 1500 : 3500), qPrintable(QString::number(duration)));
        if (!cancel) QVERIFY(duration >= 900);
        qInfo("Blocked input %s returned in %lld ms", cancel ? "cancellation" : "timeout", duration);
    }

    void outputFloodHasBoundedBatchesAndFairStderr()
    {
        LocalSshServer server;
        QVERIFY2(server.start({"-shell-mode", "flood"}), server.error().constData());
        noxshell::Libssh2Worker worker;
        trustLocalFixture(worker);
        QSignalSpy changes(&worker, &noxshell::Libssh2Worker::connectionChanged);
        qsizetype bytes = 0;
        bool stderrSeen = false;
        QObject::connect(&worker, &noxshell::Libssh2Worker::rawOutputReceived,
            &worker, [&](const QByteArray &chunk) {
                bytes += chunk.size();
                stderrSeen = stderrSeen || chunk.contains("stderr-fairness-marker");
            });
        worker.connectTo(fixtureProfile(server.port));
        QVERIFY(changes.last().at(0).toBool());
        QTest::qWait(100);
        const auto before = bytes;
        bool received = false;
        QVERIFY(QMetaObject::invokeMethod(&worker, "drainChannel", Qt::DirectConnection, Q_RETURN_ARG(bool, received)));
        QVERIFY(received);
        QVERIFY(bytes - before <= 2 * 64 * 1024);
        QTRY_VERIFY_WITH_TIMEOUT(stderrSeen, 1500);
        bool queuedOperationRan = false;
        QTimer::singleShot(0, &worker, [&] {
            worker.resizePty(100, 30, 0, 0);
            worker.disconnectFromHost();
            queuedOperationRan = true;
        });
        QElapsedTimer elapsed;
        elapsed.start();
        QTRY_VERIFY_WITH_TIMEOUT(queuedOperationRan, 1000);
        QVERIFY(elapsed.elapsed() < 1000);
        QVERIFY(!changes.last().at(0).toBool());
    }

    void finalOutputIsNotTruncatedAtEof_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::newRow("channel-eof") << QStringLiteral("burst");
        QTest::newRow("channel-and-tcp-close") << QStringLiteral("burst-close");
    }

    void finalOutputIsNotTruncatedAtEof()
    {
        QFETCH(QString, mode);
        LocalSshServer server;
        QVERIFY2(server.start({"-shell-mode", mode}), server.error().constData());
        noxshell::Libssh2Worker worker;
        trustLocalFixture(worker);
        QSignalSpy changes(&worker, &noxshell::Libssh2Worker::connectionChanged);
        QByteArray output;
        QObject::connect(&worker, &noxshell::Libssh2Worker::rawOutputReceived,
            &worker, [&](const QByteArray &chunk) { output.append(chunk); });
        worker.connectTo(fixtureProfile(server.port));
        QTRY_VERIFY_WITH_TIMEOUT(!changes.last().at(0).toBool(), 5000);
        const bool hasMarkers = output.contains("stderr-tail-marker\n")
            && output.startsWith("fixture-user@localtest:~$ ");
        const bool hasExpectedSize = output.size()
            == 1024 * 1024 + QByteArray("fixture-user@localtest:~$ stderr-tail-marker\n").size();
        output.remove(0, QByteArray("fixture-user@localtest:~$ ").size());
        output.replace("stderr-tail-marker\n", "");
#if LIBSSH2_VERSION_NUM <= 0x010B01
        // A distinct upstream limitation, not fixed by the bounded-read patch:
        // 1.11.1 channel_read returns transport EOF errors before delivering
        // already buffered packets. Preserve this reproducer as an explicit
        // expected failure; an unexpected pass requires removing the exception.
        QEXPECT_FAIL("channel-and-tcp-close", "Known libssh2 <= 1.11.1 limitation: immediate TCP close can hide buffered final output; not fixed", Continue);
#endif
        QVERIFY(hasMarkers && hasExpectedSize && output == QByteArray(1024 * 1024, 'x'));
    }

    void abruptDisconnectIsReportedAndCanReconnect()
    {
        noxshell::Libssh2Worker worker;
        trustLocalFixture(worker);
        QSignalSpy changes(&worker, &noxshell::Libssh2Worker::connectionChanged);
        LocalSshServer droppingServer;
        QVERIFY2(droppingServer.start({"-shell-mode", "reset"}), droppingServer.error().constData());
        worker.connectTo(fixtureProfile(droppingServer.port));
        QVERIFY(changes.last().at(0).toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!changes.last().at(0).toBool(), 2000);
        LocalSshServer healthyServer;
        QVERIFY2(healthyServer.start({}), healthyServer.error().constData());
        worker.connectTo(fixtureProfile(healthyServer.port));
        QVERIFY2(changes.last().at(0).toBool(), qPrintable(changes.last().at(1).toString()));
        worker.disconnectFromHost();
    }

    void discoveryTimeoutDoesNotAttemptAuthentication()
    {
        LocalSshServer server;
        QVERIFY2(server.start({"-auth", "both", "-none-delay-ms", "3000"}), server.error().constData());
        const auto attempt = attemptConnection(server, 1000);
        const auto messages = attempt.messages.join('\n');
        QVERIFY(!attempt.connected);
        QVERIFY2(messages.contains(QStringLiteral("SSH 认证方式协商")), qPrintable(messages));
        QVERIFY2(messages.contains(QStringLiteral("超时")), qPrintable(messages));
        QVERIFY2(!messages.contains(QStringLiteral("正在使用 password 认证")), qPrintable(messages));
        QVERIFY2(!messages.contains(QStringLiteral("正在使用 keyboard-interactive 认证")), qPrintable(messages));
        QVERIFY2(attempt.elapsedMs < 2500, qPrintable(messages));
        QVERIFY(server.startedMethods().isEmpty());
    }

    void passwordTimeoutDoesNotTryInteractiveAuthentication()
    {
        LocalSshServer server;
        QVERIFY2(server.start({"-auth", "both", "-password-delay-ms", "3000"}), server.error().constData());
        const auto attempt = attemptConnection(server, 1000);
        const auto messages = attempt.messages.join('\n');
        QVERIFY(!attempt.connected);
        QVERIFY2(messages.contains(QStringLiteral("超时")), qPrintable(messages));
        QVERIFY2(!messages.contains(QStringLiteral("正在使用 keyboard-interactive 认证")), qPrintable(messages));
        QVERIFY2(attempt.elapsedMs < 2500, qPrintable(messages));
        QCOMPARE(server.startedMethods(), QStringList{"password"});
    }

    void incorrectPasswordIsReportedAsAuthenticationFailure()
    {
        LocalSshServer server;
        QVERIFY2(server.start({"-auth", "password"}), server.error().constData());
        const auto attempt = attemptConnection(server, 5000, QStringLiteral("deliberately-wrong-test-password"));
        const auto messages = attempt.messages.join('\n');
        QVERIFY(!attempt.connected);
        QVERIFY2(messages.contains(QStringLiteral("SSH 认证失败")), qPrintable(messages));
        QVERIFY2(messages.contains(QStringLiteral("libssh2 -18")), qPrintable(messages));
        QVERIFY2(!messages.contains(QStringLiteral("超时")), qPrintable(messages));
        QCOMPARE(server.startedMethods(), QStringList{"password"});
    }

    void cancelPendingDiscoveryReturnsPromptly()
    {
        LocalSshServer server;
        QVERIFY2(server.start({"-auth", "both", "-none-delay-ms", "3000"}), server.error().constData());
        const auto attempt = attemptConnection(server, 30000, QString::fromLatin1(kSyntheticPassword), 200);
        const auto messages = attempt.messages.join('\n');
        QVERIFY(!attempt.connected);
        QVERIFY2(messages.contains(QStringLiteral("连接已取消")), qPrintable(messages));
        QVERIFY2(attempt.elapsedMs < 1000, qPrintable(QStringLiteral("Cancellation took %1 ms\n%2").arg(attempt.elapsedMs).arg(messages)));
        QVERIFY2(!messages.contains(QStringLiteral("正在使用 password 认证")), qPrintable(messages));
        QVERIFY2(!messages.contains(QStringLiteral("正在使用 keyboard-interactive 认证")), qPrintable(messages));
        QVERIFY(server.startedMethods().isEmpty());
    }

    void staleConnectionGenerationDoesNotOpenSocket()
    {
        QTcpServer listener;
        QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
        noxshell::Libssh2Worker worker;
        const auto queuedGeneration = worker.connectionGeneration();
        worker.cancelConnection();
        QSignalSpy changes(&worker, &noxshell::Libssh2Worker::connectionChanged);
        noxshell::ServerProfile profile;
        profile.host = QStringLiteral("127.0.0.1");
        profile.port = listener.serverPort();
        profile.user = QStringLiteral("fixture-user");
        profile.password = QString::fromLatin1(kSyntheticPassword);
        worker.connectTo(profile, queuedGeneration);
        QCOMPARE(changes.count(), 0);
        QVERIFY(!listener.waitForNewConnection(150));
        QVERIFY(!listener.hasPendingConnections());
    }
};

QTEST_GUILESS_MAIN(AuthIntegrationTest)
#include "AuthIntegrationTest.moc"
