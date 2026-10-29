#include "MacTitleBarControls.h"

#include <QMainWindow>
#include <QTabBar>
#include <QPointer>
#include <QMenu>
#include <QAction>
#include <QBuffer>

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

@interface NoxShellHoverMenuButton : NSButton
@property(nonatomic, strong) NSPopover *hoverPopover;
@property(nonatomic, strong) NSTimer *hoverTimer;
@end

@implementation NoxShellHoverMenuButton
- (void)updateTrackingAreas
{
    [super updateTrackingAreas];
    for (NSTrackingArea *area in [self.trackingAreas copy]) [self removeTrackingArea:area];
    const auto options = NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways | NSTrackingInVisibleRect;
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:NSZeroRect options:options owner:self userInfo:nil]];
}

- (void)mouseEntered:(NSEvent *)event
{
    [super mouseEntered:event];
    [self.hoverTimer invalidate];
    __weak NoxShellHoverMenuButton *weakSelf = self;
    self.hoverTimer = [NSTimer scheduledTimerWithTimeInterval:0.2 repeats:NO block:^(NSTimer *) {
        NoxShellHoverMenuButton *button = weakSelf;
        if (button.window.isKeyWindow && !button.hoverPopover.shown) {
            [button.hoverPopover showRelativeToRect:button.bounds ofView:button preferredEdge:NSRectEdgeMinY];
        }
    }];
}

- (void)mouseExited:(NSEvent *)event
{
    [super mouseExited:event];
    [self.hoverTimer invalidate];
    __weak NoxShellHoverMenuButton *weakSelf = self;
    self.hoverTimer = [NSTimer scheduledTimerWithTimeInterval:0.25 repeats:NO block:^(NSTimer *) {
        NoxShellHoverMenuButton *button = weakSelf;
        NSView *content = button.hoverPopover.contentViewController.view;
        NSPoint point = [content convertPoint:[content.window convertPointFromScreen:NSEvent.mouseLocation] fromView:nil];
        if (!NSPointInRect(point, content.bounds)) [button.hoverPopover close];
    }];
}

- (void)mouseDown:(NSEvent *)event
{
    [self.hoverTimer invalidate];
    [self.hoverPopover close];
    [super mouseDown:event];
}
@end

@interface NoxShellSessionTabButton : NSButton
@property(nonatomic) NSInteger selectionMode;
@end

@implementation NoxShellSessionTabButton
- (void)mouseDown:(NSEvent *)event
{
    const auto flags = event.modifierFlags;
    self.selectionMode = (flags & NSEventModifierFlagShift) ? 2
        : (flags & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) ? 1 : 0;
    [super mouseDown:event];
}
@end

@interface NoxShellTitleBarControlsBridge : NSObject <NSMenuDelegate> {
@public
    std::function<void()> addConnectionHandler;
    std::function<void()> credentialManagerHandler;
    std::function<void()> serverManagerHandler;
    std::function<void()> monitorHandler;
    std::function<void()> settingsHandler;
    std::function<void(int)> themeHandler;
    QPointer<QTabBar> sessionTabs;
}
@property(nonatomic, strong) NoxShellHoverMenuButton *addButton;
@property(nonatomic, strong) NSPopover *addPopover;
@property(nonatomic, strong) NSButton *monitorButton;
@property(nonatomic, strong) NSScrollView *tabsScroll;
@property(nonatomic, strong) NSView *tabsDocument;
@property(nonatomic, weak) NSWindow *window;
@property(nonatomic, strong) id resizeObserver;
@property(nonatomic, strong) NSButton *settingsButton;
@property(nonatomic, strong) NSButton *themeButton;
@property(nonatomic, strong) NSMenu *themeMenu;
@property(nonatomic, strong) NSTitlebarAccessoryViewController *leftController;
@property(nonatomic, strong) NSTitlebarAccessoryViewController *rightController;
- (void)toggleMonitor:(id)sender;
- (void)addConnection:(id)sender;
- (void)openCredentialManager:(id)sender;
- (void)openServerManager:(id)sender;
- (void)openSettings:(id)sender;
- (void)showThemeMenu:(id)sender;
- (void)selectTheme:(id)sender;
- (void)activateSessionTab:(id)sender;
- (void)closeSessionTab:(id)sender;
- (void)sessionTabMenuAction:(id)sender;
- (void)rebuildSessionTabs;
- (void)resizeSessionTabs;
@end

