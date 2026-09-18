#include "RemoteFileEditor.h"

#include "../core/SshSession.h"
#include "AppTheme.h"
#include "SearchMarkerScrollBar.h"

#include <QCloseEvent>
#include <QFileInfo>
#include <QFrame>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMessageBox>
#include <QPalette>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QScopedValueRollback>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyle>
#include <QTabBar>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <utility>

namespace noxshell::ui {

namespace {

class ElidedLabel final : public QLabel {
public:
    explicit ElidedLabel(QWidget *parent = nullptr) : QLabel(parent)
    {
        setTextFormat(Qt::PlainText);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setMinimumWidth(0);
    }
    void setText(const QString &text)
    {
        m_fullText = text;
        setToolTip(text);
        updateText();
    }
protected:
    void resizeEvent(QResizeEvent *event) override { QLabel::resizeEvent(event); updateText(); }
private:
    void updateText() { QLabel::setText(fontMetrics().elidedText(m_fullText, Qt::ElideMiddle, qMax(0, contentsRect().width() - 12))); }
    QString m_fullText;
};

enum class ConfigSyntax { Plain, Yaml, Json, Ini };

ConfigSyntax syntaxForPath(const QString &path)
{
    const auto suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("yml") || suffix == QStringLiteral("yaml")) return ConfigSyntax::Yaml;
    if (suffix == QStringLiteral("json")) return ConfigSyntax::Json;
    if (suffix == QStringLiteral("ini") || suffix == QStringLiteral("conf") || suffix == QStringLiteral("cfg")
        || QFileInfo(path).fileName() == QStringLiteral(".env")) return ConfigSyntax::Ini;
    return ConfigSyntax::Plain;
}

QString syntaxName(ConfigSyntax syntax)
{
    switch (syntax) {
    case ConfigSyntax::Yaml: return QStringLiteral("YAML");
    case ConfigSyntax::Json: return QStringLiteral("JSON");
    case ConfigSyntax::Ini: return QStringLiteral("配置");
    case ConfigSyntax::Plain: return QStringLiteral("纯文本");
    }
    return {};
}

// Display-only, bounded, single-pass highlighting. It never reformats a file,
// changes indentation, or parses/evaluates configuration values.
class ConfigHighlighter final : public QSyntaxHighlighter {
public:
    ConfigHighlighter(QTextDocument *document, ConfigSyntax syntax)
        : QSyntaxHighlighter(document), m_syntax(syntax), m_dark(isApplicationDarkTheme()) {}
    void updateTheme()
    {
        const bool dark = isApplicationDarkTheme();
        if (m_dark == dark) return;
        m_dark = dark;
        rehighlight();
    }
    void setWithinBudget(bool enabled)
    {
        if (m_enabled == enabled) return;
        m_enabled = enabled;
        rehighlight();
    }
protected:
    void highlightBlock(const QString &text) override
    {
        if (!m_enabled || m_syntax == ConfigSyntax::Plain || text.size() > 8192) return;
        const QColor key(m_dark ? "#83BCFF" : "#245DA8");
        const QColor string(m_dark ? "#A5D6AF" : "#27764A");
        const QColor number(m_dark ? "#E6BC88" : "#A05A20");
        const QColor literal(m_dark ? "#C8A9EE" : "#8056B3");
        const QColor comment(m_dark ? "#7D91A7" : "#7A8898");
        const auto isKey = [&](int end) {
            while (end < text.size() && text.at(end).isSpace()) ++end;
            return end < text.size() && (text.at(end) == QLatin1Char(':')
                || (m_syntax == ConfigSyntax::Ini && text.at(end) == QLatin1Char('=')));
        };
        for (int i = 0; i < text.size();) {
            const auto ch = text.at(i);
            if (ch.isSpace()) { ++i; continue; }
            if (m_syntax != ConfigSyntax::Json && (ch == QLatin1Char('#')
                    || (m_syntax == ConfigSyntax::Ini && ch == QLatin1Char(';')))
                && (i == 0 || text.at(i - 1).isSpace())) {
                setFormat(i, text.size() - i, comment);
                break;
            }
            if (ch == QLatin1Char('"') || ch == QLatin1Char('\'')) {
                const int start = i++;
                while (i < text.size()) {
                    if (text.at(i) == QLatin1Char('\\') && ch == QLatin1Char('"')) { i = qMin(i + 2, int(text.size())); continue; }
                    if (text.at(i++) != ch) continue;
                    if (ch == QLatin1Char('\'') && i < text.size() && text.at(i) == ch) { ++i; continue; }
                    break;
                }
                setFormat(start, i - start, isKey(i) ? key : string);
                continue;
            }
            if (m_syntax == ConfigSyntax::Ini && ch == QLatin1Char('[')) {
                const int end = text.indexOf(QLatin1Char(']'), i + 1);
                if (end >= 0) { setFormat(i, end - i + 1, key); i = end + 1; continue; }
            }
            if (QStringLiteral("{}[],:=").contains(ch)) { ++i; continue; }
            const int start = i++;
            while (i < text.size() && !text.at(i).isSpace()
                && !QStringLiteral("{}[],:=\"'").contains(text.at(i))) ++i;
            const auto token = text.mid(start, i - start);
            bool numeric = false;
            token.toDouble(&numeric);
            if (isKey(i)) setFormat(start, i - start, key);
            else if (numeric) setFormat(start, i - start, number);
            else if (token == QStringLiteral("true") || token == QStringLiteral("false")
                || token == QStringLiteral("null") || token == QStringLiteral("~")) setFormat(start, i - start, literal);
        }
    }
private:
    ConfigSyntax m_syntax;
    bool m_dark;
    bool m_enabled{true};
};

class CodeEditor;

class LineNumberArea final : public QWidget {
public:
    explicit LineNumberArea(CodeEditor *editor);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    CodeEditor *m_editor{};
};

class CodeEditor final : public QPlainTextEdit {
public:
    explicit CodeEditor(const QString &path, QWidget *parent = nullptr)
        : QPlainTextEdit(parent)
        , m_lineNumberArea(new LineNumberArea(this))
    {
        setObjectName(QStringLiteral("remoteFileEditorText"));
        m_searchMarkerBar = new SearchMarkerScrollBar(Qt::Vertical);
        m_searchMarkerBar->setObjectName(QStringLiteral("remoteFileSearchMarkerBar"));
        setVerticalScrollBar(m_searchMarkerBar);
        connect(m_searchMarkerBar, &SearchMarkerScrollBar::searchMarkerActivated, this, [this](int index) {
            if (m_searchMarkerHandler) m_searchMarkerHandler(index);
        });
        auto codeFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        for (const auto &family : {QStringLiteral("Menlo"), QStringLiteral("Cascadia Mono"),
                 QStringLiteral("Consolas"), QStringLiteral("DejaVu Sans Mono"), QStringLiteral("Liberation Mono")}) {
            if (QFontDatabase::isFixedPitch(family)) { codeFont.setFamily(family); break; }
        }
        codeFont.setStyleHint(QFont::Monospace);
        setFont(codeFont);
        // An application-wide * font rule otherwise replaces the fixed font
        // during style polish, after the initial gutter has been measured.
        setStyleSheet(QStringLiteral("QPlainTextEdit#remoteFileEditorText { font-family: \"%1\"; font-size: 14px; }")
            .arg(codeFont.family()));
        setFrameShape(QFrame::NoFrame);
        document()->setDocumentMargin(10);
        setLineWrapMode(QPlainTextEdit::NoWrap);
        setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);
        auto editorPalette = palette();
        editorPalette.setColor(QPalette::Highlight, QColor(QStringLiteral("#FFB938")));
        editorPalette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#17233D")));
        setPalette(editorPalette);
        m_highlighter = new ConfigHighlighter(document(), syntaxForPath(path));
        connect(this, &QPlainTextEdit::blockCountChanged, this, &CodeEditor::updateLineNumberAreaWidth);
        connect(this, &QPlainTextEdit::updateRequest, this, &CodeEditor::updateLineNumberArea);
        connect(this, &QPlainTextEdit::cursorPositionChanged, this, &CodeEditor::updateExtraSelections);
        connect(this, &QPlainTextEdit::textChanged, this, [this] {
            m_highlighter->setWithinBudget(document()->characterCount() <= 512 * 1024);
        });
        updateLineNumberAreaWidth();
        updateExtraSelections();
    }

