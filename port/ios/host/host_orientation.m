#import "host_orientation.h"
#include "ios_host.h"

/* SDL 3.4.16 owns these controllers. These categories add only orientation
   preferences inherited from UIKit, never replace SDL's own methods. Recheck
   the selectors when upgrading SDL; no runtime class mutation is needed. */
@interface SDL_uikitviewcontroller : UIViewController
@end
@interface SDLLaunchScreenController : UIViewController
@end
@interface SDLUIKitSceneDelegate : NSObject <UIWindowSceneDelegate>
@end

static BOOL landscape_lock(UIViewController *controller) {
    /* Don't lock a transient portrait scene before the geometry request has
       completed. The scene callback updates this preference after rotation. */
    return UIInterfaceOrientationIsLandscape(controller.viewIfLoaded.window.windowScene.interfaceOrientation);
}

#define HALO_LANDSCAPE_PREFERENCES \
- (UIInterfaceOrientation)preferredInterfaceOrientationForPresentation { return UIInterfaceOrientationLandscapeRight; } \
- (BOOL)prefersInterfaceOrientationLocked { return landscape_lock(self); }

@implementation HaloLandscapeController
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
HALO_LANDSCAPE_PREFERENCES
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    host_ios_require_landscape(self.view.window);
}
@end

@implementation HaloLandscapeDocumentPicker
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
HALO_LANDSCAPE_PREFERENCES
@end

@implementation SDL_uikitviewcontroller (HaloLandscape)
HALO_LANDSCAPE_PREFERENCES
@end

@implementation SDLLaunchScreenController (HaloLandscape)
HALO_LANDSCAPE_PREFERENCES
@end

@implementation SDLUIKitSceneDelegate (HaloLandscape)
/* iPadOS 27 consults the scene delegate in preference to Info.plist. */
- (UIInterfaceOrientationMask)supportedInterfaceOrientationsForWindowScene:(UIWindowScene *)scene {
    (void)scene;
    return UIInterfaceOrientationMaskLandscape;
}
#if __IPHONE_OS_VERSION_MAX_ALLOWED >= 260000
- (void)windowScene:(UIWindowScene *)scene didUpdateEffectiveGeometry:(UIWindowSceneGeometry *)previousGeometry API_AVAILABLE(ios(26.0)) {
    BOOL wasLandscape=UIInterfaceOrientationIsLandscape(previousGeometry.interfaceOrientation);
    BOOL isLandscape=UIInterfaceOrientationIsLandscape(scene.effectiveGeometry.interfaceOrientation);
    if (wasLandscape != isLandscape) {
        for (UIWindow *window in scene.windows) {
            UIViewController *controller=window.rootViewController;
            while (controller.presentedViewController) controller=controller.presentedViewController;
            [controller setNeedsUpdateOfPrefersInterfaceOrientationLocked];
        }
    }
    CGSize size=scene.effectiveGeometry.coordinateSpace.bounds.size;
    host_logf(HOST_LOG_INFO,"scene geometry %.0fx%.0f orientation=%ld locked=%d",
        size.width,size.height,(long)scene.effectiveGeometry.interfaceOrientation,
        scene.effectiveGeometry.isInterfaceOrientationLocked);
}
#endif
@end

void host_ios_require_landscape(UIWindow *window) {
    if (!window) return;
    [window.rootViewController setNeedsUpdateOfSupportedInterfaceOrientations];
#if __IPHONE_OS_VERSION_MAX_ALLOWED >= 260000
    if (@available(iOS 26.0,*)) [window.rootViewController setNeedsUpdateOfPrefersInterfaceOrientationLocked];
#endif
    UIWindowSceneGeometryPreferencesIOS *geometry=[[UIWindowSceneGeometryPreferencesIOS alloc]
        initWithInterfaceOrientations:UIInterfaceOrientationMaskLandscape];
    [window.windowScene requestGeometryUpdateWithPreferences:geometry errorHandler:^(NSError *error) {
        host_logf(HOST_LOG_WARN,"landscape request: %s",error.localizedDescription.UTF8String);
    }];
}
