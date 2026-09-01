#include "TerminalView.h"
#include "SearchMarkerScrollBar.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QCursor>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QRegularExpression>
#include <QShortcut>
#include <QStringList>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>

#include <utility>

namespace noxshell::ui {

namespace {
constexpr int kContentLeft = 14;
constexpr int kContentTop = 10;
constexpr int kContentRight = 6;
constexpr int kContentBottom = 8;
constexpr int kScrollBarWidth = 10;

TerminalAppearance normalizedAppearance(TerminalAppearance appearance)
{
    if (appearance.fontFamily.trimmed().isEmpty()) {
        appearance.fontFamily = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    }
    appearance.pointSize = qBound(8, appearance.pointSize, 32);
    appearance.lineSpacing = qBound<qreal>(1.0, appearance.lineSpacing, 2.0);
    return appearance;
}

TerminalAppearance &sharedDefaultAppearance()
{
    static TerminalAppearance appearance{
        QFontDatabase::systemFont(QFontDatabase::FixedFont).family(), 12, 1.05};
    return appearance;
}

const QRegularExpression &shellPromptExpression()
{
    static const QRegularExpression expression(QStringLiteral(
        "^\\s*(?:\\([^\\r\\n)]*\\)\\s*)?(?:\\[[^\\]\\r\\n]+\\]|"
        "[^\\s#$]+@[^\\s#$]+)[#$]\\s*"));
    return expression;
}
}

TerminalView::TerminalView(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("terminalOutput"));
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled);
    setAutoFillBackground(false);
    setMouseTracking(true);
    setAppearance(defaultAppearance());
    setCursor(Qt::IBeamCursor);
    m_scrollBar = new SearchMarkerScrollBar(Qt::Vertical, this);
    m_scrollBar->setObjectName(QStringLiteral("terminalScrollBar"));
    m_scrollBar->setSingleStep(1);
    m_scrollBar->setPageStep(m_model.rows());
    connect(m_scrollBar, &QScrollBar::valueChanged, this, [this] {
        if (m_lastMousePosition.x() >= 0) updateHoveredCommandBlock(m_lastMousePosition);
        update();
    });
    connect(m_scrollBar, &SearchMarkerScrollBar::searchMarkerActivated, this, [this](int index) {
        if (index < 0 || index >= m_searchMatches.size()) return;
        m_currentSearchMatch = index;
        updateSearchCounter();
        scrollToCurrentSearchMatch();
        update();
    });

    m_contextMenu = new QMenu(this);
    m_contextMenu->setObjectName(QStringLiteral("terminalContextMenu"));
    m_copyAction = m_contextMenu->addAction(QStringLiteral("复制"));
    m_copyAction->setObjectName(QStringLiteral("terminalCopyAction"));
    m_copyAction->setShortcut(QKeySequence::Copy);
    m_copyAction->setShortcutVisibleInContextMenu(true);
    m_pasteAction = m_contextMenu->addAction(QStringLiteral("粘贴"));
    m_pasteAction->setObjectName(QStringLiteral("terminalPasteAction"));
    m_pasteAction->setShortcut(QKeySequence::Paste);
    m_pasteAction->setShortcutVisibleInContextMenu(true);
    m_contextMenu->addSeparator();
    m_selectAllAction = m_contextMenu->addAction(QStringLiteral("全选"));
    m_selectAllAction->setObjectName(QStringLiteral("terminalSelectAllAction"));
    m_selectAllAction->setShortcut(QKeySequence::SelectAll);
    m_selectAllAction->setShortcutVisibleInContextMenu(true);
    m_contextMenu->addSeparator();
    m_findAction = m_contextMenu->addAction(QStringLiteral("查找"));
    m_findAction->setObjectName(QStringLiteral("terminalFindAction"));
    m_findAction->setText(QStringLiteral("查找\t%1")
                              .arg(QKeySequence(QKeySequence::Find)
                                       .toString(QKeySequence::NativeText)));
    connect(m_copyAction, &QAction::triggered, this, [this] {
        copySelection();
        QTimer::singleShot(0, this, [this] { setFocus(); });
    });
    connect(m_pasteAction, &QAction::triggered, this, [this] {
        sendPaste(QApplication::clipboard()->text());
        QTimer::singleShot(0, this, [this] { setFocus(); });
    });
    connect(m_selectAllAction, &QAction::triggered, this, &TerminalView::selectAllText);
    connect(m_findAction, &QAction::triggered, this, &TerminalView::showSearch);

    m_searchBar = new QFrame(this);
    m_searchBar->setObjectName(QStringLiteral("terminalSearchBar"));
    m_searchBar->setCursor(Qt::ArrowCursor);
    auto *searchLayout = new QHBoxLayout(m_searchBar);
    searchLayout->setContentsMargins(7, 4, 4, 4);
    searchLayout->setSpacing(2);
    m_searchInput = new QLineEdit(m_searchBar);
    m_searchInput->setObjectName(QStringLiteral("terminalSearchInput"));
    m_searchInput->setPlaceholderText(QStringLiteral("查找终端内容"));
    m_searchInput->setClearButtonEnabled(true);
    m_searchInput->installEventFilter(this);
    m_searchCounter = new QLabel(QStringLiteral("0 / 0"), m_searchBar);
    m_searchCounter->setObjectName(QStringLiteral("terminalSearchCounter"));
    m_searchCounter->setAlignment(Qt::AlignCenter);
    m_searchCounter->setMinimumWidth(48);
    m_searchPrevious = new QToolButton(m_searchBar);
    m_searchPrevious->setObjectName(QStringLiteral("terminalSearchPrevious"));
    m_searchPrevious->setText(QStringLiteral("↑"));
    m_searchPrevious->setToolTip(QStringLiteral("上一个匹配（Shift+Enter）"));
    m_searchNext = new QToolButton(m_searchBar);
    m_searchNext->setObjectName(QStringLiteral("terminalSearchNext"));
    m_searchNext->setText(QStringLiteral("↓"));
    m_searchNext->setToolTip(QStringLiteral("下一个匹配（Enter）"));
    m_searchClose = new QToolButton(m_searchBar);
    m_searchClose->setObjectName(QStringLiteral("terminalSearchClose"));
    m_searchClose->setText(QStringLiteral("×"));
    m_searchClose->setToolTip(QStringLiteral("关闭查找（Esc）"));
    for (auto *button : {m_searchPrevious, m_searchNext, m_searchClose}) button->setFixedSize(25, 25);
    searchLayout->addWidget(m_searchInput, 1);
    searchLayout->addWidget(m_searchCounter);
    searchLayout->addWidget(m_searchPrevious);
    searchLayout->addWidget(m_searchNext);
    searchLayout->addWidget(m_searchClose);
    m_searchBar->setFixedHeight(35);
    m_searchBar->hide();

    m_commandBlockTools = new QFrame(this);
    m_commandBlockTools->setObjectName(QStringLiteral("terminalCommandBlockTools"));
    m_commandBlockTools->setCursor(Qt::ArrowCursor);
    auto *blockToolsLayout = new QHBoxLayout(m_commandBlockTools);
    blockToolsLayout->setContentsMargins(3, 3, 3, 3);
    blockToolsLayout->setSpacing(2);
    m_copyCommandBlockText = new QToolButton(m_commandBlockTools);
    m_copyCommandBlockText->setObjectName(QStringLiteral("terminalCommandBlockCopyText"));
    m_copyCommandBlockText->setIcon(QIcon(QStringLiteral(":/assets/copy.svg")));
    m_copyCommandBlockText->setIconSize(QSize(16, 16));
    m_copyCommandBlockText->setToolTip(QStringLiteral("复制当前命令及输出"));
    m_copyCommandBlockImage = new QToolButton(m_commandBlockTools);
    m_copyCommandBlockImage->setObjectName(QStringLiteral("terminalCommandBlockCopyImage"));
    m_copyCommandBlockImage->setIcon(QIcon(QStringLiteral(":/assets/copy-image.svg")));
    m_copyCommandBlockImage->setIconSize(QSize(16, 16));
    m_copyCommandBlockImage->setToolTip(QStringLiteral("复制当前命令及输出为图片"));
    for (auto *button : {m_copyCommandBlockText, m_copyCommandBlockImage}) {
        button->setFixedSize(26, 24);
        blockToolsLayout->addWidget(button);
    }
    m_commandBlockTools->setFixedSize(62, 30);
    m_commandBlockTools->hide();

    connect(m_searchInput, &QLineEdit::textChanged, this, [this] {
        rebuildSearchMatches(false, true);
    });
    connect(m_searchPrevious, &QToolButton::clicked, this, &TerminalView::findPrevious);
    connect(m_searchNext, &QToolButton::clicked, this, &TerminalView::findNext);
    connect(m_searchClose, &QToolButton::clicked, this, &TerminalView::hideSearch);
    connect(m_copyCommandBlockText, &QToolButton::clicked,
        this, &TerminalView::copyHoveredCommandBlockText);
    connect(m_copyCommandBlockImage, &QToolButton::clicked,
        this, &TerminalView::copyHoveredCommandBlockImage);

    auto *findNextShortcut = new QShortcut(QKeySequence::FindNext, this);
    findNextShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(findNextShortcut, &QShortcut::activated, this, &TerminalView::findNext);
    auto *findPreviousShortcut = new QShortcut(QKeySequence::FindPrevious, this);
    findPreviousShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(findPreviousShortcut, &QShortcut::activated, this, &TerminalView::findPrevious);
}

