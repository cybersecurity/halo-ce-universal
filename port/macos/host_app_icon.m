#import <AppKit/AppKit.h>

#include "host_app_icon.h"
#include <stdio.h>
#include <stdlib.h>

void mac_host_activate_app(void)
{
	if (![NSThread isMainThread])
		return;
	@autoreleasepool {
		[[NSApplication sharedApplication] activate];
	}
}

void mac_host_apply_app_icon(void)
{
	if (![NSThread isMainThread])
		return;
	@autoreleasepool {
		const char *configured_path = getenv("HALO_APP_ICON");
		NSString *path = configured_path && configured_path[0]
			? [NSString stringWithUTF8String:configured_path]
			: [[NSBundle mainBundle] pathForResource:@"HaloCombatEvolved" ofType:@"icns"];
		if (!path)
			return;
		NSImage *icon = [[NSImage alloc] initWithContentsOfFile:path];
		if (icon) {
			[[NSApplication sharedApplication] setApplicationIconImage:icon];
			fprintf(stderr, "[macos] native application icon loaded: %s\n", [path fileSystemRepresentation]);
			[icon release];
		} else {
			fprintf(stderr, "[macos] cannot load application icon: %s\n", [path fileSystemRepresentation]);
		}
	}
}
