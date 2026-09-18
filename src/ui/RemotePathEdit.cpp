#include "RemotePathEdit.h"

#include <QFocusEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScopedValueRollback>
#include <QStyle>
#include <QStyleOptionFrame>
#include <QToolButton>

#include <algorithm>

namespace noxshell::ui {

RemotePathEdit::RemotePathEdit(QWidget *parent) : QLineEdit(parent)
{
    setObjectName(QStringLiteral("remotePathEdit"));
    setAccessibleName(QStringLiteral("远端目录路径"));
    // Directory loading disables the file tree; Qt may then advance keyboard
    // focus automatically. Editing is opt-in via click or Cmd/Ctrl+L only.
    setFocusPolicy(Qt::ClickFocus);
    setMinimumWidth(160);
    setFixedHeight(26);
    setLayoutDirection(Qt::LeftToRight);
    setStyleSheet(QStringLiteral(
        "QLineEdit#remotePathEdit{min-height:24px;max-height:24px;padding:0 9px;}"));
    m_overflow = makeButton(QStringLiteral("remotePathOverflow"));
    m_overflow->setText(QStringLiteral("…"));
    m_overflow->setToolTip(QStringLiteral("展开上级目录"));
    m_overflow->setAccessibleName(m_overflow->toolTip());
    m_overflowMenu = new QMenu(m_overflow);
    m_overflow->setMenu(m_overflowMenu);
    m_overflow->setPopupMode(QToolButton::InstantPopup);
    m_editButton = makeButton(QStringLiteral("remotePathEditButton"));
    m_editButton->setIcon(QIcon(QStringLiteral(":/assets/file-path-edit.svg")));
    m_editButton->setIconSize(QSize(14, 14));
    m_editButton->setToolTip(QStringLiteral("编辑完整路径"));
    m_editButton->setAccessibleName(m_editButton->toolTip());
    connect(m_editButton, &QToolButton::clicked, this, &RemotePathEdit::beginEditing);
    setPath(m_path);
}

QToolButton *RemotePathEdit::makeButton(const QString &name)
{
    auto *button = new QToolButton(this);
    button->setObjectName(name);
    button->setAutoRaise(true);
    // Give mouse focus to the button, not the enclosing text editor.
    button->setFocusPolicy(Qt::ClickFocus);
    button->setCursor(Qt::PointingHandCursor);
    button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    return button;
}

void RemotePathEdit::setPath(const QString &path)
{
    m_path = path.isEmpty() ? QStringLiteral("/") : path;
    for (auto *button : m_segments) {
        button->hide();
        button->deleteLater(); // A clicked breadcrumb can be the signal sender.
    }
    m_segments.clear();
    auto names = m_path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    names.prepend(QStringLiteral("/"));
    QString destination;
    for (int i = 0; i < names.size(); ++i) {
        destination = i == 0 ? QStringLiteral("/")
            : (destination == QStringLiteral("/") ? destination : destination + QLatin1Char('/')) + names.at(i);
        auto *button = makeButton(QStringLiteral("remotePathSegment"));
        button->setProperty("remotePath", destination);
        button->setProperty("segmentName", names.at(i));
        button->setProperty("current", i == names.size() - 1);
        button->setToolTip(destination);
        button->setAccessibleName(destination);
        connect(button, &QToolButton::clicked, this, [this, destination] {
            if (destination != m_path) emit pathActivated(destination);
        });
        m_segments.append(button);
    }
    setToolTip(m_path + QStringLiteral("\n点击目录跳转；点击空白处编辑，回车确认，Esc 取消"));
    endEditing();
}

void RemotePathEdit::beginEditing()
{
    if (!isEnabled()) return;
    m_editing = true;
    setText(m_path);
    layoutBreadcrumbs();
    setFocus(Qt::ShortcutFocusReason);
    selectAll();
    update();
}

void RemotePathEdit::endEditing()
{
    m_editing = false;
    setText(m_path);
    if (hasFocus()) clearFocus();
    layoutBreadcrumbs();
    update();
}

void RemotePathEdit::layoutBreadcrumbs()
{
    if (m_layingOut || !m_editButton || !m_overflow) return;
    QScopedValueRollback guard(m_layingOut, true);
    m_separators.clear();
    for (auto *button : m_segments) button->hide();
    m_overflow->hide();
    m_editButton->setVisible(!m_editing);
    if (m_editing || m_segments.isEmpty()) return;

    constexpr int gap = 12;
    constexpr int overflowWidth = 24;
    const auto area = rect().adjusted(5, 2, -5, -2);
    m_editButton->setGeometry(area.right() - 21, area.top(), 22, area.height());
    // Always leave a clickable blank area, even with a long final component.
    const int available = std::max(1, area.width() - 22 - 18);
    QList<int> widths;
    for (auto *button : m_segments) {
        button->ensurePolished();
        widths.append(std::max(24, button->fontMetrics().horizontalAdvance(
            button->property("segmentName").toString()) + 14));
    }
    QList<int> visible{0};
    int firstSuffix = 1;
    int total = widths.first();
    for (int i = 1; i < widths.size(); ++i) total += gap + widths.at(i);
    if (total > available && widths.size() > 2) {
        firstSuffix = widths.size() - 1;
        const int suffixBudget = std::max(20, available - widths.first() - gap * 2 - overflowWidth);
        int used = std::min(widths.last(), suffixBudget);
        while (firstSuffix > 2 && used + gap + widths.at(firstSuffix - 1) <= suffixBudget) {
            used += gap + widths.at(--firstSuffix);
        }
    }
    m_overflowMenu->clear();
    if (firstSuffix > 1) {
        visible.append(-1);
        for (int i = 1; i < firstSuffix; ++i) {
            const auto path = m_segments.at(i)->property("remotePath").toString();
            auto *action = m_overflowMenu->addAction(QString(path).replace(QLatin1Char('&'), QStringLiteral("&&")));
            action->setData(path);
            connect(action, &QAction::triggered, this, [this, path] { emit pathActivated(path); });
        }
    }
    for (int i = firstSuffix; i < widths.size(); ++i) visible.append(i);
    int x = area.left();
    for (int index = 0; index < visible.size(); ++index) {
        if (index > 0) {
            m_separators.append(QRect(x, area.top(), gap, area.height()));
            x += gap;
        }
        const int segment = visible.at(index);
        auto *button = segment < 0 ? m_overflow : m_segments.at(segment);
        int width = segment < 0 ? overflowWidth : widths.at(segment);
        if (index == visible.size() - 1) width = std::max(1, std::min(width, area.left() + available - x));
        if (segment >= 0) {
            const auto name = button->property("segmentName").toString();
            // Do not run elision at the exact rounded font advance: fractional
            // glyph metrics can otherwise replace even a short name with “…”.
            const auto label = width >= widths.at(segment) ? name
                : button->fontMetrics().elidedText(name, Qt::ElideMiddle, std::max(1, width - 14));
            button->setText(QString(label).replace(QLatin1Char('&'), QStringLiteral("&&")));
        }
        button->setGeometry(x, area.top(), width, area.height());
        button->show();
        x += width;
    }
    update();
}

void RemotePathEdit::paintEvent(QPaintEvent *event)
{
    if (m_editing) { QLineEdit::paintEvent(event); return; }
    QPainter painter(this);
    QStyleOptionFrame option;
    initStyleOption(&option);
    style()->drawPrimitive(QStyle::PE_PanelLineEdit, &option, &painter, this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(palette().color(QPalette::PlaceholderText), 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    for (const auto &separator : m_separators) {
        const auto c = QPointF(separator.center());
        painter.drawPolyline(QPolygonF{c + QPointF(-1.5, -3), c + QPointF(1.5, 0), c + QPointF(-1.5, 3)});
    }
}

void RemotePathEdit::resizeEvent(QResizeEvent *event)
{
    QLineEdit::resizeEvent(event);
    layoutBreadcrumbs();
}

void RemotePathEdit::changeEvent(QEvent *event)
{
    QLineEdit::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange
        || event->type() == QEvent::PaletteChange) layoutBreadcrumbs();
}

void RemotePathEdit::focusInEvent(QFocusEvent *event)
{
    QLineEdit::focusInEvent(event);
    // Merely receiving focus (including automatic transfer during loading)
    // is not a request to edit. Mouse and shortcut handlers opt in explicitly.
    update();
}

void RemotePathEdit::focusOutEvent(QFocusEvent *event)
{
    QLineEdit::focusOutEvent(event);
    // Keep an in-progress edit while its native paste menu / IME popup is open.
    if (event->reason() == Qt::PopupFocusReason) return;
    endEditing();
}

void RemotePathEdit::mousePressEvent(QMouseEvent *event)
{
    if (!m_editing && event->button() == Qt::LeftButton) {
        beginEditing();
        event->accept();
        return;
    }
    QLineEdit::mousePressEvent(event);
}

void RemotePathEdit::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape && m_editing) {
        endEditing();
        event->accept();
        return;
    }
    if (!m_editing && hasFocus() && (event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete
            || (!event->text().isEmpty() && event->text().at(0).isPrint()
                && !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))))) {
        beginEditing();
    }
    QLineEdit::keyPressEvent(event);
}

} // namespace noxshell::ui
