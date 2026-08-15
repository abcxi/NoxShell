#pragma once

#include <QColor>
#include <QString>
#include <QVector>

namespace noxshell::ui {

struct TerminalCell {
    QString text{QStringLiteral(" ")};
    QColor foreground{QStringLiteral("#D0DBE5")};
    QColor background{QStringLiteral("#0C1825")};
    bool bold{false};
    bool underline{false};
    bool inverse{false};
    bool wideContinuation{false};
};

class VtTerminalModel final {
public:
    enum class MouseTracking {
        None,
        Press,
        ButtonMotion,
        AnyMotion,
    };

    VtTerminalModel(int columns = 120, int rows = 36);

    void feed(const QString &text);
    void resize(int columns, int rows);
    void reset();
    void clear();

    [[nodiscard]] int columns() const { return m_columns; }
    [[nodiscard]] int rows() const { return m_rows; }
    [[nodiscard]] int cursorColumn() const { return m_cursorColumn; }
    [[nodiscard]] int cursorRow() const { return m_cursorRow; }
    [[nodiscard]] bool cursorVisible() const { return m_cursorVisible; }
    [[nodiscard]] bool reverseVideo() const { return m_reverseVideo; }
    [[nodiscard]] bool bracketedPaste() const { return m_bracketedPaste; }
    [[nodiscard]] bool alternateScreen() const { return m_alternateScreen; }
    [[nodiscard]] MouseTracking mouseTracking() const { return m_mouseTracking; }
    [[nodiscard]] bool sgrMouseEncoding() const { return m_sgrMouseEncoding; }
    [[nodiscard]] int historyLineCount() const { return m_scrollback.size(); }
    [[nodiscard]] int documentLineCount() const { return m_scrollback.size() + m_rows; }
    [[nodiscard]] const TerminalCell &cell(int row, int column) const;
    [[nodiscard]] const TerminalCell &documentCell(int line, int column) const;
    [[nodiscard]] QString documentLineText(int line, bool trimRight = true) const;
    [[nodiscard]] QString plainText() const;

private:
    enum class ParserState { Ground, Escape, Csi, Osc, OscEscape, Charset };

    void processCharacter(char32_t character);
    void processControl(char32_t character);
    void processEscape(char32_t character);
    void processCsi(char32_t finalCharacter);
    void processSgr(const QVector<int> &parameters);
    void putCharacter(char32_t character);
    void lineFeed();
    void reverseIndex();
    void scrollUp(int count = 1);
    void scrollDown(int count = 1);
    void eraseDisplay(int mode);
    void eraseLine(int mode);
    void eraseCells(int row, int from, int to);
    void setAlternateScreen(bool enabled, bool saveCursor);
    void clampCursor();
    void resetAttributes();
    [[nodiscard]] QVector<int> csiParameters(int defaultValue = 0) const;
    [[nodiscard]] TerminalCell blankCell() const;
    [[nodiscard]] static QColor indexedColor(int index);
    [[nodiscard]] static bool isWide(char32_t character);
    [[nodiscard]] static bool isCombining(char32_t character);
    [[nodiscard]] int index(int row, int column) const { return row * m_columns + column; }

    int m_columns{};
    int m_rows{};
    QVector<TerminalCell> m_cells;
    QVector<QVector<TerminalCell>> m_scrollback;
    QVector<TerminalCell> m_primaryCells;
    int m_cursorColumn{};
    int m_cursorRow{};
    int m_savedColumn{};
    int m_savedRow{};
    int m_primaryCursorColumn{};
    int m_primaryCursorRow{};
    int m_scrollTop{};
    int m_scrollBottom{};
    bool m_wrapPending{false};
    bool m_cursorVisible{true};
    bool m_reverseVideo{false};
    bool m_bracketedPaste{false};
    bool m_alternateScreen{false};
    MouseTracking m_mouseTracking{MouseTracking::None};
    bool m_sgrMouseEncoding{false};
    TerminalCell m_attributes;
    ParserState m_parserState{ParserState::Ground};
    QString m_csiBuffer;
    bool m_csiPrivate{false};
};

} // namespace noxshell::ui
