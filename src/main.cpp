#include "ui/AppTheme.h"
#include "ui/MainWindow.h"
#include "core/AppLogger.h"

#include <QApplication>
#include <QFont>
#include <QIcon>
#include <QTimer>

#ifndef NOXSHELL_APP_VERSION
#define NOXSHELL_APP_VERSION "0.0.0"
#endif

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("玄壳"));
    QApplication::setOrganizationName(QStringLiteral("NoxShell"));
    QApplication::setApplicationVersion(QString::fromLatin1(NOXSHELL_APP_VERSION));
    app.setWindowIcon(QIcon(QStringLiteral(":/assets/app-icon.png")));
    noxshell::AppLogger::install();
    qInfo().noquote() << QStringLiteral("玄壳 %1 启动，日志：%2")
        .arg(QApplication::applicationVersion(), noxshell::AppLogger::logFilePath());
    app.setStyleSheet(noxshell::ui::applicationStyleSheet());

    noxshell::ui::MainWindow window;
    window.show();
    const auto screenshotPath = qEnvironmentVariable("NOXSHELL_SCREENSHOT_PATH");
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
