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