@implementation NoxShellTitleBarControlsBridge
- (void)dealloc
{
    if (self.resizeObserver) [NSNotificationCenter.defaultCenter removeObserver:self.resizeObserver];
}

- (void)activateSessionTab:(id)sender
{
    if (!sessionTabs) return;
    const int index = static_cast<int>([(NSButton *)sender tag]);
    const int mode = static_cast<int>(((NoxShellSessionTabButton *)sender).selectionMode);
    auto *menu = sessionTabs->window()->findChild<QMenu *>(QStringLiteral("terminalTabMenu"));
    if (menu) QMetaObject::invokeMethod(menu->parent(), "selectSessionTab", Qt::DirectConnection, Q_ARG(int, index), Q_ARG(int, mode));
}

- (void)menuNeedsUpdate:(NSMenu *)menu
{
    if (!sessionTabs || menu.numberOfItems == 0) return;
    auto *source = sessionTabs->window()->findChild<QMenu *>(QStringLiteral("terminalTabMenu"));
    if (!source) return;
    const int index = static_cast<int>([menu itemAtIndex:0].tag / 100);
    bool prepared = false;
    QMetaObject::invokeMethod(source->parent(), "prepareTabContextMenu", Qt::DirectConnection,
        Q_RETURN_ARG(bool, prepared), Q_ARG(int, index));
    for (NSMenuItem *item in menu.itemArray) {
        if (item.isSeparatorItem) continue;
        const int action = static_cast<int>(item.tag % 100);
        item.enabled = prepared && action < source->actions().size() && source->actions()[action]->isEnabled();
    }
}

- (void)closeSessionTab:(id)sender
{
    if (!sessionTabs) return;
    emit sessionTabs->tabCloseRequested(static_cast<int>([(NSButton *)sender tag]));
}

- (void)sessionTabMenuAction:(id)sender
{
    if (!sessionTabs) return;
    const auto tag = [(NSMenuItem *)sender tag];
    auto *menu = sessionTabs->window()->findChild<QMenu *>(QStringLiteral("terminalTabMenu"));
    if (!menu) return;
    const int index = static_cast<int>(tag / 100);
    bool prepared = false;
    QMetaObject::invokeMethod(menu->parent(), "prepareTabContextMenu", Qt::DirectConnection,
        Q_RETURN_ARG(bool, prepared), Q_ARG(int, index));
    const int action = static_cast<int>(tag % 100);
    if (prepared && action < menu->actions().size()) menu->actions().at(action)->trigger();
}

- (void)resizeSessionTabs
{
    if (!self.tabsScroll || !sessionTabs) return;
    const CGFloat width = std::max<CGFloat>(140, std::min<CGFloat>(self.window.frame.size.width - 250,
        sessionTabs->count() * 148 + 30));
    self.tabsScroll.frame = NSMakeRect(60, 0, width, 28);
    self.leftController.view.frame = NSMakeRect(0, 0, 60 + width, 28);
    self.window.titleVisibility = sessionTabs->count() > 0 ? NSWindowTitleHidden : NSWindowTitleVisible;
}

