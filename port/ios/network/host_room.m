/* The game executes natively. WebKit hosts only the bundled PR #12 WebRTC
   transport, with copied bounded packet queues and no downloaded game code. */
#import <UIKit/UIKit.h>
#import <WebKit/WebKit.h>
#import <QuartzCore/QuartzCore.h>
#include <TargetConditionals.h>
#include "room_bridge.h"
#include "../host/ios_host.h"

static WKWebView *transport;
static UIWindow *room_window;
static UILabel *status_label;
static NSMutableArray *reports;
static BOOL in_flight, selected, manual;
static uint32_t input_ack;
static NSUInteger session_generation;
static CFTimeInterval last_tick;

@interface HaloRoomController : UIViewController<UITextFieldDelegate>
@property(nonatomic,strong) UITextField *code;
@property(nonatomic,strong) UILabel *status;
@property(nonatomic,strong) UIScrollView *scroll;
@property(nonatomic,strong) UIButton *join;
@property(nonatomic) BOOL chosen, cancelled;
@end
@implementation HaloRoomController
- (void)viewDidLoad {
    [super viewDidLoad];self.view.backgroundColor=UIColor.systemBackgroundColor;
    UILabel *title=[UILabel new];title.text=@"Online multiplayer";title.font=[UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    UILabel *hint=[UILabel new];hint.text=@"Enter the same room code as your friends.";hint.numberOfLines=0;
    self.code=[UITextField new];self.code.borderStyle=UITextBorderStyleRoundedRect;self.code.placeholder=@"Room code";
    self.code.text=[NSUserDefaults.standardUserDefaults stringForKey:@"haloRoom"] ?: @"FQLX01";
    self.code.autocapitalizationType=UITextAutocapitalizationTypeAllCharacters;
    self.code.autocorrectionType=UITextAutocorrectionTypeNo;self.code.returnKeyType=UIReturnKeyJoin;self.code.delegate=self;
    self.code.accessibilityIdentifier=@"halo.room.code";
    self.status=[UILabel new];self.status.numberOfLines=0;self.status.font=[UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    UIStackView *fields=[[UIStackView alloc]initWithArrangedSubviews:@[title,hint,self.code,self.status]];
    fields.axis=UILayoutConstraintAxisVertical;fields.spacing=8;fields.translatesAutoresizingMaskIntoConstraints=NO;
    self.scroll=[UIScrollView new];self.scroll.translatesAutoresizingMaskIntoConstraints=NO;self.scroll.keyboardDismissMode=UIScrollViewKeyboardDismissModeInteractive;
    [self.view addSubview:self.scroll];[self.scroll addSubview:fields];
    UIButton *cancel=[UIButton buttonWithType:UIButtonTypeSystem];[cancel setTitle:@"Cancel" forState:UIControlStateNormal];
    [cancel addTarget:self action:@selector(cancelRoom) forControlEvents:UIControlEventTouchUpInside];
    self.join=[UIButton buttonWithType:UIButtonTypeSystem];[self.join setTitle:@"Join room" forState:UIControlStateNormal];
    [self.join addTarget:self action:@selector(joinRoom) forControlEvents:UIControlEventTouchUpInside];
    UIStackView *actions=[[UIStackView alloc]initWithArrangedSubviews:@[cancel,self.join]];actions.distribution=UIStackViewDistributionFillEqually;actions.spacing=16;actions.translatesAutoresizingMaskIntoConstraints=NO;
    [self.view addSubview:actions];UILayoutGuide *safe=self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [actions.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:16],
        [actions.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-16],
        [actions.bottomAnchor constraintEqualToAnchor:self.view.keyboardLayoutGuide.topAnchor constant:-8],
        [actions.heightAnchor constraintEqualToConstant:44],
        [self.scroll.topAnchor constraintEqualToAnchor:safe.topAnchor constant:8],
        [self.scroll.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:16],
        [self.scroll.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-16],
        [self.scroll.bottomAnchor constraintEqualToAnchor:actions.topAnchor constant:-8],
        [fields.topAnchor constraintEqualToAnchor:self.scroll.contentLayoutGuide.topAnchor],
        [fields.bottomAnchor constraintEqualToAnchor:self.scroll.contentLayoutGuide.bottomAnchor],
        [fields.leadingAnchor constraintEqualToAnchor:self.scroll.contentLayoutGuide.leadingAnchor],
        [fields.trailingAnchor constraintEqualToAnchor:self.scroll.contentLayoutGuide.trailingAnchor],
        [fields.widthAnchor constraintEqualToAnchor:self.scroll.frameLayoutGuide.widthAnchor],
        [self.code.heightAnchor constraintEqualToConstant:44]]];
}
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    if(self.code.isFirstResponder)[self.scroll scrollRectToVisible:[self.code convertRect:self.code.bounds toView:self.scroll] animated:NO];
}
- (void)joinRoom {
    NSString *text=[self.code.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if(!text.length){self.status.text=@"Enter a room code.";return;}
    self.code.text=text;self.chosen=YES;self.join.enabled=NO;self.code.enabled=NO;
    [self.view endEditing:YES];self.status.text=@"Connecting…";
}
- (void)cancelRoom {self.cancelled=YES;self.chosen=YES;[self.view endEditing:YES];}
- (BOOL)textFieldShouldReturn:(UITextField *)field {(void)field;[self joinRoom];return NO;}
@end

@interface HaloRoomNavigation : NSObject<WKNavigationDelegate>
@end
@implementation HaloRoomNavigation
- (void)webView:(WKWebView *)view decidePolicyForNavigationAction:(WKNavigationAction *)action decisionHandler:(void (^)(WKNavigationActionPolicy))handler {
    (void)view;
    NSString *host = action.request.URL.host;
    handler(([host isEqualToString:@"halo-native.invalid"] || [action.request.URL.absoluteString isEqualToString:@"about:blank"])
        ? WKNavigationActionPolicyAllow : WKNavigationActionPolicyCancel);
}
- (void)webViewWebContentProcessDidTerminate:(WKWebView *)view {
    if(view!=transport)return;
    host_logf(HOST_LOG_ERROR, "Room transport stopped; restart the app to reconnect.");
    const uint32_t command[4] = {2, 1, 0, 0}; ios_room_queue_command(command);
    status_label.text = @"Network process stopped. Restart the app to reconnect.";
    transport = nil;
}
@end
static HaloRoomNavigation *navigation;

static void network_error(NSString *message) {
    status_label.text = message;
    host_logf(HOST_LOG_ERROR, "Browser room: %s", message.UTF8String);
    if (selected) { const uint32_t c[4] = {2,1,0,0}; ios_room_queue_command(c); }
}

void host_ios_room_report(const char *json) {
    NSString *copy = [NSString stringWithUTF8String:json];
    void (^append)(void) = ^{
        if (!transport) return;
        NSDictionary *value = [NSJSONSerialization JSONObjectWithData:[copy dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
        if (!value) return;
        if (reports.count >= 128) {
            network_error(@"Room status queue overflow; restart to reconnect."); transport = nil; return;
        }
        [reports addObject:value];
    };
    if (NSThread.isMainThread) append(); else dispatch_async(dispatch_get_main_queue(), append);
}

void host_ios_room_tick(void) {
    if (!transport || in_flight || !NSThread.isMainThread || CACurrentMediaTime() - last_tick < .008) return;
    last_tick = CACurrentMediaTime();
    NSMutableData *out = [NSMutableData dataWithLength:256 * 1024];
    uint32_t end;
    size_t length = ios_room_read_output(out.mutableBytes, out.length, &end);
    out.length = length;
    NSUInteger report_count = reports.count;
    NSDictionary *request = @{ @"out": [out base64EncodedStringWithOptions:0], @"ack": @(input_ack), @"reports": [reports copy] };
    NSData *json = [NSJSONSerialization dataWithJSONObject:request options:0 error:nil];
    NSString *script = [NSString stringWithFormat:@"HaloNative.exchange(%@)", [[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding]];
    in_flight = YES;
    NSUInteger generation=session_generation;
    [transport evaluateJavaScript:script completionHandler:^(id result, NSError *error) {
        if(generation!=session_generation)return;
        in_flight = NO;
        if (error || ![result isKindOfClass:NSDictionary.class]) {
            // During initial document loading HaloNative does not exist yet.
            if (!selected && error) return;
            network_error(error.localizedDescription ?: @"Invalid transport response"); transport = nil; return;
        }
        NSDictionary *value = result;
        NSUInteger sent = [value[@"sent"] unsignedIntegerValue];
        if (sent > length || !ios_room_ack_output(end - (uint32_t)length + (uint32_t)sent)) {
            network_error(@"Invalid network acknowledgement"); transport = nil; return;
        }
        if (report_count) [reports removeObjectsInRange:NSMakeRange(0, report_count)];
        NSDictionary *incoming = value[@"input"];
        NSString *encoded = incoming[@"data"];
        if (encoded.length) {
            NSData *data = [[NSData alloc] initWithBase64EncodedString:encoded options:0];
            if (data && data.length <= 256 * 1024 && ios_room_write_input(data.bytes, data.length))
                input_ack = [incoming[@"id"] unsignedIntValue];
        }
        for (NSArray *raw in value[@"commands"]) {
            if (raw.count != 4) continue;
            uint32_t c[4]; for (int i=0;i<4;i++) c[i]=[raw[i] unsignedIntValue];
            if (!ios_room_queue_command(c)) { network_error(@"Room command queue full"); transport=nil; return; }
        }
        __atomic_store_n(&web_shared_state()->net_local_address, [value[@"address"] unsignedIntValue], __ATOMIC_RELEASE);
        if ([value[@"error"] isKindOfClass:NSString.class]) network_error(value[@"error"]);
        else if ([value[@"status"] isKindOfClass:NSString.class]) status_label.text=value[@"status"];
        NSDictionary *pings = value[@"pings"];
        NSArray *rows = pings[@"rows"];
        int32_t table[WEB_PING_PEERS][2];
        NSUInteger count = MIN(rows.count, WEB_PING_PEERS);
        for (NSUInteger i = 0; i < count; i++) {
            table[i][0] = [rows[i][0] intValue]; table[i][1] = [rows[i][1] intValue];
        }
        ios_room_update_pings([pings[@"host"] unsignedIntValue], [pings[@"epoch"] unsignedIntValue],
            [pings[@"updated"] unsignedIntValue], table, count);
        NSDictionary *selection=value[@"selected"];
        if (!selected && !manual && [selection isKindOfClass:NSDictionary.class]) {
            NSString *role=selection[@"role"];
            if (![role isEqualToString:@"host"] && ![role isEqualToString:@"join"]) return;
            uint32_t ip=[selection[@"hostAddress"] unsignedIntValue];
            const uint32_t start[4]={6,[role isEqualToString:@"host"]?1U:2U,ip,[selection[@"epoch"] unsignedIntValue]};
            if(!ios_room_queue_command(start)){network_error(@"Room command queue full");return;}
            selected=YES;
        }
    }];
}

void host_ios_room_prepare(void) {
#if TARGET_OS_SIMULATOR
    if([NSProcessInfo.processInfo.environment[@"HALO_IOS_TEST_ROOM"] length] ||
       [NSProcessInfo.processInfo.environment[@"HALO_IOS_TEST_ROOM_UI"] isEqualToString:@"1"])host_ios_room_open();
#endif
}

void host_ios_room_open(void) {
    UIWindow *previous=nil;
    UIWindowScene *scene=nil;
    for (UIScene *candidate in UIApplication.sharedApplication.connectedScenes)
        if ([candidate isKindOfClass:UIWindowScene.class]) {scene=(UIWindowScene*)candidate;break;}
    if(!scene){host_logf(HOST_LOG_ERROR,"No window scene for room chooser");return;}
    for(UIWindow *window in scene.windows)if(window.isKeyWindow)previous=window;
    ++session_generation;
    [transport evaluateJavaScript:@"HaloNative.leave()" completionHandler:nil];
    transport=nil;room_window.hidden=YES;in_flight=selected=manual=NO;input_ack=0;last_tick=0;
    ios_room_reset();reports=[NSMutableArray new];host_ios_touch_reset();
    room_window=[[UIWindow alloc]initWithWindowScene:scene];
    HaloRoomController *controller=[HaloRoomController new];room_window.rootViewController=controller;
    room_window.windowLevel=UIWindowLevelNormal+1;[room_window makeKeyAndVisible];
    [controller loadViewIfNeeded];status_label=controller.status;
#if TARGET_OS_SIMULATOR
    NSString *test_room=NSProcessInfo.processInfo.environment[@"HALO_IOS_TEST_ROOM"];
    if(test_room.length){controller.code.text=test_room;[controller joinRoom];}
    else if([NSProcessInfo.processInfo.environment[@"HALO_IOS_TEST_ROOM_UI"] isEqualToString:@"1"])[controller.code becomeFirstResponder];
#endif
    while(!controller.chosen)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
    if(controller.cancelled){room_window.hidden=YES;[previous makeKeyAndVisible];return;}
    NSString *code=controller.code.text;
    [NSUserDefaults.standardUserDefaults setObject:code forKey:@"haloRoom"];
    WKWebViewConfiguration *config=[WKWebViewConfiguration new];
    // Isolated persistent origin retains the virtual room address across launches.
    config.websiteDataStore=WKWebsiteDataStore.defaultDataStore;
    transport=[[WKWebView alloc] initWithFrame:CGRectMake(0,0,1,1) configuration:config];
    navigation=[HaloRoomNavigation new];transport.navigationDelegate=navigation;
    transport.userInteractionEnabled=NO;
    [controller.view addSubview:transport];
    NSMutableString *html=[NSMutableString stringWithString:@"<!doctype html><meta name='viewport' content='width=device-width'><script>"];
    for (NSString *name in @[@"net",@"room"]) {
        NSString *path=[NSBundle.mainBundle pathForResource:name ofType:@"js" inDirectory:@"Network"];
        NSString *script=path ? [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil] : nil;
        if (!script) host_fatal("Missing bundled room transport");
        [html appendString:script];[html appendString:@"\n"];
    }
    [html appendString:@"</script>"];
    [transport loadHTMLString:html baseURL:[NSURL URLWithString:@"https://halo-native.invalid/"]];
    // Optional TURN credentials stay in the user's Documents/browser-room.json.
    NSMutableDictionary *options=[NSMutableDictionary dictionaryWithDictionary:@{@"room":code ?: @""}];
    NSData *settings=[NSData dataWithContentsOfFile:@"browser-room.json"];
    if(settings){id value=[NSJSONSerialization JSONObjectWithData:settings options:0 error:nil];if([value isKindOfClass:NSDictionary.class] && [value[@"turn"] isKindOfClass:NSDictionary.class])options[@"turn"]=value[@"turn"];}
    NSString *json=[[NSString alloc]initWithData:[NSJSONSerialization dataWithJSONObject:options options:0 error:nil] encoding:NSUTF8StringEncoding];
    NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:90];
    __block BOOL joining=NO;
    while (!selected && !controller.cancelled && deadline.timeIntervalSinceNow>0) {
        if(!transport.loading && !joining){
            joining=YES;
            [transport evaluateJavaScript:[NSString stringWithFormat:@"HaloNative.join(%@); true",json] completionHandler:^(id value,NSError*error){(void)value;if(error)joining=NO;}];
        }
        host_ios_room_tick();
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
    }
    if (!selected || controller.cancelled) {
        ++session_generation;
        ios_room_reset();selected=NO;in_flight=NO;
        [transport evaluateJavaScript:@"HaloNative.leave()" completionHandler:nil];
        transport=nil;manual=YES;
        host_logf(HOST_LOG_ERROR,"Room connection cancelled or timed out; returning to menus.");
        room_window.hidden=YES;
    }
    // Retain the WebKit view in the foreground scene beneath the SDL window.
    room_window.windowLevel=UIWindowLevelNormal-1;
    status_label.hidden=YES;
    [previous makeKeyAndVisible];
    host_ios_touch_reset();
}
