#pragma once

#include "VtTerminalModel.h"

#include <QStringDecoder>
#include <QWidget>

class QScrollBar;
class QAction;
class QContextMenuEvent;
class QMenu;

namespace noxshell::ui {

class TerminalView final : public QWidget {
    Q_OBJECT

public:
    explicit TerminalView(QWidget *parent = nullptr);

    void feedData(const QByteArray &data);
    void feedText(const QString &text);
    void clear();
    void clearSelection();
    [[nodiscard]] QString plainText() const { return m_model.plainText(); }
    [[nodiscard]] QString selectedText() const;
    [[nodiscard]] int historyLineCount() const { return m_model.historyLineCount(); }
    [[nodiscard]] int columns() const { return m_model.columns(); }
    [[nodiscard]] int rows() const { return m_model.rows(); }

signals:
    void inputGenerated(const QByteArray &data);
    void commandSubmitted(const QString &command);
    void terminalSizeChanged(int columns, int rows, int pixelWidth, int pixelHeight);

protected:
    bool event(QEvent *event) override;
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
    void updateGridSize();
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
    qreal m_cellWidth{};
    qreal m_cellHeight{};
    qreal m_ascent{};
    bool m_hasFocus{false};
    QScrollBar *m_scrollBar{};
    QMenu *m_contextMenu{};
    QAction *m_copyAction{};
    QAction *m_pasteAction{};
    QAction *m_selectAllAction{};
    QPoint m_selectionAnchor{-1, -1};
    QPoint m_selectionCursor{-1, -1};
    bool m_selecting{false};
    int m_pressedMouseButton{-1};
    QString m_pendingCommand;
    int m_pendingCommandCursor{};
    bool m_commandTrackingReliable{true};
};

} // namespace noxshell::ui