- (void)rebuildSessionTabs
{
    if (!sessionTabs || !self.tabsDocument) return;
    for (NSView *view in [self.tabsDocument.subviews copy]) [view removeFromSuperview];
    const int count = sessionTabs->count();
    self.tabsScroll.hidden = count == 0;
    if (count == 0) {
        self.leftController.view.frame = NSMakeRect(0, 0, 60, 28);
        self.window.titleVisibility = NSWindowTitleVisible;
        return;
    }
    for (int index = 0; index < count; ++index) {
        const bool selected = index == sessionTabs->currentIndex();
        const auto mergedSelection = sessionTabs->property("multiSelectedTabs").toList();
        const bool multiSelected = mergedSelection.size() > 1 && mergedSelection.contains(index);
        auto *tab = [[NSView alloc] initWithFrame:NSMakeRect(index * 148, 0, 146, 28)];
        tab.wantsLayer = YES;
        tab.layer.cornerRadius = 4;
        tab.layer.backgroundColor = (selected ? [NSColor.controlAccentColor colorWithAlphaComponent:0.16]
            : NSColor.clearColor).CGColor;
        tab.layer.borderWidth = multiSelected ? 1.5 : 0;
        tab.layer.borderColor = NSColor.controlAccentColor.CGColor;
        auto *button = [[NoxShellSessionTabButton alloc] initWithFrame:NSMakeRect(4, 2, 115, 24)];
        button.title = sessionTabs->tabText(index).toNSString();
        button.target = self; button.action = @selector(activateSessionTab:);
        button.bordered = NO;
        button.tag = index;
        button.font = [NSFont systemFontOfSize:12 weight:selected ? NSFontWeightSemibold : NSFontWeightRegular];
        button.alignment = NSTextAlignmentLeft;
        button.lineBreakMode = NSLineBreakByTruncatingTail;
        button.toolTip = sessionTabs->tabToolTip(index).toNSString();
        button.accessibilityIdentifier = [NSString stringWithFormat:@"sessionTab-%d", index];
        QBuffer icon;
        icon.open(QIODevice::WriteOnly);
        sessionTabs->tabIcon(index).pixmap(14, 14).save(&icon, "PNG");
        button.image = [[NSImage alloc] initWithData:[NSData dataWithBytes:icon.data().constData()
            length:icon.data().size()]];
        button.image.size = NSMakeSize(14, 14);
        button.imagePosition = NSImageLeft;
        [tab addSubview:button];
        auto *close = [NSButton buttonWithImage:symbolImage(@"xmark", NSImageNameStopProgressTemplate)
            target:self action:@selector(closeSessionTab:)];
        close.frame = NSMakeRect(120, 4, 22, 20);
        close.bordered = NO;
        close.tag = index;
        close.toolTip = @"关闭选项卡及其所有分屏";
        close.accessibilityIdentifier = [NSString stringWithFormat:@"sessionTabClose-%d", index];
        [tab addSubview:close];
        // Keep the existing tab context actions available on the native tabs.
        auto *sourceMenu = sessionTabs->window()->findChild<QMenu *>(QStringLiteral("terminalTabMenu"));
        if (sourceMenu) {
            auto *menu = [[NSMenu alloc] initWithTitle:@"会话操作"];
            menu.delegate = self;
            menu.autoenablesItems = NO;
            for (int actionIndex = 0; actionIndex < sourceMenu->actions().size(); ++actionIndex) {
                auto *action = sourceMenu->actions().at(actionIndex);
                if (action->isSeparator()) { [menu addItem:NSMenuItem.separatorItem]; continue; }
                auto *item = [[NSMenuItem alloc] initWithTitle:action->text().toNSString()
                    action:@selector(sessionTabMenuAction:) keyEquivalent:@""];
                item.target = self;
                item.tag = index * 100 + actionIndex;
                [menu addItem:item];
            }
            button.menu = menu;
        }
        [self.tabsDocument addSubview:tab];
    }
    auto *add = [NSButton buttonWithImage:symbolImage(@"plus", NSImageNameAddTemplate)
        target:self action:@selector(openServerManager:)];
    add.frame = NSMakeRect(count * 148, 2, 28, 24);
    add.bordered = NO;
    add.toolTip = @"服务器管理";
    [self.tabsDocument addSubview:add];
    self.tabsDocument.frame = NSMakeRect(0, 0, count * 148 + 30, 28);
    [self resizeSessionTabs];
    const int selected = sessionTabs->currentIndex();
    if (selected >= 0) [self.tabsDocument scrollRectToVisible:NSMakeRect(selected * 148, 0, 148, 28)];
}