TerminalAppearance TerminalView::defaultAppearance()
{
    return sharedDefaultAppearance();
}

void TerminalView::setDefaultAppearance(const TerminalAppearance &appearance)
{
    sharedDefaultAppearance() = normalizedAppearance(appearance);
}

void TerminalView::setAppearance(const TerminalAppearance &appearance)
{
    m_appearance = normalizedAppearance(appearance);
    m_font = QFont(m_appearance.fontFamily);
    m_font.setPointSize(m_appearance.pointSize);
    m_font.setStyleHint(QFont::Monospace);
    m_font.setFixedPitch(true);

    const QFontMetricsF metrics(m_font);
    m_cellWidth = qCeil(metrics.horizontalAdvance(QLatin1Char('M')));
    const qreal naturalHeight = metrics.height();
    m_cellHeight = qCeil(naturalHeight * m_appearance.lineSpacing);
    m_ascent = metrics.ascent() + qMax<qreal>(0.0, (m_cellHeight - naturalHeight) / 2.0);
    setMinimumSize(static_cast<int>(m_cellWidth * 20) + kContentLeft + kContentRight + kScrollBarWidth,
        static_cast<int>(m_cellHeight * 6) + kContentTop + kContentBottom);
    if (m_scrollBar) updateGridSize();
    update();
}

QPointF TerminalView::contentOrigin() const
{
    return {kContentLeft, kContentTop};
}

void TerminalView::feedData(const QByteArray &data)
{
    feedText(m_decoder(data));
}

void TerminalView::feedText(const QString &text)
{
    const bool followBottom = m_scrollBar->value() == m_scrollBar->maximum();
    m_model.feed(text);
    updateScrollBar(followBottom);
    if (m_searchBar->isVisible() && !m_searchInput->text().isEmpty()) {
        rebuildSearchMatches(true, false);
    }
    if (m_lastMousePosition.x() >= 0) updateHoveredCommandBlock(m_lastMousePosition);
    update();
}

void TerminalView::clear()
{
    m_decoder.resetState();
    m_model.clear();
    clearSelection();
    clearHoveredCommandBlock();
    rebuildSearchMatches(false, false);
    updateScrollBar(true);
    update();
}

