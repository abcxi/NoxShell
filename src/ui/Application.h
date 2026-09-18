#pragma once

#include <QApplication>
#include <QPointer>

namespace noxshell::ui {
class MainWindow;

class Application final : public QApplication {
public:
    Application(int &argc, char **argv);
    void setMainWindow(MainWindow *window);

protected:
    bool event(QEvent *event) override;

private:
    QPointer<MainWindow> m_mainWindow;
    bool m_confirmingQuit{false};
};
} // namespace noxshell::ui