- (void)addConnection:(id)sender
{
    (void)sender;
    [self.addPopover close];
    if (addConnectionHandler) addConnectionHandler();
}

- (void)openCredentialManager:(id)sender
{
    (void)sender;
    [self.addPopover close];
    if (credentialManagerHandler) credentialManagerHandler();
}

- (void)openServerManager:(id)sender
{
    (void)sender;
    [self.addPopover close];
    if (serverManagerHandler) serverManagerHandler();
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

void applyMacApplicationAppearance(int themeMode)
{
    NSAppearance *appearance = themeMode == 1
        ? [NSAppearance appearanceNamed:NSAppearanceNameAqua]
        : themeMode == 2 ? [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua] : nil;
    if (NSApp.appearance != appearance) NSApp.appearance = appearance;
}

bool installMacTitleBarControls(QMainWindow *window,
    std::function<void()> addConnection,
    std::function<void()> openCredentialManager,
    std::function<void()> openServerManager,
    std::function<void()> toggleMonitor,
    std::function<void()> openTerminalSettings,
    std::function<void(int)> selectTheme,
    int themeMode)
{
    NSWindow *native = nativeWindow(window);
    if (!native) return false;
    if (objc_getAssociatedObject(native, &kTitleBarControlsKey)) return true;

    auto *bridge = [[NoxShellTitleBarControlsBridge alloc] init];
    bridge.window = native;
    bridge->addConnectionHandler = std::move(addConnection);
    bridge->credentialManagerHandler = std::move(openCredentialManager);
    bridge->serverManagerHandler = std::move(openServerManager);
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
    auto makeHoverMenuButton = [](NSImage *image, id target, SEL action) {
        auto *button = [[NoxShellHoverMenuButton alloc] initWithFrame:NSMakeRect(0, 0, 28, 24)];
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

    bridge.addButton = makeHoverMenuButton(symbolImage(@"plus", NSImageNameAddTemplate),
        bridge, @selector(addConnection:));
    bridge.addButton.accessibilityIdentifier = @"newConnectionButton";
    bridge.addButton.toolTip = @"新建连接；停留以打开管理菜单";
    bridge.addButton.accessibilityLabel = bridge.addButton.toolTip;
    auto *menuStack = [[NSStackView alloc] initWithFrame:NSMakeRect(0, 0, 156, 76)];
    menuStack.orientation = NSUserInterfaceLayoutOrientationVertical;
    menuStack.alignment = NSLayoutAttributeLeading;
    menuStack.spacing = 4;
    menuStack.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    const auto addMenuButton = [bridge, menuStack](NSString *title, SEL action) {
        auto *button = [NSButton buttonWithTitle:title target:bridge action:action];
        button.bordered = NO;
        button.accessibilityIdentifier = action == @selector(openCredentialManager:)
            ? @"credentialManagerAction" : @"serverManagerAction";
        button.alignment = NSTextAlignmentLeft;
        [button.widthAnchor constraintEqualToConstant:140].active = YES;
        [button.heightAnchor constraintEqualToConstant:28].active = YES;
        [menuStack addArrangedSubview:button];
    };
    addMenuButton(@"凭据管理", @selector(openCredentialManager:));
    addMenuButton(@"服务器管理", @selector(openServerManager:));
    auto *menuController = [[NSViewController alloc] init];
    menuController.view = menuStack;
    bridge.addPopover = [[NSPopover alloc] init];
    bridge.addPopover.contentViewController = menuController;
    bridge.addPopover.contentSize = NSMakeSize(156, 76);
    bridge.addPopover.behavior = NSPopoverBehaviorTransient;
    bridge.addPopover.animates = NO;
    bridge.addButton.hoverPopover = bridge.addPopover;
    bridge.monitorButton = makeButton(symbolImage(@"rectangle.split.3x1", NSImageNameColumnViewTemplate),
        bridge, @selector(toggleMonitor:));
    bridge.monitorButton.accessibilityIdentifier = @"monitorToggleButton";
    bridge.settingsButton = makeButton(symbolImage(@"gearshape", NSImageNameActionTemplate),
        bridge, @selector(openSettings:));
    bridge.settingsButton.accessibilityIdentifier = @"settingsButton";
    bridge.settingsButton.toolTip = @"设置";
    bridge.settingsButton.accessibilityLabel = bridge.settingsButton.toolTip;
    bridge.themeButton = makeButton(symbolImage(@"circle.lefthalf.filled", NSImageNameColorPanel),
        bridge, @selector(showThemeMenu:));
    bridge.themeButton.accessibilityIdentifier = @"themeModeButton";
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

    auto *stack = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 60, 28)];
    bridge.addButton.translatesAutoresizingMaskIntoConstraints = YES;
    bridge.addButton.frame = NSMakeRect(0, 2, 28, 24);
    bridge.monitorButton.translatesAutoresizingMaskIntoConstraints = YES;
    bridge.monitorButton.frame = NSMakeRect(30, 2, 28, 24);
    [stack addSubview:bridge.addButton];
    [stack addSubview:bridge.monitorButton];
    bridge.tabsScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(60, 0, 0, 28)];
    bridge.tabsScroll.drawsBackground = NO;
    bridge.tabsScroll.hasHorizontalScroller = YES;
    bridge.tabsScroll.autohidesScrollers = YES;
    bridge.tabsScroll.scrollerStyle = NSScrollerStyleOverlay;
    bridge.tabsScroll.hidden = YES;
    bridge.tabsDocument = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 0, 28)];
    bridge.tabsScroll.documentView = bridge.tabsDocument;
    [stack addSubview:bridge.tabsScroll];
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

    __weak NoxShellTitleBarControlsBridge *weakBridge = bridge;
    bridge.resizeObserver = [NSNotificationCenter.defaultCenter addObserverForName:NSWindowDidResizeNotification
        object:native queue:nil usingBlock:^(NSNotification *) {
            NoxShellTitleBarControlsBridge *controls = weakBridge;
            if (controls && controls->sessionTabs && controls->sessionTabs->count() > 0) [controls resizeSessionTabs];
        }];
    updateMacTitleBarControls(window, false, themeMode);
    return true;
}

