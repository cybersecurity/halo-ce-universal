/* Native Files picker and transactional XISO import before the game starts. */
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include "ios_host.h"
#include "xiso.h"
#import "host_orientation.h"

#if !__has_feature(objc_arc)
#error The asynchronous importer requires Objective-C ARC.
#endif

@interface HaloImportController : HaloLandscapeController <UIDocumentPickerDelegate>
@property(nonatomic, copy) NSString *documents;
@property(nonatomic, strong) UILabel *statusLabel;
@property(nonatomic, strong) UIButton *chooseButton;
@property(nonatomic, strong) UIButton *cancelButton;
@property(nonatomic, strong) UIProgressView *progress;
@property(nonatomic, strong) UIActivityIndicatorView *spinner;
@property(nonatomic, strong) NSURL *pendingImage;
@property(atomic) BOOL cancelled;
@property(atomic, strong) NSFileCoordinator *coordinator;
@property(nonatomic) BOOL importing;
@property(nonatomic) BOOL ready;
@property(nonatomic) UIBackgroundTaskIdentifier backgroundTask;
@property(nonatomic) double lastProgressTime; /* extraction thread only */
@end

static int import_progress(void *context, const char *file, uint64_t done, uint64_t total) {
    HaloImportController *controller=(__bridge HaloImportController *)context;
    if (controller.cancelled) return 0;
    double now=NSProcessInfo.processInfo.systemUptime;
    if (done!=total && done && now-controller.lastProgressTime<0.1) return 1;
    controller.lastProgressTime=now;
    NSString *name=[NSString stringWithUTF8String:file];
    float fraction=total?(float)((double)done/total):0;
    dispatch_async(dispatch_get_main_queue(), ^{
        controller.progress.hidden=NO;
        [controller.spinner stopAnimating];
        controller.progress.progress=fraction;
        controller.statusLabel.text=[NSString stringWithFormat:@"Importing %@ · %.0f%%",name,fraction*100];
    });
    return 1;
}

