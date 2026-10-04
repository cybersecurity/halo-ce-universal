/* Native presentation of upstream c04765d7's signed public server listings.
   Discovery, invite validation and NAT traversal remain in the shared P2P code. */
#if HALO_MACOS
#import <AppKit/AppKit.h>
@interface NSTextField (HaloText)
@property(nonatomic,copy) NSString *text;
@end
#else
#import <UIKit/UIKit.h>
#endif
#import <QuartzCore/QuartzCore.h>
#include "p2p.h"
#include "network_mode.h"
#include "../host/ios_host.h"

static NSMutableArray<NSData *> *rows;
static struct p2p_listing selection;
static int action;
static BOOL connecting;
static double refreshed;
static NSString *lastSummary;
#if HALO_MACOS
static NSWindow *browserWindow, *previousWindow;
@interface HaloServerBrowser : NSObject<NSTableViewDataSource,NSTableViewDelegate>
@property NSTableView *table;
@property NSTextField *status;
@end
#else
static UIWindow *browserWindow, *previousWindow;
@interface HaloServerBrowser : UITableViewController
@property UILabel *status;
@end
#endif
static HaloServerBrowser *browser;
static NSString *field(const char *value,size_t size) {
    return [[NSString alloc]initWithBytes:value length:strnlen(value,size) encoding:NSASCIIStringEncoding]?:@"";
}
static NSString *serverTitle(const struct p2p_listing *game) {
    return [NSString stringWithFormat:@"%@   %u/%u",field(game->name,sizeof(game->name)),game->player_count,game->maximum_player_count];
}
static NSString *serverDetail(const struct p2p_listing *game) {
    NSString *ping=game->ping<0?@"Ping unknown":[NSString stringWithFormat:@"%d ms",game->ping];
    return [NSString stringWithFormat:@"%@ · %@ · %@%@",field(game->map,sizeof(game->map)),field(game->gametype,sizeof(game->gametype)),ping,(!game->open||game->player_count>=game->maximum_player_count)?@" · Unavailable":@""];
}
static void choose(NSInteger index) {
    if(connecting || index<0 || index>=(NSInteger)rows.count)return;
    [rows[index] getBytes:&selection length:sizeof(selection)];action=1;connecting=YES;browser.status.text=@"Connecting to server… Cancel to return.";
}
@implementation HaloServerBrowser
- (void)refresh {action=2;self.status.text=@"Refreshing public servers…";}
- (void)cancel {action=-1;}
#if HALO_MACOS
- (NSInteger)numberOfRowsInTableView:(NSTableView *)table {(void)table;return rows.count;}
- (id)tableView:(NSTableView *)table objectValueForTableColumn:(NSTableColumn *)column row:(NSInteger)row {
    (void)table;const struct p2p_listing *game=rows[row].bytes;
    return [column.identifier isEqualToString:@"server"]?serverTitle(game):serverDetail(game);
}
- (void)join {choose(self.table.selectedRow);}
#else
- (void)viewDidLoad {
    [super viewDidLoad];self.title=@"Public servers";
    self.navigationItem.leftBarButtonItem=[[UIBarButtonItem alloc]initWithBarButtonSystemItem:UIBarButtonSystemItemCancel target:self action:@selector(cancel)];
    self.navigationItem.rightBarButtonItem=[[UIBarButtonItem alloc]initWithBarButtonSystemItem:UIBarButtonSystemItemRefresh target:self action:@selector(refresh)];
    self.status=[[UILabel alloc]initWithFrame:CGRectMake(0,0,640,64)];self.status.numberOfLines=0;self.status.textAlignment=NSTextAlignmentCenter;self.status.font=[UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    self.status.text=@"Looking for compatible public servers…\nTap a server to join.";self.tableView.tableHeaderView=self.status;
}
- (NSInteger)tableView:(UITableView *)table numberOfRowsInSection:(NSInteger)section {(void)table;(void)section;return rows.count;}
- (UITableViewCell *)tableView:(UITableView *)table cellForRowAtIndexPath:(NSIndexPath *)path {
    (void)table;UITableViewCell *cell=[[UITableViewCell alloc]initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    const struct p2p_listing *game=rows[path.row].bytes;
    cell.textLabel.text=serverTitle(game);cell.detailTextLabel.text=serverDetail(game);cell.detailTextLabel.numberOfLines=0;
    cell.accessoryType=UITableViewCellAccessoryDisclosureIndicator;return cell;
}
- (void)tableView:(UITableView *)table didSelectRowAtIndexPath:(NSIndexPath *)path {[table deselectRowAtIndexPath:path animated:YES];choose(path.row);}
#endif
@end

int host_ios_browser_mode(void) {return halo_browser_rooms;}
void host_ios_browser_open(void) {
    action=0;connecting=NO;refreshed=0;lastSummary=nil;rows=[NSMutableArray new];host_ios_touch_reset();
    browser=[HaloServerBrowser new];
#if HALO_MACOS
    previousWindow=NSApp.keyWindow;
    browserWindow=[[NSWindow alloc]initWithContentRect:NSMakeRect(0,0,840,440) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];browserWindow.title=@"Public servers";
    browser.status=[NSTextField wrappingLabelWithString:@"Looking for compatible public servers…"];browser.status.frame=NSMakeRect(16,386,808,38);[browserWindow.contentView addSubview:browser.status];
    NSScrollView *scroll=[[NSScrollView alloc]initWithFrame:NSMakeRect(16,60,808,320)];scroll.hasVerticalScroller=YES;
    browser.table=[[NSTableView alloc]initWithFrame:scroll.bounds];
    for(NSString *identifier in @[@"server",@"details"]) {NSTableColumn *column=[[NSTableColumn alloc]initWithIdentifier:identifier];column.title=[identifier isEqualToString:@"server"]?@"Server / Players":@"Map / Game / Ping";column.width=[identifier isEqualToString:@"server"]?280:500;[browser.table addTableColumn:column];}
    browser.table.dataSource=browser;browser.table.delegate=browser;browser.table.target=browser;browser.table.doubleAction=@selector(join);scroll.documentView=browser.table;[browserWindow.contentView addSubview:scroll];
    NSArray *titles=@[@"Cancel",@"Refresh",@"Join server"];SEL selectors[]={@selector(cancel),@selector(refresh),@selector(join)};
    for(int i=0;i<3;i++){NSButton *button=[NSButton buttonWithTitle:titles[i] target:browser action:selectors[i]];button.frame=NSMakeRect(16+i*140,14,130,32);[browserWindow.contentView addSubview:button];}
    [browserWindow center];[browserWindow makeKeyAndOrderFront:nil];
#else
    UIWindowScene *scene=nil;
    for(UIScene *candidate in UIApplication.sharedApplication.connectedScenes)if([candidate isKindOfClass:UIWindowScene.class] && candidate.activationState==UISceneActivationStateForegroundActive){scene=(UIWindowScene *)candidate;break;}
    if(!scene){action=-1;return;}
    for(UIWindow *window in scene.windows)if(window.isKeyWindow)previousWindow=window;
    browserWindow=[[UIWindow alloc]initWithWindowScene:scene];browserWindow.windowLevel=UIWindowLevelNormal+1;
    browserWindow.rootViewController=[[UINavigationController alloc]initWithRootViewController:browser];[browserWindow makeKeyAndVisible];[browser loadViewIfNeeded];
#endif
}
int host_ios_browser_update(const void *listings,int count,void *chosen) {
    if(count<0 || count>256 || (count && !listings))return -1;
    if(CACurrentMediaTime()-refreshed>.5 && !action) {
        refreshed=CACurrentMediaTime();NSMutableArray *next=[NSMutableArray new];
        for(int i=0;i<count;i++)[next addObject:[NSData dataWithBytes:(const struct p2p_listing *)listings+i length:sizeof(struct p2p_listing)]];
        if(![next isEqual:rows]){rows=next;
#if HALO_MACOS
            [browser.table reloadData];
#else
            [browser.tableView reloadData];
#endif
        }
        NSString *summary=count?[NSString stringWithFormat:@"%d compatible public server%@ · Select a server to join.",count,count==1?@"":@"s"]:@"No compatible servers found yet. Refresh to try again.";
        if(!connecting)browser.status.text=summary;
        if(![summary isEqual:lastSummary]){host_logf(HOST_LOG_INFO,"Server browser: %s",summary.UTF8String);lastSummary=summary;for(NSData *data in rows){const struct p2p_listing *game=data.bytes;host_logf(HOST_LOG_INFO,"Server: %s / %s",serverTitle(game).UTF8String,serverDetail(game).UTF8String);}}
    }
#if HALO_MACOS
    NSEvent *event=[NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate dateWithTimeIntervalSinceNow:.005] inMode:NSDefaultRunLoopMode dequeue:YES];if(event)[NSApp sendEvent:event];
#endif
    [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.005]];
    // Device/desktop integration tests may choose one explicitly named server.
    static BOOL testJoined;
    NSString *testName=NSProcessInfo.processInfo.environment[@"HALO_NATIVE_TEST_JOIN"];
    if(!testJoined && testName.length && !action && !connecting) {
        for(NSUInteger i=0;i<rows.count;i++) {
            const struct p2p_listing *game=rows[i].bytes;
            if([field(game->name,sizeof(game->name)) isEqualToString:testName]) {testJoined=YES;choose(i);break;}
        }
    }
    int result=action;action=0;
    if(result==1){memcpy(chosen,&selection,sizeof(selection));host_logf(HOST_LOG_INFO,"Joining public server: %s",serverTitle(&selection).UTF8String);}
    return result;
}
void host_ios_browser_close(void) {
#if HALO_MACOS
    [browserWindow orderOut:nil];[previousWindow makeKeyAndOrderFront:nil];
#else
    browserWindow.hidden=YES;[previousWindow makeKeyAndVisible];
#endif
    browserWindow=nil;previousWindow=nil;browser=nil;rows=nil;host_ios_touch_reset();
}
void host_ios_browser_error(const char *text) {
    connecting=NO;
    NSString *message=[NSString stringWithUTF8String:text]?:@"Could not join the server.";
    host_logf(HOST_LOG_ERROR,"Multiplayer: %s",message.UTF8String);
    dispatch_async(dispatch_get_main_queue(), ^{
#if HALO_MACOS
        NSAlert *alert=[NSAlert new];alert.messageText=@"Multiplayer";alert.informativeText=message;[alert addButtonWithTitle:@"OK"];
        NSWindow *window=browserWindow?:NSApp.keyWindow;if(window && !window.attachedSheet)[alert beginSheetModalForWindow:window completionHandler:nil];
#else
        UIWindow *window=browserWindow;
        if(!window)for(UIScene *scene in UIApplication.sharedApplication.connectedScenes)if([scene isKindOfClass:UIWindowScene.class])for(UIWindow *w in ((UIWindowScene *)scene).windows)if(w.isKeyWindow)window=w;
        UIViewController *presenter=window.rootViewController;while(presenter.presentedViewController)presenter=presenter.presentedViewController;
        if([presenter isKindOfClass:UIAlertController.class])return;
        UIAlertController *alert=[UIAlertController alertControllerWithTitle:@"Multiplayer" message:message preferredStyle:UIAlertControllerStyleAlert];[alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];[presenter presentViewController:alert animated:YES completion:nil];
#endif
    });
}
