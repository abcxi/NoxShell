#include "MacTerminalInput.h"

#include <QApplication>
#include <QKeyEvent>
#include <QWidget>
#import <AppKit/AppKit.h>
#include <Carbon/Carbon.h>

namespace noxshell::ui {
namespace {
QString sourceId(TISInputSourceRef source)
{
    if (!source) return {};
    auto value = static_cast<CFStringRef>(TISGetInputSourceProperty(source, kTISPropertyInputSourceID));
    return value ? QString::fromCFString(value) : QString();
}

const TerminalInputSourceBackend &systemBackend()
{
    static const TerminalInputSourceBackend backend{
        [] {
            auto source = TISCopyCurrentKeyboardInputSource();
            const auto id = sourceId(source);
            if (source) CFRelease(source);
            return id;
        },
        [] {
            auto source = TISCopyCurrentKeyboardInputSource();
            const bool ascii = source && TISGetInputSourceProperty(source, kTISPropertyInputSourceIsASCIICapable) == kCFBooleanTrue;
            if (source) CFRelease(source);
            return ascii;
        },
        [] {
            auto source = TISCopyCurrentASCIICapableKeyboardLayoutInputSource();
            const auto id = sourceId(source);
            if (source) CFRelease(source);
            return id;
        },
        [](const QString &id) {
            CFStringRef value = id.toCFString();
            const void *keys[] = {kTISPropertyInputSourceID};
            const void *values[] = {value};
            auto filter = CFDictionaryCreate(nullptr, keys, values, 1,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            auto sources = TISCreateInputSourceList(filter, false);
            bool selected = false;
            if (sources && CFArrayGetCount(sources) > 0) {
                auto source = static_cast<TISInputSourceRef>(const_cast<void *>(CFArrayGetValueAtIndex(sources, 0)));
                selected = TISSelectInputSource(source) == noErr;
            }
            if (sources) CFRelease(sources);
            CFRelease(filter);
            CFRelease(value);
            return selected;
        }
    };
    return backend;
}

bool maySwitchSource()
{
    // Useful for native regression probes: never change the user's input source.
    return QApplication::platformName() == QStringLiteral("cocoa") && [NSApp isActive]
        && !qEnvironmentVariableIsSet("NOXSHELL_DISABLE_TERMINAL_INPUT_SOURCE_SWITCHING");
}
}

MacTerminalInput::MacTerminalInput(QWidget *terminal) : m_terminal(terminal)
{
    qApp->installNativeEventFilter(this);
}

MacTerminalInput::~MacTerminalInput()
{
    qApp->removeNativeEventFilter(this);
    focusOut();
}

void MacTerminalInput::focusIn(bool autoEnglish)
{
    if (maySwitchSource()) m_sourceScope.enter(autoEnglish, systemBackend());
}

void MacTerminalInput::focusOut()
{
    m_sourceScope.leave(maySwitchSource(), systemBackend());
}

bool MacTerminalInput::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result)
{
    if (eventType != "mac_generic_NSEvent" || QApplication::focusWidget() != m_terminal
        || !m_terminal->isEnabled() || !m_terminal->isActiveWindow()) return false;
    NSEvent *event = static_cast<NSEvent *>(message);
    if ((event.type != NSEventTypeKeyDown && event.type != NSEventTypeKeyUp) || event.keyCode != 53
        || (event.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl
            | NSEventModifierFlagOption | NSEventModifierFlagShift))) return false;
    // Deliver bare Esc before the IME swallows it, through Qt's normal object
    // filters (e.g. history popup). No global monitor or accessibility access.
    QKeyEvent key(event.type == NSEventTypeKeyDown ? QEvent::KeyPress : QEvent::KeyRelease,
        Qt::Key_Escape, Qt::NoModifier, QString(QChar(0x1b)), event.isARepeat);
    QApplication::sendEvent(m_terminal, &key);
    if (result) *result = 0;
    return true;
}

} // namespace noxshell::ui
