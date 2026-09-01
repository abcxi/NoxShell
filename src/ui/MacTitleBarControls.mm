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

@interface NoxShellTitleBarControlsBridge : NSObject {
@public
    std::function<void()> sidebarHandler;
    std::function<void()> monitorHandler;
    std::function<void()> settingsHandler;
    std::function<void(int)> themeHandler;
}
@property(nonatomic, strong) NSButton *sidebarButton;
@property(nonatomic, strong) NSButton *monitorButton;
@property(nonatomic, strong) NSButton *settingsButton;
@property(nonatomic, strong) NSButton *themeButton;
@property(nonatomic, strong) NSMenu *themeMenu;
@property(nonatomic, strong) NSTitlebarAccessoryViewController *leftController;
@property(nonatomic, strong) NSTitlebarAccessoryViewController *rightController;
- (void)toggleSidebar:(id)sender;
- (void)toggleMonitor:(id)sender;
- (void)openSettings:(id)sender;
- (void)showThemeMenu:(id)sender;
- (void)selectTheme:(id)sender;
@end

@implementation NoxShellTitleBarControlsBridge
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

- (void)openSettings:(id)sender
{
    (void)sender;
    if (settingsHandler) settingsHandler();
}

- (void)showThemeMenu:(id)sender
{
    auto *button = (NSButton *)sender;
    [self.themeMenu popUpMenuPositioningItem:nil
        atLocation:NSMakePoint(0, button.bounds.size.height + 2) inView:button];
}

- (void)selectTheme:(id)sender
{
    auto *item = (NSMenuItem *)sender;
    if (themeHandler) themeHandler(static_cast<int>(item.tag));
}
@end

namespace noxshell::ui {

bool installMacTitleBarControls(QMainWindow *window,
    std::function<void()> toggleSidebar,
    std::function<void()> toggleMonitor,
    std::function<void()> openTerminalSettings,
    std::function<void(int)> selectTheme,
    int themeMode)
{
    NSWindow *native = nativeWindow(window);
    if (!native) return false;
    if (objc_getAssociatedObject(native, &kTitleBarControlsKey)) return true;

    auto *bridge = [[NoxShellTitleBarControlsBridge alloc] init];
    bridge->sidebarHandler = std::move(toggleSidebar);
    bridge->monitorHandler = std::move(toggleMonitor);
    bridge->settingsHandler = std::move(openTerminalSettings);
    bridge->themeHandler = std::move(selectTheme);

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
    bridge.settingsButton = makeButton(symbolImage(@"gearshape", NSImageNameActionTemplate),
        bridge, @selector(openSettings:));
    bridge.settingsButton.toolTip = @"终端显示设置";
    bridge.settingsButton.accessibilityLabel = bridge.settingsButton.toolTip;
    bridge.themeButton = makeButton(symbolImage(@"circle.lefthalf.filled", NSImageNameColorPanel),
        bridge, @selector(showThemeMenu:));
    bridge.themeButton.toolTip = @"界面外观";
    bridge.themeButton.accessibilityLabel = bridge.themeButton.toolTip;
    bridge.themeMenu = [[NSMenu alloc] initWithTitle:@"界面外观"];
    const NSArray<NSString *> *themeTitles = @[@"跟随系统", @"亮色模式", @"暗色模式"];
    for (NSInteger index = 0; index < themeTitles.count; ++index) {
        auto *item = [[NSMenuItem alloc] initWithTitle:themeTitles[index]
            action:@selector(selectTheme:) keyEquivalent:@""];
        item.target = bridge;
        item.tag = index;
        [bridge.themeMenu addItem:item];
    }

    auto *stack = [[NSStackView alloc] initWithFrame:NSMakeRect(0, 0, 60, 28)];
    stack.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    stack.alignment = NSLayoutAttributeCenterY;
    stack.distribution = NSStackViewDistributionFill;
    stack.spacing = 2.0;
    [stack addArrangedSubview:bridge.sidebarButton];
    [stack addArrangedSubview:bridge.monitorButton];

    auto *leftController = [[NSTitlebarAccessoryViewController alloc] init];
    leftController.layoutAttribute = NSLayoutAttributeLeft;
    leftController.view = stack;
    bridge.leftController = leftController;
    [native addTitlebarAccessoryViewController:leftController];

    auto *rightStack = [[NSStackView alloc] initWithFrame:NSMakeRect(0, 0, 62, 28)];
    rightStack.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    rightStack.alignment = NSLayoutAttributeCenterY;
    rightStack.spacing = 2.0;
    [rightStack addArrangedSubview:bridge.themeButton];
    [rightStack addArrangedSubview:bridge.settingsButton];
    auto *rightController = [[NSTitlebarAccessoryViewController alloc] init];
    rightController.layoutAttribute = NSLayoutAttributeRight;
    rightController.view = rightStack;
    bridge.rightController = rightController;
    [native addTitlebarAccessoryViewController:rightController];
    objc_setAssociatedObject(native, &kTitleBarControlsKey, bridge, OBJC_ASSOCIATION_RETAIN_NONATOMIC);

    updateMacTitleBarControls(window, true, true, themeMode);
    return true;
}

void updateMacTitleBarControls(QMainWindow *window, bool sidebarVisible, bool monitorVisible, int themeMode)
{
    NSWindow *native = nativeWindow(window);
    auto *bridge = native ? (NoxShellTitleBarControlsBridge *)objc_getAssociatedObject(native, &kTitleBarControlsKey) : nil;
    if (!bridge) return;

    bridge.sidebarButton.toolTip = sidebarVisible ? @"隐藏主机列表" : @"显示主机列表";
    bridge.monitorButton.toolTip = monitorVisible ? @"隐藏实时监控栏" : @"显示实时监控栏";
    bridge.sidebarButton.accessibilityLabel = bridge.sidebarButton.toolTip;
    bridge.monitorButton.accessibilityLabel = bridge.monitorButton.toolTip;
    bridge.sidebarButton.contentTintColor = sidebarVisible ? NSColor.labelColor : NSColor.secondaryLabelColor;
    bridge.monitorButton.contentTintColor = monitorVisible ? NSColor.labelColor : NSColor.secondaryLabelColor;
    for (NSMenuItem *item in bridge.themeMenu.itemArray) {
        item.state = item.tag == themeMode ? NSControlStateValueOn : NSControlStateValueOff;
    }
    const NSArray<NSString *> *themeNames = @[@"跟随系统", @"亮色模式", @"暗色模式"];
    if (themeMode >= 0 && themeMode < static_cast<int>(themeNames.count)) {
        bridge.themeButton.toolTip = [@"界面外观：" stringByAppendingString:themeNames[themeMode]];
        bridge.themeButton.accessibilityLabel = bridge.themeButton.toolTip;
    }
    native.appearance = themeMode == 1
        ? [NSAppearance appearanceNamed:NSAppearanceNameAqua]
        : themeMode == 2 ? [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua] : nil;
}

} // namespace noxshell::ui
