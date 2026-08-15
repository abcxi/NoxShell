#include "FilePanel.h"

#include "../core/SshSession.h"
#include "RemoteFileEditor.h"
#include "TransferQueuePanel.h"

#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QProcess>
#include <QStandardPaths>
#include <QSplitter>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <QUrl>

#include <algorithm>

namespace noxshell::ui {

namespace {
constexpr int kPathRole = Qt::UserRole;
constexpr int kDirectoryRole = Qt::UserRole + 1;
constexpr int kDirectoryLoadedRole = Qt::UserRole + 2;
constexpr int kPlaceholderRole = Qt::UserRole + 3;

QString permissionText(quint32 mode)
{
    if (mode == 0) return QStringLiteral("—");
    QString result;
    const quint32 type = mode & 0170000;
    result += type == 0040000 ? QLatin1Char('d') : type == 0120000 ? QLatin1Char('l') : QLatin1Char('-');
    const auto triplet = [&result, mode](quint32 read, quint32 write, quint32 execute, quint32 special,
                             QChar specialOn, QChar specialOff) {
        result += mode & read ? QLatin1Char('r') : QLatin1Char('-');
        result += mode & write ? QLatin1Char('w') : QLatin1Char('-');
        result += mode & special ? (mode & execute ? specialOn : specialOff)
                                 : (mode & execute ? QLatin1Char('x') : QLatin1Char('-'));
    };
    triplet(0400, 0200, 0100, 04000, QLatin1Char('s'), QLatin1Char('S'));
    triplet(0040, 0020, 0010, 02000, QLatin1Char('s'), QLatin1Char('S'));
    triplet(0004, 0002, 0001, 01000, QLatin1Char('t'), QLatin1Char('T'));
    return result;
}

QString ownerGroupText(const RemoteFileEntry &entry)
{
    const auto owner = entry.owner.isEmpty() && entry.ownerIdsValid ? QString::number(entry.userId) : entry.owner;
    const auto group = entry.group.isEmpty() && entry.ownerIdsValid ? QString::number(entry.groupId) : entry.group;
    if (owner.isEmpty() && group.isEmpty()) return QStringLiteral("—");
    return QStringLiteral("%1/%2").arg(owner.isEmpty() ? QStringLiteral("—") : owner,
        group.isEmpty() ? QStringLiteral("—") : group);
}

QString formatSize(quint64 bytes)
{
    if (bytes < 1024) return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024) return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    if (bytes < 1024ULL * 1024 * 1024) return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1);
}
} // namespace

