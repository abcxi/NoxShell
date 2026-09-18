#include "Application.h"
#include "MainWindow.h"

#include <QEvent>
#include <QScopedValueRollback>

namespace noxshell::ui {
Application::Application(int &argc, char **argv) : QApplication(argc, argv)
{
    setQuitOnLastWindowClosed(false);
}

void Application::setMainWindow(MainWindow *window)
{
    m_mainWindow = window;
}

bool Application::event(QEvent *event)
{
    if (event->type() != QEvent::Quit || !m_mainWindow) return QApplication::event(event);
    // Cocoa's Dock Quit, application menu and Cmd+Q all reach this event.
    // A second request while a confirmation is open must not stack dialogs.
    if (m_confirmingQuit || activeModalWidget()) {
        event->ignore();
        return true;
    }
    const QScopedValueRollback confirming(m_confirmingQuit, true);
    const QPointer<MainWindow> window = m_mainWindow;
    if (!window->confirmApplicationQuit() || !window) {
        event->ignore();
        return true;
    }
    const bool wasVisible = window->isVisible();
    window->setQuitInProgress(true);
    // Ask all windows to close, preserving unsaved-editor cancellation. Qt's
    // default Quit handler excludes parented dialogs from its veto check;
    // remote editors are parented dialogs, so include those explicitly here.
    closeAllWindows();
    bool vetoed = false;
    for (QWidget *widget : topLevelWidgets()) {
        if (widget->isVisible() && widget->windowType() != Qt::Popup
            && widget->windowType() != Qt::ToolTip) {
            vetoed = true;
            break;
        }
    }
    if (window) {
        window->setQuitInProgress(false);
        if (vetoed && wasVisible) window->show();
    }
    if (vetoed) {
        event->ignore();
        return true;
    }
    return QCoreApplication::event(event);
}
} // namespace noxshell::ui
