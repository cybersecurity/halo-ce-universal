#import <Foundation/Foundation.h>
#include "xiso.h"
/* The root contains an atomically published generation holding the private
   image, integrity manifest and extracted map cache. Saves live outside it. */
NSString *halo_game_store_current(NSString *root);
BOOL halo_game_store_import(NSURL *image, NSString *root, xiso_progress_proc progress,
                           void *context, NSString **failure);
