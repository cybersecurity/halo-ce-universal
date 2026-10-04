#import "HaloPreferences.h"
#include "xiso.h"
#include <string.h>

static NSError *failure(NSString *message) {
    return [NSError errorWithDomain:@"HaloGameData" code:1
                          userInfo:@{NSLocalizedDescriptionKey: message}];
}

@implementation HaloPreferences {
    NSMutableDictionary *_settings;
}
- (instancetype)initWithSupportDirectory:(NSURL *)directory {
    if ((self = [super init])) {
        _supportDirectory = directory;
        NSData *data = [NSData dataWithContentsOfURL:[directory URLByAppendingPathComponent:@"macos-settings.json"]];
        id decoded = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
        _settings = [decoded isKindOfClass:NSDictionary.class] ? [decoded mutableCopy] : [NSMutableDictionary dictionary];
    }
    return self;
}
- (NSString *)dataPath { return [_settings[@"data_path"] isKindOfClass:NSString.class] ? _settings[@"data_path"] : nil; }
- (NSString *)isoPath { return [_settings[@"iso_path"] isKindOfClass:NSString.class] ? _settings[@"iso_path"] : nil; }
- (BOOL)chooseDataOnLaunch { return [_settings[@"choose_data_on_launch"] isKindOfClass:NSNumber.class] && [_settings[@"choose_data_on_launch"] boolValue]; }
- (BOOL)save:(NSMutableDictionary *)settings error:(NSError **)error {
    NSData *data = [NSJSONSerialization dataWithJSONObject:settings options:NSJSONWritingPrettyPrinted error:error];
    if (!data || ![NSFileManager.defaultManager createDirectoryAtURL:_supportDirectory
                                       withIntermediateDirectories:YES attributes:nil error:error]) return NO;
    if (![data writeToURL:[_supportDirectory URLByAppendingPathComponent:@"macos-settings.json"]
                 options:NSDataWritingAtomic error:error]) return NO;
    _settings = settings;
    return YES;
}
- (BOOL)selectDataRoot:(NSURL *)root iso:(NSURL *)iso error:(NSError **)error {
    NSURL *validated = HaloValidateGameData(root, error);
    if (!validated) return NO;
    NSMutableDictionary *settings = [_settings mutableCopy];
    settings[@"data_path"] = validated.path;
    [settings removeObjectForKey:@"choose_data_on_launch"];
    if (iso) settings[@"iso_path"] = iso.path;
    else [settings removeObjectForKey:@"iso_path"];
    return [self save:settings error:error];
}
- (BOOL)setChooseDataOnLaunch:(BOOL)choose error:(NSError **)error {
    NSMutableDictionary *settings = [_settings mutableCopy];
    settings[@"choose_data_on_launch"] = @(choose);
    return [self save:settings error:error];
}
@end

static NSDictionary<NSString *, NSURL *> *entries(NSURL *directory, NSError **error) {
    NSArray<NSURL *> *files = [NSFileManager.defaultManager contentsOfDirectoryAtURL:directory
                          includingPropertiesForKeys:@[NSURLIsRegularFileKey] options:0 error:error];
    if (!files) return nil;
    NSMutableDictionary *result = [NSMutableDictionary dictionary];
    for (NSURL *file in files) {
        NSString *name = file.lastPathComponent.lowercaseString;
        if (result[name]) {
            if (error) *error = failure(@"This folder contains duplicate names with different capitalization.");
            return nil;
        }
        result[name] = file;
    }
    return result;
}

