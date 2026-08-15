#include "MacTitleBarControls.h"

#include <QMainWindow>

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include <utility>

namespace {

char kTitleBarControlsKey;

NSImage *symbolImage(NSString *name, NSString *fallbackName)
{
    NSImage *image = nil;
    if (@available(macOS 11.0, *)) {
        image = [NSImage imageWithSystemSymbolName:name accessibilityDescription:nil];
    }
    if (!image) image = [NSImage imageNamed:fallbackName];
    [image setTemplate:YES];
    return image;
}

NSWindow *nativeWindow(QMainWindow *window)
{
    if (!window) return nil;
    auto *nativeViewPointer = reinterpret_cast<void *>(window->winId());
    NSView *nativeView = (__bridge NSView *)nativeViewPointer;
    return nativeView.window;
}

} // namespace

@interface YQTitleBarControlsBridge : NSObject {
@public
    std::function<void()> sidebarHandler;
    std::function<void()> monitorHandler;
}
@property(nonatomic, strong) NSButton *sidebarButton;
@property(nonatomic, strong) NSButton *monitorButton;
@property(nonatomic, strong) NSTitlebarAccessoryViewController *controller;
- (void)toggleSidebar:(id)sender;
- (void)toggleMonitor:(id)sender;
@end

@implementation YQTitleBarControlsBridge
- (void)toggleSidebar:(id)sender
{
    (void)sender;
    if (sidebarHandler) sidebarHandler();
}

- (void)toggleMonitor:(id)sender
{
    (void)sender;
    if (monitorHandler) monitorHandler();
}
@end

namespace noxshell::ui {

bool installMacTitleBarControls(QMainWindow *window,
    std::function<void()> toggleSidebar,
    std::function<void()> toggleMonitor)
{
    NSWindow *native = nativeWindow(window);
    if (!native) return false;
    if (objc_getAssociatedObject(native, &kTitleBarControlsKey)) return true;

    auto *bridge = [[YQTitleBarControlsBridge alloc] init];
    bridge->sidebarHandler = std::move(toggleSidebar);
    bridge->monitorHandler = std::move(toggleMonitor);

    auto makeButton = [](NSImage *image, id target, SEL action) {
        auto *button = [[NSButton alloc] initWithFrame:NSMakeRect(0, 0, 28, 24)];
        button.image = image;
        button.imagePosition = NSImageOnly;
        button.bordered = NO;
        button.bezelStyle = NSBezelStyleInline;
        button.target = target;
        button.action = action;
        button.translatesAutoresizingMaskIntoConstraints = NO;
        [button.widthAnchor constraintEqualToConstant:28.0].active = YES;
        [button.heightAnchor constraintEqualToConstant:24.0].active = YES;
        return button;
    };

    bridge.sidebarButton = makeButton(symbolImage(@"sidebar.left", NSImageNameListViewTemplate),
        bridge, @selector(toggleSidebar:));
    bridge.monitorButton = makeButton(symbolImage(@"rectangle.split.3x1", NSImageNameColumnViewTemplate),
        bridge, @selector(toggleMonitor:));

    auto *stack = [[NSStackView alloc] initWithFrame:NSMakeRect(0, 0, 60, 28)];
    stack.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    stack.alignment = NSLayoutAttributeCenterY;
    stack.distribution = NSStackViewDistributionFill;
    stack.spacing = 2.0;
    [stack addArrangedSubview:bridge.sidebarButton];
    [stack addArrangedSubview:bridge.monitorButton];

    auto *controller = [[NSTitlebarAccessoryViewController alloc] init];
    controller.layoutAttribute = NSLayoutAttributeLeft;
    controller.view = stack;
    bridge.controller = controller;
    [native addTitlebarAccessoryViewController:controller];
    objc_setAssociatedObject(native, &kTitleBarControlsKey, bridge, OBJC_ASSOCIATION_RETAIN_NONATOMIC);

    updateMacTitleBarControls(window, true, true);
    return true;
}

void updateMacTitleBarControls(QMainWindow *window, bool sidebarVisible, bool monitorVisible)
{
    NSWindow *native = nativeWindow(window);
    auto *bridge = native ? (YQTitleBarControlsBridge *)objc_getAssociatedObject(native, &kTitleBarControlsKey) : nil;
    if (!bridge) return;

    bridge.sidebarButton.toolTip = sidebarVisible ? @"隐藏主机列表" : @"显示主机列表";
    bridge.monitorButton.toolTip = monitorVisible ? @"隐藏实时监控栏" : @"显示实时监控栏";
    bridge.sidebarButton.accessibilityLabel = bridge.sidebarButton.toolTip;
    bridge.monitorButton.accessibilityLabel = bridge.monitorButton.toolTip;
    bridge.sidebarButton.contentTintColor = sidebarVisible ? NSColor.labelColor : NSColor.secondaryLabelColor;
    bridge.monitorButton.contentTintColor = monitorVisible ? NSColor.labelColor : NSColor.secondaryLabelColor;
}

} // namespace noxshell::ui