void updateMacTitleBarSessionTabs(QMainWindow *window, QTabBar *tabs)
{
    NSWindow *native = nativeWindow(window);
    auto *bridge = native ? (NoxShellTitleBarControlsBridge *)objc_getAssociatedObject(native, &kTitleBarControlsKey) : nil;
    if (!bridge) return;
    bridge->sessionTabs = tabs;
    [bridge rebuildSessionTabs];
}

void updateMacTitleBarControls(QMainWindow *window, bool monitorVisible, int themeMode)
{
    NSWindow *native = nativeWindow(window);
    auto *bridge = native ? (NoxShellTitleBarControlsBridge *)objc_getAssociatedObject(native, &kTitleBarControlsKey) : nil;
    if (!bridge) return;

    bridge.monitorButton.toolTip = monitorVisible ? @"隐藏实时监控栏" : @"显示实时监控栏";
    bridge.monitorButton.accessibilityLabel = bridge.monitorButton.toolTip;
    bridge.monitorButton.contentTintColor = monitorVisible ? NSColor.labelColor : NSColor.secondaryLabelColor;
    for (NSMenuItem *item in bridge.themeMenu.itemArray) {
        item.state = item.tag == themeMode ? NSControlStateValueOn : NSControlStateValueOff;
    }
    const NSArray<NSString *> *themeNames = @[@"跟随系统", @"亮色模式", @"暗色模式"];
    if (themeMode >= 0 && themeMode < static_cast<int>(themeNames.count)) {
        bridge.themeButton.toolTip = [@"界面外观：" stringByAppendingString:themeNames[themeMode]];
        bridge.themeButton.accessibilityLabel = bridge.themeButton.toolTip;
    }
    [bridge rebuildSessionTabs];
    native.appearance = themeMode == 1
        ? [NSAppearance appearanceNamed:NSAppearanceNameAqua]
        : themeMode == 2 ? [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua] : nil;
}

} // namespace noxshell::ui
