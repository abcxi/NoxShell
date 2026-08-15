#include "TerminalView.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStringList>
#include <QTimer>
#include <QWheelEvent>

#include <utility>

namespace noxshell::ui {

TerminalView::TerminalView(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("terminalOutput"));
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled);
    setAutoFillBackground(false);
    m_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_font.setPointSize(12);
    m_font.setStyleHint(QFont::Monospace);
    m_font.setFixedPitch(true);
    const QFontMetricsF metrics(m_font);
    m_cellWidth = qCeil(metrics.horizontalAdvance(QLatin1Char('M')));
    m_cellHeight = qCeil(metrics.height() + 1.0);
    m_ascent = metrics.ascent();
    setMinimumSize(static_cast<int>(m_cellWidth * 20), static_cast<int>(m_cellHeight * 6));
    setCursor(Qt::IBeamCursor);
    m_scrollBar = new QScrollBar(Qt::Vertical, this);
    m_scrollBar->setObjectName(QStringLiteral("terminalScrollBar"));
    m_scrollBar->setSingleStep(1);
    m_scrollBar->setPageStep(m_model.rows());
    connect(m_scrollBar, &QScrollBar::valueChanged, this, [this] { update(); });

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
    connect(m_copyAction, &QAction::triggered, this, [this] {
        copySelection();
        QTimer::singleShot(0, this, [this] { setFocus(); });
    });
    connect(m_pasteAction, &QAction::triggered, this, [this] {
        sendPaste(QApplication::clipboard()->text());
        QTimer::singleShot(0, this, [this] { setFocus(); });
    });
    connect(m_selectAllAction, &QAction::triggered, this, &TerminalView::selectAllText);
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
    update();
}

void TerminalView::clear()
{
    m_decoder.resetState();
    m_model.clear();
    clearSelection();
    updateScrollBar(true);
    update();
}

bool TerminalView::event(QEvent *event)
{
    if (event->type() == QEvent::ShortcutOverride) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        const bool commandModifier = keyEvent->modifiers().testFlag(Qt::MetaModifier)
            || keyEvent->modifiers().testFlag(Qt::ControlModifier);
        if (commandModifier && (keyEvent->key() == Qt::Key_C || keyEvent->key() == Qt::Key_V)) {
            keyEvent->accept();
            return true;
        }
    }
    if (event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Tab || keyEvent->key() == Qt::Key_Backtab) {
            keyPressEvent(keyEvent);
            return true;
        }
    }
    return QWidget::event(event);
}

void TerminalView::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    const QColor defaultForeground(QStringLiteral("#D0DBE5"));
    const QColor defaultBackground(QStringLiteral("#0C1825"));
    const bool reverseVideo = m_model.reverseVideo();
    painter.fillRect(rect(), reverseVideo ? defaultForeground : defaultBackground);
    painter.setFont(m_font);
    painter.setRenderHint(QPainter::TextAntialiasing);

    const int firstLine = m_scrollBar->value();
    for (int row = 0; row < m_model.rows(); ++row) {
        const int documentLine = firstLine + row;
        for (int column = 0; column < m_model.columns(); ++column) {
            const auto &cell = m_model.documentCell(documentLine, column);
            if (cell.wideContinuation) continue;
            QColor foreground = cell.inverse ? cell.background : cell.foreground;
            QColor background = cell.inverse ? cell.foreground : cell.background;
            if (reverseVideo) std::swap(foreground, background);
            const bool wide = column + 1 < m_model.columns() && m_model.documentCell(documentLine, column + 1).wideContinuation;
            const QRectF cellRect(column * m_cellWidth, row * m_cellHeight,
                wide ? m_cellWidth * 2 : m_cellWidth, m_cellHeight);
            if (background != (reverseVideo ? defaultForeground : defaultBackground)) painter.fillRect(cellRect, background);
            if (isSelected(documentLine, column)) painter.fillRect(cellRect, QColor(49, 84, 119, 180));
            auto font = m_font;
            font.setBold(cell.bold);
            font.setUnderline(cell.underline);
            painter.setFont(font);
            painter.setPen(foreground);
            painter.drawText(QPointF(column * m_cellWidth, row * m_cellHeight + m_ascent), cell.text);
        }
    }

    if (m_hasFocus && m_model.cursorVisible() && firstLine == m_scrollBar->maximum()) {
        const QRectF cursorRect(m_model.cursorColumn() * m_cellWidth,
            m_model.cursorRow() * m_cellHeight + 2.0, 2.0, qMax(2.0, m_cellHeight - 4.0));
        painter.fillRect(cursorRect, QColor(QStringLiteral("#7DB1DE")));
    }
}

void TerminalView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    m_scrollBar->setGeometry(width() - 10, 0, 10, height());
    m_scrollBar->raise();
    updateGridSize();
}

void TerminalView::updateGridSize()
{
    const int columns = qMax(2, static_cast<int>(width() / m_cellWidth));
    const int rows = qMax(2, static_cast<int>(height() / m_cellHeight));
    if (columns == m_model.columns() && rows == m_model.rows()) return;
    m_model.resize(columns, rows);
    updateScrollBar(true);
    emit terminalSizeChanged(columns, rows, width(), height());
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
    case Qt::Key_Up: return "\x1b[A";
    case Qt::Key_Down: return "\x1b[B";
    case Qt::Key_Right: return "\x1b[C";
    case Qt::Key_Left: return "\x1b[D";
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
    const int column = qBound(0, static_cast<int>(position.x() / m_cellWidth), m_model.columns() - 1);
    const int visibleRow = qBound(0, static_cast<int>(position.y() / m_cellHeight), m_model.rows() - 1);
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
    const int column = qBound(0, static_cast<int>(position.x() / m_cellWidth), m_model.columns() - 1) + 1;
    const int row = qBound(0, static_cast<int>(position.y() / m_cellHeight), m_model.rows() - 1) + 1;
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
        return QRectF(m_model.cursorColumn() * m_cellWidth, m_model.cursorRow() * m_cellHeight, m_cellWidth, m_cellHeight);
    }
    return QWidget::inputMethodQuery(query);
}

} // namespace noxshell::ui