    void setLoadedText(const QString &text)
    {
        m_highlighter->setWithinBudget(text.size() <= 512 * 1024);
        setPlainText(text);
        updateLineNumberAreaWidth();
        updateExtraSelections();
    }

    void setSaveHandler(std::function<void()> handler) { m_saveHandler = std::move(handler); }
    void setCloseHandler(std::function<void()> handler) { m_closeHandler = std::move(handler); }
    void setFindHandler(std::function<void(bool)> handler) { m_findHandler = std::move(handler); }
    void setSearchMarkerHandler(std::function<void(int)> handler) { m_searchMarkerHandler = std::move(handler); }
    void setSearchMarkers(const QVector<qreal> &positions, int currentIndex)
    {
        m_searchMarkerBar->setSearchMarkers(positions, currentIndex);
    }
    void setSearchSelections(QList<QTextEdit::ExtraSelection> selections)
    {
        m_searchSelections = std::move(selections);
        updateExtraSelections();
    }
    void clearSearchSelections()
    {
        m_searchMarkerBar->clearSearchMarkers();
        if (m_searchSelections.isEmpty()) return;
        m_searchSelections.clear();
        updateExtraSelections();
    }

    int lineNumberAreaWidth() const
    {
        int digits = 1;
        for (int lines = qMax(1, blockCount()); lines >= 10; lines /= 10) ++digits;
        return qMax(44, 24 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits);
    }

    void paintLineNumbers(QPaintEvent *event)
    {
        QPainter painter(m_lineNumberArea);
        painter.setFont(font());
        const bool dark = isApplicationDarkTheme();
        painter.fillRect(event->rect(), QColor(dark ? QStringLiteral("#151E29") : QStringLiteral("#F6F8FB")));
        painter.setPen(QColor(dark ? QStringLiteral("#293748") : QStringLiteral("#E5EBF2")));
        painter.drawLine(m_lineNumberArea->width() - 1, event->rect().top(),
            m_lineNumberArea->width() - 1, event->rect().bottom());

        auto block = firstVisibleBlock();
        int blockNumber = block.blockNumber();
        int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
        int bottom = top + qRound(blockBoundingRect(block).height());
        while (block.isValid() && top <= event->rect().bottom()) {
            if (block.isVisible() && bottom >= event->rect().top()) {
                const bool current = blockNumber == textCursor().blockNumber();
                painter.setPen(QColor(current ? (dark ? "#8CC4FF" : "#246AC0") : (dark ? "#64788F" : "#8C9CAF")));
                if (current) painter.fillRect(0, top, 2, qRound(blockBoundingRect(block).height()), QColor("#398DED"));
                painter.drawText(0, top, m_lineNumberArea->width() - 12, fontMetrics().height(),
                    Qt::AlignRight, QString::number(blockNumber + 1));
            }
            block = block.next();
            top = bottom;
            bottom = top + qRound(blockBoundingRect(block).height());
            ++blockNumber;
        }
    }

    void toggleHashComment()
    {
        auto original = textCursor();
        const int selectionStart = original.selectionStart();
        const int selectionEnd = original.selectionEnd();
        auto first = document()->findBlock(selectionStart);
        auto last = document()->findBlock(selectionEnd);
        if (selectionEnd > selectionStart && last.isValid() && selectionEnd == last.position()) last = last.previous();
        if (!last.isValid()) last = first;

        QVector<int> positions;
        bool allCommented = true;
        for (auto block = first; block.isValid(); block = block.next()) {
            positions.append(block.position());
            const auto text = block.text();
            int offset = 0;
            while (offset < text.size() && text.at(offset).isSpace()) ++offset;
            if (offset >= text.size() || text.at(offset) != QLatin1Char('#')) allCommented = false;
            if (block == last) break;
        }

        QTextCursor edit(document());
        edit.beginEditBlock();
        for (auto iterator = positions.crbegin(); iterator != positions.crend(); ++iterator) {
            auto block = document()->findBlock(*iterator);
            if (!block.isValid()) continue;
            const auto text = block.text();
            int offset = 0;
            while (offset < text.size() && text.at(offset).isSpace()) ++offset;
            QTextCursor lineCursor(document());
            lineCursor.setPosition(block.position() + offset);
            if (allCommented && offset < text.size() && text.at(offset) == QLatin1Char('#')) {
                lineCursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor,
                    offset + 1 < text.size() && text.at(offset + 1) == QLatin1Char(' ') ? 2 : 1);
                lineCursor.removeSelectedText();
            } else {
                lineCursor.insertText(QStringLiteral("# "));
            }
        }
        edit.endEditBlock();
    }