FilePanel::FilePanel(SshSession *session, QWidget *parent)
    : QFrame(parent)
    , m_session(session)
{
    setObjectName(QStringLiteral("filePanel"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *header = new QWidget;
    header->setObjectName(QStringLiteral("fileToolbar"));
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(11, 0, 9, 0);
    headerLayout->setSpacing(6);
    auto *title = new QLabel(QStringLiteral("文件管理"));
    title->setStyleSheet(QStringLiteral("font-weight:650;"));
    m_serverLabel = new QLabel(QStringLiteral("SFTP"));
    m_serverLabel->setObjectName(QStringLiteral("fileServerLabel"));
    m_serverLabel->setStyleSheet(QStringLiteral("color:#738297;font-size:12px;"));
    m_moreButton = new QToolButton;
    m_transferQueueButton = new QToolButton;
    m_moreButton->setObjectName(QStringLiteral("fileMoreButton"));
    m_transferQueueButton->setObjectName(QStringLiteral("transferQueueButton"));
    auto *moreMenu = new QMenu(m_moreButton);
    m_newFileAction = moreMenu->addAction(QStringLiteral("新建文件"));
    m_newFileAction->setObjectName(QStringLiteral("fileNewFileAction"));
    m_newDirectoryAction = moreMenu->addAction(QStringLiteral("新建目录"));
    m_newDirectoryAction->setObjectName(QStringLiteral("fileNewDirectoryAction"));
    moreMenu->addSeparator();
    m_downloadAction = moreMenu->addAction(QStringLiteral("下载所选文件"));
    m_downloadAction->setObjectName(QStringLiteral("fileContextDownloadAction"));
    moreMenu->addSeparator();
    m_renameAction = moreMenu->addAction(QStringLiteral("重命名"));
    m_removeAction = moreMenu->addAction(QStringLiteral("删除"));
    m_renameAction->setObjectName(QStringLiteral("fileRenameAction"));
    m_removeAction->setObjectName(QStringLiteral("fileRemoveAction"));
    m_moreButton->setText(QStringLiteral("⋯"));
    m_moreButton->setMenu(moreMenu);
    m_moreButton->setPopupMode(QToolButton::InstantPopup);
    m_moreButton->setAutoRaise(true);
    m_moreButton->setFixedSize(28, 28);
    m_transferQueueButton->setIcon(QIcon(QStringLiteral(":/assets/transfer-queue.svg")));
    m_transferQueueButton->setIconSize(QSize(18, 18));
    m_transferQueueButton->setToolTip(QStringLiteral("传输队列 · 空闲"));
    m_transferQueueButton->setAccessibleName(QStringLiteral("打开传输队列"));
    m_transferQueueButton->setAutoRaise(true);
    m_transferQueueButton->setFixedSize(28, 28);
    m_transferQueueButton->setStyleSheet(QStringLiteral(
        "QToolButton{border:1px solid transparent;border-radius:4px;padding:3px;}"
        "QToolButton:hover,QToolButton::menu-button:hover{background:#F0F5FA;border-color:#D5DFEA;}"
        "QToolButton[active=\"true\"]{background:#E8F3FF;border-color:#8BBFFF;}"
        "QToolButton::menu-indicator{image:none;}"));

    auto *queueMenu = new QMenu(m_transferQueueButton);
    queueMenu->setObjectName(QStringLiteral("transferQueueMenu"));
    queueMenu->setStyleSheet(QStringLiteral("QMenu{padding:0;border:1px solid #CCD7E3;border-radius:5px;background:#FBFCFD;}"));
    auto *queueWidgetAction = new QWidgetAction(queueMenu);
    m_transferQueuePanel = new TransferQueuePanel(m_session);
    m_transferQueuePanel->setProperty("popup", true);
    m_transferQueuePanel->setFixedWidth(500);
    queueWidgetAction->setDefaultWidget(m_transferQueuePanel);
    queueMenu->addAction(queueWidgetAction);
    m_transferQueueButton->setMenu(queueMenu);
    m_transferQueueButton->setPopupMode(QToolButton::InstantPopup);

    m_statusLabel = new QLabel(QStringLiteral("  等待 SSH 连接"));
    m_statusLabel->setObjectName(QStringLiteral("fileStatusLabel"));
    m_statusLabel->setMinimumWidth(0);
    m_statusLabel->setMaximumWidth(170);
    m_statusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_statusLabel->setStyleSheet(QStringLiteral("color:#738297;font-size:11px;"));

    m_backButton = new QPushButton(QStringLiteral("‹"));
    m_upButton = new QPushButton(QStringLiteral("↑"));
    m_refreshButton = new QPushButton(QStringLiteral("↻"));
    m_backButton->setObjectName(QStringLiteral("fileBackButton"));
    m_upButton->setObjectName(QStringLiteral("fileUpButton"));
    m_refreshButton->setObjectName(QStringLiteral("fileRefreshButton"));
    for (auto *button : {m_backButton, m_upButton, m_refreshButton}) {
        button->setFixedSize(28, 28);
        button->setStyleSheet(QStringLiteral("padding:0;"));
    }
    m_pathEdit = new QLineEdit(QStringLiteral("/"));
    m_pathEdit->setObjectName(QStringLiteral("remotePathEdit"));
    m_pathEdit->setMinimumWidth(120);
    m_pathEdit->setFixedHeight(28);

    headerLayout->addWidget(title);
    headerLayout->addWidget(m_serverLabel);
    headerLayout->addWidget(m_statusLabel);
    headerLayout->addWidget(m_backButton);
    headerLayout->addWidget(m_upButton);
    headerLayout->addWidget(m_pathEdit, 1);
    headerLayout->addWidget(m_refreshButton);
    headerLayout->addWidget(m_transferQueueButton);
    headerLayout->addWidget(m_moreButton);

    m_tree = new QTreeWidget;
    m_tree->setObjectName(QStringLiteral("remoteFileTree"));
    m_tree->setColumnCount(6);
    m_tree->setHeaderLabels({QStringLiteral("文件名"), QStringLiteral("大小"), QStringLiteral("类型"),
        QStringLiteral("修改时间"), QStringLiteral("权限"), QStringLiteral("用户/用户组")});
    m_tree->setRootIsDecorated(false);
    m_tree->setAlternatingRowColors(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setAcceptDrops(true);
    m_tree->viewport()->setAcceptDrops(true);
    m_tree->viewport()->installEventFilter(this);
    m_tree->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setMinimumSectionSize(64);
    for (int column = 0; column < m_tree->columnCount(); ++column) {
        m_tree->header()->setSectionResizeMode(column, QHeaderView::Interactive);
    }
    const int widths[] = {170, 78, 76, 132, 102, 112};
    for (int column = 0; column < m_tree->columnCount(); ++column) m_tree->setColumnWidth(column, widths[column]);

    m_directoryTree = new QTreeWidget;
    m_directoryTree->setObjectName(QStringLiteral("remoteDirectoryTree"));
    m_directoryTree->setColumnCount(1);
    m_directoryTree->setHeaderHidden(true);
    m_directoryTree->setRootIsDecorated(true);
    m_directoryTree->setUniformRowHeights(true);
    m_directoryTree->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_directoryTree->setMinimumWidth(150);

    auto *browserSplitter = new QSplitter(Qt::Horizontal);
    browserSplitter->setObjectName(QStringLiteral("fileBrowserSplitter"));
    browserSplitter->setChildrenCollapsible(false);
    browserSplitter->addWidget(m_directoryTree);
    browserSplitter->addWidget(m_tree);
    browserSplitter->setStretchFactor(0, 0);
    browserSplitter->setStretchFactor(1, 1);
    browserSplitter->setSizes({205, 760});

    header->setFixedHeight(40);
    header->setStyleSheet(QStringLiteral(
        "QWidget#fileToolbar{border-bottom:1px solid #E5EAF0;background:#FBFCFD;}"));
    layout->addWidget(header);
    layout->addWidget(browserSplitter, 1);

    connect(m_transferQueuePanel, &TransferQueuePanel::summaryChanged, this,
        [this](int activeCount, int totalCount, const QString &summary) {
            m_transferQueueButton->setProperty("active", activeCount > 0);
            m_transferQueueButton->setToolTip(QStringLiteral("传输队列 · %1 · 共 %2 项").arg(summary).arg(totalCount));
            m_transferQueueButton->style()->unpolish(m_transferQueueButton);
            m_transferQueueButton->style()->polish(m_transferQueueButton);
        });

    connect(m_refreshButton, &QPushButton::clicked, this, [this] { navigateTo(m_currentPath, false); });
    connect(m_pathEdit, &QLineEdit::returnPressed, this, [this] { navigateTo(m_pathEdit->text()); });
    connect(m_upButton, &QPushButton::clicked, this, &FilePanel::navigateUp);
    connect(m_backButton, &QPushButton::clicked, this, &FilePanel::navigateBack);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        if (item->data(0, kDirectoryRole).toBool()) navigateTo(item->data(0, kPathRole).toString());
        else openFile(item);
    });
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &FilePanel::updateActionState);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        if (auto *item = m_tree->itemAt(position); item && !item->isSelected()) {
            m_tree->clearSelection();
            item->setSelected(true);
            m_tree->setCurrentItem(item);
        } else if (!item) {
            m_tree->clearSelection();
        }
        updateActionState();
        if (m_connected) m_moreButton->menu()->exec(m_tree->viewport()->mapToGlobal(position));
    });
    connect(m_directoryTree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
        requestDirectoryNode(item);
    });
    connect(m_directoryTree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
        if (!item || item->data(0, kPlaceholderRole).toBool()) return;
        item->setExpanded(true);
        requestDirectoryNode(item);
        navigateTo(item->data(0, kPathRole).toString());
    });
    connect(m_downloadAction, &QAction::triggered, this, &FilePanel::downloadSelected);
    connect(m_newFileAction, &QAction::triggered, this, &FilePanel::createFile);
    connect(m_newDirectoryAction, &QAction::triggered, this, &FilePanel::createDirectory);
    connect(m_renameAction, &QAction::triggered, this, &FilePanel::renameSelected);
    connect(m_removeAction, &QAction::triggered, this, &FilePanel::removeSelected);
    connect(m_session, &SshSession::directoryListed, this, &FilePanel::showEntries);
    connect(m_session, &SshSession::directoryListingFailed, this, &FilePanel::showError);
    connect(m_session, &SshSession::homeDirectoryResolved, this, [this](const QString &path) {
        m_initialDirectoryResolved = true;
        m_homePath = normalizePath(path);
        m_currentPath = m_homePath;
        m_history = {m_currentPath};
        m_historyIndex = 0;
        navigateTo(m_currentPath, false);
    });
    connect(m_session, &SshSession::homeDirectoryResolutionFailed, this, [this](const QString &message) {
        m_initialDirectoryResolved = true;
        m_statusLabel->setText(QStringLiteral("  无法定位主目录，已回退到 / · %1").arg(message));
        navigateTo(QStringLiteral("/"), false);
    });
    connect(m_session, &SshSession::fileOperationProgress, this,
        [this](RemoteFileOperation operation, const QString &path, quint64 completed, quint64 total) {
            Q_UNUSED(operation);
            const int percent = total > 0 ? qRound(completed * 100.0 / total) : 0;
            m_statusLabel->setText(total > 0
                    ? QStringLiteral("  传输中 %1%  ·  %2").arg(percent).arg(QFileInfo(path).fileName())
                    : QStringLiteral("  传输中 %1  ·  %2").arg(formatSize(completed), QFileInfo(path).fileName()));
        });
    connect(m_session, &SshSession::fileOperationFinished, this,
        [this](RemoteFileOperation operation, const QString &path) {
            const auto names = QStringList{QStringLiteral("上传"), QStringLiteral("下载"), QStringLiteral("新建目录"), QStringLiteral("重命名"), QStringLiteral("删除")};
            m_statusLabel->setText(QStringLiteral("  %1完成  ·  %2").arg(names.value(static_cast<int>(operation)), QFileInfo(path).fileName()));
            if (operation == RemoteFileOperation::CreateDirectory || operation == RemoteFileOperation::Rename
                || operation == RemoteFileOperation::Remove) {
                m_mutationInFlight = false;
                if (auto *directoryNode = directoryItemForPath(m_currentPath)) {
                    directoryNode->setData(0, kDirectoryLoadedRole, false);
                    requestDirectoryNode(directoryNode);
                }
            }
            if (operation != RemoteFileOperation::Download) navigateTo(m_currentPath, false);
            updateActionState();
        });
    connect(m_session, &SshSession::fileOperationFailed, this,
        [this](RemoteFileOperation operation, const QString &, const QString &message) {
            if (operation == RemoteFileOperation::CreateDirectory || operation == RemoteFileOperation::Rename
                || operation == RemoteFileOperation::Remove) {
                m_mutationInFlight = false;
            }
            m_statusLabel->setText(QStringLiteral("  操作失败 · %1").arg(message));
            if (operation == RemoteFileOperation::Rename) navigateTo(m_currentPath, false);
            updateActionState();
        });
    connect(m_session, &SshSession::remoteFileWritten, this,
        [this](quint64, const QString &path) {
            m_mutationInFlight = false;
            m_statusLabel->setText(QStringLiteral("  文件已保存  ·  %1").arg(QFileInfo(path).fileName()));
            if (QFileInfo(path).path() == m_currentPath) {
                if (auto *directoryNode = directoryItemForPath(m_currentPath)) {
                    directoryNode->setData(0, kDirectoryLoadedRole, false);
                    requestDirectoryNode(directoryNode);
                }
                navigateTo(m_currentPath, false);
            }
            updateActionState();
        });
    connect(m_session, &SshSession::remoteFileWriteFailed, this,
        [this](quint64, const QString &, const QString &message) {
            m_mutationInFlight = false;
            m_statusLabel->setText(QStringLiteral("  保存文件失败 · %1").arg(message));
            updateActionState();
        });
    connect(m_session, &SshSession::connectionChanged, this, [this](bool connected, const QString &message) {
        m_connected = connected;
        m_directoryTree->setEnabled(connected);
        updateActionState();
        if (connected) {
            openInitialDirectory();
        } else {
            m_statusLabel->setText(QStringLiteral("  %1").arg(message));
        }
    });
    m_backButton->setEnabled(false);
    updateActionState();
}