bool TerminalView::event(QEvent *event)
{
    if (event->type() == QEvent::ShortcutOverride) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        const bool commandModifier = keyEvent->modifiers().testFlag(Qt::MetaModifier)
            || keyEvent->modifiers().testFlag(Qt::ControlModifier);
        if (commandModifier
            && (keyEvent->key() == Qt::Key_C || keyEvent->key() == Qt::Key_V
                || keyEvent->key() == Qt::Key_F)) {
            keyEvent->accept();
            return true;
        }
    }
    if (event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Tab || keyEvent->key() == Qt::Key_Backtab) {
            // Readline treats two Tab bytes differently from one. Forward only
            // the first press in each physical key-down cycle so platform key
            // repeat cannot unexpectedly expand hundreds of shell candidates.
            if (m_tabKeyDown || keyEvent->isAutoRepeat()) {
                keyEvent->accept();
                return true;
            }
            m_tabKeyDown = true;
            keyPressEvent(keyEvent);
            return true;
        }
    }
    if (event->type() == QEvent::KeyRelease) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Tab || keyEvent->key() == Qt::Key_Backtab) {
            if (!keyEvent->isAutoRepeat()) m_tabKeyDown = false;
            keyEvent->accept();
            return true;
        }
    }
    return QWidget::event(event);
}

bool TerminalView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_searchInput && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_F
            && (keyEvent->modifiers().testFlag(Qt::MetaModifier)
                || keyEvent->modifiers().testFlag(Qt::ControlModifier))) {
            m_searchInput->selectAll();
            return true;
        }
        if (keyEvent->key() == Qt::Key_Escape) {
            hideSearch();
            return true;
        }
        if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
            if (keyEvent->modifiers().testFlag(Qt::ShiftModifier)) findPrevious();
            else findNext();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void TerminalView::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    const QColor defaultForeground(QStringLiteral("#D0DBE5"));
    const QColor defaultBackground(QStringLiteral("#0C1825"));
    const bool reverseVideo = m_model.reverseVideo();
    painter.fillRect(rect(), reverseVideo ? defaultForeground : defaultBackground);
    painter.setClipRect(QRectF(kContentLeft, kContentTop,
        qMax(0, width() - kContentLeft - kContentRight - kScrollBarWidth),
        qMax(0, height() - kContentTop - kContentBottom)));
    painter.setFont(m_font);
    painter.setRenderHint(QPainter::TextAntialiasing);

    const int firstLine = m_scrollBar->value();
    int visibleMatchIndex = 0;
    while (visibleMatchIndex < m_searchMatches.size()
        && m_searchMatches.at(visibleMatchIndex).line < firstLine) {
        ++visibleMatchIndex;
    }
    for (int row = 0; row < m_model.rows(); ++row) {
        const int documentLine = firstLine + row;
        int rowMatchIndex = visibleMatchIndex;
        for (int column = 0; column < m_model.columns(); ++column) {
            const auto &cell = m_model.documentCell(documentLine, column);
            if (cell.wideContinuation) continue;
            QColor foreground = cell.inverse ? cell.background : cell.foreground;
            QColor background = cell.inverse ? cell.foreground : cell.background;
            if (reverseVideo) std::swap(foreground, background);
            const bool wide = column + 1 < m_model.columns() && m_model.documentCell(documentLine, column + 1).wideContinuation;
            const QRectF cellRect(kContentLeft + column * m_cellWidth, kContentTop + row * m_cellHeight,
                wide ? m_cellWidth * 2 : m_cellWidth, m_cellHeight);
            if (background != (reverseVideo ? defaultForeground : defaultBackground)) painter.fillRect(cellRect, background);
            if (isSelected(documentLine, column)) painter.fillRect(cellRect, QColor(49, 84, 119, 180));
            while (rowMatchIndex < m_searchMatches.size()
                && m_searchMatches.at(rowMatchIndex).line == documentLine
                && m_searchMatches.at(rowMatchIndex).endColumn <= column) {
                ++rowMatchIndex;
            }
            if (rowMatchIndex < m_searchMatches.size()) {
                const auto &match = m_searchMatches.at(rowMatchIndex);
                if (match.line == documentLine && column >= match.startColumn && column < match.endColumn) {
                    const bool current = rowMatchIndex == m_currentSearchMatch;
                    painter.fillRect(cellRect, current ? QColor(QStringLiteral("#FFB938"))
                                                       : QColor(QStringLiteral("#FFF36A")));
                    foreground = QColor(QStringLiteral("#17233D"));
                }
            }
            auto font = m_font;
            font.setBold(cell.bold);
            font.setUnderline(cell.underline);
            painter.setFont(font);
            painter.setPen(foreground);
            painter.drawText(QPointF(kContentLeft + column * m_cellWidth,
                kContentTop + row * m_cellHeight + m_ascent), cell.text);
        }
        while (visibleMatchIndex < m_searchMatches.size()
            && m_searchMatches.at(visibleMatchIndex).line <= documentLine) {
            ++visibleMatchIndex;
        }
    }

    if (m_hasFocus && m_model.cursorVisible() && firstLine == m_scrollBar->maximum()) {
        const QRectF cursorRect(kContentLeft + m_model.cursorColumn() * m_cellWidth,
            kContentTop + m_model.cursorRow() * m_cellHeight + 2.0,
            2.0, qMax(2.0, m_cellHeight - 4.0));
        painter.fillRect(cursorRect, QColor(QStringLiteral("#7DB1DE")));
    }

}

void TerminalView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    m_scrollBar->setGeometry(width() - kScrollBarWidth, 0, kScrollBarWidth, height());
    m_scrollBar->raise();
    positionSearchBar();
    positionCommandBlockTools();
    updateGridSize();
}

void TerminalView::positionSearchBar()
{
    if (!m_searchBar) return;
    const int barWidth = qBound(250, width() - 36, 390);
    m_searchBar->setGeometry(qMax(kContentLeft, width() - kScrollBarWidth - barWidth - 8),
        kContentTop, barWidth, m_searchBar->height());
    m_searchBar->raise();
}

void TerminalView::showSearch()
{
    if (!m_searchBar) return;
    clearHoveredCommandBlock();
    m_searchBar->show();
    positionSearchBar();
    rebuildSearchMatches(true, false);
    m_searchInput->setFocus(Qt::ShortcutFocusReason);
    m_searchInput->selectAll();
}