protected:
    void changeEvent(QEvent *event) override
    {
        QPlainTextEdit::changeEvent(event);
        if (event->type() != QEvent::PaletteChange && event->type() != QEvent::StyleChange
            && event->type() != QEvent::FontChange && event->type() != QEvent::ApplicationFontChange) return;
        if (!m_lineNumberArea) return;
        setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);
        updateLineNumberAreaWidth();
        updateExtraSelections();
        if (m_highlighter) m_highlighter->updateTheme();
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        const auto modifiers = event->modifiers();
        const bool commandOrControl = modifiers.testFlag(Qt::ControlModifier)
            || modifiers.testFlag(Qt::MetaModifier);
        if (commandOrControl && event->key() == Qt::Key_S) {
            if (m_saveHandler) m_saveHandler();
            event->accept();
            return;
        }
        if (commandOrControl && event->key() == Qt::Key_F) {
            if (m_findHandler) m_findHandler(modifiers.testFlag(Qt::AltModifier));
            event->accept();
            return;
        }
        if (modifiers.testFlag(Qt::ControlModifier) && event->key() == Qt::Key_H) {
            if (m_findHandler) m_findHandler(true);
            event->accept();
            return;
        }
        if (commandOrControl && event->key() == Qt::Key_Z) {
            if (modifiers.testFlag(Qt::ShiftModifier)) redo();
            else undo();
            event->accept();
            return;
        }
        if (commandOrControl && event->key() == Qt::Key_Slash) {
            toggleHashComment();
            event->accept();
            return;
        }
        if (commandOrControl && event->key() == Qt::Key_W) {
            if (m_closeHandler) m_closeHandler();
            event->accept();
            return;
        }
        QPlainTextEdit::keyPressEvent(event);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QPlainTextEdit::resizeEvent(event);
        updateLineNumberAreaWidth();
    }

    void showEvent(QShowEvent *event) override
    {
        QPlainTextEdit::showEvent(event);
        updateLineNumberAreaWidth();
    }

private:
    void updateLineNumberAreaWidth()
    {
        if (!m_lineNumberArea || m_updatingGutter) return;
        const QScopedValueRollback guard(m_updatingGutter, true);
        const int width = lineNumberAreaWidth();
        if (viewportMargins().left() != width) setViewportMargins(width, 0, 0, 0);
        // Margin and gutter geometry must change together, including font/theme
        // changes and initial load. Never wait for a scroll event to repair it.
        m_lineNumberArea->setGeometry(contentsRect().left(), viewport()->geometry().top(), width, viewport()->height());
        m_lineNumberArea->update();
    }

    void updateLineNumberArea(const QRect &rect, int dy)
    {
        if (dy) m_lineNumberArea->scroll(0, dy);
        else m_lineNumberArea->update(0, rect.y(), m_lineNumberArea->width(), rect.height());
        if (rect.contains(viewport()->rect())) updateLineNumberAreaWidth();
    }

    void updateExtraSelections()
    {
        QTextEdit::ExtraSelection currentLine;
        currentLine.format.setBackground(QColor(isApplicationDarkTheme()
                ? QStringLiteral("#1B2A38") : QStringLiteral("#F3F8FF")));
        currentLine.format.setProperty(QTextFormat::FullWidthSelection, true);
        currentLine.cursor = textCursor();
        currentLine.cursor.clearSelection();
        auto selections = QList<QTextEdit::ExtraSelection>{currentLine};
        selections.append(m_searchSelections);
        setExtraSelections(selections);
        if (m_lineNumberArea) m_lineNumberArea->update();
    }

    LineNumberArea *m_lineNumberArea{};
    SearchMarkerScrollBar *m_searchMarkerBar{};
    ConfigHighlighter *m_highlighter{};
    bool m_updatingGutter{false};
    std::function<void()> m_saveHandler;
    std::function<void()> m_closeHandler;
    std::function<void(bool)> m_findHandler;
    std::function<void(int)> m_searchMarkerHandler;
    QList<QTextEdit::ExtraSelection> m_searchSelections;
};

LineNumberArea::LineNumberArea(CodeEditor *editor)
    : QWidget(editor)
    , m_editor(editor)
{
    setObjectName(QStringLiteral("remoteFileEditorLineNumbers"));
}

QSize LineNumberArea::sizeHint() const
{
    return QSize(m_editor->lineNumberAreaWidth(), 0);
}

void LineNumberArea::paintEvent(QPaintEvent *event)
{
    m_editor->paintLineNumbers(event);
}

QIcon dirtyIcon()
{
    QPixmap pixmap(10, 10);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#E34D59")));
    painter.drawEllipse(QRectF(1, 1, 8, 8));
    return QIcon(pixmap);
}

} // namespace

struct RemoteFileEditor::Document {
    QString path;
    QWidget *page{};
    CodeEditor *editor{};
    ElidedLabel *status{};
    QLabel *position{};
    quint64 readRequestId{};
    quint64 writeRequestId{};
    bool loaded{false};
    bool dirty{false};
    bool busy{false};
    bool closeAfterSave{false};
    QVector<QPair<int, int>> searchMatches;
};