void FilePanel::setServer(const ServerProfile &profile)
{
    finishInlineRename(false);
    m_serverLabel->setText(QStringLiteral("SFTP · %1").arg(profile.name));
    m_profile = profile;
    m_connected = m_session->isConnected() && m_session->profile().id == profile.id;
    m_currentPath = QStringLiteral("/");
    m_homePath = QStringLiteral("/");
    m_initialDirectoryResolved = false;
    m_pendingPath.clear();
    m_directoryTargetPath.clear();
    m_pendingDirectoryNodes.clear();
    m_mutationInFlight = false;
    m_history = {m_currentPath};
    m_historyIndex = 0;
    m_tree->clear();
    m_directoryTree->clear();
    m_directoryTree->setEnabled(false);
    m_pathEdit->setText(m_currentPath);
    m_backButton->setEnabled(false);
    m_statusLabel->setText(QStringLiteral("  等待 %1 的 SFTP 会话").arg(profile.name));
    updateActionState();
    if (m_session->isConnected() && m_session->profile().id == profile.id) openInitialDirectory();
}

void FilePanel::updateServer(const ServerProfile &profile)
{
    if (profile.id.isEmpty() || profile.id != m_profile.id) {
        setServer(profile);
        return;
    }
    m_profile = profile;
    m_serverLabel->setText(QStringLiteral("SFTP · %1").arg(profile.name));
    m_connected = m_session->isConnected() && m_session->profile().id == profile.id;
    m_directoryTree->setEnabled(m_connected);
    updateActionState();
}

