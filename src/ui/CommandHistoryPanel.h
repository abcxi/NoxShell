#pragma once

#include <QFrame>

class QAction;
class QMenu;
class QTabBar;
class QToolButton;
class QTreeWidget;

namespace noxshell {
class ServerRepository;
}

namespace noxshell::ui {

class CommandHistoryPanel final : public QFrame {
    Q_OBJECT

public:
    explicit CommandHistoryPanel(ServerRepository *repository, QWidget *parent = nullptr);

    void setServerId(const QString &serverId);
    void recordCommand(const QString &command);
    void refresh();

signals:
    void commandSelected(const QString &command);
    void commandExecutionRequested(const QString &command);
    void closeRequested();

private:
    Q_INVOKABLE bool prepareItemActions(int row);
    void editCurrentNote();
    void clearCurrentView();
    [[nodiscard]] qint64 currentEntryId() const;
    [[nodiscard]] QString currentCommand() const;
    [[nodiscard]] bool currentFavorite() const;

    ServerRepository *m_repository{};
    QString m_serverId;
    QTabBar *m_tabs{};
    QTreeWidget *m_tree{};
    QToolButton *m_clearButton{};
    QMenu *m_menu{};
    QAction *m_executeAction{};
    QAction *m_favoriteAction{};
    QAction *m_noteAction{};
    QAction *m_deleteAction{};
};

} // namespace noxshell::ui