void TerminalView::hideSearch()
{
    if (!m_searchBar) return;
    m_searchBar->hide();
    m_searchMatches.clear();
    m_currentSearchMatch = -1;
    updateSearchCounter();
    setFocus(Qt::ShortcutFocusReason);
    if (m_lastMousePosition.x() >= 0) updateHoveredCommandBlock(m_lastMousePosition);
    update();
}

int TerminalView::columnForTextOffset(int line, int offset) const
{
    if (offset <= 0) return 0;
    int consumed = 0;
    for (int column = 0; column < m_model.columns(); ++column) {
        const auto &cell = m_model.documentCell(line, column);
        if (cell.wideContinuation) continue;
        if (consumed >= offset) return column;
        consumed += cell.text.size();
        const bool wide = column + 1 < m_model.columns()
            && m_model.documentCell(line, column + 1).wideContinuation;
        if (consumed >= offset) return qMin(m_model.columns(), column + (wide ? 2 : 1));
    }
    return m_model.columns();
}

void TerminalView::rebuildSearchMatches(bool preserveCurrent, bool revealCurrent)
{
    SearchMatch previous;
    const bool hadPrevious = preserveCurrent && m_currentSearchMatch >= 0
        && m_currentSearchMatch < m_searchMatches.size();
    if (hadPrevious) previous = m_searchMatches.at(m_currentSearchMatch);

    m_searchMatches.clear();
    const auto query = m_searchInput ? m_searchInput->text() : QString{};
    if (!query.isEmpty()) {
        for (int line = 0; line < m_model.documentLineCount(); ++line) {
            const auto text = m_model.documentLineText(line, false);
            int offset = 0;
            while ((offset = text.indexOf(query, offset, Qt::CaseInsensitive)) >= 0) {
                const int startColumn = columnForTextOffset(line, offset);
                const int endColumn = columnForTextOffset(line, offset + query.size());
                if (endColumn > startColumn) m_searchMatches.append({line, startColumn, endColumn});
                offset += qMax(1, query.size());
            }
        }
    }

    m_currentSearchMatch = -1;
    if (!m_searchMatches.isEmpty()) {
        if (hadPrevious) {
            for (int index = 0; index < m_searchMatches.size(); ++index) {
                const auto &match = m_searchMatches.at(index);
                if (match.line == previous.line && match.startColumn == previous.startColumn) {
                    m_currentSearchMatch = index;
                    break;
                }
            }
        }
        if (m_currentSearchMatch < 0) {
            const int firstVisibleLine = m_scrollBar ? m_scrollBar->value() : 0;
            for (int index = 0; index < m_searchMatches.size(); ++index) {
                if (m_searchMatches.at(index).line >= firstVisibleLine) {
                    m_currentSearchMatch = index;
                    break;
                }
            }
            if (m_currentSearchMatch < 0) m_currentSearchMatch = 0;
        }
    }
    updateSearchCounter();
    if (revealCurrent) scrollToCurrentSearchMatch();
    update();
}

void TerminalView::updateSearchCounter()
{
    if (!m_searchCounter) return;
    const bool hasMatches = !m_searchMatches.isEmpty() && m_currentSearchMatch >= 0;
    m_searchCounter->setText(hasMatches
        ? QStringLiteral("%1 / %2").arg(m_currentSearchMatch + 1).arg(m_searchMatches.size())
        : QStringLiteral("0 / 0"));
    if (m_searchPrevious) m_searchPrevious->setEnabled(hasMatches);
    if (m_searchNext) m_searchNext->setEnabled(hasMatches);
    updateSearchMarkers();
}

void TerminalView::updateSearchMarkers()
{
    if (!m_scrollBar) return;
    if (m_searchMatches.isEmpty()) {
        m_scrollBar->clearSearchMarkers();
        return;
    }
    QVector<qreal> positions;
    positions.reserve(m_searchMatches.size());
    const int maximumLine = qMax(1, m_model.documentLineCount() - 1);
    for (const auto &match : std::as_const(m_searchMatches)) {
        positions.append(qBound<qreal>(0.0, qreal(match.line) / maximumLine, 1.0));
    }
    m_scrollBar->setSearchMarkers(positions, m_currentSearchMatch);
}

void TerminalView::scrollToCurrentSearchMatch()
{
    if (!m_scrollBar || m_currentSearchMatch < 0 || m_currentSearchMatch >= m_searchMatches.size()) return;
    const int line = m_searchMatches.at(m_currentSearchMatch).line;
    const int first = m_scrollBar->value();
    const int last = first + m_model.rows() - 1;
    if (line < first) m_scrollBar->setValue(line);
    else if (line > last) m_scrollBar->setValue(line - m_model.rows() + 1);
}

void TerminalView::findNext()
{
    if (!m_searchBar->isVisible()) {
        showSearch();
        return;
    }
    if (m_searchMatches.isEmpty()) rebuildSearchMatches(false, false);
    if (m_searchMatches.isEmpty()) return;
    m_currentSearchMatch = (m_currentSearchMatch + 1) % m_searchMatches.size();
    updateSearchCounter();
    scrollToCurrentSearchMatch();
    update();
}

void TerminalView::findPrevious()
{
    if (!m_searchBar->isVisible()) {
        showSearch();
        return;
    }
    if (m_searchMatches.isEmpty()) rebuildSearchMatches(false, false);
    if (m_searchMatches.isEmpty()) return;
    m_currentSearchMatch = (m_currentSearchMatch - 1 + m_searchMatches.size()) % m_searchMatches.size();
    updateSearchCounter();
    scrollToCurrentSearchMatch();
    update();
}