void FilePanel::syncDirectoryFromTerminalCommand(const QString &command)
{
    if (!m_connected || m_profile.id.isEmpty() || m_session->profile().id != m_profile.id) return;
    auto trimmed = command.trimmed();
    if (trimmed.isEmpty() || trimmed.contains(QLatin1Char('\n')) || trimmed.contains(QLatin1Char(';'))
        || trimmed.contains(QStringLiteral("&&")) || trimmed.contains(QStringLiteral("||"))
        || trimmed.contains(QLatin1Char('|'))) return;
    auto arguments = QProcess::splitCommand(trimmed);
    if (arguments.isEmpty() || arguments.takeFirst() != QStringLiteral("cd")) return;
    if (!arguments.isEmpty() && arguments.first() == QStringLiteral("--")) arguments.removeFirst();
    if (arguments.size() > 1) return;

    const auto destination = arguments.value(0);
    QString path;
    if (destination.isEmpty() || destination == QStringLiteral("~") || destination == QStringLiteral("$HOME")) {
        path = m_homePath;
    } else if (destination.startsWith(QStringLiteral("~/"))) {
        path = m_homePath + destination.mid(1);
    } else if (destination.startsWith(QStringLiteral("$HOME/"))) {
        path = m_homePath + destination.mid(5);
    } else if (destination.startsWith(QLatin1Char('/'))) {
        path = destination;
    } else if (destination == QStringLiteral("-")) {
        if (m_historyIndex <= 0) return;
        path = m_history.at(m_historyIndex - 1);
    } else {
        path = m_currentPath + QLatin1Char('/') + destination;
    }
    navigateTo(normalizePath(path));
}

void FilePanel::openInitialDirectory()
{
    if (!m_connected) return;
    initializeDirectoryTree();
    if (m_initialDirectoryResolved) {
        navigateTo(m_currentPath, false);
        return;
    }
    m_tree->clear();
    m_tree->setEnabled(false);
    m_statusLabel->setText(QStringLiteral("  正在定位远端主目录…"));
    m_session->requestHomeDirectory();
}

