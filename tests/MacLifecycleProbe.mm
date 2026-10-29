#include <QWidget>
#include <QMenu>
#include <QAction>
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

bool nativeProbeTerminalOptionArrow(QWidget *terminal, bool left)
{
    NSView *view = reinterpret_cast<NSView *>(terminal->winId());
    if (!view.window) return false;
    const auto optionFlags = [&](NSEventModifierFlags flags) {
        [NSApp sendEvent:[NSEvent keyEventWithType:NSEventTypeFlagsChanged location:NSZeroPoint
            modifierFlags:flags timestamp:0 windowNumber:view.window.windowNumber context:nil
            characters:@"" charactersIgnoringModifiers:@"" isARepeat:NO keyCode:58]];
    };
    optionFlags(NSEventModifierFlagOption);
    const unichar character = left ? NSLeftArrowFunctionKey : NSRightArrowFunctionKey;
    NSString *characters = [NSString stringWithCharacters:&character length:1];
    for (auto type : {NSEventTypeKeyDown, NSEventTypeKeyUp}) {
        NSEvent *event = [NSEvent keyEventWithType:type location:NSZeroPoint
            modifierFlags:(NSEventModifierFlagOption | NSEventModifierFlagNumericPad | NSEventModifierFlagFunction) timestamp:0
            windowNumber:view.window.windowNumber context:nil characters:characters
            charactersIgnoringModifiers:characters isARepeat:NO keyCode:left ? 123 : 124];
        [NSApp sendEvent:event];
    }
    optionFlags(0); // Match a physical Option release; do not leak test modifier state.
    return true;
}

namespace {
NSButton *titlebarButton(NSView *view, NSString *identifier)
{
    if ([view isKindOfClass:NSButton.class]
        && [view.accessibilityIdentifier isEqualToString:identifier]) return (NSButton *)view;
    for (NSView *child in view.subviews) {
        if (NSButton *button = titlebarButton(child, identifier)) return button;
    }
    return nil;
}

NSButton *titlebarButton(QWidget *window, const QString &identifier)
{
    NSView *view = reinterpret_cast<NSView *>(window->winId());
    for (NSTitlebarAccessoryViewController *accessory in view.window.titlebarAccessoryViewControllers) {
        if (NSButton *button = titlebarButton(accessory.view, identifier.toNSString())) return button;
    }
    return nil;
}

NSPopover *connectionPopover(QWidget *window)
{
    NSButton *button = titlebarButton(window, QStringLiteral("newConnectionButton"));
    return button ? [button valueForKey:@"hoverPopover"] : nil;
}
} // namespace

bool nativeProbeTitlebarClick(QWidget *window, const QString &identifier)
{
    NSButton *button = titlebarButton(window, identifier);
    if (!button) button = titlebarButton(connectionPopover(window).contentViewController.view, identifier.toNSString());
    if (!button) return false;
    [button performClick:nil];
    return true;
}

bool nativeProbeTitlebarSelect(QWidget *window, int index, int mode)
{
    NSButton *button = titlebarButton(window, QStringLiteral("sessionTab-%1").arg(index));
    if (!button || ![button respondsToSelector:@selector(setSelectionMode:)]) return false;
    [button setValue:@(mode) forKey:@"selectionMode"];
    [button performClick:nil]; return true;
}

bool nativeProbeTitlebarTabMenu(QWidget *window, int index, const QString &actionName)
{
    NSButton *button = titlebarButton(window, QStringLiteral("sessionTab-%1").arg(index));
    NSMenu *menu = button.menu;
    auto *source = window->findChild<QMenu *>(QStringLiteral("terminalTabMenu"));
    auto *action = window->findChild<QAction *>(actionName);
    if (!menu || !source || !action) return false;
    [menu.delegate menuNeedsUpdate:menu];
    const int position = source->actions().indexOf(action);
    if (position < 0 || position >= menu.numberOfItems) return false;
    NSMenuItem *item = [menu itemAtIndex:position];
    if (!item.enabled) return false;
    return [NSApp sendAction:item.action to:item.target from:item];
}

bool nativeProbeTitlebarHover(QWidget *window)
{
    NSButton *button = titlebarButton(window, QStringLiteral("newConnectionButton"));
    if (!button) return false;
    [NSApp activateIgnoringOtherApps:YES];
    [button.window makeKeyAndOrderFront:nil];
    [button mouseEntered:[NSEvent enterExitEventWithType:NSEventTypeMouseEntered location:NSZeroPoint
        modifierFlags:0 timestamp:0 windowNumber:button.window.windowNumber context:nil eventNumber:0 trackingNumber:0 userData:nullptr]];
    return true;
}

bool nativeProbeSelectTheme(QWidget *window, int mode)
{
    NSButton *button = titlebarButton(window, QStringLiteral("themeModeButton"));
    if (!button || ![button.target respondsToSelector:@selector(themeMenu)]) return false;
    NSMenu *menu = [button.target valueForKey:@"themeMenu"];
    if (mode < 0 || mode >= menu.numberOfItems) return false;
    NSMenuItem *item = [menu itemAtIndex:mode];
    return [NSApp sendAction:item.action to:item.target from:item];
}

bool nativeProbeTitlebarMenuVisible(QWidget *window)
{
    return connectionPopover(window).shown;
}

bool nativeProbeCaptureWindow(QWidget *window, const QString &path)
{
    NSView *view = reinterpret_cast<NSView *>(window->winId());
    NSView *frame = view.window.contentView.superview;
    NSBitmapImageRep *bitmap = [frame bitmapImageRepForCachingDisplayInRect:frame.bounds];
    if (!bitmap) return false;
    [frame cacheDisplayInRect:frame.bounds toBitmapImageRep:bitmap];
    NSData *png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    return [png writeToFile:path.toNSString() atomically:YES];
}
