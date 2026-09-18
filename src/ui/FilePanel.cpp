#include "FilePanel.h"

#include "../core/SshSession.h"
#include "FilePermissionDialog.h"
#include "RemoteFileEditor.h"
#include "RemotePathEdit.h"
#include "TransferQueuePanel.h"

#include <QDir>
#include <QCheckBox>
#include <QCursor>
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
#include <QProcess>
#include <QProgressBar>
#include <QPainter>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QSplitter>
#include <QShortcut>
#include <QStackedLayout>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionHeader>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <QUrl>

#include <algorithm>

namespace noxshell::ui {

namespace {
constexpr int kInteractiveStartupGraceMs = 120;
constexpr int kPathRole = Qt::UserRole;
constexpr int kDirectoryRole = Qt::UserRole + 1;
constexpr int kDirectoryLoadedRole = Qt::UserRole + 2;
constexpr int kPlaceholderRole = Qt::UserRole + 3;
constexpr int kPermissionsRole = Qt::UserRole + 4;
constexpr int kSymbolicLinkRole = Qt::UserRole + 5;
constexpr int kSizeRole = Qt::UserRole + 6;
constexpr int kSizeKnownRole = Qt::UserRole + 7;
constexpr int kSizeStateRole = Qt::UserRole + 8;
constexpr int kSizeErrorRole = Qt::UserRole + 9;
enum SizeState { UnknownSize, QueuedSize, RunningSize, ReadySize, FailedSize };

// Paint vector controls only for visible rows: no per-directory widgets or timers.
QRect sizeActionRect(const QRect &cell)
{
    return QRect(cell.left() + 5, cell.center().y() - 11, 22, 22);
}

bool hasSizeAction(const QModelIndex &index)
{
    const auto entry = index.siblingAtColumn(0);
    return index.column() == 1 && entry.data(kDirectoryRole).toBool()
        && !entry.data(kSymbolicLinkRole).toBool();
}

bool sizeActionEnabled(const QModelIndex &index)
{
    const int state = index.siblingAtColumn(0).data(kSizeStateRole).toInt();
    return hasSizeAction(index) && state != QueuedSize && state != RunningSize;
}

class FileHeader final : public QHeaderView {
public:
    explicit FileHeader(QWidget *parent) : QHeaderView(Qt::Horizontal, parent)
    {
        setObjectName(QStringLiteral("remoteFileHeader"));
        setFixedHeight(36);
        setHighlightSections(false);
        setMouseTracking(true);
    }

protected:
    void paintSection(QPainter *painter, const QRect &rect, int column) const override
    {
        if (!rect.isValid()) return;
        painter->save();
        QStyleOptionHeader option;
        initStyleOption(&option);
        initStyleOptionForIndex(&option, column);
        option.rect = rect;
        option.text.clear();
        option.sortIndicator = QStyleOptionHeader::None;
        style()->drawControl(QStyle::CE_Header, &option, painter, this);

        const bool sortable = column == 0 || column == 1;
        const bool active = sortable && sortIndicatorSection() == column;
        auto textColor = palette().color(QPalette::Text);
        textColor.setAlpha(active ? 255 : 190);
        auto labelFont = font();
        labelFont.setPixelSize(12);
        labelFont.setWeight(QFont::Medium);
        painter->setFont(labelFont);
        painter->setPen(active ? palette().color(QPalette::Link) : textColor);
        const auto labelRect = rect.adjusted(10, 0, sortable ? -31 : -10, -1);
        painter->drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetrics(labelFont).elidedText(model()->headerData(column, orientation()).toString(),
                Qt::ElideRight, labelRect.width()));
        auto divider = palette().color(QPalette::Text);
        divider.setAlpha(24);
        painter->setPen(divider);
        painter->drawLine(rect.right(), rect.top() + 11, rect.right(), rect.bottom() - 11);
        if (sortable) {
            painter->setRenderHint(QPainter::Antialiasing);
            auto muted = palette().color(QPalette::Text);
            muted.setAlpha(85);
            const qreal y = rect.center().y();
            for (int direction = 0; direction < 2; ++direction) {
                const bool selected = active && (sortIndicatorOrder() == Qt::AscendingOrder) == (direction == 0);
                painter->setPen(QPen(selected ? palette().color(QPalette::Link) : muted,
                    1.35, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                const qreal x = rect.right() - 22 + direction * 8;
                const qreal tip = y + (direction == 0 ? -5 : 5);
                const qreal tail = y + (direction == 0 ? 5 : -5);
                const qreal shoulder = tip + (direction == 0 ? 3 : -3);
                painter->drawLine(QPointF(x, tail), QPointF(x, tip));
                painter->drawPolyline(QPolygonF{QPointF(x - 2.5, shoulder), QPointF(x, tip), QPointF(x + 2.5, shoulder)});
            }
        }
        painter->restore();
    }
};

class FileSizeDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        auto background = option;
        initStyleOption(&background, index);
        const auto text = background.text;
        background.text.clear();
        const auto *widget = option.widget;
        if (!widget) return;
        widget->style()->drawControl(QStyle::CE_ItemViewItem, &background, painter, widget);
        painter->save();
        painter->setClipRect(option.rect);
        const bool action = hasSizeAction(index);
        const auto textRect = option.rect.adjusted(action ? 34 : 8, 0, -9, 0);
        // Let the stylesheet resolve selection text colors (light mode uses
        // blue text on pale blue, unlike the application palette's white).
        auto label = background;
        label.rect = textRect;
        label.text = text;
        label.displayAlignment = Qt::AlignRight | Qt::AlignVCenter;
        label.textElideMode = Qt::ElideLeft;
        label.state &= ~QStyle::State_HasFocus;
        widget->style()->drawControl(QStyle::CE_ItemViewItem, &label, painter, widget);
        if (action) {
            const auto button = sizeActionRect(option.rect);
            const bool enabled = sizeActionEnabled(index) && (option.state & QStyle::State_Enabled);
            const auto *view = qobject_cast<const QAbstractItemView *>(widget);
            const auto *surface = view ? view->viewport() : widget;
            const bool hovered = enabled && (option.state & QStyle::State_MouseOver)
                && button.contains(surface->mapFromGlobal(QCursor::pos()));
            auto color = background.palette.color(QPalette::Link);
            if (!enabled) color.setAlpha(95);
            painter->setRenderHint(QPainter::Antialiasing);
            if (hovered) {
                auto fill = color;
                fill.setAlpha(24);
                painter->setPen(Qt::NoPen);
                painter->setBrush(fill);
                painter->drawRoundedRect(button, 4, 4);
            }
            painter->setBrush(Qt::NoBrush);
            painter->setPen(QPen(color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const auto center = QPointF(button.center());
            const int state = index.siblingAtColumn(0).data(kSizeStateRole).toInt();
            if (state == RunningSize || state == QueuedSize) {
                painter->drawEllipse(center, 5, 5);
                painter->drawPolyline(QPolygonF{center + QPointF(0, -3), center, center + QPointF(2.5, 1)});
            } else {
                painter->drawArc(QRectF(center.x() - 5, center.y() - 5, 10, 10), 45 * 16, 290 * 16);
                painter->drawPolyline(QPolygonF{center + QPointF(0.5, -3.5),
                    center + QPointF(4, -3.5), center + QPointF(4, -7)});
            }
        }
        painter->restore();
    }
};

class FileListItem final : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;
    bool operator<(const QTreeWidgetItem &other) const override
    {
        const auto *tree = treeWidget();
        const int column = tree ? tree->sortColumn() : 0;
        const bool descending = tree && tree->header()->sortIndicatorOrder() == Qt::DescendingOrder;
        if (column == 1) {
            const bool known = data(0, kSizeKnownRole).toBool();
            const bool otherKnown = other.data(0, kSizeKnownRole).toBool();
            // Unknown sizes remain last in both ascending and descending views.
            if (known != otherKnown) return descending ? !known : known;
            const auto size = data(0, kSizeRole).toULongLong();
            const auto otherSize = other.data(0, kSizeRole).toULongLong();
            if (known && size != otherSize) return size < otherSize;
        } else {
            const bool directory = data(0, kDirectoryRole).toBool();
            if (directory != other.data(0, kDirectoryRole).toBool()) return descending ? !directory : directory;
        }
        return QString::localeAwareCompare(text(0), other.text(0)) < 0;
    }
};

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
    headerLayout->setContentsMargins(11, 6, 9, 6);
    headerLayout->setSpacing(6);
    auto *title = new QLabel(QStringLiteral("文件管理"));
    title->setObjectName(QStringLiteral("filePanelTitle"));
    m_serverLabel = new QLabel(QStringLiteral("SFTP"));
    m_serverLabel->setObjectName(QStringLiteral("fileServerLabel"));
    m_transferQueueButton = new QToolButton;
    m_transferQueueButton->setObjectName(QStringLiteral("transferQueueButton"));
    m_contextMenu = new QMenu(this);
    m_contextMenu->setObjectName(QStringLiteral("fileContextMenu"));
    m_newFileAction = m_contextMenu->addAction(QStringLiteral("新建文件"));
    m_newFileAction->setObjectName(QStringLiteral("fileNewFileAction"));
    m_newDirectoryAction = m_contextMenu->addAction(QStringLiteral("新建目录"));
    m_newDirectoryAction->setObjectName(QStringLiteral("fileNewDirectoryAction"));
    m_contextMenu->addSeparator();
    m_downloadAction = m_contextMenu->addAction(QStringLiteral("下载所选文件"));
    m_downloadAction->setObjectName(QStringLiteral("fileContextDownloadAction"));
    m_contextMenu->addSeparator();
    m_renameAction = m_contextMenu->addAction(QStringLiteral("重命名"));
    m_permissionsAction = m_contextMenu->addAction(QStringLiteral("权限管理"));
    m_removeAction = m_contextMenu->addAction(QStringLiteral("删除"));
    m_renameAction->setObjectName(QStringLiteral("fileRenameAction"));
    m_permissionsAction->setObjectName(QStringLiteral("filePermissionsAction"));
    m_removeAction->setObjectName(QStringLiteral("fileRemoveAction"));
    m_transferQueueButton->setIcon(QIcon(QStringLiteral(":/assets/transfer-queue.svg")));
    m_transferQueueButton->setIconSize(QSize(16, 16));
    m_transferQueueButton->setToolTip(QStringLiteral("传输队列 · 空闲"));
    m_transferQueueButton->setAccessibleName(QStringLiteral("打开传输队列"));
    m_transferQueueButton->setAutoRaise(true);
    m_transferQueueButton->setFixedSize(26, 26);