void FilePanel::navigateTo(const QString &path, bool addToHistory)
{
    finishInlineRename(false);
    const auto normalized = normalizePath(path);
    if (addToHistory && (m_historyIndex < 0 || m_history.value(m_historyIndex) != normalized)) {
        while (m_history.size() > m_historyIndex + 1) m_history.removeLast();
        m_history.append(normalized);
        m_historyIndex = m_history.size() - 1;
    }
    m_currentPath = normalized;
    m_directoryTargetPath = normalized;
    m_pendingPath = normalized;
    m_pathEdit->setText(normalized);
    m_backButton->setEnabled(m_historyIndex > 0);
    m_upButton->setEnabled(normalized != QStringLiteral("/"));
    m_tree->clear();
    m_tree->setEnabled(false);
    m_statusLabel->setText(QStringLiteral("  正在读取 %1 …").arg(normalized));
    updateActionState();
    revealDirectoryPath(normalized);
    m_session->listDirectory(normalized);
}

void FilePanel::showEntries(const QString &path, const RemoteFileEntries &entries)
{
    if (auto *directoryNode = m_pendingDirectoryNodes.take(path)) {
        populateDirectoryNode(directoryNode, entries);
    }
    if (path != m_pendingPath) return;
    m_pendingPath.clear();
    m_tree->clear();
    m_tree->setEnabled(true);
    for (const auto &entry : entries) {
        const auto size = entry.directory ? QStringLiteral("—") : formatSize(entry.size);
        const auto type = entry.directory ? QStringLiteral("文件夹") : entry.symbolicLink ? QStringLiteral("符号链接") : QStringLiteral("文件");
        const auto modified = entry.modifiedAt.isValid() ? entry.modifiedAt.toString(QStringLiteral("yyyy/MM/dd HH:mm")) : QStringLiteral("—");
        auto *item = new QTreeWidgetItem(m_tree,
            {entry.name, size, type, modified, permissionText(entry.permissions), ownerGroupText(entry)});
        item->setIcon(0, style()->standardIcon(entry.directory ? QStyle::SP_DirIcon
                                                               : entry.symbolicLink ? QStyle::SP_FileLinkIcon : QStyle::SP_FileIcon));
        item->setData(0, kPathRole, entry.path);
        item->setData(0, kDirectoryRole, entry.directory);
        item->setToolTip(0, entry.path);
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
    }
    m_statusLabel->setText(QStringLiteral("  %1 个项目  ·  %2").arg(entries.size()).arg(path));
    revealDirectoryPath(path);
    updateActionState();
}

void FilePanel::showError(const QString &path, const QString &message)
{
    if (auto *directoryNode = m_pendingDirectoryNodes.take(path)) {
        qDeleteAll(directoryNode->takeChildren());
        directoryNode->setData(0, kDirectoryLoadedRole, true);
        auto *errorItem = new QTreeWidgetItem(directoryNode, {QStringLiteral("(无法读取)")});
        errorItem->setData(0, kPlaceholderRole, true);
        errorItem->setForeground(0, QColor(QStringLiteral("#D54941")));
        errorItem->setFlags(Qt::NoItemFlags);
    }
    if (path != m_pendingPath) return;
    m_pendingPath.clear();
    m_tree->clear();
    m_tree->setEnabled(true);
    auto *item = new QTreeWidgetItem(m_tree, {QStringLiteral("无法读取目录"), QStringLiteral("—"), QStringLiteral("—"),
        QStringLiteral("—"), QStringLiteral("—"), QStringLiteral("—")});
    item->setForeground(0, QColor(QStringLiteral("#D54941")));
    m_statusLabel->setText(QStringLiteral("  SFTP 错误 · %1").arg(message));
    updateActionState();
}

void FilePanel::initializeDirectoryTree()
{
    m_pendingDirectoryNodes.clear();
    m_directoryTree->clear();
    m_directoryTree->setEnabled(m_connected);
    auto *root = new QTreeWidgetItem(m_directoryTree, {QStringLiteral("/")});
    root->setIcon(0, style()->standardIcon(QStyle::SP_DriveHDIcon));
    root->setData(0, kPathRole, QStringLiteral("/"));
    root->setData(0, kDirectoryRole, true);
    root->setData(0, kDirectoryLoadedRole, false);
    auto *placeholder = new QTreeWidgetItem(root, {QStringLiteral("加载中…")});
    placeholder->setData(0, kPlaceholderRole, true);
    placeholder->setFlags(Qt::NoItemFlags);
    root->setExpanded(true);
    requestDirectoryNode(root);
}

void FilePanel::requestDirectoryNode(QTreeWidgetItem *item)
{
    if (!m_connected || !item || item->data(0, kPlaceholderRole).toBool()
        || item->data(0, kDirectoryLoadedRole).toBool()) return;
    const auto path = item->data(0, kPathRole).toString();
    if (path.isEmpty() || m_pendingDirectoryNodes.contains(path)) return;
    qDeleteAll(item->takeChildren());
    auto *placeholder = new QTreeWidgetItem(item, {QStringLiteral("正在读取…")});
    placeholder->setData(0, kPlaceholderRole, true);
    placeholder->setFlags(Qt::NoItemFlags);
    m_pendingDirectoryNodes.insert(path, item);
    m_session->listDirectory(path);
}