TerminalView::CommandBlock TerminalView::commandBlockAt(const QPointF &position) const
{
    CommandBlock block;
    if (!m_scrollBar || (m_searchBar && m_searchBar->isVisible())) return block;
    const QRectF contentRect(kContentLeft, kContentTop,
        qMax(0, width() - kContentLeft - kContentRight - kScrollBarWidth),
        qMax(0, height() - kContentTop - kContentBottom));
    if (!contentRect.contains(position)) return block;

    const int hoveredLine = documentPosition(position).y();
    for (int line = hoveredLine; line >= 0; --line) {
        const auto text = m_model.documentLineText(line);
        const auto match = shellPromptExpression().match(text);
        if (!match.hasMatch()) continue;
        // A bare prompt starts a new, not-yet-executed region. It must not
        // make the previous command appear hovered.
        if (text.mid(match.capturedEnd()).trimmed().isEmpty()) return block;
        block.startLine = line;
        break;
    }
    if (block.startLine < 0) return {};

    block.endLine = m_model.documentLineCount() - 1;
    for (int line = block.startLine + 1; line < m_model.documentLineCount(); ++line) {
        if (shellPromptExpression().match(m_model.documentLineText(line)).hasMatch()) {
            block.endLine = line - 1;
            break;
        }
    }
    while (block.endLine > block.startLine
        && m_model.documentLineText(block.endLine).trimmed().isEmpty()) {
        --block.endLine;
    }
    if (hoveredLine < block.startLine || hoveredLine > block.endLine) return {};

    const int firstVisible = m_scrollBar->value();
    const int lastVisible = firstVisible + m_model.rows() - 1;
    const int visibleStart = qMax(block.startLine, firstVisible);
    const int visibleEnd = qMin(block.endLine, lastVisible);
    if (visibleEnd < visibleStart) return {};
    const qreal top = kContentTop + (visibleStart - firstVisible) * m_cellHeight;
    const qreal bottom = kContentTop + (visibleEnd - firstVisible + 1) * m_cellHeight;
    block.visibleRect = QRectF(kContentLeft - 5, qMax<qreal>(kContentTop, top - 2),
        width() - (kContentLeft - 5) - kContentRight - kScrollBarWidth,
        qMin<qreal>(height() - kContentBottom, bottom + 2) - qMax<qreal>(kContentTop, top - 2));
    return block;
}

void TerminalView::updateHoveredCommandBlock(const QPointF &position)
{
    m_lastMousePosition = position;
    const auto block = commandBlockAt(position);
    const bool changed = block.startLine != m_hoveredCommandBlock.startLine
        || block.endLine != m_hoveredCommandBlock.endLine
        || block.visibleRect != m_hoveredCommandBlock.visibleRect;
    m_hoveredCommandBlock = block;
    positionCommandBlockTools();
    if (changed) update();
}

void TerminalView::clearHoveredCommandBlock()
{
    if (!m_hoveredCommandBlock.isValid() && (!m_commandBlockTools || !m_commandBlockTools->isVisible())) return;
    m_hoveredCommandBlock = {};
    if (m_commandBlockTools) m_commandBlockTools->hide();
    update();
}

void TerminalView::positionCommandBlockTools()
{
    if (!m_commandBlockTools || !m_hoveredCommandBlock.isValid()
        || m_hoveredCommandBlock.visibleRect.isEmpty()
        || (m_searchBar && m_searchBar->isVisible())) {
        if (m_commandBlockTools) m_commandBlockTools->hide();
        return;
    }
    const auto &hoverRect = m_hoveredCommandBlock.visibleRect;
    const int x = qMax(kContentLeft,
        qRound(hoverRect.right()) - m_commandBlockTools->width() - 5);
    const int y = qBound(kContentTop, qRound(hoverRect.top()) + 3,
        qMax(kContentTop, height() - kContentBottom - m_commandBlockTools->height()));
    m_commandBlockTools->move(x, y);
    m_commandBlockTools->show();
    m_commandBlockTools->raise();
}

