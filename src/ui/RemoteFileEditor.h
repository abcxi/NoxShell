#pragma once

#include <QDialog>
#include <QVector>

class QCloseEvent;
class QEvent;
class QLabel;
class QLineEdit;
class QStackedWidget;
class QTabBar;
class QToolButton;

namespace noxshell {
class SshSession;
}

namespace noxshell::ui {

class RemoteFileEditor final : public QDialog {
    Q_OBJECT

public:
    explicit RemoteFileEditor(SshSession *session, QString serverName, QString remotePath, QWidget *parent = nullptr);
    ~RemoteFileEditor() override;

    void openFile(const QString &remotePath);
    [[nodiscard]] QString remotePath() const;

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Document;

    [[nodiscard]] Document *documentForPage(QWidget *page) const;
    [[nodiscard]] Document *documentForRequest(quint64 requestId, bool writeRequest) const;
    [[nodiscard]] int documentIndex(const Document *document) const;
    [[nodiscard]] Document *activeDocument() const;
    void beginLoad(Document *document);
    void saveDocument(Document *document);
    void setBusy(Document *document, bool busy, const QString &message);
    void setDirty(Document *document, bool dirty);
    void updateTab(Document *document);
    bool requestCloseDocument(int index);
    void removeDocument(int index);
    void maybeFinishWindowClose();
    void showFindPanel(bool showReplace);
    void hideFindPanel();
    bool findNext(bool backwards);
    void updateFindStatus();
    void replaceCurrent();
    void replaceAll();

    SshSession *m_session{};
    QString m_serverName;
    QTabBar *m_tabs{};
    QStackedWidget *m_stack{};
    QWidget *m_findPanel{};
    QWidget *m_replaceRow{};
    QLineEdit *m_findEdit{};
    QLineEdit *m_replaceEdit{};
    QLabel *m_findStatus{};
    QToolButton *m_caseSensitive{};
    QToolButton *m_replaceToggle{};
    QVector<Document *> m_documents;
    bool m_closeAfterAllSaved{false};
};

} // namespace noxshell::ui