NSURL *HaloValidateGameData(NSURL *selection, NSError **error) {
    NSURL *root = selection.URLByStandardizingPath.URLByResolvingSymlinksInPath;
    NSURL *maps;
    if ([root.lastPathComponent.lowercaseString isEqualToString:@"maps"]) {
        maps = root;
        root = root.URLByDeletingLastPathComponent;
    } else {
        maps = entries(root, error)[@"maps"];
    }
    if (!maps) {
        if (error) *error = failure(@"Choose an extracted Xbox Halo game folder containing maps, or the maps folder itself.");
        return nil;
    }
    NSDictionary<NSString *, NSURL *> *files = entries(maps, error);
    if (!files) return nil;
    /* Establish the Xbox base data here. Extra Custom Edition/OpenSauce maps
       and resource files belong to the engine's existing format validator;
       they must neither reject this folder nor stand in for a retail map. */
    NSSet<NSString *> *retailNames = [NSSet setWithArray:@[
        @"ui", @"a10", @"a30", @"a50", @"b30", @"b40", @"c10", @"c20", @"c40", @"d20", @"d40",
        @"beavercreek", @"sidewinder", @"damnation", @"ratrace", @"prisoner", @"hangemhigh", @"chillout",
        @"carousel", @"boardingaction", @"bloodgulch", @"wizard", @"putput", @"longest"
    ]];
    NSString *discBuild = nil;
    BOOL hasUI = NO, hasOpeningLevel = NO;
    for (NSString *name in files) {
        if (![name.pathExtension isEqualToString:@"map"] ||
            ![retailNames containsObject:name.stringByDeletingPathExtension]) continue;
        NSURL *file = files[name];
        NSFileHandle *handle = [NSFileHandle fileHandleForReadingFromURL:file error:error];
        if (!handle) return nil;
        NSData *header = [handle readDataUpToLength:2048 error:error];
        [handle closeFile];
        if (!header) return nil;
        const unsigned char *bytes = header.bytes;
        BOOL valid = header.length == 2048 && !memcmp(bytes, "daeh", 4) && !memcmp(bytes + 2044, "toof", 4);
        uint32_t version = 0, length = 0;
        NSString *cacheName = nil, *build = nil;
        if (valid) {
            memcpy(&version, bytes + 4, 4);
            memcpy(&length, bytes + 8, 4);
            const unsigned char *nameEnd = memchr(bytes + 32, 0, 32), *buildEnd = memchr(bytes + 64, 0, 32);
            if (nameEnd && buildEnd) {
                cacheName = [[NSString alloc] initWithBytes:bytes + 32 length:nameEnd - bytes - 32 encoding:NSASCIIStringEncoding];
                build = [[NSString alloc] initWithBytes:bytes + 64 length:buildEnd - bytes - 64 encoding:NSASCIIStringEncoding];
            }
            valid = version == 5 && length >= 2048 && length <= 0x11600000 &&
                [cacheName.lowercaseString isEqualToString:name.stringByDeletingPathExtension] &&
                ([@"01.01.14.2342" isEqualToString:build] || [@"01.10.12.2276" isEqualToString:build]);
        }
        if (!valid) {
            if (error) *error = failure([NSString stringWithFormat:@"%@ must be a supported original Xbox Halo map. A PC, Custom Edition, Anniversary or MCC map cannot replace this base-game file.", file.lastPathComponent]);
            return nil;
        }
        if (discBuild && ![discBuild isEqualToString:build]) {
            if (error) *error = failure(@"The maps mix different Xbox releases. Choose one complete set from one disc.");
            return nil;
        }
        discBuild = build;
        hasUI |= [name isEqualToString:@"ui.map"];
        hasOpeningLevel |= [name isEqualToString:@"a10.map"];
    }
    if (!hasUI || !hasOpeningLevel) {
        if (error) *error = failure(@"This folder needs ui.map and a10.map from the same Xbox Halo disc. Use a complete maps folder.");
        return nil;
    }
    return root;
}

NSURL *HaloImportDiscImage(NSURL *image, NSURL *supportDirectory,
                          xiso_progress_proc progress, void *context, NSError **error) {
    /* A new directory for every import. Failed imports never replace working data. */
    NSURL *destination = [[supportDirectory URLByAppendingPathComponent:@"Game Data" isDirectory:YES]
                          URLByAppendingPathComponent:NSUUID.UUID.UUIDString isDirectory:YES];
    if (![NSFileManager.defaultManager createDirectoryAtURL:destination withIntermediateDirectories:YES
                                                attributes:nil error:error]) return nil;
    char detail[512] = {0};
    BOOL extracted = xiso_extract_maps(image.fileSystemRepresentation, destination.fileSystemRepresentation,
                                      progress, context, detail, sizeof(detail));
    NSURL *validated = extracted ? HaloValidateGameData(destination, error) : nil;
    if (!validated) {
        [NSFileManager.defaultManager removeItemAtURL:destination error:nil];
        if (!extracted && error) *error = failure([NSString stringWithUTF8String:detail] ?: @"The disc image could not be read.");
    }
    return validated;
}