RemoteFileEditor::RemoteFileEditor(SshSession *session, QString serverName, QString remotePath, QWidget *parent)
    : QDialog(parent)
    , m_session(session)
    , m_serverName(std::move(serverName))
{
    setObjectName(QStringLiteral("remoteFileEditor"));
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlag(Qt::Window, true);
    setWindowModality(Qt::NonModal);
    setWindowTitle(QStringLiteral("%1 · 远端文件编辑").arg(m_serverName));
    resize(1000, 720);
    setMinimumSize(600, 380);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_tabs = new QTabBar;
    m_tabs->setObjectName(QStringLiteral("remoteFileEditorTabs"));
    m_tabs->setFixedHeight(38);
    m_tabs->setExpanding(false);
    m_tabs->setMovable(true);
    m_tabs->setTabsClosable(true);
    m_tabs->setDocumentMode(true);
    m_tabs->setDrawBase(false);
    m_tabs->setElideMode(Qt::ElideMiddle);

    auto *toolbar = new QFrame;
    toolbar->setObjectName(QStringLiteral("remoteFileEditorToolbar"));
    auto *toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(14, 8, 12, 8);
    toolbarLayout->setSpacing(8);
    m_pathDisplay = new ElidedLabel;
    m_pathDisplay->setObjectName(QStringLiteral("remoteFileEditorPath"));
    m_documentState = new QLabel;
    m_documentState->setObjectName(QStringLiteral("remoteFileEditorState"));
    const auto tool = [&](const QString &name, const QString &text, const QString &hint) {
        auto *button = new QToolButton;
        button->setObjectName(name);
        button->setText(text);
        button->setAccessibleName(hint);
        button->setToolTip(hint);
        button->setAutoRaise(true);
        button->setFixedHeight(28);
        return button;
    };
    m_undoButton = tool(QStringLiteral("remoteFileEditorUndo"), {}, QStringLiteral("撤销（Cmd/Ctrl+Z）"));
    m_redoButton = tool(QStringLiteral("remoteFileEditorRedo"), {}, QStringLiteral("重做（Cmd/Ctrl+Shift+Z）"));
    m_undoButton->setIcon(QIcon(QStringLiteral(":/assets/editor-undo.svg")));
    m_redoButton->setIcon(QIcon(QStringLiteral(":/assets/editor-redo.svg")));
    m_undoButton->setFixedWidth(28);
    m_redoButton->setFixedWidth(28);
    auto *findButton = tool(QStringLiteral("remoteFileEditorFind"), QStringLiteral("查找"), QStringLiteral("查找 / 替换（Cmd/Ctrl+F）"));
    findButton->setIcon(QIcon(QStringLiteral(":/assets/editor-find.svg")));
    findButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_wrapButton = tool(QStringLiteral("remoteFileEditorWrap"), QStringLiteral("自动换行"), QStringLiteral("自动换行（仅改变显示，不修改文件）"));
    m_wrapButton->setCheckable(true);
    m_saveButton = new QPushButton(QStringLiteral("保存"));
    m_saveButton->setObjectName(QStringLiteral("remoteFileEditorSave"));
    m_saveButton->setToolTip(QStringLiteral("保存到远端服务器（Cmd/Ctrl+S）"));
    m_saveButton->setAutoDefault(false);
    m_saveButton->setDefault(false);
    m_saveButton->setFixedHeight(28);
    toolbarLayout->addWidget(m_pathDisplay, 1);
    toolbarLayout->addWidget(m_documentState);
    toolbarLayout->addSpacing(4);
    toolbarLayout->addWidget(m_undoButton);
    toolbarLayout->addWidget(m_redoButton);
    toolbarLayout->addWidget(findButton);
    toolbarLayout->addWidget(m_wrapButton);
    toolbarLayout->addWidget(m_saveButton);
    connect(m_undoButton, &QToolButton::clicked, this, [this] {
        if (auto *document = activeDocument()) document->editor->undo();
    });
    connect(m_redoButton, &QToolButton::clicked, this, [this] {
        if (auto *document = activeDocument()) document->editor->redo();
    });
    connect(findButton, &QToolButton::clicked, this, [this] { showFindPanel(false); });
    connect(m_wrapButton, &QToolButton::toggled, this, [this](bool wrap) {
        if (auto *document = activeDocument())
            document->editor->setLineWrapMode(wrap ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
    });
    connect(m_saveButton, &QPushButton::clicked, this, [this] { saveDocument(activeDocument()); });

    m_findPanel = new QFrame;
    m_findPanel->setObjectName(QStringLiteral("remoteFileFindPanel"));
    auto *findPanelLayout = new QVBoxLayout(m_findPanel);
    findPanelLayout->setContentsMargins(8, 6, 8, 6);
    findPanelLayout->setSpacing(5);
    auto *findRow = new QHBoxLayout;
    findRow->setSpacing(5);
    auto *findLabel = new QLabel(QStringLiteral("查找"));
    findLabel->setFixedWidth(34);
    m_findEdit = new QLineEdit;
    m_findEdit->setObjectName(QStringLiteral("remoteFileFindEdit"));
    m_findEdit->setPlaceholderText(QStringLiteral("输入查找内容"));
    m_findStatus = new QLabel(QStringLiteral("0 / 0"));
    m_findStatus->setObjectName(QStringLiteral("remoteFileFindStatus"));
    m_findStatus->setMinimumWidth(58);
    m_findStatus->setAlignment(Qt::AlignCenter);
    m_caseSensitive = new QToolButton;
    m_caseSensitive->setObjectName(QStringLiteral("remoteFileFindCase"));
    m_caseSensitive->setText(QStringLiteral("Aa"));
    m_caseSensitive->setCheckable(true);
    m_caseSensitive->setToolTip(QStringLiteral("区分大小写"));
    auto *previousButton = new QToolButton;
    previousButton->setObjectName(QStringLiteral("remoteFileFindPrevious"));
    previousButton->setText(QStringLiteral("↑"));
    previousButton->setToolTip(QStringLiteral("上一个（Shift+Enter）"));
    auto *nextButton = new QToolButton;
    nextButton->setObjectName(QStringLiteral("remoteFileFindNext"));
    nextButton->setText(QStringLiteral("↓"));
    nextButton->setToolTip(QStringLiteral("下一个（Enter）"));
    m_replaceToggle = new QToolButton;
    m_replaceToggle->setObjectName(QStringLiteral("remoteFileReplaceToggle"));
    m_replaceToggle->setText(QStringLiteral("替换"));
    m_replaceToggle->setCheckable(true);
    m_replaceToggle->setToolTip(QStringLiteral("展开替换（Cmd+Option+F / Ctrl+H）"));
    auto *closeFindButton = new QToolButton;
    closeFindButton->setObjectName(QStringLiteral("remoteFileFindClose"));
    closeFindButton->setText(QStringLiteral("×"));
    closeFindButton->setToolTip(QStringLiteral("关闭查找栏（Esc）"));
    findRow->addWidget(findLabel);
    findRow->addWidget(m_findEdit, 1);
    findRow->addWidget(m_findStatus);
    findRow->addWidget(m_caseSensitive);
    findRow->addWidget(previousButton);
    findRow->addWidget(nextButton);
    findRow->addWidget(m_replaceToggle);
    findRow->addWidget(closeFindButton);
    findPanelLayout->addLayout(findRow);

    m_replaceRow = new QWidget;
    m_replaceRow->setObjectName(QStringLiteral("remoteFileReplaceRow"));
    auto *replaceLayout = new QHBoxLayout(m_replaceRow);
    replaceLayout->setContentsMargins(0, 0, 0, 0);
    replaceLayout->setSpacing(5);
    auto *replaceLabel = new QLabel(QStringLiteral("替换"));
    replaceLabel->setFixedWidth(34);
    m_replaceEdit = new QLineEdit;
    m_replaceEdit->setObjectName(QStringLiteral("remoteFileReplaceEdit"));
    m_replaceEdit->setPlaceholderText(QStringLiteral("输入替换内容"));
    auto *replaceButton = new QPushButton(QStringLiteral("替换"));
    replaceButton->setObjectName(QStringLiteral("remoteFileReplaceOne"));
    auto *replaceAllButton = new QPushButton(QStringLiteral("全部替换"));
    replaceAllButton->setObjectName(QStringLiteral("remoteFileReplaceAll"));
    replaceLayout->addWidget(replaceLabel);
    replaceLayout->addWidget(m_replaceEdit, 1);
    replaceLayout->addWidget(replaceButton);
    replaceLayout->addWidget(replaceAllButton);
    findPanelLayout->addWidget(m_replaceRow);
    m_findPanel->hide();
    m_replaceRow->hide();

    m_stack = new QStackedWidget;
    m_stack->setObjectName(QStringLiteral("remoteFileEditorStack"));
    layout->addWidget(m_tabs);
    layout->addWidget(toolbar);
    layout->addWidget(m_findPanel);
    layout->addWidget(m_stack, 1);

    connect(m_tabs, &QTabBar::currentChanged, this, [this](int index) {
        if (index >= 0 && index < m_stack->count()) m_stack->setCurrentIndex(index);
        updateChrome();
        if (m_findPanel && m_findPanel->isVisible() && !m_findEdit->text().isEmpty()) findNext(false);
    });
    connect(m_tabs, &QTabBar::tabMoved, this, [this](int from, int to) {
        if (from == to || from < 0 || from >= m_documents.size() || to < 0 || to >= m_documents.size()) return;
        auto *document = m_documents.takeAt(from);
        m_documents.insert(to, document);
        m_stack->insertWidget(to, document->page);
        m_stack->setCurrentIndex(m_tabs->currentIndex());
        updateChrome();
    });
    connect(m_tabs, &QTabBar::tabCloseRequested, this, &RemoteFileEditor::requestCloseDocument);
    connect(m_findEdit, &QLineEdit::textChanged, this, [this] {
        if (m_findEdit->text().isEmpty()) updateFindStatus();
        else findNext(false);
    });
    connect(m_caseSensitive, &QToolButton::toggled, this, [this] {
        if (!m_findEdit->text().isEmpty()) findNext(false);
        else updateFindStatus();
    });
    connect(previousButton, &QToolButton::clicked, this, [this] { findNext(true); });
    connect(nextButton, &QToolButton::clicked, this, [this] { findNext(false); });
    connect(m_replaceToggle, &QToolButton::toggled, this, [this](bool expanded) {
        m_replaceRow->setVisible(expanded);
        m_replaceToggle->setText(expanded ? QStringLiteral("收起替换") : QStringLiteral("替换"));
        m_replaceToggle->setToolTip(expanded ? QStringLiteral("收起替换")
                                             : QStringLiteral("展开替换（Cmd+Option+F / Ctrl+H）"));
        if (expanded && m_findPanel->isVisible()) {
            m_replaceEdit->setFocus();
            m_replaceEdit->selectAll();
        }
    });
    connect(closeFindButton, &QToolButton::clicked, this, &RemoteFileEditor::hideFindPanel);
    connect(replaceButton, &QPushButton::clicked, this, &RemoteFileEditor::replaceCurrent);
    connect(replaceAllButton, &QPushButton::clicked, this, &RemoteFileEditor::replaceAll);
    m_findEdit->installEventFilter(this);
    m_replaceEdit->installEventFilter(this);

    connect(m_session, &SshSession::remoteFileRead, this,
        [this](quint64 requestId, const QString &path, const QByteArray &data) {
            auto *document = documentForRequest(requestId, false);
            if (!document || document->path != path) return;
            if (data.contains('\0')) {
                setBusy(document, false, QStringLiteral("%1 · 二进制文件不支持文本编辑").arg(path));
                return;
            }
            // busy/loaded guard the dirty handler. Blocking editor signals here
            // also blocks blockCountChanged/updateRequest and leaves the gutter
            // over the text until a later scroll or resize repairs the margins.
            document->editor->setLoadedText(QString::fromUtf8(data));
            document->editor->document()->setModified(false);
            document->loaded = true;
            setDirty(document, false);
            setBusy(document, false, QStringLiteral("已读取 %1 字节 · Cmd/Ctrl+S 保存 · Ctrl+/ 注释").arg(data.size()));
            updatePosition(document);
            if (document == activeDocument()) document->editor->setFocus();
            if (document == activeDocument() && m_findPanel->isVisible() && !m_findEdit->text().isEmpty()) findNext(false);
        });
    connect(m_session, &SshSession::remoteFileReadFailed, this,
        [this](quint64 requestId, const QString &path, const QString &message) {
            auto *document = documentForRequest(requestId, false);
            if (!document || document->path != path) return;
            setBusy(document, false, QStringLiteral("%1 · 读取失败：%2").arg(path, message));
        });
    connect(m_session, &SshSession::remoteFileWritten, this,
        [this](quint64 requestId, const QString &path) {
            auto *document = documentForRequest(requestId, true);
            if (!document || document->path != path) return;
            document->editor->document()->setModified(false);
            setDirty(document, false);
            setBusy(document, false, QStringLiteral("%1 · 已保存到远端服务器").arg(path));
            if (document->closeAfterSave) {
                const int index = documentIndex(document);
                if (index >= 0) removeDocument(index);
            }
            maybeFinishWindowClose();
        });
    connect(m_session, &SshSession::remoteFileWriteFailed, this,
        [this](quint64 requestId, const QString &path, const QString &message) {
            auto *document = documentForRequest(requestId, true);
            if (!document || document->path != path) return;
            document->closeAfterSave = false;
            m_closeAfterAllSaved = false;
            setBusy(document, false, QStringLiteral("%1 · 保存失败：%2").arg(path, message));
        });
    connect(m_session, &SshSession::connectionChanged, this, [this](bool connected, const QString &message) {
        if (connected) return;
        for (auto *document : m_documents) {
            document->editor->setEnabled(false);
            document->status->setText(QStringLiteral("%1 · 连接已断开：%2").arg(document->path, message));
        }
        updateChrome();
    });

    auto *closeShortcut = new QShortcut(QKeySequence::Close, this);
    closeShortcut->setObjectName(QStringLiteral("remoteFileEditorCloseShortcut"));
    connect(closeShortcut, &QShortcut::activated, this, [this] {
        const int index = m_tabs->currentIndex();
        if (index >= 0) requestCloseDocument(index);
    });

    auto *findShortcut = new QShortcut(QKeySequence::Find, this);
    findShortcut->setObjectName(QStringLiteral("remoteFileEditorFindShortcut"));
    connect(findShortcut, &QShortcut::activated, this, [this] { showFindPanel(false); });
    auto *replaceShortcut = new QShortcut(QKeySequence(Qt::META | Qt::ALT | Qt::Key_F), this);
    replaceShortcut->setObjectName(QStringLiteral("remoteFileEditorReplaceShortcut"));
    connect(replaceShortcut, &QShortcut::activated, this, [this] { showFindPanel(true); });
    auto *replaceControlShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_H), this);
    replaceControlShortcut->setObjectName(QStringLiteral("remoteFileEditorReplaceControlShortcut"));
    connect(replaceControlShortcut, &QShortcut::activated, this, [this] { showFindPanel(true); });
    m_hideFindShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    m_hideFindShortcut->setObjectName(QStringLiteral("remoteFileEditorHideFindShortcut"));
    m_hideFindShortcut->setEnabled(false);
    connect(m_hideFindShortcut, &QShortcut::activated, this, &RemoteFileEditor::hideFindPanel);

    openFile(remotePath);
    updateChrome();
}