void FilePanel::populateDirectoryNode(QTreeWidgetItem *item, const RemoteFileEntries &entries)
{
    if (!item) return;
    qDeleteAll(item->takeChildren());
    for (const auto &entry : entries) {
        if (!entry.directory) continue;
        auto *child = new QTreeWidgetItem(item, {entry.name});
        child->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
        child->setData(0, kPathRole, entry.path);
        child->setData(0, kDirectoryRole, true);
        child->setData(0, kDirectoryLoadedRole, false);
        child->setToolTip(0, entry.path);
        auto *placeholder = new QTreeWidgetItem(child, {QStringLiteral("加载中…")});
        placeholder->setData(0, kPlaceholderRole, true);
        placeholder->setFlags(Qt::NoItemFlags);
    }
    item->setData(0, kDirectoryLoadedRole, true);
    revealDirectoryPath(m_directoryTargetPath);
}

void FilePanel::revealDirectoryPath(const QString &path)
{
    if (!m_directoryTree || m_directoryTree->topLevelItemCount() == 0) return;
    const auto normalized = normalizePath(path);
    m_directoryTargetPath = normalized;
    auto *current = m_directoryTree->topLevelItem(0);
    current->setExpanded(true);
    if (normalized == QStringLiteral("/")) {
        m_directoryTree->setCurrentItem(current);
        m_directoryTree->scrollToItem(current);
        return;
    }

    const auto components = normalized.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString assembled;
    for (qsizetype index = 0; index < components.size(); ++index) {
        if (!current->data(0, kDirectoryLoadedRole).toBool()) {
            requestDirectoryNode(current);
            return;
        }
        assembled += QLatin1Char('/') + components.at(index);
        QTreeWidgetItem *next = nullptr;
        for (int childIndex = 0; childIndex < current->childCount(); ++childIndex) {
            auto *child = current->child(childIndex);
            if (child->data(0, kPathRole).toString() == assembled) {
                next = child;
                break;
            }
        }
        if (!next) return;
        current = next;
        if (index + 1 < components.size()) current->setExpanded(true);
    }
    m_directoryTree->setCurrentItem(current);
    m_directoryTree->scrollToItem(current);
}

QTreeWidgetItem *FilePanel::directoryItemForPath(const QString &path) const
{
    if (!m_directoryTree) return nullptr;
    QTreeWidgetItemIterator iterator(m_directoryTree);
    while (*iterator) {
        if ((*iterator)->data(0, kPathRole).toString() == path) return *iterator;
        ++iterator;
    }
    return nullptr;
}

QTreeWidgetItem *FilePanel::selectedEntry() const
{
    const auto selected = m_tree->selectedItems();
    return selected.isEmpty() ? nullptr : selected.first();
}

QList<QTreeWidgetItem *> FilePanel::selectedEntries() const
{
    return m_tree ? m_tree->selectedItems() : QList<QTreeWidgetItem *>{};
}

void FilePanel::updateActionState()
{
    const auto items = selectedEntries();
    const bool canMutate = m_connected && !m_mutationInFlight && !m_inlineRenameActive;
    const bool hasSelection = canMutate && !items.isEmpty();
    const int fileCount = std::count_if(items.cbegin(), items.cend(), [](const QTreeWidgetItem *item) {
        return !item->data(0, kDirectoryRole).toBool();
    });
    const bool singleSelection = items.size() == 1;
    m_newFileAction->setEnabled(canMutate);
    m_newDirectoryAction->setEnabled(canMutate);
    m_downloadAction->setEnabled(m_connected && fileCount > 0);
    m_moreButton->setEnabled(m_connected && m_tree->isEnabled());
    m_renameAction->setEnabled(hasSelection && singleSelection);
    m_removeAction->setEnabled(hasSelection && singleSelection);
}

void FilePanel::uploadFile()
{
    const auto localPaths = QFileDialog::getOpenFileNames(this, QStringLiteral("选择要上传的文件"));
    uploadLocalFiles(localPaths, m_currentPath);
}

void FilePanel::uploadLocalFiles(const QStringList &localPaths, const QString &remoteDirectory)
{
    if (!m_connected || localPaths.isEmpty()) return;
    int queued = 0;
    int skipped = 0;
    const auto destination = normalizePath(remoteDirectory);
    for (const auto &localPath : localPaths) {
        const QFileInfo source(localPath);
        if (!source.isFile()) {
            ++skipped;
            continue;
        }
        const auto remotePath = destination == QStringLiteral("/")
            ? QStringLiteral("/") + source.fileName()
            : destination + QLatin1Char('/') + source.fileName();
        m_session->uploadFile(source.absoluteFilePath(), remotePath);
        ++queued;
    }
    if (queued > 0) {
        m_statusLabel->setText(QStringLiteral("  已加入 %1 个上传任务%2")
                .arg(queued)
                .arg(skipped > 0 ? QStringLiteral("，已跳过 %1 个目录或无效项").arg(skipped) : QString{}));
    } else if (skipped > 0) {
        m_statusLabel->setText(QStringLiteral("  拖拽上传暂不支持本地目录，请选择文件。"));
    }
}

