/* Shader-source diagnostics are exportable data, never game maps or saves. */
#if HALO_MACOS
#import <AppKit/AppKit.h>
#else
#import <UIKit/UIKit.h>
#endif
#import <QuartzCore/QuartzCore.h>
#include <GLES3/gl32.h>
#import <CommonCrypto/CommonDigest.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/utsname.h>
#include "host_debug.h"
#include "ios_host.h"
#include "host_gl_dispatch.h"

static atomic_bool recording;
static NSMutableDictionary *shaderKeys,*sources,*attachments;
static NSMutableArray *events;
static NSMutableSet *drawnPrograms;
static uint32_t currentProgram;
static NSString *glRenderer;
#ifndef HALO_BUILD_REVISION
#define HALO_BUILD_REVISION "unknown"
#endif
static NSUInteger sourceBytes,dropped;
#if HALO_MACOS
static BOOL debugPresented;
#else
static __weak UIViewController *debugController;
#endif
void halo_debug_initialize(void) {
    NSUserDefaults *settings=NSUserDefaults.standardUserDefaults;
    atomic_store(&recording,[settings boolForKey:@"HaloRecordShaderStalls"] || [NSProcessInfo.processInfo.environment[@"HALO_IOS_TEST_SHADER_CAPTURE"] boolValue]);
    halo_metalfx_set_enabled([settings boolForKey:@"HaloMetalFX"] || [NSProcessInfo.processInfo.environment[@"HALO_IOS_TEST_METALFX"] boolValue]);
}
static void prepare(void) {
    if(!shaderKeys){shaderKeys=[NSMutableDictionary new];sources=[NSMutableDictionary new];attachments=[NSMutableDictionary new];events=[NSMutableArray new];drawnPrograms=[NSMutableSet new];}
}
void halo_debug_shader_source(uint32_t shader,int count,const char *const *strings,const int *lengths) {
    prepare();if(!glRenderer){const GLubyte *name=glGetString(GL_RENDERER);if(name)glRenderer=[NSString stringWithUTF8String:(const char *)name];}
    [shaderKeys removeObjectForKey:@(shader)];NSMutableData *data=[NSMutableData new];
    for(int i=0;i<count;i++) {
        NSUInteger length=lengths && lengths[i]>=0?(NSUInteger)lengths[i]:strlen(strings[i]);
        if(length>1024*1024 || data.length+length>1024*1024){dropped++;return;}
        [data appendBytes:strings[i] length:length];
    }
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256(data.bytes,(CC_LONG)data.length,digest);
    NSMutableString *key=[NSMutableString new];for(int i=0;i<CC_SHA256_DIGEST_LENGTH;i++)[key appendFormat:@"%02x",digest[i]];
    NSString *text=[[NSString alloc]initWithData:data encoding:NSUTF8StringEncoding];
    if(!text)return;
    if(!sources[key]) {
        if(sourceBytes+data.length>32*1024*1024){dropped++;return;}
        GLint type=0;glGetShaderiv(shader,GL_SHADER_TYPE,&type);
        sources[key]=@{@"source":text,@"type":type==GL_VERTEX_SHADER?@"vertex":@"fragment"};sourceBytes+=data.length;
    }
    shaderKeys[@(shader)]=key;
}
void halo_debug_attach(uint32_t program,uint32_t shader) {
    prepare();NSMutableArray *list=attachments[@(program)];
    if(!list){list=[NSMutableArray new];attachments[@(program)]=list;}
    NSString *key=shaderKeys[@(shader)];if(key && ![list containsObject:key])[list addObject:key];
}
double halo_debug_begin(void) {return CACurrentMediaTime();}
void halo_debug_end(uint32_t object,int program,double started) {
    if(!started)return;
    GLint ok=0;
    if(program)glGetProgramiv(object,GL_LINK_STATUS,&ok);else glGetShaderiv(object,GL_COMPILE_STATUS,&ok);
    double ms=(CACurrentMediaTime()-started)*1000;
    if(ms<2 && ok)return;
    prepare();if(events.count>=4096){dropped++;return;}
    NSArray *keys=program?(attachments[@(object)]?:@[]):(shaderKeys[@(object)]?@[shaderKeys[@(object)]]:@[]);
    [events addObject:@{@"stage":program?@"link":@"compile",@"milliseconds":@(ms),@"success":@(ok!=0),@"sources":keys,@"time":@([NSDate date].timeIntervalSince1970)}];
}
void halo_debug_use_program(uint32_t program) {currentProgram=program;}
double halo_debug_draw_begin(void) {
    if(!atomic_load(&recording) || !currentProgram)return 0;
    prepare();if([drawnPrograms containsObject:@(currentProgram)])return 0;
    [drawnPrograms addObject:@(currentProgram)];
    /* Drain older work outside the timed interval. This is deliberately
       intrusive diagnostics, never enabled during normal rendering. */
    glFinish();return CACurrentMediaTime();
}
void halo_debug_draw_end(double started) {
    if(!started)return;
    glFinish();double ms=(CACurrentMediaTime()-started)*1000;
    if(ms<2)return;
    if(events.count>=4096){dropped++;return;}
    [events addObject:@{@"stage":@"first_draw_wait",@"milliseconds":@(ms),@"sources":[attachments[@(currentProgram)] copy]?:@[],@"time":@([NSDate date].timeIntervalSince1970)}];
}
#if HALO_MACOS
int halo_debug_is_presented(void) {return debugPresented;}
#else
int halo_debug_is_presented(void) {return debugController!=nil;}
#endif
void halo_debug_forget(uint32_t object,int program) {
    if(program)[drawnPrograms removeObject:@(object)];
    if(program)[attachments removeObjectForKey:@(object)];else [shaderKeys removeObjectForKey:@(object)];
}
#if HALO_MACOS
static void exportReport(id presenter) {
#else
static void exportReport(UIViewController *presenter) {
#endif
    prepare();struct utsname device;uname(&device);
    NSMutableDictionary *programs=[NSMutableDictionary new];
    for(NSNumber *name in attachments)programs[name.stringValue]=[attachments[name] copy];
    NSDictionary *report=@{@"schema":@2,@"renderer":halo_graphics_metal()?@"Metal (ANGLE)":@"OpenGL ES (Apple)",@"gpu":glRenderer?:@"unknown",@"revision":@HALO_BUILD_REVISION,@"metalfx":@(halo_metalfx_enabled()!=0),@"device":[NSString stringWithUTF8String:device.machine],@"os":NSProcessInfo.processInfo.operatingSystemVersionString,
        @"build":[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"]?:@"unknown",@"threshold_ms":@2,
        @"note":@"CPU compile/link completion timings are always recorded. Synchronized first-use draw waits are opt-in. First-use waits include draw cost and may include deferred driver compilation; they are not isolated shader-compiler timings. Shader sources can guide warm-up updates.",
        @"first_use_gpu_recording":@(atomic_load(&recording)),@"programs":programs,
        @"dropped":@(dropped),@"events":[events copy],@"sources":[sources copy]};
    NSError *error=nil;NSData *data=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:&error];
    NSURL *folder=[[NSFileManager.defaultManager URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask].firstObject URLByAppendingPathComponent:@"ShaderReports" isDirectory:YES];
    [NSFileManager.defaultManager createDirectoryAtURL:folder withIntermediateDirectories:YES attributes:nil error:&error];
    NSArray<NSString *> *oldReports=[[NSFileManager.defaultManager contentsOfDirectoryAtPath:folder.path error:nil] filteredArrayUsingPredicate:[NSPredicate predicateWithBlock:^BOOL(NSString *name,NSDictionary *bindings){(void)bindings;return [name hasPrefix:@"halo-shaders-"] && [name hasSuffix:@".json"];}]];
    oldReports=[oldReports sortedArrayUsingSelector:@selector(compare:)];
    for(NSUInteger i=0;i+4<oldReports.count;i++)[NSFileManager.defaultManager removeItemAtURL:[folder URLByAppendingPathComponent:oldReports[i]] error:nil];
    NSURL *file=[folder URLByAppendingPathComponent:[NSString stringWithFormat:@"halo-shaders-%.0f.json",NSDate.date.timeIntervalSince1970]];
#if HALO_MACOS
    (void)presenter;
    NSString *testExport=NSProcessInfo.processInfo.environment[@"HALO_MAC_TEST_SHADER_EXPORT"];
    if(testExport.length) {
        if(!data || ![data writeToFile:testExport options:NSDataWritingAtomic error:&error])host_fatal("Shader report export failed: %s",error.localizedDescription.UTF8String);
        return;
    }
    if(!data || ![data writeToURL:file options:NSDataWritingAtomic error:&error]){NSAlert *alert=[NSAlert new];alert.messageText=@"Export failed";alert.informativeText=error.localizedDescription;[alert runModal];return;}
    NSSharingService *share=[NSSharingService sharingServiceNamed:NSSharingServiceNameSendViaAirDrop];
    [share performWithItems:@[file]];
}
#else
    if(!data || ![data writeToURL:file options:NSDataWritingAtomic error:&error]){UIAlertController *alert=[UIAlertController alertControllerWithTitle:@"Export failed" message:error.localizedDescription preferredStyle:UIAlertControllerStyleAlert];[alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];[presenter presentViewController:alert animated:YES completion:nil];return;}
    UIActivityViewController *share=[[UIActivityViewController alloc]initWithActivityItems:@[file] applicationActivities:nil];
    share.popoverPresentationController.sourceView=presenter.view;share.popoverPresentationController.sourceRect=CGRectMake(CGRectGetMidX(presenter.view.bounds),40,1,1);
    [presenter presentViewController:share animated:YES completion:nil];
}
@interface HaloDebugController : UITableViewController <UIAdaptivePresentationControllerDelegate>
@end
@implementation HaloDebugController
- (void)viewDidLoad {
    [super viewDidLoad];self.title=@"Settings";
    self.navigationItem.rightBarButtonItem=[[UIBarButtonItem alloc]initWithBarButtonSystemItem:UIBarButtonSystemItemDone target:self action:@selector(done)];
}
- (void)done {[self dismissViewControllerAnimated:YES completion:^{debugController=nil;host_ios_touch_focus();}];}
- (void)presentationControllerDidDismiss:(UIPresentationController *)presentationController {
    (void)presentationController;debugController=nil;host_ios_touch_focus();
}
- (NSInteger)tableView:(UITableView *)table numberOfRowsInSection:(NSInteger)section {(void)table;(void)section;return halo_graphics_prefer_metal()?5:4;}
- (UITableViewCell *)tableView:(UITableView *)table cellForRowAtIndexPath:(NSIndexPath *)path {
    (void)table;UITableViewCell *cell=[[UITableViewCell alloc]initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    cell.detailTextLabel.numberOfLines=0;
    BOOL selectedMetal=halo_graphics_prefer_metal();
    if(path.row==0) {
        cell.textLabel.text=@"Renderer";
        BOOL pending=selectedMetal!=halo_graphics_metal();
        cell.detailTextLabel.text=pending?@"Relaunch the app to use this renderer.":(halo_graphics_metal()?@"Metal via ANGLE. Changes apply after relaunch.":@"OpenGL ES. Changes apply after relaunch.");
        UISegmentedControl *choice=[[UISegmentedControl alloc]initWithItems:@[@"OpenGL",@"Metal"]];
        choice.selectedSegmentIndex=selectedMetal?1:0;
        [choice addTarget:self action:@selector(rendererChanged:) forControlEvents:UIControlEventValueChanged];cell.accessoryView=choice;
    } else if(selectedMetal && path.row==1) {
        UISwitch *toggle=[UISwitch new];toggle.tag=0;
        toggle.enabled=halo_graphics_metal() && halo_metalfx_supported();toggle.on=halo_metalfx_enabled();
        cell.textLabel.text=@"MetalFX spatial upscaling";
        cell.detailTextLabel.text=toggle.enabled?@"Render at up to 720p, then upscale with MetalFX.":(!halo_graphics_metal()?@"Select Metal and relaunch to enable.":@"Unavailable on this device.");
        [toggle addTarget:self action:@selector(changed:) forControlEvents:UIControlEventValueChanged];cell.accessoryView=toggle;
    } else {
        NSInteger action=path.row-(selectedMetal?2:1);
        if(action==0) {
            UISwitch *toggle=[UISwitch new];toggle.tag=1;toggle.on=atomic_load(&recording);
            cell.textLabel.text=@"Record first-use GPU stalls";cell.detailTextLabel.text=@"Compile/link stalls are always recorded. This also times first draws and adds GPU synchronization overhead.";
            [toggle addTarget:self action:@selector(changed:) forControlEvents:UIControlEventValueChanged];cell.accessoryView=toggle;
        } else if(action==1){cell.textLabel.text=@"Share shader report…";cell.detailTextLabel.text=@"Choose AirDrop in the share sheet to send it to your Mac.";}
        else{cell.textLabel.text=@"Clear recorded events";cell.detailTextLabel.text=[NSString stringWithFormat:@"%lu events; %lu dropped",(unsigned long)events.count,(unsigned long)dropped];}
    }
    return cell;
}
- (void)rendererChanged:(UISegmentedControl *)sender {
    halo_graphics_set_preference(sender.selectedSegmentIndex==1);[self.tableView reloadData];
}
- (void)changed:(UISwitch *)sender {
    if(!sender.tag)halo_metalfx_set_enabled(sender.on);else {atomic_store(&recording,sender.on);[drawnPrograms removeAllObjects];}
    [NSUserDefaults.standardUserDefaults setBool:sender.on forKey:sender.tag?@"HaloRecordShaderStalls":@"HaloMetalFX"];
}
- (void)tableView:(UITableView *)table didSelectRowAtIndexPath:(NSIndexPath *)path {
    [table deselectRowAtIndexPath:path animated:YES];
    NSInteger action=path.row-(halo_graphics_prefer_metal()?2:1);
    if(action==1)exportReport(self);
    else if(action==2){[events removeAllObjects];[drawnPrograms removeAllObjects];dropped=0;[table reloadData];}
}
@end
void halo_debug_present(void) {
    if(debugController)return;prepare();
    UIWindow *window=nil;for(UIScene *scene in UIApplication.sharedApplication.connectedScenes)if([scene isKindOfClass:UIWindowScene.class])for(UIWindow *w in ((UIWindowScene*)scene).windows)if(w.isKeyWindow)window=w;
    UIViewController *presenter=window.rootViewController;
    while(presenter.presentedViewController)presenter=presenter.presentedViewController;
    if(!presenter)return;
    host_ios_touch_reset();HaloDebugController *controller=[[HaloDebugController alloc]initWithStyle:UITableViewStyleInsetGrouped];
    UINavigationController *navigation=[[UINavigationController alloc]initWithRootViewController:controller];
    navigation.modalPresentationStyle=UIModalPresentationPageSheet;debugController=navigation;
    [presenter presentViewController:navigation animated:YES completion:nil];
    navigation.presentationController.delegate=controller;
}

#endif
#if HALO_MACOS
@interface HaloDebugActions : NSWindowController <NSWindowDelegate>
@property NSButton *upscaleButton;
@property NSButton *recordButton;
@property NSTextField *eventCount;
@end
@implementation HaloDebugActions
- (instancetype)init {
    NSWindow *window=[[NSWindow alloc]initWithContentRect:NSMakeRect(0,0,480,360)
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:NO];
    self=[super initWithWindow:window];
    if(self) {
        window.title=@"Settings";window.releasedWhenClosed=NO;window.delegate=self;
        NSStackView *stack=[NSStackView new];stack.orientation=NSUserInterfaceLayoutOrientationVertical;
        stack.alignment=NSLayoutAttributeLeading;stack.spacing=14;stack.translatesAutoresizingMaskIntoConstraints=NO;
        [window.contentView addSubview:stack];
        [NSLayoutConstraint activateConstraints:@[
            [stack.leadingAnchor constraintEqualToAnchor:window.contentView.leadingAnchor constant:24],
            [stack.trailingAnchor constraintEqualToAnchor:window.contentView.trailingAnchor constant:-24],
            [stack.topAnchor constraintEqualToAnchor:window.contentView.topAnchor constant:24],
            [stack.bottomAnchor constraintLessThanOrEqualToAnchor:window.contentView.bottomAnchor constant:-24]]];
        [stack addArrangedSubview:[NSTextField labelWithString:@"Renderer: Metal (ANGLE)"]];
        self.upscaleButton=[NSButton checkboxWithTitle:@"MetalFX spatial upscaling" target:self action:@selector(upscale:)];
        [stack addArrangedSubview:self.upscaleButton];
        self.recordButton=[NSButton checkboxWithTitle:@"Record first-use GPU stalls" target:self action:@selector(record:)];
        [stack addArrangedSubview:self.recordButton];
        NSTextField *note=[NSTextField wrappingLabelWithString:@"Compile/link stalls are recorded automatically. First-use GPU recording adds synchronization overhead; disable it for normal play."];
        [stack addArrangedSubview:note];[note.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active=YES;
        [stack addArrangedSubview:[NSButton buttonWithTitle:@"AirDrop shader report…" target:self action:@selector(share:)]];
        [stack addArrangedSubview:[NSButton buttonWithTitle:@"Clear recorded events" target:self action:@selector(clear:)]];
        self.eventCount=[NSTextField labelWithString:@""];[stack addArrangedSubview:self.eventCount];
        [window center];
    }
    return self;
}
-(void)refresh {
    self.upscaleButton.enabled=halo_metalfx_supported();self.upscaleButton.state=halo_metalfx_enabled();
    self.recordButton.state=atomic_load(&recording);
    self.eventCount.stringValue=[NSString stringWithFormat:@"%lu events; %lu dropped",(unsigned long)events.count,(unsigned long)dropped];
}
-(void)openSettings:(id)sender {
    (void)sender;[self refresh];debugPresented=YES;[self showWindow:nil];[self.window makeKeyAndOrderFront:nil];
    NSString *capture=NSProcessInfo.processInfo.environment[@"HALO_MAC_TEST_SETTINGS_IMAGE"];
    if(capture.length) {
        NSView *view=self.window.contentView;[view layoutSubtreeIfNeeded];
        NSBitmapImageRep *bitmap=[view bitmapImageRepForCachingDisplayInRect:view.bounds];
        [view cacheDisplayInRect:view.bounds toBitmapImageRep:bitmap];
        [[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}]writeToFile:capture atomically:YES];
    }
}
-(void)windowWillClose:(NSNotification *)notification {(void)notification;debugPresented=NO;}
-(void)upscale:(NSButton *)item {BOOL on=item.state==NSControlStateValueOn;halo_metalfx_set_enabled(on);[NSUserDefaults.standardUserDefaults setBool:on forKey:@"HaloMetalFX"];[self refresh];}
-(void)record:(NSButton *)item {BOOL on=item.state==NSControlStateValueOn;atomic_store(&recording,on);[drawnPrograms removeAllObjects];[NSUserDefaults.standardUserDefaults setBool:on forKey:@"HaloRecordShaderStalls"];[self refresh];}
-(void)share:(id)sender {(void)sender;exportReport(nil);}
-(void)clear:(id)sender {(void)sender;[events removeAllObjects];[drawnPrograms removeAllObjects];dropped=0;[self refresh];}
@end
static HaloDebugActions *actions;
void halo_debug_install_menu(void) {
    if(actions)return;prepare();actions=[HaloDebugActions new];
    NSMenu *menu=NSApp.mainMenu.itemArray.firstObject.submenu;
    if(!menu)return;
    /* Reuse SDL's standard Preferences placeholder. AppKit reserves Cmd+,
       for that item and strips it from duplicate Settings entries. */
    NSMenuItem *item=nil;
    for(NSMenuItem *candidate in menu.itemArray)if([candidate.keyEquivalent isEqualToString:@","]){item=candidate;break;}
    if(!item){item=[[NSMenuItem alloc]initWithTitle:@"Settings…" action:NULL keyEquivalent:@","];[menu insertItem:item atIndex:MIN(2,menu.numberOfItems)];}
    item.title=@"Settings…";item.action=@selector(openSettings:);item.target=actions;
}
void halo_debug_present(void) {halo_debug_install_menu();[actions openSettings:nil];}
#endif

/* Developer regression: compile every source in a received report using the
   actual bundled driver. No game state, textures or buffers are changed. */
void halo_debug_replay_shader_report(void) {
#if HALO_MACOS
    NSString *path=NSProcessInfo.processInfo.environment[@"HALO_MAC_TEST_SHADER_REPORT"];
    if(!path.length)return;
    NSData *data=[NSData dataWithContentsOfFile:path];
    NSDictionary *report=data?[NSJSONSerialization JSONObjectWithData:data options:0 error:nil]:nil;
    NSDictionary *entries=[report isKindOfClass:NSDictionary.class]?report[@"sources"]:nil;
    if(![entries isKindOfClass:NSDictionary.class] || entries.count>4096)host_fatal("Invalid shader replay report");
    __typeof__(&glCreateShader) create=halo_graphics_proc("glCreateShader");
    __typeof__(&glShaderSource) source=halo_graphics_proc("glShaderSource");
    __typeof__(&glCompileShader) compile=halo_graphics_proc("glCompileShader");
    __typeof__(&glDeleteShader) destroy=halo_graphics_proc("glDeleteShader");
    __typeof__(&glGetShaderInfoLog) getLog=halo_graphics_proc("glGetShaderInfoLog");
    unsigned passed=0,failed=0;
    for(NSString *key in entries) {
        NSDictionary *entry=entries[key];
        if(![entry isKindOfClass:NSDictionary.class])host_fatal("Invalid shader replay entry");
        NSString *text=entry[@"source"],*kind=entry[@"type"];
        if(![text isKindOfClass:NSString.class] || text.length>1024*1024 ||
            (![kind isEqualToString:@"vertex"] && ![kind isEqualToString:@"fragment"]))host_fatal("Invalid shader replay entry");
        GLuint shader=create([kind isEqualToString:@"vertex"]?GL_VERTEX_SHADER:GL_FRAGMENT_SHADER);
        const char *utf8=text.UTF8String;source(shader,1,&utf8,NULL);compile(shader);
        GLint ok=0;glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
        if(ok)passed++;else {char log[4096];getLog(shader,sizeof(log),NULL,log);host_logf(HOST_LOG_ERROR,"Shader replay %s failed: %s",key.UTF8String,log);failed++;}
        destroy(shader);
    }
    host_logf(HOST_LOG_INFO,"Shader replay: %u compiled, %u failed",passed,failed);
#endif
}

void halo_debug_test_export(void) {
#if HALO_MACOS
    if(NSProcessInfo.processInfo.environment[@"HALO_MAC_TEST_SHADER_EXPORT"].length)exportReport(nil);
#endif
}