QString TerminalView::commandBlockText(const CommandBlock &block) const
{
    if (!block.isValid()) return {};
    QStringList lines;
    lines.reserve(block.endLine - block.startLine + 1);
    for (int line = block.startLine; line <= block.endLine; ++line) {
        lines.append(m_model.documentLineText(line));
    }
    while (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
    return lines.join(QLatin1Char('\n'));
}

QImage TerminalView::commandBlockImage(const CommandBlock &block) const
{
    if (!block.isValid()) return {};
    constexpr int imagePadding = 10;
    constexpr int maximumRenderedLines = 300;
    const int lineCount = qMin(maximumRenderedLines, block.endLine - block.startLine + 1);
    const int imageWidth = qMax(1,
        width() - kContentLeft - kContentRight - kScrollBarWidth + imagePadding * 2);
    const int imageHeight = qMax(1, qCeil(lineCount * m_cellHeight) + imagePadding * 2);
    QImage image(imageWidth, imageHeight, QImage::Format_ARGB32_Premultiplied);
    const QColor defaultForeground(QStringLiteral("#D0DBE5"));
    const QColor defaultBackground(QStringLiteral("#0C1825"));
    const bool reverseVideo = m_model.reverseVideo();
    image.fill(reverseVideo ? defaultForeground : defaultBackground);

    QPainter painter(&image);
    painter.setFont(m_font);
    painter.setRenderHint(QPainter::TextAntialiasing);
    for (int row = 0; row < lineCount; ++row) {
        const int documentLine = block.startLine + row;
        for (int column = 0; column < m_model.columns(); ++column) {
            const auto &cell = m_model.documentCell(documentLine, column);
            if (cell.wideContinuation) continue;
            QColor foreground = cell.inverse ? cell.background : cell.foreground;
            QColor background = cell.inverse ? cell.foreground : cell.background;
            if (reverseVideo) std::swap(foreground, background);
            const bool wide = column + 1 < m_model.columns()
                && m_model.documentCell(documentLine, column + 1).wideContinuation;
            const QRectF cellRect(imagePadding + column * m_cellWidth,
                imagePadding + row * m_cellHeight,
                wide ? m_cellWidth * 2 : m_cellWidth, m_cellHeight);
            if (background != (reverseVideo ? defaultForeground : defaultBackground)) {
                painter.fillRect(cellRect, background);
            }
            auto font = m_font;
            font.setBold(cell.bold);
            font.setUnderline(cell.underline);
            painter.setFont(font);
            painter.setPen(foreground);
            painter.drawText(QPointF(imagePadding + column * m_cellWidth,
                imagePadding + row * m_cellHeight + m_ascent), cell.text);
        }
    }
    return image;
}

void TerminalView::copyHoveredCommandBlockText()
{
    const auto text = commandBlockText(m_hoveredCommandBlock);
    if (!text.isEmpty()) QApplication::clipboard()->setText(text);
}

void TerminalView::copyHoveredCommandBlockImage()
{
    const auto image = commandBlockImage(m_hoveredCommandBlock);
    if (!image.isNull()) QApplication::clipboard()->setImage(image);
}

void TerminalView::updateGridSize()
{
    const int contentWidth = qMax(1, width() - kContentLeft - kContentRight - kScrollBarWidth);
    const int contentHeight = qMax(1, height() - kContentTop - kContentBottom);
    const int columns = qMax(2, static_cast<int>(contentWidth / m_cellWidth));
    const int rows = qMax(2, static_cast<int>(contentHeight / m_cellHeight));
    if (columns == m_model.columns() && rows == m_model.rows()) return;
    m_model.resize(columns, rows);
    updateScrollBar(true);
    emit terminalSizeChanged(columns, rows, contentWidth, contentHeight);
    update();
}

QByteArray TerminalView::keySequence(QKeyEvent *event) const
{
    switch (event->key()) {
    case Qt::Key_Return: case Qt::Key_Enter: return "\r";
    case Qt::Key_Backspace: return QByteArray(1, '\x7f');
    case Qt::Key_Tab: return "\t";
    case Qt::Key_Backtab: return "\x1b[Z";
    case Qt::Key_Escape: return "\x1b";
    case Qt::Key_Up: return m_model.applicationCursorKeys() ? "\x1bOA" : "\x1b[A";
    case Qt::Key_Down: return m_model.applicationCursorKeys() ? "\x1bOB" : "\x1b[B";
    case Qt::Key_Right: return m_model.applicationCursorKeys() ? "\x1bOC" : "\x1b[C";
    case Qt::Key_Left: return m_model.applicationCursorKeys() ? "\x1bOD" : "\x1b[D";
    case Qt::Key_Home: return "\x1b[H";
    case Qt::Key_End: return "\x1b[F";
    case Qt::Key_Insert: return "\x1b[2~";
    case Qt::Key_Delete: return "\x1b[3~";
    case Qt::Key_PageUp: return "\x1b[5~";
    case Qt::Key_PageDown: return "\x1b[6~";
    case Qt::Key_F1: return "\x1bOP";
    case Qt::Key_F2: return "\x1bOQ";
    case Qt::Key_F3: return "\x1bOR";
    case Qt::Key_F4: return "\x1bOS";
    case Qt::Key_F5: return "\x1b[15~";
    case Qt::Key_F6: return "\x1b[17~";
    case Qt::Key_F7: return "\x1b[18~";
    case Qt::Key_F8: return "\x1b[19~";
    case Qt::Key_F9: return "\x1b[20~";
    case Qt::Key_F10: return "\x1b[21~";
    case Qt::Key_F11: return "\x1b[23~";
    case Qt::Key_F12: return "\x1b[24~";
    default: break;
    }
    if (event->modifiers().testFlag(Qt::ControlModifier)
        && event->key() >= Qt::Key_A && event->key() <= Qt::Key_Z) {
        return QByteArray(1, static_cast<char>(event->key() - Qt::Key_A + 1));
    }
    if (!event->text().isEmpty() && !event->modifiers().testFlag(Qt::ControlModifier)
        && !event->modifiers().testFlag(Qt::MetaModifier)) {
        return event->text().toUtf8();
    }
    return {};
}

void TerminalView::keyPressEvent(QKeyEvent *event)
{
    const bool metaModifier = event->modifiers().testFlag(Qt::MetaModifier);
    const bool controlModifier = event->modifiers().testFlag(Qt::ControlModifier);
    const bool terminalShortcutModifier = metaModifier
        || (controlModifier && event->modifiers().testFlag(Qt::ShiftModifier));
    if (event->key() == Qt::Key_F && (metaModifier || controlModifier)) {
        showSearch();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_C && (metaModifier || controlModifier)) {
        if (hasSelection()) copySelection();
        else sendInterrupt();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_V && (terminalShortcutModifier || controlModifier)) {
        sendPaste(QApplication::clipboard()->text());
        event->accept();
        return;
    }
    const auto sequence = keySequence(event);
    if (!sequence.isEmpty()) {
        clearSelection();
        trackKeyForCommand(event);
        emit inputGenerated(sequence);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void TerminalView::contextMenuEvent(QContextMenuEvent *event)
{
    if (!m_contextMenu) return;
    m_copyAction->setEnabled(hasSelection());
    m_pasteAction->setEnabled(isEnabled() && !QApplication::clipboard()->text().isEmpty());
    m_selectAllAction->setEnabled(!plainText().isEmpty());
    m_contextMenu->popup(event->globalPos());
    event->accept();
}

void TerminalView::trackKeyForCommand(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        submitTrackedCommand();
        return;
    }
    if (event->key() == Qt::Key_Backspace) {
        if (m_pendingCommandCursor > 0) {
            m_pendingCommand.remove(m_pendingCommandCursor - 1, 1);
            --m_pendingCommandCursor;
        }
        return;
    }
    if (event->key() == Qt::Key_Delete) {
        if (m_pendingCommandCursor < m_pendingCommand.size()) m_pendingCommand.remove(m_pendingCommandCursor, 1);
        return;
    }
    if (event->key() == Qt::Key_Left) {
        m_pendingCommandCursor = qMax(0, m_pendingCommandCursor - 1);
        return;
    }
    if (event->key() == Qt::Key_Right) {
        m_pendingCommandCursor = qMin(m_pendingCommand.size(), m_pendingCommandCursor + 1);
        return;
    }
    if (event->key() == Qt::Key_Home
        || (event->key() == Qt::Key_A && event->modifiers().testFlag(Qt::ControlModifier))) {
        m_pendingCommandCursor = 0;
        return;
    }
    if (event->key() == Qt::Key_End
        || (event->key() == Qt::Key_E && event->modifiers().testFlag(Qt::ControlModifier))) {
        m_pendingCommandCursor = m_pendingCommand.size();
        return;
    }
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        if (event->key() == Qt::Key_C) {
            m_pendingCommand.clear();
            m_pendingCommandCursor = 0;
        } else if (event->key() == Qt::Key_U) {
            m_pendingCommand.remove(0, m_pendingCommandCursor);
            m_pendingCommandCursor = 0;
        } else if (event->key() == Qt::Key_K) {
            m_pendingCommand.truncate(m_pendingCommandCursor);
        }
        return;
    }
    if (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab
        || event->key() == Qt::Key_Up || event->key() == Qt::Key_Down) {
        m_commandTrackingReliable = false;
        return;
    }
    if (!event->text().isEmpty() && !event->modifiers().testFlag(Qt::MetaModifier)) {
        m_pendingCommand.insert(m_pendingCommandCursor, event->text());
        m_pendingCommandCursor += event->text().size();
    }
}

void TerminalView::trackTextForCommand(const QString &text)
{
    for (const auto character : text) {
        if (character == QLatin1Char('\r') || character == QLatin1Char('\n')) {
            submitTrackedCommand();
        } else {
            m_pendingCommand.insert(m_pendingCommandCursor, character);
            ++m_pendingCommandCursor;
        }
    }
}

void TerminalView::submitTrackedCommand()
{
    auto command = m_pendingCommand;
    if (!m_commandTrackingReliable) {
        const auto screenCommand = commandFromCurrentScreenLine();
        if (!screenCommand.isEmpty()) command = screenCommand;
    }
    emit commandSubmitted(command);
    m_pendingCommand.clear();
    m_pendingCommandCursor = 0;
    m_commandTrackingReliable = true;
}

QString TerminalView::commandFromCurrentScreenLine() const
{
    const int lineIndex = m_model.historyLineCount() + m_model.cursorRow();
    auto line = m_model.documentLineText(lineIndex, false).left(m_model.cursorColumn());
    static const QRegularExpression promptExpression(
        QStringLiteral("(?:\\[[^\\]]+\\][#$]|[^\\s]+@[^\\s]+:[^\\s]*[#$]|[#$])\\s+"));
    auto iterator = promptExpression.globalMatch(line);
    int commandStart = -1;
    while (iterator.hasNext()) commandStart = iterator.next().capturedEnd();
    return commandStart >= 0 ? line.mid(commandStart) : QString{};
}

void TerminalView::updateScrollBar(bool followBottom)
{
    const int maximum = qMax(0, m_model.documentLineCount() - m_model.rows());
    m_scrollBar->setPageStep(m_model.rows());
    m_scrollBar->setRange(0, maximum);
    if (followBottom) m_scrollBar->setValue(maximum);
}

QPoint TerminalView::documentPosition(const QPointF &position) const
{
    const int column = qBound(0, static_cast<int>((position.x() - kContentLeft) / m_cellWidth), m_model.columns() - 1);
    const int visibleRow = qBound(0, static_cast<int>((position.y() - kContentTop) / m_cellHeight), m_model.rows() - 1);
    return {column, qMin(m_model.documentLineCount() - 1, m_scrollBar->value() + visibleRow)};
}

bool TerminalView::hasSelection() const
{
    return m_selectionAnchor.x() >= 0 && m_selectionCursor.x() >= 0 && m_selectionAnchor != m_selectionCursor;
}

bool TerminalView::isSelected(int line, int column) const
{
    if (!hasSelection()) return false;
    auto start = m_selectionAnchor;
    auto end = m_selectionCursor;
    if (start.y() > end.y() || (start.y() == end.y() && start.x() > end.x())) std::swap(start, end);
    if (line < start.y() || line > end.y()) return false;
    if (start.y() == end.y()) return column >= start.x() && column < end.x();
    if (line == start.y()) return column >= start.x();
    if (line == end.y()) return column < end.x();
    return true;
}

QString TerminalView::selectedText() const
{
    if (!hasSelection()) return {};
    auto start = m_selectionAnchor;
    auto end = m_selectionCursor;
    if (start.y() > end.y() || (start.y() == end.y() && start.x() > end.x())) std::swap(start, end);
    QStringList lines;
    for (int line = start.y(); line <= end.y(); ++line) {
        QString text = m_model.documentLineText(line, false);
        const int from = line == start.y() ? start.x() : 0;
        const int to = line == end.y() ? end.x() : m_model.columns();
        text = text.mid(from, qMax(0, to - from));
        while (text.endsWith(QLatin1Char(' '))) text.chop(1);
        lines.append(text);
    }
    return lines.join(QLatin1Char('\n'));
}

bool TerminalView::shouldReportMouse(Qt::KeyboardModifiers modifiers) const
{
    return m_model.mouseTracking() != VtTerminalModel::MouseTracking::None
        && !modifiers.testFlag(Qt::ShiftModifier);
}

QByteArray TerminalView::mouseReport(int button, const QPointF &position, bool release) const
{
    const int column = qBound(0, static_cast<int>((position.x() - kContentLeft) / m_cellWidth), m_model.columns() - 1) + 1;
    const int row = qBound(0, static_cast<int>((position.y() - kContentTop) / m_cellHeight), m_model.rows() - 1) + 1;
    const char final = release ? 'm' : 'M';
    if (m_model.sgrMouseEncoding()) {
        return QByteArray("\x1b[<") + QByteArray::number(button) + ';' + QByteArray::number(column) + ';'
            + QByteArray::number(row) + final;
    }
    return QByteArray("\x1b[M") + QByteArray(1, static_cast<char>(32 + button))
        + QByteArray(1, static_cast<char>(32 + qMin(column, 223))) + QByteArray(1, static_cast<char>(32 + qMin(row, 223)));
}

void TerminalView::mousePressEvent(QMouseEvent *event)
{
    setFocus();
    if (event->button() == Qt::RightButton) {
        // Reserve right-click for the local copy/paste menu, even when the
        // remote application has enabled terminal mouse reporting.
        event->accept();
        return;
    }
    const int button = event->button() == Qt::LeftButton ? 0 : event->button() == Qt::MiddleButton ? 1 : 2;
    if (shouldReportMouse(event->modifiers())) {
        m_pressedMouseButton = button;
        emit inputGenerated(mouseReport(button, event->position()));
    } else if (event->button() == Qt::LeftButton) {
        m_selectionAnchor = documentPosition(event->position());
        m_selectionCursor = m_selectionAnchor;
        m_selecting = true;
        update();
    }
    event->accept();
}

void TerminalView::mouseMoveEvent(QMouseEvent *event)
{
    updateHoveredCommandBlock(event->position());
    if (shouldReportMouse(event->modifiers())) {
        const auto tracking = m_model.mouseTracking();
        if (tracking == VtTerminalModel::MouseTracking::AnyMotion
            || (tracking == VtTerminalModel::MouseTracking::ButtonMotion && m_pressedMouseButton >= 0)) {
            const int button = (m_pressedMouseButton >= 0 ? m_pressedMouseButton : 3) + 32;
            emit inputGenerated(mouseReport(button, event->position()));
        }
    } else if (m_selecting) {
        m_selectionCursor = documentPosition(event->position());
        update();
    }
    event->accept();
}

void TerminalView::leaveEvent(QEvent *event)
{
    // Moving from the terminal canvas onto its child overlay can produce a
    // leave event on some platforms. Keep the command tools alive while the
    // pointer is still anywhere inside this widget's bounds.
    if (rect().contains(mapFromGlobal(QCursor::pos()))) {
        QWidget::leaveEvent(event);
        return;
    }
    m_lastMousePosition = {-1, -1};
    clearHoveredCommandBlock();
    QWidget::leaveEvent(event);
}

void TerminalView::mouseReleaseEvent(QMouseEvent *event)
{
    if (shouldReportMouse(event->modifiers()) && m_pressedMouseButton >= 0) {
        emit inputGenerated(mouseReport(m_pressedMouseButton, event->position(), true));
        m_pressedMouseButton = -1;
    }
    m_selecting = false;
    event->accept();
}

void TerminalView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (shouldReportMouse(event->modifiers())) {
        mousePressEvent(event);
        return;
    }
    const auto position = documentPosition(event->position());
    const auto line = m_model.documentLineText(position.y(), false);
    auto isWord = [](QChar character) { return character.isLetterOrNumber() || QStringLiteral("_-./").contains(character); };
    int start = qMin(position.x(), line.size());
    int end = start;
    while (start > 0 && isWord(line.at(start - 1))) --start;
    while (end < line.size() && isWord(line.at(end))) ++end;
    m_selectionAnchor = {start, position.y()};
    m_selectionCursor = {end, position.y()};
    update();
    event->accept();
}

