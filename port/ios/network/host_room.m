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
static char mode[40], target[64];
static CFTimeInterval last_tick;

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
    (void)view;
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
    [transport evaluateJavaScript:script completionHandler:^(id result, NSError *error) {
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
            snprintf(mode,sizeof(mode),"HALO_QUICK_PLAY=%s",role.UTF8String);
            snprintf(target,sizeof(target),"HALO_QUICK_PLAY_TARGET=%u.%u.%u.%u",ip&255,(ip>>8)&255,(ip>>16)&255,ip>>24);
            selected=YES;
        }
    }];
}

const char *host_ios_room_mode(void) { return mode; }
const char *host_ios_room_target(void) { return target; }

void host_ios_room_prepare(void) {
    reports=[NSMutableArray new];
    UIWindowScene *scene=nil;
    for (UIScene *candidate in UIApplication.sharedApplication.connectedScenes)
        if ([candidate isKindOfClass:UIWindowScene.class]) {scene=(UIWindowScene*)candidate;break;}
    if (!scene) { host_logf(HOST_LOG_ERROR,"No window scene for room chooser"); return; }
    room_window=[[UIWindow alloc] initWithWindowScene:scene];
    UIViewController *controller=[UIViewController new];
    controller.view.backgroundColor=UIColor.systemBackgroundColor;
    room_window.rootViewController=controller;
    [room_window makeKeyAndVisible];
    status_label=[[UILabel alloc] initWithFrame:CGRectMake(24,24,scene.coordinateSpace.bounds.size.width-48,100)];
    status_label.numberOfLines=0; status_label.text=@"Choose a browser room, or open the game menus.";
    [controller.view addSubview:status_label];
    __block BOOL chosen=NO;
    __block NSString *code=nil;
    UIAlertController *chooser=[UIAlertController alertControllerWithTitle:@"Halo online" message:@"Enter the same room code as the web players. Keep the app open during play." preferredStyle:UIAlertControllerStyleAlert];
    [chooser addTextFieldWithConfigurationHandler:^(UITextField *field){field.placeholder=@"Room code";field.text=[NSUserDefaults.standardUserDefaults stringForKey:@"haloRoom"] ?: @"FQLX01";field.autocapitalizationType=UITextAutocapitalizationTypeAllCharacters;}];
    [chooser addAction:[UIAlertAction actionWithTitle:@"Game menus" style:UIAlertActionStyleCancel handler:^(UIAlertAction*a){(void)a;manual=YES;chosen=YES;}]];
    [chooser addAction:[UIAlertAction actionWithTitle:@"Join room" style:UIAlertActionStyleDefault handler:^(UIAlertAction*a){(void)a;code=chooser.textFields.firstObject.text;chosen=YES;}]];
#if TARGET_OS_SIMULATOR
    // Explicit simulator launch settings make real-engine smoke tests repeatable.
    NSDictionary *environment=NSProcessInfo.processInfo.environment;
    NSString *test_room=environment[@"HALO_IOS_TEST_ROOM"];
    if (test_room.length) {code=test_room;chosen=YES;}
    else if ([environment[@"HALO_IOS_TEST_MENUS"] isEqualToString:@"1"]) {manual=YES;chosen=YES;}
#endif
    if (!chosen) [controller presentViewController:chooser animated:NO completion:nil];
    while (!chosen) [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
    if (manual) {room_window.hidden=YES;return;}
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
    while (!selected && deadline.timeIntervalSinceNow>0) {
        if(!transport.loading && !joining){
            joining=YES;
            [transport evaluateJavaScript:[NSString stringWithFormat:@"HaloNative.join(%@); true",json] completionHandler:^(id value,NSError*error){(void)value;if(error)joining=NO;}];
        }
        host_ios_room_tick();
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
    }
    if (!selected) {
        [transport evaluateJavaScript:@"HaloNative.leave()" completionHandler:nil];
        transport=nil;manual=YES;
        host_logf(HOST_LOG_ERROR,"Room connection timed out; opening game menus.");
    }
    // Retain the WebKit view in the foreground scene beneath the SDL window.
    room_window.windowLevel=UIWindowLevelNormal-1;
    status_label.hidden=YES;
}
