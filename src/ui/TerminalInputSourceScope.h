#pragma once

#include <QString>
#include <functional>

namespace noxshell::ui {

// Keep source selection separate from Cocoa so restoration/manual overrides can
// be tested without changing the user's keyboard. Only used on focus transitions.
struct TerminalInputSourceBackend {
    std::function<QString()> current;
    std::function<bool()> isAscii;
    std::function<QString()> asciiSource;
    std::function<bool(const QString &)> select;
};

class TerminalInputSourceScope {
public:
    void enter(bool enabled, const TerminalInputSourceBackend &backend)
    {
        if (!enabled || !m_selected.isEmpty() || backend.isAscii()) return;
        const auto previous = backend.current();
        const auto selected = backend.asciiSource();
        if (previous.isEmpty() || selected.isEmpty() || previous == selected) return;
        if (backend.select(selected)) {
            m_previous = previous;
            m_selected = selected;
        }
    }

    void leave(bool restoreAllowed, const TerminalInputSourceBackend &backend)
    {
        // A manual source change wins over our earlier automatic selection.
        if (restoreAllowed && !m_selected.isEmpty() && backend.current() == m_selected)
            backend.select(m_previous);
        m_previous.clear();
        m_selected.clear();
    }

private:
    QString m_previous;
    QString m_selected;
};

} // namespace noxshell::ui
