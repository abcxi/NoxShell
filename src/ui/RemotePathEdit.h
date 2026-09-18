#pragma once

#include <QLineEdit>
#include <QList>

class QMenu;
class QToolButton;

namespace noxshell::ui {

// A path is only submitted on Enter or an explicit breadcrumb click. Blurring
// the editor must never navigate to an unfinished path.
class RemotePathEdit final : public QLineEdit {
    Q_OBJECT

public:
    explicit RemotePathEdit(QWidget *parent = nullptr);
    void setPath(const QString &path);
    void beginEditing();
    [[nodiscard]] bool isEditing() const { return m_editing; }

signals:
    void pathActivated(const QString &path);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void endEditing();
    void layoutBreadcrumbs();
    QToolButton *makeButton(const QString &name);

    QString m_path{QStringLiteral("/")};
    QList<QToolButton *> m_segments;
    QList<QRect> m_separators;
    QToolButton *m_overflow{};
    QMenu *m_overflowMenu{};
    QToolButton *m_editButton{};
    bool m_editing{false};
    bool m_layingOut{false};
};

} // namespace noxshell::ui
