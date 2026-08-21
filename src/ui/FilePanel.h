#pragma once

#include "../core/RemoteFileEntry.h"
#include "../core/ServerProfile.h"

#include <QFrame>
#include <QHash>
#include <QList>
#include <QPair>
#include <QStringList>

class QLabel;
class QLineEdit;
class QTreeWidget;
class QMenu;
class QToolButton;
class QAction;
class QTreeWidgetItem;
class QEvent;

namespace noxshell {
class SshSession;
}

namespace noxshell::ui {

class RemoteFileEditor;
class TransferQueuePanel;

class FilePanel final : public QFrame {
    Q_OBJECT

public:
    explicit FilePanel(SshSession *session, QWidget *parent = nullptr);

    void setServer(const ServerProfile &profile);
    void updateServer(const ServerProfile &profile);
    void syncDirectoryFromTerminalCommand(const QString &command);
    [[nodiscard]] QString currentPath() const { return m_currentPath; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void navigateTo(const QString &path, bool addToHistory = true);
    void showEntries(const QString &path, const RemoteFileEntries &entries);
    void showError(const QString &path, const QString &message);
    void initializeDirectoryTree();
    void requestDirectoryNode(QTreeWidgetItem *item);
    void populateDirectoryNode(QTreeWidgetItem *item, const RemoteFileEntries &entries);
    void revealDirectoryPath(const QString &path);
    [[nodiscard]] QTreeWidgetItem *directoryItemForPath(const QString &path) const;
    void openInitialDirectory();
    void navigateBack();
    void navigateUp();
    void uploadFile();
    void uploadLocalFiles(const QStringList &localPaths, const QString &remoteDirectory);
    void downloadSelected();
    void createFile();
    void createDirectory();
    void openFile(QTreeWidgetItem *item);
    void renameSelected();
    void finishInlineRename(bool submit);
    void removeSelected();
    void startNextRemoval();
    void finishRemovalBatch();
    void changeSelectedPermissions();
    void updateActionState();
    [[nodiscard]] QTreeWidgetItem *selectedEntry() const;
    [[nodiscard]] QList<QTreeWidgetItem *> selectedEntries() const;
    static QString normalizePath(const QString &path);

    SshSession *m_session{};
    QLabel *m_serverLabel{};
    QLabel *m_statusLabel{};
    QLineEdit *m_pathEdit{};
    QTreeWidget *m_directoryTree{};
    QTreeWidget *m_tree{};
    QToolButton *m_backButton{};
    QToolButton *m_upButton{};
    QToolButton *m_refreshButton{};
    QToolButton *m_transferQueueButton{};
    QMenu *m_contextMenu{};
    QMenu *m_transferQueueMenu{};
    QAction *m_downloadAction{};
    QAction *m_newFileAction{};
    QAction *m_newDirectoryAction{};
    QAction *m_renameAction{};
    QAction *m_removeAction{};
    QAction *m_permissionsAction{};
    RemoteFileEditor *m_fileEditor{};
    TransferQueuePanel *m_transferQueuePanel{};
    ServerProfile m_profile;
    QString m_currentPath{QStringLiteral("/")};
    QString m_homePath{QStringLiteral("/")};
    QString m_pendingPath;
    QString m_directoryTargetPath;
    QHash<QString, QTreeWidgetItem *> m_pendingDirectoryNodes;
    QStringList m_history;
    int m_historyIndex{-1};
    bool m_connected{false};
    bool m_initialDirectoryResolved{false};
    bool m_mutationInFlight{false};
    bool m_inlineRenameActive{false};
    QTreeWidgetItem *m_renamingItem{};
    QLineEdit *m_renameEditor{};
    QString m_renameSourcePath;
    QString m_renameOriginalName;
    QList<QPair<QString, bool>> m_removeQueue;
    int m_removeTotal{};
    int m_removeSucceeded{};
    QStringList m_removeFailures;
    QString m_postRefreshStatus;
};

} // namespace noxshell::ui
