#pragma once

#include "VtTerminalModel.h"

#include <QPointF>
#include <QStringDecoder>
#include <QWidget>

class QScrollBar;
class QAction;
class QContextMenuEvent;
class QFrame;
class QLabel;
class QLineEdit;
class QMenu;
class QToolButton;

namespace noxshell::ui {

struct TerminalAppearance {
    QString fontFamily;
    int pointSize{12};
    qreal lineSpacing{1.05};

    friend bool operator==(const TerminalAppearance &, const TerminalAppearance &) = default;
};

class TerminalView final : public QWidget {
    Q_OBJECT

public:
    explicit TerminalView(QWidget *parent = nullptr);

    static TerminalAppearance defaultAppearance();
    static void setDefaultAppearance(const TerminalAppearance &appearance);
    void setAppearance(const TerminalAppearance &appearance);
    [[nodiscard]] TerminalAppearance appearance() const { return m_appearance; }

    void feedData(const QByteArray &data);
    void feedText(const QString &text);
    void clear();
    void clearSelection();
    [[nodiscard]] QString plainText() const { return m_model.plainText(); }
    [[nodiscard]] QString selectedText() const;
    [[nodiscard]] int historyLineCount() const { return m_model.historyLineCount(); }
    [[nodiscard]] int columns() const { return m_model.columns(); }
    [[nodiscard]] int rows() const { return m_model.rows(); }
    [[nodiscard]] QSizeF cellSize() const { return {m_cellWidth, m_cellHeight}; }
    [[nodiscard]] QPointF contentOrigin() const;
    [[nodiscard]] int searchMatchCount() const { return m_searchMatches.size(); }
    [[nodiscard]] int currentSearchMatch() const { return m_currentSearchMatch; }

public slots:
    void showSearch();
    void hideSearch();
    void findNext();
    void findPrevious();

signals:
    void inputGenerated(const QByteArray &data);
    void commandSubmitted(const QString &command);
    void terminalSizeChanged(int columns, int rows, int pixelWidth, int pixelHeight);

protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

private:
    struct SearchMatch {
        int line{};
        int startColumn{};
        int endColumn{};
    };

    void updateGridSize();
    void positionSearchBar();
    void rebuildSearchMatches(bool preserveCurrent, bool revealCurrent);
    void updateSearchCounter();
    void scrollToCurrentSearchMatch();
    [[nodiscard]] int columnForTextOffset(int line, int offset) const;
    void sendPaste(const QString &text);
    void sendInterrupt();
    void copySelection();
    void selectAllText();
    void trackKeyForCommand(QKeyEvent *event);
    void trackTextForCommand(const QString &text);
    void submitTrackedCommand();
    [[nodiscard]] QString commandFromCurrentScreenLine() const;
    void updateScrollBar(bool followBottom);
    [[nodiscard]] QPoint documentPosition(const QPointF &position) const;
    [[nodiscard]] bool hasSelection() const;
    [[nodiscard]] bool isSelected(int line, int column) const;
    [[nodiscard]] QByteArray mouseReport(int button, const QPointF &position, bool release = false) const;
    [[nodiscard]] bool shouldReportMouse(Qt::KeyboardModifiers modifiers) const;
    [[nodiscard]] QByteArray keySequence(QKeyEvent *event) const;

    VtTerminalModel m_model;
    QStringDecoder m_decoder{QStringDecoder::Utf8};
    QFont m_font;
    TerminalAppearance m_appearance;
    qreal m_cellWidth{};
    qreal m_cellHeight{};
    qreal m_ascent{};
    bool m_hasFocus{false};
    bool m_tabKeyDown{false};
    QScrollBar *m_scrollBar{};
    QMenu *m_contextMenu{};
    QAction *m_findAction{};
    QAction *m_copyAction{};
    QAction *m_pasteAction{};
    QAction *m_selectAllAction{};
    QFrame *m_searchBar{};
    QLineEdit *m_searchInput{};
    QLabel *m_searchCounter{};
    QToolButton *m_searchPrevious{};
    QToolButton *m_searchNext{};
    QToolButton *m_searchClose{};
    QVector<SearchMatch> m_searchMatches;
    int m_currentSearchMatch{-1};
    QPoint m_selectionAnchor{-1, -1};
    QPoint m_selectionCursor{-1, -1};
    bool m_selecting{false};
    int m_pressedMouseButton{-1};
    QString m_pendingCommand;
    int m_pendingCommandCursor{};
    bool m_commandTrackingReliable{true};
};

} // namespace noxshell::ui