void FilePanel::downloadSelected()
{
    QStringList remotePaths;
    for (auto *item : selectedEntries()) {
        if (!item->data(0, kDirectoryRole).toBool()) remotePaths.append(item->data(0, kPathRole).toString());
    }
    if (remotePaths.isEmpty()) return;

    const auto downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (remotePaths.size() == 1) {
        const auto remotePath = remotePaths.first();
        const auto suggested = QDir(downloads).filePath(QFileInfo(remotePath).fileName());
        const auto localPath = QFileDialog::getSaveFileName(this, QStringLiteral("保存远端文件"), suggested);
        if (localPath.isEmpty()) return;
        m_statusLabel->setText(QStringLiteral("  准备下载 · %1").arg(QFileInfo(remotePath).fileName()));
        m_session->downloadFile(remotePath, localPath);
        return;
    }

    const auto localDirectory = QFileDialog::getExistingDirectory(this, QStringLiteral("选择批量下载目录"), downloads);
    if (localDirectory.isEmpty()) return;
    for (const auto &remotePath : remotePaths) {
        m_session->downloadFile(remotePath, QDir(localDirectory).filePath(QFileInfo(remotePath).fileName()));
    }
    m_statusLabel->setText(QStringLiteral("  已加入 %1 个下载任务").arg(remotePaths.size()));
}

void FilePanel::openFile(QTreeWidgetItem *item)
{
    if (!m_connected || !item || item->data(0, kDirectoryRole).toBool()) return;
    const auto path = item->data(0, kPathRole).toString();
    if (path.isEmpty()) return;
    if (!m_fileEditor) {
        m_fileEditor = new RemoteFileEditor(m_session, m_profile.name, path, this);
        connect(m_fileEditor, &QObject::destroyed, this, [this] { m_fileEditor = nullptr; });
    } else {
        m_fileEditor->openFile(path);
    }
    m_fileEditor->show();
    m_fileEditor->raise();
    m_fileEditor->activateWindow();
}

void FilePanel::createFile()
{
    if (!m_connected || m_mutationInFlight || m_inlineRenameActive) return;
    bool accepted = false;
    const auto name = QInputDialog::getText(this, QStringLiteral("新建文件"), QStringLiteral("文件名称"),
        QLineEdit::Normal, {}, &accepted).trimmed();
    if (!accepted || name.isEmpty()) return;
    if (name.contains(QLatin1Char('/')) || name == QStringLiteral(".") || name == QStringLiteral("..")) {
        QMessageBox::warning(this, QStringLiteral("文件名称无效"), QStringLiteral("文件名称不能包含 /，也不能是 . 或 ..。"));
        return;
    }
    const auto path = m_currentPath == QStringLiteral("/") ? QStringLiteral("/") + name : m_currentPath + QLatin1Char('/') + name;
    m_mutationInFlight = true;
    m_statusLabel->setText(QStringLiteral("  正在新建文件 · %1").arg(name));
    updateActionState();
    m_session->writeFile(path, {}, false);
}

void FilePanel::createDirectory()
{
    if (m_mutationInFlight || m_inlineRenameActive) return;
    bool accepted = false;
    const auto name = QInputDialog::getText(this, QStringLiteral("新建目录"), QStringLiteral("目录名称"), QLineEdit::Normal, {}, &accepted).trimmed();
    if (!accepted || name.isEmpty()) return;
    if (name.contains(QLatin1Char('/')) || name == QStringLiteral(".") || name == QStringLiteral("..")) {
        QMessageBox::warning(this, QStringLiteral("目录名称无效"), QStringLiteral("目录名称不能包含 /，也不能是 . 或 ..。"));
        return;
    }
    const auto path = m_currentPath == QStringLiteral("/") ? QStringLiteral("/") + name : m_currentPath + QLatin1Char('/') + name;
    m_mutationInFlight = true;
    m_statusLabel->setText(QStringLiteral("  正在新建目录 · %1").arg(name));
    updateActionState();
    m_session->createDirectory(path);
}

void FilePanel::renameSelected()
{
    if (m_mutationInFlight || m_inlineRenameActive) return;
    if (selectedEntries().size() != 1) return;
    auto *item = selectedEntry();
    if (!item) return;
    m_inlineRenameActive = true;
    m_renamingItem = item;
    m_renameSourcePath = item->data(0, kPathRole).toString();
    m_renameOriginalName = item->text(0);
    m_renameEditor = new QLineEdit(m_renameOriginalName, m_tree);
    m_renameEditor->setObjectName(QStringLiteral("inlineRenameEditor"));
    m_renameEditor->setFrame(true);
    m_renameEditor->installEventFilter(this);
    m_tree->setItemWidget(item, 0, m_renameEditor);
    connect(m_renameEditor, &QLineEdit::editingFinished, this, [this] { finishInlineRename(true); });
    m_renameEditor->setFocus();
    m_renameEditor->selectAll();
    m_statusLabel->setText(QStringLiteral("  输入新名称，Enter 或失去焦点保存，Esc 取消"));
    updateActionState();
}