void TerminalView::wheelEvent(QWheelEvent *event)
{
    if (shouldReportMouse(event->modifiers())) {
        const int button = event->angleDelta().y() > 0 ? 64 : 65;
        emit inputGenerated(mouseReport(button, event->position()));
    } else {
        const int steps = event->angleDelta().y() / 120;
        m_scrollBar->setValue(m_scrollBar->value() - steps * 3);
    }
    event->accept();
}

void TerminalView::sendPaste(const QString &text)
{
    if (text.isEmpty()) return;
    clearSelection();
    trackTextForCommand(text);
    auto payload = text.toUtf8();
    if (m_model.bracketedPaste()) payload = QByteArray("\x1b[200~") + payload + QByteArray("\x1b[201~");
    emit inputGenerated(payload);
}

void TerminalView::sendInterrupt()
{
    clearSelection();
    m_pendingCommand.clear();
    m_pendingCommandCursor = 0;
    m_commandTrackingReliable = true;
    emit inputGenerated(QByteArray(1, '\x03'));
}

void TerminalView::copySelection()
{
    if (hasSelection()) QApplication::clipboard()->setText(selectedText());
}

void TerminalView::selectAllText()
{
    if (m_model.documentLineCount() <= 0) return;
    const int lastLine = m_model.documentLineCount() - 1;
    m_selectionAnchor = {0, 0};
    m_selectionCursor = {static_cast<int>(m_model.documentLineText(lastLine, false).size()), lastLine};
    update();
}

