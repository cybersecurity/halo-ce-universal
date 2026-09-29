#import <AppKit/AppKit.h>
#import <CommonCrypto/CommonDigest.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static BOOL checking;
static BOOL diagnosticMode;
static int instanceLock = -1;

static int fail(NSString *message)
{
	if (instanceLock >= 0) {
		flock(instanceLock, LOCK_UN);
		close(instanceLock);
		instanceLock = -1;
	}
	fprintf(stderr, "%s\n", [message UTF8String]);
	if (!checking) {
		[NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
		[NSApp activate];
		NSAlert *alert = [[[NSAlert alloc] init] autorelease];
		[alert setMessageText:@"Halo Combat Evolved could not start"];
		[alert setInformativeText:message];
		[alert runModal];
	}
	return 2;
}

static NSString *sha256(NSString *path)
{
	FILE *file = fopen([path fileSystemRepresentation], "rb");
	if (!file) return nil;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	CC_SHA256_CTX hash;
	CC_SHA256_Init(&hash);
	unsigned char buffer[65536], digest[CC_SHA256_DIGEST_LENGTH];
	size_t count;
	while ((count = fread(buffer, 1, sizeof(buffer), file)) != 0)
		CC_SHA256_Update(&hash, buffer, (CC_LONG)count);
	BOOL okay = !ferror(file);
	fclose(file);
	if (!okay) return nil;
	CC_SHA256_Final(digest, &hash);
#pragma clang diagnostic pop
	NSMutableString *result = [NSMutableString string];
	for (NSUInteger i = 0; i < sizeof(digest); ++i)
		[result appendFormat:@"%02x", digest[i]];
	return result;
}

static BOOL mkdirs(NSString *path)
{
	return [[NSFileManager defaultManager] createDirectoryAtPath:path
		withIntermediateDirectories:YES attributes:nil error:nil];
}

static NSString *absolutePath(NSString *path)
{
	path = [path stringByExpandingTildeInPath];
	if (![path isAbsolutePath])
		path = [[[NSFileManager defaultManager] currentDirectoryPath] stringByAppendingPathComponent:path];
	return [path stringByStandardizingPath];
}

int main(int argc, char **argv)
{
	@autoreleasepool {
		for (int i = 1; i < argc; ++i)
			if (!strcmp(argv[i], "--check")) checking = YES;
		NSString *state = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/Halo Native AOT"];
		NSString *assetOverride = nil;
		for (int i = 1; i < argc; ++i) {
			if (!strcmp(argv[i], "--check")) checking = YES;
			else if (!strcmp(argv[i], "--diagnostic")) diagnosticMode = YES;
			else if ((!strcmp(argv[i], "--state-root") || !strcmp(argv[i], "--assets")) && i + 1 < argc) {
				BOOL isState = !strcmp(argv[i], "--state-root");
				NSString *value = absolutePath([NSString stringWithUTF8String:argv[++i]]);
				if (isState) state = value; else assetOverride = value;
			} else if (!strncmp(argv[i], "-psn_", 5)) continue;
			else return fail(@"Unknown startup option. Supported options: --check, --assets PATH, --state-root PATH.");
		}
		NSBundle *bundle = [NSBundle mainBundle];
		NSString *resources = [bundle resourcePath];
		NSString *engine = [[bundle executablePath] stringByDeletingLastPathComponent];
		engine = [engine stringByAppendingPathComponent:@"halo_engine"];
		NSString *elf = [resources stringByAppendingPathComponent:@"halo_guest.elf"];
		NSString *manifestPath = [resources stringByAppendingPathComponent:@"launch-config.json"];
		NSData *manifestData = [NSData dataWithContentsOfFile:manifestPath];
		NSDictionary *manifest = manifestData ? [NSJSONSerialization JSONObjectWithData:manifestData options:0 error:nil] : nil;
		if (![manifest isKindOfClass:[NSDictionary class]]) return fail(@"The application is missing its launch manifest. Rebuild or reinstall it.");
		for (NSString *key in @[@"binary_sha256", @"elf_sha256"]) {
			NSString *path = [key isEqualToString:@"binary_sha256"] ? engine : elf;
			NSString *actual = sha256(path);
			if (!actual || ![actual isEqual:[manifest objectForKey:key]])
				return fail([NSString stringWithFormat:@"The bundled %@ differs from its verified package. Rebuild or reinstall the application.", [path lastPathComponent]]);
		}
		if (assetOverride && !diagnosticMode) return fail(@"External --assets is available only with explicit --diagnostic. Normal startup uses bundled game data.");
		NSString *bundledAssets = [resources stringByAppendingPathComponent:@"GameData"];
		NSString *assets = assetOverride ?: bundledAssets;
		BOOL bundledData = assetOverride == nil;
		if (bundledData) {
			NSData *sealData = [NSData dataWithContentsOfFile:[assets stringByAppendingPathComponent:@"asset-manifest.json"]];
			NSDictionary *seal = sealData ? [NSJSONSerialization JSONObjectWithData:sealData options:0 error:nil] : nil;
			NSArray *entries = [seal isKindOfClass:[NSDictionary class]] ? [seal objectForKey:@"assets"] : nil;
			if (![seal isKindOfClass:[NSDictionary class]] || ![entries isKindOfClass:[NSArray class]] || ![entries count])
				return fail(@"Bundled game data is missing its asset manifest. Rebuild the app from your own disc image.");
			if (![sha256([assets stringByAppendingPathComponent:@"asset-manifest.json"]) isEqual:[manifest objectForKey:@"asset_manifest_sha256"]])
				return fail(@"The bundled asset manifest differs from its package provenance.");
			for (NSDictionary *entry in entries) {
				if (![entry isKindOfClass:[NSDictionary class]]) return fail(@"Invalid bundled asset manifest entry.");
				NSString *relative = [entry objectForKey:@"path"];
				if (![relative isKindOfClass:[NSString class]] || [relative isAbsolutePath] || [[relative pathComponents] containsObject:@".."])
					return fail(@"Invalid bundled asset manifest path.");
				NSString *path = [assets stringByAppendingPathComponent:relative];
				NSDictionary *attributes = [[NSFileManager defaultManager] attributesOfItemAtPath:path error:nil];
				if (![[attributes objectForKey:NSFileType] isEqual:NSFileTypeRegular] || ![[attributes objectForKey:NSFileSize] isEqual:[entry objectForKey:@"bytes"]])
					return fail([NSString stringWithFormat:@"Bundled game asset %@ is missing or differs in size. Rebuild the app.", relative]);
				if (checking && ![[entry objectForKey:@"sha256"] isEqual:sha256(path)])
					return fail([NSString stringWithFormat:@"Bundled game asset %@ differs from its recorded hash. Rebuild the app.", relative]);
			}
		}
		NSString *maps = [assets stringByAppendingPathComponent:@"maps"];
		NSString *movies = [assets stringByAppendingPathComponent:@"bink"];
		NSFileManager *files = [NSFileManager defaultManager];
		if (![files fileExistsAtPath:movies]) movies = [assets stringByAppendingPathComponent:@"bink-source"];
		if (![files fileExistsAtPath:maps] || ![files fileExistsAtPath:movies])
			return fail([NSString stringWithFormat:@"Game maps or movies are missing at %@. Rebuild the app from a supported Xbox image.", assets]);
		NSString *data = [state stringByAppendingPathComponent:@"data"];
		NSString *save = [state stringByAppendingPathComponent:@"save"];
		NSString *icon = [resources stringByAppendingPathComponent:@"HaloCombatEvolved.icns"];
		if (checking) {
			NSDictionary *report = @{ @"binary":engine, @"elf":elf, @"assets":assets, @"data_root":data,
				@"save_root":save, @"config":[save stringByAppendingPathComponent:@"config.toml"],
				@"icon":icon, @"assets_in_bundle":@(bundledData), @"asset_hashes_verified":@(bundledData && checking), @"verified":@YES, @"launches_game":@NO, @"diagnostic_mode":@(diagnosticMode) };
			NSData *json = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil];
			fwrite([json bytes], 1, [json length], stdout); fputc('\n', stdout);
			return 0;
		}
		if (!mkdirs(state)) return fail(@"The user Application Support directory cannot be created.");
		NSString *lockPath = [state stringByAppendingPathComponent:@"instance.lock"];
		int lock = open([lockPath fileSystemRepresentation], O_RDWR | O_CREAT, 0600);
		if (lock < 0) return fail(@"The application instance lock cannot be opened.");
		if (lock < 3) {
			int retained = fcntl(lock, F_DUPFD, 3);
			close(lock);
			if (retained < 0) return fail(@"The application instance lock cannot be retained.");
			lock = retained;
		}
		if (flock(lock, LOCK_EX | LOCK_NB) != 0) {
			int lockError = errno;
			if (lockError != EWOULDBLOCK && lockError != EAGAIN) {
				close(lock);
				return fail(@"The application instance lock cannot be acquired.");
			}
			char pidText[32] = {0};
			pread(lock, pidText, sizeof(pidText) - 1, 0);
			pid_t pid = (pid_t)strtol(pidText, NULL, 10);
			NSRunningApplication *existing = pid > 0 ? [NSRunningApplication runningApplicationWithProcessIdentifier:pid] : nil;
			if (existing) [existing activateWithOptions:0];
			close(lock);
			if (!existing) return fail(@"Halo is already running. Close the current game before starting another instance.");
			return 0;
		}
		instanceLock = lock;
		/* This descriptor intentionally survives exec: the actual game owns the lock. */
		char pidText[32];
		int pidLength = snprintf(pidText, sizeof(pidText), "%ld\n", (long)getpid());
		if (ftruncate(lock, 0) || pwrite(lock, pidText, (size_t)pidLength, 0) != pidLength)
			return fail(@"The application instance lock cannot be updated.");
		if (!mkdirs(data) || !mkdirs(save) || !mkdirs([state stringByAppendingPathComponent:@"build/macos-aot/run-data"]))
			return fail(@"The game data and save directories cannot be created.");
		NSString *configPath = [save stringByAppendingPathComponent:@"config.toml"];
		int config = open([configPath fileSystemRepresentation], O_WRONLY | O_CREAT | O_EXCL, 0600);
		if (config >= 0) {
			NSData *starter = [NSData dataWithContentsOfFile:[resources stringByAppendingPathComponent:@"starter-config.toml"]];
			if (!starter || write(config, [starter bytes], [starter length]) != (ssize_t)[starter length]) {
				close(config);
				unlink([configPath fileSystemRepresentation]);
				return fail(@"The initial game settings cannot be written.");
			}
			close(config);
		} else if (errno != EEXIST) return fail(@"The game settings file cannot be opened.");
		if ([files fileExistsAtPath:[data stringByAppendingPathComponent:@"init.txt"]])
			return fail(@"The normal game data folder contains a test init.txt. Remove that test override before normal startup.");
		NSMutableArray *assetDirectories = [NSMutableArray arrayWithArray:@[@"maps", @"bink"]];
		for (NSString *name in [files contentsOfDirectoryAtPath:assets error:nil])
			if ([name hasPrefix:@"maps_"]) [assetDirectories addObject:name];
		for (NSString *oldName in [files contentsOfDirectoryAtPath:data error:nil]) {
			if (![oldName hasPrefix:@"maps_"] || [assetDirectories containsObject:oldName]) continue;
			NSString *oldPath = [data stringByAppendingPathComponent:oldName];
			if ([files destinationOfSymbolicLinkAtPath:oldPath error:nil]) {
				if (![files removeItemAtPath:oldPath error:nil]) return fail(@"An obsolete language-data link cannot be removed.");
			} else return fail(@"A language-data directory in writable state conflicts with the bundled assets. Preserve/move it before starting the app.");
		}
		for (NSString *name in assetDirectories) {
			NSString *destination = [data stringByAppendingPathComponent:name];
			NSString *source = [name isEqualToString:@"bink"] ? movies : [assets stringByAppendingPathComponent:name];
			NSString *oldLink = [files destinationOfSymbolicLinkAtPath:destination error:nil];
			if (!oldLink && [files fileExistsAtPath:destination]) return fail(@"A writable-state asset directory conflicts with the bundle overlay. Preserve/move that directory before starting the app.");
			if (oldLink && ![oldLink isEqual:source]) {
				if (![files removeItemAtPath:destination error:nil]) return fail(@"A game data link cannot be updated.");
			}
			if (![files fileExistsAtPath:destination] && ![files createSymbolicLinkAtPath:destination withDestinationPath:source error:nil])
				return fail(@"A game data link cannot be created.");
		}
		NSDictionary *environment = [[NSProcessInfo processInfo] environment];
		NSSet *diagnostic = [NSSet setWithArray:@[@"HALO_EXIT_AFTER", @"HALO_TRACE_RT_ALLOC", @"HALO_FRAME_METRICS",
			@"HALO_UI_POINTER_TRACE", @"HALO_INPUT_TRACE", @"HALO_FINGERPRINT_METRICS", @"HALO_AUDIO_EVIDENCE", @"HALO_BINK_FRAME_TRACE", @"HALO_SDL_SWAP_TRACE", @"HALO_GL_FENCE_TRACE"]];
		for (NSString *key in environment) {
			BOOL permittedDiagnostic = diagnosticMode && ([key hasPrefix:@"HALO_SDL_TEST_"] || [key hasPrefix:@"HALO_CAPTURE_"] || [key isEqualToString:@"HALO_FRAME_METRICS"] || [key isEqualToString:@"HALO_INPUT_TRACE"] || [key isEqualToString:@"HALO_UI_POINTER_TRACE"]);
			if (!permittedDiagnostic && ([key hasPrefix:@"HALO_TEST_"] || [key hasPrefix:@"HALO_SDL_TEST_"] || [key hasPrefix:@"HALO_CAPTURE_"] || [diagnostic containsObject:key]))
				unsetenv([key UTF8String]);
		}
		NSDictionary *settings = @{ @"HALO_GUEST_ELF":elf, @"HALO_DATA_ROOT":data, @"HALO_SAVE_ROOT":save,
			@"HALO_CONFIG_ROOT":save, @"HALO_PROJECT_ROOT":state, @"HALO_AUDIO_ENABLE":@"1", @"HALO_APP_ICON":icon };
		for (NSString *key in settings)
			if (setenv([key UTF8String], [[settings objectForKey:key] fileSystemRepresentation], 1) != 0)
				return fail(@"The game environment cannot be configured.");
		NSString *logPath = [state stringByAppendingPathComponent:@"halo.log"];
		int log = open([logPath fileSystemRepresentation], O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (log < 0 || dup2(log, STDOUT_FILENO) < 0 || dup2(log, STDERR_FILENO) < 0)
			return fail(@"The game log cannot be opened.");
		if (log > STDERR_FILENO) close(log);
		if (chdir([state fileSystemRepresentation]) != 0) return fail(@"The writable game directory cannot be opened.");
		fprintf(stderr, "[launcher] verified bundled engine; data=%s save=%s\n", [data fileSystemRepresentation], [save fileSystemRepresentation]);
		char *arguments[] = {(char *)[engine fileSystemRepresentation], (char *)[elf fileSystemRepresentation], NULL};
		execv(arguments[0], arguments);
		return fail([NSString stringWithFormat:@"The bundled game could not execute: %s", strerror(errno)]);
	}
}
