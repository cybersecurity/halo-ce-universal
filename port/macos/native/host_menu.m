#import <Cocoa/Cocoa.h>
#import "HaloPreferences.h"
#include "host_menu.h"
#include <stdlib.h>

@interface HaloMenu : NSObject <NSApplicationDelegate, NSMenuDelegate>
@property(nonatomic, strong) HaloPreferences *preferences;
@property(nonatomic, strong) id previousDelegate;
@property(nonatomic) BOOL gameRunning;
@property(nonatomic) BOOL importing;
- (BOOL)chooseData:(BOOL)disc;
@end

static HaloMenu *menu;

static void showError(NSError *error) {
    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = @"Halo could not use that setting";
    alert.informativeText = error.localizedDescription ?: @"Please try again.";
    if (menu.gameRunning && NSApp.keyWindow)
        [alert beginSheetModalForWindow:NSApp.keyWindow completionHandler:nil];
    else [alert runModal];
}

static void addItem(NSMenu *parent, NSString *title, SEL action, NSString *key, id target) {
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
    item.target = target;
    [parent addItem:item];
}

@implementation HaloMenu
- (BOOL)respondsToSelector:(SEL)selector {
    return [super respondsToSelector:selector] || [self.previousDelegate respondsToSelector:selector];
}
- (id)forwardingTargetForSelector:(SEL)selector {
    return [self.previousDelegate respondsToSelector:selector] ? self.previousDelegate : [super forwardingTargetForSelector:selector];
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {
    (void)sender;
    if (self.importing) return NSTerminateCancel;
    if (self.gameRunning) { host_sdl_request_quit(); return NSTerminateCancel; }
    return NSTerminateNow;
}
- (void)menuWillOpen:(NSMenu *)sender { (void)sender; host_sdl_release_mouse(); }
- (void)menuNeedsUpdate:(NSMenu *)sender {
    for (NSMenuItem *item in sender.itemArray) {
        if (item.action == @selector(changeData:))
            item.state = self.preferences.chooseDataOnLaunch ? NSControlStateValueOn : NSControlStateValueOff;
        item.enabled = !self.importing;
    }
}
- (void)about:(id)sender { (void)sender; [NSApp orderFrontStandardAboutPanel:self]; }
- (void)openSaves:(id)sender {
    (void)sender;
    [NSWorkspace.sharedWorkspace openURL:self.preferences.supportDirectory];
}
- (void)changeData:(id)sender {
    (void)sender;
    NSError *error = nil;
    if (![self.preferences setChooseDataOnLaunch:!self.preferences.chooseDataOnLaunch error:&error]) showError(error);
}
- (void)buildMenus {
    NSMenu *main = [[NSMenu alloc] initWithTitle:@"Main"];
    NSMenuItem *app = [[NSMenuItem alloc] initWithTitle:@"Halo" action:nil keyEquivalent:@""];
    NSMenu *actions = [[NSMenu alloc] initWithTitle:@"Halo"];
    actions.autoenablesItems = NO;
    actions.delegate = self;
    addItem(actions, @"About Halo CE Universal", @selector(about:), @"", self);
    [actions addItem:NSMenuItem.separatorItem];
    addItem(actions, @"Choose Game Data on Next Launch", @selector(changeData:), @"", self);
    addItem(actions, @"Open Saves Folder", @selector(openSaves:), @"", self);
    [actions addItem:NSMenuItem.separatorItem];
    addItem(actions, @"Quit Halo", @selector(terminate:), @"q", NSApp);
    app.submenu = actions;
    [main addItem:app];
    NSApp.mainMenu = main;
}
- (BOOL)chooseData:(BOOL)disc {
    NSOpenPanel *panel = NSOpenPanel.openPanel;
    panel.title = disc ? @"Choose Your Xbox Halo Disc Image" : @"Choose Your Xbox Halo Maps";
    panel.message = disc ? @"Import the maps from your own original Xbox Halo disc image."
                         : @"Choose an extracted game folder or its maps folder.";
    panel.canChooseDirectories = !disc;
    panel.canChooseFiles = disc;
    panel.allowsMultipleSelection = NO;
    if ([panel runModal] != NSModalResponseOK) return NO;
    __block NSError *error = nil;
    __block NSURL *root = panel.URL;
    if (disc) {
        NSWindow *progress = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 400, 100)
            styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
        progress.title = @"Importing Halo Maps";
        NSTextField *label = [NSTextField labelWithString:@"Importing maps; your existing data stays in place."];
        label.frame = NSMakeRect(20, 58, 360, 20);
        [progress.contentView addSubview:label];
        NSProgressIndicator *bar = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(20, 26, 360, 16)];
        bar.indeterminate = YES;
        [progress.contentView addSubview:bar];
        [bar startAnimation:self];
        [progress center];
        [progress makeKeyAndOrderFront:self];
        self.importing = YES;
        __block BOOL finished = NO;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            NSError *importError = nil;
            NSURL *imported = HaloImportDiscImage(panel.URL, self.preferences.supportDirectory, NULL, NULL, &importError);
            dispatch_async(dispatch_get_main_queue(), ^{
                root = imported; error = importError; finished = YES;
            });
        });
        /* Import only before the game starts; keep Cocoa responsive while copying. */
        while (!finished)
            [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
        self.importing = NO;
        [progress orderOut:self];
    }
    if (root && [self.preferences selectDataRoot:root iso:disc ? panel.URL : nil error:&error]) return YES;
    if (disc && root) [NSFileManager.defaultManager removeItemAtURL:root error:nil];
    showError(error);
    return NO;
}
@end

void host_menu_initialize_application(void) {
    /* Let Cocoa route Quit through the delegate so the guest can save first. */
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
}

int host_menu_prepare(const char *support, const char *fallback, char *data, size_t capacity) {
    @autoreleasepool {
        menu = [[HaloMenu alloc] init];
        menu.preferences = [[HaloPreferences alloc] initWithSupportDirectory:[NSURL fileURLWithPath:@(support) isDirectory:YES]];
        menu.previousDelegate = NSApp.delegate;
        NSApp.delegate = menu;
        [menu buildMenus];
        [NSApp finishLaunching];
        NSString *selected = menu.preferences.dataPath;
        const char *override = getenv("HALO_DATA_ROOT");
        if (override && *override) selected = @(override);
        NSURL *valid = selected ? HaloValidateGameData([NSURL fileURLWithPath:selected], nil) : nil;
        if (!selected && fallback && *fallback) valid = HaloValidateGameData([NSURL fileURLWithPath:@(fallback)], nil);
        if (menu.preferences.chooseDataOnLaunch && !(override && *override)) valid = nil;
        while (!valid) {
            NSAlert *alert = [[NSAlert alloc] init];
            alert.messageText = @"Choose your Halo game data";
            alert.informativeText = @"Use your own original Xbox Halo disc image or extracted maps folder. The app does not include game data.";
            [alert addButtonWithTitle:@"Choose Disc Image…"];
            [alert addButtonWithTitle:@"Choose Maps Folder…"];
            [alert addButtonWithTitle:@"Quit"];
            NSModalResponse answer = [alert runModal];
            if (answer == NSAlertThirdButtonReturn) return 0;
            if ([menu chooseData:answer == NSAlertFirstButtonReturn])
                valid = [NSURL fileURLWithPath:menu.preferences.dataPath];
        }
        return [valid.path getCString:data maxLength:capacity encoding:NSUTF8StringEncoding] ? 1 : 0;
    }
}
void host_menu_begin_game(void) { menu.gameRunning = YES; }
void host_menu_finish_game(int exit_code) { (void)exit_code; menu.gameRunning = NO; }