    m_transferQueueMenu = new QMenu(m_transferQueueButton);
    m_transferQueueMenu->setObjectName(QStringLiteral("transferQueueMenu"));
    auto *queueWidgetAction = new QWidgetAction(m_transferQueueMenu);
    m_transferQueuePanel = new TransferQueuePanel(m_session);
    m_transferQueuePanel->setProperty("popup", true);
    m_transferQueuePanel->setFixedWidth(520);
    queueWidgetAction->setDefaultWidget(m_transferQueuePanel);
    m_transferQueueMenu->addAction(queueWidgetAction);
    m_transferQueueButton->setMenu(m_transferQueueMenu);
    m_transferQueueButton->setPopupMode(QToolButton::InstantPopup);

    m_statusLabel = new QLabel(QStringLiteral("  等待 SSH 连接"));
    m_statusLabel->setObjectName(QStringLiteral("fileStatusLabel"));
    m_statusLabel->setMinimumWidth(0);
    m_statusLabel->setMaximumWidth(170);
    m_statusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    m_backButton = new QToolButton;
    m_upButton = new QToolButton;
    m_refreshButton = new QToolButton;
    m_backButton->setObjectName(QStringLiteral("fileBackButton"));
    m_upButton->setObjectName(QStringLiteral("fileUpButton"));
    m_refreshButton->setObjectName(QStringLiteral("fileRefreshButton"));
    m_backButton->setIcon(QIcon(QStringLiteral(":/assets/file-back.svg")));
    m_upButton->setIcon(QIcon(QStringLiteral(":/assets/file-up.svg")));
    m_refreshButton->setIcon(QIcon(QStringLiteral(":/assets/file-refresh.svg")));
    m_backButton->setToolTip(QStringLiteral("返回"));
    m_upButton->setToolTip(QStringLiteral("上级目录"));
    m_refreshButton->setToolTip(QStringLiteral("刷新目录"));
    for (auto *button : {m_backButton, m_upButton, m_refreshButton}) {
        button->setAutoRaise(true);
        button->setFixedSize(26, 26);
        button->setIconSize(QSize(16, 16));
    }
    m_pathEdit = new RemotePathEdit;
    auto *editPathShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_L), this);
    editPathShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(editPathShortcut, &QShortcut::activated, m_pathEdit, &RemotePathEdit::beginEditing);
    m_autoSize = new QCheckBox(QStringLiteral("自动计算"));
    m_autoSize->setObjectName(QStringLiteral("fileAutoDirectorySize"));
    m_autoSize->setToolTip(QStringLiteral("逐个计算当前列表的目录磁盘占用，默认关闭。\n低优先级，每项最多 60 秒；不跟随符号链接、不跨文件系统。\n切换目录或关闭开关会停止旧计算；扫描仍会产生磁盘 I/O。"));

    headerLayout->addWidget(title);
    headerLayout->addWidget(m_serverLabel);
    headerLayout->addWidget(m_statusLabel);
    headerLayout->addWidget(m_backButton);
    headerLayout->addWidget(m_upButton);
    headerLayout->addWidget(m_pathEdit, 1);
    headerLayout->addWidget(m_autoSize);
    headerLayout->addWidget(m_refreshButton);
    headerLayout->addWidget(m_transferQueueButton);

    m_tree = new QTreeWidget;
    m_tree->setObjectName(QStringLiteral("remoteFileTree"));
    m_tree->setHeader(new FileHeader(m_tree));
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
    m_tree->setMouseTracking(true);
    m_tree->setItemDelegateForColumn(1, new FileSizeDelegate(m_tree));
    m_tree->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setMinimumSectionSize(64);
    for (int column = 0; column < m_tree->columnCount(); ++column) {
        m_tree->header()->setSectionResizeMode(column, QHeaderView::Interactive);
    }
    const int widths[] = {190, 132, 80, 150, 112, 122};
    for (int column = 0; column < m_tree->columnCount(); ++column) m_tree->setColumnWidth(column, widths[column]);
    m_tree->headerItem()->setToolTip(0, QStringLiteral("点击按文件名升序 / 降序排列"));
    m_tree->headerItem()->setToolTip(1, QStringLiteral("点击按实际字节数升序 / 降序排列；未计算的目录排在末尾"));
    applyFileSort(m_sortIndex);

    auto *fileListContainer = new QWidget;
    fileListContainer->setObjectName(QStringLiteral("fileListContainer"));
    auto *fileListStack = new QStackedLayout(fileListContainer);
    fileListStack->setContentsMargins(0, 0, 0, 0);
    fileListStack->setStackingMode(QStackedLayout::StackAll);
    fileListStack->addWidget(m_tree);

    m_fileLoadingOverlay = new QWidget;
    m_fileLoadingOverlay->setObjectName(QStringLiteral("fileLoadingOverlay"));
    auto *loadingOverlayLayout = new QVBoxLayout(m_fileLoadingOverlay);
    loadingOverlayLayout->setContentsMargins(24, 24, 24, 24);
    loadingOverlayLayout->addStretch();
    auto *loadingRow = new QHBoxLayout;
    loadingRow->addStretch();
    auto *loadingCard = new QFrame;
    loadingCard->setObjectName(QStringLiteral("fileLoadingCard"));
    loadingCard->setFixedWidth(280);
    auto *loadingCardLayout = new QVBoxLayout(loadingCard);
    loadingCardLayout->setContentsMargins(22, 17, 22, 16);
    loadingCardLayout->setSpacing(8);
    auto *loadingTitle = new QLabel(QStringLiteral("正在加载文件"));
    loadingTitle->setObjectName(QStringLiteral("fileLoadingTitle"));
    loadingTitle->setAlignment(Qt::AlignCenter);
    m_fileLoadingDetail = new QLabel(QStringLiteral("正在读取远端目录…"));
    m_fileLoadingDetail->setObjectName(QStringLiteral("fileLoadingDetail"));
    m_fileLoadingDetail->setAlignment(Qt::AlignCenter);
    m_fileLoadingDetail->setWordWrap(true);
    auto *loadingProgress = new QProgressBar;
    loadingProgress->setObjectName(QStringLiteral("fileLoadingProgress"));
    loadingProgress->setRange(0, 0);
    loadingProgress->setTextVisible(false);
    loadingProgress->setFixedHeight(4);
    loadingCardLayout->addWidget(loadingTitle);
    loadingCardLayout->addWidget(m_fileLoadingDetail);
    loadingCardLayout->addSpacing(2);
    loadingCardLayout->addWidget(loadingProgress);
    loadingRow->addWidget(loadingCard);
    loadingRow->addStretch();
    loadingOverlayLayout->addLayout(loadingRow);
    loadingOverlayLayout->addStretch();
    fileListStack->addWidget(m_fileLoadingOverlay);
    m_fileLoadingOverlay->hide();

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
    browserSplitter->addWidget(fileListContainer);
    browserSplitter->setStretchFactor(0, 0);
    browserSplitter->setStretchFactor(1, 1);
    browserSplitter->setSizes({205, 760});

    header->setFixedHeight(40);
    layout->addWidget(header);
    layout->addWidget(browserSplitter, 1);

    connect(m_transferQueuePanel, &TransferQueuePanel::summaryChanged, this,
        [this](int activeCount, int totalCount, const QString &summary) {
            m_transferQueueButton->setProperty("active", activeCount > 0);
            m_transferQueueButton->setToolTip(QStringLiteral("传输队列 · %1 · 共 %2 项").arg(summary).arg(totalCount));
            m_transferQueueButton->style()->unpolish(m_transferQueueButton);
            m_transferQueueButton->style()->polish(m_transferQueueButton);
        });
    connect(m_transferQueuePanel, &TransferQueuePanel::taskAdded, this, [this] {
        QTimer::singleShot(0, this, [this] {
            if (!isVisible() || !m_transferQueueMenu || m_transferQueueMenu->isVisible()) return;
            m_transferQueueMenu->popup(
                m_transferQueueButton->mapToGlobal(QPoint(0, m_transferQueueButton->height())));
        });
    });

    connect(m_refreshButton, &QToolButton::clicked, this, [this] { navigateTo(m_currentPath, false); });
    connect(m_pathEdit, &QLineEdit::returnPressed, this, [this] { navigateTo(m_pathEdit->text()); });
    connect(m_pathEdit, &RemotePathEdit::pathActivated, this, [this](const QString &path) { navigateTo(path); });
    connect(m_upButton, &QToolButton::clicked, this, &FilePanel::navigateUp);
    connect(m_backButton, &QToolButton::clicked, this, &FilePanel::navigateBack);
    m_tree->header()->setSectionsClickable(true);
    connect(m_tree->header(), &QHeaderView::sectionClicked, this, [this](int column) {
        // QHeaderView changes its sort section even with the native indicator
        // hidden. Restore our state for columns that do not support sorting.
        if (column != 0 && column != 1) { applyFileSort(m_sortIndex); return; }
        const int base = column == 0 ? 0 : 2;
        applyFileSort(m_sortIndex == base ? base + 1 : base);
    });
    connect(m_autoSize, &QCheckBox::toggled, this, [this](bool enabled) {
        if (!enabled) { cancelSizeCalculations(); return; }
        for (int row = 0; row < m_tree->topLevelItemCount(); ++row) {
            auto *item = m_tree->topLevelItem(row);
            if (!item->data(0, kSizeKnownRole).toBool()) queueDirectorySize(item);
        }
    });
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int column) {
        if (column == 1 && item->data(0, kDirectoryRole).toBool()) return;
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
        if (m_connected) m_contextMenu->exec(m_tree->viewport()->mapToGlobal(position));
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
    connect(m_permissionsAction, &QAction::triggered, this, &FilePanel::changeSelectedPermissions);
    connect(m_removeAction, &QAction::triggered, this, &FilePanel::removeSelected);
    connect(m_session, &SshSession::directoryListed, this, &FilePanel::showEntries);
    connect(m_session, &SshSession::directoryListingFailed, this, &FilePanel::showError);
    connect(m_session, &SshSession::directorySizeCalculated, this,
        [this](quint64 request, const QString &path, quint64 bytes, const QString &error) {
            if (!m_sizeRequest || request != m_sizeRequest || path != m_sizeActivePath || !m_pendingPath.isEmpty()) return;
            m_sizeRequest = 0;
            m_sizeActivePath.clear();
            for (int row = 0; row < m_tree->topLevelItemCount(); ++row) {
                auto *item = m_tree->topLevelItem(row);
                if (item->data(0, kPathRole).toString() != path) continue;
                item->setData(0, kSizeStateRole, error.isEmpty() ? ReadySize : FailedSize);
                item->setData(0, kSizeKnownRole, error.isEmpty());
                item->setData(0, kSizeRole, QVariant::fromValue(bytes));
                item->setData(0, kSizeErrorRole, error);
                updateSizeCell(item);
                break;
            }
            if (m_sortIndex >= 2) applyFileSort(m_sortIndex);
            // Yield to terminal, navigation and transfers between directories.
            QTimer::singleShot(200, this, &FilePanel::startNextDirectorySize);
        });
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
            if (operation == RemoteFileOperation::Remove && !m_removeQueue.isEmpty()) {
                m_removeQueue.removeFirst();
                ++m_removeSucceeded;
                QTimer::singleShot(0, this, &FilePanel::startNextRemoval);
                return;
            }
            const auto names = QStringList{QStringLiteral("上传"), QStringLiteral("下载"), QStringLiteral("新建目录"),
                QStringLiteral("重命名"), QStringLiteral("删除"), QStringLiteral("权限修改")};
            m_statusLabel->setText(QStringLiteral("  %1完成  ·  %2").arg(names.value(static_cast<int>(operation)), QFileInfo(path).fileName()));
            if (operation == RemoteFileOperation::MakeDirectory || operation == RemoteFileOperation::Rename
                || operation == RemoteFileOperation::Remove || operation == RemoteFileOperation::ChangePermissions) {
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
        [this](RemoteFileOperation operation, const QString &path, const QString &message) {
            if (operation == RemoteFileOperation::Remove && !m_removeQueue.isEmpty()) {
                m_removeFailures.append(QStringLiteral("%1：%2").arg(QFileInfo(path).fileName(), message));
                m_removeQueue.removeFirst();
                QTimer::singleShot(0, this, &FilePanel::startNextRemoval);
                return;
            }
            if (operation == RemoteFileOperation::MakeDirectory || operation == RemoteFileOperation::Rename
                || operation == RemoteFileOperation::Remove || operation == RemoteFileOperation::ChangePermissions) {
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
        if (!connected) cancelSizeCalculations();
        m_connected = connected;
        m_directoryTree->setEnabled(connected);
        updateActionState();
        if (connected) {
            // Let the interactive shell paint its first prompt before SFTP
            // temporarily borrows the shared SSH transport.
            QTimer::singleShot(kInteractiveStartupGraceMs, this, [this] {
                if (m_connected && m_session && m_session->isConnected()) openInitialDirectory();
            });
        } else {
            hideFileLoading();
            m_statusLabel->setText(QStringLiteral("  %1").arg(message));
        }
    });
    m_backButton->setEnabled(false);
    updateActionState();
}

FilePanel::~FilePanel()
{
    if (m_sizeRequest && m_session) m_session->cancelDirectorySize();
}

void FilePanel::changeEvent(QEvent *event)
{
    QFrame::changeEvent(event);
    if (m_tree && (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)) {
        for (int row = 0; row < m_tree->topLevelItemCount(); ++row) updateSizeCell(m_tree->topLevelItem(row));
    }
}

void FilePanel::setServer(const ServerProfile &profile)
{
    cancelSizeCalculations();
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
    m_removeQueue.clear();
    m_removeTotal = 0;
    m_removeSucceeded = 0;
    m_removeFailures.clear();
    m_statusLabel->setToolTip({});
    m_postRefreshStatus.clear();
    m_history = {m_currentPath};
    m_historyIndex = 0;
    m_tree->clear();
    hideFileLoading();
    m_directoryTree->clear();
    m_directoryTree->setEnabled(false);
    m_pathEdit->setPath(m_currentPath);
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
    showFileLoading(QStringLiteral("正在定位远端主目录…"));
    m_session->requestHomeDirectory();
}

void FilePanel::navigateTo(const QString &path, bool addToHistory)
{
    cancelSizeCalculations();
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
    m_pathEdit->setPath(normalized);
    m_backButton->setEnabled(m_historyIndex > 0);
    m_upButton->setEnabled(normalized != QStringLiteral("/"));
    m_tree->clear();
    m_tree->setEnabled(false);
    m_statusLabel->setText(QStringLiteral("  正在读取 %1 …").arg(normalized));
    showFileLoading(QStringLiteral("正在读取 %1").arg(normalized));
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
    hideFileLoading();
    m_tree->clear();
    m_tree->setEnabled(true);
    for (const auto &entry : entries) {
        const auto size = entry.directory ? QStringLiteral("—") : formatSize(entry.size);
        const auto type = entry.directory ? QStringLiteral("文件夹") : entry.symbolicLink ? QStringLiteral("符号链接") : QStringLiteral("文件");
        const auto modified = entry.modifiedAt.isValid() ? entry.modifiedAt.toString(QStringLiteral("yyyy/MM/dd HH:mm")) : QStringLiteral("—");
        auto *item = new FileListItem(m_tree,
            {entry.name, size, type, modified, permissionText(entry.permissions), ownerGroupText(entry)});
        item->setIcon(0, style()->standardIcon(entry.directory ? QStyle::SP_DirIcon
                                                               : entry.symbolicLink ? QStyle::SP_FileLinkIcon : QStyle::SP_FileIcon));
        item->setData(0, kPathRole, entry.path);
        item->setData(0, kDirectoryRole, entry.directory);
        item->setData(0, kPermissionsRole, entry.permissions);
        item->setData(0, kSymbolicLinkRole, entry.symbolicLink);
        item->setData(0, kSizeRole, QVariant::fromValue(entry.size));
        item->setData(0, kSizeKnownRole, !entry.directory);
        item->setToolTip(0, entry.path);
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        if (entry.directory) updateSizeCell(item);
    }
    if (m_postRefreshStatus.isEmpty()) {
        m_statusLabel->setText(QStringLiteral("  %1 个项目  ·  %2").arg(entries.size()).arg(path));
    } else {
        m_statusLabel->setText(m_postRefreshStatus);
        m_postRefreshStatus.clear();
    }
    revealDirectoryPath(path);
    applyFileSort(m_sortIndex);
    updateActionState();
    if (m_autoSize->isChecked()) {
        for (int row = 0; row < m_tree->topLevelItemCount(); ++row) queueDirectorySize(m_tree->topLevelItem(row));
    }
}

void FilePanel::applyFileSort(int index)
{
    m_sortIndex = index;
    const int column = index >= 2 ? 1 : 0;
    const auto order = index % 2 ? Qt::DescendingOrder : Qt::AscendingOrder;
    m_tree->header()->setSortIndicator(column, order);
    // Only the custom paired arrows are painted, not Qt's native single arrow.
    m_tree->header()->setSortIndicatorShown(false);
    m_tree->sortItems(column, order);
    QStringList ordered;
    for (int row = 0; row < m_tree->topLevelItemCount(); ++row) {
        const auto path = m_tree->topLevelItem(row)->data(0, kPathRole).toString();
        if (m_sizeQueue.contains(path)) ordered.append(path);
    }
    m_sizeQueue = ordered;
}

void FilePanel::updateSizeCell(QTreeWidgetItem *item)
{
    if (!item->data(0, kDirectoryRole).toBool()) return;
    if (item->data(0, kSymbolicLinkRole).toBool()) {
        item->setText(1, QStringLiteral("—"));
        item->setToolTip(1, QStringLiteral("不遍历符号链接，避免循环或扫描目录外的文件"));
        return;
    }
    switch (item->data(0, kSizeStateRole).toInt()) {
    case QueuedSize: item->setText(1, QStringLiteral("等待计算…")); break;
    case RunningSize: item->setText(1, QStringLiteral("计算中…")); break;
    case ReadySize: item->setText(1, formatSize(item->data(0, kSizeRole).toULongLong())); break;
    case FailedSize: item->setText(1, QStringLiteral("未完成")); break;
    default: item->setText(1, QStringLiteral("—")); break;
    }
    item->setToolTip(1, QStringLiteral("点击左侧图标计算 / 重新计算目录磁盘占用。\n包含隐藏文件；不跟随链接、不跨文件系统；硬链接按 du 规则计数。\n不是下载文件，不读取文件内容；结果是本次扫描快照。\n%1")
        .arg(item->data(0, kSizeErrorRole).toString().toHtmlEscaped()));
}

void FilePanel::cancelSizeCalculations()
{
    if (m_sizeRequest && m_session) m_session->cancelDirectorySize();
    m_sizeRequest = 0;
    m_sizeActivePath.clear();
    m_sizeQueue.clear();
    if (!m_tree) return;
    for (int row = 0; row < m_tree->topLevelItemCount(); ++row) {
        auto *item = m_tree->topLevelItem(row);
        const int state = item->data(0, kSizeStateRole).toInt();
        if (state != QueuedSize && state != RunningSize) continue;
        item->setData(0, kSizeStateRole, UnknownSize);
        item->setData(0, kSizeKnownRole, false);
        updateSizeCell(item);
    }
}

void FilePanel::queueDirectorySize(QTreeWidgetItem *item)
{
    if (!m_connected || !m_pendingPath.isEmpty() || !item || !item->data(0, kDirectoryRole).toBool()
        || item->data(0, kSymbolicLinkRole).toBool()) return;
    const auto path = item->data(0, kPathRole).toString();
    if (path.isEmpty() || path == m_sizeActivePath || m_sizeQueue.contains(path)) return;
    item->setData(0, kSizeStateRole, QueuedSize);
    item->setData(0, kSizeKnownRole, false);
    item->setData(0, kSizeErrorRole, QString{});
    updateSizeCell(item);
    m_sizeQueue.append(path);
    if (m_sizeQueue.size() == 1 && !m_sizeRequest) QTimer::singleShot(0, this, &FilePanel::startNextDirectorySize);
}

void FilePanel::startNextDirectorySize()
{
    if (m_sizeRequest || !m_connected || !m_pendingPath.isEmpty() || m_sizeQueue.isEmpty()) return;
    const auto path = m_sizeQueue.takeFirst();
    for (int row = 0; row < m_tree->topLevelItemCount(); ++row) {
        auto *item = m_tree->topLevelItem(row);
        if (item->data(0, kPathRole).toString() != path) continue;
        m_sizeActivePath = path;
        item->setData(0, kSizeStateRole, RunningSize);
        updateSizeCell(item);
        m_sizeRequest = m_session->calculateDirectorySize(path);
        return;
    }
    QTimer::singleShot(0, this, &FilePanel::startNextDirectorySize);
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
    hideFileLoading();
    m_tree->clear();
    m_tree->setEnabled(true);
    auto *item = new QTreeWidgetItem(m_tree, {QStringLiteral("无法读取目录"), QStringLiteral("—"), QStringLiteral("—"),
        QStringLiteral("—"), QStringLiteral("—"), QStringLiteral("—")});
    item->setForeground(0, QColor(QStringLiteral("#D54941")));
    m_statusLabel->setText(QStringLiteral("  SFTP 错误 · %1").arg(message));
    updateActionState();
}

void FilePanel::showFileLoading(const QString &detail)
{
    if (!m_fileLoadingOverlay || !m_fileLoadingDetail) return;
    m_fileLoadingDetail->setText(detail);
    m_fileLoadingOverlay->show();
    m_fileLoadingOverlay->raise();
}

void FilePanel::hideFileLoading()
{
    if (m_fileLoadingOverlay) m_fileLoadingOverlay->hide();
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
    m_autoSize->setEnabled(m_connected);
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
    m_renameAction->setEnabled(hasSelection && singleSelection);
    m_removeAction->setEnabled(hasSelection);
    m_permissionsAction->setEnabled(hasSelection && singleSelection
        && !items.first()->data(0, kSymbolicLinkRole).toBool());
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
        if (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonPress
            || event->type() == QEvent::MouseButtonRelease) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            const auto index = m_tree->indexAt(mouse->position().toPoint());
            const bool overAction = m_connected && m_tree->isEnabled() && sizeActionEnabled(index)
                && sizeActionRect(m_tree->visualRect(index)).contains(mouse->position().toPoint());
            m_tree->viewport()->setCursor(overAction ? Qt::PointingHandCursor : Qt::ArrowCursor);
            if (event->type() == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton) {
                m_pressedSizeAction = overAction ? QPersistentModelIndex(index) : QPersistentModelIndex{};
            } else if (event->type() == QEvent::MouseButtonRelease && mouse->button() == Qt::LeftButton) {
                const bool activate = overAction && m_pressedSizeAction == index;
                m_pressedSizeAction = QPersistentModelIndex{};
                if (activate) queueDirectorySize(m_tree->itemAt(mouse->position().toPoint()));
            }
        } else if (event->type() == QEvent::Leave) {
            m_tree->viewport()->unsetCursor();
        }
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
    const auto items = selectedEntries();
    if (items.isEmpty()) return;
    const auto answer = QMessageBox::warning(this, QStringLiteral("删除远端项目"),
        items.size() == 1
            ? (items.first()->data(0, kDirectoryRole).toBool()
                    ? QStringLiteral("确定删除空目录“%1”吗？非空目录会被服务器拒绝。此操作不可撤销。")
                          .arg(QFileInfo(items.first()->data(0, kPathRole).toString()).fileName())
                    : QStringLiteral("确定删除文件“%1”吗？此操作不可撤销。")
                          .arg(QFileInfo(items.first()->data(0, kPathRole).toString()).fileName()))
            : QStringLiteral("确定删除选中的 %1 个项目吗？非空目录会被服务器拒绝，其他项目仍会继续删除。此操作不可撤销。")
                  .arg(items.size()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;

    m_removeQueue.clear();
    for (auto *item : items) {
        const auto path = item->data(0, kPathRole).toString();
        if (!path.isEmpty()) m_removeQueue.append({path, item->data(0, kDirectoryRole).toBool()});
    }
    if (m_removeQueue.isEmpty()) return;
    m_removeTotal = m_removeQueue.size();
    m_removeSucceeded = 0;
    m_removeFailures.clear();
    m_statusLabel->setToolTip({});
    m_mutationInFlight = true;
    m_tree->clearSelection();
    updateActionState();
    startNextRemoval();
}

void FilePanel::startNextRemoval()
{
    if (m_removeQueue.isEmpty()) {
        finishRemovalBatch();
        return;
    }
    const auto &next = m_removeQueue.first();
    const int current = m_removeTotal - m_removeQueue.size() + 1;
    m_statusLabel->setText(QStringLiteral("  正在删除 %1/%2 · %3")
            .arg(current)
            .arg(m_removeTotal)
            .arg(QFileInfo(next.first).fileName()));
    m_session->removePath(next.first, next.second);
}

void FilePanel::finishRemovalBatch()
{
    m_mutationInFlight = false;
    if (auto *directoryNode = directoryItemForPath(m_currentPath)) {
        directoryNode->setData(0, kDirectoryLoadedRole, false);
        requestDirectoryNode(directoryNode);
    }
    if (m_removeFailures.isEmpty()) {
        m_postRefreshStatus = QStringLiteral("  已删除 %1 个项目").arg(m_removeSucceeded);
    } else {
        m_postRefreshStatus = QStringLiteral("  删除完成：成功 %1，失败 %2 · %3")
                .arg(m_removeSucceeded)
                .arg(m_removeFailures.size())
                .arg(m_removeFailures.join(QStringLiteral("；")));
        m_statusLabel->setToolTip(m_removeFailures.join(QLatin1Char('\n')));
    }
    navigateTo(m_currentPath, false);
    updateActionState();
}

void FilePanel::changeSelectedPermissions()
{
    if (m_mutationInFlight || m_inlineRenameActive || selectedEntries().size() != 1) return;
    auto *item = selectedEntry();
    if (!item || item->data(0, kSymbolicLinkRole).toBool()) return;

    RemoteFileEntry entry;
    entry.name = item->text(0);
    entry.path = item->data(0, kPathRole).toString();
    entry.directory = item->data(0, kDirectoryRole).toBool();
    entry.symbolicLink = item->data(0, kSymbolicLinkRole).toBool();
    entry.permissions = item->data(0, kPermissionsRole).toUInt();
    if (entry.path.isEmpty()) return;

    FilePermissionDialog dialog(entry, this);
    if (dialog.exec() != QDialog::Accepted) return;
    m_mutationInFlight = true;
    m_statusLabel->setText(QStringLiteral("  正在修改权限 · %1").arg(entry.name));
    updateActionState();
    m_session->changePermissions(entry.path, dialog.permissions(), dialog.recursive(), dialog.scope());
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