RemoteFileEditor::~RemoteFileEditor()
{
    qDeleteAll(m_documents);
}

void RemoteFileEditor::openFile(const QString &remotePath)
{
    if (remotePath.isEmpty()) return;
    for (int index = 0; index < m_documents.size(); ++index) {
        if (m_documents.at(index)->path != remotePath) continue;
        m_tabs->setCurrentIndex(index);
        show();
        raise();
        activateWindow();
        return;
    }

    auto *document = new Document;
    document->path = remotePath;
    document->page = new QWidget;
    auto *pageLayout = new QVBoxLayout(document->page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(0);
    document->editor = new CodeEditor(remotePath);
    document->editor->setEnabled(false);
    document->status = new ElidedLabel;
    document->status->setText(QStringLiteral("正在读取远端文件…"));
    document->status->setObjectName(QStringLiteral("remoteFileEditorStatus"));
    auto *statusBar = new QFrame;
    statusBar->setObjectName(QStringLiteral("remoteFileEditorStatusBar"));
    statusBar->setFixedHeight(32);
    auto *statusLayout = new QHBoxLayout(statusBar);
    statusLayout->setContentsMargins(12, 0, 14, 0);
    statusLayout->setSpacing(18);
    document->position = new QLabel;
    document->position->setObjectName(QStringLiteral("remoteFileEditorPosition"));
    auto *language = new QLabel(syntaxName(syntaxForPath(remotePath)) + QStringLiteral("  ·  UTF-8"));
    language->setObjectName(QStringLiteral("remoteFileEditorLanguage"));
    statusLayout->addWidget(document->status, 1);
    statusLayout->addWidget(document->position);
    statusLayout->addWidget(language);
    pageLayout->addWidget(document->editor, 1);
    pageLayout->addWidget(statusBar);

    const int index = m_tabs->addTab(QString{});
    m_stack->addWidget(document->page);
    m_documents.append(document);
    m_tabs->setTabToolTip(index, remotePath);
    updateTab(document);
    installCloseButton(index);
    m_tabs->setCurrentIndex(index);
    updateChrome();

    connect(document->editor->document(), &QTextDocument::contentsChange, document->page,
        [this, document](int, int removed, int added) {
        // Syntax/theme-only format changes must not mark a remote file dirty.
        if ((!removed && !added) || !document->loaded || document->busy) return;
        setDirty(document, true);
        document->status->setText(QStringLiteral("有未保存的修改 · Cmd/Ctrl+S 保存"));
    });
    connect(document->editor, &QPlainTextEdit::textChanged, document->page, [this, document] {
        if (document->loaded && !document->busy && document == activeDocument() && m_findPanel->isVisible()) updateFindStatus();
    });
    connect(document->editor, &QPlainTextEdit::cursorPositionChanged, document->page, [this, document] { updatePosition(document); });
    connect(document->editor, &QPlainTextEdit::blockCountChanged, document->page, [this, document] { updatePosition(document); });
    connect(document->editor, &QPlainTextEdit::undoAvailable, document->page, [this] { updateChrome(); });
    connect(document->editor, &QPlainTextEdit::redoAvailable, document->page, [this] { updateChrome(); });
    document->editor->setSaveHandler([this, document] { saveDocument(document); });
    document->editor->setCloseHandler([this, document] {
        const int index = documentIndex(document);
        if (index >= 0) requestCloseDocument(index);
    });
    document->editor->setFindHandler([this](bool showReplace) { showFindPanel(showReplace); });
    document->editor->setSearchMarkerHandler([this, document](int index) {
        if (document != activeDocument() || index < 0 || index >= document->searchMatches.size()) return;
        const auto range = document->searchMatches.at(index);
        QTextCursor cursor(document->editor->document());
        cursor.setPosition(range.first);
        cursor.setPosition(range.second, QTextCursor::KeepAnchor);
        document->editor->setTextCursor(cursor);
        document->editor->centerCursor();
        updateFindStatus();
        document->editor->setFocus(Qt::MouseFocusReason);
    });

    // Closing a not-yet-loaded tab must cancel its queued load callback.
    QTimer::singleShot(0, document->page, [this, page = document->page] {
        if (auto *pending = documentForPage(page)) beginLoad(pending);
    });
}

QString RemoteFileEditor::remotePath() const
{
    const int index = m_tabs ? m_tabs->currentIndex() : -1;
    return index >= 0 && index < m_documents.size() ? m_documents.at(index)->path : QString{};
}

RemoteFileEditor::Document *RemoteFileEditor::documentForPage(QWidget *page) const
{
    const auto found = std::find_if(m_documents.cbegin(), m_documents.cend(), [page](const Document *document) {
        return document->page == page;
    });
    return found == m_documents.cend() ? nullptr : *found;
}

RemoteFileEditor::Document *RemoteFileEditor::documentForRequest(quint64 requestId, bool writeRequest) const
{
    const auto found = std::find_if(m_documents.cbegin(), m_documents.cend(), [requestId, writeRequest](const Document *document) {
        return (writeRequest ? document->writeRequestId : document->readRequestId) == requestId;
    });
    return found == m_documents.cend() ? nullptr : *found;
}

int RemoteFileEditor::documentIndex(const Document *document) const
{
    return m_documents.indexOf(const_cast<Document *>(document));
}

RemoteFileEditor::Document *RemoteFileEditor::activeDocument() const
{
    const int index = m_tabs ? m_tabs->currentIndex() : -1;
    return index >= 0 && index < m_documents.size() ? m_documents.at(index) : nullptr;
}

void RemoteFileEditor::showFindPanel(bool showReplace)
{
    auto *document = activeDocument();
    if (!document || !document->editor) return;
    const auto selected = document->editor->textCursor().selectedText();
    if (!selected.isEmpty() && !selected.contains(QChar::ParagraphSeparator) && !selected.contains(QLatin1Char('\n'))) {
        const QSignalBlocker blocker(m_findEdit);
        m_findEdit->setText(selected);
    }
    m_findPanel->show();
    if (m_hideFindShortcut) m_hideFindShortcut->setEnabled(true);
    m_replaceToggle->setChecked(showReplace);
    m_replaceRow->setVisible(showReplace);
    if (showReplace && !m_findEdit->text().isEmpty()) {
        m_replaceEdit->setFocus();
        m_replaceEdit->selectAll();
    } else {
        m_findEdit->setFocus();
        m_findEdit->selectAll();
    }
    if (!m_findEdit->text().isEmpty()) findNext(false);
    else updateFindStatus();
}

void RemoteFileEditor::hideFindPanel()
{
    if (!m_findPanel) return;
    m_findPanel->hide();
    if (m_hideFindShortcut) m_hideFindShortcut->setEnabled(false);
    for (auto *document : std::as_const(m_documents)) {
        if (document && document->editor) {
            document->searchMatches.clear();
            document->editor->clearSearchSelections();
        }
    }
    if (auto *document = activeDocument(); document && document->editor) document->editor->setFocus();
}

bool RemoteFileEditor::findNext(bool backwards)
{
    auto *document = activeDocument();
    const auto needle = m_findEdit ? m_findEdit->text() : QString{};
    if (!document || !document->editor || needle.isEmpty()) {
        updateFindStatus();
        return false;
    }

    QTextDocument::FindFlags flags;
    if (backwards) flags |= QTextDocument::FindBackward;
    if (m_caseSensitive && m_caseSensitive->isChecked()) flags |= QTextDocument::FindCaseSensitively;
    bool found = document->editor->find(needle, flags);
    if (!found) {
        auto cursor = document->editor->textCursor();
        cursor.movePosition(backwards ? QTextCursor::End : QTextCursor::Start);
        document->editor->setTextCursor(cursor);
        found = document->editor->find(needle, flags);
    }
    updateFindStatus();
    return found;
}

void RemoteFileEditor::updateFindStatus()
{
    if (!m_findStatus || !m_findEdit) return;
    auto *document = activeDocument();
    const auto needle = m_findEdit->text();
    for (auto *candidate : std::as_const(m_documents)) {
        if (candidate != document && candidate && candidate->editor) {
            candidate->searchMatches.clear();
            candidate->editor->clearSearchSelections();
        }
    }
    if (!document || !document->editor || needle.isEmpty()) {
        if (document && document->editor) {
            document->searchMatches.clear();
            document->editor->clearSearchSelections();
        }
        m_findStatus->setText(QStringLiteral("0 / 0"));
        m_findStatus->setStyleSheet(isApplicationDarkTheme() ? QStringLiteral("color:#A6BCD1;") : QStringLiteral("color:#738297;"));
        return;
    }

    QTextDocument::FindFlags flags;
    if (m_caseSensitive && m_caseSensitive->isChecked()) flags |= QTextDocument::FindCaseSensitively;
    const auto selectedCursor = document->editor->textCursor();
    QTextCursor scan(document->editor->document());
    scan.movePosition(QTextCursor::Start);
    int total = 0;
    int current = 0;
    QList<QTextEdit::ExtraSelection> highlights;
    QVector<qreal> markerPositions;
    document->searchMatches.clear();
    const int maximumBlock = qMax(1, document->editor->document()->blockCount() - 1);
    while (true) {
        const auto match = document->editor->document()->find(needle, scan, flags);
        if (match.isNull()) break;
        ++total;
        const bool isCurrent = match.selectionStart() == selectedCursor.selectionStart()
            && match.selectionEnd() == selectedCursor.selectionEnd();
        if (isCurrent) {
            current = total;
        }
        QTextEdit::ExtraSelection highlight;
        highlight.cursor = match;
        highlight.format.setBackground(QColor(isCurrent ? QStringLiteral("#FFB938")
                                                        : QStringLiteral("#FFF36A")));
        highlight.format.setForeground(QColor(QStringLiteral("#17233D")));
        highlights.append(highlight);
        document->searchMatches.append({match.selectionStart(), match.selectionEnd()});
        markerPositions.append(qBound<qreal>(0.0,
            qreal(match.block().blockNumber()) / maximumBlock, 1.0));
        scan.setPosition(match.selectionEnd());
    }
    document->editor->setSearchSelections(std::move(highlights));
    document->editor->setSearchMarkers(markerPositions, current > 0 ? current - 1 : (total > 0 ? 0 : -1));
    if (total == 0) {
        m_findStatus->setText(QStringLiteral("无匹配"));
        m_findStatus->setStyleSheet(isApplicationDarkTheme() ? QStringLiteral("color:#F08C82;") : QStringLiteral("color:#D54941;"));
    } else {
        m_findStatus->setText(QStringLiteral("%1 / %2").arg(current > 0 ? current : 1).arg(total));
        m_findStatus->setStyleSheet(isApplicationDarkTheme() ? QStringLiteral("color:#A6BCD1;") : QStringLiteral("color:#53677E;"));
    }
}

void RemoteFileEditor::replaceCurrent()
{
    auto *document = activeDocument();
    if (!document || !document->editor || !document->editor->isEnabled() || m_findEdit->text().isEmpty()) return;
    auto cursor = document->editor->textCursor();
    const auto sensitivity = m_caseSensitive->isChecked() ? Qt::CaseSensitive : Qt::CaseInsensitive;
    if (!cursor.hasSelection() || cursor.selectedText().compare(m_findEdit->text(), sensitivity) != 0) {
        if (!findNext(false)) return;
        cursor = document->editor->textCursor();
    }
    cursor.insertText(m_replaceEdit->text());
    document->editor->setTextCursor(cursor);
    findNext(false);
}

void RemoteFileEditor::replaceAll()
{
    auto *document = activeDocument();
    const auto needle = m_findEdit ? m_findEdit->text() : QString{};
    if (!document || !document->editor || !document->editor->isEnabled() || needle.isEmpty()) return;
    QTextDocument::FindFlags flags;
    if (m_caseSensitive->isChecked()) flags |= QTextDocument::FindCaseSensitively;
    QTextCursor edit(document->editor->document());
    edit.beginEditBlock();
    QTextCursor scan(document->editor->document());
    scan.movePosition(QTextCursor::Start);
    int replacements = 0;
    while (true) {
        auto match = document->editor->document()->find(needle, scan, flags);
        if (match.isNull()) break;
        match.insertText(m_replaceEdit->text());
        scan = match;
        ++replacements;
    }
    edit.endEditBlock();
    updateFindStatus();
    m_findStatus->setText(QStringLiteral("已替换 %1 处").arg(replacements));
    m_findStatus->setStyleSheet(QStringLiteral("color:#008858;"));
}

bool RemoteFileEditor::eventFilter(QObject *watched, QEvent *event)
{
    if ((watched == m_findEdit || watched == m_replaceEdit) && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        const bool commandOrControl = keyEvent->modifiers().testFlag(Qt::ControlModifier)
            || keyEvent->modifiers().testFlag(Qt::MetaModifier);
        if (commandOrControl && keyEvent->key() == Qt::Key_F) {
            showFindPanel(keyEvent->modifiers().testFlag(Qt::AltModifier));
            return true;
        }
        if (keyEvent->modifiers().testFlag(Qt::ControlModifier) && keyEvent->key() == Qt::Key_H) {
            showFindPanel(true);
            return true;
        }
        if (keyEvent->key() == Qt::Key_Escape) {
            hideFindPanel();
            return true;
        }
        if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
            findNext(keyEvent->modifiers().testFlag(Qt::ShiftModifier));
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void RemoteFileEditor::beginLoad(Document *document)
{
    if (!document || !m_session || !m_session->isConnected()) {
        if (document) setBusy(document, false, QStringLiteral("%1 · SSH 会话未连接").arg(document->path));
        return;
    }
    setBusy(document, true, QStringLiteral("%1 · 正在读取远端文件…").arg(document->path));
    document->readRequestId = m_session->readFile(document->path);
}

void RemoteFileEditor::saveDocument(Document *document)
{
    if (!document || !document->loaded || document->busy || !document->dirty
        || !m_session || !m_session->isConnected()) return;
    setBusy(document, true, QStringLiteral("%1 · 正在原子保存到远端服务器…").arg(document->path));
    document->writeRequestId = m_session->writeFile(document->path, document->editor->toPlainText().toUtf8(), true);
}

void RemoteFileEditor::setBusy(Document *document, bool busy, const QString &message)
{
    if (!document) return;
    document->busy = busy;
    const auto prefix = document->path + QStringLiteral(" · ");
    document->status->setText(message.startsWith(prefix) ? message.mid(prefix.size()) : message);
    document->editor->setEnabled(document->loaded && !busy && m_session && m_session->isConnected());
    updateChrome();
}

void RemoteFileEditor::setDirty(Document *document, bool dirty)
{
    if (!document || document->dirty == dirty) return;
    document->dirty = dirty;
    updateTab(document);
    updateChrome();
}

void RemoteFileEditor::updatePosition(Document *document)
{
    if (!document || !document->position) return;
    const auto cursor = document->editor->textCursor();
    document->position->setText(QStringLiteral("行 %1，列 %2  ·  %3 行")
        .arg(cursor.blockNumber() + 1).arg(cursor.positionInBlock() + 1).arg(document->editor->blockCount()));
}

void RemoteFileEditor::updateChrome()
{
    if (!m_saveButton || !m_tabs || !m_pathDisplay) return;
    const auto *document = activeDocument();
    const bool ready = document && document->loaded && !document->busy
        && m_session && m_session->isConnected();
    m_saveButton->setEnabled(ready && document->dirty);
    m_undoButton->setEnabled(ready && document->editor->document()->isUndoAvailable());
    m_redoButton->setEnabled(ready && document->editor->document()->isRedoAvailable());
    m_wrapButton->setEnabled(document && document->loaded);
    const QSignalBlocker blocker(m_wrapButton);
    m_wrapButton->setChecked(document && document->editor->lineWrapMode() != QPlainTextEdit::NoWrap);
    static_cast<ElidedLabel *>(m_pathDisplay)->setText(document ? document->path : QString{});
    m_documentState->setText(!document ? QString{} : document->busy ? QStringLiteral("处理中…")
        : document->dirty ? QStringLiteral("未保存") : document->loaded ? QStringLiteral("已同步") : QStringLiteral("未读取"));
    const bool dirty = document && document->dirty;
    if (m_documentState->property("dirty").toBool() != dirty) {
        m_documentState->setProperty("dirty", dirty);
        m_documentState->style()->unpolish(m_documentState);
        m_documentState->style()->polish(m_documentState);
    }
}

void RemoteFileEditor::changeEvent(QEvent *event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        if (m_findPanel && m_findPanel->isVisible()) updateFindStatus();
        updateChrome();
    }
}

void RemoteFileEditor::updateTab(Document *document)
{
    const int index = documentIndex(document);
    if (index < 0) return;
    m_tabs->setTabText(index, QStringLiteral("%1 · %2").arg(m_serverName, QFileInfo(document->path).fileName()));
    m_tabs->setTabIcon(index, document->dirty ? dirtyIcon() : QIcon{});
    m_tabs->setTabToolTip(index, document->path);
}

void RemoteFileEditor::installCloseButton(int index)
{
    if (index < 0 || index >= m_tabs->count()) return;

    auto *nativeLeftButton = m_tabs->tabButton(index, QTabBar::LeftSide);
    auto *nativeRightButton = m_tabs->tabButton(index, QTabBar::RightSide);
    auto *container = new QWidget(m_tabs);
    container->setObjectName(QStringLiteral("remoteFileTabCloseContainer"));
    container->setFixedSize(25, 22);
    auto *containerLayout = new QHBoxLayout(container);
    containerLayout->setContentsMargins(1, 0, 5, 0);
    containerLayout->setSpacing(0);
    auto *closeButton = new QToolButton(container);
    closeButton->setObjectName(QStringLiteral("remoteFileTabCloseButton"));
    closeButton->setText(QString::fromUtf8("×"));
    closeButton->setToolTip(QStringLiteral("关闭标签"));
    closeButton->setFixedSize(19, 19);
    containerLayout->addWidget(closeButton);
    m_tabs->setTabButton(index, QTabBar::LeftSide, nullptr);
    m_tabs->setTabButton(index, QTabBar::RightSide, container);
    if (nativeLeftButton && nativeLeftButton != container) nativeLeftButton->deleteLater();
    if (nativeRightButton && nativeRightButton != container) nativeRightButton->deleteLater();

    connect(closeButton, &QToolButton::clicked, this, [this, container] {
        for (int candidate = 0; candidate < m_tabs->count(); ++candidate) {
            if (m_tabs->tabButton(candidate, QTabBar::RightSide) != container) continue;
            requestCloseDocument(candidate);
            return;
        }
    });
}

bool RemoteFileEditor::requestCloseDocument(int index)
{
    if (index < 0 || index >= m_documents.size()) return false;
    auto *document = m_documents.at(index);
    if (document->busy) {
        document->status->setText(QStringLiteral("%1 · 当前操作完成后再关闭").arg(document->path));
        return false;
    }
    if (document->dirty) {
        const auto answer = QMessageBox::warning(this, QStringLiteral("未保存的修改"),
            QStringLiteral("是否在关闭前保存“%1”？").arg(QFileInfo(document->path).fileName()),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
        if (answer == QMessageBox::Cancel) return false;
        if (answer == QMessageBox::Save) {
            document->closeAfterSave = true;
            saveDocument(document);
            return false;
        }
    }
    removeDocument(index);
    return true;
}

void RemoteFileEditor::removeDocument(int index)
{
    if (index < 0 || index >= m_documents.size()) return;
    auto *document = m_documents.takeAt(index);
    {
        const QSignalBlocker blocker(m_tabs);
        m_tabs->removeTab(index);
        m_stack->removeWidget(document->page);
    }
    document->page->deleteLater();
    delete document;
    if (m_documents.isEmpty()) {
        close();
        return;
    }
    const int next = qMin(index, m_documents.size() - 1);
    m_tabs->setCurrentIndex(next);
    m_stack->setCurrentIndex(next);
    updateChrome();
}

void RemoteFileEditor::maybeFinishWindowClose()
{
    if (!m_closeAfterAllSaved) return;
    const bool pending = std::any_of(m_documents.cbegin(), m_documents.cend(), [](const Document *document) {
        return document->dirty || document->busy;
    });
    if (pending) return;
    m_closeAfterAllSaved = false;
    QTimer::singleShot(0, this, &QDialog::close);
}

void RemoteFileEditor::closeEvent(QCloseEvent *event)
{
    const auto dirtyDocuments = std::count_if(m_documents.cbegin(), m_documents.cend(), [](const Document *document) {
        return document->dirty;
    });
    if (dirtyDocuments == 0) {
        event->accept();
        return;
    }
    const auto answer = QMessageBox::warning(this, QStringLiteral("未保存的修改"),
        QStringLiteral("有 %1 个远端文件尚未保存，是否全部保存后关闭？").arg(dirtyDocuments),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Discard) {
        event->accept();
    } else if (answer == QMessageBox::Save) {
        m_closeAfterAllSaved = true;
        for (auto *document : m_documents) {
            if (document->dirty && !document->busy) saveDocument(document);
        }
        event->ignore();
    } else {
        event->ignore();
    }
}

} // namespace noxshell::ui
