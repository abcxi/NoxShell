#include <QWidget>
#import <AppKit/AppKit.h>

// Exercise the actual Cocoa close, Dock reopen, and termination routes using
// synthetic sessions and temporary data; never automate the installed app.
void nativeProbeCloseWindow(QWidget *window)
{
    NSView *view = reinterpret_cast<NSView *>(window->winId());
    [view.window performClose:nil];
}

void nativeProbeReopenApplication()
{
    [NSApp.delegate applicationShouldHandleReopen:NSApp hasVisibleWindows:NO];
}

void nativeProbeQuitApplication()
{
    [NSApp terminate:nil];
}

bool nativeProbeTerminalMarkText(QWidget *terminal, const QString &text)
{
    NSView *view = reinterpret_cast<NSView *>(terminal->winId());
    id client = view.window.firstResponder;
    if (![client conformsToProtocol:@protocol(NSTextInputClient)]) return false;
    [(id<NSTextInputClient>)client setMarkedText:text.toNSString()
        selectedRange:NSMakeRange(text.size(), 0) replacementRange:NSMakeRange(NSNotFound, 0)];
    return true;
}

bool nativeProbeTerminalCommitText(QWidget *terminal, const QString &text)
{
    NSView *view = reinterpret_cast<NSView *>(terminal->winId());
    id client = view.window.firstResponder;
    if (![client conformsToProtocol:@protocol(NSTextInputClient)]) return false;
    [(id<NSTextInputClient>)client insertText:text.toNSString() replacementRange:NSMakeRange(NSNotFound, 0)];
    return true;
}

bool nativeProbeTerminalEscape(QWidget *terminal)
{
    NSView *view = reinterpret_cast<NSView *>(terminal->winId());
    if (!view.window) return false;
    for (auto type : {NSEventTypeKeyDown, NSEventTypeKeyUp}) {
        NSEvent *event = [NSEvent keyEventWithType:type location:NSZeroPoint modifierFlags:0
            timestamp:0 windowNumber:view.window.windowNumber context:nil
            characters:@"\x1b" charactersIgnoringModifiers:@"\x1b" isARepeat:NO keyCode:53];
        [NSApp sendEvent:event];
    }
    return true;
}
