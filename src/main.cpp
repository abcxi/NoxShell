#include "ui/AppTheme.h"
#include "ui/MainWindow.h"
#include "core/AppLogger.h"
#include "core/ServerRepository.h"

#include <QApplication>
#include <QFont>
#include <QIcon>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>

#include <memory>
#include <cstdio>

#ifndef NOXSHELL_APP_VERSION
#define NOXSHELL_APP_VERSION "0.0.0"
#endif

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("玄壳"));
    QApplication::setOrganizationName(QStringLiteral("NoxShell"));
    QApplication::setApplicationVersion(QString::fromLatin1(NOXSHELL_APP_VERSION));

    if (app.arguments().contains(QStringLiteral("--startup-smoke-test"))) {
        // Run before installing the file logger or opening the real database.
        // This validates the final distributed app, including Cocoa and SQLite.
        QTemporaryDir testData;
        if (!testData.isValid()) return 2;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, testData.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, testData.path());
        const auto database = testData.filePath(QStringLiteral("startup-test.sqlite3"));
        {
            noxshell::ServerRepository probe(database, false);
            if (!probe.initialize()) return 3;
        }
        noxshell::ui::MainWindow window(database);
        window.show();
        QTimer::singleShot(500, &app, [&app] {
            std::puts("NOXSHELL_STARTUP_SMOKE_OK");
            app.quit();
        });
        return app.exec();
    }

    app.setWindowIcon(QIcon(QStringLiteral(":/assets/app-icon.png")));
    noxshell::AppLogger::install();
    qInfo().noquote() << QStringLiteral("玄壳 %1 启动，日志：%2")
        .arg(QApplication::applicationVersion(), noxshell::AppLogger::logFilePath());
    noxshell::ui::applyApplicationTheme(noxshell::ui::storedThemeMode());

    const auto screenshotPath = qEnvironmentVariable("NOXSHELL_SCREENSHOT_PATH");
    std::unique_ptr<QTemporaryDir> screenshotData;
    QString screenshotDatabase;
    if (!screenshotPath.isEmpty()) {
        screenshotData = std::make_unique<QTemporaryDir>();
        if (screenshotData->isValid()) screenshotDatabase = screenshotData->filePath(QStringLiteral("screenshot.sqlite3"));
    }
    noxshell::ui::MainWindow window(screenshotDatabase);
    window.show();
    if (!screenshotPath.isEmpty()) {
        QTimer::singleShot(1000, &app, [&app, &window, screenshotPath] {
            const auto saved = window.grab().save(screenshotPath);
            if (saved) qInfo().noquote() << QStringLiteral("界面效果图已保存：%1").arg(screenshotPath);
            else qCritical().noquote() << QStringLiteral("界面效果图保存失败：%1").arg(screenshotPath);
            app.exit(saved ? 0 : 1);
        });
    }
    return app.exec();
}
