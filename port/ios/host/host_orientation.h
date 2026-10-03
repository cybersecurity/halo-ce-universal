#pragma once
#import <UIKit/UIKit.h>

/* Keep launch, import, Files, and SDL on the same landscape policy. */
@interface HaloLandscapeController : UIViewController
@end

@interface HaloLandscapeDocumentPicker : UIDocumentPickerViewController
@end

void host_ios_require_landscape(UIWindow *window);
