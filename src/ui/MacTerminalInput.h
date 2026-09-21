#pragma once

#include "TerminalInputSourceScope.h"
#include <QAbstractNativeEventFilter>

class QWidget;

namespace noxshell::ui {

class MacTerminalInput final : public QAbstractNativeEventFilter {
public:
    explicit MacTerminalInput(QWidget *terminal);
    ~MacTerminalInput() override;
    void focusIn(bool autoEnglish);
    void focusOut();
    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

private:
    QWidget *m_terminal;
    TerminalInputSourceScope m_sourceScope;
};

} // namespace noxshell::ui
