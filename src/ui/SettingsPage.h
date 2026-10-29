#pragma once
#include <QWidget>
class QListWidget;
class QStackedWidget;
class QVBoxLayout;
class QGridLayout;
namespace noxshell { class CredentialStore; }
namespace noxshell::ui {
class SettingsPage final : public QWidget {
    Q_OBJECT
public:
    explicit SettingsPage(CredentialStore *credentials, QWidget *parent = nullptr);
signals:
    void themeModeRequested(const QString &mode);
protected:
    void changeEvent(QEvent *event) override;
private:
    QVBoxLayout *page(const QString &title, const QString &description = {});
    QGridLayout *section(QVBoxLayout *page, const QString &title);
    void toggle(QGridLayout *grid, const QString &key, const QString &title, const QString &description);
    void choice(QGridLayout *grid, const QString &key, const QString &title,
        const QStringList &labels, const QStringList &values, const QString &description = {});
    void number(QGridLayout *grid, const QString &key, const QString &title, int low, int high, const QString &description = {});
    QWidget *card(QGridLayout *grid, const QString &title, const QString &description, QWidget *control);
    void buildGeneral(); void buildAppearance(); void buildShortcuts(); void buildTabs();
    void buildAi(); void buildAgents(); void buildHighlight(); void buildProxy(); void buildAbout();
    void refreshStyle();
    bool m_styleInitialized{},m_darkStyle{};
    CredentialStore *m_credentials{};
    QListWidget *m_navigation{};
    QStackedWidget *m_pages{};
};
}
