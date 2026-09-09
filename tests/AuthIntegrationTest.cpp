#include "../src/core/Libssh2Worker.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTest>

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
};

ConnectionAttempt attemptConnection(LocalSshServer &server, int timeoutMs,
    const QString &password = QString::fromLatin1(kSyntheticPassword), int cancelAfterMs = -1)
{
    noxshell::Libssh2Worker worker(nullptr, timeoutMs);
    ConnectionAttempt attempt;
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
    QObject::connect(&worker, &noxshell::Libssh2Worker::hostKeyVerificationRequired,
        &worker, [&worker](const QString &, const QString &) { worker.approveHostKey(true); });
    noxshell::ServerProfile profile;
    profile.name = QStringLiteral("Local authentication fixture");
    profile.connectionMode = noxshell::ConnectionMode::Ssh;
    profile.authentication = noxshell::AuthenticationMethod::Password;
    profile.host = QStringLiteral("127.0.0.1");
    profile.port = server.port;
    profile.user = QStringLiteral("fixture-user");
    profile.password = password;
    QElapsedTimer elapsed;
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