bool FilePanel::eventFilter(QObject *watched, QEvent *event)
{
    if (m_tree && watched == m_tree->viewport()) {
        const auto localFiles = [](const QMimeData *mimeData) {
            QStringList paths;
            if (!mimeData || !mimeData->hasUrls()) return paths;
            for (const auto &url : mimeData->urls()) {
                if (url.isLocalFile()) paths.append(url.toLocalFile());
            }
            return paths;
        };
        if (event->type() == QEvent::DragEnter) {
            auto *dragEvent = static_cast<QDragEnterEvent *>(event);
            if (m_connected && !localFiles(dragEvent->mimeData()).isEmpty()) {
                dragEvent->setDropAction(Qt::CopyAction);
                dragEvent->accept();
                return true;
            }
        } else if (event->type() == QEvent::DragMove) {
            auto *dragEvent = static_cast<QDragMoveEvent *>(event);
            if (m_connected && !localFiles(dragEvent->mimeData()).isEmpty()) {
                dragEvent->setDropAction(Qt::CopyAction);
                dragEvent->accept();
                return true;
            }
        } else if (event->type() == QEvent::Drop) {
            auto *dropEvent = static_cast<QDropEvent *>(event);
            const auto paths = localFiles(dropEvent->mimeData());
            if (m_connected && !paths.isEmpty()) {
                auto destination = m_currentPath;
                if (auto *item = m_tree->itemAt(dropEvent->position().toPoint());
                    item && item->data(0, kDirectoryRole).toBool()) {
                    destination = item->data(0, kPathRole).toString();
                }
                uploadLocalFiles(paths, destination);
                dropEvent->setDropAction(Qt::CopyAction);
                dropEvent->accept();
                return true;
            }
        }
    }
    if (watched == m_renameEditor && event->type() == QEvent::KeyPress) {
        const auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Escape) {
            finishInlineRename(false);
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}

void FilePanel::finishInlineRename(bool submit)
{
    if (!m_inlineRenameActive) return;
    auto *item = m_renamingItem;
    auto *editor = m_renameEditor;
    const auto name = editor ? editor->text().trimmed() : QString{};
    if (submit && (name.isEmpty() || name.contains(QLatin1Char('/')) || name == QStringLiteral(".") || name == QStringLiteral(".."))) {
        m_statusLabel->setText(QStringLiteral("  名称无效：不能为空、包含 /，也不能是 . 或 .."));
        QTimer::singleShot(0, editor, [editor] {
            editor->setFocus();
            editor->selectAll();
        });
        return;
    }

    m_inlineRenameActive = false;
    m_renameEditor = nullptr;
    if (item) m_tree->removeItemWidget(item, 0);
    if (editor) editor->deleteLater();
    if (!submit || name == m_renameOriginalName || !item) {
        m_renamingItem = nullptr;
        m_renameSourcePath.clear();
        m_renameOriginalName.clear();
        updateActionState();
        return;
    }

    const auto sourcePath = m_renameSourcePath;
    const auto originalName = m_renameOriginalName;
    const auto destinationPath = m_currentPath == QStringLiteral("/") ? QStringLiteral("/") + name
                                                                        : m_currentPath + QLatin1Char('/') + name;
    item->setText(0, name);
    m_renamingItem = nullptr;
    m_renameSourcePath.clear();
    m_renameOriginalName.clear();
    m_mutationInFlight = true;
    m_statusLabel->setText(QStringLiteral("  正在重命名 · %1 → %2").arg(originalName, name));
    updateActionState();
    m_session->renamePath(sourcePath, destinationPath);
}

void FilePanel::removeSelected()
{
    if (m_mutationInFlight || m_inlineRenameActive) return;
    if (selectedEntries().size() != 1) return;
    auto *item = selectedEntry();
    if (!item) return;
    const auto path = item->data(0, kPathRole).toString();
    const bool directory = item->data(0, kDirectoryRole).toBool();
    const auto answer = QMessageBox::warning(this, QStringLiteral("删除远端项目"),
        directory
            ? QStringLiteral("确定删除空目录“%1”吗？非空目录会被服务器拒绝。此操作不可撤销。").arg(QFileInfo(path).fileName())
            : QStringLiteral("确定删除文件“%1”吗？此操作不可撤销。").arg(QFileInfo(path).fileName()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;
    m_mutationInFlight = true;
    m_tree->clearSelection();
    m_statusLabel->setText(QStringLiteral("  正在删除 · %1").arg(QFileInfo(path).fileName()));
    updateActionState();
    m_session->removePath(path, directory);
}

void FilePanel::navigateBack()
{
    if (m_historyIndex <= 0) return;
    --m_historyIndex;
    navigateTo(m_history.at(m_historyIndex), false);
}

void FilePanel::navigateUp()
{
    if (m_currentPath == QStringLiteral("/")) return;
    navigateTo(QFileInfo(m_currentPath).path());
}

QString FilePanel::normalizePath(const QString &path)
{
    auto value = path.trimmed();
    if (value.isEmpty()) return QStringLiteral("/");
    if (!value.startsWith(QLatin1Char('/'))) value.prepend(QLatin1Char('/'));
    return QDir::cleanPath(value);
}

} // namespace noxshell::ui
