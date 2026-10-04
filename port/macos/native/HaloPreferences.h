#import <Foundation/Foundation.h>

/* User data stays outside the signed app and survives an application update. */
@interface HaloPreferences : NSObject
@property(nonatomic, readonly) NSURL *supportDirectory;
@property(nonatomic, readonly) NSString *dataPath;
@property(nonatomic, readonly) NSString *isoPath;
@property(nonatomic, readonly) BOOL chooseDataOnLaunch;
- (instancetype)initWithSupportDirectory:(NSURL *)directory;
- (BOOL)selectDataRoot:(NSURL *)root iso:(NSURL *)iso error:(NSError **)error;
- (BOOL)setChooseDataOnLaunch:(BOOL)choose error:(NSError **)error;
@end

/* Accept the extracted game directory or its maps subdirectory. Header checks
   establish the supported format, not a claim of complete gameplay parity. */
NSURL *HaloValidateGameData(NSURL *selection, NSError **error);
NSURL *HaloImportDiscImage(NSURL *image, NSURL *supportDirectory,
                          void (*progress)(void *, const char *, unsigned long long, unsigned long long),
                          void *context, NSError **error);
