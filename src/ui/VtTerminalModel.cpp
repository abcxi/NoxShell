#include "VtTerminalModel.h"

#include <QStringList>

#include <algorithm>

namespace noxshell::ui {

namespace {
const TerminalCell kFallbackCell{};
}

VtTerminalModel::VtTerminalModel(int columns, int rows)
{
    resize(columns, rows);
    reset();
}

void VtTerminalModel::resetAttributes()
{
    m_attributes = {};
}

TerminalCell VtTerminalModel::blankCell() const
{
    TerminalCell cell;
    cell.foreground = m_attributes.foreground;
    cell.background = m_attributes.background;
    return cell;
}

void VtTerminalModel::reset()
{
    resetAttributes();
    m_cursorColumn = 0;
    m_cursorRow = 0;
    m_savedColumn = 0;
    m_savedRow = 0;
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    m_wrapPending = false;
    m_cursorVisible = true;
    m_reverseVideo = false;
    m_bracketedPaste = false;
    m_mouseTracking = MouseTracking::None;
    m_sgrMouseEncoding = false;
    m_scrollback.clear();
    m_parserState = ParserState::Ground;
    m_csiBuffer.clear();
    clear();
}

void VtTerminalModel::clear()
{
    m_scrollback.clear();
    m_cells.fill(blankCell(), m_columns * m_rows);
    m_cursorColumn = 0;
    m_cursorRow = 0;
    m_wrapPending = false;
}

void VtTerminalModel::resize(int columns, int rows)
{
    columns = qMax(2, columns);
    rows = qMax(2, rows);
    if (columns == m_columns && rows == m_rows) return;
    QVector<TerminalCell> resized(columns * rows, blankCell());
    const int copiedRows = qMin(rows, m_rows);
    const int copiedColumns = qMin(columns, m_columns);
    for (int row = 0; row < copiedRows; ++row) {
        for (int column = 0; column < copiedColumns; ++column) {
            resized[row * columns + column] = m_cells.value(index(row, column));
        }
    }
    m_columns = columns;
    m_rows = rows;
    m_cells = std::move(resized);
    m_scrollTop = 0;
    m_scrollBottom = rows - 1;
    clampCursor();
}

const TerminalCell &VtTerminalModel::cell(int row, int column) const
{
    if (row < 0 || row >= m_rows || column < 0 || column >= m_columns) return kFallbackCell;
    return m_cells.at(index(row, column));
}

const TerminalCell &VtTerminalModel::documentCell(int line, int column) const
{
    if (column < 0 || column >= m_columns || line < 0 || line >= documentLineCount()) return kFallbackCell;
    if (line < m_scrollback.size()) {
        const auto &historyLine = m_scrollback.at(line);
        return column < historyLine.size() ? historyLine.at(column) : kFallbackCell;
    }
    return cell(line - m_scrollback.size(), column);
}

QString VtTerminalModel::documentLineText(int line, bool trimRight) const
{
    QString result;
    for (int column = 0; column < m_columns; ++column) {
        const auto &current = documentCell(line, column);
        if (!current.wideContinuation) result += current.text;
    }
    if (trimRight) while (result.endsWith(QLatin1Char(' '))) result.chop(1);
    return result;
}

QString VtTerminalModel::plainText() const
{
    QStringList lines;
    lines.reserve(documentLineCount());
    for (int line = 0; line < documentLineCount(); ++line) lines.append(documentLineText(line));
    while (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
    return lines.join(QLatin1Char('\n'));
}

void VtTerminalModel::feed(const QString &text)
{
    const auto codePoints = text.toUcs4();
    for (char32_t character : codePoints) processCharacter(character);
}

void VtTerminalModel::processCharacter(char32_t character)
{
    switch (m_parserState) {
    case ParserState::Ground:
        if (character == 0x1b) m_parserState = ParserState::Escape;
        else if (character < 0x20 || character == 0x7f) processControl(character);
        else putCharacter(character);
        break;
    case ParserState::Escape:
        processEscape(character);
        break;
    case ParserState::Csi:
        if (character == '?' && m_csiBuffer.isEmpty()) m_csiPrivate = true;
        else if (character >= 0x40 && character <= 0x7e) {
            processCsi(character);
            m_parserState = ParserState::Ground;
        } else if (character >= 0x20 && character <= 0x3f) {
            m_csiBuffer += QChar(static_cast<ushort>(character));
        } else if (character == 0x1b) {
            m_parserState = ParserState::Escape;
        }
        break;
    case ParserState::Osc:
        if (character == 0x07) m_parserState = ParserState::Ground;
        else if (character == 0x1b) m_parserState = ParserState::OscEscape;
        break;
    case ParserState::OscEscape:
        m_parserState = character == '\\' ? ParserState::Ground : ParserState::Osc;
        break;
    case ParserState::Charset:
        m_parserState = ParserState::Ground;
        break;
    }
}

void VtTerminalModel::processControl(char32_t character)
{
    switch (character) {
    case '\r': m_cursorColumn = 0; m_wrapPending = false; break;
    case '\n': case '\v': case '\f': lineFeed(); break;
    case '\b': m_cursorColumn = qMax(0, m_cursorColumn - 1); m_wrapPending = false; break;
    case '\t': m_cursorColumn = qMin(m_columns - 1, (m_cursorColumn / 8 + 1) * 8); m_wrapPending = false; break;
    default: break;
    }
}

void VtTerminalModel::processEscape(char32_t character)
{
    m_parserState = ParserState::Ground;
    switch (character) {
    case '[':
        m_csiBuffer.clear();
        m_csiPrivate = false;
        m_parserState = ParserState::Csi;
        break;
    case ']': m_parserState = ParserState::Osc; break;
    case '(': case ')': m_parserState = ParserState::Charset; break;
    case '7': m_savedColumn = m_cursorColumn; m_savedRow = m_cursorRow; break;
    case '8': m_cursorColumn = m_savedColumn; m_cursorRow = m_savedRow; clampCursor(); break;
    case 'D': lineFeed(); break;
    case 'E': m_cursorColumn = 0; lineFeed(); break;
    case 'M': reverseIndex(); break;
    case 'c': reset(); break;
    default: break;
    }
}

QVector<int> VtTerminalModel::csiParameters(int defaultValue) const
{
    QVector<int> result;
    if (m_csiBuffer.isEmpty()) return {defaultValue};
    for (const auto &part : m_csiBuffer.split(QLatin1Char(';'))) {
        bool ok = false;
        const int value = part.toInt(&ok);
        result.append(ok ? value : defaultValue);
    }
    return result;
}

void VtTerminalModel::processCsi(char32_t finalCharacter)
{
    const bool zeroIsDefault = finalCharacter == 'm' || finalCharacter == 'J' || finalCharacter == 'K';
    auto parameters = csiParameters(zeroIsDefault ? 0 : 1);
    const int first = parameters.value(0, 1);
    switch (finalCharacter) {
    case 'A': m_cursorRow = qMax(m_scrollTop, m_cursorRow - qMax(1, first)); break;
    case 'B': m_cursorRow = qMin(m_scrollBottom, m_cursorRow + qMax(1, first)); break;
    case 'C': case 'a': m_cursorColumn = qMin(m_columns - 1, m_cursorColumn + qMax(1, first)); break;
    case 'D': m_cursorColumn = qMax(0, m_cursorColumn - qMax(1, first)); break;
    case 'E': m_cursorRow = qMin(m_scrollBottom, m_cursorRow + qMax(1, first)); m_cursorColumn = 0; break;
    case 'F': m_cursorRow = qMax(m_scrollTop, m_cursorRow - qMax(1, first)); m_cursorColumn = 0; break;
    case 'G': case '`': m_cursorColumn = qBound(0, first - 1, m_columns - 1); break;
    case 'd': m_cursorRow = qBound(0, first - 1, m_rows - 1); break;
    case 'H': case 'f':
        m_cursorRow = qBound(0, parameters.value(0, 1) - 1, m_rows - 1);
        m_cursorColumn = qBound(0, parameters.value(1, 1) - 1, m_columns - 1);
        break;
    case 'J': eraseDisplay(parameters.value(0, 0)); break;
    case 'K': eraseLine(parameters.value(0, 0)); break;
    case 'm': processSgr(parameters); break;
    case 'r':
        m_scrollTop = qBound(0, parameters.value(0, 1) - 1, m_rows - 1);
        m_scrollBottom = qBound(m_scrollTop, parameters.value(1, m_rows) - 1, m_rows - 1);
        m_cursorColumn = 0; m_cursorRow = 0;
        break;
    case 's': m_savedColumn = m_cursorColumn; m_savedRow = m_cursorRow; break;
    case 'u': m_cursorColumn = m_savedColumn; m_cursorRow = m_savedRow; clampCursor(); break;
    case 'S': scrollUp(qMax(1, first)); break;
    case 'T': scrollDown(qMax(1, first)); break;
    case 'X': eraseCells(m_cursorRow, m_cursorColumn, qMin(m_columns - 1, m_cursorColumn + qMax(1, first) - 1)); break;
    case 'P': {
        const int count = qMin(qMax(1, first), m_columns - m_cursorColumn);
        for (int column = m_cursorColumn; column < m_columns - count; ++column) m_cells[index(m_cursorRow, column)] = m_cells[index(m_cursorRow, column + count)];
        eraseCells(m_cursorRow, m_columns - count, m_columns - 1);
        break;
    }
    case '@': {
        const int count = qMin(qMax(1, first), m_columns - m_cursorColumn);
        for (int column = m_columns - 1; column >= m_cursorColumn + count; --column) m_cells[index(m_cursorRow, column)] = m_cells[index(m_cursorRow, column - count)];
        eraseCells(m_cursorRow, m_cursorColumn, m_cursorColumn + count - 1);
        break;
    }
    case 'L': {
        const int count = qMin(qMax(1, first), m_scrollBottom - m_cursorRow + 1);
        for (int row = m_scrollBottom; row >= m_cursorRow + count; --row)
            for (int column = 0; column < m_columns; ++column) m_cells[index(row, column)] = m_cells[index(row - count, column)];
        for (int row = m_cursorRow; row < m_cursorRow + count; ++row) eraseCells(row, 0, m_columns - 1);
        break;
    }
    case 'M': {
        const int count = qMin(qMax(1, first), m_scrollBottom - m_cursorRow + 1);
        for (int row = m_cursorRow; row <= m_scrollBottom - count; ++row)
            for (int column = 0; column < m_columns; ++column) m_cells[index(row, column)] = m_cells[index(row + count, column)];
        for (int row = m_scrollBottom - count + 1; row <= m_scrollBottom; ++row) eraseCells(row, 0, m_columns - 1);
        break;
    }
    case 'h': case 'l':
        if (m_csiPrivate) {
            const bool enabled = finalCharacter == 'h';
            for (int parameter : parameters) {
                if (parameter == 5) m_reverseVideo = enabled;
                else if (parameter == 25) m_cursorVisible = enabled;
                else if (parameter == 1049 || parameter == 47) setAlternateScreen(enabled, parameter == 1049);
                else if (parameter == 2004) m_bracketedPaste = enabled;
                else if (parameter == 1000) m_mouseTracking = enabled ? MouseTracking::Press : MouseTracking::None;
                else if (parameter == 1002) m_mouseTracking = enabled ? MouseTracking::ButtonMotion : MouseTracking::None;
                else if (parameter == 1003) m_mouseTracking = enabled ? MouseTracking::AnyMotion : MouseTracking::None;
                else if (parameter == 1006) m_sgrMouseEncoding = enabled;
            }
        }
        break;
    default: break;
    }
    m_wrapPending = false;
}

void VtTerminalModel::processSgr(const QVector<int> &parameters)
{
    for (int index = 0; index < parameters.size(); ++index) {
        const int value = parameters.at(index);
        if (value == 0) resetAttributes();
        else if (value == 1) m_attributes.bold = true;
        else if (value == 4) m_attributes.underline = true;
        else if (value == 7) m_attributes.inverse = true;
        else if (value == 22) m_attributes.bold = false;
        else if (value == 24) m_attributes.underline = false;
        else if (value == 27) m_attributes.inverse = false;
        else if (value >= 30 && value <= 37) m_attributes.foreground = indexedColor(value - 30);
        else if (value >= 40 && value <= 47) m_attributes.background = indexedColor(value - 40);
        else if (value >= 90 && value <= 97) m_attributes.foreground = indexedColor(value - 90 + 8);
        else if (value >= 100 && value <= 107) m_attributes.background = indexedColor(value - 100 + 8);
        else if (value == 39) m_attributes.foreground = QColor(QStringLiteral("#D0DBE5"));
        else if (value == 49) m_attributes.background = QColor(QStringLiteral("#0C1825"));
        else if ((value == 38 || value == 48) && index + 1 < parameters.size()) {
            QColor color;
            if (parameters.at(index + 1) == 5 && index + 2 < parameters.size()) {
                color = indexedColor(parameters.at(index + 2));
                index += 2;
            } else if (parameters.at(index + 1) == 2 && index + 4 < parameters.size()) {
                color = QColor(qBound(0, parameters.at(index + 2), 255), qBound(0, parameters.at(index + 3), 255), qBound(0, parameters.at(index + 4), 255));
                index += 4;
            }
            if (color.isValid()) {
                if (value == 38) m_attributes.foreground = color;
                else m_attributes.background = color;
            }
        }
    }
}

QColor VtTerminalModel::indexedColor(int index)
{
    static const QVector<QColor> base{
        QColor("#17212B"), QColor("#E06C75"), QColor("#53C99C"), QColor("#E5C07B"),
        QColor("#61AFEF"), QColor("#C678DD"), QColor("#56B6C2"), QColor("#D0DBE5"),
        QColor("#5C6B7A"), QColor("#FF7B86"), QColor("#6EDCB0"), QColor("#FFD68A"),
        QColor("#78BCFF"), QColor("#DF91F2"), QColor("#75D5E2"), QColor("#FFFFFF")};
    index = qBound(0, index, 255);
    if (index < 16) return base.at(index);
    if (index < 232) {
        const int value = index - 16;
        const auto component = [](int part) { return part == 0 ? 0 : 55 + part * 40; };
        return QColor(component(value / 36), component((value / 6) % 6), component(value % 6));
    }
    const int gray = 8 + (index - 232) * 10;
    return QColor(gray, gray, gray);
}

void VtTerminalModel::putCharacter(char32_t character)
{
    if (isCombining(character) && m_cursorColumn > 0) {
        auto &previous = m_cells[index(m_cursorRow, m_cursorColumn - 1)];
        previous.text += QString::fromUcs4(&character, 1);
        return;
    }
    if (m_wrapPending) {
        m_cursorColumn = 0;
        lineFeed();
    }
    const bool wide = isWide(character);
    if (wide && m_cursorColumn == m_columns - 1) {
        m_cursorColumn = 0;
        lineFeed();
    }
    auto cellValue = m_attributes;
    cellValue.text = QString::fromUcs4(&character, 1);
    cellValue.wideContinuation = false;
    m_cells[index(m_cursorRow, m_cursorColumn)] = cellValue;
    if (wide && m_cursorColumn + 1 < m_columns) {
        auto continuation = blankCell();
        continuation.wideContinuation = true;
        m_cells[index(m_cursorRow, m_cursorColumn + 1)] = continuation;
        m_cursorColumn += 2;
    } else {
        ++m_cursorColumn;
    }
    if (m_cursorColumn >= m_columns) {
        m_cursorColumn = m_columns - 1;
        m_wrapPending = true;
    }
}

void VtTerminalModel::lineFeed()
{
    if (m_cursorRow == m_scrollBottom) scrollUp();
    else m_cursorRow = qMin(m_rows - 1, m_cursorRow + 1);
    m_wrapPending = false;
}

void VtTerminalModel::reverseIndex()
{
    if (m_cursorRow == m_scrollTop) scrollDown();
    else m_cursorRow = qMax(0, m_cursorRow - 1);
}

void VtTerminalModel::scrollUp(int count)
{
    count = qMin(qMax(1, count), m_scrollBottom - m_scrollTop + 1);
    if (!m_alternateScreen && m_scrollTop == 0 && m_scrollBottom == m_rows - 1) {
        for (int row = 0; row < count; ++row) {
            QVector<TerminalCell> historyLine;
            historyLine.reserve(m_columns);
            for (int column = 0; column < m_columns; ++column) historyLine.append(m_cells.at(index(row, column)));
            m_scrollback.append(std::move(historyLine));
        }
        constexpr int maxScrollbackLines = 5000;
        if (m_scrollback.size() > maxScrollbackLines) m_scrollback.remove(0, m_scrollback.size() - maxScrollbackLines);
    }
    for (int row = m_scrollTop; row <= m_scrollBottom - count; ++row)
        for (int column = 0; column < m_columns; ++column) m_cells[index(row, column)] = m_cells[index(row + count, column)];
    for (int row = m_scrollBottom - count + 1; row <= m_scrollBottom; ++row) eraseCells(row, 0, m_columns - 1);
}

void VtTerminalModel::scrollDown(int count)
{
    count = qMin(qMax(1, count), m_scrollBottom - m_scrollTop + 1);
    for (int row = m_scrollBottom; row >= m_scrollTop + count; --row)
        for (int column = 0; column < m_columns; ++column) m_cells[index(row, column)] = m_cells[index(row - count, column)];
    for (int row = m_scrollTop; row < m_scrollTop + count; ++row) eraseCells(row, 0, m_columns - 1);
}

void VtTerminalModel::eraseCells(int row, int from, int to)
{
    if (row < 0 || row >= m_rows) return;
    for (int column = qMax(0, from); column <= qMin(m_columns - 1, to); ++column) m_cells[index(row, column)] = blankCell();
}

void VtTerminalModel::eraseLine(int mode)
{
    if (mode == 1) eraseCells(m_cursorRow, 0, m_cursorColumn);
    else if (mode == 2) eraseCells(m_cursorRow, 0, m_columns - 1);
    else eraseCells(m_cursorRow, m_cursorColumn, m_columns - 1);
}

void VtTerminalModel::eraseDisplay(int mode)
{
    if (mode == 3) m_scrollback.clear();
    if (mode == 2 || mode == 3) {
        for (int row = 0; row < m_rows; ++row) eraseCells(row, 0, m_columns - 1);
    } else if (mode == 1) {
        for (int row = 0; row < m_cursorRow; ++row) eraseCells(row, 0, m_columns - 1);
        eraseCells(m_cursorRow, 0, m_cursorColumn);
    } else {
        eraseCells(m_cursorRow, m_cursorColumn, m_columns - 1);
        for (int row = m_cursorRow + 1; row < m_rows; ++row) eraseCells(row, 0, m_columns - 1);
    }
}

void VtTerminalModel::setAlternateScreen(bool enabled, bool saveCursor)
{
    if (enabled == m_alternateScreen) return;
    if (enabled) {
        m_primaryCells = m_cells;
        m_primaryCursorColumn = m_cursorColumn;
        m_primaryCursorRow = m_cursorRow;
        m_alternateScreen = true;
        m_cells.fill(blankCell(), m_columns * m_rows);
        m_cursorColumn = 0;
        m_cursorRow = 0;
        m_wrapPending = false;
    } else {
        if (!m_primaryCells.isEmpty()) m_cells = m_primaryCells;
        if (saveCursor) {
            m_cursorColumn = m_primaryCursorColumn;
            m_cursorRow = m_primaryCursorRow;
        }
        m_primaryCells.clear();
        m_alternateScreen = false;
        clampCursor();
    }
}

void VtTerminalModel::clampCursor()
{
    m_cursorColumn = qBound(0, m_cursorColumn, m_columns - 1);
    m_cursorRow = qBound(0, m_cursorRow, m_rows - 1);
}

bool VtTerminalModel::isWide(char32_t character)
{
    return (character >= 0x1100 && character <= 0x115f)
        || (character >= 0x2e80 && character <= 0xa4cf)
        || (character >= 0xac00 && character <= 0xd7a3)
        || (character >= 0xf900 && character <= 0xfaff)
        || (character >= 0xfe10 && character <= 0xfe6f)
        || (character >= 0xff00 && character <= 0xff60)
        || (character >= 0x1f300 && character <= 0x1faff)
        || (character >= 0x20000 && character <= 0x3fffd);
}

bool VtTerminalModel::isCombining(char32_t character)
{
    return (character >= 0x0300 && character <= 0x036f)
        || (character >= 0x1ab0 && character <= 0x1aff)
        || (character >= 0x1dc0 && character <= 0x1dff)
        || (character >= 0xfe20 && character <= 0xfe2f);
}

} // namespace noxshell::ui