@implementation HaloImportController
- (void)viewDidLoad {
    [super viewDidLoad];
    self.backgroundTask=UIBackgroundTaskInvalid;
    self.view.backgroundColor=[UIColor colorWithRed:0.025 green:0.045 blue:0.055 alpha:1];
    self.overrideUserInterfaceStyle=UIUserInterfaceStyleDark;
    UIScrollView *scroll=[UIScrollView new];scroll.translatesAutoresizingMaskIntoConstraints=NO;
    [self.view addSubview:scroll];
    UIView *content=[UIView new];content.translatesAutoresizingMaskIntoConstraints=NO;[scroll addSubview:content];
    UIStackView *stack=[UIStackView new];stack.axis=UILayoutConstraintAxisVertical;stack.spacing=14;
    stack.alignment=UIStackViewAlignmentCenter;stack.translatesAutoresizingMaskIntoConstraints=NO;[content addSubview:stack];
    UIImageView *icon=[[UIImageView alloc] initWithImage:[UIImage imageNamed:@"AppIcon60x60"]];
    icon.contentMode=UIViewContentModeScaleAspectFill;icon.clipsToBounds=YES;icon.layer.cornerRadius=16;
    [icon.widthAnchor constraintEqualToConstant:72].active=YES;[icon.heightAnchor constraintEqualToConstant:72].active=YES;
    [stack addArrangedSubview:icon];
    UILabel *title=[UILabel new];title.text=@"Halo: CE";title.font=[UIFont preferredFontForTextStyle:UIFontTextStyleLargeTitle];
    title.adjustsFontForContentSizeCategory=YES;title.textAlignment=NSTextAlignmentCenter;[stack addArrangedSubview:title];
    UILabel *body=[UILabel new];body.text=@"Choose your own Halo: Combat Evolved Xbox XISO.\nWe'll import the game and start it for you.";
    body.font=[UIFont preferredFontForTextStyle:UIFontTextStyleBody];body.adjustsFontForContentSizeCategory=YES;
    body.numberOfLines=0;body.textAlignment=NSTextAlignmentCenter;body.textColor=UIColor.secondaryLabelColor;[stack addArrangedSubview:body];
    self.chooseButton=[UIButton buttonWithType:UIButtonTypeSystem];
    UIButtonConfiguration *style=UIButtonConfiguration.filledButtonConfiguration;
    style.title=@"Choose Halo XISO";style.baseBackgroundColor=[UIColor colorWithRed:0.36 green:0.65 blue:0.31 alpha:1];
    style.baseForegroundColor=UIColor.blackColor;style.cornerStyle=UIButtonConfigurationCornerStyleLarge;
    style.contentInsets=NSDirectionalEdgeInsetsMake(14,24,14,24);self.chooseButton.configuration=style;
    self.chooseButton.accessibilityIdentifier=@"chooseXISO";
    [self.chooseButton addTarget:self action:@selector(chooseImage) forControlEvents:UIControlEventTouchUpInside];[stack addArrangedSubview:self.chooseButton];
    self.statusLabel=[UILabel new];self.statusLabel.font=[UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    self.statusLabel.adjustsFontForContentSizeCategory=YES;self.statusLabel.numberOfLines=0;self.statusLabel.textAlignment=NSTextAlignmentCenter;
    self.statusLabel.text=@"Original Xbox PAL or NTSC-US · .iso or .xiso\nNo game files are included.";
    self.statusLabel.accessibilityIdentifier=@"importStatus";[stack addArrangedSubview:self.statusLabel];
    self.spinner=[[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
    self.spinner.hidesWhenStopped=YES;[stack addArrangedSubview:self.spinner];
    self.progress=[[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];self.progress.hidden=YES;
    self.progress.accessibilityLabel=@"Game import progress";[stack addArrangedSubview:self.progress];
    [self.progress.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active=YES;
    self.cancelButton=[UIButton buttonWithType:UIButtonTypeSystem];[self.cancelButton setTitle:@"Cancel import" forState:UIControlStateNormal];
    self.cancelButton.accessibilityIdentifier=@"cancelImport";
    [self.cancelButton addTarget:self action:@selector(cancelImport) forControlEvents:UIControlEventTouchUpInside];
    self.cancelButton.hidden=YES;[stack addArrangedSubview:self.cancelButton];
    NSLayoutConstraint *width=[stack.widthAnchor constraintEqualToAnchor:content.widthAnchor constant:-48];width.priority=999;
    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor],
        [scroll.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],[scroll.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [content.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor],[content.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor],
        [content.leadingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.leadingAnchor],[content.trailingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.trailingAnchor],
        [content.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor],[content.heightAnchor constraintGreaterThanOrEqualToAnchor:scroll.frameLayoutGuide.heightAnchor],
        [stack.centerXAnchor constraintEqualToAnchor:content.centerXAnchor],[stack.centerYAnchor constraintEqualToAnchor:content.centerYAnchor],
        [stack.topAnchor constraintGreaterThanOrEqualToAnchor:content.topAnchor constant:24],
        [stack.bottomAnchor constraintLessThanOrEqualToAnchor:content.bottomAnchor constant:-24],
        [stack.widthAnchor constraintLessThanOrEqualToConstant:520],width]];
}
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    if (self.pendingImage && !self.importing) {NSURL *url=self.pendingImage;self.pendingImage=nil;[self importImage:url];}
}
- (void)chooseImage {
    if (self.importing) return;
    /* Some Files providers classify .xiso as generic data. Validate the file's
       contents ourselves, allowing both that type and normal ISO disk images. */
    UIDocumentPickerViewController *picker=[[HaloLandscapeDocumentPicker alloc] initForOpeningContentTypes:@[UTTypeData] asCopy:NO];
    picker.delegate=self;picker.allowsMultipleSelection=NO;
    [self presentViewController:picker animated:YES completion:nil];
}
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    (void)controller;if (urls.count) [self importImage:urls.firstObject];
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {
    (void)controller;self.statusLabel.text=@"Choose your Halo XISO whenever you're ready.";
}
- (void)cancelImport {self.cancelled=YES;[self.coordinator cancel];self.cancelButton.enabled=NO;self.statusLabel.text=@"Cancelling import…";}
- (void)importImage:(NSURL *)url {
    if (self.importing) return;
    self.importing=YES;self.cancelled=NO;self.lastProgressTime=0;
    self.chooseButton.hidden=YES;self.cancelButton.hidden=NO;self.cancelButton.enabled=YES;
    self.progress.hidden=YES;self.progress.progress=0;[self.spinner startAnimating];
    self.statusLabel.text=@"Opening disc image… If it's in iCloud, it may need to download first.";
    BOOL scoped=[url startAccessingSecurityScopedResource];
    self.backgroundTask=[UIApplication.sharedApplication beginBackgroundTaskWithName:@"Import Halo" expirationHandler:^{self.cancelled=YES;[self.coordinator cancel];}];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0), ^{
        @autoreleasepool {
            NSFileManager *files=NSFileManager.defaultManager;
            NSString *staging=[self.documents stringByAppendingPathComponent:[@".halo-import-" stringByAppendingString:NSUUID.UUID.UUIDString]];
            NSError *folderError=nil;__block NSString *failure=nil;__block BOOL imported=NO;
            if (![files createDirectoryAtPath:staging withIntermediateDirectories:NO attributes:nil error:&folderError]) failure=@"Could not create the import folder. Check available storage.";
            if (!failure) {
                [@"Halo XISO import v1" writeToFile:[staging stringByAppendingPathComponent:@"owner.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
                [[NSURL fileURLWithPath:staging] setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
                NSFileCoordinator *coordinator=[[NSFileCoordinator alloc] initWithFilePresenter:nil];NSError *coordinationError=nil;
                self.coordinator=coordinator;
                if (self.cancelled) [coordinator cancel];
                [coordinator coordinateReadingItemAtURL:url options:0 error:&coordinationError byAccessor:^(NSURL *readable) {
                    char error[1024]={0};
                    if (self.cancelled) {failure=@"Import cancelled.";return;}
                    imported=xiso_extract_maps(readable.fileSystemRepresentation,staging.fileSystemRepresentation,import_progress,(__bridge void *)self,error,sizeof(error));
                    if (!imported) failure=[NSString stringWithUTF8String:error];
                }];
                if (coordinationError) {imported=NO;failure=self.cancelled?@"Import cancelled.":@"Could not access the disc image. Download it in Files, then choose it again.";}
                self.coordinator=nil;
            }
            if (imported && self.cancelled) {imported=NO;failure=@"Import cancelled.";}
            if (imported) {
                NSString *target=[self.documents stringByAppendingPathComponent:@"maps"];
                NSString *source=[staging stringByAppendingPathComponent:@"maps"];
                NSString *backup=[staging stringByAppendingPathComponent:@"previous-maps"];
                NSError *moveError=nil;
                BOOL existing=[files fileExistsAtPath:target];
                if (existing && ![files moveItemAtPath:target toPath:backup error:&moveError]) {imported=NO;failure=@"Could not replace incomplete game data. Your existing files were kept.";}
                if (imported && ![files moveItemAtPath:source toPath:target error:&moveError]) {
                    imported=NO;failure=@"Could not finish the game import. Try again.";
                    if (existing && ![files moveItemAtPath:backup toPath:target error:nil])
                        failure=@"Could not finish the import. Existing maps remain in the import backup folder.";
                }
                if (imported) {
                    NSURL *mapsURL=[NSURL fileURLWithPath:target];[mapsURL setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
                    /* Successful completion marker used only for diagnostics; game data stays separate. */
                    host_logf(HOST_LOG_INFO,"XISO import complete; original map bytes preserved");
                }
                /* Preserve the backup if an unexpected restore failure left it here. */
                if (!imported && [files fileExistsAtPath:backup]) staging=nil;
            }
            if (staging) [files removeItemAtPath:staging error:nil];
            if (scoped) [url stopAccessingSecurityScopedResource];
            dispatch_async(dispatch_get_main_queue(), ^{
                [self.spinner stopAnimating];self.cancelButton.hidden=YES;self.importing=NO;
                if (self.backgroundTask!=UIBackgroundTaskInvalid) { [UIApplication.sharedApplication endBackgroundTask:self.backgroundTask];self.backgroundTask=UIBackgroundTaskInvalid; }
                if (imported) {self.statusLabel.text=@"Ready. Starting Halo…";self.ready=YES;}
                else {self.statusLabel.text=failure?:@"Could not import this disc image.";self.chooseButton.hidden=NO;self.progress.hidden=YES;}
            });
        }
    });
}
@end

void host_ios_prepare_assets(const char *root) {
    NSString *documents=[NSString stringWithUTF8String:root];
    NSFileManager *files=NSFileManager.defaultManager;
    NSString *maps=[documents stringByAppendingPathComponent:@"maps"];
    /* Recover only directories carrying our own marker. Interrupted copies
       can be discarded; a displaced maps folder is restored before retrying. */
    for (NSString *name in [files contentsOfDirectoryAtPath:documents error:nil]) {
        if (![name hasPrefix:@".halo-import-"] || ![[NSUUID alloc] initWithUUIDString:[name substringFromIndex:13]]) continue;
        NSString *staging=[documents stringByAppendingPathComponent:name];
        if (![[NSString stringWithContentsOfFile:[staging stringByAppendingPathComponent:@"owner.txt"] encoding:NSUTF8StringEncoding error:nil] isEqualToString:@"Halo XISO import v1"]) continue;
        NSString *backup=[staging stringByAppendingPathComponent:@"previous-maps"];
        if ([files fileExistsAtPath:backup]) {
            if (![files fileExistsAtPath:maps]) [files moveItemAtPath:backup toPath:maps error:nil];
            if ([files fileExistsAtPath:backup] && !xiso_maps_ready(maps.fileSystemRepresentation,NULL,0)) continue;
        }
        [files removeItemAtPath:staging error:nil];
    }
    char reason[1024]={0};
    if (xiso_maps_ready(maps.fileSystemRepresentation,reason,sizeof(reason))) return;
    UIWindowScene *scene=nil;
    for (UIScene *candidate in UIApplication.sharedApplication.connectedScenes)
        if ([candidate isKindOfClass:UIWindowScene.class]) {scene=(UIWindowScene *)candidate;break;}
    if (!scene) host_fatal("Could not open the game import screen.");
    HaloImportController *controller=[HaloImportController new];controller.documents=documents;
    /* Dropping an XISO into the app with Finder/Files also works. Only choose
       automatically when exactly one image is present; never alter the source. */
    NSMutableArray<NSURL *> *images=[NSMutableArray new];
    for (NSString *directory in @[documents,[documents stringByAppendingPathComponent:@"Inbox"]]) {
        for (NSURL *url in [files contentsOfDirectoryAtURL:[NSURL fileURLWithPath:directory] includingPropertiesForKeys:@[NSURLIsRegularFileKey] options:NSDirectoryEnumerationSkipsHiddenFiles error:nil]) {
            NSString *extension=url.pathExtension.lowercaseString;NSNumber *regular=nil;[url getResourceValue:&regular forKey:NSURLIsRegularFileKey error:nil];
            if (regular.boolValue && ([extension isEqualToString:@"iso"] || [extension isEqualToString:@"xiso"])) [images addObject:url];
        }
    }
    if (images.count==1) controller.pendingImage=images.firstObject;
    UIWindow *window=[[UIWindow alloc] initWithWindowScene:scene];window.frame=scene.coordinateSpace.bounds;
    window.windowLevel=UIWindowLevelNormal+2;window.rootViewController=controller;[window makeKeyAndVisible];
    while (!controller.ready) {
        @autoreleasepool {
            /* Like SDL's UIKit event pump, service tracking as well as the
               default mode so controls inside the scroll view receive taps. */
            CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.01,true);
            CFRunLoopRunInMode((__bridge CFStringRef)UITrackingRunLoopMode,0.01,true);
        }
    }
    window.hidden=YES;window.rootViewController=nil;
}
