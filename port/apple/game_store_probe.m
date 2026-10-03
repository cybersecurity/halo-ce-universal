#import "game_store.h"
static int cancel(void *context,const char *file,uint64_t done,uint64_t total){(void)context;(void)file;(void)done;(void)total;return 0;}
int main(int argc,char **argv){@autoreleasepool{
 if(argc<3)return 2;NSString *root=[NSString stringWithUTF8String:argv[2]];
 if(!strcmp(argv[1],"ready"))return halo_game_store_current(root)?0:1;
 if(argc!=4)return 2;
 [NSFileManager.defaultManager createDirectoryAtPath:root withIntermediateDirectories:YES attributes:nil error:nil];
 NSString *failure=nil;BOOL ok=halo_game_store_import([NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[3]]],root,!strcmp(argv[1],"cancel")?cancel:NULL,NULL,&failure);
 if(failure)fprintf(stderr,"%s\n",failure.UTF8String);return ok?0:1;
}}