void TerminalView::focusInEvent(QFocusEvent *event)
{
    m_hasFocus = true;
    update();
    QWidget::focusInEvent(event);
}

void TerminalView::focusOutEvent(QFocusEvent *event)
{
    m_hasFocus = false;
    m_tabKeyDown = false;
    update();
    QWidget::focusOutEvent(event);
}

void TerminalView::inputMethodEvent(QInputMethodEvent *event)
{
    if (!event->commitString().isEmpty()) {
        clearSelection();
        trackTextForCommand(event->commitString());
        emit inputGenerated(event->commitString().toUtf8());
    }
    event->accept();
}

void TerminalView::clearSelection()
{
    if (!hasSelection() && m_selectionAnchor.x() < 0 && m_selectionCursor.x() < 0) return;
    m_selectionAnchor = {-1, -1};
    m_selectionCursor = {-1, -1};
    m_selecting = false;
    update();
}

QVariant TerminalView::inputMethodQuery(Qt::InputMethodQuery query) const
{
    if (query == Qt::ImEnabled) return true;
    if (query == Qt::ImCursorRectangle) {
        return QRectF(kContentLeft + m_model.cursorColumn() * m_cellWidth,
            kContentTop + m_model.cursorRow() * m_cellHeight, m_cellWidth, m_cellHeight);
    }
    return QWidget::inputMethodQuery(query);
}

} // namespace noxshell::ui
